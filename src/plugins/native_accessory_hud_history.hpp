#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Six native accessory pose axes, only in camera-panel accessory coordinates.
// Samples still use ModeCameraAdjust/ModeAngleAdjust and one shared journal.
bool AccessoryHudOperation(MMDApp&, int operation) noexcept;
MmsResult BeginAccessoryHudGesture(MMDApp&, SharedHistory&, HWND owner, int operation) noexcept;
bool AccessoryHudGestureActive(const MMDApp&) noexcept;
bool AllowAccessoryHudPreview(MMDApp&, int operation) noexcept;
bool AcceptAccessoryHudPreview(MMDApp&) noexcept;
// Keep the current sample's exact gesture alive across its native EDIT echo.
std::shared_ptr<const HistoryChange> AccessoryHudPreviewSource(MMDApp&) noexcept;
MmsResult EndAccessoryHudGesture(MMDApp&, bool& changed) noexcept;
bool CancelAccessoryHudGesture(MMDApp&) noexcept;
// The viewport lifecycle dispatcher calls this beside HandleCameraHudMessage.
// Mouse-up samples the live OS cursor after centered warps, never stale lParam.
bool HandleAccessoryHudMessage(MMDApp&, HWND, UINT, WPARAM, LPARAM) noexcept;
}
#endif
