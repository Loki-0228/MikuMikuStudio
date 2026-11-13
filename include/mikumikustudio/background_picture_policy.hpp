#pragma once
#include "system_settings.hpp"
#include <cstdint>

namespace mikumikustudio::media {
constexpr std::uint64_t kDefaultPictureSourceLimit = 128ull * 1024 * 1024;
// Reads sections.resources.backgroundPictureSourcesMiB in data/settings.json.
// Missing section/field uses the default. Invalid/busy/unreadable settings
// leave output unchanged; callers must not silently relax the active limit.
system_settings::Result ReadBackgroundPictureSourceLimit(
    const system_settings::Store&, std::uint64_t& output) noexcept;
system_settings::Result ReadBackgroundPictureSourceLimit(std::uint64_t& output) noexcept;
}
