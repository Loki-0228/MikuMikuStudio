#pragma once

#include <array>
#include <cstdint>

namespace mikumikustudio {

struct GroundVertex {
    float x, y, z;
    std::uint32_t color;
};
static_assert(sizeof(GroundVertex) == 16);

// The grid uses the same positions and packed D3DCOLOR values in both backends.
inline constexpr auto kGroundGrid = [] {
    constexpr std::uint32_t blue = 0xFF0000FFu;
    constexpr std::uint32_t red = 0xFFFF0000u;
    constexpr std::uint32_t green = 0xFF00FF00u;
    constexpr std::uint32_t gray = 0xFFB4B4B4u;
    std::array<GroundVertex, 90> vertices{{
        {0.0f, 0.1f, 0.0f, blue}, {0.0f, 0.1f, -65.0f, blue},
        {0.0f, 0.1f, 0.0f, red}, {65.0f, 0.1f, 0.0f, red},
        {0.0f, 0.1f, 0.0f, green}, {0.0f, 65.0f, 0.0f, green},
        {0.0f, 0.1f, 0.0f, gray}, {0.0f, 0.1f, 50.0f, gray},
        {0.0f, 0.1f, 0.0f, gray}, {-50.0f, 0.1f, 0.0f, gray},
    }};
    for (int i = 1; i <= 10; ++i) {
        const float p = i * 5.0f, n = -p;
        auto* line = vertices.data() + 10 + 8 * (i - 1);
        line[0] = {p, 0.1f, -50.0f, gray};
        line[1] = {p, 0.1f, 50.0f, gray};
        line[2] = {n, 0.1f, -50.0f, gray};
        line[3] = {n, 0.1f, 50.0f, gray};
        line[4] = {-50.0f, 0.1f, p, gray};
        line[5] = {50.0f, 0.1f, p, gray};
        line[6] = {-50.0f, 0.1f, n, gray};
        line[7] = {50.0f, 0.1f, n, gray};
    }
    return vertices;
}();

}
