#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Internal menu-230 candidate. No live writes or UI calls during preparation.
// Only weights changed by this reset are owned during later undo/redo.
MmsResult PlanMorphReset(MMDApp&, unsigned modelSlot, std::shared_ptr<HistoryChange>&) noexcept;
// Owns success, no-op and rejection under the explicit migration binding.
bool TryResetBoundMorphs(MMDApp*) noexcept;
}
#endif
