#include "native_ui.hpp"
#include "ui_refresh_binding.hpp"
#include <mms/cpp/refresh.hpp>
#include <array>
#include <functional>

namespace layout_refresh_checks {
namespace sdk = mms::sdk;
struct Fixture {
    static constexpr int heightLimit = 1024; // Keep wrapping changes below the host's clipping limit at 100–200% DPI.
    manager_refresh_checks::Window window;
    UiRegistry registry;
    UiRefreshQueue& queue = registry.Refresh();
    UiClient owner{&registry, "example.layout"}, provider{&registry, "example.surface"};
    RefreshClient client{&queue, owner.id}, providerClient{&queue, provider.id};
    MmsUiApiV1 ui = registry.Api(owner), providerUi = registry.Api(provider);
    MmsUiRefreshApiV1 api = queue.Api(client), providerApi = queue.Api(providerClient);
    MmsUiLayoutApiV1 layout = registry.LayoutApi(owner);
    std::unique_ptr<NativeUi> native;
    std::array<MmsHandle, 3> commands{}, buttons{};
    std::array<HWND, 3> controls{};
    std::array<unsigned, 3> stateReads{};
    std::array<uint32_t, 3> visible{1,1,1}, enabled{1,1,1}, checked{};
    struct Command { Fixture* fixture; unsigned index; } commandContext[3]{{this,0},{this,1},{this,2}};
    MmsHandle surface = 0, panel = 0, source = 0, binding = 0, providerSource = 0, providerBinding = 0;
    HWND surfaceWindow = nullptr, workspace = nullptr;
    std::vector<MmsHandle> tickets, leases;
    unsigned creates = 0, destroys = 0, resizes = 0, shows = 0, heightNotifications = 0;
    int publishedHeight = 0, constrainWidth = 0;
    bool resizeThrows = false, publishedThrows = false, inspectProvider = false, providerObserved = false;
    MmsResult publishedResult = MMS_OK;
    std::function<void()> onResize;
    explicit Fixture(int width = 250) {
        const char* ids[] = {"example.layout.first", "example.layout.second", "example.layout.third"};
        const char* labels[] = {"First", "Second", "Third"};
        for (unsigned i = 0; i < 3; ++i) {
            const auto commandId = std::string(ids[i]) + ".command";
            MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>();
            command.info.id = Text(commandId); command.info.ownerPlugin = Text(owner.id); command.info.label = S(labels[i]);
            command.context = &commandContext[i];
            command.getState = [](void* p, MmsObjectRef, MmsCommandStateV1* out) -> MmsResult {
                auto& c = *static_cast<Command*>(p); auto& x = *c.fixture; ++x.stateReads[c.index];
                *out = {Header<MmsCommandStateV1>(), x.enabled[c.index], x.checked[c.index], x.visible[c.index], 0, {}}; return MMS_OK;
            };
            command.invoke = [](void*, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { return MMS_OK; };
            CHECK(ui.registerCommand(ui.context, &command, &commands[i]) == MMS_OK);
            MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = S(ids[i]);
            element.ownerPlugin = Text(owner.id); element.parentId = S("mms.toolbar.main.extensions");
            element.label = S(labels[i]); element.commandId = Text(commandId); element.kind = MMS_UI_BUTTON;
            element.capabilities = MMS_UI_LAYOUT | MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_RESIZE;
            CHECK(ui.registerElement(ui.context, &element, &buttons[i]) == MMS_OK);
            MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>();
            hints.minimum[0] = hints.preferred[0] = hints.maximum[0] = 100;
            hints.minimum[1] = hints.preferred[1] = hints.maximum[1] = 24;
            CHECK(layout.setDefaultHints(layout.context, element.id, &hints) == MMS_OK);
        }
        auto surfaces = registry.SurfaceApi(provider);
        MmsUiSurfaceDescV1 view{}; view.h = Header<MmsUiSurfaceDescV1>(); view.id = S("example.surface.factory");
        view.context = this; view.minimumWidth = 40; view.minimumHeight = 24; view.preferredWidth = 80; view.preferredHeight = 64;
        view.createView = [](void* p, const MmsUiMountV1* mount, void** context, uintptr_t* native) -> MmsResult {
            auto& x = *static_cast<Fixture*>(p); ++x.creates;
            x.surfaceWindow = CreateWindowExW(0, L"STATIC", L"Layout surface", WS_CHILD,
                mount->bounds.x, mount->bounds.y, mount->bounds.width, mount->bounds.height,
                reinterpret_cast<HWND>(mount->parentWindow), nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!x.surfaceWindow) return MMS_E_FAILED; *context = &x; *native = reinterpret_cast<uintptr_t>(x.surfaceWindow); return MMS_OK;
        };
        view.resizeView = [](void* p, const MmsUiRectV1* bounds, uint32_t) {
            auto& x = *static_cast<Fixture*>(p); ++x.resizes;
            if (x.inspectProvider) x.InspectProvider();
            if (x.onResize) x.onResize();
            if (x.resizeThrows) throw std::runtime_error("controlled surface resize failure");
            CHECK(MoveWindow(x.surfaceWindow, bounds->x, bounds->y, bounds->width, bounds->height, FALSE));
        };
        view.showView = [](void* p, uint32_t show) { auto& x = *static_cast<Fixture*>(p); ++x.shows; ShowWindow(x.surfaceWindow, show ? SW_SHOWNA : SW_HIDE); };
        view.focusView = [](void* p) -> MmsResult { SetFocus(static_cast<Fixture*>(p)->surfaceWindow); return MMS_OK; };
        view.destroyView = [](void* p) { auto& x = *static_cast<Fixture*>(p); ++x.destroys; DestroyWindow(x.surfaceWindow); x.surfaceWindow = nullptr; };
        CHECK(surfaces.registerSurface(surfaces.context, &view, &surface) == MMS_OK);
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = S("example.surface.panel");
        element.ownerPlugin = Text(provider.id); element.parentId = S("mms.workspace.tabs.extensions");
        element.label = S("Surface"); element.surfaceId = view.id; element.kind = MMS_UI_PANEL;
        element.capabilities = MMS_UI_LAYOUT | MMS_UI_HIDE | MMS_UI_RESIZE;
        CHECK(providerUi.registerElement(providerUi.context, &element, &panel) == MMS_OK);
        native = std::make_unique<NativeUi>(registry, window.value, Log{}); native->Refresh();
        CHECK(native->Layout(10, width, heightLimit, true) > 0);
        for (auto child = GetWindow(native->Toolbar(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t text[32]{}; GetWindowTextW(child, text, 32);
            for (unsigned i = 0; i < 3; ++i) if (std::wstring(text) == Wide(labels[i])) controls[i] = child;
        }
        for (auto control : controls) CHECK(control && IsWindowVisible(control));
        for (auto child = GetWindow(window.value, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t name[64]{}; GetClassNameW(child, name, 64);
            if (!std::wcscmp(name, L"MMS.PluginAbi.Workspace.V1")) workspace = child;
        }
        CHECK(workspace && surfaceWindow && creates == 1);
        CHECK(SetWindowSubclass(workspace, Constrain, 0x1a70, reinterpret_cast<DWORD_PTR>(this)));
        native->SetLayoutPublished([this](int height) -> MmsResult {
            CHECK(registry.InCallback() && queue.ReadOnlyCause(client));
            MmsUiLayoutSnapshotV1 pending{}; pending.h = Header<MmsUiLayoutSnapshotV1>(); pending.handle = 999;
            CHECK(layout.acquireSnapshot(layout.context, &pending) == MMS_E_NOT_READY && pending.handle == 999);
            ++heightNotifications; publishedHeight = height;
            if (publishedThrows) throw std::runtime_error("controlled dependent layout failure");
            return publishedResult;
        });
        RegisterData(client, Target(0).target, "layout-source", source, binding);
        RegisterData(providerClient, ProviderTarget().target, "surface-source", providerSource, providerBinding);
    }
    ~Fixture() {
        queue.Retire(client); queue.Retire(providerClient);
        for (auto h : tickets) api.release(api.context, h);
        for (auto h : leases) layout.releaseSnapshot(layout.context, h);
        if (IsWindow(workspace)) RemoveWindowSubclass(workspace, Constrain, 0x1a70);
        native.reset(); registry.Retire(owner); registry.Retire(provider);
    }
    static LRESULT CALLBACK Constrain(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR p) {
        auto& x = *reinterpret_cast<Fixture*>(p);
        if (message == WM_WINDOWPOSCHANGING && x.constrainWidth) {
            auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
            if (!(position.flags & SWP_NOSIZE)) position.cx = (std::max)(100, position.cx - x.constrainWidth);
        }
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    void RegisterData(RefreshClient& c, MmsUiTargetRefV1 target, const char* id, MmsHandle& sourceHandle, MmsHandle& bindingHandle) {
        auto data = queue.Bindings().Api(c);
        MmsUiDataSourceDescV1 desc{}; desc.h = Header<MmsUiDataSourceDescV1>(); desc.id = S(id); desc.maxBytes = 64;
        CHECK(data.registerSource(data.context, &desc, &sourceHandle) == MMS_OK);
        const int value = 1; MmsUiDataSourcePublishV1 update{}; update.h = Header<MmsUiDataSourcePublishV1>();
        update.source = sourceHandle; update.bytes = {&value, sizeof(value)};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        CHECK(data.publishSource(data.context, &update, &state) == MMS_OK && state.revision == 1);
        MmsUiDataBindingDescV1 bound{}; bound.h = Header<MmsUiDataBindingDescV1>(); bound.target = target;
        bound.source = sourceHandle; bound.maxDisplayBytes = 64; CHECK(data.bind(data.context, &bound, &bindingHandle) == MMS_OK);
    }
    MmsUiRefreshTargetV1 Target(unsigned i) {
        MmsUiRefreshTargetV1 value{}; value.h = Header<MmsUiRefreshTargetV1>(); CHECK(api.getTarget(api.context, buttons[i], &value) == MMS_OK); return value;
    }
    MmsUiRefreshTargetV1 ProviderTarget() {
        MmsUiRefreshTargetV1 value{}; value.h = Header<MmsUiRefreshTargetV1>(); CHECK(providerApi.getTarget(providerApi.context, panel, &value) == MMS_OK); return value;
    }
    MmsUiRefreshRequestV1 Request(unsigned i, uint32_t kinds = MMS_UI_REFRESH_LAYOUT) {
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>(); request.target = Target(i).target;
        request.kinds = kinds; request.scope = MMS_UI_REFRESH_SELF; return request;
    }
    MmsHandle Submit(const MmsUiRefreshRequestV1& request) {
        MmsHandle ticket = 0; CHECK(api.request(api.context, &request, &ticket) == MMS_OK && ticket); tickets.push_back(ticket); return ticket;
    }
    MmsUiRefreshStateV1 State(MmsHandle ticket) {
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>(); CHECK(api.query(api.context, ticket, &state) == MMS_OK); return state;
    }
    void Completed(MmsHandle ticket, uint32_t kinds) {
        const auto state = State(ticket); CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.result == MMS_OK && state.completedKinds == kinds);
    }
    MmsUiLayoutSnapshotV1 Snapshot() {
        MmsUiLayoutSnapshotV1 value{}; value.h = Header<MmsUiLayoutSnapshotV1>(); CHECK(layout.acquireSnapshot(layout.context, &value) == MMS_OK);
        leases.push_back(value.handle); return value;
    }
    static MmsUiLayoutItemV1 Item(const MmsUiLayoutSnapshotV1& snapshot, const char* id) {
        for (uint64_t i = 0; i < snapshot.items.count; ++i) {
            const auto& item = *reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(snapshot.items.data) + i * snapshot.items.stride);
            if (String(item.elementId) == id) return item;
        }
        throw std::runtime_error("native layout snapshot item missing");
    }
    static RECT Bounds(HWND window) { RECT bounds{}; CHECK(GetWindowRect(window, &bounds)); return bounds; }
    void InspectProvider() {
        providerObserved = true; CHECK(registry.InCallback() && !registry.InvokingClient());
        CHECK(queue.ReadOnlyCause(client) && queue.ReadOnlyCause(providerClient));
        CHECK(ui.unregisterItem(ui.context, buttons[2]) == MMS_E_BUSY);
        CHECK(providerUi.unregisterItem(providerUi.context, panel) == MMS_E_BUSY);
        for (auto kinds : {MMS_UI_REFRESH_LAYOUT, MMS_UI_REFRESH_STATE, MMS_UI_REFRESH_DATA}) {
            auto request = Request(0, kinds); if (kinds == MMS_UI_REFRESH_DATA) { request.binding = binding; request.sourceRevision = 1; }
            MmsHandle untouched = 777; CHECK(api.request(api.context, &request, &untouched) == MMS_E_BUSY && untouched == 777);
            request.target = ProviderTarget().target;
            if (kinds == MMS_UI_REFRESH_DATA) request.binding = providerBinding;
            CHECK(providerApi.request(providerApi.context, &request, &untouched) == MMS_E_BUSY && untouched == 777);
        }
    }
};
static void CachedPureLayoutAndSdk() {
    Fixture x; const auto target = x.Target(0), sibling = x.Target(2); const auto initial = x.Snapshot();
    const auto reads = x.stateReads; const auto uiRevision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    const auto saved = x.registry.ExportPreferences(); const auto creates = x.creates;
    CHECK((target.supportedKinds & MMS_UI_REFRESH_LAYOUT) && (x.ProviderTarget().supportedKinds & MMS_UI_REFRESH_LAYOUT));
    x.visible[1] = 0; // Pure LAYOUT must use the last published state.
    auto ticket = x.Submit(x.Request(0)); x.queue.Drain(); x.Completed(ticket, MMS_UI_REFRESH_LAYOUT);
    CHECK(x.stateReads == reads && IsWindowVisible(x.controls[1]) && x.Target(1).visible);
    CHECK(x.Target(0).geometryRevision == target.geometryRevision && x.Target(2).geometryRevision == sibling.geometryRevision);
    const auto current = x.Snapshot(); CHECK(current.geometryRevision == initial.geometryRevision && x.heightNotifications == 0 && x.creates == creates);
    MmsHostV1 host{}; host.h = Header<MmsHostV1>(); host.context = &x;
    host.queryService = [](void* p, MmsString id, uint32_t major, uint32_t minor, uint32_t bytes, void* out) -> MmsResult {
        auto& f = *static_cast<Fixture*>(p); if (String(id) != "mms.ui.refresh") return MMS_E_NOT_FOUND;
        return Table(major, minor, bytes, out, f.api);
    };
    sdk::RefreshServices service; CHECK(sdk::RefreshServices::Query(host, service) == MMS_OK);
    sdk::RefreshTicket owned; CHECK(service.RelayoutCurrent(target.target, owned) == MMS_OK && owned);
    const auto sameHandle = owned.get(); x.queue.Drain(); MmsUiRefreshStateV1 state{}; CHECK(owned.Query(state) == MMS_OK);
    CHECK(x.State(sameHandle).completedKinds == MMS_UI_REFRESH_LAYOUT && state.status == MMS_UI_REFRESH_COMPLETED && x.stateReads == reads);
    CHECK(owned.Release() == MMS_OK && !owned);
    CHECK(service.RefreshStateLayout(x.Target(1).target, owned) == MMS_OK); x.queue.Drain();
    CHECK(owned.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == 13);
    CHECK(owned.Release() == MMS_OK && !x.Target(1).ready && !x.Target(1).visible);
    CHECK(x.registry.Revision() == uiRevision && x.registry.PreferencesRevision() == preferences && x.registry.ExportPreferences() == saved);
}
static void CollapseRestoreAndStrictTickets() {
    Fixture x; const auto first = x.Target(1), peer = x.Target(2); const auto old = x.Snapshot();
    const auto oldPeer = Fixture::Bounds(x.controls[2]); const auto oldFirst = Fixture::Bounds(x.controls[0]);
    CHECK(oldPeer.top > oldFirst.top); // A real wrapping row must disappear on collapse.
    const auto reads = x.stateReads; const auto saved = x.registry.ExportPreferences();
    SetFocus(x.controls[1]); CHECK(GetFocus() == x.controls[1]); x.visible[1] = 0;
    const auto hide = x.Submit(x.Request(1, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT));
    MmsUiDirtyRectV1 rect{1,1,4,4}; auto strict = x.Request(2, MMS_UI_REFRESH_REPAINT);
    strict.expectedGeometryRevision = peer.geometryRevision; strict.dirtyRects = {&rect,1,sizeof(rect),1}; const auto stale = x.Submit(strict);
    x.queue.Drain(); x.Completed(hide, 13); CHECK(x.State(stale).result == MMS_E_STALE && !x.State(stale).completedKinds);
    const auto collapsed = x.Target(1); const auto moved = Fixture::Bounds(x.controls[2]), firstAfter = Fixture::Bounds(x.controls[0]);
    CHECK(!collapsed.ready && !collapsed.visible && (!collapsed.width || !collapsed.height) && !IsWindowVisible(x.controls[1]));
    CHECK(collapsed.target.instanceGeneration == first.target.instanceGeneration && collapsed.target.projectEpoch == first.target.projectEpoch);
    CHECK(!EqualRect(&oldPeer, &moved) && EqualRect(&oldFirst, &firstAfter) && IsWindowVisible(x.controls[2]));
    CHECK(GetFocus() != x.controls[1] && GetFocus() && IsWindowVisible(GetFocus()) && IsWindowEnabled(GetFocus()));
    const auto focus = GetFocus(); CHECK(x.stateReads[0] == reads[0] && x.stateReads[1] == reads[1] + 1 && x.stateReads[2] == reads[2]);
    CHECK(x.heightNotifications == 1 && Fixture::Item(old, "example.layout.second").visible);
    const auto hidden = x.Snapshot(); CHECK(!Fixture::Item(hidden, "example.layout.second").visible);
    x.visible[1] = 1; auto restore = x.Request(1, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT);
    restore.target = first.target; const auto restored = x.Submit(restore); x.queue.Drain(); x.Completed(restored, 13);
    CHECK(x.Target(1).ready && x.Target(1).visible && x.Target(1).target.instanceGeneration == first.target.instanceGeneration);
    const auto peerRestored = Fixture::Bounds(x.controls[2]); CHECK(EqualRect(&oldPeer, &peerRestored) && GetFocus() == focus);
    CHECK(x.heightNotifications == 2 && x.registry.ExportPreferences() == saved);
    const auto notifications = x.heightNotifications; const auto geometry = x.Snapshot().geometryRevision;
    const auto same = x.Submit(x.Request(1)); x.queue.Drain(); x.Completed(same, MMS_UI_REFRESH_LAYOUT);
    CHECK(x.heightNotifications == notifications && x.Snapshot().geometryRevision == geometry);
}
static void PartialRectAndEpoch() {
    {
        Fixture x; const auto original = x.Target(1); x.visible[1] = 0;
        auto request = x.Request(1, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT);
        MmsUiDirtyRectV1 rect{1,1,3,3}; request.expectedGeometryRevision = original.geometryRevision;
        request.dirtyRects = {&rect,1,sizeof(rect),1}; const auto ticket = x.Submit(request); x.queue.Drain(); const auto state = x.State(ticket);
        CHECK(state.status == MMS_UI_REFRESH_FAILED && state.result == MMS_E_STALE && state.completedKinds == (MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT));
        CHECK(state.geometryRevision == x.Target(1).geometryRevision && !x.Target(1).visible);
    }
    {
        Fixture x; const auto original = x.Target(0); x.constrainWidth = 30;
        x.onResize = [&] { x.queue.AdvanceProjectEpoch(); }; const auto ticket = x.Submit(x.Request(0)); x.queue.Drain();
        CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_STALE);
        CHECK(x.Target(0).target.projectEpoch != original.target.projectEpoch && !x.queue.ReadOnlyCause() && !x.registry.InCallback());
        auto stale = x.Request(0); stale.target = original.target; MmsHandle untouched = 456;
        CHECK(x.api.request(x.api.context, &stale, &untouched) == MMS_E_STALE && untouched == 456);
    }
    {
        Fixture x; const auto ticket = x.Submit(x.Request(0)); CHECK(DestroyWindow(x.controls[0])); x.queue.Drain();
        CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_STALE && !x.State(ticket).completedKinds);
    }
}
static void SurfaceFailureAndPublishedFailure() {
    {
        Fixture x; const auto old = x.Snapshot(); const auto original = Fixture::Item(old, "example.surface.panel");
        const auto reads = x.stateReads; x.constrainWidth = 30; x.resizeThrows = true; x.inspectProvider = true;
        const auto ticket = x.Submit(x.Request(0)); x.queue.Drain();
        CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_FAILED && !x.State(ticket).completedKinds);
        CHECK(x.providerObserved && x.stateReads == reads && !x.registry.InCallback() && !x.queue.ReadOnlyCause());
        MmsUiLayoutSnapshotV1 unavailable{}; unavailable.h = Header<MmsUiLayoutSnapshotV1>(); unavailable.handle = 777;
        CHECK(x.layout.acquireSnapshot(x.layout.context, &unavailable) == MMS_E_NOT_READY && unavailable.handle == 777);
        const auto held = Fixture::Item(old, "example.surface.panel"); CHECK(!std::memcmp(held.rect, original.rect, sizeof(held.rect)));
        CHECK(!x.Target(0).ready); x.resizeThrows = false; x.inspectProvider = false; x.constrainWidth = 0;
        CHECK(x.native->Layout(10,250,Fixture::heightLimit,true) > 0 && x.Target(0).ready); const auto recovered = x.Snapshot();
        CHECK(Fixture::Item(recovered, "example.surface.panel").visible && x.creates == 1 && x.destroys == 0);
        const auto retry = x.Submit(x.Request(0)); const auto now = x.stateReads; x.queue.Drain(); x.Completed(retry, MMS_UI_REFRESH_LAYOUT);
        CHECK(x.stateReads == now);
    }
    for (bool throws : {false, true}) {
        Fixture x; const auto old = x.Snapshot(); x.publishedResult = MMS_E_FAILED; x.publishedThrows = throws; x.visible[1] = 0;
        const auto ticket = x.Submit(x.Request(1, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT)); x.queue.Drain();
        CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_FAILED && x.State(ticket).completedKinds == MMS_UI_REFRESH_STATE);
        CHECK(x.heightNotifications == 1 && Fixture::Item(old, "example.layout.second").visible);
        MmsUiLayoutSnapshotV1 unavailable{}; unavailable.h = Header<MmsUiLayoutSnapshotV1>(); unavailable.handle = 888;
        CHECK(x.layout.acquireSnapshot(x.layout.context, &unavailable) == MMS_E_NOT_READY && unavailable.handle == 888);
        x.publishedResult = MMS_OK; x.publishedThrows = false;
        CHECK(x.native->Layout(10,250,Fixture::heightLimit,true) > 0 && x.Target(0).ready); x.Snapshot();
    }
}
} // namespace layout_refresh_checks

static void LayoutRefreshChecks() {
    layout_refresh_checks::CachedPureLayoutAndSdk();
    layout_refresh_checks::CollapseRestoreAndStrictTickets();
    layout_refresh_checks::PartialRectAndEpoch();
    layout_refresh_checks::SurfaceFailureAndPublishedFailure();
}
