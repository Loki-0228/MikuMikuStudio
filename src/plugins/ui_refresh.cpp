#include "ui_refresh.hpp"
#include "ui_registry.hpp"
#include "ui_refresh_binding.hpp"
#include "ui_refresh_dependencies.hpp"
#include "ui_property_refresh.hpp"
#include <cmath>
#include <limits>

namespace mikumikustudio::plugins {
namespace {
constexpr std::size_t kTickets = 256, kGlobalTickets = 4096, kRects = 32;
constexpr std::size_t kBytes = 256 * 1024, kGlobalBytes = 4 * 1024 * 1024, kBatch = 64;
constexpr std::size_t kSubtreeMembers = 256;
constexpr std::uint32_t kSupported = MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_DATA;
constexpr std::uint32_t kAllKinds = kSupported | MMS_UI_REFRESH_LAYOUT;
bool MenuTarget(const RefreshPresentation& p) noexcept { return p.kind == MMS_UI_MENU || p.kind == MMS_UI_MENU_ITEM; }
bool SameRef(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) noexcept {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
struct Flight {
    RefreshClient& client;
    explicit Flight(RefreshClient& c) : client(c) { client.inFlight.fetch_add(1); }
    ~Flight() { client.inFlight.fetch_sub(1); }
};
struct RectangleGeometry {
    float x = 0, y = 0, width = 0, height = 0;
    uint32_t dpi = 96, coordinateDpi = 96;
    HWND coordinateWindow = nullptr;
    LONG pixelX = 0, pixelY = 0;
    bool pixelOrigin = false;
    RectangleGeometry() noexcept = default;
    explicit RectangleGeometry(const RefreshPresentation& p) noexcept : x(p.x), y(p.y), width(p.width), height(p.height),
        dpi(p.dpi), coordinateDpi(p.coordinateDpi), coordinateWindow(p.coordinateWindow), pixelX(p.pixelX), pixelY(p.pixelY), pixelOrigin(p.pixelOrigin) {}
    bool Same(const RefreshPresentation& p) const noexcept {
        return x == p.x && y == p.y && width == p.width && height == p.height && dpi == p.dpi &&
            coordinateDpi == p.coordinateDpi && coordinateWindow == p.coordinateWindow && pixelOrigin == p.pixelOrigin &&
            (!pixelOrigin || (pixelX == p.pixelX && pixelY == p.pixelY));
    }
};
bool SameCoordinateSpace(const RefreshPresentation& a, const RefreshPresentation& b) noexcept {
    return a.coordinateDpi == b.coordinateDpi && a.coordinateWindow == b.coordinateWindow;
}
double Origin(const RefreshPresentation& p, bool vertical) noexcept {
    return p.pixelOrigin ? double(vertical ? p.pixelY : p.pixelX) : double(vertical ? p.y : p.x) * p.coordinateDpi / 96;
}
MmsResult Invalidate(const RefreshPresentation& target, bool whole, const std::vector<MmsUiDirtyRectV1>& rects,
    const RefreshPresentation* root = nullptr) {
    if (!IsWindow(target.window)) return MMS_E_STALE;
    if (whole) return InvalidateRect(target.window, nullptr, FALSE) ? MMS_OK : MMS_E_FAILED;
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return MMS_E_MEMORY;
    struct RegionGuard { HRGN value; ~RegionGuard() { DeleteObject(value); } } guard{region};
    const double scale = target.dpi / 96.0;
    bool any = false;
    for (const auto& r : rects) {
        const auto& source = root ? *root : target;
        double left = std::clamp(double(r.x), 0.0, double(source.width));
        double top = std::clamp(double(r.y), 0.0, double(source.height));
        double right = std::clamp(double(r.x) + r.width, 0.0, double(source.width));
        double bottom = std::clamp(double(r.y) + r.height, 0.0, double(source.height));
        if (r.width == 0 || r.height == 0 || right <= left || bottom <= top) continue;
        if (root) {
            const double dx = Origin(source, false) - Origin(target, false), dy = Origin(source, true) - Origin(target, true);
            const double rootScale = source.dpi / 96.0;
            left = std::clamp(dx + left * rootScale, 0.0, double(target.width) * scale);
            top = std::clamp(dy + top * rootScale, 0.0, double(target.height) * scale);
            right = std::clamp(dx + right * rootScale, 0.0, double(target.width) * scale);
            bottom = std::clamp(dy + bottom * rootScale, 0.0, double(target.height) * scale);
        } else { left *= scale; top *= scale; right *= scale; bottom *= scale; }
        if (right <= left || bottom <= top) continue;
        HRGN part = CreateRectRgn(int(std::floor(left)), int(std::floor(top)), int(std::ceil(right)), int(std::ceil(bottom)));
        if (!part) return MMS_E_MEMORY;
        const auto combined = CombineRgn(region, region, part, RGN_OR); DeleteObject(part);
        if (combined == ERROR) return MMS_E_MEMORY;
        any = true;
    }
    return !any || InvalidateRgn(target.window, region, FALSE) ? MMS_OK : MMS_E_FAILED;
}
}
struct UiRefreshQueue::Target {
    RefreshPresentation presentation;
    std::uint64_t generation = 0, geometry = 0, stateProviderRevision = 0;
    bool suspended = false;
    bool Ready() const noexcept { return !suspended && (MenuTarget(presentation) ?
        presentation.menu && presentation.menuCookie && presentation.menuItem : presentation.window &&
        (presentation.container || (presentation.width > 0 && presentation.height > 0))); }
    bool ReadyFor(uint32_t kinds, bool allowSuspendedLayout = false) const noexcept {
        // A published collapsed button can request STATE|LAYOUT to regain its
        // slot; pure REPAINT/STATE keep their original positive-size rule.
        // After a failed geometry publication, only a layout repair (with an
        // optional repaint after it) may reuse the still-live exact instance.
        // DATA and STATE must wait until that repair publishes ready geometry.
        const bool layoutRepair = !suspended || (allowSuspendedLayout && !(kinds & (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE)));
        return Ready() || ((kinds & MMS_UI_REFRESH_LAYOUT) && layoutRepair &&
            presentation.window && presentation.layoutRefresh);
    }
};
struct UiRefreshQueue::Ticket {
    struct Member {
        MmsUiTargetRefV1 ref{};
        uint64_t geometry = 0, stateProviderRevision = 0;
        uint32_t kinds = 0;
        bool propertyData = false;
        std::shared_ptr<const PropertyRecord> propertyRecord;
    };
    RefreshClient* owner = nullptr;
    MmsUiTargetRefV1 target{};
    std::uint64_t expectedGeometry = 0, key = 0;
    std::uint32_t kinds = MMS_UI_REFRESH_REPAINT;
    uint32_t scope = MMS_UI_REFRESH_SELF;
    uint64_t topology = 0, dependencyRevision = 0;
    MmsHandle binding = 0;
    UiRefreshBindings* subscriptionHost = nullptr;
    UiPropertyRefresh* propertySubscriptionHost = nullptr;
    uint64_t* capacityRevision = nullptr;
    MmsHandle subscription = 0, handle = 0;
    MmsHandle propertySubscription = 0;
    uint64_t propertySequence = 0;
    std::uint64_t sourceRevision = 0;
    std::uint64_t stateProviderRevision = 0;
    UiRefreshBindings::Contribution data;
    bool propertyData = false;
    std::shared_ptr<const PropertyRecord> propertyRecord;
    std::vector<Member> members;
    std::vector<MmsUiTargetRefV1> dataTargets;
    std::vector<UiRefreshBindings::Captured> captured;
    std::vector<MmsUiDirtyRectV1> rects;
    bool whole = false, released = false;
    MmsUiRefreshStateV1 state{Header<MmsUiRefreshStateV1>(), MMS_UI_REFRESH_PENDING, 0, MMS_OK, 0, 0, 0};
    std::size_t Bytes() const noexcept { return sizeof(Ticket) + rects.capacity() * sizeof(MmsUiDirtyRectV1) +
        members.capacity() * sizeof(Member) + dataTargets.capacity() * sizeof(MmsUiTargetRefV1) +
        captured.capacity() * sizeof(UiRefreshBindings::Captured); }
    void Finish(std::uint32_t status, MmsResult result, std::uint64_t geometry = 0, std::uint32_t completed = 0,
                uint32_t subscriptionWait = 0, uint64_t subscriptionCapacityRevision = UINT64_MAX,
                uint32_t propertyWait = MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER) noexcept {
        state.status = status; state.result = result; state.geometryRevision = geometry;
        state.completedKinds = status == MMS_UI_REFRESH_COMPLETED ? kinds : completed;
        data.reset();
        propertyRecord.reset();
        std::vector<MmsUiDirtyRectV1>().swap(rects);
        std::vector<Member>().swap(members);
        std::vector<MmsUiTargetRefV1>().swap(dataTargets);
        std::vector<UiRefreshBindings::Captured>().swap(captured);
        if (subscriptionHost) {
            // Completion frees contribution memory. Retry blocked mailboxes
            // once for this real capacity change, never once per timer tick.
            if (capacityRevision && *capacityRevision != UINT64_MAX) ++*capacityRevision;
            subscriptionHost->RetrySubscriptions(UiRefreshBindings::RetryQueueCapacity, capacityRevision ? *capacityRevision : 0);
            // Cancel/provider invalidation can finish outside Drain. Clearing
            // the final accepted contribution may free a retained snapshot.
            subscriptionHost->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
            if (propertySubscriptionHost) propertySubscriptionHost->RetryCapacity(capacityRevision ? *capacityRevision : 0);
            // Record this ticket's own BUSY/NOT_READY only after that event;
            // its cleanup must not immediately retry the same failed work.
            if (subscription) subscriptionHost->CompleteSubscription(subscription, handle, sourceRevision, result,
                subscriptionWait, subscriptionCapacityRevision);
        }
        if (propertySubscription && propertySubscriptionHost)
            propertySubscriptionHost->Complete(propertySubscription, handle, propertySequence, result, propertyWait);
    }
    void FinishValidation(MmsResult result) noexcept {
        Finish(result == MMS_E_CANCELLED ? MMS_UI_REFRESH_CANCELLED : MMS_UI_REFRESH_FAILED, result, 0, 0, 0, UINT64_MAX,
            result == MMS_E_NOT_READY ? MMS_UI_PROPERTY_REFRESH_WAIT_TARGET : MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER);
    }
};
UiRefreshQueue::UiRefreshQueue(UiRegistry& registry) : registry_(registry),
    bindings_(std::make_unique<UiRefreshBindings>(UiRefreshBindings::Hooks{
        [this](RefreshClient& client, const MmsUiTargetRefV1& ref) -> MmsResult {
            MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
            if (auto result = GetTarget(client, ref.element, &target)) return result;
            if (!SameRef(ref, target.target)) return MMS_E_STALE;
            std::lock_guard<std::mutex> lock(mutex_);
            const auto native = targets_.find(ref.element);
            if (native != targets_.end() && MenuTarget(native->second.presentation)) return MMS_E_UNSUPPORTED;
            if (native == targets_.end() || !IsWindow(native->second.presentation.window)) return MMS_E_STALE;
            if (native->second.presentation.container) return MMS_E_UNSUPPORTED;
            return target.ready ? MMS_OK : MMS_E_NOT_READY;
        },
        [this](const std::function<MmsResult()>& callback) { return registry_.WithReadOnlyCallback(callback); },
        [this] { return registry_.InCallback() || ReadOnlyCause(); },
        [this](RefreshClient& client, const MmsUiTargetRefV1& ref, uint32_t kinds) -> MmsResult {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ReadOnlyCauseLocked(client)) return MMS_E_BUSY;
            const auto found = targets_.find(ref.element);
            if (ref.projectEpoch != epoch_ || found == targets_.end() || found->second.generation != ref.instanceGeneration) return MMS_E_STALE;
            const auto& p = found->second.presentation;
            if (client.owner == "mms" || p.owner != client.owner) return MMS_E_ARGUMENT;
            if (p.container || MenuTarget(p)) return MMS_E_UNSUPPORTED;
            if (!IsWindow(p.window)) return MMS_E_STALE;
            const uint32_t supported = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT |
                (p.stateRefresh ? MMS_UI_REFRESH_STATE : 0) | (p.layoutRefresh && layout_ ? MMS_UI_REFRESH_LAYOUT : 0);
            return kinds & ~supported ? MMS_E_UNSUPPORTED : MMS_OK;
        },
        [this] { WakeSubscriptions(); }, [this] { CleanupSubscriptions(); },
        [this] { RetrySubscriptions(); }, [this] { PropertyBindingsChanged(); }})),
    dependencies_(std::make_unique<UiRefreshDependencies>()), propertyRefresh_(std::make_unique<UiPropertyRefresh>(*this, registry)) {}
UiRefreshQueue::~UiRefreshQueue() = default;
std::map<MmsHandle, UiRefreshQueue::Ticket>::iterator UiRefreshQueue::EraseTicketLocked(std::map<MmsHandle, Ticket>::iterator it) noexcept {
    const auto next = tickets_.erase(it);
    if (subscriptionCapacityRevision_ != UINT64_MAX) ++subscriptionCapacityRevision_;
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryQueueCapacity, subscriptionCapacityRevision_);
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
    propertyRefresh_->RetryCapacity(subscriptionCapacityRevision_);
    return next;
}
void UiRefreshQueue::WakeSubscriptions() noexcept {
    // Binding publishers call this only after releasing the binding lock.
    // The configured hook posts a host message; it never runs UI work here.
    std::lock_guard<std::mutex> lock(mutex_);
    SignalSubscriptionWakeLocked();
}
void UiRefreshQueue::SignalSubscriptionWakeLocked() noexcept {
    if (wake_) {
        bool signalled = false; try { signalled = wake_(); } catch (...) {}
        propertyRefresh_->HistoryWakeResult(signalled);
        if (!signalled) {
            bindings_->FailSubscriptionScheduling(MMS_E_FAILED);
            propertyRefresh_->FailScheduling(MMS_E_FAILED);
        }
    }
}
void UiRefreshQueue::CleanupSubscriptions() noexcept {
    std::lock_guard<std::mutex> lock(mutex_); bool freed = false;
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        const bool deadBinding = t.subscription && !bindings_->SubscriptionAlive(*t.owner, t.subscription);
        const bool deadProperty = t.propertySubscription && !propertyRefresh_->Alive(*t.owner, t.propertySubscription);
        if (!deadBinding && !deadProperty) { ++it; continue; }
        if (t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_CANCELLED, MMS_E_CANCELLED);
        if (t.state.status != MMS_UI_REFRESH_RUNNING) { it = EraseTicketLocked(it); freed = true; }
        else ++it;
    }
    if (freed) bindings_->RetrySubscriptions(UiRefreshBindings::RetryQueueCapacity, subscriptionCapacityRevision_);
    // UnregisterSource/Unbind can release an unsubscribed retained snapshot
    // before invoking cleanup, without leaving any internal ticket to erase.
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
bool UiRefreshQueue::PendingSubscriptionsLocked() const noexcept {
    return bindings_->PendingSubscriptions() || propertyRefresh_->Pending();
}
void UiRefreshQueue::PropertyBindingsChanged() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        UiPropertyRefresh::Work work; MmsHandle after = 0;
        while (propertyRefresh_->NextTarget(after, work)) {
            after = work.subscription;
            propertyRefresh_->BindingChanged(*work.client, work.target, bindings_->HasBinding(*work.client, work.target));
        }
    }
    // The binding caller performs one cleanup after this notification, outside
    // its store lock. Unbind/Unregister already share that cleanup boundary.
}
void UiRefreshQueue::SchedulePropertySubscriptions() {
    propertyRefresh_->DrainHistory();
    UiPropertyRefresh::Work work; MmsHandle after = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!propertyRefresh_->Take(after, work)) break;
            work.capacityRevision = subscriptionCapacityRevision_;
        }
        after = work.subscription;
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = work.target; request.kinds = work.kinds; request.scope = MMS_UI_REFRESH_SELF;
        MmsHandle ticket = 0;
        const auto result = Boundary([&] { return Request(*work.client, &request, &ticket, 0, work.subscription, work.sequence); });
        bool retained = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            retained = propertyRefresh_->Submit(work, ticket, result);
            if (result) propertyRefresh_->RetryCapacity(subscriptionCapacityRevision_);
        }
        if (!retained && !result) CleanupSubscriptions();
    }
}
MmsResult UiRefreshQueue::CheckPropertyRefreshTarget(RefreshClient& client, const UiClient* ui, const MmsUiTargetRefV1& ref,
    const std::shared_ptr<const PropertyRecord>& property, uint32_t kinds, bool& bound, bool& ready) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(ref.element);
    if (ref.projectEpoch != epoch_ || found == targets_.end() || found->second.generation != ref.instanceGeneration) return MMS_E_STALE;
    const auto& target = found->second; const auto& p = target.presentation;
    if (!property || property->owner != ui || p.propertyRecord != property || !registry_.IsPropertyInputTargetCurrent(ref.element, property.get())) return MMS_E_ARGUMENT;
    if (p.owner != client.owner) return MMS_E_ARGUMENT;
    if (!IsWindow(p.window)) return MMS_E_STALE;
    if (!p.propertyDataRefresh || !propertyData_ || p.container || p.kind != MMS_UI_INPUT) return MMS_E_UNSUPPORTED;
    const uint32_t supported = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT |
        (p.stateRefresh ? MMS_UI_REFRESH_STATE : 0) | (p.layoutRefresh && layout_ ? MMS_UI_REFRESH_LAYOUT : 0);
    if (kinds & ~supported) return MMS_E_UNSUPPORTED;
    bound = bindings_->HasBinding(client, ref); ready = target.ReadyFor(kinds); return MMS_OK;
}
bool UiRefreshQueue::PropertyReadOnlyCause(const RefreshClient& client, const UiClient* ui) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evaluatingStateOwner_ == &client || evaluatingDataOwner_ == &client || evaluatingLayoutOwner_ == &client ||
        inheritedReadOnlyCause_ == &client || (ui && (evaluatingStateProvider_ == ui || evaluatingLayoutProvider_ == ui));
}
void UiRefreshQueue::RetrySubscriptions() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity)) SignalSubscriptionWakeLocked();
}
void UiRefreshQueue::ScheduleSubscriptions() {
    // Each handle is considered at most once per drain. A racing publisher
    // may arm the same mailbox again, but cannot turn this into a busy loop.
    UiRefreshBindings::SubscriptionWork work; MmsHandle after = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!bindings_->TakeSubscription(after, work)) break;
            work.queueCapacityRevision = subscriptionCapacityRevision_;
        }
        after = work.subscription;
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = work.target; request.binding = work.binding;
        request.kinds = work.kinds; request.scope = MMS_UI_REFRESH_SELF; request.sourceRevision = work.revision;
        MmsHandle ticket = 0;
        const auto result = Boundary([&] { return Request(*work.client, &request, &ticket, work.subscription); });
        bool retained = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            retained = bindings_->SubmitSubscription(work, ticket, result);
            // A worker may have released capacity after Request failed but
            // before its mailbox was parked. Replay only newer real events.
            if (result) {
                bindings_->RetrySubscriptions(UiRefreshBindings::RetryQueueCapacity, subscriptionCapacityRevision_);
                bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
            }
        }
        if (!retained && !result) CleanupSubscriptions();
    }
}
void UiRefreshQueue::PruneInvalidSubscriptionTargets() {
    // UI-only safe point. A plugin may destroy a surface HWND before the
    // next retained-layout publication. Retire that original presentation
    // and its bindings immediately; never reenter InvalidateTarget's lock.
    std::lock_guard<std::mutex> lock(mutex_); bool removed = false;
    for (auto it = targets_.begin(); it != targets_.end();) {
        if (MenuTarget(it->second.presentation) || IsWindow(it->second.presentation.window)) { ++it; continue; }
        const auto element = it->first; it = targets_.erase(it); removed = true;
        bindings_->InvalidateTarget(element); dependencies_->InvalidateTarget(element); propertyRefresh_->InvalidateTarget(element);
    }
    if (!removed) return;
    InvalidateDependencyTicketsLocked();
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.state.status == MMS_UI_REFRESH_PENDING) {
            if (const auto result = ValidateTicketLocked(t, true))
                t.FinishValidation(result);
        }
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING)
            it = EraseTicketLocked(it); else ++it;
    }
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
void UiRefreshQueue::SetWake(std::function<bool()> wake) { wake_ = std::move(wake); }
void UiRefreshQueue::SetLayout(std::function<MmsResult(MmsHandle)> layout) {
    if (GetCurrentThreadId() != thread_) throw std::invalid_argument("refresh layout thread");
    std::lock_guard<std::mutex> lock(mutex_); layout_ = std::move(layout);
}
void UiRefreshQueue::SetStateContent(std::function<MmsResult(MmsHandle, const std::shared_ptr<const ButtonStateContent>&, bool)> callback) {
    SetValidatedStateContent(callback ? std::function<MmsResult(MmsHandle, const std::shared_ptr<const ButtonStateContent>&,
        bool, const std::function<MmsResult()>&)>([callback = std::move(callback)](auto element, const auto& content, bool enabled, const auto&) {
            return callback(element, content, enabled);
        }) : nullptr);
}
void UiRefreshQueue::SetValidatedStateContent(std::function<MmsResult(MmsHandle, const std::shared_ptr<const ButtonStateContent>&,
    bool, const std::function<MmsResult()>&)> callback) {
    if (GetCurrentThreadId() != thread_) throw std::invalid_argument("refresh content thread");
    std::lock_guard<std::mutex> lock(mutex_); stateContent_ = std::move(callback);
}
void UiRefreshQueue::SetPropertyData(std::function<MmsResult(MmsHandle, const std::function<MmsResult()>&, uint64_t&)> callback) {
    if (GetCurrentThreadId() != thread_) throw std::invalid_argument("refresh property data thread");
    std::lock_guard<std::mutex> lock(mutex_); propertyData_ = std::move(callback);
    if (propertyData_) for (const auto& entry : targets_)
        propertyRefresh_->TargetPublished(entry.first, entry.second.Ready(), true);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
bool UiRefreshQueue::HasStateContent() const { std::lock_guard<std::mutex> lock(mutex_); return bool(stateContent_); }
MmsResult UiRefreshQueue::CheckStateContentTarget(const std::string& owner, const MmsUiTargetRefV1& ref) const {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(&ref) || !ref.element || !ref.instanceGeneration || !ref.projectEpoch) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(ref.element);
    if (ref.projectEpoch != epoch_ || found == targets_.end() || found->second.generation != ref.instanceGeneration) return MMS_E_STALE;
    const auto& target = found->second;
    if (owner == "mms" || owner != target.presentation.owner) return MMS_E_ARGUMENT;
    if (!stateContent_ || !target.presentation.stateRefresh) return MMS_E_UNSUPPORTED;
    if (!MenuTarget(target.presentation) && !IsWindow(target.presentation.window)) return MMS_E_STALE;
    return target.ReadyFor(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT) ? MMS_OK : MMS_E_NOT_READY;
}
void UiRefreshQueue::SetStateProviderRevision(MmsHandle element, uint64_t revision) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    bool providerChanged = false;
    if (const auto found = targets_.find(element); found != targets_.end()) providerChanged = found->second.stateProviderRevision != revision;
    if (const auto found = targets_.find(element); found != targets_.end()) found->second.stateProviderRevision = revision;
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& ticket = it->second;
        if (ticket.state.status == MMS_UI_REFRESH_PENDING && (ticket.kinds & MMS_UI_REFRESH_STATE) &&
            ((ticket.scope == MMS_UI_REFRESH_SELF && ticket.target.element == element && ticket.stateProviderRevision != revision) ||
             std::any_of(ticket.members.begin(), ticket.members.end(), [&](const auto& m) {
                 return m.ref.element == element && (m.kinds & MMS_UI_REFRESH_STATE) && m.stateProviderRevision != revision;
             })))
            ticket.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (ticket.released && ticket.state.status != MMS_UI_REFRESH_PENDING && ticket.state.status != MMS_UI_REFRESH_RUNNING)
            it = EraseTicketLocked(it); else ++it;
    }
    if (providerChanged) if (const auto found = targets_.find(element); found != targets_.end())
        propertyRefresh_->TargetPublished(element, found->second.Ready(), true);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
bool UiRefreshQueue::CachedStateVisible(MmsHandle element, bool fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(element);
    return found == targets_.end() ? fallback : found->second.presentation.stateVisible;
}
MmsResult UiRefreshQueue::WithLayoutProvider(const UiClient& provider, const std::function<MmsResult()>& callback) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    const UiClient* previous = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); previous = evaluatingLayoutProvider_;
        if (evaluatingLayoutOwner_) evaluatingLayoutProvider_ = &provider; }
    struct Guard { UiRefreshQueue& queue; const UiClient* previous; ~Guard() {
        std::lock_guard<std::mutex> lock(queue.mutex_); queue.evaluatingLayoutProvider_ = previous;
    } } guard{*this, previous};
    return callback ? callback() : MMS_E_ARGUMENT;
}
MmsUiRefreshApiV1 UiRefreshQueue::Api(RefreshClient& client) {
    MmsUiRefreshApiV1 api{}; api.h = Header<MmsUiRefreshApiV1>(); api.context = &client;
#define REFRESH_CALL(body) auto& c = *static_cast<RefreshClient*>(p); Flight flight(c); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiRefreshCapabilitiesV1* out) { REFRESH_CALL(return c.queue->Capabilities(c, out)); };
    api.getTarget = [](void* p, MmsHandle h, MmsUiRefreshTargetV1* out) { REFRESH_CALL(return c.queue->GetTarget(c, h, out)); };
    api.request = [](void* p, const MmsUiRefreshRequestV1* request, MmsHandle* out) { REFRESH_CALL(return c.queue->Request(c, request, out)); };
    api.query = [](void* p, MmsHandle h, MmsUiRefreshStateV1* out) { REFRESH_CALL(return c.queue->Query(c, h, out)); };
    api.cancel = [](void* p, MmsHandle h) { REFRESH_CALL(return c.queue->Cancel(c, h)); };
    api.release = [](void* p, MmsHandle h) { REFRESH_CALL(return c.queue->Release(c, h)); };
#undef REFRESH_CALL
    return api;
}
MmsUiRefreshDependencyApiV1 UiRefreshQueue::DependencyApi(RefreshClient& client) {
    MmsUiRefreshDependencyApiV1 api{}; api.h = Header<MmsUiRefreshDependencyApiV1>(); api.context = &client;
#define DEPENDENCY_CALL(body) auto& c = *static_cast<RefreshClient*>(p); Flight flight(c); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiRefreshDependencyCapabilitiesV1* out) { DEPENDENCY_CALL(return c.queue->DependencyCapabilities(c, out)); };
    api.registerDependency = [](void* p, const MmsUiRefreshDependencyDescV1* desc, MmsHandle* out) { DEPENDENCY_CALL(return c.queue->RegisterDependency(c, desc, out)); };
    api.unregisterDependency = [](void* p, MmsHandle handle) { DEPENDENCY_CALL(return c.queue->UnregisterDependency(c, handle)); };
#undef DEPENDENCY_CALL
    return api;
}
MmsResult UiRefreshQueue::DependencyCapabilities(RefreshClient& client, MmsUiRefreshDependencyCapabilitiesV1* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(out) || client.queue != this || client.owner.empty() || client.owner == "mms") return MMS_E_ARGUMENT;
    if (client.stopping.load()) return MMS_E_NOT_READY;
    *out = {Header<MmsUiRefreshDependencyCapabilitiesV1>(), UiRefreshDependencies::kPerOwner, UiRefreshDependencies::kMembers, 0, 0};
    return MMS_OK;
}
MmsResult UiRefreshQueue::RegisterDependency(RefreshClient& client, const MmsUiRefreshDependencyDescV1* desc, MmsHandle* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(desc) || !out || !Valid(&desc->upstream) || !Valid(&desc->dependent) ||
        !desc->upstream.element || !desc->upstream.instanceGeneration || !desc->upstream.projectEpoch ||
        !desc->dependent.element || !desc->dependent.instanceGeneration || !desc->dependent.projectEpoch ||
        desc->flags || desc->reserved || client.queue != this || client.owner.empty() || client.owner == "mms") return MMS_E_ARGUMENT;
    if (registry_.InCallback()) return MMS_E_BUSY;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.stopping.load()) return MMS_E_NOT_READY;
    if (ReadOnlyCauseLocked(client)) return MMS_E_BUSY;
    for (const auto& ref : {desc->upstream, desc->dependent}) {
        const auto target = targets_.find(ref.element);
        if (ref.projectEpoch != epoch_ || target == targets_.end() || target->second.generation != ref.instanceGeneration) return MMS_E_STALE;
        const auto& t = target->second;
        if (t.presentation.owner != client.owner) return MMS_E_ARGUMENT;
        // Zero-sized retained layout targets are valid endpoints. No query or
        // registration mounts a surface or bypasses an in-progress layout.
        if (t.suspended || (!MenuTarget(t.presentation) && !t.presentation.window)) return MMS_E_NOT_READY;
        if (!MenuTarget(t.presentation) && !IsWindow(t.presentation.window)) return MMS_E_STALE;
        if (MenuTarget(t.presentation) && !t.Ready()) return MMS_E_NOT_READY;
    }
    MmsHandle candidate = 0;
    if (const auto result = dependencies_->Add(client, desc->upstream, desc->dependent, candidate)) return result;
    InvalidateDependencyTicketsLocked(); *out = candidate; return MMS_OK;
}
MmsResult UiRefreshQueue::UnregisterDependency(RefreshClient& client, MmsHandle handle) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!handle || client.queue != this) return MMS_E_ARGUMENT;
    if (registry_.InCallback()) return MMS_E_BUSY;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ReadOnlyCauseLocked(client)) return MMS_E_BUSY;
    // Cleanup remains valid during stop; Retire may already have removed it.
    if (const auto result = dependencies_->Remove(client, handle)) return result;
    InvalidateDependencyTicketsLocked(); return MMS_OK;
}
void UiRefreshQueue::InvalidateDependencyTicketsLocked() noexcept {
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& ticket = it->second;
        if (ticket.scope == MMS_UI_REFRESH_DEPENDENTS && ticket.state.status == MMS_UI_REFRESH_PENDING &&
            ticket.dependencyRevision != dependencies_->Revision(*ticket.owner)) ticket.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (ticket.released && ticket.state.status != MMS_UI_REFRESH_PENDING && ticket.state.status != MMS_UI_REFRESH_RUNNING)
            it = EraseTicketLocked(it); else ++it;
    }
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
bool UiRefreshQueue::MenuBlockedLocked(const Ticket& ticket) const {
    if (!menuTracking_) return false;
    const auto menu = [&](MmsHandle element) {
        const auto target = targets_.find(element);
        return target != targets_.end() && MenuTarget(target->second.presentation);
    };
    return (ticket.scope != MMS_UI_REFRESH_DEPENDENTS && menu(ticket.target.element)) || std::any_of(ticket.members.begin(), ticket.members.end(), [&](const auto& member) {
        return member.kinds && menu(member.ref.element);
    });
}
MmsResult UiRefreshQueue::Capabilities(RefreshClient& client, MmsUiRefreshCapabilitiesV1* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(out)) return MMS_E_ARGUMENT;
    if (client.stopping.load()) return MMS_E_NOT_READY;
    std::lock_guard<std::mutex> lock(mutex_);
    *out = {Header<MmsUiRefreshCapabilitiesV1>(), uint32_t(kSupported | (layout_ ? MMS_UI_REFRESH_LAYOUT : 0)), MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS,
        std::uint32_t(kTickets), std::uint32_t(kRects), kBytes, std::uint32_t(kBatch), 0};
    return MMS_OK;
}
MmsResult UiRefreshQueue::GetTarget(RefreshClient& client, MmsHandle handle, MmsUiRefreshTargetV1* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(out) || !handle) return MMS_E_ARGUMENT;
    if (client.stopping.load()) return MMS_E_NOT_READY;
    const ElementRecord* element = nullptr;
    for (const auto& entry : registry_.elements) if (entry.second.handle == handle) { element = &entry.second; break; }
    if (!element) return MMS_E_STALE;
    if (String(element->desc.ownerPlugin) != client.owner || client.owner == "mms") return MMS_E_ARGUMENT;
    if (element->desc.kind != MMS_UI_BUTTON && element->desc.kind != MMS_UI_PANEL &&
        element->desc.kind != MMS_UI_GROUP && element->desc.kind != MMS_UI_SEPARATOR && element->desc.kind != MMS_UI_INPUT &&
        element->desc.kind != MMS_UI_MENU && element->desc.kind != MMS_UI_MENU_ITEM) return MMS_E_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(handle); if (found == targets_.end()) return MMS_E_NOT_READY;
    const auto& t = found->second;
    if (t.presentation.owner != client.owner) return MMS_E_STALE;
    const MmsUiTargetRefV1 ref{Header<MmsUiTargetRefV1>(), handle, t.generation, epoch_};
    uint32_t kinds = 0;
    auto handles = topologyReady_ && tree_.count(handle) ? SubtreeLocked(handle) : std::vector<MmsHandle>{handle};
    uint32_t scopes = (t.presentation.container ? 0 : MMS_UI_REFRESH_SELF) | MMS_UI_REFRESH_SUBTREE;
    std::vector<MmsUiTargetRefV1> dependents; uint64_t dependencyRevision = 0;
    if (!dependencies_->Capture(client, ref, dependents, dependencyRevision)) {
        scopes |= MMS_UI_REFRESH_DEPENDENTS;
        for (const auto& dependent : dependents)
            if (std::find(handles.begin(), handles.end(), dependent.element) == handles.end()) handles.push_back(dependent.element);
    }
    for (const auto member : handles) {
        const auto current = targets_.find(member);
        if (current == targets_.end() || current->second.presentation.owner != client.owner || current->second.presentation.container) continue;
        const auto& p = current->second.presentation;
        if (MenuTarget(p)) { kinds |= p.stateRefresh ? MMS_UI_REFRESH_STATE : 0; continue; }
        const MmsUiTargetRefV1 memberRef{Header<MmsUiTargetRefV1>(), member, current->second.generation, epoch_};
        kinds |= MMS_UI_REFRESH_REPAINT | (p.stateRefresh ? MMS_UI_REFRESH_STATE : 0) |
            (p.layoutRefresh && layout_ ? MMS_UI_REFRESH_LAYOUT : 0) |
            (bindings_->HasBinding(client, memberRef) || (p.propertyDataRefresh && p.propertyRecord && propertyData_) ? MMS_UI_REFRESH_DATA : 0);
    }
    *out = {Header<MmsUiRefreshTargetV1>(), ref, t.geometry,
        kinds, scopes, std::uint32_t(t.Ready()), std::uint32_t(t.presentation.visible),
        t.presentation.width, t.presentation.height};
    return MMS_OK;
}
MmsResult UiRefreshQueue::Request(RefreshClient& client, const MmsUiRefreshRequestV1* request, MmsHandle* out,
    MmsHandle subscription, MmsHandle propertySubscription, uint64_t propertySequence) {
    if (!Valid(request) || !Valid(&request->target) || !out || !request->target.element ||
        !request->target.instanceGeneration || !request->target.projectEpoch || !request->kinds ||
        request->kinds & ~15u || request->flags || request->reserved) return MMS_E_ARGUMENT;
    if (request->kinds & ~kAllKinds) return MMS_E_UNSUPPORTED;
    if (request->scope != MMS_UI_REFRESH_SELF && request->scope != MMS_UI_REFRESH_SUBTREE && request->scope != MMS_UI_REFRESH_DEPENDENTS)
        return MMS_E_ARGUMENT;
    if (request->kinds & MMS_UI_REFRESH_DATA) {
        if (request->scope == MMS_UI_REFRESH_SELF) {
            if (bool(request->binding) != bool(request->sourceRevision)) return MMS_E_ARGUMENT;
        } else if (request->binding || request->sourceRevision) return MMS_E_ARGUMENT;
    } else if (request->binding || request->sourceRevision) return MMS_E_ARGUMENT;
    const auto& span = request->dirtyRects;
    if (request->scope == MMS_UI_REFRESH_DEPENDENTS && span.count) return MMS_E_UNSUPPORTED;
    if (!(request->kinds & MMS_UI_REFRESH_REPAINT) && span.count) return MMS_E_ARGUMENT;
    if (span.count > kRects) return MMS_E_BUSY;
    if (span.count && (!span.data || span.stride < sizeof(MmsUiDirtyRectV1) || span.recordVersion != 1 ||
        !request->expectedGeometryRevision)) return MMS_E_ARGUMENT;
    // A failed request may temporarily be the last owner of a captured old
    // source version. Construct before Ticket: the queue lock and the local
    // payload must both be gone before capacity notifications can reacquire
    // the queue and binding locks in their normal order.
    struct CapacityGuard { UiRefreshQueue& queue; ~CapacityGuard() { queue.RetrySubscriptions(); } } capacityGuard{*this};
    Ticket ticket; ticket.owner = &client; ticket.target = request->target;
    ticket.subscriptionHost = bindings_.get(); ticket.subscription = subscription; ticket.released = subscription != 0;
    ticket.propertySubscriptionHost = propertyRefresh_.get(); ticket.propertySubscription = propertySubscription;
    ticket.propertySequence = propertySequence; ticket.released |= propertySubscription != 0;
    ticket.capacityRevision = &subscriptionCapacityRevision_;
    ticket.kinds = request->kinds;
    ticket.scope = request->scope;
    ticket.binding = request->binding; ticket.sourceRevision = request->sourceRevision;
    ticket.expectedGeometry = request->expectedGeometryRevision; ticket.key = request->coalesceKey; ticket.whole = span.count == 0;
    ticket.rects.reserve(std::size_t(span.count));
    for (std::uint64_t index = 0; index < span.count; ++index) {
        MmsUiDirtyRectV1 rect{};
        std::memcpy(&rect, static_cast<const unsigned char*>(span.data) + std::size_t(index * span.stride), sizeof(rect));
        for (float value : {rect.x, rect.y, rect.width, rect.height})
            if (!std::isfinite(value) || std::abs(value) > 1048576) return MMS_E_ARGUMENT;
        if (rect.width < 0 || rect.height < 0) return MMS_E_ARGUMENT;
        ticket.rects.push_back(rect);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.stopping.load()) return MMS_E_NOT_READY;
    if ((ticket.kinds & (MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT)) && ReadOnlyCauseLocked(client)) return MMS_E_BUSY;
    if (ticket.target.projectEpoch != epoch_) return MMS_E_STALE;
    const auto target = targets_.find(ticket.target.element);
    if (target == targets_.end() || target->second.generation != ticket.target.instanceGeneration) return MMS_E_STALE;
    if (target->second.presentation.owner != client.owner || client.owner == "mms") return MMS_E_ARGUMENT;
    if (ticket.scope != MMS_UI_REFRESH_DEPENDENTS && MenuTarget(target->second.presentation) &&
        (ticket.kinds != MMS_UI_REFRESH_STATE || ticket.expectedGeometry || !ticket.rects.empty())) return MMS_E_UNSUPPORTED;
    if (ticket.scope == MMS_UI_REFRESH_SELF) {
        if (target->second.presentation.container) return MMS_E_UNSUPPORTED;
        if ((ticket.kinds & MMS_UI_REFRESH_STATE) && !target->second.presentation.stateRefresh) return MMS_E_UNSUPPORTED;
        if (ticket.kinds & MMS_UI_REFRESH_STATE) ticket.stateProviderRevision = target->second.stateProviderRevision;
        if (ticket.kinds & MMS_UI_REFRESH_STATE) ticket.propertyRecord = target->second.presentation.propertyRecord;
        if ((ticket.kinds & MMS_UI_REFRESH_LAYOUT) && (!layout_ || !target->second.presentation.layoutRefresh)) return MMS_E_UNSUPPORTED;
        // Preserve SELF's original error precedence: an unsupported phase is
        // rejected before inspecting another target's geometry or readiness.
        if (ticket.expectedGeometry && ticket.expectedGeometry != target->second.geometry) return MMS_E_STALE;
        if (!target->second.ReadyFor(ticket.kinds, true)) return MMS_E_NOT_READY;
        if (ticket.kinds & MMS_UI_REFRESH_DATA) {
            if (!ticket.binding) {
                if (!target->second.presentation.propertyDataRefresh || !target->second.presentation.propertyRecord || !propertyData_) return MMS_E_ARGUMENT;
                if (bindings_->HasBinding(client, ticket.target)) return MMS_E_CONFLICT;
                ticket.propertyData = true;
                ticket.propertyRecord = target->second.presentation.propertyRecord;
            } else if (auto result = bindings_->Capture(client, ticket.binding, ticket.target, ticket.sourceRevision, ticket.data)) return result;
        }
    } else {
        if (ticket.expectedGeometry && ticket.expectedGeometry != target->second.geometry) return MMS_E_STALE;
        if (target->second.suspended || (!MenuTarget(target->second.presentation) && !target->second.presentation.window)) return MMS_E_NOT_READY;
        if (!topologyReady_) return MMS_E_NOT_READY;
        std::vector<MmsHandle> handles;
        std::vector<MmsUiTargetRefV1> dependentRefs;
        if (ticket.scope == MMS_UI_REFRESH_DEPENDENTS) {
            if (const auto result = dependencies_->Capture(client, ticket.target, dependentRefs, ticket.dependencyRevision)) return result;
            handles.reserve(dependentRefs.size());
            for (const auto& ref : dependentRefs) handles.push_back(ref.element);
        } else handles = SubtreeLocked(ticket.target.element);
        if (handles.size() > kSubtreeMembers) return MMS_E_BUSY;
        if (ticket.scope == MMS_UI_REFRESH_SUBTREE) ticket.topology = topologyRevision_;
        uint32_t applicable = 0;
        ticket.members.reserve(handles.size()); ticket.dataTargets.reserve(handles.size());
        for (const auto handle : handles) {
            const auto node = tree_.find(handle);
            if (node == tree_.end()) return MMS_E_STALE;
            if (node->second.owner != client.owner) return MMS_E_ARGUMENT;
            const auto kind = node->second.kind;
            if (kind != MMS_UI_BUTTON && kind != MMS_UI_PANEL && kind != MMS_UI_SEPARATOR && kind != MMS_UI_INPUT && kind != MMS_UI_MENU && kind != MMS_UI_MENU_ITEM &&
                !(ticket.scope == MMS_UI_REFRESH_DEPENDENTS && kind == MMS_UI_GROUP)) continue;
            const bool menu = kind == MMS_UI_MENU || kind == MMS_UI_MENU_ITEM;
            const auto current = targets_.find(handle);
            const uint32_t potential = menu ? MMS_UI_REFRESH_STATE : MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_LAYOUT |
                ((kind == MMS_UI_BUTTON || kind == MMS_UI_INPUT || kind == MMS_UI_PANEL) ? MMS_UI_REFRESH_STATE : 0) | (kind == MMS_UI_INPUT ? MMS_UI_REFRESH_DATA : 0);
            if (current == targets_.end()) {
                if (ticket.kinds & potential) return MMS_E_NOT_READY;
                continue; // An unmounted target cannot have a valid DATA binding.
            }
            const auto& t = current->second; const auto& p = t.presentation;
            if (p.owner != client.owner || (p.container && ticket.scope != MMS_UI_REFRESH_DEPENDENTS)) return MMS_E_STALE;
            if (!ticket.whole && !SameCoordinateSpace(target->second.presentation, p)) return MMS_E_NOT_READY;
            const MmsUiTargetRefV1 ref{Header<MmsUiTargetRefV1>(), handle, t.generation, epoch_};
            if (ticket.scope == MMS_UI_REFRESH_DEPENDENTS) {
                const auto exact = std::find_if(dependentRefs.begin(), dependentRefs.end(), [&](const auto& value) { return SameRef(value, ref); });
                if (exact == dependentRefs.end()) return MMS_E_STALE;
                if (t.suspended || (!MenuTarget(p) && !p.window)) return MMS_E_NOT_READY;
                if (p.container) { ticket.members.push_back({ref, t.geometry, t.stateProviderRevision, 0}); continue; }
            }
            uint32_t supported = MenuTarget(p) ? MMS_UI_REFRESH_STATE : MMS_UI_REFRESH_REPAINT | (p.stateRefresh ? MMS_UI_REFRESH_STATE : 0) |
                (p.layoutRefresh && layout_ ? MMS_UI_REFRESH_LAYOUT : 0);
            const uint32_t kinds = supported & ticket.kinds;
            // LAYOUT may restore a zero-sized member during the same request.
            if (kinds && !t.ReadyFor(kinds)) return MMS_E_NOT_READY;
            ticket.members.push_back({ref, t.geometry, t.stateProviderRevision, kinds});
            if (kinds & MMS_UI_REFRESH_STATE) ticket.members.back().propertyRecord = p.propertyRecord;
            if ((ticket.kinds & MMS_UI_REFRESH_DATA) && p.propertyDataRefresh && p.propertyRecord && propertyData_) {
                ticket.members.back().propertyData = true;
                ticket.members.back().propertyRecord = p.propertyRecord;
            }
            ticket.dataTargets.push_back(ref); applicable |= kinds;
        }
        if (ticket.kinds & MMS_UI_REFRESH_DATA) {
            const bool nativeData = std::any_of(ticket.members.begin(), ticket.members.end(), [](const auto& member) { return member.propertyData; });
            if (const auto result = bindings_->CaptureSubtree(client, ticket.dataTargets, ticket.captured))
                if (result != MMS_E_UNSUPPORTED || !nativeData) return result;
            for (const auto& captured : ticket.captured)
                for (auto& member : ticket.members) if (SameRef(member.ref, captured.target)) {
                    member.kinds |= MMS_UI_REFRESH_DATA; member.propertyData = false;
                    if (!(member.kinds & MMS_UI_REFRESH_STATE)) member.propertyRecord.reset();
                }
            // The explicit bindings form one atomic cohort. Native properties
            // are its unbound complement, never a separate HasBinding scan.
            for (auto& member : ticket.members) if (member.propertyData) member.kinds |= MMS_UI_REFRESH_DATA;
            applicable |= MMS_UI_REFRESH_DATA;
        }
        for (const auto& member : ticket.members) if (member.kinds && !targets_.at(member.ref.element).ReadyFor(member.kinds)) return MMS_E_NOT_READY;
        if ((applicable & ticket.kinds) != ticket.kinds) return MMS_E_UNSUPPORTED;
    }
    std::size_t count = 0, bytes = 0, globalBytes = 0; bool pending = false;
    for (const auto& item : tickets_) {
        globalBytes += item.second.Bytes();
        if (item.second.owner == &client) { ++count; bytes += item.second.Bytes(); }
        pending |= item.second.state.status == MMS_UI_REFRESH_PENDING && !MenuBlockedLocked(item.second);
    }
    if (count >= kTickets || tickets_.size() >= kGlobalTickets || ticket.Bytes() > kBytes - bytes ||
        ticket.Bytes() > kGlobalBytes - globalBytes || next_ == UINT64_MAX) return MMS_E_BUSY;
    const bool blocked = MenuBlockedLocked(ticket);
    const auto handle = next_++;
    ticket.handle = handle;
    tickets_.emplace(handle, std::move(ticket));
    // This internal wake is a non-reentrant PostMessage signal, never a plugin
    // callback. Holding the lock prevents UI consumption before failure rollback.
    bool signalled = true;
    if (!pending && wake_ && !blocked) {
        try { signalled = wake_(); } catch (...) { signalled = false; }
        propertyRefresh_->HistoryWakeResult(signalled);
    }
    if (!signalled) { tickets_.erase(handle); return MMS_E_FAILED; }
    *out = handle; return MMS_OK;
}
MmsResult UiRefreshQueue::Query(RefreshClient& client, MmsHandle handle, MmsUiRefreshStateV1* out) {
    if (!Valid(out)) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_); const auto found = tickets_.find(handle);
    if (found == tickets_.end() || found->second.owner != &client || found->second.released) return MMS_E_NOT_FOUND;
    *out = found->second.state; return MMS_OK;
}
MmsResult UiRefreshQueue::Cancel(RefreshClient& client, MmsHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_); const auto found = tickets_.find(handle);
    if (found == tickets_.end() || found->second.owner != &client || found->second.released) return MMS_E_NOT_FOUND;
    if (found->second.state.status == MMS_UI_REFRESH_RUNNING) return MMS_E_BUSY;
    if (found->second.state.status == MMS_UI_REFRESH_PENDING) found->second.Finish(MMS_UI_REFRESH_CANCELLED, MMS_E_CANCELLED);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
    return MMS_OK;
}
MmsResult UiRefreshQueue::Release(RefreshClient& client, MmsHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_); const auto found = tickets_.find(handle);
    if (found == tickets_.end() || found->second.owner != &client || found->second.released) return MMS_E_NOT_FOUND;
    if (found->second.state.status == MMS_UI_REFRESH_PENDING || found->second.state.status == MMS_UI_REFRESH_RUNNING)
        found->second.released = true;
    else {
        EraseTicketLocked(found);
        if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
    }
    return MMS_OK;
}
std::vector<MmsHandle> UiRefreshQueue::SubtreeLocked(MmsHandle root) const {
    std::vector<MmsHandle> result;
    for (const auto& entry : tree_) {
        auto current = entry.first;
        for (size_t depth = 0; current && depth <= tree_.size(); ++depth) {
            if (current == root) { result.push_back(entry.first); break; }
            const auto parent = tree_.find(current);
            if (parent == tree_.end()) break;
            current = parent->second.parent;
        }
    }
    return result;
}
void UiRefreshQueue::SyncTopology() noexcept {
    if (GetCurrentThreadId() != thread_) return;
    try {
        std::map<MmsHandle, TreeNode> candidate;
        for (const auto& [id, element] : registry_.elements) {
            const auto parent = registry_.elements.find(String(element.desc.parentId));
            candidate.emplace(element.handle, TreeNode{parent == registry_.elements.end() ? 0 : parent->second.handle,
                String(element.desc.ownerPlugin), element.desc.kind});
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!topologyReady_ || candidate != tree_) {
            for (const auto& prior : tree_) {
                const auto current = candidate.find(prior.first);
                if (current == candidate.end() || current->second.owner != prior.second.owner || current->second.kind != prior.second.kind)
                    dependencies_->InvalidateTarget(prior.first);
            }
            tree_.swap(candidate); ++topologyRevision_;
            InvalidateDependencyTicketsLocked();
        }
        topologyReady_ = true;
        for (auto it = tickets_.begin(); it != tickets_.end();) {
            auto& t = it->second;
            if (t.scope == MMS_UI_REFRESH_SUBTREE && t.state.status == MMS_UI_REFRESH_PENDING && t.topology != topologyRevision_)
                t.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
            if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING)
                it = EraseTicketLocked(it); else ++it;
        }
        if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_); topologyReady_ = false; ++topologyRevision_;
    }
}
MmsResult UiRefreshQueue::ValidateTicketLocked(const Ticket& ticket, bool geometry) const {
    if (ticket.owner->stopping.load()) return MMS_E_CANCELLED;
    if (ticket.subscription && !bindings_->SubscriptionAlive(*ticket.owner, ticket.subscription)) return MMS_E_CANCELLED;
    if (ticket.propertySubscription && !propertyRefresh_->Alive(*ticket.owner, ticket.propertySubscription)) return MMS_E_CANCELLED;
    const auto root = targets_.find(ticket.target.element);
    if (ticket.target.projectEpoch != epoch_ || root == targets_.end() || root->second.generation != ticket.target.instanceGeneration ||
        (geometry && ticket.expectedGeometry && root->second.geometry != ticket.expectedGeometry)) return MMS_E_STALE;
    if (ticket.scope == MMS_UI_REFRESH_SELF ? !root->second.ReadyFor(ticket.kinds, true) :
        (root->second.suspended || (!MenuTarget(root->second.presentation) && !root->second.presentation.window))) return MMS_E_NOT_READY;
    if (ticket.scope == MMS_UI_REFRESH_SELF) {
        if (ticket.propertyData && (!root->second.presentation.propertyDataRefresh || !propertyData_)) return MMS_E_NOT_READY;
        if (ticket.propertyRecord && root->second.presentation.propertyRecord != ticket.propertyRecord) return MMS_E_STALE;
        if (ticket.propertyData && bindings_->HasBinding(*ticket.owner, ticket.target)) return MMS_E_STALE;
        if ((ticket.kinds & MMS_UI_REFRESH_STATE) && root->second.stateProviderRevision != ticket.stateProviderRevision) return MMS_E_STALE;
        return MMS_OK;
    }
    if (ticket.scope == MMS_UI_REFRESH_DEPENDENTS) {
        if (ticket.dependencyRevision != dependencies_->Revision(*ticket.owner)) return MMS_E_STALE;
    } else if (!topologyReady_ || ticket.topology != topologyRevision_) return MMS_E_STALE;
    for (const auto& member : ticket.members) {
        const auto found = targets_.find(member.ref.element);
        if (found == targets_.end() || found->second.generation != member.ref.instanceGeneration ||
            (geometry && ticket.expectedGeometry && found->second.geometry != member.geometry) ||
            ((member.kinds & MMS_UI_REFRESH_STATE) && found->second.stateProviderRevision != member.stateProviderRevision)) return MMS_E_STALE;
        if (member.kinds && !found->second.ReadyFor(member.kinds)) return MMS_E_NOT_READY;
        if (member.propertyData && (!found->second.presentation.propertyDataRefresh || !propertyData_)) return MMS_E_NOT_READY;
        if (member.propertyRecord && found->second.presentation.propertyRecord != member.propertyRecord) return MMS_E_STALE;
    }
    return ticket.kinds & MMS_UI_REFRESH_DATA ? bindings_->ValidateSubtree(*ticket.owner, ticket.dataTargets, ticket.captured) : MMS_OK;
}
void UiRefreshQueue::SyncTargets(const std::vector<RefreshPresentation>& presentations) {
    SyncTargets(presentations, false);
}
void UiRefreshQueue::SyncMenuTargets(const std::vector<RefreshPresentation>& presentations) {
    SyncTargets(presentations, true);
}
void UiRefreshQueue::SyncTargets(const std::vector<RefreshPresentation>& presentations, bool menus) {
    if (GetCurrentThreadId() != thread_) throw std::invalid_argument("refresh presentation thread");
    if (presentations.size() > 8192) throw std::bad_alloc();
    SyncTopology();
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<MmsHandle, Target> candidate;
    for (const auto& item : targets_) if (MenuTarget(item.second.presentation) != menus) candidate.emplace(item);
    for (const auto& p : presentations) {
        if (MenuTarget(p) != menus || !p.element || !p.instance || p.owner.empty() ||
            (menus ? !p.menu || !p.menuCookie || !p.menuItem || p.window || p.width || p.height || !p.stateRefresh || p.layoutRefresh || p.propertyDataRefresh : !p.window) || p.dpi < 48 || p.dpi > 960 ||
            p.coordinateDpi < 48 || p.coordinateDpi > 960 ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.width) || !std::isfinite(p.height) ||
            p.width < 0 || p.height < 0 || p.width > 1048576 || p.height > 1048576) throw std::invalid_argument("refresh presentation");
        const auto old = targets_.find(p.element);
        Target target; target.presentation = p;
        const bool sameInstance = old != targets_.end() && old->second.presentation.owner == p.owner &&
            old->second.presentation.instance == p.instance && old->second.presentation.window == p.window &&
            old->second.presentation.menu == p.menu && old->second.presentation.menuCookie == p.menuCookie && old->second.presentation.menuItem == p.menuItem;
        if (!nextGeneration_ || !nextGeometry_) throw std::bad_alloc();
        target.generation = sameInstance ? old->second.generation : nextGeneration_++;
        target.stateProviderRevision = sameInstance ? old->second.stateProviderRevision : 0;
        const bool sameGeometry = sameInstance && RectangleGeometry(old->second.presentation).Same(p) && old->second.presentation.visible == p.visible;
        target.geometry = menus ? 0 : sameGeometry ? old->second.geometry : nextGeometry_++;
        if (!candidate.emplace(p.element, std::move(target)).second) throw std::invalid_argument("duplicate refresh target");
    }
    targets_.swap(candidate);
    for (const auto& [handle, old] : candidate) {
        const auto current = targets_.find(handle);
        if (current == targets_.end() || current->second.generation != old.generation) {
            bindings_->InvalidateTarget(handle);
            dependencies_->InvalidateTarget(handle);
            propertyRefresh_->InvalidateTarget(handle);
        }
    }
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& ticket = it->second;
        if (ticket.state.status == MMS_UI_REFRESH_PENDING) {
            const auto check = ValidateTicketLocked(ticket, true);
            if (check) ticket.FinishValidation(check);
        }
        if (ticket.released && ticket.state.status != MMS_UI_REFRESH_PENDING && ticket.state.status != MMS_UI_REFRESH_RUNNING)
            it = EraseTicketLocked(it); else ++it;
    }
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryTargetPublication);
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
    for (const auto& entry : targets_) {
        const auto old = candidate.find(entry.first);
        const bool providerChanged = old != candidate.end() && !old->second.presentation.propertyDataRefresh && entry.second.presentation.propertyDataRefresh;
        propertyRefresh_->TargetPublished(entry.first, entry.second.Ready(), providerChanged);
    }
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
void UiRefreshQueue::InvalidateTarget(MmsHandle element) noexcept {
    std::lock_guard<std::mutex> lock(mutex_); targets_.erase(element);
    bindings_->InvalidateTarget(element);
    dependencies_->InvalidateTarget(element);
    propertyRefresh_->InvalidateTarget(element);
    InvalidateDependencyTicketsLocked();
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& ticket = it->second;
        if (ticket.state.status == MMS_UI_REFRESH_PENDING && (ticket.target.element == element ||
            std::any_of(ticket.members.begin(), ticket.members.end(), [&](const auto& m) { return m.ref.element == element; })))
            ticket.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (ticket.released && ticket.state.status != MMS_UI_REFRESH_PENDING && ticket.state.status != MMS_UI_REFRESH_RUNNING)
            it = EraseTicketLocked(it); else ++it;
    }
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
void UiRefreshQueue::SuspendTargets() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& target : targets_) if (!MenuTarget(target.second.presentation)) target.second.suspended = true;
}
void UiRefreshQueue::SuspendMenuTargets() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& target : targets_) if (MenuTarget(target.second.presentation)) target.second.suspended = true;
}
void UiRefreshQueue::SetMenuTracking(bool tracking) noexcept {
    std::lock_guard<std::mutex> lock(mutex_); menuTracking_ = tracking;
    if (!tracking && wake_) {
        bool pending = false;
        for (const auto& item : tickets_) pending |= item.second.state.status == MMS_UI_REFRESH_PENDING;
        if (pending) {
            bool signalled = false; try { signalled = wake_(); } catch (...) {}
            if (!signalled) {
                for (auto& item : tickets_) if (item.second.state.status == MMS_UI_REFRESH_PENDING)
                    item.second.Finish(MMS_UI_REFRESH_FAILED, MMS_E_FAILED);
                // This is an infrastructure failure, not a successful retry
                // signal for capacity freed while rejecting the menu work.
                bindings_->FailSubscriptionScheduling(MMS_E_FAILED);
                propertyRefresh_->FailScheduling(MMS_E_FAILED);
            }
        }
    }
}
void UiRefreshQueue::Drain() {
    if (GetCurrentThreadId() != thread_ || registry_.InCallback()) return;
    { std::lock_guard<std::mutex> lock(mutex_); if (draining_) return; draining_ = true; }
    struct DrainGuard { UiRefreshQueue& queue; ~DrainGuard() {
        std::lock_guard<std::mutex> lock(queue.mutex_); queue.draining_ = false;
        // groups are destroyed before this guard. Only a retained source or
        // display snapshot actually freed since parking can trigger this path.
        const bool sourceRetry = queue.bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
        if (sourceRetry || queue.propertyRefresh_->Pending()) queue.SignalSubscriptionWakeLocked();
    } } drainGuard{*this};
    PruneInvalidSubscriptionTargets();
    ScheduleSubscriptions();
    SchedulePropertySubscriptions();
    struct Work {
        Target target; RefreshClient* owner = nullptr; MmsUiTargetRefV1 ref{}; uint32_t kinds = 0;
        uint64_t stateProviderRevision = 0; UiRefreshBindings::Contribution data;
        bool propertyData = false;
        std::shared_ptr<const PropertyRecord> propertyRecord;
        std::vector<MmsUiDirtyRectV1> rects; bool whole = true, fixedGeometry = false;
        RectangleGeometry region;
    };
    struct Group {
        Target root; RefreshClient* owner = nullptr; MmsUiTargetRefV1 ref{};
        uint64_t key = 0, topology = 0, dependencyRevision = 0, sourceRevision = 0; MmsHandle binding = 0, subscription = 0;
        MmsHandle propertySubscription = 0; uint64_t propertySequence = 0;
        uint32_t kinds = 0, scope = MMS_UI_REFRESH_SELF; bool fixedGeometry = false;
        std::vector<Work> members; std::vector<MmsHandle> tickets;
        std::vector<MmsUiTargetRefV1> dataTargets; std::vector<UiRefreshBindings::Captured> captured;
        RectangleGeometry region;
        std::vector<MmsUiDirtyRectV1> rects;
        bool whole = false;
    };
    std::vector<Group> groups;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (auto it = tickets_.begin(); it != tickets_.end() && count < kBatch;) {
            auto& ticket = it->second;
            if (ticket.state.status != MMS_UI_REFRESH_PENDING) { ++it; continue; }
            const auto result = ValidateTicketLocked(ticket, true);
            if (result) {
                ticket.FinishValidation(result);
                if (ticket.released) it = EraseTicketLocked(it); else ++it;
                continue;
            }
            if (MenuBlockedLocked(ticket)) { ++it; continue; }
            ++count;
            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) {
                if (g.owner != ticket.owner || !SameRef(g.ref, ticket.target) || g.key != ticket.key ||
                    g.kinds != ticket.kinds || g.scope != ticket.scope || g.topology != ticket.topology || g.dependencyRevision != ticket.dependencyRevision ||
                    g.binding != ticket.binding || g.sourceRevision != ticket.sourceRevision || g.subscription != ticket.subscription ||
                    g.propertySubscription != ticket.propertySubscription || g.propertySequence != ticket.propertySequence ||
                    g.fixedGeometry != (ticket.expectedGeometry != 0)) return false;
                if (g.scope == MMS_UI_REFRESH_SELF)
                    return g.members.front().stateProviderRevision == ticket.stateProviderRevision &&
                        g.members.front().propertyData == ticket.propertyData && g.members.front().propertyRecord == ticket.propertyRecord;
                if (g.members.size() != ticket.members.size() || g.captured.size() != ticket.captured.size()) return false;
                for (size_t i = 0; i < g.members.size(); ++i) {
                    const auto& w = g.members[i]; const auto& m = ticket.members[i];
                    if (!SameRef(w.ref, m.ref) || w.kinds != m.kinds || w.propertyData != m.propertyData || w.propertyRecord != m.propertyRecord || w.stateProviderRevision != m.stateProviderRevision ||
                        (g.fixedGeometry && w.target.geometry != m.geometry)) return false;
                }
                for (size_t i = 0; i < g.captured.size(); ++i)
                    if (g.captured[i].binding != ticket.captured[i].binding ||
                        g.captured[i].revision != ticket.captured[i].revision ||
                        !SameRef(g.captured[i].target, ticket.captured[i].target)) return false;
                return true;
            });
            if (group == groups.end()) {
                Group candidate; candidate.root = targets_.at(ticket.target.element); candidate.owner = ticket.owner;
                candidate.region = RectangleGeometry(candidate.root.presentation);
                candidate.ref = ticket.target; candidate.key = ticket.key; candidate.topology = ticket.topology; candidate.dependencyRevision = ticket.dependencyRevision;
                candidate.sourceRevision = ticket.sourceRevision; candidate.binding = ticket.binding; candidate.subscription = ticket.subscription;
                candidate.propertySubscription = ticket.propertySubscription; candidate.propertySequence = ticket.propertySequence;
                candidate.kinds = ticket.kinds; candidate.scope = ticket.scope; candidate.fixedGeometry = ticket.expectedGeometry != 0;
                candidate.dataTargets = ticket.dataTargets; candidate.captured = ticket.captured;
                if (ticket.scope == MMS_UI_REFRESH_SELF) {
                    Work work; work.target = candidate.root; work.owner = ticket.owner; work.ref = ticket.target;
                    work.kinds = ticket.kinds; work.stateProviderRevision = ticket.stateProviderRevision; work.data = ticket.data;
                    work.propertyData = ticket.propertyData;
                    work.propertyRecord = ticket.propertyRecord;
                    work.whole = false; work.fixedGeometry = candidate.fixedGeometry;
                    candidate.members.push_back(std::move(work));
                } else for (const auto& member : ticket.members) {
                    Work work; work.target = targets_.at(member.ref.element); work.owner = ticket.owner; work.ref = member.ref;
                    work.kinds = member.kinds; work.stateProviderRevision = member.stateProviderRevision; work.fixedGeometry = candidate.fixedGeometry;
                    work.propertyData = member.propertyData;
                    work.propertyRecord = member.propertyRecord;
                    work.region = RectangleGeometry(work.target.presentation);
                    for (const auto& data : ticket.captured) if (SameRef(data.target, member.ref)) work.data = data.data;
                    candidate.members.push_back(std::move(work));
                }
                groups.push_back(std::move(candidate)); group = std::prev(groups.end());
            }
            group->tickets.push_back(it->first);
            if (ticket.scope == MMS_UI_REFRESH_SELF) {
                auto& work = group->members.front(); work.whole |= ticket.whole;
                work.rects.insert(work.rects.end(), ticket.rects.begin(), ticket.rects.end());
            } else {
                group->whole |= ticket.whole;
                group->rects.insert(group->rects.end(), ticket.rects.begin(), ticket.rects.end());
            }
            ++it;
        }
        for (const auto& group : groups) for (auto handle : group.tickets) tickets_.at(handle).state.status = MMS_UI_REFRESH_RUNNING;
    }
    // A group is a versioned contribution, not a second object registry. Every
    // stage completes across all its original handles before the next begins.
    for (auto& batch : groups) {
        uint32_t completed = 0, subscriptionWait = 0;
        uint32_t propertyWait = MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER;
        uint64_t consumedRevision = 0, subscriptionCapacityRevision = UINT64_MAX;
        const auto validateAll = [&](bool strict) -> MmsResult {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto targetNotReady = [&]() -> MmsResult {
                propertyWait = MMS_UI_PROPERTY_REFRESH_WAIT_TARGET;
                return MMS_E_NOT_READY;
            };
            if (batch.owner->stopping.load()) return MMS_E_CANCELLED;
            if (batch.subscription && !bindings_->SubscriptionAlive(*batch.owner, batch.subscription)) return MMS_E_CANCELLED;
            if (batch.propertySubscription && !propertyRefresh_->Alive(*batch.owner, batch.propertySubscription)) return MMS_E_CANCELLED;
            const auto root = targets_.find(batch.ref.element);
            if (batch.ref.projectEpoch != epoch_ || root == targets_.end() || root->second.generation != batch.ref.instanceGeneration)
                return MMS_E_STALE;
            if (batch.scope != MMS_UI_REFRESH_SELF) {
                if (batch.scope == MMS_UI_REFRESH_DEPENDENTS ? batch.dependencyRevision != dependencies_->Revision(*batch.owner) :
                    (!topologyReady_ || batch.topology != topologyRevision_)) return MMS_E_STALE;
                if (strict && root->second.geometry != batch.root.geometry) return MMS_E_STALE;
                if (root->second.suspended || (!MenuTarget(root->second.presentation) && !root->second.presentation.window)) return targetNotReady();
                if (!MenuTarget(root->second.presentation) && !IsWindow(root->second.presentation.window)) return MMS_E_STALE;
                if (batch.kinds & MMS_UI_REFRESH_DATA)
                    if (const auto result = bindings_->ValidateSubtree(*batch.owner, batch.dataTargets, batch.captured)) return result;
            }
            for (auto& work : batch.members) {
                const auto target = targets_.find(work.ref.element);
                if (target == targets_.end() || target->second.generation != work.ref.instanceGeneration ||
                    (strict && target->second.geometry != work.target.geometry)) return MMS_E_STALE;
                if (work.kinds && !target->second.ReadyFor(work.kinds, batch.scope == MMS_UI_REFRESH_SELF)) return targetNotReady();
                if ((work.kinds || batch.scope == MMS_UI_REFRESH_DEPENDENTS) && !MenuTarget(target->second.presentation) &&
                    !IsWindow(target->second.presentation.window)) return MMS_E_STALE;
                if (work.propertyData && (!target->second.presentation.propertyDataRefresh || !propertyData_)) return targetNotReady();
                if (work.propertyRecord && target->second.presentation.propertyRecord != work.propertyRecord) return MMS_E_STALE;
                if ((work.propertyData || work.propertyRecord) &&
                    !registry_.IsPropertyInputTargetCurrent(work.ref.element, work.propertyRecord.get())) return MMS_E_STALE;
                if (work.propertyData && bindings_->HasBinding(*work.owner, work.ref)) return MMS_E_STALE;
                if ((work.kinds & MMS_UI_REFRESH_STATE) && !target->second.presentation.stateRefresh) return MMS_E_UNSUPPORTED;
                if ((work.kinds & MMS_UI_REFRESH_STATE) && target->second.stateProviderRevision != work.stateProviderRevision) return MMS_E_STALE;
                if (!strict) work.target = target->second;
            }
            if (!strict) batch.root = root->second;
            return MMS_OK;
        };
        const auto applyTarget = [&](Work& group, uint32_t phase) -> MmsResult {
            const auto validate = [&](bool strict) { return validateAll(strict); };
            if (const auto r = validate(group.fixedGeometry)) return r;
            if (phase == MMS_UI_REFRESH_DATA) {
                { std::lock_guard<std::mutex> lock(mutex_); evaluatingDataOwner_ = group.owner; }
                struct DataGuard { UiRefreshQueue& queue; ~DataGuard() {
                    std::lock_guard<std::mutex> lock(queue.mutex_); queue.evaluatingDataOwner_ = nullptr;
                } } guard{*this};
                bool displayChanged = false;
                const auto result = group.propertyData ? registry_.WithReadOnlyCallback([&]() -> MmsResult {
                    if (!propertyData_) return MMS_E_NOT_READY;
                    uint64_t currentRevision = 0;
                    const auto refreshed = propertyData_(group.ref.element, [&] { return validate(group.fixedGeometry); }, currentRevision);
                    // This mode reads current data at consumption. It does not
                    // claim to have consumed a versioned binding snapshot.
                    if (!refreshed) consumedRevision = 0;
                    return refreshed;
                }) : bindings_->Consume(group.data, [&] { return validate(group.fixedGeometry); }, consumedRevision, displayChanged,
                    &subscriptionWait, &subscriptionCapacityRevision);
                if (result) return result;
            }
            if (phase == MMS_UI_REFRESH_STATE) {
                // An element may reference another plugin's public command.
                // Protect both the requesting owner and the actual provider;
                // unrelated plugins keep their independent background work.
                const UiClient* provider = nullptr;
                for (const auto& item : registry_.elements) if (item.second.handle == group.ref.element) {
                    const auto command = registry_.commands.find(String(item.second.desc.commandId));
                    if (command != registry_.commands.end()) provider = command->second->owner;
                    else if (group.propertyRecord) provider = group.propertyRecord->owner;
                    break;
                }
                { std::lock_guard<std::mutex> lock(mutex_);
                    evaluatingStateOwner_ = group.owner; evaluatingStateProvider_ = provider; }
                struct StateGuard { UiRefreshQueue& queue; ~StateGuard() {
                    std::lock_guard<std::mutex> lock(queue.mutex_);
                    queue.evaluatingStateOwner_ = nullptr; queue.evaluatingStateProvider_ = nullptr;
                } } guard{*this};
                // A provider may disable its own HWND while answering getState.
                // Remember ownership before that callback: USER32 may clear focus
                // as soon as the HWND becomes disabled.
                const HWND focusedBeforeState = GetFocus();
                const auto r = registry_.WithElementStateContent(group.ref, group.stateProviderRevision,
                    [&](const MmsCommandStateV1& state, const std::shared_ptr<const ButtonStateContent>& content) -> MmsResult {
                    if (const auto check = validate(true)) return check;
                    if (MenuTarget(group.target.presentation)) {
                        if (!stateContent_) return MMS_E_NOT_READY;
                        if (const auto result = stateContent_(group.ref.element, content, state.enabled != 0, [&] { return validate(true); })) return result;
                        if (const auto result = validate(true)) return result;
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto& target = targets_.at(group.ref.element);
                        target.presentation.visible = target.presentation.stateVisible = state.visible != 0;
                        group.target.presentation.visible = group.target.presentation.stateVisible = state.visible != 0;
                        if (group.ref.element == batch.ref.element)
                            batch.root.presentation.visible = batch.root.presentation.stateVisible = state.visible != 0;
                        return MMS_OK;
                    }
                    const bool visibilityChanged = bool(state.visible) != group.target.presentation.stateVisible;
                    // Only an existing reserved slot can change visibility without
                    // measurement, sibling reflow or viewport changes.
                    if (visibilityChanged && !group.target.presentation.stateVisibility &&
                        !(group.kinds & MMS_UI_REFRESH_LAYOUT)) return MMS_E_UNSUPPORTED;
                    if (visibilityChanged) if (const auto check = registry_.CheckVisibilityPublication(group.ref.element)) return check;
                    HWND window = group.target.presentation.window;
                    if (!IsWindow(window)) return MMS_E_STALE;
                    const bool input = content && content->input;
                    const bool panel = group.target.presentation.kind == MMS_UI_PANEL;
                    // Cancel/revoke input intent before any focus, enable or
                    // visibility message can produce EN_KILLFOCUS submission.
                    if (input) {
                        if (!stateContent_) return MMS_E_NOT_READY;
                        if (const auto result = stateContent_(group.ref.element, content, state.enabled != 0, [&] { return validate(true); })) return result;
                        if (const auto result = validate(true)) return result;
                    }
                    const auto contains = [&](HWND focus) { return focus && (focus == window || ((input || panel) && IsChild(window, focus))); };
                    const bool losingAvailability = !state.enabled || !(state.visible && group.target.presentation.layoutVisible);
                    const auto relocateFocus = [&]() -> MmsResult {
                        const HWND focus = GetFocus();
                        // Keep an explicit new focus chosen by a provider/native
                        // callback, but recover focus that USER32 cleared itself.
                        if ((!contains(focus) && (focus || !contains(focusedBeforeState))) ||
                            (!losingAvailability && IsWindowEnabled(window) && IsWindowVisible(window))) return MMS_OK;
                        const HWND parent = GetParent(window);
                        const auto eligible = [&](HWND candidate) {
                            return candidate && !contains(candidate) && IsWindow(candidate) &&
                                IsWindowVisible(candidate) && IsWindowEnabled(candidate) &&
                                (GetWindowLongPtrW(candidate, GWL_STYLE) & WS_TABSTOP);
                        };
                        HWND next = parent ? GetNextDlgTabItem(parent, window, FALSE) : nullptr;
                        if (!eligible(next)) {
                            // Custom control-parent containers are not dialogs.
                            // Fall back to their actual sibling tab order if the
                            // dialog helper returns the container or no tab stop.
                            const HWND first = parent ? GetWindow(parent, GW_CHILD) : nullptr;
                            HWND child = GetWindow(window, GW_HWNDNEXT);
                            if (!child) child = first;
                            for (unsigned visited = 0; child && child != window && visited < 8192; ++visited) {
                                if (eligible(child)) { next = child; break; }
                                child = GetWindow(child, GW_HWNDNEXT); if (!child) child = first;
                            }
                        }
                        if (!eligible(next)) {
                            next = parent;
                            while (next && (!IsWindowVisible(next) || !IsWindowEnabled(next))) next = GetParent(next);
                        }
                        if (const auto check = validate(true)) return check;
                        if (!IsWindow(window)) return MMS_E_STALE;
                        if (contains(GetFocus()) || (!GetFocus() && contains(focusedBeforeState))) SetFocus(next);
                        if (const auto check = validate(true)) return check;
                        if (!IsWindow(window)) return MMS_E_STALE;
                        return contains(GetFocus()) || (next && !GetFocus()) ? MMS_E_FAILED : MMS_OK;
                    };
                    // Choose a valid keyboard target while the old HWND is still
                    // enabled. EnableWindow/ShowWindow are allowed to clear its
                    // focus; their callbacks may subsequently choose another one.
                    if (losingAvailability) if (const auto check = relocateFocus()) return check;
                    if (panel) {
                        if (!stateContent_) return MMS_E_NOT_READY;
                        if (const auto r = stateContent_(group.ref.element, content, state.enabled != 0, [&] { return validate(true); })) return r;
                        if (const auto r = validate(true)) return r;
                    }
                    if (!panel && bool(IsWindowEnabled(window)) != bool(state.enabled)) EnableWindow(window, state.enabled);
                    if (const auto check = validate(true)) return check;
                    if (!IsWindow(window)) return MMS_E_STALE;
                    const auto checked = state.checked ? BST_CHECKED : BST_UNCHECKED;
                    if (!input && !panel && SendMessageW(window, BM_GETCHECK, 0, 0) != checked) SendMessageW(window, BM_SETCHECK, checked, 0);
                    if (const auto check = validate(true)) return check;
                    if (!IsWindow(window)) return MMS_E_STALE;
                    if (visibilityChanged) {
                        const bool shown = state.visible && group.target.presentation.layoutVisible;
                        if (!panel && bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != shown) ShowWindow(window, shown ? SW_SHOWNA : SW_HIDE);
                        const auto focusResult = relocateFocus();
                        if (const auto check = focusResult ? focusResult : validate(true)) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            auto target = targets_.find(group.ref.element);
                            if (target != targets_.end() && target->second.generation == group.ref.instanceGeneration) target->second.suspended = true;
                            return check;
                        }
                        if (!IsWindow(window)) return MMS_E_STALE;
                        if (!panel && bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != shown) return MMS_E_FAILED;
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto& target = targets_.at(group.ref.element);
                        const auto result = registry_.PublishVisibility(group.ref.element, shown);
                        if (result) { target.suspended = true; return result; }
                        if (target.presentation.visible != shown) target.geometry = nextGeometry_++;
                        target.presentation.visible = shown; target.presentation.stateVisible = state.visible != 0;
                        group.target.geometry = target.geometry;
                        group.target.presentation.visible = shown; group.target.presentation.stateVisible = state.visible != 0;
                        if (group.ref.element == batch.ref.element) batch.root = target;
                    }
                    if (const auto check = relocateFocus()) return check;
                    if (!input && !panel && stateContent_) {
                        const auto contentResult = stateContent_(group.ref.element, content, state.enabled != 0, [&] { return validate(true); });
                        if (contentResult) return contentResult;
                        if (const auto check = validate(true)) return check;
                    }
                    const bool applied = bool(IsWindowEnabled(window)) == bool(state.enabled) &&
                        (input || panel || SendMessageW(window, BM_GETCHECK, 0, 0) == checked);
                    if (const auto check = validate(true)) return check;
                    return IsWindow(window) ? (applied ? MMS_OK : MMS_E_FAILED) : MMS_E_STALE;
                });
                if (r) return r;
                if (const auto check = validate(true)) return check;
            }
            if (phase == MMS_UI_REFRESH_REPAINT) {
                if (const auto r = validate(group.fixedGeometry || (group.kinds & MMS_UI_REFRESH_STATE))) return r;
                const auto r = batch.scope == MMS_UI_REFRESH_SUBTREE ?
                    Invalidate(group.target.presentation, batch.whole, batch.rects, &batch.root.presentation) :
                    Invalidate(group.target.presentation, group.whole, group.rects);
                return r;
            }
            return MMS_OK;
        };
        const auto result = Boundary([&]() -> MmsResult {
            if (const auto check = validateAll(batch.fixedGeometry)) return check;
            for (const uint32_t phase : {MMS_UI_REFRESH_DATA, MMS_UI_REFRESH_STATE, MMS_UI_REFRESH_LAYOUT, MMS_UI_REFRESH_REPAINT}) {
                if (!(batch.kinds & phase)) continue;
                if (phase == MMS_UI_REFRESH_REPAINT && batch.scope == MMS_UI_REFRESH_SUBTREE && !batch.whole) {
                    if (const auto check = validateAll(batch.fixedGeometry)) return check;
                    // Successful preceding phases may publish new geometry. A
                    // root-local region still belongs to its captured mapping;
                    // check every member before invalidating even the first.
                    if (!batch.region.Same(batch.root.presentation)) return MMS_E_STALE;
                    for (const auto& work : batch.members) if ((work.kinds & MMS_UI_REFRESH_REPAINT) &&
                        !work.region.Same(work.target.presentation)) return MMS_E_STALE;
                }
                if (phase == MMS_UI_REFRESH_LAYOUT) {
                    if (const auto check = validateAll(batch.fixedGeometry)) return check;
                    { std::lock_guard<std::mutex> lock(mutex_); evaluatingLayoutOwner_ = batch.owner; }
                    struct LayoutGuard { UiRefreshQueue& queue; ~LayoutGuard() {
                        std::lock_guard<std::mutex> lock(queue.mutex_); queue.evaluatingLayoutOwner_ = nullptr;
                    } } guard{*this};
                    // One retained-tree pass resolves sibling dependencies too.
                    const auto anchor = batch.scope == MMS_UI_REFRESH_DEPENDENTS ?
                        std::find_if(batch.members.begin(), batch.members.end(), [](const auto& work) { return (work.kinds & MMS_UI_REFRESH_LAYOUT) != 0; }) : batch.members.end();
                    const auto element = anchor != batch.members.end() ? anchor->ref.element : batch.ref.element;
                    const auto r = registry_.WithReadOnlyCallback([&] { return layout_ ? layout_(element) : MMS_E_UNSUPPORTED; });
                    if (r) return r;
                    const auto priorGeometry = batch.members.empty() ? batch.root.geometry : batch.members.front().target.geometry;
                    if (const auto check = validateAll(false)) return check;
                    completed |= phase;
                    if (batch.scope == MMS_UI_REFRESH_SELF && !batch.members.front().rects.empty() &&
                        priorGeometry != batch.members.front().target.geometry) return MMS_E_STALE;
                } else {
                    for (auto& work : batch.members) if (work.kinds & phase) {
                        if (const auto r = applyTarget(work, phase)) return r;
                        if (const auto check = validateAll(work.fixedGeometry)) return check;
                    }
                    completed |= phase;
                }
            }
            return MMS_OK;
        });
        std::lock_guard<std::mutex> lock(mutex_);
        const auto root = targets_.find(batch.ref.element);
        const auto geometry = root != targets_.end() && root->second.generation == batch.ref.instanceGeneration ? root->second.geometry : 0;
        for (auto handle : batch.tickets) {
            auto found = tickets_.find(handle); if (found == tickets_.end()) continue;
            found->second.Finish(result == MMS_E_CANCELLED ? MMS_UI_REFRESH_CANCELLED : result ? MMS_UI_REFRESH_FAILED : MMS_UI_REFRESH_COMPLETED,
                result, geometry, completed, subscriptionWait, subscriptionCapacityRevision, propertyWait);
            found->second.state.sourceRevision = batch.scope == MMS_UI_REFRESH_SELF ? consumedRevision : 0;
            if (found->second.released) EraseTicketLocked(found);
        }
    }
}
bool UiRefreshQueue::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // The manager uses this to arm its retry timer. Accepted menu work remains
    // pending in its ticket, but must not poll a native modal menu loop.
    return PendingSubscriptionsLocked() || std::any_of(tickets_.begin(), tickets_.end(), [&](const auto& t) {
        if (t.second.state.status != MMS_UI_REFRESH_PENDING) return false;
        return !MenuBlockedLocked(t.second);
    });
}
bool UiRefreshQueue::ReadOnlyCause() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evaluatingStateOwner_ || evaluatingStateProvider_ || evaluatingDataOwner_ ||
        evaluatingLayoutOwner_ || evaluatingLayoutProvider_ || inheritedReadOnlyCause_;
}
bool UiRefreshQueue::ReadOnlyCauseLocked(const RefreshClient& client) const noexcept {
    if (client.queue != this) return false;
    const auto sameOwner = [&](const RefreshClient* cause) { return cause && cause->owner == client.owner; };
    return sameOwner(evaluatingStateOwner_) || sameOwner(evaluatingDataOwner_) || sameOwner(evaluatingLayoutOwner_) ||
        sameOwner(inheritedReadOnlyCause_) || (evaluatingStateProvider_ && evaluatingStateProvider_->id == client.owner) ||
        (evaluatingLayoutProvider_ && evaluatingLayoutProvider_->id == client.owner);
}
bool UiRefreshQueue::ReadOnlyCause(const RefreshClient& client) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadOnlyCauseLocked(client);
}
const RefreshClient* UiRefreshQueue::ExchangeInheritedReadOnlyCause(const RefreshClient* value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto previous = inheritedReadOnlyCause_;
    inheritedReadOnlyCause_ = value;
    return previous;
}
void UiRefreshQueue::Retire(RefreshClient& client) noexcept {
    client.stopping.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    bindings_->Retire(client);
    dependencies_->Retire(client);
    propertyRefresh_->Retire(client);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.owner == &client && t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_CANCELLED, MMS_E_CANCELLED);
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING) it = EraseTicketLocked(it); else ++it;
    }
    bindings_->RetrySubscriptions(UiRefreshBindings::RetryDisplayCapacity);
    if (PendingSubscriptionsLocked()) SignalSubscriptionWakeLocked();
}
bool UiRefreshQueue::HasLeases(const RefreshClient& client) const {
    if (client.inFlight.load()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    return client.inFlight.load() || bindings_->HasLeases(client) ||
        std::any_of(tickets_.begin(), tickets_.end(), [&](const auto& t) { return t.second.owner == &client; });
}
void UiRefreshQueue::FailPending(MmsResult result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_FAILED, result);
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING) it = EraseTicketLocked(it); else ++it;
    }
    bindings_->FailSubscriptionScheduling(result);
    propertyRefresh_->FailScheduling(result);
}
void UiRefreshQueue::AdvanceProjectEpoch() noexcept {
    std::lock_guard<std::mutex> lock(mutex_); ++epoch_;
    bindings_->AdvanceProjectEpoch(epoch_);
    dependencies_->Clear();
    propertyRefresh_->AdvanceProjectEpoch(epoch_);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING) it = EraseTicketLocked(it); else ++it;
    }
}
}
