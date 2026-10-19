#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Internal menu-230 candidate. No live writes or UI calls during preparation.
// Only weights changed by this reset are owned during later undo/redo.
MmsResult PlanMorphReset(MMDApp&, unsigned modelSlot, std::shared_ptr<HistoryChange>&) noexcept;
// Owns success, no-op and rejection under the explicit migration binding.
bool TryResetBoundMorphs(MMDApp*) noexcept;
}
#endif
