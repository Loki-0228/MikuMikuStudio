struct SurfaceProbe {
    int creates = 0, destroys = 0, resizes = 0, shows = 0, focuses = 0;
    bool fail = false, wrongParent = false, visible = false;
    MmsUiRectV1 bounds{}; uint32_t dpi = 0;
    struct View { SurfaceProbe* owner; HWND window; };
    MmsUiSurfaceDescV1 Descriptor() {
        MmsUiSurfaceDescV1 d{}; d.h = Header<MmsUiSurfaceDescV1>(); d.id = S("example.panel.factory"); d.context = this;
        d.minimumWidth = 80; d.minimumHeight = 40; d.preferredWidth = 180; d.preferredHeight = 100;
        d.createView = [](void* p, const MmsUiMountV1* mount, void** out, uintptr_t* native) -> MmsResult {
            auto& s = *static_cast<SurfaceProbe*>(p); ++s.creates; *out = nullptr; *native = 0;
            if (s.fail) return MMS_E_FAILED;
            HWND window = CreateWindowExW(0, L"STATIC", L"面板测试", s.wrongParent ? WS_OVERLAPPED : WS_CHILD,
                mount->bounds.x, mount->bounds.y, mount->bounds.width, mount->bounds.height,
                s.wrongParent ? nullptr : reinterpret_cast<HWND>(mount->parentWindow), nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window) return MMS_E_FAILED;
            auto view = new View{&s, window}; *out = view; *native = reinterpret_cast<uintptr_t>(window);
            s.bounds = mount->bounds; s.dpi = mount->dpi; return MMS_OK;
        };
        d.resizeView = [](void* p, const MmsUiRectV1* rect, uint32_t dpi) { auto& v = *static_cast<View*>(p); ++v.owner->resizes; v.owner->bounds = *rect; v.owner->dpi = dpi; MoveWindow(v.window, rect->x, rect->y, rect->width, rect->height, TRUE); };
        d.showView = [](void* p, uint32_t visible) { auto& v = *static_cast<View*>(p); ++v.owner->shows; v.owner->visible = visible != 0; ShowWindow(v.window, visible ? SW_SHOWNA : SW_HIDE); };
        d.focusView = [](void* p) -> MmsResult { auto& v = *static_cast<View*>(p); ++v.owner->focuses; SetFocus(v.window); return MMS_OK; };
        d.destroyView = [](void* p) { auto v = static_cast<View*>(p); ++v->owner->destroys; DestroyWindow(v->window); delete v; };
        return d;
    }
};
static void SurfaceChecks() {
    UiRegistry registry; UiClient owner{&registry, "example.panel"}, foreign{&registry, "example.foreign"};
    auto api = registry.Api(owner); auto surfaces = registry.SurfaceApi(owner); SurfaceProbe probe; auto d = probe.Descriptor();
    MmsHandle factory = 0, panel = 0, second = 0;
    CHECK(surfaces.registerSurface(surfaces.context, &d, &factory) == MMS_OK);
    MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = S("example.panel.view"); e.ownerPlugin = S("example.panel"); e.label = S("测试面板");
    e.parentId = S("mms.workspace.tabs.extensions"); e.surfaceId = d.id; e.kind = MMS_UI_PANEL; e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE;
    CHECK(api.registerElement(api.context, &e, &panel) == MMS_OK);
    e.id = S("example.panel.second"); CHECK(api.registerElement(api.context, &e, &second) == MMS_E_BUSY);
    CHECK(surfaces.unregisterSurface(surfaces.context, factory) == MMS_E_BUSY);
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND other = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(parent && other);
    MmsUiRectV1 rect{1, 2, 180, 100};
    CHECK(registry.ShowSurface("example.panel.view", parent, rect, 96, false) == MMS_OK && probe.creates == 0);
    probe.fail = true; CHECK(registry.ShowSurface("example.panel.view", parent, rect, 96, true) == MMS_E_FAILED && probe.destroys == 0);
    probe.fail = false; probe.wrongParent = true;
    CHECK(registry.ShowSurface("example.panel.view", parent, rect, 96, true) == MMS_E_FAILED && probe.destroys == 1);
    probe.wrongParent = false;
    CHECK(registry.ShowSurface("example.panel.view", parent, rect, 96, true) == MMS_OK && probe.creates == 3 && probe.visible);
    CHECK(registry.ShowSurface("example.panel.view", parent, rect, 96, true) == MMS_OK && probe.creates == 3 && probe.resizes == 0 && probe.shows == 1);
    CHECK(registry.ShowSurface("example.panel.view", parent, rect, 96, false) == MMS_OK && !probe.visible);
    rect.width = 240; CHECK(registry.ShowSurface("example.panel.view", parent, rect, 144, false) == MMS_OK && probe.resizes == 1 && probe.dpi == 144 && !probe.visible);
    CHECK(registry.ShowSurface("example.panel.view", other, rect, 144, true) == MMS_OK && probe.creates == 4 && probe.destroys == 2);
    CHECK(registry.FocusSurface("example.panel.view") == MMS_OK && probe.focuses == 1);
    MmsResult threadResult = MMS_OK; std::thread worker([&] { threadResult = registry.ShowSurface("example.panel.view", other, rect, 144, true); }); worker.join(); CHECK(threadResult == MMS_E_THREAD);
    owner.stopping = true; CHECK(registry.FocusSurface("example.panel.view") == MMS_E_NOT_READY);
    CHECK(api.unregisterItem(api.context, panel) == MMS_OK && probe.destroys == 3);
    CHECK(surfaces.unregisterSurface(surfaces.context, factory) == MMS_OK);
    registry.DestroyViews(); CHECK(probe.destroys == 3 && !owner.unsafeToUnload);
    owner.stopping = false; d.flags = MMS_SURFACE_MULTIPLE_VIEWS;
    CHECK(surfaces.registerSurface(surfaces.context, &d, &factory) == MMS_OK);
    e.id = S("example.panel.first"); CHECK(api.registerElement(api.context, &e, &panel) == MMS_OK);
    e.id = S("example.panel.second"); CHECK(api.registerElement(api.context, &e, &second) == MMS_OK);
    { NativeUi ui(registry, parent, {}); ui.Refresh(); CHECK(ui.Layout(0, 200, 120, true) > 0); CHECK(probe.creates == 5); CHECK(ui.Layout(0, 100, 80, false) == 0 && !probe.visible); }
    CHECK(probe.destroys == 4); registry.Retire(owner); CHECK(registry.surfaces.empty());
    DestroyWindow(parent); DestroyWindow(other);
}
