#pragma once
#include "common.hpp"
#include <mms/refresh.h>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>

namespace mikudancestudio::plugins {
class UiRegistry;
class UiRefreshQueue;
struct RefreshClient {
    UiRefreshQueue* queue;
    std::string owner;
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> inFlight{0};
};
// Only the UI adapter supplies these. No HWND or registry access occurs in
// request/query/cancel/release, even when called from a background thread.
struct RefreshPresentation {
    MmsHandle element = 0;
    std::string owner;
    std::uint64_t instance = 0;
    HWND window = nullptr;
    std::uint32_t dpi = 96;
    float x = 0, y = 0, width = 0, height = 0;
    bool visible = false;
    // Only host-owned buttons opt in. STATE does not yet change visibility.
    bool stateRefresh = false, stateVisible = true;
};
class UiRefreshQueue {
    struct Target;
    struct Ticket;
    UiRegistry& registry_;
    DWORD thread_ = GetCurrentThreadId();
    mutable std::mutex mutex_;
    std::map<MmsHandle, Target> targets_;
    std::map<MmsHandle, Ticket> tickets_;
    std::uint64_t next_ = 1, nextGeneration_ = 1, nextGeometry_ = 1, epoch_ = 1;
    bool draining_ = false;
    bool evaluatingState_ = false;
    std::function<bool()> wake_;
    MmsResult Capabilities(RefreshClient&, MmsUiRefreshCapabilitiesV1*);
    MmsResult GetTarget(RefreshClient&, MmsHandle, MmsUiRefreshTargetV1*);
    MmsResult Request(RefreshClient&, const MmsUiRefreshRequestV1*, MmsHandle*);
    MmsResult Query(RefreshClient&, MmsHandle, MmsUiRefreshStateV1*);
    MmsResult Cancel(RefreshClient&, MmsHandle);
    MmsResult Release(RefreshClient&, MmsHandle);
public:
    explicit UiRefreshQueue(UiRegistry&);
    ~UiRefreshQueue();
    MmsUiRefreshApiV1 Api(RefreshClient&);
    // Configure once before plugin loading; Wake must be a nonblocking signal.
    void SetWake(std::function<bool()>);
    void SyncTargets(const std::vector<RefreshPresentation>&);
    void InvalidateTarget(MmsHandle) noexcept;
    // Pause before native moves, retaining the last geometry revision. Only
    // a successful publication may make a target ready again.
    void SuspendTargets() noexcept;
    void Drain();
    bool Pending() const;
    void Retire(RefreshClient&) noexcept;
    bool HasLeases(const RefreshClient&) const;
    void AdvanceProjectEpoch() noexcept;
    void FailPending(MmsResult) noexcept;
};
}
