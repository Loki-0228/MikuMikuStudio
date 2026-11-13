#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <utility>
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::curve_host {
enum class Kind;
class TrackAuthoring;
struct NativeKeyView;
// Implemented alongside TrackAuthoring, where its private seed/target storage
// is visible. Read-only and allocation-free; requires frozen RetimingFrames.
// Bound one deleteKeys=true PrepareRetimed plus its owned target and Sample
// scratch, from actual channel/key/interval/string capacities and map entries.
// Include container growth and coexistence of old/new buffers; do not include
// the borrowed native view/map or the already retained source record. Failure
// leaves bytes unchanged. Unknown storage/overflow must fail, never guess.
MmsResult MeasureGlobalDeletionRetimingPreparation(const TrackAuthoring&,
    const NativeKeyView&,const std::vector<std::pair<int,int>>& frameMap,
    bool forward,std::uint32_t resetRootChannels,std::uint64_t& bytes) noexcept;
}
namespace mikumikustudio::plugins {
class RetainedSceneObject;
using GlobalDeletionObjects=std::vector<std::shared_ptr<RetainedSceneObject>>;
using GlobalDeletionOwnerVisitor=void(*)(void*,const std::shared_ptr<RetainedSceneObject>&);
// Implemented alongside RetainedSceneObject. Read-only, allocation-free bound
// for acquiring an already identified model's holder at this slot, including
// registry/control blocks and physics bookkeeping snapshots. Existing shared
// owners need no new allocation charge. Missing identity returns NOT_READY;
// preparation cannot register permanent identities. Failure leaves bytes unchanged.
MmsResult MeasureGlobalDeletionParentPreparation(MMDApp&,unsigned slot,
    std::uint64_t& bytes) noexcept;
// Per-preparation target values in native pose units (camera/accessory angles
// are radians; gravity lane 5 retains the complete native mode word). A signed
// aggregate uses these before publication to evaluate its final negative pose.
struct GlobalDeletionTarget {
    curve_host::Kind kind;
    unsigned slot;
    const void* object;
    std::array<double,12> zeroPose;
};
class PreparedGlobalKeyDeletion : public PreparedHistoryChange {
public:
    virtual MMDApp* OwnerApp() const noexcept = 0;
    virtual const std::vector<GlobalDeletionTarget>& ZeroTargets() const noexcept = 0;
    // Complete candidate holders, including pose parents acquired after the
    // record was planned. Borrowed, deduplicated list; accessor never allocates.
    virtual const GlobalDeletionObjects& RetainedObjects() const noexcept = 0;
    void VisitRetainedOwners(void* context,GlobalDeletionOwnerVisitor visit)const{
        for(const auto& owner:RetainedObjects())visit(context,owner);
    }
    template<class Visitor>void VisitRetainedOwners(Visitor&& visit)const{
        for(const auto& owner:RetainedObjects())visit(owner);
    }
};
class GlobalKeyDeletion : public HistoryChange {
public:
    // RetainedBytes includes reference-container storage, but not these
    // holders' private bookkeeping. Visit all participants with one shared
    // MeasureBookkeeping collector and guard it with CoversBookkeeping. Also
    // visit prepared holders for new pose parents. Do not charge scene-resident
    // model/mesh/Bullet resources again.
    virtual const GlobalDeletionObjects& RetainedObjects() const noexcept = 0;
    void VisitRetainedOwners(void* context,GlobalDeletionOwnerVisitor visit)const{
        for(const auto& owner:RetainedObjects())visit(context,owner);
    }
    template<class Visitor>void VisitRetainedOwners(Visitor&& visit)const{
        for(const auto& owner:RetainedObjects())visit(owner);
    }
    // Only for a coordinated signed transaction. At a negative playhead this
    // publishes keys/authoring/cursors, leaving final pose publication to the
    // aggregate. Its guards still use the real signed playhead and live pose.
    // Ordinary Prepare retains the nonnegative-playhead contract.
    virtual MmsResult PrepareSigned(bool forward,
        std::unique_ptr<PreparedGlobalKeyDeletion>&) noexcept = 0;
    // Owning UI thread, with native edits/teardown excluded. The coordinator
    // has already charged every retained participant. Debit this shared budget
    // for positive candidate storage and preparation scratch before passing it
    // to the next participant. The charge conservatively sums measured stages
    // (released scratch is not refunded); it is not a process heap/RSS bound.
    // On failure out is null and remainingBytes and the live source are intact.
    // Planning, retained records/holders and post-publication work are excluded.
    virtual MmsResult PrepareSignedBudgeted(bool forward,std::uint64_t& remainingBytes,
        std::unique_ptr<PreparedGlobalKeyDeletion>&) noexcept = 0;
};
// Five global key families, including every selected accessory. One detached
// explicit transaction. No live mark staging, legacy write, seek or UI call.
// Current-frame fallback applies only when there are no explicit marks and
// only to selected timeline rows at a nonzero current frame.
MmsResult PlanGlobalKeyDeletion(MMDApp&,std::shared_ptr<HistoryChange>&,
    bool selectedAtCurrentFrame=false) noexcept;
// No bound command uses this until both signed halves can publish atomically.
// Current-frame fallback never selects a native key at a negative playhead.
MmsResult PlanSignedGlobalKeyDeletion(MMDApp&,std::shared_ptr<GlobalKeyDeletion>&,
    bool selectedAtCurrentFrame=false) noexcept;
}
#endif
