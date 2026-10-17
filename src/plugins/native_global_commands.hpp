#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
#include <memory>
#include <mms/plugin.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
class HistoryChange;
// Owned camera-pose delta, separate from camera keys and curve authoring.
// A successful no-op leaves out empty. The app outlives the record/history.
MmsResult PlanCameraRotationReset(MMDApp&, std::shared_ptr<HistoryChange>& out) noexcept;
bool TryResetBoundCameraRotation(MMDApp*) noexcept;
// True consumes the bound command, including busy/invalid/no-op outcomes.
// These read controls before planning and never enter a legacy registrar.
bool TryRegisterBoundCamera(MMDApp*, HWND) noexcept;
bool TryRegisterBoundLight(MMDApp*, HWND) noexcept;
bool TryRegisterBoundShadow(MMDApp*, HWND) noexcept;
bool TryRegisterBoundAccessory(MMDApp*, HWND) noexcept;
// Echo current global values after history publication without evaluating keys.
void RefreshBoundGlobalReadout(MMDApp&) noexcept;
}
#endif
