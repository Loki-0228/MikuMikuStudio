#pragma once
#include <cstdint>
#include <limits>
#include <vector>
#include "mikumikustudio/view_options.hpp"
namespace mikumikustudio::transparent_geometry {
// Projected position; data retains XYZ, normal, UV0 and four extra float4 UVs.
struct Vertex { double p[3]{}; double inverseW = 1; float data[24]{}; };
struct Triangle {
    Vertex v[3];
    std::uint32_t draw = 0, ordinal = 0;
    std::uint32_t object = 0, flags = 0;
    // Original model indices, preserved when a policy does not split faces.
    // After splitting these identify the source face, not generated vertices.
    std::uint32_t sourceIndices[3]{};
    // Explicitly initialized alignment slot: the legacy plan cache compares
    // complete Triangle bytes, so adding implicit padding would make it noisy.
    std::uint32_t reserved = 0;
    double linearDepth = std::numeric_limits<double>::quiet_NaN();
};
struct Statistics {
    std::size_t inputTriangles = 0, outputTriangles = 0, splitCount = 0;
    // Used buckets: legacy Low/Standard have 16/64; policy High has 256 too.
    std::size_t bucketCount = 0;
    std::size_t offscreenTriangles = 0;
    bool limited = false;
};
// Input has already been clipped to the D3D frustum. Order all materials together.
// Keep every nondegenerate input piece, including when a precision budget is hit.
std::vector<Triangle> Order(const std::vector<Triangle>& input,
                           TransparentSortPrecision precision, Statistics& stats);
// Explicit policy for the pending renderer migration. Existing Order callers
// keep their projected-depth, global ordering contract until migrated together.
constexpr std::uint32_t kOutsideFrustum = 1;
unsigned FrustumOutsidePlanes(const double clip[4]) noexcept;
struct OrderPolicy {
    bool crossObjects = false;
    bool splitIntersections = true;
    bool retainOutside = false;
};
// Input is grouped in authored object order. Scope=false preserves contiguous
// object groups; true orders the whole set. Low/Standard/High use 16/64/256
// stable buckets with whole-face linearDepth (projected sum when not finite).
// All valid source faces in each scope set its depth bounds. Only visible
// bucket faces reorder; retainOutside keeps outside faces in their source slots.
// Maximum uses projected-depth BSP, or exact whole-face order when splitting
// is disabled. BSP input must already be clipped. Whole-face Maximum accepts
// valid projected source faces without clipping, retaining their indices and
// ignoring outside flags/retainOutside. Off returns input unchanged, including
// otherwise invalid faces.
// Statistics publish only after success. Allocation failures propagate, input
// and caller statistics remain unchanged. This is internal C++, not plugin ABI.
std::vector<Triangle> OrderWithPolicy(const std::vector<Triangle>& input,
    TransparentSortPrecision precision, Statistics& stats, const OrderPolicy& policy);
// Reduce draw changes in an already ordered sequence. Every pair that can
// cover the same pixel keeps its original order, including within one draw.
// Failure, an exhausted work budget, or no reduction leaves input untouched.
// Use only for native shaders whose output stays inside the source triangle.
bool BatchDraws(std::vector<Triangle>& ordered, unsigned viewportWidth,
                unsigned viewportHeight, std::size_t budget = 2000000);
} // namespace mikumikustudio::transparent_geometry
