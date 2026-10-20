#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::audio_menu {
// Independent of command 297, which only mutes the current audio.
inline constexpr unsigned kRemoveCommand = 32815;
void EnsureItem(MMDApp*) noexcept;
bool HandleCommand(MMDApp*, unsigned command) noexcept;
}
#endif
