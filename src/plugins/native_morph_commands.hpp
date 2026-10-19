#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
#include <mms/plugin.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Authored weight, not a percentage. Internal UI-thread edit; no key sampling.
MmsResult ApplyBoundMorphValue(MMDApp&, unsigned modelSlot, unsigned morph, float value, bool& changed) noexcept;
// Consumes bound UI notifications, including rejected edits (no legacy fallback).
bool TryCommitBoundMorphSlider(MMDApp*, HWND slider) noexcept;
void RefreshBoundMorphReadout(MMDApp&) noexcept;
}
#endif
