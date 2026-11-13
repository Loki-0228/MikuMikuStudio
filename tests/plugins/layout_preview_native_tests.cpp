// Hidden-window integration of the real NativeUi and UiRegistry. This test
// neither starts MMS/Manager nor writes settings, loads models or sends input.
// Child WS_VISIBLE and geometry are checked beneath an intentionally hidden
// top-level window; this is not a visible-pixel or physical-input acceptance.
#include "../../src/plugins/native_ui.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>
#include <commctrl.h>

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); throw std::runtime_error(#x); } } while (false)
MmsString S(const char* value) { return {value,std::strlen(value)}; }
constexpr const char* ownerId = "example.preview_native";
constexpr const char* groupId = "example.preview_native.group";
constexpr std::array<const char*,2> ids{"example.preview_native.first","example.preview_native.second"};
constexpr std::array<const char*,2> labels{"Preview First","Preview Second"};
constexpr std::array<const wchar_t*,2> wideLabels{L"Preview First",L"Preview Second"};

struct Window {
    HWND handle = nullptr;
    unsigned activations = 0, focusRequests = 0;
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) ++self->activations;
        if (self && msg == WM_SETFOCUS) ++self->focusRequests;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    Window() {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES}; CHECK(InitCommonControlsEx(&controls));
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Proc;
        cls.lpszClassName = L"MMS.LayoutPreview.HiddenTest";
        CHECK(RegisterClassW(&cls) != 0);
        handle = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,cls.lpszClassName,L"MMS hidden layout preview fixture",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,0,0,900,600,nullptr,nullptr,cls.hInstance,this);
        CHECK(handle != nullptr);
        const HMENU menu = CreateMenu(); CHECK(menu != nullptr);
        if (!SetMenu(handle,menu)) { DestroyMenu(menu); CHECK(false); }
        // Never call ShowWindow on the top-level owner.
        CHECK(!IsWindowVisible(handle));
    }
    ~Window() {
        if (handle) { const auto menu = GetMenu(handle); SetMenu(handle,nullptr); if (menu) DestroyMenu(menu); DestroyWindow(handle); }
        UnregisterClassW(L"MMS.LayoutPreview.HiddenTest",GetModuleHandleW(nullptr));
    }
    void Hidden() const {
        CHECK(!IsWindowVisible(handle) && !(GetWindowLongPtrW(handle,GWL_STYLE) & WS_VISIBLE));
        CHECK(!activations && !focusRequests);
        const auto focus = GetFocus(); CHECK(focus != handle && (!focus || !IsChild(handle,focus)));
    }
};
UINT WindowDpi(HWND window) {
    using Query = UINT (WINAPI*)(HWND);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    if (query) { const UINT dpi = query(window); if (dpi) return dpi; }
    HDC dc = GetDC(window); CHECK(dc != nullptr);
    const int dpi = GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(window,dc);
    CHECK(dpi > 0); return UINT(dpi);
}
RECT Bounds(HWND window,HWND parent) {
    RECT rect{}; CHECK(IsWindow(window) && GetWindowRect(window,&rect));
    SetLastError(ERROR_SUCCESS);
    CHECK(MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rect),2) || GetLastError() == ERROR_SUCCESS);
    return rect;
}
bool Equal(const RECT& a,const RECT& b) { return EqualRect(&a,&b) != FALSE; }
bool Near(float a,float b) { return std::abs(a-b) <= .02f; }
struct Item {
    std::array<float,4> rect{};
    bool visible = false;
    int32_t order = 0;
};
struct Snapshot {
    MmsUiLayoutApiV1 api;
    MmsUiLayoutSnapshotV1 value{};
    explicit Snapshot(MmsUiLayoutApiV1 table) : api(table) {
        value.h = Header<MmsUiLayoutSnapshotV1>();
        CHECK(api.acquireSnapshot(api.context,&value) == MMS_OK && value.handle != 0);
        CHECK(value.items.stride == sizeof(MmsUiLayoutItemV1) && value.items.recordVersion == 1);
    }
    ~Snapshot() { if (value.handle) api.releaseSnapshot(api.context,value.handle); }
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Item Find(const char* id) const {
        for (uint64_t i = 0; i < value.items.count; ++i) {
            const auto& item = *reinterpret_cast<const MmsUiLayoutItemV1*>(
                static_cast<const unsigned char*>(value.items.data) + i * value.items.stride);
            if (item.elementId.bytes == std::strlen(id) && !std::memcmp(item.elementId.data,id,item.elementId.bytes)) {
                Item result; std::copy_n(item.rect,4,result.rect.begin()); result.visible = item.visible != 0;
                result.order = item.effective.order; return result;
            }
        }
        CHECK(false); return {};
    }
};
struct Fixture {
    Window window;
    UiRegistry registry;
    UiClient owner{&registry,ownerId};
    MmsUiApiV1 ui = registry.Api(owner);
    MmsUiLayoutApiV1 layout = registry.LayoutApi(owner);
    MmsUiLayoutApiV1_1 history = registry.LayoutHistoryApi(owner);
    std::unique_ptr<NativeUi> native;
    std::array<MmsHandle,2> buttons{};
    std::array<HWND,2> controls{};
    std::array<unsigned,2> stateReads{};
    MmsHandle group = 0;
    std::string persistent;
    uint64_t preferenceRevision = 0;
    uint32_t undo = 0, redo = 0;
    Fixture() {
        MmsUiElementV1 g{}; g.h = Header<MmsUiElementV1>(); g.id = S(groupId); g.ownerPlugin = S(ownerId);
        g.parentId = S("mms.toolbar.main.extensions"); g.kind = MMS_UI_GROUP;
        g.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT | MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_RESIZE;
        CHECK(ui.registerElement(ui.context,&g,&group) == MMS_OK);
        MmsUiLayoutHintsV1 gh{}; gh.h = Header<MmsUiLayoutHintsV1>(); gh.flow = MMS_FLOW_ROW;
        gh.flags = MMS_HINT_EXPLICIT_SPACING; gh.gap[0] = 6;
        CHECK(layout.setDefaultHints(layout.context,g.id,&gh) == MMS_OK);
        for (unsigned i = 0; i < 2; ++i) {
            const auto commandId = std::string(ids[i]) + ".run";
            MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>();
            command.info.id = Text(commandId); command.info.ownerPlugin = S(ownerId); command.info.label = S(labels[i]);
            command.context = &stateReads[i];
            command.getState = [](void* context,MmsObjectRef,MmsCommandStateV1* out) -> MmsResult {
                ++*static_cast<unsigned*>(context);
                *out = {Header<MmsCommandStateV1>(),1,0,1,0,{}}; return MMS_OK;
            };
            command.invoke = [](void*,MmsObjectRef,MmsSpan,MmsHandle) -> MmsResult { return MMS_OK; };
            MmsHandle commandHandle = 0; CHECK(ui.registerCommand(ui.context,&command,&commandHandle) == MMS_OK);
            MmsUiElementV1 button{}; button.h = Header<MmsUiElementV1>(); button.id = S(ids[i]); button.ownerPlugin = S(ownerId);
            button.parentId = S(groupId); button.commandId = Text(commandId); button.label = S(labels[i]); button.kind = MMS_UI_BUTTON;
            button.capabilities = MMS_UI_LAYOUT | MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_RESIZE;
            CHECK(ui.registerElement(ui.context,&button,&buttons[i]) == MMS_OK);
            MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.order = int32_t(i);
            hints.minimum[0] = 20; hints.minimum[1] = 16;
            hints.preferred[0] = i ? 110.f : 100.f; hints.preferred[1] = i ? 34.f : 30.f;
            hints.maximum[0] = 500; hints.maximum[1] = 150;
            CHECK(layout.setDefaultHints(layout.context,button.id,&hints) == MMS_OK);
        }
        native = std::make_unique<NativeUi>(registry,window.handle,Log{});
        native->Refresh(); native->Layout(12,760,280,true);
        controls = FindControls(); CHECK(controls[0] && controls[1] && controls[0] != controls[1]);
        for (auto control : controls) CHECK((GetWindowLongPtrW(control,GWL_STYLE) & WS_VISIBLE) != 0);
        persistent = registry.ExportPreferences(); preferenceRevision = registry.PreferencesRevision();
        MmsUiLayoutHistoryInfoV1 info{}; info.h = Header<MmsUiLayoutHistoryInfoV1>();
        CHECK(history.queryHistory(history.context,&info) == MMS_OK); undo = info.undoDepth; redo = info.redoDepth;
        window.Hidden();
    }
    ~Fixture() {
        registry.RevokeLayoutPreviews(owner); native.reset(); registry.Retire(owner);
    }
    std::array<HWND,2> FindControls() const {
        std::array<HWND,2> found{};
        for (HWND child = GetWindow(native->Toolbar(),GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) {
            wchar_t name[32]{}, label[64]{}; GetClassNameW(child,name,32); GetWindowTextW(child,label,64);
            if (_wcsicmp(name,L"Button")) continue;
            for (unsigned i = 0; i < 2; ++i) if (!std::wcscmp(label,wideLabels[i])) {
                CHECK(found[i] == nullptr); found[i] = child;
            }
        }
        return found;
    }
    void Identity() const {
        const auto current = FindControls(); CHECK(current == controls);
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(IsWindow(controls[i]) && GetParent(controls[i]) == native->Toolbar());
            CHECK(registry.elements.at(ids[i]).handle == buttons[i]);
        }
    }
    void PersistentUnchanged() {
        CHECK(persistent == registry.ExportPreferences() && preferenceRevision == registry.PreferencesRevision());
        MmsUiLayoutHistoryInfoV1 info{}; info.h = Header<MmsUiLayoutHistoryInfoV1>();
        CHECK(history.queryHistory(history.context,&info) == MMS_OK && info.undoDepth == undo && info.redoDepth == redo);
    }
    std::array<RECT,2> Geometry(bool shown, bool checkPersistent = true) {
        Identity(); window.Hidden();
        const Snapshot snapshot(layout); std::array<RECT,2> bounds{};
        const float factor = 96.f / WindowDpi(window.handle);
        for (unsigned i = 0; i < 2; ++i) {
            bounds[i] = Bounds(controls[i],window.handle); const auto item = snapshot.Find(ids[i]);
            CHECK(item.visible == shown);
            CHECK(bool(GetWindowLongPtrW(controls[i],GWL_STYLE) & WS_VISIBLE) == shown);
            CHECK(Near(item.rect[0],bounds[i].left * factor) && Near(item.rect[1],bounds[i].top * factor));
            CHECK(Near(item.rect[2],(bounds[i].right-bounds[i].left)*factor) && Near(item.rect[3],(bounds[i].bottom-bounds[i].top)*factor));
        }
        CHECK(snapshot.Find(groupId).visible == shown);
        if (checkPersistent) PersistentUnchanged();
        return bounds;
    }
    MmsHandle Begin(const std::vector<LayoutPreviewValue>& values) {
        MmsHandle handle = 0; CHECK(registry.BeginLayoutPreview(owner,values,handle) == MMS_OK && handle); return handle;
    }
    uint64_t Revision(MmsHandle handle) {
        LayoutPreviewInfo info{}; CHECK(registry.QueryLayoutPreview(owner,handle,info) == MMS_OK); return info.revision;
    }
    void Apply(MmsHandle handle,const std::vector<LayoutPreviewValue>& values) {
        CHECK(registry.ApplyLayoutPreview(owner,handle,Revision(handle),values) == MMS_OK);
    }
    void Rollback(MmsHandle handle) {
        CHECK(registry.EndLayoutPreview(owner,handle,Revision(handle),false) == MMS_OK);
        LayoutPreviewInfo info{}; CHECK(registry.QueryLayoutPreview(owner,handle,info) == MMS_OK && info.state == LayoutPreviewState::RolledBack);
        CHECK(registry.ReleaseLayoutPreview(owner,handle) == MMS_OK);
    }
};
void Run() {
    Fixture f;
    const auto baseline = f.Geometry(true);
    CHECK(baseline[0].left < baseline[1].left);
    const Snapshot retained(f.layout);
    const auto oldFirst = retained.Find(ids[0]), oldSecond = retained.Find(ids[1]);
    const std::vector<LayoutPreviewValue> changed{
        {f.buttons[0],PreviewSize,true,180,52,0}, {f.buttons[1],PreviewOrder,true,0,0,-10}};
    const auto handle = f.Begin(changed);
    f.native->Layout(12,760,280,true);
    auto beforeApply = f.Geometry(true);
    CHECK(Equal(beforeApply[0],baseline[0]) && Equal(beforeApply[1],baseline[1]));
    f.Apply(handle,changed);
    f.native->Layout(12,760,280,true); // Direct layout must consume the overlay.
    const auto direct = f.Geometry(true);
    CHECK(direct[1].left < direct[0].left);
    const float scale = WindowDpi(f.window.handle) / 96.f;
    CHECK(direct[0].right-direct[0].left == int(std::lround(180 * scale)) &&
          direct[0].bottom-direct[0].top == int(std::lround(52 * scale)));
    {
        const Snapshot current(f.layout);
        CHECK(current.Find(ids[1]).order == -10);
        CHECK(retained.Find(ids[0]).rect == oldFirst.rect && retained.Find(ids[1]).rect == oldSecond.rect &&
              retained.Find(ids[1]).order == oldSecond.order);
    }
    f.native->Refresh(); // Full presentation refresh must produce the same HWND geometry.
    const auto refreshed = f.Geometry(true);
    CHECK(Equal(refreshed[0],direct[0]) && Equal(refreshed[1],direct[1]));
    const std::vector<LayoutPreviewValue> hidden{{f.group,PreviewVisible,false,0,0,0}};
    const auto hiding = f.Begin(hidden); f.Apply(hiding,hidden);
    f.native->Layout(12,760,280,true); f.Geometry(false);
    f.native->Refresh(); f.Geometry(false); // A temporary ancestor hide must not destroy child HWNDs.
    f.Rollback(hiding);
    f.native->Layout(12,760,280,true);
    const auto layoutUnhidden = f.Geometry(true);
    CHECK(Equal(layoutUnhidden[0],direct[0]) && Equal(layoutUnhidden[1],direct[1]));
    f.native->Refresh();
    const auto unhidden = f.Geometry(true);
    CHECK(Equal(unhidden[0],direct[0]) && Equal(unhidden[1],direct[1]));
    f.Rollback(handle);
    f.native->Layout(12,760,280,true);
    const auto restored = f.Geometry(true);
    CHECK(Equal(restored[0],baseline[0]) && Equal(restored[1],baseline[1]));
    f.native->Refresh();
    const auto refreshedBaseline = f.Geometry(true);
    CHECK(Equal(refreshedBaseline[0],baseline[0]) && Equal(refreshedBaseline[1],baseline[1]));
    CHECK(retained.Find(ids[0]).rect == oldFirst.rect && retained.Find(ids[1]).rect == oldSecond.rect);
    std::puts("hidden native preview size/order/group visibility: original HWNDs and layout snapshots preserved");
}
void ParentRun() {
    Fixture f;
    constexpr const char* destination = "example.preview_native.destination";
    constexpr const char* sibling = "example.preview_native.sibling";
    const auto addGroup = [&](const char* id, int order) {
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>();
        element.id = S(id); element.ownerPlugin = S(ownerId);
        element.parentId = S("mms.toolbar.main.extensions"); element.kind = MMS_UI_GROUP;
        element.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT | MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_RESIZE;
        MmsHandle handle = 0; CHECK(f.ui.registerElement(f.ui.context,&element,&handle) == MMS_OK && handle);
        MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>();
        hints.flow = MMS_FLOW_COLUMN; hints.order = order;
        hints.flags = MMS_HINT_EXPLICIT_SPACING;
        hints.padding[0] = 19; hints.padding[1] = 23; hints.padding[2] = 11; hints.padding[3] = 13;
        hints.minimum[0] = hints.preferred[0] = 210; hints.minimum[1] = hints.preferred[1] = 110;
        CHECK(f.layout.setDefaultHints(f.layout.context,element.id,&hints) == MMS_OK);
        return handle;
    };
    const auto destinationHandle = addGroup(destination,10), siblingHandle = addGroup(sibling,20);
    f.native->Refresh(); f.native->Layout(12,760,280,true);
    f.persistent = f.registry.ExportPreferences(); f.preferenceRevision = f.registry.PreferencesRevision();
    MmsUiLayoutHistoryInfoV1 initial{}; initial.h = Header<MmsUiLayoutHistoryInfoV1>();
    CHECK(f.history.queryHistory(f.history.context,&initial) == MMS_OK);
    f.undo = initial.undoDepth; f.redo = initial.redoDepth;
    const auto baseline = f.Geometry(true);
    const auto parent = [&](const char* id) { return String(f.registry.EffectiveParent(f.registry.elements.at(id))); };
    const auto savedParent = [&](const char* id) { return String(f.registry.elements.at(id).desc.parentId); };
    const auto values = [&](MmsHandle element, MmsHandle target) {
        LayoutPreviewValue value{}; value.element = element; value.fields = PreviewParent; value.parent = target;
        return std::vector<LayoutPreviewValue>{value};
    };
    const auto sameGeometry = [&](const std::array<RECT,2>& expected, bool persistent = true) {
        const auto actual = f.Geometry(true,persistent);
        CHECK(Equal(actual[0],expected[0]) && Equal(actual[1],expected[1]));
    };
    // Use the actual NativeUi target publication and refresh consumer. Counting
    // original command callbacks proves membership, rather than inspecting a
    // second test-maintained tree or replacing the production consumer.
    RefreshClient refreshClient{&f.registry.Refresh(),ownerId};
    const auto refresh = f.registry.Refresh().Api(refreshClient);
    struct RefreshOwner {
        UiRefreshQueue& queue; RefreshClient& client;
        ~RefreshOwner() { queue.Retire(client); queue.SetWake({}); }
    } refreshOwner{f.registry.Refresh(),refreshClient};
    f.registry.Refresh().SetWake([] { return true; });
    const auto submitSubtree = [&](MmsHandle root) {
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        CHECK(refresh.getTarget(refresh.context,root,&target) == MMS_OK && target.ready &&
            (target.supportedKinds & MMS_UI_REFRESH_STATE) && (target.supportedScopes & MMS_UI_REFRESH_SUBTREE));
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = target.target; request.kinds = MMS_UI_REFRESH_STATE; request.scope = MMS_UI_REFRESH_SUBTREE;
        request.dirtyRects = {nullptr,0,sizeof(MmsUiDirtyRectV1),1};
        MmsHandle ticket = 0; CHECK(refresh.request(refresh.context,&request,&ticket) == MMS_OK && ticket);
        return ticket;
    };
    const auto ticketState = [&](MmsHandle ticket) {
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>();
        CHECK(refresh.query(refresh.context,ticket,&state) == MMS_OK); return state;
    };
    const auto checkMembers = [&](MmsHandle root, bool first, bool second) {
        f.stateReads = {};
        const auto ticket = submitSubtree(root);
        CHECK(f.stateReads[0] == 0 && f.stateReads[1] == 0);
        f.registry.Refresh().Drain();
        const auto state = ticketState(ticket);
        CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.result == MMS_OK &&
            state.completedKinds == MMS_UI_REFRESH_STATE && !state.sourceRevision);
        CHECK((f.stateReads[0] != 0) == first && (f.stateReads[1] != 0) == second);
        CHECK(refresh.release(refresh.context,ticket) == MMS_OK);
        f.Identity(); f.window.Hidden();
    };
    checkMembers(f.group,true,true);

    const auto moving = values(f.buttons[0],destinationHandle);
    const auto session = f.Begin(moving);
    f.native->Layout(12,760,280,true);
    sameGeometry(baseline); CHECK(parent(ids[0]) == groupId); // Begin only reserves the claim.
    const auto oldSubtreeTicket = submitSubtree(f.group);
    CHECK(ticketState(oldSubtreeTicket).status == MMS_UI_REFRESH_PENDING);
    f.Apply(session,moving);
    CHECK(parent(ids[0]) == destination && savedParent(ids[0]) == groupId);
    f.native->Layout(12,760,280,true);
    const auto moved = f.Geometry(true);
    CHECK(!Equal(moved[0],baseline[0]));
    f.native->Refresh(); sameGeometry(moved); // HWND and element identities survive the regrouping.
    {
        const auto state = ticketState(oldSubtreeTicket);
        CHECK(state.status == MMS_UI_REFRESH_FAILED && state.result == MMS_E_STALE && !state.completedKinds);
        f.stateReads = {}; f.registry.Refresh().Drain();
        CHECK(f.stateReads[0] == 0 && f.stateReads[1] == 0);
        CHECK(refresh.release(refresh.context,oldSubtreeTicket) == MMS_OK);
    }
    checkMembers(f.group,false,true);
    checkMembers(destinationHandle,true,false);

    const std::vector<LayoutPreviewValue> hide{{destinationHandle,PreviewVisible,false,0,0,0}};
    const auto hiding = f.Begin(hide); f.Apply(hiding,hide);
    f.native->Layout(12,760,280,true); f.native->Refresh(); f.Identity(); f.PersistentUnchanged();
    {
        const Snapshot current(f.layout);
        CHECK(!current.Find(ids[0]).visible && current.Find(ids[1]).visible);
        CHECK(!(GetWindowLongPtrW(f.controls[0],GWL_STYLE) & WS_VISIBLE));
        CHECK(GetWindowLongPtrW(f.controls[1],GWL_STYLE) & WS_VISIBLE);
        CHECK(f.registry.ControlVisibility(f.registry.elements.at(ids[0])) == ControlLayoutVisibility::TemporaryHidden);
    }
    f.Rollback(hiding); f.native->Refresh(); sameGeometry(moved);
    f.Rollback(session); f.native->Layout(12,760,280,true); sameGeometry(baseline);
    CHECK(parent(ids[0]) == groupId && savedParent(ids[0]) == groupId);
    f.native->Refresh(); sameGeometry(baseline);
    checkMembers(f.group,true,true);

    // Move the entire group under a sibling in the same native mount. Its
    // descendants remain the same native windows, rather than being rebuilt.
    const auto groupMove = values(f.group,siblingHandle);
    const auto groupSession = f.Begin(groupMove); f.Apply(groupSession,groupMove);
    f.native->Layout(12,760,280,true);
    const auto groupMoved = f.Geometry(true);
    CHECK(parent(groupId) == sibling && savedParent(groupId) == "mms.toolbar.main.extensions");
    CHECK(f.registry.elements.at(groupId).handle == f.group &&
        f.registry.elements.at(destination).handle == destinationHandle && f.registry.elements.at(sibling).handle == siblingHandle);
    CHECK(!Equal(groupMoved[0],baseline[0]) && !Equal(groupMoved[1],baseline[1]));
    f.native->Refresh(); sameGeometry(groupMoved);
    checkMembers(siblingHandle,true,true);
    CHECK(f.registry.EndLayoutPreview(f.owner,groupSession,f.Revision(groupSession),true) == MMS_OK);
    LayoutPreviewInfo committed{};
    CHECK(f.registry.QueryLayoutPreview(f.owner,groupSession,committed) == MMS_OK && committed.state == LayoutPreviewState::Committed);
    CHECK(parent(groupId) == sibling && savedParent(groupId) == sibling);
    CHECK(f.registry.ExportPreferences() != f.persistent && f.registry.PreferencesRevision() != f.preferenceRevision);
    MmsUiLayoutHistoryInfoV1 info{}; info.h = Header<MmsUiLayoutHistoryInfoV1>();
    CHECK(f.history.queryHistory(f.history.context,&info) == MMS_OK && info.undoDepth == f.undo + 1 && info.redoDepth == 0);
    f.native->Layout(12,760,280,true); sameGeometry(groupMoved,false);
    f.native->Refresh(); sameGeometry(groupMoved,false);
    CHECK(f.history.undo(f.history.context) == MMS_OK);
    CHECK(parent(groupId) == "mms.toolbar.main.extensions" && savedParent(groupId) == "mms.toolbar.main.extensions");
    f.native->Refresh(); f.native->Layout(12,760,280,true); sameGeometry(baseline,false);
    CHECK(f.registry.QueryLayoutPreview(f.owner,groupSession,committed) == MMS_OK && committed.state == LayoutPreviewState::Committed);
    CHECK(f.history.queryHistory(f.history.context,&info) == MMS_OK && info.undoDepth == f.undo && info.redoDepth == 1);
    CHECK(f.history.redo(f.history.context) == MMS_OK);
    CHECK(parent(groupId) == sibling && savedParent(groupId) == sibling);
    f.native->Layout(12,760,280,true); f.native->Refresh(); sameGeometry(groupMoved,false);
    CHECK(f.history.queryHistory(f.history.context,&info) == MMS_OK && info.undoDepth == f.undo + 1 && info.redoDepth == 0);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,groupSession) == MMS_OK);
    f.Identity(); f.window.Hidden();
    std::puts("hidden native parent preview: regrouping, unchanged HWNDs, effective refresh subtree, stale tickets and persistent undo/redo");
}
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try { Run(); ParentRun(); }
    catch (const std::exception& error) { if (!failures) ++failures; std::fprintf(stderr,"native preview failure: %s\n",error.what()); }
    catch (...) { if (!failures) ++failures; std::fprintf(stderr,"native preview failure: unknown exception\n"); }
    std::printf("layout_preview_native_tests: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
