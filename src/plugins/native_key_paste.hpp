#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <windows.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Explicit migration binding only. A bound unsupported/rejected operation
// returns true to prevent a partial edit through the legacy undo rings.
bool TryPasteBoundKeys(MMDApp*, HWND, bool mirror) noexcept;
}
#endif
