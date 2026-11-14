#pragma once

namespace mikumikustudio::menu_command_ids {
// WM_COMMAND carries a 16-bit ID. All limits here are exclusive.
inline constexpr unsigned kNativeAbiFirst = 0xa000;
inline constexpr unsigned kNativeAbiLimit = 0xc000;
inline constexpr unsigned kLegacyObserverFirst = 0xc000;
inline constexpr unsigned kLegacyObserverLimit = 0xc800;
inline constexpr unsigned kPluginMenuFirst = 0xd000;
inline constexpr unsigned kPluginMenuLimit = 0xf000;

static_assert(kNativeAbiFirst < kNativeAbiLimit &&
              kNativeAbiLimit <= kLegacyObserverFirst &&
              kLegacyObserverFirst < kLegacyObserverLimit &&
              kLegacyObserverLimit <= kPluginMenuFirst &&
              kPluginMenuFirst < kPluginMenuLimit &&
              kPluginMenuLimit <= 0x10000);

constexpr bool IsNativeAbi(unsigned id) noexcept {
    return id >= kNativeAbiFirst && id < kNativeAbiLimit;
}

constexpr bool IsLegacyObserver(unsigned id) noexcept {
    return id >= kLegacyObserverFirst && id < kLegacyObserverLimit;
}
}
