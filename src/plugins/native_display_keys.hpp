#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikudancestudio/model.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
class DisplayKeyDeletion : public HistoryChange {
public:
    // Re-evaluate at the current playhead without writing live selectors.
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
MmsResult PlanDisplayKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>& out, bool allowOtherMarks = false,
    bool evaluateBones = true) noexcept;
}
#endif
