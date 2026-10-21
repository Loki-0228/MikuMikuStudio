#include <mms/cpp/property_refresh.hpp>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

namespace cpp_property_refresh_checks {
namespace sdk = mms::sdk;

template<class T> static bool HeaderIs(const T* value) {
    return value && value->h.size == sizeof(T) && value->h.major == 1 && value->h.minor == MMS_ABI_MINOR;
}
template<class T> static T Sentinel() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> static bool Same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
static bool TextIs(MmsString value, std::string_view expected) {
    return value.bytes == expected.size() && value.data && std::memcmp(value.data, expected.data(), expected.size()) == 0;
}
static void Throw(unsigned mode) {
    if (mode == 1) throw std::bad_alloc();
    if (mode == 2) throw std::length_error("property fake length");
    if (mode == 3) throw std::runtime_error("property fake runtime");
}
static MmsResult ExceptionResult(unsigned mode) { return mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED; }

// An independent C host: it owns no production registry, property reader,
// history, binding or refresh queue. Only exact references and calls cross
// the boundary; failed callbacks intentionally scribble candidate outputs.
struct Host {
    const std::thread::id ui = std::this_thread::get_id();
    const MmsHandle prefix, input, property;
    MmsHandle next;
    MmsHostV1 host{};
    MmsUiPropertyRefreshApiV1 table{};
    MmsUiRefreshApiV1 refresh{};
    MmsUiPropertyRefreshSubscriptionDescV1 lastDesc{};
    MmsUiPropertyRefV1 notified{};
    std::map<MmsHandle, MmsUiPropertyRefreshSubscriptionDescV1> subscriptions;
    unsigned queries = 0, refreshQueries = 0, caps = 0, refs = 0, targets = 0, subscribes = 0, notifications = 0, states = 0, releases = 0;
    unsigned workerNotifications = 0, workerStates = 0, wrongThread = 0, unexpected = 0;
    unsigned throwQuery = 0, throwCaps = 0, throwRef = 0, throwTarget = 0, throwSubscribe = 0, throwNotify = 0, throwState = 0, throwRelease = 0;
    MmsResult queryResult = MMS_OK, capsResult = MMS_OK, refResult = MMS_OK, targetResult = MMS_OK;
    MmsResult subscribeResult = MMS_OK, notifyResult = MMS_OK, stateResult = MMS_OK, releaseResult = MMS_OK;
    bool arguments = true, zeroHandle = false, badCaps = false, badRef = false, badTarget = false, badState = false;
    std::uint32_t features = 0, targetKinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT;
    std::uint32_t targetScopes = MMS_UI_REFRESH_SELF;
    explicit Host(MmsHandle seed = UINT64_C(0x1800000000000000))
        : prefix(seed), input(seed + 11), property(seed + 29), next(seed + 101) {
        table.h = sdk::Header<MmsUiPropertyRefreshApiV1>(); table.context = this;
        table.getCapabilities = Caps; table.getPropertyRef = Ref; table.subscribe = Subscribe;
        table.notify = Notify; table.query = State; table.unsubscribe = Unsubscribe;
        refresh.h = sdk::Header<MmsUiRefreshApiV1>(); refresh.context = this;
        refresh.getCapabilities = [](void* p, MmsUiRefreshCapabilitiesV1*) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.getTarget = Target;
        refresh.request = [](void* p, const MmsUiRefreshRequestV1*, MmsHandle*) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.query = [](void* p, MmsHandle, MmsUiRefreshStateV1*) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.cancel = [](void* p, MmsHandle) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.release = refresh.cancel;
        host.h = sdk::Header<MmsHostV1>(); host.context = this; host.queryService = Query;
    }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    bool Ui() { if (std::this_thread::get_id() == ui) return true; ++wrongThread; return false; }
    MmsUiTargetRefV1 TargetRef() const {
        return {sdk::Header<MmsUiTargetRefV1>(), input, prefix + 37, prefix + 43};
    }
    MmsUiPropertyRefV1 PropertyRef() const {
        return {sdk::Header<MmsUiPropertyRefV1>(), property, prefix + 53, prefix + 43};
    }
    MmsUiPropertyRefreshSubscriptionDescV1 Desc(std::uint32_t flags = 0) const {
        MmsUiPropertyRefreshSubscriptionDescV1 result{}; result.h = sdk::Header<MmsUiPropertyRefreshSubscriptionDescV1>();
        result.target = TargetRef(); result.property = PropertyRef();
        result.kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT; result.flags = flags; return result;
    }
    static MmsResult MMS_CALL Query(void* p, MmsString id, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        auto& x = *static_cast<Host*>(p); if (!x.Ui()) return MMS_E_THREAD;
        if (TextIs(id, "mms.ui.refresh.property")) {
            ++x.queries; x.arguments &= major == 1 && minor == 0 && bytes == sizeof(x.table) && out;
            if (!out || bytes != sizeof(x.table)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.table, sizeof(x.table)); Throw(x.throwQuery); return x.queryResult;
        }
        if (TextIs(id, "mms.ui.refresh")) {
            ++x.refreshQueries; x.arguments &= major == 1 && minor == 0 && bytes == sizeof(x.refresh) && out;
            if (!out || bytes != sizeof(x.refresh)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.refresh, sizeof(x.refresh)); return MMS_OK;
        }
        ++x.unexpected; return MMS_E_NOT_FOUND;
    }
    static MmsResult MMS_CALL Caps(void* p, MmsUiPropertyRefreshCapabilitiesV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.caps; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiPropertyRefreshCapabilitiesV1>();
        out->maxSubscriptionsPerPlugin = 37; out->supportedKinds = x.targetKinds;
        out->supportedFlags = x.features;
        if (x.badCaps) out->h.size = sizeof(*out) - 1;
        Throw(x.throwCaps); return x.capsResult;
    }
    static MmsResult MMS_CALL Ref(void* p, MmsHandle property, MmsUiPropertyRefV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.refs; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = x.PropertyRef(); if (x.badRef) out->h.major = 2;
        Throw(x.throwRef); if (x.refResult) return x.refResult;
        return property == x.property ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Target(void* p, MmsHandle input, MmsUiRefreshTargetV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.targets; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiRefreshTargetV1>(); out->target = x.TargetRef();
        out->ready = 1; out->supportedKinds = x.targetKinds; out->supportedScopes = x.targetScopes;
        if (x.badTarget) out->target.h.major = 2;
        Throw(x.throwTarget); if (x.targetResult) return x.targetResult;
        return input == x.input ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Subscribe(void* p, const MmsUiPropertyRefreshSubscriptionDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(p); ++x.subscribes; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out;
        if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        x.lastDesc = *desc; *out = x.prefix + 999;
        Throw(x.throwSubscribe); if (x.subscribeResult) return x.subscribeResult;
        if (!Same(desc->target, x.TargetRef()) || !Same(desc->property, x.PropertyRef())) return MMS_E_STALE;
        if (!(desc->kinds & MMS_UI_REFRESH_DATA) || (desc->kinds & ~x.targetKinds) || desc->reserved) return MMS_E_ARGUMENT;
        if (desc->flags & ~x.features) return MMS_E_UNSUPPORTED;
        if (x.zeroHandle) { *out = 0; return MMS_OK; }
        const auto handle = x.next++; x.subscriptions.emplace(handle, *desc); *out = handle; return MMS_OK;
    }
    static MmsResult MMS_CALL Notify(void* p, const MmsUiPropertyRefV1* property) {
        auto& x = *static_cast<Host*>(p); ++x.notifications;
        if (std::this_thread::get_id() != x.ui) ++x.workerNotifications;
        if (!property) return MMS_E_ARGUMENT; x.notified = *property;
        Throw(x.throwNotify); if (x.notifyResult) return x.notifyResult;
        return Same(*property, x.PropertyRef()) ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL State(void* p, MmsHandle handle, MmsUiPropertyRefreshSubscriptionStateV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.states;
        if (std::this_thread::get_id() != x.ui) ++x.workerStates;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiPropertyRefreshSubscriptionStateV1>(); out->subscription = handle;
        out->target = x.TargetRef(); out->property = x.PropertyRef();
        out->latestSequence = 31; out->submittedSequence = 29; out->completedSequence = 23;
        out->lastResult = MMS_E_BUSY; out->pending = 1; out->waitingFor = MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY;
        if (x.badState) out->h.size = sizeof(*out) - 1;
        Throw(x.throwState); if (x.stateResult) return x.stateResult;
        return x.subscriptions.count(handle) ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Unsubscribe(void* p, MmsHandle handle) {
        auto& x = *static_cast<Host*>(p); ++x.releases; if (!x.Ui()) return MMS_E_THREAD;
        Throw(x.throwRelease); if (x.releaseResult) return x.releaseResult;
        return x.subscriptions.erase(handle) ? MMS_OK : MMS_E_STALE;
    }
    sdk::PropertyRefreshServices Services() {
        sdk::PropertyRefreshServices service; CHECK(sdk::PropertyRefreshServices::Query(host, service) == MMS_OK); return service;
    }
    sdk::RefreshServices RefreshServices() {
        sdk::RefreshServices service; CHECK(sdk::RefreshServices::Query(host, service) == MMS_OK); return service;
    }
};

static void Negotiation() {
    static_assert(sizeof(MmsUiPropertyRefV1) == 32);
    static_assert(sizeof(MmsUiPropertyRefreshCapabilitiesV1) == 32);
    static_assert(sizeof(MmsUiPropertyRefreshSubscriptionDescV1) == 88);
    static_assert(sizeof(MmsUiPropertyRefreshSubscriptionStateV1) == 120);
    static_assert(sizeof(MmsUiPropertyRefreshApiV1) == 64);
    static_assert(!std::is_copy_constructible_v<sdk::PropertyRefreshSubscription>);
    static_assert(!std::is_copy_assignable_v<sdk::PropertyRefreshSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::PropertyRefreshSubscription>);
    static_assert(!std::is_move_assignable_v<sdk::PropertyRefreshSubscription>);
    static_assert(std::is_nothrow_destructible_v<sdk::PropertyRefreshSubscription>);
    Host a, b(UINT64_C(0x2800000000000000)); auto service = a.Services(); const auto original = service.api();
    sdk::PropertyRefreshServices empty; sdk::PropertyRefreshSubscription subscription;
    auto caps = Sentinel<MmsUiPropertyRefreshCapabilitiesV1>(); const auto oldCaps = caps;
    auto ref = Sentinel<MmsUiPropertyRefV1>(); const auto oldRef = ref;
    auto state = Sentinel<MmsUiPropertyRefreshSubscriptionStateV1>(); const auto oldState = state;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY);
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && Same(caps, oldCaps));
    CHECK(empty.GetPropertyRef(a.property, ref) == MMS_E_NOT_READY && Same(ref, oldRef));
    CHECK(empty.Notify(a.PropertyRef()) == MMS_E_NOT_READY);
    CHECK(empty.Subscribe(a.Desc(), subscription) == MMS_E_NOT_READY && !subscription);
    CHECK(subscription.State(state) == MMS_E_NOT_READY && Same(state, oldState));
    CHECK(subscription.Release() == MMS_OK && !subscription.detach());
    for (const auto error : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY, MMS_E_FAILED}) {
        b.queryResult = error; CHECK(sdk::PropertyRefreshServices::Query(b.host, service) == error && Same(service.api(), original));
    }
    b.queryResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        b.throwQuery = mode; CHECK(sdk::PropertyRefreshServices::Query(b.host, service) == ExceptionResult(mode) && Same(service.api(), original));
    }
    b.throwQuery = 0; const auto calls = b.queries; auto host = b.host;
    host.h.size = sizeof(host) - 1; CHECK(sdk::PropertyRefreshServices::Query(host, service) == MMS_E_VERSION && Same(service.api(), original));
    host = b.host; host.h.major = 2; CHECK(sdk::PropertyRefreshServices::Query(host, service) == MMS_E_VERSION && Same(service.api(), original));
    host = b.host; host.queryService = nullptr; CHECK(sdk::PropertyRefreshServices::Query(host, service) == MMS_E_ARGUMENT && Same(service.api(), original));
    CHECK(b.queries == calls);
    const auto table = b.table;
    b.table.h.size = sizeof(table) - 1; CHECK(sdk::PropertyRefreshServices::Query(b.host, service) == MMS_E_VERSION && Same(service.api(), original));
    b.table = table; b.table.h.major = 2; CHECK(sdk::PropertyRefreshServices::Query(b.host, service) == MMS_E_VERSION && Same(service.api(), original));
    for (unsigned field = 0; field != 6; ++field) {
        b.table = table;
        if (field == 0) b.table.getCapabilities = nullptr;
        if (field == 1) b.table.getPropertyRef = nullptr;
        if (field == 2) b.table.subscribe = nullptr;
        if (field == 3) b.table.notify = nullptr;
        if (field == 4) b.table.query = nullptr;
        if (field == 5) b.table.unsubscribe = nullptr;
        CHECK(sdk::PropertyRefreshServices::Query(b.host, service) == MMS_E_UNSUPPORTED && Same(service.api(), original));
    }
    b.table = table; b.table.h.size += 32; b.table.h.minor = 9;
    host = b.host; host.h.size += 16; host.h.minor = 7;
    CHECK(sdk::PropertyRefreshServices::Query(host, empty) == MMS_OK && empty.ready() && empty.api().context == &b && empty.api().h.minor == 9);
    CHECK(service.api().context == &a && service.CheckThread() == MMS_OK && a.arguments && b.arguments);
}

static void BorrowedOutputsAndExceptions() {
    Host x; auto service = x.Services();
    const auto capSentinel = Sentinel<MmsUiPropertyRefreshCapabilitiesV1>();
    const auto refSentinel = Sentinel<MmsUiPropertyRefV1>();
    auto caps = capSentinel; auto ref = refSentinel;
    CHECK(service.Capabilities(caps) == MMS_OK && HeaderIs(&caps) && caps.maxSubscriptionsPerPlugin == 37 &&
        caps.supportedKinds == x.targetKinds && !caps.supportedFlags && !caps.flags && !caps.reserved);
    CHECK(service.GetPropertyRef(x.property, ref) == MMS_OK && Same(ref, x.PropertyRef()));
    CHECK(ref.property == x.property && ref.registrationGeneration > UINT32_MAX && ref.projectEpoch > UINT32_MAX);
    const auto refs = x.refs;
    CHECK(service.GetPropertyRef(0, ref) == MMS_E_ARGUMENT && x.refs == refs);
    for (const auto error : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.capsResult = error; caps = capSentinel; CHECK(service.Capabilities(caps) == error && Same(caps, capSentinel));
        x.refResult = error; ref = refSentinel; CHECK(service.GetPropertyRef(x.property, ref) == error && Same(ref, refSentinel));
        x.notifyResult = error; CHECK(service.Notify(x.PropertyRef()) == error && Same(x.notified, x.PropertyRef()));
    }
    x.capsResult = x.refResult = x.notifyResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwCaps = mode; caps = capSentinel; CHECK(service.Capabilities(caps) == ExceptionResult(mode) && Same(caps, capSentinel));
        x.throwRef = mode; ref = refSentinel; CHECK(service.GetPropertyRef(x.property, ref) == ExceptionResult(mode) && Same(ref, refSentinel));
        x.throwNotify = mode; CHECK(service.Notify(x.PropertyRef()) == ExceptionResult(mode));
    }
    x.throwCaps = x.throwRef = x.throwNotify = 0;
    x.badCaps = true; caps = capSentinel; CHECK(service.Capabilities(caps) == MMS_E_VERSION && Same(caps, capSentinel)); x.badCaps = false;
    x.badRef = true; ref = refSentinel; CHECK(service.GetPropertyRef(x.property, ref) == MMS_E_VERSION && Same(ref, refSentinel)); x.badRef = false;
    ref = refSentinel; CHECK(service.GetPropertyRef(x.property + 1, ref) == MMS_E_STALE && Same(ref, refSentinel));
    auto stale = x.PropertyRef(); ++stale.registrationGeneration;
    CHECK(service.Notify(stale) == MMS_E_STALE && Same(x.notified, stale));
    const auto before = x.subscribes; sdk::PropertyRefreshSubscription subscription;
    for (const auto error : {MMS_E_ARGUMENT, MMS_E_BUSY, MMS_E_STALE, MMS_E_FAILED}) {
        x.subscribeResult = error; CHECK(service.Subscribe(x.Desc(), subscription) == error && !subscription && x.subscriptions.empty());
    }
    x.subscribeResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwSubscribe = mode; CHECK(service.Subscribe(x.Desc(), subscription) == ExceptionResult(mode) && !subscription && x.subscriptions.empty());
    }
    x.throwSubscribe = 0; x.zeroHandle = true;
    CHECK(service.Subscribe(x.Desc(), subscription) == MMS_E_FAILED && !subscription && x.subscriptions.empty()); x.zeroHandle = false;
    CHECK(x.subscribes > before && !x.wrongThread && !x.unexpected && x.arguments);
}

static void RawAndInputConvenience() {
    Host x; auto service = x.Services(); auto refresh = x.RefreshServices();
    sdk::PropertyRefreshSubscription subscription;
    const auto raw = x.Desc(); CHECK(service.Subscribe(raw, subscription) == MMS_OK && subscription.get() > UINT32_MAX && Same(raw, x.lastDesc));
    const auto handle = subscription.get(); const auto calls = x.subscribes;
    CHECK(service.WatchInput(refresh, x.input, x.property, subscription) == MMS_E_BUSY && subscription.get() == handle && x.subscribes == calls && !x.refs && !x.targets);
    CHECK(service.Subscribe(raw, subscription) == MMS_E_BUSY && x.subscribes == calls);
    CHECK(subscription.Release() == MMS_OK);
    auto stale = raw; ++stale.property.registrationGeneration;
    CHECK(service.Subscribe(stale, subscription) == MMS_E_STALE && !subscription && Same(x.lastDesc, stale));
    for (const auto kinds : {0u, MMS_UI_REFRESH_REPAINT, MMS_UI_REFRESH_DATA | 0x80000000u}) {
        auto invalid = raw; invalid.kinds = kinds;
        CHECK(service.Subscribe(invalid, subscription) == MMS_E_ARGUMENT && !subscription && Same(x.lastDesc, invalid));
    }
    CHECK(service.SubscribeInput(refresh, x.input, x.property, subscription) == MMS_OK && Same(x.lastDesc, raw));
    CHECK(x.targets == 1 && x.refs == 1 && x.refreshQueries == 1 && x.queries == 1 && !x.caps && !x.lastDesc.flags && !x.lastDesc.reserved);
    CHECK(subscription.Release() == MMS_OK);
    CHECK(service.WatchInput(refresh, x.input, x.property, subscription) == MMS_OK && Same(x.lastDesc, raw) && !x.caps);
    CHECK(subscription.Release() == MMS_OK);
    const auto flags = MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY;
    auto follow = x.Desc(flags); const auto subscriptions = x.subscribes;
    CHECK(service.Subscribe(follow, subscription) == MMS_E_UNSUPPORTED && !subscription && x.caps == 1 && x.subscribes == subscriptions);
    x.features = flags;
    CHECK(service.WatchInput(refresh, x.input, x.property, subscription, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT, flags) == MMS_OK);
    CHECK(x.lastDesc.flags == flags && x.lastDesc.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT) && Same(x.lastDesc.target, x.TargetRef()) && Same(x.lastDesc.property, x.PropertyRef()));
    CHECK(subscription.Release() == MMS_OK);
    x.capsResult = MMS_E_BUSY; const auto before = x.subscribes;
    CHECK(service.Subscribe(follow, subscription) == MMS_E_BUSY && !subscription && x.subscribes == before); x.capsResult = MMS_OK;
    x.throwCaps = 1; CHECK(service.Subscribe(follow, subscription) == MMS_E_MEMORY && !subscription && x.subscribes == before); x.throwCaps = 0;
    for (const auto fields : {std::array<MmsHandle, 2>{0, x.property}, std::array<MmsHandle, 2>{x.input, 0}}) {
        const auto targets = x.targets, refs = x.refs;
        CHECK(service.SubscribeInput(refresh, fields[0], fields[1], subscription) == MMS_E_ARGUMENT && x.targets == targets && x.refs == refs && x.subscribes == before);
    }
    for (const auto error : {MMS_E_NOT_READY, MMS_E_STALE, MMS_E_FAILED}) {
        x.targetResult = error; const auto refs = x.refs;
        CHECK(service.SubscribeInput(refresh, x.input, x.property, subscription) == error && x.refs == refs && x.subscribes == before);
    }
    x.targetResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwTarget = mode; const auto refs = x.refs;
        CHECK(service.WatchInput(refresh, x.input, x.property, subscription) == ExceptionResult(mode) && x.refs == refs && x.subscribes == before);
    }
    x.throwTarget = 0; x.refResult = MMS_E_STALE;
    CHECK(service.SubscribeInput(refresh, x.input, x.property, subscription) == MMS_E_STALE && x.subscribes == before); x.refResult = MMS_OK;
    x.badTarget = true; const auto beforeBadTarget = x.refs;
    CHECK(service.WatchInput(refresh, x.input, x.property, subscription) == MMS_E_VERSION && x.refs == beforeBadTarget && x.subscribes == before); x.badTarget = false;
    x.targetKinds = MMS_UI_REFRESH_REPAINT; const auto refs = x.refs;
    CHECK(service.WatchInput(refresh, x.input, x.property, subscription) == MMS_E_UNSUPPORTED && x.refs == refs && x.subscribes == before);
    x.targetKinds |= MMS_UI_REFRESH_DATA; x.targetScopes = MMS_UI_REFRESH_SUBTREE;
    CHECK(service.WatchInput(refresh, x.input, x.property, subscription) == MMS_E_UNSUPPORTED && x.refs == refs && x.subscribes == before);
    CHECK(x.subscriptions.empty() && !x.wrongThread && !x.unexpected && x.arguments);
}

static void WorkerAndOwnership() {
    Host x; auto service = x.Services(); auto refresh = x.RefreshServices(); sdk::PropertyRefreshSubscription owner;
    CHECK(service.Subscribe(x.Desc(), owner) == MMS_OK); const auto handle = owner.get();
    MmsUiPropertyRefreshSubscriptionStateV1 state{};
    CHECK(owner.State(state) == MMS_OK && state.subscription == handle && Same(state.target, x.TargetRef()) && Same(state.property, x.PropertyRef()));
    CHECK(state.latestSequence == 31 && state.submittedSequence == 29 && state.completedSequence == 23 && state.lastResult == MMS_E_BUSY && state.pending == 1 && state.waitingFor == MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY);
    const auto stateSentinel = Sentinel<MmsUiPropertyRefreshSubscriptionStateV1>();
    for (const auto error : {MMS_E_STALE, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.stateResult = error; state = stateSentinel; CHECK(owner.State(state) == error && Same(state, stateSentinel) && owner.get() == handle);
    }
    x.stateResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwState = mode; state = stateSentinel; CHECK(owner.State(state) == ExceptionResult(mode) && Same(state, stateSentinel) && owner.get() == handle);
    }
    x.throwState = 0; x.badState = true; state = stateSentinel;
    CHECK(owner.State(state) == MMS_E_VERSION && Same(state, stateSentinel) && owner.get() == handle); x.badState = false;
    MmsResult notified = MMS_E_FAILED, queried = MMS_E_FAILED, capResult = MMS_OK, refResult = MMS_OK, subscribed = MMS_OK, watched = MMS_OK, released = MMS_OK;
    sdk::PropertyRefreshSubscription unused;
    auto caps = Sentinel<MmsUiPropertyRefreshCapabilitiesV1>(); const auto oldCaps = caps;
    auto ref = Sentinel<MmsUiPropertyRefV1>(); const auto oldRef = ref;
    const auto capCalls = x.caps, refCalls = x.refs, subscribeCalls = x.subscribes, releaseCalls = x.releases, targets = x.targets;
    std::thread worker([&] {
        auto copied = service; auto movedService = std::move(copied);
        notified = movedService.Notify(x.PropertyRef()); queried = owner.State(state);
        capResult = movedService.Capabilities(caps); refResult = service.GetPropertyRef(x.property, ref);
        subscribed = service.Subscribe(x.Desc(), unused); watched = service.WatchInput(refresh, x.input, x.property, unused); released = owner.Release();
    }); worker.join();
    CHECK(notified == MMS_OK && queried == MMS_OK && x.workerNotifications == 1 && x.workerStates == 1 && Same(x.notified, x.PropertyRef()) && state.subscription == handle);
    CHECK(capResult == MMS_E_THREAD && refResult == MMS_E_THREAD && subscribed == MMS_E_THREAD && watched == MMS_E_THREAD && released == MMS_E_THREAD);
    CHECK(Same(caps, oldCaps) && Same(ref, oldRef) && x.caps == capCalls && x.refs == refCalls && x.subscribes == subscribeCalls && x.releases == releaseCalls && x.targets == targets && !x.wrongThread && !unused);
    sdk::PropertyRefreshSubscription moved(std::move(owner));
    CHECK(!owner && !owner.get() && moved.get() == handle && owner.Release() == MMS_OK);
    for (const auto error : {MMS_E_BUSY, MMS_E_THREAD, MMS_E_NOT_FOUND, MMS_E_FAILED, MMS_E_MEMORY}) {
        x.releaseResult = error; CHECK(moved.Release() == error && moved.get() == handle && x.subscriptions.count(handle));
        CHECK(moved.State(state) == MMS_OK && state.subscription == handle);
    }
    x.releaseResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwRelease = mode; CHECK(moved.Release() == ExceptionResult(mode) && moved.get() == handle && x.subscriptions.count(handle));
    }
    x.throwRelease = 0;
    CHECK(moved.Release() == MMS_OK && !moved && x.subscriptions.empty());
    CHECK(service.Subscribe(x.Desc(), owner) == MMS_OK); const auto crossing = owner.get();
    std::unique_ptr<sdk::PropertyRefreshSubscription> returned; MmsResult crossingRelease = MMS_OK;
    const auto releases = x.releases;
    std::thread transfer([&] { returned = std::make_unique<sdk::PropertyRefreshSubscription>(std::move(owner)); crossingRelease = returned->Release(); queried = returned->State(state); }); transfer.join();
    CHECK(!owner && returned && returned->get() == crossing && crossingRelease == MMS_E_THREAD && queried == MMS_OK && x.releases == releases && !x.wrongThread);
    Host replacement(UINT64_C(0x3800000000000000));
    CHECK(sdk::PropertyRefreshServices::Query(replacement.host, service) == MMS_OK && service.api().context == &replacement);
    CHECK(returned->State(state) == MMS_OK && state.subscription == crossing && Same(state.property, x.PropertyRef()));
    CHECK(returned->Release() == MMS_OK && !*returned && x.subscriptions.empty() && !replacement.releases);
    CHECK(x.arguments && replacement.arguments && !x.unexpected && !replacement.unexpected);
}

static void StaleDetachAndDestructors() {
    Host x; auto service = x.Services(); sdk::PropertyRefreshSubscription owner;
    CHECK(service.Subscribe(x.Desc(), owner) == MMS_OK); auto handle = owner.get(); x.subscriptions.erase(handle);
    auto state = Sentinel<MmsUiPropertyRefreshSubscriptionStateV1>(); const auto oldState = state;
    CHECK(owner.State(state) == MMS_E_STALE && Same(state, oldState) && owner.get() == handle);
    CHECK(owner.Release() == MMS_OK && !owner); const auto releases = x.releases;
    CHECK(owner.Release() == MMS_OK && !owner.detach() && x.releases == releases);
    {
        sdk::PropertyRefreshSubscription scoped; CHECK(service.Subscribe(x.Desc(), scoped) == MMS_OK); handle = scoped.get();
        CHECK(scoped.detach() == handle && !scoped && x.subscriptions.count(handle) && x.releases == releases);
    }
    CHECK(x.subscriptions.count(handle) && x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    const auto before = x.releases;
    { sdk::PropertyRefreshSubscription scoped; CHECK(service.Subscribe(x.Desc(), scoped) == MMS_OK); handle = scoped.get(); }
    CHECK(x.releases == before + 1 && !x.subscriptions.count(handle));
    { sdk::PropertyRefreshSubscription scoped; CHECK(service.Subscribe(x.Desc(), scoped) == MMS_OK); handle = scoped.get(); x.releaseResult = MMS_E_BUSY; }
    CHECK(x.subscriptions.count(handle)); x.releaseResult = MMS_OK; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    for (unsigned mode = 1; mode <= 3; ++mode) {
        { sdk::PropertyRefreshSubscription scoped; CHECK(service.Subscribe(x.Desc(), scoped) == MMS_OK); handle = scoped.get(); x.throwRelease = mode; }
        CHECK(x.subscriptions.count(handle)); x.throwRelease = 0; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    {
        auto scoped = std::make_unique<sdk::PropertyRefreshSubscription>(); CHECK(service.Subscribe(x.Desc(), *scoped) == MMS_OK); handle = scoped->get(); const auto calls = x.releases;
        std::thread worker([owned = std::move(scoped)]() mutable { owned.reset(); }); worker.join();
        CHECK(!scoped && x.releases == calls && x.subscriptions.count(handle) && !x.wrongThread);
        CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    CHECK(service.Subscribe(x.Desc(), owner) == MMS_OK); handle = owner.get(); MmsHandle detached = 0; const auto calls = x.releases;
    std::thread worker([&] { detached = owner.detach(); }); worker.join();
    CHECK(detached == handle && !owner && x.releases == calls && x.subscriptions.count(handle));
    CHECK(x.table.unsubscribe(x.table.context, detached) == MMS_OK && x.subscriptions.empty() && !x.wrongThread && !x.unexpected && x.arguments);
}
} // namespace cpp_property_refresh_checks
static void CppPropertyRefreshChecks() {
    cpp_property_refresh_checks::Negotiation();
    cpp_property_refresh_checks::BorrowedOutputsAndExceptions();
    cpp_property_refresh_checks::RawAndInputConvenience();
    cpp_property_refresh_checks::WorkerAndOwnership();
    cpp_property_refresh_checks::StaleDetachAndDestructors();
}
