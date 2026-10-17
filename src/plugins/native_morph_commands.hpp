#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <mms/plugin.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Authored weight, not a percentage. Internal UI-thread edit; no key sampling.
MmsResult ApplyBoundMorphValue(MMDApp&, unsigned modelSlot, unsigned morph, float value, bool& changed) noexcept;
void RefreshBoundMorphReadout(MMDApp&) noexcept;
}
#endif
