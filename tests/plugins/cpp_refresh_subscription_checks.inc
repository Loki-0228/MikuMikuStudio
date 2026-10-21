#include <mms/cpp/refresh_subscription.hpp>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

namespace cpp_refresh_subscription_checks {
namespace sdk = mms::sdk;
template<class T> static bool HeaderIs(const T* value) {
    return value && value->h.size == sizeof(T) && value->h.major == 1 && value->h.minor == MMS_ABI_MINOR;
}
template<class T> static T Sentinel() {
    T value; std::memset(&value, 0xa5, sizeof(value)); return value;
}
template<class T> static bool SameBytes(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
static bool TextIs(MmsString value, std::string_view expected) {
    return value.bytes == expected.size() && (!value.bytes ||
        (value.data && std::memcmp(value.data, expected.data(), expected.size()) == 0));
}
static void Throw(unsigned mode) {
    if (mode == 1) throw std::bad_alloc();
    if (mode == 2) throw std::length_error("subscription fake length");
    if (mode == 3) throw std::runtime_error("subscription fake runtime");
}
// This host is deliberately independent of the subscription implementation.
// It records the exact C handles, rejects cleanup on a worker, and scribbles
// candidate outputs before injected failures to exercise SDK output ownership.
struct Host {
    const std::thread::id ui = std::this_thread::get_id();
    const MmsHandle prefix, rawSource, rawBinding;
    MmsHandle nextSubscription, nextData;
    MmsHostV1 host{};
    MmsUiRefreshSubscriptionApiV1 table{};
    MmsUiRefreshBindingApiV1 data{};
    MmsUiRefreshSubscriptionDescV1 lastDesc{};
    MmsUiDataBindingDescV1 lastBinding{};
    std::map<MmsHandle, MmsHandle> subscriptions, bindings;
    std::set<MmsHandle> sources;
    unsigned serviceQueries = 0, dataQueries = 0, capCalls = 0, subscribeCalls = 0;
    unsigned stateCalls = 0, workerStates = 0, releaseCalls = 0, wrongThreadCalls = 0;
    unsigned sourceRegistrations = 0, sourceReleases = 0, bindingRegistrations = 0, bindingReleases = 0, unexpected = 0;
    bool arguments = true, zeroHandle = false;
    unsigned throwService = 0, throwCaps = 0, throwSubscribe = 0, throwState = 0, throwRelease = 0;
    MmsResult negotiation = MMS_OK, capResult = MMS_OK, subscribeResult = MMS_OK;
    MmsResult stateResult = MMS_OK, releaseResult = MMS_OK;
    explicit Host(MmsHandle seed = UINT64_C(0x1700000000000000))
        : prefix(seed), rawSource(seed + 11), rawBinding(seed + 29),
          nextSubscription(seed + 101), nextData(seed + 1001) {
        sources.insert(rawSource); bindings.emplace(rawBinding, rawSource);
        table.h = sdk::Header<MmsUiRefreshSubscriptionApiV1>(); table.context = this;
        table.getCapabilities = Caps; table.subscribe = Subscribe; table.query = State; table.unsubscribe = Unsubscribe;
        data.h = sdk::Header<MmsUiRefreshBindingApiV1>(); data.context = this;
        data.getCapabilities = [](void* p, MmsUiDataCapabilitiesV1*) -> MmsResult {
            ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED;
        };
        data.registerSource = RegisterSource;
        data.publishSource = [](void* p, const MmsUiDataSourcePublishV1*, MmsUiDataSourceStateV1*) -> MmsResult {
            ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED;
        };
        data.getSource = [](void* p, MmsHandle, MmsUiDataSourceStateV1*) -> MmsResult {
            ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED;
        };
        data.unregisterSource = UnregisterSource; data.bind = Bind; data.unbind = Unbind;
        data.writePrepared = [](void* p, MmsHandle, MmsBytes) -> MmsResult {
            ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED;
        };
        data.acquireDisplay = [](void* p, MmsHandle, MmsUiDataDisplayV1*) -> MmsResult {
            ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED;
        };
        data.releaseDisplay = [](void* p, MmsHandle) -> MmsResult {
            ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED;
        };
        host.h = sdk::Header<MmsHostV1>(); host.context = this; host.queryService = Query;
    }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    bool UiCall() {
        if (std::this_thread::get_id() == ui) return true;
        ++wrongThreadCalls; return false;
    }
    static MmsResult MMS_CALL Query(void* context, MmsString id, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        auto& x = *static_cast<Host*>(context);
        if (TextIs(id, "mms.ui.refresh.subscribe")) {
            ++x.serviceQueries; x.arguments &= major == 1 && minor == 0 && bytes == sizeof(x.table) && out;
            if (!out || bytes != sizeof(x.table)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.table, sizeof(x.table)); Throw(x.throwService); return x.negotiation;
        }
        if (TextIs(id, "mms.ui.refresh.bind")) {
            ++x.dataQueries; x.arguments &= major == 1 && minor == 0 && bytes == sizeof(x.data) && out;
            if (!out || bytes != sizeof(x.data)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.data, sizeof(x.data)); return MMS_OK;
        }
        ++x.unexpected; return MMS_E_NOT_FOUND;
    }
    static MmsResult MMS_CALL Caps(void* context, MmsUiRefreshSubscriptionCapabilitiesV1* out) {
        auto& x = *static_cast<Host*>(context); ++x.capCalls;
        if (!x.UiCall()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiRefreshSubscriptionCapabilitiesV1>();
        out->maxSubscriptionsPerPlugin = 37;
        out->supportedKinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT;
        out->supportedScopes = MMS_UI_REFRESH_SELF;
        Throw(x.throwCaps); return x.capResult;
    }
    static MmsResult MMS_CALL Subscribe(void* context, const MmsUiRefreshSubscriptionDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(context); ++x.subscribeCalls;
        if (!x.UiCall()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out && !desc->flags && !desc->reserved;
        if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        x.lastDesc = *desc; *out = x.prefix + 999;
        Throw(x.throwSubscribe); if (x.subscribeResult) return x.subscribeResult;
        if (!x.bindings.count(desc->binding)) return MMS_E_ARGUMENT;
        constexpr uint32_t supported = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT;
        if (!(desc->kinds & MMS_UI_REFRESH_DATA) || (desc->kinds & ~supported)) return MMS_E_ARGUMENT;
        if (x.zeroHandle) { *out = 0; return MMS_OK; }
        const auto handle = x.nextSubscription++; x.subscriptions.emplace(handle, desc->binding); *out = handle; return MMS_OK;
    }
    static MmsResult MMS_CALL State(void* context, MmsHandle handle, MmsUiRefreshSubscriptionStateV1* out) {
        auto& x = *static_cast<Host*>(context); ++x.stateCalls;
        if (std::this_thread::get_id() != x.ui) ++x.workerStates;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiRefreshSubscriptionStateV1>();
        out->subscription = handle; out->binding = x.prefix + 999;
        Throw(x.throwState); if (x.stateResult) return x.stateResult;
        const auto found = x.subscriptions.find(handle); if (found == x.subscriptions.end()) return MMS_E_STALE;
        out->binding = found->second; out->projectEpoch = 43; out->latestRevision = 19;
        out->submittedRevision = 17; out->completedRevision = 13; out->lastResult = MMS_E_BUSY; out->pending = 1; return MMS_OK;
    }
    static MmsResult MMS_CALL Unsubscribe(void* context, MmsHandle handle) {
        auto& x = *static_cast<Host*>(context); ++x.releaseCalls;
        if (!x.UiCall()) return MMS_E_THREAD;
        Throw(x.throwRelease); if (x.releaseResult) return x.releaseResult;
        return x.subscriptions.erase(handle) ? MMS_OK : MMS_E_STALE;
    }
    MmsUiTargetRefV1 Target() const {
        MmsUiTargetRefV1 target{}; target.h = sdk::Header<MmsUiTargetRefV1>();
        target.element = prefix + 47; target.instanceGeneration = 7; target.projectEpoch = 43; return target;
    }
    static MmsResult MMS_CALL RegisterSource(void* context, const MmsUiDataSourceDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(context); ++x.sourceRegistrations;
        if (!x.UiCall()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out && TextIs(desc->id, "example.sdk.subscription.data") &&
            desc->maxBytes == 64 && !desc->flags && !desc->reserved;
        if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        *out = x.nextData++; x.sources.insert(*out); return MMS_OK;
    }
    static MmsResult MMS_CALL UnregisterSource(void* context, MmsHandle source) {
        auto& x = *static_cast<Host*>(context); ++x.sourceReleases;
        if (!x.UiCall()) return MMS_E_THREAD;
        for (const auto& binding : x.bindings) if (binding.second == source) return MMS_E_BUSY;
        return x.sources.erase(source) ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Bind(void* context, const MmsUiDataBindingDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(context); ++x.bindingRegistrations;
        if (!x.UiCall()) return MMS_E_THREAD;
        const auto target = x.Target();
        x.arguments &= HeaderIs(desc) && out && HeaderIs(&desc->target) && desc->target.element == target.element &&
            desc->target.instanceGeneration == target.instanceGeneration && desc->target.projectEpoch == target.projectEpoch &&
            desc->maxDisplayBytes == 64 && !desc->prepare && !desc->context && !desc->flags && !desc->reserved;
        if (!HeaderIs(desc) || !out || !x.sources.count(desc->source)) return MMS_E_ARGUMENT;
        x.lastBinding = *desc; *out = x.nextData++; x.bindings.emplace(*out, desc->source); return MMS_OK;
    }
    static MmsResult MMS_CALL Unbind(void* context, MmsHandle binding) {
        auto& x = *static_cast<Host*>(context); ++x.bindingReleases;
        if (!x.UiCall()) return MMS_E_THREAD;
        for (const auto& subscription : x.subscriptions) if (subscription.second == binding) return MMS_E_BUSY;
        return x.bindings.erase(binding) ? MMS_OK : MMS_E_STALE;
    }
    sdk::RefreshSubscriptionServices Services() {
        sdk::RefreshSubscriptionServices result;
        CHECK(sdk::RefreshSubscriptionServices::Query(host, result) == MMS_OK); return result;
    }
};

static void NegotiationAndEmpty() {
    static_assert(!std::is_copy_constructible_v<sdk::RefreshSubscription>);
    static_assert(!std::is_copy_assignable_v<sdk::RefreshSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::RefreshSubscription>);
    static_assert(!std::is_move_assignable_v<sdk::RefreshSubscription>);
    static_assert(std::is_nothrow_destructible_v<sdk::RefreshSubscription>);
    Host original, candidate(UINT64_C(0x2700000000000000));
    sdk::RefreshSubscriptionServices empty; sdk::RefreshSubscription subscription;
    auto caps = Sentinel<MmsUiRefreshSubscriptionCapabilitiesV1>(); const auto originalCaps = caps;
    auto state = Sentinel<MmsUiRefreshSubscriptionStateV1>(); const auto originalState = state;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY);
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && SameBytes(caps, originalCaps));
    CHECK(empty.Subscribe(original.rawBinding, subscription) == MMS_E_NOT_READY && !subscription);
    CHECK(!subscription.get() && subscription.State(state) == MMS_E_NOT_READY && SameBytes(state, originalState));
    CHECK(subscription.Release() == MMS_OK && subscription.detach() == 0);
    auto service = original.Services(); const auto originalTable = service.api();
    CHECK(service.ready() && service.CheckThread() == MMS_OK && originalTable.context == &original && original.arguments);
    for (auto failure : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY, MMS_E_FAILED}) {
        candidate.negotiation = failure;
        CHECK(sdk::RefreshSubscriptionServices::Query(candidate.host, service) == failure);
        CHECK(service.ready() && service.CheckThread() == MMS_OK && SameBytes(service.api(), originalTable));
    }
    candidate.negotiation = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        candidate.throwService = mode;
        CHECK(sdk::RefreshSubscriptionServices::Query(candidate.host, service) == (mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED));
        CHECK(SameBytes(service.api(), originalTable) && service.CheckThread() == MMS_OK);
    }
    candidate.throwService = 0; auto host = candidate.host; const auto calls = candidate.serviceQueries;
    host.h.size = sizeof(host) - 1;
    CHECK(sdk::RefreshSubscriptionServices::Query(host, service) == MMS_E_VERSION && SameBytes(service.api(), originalTable));
    host = candidate.host; host.h.major = 2;
    CHECK(sdk::RefreshSubscriptionServices::Query(host, empty) == MMS_E_VERSION && !empty.ready());
    host = candidate.host; host.queryService = nullptr;
    CHECK(sdk::RefreshSubscriptionServices::Query(host, service) == MMS_E_ARGUMENT && SameBytes(service.api(), originalTable));
    CHECK(candidate.serviceQueries == calls);
    const auto table = candidate.table;
    candidate.table.h.size = sizeof(table) - 1;
    CHECK(sdk::RefreshSubscriptionServices::Query(candidate.host, service) == MMS_E_VERSION && SameBytes(service.api(), originalTable));
    candidate.table = table; candidate.table.h.major = 2;
    CHECK(sdk::RefreshSubscriptionServices::Query(candidate.host, service) == MMS_E_VERSION && SameBytes(service.api(), originalTable));
    for (unsigned field = 0; field < 4; ++field) {
        candidate.table = table;
        if (field == 0) candidate.table.getCapabilities = nullptr;
        if (field == 1) candidate.table.subscribe = nullptr;
        if (field == 2) candidate.table.query = nullptr;
        if (field == 3) candidate.table.unsubscribe = nullptr;
        CHECK(sdk::RefreshSubscriptionServices::Query(candidate.host, service) == MMS_E_UNSUPPORTED);
        CHECK(SameBytes(service.api(), originalTable) && service.CheckThread() == MMS_OK);
    }
    candidate.table = table; candidate.table.h.size += 32; candidate.table.h.minor = 8;
    host = candidate.host; host.h.size += 32; host.h.minor = 8;
    CHECK(sdk::RefreshSubscriptionServices::Query(host, empty) == MMS_OK && empty.ready());
    CHECK(empty.api().context == &candidate && empty.api().h.minor == 8 && candidate.arguments);
    CHECK(service.Capabilities(caps) == MMS_OK && caps.h.size == sizeof(caps) && caps.maxSubscriptionsPerPlugin == 37);
    CHECK(caps.supportedKinds == 15 && caps.supportedScopes == MMS_UI_REFRESH_SELF && !caps.flags && !caps.reserved[0] && !caps.reserved[1]);
    for (auto failure : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED}) {
        caps = originalCaps; original.capResult = failure;
        CHECK(service.Capabilities(caps) == failure && SameBytes(caps, originalCaps));
    }
    original.capResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        caps = originalCaps; original.throwCaps = mode;
        CHECK(service.Capabilities(caps) == (mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED) && SameBytes(caps, originalCaps));
    }
    original.throwCaps = 0;
    CHECK(original.arguments && original.subscriptions.empty() && candidate.subscriptions.empty());
}

static void RegistrationAndState() {
    Host x; auto service = x.Services(); sdk::RefreshSubscription subscription;
    const auto before = x.subscribeCalls;
    CHECK(service.Subscribe(0, subscription) == MMS_E_ARGUMENT && !subscription && x.subscribeCalls == before);
    for (auto failure : {MMS_E_ARGUMENT, MMS_E_BUSY, MMS_E_STALE, MMS_E_FAILED}) {
        x.subscribeResult = failure;
        CHECK(service.Subscribe(x.rawBinding, subscription) == failure && !subscription && x.subscriptions.empty());
    }
    x.subscribeResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwSubscribe = mode;
        CHECK(service.Subscribe(x.rawBinding, subscription) == (mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED));
        CHECK(!subscription && x.subscriptions.empty());
    }
    x.throwSubscribe = 0; x.zeroHandle = true;
    CHECK(service.Subscribe(x.rawBinding, subscription) == MMS_E_FAILED && !subscription && x.subscriptions.empty());
    x.zeroHandle = false;
    for (uint32_t kinds : {0u, MMS_UI_REFRESH_REPAINT, MMS_UI_REFRESH_DATA | 0x80000000u}) {
        CHECK(service.Subscribe(x.rawBinding, subscription, kinds) == MMS_E_ARGUMENT && !subscription && x.lastDesc.kinds == kinds);
    }
    CHECK(!x.releaseCalls && x.subscriptions.empty() && x.bindings.size() == 1 && x.sources.size() == 1);
    CHECK(service.Subscribe(x.rawBinding, subscription) == MMS_OK && subscription);
    const auto handle = subscription.get(); const auto subscribed = x.subscribeCalls;
    CHECK(handle > UINT32_MAX && x.subscriptions.size() == 1 && x.subscriptions.at(handle) == x.rawBinding);
    CHECK(x.lastDesc.binding == x.rawBinding && x.lastDesc.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) && !x.lastDesc.flags && !x.lastDesc.reserved);
    CHECK(service.Subscribe(x.rawBinding, subscription, MMS_UI_REFRESH_DATA) == MMS_E_BUSY && subscription.get() == handle && x.subscribeCalls == subscribed);
    MmsUiRefreshSubscriptionStateV1 low{}; low.h = sdk::Header<MmsUiRefreshSubscriptionStateV1>();
    CHECK(x.table.query(x.table.context, handle, &low) == MMS_OK);
    auto state = Sentinel<MmsUiRefreshSubscriptionStateV1>();
    CHECK(subscription.State(state) == MMS_OK && SameBytes(state, low));
    CHECK(state.subscription == handle && state.binding == x.rawBinding && state.projectEpoch == 43 && state.latestRevision == 19 &&
        state.submittedRevision == 17 && state.completedRevision == 13 && state.lastResult == MMS_E_BUSY && state.pending == 1 && !state.reserved);
    const auto sentinel = Sentinel<MmsUiRefreshSubscriptionStateV1>();
    for (auto failure : {MMS_E_STALE, MMS_E_NOT_READY, MMS_E_FAILED, MMS_E_MEMORY}) {
        state = sentinel; x.stateResult = failure;
        CHECK(subscription.State(state) == failure && SameBytes(state, sentinel) && subscription.get() == handle);
    }
    x.stateResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        state = sentinel; x.throwState = mode;
        CHECK(subscription.State(state) == (mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED));
        CHECK(SameBytes(state, sentinel) && subscription.get() == handle);
    }
    x.throwState = 0;
    MmsResult query = MMS_E_FAILED, threadCheck = MMS_OK, capsResult = MMS_OK, subscribeResult = MMS_OK, emptyRelease = MMS_E_FAILED;
    auto caps = Sentinel<MmsUiRefreshSubscriptionCapabilitiesV1>(); const auto capsBefore = caps;
    sdk::RefreshSubscription workerOutput; const auto capCalls = x.capCalls, subscribeCalls = x.subscribeCalls;
    const auto stateCalls = x.stateCalls, workerStates = x.workerStates;
    std::thread worker([&] {
        query = subscription.State(state); threadCheck = service.CheckThread();
        capsResult = service.Capabilities(caps); subscribeResult = service.Subscribe(x.rawBinding, workerOutput);
        emptyRelease = workerOutput.Release();
    }); worker.join();
    CHECK(query == MMS_OK && SameBytes(state, low) && x.stateCalls == stateCalls + 1 && x.workerStates == workerStates + 1);
    CHECK(threadCheck == MMS_E_THREAD && capsResult == MMS_E_THREAD && subscribeResult == MMS_E_THREAD && !workerOutput && emptyRelease == MMS_OK);
    CHECK(SameBytes(caps, capsBefore) && x.capCalls == capCalls && x.subscribeCalls == subscribeCalls && !x.wrongThreadCalls);
    CHECK(subscription.Release() == MMS_OK && !subscription && x.subscriptions.empty());
    CHECK(service.Subscribe(x.rawBinding, subscription, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT) == MMS_OK);
    CHECK(x.lastDesc.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT));
    CHECK(subscription.Release() == MMS_OK && x.arguments && !x.unexpected);
}

static void ReleaseAndMove() {
    Host x; auto service = x.Services(); sdk::RefreshSubscription original;
    CHECK(service.Subscribe(x.rawBinding, original) == MMS_OK); const auto handle = original.get();
    sdk::RefreshSubscription moved(std::move(original)); const auto releases = x.releaseCalls, queries = x.stateCalls;
    auto state = Sentinel<MmsUiRefreshSubscriptionStateV1>(); const auto sentinel = state;
    CHECK(!original && !original.get() && moved.get() == handle && original.Release() == MMS_OK);
    CHECK(original.State(state) == MMS_E_NOT_READY && SameBytes(state, sentinel) && !original.detach());
    CHECK(x.releaseCalls == releases && x.stateCalls == queries);
    MmsResult workerRelease = MMS_OK;
    std::thread worker([&] { workerRelease = moved.Release(); }); worker.join();
    CHECK(workerRelease == MMS_E_THREAD && moved.get() == handle && x.releaseCalls == releases && !x.wrongThreadCalls);
    for (auto failure : {MMS_E_BUSY, MMS_E_THREAD, MMS_E_NOT_FOUND, MMS_E_FAILED, MMS_E_MEMORY}) {
        x.releaseResult = failure;
        CHECK(moved.Release() == failure && moved.get() == handle && x.subscriptions.count(handle) == 1);
        CHECK(moved.State(state) == MMS_OK && state.subscription == handle);
    }
    x.releaseResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwRelease = mode;
        CHECK(moved.Release() == (mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED) && moved.get() == handle && x.subscriptions.count(handle) == 1);
    }
    x.throwRelease = 0;
    CHECK(moved.Release() == MMS_OK && !moved && x.subscriptions.empty());
    const auto released = x.releaseCalls;
    CHECK(moved.Release() == MMS_OK && moved.State(state) == MMS_E_NOT_READY && x.releaseCalls == released);
    CHECK(service.Subscribe(x.rawBinding, original) == MMS_OK); const auto stale = original.get();
    x.subscriptions.erase(stale); state = sentinel;
    CHECK(original.State(state) == MMS_E_STALE && original.get() == stale && SameBytes(state, sentinel));
    CHECK(original.Release() == MMS_OK && !original && !original.get());
    const auto afterStale = x.releaseCalls; CHECK(original.Release() == MMS_OK && x.releaseCalls == afterStale);
    CHECK(service.Subscribe(x.rawBinding, original) == MMS_OK); const auto crossing = original.get();
    const auto beforeTransfer = x.releaseCalls; std::unique_ptr<sdk::RefreshSubscription> returned;
    MmsResult transferRelease = MMS_OK, transferState = MMS_E_FAILED;
    std::thread transfer([&] {
        returned = std::make_unique<sdk::RefreshSubscription>(std::move(original));
        transferRelease = returned->Release(); transferState = returned->State(state);
    }); transfer.join();
    CHECK(!original && returned && returned->get() == crossing && transferRelease == MMS_E_THREAD && transferState == MMS_OK);
    CHECK(state.subscription == crossing && x.releaseCalls == beforeTransfer && !x.wrongThreadCalls);
    CHECK(returned->Release() == MMS_OK && !*returned && x.subscriptions.empty()); returned.reset();
    // Re-querying a borrowed service cannot redirect an existing owner's API.
    Host replacement(UINT64_C(0x3700000000000000));
    CHECK(service.Subscribe(x.rawBinding, original) == MMS_OK); const auto retained = original.get();
    CHECK(sdk::RefreshSubscriptionServices::Query(replacement.host, service) == MMS_OK && service.api().context == &replacement);
    CHECK(original.State(state) == MMS_OK && state.subscription == retained && state.binding == x.rawBinding);
    CHECK(original.Release() == MMS_OK && !original && x.subscriptions.empty() && replacement.releaseCalls == 0);
    CHECK(x.arguments && replacement.arguments && !x.unexpected && !replacement.unexpected);
}

static void DetachAndDestructors() {
    Host x; auto service = x.Services(); MmsHandle handle = 0;
    {
        sdk::RefreshSubscription subscription; CHECK(service.Subscribe(x.rawBinding, subscription) == MMS_OK);
        handle = subscription.get(); const auto calls = x.releaseCalls;
        CHECK(subscription.detach() == handle && !subscription && x.releaseCalls == calls && x.subscriptions.count(handle) == 1);
    }
    CHECK(x.subscriptions.count(handle) == 1 && x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    const auto beforeScope = x.releaseCalls;
    {
        sdk::RefreshSubscription subscription; CHECK(service.Subscribe(x.rawBinding, subscription) == MMS_OK); handle = subscription.get();
    }
    CHECK(x.releaseCalls == beforeScope + 1 && !x.subscriptions.count(handle));
    {
        sdk::RefreshSubscription subscription; CHECK(service.Subscribe(x.rawBinding, subscription) == MMS_OK); handle = subscription.get();
        x.releaseResult = MMS_E_BUSY;
    }
    CHECK(x.subscriptions.count(handle) == 1); x.releaseResult = MMS_OK;
    CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    for (unsigned mode = 1; mode <= 3; ++mode) {
        const auto before = x.releaseCalls;
        { sdk::RefreshSubscription subscription; CHECK(service.Subscribe(x.rawBinding, subscription) == MMS_OK);
          handle = subscription.get(); x.throwRelease = mode; }
        CHECK(x.releaseCalls == before + 1 && x.subscriptions.count(handle) == 1);
        x.throwRelease = 0; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    {
        auto owner = std::make_unique<sdk::RefreshSubscription>(); CHECK(service.Subscribe(x.rawBinding, *owner) == MMS_OK);
        handle = owner->get(); const auto before = x.releaseCalls;
        std::thread worker([owned = std::move(owner)]() mutable { owned.reset(); }); worker.join();
        CHECK(!owner && x.releaseCalls == before && x.subscriptions.count(handle) == 1 && !x.wrongThreadCalls);
        CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    {
        sdk::RefreshSubscription owner; CHECK(service.Subscribe(x.rawBinding, owner) == MMS_OK); handle = owner.get();
        MmsHandle detached = 0; MmsResult emptyRelease = MMS_E_FAILED; const auto before = x.releaseCalls;
        std::thread worker([&] { detached = owner.detach(); emptyRelease = owner.Release(); }); worker.join();
        CHECK(detached == handle && !owner && emptyRelease == MMS_OK && x.releaseCalls == before && x.subscriptions.count(handle) == 1);
        CHECK(x.table.unsubscribe(x.table.context, detached) == MMS_OK);
    }
    CHECK(x.subscriptions.empty() && x.arguments && !x.wrongThreadCalls && !x.unexpected);
}

static void ExistingDataBindingOverload() {
    Host x; auto service = x.Services(); sdk::DataServices data;
    CHECK(sdk::DataServices::Query(x.host, data) == MMS_OK && data.api().context == &x);
    sdk::DataSource source; sdk::DataBinding binding, empty; sdk::RefreshSubscription subscription;
    const auto initial = x.subscribeCalls;
    CHECK(service.Subscribe(empty, subscription) == MMS_E_NOT_READY && !subscription && x.subscribeCalls == initial);
    CHECK(data.RegisterSource(sdk::Text("example.sdk.subscription.data"), 64, source) == MMS_OK);
    CHECK(data.Bind(x.Target(), source, 64, binding) == MMS_OK && binding);
    const auto originalBinding = binding.get(), originalSource = source.get();
    const auto sources = x.sources.size(), bindings = x.bindings.size();
    CHECK(x.sourceRegistrations == 1 && x.bindingRegistrations == 1 && x.lastBinding.source == originalSource);
    CHECK(service.Subscribe(binding, subscription) == MMS_OK && subscription && binding.get() == originalBinding);
    CHECK(x.lastDesc.binding == originalBinding && x.subscriptions.at(subscription.get()) == originalBinding);
    CHECK(x.lastDesc.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) && x.sources.size() == sources && x.bindings.size() == bindings);
    CHECK(x.sourceRegistrations == 1 && x.bindingRegistrations == 1 && x.dataQueries == 1 && x.serviceQueries == 1);
    MmsUiRefreshSubscriptionStateV1 state{}; CHECK(subscription.State(state) == MMS_OK && state.binding == originalBinding);
    const auto calls = x.subscribeCalls;
    CHECK(service.Subscribe(binding, subscription, MMS_UI_REFRESH_DATA) == MMS_E_BUSY && x.subscribeCalls == calls);
    CHECK(binding.Release() == MMS_E_BUSY && binding.get() == originalBinding);
    CHECK(source.Release() == MMS_E_BUSY && source.get() == originalSource);
    CHECK(subscription.Release() == MMS_OK && binding.get() == originalBinding && source.get() == originalSource);
    MmsResult threadSubscribe = MMS_OK; sdk::RefreshSubscription workerOutput;
    std::thread worker([&] { threadSubscribe = service.Subscribe(binding, workerOutput); }); worker.join();
    CHECK(threadSubscribe == MMS_E_THREAD && !workerOutput && x.subscribeCalls == calls);
    CHECK(service.Subscribe(binding, subscription, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE) == MMS_OK);
    CHECK(x.lastDesc.binding == originalBinding && x.lastDesc.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE));
    CHECK(subscription.Release() == MMS_OK && binding.Release() == MMS_OK && source.Release() == MMS_OK);
    CHECK(x.bindings.size() == 1 && x.bindings.count(x.rawBinding) == 1 && x.sources.size() == 1 && x.sources.count(x.rawSource) == 1);
    CHECK(x.subscriptions.empty() && x.arguments && !x.wrongThreadCalls && !x.unexpected);
}
} // namespace cpp_refresh_subscription_checks
static void CppRefreshSubscriptionChecks() {
    cpp_refresh_subscription_checks::NegotiationAndEmpty();
    cpp_refresh_subscription_checks::RegistrationAndState();
    cpp_refresh_subscription_checks::ReleaseAndMove();
    cpp_refresh_subscription_checks::DetachAndDestructors();
    cpp_refresh_subscription_checks::ExistingDataBindingOverload();
}
