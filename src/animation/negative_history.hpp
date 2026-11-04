#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <memory>
#include <mms/edit.h>
namespace mikumikustudio {
class MMDApp;
namespace plugins {class HistoryChange;}
namespace negative_frames {
// Internal detached deletion for selected keys in the active timeline. A null
// success is empty. A future signed aggregate must reconcile shared selection
// and target zero-frame baselines before joining any positive participant.
// This never deletes positive keys or changes current selection/pose itself.
MmsResult PlanSelectedDeletion(MMDApp&,std::shared_ptr<plugins::HistoryChange>&) noexcept;
}
}
#endif
