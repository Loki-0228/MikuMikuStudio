#include "ui_refresh_binding.hpp"
#include <mms/cpp/refresh.hpp>

namespace property_data_refresh_checks {
namespace sdk = mms::sdk;
static MmsValue Integer(int64_t value) {
    MmsValue result{}; result.type = MMS_VALUE_INT; result.value.signedValue = value; return result;
}

// This deliberately uses ordinary child windows rather than NativeUi. It
// tests the queue's original registrations and host adapter contract without
// depending on a native property's editor or readback implementation.
struct Fixture : refresh_checks::Fixture {
    struct Context { Fixture* fixture; unsigned index; } contexts[2]{{this, 0}, {this, 1}};
    std::array<std::string, 2> propertyIds{"example.refresh.property.a", "example.refresh.property.b"};
    std::array<std::string, 2> inputIds{"example.refresh.input.a", "example.refresh.input.b"};
    std::array<MmsHandle, 2> properties{}, inputs{}, sources{}, bindings{};
    std::array<HWND, 2> windows{};
    std::array<int64_t, 2> values{7, 9}, displays{-1, -1};
    std::array<unsigned, 2> reads{}, releases{}, hookCalls{}, prepares{};
    std::array<MmsResult, 2> readResults{MMS_OK, MMS_OK};
    std::array<uint64_t, 2> sourceRevisions{};
    std::function<void(unsigned)> onRead;
    std::function<MmsResult(unsigned)> beforeApply;
    MmsUiRefreshBindingApiV1 data = queue.Bindings().Api(client);
    MmsHandle group = 0;
    uint64_t sceneRevision = 73;
    unsigned edits = 0, stages = 0;
    int playhead = 31;
    DWORD uiThread = GetCurrentThreadId();

    Fixture() {
        MmsUiElementV1 g{}; g.h = Header<MmsUiElementV1>(); g.id = S("example.refresh.property.group");
        g.ownerPlugin = Text(owner.id); g.parentId = S("mms.toolbar.main.extensions");
        g.kind = MMS_UI_GROUP; g.capabilities = MMS_UI_CONTAINER | MMS_UI_MOVE | MMS_UI_LAYOUT;
        CHECK(ui.registerElement(ui.context, &g, &group) == MMS_OK);
        RefreshPresentation presentation{group, owner.id, 80, parent, 96, 0, 0, 640, 480, true};
        presentation.container = true; presentations.push_back(presentation);
        for (unsigned i = 0; i < 2; ++i) {
            auto desc = Description(i); CHECK(ui.registerProperty(ui.context, &desc, &properties[i]) == MMS_OK);
            MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = Text(inputIds[i]); e.ownerPlugin = Text(owner.id);
            e.parentId = g.id; e.propertyId = Text(propertyIds[i]); e.label = S("Property"); e.kind = MMS_UI_INPUT;
            e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_LAYOUT;
            CHECK(ui.registerElement(ui.context, &e, &inputs[i]) == MMS_OK);
            windows[i] = CreateWindowExW(0, L"STATIC", L"old value", WS_CHILD | WS_VISIBLE,
                40 + int(i) * 250, 410, 200, 24, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            CHECK(windows[i]);
            presentation = {inputs[i], owner.id, 81 + i, windows[i], 96, float(40 + i * 250), 410, 200, 24, true};
            presentation.propertyDataRefresh = true; presentation.propertyRecord = registry.properties.at(propertyIds[i]);
            presentations.push_back(presentation);
        }
        registry.SetPropertyInputBridges([this](UiClient& c, uint64_t& revision) {
            CHECK(&c == &owner); revision = sceneRevision; return MMS_OK;
        }, [this](UiClient&, const PropertyInputSnapshot&, const MmsValue&) {
            ++edits; return MMS_E_FAILED;
        });
        queue.SetPropertyData([this](MmsHandle element, const std::function<MmsResult()>& revalidate,
            uint64_t& consumedRevision) -> MmsResult {
            CHECK(GetCurrentThreadId() == uiThread && registry.InCallback() && queue.ReadOnlyCause(client));
            const unsigned i = element == inputs[0] ? 0 : 1; CHECK(element == inputs[i]); ++hookCalls[i];
            if (const auto result = revalidate()) return result;
            PropertyInputSnapshot candidate;
            if (const auto result = registry.CapturePropertyInput(inputIds[i], candidate)) return result;
            if (beforeApply) if (const auto result = beforeApply(i)) return result;
            if (const auto result = revalidate()) return result;
            if (!registry.IsPropertyInputCurrent(candidate)) return MMS_E_STALE;
            displays[i] = candidate.value.View().value.signedValue;
            // Scene revisions are not public DATA source revisions. The queue
            // must report zero for a consume-current property read.
            consumedRevision = sceneRevision; return MMS_OK;
        });
        Sync(); CleanAll();
    }
    ~Fixture() { queue.SetPropertyData({}); }
    MmsPropertyDescV1 Description(unsigned i) {
        MmsPropertyDescV1 d{}; d.info.h = Header<MmsPropertyDescV1>(); d.info.id = Text(propertyIds[i]);
        d.info.ownerPlugin = Text(owner.id); d.info.label = S("Property"); d.info.valueType = MMS_VALUE_INT;
        d.info.flags = MMS_PROPERTY_UNDOABLE; d.info.minimum = Integer(-1000); d.info.maximum = Integer(1000);
        d.context = &contexts[i];
        d.read = [](void* p, MmsObjectRef object, MmsValue* out, MmsHandle* lease) -> MmsResult {
            auto& c = *static_cast<Context*>(p); auto& x = *c.fixture;
            CHECK(GetCurrentThreadId() == x.uiThread && x.registry.InCallback() && x.queue.ReadOnlyCause(x.client));
            CHECK(!object.id.lo && !object.id.hi && !object.generation);
            ++x.reads[c.index]; *out = Integer(x.values[c.index]); *lease = 100 + c.index;
            if (x.onRead) x.onRead(c.index); return x.readResults[c.index];
        };
        d.releaseValue = [](void* p, MmsHandle lease) {
            auto& c = *static_cast<Context*>(p); auto& x = *c.fixture;
            CHECK(lease == 100 + c.index && x.registry.InCallback() && x.queue.ReadOnlyCause(x.client)); ++x.releases[c.index];
        };
        d.stage = [](void* p, MmsHandle, MmsObjectRef, const MmsValue*) -> MmsResult {
            ++static_cast<Context*>(p)->fixture->stages; return MMS_E_NOT_READY;
        }; return d;
    }
    RefreshPresentation& Presentation(unsigned i) { return presentations[4 + i]; }
    void Sync() { queue.SyncTargets(presentations); }
    void CleanAll() {
        Clean(); for (auto window : windows) { UpdateWindow(window); ValidateRect(window, nullptr); }
    }
    MmsUiRefreshRequestV1 Current(unsigned i = 0, uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) {
        auto request = Request(Target(inputs[i])); request.kinds = kinds; request.expectedGeometryRevision = 0; return request;
    }
    MmsUiRefreshRequestV1 Tree(uint32_t kinds = MMS_UI_REFRESH_DATA) {
        auto request = Request(Target(group)); request.kinds = kinds; request.scope = MMS_UI_REFRESH_SUBTREE;
        request.expectedGeometryRevision = 0; return request;
    }
    void Done(MmsHandle ticket, uint32_t kinds) {
        const auto state = State(ticket);
        CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.result == MMS_OK && state.completedKinds == kinds && !state.sourceRevision);
    }
    void Failed(MmsHandle ticket, MmsResult error) {
        const auto state = State(ticket);
        CHECK(state.status == MMS_UI_REFRESH_FAILED && state.result == error && !state.completedKinds && !state.sourceRevision);
    }
    void Bind(unsigned i) {
        MmsUiDataSourceDescV1 d{}; d.h = Header<MmsUiDataSourceDescV1>();
        d.id = Text(propertyIds[i]); d.maxBytes = 64;
        CHECK(data.registerSource(data.context, &d, &sources[i]) == MMS_OK);
        const int64_t value = 400 + i;
        MmsUiDataSourcePublishV1 publish{}; publish.h = Header<MmsUiDataSourcePublishV1>();
        publish.source = sources[i]; publish.bytes = {&value, sizeof(value)};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        CHECK(data.publishSource(data.context, &publish, &state) == MMS_OK); sourceRevisions[i] = state.revision;
        MmsUiDataBindingDescV1 b{}; b.h = Header<MmsUiDataBindingDescV1>(); b.target = Target(inputs[i]).target;
        b.source = sources[i]; b.context = &contexts[i]; b.maxDisplayBytes = 64;
        b.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
            auto& c = *static_cast<Context*>(p); auto& x = *c.fixture; ++x.prepares[c.index];
            CHECK(x.registry.InCallback() && x.queue.ReadOnlyCause(x.client));
            return x.data.writePrepared(x.data.context, input->builder, input->sourceBytes);
        }; CHECK(data.bind(data.context, &b, &bindings[i]) == MMS_OK);
    }
    int64_t BoundDisplay(unsigned i) {
        MmsUiDataDisplayV1 display{}; display.h = Header<MmsUiDataDisplayV1>();
        CHECK(data.acquireDisplay(data.context, bindings[i], &display) == MMS_OK && display.bytes.bytes == sizeof(int64_t));
        int64_t value = 0; std::memcpy(&value, display.bytes.data, sizeof(value));
        CHECK(data.releaseDisplay(data.context, display.lease) == MMS_OK); return value;
    }
    MmsHostV1 Host() {
        MmsHostV1 host{}; host.h = Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
            auto& x = *static_cast<Fixture*>(p);
            CHECK(String(name) == "mms.ui.refresh" && major == 1 && minor == 0 && size == sizeof(x.api));
            std::memcpy(out, &x.api, sizeof(x.api)); return MMS_OK;
        }; return host;
    }
};

static void WorkerConsumptionAndCoalescing() {
    Fixture x; const auto target = x.Target(x.inputs[0]);
    CHECK(target.target.element == x.inputs[0] && (target.supportedKinds & MMS_UI_REFRESH_DATA));
    const auto revision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    auto request = x.Current(); std::array<MmsHandle, 2> tickets{};
    std::array<MmsResult, 2> results{MMS_E_FAILED, MMS_E_FAILED};
    std::thread worker([&] {
        for (unsigned i = 0; i < 2; ++i) results[i] = x.api.request(x.api.context, &request, &tickets[i]);
        x.queue.Drain(); // A worker must not evaluate a property or apply UI.
    }); worker.join();
    for (unsigned i = 0; i < 2; ++i) { CHECK(results[i] == MMS_OK && tickets[i]); x.tickets.push_back(tickets[i]); }
    CHECK(!x.reads[0] && !x.reads[1] && !x.hookCalls[0] && x.displays[0] == -1);
    x.values[0] = 12; x.queue.Drain();
    for (auto ticket : tickets) x.Done(ticket, request.kinds);
    CHECK(x.reads[0] == 1 && x.releases[0] == 1 && x.hookCalls[0] == 1 && x.displays[0] == 12);
    CHECK(!x.reads[1] && !refresh_checks::Empty(x.windows[0]) && refresh_checks::Empty(x.windows[1]));
    x.CleanAll(); const auto noChange = x.Submit(x.Current()); x.queue.Drain(); x.Done(noChange, request.kinds);
    CHECK(x.displays[0] == 12 && x.reads[0] == 2 && x.releases[0] == 2);
    CHECK(x.registry.Revision() == revision && x.registry.PreferencesRevision() == preferences);
    CHECK(!x.edits && !x.stages && x.playhead == 31 && x.sceneRevision == 73 && !x.registry.HasLeases(x.owner));
}

static void ReadOnlyAndFailure() {
    Fixture x; auto request = x.Current();
    x.onRead = [&](unsigned) {
        CHECK(x.ui.unregisterItem(x.ui.context, x.second) == MMS_E_BUSY);
        MmsHandle unchanged = 718;
        CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_BUSY && unchanged == 718);
        auto nested = request; nested.kinds = MMS_UI_REFRESH_STATE;
        MmsResult result = MMS_OK;
        std::thread worker([&] { result = x.api.request(x.api.context, &nested, &unchanged); }); worker.join();
        CHECK(result == MMS_E_BUSY && unchanged == 718); x.queue.Drain();
    };
    const auto first = x.Submit(request); x.queue.Drain(); x.Done(first, request.kinds);
    CHECK(x.displays[0] == 7 && !x.registry.InCallback() && !x.queue.ReadOnlyCause());
    x.onRead = {}; x.values[0] = 18; x.readResults[0] = MMS_E_FAILED; x.CleanAll();
    const auto failed = x.Submit(request); x.queue.Drain(); x.Failed(failed, MMS_E_FAILED);
    CHECK(x.displays[0] == 7 && x.reads[0] == x.releases[0] && refresh_checks::Empty(x.windows[0]));
    x.readResults[0] = MMS_OK;
    x.beforeApply = [](unsigned) { return MMS_E_BUSY; };
    const auto refused = x.Submit(request); x.queue.Drain(); x.Failed(refused, MMS_E_BUSY);
    CHECK(x.displays[0] == 7 && refresh_checks::Empty(x.windows[0]) && !x.edits && !x.stages);
    x.beforeApply = {}; x.onRead = [&](unsigned) { x.queue.AdvanceProjectEpoch(); };
    const auto invalidated = x.Submit(request); x.queue.Drain(); x.Failed(invalidated, MMS_E_STALE);
    CHECK(x.displays[0] == 7 && x.reads[0] == x.releases[0] && refresh_checks::Empty(x.windows[0]));
}

static void IdentityAndValidation() {
    for (bool republish : {false, true}) {
        Fixture x; const auto pending = x.Submit(x.Current()); auto oldRecord = x.registry.properties.at(x.propertyIds[0]);
        auto replacement = x.Description(0); replacement.info.label = S("replacement with the same public handle");
        CHECK(x.ui.updateProperty(x.ui.context, x.properties[0], &replacement) == MMS_OK);
        CHECK(x.registry.properties.at(x.propertyIds[0])->handle == oldRecord->handle);
        if (republish) { x.Presentation(0).propertyRecord = x.registry.properties.at(x.propertyIds[0]); x.Sync(); }
        x.queue.Drain(); x.Failed(pending, MMS_E_STALE); CHECK(!x.reads[0] && x.displays[0] == -1);
    }
    Fixture x; MmsHandle unchanged = 919;
    auto bad = x.Current(); bad.sourceRevision = 1;
    CHECK(x.api.request(x.api.context, &bad, &unchanged) == MMS_E_ARGUMENT && unchanged == 919);
    bad = x.Current(); bad.binding = 1;
    CHECK(x.api.request(x.api.context, &bad, &unchanged) == MMS_E_ARGUMENT && unchanged == 919);
    bad = x.Request(x.Target()); bad.kinds = MMS_UI_REFRESH_DATA; bad.expectedGeometryRevision = 0;
    CHECK(x.api.request(x.api.context, &bad, &unchanged) == MMS_E_ARGUMENT && unchanged == 919);
    const auto pending = x.Submit(x.Current()); x.Bind(0); x.queue.Drain(); x.Failed(pending, MMS_E_STALE);
    bad = x.Current(); CHECK(x.api.request(x.api.context, &bad, &unchanged) == MMS_E_CONFLICT && unchanged == 919);
    bad.binding = x.bindings[0]; bad.sourceRevision = x.sourceRevisions[0];
    const auto explicitTicket = x.Submit(bad); x.queue.Drain();
    const auto state = x.State(explicitTicket);
    CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.sourceRevision == x.sourceRevisions[0] && state.completedKinds == bad.kinds);
    CHECK(x.BoundDisplay(0) == 400 && !x.reads[0] && x.prepares[0] == 1);
}

static void SubtreeAndBindingPriority() {
    Fixture x; const auto plain = x.Submit(x.Tree()); x.queue.Drain(); x.Done(plain, MMS_UI_REFRESH_DATA);
    CHECK(x.reads[0] == 1 && x.reads[1] == 1 && x.displays[0] == 7 && x.displays[1] == 9);
    x.Bind(0); x.values[0] = 30; x.values[1] = 31;
    const auto mixed = x.Submit(x.Tree(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT)); x.queue.Drain();
    x.Done(mixed, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT);
    CHECK(x.prepares[0] == 1 && x.reads[0] == 1 && x.reads[1] == 2 && x.displays[0] == 7 && x.displays[1] == 31);
    CHECK(x.BoundDisplay(0) == 400);
    const auto stale = x.Submit(x.Tree()); x.Bind(1); const auto before = x.reads;
    x.queue.Drain(); x.Failed(stale, MMS_E_STALE); CHECK(x.reads == before && !x.prepares[1]);
    Fixture unmounted;
    unmounted.presentations.erase(std::remove_if(unmounted.presentations.begin(), unmounted.presentations.end(),
        [&](const auto& view) { return view.element == unmounted.inputs[1]; }), unmounted.presentations.end());
    unmounted.Sync();
    auto request = unmounted.Tree(); MmsHandle unchanged = 994;
    CHECK(unmounted.api.request(unmounted.api.context, &request, &unchanged) == MMS_E_NOT_READY && unchanged == 994);
    Fixture noData; noData.Presentation(0).propertyDataRefresh = noData.Presentation(1).propertyDataRefresh = false; noData.Sync();
    request = noData.Tree(); CHECK(noData.api.request(noData.api.context, &request, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 994);
    CHECK(!unmounted.reads[0] && !unmounted.reads[1] && !noData.reads[0] && !noData.reads[1]);
}

static void SdkUsesOriginalObjects() {
    Fixture x; sdk::RefreshServices service; CHECK(sdk::RefreshServices::Query(x.Host(), service) == MMS_OK);
    CHECK(service.api().context == x.api.context);
    MmsUiRefreshTargetV1 target{}; CHECK(service.Target(x.inputs[0], target) == MMS_OK && target.target.element == x.inputs[0]);
    sdk::RefreshTicket ticket; MmsResult result = MMS_E_FAILED;
    std::thread worker([&] { result = service.RefreshPropertyCurrent(target.target, ticket, 512); }); worker.join();
    CHECK(result == MMS_OK && ticket && !x.reads[0]); const auto original = ticket.get();
    CHECK(x.State(original).status == MMS_UI_REFRESH_PENDING);
    CHECK(service.RefreshPropertyElementCurrent(x.inputs[0], ticket) == MMS_E_BUSY && ticket.get() == original);
    x.values[0] = 22; x.queue.Drain(); x.Done(original, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT);
    MmsUiRefreshStateV1 state{}; CHECK(ticket.Query(state) == MMS_OK && !state.sourceRevision && x.displays[0] == 22);
    CHECK(ticket.Release() == MMS_OK && !ticket);
    std::thread wrongThread([&] { result = service.RefreshPropertyElementCurrent(x.inputs[0], ticket); }); wrongThread.join();
    CHECK(result == MMS_E_THREAD && !ticket);
    CHECK(service.RefreshPropertyElementCurrent(x.element, ticket) == MMS_E_UNSUPPORTED && !ticket);
    CHECK(service.RefreshPropertyElementCurrent(x.inputs[0], ticket, 513) == MMS_OK && ticket);
    x.readResults[0] = MMS_E_FAILED; x.CleanAll(); x.queue.Drain();
    CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_FAILED && !state.completedKinds && !state.sourceRevision);
    CHECK(x.displays[0] == 22 && refresh_checks::Empty(x.windows[0])); CHECK(ticket.Release() == MMS_OK);
    x.Bind(0); CHECK(service.RefreshPropertyCurrent(target.target, ticket) == MMS_E_CONFLICT && !ticket);
    CHECK(!x.edits && !x.stages && x.playhead == 31);
}
} // namespace property_data_refresh_checks

static void PropertyDataRefreshChecks() {
    property_data_refresh_checks::WorkerConsumptionAndCoalescing();
    property_data_refresh_checks::ReadOnlyAndFailure();
    property_data_refresh_checks::IdentityAndValidation();
    property_data_refresh_checks::SubtreeAndBindingPriority();
    property_data_refresh_checks::SdkUsesOriginalObjects();
}
