#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Five global key families, including every selected accessory. One detached
// explicit transaction. No live mark staging, legacy write, seek or UI call.
// Current-frame fallback applies only when there are no explicit marks and
// only to selected timeline rows at a nonzero current frame.
MmsResult PlanGlobalKeyDeletion(MMDApp&,std::shared_ptr<HistoryChange>&,
    bool selectedAtCurrentFrame=false) noexcept;
}
#endif
