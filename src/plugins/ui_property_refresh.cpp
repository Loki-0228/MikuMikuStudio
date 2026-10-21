#include "ui_property_refresh.hpp"
#include "ui_registry.hpp"
#include "ui_refresh_binding.hpp"

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
    void Wake(bool pending) noexcept { if (pending) queue.WakeSubscriptions(); }
    MmsResult Capabilities(Owner& owner, MmsUiPropertyRefreshCapabilitiesV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        if (const auto result = self.CheckUi(*owner.client, owner.ui)) return result;
        std::lock_guard<std::mutex> lock(mutex);
        if (!Current(owner)) return MMS_E_STALE;
        *out = {Header<MmsUiPropertyRefreshCapabilitiesV1>(), kOwnerLimit, kKinds,
            historyAvailable.load() ? MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY : 0u, 0, 0};
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
                if (sub.target.element == desc->target.element) return MMS_E_CONFLICT;
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
                if (sub.bound) { sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_BINDING; continue; }
                sub.waiting = 0;
                if (!sub.ticket && !sub.claimed) { wake |= !sub.armed; sub.armed = true; }
            }
        }
        Wake(wake); return MMS_OK;
    }
    MmsResult Query(Owner& owner, MmsHandle handle, MmsUiPropertyRefreshSubscriptionStateV1* out) {
        if (!handle || !Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = subscriptions.find(handle);
        if (!Current(owner) || found == subscriptions.end() || found->second.owner != &owner) return MMS_E_STALE;
        const auto& sub = found->second;
        const bool deferredHistory = (sub.flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) && historyAvailable.load(std::memory_order_acquire) &&
            historyPending.load(std::memory_order_acquire) && historyRevision.load(std::memory_order_acquire) != consumedHistory;
        const uint32_t historyWait = deferredHistory && historyWakeFailed.load(std::memory_order_acquire) ? MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE : 0;
        *out = {Header<MmsUiPropertyRefreshSubscriptionStateV1>(), handle, sub.target, sub.property,
            sub.latest, sub.submitted, sub.completed, historyWait ? MMS_E_FAILED : sub.result,
            uint32_t(sub.dirty || sub.claimed || sub.ticket || deferredHistory), sub.waiting | historyWait, 0};
        return MMS_OK;
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
        if (sub.waiting == MMS_UI_PROPERTY_REFRESH_WAIT_HOST_WAKE && sub.hostWake != hostWake && !sub.bound && !sub.ticket && !sub.claimed) {
            sub.waiting = 0; sub.armed = true;
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
        sub.waiting = sub.bound ? MMS_UI_PROPERTY_REFRESH_WAIT_BINDING : 0;
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
void UiPropertyRefresh::Complete(MmsHandle subscription, MmsHandle ticket, uint64_t sequence, MmsResult result, uint32_t waiting) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(subscription);
    if (found == impl_->subscriptions.end() || found->second.ticket != ticket) return;
    auto& sub = found->second; sub.ticket = 0; sub.result = result;
    if (!result) sub.completed = std::max(sub.completed, sequence);
    if (sub.bound) { sub.dirty = true; sub.waiting = MMS_UI_PROPERTY_REFRESH_WAIT_BINDING; sub.armed = false; }
    else if (sub.latest != sequence) { sub.dirty = true; sub.waiting = 0; sub.armed = true; }
    else if (result) { sub.dirty = true; sub.waiting = waiting; sub.armed = false; }
    else { sub.dirty = false; sub.waiting = 0; sub.armed = false; }
}
bool UiPropertyRefresh::Alive(const RefreshClient& client, MmsHandle handle) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.find(handle);
    return found != impl_->subscriptions.end() && found->second.owner->client == &client && !found->second.bound && impl_->Current(*found->second.owner);
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
        if (sub.waiting != MMS_UI_PROPERTY_REFRESH_WAIT_CAPACITY || sub.capacity == revision || sub.bound || sub.ticket || sub.claimed) continue;
        sub.waiting = 0; sub.armed = true; wake = true;
    }
    return wake;
}
bool UiPropertyRefresh::TargetPublished(MmsHandle element, bool ready, bool providerChanged) noexcept {
    if (!ready) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex); bool wake = false;
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        if (sub.target.element != element || sub.bound || sub.ticket || sub.claimed ||
            (sub.waiting != MMS_UI_PROPERTY_REFRESH_WAIT_TARGET && !(providerChanged && sub.waiting == MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER))) continue;
        sub.waiting = 0; sub.armed = true; wake = true;
    }
    return wake;
}
bool UiPropertyRefresh::BindingChanged(const RefreshClient& client, const MmsUiTargetRefV1& ref, bool bound) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex); bool wake = false;
    for (auto& item : impl_->subscriptions) {
        auto& sub = item.second;
        if (sub.owner->client != &client || !Same(sub.target, ref) || sub.bound == bound) continue;
        sub.bound = bound; sub.dirty = true; sub.waiting = bound ? MMS_UI_PROPERTY_REFRESH_WAIT_BINDING : 0;
        sub.armed = !bound && !sub.ticket && !sub.claimed; wake |= sub.armed;
    }
    return wake;
}
bool UiPropertyRefresh::NextTarget(MmsHandle after, Work& out) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->subscriptions.upper_bound(after);
    if (found == impl_->subscriptions.end()) return false;
    const auto& sub = found->second; out = {sub.owner->client, sub.handle, sub.target, sub.kinds, sub.latest, 0}; return true;
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
