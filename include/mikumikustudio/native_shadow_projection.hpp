#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace mikumikustudio {
struct NativeShadowBounds {
    std::array<float, 3> minimum{INFINITY, INFINITY, INFINITY};
    std::array<float, 3> maximum{-INFINITY, -INFINITY, -INFINITY};
    bool Valid() const { return minimum[0] <= maximum[0]; }
    void Include(const float point[3]) {
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) return;
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = (std::min)(minimum[axis], point[axis]);
            maximum[axis] = (std::max)(maximum[axis], point[axis]);
        }
    }
    std::array<float, 3> Corner(unsigned index) const {
        return {index & 1 ? maximum[0] : minimum[0],
                index & 2 ? maximum[1] : minimum[1],
                index & 4 ? maximum[2] : minimum[2]};
    }
};
// Read every position from the already locked vertex buffer. SIMD compares
// preserve the scalar Include rule: ignore the whole non-finite vertex and
// keep the earlier bit pattern for equal coordinates (including signed zero).
inline NativeShadowBounds NativeShadowVertexBounds(const void* data,
                                                   std::size_t stride,
                                                   std::size_t count) {
    NativeShadowBounds bounds;
    if (!data || stride < 12 || count > (std::numeric_limits<std::size_t>::max)() / stride)
        return bounds;
    const auto* bytes = static_cast<const unsigned char*>(data);
#if defined(_M_X64) || defined(__SSE2__)
    if (stride >= 16) {
        __m128 low = _mm_set1_ps(INFINITY), high = _mm_set1_ps(-INFINITY);
        const __m128 sign = _mm_set1_ps(-0.0f);
        const __m128 largest = _mm_set1_ps((std::numeric_limits<float>::max)());
        for (std::size_t i = 0; i < count; ++i, bytes += stride) {
            const __m128 point = _mm_loadu_ps(reinterpret_cast<const float*>(bytes));
            if ((_mm_movemask_ps(_mm_cmple_ps(_mm_andnot_ps(sign, point), largest)) & 7) != 7)
                continue;
            // MINPS/MAXPS return their second operand on equality.
            low = _mm_min_ps(point, low);
            high = _mm_max_ps(point, high);
        }
        float minimum[4], maximum[4];
        _mm_storeu_ps(minimum, low); _mm_storeu_ps(maximum, high);
        std::memcpy(bounds.minimum.data(), minimum, 12);
        std::memcpy(bounds.maximum.data(), maximum, 12);
        return bounds;
    }
#endif
    for (std::size_t i = 0; i < count; ++i, bytes += stride) {
        float point[3]; std::memcpy(point, bytes, sizeof(point));
        bounds.Include(point);
    }
    return bounds;
}
inline float NativeShadowLateralScale(float range, int mode) {
    const float base = std::isfinite(range) ? (std::max)(range, .00001f) : .01784f;
    return base * (mode == 1 ? 2.f : 3.f);
}
// Preserve the 9.32 sampling grid unless visible geometry needs more coverage.
// An affine transform of homogeneous clip coordinates enlarges the frustum
// without changing the direction of light rays or discarding its mode warp.
inline std::array<float, 16> FitReferenceShadowProjection(
    std::array<float, 16> matrix, const NativeShadowBounds& bounds) {
    if (!bounds.Valid()) return matrix;
    double minimumW = INFINITY;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner);
        minimumW = (std::min)(minimumW, double(matrix[15]) +
            p[0]*double(matrix[3]) + p[1]*double(matrix[7]) + p[2]*double(matrix[11]));
    }
    // Do not let a projective pole cross the model. Reduce only the warp when
    // necessary; move Z with W to preserve its centre plane before fitting.
    if (minimumW < .25) {
        const double factor = .75 / (1.0 - minimumW);
        for (unsigned row = 0; row < 4; ++row) {
            const double oldW = matrix[row*4+3];
            const double newW = oldW*factor + (row == 3 ? 1.0-factor : 0.0);
            matrix[row*4+2] += float(.5*(newW-oldW));
            matrix[row*4+3] = float(newW);
        }
    }
    double lo[3] = {INFINITY, INFINITY, INFINITY};
    double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner);
        double q[4]{};
        for (int a = 0; a < 4; ++a)
            q[a] = matrix[12+a] + p[0]*double(matrix[a]) +
                p[1]*double(matrix[4+a]) + p[2]*double(matrix[8+a]);
        for (int a = 0; a < 3; ++a) {
            lo[a] = (std::min)(lo[a], q[a]/q[3]);
            hi[a] = (std::max)(hi[a], q[a]/q[3]);
        }
    }
    for (int a = 0; a < 3; ++a) {
        const double clipLo = a == 2 ? 0.0 : -1.0;
        const double margin = (std::max)(a == 2 ? .000001 : .0005, (hi[a]-lo[a])*.05);
        const double expandedLo = (std::min)(clipLo, lo[a]-margin);
        const double expandedHi = (std::max)(1.0, hi[a]+margin);
        if (expandedLo == clipLo && expandedHi == 1.0) continue;
        const double scale = (1.0-clipLo)/(expandedHi-expandedLo);
        const double offset = clipLo-expandedLo*scale;
        for (unsigned row = 0; row < 4; ++row)
            matrix[row*4+a] = float(matrix[row*4+a]*scale + matrix[row*4+3]*offset);
    }
    return matrix;
}

// Smallest local light-plane sampling density, including perspective division.
// The adaptive allocator must not treat the warped matrix as orthographic.
inline float MinimumShadowScale(const std::array<float, 16>& m,
                                const NativeShadowBounds& bounds) {
    if (!bounds.Valid()) return .00001f;
    double result = INFINITY;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner);
        double q[4]{};
        for (int a = 0; a < 4; ++a)
            q[a] = m[12+a] + p[0]*double(m[a]) + p[1]*double(m[4+a]) + p[2]*double(m[8+a]);
        if (q[3] <= .00001) continue;
        double xx=0, yy=0, xy=0;
        for (int r=0; r<3; ++r) {
            const double dx=(m[r*4]*q[3]-q[0]*m[r*4+3])/(q[3]*q[3]);
            const double dy=(m[r*4+1]*q[3]-q[1]*m[r*4+3])/(q[3]*q[3]);
            xx+=dx*dx; yy+=dy*dy; xy+=dx*dy;
        }
        const double disc=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy);
        result=(std::min)(result, std::sqrt((std::max)(0.0, (xx+yy-disc)*.5)));
    }
    return std::isfinite(result) ? float((std::max)(.0000001, result)) : .00001f;
}
// Model vertices and accessory world matrices are in scene coordinates. Build
// the directional-light projection there, independently of the orbit rotation.
// The requested range cannot clip rendered geometry. Fit its world-space
// bounds independently of the camera, with room for filtering at every edge.
inline std::array<float, 16> NativeShadowProjection(
    const float focus[3], const float direction[3], float range, int mode,
    const NativeShadowBounds* bounds = nullptr) {
    float z[3] = {direction[0], direction[1], direction[2]};
    float length = std::sqrt(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]);
    if (!std::isfinite(length) || length < 1e-6f) {
        z[0] = 0; z[1] = -1; z[2] = 0; length = 1;
    }
    for (float& value : z) value /= length;
    const bool pole = std::abs(z[2]) > .95f;
    float x[3] = {pole ? z[2] : -z[1], pole ? 0.f : z[0], pole ? -z[0] : 0.f};
    length = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    for (float& value : x) value /= length;
    const float y[3] = {z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0]};
    const float* axes[] = {x, y, z};
    const float base = std::isfinite(range) ? (std::max)(range, .00001f) : .01784f;
    const float lateral = NativeShadowLateralScale(range, mode);
    float scale[] = {lateral, lateral, base * .15f};
    float centre[] = {
        focus[0]*x[0] + focus[1]*x[1] + focus[2]*x[2],
        focus[0]*y[0] + focus[1]*y[1] + focus[2]*y[2],
        focus[0]*z[0] + focus[1]*z[1] + focus[2]*z[2]};
    if (bounds && bounds->Valid()) {
        NativeShadowBounds lightBounds;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const auto point = bounds->Corner(corner);
            float light[3]{};
            for (int axis = 0; axis < 3; ++axis)
                for (int row = 0; row < 3; ++row) light[axis] += point[row]*axes[axis][row];
            lightBounds.Include(light);
        }
        float half[3]{};
        for (int axis = 0; axis < 3; ++axis) {
            centre[axis] = lightBounds.minimum[axis]*.5f + lightBounds.maximum[axis]*.5f;
            const float extent = lightBounds.maximum[axis]*.5f - lightBounds.minimum[axis]*.5f;
            half[axis] = extent + (std::max)(.25f, extent*.05f);
        }
        const float halfWidth = (std::max)(1.f/lateral, (std::max)(half[0], half[1]));
        const float halfDepth = (std::max)(.5f/scale[2], half[2]);
        scale[0] = scale[1] = 1.f/halfWidth;
        scale[2] = .5f/halfDepth;
    }
    std::array<float, 16> matrix{};
    matrix[15] = 1;
    for (int axis = 0; axis < 3; ++axis) {
        for (int row = 0; row < 3; ++row) matrix[row*4+axis] = axes[axis][row]*scale[axis];
        matrix[12+axis] = -centre[axis]*scale[axis] + (axis == 2 ? .5f : 0.f);
    }
    return matrix;
}
}
