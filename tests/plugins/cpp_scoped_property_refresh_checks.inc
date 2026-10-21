#include <mms/cpp/scoped_property_refresh.hpp>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

namespace cpp_scoped_property_refresh_checks {
namespace sdk = mms::sdk;
template<class T> static bool HeaderIs(const T* value) {
    return value && value->h.size == sizeof(T) && value->h.major == 1 && value->h.minor == 0;
}
template<class T> static T Sentinel() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> static bool Same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
static bool TextIs(MmsString value, std::string_view expected) {
    return value.data && value.bytes == expected.size() && !std::memcmp(value.data, expected.data(), expected.size());
}
static void Throw(unsigned mode) {
    if (mode == 1) throw std::bad_alloc();
    if (mode == 2) throw std::length_error("scoped property fake length");
    if (mode == 3) throw std::runtime_error("scoped property fake runtime");
}
static MmsResult Error(unsigned mode) { return mode < 3 ? MMS_E_MEMORY : MMS_E_FAILED; }
struct Fault {
    MmsResult result = MMS_OK;
    unsigned exception = 0, header = 0;
    void Header(MmsHeader& value) const {
        if (header == 1) --value.size;
        if (header == 2) value.major = 2;
        if (header == 3) value.minor = 1;
    }
    MmsResult Finish() const { Throw(exception); return result; }
};

// Independent C protocol host: one subscription pool and argument recording.
// It has no production registry, history, dependency graph or refresh queue.
struct Host {
    const std::thread::id ui = std::this_thread::get_id();
    const MmsHandle prefix, property, seed;
    MmsHandle next;
    MmsHostV1 host{};
    MmsUiPropertyRefreshApiV1_1 table{};
    MmsUiPropertyRefreshApiV1 legacy{};
    MmsUiRefreshApiV1 refresh{};
    MmsUiPropertyRefreshScopedSubscriptionDescV1 last{};
    MmsUiPropertyRefV1 notified{};
    std::map<MmsHandle, MmsUiPropertyRefreshScopedSubscriptionDescV1> subscriptions;
    unsigned queries = 0, legacyQueries = 0, refreshQueries = 0, caps = 0, baseCaps = 0, refs = 0, targets = 0;
    unsigned scopedSubscribes = 0, legacySubscribes = 0, states = 0, baseStates = 0, releases = 0;
    unsigned notifications = 0, workerNotifications = 0, workerStates = 0, workerBaseStates = 0, wrongThread = 0, unexpected = 0;
    Fault queryFault, capsFault, baseCapsFault, refFault, targetFault, subscribeFault, stateFault, baseStateFault, notifyFault, releaseFault;
    bool arguments = true, legacyOnly = false, zeroHandle = false, targetOuterHeader = false;
    uint32_t scopes = MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS;
    uint32_t targetScopes = scopes, features = 0, legacyFeatures = MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY;
    explicit Host(MmsHandle value = UINT64_C(0x1a00000000000000))
        : prefix(value), property(value + 11), seed(value + 29), next(value + 101) {
        table.h = {sizeof(table), 1, 1}; table.context = this;
        table.getCapabilities = BaseCaps; table.getPropertyRef = Ref; table.subscribe = LegacySubscribe;
        table.notify = Notify; table.query = BaseState; table.unsubscribe = Unsubscribe;
        table.getScopedCapabilities = Caps; table.subscribeScoped = Subscribe; table.queryScoped = State;
        legacy.h = {sizeof(legacy), 1, 0}; legacy.context = this;
        legacy.getCapabilities = BaseCaps; legacy.getPropertyRef = Ref; legacy.subscribe = LegacySubscribe;
        legacy.notify = Notify; legacy.query = BaseState; legacy.unsubscribe = Unsubscribe;
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
    MmsUiPropertyRefV1 Property() const { return {sdk::Header<MmsUiPropertyRefV1>(), property, prefix + 37, prefix + 43}; }
    MmsUiTargetRefV1 Seed() const { return {sdk::Header<MmsUiTargetRefV1>(), seed, prefix + 53, prefix + 43}; }
    MmsUiPropertyRefreshScopedSubscriptionDescV1 Desc(uint32_t scope = MMS_UI_REFRESH_SUBTREE,
        uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, uint32_t flags = 0) const {
        MmsUiPropertyRefreshScopedSubscriptionDescV1 value{}; value.h = sdk::Header<MmsUiPropertyRefreshScopedSubscriptionDescV1>();
        value.property = Property(); value.seed = Seed(); value.scope = scope; value.kinds = kinds; value.flags = flags; return value;
    }
    static MmsResult MMS_CALL Query(void* p, MmsString id, uint32_t major, uint32_t minor, uint32_t bytes, void* out) {
        auto& x = *static_cast<Host*>(p); if (!x.Ui()) return MMS_E_THREAD;
        if (TextIs(id, "mms.ui.refresh.property")) {
            if (minor == 0) {
                ++x.legacyQueries; x.arguments &= major == 1 && bytes == sizeof(x.legacy) && out;
                if (!out || bytes != sizeof(x.legacy)) return MMS_E_ARGUMENT;
                std::memcpy(out, &x.legacy, sizeof(x.legacy)); return MMS_OK;
            }
            ++x.queries; x.arguments &= major == 1 && minor == 1 && bytes == sizeof(x.table) && out;
            if (!out || bytes != sizeof(x.table)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.table, sizeof(x.table));
            if (const auto result = x.queryFault.Finish()) return result;
            return x.legacyOnly ? MMS_E_VERSION : MMS_OK;
        }
        if (TextIs(id, "mms.ui.refresh")) {
            ++x.refreshQueries; x.arguments &= major == 1 && minor == 0 && bytes == sizeof(x.refresh) && out;
            if (!out || bytes != sizeof(x.refresh)) return MMS_E_ARGUMENT;
            std::memcpy(out, &x.refresh, sizeof(x.refresh)); return MMS_OK;
        }
        ++x.unexpected; return MMS_E_NOT_FOUND;
    }
    static MmsResult MMS_CALL BaseCaps(void* p, MmsUiPropertyRefreshCapabilitiesV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.baseCaps; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {sdk::Header<MmsUiPropertyRefreshCapabilitiesV1>(), 37, 15, x.legacyFeatures, 0, 0};
        x.baseCapsFault.Header(out->h); return x.baseCapsFault.Finish();
    }
    static MmsResult MMS_CALL Caps(void* p, MmsUiPropertyRefreshScopedCapabilitiesV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.caps; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {sdk::Header<MmsUiPropertyRefreshScopedCapabilitiesV1>(), 37, 15, x.features, 0, 0, x.scopes, 0};
        x.capsFault.Header(out->h); return x.capsFault.Finish();
    }
    static MmsResult MMS_CALL Ref(void* p, MmsHandle property, MmsUiPropertyRefV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.refs; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = x.Property(); x.refFault.Header(out->h);
        if (const auto result = x.refFault.Finish()) return result;
        return property == x.property ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Target(void* p, MmsHandle seed, MmsUiRefreshTargetV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.targets; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; out->h = sdk::Header<MmsUiRefreshTargetV1>(); out->target = x.Seed();
        // The independent seed is not a DATA consumer. Its scope support is
        // sufficient; member DATA capabilities are the host's responsibility.
        out->supportedKinds = MMS_UI_REFRESH_REPAINT; out->supportedScopes = x.targetScopes; out->ready = 1;
        x.targetFault.Header(x.targetOuterHeader ? out->h : out->target.h);
        if (const auto result = x.targetFault.Finish()) return result;
        return seed == x.seed ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL Subscribe(void* p, const MmsUiPropertyRefreshScopedSubscriptionDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(p); ++x.scopedSubscribes; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out; if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        x.last = *desc; *out = x.prefix + 999;
        if (const auto result = x.subscribeFault.Finish()) return result;
        if (!Same(desc->property, x.Property()) || !Same(desc->seed, x.Seed())) return MMS_E_STALE;
        if ((desc->scope != MMS_UI_REFRESH_SELF && desc->scope != MMS_UI_REFRESH_SUBTREE && desc->scope != MMS_UI_REFRESH_DEPENDENTS) ||
            !(desc->kinds & MMS_UI_REFRESH_DATA) || desc->kinds & ~15u || desc->reserved || desc->reservedTail) return MMS_E_ARGUMENT;
        if (!(x.scopes & desc->scope) || desc->flags & ~x.features) return MMS_E_UNSUPPORTED;
        if (x.zeroHandle) { *out = 0; return MMS_OK; }
        const auto handle = x.next++; x.subscriptions.emplace(handle, *desc); *out = handle; return MMS_OK;
    }
    static MmsResult MMS_CALL LegacySubscribe(void* p, const MmsUiPropertyRefreshSubscriptionDescV1* desc, MmsHandle* out) {
        auto& x = *static_cast<Host*>(p); ++x.legacySubscribes; if (!x.Ui()) return MMS_E_THREAD;
        x.arguments &= HeaderIs(desc) && out; if (!HeaderIs(desc) || !out) return MMS_E_ARGUMENT;
        if (!Same(desc->property, x.Property()) || !Same(desc->target, x.Seed())) return MMS_E_STALE;
        const auto handle = x.next++; auto value = x.Desc(MMS_UI_REFRESH_SELF, desc->kinds, desc->flags);
        x.subscriptions.emplace(handle, value); *out = handle; return MMS_OK;
    }
    static MmsResult MMS_CALL Notify(void* p, const MmsUiPropertyRefV1* property) {
        auto& x = *static_cast<Host*>(p); ++x.notifications;
        if (std::this_thread::get_id() != x.ui) ++x.workerNotifications;
        if (!property) return MMS_E_ARGUMENT; x.notified = *property;
        if (const auto result = x.notifyFault.Finish()) return result;
        return Same(*property, x.Property()) ? MMS_OK : MMS_E_STALE;
    }
    MmsUiPropertyRefreshSubscriptionStateV1 Base(MmsHandle handle) const {
        MmsUiPropertyRefreshSubscriptionStateV1 value{}; value.h = sdk::Header<MmsUiPropertyRefreshSubscriptionStateV1>();
        value.subscription = handle; value.target = Seed(); value.property = Property();
        value.latestSequence = prefix + 83; value.submittedSequence = prefix + 79; value.completedSequence = prefix + 73;
        value.lastResult = MMS_E_BUSY; value.pending = 1; value.waitingFor = MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY; return value;
    }
    static MmsResult MMS_CALL BaseState(void* p, MmsHandle handle, MmsUiPropertyRefreshSubscriptionStateV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.baseStates;
        if (std::this_thread::get_id() != x.ui) ++x.workerBaseStates;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = x.Base(handle); x.baseStateFault.Header(out->h);
        if (const auto result = x.baseStateFault.Finish()) return result;
        return x.subscriptions.count(handle) ? MMS_OK : MMS_E_STALE;
    }
    static MmsResult MMS_CALL State(void* p, MmsHandle handle, MmsUiPropertyRefreshScopedSubscriptionStateV1* out) {
        auto& x = *static_cast<Host*>(p); ++x.states;
        if (std::this_thread::get_id() != x.ui) ++x.workerStates;
        x.arguments &= HeaderIs(out); if (!HeaderIs(out)) return MMS_E_ARGUMENT;
        *out = {}; const auto base = x.Base(handle); std::memcpy(out, &base, sizeof(base)); out->h = sdk::Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
        const auto found = x.subscriptions.find(handle);
        if (found != x.subscriptions.end()) { out->scope = found->second.scope; out->kinds = found->second.kinds; out->flags = found->second.flags; }
        out->workGeneration = x.prefix + 71; out->submittedWorkGeneration = x.prefix + 67;
        out->completedWorkGeneration = x.prefix + 61; out->submittedDependencyRevision = x.prefix + 59;
        x.stateFault.Header(out->h); if (const auto result = x.stateFault.Finish()) return result;
        return found == x.subscriptions.end() ? MMS_E_STALE : MMS_OK;
    }
    static MmsResult MMS_CALL Unsubscribe(void* p, MmsHandle handle) {
        auto& x = *static_cast<Host*>(p); ++x.releases; if (!x.Ui()) return MMS_E_THREAD;
        if (const auto result = x.releaseFault.Finish()) return result;
        return x.subscriptions.erase(handle) ? MMS_OK : MMS_E_STALE;
    }
    sdk::ScopedPropertyRefreshServices Services() {
        sdk::ScopedPropertyRefreshServices value; CHECK(sdk::ScopedPropertyRefreshServices::Query(host, value) == MMS_OK); return value;
    }
    sdk::RefreshServices Refresh() {
        sdk::RefreshServices value; CHECK(sdk::RefreshServices::Query(host, value) == MMS_OK); return value;
    }
};

static void NegotiationAndEmpty() {
    static_assert(sizeof(MmsUiPropertyRefreshApiV1) == 64 && sizeof(MmsUiPropertyRefreshApiV1_1) == 88);
    static_assert(sizeof(MmsUiPropertyRefreshScopedCapabilitiesV1) == 40 && sizeof(MmsUiPropertyRefreshScopedSubscriptionDescV1) == 96 &&
        sizeof(MmsUiPropertyRefreshScopedSubscriptionStateV1) == 176);
    static_assert(!std::is_copy_constructible_v<sdk::ScopedPropertyRefreshSubscription> && !std::is_copy_assignable_v<sdk::ScopedPropertyRefreshSubscription>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::ScopedPropertyRefreshSubscription> && !std::is_move_assignable_v<sdk::ScopedPropertyRefreshSubscription>);
    static_assert(std::is_nothrow_destructible_v<sdk::ScopedPropertyRefreshSubscription>);
    Host a, b(UINT64_C(0x2a00000000000000)); auto service = a.Services(); const auto original = service.api();
    sdk::ScopedPropertyRefreshServices empty; sdk::ScopedPropertyRefreshSubscription owner;
    auto caps = Sentinel<MmsUiPropertyRefreshScopedCapabilitiesV1>(); const auto oldCaps = caps;
    auto baseCaps = Sentinel<MmsUiPropertyRefreshCapabilitiesV1>(); const auto oldBaseCaps = baseCaps;
    auto ref = Sentinel<MmsUiPropertyRefV1>(); const auto oldRef = ref;
    auto state = Sentinel<MmsUiPropertyRefreshScopedSubscriptionStateV1>(); const auto oldState = state;
    auto base = Sentinel<MmsUiPropertyRefreshSubscriptionStateV1>(); const auto oldBase = base;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY && !owner.get());
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && Same(caps, oldCaps));
    CHECK(empty.BaseCapabilities(baseCaps) == MMS_E_NOT_READY && Same(baseCaps, oldBaseCaps));
    CHECK(empty.GetPropertyRef(a.property, ref) == MMS_E_NOT_READY && Same(ref, oldRef));
    CHECK(empty.Notify(a.Property()) == MMS_E_NOT_READY && empty.Subscribe(a.Desc(), owner) == MMS_E_NOT_READY && !owner);
    CHECK(owner.State(state) == MMS_E_NOT_READY && Same(state, oldState));
    CHECK(owner.BaseState(base) == MMS_E_NOT_READY && Same(base, oldBase));
    CHECK(owner.Release() == MMS_OK && !owner.detach());
    b.legacyOnly = true;
    CHECK(sdk::ScopedPropertyRefreshServices::Query(b.host, service) == MMS_E_VERSION && Same(service.api(), original) && b.queries == 1 && !b.legacyQueries);
    sdk::PropertyRefreshServices legacy; CHECK(sdk::PropertyRefreshServices::Query(b.host, legacy) == MMS_OK && legacy.ready() && b.legacyQueries == 1); b.legacyOnly = false;
    for (const auto error : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY, MMS_E_FAILED}) {
        b.queryFault.result = error; CHECK(sdk::ScopedPropertyRefreshServices::Query(b.host, service) == error && Same(service.api(), original));
    }
    b.queryFault.result = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { b.queryFault.exception = mode; CHECK(sdk::ScopedPropertyRefreshServices::Query(b.host, service) == Error(mode) && Same(service.api(), original)); }
    b.queryFault = {}; const auto calls = b.queries; auto host = b.host;
    host.h.size = sizeof(host) - 1; CHECK(sdk::ScopedPropertyRefreshServices::Query(host, service) == MMS_E_VERSION && Same(service.api(), original));
    host = b.host; host.h.major = 2; CHECK(sdk::ScopedPropertyRefreshServices::Query(host, service) == MMS_E_VERSION && Same(service.api(), original));
    host = b.host; host.queryService = nullptr; CHECK(sdk::ScopedPropertyRefreshServices::Query(host, service) == MMS_E_ARGUMENT && Same(service.api(), original)); CHECK(b.queries == calls);
    const auto table = b.table;
    for (unsigned invalid = 0; invalid != 3; ++invalid) {
        b.table = table; if (invalid == 0) --b.table.h.size; if (invalid == 1) b.table.h.major = 2; if (invalid == 2) b.table.h.minor = 0;
        CHECK(sdk::ScopedPropertyRefreshServices::Query(b.host, service) == MMS_E_VERSION && Same(service.api(), original));
    }
    for (unsigned field = 0; field != 9; ++field) {
        b.table = table;
        if (field == 0) b.table.getCapabilities = nullptr; if (field == 1) b.table.getPropertyRef = nullptr;
        if (field == 2) b.table.subscribe = nullptr; if (field == 3) b.table.notify = nullptr;
        if (field == 4) b.table.query = nullptr; if (field == 5) b.table.unsubscribe = nullptr;
        if (field == 6) b.table.getScopedCapabilities = nullptr; if (field == 7) b.table.subscribeScoped = nullptr;
        if (field == 8) b.table.queryScoped = nullptr;
        CHECK(sdk::ScopedPropertyRefreshServices::Query(b.host, service) == MMS_E_UNSUPPORTED && Same(service.api(), original));
    }
    b.table = table; b.table.h.size += 32; b.table.h.minor = 3; host = b.host; host.h.size += 16; host.h.minor = 4;
    CHECK(sdk::ScopedPropertyRefreshServices::Query(host, empty) == MMS_OK && empty.api().context == &b && empty.api().h.minor == 3);
    CHECK(service.api().context == &a && service.CheckThread() == MMS_OK && a.arguments && b.arguments && !a.unexpected && !b.unexpected);
}

static void ValuesAndExceptions() {
    Host x; auto service = x.Services();
    auto caps = Sentinel<MmsUiPropertyRefreshScopedCapabilitiesV1>(); const auto oldCaps = caps;
    auto baseCaps = Sentinel<MmsUiPropertyRefreshCapabilitiesV1>(); const auto oldBaseCaps = baseCaps;
    auto ref = Sentinel<MmsUiPropertyRefV1>(); const auto oldRef = ref;
    CHECK(service.Capabilities(caps) == MMS_OK && caps.supportedScopes == x.scopes && caps.maxSubscriptionsPerPlugin == 37 &&
        caps.supportedKinds == 15 && !caps.supportedFlags && !caps.flags && !caps.reserved && !caps.reservedTail);
    CHECK(service.BaseCapabilities(baseCaps) == MMS_OK && baseCaps.supportedFlags == x.legacyFeatures);
    CHECK(service.GetPropertyRef(x.property, ref) == MMS_OK && Same(ref, x.Property()) && ref.property > UINT32_MAX);
    for (const auto error : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.capsFault.result = x.baseCapsFault.result = x.refFault.result = error;
        caps = oldCaps; baseCaps = oldBaseCaps; ref = oldRef;
        CHECK(service.Capabilities(caps) == error && Same(caps, oldCaps));
        CHECK(service.BaseCapabilities(baseCaps) == error && Same(baseCaps, oldBaseCaps));
        CHECK(service.GetPropertyRef(x.property, ref) == error && Same(ref, oldRef));
    }
    x.capsFault = x.baseCapsFault = x.refFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.capsFault.exception = x.baseCapsFault.exception = x.refFault.exception = mode;
        CHECK(service.Capabilities(caps) == Error(mode) && Same(caps, oldCaps));
        CHECK(service.BaseCapabilities(baseCaps) == Error(mode) && Same(baseCaps, oldBaseCaps));
        CHECK(service.GetPropertyRef(x.property, ref) == Error(mode) && Same(ref, oldRef));
    }
    x.capsFault = x.baseCapsFault = x.refFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.capsFault.header = x.baseCapsFault.header = x.refFault.header = mode;
        CHECK(service.Capabilities(caps) == MMS_E_VERSION && Same(caps, oldCaps));
        CHECK(service.BaseCapabilities(baseCaps) == MMS_E_VERSION && Same(baseCaps, oldBaseCaps));
        CHECK(service.GetPropertyRef(x.property, ref) == MMS_E_VERSION && Same(ref, oldRef));
    }
    x.capsFault = x.baseCapsFault = x.refFault = {};
    const auto refs = x.refs; CHECK(service.GetPropertyRef(0, ref) == MMS_E_ARGUMENT && Same(ref, oldRef) && x.refs == refs);
    CHECK(service.GetPropertyRef(x.property + 1, ref) == MMS_E_STALE && Same(ref, oldRef));
    const auto property = x.Property();
    CHECK(service.Notify(property) == MMS_OK && Same(x.notified, property));
    for (const auto error : {MMS_E_BUSY, MMS_E_STALE, MMS_E_NOT_READY, MMS_E_FAILED}) { x.notifyFault.result = error; CHECK(service.Notify(property) == error && Same(property, x.Property())); }
    x.notifyFault.result = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.notifyFault.exception = mode; CHECK(service.Notify(property) == Error(mode) && Same(property, x.Property())); }
    x.notifyFault = {}; auto stale = property; ++stale.registrationGeneration; CHECK(service.Notify(stale) == MMS_E_STALE && Same(x.notified, stale));
    CHECK(x.arguments && !x.wrongThread && !x.unexpected);
}

static void SubscriptionsAndConveniences() {
    Host x; auto service = x.Services(); auto refresh = x.Refresh(); sdk::ScopedPropertyRefreshSubscription owner;
    const auto raw = x.Desc();
    for (const auto error : {MMS_E_ARGUMENT, MMS_E_BUSY, MMS_E_STALE, MMS_E_FAILED}) {
        x.subscribeFault.result = error; CHECK(service.Subscribe(raw, owner) == error && !owner && x.subscriptions.empty() && Same(x.last, raw));
    }
    x.subscribeFault.result = MMS_OK;
    for (unsigned mode = 1; mode <= 3; ++mode) { x.subscribeFault.exception = mode; CHECK(service.Subscribe(raw, owner) == Error(mode) && !owner && x.subscriptions.empty()); }
    x.subscribeFault = {}; x.zeroHandle = true; CHECK(service.Subscribe(raw, owner) == MMS_E_FAILED && !owner && x.subscriptions.empty()); x.zeroHandle = false;
    CHECK(service.Subscribe(raw, owner) == MMS_OK && owner.get() > UINT32_MAX && Same(x.last, raw));
    const auto handle = owner.get(); const auto calls = x.scopedSubscribes;
    CHECK(service.Subscribe(raw, owner) == MMS_E_BUSY && owner.get() == handle && x.scopedSubscribes == calls);
    const auto refs = x.refs, targets = x.targets;
    CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == MMS_E_BUSY && x.refs == refs && x.targets == targets);
    CHECK(owner.Release() == MMS_OK);
    const auto capCalls = x.caps;
    CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_OK && Same(x.last, raw) && x.caps == capCalls); CHECK(owner.Release() == MMS_OK);
    const auto dependents = x.Desc(MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT);
    CHECK(service.SubscribeDependents(x.Property(), x.Seed(), owner) == MMS_OK && Same(x.last, dependents)); CHECK(owner.Release() == MMS_OK);
    const auto self = x.Desc(MMS_UI_REFRESH_SELF, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE);
    CHECK(service.Subscribe(self, owner) == MMS_OK && Same(x.last, self)); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == MMS_OK && Same(x.last, raw)); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == MMS_OK && Same(x.last, dependents)); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT) == MMS_OK);
    CHECK(x.last.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT) && !x.last.flags && Same(x.last.property, x.Property()) && Same(x.last.seed, x.Seed())); CHECK(owner.Release() == MMS_OK);
    const auto attempted = x.scopedSubscribes, refCalls = x.refs, targetCalls = x.targets;
    CHECK(service.SubscribeSubtree(refresh, 0, x.seed, owner) == MMS_E_ARGUMENT && x.refs == refCalls && x.targets == targetCalls);
    CHECK(service.SubscribeDependents(refresh, x.property, 0, owner) == MMS_E_ARGUMENT && x.refs == refCalls && x.targets == targetCalls);
    auto bad = raw; ++bad.property.registrationGeneration; CHECK(service.Subscribe(bad, owner) == MMS_E_STALE && !owner && Same(x.last, bad));
    bad = raw; ++bad.seed.instanceGeneration; CHECK(service.Subscribe(bad, owner) == MMS_E_STALE && !owner && Same(x.last, bad));
    bad = raw; bad.reservedTail = UINT64_C(0x123456789abcdef0); CHECK(service.Subscribe(bad, owner) == MMS_E_ARGUMENT && !owner && Same(x.last, bad));
    bad = raw; bad.flags = 0x80000000u; CHECK(service.Subscribe(bad, owner) == MMS_E_UNSUPPORTED && !owner && Same(x.last, bad));
    const auto beforeLookup = x.scopedSubscribes;
    for (const auto error : {MMS_E_NOT_READY, MMS_E_STALE, MMS_E_FAILED}) {
        x.refFault.result = error; CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == error && !owner && x.scopedSubscribes == beforeLookup);
    }
    x.refFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.refFault.exception = mode; CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == Error(mode) && x.scopedSubscribes == beforeLookup);
    }
    x.refFault = {};
    for (const auto error : {MMS_E_NOT_READY, MMS_E_STALE, MMS_E_FAILED}) {
        x.targetFault.result = error; CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == error && !owner && x.scopedSubscribes == beforeLookup);
    }
    x.targetFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.targetFault.exception = mode; CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == Error(mode) && x.scopedSubscribes == beforeLookup);
    }
    x.targetFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.targetFault.header = mode; CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == MMS_E_VERSION && x.scopedSubscribes == beforeLookup);
    }
    x.targetOuterHeader = true;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.targetFault.header = mode; CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == MMS_E_VERSION && x.scopedSubscribes == beforeLookup);
    }
    x.targetOuterHeader = false;
    x.targetFault = {}; x.targetScopes = MMS_UI_REFRESH_SELF;
    // GetTarget reports dynamic graph availability, not the service's ability
    // to register a mailbox while that graph is empty. It also omits DATA.
    CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == MMS_OK && Same(x.last, dependents) && x.scopedSubscribes == beforeLookup + 1);
    CHECK(owner.Release() == MMS_OK);
    x.targetScopes = x.scopes; x.scopes = MMS_UI_REFRESH_SELF;
    const auto beforeUnsupported = x.scopedSubscribes, beforeRefs = x.refs, beforeTargets = x.targets;
    CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == MMS_E_UNSUPPORTED && !owner &&
        x.scopedSubscribes == beforeUnsupported && x.refs == beforeRefs && x.targets == beforeTargets);
    CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == MMS_E_UNSUPPORTED && !owner && x.scopedSubscribes == beforeUnsupported);
    x.scopes = MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS;
    for (const auto error : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.capsFault.result = error;
        CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == error && !owner && x.scopedSubscribes == beforeUnsupported &&
            x.refs == beforeRefs && x.targets == beforeTargets);
    }
    x.capsFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.capsFault.exception = mode;
        CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner) == Error(mode) && !owner && x.scopedSubscribes == beforeUnsupported);
    }
    x.capsFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.capsFault.header = mode;
        CHECK(service.SubscribeDependents(refresh, x.property, x.seed, owner) == MMS_E_VERSION && !owner && x.scopedSubscribes == beforeUnsupported &&
            x.refs == beforeRefs && x.targets == beforeTargets);
    }
    x.capsFault = {}; x.scopes = MMS_UI_REFRESH_SELF;
    CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_E_UNSUPPORTED && !owner);
    CHECK(x.scopedSubscribes > attempted && !x.legacySubscribes && x.subscriptions.empty() && x.arguments && !x.wrongThread && !x.unexpected);
}

static void ExplicitHistoryCapabilities() {
    Host x; auto service = x.Services(); auto refresh = x.Refresh(); sdk::ScopedPropertyRefreshSubscription owner;
    const auto desc = x.Desc(MMS_UI_REFRESH_DEPENDENTS, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY);
    const auto subscribed = x.scopedSubscribes, baseCaps = x.baseCaps;
    // Old capabilities advertise history, while the actual 1.1 capabilities
    // do not. New subscriptions must consult the latter and keep flags opt-in.
    CHECK(service.Subscribe(desc, owner) == MMS_E_UNSUPPORTED && !owner && x.scopedSubscribes == subscribed && x.baseCaps == baseCaps);
    for (const auto error : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.capsFault.result = error; CHECK(service.Subscribe(desc, owner) == error && !owner && x.scopedSubscribes == subscribed);
    }
    x.capsFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.capsFault.exception = mode; CHECK(service.Subscribe(desc, owner) == Error(mode) && !owner && x.scopedSubscribes == subscribed);
    }
    x.capsFault = {}; x.capsFault.header = 1;
    CHECK(service.Subscribe(desc, owner) == MMS_E_VERSION && !owner && x.scopedSubscribes == subscribed);
    x.capsFault = {}; x.features = MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY;
    CHECK(service.Subscribe(desc, owner) == MMS_OK && Same(x.last, desc));
    MmsUiPropertyRefreshScopedSubscriptionStateV1 state{}; CHECK(owner.State(state) == MMS_OK && state.flags == MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeDependents(x.Property(), x.Seed(), owner, desc.kinds, MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) == MMS_OK && Same(x.last, desc)); CHECK(owner.Release() == MMS_OK);
    CHECK(service.SubscribeSubtree(refresh, x.property, x.seed, owner, MMS_UI_REFRESH_DATA, MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) == MMS_OK &&
        x.last.scope == MMS_UI_REFRESH_SUBTREE && x.last.flags == MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY); CHECK(owner.Release() == MMS_OK);
    const auto caps = x.caps;
    CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_OK && !x.last.flags && x.caps == caps); CHECK(owner.Release() == MMS_OK);
    CHECK(!x.legacySubscribes && x.subscriptions.empty() && x.arguments && !x.unexpected);
}

static void StatesWorkersAndOwnership() {
    Host x; auto service = x.Services(); auto refresh = x.Refresh(); sdk::ScopedPropertyRefreshSubscription owner;
    CHECK(service.SubscribeDependents(x.Property(), x.Seed(), owner) == MMS_OK); const auto handle = owner.get();
    MmsUiPropertyRefreshScopedSubscriptionStateV1 state{}; MmsUiPropertyRefreshSubscriptionStateV1 base{};
    CHECK(owner.State(state) == MMS_OK && owner.BaseState(base) == MMS_OK && Same(base, x.Base(handle)));
    CHECK(state.subscription == handle && Same(state.target, x.Seed()) && Same(state.property, x.Property()) && state.scope == MMS_UI_REFRESH_DEPENDENTS);
    CHECK(state.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT) && !state.flags && !state.reservedTail0 && !state.reservedTail &&
        state.workGeneration == x.prefix + 71 && state.submittedWorkGeneration == x.prefix + 67 && state.completedWorkGeneration == x.prefix + 61 && state.submittedDependencyRevision == x.prefix + 59);
    MmsUiPropertyRefreshSubscriptionStateV1 prefix{}; std::memcpy(&prefix, &state, sizeof(prefix)); prefix.h = sdk::Header<MmsUiPropertyRefreshSubscriptionStateV1>(); CHECK(Same(prefix, base));
    const auto oldState = Sentinel<MmsUiPropertyRefreshScopedSubscriptionStateV1>(); const auto oldBase = Sentinel<MmsUiPropertyRefreshSubscriptionStateV1>();
    for (const auto error : {MMS_E_STALE, MMS_E_NOT_READY, MMS_E_FAILED}) {
        x.stateFault.result = x.baseStateFault.result = error; state = oldState; base = oldBase;
        CHECK(owner.State(state) == error && Same(state, oldState) && owner.BaseState(base) == error && Same(base, oldBase) && owner.get() == handle);
    }
    x.stateFault = x.baseStateFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.stateFault.exception = x.baseStateFault.exception = mode; state = oldState; base = oldBase;
        CHECK(owner.State(state) == Error(mode) && Same(state, oldState) && owner.BaseState(base) == Error(mode) && Same(base, oldBase));
    }
    x.stateFault = x.baseStateFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) {
        x.stateFault.header = x.baseStateFault.header = mode; state = oldState; base = oldBase;
        CHECK(owner.State(state) == MMS_E_VERSION && Same(state, oldState) && owner.BaseState(base) == MMS_E_VERSION && Same(base, oldBase));
    }
    x.stateFault = x.baseStateFault = {};
    auto caps = Sentinel<MmsUiPropertyRefreshScopedCapabilitiesV1>(); const auto oldCaps = caps;
    auto baseCaps = Sentinel<MmsUiPropertyRefreshCapabilitiesV1>(); const auto oldBaseCaps = baseCaps;
    auto ref = Sentinel<MmsUiPropertyRefV1>(); const auto oldRef = ref;
    sdk::ScopedPropertyRefreshSubscription unused;
    const auto capsBefore = x.caps, baseCapsBefore = x.baseCaps, refsBefore = x.refs, targetsBefore = x.targets, subscribed = x.scopedSubscribes, releases = x.releases;
    MmsResult queried = MMS_E_FAILED, baseQueried = MMS_E_FAILED, notified = MMS_E_FAILED;
    std::array<MmsResult, 9> denied{};
    std::thread worker([&] {
        auto copy = service; auto movedService = std::move(copy);
        queried = owner.State(state); baseQueried = owner.BaseState(base); notified = movedService.Notify(x.Property());
        denied[0] = movedService.Capabilities(caps); denied[1] = movedService.BaseCapabilities(baseCaps); denied[2] = movedService.GetPropertyRef(x.property, ref);
        denied[3] = movedService.Subscribe(x.Desc(), unused); denied[4] = movedService.SubscribeSubtree(x.Property(), x.Seed(), unused);
        denied[5] = movedService.SubscribeDependents(x.Property(), x.Seed(), unused); denied[6] = movedService.SubscribeSubtree(refresh, x.property, x.seed, unused);
        denied[7] = movedService.SubscribeDependents(refresh, x.property, x.seed, unused); denied[8] = owner.Release();
    }); worker.join();
    CHECK(queried == MMS_OK && baseQueried == MMS_OK && notified == MMS_OK && state.subscription == handle && Same(base, x.Base(handle)));
    CHECK(x.workerStates == 1 && x.workerBaseStates == 1 && x.workerNotifications == 1 && Same(x.notified, x.Property()));
    for (const auto result : denied) CHECK(result == MMS_E_THREAD);
    CHECK(Same(caps, oldCaps) && Same(baseCaps, oldBaseCaps) && Same(ref, oldRef) && !unused && x.caps == capsBefore && x.baseCaps == baseCapsBefore &&
        x.refs == refsBefore && x.targets == targetsBefore && x.scopedSubscribes == subscribed && x.releases == releases && !x.wrongThread);
    sdk::ScopedPropertyRefreshSubscription moved(std::move(owner)); CHECK(!owner && !owner.get() && moved.get() == handle && owner.Release() == MMS_OK);
    for (const auto error : {MMS_E_BUSY, MMS_E_THREAD, MMS_E_NOT_FOUND, MMS_E_MEMORY, MMS_E_FAILED}) {
        x.releaseFault.result = error; CHECK(moved.Release() == error && moved.get() == handle && moved.State(state) == MMS_OK && moved.BaseState(base) == MMS_OK);
    }
    x.releaseFault = {};
    for (unsigned mode = 1; mode <= 3; ++mode) { x.releaseFault.exception = mode; CHECK(moved.Release() == Error(mode) && moved.get() == handle && x.subscriptions.count(handle)); }
    x.releaseFault = {}; CHECK(moved.Release() == MMS_OK && !moved && x.subscriptions.empty());
    CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_OK); const auto crossing = owner.get();
    std::unique_ptr<sdk::ScopedPropertyRefreshSubscription> returned; MmsResult crossingRelease = MMS_OK; const auto before = x.releases;
    std::thread transfer([&] { returned = std::make_unique<sdk::ScopedPropertyRefreshSubscription>(std::move(owner)); crossingRelease = returned->Release(); queried = returned->State(state); }); transfer.join();
    CHECK(!owner && returned && returned->get() == crossing && crossingRelease == MMS_E_THREAD && queried == MMS_OK && x.releases == before && !x.wrongThread);
    Host replacement(UINT64_C(0x3a00000000000000)); CHECK(sdk::ScopedPropertyRefreshServices::Query(replacement.host, service) == MMS_OK);
    CHECK(returned->BaseState(base) == MMS_OK && Same(base.property, x.Property()) && returned->Release() == MMS_OK && !*returned && !replacement.releases && x.subscriptions.empty());
    CHECK(x.arguments && replacement.arguments && !x.unexpected && !replacement.unexpected);
}

static void SamePoolDetachAndDestructors() {
    Host x; auto service = x.Services(); sdk::PropertyRefreshServices legacy; CHECK(sdk::PropertyRefreshServices::Query(x.host, legacy) == MMS_OK);
    sdk::ScopedPropertyRefreshSubscription owner; CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_OK); auto handle = owner.get();
    MmsUiPropertyRefreshSubscriptionStateV1 base{}; base.h = sdk::Header<MmsUiPropertyRefreshSubscriptionStateV1>();
    CHECK(legacy.api().query(legacy.api().context, handle, &base) == MMS_OK && base.subscription == handle && Same(base.property, x.Property()));
    const auto releases = x.releases; CHECK(owner.detach() == handle && !owner && x.releases == releases);
    CHECK(legacy.api().unsubscribe(legacy.api().context, handle) == MMS_OK && x.subscriptions.empty());
    MmsUiPropertyRefreshSubscriptionDescV1 desc{}; desc.h = sdk::Header<MmsUiPropertyRefreshSubscriptionDescV1>();
    desc.target = x.Seed(); desc.property = x.Property(); desc.kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT;
    CHECK(legacy.api().subscribe(legacy.api().context, &desc, &handle) == MMS_OK);
    MmsUiPropertyRefreshScopedSubscriptionStateV1 state{}; state.h = sdk::Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
    CHECK(service.api().queryScoped(service.api().context, handle, &state) == MMS_OK && state.scope == MMS_UI_REFRESH_SELF && Same(state.target, x.Seed()));
    CHECK(legacy.api().unsubscribe(legacy.api().context, handle) == MMS_OK);
    CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_OK); handle = owner.get(); x.subscriptions.erase(handle);
    const auto sentinel = Sentinel<MmsUiPropertyRefreshScopedSubscriptionStateV1>(); state = sentinel;
    CHECK(owner.State(state) == MMS_E_STALE && Same(state, sentinel) && owner.get() == handle && owner.Release() == MMS_OK && !owner);
    const auto before = x.releases; CHECK(owner.Release() == MMS_OK && !owner.detach() && x.releases == before);
    { sdk::ScopedPropertyRefreshSubscription scoped; CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), scoped) == MMS_OK); handle = scoped.get(); }
    CHECK(!x.subscriptions.count(handle) && x.releases == before + 1);
    { sdk::ScopedPropertyRefreshSubscription scoped; CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), scoped) == MMS_OK); handle = scoped.get(); x.releaseFault.result = MMS_E_BUSY; }
    CHECK(x.subscriptions.count(handle)); x.releaseFault = {}; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    for (unsigned mode = 1; mode <= 3; ++mode) {
        { sdk::ScopedPropertyRefreshSubscription scoped; CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), scoped) == MMS_OK); handle = scoped.get(); x.releaseFault.exception = mode; }
        CHECK(x.subscriptions.count(handle)); x.releaseFault = {}; CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    {
        auto scoped = std::make_unique<sdk::ScopedPropertyRefreshSubscription>(); CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), *scoped) == MMS_OK); handle = scoped->get(); const auto calls = x.releases;
        std::thread worker([owned = std::move(scoped)]() mutable { owned.reset(); }); worker.join();
        CHECK(!scoped && x.releases == calls && x.subscriptions.count(handle) && !x.wrongThread); CHECK(x.table.unsubscribe(x.table.context, handle) == MMS_OK);
    }
    CHECK(service.SubscribeSubtree(x.Property(), x.Seed(), owner) == MMS_OK); handle = owner.get(); MmsHandle detached = 0; const auto calls = x.releases;
    std::thread worker([&] { detached = owner.detach(); }); worker.join();
    CHECK(detached == handle && !owner && x.releases == calls && x.subscriptions.count(handle)); CHECK(x.table.unsubscribe(x.table.context, detached) == MMS_OK);
    CHECK(x.subscriptions.empty() && x.arguments && !x.wrongThread && !x.unexpected);
}
} // namespace cpp_scoped_property_refresh_checks
static void CppScopedPropertyRefreshChecks() {
    cpp_scoped_property_refresh_checks::NegotiationAndEmpty();
    cpp_scoped_property_refresh_checks::ValuesAndExceptions();
    cpp_scoped_property_refresh_checks::SubscriptionsAndConveniences();
    cpp_scoped_property_refresh_checks::ExplicitHistoryCapabilities();
    cpp_scoped_property_refresh_checks::StatesWorkersAndOwnership();
    cpp_scoped_property_refresh_checks::SamePoolDetachAndDestructors();
}
