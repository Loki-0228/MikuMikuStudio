#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
MmsResult PlanCameraFrameLine(MMDApp&, int frame, bool insert, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult PlanLightFrameLine(MMDApp&, int frame, bool insert, std::shared_ptr<HistoryChange>&) noexcept;
// The migration binding consumes failure/no-op without entering an old ring.
bool TryBoundFrameLineCommand(MMDApp*, unsigned command, int& affected) noexcept;
}
#endif
