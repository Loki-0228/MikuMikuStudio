#pragma once
#include "ui_refresh.hpp"
#include <mms/refresh_binding.h>
#include <mms/refresh_subscription.h>

namespace mikumikustudio::plugins {
class UiRefreshBindings {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    struct Hooks {
        std::function<MmsResult(RefreshClient&, const MmsUiTargetRefV1&)> validateTarget;
        std::function<MmsResult(const std::function<MmsResult()>&)> readOnly;
        std::function<bool()> inCallback;
        std::function<MmsResult(RefreshClient&, const MmsUiTargetRefV1&, uint32_t)> subscriptionTarget;
        std::function<void()> wakeSubscriptions;
        std::function<void()> cleanupSubscriptions;
        std::function<void()> capacityReleased;
        // UI binding mutations call this only after releasing the store lock.
        std::function<void()> bindingChanged;
        std::function<MmsResult(RefreshClient&, const MmsUiTargetRefV1&, uint32_t, uint32_t)> scopedSubscriptionTarget;
    };
    struct DataContribution;
    using Contribution = std::shared_ptr<DataContribution>;
    struct Captured {
        MmsUiTargetRefV1 target{};
        MmsHandle binding = 0;
        uint64_t revision = 0;
        Contribution data;
    };
    explicit UiRefreshBindings(Hooks = {});
    ~UiRefreshBindings();
    UiRefreshBindings(const UiRefreshBindings&) = delete;
    UiRefreshBindings& operator=(const UiRefreshBindings&) = delete;
    void SetHooks(Hooks);
    MmsUiRefreshBindingApiV1 Api(RefreshClient&);
    MmsUiRefreshSubscriptionApiV1 SubscriptionApi(RefreshClient&);
    MmsUiRefreshSubscriptionApiV1_1 ScopedSubscriptionApi(RefreshClient&);
    using SubscriptionWork = UiRefreshSubscriptionWork;
    enum SubscriptionRetryEvent : uint32_t {
        RetryQueueCapacity = 1, RetryTargetPublication = 2,
        RetryDisplayCapacity = 4, RetryConsumerPublication = 8, RetryHostWake = 16
    };
    // Queue -> binding only. These operations never call hooks or allocate.
    bool TakeSubscription(MmsHandle after, SubscriptionWork&) noexcept;
    bool SubmitSubscription(const SubscriptionWork&, MmsHandle ticket, MmsResult) noexcept;
    void CompleteSubscription(MmsHandle, MmsHandle ticket, uint64_t revision, MmsResult, uint32_t waitEvents = 0,
        uint64_t capacityRevision = UINT64_MAX, uint64_t workGeneration = 0) noexcept;
    bool SubscriptionAlive(const RefreshClient&, MmsHandle, uint64_t workGeneration = 0) const noexcept;
    bool PendingSubscriptions() const noexcept;
    bool PendingScopedSubscriptions() const noexcept;
    bool NextScopedSubscription(MmsHandle after, SubscriptionWork&) const noexcept;
    void ObserveScopedSubscription(const SubscriptionWork&, const std::vector<MmsUiTargetRefV1>&, uint64_t dependencyRevision,
        uint32_t blocked) noexcept;
    void ScopedGraphChanged(const RefreshClient&, const MmsUiTargetRefV1& upstream) noexcept;
    void ScopedTargetPublished(MmsHandle element, bool providerChanged) noexcept;
    bool RetrySubscriptions(uint32_t events = RetryQueueCapacity, uint64_t queueCapacityRevision = UINT64_MAX) noexcept;
    void FailSubscriptionScheduling(MmsResult) noexcept;
    // queue -> binding is the only nested lock order. Capture/HasBinding do
    // not call hooks or touch HWND/registry, and are safe under the queue lock.
    MmsResult Capture(RefreshClient&, MmsHandle binding, const MmsUiTargetRefV1&,
        uint64_t requestedRevision, Contribution&);
    // One binding-lock critical section captures the complete version cohort.
    // Unbound targets are omitted; a bound source without a snapshot rejects all.
    MmsResult CaptureSubtree(RefreshClient&, const std::vector<MmsUiTargetRefV1>&, std::vector<Captured>&);
    MmsResult CaptureScoped(RefreshClient&, MmsHandle subscription, MmsHandle trigger, uint64_t triggerRevision,
        const std::vector<MmsUiTargetRefV1>&, Contribution& triggerPin, std::vector<Captured>&);
    MmsResult ValidateTrigger(const RefreshClient&, const Contribution&) const;
    MmsResult ValidateSubtree(const RefreshClient&, const std::vector<MmsUiTargetRefV1>&,
        const std::vector<Captured>&) const;
    MmsResult Consume(const Contribution&, const std::function<MmsResult()>& revalidate,
        uint64_t& consumedRevision, bool& displayChanged, uint32_t* subscriptionWait = nullptr,
        uint64_t* subscriptionCapacityRevision = nullptr);
    bool HasBinding(const RefreshClient&, const MmsUiTargetRefV1&) const;
    void InvalidateTarget(MmsHandle) noexcept;
    void AdvanceProjectEpoch(uint64_t) noexcept;
    void Retire(RefreshClient&) noexcept;
    bool HasLeases(const RefreshClient&) const;
};
}
