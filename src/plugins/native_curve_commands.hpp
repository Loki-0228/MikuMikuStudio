#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/animation_curve_host.hpp"
namespace mikudancestudio::plugins {
struct BoneCurveEdit;
struct DisplayCurveRow;
// Bound writes consume every outcome. They never return to the legacy writer.
bool WriteBoundGlobalCurves(MMDApp&, curve_host::Kind, int slot,
    const std::vector<curve_editor::Channel>&, std::wstring& error) noexcept;
bool WriteBoundMorphCurves(MMDApp&, unsigned modelSlot,
    const std::vector<animation_curve::NamedTrack>&, std::wstring& error) noexcept;
bool WriteBoundBoneCurves(MMDApp&, unsigned modelSlot,
    const std::vector<BoneCurveEdit>&, const std::vector<DisplayCurveRow>* displayRows,
    std::wstring& error) noexcept;
}
#endif
