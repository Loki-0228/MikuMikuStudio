#include <mms/cpp/refresh.hpp>
namespace visibility_refresh_checks {
struct Fixture : state_refresh_checks::Fixture {
    MmsUiLayoutApiV1 layout = registry.LayoutApi(owner);
    std::unique_ptr<NativeUi> native;
    HWND targetWindow = nullptr, peerWindow = nullptr;
    Fixture() {
        auto target = registry.elements.at("example.refresh.button").desc;
        target.capabilities |= MMS_UI_LAYOUT; target.label = S("Reserved state target");
        CHECK(ui.updateElement(ui.context, element, &target) == MMS_OK);
        MmsCommandDescV1 neighbor{}; neighbor.info.h = Header<MmsCommandDescV1>();
        neighbor.info.id = S("example.refresh.neighbor"); neighbor.info.ownerPlugin = S("example.refresh"); neighbor.info.label = S("Neighbor");
        neighbor.getState = [](void*, MmsObjectRef, MmsCommandStateV1* out) -> MmsResult {
            *out = {}; out->h = Header<MmsCommandStateV1>(); out->enabled = out->visible = 1; return MMS_OK;
        };
        neighbor.invoke = [](void*, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { return MMS_OK; };
        MmsHandle neighborCommand = 0; CHECK(ui.registerCommand(ui.context, &neighbor, &neighborCommand) == MMS_OK);
        auto peer = registry.elements.at("example.refresh.sibling").desc; peer.label = S("Neighbor");
        peer.commandId = neighbor.info.id;
        CHECK(ui.updateElement(ui.context, second, &peer) == MMS_OK);
        Hints(MMS_HINT_PRESERVE_HIDDEN_SLOT);
        native = std::make_unique<NativeUi>(registry, parent, Log{});
        native->Refresh(); native->Layout(10, 500, 140, true);
        for (auto child = GetWindow(native->Toolbar(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t text[128]{}; GetWindowTextW(child, text, 128);
            if (!std::wcscmp(text, L"Reserved state target")) targetWindow = child;
            if (!std::wcscmp(text, L"Neighbor")) peerWindow = child;
        }
        CHECK(targetWindow && peerWindow && Target().ready);
    }
    void Hints(uint32_t flags) {
        MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.flags = flags;
        hints.preferred[0] = 100; hints.preferred[1] = 24;
        CHECK(layout.setDefaultHints(layout.context, S("example.refresh.button"), &hints) == MMS_OK);
    }
    MmsUiLayoutSnapshotV1 Snapshot() {
        MmsUiLayoutSnapshotV1 snapshot{}; snapshot.h = Header<MmsUiLayoutSnapshotV1>();
        CHECK(layout.acquireSnapshot(layout.context, &snapshot) == MMS_OK); return snapshot;
    }
    static MmsUiLayoutItemV1 Item(const MmsUiLayoutSnapshotV1& snapshot) {
        for (uint64_t n = 0; n < snapshot.items.count; ++n) {
            const auto& item = *reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(snapshot.items.data) + n * snapshot.items.stride);
            if (String(item.elementId) == "example.refresh.button") return item;
        }
        throw std::runtime_error("reserved slot item missing");
    }
    static RECT Bounds(HWND window) { RECT result{}; CHECK(GetWindowRect(window, &result)); return result; }
};
struct ShowProbe {
    Fixture& fixture;
    MmsHandle paint = 0;
    MmsResult result = MMS_E_FAILED;
    bool observed = false, replaceProject = false;
    static LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR context) {
        auto& x = *reinterpret_cast<ShowProbe*>(context);
        if (message == WM_SHOWWINDOW && !wp && !x.observed) {
            x.observed = true;
            MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
            x.result = x.fixture.api.getTarget(x.fixture.api.context, x.fixture.element, &target);
            if (!x.result) {
                MmsUiDirtyRectV1 rect{1,1,2,2}; auto request = x.fixture.Request(target, &rect, 1);
                x.result = x.fixture.api.request(x.fixture.api.context, &request, &x.paint);
            }
            if (x.replaceProject) x.fixture.queue.AdvanceProjectEpoch();
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    explicit ShowProbe(Fixture& f, bool replace = false) : fixture(f), replaceProject(replace) {
        CHECK(SetWindowSubclass(f.targetWindow, Window, 80, reinterpret_cast<DWORD_PTR>(this)));
    }
    ~ShowProbe() { RemoveWindowSubclass(fixture.targetWindow, Window, 80); if (paint) fixture.api.release(fixture.api.context, paint); }
};
static void SlotVisibilityAndLeases() {
    Fixture x; const auto initial = x.Target(); const auto old = x.Snapshot(); const auto original = Fixture::Item(old);
    const auto targetBounds = Fixture::Bounds(x.targetWindow), peerBounds = Fixture::Bounds(x.peerWindow);
    const auto uiRevision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    const auto saved = x.registry.ExportPreferences(); ShowProbe show(x);
    CHECK(original.visible && (original.effective.flags & MMS_HINT_PRESERVE_HIDDEN_SLOT));
    SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
    x.stateVisible = 0; auto request = x.RequestState();
    MmsHandle state = 0; MmsResult accepted = MMS_E_FAILED;
    std::thread worker([&] { accepted = x.api.request(x.api.context, &request, &state); }); worker.join();
    CHECK(accepted == MMS_OK); x.tickets.push_back(state);
    MmsUiDirtyRectV1 rect{1,1,2,2}; const auto local = x.Submit(x.Request(initial, &rect, 1));
    auto wholeRequest = x.Request(initial); wholeRequest.expectedGeometryRevision = 0;
    const auto whole = x.Submit(wholeRequest);
    const auto reads = x.stateReads;
    x.queue.Drain(); x.StateDone(state); x.Completed(whole);
    CHECK(x.State(local).result == MMS_E_STALE && x.stateReads == reads + 1);
    const auto hidden = x.Target();
    CHECK(hidden.ready && !hidden.visible && hidden.target.instanceGeneration == initial.target.instanceGeneration && hidden.geometryRevision != initial.geometryRevision);
    CHECK(x.State(state).geometryRevision == hidden.geometryRevision && !IsWindowVisible(x.targetWindow));
    CHECK(GetFocus() != x.targetWindow); const auto focusAfterHide = GetFocus();
    auto targetNow = Fixture::Bounds(x.targetWindow), peerNow = Fixture::Bounds(x.peerWindow);
    CHECK(EqualRect(&targetBounds, &targetNow) && EqualRect(&peerBounds, &peerNow) && IsWindowVisible(x.peerWindow));
    auto current = x.Snapshot(); const auto item = Fixture::Item(current);
    CHECK(!item.visible && !std::memcmp(item.rect, original.rect, sizeof(item.rect)) && Fixture::Item(old).visible);
    CHECK(current.uiRevision == old.uiRevision && current.geometryRevision != old.geometryRevision);
    CHECK(x.registry.Revision() == uiRevision && x.registry.PreferencesRevision() == preferences && x.registry.ExportPreferences() == saved);
    CHECK(show.observed && show.result == MMS_OK); x.queue.Drain(); CHECK(x.State(show.paint).result == MMS_E_STALE);
    // A normal layout must preserve the already published hide, without a
    // phantom geometry revision or a different slot for its sibling.
    x.native->Refresh(); CHECK(x.Target().geometryRevision == hidden.geometryRevision);
    auto relaid = x.Snapshot(); CHECK(relaid.geometryRevision == current.geometryRevision);
    x.stateVisible = 1; const auto restore = x.Submit(x.RequestState(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT));
    x.queue.Drain(); x.StateDone(restore, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT);
    const auto restored = x.Target(); CHECK(restored.visible && restored.ready && restored.geometryRevision != hidden.geometryRevision);
    targetNow = Fixture::Bounds(x.targetWindow); peerNow = Fixture::Bounds(x.peerWindow);
    CHECK(IsWindowVisible(x.targetWindow) && EqualRect(&targetBounds, &targetNow) && EqualRect(&peerBounds, &peerNow));
    CHECK(GetFocus() == focusAfterHide);
    auto fresh = x.Snapshot(); CHECK(Fixture::Item(fresh).visible && !Fixture::Item(current).visible);
    for (auto handle : {old.handle, current.handle, relaid.handle, fresh.handle}) x.layout.releaseSnapshot(x.layout.context, handle);
}
static void ParentVisibilityAndStalePolicy() {
    Fixture x; x.native->Layout(10, 500, 140, false);
    const auto hidden = x.Target(); CHECK(hidden.ready && !hidden.visible);
    x.stateVisible = 0; auto ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
    x.stateVisible = 1; ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
    CHECK(!IsWindowVisible(x.targetWindow) && !(GetWindowLongPtrW(x.targetWindow, GWL_STYLE) & WS_VISIBLE));
    CHECK(x.Target().geometryRevision == hidden.geometryRevision);
    x.native->Layout(10, 500, 140, true); CHECK(x.Target().visible && IsWindowVisible(x.targetWindow));
    // A user/default policy change invalidates the public layout. Old opt-in
    // must not allow changing native state before the new policy is applied.
    x.Hints(0); x.stateVisible = 0; x.stateEnabled = 0; x.stateChecked = 1;
    ticket = x.Submit(x.RequestState()); x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_NOT_READY);
    CHECK(IsWindowVisible(x.targetWindow) && IsWindowEnabled(x.targetWindow) && SendMessageW(x.targetWindow, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    x.stateVisible = x.stateEnabled = 1; x.stateChecked = 0; x.native->Refresh();
    x.stateVisible = 0; ticket = x.Submit(x.RequestState()); x.queue.Drain(); CHECK(x.State(ticket).result == MMS_E_UNSUPPORTED);
    CHECK(IsWindowVisible(x.targetWindow));
}
static void ProjectChangesDuringShow() {
    Fixture x; ShowProbe show(x, true);
    x.stateVisible = 0; auto ticket = x.Submit(x.RequestState()); x.queue.Drain();
    CHECK(show.observed && x.State(ticket).result == MMS_E_STALE && !x.Target().ready);
    x.queue.Drain(); CHECK(x.State(show.paint).result == MMS_E_STALE);
    x.native->Refresh(); CHECK(x.Target().ready && !x.Target().visible);
}
static void CachedReferenceOnWorker() {
    Fixture x; MmsHostV1 host{}; host.h = Header<MmsHostV1>(); host.context = &x;
    host.queryService = [](void* context, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
        auto& f = *static_cast<Fixture*>(context);
        if (String(name) != "mms.ui.refresh" || major != 1 || minor || size < sizeof(f.api) || !out) return MMS_E_ARGUMENT;
        std::memcpy(out, &f.api, sizeof(f.api)); return MMS_OK;
    };
    mms::sdk::RefreshServices services; CHECK(mms::sdk::RefreshServices::Query(host, services) == MMS_OK);
    const auto captured = x.Target(); mms::sdk::RefreshTicket ticket;
    for (auto visible : {0u, 1u}) {
        x.stateVisible = visible; MmsResult result = MMS_E_FAILED;
        std::thread worker([&] { result = services.RefreshStateCurrent(captured.target, ticket, 49); }); worker.join();
        CHECK(result == MMS_OK); x.queue.Drain(); MmsUiRefreshStateV1 state{};
        CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == MMS_UI_REFRESH_STATE);
        CHECK(bool(IsWindowVisible(x.targetWindow)) == bool(visible)); CHECK(ticket.Release() == MMS_OK);
    }
    CHECK(x.Target().geometryRevision != captured.geometryRevision);
    // The existing strict helper still rejects the original geometry, and
    // CURRENT cannot bypass an instance or project boundary.
    CHECK(services.RefreshState(captured, ticket) == MMS_E_STALE && !ticket);
    x.queue.AdvanceProjectEpoch(); CHECK(services.RefreshStateCurrent(captured.target, ticket) == MMS_E_STALE && !ticket);
}
struct FocusProbe {
    Fixture& fixture;
    unsigned enables = 0, departures = 0;
    HWND focusAtDisable = nullptr;
    bool replaceProject = false, removeTarget = false, redirectOnDisable = false;
    MmsResult recursive = MMS_OK, invoke = MMS_OK, targetRead = MMS_E_FAILED;
    bool readOnly = false;
    static LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR context) {
        auto& x = *reinterpret_cast<FocusProbe*>(context);
        if (message == WM_ENABLE) {
            ++x.enables;
            if (!wp) x.focusAtDisable = GetFocus();
            if (!wp && x.redirectOnDisable) SetFocus(x.fixture.native->Toolbar());
        }
        if (message == WM_KILLFOCUS) {
            ++x.departures;
            x.readOnly = x.fixture.registry.InCallback() && !x.fixture.registry.InvokingClient();
            MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
            x.targetRead = x.fixture.api.getTarget(x.fixture.api.context, x.fixture.element, &target);
            auto request = x.fixture.Request(target); request.kinds = MMS_UI_REFRESH_STATE;
            MmsHandle ticket = 0;
            x.recursive = x.fixture.api.request(x.fixture.api.context, &request, &ticket);
            x.invoke = x.fixture.ui.invoke(x.fixture.ui.context, S("example.refresh.command"), {}, {});
            x.fixture.queue.Drain();
            if (x.replaceProject) x.fixture.queue.AdvanceProjectEpoch();
            if (x.removeTarget) DestroyWindow(window);
        }
        return IsWindow(window) ? DefSubclassProc(window, message, wp, lp) : 0;
    }
    explicit FocusProbe(Fixture& f) : fixture(f) {
        CHECK(SetWindowSubclass(f.targetWindow, Window, 81, reinterpret_cast<DWORD_PTR>(this)));
    }
    ~FocusProbe() { if (IsWindow(fixture.targetWindow)) RemoveWindowSubclass(fixture.targetWindow, Window, 81); }
};
static void DisableFocusAndRestore() {
    Fixture x; FocusProbe focus(x); const auto original = x.Target();
    const auto uiRevision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    const auto saved = x.registry.ExportPreferences();
    const auto targetBounds = Fixture::Bounds(x.targetWindow), peerBounds = Fixture::Bounds(x.peerWindow);
    SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
    x.stateEnabled = 0;
    const auto request = x.RequestState(); MmsHandle a = 0, b = 0;
    MmsResult first = MMS_E_FAILED, second = MMS_E_FAILED;
    std::thread worker([&] {
        first = x.api.request(x.api.context, &request, &a);
        second = x.api.request(x.api.context, &request, &b);
    }); worker.join();
    CHECK(first == MMS_OK && second == MMS_OK); x.tickets.push_back(a); x.tickets.push_back(b);
    const auto cancelled = x.Submit(request); CHECK(x.api.cancel(x.api.context, cancelled) == MMS_OK);
    const auto before = x.stateReads;
    x.queue.Drain(); x.StateDone(a); x.StateDone(b);
    CHECK(x.State(cancelled).status == MMS_UI_REFRESH_CANCELLED);
    CHECK(x.stateReads == before + 1 && focus.enables == 1 && focus.departures == 1);
    if (IsWindowEnabled(x.targetWindow) || GetFocus() != x.peerWindow) {
        std::printf("REFRESH_FOCUS target=%p peer=%p toolbar=%p focus=%p atDisable=%p next=%p enabled=%d peerEnabled=%d peerVisible=%d peerStyle=%llx\n",
            x.targetWindow, x.peerWindow, x.native->Toolbar(), GetFocus(), focus.focusAtDisable,
            GetNextDlgTabItem(x.native->Toolbar(), x.targetWindow, FALSE), IsWindowEnabled(x.targetWindow),
            IsWindowEnabled(x.peerWindow), IsWindowVisible(x.peerWindow),
            static_cast<unsigned long long>(GetWindowLongPtrW(x.peerWindow, GWL_STYLE)));
        std::fflush(stdout);
    }
    CHECK(!IsWindowEnabled(x.targetWindow) && GetFocus() == x.peerWindow);
    CHECK(focus.focusAtDisable == x.peerWindow);
    CHECK(focus.readOnly && focus.targetRead == MMS_OK && focus.recursive == MMS_E_BUSY && focus.invoke == MMS_E_BUSY && !x.queue.Pending());
    auto again = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(again);
    CHECK(focus.enables == 1 && focus.departures == 1 && GetFocus() == x.peerWindow);
    x.stateEnabled = 1; auto restore = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(restore);
    CHECK(IsWindowEnabled(x.targetWindow) && focus.enables == 2 && focus.departures == 1 && GetFocus() == x.peerWindow);
    // A focused, enabled button remains focused when its state is unchanged.
    SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
    const auto same = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(same);
    CHECK(GetFocus() == x.targetWindow && focus.enables == 2 && focus.departures == 1);
    const auto now = x.Target();
    CHECK(now.geometryRevision == original.geometryRevision && now.target.instanceGeneration == original.target.instanceGeneration && now.visible == original.visible);
    auto targetNow = Fixture::Bounds(x.targetWindow), peerNow = Fixture::Bounds(x.peerWindow);
    CHECK(EqualRect(&targetBounds, &targetNow) && EqualRect(&peerBounds, &peerNow));
    CHECK(x.registry.Revision() == uiRevision && x.registry.PreferencesRevision() == preferences && x.registry.ExportPreferences() == saved && !x.invokes);
    // No eligible neighboring tab stop: retain keyboard focus in the toolbar.
    EnableWindow(x.peerWindow, FALSE); x.stateEnabled = 0;
    const auto fallback = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(fallback);
    CHECK(GetFocus() == x.native->Toolbar() && !IsWindowEnabled(x.targetWindow));
}
static void NativeFocusCallbackBoundaries() {
    {
        Fixture x; FocusProbe focus(x); SetFocus(x.peerWindow); CHECK(GetFocus() == x.peerWindow);
        x.stateEnabled = 0;
        const auto ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
        CHECK(focus.enables == 1 && !focus.departures && GetFocus() == x.peerWindow);
    }
    {
        Fixture x; FocusProbe focus(x); SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
        focus.redirectOnDisable = true; x.stateEnabled = 0;
        const auto ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
        CHECK(focus.enables == 1 && focus.departures == 1 && GetFocus() == x.native->Toolbar());
    }
    {
        Fixture x; FocusProbe focus(x); SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
        // A provider may disable its own HWND before returning the same state.
        // The host must still repair focus without a second enable notification.
        x.stateEnabled = 0;
        x.onState = [&] { EnableWindow(x.targetWindow, FALSE); };
        const auto ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
        CHECK(focus.enables == 1 && focus.departures == 1 && GetFocus() == x.peerWindow && focus.recursive == MMS_E_BUSY);
    }
    {
        Fixture x; FocusProbe focus(x); SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
        x.stateEnabled = 0;
        // An explicit provider focus choice must survive the host's pre-disable
        // handoff and the later native enable/show notifications.
        x.onState = [&] { SetFocus(x.native->Toolbar()); };
        const auto ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
        CHECK(focus.enables == 1 && focus.departures == 1 && GetFocus() == x.native->Toolbar());
    }
    {
        Fixture x; FocusProbe focus(x); SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
        focus.replaceProject = true; x.stateEnabled = 0;
        const auto ticket = x.Submit(x.RequestState()); x.queue.Drain();
        CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_STALE && !x.State(ticket).completedKinds);
        CHECK(focus.departures == 1 && !x.queue.Pending() && !x.registry.InCallback());
    }
    {
        Fixture x; FocusProbe focus(x); SetFocus(x.targetWindow); CHECK(GetFocus() == x.targetWindow);
        focus.removeTarget = true; x.stateEnabled = 0;
        const auto ticket = x.Submit(x.RequestState()); x.queue.Drain();
        CHECK(x.State(ticket).status == MMS_UI_REFRESH_FAILED && x.State(ticket).result == MMS_E_STALE && !x.State(ticket).completedKinds);
        CHECK(focus.departures == 1 && !IsWindow(x.targetWindow) && !x.queue.Pending() && !x.registry.InCallback());
    }
}
} // namespace visibility_refresh_checks
static void VisibilityRefreshChecks() {
    visibility_refresh_checks::SlotVisibilityAndLeases();
    visibility_refresh_checks::ParentVisibilityAndStalePolicy();
    visibility_refresh_checks::ProjectChangesDuringShow();
    visibility_refresh_checks::CachedReferenceOnWorker();
    visibility_refresh_checks::DisableFocusAndRestore();
    visibility_refresh_checks::NativeFocusCallbackBoundaries();
}
