#include <mms/cpp/scoped_refresh_subscription.hpp>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

namespace cpp_scoped_refresh_subscription_checks {
namespace sdk = mms::sdk;
template<class T> static bool HeaderIs(const T* value) {
    return value && value->h.size == sizeof(T) && value->h.major == 1 && value->h.minor == 0;
}
template<class T> static T Sentinel() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> static bool Same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
static bool TextIs(MmsString value, std::string_view expected) {
    return value.bytes == expected.size() && value.data && std::memcmp(value.data, expected.data(), expected.size()) == 0;
}
static void Throw(unsigned mode) {
    if (mode == 1) throw std::bad_alloc();
    if (mode == 2) throw std::length_error("scoped fake length");
    if (mode == 3) throw std::runtime_error("scoped fake runtime");
}
static MmsResult Error(unsigned mode) { return mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED; }
static void Corrupt(MmsHeader& h, unsigned mode) {
    if (mode == 1) --h.size;
    if (mode == 2) h.major = 2;
    if (mode == 3) h.minor = 1;
}

// Independent protocol host. It has one handle pool and records C arguments;
// it contains no production queue, registry, source publication or graph code.
struct Host {
    const std::thread::id ui = std::this_thread::get_id();
    const MmsHandle prefix, binding, seed;
    MmsHandle next;
    MmsHostV1 host{};
    MmsUiRefreshSubscriptionApiV1_1 table{};
    MmsUiRefreshSubscriptionApiV1 base{};
    MmsUiRefreshApiV1 refresh{};
    MmsUiRefreshScopedSubscriptionDescV1 last{};
    std::map<MmsHandle, MmsUiRefreshScopedSubscriptionDescV1> subscriptions;
    unsigned queries = 0, legacyQueries = 0, refreshQueries = 0, caps = 0, scopedSubscribes = 0, legacySubscribes = 0;
    unsigned scopedStates = 0, baseStates = 0, workerScopedStates = 0, workerBaseStates = 0, releases = 0, targets = 0, wrongThread = 0, unexpected = 0;
    unsigned throwQuery = 0, throwCaps = 0, throwSubscribe = 0, throwState = 0, throwBase = 0, throwRelease = 0, throwTarget = 0;
    unsigned badCaps = 0, badState = 0, badBase = 0, badTarget = 0;
    MmsResult queryResult = MMS_OK, capsResult = MMS_OK, subscribeResult = MMS_OK, stateResult = MMS_OK, baseResult = MMS_OK, releaseResult = MMS_OK, targetResult = MMS_OK;
    bool arguments = true, legacyOnly = false, zeroHandle = false;
    uint32_t scopes = MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS;
    explicit Host(MmsHandle value = UINT64_C(0x1900000000000000))
        : prefix(value), binding(value + 11), seed(value + 29), next(value + 101) {
        table.h = {sizeof(table), 1, 1}; table.context = this;
        table.getCapabilities = Caps; table.subscribe = LegacySubscribe; table.query = BaseState; table.unsubscribe = Unsubscribe;
        table.subscribeScoped = Subscribe; table.queryScoped = State;
        base.h = {sizeof(base), 1, 0}; base.context = this;
        base.getCapabilities = Caps; base.subscribe = LegacySubscribe; base.query = BaseState; base.unsubscribe = Unsubscribe;
        refresh.h = sdk::Header<MmsUiRefreshApiV1>(); refresh.context = this; refresh.getTarget = Target;
        refresh.getCapabilities = [](void* p, MmsUiRefreshCapabilitiesV1*) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.request = [](void* p, const MmsUiRefreshRequestV1*, MmsHandle*) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.query = [](void* p, MmsHandle, MmsUiRefreshStateV1*) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.cancel = [](void* p, MmsHandle) -> MmsResult { ++static_cast<Host*>(p)->unexpected; return MMS_E_UNSUPPORTED; };
        refresh.release = refresh.cancel;
        host.h = sdk::Header<MmsHostV1>(); host.context = this; host.queryService = Query;
    }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    bool Ui() { if (std::this_thread::get_id() == ui) return true; ++wrongThread; return false; }
    MmsUiTargetRefV1 Seed() const { return {sdk::Header<MmsUiTargetRefV1>(), seed, prefix + 37, prefix + 43}; }
    MmsUiRefreshScopedSubscriptionDescV1 Desc(uint32_t scope = MMS_UI_REFRESH_SUBTREE,
        uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) const {
        MmsUiRefreshScopedSubscriptionDescV1 d{}; d.h = {sizeof(d), 1, 0};
        d.binding = binding; d.seed = Seed(); d.scope = scope; d.kinds = kinds; return d;
    }
    static MmsResult MMS_CALL Query(void* p, MmsString id, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        auto& x = *static_cast<Host*>(p); if (!x.Ui()) return MMS_E_THREAD;
        if (TextIs(id, "mms.ui.refresh.subscribe")) {
            if (minor == 0) {
                ++x.legacyQueries; x.arguments &= major == 1 && bytes == sizeof(x.base) && out;
                if (!out || bytes != sizeof(x.base)) return MMS_E_ARGUMENT;
                std::memcpy(out, &x.base, sizeof(x.base)); return MMS_OK;
            }
            ++x.queries; x.arguments &= major == 1 && minor == 1 && bytes == sizeof(x.table) && out;
            if (!out || bytes != sizeof(x.table)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.table, sizeof(x.table)); Throw(x.throwQuery);
            return x.legacyOnly ? MMS_E_VERSION : x.queryResult;
        }
        if (TextIs(id, "mms.ui.refresh")) {
            ++x.refreshQueries; x.arguments &= major == 1 && minor == 0 && bytes == sizeof(x.refresh) && out;
            if (!out || bytes != sizeof(x.refresh)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.refresh, sizeof(x.refresh)); return MMS_OK;
        }
        ++x.unexpected; return MMS_E_NOT_FOUND;
    }
    static MmsResult MMS_CALL Caps(void* p, MmsUiRefreshSubscriptionCapabilitiesV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.caps; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {{sizeof(*out), 1, 0}, 37, 15, x.scopes, 0, {0, 0}};
        Corrupt(out->h, x.badCaps); Throw(x.throwCaps); return x.capsResult;
    }
    static MmsResult MMS_CALL Subscribe(void* p, const MmsUiRefreshScopedSubscriptionDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(p); ++x.scopedSubscribes; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out; if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        x.last = *desc; *out = x.prefix + 999; Throw(x.throwSubscribe); if (x.subscribeResult) return x.subscribeResult;
        if (desc->binding != x.binding || !Same(desc->seed, x.Seed())) return MMS_E_STALE;
        if ((desc->scope != MMS_UI_REFRESH_SELF && desc->scope != MMS_UI_REFRESH_SUBTREE && desc->scope != MMS_UI_REFRESH_DEPENDENTS) ||
            !(desc->kinds & MMS_UI_REFRESH_DATA) || desc->kinds & ~15u || desc->flags || desc->reserved || desc->reservedTail[0] || desc->reservedTail[1]) return MMS_E_ARGUMENT;
        if (!(x.scopes & desc->scope)) return MMS_E_UNSUPPORTED;
        if (x.zeroHandle) { *out = 0; return MMS_OK; }
        const auto handle = x.next++; x.subscriptions.emplace(handle, *desc); *out = handle; return MMS_OK;
    }
    static MmsResult MMS_CALL LegacySubscribe(void* p, const MmsUiRefreshSubscriptionDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(p); ++x.legacySubscribes; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out; if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        if (desc->binding != x.binding) return MMS_E_STALE;
        const auto handle = x.next++; auto d = x.Desc(MMS_UI_REFRESH_SELF, desc->kinds);
        x.subscriptions.emplace(handle, d); *out = handle; return MMS_OK;
    }
    MmsUiRefreshSubscriptionStateV1 Base(MmsHandle handle) const {
        return {{sizeof(MmsUiRefreshSubscriptionStateV1), 1, 0}, handle, binding,
            prefix + 43, 31, 29, 23, MMS_E_BUSY, 1, 0};
    }
    static MmsResult MMS_CALL BaseState(void* p, MmsHandle handle, MmsUiRefreshSubscriptionStateV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.baseStates; if (std::this_thread::get_id() != x.ui) ++x.workerBaseStates;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = x.Base(handle); Corrupt(out->h, x.badBase); Throw(x.throwBase);
        if (x.baseResult) return x.baseResult; return x.subscriptions.count(handle) ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL State(void* p, MmsHandle handle, MmsUiRefreshScopedSubscriptionStateV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.scopedStates; if (std::this_thread::get_id() != x.ui) ++x.workerScopedStates;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; const auto base = x.Base(handle); std::memcpy(out, &base, sizeof(base)); out->h = {sizeof(*out), 1, 0};
        out->seed = x.Seed();
        const auto found = x.subscriptions.find(handle);
        if (found != x.subscriptions.end()) { out->scope = found->second.scope; out->kinds = found->second.kinds; }
        out->workGeneration = x.prefix + 71; out->submittedWorkGeneration = x.prefix + 67;
        out->completedWorkGeneration = x.prefix + 61; out->submittedDependencyRevision = x.prefix + 59;
        out->waitingFor = MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY; Corrupt(out->h, x.badState); Throw(x.throwState);
        if (x.stateResult) return x.stateResult; return found == x.subscriptions.end() ? MMS_E_STALE : MMS_OK;
    }
    static MmsResult MMS_CALL Unsubscribe(void* p, MmsHandle handle) {
        auto& x = *static_cast<Host*>(p); ++x.releases; if (!x.Ui()) return MMS_E_THREAD;
        Throw(x.throwRelease); if (x.releaseResult) return x.releaseResult;
        return x.subscriptions.erase(handle) ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Target(void* p, MmsHandle seed, MmsUiRefreshTargetV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.targets; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiRefreshTargetV1>(); out->target = x.Seed(); out->supportedScopes = x.scopes;
        Corrupt(out->target.h, x.badTarget); Throw(x.throwTarget);
        if (x.targetResult) return x.targetResult; return seed == x.seed ? MMS_OK : MMS_E_STALE;
    }
    sdk::ScopedRefreshSubscriptionServices Services() {
        sdk::ScopedRefreshSubscriptionServices value; CHECK(sdk::ScopedRefreshSubscriptionServices::Query(host, value) == MMS_OK); return value;
    }
    sdk::RefreshServices Refresh() {
        sdk::RefreshServices value; CHECK(sdk::RefreshServices::Query(host, value) == MMS_OK); return value;
    }
};

static void NegotiationAndEmpty() {
    static_assert(sizeof(MmsUiRefreshSubscriptionApiV1) == 48 && sizeof(MmsUiRefreshSubscriptionApiV1_1) == 64);
    static_assert(sizeof(MmsUiRefreshScopedSubscriptionDescV1) == 80 && sizeof(MmsUiRefreshScopedSubscriptionStateV1) == 160);
    static_assert(!std::is_copy_constructible_v<sdk::ScopedRefreshSubscription> && !std::is_copy_assignable_v<sdk::ScopedRefreshSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::ScopedRefreshSubscription> && !std::is_move_assignable_v<sdk::ScopedRefreshSubscription>);
    static_assert(std::is_nothrow_destructible_v<sdk::ScopedRefreshSubscription>);
    Host a, b(UINT64_C(0x2900000000000000)); auto service = a.Services(); const auto original = service.api();
    sdk::ScopedRefreshSubscriptionServices empty; sdk::ScopedRefreshSubscription subscription;
    auto caps = Sentinel<MmsUiRefreshSubscriptionCapabilitiesV1>(); const auto oldCaps = caps;
    auto state = Sentinel<MmsUiRefreshScopedSubscriptionStateV1>(); const auto oldState = state;
    auto base = Sentinel<MmsUiRefreshSubscriptionStateV1>(); const auto oldBase = base;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY && !subscription.get());
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && Same(caps, oldCaps));
    CHECK(empty.Subscribe(a.Desc(), subscription) == MMS_E_NOT_READY && !subscription);
    CHECK(subscription.State(state) == MMS_E_NOT_READY && Same(state, oldState));
    CHECK(subscription.BaseState(base) == MMS_E_NOT_READY && Same(base, oldBase));
    CHECK(subscription.Release() == MMS_OK && !subscription.detach());
    b.legacyOnly = true;
    CHECK(sdk::ScopedRefreshSubscriptionServices::Query(b.host, service) == MMS_E_VERSION && Same(service.api(), original) && b.queries == 1 && !b.legacyQueries);
    sdk::RefreshSubscriptionServices legacy;
    CHECK(sdk::RefreshSubscriptionServices::Query(b.host, legacy) == MMS_OK && legacy.ready() && b.legacyQueries == 1); b.legacyOnly = false;
    for (const auto error : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY, MMS_E_FAILED}) {
        b.queryResult = error; CHECK(sdk::ScopedRefreshSubscriptionServices::Query(b.host, service) == error && Same(service.api(), original));
    }
    b.queryResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { b.throwQuery = mode; CHECK(sdk::ScopedRefreshSubscriptionServices::Query(b.host, service) == Error(mode) && Same(service.api(), original)); }
    b.throwQuery = 0; const auto calls = b.queries; auto host = b.host;
    host.h.size = sizeof(host) - 1; CHECK(sdk::ScopedRefreshSubscriptionServices::Query(host, service) == MMS_E_VERSION && Same(service.api(), original));
    host = b.host; host.h.major = 2; CHECK(sdk::ScopedRefreshSubscriptionServices::Query(host, service) == MMS_E_VERSION && Same(service.api(), original));
    host = b.host; host.queryService = nullptr; CHECK(sdk::ScopedRefreshSubscriptionServices::Query(host, service) == MMS_E_ARGUMENT && Same(service.api(), original)); CHECK(b.queries == calls);
    const auto table = b.table;
    for (unsigned invalid = 0; invalid != 3; ++invalid) {
        b.table = table; if (invalid == 0) b.table.h.size = sizeof(table) - 1; if (invalid == 1) b.table.h.major = 2; if (invalid == 2) b.table.h.minor = 0;
        CHECK(sdk::ScopedRefreshSubscriptionServices::Query(b.host, service) == MMS_E_VERSION && Same(service.api(), original));
    }
    for (unsigned field = 0; field != 6; ++field) {
        b.table = table;
        if (field == 0) b.table.getCapabilities = nullptr; if (field == 1) b.table.subscribe = nullptr;
        if (field == 2) b.table.query = nullptr; if (field == 3) b.table.unsubscribe = nullptr;
        if (field == 4) b.table.subscribeScoped = nullptr; if (field == 5) b.table.queryScoped = nullptr;
        CHECK(sdk::ScopedRefreshSubscriptionServices::Query(b.host, service) == MMS_E_UNSUPPORTED && Same(service.api(), original));
    }
    b.table = table; b.table.h.size += 32; b.table.h.minor = 3; host = b.host; host.h.size += 16; host.h.minor = 4;
    CHECK(sdk::ScopedRefreshSubscriptionServices::Query(host, empty) == MMS_OK && empty.api().context == &b && empty.api().h.minor == 3);
    CHECK(service.api().context == &a && service.CheckThread() == MMS_OK && a.arguments && b.arguments && !a.unexpected && !b.unexpected);
}

static void CallsAndConveniences() {
    Host x; auto service = x.Services(); auto refresh = x.Refresh(); sdk::ScopedRefreshSubscription owner;
    auto caps = Sentinel<MmsUiRefreshSubscriptionCapabilitiesV1>(); const auto oldCaps = caps;
    CHECK(service.Capabilities(caps) == MMS_OK && caps.supportedScopes == x.scopes && caps.supportedKinds == 15 && caps.maxSubscriptionsPerPlugin == 37 && !caps.flags);
    for (const auto error : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED}) { x.capsResult = error; caps = oldCaps; CHECK(service.Capabilities(caps) == error && Same(caps, oldCaps)); }
    x.capsResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.throwCaps = mode; caps = oldCaps; CHECK(service.Capabilities(caps) == Error(mode) && Same(caps, oldCaps)); }
    x.throwCaps = 0;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.badCaps = mode; caps = oldCaps; CHECK(service.Capabilities(caps) == MMS_E_VERSION && Same(caps, oldCaps)); }
    x.badCaps = 0;
    const auto raw = x.Desc();
    for (const auto error : {MMS_E_ARGUMENT, MMS_E_BUSY, MMS_E_STALE, MMS_E_FAILED}) { x.subscribeResult = error; CHECK(service.Subscribe(raw, owner) == error && !owner && x.subscriptions.empty() && Same(x.last, raw)); }
    x.subscribeResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.throwSubscribe = mode; CHECK(service.Subscribe(raw, owner) == Error(mode) && !owner && x.subscriptions.empty()); }
    x.throwSubscribe = 0; x.zeroHandle = true; CHECK(service.Subscribe(raw, owner) == MMS_E_FAILED && !owner && x.subscriptions.empty()); x.zeroHandle = false;
    CHECK(service.Subscribe(raw, owner) == MMS_OK && owner.get() > UINT32_MAX && Same(x.last, raw));
    const auto handle = owner.get(); const auto calls = x.scopedSubscribes;
    CHECK(service.Subscribe(raw, owner) == MMS_E_BUSY && owner.get() == handle && x.scopedSubscribes == calls);
    CHECK(service.SubscribeSubtree(refresh, x.binding, x.seed, owner) == MMS_E_BUSY && !x.targets && x.scopedSubscribes == calls);
    CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeSubtree(x.binding, x.Seed(), owner) == MMS_OK && Same(x.last, raw)); CHECK(owner.Release() == MMS_OK);
    const auto self = x.Desc(MMS_UI_REFRESH_SELF, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE);
    CHECK(service.Subscribe(self, owner) == MMS_OK && Same(x.last, self)); CHECK(owner.Release() == MMS_OK);
    const auto dependencies = x.Desc(MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT);
    CHECK(service.SubscribeDependents(x.binding, x.Seed(), owner) == MMS_OK && Same(x.last, dependencies)); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeSubtree(refresh, x.binding, x.seed, owner, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT) == MMS_OK);
    CHECK(Same(x.last.seed, x.Seed()) && x.last.binding == x.binding && x.last.scope == MMS_UI_REFRESH_SUBTREE && x.last.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT)); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeDependents(refresh, x.binding, x.seed, owner) == MMS_OK && Same(x.last, dependencies)); CHECK(owner.Release() == MMS_OK);
    const auto before = x.scopedSubscribes, targets = x.targets;
    CHECK(service.SubscribeSubtree(0, x.Seed(), owner) == MMS_E_ARGUMENT && x.scopedSubscribes == before);
    CHECK(service.SubscribeDependents(refresh, x.binding, 0, owner) == MMS_E_ARGUMENT && x.targets == targets);
    auto stale = raw; ++stale.seed.instanceGeneration; CHECK(service.Subscribe(stale, owner) == MMS_E_STALE && !owner && Same(x.last, stale));
    auto invalid = raw; invalid.flags = 0x80000000u; CHECK(service.Subscribe(invalid, owner) == MMS_E_ARGUMENT && !owner && Same(x.last, invalid));
    invalid = raw; invalid.reservedTail[1] = UINT64_C(0x123456789abcdef0);
    CHECK(service.Subscribe(invalid, owner) == MMS_E_ARGUMENT && !owner && Same(x.last, invalid));
    const auto attempted = x.scopedSubscribes;
    for (const auto error : {MMS_E_NOT_READY, MMS_E_STALE, MMS_E_FAILED}) { x.targetResult = error; CHECK(service.SubscribeSubtree(refresh, x.binding, x.seed, owner) == error && !owner && x.scopedSubscribes == attempted); }
    x.targetResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.throwTarget = mode; CHECK(service.SubscribeDependents(refresh, x.binding, x.seed, owner) == Error(mode) && x.scopedSubscribes == attempted); }
    x.throwTarget = 0;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.badTarget = mode; CHECK(service.SubscribeSubtree(refresh, x.binding, x.seed, owner) == MMS_E_VERSION && x.scopedSubscribes == attempted); }
    x.badTarget = 0; x.scopes = MMS_UI_REFRESH_SELF;
    CHECK(service.SubscribeDependents(refresh, x.binding, x.seed, owner) == MMS_E_UNSUPPORTED && !owner && x.scopedSubscribes == attempted);
    CHECK(!x.legacySubscribes && x.subscriptions.empty() && !x.wrongThread && !x.unexpected && x.arguments);
}

static void StatesWorkersAndOwnership() {
    Host x; auto service = x.Services(); auto refresh = x.Refresh(); sdk::ScopedRefreshSubscription owner;
    CHECK(service.SubscribeDependents(x.binding, x.Seed(), owner) == MMS_OK); const auto handle = owner.get();
    MmsUiRefreshScopedSubscriptionStateV1 state{}; MmsUiRefreshSubscriptionStateV1 base{};
    CHECK(owner.State(state) == MMS_OK && owner.BaseState(base) == MMS_OK && Same(base, x.Base(handle)));
    CHECK(state.subscription == handle && state.binding == x.binding && Same(state.seed, x.Seed()) && state.scope == MMS_UI_REFRESH_DEPENDENTS);
    CHECK(state.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT) && state.workGeneration == x.prefix + 71 && state.submittedWorkGeneration == x.prefix + 67 &&
        state.completedWorkGeneration == x.prefix + 61 && state.submittedDependencyRevision == x.prefix + 59 &&
        state.waitingFor == MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY && !state.flags && !state.reservedTail);
    MmsUiRefreshSubscriptionStateV1 prefix{}; std::memcpy(&prefix, &state, sizeof(prefix)); prefix.h = {sizeof(prefix), 1, 0}; CHECK(Same(prefix, base));
    const auto oldState = Sentinel<MmsUiRefreshScopedSubscriptionStateV1>(); const auto oldBase = Sentinel<MmsUiRefreshSubscriptionStateV1>();
    for (const auto error : {MMS_E_STALE, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.stateResult = x.baseResult = error; state = oldState; base = oldBase;
        CHECK(owner.State(state) == error && Same(state, oldState) && owner.BaseState(base) == error && Same(base, oldBase) && owner.get() == handle);
    }
    x.stateResult = x.baseResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.throwState = x.throwBase = mode; state = oldState; base = oldBase;
        CHECK(owner.State(state) == Error(mode) && Same(state, oldState) && owner.BaseState(base) == Error(mode) && Same(base, oldBase));
    }
    x.throwState = x.throwBase = 0;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.badState = x.badBase = mode; state = oldState; base = oldBase;
        CHECK(owner.State(state) == MMS_E_VERSION && Same(state, oldState) && owner.BaseState(base) == MMS_E_VERSION && Same(base, oldBase));
    }
    x.badState = x.badBase = 0;
    sdk::ScopedRefreshSubscription unused; auto caps = Sentinel<MmsUiRefreshSubscriptionCapabilitiesV1>(); const auto oldCaps = caps;
    MmsResult queried = MMS_E_FAILED, baseQueried = MMS_E_FAILED, capability = MMS_OK, subscribed = MMS_OK, subtree = MMS_OK, dependents = MMS_OK, element = MMS_OK, release = MMS_OK;
    const auto capCalls = x.caps, subCalls = x.scopedSubscribes, targetCalls = x.targets, releases = x.releases;
    std::thread worker([&] {
        auto copied = service; auto movedService = std::move(copied);
        queried = owner.State(state); baseQueried = owner.BaseState(base); capability = movedService.Capabilities(caps);
        subscribed = movedService.Subscribe(x.Desc(), unused); subtree = movedService.SubscribeSubtree(x.binding, x.Seed(), unused);
        dependents = movedService.SubscribeDependents(x.binding, x.Seed(), unused); element = movedService.SubscribeSubtree(refresh, x.binding, x.seed, unused); release = owner.Release();
    }); worker.join();
    CHECK(queried == MMS_OK && baseQueried == MMS_OK && state.subscription == handle && Same(base, x.Base(handle)) && x.workerScopedStates == 1 && x.workerBaseStates == 1);
    CHECK(capability == MMS_E_THREAD && subscribed == MMS_E_THREAD && subtree == MMS_E_THREAD && dependents == MMS_E_THREAD && element == MMS_E_THREAD && release == MMS_E_THREAD);
    CHECK(Same(caps, oldCaps) && x.caps == capCalls && x.scopedSubscribes == subCalls && x.targets == targetCalls && x.releases == releases && !x.wrongThread && !unused);
    sdk::ScopedRefreshSubscription moved(std::move(owner)); CHECK(!owner && !owner.get() && moved.get() == handle && owner.Release() == MMS_OK);
    for (const auto error : {MMS_E_BUSY, MMS_E_THREAD, MMS_E_NOT_FOUND, MMS_E_MEMORY, MMS_E_FAILED}) {
        x.releaseResult = error; CHECK(moved.Release() == error && moved.get() == handle && moved.State(state) == MMS_OK && moved.BaseState(base) == MMS_OK);
    }
    x.releaseResult = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.throwRelease = mode; CHECK(moved.Release() == Error(mode) && moved.get() == handle && x.subscriptions.count(handle)); }
    x.throwRelease = 0; CHECK(moved.Release() == MMS_OK && !moved && x.subscriptions.empty());
    CHECK(service.SubscribeSubtree(x.binding, x.Seed(), owner) == MMS_OK); const auto crossing = owner.get();
    std::unique_ptr<sdk::ScopedRefreshSubscription> returned; MmsResult crossingRelease = MMS_OK; const auto before = x.releases;
    std::thread transfer([&] { returned = std::make_unique<sdk::ScopedRefreshSubscription>(std::move(owner)); crossingRelease = returned->Release(); queried = returned->State(state); }); transfer.join();
    CHECK(!owner && returned && returned->get() == crossing && crossingRelease == MMS_E_THREAD && queried == MMS_OK && x.releases == before && !x.wrongThread);
    Host replacement(UINT64_C(0x3900000000000000)); CHECK(sdk::ScopedRefreshSubscriptionServices::Query(replacement.host, service) == MMS_OK);
    CHECK(returned->BaseState(base) == MMS_OK && base.binding == x.binding && returned->Release() == MMS_OK && !*returned && !replacement.releases && x.subscriptions.empty());
    CHECK(x.arguments && replacement.arguments && !x.unexpected && !replacement.unexpected);
}

static void SamePoolDetachAndDestructors() {
    Host x; auto service = x.Services(); sdk::RefreshSubscriptionServices legacy; CHECK(sdk::RefreshSubscriptionServices::Query(x.host, legacy) == MMS_OK);
    sdk::ScopedRefreshSubscription owner; CHECK(service.SubscribeSubtree(x.binding, x.Seed(), owner) == MMS_OK); auto handle = owner.get();
    MmsUiRefreshSubscriptionStateV1 base{}; base.h = sdk::Header<MmsUiRefreshSubscriptionStateV1>();
    CHECK(legacy.api().query(legacy.api().context, handle, &base) == MMS_OK && base.subscription == handle && base.binding == x.binding);
    const auto releases = x.releases; CHECK(owner.detach() == handle && !owner && x.releases == releases);
    CHECK(legacy.api().unsubscribe(legacy.api().context, handle) == MMS_OK && x.subscriptions.empty());
    MmsUiRefreshSubscriptionDescV1 desc{sdk::Header<MmsUiRefreshSubscriptionDescV1>(), x.binding, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, 0, 0};
    CHECK(legacy.api().subscribe(legacy.api().context, &desc, &handle) == MMS_OK);
    MmsUiRefreshScopedSubscriptionStateV1 state{}; state.h = {sizeof(state), 1, 0};
    CHECK(service.api().queryScoped(service.api().context, handle, &state) == MMS_OK && state.scope == MMS_UI_REFRESH_SELF && Same(state.seed, x.Seed()));
    CHECK(legacy.api().unsubscribe(legacy.api().context, handle) == MMS_OK);
    CHECK(service.SubscribeSubtree(x.binding, x.Seed(), owner) == MMS_OK); handle = owner.get(); x.subscriptions.erase(handle);
    const auto sentinel = Sentinel<MmsUiRefreshScopedSubscriptionStateV1>(); state = sentinel;
    CHECK(owner.State(state) == MMS_E_STALE && Same(state, sentinel) && owner.get() == handle && owner.Release() == MMS_OK && !owner);
    const auto before = x.releases; CHECK(owner.Release() == MMS_OK && !owner.detach() && x.releases == before);
    { sdk::ScopedRefreshSubscription scoped; CHECK(service.SubscribeSubtree(x.binding, x.Seed(), scoped) == MMS_OK); handle = scoped.get(); }
    CHECK(!x.subscriptions.count(handle) && x.releases == before + 1);
    { sdk::ScopedRefreshSubscription scoped; CHECK(service.SubscribeSubtree(x.binding, x.Seed(), scoped) == MMS_OK); handle = scoped.get(); x.releaseResult = MMS_E_BUSY; }
    CHECK(x.subscriptions.count(handle)); x.releaseResult = MMS_OK; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    for (unsigned mode = 1; mode <= 3; ++mode) {
        { sdk::ScopedRefreshSubscription scoped; CHECK(service.SubscribeSubtree(x.binding, x.Seed(), scoped) == MMS_OK); handle = scoped.get(); x.throwRelease = mode; }
        CHECK(x.subscriptions.count(handle)); x.throwRelease = 0; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    {
        auto scoped = std::make_unique<sdk::ScopedRefreshSubscription>(); CHECK(service.SubscribeSubtree(x.binding, x.Seed(), *scoped) == MMS_OK); handle = scoped->get(); const auto calls = x.releases;
        std::thread worker([owned = std::move(scoped)]() mutable { owned.reset(); }); worker.join();
        CHECK(!scoped && x.releases == calls && x.subscriptions.count(handle) && !x.wrongThread); CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    CHECK(service.SubscribeSubtree(x.binding, x.Seed(), owner) == MMS_OK); handle = owner.get(); MmsHandle detached = 0; const auto calls = x.releases;
    std::thread worker([&] { detached = owner.detach(); }); worker.join();
    CHECK(detached == handle && !owner && x.releases == calls && x.subscriptions.count(handle)); CHECK(x.table.unsubscribe(x.table.context, detached) == MMS_OK);
    CHECK(x.subscriptions.empty() && x.arguments && !x.wrongThread && !x.unexpected);
}
} // namespace cpp_scoped_refresh_subscription_checks
static void CppScopedRefreshSubscriptionChecks() {
    cpp_scoped_refresh_subscription_checks::NegotiationAndEmpty();
    cpp_scoped_refresh_subscription_checks::CallsAndConveniences();
    cpp_scoped_refresh_subscription_checks::StatesWorkersAndOwnership();
    cpp_scoped_refresh_subscription_checks::SamePoolDetachAndDestructors();
}
