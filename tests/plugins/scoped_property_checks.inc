// Original property records, retained graph and refresh queue. These fixtures
// own their HWNDs and do not construct a scene or claim hardware acceptance.
#include <mms/property_refresh.h>

namespace scoped_property_checks {
struct Fixture : property_refresh_checks::Fixture {
    MmsUiPropertyRefreshApiV1_1 scoped = notifications.ScopedApi(client);
    MmsUiRefreshDependencyApiV1 graph = queue.DependencyApi(client);
    MmsHandle trigger = 0, subscription = 0;
    MmsUiPropertyRefV1 triggerRef{};
    unsigned triggerReads = 0, layouts = 0;
    std::function<void(unsigned)> inspectState;
    std::vector<unsigned> trace;
    std::vector<MmsHandle> edges;
    Fixture() {
        auto d = Description(0); d.info.id = S("example.refresh.independent-trigger");
        d.info.flags = MMS_PROPERTY_READ_ONLY; d.context = this; d.stage = nullptr;
        d.releaseValue = [](void*, MmsHandle) {};
        d.read = [](void* p, MmsObjectRef, MmsValue* out, MmsHandle* lease) -> MmsResult {
            ++static_cast<Fixture*>(p)->triggerReads; *out = property_data_refresh_checks::Integer(91); *lease = 0; return MMS_OK;
        };
        CHECK(ui.registerProperty(ui.context, &d, &trigger) == MMS_OK);
        triggerRef.h = Header<MmsUiPropertyRefV1>();
        CHECK(scoped.getPropertyRef(scoped.context, trigger, &triggerRef) == MMS_OK);
        for (unsigned i = 0; i < 2; ++i) Presentation(i).stateRefresh = Presentation(i).layoutRefresh = true;
        queue.SetValidatedStateContent([this](MmsHandle element, const std::shared_ptr<const ButtonStateContent>&, bool,
            const std::function<MmsResult()>& validate) -> MmsResult {
            const unsigned i = element == inputs[0] ? 0 : 1; CHECK(element == inputs[i]);
            CHECK(registry.InCallback() && queue.ReadOnlyCause(client));
            if (inspectState) inspectState(i);
            trace.push_back(100 + i); return validate();
        });
        queue.SetLayout([this](MmsHandle anchor) -> MmsResult {
            CHECK(anchor == group || anchor == inputs[0] || anchor == inputs[1]);
            CHECK(registry.InCallback() && queue.ReadOnlyCause(client));
            ++layouts; trace.push_back(200); Sync(); return MMS_OK;
        });
        onRead = [this](unsigned i) { trace.push_back(10 + i); };
        Sync(); CleanAll();
    }
    MmsUiPropertyRefreshScopedSubscriptionDescV1 Desc(uint32_t scope = MMS_UI_REFRESH_DEPENDENTS, uint32_t kinds = 15, uint32_t flags = 0) {
        return {Header<MmsUiPropertyRefreshScopedSubscriptionDescV1>(), triggerRef, Target(group).target, scope, kinds, flags, 0, 0};
    }
    void Subscribe(uint32_t scope = MMS_UI_REFRESH_DEPENDENTS, uint32_t kinds = 15, uint32_t flags = 0) {
        const auto desc = Desc(scope, kinds, flags);
        CHECK(scoped.subscribeScoped(scoped.context, &desc, &subscription) == MMS_OK && subscription);
    }
    MmsUiPropertyRefreshScopedSubscriptionStateV1 Status() {
        MmsUiPropertyRefreshScopedSubscriptionStateV1 state{}; state.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
        CHECK(scoped.queryScoped(scoped.context, subscription, &state) == MMS_OK); return state;
    }
    void Complete() {
        for (unsigned n = 0; n < 10; ++n) {
            const auto s = Status();
            if (!s.pending && !s.lastResult && s.completedSequence == s.latestSequence && s.workGeneration == s.completedWorkGeneration) return;
            queue.Drain();
        }
        const auto s = Status(); std::cerr << "SCOPED_PROPERTY sequence=" << s.latestSequence << '/' << s.submittedSequence << '/' << s.completedSequence
            << " work=" << s.workGeneration << '/' << s.submittedWorkGeneration << '/' << s.completedWorkGeneration
            << " wait=" << s.waitingFor << " result=" << s.lastResult << '\n'; CHECK(false);
    }
    void Notify(unsigned count = 1, bool worker = true) {
        MmsResult result = MMS_OK;
        const auto run = [&] { for (unsigned i = 0; i < count && !result; ++i) result = scoped.notify(scoped.context, &triggerRef); };
        if (worker) { std::thread t(run); t.join(); } else run(); CHECK(result == MMS_OK);
    }
    MmsHandle Edge(MmsHandle to) {
        const MmsUiRefreshDependencyDescV1 desc{Header<MmsUiRefreshDependencyDescV1>(), Target(group).target, Target(to).target, 0, 0};
        MmsHandle edge = 0; CHECK(graph.registerDependency(graph.context, &desc, &edge) == MMS_OK && edge); edges.push_back(edge); return edge;
    }
    void StraightGraph() { Edge(inputs[0]); Edge(inputs[1]); }
    void Publish(unsigned i, int64_t value) {
        MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), sources[i], sourceRevisions[i], {&value, sizeof(value)}, 0, 0};
        MmsUiDataSourceStateV1 out{}; out.h = Header<MmsUiDataSourceStateV1>(); MmsResult result = MMS_E_FAILED;
        std::thread worker([&] { result = data.publishSource(data.context, &publish, &out); }); worker.join();
        CHECK(result == MMS_OK); sourceRevisions[i] = out.revision;
    }
    void Stale() {
        MmsUiPropertyRefreshScopedSubscriptionStateV1 out{}; out.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
        out.completedWorkGeneration = UINT64_C(0xf123456789abcdef); const auto before = out;
        CHECK(scoped.queryScoped(scoped.context, subscription, &out) == MMS_E_STALE && !std::memcmp(&out, &before, sizeof(out)));
    }
};
void ProtocolAndIndependentTrigger() {
    Fixture f; f.StraightGraph(); const auto registry = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    CHECK(f.changes.h.size == 64 && f.changes.h.minor == 0 && f.scoped.h.size == 88 && f.scoped.h.minor == 1);
    MmsUiPropertyRefreshScopedCapabilitiesV1 caps{}; caps.h = Header<MmsUiPropertyRefreshScopedCapabilitiesV1>();
    CHECK(f.scoped.getScopedCapabilities(f.scoped.context, &caps) == MMS_OK && caps.maxSubscriptionsPerPlugin == 128);
    CHECK(caps.supportedScopes == 7 && caps.supportedKinds == 15 && !caps.supportedFlags && !caps.flags && !caps.reserved && !caps.reservedTail);
    CHECK(!f.queue.Bindings().HasBinding(f.client, f.Target(f.group).target));
    for (const auto& entry : f.registry.elements) CHECK(String(entry.second.desc.propertyId) != "example.refresh.independent-trigger");
    f.Subscribe(); f.values = {31, 41}; f.Notify(73); CHECK(!f.reads[0] && !f.reads[1] && !f.triggerReads);
    f.values = {32, 42}; f.Complete();
    CHECK(f.trace == std::vector<unsigned>({10, 11, 100, 101, 200}) && f.layouts == 1);
    CHECK(f.displays[0] == 32 && f.displays[1] == 42 && f.reads[0] == 1 && f.reads[1] == 1 && !f.triggerReads);
    CHECK(f.Status().latestSequence == 74 && !f.Status().waitingFor && f.Status().target.element == f.group && f.Status().property.property == f.trigger);
    MmsUiPropertyRefreshSubscriptionStateV1 old{}; old.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>();
    CHECK(f.changes.query(f.changes.context, f.subscription, &old) == MMS_OK && old.completedSequence == 74 && old.target.element == f.group);
    MmsResult query = MMS_E_FAILED, capability = MMS_OK, subscribe = MMS_OK, unsubscribe = MMS_OK;
    const auto desc = f.Desc(); MmsHandle untouched = 719;
    std::thread worker([&] {
        auto out = f.Status(); query = f.scoped.queryScoped(f.scoped.context, f.subscription, &out);
        capability = f.scoped.getScopedCapabilities(f.scoped.context, &caps);
        subscribe = f.scoped.subscribeScoped(f.scoped.context, &desc, &untouched);
        unsubscribe = f.scoped.unsubscribe(f.scoped.context, f.subscription);
    }); worker.join();
    CHECK(query == MMS_OK && capability == MMS_E_THREAD && subscribe == MMS_E_THREAD && unsubscribe == MMS_E_THREAD && untouched == 719);
    CHECK(f.registry.Revision() == registry && f.registry.PreferencesRevision() == preferences && !f.edits && !f.stages && f.playhead == 31 && f.sceneRevision == 73);
    CHECK(f.changes.unsubscribe(f.changes.context, f.subscription) == MMS_OK); f.Stale();
}
void AcceptedMixedCohortAndConsumeCurrent() {
    Fixture f; f.StraightGraph(); f.Bind(1);
    const auto repaint = f.Request(f.Target()); for (unsigned n = 0; n < 64; ++n) f.Submit(repaint);
    f.Subscribe(MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE); f.queue.Drain();
    const auto accepted = f.Status(); CHECK(accepted.submittedSequence == 1 && !accepted.completedSequence && accepted.pending);
    CHECK(!f.reads[0] && !f.reads[1] && !f.prepares[1]);
    f.values = {71, 72}; f.Publish(1, 901); f.Notify(); f.values[0] = 81;
    f.queue.Drain();
    CHECK(f.Status().completedSequence == 1 && f.Status().latestSequence == 2 && f.Status().pending);
    CHECK(f.displays[0] == 81 && f.BoundDisplay(1) == 401 && f.reads[0] == 1 && !f.reads[1] && f.prepares[1] == 1);
    f.Complete(); CHECK(f.displays[0] == 81 && f.BoundDisplay(1) == 901 && f.reads[0] == 2 && !f.reads[1] && f.prepares[1] == 2);
    CHECK(f.data.unbind(f.data.context, f.bindings[1]) == MMS_OK); f.Complete();
    CHECK(f.displays[1] == 72 && f.reads[1] == 1 && f.Status().completedSequence == 2 && f.Status().completedWorkGeneration > accepted.submittedWorkGeneration);
}
void GraphChangesAndConsumerRecovery() {
    Fixture f; f.Subscribe(); f.queue.Drain(); CHECK(f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_GRAPH);
    const auto wakes = f.wakes.load(); for (unsigned n = 0; n < 8; ++n) f.queue.Drain();
    CHECK(!f.reads[0] && !f.reads[1] && !f.queue.Pending() && f.wakes.load() == wakes);
    const auto edge = f.Edge(f.inputs[0]); f.Complete(); const auto a = f.Status();
    CHECK(f.reads[0] == 1 && !f.reads[1] && a.latestSequence == 1);
    f.Edge(f.inputs[1]); f.Complete(); CHECK(f.reads[0] == 2 && f.reads[1] == 1 && f.Status().completedWorkGeneration > a.completedWorkGeneration);
    const auto beforeRemove = f.Status(); const auto beforeRemoveB = f.reads[1];
    CHECK(f.graph.unregisterDependency(f.graph.context, edge) == MMS_OK); const auto readsA = f.reads[0]; f.Complete();
    CHECK(f.reads[0] == readsA && f.reads[1] == beforeRemoveB + 1 && f.Status().latestSequence == beforeRemove.latestSequence);
    CHECK(f.Status().completedWorkGeneration > beforeRemove.completedWorkGeneration);
    f.values[1] = 93; f.readResults[1] = MMS_E_FAILED; f.Notify(); f.queue.Drain();
    CHECK(f.Status().lastResult == MMS_E_FAILED && (f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER));
    const auto reads = f.reads;
    for (unsigned n = 0; n < 8; ++n) { const auto r = f.Request(f.Target()); const auto h = f.Submit(r); f.queue.Drain(); f.Completed(h); }
    CHECK(f.reads == reads && !f.queue.Pending());
    f.readResults[1] = MMS_OK; f.Notify(); f.Complete(); CHECK(f.displays[1] == 93 && f.reads[1] == reads[1] + 1 && f.reads[0] == readsA);
}
void MissingExplicitSourceAndRebinding() {
    Fixture f; f.StraightGraph(); f.Bind(1);
    CHECK(f.data.unregisterSource(f.data.context, f.sources[1]) == MMS_OK);
    MmsUiDataSourceDescV1 source{Header<MmsUiDataSourceDescV1>(), S("example.scoped.property.cold"), 64, 0, 0};
    CHECK(f.data.registerSource(f.data.context, &source, &f.sources[1]) == MMS_OK); f.sourceRevisions[1] = 0;
    MmsUiDataBindingDescV1 binding{}; binding.h = Header<MmsUiDataBindingDescV1>(); binding.source = f.sources[1];
    binding.target = f.Target(f.inputs[1]).target; binding.context = &f; binding.maxDisplayBytes = 64;
    binding.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult { auto& f = *static_cast<Fixture*>(p); ++f.prepares[1]; return f.data.writePrepared(f.data.context, input->builder, input->sourceBytes); };
    CHECK(f.data.bind(f.data.context, &binding, &f.bindings[1]) == MMS_OK); f.Subscribe(); f.queue.Drain();
    CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE) && !f.reads[0] && !f.reads[1]);
    const auto wakes = f.wakes.load(); for (unsigned n = 0; n < 8; ++n) f.queue.Drain(); CHECK(!f.queue.Pending() && f.wakes.load() == wakes);
    f.Publish(1, 721); f.Complete(); CHECK(f.displays[0] == 7 && f.BoundDisplay(1) == 721 && f.Status().completedSequence == 1);
    CHECK(f.data.unbind(f.data.context, f.bindings[1]) == MMS_OK); f.values[1] = 722; f.Complete();
    CHECK(f.displays[1] == 722 && f.reads[1] == 1 && f.Status().latestSequence == 1);
}
void ReadOnlyValidationAndLifecycle() {
    Fixture f; f.StraightGraph(); auto bad = f.Desc(); MmsHandle out = 913;
    bad.flags = MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY; CHECK(f.scoped.subscribeScoped(f.scoped.context, &bad, &out) == MMS_E_UNSUPPORTED && out == 913);
    bad = f.Desc(); bad.property.registrationGeneration++; CHECK(f.scoped.subscribeScoped(f.scoped.context, &bad, &out) == MMS_E_STALE && out == 913);
    bad = f.Desc(); bad.seed.instanceGeneration++; CHECK(f.scoped.subscribeScoped(f.scoped.context, &bad, &out) == MMS_E_STALE && out == 913);
    bad = f.Desc(); bad.kinds = MMS_UI_REFRESH_REPAINT; CHECK(f.scoped.subscribeScoped(f.scoped.context, &bad, &out) == MMS_E_ARGUMENT && out == 913);
    bad = f.Desc(); bad.reservedTail = 1; CHECK(f.scoped.subscribeScoped(f.scoped.context, &bad, &out) == MMS_E_ARGUMENT && out == 913);
    auto foreign = f.notifications.ScopedApi(f.other); bad = f.Desc(); CHECK(foreign.subscribeScoped(foreign.context, &bad, &out) != MMS_OK && out == 913);
    f.onRead = [&](unsigned) {
        CHECK(f.scoped.notify(f.scoped.context, &f.triggerRef) == MMS_E_BUSY);
        MmsResult result = MMS_OK; std::thread t([&] { result = f.scoped.notify(f.scoped.context, &f.triggerRef); }); t.join(); CHECK(result == MMS_E_BUSY);
        CHECK(f.scoped.subscribeScoped(f.scoped.context, &bad, &out) == MMS_E_BUSY && out == 913);
        CHECK(f.scoped.unsubscribe(f.scoped.context, f.subscription) == MMS_E_BUSY);
        CHECK(f.graph.unregisterDependency(f.graph.context, f.edges[0]) == MMS_E_BUSY); f.queue.Drain();
    };
    f.Subscribe(); f.Complete(); CHECK(f.reads[0] == 1 && f.reads[1] == 1 && !f.queue.Pending());
    for (unsigned mode = 0; mode < 6; ++mode) {
        Fixture x; x.StraightGraph(); const auto r = x.Request(x.Target()); for (unsigned n = 0; n < 64; ++n) x.Submit(r);
        x.Subscribe(); x.queue.Drain(); CHECK(x.Status().submittedSequence == 1 && !x.reads[0] && !x.reads[1]);
        if (mode == 0) CHECK(x.changes.unsubscribe(x.changes.context, x.subscription) == MMS_OK);
        else if (mode == 1) x.queue.InvalidateTarget(x.group);
        else if (mode == 2) CHECK(x.ui.unregisterItem(x.ui.context, x.trigger) == MMS_OK);
        else if (mode == 3) { auto d = x.registry.properties.at("example.refresh.independent-trigger")->desc; d.info.label = S("replacement trigger"); CHECK(x.ui.updateProperty(x.ui.context, x.trigger, &d) == MMS_OK); }
        else if (mode == 4) x.queue.AdvanceProjectEpoch();
        else x.queue.Retire(x.client);
        x.Stale(); x.queue.Drain(); CHECK(!x.reads[0] && !x.reads[1] && !x.queue.Pending());
    }
}
void CapacityTargetAndWakeRecovery() {
    {
        Fixture f; f.StraightGraph(); MmsUiRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshCapabilitiesV1>(); CHECK(f.api.getCapabilities(f.api.context, &caps) == MMS_OK);
        const auto repaint = f.Request(f.Target()); for (unsigned n = 0; n < caps.maxTicketsPerPlugin; ++n) f.Submit(repaint);
        for (unsigned n = 0; n < caps.maxTicketsPerPlugin; n += caps.maxContributionsPerDrain) f.queue.Drain();
        f.Subscribe(); f.queue.Drain(); CHECK(f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY);
        const auto wakes = f.wakes.load(); for (unsigned n = 0; n < 8; ++n) f.queue.Drain(); CHECK(!f.queue.Pending() && f.wakes.load() == wakes && !f.reads[0]);
        CHECK(f.api.release(f.api.context, f.tickets.back()) == MMS_OK); f.tickets.pop_back(); f.Complete(); CHECK(f.reads[0] == 1 && f.reads[1] == 1);
        f.queue.SuspendTargets(); f.Notify(); f.queue.Drain(); CHECK(f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_TARGET);
        const auto reads = f.reads; for (unsigned n = 0; n < 8; ++n) f.queue.Drain(); CHECK(f.reads == reads && !f.queue.Pending());
        f.Sync(); f.Complete(); CHECK(f.reads[0] == reads[0] + 1 && f.reads[1] == reads[1] + 1);
    }
    {
        Fixture f; f.StraightGraph(); f.queue.SetWake([&] { ++f.wakes; return false; }); f.Subscribe();
        CHECK(f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE); const auto wakes = f.wakes.load();
        for (unsigned n = 0; n < 8; ++n) f.queue.Drain(); CHECK(!f.reads[0] && !f.queue.Pending() && f.wakes.load() == wakes);
        f.queue.SetWake([&] { ++f.wakes; return true; }); f.Notify(); f.Complete(); CHECK(f.reads[0] == 1 && f.reads[1] == 1);
    }
}
void NativeHistoryAndDraft() {
    for (uint32_t scope : {MMS_UI_REFRESH_SUBTREE, MMS_UI_REFRESH_DEPENDENTS}) {
        input_state_content_checks::Fixture f(true, false, true);
        auto& store = f.queue.PropertyRefresh(); CHECK(store.BindClient(f.client, f.data.owner) == MMS_OK);
        auto api = store.ScopedApi(f.client); MmsUiPropertyRefV1 ref{}; ref.h = Header<MmsUiPropertyRefV1>();
        CHECK(api.getPropertyRef(api.context, f.data.property, &ref) == MMS_OK); store.SetHistoryAvailable(true);
        CHECK(f.history.SetPublicationSignal({&store, [](void* p, uint64_t revision) noexcept {
            auto& store = *static_cast<UiPropertyRefresh*>(p); store.NotifyHistory(revision); store.HistoryWakeResult(true);
        }}) == MMS_OK);
        auto graph = f.queue.DependencyApi(f.client);
        if (scope == MMS_UI_REFRESH_DEPENDENTS) for (auto member : {f.data.element, f.peer}) {
            MmsUiRefreshDependencyDescV1 edge{Header<MmsUiRefreshDependencyDescV1>(), f.Target(f.group).target, f.Target(member).target, 0, 0}; MmsHandle handle = 0;
            CHECK(graph.registerDependency(graph.context, &edge, &handle) == MMS_OK);
        }
        MmsUiPropertyRefreshScopedSubscriptionDescV1 desc{Header<MmsUiPropertyRefreshScopedSubscriptionDescV1>(), ref, f.Target(f.group).target,
            scope, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY, 0, 0};
        MmsHandle watch = 0; CHECK(api.subscribeScoped(api.context, &desc, &watch) == MMS_OK);
        const MmsUiPropertyRefreshSubscriptionDescV1 manualDesc{Header<MmsUiPropertyRefreshSubscriptionDescV1>(), f.Target(f.peer).target, ref,
            MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, 0, 0};
        MmsHandle manual = 0; CHECK(api.subscribe(api.context, &manualDesc, &manual) == MMS_OK);
        const auto manualStatus = [&] { MmsUiPropertyRefreshSubscriptionStateV1 s{}; s.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>(); CHECK(api.query(api.context, manual, &s) == MMS_OK); return s; };
        const auto status = [&] { MmsUiPropertyRefreshScopedSubscriptionStateV1 s{}; s.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>(); CHECK(api.queryScoped(api.context, watch, &s) == MMS_OK); return s; };
        const auto complete = [&] { for (unsigned n = 0; n < 8; ++n) { const auto s = status();
            if (!s.pending && !s.lastResult && s.latestSequence == s.submittedSequence && s.completedSequence == s.latestSequence &&
                s.workGeneration && s.workGeneration == s.submittedWorkGeneration && s.completedWorkGeneration == s.workGeneration) return;
            f.queue.Drain(); } CHECK(false); };
        complete(); const auto field = f.field; const auto peer = native_property_input_checks::FindField(input_state_content_checks::Wrapper(f.native->Toolbar(), L"Peer input"));
        const auto initial = status().completedSequence; const auto manualSequence = manualStatus().latestSequence;
        const auto change = [&](int64_t value, SharedHistory::NativeIntent intent) {
            auto c = std::make_shared<input_state_content_checks::IntChange>(f.data.value, f.observation, value);
            std::unique_ptr<SharedHistory::NativeTransaction> token;
            CHECK(f.history.PrepareNative("scoped property actual history", c, token, intent) == MMS_OK);
            CHECK(f.history.CommitNative(std::move(token)) == MMS_OK);
        };
        change(8, SharedHistory::NativeIntent::ExplicitCommit); CHECK(native_property_input_checks::Caption(field) == L"2");
        complete(); CHECK(native_property_input_checks::Caption(field) == L"8" && native_property_input_checks::Caption(peer) == L"8" && status().completedSequence > initial);
        CHECK(manualStatus().latestSequence == manualSequence && !manualStatus().pending);
        CHECK(f.history.History(false) == MMS_OK); complete(); CHECK(native_property_input_checks::Caption(field) == L"2" && native_property_input_checks::Caption(peer) == L"2");
        CHECK(f.history.History(true) == MMS_OK); complete(); CHECK(native_property_input_checks::Caption(field) == L"8" && native_property_input_checks::Caption(peer) == L"8");
        CHECK(f.history.SetMode(SharedHistory::RecordingMode::ExplicitOnly) == MMS_OK); const auto undo = f.history.UndoCount();
        input_state_content_checks::SetRaw(field, L"7"); SendMessageW(field, EM_SETSEL, 0, 1); SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0);
        change(9, SharedHistory::NativeIntent::Adjustment); complete();
        CHECK(native_property_input_checks::Caption(field) == L"7" && native_property_input_checks::Caption(peer) == L"9" && f.history.UndoCount() == undo);
        DWORD start = 99, end = 99; SendMessageW(field, EM_GETSEL, WPARAM(&start), LPARAM(&end));
        CHECK(GetFocus() == field && start == 0 && end == 1);
        SendMessageW(field, WM_IME_ENDCOMPOSITION, 0, 0); native_property_input_checks::Escape(field);
        CHECK(native_property_input_checks::Caption(field) == L"9" && f.field == field && !f.data.edits);
        CHECK(manualStatus().latestSequence == manualSequence && !manualStatus().pending);
        const auto sequence = status().latestSequence; std::unique_ptr<SharedHistory::NativeTransaction> cancelled;
        auto c = std::make_shared<input_state_content_checks::IntChange>(f.data.value, f.observation, 10);
        CHECK(f.history.PrepareNative("cancelled history", c, cancelled) == MMS_OK); cancelled.reset();
        f.queue.Drain(); CHECK(status().latestSequence == sequence && native_property_input_checks::Caption(field) == L"9");
        CHECK(f.history.SetPublicationSignal({}) == MMS_OK); CHECK(api.unsubscribe(api.context, watch) == MMS_OK);
        CHECK(api.unsubscribe(api.context, manual) == MMS_OK);
    }
}
void CaptureContributionCapacity() {
    Fixture f; f.StraightGraph(); f.Bind(0); f.Bind(1);
    MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
    CHECK(f.data.getCapabilities(f.data.context, &caps) == MMS_OK && caps.maxContributionsPerPlugin == 256);
    auto request = f.Request(f.Target(f.group)); request.expectedGeometryRevision = 0;
    request.scope = MMS_UI_REFRESH_DEPENDENTS; request.kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT;
    for (unsigned n = 0; n < caps.maxContributionsPerPlugin / 2; ++n) f.Submit(request);
    // Each accepted request owns two actual captured contributions. Inspect
    // the automatic mailbox before the old group's Finish releases them.
    bool classified = false;
    f.inspectState = [&](unsigned) {
        if (classified) return;
        const auto state = f.Status();
        CHECK(state.lastResult == MMS_E_BUSY && !state.submittedSequence && state.pending);
        CHECK((state.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY) && !(state.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY));
        classified = true;
    };
    f.Subscribe(); f.queue.Drain(); CHECK(classified);
    f.Complete(); CHECK(f.Status().completedSequence == 1 && !f.reads[0] && !f.reads[1]);
    CHECK(f.prepares[0] == 1 && f.prepares[1] == 1 && f.BoundDisplay(0) == 400 && f.BoundDisplay(1) == 401);
}
void DisplayFailureDoesNotRetryItsOwnReleasedContribution() {
    struct Context { Fixture* fixture = nullptr; bool fail = false; unsigned calls = 0; } context;
    Fixture f; context.fixture = &f; f.StraightGraph(); f.Bind(1);
    CHECK(f.data.unbind(f.data.context, f.bindings[1]) == MMS_OK);
    MmsUiDataBindingDescV1 binding{}; binding.h = Header<MmsUiDataBindingDescV1>(); binding.source = f.sources[1];
    binding.target = f.Target(f.inputs[1]).target; binding.context = &context; binding.maxDisplayBytes = 64;
    binding.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
        auto& c = *static_cast<Context*>(p); ++c.calls; auto& f = *c.fixture;
        const auto hits = overflow_allocation_fault::hits;
        if (c.fail) overflow_allocation_fault::bytes = size_t(input->sourceBytes.bytes);
        const auto result = f.data.writePrepared(f.data.context, input->builder, input->sourceBytes);
        overflow_allocation_fault::bytes = 0;
        if (c.fail) CHECK(result == MMS_E_MEMORY && overflow_allocation_fault::hits == hits + 1);
        return result;
    };
    CHECK(f.data.bind(f.data.context, &binding, &f.bindings[1]) == MMS_OK); f.Subscribe(); f.Complete();
    CHECK(context.calls == 1 && f.BoundDisplay(1) == 401);
    context.fail = true; f.Publish(1, 501); f.Notify(); f.queue.Drain();
    CHECK(context.calls == 2 && f.BoundDisplay(1) == 401 && f.Status().lastResult == MMS_E_MEMORY);
    CHECK(f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY);
    const auto reads = f.reads; const auto work = f.Status().workGeneration;
    for (unsigned n = 0; n < 8; ++n) { const auto ticket = f.Submit(f.Request(f.Target())); f.queue.Drain(); f.Completed(ticket); }
    CHECK(context.calls == 2 && f.reads == reads && f.Status().workGeneration == work && !f.queue.Pending());
    // Replacing the retained source really releases its failed version. That
    // event may recover this same property sequence; merely finishing the
    // failed ticket and releasing its contribution must not do so.
    context.fail = false; f.Publish(1, 502); f.Complete();
    CHECK(context.calls == 3 && f.BoundDisplay(1) == 502 && f.Status().completedSequence == 2);
}
void ObservationFailurePreservesParkedConsumer() {
    Fixture f; f.StraightGraph(); f.Bind(1); f.Subscribe(); f.Complete();
    f.values[0] = 88; f.readResults[0] = MMS_E_FAILED; f.Notify(); f.queue.Drain();
    const auto parked = f.Status(); CHECK(parked.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER); const auto reads = f.reads;
    const auto unchanged = [&] {
        const auto now = f.Status();
        CHECK(now.workGeneration == parked.workGeneration && now.submittedWorkGeneration == parked.submittedWorkGeneration &&
            now.completedWorkGeneration == parked.completedWorkGeneration && now.submittedDependencyRevision == parked.submittedDependencyRevision);
        CHECK(now.latestSequence == parked.latestSequence && now.submittedSequence == parked.submittedSequence &&
            now.completedSequence == parked.completedSequence && now.lastResult == parked.lastResult &&
            now.waitingFor == parked.waitingFor && now.pending == parked.pending);
        CHECK(f.reads == reads && !f.queue.Pending());
    };
    const auto hits = overflow_allocation_fault::hits;
    // The two-member watch reserves two real PropertySource records. Its
    // temporary observation failure is not a property/source/provider event.
    overflow_allocation_fault::bytes = 2 * sizeof(UiRefreshBindings::PropertySource);
    f.queue.Drain(); overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == hits + 1);
    unchanged();
    for (unsigned n = 0; n < 4; ++n) { f.queue.Drain(); unchanged(); }
    f.readResults[0] = MMS_OK; f.Notify(); f.Complete();
    CHECK(f.displays[0] == 88 && f.reads[0] == reads[0] + 1 && f.Status().latestSequence == parked.latestSequence + 1 &&
        f.Status().workGeneration > parked.workGeneration);
}
void BindingEventSurvivesObservationFailure() {
    for (unsigned mode = 0; mode < 3; ++mode) {
    const bool failObservation = mode != 0, failWake = mode == 2;
    for (const bool consumeCold : {false, true}) {
    Fixture f;
    for (unsigned i = 0; i < 2; ++i) f.Presentation(i).propertyDataRefresh = false;
    f.Sync(); f.StraightGraph(); f.Subscribe(); f.queue.Drain();
    CHECK((f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_BINDING) && !f.queue.Pending());
    const auto wakes = f.wakes.load(); const auto sequence = f.Status().latestSequence;
    MmsUiDataSourceDescV1 source{Header<MmsUiDataSourceDescV1>(), S("example.scoped.property.late-cold"), 64, 0, 0};
    CHECK(f.data.registerSource(f.data.context, &source, &f.sources[1]) == MMS_OK);
    MmsUiDataBindingDescV1 binding{}; binding.h = Header<MmsUiDataBindingDescV1>(); binding.source = f.sources[1];
    binding.target = f.Target(f.inputs[1]).target; binding.context = &f; binding.maxDisplayBytes = 64;
    binding.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
        auto& x = *static_cast<Fixture*>(p); ++x.prepares[1];
        return x.data.writePrepared(x.data.context, input->builder, input->sourceBytes);
    };
    const auto hits = overflow_allocation_fault::hits;
    if (failWake) f.queue.SetWake([&] { ++f.wakes; return false; });
    if (failObservation) overflow_allocation_fault::bytes = 2 * sizeof(UiRefreshBindings::PropertySource);
    const auto result = f.data.bind(f.data.context, &binding, &f.bindings[1]); overflow_allocation_fault::bytes = 0;
    CHECK(result == MMS_OK && overflow_allocation_fault::hits == hits + unsigned(failObservation));
    // A successful complete observation already knows a source is cold;
    // the real binding event must not force an unnecessary probe in that case.
    if (!failObservation) CHECK((f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE) && !f.queue.Pending());
    if (failWake) {
        CHECK((f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) && !f.queue.Pending());
        const auto wakeAttempts = f.wakes.load();
        for (unsigned n = 0; n < 4; ++n) {
            f.queue.Drain(); CHECK((f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) && !f.queue.Pending() && f.wakes.load() == wakeAttempts);
        }
        f.queue.SetWake([&] { ++f.wakes; return true; });
    } else if (consumeCold && failObservation) {
        CHECK(f.queue.Pending()); f.queue.Drain();
        CHECK((f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE) && !f.queue.Pending());
    }
    // Test publication before UI consumption and after consuming a cold
    // source. Neither ordering may require an extra Notify or manual wake.
    f.Publish(1, 721); CHECK(f.queue.Pending() && f.wakes.load() > wakes);
    f.Complete(); CHECK(f.BoundDisplay(1) == 721 && f.Status().completedSequence == sequence && !f.reads[0] && !f.reads[1]);
    // Source removal is also a real binding event, including when its watch
    // observation fails. It must not remain parked on the old missing source.
    const auto removeHits = overflow_allocation_fault::hits;
    overflow_allocation_fault::bytes = 2 * sizeof(UiRefreshBindings::PropertySource);
    const auto removed = f.data.unregisterSource(f.data.context, f.sources[1]); overflow_allocation_fault::bytes = 0;
    CHECK(removed == MMS_OK && overflow_allocation_fault::hits == removeHits + 1 && f.queue.Pending());
    f.queue.Drain(); CHECK((f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_BINDING) && !f.queue.Pending());
    }
    }
}
void UnrelatedBindingPreservesParkedConsumer() {
    for (const bool seedBinding : {false, true}) {
    Fixture f;
    if (seedBinding) {
        const MmsUiRefreshDependencyDescV1 edge{Header<MmsUiRefreshDependencyDescV1>(),
            f.Target(f.inputs[1]).target, f.Target(f.inputs[0]).target, 0, 0};
        MmsHandle handle = 0; CHECK(f.graph.registerDependency(f.graph.context, &edge, &handle) == MMS_OK);
        f.edges.push_back(handle); auto desc = f.Desc(); desc.seed = edge.upstream;
        CHECK(f.scoped.subscribeScoped(f.scoped.context, &desc, &f.subscription) == MMS_OK);
    } else { f.Edge(f.inputs[0]); f.Subscribe(); }
    f.Complete();
    f.readResults[0] = MMS_E_FAILED; f.Notify(); f.queue.Drain();
    const auto before = f.Status(); const auto reads = f.reads;
    CHECK(before.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER);
    const auto unchanged = [&] {
        const auto now = f.Status(); CHECK(!f.queue.Pending() && f.reads == reads && now.workGeneration == before.workGeneration &&
            now.waitingFor == before.waitingFor && now.lastResult == before.lastResult && now.latestSequence == before.latestSequence);
    };
    f.Bind(1); unchanged(); f.Publish(1, 812); unchanged(); f.queue.Drain(); unchanged();
    CHECK(f.data.unbind(f.data.context, f.bindings[1]) == MMS_OK); unchanged();
    CHECK(f.data.unregisterSource(f.data.context, f.sources[1]) == MMS_OK); unchanged(); f.queue.Drain(); unchanged();
    }
}
void SharedQuotaAndTopologyRecovery() {
    {
        Fixture f; static_cast<property_refresh_checks::Fixture&>(f).Subscribe(0);
        MmsUiPropertyRefreshScopedSubscriptionDescV1 last{};
        for (unsigned n = 0; n < 128; ++n) {
            const std::string id = "example.refresh.quota-trigger." + std::to_string(n);
            auto d = f.Description(0); d.info.id = Text(id); MmsHandle property = 0;
            CHECK(f.ui.registerProperty(f.ui.context, &d, &property) == MMS_OK);
            MmsUiPropertyRefV1 ref{}; ref.h = Header<MmsUiPropertyRefV1>();
            CHECK(f.scoped.getPropertyRef(f.scoped.context, property, &ref) == MMS_OK);
            last = f.Desc(MMS_UI_REFRESH_SUBTREE, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT); last.property = ref;
            MmsHandle out = 731; const auto result = f.scoped.subscribeScoped(f.scoped.context, &last, &out);
            if (n < 127) CHECK(result == MMS_OK && out != 731 && out);
            else CHECK(result == MMS_E_BUSY && out == 731);
        }
        CHECK(f.changes.unsubscribe(f.changes.context, f.subscriptions[0]) == MMS_OK);
        MmsHandle out = 0; CHECK(f.scoped.subscribeScoped(f.scoped.context, &last, &out) == MMS_OK && out);
    }
    {
        Fixture f; f.StraightGraph(); f.Subscribe(); f.Complete();
        size_t allocation = 0;
        for (const auto& entry : f.registry.elements) {
            if (entry.second.desc.parentId.bytes <= 15) continue;
            const auto parent = String(entry.second.desc.parentId); allocation = parent.capacity() + 1; break;
        }
        CHECK(allocation && !overflow_allocation_fault::bytes); const auto hits = overflow_allocation_fault::hits;
        overflow_allocation_fault::bytes = allocation; f.queue.SyncTopology(); overflow_allocation_fault::bytes = 0;
        CHECK(overflow_allocation_fault::hits == hits + 1);
        f.Notify(); f.queue.Drain(); CHECK(f.Status().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_TARGET);
        CHECK(!f.queue.Pending()); const auto reads = f.reads; const auto wakes = f.wakes.load();
        f.queue.SyncTopology(); CHECK(f.queue.Pending() && f.wakes.load() > wakes && f.reads == reads);
        f.Complete(); CHECK(f.reads[0] == reads[0] + 1 && f.reads[1] == reads[1] + 1);
    }
}
} // namespace scoped_property_checks
static void ScopedPropertyChecks() {
    scoped_property_checks::ProtocolAndIndependentTrigger();
    scoped_property_checks::AcceptedMixedCohortAndConsumeCurrent();
    scoped_property_checks::GraphChangesAndConsumerRecovery();
    scoped_property_checks::MissingExplicitSourceAndRebinding();
    scoped_property_checks::ReadOnlyValidationAndLifecycle();
    scoped_property_checks::CapacityTargetAndWakeRecovery();
    scoped_property_checks::NativeHistoryAndDraft();
    scoped_property_checks::CaptureContributionCapacity();
    scoped_property_checks::DisplayFailureDoesNotRetryItsOwnReleasedContribution();
    scoped_property_checks::ObservationFailurePreservesParkedConsumer();
    scoped_property_checks::BindingEventSurvivesObservationFailure();
    scoped_property_checks::UnrelatedBindingPreservesParkedConsumer();
    scoped_property_checks::SharedQuotaAndTopologyRecovery();
}
