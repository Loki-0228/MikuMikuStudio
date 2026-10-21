// Original registry, queue and NativeUi paths. These deterministic INT
// fixtures do not construct MMDApp or claim physical/scene evaluation proof.
#include "ui_property_refresh.hpp"
#include <mms/cpp/property_refresh.hpp>

namespace property_refresh_checks {
struct Fixture : property_data_refresh_checks::Fixture {
    UiPropertyRefresh& notifications = queue.PropertyRefresh();
    MmsUiPropertyRefreshApiV1 changes{};
    std::array<MmsUiPropertyRefV1, 2> refs{};
    std::array<MmsHandle, 2> subscriptions{};
    Fixture() {
        for (unsigned i = 0; i < 2; ++i) Presentation(i).kind = MMS_UI_INPUT;
        Sync();
        CHECK(notifications.BindClient(client, owner) == MMS_OK);
        CHECK(notifications.BindClient(client, owner) == MMS_OK);
        CHECK(notifications.BindClient(other, foreign) == MMS_OK);
        changes = notifications.Api(client);
        for (unsigned i = 0; i < 2; ++i) {
            refs[i].h = Header<MmsUiPropertyRefV1>();
            CHECK(changes.getPropertyRef(changes.context, properties[i], &refs[i]) == MMS_OK);
            CHECK(refs[i].property == properties[i] && refs[i].registrationGeneration && refs[i].projectEpoch);
        }
    }
    MmsUiPropertyRefreshSubscriptionDescV1 Desc(unsigned i = 0, uint32_t flags = 0) {
        return {Header<MmsUiPropertyRefreshSubscriptionDescV1>(), Target(inputs[i]).target,
            refs[i], MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, flags, 0};
    }
    void Subscribe(unsigned i = 0, uint32_t flags = 0) {
        const auto desc = Desc(i, flags);
        CHECK(changes.subscribe(changes.context, &desc, &subscriptions[i]) == MMS_OK && subscriptions[i]);
    }
    MmsUiPropertyRefreshSubscriptionStateV1 State(unsigned i = 0) {
        MmsUiPropertyRefreshSubscriptionStateV1 out{}; out.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>();
        CHECK(changes.query(changes.context, subscriptions[i], &out) == MMS_OK); return out;
    }
    void Complete(unsigned i = 0) {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            const auto state = State(i);
            if (!state.pending && state.completedSequence == state.latestSequence && state.lastResult == MMS_OK) return;
            queue.Drain();
        }
        const auto state = State(i);
        std::cerr << "PROPERTY_REFRESH completion latest=" << state.latestSequence << " submitted=" << state.submittedSequence
            << " completed=" << state.completedSequence << " result=" << state.lastResult << " waiting=" << state.waitingFor << '\n';
        CHECK(false);
    }
    void Notify(unsigned i = 0, bool worker = false, unsigned count = 1) {
        MmsResult result = MMS_OK;
        const auto run = [&] { for (unsigned n = 0; n < count; ++n) {
            const auto call = changes.notify(changes.context, &refs[i]); if (call) result = call;
        }};
        if (worker) { std::thread thread(run); thread.join(); } else run();
        CHECK(result == MMS_OK);
    }
    void Stale(unsigned i = 0) {
        MmsUiPropertyRefreshSubscriptionStateV1 out{}; out.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>();
        out.completedSequence = UINT64_C(0x123456789abcdef); const auto before = out;
        CHECK(changes.query(changes.context, subscriptions[i], &out) == MMS_E_STALE);
        CHECK(!std::memcmp(&out, &before, sizeof(out)));
    }
};
void MailboxesAndReadOnly() {
    Fixture f; const auto registry = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    MmsUiPropertyRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiPropertyRefreshCapabilitiesV1>();
    CHECK(f.changes.getCapabilities(f.changes.context, &caps) == MMS_OK);
    CHECK(caps.maxSubscriptionsPerPlugin == 128 && !caps.supportedFlags && !caps.flags && !caps.reserved);
    CHECK((caps.supportedKinds & (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT)) ==
        (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT));
    auto desc = f.Desc(); MmsHandle unchanged = 0x9911;
    desc.flags = MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY;
    CHECK(f.changes.subscribe(f.changes.context, &desc, &unchanged) == MMS_E_UNSUPPORTED && unchanged == 0x9911);
    desc = f.Desc(); desc.kinds = MMS_UI_REFRESH_REPAINT;
    CHECK(f.changes.subscribe(f.changes.context, &desc, &unchanged) == MMS_E_ARGUMENT && unchanged == 0x9911);
    f.Subscribe(); CHECK(!f.reads[0] && f.State().latestSequence == 1);
    f.values[0] = 31; f.Notify(0, true, 100); const auto queued = f.State();
    CHECK(queued.pending && queued.latestSequence > 1 && !f.reads[0]);
    f.Complete(); CHECK(f.displays[0] == 31 && f.reads[0] == 1 && f.releases[0] == 1 && !f.reads[1]);
    CHECK(!f.State().waitingFor && f.State().completedSequence == queued.latestSequence);
    const auto calls = f.reads[0]; f.Notify(); f.values[0] = 32;
    f.onRead = [&](unsigned i) {
        CHECK(i == 0 && f.queue.ReadOnlyCause(f.client));
        CHECK(f.changes.notify(f.changes.context, &f.refs[0]) == MMS_E_BUSY);
        MmsResult worker = MMS_OK;
        std::thread thread([&] { worker = f.changes.notify(f.changes.context, &f.refs[0]); }); thread.join();
        CHECK(worker == MMS_E_BUSY);
        auto next = f.Desc(1); MmsHandle output = 987;
        CHECK(f.changes.subscribe(f.changes.context, &next, &output) == MMS_E_BUSY && output == 987);
        CHECK(f.changes.unsubscribe(f.changes.context, f.subscriptions[0]) == MMS_E_BUSY);
        f.queue.Drain();
    };
    f.Complete(); CHECK(f.reads[0] == calls + 1 && f.displays[0] == 32);
    f.onRead = {}; const auto finished = f.reads[0];
    for (unsigned n = 0; n < 16; ++n) f.queue.Drain();
    CHECK(f.reads[0] == finished && !f.queue.Pending());
    MmsResult workerCaps = MMS_OK, workerRef = MMS_OK, workerSubscribe = MMS_OK, workerUnsubscribe = MMS_OK, workerQuery = MMS_E_FAILED;
    std::thread thread([&] {
        workerCaps = f.changes.getCapabilities(f.changes.context, &caps);
        auto ref = f.refs[0]; workerRef = f.changes.getPropertyRef(f.changes.context, f.properties[0], &ref);
    });
    // Do not call the test fixture's Target helper from a worker.
    thread.join(); CHECK(workerCaps == MMS_E_THREAD && workerRef == MMS_E_THREAD);
    const auto captured = f.Desc();
    std::thread worker([&] {
        MmsHandle handle = 0x9911;
        workerSubscribe = f.changes.subscribe(f.changes.context, &captured, &handle);
        CHECK(handle == 0x9911);
        workerUnsubscribe = f.changes.unsubscribe(f.changes.context, f.subscriptions[0]);
        MmsUiPropertyRefreshSubscriptionStateV1 out{}; out.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>();
        workerQuery = f.changes.query(f.changes.context, f.subscriptions[0], &out);
    }); worker.join();
    CHECK(workerSubscribe == MMS_E_THREAD && workerUnsubscribe == MMS_E_THREAD && workerQuery == MMS_OK);
    CHECK(f.registry.Revision() == registry && f.registry.PreferencesRevision() == preferences);
    CHECK(!f.edits && !f.stages && f.sceneRevision == 73 && f.playhead == 31);
    CHECK(f.changes.unsubscribe(f.changes.context, f.subscriptions[0]) == MMS_OK); f.Stale();
}
void FailureAndBindingPriority() {
    Fixture f; f.Subscribe(); f.Complete();
    f.values[0] = 41; f.readResults[0] = MMS_E_FAILED; f.Notify(); f.queue.Drain();
    const auto failed = f.State(); CHECK(failed.pending && failed.lastResult == MMS_E_FAILED);
    CHECK(failed.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER);
    const auto reads = f.reads[0];
    for (unsigned i = 0; i < 12; ++i) { f.queue.Drain(); auto request = f.Current(1); const auto ticket = f.Submit(request); f.queue.Drain(); f.Done(ticket, request.kinds); }
    CHECK(f.reads[0] == reads && !f.queue.Pending());
    f.readResults[0] = MMS_OK; f.Notify(); f.Complete(); CHECK(f.displays[0] == 41 && f.reads[0] == reads + 1);
    f.Bind(0); f.Notify(0, true, 5); f.queue.Drain();
    auto parked = f.State(); CHECK(parked.pending && (parked.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_BINDING));
    const auto parkedReads = f.reads[0];
    auto explicitRequest = f.Current(); explicitRequest.binding = f.bindings[0]; explicitRequest.sourceRevision = f.sourceRevisions[0];
    const auto explicitTicket = f.Submit(explicitRequest); f.queue.Drain();
    const auto explicitState = static_cast<refresh_checks::Fixture&>(f).State(explicitTicket);
    CHECK(explicitState.status == MMS_UI_REFRESH_COMPLETED && explicitState.result == MMS_OK &&
        explicitState.completedKinds == explicitRequest.kinds && explicitState.sourceRevision == f.sourceRevisions[0]);
    CHECK(f.BoundDisplay(0) == 400 && f.reads[0] == parkedReads && f.prepares[0] == 1);
    CHECK(f.data.unbind(f.data.context, f.bindings[0]) == MMS_OK); f.values[0] = 42;
    f.Complete(); CHECK(f.displays[0] == 42 && f.reads[0] == parkedReads + 1);
    // A new explicit binding also takes priority over an unscheduled mailbox.
    CHECK(f.data.unregisterSource(f.data.context, f.sources[0]) == MMS_OK);
    f.Notify(); f.Bind(0); f.queue.Drain(); CHECK(f.reads[0] == parkedReads + 1);
    CHECK(f.data.unbind(f.data.context, f.bindings[0]) == MMS_OK); f.Complete();
}
void AcceptedTicketAndCapacity() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f;
        const auto repaint = f.Request(f.Target());
        for (unsigned i = 0; i < 64; ++i) f.Submit(repaint);
        f.Subscribe(); f.queue.Drain(); const auto queued = f.State();
        // The actual internal ticket was accepted, but the original queue's
        // 64-contribution budget leaves it pending beyond this drain.
        CHECK(queued.submittedSequence == queued.latestSequence && !queued.completedSequence && queued.pending && !f.reads[0]);
        if (mode == 0) {
            f.Bind(0); f.queue.Drain(); CHECK(!f.reads[0] && (f.State().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_BINDING));
            CHECK(f.data.unbind(f.data.context, f.bindings[0]) == MMS_OK); f.values[0] = 63; f.Complete(); CHECK(f.displays[0] == 63);
        } else if (mode == 1) {
            CHECK(f.changes.unsubscribe(f.changes.context, f.subscriptions[0]) == MMS_OK); f.Stale();
        } else if (mode == 2) { f.queue.AdvanceProjectEpoch(); f.Stale(); }
        else { f.queue.Retire(f.client); f.Stale(); }
        f.queue.Drain(); if (mode) CHECK(!f.reads[0]);
    }
    Fixture f; MmsUiRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshCapabilitiesV1>();
    CHECK(f.api.getCapabilities(f.api.context, &caps) == MMS_OK);
    const auto repaint = f.Request(f.Target());
    for (unsigned i = 0; i < caps.maxTicketsPerPlugin; ++i) f.Submit(repaint);
    for (unsigned i = 0; i < caps.maxTicketsPerPlugin; i += caps.maxContributionsPerDrain) f.queue.Drain();
    CHECK(!f.queue.Pending()); f.Subscribe(); f.queue.Drain();
    const auto parked = f.State(); CHECK(parked.pending && parked.lastResult == MMS_E_BUSY &&
        (parked.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY));
    const auto wakes = f.wakes.load();
    for (unsigned i = 0; i < 12; ++i) f.queue.Drain();
    CHECK(!f.reads[0] && !f.queue.Pending() && f.wakes.load() == wakes);
    CHECK(f.api.release(f.api.context, f.tickets.back()) == MMS_OK); f.tickets.pop_back();
    f.Complete(); CHECK(f.reads[0] == 1 && f.displays[0] == 7);
}
void FailedWake() {
    Fixture f; f.queue.SetWake([&] { f.wakes.fetch_add(1); return false; });
    f.Subscribe(); auto state = f.State();
    CHECK(state.pending && state.lastResult == MMS_E_FAILED && (state.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE));
    const auto wakes = f.wakes.load();
    for (unsigned i = 0; i < 12; ++i) f.queue.Drain();
    CHECK(!f.reads[0] && !f.queue.Pending() && f.wakes.load() == wakes);
    f.queue.SetWake([&] { f.wakes.fetch_add(1); return true; });
    f.Notify(); f.Complete(); CHECK(f.reads[0] == 1);
    f.notifications.SetHistoryAvailable(true);
    CHECK(f.changes.unsubscribe(f.changes.context, f.subscriptions[0]) == MMS_OK);
    f.Subscribe(0, MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY); f.Complete(); const auto reads = f.reads[0];
    f.notifications.NotifyHistory(2); f.notifications.HistoryWakeResult(false);
    state = f.State(); CHECK(state.pending && (state.waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE));
    for (unsigned i = 0; i < 8; ++i) f.queue.Drain();
    CHECK(f.reads[0] == reads && !f.queue.Pending());
    f.values[0] = 88; f.notifications.NotifyHistory(3); f.notifications.HistoryWakeResult(true);
    f.queue.Drain(); f.Complete(); CHECK(f.displays[0] == 88 && f.reads[0] == reads + 1);
}
void IdentityAndEpoch() {
    Fixture f; auto foreign = f.notifications.Api(f.other); auto desc = f.Desc(); MmsHandle output = 1234;
    CHECK(foreign.subscribe(foreign.context, &desc, &output) != MMS_OK && output == 1234);
    CHECK(foreign.notify(foreign.context, &f.refs[0]) != MMS_OK);
    UiClient sameId{&f.registry, f.owner.id}; RefreshClient copied{&f.queue, f.owner.id};
    CHECK(f.notifications.BindClient(copied, sameId) == MMS_OK);
    auto copiedApi = f.notifications.Api(copied); MmsUiPropertyRefV1 out{}; out.h = Header<MmsUiPropertyRefV1>(); out.property = 999;
    CHECK(copiedApi.getPropertyRef(copiedApi.context, f.properties[0], &out) != MMS_OK && out.property == 999);
    CHECK(copiedApi.notify(copiedApi.context, &f.refs[0]) != MMS_OK);
    CHECK(f.notifications.BindClient(f.client, sameId) != MMS_OK);
    f.queue.Retire(copied);
    f.Subscribe(); f.Notify(); const auto old = f.refs[0];
    auto replacement = f.Description(0); replacement.info.label = S("new original property record");
    CHECK(f.ui.updateProperty(f.ui.context, f.properties[0], &replacement) == MMS_OK); f.Stale();
    CHECK(f.changes.notify(f.changes.context, &old) == MMS_E_STALE);
    out = {}; out.h = Header<MmsUiPropertyRefV1>();
    CHECK(f.changes.getPropertyRef(f.changes.context, f.properties[0], &out) == MMS_OK);
    CHECK(out.property == old.property && out.registrationGeneration != old.registrationGeneration && out.projectEpoch == old.projectEpoch);
    f.refs[0] = out; f.Presentation(0).propertyRecord = f.registry.properties.at(f.propertyIds[0]); f.Sync(); f.Subscribe();
    f.Complete(); const auto current = f.refs[0]; f.queue.AdvanceProjectEpoch(); f.Stale();
    CHECK(f.changes.notify(f.changes.context, &current) == MMS_E_STALE);
    out.h = Header<MmsUiPropertyRefV1>();
    CHECK(f.changes.getPropertyRef(f.changes.context, f.properties[0], &out) == MMS_OK && out.projectEpoch != current.projectEpoch);
    f.refs[0] = out; f.Sync(); f.Subscribe();
    CHECK(f.ui.unregisterItem(f.ui.context, f.inputs[0]) == MMS_OK); f.Stale(); f.queue.Drain();
}
void QuotaAndTargetRecovery() {
    Fixture f; f.Subscribe(); f.Complete();
    f.queue.SuspendTargets(); f.values[0] = 55; f.Notify(); f.queue.Drain();
    CHECK(f.State().pending && (f.State().waitingFor & MMS_UI_PROPERTY_REFRESH_WAIT_TARGET));
    const auto reads = f.reads[0];
    for (unsigned i = 0; i < 8; ++i) f.queue.Drain();
    CHECK(f.reads[0] == reads && !f.queue.Pending());
    f.Sync(); f.Complete(); CHECK(f.displays[0] == 55 && f.reads[0] == reads + 1);
    // Retain exactly 128 original INPUT registrations, with one subscription
    // each. No drain or property reader is needed to test admission capacity.
    std::vector<MmsHandle> subscriptions{f.subscriptions[0]};
    std::vector<HWND> fields;
    for (unsigned i = 1; i <= 128; ++i) {
        const auto id = std::string("example.refresh.quota-input-") + std::to_string(i);
        auto input = ui_input_broker_checks::Input(id, f.owner.id, f.propertyIds[0]);
        MmsHandle element = 0; CHECK(f.ui.registerElement(f.ui.context, &input, &element) == MMS_OK);
        const auto field = CreateWindowExW(0, L"STATIC", L"quota", WS_CHILD | WS_VISIBLE,
            0, 0, 30, 20, f.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(field); fields.push_back(field);
        RefreshPresentation p{element, f.owner.id, 200 + i, field, 96, 0, 0, 30, 20, true};
        p.kind = MMS_UI_INPUT; p.propertyDataRefresh = true; p.propertyRecord = f.registry.properties.at(f.propertyIds[0]);
        f.presentations.push_back(p);
        f.Sync(); auto desc = f.Desc(); desc.target = f.Target(element).target;
        MmsHandle output = 0x9911;
        const auto result = f.changes.subscribe(f.changes.context, &desc, &output);
        if (i < 128) { CHECK(result == MMS_OK && output && output != 0x9911); subscriptions.push_back(output); }
        else CHECK(result == MMS_E_BUSY && output == 0x9911);
    }
    CHECK(subscriptions.size() == 128 && !f.reads[1]);
    for (auto handle : subscriptions) CHECK(f.changes.unsubscribe(f.changes.context, handle) == MMS_OK);
    for (auto field : fields) CHECK(DestroyWindow(field));
}
void TargetDuringConsumption() {
    Fixture f; f.Subscribe(); f.Complete();
    const auto initial = f.Target(f.inputs[0]).target; const auto width = f.Presentation(0).width;
    bool collapse = true;
    f.beforeApply = [&](unsigned i) -> MmsResult {
        if (i == 0 && collapse) {
            collapse = false;
            CHECK(SetWindowPos(f.windows[0], nullptr, 0, 0, 0, 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
            f.Presentation(0).width = 0; f.Sync();
        }
        return MMS_OK;
    };
    f.values[0] = 77; f.Notify(); f.queue.Drain();
    const auto parked = f.State();
    std::cout << "PROPERTY_REFRESH running target unavailable result=" << parked.lastResult << " wait=" << parked.waitingFor << std::endl;
    CHECK(parked.pending && parked.lastResult == MMS_E_NOT_READY && parked.waitingFor == MMS_UI_PROPERTY_REFRESH_WAIT_TARGET);
    const auto reads = f.reads[0]; for (unsigned i = 0; i < 8; ++i) f.queue.Drain();
    CHECK(f.reads[0] == reads && !f.queue.Pending());
    CHECK(SetWindowPos(f.windows[0], nullptr, 0, 0, int(width), 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    f.Presentation(0).width = width; f.Sync();
    CHECK(f.Target(f.inputs[0]).target.instanceGeneration == initial.instanceGeneration);
    f.Complete(); CHECK(f.displays[0] == 77 && f.reads[0] == reads + 1);
    // A provider's own NOT_READY remains a consumer failure. An unchanged
    // geometry publication must not turn it into a retry loop.
    f.beforeApply = {}; f.readResults[0] = MMS_E_NOT_READY; f.Notify(); f.queue.Drain();
    CHECK(f.State().lastResult == MMS_E_NOT_READY && f.State().waitingFor == MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER);
    const auto consumerReads = f.reads[0]; f.Sync();
    for (unsigned i = 0; i < 8; ++i) f.queue.Drain();
    CHECK(f.reads[0] == consumerReads && !f.queue.Pending());
    f.readResults[0] = MMS_OK; f.Notify(); f.Complete();
}
struct NativeFixture : input_state_content_checks::Fixture {
    UiPropertyRefresh& notifications = queue.PropertyRefresh();
    MmsUiPropertyRefreshApiV1 changes{}; MmsUiPropertyRefV1 ref{}; MmsHandle subscription = 0;
    unsigned wakes = 0;
    explicit NativeFixture(bool host = true): input_state_content_checks::Fixture(host) {
        CHECK(notifications.BindClient(client, data.owner) == MMS_OK); changes = notifications.Api(client);
        ref.h = Header<MmsUiPropertyRefV1>(); CHECK(changes.getPropertyRef(changes.context, data.property, &ref) == MMS_OK);
        queue.SetWake([this] { ++wakes; return true; });
    }
    void Subscribe(uint32_t flags = 0) {
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        CHECK(refresh.getTarget(refresh.context, data.element, &target) == MMS_OK);
        MmsUiPropertyRefreshSubscriptionDescV1 desc{Header<MmsUiPropertyRefreshSubscriptionDescV1>(), target.target, ref,
            MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, flags, 0};
        CHECK(changes.subscribe(changes.context, &desc, &subscription) == MMS_OK);
    }
    MmsUiPropertyRefreshSubscriptionStateV1 State() {
        MmsUiPropertyRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>();
        CHECK(changes.query(changes.context, subscription, &state) == MMS_OK); return state;
    }
    void Complete() {
        for (unsigned i = 0; i < 8; ++i) { auto state = State(); if (!state.pending && state.lastResult == MMS_OK) return; queue.Drain(); }
        CHECK(false);
    }
    void Notify() { CHECK(changes.notify(changes.context, &ref) == MMS_OK); }
    ~NativeFixture() { history.SetPublicationSignal({}); queue.SetWake({}); }
};
void NativeDraftAndHistory() {
    for (bool host : {false, true}) {
        NativeFixture f(host); const auto field = f.field, wrapper = f.wrapper;
        const auto revision = f.data.registry.Revision(), preferences = f.data.registry.PreferencesRevision();
        f.Subscribe(); f.Complete(); f.data.value = ui_input_broker_checks::Integer(5); f.Notify(); f.Complete();
        CHECK(native_property_input_checks::Caption(field) == L"5");
        if (host) {
            input_state_content_checks::SetRaw(field, L"7"); SendMessageW(field, EM_SETSEL, 0, 1);
            SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0); f.data.value = ui_input_broker_checks::Integer(6);
            f.Notify(); f.Complete();
            DWORD start = 99, end = 99; SendMessageW(field, EM_GETSEL, WPARAM(&start), LPARAM(&end));
            CHECK(native_property_input_checks::Caption(field) == L"7" && GetFocus() == field && start == 0 && end == 1);
            SendMessageW(field, WM_IME_ENDCOMPOSITION, 0, 0); native_property_input_checks::Escape(field);
            CHECK(native_property_input_checks::Caption(field) == L"6");
        } else CHECK(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY);
        CHECK(f.field == field && GetParent(field) == wrapper && !f.data.edits && !f.history.UndoCount());
        CHECK(f.data.registry.Revision() == revision && f.data.registry.PreferencesRevision() == preferences);
    }
    NativeFixture f; f.notifications.SetHistoryAvailable(true);
    CHECK(f.history.SetPublicationSignal({&f, [](void* p, uint64_t revision) noexcept {
        auto& f = *static_cast<NativeFixture*>(p); f.notifications.NotifyHistory(revision); ++f.wakes;
    }}) == MMS_OK);
    f.Subscribe(MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY); f.Complete();
    const auto initial = f.State().latestSequence;
    auto commit = [&](int64_t value, SharedHistory::NativeIntent intent) {
        auto change = std::make_shared<input_state_content_checks::IntChange>(f.data.value, f.observation, value);
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        CHECK(f.history.PrepareNative("property history source", change, token, intent) == MMS_OK);
        CHECK(f.history.CommitNative(std::move(token)) == MMS_OK);
    };
    commit(8, SharedHistory::NativeIntent::ExplicitCommit); CHECK(native_property_input_checks::Caption(f.field) == L"2");
    f.queue.Drain(); f.Complete(); CHECK(native_property_input_checks::Caption(f.field) == L"8" && f.State().latestSequence > initial);
    CHECK(f.history.History(false) == MMS_OK); f.queue.Drain(); f.Complete(); CHECK(native_property_input_checks::Caption(f.field) == L"2");
    CHECK(f.history.History(true) == MMS_OK); f.queue.Drain(); f.Complete(); CHECK(native_property_input_checks::Caption(f.field) == L"8");
    CHECK(f.history.SetMode(SharedHistory::RecordingMode::ExplicitOnly) == MMS_OK); const auto undo = f.history.UndoCount();
    commit(9, SharedHistory::NativeIntent::Adjustment); f.queue.Drain(); f.Complete();
    CHECK(native_property_input_checks::Caption(f.field) == L"9" && f.history.UndoCount() == undo && !f.data.edits);
    CHECK(f.history.SetPublicationSignal({}) == MMS_OK);
}
}
static void PropertyRefreshChecks() {
    property_refresh_checks::MailboxesAndReadOnly();
    property_refresh_checks::FailureAndBindingPriority();
    property_refresh_checks::AcceptedTicketAndCapacity();
    property_refresh_checks::FailedWake();
    property_refresh_checks::IdentityAndEpoch();
    property_refresh_checks::QuotaAndTargetRecovery();
    property_refresh_checks::TargetDuringConsumption();
    property_refresh_checks::NativeDraftAndHistory();
}
