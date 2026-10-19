#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// True means the explicit migration binding owns the command, including
// rejection and no-op. Call before any old-ring write or selection fallback.
// Camera mode deletes the five global key families in one explicit node;
// current-frame fallback is limited to selected rows with no explicit marks.
bool TryDeleteBoundKeys(MMDApp*, bool selectedAtCurrentFrame = false) noexcept;
bool TryScaleBoundBoneRange(MMDApp*, HWND dialog) noexcept;
bool TryApplyBoundInterpolation(MMDApp*, HWND window, bool reset) noexcept;
// Current-state registration. lane is the native morph selector row (0..3),
// not its control ID. The bound route consumes invalid/busy/no-op requests.
bool TryRegisterBoundMorph(MMDApp*, unsigned lane) noexcept;
bool TryRegisterBoundDisplay(MMDApp*) noexcept;
}
#endif
