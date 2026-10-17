#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Camera right/middle drags reserve once, preview in place, then publish once.
// No history/storage lock is retained across window messages.
MmsResult BeginViewportCameraGesture(MMDApp&, SharedHistory&, HWND owner, UINT button) noexcept;
bool AllowViewportCameraPreview(MMDApp&, UINT button) noexcept;
bool AcceptViewportCameraPreview(MMDApp&) noexcept;
MmsResult EndViewportCameraGesture(MMDApp&, bool& changed) noexcept;
bool CancelViewportCameraGesture(MMDApp&) noexcept;
// A wheel message is one adjustment. Main/float legacy math differs, and
// parented main-window cameras adjust target Z instead of distance.
MmsResult AdjustViewportCameraWheel(MMDApp&, SharedHistory&, std::int16_t delta,
    bool floating, bool& changed) noexcept;
bool HandleBoundViewportMessage(MMDApp&, HWND, UINT, WPARAM, LPARAM) noexcept;
}
#endif
