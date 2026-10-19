#pragma once

#ifndef MMD_ORIGINAL_PROFILE
#define MMD_ORIGINAL_PROFILE 0
#endif

namespace mikumikustudio::build_profile {
inline constexpr bool original = MMD_ORIGINAL_PROFILE != 0;
}
