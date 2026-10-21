#include "ui_refresh.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <thread>

namespace refresh_checks {
struct PaintProbe {
    unsigned paints = 0;
    RECT last{};
    static LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* probe = reinterpret_cast<PaintProbe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PAINT && probe) {
            PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
            ++probe->paints; probe->last = paint.rcPaint;
            FillRect(dc, &paint.rcPaint, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
            EndPaint(window, &paint); return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};

struct Fixture {
    UiRegistry registry;
    UiRefreshQueue& queue = registry.Refresh();
    UiClient owner{&registry, "example.refresh"}, foreign{&registry, "example.foreign"};
    RefreshClient client{&queue, "example.refresh"}, other{&queue, "example.foreign"};
    MmsUiApiV1 ui = registry.Api(owner);
    MmsUiRefreshApiV1 api = queue.Api(client), otherApi = queue.Api(other);
    HWND parent = nullptr, button = nullptr, sibling = nullptr, panel = nullptr;
    PaintProbe panelPaint;
    MmsHandle command = 0, element = 0, second = 0, factory = 0, panelElement = 0;
    unsigned stateReads = 0, invokes = 0, creates = 0, destroys = 0;
    uint32_t stateEnabled = 1, stateChecked = 0, stateVisible = 1;
    MmsResult stateResult = MMS_OK;
    std::function<void()> onState;
    bool targetDuringInvoke = false;
    MmsResult invokeTargetResult = MMS_E_FAILED;
    std::atomic<unsigned> wakes{0};
    std::vector<RefreshPresentation> presentations;
    std::vector<MmsHandle> tickets;

    explicit Fixture(bool rejectWake = false) {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = PaintProbe::Window;
        wc.lpszClassName = L"MMS.Refresh.Contract.Paint";
        CHECK(RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        // The runner places this process on a private desktop. These test-only
        // windows use real update regions and BeginPaint, without paint hooks.
        parent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Refresh contract", WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(parent != nullptr);
        button = CreateWindowExW(0, L"BUTTON", L"target", WS_CHILD | WS_VISIBLE, 40, 50, 300, 120,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        sibling = CreateWindowExW(0, L"BUTTON", L"other", WS_CHILD | WS_VISIBLE, 360, 50, 180, 120,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(button && sibling);
        MmsCommandDescV1 d{}; d.info.h = Header<MmsCommandDescV1>(); d.info.id = S("example.refresh.command");
        d.info.ownerPlugin = S("example.refresh"); d.info.label = S("Refresh"); d.context = this;
        d.getState = [](void* p, MmsObjectRef, MmsCommandStateV1* s) -> MmsResult {
            auto& x = *static_cast<Fixture*>(p); ++x.stateReads;
            if (x.onState) x.onState();
            *s = {}; s->h = Header<MmsCommandStateV1>();
            s->enabled = x.stateEnabled; s->checked = x.stateChecked; s->visible = x.stateVisible; return x.stateResult;
        };
        d.invoke = [](void* p, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult {
            auto& x = *static_cast<Fixture*>(p); ++x.invokes;
            if (x.targetDuringInvoke) {
                MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
                x.invokeTargetResult = x.api.getTarget(x.api.context, x.element, &target);
                if (!x.invokeTargetResult && target.target.element != x.element) return MMS_E_FAILED;
            }
            return MMS_OK;
        };
        CHECK(ui.registerCommand(ui.context, &d, &command) == MMS_OK);
        auto e = Button("example.refresh.button"); CHECK(ui.registerElement(ui.context, &e, &element) == MMS_OK);
        e.id = S("example.refresh.sibling"); CHECK(ui.registerElement(ui.context, &e, &second) == MMS_OK);
        auto surfaces = registry.SurfaceApi(owner); MmsUiSurfaceDescV1 s{}; s.h = Header<MmsUiSurfaceDescV1>();
        s.id = S("example.refresh.factory"); s.context = this; s.flags = MMS_SURFACE_MULTIPLE_VIEWS;
        s.createView = [](void* p, const MmsUiMountV1* m, void** out, uintptr_t* native) -> MmsResult {
            auto& x = *static_cast<Fixture*>(p); ++x.creates;
            x.panel = CreateWindowExW(0, L"MMS.Refresh.Contract.Paint", L"panel", WS_CHILD | WS_VISIBLE,
                m->bounds.x, m->bounds.y, m->bounds.width, m->bounds.height,
                reinterpret_cast<HWND>(m->parentWindow), nullptr, GetModuleHandleW(nullptr), &x.panelPaint);
            if (!x.panel) return MMS_E_FAILED;
            *out = &x; *native = reinterpret_cast<uintptr_t>(x.panel); return MMS_OK;
        };
        s.resizeView = [](void* p, const MmsUiRectV1* r, uint32_t) { auto& x = *static_cast<Fixture*>(p); MoveWindow(x.panel, r->x, r->y, r->width, r->height, FALSE); };
        s.showView = [](void* p, uint32_t show) { ShowWindow(static_cast<Fixture*>(p)->panel, show ? SW_SHOWNOACTIVATE : SW_HIDE); };
        s.focusView = [](void*) -> MmsResult { return MMS_OK; };
        s.destroyView = [](void* p) { auto& x = *static_cast<Fixture*>(p); ++x.destroys; DestroyWindow(x.panel); x.panel = nullptr; };
        CHECK(surfaces.registerSurface(surfaces.context, &s, &factory) == MMS_OK);
        e = {}; e.h = Header<MmsUiElementV1>(); e.id = S("example.refresh.panel"); e.ownerPlugin = S("example.refresh");
        e.parentId = S("mms.workspace.tabs.extensions"); e.surfaceId = s.id; e.kind = MMS_UI_PANEL; e.capabilities = MMS_UI_HIDE | MMS_UI_RESIZE;
        CHECK(ui.registerElement(ui.context, &e, &panelElement) == MMS_OK);
        CHECK(registry.ShowSurface("example.refresh.panel", parent, {40, 210, 300, 180}, 96, true) == MMS_OK);
        ShowWindow(parent, SW_SHOWNOACTIVATE);
        presentations = {{element, owner.id, 11, button, 96, 40, 50, 300, 120, true},
                         {second, owner.id, 12, sibling, 96, 360, 50, 180, 120, true},
                         {panelElement, owner.id, 13, panel, 96, 40, 210, 300, 180, true}};
        queue.SetWake([this, rejectWake] { wakes.fetch_add(1); return !rejectWake; }); queue.SyncTargets(presentations); Clean();
    }
    ~Fixture() {
        queue.Retire(client); queue.Retire(other);
        for (auto h : tickets) api.release(api.context, h);
        queue.Drain(); registry.DestroyViews();
        if (parent) DestroyWindow(parent);
    }
    MmsUiElementV1 Button(const char* id) const {
        MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = S(id); e.ownerPlugin = S("example.refresh");
        e.parentId = S("mms.toolbar.main.extensions"); e.commandId = S("example.refresh.command");
        e.kind = MMS_UI_BUTTON; e.capabilities = MMS_UI_HIDE | MMS_UI_MOVE; return e;
    }
    void Clean() {
        UpdateWindow(parent); UpdateWindow(button); UpdateWindow(sibling); if (panel) UpdateWindow(panel);
        ValidateRect(parent, nullptr); ValidateRect(button, nullptr); ValidateRect(sibling, nullptr); if (panel) ValidateRect(panel, nullptr);
    }
    MmsUiRefreshTargetV1 Target(MmsHandle h = 0) {
        MmsUiRefreshTargetV1 t{}; t.h = Header<MmsUiRefreshTargetV1>();
        CHECK(api.getTarget(api.context, h ? h : element, &t) == MMS_OK); return t;
    }
    MmsUiRefreshRequestV1 Request(const MmsUiRefreshTargetV1& target, const MmsUiDirtyRectV1* rects = nullptr, size_t count = 0) const {
        MmsUiRefreshRequestV1 r{}; r.h = Header<MmsUiRefreshRequestV1>(); r.target = target.target;
        r.kinds = MMS_UI_REFRESH_REPAINT; r.scope = MMS_UI_REFRESH_SELF;
        r.expectedGeometryRevision = target.geometryRevision;
        r.dirtyRects = {rects, count, sizeof(MmsUiDirtyRectV1), 1}; r.coalesceKey = 29; return r;
    }
    MmsHandle Submit(const MmsUiRefreshRequestV1& r) {
        MmsHandle ticket = 0; CHECK(api.request(api.context, &r, &ticket) == MMS_OK && ticket != 0); tickets.push_back(ticket); return ticket;
    }
    MmsUiRefreshStateV1 State(MmsHandle ticket) {
        MmsUiRefreshStateV1 s{}; s.h = Header<MmsUiRefreshStateV1>(); CHECK(api.query(api.context, ticket, &s) == MMS_OK); return s;
    }
    void Completed(MmsHandle ticket) {
        auto s = State(ticket); CHECK(s.status == MMS_UI_REFRESH_COMPLETED && s.result == MMS_OK && s.completedKinds == MMS_UI_REFRESH_REPAINT && s.sourceRevision == 0);
    }
};

static bool Empty(HWND window) { RECT r{}; return GetUpdateRect(window, &r, FALSE) == FALSE; }
static void Region(HWND window, std::initializer_list<RECT> rectangles) {
    static unsigned regionCheck = 0; ++regionCheck;
    HRGN actual = CreateRectRgn(0, 0, 0, 0), expected = CreateRectRgn(0, 0, 0, 0);
    CHECK(actual && expected); CHECK(GetUpdateRgn(window, actual, FALSE) != ERROR);
    for (const auto& rect : rectangles) { HRGN part = CreateRectRgnIndirect(&rect); CHECK(part != nullptr); CHECK(CombineRgn(expected, expected, part, RGN_OR) != ERROR); DeleteObject(part); }
    const bool same = EqualRgn(actual, expected) != FALSE;
    if (!same) {
        RECT client{}, screen{}; GetClientRect(window, &client); GetWindowRect(window, &screen);
        std::printf("REFRESH_REGION check=%u hwnd=%p visible=%d client=%ld,%ld,%ld,%ld screen=%ld,%ld,%ld,%ld\n",
            regionCheck, window, IsWindowVisible(window), client.left, client.top, client.right, client.bottom,
            screen.left, screen.top, screen.right, screen.bottom);
        const auto print = [](const char* label, HRGN region) {
            RECT box{}; const auto kind = GetRgnBox(region, &box); const auto bytes = GetRegionData(region, 0, nullptr);
            std::printf("REFRESH_REGION %s kind=%d box=%ld,%ld,%ld,%ld bytes=%lu\n", label, kind, box.left, box.top, box.right, box.bottom, bytes);
            if (!bytes) return;
            std::vector<unsigned char> storage(bytes); auto* data = reinterpret_cast<RGNDATA*>(storage.data());
            if (!GetRegionData(region, bytes, data)) return;
            const auto* items = reinterpret_cast<const RECT*>(data->Buffer);
            for (DWORD i = 0; i < data->rdh.nCount; ++i)
                std::printf("REFRESH_REGION %s rect[%lu]=%ld,%ld,%ld,%ld\n", label, i, items[i].left, items[i].top, items[i].right, items[i].bottom);
        };
        print("actual", actual); print("expected", expected); std::fflush(stdout);
    }
    DeleteObject(actual); DeleteObject(expected); CHECK(same);
}

static void CapabilitiesAndValidation() {
    Fixture x; MmsUiRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshCapabilitiesV1>();
    CHECK(x.api.getCapabilities(x.api.context, &caps) == MMS_OK);
    CHECK(caps.supportedKinds == (MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_DATA) && caps.supportedScopes == (MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS));
    CHECK(caps.maxTicketsPerPlugin == 256 && caps.maxRectsPerRequest == 32 && caps.maxPendingBytesPerPlugin == 256 * 1024 && caps.maxContributionsPerDrain == 64);
    auto target = x.Target(); CHECK(target.target.element == x.element && target.target.instanceGeneration && target.target.projectEpoch && target.geometryRevision);
    CHECK(target.ready && target.visible && target.width == 300 && target.height == 120);
    CHECK(target.supportedKinds == MMS_UI_REFRESH_REPAINT && target.supportedScopes == (MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE));
    MmsUiRefreshTargetV1 wrong{}; wrong.h = Header<MmsUiRefreshTargetV1>();
    CHECK(x.otherApi.getTarget(x.otherApi.context, x.element, &wrong) != MMS_OK);
    CHECK(x.api.getTarget(x.api.context, x.command, &wrong) != MMS_OK);
    CHECK(x.api.getTarget(x.api.context, x.factory, &wrong) != MMS_OK);
    auto menu = x.Button("example.refresh.menu"); menu.kind = MMS_UI_MENU_ITEM; menu.parentId = S("mms.menu.plugins");
    MmsHandle menuHandle = 0; CHECK(x.ui.registerElement(x.ui.context, &menu, &menuHandle) == MMS_OK);
    // MENU_ITEM supports STATE once NativeUi publishes its real presentation.
    CHECK(x.api.getTarget(x.api.context, menuHandle, &wrong) == MMS_E_NOT_READY);
    auto group = x.Button("example.refresh.group"); group.kind = MMS_UI_GROUP; group.commandId = {}; group.capabilities |= MMS_UI_CONTAINER;
    MmsHandle groupHandle = 0; CHECK(x.ui.registerElement(x.ui.context, &group, &groupHandle) == MMS_OK);
    CHECK(x.api.getTarget(x.api.context, groupHandle, &wrong) == MMS_E_NOT_READY);
    CHECK(x.api.getTarget(x.api.context, x.registry.elements.at("mms.toolbar.main.extensions").handle, &wrong) == MMS_E_ARGUMENT);
    auto request = x.Request(target); MmsHandle handle = 0;
    CHECK(x.otherApi.request(x.otherApi.context, &request, &handle) != MMS_OK && handle == 0);
    for (uint32_t kinds : {MMS_UI_REFRESH_STATE, MMS_UI_REFRESH_LAYOUT}) {
        auto r = request; r.kinds = kinds; CHECK(x.api.request(x.api.context, &r, &handle) == MMS_E_UNSUPPORTED && handle == 0);
    }
    for (uint32_t kinds : {MMS_UI_REFRESH_DATA, MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_DATA}) {
        auto r = request; r.kinds = kinds; CHECK(x.api.request(x.api.context, &r, &handle) == MMS_E_ARGUMENT && handle == 0);
    }
    for (uint32_t scope : {MMS_UI_REFRESH_DEPENDENTS}) {
        auto r = request; r.scope = scope; CHECK(x.api.request(x.api.context, &r, &handle) == MMS_E_UNSUPPORTED && handle == 0);
    }
    for (unsigned change = 0; change < 9; ++change) {
        auto r = request;
        switch (change) { case 0: r.kinds = 16; break; case 1: r.scope = 8; break; case 2: r.flags = 1; break;
        case 3: r.reserved = 1; break; case 4: r.h.size = 1; break; case 5: r.target.h.major = 2; break;
        case 6: r.kinds = 0; break; case 7: r.binding = 1; break; default: r.sourceRevision = 1; break; }
        CHECK(x.api.request(x.api.context, &r, &handle) == MMS_E_ARGUMENT && handle == 0);
    }
    MmsUiDirtyRectV1 rect{1, 2, 3, 4};
    for (unsigned change = 0; change < 10; ++change) {
        auto bad = rect; auto r = x.Request(target, &bad, 1);
        switch (change) { case 0: bad.width = -1; break; case 1: bad.height = -1; break;
        case 2: bad.x = std::numeric_limits<float>::quiet_NaN(); break; case 3: bad.y = std::numeric_limits<float>::infinity(); break;
        case 4: bad.width = std::numeric_limits<float>::infinity(); break; case 5: bad.height = std::numeric_limits<float>::quiet_NaN(); break;
        case 6: r.dirtyRects.data = nullptr; break; case 7: r.dirtyRects.count = 33; break; case 8: r.dirtyRects.stride = 0; break; default: r.dirtyRects.recordVersion = 0; break; }
        CHECK(x.api.request(x.api.context, &r, &handle) == (change == 7 ? MMS_E_BUSY : MMS_E_ARGUMENT) && handle == 0);
    }
    auto noGeometry = x.Request(target, &rect, 1); noGeometry.expectedGeometryRevision = 0;
    CHECK(x.api.request(x.api.context, &noGeometry, &handle) != MMS_OK && handle == 0);
    CHECK(!x.queue.Pending() && Empty(x.button) && Empty(x.panel) && x.stateReads == 0 && x.invokes == 0);
    MmsResult capabilitiesWorker = MMS_OK, targetWorker = MMS_OK;
    std::thread worker([&] { capabilitiesWorker = x.api.getCapabilities(x.api.context, &caps); targetWorker = x.api.getTarget(x.api.context, x.element, &wrong); }); worker.join();
    CHECK(capabilitiesWorker == MMS_E_THREAD && targetWorker == MMS_E_THREAD);
    x.targetDuringInvoke = true;
    CHECK(x.ui.invoke(x.ui.context, S("example.refresh.command"), {}, {}) == MMS_OK && x.invokeTargetResult == MMS_OK);
}

static void ContributionsAndPaint() {
    Fixture x; auto target = x.Target(); const auto revision = x.registry.Revision();
    MmsUiDirtyRectV1 a{10, 10, 20, 20}, b{90, 40, 10, 10};
    auto first = x.Submit(x.Request(target, &a, 1)), second = x.Submit(x.Request(target, &b, 1));
    CHECK(x.State(first).status == MMS_UI_REFRESH_PENDING && x.State(second).status == MMS_UI_REFRESH_PENDING);
    std::thread wrongDrain([&] { x.queue.Drain(); }); wrongDrain.join();
    CHECK(x.State(first).status == MMS_UI_REFRESH_PENDING && Empty(x.button));
    MmsUiRefreshStateV1 foreignState{}; foreignState.h = Header<MmsUiRefreshStateV1>();
    CHECK(x.otherApi.query(x.otherApi.context, first, &foreignState) == MMS_E_NOT_FOUND);
    CHECK(x.otherApi.cancel(x.otherApi.context, first) == MMS_E_NOT_FOUND);
    CHECK(x.otherApi.release(x.otherApi.context, first) == MMS_E_NOT_FOUND);
    CHECK(x.api.cancel(x.api.context, first) == MMS_OK); x.queue.Drain();
    CHECK(x.State(first).status == MMS_UI_REFRESH_CANCELLED); x.Completed(second);
    Region(x.button, {{90, 40, 100, 50}}); CHECK(Empty(x.sibling) && Empty(x.panel) && Empty(x.parent));
    x.Clean();
    first = x.Submit(x.Request(target, &a, 1)); second = x.Submit(x.Request(target, &b, 1));
    CHECK(x.api.release(x.api.context, first) == MMS_OK);
    MmsUiRefreshStateV1 released{}; released.h = Header<MmsUiRefreshStateV1>();
    CHECK(x.api.query(x.api.context, first, &released) == MMS_E_NOT_FOUND);
    x.queue.Drain(); x.Completed(second); Region(x.button, {{10, 10, 30, 30}, {90, 40, 100, 50}}); x.Clean();
    // Worker calls copy the input; later edits to the caller's rectangle have no effect.
    std::array<MmsHandle, 8> handles{}; std::array<MmsResult, 8> results{}; std::vector<std::thread> workers;
    for (unsigned i = 0; i < handles.size(); ++i) workers.emplace_back([&, i] {
        MmsUiDirtyRectV1 local{float(i * 12), 2, 5, 6}; auto request = x.Request(target, &local, 1);
        results[i] = x.api.request(x.api.context, &request, &handles[i]); local = {0, 0, 300, 120};
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>();
        if (results[i] == MMS_OK) results[i] = x.api.query(x.api.context, handles[i], &state);
    });
    for (auto& thread : workers) thread.join();
    for (unsigned i = 0; i < handles.size(); ++i) { CHECK(results[i] == MMS_OK && handles[i]); x.tickets.push_back(handles[i]); }
    MmsResult cancelled = MMS_E_FAILED, releasedResult = MMS_E_FAILED;
    std::thread controls([&] { cancelled = x.api.cancel(x.api.context, handles[0]); releasedResult = x.api.release(x.api.context, handles[1]); }); controls.join();
    CHECK(cancelled == MMS_OK && releasedResult == MMS_OK); x.queue.Drain();
    Region(x.button, {{12,2,17,8},{24,2,29,8},{36,2,41,8},{48,2,53,8},{60,2,65,8},{72,2,77,8},{84,2,89,8}});
    CHECK(x.wakes.load() != 0 && x.registry.Revision() == revision && x.stateReads == 0 && x.invokes == 0); x.Clean();
    const auto panelTarget = x.Target(x.panelElement); const auto before = x.panelPaint.paints;
    auto panelTicket = x.Submit(x.Request(panelTarget, &a, 1)); x.queue.Drain(); x.Completed(panelTicket);
    CHECK(x.panelPaint.paints == before); Region(x.panel, {{10,10,30,30}});
    CHECK(Empty(x.button) && Empty(x.sibling) && Empty(x.parent));
    CHECK(UpdateWindow(x.panel)); CHECK(x.panelPaint.paints == before + 1);
    const RECT painted{10,10,30,30}; CHECK(EqualRect(&x.panelPaint.last, &painted) && Empty(x.panel));
    CHECK(x.creates == 1 && x.destroys == 0);
}

static void GeometryAndLifecycle() {
    Fixture x; auto old = x.Target(); MmsUiDirtyRectV1 rect{2, 3, 7, 5};
    auto staleGeometry = x.Submit(x.Request(old, &rect, 1));
    x.presentations[0].dpi = 144; x.presentations[0].width = 200; x.presentations[0].height = 80;
    x.presentations[0].x = 123; x.presentations[0].y = -27; x.queue.SyncTargets(x.presentations);
    auto current = x.Target(); CHECK(current.geometryRevision != old.geometryRevision && current.target.instanceGeneration == old.target.instanceGeneration);
    x.queue.Drain(); CHECK(x.State(staleGeometry).status == MMS_UI_REFRESH_FAILED && x.State(staleGeometry).result == MMS_E_STALE && Empty(x.button));
    MmsHandle bad = 0; auto request = x.Request(old, &rect, 1);
    CHECK(x.api.request(x.api.context, &request, &bad) == MMS_E_STALE && bad == 0);
    auto ticket = x.Submit(x.Request(current, &rect, 1)); x.queue.Drain(); x.Completed(ticket);
    Region(x.button, {{3,4,14,12}}); x.Clean(); // Local DIP, outward rounding, no parent offset.
    MmsUiDirtyRectV1 clipped{-2,-1,4,3}; ticket = x.Submit(x.Request(current, &clipped, 1));
    x.queue.Drain(); x.Completed(ticket); Region(x.button, {{0,0,3,3}}); x.Clean();
    MmsUiDirtyRectV1 empty{8,8,0,4}; ticket = x.Submit(x.Request(current, &empty, 1));
    x.queue.Drain(); x.Completed(ticket); CHECK(Empty(x.button));
    ticket = x.Submit(x.Request(current)); x.queue.Drain(); x.Completed(ticket);
    Region(x.button, {{0,0,300,120}}); x.Clean();
    auto fullWithoutGeometry = x.Request(current); fullWithoutGeometry.expectedGeometryRevision = 0;
    ticket = x.Submit(fullWithoutGeometry); x.presentations[0].x += 17; x.queue.SyncTargets(x.presentations);
    current = x.Target(); x.queue.Drain(); x.Completed(ticket); CHECK(x.State(ticket).geometryRevision == current.geometryRevision);
    Region(x.button, {{0,0,300,120}}); x.Clean();
    const auto stableGeometry = current.geometryRevision;
    x.queue.SyncTargets(x.presentations); CHECK(x.Target().geometryRevision == stableGeometry);
    ticket = x.Submit(x.Request(current, &rect, 1)); x.queue.SuspendTargets();
    CHECK(!x.Target().ready);
    x.queue.Drain(); CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_NOT_READY && Empty(x.button));
    request = x.Request(current); CHECK(x.api.request(x.api.context, &request, &bad) == MMS_E_NOT_READY && bad == 0);
    x.queue.SyncTargets(x.presentations); current = x.Target(); CHECK(current.ready && current.geometryRevision == stableGeometry);
    CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_NOT_READY);
    // Ordinary command refresh suspends and republishes unchanged layout before
    // the message pump drains. It must preserve this original local request.
    ticket = x.Submit(x.Request(current, &rect, 1)); x.queue.SuspendTargets(); x.queue.SyncTargets(x.presentations);
    CHECK(x.Target().geometryRevision == stableGeometry); x.queue.Drain(); x.Completed(ticket);
    Region(x.button, {{3,4,14,12}}); x.Clean();
    x.presentations[0].visible = false; ShowWindow(x.button, SW_HIDE); x.queue.SyncTargets(x.presentations); x.Clean();
    current = x.Target(); CHECK(current.ready && !current.visible);
    const auto paintsBeforeHidden = x.panelPaint.paints;
    ticket = x.Submit(x.Request(current, &rect, 1)); x.queue.Drain(); x.Completed(ticket);
    // USER32 may discard the update region of a genuinely hidden window.
    // Completion confirms invalidation submission, not a retained region/paint.
    CHECK(!IsWindowVisible(x.button) && x.panelPaint.paints == paintsBeforeHidden); x.Clean();
    ShowWindow(x.button, SW_SHOWNOACTIVATE); x.presentations[0].visible = true; x.queue.SyncTargets(x.presentations); x.Clean(); current = x.Target();
    auto oldInstance = current; ticket = x.Submit(x.Request(current, &rect, 1));
    ++x.presentations[0].instance; x.queue.SyncTargets(x.presentations); current = x.Target();
    CHECK(current.target.instanceGeneration != oldInstance.target.instanceGeneration);
    x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_STALE && Empty(x.button));
    request = x.Request(oldInstance); CHECK(x.api.request(x.api.context, &request, &bad) == MMS_E_STALE);
    ticket = x.Submit(x.Request(current, &rect, 1)); x.queue.AdvanceProjectEpoch();
    current = x.Target(); CHECK(current.target.projectEpoch != oldInstance.target.projectEpoch);
    x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_STALE && Empty(x.button));
    auto beforeUnregister = current; ticket = x.Submit(x.Request(current));
    CHECK(x.ui.unregisterItem(x.ui.context, x.element) == MMS_OK); x.queue.Drain();
    CHECK(x.State(ticket).result == MMS_E_STALE && Empty(x.button));
    auto replacement = x.Button("example.refresh.button"); MmsHandle replacementHandle = 0;
    CHECK(x.ui.registerElement(x.ui.context, &replacement, &replacementHandle) == MMS_OK && replacementHandle != x.element);
    x.element = replacementHandle; x.presentations[0].element = replacementHandle; ++x.presentations[0].instance;
    x.queue.SyncTargets(x.presentations); current = x.Target();
    request = x.Request(beforeUnregister); CHECK(x.api.request(x.api.context, &request, &bad) != MMS_OK && bad == 0);
    ticket = x.Submit(x.Request(current)); CHECK(x.queue.HasLeases(x.client)); x.queue.Retire(x.client);
    CHECK(x.State(ticket).status == MMS_UI_REFRESH_CANCELLED && x.State(ticket).result == MMS_E_CANCELLED);
    request = x.Request(current); CHECK(x.api.request(x.api.context, &request, &bad) == MMS_E_NOT_READY);
    x.queue.Drain(); CHECK(Empty(x.button));
    for (auto h : x.tickets) x.api.release(x.api.context, h);
    CHECK(!x.queue.HasLeases(x.client));
}

static void WakeAndUnavailable() {
    Fixture failed(true); const auto target = failed.Target(); const auto request = failed.Request(target);
    for (unsigned i = 0; i < 300; ++i) {
        MmsHandle ticket = 0; CHECK(failed.api.request(failed.api.context, &request, &ticket) == MMS_E_FAILED && ticket == 0);
    }
    CHECK(!failed.queue.HasLeases(failed.client) && !failed.queue.Pending() && Empty(failed.button));
    Fixture unavailable; auto e = unavailable.Button("example.refresh.unmounted"); MmsHandle handle = 0;
    CHECK(unavailable.ui.registerElement(unavailable.ui.context, &e, &handle) == MMS_OK);
    MmsUiRefreshTargetV1 out{}; out.h = Header<MmsUiRefreshTargetV1>();
    CHECK(unavailable.api.getTarget(unavailable.api.context, handle, &out) == MMS_E_NOT_READY);
    auto old = unavailable.Target(); unavailable.queue.SyncTargets({});
    auto stale = unavailable.Request(old); MmsHandle ticket = 0;
    CHECK(unavailable.api.request(unavailable.api.context, &stale, &ticket) != MMS_OK && ticket == 0);
    CHECK(Empty(unavailable.button) && !unavailable.queue.Pending());
}

static void QuotasAndDrainBudget() {
    Fixture x; const auto target = x.Target(); const auto request = x.Request(target);
    std::vector<MmsHandle> held;
    for (unsigned i = 0; i < 256; ++i) { const auto h = x.Submit(request); held.push_back(h); CHECK(x.api.release(x.api.context, h) == MMS_OK); }
    MmsHandle rejected = 0; CHECK(x.api.request(x.api.context, &request, &rejected) == MMS_E_BUSY && rejected == 0);
    CHECK(x.queue.HasLeases(x.client)); x.queue.Drain();
    // Released pending contributions keep their quota until actually drained.
    for (unsigned i = 0; i < 64; ++i) held.push_back(x.Submit(request));
    CHECK(x.api.request(x.api.context, &request, &rejected) == MMS_E_BUSY && rejected == 0);
    for (unsigned i = 0; i < 4; ++i) x.queue.Drain();
    for (size_t i = 256; i < held.size(); ++i) { x.Completed(held[i]); CHECK(x.api.release(x.api.context, held[i]) == MMS_OK); }
    CHECK(!x.queue.HasLeases(x.client)); x.Clean();
    held.clear(); for (unsigned i = 0; i < 65; ++i) held.push_back(x.Submit(request));
    x.queue.Drain(); unsigned completed = 0, pending = 0;
    for (auto h : held) { auto s = x.State(h); completed += s.status == MMS_UI_REFRESH_COMPLETED; pending += s.status == MMS_UI_REFRESH_PENDING; }
    CHECK(completed == 64 && pending == 1 && x.queue.Pending()); x.queue.Drain();
    for (auto h : held) x.Completed(h);
    CHECK(!x.queue.Pending() && x.queue.HasLeases(x.client));
    for (auto h : held) CHECK(x.api.release(x.api.context, h) == MMS_OK);
    CHECK(!x.queue.HasLeases(x.client));
    held.clear(); for (unsigned i = 0; i < 256; ++i) held.push_back(x.Submit(request));
    for (unsigned i = 0; i < 4; ++i) x.queue.Drain();
    CHECK(x.api.request(x.api.context, &request, &rejected) == MMS_E_BUSY);
    CHECK(x.api.release(x.api.context, held.back()) == MMS_OK); held.pop_back();
    auto replacement = x.Submit(request); CHECK(x.api.cancel(x.api.context, replacement) == MMS_OK);
    CHECK(x.api.request(x.api.context, &request, &rejected) == MMS_E_BUSY); // Terminal-but-unreleased still owns a slot.
    CHECK(x.api.release(x.api.context, replacement) == MMS_OK);
    for (auto h : held) CHECK(x.api.release(x.api.context, h) == MMS_OK);
    CHECK(!x.queue.HasLeases(x.client));
    std::array<MmsUiDirtyRectV1, 32> rects{};
    for (unsigned i = 0; i < rects.size(); ++i) rects[i] = {float(i * 3), 0, 2, 2};
    const auto maximumRects = x.Request(target, rects.data(), rects.size());
    held.clear(); for (unsigned i = 0; i < 256; ++i) held.push_back(x.Submit(maximumRects));
    CHECK(x.api.request(x.api.context, &maximumRects, &rejected) == MMS_E_BUSY);
    for (auto h : held) { CHECK(x.api.cancel(x.api.context, h) == MMS_OK); CHECK(x.api.release(x.api.context, h) == MMS_OK); }
    CHECK(!x.queue.HasLeases(x.client) && !x.queue.Pending());
}

static void GlobalQuota() {
    Fixture x;
    struct Extra {
        Fixture& fixture;
        UiClient uiClient;
        RefreshClient client;
        MmsUiRefreshApiV1 api;
        HWND window = nullptr;
        MmsHandle element = 0;
        std::vector<MmsHandle> tickets;
        Extra(Fixture& f, unsigned index) : fixture(f), uiClient{&f.registry, "example.refreshquota" + std::to_string(index)},
            client{&f.queue, uiClient.id}, api(f.queue.Api(client)) {
            auto ui = fixture.registry.Api(uiClient);
            const auto id = uiClient.id + ".button"; auto d = fixture.Button(id.c_str()); d.ownerPlugin = Text(uiClient.id);
            CHECK(ui.registerElement(ui.context, &d, &element) == MMS_OK);
            window = CreateWindowExW(0, L"BUTTON", L"quota", WS_CHILD | WS_VISIBLE, 5 + int(index) * 15, 430, 10, 10,
                fixture.parent, nullptr, GetModuleHandleW(nullptr), nullptr); CHECK(window != nullptr);
            fixture.presentations.push_back({element, uiClient.id, 100 + index, window, 96, 0, 0, 10, 10, true});
        }
        ~Extra() {
            fixture.queue.Retire(client); for (auto h : tickets) api.release(api.context, h);
            fixture.registry.Retire(uiClient); if (window) DestroyWindow(window);
        }
        MmsUiRefreshRequestV1 Request() {
            MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
            CHECK(api.getTarget(api.context, element, &target) == MMS_OK); return fixture.Request(target);
        }
    };
    std::vector<std::unique_ptr<Extra>> owners;
    for (unsigned i = 0; i < 17; ++i) owners.push_back(std::make_unique<Extra>(x, i));
    x.queue.SyncTargets(x.presentations);
    for (unsigned i = 0; i < 16; ++i) {
        auto& owner = *owners[i]; const auto request = owner.Request();
        for (unsigned n = 0; n < 256; ++n) {
            MmsHandle ticket = 0; CHECK(owner.api.request(owner.api.context, &request, &ticket) == MMS_OK && ticket != 0);
            owner.tickets.push_back(ticket);
        }
    }
    auto& last = *owners.back(); auto request = last.Request(); MmsHandle rejected = 0;
    CHECK(last.api.request(last.api.context, &request, &rejected) == MMS_E_BUSY && rejected == 0);
    auto& first = *owners.front(); const auto releasedPending = first.tickets.back();
    CHECK(first.api.release(first.api.context, releasedPending) == MMS_OK);
    CHECK(last.api.request(last.api.context, &request, &rejected) == MMS_E_BUSY); // Global pending survives release, too.
    for (unsigned i = 0; i < 4; ++i) x.queue.Drain();
    MmsHandle replacement = 0; CHECK(last.api.request(last.api.context, &request, &replacement) == MMS_OK && replacement != 0);
    last.tickets.push_back(replacement);
    CHECK(last.api.request(last.api.context, &request, &rejected) == MMS_E_BUSY);
    owners.clear(); CHECK(!x.queue.Pending());
}
} // namespace refresh_checks

static void RefreshChecks() {
    refresh_checks::CapabilitiesAndValidation();
    refresh_checks::ContributionsAndPaint();
    refresh_checks::GeometryAndLifecycle();
    refresh_checks::WakeAndUnavailable();
    refresh_checks::QuotasAndDrainBudget();
    refresh_checks::GlobalQuota();
}
