#include "ui_refresh_binding.hpp"
#include <map>
#include <mutex>
#include <limits>
#include <array>

namespace mikumikustudio::plugins {
namespace {
constexpr uint32_t kSources = 64, kBindings = 128, kLeases = 128, kContributions = 256;
constexpr uint32_t kSubscriptions = 128, kGlobalSubscriptions = 2048;
constexpr uint32_t kSubscriptionKinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT;
constexpr uint64_t kModelBytes = 256 * 1024, kOwnerBytes = 2 * 1024 * 1024, kGlobalBytes = 32 * 1024 * 1024;
bool SameTarget(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) noexcept {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
bool TargetValid(const MmsUiTargetRefV1& target) noexcept {
    return Valid(&target) && target.element && target.instanceGeneration && target.projectEpoch;
}
struct Accounting {
    std::atomic<uint64_t> bytes{0};
    std::atomic<unsigned> contributions{0};
    std::atomic<uint64_t> releasedRevision{0};
};
bool Reserve(std::atomic<uint64_t>& value, uint64_t amount, uint64_t maximum) noexcept {
    auto previous = value.load();
    do { if (previous > maximum || amount > maximum - previous) return false; }
    while (!value.compare_exchange_weak(previous, previous + amount));
    return true;
}
struct Snapshot {
    std::shared_ptr<Accounting> owner, global;
    uint64_t charge = 0;
    // Only a source/display snapshot actually retained by the host creates a
    // capacity event. Rolling back an uncommitted candidate cannot awaken
    // another failed candidate and make two OOM consumers retry each other.
    mutable bool retained = false;
    std::vector<unsigned char> bytes;
    ~Snapshot() { if (charge) {
        owner->bytes.fetch_sub(charge); global->bytes.fetch_sub(charge);
        if (retained) global->releasedRevision.fetch_add(1);
    } }
    MmsBytes View() const noexcept { return {bytes.empty() ? nullptr : bytes.data(), bytes.size()}; }
};
MmsResult CopySnapshot(const std::shared_ptr<Accounting>& owner, const std::shared_ptr<Accounting>& global,
                       MmsBytes bytes, uint64_t maximum, std::shared_ptr<const Snapshot>& out) {
    if ((bytes.bytes && !bytes.data) || bytes.bytes > maximum) return MMS_E_ARGUMENT;
    auto next = std::make_shared<Snapshot>(); next->owner = owner; next->global = global;
    if (!Reserve(owner->bytes, bytes.bytes, kOwnerBytes)) return MMS_E_BUSY;
    if (!Reserve(global->bytes, bytes.bytes, kGlobalBytes)) { owner->bytes.fetch_sub(bytes.bytes); return MMS_E_BUSY; }
    next->charge = bytes.bytes;
    next->bytes.resize(static_cast<size_t>(bytes.bytes));
    if (bytes.bytes) std::memcpy(next->bytes.data(), bytes.data, static_cast<size_t>(bytes.bytes));
    out = std::move(next); return MMS_OK;
}
bool Equal(const std::shared_ptr<const Snapshot>& a, MmsBytes b) noexcept {
    return a && a->bytes.size() == b.bytes && (!b.bytes || !std::memcmp(a->bytes.data(), b.data, size_t(b.bytes)));
}
}

struct UiRefreshBindings::Impl {
    struct Owner {
        Impl* host;
        RefreshClient* client;
        std::shared_ptr<Accounting> accounting = std::make_shared<Accounting>();
        std::atomic<bool> retired{false};
        unsigned sources = 0, bindings = 0, leases = 0, builders = 0, subscriptions = 0;
    };
    struct Source {
        MmsHandle handle = 0;
        std::shared_ptr<Owner> owner;
        std::string id;
        uint64_t epoch = 0, revision = 0, maximum = 0;
        std::shared_ptr<const Snapshot> snapshot;
    };
    struct Binding {
        MmsHandle handle = 0, source = 0;
        std::shared_ptr<Owner> owner;
        MmsUiTargetRefV1 target{};
        void* context = nullptr;
        MmsUiDataPrepareCallbackV1 prepare = nullptr;
        uint64_t maximum = 0, consumed = 0, displayRevision = 0;
        bool active = true;
        std::shared_ptr<const Snapshot> display;
    };
    struct Lease { std::shared_ptr<Owner> owner; std::shared_ptr<const Snapshot> display; };
    struct Subscription {
        MmsHandle handle = 0, binding = 0, ticket = 0;
        std::shared_ptr<Owner> owner;
        uint32_t kinds = 0;
        uint64_t latest = 0, submitted = 0, completed = 0;
        MmsResult lastResult = MMS_OK;
        bool armed = false, claimed = false;
        uint32_t waitEvents = 0;
        uint64_t waitCapacityRevision = 0, waitQueueCapacityRevision = 0;
        MmsUiTargetRefV1 seed{};
        uint32_t scope = MMS_UI_REFRESH_SELF, waitingFor = 0, scopeBlock = 0;
        uint64_t workGeneration = 1, submittedWorkGeneration = 0, completedWorkGeneration = 0;
        uint64_t invalidationGeneration = 1, dependencyRevision = 0, submittedDependencyRevision = 0;
        uint64_t sourcePublication = 0, claimedSourcePublication = 0, submittedSourcePublication = 0;
        bool observed = false;
        std::array<MmsUiTargetRefV1, 256> members{};
        size_t memberCount = 0;
        std::array<MmsHandle, kSources> watchedSources{};
        size_t sourceCount = 0;
    };
    struct Builder {
        std::shared_ptr<Owner> owner;
        uint64_t maximum = 0;
        bool written = false;
        MmsResult failure = MMS_OK;
        uint64_t failureCapacityRevision = 0;
        std::shared_ptr<const Snapshot> candidate;
    };
    struct Flight {
        RefreshClient& client;
        explicit Flight(Owner& owner) : client(*owner.client) { client.inFlight.fetch_add(1); }
        ~Flight() { client.inFlight.fetch_sub(1); }
    };
    mutable std::mutex mutex;
    DWORD thread = GetCurrentThreadId();
    uint64_t epoch = 1, next = 1;
    Hooks hooks;
    std::shared_ptr<Accounting> global = std::make_shared<Accounting>();
    std::map<RefreshClient*, std::shared_ptr<Owner>> owners;
    // API contexts remain stable until the manager has released all plugin tables.
    std::vector<std::shared_ptr<Owner>> contexts;
    std::map<MmsHandle, Source> sources;
    std::map<MmsHandle, std::shared_ptr<Binding>> bindings;
    std::map<MmsHandle, Subscription> subscriptions;
    std::map<MmsHandle, Lease> leases;
    std::map<MmsHandle, Builder*> builders;

    explicit Impl(Hooks value) : hooks(std::move(value)) {}
    static bool Scoped(const Subscription& s) noexcept { return s.scope != MMS_UI_REFRESH_SELF; }
    static bool Watches(const Subscription& s, const MmsUiTargetRefV1& ref) noexcept {
        if (SameTarget(s.seed, ref)) return true;
        for (size_t i = 0; i < s.memberCount; ++i) if (SameTarget(s.members[i], ref)) return true;
        return false;
    }
    void WatchSources(Subscription& s) noexcept {
        s.sourceCount = 0;
        const auto add = [&](MmsHandle source) {
            if (std::find(s.watchedSources.begin(), s.watchedSources.begin() + s.sourceCount, source) == s.watchedSources.begin() + s.sourceCount &&
                s.sourceCount < s.watchedSources.size()) s.watchedSources[s.sourceCount++] = source;
        };
        const auto trigger = bindings.find(s.binding); if (trigger != bindings.end()) add(trigger->second->source);
        for (const auto& entry : bindings) {
            if (entry.second->owner != s.owner) continue;
            for (size_t i = 0; i < s.memberCount; ++i) if (SameTarget(s.members[i], entry.second->target)) { add(entry.second->source); break; }
        }
    }
    bool SourcesReady(const Subscription& s) const noexcept {
        for (size_t i = 0; i < s.sourceCount; ++i) {
            const auto source = sources.find(s.watchedSources[i]);
            if (source == sources.end() || !source->second.snapshot || !source->second.revision) return false;
        }
        return s.latest != 0;
    }
    void ArmScoped(Subscription& s) noexcept {
        const auto block = s.scopeBlock | (SourcesReady(s) ? 0u : MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE);
        if (s.ticket || s.claimed) return;
        s.armed = !block; s.waitingFor = block;
        s.waitEvents = block & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET ? RetryTargetPublication : 0;
    }
    void ChangeScoped(Subscription& s, bool invalidate) noexcept {
        if (s.workGeneration == UINT64_MAX) {
            s.armed = false; s.waitEvents = 0; s.waitingFor = MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER; s.lastResult = MMS_E_BUSY; return;
        }
        ++s.workGeneration;
        if (invalidate) s.invalidationGeneration = s.workGeneration;
        ArmScoped(s);
    }
    void BindingPublication(const MmsUiTargetRefV1& target) noexcept {
        for (auto& entry : subscriptions) {
            auto& sub = entry.second;
            if (!Scoped(sub)) continue;
            bool member = false;
            for (size_t i = 0; !member && i < sub.memberCount; ++i) member = SameTarget(sub.members[i], target);
            if (!member) continue;
            WatchSources(sub); ChangeScoped(sub, true);
        }
    }
    bool ReadOnly() const { return GetCurrentThreadId() == thread && hooks.inCallback && hooks.inCallback(); }
    MmsResult Mutation(Owner& owner, bool ui) const {
        if (ui && GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        return owner.retired || owner.client->stopping.load() ? MMS_E_NOT_READY : MMS_OK;
    }
    MmsHandle Next() { if (next == UINT64_MAX) throw std::bad_alloc(); return next++; }
    std::shared_ptr<Owner> For(RefreshClient& client) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = owners.find(&client);
        if (found != owners.end() && (!found->second->retired.load() || client.stopping.load())) return found->second;
        if (found != owners.end() && (found->second->leases || found->second->builders ||
            found->second->accounting->contributions.load())) throw std::invalid_argument("DATA owner still leased");
        auto owner = std::make_shared<Owner>(); owner->host = this; owner->client = &client;
        contexts.push_back(owner); owners[&client] = owner; return owner;
    }
    MmsResult Caps(Owner& owner, MmsUiDataCapabilitiesV1* out) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        *out = {Header<MmsUiDataCapabilitiesV1>(), kSources, kBindings, kLeases, 1,
            kModelBytes, kModelBytes, kOwnerBytes, kContributions, 0}; return MMS_OK;
    }
    MmsResult Register(Owner& owner, const MmsUiDataSourceDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || desc->flags || desc->reserved || !desc->maxBytes || desc->maxBytes > kModelBytes) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, true)) return r;
        auto id = String(desc->id); if (!ValidId(id)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        if (owner.sources >= kSources || sources.size() >= 1024) return MMS_E_BUSY;
        for (const auto& item : sources) if (item.second.owner.get() == &owner && item.second.id == id) return MMS_E_CONFLICT;
        const auto handle = Next();
        Source source; source.handle = handle; source.owner = owners.at(owner.client); source.id = std::move(id);
        source.epoch = epoch; source.maximum = desc->maxBytes;
        sources.emplace(handle, std::move(source)); ++owner.sources; *out = handle; return MMS_OK;
    }
    MmsResult SourceState(Owner& owner, MmsHandle handle, MmsUiDataSourceStateV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        auto found = sources.find(handle); if (found == sources.end()) return MMS_E_STALE;
        const auto& s = found->second;
        if (s.owner.get() != &owner) return MMS_E_ARGUMENT;
        *out = {Header<MmsUiDataSourceStateV1>(), s.handle, s.revision, s.epoch, s.snapshot ? s.snapshot->bytes.size() : 0}; return MMS_OK;
    }
    MmsResult Publish(Owner& owner, const MmsUiDataSourcePublishV1* update, MmsUiDataSourceStateV1* out) {
        if (!Valid(update) || !Valid(out) || update->flags || update->reserved || (update->bytes.bytes && !update->bytes.data)) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, false)) return r;
        bool wake = false, capacityReleased = false;
        std::unique_lock<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        auto found = sources.find(update->source); if (found == sources.end()) return MMS_E_STALE;
        auto& s = found->second; if (s.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (s.epoch != epoch) return MMS_E_STALE;
        if (update->expectedRevision != s.revision) return MMS_E_CONFLICT;
        if (update->bytes.bytes > s.maximum) return MMS_E_ARGUMENT;
        if (!Equal(s.snapshot, update->bytes)) {
            if (s.revision == UINT64_MAX) return MMS_E_BUSY;
            std::shared_ptr<const Snapshot> candidate;
            if (const auto r = CopySnapshot(owner.accounting, global, update->bytes, s.maximum, candidate)) return r;
            const auto releasedBefore = global->releasedRevision.load();
            candidate->retained = true; s.snapshot = std::move(candidate); ++s.revision;
            capacityReleased = global->releasedRevision.load() != releasedBefore;
            // All slots were allocated at subscription time. Publishing only
            // updates bounded scalar mailboxes while holding this lock.
            for (auto& entry : subscriptions) {
                auto& sub = entry.second;
                const auto binding = bindings.find(sub.binding);
                if (binding == bindings.end()) continue;
                if (binding->second->source != s.handle) {
                    if (!Scoped(sub) || sub.owner.get() != &owner ||
                        std::find(sub.watchedSources.begin(), sub.watchedSources.begin() + sub.sourceCount, s.handle) == sub.watchedSources.begin() + sub.sourceCount) continue;
                    if (sub.sourcePublication != UINT64_MAX) ++sub.sourcePublication;
                    // A contextual source is not another trigger. Its real
                    // publication only recovers missing/failed consumption.
                    if (!sub.ticket && !sub.claimed && (sub.waitingFor &
                        (MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE | MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER))) {
                        const bool armed = sub.armed; ChangeScoped(sub, false); wake |= !armed && sub.armed;
                    }
                    continue;
                }
                sub.latest = s.revision;
                if (Scoped(sub)) {
                    const bool armed = sub.armed; ChangeScoped(sub, false); wake |= !armed && sub.armed && !sub.claimed && !sub.ticket;
                    continue;
                }
                if (sub.workGeneration != UINT64_MAX) ++sub.workGeneration;
                if (!sub.ticket) {
                    wake |= !sub.armed && !sub.claimed;
                    sub.armed = true; sub.waitEvents = 0;
                }
            }
        }
        *out = {Header<MmsUiDataSourceStateV1>(), s.handle, s.revision, s.epoch, s.snapshot->bytes.size()};
        lock.unlock();
        if (wake && hooks.wakeSubscriptions) hooks.wakeSubscriptions();
        // Replacing an unsubscribed source can free the retained bytes that
        // another binding's display is waiting for. Notify only after the
        // lock and only when an old retained snapshot was really released.
        if (capacityReleased && hooks.capacityReleased) hooks.capacityReleased();
        return MMS_OK;
    }
    void RemoveBinding(std::map<MmsHandle, std::shared_ptr<Binding>>::iterator found) noexcept {
        const auto target = found->second->target;
        for (auto it = subscriptions.begin(); it != subscriptions.end();) {
            if (it->second.binding == found->first) { --it->second.owner->subscriptions; it = subscriptions.erase(it); }
            else ++it;
        }
        found->second->active = false; --found->second->owner->bindings; bindings.erase(found);
        BindingPublication(target);
    }
    MmsResult Unregister(Owner& owner, MmsHandle handle) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        std::unique_lock<std::mutex> lock(mutex);
        auto found = sources.find(handle); if (found == sources.end()) return MMS_E_STALE;
        if (found->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        for (auto it = bindings.begin(); it != bindings.end();) {
            if (it->second->source == handle) { auto erase = it++; RemoveBinding(erase); } else ++it;
        }
        --owner.sources; sources.erase(found); lock.unlock();
        if (hooks.bindingChanged) hooks.bindingChanged();
        if (hooks.cleanupSubscriptions) hooks.cleanupSubscriptions();
        if (hooks.capacityReleased) hooks.capacityReleased();
        return MMS_OK;
    }
    MmsResult Bind(Owner& owner, const MmsUiDataBindingDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || !TargetValid(desc->target) || !desc->source || !desc->maxDisplayBytes ||
            desc->maxDisplayBytes > kModelBytes || desc->flags || desc->reserved) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, true)) return r;
        if (!hooks.validateTarget) return MMS_E_NOT_READY;
        if (const auto r = hooks.validateTarget(*owner.client, desc->target)) return r;
        auto binding = std::make_shared<Binding>();
        std::unique_lock<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        if (desc->target.projectEpoch != epoch) return MMS_E_STALE;
        auto source = sources.find(desc->source); if (source == sources.end() || source->second.epoch != epoch) return MMS_E_STALE;
        if (source->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (owner.bindings >= kBindings || bindings.size() >= 2048) return MMS_E_BUSY;
        for (const auto& entry : bindings) if (SameTarget(entry.second->target, desc->target)) return MMS_E_CONFLICT;
        binding->handle = Next(); binding->source = desc->source; binding->owner = source->second.owner;
        binding->target = desc->target; binding->context = desc->context; binding->prepare = desc->prepare; binding->maximum = desc->maxDisplayBytes;
        bindings.emplace(binding->handle, binding); ++owner.bindings; *out = binding->handle;
        BindingPublication(binding->target);
        lock.unlock(); if (hooks.bindingChanged) hooks.bindingChanged();
        if (hooks.cleanupSubscriptions) hooks.cleanupSubscriptions(); return MMS_OK;
    }
    MmsResult Unbind(Owner& owner, MmsHandle handle) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        std::unique_lock<std::mutex> lock(mutex);
        auto found = bindings.find(handle); if (found == bindings.end()) return MMS_E_STALE;
        if (found->second->owner.get() != &owner) return MMS_E_ARGUMENT;
        RemoveBinding(found); lock.unlock();
        if (hooks.bindingChanged) hooks.bindingChanged();
        if (hooks.cleanupSubscriptions) hooks.cleanupSubscriptions();
        if (hooks.capacityReleased) hooks.capacityReleased();
        return MMS_OK;
    }
    MmsResult SubscriptionCaps(Owner& owner, MmsUiRefreshSubscriptionCapabilitiesV1* out, bool scoped = false) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, true)) return r;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        *out = {Header<MmsUiRefreshSubscriptionCapabilitiesV1>(), kSubscriptions, kSubscriptionKinds,
            uint32_t(MMS_UI_REFRESH_SELF | (scoped && hooks.scopedSubscriptionTarget ? MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS : 0)), 0, {0, 0}};
        return MMS_OK;
    }
    MmsResult Subscribe(Owner& owner, const MmsUiRefreshSubscriptionDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || !desc->binding || !(desc->kinds & MMS_UI_REFRESH_DATA) ||
            (desc->kinds & ~kSubscriptionKinds) || desc->flags || desc->reserved) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, true)) return r;
        MmsUiTargetRefV1 target{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = bindings.find(desc->binding);
            if (found == bindings.end()) return MMS_E_STALE;
            if (found->second->owner.get() != &owner) return MMS_E_ARGUMENT;
            target = found->second->target;
        }
        if (!hooks.subscriptionTarget) return MMS_E_NOT_READY;
        if (const auto r = hooks.subscriptionTarget(*owner.client, target, desc->kinds)) return r;
        std::unique_lock<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        const auto binding = bindings.find(desc->binding);
        if (binding == bindings.end() || !binding->second->active || !SameTarget(binding->second->target, target) || target.projectEpoch != epoch)
            return MMS_E_STALE;
        if (binding->second->owner.get() != &owner) return MMS_E_ARGUMENT;
        for (const auto& entry : subscriptions) if (entry.second.binding == desc->binding) return MMS_E_CONFLICT;
        if (owner.subscriptions >= kSubscriptions || subscriptions.size() >= kGlobalSubscriptions) return MMS_E_BUSY;
        const auto source = sources.find(binding->second->source);
        if (source == sources.end() || source->second.epoch != epoch) return MMS_E_STALE;
        Subscription sub; sub.handle = Next(); sub.binding = desc->binding; sub.owner = binding->second->owner;
        sub.seed = target;
        sub.kinds = desc->kinds; sub.latest = source->second.revision; sub.armed = bool(source->second.snapshot);
        const auto handle = sub.handle; const auto wake = sub.armed;
        subscriptions.emplace(handle, std::move(sub)); ++owner.subscriptions; *out = handle;
        lock.unlock();
        if (wake && hooks.wakeSubscriptions) hooks.wakeSubscriptions();
        return MMS_OK;
    }
    MmsResult SubscribeScoped(Owner& owner, const MmsUiRefreshScopedSubscriptionDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || !desc->binding || !TargetValid(desc->seed) || desc->flags || desc->reserved ||
            desc->reservedTail[0] || desc->reservedTail[1] || !(desc->kinds & MMS_UI_REFRESH_DATA) || desc->kinds & ~kSubscriptionKinds ||
            (desc->scope != MMS_UI_REFRESH_SELF && desc->scope != MMS_UI_REFRESH_SUBTREE && desc->scope != MMS_UI_REFRESH_DEPENDENTS)) return MMS_E_ARGUMENT;
        if (const auto result = Mutation(owner, true)) return result;
        MmsUiTargetRefV1 trigger{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto binding = bindings.find(desc->binding);
            if (binding == bindings.end()) return MMS_E_STALE;
            if (binding->second->owner.get() != &owner) return MMS_E_ARGUMENT;
            trigger = binding->second->target;
        }
        if (desc->scope == MMS_UI_REFRESH_SELF) {
            if (!SameTarget(desc->seed, trigger)) return MMS_E_ARGUMENT;
            const MmsUiRefreshSubscriptionDescV1 legacy{Header<MmsUiRefreshSubscriptionDescV1>(), desc->binding, desc->kinds, 0, 0};
            return Subscribe(owner, &legacy, out);
        }
        if (!hooks.scopedSubscriptionTarget) return MMS_E_NOT_READY;
        if (const auto result = hooks.scopedSubscriptionTarget(*owner.client, desc->seed, desc->scope, desc->kinds)) return result;
        std::unique_lock<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        const auto binding = bindings.find(desc->binding);
        if (binding == bindings.end() || !binding->second->active || !SameTarget(trigger, binding->second->target) || desc->seed.projectEpoch != epoch) return MMS_E_STALE;
        if (binding->second->owner.get() != &owner) return MMS_E_ARGUMENT;
        for (const auto& entry : subscriptions) if (entry.second.binding == desc->binding && entry.second.scope == desc->scope && SameTarget(entry.second.seed, desc->seed)) return MMS_E_CONFLICT;
        if (owner.subscriptions >= kSubscriptions || subscriptions.size() >= kGlobalSubscriptions) return MMS_E_BUSY;
        const auto source = sources.find(binding->second->source);
        if (source == sources.end() || source->second.epoch != epoch) return MMS_E_STALE;
        Subscription sub; sub.handle = Next(); sub.binding = desc->binding; sub.owner = binding->second->owner;
        sub.seed = desc->seed; sub.scope = desc->scope; sub.kinds = desc->kinds; sub.latest = source->second.revision;
        WatchSources(sub); ArmScoped(sub);
        const auto handle = sub.handle; const bool wake = sub.armed;
        subscriptions.emplace(handle, std::move(sub)); ++owner.subscriptions; *out = handle;
        lock.unlock(); if (wake && hooks.wakeSubscriptions) hooks.wakeSubscriptions(); return MMS_OK;
    }
    static uint32_t WaitingFor(const Subscription& s) noexcept {
        if (Scoped(s)) return s.waitingFor;
        return (s.waitEvents & RetryQueueCapacity ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY : 0) |
            (s.waitEvents & RetryTargetPublication ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET : 0) |
            (s.waitEvents & RetryDisplayCapacity ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY : 0) |
            (s.waitEvents & RetryConsumerPublication ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER : 0) |
            (s.waitEvents & RetryHostWake ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE : 0);
    }
    MmsResult QueryScoped(Owner& owner, MmsHandle handle, MmsUiRefreshScopedSubscriptionStateV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = subscriptions.find(handle); if (found == subscriptions.end()) return MMS_E_STALE;
        const auto& s = found->second;
        if (s.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (owner.retired || owner.client->stopping.load() || !bindings.count(s.binding)) return MMS_E_STALE;
        *out = {Header<MmsUiRefreshScopedSubscriptionStateV1>(), s.handle, s.binding, s.seed.projectEpoch,
            s.latest, s.submitted, s.completed, s.lastResult, uint32_t(s.ticket || s.armed || s.waitEvents || s.claimed || s.waitingFor), 0,
            s.seed, s.scope, s.kinds, s.workGeneration, s.submittedWorkGeneration, s.completedWorkGeneration,
            s.submittedDependencyRevision, WaitingFor(s), 0, 0}; return MMS_OK;
    }
    MmsResult QuerySubscription(Owner& owner, MmsHandle handle, MmsUiRefreshSubscriptionStateV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = subscriptions.find(handle);
        if (found == subscriptions.end()) return MMS_E_STALE;
        const auto& s = found->second;
        if (s.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (owner.retired || owner.client->stopping.load()) return MMS_E_STALE;
        const auto binding = bindings.find(s.binding);
        if (binding == bindings.end()) return MMS_E_STALE;
        *out = {Header<MmsUiRefreshSubscriptionStateV1>(), s.handle, s.binding, binding->second->target.projectEpoch,
            s.latest, s.submitted, s.completed, s.lastResult, uint32_t(s.ticket || s.armed || s.waitEvents || s.claimed || s.waitingFor), 0};
        return MMS_OK;
    }
    MmsResult Unsubscribe(Owner& owner, MmsHandle handle) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        std::unique_lock<std::mutex> lock(mutex);
        const auto found = subscriptions.find(handle);
        if (found == subscriptions.end()) return MMS_E_STALE;
        if (found->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        --owner.subscriptions; subscriptions.erase(found); lock.unlock();
        if (hooks.cleanupSubscriptions) hooks.cleanupSubscriptions();
        return MMS_OK;
    }
    MmsResult WritePrepared(Owner& owner, MmsHandle handle, MmsBytes bytes) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        std::lock_guard<std::mutex> lock(mutex);
        auto found = builders.find(handle); if (found == builders.end()) return MMS_E_STALE;
        auto& builder = *found->second;
        if (builder.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (owner.retired || owner.client->stopping.load()) return builder.failure = MMS_E_CANCELLED;
        if (builder.failure) return builder.failure;
        if (builder.written) return builder.failure = MMS_E_CONFLICT;
        const auto capacityRevision = global->releasedRevision.load();
        const auto result = Boundary([&] { return CopySnapshot(owner.accounting, global, bytes, builder.maximum, builder.candidate); });
        if (result) { builder.failureCapacityRevision = capacityRevision; return builder.failure = result; }
        builder.written = true; return MMS_OK;
    }
    MmsResult Acquire(Owner& owner, MmsHandle handle, MmsUiDataDisplayV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        auto found = bindings.find(handle); if (found == bindings.end()) return MMS_E_STALE;
        const auto& b = *found->second; if (b.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (!b.display) return MMS_E_NOT_READY;
        if (owner.leases >= kLeases || leases.size() >= 4096) return MMS_E_BUSY;
        const auto lease = Next(); leases.emplace(lease, Lease{b.owner, b.display}); ++owner.leases;
        *out = {Header<MmsUiDataDisplayV1>(), lease, b.handle, b.source, b.consumed, b.displayRevision, b.target.projectEpoch, b.display->View()}; return MMS_OK;
    }
    MmsResult Release(Owner& owner, MmsHandle handle) {
        std::unique_lock<std::mutex> lock(mutex);
        auto found = leases.find(handle); if (found == leases.end()) return MMS_E_NOT_FOUND;
        if (found->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        --owner.leases; leases.erase(found); lock.unlock();
        if (hooks.capacityReleased) hooks.capacityReleased();
        return MMS_OK;
    }
};

struct UiRefreshBindings::DataContribution {
    std::shared_ptr<Impl::Binding> binding;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<Accounting> global;
    uint64_t revision = 0;
    bool charged = false;
    ~DataContribution() {
        if (charged) { binding->owner->accounting->contributions.fetch_sub(1); global->contributions.fetch_sub(1); }
    }
};

UiRefreshBindings::UiRefreshBindings(Hooks hooks) : impl_(std::make_unique<Impl>(std::move(hooks))) {}
UiRefreshBindings::~UiRefreshBindings() = default;
void UiRefreshBindings::SetHooks(Hooks hooks) {
    if (GetCurrentThreadId() != impl_->thread) throw std::invalid_argument("DATA hooks thread");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->contexts.empty()) throw std::invalid_argument("DATA hooks already in use");
    impl_->hooks = std::move(hooks);
}
MmsUiRefreshBindingApiV1 UiRefreshBindings::Api(RefreshClient& client) {
    auto owner = impl_->For(client);
    MmsUiRefreshBindingApiV1 api{}; api.h = Header<MmsUiRefreshBindingApiV1>(); api.context = owner.get();
#define DATA_CALL(body) auto& o = *static_cast<Impl::Owner*>(p); Impl::Flight flight(o); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiDataCapabilitiesV1* out) { DATA_CALL(return o.host->Caps(o, out)); };
    api.registerSource = [](void* p, const MmsUiDataSourceDescV1* desc, MmsHandle* out) { DATA_CALL(return o.host->Register(o, desc, out)); };
    api.publishSource = [](void* p, const MmsUiDataSourcePublishV1* update, MmsUiDataSourceStateV1* out) { DATA_CALL(return o.host->Publish(o, update, out)); };
    api.getSource = [](void* p, MmsHandle h, MmsUiDataSourceStateV1* out) { DATA_CALL(return o.host->SourceState(o, h, out)); };
    api.unregisterSource = [](void* p, MmsHandle h) { DATA_CALL(return o.host->Unregister(o, h)); };
    api.bind = [](void* p, const MmsUiDataBindingDescV1* desc, MmsHandle* out) { DATA_CALL(return o.host->Bind(o, desc, out)); };
    api.unbind = [](void* p, MmsHandle h) { DATA_CALL(return o.host->Unbind(o, h)); };
    api.writePrepared = [](void* p, MmsHandle h, MmsBytes bytes) { DATA_CALL(return o.host->WritePrepared(o, h, bytes)); };
    api.acquireDisplay = [](void* p, MmsHandle h, MmsUiDataDisplayV1* out) { DATA_CALL(return o.host->Acquire(o, h, out)); };
    api.releaseDisplay = [](void* p, MmsHandle h) { DATA_CALL(return o.host->Release(o, h)); };
#undef DATA_CALL
    return api;
}
MmsUiRefreshSubscriptionApiV1 UiRefreshBindings::SubscriptionApi(RefreshClient& client) {
    auto owner = impl_->For(client);
    MmsUiRefreshSubscriptionApiV1 api{}; api.h = Header<MmsUiRefreshSubscriptionApiV1>(); api.context = owner.get();
#define SUBSCRIPTION_CALL(body) auto& o = *static_cast<Impl::Owner*>(p); Impl::Flight flight(o); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiRefreshSubscriptionCapabilitiesV1* out) { SUBSCRIPTION_CALL(return o.host->SubscriptionCaps(o, out)); };
    api.subscribe = [](void* p, const MmsUiRefreshSubscriptionDescV1* desc, MmsHandle* out) { SUBSCRIPTION_CALL(return o.host->Subscribe(o, desc, out)); };
    api.query = [](void* p, MmsHandle h, MmsUiRefreshSubscriptionStateV1* out) { SUBSCRIPTION_CALL(return o.host->QuerySubscription(o, h, out)); };
    api.unsubscribe = [](void* p, MmsHandle h) { SUBSCRIPTION_CALL(return o.host->Unsubscribe(o, h)); };
#undef SUBSCRIPTION_CALL
    return api;
}
MmsUiRefreshSubscriptionApiV1_1 UiRefreshBindings::ScopedSubscriptionApi(RefreshClient& client) {
    const auto legacy = SubscriptionApi(client);
    MmsUiRefreshSubscriptionApiV1_1 api{}; api.h = {sizeof(api), 1, 1}; api.context = legacy.context;
    api.subscribe = legacy.subscribe; api.query = legacy.query; api.unsubscribe = legacy.unsubscribe;
#define SCOPED_CALL(body) auto& o = *static_cast<Impl::Owner*>(p); Impl::Flight flight(o); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiRefreshSubscriptionCapabilitiesV1* out) { SCOPED_CALL(return o.host->SubscriptionCaps(o, out, true)); };
    api.subscribeScoped = [](void* p, const MmsUiRefreshScopedSubscriptionDescV1* desc, MmsHandle* out) { SCOPED_CALL(return o.host->SubscribeScoped(o, desc, out)); };
    api.queryScoped = [](void* p, MmsHandle handle, MmsUiRefreshScopedSubscriptionStateV1* out) { SCOPED_CALL(return o.host->QueryScoped(o, handle, out)); };
#undef SCOPED_CALL
    return api;
}
bool UiRefreshBindings::TakeSubscription(MmsHandle after, SubscriptionWork& out) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto it = state.subscriptions.upper_bound(after); it != state.subscriptions.end(); ++it) {
        auto& s = it->second;
        if (!s.armed || s.ticket || s.claimed || s.owner->retired || s.owner->client->stopping.load()) continue;
        const auto binding = state.bindings.find(s.binding);
        if (binding == state.bindings.end()) continue;
        out = {s.owner->client, s.handle, s.binding, binding->second->target, s.kinds, s.latest, state.global->releasedRevision.load(), 0};
        out.seed = s.seed; out.scope = s.scope; out.workGeneration = s.workGeneration; out.dependencyRevision = s.dependencyRevision;
        out.sourcePublication = s.sourcePublication;
        s.armed = false; s.claimed = true; return true;
    }
    return false;
}
bool UiRefreshBindings::SubmitSubscription(const SubscriptionWork& work, MmsHandle ticket, MmsResult result) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.subscriptions.find(work.subscription);
    if (found == state.subscriptions.end() || found->second.owner->client != work.client) return false;
    auto& s = found->second; s.claimed = false; s.lastResult = result;
    if (!result) {
        s.ticket = ticket; s.submitted = work.revision; s.armed = false; s.waitEvents = 0;
        s.submittedWorkGeneration = work.workGeneration; s.submittedDependencyRevision = work.dependencyRevision;
        s.submittedSourcePublication = s.claimedSourcePublication; s.waitingFor = 0;
    } else if (Impl::Scoped(s)) {
        s.waitCapacityRevision = work.capacityRevision; s.waitQueueCapacityRevision = work.queueCapacityRevision;
        if (s.workGeneration > work.workGeneration || s.latest > work.revision) state.ArmScoped(s);
        else {
            s.armed = false;
            s.waitingFor = work.waitingFor ? work.waitingFor : result == MMS_E_BUSY ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY :
                result == MMS_E_NOT_READY ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET : result == MMS_E_MEMORY ?
                MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY | MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY :
                result == MMS_E_FAILED ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE : MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER;
            s.waitEvents = (s.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY ? RetryQueueCapacity : 0) |
                (s.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY ? RetryDisplayCapacity : 0) |
                (s.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET ? RetryTargetPublication : 0) |
                (s.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER ? RetryConsumerPublication : 0) |
                (s.waitingFor & MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE ? RetryHostWake : 0);
            // A source can publish after a failed cohort capture and before
            // this mailbox is parked. Replay only that newer relevant event.
            if (s.sourcePublication > work.sourcePublication && state.SourcesReady(s) &&
                (s.waitingFor & (MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE | MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER))) state.ChangeScoped(s, false);
        }
    } else if (s.latest > work.revision) {
        s.armed = true; s.waitEvents = 0;
    } else {
        s.armed = false;
        s.waitEvents = result == MMS_E_BUSY ? RetryQueueCapacity : result == MMS_E_NOT_READY ? RetryTargetPublication :
            result == MMS_E_MEMORY ? RetryQueueCapacity | RetryDisplayCapacity : result == MMS_E_FAILED ? RetryHostWake : 0;
        s.waitCapacityRevision = work.capacityRevision; s.waitQueueCapacityRevision = work.queueCapacityRevision;
    }
    return true;
}
void UiRefreshBindings::CompleteSubscription(MmsHandle handle, MmsHandle ticket, uint64_t revision, MmsResult result,
                                             uint32_t waitEvents, uint64_t capacityRevision, uint64_t workGeneration) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.subscriptions.find(handle);
    if (found == state.subscriptions.end() || found->second.ticket != ticket) return;
    auto& s = found->second; s.ticket = 0; s.lastResult = result;
    if (!result) s.completed = std::max(s.completed, revision);
    if (!result) s.completedWorkGeneration = std::max(s.completedWorkGeneration, workGeneration ? workGeneration : s.submittedWorkGeneration);
    if (Impl::Scoped(s)) {
        if (result && s.sourcePublication > s.submittedSourcePublication && s.workGeneration == workGeneration) state.ChangeScoped(s, false);
        if (s.workGeneration > workGeneration || s.latest > revision) state.ArmScoped(s);
        else if (result) {
            s.armed = false;
            s.waitEvents = waitEvents ? waitEvents : RetryConsumerPublication;
            s.waitingFor = (s.waitEvents & RetryDisplayCapacity ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_DISPLAY_CAPACITY : 0) |
                (s.waitEvents & RetryQueueCapacity ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_QUEUE_CAPACITY : 0) |
                (s.waitEvents & RetryTargetPublication ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET : 0) |
                (s.waitEvents & RetryHostWake ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE : 0) |
                (s.waitEvents & RetryConsumerPublication ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER : 0);
            s.waitingFor |= s.scopeBlock;
        } else { s.armed = false; s.waitEvents = 0; s.waitingFor = 0; }
        s.waitCapacityRevision = capacityRevision == UINT64_MAX ? state.global->releasedRevision.load() : capacityRevision;
        return;
    }
    s.armed = s.latest > s.submitted;
    s.waitEvents = !s.armed && (result == MMS_E_BUSY || result == MMS_E_NOT_READY || result == MMS_E_MEMORY) ?
        (waitEvents ? waitEvents : result == MMS_E_NOT_READY ? RetryTargetPublication : RetryConsumerPublication) : 0;
    s.waitCapacityRevision = capacityRevision == UINT64_MAX ? state.global->releasedRevision.load() : capacityRevision;
}
bool UiRefreshBindings::SubscriptionAlive(const RefreshClient& client, MmsHandle handle, uint64_t workGeneration) const noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.subscriptions.find(handle);
    return found != state.subscriptions.end() && found->second.owner->client == &client &&
        !found->second.owner->retired && !client.stopping.load() &&
        (!workGeneration || !Impl::Scoped(found->second) || workGeneration >= found->second.invalidationGeneration);
}
bool UiRefreshBindings::NextScopedSubscription(MmsHandle after, SubscriptionWork& out) const noexcept {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto it = state.subscriptions.upper_bound(after); it != state.subscriptions.end(); ++it) {
        const auto& s = it->second;
        if (!Impl::Scoped(s) || s.owner->retired || s.owner->client->stopping.load()) continue;
        out = {}; out.client = s.owner->client; out.subscription = s.handle; out.binding = s.binding;
        out.seed = s.seed; out.scope = s.scope; out.kinds = s.kinds; out.revision = s.latest;
        out.workGeneration = s.workGeneration; out.dependencyRevision = s.dependencyRevision; return true;
    }
    return false;
}
void UiRefreshBindings::ObserveScopedSubscription(const SubscriptionWork& work, const std::vector<MmsUiTargetRefV1>& members,
    uint64_t dependencyRevision, uint32_t blocked) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.subscriptions.find(work.subscription);
    if (found == state.subscriptions.end() || found->second.owner->client != work.client || !Impl::Scoped(found->second)) return;
    auto& s = found->second;
    if (members.size() > s.members.size()) { s.armed = false; s.waitEvents = 0; s.waitingFor = MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH; return; }
    bool changed = members.size() != s.memberCount;
    for (size_t i = 0; !changed && i < members.size(); ++i) changed = !SameTarget(members[i], s.members[i]);
    const auto oldBlock = s.scopeBlock;
    s.scopeBlock = blocked; s.dependencyRevision = dependencyRevision;
    s.memberCount = members.size(); std::copy(members.begin(), members.end(), s.members.begin()); state.WatchSources(s);
    if (s.observed && changed) state.ChangeScoped(s, true);
    else if (s.observed && oldBlock && !blocked) state.ChangeScoped(s, false);
    else if (!s.observed && (s.waitEvents & RetryHostWake)) {
        // The first UI observation is bookkeeping, not a new publication.
        // In particular it must not resurrect an initial failed host wake.
        s.waitingFor |= blocked | (state.SourcesReady(s) ? 0u : MMS_UI_REFRESH_SUBSCRIPTION_WAIT_SOURCE);
        s.armed = false;
    } else if (!s.observed || blocked) state.ArmScoped(s);
    s.observed = true;
}
void UiRefreshBindings::ScopedGraphChanged(const RefreshClient& client, const MmsUiTargetRefV1& upstream) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& entry : state.subscriptions) {
        auto& s = entry.second;
        if (s.owner->client == &client && s.scope == MMS_UI_REFRESH_DEPENDENTS && Impl::Watches(s, upstream)) state.ChangeScoped(s, true);
    }
}
void UiRefreshBindings::ScopedTargetPublished(MmsHandle element, bool providerChanged) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& entry : state.subscriptions) {
        auto& s = entry.second;
        if (!Impl::Scoped(s) || s.ticket || s.claimed ||
            !(s.waitingFor & (MMS_UI_REFRESH_SUBSCRIPTION_WAIT_TARGET | (providerChanged ? MMS_UI_REFRESH_SUBSCRIPTION_WAIT_CONSUMER : 0)))) continue;
        bool member = s.seed.element == element;
        for (size_t i = 0; !member && i < s.memberCount; ++i) member = s.members[i].element == element;
        if (member) state.ChangeScoped(s, false);
    }
}
bool UiRefreshBindings::PendingScopedSubscriptions() const noexcept {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (const auto& entry : state.subscriptions) {
        const auto& s = entry.second;
        if (Impl::Scoped(s) && s.armed && !s.ticket && !s.claimed && !s.owner->retired && !s.owner->client->stopping.load()) return true;
    }
    return false;
}
bool UiRefreshBindings::PendingSubscriptions() const noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (const auto& entry : state.subscriptions) {
        const auto& s = entry.second;
        if (s.armed && !s.ticket && !s.claimed && !s.owner->retired && !s.owner->client->stopping.load()) return true;
    }
    return false;
}
bool UiRefreshBindings::RetrySubscriptions(uint32_t events, uint64_t queueCapacityRevision) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex); bool changed = false;
    for (auto& entry : state.subscriptions) {
        auto& s = entry.second;
        const auto matching = s.waitEvents & events;
        if (Impl::Scoped(s) && (matching & (RetryTargetPublication | RetryConsumerPublication))) continue;
        const bool actualCapacity = (matching & ~(RetryDisplayCapacity | RetryQueueCapacity)) ||
            ((matching & RetryQueueCapacity) && queueCapacityRevision > s.waitQueueCapacityRevision) ||
            ((matching & RetryDisplayCapacity) && state.global->releasedRevision.load() > s.waitCapacityRevision);
        if (actualCapacity && !s.ticket && !s.claimed && !s.owner->retired && !s.owner->client->stopping.load()) {
            if (Impl::Scoped(s)) state.ArmScoped(s); else { s.armed = true; s.waitEvents = 0; }
            changed |= s.armed;
        }
    }
    return changed;
}
void UiRefreshBindings::FailSubscriptionScheduling(MmsResult result) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& entry : state.subscriptions) {
        auto& s = entry.second;
        if (s.armed && !s.ticket && !s.claimed) {
            s.armed = false; s.waitEvents = RetryHostWake; s.lastResult = result;
            if (Impl::Scoped(s)) s.waitingFor = MMS_UI_REFRESH_SUBSCRIPTION_WAIT_HOST_WAKE;
        }
    }
}
MmsResult UiRefreshBindings::Capture(RefreshClient& client, MmsHandle handle, const MmsUiTargetRefV1& target,
                                    uint64_t revision, Contribution& out) {
    if (!TargetValid(target) || !handle || !revision) return MMS_E_ARGUMENT;
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.bindings.find(handle); if (found == state.bindings.end()) return MMS_E_STALE;
    const auto& b = found->second;
    if (b->owner->client != &client) return MMS_E_ARGUMENT;
    if (b->owner->retired || client.stopping.load()) return MMS_E_NOT_READY;
    if (!SameTarget(b->target, target) || target.projectEpoch != state.epoch) return MMS_E_STALE;
    auto source = state.sources.find(b->source); if (source == state.sources.end()) return MMS_E_STALE;
    if (source->second.revision != revision) return MMS_E_STALE;
    if (!source->second.snapshot) return MMS_E_NOT_READY;
    if (b->owner->accounting->contributions.load() >= kContributions || state.global->contributions.load() >= 4096) return MMS_E_BUSY;
    auto item = std::make_shared<DataContribution>(); item->binding = b; item->snapshot = source->second.snapshot;
    item->global = state.global; item->revision = revision;
    b->owner->accounting->contributions.fetch_add(1); state.global->contributions.fetch_add(1); item->charged = true;
    out = std::move(item); return MMS_OK;
}
MmsResult UiRefreshBindings::CaptureSubtree(RefreshClient& client, const std::vector<MmsUiTargetRefV1>& targets,
                                           std::vector<Captured>& out) {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    if (client.stopping.load()) return MMS_E_NOT_READY;
    std::vector<Captured> candidate; candidate.reserve(targets.size());
    for (const auto& target : targets) {
        if (!TargetValid(target) || target.projectEpoch != state.epoch) return MMS_E_STALE;
        for (const auto& entry : state.bindings) {
            const auto& b = entry.second;
            if (b->owner->client != &client || !SameTarget(b->target, target)) continue;
            if (!b->active || b->owner->retired) return MMS_E_STALE;
            const auto source = state.sources.find(b->source);
            if (source == state.sources.end()) return MMS_E_STALE;
            if (!source->second.snapshot || !source->second.revision) return MMS_E_NOT_READY;
            auto item = std::make_shared<DataContribution>(); item->binding = b;
            item->snapshot = source->second.snapshot; item->revision = source->second.revision; item->global = state.global;
            candidate.push_back({target, b->handle, item->revision, std::move(item)});
        }
    }
    if (candidate.empty()) return MMS_E_UNSUPPORTED;
    const auto accounting = candidate.front().data->binding->owner->accounting;
    if (candidate.size() > kContributions - accounting->contributions.load() ||
        candidate.size() > 4096 - state.global->contributions.load()) return MMS_E_BUSY;
    // No allocation or failure remains once accounting is charged.
    accounting->contributions.fetch_add(unsigned(candidate.size()));
    state.global->contributions.fetch_add(unsigned(candidate.size()));
    for (auto& item : candidate) item.data->charged = true;
    out.swap(candidate); return MMS_OK;
}
MmsResult UiRefreshBindings::ValidateSubtree(const RefreshClient& client, const std::vector<MmsUiTargetRefV1>& targets,
                                            const std::vector<Captured>& captured) const {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    if (client.stopping.load()) return MMS_E_CANCELLED;
    size_t count = 0;
    for (const auto& target : targets) {
        if (target.projectEpoch != state.epoch) return MMS_E_STALE;
        for (const auto& entry : state.bindings) {
            const auto& b = entry.second;
            if (b->owner->client != &client || !SameTarget(b->target, target)) continue;
            const auto old = std::find_if(captured.begin(), captured.end(), [&](const auto& item) {
                return item.binding == b->handle && SameTarget(item.target, target);
            });
            if (old == captured.end() || !b->active || b->owner->retired ||
                old->data->binding != b || !state.sources.count(b->source)) return MMS_E_STALE;
            ++count;
        }
    }
    return count == captured.size() ? MMS_OK : MMS_E_STALE;
}
MmsResult UiRefreshBindings::CaptureScoped(RefreshClient& client, MmsHandle subscription, MmsHandle trigger,
    uint64_t revision, const std::vector<MmsUiTargetRefV1>& targets, Contribution& triggerPin, std::vector<Captured>& out) {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    if (client.stopping.load()) return MMS_E_NOT_READY;
    const auto sub = state.subscriptions.find(subscription);
    if (sub == state.subscriptions.end() || sub->second.owner->client != &client || sub->second.binding != trigger) return MMS_E_STALE;
    const auto binding = state.bindings.find(trigger);
    if (binding == state.bindings.end() || !binding->second->active || binding->second->owner->client != &client) return MMS_E_STALE;
    const auto source = state.sources.find(binding->second->source);
    if (source == state.sources.end() || source->second.epoch != state.epoch || source->second.owner != binding->second->owner) return MMS_E_STALE;
    if (source->second.revision != revision) return MMS_E_STALE;
    if (!source->second.snapshot || !revision) return MMS_E_NOT_READY;
    auto pin = std::make_shared<DataContribution>(); pin->binding = binding->second;
    pin->snapshot = source->second.snapshot; pin->revision = revision; pin->global = state.global;
    std::vector<Captured> candidate; candidate.reserve(targets.size());
    for (const auto& target : targets) {
        if (!TargetValid(target) || target.projectEpoch != state.epoch) return MMS_E_STALE;
        for (const auto& entry : state.bindings) {
            const auto& b = entry.second;
            if (b->owner->client != &client || !SameTarget(b->target, target)) continue;
            if (!b->active || b->owner->retired) return MMS_E_STALE;
            const auto current = state.sources.find(b->source);
            if (current == state.sources.end() || current->second.epoch != state.epoch) return MMS_E_STALE;
            if (!current->second.snapshot || !current->second.revision) return MMS_E_NOT_READY;
            auto item = std::make_shared<DataContribution>(); item->binding = b; item->global = state.global;
            // All sources are captured under this one lock. In particular,
            // members sharing the trigger source must use its exact version.
            item->snapshot = b->source == binding->second->source ? pin->snapshot : current->second.snapshot;
            item->revision = b->source == binding->second->source ? revision : current->second.revision;
            candidate.push_back({target, b->handle, item->revision, std::move(item)});
        }
    }
    const auto accounting = binding->second->owner->accounting;
    if (candidate.size() > kContributions - accounting->contributions.load() ||
        candidate.size() > 4096 - state.global->contributions.load()) return MMS_E_BUSY;
    accounting->contributions.fetch_add(unsigned(candidate.size()));
    state.global->contributions.fetch_add(unsigned(candidate.size()));
    for (auto& item : candidate) item.data->charged = true;
    // The trigger pin is not another display consumer/contribution. Its
    // existing retained source bytes remain charged until the whole ticket
    // and its in-progress group have released it.
    sub->second.claimedSourcePublication = sub->second.sourcePublication;
    triggerPin = std::move(pin); out.swap(candidate); return MMS_OK;
}
MmsResult UiRefreshBindings::ValidateTrigger(const RefreshClient& client, const Contribution& pin) const {
    if (!pin) return MMS_E_STALE;
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    const auto& binding = pin->binding;
    return !client.stopping.load() && binding->active && !binding->owner->retired && binding->owner->client == &client &&
        binding->target.projectEpoch == state.epoch && state.sources.count(binding->source) ? MMS_OK : MMS_E_STALE;
}
MmsResult UiRefreshBindings::Consume(const Contribution& contribution, const std::function<MmsResult()>& revalidate,
                                    uint64_t& consumedRevision, bool& displayChanged, uint32_t* subscriptionWait,
                                    uint64_t* subscriptionCapacityRevision) {
    auto& state = *impl_;
    if (GetCurrentThreadId() != state.thread) return MMS_E_THREAD;
    if (!contribution || !revalidate) return MMS_E_ARGUMENT;
    if (!state.hooks.readOnly) return MMS_E_NOT_READY;
    auto binding = contribution->binding;
    Impl::Flight flight(*binding->owner);
    bool changed = false; uint32_t waitEvents = 0; uint64_t waitCapacityRevision = state.global->releasedRevision.load();
    const auto result = Boundary([&] { return state.hooks.readOnly([&]() -> MmsResult {
        if (const auto r = revalidate()) { if (r == MMS_E_NOT_READY) waitEvents = RetryTargetPublication; return r; }
        bool already = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (binding->owner->retired || binding->owner->client->stopping.load()) return MMS_E_CANCELLED;
            if (!binding->active || binding->target.projectEpoch != state.epoch) return MMS_E_STALE;
            if (binding->consumed > contribution->revision) return MMS_E_STALE;
            already = binding->consumed == contribution->revision;
        }
        if (already) return MMS_OK;
        Impl::Builder builder; builder.owner = binding->owner; builder.maximum = binding->maximum;
        MmsHandle token = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (builder.owner->builders) { waitEvents = RetryDisplayCapacity; return MMS_E_BUSY; }
            waitEvents = RetryDisplayCapacity;
            waitCapacityRevision = state.global->releasedRevision.load();
            token = state.Next(); state.builders.emplace(token, &builder); ++builder.owner->builders;
        }
        struct ClearBuilder {
            Impl& state; MmsHandle token; Impl::Builder& builder;
            ~ClearBuilder() { std::lock_guard<std::mutex> lock(state.mutex); state.builders.erase(token); --builder.owner->builders; }
        } clear{state, token, builder};
        const MmsUiDataPrepareV1 input{Header<MmsUiDataPrepareV1>(), binding->target, binding->source,
            contribution->revision, contribution->snapshot->View(), token, binding->consumed};
        waitEvents = 0;
        MmsResult prepared = MMS_OK;
        if (binding->prepare) prepared = Boundary([&] { return binding->prepare(binding->context, &input); });
        else prepared = state.WritePrepared(*binding->owner, token, input.sourceBytes);
        if (prepared) {
            waitEvents = builder.failure == prepared && (prepared == MMS_E_BUSY || prepared == MMS_E_MEMORY) ? RetryDisplayCapacity : RetryConsumerPublication;
            if (waitEvents == RetryDisplayCapacity) waitCapacityRevision = builder.failureCapacityRevision;
            return prepared;
        }
        if (builder.failure) { waitEvents = RetryDisplayCapacity; waitCapacityRevision = builder.failureCapacityRevision; return builder.failure; }
        if (!builder.written || !builder.candidate) return MMS_E_FAILED;
        if (const auto r = revalidate()) { if (r == MMS_E_NOT_READY) waitEvents = RetryTargetPublication; return r; }
        std::lock_guard<std::mutex> lock(state.mutex);
        if (binding->owner->retired || binding->owner->client->stopping.load()) return MMS_E_CANCELLED;
        if (!binding->active || binding->target.projectEpoch != state.epoch) return MMS_E_STALE;
        if (binding->consumed > contribution->revision) return MMS_E_STALE;
        changed = !Equal(binding->display, builder.candidate->View());
        if (changed && binding->displayRevision == UINT64_MAX) return MMS_E_BUSY;
        if (changed) { builder.candidate->retained = true; binding->display.swap(builder.candidate); ++binding->displayRevision; }
        binding->consumed = contribution->revision;
        return MMS_OK;
    }); });
    if (!result) { consumedRevision = contribution->revision; displayChanged = changed; }
    else if (subscriptionWait) *subscriptionWait = waitEvents;
    if (result && subscriptionCapacityRevision) *subscriptionCapacityRevision = waitCapacityRevision;
    return result;
}
bool UiRefreshBindings::HasBinding(const RefreshClient& client, const MmsUiTargetRefV1& target) const {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    if (target.projectEpoch != state.epoch || client.stopping.load()) return false;
    for (const auto& item : state.bindings) if (item.second->owner->client == &client && SameTarget(item.second->target, target)) return true;
    return false;
}
void UiRefreshBindings::InvalidateTarget(MmsHandle element) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto it = state.subscriptions.begin(); it != state.subscriptions.end();) {
        if (it->second.seed.element == element) { --it->second.owner->subscriptions; it = state.subscriptions.erase(it); } else ++it;
    }
    for (auto it = state.bindings.begin(); it != state.bindings.end();) {
        if (it->second->target.element == element) { auto erase = it++; state.RemoveBinding(erase); } else ++it;
    }
}
void UiRefreshBindings::AdvanceProjectEpoch(uint64_t epoch) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    state.epoch = epoch;
    for (auto& item : state.bindings) item.second->active = false;
    state.bindings.clear(); state.sources.clear(); state.subscriptions.clear();
    for (auto& item : state.owners) { item.second->sources = 0; item.second->bindings = 0; item.second->subscriptions = 0; }
}
void UiRefreshBindings::Retire(RefreshClient& client) noexcept {
    client.stopping.store(true);
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.owners.find(&client); if (found == state.owners.end()) return;
    auto& owner = *found->second; owner.retired = true;
    for (auto it = state.bindings.begin(); it != state.bindings.end();) {
        if (it->second->owner.get() == &owner) { auto erase = it++; state.RemoveBinding(erase); } else ++it;
    }
    for (auto it = state.sources.begin(); it != state.sources.end();) {
        if (it->second.owner.get() == &owner) { --owner.sources; it = state.sources.erase(it); } else ++it;
    }
}
bool UiRefreshBindings::HasLeases(const RefreshClient& client) const {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.owners.find(const_cast<RefreshClient*>(&client));
    if (found == state.owners.end()) return false;
    const auto& o = *found->second;
    return client.inFlight.load() || o.sources || o.bindings || o.leases || o.builders || o.subscriptions || o.accounting->contributions.load();
}
}
