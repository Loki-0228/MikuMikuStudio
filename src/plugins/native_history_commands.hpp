#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
void RefreshBoundPoseReadout(MMDApp&) noexcept;
// Only after a successful, nonempty edit and after its token has been released.
// Publishes exactly the frozen effects of that publication: project scalars for
// ProjectData, pose/scene readouts for SceneReadout, and a drawing-only viewport
// request for Viewport. Physics invalidation stays independent of the mask. A
// failed UI write can only queue a retry at the normal message boundary; it can
// never fail the history publication that already committed.
void PublishBoundHistoryEffects(MMDApp&, SharedHistory&, HistoryEffects, bool physicsReset = false) noexcept;
// Full-mask compatibility wrapper for every native command that predates the
// effect masks. It keeps the original all-readouts publication behavior.
void PublishBoundPoseEdit(MMDApp&, SharedHistory&, bool physicsReset = false) noexcept;
// Internal migration binding only. True means the route is owned, including
// empty/busy/failed history: callers must not fall through to the legacy ring.
// Playback and recording cannot execute this route. Common history is chosen
// before the selected model or camera-mode gate.
bool TryBoundHistoryCommand(MMDApp*, bool redo) noexcept;
}
#endif
