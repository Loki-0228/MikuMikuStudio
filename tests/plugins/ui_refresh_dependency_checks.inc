// Include after ui_refresh_subtree_checks.inc, menu_state_refresh_checks.inc and
// overflow_menu_checks.inc. UI probes use only their own HWNDs and menu loops.
#include "ui_refresh_dependencies.hpp"
#include <mms/refresh_dependency.h>

namespace ui_refresh_dependency_checks {
static MmsUiRefreshDependencyDescV1 Desc(const MmsUiTargetRefV1& upstream, const MmsUiTargetRefV1& dependent) {
    return {Header<MmsUiRefreshDependencyDescV1>(), upstream, dependent, 0, 0};
}
struct Fixture : ui_refresh_subtree_checks::Fixture {
    MmsUiRefreshDependencyApiV1 dependency = queue.DependencyApi(client);
    std::vector<MmsHandle> edges;
    std::vector<unsigned> dataOrder;
    std::vector<MmsHandle> layoutAnchors;
    Fixture() {
        onPrepare = [this](unsigned i) -> MmsResult { dataOrder.push_back(i); return MMS_OK; };
        queue.SetLayout([this](MmsHandle anchor) -> MmsResult {
            CHECK(registry.InCallback() && queue.ReadOnlyCause(client));
            layoutAnchors.push_back(anchor); ++layouts; stages.push_back(3);
            if (layoutResult) return layoutResult;
            queue.SyncTargets(presentations); return MMS_OK;
        });
    }
    MmsUiRefreshDependencyDescV1 EdgeDesc(MmsHandle from, MmsHandle to) { return Desc(Target(from).target, Target(to).target); }
    MmsHandle Edge(MmsHandle from, MmsHandle to) {
        const auto desc = EdgeDesc(from, to); MmsHandle edge = 0;
        CHECK(dependency.registerDependency(dependency.context, &desc, &edge) == MMS_OK && edge);
        edges.push_back(edge); return edge;
    }
    MmsUiRefreshRequestV1 Dependents(MmsHandle seed, uint32_t kinds) {
        auto request = Request(Target(seed)); request.scope = MMS_UI_REFRESH_DEPENDENTS;
        request.kinds = kinds; request.expectedGeometryRevision = 0; return request;
    }
    void Diamond() { Edge(group, element); Edge(group, second); Edge(element, separator); Edge(second, separator); }
};
static void GraphAndAtomicFailures() {
    Fixture x; auto dep = x.dependency;
    MmsUiRefreshDependencyCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshDependencyCapabilitiesV1>();
    CHECK(dep.getCapabilities(dep.context, &caps) == MMS_OK && caps.maxDependenciesPerPlugin == 512 && caps.maxDependentsPerRequest == 256 && !caps.flags && !caps.reserved);
    const auto plain = x.Target(x.group); CHECK(plain.supportedScopes == MMS_UI_REFRESH_SUBTREE);
    MmsHandle unchanged = 771; auto d = x.EdgeDesc(x.element, x.element);
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_CONFLICT && unchanged == 771);
    const auto ab = x.Edge(x.element, x.second); d = x.EdgeDesc(x.element, x.second);
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_CONFLICT && unchanged == 771);
    const auto bc = x.Edge(x.second, x.separator); d = x.EdgeDesc(x.separator, x.element);
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_CONFLICT && unchanged == 771);
    auto other = x.queue.DependencyApi(x.other);
    CHECK(other.unregisterDependency(other.context, ab) == MMS_E_ARGUMENT);
    // Matching textual owner IDs do not share edge ownership or graph revision.
    RefreshClient sameName{&x.queue, x.client.owner}; auto alias = x.queue.DependencyApi(sameName);
    CHECK(alias.unregisterDependency(alias.context, ab) == MMS_E_ARGUMENT);
    d = x.EdgeDesc(x.group, x.element); d.upstream.projectEpoch += 1;
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_STALE && unchanged == 771);
    d = x.EdgeDesc(x.group, x.element); d.dependent.instanceGeneration += 1;
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_STALE && unchanged == 771);
    d = x.EdgeDesc(x.group, x.element); d.flags = 1;
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_ARGUMENT && unchanged == 771);
    d = x.EdgeDesc(x.group, x.element); MmsResult workerAdd = MMS_OK, workerRemove = MMS_OK, workerCaps = MMS_OK;
    std::thread worker([&] {
        workerAdd = dep.registerDependency(dep.context, &d, &unchanged);
        workerRemove = dep.unregisterDependency(dep.context, ab);
        workerCaps = dep.getCapabilities(dep.context, &caps);
    }); worker.join();
    CHECK(workerAdd == MMS_E_THREAD && workerRemove == MMS_E_THREAD && workerCaps == MMS_E_THREAD && unchanged == 771);
    auto r = x.Dependents(x.element, MMS_UI_REFRESH_REPAINT); const auto pending = x.Submit(r);
    // This real allocation is the graph cycle-check's first target-ref buffer.
    // Failure must neither publish an edge nor stale an already accepted ticket.
    d = x.EdgeDesc(x.group, x.element); const auto hits = overflow_allocation_fault::hits;
    overflow_allocation_fault::bytes = sizeof(MmsUiTargetRefV1);
    const auto failure = dep.registerDependency(dep.context, &d, &unchanged);
    overflow_allocation_fault::bytes = 0;
    CHECK(failure == MMS_E_MEMORY && unchanged == 771 && overflow_allocation_fault::hits == hits + 1);
    CHECK(x.State(pending).status == MMS_UI_REFRESH_PENDING);
    x.queue.Drain(); x.Done(pending, MMS_UI_REFRESH_REPAINT);
    CHECK(dep.unregisterDependency(dep.context, bc) == MMS_OK);
    CHECK(dep.unregisterDependency(dep.context, bc) == MMS_E_NOT_FOUND);
    // The exact retained presentation must belong to the calling plugin.
    auto foreign = x.registry.Api(x.foreign); auto e = x.Button("example.foreign.dep");
    e.ownerPlugin = S("example.foreign"); MmsHandle foreignElement = 0;
    CHECK(foreign.registerElement(foreign.context, &e, &foreignElement) == MMS_OK);
    RefreshPresentation p{foreignElement, x.foreign.id, 1000, x.parent, 96, 0, 0, 20, 20, true};
    x.presentations.push_back(p); x.queue.SyncTargets(x.presentations);
    MmsUiRefreshTargetV1 foreignTarget{}; foreignTarget.h = Header<MmsUiRefreshTargetV1>(); auto foreignApi = x.queue.Api(x.other);
    CHECK(foreignApi.getTarget(foreignApi.context, foreignElement, &foreignTarget) == MMS_OK);
    d = Desc(x.Target(x.element).target, foreignTarget.target);
    CHECK(dep.registerDependency(dep.context, &d, &unchanged) == MMS_E_ARGUMENT && unchanged == 771);
    x.queue.Retire(sameName);
}
static void MapAllocationRollback() {
    // Only allocation fault targeting depends on the test compiler's tree-node
    // layout. Functional expectations use the public service and real tickets.
    struct OwnerValue { uint64_t revision; size_t count; };
    struct EdgeValue { const RefreshClient* owner; MmsUiTargetRefV1 upstream, dependent; };
    using OwnerPair = std::pair<const RefreshClient* const, OwnerValue>;
    using EdgePair = std::pair<const MmsHandle, EdgeValue>;
    const size_t ownerNode = sizeof(std::_Tree_node<OwnerPair, void*>);
    const size_t edgeNode = sizeof(std::_Tree_node<EdgePair, void*>);
    CHECK(ownerNode != sizeof(MmsUiTargetRefV1) && edgeNode != ownerNode && edgeNode != sizeof(MmsUiTargetRefV1));
    Fixture x; x.Edge(x.group, x.element);
    const auto pending = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_REPAINT));
    const auto d = x.EdgeDesc(x.group, x.panelElement);
    RefreshClient newcomer{&x.queue, x.client.owner}; const auto api = x.queue.DependencyApi(newcomer);
    auto fail = [&](const MmsUiRefreshDependencyApiV1& service, size_t bytes) {
        MmsHandle unchanged = 997; const auto before = overflow_allocation_fault::hits;
        overflow_allocation_fault::bytes = bytes;
        const auto result = service.registerDependency(service.context, &d, &unchanged);
        overflow_allocation_fault::bytes = 0;
        CHECK(result == MMS_E_MEMORY && unchanged == 997 && overflow_allocation_fault::hits == before + 1);
        CHECK(x.State(pending).status == MMS_UI_REFRESH_PENDING);
    };
    fail(api, ownerNode);
    fail(api, edgeNode);
    // If the failed edge insertion left its newly created empty owner behind,
    // this next owner-node allocation would be skipped and the call would pass.
    fail(api, ownerNode);
    // The existing owner's edge-map failure must preserve its graph revision.
    fail(x.dependency, edgeNode);
    const auto reads = x.stateReads; x.queue.Drain(); x.Done(pending, MMS_UI_REFRESH_REPAINT);
    CHECK(x.stateReads == reads && refresh_checks::Empty(x.panel));
    // No half edge may remain: the same edge must now register once, followed
    // by an ordinary duplicate conflict, and affect only this exact client.
    MmsHandle edge = 0; CHECK(api.registerDependency(api.context, &d, &edge) == MMS_OK && edge);
    MmsHandle unchanged = 997;
    CHECK(api.registerDependency(api.context, &d, &unchanged) == MMS_E_CONFLICT && unchanged == 997);
    CHECK(api.unregisterDependency(api.context, edge) == MMS_OK);
    CHECK(x.Target(x.group).supportedScopes & MMS_UI_REFRESH_DEPENDENTS);
    std::cout << "UI_DEPENDENCY_ALLOCATION owner_node_bytes=" << ownerNode << " edge_node_bytes=" << edgeNode << " faults=4 rollback=1\n";
    x.queue.Retire(newcomer);
}
static void UnboundPropertyData() {
    property_data_refresh_checks::Fixture x; const auto dependency = x.queue.DependencyApi(x.client);
    const auto d = Desc(x.Target(x.element).target, x.Target(x.inputs[0]).target); MmsHandle edge = 0;
    CHECK(dependency.registerDependency(dependency.context, &d, &edge) == MMS_OK && edge);
    auto request = x.Request(x.Target(x.element)); request.scope = MMS_UI_REFRESH_DEPENDENTS;
    request.expectedGeometryRevision = 0; request.kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT;
    const auto revision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    const auto ticket = x.Submit(request); x.values[0] = 42;
    CHECK(!x.reads[0] && !x.reads[1] && !x.hookCalls[0] && !x.hookCalls[1]);
    x.queue.Drain(); x.Done(ticket, request.kinds);
    CHECK(x.displays[0] == 42 && x.displays[1] == -1 && x.reads[0] == 1 && !x.reads[1]);
    CHECK(x.hookCalls[0] == 1 && !x.hookCalls[1] && x.releases[0] == 1);
    CHECK(!refresh_checks::Empty(x.windows[0]) && refresh_checks::Empty(x.windows[1]) && refresh_checks::Empty(x.button));
    CHECK(!x.prepares[0] && !x.prepares[1] && !x.edits && !x.stages && x.playhead == 31 && x.sceneRevision == 73);
    CHECK(x.registry.Revision() == revision && x.registry.PreferencesRevision() == preferences && !x.State(ticket).sourceRevision);
    // A bound complement is chosen from the accepted cohort; a binding added
    // afterwards invalidates the unbound-current promise before any callback.
    const auto stale = x.Submit(request); const auto reads = x.reads;
    x.Bind(0); x.queue.Drain(); x.Failed(stale, MMS_E_STALE); CHECK(x.reads == reads && !x.prepares[0]);
    const auto fresh = x.Submit(request); x.queue.Drain(); x.Done(fresh, request.kinds);
    CHECK(x.BoundDisplay(0) == 400 && x.prepares[0] == 1 && x.reads == reads);
    CHECK(dependency.unregisterDependency(dependency.context, edge) == MMS_OK);
}
static void ReplacedBindingsAndProviders() {
    {
        Fixture x; x.Edge(x.group, x.element); const auto request = x.Dependents(x.group, MMS_UI_REFRESH_DATA);
        const auto old = x.Submit(request);
        CHECK(x.data.unbind(x.data.context, x.bindings[0]) == MMS_OK); x.Bind(0);
        x.queue.Drain(); CHECK(x.State(old).result == MMS_E_STALE && !x.State(old).completedKinds && !x.prepares[0]);
        const auto fresh = x.Submit(request); x.queue.Drain(); x.Done(fresh, MMS_UI_REFRESH_DATA);
        CHECK(x.prepares[0] == 1 && x.Display(0) == 10);
    }
    for (bool replace : {false, true}) {
        state_content_checks::Fixture x; x.Register(); const auto dependency = x.queue.DependencyApi(x.client);
        const auto d = Desc(x.Target(1).target, x.Target(0).target); MmsHandle edge = 0;
        CHECK(dependency.registerDependency(dependency.context, &d, &edge) == MMS_OK && edge);
        auto request = x.Request(MMS_UI_REFRESH_STATE); request.target = x.Target(1).target; request.scope = MMS_UI_REFRESH_DEPENDENTS;
        const auto old = x.Submit(request); const auto before = x.stateReads;
        if (replace) {
            CHECK(x.content.unregisterProvider(x.content.context, x.provider) == MMS_OK); x.provider = 0; x.Register();
        } else { const auto replacement = x.Desc(); CHECK(x.content.updateProvider(x.content.context, x.provider, &replacement) == MMS_OK); }
        x.queue.Drain();
        CHECK(x.State(old).status == MMS_UI_REFRESH_FAILED && x.State(old).result == MMS_E_STALE && !x.State(old).completedKinds);
        CHECK(x.stateReads == before && !x.prepares);
        const auto fresh = x.Submit(request); x.queue.Drain(); x.Complete(fresh, MMS_UI_REFRESH_STATE);
        CHECK(x.prepares == 1 && state_content_checks::Fixture::Caption(x.controls[0]) == L"Updated caption");
        CHECK(dependency.unregisterDependency(dependency.context, edge) == MMS_OK);
    }
}
static void StateAndLayoutCausalProtection() {
    for (bool layout : {false, true}) {
        Fixture x; const auto edge = x.Edge(x.group, x.element);
        const auto prospective = x.EdgeDesc(x.element, x.panelElement);
        unsigned attempts = 0;
        auto denied = [&] {
            ++attempts; MmsHandle unchanged = 955;
            CHECK(x.registry.InCallback() && x.queue.ReadOnlyCause(x.client));
            CHECK(x.dependency.registerDependency(x.dependency.context, &prospective, &unchanged) == MMS_E_BUSY && unchanged == 955);
            CHECK(x.dependency.unregisterDependency(x.dependency.context, edge) == MMS_E_BUSY);
        };
        if (layout) x.queue.SetLayout([&](MmsHandle anchor) -> MmsResult { CHECK(anchor == x.element); denied(); return MMS_OK; });
        else x.onState = denied;
        const auto kinds = layout ? MMS_UI_REFRESH_LAYOUT : MMS_UI_REFRESH_STATE;
        auto ticket = x.Submit(x.Dependents(x.group, kinds)); x.queue.Drain(); x.Done(ticket, kinds);
        CHECK(attempts == 1 && !x.queue.ReadOnlyCause() && !x.registry.InCallback());
        x.onState = {}; x.queue.SetLayout([&](MmsHandle) -> MmsResult { return MMS_OK; });
        MmsHandle newEdge = 0;
        CHECK(x.dependency.registerDependency(x.dependency.context, &prospective, &newEdge) == MMS_OK && newEdge);
        ticket = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_REPAINT)); x.CleanAll(); x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_REPAINT);
        CHECK(!refresh_checks::Empty(x.panel) && !refresh_checks::Empty(x.button));
        CHECK(x.dependency.unregisterDependency(x.dependency.context, newEdge) == MMS_OK);
        CHECK(x.dependency.unregisterDependency(x.dependency.context, edge) == MMS_OK);
        MmsHandle unchanged = 955; const auto empty = x.Dependents(x.group, MMS_UI_REFRESH_REPAINT);
        CHECK(x.api.request(x.api.context, &empty, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 955);
    }
}
static void OrderedDiamondPipeline() {
    Fixture x; x.Diamond();
    const auto registry = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    CHECK(x.Target(x.group).supportedScopes == (MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS));
    x.Publish(1, 51); x.stateChecked = 1;
    x.onState = [&] { x.stages.push_back(2); CHECK(x.Display(0) == 10 && x.Display(1) == 51); };
    auto request = x.Dependents(x.group, 15); MmsHandle a = 0, b = 0; MmsResult accepted = MMS_E_FAILED;
    std::thread worker([&] { accepted = x.api.request(x.api.context, &request, &a); }); worker.join();
    CHECK(accepted == MMS_OK && a); x.tickets.push_back(a); b = x.Submit(request);
    CHECK(!x.prepares[0] && !x.prepares[1] && !x.stateReads && !x.layouts);
    x.queue.Drain(); x.Done(a, 15); x.Done(b, 15);
    CHECK(x.prepares[0] == 1 && x.prepares[1] == 1 && x.stateReads == 2 && x.layouts == 1);
    CHECK(x.dataOrder == std::vector<unsigned>({0, 1}) && x.stages == std::vector<unsigned>({1, 1, 2, 2, 3}));
    CHECK(x.layoutAnchors == std::vector<MmsHandle>({x.element}));
    CHECK(!refresh_checks::Empty(x.button) && !refresh_checks::Empty(x.sibling) && !refresh_checks::Empty(x.separatorWindow));
    CHECK(refresh_checks::Empty(x.parent) && refresh_checks::Empty(x.panel));
    CHECK(x.registry.Revision() == registry && x.registry.PreferencesRevision() == preferences && !x.invokes && x.creates == 1);
    // A single explicit cross-position edge reaches a panel but never the seed's
    // unrelated retained children. No subtree expansion is implicit here.
    const auto edge = x.Edge(x.element, x.panelElement); (void)edge; x.CleanAll();
    const auto reads = x.stateReads; const auto prepares = x.prepares;
    auto t = x.Submit(x.Dependents(x.element, MMS_UI_REFRESH_REPAINT)); x.queue.Drain(); x.Done(t, MMS_UI_REFRESH_REPAINT);
    if (!refresh_checks::Empty(x.button) || !refresh_checks::Empty(x.sibling) ||
        refresh_checks::Empty(x.separatorWindow) || refresh_checks::Empty(x.panel)) {
        std::cerr << "DEPENDENCY_CROSS_POSITION empty seed=" << refresh_checks::Empty(x.button)
            << " sibling=" << refresh_checks::Empty(x.sibling)
            << " separator=" << refresh_checks::Empty(x.separatorWindow)
            << " panel=" << refresh_checks::Empty(x.panel) << '\n';
    }
    CHECK(refresh_checks::Empty(x.button) && refresh_checks::Empty(x.sibling) && !refresh_checks::Empty(x.separatorWindow) && !refresh_checks::Empty(x.panel));
    CHECK(x.stateReads == reads && x.prepares == prepares);
}
static void TicketVersionsAndCancellation() {
    Fixture x; x.Diamond(); auto r = x.Dependents(x.group, 15);
    const auto a = x.Submit(r), cancel = x.Submit(r), released = x.Submit(r);
    CHECK(x.api.cancel(x.api.context, cancel) == MMS_OK && x.api.release(x.api.context, released) == MMS_OK);
    x.queue.Drain(); x.Done(a, 15);
    CHECK(x.State(cancel).status == MMS_UI_REFRESH_CANCELLED && x.prepares[0] == 1 && x.prepares[1] == 1 && x.layouts == 1);
    MmsUiRefreshStateV1 inaccessible{}; inaccessible.h = Header<MmsUiRefreshStateV1>();
    CHECK(x.api.query(x.api.context, released, &inaccessible) == MMS_E_NOT_FOUND);
    const auto stale = x.Submit(r); const auto added = x.Edge(x.element, x.panelElement);
    CHECK(x.State(stale).status == MMS_UI_REFRESH_FAILED && x.State(stale).result == MMS_E_STALE);
    auto newer = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_REPAINT));
    CHECK(x.dependency.unregisterDependency(x.dependency.context, added) == MMS_OK);
    CHECK(x.State(newer).result == MMS_E_STALE);
    // Geometry changes preserve edges. An explicitly fixed mapping still stales.
    auto moving = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_REPAINT));
    auto fixed = x.Dependents(x.group, MMS_UI_REFRESH_REPAINT); fixed.expectedGeometryRevision = x.Target(x.group).geometryRevision;
    const auto fixedTicket = x.Submit(fixed); x.presentations[0].x += 7; x.queue.SyncTargets(x.presentations);
    x.queue.Drain(); x.Done(moving, MMS_UI_REFRESH_REPAINT); CHECK(x.State(fixedTicket).result == MMS_E_STALE);
    // Publishing a new source revision does not invalidate an accepted cohort.
    auto oldData = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_DATA)); x.Publish(1, 72);
    auto newData = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_DATA));
    const auto before = x.prepares; x.queue.Drain(); x.Done(oldData, MMS_UI_REFRESH_DATA); x.Done(newData, MMS_UI_REFRESH_DATA);
    CHECK(x.Display(1) == 72 && x.prepares[0] == before[0] && x.prepares[1] == before[1] + 1);
    CHECK(x.State(oldData).sourceRevision == 0 && x.State(newData).sourceRevision == 0);
}
static void PhaseFailuresAndCausalProtection() {
    Fixture x; x.Diamond(); auto request = x.Dependents(x.group, 15);
    const auto d = x.EdgeDesc(x.element, x.panelElement);
    x.onPrepare = [&](unsigned) -> MmsResult {
        MmsHandle unchanged = 918;
        CHECK(x.dependency.registerDependency(x.dependency.context, &d, &unchanged) == MMS_E_BUSY && unchanged == 918);
        CHECK(x.dependency.unregisterDependency(x.dependency.context, x.edges.front()) == MMS_E_BUSY);
        CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_BUSY && unchanged == 918);
        CHECK(x.ui.unregisterItem(x.ui.context, x.second) == MMS_E_BUSY);
        MmsResult inherited = MMS_OK; std::thread worker([&] { inherited = x.api.request(x.api.context, &request, &unchanged); }); worker.join();
        CHECK(inherited == MMS_E_BUSY && unchanged == 918); x.queue.Drain(); return MMS_OK;
    };
    auto t = x.Submit(request); x.queue.Drain(); x.Done(t, 15);
    CHECK(!x.queue.ReadOnlyCause() && !x.registry.InCallback());
    x.Publish(0, 99); x.onPrepare = [](unsigned i) -> MmsResult { return i ? MMS_E_FAILED : MMS_OK; };
    // Changed source 0 is prepared first; source 1's next version forces the
    // second callback to fail before STATE, LAYOUT or REPAINT can start.
    x.Publish(1, 100); x.CleanAll(); const auto reads = x.stateReads, layouts = x.layouts;
    t = x.Submit(request); x.queue.Drain();
    CHECK(x.State(t).status == MMS_UI_REFRESH_FAILED && x.State(t).result == MMS_E_FAILED && !x.State(t).completedKinds);
    CHECK(x.stateReads == reads && x.layouts == layouts && refresh_checks::Empty(x.button));
    x.onPrepare = {}; x.layoutResult = MMS_E_FAILED; t = x.Submit(request); x.queue.Drain();
    CHECK(x.State(t).result == MMS_E_FAILED && x.State(t).completedKinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE));
    CHECK(!x.queue.ReadOnlyCause() && !x.registry.InCallback());
    x.layoutResult = MMS_OK; x.onState = [&] { x.queue.AdvanceProjectEpoch(); };
    t = x.Submit(x.Dependents(x.group, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT)); x.CleanAll(); x.queue.Drain();
    CHECK(x.State(t).result == MMS_E_STALE && !x.State(t).completedKinds && refresh_checks::Empty(x.button));
}
static void LifecycleAndRequestValidation() {
    for (unsigned change = 0; change < 5; ++change) {
        Fixture x; x.Edge(x.group, x.element); auto r = x.Dependents(x.group, MMS_UI_REFRESH_REPAINT);
        const auto oldDesc = x.EdgeDesc(x.group, x.element);
        const auto ticket = x.Submit(r);
        if (change == 0) { ++x.presentations[0].instance; x.queue.SyncTargets(x.presentations); }
        else if (change == 1) x.queue.InvalidateTarget(x.element);
        else if (change == 2) x.queue.AdvanceProjectEpoch();
        else if (change == 3) x.queue.Retire(x.client);
        else { CHECK(x.ui.unregisterItem(x.ui.context, x.element) == MMS_OK); x.queue.SyncTopology(); }
        x.queue.Drain();
        CHECK(x.State(ticket).result == (change == 3 ? MMS_E_CANCELLED : MMS_E_STALE));
        CHECK(!x.prepares[0] && !x.stateReads && !x.layouts);
        MmsHandle unchanged = 811;
        CHECK(x.dependency.unregisterDependency(x.dependency.context, x.edges.front()) == MMS_E_NOT_FOUND);
        const auto result = x.dependency.registerDependency(x.dependency.context, &oldDesc, &unchanged);
        CHECK(result == (change == 3 ? MMS_E_NOT_READY : MMS_E_STALE) && unchanged == 811);
    }
    Fixture x; MmsHandle unchanged = 881; auto request = x.Dependents(x.group, MMS_UI_REFRESH_REPAINT);
    CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 881);
    x.Edge(x.group, x.panelElement); request = x.Dependents(x.group, MMS_UI_REFRESH_STATE);
    CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 881);
    request = x.Dependents(x.group, MMS_UI_REFRESH_REPAINT); MmsUiDirtyRectV1 rect{0, 0, 4, 4};
    request.dirtyRects = {&rect, 1, sizeof(rect), 1};
    CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 881);
    request = x.Dependents(x.group, MMS_UI_REFRESH_DATA); request.binding = x.bindings[0]; request.sourceRevision = x.revisions[0];
    CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_ARGUMENT && unchanged == 881);
    x.queue.SetLayout({}); request = x.Dependents(x.group, MMS_UI_REFRESH_LAYOUT);
    CHECK(x.api.request(x.api.context, &request, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 881);
}
static void MenuMixedDeferralAndLayoutAnchor() {
    menu_state_refresh_checks::Fixture x; auto dependency = x.queue.DependencyApi(x.client);
    auto add = [&](MmsHandle from, MmsHandle to) {
        const auto d = Desc(x.NativeTarget(from).target, x.NativeTarget(to).target); MmsHandle edge = 0;
        CHECK(dependency.registerDependency(dependency.context, &d, &edge) == MMS_OK && edge); return edge;
    };
    add(x.input, x.button); add(x.input, x.elements[1]); x.Register();
    auto request = x.Request(1, MMS_UI_REFRESH_DEPENDENTS); request.target = x.NativeTarget(x.input).target;
    MmsHandle waiting = 0; unsigned wakesAtExit = 0;
    x.label = "Dependency loop result";
    x.Loop([&] {
        const auto reads = x.reads; const auto prepares = x.prepares; const auto wakes = x.wakeSignals.load();
        waiting = x.Submit(request, true);
        for (unsigned i = 0; i < 12; ++i) x.queue.Drain();
        CHECK(x.State(waiting).status == MMS_UI_REFRESH_PENDING && !x.queue.Pending());
        CHECK(x.reads == reads && x.prepares == prepares && x.wakeSignals == wakes);
        // Unrelated SELF work remains runnable while the mixed group waits.
        auto self = request; self.target = x.NativeTarget(x.button).target; self.scope = MMS_UI_REFRESH_SELF;
        const auto independent = x.Submit(self, true); x.queue.Drain();
        CHECK(x.State(independent).status == MMS_UI_REFRESH_COMPLETED && x.State(waiting).status == MMS_UI_REFRESH_PENDING);
        wakesAtExit = x.wakeSignals;
    });
    CHECK(x.wakeSignals == wakesAtExit + 1); x.queue.Drain();
    CHECK(x.State(waiting).status == MMS_UI_REFRESH_COMPLETED && x.At(1).caption == L"Dependency loop result");
    manager_refresh_checks::Pump(35); CHECK(!x.retryDispatches);
    // Seed menus do not support LAYOUT. Only the dependent native button is a
    // valid anchor, and no native menu callback is needed for this request.
    add(x.elements[3], x.button); MmsHandle anchor = 0;
    x.queue.SetLayout([&](MmsHandle element) -> MmsResult { anchor = element; return MMS_OK; });
    auto layout = x.Request(3, MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_LAYOUT);
    const auto t = x.Submit(layout, true); x.queue.Drain();
    CHECK(x.State(t).status == MMS_UI_REFRESH_COMPLETED && x.State(t).completedKinds == MMS_UI_REFRESH_LAYOUT && anchor == x.button);
    CHECK(!x.history.UndoCount() && !x.history.RedoCount() && !x.calls[0] && !x.calls[1] && !x.calls[2]);
}
static void GraphQuotasAndExpansionBound() {
    UiRefreshDependencies graph; std::array<std::unique_ptr<RefreshClient>, 9> clients;
    const auto ref = [](MmsHandle h) { return MmsUiTargetRefV1{Header<MmsUiTargetRefV1>(), h, 1, 1}; };
    for (unsigned i = 0; i < clients.size(); ++i) clients[i] = std::unique_ptr<RefreshClient>(new RefreshClient{nullptr, "example.quota" + std::to_string(i)});
    {
        UiRefreshDependencies ordered;
        for (const auto pair : {std::pair<MmsHandle, MmsHandle>{9, 7}, {9, 5}, {7, 2}, {5, 2}, {4, 1}}) {
            MmsHandle handle = 0; CHECK(ordered.Add(*clients[0], ref(pair.first), ref(pair.second), handle) == MMS_OK);
        }
        std::vector<MmsUiTargetRefV1> capture; uint64_t version = 0;
        CHECK(ordered.Capture(*clients[0], ref(9), capture, version) == MMS_OK && capture.size() == 3);
        CHECK(capture[0].element == 5 && capture[1].element == 7 && capture[2].element == 2);
        MmsHandle unchanged = 991;
        CHECK(ordered.Add(*clients[0], ref(2), ref(9), unchanged) == MMS_E_CONFLICT && unchanged == 991);
        CHECK(ordered.Revision(*clients[0]) == version);
    }
    // Exercise the production graph's actual global/per-client quotas without
    // creating thousands of equivalent USER32 controls.
    for (unsigned owner = 0; owner < 8; ++owner) {
        for (unsigned edge = 0; edge < 512; ++edge) {
            MmsHandle handle = 0; CHECK(graph.Add(*clients[owner], ref(1), ref(2 + edge), handle) == MMS_OK && handle);
        }
        MmsHandle unchanged = 718; CHECK(graph.Add(*clients[owner], ref(1), ref(600), unchanged) == MMS_E_BUSY && unchanged == 718);
    }
    MmsHandle unchanged = 718; CHECK(graph.Add(*clients[8], ref(1), ref(2), unchanged) == MMS_E_BUSY && unchanged == 718);
    std::vector<MmsUiTargetRefV1> captured{ref(777)}; uint64_t version = 998;
    CHECK(graph.Capture(*clients[0], ref(1), captured, version) == MMS_E_BUSY && captured.size() == 1 && captured[0].element == 777 && version == 998);
    graph.Retire(*clients[7]); CHECK(graph.Add(*clients[8], ref(1), ref(2), unchanged) == MMS_OK);
    graph.Clear();
    for (unsigned i = 0; i < 256; ++i) { MmsHandle h = 0; CHECK(graph.Add(*clients[0], ref(1), ref(2 + i), h) == MMS_OK); }
    CHECK(graph.Capture(*clients[0], ref(1), captured, version) == MMS_OK && captured.size() == 256);
    CHECK(captured.front().element == 2 && captured.back().element == 257);
    const auto prior = version; MmsHandle h = 0; CHECK(graph.Add(*clients[0], ref(1), ref(258), h) == MMS_OK);
    CHECK(graph.Capture(*clients[0], ref(1), captured, version) == MMS_E_BUSY && captured.size() == 256 && version == prior);
}
} // namespace ui_refresh_dependency_checks
static void UiRefreshDependencyChecks() {
    ui_refresh_dependency_checks::GraphAndAtomicFailures();
    ui_refresh_dependency_checks::MapAllocationRollback();
    ui_refresh_dependency_checks::UnboundPropertyData();
    ui_refresh_dependency_checks::ReplacedBindingsAndProviders();
    ui_refresh_dependency_checks::StateAndLayoutCausalProtection();
    ui_refresh_dependency_checks::OrderedDiamondPipeline();
    ui_refresh_dependency_checks::TicketVersionsAndCancellation();
    ui_refresh_dependency_checks::PhaseFailuresAndCausalProtection();
    ui_refresh_dependency_checks::LifecycleAndRequestValidation();
    ui_refresh_dependency_checks::MenuMixedDeferralAndLayoutAnchor();
    ui_refresh_dependency_checks::GraphQuotasAndExpansionBound();
}
