#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::picture_menu {
inline constexpr unsigned kRemoveCommand = 32816;
bool EnsureItem(MMDApp*) noexcept;
bool HandleCommand(MMDApp*, unsigned command) noexcept;
}
#endif
