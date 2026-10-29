#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
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
// Density, not texture dimensions, is the invariant when the box expands.
inline unsigned ShadowSizeForDensity(float width,float density,unsigned minimum,unsigned maximum) {
    unsigned size=(std::min)(minimum,maximum);
    const double required=double(width)*density;
    if(!std::isfinite(required))return maximum;
    while(size<maximum && double(size)+1e-4<required)size=(std::min)(maximum,size*2);
    return size;
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
// Compress the slider's world-density range into a screen-pixel budget when
// zooming in. Using the linear ratio would allow nearly nine times larger
// screen texels at 8875 than at 0, even after dynamic precision took over.
// The fourth root retains slider ordering without waiting for an extreme zoom.
inline float ShadowScreenQuality(float referenceDensity,float finestReferenceDensity) {
    if(!std::isfinite(referenceDensity) || referenceDensity<=0 ||
       !std::isfinite(finestReferenceDensity) || finestReferenceDensity<=0)return 0.f;
    return float(std::sqrt(std::sqrt((std::min)(1.0,double(referenceDensity)/finestReferenceDensity))));
}
// Only maps confined to the close regions may exceed fixed precision. Distant
// maps follow a smooth minimum of the fixed baseline and screen demand. This
// makes finer presets shed more excess sampling at distance without reversing
// their absolute quality order. Every object shares these camera-depth regions.
// Distant silhouettes use a wider screen footprint; comparison filtering
// reconstructs their coverage. The 25% demand margin remains in effect.
inline constexpr float kShadowFarPixelBudget=3.f;
inline constexpr float kShadowCloseBoost=0.8f;
inline float ShadowLayerDensity(float referenceDensity,float finestReferenceDensity,
    float projectedPixels,float depth,bool orthographic,bool dynamic,bool closeLayer,
    float maximumDensity=(std::numeric_limits<float>::infinity)()) {
    if(!dynamic)return referenceDensity;
    const float screen=DynamicShadowMapDemand(projectedPixels,1.f,depth,orthographic);
    if(!std::isfinite(screen) || screen<=0 || !std::isfinite(finestReferenceDensity) || finestReferenceDensity<=0)
        return referenceDensity;
    const double base=referenceDensity;
    const double footprint=double(screen)*ShadowScreenQuality(referenceDensity,finestReferenceDensity)/
        (closeLayer?1.0:double(kShadowFarPixelBudget));
    const double high=(std::max)(base,footprint),low=(std::min)(base,footprint);
    if(!(high>0))return referenceDensity;
    const double ratio2=(low/high)*(low/high);
    const double norm=std::sqrt(std::sqrt(1.0+ratio2*ratio2));
    // Both branches are smooth at equal demand. The far branch can never
    // exceed either budget, including where a coarse preset meets the screen.
    if(!closeLayer)return float(low/norm);
    // Reduce the additional sampling, not the fixed slider baseline. Apply the
    // device/box capacity first so a capped close map also receives the change.
    const double capacity=maximumDensity>0?double(maximumDensity):high*norm;
    const double fixed=(std::min)(base,capacity);
    const double boosted=(std::min)(high*norm,capacity);
    return float(fixed+double(kShadowCloseBoost)*(boosted-fixed));
}

// Keep the finer map fully weighted while the neighbouring coarse map still
// magnifies its texels on screen. Use the density after fitting to the actual
// allocation, not its requested size. Keep a 1.5x logarithmic fade, or the
// original interval if that was narrower, even when hardware limits precision.
inline float ShadowScreenBlendStartForDemand(float nearDepth,float farDepth,float fixedStart,
    float coarseDensity,float screen,bool orthographic) {
    if(!std::isfinite(coarseDensity) || coarseDensity<=0 || farDepth<=nearDepth)return fixedStart;
    if(!std::isfinite(screen) || screen<=0)return fixedStart;
    const float latest=(std::max)(fixedStart,farDepth/1.5f);
    const float t=(std::clamp)((screen/coarseDensity-1.f)*4.f,0.f,1.f);
    const float requested=orthographic?fixedStart+(latest-fixedStart)*t*t*(3.f-2.f*t):screen/coarseDensity;
    return (std::clamp)(requested,fixedStart,latest);
}
inline float ShadowScreenBlendStart(float nearDepth,float farDepth,float fixedStart,
    float coarseDensity,float referenceDensity,float finestReferenceDensity,
    float projectedPixels,bool orthographic) {
    const float screen=DynamicShadowMapDemand(projectedPixels,1.f,1.f,orthographic)*
        ShadowScreenQuality(referenceDensity,finestReferenceDensity);
    return ShadowScreenBlendStartForDemand(nearDepth,farDepth,fixedStart,coarseDensity,screen,orthographic);
}

inline unsigned ShadowTextureSizeForDensity(float width,float density,unsigned maximum) {
    // Two spare texels let the final world grid snap without clipping the
    // requested box or subtly lowering an otherwise attainable reference.
    return ShadowSizeForDensity(width+2.f/density,density,512u,maximum);
}
// Fade over the interval between adjacent camera-depth regions, instead of
// holding a level until the last 35% of its box. Allocation demand is separate.
inline float ShadowTransitionFraction(float nearDepth,float startDepth,float farDepth) {
    if(!std::isfinite(nearDepth) || !std::isfinite(startDepth) || !std::isfinite(farDepth) || farDepth<=nearDepth)
        return .35f;
    return (std::clamp)((farDepth-startDepth)/(farDepth-nearDepth),.001f,1.f);
}
inline float ShadowTransitionLogRatio(float startDepth,float farDepth) {
    if(!std::isfinite(startDepth) || !std::isfinite(farDepth) || startDepth<=0 || farDepth<=startDepth)
        return 0.f;
    return (std::clamp)(1.f-startDepth/farDepth,0.f,.9999f);
}
// Coverage size that belongs to a quality tier: the coverage pass runs at half
// the detail tier, with the existing 512 minimum.
inline unsigned ShadowCoverageSize(unsigned quality) {
    return (std::max)(512u, quality / 2u);
}
// Dynamic precision only raises the selected quality, never lowers it: the
// same slider position has to keep at least the precision of a character-sized
// scene, and the nearest content (including content close to the camera) must
// not drop below the slider setting. One tier is added once the selected
// coverage would be 2.5 times coarser than the screen needs. The return
// threshold sits closer to the selected level so a camera resting on a
// boundary does not reallocate every frame. One tier changes per frame at
// most, so a fast zoom cannot jump two octaves.
inline unsigned DynamicShadowQualitySize(float requiredCoverage, unsigned selected,
                                         unsigned previous, unsigned maximum) {
    maximum = (std::min)(8192u, maximum);
    if (maximum < 512) return maximum;
    selected = (std::min)(maximum, (std::max)(512u, selected));
    const unsigned upper = (std::min)(maximum, selected * 2);
    if (!std::isfinite(requiredCoverage) || requiredCoverage <= 0) return selected;
    unsigned quality = previous >= 512 ? previous : selected;
    quality = (std::min)(upper, (std::max)(selected, quality));
    const float coverage = float(ShadowCoverageSize(selected));
    if (quality > selected) {
        if (requiredCoverage < coverage * 1.6f) quality = selected;
    } else if (quality < upper && requiredCoverage > coverage * 2.5f) {
        quality *= 2;
    }
    return (std::min)(upper, (std::max)(selected, quality));
}
}
