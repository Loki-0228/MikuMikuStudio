#include "ui_refresh_binding.hpp"
#include <mms/cpp/refresh.hpp>

namespace ui_refresh_subtree_checks {
struct Fixture : refresh_checks::Fixture {
    MmsHandle group = 0, nested = 0, separator = 0;
    HWND separatorWindow = nullptr;
    MmsUiRefreshBindingApiV1 data = queue.Bindings().Api(client);
    std::array<MmsHandle, 2> sources{}, bindings{};
    std::array<uint64_t, 2> revisions{}, preparedRevisions{};
    std::array<unsigned, 2> prepares{};
    std::vector<unsigned> stages;
    std::function<MmsResult(unsigned)> onPrepare;
    unsigned layouts = 0;
    MmsResult layoutResult = MMS_OK;
    struct Prepare { Fixture* fixture; unsigned index; } context[2]{{this,0},{this,1}};
    Fixture() {
        MmsUiElementV1 g{}; g.h = Header<MmsUiElementV1>(); g.id = S("example.refresh.tree");
        g.ownerPlugin = S("example.refresh"); g.parentId = S("mms.toolbar.main.extensions");
        g.kind = MMS_UI_GROUP; g.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT | MMS_UI_MOVE;
        CHECK(ui.registerElement(ui.context, &g, &group) == MMS_OK);
        g.id = S("example.refresh.tree.nested"); g.parentId = S("example.refresh.tree");
        CHECK(ui.registerElement(ui.context, &g, &nested) == MMS_OK);
        Reparent("example.refresh.button", "example.refresh.tree");
        Reparent("example.refresh.sibling", "example.refresh.tree.nested");
        auto e = Button("example.refresh.separator"); e.kind = MMS_UI_SEPARATOR; e.commandId = {};
        e.parentId = S("example.refresh.tree.nested");
        CHECK(ui.registerElement(ui.context, &e, &separator) == MMS_OK);
        separatorWindow = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            350, 50, 2, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(separatorWindow);
        for (unsigned i = 0; i < 2; ++i) {
            SendMessageW(i ? sibling : button, BM_SETSTYLE, BS_CHECKBOX | BS_PUSHLIKE, FALSE);
            presentations[i].stateRefresh = presentations[i].layoutRefresh = true;
        }
        RefreshPresentation p{separator, owner.id, 14, separatorWindow, 96, 350, 50, 2, 100, true}; p.layoutRefresh = true;
        presentations.push_back(p);
        p = {group, owner.id, 15, parent, 96, 40, 50, 500, 120, true}; p.container = p.layoutRefresh = true;
        presentations.push_back(p); p.element = nested; p.instance = 16; presentations.push_back(p);
        queue.SyncTargets(presentations);
        queue.SetLayout([this](MmsHandle target) -> MmsResult {
            CHECK(target == group || target == nested || target == element || target == second);
            CHECK(registry.InCallback() && queue.ReadOnlyCause(client)); ++layouts; stages.push_back(3);
            if (layoutResult) return layoutResult;
            queue.SyncTargets(presentations); return MMS_OK;
        });
        for (unsigned i = 0; i < 2; ++i) {
            const auto id = std::string("subtree-source-") + char('0' + i);
            MmsUiDataSourceDescV1 d{}; d.h = Header<MmsUiDataSourceDescV1>(); d.id = Text(id); d.maxBytes = 64;
            CHECK(data.registerSource(data.context, &d, &sources[i]) == MMS_OK);
            Publish(i, int(10 + i)); Bind(i);
        }
        CleanAll();
    }
    void Reparent(const char* id, const char* parentId) {
        // parentId is a caller-owned C string. Text(const std::string&) would
        // borrow an implicit temporary destroyed before updateElement runs.
        auto e = registry.elements.at(id).desc; e.parentId = S(parentId);
        CHECK(ui.updateElement(ui.context, registry.elements.at(id).handle, &e) == MMS_OK);
    }
    void Publish(unsigned i, int value) {
        MmsUiDataSourcePublishV1 p{}; p.h = Header<MmsUiDataSourcePublishV1>();
        p.source = sources[i]; p.expectedRevision = revisions[i]; p.bytes = {&value, sizeof(value)};
        MmsUiDataSourceStateV1 s{}; s.h = Header<MmsUiDataSourceStateV1>();
        CHECK(data.publishSource(data.context, &p, &s) == MMS_OK); revisions[i] = s.revision;
    }
    void Bind(unsigned i, MmsHandle source = 0) {
        MmsUiDataBindingDescV1 b{}; b.h = Header<MmsUiDataBindingDescV1>();
        b.target = Target(i ? second : element).target; b.source = source ? source : sources[i];
        b.context = &context[i]; b.maxDisplayBytes = 64;
        b.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
            auto& c = *static_cast<Prepare*>(p); auto& x = *c.fixture;
            ++x.prepares[c.index]; x.preparedRevisions[c.index] = input->sourceRevision; x.stages.push_back(1);
            CHECK(x.registry.InCallback() && x.queue.ReadOnlyCause(x.client));
            if (x.onPrepare) if (const auto result = x.onPrepare(c.index)) return result;
            return x.data.writePrepared(x.data.context, input->builder, input->sourceBytes);
        };
        CHECK(data.bind(data.context, &b, &bindings[i]) == MMS_OK);
    }
    int Display(unsigned i) {
        MmsUiDataDisplayV1 d{}; d.h = Header<MmsUiDataDisplayV1>();
        CHECK(data.acquireDisplay(data.context, bindings[i], &d) == MMS_OK && d.bytes.bytes == sizeof(int));
        int value = 0; std::memcpy(&value, d.bytes.data, sizeof(value));
        CHECK(data.releaseDisplay(data.context, d.lease) == MMS_OK); return value;
    }
    MmsUiRefreshRequestV1 Tree(uint32_t kinds, MmsHandle root = 0) {
        auto r = Request(Target(root ? root : group)); r.scope = MMS_UI_REFRESH_SUBTREE;
        r.kinds = kinds; r.expectedGeometryRevision = 0; return r;
    }
    void Done(MmsHandle h, uint32_t kinds) {
        auto s = State(h); CHECK(s.status == MMS_UI_REFRESH_COMPLETED && s.result == MMS_OK && s.completedKinds == kinds);
        CHECK(!s.sourceRevision); // A version set is not a scalar source revision.
    }
    void CleanAll() { Clean(); if (separatorWindow) { UpdateWindow(separatorWindow); ValidateRect(separatorWindow, nullptr); } }
};

static void PipelineAndScopes() {
    Fixture x; auto root = x.Target(x.group);
    CHECK(root.target.element == x.group && root.supportedScopes == MMS_UI_REFRESH_SUBTREE && root.supportedKinds == 15);
    CHECK(x.Target(x.nested).supportedKinds == 15);
    const auto revision = x.registry.Revision(), prefs = x.registry.PreferencesRevision();
    x.Publish(1, 21); x.stateChecked = 1;
    x.onState = [&] {
        x.stages.push_back(2); CHECK(x.Display(0) == 10 && x.Display(1) == 21);
        CHECK(x.preparedRevisions[0] == 1 && x.preparedRevisions[1] == 2);
    };
    auto r = x.Tree(15); MmsHandle ticket = 0;
    MmsResult accepted = MMS_E_FAILED;
    std::thread worker([&] { accepted = x.api.request(x.api.context, &r, &ticket); }); worker.join();
    CHECK(accepted == MMS_OK && ticket); x.tickets.push_back(ticket);
    CHECK(x.prepares[0] == 0 && x.prepares[1] == 0 && x.stateReads == 0 && x.layouts == 0);
    x.queue.Drain(); x.Done(ticket, 15);
    CHECK(x.stages == std::vector<unsigned>({1,1,2,2,3}) && x.layouts == 1 && x.stateReads == 2);
    CHECK(SendMessageW(x.button, BM_GETCHECK, 0, 0) == BST_CHECKED && SendMessageW(x.sibling, BM_GETCHECK, 0, 0) == BST_CHECKED);
    CHECK(!refresh_checks::Empty(x.button) && !refresh_checks::Empty(x.sibling) && !refresh_checks::Empty(x.separatorWindow));
    CHECK(refresh_checks::Empty(x.panel) && refresh_checks::Empty(x.parent));
    CHECK(x.registry.Revision() == revision && x.registry.PreferencesRevision() == prefs && x.invokes == 0 && x.creates == 1);
    x.onState = {}; x.CleanAll(); auto nested = x.Submit(x.Tree(MMS_UI_REFRESH_REPAINT, x.nested));
    x.queue.Drain(); x.Done(nested, MMS_UI_REFRESH_REPAINT);
    CHECK(refresh_checks::Empty(x.button) && !refresh_checks::Empty(x.sibling) && !refresh_checks::Empty(x.separatorWindow));
    auto bad = x.Tree(MMS_UI_REFRESH_REPAINT); bad.scope = MMS_UI_REFRESH_SELF;
    MmsHandle out = 777; CHECK(x.api.request(x.api.context, &bad, &out) == MMS_E_UNSUPPORTED && out == 777);
    MmsUiDirtyRectV1 rect{0,0,5,5}; bad = x.Tree(MMS_UI_REFRESH_REPAINT); bad.dirtyRects = {&rect,1,sizeof(rect),1};
    CHECK(x.api.request(x.api.context, &bad, &out) == MMS_E_ARGUMENT && out == 777);
    bad = x.Tree(MMS_UI_REFRESH_DATA); bad.binding = x.bindings[0]; bad.sourceRevision = x.revisions[0];
    CHECK(x.api.request(x.api.context, &bad, &out) == MMS_E_ARGUMENT && out == 777);
    bad = x.Tree(MMS_UI_REFRESH_STATE, x.panelElement);
    CHECK(x.api.request(x.api.context, &bad, &out) == MMS_E_UNSUPPORTED && out == 777);
    auto panel = x.Submit(x.Tree(MMS_UI_REFRESH_REPAINT, x.panelElement)); x.queue.Drain(); x.Done(panel, MMS_UI_REFRESH_REPAINT);
}
static void VersionsCancellationAndFailures() {
    Fixture x; auto r = x.Tree(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE);
    auto first = x.Submit(r), same = x.Submit(r), cancelled = x.Submit(r);
    CHECK(x.api.cancel(x.api.context, cancelled) == MMS_OK);
    x.Publish(0, 42); x.Publish(1, 43);
    x.onState = [&] { CHECK(x.Display(0) == 10 && x.Display(1) == 11); x.stages.push_back(2); };
    x.queue.Drain(); x.Done(first, r.kinds); x.Done(same, r.kinds);
    CHECK(x.State(cancelled).status == MMS_UI_REFRESH_CANCELLED && x.prepares[0] == 1 && x.prepares[1] == 1 && x.stateReads == 2);
    x.onState = {}; auto next = x.Submit(x.Tree(MMS_UI_REFRESH_DATA)); x.queue.Drain(); x.Done(next, MMS_UI_REFRESH_DATA);
    CHECK(x.Display(0) == 42 && x.Display(1) == 43 && x.preparedRevisions[0] == 2 && x.preparedRevisions[1] == 2);
    x.Publish(0, 50); x.Publish(1, 51); x.CleanAll();
    x.onPrepare = [](unsigned i) { return i == 1 ? MMS_E_FAILED : MMS_OK; };
    auto failed = x.Submit(x.Tree(15)); const auto states = x.stateReads; x.queue.Drain();
    CHECK(x.State(failed).status == MMS_UI_REFRESH_FAILED && x.State(failed).result == MMS_E_FAILED && !x.State(failed).completedKinds);
    CHECK(x.Display(0) == 50 && x.Display(1) == 43 && x.stateReads == states && !x.layouts);
    // UI application is not rolled back, but no dependent phase runs.
    x.onPrepare = {}; x.layoutResult = MMS_E_FAILED;
    failed = x.Submit(x.Tree(15)); x.queue.Drain();
    CHECK(x.State(failed).result == MMS_E_FAILED && x.State(failed).completedKinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE));
    CHECK(!x.queue.ReadOnlyCause() && !x.registry.InCallback());
}
static void CohortTopologyAndReadOnly() {
    for (unsigned change = 0; change < 6; ++change) {
        Fixture x; auto pending = x.Submit(x.Tree(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE));
        if (change == 0) { CHECK(x.data.unbind(x.data.context, x.bindings[1]) == MMS_OK); x.Bind(1); }
        else if (change == 1) { CHECK(x.data.unbind(x.data.context, x.bindings[1]) == MMS_OK); }
        else if (change == 2) { x.Reparent("example.refresh.sibling", "mms.toolbar.main.extensions"); }
        else if (change == 3) { ++x.presentations[1].instance; x.queue.SyncTargets(x.presentations); }
        else if (change == 4) { x.queue.AdvanceProjectEpoch(); }
        else { x.queue.Retire(x.client); }
        x.queue.Drain(); const auto s = x.State(pending);
        CHECK(s.status == (change == 5 ? MMS_UI_REFRESH_CANCELLED : MMS_UI_REFRESH_FAILED));
        CHECK(s.result == (change == 5 ? MMS_E_CANCELLED : MMS_E_STALE) && !s.completedKinds && !x.prepares[0] && !x.prepares[1]);
    }
    Fixture x; auto r = x.Tree(15); x.onPrepare = [&](unsigned) -> MmsResult {
        MmsHandle out = 918;
        CHECK(x.api.request(x.api.context, &r, &out) == MMS_E_BUSY && out == 918);
        CHECK(x.ui.unregisterItem(x.ui.context, x.second) == MMS_E_BUSY);
        x.queue.Drain(); return MMS_OK;
    };
    auto ticket = x.Submit(r); x.queue.Drain(); x.Done(ticket, 15);
    CHECK(x.prepares[0] == 1 && x.prepares[1] == 1 && x.layouts == 1);
    x.onState = [&] { x.queue.AdvanceProjectEpoch(); };
    ticket = x.Submit(x.Tree(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT)); x.CleanAll(); x.queue.Drain();
    CHECK(x.State(ticket).result == MMS_E_STALE && !x.State(ticket).completedKinds && refresh_checks::Empty(x.button));
}
static void DataSkipsUnboundCollapsedAndAtomicAcceptance() {
    Fixture x;
    CHECK(x.data.unbind(x.data.context, x.bindings[1]) == MMS_OK);
    x.presentations[1].width = x.presentations[1].height = 0; x.queue.SyncTargets(x.presentations);
    auto ticket = x.Submit(x.Tree(MMS_UI_REFRESH_DATA)); x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_DATA);
    CHECK(x.prepares[0] == 1 && !x.prepares[1] && x.Display(0) == 10);
    x.presentations[1].width = 180; x.presentations[1].height = 120; x.queue.SyncTargets(x.presentations); x.Bind(1);
    // A late binding in the captured set is a topology change for DATA.
    CHECK(x.data.unbind(x.data.context, x.bindings[1]) == MMS_OK);
    ticket = x.Submit(x.Tree(MMS_UI_REFRESH_DATA)); x.Bind(1); x.queue.Drain();
    CHECK(x.State(ticket).result == MMS_E_STALE && x.prepares[0] == 1);
    // An unpublished bound source rejects the entire request before a ticket
    // is published, including the otherwise-valid first binding.
    CHECK(x.data.unbind(x.data.context, x.bindings[1]) == MMS_OK);
    MmsUiDataSourceDescV1 d{}; d.h = Header<MmsUiDataSourceDescV1>(); d.id = S("unpublished"); d.maxBytes = 64;
    MmsHandle empty = 0; CHECK(x.data.registerSource(x.data.context, &d, &empty) == MMS_OK); x.Bind(1, empty);
    auto r = x.Tree(MMS_UI_REFRESH_DATA); MmsHandle unchanged = 997;
    CHECK(x.api.request(x.api.context, &r, &unchanged) == MMS_E_NOT_READY && unchanged == 997 && !x.queue.Pending());
}
static void ForeignOwnerAndFixedGeometry() {
    Fixture x; auto r = x.Tree(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE); r.expectedGeometryRevision = x.Target(x.group).geometryRevision;
    auto pending = x.Submit(r); x.presentations[1].x += 1; x.queue.SyncTargets(x.presentations); x.queue.Drain();
    CHECK(x.State(pending).result == MMS_E_STALE && !x.prepares[0]);
    auto e = x.Button("example.foreign.child"); e.ownerPlugin = S("example.foreign"); e.parentId = S("example.refresh.tree");
    auto foreign = x.registry.Api(x.foreign); MmsHandle foreignElement = 0;
    CHECK(foreign.registerElement(foreign.context, &e, &foreignElement) == MMS_OK);
    auto fresh = x.Tree(MMS_UI_REFRESH_DATA); MmsHandle unchanged = 778;
    CHECK(x.api.request(x.api.context, &fresh, &unchanged) == MMS_E_ARGUMENT && unchanged == 778);
}
static void SharedSourceCapture() {
    Fixture x;
    CHECK(x.data.unbind(x.data.context, x.bindings[1]) == MMS_OK); x.Bind(1, x.sources[0]);
    std::atomic<bool> started{false}; MmsResult published = MMS_OK;
    std::thread publisher([&] {
        uint64_t expected = x.revisions[0];
        started.store(true);
        for (int value = 100; value < 500; ++value) {
            MmsUiDataSourcePublishV1 update{}; update.h = Header<MmsUiDataSourcePublishV1>(); update.source = x.sources[0];
            update.expectedRevision = expected;
            update.bytes = {&value,sizeof(value)}; MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
            if ((published = x.data.publishSource(x.data.context, &update, &state))) break;
            expected = state.revision;
        }
    });
    while (!started.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 16; ++i) x.Submit(x.Tree(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE));
    publisher.join(); CHECK(published == MMS_OK);
    x.onState = [&] { CHECK(x.preparedRevisions[0] == x.preparedRevisions[1] && x.Display(0) == x.Display(1)); };
    x.queue.Drain();
    for (const auto ticket : x.tickets) x.Done(ticket, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE);
}
static void NativeGroupAndSdk() {
    layout_refresh_checks::Fixture x(400);
    MmsUiElementV1 g{}; g.h = Header<MmsUiElementV1>(); g.id = S("example.layout.group"); g.ownerPlugin = Text(x.owner.id);
    g.parentId = S("mms.toolbar.main.extensions"); g.kind = MMS_UI_GROUP; g.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT | MMS_UI_MOVE;
    MmsHandle group = 0; CHECK(x.ui.registerElement(x.ui.context, &g, &group) == MMS_OK);
    for (const char* id : {"example.layout.first", "example.layout.second"}) {
        auto e = x.registry.elements.at(id).desc; e.parentId = g.id;
        CHECK(x.ui.updateElement(x.ui.context, x.registry.elements.at(id).handle, &e) == MMS_OK);
    }
    x.native->Refresh(); CHECK(x.native->Layout(10,400,layout_refresh_checks::Fixture::heightLimit,true) > 0);
    MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
    CHECK(x.api.getTarget(x.api.context, group, &target) == MMS_OK && target.target.element == group && target.supportedScopes == MMS_UI_REFRESH_SUBTREE);
    CHECK(target.supportedKinds == (MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT));
    MmsHostV1 host{}; host.h = Header<MmsHostV1>(); host.context = &x.api;
    host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
        if (String(name) != "mms.ui.refresh" || major != 1 || minor != 0 || size != sizeof(MmsUiRefreshApiV1)) return MMS_E_VERSION;
        std::memcpy(out, p, size); return MMS_OK;
    };
    mms::sdk::RefreshServices service; CHECK(mms::sdk::RefreshServices::Query(host, service) == MMS_OK);
    const auto reads = x.stateReads; const auto firstWindow = x.controls[0], secondWindow = x.controls[1];
    x.enabled[0] = x.enabled[1] = 0; x.checked[0] = x.checked[1] = 1;
    mms::sdk::RefreshTicket ticket;
    CHECK(service.RefreshSubtreeElement(group, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT, ticket) == MMS_OK);
    x.queue.Drain(); MmsUiRefreshStateV1 state{}; CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == 13);
    CHECK(x.stateReads[0] == reads[0] + 1 && x.stateReads[1] == reads[1] + 1 && x.stateReads[2] == reads[2]);
    CHECK(IsWindow(firstWindow) && IsWindow(secondWindow) && !IsWindowEnabled(firstWindow) && !IsWindowEnabled(secondWindow));
    CHECK(ticket.Release() == MMS_OK);
    const auto before = x.stateReads;
    CHECK(service.RepaintSubtree(target.target, ticket) == MMS_OK); x.queue.Drain(); CHECK(ticket.Query(state) == MMS_OK && state.completedKinds == 1);
    CHECK(x.stateReads == before && ticket.Release() == MMS_OK);
}
} // namespace ui_refresh_subtree_checks
static void UiRefreshSubtreeChecks() {
    ui_refresh_subtree_checks::PipelineAndScopes();
    ui_refresh_subtree_checks::VersionsCancellationAndFailures();
    ui_refresh_subtree_checks::CohortTopologyAndReadOnly();
    ui_refresh_subtree_checks::DataSkipsUnboundCollapsedAndAtomicAcceptance();
    ui_refresh_subtree_checks::ForeignOwnerAndFixedGeometry();
    ui_refresh_subtree_checks::SharedSourceCapture();
    ui_refresh_subtree_checks::NativeGroupAndSdk();
}
