#pragma once
#include <cstdint>

namespace mikumikustudio {
class MMDApp;
namespace plugins::background_identity { class Mutation; }
// Internal PMM entry: the caller owns the live outer project-media scope.
// Empty paths clear the project background. Failure clears only while this
// preparation still owns the same application/scene; no dirty/history change.
// resolutionFailed distinguishes a missing named resource from an empty slot.
bool LoadProjectBackgroundPicture(MMDApp* app, const wchar_t* path,
    std::int32_t x, std::int32_t y, float scale, bool visible,
    const plugins::background_identity::Mutation& projectScope, std::uint64_t expectedScene,
    bool resolutionFailed = false);
}
