#pragma once
#include "common.hpp"
#include <mms/refresh_dependency.h>
#include <map>
#include <vector>

namespace mikumikustudio::plugins {
struct RefreshClient;
// Pure retained graph. The refresh queue owns its lock and validates endpoint
// publications; this module never reads HWNDs, the registry or plugin callbacks.
class UiRefreshDependencies {
    struct Owner { uint64_t revision = 1; size_t count = 0; };
    struct Edge { const RefreshClient* owner; MmsUiTargetRefV1 upstream, dependent; };
    std::map<const RefreshClient*, Owner> owners_;
    std::map<MmsHandle, Edge> edges_;
    MmsHandle next_ = 1;
    static void Advance(Owner&) noexcept;
public:
    static constexpr uint32_t kPerOwner = 512, kGlobal = 4096, kMembers = 256;
    uint64_t Revision(const RefreshClient&) const noexcept;
    bool HasDependents(const RefreshClient&, const MmsUiTargetRefV1&) const noexcept;
    // Exact retained-graph reachability; the seed itself is zero hops. The BFS
    // is bounded by kPerOwner + 1 vertices and only walks edges this client
    // already retained. It never allocates or reads targets, the registry or
    // HWNDs.
    bool Reaches(const RefreshClient&, const MmsUiTargetRefV1& seed, const MmsUiTargetRefV1& target) const noexcept;
    MmsResult Add(const RefreshClient&, const MmsUiTargetRefV1&, const MmsUiTargetRefV1&, MmsHandle&);
    MmsResult Remove(const RefreshClient&, MmsHandle, MmsUiTargetRefV1* removedUpstream = nullptr);
    MmsResult Capture(const RefreshClient&, const MmsUiTargetRefV1&,
        std::vector<MmsUiTargetRefV1>&, uint64_t&) const;
    void InvalidateTarget(MmsHandle) noexcept;
    void Retire(const RefreshClient&) noexcept;
    void Clear() noexcept;
};
}
