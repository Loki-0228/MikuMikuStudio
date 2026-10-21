// Standalone store contract: compile this TU instead of separately compiling
// ui_refresh_binding.cpp. No HWND, project, plugin DLL or global input is used.
#include "../../src/plugins/ui_refresh_binding.cpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace { thread_local bool failSubscriptionAllocation = false; }
void* operator new(std::size_t bytes) {
    if (failSubscriptionAllocation) throw std::bad_alloc();
    if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0;
void Check(bool value, const char* text) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL subscription store: %s\n", text); std::exit(1); }
}
struct Fixture {
    UiRefreshBindings bindings;
    RefreshClient client{nullptr, "example.subscription"}, alias{nullptr, "example.subscription"};
    MmsUiRefreshBindingApiV1 data{};
    MmsUiRefreshSubscriptionApiV1 api{};
    MmsHandle source = 0;
    uint64_t revision = 0;
    std::atomic<unsigned> wakes{0}, cleanup{0}, capacity{0};
    bool readOnly = false;
    Fixture() {
        bindings.SetHooks({
            [](RefreshClient&, const MmsUiTargetRefV1&) { return MMS_OK; },
            [](const std::function<MmsResult()>& body) { return body(); },
            [this] { return readOnly; },
            [](RefreshClient&, const MmsUiTargetRefV1&, uint32_t) { return MMS_OK; },
            [this] { ++wakes; }, [this] { ++cleanup; }, [this] { ++capacity; }});
        data = bindings.Api(client); api = bindings.SubscriptionApi(client);
        MmsUiDataSourceDescV1 desc{Header<MmsUiDataSourceDescV1>(), {"model", 5}, 256, 0, 0};
        Check(data.registerSource(data.context, &desc, &source) == MMS_OK, "create original source");
    }
    ~Fixture() { bindings.Retire(client); bindings.Retire(alias); }
    MmsHandle Bind(unsigned id) {
        MmsUiDataBindingDescV1 desc{Header<MmsUiDataBindingDescV1>(),
            {Header<MmsUiTargetRefV1>(), id, id + 100, 1}, source, nullptr, nullptr, 256, 0, 0};
        MmsHandle handle = 0; Check(data.bind(data.context, &desc, &handle) == MMS_OK, "bind existing target/source identity"); return handle;
    }
    MmsHandle Subscribe(MmsHandle binding) {
        MmsUiRefreshSubscriptionDescV1 desc{Header<MmsUiRefreshSubscriptionDescV1>(), binding, MMS_UI_REFRESH_DATA, 0, 0};
        MmsHandle handle = 0; Check(api.subscribe(api.context, &desc, &handle) == MMS_OK, "subscribe original binding"); return handle;
    }
    MmsUiRefreshSubscriptionStateV1 State(MmsHandle handle) {
        MmsUiRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshSubscriptionStateV1>();
        Check(api.query(api.context, handle, &state) == MMS_OK, "query same subscription"); return state;
    }
    void Publish(int value) {
        MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), source, revision, {&value, sizeof(value)}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        Check(data.publishSource(data.context, &publish, &state) == MMS_OK, "publish host-owned bytes"); revision = state.revision;
    }
};
void AllocationAndMailbox() {
    Fixture f; const auto binding = f.Bind(1); f.Publish(10);
    const MmsUiRefreshSubscriptionDescV1 desc{Header<MmsUiRefreshSubscriptionDescV1>(), binding, MMS_UI_REFRESH_DATA, 0, 0};
    MmsHandle untouched = 876;
    failSubscriptionAllocation = true;
    const auto failed = f.api.subscribe(f.api.context, &desc, &untouched);
    failSubscriptionAllocation = false;
    Check(failed == MMS_E_MEMORY && untouched == 876 && !f.wakes.load() && !f.bindings.PendingSubscriptions(),
        "failed subscription allocation publishes no handle, mailbox or wake");
    const auto subscription = f.Subscribe(binding);
    auto state = f.State(subscription);
    Check(state.latestRevision == 1 && !state.submittedRevision && state.pending && f.wakes == 1, "existing source schedules exactly once");
    for (int i = 0; i != 1000; ++i) f.Publish(11 + i);
    Check(f.wakes == 1 && f.State(subscription).latestRevision == 1001, "burst coalesces both mailbox and host wake");
    UiRefreshBindings::SubscriptionWork work{};
    Check(f.bindings.TakeSubscription(0, work) && work.subscription == subscription && work.binding == binding && work.revision == 1001,
        "take only newest pending version on same binding");
    Check(!f.bindings.TakeSubscription(0, work) && !f.bindings.PendingSubscriptions(), "claimed work cannot be submitted twice");
    Check(f.State(subscription).pending, "worker query sees a claimed mailbox as pending");
    f.Publish(8000);
    Check(f.wakes == 1, "publication racing a claim does not recursively wake another submission");
    Check(f.bindings.SubmitSubscription(work, 100, MMS_OK), "acknowledge existing refresh ticket");
    Check(!f.bindings.TakeSubscription(0, work), "one in-flight ticket per subscription");
    f.bindings.CompleteSubscription(subscription, 100, 1001, MMS_OK);
    state = f.State(subscription);
    Check(state.completedRevision == 1001 && state.latestRevision == 1002 && state.pending && f.bindings.PendingSubscriptions(),
        "completion retains newer unpublished mailbox");
    Check(f.bindings.TakeSubscription(0, work) && work.revision == 1002, "next drain selects subsequent cohort");
    work.queueCapacityRevision = 81;
    Check(f.bindings.SubmitSubscription(work, 0, MMS_E_BUSY) && !f.bindings.PendingSubscriptions(), "budget failure parks rather than spins");
    state = f.State(subscription); Check(state.lastResult == MMS_E_BUSY && state.pending && state.submittedRevision == 1001, "failed request does not invent an accepted revision");
    Check(!f.bindings.TakeSubscription(0, work), "parked mailbox is not timer work");
    Check(!f.bindings.RetrySubscriptions(UiRefreshBindings::RetryQueueCapacity, 81), "old queue capacity observations do not retry a failed request");
    Check(f.bindings.RetrySubscriptions(UiRefreshBindings::RetryQueueCapacity, 82) && f.bindings.TakeSubscription(0, work), "real external capacity event rearms parked work");
    Check(f.bindings.SubmitSubscription(work, 101, MMS_OK), "accept retried ticket");
    f.bindings.CompleteSubscription(subscription, 101, 1002, MMS_E_BUSY);
    Check(!f.bindings.PendingSubscriptions() && f.State(subscription).pending, "BUSY consumer does not spin after its own cleanup");
    f.Publish(9000); Check(f.bindings.PendingSubscriptions(), "new publication retries a parked failed consumer");
    f.bindings.FailSubscriptionScheduling(MMS_E_FAILED);
    Check(!f.bindings.PendingSubscriptions() && f.State(subscription).lastResult == MMS_E_FAILED, "failed host signal parks runnable work");
    const auto wakeCount = f.wakes.load(); f.Publish(9000);
    Check(f.wakes == wakeCount && !f.bindings.PendingSubscriptions(), "equal bytes do not retry a parked subscription");
    f.Publish(9001); Check(f.wakes == wakeCount + 1 && f.bindings.PendingSubscriptions(), "a new version retries a failed host wake");
    Check(f.api.unsubscribe(f.api.context, subscription) == MMS_OK && f.cleanup == 1 && !f.bindings.PendingSubscriptions(), "unsubscribe drops mailbox and requests internal ticket cleanup");
    state.h = Header<MmsUiRefreshSubscriptionStateV1>(); state.latestRevision = 554;
    Check(f.api.query(f.api.context, subscription, &state) == MMS_E_STALE && state.latestRevision == 554, "old subscription query remains stale and atomic");
}
void QuotasAndLifetime() {
    Fixture f; MmsUiRefreshSubscriptionCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshSubscriptionCapabilitiesV1>();
    Check(f.api.getCapabilities(f.api.context, &caps) == MMS_OK && caps.maxSubscriptionsPerPlugin == 128, "bounded subscription quota is public");
    std::vector<MmsHandle> handles, bindings;
    for (unsigned i = 0; i != caps.maxSubscriptionsPerPlugin; ++i) {
        const auto binding = f.Bind(i + 1); bindings.push_back(binding); handles.push_back(f.Subscribe(binding));
    }
    f.Publish(44);
    UiRefreshBindings::SubscriptionWork work{}; unsigned count = 0; MmsHandle after = 0;
    while (f.bindings.TakeSubscription(after, work)) { after = work.subscription; ++count; f.bindings.SubmitSubscription(work, 500 + count, MMS_OK); }
    Check(count == caps.maxSubscriptionsPerPlugin, "all bounded subscriptions receive one source publication");
    auto alias = f.bindings.SubscriptionApi(f.alias);
    MmsUiRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshSubscriptionStateV1>(); state.latestRevision = 17;
    Check(alias.query(alias.context, handles.front(), &state) == MMS_E_ARGUMENT && state.latestRevision == 17,
        "same textual owner does not bypass exact client ownership");
    f.readOnly = true;
    Check(f.api.getCapabilities(f.api.context, &caps) == MMS_E_BUSY && f.api.unsubscribe(f.api.context, handles.front()) == MMS_E_BUSY,
        "read-only callbacks cannot alter subscriptions or query mutation capabilities");
    f.readOnly = false;
    Check(f.data.unbind(f.data.context, bindings.front()) == MMS_OK, "remove original binding");
    Check(!f.bindings.SubscriptionAlive(f.client, handles.front()) && f.api.query(f.api.context, handles.front(), &state) == MMS_E_STALE,
        "binding removal clears exact subscription");
    f.bindings.AdvanceProjectEpoch(2);
    for (auto handle : handles) Check(f.api.query(f.api.context, handle, &state) == MMS_E_STALE, "epoch invalidates old subscription handles");
    Check(!f.bindings.PendingSubscriptions(), "epoch leaves no pending mailbox");
    f.bindings.Retire(f.client); Check(!f.bindings.HasLeases(f.client), "retirement leaves no subscription lease");
}
void PublishCapacityNotificationIsExact() {
    Fixture f; f.Publish(12); const auto binding = f.Bind(1);
    UiRefreshBindings::Contribution pinned;
    const MmsUiTargetRefV1 target{Header<MmsUiTargetRefV1>(), 1, 101, 1};
    Check(f.bindings.Capture(f.client, binding, target, 1, pinned) == MMS_OK, "pin old published source through the existing contribution");
    const auto initial = f.capacity.load();
    f.Publish(19);
    Check(f.capacity == initial, "replacing a still-pinned source does not announce capacity that was not released");
    f.Publish(19);
    Check(f.capacity == initial, "identical source bytes do not announce a capacity event");
    pinned.reset();
    // A historical release before Publish must not be mistaken for a release
    // caused by an equal-byte publication itself.
    f.Publish(19); Check(f.capacity == initial, "an earlier release does not make a no-op publication notify capacity");
    f.Publish(20);
    Check(f.capacity == initial + 1, "replacing the unpinned retained source notifies capacity exactly once");
    Check(!f.wakes.load(), "an unsubscribed source does not create its own refresh request");
}
void RealDisplayBudgetRelease(bool releaseBeforeCompletion) {
    Fixture f; const auto binding = f.Bind(1); f.Publish(31); const auto subscription = f.Subscribe(binding);
    MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
    Check(f.data.getCapabilities(f.data.context, &caps) == MMS_OK, "read actual retained snapshot quotas");
    uint64_t remaining = caps.maxRetainedBytesPerPlugin - sizeof(int);
    std::vector<MmsHandle> fillers;
    while (remaining) {
        const auto bytes = std::min(remaining, caps.maxSourceBytes);
        const auto id = std::string("retained-") + std::to_string(fillers.size());
        MmsUiDataSourceDescV1 desc{Header<MmsUiDataSourceDescV1>(), {id.data(), id.size()}, bytes, 0, 0};
        MmsHandle source = 0;
        Check(f.data.registerSource(f.data.context, &desc, &source) == MMS_OK, "register retained capacity filler");
        const std::string payload(size_t(bytes), 'x');
        MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(), source, 0, {payload.data(), payload.size()}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        Check(f.data.publishSource(f.data.context, &publish, &state) == MMS_OK, "fill real immutable snapshot accounting");
        fillers.push_back(source); remaining -= bytes;
    }
    UiRefreshBindings::SubscriptionWork work{};
    Check(f.bindings.TakeSubscription(0, work) && f.bindings.SubmitSubscription(work, 300, MMS_OK), "accept existing revision before a host display-budget failure");
    UiRefreshBindings::Contribution contribution;
    Check(f.bindings.Capture(f.client, binding, work.target, work.revision, contribution) == MMS_OK, "capture exact source without adding snapshot bytes");
    uint64_t consumed = 991, waitRevision = UINT64_MAX; bool changed = true; uint32_t wait = 0;
    Check(f.bindings.Consume(contribution, [] { return MMS_OK; }, consumed, changed, &wait, &waitRevision) == MMS_E_BUSY &&
        wait == UiRefreshBindings::RetryDisplayCapacity && consumed == 991 && changed,
        "host writePrepared budget failure has an explicit capacity wait, distinct from provider BUSY");
    if (releaseBeforeCompletion) {
        Check(f.data.unregisterSource(f.data.context, fillers.back()) == MMS_OK, "release retained capacity before the failing ticket is marked parked");
        fillers.pop_back();
        Check(!f.bindings.RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity), "in-flight ticket cannot submit a second ticket during its callback");
    }
    f.bindings.CompleteSubscription(subscription, 300, work.revision, MMS_E_BUSY, wait, waitRevision);
    Check(!f.bindings.RetrySubscriptions() && !f.bindings.PendingSubscriptions(), "unrelated ticket capacity cannot retry a display-budget failure");
    if (!releaseBeforeCompletion) {
        Check(!f.bindings.RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity), "rolled-back candidate is not a retained snapshot capacity event");
        Check(f.data.unregisterSource(f.data.context, fillers.back()) == MMS_OK, "release a genuinely retained host snapshot");
        fillers.pop_back();
    }
    Check(f.bindings.RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity) && f.bindings.TakeSubscription(0, work),
        "actual retained-byte release rearms the same waiting source version");
    Check(f.bindings.SubmitSubscription(work, 301, MMS_OK), "accept retry after true capacity release");
    consumed = 0; changed = false; wait = 0;
    Check(f.bindings.Consume(contribution, [] { return MMS_OK; }, consumed, changed, &wait) == MMS_OK && consumed == 1 && changed,
        "same captured source now creates its host display model");
    f.bindings.CompleteSubscription(subscription, 301, 1, MMS_OK);
    Check(f.State(subscription).completedRevision == 1 && !f.State(subscription).pending, "host budget recovery completes without a new publication");
}
} // namespace
int main() {
    AllocationAndMailbox(); QuotasAndLifetime(); PublishCapacityNotificationIsExact(); RealDisplayBudgetRelease(false); RealDisplayBudgetRelease(true);
    std::printf("UI refresh subscription store: %u checks passed\n", checks); return 0;
}
