// MAINTENANCE: regression-keep; custom PMX display groups share the original
// morph tracks while keeping independent expand/collapse and group selection.
// Runs the production window and mouse handlers hidden, using a generated PMX.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>

#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"

#include <cstdio>
#include <cwchar>
#include <fcntl.h>
#include <initializer_list>
#include <io.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

using namespace mikumikustudio;

namespace {
int checks = 0, failures = 0, dialogs = 0, compatibilityDialogs = 0;

void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

using Bytes = std::vector<unsigned char>;
template<class T> void Put(Bytes& bytes, T value) {
    const auto* first = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof value);
}
void Text(Bytes& bytes, const wchar_t* value) {
    const auto size = static_cast<unsigned>(wcslen(value) * sizeof(wchar_t));
    Put(bytes, size);
    const auto* first = reinterpret_cast<const unsigned char*>(value);
    bytes.insert(bytes.end(), first, first + size);
}
void Byte(Bytes& bytes, unsigned char value) { Put(bytes, value); }

Bytes PmxFixture() {
    Bytes bytes{'P', 'M', 'X', ' '};
    Put(bytes, 2.0f);
    for (unsigned char value : {8, 0, 0, 1, 1, 1, 1, 1, 1}) Byte(bytes, value);
    Text(bytes, L"Display groups"); Text(bytes, L"Display groups");
    Text(bytes, L""); Text(bytes, L"");
    Put(bytes, 3u);
    for (int vertex = 0; vertex < 3; ++vertex) {
        Put(bytes, vertex == 1 ? 1.0f : 0.0f);
        Put(bytes, vertex == 2 ? 1.0f : 0.0f); Put(bytes, 0.0f);
        Put(bytes, 0.0f); Put(bytes, 0.0f); Put(bytes, 1.0f);
        Put(bytes, 0.0f); Put(bytes, 0.0f);
        Byte(bytes, 0); Byte(bytes, 0); Put(bytes, 1.0f); // BDEF1, root, edge
    }
    Put(bytes, 3u); Byte(bytes, 0); Byte(bytes, 1); Byte(bytes, 2);
    Put(bytes, 0u); // textures
    Put(bytes, 1u); Text(bytes, L"Material"); Text(bytes, L"Material");
    for (int i = 0; i < 4; ++i) Put(bytes, 1.0f); // diffuse
    for (int i = 0; i < 4; ++i) Put(bytes, 0.0f); // specular and power
    for (int i = 0; i < 3; ++i) Put(bytes, 0.5f); // ambient
    Byte(bytes, 0);
    for (int i = 0; i < 5; ++i) Put(bytes, 0.0f); // edge colour and size
    Byte(bytes, 255); Byte(bytes, 255); Byte(bytes, 0);
    Byte(bytes, 1); Byte(bytes, 0); // shared toon 0
    Text(bytes, L""); Put(bytes, 3u);
    Put(bytes, 1u); Text(bytes, L"Root"); Text(bytes, L"Root");
    for (int i = 0; i < 3; ++i) Put(bytes, 0.0f);
    Byte(bytes, 255); Put(bytes, 0u); Put(bytes, static_cast<unsigned short>(0x1e));
    Put(bytes, 0.0f); Put(bytes, 1.0f); Put(bytes, 0.0f);
    Put(bytes, 3u);
    for (const auto* name : {L"Default", L"Shared", L"Mixed"}) {
        Text(bytes, name); Text(bytes, name);
        Byte(bytes, 1); Byte(bytes, 1); Put(bytes, 1u); // brow, vertex morph
        Byte(bytes, 0); Put(bytes, 0.0f); Put(bytes, 0.1f); Put(bytes, 0.0f);
    }
    Put(bytes, 4u);
    const auto frame = [&](const wchar_t* native, const wchar_t* english,
                           unsigned char special,
                           std::initializer_list<std::pair<unsigned char, unsigned char>> entries) {
        Text(bytes, native); Text(bytes, english); Byte(bytes, special);
        Put(bytes, static_cast<unsigned>(entries.size()));
        for (const auto& entry : entries) {
            Byte(bytes, entry.first); Byte(bytes, entry.second);
        }
    };
    frame(L"Root", L"Root", 1, {{0, 0}});
    frame(L"表情", L"Facial", 1, {{1, 0}, {1, 1}});
    frame(L"眉毛_自定义", L"Custom brows", 0, {{1, 1}});
    frame(L"混合_自定义", L"Mixed group", 0, {{0, 0}, {1, 2}});
    Put(bytes, 0u); Put(bytes, 0u); // rigid bodies and joints
    return bytes;
}

struct TemporaryModel {
    std::wstring path;
    bool Write() {
        wchar_t directory[MAX_PATH]{}, temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, directory) ||
            !GetTempFileNameW(directory, L"dsp", 0, temporary)) return false;
        path = std::wstring(temporary) + L".pmx";
        DeleteFileW(temporary);
        const auto bytes = PmxFixture();
        const int descriptor = _wopen(path.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL,
                                      _S_IREAD | _S_IWRITE);
        if (descriptor < 0) return false;
        const bool written = _write(descriptor, bytes.data(), static_cast<unsigned>(bytes.size())) == bytes.size();
        _close(descriptor);
        return written;
    }
    ~TemporaryModel() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

LRESULT CALLBACK HideHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) create->style &= ~WS_VISIBLE;
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK DialogHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (message.message == WM_INITDIALOG) {
            wchar_t name[32]{};
            GetClassNameW(message.hwnd, name, 32);
            if (wcscmp(name, L"#32770") == 0) {
                ++dialogs;
                wchar_t text[2048]{};
                GetDlgItemTextW(message.hwnd, 65535, text, 2048);
                if (wcsstr(text, L"The model was loaded. Some original MMD output formats"))
                    ++compatibilityDialogs;
                EndDialog(message.hwnd, IDCANCEL);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

int Header(const mdl::ModelRecord& model, unsigned group) {
    for (int row = 1; row < 200; ++row)
        if (mdl::DisplayRowType(model, row) == group) return row;
    return -1;
}
int Child(const mdl::ModelRecord& model, unsigned group, int record) {
    const int header = Header(model, group);
    if (header < 0) return -1;
    for (int row = header + 1; row < 200; ++row) {
        if (mdl::DisplayRowType(model, row) != 0) break;
        if (model.boneListRowRecord[row] == record) return row;
    }
    return -1;
}
int MorphRow(const mdl::ModelRecord& model, unsigned group, unsigned morph) {
    return Child(model, group, -1 - static_cast<int>(morph));
}
int RowY(int row) { return 0xa0 + 14 * row + 7; }
void Click(MMDApp* app, int x, int row, bool shift = false) {
    Check(row >= 0 && row < 200, "click target has a visible timeline row");
    if (row < 0 || row >= 200) return;
    RECT client{}; GetClientRect(app->MainWindow(), &client);
    Check(RowY(row) < client.bottom - 0xf8, "click target passes the production row gate");
    const auto previousShift = app->ShiftModifierState();
    app->ShiftModifierState() = shift ? 3 : 0;
    SendMessageW(app->MainWindow(), WM_LBUTTONDOWN, MK_LBUTTON | (shift ? MK_SHIFT : 0),
                 MAKELPARAM(x, RowY(row)));
    SendMessageW(app->MainWindow(), WM_LBUTTONUP, 0, MAKELPARAM(x, RowY(row)));
    app->ShiftModifierState() = previousShift;
}
void SetExpanded(MMDApp* app, mdl::ModelRecord& model, unsigned group, bool expanded) {
    auto* groups = mdl::DisplayGroups(reinterpret_cast<unsigned char*>(&model));
    if ((groups[group].flags != 0) != expanded) Click(app, 12, Header(model, group));
    Check((groups[group].flags != 0) == expanded, "margin click changes the addressed group's expansion");
}
bool Selected(const mdl::ModelRecord& model, unsigned morph) {
    bool found = false;
    for (unsigned row = 0; row < mdl::FacialFrameCount(model); ++row) {
        if (mdl::FrameTargetIndex(model.displayFrames[row]) != morph) continue;
        found = true;
        if (!model.displayFrames[row].selected) return false;
    }
    return found;
}
bool Unselected(const mdl::ModelRecord& model, unsigned morph) {
    for (unsigned row = 0; row < mdl::FacialFrameCount(model); ++row)
        if (mdl::FrameTargetIndex(model.displayFrames[row]) == morph &&
            model.displayFrames[row].selected) return false;
    return true;
}

void Exercise(MMDApp* app, unsigned char* raw) {
    auto& model = *mdl::Mdl(raw);
    auto* groups = mdl::DisplayGroups(raw);
    Check(mdl::DisplayGroupCount(model) == 5 && mdl::FacialFrameCount(model) == 4,
          "loader retains standard and custom display groups and repeated references");
    Check(text_encoding::DisplayGroupName(groups[3], false) == L"眉毛_自定义" &&
              text_encoding::DisplayGroupName(groups[4], true) == L"Mixed group",
          "custom group captions retain native Unicode and English names");
    model.boneListPos = 0;
    PostLanguageSweep(app);
    Check(Header(model, 2) >= 0 && Header(model, 3) >= 0 && Header(model, 4) >= 0,
          "all standard and custom group headers appear in the production row table");
    for (unsigned group : {2u, 3u, 4u}) SetExpanded(app, model, group, false);
    Check(MorphRow(model, 2, 0) < 0 && MorphRow(model, 3, 1) < 0 && MorphRow(model, 4, 2) < 0,
          "collapsed groups hide their child labels");
    SetExpanded(app, model, 3, true);
    Check(MorphRow(model, 3, 1) >= 0 && MorphRow(model, 3, 0) < 0 && MorphRow(model, 3, 2) < 0,
          "custom morph-only group expands only its own expression");
    SetExpanded(app, model, 2, true);
    SetExpanded(app, model, 4, true);
    Check(MorphRow(model, 2, 0) >= 0 && MorphRow(model, 2, 1) >= 0 && MorphRow(model, 2, 2) < 0,
          "fixed Facial group lists only expressions assigned to it");
    Check(MorphRow(model, 4, 2) >= 0 && Child(model, 4, 0) >= 0,
          "mixed custom group expands both its bone and its expression");

    Click(app, 40, Header(model, 3));
    Check(Selected(model, 1) && Unselected(model, 0) && Unselected(model, 2) && !model.boneSelection[0],
          "custom group selection affects only its referenced expression");
    Click(app, 40, Header(model, 4));
    Check(Selected(model, 2) && Unselected(model, 0) && Unselected(model, 1) && model.boneSelection[0],
          "mixed group selection selects its bone and expression together");
    Click(app, 40, Header(model, 2));
    Check(Selected(model, 0) && Selected(model, 1) && Unselected(model, 2) && !model.boneSelection[0],
          "fixed Facial header selection excludes expressions in other groups");

    model.morphs[1].value = 0.37f;
    Click(app, 40, MorphRow(model, 3, 1));
    const HWND combo = GetDlgItem(app->MainWindow(), panel::kMorphCombo0);
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    Check(model.selectedMorphs[0] == 1 && selection != CB_ERR &&
              SendMessageW(combo, CB_GETITEMDATA, selection, 0) == 1,
          "custom expression click synchronizes the ordinary facial selector");
    Check(SendMessageW(GetDlgItem(app->MainWindow(), panel::kMorphSlider0), TBM_GETPOS, 0, 0) == 37,
          "custom expression click synchronizes the ordinary weight slider");
    Check(Selected(model, 1), "selecting a repeated expression updates every displayed reference");
    Click(app, 40, MorphRow(model, 2, 1), true);
    Check(Unselected(model, 1), "Shift-click toggles the same expression through another display group");

    constexpr unsigned keyFrame = 8;
    RegisterMorphKeyCurrent(raw, 1, keyFrame);
    const unsigned key = model.morphKeys[1].next;
    Check(key != 0 && model.morphKeys[key].frame == keyFrame,
          "custom expression uses the model's original registered morph track");
    if (key == 0 || model.morphKeys[key].frame != keyFrame) return;
    for (std::size_t i = 0; i < mdl::MorphKeyCapacity(model); ++i) model.morphKeys[i].allocated = 0;
    app->state.timelineStartFrame = 0;
    PostLanguageSweep(app);
    const int original = MorphRow(model, 2, 1), repeated = MorphRow(model, 3, 1);
    Check(original >= 0 && repeated >= 0 &&
              app->state.rowHitMorph[200 * keyFrame + original] == static_cast<int>(key) &&
              app->state.rowHitMorph[200 * keyFrame + repeated] == static_cast<int>(key),
          "both expanded references map to the same original keyframe record");
    Click(app, 100 + 13 * keyFrame + 6, repeated);
    Check(model.morphKeys[key].allocated != 0, "custom-group keyframe click selects the original record");
    Click(app, 100 + 13 * keyFrame + 6, original, true);
    Check(model.morphKeys[key].allocated == 0, "another reference toggles the same keyframe selection");
    SetExpanded(app, model, 3, false);
    const int collapsed = Header(model, 3);
    Check(collapsed >= 0 && app->state.rowHitMorph[200 * keyFrame + collapsed] == -2,
          "collapsed custom group retains a selectable aggregate keyframe cell");
    Click(app, 100 + 13 * keyFrame + 6, collapsed);
    Check(model.morphKeys[key].allocated != 0,
          "collapsed custom-group cell resolves selection to the original expression key");
    SetExpanded(app, model, 3, true);
    Check(MorphRow(model, 3, 1) >= 0 &&
              app->state.rowHitMorph[200 * keyFrame + MorphRow(model, 3, 1)] == static_cast<int>(key),
          "re-expanding preserves the original keyframe and its selection");

    const int firstRow = MorphRow(model, 2, 1), secondRow = MorphRow(model, 3, 1);
    Check(negative_frames::RegisterMorph(raw, 1, -4, 0.37f),
          "repeated expression retains one negative-frame track");
    Check(negative_frames::SelectCell(app, -4, secondRow, false) &&
              negative_frames::CellSelected(app, -4, firstRow),
          "negative key selection is shared by both expanded references");
    Check(negative_frames::SelectCell(app, -4, firstRow, true) &&
              !negative_frames::CellSelected(app, -4, secondRow),
          "another negative-frame reference toggles the same key once");
    negative_frames::SelectBox(app, -4, -3, secondRow, secondRow + 1, true, false);
    Check(negative_frames::CellSelected(app, -4, firstRow),
          "range selection in one group is not cleared by another reference outside the range");
    SetExpanded(app, model, 3, false);
    Check(negative_frames::CellSelected(app, -4, Header(model, 3)),
          "negative key remains selected in a collapsed custom group");
    Check(negative_frames::SelectCell(app, -4, Header(model, 3), true) &&
              !negative_frames::CellSelected(app, -4, MorphRow(model, 2, 1)),
          "collapsed group toggles the shared negative key once");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    TemporaryModel fixture;
    Check(fixture.Write(), "write isolated synthetic PMX fixture");
    if (failures) return 1;
    const auto hide = SetWindowsHookExW(WH_CBT, HideHook, nullptr, GetCurrentThreadId());
    const auto dialog = SetWindowsHookExW(WH_CALLWNDPROCRET, DialogHook, nullptr, GetCurrentThreadId());
    Check(hide && dialog, "install hidden-window and modal-dialog guards");
    if (!hide || !dialog) return 2;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{}; app->InitDefaults(); g_Block = app.get();
    if (!InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE)) {
        std::puts("FAIL: production startup failed");
        g_Block = nullptr;
        return 2;
    }
    ShowWindow(app->MainWindow(), SW_HIDE);
    Check(dialogs == 0, "production startup needs no modal dialogs");
    app->EnglishUI() = 1;
    SetWindowPos(app->MainWindow(), nullptr, 0, 0, 1280, 960,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LoadModelFile(app.get(), fixture.path.c_str(), false);
    app->SetSelectedModelSlot(0);
    auto* raw = app->ModelSlot(0);
    Check(dialogs == compatibilityDialogs && compatibilityDialogs == (MMD_ORIGINAL_PROFILE ? 1 : 0),
          "model loading reports only the expected original-format Unicode compatibility warning");
    Check(raw && mdl::Mdl(raw)->vertexCount == 3 && mdl::Mdl(raw)->morphCount == 3,
          "production loader opens the generated triangle and three expressions");
    SetFocus(app->MainWindow());
    if (raw && mdl::Mdl(raw)->vertexCount == 3 && mdl::Mdl(raw)->morphCount == 3) Exercise(app.get(), raw);
    ShutdownCleanup(app.get()); g_Block = nullptr;
    UnhookWindowsHookEx(dialog); UnhookWindowsHookEx(hide);
    std::printf("pmx_display_groups checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
