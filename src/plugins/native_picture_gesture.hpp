#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <Windows.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
bool PictureGestureActive(const MMDApp&) noexcept;
std::uint64_t PictureGestureSerial(const MMDApp&) noexcept;
int PictureGestureOperation(const MMDApp&) noexcept;
HWND PictureGestureOwner(const MMDApp&) noexcept;
MmsResult BeginPictureGesture(MMDApp&, SharedHistory&, HWND, int operation) noexcept;
// No callbacks, COM or UI work. Failure does not write a partial sample; the
// input owner must cancel the gesture before processing further input.
MmsResult PreviewPictureGesture(MMDApp&, std::int32_t x, std::int32_t y, float scale) noexcept;
MmsResult EndPictureGesture(MMDApp&, bool& changed) noexcept;
// True means an active session was consumed, including a retired media domain
// into which no rollback is permitted. Never touches a foreign field author.
bool CancelPictureGesture(MMDApp&) noexcept;
void RefreshPictureGesture(MMDApp&) noexcept;
}
#endif
