#pragma once
#include "ui_refresh.hpp"
#include <mms/property_refresh.h>

namespace mikumikustudio::plugins {
// Original PropertyRecord notifications use the refresh queue's native INPUT
// consumer. This store holds identities/mailboxes, never another UI queue.
class UiPropertyRefresh {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MmsResult CheckUi(RefreshClient&, const UiClient*) const;
    MmsResult ValidateTarget(RefreshClient&, const UiClient*, const MmsUiTargetRefV1&,
        const std::shared_ptr<const PropertyRecord>&, uint32_t, bool&, bool&) const;
    bool ReadOnlyNotification(RefreshClient&, const UiClient*) const;
public:
    struct Work {
        RefreshClient* client = nullptr;
        MmsHandle subscription = 0;
        MmsUiTargetRefV1 target{};
        uint32_t kinds = 0;
        uint64_t sequence = 0, capacityRevision = 0;
    };
    explicit UiPropertyRefresh(UiRefreshQueue&, UiRegistry&);
    ~UiPropertyRefresh();
    MmsResult BindClient(RefreshClient&, UiClient&);
    MmsUiPropertyRefreshApiV1 Api(RefreshClient&);
    void SetHistoryAvailable(bool);
    // Trusted producer signal only. The host posts its existing UI wake;
    // Drain consumes this atomic marker before taking subscription work.
    void NotifyHistory(uint64_t) noexcept;
    void HistoryWakeResult(bool) noexcept;
    void DrainHistory() noexcept;
    void InvalidateProperty(MmsHandle) noexcept;
    void Retire(UiClient&) noexcept;
    // Queue -> property store only. No hooks/callbacks from these methods.
    bool Take(MmsHandle after, Work&) noexcept;
    bool Submit(const Work&, MmsHandle ticket, MmsResult) noexcept;
    void Complete(MmsHandle subscription, MmsHandle ticket, uint64_t sequence, MmsResult,
        uint32_t waiting = MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER) noexcept;
    bool Alive(const RefreshClient&, MmsHandle) const noexcept;
    bool Pending() const noexcept;
    bool RetryCapacity(uint64_t) noexcept;
    bool TargetPublished(MmsHandle, bool ready, bool providerChanged = false) noexcept;
    bool BindingChanged(const RefreshClient&, const MmsUiTargetRefV1&, bool bound) noexcept;
    bool NextTarget(MmsHandle after, Work&) const noexcept;
    void FailScheduling(MmsResult) noexcept;
    void InvalidateTarget(MmsHandle) noexcept;
    void AdvanceProjectEpoch(uint64_t) noexcept;
    void Retire(RefreshClient&) noexcept;
};
}
