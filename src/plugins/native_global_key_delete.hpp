#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Five global key families, including every selected accessory. One detached
// explicit transaction. No live mark staging, legacy write, seek or UI call.
// Current-frame fallback applies only when there are no explicit marks and
// only to selected timeline rows at a nonzero current frame.
MmsResult PlanGlobalKeyDeletion(MMDApp&,std::shared_ptr<HistoryChange>&,
    bool selectedAtCurrentFrame=false) noexcept;
}
#endif
