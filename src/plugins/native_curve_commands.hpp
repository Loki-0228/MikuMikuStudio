#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/animation_curve_host.hpp"
namespace mikudancestudio::plugins {
// Bound writes consume every outcome. They never return to the legacy writer.
bool WriteBoundGlobalCurves(MMDApp&, curve_host::Kind, int slot,
    const std::vector<curve_editor::Channel>&, std::wstring& error) noexcept;
}
#endif
