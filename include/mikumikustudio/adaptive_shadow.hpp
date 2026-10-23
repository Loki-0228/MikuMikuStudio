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
// Precision changes sample density only. All distance layers retain the same
// geometry and blend distances at every slider position (0 is finest).
inline unsigned ShadowQualityMapSize(float range, unsigned maximum) {
    maximum=(std::min)(8192u,maximum);
    if(maximum<512)return maximum;
    const float quality=(std::clamp)(std::isfinite(range)?range/.1f:0.f,0.f,1.f);
    const unsigned tier=quality>.75f?0u:quality>.5f?1u:quality>.25f?2u:3u;
    return (std::max)(512u,maximum>>tier);
}
inline float ShadowSamplingFactor(float range) {
    // The fine half of the shared range control retains the former finest
    // sampling demand (4x). There is no separately stored precision value.
    if (!std::isfinite(range)) return 1.f;
    return std::exp2((std::clamp)(40.f * range - 2.f, 0.f, 2.f));
}
inline unsigned FittedShadowResolutionLimit(unsigned reference, float requestedScale,
                                            float fittedScale, unsigned hardwareLimit) {
    // Widening coverage must not silently reduce the requested texels/unit.
    // Compensate for the full coverage ratio up to the existing 8192 safety
    // ceiling and device limit. Allocation failure still falls back.
    unsigned limit = 512;
    const unsigned maximum = (std::min)(8192u, hardwareLimit);
    while (limit <= maximum / 2) limit *= 2;
    if (maximum < 512) return maximum;
    unsigned size = (std::min)(reference, limit);
    const float ratio = (std::max)(1.f, requestedScale / fittedScale);
    while (size < limit && size * 1.001f < reference * ratio) size *= 2;
    return (std::min)(size, limit);
}
// Dynamic precision: size the coverage map so one shadow texel matches one
// screen pixel at the picture's focus depth, with a 25% margin. Shadow texel
// density falls with the inverse of that depth, so the demand doubles whenever
// the camera moves twice as close; resolution tiers follow the logarithmic
// curve that this inverse relation defines.
inline float DynamicShadowMapDemand(float projectedPixels, float coverageWidth,
                                    float focusDepth, bool orthographic) {
    if (!std::isfinite(coverageWidth) || coverageWidth <= 0) return 0.f;
    if (orthographic) return 1.25f * projectedPixels * coverageWidth * .5f;
    if (!std::isfinite(focusDepth) || focusDepth <= 0) return 0.f;
    return 1.25f * projectedPixels * coverageWidth / (2.f * focusDepth);
}
// Coverage size that belongs to a quality tier: the coverage pass runs at half
// the detail tier, with the existing 512 minimum.
inline unsigned ShadowCoverageSize(unsigned quality) {
    return (std::max)(512u, quality / 2u);
}
// Dynamic precision offsets the selected quality by at most one tier. The
// demand curve is logarithmic: one tier is added once the selected coverage
// would be 2.5 times coarser than the screen needs, and one tier is removed
// once it is 2.5 times finer. The return thresholds sit closer to the selected
// level so a camera resting on a boundary does not reallocate every frame. One
// tier changes per frame at most, so a fast zoom cannot jump two octaves.
inline unsigned DynamicShadowQualitySize(float requiredCoverage, unsigned selected,
                                         unsigned previous, unsigned maximum) {
    maximum = (std::min)(8192u, maximum);
    if (maximum < 512) return maximum;
    selected = (std::min)(maximum, (std::max)(512u, selected));
    const unsigned lower = (std::max)(512u, selected / 2);
    const unsigned upper = (std::min)(maximum, selected * 2);
    if (!std::isfinite(requiredCoverage) || requiredCoverage <= 0) return selected;
    unsigned quality = previous >= 512 ? previous : selected;
    quality = (std::min)(upper, (std::max)(lower, quality));
    const float coverage = float(ShadowCoverageSize(selected));
    if (quality > selected) {
        if (requiredCoverage < coverage * 1.6f) quality = selected;
    } else if (quality < selected) {
        if (requiredCoverage > coverage * .62f) quality = selected;
    } else if (quality < upper && requiredCoverage > coverage * 2.5f) {
        quality *= 2;
    } else if (quality > lower && requiredCoverage < coverage * .4f) {
        quality /= 2;
    }
    return (std::min)(upper, (std::max)(lower, quality));
}
}
