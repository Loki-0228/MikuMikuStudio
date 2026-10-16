#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Display/IK/external-parent keys only. Bone or morph marks reject the whole
// request. Null output on success means no marked display keys. The display
// pool must have registered ownership; borrowed arrays are not inferred owned.
// Planning and the first Prepare share one serialized editing scope. The
// record retains the model owner. The app must outlive records and candidates.
// Resident model resources are budgeted by the caller, separately from payload.
MmsResult PlanDisplayKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>& out) noexcept;
}
#endif
