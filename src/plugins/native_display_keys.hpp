#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/model.hpp"
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
class RetainedSceneObject;
using ModelDeletionOwnerVisitor = void (*)(void*, const std::shared_ptr<RetainedSceneObject>&);
struct BoneRangeScaleOptions;
struct RangeScaleResult;
struct ModelPasteFamilyResult;
struct ModelPasteSource;
struct DisplayDeletionTarget {
    unsigned char visible=0;
    std::vector<unsigned char> ik;
    std::vector<mdl::BoneOrderEntry> selectors;
};
class PreparedDisplayKeyDeletion : public PreparedHistoryChange {
public:
    // Registered target at frame zero, never a private-selection live draft.
    // The candidate owns this value and leases all source/target parent owners.
    virtual const DisplayDeletionTarget& ZeroTarget() const noexcept = 0;
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
};
// Read dependency, not an edit: captures registered frame-zero display state
// regardless of marks, the real signed playhead or live display drafts. Guards
// the complete source chain/deep payloads, live state and all parent identities.
// Publish only consumes this candidate; it never writes native/authoring state,
// evaluates bones or grows storage. Callers may retain it solely for IsCurrent.
// The caller serializes preparation with scene edits. Failure preserves out.
MmsResult PrepareDisplayKeyZeroRead(MMDApp&, unsigned modelSlot,
    std::unique_ptr<PreparedDisplayKeyDeletion>& out) noexcept;
MmsResult PrepareDisplayKeyZeroReadBudgeted(MMDApp&, unsigned modelSlot, std::uint64_t& remainingBytes,
    std::unique_ptr<PreparedDisplayKeyDeletion>& out) noexcept;
class DisplayKeyDeletion : public HistoryChange {
public:
    // Existing model and parent holders only, including paste references.
    // No allocation, RetainSceneObject or scene scan. The visitor may throw;
    // duplicates share one collector and RetainedBytes keeps its payload scope.
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
    // Re-evaluate at the current playhead without writing live selectors.
    // A private deletion's first forward target preserves live drafts and is
    // identical to its display publication; undo/redo sample registered keys.
    // Failure leaves out unchanged. Other selector fields retain live values.
    virtual MmsResult TargetSelectors(bool forward,
        std::vector<mdl::BoneOrderEntry>& out) const noexcept = 0;
    // Deletion records only; non-deletion records return MMS_E_NOT_READY.
    // Explicit nonnegative sample frame, independent of the real playhead.
    // Registered target selectors retain other live selector metadata, but no
    // draft linked parent/window override. Failure preserves out.
    virtual MmsResult TargetSelectorsAt(bool forward,int sampleFrame,
        std::vector<mdl::BoneOrderEntry>& out) const noexcept = 0;
    // Negative real playheads publish keys/marks only: no display/IK/selectors
    // or bone pose writes. Nonnegative playheads keep ordinary pose semantics.
    // Both directions expose registered zero targets and validate the full live
    // source and parent identities before publication. Failure preserves out.
    virtual MmsResult PrepareSigned(bool forward,
        std::unique_ptr<PreparedDisplayKeyDeletion>& out) = 0;
    virtual MmsResult PrepareSignedBudgeted(bool forward, std::uint64_t& remainingBytes,
        std::unique_ptr<PreparedDisplayKeyDeletion>& out) noexcept = 0;
    // Scratch bound for TargetSelectors at either zero or the real playhead.
    virtual std::uint64_t SelectorPreparationBytes() const noexcept = 0;
};
// Display/IK/external-parent keys only. Bone or morph marks reject the whole
// request unless allowOtherMarks is true. Null success means no marked keys.
// A combined deletion supplies evaluateBones=false and publishes its own bone
// participant before this display participant, after validating all candidates.
// Independent deletion evaluates associated clean bones and preserves dirty
// poses. Neither form seeks the whole model or changes physics mode. The display
// pool must have registered ownership; borrowed arrays are not inferred owned.
// Planning and the first Prepare share one serialized editing scope. The
// record retains the model owner. The app must outlive records and candidates.
// Resident model resources are budgeted by the caller, separately from payload.
// A nonnull private slot set preserves current visibility, IK and complete
// external-parent selectors only on first forward publication. It never stages
// live marks; later undo/redo retain the normal current-time evaluation rules.
MmsResult PlanDisplayKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>& out, bool allowOtherMarks = false,
    bool evaluateBones = true, const std::vector<unsigned>* selectedSlots = nullptr) noexcept;
// Null success is empty; failure leaves out unchanged. This only creates an
// internal record and does not admit any mixed command or signed publication.
MmsResult PlanSignedDisplayKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<DisplayKeyDeletion>& out, bool allowOtherMarks = false,
    bool evaluateBones = true, const std::vector<unsigned>* selectedSlots = nullptr) noexcept;
// Private display-family range plan. The aggregate owns final selection marks
// and shared cursor/max-frame publication and supplies bone reevaluation.
// Name mapping matches the native registrar, including duplicate/empty names
// and the model-parent slot zero. Nonterminated legacy names and import-name
// scopes are rejected. No live pool growth is performed; failure preserves out.
MmsResult PlanDisplayRangeScale(MMDApp&, unsigned modelSlot,
    const BoneRangeScaleOptions&, RangeScaleResult& out) noexcept;
// Register current visibility/IK/external parents by index, matching command
// 438. First publication only writes keys, selection marks and registration
// metadata; undo/redo sample at the current playhead through the local evaluator.
// Dirty authoring poses survive. Identical authoring data returns null success
// without clearing marks or redo. Growth is prepared privately for owned pools.
MmsResult PlanDisplayRegistration(MMDApp&, unsigned modelSlot, int frame,
    std::shared_ptr<HistoryChange>& out) noexcept;
// Display-only clipboard paste. Deep payloads and both directions are owned;
// no clipboard/source file reads occur during undo or redo. First publication
// samples the target track while preserving dirty authoring poses.
// Aggregate mode retains private growth but delegates marks/metadata and bone
// evaluation to the model aggregate; its target selectors remain queryable.
MmsResult PlanDisplayKeyPaste(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>& out, ModelPasteFamilyResult* aggregate = nullptr) noexcept;
MmsResult PlanDisplayKeyPaste(MMDApp&,unsigned modelSlot,const ModelPasteSource&,
    std::shared_ptr<HistoryChange>&,ModelPasteFamilyResult* aggregate=nullptr) noexcept;
}
#endif
