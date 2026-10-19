#pragma once
#include <cstdint>
#include <vector>
#include "mikumikustudio/view_options.hpp"
namespace mikumikustudio::transparent_geometry {
// Projected position; data retains XYZ, normal, UV0 and four extra float4 UVs.
struct Vertex { double p[3]{}; double inverseW = 1; float data[24]{}; };
struct Triangle { Vertex v[3]; std::uint32_t draw = 0; std::uint32_t ordinal = 0; };
struct Statistics {
    std::size_t inputTriangles = 0, outputTriangles = 0, splitCount = 0;
    // Used depth buckets: Low has 16, Standard has 64; 0 for other tiers.
    std::size_t bucketCount = 0;
    bool limited = false;
};
// Input has already been clipped to the D3D frustum. Order all materials together.
// Keep every nondegenerate input piece, including when a precision budget is hit.
std::vector<Triangle> Order(const std::vector<Triangle>& input,
                           TransparentSortPrecision precision, Statistics& stats);
// Reduce draw changes in an already ordered sequence. Every pair that can
// cover the same pixel keeps its original order, including within one draw.
// Failure, an exhausted work budget, or no reduction leaves input untouched.
// Use only for native shaders whose output stays inside the source triangle.
bool BatchDraws(std::vector<Triangle>& ordered, unsigned viewportWidth,
                unsigned viewportHeight, std::size_t budget = 2000000);
} // namespace mikumikustudio::transparent_geometry
