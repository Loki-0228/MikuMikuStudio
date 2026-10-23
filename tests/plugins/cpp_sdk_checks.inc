#include <mms/cpp/ui.hpp>
#include <mms/cpp/render.hpp>
#include <mms/cpp/layout_history.hpp>
#include <mms/cpp/layout.hpp>
#include <set>
#include <type_traits>

namespace cpp_sdk_checks {
namespace sdk = mms::sdk;

// These tables forward to the production registry. Only the selected failure
// point is synthetic; registration, dependencies, handles and leases are real.
struct Host {
    UiRegistry registry;
    UiClient client{&registry, "example.cppsdk"};
    MmsUiApiV1 raw = registry.Api(client), ui{};
    MmsUiSurfaceApiV1 rawSurface = registry.SurfaceApi(client), surface{};
    MmsUiLayoutApiV1 rawLayout = registry.LayoutApi(client), layout{};
    MmsHostV1 host{};
    MmsRenderApiV1 render{};
    MmsUiLayoutApiV1_1 layoutHistory{};
    MmsUiLayoutHistoryInfoV1 historyInfo{};
    unsigned historyQueries = 0, historyInfoCalls = 0, historyUndo = 0, historyRedo = 0;
    bool historyArguments = true;
    MmsResult historyResult = MMS_OK;
    bool missingHistoryFunction = false;
    MmsRenderInfoV1 renderInfo{};
    unsigned queries = 0, surfaceQueries = 0, layoutQueries = 0;
    unsigned renderQueries = 0, acquireCalls = 0, releaseCalls = 0;
    MmsHandle nextLease = 41, releasedLease = 0;
    MmsResult renderResult = MMS_OK;
    bool missingRenderFunction = false;
    unsigned registrations = 0, failRegistration = 0, invokes = 0, providerReleases = 0;
    MmsResult queryResult = MMS_OK, surfaceResult = MMS_OK, layoutResult = MMS_OK;
    MmsResult removeResult = MMS_OK, hintsResult = MMS_OK, callbackCloseResult = MMS_OK;
    bool badHeaderSize = false, badHeaderVersion = false, missingFunction = false, futureHeader = false;
    bool queryArguments = true;
    MmsHandle blocked = 0;
    sdk::RegistrationBundle* closeOnInvoke = nullptr;
    std::vector<MmsHandle> added, removed, attempted;
    std::string text = "provider text", reason = "reason from callback";

    Host() {
        ui = raw; ui.context = this;
        ui.registerCommand = [](void* p, const MmsCommandDescV1* d, MmsHandle* h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); if (x.FailRegistration()) return MMS_E_FAILED;
            const auto r = x.raw.registerCommand(x.raw.context, d, h); if (!r) x.added.push_back(*h); return r;
        };
        ui.registerElement = [](void* p, const MmsUiElementV1* d, MmsHandle* h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); if (x.FailRegistration()) return MMS_E_FAILED;
            const auto r = x.raw.registerElement(x.raw.context, d, h); if (!r) x.added.push_back(*h); return r;
        };
        ui.registerProperty = [](void* p, const MmsPropertyDescV1* d, MmsHandle* h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); if (x.FailRegistration()) return MMS_E_FAILED;
            const auto r = x.raw.registerProperty(x.raw.context, d, h); if (!r) x.added.push_back(*h); return r;
        };
        ui.updateCommand = [](void* p, MmsHandle h, const MmsCommandDescV1* d) { auto& x = *static_cast<Host*>(p); return x.raw.updateCommand(x.raw.context, h, d); };
        ui.updateElement = [](void* p, MmsHandle h, const MmsUiElementV1* d) { auto& x = *static_cast<Host*>(p); return x.raw.updateElement(x.raw.context, h, d); };
        ui.updateProperty = [](void* p, MmsHandle h, const MmsPropertyDescV1* d) { auto& x = *static_cast<Host*>(p); return x.raw.updateProperty(x.raw.context, h, d); };
        ui.unregisterItem = [](void* p, MmsHandle h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); x.attempted.push_back(h);
            if (x.removeResult != MMS_OK && (!x.blocked || h == x.blocked)) return x.removeResult;
            const auto r = x.raw.unregisterItem(x.raw.context, h); if (!r) x.removed.push_back(h); return r;
        };
        ui.acquireSnapshot = [](void* p, uint32_t f, MmsUiSnapshotV1* s) { auto& x = *static_cast<Host*>(p); return x.raw.acquireSnapshot(x.raw.context, f, s); };
        ui.releaseSnapshot = [](void* p, MmsHandle h) { auto& x = *static_cast<Host*>(p); x.raw.releaseSnapshot(x.raw.context, h); };
        ui.getCommandState = [](void* p, MmsString id, MmsObjectRef o, MmsCommandStateV1* s, MmsHandle* h) { auto& x = *static_cast<Host*>(p); return x.raw.getCommandState(x.raw.context, id, o, s, h); };
        ui.invoke = [](void* p, MmsString id, MmsObjectRef o, MmsSpan a) { auto& x = *static_cast<Host*>(p); return x.raw.invoke(x.raw.context, id, o, a); };
        ui.readProperty = [](void* p, MmsString id, MmsObjectRef o, MmsValue* v, MmsHandle* h) { auto& x = *static_cast<Host*>(p); return x.raw.readProperty(x.raw.context, id, o, v, h); };
        ui.releaseValue = [](void* p, MmsHandle h) { auto& x = *static_cast<Host*>(p); x.raw.releaseValue(x.raw.context, h); };
        ui.stageProperty = [](void* p, MmsHandle t, MmsString id, MmsObjectRef o, const MmsValue* v) { auto& x = *static_cast<Host*>(p); return x.raw.stageProperty(x.raw.context, t, id, o, v); };
        ui.beginLayout = [](void* p, uint64_t r, MmsHandle* h) { auto& x = *static_cast<Host*>(p); return x.raw.beginLayout(x.raw.context, r, h); };
        ui.applyLayout = [](void* p, MmsHandle h, const MmsLayoutOpV1* op) { auto& x = *static_cast<Host*>(p); return x.raw.applyLayout(x.raw.context, h, op); };
        ui.commitLayout = [](void* p, MmsHandle h) { auto& x = *static_cast<Host*>(p); return x.raw.commitLayout(x.raw.context, h); };
        ui.cancelLayout = [](void* p, MmsHandle h) { auto& x = *static_cast<Host*>(p); x.raw.cancelLayout(x.raw.context, h); };
        surface = rawSurface; surface.context = this;
        surface.acquireAnchors = [](void* p, MmsSpan* a, MmsHandle* h) { auto& x = *static_cast<Host*>(p); return x.rawSurface.acquireAnchors(x.rawSurface.context, a, h); };
        surface.releaseAnchors = [](void* p, MmsHandle h) { auto& x = *static_cast<Host*>(p); x.rawSurface.releaseAnchors(x.rawSurface.context, h); };
        surface.registerSurface = [](void* p, const MmsUiSurfaceDescV1* d, MmsHandle* h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); if (x.FailRegistration()) return MMS_E_FAILED;
            const auto r = x.rawSurface.registerSurface(x.rawSurface.context, d, h); if (!r) x.added.push_back(*h); return r;
        };
        surface.unregisterSurface = [](void* p, MmsHandle h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); x.attempted.push_back(h);
            if (x.removeResult != MMS_OK && (!x.blocked || h == x.blocked)) return x.removeResult;
            const auto r = x.rawSurface.unregisterSurface(x.rawSurface.context, h); if (!r) x.removed.push_back(h); return r;
        };
        layout = rawLayout; layout.context = this;
        layout.setDefaultHints = [](void* p, MmsString id, const MmsUiLayoutHintsV1* h) -> MmsResult {
            auto& x = *static_cast<Host*>(p); return x.hintsResult ? x.hintsResult : x.rawLayout.setDefaultHints(x.rawLayout.context, id, h);
        };
        layout.acquireSnapshot = [](void* p, MmsUiLayoutSnapshotV1* s) { auto& x = *static_cast<Host*>(p); return x.rawLayout.acquireSnapshot(x.rawLayout.context, s); };
        layout.releaseSnapshot = [](void* p, MmsHandle h) { auto& x = *static_cast<Host*>(p); x.rawLayout.releaseSnapshot(x.rawLayout.context, h); };
        layout.stageHints = [](void* p, MmsHandle t, MmsString id, uint64_t g, const MmsUiLayoutHintsV1* h) { auto& x = *static_cast<Host*>(p); return x.rawLayout.stageHints(x.rawLayout.context, t, id, g, h); };
        host.h = sdk::Header<MmsHostV1>(); host.context = this;
        layoutHistory.h = sdk::Header<MmsUiLayoutApiV1_1>(); layoutHistory.context = this;
        historyInfo.h = sdk::Header<MmsUiLayoutHistoryInfoV1>();
        historyInfo.canUndo = historyInfo.canRedo = 1; historyInfo.undoDepth = 2; historyInfo.redoDepth = 1;
        layoutHistory.setDefaultHints = [](void*, MmsString, const MmsUiLayoutHintsV1*) -> MmsResult { return MMS_OK; };
        layoutHistory.acquireSnapshot = [](void*, MmsUiLayoutSnapshotV1*) -> MmsResult { return MMS_E_NOT_READY; };
        layoutHistory.releaseSnapshot = [](void*, MmsHandle) {};
        layoutHistory.stageHints = [](void*, MmsHandle, MmsString, uint64_t, const MmsUiLayoutHintsV1*) -> MmsResult { return MMS_OK; };
        layoutHistory.queryHistory = [](void* p, MmsUiLayoutHistoryInfoV1* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p); ++x.historyInfoCalls;
            if (!out) return MMS_E_ARGUMENT;
            *out = x.historyInfo; return MMS_OK;
        };
        layoutHistory.undo = [](void* p) -> MmsResult { auto& x = *static_cast<Host*>(p); ++x.historyUndo; return MMS_OK; };
        layoutHistory.redo = [](void* p) -> MmsResult { auto& x = *static_cast<Host*>(p); ++x.historyRedo; return MMS_OK; };
        render.h = sdk::Header<MmsRenderApiV1>(); render.context = this;
        renderInfo.h = sdk::Header<MmsRenderInfoV1>(); renderInfo.deviceGeneration = 3; renderInfo.width = 640; renderInfo.height = 480;
        render.query = [](void* p, MmsRenderInfoV1* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p); ++x.renderQueries; if (!out) return MMS_E_ARGUMENT;
            *out = x.renderInfo; return MMS_OK;
        };
        render.acquireLease = [](void* p, const MmsRenderLeaseDescV1* desc, MmsHandle* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p); ++x.acquireCalls;
            if (!desc || !desc->release || !out || !desc->name.bytes) return MMS_E_ARGUMENT;
            *out = x.nextLease++; return MMS_OK;
        };
        render.releaseLease = [](void* p, MmsHandle lease) -> MmsResult {
            auto& x = *static_cast<Host*>(p); ++x.releaseCalls; x.releasedLease = lease; return MMS_OK;
        };
        host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t bytes, void* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p); ++x.queries;
            x.queryArguments = x.queryArguments && major == 1 && minor == 0 && out != nullptr;
            const auto id = String(name);
            if (id == "mms.ui") {
                x.queryArguments = x.queryArguments && bytes == sizeof(MmsUiApiV1);
                if (x.queryResult) return x.queryResult;
                auto value = x.ui;
                if (x.badHeaderSize) value.h.size = sizeof(MmsHeader);
                if (x.badHeaderVersion) value.h.major = 2;
                if (x.futureHeader) { value.h.size += 64; value.h.minor = 7; }
                if (x.missingFunction) value.unregisterItem = nullptr;
                std::memcpy(out, &value, sizeof(value)); return MMS_OK;
            }
            if (id == "mms.ui.surface") {
                ++x.surfaceQueries; x.queryArguments = x.queryArguments && bytes == sizeof(MmsUiSurfaceApiV1);
                if (x.surfaceResult) return x.surfaceResult;
                std::memcpy(out, &x.surface, sizeof(x.surface)); return MMS_OK;
            }
            if (id == "mms.ui.layout" && minor == 1) {
                ++x.historyQueries; x.historyArguments = x.historyArguments && bytes == sizeof(MmsUiLayoutApiV1_1) && out != nullptr;
                if (x.historyResult) return x.historyResult;
                auto value = x.layoutHistory;
                if (x.missingHistoryFunction) value.queryHistory = nullptr;
                std::memcpy(out, &value, sizeof(value)); return MMS_OK;
            }
            if (id == "mms.ui.layout") {
                ++x.layoutQueries; x.queryArguments = x.queryArguments && bytes == sizeof(MmsUiLayoutApiV1);
                if (x.layoutResult) return x.layoutResult;
                std::memcpy(out, &x.layout, sizeof(x.layout)); return MMS_OK;
            }
            if (id == "mms.render") {
                ++x.renderQueries; x.queryArguments = x.queryArguments && bytes == sizeof(MmsRenderApiV1);
                if (x.renderResult) return x.renderResult;
                auto value = x.render;
                if (x.missingRenderFunction) value.acquireLease = nullptr;
                std::memcpy(out, &value, sizeof(value)); return MMS_OK;
            }
            return MMS_E_NOT_FOUND;
        };
    }
    bool FailRegistration() { return ++registrations == failRegistration; }
    sdk::UiServices Services() {
        sdk::UiServices result; CHECK(sdk::UiServices::Query(host, result) == MMS_OK); return result;
    }
    static MmsResult MMS_CALL State(void* p, MmsObjectRef, MmsCommandStateV1* out) {
        *out = {}; out->h = sdk::Header<MmsCommandStateV1>(); out->enabled = out->visible = 1;
        out->disabledReason = sdk::Text(static_cast<Host*>(p)->reason); return MMS_OK;
    }
    static MmsResult MMS_CALL Invoke(void* p, MmsObjectRef, MmsSpan, MmsHandle transaction) {
        auto& x = *static_cast<Host*>(p); if (transaction) return MMS_E_ARGUMENT;
        ++x.invokes; if (x.closeOnInvoke) x.callbackCloseResult = x.closeOnInvoke->Close(); return MMS_OK;
    }
    MmsCommandDescV1 Command(const char* id = "example.cppsdk.command") {
        MmsCommandDescV1 d{}; d.info.h = sdk::Header<MmsCommandDescV1>();
        d.info.id = sdk::Text(id); d.info.ownerPlugin = sdk::Text(client.id); d.info.label = sdk::Text("SDK command");
        d.context = this; d.getState = State; d.invoke = Invoke; return d;
    }
    MmsUiElementV1 Button(const char* id, const char* command = "example.cppsdk.command", const char* parent = "mms.toolbar.main.extensions") {
        MmsUiElementV1 e{}; e.h = sdk::Header<MmsUiElementV1>(); e.id = sdk::Text(id); e.ownerPlugin = sdk::Text(client.id);
        e.parentId = sdk::Text(parent); e.commandId = sdk::Text(command); e.label = sdk::Text("SDK button");
        e.kind = MMS_UI_BUTTON; e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_LAYOUT; return e;
    }
    sdk::CommandUiSpec Spec() {
        sdk::CommandUiSpec s{}; s.id = "example.cppsdk.command"; s.owner = client.id; s.label = "SDK command";
        s.context = this; s.getState = State; s.invoke = Invoke;
        s.menu = {"example.cppsdk.menu", "mms.menu.plugins", "SDK menu"};
        s.group = {"example.cppsdk.group", "mms.toolbar.main.extensions", "SDK group"};
        s.button = {"example.cppsdk.button", "example.cppsdk.group", "SDK button"}; return s;
    }
    MmsPropertyDescV1 Property() {
        MmsPropertyDescV1 d{}; d.info.h = sdk::Header<MmsPropertyDescV1>(); d.info.id = sdk::Text("example.cppsdk.text");
        d.info.ownerPlugin = sdk::Text(client.id); d.info.label = sdk::Text("Text"); d.info.valueType = MMS_VALUE_TEXT;
        d.info.flags = MMS_PROPERTY_READ_ONLY; d.context = this;
        d.read = [](void* p, MmsObjectRef, MmsValue* v, MmsHandle* h) -> MmsResult {
            *v = {}; v->type = MMS_VALUE_TEXT; v->value.text = sdk::Text(static_cast<Host*>(p)->text); *h = 77; return MMS_OK;
        };
        d.releaseValue = [](void* p, MmsHandle h) { if (h == 77) ++static_cast<Host*>(p)->providerReleases; };
        return d;
    }
    MmsUiSurfaceDescV1 Surface() {
        MmsUiSurfaceDescV1 d{}; d.h = sdk::Header<MmsUiSurfaceDescV1>(); d.id = sdk::Text("example.cppsdk.surface"); d.context = this;
        d.createView = [](void*, const MmsUiMountV1*, void**, uintptr_t*) -> MmsResult { return MMS_E_NOT_READY; };
        d.resizeView = [](void*, const MmsUiRectV1*, uint32_t) {};
        d.showView = [](void*, uint32_t) {};
        d.focusView = [](void*) -> MmsResult { return MMS_OK; };
        d.destroyView = [](void*) {}; return d;
    }
};

static void QueryChecks() {
    static_assert(!std::is_copy_constructible<sdk::RegistrationBundle>::value, "registrations have one owner");
    static_assert(std::is_move_constructible<sdk::RegistrationBundle>::value, "registrations may transfer ownership");
    static_assert(!std::is_move_assignable<sdk::RegistrationBundle>::value, "assignment cannot discard a failed close");
    Host x; sdk::UiServices empty;
    CHECK(!empty.ready()); CHECK(sdk::UiServices::Query(x.host, empty) == MMS_OK && empty.ready());
    CHECK(x.queries == 1 && x.surfaceQueries == 0 && x.layoutQueries == 0 && x.queryArguments);
    CHECK(empty.ui().context == &x && empty.ui().registerCommand == x.ui.registerCommand);
    CHECK(empty.RequireSurface() == MMS_OK && empty.RequireSurface() == MMS_OK && x.surfaceQueries == 1);
    CHECK(empty.RequireLayout() == MMS_OK && empty.RequireLayout() == MMS_OK && x.layoutQueries == 1);
    CHECK(empty.surface().context == &x && empty.layout().context == &x && x.queryArguments);
    const auto context = empty.ui().context;
    for (MmsResult failure : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_THREAD, MMS_E_MEMORY}) {
        x.queryResult = failure; CHECK(sdk::UiServices::Query(x.host, empty) == failure);
        CHECK(empty.ready() && empty.ui().context == context);
    }
    x.queryResult = MMS_OK;
    x.badHeaderSize = true; CHECK(sdk::UiServices::Query(x.host, empty) != MMS_OK); x.badHeaderSize = false;
    x.badHeaderVersion = true; CHECK(sdk::UiServices::Query(x.host, empty) != MMS_OK); x.badHeaderVersion = false;
    x.missingFunction = true; CHECK(sdk::UiServices::Query(x.host, empty) != MMS_OK); x.missingFunction = false;
    CHECK(empty.ready() && empty.ui().context == context);
    auto noQuery = x.host; noQuery.queryService = nullptr; CHECK(sdk::UiServices::Query(noQuery, empty) != MMS_OK);
    x.futureHeader = true; auto futureHost = x.host; futureHost.h.size += 32; futureHost.h.minor = 5;
    CHECK(sdk::UiServices::Query(futureHost, empty) == MMS_OK && empty.ready() && x.queryArguments);
    x.futureHeader = false;
    Host optional; optional.surfaceResult = optional.layoutResult = MMS_E_NOT_FOUND;
    auto basic = optional.Services(); CHECK(basic.RequireSurface() == MMS_E_NOT_FOUND && basic.ready());
    CHECK(basic.RequireLayout() == MMS_E_NOT_FOUND && basic.ready());
    optional.surfaceResult = optional.layoutResult = MMS_OK;
    auto removeSurface = optional.surface.unregisterSurface; optional.surface.unregisterSurface = nullptr;
    CHECK(basic.RequireSurface() == MMS_E_UNSUPPORTED && basic.surface().unregisterSurface == nullptr);
    optional.surface.unregisterSurface = removeSurface;
    auto setHints = optional.layout.setDefaultHints; optional.layout.setDefaultHints = nullptr;
    CHECK(basic.RequireLayout() == MMS_E_UNSUPPORTED && basic.layout().setDefaultHints == nullptr);
    optional.layout.setDefaultHints = setHints;
    CHECK(basic.RequireSurface() == MMS_OK && basic.RequireLayout() == MMS_OK && optional.queryArguments);
}

static void LayeredChecks() {
    Host x; auto services = x.Services(); sdk::CommandUi control(services); auto spec = x.Spec();
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>(); hints.flow = MMS_FLOW_WRAP;
    hints.minimum[0] = 30; hints.preferred[0] = 90; hints.preferred[1] = 24;
    spec.button.hints = &hints; spec.order = 42;
    CHECK(control.Register(spec) == MMS_OK); CHECK(control.registrations().size() == 4);
    CHECK(control.command().handle == x.registry.commands.at("example.cppsdk.command")->handle);
    CHECK(control.menu().handle == x.registry.elements.at("example.cppsdk.menu").handle);
    CHECK(control.group().handle == x.registry.elements.at("example.cppsdk.group").handle);
    CHECK(control.button().handle == x.registry.elements.at("example.cppsdk.button").handle);
    CHECK(String(x.registry.elements.at("example.cppsdk.button").desc.commandId) == spec.id);
    CHECK(String(x.registry.elements.at("example.cppsdk.button").desc.parentId) == spec.group.id);
    CHECK(x.registry.elements.at("example.cppsdk.button").defaultHints.preferred[0] == 90);
    CHECK(x.registry.elements.at("example.cppsdk.button").defaultHints.order == 42);
    CHECK(services.ui().invoke(services.ui().context, sdk::Text(spec.id), {}, {}) == MMS_OK && x.invokes == 1);
    auto command = x.registry.commands.at("example.cppsdk.command")->desc;
    command.info.label = sdk::Text("updated through C table");
    CHECK(services.ui().updateCommand(services.ui().context, control.command().handle, &command) == MMS_OK);
    CHECK(x.registry.commands.at("example.cppsdk.command")->handle == control.command().handle);
    CHECK(String(x.registry.commands.at("example.cppsdk.command")->desc.info.label) == "updated through C table");
    auto element = x.registry.elements.at("example.cppsdk.button").desc; element.label = sdk::Text("updated button");
    CHECK(services.ui().updateElement(services.ui().context, control.button().handle, &element) == MMS_OK);
    CHECK(x.registry.elements.at("example.cppsdk.button").handle == control.button().handle);
    CHECK(control.Close() == MMS_OK && control.Close() == MMS_OK && control.registrations().size() == 0);
    CHECK(x.registry.commands.empty() && x.registry.elements.size() == 6);
    CHECK(x.removed.size() == 4 && x.removed.back() == x.added.front());
    const auto child = std::find(x.removed.begin(), x.removed.end(), x.added.back());
    CHECK(child != x.removed.end());

    // Reparenting through the C table can reverse registration dependency order.
    sdk::RegistrationBundle reordered(services); sdk::RegistrationRef c, button, group;
    CHECK(reordered.AddCommand(x.Command(), &c) == MMS_OK);
    CHECK(reordered.AddElement(x.Button("example.cppsdk.early"), &button) == MMS_OK);
    auto g = x.Button("example.cppsdk.late"); g.kind = MMS_UI_GROUP; g.commandId = {}; g.capabilities |= MMS_UI_CONTAINER;
    CHECK(reordered.AddElement(g, &group) == MMS_OK);
    element = x.registry.elements.at("example.cppsdk.early").desc; element.parentId = g.id;
    CHECK(x.raw.updateElement(x.raw.context, button.handle, &element) == MMS_OK);
    CHECK(reordered.Close() == MMS_OK && reordered.size() == 0 && x.registry.commands.empty());
}

static void OwnershipChecks() {
    Host x; auto services = x.Services(); auto external = x.Command("example.cppsdk.external"); MmsHandle externalHandle = 0;
    CHECK(x.raw.registerCommand(x.raw.context, &external, &externalHandle) == MMS_OK);
    sdk::RegistrationRef button;
    {
        sdk::RegistrationBundle original(services);
        CHECK(original.AddElement(x.Button("example.cppsdk.owned", "example.cppsdk.external"), &button) == MMS_OK);
        CHECK(original.owns(button) && original.get(0).handle == button.handle);
        sdk::RegistrationBundle moved(std::move(original));
        CHECK(original.size() == 0 && moved.owns(button) && moved.size() == 1);
        CHECK(original.Close() == MMS_OK && x.registry.elements.count("example.cppsdk.owned") == 1);
        sdk::RegistrationRef foreign{sdk::RegistrationKind::Command, externalHandle};
        CHECK(!moved.owns(foreign) && !moved.detach(foreign));
        auto detached = moved.detach(button); CHECK(detached.handle == button.handle && !moved.owns(button));
        CHECK(moved.Close() == MMS_OK && x.registry.elements.count("example.cppsdk.owned") == 1);
    }
    CHECK(x.registry.commands.at("example.cppsdk.external")->handle == externalHandle);
    CHECK(x.raw.unregisterItem(x.raw.context, button.handle) == MMS_OK);
    CHECK(x.raw.unregisterItem(x.raw.context, externalHandle) == MMS_OK);
    {
        sdk::RegistrationBundle owner(services); sdk::RegistrationRef c;
        CHECK(owner.AddCommand(x.Command(), &c) == MMS_OK);
        CHECK(x.raw.unregisterItem(x.raw.context, c.handle) == MMS_OK);
        CHECK(owner.Close() == MMS_OK && owner.size() == 0); // Already removed externally.
    }
    {
        sdk::RegistrationBundle owner(services); CHECK(owner.AddCommand(x.Command()) == MMS_OK);
    }
    CHECK(x.registry.commands.empty()); // Normal destructor cleanup.
    {
        sdk::CommandUi original(services); CHECK(original.Register(x.Spec()) == MMS_OK);
        const auto ownedCommand = original.command();
        sdk::CommandUi moved(std::move(original));
        CHECK(!original.command() && moved.command() == ownedCommand && original.Close() == MMS_OK);
        auto oldButton = moved.button(); auto detached = moved.registrations().detach(oldButton);
        CHECK(detached == oldButton && !moved.button());
        // The detached child is now an external dependency of the owned group.
        CHECK(moved.Close() == MMS_E_BUSY && moved.command() == ownedCommand && moved.group());
        CHECK(x.raw.unregisterItem(x.raw.context, detached.handle) == MMS_OK);
        CHECK(moved.Close() == MMS_OK && !moved.command() && !moved.group());
    }
}

static void PresentationChecks() {
    Host x; auto services = x.Services(); sdk::CommandUi control(services); auto spec = x.Spec();
    spec.menu.parent = {}; spec.menu.label = {}; spec.button.parent = {}; spec.button.label = {}; spec.group.parent = {};
    CHECK(control.Register(spec) == MMS_OK);
    CHECK(String(x.registry.elements.at("example.cppsdk.menu").desc.parentId) == "mms.menu.plugins");
    CHECK(String(x.registry.elements.at("example.cppsdk.menu").desc.label) == spec.label);
    CHECK(String(x.registry.elements.at("example.cppsdk.group").desc.parentId) == "mms.toolbar.main.extensions");
    CHECK(String(x.registry.elements.at("example.cppsdk.button").desc.parentId) == spec.group.id);
    CHECK(control.Close() == MMS_OK);
    spec.menu = {}; spec.group = {}; spec.button = {};
    CHECK(control.Register(spec) == MMS_OK && control.command() && !control.menu() && !control.button() && !control.group());
    CHECK(control.registrations().size() == 1 && control.Close() == MMS_OK);
    spec.button.id = "example.cppsdk.button";
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>(); hints.order = -12;
    spec.button.hints = &hints;
    CHECK(control.Register(spec) == MMS_OK && x.registry.elements.at("example.cppsdk.button").defaultHints.order == -12);
    CHECK(String(x.registry.elements.at("example.cppsdk.button").desc.parentId) == "mms.toolbar.main.extensions");
    CHECK(control.Close() == MMS_OK);
    spec = x.Spec(); spec.button = {};
    const auto count = x.registrations; CHECK(control.Register(spec) == MMS_E_ARGUMENT && x.registrations == count);
    spec = x.Spec(); spec.menu.hints = &hints;
    CHECK(control.Register(spec) == MMS_E_ARGUMENT && x.registrations == count);
    CHECK(control.registrations().size() == 0 && x.registry.commands.empty());
}

static void FailureChecks() {
    for (unsigned failure = 1; failure <= 4; ++failure) {
        Host x; auto services = x.Services(); sdk::CommandUi control(services); x.failRegistration = failure;
        CHECK(control.Register(x.Spec()) == MMS_E_FAILED);
        CHECK(control.cleanup_result() == MMS_OK && control.registrations().size() == 0);
        CHECK(x.registry.commands.empty() && x.registry.elements.size() == 6);
        CHECK(x.added.size() == failure - 1 && x.removed.size() == x.added.size());
        CHECK(std::equal(x.removed.begin(), x.removed.end(), x.added.rbegin()));
    }
    Host x; auto services = x.Services(); sdk::CommandUi control(services);
    x.failRegistration = 3; x.removeResult = MMS_E_BUSY;
    CHECK(control.Register(x.Spec()) == MMS_E_FAILED && control.cleanup_result() == MMS_E_BUSY);
    CHECK(control.registrations().size() == x.added.size() && !x.added.empty());
    for (auto h : x.added) {
        bool held = false; for (size_t i = 0; i < control.registrations().size(); ++i) held |= control.registrations().get(i).handle == h;
        CHECK(held);
    }
    CHECK(x.registry.commands.count("example.cppsdk.command") == 1 && x.removed.empty());
    const auto registrations = x.registrations;
    CHECK(control.Register(x.Spec()) == MMS_E_BUSY && x.registrations == registrations);
    x.removeResult = MMS_OK; CHECK(control.Close() == MMS_OK && control.registrations().size() == 0);
    x.failRegistration = 0; CHECK(control.Register(x.Spec()) == MMS_OK);
    const auto remaining = control.registrations().size();
    MmsResult fromWorker = MMS_OK; std::thread worker([&] { fromWorker = control.Close(); }); worker.join();
    CHECK(fromWorker == MMS_E_THREAD && control.registrations().size() == remaining);
    x.closeOnInvoke = &control.registrations();
    CHECK(services.ui().invoke(services.ui().context, sdk::Text("example.cppsdk.command"), {}, {}) == MMS_OK);
    CHECK(x.callbackCloseResult == MMS_E_BUSY && control.registrations().size() == remaining);
    x.closeOnInvoke = nullptr; CHECK(control.Close() == MMS_OK && x.registry.commands.empty());

    Host hintsHost; auto hintsServices = hintsHost.Services(); sdk::CommandUi hintsControl(hintsServices);
    auto spec = hintsHost.Spec(); MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>();
    spec.button.hints = &hints; hintsHost.hintsResult = MMS_E_FAILED; hintsHost.removeResult = MMS_E_BUSY;
    CHECK(hintsControl.Register(spec) == MMS_E_FAILED && hintsControl.cleanup_result() == MMS_E_BUSY);
    CHECK(hintsControl.registrations().size() != 0 && !hintsHost.registry.commands.empty());
    hintsHost.removeResult = MMS_OK; CHECK(hintsControl.Close() == MMS_OK && hintsHost.registry.commands.empty());
    Host hard; sdk::RegistrationBundle owned(hard.Services()); sdk::RegistrationRef command;
    CHECK(owned.AddCommand(hard.Command(), &command) == MMS_OK); hard.removeResult = MMS_E_FAILED;
    CHECK(owned.Close() == MMS_E_FAILED && owned.owns(command));
    hard.removeResult = MMS_OK; CHECK(owned.reset() == MMS_OK && owned.Close() == MMS_OK);
}

static void PropertySurfaceLeaseChecks() {
    Host x; auto services = x.Services(); sdk::RegistrationBundle bundle(services);
    sdk::RegistrationRef property, surface, panel, command;
    CHECK(bundle.AddProperty(x.Property(), &property) == MMS_OK);
    CHECK(property.handle == x.registry.properties.at("example.cppsdk.text")->handle);
    CHECK(bundle.AddCommand(x.Command(), &command) == MMS_OK);
    CHECK(bundle.AddSurface(x.Surface(), &surface) == MMS_OK);
    CHECK(surface.handle == x.registry.surfaces.at("example.cppsdk.surface")->handle);
    MmsUiElementV1 e{}; e.h = sdk::Header<MmsUiElementV1>(); e.id = sdk::Text("example.cppsdk.panel");
    e.ownerPlugin = sdk::Text(x.client.id); e.parentId = sdk::Text("mms.workspace.tabs.extensions"); e.label = sdk::Text("Panel");
    e.kind = MMS_UI_PANEL; e.capabilities = MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_LAYOUT; e.surfaceId = sdk::Text("example.cppsdk.surface");
    CHECK(bundle.AddElement(e, &panel) == MMS_OK);
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>(); hints.preferred[0] = 190; hints.preferred[1] = 80;
    CHECK(bundle.SetDefaultHints("example.cppsdk.panel", hints) == MMS_OK);
    CHECK(x.registry.elements.at("example.cppsdk.panel").defaultHints.preferred[0] == 190);
    CHECK(services.RequireSurface() == MMS_OK && services.RequireLayout() == MMS_OK);
    MmsUiSnapshotV1 snapshot{}; snapshot.h = sdk::Header<MmsUiSnapshotV1>();
    CHECK(services.ui().acquireSnapshot(services.ui().context, 0, &snapshot) == MMS_OK);
    MmsValue value{}; MmsHandle valueLease = 0;
    CHECK(services.ui().readProperty(services.ui().context, sdk::Text("example.cppsdk.text"), {}, &value, &valueLease) == MMS_OK);
    CHECK(valueLease != 0 && x.providerReleases == 1 && String(value.value.text) == "provider text");
    MmsCommandStateV1 state{}; state.h = sdk::Header<MmsCommandStateV1>(); MmsHandle stateLease = 0;
    CHECK(services.ui().getCommandState(services.ui().context, sdk::Text("example.cppsdk.command"), {}, &state, &stateLease) == MMS_OK);
    CHECK(stateLease != 0 && String(state.disabledReason) == x.reason);
    MmsSpan anchors{}; MmsHandle anchorLease = 0;
    CHECK(services.surface().acquireAnchors(services.surface().context, &anchors, &anchorLease) == MMS_OK && anchors.count == 6);
    const uint64_t controls = (1ull << MMS_UI_BUTTON) | (1ull << MMS_UI_GROUP) | (1ull << MMS_UI_SEPARATOR) | (1ull << MMS_UI_INPUT);
    const std::map<std::string, uint64_t> expectedAnchors{
        {"mms.menu.plugins", (1ull << MMS_UI_MENU) | (1ull << MMS_UI_MENU_ITEM) | (1ull << MMS_UI_SEPARATOR)},
        {"mms.toolbar.main.extensions", controls}, {"mms.properties.main.extensions", controls},
        {"mms.sidebar.left.extensions", controls}, {"mms.sidebar.right.extensions", controls},
        {"mms.workspace.tabs.extensions", 1ull << MMS_UI_PANEL}};
    std::set<std::string> anchorIds;
    for (uint64_t i = 0; i < anchors.count; ++i) {
        const auto& anchor = *reinterpret_cast<const MmsUiAnchorV1*>(static_cast<const unsigned char*>(anchors.data) + size_t(i * anchors.stride));
        const auto id = String(anchor.id); CHECK(expectedAnchors.count(id) && anchorIds.insert(id).second);
        CHECK(anchor.allowedKinds == expectedAnchors.at(id) && !anchor.reserved);
        CHECK(anchor.flags == (id == "mms.workspace.tabs.extensions" ? MMS_ANCHOR_NATIVE_CHILD : 0));
        if (id == "mms.sidebar.left.extensions" || id == "mms.sidebar.right.extensions")
            CHECK(x.registry.elements.at(id).desc.kind == MMS_UI_TOOLBAR &&
                x.registry.elements.at(id).desc.capabilities == (MMS_UI_CONTAINER | MMS_UI_LAYOUT));
    }
    std::map<std::string, UiRegistry::Geometry> geometry;
    for (const auto& entry : x.registry.elements) geometry.emplace(entry.first, UiRegistry::Geometry{1, 2, 190, 80, true, false});
    CHECK(x.registry.PublishGeometry(geometry, x.registry.Revision()));
    MmsUiLayoutSnapshotV1 layoutSnapshot{}; layoutSnapshot.h = sdk::Header<MmsUiLayoutSnapshotV1>();
    CHECK(services.layout().acquireSnapshot(services.layout().context, &layoutSnapshot) == MMS_OK);
    CHECK(x.registry.HasLeases(x.client));
    CHECK(services.ui().stageProperty(services.ui().context, 0, sdk::Text("example.cppsdk.text"), {}, &value) == MMS_E_NOT_READY);
    x.text = "changed provider"; x.reason = "changed reason";
    CHECK(bundle.Close() == MMS_OK && x.registry.properties.empty() && x.registry.surfaces.empty() && x.registry.commands.empty());
    CHECK(String(value.value.text) == "provider text" && String(state.disabledReason) == "reason from callback");
    CHECK(snapshot.commands.count == 1 && snapshot.properties.count == 1);
    const auto* properties = static_cast<const MmsPropertyInfoV1*>(snapshot.properties.data);
    CHECK(String(properties[0].id) == "example.cppsdk.text");
    CHECK(static_cast<const MmsUiAnchorV1*>(anchors.data)[0].id.bytes != 0);
    CHECK(layoutSnapshot.items.count == 6 && static_cast<const MmsUiLayoutItemV1*>(layoutSnapshot.items.data)[0].elementId.bytes != 0);
    CHECK(x.removed.back() == surface.handle);
    CHECK(std::find(x.removed.begin(), x.removed.end(), panel.handle) < std::find(x.removed.begin(), x.removed.end(), surface.handle));
    services.ui().releaseSnapshot(services.ui().context, snapshot.handle);
    services.ui().releaseValue(services.ui().context, valueLease);
    services.ui().releaseValue(services.ui().context, stateLease);
    CHECK(x.registry.HasLeases(x.client));
    services.surface().releaseAnchors(services.surface().context, anchorLease);
    CHECK(x.registry.HasLeases(x.client));
    services.layout().releaseSnapshot(services.layout().context, layoutSnapshot.handle);
    CHECK(!x.registry.HasLeases(x.client));

    sdk::RegistrationBundle gates(services); auto badCommand = x.Command(); badCommand.info.flags = MMS_COMMAND_EDIT;
    CHECK(gates.AddCommand(badCommand) == MMS_E_NOT_READY && gates.size() == 0);
    auto badProperty = x.Property(); badProperty.info.flags = 0;
    CHECK(gates.AddProperty(badProperty) == MMS_E_NOT_READY && gates.size() == 0);
    auto input = x.Button("example.cppsdk.input"); input.kind = MMS_UI_INPUT; input.commandId = {};
    CHECK(gates.AddElement(input) == MMS_E_ARGUMENT && gates.size() == 0);
    CHECK(gates.Close() == MMS_OK && x.registry.elements.size() == 6);
}
static void RenderChecks() {
    Host x;
    sdk::RenderServices render;
    CHECK(!render.ready());
    CHECK(sdk::RenderServices::Query(x.host, render) == MMS_OK && render.ready());
    CHECK(x.renderQueries == 1 && x.queryArguments);
    MmsRenderInfoV1 info{};
    CHECK(render.Info(info) == MMS_OK && info.deviceGeneration == 3 && info.lost == 0 && info.width == 640);
    MmsHostV1 noService = x.host; noService.queryService = nullptr;
    sdk::RenderServices missing;
    CHECK(sdk::RenderServices::Query(noService, missing) == MMS_E_ARGUMENT && !missing.ready());
    x.missingRenderFunction = true;
    sdk::RenderServices incomplete;
    CHECK(sdk::RenderServices::Query(x.host, incomplete) == MMS_E_UNSUPPORTED && !incomplete.ready());
    x.missingRenderFunction = false;
    sdk::RenderServices usable;
    CHECK(sdk::RenderServices::Query(x.host, usable) == MMS_OK);
    sdk::RenderLease lease;
    CHECK(usable.Acquire("example.device", [](void*) {}, &x, lease) == MMS_OK && lease.valid() && x.acquireCalls == 1);
    CHECK(lease.Release() == MMS_OK && x.releaseCalls == 1 && x.releasedLease == 41 && !lease.valid());
    CHECK(lease.Release() == MMS_OK && x.releaseCalls == 1);
    sdk::RenderLease moved;
    CHECK(usable.Acquire("example.device2", [](void*) {}, &x, moved) == MMS_OK);
    const MmsHandle raw = moved.get();
    sdk::RenderLease target;
    target = std::move(moved);
    CHECK(!moved.valid() && target.get() == raw && target.Release() == MMS_OK && x.releaseCalls == 2);
    std::cout << "cpp render sdk PASS\n";
}
static void LayoutHistorySdkChecks() {
    Host x;
    sdk::LayoutHistoryServices history;
    CHECK(!history.ready());
    CHECK(sdk::LayoutHistoryServices::Query(x.host, history) == MMS_OK && history.ready());
    CHECK(x.historyQueries == 1 && x.historyArguments);
    MmsUiLayoutHistoryInfoV1 info{};
    CHECK(history.Info(info) == MMS_OK && x.historyInfoCalls == 1 &&
          info.canUndo == 1 && info.canRedo == 1 && info.undoDepth == 2 && info.redoDepth == 1);
    CHECK(history.Undo() == MMS_OK && x.historyUndo == 1);
    CHECK(history.Redo() == MMS_OK && x.historyRedo == 1);
    MmsHostV1 noService = x.host; noService.queryService = nullptr;
    sdk::LayoutHistoryServices missing;
    CHECK(sdk::LayoutHistoryServices::Query(noService, missing) == MMS_E_ARGUMENT && !missing.ready());
    x.missingHistoryFunction = true;
    sdk::LayoutHistoryServices incomplete;
    CHECK(sdk::LayoutHistoryServices::Query(x.host, incomplete) == MMS_E_UNSUPPORTED && !incomplete.ready());
    x.missingHistoryFunction = false;
    x.historyResult = MMS_E_VERSION;
    sdk::LayoutHistoryServices oldHost;
    CHECK(sdk::LayoutHistoryServices::Query(x.host, oldHost) == MMS_E_VERSION && !oldHost.ready());
    std::cout << "cpp layout history sdk PASS\n";
}
static void LayoutSdkChecks() {
    Host x;
    sdk::LayoutServices layout;
    CHECK(!layout.ready());
    CHECK(sdk::LayoutServices::Query(x.host, layout) == MMS_OK && layout.ready());
    MmsUiLayoutHintsV1 hints{}; hints.h = sdk::Header<MmsUiLayoutHintsV1>();
    CHECK(layout.SetDefaultHints("example.cppsdk.missing", hints) == MMS_E_NOT_FOUND);
    CHECK(layout.StageHints(999, "example.cppsdk.missing", 1, hints) == MMS_E_NOT_FOUND);
    MmsHostV1 noService = x.host; noService.queryService = nullptr;
    sdk::LayoutServices missing;
    CHECK(sdk::LayoutServices::Query(noService, missing) == MMS_E_ARGUMENT && !missing.ready());
    Host incompleteHost;
    incompleteHost.layout.releaseSnapshot = nullptr;
    sdk::LayoutServices incomplete;
    CHECK(sdk::LayoutServices::Query(incompleteHost.host, incomplete) == MMS_E_UNSUPPORTED && !incomplete.ready());
    Host leaseHost;
    unsigned releases = 0;
    leaseHost.layout.context = &leaseHost;
    leaseHost.layout.acquireSnapshot = [](void*, MmsUiLayoutSnapshotV1* out) -> MmsResult {
        if (!out) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiLayoutSnapshotV1>(); out->handle = 9;
        return MMS_OK;
    };
    leaseHost.layout.releaseSnapshot = [](void* p, MmsHandle handle) {
        if (handle == 9) ++static_cast<Host*>(p)->invokes;
    };
    sdk::LayoutServices leased;
    CHECK(sdk::LayoutServices::Query(leaseHost.host, leased) == MMS_OK);
    sdk::LayoutSnapshotLease lease;
    CHECK(leased.AcquireSnapshot(lease) == MMS_OK && lease.valid() && lease.get() == 9);
    sdk::LayoutSnapshotLease moved = std::move(lease);
    CHECK(!lease.valid() && moved.valid() && moved.get() == 9);
    CHECK(moved.Release() == MMS_OK && !moved.valid() && leaseHost.invokes == 1);
    CHECK(moved.Release() == MMS_OK && leaseHost.invokes == 1);
    std::cout << "cpp layout sdk PASS\n";
}
} // namespace cpp_sdk_checks

static void CppSdkChecks() {
    cpp_sdk_checks::QueryChecks();
    cpp_sdk_checks::LayeredChecks();
    cpp_sdk_checks::OwnershipChecks();
    cpp_sdk_checks::PresentationChecks();
    cpp_sdk_checks::FailureChecks();
    cpp_sdk_checks::PropertySurfaceLeaseChecks();
    cpp_sdk_checks::RenderChecks();
    cpp_sdk_checks::LayoutHistorySdkChecks();
    cpp_sdk_checks::LayoutSdkChecks();
}
