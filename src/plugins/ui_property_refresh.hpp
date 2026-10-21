#pragma once
#include "ui_refresh.hpp"
#include "ui_refresh_binding.hpp"
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
    using Work = UiPropertyRefreshWork;
    explicit UiPropertyRefresh(UiRefreshQueue&, UiRegistry&);
    ~UiPropertyRefresh();
    MmsResult BindClient(RefreshClient&, UiClient&);
    MmsUiPropertyRefreshApiV1 Api(RefreshClient&);
    MmsUiPropertyRefreshApiV1_1 ScopedApi(RefreshClient&);
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
        uint32_t waiting = MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER, uint64_t workGeneration = 0,
        uint64_t displayCapacityRevision = UINT64_MAX) noexcept;
    bool Alive(const RefreshClient&, MmsHandle, uint64_t workGeneration = 0) const noexcept;
    bool Pending() const noexcept;
    bool RetryCapacity(uint64_t) noexcept;
    bool TargetPublished(MmsHandle, bool ready, bool providerChanged = false) noexcept;
    bool BindingChanged(const RefreshClient&, const MmsUiTargetRefV1&, bool bound) noexcept;
    // A real public binding identity event for a non-SELF scope. It marks the
    // exact captured member (or the SUBTREE root) for one fresh capture and
    // never wakes; the original cleanup after the same mutation performs the
    // single wake. A DEPENDENTS seed is an anchor, not a captured data member.
    void ScopedBindingPublished(const RefreshClient&, const MmsUiTargetRefV1&) noexcept;
    // Same successful retained-tree handshake for SUBTREE watches.
    void ScopedTopologyChanged(void* context, bool (*changed)(void*, MmsHandle)) noexcept;
    bool NextTarget(MmsHandle after, Work&) const noexcept;
    bool NextScoped(MmsHandle after, Work&) const noexcept;
    // Trusted commit after every temporary allocation succeeded. Members,
    // dependency revision, real blocks and source watches land under one
    // property lock; a failed observation never reaches this call.
    void ObserveScope(const Work&, const std::vector<MmsUiTargetRefV1>&, uint64_t dependencyRevision,
        uint32_t blocked, const std::vector<UiRefreshBindings::PropertySource>&) noexcept;
    void ObserveSources(MmsHandle subscription, const std::vector<UiRefreshBindings::PropertySource>&, bool captured = false) noexcept;
    void SourcePublished(RefreshClient&, MmsHandle, uint64_t) noexcept;
    // Only the current retained graph decides whether a real graph event is
    // relevant: the event upstream must be exactly reachable from the seed.
    void GraphChanged(const RefreshClient&, const MmsUiTargetRefV1&, const UiRefreshDependencies& graph) noexcept;
    bool RetryDisplayCapacity(uint64_t displayRevision, uint64_t resourceRevision = UINT64_MAX) noexcept;
    void FailScheduling(MmsResult) noexcept;
    void InvalidateTarget(MmsHandle) noexcept;
    void AdvanceProjectEpoch(uint64_t) noexcept;
    void Retire(RefreshClient&) noexcept;
};
}
