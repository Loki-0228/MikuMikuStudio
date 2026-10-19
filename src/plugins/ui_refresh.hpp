#pragma once
#include "common.hpp"
#include <mms/refresh.h>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>

namespace mikumikustudio::plugins {
class UiRegistry;
struct UiClient;
class UiRefreshQueue;
class UiRefreshBindings;
struct ButtonStateContent;
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
    // Host-owned buttons opt in. Visibility is mutable only in a reserved slot.
    bool stateRefresh = false, stateVisible = true;
    bool stateVisibility = false, layoutVisible = true;
    bool layoutRefresh = false;
    // A retained GROUP uses its original registration identity but has no
    // separate drawable client area. Only SUBTREE is exposed for it.
    bool container = false;
};
class UiRefreshQueue {
    struct Target;
    struct Ticket;
    struct TreeNode {
        MmsHandle parent = 0;
        std::string owner;
        uint32_t kind = 0;
        bool operator==(const TreeNode& other) const noexcept {
            return parent == other.parent && owner == other.owner && kind == other.kind;
        }
    };
    UiRegistry& registry_;
    std::unique_ptr<UiRefreshBindings> bindings_;
    DWORD thread_ = GetCurrentThreadId();
    mutable std::mutex mutex_;
    std::map<MmsHandle, Target> targets_;
    std::map<MmsHandle, Ticket> tickets_;
    std::map<MmsHandle, TreeNode> tree_;
    uint64_t topologyRevision_ = 1;
    bool topologyReady_ = false;
    std::uint64_t next_ = 1, nextGeneration_ = 1, nextGeometry_ = 1, epoch_ = 1;
    bool draining_ = false;
    const RefreshClient* evaluatingStateOwner_ = nullptr;
    const UiClient* evaluatingStateProvider_ = nullptr;
    const RefreshClient* evaluatingDataOwner_ = nullptr;
    const RefreshClient* evaluatingLayoutOwner_ = nullptr;
    const UiClient* evaluatingLayoutProvider_ = nullptr;
    const RefreshClient* inheritedReadOnlyCause_ = nullptr;
    std::function<bool()> wake_;
    std::function<MmsResult(MmsHandle)> layout_;
    std::function<MmsResult(MmsHandle, const std::shared_ptr<const ButtonStateContent>&, bool)> stateContent_;
    MmsResult Capabilities(RefreshClient&, MmsUiRefreshCapabilitiesV1*);
    MmsResult GetTarget(RefreshClient&, MmsHandle, MmsUiRefreshTargetV1*);
    MmsResult Request(RefreshClient&, const MmsUiRefreshRequestV1*, MmsHandle*);
    MmsResult Query(RefreshClient&, MmsHandle, MmsUiRefreshStateV1*);
    MmsResult Cancel(RefreshClient&, MmsHandle);
    MmsResult Release(RefreshClient&, MmsHandle);
    bool ReadOnlyCauseLocked(const RefreshClient&) const noexcept;
    std::vector<MmsHandle> SubtreeLocked(MmsHandle) const;
    MmsResult ValidateTicketLocked(const Ticket&, bool geometry) const;
public:
    explicit UiRefreshQueue(UiRegistry&);
    ~UiRefreshQueue();
    MmsUiRefreshApiV1 Api(RefreshClient&);
    UiRefreshBindings& Bindings() noexcept { return *bindings_; }
    // Manager UI tasks inherit the read-only origin of a refresh callback.
    // These tokens never enter public request structs or plugin-owned data.
    bool ReadOnlyCause() const;
    bool ReadOnlyCause(const RefreshClient&) const;
    const RefreshClient* ExchangeInheritedReadOnlyCause(const RefreshClient*) noexcept;
    // Configure once before plugin loading; Wake must be a nonblocking signal.
    void SetWake(std::function<bool()>);
    // The native adapter remeasures its retained layout. This is not a plugin
    // callback and must never evaluate scene data or save user preferences.
    void SetLayout(std::function<MmsResult(MmsHandle)>);
    void SetStateContent(std::function<MmsResult(MmsHandle, const std::shared_ptr<const ButtonStateContent>&, bool)>);
    bool HasStateContent() const;
    MmsResult CheckStateContentTarget(const std::string&, const MmsUiTargetRefV1&) const;
    void SetStateProviderRevision(MmsHandle, uint64_t) noexcept;
    bool CachedStateVisible(MmsHandle, bool fallback) const;
    MmsResult WithLayoutProvider(const UiClient&, const std::function<MmsResult()>&);
    void SyncTargets(const std::vector<RefreshPresentation>&);
    // UI-only publication. Requests use this copy, never registry pointers.
    // Failure closes SUBTREE acceptance until the next successful publication.
    void SyncTopology() noexcept;
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
