#pragma once
#include <cstdint>
#include <limits>
#include <vector>
#include "mikumikustudio/view_options.hpp"
namespace mikumikustudio::transparent_geometry {
constexpr std::uint32_t kOutsideFrustum = 1;
// Conservative homogeneous half-space mask. A triangle is outside only when
// all three vertices share a bit; boundary and invalid vertices are retained.
unsigned FrustumOutsidePlanes(const double clip[4]) noexcept;
// Projected position; data retains XYZ, normal, UV0 and four extra float4 UVs.
struct Vertex { double p[3]{}; double inverseW = 1; float data[24]{}; };
struct Triangle {
    Vertex v[3];
    std::uint32_t draw = 0, ordinal = 0;
    std::uint32_t object = 0, flags = 0;
    // MME's _INDEX semantic addresses the original model's vertex history.
    std::uint32_t sourceIndices[3]{};
    // Unclipped face centre depth in a linear camera coordinate (sum of Z).
    // NaN retains the projected-depth contract for geometry-only callers.
    double linearDepth = std::numeric_limits<double>::quiet_NaN();
};
struct Statistics {
    std::size_t inputTriangles = 0, outputTriangles = 0, splitCount = 0;
    // Used depth buckets: Low 16, Standard 64, High 256; 0 for other tiers.
    std::size_t bucketCount = 0;
    std::size_t offscreenTriangles = 0;
    bool limited = false;
};
// Input is grouped in object order. Bucket tiers retain whole source faces;
// native Maximum receives faces clipped to the D3D frustum.
// Scope is independent of strength. With crossObjects false every strength
// retains authored object order; true orders the whole input set.
// Full input determines bucket depth bounds, including offscreen faces. Bucket
// tiers sort only visible faces; retainOutside preserves offscreen submissions
// in their original slots for unknown MME vertex shaders.
std::vector<Triangle> Order(const std::vector<Triangle>& input,
                           TransparentSortPrecision precision, Statistics& stats,
                           bool crossObjects = false, bool splitIntersections = true,
                           bool retainOutside = false);
// Reduce draw changes in an already ordered sequence. Every pair that can
// cover the same pixel keeps its original order, including within one draw.
// Failure, an exhausted work budget, or no reduction leaves input untouched.
// Use only for native shaders whose output stays inside the source triangle.
bool BatchDraws(std::vector<Triangle>& ordered, unsigned viewportWidth,
                unsigned viewportHeight, std::size_t budget = 2000000);
} // namespace mikumikustudio::transparent_geometry
