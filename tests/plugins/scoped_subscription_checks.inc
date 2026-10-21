// The original queue/graph/bindings with fixture-owned HWNDs. These tests
// exercise actual automatic composite tickets, not a list of SELF requests.
#include <mms/refresh_subscription.h>

namespace scoped_subscription_checks {
struct Fixture : ui_refresh_dependency_checks::Fixture {
    MmsUiRefreshSubscriptionApiV1_1 subscriptions = queue.Bindings().ScopedSubscriptionApi(client);
    MmsHandle trigger = 0, triggerBinding = 0, subscription = 0;
    HWND triggerWindow = nullptr;
    unsigned triggerPrepares = 0;
    std::vector<unsigned> trace;
    std::vector<std::pair<unsigned, uint64_t>> consumed;
    std::function<MmsResult(unsigned)> prepare;
    Fixture() {
        auto e = Button("example.refresh.scoped-trigger"); e.parentId = S("mms.toolbar.main.extensions");
        CHECK(ui.registerElement(ui.context, &e, &trigger) == MMS_OK);
        triggerWindow = CreateWindowExW(0, L"BUTTON", L"Actual trigger", WS_CHILD | WS_VISIBLE,
            10, 12, 120, 24, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(triggerWindow);
        RefreshPresentation p{trigger, owner.id, 201, triggerWindow, 96, 10, 12, 120, 24, true};
        p.kind = MMS_UI_BUTTON; p.stateRefresh = p.layoutRefresh = true; presentations.push_back(p); queue.SyncTargets(presentations);
        MmsUiDataBindingDescV1 desc{}; desc.h = Header<MmsUiDataBindingDescV1>(); desc.target = Target(trigger).target;
        desc.source = sources[0]; desc.context = this; desc.maxDisplayBytes = 64;
        desc.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
            auto& x = *static_cast<Fixture*>(p); ++x.triggerPrepares;
            CHECK(input->target.element == x.trigger && input->source == x.sources[0]);
            return x.data.writePrepared(x.data.context, input->builder, input->sourceBytes);
        };
        CHECK(data.bind(data.context, &desc, &triggerBinding) == MMS_OK);
        onPrepare = [this](unsigned i) -> MmsResult {
            trace.push_back(10 + i); consumed.emplace_back(i, preparedRevisions[i]);
            return prepare ? prepare(i) : MMS_OK;
        };
        onState = [this] { trace.push_back(100); };
        queue.SetLayout([this](MmsHandle anchor) -> MmsResult {
            CHECK(anchor != trigger && registry.InCallback() && queue.ReadOnlyCause(client));
            trace.push_back(200); layoutAnchors.push_back(anchor); ++layouts;
            queue.SyncTargets(presentations); return MMS_OK;
        });
        CleanAll(); UpdateWindow(triggerWindow); ValidateRect(triggerWindow, nullptr);
    }
    MmsUiRefreshScopedSubscriptionDescV1 Desc(uint32_t scope = MMS_UI_REFRESH_DEPENDENTS,
        uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT) {
        MmsUiRefreshScopedSubscriptionDescV1 desc{}; desc.h = Header<MmsUiRefreshScopedSubscriptionDescV1>();
        desc.binding = triggerBinding; desc.seed = Target(group).target; desc.scope = scope; desc.kinds = kinds; return desc;
    }
    void Subscribe(uint32_t scope = MMS_UI_REFRESH_DEPENDENTS, uint32_t kinds = 15) {
        const auto desc = Desc(scope, kinds);
        CHECK(subscriptions.subscribeScoped(subscriptions.context, &desc, &subscription) == MMS_OK && subscription);
    }
    MmsUiRefreshScopedSubscriptionStateV1 Status() {
        MmsUiRefreshScopedSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshScopedSubscriptionStateV1>();
        CHECK(subscriptions.queryScoped(subscriptions.context, subscription, &state) == MMS_OK); return state;
    }
    void Complete() {
        for (unsigned n = 0; n < 10; ++n) {
            const auto state = Status();
            if (!state.pending && !state.lastResult && state.latestRevision == state.completedRevision &&
                state.workGeneration == state.completedWorkGeneration) return;
            queue.Drain();
        }
        const auto state = Status();
        std::cerr << "SCOPED completion latest=" << state.latestRevision << " submitted=" << state.submittedRevision
            << " complete=" << state.completedRevision << " generation=" << state.workGeneration << "/"
            << state.submittedWorkGeneration << "/" << state.completedWorkGeneration << " wait=" << state.waitingFor
            << " result=" << state.lastResult << '\n'; CHECK(false);
    }
    void WorkerPublish(unsigned i, int value) {
        MmsUiDataSourcePublishV1 desc{Header<MmsUiDataSourcePublishV1>(), sources[i], revisions[i], {&value, sizeof(value)}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>(); MmsResult result = MMS_E_FAILED;
        std::thread worker([&] { result = data.publishSource(data.context, &desc, &state); }); worker.join();
        CHECK(result == MMS_OK); revisions[i] = state.revision;
    }
    void Stale() {
        MmsUiRefreshScopedSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshScopedSubscriptionStateV1>(); state.workGeneration = 991;
        const auto before = state; CHECK(subscriptions.queryScoped(subscriptions.context, subscription, &state) == MMS_E_STALE);
        CHECK(!std::memcmp(&state, &before, sizeof(state)));
    }
    void StraightGraph() { Edge(group, element); Edge(group, second); }
};
void OrderedCompositeAndThreads() {
    Fixture f; f.Diamond(); const auto revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    CHECK(f.subscriptions.h.size == 64 && f.subscriptions.h.major == 1 && f.subscriptions.h.minor == 1);
    MmsUiRefreshSubscriptionCapabilitiesV1 oldCaps{}, caps{}; oldCaps.h = caps.h = Header<MmsUiRefreshSubscriptionCapabilitiesV1>();
    const auto old = f.queue.Bindings().SubscriptionApi(f.client);
    CHECK(old.h.size == 48 && old.h.minor == 0 && old.getCapabilities(old.context, &oldCaps) == MMS_OK);
    CHECK(oldCaps.supportedScopes == MMS_UI_REFRESH_SELF);
    CHECK(f.subscriptions.getCapabilities(f.subscriptions.context, &caps) == MMS_OK && caps.maxSubscriptionsPerPlugin == 128);
    CHECK(caps.supportedScopes == (MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS));
    CHECK(!f.queue.Bindings().HasBinding(f.client, f.Target(f.group).target));
    f.Subscribe(); const auto initial = f.Status();
    CHECK(initial.seed.element == f.group && initial.binding == f.triggerBinding && initial.scope == MMS_UI_REFRESH_DEPENDENTS && initial.pending);
    CHECK(initial.latestRevision == 1 && !initial.completedRevision && initial.workGeneration);
    f.Complete();
    CHECK(f.trace == std::vector<unsigned>({10, 11, 100, 100, 200}) && f.triggerPrepares == 0);
    CHECK(f.Display(0) == 10 && f.Display(1) == 11 && f.prepares[0] == 1 && f.prepares[1] == 1);
    CHECK(f.layoutAnchors.size() == 1 && f.layoutAnchors[0] == f.element && refresh_checks::Empty(f.triggerWindow));
    const auto before = f.Status(); f.trace.clear(); const auto reads = f.prepares;
    f.WorkerPublish(0, 21); CHECK(f.prepares == reads && f.Status().latestRevision == 2);
    f.Complete(); CHECK(f.Display(0) == 21 && f.Status().completedRevision == 2 && !f.Status().pending && !f.triggerPrepares);
    CHECK(f.Status().completedWorkGeneration > before.completedWorkGeneration);
    const auto desc = f.Desc(); MmsHandle output = 0x9988;
    CHECK(f.subscriptions.subscribeScoped(f.subscriptions.context, &desc, &output) == MMS_E_CONFLICT && output == 0x9988);
    auto alias = f.queue.Bindings().ScopedSubscriptionApi(f.other);
    CHECK(alias.subscribeScoped(alias.context, &desc, &output) != MMS_OK && output == 0x9988);
    MmsResult add = MMS_OK, remove = MMS_OK, capabilities = MMS_OK, query = MMS_E_FAILED;
    std::thread worker([&] {
        add = f.subscriptions.subscribeScoped(f.subscriptions.context, &desc, &output);
        remove = f.subscriptions.unsubscribe(f.subscriptions.context, f.subscription);
        capabilities = f.subscriptions.getCapabilities(f.subscriptions.context, &caps);
        MmsUiRefreshScopedSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshScopedSubscriptionStateV1>();
        query = f.subscriptions.queryScoped(f.subscriptions.context, f.subscription, &state);
    }); worker.join();
    CHECK(add == MMS_E_THREAD && remove == MMS_E_THREAD && capabilities == MMS_E_THREAD && query == MMS_OK && output == 0x9988);
    MmsUiRefreshSubscriptionStateV1 base{}; base.h = Header<MmsUiRefreshSubscriptionStateV1>();
    CHECK(old.query(old.context, f.subscription, &base) == MMS_OK && base.binding == f.triggerBinding && base.completedRevision == 2);
    CHECK(f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences);
    CHECK(old.unsubscribe(old.context, f.subscription) == MMS_OK); f.Stale();
}
void AcceptedCohortSurvivesLaterPublications() {
    Fixture f; f.StraightGraph(); const auto repaint = f.Request(f.Target(f.trigger));
    for (unsigned n = 0; n < 64; ++n) f.Submit(repaint);
    f.Subscribe(MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE); f.queue.Drain();
    const auto accepted = f.Status(); CHECK(accepted.submittedRevision == 1 && !accepted.completedRevision && accepted.pending);
    CHECK(accepted.submittedWorkGeneration && !f.prepares[0] && !f.prepares[1]);
    f.WorkerPublish(0, 88); f.WorkerPublish(1, 99);
    const auto newer = f.Status(); CHECK(newer.latestRevision == 2 && newer.submittedRevision == 1 && newer.workGeneration > accepted.workGeneration);
    f.queue.Drain();
    CHECK(f.Display(0) == 10 && f.Display(1) == 11 && f.preparedRevisions[0] == 1 && f.preparedRevisions[1] == 1);
    CHECK(f.Status().completedRevision == 1 && f.Status().pending && !f.triggerPrepares);
    CHECK((f.consumed == std::vector<std::pair<unsigned, uint64_t>>({{0, 1}, {1, 1}})));
    f.Complete(); CHECK(f.Display(0) == 88 && f.Display(1) == 99 && f.Status().completedRevision == 2);
    CHECK(f.preparedRevisions[0] == 2 && f.preparedRevisions[1] == 2 && f.prepares[0] == 2 && f.prepares[1] == 2);
}
void EmptyGraphAndTopologyChanges() {
    Fixture f; f.Subscribe(); f.queue.Drain(); const auto empty = f.Status();
    CHECK(empty.pending && (empty.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH) && !f.queue.Pending());
    for (unsigned n = 0; n < 8; ++n) f.queue.Drain(); CHECK(!f.prepares[0] && !f.prepares[1]);
    const auto a = f.Edge(f.group, f.element); f.Complete();
    CHECK(f.prepares[0] == 1 && !f.prepares[1] && f.Status().latestRevision == 1);
    const auto first = f.Status(); f.Edge(f.group, f.second); f.Complete();
    CHECK(f.prepares[0] == 1 && f.prepares[1] == 1 && f.Status().latestRevision == 1 && f.Status().completedWorkGeneration > first.completedWorkGeneration);
    CHECK(f.subscriptions.unsubscribe(f.subscriptions.context, f.subscription) == MMS_OK);
    // Accept the old graph behind the real queue budget, then change it.
    const auto repaint = f.Request(f.Target(f.trigger)); for (unsigned n = 0; n < 64; ++n) f.Submit(repaint);
    f.Subscribe(); f.queue.Drain(); const auto accepted = f.Status(); const auto reads = f.prepares;
    CHECK(accepted.submittedWorkGeneration && accepted.pending);
    f.trace.clear();
    CHECK(f.dependency.unregisterDependency(f.dependency.context, a) == MMS_OK);
    f.Complete(); CHECK(f.prepares == reads && f.Status().completedRevision == 1);
    CHECK(std::count(f.trace.begin(), f.trace.end(), 100u) == 1);
    CHECK(f.Status().completedWorkGeneration > accepted.submittedWorkGeneration);
    CHECK(f.Status().submittedDependencyRevision > accepted.submittedDependencyRevision);
}
void MissingSourceAndConsumerWaits() {
    Fixture f; f.StraightGraph(); CHECK(f.data.unbind(f.data.context, f.bindings[1]) == MMS_OK);
    MmsUiDataSourceDescV1 source{Header<MmsUiDataSourceDescV1>(), S("scoped-cold-source"), 64, 0, 0}; MmsHandle cold = 0;
    CHECK(f.data.registerSource(f.data.context, &source, &cold) == MMS_OK); f.Bind(1, cold); f.Subscribe(); f.queue.Drain();
    CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE) && !f.prepares[0] && !f.prepares[1]);
    const auto wakes = f.wakes.load(); for (unsigned n = 0; n < 8; ++n) f.queue.Drain();
    CHECK(!f.queue.Pending() && f.wakes.load() == wakes);
    source.id = S("scoped-unrelated-source"); MmsHandle unrelated = 0; CHECK(f.data.registerSource(f.data.context, &source, &unrelated) == MMS_OK);
    int value = 61; MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), unrelated, 0, {&value, sizeof(value)}, 0, 0};
    MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
    CHECK(f.data.publishSource(f.data.context, &publish, &state) == MMS_OK); f.queue.Drain(); CHECK(!f.prepares[0] && !f.prepares[1]);
    publish.source = cold; CHECK(f.data.publishSource(f.data.context, &publish, &state) == MMS_OK); f.Complete();
    CHECK(f.Display(0) == 10 && f.Display(1) == 61 && f.Status().completedRevision == 1);
    value = 62; publish.expectedRevision = state.revision;
    CHECK(f.data.publishSource(f.data.context, &publish, &state) == MMS_OK);
    f.WorkerPublish(0, 15); f.prepare = [](unsigned i) { return i == 1 ? MMS_E_FAILED : MMS_OK; }; f.queue.Drain();
    CHECK(f.Status().lastResult == MMS_E_FAILED && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER));
    const auto reads = f.prepares; const auto work = f.Status().workGeneration;
    for (unsigned n = 0; n < 8; ++n) {
        const auto ticket = f.Submit(f.Request(f.Target(f.trigger))); f.queue.Drain(); f.Done(ticket, MMS_UI_REFRESH_REPAINT);
    }
    CHECK(f.prepares == reads && f.Status().workGeneration == work && !f.queue.Pending());
    f.prepare = {}; value = 63; publish.expectedRevision = state.revision;
    CHECK(f.data.publishSource(f.data.context, &publish, &state) == MMS_OK); f.Complete();
    CHECK(f.Display(1) == 63 && f.Status().completedRevision == 2 && f.Status().completedWorkGeneration > work);
    CHECK(f.prepares[0] == reads[0] && f.prepares[1] == reads[1] + 1);
}
void ReadOnlyAndValidation() {
    Fixture f; f.StraightGraph(); const auto desc = f.Desc();
    MmsHandle output = 817; auto bad = desc; bad.kinds = MMS_UI_REFRESH_REPAINT;
    CHECK(f.subscriptions.subscribeScoped(f.subscriptions.context, &bad, &output) == MMS_E_ARGUMENT && output == 817);
    bad = desc; bad.seed.instanceGeneration += 1;
    CHECK(f.subscriptions.subscribeScoped(f.subscriptions.context, &bad, &output) == MMS_E_STALE && output == 817);
    bad = desc; bad.flags = 1;
    CHECK(f.subscriptions.subscribeScoped(f.subscriptions.context, &bad, &output) == MMS_E_ARGUMENT && output == 817);
    unsigned denied = 0; f.prepare = [&](unsigned i) -> MmsResult {
        if (i || denied) return MMS_OK;
        ++denied; MmsHandle unchanged = 817;
        CHECK(f.subscriptions.subscribeScoped(f.subscriptions.context, &desc, &unchanged) == MMS_E_BUSY && unchanged == 817);
        CHECK(f.subscriptions.unsubscribe(f.subscriptions.context, f.subscription) == MMS_E_BUSY);
        CHECK(f.dependency.unregisterDependency(f.dependency.context, f.edges[0]) == MMS_E_BUSY);
        int value = 81; MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), f.sources[0], f.revisions[0], {&value, sizeof(value)}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>(); state.revision = 991;
        CHECK(f.data.publishSource(f.data.context, &publish, &state) == MMS_E_BUSY && state.revision == 991);
        MmsResult result = MMS_OK; std::thread worker([&] { result = f.data.publishSource(f.data.context, &publish, &state); }); worker.join();
        CHECK(result == MMS_OK && state.revision == 2); f.revisions[0] = state.revision;
        const auto reads = f.prepares; f.queue.Drain(); CHECK(f.prepares == reads); return MMS_OK;
    };
    f.Subscribe(); f.queue.Drain();
    CHECK(denied == 1 && f.Status().completedRevision == 1 && f.Status().latestRevision == 2 && f.Status().pending);
    CHECK(f.Display(0) == 10 && f.Display(1) == 11 && f.prepares[0] == 1 && f.prepares[1] == 1);
    f.Complete(); CHECK(denied == 1 && !f.queue.Pending() && f.Status().completedRevision == 2 && f.Display(0) == 81);
    CHECK(f.prepares[0] == 2 && f.prepares[1] == 1);
    Fixture self; auto own = self.Desc(MMS_UI_REFRESH_SELF, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT);
    own.seed = self.Target(self.trigger).target;
    CHECK(self.subscriptions.subscribeScoped(self.subscriptions.context, &own, &self.subscription) == MMS_OK);
    self.Complete(); CHECK(self.triggerPrepares == 1 && !self.prepares[0] && !self.prepares[1]);
    CHECK(self.Status().scope == MMS_UI_REFRESH_SELF && self.Status().seed.element == self.trigger);
}
void CapacityReadinessAndHostWake() {
    {
        Fixture f; f.StraightGraph(); MmsUiRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshCapabilitiesV1>();
        CHECK(f.api.getCapabilities(f.api.context, &caps) == MMS_OK);
        const auto repaint = f.Request(f.Target(f.trigger));
        for (unsigned n = 0; n < caps.maxTicketsPerPlugin; ++n) f.Submit(repaint);
        for (unsigned n = 0; n < caps.maxTicketsPerPlugin; n += caps.maxContributionsPerDrain) f.queue.Drain();
        CHECK(!f.queue.Pending()); f.Subscribe(); f.queue.Drain();
        CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY));
        const auto wakes = f.wakes.load();
        for (unsigned n = 0; n < 12; ++n) f.queue.Drain();
        CHECK(!f.prepares[0] && !f.prepares[1] && !f.queue.Pending() && f.wakes.load() == wakes);
        CHECK(f.api.release(f.api.context, f.tickets.back()) == MMS_OK); f.tickets.pop_back(); f.Complete();
        CHECK(f.Display(0) == 10 && f.Display(1) == 11);
    }
    {
        Fixture f; f.StraightGraph(); f.Subscribe(); f.Complete();
        f.queue.SuspendTargets(); f.WorkerPublish(0, 77); f.queue.Drain();
        CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET));
        const auto reads = f.prepares; for (unsigned n = 0; n < 8; ++n) f.queue.Drain();
        CHECK(f.prepares == reads && !f.queue.Pending());
        f.queue.SyncTargets(f.presentations); f.Complete(); CHECK(f.Display(0) == 77);
        CHECK(f.data.unbind(f.data.context, f.bindings[0]) == MMS_OK);
        CHECK(f.data.unbind(f.data.context, f.bindings[1]) == MMS_OK); f.queue.Drain();
        CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_BINDING));
        const auto unboundReads = f.prepares;
        for (unsigned n = 0; n < 8; ++n) f.queue.Drain();
        CHECK(f.prepares == unboundReads && !f.queue.Pending());
        f.Bind(0); f.Bind(1); f.Complete(); CHECK(f.Display(0) == 77 && f.Display(1) == 11);
    }
    {
        Fixture f; f.StraightGraph(); f.queue.SetWake([&] { f.wakes.fetch_add(1); return false; });
        f.Subscribe(); CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE));
        const auto wakes = f.wakes.load(); for (unsigned n = 0; n < 8; ++n) f.queue.Drain();
        CHECK(!f.prepares[0] && !f.prepares[1] && !f.queue.Pending() && f.wakes.load() == wakes);
        f.queue.SetWake([&] { f.wakes.fetch_add(1); return true; }); f.WorkerPublish(0, 79); f.Complete();
        CHECK(f.Display(0) == 79 && f.Display(1) == 11);
    }
}
void FailedTopologyPublicationRecoversWithoutAnotherLayout() {
    Fixture f; f.StraightGraph(); f.Subscribe(); f.Complete();
    // Fail a real string allocation while copying the retained registration
    // tree. No test-only production hook or replacement graph is involved.
    size_t allocation = 0;
    for (const auto& entry : f.registry.elements) {
        if (entry.second.desc.parentId.bytes <= 15) continue;
        const auto parent = String(entry.second.desc.parentId);
        allocation = parent.capacity() + 1; break;
    }
    CHECK(allocation && !overflow_allocation_fault::bytes);
    const auto hits = overflow_allocation_fault::hits;
    overflow_allocation_fault::bytes = allocation; f.queue.SyncTopology();
    overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == hits + 1);
    f.WorkerPublish(0, 76); f.queue.Drain();
    CHECK(f.Status().pending && (f.Status().waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET));
    const auto reads = f.prepares; CHECK(!f.queue.Pending());
    const auto wakes = f.wakes.load();
    f.queue.SyncTopology();
    // A successful topology publication must wake the parked subscription
    // itself, without SyncTargets or an unrelated user/worker event.
    CHECK(f.queue.Pending() && f.wakes.load() > wakes && f.prepares == reads);
    f.Complete(); CHECK(f.Display(0) == 76 && f.Display(1) == 11);
}
void SubtreeAndLifecycle() {
    { Fixture f; f.Subscribe(MMS_UI_REFRESH_SUBTREE); f.Complete();
      CHECK(f.Display(0) == 10 && f.Display(1) == 11 && f.layouts == 1 && !f.triggerPrepares && f.Status().scope == MMS_UI_REFRESH_SUBTREE); }
    for (unsigned mode = 0; mode < 5; ++mode) {
        Fixture f; f.StraightGraph(); const auto repaint = f.Request(f.Target(f.trigger));
        for (unsigned n = 0; n < 64; ++n) f.Submit(repaint); f.Subscribe(); f.queue.Drain();
        CHECK(f.Status().submittedWorkGeneration && !f.prepares[0] && !f.prepares[1]);
        if (mode == 0) CHECK(f.subscriptions.unsubscribe(f.subscriptions.context, f.subscription) == MMS_OK);
        else if (mode == 1) CHECK(f.data.unbind(f.data.context, f.triggerBinding) == MMS_OK);
        else if (mode == 2) f.queue.InvalidateTarget(f.group);
        else if (mode == 3) f.queue.AdvanceProjectEpoch();
        else f.queue.Retire(f.client);
        f.Stale(); f.queue.Drain(); CHECK(!f.prepares[0] && !f.prepares[1] && !f.queue.Pending());
    }
}
}
static void ScopedSubscriptionChecks() {
    scoped_subscription_checks::OrderedCompositeAndThreads();
    scoped_subscription_checks::AcceptedCohortSurvivesLaterPublications();
    scoped_subscription_checks::EmptyGraphAndTopologyChanges();
    scoped_subscription_checks::MissingSourceAndConsumerWaits();
    scoped_subscription_checks::ReadOnlyAndValidation();
    scoped_subscription_checks::CapacityReadinessAndHostWake();
    scoped_subscription_checks::FailedTopologyPublicationRecoversWithoutAnotherLayout();
    scoped_subscription_checks::SubtreeAndLifecycle();
}
