namespace foreign_dpi_layout_checks {
struct Functions {
    using GetThread = HANDLE(WINAPI*)();
    using GetWindow = HANDLE(WINAPI*)(HWND);
    using SetThread = HANDLE(WINAPI*)(HANDLE);
    using Equal = BOOL(WINAPI*)(HANDLE, HANDLE);
    GetThread thread = nullptr; GetWindow window = nullptr; SetThread set = nullptr; Equal equal = nullptr;
    Functions() {
        const auto module = GetModuleHandleW(L"user32.dll");
        thread = reinterpret_cast<GetThread>(GetProcAddress(module, "GetThreadDpiAwarenessContext"));
        window = reinterpret_cast<GetWindow>(GetProcAddress(module, "GetWindowDpiAwarenessContext"));
        set = reinterpret_cast<SetThread>(GetProcAddress(module, "SetThreadDpiAwarenessContext"));
        equal = reinterpret_cast<Equal>(GetProcAddress(module, "AreDpiAwarenessContextsEqual"));
    }
};
struct Context {
    Functions::SetThread set; HANDLE previous;
    Context(const Functions& api, HANDLE desired): set(api.set), previous(set(desired)) { CHECK(previous); }
    Context(Functions::SetThread function, HANDLE saved): set(function), previous(saved) {}
    ~Context() { if (previous) set(previous); }
};
struct Bounds { HWND window, parent; RECT outer{}, client{}; };
static Bounds Actual(const Functions& api, HWND window) {
    CHECK(IsWindow(window)); const HWND parent = GetParent(window);
    const auto dpi = api.window(parent ? parent : window); CHECK(dpi);
    Context scope(api, dpi);
    Bounds result{window, parent}; CHECK(GetWindowRect(window, &result.outer)); CHECK(GetClientRect(window, &result.client));
    if (parent) {
        SetLastError(ERROR_SUCCESS);
        const auto mapped = MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&result.outer), 2);
        CHECK(mapped || GetLastError() == ERROR_SUCCESS);
    }
    return result;
}
static void Gather(HWND parent, std::vector<HWND>& out) {
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        out.push_back(child); Gather(child, out);
    }
}
static void Check(NativeUi& native, HWND host, const MmsUiLayoutApiV1& layout, int expectedHeight) {
    Functions api;
    if (!api.thread || !api.window || !api.set || !api.equal) {
        std::cout << "LAYOUT_FOREIGN_DPI unavailable=thread_context_api\n"; return;
    }
    const auto original = api.thread(), parentDpi = api.window(host); CHECK(original && parentDpi);
    HANDLE foreign = nullptr;
    for (const auto value : {INT_PTR(-4), INT_PTR(-1)}) {
        const auto requested = reinterpret_cast<HANDLE>(value);
        if (api.equal(requested, parentDpi)) continue;
        const auto previous = api.set(requested);
        if (!previous) { CHECK(api.equal(api.thread(), original)); continue; }
        { Context restore(api.set, previous);
            const auto actual = api.thread(); CHECK(actual && api.equal(actual, requested)); foreign = actual;
        }
        CHECK(api.equal(api.thread(), original)); break;
    }
    if (!foreign) { std::cout << "LAYOUT_FOREIGN_DPI unavailable=different_supported_context\n"; return; }
    CHECK(!api.equal(foreign, parentDpi));
    std::vector<HWND> windows{host}; Gather(host, windows); CHECK(windows.size() >= 4);
    std::vector<Bounds> baseline; baseline.reserve(windows.size());
    for (HWND window : windows) baseline.push_back(Actual(api, window));
    MmsUiLayoutSnapshotV1 before{}; before.h = Header<MmsUiLayoutSnapshotV1>();
    CHECK(layout.acquireSnapshot(layout.context, &before) == MMS_OK);
    struct Lease { MmsUiLayoutApiV1 api; MmsHandle value; ~Lease() { api.releaseSnapshot(api.context, value); } } old{layout, before.handle};
    {
        Context scope(api, foreign); CHECK(api.equal(api.thread(), foreign));
        CHECK(native.Layout(100, 400, 180, true) == expectedHeight);
        CHECK(api.equal(api.thread(), foreign));
        for (const auto& saved : baseline) {
            const auto actual = Actual(api, saved.window);
            CHECK(actual.parent == saved.parent && EqualRect(&actual.outer, &saved.outer) && EqualRect(&actual.client, &saved.client));
            CHECK(api.equal(api.thread(), foreign));
        }
        MmsUiLayoutSnapshotV1 after{}; after.h = Header<MmsUiLayoutSnapshotV1>();
        CHECK(layout.acquireSnapshot(layout.context, &after) == MMS_OK); Lease current{layout, after.handle};
        CHECK(after.geometryRevision == before.geometryRevision && after.uiRevision == before.uiRevision && after.items.count == before.items.count);
        for (uint64_t i = 0; i < before.items.count; ++i) {
            const auto& first = *reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(before.items.data) + i * before.items.stride);
            const auto& last = *reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(after.items.data) + i * after.items.stride);
            CHECK(String(first.elementId) == String(last.elementId) && !std::memcmp(first.rect, last.rect, sizeof(first.rect)) &&
                first.visible == last.visible && first.overflowed == last.overflowed);
        }
        CHECK(api.equal(api.thread(), foreign));
    }
    CHECK(api.equal(api.thread(), original));
    std::cout << "LAYOUT_FOREIGN_DPI actual_windows=" << baseline.size() << " revisions=unchanged caller_context=preserved\n";
}
}
static void TreeLayoutChecks() {
    using namespace native_ui;
    Node root; root.id = "root"; root.hints.flow = Flow::Row;
    Node a; a.id = "a"; a.measuredMinimum = {20, 24}; a.measuredPreferred = {60, 24}; a.hints.grow.width = a.hints.shrink.width = 1;
    Node b = a; b.id = "b"; a.hints.maximum.width = 80; root.children = {a, b};
    auto result = Arrange(root, {212, 0}); CHECK(result.items[1].rect.width == 80 && result.items[2].rect.width == 120);
    result = Arrange(root, {92, 0}); CHECK(result.items[1].rect.width == 40 && result.items[2].rect.width == 40);
    result = Arrange(root, {32, 0}); CHECK(result.items[1].rect.width == 20 && result.items[2].overflowed);
    root.hints.flow = Flow::Wrap; root.children[0].hints.shrink.width = root.children[1].hints.shrink.width = 0;
    result = Arrange(root, {90, 0}); CHECK(result.items[2].rect.y > result.items[1].rect.y);
    root.children[0].visible = false; result = Arrange(root, {140, 0}); CHECK(result.items[1].rect.width == 0 && result.items[2].rect.x == 4);
    root.children[0].hints.preserveHidden = true; result = Arrange(root, {140, 0}); CHECK(!result.items[1].visible && result.items[1].rect.width > 0 && result.items[2].rect.x > 4);
    root.children[0].visible = true; root.hints.flow = Flow::Grid; root.hints.columns = 3; root.children.push_back(b); root.children.back().id = "c";
    result = Arrange(root, {80, 0}); CHECK(result.items[1].rect.y == result.items[3].rect.y && result.items[1].rect.width >= 20);
    auto original = Arrange(root, {220, 0}); Arrange(root, {40, 0}); auto restored = Arrange(root, {220, 0}); CHECK(original.items[3].rect.x == restored.items[3].rect.x);
    root.children[0].hints.minimum.width = 90; CHECK(Throws([&] { Arrange(root, {200, 0}); }));
    Node column; column.id = "column"; column.hints.flow = Flow::Column; column.hints.align = 3;
    for (float& p : column.hints.padding) p = 0;
    a.hints.maximum.width = 0; a.measuredPreferred = {200, 24}; column.children = {a};
    CHECK(Arrange(column, {100, 0}).items[1].rect.width == 100);
    column.hints.flow = Flow::Row; column.children[0].measuredPreferred = {20, 200}; column.children[0].hints.shrink.height = 1;
    CHECK(Arrange(column, {100, 100}).items[1].rect.height == 100);
    column.hints.flow = Flow::Column; column.hints.align = 3; column.hints.gap[0] = column.hints.gap[1] = 0;
    Node group; group.id = "group"; group.hints.flow = Flow::Wrap; group.hints.maximum.width = 100;
    for (float& p : group.hints.padding) p = 0; group.hints.gap[0] = group.hints.gap[1] = 0;
    a.measuredMinimum = a.measuredPreferred = {60, 20}; a.hints = {}; b = a; b.id = "b"; group.children = {a, b};
    column.children = {group, a}; result = Arrange(column, {100, 0});
    CHECK(result.items[1].rect.height == 40 && result.items[4].rect.y >= result.items[3].rect.y + result.items[3].rect.height);
}
static void LayoutServiceChecks() {
    UiRegistry registry; UiClient c{&registry, "example.layout"}, other{&registry, "example.editor"}; auto api = registry.Api(c); auto layout = registry.LayoutApi(c);
    MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>(); command.info.id = S("example.layout.run"); command.info.ownerPlugin = S("example.layout");
    command.getState = [](void*, MmsObjectRef, MmsCommandStateV1* s) -> MmsResult { s->enabled = s->visible = 1; return MMS_OK; };
    command.invoke = [](void*, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { return MMS_OK; };
    MmsHandle handle; CHECK(api.registerCommand(api.context, &command, &handle) == MMS_OK);
    MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.ownerPlugin = command.info.ownerPlugin; e.parentId = S("mms.toolbar.main.extensions"); e.commandId = command.info.id;
    e.kind = MMS_UI_BUTTON; e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_LAYOUT; e.label = S("布局按钮");
    for (const char* id : {"example.layout.a", "example.layout.b", "example.layout.c"}) { e.id = S(id); CHECK(api.registerElement(api.context, &e, &handle) == MMS_OK); }
    MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.minimum[0] = 60; hints.preferred[0] = 100; hints.preferred[1] = 24;
    CHECK(layout.setDefaultHints(layout.context, S("example.layout.a"), &hints) == MMS_OK);
    auto foreign = registry.LayoutApi(other); CHECK(foreign.setDefaultHints(foreign.context, S("example.layout.a"), &hints) == MMS_E_ARGUMENT);
    auto bad = hints; bad.maximum[0] = 20; CHECK(layout.setDefaultHints(layout.context, S("example.layout.a"), &bad) == MMS_E_ARGUMENT);
    MmsUiLayoutSnapshotV1 old{}; old.h = Header<MmsUiLayoutSnapshotV1>(); CHECK(layout.acquireSnapshot(layout.context, &old) == MMS_E_NOT_READY);
    HWND window = CreateWindowExW(0, L"STATIC", L"布局验证", WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr); CHECK(window != nullptr);
    {
        NativeUi ui(registry, window, {}); ui.Refresh(); const auto reservedHeight = ui.Layout(100, 400, 180, true);
        CHECK(layout.acquireSnapshot(layout.context, &old) == MMS_OK && old.items.count == 5);
        auto geometryRevision = old.geometryRevision; auto uiRevision = old.uiRevision;
        MmsUiLayoutSnapshotV1 current{}; current.h = Header<MmsUiLayoutSnapshotV1>(); ui.Layout(100, 400, 180, true);
        CHECK(layout.acquireSnapshot(layout.context, &current) == MMS_OK && current.geometryRevision == geometryRevision); layout.releaseSnapshot(layout.context, current.handle);
        foreign_dpi_layout_checks::Check(ui, window, layout, reservedHeight);
        ui.Layout(100, 130, 180, true); CHECK(layout.acquireSnapshot(layout.context, &current) == MMS_OK && current.geometryRevision > geometryRevision && current.uiRevision == uiRevision); layout.releaseSnapshot(layout.context, current.handle);
        CHECK(old.geometryRevision == geometryRevision && old.uiRevision == uiRevision);
        MmsHandle transaction; CHECK(api.beginLayout(api.context, registry.Revision(), &transaction) == MMS_OK);
        hints.preferred[0] = 180;
        CHECK(layout.stageHints(layout.context, transaction, S("example.layout.a"), registry.elements.at("example.layout.a").desc.generation, &hints) == MMS_OK);
        CHECK(api.commitLayout(api.context, transaction) == MMS_OK);
        CHECK(layout.acquireSnapshot(layout.context, &current) == MMS_E_NOT_READY);
        hints.preferred[0] = 120; CHECK(layout.setDefaultHints(layout.context, S("example.layout.a"), &hints) == MMS_OK);
        CHECK(registry.elements.at("example.layout.a").effectiveHints.preferred[0] == 180);
        ui.Refresh(); CHECK(layout.acquireSnapshot(layout.context, &current) == MMS_OK); layout.releaseSnapshot(layout.context, current.handle);
        CHECK(api.beginLayout(api.context, registry.Revision(), &transaction) == MMS_OK);
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_MOVE; op.elementId = S("example.layout.c"); op.parentId = e.parentId; op.beforeId = S("example.layout.a"); op.expectedGeneration = registry.elements.at("example.layout.c").desc.generation;
        CHECK(api.applyLayout(api.context, transaction, &op) == MMS_OK); CHECK(api.commitLayout(api.context, transaction) == MMS_OK);
        CHECK(registry.elements.at("example.layout.c").effectiveHints.order < registry.elements.at("example.layout.a").effectiveHints.order);
        CHECK(api.beginLayout(api.context, registry.Revision(), &transaction) == MMS_OK);
        op = {}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_CREATE_GROUP; op.elementId = S("example.layout.group"); op.parentId = e.parentId; op.value.groupLabel = S("分组");
        CHECK(api.applyLayout(api.context, transaction, &op) == MMS_OK); CHECK(api.commitLayout(api.context, transaction) == MMS_OK);
        CHECK(api.beginLayout(api.context, registry.Revision(), &transaction) == MMS_OK);
        op.operation = MMS_LAYOUT_MOVE; op.elementId = S("example.layout.a"); op.parentId = S("example.layout.group"); op.value = {}; op.expectedGeneration = registry.elements.at("example.layout.a").desc.generation;
        CHECK(api.applyLayout(api.context, transaction, &op) == MMS_OK); CHECK(api.commitLayout(api.context, transaction) == MMS_OK);
        CHECK(api.beginLayout(api.context, registry.Revision(), &transaction) == MMS_OK);
        op.operation = MMS_LAYOUT_REMOVE_GROUP; op.elementId = S("example.layout.group"); op.parentId = {}; op.expectedGeneration = registry.elements.at("example.layout.group").desc.generation;
        CHECK(api.applyLayout(api.context, transaction, &op) == MMS_OK); CHECK(api.commitLayout(api.context, transaction) == MMS_E_BUSY); CHECK(registry.elements.count("example.layout.group") == 1);
        ui.Refresh(); CHECK(layout.acquireSnapshot(layout.context, &current) == MMS_OK); layout.releaseSnapshot(layout.context, current.handle);
        layout.releaseSnapshot(layout.context, old.handle);
    }
    registry.Retire(c); DestroyWindow(window); CHECK(!registry.HasLeases(c));
}
