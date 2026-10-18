#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Both checkbox and menu request a complete detached follow transition.
// Initial enable samples all legacy tracks; replay owns only changed scalars.
MmsResult PlanCameraFollow(MMDApp&,bool enabled,std::shared_ptr<HistoryChange>&) noexcept;
bool TrySetBoundCameraFollow(MMDApp*,bool enabled) noexcept;
}
#endif
