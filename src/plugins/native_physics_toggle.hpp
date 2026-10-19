#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Change the physics marker of selected, existing keys on rigid-body bones.
// A successful null result is a no-op. The caller serializes planning and the
// first preparation, and budgets retained resident model resources separately.
// Preparation samples only affected bones at the current playhead. It uses the
// native seek on a private bone/key copy, including mode-3 IK arming, and never
// steps the world or seeks the live model. Dirty author poses remain untouched.
// Publication owns key bytes, free-curve baselines, affected bone runtime data
// and the corresponding Bullet kinematic bits / native rigid-mode bytes.
MmsResult PlanPhysicsKeyToggle(MMDApp&, unsigned modelSlot, bool disabled,
    std::shared_ptr<HistoryChange>&) noexcept;
// Explicit dialog confirmation. True consumes rejection/no-op while bound;
// unbound and compatibility-profile callers retain the original native route.
bool TryApplyBoundPhysicsKeyToggle(MMDApp*, bool disabled) noexcept;
}
#endif
