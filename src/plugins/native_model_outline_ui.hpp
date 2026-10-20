#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Native modeless property controller; no public plugin ABI or HWND ownership.
bool InitializeBoundModelOutline(MMDApp&,HWND) noexcept;
bool ControlledModelOutlineDialog(HWND) noexcept;
bool HandleBoundModelOutlineDialog(MMDApp&,HWND,UINT,WPARAM,LPARAM,INT_PTR&) noexcept;
bool RefreshBoundModelOutline(MMDApp&) noexcept;
bool CancelBoundModelOutlineGesture(MMDApp&) noexcept;
}
#endif
