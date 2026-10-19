#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <windows.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Explicit migration binding only. A bound unsupported/rejected operation
// returns true to prevent a partial edit through the legacy undo rings.
bool TryPasteBoundKeys(MMDApp*, HWND, bool mirror) noexcept;
}
#endif
