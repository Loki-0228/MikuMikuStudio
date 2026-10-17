namespace state_refresh_checks {
struct Fixture : refresh_checks::Fixture {
    Fixture() {
        SendMessageW(button, BM_SETSTYLE, BS_CHECKBOX | BS_PUSHLIKE, FALSE);
        presentations[0].stateRefresh = true; queue.SyncTargets(presentations); Clean();
    }
    MmsUiRefreshRequestV1 RequestState(uint32_t kinds = MMS_UI_REFRESH_STATE) {
        auto request = Request(Target()); request.kinds = kinds; return request;
    }
    void StateDone(MmsHandle ticket, uint32_t kinds = MMS_UI_REFRESH_STATE) {
        auto value = State(ticket); CHECK(value.status == MMS_UI_REFRESH_COMPLETED && value.result == MMS_OK && value.completedKinds == kinds);
    }
};
static void StateAndCoalescing() {
    Fixture x; auto target = x.Target(); CHECK(target.supportedKinds == (MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE));
    CHECK(x.Target(x.panelElement).supportedKinds == MMS_UI_REFRESH_REPAINT);
    const auto revision = x.registry.Revision(), geometry = target.geometryRevision;
    x.stateEnabled = 0; x.stateChecked = 1;
    auto request = x.RequestState(); MmsHandle a = 0, b = 0, cancelled = 0;
    MmsResult first = MMS_E_FAILED, second = MMS_E_FAILED;
    std::thread worker([&] { first = x.api.request(x.api.context, &request, &a); second = x.api.request(x.api.context, &request, &b); }); worker.join();
    CHECK(first == MMS_OK && second == MMS_OK); x.tickets.push_back(a); x.tickets.push_back(b);
    cancelled = x.Submit(request); CHECK(x.api.cancel(x.api.context, cancelled) == MMS_OK);
    CHECK(x.stateReads == 0 && IsWindowEnabled(x.button) && SendMessageW(x.button, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    x.queue.Drain(); x.StateDone(a); x.StateDone(b); CHECK(x.State(cancelled).status == MMS_UI_REFRESH_CANCELLED);
    CHECK(x.stateReads == 1 && !IsWindowEnabled(x.button) && SendMessageW(x.button, BM_GETCHECK, 0, 0) == BST_CHECKED);
    CHECK(x.registry.Revision() == revision && x.Target().geometryRevision == geometry && x.invokes == 0 && x.creates == 1 && x.destroys == 0);
    x.Clean(); auto idempotent = x.Submit(request); x.queue.Drain(); x.StateDone(idempotent);
    CHECK(x.stateReads == 2 && refresh_checks::Empty(x.button));
    // A failed STATE contribution cannot absorb a separate pure REPAINT.
    x.stateResult = MMS_E_FAILED; auto failure = x.Submit(request);
    MmsUiDirtyRectV1 rect{3,4,7,8}; auto paint = x.Submit(x.Request(x.Target(), &rect, 1));
    x.queue.Drain(); CHECK(x.State(failure).status == MMS_UI_REFRESH_FAILED && !x.State(failure).completedKinds);
    x.Completed(paint); refresh_checks::Region(x.button, {{3,4,10,12}}); x.Clean();
    x.stateResult = MMS_OK; x.stateEnabled = 1; x.stateChecked = 0;
    auto combined = x.RequestState(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT);
    combined.dirtyRects = {&rect, 1, sizeof(rect), 1}; auto ticket = x.Submit(combined);
    x.queue.Drain(); x.StateDone(ticket, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT);
    CHECK(IsWindowEnabled(x.button) && SendMessageW(x.button, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    CHECK(x.registry.Revision() == revision && x.Target().geometryRevision == geometry);
}
static void VisibilityAndFailures() {
    Fixture x; const auto before = x.Target(); auto request = x.RequestState(); MmsHandle bad = 0;
    MmsUiDirtyRectV1 rect{1,1,3,3}; request.dirtyRects = {&rect, 1, sizeof(rect), 1};
    CHECK(x.api.request(x.api.context, &request, &bad) == MMS_E_ARGUMENT && bad == 0);
    request = x.RequestState(); request.target = x.Target(x.panelElement).target;
    CHECK(x.api.request(x.api.context, &request, &bad) == MMS_E_UNSUPPORTED && bad == 0);
    request = x.RequestState(); x.stateEnabled = 0; x.stateChecked = 1; x.stateVisible = 0;
    auto ticket = x.Submit(request); x.queue.Drain();
    CHECK(x.State(ticket).result == MMS_E_UNSUPPORTED && !x.State(ticket).completedKinds);
    CHECK(IsWindowEnabled(x.button) && SendMessageW(x.button, BM_GETCHECK, 0, 0) == BST_UNCHECKED && IsWindowVisible(x.button));
    CHECK(x.Target().geometryRevision == before.geometryRevision && x.Target().visible == before.visible);
    x.stateVisible = 2; ticket = x.Submit(request); x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_FAILED);
    CHECK(IsWindowEnabled(x.button));
    x.stateVisible = 1; x.onState = [] { throw std::runtime_error("state provider failure"); };
    ticket = x.Submit(request); x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_FAILED && !x.registry.InCallback());
    x.onState = {}; ticket = x.Submit(request); x.queue.Drain(); x.StateDone(ticket);
    CHECK(!IsWindowEnabled(x.button));
}
static void CallbackBoundaries() {
    Fixture x; auto request = x.RequestState(); unsigned nested = 0;
    MmsResult recursive = MMS_OK, invoke = MMS_OK, remove = MMS_OK, readTarget = MMS_E_FAILED; bool classified = false;
    x.onState = [&] {
        ++nested; classified = x.registry.InCallback() && x.registry.InvokingClient() == nullptr;
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        readTarget = x.api.getTarget(x.api.context, x.element, &target);
        MmsHandle forbidden = 0; recursive = x.api.request(x.api.context, &request, &forbidden);
        invoke = x.ui.invoke(x.ui.context, S("example.refresh.command"), {}, {});
        remove = x.ui.unregisterItem(x.ui.context, x.second);
        x.queue.Drain();
    };
    auto ticket = x.Submit(request); x.queue.Drain(); x.StateDone(ticket);
    CHECK(nested == 1 && classified && readTarget == MMS_OK && recursive == MMS_E_BUSY && invoke == MMS_E_BUSY && remove == MMS_E_BUSY && !x.queue.Pending());
    // A callback may pump a host scene replacement. Already selected groups
    // must be validated again, including separate REPAINT contributions.
    x.onState = [&] { x.queue.AdvanceProjectEpoch(); };
    auto stale = x.Submit(request); auto paint = x.Submit(x.Request(x.Target()));
    x.Clean(); x.queue.Drain();
    CHECK(x.State(stale).result == MMS_E_STALE && x.State(paint).result == MMS_E_STALE && refresh_checks::Empty(x.button));
    request = x.RequestState();
    x.onState = [&] { ++x.presentations[0].x; x.queue.SyncTargets(x.presentations); };
    ticket = x.Submit(request); x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_STALE);
    request = x.RequestState(); x.onState = [&] { x.queue.SuspendTargets(); };
    ticket = x.Submit(request); x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_NOT_READY);
    x.onState = {}; x.queue.SyncTargets(x.presentations); request = x.RequestState();
    ticket = x.Submit(request); x.queue.Retire(x.client); x.queue.Drain();
    CHECK(x.State(ticket).status == MMS_UI_REFRESH_CANCELLED);
}
static void DeferredNativeLayout() {
    Fixture x;
    // Use the production adapter for this callback-pump regression. Its normal
    // refresh queries getState too, so an unguarded nested call would recurse.
    NativeUi native(x.registry, x.parent, {});
    native.Refresh(); native.Layout(10, 500, 150, true); x.Clean();
    const auto before = x.stateReads;
    const auto revision = x.registry.Revision();
    bool entered = false;
    x.onState = [&] {
        if (entered) throw std::runtime_error("recursive native state query");
        entered = true; native.Refresh(); native.Layout(20, 420, 130, true);
    };
    // The adapter replaced the fixture's mirror with its real host buttons.
    auto request = x.RequestState(); auto ticket = x.Submit(request); x.queue.Drain(); x.StateDone(ticket);
    CHECK(entered && x.stateReads == before + 1 && x.registry.Revision() == revision);
    x.onState = {}; native.Refresh(); CHECK(x.Target().ready);
}
} // namespace state_refresh_checks
static void StateRefreshChecks() {
    state_refresh_checks::StateAndCoalescing();
    state_refresh_checks::VisibilityAndFailures();
    state_refresh_checks::CallbackBoundaries();
    state_refresh_checks::DeferredNativeLayout();
}
