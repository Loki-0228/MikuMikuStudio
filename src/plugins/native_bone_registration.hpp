#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "native_bone_evaluation.hpp"
#include <new>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::bone_scale {
// Owner-module integration hook: allocation-free, checked upper bound for live
// AuthoringState capture plus AuthoringRestore(source,target) when restore is
// true; PoseStorageLease (including newly owned empty registry/ring sentinels)
// when lease is true; and before/after pose maps, including at most boneCount
// added pose nodes, when evaluate is true. Exclude existing live rings/caches.
// Null target means the live authoring is also the target. Return UINT64_MAX
// on overflow. No source or registry mutation is permitted.
std::uint64_t DeletionPreparationBytes(const unsigned char* model,
    const AuthoringState* target, bool restore, bool lease, bool evaluate) noexcept;
}
namespace mikumikustudio::bone_curve {
// Owner-module integration hook: allocation-free, checked upper bound for the
// live authoring validation capture and AuthoringRestore(source,target), with
// copied free tracks, native baselines, registration sets and any staged empty
// registry node/sentinels. Null target means live authoring. Exclude existing
// live undo/redo contents. Return UINT64_MAX on overflow.
std::uint64_t DeletionPreparationBytes(const unsigned char* model,
    const AuthoringState* target) noexcept;
}
namespace mikumikustudio::plugins {
class RetainedSceneObject;
using ModelDeletionOwnerVisitor = void (*)(void*, const std::shared_ptr<RetainedSceneObject>&);
// Owner-module hook: only new RetainSceneObject bookkeeping, including physics
// owner capture if absent; zero for an already retained owner. No allocations.
// A missing published scene identity is unsupported (UINT64_MAX); scene
// admission must register it before cancellable candidate preparation.
std::uint64_t ModelDeletionOwnerPreparationBytes(MMDApp&, unsigned modelSlot) noexcept;
// Fixed evaluation object, sample array and sorted-index scratch only. Pose
// map copies are charged by bone_scale::DeletionPreparationBytes.
std::uint64_t BoneDeletionEvaluationStorageBytes(std::size_t affectedCount) noexcept;
namespace model_delete_budget {
// A structured payload reservation, not allocator/process accounting. Scratch
// is conservatively retained in the charge so sibling peaks cannot each reuse
// the full allowance. Callers commit their local balance only after success.
struct Bytes {
    std::uint64_t value = 0;
    void Add(std::uint64_t count, std::uint64_t stride = 1) {
        if (count == UINT64_MAX || (stride && count > (UINT64_MAX - value) / stride)) throw std::bad_alloc();
        value += count * stride;
    }
    template<class T> void Array(std::uint64_t count) { Add(count, sizeof(T)); }
    template<class T> void Nodes(std::uint64_t count) {
        Add(1, sizeof(T) + 4 * sizeof(void*)); // MSVC tree sentinel.
        Add(count, sizeof(T) + 4 * sizeof(void*));
    }
    void Take(std::uint64_t& remaining) const {
        if (value > remaining) throw std::bad_alloc();
        remaining -= value;
    }
};
}
class DisplayKeyDeletion;
struct RangeScaleResult;
struct ModelPasteFamilyResult;
struct ModelPasteSource;
struct BoneRegistrationOptions {
    int frame = 0;
    int physicsMode = 2;
    bool automaticInterpolation = false;
    bool physicsEnabled = false;
};
struct BoneDeletionTarget {
    unsigned bone;
    BoneEvaluationSample zeroPose;
};
class PreparedBoneKeyDeletion : public PreparedHistoryChange {
public:
    virtual const std::vector<BoneDeletionTarget>& ZeroTargets() const noexcept = 0;
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
};
class BoneKeyDeletion : public HistoryChange {
public:
    // Enumerate existing holders only, without allocation or RetainSceneObject.
    // Includes any attached display record. Duplicates share one collector;
    // the visitor may throw. RetainedBytes remains record-payload accounting.
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
    // zeroSelectors is the direction's frame-zero display target, never the
    // current-playhead window. Required when this record has a display part or
    // any evaluated bone has a selector. Without a display edit the coordinator
    // must supply guarded frame-zero windows from the registered display track;
    // live windows are never a fallback. No pointers are retained.
    // Negative playback publishes keys/authoring/metadata only; a future signed
    // aggregate exclusively owns final pose publication, including dirty policy.
    virtual MmsResult PrepareSigned(bool forward, const std::vector<mdl::BoneOrderEntry>* zeroSelectors,
        std::unique_ptr<PreparedBoneKeyDeletion>&) noexcept = 0;
    virtual MmsResult PrepareSignedBudgeted(bool forward, const std::vector<mdl::BoneOrderEntry>* zeroSelectors,
        std::uint64_t& remainingBytes, std::unique_ptr<PreparedBoneKeyDeletion>&) noexcept = 0;
};
// Builds both directions entirely in private storage. Null output on success
// means no authored change. Only owned bone pools and unchanged solver switches
// are supported. The caller budgets resident objects separately from the record.
// Planning and the first Prepare must run in one serialized editing scope,
// without intervening authoring changes or window-message dispatch. Subsequent
// undo/redo prepares validate the retained record against the current scene.
MmsResult PlanBoneRegistration(MMDApp&, unsigned modelSlot,
    const BoneRegistrationOptions&, std::shared_ptr<HistoryChange>&) noexcept;
// Bone deletion. Auxiliary marks reject unless a model-key aggregate explicitly
// owns the other families. An optional display record provides target parent
// windows; publish this bone participant before that display participant. The
// optional keyboard fallback selects current-frame keys privately only when
// none of the three families has an explicit mark; frame zero is not implied.
MmsResult PlanBoneKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, bool selectedAtCurrentFrame = false,
    bool allowOtherMarks = false, std::shared_ptr<DisplayKeyDeletion> display = {},
    const std::vector<unsigned>* selectedSlots = nullptr) noexcept;
MmsResult PlanSignedBoneKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<BoneKeyDeletion>&, bool selectedAtCurrentFrame = false,
    bool allowOtherMarks = false, std::shared_ptr<DisplayKeyDeletion> display = {},
    const std::vector<unsigned>* selectedSlots = nullptr) noexcept;
MmsResult PlanBoneFrameLine(MMDApp&, unsigned modelSlot, int frame, bool insert, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>& out) noexcept;
struct BoneRangeScaleOptions {
    std::uint32_t start = 0, end = 0;
    double scale = 1;
    bool automaticInterpolation = false;
};
// Uses native slot-order reinsertion/collision rules. Successful edits include
// clearing the other key families' selection flags, without changing payloads.
// Requires start < end and a finite positive scale. Out-of-range frames reject
// the whole plan; an identity mapping after truncation preserves all selection.
// Fixed-axis correction needs the same optional D3DX math as the old registrar.
MmsResult PlanBoneRangeScale(MMDApp&, unsigned modelSlot, const BoneRangeScaleOptions&,
    std::shared_ptr<HistoryChange>&, RangeScaleResult* aggregate = nullptr,
    std::shared_ptr<DisplayKeyDeletion> display = {}) noexcept;
// Prepare target display-window sampling without deleting marked bone keys.
MmsResult PlanBoneDisplayEvaluation(MMDApp&, unsigned modelSlot,
    std::shared_ptr<DisplayKeyDeletion>, std::shared_ptr<HistoryChange>&) noexcept;
struct BoneInterpolationOptions {
    // Bits 0..3 select native X/Y/Z/rotation; bit 4 selects scale. Values
    // contain x1/y1/x2/y2 in 0..127. A zero mask is a no-op.
    std::uint8_t channels = 0;
    std::array<bone_scale::Curve, 5> values{};
};
// Edits marked existing keys, including marked frame-zero heads. Does not
// retime, change marks/metadata, seek, or overwrite the current authored pose
// on commit, undo or redo. Scale changes invalidate only derived transforms.
MmsResult PlanBoneInterpolationEdit(MMDApp&, unsigned modelSlot,
    const BoneInterpolationOptions&, std::shared_ptr<HistoryChange>&) noexcept;
// Menu 219: offset marked keys whose owning bone is a model root. Native
// float additions and sparse free-curve reconciliation are prepared together.
// Other key fields, scale authoring, later marks and timeline metadata remain.
MmsResult PlanBoneCenterOffset(MMDApp&, unsigned modelSlot,
    const std::array<float, 3>&, std::shared_ptr<HistoryChange>&) noexcept;
// Production register-button route under the explicit migration binding only.
// True also covers rejection/no-op: never fall through to the old ring.
bool TryRegisterBoundBones(MMDApp*) noexcept;
// Explicit model bone clipboard paste. Other copied model families are rejected
// by the caller, not partially pasted. The plan owns both key directions and
// scale/curve authoring; undo/redo never re-read the clipboard.
// Aggregate mode delegates all marks/metadata to its caller and evaluates
// associated bones against the supplied private target display selectors.
MmsResult PlanBoneKeyPaste(MMDApp&, unsigned modelSlot, bool mirror,
    std::shared_ptr<HistoryChange>&, ModelPasteFamilyResult* aggregate = nullptr,
    std::shared_ptr<DisplayKeyDeletion> display = {}) noexcept;
MmsResult PlanBoneKeyPaste(MMDApp&,unsigned modelSlot,bool mirror,const ModelPasteSource&,
    std::shared_ptr<HistoryChange>&,ModelPasteFamilyResult* aggregate=nullptr,
    std::shared_ptr<DisplayKeyDeletion> display={}) noexcept;
}
#endif
