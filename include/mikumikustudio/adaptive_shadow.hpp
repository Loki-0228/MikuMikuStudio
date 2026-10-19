#pragma once
#include <algorithm>
#include <cmath>
#include "mikumikustudio/native_shadow_projection.hpp"

namespace mikumikustudio {
class MMDApp;
void LoadShadowResolutionPreference(MMDApp* app);
void SaveShadowResolutionPreference(MMDApp* app);
std::array<float, 16> UpdateAdaptiveShadowMap(MMDApp* app, const float frame[16]);
int ShadowSliderPosition(MMDApp* app);
void SetShadowSliderPosition(MMDApp* app, int position);
inline float ShadowSamplingFactor(float range) {
    // The fine half of the shared range control retains the former finest
    // sampling demand (4x). There is no separately stored precision value.
    if (!std::isfinite(range)) return 1.f;
    return std::exp2((std::clamp)(40.f * range - 2.f, 0.f, 2.f));
}
inline unsigned FittedShadowResolutionLimit(unsigned reference, float requestedScale,
                                            float fittedScale, unsigned hardwareLimit) {
    // Widening coverage must not silently reduce the requested texels/unit.
    // Permit one extra size tier (4096/8192), retaining the fine-shadow
    // quality distinction. Allocation failure still falls back.
    unsigned limit = 512;
    const unsigned maximum = (std::min)({8192u, reference * 2, hardwareLimit});
    while (limit <= maximum / 2) limit *= 2;
    if (maximum < 512) return maximum;
    unsigned size = (std::min)(reference, limit);
    const float ratio = (std::max)(1.f, requestedScale / fittedScale);
    while (size < limit && size * 1.001f < reference * ratio) size *= 2;
    return (std::min)(size, limit);
}
inline float RequiredShadowMapSizeForScale(float lateralScale, float projectedPixels,
                                           float nearestDepth, bool orthographic) {
    if (!std::isfinite(nearestDepth) || nearestDepth <= 0) return 512.f;
    // Full map width = 2 / lateralScale; screen pixels per world unit =
    // projectedPixels / (2 * depth). Keep a 25% sampling margin.
    const float depth = orthographic ? 1.f : (std::max)(.0001f, nearestDepth);
    return 1.25f * projectedPixels / (lateralScale * depth);
}
inline float RequiredShadowMapSize(float range, int mode, float projectedPixels,
                                   float nearestDepth, bool orthographic) {
    return RequiredShadowMapSizeForScale(NativeShadowLateralScale(range, mode),
        projectedPixels, nearestDepth, orthographic);
}
// Choose power-of-two maps from the projected density of visible geometry.
// Different grow/shrink thresholds prevent reallocating around a boundary.
inline unsigned AdaptiveShadowSize(float required, unsigned current, unsigned limit) {
    limit = (std::max)(512u, limit);
    if (!std::isfinite(required) || required <= 0) return limit;
    unsigned size = (std::min)(limit, (std::max)(512u, current));
    while (size < limit && required > size * 1.1f) size *= 2;
    while (size > 512 && required < size * 0.4f) size /= 2;
    return (std::min)(size, limit);
}
}
