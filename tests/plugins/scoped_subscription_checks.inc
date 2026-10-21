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
void QueueCapacityReleaseBeforeDeferredGraphObservation() {
    Fixture f;
    // Complete this public SELF/REPAINT setup ticket before a subscription
    // exists. It stays terminal and retained through the failing Drain, so
    // only the later explicit release can publish its queue-capacity event.
    const auto terminal = f.Submit(f.Request(f.Target(f.trigger))); f.queue.Drain();
    const auto terminalState = f.State(terminal);
    CHECK(terminalState.status == MMS_UI_REFRESH_COMPLETED && terminalState.result == MMS_OK &&
        terminalState.completedKinds == MMS_UI_REFRESH_REPAINT && !f.queue.Pending());
    CHECK(f.tickets.size() == 1 && f.tickets.back() == terminal);
    const auto sourceRevisions = f.revisions;
    const auto warm = f.Edge(f.group, f.element);
    CHECK(f.dependency.unregisterDependency(f.dependency.context, warm) == MMS_OK);
    f.Subscribe(); f.queue.Drain(); const auto empty = f.Status();
    CHECK(empty.pending && empty.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH && !f.queue.Pending());
    CHECK(!overflow_allocation_fault::bytes && !overflow_allocation_fault::matchingFailures);
    const auto mutationHits = overflow_allocation_fault::hits;
    struct EdgeValue { const RefreshClient* owner; MmsUiTargetRefV1 upstream, dependent; };
    using EdgePair = std::pair<const MmsHandle, EdgeValue>;
    CHECK(sizeof(std::_Tree_node<EdgePair, void*>) != 2 * sizeof(MmsUiTargetRefV1));
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    const auto edge = f.Edge(f.group, f.element); overflow_allocation_fault::bytes = 0;
    CHECK(edge && overflow_allocation_fault::hits == mutationHits + 1 && f.queue.Pending());
    const auto requestHits = overflow_allocation_fault::hits;
    overflow_allocation_fault::matchingFailures = 2;
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1); f.queue.Drain();
    const auto remaining = overflow_allocation_fault::matchingFailures;
    overflow_allocation_fault::bytes = 0; overflow_allocation_fault::matchingFailures = 0;
    CHECK(overflow_allocation_fault::hits == requestHits + 2 && !remaining);
    const auto failed = f.Status(); const auto wakes = f.wakes.load();
    CHECK(failed.lastResult == MMS_E_MEMORY && failed.pending && failed.latestRevision == empty.latestRevision &&
        !failed.submittedRevision && !failed.completedRevision && !failed.submittedWorkGeneration &&
        !failed.completedWorkGeneration && (failed.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY) && !f.queue.Pending());
    CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts && !f.triggerPrepares);
    CHECK(f.State(terminal).status == MMS_UI_REFRESH_COMPLETED);
    // No observer, trigger publish or graph mutation intervenes. Erasing the
    // original terminal ticket advances the real public queue-capacity clock.
    CHECK(f.api.release(f.api.context, terminal) == MMS_OK); f.tickets.pop_back();
    const auto released = f.Status();
    CHECK(released.latestRevision == failed.latestRevision && released.workGeneration == failed.workGeneration &&
        released.submittedRevision == failed.submittedRevision && released.completedRevision == failed.completedRevision &&
        released.pending && f.revisions == sourceRevisions);
    std::cerr << "SCOPED early-capacity generation=" << released.workGeneration << " wait=" << released.waitingFor
        << " pending=" << f.queue.Pending() << " wakes=" << f.wakes.load() - wakes << '\n';
    CHECK(f.queue.Pending() && f.wakes.load() > wakes);
    f.Complete(); const auto complete = f.Status();
    CHECK(complete.latestRevision == failed.latestRevision && complete.submittedRevision == failed.latestRevision &&
        complete.completedRevision == failed.latestRevision && complete.workGeneration == failed.workGeneration &&
        complete.submittedWorkGeneration == failed.workGeneration && complete.completedWorkGeneration == failed.workGeneration &&
        !complete.pending && !complete.waitingFor && !complete.lastResult && f.revisions == sourceRevisions);
    CHECK(f.prepares[0] == 1 && !f.prepares[1] && f.preparedRevisions[0] == failed.latestRevision &&
        f.Display(0) == 10 && f.stateReads == 1 && f.layouts == 1 && !f.triggerPrepares);
    CHECK(f.trace == std::vector<unsigned>({10, 100, 200}));
    const auto settledWakes = f.wakes.load();
    for (unsigned n = 0; n < 6; ++n) {
        f.queue.Drain(); const auto settled = f.Status();
        CHECK(!std::memcmp(&settled, &complete, sizeof(settled)) && !f.queue.Pending() && f.wakes.load() == settledWakes);
        CHECK(f.prepares[0] == 1 && !f.prepares[1] && f.stateReads == 1 && f.layouts == 1 &&
            !f.triggerPrepares && f.revisions == sourceRevisions && f.trace == std::vector<unsigned>({10, 100, 200}));
    }
}
void TriggerPublicationBeforeDeferredGraphObservation() {
    Fixture f; const auto warm = f.Edge(f.group, f.element);
    CHECK(f.dependency.unregisterDependency(f.dependency.context, warm) == MMS_OK);
    f.Subscribe(); f.queue.Drain(); const auto empty = f.Status();
    CHECK(empty.pending && empty.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH && !f.queue.Pending());
    CHECK(!overflow_allocation_fault::bytes && !overflow_allocation_fault::matchingFailures);
    const auto mutationHits = overflow_allocation_fault::hits;
    // The real S -> A edge commits; only its observer's second vertex fails.
    struct EdgeValue { const RefreshClient* owner; MmsUiTargetRefV1 upstream, dependent; };
    using EdgePair = std::pair<const MmsHandle, EdgeValue>;
    CHECK(sizeof(std::_Tree_node<EdgePair, void*>) != 2 * sizeof(MmsUiTargetRefV1));
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    const auto edge = f.Edge(f.group, f.element); overflow_allocation_fault::bytes = 0;
    CHECK(edge && overflow_allocation_fault::hits == mutationHits + 1 && f.queue.Pending());
    const auto requestHits = overflow_allocation_fault::hits;
    // Keep the new graph unobserved through the scheduling observer and the
    // one original Request. This must fail that Request, not a consumer.
    overflow_allocation_fault::matchingFailures = 2;
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1); f.queue.Drain();
    const auto remaining = overflow_allocation_fault::matchingFailures;
    overflow_allocation_fault::bytes = 0; overflow_allocation_fault::matchingFailures = 0;
    CHECK(overflow_allocation_fault::hits == requestHits + 2 && !remaining);
    const auto failed = f.Status(); const auto wakes = f.wakes.load();
    CHECK(failed.lastResult == MMS_E_MEMORY && failed.pending && failed.latestRevision == empty.latestRevision &&
        !failed.submittedRevision && !failed.completedRevision && !failed.submittedWorkGeneration &&
        !failed.completedWorkGeneration && !f.queue.Pending());
    CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts && !f.triggerPrepares);
    // No successful Drain/Sync/graph mutation may repair the old metadata
    // before this one genuine worker publication advances trigger revision.
    f.WorkerPublish(0, 94); const auto notified = f.Status();
    CHECK(notified.latestRevision == failed.latestRevision + 1 && notified.latestRevision == f.revisions[0] &&
        notified.workGeneration == failed.workGeneration + 1 && notified.pending);
    std::cerr << "SCOPED early-publication generation=" << notified.workGeneration << " wait=" << notified.waitingFor
        << " pending=" << f.queue.Pending() << " wakes=" << f.wakes.load() - wakes << '\n';
    CHECK(f.queue.Pending() && f.wakes.load() > wakes);
    f.Complete(); const auto complete = f.Status();
    CHECK(complete.latestRevision == notified.latestRevision && complete.submittedRevision == notified.latestRevision &&
        complete.completedRevision == notified.latestRevision && complete.workGeneration == notified.workGeneration &&
        complete.submittedWorkGeneration == notified.workGeneration && complete.completedWorkGeneration == notified.workGeneration &&
        !complete.pending && !complete.waitingFor && !complete.lastResult);
    CHECK(f.prepares[0] == 1 && !f.prepares[1] && f.preparedRevisions[0] == notified.latestRevision &&
        f.Display(0) == 94 && f.stateReads == 1 && f.layouts == 1 && !f.triggerPrepares);
    MmsUiDataDisplayV1 untouched{}; untouched.h = Header<MmsUiDataDisplayV1>(); untouched.lease = 991;
    const auto sentinel = untouched;
    CHECK(f.data.acquireDisplay(f.data.context, f.bindings[1], &untouched) == MMS_E_NOT_READY &&
        !std::memcmp(&untouched, &sentinel, sizeof(untouched)));
    CHECK(f.trace == std::vector<unsigned>({10, 100, 200}));
    const auto settledWakes = f.wakes.load();
    for (unsigned n = 0; n < 6; ++n) {
        f.queue.Drain(); const auto settled = f.Status();
        CHECK(!std::memcmp(&settled, &complete, sizeof(settled)) && !f.queue.Pending() && f.wakes.load() == settledWakes);
        CHECK(f.prepares[0] == 1 && !f.prepares[1] && f.stateReads == 1 && f.layouts == 1 &&
            !f.triggerPrepares && f.trace == std::vector<unsigned>({10, 100, 200}));
    }
}
void NewlyReachableGraphEventRecoversClaimedFailure() {
    Fixture f; const auto warm = f.Edge(f.group, f.element);
    CHECK(f.dependency.unregisterDependency(f.dependency.context, warm) == MMS_OK);
    f.Subscribe(); f.queue.Drain(); const auto empty = f.Status();
    CHECK(empty.pending && empty.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH && !f.queue.Pending());
    CHECK(!overflow_allocation_fault::bytes && !overflow_allocation_fault::matchingFailures);
    const auto revisions = f.revisions;
    const auto mutationHits = overflow_allocation_fault::hits;
    // S -> A commits, but its first observation cannot retain A as a member.
    struct EdgeValue { const RefreshClient* owner; MmsUiTargetRefV1 upstream, dependent; };
    using EdgePair = std::pair<const MmsHandle, EdgeValue>;
    CHECK(sizeof(std::_Tree_node<EdgePair, void*>) != 2 * sizeof(MmsUiTargetRefV1));
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    const auto first = f.Edge(f.group, f.element); overflow_allocation_fault::bytes = 0;
    CHECK(first && overflow_allocation_fault::hits == mutationHits + 1 && f.queue.Pending());
    const auto requestHits = overflow_allocation_fault::hits;
    // Both the next observation and the original Request capture fail. No
    // complete observation may run between this claimed failure and A -> B.
    overflow_allocation_fault::matchingFailures = 2;
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1);
    f.queue.Drain();
    const auto remaining = overflow_allocation_fault::matchingFailures;
    overflow_allocation_fault::bytes = 0; overflow_allocation_fault::matchingFailures = 0;
    CHECK(overflow_allocation_fault::hits == requestHits + 2 && !remaining);
    const auto failed = f.Status(); const auto wakes = f.wakes.load();
    CHECK(failed.lastResult == MMS_E_MEMORY && failed.pending && failed.latestRevision == empty.latestRevision &&
        !failed.submittedRevision && !failed.completedRevision && !failed.submittedWorkGeneration &&
        !failed.completedWorkGeneration && !f.queue.Pending());
    CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts && !f.triggerPrepares);
    // A is reachable in the actual graph, although the retained member list is
    // still empty. Only this second real graph event may recover the request.
    const auto second = f.Edge(f.element, f.second);
    CHECK(second && f.queue.Pending() && f.wakes.load() > wakes);
    CHECK(f.Status().workGeneration > failed.workGeneration && f.revisions == revisions);
    f.Complete(); const auto complete = f.Status();
    CHECK(complete.latestRevision == empty.latestRevision && complete.completedRevision == empty.latestRevision &&
        complete.completedWorkGeneration > failed.workGeneration && !complete.pending && !complete.waitingFor);
    CHECK(f.prepares[0] == 1 && f.prepares[1] == 1 && f.Display(0) == 10 && f.Display(1) == 11);
    CHECK(f.preparedRevisions == revisions && f.revisions == revisions && !f.triggerPrepares && !f.queue.Pending());
    CHECK(f.trace == std::vector<unsigned>({10, 11, 100, 100, 200}));
}
void RealGraphEventSurvivesObservationAllocationFailure() {
    Fixture f;
    // Keep a real empty graph with its owner node already allocated. The
    // subsequent fault must hit graph observation after Add has committed,
    // not Add's cycle-check vector or a new owner-map node.
    const auto warm = f.Edge(f.group, f.element);
    CHECK(f.dependency.unregisterDependency(f.dependency.context, warm) == MMS_OK);
    f.Subscribe(); f.queue.Drain(); const auto empty = f.Status();
    CHECK(empty.pending && empty.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH && !f.queue.Pending());
    CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts);
    const auto reads = f.prepares; const auto wakes = f.wakes.load();
    CHECK(!overflow_allocation_fault::bytes); const auto hits = overflow_allocation_fault::hits;
    // Add checks reachable{dependent} with one 32-byte ref and no outgoing
    // edge. Capture(seed) then grows vertices to two refs (64 bytes). Only
    // the latter failure occurs after the real edge is successfully added.
    struct EdgeValue { const RefreshClient* owner; MmsUiTargetRefV1 upstream, dependent; };
    using EdgePair = std::pair<const MmsHandle, EdgeValue>;
    CHECK(sizeof(std::_Tree_node<EdgePair, void*>) != 2 * sizeof(MmsUiTargetRefV1));
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    const auto edge = f.Edge(f.group, f.second); overflow_allocation_fault::bytes = 0;
    CHECK(edge && overflow_allocation_fault::hits == hits + 1);
    CHECK(f.Target(f.group).supportedScopes & MMS_UI_REFRESH_DEPENDENTS);
    CHECK(f.prepares == reads && f.queue.Pending() && f.wakes.load() > wakes);
    // Only the successful edge registration supplies the event. Complete
    // drains the original queue; no source publication or layout is supplied.
    f.Complete(); const auto complete = f.Status();
    CHECK(complete.latestRevision == empty.latestRevision && complete.completedRevision == empty.latestRevision &&
        complete.completedWorkGeneration > empty.completedWorkGeneration && !complete.pending && !complete.waitingFor);
    CHECK(!f.prepares[0] && f.prepares[1] == 1 && f.Display(1) == 11 && !f.triggerPrepares);
    CHECK(f.trace == std::vector<unsigned>({11, 100, 200}) && !f.queue.Pending());
}
void RealSubtreePublicationSurvivesObservationAllocationFailure() {
    Fixture f;
    // The fixture supplies the host's actual retained registration tree to
    // SyncTopology. Change only a registered child's parent; preserve its
    // original HWND, binding and source identity throughout the publication.
    auto& child = f.registry.elements.at("example.refresh.sibling");
    child.desc.parentId = S("mms.toolbar.main.extensions"); f.queue.SyncTopology();
    f.Subscribe(MMS_UI_REFRESH_SUBTREE); f.Complete(); const auto before = f.Status();
    CHECK(f.prepares[0] == 1 && !f.prepares[1] && !f.queue.Pending());
    const auto reads = f.prepares; const auto wakes = f.wakes.load();
    child.desc.parentId = S("example.refresh.tree.nested");
    CHECK(!overflow_allocation_fault::bytes); const auto hits = overflow_allocation_fault::hits;
    // Topology's map/string copy succeeds; CollectScopedTargetsLocked's
    // SubtreeLocked allocates its first real handle buffer at exactly 8 bytes.
    overflow_allocation_fault::bytes = sizeof(MmsHandle);
    f.queue.SyncTopology(); overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == hits + 1);
    CHECK(f.prepares == reads && f.queue.Pending() && f.wakes.load() > wakes);
    f.Complete(); const auto complete = f.Status();
    CHECK(complete.latestRevision == before.latestRevision && complete.completedRevision == before.latestRevision &&
        complete.completedWorkGeneration > before.completedWorkGeneration && !complete.pending && !complete.waitingFor);
    CHECK(f.Display(0) == 10 && f.Display(1) == 11 && f.prepares[0] == reads[0] && f.prepares[1] == 1);
    CHECK(f.stateReads == 3 && f.layouts == 2 && !f.triggerPrepares && !f.queue.Pending());
}
void ClaimedGraphEventDoesNotReplayAfterRequestAllocationFailure() {
    Fixture f; const auto warm = f.Edge(f.group, f.element);
    CHECK(f.dependency.unregisterDependency(f.dependency.context, warm) == MMS_OK);
    f.Subscribe(); f.queue.Drain();
    CHECK(f.Status().waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH && !f.queue.Pending());
    CHECK(!overflow_allocation_fault::bytes && !overflow_allocation_fault::matchingFailures);
    const auto mutationHits = overflow_allocation_fault::hits;
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    f.Edge(f.group, f.second); overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == mutationHits + 1 && f.queue.Pending());
    const auto requestHits = overflow_allocation_fault::hits;
    // One Drain first observes the real scope, then captures the original
    // Request. Fail both initial graph-ref buffers so Request truly returns
    // MEMORY, rather than merely postponing a consumer callback.
    overflow_allocation_fault::matchingFailures = 2;
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1);
    f.queue.Drain();
    const auto remaining = overflow_allocation_fault::matchingFailures;
    overflow_allocation_fault::bytes = 0; overflow_allocation_fault::matchingFailures = 0;
    CHECK(overflow_allocation_fault::hits == requestHits + 2 && !remaining);
    const auto failed = f.Status(); const auto wakes = f.wakes.load();
    CHECK(failed.lastResult == MMS_E_MEMORY && failed.pending && !failed.submittedRevision && !failed.completedRevision &&
        !failed.submittedWorkGeneration && !failed.completedWorkGeneration && !f.queue.Pending());
    CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts && !f.triggerPrepares);
    for (unsigned n = 0; n < 6; ++n) {
        // The first successful observation installs missing members/watches;
        // it may not replay this already-claimed real structure event.
        f.queue.Drain(); const auto state = f.Status();
        CHECK(!std::memcmp(&state, &failed, sizeof(state)) && !f.queue.Pending() && f.wakes.load() == wakes);
        CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts && !f.triggerPrepares);
    }
    f.WorkerPublish(0, 93); CHECK(f.queue.Pending()); f.Complete();
    CHECK(f.Status().completedRevision == 2 && f.Display(1) == 11 && !f.prepares[0] && f.prepares[1] == 1);
}
void RemovedCachedMemberCannotPublishUnrelatedGraphEvent() {
    Fixture f; f.StraightGraph(); f.Subscribe(); f.Complete(); const auto initial = f.Status();
    CHECK(initial.completedRevision == 1 && initial.submittedRevision == 1 && !f.queue.Pending());
    // Prepare a new contextual B version before the removal. It is not a
    // trigger and cannot itself schedule a successfully completed scope.
    f.WorkerPublish(1, 72); CHECK(!f.queue.Pending());
    const auto reads = f.prepares; const auto sourceReads = f.preparedRevisions;
    const auto stateReads = f.stateReads, layouts = f.layouts; const auto trace = f.trace;
    CHECK(!overflow_allocation_fault::bytes && !overflow_allocation_fault::matchingFailures);
    const auto removeHits = overflow_allocation_fault::hits;
    // Remove commits without allocating. Its resulting S->B capture grows
    // vertices from S to S+B at 64 bytes, so this fault occurs after deletion.
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    const auto removed = f.dependency.unregisterDependency(f.dependency.context, f.edges[0]);
    overflow_allocation_fault::bytes = 0;
    CHECK(removed == MMS_OK && overflow_allocation_fault::hits == removeHits + 1 && f.queue.Pending());
    const auto captureHits = overflow_allocation_fault::hits;
    // Keep the original cached {A,B} unobserved: both the scheduling observer
    // and the real Request's graph capture must fail, not a prepare callback.
    overflow_allocation_fault::matchingFailures = 2;
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1); f.queue.Drain();
    const auto remaining = overflow_allocation_fault::matchingFailures;
    overflow_allocation_fault::bytes = 0; overflow_allocation_fault::matchingFailures = 0;
    CHECK(overflow_allocation_fault::hits == captureHits + 2 && !remaining);
    const auto failed = f.Status(); const auto wakes = f.wakes.load();
    CHECK(failed.lastResult == MMS_E_MEMORY && failed.pending && !f.queue.Pending());
    CHECK(failed.latestRevision == initial.latestRevision && failed.submittedRevision == initial.submittedRevision &&
        failed.completedRevision == initial.completedRevision && failed.workGeneration > initial.workGeneration &&
        failed.submittedWorkGeneration == initial.submittedWorkGeneration && failed.completedWorkGeneration == initial.completedWorkGeneration);
    const auto unchanged = [&] {
        const auto state = f.Status();
        CHECK(state.workGeneration == failed.workGeneration && state.submittedWorkGeneration == failed.submittedWorkGeneration &&
            state.completedWorkGeneration == failed.completedWorkGeneration);
        CHECK(state.latestRevision == failed.latestRevision && state.submittedRevision == failed.submittedRevision &&
            state.completedRevision == failed.completedRevision && state.submittedDependencyRevision == failed.submittedDependencyRevision);
        CHECK(state.waitingFor == failed.waitingFor && state.lastResult == failed.lastResult && state.pending == failed.pending);
        CHECK(f.prepares == reads && f.preparedRevisions == sourceReads && f.stateReads == stateReads && f.layouts == layouts && f.trace == trace);
        CHECK(!f.queue.Pending() && f.wakes.load() == wakes && !f.triggerPrepares);
    };
    unchanged();
    // A is still present in the old cached observation, but the real graph
    // now contains only S->B. A->external-trigger is a successful unrelated
    // mutation; cached-members OR current reachability would wrongly wake it.
    const auto edge = f.Edge(f.element, f.trigger); CHECK(edge); unchanged();
    for (unsigned n = 0; n < 6; ++n) { f.queue.Drain(); unchanged(); }
    // Only an actual trigger publication recovers the claimed memory park.
    // The original Request now processes B, never the detached cached A.
    f.WorkerPublish(0, 73); CHECK(f.queue.Pending()); f.Complete();
    CHECK(f.Status().completedRevision == 2 && f.Display(0) == 10 && f.Display(1) == 72);
    CHECK(f.prepares[0] == reads[0] && f.prepares[1] == reads[1] + 1 && f.stateReads == stateReads + 1 && f.layouts == layouts + 1);
    CHECK(f.trace.size() == trace.size() + 3 && f.trace[trace.size()] == 11 && f.trace[trace.size() + 1] == 100 &&
        f.trace[trace.size() + 2] == 200 && !f.triggerPrepares && !f.queue.Pending());
}
void FailedGraphEventWakeSurvivesSuccessfulObservation() {
    Fixture f; const auto warm = f.Edge(f.group, f.element);
    CHECK(f.dependency.unregisterDependency(f.dependency.context, warm) == MMS_OK);
    f.Subscribe(); f.queue.Drain(); CHECK(!f.queue.Pending());
    f.queue.SetWake([&] { f.wakes.fetch_add(1); return false; });
    const auto hits = overflow_allocation_fault::hits;
    overflow_allocation_fault::bytes = 2 * sizeof(MmsUiTargetRefV1);
    f.Edge(f.group, f.second); overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == hits + 1);
    const auto failed = f.Status(); const auto wakes = f.wakes.load();
    CHECK(failed.lastResult == MMS_E_FAILED && (failed.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE) && !f.queue.Pending());
    f.queue.Drain(); const auto observed = f.Status();
    CHECK(observed.waitingFor == failed.waitingFor && observed.lastResult == failed.lastResult && observed.pending == failed.pending);
    CHECK(observed.latestRevision == failed.latestRevision && !observed.submittedRevision && !observed.completedRevision);
    CHECK(!f.queue.Pending() && f.wakes.load() == wakes && !f.prepares[0] && !f.prepares[1]);
    for (unsigned n = 0; n < 6; ++n) {
        f.queue.Drain(); const auto state = f.Status();
        CHECK(!std::memcmp(&state, &observed, sizeof(state)) && !f.queue.Pending() && f.wakes.load() == wakes);
        CHECK(!f.prepares[0] && !f.prepares[1] && !f.stateReads && !f.layouts && !f.triggerPrepares);
    }
    f.queue.SetWake([&] { f.wakes.fetch_add(1); return true; });
    f.WorkerPublish(0, 94); CHECK(f.queue.Pending()); f.Complete();
    CHECK(f.Status().completedRevision == 2 && f.Display(1) == 11 && !f.prepares[0] && f.prepares[1] == 1);
}
void UnrelatedStructurePreservesParkedConsumer() {
    for (uint32_t scope : {MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_SUBTREE}) {
        Fixture f; f.StraightGraph(); f.Subscribe(scope); f.Complete();
        f.WorkerPublish(1, 92); f.WorkerPublish(0, 91);
        f.prepare = [](unsigned i) { return i == 1 ? MMS_E_FAILED : MMS_OK; }; f.queue.Drain();
        const auto before = f.Status(); const auto reads = f.prepares; const auto sourceReads = f.preparedRevisions;
        const auto wakes = f.wakes.load(); const auto stateReads = f.stateReads, layouts = f.layouts;
        CHECK(before.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER && before.lastResult == MMS_E_FAILED && !f.queue.Pending());
        const auto unchanged = [&] {
            const auto state = f.Status(); CHECK(!std::memcmp(&state, &before, sizeof(state)));
            CHECK(f.prepares == reads && f.preparedRevisions == sourceReads && f.stateReads == stateReads && f.layouts == layouts);
            CHECK(!f.queue.Pending() && f.wakes.load() == wakes && !f.triggerPrepares);
        };
        // This graph's anchor is the external trigger, never reachable from
        // the registered scope seed. Its DATA binding does not make it a member.
        const auto edge = f.Edge(f.trigger, f.panelElement); unchanged();
        CHECK(f.dependency.unregisterDependency(f.dependency.context, edge) == MMS_OK); unchanged();
        CHECK(f.registry.elements.count("mms.sidebar.left.extensions"));
        auto& external = f.registry.elements.at("example.refresh.scoped-trigger");
        external.desc.parentId = S("mms.sidebar.left.extensions"); f.queue.SyncTopology(); unchanged();
        external.desc.parentId = S("mms.toolbar.main.extensions"); f.queue.SyncTopology(); unchanged();
        for (unsigned n = 0; n < 4; ++n) { f.queue.Drain(); unchanged(); }
        f.prepare = {}; f.WorkerPublish(0, 95); f.Complete();
        CHECK(f.Display(0) == 95 && f.Display(1) == 92 && f.Status().completedRevision == 3 && !f.queue.Pending());
    }
}
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
void ObservationAllocationFailurePreservesParkedConsumer() {
    Fixture f; f.StraightGraph(); f.Subscribe(); f.Complete();
    f.WorkerPublish(1, 82); f.WorkerPublish(0, 81);
    f.prepare = [](unsigned i) { return i == 1 ? MMS_E_FAILED : MMS_OK; };
    f.queue.Drain();
    const auto before = f.Status();
    CHECK(before.lastResult == MMS_E_FAILED && before.pending &&
        before.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER && !f.queue.Pending());
    CHECK(before.latestRevision == 2 && before.submittedRevision == 2 && before.completedRevision == 1);
    CHECK(f.Display(0) == 81 && f.Display(1) == 11);
    const auto reads = f.prepares; const auto sourceReads = f.preparedRevisions;
    const auto trace = f.trace; const auto consumed = f.consumed;
    const auto stateReads = f.stateReads, layouts = f.layouts, triggerReads = f.triggerPrepares;
    const auto wakes = f.wakes.load(); const auto registryRevision = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    std::array<MmsUiDataSourceStateV1, 2> sources{};
    for (size_t i = 0; i < sources.size(); ++i) {
        sources[i].h = Header<MmsUiDataSourceStateV1>();
        CHECK(f.data.getSource(f.data.context, f.sources[i], &sources[i]) == MMS_OK);
    }
    const auto unchanged = [&] {
        const auto state = f.Status();
        CHECK(state.waitingFor == before.waitingFor && state.lastResult == before.lastResult && state.pending == before.pending);
        CHECK(state.workGeneration == before.workGeneration && state.submittedWorkGeneration == before.submittedWorkGeneration &&
            state.completedWorkGeneration == before.completedWorkGeneration);
        CHECK(state.latestRevision == before.latestRevision && state.submittedRevision == before.submittedRevision &&
            state.completedRevision == before.completedRevision && state.submittedDependencyRevision == before.submittedDependencyRevision);
        CHECK(!std::memcmp(&state, &before, sizeof(state)));
        CHECK(f.prepares == reads && f.preparedRevisions == sourceReads && f.triggerPrepares == triggerReads);
        CHECK(f.stateReads == stateReads && f.layouts == layouts && f.trace == trace && f.consumed == consumed);
        CHECK(!f.queue.Pending() && f.wakes.load() == wakes);
        CHECK(f.registry.Revision() == registryRevision && f.registry.PreferencesRevision() == preferences);
        for (size_t i = 0; i < sources.size(); ++i) {
            MmsUiDataSourceStateV1 actual{}; actual.h = Header<MmsUiDataSourceStateV1>();
            CHECK(f.data.getSource(f.data.context, f.sources[i], &actual) == MMS_OK);
            CHECK(!std::memcmp(&actual, &sources[i], sizeof(actual)));
        }
        CHECK(f.Display(0) == 81 && f.Display(1) == 11);
    };
    unchanged();
    CHECK(!overflow_allocation_fault::bytes); const auto hits = overflow_allocation_fault::hits;
    // CollectScopedTargetsLocked enters the real dependency capture first.
    // Its initial vertices{seed} allocation is exactly one TargetRef (32
    // bytes), before any ticket, DATA prepare callback or display allocation.
    // The injector consumes this one failure, leaving later work unaffected.
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1);
    f.queue.Drain(); overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == hits + 1);
    unchanged();
    for (unsigned n = 0; n < 6; ++n) { f.queue.Drain(); unchanged(); }
    // A real contextual-source publication, without changing the trigger
    // revision, must still wake and finish the same pending source version.
    f.prepare = {}; f.WorkerPublish(1, 83);
    CHECK(f.prepares == reads && f.queue.Pending() && f.wakes.load() > wakes);
    CHECK(f.Status().latestRevision == before.latestRevision && f.Status().workGeneration > before.workGeneration);
    f.Complete();
    CHECK(f.Display(0) == 81 && f.Display(1) == 83 && f.Status().completedRevision == before.latestRevision);
    CHECK(f.Status().completedWorkGeneration > before.completedWorkGeneration && !f.Status().pending && !f.queue.Pending());
    CHECK(f.prepares[0] == reads[0] && f.prepares[1] == reads[1] + 1 && f.triggerPrepares == triggerReads);
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
    scoped_subscription_checks::QueueCapacityReleaseBeforeDeferredGraphObservation();
    scoped_subscription_checks::TriggerPublicationBeforeDeferredGraphObservation();
    scoped_subscription_checks::NewlyReachableGraphEventRecoversClaimedFailure();
    scoped_subscription_checks::RealGraphEventSurvivesObservationAllocationFailure();
    scoped_subscription_checks::RealSubtreePublicationSurvivesObservationAllocationFailure();
    scoped_subscription_checks::ClaimedGraphEventDoesNotReplayAfterRequestAllocationFailure();
    scoped_subscription_checks::RemovedCachedMemberCannotPublishUnrelatedGraphEvent();
    scoped_subscription_checks::FailedGraphEventWakeSurvivesSuccessfulObservation();
    scoped_subscription_checks::UnrelatedStructurePreservesParkedConsumer();
    scoped_subscription_checks::OrderedCompositeAndThreads();
    scoped_subscription_checks::AcceptedCohortSurvivesLaterPublications();
    scoped_subscription_checks::EmptyGraphAndTopologyChanges();
    scoped_subscription_checks::MissingSourceAndConsumerWaits();
    scoped_subscription_checks::ObservationAllocationFailurePreservesParkedConsumer();
    scoped_subscription_checks::ReadOnlyAndValidation();
    scoped_subscription_checks::CapacityReadinessAndHostWake();
    scoped_subscription_checks::FailedTopologyPublicationRecoversWithoutAnotherLayout();
    scoped_subscription_checks::SubtreeAndLifecycle();
}
