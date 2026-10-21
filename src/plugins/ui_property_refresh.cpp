#include "ui_property_refresh.hpp"
#include "ui_registry.hpp"
#include "ui_refresh_binding.hpp"
#include "ui_refresh_dependencies.hpp"
#include <array>

namespace mikumikustudio::plugins {
namespace {
constexpr uint32_t kOwnerLimit = 128, kGlobalLimit = 2048;
constexpr uint32_t kKinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT;
bool Same(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) noexcept {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
bool Same(const MmsUiPropertyRefV1& a, const MmsUiPropertyRefV1& b) noexcept {
    return a.property == b.property && a.registrationGeneration == b.registrationGeneration && a.projectEpoch == b.projectEpoch;
}
bool ValidRef(const MmsUiPropertyRefV1* ref) noexcept {
    return Valid(ref) && ref->property && ref->registrationGeneration && ref->projectEpoch;
}
uint32_t WaitFor(MmsResult result) noexcept {
    if (result == MMS_E_BUSY) return MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY;
    if (result == MMS_E_NOT_READY) return MMS_UI_PROPERTY_REFRESH_WAIT_TARGET;
    if (result == MMS_E_CONFLICT) return MMS_UI_PROPERTY_REFRESH_WAIT_BINDING;
    return MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER;
}
}
struct UiPropertyRefresh::Impl {
    struct Owner {
        Impl* store = nullptr;
        RefreshClient* client = nullptr;
        std::atomic<UiClient*> ui{nullptr};
        bool retired = false;
    };
    struct Property {
        Owner* owner = nullptr;
        MmsUiPropertyRefV1 ref{};
        std::weak_ptr<const PropertyRecord> record;
        uint64_t sequence = 1;
    };
    struct Subscription {
        Owner* owner = nullptr;
        MmsHandle handle = 0, ticket = 0;
        MmsUiTargetRefV1 target{};
        MmsUiPropertyRefV1 property{};
        uint32_t kinds = 0, flags = 0, waiting = 0;
        uint64_t latest = 1, submitted = 0, completed = 0, capacity = 0, hostWake = 0;
        MmsResult result = MMS_OK;
        bool dirty = true, armed = true, claimed = false, bound = false;
        uint32_t scope = MMS_UI_REFRESH_SELF, scopeBlock = 0;
        uint64_t workGeneration = 1, invalidationGeneration = 1, submittedWorkGeneration = 0, completedWorkGeneration = 0;
        uint64_t dependencyRevision = 0, submittedDependencyRevision = 0;
        uint64_t sourcePublication = 0, claimedSourcePublication = 0, submittedSourcePublication = 0, displayCapacity = 0;
        bool observed = false, sourcesObserved = false, sourceMissing = false, resourceCapacity = false;
        // One real structure event is pending observation and one original
        // Request attempt; claiming that attempt spends the probe.
        bool structurePending = false, structureProbe = false;
        // Set only by a real public binding identity event. Until a complete
        // source observation lands, the cached sourceMissing describes the old
        // binding set and must not suppress the one re-capture the event needs.
        bool bindingObservationPending = false;
        std::array<MmsUiTargetRefV1, 256> members{};
        size_t memberCount = 0, sourceCount = 0;
        std::array<UiRefreshBindings::PropertySource, 128> sources{};
    };
    UiPropertyRefresh& self;
    UiRefreshQueue& queue;
    UiRegistry& registry;
    DWORD thread = GetCurrentThreadId();
    mutable std::mutex mutex;
    std::map<RefreshClient*, std::unique_ptr<Owner>> owners;
    std::vector<std::unique_ptr<Owner>> retiredContexts;
    std::map<MmsHandle, Property> properties;
    std::map<MmsHandle, Subscription> subscriptions;
    MmsHandle next = 1;
    uint64_t epoch = 1;
    std::atomic<bool> historyAvailable{false}, historyPending{false};
    std::atomic<bool> historyWakeFailed{false};
    std::atomic<uint64_t> historyRevision{0};
    std::atomic<uint64_t> hostWakeRevision{0};
    uint64_t consumedHistory = 0;
    explicit Impl(UiPropertyRefresh& s, UiRefreshQueue& q, UiRegistry& r) : self(s), queue(q), registry(r) {}
    Owner& For(RefreshClient& c) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = owners.find(&c);
        if (found != owners.end() && found->second->retired && !c.stopping.load()) {
            auto owner = std::make_unique<Owner>(); owner->store = this; owner->client = &c;
            retiredContexts.push_back(std::move(found->second));
            found->second = std::move(owner);
        }
        if (found == owners.end()) {
            auto owner = std::make_unique<Owner>(); owner->store = this; owner->client = &c;
            found = owners.emplace(&c, std::move(owner)).first;
        }
        return *found->second;
    }
    bool Current(const Owner& owner) const noexcept { return owner.ui && !owner.retired && !owner.client->stopping.load(); }
    static bool Scoped(const Subscription& sub) noexcept { return sub.scope != MMS_UI_REFRESH_SELF; }
    void ArmScoped(Subscription& sub) noexcept {
        if (sub.ticket || sub.claimed) return;
        if (sub.structurePending && !sub.structureProbe) {
            // A real structure event gets exactly one original Request while
            // the cached graph/source park still describes the old collection.
            sub.waiting = 0; sub.armed = true; return;
        }
        // A pending real binding event makes the cached sourceMissing stale.
        // Re-capture once, while a real scope block still parks the mailbox.
        const bool missing = sub.sourceMissing && !sub.bindingObservationPending;
        sub.waiting = sub.scopeBlock | (missing ? MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE : 0);
        sub.armed = !sub.waiting;
    }
    // Re-decide a parked scope from the just-observed sources and the real
    // scope block. A failed host wake stays parked; an accepted ticket or
    // claim keeps its existing decision. Only the binding-event handshake uses
    // this, never an ordinary observation or a temporary observation failure.
    void ReparkScoped(Subscription& sub, bool hostWake) noexcept {
        if (sub.ticket || sub.claimed) return;
        const uint32_t park = sub.scopeBlock | (sub.sourceMissing ? MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE : 0);
        sub.waiting = park | (hostWake ? MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE : 0);
        sub.armed = !sub.waiting;
    }
    void Change(Subscription& sub, bool invalidate) noexcept {
        if (sub.workGeneration == UINT64_MAX) {
            sub.armed = false; sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER; sub.result = MMS_E_BUSY; return;
        }
        ++sub.workGeneration; sub.dirty = true;
        if (invalidate) sub.invalidationGeneration = sub.workGeneration;
        // A new real work event is its own request opportunity even while the
        // cached structure metadata is still pending. Clear only the old
        // structure event's spent probe; the metadata stays pending until its
        // own observation arrives.
        if (sub.structurePending) sub.structureProbe = false;
        ArmScoped(sub);
    }
    void Wake(bool pending) noexcept { if (pending) queue.WakeSubscriptions(); }
    void ObserveSourcesLocked(Subscription& sub, const std::vector<UiRefreshBindings::PropertySource>& sources, bool captured,
        bool allowChange = true) noexcept {
        if (sources.size() > sub.sources.size()) { sub.armed = false; sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY; return; }
        bool bindingsChanged = sub.sourceCount != sources.size(), publication = false, missing = false;
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i]; missing |= source.revision == 0;
            if (i >= sub.sourceCount || sub.sources[i].binding != source.binding || sub.sources[i].source != source.source || !Same(sub.sources[i].target, source.target)) bindingsChanged = true;
            const auto old = std::find_if(sub.sources.begin(), sub.sources.begin() + sub.sourceCount, [&](const auto& item) { return item.binding == source.binding && item.source == source.source; });
            publication |= old != sub.sources.begin() + sub.sourceCount && source.revision > old->revision;
        }
        sub.sourceCount = sources.size(); std::copy(sources.begin(), sources.end(), sub.sources.begin()); sub.sourceMissing = missing;
        // A complete observation is the handshake the binding event awaited.
        // The cached missing state now describes the current binding set, so
        // parks are decided again from these sources and the real scope block.
        const bool bindingEvent = sub.bindingObservationPending;
        const bool hostWake = (sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) != 0;
        sub.bindingObservationPending = false;
        if (publication && sub.sourcePublication != UINT64_MAX) ++sub.sourcePublication;
        if (bindingEvent && hostWake) {
            // The real binding event already recorded this work and the host
            // wake really failed. A successful observation that only installs
            // the missing watch is not another event: keep that park and only
            // refresh the captured source/block state. Only a later real
            // publication or notify whose host wake is delivered may recover it.
            ReparkScoped(sub, true);
        } else {
            // A structure-event observation owns its own generation step and
            // must not count the same real change twice.
            if (allowChange && sub.sourcesObserved && bindingsChanged) Change(sub, true);
            else if (allowChange && publication && !sub.ticket && !sub.claimed && (sub.waiting & (MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE | MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER))) Change(sub, false);
            else if (!sub.sourcesObserved) {
                if (sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) { sub.waiting |= missing ? MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE : 0; sub.armed = false; }
                else ArmScoped(sub);
            }
            // Without a failed wake the handshake still re-decides the park from
            // these sources and blocks; a real change above already records it.
            if (bindingEvent) ReparkScoped(sub, hostWake);
        }
        sub.sourcesObserved = true;
        if (captured) sub.claimedSourcePublication = sub.sourcePublication;
    }
    // One trusted commit for a complete observation. A temporary collection or
    // allocation failure must never write half of this state: a synthetic graph
    // block would later look like a real graph recovery and retry a parked
    // consumer. Members and source watches are therefore written together.
    void ObserveLocked(Subscription& sub, const std::vector<MmsUiTargetRefV1>& members,
        uint64_t dependencyRevision, uint32_t blocked, const std::vector<UiRefreshBindings::PropertySource>& sources) noexcept {
        if (members.size() > sub.members.size() || sources.size() > sub.sources.size()) {
            sub.structurePending = false; sub.structureProbe = false; return;
        }
        bool changed = sub.memberCount != members.size();
        for (size_t i = 0; !changed && i < members.size(); ++i) changed = !Same(sub.members[i], members[i]);
        const auto oldBlock = sub.scopeBlock; sub.scopeBlock = blocked; sub.dependencyRevision = dependencyRevision;
        sub.memberCount = members.size(); std::copy(members.begin(), members.end(), sub.members.begin());
        const bool event = sub.structurePending;
        const bool attempted = sub.structureProbe;
        if (event) { sub.structurePending = false; sub.structureProbe = false; }
        else if (sub.observed && changed) Change(sub, true);
        else if (sub.observed && oldBlock && !blocked) Change(sub, false);
        else if (!sub.observed || blocked) {
            if (sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) { sub.waiting |= blocked; sub.armed = false; }
            else ArmScoped(sub);
        }
        sub.observed = true;
        ObserveSourcesLocked(sub, sources, false, !event);
        if (event) {
            // The observation consumed the real structure event: members,
            // scope blocks and source watches are already refreshed, and the
            // generation was advanced when the event was recorded. Only the
            // first probe may bypass the old park; a spent probe keeps its
            // Request outcome, and a failed host wake stays parked.
            if (sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) {
                sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE | sub.scopeBlock |
                    (sub.sourceMissing ? MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE : 0);
                sub.armed = false;
            } else if (attempted) {
                sub.waiting = (sub.waiting & ~(MMS_UI_PROPERTY_REFRESH_WAIT_TARGET |
                    MMS_UI_PROPERTY_REFRESH_WAIT_GRAPH | MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE)) | sub.scopeBlock |
                    (sub.sourceMissing ? MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE : 0);
                sub.armed = false;
            } else ArmScoped(sub);
        }
    }
    MmsResult Capabilities(Owner& owner, MmsUiPropertyRefreshCapabilitiesV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        if (const auto result = self.CheckUi(*owner.client, owner.ui)) return result;
        std::lock_guard<std::mutex> lock(mutex);
        if (!Current(owner)) return MMS_E_STALE;
        *out = {Header<MmsUiPropertyRefreshCapabilitiesV1>(), kOwnerLimit, kKinds,
            historyAvailable.load() ? MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY : 0u, 0, 0};
        return MMS_OK;
    }
    MmsResult ScopedCapabilities(Owner& owner, MmsUiPropertyRefreshScopedCapabilitiesV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        MmsUiPropertyRefreshCapabilitiesV1 caps{}; caps.h = Header<MmsUiPropertyRefreshCapabilitiesV1>();
        if (const auto result = Capabilities(owner, &caps)) return result;
        *out = {Header<MmsUiPropertyRefreshScopedCapabilitiesV1>(), caps.maxSubscriptionsPerPlugin, caps.supportedKinds,
            caps.supportedFlags, caps.flags, caps.reserved, MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS, 0};
        return MMS_OK;
    }
    MmsResult GetRef(Owner& owner, MmsHandle handle, MmsUiPropertyRefV1* out) {
        if (!handle || !Valid(out)) return MMS_E_ARGUMENT;
        if (const auto result = self.CheckUi(*owner.client, owner.ui)) return result;
        std::shared_ptr<const PropertyRecord> record;
        for (const auto& entry : registry.properties) if (entry.second->handle == handle) { record = entry.second; break; }
        if (!record) return MMS_E_STALE;
        if (record->owner != owner.ui) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (!Current(owner)) return MMS_E_STALE;
        const MmsUiPropertyRefV1 ref{Header<MmsUiPropertyRefV1>(), handle, record->registrationGeneration, epoch};
        if (!ref.registrationGeneration) return MMS_E_STALE;
        auto found = properties.find(handle);
        if (found == properties.end()) {
            Property property; property.owner = &owner; property.ref = ref; property.record = record;
            properties.emplace(handle, std::move(property));
        } else if (found->second.owner != &owner || !Same(found->second.ref, ref)) return MMS_E_STALE;
        *out = ref; return MMS_OK;
    }
    MmsResult Subscribe(Owner& owner, const MmsUiPropertyRefreshSubscriptionDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || !Valid(&desc->target) || !desc->target.element || !desc->target.instanceGeneration ||
            !desc->target.projectEpoch || !ValidRef(&desc->property) || desc->reserved ||
            desc->flags & ~MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY || !(desc->kinds & MMS_UI_REFRESH_DATA) || desc->kinds & ~kKinds) return MMS_E_ARGUMENT;
        if (const auto result = self.CheckUi(*owner.client, owner.ui)) return result;
        std::shared_ptr<const PropertyRecord> record;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Current(owner)) return MMS_E_STALE;
            if ((desc->flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) && !historyAvailable.load()) return MMS_E_UNSUPPORTED;
            const auto property = properties.find(desc->property.property);
            if (property == properties.end() || !Same(property->second.ref, desc->property)) return MMS_E_STALE;
            if (property->second.owner != &owner) return MMS_E_ARGUMENT;
            record = property->second.record.lock();
        }
        bool bound = false, ready = false;
        if (const auto result = self.ValidateTarget(*owner.client, owner.ui, desc->target, record, desc->kinds, bound, ready)) return result;
        bool wake = false; MmsHandle handle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Current(owner)) return MMS_E_STALE;
            const auto property = properties.find(desc->property.property);
            if (property == properties.end() || property->second.owner != &owner || !Same(property->second.ref, desc->property)) return MMS_E_STALE;
            size_t count = 0;
            for (const auto& entry : subscriptions) {
                const auto& sub = entry.second;
                if (sub.scope == MMS_UI_REFRESH_SELF && sub.target.element == desc->target.element) return MMS_E_CONFLICT;
                count += sub.owner == &owner;
            }
            if (count >= kOwnerLimit || subscriptions.size() >= kGlobalLimit || next == UINT64_MAX) return MMS_E_BUSY;
            Subscription sub; sub.owner = &owner; sub.handle = next; sub.target = desc->target; sub.property = desc->property;
            sub.kinds = desc->kinds; sub.flags = desc->flags; sub.latest = property->second.sequence; sub.bound = bound;
            sub.waiting = bound ? MMS_UI_PROPERTY_REFRESH_WAIT_BINDING : !ready ? MMS_UI_PROPERTY_REFRESH_WAIT_TARGET : 0;
            sub.armed = !sub.waiting; handle = next;
            subscriptions.emplace(handle, std::move(sub)); ++next;
            wake = !bound && ready;
        }
        // A failed host wake parks the accepted subscription; registration is
        // still concrete and queryable, with no half-created output identity.
        Wake(wake); *out = handle; return MMS_OK;
    }
    MmsResult SubscribeScoped(Owner& owner, const MmsUiPropertyRefreshScopedSubscriptionDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || !ValidRef(&desc->property) || !Valid(&desc->seed) || !desc->seed.element ||
            !desc->seed.instanceGeneration || !desc->seed.projectEpoch || desc->reserved || desc->reservedTail ||
            desc->flags & ~MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY || !(desc->kinds & MMS_UI_REFRESH_DATA) || desc->kinds & ~kKinds ||
            (desc->scope != MMS_UI_REFRESH_SELF && desc->scope != MMS_UI_REFRESH_SUBTREE && desc->scope != MMS_UI_REFRESH_DEPENDENTS)) return MMS_E_ARGUMENT;
        if (desc->scope == MMS_UI_REFRESH_SELF) {
            const MmsUiPropertyRefreshSubscriptionDescV1 legacy{Header<MmsUiPropertyRefreshSubscriptionDescV1>(), desc->seed, desc->property, desc->kinds, desc->flags, 0};
            return Subscribe(owner, &legacy, out);
        }
        if (const auto result = self.CheckUi(*owner.client, owner.ui)) return result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Current(owner)) return MMS_E_STALE;
            const auto property = properties.find(desc->property.property);
            if (property == properties.end() || !Same(property->second.ref, desc->property)) return MMS_E_STALE;
            if (property->second.owner != &owner) return MMS_E_ARGUMENT;
            if ((desc->flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) && !historyAvailable.load()) return MMS_E_UNSUPPORTED;
        }
        if (const auto result = queue.CheckPropertyScopedSeed(*owner.client, desc->seed, desc->scope, desc->kinds)) return result;
        MmsHandle handle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Current(owner)) return MMS_E_STALE;
            const auto property = properties.find(desc->property.property);
            if (property == properties.end() || property->second.owner != &owner || !Same(property->second.ref, desc->property)) return MMS_E_STALE;
            size_t count = 0;
            for (const auto& entry : subscriptions) {
                const auto& sub = entry.second; count += sub.owner == &owner;
                if (sub.owner == &owner && Same(sub.property, desc->property) && Same(sub.target, desc->seed) && sub.scope == desc->scope) return MMS_E_CONFLICT;
            }
            if (count >= kOwnerLimit || subscriptions.size() >= kGlobalLimit || next == UINT64_MAX) return MMS_E_BUSY;
            Subscription sub; sub.owner = &owner; sub.handle = next; sub.target = desc->seed; sub.property = desc->property;
            sub.scope = desc->scope; sub.kinds = desc->kinds; sub.flags = desc->flags; sub.latest = property->second.sequence;
            handle = next; subscriptions.emplace(handle, std::move(sub)); ++next;
        }
        Wake(true); *out = handle; return MMS_OK;
    }
    MmsResult Notify(Owner& owner, const MmsUiPropertyRefV1* ref) {
        if (!ValidRef(ref)) return MMS_E_ARGUMENT;
        if (self.ReadOnlyNotification(*owner.client, owner.ui)) return MMS_E_BUSY;
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!Current(owner)) return MMS_E_STALE;
            const auto property = properties.find(ref->property);
            if (property == properties.end() || !Same(property->second.ref, *ref)) return MMS_E_STALE;
            if (property->second.owner != &owner) return MMS_E_ARGUMENT;
            if (property->second.sequence == UINT64_MAX) return MMS_E_BUSY;
            const auto sequence = ++property->second.sequence;
            for (auto& entry : subscriptions) {
                auto& sub = entry.second;
                if (sub.owner != &owner || !Same(sub.property, *ref)) continue;
                sub.latest = sequence; sub.dirty = true;
                if (Scoped(sub)) { const bool armed = sub.armed; Change(sub, false); wake |= !armed && sub.armed && !sub.ticket && !sub.claimed; continue; }
                if (sub.workGeneration != UINT64_MAX) ++sub.workGeneration;
                if (sub.bound) { sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_BINDING; continue; }
                sub.waiting = 0;
                if (!sub.ticket && !sub.claimed) { wake |= !sub.armed; sub.armed = true; }
            }
        }
        Wake(wake); return MMS_OK;
    }
    void State(const Subscription& sub, MmsUiPropertyRefreshSubscriptionStateV1& out) const noexcept {
        const bool deferredHistory = (sub.flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) && historyAvailable.load(std::memory_order_acquire) &&
            historyPending.load(std::memory_order_acquire) && historyRevision.load(std::memory_order_acquire) != consumedHistory;
        const uint32_t historyWait = deferredHistory && historyWakeFailed.load(std::memory_order_acquire) ? MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE : 0;
        out = {Header<MmsUiPropertyRefreshSubscriptionStateV1>(), sub.handle, sub.target, sub.property,
            sub.latest, sub.submitted, sub.completed, historyWait ? MMS_E_FAILED : sub.result,
            uint32_t(sub.dirty || sub.claimed || sub.ticket || deferredHistory), sub.waiting | historyWait, 0};
    }
    MmsResult Query(Owner& owner, MmsHandle handle, MmsUiPropertyRefreshSubscriptionStateV1* out) {
        if (!handle || !Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = subscriptions.find(handle);
        if (!Current(owner) || found == subscriptions.end() || found->second.owner != &owner) return MMS_E_STALE;
        State(found->second, *out); return MMS_OK;
    }
    MmsResult QueryScoped(Owner& owner, MmsHandle handle, MmsUiPropertyRefreshScopedSubscriptionStateV1* out) {
        if (!handle || !Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = subscriptions.find(handle);
        if (!Current(owner) || found == subscriptions.end() || found->second.owner != &owner) return MMS_E_STALE;
        const auto& sub = found->second; MmsUiPropertyRefreshSubscriptionStateV1 state{}; State(sub, state);
        *out = {Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>(), state.subscription, state.target, state.property,
            state.latestSequence, state.submittedSequence, state.completedSequence, state.lastResult, state.pending, state.waitingFor, state.reserved,
            sub.scope, sub.kinds, sub.flags, 0, sub.workGeneration, sub.submittedWorkGeneration, sub.completedWorkGeneration,
            sub.submittedDependencyRevision, 0}; return MMS_OK;
    }
    MmsResult Unsubscribe(Owner& owner, MmsHandle handle) {
        if (!handle) return MMS_E_ARGUMENT;
        if (const auto result = self.CheckUi(*owner.client, owner.ui)) return result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = subscriptions.find(handle);
            if (found == subscriptions.end() || found->second.owner != &owner) return MMS_E_STALE;
            subscriptions.erase(found);
        }
        queue.CleanupSubscriptions(); return MMS_OK;
    }
};
UiPropertyRefresh::UiPropertyRefresh(UiRefreshQueue& queue, UiRegistry& registry) : impl_(std::make_unique<Impl>(*this, queue, registry)) {}
UiPropertyRefresh::~UiPropertyRefresh() = default;
MmsResult UiPropertyRefresh::BindClient(RefreshClient& client, UiClient& ui) {
    if (GetCurrentThreadId() != impl_->thread) return MMS_E_THREAD;
    if (client.queue != &impl_->queue || ui.registry != &impl_->registry || client.owner.empty() || client.owner == "mms" || client.owner != ui.id) return MMS_E_ARGUMENT;
    if (impl_->registry.InCallback() || impl_->queue.ReadOnlyCause()) return MMS_E_BUSY;
    auto& owner = impl_->For(client);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (owner.ui && owner.ui != &ui) return MMS_E_CONFLICT;
    for (const auto& entry : impl_->owners)
        if (entry.second.get() != &owner && !entry.second->retired && entry.second->ui == &ui) return MMS_E_CONFLICT;
    if (owner.retired || client.stopping.load() || ui.stopping) return MMS_E_STALE;
    owner.ui = &ui; return MMS_OK;
}
MmsResult UiPropertyRefresh::CheckUi(RefreshClient& client, const UiClient* ui) const {
    if (GetCurrentThreadId() != impl_->thread) return MMS_E_THREAD;
    if (client.queue != &impl_->queue || !ui) return MMS_E_NOT_READY;
    if (impl_->queue.ReadOnlyCause() || (impl_->registry.InCallback() && impl_->registry.InvokingClient() != ui)) return MMS_E_BUSY;
    return MMS_OK;
}
bool UiPropertyRefresh::ReadOnlyNotification(RefreshClient& client, const UiClient* ui) const {
    // Test the refresh cause before acquiring the property mutex. A provider
    // can wait for its worker; that worker must reject feedback immediately,
    // without reading UI-thread maps or UiClient's non-atomic state.
    if (impl_->queue.PropertyReadOnlyCause(client, ui)) return true;
    return GetCurrentThreadId() == impl_->thread && impl_->registry.InCallback() && !impl_->registry.InvokingClient();
}
MmsResult UiPropertyRefresh::ValidateTarget(RefreshClient& client, const UiClient* ui, const MmsUiTargetRefV1& ref,
    const std::shared_ptr<const PropertyRecord>& property, uint32_t kinds, bool& bound, bool& ready) const {
    return impl_->queue.CheckPropertyRefreshTarget(client, ui, ref, property, kinds, bound, ready);
}
MmsUiPropertyRefreshApiV1 UiPropertyRefresh::Api(RefreshClient& client) {
    auto& owner = impl_->For(client);
    MmsUiPropertyRefreshApiV1 api{}; api.h = Header<MmsUiPropertyRefreshApiV1>(); api.context = &owner;
#define PROPERTY_CALL(body) auto& o = *static_cast<Impl::Owner*>(p); o.client->inFlight.fetch_add(1); \
    struct Guard { RefreshClient& client; ~Guard() { client.inFlight.fetch_sub(1); } } guard{*o.client}; \
    return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiPropertyRefreshCapabilitiesV1* out) { PROPERTY_CALL(return o.store->Capabilities(o, out)); };
    api.getPropertyRef = [](void* p, MmsHandle h, MmsUiPropertyRefV1* out) { PROPERTY_CALL(return o.store->GetRef(o, h, out)); };
    api.subscribe = [](void* p, const MmsUiPropertyRefreshSubscriptionDescV1* d, MmsHandle* out) { PROPERTY_CALL(return o.store->Subscribe(o, d, out)); };
    api.notify = [](void* p, const MmsUiPropertyRefV1* ref) { PROPERTY_CALL(return o.store->Notify(o, ref)); };
    api.query = [](void* p, MmsHandle h, MmsUiPropertyRefreshSubscriptionStateV1* out) { PROPERTY_CALL(return o.store->Query(o, h, out)); };
    api.unsubscribe = [](void* p, MmsHandle h) { PROPERTY_CALL(return o.store->Unsubscribe(o, h)); };
#undef PROPERTY_CALL
    return api;
}
MmsUiPropertyRefreshApiV1_1 UiPropertyRefresh::ScopedApi(RefreshClient& client) {
    const auto legacy = Api(client); MmsUiPropertyRefreshApiV1_1 api{}; api.h = {sizeof(api), 1, 1}; api.context = legacy.context;
    api.getCapabilities = legacy.getCapabilities; api.getPropertyRef = legacy.getPropertyRef; api.subscribe = legacy.subscribe;
    api.notify = legacy.notify; api.query = legacy.query; api.unsubscribe = legacy.unsubscribe;
#define SCOPED_PROPERTY_CALL(body) auto& o = *static_cast<Impl::Owner*>(p); o.client->inFlight.fetch_add(1); \
    struct Guard { RefreshClient& client; ~Guard() { client.inFlight.fetch_sub(1); } } guard{*o.client}; \
    return Boundary([&]() -> MmsResult { body; })
    api.getScopedCapabilities = [](void* p, MmsUiPropertyRefreshScopedCapabilitiesV1* out) { SCOPED_PROPERTY_CALL(return o.store->ScopedCapabilities(o, out)); };
    api.subscribeScoped = [](void* p, const MmsUiPropertyRefreshScopedSubscriptionDescV1* desc, MmsHandle* out) { SCOPED_PROPERTY_CALL(return o.store->SubscribeScoped(o, desc, out)); };
    api.queryScoped = [](void* p, MmsHandle h, MmsUiPropertyRefreshScopedSubscriptionStateV1* out) { SCOPED_PROPERTY_CALL(return o.store->QueryScoped(o, h, out)); };
#undef SCOPED_PROPERTY_CALL
    return api;
}
void UiPropertyRefresh::SetHistoryAvailable(bool available) {
    if (GetCurrentThreadId() != impl_->thread) throw std::invalid_argument("property history thread");
    impl_->historyAvailable.store(available, std::memory_order_release);
}
void UiPropertyRefresh::NotifyHistory(uint64_t revision) noexcept {
    impl_->historyWakeFailed.store(false, std::memory_order_release);
    if (impl_->historyRevision.exchange(revision, std::memory_order_acq_rel) != revision)
        impl_->historyPending.store(true, std::memory_order_release);
}
void UiPropertyRefresh::HistoryWakeResult(bool success) noexcept {
    impl_->historyWakeFailed.store(!success, std::memory_order_release);
    if (success) impl_->hostWakeRevision.fetch_add(1, std::memory_order_release);
}
void UiPropertyRefresh::DrainHistory() noexcept {
    if (impl_->historyWakeFailed.load(std::memory_order_acquire)) return;
    const bool pending = impl_->historyAvailable.load(std::memory_order_acquire) && impl_->historyPending.exchange(false, std::memory_order_acq_rel);
    const auto revision = impl_->historyRevision.load(std::memory_order_acquire);
    const auto hostWake = impl_->hostWakeRevision.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        if ((sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE) && sub.hostWake != hostWake && !sub.bound && !sub.ticket && !sub.claimed) {
            if (Impl::Scoped(sub)) impl_->ArmScoped(sub); else { sub.waiting = 0; sub.armed = true; }
        }
    }
    if (!pending || revision == impl_->consumedHistory) return;
    impl_->consumedHistory = revision;
    // At most one increment per original property, even if several INPUTs
    // follow it. Notify-only subscriptions do not receive a history event.
    for (auto& entry : impl_->properties) {
        auto& property = entry.second; bool changed = false;
        for (auto& item : impl_->subscriptions) {
            auto& sub = item.second;
            if (!(sub.flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) || !Same(sub.property, property.ref)) continue;
            if (!changed) { if (property.sequence == UINT64_MAX) break; ++property.sequence; changed = true; }
            sub.latest = property.sequence; sub.dirty = true;
            if (Impl::Scoped(sub)) { impl_->Change(sub, false); continue; }
            if (sub.workGeneration != UINT64_MAX) ++sub.workGeneration;
            if (sub.bound) { sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_BINDING; continue; }
            sub.waiting = 0;
            if (!sub.ticket && !sub.claimed) sub.armed = true;
        }
    }
}
bool UiPropertyRefresh::Take(MmsHandle after, Work& out) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->subscriptions.upper_bound(after); it != impl_->subscriptions.end(); ++it) {
        auto& sub = it->second;
        if (!sub.armed || sub.ticket || sub.claimed || sub.bound || !impl_->Current(*sub.owner)) continue;
        out = {sub.owner->client, sub.handle, sub.target, sub.kinds, sub.latest, 0};
        out.scope = sub.scope; out.workGeneration = sub.workGeneration; out.dependencyRevision = sub.dependencyRevision;
        out.sourcePublication = sub.sourcePublication;
        if (sub.structurePending) sub.structureProbe = true;
        sub.armed = false; sub.claimed = true; return true;
    }
    return false;
}
bool UiPropertyRefresh::Submit(const Work& work, MmsHandle ticket, MmsResult result) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(work.subscription);
    if (found == impl_->subscriptions.end() || found->second.owner->client != work.client) return false;
    auto& sub = found->second; sub.claimed = false; sub.result = result;
    if (!result) {
        sub.ticket = ticket; sub.submitted = work.sequence; sub.dirty = sub.latest != work.sequence;
        sub.submittedWorkGeneration = work.workGeneration; sub.submittedDependencyRevision = work.dependencyRevision;
        sub.submittedSourcePublication = sub.claimedSourcePublication;
        sub.waiting = sub.bound ? MMS_UI_PROPERTY_REFRESH_WAIT_BINDING : 0;
    } else if (Impl::Scoped(sub)) {
        sub.dirty = true; sub.capacity = work.capacityRevision;
        sub.resourceCapacity = work.resourceCapacity;
        sub.displayCapacity = work.resourceCapacity ? work.resourceCapacityRevision : work.displayCapacityRevision;
        if (sub.latest != work.sequence || sub.workGeneration > work.workGeneration) impl_->ArmScoped(sub);
        else {
            sub.waiting = work.waitingFor ? work.waitingFor : result == MMS_E_MEMORY ?
                MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY | MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY : WaitFor(result);
            if (result == MMS_E_FAILED && impl_->historyWakeFailed.load(std::memory_order_acquire)) {
                sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE; sub.hostWake = impl_->hostWakeRevision.load(std::memory_order_acquire);
            }
            sub.armed = false;
            if (sub.sourcePublication > work.sourcePublication && !sub.sourceMissing &&
                (sub.waiting & (MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE | MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER))) impl_->Change(sub, false);
        }
    } else {
        sub.dirty = true; sub.capacity = work.capacityRevision;
        sub.waiting = sub.bound ? MMS_UI_PROPERTY_REFRESH_WAIT_BINDING : WaitFor(result);
        if (!sub.bound && result == MMS_E_FAILED && impl_->historyWakeFailed.load(std::memory_order_acquire)) {
            sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE;
            sub.hostWake = impl_->hostWakeRevision.load(std::memory_order_acquire);
        }
        if (!sub.bound && sub.latest != work.sequence) { sub.waiting = 0; sub.armed = true; }
    }
    return true;
}
void UiPropertyRefresh::Complete(MmsHandle subscription, MmsHandle ticket, uint64_t sequence, MmsResult result, uint32_t waiting,
    uint64_t workGeneration, uint64_t displayCapacityRevision) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(subscription);
    if (found == impl_->subscriptions.end() || found->second.ticket != ticket) return;
    auto& sub = found->second; sub.ticket = 0; sub.result = result;
    if (!result) sub.completed = std::max(sub.completed, sequence);
    if (!result) sub.completedWorkGeneration = std::max(sub.completedWorkGeneration, workGeneration ? workGeneration : sub.submittedWorkGeneration);
    if (Impl::Scoped(sub)) {
        if (result && sub.sourcePublication > sub.submittedSourcePublication && sub.workGeneration == workGeneration) impl_->Change(sub, false);
        if (sub.latest != sequence || sub.workGeneration > workGeneration) { sub.dirty = true; impl_->ArmScoped(sub); }
        else if (result) { sub.dirty = true; sub.waiting = waiting | sub.scopeBlock; sub.armed = false; }
        else { sub.dirty = false; sub.waiting = 0; sub.armed = false; }
        sub.resourceCapacity = false; sub.displayCapacity = displayCapacityRevision;
        return;
    }
    if (sub.bound) { sub.dirty = true; sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_BINDING; sub.armed = false; }
    else if (sub.latest != sequence) { sub.dirty = true; sub.waiting = 0; sub.armed = true; }
    else if (result) { sub.dirty = true; sub.waiting = waiting; sub.armed = false; }
    else { sub.dirty = false; sub.waiting = 0; sub.armed = false; }
}
bool UiPropertyRefresh::Alive(const RefreshClient& client, MmsHandle handle, uint64_t workGeneration) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(handle);
    return found != impl_->subscriptions.end() && found->second.owner->client == &client && !found->second.bound && impl_->Current(*found->second.owner) &&
        (!workGeneration || !Impl::Scoped(found->second) || workGeneration >= found->second.invalidationGeneration);
}
bool UiPropertyRefresh::Pending() const noexcept {
    if (impl_->historyAvailable.load(std::memory_order_acquire) && impl_->historyPending.load(std::memory_order_acquire) &&
        !impl_->historyWakeFailed.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& item : impl_->subscriptions) if (item.second.armed && !item.second.bound && impl_->Current(*item.second.owner)) return true;
    return false;
}
bool UiPropertyRefresh::RetryCapacity(uint64_t revision) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex); bool wake = false;
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        const bool capacity = Impl::Scoped(sub) ? (sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY) != 0 : sub.waiting == MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY;
        if (!capacity || sub.capacity == revision || sub.bound || sub.ticket || sub.claimed) continue;
        if (Impl::Scoped(sub)) {
            // The park belongs to the old structure event. A strict real
            // capacity advance is a new request opportunity for the same
            // still-pending metadata, so clear only its spent probe before
            // the normal arm decision.
            if (sub.structurePending) sub.structureProbe = false;
            impl_->ArmScoped(sub);
        } else { sub.waiting = 0; sub.armed = true; }
        wake |= sub.armed;
    }
    return wake;
}
bool UiPropertyRefresh::TargetPublished(MmsHandle element, bool ready, bool providerChanged) noexcept {
    if (!ready) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex); bool wake = false;
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        bool member = sub.target.element == element;
        if (Impl::Scoped(sub)) for (size_t i = 0; !member && i < sub.memberCount; ++i) member = sub.members[i].element == element;
        const bool scoped = Impl::Scoped(sub);
        const bool waitingTarget = scoped ? (sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_TARGET) != 0 : sub.waiting == MMS_UI_PROPERTY_REFRESH_WAIT_TARGET;
        const bool waitingProvider = scoped ? (sub.waiting & (MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER | MMS_UI_PROPERTY_REFRESH_WAIT_BINDING)) != 0 : sub.waiting == MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER;
        if (!member || sub.bound || sub.ticket || sub.claimed || (!waitingTarget && !(providerChanged && waitingProvider))) continue;
        if (Impl::Scoped(sub)) impl_->Change(sub, false); else { sub.waiting = 0; sub.armed = true; }
        wake |= sub.armed;
    }
    return wake;
}
bool UiPropertyRefresh::BindingChanged(const RefreshClient& client, const MmsUiTargetRefV1& ref, bool bound) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex); bool wake = false;
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        if (Impl::Scoped(sub) || sub.owner->client != &client || !Same(sub.target, ref) || sub.bound == bound) continue;
        sub.bound = bound; sub.dirty = true; sub.waiting = bound ? MMS_UI_PROPERTY_REFRESH_WAIT_BINDING : 0;
        sub.armed = !bound && !sub.ticket && !sub.claimed; wake |= sub.armed;
    }
    return wake;
}
void UiPropertyRefresh::ScopedBindingPublished(const RefreshClient& client, const MmsUiTargetRefV1& ref) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& entry : impl_->subscriptions) {
        auto& sub = entry.second;
        if (!Impl::Scoped(sub) || sub.owner->client != &client || !impl_->Current(*sub.owner)) continue;
        // Only the scope's captured member list may trigger a DEPENDENTS
        // event: its seed is just the anchor. SUBTREE includes its root, so an
        // exact seed ref may also match that scope directly.
        bool affected = sub.scope == MMS_UI_REFRESH_SUBTREE && Same(sub.target, ref);
        for (size_t i = 0; !affected && i < sub.memberCount; ++i) affected = Same(sub.members[i], ref);
        if (!affected) continue;
        sub.bindingObservationPending = true;
        impl_->Change(sub, true);
    }
}
bool UiPropertyRefresh::NextTarget(MmsHandle after, Work& out) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.upper_bound(after);
    if (found == impl_->subscriptions.end()) return false;
    const auto& sub = found->second; out = {sub.owner->client, sub.handle, sub.target, sub.kinds, sub.latest, 0};
    out.scope = sub.scope; out.workGeneration = sub.workGeneration; out.dependencyRevision = sub.dependencyRevision; return true;
}
bool UiPropertyRefresh::NextScoped(MmsHandle after, Work& out) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->subscriptions.upper_bound(after); it != impl_->subscriptions.end(); ++it) {
        const auto& sub = it->second;
        if (!Impl::Scoped(sub) || !impl_->Current(*sub.owner)) continue;
        out = {sub.owner->client, sub.handle, sub.target, sub.kinds, sub.latest, 0};
        out.scope = sub.scope; out.workGeneration = sub.workGeneration; out.dependencyRevision = sub.dependencyRevision;
        out.sourcePublication = sub.sourcePublication; return true;
    }
    return false;
}
void UiPropertyRefresh::ObserveScope(const Work& work, const std::vector<MmsUiTargetRefV1>& members,
    uint64_t dependencyRevision, uint32_t blocked, const std::vector<UiRefreshBindings::PropertySource>& sources) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(work.subscription);
    if (found == impl_->subscriptions.end() || found->second.owner->client != work.client || !Impl::Scoped(found->second)) return;
    impl_->ObserveLocked(found->second, members, dependencyRevision, blocked, sources);
}
void UiPropertyRefresh::ObserveSources(MmsHandle subscription, const std::vector<UiRefreshBindings::PropertySource>& sources, bool captured) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(subscription);
    if (found == impl_->subscriptions.end() || !Impl::Scoped(found->second)) return;
    impl_->ObserveSourcesLocked(found->second, sources, captured);
}
void UiPropertyRefresh::SourcePublished(RefreshClient& client, MmsHandle source, uint64_t revision) noexcept {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& entry : impl_->subscriptions) {
            auto& sub = entry.second;
            if (!Impl::Scoped(sub) || sub.owner->client != &client || !impl_->Current(*sub.owner)) continue;
            bool changed = false, missing = false;
            for (size_t i = 0; i < sub.sourceCount; ++i) {
                auto& watch = sub.sources[i];
                if (watch.source == source && revision > watch.revision) { watch.revision = revision; changed = true; }
                missing |= watch.revision == 0;
            }
            if (!changed) continue;
            sub.sourceMissing = missing;
            if (sub.sourcePublication != UINT64_MAX) ++sub.sourcePublication;
            if (!sub.ticket && !sub.claimed && (sub.waiting & (MMS_UI_PROPERTY_REFRESH_WAIT_SOURCE | MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER))) {
                const bool armed = sub.armed; impl_->Change(sub, false); wake |= !armed && sub.armed;
            }
        }
    }
    impl_->Wake(wake);
}
void UiPropertyRefresh::GraphChanged(const RefreshClient& client, const MmsUiTargetRefV1& upstream,
    const UiRefreshDependencies& graph) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& entry : impl_->subscriptions) {
        auto& sub = entry.second;
        if (sub.owner->client != &client || sub.scope != MMS_UI_REFRESH_DEPENDENTS) continue;
        // The retained members may be missing or stale after a failed
        // observation, and they may still list a node that left the graph.
        // Only the exact current graph decides whether this event is this
        // subscription's business.
        if (!graph.Reaches(client, sub.target, upstream)) continue;
        // A real graph mutation records one bounded structure event; only its
        // single probe may bypass the cached graph/source park.
        sub.structurePending = true; sub.structureProbe = false; impl_->Change(sub, true);
    }
}
void UiPropertyRefresh::ScopedTopologyChanged(void* context, bool (*changed)(void*, MmsHandle)) noexcept {
    if (!changed) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& entry : impl_->subscriptions) {
        auto& sub = entry.second;
        if (!Impl::Scoped(sub) || sub.scope != MMS_UI_REFRESH_SUBTREE || !changed(context, sub.target.element)) continue;
        sub.structurePending = true; sub.structureProbe = false; impl_->Change(sub, true);
    }
}
bool UiPropertyRefresh::RetryDisplayCapacity(uint64_t displayRevision, uint64_t resourceRevision) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex); bool wake = false;
    for (auto& entry : impl_->subscriptions) {
        auto& sub = entry.second;
        const auto revision = sub.resourceCapacity ? resourceRevision : displayRevision;
        if (!Impl::Scoped(sub) || !(sub.waiting & MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY) ||
            revision == UINT64_MAX || sub.displayCapacity == revision || sub.ticket || sub.claimed) continue;
        // Same as the queue-capacity clock: only the strict real advance asks
        // for a new request while the structure metadata is still pending.
        if (sub.structurePending) sub.structureProbe = false;
        impl_->ArmScoped(sub); wake |= sub.armed;
    }
    return wake;
}
void UiPropertyRefresh::FailScheduling(MmsResult result) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        // Do not convert an unrelated consumer/capacity/binding failure into
        // a host-wake retry. Only work that this signal would have scheduled
        // is affected. Claimed Request failure is classified by Submit.
        if (!sub.armed || sub.ticket || sub.claimed || sub.bound) continue;
        sub.armed = false; sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE; sub.result = result;
        sub.hostWake = impl_->hostWakeRevision.load(std::memory_order_acquire);
    }
}
void UiPropertyRefresh::InvalidateTarget(MmsHandle element) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->subscriptions.begin(); it != impl_->subscriptions.end();) {
        if (it->second.target.element == element) it = impl_->subscriptions.erase(it); else ++it;
    }
}
void UiPropertyRefresh::InvalidateProperty(MmsHandle handle) noexcept {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->properties.erase(handle);
        for (auto it = impl_->subscriptions.begin(); it != impl_->subscriptions.end();) {
            if (it->second.property.property == handle) it = impl_->subscriptions.erase(it); else ++it;
        }
    }
    impl_->queue.CleanupSubscriptions();
}
void UiPropertyRefresh::Retire(UiClient& client) noexcept {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& owner : impl_->owners) if (owner.second->ui == &client) owner.second->retired = true;
        for (auto it = impl_->subscriptions.begin(); it != impl_->subscriptions.end();) {
            if (it->second.owner->ui == &client) it = impl_->subscriptions.erase(it); else ++it;
        }
        for (auto it = impl_->properties.begin(); it != impl_->properties.end();) {
            if (it->second.owner->ui == &client) it = impl_->properties.erase(it); else ++it;
        }
    }
    impl_->queue.CleanupSubscriptions();
}
void UiPropertyRefresh::Retire(RefreshClient& client) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto owner = impl_->owners.find(&client); if (owner == impl_->owners.end()) return;
    owner->second->retired = true;
    for (auto it = impl_->subscriptions.begin(); it != impl_->subscriptions.end();) {
        if (it->second.owner == owner->second.get()) it = impl_->subscriptions.erase(it); else ++it;
    }
    for (auto it = impl_->properties.begin(); it != impl_->properties.end();) {
        if (it->second.owner == owner->second.get()) it = impl_->properties.erase(it); else ++it;
    }
}
void UiPropertyRefresh::AdvanceProjectEpoch(uint64_t epoch) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->epoch = epoch;
    impl_->subscriptions.clear(); impl_->properties.clear();
    impl_->historyPending.store(false, std::memory_order_release);
    impl_->consumedHistory = impl_->historyRevision.load(std::memory_order_acquire);
}
}
