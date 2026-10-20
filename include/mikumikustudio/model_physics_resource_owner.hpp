#pragma once
#include "build_profile.hpp"
namespace mikumikustudio::model_physics_resource_owner {
#if !MMD_ORIGINAL_PROFILE
// Optional internal owner hook keeps standalone legacy model-disposal targets
// independent of plugin history. Capture installs it; borrowed models still
// fall through to their original disposal until a publication adopts them.
using Dispose = bool (*)(unsigned char*) noexcept;
inline Dispose dispose = nullptr;
inline bool Release(unsigned char* model) noexcept { return dispose && dispose(model); }
#else
inline bool Release(unsigned char*) noexcept { return false; }
#endif
}
