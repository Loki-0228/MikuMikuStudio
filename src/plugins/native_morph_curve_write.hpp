#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/animation_curve.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Entirely detached preparation. Null success is a semantic no-op. Publish
// through SharedHistory with NativeIntent::ExplicitCommit in either mode.
MmsResult PlanMorphCurveWrite(MMDApp&,unsigned modelSlot,
    const std::vector<animation_curve::NamedTrack>&,std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
