#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <mms/plugin.h>
#include <memory>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
class HistoryChange;
// Internal project adjustment, including the live ground body's exact flags.
// The application owns the scene/world for the journal's lifetime. Records do
// not retain a dead physics scene: every access first checks the live source.
MmsResult PlanFloor(MMDApp&, bool enabled, std::shared_ptr<HistoryChange>& out) noexcept;
MmsResult ApplyBoundFloor(MMDApp&, bool enabled, bool& changed) noexcept;
// A bound route consumes no-op and rejection, with no legacy fallthrough.
bool TryToggleBoundFloor(MMDApp*) noexcept;
void RefreshBoundFloorReadout(MMDApp&) noexcept;
}
#endif
