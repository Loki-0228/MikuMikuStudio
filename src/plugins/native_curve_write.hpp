#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Detached, full authored replacement for the five host curve families.
// Null success is a semantic no-op. The caller publishes through the shared
// journal with ExplicitCommit; no callbacks/UI/file access occur in Publish.
MmsResult PlanGlobalCurveWrite(MMDApp&,curve_host::Kind,int slot,
    const std::vector<curve_editor::Channel>&,std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
