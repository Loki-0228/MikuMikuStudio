#pragma once
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <cmath>
#include <cstring>

namespace mikumikustudio::bone_key_sampling {
// Shared native direct-seek math. Preserve operation order for VMD parity.
inline float Ease(const mdl::BoneKey& key, unsigned channel, float time) {
    // CurveTime is the same architecture-specific byte Bezier as BoneEase,
    // and accepts the private target bytes without borrowing a live model.
    return bone_scale::CurveTime({key.interpolation[channel], key.interpolation[channel + 4],
        key.interpolation[channel + 8], key.interpolation[channel + 12]}, time);
}
inline void SeekSlerp(const float* previous, const float* next, float time, float* out) noexcept {
    // Keep the direct-seek operation order. The playback routine uses a
    // reciprocal multiply instead of this divide and is not interchangeable.
#if defined(_M_IX86)
    const float dot = float(double(previous[0]) * next[0] + double(previous[1]) * next[1] +
        double(previous[2]) * next[2] + double(previous[3]) * next[3]);
    if (float(1.0 - double(dot) * dot) == 0.0f) {
        std::memcpy(out, previous, 4 * sizeof(float)); return;
    }
    const float clamped = dot > 1.0f ? 0.999999f : dot < -1.0f ? -0.999999f : dot;
    const float theta = float(std::acos(double(clamped)));
    const bool reverse = double(theta) > double(3.141592f) * 0.5 && clamped < 0;
    const float angle = reverse ? float(std::acos(-double(clamped))) : theta;
    const float sine = float(std::sin(double(angle)));
    const float a0 = float((1.0 - double(time)) * angle), a1 = float(double(time) * angle);
    const float w0 = float(double(float(std::sin(double(a0)))) / sine);
    const float w1 = float(double(float(std::sin(double(a1)))) / sine);
    for (unsigned n = 0; n < 4; ++n)
        out[n] = reverse ? float(double(previous[n]) * w0 - double(w1) * next[n]) :
            float(double(previous[n]) * w0 + double(w1) * next[n]);
#else
    const float dot = previous[0] * next[0] + previous[1] * next[1] +
        previous[2] * next[2] + previous[3] * next[3];
    if (1.0f - dot * dot == 0.0f) {
        std::memcpy(out, previous, 4 * sizeof(float)); return;
    }
    const float clamped = dot > 1.0f ? 0.999999f : dot < -1.0f ? -0.999999f : dot;
    const float theta = std::acos(clamped);
    const bool reverse = theta > 1.570796012878418f && clamped < 0;
    const float angle = reverse ? std::acos(-clamped) : theta;
    const float sine = std::sin(angle);
    const float w0 = std::sin((1.0f - time) * angle) / sine;
    const float w1 = std::sin(time * angle) / sine;
    for (unsigned n = 0; n < 4; ++n)
        out[n] = reverse ? previous[n] * w0 - w1 * next[n] : previous[n] * w0 + w1 * next[n];
#endif
}
}
