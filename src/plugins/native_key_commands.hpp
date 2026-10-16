#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// True means the explicit migration binding owns the command, including
// rejection and no-op. Call before any old-ring write or selection fallback.
bool TryDeleteBoundKeys(MMDApp*, bool selectedAtCurrentFrame = false) noexcept;
bool TryScaleBoundBoneRange(MMDApp*, HWND dialog) noexcept;
}
#endif
