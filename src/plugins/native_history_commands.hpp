#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
class SharedHistory;
void RefreshBoundPoseReadout(MMDApp&) noexcept;
// Only after a successful, nonempty edit and after its token has been released.
// Refreshes readouts and common-history buttons without selecting global tracks,
// seeking, changing the selected model, or touching the legacy rings.
void PublishBoundPoseEdit(MMDApp&, SharedHistory&, bool physicsReset = false) noexcept;
// Internal migration binding only. True means the route is owned, including
// empty/busy/failed history: callers must not fall through to the legacy ring.
// Playback and recording cannot execute this route. Common history is chosen
// before the selected model or camera-mode gate.
bool TryBoundHistoryCommand(MMDApp*, bool redo) noexcept;
}
#endif
