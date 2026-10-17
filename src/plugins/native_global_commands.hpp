#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
#include <memory>
#include <array>
#include <mms/plugin.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
class HistoryChange;
class SharedHistory;
// Internal modal session. The app outlives the session; no journal reservation
// or scene resource ownership is held while a dialog pumps messages.
class CameraNumericEdit {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    CameraNumericEdit();
    ~CameraNumericEdit();
    MmsResult Begin(MMDApp&, SharedHistory&) noexcept;
    std::array<float, 7> Values() const noexcept;
    // Consumes the session, including rejection and no-op.
    MmsResult Apply(const std::array<float, 7>&) noexcept;
};
// Owned camera-pose delta, separate from camera keys and curve authoring.
// A successful no-op leaves out empty. The app outlives the record/history.
MmsResult PlanCameraRotationReset(MMDApp&, std::shared_ptr<HistoryChange>& out) noexcept;
bool TryResetBoundCameraRotation(MMDApp*) noexcept;
// Fields 0..2 position, 3..5 rotation, 6 distance. Main/floating panels share
// this authoring route; true also consumes rejection and no-op while bound.
bool TryResetBoundCameraField(MMDApp*, unsigned field, bool activateCameraTrack = true) noexcept;
// Authored units: position, radians, signed camera distance. Reports committed
// changes separately from no-op; notifications cannot turn a commit into failure.
MmsResult ApplyBoundCameraField(MMDApp&, unsigned field, float value, bool& changed,
    bool activateCameraTrack = true) noexcept;
// Fields 0..2 color (authored float), 3..5 direction. CPU mirror publication
// is atomic; device and readout updates happen only after journal publication.
MmsResult ApplyBoundLightField(MMDApp&, unsigned field, float value, bool& changed) noexcept;
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
