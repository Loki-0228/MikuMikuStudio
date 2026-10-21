// Include after data_refresh_checks.inc and state_refresh_checks.inc.
// Wire UiRefreshSubscriptionChecks() into the focused subscription CLI and
// ordinary ABI contract suite. These probes touch only fixture-owned HWNDs.
#include <mms/refresh_subscription.h>

namespace ui_refresh_subscription_checks {
struct Fixture : data_refresh_checks::Fixture {
    MmsUiRefreshSubscriptionApiV1 subscriptions = queue.Bindings().SubscriptionApi(client);
    MmsHandle subscription = 0;
    unsigned layouts = 0;
    Fixture() {
        presentations[0].layoutRefresh = true;
        queue.SetLayout([this](MmsHandle element) -> MmsResult {
            CHECK(element == this->element && registry.InCallback() && queue.ReadOnlyCause(client));
            ++layouts; stages.push_back(3); queue.SyncTargets(presentations); return MMS_OK;
        });
        queue.SyncTargets(presentations); Clean();
    }
    MmsUiRefreshSubscriptionDescV1 Desc(uint32_t kinds = MMS_UI_REFRESH_DATA) {
        return {Header<MmsUiRefreshSubscriptionDescV1>(), binding, kinds, 0, 0};
    }
    void Subscribe(uint32_t kinds = MMS_UI_REFRESH_DATA) {
        const auto desc = Desc(kinds);
        CHECK(subscriptions.subscribe(subscriptions.context, &desc, &subscription) == MMS_OK && subscription);
    }
    MmsUiRefreshSubscriptionStateV1 Status() {
        MmsUiRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshSubscriptionStateV1>();
        CHECK(subscriptions.query(subscriptions.context, subscription, &state) == MMS_OK); return state;
    }
    void WorkerPublish(int value) {
        MmsUiDataSourcePublishV1 p{Header<MmsUiDataSourcePublishV1>(), source, sourceState.revision, {&value, sizeof(value)}, 0, 0};
        MmsUiDataSourceStateV1 next{}; next.h = Header<MmsUiDataSourceStateV1>(); MmsResult result = MMS_E_FAILED;
        std::thread worker([&] { result = data.publishSource(data.context, &p, &next); }); worker.join();
        CHECK(result == MMS_OK); sourceState = next;
    }
    void Stale() {
        MmsUiRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshSubscriptionStateV1>(); state.latestRevision = 919;
        const auto before = state;
        CHECK(subscriptions.query(subscriptions.context, subscription, &state) == MMS_E_STALE);
        CHECK(!std::memcmp(&state, &before, sizeof(state)));
    }
};
struct HostPump {
    enum : UINT { message = WM_APP + 0x6e2 };
    Fixture& fixture; unsigned dispatches = 0;
    static LRESULT CALLBACK Window(HWND window, UINT messageId, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& host = *reinterpret_cast<HostPump*>(data);
        if (messageId == message) { ++host.dispatches; host.fixture.queue.Drain(); return 0; }
        return DefSubclassProc(window, messageId, wp, lp);
    }
    explicit HostPump(Fixture& x) : fixture(x) {
        CHECK(SetWindowSubclass(fixture.parent, Window, 0x6e210, reinterpret_cast<DWORD_PTR>(this))); InstallWake();
    }
    void InstallWake() {
        fixture.queue.SetWake([this] {
            fixture.wakes.fetch_add(1);
            return PostMessageW(fixture.parent, message, 0, 0) != FALSE;
        });
    }
    ~HostPump() {
        fixture.queue.SetWake([value = &fixture] { value->wakes.fetch_add(1); return true; });
        RemoveWindowSubclass(fixture.parent, Window, 0x6e210);
    }
    void Dispatch() {
        MSG messageValue{};
        while (PeekMessageW(&messageValue, fixture.parent, message, message, PM_REMOVE)) DispatchMessageW(&messageValue);
    }
    void Wait(const std::function<bool()>& done) {
        const auto started = GetTickCount();
        while (!done()) {
            Dispatch(); CHECK(GetTickCount() - started < 5000);
            if (!done()) Sleep(1);
        }
    }
};
struct RetainedBudgetFixture : Fixture {
    unsigned aCalls = 0, bCalls = 0;
    std::array<unsigned char, 4096> aDisplay{}, bDisplay{};
    MmsHandle sourceB = 0, bindingB = 0, heldB = 0, edgeB = 0;
    HMENU menu = nullptr;
    MmsResult bResult = MMS_E_FAILED;
    explicit RetainedBudgetFixture(bool keepDisplayB = false) {
        CHECK(data.unbind(data.context, binding) == MMS_OK);
        MmsUiDataBindingDescV1 a{Header<MmsUiDataBindingDescV1>(), Target().target, source, this,
            [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
                auto& x = *static_cast<RetainedBudgetFixture*>(p); ++x.aCalls;
                CHECK(input->sourceBytes.bytes == sizeof(int));
                std::memcpy(x.aDisplay.data(), input->sourceBytes.data, sizeof(int));
                return x.data.writePrepared(x.data.context, input->builder, {x.aDisplay.data(), x.aDisplay.size()});
            }, aDisplay.size(), 0, 0};
        CHECK(data.bind(data.context, &a, &binding) == MMS_OK);
        MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
        CHECK(data.getCapabilities(data.context, &caps) == MMS_OK && caps.maxRetainedBytesPerPlugin == 2 * 1024 * 1024);
        const std::string large(size_t(caps.maxSourceBytes), 'b');
        MmsUiDataSourceDescV1 b{Header<MmsUiDataSourceDescV1>(), S("held-source-b"), caps.maxSourceBytes, 0, 0};
        CHECK(data.registerSource(data.context, &b, &sourceB) == MMS_OK);
        MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), sourceB, 0, {large.data(), large.size()}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        CHECK(data.publishSource(data.context, &publish, &state) == MMS_OK);
        presentations[1].stateRefresh = true; queue.SyncTargets(presentations);
        bResult = keepDisplayB ? MMS_OK : MMS_E_FAILED;
        MmsUiDataBindingDescV1 bindB{Header<MmsUiDataBindingDescV1>(), Target(second).target, sourceB, this,
            [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
                auto& x = *static_cast<RetainedBudgetFixture*>(p); ++x.bCalls;
                if (x.bResult) return x.bResult;
                return x.data.writePrepared(x.data.context, input->builder, {x.bDisplay.data(), x.bDisplay.size()});
            }, bDisplay.size(), 0, 0};
        CHECK(data.bind(data.context, &bindB, &bindingB) == MMS_OK);
        if (keepDisplayB) {
            auto request = Request(Target(second)); request.kinds = MMS_UI_REFRESH_DATA; request.binding = bindingB; request.sourceRevision = 1;
            const auto ticket = Submit(request); queue.Drain(); CHECK(State(ticket).result == MMS_OK && bCalls == 1);
            bResult = MMS_E_FAILED;
        }
        uint64_t remaining = caps.maxRetainedBytesPerPlugin - sizeof(int) - large.size() -
            (keepDisplayB ? bDisplay.size() : 0) - 16;
        for (unsigned i = 0; remaining; ++i) {
            const auto bytes = std::min(remaining, caps.maxSourceBytes);
            const auto id = std::string("held-fill-") + std::to_string(i);
            MmsUiDataSourceDescV1 desc{Header<MmsUiDataSourceDescV1>(), {id.data(), id.size()}, bytes, 0, 0};
            MmsHandle handle = 0; CHECK(data.registerSource(data.context, &desc, &handle) == MMS_OK);
            const std::string payload(size_t(bytes), 'f');
            publish.source = handle; publish.bytes = {payload.data(), payload.size()};
            CHECK(data.publishSource(data.context, &publish, &state) == MMS_OK); remaining -= bytes;
        }
    }
    ~RetainedBudgetFixture() { if (menu) DestroyMenu(menu); }
    void HoldOldB() {
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = S("example.refresh.hold-menu");
        element.ownerPlugin = S("example.refresh"); element.parentId = S("mms.menu.plugins"); element.commandId = S("example.refresh.command");
        element.label = S("Hold old source"); element.kind = MMS_UI_MENU_ITEM; element.capabilities = MMS_UI_HIDE;
        MmsHandle handle = 0; CHECK(ui.registerElement(ui.context, &element, &handle) == MMS_OK);
        menu = CreatePopupMenu(); CHECK(menu && AppendMenuW(menu, MF_STRING, 103, L"Hold old source"));
        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_DATA; item.dwItemData = 323;
        CHECK(SetMenuItemInfoW(menu, 103, FALSE, &item));
        RefreshPresentation presentation; presentation.element = handle; presentation.owner = client.owner; presentation.instance = 39;
        presentation.kind = MMS_UI_MENU_ITEM; presentation.stateRefresh = true; presentation.menu = menu;
        presentation.menuItem = 103; presentation.menuCookie = 323;
        queue.SyncMenuTargets({presentation}); queue.SetMenuTracking(true);
        auto dependencies = queue.DependencyApi(client);
        MmsUiRefreshDependencyDescV1 edge{Header<MmsUiRefreshDependencyDescV1>(), Target().target, Target(second).target, 0, 0};
        CHECK(dependencies.registerDependency(dependencies.context, &edge, &edgeB) == MMS_OK);
        edge.dependent = Target(handle).target; MmsHandle menuEdge = 0;
        CHECK(dependencies.registerDependency(dependencies.context, &edge, &menuEdge) == MMS_OK);
        auto waiting = Request(Target()); waiting.expectedGeometryRevision = 0;
        waiting.kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE; waiting.scope = MMS_UI_REFRESH_DEPENDENTS;
        heldB = Submit(waiting); CHECK(!queue.Pending() && !bCalls);
        const int small = 9;
        MmsUiDataSourcePublishV1 update{Header<MmsUiDataSourcePublishV1>(), sourceB, 1, {&small, sizeof(small)}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        const auto before = wakes.load(); CHECK(data.publishSource(data.context, &update, &state) == MMS_OK && state.revision == 2);
        CHECK(wakes.load() == before && !queue.Pending()); // The explicit cohort still pins the old large snapshot.
    }
    void Park(HostPump& host) {
        Subscribe(); host.Wait([&] { return aCalls == 1 && !queue.Pending(); });
        const auto state = Status(); CHECK(state.lastResult == MMS_E_BUSY && state.pending && !state.completedRevision && state.latestRevision == 1);
    }
    void Recovered(HostPump& host) {
        host.Wait([&] { return Status().completedRevision == 1 && !Status().pending; });
        CHECK(aCalls == 2 && !queue.Pending() && Status().latestRevision == 1);
        MmsUiDataDisplayV1 display{}; display.h = Header<MmsUiDataDisplayV1>();
        CHECK(data.acquireDisplay(data.context, binding, &display) == MMS_OK && display.sourceRevision == 1 && display.bytes.bytes == aDisplay.size());
        int value = 0; std::memcpy(&value, display.bytes.data, sizeof(value)); CHECK(value == 12);
        CHECK(data.releaseDisplay(data.context, display.lease) == MMS_OK);
    }
};
static void WorkerPipelineAndBursts() {
    Fixture x;
    const auto revision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    x.onState = [&] { x.stages.push_back(2); CHECK(x.Display() == x.sampled * 2); };
    const auto kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT;
    x.Subscribe(kinds); auto initial = x.Status();
    CHECK(initial.latestRevision == 1 && !initial.submittedRevision && initial.pending && x.queue.Pending());
    CHECK(x.prepares == 0 && x.stateReads == 0 && x.layouts == 0 && refresh_checks::Empty(x.button));
    x.queue.Drain(); auto state = x.Status();
    CHECK(state.latestRevision == 1 && state.submittedRevision == 1 && state.completedRevision == 1 && !state.pending && state.lastResult == MMS_OK);
    CHECK(x.stages == std::vector<unsigned>({1,2,3}) && x.Display() == 24 && !refresh_checks::Empty(x.button));
    CHECK(!x.queue.Pending()); x.Clean(); x.stages.clear();
    const auto wakes = x.wakes.load(), reads = x.stateReads;
    x.WorkerPublish(12);
    CHECK(x.sourceState.revision == 1 && x.wakes.load() == wakes && !x.queue.Pending());
    x.queue.Drain(); CHECK(x.prepares == 1 && x.stateReads == reads && x.stages.empty() && refresh_checks::Empty(x.button));
    x.WorkerPublish(19);
    CHECK(x.queue.Pending() && x.prepares == 1 && x.stateReads == reads && refresh_checks::Empty(x.button));
    MmsUiRefreshSubscriptionStateV1 workerState{}; workerState.h = Header<MmsUiRefreshSubscriptionStateV1>(); MmsResult query = MMS_E_FAILED;
    std::thread metadata([&] { query = x.subscriptions.query(x.subscriptions.context, x.subscription, &workerState); }); metadata.join();
    CHECK(query == MMS_OK && workerState.latestRevision == 2 && workerState.completedRevision == 1 && workerState.pending);
    x.queue.Drain(); CHECK(x.Display() == 38 && x.prepares == 2 && x.stages == std::vector<unsigned>({1,2,3}));
    x.Clean(); x.stages.clear();
    // No request API is used between these publications and the UI drain.
    for (int i = 0; i != 400; ++i) x.WorkerPublish(100 + i);
    CHECK(x.prepares == 2 && x.Status().latestRevision == 402 && x.Status().submittedRevision == 2);
    x.queue.Drain(); CHECK(x.prepares == 3 && x.Display() == 998 && x.Status().completedRevision == 402 && !x.queue.Pending());
    // Internal tickets are host-owned and released at completion. Repeated
    // refreshes must not exhaust the ordinary 256-ticket allowance.
    for (int i = 0; i != 270; ++i) { x.Publish(700 + i); x.queue.Drain(); CHECK(!x.Status().pending && !x.queue.Pending()); }
    CHECK(x.registry.Revision() == revision && x.registry.PreferencesRevision() == preferences && x.invokes == 0);
}
static void ExactAcceptedCohorts() {
    Fixture x; const auto explicitOld = x.Submit(x.DataRequest()); x.Subscribe(); x.WorkerPublish(20);
    x.queue.Drain();
    CHECK(x.State(explicitOld).sourceRevision == 1 && x.State(explicitOld).status == MMS_UI_REFRESH_COMPLETED);
    CHECK(x.prepares == 2 && x.sampled == 20 && x.Display() == 40 && x.Status().completedRevision == 2);
    x.Publish(30);
    bool published = false;
    x.onPrepare = [&]() -> MmsResult {
        if (!published) { published = true; x.WorkerPublish(40); }
        return MMS_OK;
    };
    x.queue.Drain(); auto flight = x.Status();
    CHECK(published && x.sampled == 30 && x.Display() == 60);
    CHECK(flight.latestRevision == 4 && flight.submittedRevision == 3 && flight.completedRevision == 3 && flight.pending && x.queue.Pending());
    x.queue.Drain(); CHECK(x.sampled == 40 && x.Display() == 80 && x.Status().completedRevision == 4 && !x.Status().pending);
}
static void PhaseCombinationsAndReadOnly() {
    for (uint32_t extra = 0; extra != 8; ++extra) {
        Fixture x;
        const uint32_t kinds = MMS_UI_REFRESH_DATA | ((extra & 1) ? MMS_UI_REFRESH_STATE : 0) |
            ((extra & 2) ? MMS_UI_REFRESH_LAYOUT : 0) | ((extra & 4) ? MMS_UI_REFRESH_REPAINT : 0);
        x.onState = [&] { x.stages.push_back(2); CHECK(x.Display() == 24); };
        x.Subscribe(kinds); x.queue.Drain();
        std::vector<unsigned> expected{1}; if (extra & 1) expected.push_back(2); if (extra & 2) expected.push_back(3);
        CHECK(x.stages == expected && x.Status().completedRevision == 1 && !x.Status().pending);
        CHECK(x.layouts == ((extra & 2) ? 1u : 0u));
        if (!(extra & 4)) CHECK(refresh_checks::Empty(x.button));
    }
    Fixture x; x.Subscribe();
    x.onPrepare = [&]() -> MmsResult {
        auto desc = x.Desc(); MmsHandle untouched = 878;
        CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &untouched) == MMS_E_BUSY && untouched == 878);
        CHECK(x.subscriptions.unsubscribe(x.subscriptions.context, x.subscription) == MMS_E_BUSY);
        MmsUiRefreshSubscriptionCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshSubscriptionCapabilitiesV1>(); caps.flags = 77;
        CHECK(x.subscriptions.getCapabilities(x.subscriptions.context, &caps) == MMS_E_BUSY && caps.flags == 77);
        const int value = 90;
        auto p = MmsUiDataSourcePublishV1{Header<MmsUiDataSourcePublishV1>(), x.source, x.sourceState.revision, {&value, sizeof(value)}, 0, 0};
        MmsUiDataSourceStateV1 unchanged{}; unchanged.h = Header<MmsUiDataSourceStateV1>(); unchanged.revision = 79;
        CHECK(x.data.publishSource(x.data.context, &p, &unchanged) == MMS_E_BUSY && unchanged.revision == 79);
        const auto reads = x.prepares; x.queue.Drain(); CHECK(x.prepares == reads);
        return MMS_OK;
    };
    x.queue.Drain(); CHECK(x.prepares == 1 && !x.Status().pending && !x.registry.InCallback());
}
static void WaitingForFirstPublicationAndValidation() {
    Fixture x; CHECK(x.data.unbind(x.data.context, x.binding) == MMS_OK);
    CHECK(x.data.unregisterSource(x.data.context, x.source) == MMS_OK);
    MmsUiDataSourceDescV1 sourceDesc{Header<MmsUiDataSourceDescV1>(), S("unpublished"), 4096, 0, 0};
    CHECK(x.data.registerSource(x.data.context, &sourceDesc, &x.source) == MMS_OK);
    MmsUiDataBindingDescV1 binding{Header<MmsUiDataBindingDescV1>(), x.Target().target, x.source, nullptr, nullptr, 4096, 0, 0};
    CHECK(x.data.bind(x.data.context, &binding, &x.binding) == MMS_OK);
    MmsUiRefreshSubscriptionCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshSubscriptionCapabilitiesV1>();
    CHECK(x.subscriptions.getCapabilities(x.subscriptions.context, &caps) == MMS_OK && caps.maxSubscriptionsPerPlugin == 128 && caps.supportedKinds == 15 && caps.supportedScopes == MMS_UI_REFRESH_SELF);
    CHECK(!caps.flags && !caps.reserved[0] && !caps.reserved[1]);
    auto desc = x.Desc(); MmsHandle unchanged = 556;
    desc.flags = 1; CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_ARGUMENT && unchanged == 556);
    desc = x.Desc(); desc.reserved = 1; CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_ARGUMENT && unchanged == 556);
    desc = x.Desc(MMS_UI_REFRESH_REPAINT); CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_ARGUMENT && unchanged == 556);
    desc = x.Desc(); desc.kinds |= 16; CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_ARGUMENT && unchanged == 556);
    x.Subscribe(); CHECK(!x.Status().latestRevision && !x.Status().pending && !x.queue.Pending());
    x.queue.Drain(); CHECK(!x.Status().submittedRevision);
    desc = x.Desc(); CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_CONFLICT && unchanged == 556);
    auto other = x.queue.Bindings().SubscriptionApi(x.other);
    CHECK(other.subscribe(other.context, &desc, &unchanged) == MMS_E_ARGUMENT && unchanged == 556);
    CHECK(other.unsubscribe(other.context, x.subscription) == MMS_E_ARGUMENT);
    MmsUiRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshSubscriptionStateV1>(); state.latestRevision = 887;
    CHECK(other.query(other.context, x.subscription, &state) == MMS_E_ARGUMENT && state.latestRevision == 887);
    MmsResult add = MMS_OK, remove = MMS_OK, capabilities = MMS_OK;
    std::thread worker([&] {
        add = x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged);
        remove = x.subscriptions.unsubscribe(x.subscriptions.context, x.subscription);
        capabilities = x.subscriptions.getCapabilities(x.subscriptions.context, &caps);
    }); worker.join();
    CHECK(add == MMS_E_THREAD && remove == MMS_E_THREAD && capabilities == MMS_E_THREAD && unchanged == 556);
    const int value = 72; MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), x.source, 0, {&value, sizeof(value)}, 0, 0};
    MmsUiDataSourceStateV1 next{}; next.h = Header<MmsUiDataSourceStateV1>();
    std::thread first([&] { add = x.data.publishSource(x.data.context, &publish, &next); }); first.join();
    CHECK(add == MMS_OK && next.revision == 1 && x.queue.Pending()); x.queue.Drain();
    CHECK(x.Status().completedRevision == 1 && x.Display() == value && !x.queue.Pending());
}
static void CancelAndLifecycle() {
    for (unsigned mode = 0; mode != 6; ++mode) {
        Fixture x; x.Subscribe(); x.queue.Drain(); CHECK(x.Status().completedRevision == 1);
        MmsUiDataDisplayV1 hold{}; hold.h = Header<MmsUiDataDisplayV1>();
        CHECK(x.data.acquireDisplay(x.data.context, x.binding, &hold) == MMS_OK);
        x.Publish(31); const auto prepares = x.prepares;
        if (mode == 0) CHECK(x.subscriptions.unsubscribe(x.subscriptions.context, x.subscription) == MMS_OK);
        else if (mode == 1) CHECK(x.data.unbind(x.data.context, x.binding) == MMS_OK);
        else if (mode == 2) CHECK(x.data.unregisterSource(x.data.context, x.source) == MMS_OK);
        else if (mode == 3) { ++x.presentations[0].instance; x.queue.SyncTargets(x.presentations); }
        else if (mode == 4) x.queue.AdvanceProjectEpoch();
        else x.queue.Retire(x.client);
        x.Stale(); x.queue.Drain(); CHECK(x.prepares == prepares && !x.queue.Pending());
        int value = 0; std::memcpy(&value, hold.bytes.data, sizeof(value)); CHECK(value == 24);
        MmsResult released = MMS_E_FAILED;
        std::thread worker([&] { released = x.data.releaseDisplay(x.data.context, hold.lease); }); worker.join();
        CHECK(released == MMS_OK);
        if (mode == 5) CHECK(!x.queue.HasLeases(x.client));
    }
    // Retire during DATA has to invalidate the accepted internal ticket and
    // reject its prepared model before calling subsequent STATE or LAYOUT.
    Fixture x; x.Subscribe(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT);
    x.onPrepare = [&]() -> MmsResult { x.queue.Retire(x.client); return MMS_OK; };
    x.queue.Drain(); CHECK(x.prepares == 1 && !x.stateReads && !x.layouts && !x.queue.Pending()); x.Stale();
    for (unsigned mode = 0; mode != 6; ++mode) {
        Fixture pending;
        // Leave the subscription's accepted ticket behind the 64-contribution
        // drain budget. Cancellation must remove it, not merely its mailbox.
        auto repaint = pending.Request(pending.Target());
        for (unsigned i = 0; i != 64; ++i) pending.Submit(repaint);
        pending.Subscribe(); pending.queue.Drain();
        const auto state = pending.Status();
        CHECK(state.submittedRevision == 1 && !state.completedRevision && state.pending && pending.prepares == 0);
        if (mode == 0) CHECK(pending.subscriptions.unsubscribe(pending.subscriptions.context, pending.subscription) == MMS_OK);
        else if (mode == 1) CHECK(pending.data.unbind(pending.data.context, pending.binding) == MMS_OK);
        else if (mode == 2) CHECK(pending.data.unregisterSource(pending.data.context, pending.source) == MMS_OK);
        else if (mode == 3) { ++pending.presentations[0].instance; pending.queue.SyncTargets(pending.presentations); }
        else if (mode == 4) pending.queue.AdvanceProjectEpoch();
        else pending.queue.Retire(pending.client);
        pending.Stale(); pending.queue.Drain(); CHECK(!pending.prepares && !pending.queue.Pending());
        if (mode == 5) {
            for (auto handle : pending.tickets) CHECK(pending.api.release(pending.api.context, handle) == MMS_OK);
            pending.tickets.clear(); CHECK(!pending.queue.HasLeases(pending.client));
        }
    }
}
static void CapacityAndReadinessDoNotSpin() {
    Fixture x; auto repaint = x.Request(x.Target());
    MmsUiRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshCapabilitiesV1>(); CHECK(x.api.getCapabilities(x.api.context, &caps) == MMS_OK);
    for (unsigned i = 0; i != caps.maxTicketsPerPlugin; ++i) x.Submit(repaint);
    for (unsigned i = 0; i < caps.maxTicketsPerPlugin; i += caps.maxContributionsPerDrain) x.queue.Drain();
    CHECK(!x.queue.Pending());
    x.Subscribe(); x.queue.Drain();
    auto busy = x.Status(); CHECK(busy.lastResult == MMS_E_BUSY && busy.pending && !busy.submittedRevision && !x.queue.Pending());
    const auto wakes = x.wakes.load();
    for (unsigned i = 0; i != 8; ++i) x.queue.Drain();
    CHECK(x.prepares == 0 && x.wakes.load() == wakes && !x.queue.Pending());
    const auto released = x.tickets.back(); x.tickets.pop_back();
    CHECK(x.api.release(x.api.context, released) == MMS_OK && x.queue.Pending());
    x.queue.Drain(); CHECK(x.Status().completedRevision == 1 && x.prepares == 1 && !x.Status().pending);
    x.Publish(16); x.queue.SuspendTargets(); x.queue.Drain();
    CHECK(x.Status().lastResult == MMS_E_NOT_READY && x.Status().pending && !x.queue.Pending());
    const auto reads = x.prepares;
    for (unsigned i = 0; i != 8; ++i) x.queue.Drain();
    CHECK(x.prepares == reads && !x.queue.Pending());
    x.queue.SyncTargets(x.presentations); CHECK(x.queue.Pending()); x.queue.Drain();
    CHECK(x.prepares == reads + 1 && x.Status().completedRevision == 2 && !x.Status().pending);
    // A BUSY prepare is also parked. Finishing its own ticket must not rearm
    // it, otherwise the manager's retry timer would spin forever.
    x.Publish(22); x.onPrepare = [] { return MMS_E_BUSY; }; x.queue.Drain();
    CHECK(x.Status().lastResult == MMS_E_BUSY && x.Status().pending && !x.queue.Pending());
    const auto failedPrepares = x.prepares;
    for (unsigned i = 0; i != 8; ++i) x.queue.Drain();
    CHECK(x.prepares == failedPrepares && !x.queue.Pending());
    x.onPrepare = {}; x.Publish(23); x.queue.Drain();
    CHECK(x.Status().completedRevision == 4 && x.Display() == 46 && !x.Status().pending);
}
static void MultipleConsumerFailuresDoNotRearmEachOther() {
    const MmsResult failures[]{MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_MEMORY};
    for (const auto failure : failures) for (unsigned candidate = 0; candidate != 2; ++candidate) {
        Fixture x;
        struct Context { Fixture* fixture; MmsResult failure; bool candidate; unsigned calls = 0; } context{&x, failure, candidate != 0};
        MmsUiDataBindingDescV1 binding{Header<MmsUiDataBindingDescV1>(), x.Target(x.second).target, x.source, &context,
            [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
                auto& c = *static_cast<Context*>(p); ++c.calls;
                if (c.candidate || !c.failure) {
                    const auto written = c.fixture->data.writePrepared(c.fixture->data.context, input->builder, input->sourceBytes);
                    if (written) return written;
                }
                return c.failure;
            }, 4096, 0, 0};
        MmsHandle otherBinding = 0, otherSubscription = 0;
        CHECK(x.data.bind(x.data.context, &binding, &otherBinding) == MMS_OK);
        x.onPrepare = [&]() -> MmsResult {
            if (candidate) {
                const int value = 41;
                CHECK(x.data.writePrepared(x.data.context, x.lastBuilder, {&value, sizeof(value)}) == MMS_OK);
            }
            return failure;
        };
        x.Subscribe(); auto desc = x.Desc(); desc.binding = otherBinding;
        CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &otherSubscription) == MMS_OK);
        x.queue.Drain(); CHECK(x.prepares == 1 && context.calls == 1 && !x.queue.Pending());
        const auto state = x.Status(); CHECK(state.pending && state.lastResult == failure && !state.completedRevision);
        // Generic ticket capacity and target publication are not evidence
        // that a provider's own business failure can now succeed.
        auto ticket = x.Submit(x.Request(x.Target())); x.queue.Drain();
        CHECK(x.api.release(x.api.context, ticket) == MMS_OK); x.tickets.pop_back();
        x.queue.SyncTargets(x.presentations);
        const auto parkedWakes = x.wakes.load();
        for (unsigned i = 0; i != 16; ++i) x.queue.Drain();
        CHECK(x.prepares == 1 && context.calls == 1 && !x.queue.Pending() && x.wakes.load() == parkedWakes);
        x.onPrepare = {}; context.failure = MMS_OK; x.Publish(37); x.queue.Drain();
        CHECK(x.prepares == 2 && context.calls == 2 && x.Status().completedRevision == 2 && !x.queue.Pending());
    }
}
static void DestroyedTargetRetiresSubscription() {
    Fixture x; x.Subscribe(); x.queue.Drain(); CHECK(x.Status().completedRevision == 1);
    const auto prepares = x.prepares;
    CHECK(DestroyWindow(x.button)); x.button = nullptr;
    x.WorkerPublish(52); x.queue.Drain(); x.Stale(); CHECK(!x.queue.Pending() && x.prepares == prepares);
    const auto wakes = x.wakes.load();
    x.WorkerPublish(53); CHECK(x.sourceState.revision == 3 && x.wakes.load() == wakes && !x.queue.Pending());
    x.queue.Drain(); CHECK(x.prepares == prepares);
    MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
    CHECK(x.data.getSource(x.data.context, x.source, &state) == MMS_OK && state.revision == 3);
    MmsUiDataDisplayV1 display{}; display.h = Header<MmsUiDataDisplayV1>();
    CHECK(x.data.acquireDisplay(x.data.context, x.binding, &display) == MMS_E_STALE);
}
static void IndependentInvalidationSignalsFreedCapacity() {
    for (unsigned topology = 0; topology != 2; ++topology) {
        Fixture x;
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>();
        element.id = S("example.refresh.subscription.waiting-menu"); element.ownerPlugin = S("example.refresh");
        element.parentId = S("mms.menu.plugins"); element.commandId = S("example.refresh.command");
        element.label = S("Waiting menu"); element.kind = MMS_UI_MENU; element.capabilities = MMS_UI_HIDE | MMS_UI_CONTAINER;
        MmsHandle menuElement = 0; CHECK(x.ui.registerElement(x.ui.context, &element, &menuElement) == MMS_OK);
        struct Menu { HMENU value = CreatePopupMenu(); ~Menu() { if (value) DestroyMenu(value); } } menu;
        CHECK(menu.value && AppendMenuW(menu.value, MF_STRING, 101, L"Waiting menu"));
        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_DATA; item.dwItemData = 321;
        CHECK(SetMenuItemInfoW(menu.value, 101, FALSE, &item));
        RefreshPresentation presentation; presentation.element = menuElement; presentation.owner = x.client.owner;
        presentation.instance = 29; presentation.kind = MMS_UI_MENU; presentation.stateRefresh = true;
        presentation.menu = menu.value; presentation.menuItem = 101; presentation.menuCookie = 321;
        x.queue.SyncMenuTargets({presentation}); x.queue.SetMenuTracking(true);
        auto dependencies = x.queue.DependencyApi(x.client); MmsHandle edge = 0;
        MmsUiRefreshRequestV1 waiting{}; waiting.h = Header<MmsUiRefreshRequestV1>(); waiting.kinds = MMS_UI_REFRESH_STATE;
        if (!topology) {
            const MmsUiRefreshDependencyDescV1 desc{Header<MmsUiRefreshDependencyDescV1>(), x.Target().target, x.Target(menuElement).target, 0, 0};
            CHECK(dependencies.registerDependency(dependencies.context, &desc, &edge) == MMS_OK);
            waiting.target = x.Target().target; waiting.scope = MMS_UI_REFRESH_DEPENDENTS;
        } else { waiting.target = x.Target(menuElement).target; waiting.scope = MMS_UI_REFRESH_SUBTREE; }
        MmsHandle waitingTicket = 0;
        CHECK(x.api.request(x.api.context, &waiting, &waitingTicket) == MMS_OK);
        CHECK(x.api.release(x.api.context, waitingTicket) == MMS_OK);
        MmsUiRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshCapabilitiesV1>(); CHECK(x.api.getCapabilities(x.api.context, &caps) == MMS_OK);
        const auto repaint = x.Request(x.Target());
        for (unsigned i = 1; i != caps.maxTicketsPerPlugin; ++i) x.Submit(repaint);
        for (unsigned i = 1; i < caps.maxTicketsPerPlugin; i += caps.maxContributionsPerDrain) x.queue.Drain();
        CHECK(!x.queue.Pending()); x.Subscribe(); x.queue.Drain();
        CHECK(x.Status().lastResult == MMS_E_BUSY && !x.Status().submittedRevision && !x.queue.Pending());
        const auto wakes = x.wakes.load();
        if (!topology) CHECK(dependencies.unregisterDependency(dependencies.context, edge) == MMS_OK);
        else {
            MmsUiElementV1 child = element; child.id = S("example.refresh.subscription.waiting-child");
            child.parentId = element.id; child.kind = MMS_UI_MENU_ITEM; child.capabilities = MMS_UI_HIDE;
            MmsHandle handle = 0; CHECK(x.ui.registerElement(x.ui.context, &child, &handle) == MMS_OK);
        }
        // No new source, target publication or explicit request is allowed to
        // provide this wake. The independent invalidation owns it.
        CHECK(x.wakes.load() > wakes && x.queue.Pending()); x.queue.Drain();
        CHECK(x.prepares == 1 && x.Status().completedRevision == 1 && !x.queue.Pending());
    }
}
static void WakeFailureAndTargetCapabilities() {
    Fixture x; auto desc = x.Desc(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE); MmsHandle unchanged = 663;
    x.presentations[0].stateRefresh = false; x.queue.SyncTargets(x.presentations);
    CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 663);
    desc = x.Desc(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT);
    x.presentations[0].layoutRefresh = false; x.queue.SyncTargets(x.presentations);
    CHECK(x.subscriptions.subscribe(x.subscriptions.context, &desc, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 663);
    x.queue.SetWake([&] { x.wakes.fetch_add(1); return false; });
    x.Subscribe(); const auto failed = x.Status();
    CHECK(failed.lastResult == MMS_E_FAILED && failed.pending && !failed.submittedRevision && !x.queue.Pending());
    const auto wakes = x.wakes.load();
    for (unsigned i = 0; i != 8; ++i) x.queue.Drain();
    CHECK(!x.prepares && x.wakes.load() == wakes);
    x.WorkerPublish(12); CHECK(x.wakes.load() == wakes && !x.queue.Pending());
    x.queue.SetWake([&] { x.wakes.fetch_add(1); return true; });
    x.WorkerPublish(91); CHECK(x.queue.Pending()); x.queue.Drain();
    CHECK(x.prepares == 1 && x.Display() == 182 && x.Status().completedRevision == 2 && !x.Status().pending);
}
static void UnsubscribedSourceShrinkSignalsDisplayCapacity() {
    Fixture x;
    struct Prepared {
        Fixture* fixture;
        unsigned calls = 0;
        std::array<unsigned char, 4096> bytes{};
    } prepared{&x};
    CHECK(x.data.unbind(x.data.context, x.binding) == MMS_OK);
    MmsUiDataBindingDescV1 binding{Header<MmsUiDataBindingDescV1>(), x.Target().target, x.source, &prepared,
        [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
            auto& state = *static_cast<Prepared*>(p); ++state.calls;
            CHECK(input->sourceBytes.bytes == sizeof(int));
            std::memcpy(state.bytes.data(), input->sourceBytes.data, sizeof(int));
            return state.fixture->data.writePrepared(state.fixture->data.context, input->builder,
                {state.bytes.data(), state.bytes.size()});
        }, prepared.bytes.size(), 0, 0};
    CHECK(x.data.bind(x.data.context, &binding, &x.binding) == MMS_OK);
    MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
    CHECK(x.data.getCapabilities(x.data.context, &caps) == MMS_OK && caps.maxRetainedBytesPerPlugin == 2 * 1024 * 1024);
    // Leave enough room to publish B's small replacement, but less than A's
    // 4096-byte prepared display. B has no binding and no subscription.
    uint64_t remaining = caps.maxRetainedBytesPerPlugin - sizeof(int) - 16;
    MmsHandle sourceB = 0; uint64_t sourceBBytes = 0;
    for (unsigned i = 0; remaining; ++i) {
        const auto bytes = std::min(remaining, caps.maxSourceBytes);
        const auto id = std::string("unsubscribed-capacity-") + std::to_string(i);
        MmsUiDataSourceDescV1 source{Header<MmsUiDataSourceDescV1>(), {id.data(), id.size()}, bytes, 0, 0};
        MmsHandle handle = 0; CHECK(x.data.registerSource(x.data.context, &source, &handle) == MMS_OK);
        const std::string payload(size_t(bytes), 'x');
        MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), handle, 0, {payload.data(), payload.size()}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        CHECK(x.data.publishSource(x.data.context, &publish, &state) == MMS_OK);
        if (!sourceB) { sourceB = handle; sourceBBytes = bytes; }
        remaining -= bytes;
    }
    HostPump host{x};
    x.Subscribe(); host.Wait([&] { return prepared.calls == 1 && !x.queue.Pending(); });
    const auto parked = x.Status();
    CHECK(parked.lastResult == MMS_E_BUSY && parked.pending && parked.submittedRevision == 1 && !parked.completedRevision);
    const auto wakes = x.wakes.load(), dispatches = host.dispatches;
    const auto publishB = [&](MmsBytes bytes) {
        MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), sourceB, 1, bytes, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>(); MmsResult result = MMS_E_FAILED;
        std::thread worker([&] { result = x.data.publishSource(x.data.context, &publish, &state); }); worker.join();
        CHECK(result == MMS_OK); return state;
    };
    const std::string same(size_t(sourceBBytes), 'x');
    CHECK(publishB({same.data(), same.size()}).revision == 1);
    host.Dispatch();
    CHECK(x.wakes.load() == wakes && host.dispatches == dispatches && prepared.calls == 1 && !x.queue.Pending());
    const int small = 7; CHECK(publishB({&small, sizeof(small)}).revision == 2);
    // The worker only posts a wake. A's own revision and UI callback count
    // have not changed; no explicit refresh request or manual Drain follows.
    CHECK(x.wakes.load() == wakes + 1 && prepared.calls == 1 && x.queue.Pending());
    host.Wait([&] { return x.Status().completedRevision == 1 && !x.Status().pending; });
    CHECK(host.dispatches > dispatches && prepared.calls == 2 && !x.queue.Pending());
    CHECK(x.Status().latestRevision == 1 && x.Status().submittedRevision == 1 && x.Status().lastResult == MMS_OK);
    MmsUiDataDisplayV1 display{}; display.h = Header<MmsUiDataDisplayV1>();
    CHECK(x.data.acquireDisplay(x.data.context, x.binding, &display) == MMS_OK && display.sourceRevision == 1 && display.bytes.bytes == prepared.bytes.size());
    int value = 0; std::memcpy(&value, display.bytes.data, sizeof(value)); CHECK(value == 12);
    const auto settledWakes = x.wakes.load(), settledDispatches = host.dispatches;
    CHECK(x.data.releaseDisplay(x.data.context, display.lease) == MMS_OK);
    host.Dispatch(); CHECK(x.wakes.load() == settledWakes && host.dispatches == settledDispatches);
}
static void RetiredExplicitCohortSignalsDisplayCapacity() {
    for (unsigned mode = 0; mode != 4; ++mode) {
        RetainedBudgetFixture x; x.HoldOldB(); HostPump host{x}; x.Park(host);
        const auto wakes = x.wakes.load();
        if (mode == 0) {
            MmsResult cancelled = MMS_E_FAILED;
            std::thread worker([&] { cancelled = x.api.cancel(x.api.context, x.heldB); }); worker.join();
            CHECK(cancelled == MMS_OK && x.State(x.heldB).status == MMS_UI_REFRESH_CANCELLED);
        } else if (mode == 1) {
            x.queue.SetStateProviderRevision(x.second, 1);
            CHECK(x.State(x.heldB).status == MMS_UI_REFRESH_FAILED && x.State(x.heldB).result == MMS_E_STALE);
        } else if (mode == 2) {
            const auto dependencies = x.queue.DependencyApi(x.client);
            CHECK(dependencies.unregisterDependency(dependencies.context, x.edgeB) == MMS_OK);
            CHECK(x.State(x.heldB).status == MMS_UI_REFRESH_FAILED && x.State(x.heldB).result == MMS_E_STALE);
        } else {
            // This case deliberately releases the last old snapshot from
            // Group/Work destruction, after Ticket::Finish has reset its own
            // reference. The existing DrainGuard must provide the later wake.
            x.queue.SetMenuTracking(false);
        }
        CHECK(x.wakes.load() == wakes + 1 && x.aCalls == 1 && x.queue.Pending());
        x.Recovered(host);
        CHECK(x.bCalls == (mode == 3 ? 1u : 0u));
        if (mode == 3) CHECK(x.State(x.heldB).status == MMS_UI_REFRESH_FAILED && x.State(x.heldB).result == MMS_E_FAILED);
    }
}
static void UnsubscribedRemovalSignalsDisplayCapacity() {
    for (unsigned mode = 0; mode != 2; ++mode) {
        RetainedBudgetFixture x(mode == 1); HostPump host{x}; x.Park(host);
        const auto wakes = x.wakes.load();
        if (!mode) CHECK(x.data.unregisterSource(x.data.context, x.sourceB) == MMS_OK);
        else CHECK(x.data.unbind(x.data.context, x.bindingB) == MMS_OK);
        CHECK(x.wakes.load() == wakes + 1 && x.aCalls == 1 && x.queue.Pending());
        x.Recovered(host);
        MmsUiDataDisplayV1 stale{}; stale.h = Header<MmsUiDataDisplayV1>();
        CHECK(x.data.acquireDisplay(x.data.context, x.bindingB, &stale) == MMS_E_STALE);
        if (mode) {
            MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
            CHECK(x.data.getSource(x.data.context, x.sourceB, &state) == MMS_OK && state.revision == 1);
        }
    }
}
static void InfrastructureFailureDoesNotRetryFreedCapacity() {
    for (unsigned mode = 0; mode != 2; ++mode) {
        RetainedBudgetFixture x; x.HoldOldB(); HostPump host{x}; x.Park(host);
        const auto wakes = x.wakes.load(), dispatches = host.dispatches;
        if (!mode) x.queue.FailPending(MMS_E_FAILED);
        else {
            x.queue.SetWake([&] { x.wakes.fetch_add(1); return false; });
            x.queue.SetMenuTracking(false);
        }
        CHECK(x.State(x.heldB).status == MMS_UI_REFRESH_FAILED && x.State(x.heldB).result == MMS_E_FAILED);
        const auto state = x.Status();
        CHECK(state.lastResult == MMS_E_FAILED && state.pending && !state.completedRevision && !x.queue.Pending());
        CHECK(x.wakes.load() == wakes + mode && x.aCalls == 1 && !x.bCalls);
        for (unsigned i = 0; i != 8; ++i) host.Dispatch();
        CHECK(host.dispatches == dispatches && x.aCalls == 1 && !x.queue.Pending());
        // Recovery requires a subsequent real publication. Restoring a signal
        // function alone must not turn the failed infrastructure into a poll.
        host.InstallWake(); CHECK(!x.queue.Pending());
        x.WorkerPublish(13);
        CHECK(x.Status().latestRevision == 2 && x.aCalls == 1 && x.queue.Pending());
        host.Wait([&] { return x.Status().completedRevision == 2 && !x.Status().pending; });
        CHECK(x.aCalls == 2 && !x.bCalls && !x.queue.Pending());
    }
}
} // namespace ui_refresh_subscription_checks
static void UiRefreshSubscriptionChecks() {
    ui_refresh_subscription_checks::WorkerPipelineAndBursts();
    ui_refresh_subscription_checks::ExactAcceptedCohorts();
    ui_refresh_subscription_checks::PhaseCombinationsAndReadOnly();
    ui_refresh_subscription_checks::WaitingForFirstPublicationAndValidation();
    ui_refresh_subscription_checks::CancelAndLifecycle();
    ui_refresh_subscription_checks::CapacityAndReadinessDoNotSpin();
    ui_refresh_subscription_checks::MultipleConsumerFailuresDoNotRearmEachOther();
    ui_refresh_subscription_checks::DestroyedTargetRetiresSubscription();
    ui_refresh_subscription_checks::IndependentInvalidationSignalsFreedCapacity();
    ui_refresh_subscription_checks::WakeFailureAndTargetCapabilities();
    ui_refresh_subscription_checks::UnsubscribedSourceShrinkSignalsDisplayCapacity();
    ui_refresh_subscription_checks::RetiredExplicitCohortSignalsDisplayCapacity();
    ui_refresh_subscription_checks::UnsubscribedRemovalSignalsDisplayCapacity();
    ui_refresh_subscription_checks::InfrastructureFailureDoesNotRetryFreedCapacity();
}
