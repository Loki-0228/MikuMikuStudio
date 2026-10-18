#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikudancestudio/animation_curve.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Entirely detached preparation. Null success is a semantic no-op. Publish
// through SharedHistory with NativeIntent::ExplicitCommit in either mode.
MmsResult PlanMorphCurveWrite(MMDApp&,unsigned modelSlot,
    const std::vector<animation_curve::NamedTrack>&,std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
