#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// One native journal participant for the active timeline's signed selection.
// Source and direction targets remain detached until the journal commits it.
// The caller must use the app's bound history. A null result is a true no-op.
MmsResult PlanSignedKeyDeletion(MMDApp&,SharedHistory&,bool automaticInterpolation,
    bool selectedAtCurrentFrame,std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
