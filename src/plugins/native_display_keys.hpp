#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/model.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct BoneRangeScaleOptions;
struct RangeScaleResult;
struct ModelPasteFamilyResult;
struct ModelPasteSource;
class DisplayKeyDeletion : public HistoryChange {
public:
    // Re-evaluate at the current playhead without writing live selectors.
    // A private deletion's first forward target preserves live drafts and is
    // identical to its display publication; undo/redo sample registered keys.
    // Failure leaves out unchanged. Other selector fields retain live values.
    virtual MmsResult TargetSelectors(bool forward,
        std::vector<mdl::BoneOrderEntry>& out) const noexcept = 0;
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
