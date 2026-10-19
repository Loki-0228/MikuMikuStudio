#pragma once

#include <cstdint>

namespace mikumikustudio {
inline constexpr std::uint16_t kAutoTransparentMaterialSortingMenuId = 303;
// Five-tier test format; no migration from the earlier six-tier experiments.
enum class TransparentSortPrecision : std::uint8_t { Low = 0, Standard = 1, High = 2, Maximum = 3, Off = 4 };
// Low: 16 depth buckets; Standard: 64; High: stable triangle depth order.
// Maximum retains pixel peeling and its existing BSP fallback. Persisted ids
// remain unchanged. New projects start with sorting disabled.
inline constexpr auto kDefaultTransparentSortPrecision = TransparentSortPrecision::Off;
inline constexpr std::uint16_t kTransparentSortPrecisionMenuFirst = 304;
inline constexpr std::uint16_t kTransparentSortPrecisionMenuLast = 307;
inline constexpr std::uint16_t kTransparentSortPrecisionMenuOff = 308;
class MMDApp;
void RefreshTransparentSortPrecisionMenu(MMDApp* app);
}
