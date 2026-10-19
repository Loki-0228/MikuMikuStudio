#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Internal migration for the six camera HUD pose axes (operations 15..20).
// The native ModeCameraAdjust/ModeAngleAdjust bodies still compute samples.
// All other HUD writers remain behind their existing migration gates.
bool CameraHudOperation(MMDApp&, int operation) noexcept;
MmsResult BeginCameraHudGesture(MMDApp&, SharedHistory&, HWND owner, int operation) noexcept;
bool CameraHudGestureActive(const MMDApp&) noexcept;
bool AllowCameraHudPreview(MMDApp&, int operation) noexcept;
bool AcceptCameraHudPreview(MMDApp&) noexcept;
MmsResult EndCameraHudGesture(MMDApp&, bool& changed) noexcept;
bool CancelCameraHudGesture(MMDApp&) noexcept;
// Cancels on focus/capture/window lifecycle boundaries; includes the final
// coalesced mouse-up sample, then leaves native mouse-up housekeeping intact.
bool HandleCameraHudMessage(MMDApp&, HWND, UINT, WPARAM, LPARAM) noexcept;
}
#endif
