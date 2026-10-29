#pragma once

#include <cstdint>

namespace mikumikustudio {
inline constexpr std::uint16_t kAutoTransparentMaterialSortingMenuId = 303;
// Five-tier test format; no migration from the earlier six-tier experiments.
enum class TransparentSortPrecision : std::uint8_t { Low = 0, Standard = 1, High = 2, Maximum = 3, Off = 4 };
// Strength and scope are independent: Low uses 16 buckets, Standard 64,
// High uses 256 buckets, Maximum resolves intersections where supported and
// otherwise sorts all whole triangles by depth without depth buckets.
// New projects start Off and keep authored object order.
inline constexpr auto kDefaultTransparentSortPrecision = TransparentSortPrecision::Off;
inline constexpr std::uint16_t kTransparentSortPrecisionMenuFirst = 304;
inline constexpr std::uint16_t kTransparentSortPrecisionMenuLast = 307;
inline constexpr std::uint16_t kTransparentSortPrecisionMenuOff = 308;
inline constexpr std::uint16_t kTransparentSortCrossObjectsMenuId = 309;
class MMDApp;
void RefreshTransparentSortPrecisionMenu(MMDApp* app);
}
