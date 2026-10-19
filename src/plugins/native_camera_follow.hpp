#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Both checkbox and menu request a complete detached follow transition.
// Initial enable samples all legacy tracks; replay owns only changed scalars.
MmsResult PlanCameraFollow(MMDApp&,bool enabled,std::shared_ptr<HistoryChange>&) noexcept;
bool TrySetBoundCameraFollow(MMDApp*,bool enabled) noexcept;
}
#endif
