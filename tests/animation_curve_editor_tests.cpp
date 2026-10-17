// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Background regression for the shared modeless curve editor
// (src/window/animation_curve_editor.cpp).  Everything runs without a message
// loop: the editor window is created hidden, no window is ever shown, no
// desktop pixel is read, no GPU device is created and no message box is
// expected.  Real callbacks are verified through the real WM_COMMAND path.
//
// The control ids and the WM_APP test hooks are mirrored from the comment block
// at the top of src/window/animation_curve_editor.cpp; keep the two lists in
// step when either changes.  The hooks are read-only geometry queries:
//   kHookPointForKey   WM_APP+300  wParam = key index -> MAKELPARAM(client x, y)
//   kHookPointForFrame WM_APP+301  wParam = frame     -> MAKELPARAM(client x, y)
//   kHookMmdControls   WM_APP+302  lParam = double[4] -> TRUE when the MMD
//                                  panel can show the selected interval
//   kHookKeyCount      WM_APP+303  wParam = channel index or -1 -> key count
//
// Verification: hidden create/destroy, add/delete/undo/redo through the real
// apply callback, duplicate-frame rejection, two independent sources, a failing
// apply that must roll back, a source without callbacks that must stay disabled,
// MMD diagonal handles for a Linear interval, disabled MMD panel for an
// unrepresentable interval, the launcher icon geometry for the 22 and 18 pixel
// buttons, and the three-language layout (no overlap, >= 4 logical pixel label
// gaps, >= 4 logical pixel edit margins).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>

#include "mikudancestudio/animation_curve_editor.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

namespace ce = mikudancestudio::curve_editor;
namespace ac = mikudancestudio::animation_curve;

namespace {

// Control ids, mirrored from src/window/animation_curve_editor.cpp.
enum : int {
    kIdSourceLabel = 1001, kIdSourceCombo = 1002,
    kIdReadMotion = 1003, kIdWriteMotion = 1004, kIdRefresh = 1005,
    kIdViewLabel = 1006, kIdViewCombo = 1007, kIdToolLabel = 1008, kIdToolCombo = 1009,
    kIdFitAll = 1010, kIdFitRange = 1011, kIdShortcutToggle = 1012, kIdShortcutText = 1013,
    kIdChannelLabel = 1014, kIdChannelList = 1015, kIdChannelAll = 1016,
    kIdCanvas = 1020,
    kIdMmdLabel = 1030, kIdMmdPanel = 1031, kIdMmdReadout = 1032,
    kIdFrameLabel = 1040, kIdFrameEdit = 1041,
    kIdValueLabel = 1042, kIdValueEdit = 1043,
    kIdInterpolationLabel = 1044, kIdInterpolationCombo = 1045,
    kIdTangentLabel = 1046, kIdTangentCombo = 1047,
    kIdInSlopeLabel = 1048, kIdInSlopeEdit = 1049,
    kIdOutSlopeLabel = 1050, kIdOutSlopeEdit = 1051,
    kIdInWeightLabel = 1052, kIdInWeightEdit = 1053,
    kIdOutWeightLabel = 1054, kIdOutWeightEdit = 1055,
    kIdAddKey = 1056, kIdDeleteKey = 1057, kIdApplyKey = 1058,
    kIdUndo = 1059, kIdRedo = 1060,
    kIdBakeFirstLabel = 1070, kIdBakeFirstEdit = 1071,
    kIdBakeLastLabel = 1072, kIdBakeLastEdit = 1073,
    kIdBakeStepLabel = 1074, kIdBakeStepEdit = 1075,
    kIdBakeRun = 1076,
    kIdStatus = 1080,
    kIdCompareLabel = 1081, kIdCompareSameGroup = 1082,
    kIdCompareFilter = 1083,
    kIdHoverHint = 1085, kIdCompareAllGroups = 1086, kIdCompareFree = 1087,
};

constexpr UINT kHookPointForKey = WM_APP + 300;
constexpr UINT kHookPointForFrame = WM_APP + 301;
constexpr UINT kHookMmdControls = WM_APP + 302;
constexpr UINT kHookKeyCount = WM_APP + 303;
constexpr UINT kHookView = WM_APP + 304;
constexpr UINT kHookBarPoint = WM_APP + 305;
constexpr UINT kHookFocusId = WM_APP + 306;
constexpr UINT kHookPlayheadState = WM_APP + 307;
constexpr UINT kHookCompare = WM_APP + 308;
constexpr UINT kHookCompareRange = WM_APP + 309;
constexpr UINT kHookChannelVisible = WM_APP + 310;
constexpr UINT kHookHoverHint = WM_APP + 311;
constexpr UINT kPlayheadTimerId = 1;
constexpr double kMinimumTimeSpan = 1.0;
constexpr double kMinimumValueSpan = 1.0e-5;

int g_checks = 0;

void Check(bool condition, const char *description) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::fflush(stderr);
        std::exit(1);
    }
}

void Note(const char *description) { std::printf("PASS %s\n", description); }

// ---------------------------------------------------------------------------
// Host-side stub source.
// ---------------------------------------------------------------------------
struct HostSource {
    std::vector<ce::Channel> data;
    int readCalls = 0;
    int applyCalls = 0;
    bool failApply = false;
    bool failRead = false;
    int readMotionCalls = 0;
    int writeMotionCalls = 0;
    bool hasReadMotion = false;
    bool hasWriteMotion = false;
    std::vector<std::vector<ce::Channel>> applied;
    // Comparison display metadata.  storedMode is what the host remembers and
    // hands back on every open, modeWrites counts the accepted changes.
    bool compareEnabled = false;
    bool hasModeCallbacks = false;
    std::string initialMain;
    int storedMode = ce::kCompareSameGroup;
    int modeWrites = 0;
    int modeReads = 0;
    bool failModeWrite = false;

    ce::Source Make(const std::string &id, const std::wstring &label) {
        ce::Source source;
        source.id = id;
        source.label = label;
        HostSource *self = this;
        source.read = [self](std::vector<ce::Channel> &out, std::wstring &error) {
            ++self->readCalls;
            if (self->failRead) {
                error = L"the host refused to read";
                return false;
            }
            out = self->data;
            return true;
        };
        source.apply = [self](const std::vector<ce::Channel> &channels, std::wstring &error) {
            ++self->applyCalls;
            if (self->failApply) {
                error = L"the host refused this write";
                return false;
            }
            self->applied.push_back(channels);
            self->data = channels;
            return true;
        };
        if (hasReadMotion) {
            source.readMotion = [self](void *, std::wstring &error) -> bool {
                ++self->readMotionCalls;
                if (self->failRead) {
                    error = L"motion read failed";
                    return false;
                }
                return true;
            };
        }
        if (hasWriteMotion) {
            source.writeMotion = [self](void *, const std::vector<ce::Channel> &, std::wstring &error) -> bool {
                ++self->writeMotionCalls;
                if (self->failApply) {
                    error = L"motion write failed";
                    return false;
                }
                return true;
            };
        }
        source.comparisonEnabled = compareEnabled;
        source.initialMainChannel = initialMain;
        if (hasModeCallbacks) {
            source.readComparisonMode = [self]() -> int {
                ++self->modeReads;
                return self->storedMode;
            };
            source.writeComparisonMode = [self](int mode, std::wstring &error) -> bool {
                if (self->failModeWrite) {
                    error = L"the settings store refused the mode";
                    return false;
                }
                ++self->modeWrites;
                self->storedMode = mode;
                return true;
            };
        }
        return source;
    }
};

ac::Key MakeKey(std::int32_t frame, double value, ac::Interpolation interpolation = ac::Interpolation::Bezier,
                ac::Tangent tangent = ac::Tangent::Automatic) {
    ac::Key key;
    key.frame = frame;
    key.value = value;
    key.interpolation = interpolation;
    key.tangent = tangent;
    return key;
}

ce::Channel MakeChannel(const std::string &id, const std::wstring &label, std::vector<ac::Key> keys) {
    ce::Channel channel;
    channel.id = id;
    channel.label = label;
    channel.track.keys = std::move(keys);
    return channel;
}

// ---------------------------------------------------------------------------
// Window helpers.
// ---------------------------------------------------------------------------
void Click(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the command button exists");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(id), BN_CLICKED), reinterpret_cast<LPARAM>(control));
}

void SetText(HWND window, int id, const wchar_t *text) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the edit control exists");
    SetWindowTextW(control, text);
}

std::wstring GetText(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    if (!control) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &text[0], length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

int KeyCount(HWND window, int channel = -1) {
    return static_cast<int>(SendMessageW(window, kHookKeyCount, static_cast<WPARAM>(channel), 0));
}

// Reads the shared time range and the value range of the current view.
void ReadView(HWND window, double view[4]) {
    view[0] = view[1] = view[2] = view[3] = 0;
    SendMessageW(window, kHookView, 0, reinterpret_cast<LPARAM>(view));
}

LRESULT FocusId(HWND window) { return SendMessageW(window, kHookFocusId, 0, 0); }

POINT AskBarPoint(HWND window, int bar, int segment) {
    const LPARAM packed = static_cast<LPARAM>(SendMessageW(window, kHookBarPoint, static_cast<WPARAM>(bar),
                                                           static_cast<LPARAM>(segment)));
    POINT point = {static_cast<LONG>(static_cast<short>(LOWORD(packed))),
                   static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
    return point;
}

void ClickBar(HWND window, int bar, int segment) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    const POINT point = AskBarPoint(window, bar, segment);
    Check(point.x > 0 && point.y > 0, "the queried range-bar segment lies inside the canvas");
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
}

void PressKey(HWND window, int key) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    SendMessageW(canvas, WM_KEYDOWN, static_cast<WPARAM>(key), 0);
    SendMessageW(canvas, WM_KEYUP, static_cast<WPARAM>(key), 0);
}

POINT AskPoint(HWND window, UINT hook, WPARAM value) {
    const LPARAM packed = static_cast<LPARAM>(SendMessageW(window, hook, value, 0));
    POINT point = {static_cast<LONG>(static_cast<short>(LOWORD(packed))),
                   static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
    return point;
}

void ClickKey(HWND window, int keyIndex) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(canvas != nullptr, "the canvas exists");
    const POINT point = AskPoint(window, kHookPointForKey, static_cast<WPARAM>(keyIndex));
    Check(point.x > 0 && point.y > 0, "the queried key lies inside the canvas");
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(canvas, WM_LBUTTONUP, 0, position);
}

// Drags the key at `keyIndex` to `targetFrame`, keeping the value.
void DragKeyToFrame(HWND window, int keyIndex, int targetFrame) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(canvas != nullptr, "the canvas exists");
    const POINT from = AskPoint(window, kHookPointForKey, static_cast<WPARAM>(keyIndex));
    const POINT framePoint = AskPoint(window, kHookPointForFrame, static_cast<WPARAM>(targetFrame));
    Check(from.x > 0 && from.y > 0 && framePoint.x > 0, "the drag endpoints lie inside the canvas");
    POINT to = {framePoint.x, from.y};
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
}

const ac::Key *FindKey(const std::vector<ce::Channel> &channels, std::size_t channel, std::int32_t frame) {
    if (channel >= channels.size()) return nullptr;
    for (const ac::Key &key : channels[channel].track.keys) {
        if (key.frame == frame) return &key;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Channel tree and hint helpers.  The tree is driven through real Win32 mouse
// and key messages; the state comes back through the WM_APP hooks.
// ---------------------------------------------------------------------------
int ChannelItemIndex(HWND tree, HTREEITEM item) {
    TVITEMW value = {};
    value.mask = TVIF_PARAM;
    value.hItem = item;
    return item && SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&value)) && value.lParam > 0
               ? static_cast<int>(value.lParam - 1) : -1;
}

HTREEITEM FindChannelItem(HWND tree, HTREEITEM item, int index) {
    for (; item; item = TreeView_GetNextSibling(tree, item)) {
        if (ChannelItemIndex(tree, item) == index) return item;
        if (HTREEITEM child = FindChannelItem(tree, TreeView_GetChild(tree, item), index)) return child;
    }
    return nullptr;
}

HTREEITEM ChannelItem(HWND window, int index) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    Check(tree != nullptr, "the channel tree exists");
    HTREEITEM item = FindChannelItem(tree, TreeView_GetRoot(tree), index);
    Check(item != nullptr, "the channel has a tree leaf");
    return item;
}

int ChannelCaretIndex(HWND window) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    return ChannelItemIndex(tree, TreeView_GetSelection(tree));
}

int RenderInk(HWND child);

POINT ChannelRowPoint(HWND window, int index, bool checkbox) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    HTREEITEM item = ChannelItem(window, index);
    TreeView_EnsureVisible(tree, item);
    if (checkbox && !TreeView_GetImageList(tree, TVSIL_STATE)) {
        // Native checkbox images are created on the first paint. Hidden test
        // windows have not had one yet; initialize them into a memory DC.
        RenderInk(tree);
        Check(TreeView_GetImageList(tree, TVSIL_STATE) != nullptr,
              "a memory paint initializes the hidden tree's checkbox images");
    }
    RECT row = {};
    Check(TreeView_GetItemRect(tree, item, &row, TRUE) != FALSE, "the channel leaf has a text rectangle");
    const int y = (row.top + row.bottom) / 2;
    // A tree's checkbox and label have separate actions. Verify the hit zone
    // instead of guessing an indent or sending the former list-box messages.
    const UINT wanted = checkbox ? TVHT_ONITEMSTATEICON : TVHT_ONITEMLABEL;
    for (int x = 0; x < row.right; ++x) {
        TVHITTESTINFO hit = {};
        hit.pt = POINT{x, y};
        if (TreeView_HitTest(tree, &hit) == item && (hit.flags & wanted)) return hit.pt;
    }
    RECT client = {}; GetClientRect(tree, &client);
    std::fprintf(stderr, "CHANNEL miss index=%d checkbox=%d row=%ld,%ld,%ld,%ld client=%ld,%ld image=%p state=%u\n",
                 index, int(checkbox), row.left, row.top, row.right, row.bottom, client.right, client.bottom,
                 TreeView_GetImageList(tree, TVSIL_STATE), TreeView_GetItemState(tree, item, TVIS_STATEIMAGEMASK));
    Check(false, "the channel exposes the requested checkbox or label hit zone");
    return POINT{};
}

void ClickChannelOnce(HWND window, int index) {
    HWND list = GetDlgItem(window, kIdChannelList);
    const POINT point = ChannelRowPoint(window, index, true);
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(list, WM_LBUTTONUP, 0, position);
}

void ClickChannelLabel(HWND window, int index) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    const POINT point = ChannelRowPoint(window, index, false);
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(tree, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(tree, WM_LBUTTONUP, 0, position);
}

// The exact message sequence Windows delivers for a double click on the label:
// down, up, double-click, up. The checkbox is a separate visibility target.
void DoubleClickChannel(HWND window, int index) {
    HWND list = GetDlgItem(window, kIdChannelList);
    const POINT point = ChannelRowPoint(window, index, false);
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(list, WM_LBUTTONUP, 0, position);
    SendMessageW(list, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
    SendMessageW(list, WM_LBUTTONUP, 0, position);
}

int ChannelVisible(HWND window, int index) {
    return static_cast<int>(SendMessageW(window, kHookChannelVisible, static_cast<WPARAM>(index), 0));
}

int ActiveChannelIndex(HWND window) {
    int info[12] = {};
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(info));
    return info[1];
}

int HoverHint(HWND window) { return static_cast<int>(SendMessageW(window, kHookHoverHint, 0, 0)); }

void HoverControl(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the hovered control exists");
    SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
}

void LeaveControl(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the left control exists");
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
}

std::wstring ChannelRowText(HWND window, int index) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    wchar_t text[256] = {};
    TVITEMW value = {};
    value.mask = TVIF_TEXT;
    value.hItem = ChannelItem(window, index);
    value.pszText = text;
    value.cchTextMax = static_cast<int>(sizeof(text) / sizeof(text[0]));
    Check(SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&value)) != FALSE,
          "the channel leaf text is readable");
    return text;
}
ce::Session MakeSession(HostSource &host, unsigned language = 2) {
    ce::Session session;
    session.id = "test";
    session.sources.push_back(host.Make("source", L"Test source"));
    session.language = language;
    session.currentFrame = [] { return 30.0; };
    session.seek = [](int) {};
    return session;
}

// ---------------------------------------------------------------------------
// 1. Hidden lifecycle.
// ---------------------------------------------------------------------------
void HiddenLifecycle() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    void *window = ce::Open(nullptr, session, false);
    Check(window != nullptr, "Open returns a window for a valid session");
    HWND handle = static_cast<HWND>(window);
    Check(IsWindow(handle) != FALSE, "the editor window exists");
    Check(IsWindowVisible(handle) == FALSE, "a hidden Open shows nothing");
    Check(IsWindowVisible(GetDlgItem(handle, kIdCanvas)) == FALSE, "the canvas of a hidden editor is invisible");
    Check(host.readCalls == 1, "Open reads the source once");
    Check(KeyCount(handle) == 2, "the read data reaches the editor");

    void *again = ce::Open(nullptr, session, false);
    Check(again == window, "one editor window is reused per owner");
    ce::CloseOwned(nullptr);
    Check(IsWindow(handle) == FALSE, "CloseOwned destroys the editor window");
    Note("hidden create/destroy with a single editor per owner");
}

// ---------------------------------------------------------------------------
// 2. Add, delete, undo and redo through the real callbacks.
// ---------------------------------------------------------------------------
void AddDeleteUndoRedo() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the editor opens hidden");
    Check(IsWindowEnabled(GetDlgItem(window, kIdAddKey)) != FALSE, "add stays enabled for a writable source");

    SetText(window, kIdFrameEdit, L"20");
    SetText(window, kIdValueEdit, L"3.5");
    const int before = host.applyCalls;
    Click(window, kIdAddKey);
    Check(host.applyCalls == before + 1, "Add reaches the apply callback");
    Check(KeyCount(window) == 3, "the track holds the new key");
    const ac::Key *added = FindKey(host.data, 0, 20);
    Check(added != nullptr, "the callback received a key at frame 20");
    Check(added && std::fabs(added->value - 3.5) < 1e-9, "the callback received the edited value");

    // A second key on the same frame is refused without touching the source.
    const int afterAdd = host.applyCalls;
    SetText(window, kIdValueEdit, L"9");
    Click(window, kIdAddKey);
    Check(host.applyCalls == afterAdd, "a duplicate frame is refused before apply");
    Check(KeyCount(window) == 3, "a refused duplicate leaves the track unchanged");
    Check(GetText(window, kIdStatus).find(L"20") != std::wstring::npos, "the status names the duplicate frame");

    ClickKey(window, 2);
    Check(GetText(window, kIdFrameEdit) == L"20", "selecting a key loads its frame into the editor");
    Click(window, kIdDeleteKey);
    Check(host.applyCalls == afterAdd + 1, "Delete reaches the apply callback");
    Check(KeyCount(window) == 2, "Delete removes the selected key");
    Check(FindKey(host.data, 0, 20) == nullptr, "the callback received the deletion");

    Click(window, kIdUndo);
    Check(KeyCount(window) == 3, "Undo restores the key");
    Check(FindKey(host.data, 0, 20) != nullptr, "Undo commits the restored track through apply");
    Click(window, kIdRedo);
    Check(KeyCount(window) == 2, "Redo removes it again");
    Check(FindKey(host.data, 0, 20) == nullptr, "Redo commits through apply as well");

    ce::CloseOwned(nullptr);
    Note("add, delete, undo and redo through the real callbacks");
}

// ---------------------------------------------------------------------------
// 3. Dragging a key moves its frame; a collision is refused.
// ---------------------------------------------------------------------------
void DragKey() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    DragKeyToFrame(window, 1, 25);
    Check(FindKey(host.data, 0, 25) != nullptr, "the dragged key lands on the snapped frame");
    Check(FindKey(host.data, 0, 10) == nullptr, "the original frame is gone");
    Check(KeyCount(window) == 3, "dragging keeps the key count");

    // Dragging onto an occupied frame must be refused outright.
    const int applyCalls = host.applyCalls;
    DragKeyToFrame(window, 1, 20);
    Check(host.applyCalls == applyCalls, "a colliding drag never reaches apply");
    Check(FindKey(host.data, 0, 25) != nullptr, "a refused drag leaves the key where it was");
    Check(FindKey(host.data, 0, 20) != nullptr, "the unselected key stays untouched");

    ce::CloseOwned(nullptr);
    Note("whole-frame key dragging with conflict rejection");
}

// ---------------------------------------------------------------------------
// 4. Two sources stay independent.
// ---------------------------------------------------------------------------
void TwoSources() {
    HostSource accessory;
    accessory.data.push_back(MakeChannel("acc.x", L"Accessory X", {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    HostSource camera;
    camera.data.push_back(MakeChannel("cam.x", L"Camera X",
                                      {MakeKey(0, 0.0), MakeKey(5, 1.0), MakeKey(10, 2.0), MakeKey(15, 3.0)}));
    camera.data.push_back(MakeChannel("cam.y", L"Camera Y", {MakeKey(0, 4.0)}));

    ce::Session session;
    session.id = "two";
    session.language = 1;
    session.sources.push_back(accessory.Make("accessory", L"Accessory"));
    session.sources.push_back(camera.Make("camera", L"Camera"));
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the two-source editor opens");
    Check(KeyCount(window) == 2, "the first source is loaded first");
    Check(camera.readCalls == 0, "the second source is not read before it is selected");

    HWND combo = GetDlgItem(window, kIdSourceCombo);
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(kIdSourceCombo, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    Check(camera.readCalls == 1, "switching the combo reads the new source");
    Check(KeyCount(window) == 4, "the second source brings its own channels");

    SetText(window, kIdFrameEdit, L"7");
    SetText(window, kIdValueEdit, L"1.25");
    const int accessoryApplies = accessory.applyCalls;
    Click(window, kIdAddKey);
    Check(camera.applyCalls == 1, "the edit is written to the selected source");
    Check(accessory.applyCalls == accessoryApplies, "the other source is not touched");
    Check(FindKey(camera.data, 0, 7) != nullptr, "the camera source received the key");

    const std::wstring title = GetText(window, 0);
    (void)title;
    wchar_t caption[256] = {};
    GetWindowTextW(window, caption, 256);
    Check(std::wstring(caption).find(L"Camera") != std::wstring::npos, "the window title names the current source");

    ce::CloseOwned(nullptr);
    Note("two sources with independent reads, writes and titles");
}

// ---------------------------------------------------------------------------
// 5. A refused apply rolls back and an invalid source stays disabled.
// ---------------------------------------------------------------------------
void RefusedApply() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    host.failApply = true;
    SetText(window, kIdFrameEdit, L"40");
    SetText(window, kIdValueEdit, L"7");
    Click(window, kIdAddKey);
    Check(host.applyCalls == 1, "the refused write still called apply");
    Check(KeyCount(window) == 2, "a refused write leaves the editing state unchanged");
    Check(host.data.size() == 1 && host.data[0].track.keys.size() == 2, "the host data is unchanged too");
    Check(GetText(window, kIdStatus).find(L"refused") != std::wstring::npos, "the status shows the callback error");

    // The rolled back state is still the original track: undo has nothing to
    // remove and the next accepted write starts from the original keys.
    Click(window, kIdUndo);
    Check(KeyCount(window) == 2, "nothing was pushed onto the undo stack by the refused write");

    host.failApply = false;
    ClickKey(window, 1);
    Click(window, kIdDeleteKey);
    Check(host.applied.empty() == false, "an accepted write follows");
    Check(host.applied.back()[0].track.keys.size() == 1, "the accepted write starts from the unmodified track");
    Check(host.applied.back()[0].track.keys.front().frame == 0, "the surviving key is the original first key");

    // A source without callbacks must not be editable at all.
    HostSource mute;
    mute.data.push_back(MakeChannel("mute", L"Muted", {MakeKey(0, 0.0)}));
    ce::Channel channel = mute.data.front();
    (void)channel;
    ce::Session muteSession;
    muteSession.id = "mute";
    muteSession.sources.push_back(ce::Source());
    muteSession.sources[0].id = "mute";
    muteSession.sources[0].label = L"Muted";
    HWND muteWindow = static_cast<HWND>(ce::Open(nullptr, muteSession, false));
    Check(muteWindow != nullptr, "a source without callbacks still opens");
    Check(IsWindowEnabled(GetDlgItem(muteWindow, kIdAddKey)) == FALSE, "editing is disabled for a source without callbacks");
    SetText(muteWindow, kIdFrameEdit, L"5");
    Click(muteWindow, kIdAddKey);
    Check(GetText(muteWindow, kIdStatus).empty() == false, "the disabled source reports a reason");
    ce::CloseOwned(nullptr);
    Note("refused apply rolls back, invalid source stays disabled");
}

// ---------------------------------------------------------------------------
// 6. MMD handle panel: diagonal for Linear, disabled with a reason otherwise.
// ---------------------------------------------------------------------------
void MmdPanel() {
    HostSource host;
    ce::Channel linear = MakeChannel(
        "pos.x", L"Position X",
        {MakeKey(0, 0.0, ac::Interpolation::Bezier, ac::Tangent::Free),
         MakeKey(10, 5.0, ac::Interpolation::Linear, ac::Tangent::Linear)});
    // The core stores a straight interval as Linear while the slopes may still
    // be zero, so the panel has to derive the diagonal handles from the
    // weights instead of drawing an S curve.
    linear.track.keys[0].outSlope = 0.0;
    linear.track.keys[1].inSlope = 0.0;
    host.data.push_back(linear);
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    ClickKey(window, 1);
    double controls[4] = {-1, -1, -1, -1};
    const BOOL enabled = static_cast<BOOL>(SendMessageW(window, kHookMmdControls, 0, reinterpret_cast<LPARAM>(controls)));
    Check(enabled != FALSE, "a Linear interval is representable");
    Check(controls[0] == controls[1] && controls[2] == controls[3],
          "a Linear interval shows the diagonal handles, not the stale slopes");
    Check(controls[0] < controls[2], "the two handles stay separated in time");
    Check(controls[0] > 0 && controls[2] < 127, "the diagonal handles stay inside 0..127");
    Check(IsWindowEnabled(GetDlgItem(window, kIdMmdPanel)) != FALSE, "the MMD panel is enabled");

    // Equal-valued endpoints cannot be normalised by the MMD handle grid, so
    // the panel must disable itself and say why instead of clamping the curve.
    HostSource flat;
    flat.data.push_back(MakeChannel("pos.y", L"Position Y", {MakeKey(0, 2.0), MakeKey(10, 2.0)}));
    ce::Session flatSession = MakeSession(flat);
    ce::CloseOwned(nullptr);
    HWND flatWindow = static_cast<HWND>(ce::Open(nullptr, flatSession, false));
    ClickKey(flatWindow, 1);
    double flatControls[4] = {};
    const BOOL flatEnabled =
        static_cast<BOOL>(SendMessageW(flatWindow, kHookMmdControls, 0, reinterpret_cast<LPARAM>(flatControls)));
    Check(flatEnabled == FALSE, "an equal-valued interval is not representable");
    Check(IsWindowEnabled(GetDlgItem(flatWindow, kIdMmdPanel)) == FALSE, "the MMD panel is disabled instead of clamping");
    Check(GetText(flatWindow, kIdMmdReadout).empty() == false, "the disabled panel explains the reason");
    ce::CloseOwned(nullptr);
    Note("MMD panel: Linear diagonal handles, disabled with a reason when unrepresentable");
}

// ---------------------------------------------------------------------------
// 7. Launcher icon geometry for the 22 and 18 pixel buttons.
// ---------------------------------------------------------------------------
int CountInk(const unsigned *pixels, int width, int left, int top, int right, int bottom, int channel) {
    int count = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const unsigned value = pixels[static_cast<std::size_t>(y) * width + x];
            const int red = static_cast<int>(value & 0xFF);
            const int green = static_cast<int>((value >> 8) & 0xFF);
            const int blue = static_cast<int>((value >> 16) & 0xFF);
            if (red < channel && green < channel && blue < channel) ++count;
        }
    }
    return count;
}

bool AnyDark(const unsigned *pixels, int width, int x, int y, int channel) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const unsigned value = pixels[static_cast<std::size_t>(y + dy) * width + (x + dx)];
            const int red = static_cast<int>(value & 0xFF);
            const int green = static_cast<int>((value >> 8) & 0xFF);
            const int blue = static_cast<int>((value >> 16) & 0xFF);
            if (red < channel && green < channel && blue < channel) return true;
        }
    }
    return false;
}

// Mirrors the documented mapping of DrawLauncher: the button rectangle is
// physical, the icon box is the button side minus two logical pixels per side,
// and the regular asset (viewport 20) is used from a 22 pixel button upwards.
struct IconGeometry {
    int left = 0;
    int top = 0;
    int side = 0;
    double scale = 1.0;
    bool regular = true;
};

IconGeometry LauncherIconRect(HDC dc, int x, int y, int width, int height, bool pressed) {
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    if (dpi < 48) dpi = 96;
    const int buttonSide = (width < height) ? width : height;
    int logical = static_cast<int>(std::lround(buttonSide * 96.0 / dpi));
    if (logical < 1) logical = 1;
    const bool regular = logical >= 21;
    int inset = 0;
    if (logical == 18 || logical == 20 || logical == 22 || logical == 24)
        inset = (std::max)(1, MulDiv(2, dpi, 96));
    else
        inset = (std::max)(1, (buttonSide + 5) / 11);
    const int offset = pressed ? (std::max)(1, MulDiv(1, dpi, 96)) : 0;
    IconGeometry geometry;
    geometry.side = buttonSide - 2 * inset;
    geometry.left = x + (width - geometry.side) / 2 + offset;
    geometry.top = y + (height - geometry.side) / 2 + offset;
    geometry.regular = regular;
    geometry.scale = geometry.side / (regular ? 20.0 : 14.0);
    return geometry;
}

int MapX(const IconGeometry &geometry, double u) {
    return geometry.left + static_cast<int>(std::lround(u * geometry.scale));
}

int MapY(const IconGeometry &geometry, double v) {
    return geometry.top + static_cast<int>(std::lround(v * geometry.scale));
}

void LauncherIcon() {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    const int width = 160;
    const int height = 96;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Check(bitmap != nullptr && bits != nullptr, "a 32-bit memory bitmap is available");
    HGDIOBJ previous = SelectObject(dc, bitmap);
    const auto *pixels = static_cast<const unsigned *>(bits);
    RECT all = {0, 0, width, height};

    // 22 pixel button: regular asset, viewport 20, icon box 18.
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 4, 4, 22, 22, false, false, false);
    IconGeometry regular = LauncherIconRect(dc, 4, 4, 22, 22, false);
    Check(regular.regular, "a 22 pixel button uses the regular asset");
    const int regularArea = regular.side * regular.side;
    const int regularInk =
        CountInk(pixels, width, regular.left, regular.top, regular.left + regular.side, regular.top + regular.side, 200);
    Check(regularInk > regularArea / 20, "the 22 pixel launcher draws its box and curve");
    Check(regularInk < regularArea / 2, "the 22 pixel launcher leaves the icon box hollow");
    // The SVG box sits at 1.5 and 18.5 of a 20 unit viewport.
    Check(AnyDark(pixels, width, MapX(regular, 10.0), MapY(regular, 1.5), 200), "the regular asset draws its top edge");
    Check(AnyDark(pixels, width, MapX(regular, 1.5), MapY(regular, 10.0), 200), "the regular asset draws its left edge");
    // The cubic passes exactly through (10, 10) of the viewport.
    Check(AnyDark(pixels, width, MapX(regular, 10.0), MapY(regular, 10.0), 200),
          "the regular asset curve passes through the icon centre");

    // 18 pixel button: compact asset, viewport 14, icon box 14.
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 4, 40, 18, 18, false, false, false);
    IconGeometry compact = LauncherIconRect(dc, 4, 40, 18, 18, false);
    Check(!compact.regular, "an 18 pixel button uses the compact asset");
    const int compactArea = compact.side * compact.side;
    const int compactInk =
        CountInk(pixels, width, compact.left, compact.top, compact.left + compact.side, compact.top + compact.side, 200);
    Check(compactInk > compactArea / 20, "the 18 pixel launcher draws its box and curve");
    Check(compactInk < compactArea / 2, "the 18 pixel launcher leaves the icon box hollow");
    // The compact cubic passes through (7, 7) of its 14 unit viewport.
    Check(AnyDark(pixels, width, MapX(compact, 7.0), MapY(compact, 7.0), 200),
          "the compact asset curve passes through its centre");
    Check(AnyDark(pixels, width, MapX(compact, 7.0), MapY(compact, 1.5), 200), "the compact asset draws its top edge");

    // Pressed, focused and disabled states must draw without changing geometry.
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 4, 4, 24, 24, true, false, true);
    IconGeometry pressed = LauncherIconRect(dc, 4, 4, 24, 24, true);
    Check(CountInk(pixels, width, pressed.left, pressed.top, pressed.left + pressed.side, pressed.top + pressed.side, 200) >
              pressed.side * pressed.side / 20,
          "the 24 pixel pressed and focused launcher draws");
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 60, 4, 22, 22, false, true, false);
    IconGeometry disabled = LauncherIconRect(dc, 60, 4, 22, 22, false);
    Check(CountInk(pixels, width, disabled.left, disabled.top, disabled.left + disabled.side,
                   disabled.top + disabled.side, 220) > disabled.side * disabled.side / 20,
          "the disabled launcher still draws its icon");

    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    Note("launcher icon geometry for the 22 and 18 pixel buttons");
}

// ---------------------------------------------------------------------------
// 8. Layout in the three languages.
// ---------------------------------------------------------------------------
struct ChildList {
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectChild(HWND child, LPARAM param) {
    reinterpret_cast<ChildList *>(param)->windows.push_back(child);
    return TRUE;
}

struct Rect {
    RECT rect = {};
    std::wstring className;
    bool visible = false;
};

void CheckLayout(HWND window, const wchar_t *language) {
    RECT client = {};
    GetClientRect(window, &client);
    ChildList children;
    EnumChildWindows(window, CollectChild, reinterpret_cast<LPARAM>(&children));
    Check(children.windows.size() > 20, "the editor builds its full control set");

    std::vector<Rect> rects;
    for (HWND child : children.windows) {
        Rect entry;
        // The style bit, not IsWindowVisible: the editor of a background test
        // is hidden, and IsWindowVisible would then report every child as
        // invisible and silently skip the whole geometry check.
        entry.visible = (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0;
        if (!entry.visible) continue;
        GetWindowRect(child, &entry.rect);
        POINT corners[2] = {{entry.rect.left, entry.rect.top}, {entry.rect.right, entry.rect.bottom}};
        MapWindowPoints(nullptr, window, corners, 2);
        entry.rect.left = corners[0].x;
        entry.rect.top = corners[0].y;
        entry.rect.right = corners[1].x;
        entry.rect.bottom = corners[1].y;
        wchar_t name[64] = {};
        GetClassNameW(child, name, 64);
        entry.className = name;
        rects.push_back(entry);
        Check(entry.rect.left >= 0 && entry.rect.top >= 0 && entry.rect.right <= client.right &&
                  entry.rect.bottom <= client.bottom,
              "every visible control stays inside the client area");
    }

    for (std::size_t first = 0; first < rects.size(); ++first) {
        for (std::size_t second = first + 1; second < rects.size(); ++second) {
            const RECT &a = rects[first].rect;
            const RECT &b = rects[second].rect;
            const bool overlap = a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
            if (overlap) {
                std::fprintf(stderr, "FAIL: overlapping controls in language %ls: %ls and %ls\n", language,
                             rects[first].className.c_str(), rects[second].className.c_str());
                std::exit(1);
            }
        }
    }

    const struct {
        int label;
        int control;
        bool vertical;
    } pairs[] = {
        {kIdSourceLabel, kIdSourceCombo, false},   {kIdViewLabel, kIdViewCombo, false},
        {kIdToolLabel, kIdToolCombo, false},       {kIdChannelLabel, kIdChannelList, true},

        {kIdFrameLabel, kIdFrameEdit, false},      {kIdValueLabel, kIdValueEdit, false},
        {kIdInterpolationLabel, kIdInterpolationCombo, false},
        {kIdTangentLabel, kIdTangentCombo, false}, {kIdInSlopeLabel, kIdInSlopeEdit, false},
        {kIdOutSlopeLabel, kIdOutSlopeEdit, false}, {kIdInWeightLabel, kIdInWeightEdit, false},
        {kIdOutWeightLabel, kIdOutWeightEdit, false},
        {kIdBakeFirstLabel, kIdBakeFirstEdit, false},
        {kIdBakeLastLabel, kIdBakeLastEdit, false}, {kIdBakeStepLabel, kIdBakeStepEdit, false},
    };
    for (const auto &pair : pairs) {
        HWND label = GetDlgItem(window, pair.label);
        HWND control = GetDlgItem(window, pair.control);
        Check(label != nullptr && control != nullptr, "both members of a label pair exist");
        if(!(GetWindowLongPtrW(label,GWL_STYLE)&WS_VISIBLE)||!(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE))continue;
        RECT labelRect = {};
        RECT controlRect = {};
        GetWindowRect(label, &labelRect);
        GetWindowRect(control, &controlRect);
        POINT labelCorners[2] = {{labelRect.left, labelRect.top}, {labelRect.right, labelRect.bottom}};
        POINT controlCorners[2] = {{controlRect.left, controlRect.top}, {controlRect.right, controlRect.bottom}};
        MapWindowPoints(nullptr, window, labelCorners, 2);
        MapWindowPoints(nullptr, window, controlCorners, 2);
        const int gap = pair.vertical ? controlCorners[0].y - labelCorners[1].y
                                      : controlCorners[0].x - labelCorners[1].x;
        if (gap < 4) {
            std::fprintf(stderr, "FAIL: label gap %d in language %ls (ids %d/%d)\n", gap, language, pair.label,
                         pair.control);
            std::exit(1);
        }
        ++g_checks;
    }

    for (HWND child : children.windows) {
        wchar_t name[64] = {};
        GetClassNameW(child, name, 64);
        if (std::wstring(name) == L"Edit") {
            const DWORD margins = static_cast<DWORD>(SendMessageW(child, EM_GETMARGINS, 0, 0));
            Check(LOWORD(margins) >= 4 && HIWORD(margins) >= 4, "an edit box keeps at least 4 logical pixels of padding");
            RECT rect = {};
            GetClientRect(child, &rect);
            Check(rect.bottom - rect.top >= 16, "an edit box is tall enough for its font");
        }
    }
    (void)language;
}

void LayoutThreeLanguages() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host, 0);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the editor opens for the layout check");

    const wchar_t *names[3] = {L"ja", L"en", L"zh"};
    const wchar_t *addKeyText[3] = {L"\u30AD\u30FC\u8FFD\u52A0", L"Add key", L"\u6DFB\u52A0\u5173\u952E\u5E27"};
    for (unsigned language = 0; language < 3; ++language) {
        ce::SetLanguage(nullptr, language);
        Check(GetText(window, kIdAddKey) == addKeyText[language], "SetLanguage retranslates the controls");
        Check(GetText(window, kIdFrameEdit).empty() == false, "SetLanguage preserves the editing state");
        Check(KeyCount(window) == 2, "SetLanguage preserves the loaded channels");
        Check(GetText(window, kIdStatus).empty() == false, "a status line is always present");
        Check(IsWindowEnabled(GetDlgItem(window, kIdAddKey)) != FALSE, "language switching keeps editing enabled");
        wchar_t caption[256] = {};
        GetWindowTextW(window, caption, 256);
        Check(std::wstring(caption).find(L"Test source") != std::wstring::npos, "the title keeps naming the source");
        CheckLayout(window, names[language]);
    }

    // The layout must survive the smallest window the editor allows.  The
    // minimum comes from the editor itself, so this check scales with the DPI.
    MINMAXINFO limits = {};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    Check(limits.ptMinTrackSize.x > 0 && limits.ptMinTrackSize.y > 0, "the editor publishes a minimum window size");
    SetWindowPos(window, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(limits.ptMinTrackSize.x, limits.ptMinTrackSize.y));
    CheckLayout(window, L"minimum");

    ce::CloseOwned(nullptr);
    Note("three-language layout with minimum size, gaps and padding");
}

// ---------------------------------------------------------------------------
// 9. Motion callbacks gate their buttons.
// ---------------------------------------------------------------------------
void MotionButtons() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    host.hasReadMotion = true;
    host.hasWriteMotion = true;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(IsWindowEnabled(GetDlgItem(window, kIdReadMotion)) != FALSE, "the read motion button is enabled with its callback");
    Check(IsWindowEnabled(GetDlgItem(window, kIdWriteMotion)) != FALSE, "the write motion button is enabled with its callback");
    Click(window, kIdReadMotion);
    Check(host.readMotionCalls == 1, "the read motion callback runs");
    Check(host.readCalls >= 2, "a successful motion read re-reads the source");
    Click(window, kIdWriteMotion);
    Check(host.writeMotionCalls == 1, "the write motion callback runs");
    ce::CloseOwned(nullptr);

    HostSource plain;
    plain.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0)}));
    ce::Session plainSession = MakeSession(plain);
    HWND plainWindow = static_cast<HWND>(ce::Open(nullptr, plainSession, false));
    Check(IsWindowEnabled(GetDlgItem(plainWindow, kIdReadMotion)) == FALSE,
          "the read motion button is disabled without its callback");
    Check(IsWindowEnabled(GetDlgItem(plainWindow, kIdWriteMotion)) == FALSE,
          "the write motion button is disabled without its callback");
    ce::CloseOwned(nullptr);
    Note("motion buttons follow the presence of their callbacks");
}

// ---------------------------------------------------------------------------
// 7. Rubber-band selection moves every selected key together.
// ---------------------------------------------------------------------------
void RubberBandMultiMove() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);

    const POINT first = AskPoint(window, kHookPointForKey, 1);
    const POINT last = AskPoint(window, kHookPointForKey, 2);
    Check(first.x > 0 && last.x > 0, "both keys are visible before the rubber band");
    // Starting on empty canvas clears the old selection and drags a box around
    // both keys; the corners come from the two key centres plus a margin.
    const int left = (first.x < last.x ? first.x : last.x) - 20;
    const int right = (first.x > last.x ? first.x : last.x) + 20;
    const int top = (first.y < last.y ? first.y : last.y) - 20;
    const int bottom = (first.y > last.y ? first.y : last.y) + 20;
    const POINT from = {left, top};
    const POINT to = {right, bottom};
    // Starting on empty canvas clears the old selection and drags a box.
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
    const std::wstring rubberStatus = GetText(window, kIdStatus);
    if (rubberStatus.find(L"2") == std::wstring::npos) {
        // The text is localised, so print the code units instead of %ls.
        std::fprintf(stderr, "FAIL: rubber band status length %d:", static_cast<int>(rubberStatus.size()));
        for (const wchar_t unit : rubberStatus) std::fprintf(stderr, " %04X", static_cast<unsigned>(unit));
        std::fprintf(stderr, " from %d,%d to %d,%d; keys %d,%d and %d,%d\n", static_cast<int>(from.x),
                     static_cast<int>(from.y), static_cast<int>(to.x), static_cast<int>(to.y),
                     static_cast<int>(first.x), static_cast<int>(first.y), static_cast<int>(last.x),
                     static_cast<int>(last.y));
        std::exit(1);
    }
    ++g_checks;

    const int applies = host.applyCalls;
    DragKeyToFrame(window, 1, 15);
    Check(host.applyCalls == applies + 1, "a multi-key drag commits once");
    Check(FindKey(host.data, 0, 15) != nullptr, "the dragged key moved to the target frame");
    Check(FindKey(host.data, 0, 25) != nullptr, "the other selected key moved by the same delta");
    Check(FindKey(host.data, 0, 10) == nullptr && FindKey(host.data, 0, 20) == nullptr,
          "no key was left behind at the old frames");

    ce::CloseOwned(nullptr);
    Note("rubber-band selection and multi-key dragging");
}

// ---------------------------------------------------------------------------
// 8. Channel selection and whole-range baking.
// ---------------------------------------------------------------------------
void BakeSelectedChannels() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    host.data.push_back(MakeChannel("pos.z", L"Position Z",
                                    {MakeKey(0, 1.0), MakeKey(10, 2.0), MakeKey(20, 3.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    Click(window, kIdChannelAll);
    SetText(window, kIdBakeFirstEdit, L"0");
    SetText(window, kIdBakeLastEdit, L"10");
    SetText(window, kIdBakeStepEdit, L"5");
    const int applies = host.applyCalls;
    Click(window, kIdBakeRun);
    Check(host.applyCalls == applies, "a range that does not cover every selected track is refused");
    Check(GetText(window, kIdStatus).find(L"partial-range") != std::wstring::npos,
          "the core error for a partial range reaches the status line");

    SetText(window, kIdBakeLastEdit, L"20");
    SetText(window, kIdBakeStepEdit, L"10");
    Click(window, kIdBakeRun);
    Check(host.applyCalls == applies + 1, "the whole range is baked through one apply");
    Check(host.data[0].track.keys.size() == 3, "the short channel is resampled over the whole range");
    Check(host.data[1].track.keys.size() == 3, "the long channel is resampled too");
    Check(host.data[0].track.keys[1].frame == 10 && host.data[0].track.keys[2].frame == 20,
          "the baked samples land on the requested step");
    Check(host.data[0].track.keys[0].interpolation == ac::Interpolation::Linear,
          "a baked sample interval becomes straight");

    Click(window, kIdUndo);
    Check(host.data[0].track.keys.size() == 2, "Undo restores the pre-bake track through apply");
    ce::CloseOwned(nullptr);
    Note("channel selection and whole-range baking with the core range check");
}

// ---------------------------------------------------------------------------
// 9. The painting paths render into a memory DC (no desktop pixel is read).
// ---------------------------------------------------------------------------
int RenderInk(HWND child) {
    RECT client = {};
    GetClientRect(child, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return -1;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return -1;
    }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all = {0, 0, width, height};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    const auto *pixels = static_cast<const unsigned *>(bits);
    const unsigned background = pixels[0];
    int ink = 0;
    const int total = width * height;
    for (int index = 0; index < total; ++index) {
        if (pixels[index] != background) ++ink;
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ink;
}

void CanvasPainting() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    host.data.push_back(MakeChannel("flag", L"Visible", {MakeKey(0, 1.0), MakeKey(10, 0.0)}));
    host.data[1].track.discrete = true;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    HWND mmd = GetDlgItem(window, kIdMmdPanel);
    Check(canvas != nullptr && mmd != nullptr, "the painting surfaces exist");

    Check(RenderInk(canvas) > 100, "the value view paints the curve, keys and range bars");
    ClickKey(window, 1);
    Check(RenderInk(canvas) > 100, "the selected key paints its tangent handles");
    Check(RenderInk(mmd) > 100, "the MMD panel paints the square and its two handles");

    HWND view = GetDlgItem(window, kIdViewCombo);
    SendMessageW(view, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(kIdViewCombo, CBN_SELCHANGE), reinterpret_cast<LPARAM>(view));
    Check(RenderInk(canvas) > 100, "the speed view paints the analytic derivative");

    HWND list = GetDlgItem(window, kIdChannelList);
    DoubleClickChannel(window, 1);
    Check(ChannelCaretIndex(window) == 1,
          "a list double click moves the caret to that row");
    Check(KeyCount(window) == 2, "the double click made the second channel the active one");
    Check(RenderInk(canvas) > 20, "a discrete channel in the speed view paints its notice instead of a curve");
    Check(RenderInk(mmd) > 20, "an interval with no handles still paints the disabled square");

    // A channel without keys must fall back to the empty notice.
    HostSource empty;
    empty.data.push_back(MakeChannel("empty", L"Empty", {}));
    ce::Session emptySession = MakeSession(empty);
    ce::CloseOwned(nullptr);
    HWND emptyWindow = static_cast<HWND>(ce::Open(nullptr, emptySession, false));
    HWND emptyCanvas = GetDlgItem(emptyWindow, kIdCanvas);
    Check(RenderInk(emptyCanvas) > 20, "an empty channel paints its notice");
    ce::CloseOwned(nullptr);
    Note("canvas and MMD panel painting into a memory DC");
}

// ---------------------------------------------------------------------------
// 11. Adding over an existing frame selects that key instead of writing.
// ---------------------------------------------------------------------------
void DuplicateFrameSelectsExisting() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    SetText(window, kIdFrameEdit, L"10");
    SetText(window, kIdValueEdit, L"99");
    const int applies = host.applyCalls;
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "adding over an existing frame writes nothing");
    Check(KeyCount(window) == 2, "the duplicate frame does not add a second key");
    Check(GetText(window, kIdValueEdit) == L"5", "the existing key is selected and its value is loaded");
    Check(GetText(window, kIdStatus).find(L"10") != std::wstring::npos,
          "the status says which frame is now selected");

    // The selection really is the existing key, so Delete removes that one.
    Click(window, kIdDeleteKey);
    Check(host.applyCalls == applies + 1, "the newly selected key can be deleted");
    Check(KeyCount(window) == 1, "deleting removed the key at frame 10");
    Check(FindKey(host.data, 0, 10) == nullptr, "the key at frame 10 is gone");
    ce::CloseOwned(nullptr);
    Note("an add over an existing frame selects it instead of writing");
}

// ---------------------------------------------------------------------------
// 12. Deleting re-runs the automatic tangents and frames stay inside int32.
// ---------------------------------------------------------------------------
void DeleteRecomputesAutomatic() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    ClickKey(window, 1);
    Click(window, kIdDeleteKey);
    Check(KeyCount(window) == 2, "the middle key is gone");
    const ac::Key *first = FindKey(host.data, 0, 0);
    const ac::Key *last = FindKey(host.data, 0, 20);
    Check(first != nullptr && last != nullptr, "the outer keys survive");
    const double secant = (9.0 - 0.0) / 20.0;
    Check(first && std::fabs(first->outSlope - secant) < 1e-12,
          "the surviving first key gets the automatic slope of its new interval");
    Check(last && std::fabs(last->inSlope - secant) < 1e-12,
          "the surviving last key gets the automatic slope of its new interval");
    ce::CloseOwned(nullptr);
    Note("deleting a key recomputes the automatic tangents of the remaining track");
}

void FrameRangeRejected() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    SetText(window, kIdValueEdit, L"1");
    const int applies = host.applyCalls;
    SetText(window, kIdFrameEdit, L"2147483648");  // one past std::int32_t
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "a frame above the int32 range is refused");
    Check(KeyCount(window) == 1, "the refused frame leaves the track alone");
    SetText(window, kIdFrameEdit, L"3000000000");
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "a far larger frame is refused as well");
    SetText(window, kIdFrameEdit, L"4294967296");
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "a frame beyond 32 bits is refused as well");

    SetText(window, kIdFrameEdit, L"2147483647");  // exactly std::int32_t
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies + 1, "the largest int32 frame is accepted");
    Check(FindKey(host.data, 0, 2147483647) != nullptr, "the boundary frame is stored");
    ce::CloseOwned(nullptr);
    Note("frame numbers are range checked before the int32 conversion");
}

// ---------------------------------------------------------------------------
// 13. Localized source names and a read that only renamed channels.
// ---------------------------------------------------------------------------
void LocalizedSourceLabel() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0)}));
    ce::Session session = MakeSession(host, 1);
    session.sources[0].label = L"Plain source";
    session.sources[0].localizedLabel = [](unsigned language) -> std::wstring {
        switch (language) {
        case 0: return L"\u30AB\u30E1\u30E9";
        case 1: return L"Camera";
        default: return L"\u76F8\u673A";
        }
    };
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND combo = GetDlgItem(window, kIdSourceCombo);
    wchar_t item[128] = {};
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(item));
    Check(std::wstring(item) == L"Camera", "the combo uses the localized source name");

    ce::SetLanguage(nullptr, 2);
    item[0] = L'\0';
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(item));
    Check(std::wstring(item) == L"\u76F8\u673A", "SetLanguage re-localizes the source name");
    wchar_t caption[256] = {};
    GetWindowTextW(window, caption, 256);
    Check(std::wstring(caption).find(L"\u76F8\u673A") != std::wstring::npos,
          "the window title follows the localized name");
    ce::SetLanguage(nullptr, 0);
    item[0] = L'\0';
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(item));
    Check(std::wstring(item) == L"\u30AB\u30E1\u30E9", "language 0 uses the Japanese name");
    ce::CloseOwned(nullptr);
    Note("Source::localizedLabel drives the combo and the title in every language");
}

void ReadKeepsSelectionAndHistory() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    SetText(window, kIdFrameEdit, L"5");
    SetText(window, kIdValueEdit, L"2.5");
    Click(window, kIdAddKey);
    Check(KeyCount(window) == 3, "the edit produced an undo entry and a selection");
    Check(GetText(window, kIdFrameEdit) == L"5", "the new key is selected");

    // The host only renames the channel and gives it a unit.
    host.data[0].label = L"Position X renamed";
    host.data[0].unit = L"m";
    SendMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    // Native synchronization is posted and coalesced, like the real UI loop.
    MSG change{};
    while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Check(KeyCount(window) == 3, "a rename-only re-read keeps every key");
    HWND list = GetDlgItem(window, kIdChannelList);
    wchar_t item[256] = {};
    HTREEITEM first=TreeView_GetRoot(list);
    while(first && TreeView_GetChild(list,first))first=TreeView_GetChild(list,first);
    TVITEMW treeItem{};treeItem.mask=TVIF_TEXT;treeItem.hItem=first;treeItem.pszText=item;treeItem.cchTextMax=256;
    SendMessageW(list,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&treeItem));
    Check(std::wstring(item).find(L"renamed") != std::wstring::npos, "the new channel label is adopted");
    Check(std::wstring(item).find(L"m") != std::wstring::npos, "the new channel unit is adopted");
    Check(GetText(window, kIdFrameEdit) == L"5", "the local selection survives the rename");
    const int applies = host.applyCalls;
    Click(window, kIdUndo);
    Check(host.applyCalls == applies + 1, "the local undo stack survives the rename");
    Check(KeyCount(window) == 2, "undo still removes the local edit");

    // A real value change on activation is still treated as an external edit.
    host.data[0].track.keys[0].value = 42.0;
    SendMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Check(GetText(window, kIdFrameEdit) == L"30", "a real external edit clears the local selection");
    const int appliesAfter = host.applyCalls;
    Click(window, kIdUndo);
    Check(host.applyCalls == appliesAfter, "a real external edit also clears the undo stack");
    ce::CloseOwned(nullptr);
    Note("a rename-only re-read keeps the selection and history, a value change does not");
}

// ---------------------------------------------------------------------------
// 14. The playhead timer does nothing while the window is hidden.
// ---------------------------------------------------------------------------
void PendingNativeChangesBeforeWrite(bool notify = true) {
    HostSource host;
    host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(30,1)}));
    HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    SetText(window,kIdFrameEdit,L"15");SetText(window,kIdValueEdit,L"0.5");Click(window,kIdAddKey);
    host.data[0].track.keys.push_back(MakeKey(60,2));
    if(notify)ce::NotifySourceChanged(nullptr,"source");
    const int applies=host.applyCalls;
    SetText(window,kIdFrameEdit,L"45");SetText(window,kIdValueEdit,L"1.5");Click(window,kIdAddKey);
    Check(FindKey(host.data,0,60)!=nullptr,"a queued native registration survives a curve-editor write");
    Check(host.applyCalls==applies,"a stale curve draft is refreshed before it can overwrite native keys");
    Click(window,kIdUndo);
    Check(host.applyCalls==applies,"stale undo cannot remove a newly registered native key");
    SetText(window,kIdFrameEdit,L"45");SetText(window,kIdValueEdit,L"1.5");Click(window,kIdAddKey);
    Check(FindKey(host.data,0,45)&&FindKey(host.data,0,60),"retrying after refresh keeps both native and editor registrations");
    ce::CloseOwned(nullptr);
    Note("pending native changes cannot be overwritten by an editor draft or undo");
}

void NativeSelectionBeforeDelete(bool activate = true) {
    HostSource host;
    host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2),MakeKey(30,3)}));
    auto session=MakeSession(host);
    std::vector<int> selected;
    session.sources[0].readKeySelection=[&](const auto&){return selected;};
    session.sources[0].writeKeySelection=[&](const auto&,const auto& frames){selected=frames;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    ClickKey(window,1);
    Check(selected==std::vector<int>({10}),"curve selection reaches the native timeline");
    selected={20};
    if(activate)SendMessageW(window,WM_ACTIVATE,WA_ACTIVE,0);
    MSG change{};
    while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Click(window,kIdDeleteKey);
    Check(FindKey(host.data,0,10)&&!FindKey(host.data,0,20),"delete immediately after native selection targets the native selection, without waiting for a timer");
    ce::CloseOwned(nullptr);
}

void NativeEditsBeforeHistory() {
    for(int change=0;change<4;++change){
        HostSource host;
        host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2)}));
        HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
        SetText(window,kIdFrameEdit,L"15");SetText(window,kIdValueEdit,L"1.5");Click(window,kIdAddKey);
        if(change==3)Click(window,kIdUndo);
        if(change==0)host.data[0].track.keys.back().value=7;
        if(change==1)host.data[0].track.keys.erase(host.data[0].track.keys.begin()+1);
        if(change==2)host.data[0].track.keys.back().frame=21;
        if(change==3)host.data[0].track.keys.push_back(MakeKey(40,4));
        const int applies=host.applyCalls;
        Click(window,change==3?kIdRedo:kIdUndo);
        Check(host.applyCalls==applies,"native value/delete/move/registration changes invalidate stale editor history");
        Check(KeyCount(window)==int(host.data[0].track.keys.size()),"history rejection refreshes the actual native keys");
        SetText(window,kIdFrameEdit,L"45");SetText(window,kIdValueEdit,L"4.5");Click(window,kIdAddKey);
        Check(host.applyCalls==applies+1&&FindKey(host.data,0,45),"editing resumes after adopting a native change");
        if(change==0)Check(FindKey(host.data,0,20)->value==7,"retry keeps the native value change");
        if(change==1)Check(!FindKey(host.data,0,10),"retry never resurrects a natively deleted key");
        if(change==2)Check(FindKey(host.data,0,21)&&!FindKey(host.data,0,20),"retry keeps the native key move");
        if(change==3)Check(FindKey(host.data,0,40)&&!FindKey(host.data,0,15),"redo never erases a newly registered native key");
        ce::CloseOwned(nullptr);
    }
}

void CanonicalWriteAndUndo() {
    HostSource host;host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(10,1)}));
    auto session=MakeSession(host);auto apply=session.sources[0].apply;
    session.sources[0].apply=[apply](const auto& channels,auto& error){
        auto quantized=channels;
        for(auto& channel:quantized)for(auto& key:channel.track.keys)key.value=std::round(key.value*100)/100;
        return apply(quantized,error);
    };
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    ClickKey(window,1);SetText(window,kIdValueEdit,L"0.333333");Click(window,kIdApplyKey);
    Check(GetText(window,kIdValueEdit)==L"0.33","fields display the value actually accepted by the native source");
    Click(window,kIdUndo);Check(FindKey(host.data,0,10)->value==1,"native rounding does not invalidate our own undo");
    Click(window,kIdRedo);Check(FindKey(host.data,0,10)->value==0.33,"redo restores the accepted native value");
    ce::CloseOwned(nullptr);
}

void NativeChangesDuringMmdDrag() {
    HostSource host;host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(30,1)}));
    std::string error;Check(ce::SetChannelControls(host.data[0],1,{20,20,107,107},error),"initialize native drag interval");
    HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));ClickKey(window,1);
    HWND panel=GetDlgItem(window,kIdMmdPanel);double controls[4]{};
    Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls))!=0,"native handles are editable");
    const auto getDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    UINT dpi=getDpi?getDpi(panel):96;RECT client{};GetClientRect(panel,&client);
    const int margin=MulDiv(6,dpi?dpi:96,96),side=std::min(int(client.right),int(client.bottom))-2*margin;
    const LPARAM point=MAKELPARAM(margin+int(std::lround(controls[0]*side/127)),margin+side-int(std::lround(controls[1]*side/127)));
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point);
    const int reads=host.readCalls,applies=host.applyCalls;
    host.data[0].track.keys.push_back(MakeKey(60,2));ce::NotifySourceChanged(nullptr,"source");
    MSG change{};while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Check(host.readCalls==reads,"native changes wait while an MMD handle drag owns the interval");
    SendMessageW(panel,WM_LBUTTONUP,0,point);
    Check(host.applyCalls==applies&&FindKey(host.data,0,60),"releasing a stale native handle cannot overwrite a registered key");
    ce::CloseOwned(nullptr);
}

void PlayheadTimer() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    double frame = 4.0;
    session.currentFrame = [&frame] { return frame; };
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    long long state[3] = {};

    const auto readState = [&] { SendMessageW(window, kHookPlayheadState, 0, reinterpret_cast<LPARAM>(state)); };
    readState();
    Check(state[0] == 0, "a hidden editor runs no playhead timer");
    Check(IsWindowVisible(window) == FALSE, "the playhead test keeps the editor hidden");

    const int reads = host.readCalls;
    const int applies = host.applyCalls;
    frame = 5.0;
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[1] == 0 && state[2] == 0, "ticks of a hidden editor neither count nor repaint");
    Check(host.readCalls == reads && host.applyCalls == applies, "a tick never reads or writes the source");

    // The visible branch is exercised without ever showing the window: setting
    // the WS_VISIBLE style bit makes IsWindowVisible true without displaying
    // anything on the desktop, and the style is restored right afterwards.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_VISIBLE);
    Check(IsWindowVisible(window) != FALSE, "the style bit makes the window report itself visible");
    frame = 6.0;
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[1] == 1, "a visible tick is processed");
    Check(state[2] == 1, "a changed frame repaints the playhead");
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[2] == 1, "an unchanged frame does not repaint again");
    frame = 7.0;
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[2] == 2, "the next frame change repaints again");
    Check(host.applyCalls == applies, "no tick ever wrote channel data");
    SetWindowLongPtrW(window, GWL_STYLE, style);
    Check(IsWindowVisible(window) == FALSE, "the style bit is restored");
    ce::CloseOwned(nullptr);
    Note("the playhead timer refreshes only a visible, changed frame");
}

// ---------------------------------------------------------------------------
// 15. Range-bar segments take the arrow keys.
// ---------------------------------------------------------------------------
void RangeBarKeyboard() {
    HostSource host;
    // Leave room on both sides for directional assertions. Zero is tested
    // separately below; the editor intentionally never pans to negative time.
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(20, 0.0), MakeKey(40, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    double view[4] = {};

    // Without a bar click the arrows must not move the view.
    ReadView(window, view);
    const double startBefore = view[0];
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(view[0] == startBefore, "arrow keys do nothing before a range bar is clicked");

    // Time bar centre pans, one frame per press.
    ClickBar(window, 1, 0);
    ReadView(window, view);
    const double panStart = view[0], panEnd = view[1];
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs(view[0] - (panStart + 1.0)) < 1e-9 && std::fabs(view[1] - (panEnd + 1.0)) < 1e-9,
          "the time bar centre pans by one frame");
    PressKey(window, VK_LEFT);
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(std::fabs(view[0] - (panStart - 1.0)) < 1e-9, "the left arrow pans the other way");

    // Time bar low end drags the left edge.
    ClickBar(window, 1, 1);
    ReadView(window, view);
    const double fixedEnd = view[1];
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs(view[0] - (panStart)) < 1e-9 && std::fabs(view[1] - fixedEnd) < 1e-9,
          "the low grip moves only the left edge");
    ClickBar(window, 1, 2);
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs(view[0] - panStart) < 1e-9 && std::fabs(view[1] - (fixedEnd + 1.0)) < 1e-9,
          "the high grip moves only the right edge outward");
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(std::fabs(view[0] - panStart) < 1e-9 && std::fabs(view[1] - fixedEnd) < 1e-9,
          "the high grip moves only the right edge inward");
    ClickBar(window, 1, 1);
    for (int step = 0; step < 200; ++step) PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs((view[1] - view[0]) - kMinimumTimeSpan) < 1e-9,
          "the time span stops shrinking at the one-frame minimum");

    // Value bar high end (the top edge) drags the upper value and clamps.
    ClickBar(window, 2, 2);
    ReadView(window, view);
    const double upperLowBefore = view[2];
    const double upperHighBefore = view[3];
    PressKey(window, VK_DOWN);
    ReadView(window, view);
    if (!(view[2] == upperLowBefore && view[3] < upperHighBefore)) {
        const POINT probe = AskBarPoint(window, 2, 2);
        std::fprintf(stderr, "FAIL: value bar high grip %.6f,%.6f -> %.6f,%.6f (probe %d,%d)\n", upperLowBefore,
                     upperHighBefore, view[2], view[3], static_cast<int>(probe.x), static_cast<int>(probe.y));
        std::exit(1);
    }
    ++g_checks;
    const double highAfter = view[3];
    PressKey(window, VK_UP);
    ReadView(window, view);
    Check(view[2] == upperLowBefore && view[3] > highAfter, "the up arrow raises the upper edge again");

    // The low end (the bottom edge) drags the lower value only.  This runs
    // before the span reaches its minimum, where the clamp would freeze it.
    ClickBar(window, 2, 1);
    ReadView(window, view);
    const double keptHigh = view[3];
    const double keptLow = view[2];
    PressKey(window, VK_UP);
    ReadView(window, view);
    Check(view[3] == keptHigh && view[2] > keptLow, "the value bar low grip moves only the lower edge");

    // Test the centre while it is distinct from the endpoint hit zones.
    // At the minimum span the overview thumb can be less than one pixel tall,
    // so its geometric midpoint belongs to an endpoint, not a pan segment.
    ClickBar(window, 2, 0);
    ReadView(window, view);
    const double lowBefore = view[2], highBefore = view[3];
    const double valueStep = (highBefore - lowBefore) * 0.02;
    PressKey(window, VK_DOWN);
    ReadView(window, view);
    Check(std::fabs(view[2] - (lowBefore - valueStep)) < 1e-9 &&
          std::fabs(view[3] - (highBefore - valueStep)) < 1e-9,
          "the value bar centre pans both edges down by the same step");
    PressKey(window, VK_UP);
    ReadView(window, view);
    Check(std::fabs(view[2] - lowBefore) < 1e-9 && std::fabs(view[3] - highBefore) < 1e-9,
          "the value bar centre pans both edges back without changing the span");

    // Each press removes 2% of the current span; the span stops at the view
    // minimum, one thousandth of the data scale (never below 0.001 units).
    ClickBar(window, 2, 2);
    for (int step = 0; step < 1500; ++step) PressKey(window, VK_DOWN);
    ReadView(window, view);
    const double stoppedSpan = view[3] - view[2];
    PressKey(window, VK_DOWN);
    ReadView(window, view);
    Check(stoppedSpan >= 1.0e-3 - 1e-12 && std::fabs((view[3] - view[2]) - stoppedSpan) < 1e-12,
          "the value span stops shrinking at the minimum");
    Check(host.applyCalls == 0, "range-bar keys never write channel data");
    Check(KeyCount(window) == 2, "range-bar keys leave the keyframes alone");

    Click(window, kIdFitAll);
    ClickBar(window, 1, 0);
    ReadView(window, view);
    const double boundarySpan = view[1] - view[0];
    for (int step = 0; step < 60; ++step) PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] == 0.0 && std::fabs(view[1] - boundarySpan) < 1e-9,
          "time panning clamps at zero while preserving the view span");
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] == 0.0 && std::fabs(view[1] - boundarySpan) < 1e-9,
          "another left pan at zero leaves both time edges unchanged");
    ClickBar(window, 1, 1);
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] == 0.0 && std::fabs(view[1] - boundarySpan) < 1e-9,
          "the low time grip also clamps at zero without moving the high edge");
    Check(host.applyCalls == 0 && KeyCount(window) == 2,
          "range boundaries leave the authored keys unchanged");
    ce::CloseOwned(nullptr);
    Note("range-bar segments pan and zoom with the arrow keys");
}

// ---------------------------------------------------------------------------
// 16. Tab navigation between the native controls.
// ---------------------------------------------------------------------------
void TabNavigation() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    host.hasReadMotion = true;
    host.hasWriteMotion = true;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    // SetFocus refuses a window whose top level is hidden, so the style bit is
    // set for the duration of the test; nothing is displayed on the desktop
    // and the style is restored before the window is closed.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_VISIBLE);
    SetFocus(window);

    HWND source = GetDlgItem(window, kIdSourceCombo);
    SendMessageW(source, WM_KEYDOWN, VK_TAB, 0);
    if (FocusId(window) != kIdReadMotion) {
        std::fprintf(stderr, "FAIL: Tab from the source combo focused id %ld (getfocus=%p, combo=%p)\n",
                     static_cast<long>(FocusId(window)), static_cast<void *>(GetFocus()),
                     static_cast<void *>(source));
        std::exit(1);
    }
    ++g_checks;
    HWND read = GetDlgItem(window, kIdReadMotion);
    SendMessageW(read, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdWriteMotion, "Tab keeps walking the reading order");

    Check(IsWindowEnabled(GetDlgItem(window, kIdDeleteKey)) == FALSE, "delete is disabled without a selection");
    Check(IsWindowEnabled(GetDlgItem(window, kIdApplyKey)) == FALSE, "apply is disabled without a selection");
    HWND add = GetDlgItem(window, kIdAddKey);
    SetFocus(add);
    SendMessageW(add, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdUndo, "Tab skips the disabled delete and apply buttons");

    HWND canvas = GetDlgItem(window, kIdCanvas);
    SetFocus(canvas);
    SendMessageW(canvas, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdFrameEdit, "Tab leaves the canvas towards the key fields");

    HWND run = GetDlgItem(window, kIdBakeRun);
    SetFocus(run);
    SendMessageW(run, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdSourceCombo, "Tab wraps around to the first control");
    SetWindowLongPtrW(window, GWL_STYLE, style);
    Check(IsWindowVisible(window) == FALSE, "the style bit is restored");
    ce::CloseOwned(nullptr);
    Note("Tab navigation walks the native controls in reading order");
}

// ---------------------------------------------------------------------------
// 19..23. Comparison display.
// ---------------------------------------------------------------------------
ce::Channel MakeGroupChannel(const std::string &id, const std::wstring &label, int group, std::vector<ac::Key> keys,
                             bool selected = true) {
    ce::Channel channel = MakeChannel(id, label, std::move(keys));
    channel.group = group;
    channel.selected = selected;
    return channel;
}

// A key with explicit slopes and weights, which is what the comparison
// boundary cases need: equal endpoints and a Bezier bump between them.
ac::Key MakeShapeKey(std::int32_t frame, double value, double inSlope = 0.0, double outSlope = 0.0,
                     double inWeight = 1.0 / 3.0, double outWeight = 1.0 / 3.0,
                     ac::Interpolation interpolation = ac::Interpolation::Bezier) {
    ac::Key key = MakeKey(frame, value, interpolation);
    key.inSlope = inSlope;
    key.outSlope = outSlope;
    key.inWeight = inWeight;
    key.outWeight = outWeight;
    return key;
}

// IsWindowVisible also follows the parent chain, so a child of the hidden test
// window always reports false.  The comparison row is checked by its own style
// bit instead; CheckLayout below turns the parent style bit on for the duration
// of its run so its pairwise overlap check is not vacuous either.
bool StyleVisible(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    if (!control) return false;
    return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
}

// A real button click: BM_CLICK toggles an auto check box and notifies the
// parent with BN_CLICKED, so the test drives the same path the user does.
void ClickButton(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the button exists");
    SendMessageW(control, BM_CLICK, 0, 0);
}

// The comparison modes are three direct buttons that act as a radio group, so
// picking a mode is one click and never a drop-down.
void ClickCompareMode(HWND window, int mode) {
    int id = kIdCompareSameGroup;
    if (mode == ce::kCompareAllGroups) id = kIdCompareAllGroups;
    else if (mode == ce::kCompareFree) id = kIdCompareFree;
    ClickButton(window, id);
}

int CheckedCompareMode(HWND window) {
    if (SendMessageW(GetDlgItem(window, kIdCompareSameGroup), BM_GETCHECK, 0, 0) == BST_CHECKED)
        return ce::kCompareSameGroup;
    if (SendMessageW(GetDlgItem(window, kIdCompareAllGroups), BM_GETCHECK, 0, 0) == BST_CHECKED)
        return ce::kCompareAllGroups;
    if (SendMessageW(GetDlgItem(window, kIdCompareFree), BM_GETCHECK, 0, 0) == BST_CHECKED)
        return ce::kCompareFree;
    return ce::kCompareOff;
}

// The invariant the comparison display promises: a row is ticked exactly when
// that curve is on the canvas.  The main curve draws when its own row is ticked;
// every other curve draws when it is in the compared set the hook reports.  Only
// comparison sources are held to this - for a plain source the tick is the bake
// scope and no comparison curve exists.
void CheckTicksMatchCanvas(HWND window, int channelCount, const char *what) {
    int info[12] = {};
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(info));
    for (int index = 0; index < channelCount; ++index) {
        const bool visible = ChannelVisible(window, index) == 1;
        bool drawn = false;
        if (index == info[1]) {
            drawn = visible;
        } else {
            for (int position = 0; position < info[2] && position < 8; ++position) {
                if (info[3 + position] == index) drawn = true;
            }
        }
        if (visible != drawn) {
            std::fprintf(stderr, "FAIL: %s: channel %d is ticked=%d but drawn=%d\n", what, index, visible ? 1 : 0,
                         drawn ? 1 : 0);
            std::exit(1);
        }
        ++g_checks;
    }
}

// Adds a key to the current selection instead of replacing it, the way a
// Ctrl+click does.
void ClickKeyAdditive(HWND window, int keyIndex) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(canvas != nullptr, "the canvas exists for an additive key click");
    const POINT point = AskPoint(window, kHookPointForKey, static_cast<WPARAM>(keyIndex));
    Check(point.x >= 0, "the key is on screen");
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON | MK_CONTROL, position);
    SendMessageW(canvas, WM_LBUTTONUP, 0, position);
}

void ReadCompare(HWND window, int info[12]) {
    for (int index = 0; index < 12; ++index) info[index] = 0;
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(info));
}

// lParam = double[3] {first, last, source}: the span the filter would use now.
bool ReadCompareSpan(HWND window, double span[3]) {
    span[0] = span[1] = span[2] = 0;
    return SendMessageW(window, kHookCompareRange, 0, reinterpret_cast<LPARAM>(span)) != 0;
}

// Reads a hidden WM_PRINTCLIENT render into memory, without desktop pixels.
// Comparison strokes are translucent and antialiased, so their source colour
// is not an exact output pixel value.
std::vector<unsigned> RenderPixels(HWND child) {
    RECT client = {};
    GetClientRect(child, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return {};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return {};
    }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all = {0, 0, width, height};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    const auto *pixels = static_cast<const unsigned *>(bits);
    // A BI_RGB 32 bit DIB keeps its bytes as B, G, R, unused.
    std::vector<unsigned> image;
    const int total = width * height;
    image.reserve(total);
    for (int index = 0; index < total; ++index) {
        image.push_back(pixels[index] & 0x00FFFFFFu);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return image;
}

int CountRedCurvePixels(const std::vector<unsigned> &image) {
    int count = 0;
    for (const unsigned pixel : image) {
        const int red = (pixel >> 16) & 255;
        const int green = (pixel >> 8) & 255;
        const int blue = pixel & 255;
        if (red > green + 20 && red > blue + 20) ++count;
    }
    return count;
}

// The mode set, the stability of the main curve and the host memory.
void ComparisonModes() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareSameGroup;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 0, {MakeKey(0, 0.0), MakeKey(10, 0.5)}));
    host.data.push_back(MakeGroupChannel("m_c", L"C", 1, {MakeKey(0, 0.0), MakeKey(10, 0.25)}));
    // Constant inside its own span, so it must never join an automatic set.
    host.data.push_back(MakeGroupChannel("m_d", L"D", 0, {MakeKey(0, 0.5), MakeKey(10, 0.5)}));
    // No group at all: only the free mode may show it.
    host.data.push_back(MakeGroupChannel("m_e", L"E", -1, {MakeKey(0, 0.0), MakeKey(10, 0.9)}));
    // Registered in a group but empty: never a comparison curve.
    host.data.push_back(MakeGroupChannel("m_f", L"F", 0, {}));

    const int appliesBeforeOpen = host.applyCalls;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the comparison source opens hidden");
    Check(host.modeReads >= 1, "the editor asks the host for the remembered mode");
    Check(host.applyCalls == appliesBeforeOpen, "opening a comparison source writes no curve data");

    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareSameGroup, "the remembered mode is inherited on open");
    Check(info[1] == 0, "the initial main channel id selects the main curve");
    Check(info[2] == 1 && info[3] == 1, "the same-group set holds exactly the varying curve of that group");
    Check(IsWindowEnabled(GetDlgItem(window, kIdAddKey)) != FALSE, "the main curve stays editable");
    Check(GetText(window, kIdValueEdit).empty() == false, "the key fields still show the main curve");
    // An automatic mode owns the ticks: only the curves it draws may be ticked,
    // even though the host handed every channel over as selected.
    Check(ChannelVisible(window, 0) == 1, "the main curve is ticked in an automatic mode");
    Check(ChannelVisible(window, 1) == 1, "the curve the mode draws is ticked");
    Check(ChannelVisible(window, 2) == 0 && ChannelVisible(window, 3) == 0 && ChannelVisible(window, 4) == 0 &&
              ChannelVisible(window, 5) == 0,
          "nothing outside the automatic set stays ticked");
    CheckTicksMatchCanvas(window, 6, "same-group mode");
    Check(host.data[3].selected, "the host data itself is not rewritten by the mask");
    // A re-read hands the host flags back; the automatic mode restores its mask
    // instead of listing curves it does not draw.
    SendMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    Check(ChannelVisible(window, 3) == 0 && ChannelVisible(window, 1) == 1,
          "a re-read does not bring foreign ticks back into an automatic mode");
    CheckTicksMatchCanvas(window, 6, "same-group mode after a re-read");

    // Four groups: every registered and varying curve of groups 0..3.
    const int appliesBeforeSwitch = host.applyCalls;
    Check(IsWindowEnabled(GetDlgItem(window, kIdCompareAllGroups)) != FALSE,
          "the four-group button is enabled for a comparison source");
    ClickCompareMode(window, ce::kCompareAllGroups);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups, "one click on the four-group button picks that mode");
    Check(CheckedCompareMode(window) == ce::kCompareAllGroups, "the pressed button follows the mode");
    Check(info[2] == 2 && info[3] == 1 && info[4] == 2, "the four-group set holds both varying group curves");
    Check(info[1] == 0, "switching the mode keeps the main curve");
    Check(host.modeWrites == 1 && host.storedMode == ce::kCompareAllGroups,
          "the mode change reaches the host write callback");
    Check(host.applyCalls == appliesBeforeSwitch, "switching the mode writes no curve data");
    Check(host.data[1].track.keys.size() == 2 && host.data[1].track.keys[1].value == 0.5 &&
              host.data[3].track.keys.size() == 2 && host.data[3].track.keys[1].value == 0.5 &&
              host.data[5].track.keys.empty(),
          "the comparison curves keep their data through a mode switch");
    Check(ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 1, "the four-group mask ticks both curves");
    Check(ChannelVisible(window, 3) == 0 && ChannelVisible(window, 4) == 0 && ChannelVisible(window, 5) == 0,
          "the constant, ungrouped and empty curves stay unticked");
    CheckTicksMatchCanvas(window, 6, "four-group mode");

    // A manual show/hide leaves the automatic mode: the free display carries the
    // set that is on screen, and the click changes only the row it names.
    ClickChannelOnce(window, 3);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareFree, "a manual show/hide moves the display to the free segment");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "the free segment shows as selected");
    Check(ChannelVisible(window, 3) == 1, "one click shows a curve the automatic mode had hidden");
    Check(info[2] == 3 && info[3] == 1 && info[4] == 2 && info[5] == 3,
          "the drawn set gained exactly the clicked curve");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 1,
          "the curves that were visible stay visible");
    Check(ChannelVisible(window, 4) == 0 && ChannelVisible(window, 5) == 0, "the other hidden curves stay hidden");
    Check(ActiveChannelIndex(window) == 0, "a manual show/hide keeps the active curve");
    CheckTicksMatchCanvas(window, 6, "free mode after one click");
    ClickChannelOnce(window, 3);
    Check(ChannelVisible(window, 3) == 0, "a second click hides it again");
    CheckTicksMatchCanvas(window, 6, "free mode after the second click");

    // Pressing an automatic segment again regenerates its set.
    ClickCompareMode(window, ce::kCompareAllGroups);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups && info[2] == 2 && info[3] == 1 && info[4] == 2,
          "pressing the automatic segment regenerates its set");
    Check(ChannelVisible(window, 3) == 0, "the mask drops what the user had shown by hand");
    Check(ActiveChannelIndex(window) == 0, "the automatic segment keeps the main expression");
    CheckTicksMatchCanvas(window, 6, "regenerated four-group mode");

    // A host that refuses to store the mode keeps mode, flags and active curve,
    // so a manual change never half-applies.
    host.failModeWrite = true;
    const int appliesBeforeRefusal = host.applyCalls;
    ClickChannelOnce(window, 3);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups, "a refused mode write keeps the automatic mode");
    Check(CheckedCompareMode(window) == ce::kCompareAllGroups, "the segments stay on the automatic mode");
    Check(ChannelVisible(window, 3) == 0, "a refused mode write keeps the display flags");
    Check(ActiveChannelIndex(window) == 0, "a refused mode write keeps the active curve");
    Check(GetText(window, kIdStatus).empty() == false, "a refused mode write reports a status line");
    Check(host.applyCalls == appliesBeforeRefusal, "a refused mode write touches no curve data");
    CheckTicksMatchCanvas(window, 6, "refused manual change");
    host.failModeWrite = false;

    // The free mode shows exactly what the channel list has ticked.
    ce::CloseOwned(nullptr);
    host.data[1].selected = true;
    host.data[2].selected = false;
    host.data[3].selected = false;
    host.data[4].selected = false;
    host.data[5].selected = false;
    host.storedMode = ce::kCompareFree;
    ce::Session freeSession = MakeSession(host);
    HWND freeWindow = static_cast<HWND>(ce::Open(nullptr, freeSession, false));
    Check(freeWindow != nullptr, "the editor reopens for the free mode");
    ReadCompare(freeWindow, info);
    Check(info[0] == ce::kCompareFree, "the free mode is inherited from the host");
    Check(CheckedCompareMode(freeWindow) == ce::kCompareFree, "the free button shows the inherited mode");
    Check(info[2] == 1 && info[3] == 1, "the free mode shows exactly the ticked curve");
    Check(info[1] == 0, "the free mode keeps the main curve");
    Check(ChannelVisible(freeWindow, 2) == 0 && ChannelVisible(freeWindow, 3) == 0,
          "the free mode leaves the unticked rows unticked");
    CheckTicksMatchCanvas(freeWindow, 6, "inherited free mode");

    // A curve without keys has nothing to draw: ticking it would promise a
    // picture that never appears, so it stays unticked and says why.
    ClickChannelOnce(freeWindow, 5);
    Check(ChannelVisible(freeWindow, 5) == 0, "a curve without keys cannot be ticked into the display");
    Check(GetText(freeWindow, kIdStatus).empty() == false, "the refused tick reports a status line");
    CheckTicksMatchCanvas(freeWindow, 6, "refused tick of a curve without keys");

    // There is no off button: a comparison source always shows one of the three
    // modes, and pressing the pressed button changes nothing.
    const int writesBefore = host.modeWrites;
    ClickCompareMode(freeWindow, ce::kCompareFree);
    ReadCompare(freeWindow, info);
    Check(host.modeWrites == writesBefore, "pressing the current mode writes nothing");
    Check(info[0] == ce::kCompareFree && info[2] == 1, "the set is unchanged by pressing the current mode");
    Check(CheckedCompareMode(freeWindow) == ce::kCompareFree, "exactly one mode button stays pressed");
    Check(info[1] == 0, "the main curve is still active");
    // Free is the segment manual work enters, so picking it never restores the
    // main expression: the curve the user activated stays active.
    DoubleClickChannel(freeWindow, 2);
    Check(ActiveChannelIndex(freeWindow) == 2, "a double click activates the row it names");
    Check(CheckedCompareMode(freeWindow) == ce::kCompareFree, "the double click stays on the free segment");
    ClickCompareMode(freeWindow, ce::kCompareFree);
    Check(ActiveChannelIndex(freeWindow) == 2, "picking the free segment keeps the curve the user activated");
    Check(ChannelVisible(freeWindow, 1) == 1 && ChannelVisible(freeWindow, 2) == 1,
          "picking the free segment changes no display flag");
    CheckTicksMatchCanvas(freeWindow, 6, "free segment after a double click");
    ce::CloseOwned(nullptr);
    Note("comparison modes, main-curve stability and host memory");
}

// The range filter: the span, its source, the ticked curves and the readout.
void ComparisonRangeFilter() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareAllGroups;
    host.initialMain = "main";
    host.data.push_back(MakeGroupChannel("main", L"Main", 0,
                                         {MakeKey(0, 0.0), MakeKey(10, 0.0), MakeKey(20, 0.0)}));
    // Equal endpoints with a Bezier bump in between: flat at every integer
    // frame only for the first ten frames, so the span decides.
    host.data.push_back(MakeGroupChannel("bump", L"Bump", 0,
                                         {MakeShapeKey(0, 0.0, 0.0, 2.0), MakeShapeKey(20, 0.0, -2.0, 0.0)}));
    // One value everywhere.
    host.data.push_back(MakeGroupChannel("flat", L"Flat", 1,
                                         {MakeShapeKey(0, 0.5, 0.0, 0.0), MakeShapeKey(20, 0.5, 0.0, 0.0)}));
    // Varies at the front, constant afterwards.
    host.data.push_back(MakeGroupChannel("early", L"Early", 1,
                                         {MakeShapeKey(0, 0.0, 0.0, 0.0), MakeShapeKey(5, 1.0, 0.0, 0.0)}));
    // Registered in a group but without keys: nothing to draw, so the filter
    // must never tick it.
    host.data.push_back(MakeGroupChannel("ghost", L"Ghost", 1, {}));

    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(StyleVisible(window, kIdCompareFilter), "the filter button is shown");
    Check(CheckedCompareMode(window) == ce::kCompareAllGroups, "the filter starts from the stored mode");

    // Two selected keys of the main curve: frames 10 and 20.
    ClickKey(window, 1);
    ClickKeyAdditive(window, 2);
    double span[3] = {};
    Check(ReadCompareSpan(window, span), "a span is available once two keys are selected");
    Check(span[2] == 1, "the span comes from the selected keys first");
    Check(span[0] == 10.0 && span[1] == 20.0, "the span is the smallest and largest selected frame");

    // One click runs the filter once: it ticks the curves that really change in
    // the span and hands the result to the free display.
    const int writesBefore = host.modeWrites;
    ClickButton(window, kIdCompareFilter);
    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareFree, "the filter hands its result to the free display");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "the free button is pressed after a filter run");
    Check(host.modeWrites == writesBefore + 1, "the mode the filter picks reaches the host once");
    Check(host.applyCalls == 0, "a filter run writes no curve data");
    Check(info[1] == 0, "a filter run keeps the active curve");
    Check(info[2] == 1 && info[3] == 1, "only the curve that varies inside the span is shown");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1,
          "the main curve and the varying curve keep their tick");
    Check(ChannelVisible(window, 2) == 0 && ChannelVisible(window, 3) == 0,
          "the flat curve and the curve that is flat inside [10,20] are hidden");
    Check(ChannelVisible(window, 4) == 0, "a curve without keys is never ticked by the filter");
    CheckTicksMatchCanvas(window, 5, "after a filter run");
    const std::wstring filtered = GetText(window, kIdStatus);
    Check(filtered.find(L"10") != std::wstring::npos && filtered.find(L"20") != std::wstring::npos,
          "the status line names the span that was used");
    Check(filtered.find(L"\u6765\u81EA\u9009\u4E2D\u5173\u952E\u5E27") != std::wstring::npos,
          "the status line names the selected keys as the source of the span");

    // A second run with the range fields as the source: the early curve varies
    // inside [0,5] and the flat one still does not.
    ClickKey(window, 0);
    SetText(window, kIdBakeFirstEdit, L"0");
    SetText(window, kIdBakeLastEdit, L"5");
    Check(ReadCompareSpan(window, span), "the range fields provide a span without a key selection");
    Check(span[2] == 2, "the span falls back to the range fields");
    Check(span[0] == 0.0 && span[1] == 5.0, "the range fields are used as written");
    ClickButton(window, kIdCompareFilter);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareFree && info[2] == 2 && info[3] == 1 && info[4] == 3,
          "both curves that vary inside the new span are shown");
    Check(ChannelVisible(window, 1) == 1 && ChannelVisible(window, 3) == 1, "the two varying curves are ticked");
    Check(ChannelVisible(window, 2) == 0, "the flat curve stays hidden");
    const std::wstring second = GetText(window, kIdStatus);
    Check(second.find(L"5") != std::wstring::npos, "the status line names the second span too");
    Check(ActiveChannelIndex(window) == 0, "filter runs never move the active curve");

    // A run without a usable span reports the problem and changes nothing.
    SetText(window, kIdBakeFirstEdit, L"");
    SetText(window, kIdBakeLastEdit, L"");
    ClickButton(window, kIdCompareFilter);
    const std::wstring refused = GetText(window, kIdStatus);
    Check(refused.empty() == false, "a run without a span reports a status line");
    Check(ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 0,
          "a refused run keeps the ticks it found");
    ce::CloseOwned(nullptr);
    Note("one-shot range filter: span, source, ticked curves and preserved activity");
}

// The background curves reach the canvas and leave again.
void ComparisonPainting() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareFree;
    // Fixed axis colours distinguish the green main curve from the translucent
    // red comparison curve without depending on hashed channel IDs or the old
    // opaque-grey rendering. The surrounding grid is neutral or blue.
    host.initialMain = "y";
    host.data.push_back(MakeGroupChannel("y", L"Y", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("x", L"X", 0, {MakeKey(0, 0.0), MakeKey(10, 0.2)}));
    host.data[1].selected = false;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    const auto mainOnly = RenderPixels(canvas);
    Check(!mainOnly.empty() && ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 0,
          "the comparison paint fixture starts with only the green main curve");
    ClickCompareMode(window, ce::kCompareSameGroup);
    const auto visible = RenderPixels(canvas);
    Check(visible.size() == mainOnly.size() && visible != mainOnly &&
              CountRedCurvePixels(visible) > CountRedCurvePixels(mainOnly),
          "the background comparison curve is painted in its own colour");

    // A mode button repaints without losing the background curve.
    ClickCompareMode(window, ce::kCompareFree);
    Check(RenderPixels(canvas) == visible, "the free mode still paints the same main and ticked comparison curves");
    ClickCompareMode(window, ce::kCompareSameGroup);
    Check(RenderPixels(canvas) == visible, "the same-group mode paints the same curves again");

    // Hiding it in the list takes it off the canvas; the main curve stays.
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 0 && ActiveChannelIndex(window) == 0,
          "hiding the comparison curve preserves the main curve's active channel");
    const auto hidden = RenderPixels(canvas);
    Check(hidden == mainOnly && hidden != visible,
          "hiding the curve restores exactly the main-only canvas");
    Check(RenderInk(canvas) > 100, "the main curve is still painted");
    ClickChannelOnce(window, 1);
    Check(RenderPixels(canvas) == visible, "showing it again restores the complete original curve rendering");
    ce::CloseOwned(nullptr);
    Note("comparison curves paint behind the main curve through WM_PRINTCLIENT");
}

// A source without comparison metadata keeps the old control set and the old
// behaviour, in all three languages.
void ComparisonNoRegression() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    for (int id : {kIdCompareLabel, kIdCompareSameGroup, kIdCompareAllGroups, kIdCompareFree, kIdCompareFilter})
        Check(StyleVisible(window, id) == false,
              "a source without comparison metadata keeps the comparison row hidden");
    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareOff && info[2] == 0 && info[1] == 0,
          "a source without comparison metadata stays in the plain single-curve view");
    Check(host.modeWrites == 0 && host.modeReads == 0, "no comparison callback is used for a plain source");

    const int appliesBefore = host.applyCalls;
    SetText(window, kIdFrameEdit, L"20");
    SetText(window, kIdValueEdit, L"1.5");
    Click(window, kIdAddKey);
    Check(host.applyCalls == appliesBefore + 1, "editing still reaches a source without comparison metadata");
    Check(KeyCount(window) == 3, "the added key arrived");
    Click(window, kIdUndo);
    Check(KeyCount(window) == 2, "undo still works without comparison");

    for (unsigned language = 0; language < 3; ++language) {
        ce::SetLanguage(nullptr, language);
        Check(StyleVisible(window, kIdCompareSameGroup) == false,
              "the comparison row stays hidden in every language");
    }
    Check(RenderInk(GetDlgItem(window, kIdCanvas)) > 100, "the canvas paints the plain single curve");
    ce::CloseOwned(nullptr);
    Note("a source without comparison metadata is unchanged");
}

// The comparison row in the three languages and at the minimum window size.
void ComparisonLayout() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareAllGroups;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 1, {MakeKey(0, 0.0), MakeKey(10, 0.5)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(StyleVisible(window, kIdCompareSameGroup), "the comparison row appears for a comparison source");
    const wchar_t *names[3] = {L"ja", L"en", L"zh"};
    for (unsigned language = 0; language < 3; ++language) {
        ce::SetLanguage(nullptr, language);
        Check(GetText(window, kIdCompareFilter).empty() == false, "the filter button is translated");
        Check(GetText(window, kIdCompareSameGroup).empty() == false, "the same-group button is translated");
        Check(GetText(window, kIdCompareAllGroups).empty() == false, "the four-group button is translated");
        Check(GetText(window, kIdCompareFree).empty() == false, "the free button is translated");
        Check(CheckedCompareMode(window) == ce::kCompareAllGroups,
              "a language switch keeps the pressed comparison mode");
        CheckLayout(window, names[language]);
    }

    // The three segments are one control: equal width, touching, in the visual
    // order same group - free - four groups, while the stored values stay
    // 1 same group, 2 four groups and 3 free.
    RECT left = {}, middle = {}, right = {};
    Check(GetWindowRect(GetDlgItem(window, kIdCompareSameGroup), &left) != FALSE, "the left segment exists");
    Check(GetWindowRect(GetDlgItem(window, kIdCompareFree), &middle) != FALSE, "the middle segment exists");
    Check(GetWindowRect(GetDlgItem(window, kIdCompareAllGroups), &right) != FALSE, "the right segment exists");
    Check(left.left < middle.left && middle.left < right.left,
          "the visual order is same group, free, four groups");
    Check(left.right == middle.left && middle.right == right.left,
          "the three segments touch, so they read as one segmented control");
    Check((left.right - left.left) == (middle.right - middle.left) &&
              (middle.right - middle.left) == (right.right - right.left),
          "the three segments share one width");
    Check(left.top == middle.top && middle.top == right.top && left.bottom == middle.bottom &&
              middle.bottom == right.bottom,
          "the three segments share one row and one height");
    Check(static_cast<int>(SendMessageW(GetDlgItem(window, kIdCompareSameGroup), BM_GETCHECK, 0, 0)) ==
                  BST_UNCHECKED &&
              static_cast<int>(SendMessageW(GetDlgItem(window, kIdCompareFree), BM_GETCHECK, 0, 0)) ==
                  BST_UNCHECKED &&
              static_cast<int>(SendMessageW(GetDlgItem(window, kIdCompareAllGroups), BM_GETCHECK, 0, 0)) ==
                  BST_CHECKED,
          "only the current segment shows the selected state");
    ClickCompareMode(window, ce::kCompareFree);
    Check(CheckedCompareMode(window) == ce::kCompareFree, "one click selects the middle segment");
    Check(ActiveChannelIndex(window) == 0, "a segment click keeps the main curve");
    ClickCompareMode(window, ce::kCompareSameGroup);
    Check(CheckedCompareMode(window) == ce::kCompareSameGroup, "one click selects the left segment");
    MINMAXINFO limits = {};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    SetWindowPos(window, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(limits.ptMinTrackSize.x, limits.ptMinTrackSize.y));
    CheckLayout(window, L"minimum comparison");
    ce::CloseOwned(nullptr);
    Note("comparison row in three languages and at the minimum window size");
}

// ---------------------------------------------------------------------------
// 24. The channel tree separates visibility checkboxes from activation labels;
// keyboard navigation and activation must follow the actual clicked leaf.
// ---------------------------------------------------------------------------
void ChannelListInteraction() {
    HostSource host;
    host.data.push_back(MakeChannel("a", L"A", {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeChannel("b", L"B", {MakeKey(0, 0.0), MakeKey(10, 2.0), MakeKey(20, 3.0)}));
    host.data.push_back(MakeChannel("c", L"C", {MakeKey(0, 0.0), MakeKey(10, 4.0), MakeKey(20, 5.0),
                                                MakeKey(30, 6.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND list = GetDlgItem(window, kIdChannelList);

    Check(ActiveChannelIndex(window) == 0, "the first channel starts active");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 1,
          "every curve starts shown");
    Check(ChannelRowText(window, 0).find(L"\u25CF") == 0, "the active row carries the active marker");
    Check(ChannelRowText(window, 1).find(L"\u25CF") == std::wstring::npos,
          "an inactive row carries no active marker");

    ClickChannelLabel(window, 1);
    Check(ChannelCaretIndex(window) == 1 && ActiveChannelIndex(window) == 1 && KeyCount(window) == 3,
          "a label click activates the leaf and moves the keyboard caret to it");
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    Check(ChannelVisible(window, 1) == 0 && ChannelVisible(window, 0) == 1 && ChannelVisible(window, 2) == 1 &&
              ActiveChannelIndex(window) == 1,
          "Space after a label click toggles only that leaf without changing the active channel");
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    Check(ChannelCaretIndex(window) == 1 && ActiveChannelIndex(window) == 1 && ChannelVisible(window, 1) == 1,
          "Enter after a label click reactivates and shows the same leaf");
    ClickChannelLabel(window, 0);
    Check(ChannelCaretIndex(window) == 0 && ActiveChannelIndex(window) == 0 && KeyCount(window) == 2 &&
              host.applyCalls == 0,
          "clicking another label moves both caret and active channel without writing authored keys");

    // One checkbox click: show or hide only, never a change of the active curve.
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 0, "a single click hides the clicked curve");
    Check(ActiveChannelIndex(window) == 0, "a single click never changes the active curve");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 2) == 1,
          "a single click leaves the other curves alone");
    Check(KeyCount(window) == 2, "the key fields still describe the first curve");
    Check(ChannelCaretIndex(window) == 1,
          "the caret follows the clicked row");
    // Plain sources still overlay their other visible tracks, but never gain
    // the expression comparison modes or invoke their persistence callbacks.
    int plainInfo[12] = {};
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(plainInfo));
    Check(plainInfo[0] == ce::kCompareOff && host.modeReads == 0 && host.modeWrites == 0 &&
              !StyleVisible(window, kIdCompareSameGroup) && !StyleVisible(window, kIdCompareAllGroups) &&
              !StyleVisible(window, kIdCompareFree),
          "a plain source never gains a comparison mode from the list");
    Check(plainInfo[2] == 1 && plainInfo[3] == 2,
          "a plain source continues to draw exactly the untouched visible background channel");
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 1, "a second single click shows the curve again");
    Check(ActiveChannelIndex(window) == 0, "the active curve is still the first one");

    // A label double click includes a single click first; both activate the
    // same leaf without toggling its visibility checkbox.
    DoubleClickChannel(window, 2);
    Check(ActiveChannelIndex(window) == 2, "a double click makes that curve the active one");
    Check(ChannelVisible(window, 2) == 1, "a double click leaves the curve shown after its first click");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1,
          "a double click leaves the other curves alone");
    Check(KeyCount(window) == 4, "the key fields follow the newly active curve");
    Check(ChannelRowText(window, 2).find(L"\u25CF") == 0, "the active marker moved to the new row");
    Check(ChannelRowText(window, 0).find(L"\u25CF") == std::wstring::npos, "the old row lost the marker");

    // Activating a previously hidden leaf makes it visible again.
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 0, "the row is hidden before the double click");
    DoubleClickChannel(window, 1);
    Check(ChannelVisible(window, 1) == 1, "a double click on a hidden row never leaves it hidden");
    Check(ActiveChannelIndex(window) == 1, "the double click activated the hidden row");
    Check(ChannelVisible(window, 2) == 1, "the previously active curve is still shown");

    // Keyboard parity: arrows move the caret only, Space toggles, Enter
    // activates.
    TreeView_SelectItem(list, ChannelItem(window, 0));
    SendMessageW(list, WM_KEYDOWN, VK_DOWN, 0);
    Check(ChannelCaretIndex(window) == 1, "the down arrow moves the caret");
    Check(ActiveChannelIndex(window) == 1, "the down arrow does not change the active curve");
    const int beforeSpace = ActiveChannelIndex(window);
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    Check(ChannelVisible(window, 1) == 0, "Space hides or shows the caret row");
    Check(ActiveChannelIndex(window) == beforeSpace, "Space never changes the active curve");
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    Check(ChannelVisible(window, 1) == 1, "a second Space shows it again");
    SendMessageW(list, WM_KEYDOWN, VK_UP, 0);
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    Check(ActiveChannelIndex(window) == 0, "Enter activates the caret row");
    Check(KeyCount(window) == 2, "the key fields follow the keyboard activation");
    SendMessageW(list, WM_KEYDOWN, VK_END, 0);
    Check(ChannelCaretIndex(window) == 2, "End reaches the last row");
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    Check(ActiveChannelIndex(window) == 2 && ChannelVisible(window, 2) == 1,
          "Enter on a hidden row activates and shows it");
    Check(KeyCount(window) == 4, "the active curve really changed by keyboard");
    ClickChannelOnce(window, 0);
    Check(ChannelVisible(window, 0) == 0 && ActiveChannelIndex(window) == 2,
          "a click on another row only hides that row");
    Check(RenderInk(GetDlgItem(window, kIdCanvas)) > 20, "a hidden active curve still paints its notice");
    ce::CloseOwned(nullptr);
    Note("channel list single click, double click and keyboard reachability");
}

// ---------------------------------------------------------------------------
// 25. Hover help: one line per control, cleared on leave, never over the
// persistent status or error text.
// ---------------------------------------------------------------------------
void HoverHints() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 1, {MakeKey(0, 0.0), MakeKey(10, 0.5)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    HWND mmd = GetDlgItem(window, kIdMmdPanel);
    Check(HoverHint(window) == 0 && GetText(window, kIdHoverHint).empty(), "the hint line starts empty");

    // The three canvas regions explain themselves separately.
    const POINT plot = AskPoint(window, kHookPointForFrame, 5);
    SendMessageW(canvas, WM_MOUSEMOVE, 0, MAKELPARAM(plot.x, plot.y));
    Check(HoverHint(window) == 0, "the plot has no redundant gesture paragraph");
    Check(GetText(window, kIdHoverHint).empty(), "the plot leaves the hint line empty");
    const POINT timeBar = AskBarPoint(window, 1, 0);
    SendMessageW(canvas, WM_MOUSEMOVE, 0, MAKELPARAM(timeBar.x, timeBar.y));
    Check(HoverHint(window) == 9, "the horizontal bar explains panning and zooming in time");
    const POINT valueBar = AskBarPoint(window, 2, 0);
    SendMessageW(canvas, WM_MOUSEMOVE, 0, MAKELPARAM(valueBar.x, valueBar.y));
    Check(HoverHint(window) == 10, "the vertical bar explains panning and zooming in value");
    SendMessageW(canvas, WM_MOUSELEAVE, 0, 0);
    Check(HoverHint(window) == 0, "leaving the canvas clears the hint");
    Check(GetText(window, kIdHoverHint).empty(), "the cleared hint line is empty");

    SendMessageW(mmd, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    Check(HoverHint(window) == 8, "the MMD panel explains its handles");
    SendMessageW(mmd, WM_MOUSELEAVE, 0, 0);
    Check(HoverHint(window) == 0, "leaving the MMD panel clears the hint");

    // Child controls answer through their subclass.
    const struct {
        int id;
        int hint;
        const char *text;
    } targets[] = {
        {kIdChannelList, 1, "the channel list explains click, double click and the keys"},
        {kIdCompareSameGroup, 2, "the same-group button explains the modes"},
        {kIdCompareAllGroups, 2, "the four-group button explains the modes"},
        {kIdCompareFree, 2, "the free button explains the modes"},
        {kIdCompareFilter, 3, "the filter button explains the span and the ticks"},
        {kIdAddKey, 4, "the add button explains itself"},
        {kIdDeleteKey, 5, "the delete button explains itself"},
        {kIdApplyKey, 6, "the apply button explains itself"},
    };
    for (const auto &target : targets) {
        HoverControl(window, target.id);
        Check(HoverHint(window) == target.hint, target.text);
        Check(GetText(window, kIdHoverHint).empty() == false, "a hovered control always shows a sentence");
        LeaveControl(window, target.id);
        Check(HoverHint(window) == 0, "leaving the control clears the hint");
    }

    // The hint is retranslated while it is on screen.
    HoverControl(window, kIdChannelList);
    const std::wstring chinese = GetText(window, kIdHoverHint);
    ce::SetLanguage(nullptr, 1);
    const std::wstring english = GetText(window, kIdHoverHint);
    Check(english.empty() == false && english != chinese, "the hint follows a language change");
    ce::SetLanguage(nullptr, 0);
    Check(GetText(window, kIdHoverHint) != english, "the hint follows the second language change too");
    ce::SetLanguage(nullptr, 2);
    Check(GetText(window, kIdHoverHint) == chinese, "the Chinese hint comes back unchanged");
    LeaveControl(window, kIdChannelList);

    // A hover never touches the persistent status line, so an error stays
    // readable; leaving restores the hint line only.
    const LRESULT focusBefore = FocusId(window);
    SetText(window, kIdFrameEdit, L"not a frame");
    Click(window, kIdAddKey);
    const std::wstring error = GetText(window, kIdStatus);
    Check(error.empty() == false, "the failed action left a status line");
    HoverControl(window, kIdChannelList);
    Check(HoverHint(window) == 1, "the hint is shown while the error line is on screen");
    Check(GetText(window, kIdStatus) == error, "a hover never overwrites the persistent status line");
    Check(FocusId(window) == focusBefore, "a hover never moves the keyboard focus");
    LeaveControl(window, kIdChannelList);
    Check(HoverHint(window) == 0, "leaving clears the hint again");
    Check(GetText(window, kIdStatus) == error, "the error line survives the leave as well");
    ce::CloseOwned(nullptr);
    Note("hover hints per control, cleared on leave and never over the status line");
}

// The automatic segments are the documented way back to the main expression:
// same group and four groups restore the curve Source::initialMainChannel names,
// show it and write their set into the list.  The free segment is the one manual
// work enters, so picking it leaves the active curve and the display flags alone.
void ComparisonModeRestoresMainCurve() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareSameGroup;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 0, {MakeKey(0, 0.0), MakeKey(10, 2.0), MakeKey(20, 3.0)}));
    host.data.push_back(MakeGroupChannel("m_c", L"C", 1, {MakeKey(0, 0.0), MakeKey(10, 4.0), MakeKey(20, 5.0),
                                                          MakeKey(30, 6.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(ActiveChannelIndex(window) == 0, "the window opens on the main curve");
    CheckTicksMatchCanvas(window, 3, "same-group entry");

    DoubleClickChannel(window, 2);
    Check(ActiveChannelIndex(window) == 2, "a double click moves the active curve away from the main one");
    Check(KeyCount(window) == 4, "the key fields follow the curve the user picked");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "the double click stays on the free segment");
    Check(ChannelVisible(window, 2) == 1, "the double click shows the curve it activated");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1,
          "the double click leaves the other visible curves alone");
    CheckTicksMatchCanvas(window, 3, "after a double click");

    ClickCompareMode(window, ce::kCompareAllGroups);
    Check(ActiveChannelIndex(window) == 0, "pressing an automatic segment restores the main curve");
    Check(KeyCount(window) == 2, "the key fields follow the restored main curve");
    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups && info[2] == 2 && info[3] == 1 && info[4] == 2,
          "the four-group set is computed around the restored main curve");
    CheckTicksMatchCanvas(window, 3, "restored four-group mode");

    ClickCompareMode(window, ce::kCompareSameGroup);
    ReadCompare(window, info);
    Check(info[2] == 1 && info[3] == 1, "the same-group set follows the restored main curve's group");
    CheckTicksMatchCanvas(window, 3, "restored same-group mode");

    // The main curve can be hidden by hand; an automatic segment shows it again.
    ClickChannelOnce(window, 0);
    Check(ChannelVisible(window, 0) == 0, "the main curve can be hidden from the list");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "hiding by hand moves the display to free");
    ClickCompareMode(window, ce::kCompareFree);
    Check(ChannelVisible(window, 0) == 0, "picking the free segment does not force the main curve back");
    ClickCompareMode(window, ce::kCompareSameGroup);
    Check(ChannelVisible(window, 0) == 1, "an automatic segment shows the main curve again");
    Check(ActiveChannelIndex(window) == 0, "the automatic segment keeps the main curve active");
    CheckTicksMatchCanvas(window, 3, "after the automatic segment restored the main curve");
    Check(HoverHint(window) == 0, "no hint is left over from the interactions");
    ce::CloseOwned(nullptr);
    Note("automatic segments restore and show the main curve, free leaves the user's choice");
}

void AdjacentHandleLinking() {
    HostSource host;
    for(const char* id : {"rotation.x","rotation.y","rotation.z"}) {
        auto channel=MakeChannel(id,L"Rotation",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2),MakeKey(30,3)});
        channel.interpolationGroup="rotation";
        std::string error;
        for(std::size_t end=1;end<4;++end)Check(ce::SetChannelControls(channel,end,{20,20,107,107},error),"initialize linked intervals");
        host.data.push_back(channel);
    }
    bool automatic=true;
    auto session=MakeSession(host);session.sources[0].linkAdjacentControls=[&]{return automatic;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,session,false));ClickKey(window,2);
    HWND panel=GetDlgItem(window,kIdMmdPanel);
    const auto read=[&](int end){std::array<double,4> controls{};Check(ce::ReadChannelControls(host.data[0],end,controls),"read linked controls");return controls;};
    const auto drag=[&](int handle,int x,int y){
        double current[4]{};Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(current))!=0,"linked panel is editable");
        const auto getDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
        UINT dpi=getDpi?getDpi(panel):0;
        if(!dpi){HDC dc=GetDC(panel);dpi=dc?UINT(GetDeviceCaps(dc,LOGPIXELSX)):96;if(dc)ReleaseDC(panel,dc);}
        RECT client{};GetClientRect(panel,&client);const int margin=MulDiv(6,dpi?dpi:96,96);
        const int side=std::min(int(client.right),int(client.bottom))-2*margin;
        const auto point=[&](double a,double b){return MAKELPARAM(margin+int(std::lround(a*side/127.0)),margin+side-int(std::lround(b*side/127.0)));};
        SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point(current[handle*2],current[handle*2+1]));
        SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,point(x,y));SendMessageW(panel,WM_LBUTTONUP,0,point(x,y));
    };
    drag(1,80,110);auto edited=read(2),neighbor=read(3);
    Check(std::fabs(neighbor[0]-(127-edited[2]))<0.001&&std::fabs(neighbor[1]-(127-edited[3]))<0.001,"end handle links the next interval start");
    Check(read(1)==std::array<double,4>{20,20,107,107},"linking does not cascade to the previous interval");
    for(const auto& channel:host.data){std::array<double,4> controls{};Check(ce::ReadChannelControls(channel,3,controls)&&controls==neighbor,"shared rotation axes link together");}
    Click(window,kIdUndo);Check(std::fabs(read(3)[0]-20)<0.001,"one undo restores the linked neighbor");
    Click(window,kIdRedo);Check(std::fabs(read(3)[0]-neighbor[0])<0.001,"redo restores the linked edit");
    automatic=false;const auto previous=read(1);drag(0,40,60);Check(read(1)==previous,"unchecked auto leaves the previous interval intact");
    automatic=true;drag(0,45,70);edited=read(2);neighbor=read(1);
    Check(std::fabs(neighbor[2]-(127-edited[0]))<0.001&&std::fabs(neighbor[3]-(127-edited[1]))<0.001,"start handle links the previous interval end");
    host.failApply=true;const auto before=host.data;drag(1,70,105);Check(host.data[0].track.keys[2].inSlope==before[0].track.keys[2].inSlope,"host refusal rolls back all linked changes");
    ce::CloseOwned(nullptr);Note("automatic adjacent handles and atomic undo/redo");
}


// A full value curve owns its tangents even when the legacy MMD panel links
// adjacent interpolation handles. Exercise actual drags, history and fields.
void IndependentValueHandles() {
    for (bool outSide : {true, false}) {
        HostSource host;
        auto curve = MakeChannel("pos.x", L"Position X", {
            MakeKey(0, 0, ac::Interpolation::Bezier, ac::Tangent::Free),
            MakeKey(20, 5, ac::Interpolation::Bezier, ac::Tangent::Free),
            MakeKey(40, 10, ac::Interpolation::Bezier, ac::Tangent::Free)});
        curve.track.keys[1].inWeight = curve.track.keys[1].outWeight = 0.3;
        curve.track.keys[1].inSlope = curve.track.keys[1].outSlope = 0;
        host.data.push_back(curve);
        auto session = MakeSession(host);
        session.sources[0].linkAdjacentControls = [] { return true; };
        HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
        ClickKey(window, 1);
        HWND canvas = GetDlgItem(window, kIdCanvas);
        const POINT key = AskPoint(window, kHookPointForKey, 1);
        const POINT handle = AskPoint(window, kHookPointForFrame, outSide ? 26 : 14);
        const POINT target = AskPoint(window, kHookPointForFrame, outSide ? 30 : 10);
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(handle.x, key.y));
        SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target.x, key.y + (outSide ? -20 : 20)));
        // PREVIEW_CHECK
        SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(target.x, key.y + (outSide ? -20 : 20)));
        const auto changed = host.data[0].track.keys[1];
        Check(host.applyCalls == 1, "value handle drag commits once");
        Check(std::fabs(outSide ? changed.outSlope : changed.inSlope) > 0.01,
              "the dragged value handle changes slope");
        Check((outSide ? changed.inSlope : changed.outSlope) == 0 &&
              (outSide ? changed.inWeight : changed.outWeight) == 0.3,
              "the opposite value handle remains independent with automatic linking on");
        Click(window, kIdUndo);
        Check(host.data[0].track.keys[1].inSlope == 0 && host.data[0].track.keys[1].outSlope == 0,
              "undo restores both handles");
        Click(window, kIdRedo);
        Check(host.data[0].track.keys[1].inSlope == changed.inSlope &&
              host.data[0].track.keys[1].outSlope == changed.outSlope,
              "redo preserves the independent handle edit");
        ClickKey(window, 1);
        SetText(window, outSide ? kIdOutSlopeEdit : kIdInSlopeEdit, L"0.2");
        Click(window, kIdApplyKey);
        Check((outSide ? host.data[0].track.keys[1].inSlope : host.data[0].track.keys[1].outSlope) == 0,
              "numeric free-handle editing also leaves the opposite handle alone");
        ce::CloseOwned(nullptr);
    }
    Note("independent value handles with legacy auto-link enabled");
}


void AlignedValueHandles() {
    for (bool outSide : {true, false}) {
        HostSource host;
        auto curve = MakeChannel("pos.x", L"Position X", {
            MakeKey(0, 0, ac::Interpolation::Bezier, ac::Tangent::Free),
            MakeKey(20, 5, ac::Interpolation::Bezier, ac::Tangent::Aligned),
            MakeKey(40, 10, ac::Interpolation::Bezier, ac::Tangent::Free)});
        curve.track.keys[1].inWeight = curve.track.keys[1].outWeight = 0.3;
        curve.track.keys[1].inSlope = curve.track.keys[1].outSlope = 0;
        host.data.push_back(curve);
        auto session = MakeSession(host);
        session.sources[0].linkAdjacentControls = [] { return true; };
        HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
        ClickKey(window, 1);
        HWND canvas = GetDlgItem(window, kIdCanvas);
        const auto key = AskPoint(window, kHookPointForKey, 1);
        const auto handle = AskPoint(window, kHookPointForFrame, outSide ? 26 : 14);
        const auto target = AskPoint(window, kHookPointForFrame, outSide ? 30 : 10);
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(handle.x, key.y));
        const LPARAM destination=MAKELPARAM(target.x,key.y+(outSide?-20:20));
        SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, destination);
        SendMessageW(canvas, WM_LBUTTONUP, 0, destination);
        const auto& changed = host.data[0].track.keys[1];
        Check(host.applyCalls == 1 && std::fabs(changed.outSlope) > 0.01,
              "an aligned value handle drag commits");
        Check(changed.tangent == ac::Tangent::Aligned && changed.inSlope == changed.outSlope,
              "explicit aligned mode still couples slopes");
        Check((outSide ? changed.inWeight : changed.outWeight) == 0.3,
              "aligned mode preserves the opposite handle length");
        ce::CloseOwned(nullptr);
    }
    Note("explicit aligned value handles");
}

void CurveModeButtons() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {
        MakeKey(0, 0, ac::Interpolation::Linear, ac::Tangent::Linear),
        MakeKey(20, 5, ac::Interpolation::Linear, ac::Tangent::Linear),
        MakeKey(40, 10, ac::Interpolation::Linear, ac::Tangent::Linear)}));
    HWND window = static_cast<HWND>(ce::Open(nullptr, MakeSession(host), false));
    ClickKey(window, 1);
    const auto choose = [&](int id, int index) {
        HWND control = GetDlgItem(window, id);
        RECT rect{}; GetClientRect(control, &rect);
        const int count = int(SendMessageW(control, CB_GETCOUNT, 0, 0));
        const int gap = id == kIdTangentCombo ? 6 : 0;
        const int x = (rect.right-gap)*(2*index+1)/(2*count) +
                      (id == kIdTangentCombo && index >= 3 ? gap : 0);
        SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, rect.bottom/2));
        SendMessageW(control, WM_LBUTTONUP, 0, MAKELPARAM(x, rect.bottom/2));
    };
    choose(kIdTangentCombo, 3); // Flat preset, after Free / Aligned / Automatic.
    Check(host.applyCalls == 1, "clicking a tangent preset commits immediately without Apply");
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Bezier &&
          host.data[0].track.keys[2].interpolation == ac::Interpolation::Bezier,
          "flat handles activate Bezier on both former linear intervals");
    Check(std::fabs(ac::Evaluate(host.data[0].track, 10).value - 2.5) > 0.1,
          "the tangent preset changes the evaluated animation, not only the button highlight");
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Free,
          "a one-shot flat preset leaves editable free handles");
    const auto flat = host.data[0].track;
    Click(window, kIdUndo);
    Check(ac::Evaluate(host.data[0].track, 10).value == 2.5, "undo restores the original linear interval");
    int unchangedApplies = host.applyCalls;
    choose(kIdInterpolationCombo, 1);
    Check(host.applyCalls == unchangedApplies,
          "reselecting Linear writes nothing");
    Click(window, kIdUndo);
    Check(host.applyCalls == unchangedApplies,
          "an unchanged interpolation choice does not append an undo entry");
    Click(window, kIdRedo);
    Check(host.applyCalls == unchangedApplies + 1 &&
          ac::Evaluate(host.data[0].track, 10).value == ac::Evaluate(flat, 10).value,
          "redo restores the evaluated preset after an unchanged interpolation choice");
    unchangedApplies = host.applyCalls;
    choose(kIdTangentCombo, 3);
    Check(host.applyCalls == unchangedApplies,
          "reapplying the same flat preset by mouse does not write unchanged curves");
    SetText(window, kIdValueEdit, L"999");
    SendMessageW(GetDlgItem(window, kIdTangentCombo), WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(GetDlgItem(window, kIdTangentCombo), WM_CHAR, L'\r', 0);
    Check(host.applyCalls == unchangedApplies && host.data[0].track.keys[1].value == 5,
          "Enter on the unchanged preset neither writes curves nor applies unrelated numeric text");

    choose(kIdInterpolationCombo, 0);
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Step &&
          ac::Evaluate(host.data[0].track, 10).value == 0,
          "step interpolation applies immediately to the interval ending at the selected key");
    Check(host.data[0].track.keys[2].interpolation == ac::Interpolation::Bezier,
          "interpolation changes only the incoming interval");
    choose(kIdInterpolationCombo, 1);
    Check(ac::Evaluate(host.data[0].track, 10).value == 2.5,
          "linear interpolation uses the chord even with flat tangent slopes");
    choose(kIdInterpolationCombo, 2);
    Check(ac::Evaluate(host.data[0].track, 10).value == ac::Evaluate(flat, 10).value,
          "Bezier interpolation restores the stored curved handles");

    choose(kIdTangentCombo, 1);
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Aligned, "aligned is a persistent handle mode");
    SetText(window, kIdOutSlopeEdit, L"0.2"); Click(window, kIdApplyKey);
    Check(host.data[0].track.keys[1].inSlope == 0.2 && host.data[0].track.keys[1].outSlope == 0.2,
          "numeric editing preserves the chosen aligned slope instead of averaging with the old field");
    choose(kIdTangentCombo, 2);
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Automatic,
          "automatic mode is applied immediately");
    SetText(window, kIdOutSlopeEdit, L"0.1"); Click(window, kIdApplyKey);
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Free &&
          host.data[0].track.keys[1].outSlope == 0.1,
          "manual slope input leaves automatic mode and keeps the typed slope");
    choose(kIdTangentCombo, 4);
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Linear &&
          host.data[0].track.keys[2].interpolation == ac::Interpolation::Linear,
          "straighten preset makes both adjacent intervals linear");
    choose(kIdTangentCombo, 5);
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Bezier &&
          host.data[0].track.keys[1].inSlope == 0 && ac::SideIsStraight(host.data[0].track,1,true),
          "ease in bends only the incoming side");
    choose(kIdTangentCombo, 6);
    Check(host.data[0].track.keys[1].outSlope == 0 && ac::SideIsStraight(host.data[0].track,1,false),
          "ease out bends only the outgoing side");
    std::vector<std::uint8_t> bytes; std::vector<ac::NamedTrack> decoded; std::string error;
    Check(ac::Encode({{"pos.x",host.data[0].track}}, bytes, error) &&
          ac::Decode(bytes.data(),bytes.size(),decoded,error), "edited modes survive the existing curve codec");
    for (int frame=0;frame<=40;++frame)
        Check(ac::Evaluate(decoded[0].track,frame).value == ac::Evaluate(host.data[0].track,frame).value,
              "curve serialization preserves the evaluated animation");
    host.failApply = true;
    const auto before = host.data[0].track;
    choose(kIdInterpolationCombo, 0);
    Check(host.data[0].track.keys[1].interpolation == before.keys[1].interpolation &&
          SendMessageW(GetDlgItem(window,kIdInterpolationCombo),CB_GETCURSEL,0,0) ==
              static_cast<int>(before.keys[1].interpolation),
          "a rejected mode write restores both data and button state");
    host.failApply = false;
    ce::CloseOwned(nullptr);
    window = static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    ClickKey(window,1);
    Check(SendMessageW(GetDlgItem(window,kIdTangentCombo),CB_GETCURSEL,0,0)==0,
          "reopening shows free mode, not a sticky one-shot preset");
    ClickKey(window,0);
    Check(!IsWindowEnabled(GetDlgItem(window,kIdInterpolationCombo)),
          "the first key has no incoming interval to edit");
    ce::CloseOwned(nullptr);
    host.data[0].track.discrete=true;
    window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false)); ClickKey(window,1);
    Check(!IsWindowEnabled(GetDlgItem(window,kIdTangentCombo)) &&
          !IsWindowEnabled(GetDlgItem(window,kIdInterpolationCombo)),
          "discrete channels disable unsupported interpolation and tangent operations");
    ce::CloseOwned(nullptr);
    Note("immediate curve modes and one-shot tangent presets, evaluated samples, history and serialization");

    // Endpoint keys have only one handle; multi-selection is one transaction.
    for (unsigned language=0; language<3; ++language) {
        host = HostSource{};
        host.data.push_back(MakeChannel("pos.x", L"Position X", {
            MakeKey(0,0,ac::Interpolation::Linear,ac::Tangent::Linear),
            MakeKey(20,5,ac::Interpolation::Linear,ac::Tangent::Linear),
            MakeKey(40,20,ac::Interpolation::Linear,ac::Tangent::Linear)}));
        window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host,language),false));
        Check(!IsWindowEnabled(GetDlgItem(window,kIdTangentCombo)), "handle controls require a selected key");
        ClickKey(window,2); choose(kIdTangentCombo,3);
        Check(host.data[0].track.keys[2].inSlope==0 &&
              std::fabs(ac::Evaluate(host.data[0].track,30).value-12.5)>0.1,
              "flat preset works on the final key's incoming interval");
        ClickKey(window,0); choose(kIdTangentCombo,3);
        Check(host.data[0].track.keys[0].outSlope==0 &&
              std::fabs(ac::Evaluate(host.data[0].track,10).value-2.5)>0.1,
              "flat preset works on the first key's outgoing interval");
        Click(window,kIdFitAll);
        const POINT a=AskPoint(window,kHookPointForKey,1), b=AskPoint(window,kHookPointForKey,2);
        HWND canvas=GetDlgItem(window,kIdCanvas);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x-20,std::min(a.y,b.y)-20));
        SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x+20,std::max(a.y,b.y)+20));
        SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(b.x+20,std::max(a.y,b.y)+20));
        int applies=host.applyCalls;
        choose(kIdInterpolationCombo,0);
        Check(host.applyCalls==applies+1 && host.data[0].track.keys[1].interpolation==ac::Interpolation::Step &&
              host.data[0].track.keys[2].interpolation==ac::Interpolation::Step,
              "multi-selection changes both incoming intervals in one transaction");
        Check(ac::Evaluate(host.data[0].track,10).value==0 && ac::Evaluate(host.data[0].track,30).value==5,
              "both selected step intervals evaluate correctly");
        Click(window,kIdUndo);
        Check(host.data[0].track.keys[1].interpolation==ac::Interpolation::Bezier &&
              host.data[0].track.keys[2].interpolation==ac::Interpolation::Bezier,
              "one undo restores the complete multi-selection operation");
        applies=host.applyCalls;
        choose(kIdInterpolationCombo,2);
        Check(host.applyCalls==applies,
              "unchanged multi-selection interpolation makes no host write");
        Click(window,kIdRedo);
        Check(host.applyCalls==applies+1 && host.data[0].track.keys[1].interpolation==ac::Interpolation::Step &&
              host.data[0].track.keys[2].interpolation==ac::Interpolation::Step,
              "unchanged multi-selection interpolation preserves the complete redo operation");
        Click(window,kIdUndo);
        choose(kIdTangentCombo,2);
        Check(host.data[0].track.keys[1].tangent==ac::Tangent::Automatic &&
              host.data[0].track.keys[2].tangent==ac::Tangent::Automatic,
              "automatic mode applies to every selected key");
        ClickKey(window,0); ClickKey(window,2);
        const double oldSlope=host.data[0].track.keys[1].outSlope;
        SetText(window,kIdValueEdit,L"30"); Click(window,kIdApplyKey);
        Check(host.data[0].track.keys[1].outSlope!=oldSlope,
              "automatic slopes follow changes to a neighbouring key");
        HWND tangent=GetDlgItem(window,kIdTangentCombo);
        SendMessageW(tangent,WM_KEYDOWN,VK_HOME,0); // Free mode.
        Check(host.data[0].track.keys[2].tangent==ac::Tangent::Free,
              "keyboard selection applies free mode");
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Aligned.
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Automatic.
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Flat preset; highlight returns to Free.
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Straighten, not Aligned.
        Check(host.data[0].track.keys[2].interpolation==ac::Interpolation::Linear,
              "keyboard focus continues across presets after their mode highlight resets");
        ClickKey(window,1); HoverControl(window,kIdTangentCombo);
        Check(HoverHint(window)==12, "the tangent strip explains mode and preset semantics");
        HoverControl(window,kIdInterpolationCombo);
        Check(HoverHint(window)==11, "the interpolation strip explains incoming interval semantics");
        ce::CloseOwned(nullptr);
    }
    Note("endpoint keys, multi-selection, neighbour updates and keyboard presets in all three languages");

    host=HostSource{};
    host.data.push_back(MakeChannel("pos.x",L"Position X",{
        MakeKey(0,0,ac::Interpolation::Linear,ac::Tangent::Free),
        MakeKey(20,5,ac::Interpolation::Linear,ac::Tangent::Free),
        MakeKey(40,10,ac::Interpolation::Step,ac::Tangent::Free)}));
    window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    ClickKey(window,1);ClickKeyAdditive(window,2);
    Check(SendMessageW(GetDlgItem(window,kIdInterpolationCombo),CB_GETCURSEL,0,0)==-1,
          "mixed interpolation selection has no misleading active button");
    choose(kIdInterpolationCombo,1);
    Check(host.applyCalls==1 && host.data[0].track.keys[1].interpolation==ac::Interpolation::Linear &&
          host.data[0].track.keys[2].interpolation==ac::Interpolation::Linear,
          "choosing a mode already used by one selected key still updates the other key once");
    Click(window,kIdUndo);
    Check(host.data[0].track.keys[1].interpolation==ac::Interpolation::Linear &&
          host.data[0].track.keys[2].interpolation==ac::Interpolation::Step,
          "one undo restores a mixed interpolation selection");
    ce::CloseOwned(nullptr);
}

}  // namespace

int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--native-sync"){
        PendingNativeChangesBeforeWrite(false);NativeSelectionBeforeDelete();NativeSelectionBeforeDelete(false);
        NativeEditsBeforeHistory();CanonicalWriteAndUndo();NativeChangesDuringMmdDrag();
        std::printf("native synchronization regressions: %d checks\n",g_checks);return 0;
    }
    if(argc>1 && std::string(argv[1])=="--curve-modes") {
        CurveModeButtons();
        std::printf("curve mode regressions: %d checks\n",g_checks); return 0;
    }
    if(argc>1 && std::string(argv[1])=="--curve-ui-compat") {
        IndependentValueHandles(); AlignedValueHandles(); AdjacentHandleLinking();
        LayoutThreeLanguages(); ComparisonLayout(); HoverHints(); TabNavigation();
        std::printf("curve UI compatibility regressions: %d checks\n",g_checks); return 0;
    }
    CurveModeButtons();
    if(argc>1 && std::string(argv[1])=="--editor-input") {
        IndependentValueHandles(); AlignedValueHandles(); AdjacentHandleLinking(); BakeSelectedChannels();
        std::printf("editor input regressions: %d checks\n",g_checks);
        return 0;
    }
    if(argc>1&&std::string(argv[1])=="auto-link"){AdjacentHandleLinking();return 0;}
    IndependentValueHandles();
    AlignedValueHandles();
    HiddenLifecycle();
    AddDeleteUndoRedo();
    DragKey();
    RubberBandMultiMove();
    TwoSources();
    RefusedApply();
    MmdPanel();
    MotionButtons();
    BakeSelectedChannels();
    DuplicateFrameSelectsExisting();
    DeleteRecomputesAutomatic();
    FrameRangeRejected();
    LocalizedSourceLabel();
    ReadKeepsSelectionAndHistory();
    PendingNativeChangesBeforeWrite();
    PendingNativeChangesBeforeWrite(false);
    NativeSelectionBeforeDelete();
    NativeSelectionBeforeDelete(false);
    NativeEditsBeforeHistory();
    CanonicalWriteAndUndo();
    NativeChangesDuringMmdDrag();
    PlayheadTimer();
    RangeBarKeyboard();
    TabNavigation();
    LauncherIcon();
    CanvasPainting();
    LayoutThreeLanguages();
    ComparisonModes();
    ComparisonRangeFilter();
    ComparisonPainting();
    ComparisonNoRegression();
    ComparisonLayout();
    ChannelListInteraction();
    HoverHints();
    ComparisonModeRestoresMainCurve();
    AdjacentHandleLinking();
    std::printf("Animation curve editor regressions passed (%d checks)\n", g_checks);
    return 0;
}
