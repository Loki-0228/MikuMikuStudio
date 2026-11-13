#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::morph_curve {
// Owner-module integration hook: allocation-free checked bound for one live
// AuthoredLayers capture and SampleLayersTarget scratch at nativeCapacity,
// including native traversal, copied/derived tracks and reconciliation maps.
// Source and target samples are sequential; charge their maximum scratch.
// Return UINT64_MAX on overflow; never refresh or mutate authored layers.
std::uint64_t DeletionPreparationBytes(const unsigned char* model, std::size_t nativeCapacity) noexcept;
}
namespace mikumikustudio::plugins {
class RetainedSceneObject;
using ModelDeletionOwnerVisitor = void (*)(void*, const std::shared_ptr<RetainedSceneObject>&);
// Register one morph's current weight at an absolute frame. Matches the native
// current-value registrar (including its local free-slot scan and unchanged
// searchCursor), then clears all three key families' selection marks. Only an
// actual native key-data change creates a record; mark-only changes are no-ops.
// Pristine morph curves survive and derive against the registered native keys.
// First publication preserves the live weight; undo/redo sample only this morph
// at the then-current playhead. Other weights/bone poses and old rings survive.
// The app outlives the retained scene owner/journal. As for bone registration,
// resident model/pool resources are budgeted separately from this record.
MmsResult PlanMorphRegistration(MMDApp&, unsigned modelSlot, int morphIndex, int frame,
    std::shared_ptr<HistoryChange>& out) noexcept;
// Deletes only marked morph keys, with the native root/sentinel rules. Other
// families' marks reject by default; allowOtherMarks is for a caller that
// prepares and validates all families before publishing one aggregate change.
// No other family's marks, undo rings, cursor or maximum frame are changed.
//
// Each direction samples only affected morphs at the current playhead, using
// the untouched pristine curves and the target native tracks. Morph weights
// have no unregistered-edit flag: like native delete/undo, affected weights are
// replaced by evaluation; unrelated morph weights remain as they were.
// Registered pool identity survives growth. The record retains its scene
// object; the app must outlive the journal. Resident resources are budgeted
// separately from this record's payload. Planning/first Prepare are serialized.
// A successful null result is a no-op and must not discard redo.
struct ModelPasteFamilyResult;
struct ModelPasteSource;
MmsResult PlanMorphKeyPaste(MMDApp&, unsigned modelSlot, std::shared_ptr<HistoryChange>& out,
    ModelPasteFamilyResult* aggregate = nullptr) noexcept;
MmsResult PlanMorphKeyPaste(MMDApp&,unsigned modelSlot,const ModelPasteSource&,
    std::shared_ptr<HistoryChange>&,ModelPasteFamilyResult* aggregate=nullptr) noexcept;
MmsResult PlanMorphKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>& out, bool allowOtherMarks = false,
    const std::vector<unsigned>* selectedSlots = nullptr) noexcept;
struct MorphDeletionTarget {
    unsigned morph;
    float zeroWeight;
};
class PreparedMorphKeyDeletion : public PreparedHistoryChange {
public:
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
    // Direction-specific frame-zero weights for every affected morph, including
    // positive-only deletions. Owned by this candidate; no live cache syncing.
    virtual const std::vector<MorphDeletionTarget>& ZeroTargets() const noexcept = 0;
};
class MorphKeyDeletion : public HistoryChange {
public:
    // Existing holders only; traversal allocates nothing and never retains or
    // scans scene objects. The visitor may throw; RetainedBytes is unchanged.
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
    // Internal signed-delete preparation only. A negative live playhead keeps
    // all live morph weights unchanged on publication; target keys/marks still
    // publish. At nonnegative playheads normal deletion pose semantics apply.
    // Both paths guard the real playhead, source weights, keys and author layers.
    // Failure leaves out unchanged. Ordinary Prepare retains its negative gate.
    virtual MmsResult PrepareSigned(bool forward, std::unique_ptr<PreparedMorphKeyDeletion>& out) = 0;
    virtual MmsResult PrepareSignedBudgeted(bool forward, std::uint64_t& remainingBytes,
        std::unique_ptr<PreparedMorphKeyDeletion>& out) noexcept = 0;
};
// Same selection and ownership rules as PlanMorphKeyDeletion. No command gate
// is changed. Null success is empty; failure preserves the caller's output.
MmsResult PlanSignedMorphKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<MorphKeyDeletion>& out, bool allowOtherMarks = false,
    const std::vector<unsigned>* selectedSlots = nullptr) noexcept;
MmsResult PlanMorphFrameLine(MMDApp&, unsigned modelSlot, int frame, bool insert,
    std::shared_ptr<HistoryChange>& out) noexcept;
struct BoneRangeScaleOptions;
struct RangeScaleResult;
// Internal aggregate participant. Simulates the native slot-order deletion /
// reinsertion even when every mapped frame is identical; the caller decides
// whether the complete multi-family operation is a no-op. The result carries
// cursor/maximum/marks for the aggregate, never writes shared scalar fields or
// another family's marks, and never grows the live key pool.
MmsResult PlanMorphRangeScale(MMDApp&, unsigned modelSlot,
    const BoneRangeScaleOptions&, RangeScaleResult&) noexcept;
}
#endif
