#pragma once

// ===========================================================================
// Matrix -> bone Euler readout (shared)
// ===========================================================================
// PostViewRefresh's Euler extraction (VA 0x40D643..0x40D7BF): a quaternion
// rotation matrix to the X/Y/Z degree values shown in the bone rotation
// readout.  It lives in its own function so the rotation-reset path can
// reuse the exact float-truncated x87 sequence (asin clamp, 1e-6f pole and
// small-angle handling, pi = (double)3.141592f).
// ===========================================================================
#include <algorithm>
#include <array>
#include <cmath>

#include "mikumikustudio/d3dx_dyn.hpp"

namespace mikumikustudio::ui {

inline std::array<float, 3> BoneRotationDegreesFromMatrix(
    const d3dx::D3DXMATRIXF& m) noexcept {
    // Every intermediate is float-truncated exactly as the original x87
    // sequence (fstp dword) does.
    const float m12 = m.m[0][1];
    const float m22 = m.m[1][1];
    const float m31 = m.m[2][0];
    const float m32 = m.m[2][1];
    const float m33 = m.m[2][2];

    float rz = static_cast<float>(std::atan2(static_cast<double>(m12),
                                             static_cast<double>(m22)));
    float rx = static_cast<float>(std::asin((std::max)(-1.0,(std::min)(1.0,-static_cast<double>(m32)))));
    float ry = static_cast<float>(std::atan2(static_cast<double>(m31),
                                             static_cast<double>(m33)));
    float c = static_cast<float>(std::cos(static_cast<double>(rx)));

    if (std::fabs(static_cast<double>(c)) < 1e-6f) {    // 0x40D6AD flt_52B740
        // At the pole choose a stable equivalent decomposition with Z = 0.
        rz = 0.0f;
        ry = std::atan2(-m.m[0][2],m.m[0][0]);
    }
    // Zeroing (0x40D728/0x40D747/0x40D766).  Treating the comparison operand
    // as |cos(rx)| erased normal sub-radian angles; the original uses 1e-6f.
    if (std::fabs(static_cast<double>(rx)) < 1e-6f)
        rx = 0.0f;
    if (std::fabs(static_cast<double>(ry)) < 1e-6f)
        ry = 0.0f;
    if (std::fabs(static_cast<double>(rz)) < 1e-6f)
        rz = 0.0f;

    // Radians -> degrees in double, pi = (double)3.141592f (dbl_52B768),
    // 180.0 (dbl_52B760); each result stored back as float.
    rx = static_cast<float>(static_cast<double>(rx) / 3.141592f * 180.0f);
    ry = static_cast<float>(-static_cast<double>(ry) / 3.141592f * 180.0f);
    rz = static_cast<float>(180.0f * (-static_cast<double>(rz) / 3.141592f));

    return {rx, ry, rz};
}

}  // namespace mikumikustudio::ui
