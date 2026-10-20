#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Global playback mode is authored project state. Rigid mode/kinematic bits
// are its live projection at the current per-bone physics switches. Replay
// changes that projection without restoring an old simulation time or pose.
// Same-value plans return no participant. The menu may atomically repair
// transient derived rigid flags without editing project data or history.
MmsResult PlanPhysicsMode(MMDApp&, int mode, std::shared_ptr<HistoryChange>&) noexcept;
bool TrySetBoundPhysicsMode(MMDApp*, int mode) noexcept;
void RefreshBoundPhysicsModeReadout(MMDApp&) noexcept;
}
#endif
