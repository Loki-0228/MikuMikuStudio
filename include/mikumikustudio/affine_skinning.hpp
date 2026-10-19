// ===========================================================================
// MikuMikuStudio - affine bone skinning math (shared CPU/GPU source of truth)
// ===========================================================================
// Pure inline mathematics used by three consumers:
//   * the CPU skinning workers (src/model/model_skinning.cpp),
//   * the GPU bone packing (PrepareGpuModelInput) and its HLSL twin
//     (src/render/d3d12_skinning.hlsl),
//   * tests/affine_skinning_tests.cpp.
// The header has no DirectX dependency. The x64 BDEF1 path uses SSE; other
// targets retain scalar mathematics. The same code compiles into the renderer
// and a standalone test binary without changing the BoneRecord layout.
//
// Conventions
// -----------
// Row-vector convention throughout: v' = v * M. A 3x3 linear part is stored
// row-major as m[row * 3 + col] and equals BoneRecord.matInit's rows
// matInit[row * 4 + col] for row, col in 0..2. Translation lives in
// matInit[12..14] and is not part of any 3x3. Quaternions are (x, y, z, w),
// matching D3DX and the GPU record.
//
// Affine contract
// ---------------
// A bone matrix carries a full affine transform: non-uniform scale and shear
// are legal, the determinant is positive. Positions always use the full
// matrix, which needs no change - the legacy kernels already do that. The
// extension covers two things the legacy rigid code got wrong once scaling
// exists:
//   * Normals must use the per-bone inverse transpose of the linear part and
//     be normalized again after the weight blend (a plain rotation is its own
//     inverse transpose, so rigid bones keep the legacy product).
//   * SDEF may not run D3DXQuaternionRotationMatrix on a non-rigid matrix.
//     The rotation comes from the polar decomposition A = H * R (H symmetric
//     positive definite, R orthogonal), so the quaternion is always legal.
//     The two H matrices blend linearly with the vertex weights and the SDEF
//     delta becomes (position - center) * Hblend * Rblend; the centre/arms
//     pivot keeps the legacy full-matrix formula, and the normal uses the
//     inverse transpose of Hblend * Rblend, normalized.
//
// Legacy compatibility
// ---------------------
// Precomputation flags a bone rigid when its linear part is orthonormal within
// kRigidEpsilon. Every kernel checks the flag and runs the untouched legacy
// arithmetic when all contributing bones are rigid (and the legacy SDEF path
// verbatim when both bones are rigid), so models without bone scaling keep
// their exact previous output, including the x86 x87 branch of
// BlendTransformComponent. A degenerate or mirrored matrix (determinant not
// positive) also falls back to the legacy path; the affine contract requires
// det > 0.
//
// Cost model
// ----------
// The polar decomposition, the inverse transpose and the rigid test run once
// per bone per frame (PrecomputeBone), never per vertex. Per-vertex work is
// matrix-vector products and weight blends only. The one exception is the SDEF
// normal of an affine vertex: it needs the inverse transpose of the blended
// stretch, which depends on the vertex weights. It is applied through the
// symmetric adjugate (SymmetricAdjugateMul), a fixed 15 flops with no matrix
// inverse and no polar call; the omitted factor det(Hblend) is positive and
// the result is normalized immediately afterwards, so it cannot change the
// direction. No cache is shared between calls or threads: callers pass their
// own BoneAux array.
// ===========================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace mikumikustudio::affine {

// Bit 0 of BoneAux::flags and gpu::Bone::flags: the record carries affine
// data (stretch + inverse transpose) and is not a pure rotation. Clear means
// the legacy rigid path, which is also what a default-constructed GPU record
// selects.
constexpr std::uint32_t kBoneFlagAffine = 1u;

// Gram-matrix tolerance separating "rotation" from "scaled/sheared". Rotation
// matrices rebuilt from quaternions deviate by ~1e-7; visible bone scaling
// deviates by orders of magnitude more, so only sub-visible factors (below
// ~5e-6 relative) fall on the legacy side of the line.
constexpr float kRigidEpsilon = 1e-5f;

// Per-bone precomputation produced once per bone per frame.
struct BoneAux {
    float stretch[9];       // H of A = H * R; identity for a rigid bone
    float normalMatrix[9];  // (linear)^-T = H^-1 * R; the linear part when rigid
    float rotation[4];      // legal rotation quaternion (x, y, z, w) of R
    std::uint32_t flags;    // kBoneFlagAffine when the linear part is not a rotation
};

// The two D3DX entry points the legacy SDEF path calls through. Production
// injects the real D3DX functions (bit parity with the x64 original); tests
// use the faithful re-implementations below.
struct RotationOps {
    void (*quatFromMatrix)(float out[4], const float matrix[16]);
    void (*matrixFromQuaternion)(float out[16], const float quaternion[4]);
};

// ---------------------------------------------------------------------------
// D3DX-compatible reference implementations (float arithmetic of
// D3DXQuaternionRotationMatrix / D3DXMatrixRotationQuaternion).
// ---------------------------------------------------------------------------
inline void DefaultQuatFromMatrix(float out[4], const float matrix[16]) {
    // D3DXQuaternionRotationMatrix's own structure: largest diagonal selects
    // the pivot, ties keep the lowest index. A zero matrix therefore yields
    // (0.5, 0, 0, 0), which is the missing-bone value the shader uses too.
    const float m11 = matrix[0], m12 = matrix[1], m13 = matrix[2];
    const float m21 = matrix[4], m22 = matrix[5], m23 = matrix[6];
    const float m31 = matrix[8], m32 = matrix[9], m33 = matrix[10];
    const float trace = m11 + m22 + m33;
    if (trace > 0.0f) {
        float root = std::sqrt(trace + 1.0f);
        out[3] = 0.5f * root;
        root = 0.5f / root;
        out[0] = (m32 - m23) * root;
        out[1] = (m13 - m31) * root;
        out[2] = (m21 - m12) * root;
        return;
    }
    const float diagonal[3] = {m11, m22, m33};
    int i = 0;
    if (diagonal[1] > diagonal[0])
        i = 1;
    if (diagonal[2] > diagonal[i])
        i = 2;
    const int j = (i + 1) % 3;
    const int k = (i + 2) % 3;
    const float element[3][3] = {
        {m11, m12, m13},
        {m21, m22, m23},
        {m31, m32, m33},
    };
    float root = std::sqrt((diagonal[i] - diagonal[j]) - diagonal[k] + 1.0f);
    out[i] = 0.5f * root;
    root = 0.5f / root;
    out[3] = (element[k][j] - element[j][k]) * root;
    out[j] = (element[j][i] + element[i][j]) * root;
    out[k] = (element[k][i] + element[i][k]) * root;
}

inline void DefaultMatrixFromQuaternion(float out[16],
                                        const float quaternion[4]) {
    const float x = quaternion[0], y = quaternion[1];
    const float z = quaternion[2], w = quaternion[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    out[0] = 1.0f - 2.0f * (yy + zz);
    out[1] = 2.0f * (xy + wz);
    out[2] = 2.0f * (xz - wy);
    out[3] = 0.0f;
    out[4] = 2.0f * (xy - wz);
    out[5] = 1.0f - 2.0f * (xx + zz);
    out[6] = 2.0f * (yz + wx);
    out[7] = 0.0f;
    out[8] = 2.0f * (xz + wy);
    out[9] = 2.0f * (yz - wx);
    out[10] = 1.0f - 2.0f * (xx + yy);
    out[11] = 0.0f;
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

inline RotationOps DefaultRotationOps() {
    return {DefaultQuatFromMatrix, DefaultMatrixFromQuaternion};
}

// ---------------------------------------------------------------------------
// Small matrix/vector helpers.
// ---------------------------------------------------------------------------
inline void Mat3Identity(float out[9]) {
    out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
    out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
    out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;
}

// out = a * b (row-major 3x3).
inline void Mat3Mul(const float a[9], const float b[9], float out[9]) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row * 3 + col] =
                a[row * 3] * b[col] +
                a[row * 3 + 1] * b[3 + col] +
                a[row * 3 + 2] * b[6 + col];
        }
    }
}

// out = v * m (row vector times row-major 3x3).
inline void MulVec3(float out[3], const float v[3], const float m[9]) {
    for (int col = 0; col < 3; ++col) {
        out[col] = v[0] * m[col] + v[1] * m[3 + col] + v[2] * m[6 + col];
    }
}

inline float Mat3Det(const float m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

namespace detail {

inline bool Mat3InverseDouble(const double m[9], double out[9]) {
    const double det =
        m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) +
        m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!(det > 1e-300) || !std::isfinite(det))
        return false;
    const double inverse = 1.0 / det;
    out[0] = (m[4] * m[8] - m[5] * m[7]) * inverse;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * inverse;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * inverse;
    out[3] = (m[5] * m[6] - m[3] * m[8]) * inverse;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * inverse;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * inverse;
    out[6] = (m[3] * m[7] - m[4] * m[6]) * inverse;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * inverse;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * inverse;
    return true;
}

}  // namespace detail

// out = inverse(m) for a row-major 3x3. False leaves out untouched.
inline bool Mat3Inverse(const float m[9], float out[9]) {
    double source[9];
    double inverse[9];
    for (int i = 0; i < 9; ++i)
        source[i] = m[i];
    if (!detail::Mat3InverseDouble(source, inverse))
        return false;
    for (int i = 0; i < 9; ++i)
        out[i] = static_cast<float>(inverse[i]);
    return true;
}

// out = (linear)^-T, the matrix that transforms normals under v' = v * linear.
inline void NormalMatrixFromLinear(const float linear[9], float out[9]) {
    double source[9];
    double inverse[9];
    for (int i = 0; i < 9; ++i)
        source[i] = linear[i];
    Mat3Identity(out);
    if (!detail::Mat3InverseDouble(source, inverse))
        return;
    // transpose while storing
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row * 3 + col] = static_cast<float>(inverse[col * 3 + row]);
}

// True when the linear part is a rotation (Gram matrix identity within
// kRigidEpsilon). Mirrored and singular matrices never pass this test; the
// caller's finite-positive-determinant check sends them down the legacy path.
inline bool LinearIsRotation(const float linear[9]) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float dot = linear[row * 3] * linear[col * 3] +
                              linear[row * 3 + 1] * linear[col * 3 + 1] +
                              linear[row * 3 + 2] * linear[col * 3 + 2];
            const float target = row == col ? 1.0f : 0.0f;
            if (std::fabs(dot - target) > kRigidEpsilon)
                return false;
        }
    }
    return true;
}

// Legal unit quaternion of a proper rotation matrix (row-major rows). Not
// bit-comparable with D3DX and not meant to be: the legacy path keeps calling
// D3DX, this extraction feeds only the polar factors.
inline void QuaternionFromRotationMatrix(const float m[9], float out[4]) {
    // Synthesis is RotationMatrixFromQuaternion's:
    //   r01 - r10 = 4wz, r20 - r02 = 4wy, r12 - r21 = 4wx
    const float trace = m[0] + m[4] + m[8];
    float x, y, z, w;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;  // s = 4w
        w = 0.25f * s;
        x = (m[5] - m[7]) / s;
        y = (m[6] - m[2]) / s;
        z = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        w = (m[5] - m[7]) / s;
        x = 0.25f * s;
        y = (m[1] + m[3]) / s;
        z = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        w = (m[6] - m[2]) / s;
        x = (m[1] + m[3]) / s;
        y = 0.25f * s;
        z = (m[5] + m[7]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        w = (m[1] - m[3]) / s;
        x = (m[2] + m[6]) / s;
        y = (m[5] + m[7]) / s;
        z = 0.25f * s;
    }
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 0.0f)) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    const float inverse = 1.0f / length;
    out[0] = x * inverse;
    out[1] = y * inverse;
    out[2] = z * inverse;
    out[3] = w * inverse;
}

// Row-major rotation matrix of a (x, y, z, w) quaternion. The rows match the
// shader's rotate() construction.
inline void RotationMatrixFromQuaternion(const float q[4], float out[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    out[0] = 1.0f - 2.0f * (yy + zz);
    out[1] = 2.0f * (xy + wz);
    out[2] = 2.0f * (xz - wy);
    out[3] = 2.0f * (xy - wz);
    out[4] = 1.0f - 2.0f * (xx + zz);
    out[5] = 2.0f * (yz + wx);
    out[6] = 2.0f * (xz + wy);
    out[7] = 2.0f * (yz - wx);
    out[8] = 1.0f - 2.0f * (xx + yy);
}

// Polar decomposition A = H * R with H symmetric positive definite and R
// orthogonal, for a row-major 3x3 with det > 0. The Newton iteration
// X <- (gamma * X + X^-T / gamma) / 2 runs on A^T and converges to U = R^T;
// then H = A * U. The per-step factor gamma = (||X^-1||F^2 / ||X||F^2)^(1/4)
// (Higham's scaled Newton) balances the two summands so large anisotropy is
// handled in a few steps, and the initial max|a| scaling keeps the first
// inverse away from overflow and underflow for extreme magnitudes.
// False on a singular matrix, non-finite arithmetic or a stalled iteration -
// the caller then keeps the legacy rigid path.
inline bool PolarDecompose(const float a[9], float stretch[9],
                           float rotation[4]) {
    double maxMagnitude = 0.0;
    for (int i = 0; i < 9; ++i) {
        const double magnitude = std::fabs(static_cast<double>(a[i]));
        if (!std::isfinite(magnitude))
            return false;
        if (magnitude > maxMagnitude)
            maxMagnitude = magnitude;
    }
    if (!(maxMagnitude > 0.0))
        return false;
    const double balance = 1.0 / maxMagnitude;
    double x[9];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            x[row * 3 + col] = a[col * 3 + row] * balance;  // X0 = A^T / max|a|
    // Far from the solution each step only halves the anisotropy (linear
    // convergence), so the iteration count is driven by log2(cond(A)): a
    // hierarchical composite with cond ~ 1e12 needs about 45 steps before the
    // quadratic phase. 96 steps cover cond up to ~1e24; a worse matrix
    // returns false and keeps the legacy path.
    for (int iteration = 0; iteration < 96; ++iteration) {
        double inverse[9];
        if (!detail::Mat3InverseDouble(x, inverse))
            return false;
        // Higham's scaled Newton step: gamma = (||X^-1||F^2 / ||X||F^2)^(1/4)
        // balances the two summands so each step handles large anisotropy
        // instead of only halving it (a plain step needs log2(cond) steps).
        double normX = 0.0;
        double normInverse = 0.0;
        for (int i = 0; i < 9; ++i) {
            normX += x[i] * x[i];
            normInverse += inverse[i] * inverse[i];
        }
        if (!(normX > 0.0) || !(normInverse > 0.0) ||
            !std::isfinite(normX) || !std::isfinite(normInverse))
            return false;
        const double gamma = std::sqrt(std::sqrt(normInverse / normX));
        if (!std::isfinite(gamma) || !(gamma > 0.0))
            return false;
        const double inverseGamma = 1.0 / gamma;
        double next[9];
        double change = 0.0;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const double value =
                    0.5 * (gamma * x[row * 3 + col] +
                           inverse[col * 3 + row] * inverseGamma);
                if (!std::isfinite(value))
                    return false;
                const double delta = value - x[row * 3 + col];
                const double magnitude = delta < 0.0 ? -delta : delta;
                if (magnitude > change)
                    change = magnitude;
                next[row * 3 + col] = value;
            }
        }
        for (int i = 0; i < 9; ++i)
            x[i] = next[i];
        if (change <= 1e-13) {
            double r[9];
            double h[9];
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    r[row * 3 + col] = x[col * 3 + row];  // R = U^T
                    h[row * 3 + col] = static_cast<double>(a[row * 3]) * x[col] +
                                       static_cast<double>(a[row * 3 + 1]) *
                                           x[3 + col] +
                                       static_cast<double>(a[row * 3 + 2]) *
                                           x[6 + col];  // H = A * U
                }
            }
            // Symmetrize: H must be bitwise symmetric so the vertex blend of
            // two H matrices stays symmetric.
            float hOut[9];
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    const double symmetric =
                        0.5 * (h[row * 3 + col] + h[col * 3 + row]);
                    if (!std::isfinite(symmetric) ||
                        std::fabs(symmetric) > 3.0e38)
                        return false;
                    hOut[row * 3 + col] = static_cast<float>(symmetric);
                }
            }
            for (int i = 0; i < 9; ++i)
                stretch[i] = hOut[i];
            float rFloat[9];
            for (int i = 0; i < 9; ++i)
                rFloat[i] = static_cast<float>(r[i]);
            QuaternionFromRotationMatrix(rFloat, rotation);
            return true;
        }
    }
    return false;  // legal fallback: the caller keeps the legacy rigid path
}

// Per-bone precomputation. matrix is the full 4x4 affine bone transform
// (BoneRecord.matInit / gpu::Bone.matrix). rigidQuaternion is the value the
// legacy path would use (D3DXQuaternionRotationMatrix of the same matrix); it
// is kept verbatim for rigid bones so the legacy SDEF result stays bit-exact.
// Pass nullptr when the caller has no SDEF vertices (the PMD path).
inline void PrecomputeBone(const float matrix[16], const float rigidQuaternion[4],
                           BoneAux& out) {
    float linear[9];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            linear[row * 3 + col] = matrix[row * 4 + col];
    Mat3Identity(out.stretch);
    const auto legacyFallback = [&]() {
        out.flags = 0;
        for (int i = 0; i < 9; ++i)
            out.normalMatrix[i] = linear[i];
        if (rigidQuaternion != nullptr) {
            for (int i = 0; i < 4; ++i)
                out.rotation[i] = rigidQuaternion[i];
        } else {
            out.rotation[0] = out.rotation[1] = out.rotation[2] = 0.0f;
            out.rotation[3] = 1.0f;
        }
    };
    // Mirror (det <= 0) and non-finite matrices are outside the affine
    // contract: keep the legacy rigid path instead of producing an illegal
    // rotation. There is deliberately no relative singularity gate - any
    // matrix with a finite positive determinant is legitimate (for example
    // diag(10000, 0.0001, 0.0001), which bone_scale::Valid accepts). What
    // guards the rest is the decomposition itself: PolarDecompose only
    // succeeds when its inverses stay finite and the iteration converges.
    double determinant =
        static_cast<double>(linear[0]) *
            (static_cast<double>(linear[4]) * linear[8] -
             static_cast<double>(linear[5]) * linear[7]) -
        static_cast<double>(linear[1]) *
            (static_cast<double>(linear[3]) * linear[8] -
             static_cast<double>(linear[5]) * linear[6]) +
        static_cast<double>(linear[2]) *
            (static_cast<double>(linear[3]) * linear[7] -
             static_cast<double>(linear[4]) * linear[6]);
    if (!std::isfinite(determinant) || !(determinant > 0.0) ||
        LinearIsRotation(linear)) {
        legacyFallback();
        return;
    }
    float rotation[4];
    if (!PolarDecompose(linear, out.stretch, rotation)) {
        legacyFallback();
        return;
    }
    out.flags = kBoneFlagAffine;
    for (int i = 0; i < 4; ++i)
        out.rotation[i] = rotation[i];
    NormalMatrixFromLinear(linear, out.normalMatrix);
}

// ---------------------------------------------------------------------------
// Legacy transform primitives, moved verbatim from model_skinning.cpp so the
// rigid path stays bit-identical and the tests can pin it.
// ---------------------------------------------------------------------------
inline void TransformPosition(float out[3], const float in[3], const float* m) {
    out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
    out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
    out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
}

inline void TransformNormal(float out[3], const float in[3], const float* m) {
    out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8];
    out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9];
    out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10];
}

inline float BlendTransformComponent(const float* input, const float* matrix0,
                                     const float* matrix1, int component,
                                     float weight0, bool position) {
#if defined(_M_IX86)
    float result = 0.0f;
    const int byteOffset = component * 4;
    const float weight1 = 1.0f - weight0;
    __asm {
        mov eax, input
        mov ecx, matrix0
        mov edx, matrix1
        mov ebx, byteOffset

        // Original 0x4A95E0 family: bone 1 is accumulated first, multiplied
        // by (1-weight), then bone 0 is accumulated and multiplied by
        // weight.  No transformed component is rounded to float in between.
        fld dword ptr [eax]
        fmul dword ptr [edx+ebx]
        fld dword ptr [eax+4]
        fmul dword ptr [edx+ebx+16]
        faddp st(1), st
        fld dword ptr [eax+8]
        fmul dword ptr [edx+ebx+32]
        faddp st(1), st
        cmp position, 0
        je no_translation_1
        fadd dword ptr [edx+ebx+48]
no_translation_1:
        fmul dword ptr [weight1]

        fld dword ptr [eax]
        fmul dword ptr [ecx+ebx]
        fld dword ptr [eax+4]
        fmul dword ptr [ecx+ebx+16]
        faddp st(1), st
        fld dword ptr [eax+8]
        fmul dword ptr [ecx+ebx+32]
        faddp st(1), st
        cmp position, 0
        je no_translation_0
        fadd dword ptr [ecx+ebx+48]
no_translation_0:
        fmul dword ptr [weight0]
        faddp st(1), st
        fstp dword ptr [result]
    }
    return result;
#else
    const float weight1 = 1.0f - weight0;
    const float a = input[0] * matrix0[component] +
                    input[1] * matrix0[component + 4] +
                    input[2] * matrix0[component + 8] +
                    (position ? matrix0[component + 12] : 0.0f);
    const float b = input[0] * matrix1[component] +
                    input[1] * matrix1[component + 4] +
                    input[2] * matrix1[component + 8] +
                    (position ? matrix1[component + 12] : 0.0f);
    return b * weight1 + a * weight0;
#endif
}

inline float TransformComponent(const float input[3], const float matrix[16],
                                int component, bool position) {
    float value = input[0] * matrix[component];
    value += input[1] * matrix[component + 4];
    value += input[2] * matrix[component + 8];
    if (position)
        value += matrix[component + 12];
    return value;
}

// x64 helper 0x1400E2D20: dot-sign-corrected normalized lerp (not a true
// slerp). `t` reaches 1 - weight0, so weight0 = 1 keeps q0.
inline void BlendSdefQuaternions(float out[4], const float from[4],
                                 const float to[4], float t) {
    const float dot = ((to[0] * from[0] + to[3] * from[3]) +
                       to[1] * from[1]) + to[2] * from[2];
    float delta[4];
    if (dot >= 0.0f) {
        delta[0] = to[0] - from[0];
        delta[1] = to[1] - from[1];
        delta[2] = to[2] - from[2];
        delta[3] = to[3] - from[3];
    } else {
        t = -t;
        delta[0] = to[0] + from[0];
        delta[1] = to[1] + from[1];
        delta[2] = to[2] + from[2];
        delta[3] = to[3] + from[3];
    }
    out[0] = delta[0] * t + from[0];
    out[1] = delta[1] * t + from[1];
    out[2] = delta[2] * t + from[2];
    out[3] = delta[3] * t + from[3];
    const float length = std::sqrt(
        ((out[0] * out[0] + out[1] * out[1]) + out[2] * out[2]) +
        out[3] * out[3]);
    if (length <= 0.0f) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    const float inverse = 1.0f / length;
    out[0] *= inverse;
    out[1] *= inverse;
    out[2] *= inverse;
    out[3] *= inverse;
}

// ---------------------------------------------------------------------------
// Affine-path helpers.
// ---------------------------------------------------------------------------
// out = in * q as a rotation, same construction and accumulation order as the
// shader's rotate().
inline void RotateByQuaternion(float out[3], const float in[3],
                               const float q[4]) {
    float rotation[9];
    RotationMatrixFromQuaternion(q, rotation);
    for (int component = 0; component < 3; ++component) {
        float value = in[0] * rotation[component];
        value += in[1] * rotation[3 + component];
        value += in[2] * rotation[6 + component];
        out[component] = value;
    }
}

inline void Normalize3(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 0.0f))
        return;
    const float inverse = 1.0f / length;
    v[0] *= inverse;
    v[1] *= inverse;
    v[2] *= inverse;
}

// Per-bone normal transform: the linear part when the bone is rigid (a
// rotation is its own inverse transpose), the precomputed inverse transpose
// when it is not.
inline void TransformNormalBone(float out[3], const float in[3],
                                const float* matrix, const BoneAux& aux) {
    if ((aux.flags & kBoneFlagAffine) == 0) {
        TransformNormal(out, in, matrix);
        return;
    }
    MulVec3(out, in, aux.normalMatrix);
}

// out = in * adj(h) for a symmetric h; adj(h) = det(h) * inverse(h), and h is
// symmetric so the adjugate is too. The result is det(h) times the exact
// inverse-transpose application; det(h) > 0 and callers normalize right after,
// so the scale cannot change the direction. This is what lets the SDEF normal
// follow the blended stretch without a per-vertex matrix inverse.
inline void SymmetricAdjugateMul(float out[3], const float in[3],
                                 const float h[9]) {
    const float a = h[0], b = h[4], c = h[8];
    const float d = h[1], e = h[2], f = h[5];
    const float adj00 = b * c - f * f;
    const float adj01 = e * f - d * c;
    const float adj02 = d * f - e * b;
    const float adj11 = a * c - e * e;
    const float adj12 = d * e - a * f;
    const float adj22 = a * b - d * d;
    out[0] = in[0] * adj00 + in[1] * adj01 + in[2] * adj02;
    out[1] = in[0] * adj01 + in[1] * adj11 + in[2] * adj12;
    out[2] = in[0] * adj02 + in[1] * adj12 + in[2] * adj22;
}

// ---------------------------------------------------------------------------
// Vertex kernels. Positions keep the legacy arithmetic (and therefore the
// legacy x86 x87 path) unchanged in every case; only normals branch.
// ---------------------------------------------------------------------------
struct BoneRef {
    const float* matrix;  // 16-float transform (zero matrix for missing bones)
    const BoneAux* aux;   // per-bone precomputation (rigid for missing bones)
};

struct Skinned {
    float position[3];
    float normal[3];
};

// x64 worker 0x14011F991 (BDEF1) and the PMD two-bone worker (BDEF2 with the
// weightPercent weight).
inline void SkinBdef1(const BoneRef& bone, const float position[3],
                      const float normal[3], Skinned& out) {
#if defined(_M_X64) || defined(__x86_64__)
    {
        // Compute XYZ together, retaining the scalar multiply/add order.
        // Do not fuse operations or read a fourth input coordinate.
        const __m128 row0 = _mm_loadu_ps(bone.matrix);
        const __m128 row1 = _mm_loadu_ps(bone.matrix + 4);
        const __m128 row2 = _mm_loadu_ps(bone.matrix + 8);
        const auto transform = [](const float* value, __m128 row0, __m128 row1, __m128 row2) {
            return _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(value[0]), row0),
                                         _mm_mul_ps(_mm_set1_ps(value[1]), row1)),
                              _mm_mul_ps(_mm_set1_ps(value[2]), row2));
        };
        float result[4];
        _mm_storeu_ps(result, _mm_add_ps(transform(position, row0, row1, row2), _mm_loadu_ps(bone.matrix + 12)));
        std::memcpy(out.position, result, sizeof(out.position));
        const bool affine = (bone.aux->flags & kBoneFlagAffine) != 0;
        if (affine) {
            const float* m = bone.aux->normalMatrix;
            _mm_storeu_ps(result, transform(normal, _mm_loadu_ps(m), _mm_loadu_ps(m + 3),
                                            _mm_set_ps(0.f, m[8], m[7], m[6])));
        } else {
            _mm_storeu_ps(result, transform(normal, row0, row1, row2));
        }
        std::memcpy(out.normal, result, sizeof(out.normal));
        if (affine) Normalize3(out.normal);
        return;
    }
#endif
    TransformPosition(out.position, position, bone.matrix);
    TransformNormalBone(out.normal, normal, bone.matrix, *bone.aux);
    if (bone.aux->flags & kBoneFlagAffine)
        Normalize3(out.normal);
}

// x64 worker 0x14011F513 (BDEF2) and the PMD worker: bone 1 first with
// (1-weight), then bone 0 with weight. PMD only differs in where the weight
// comes from and uses this same kernel.
inline void SkinBdef2(const BoneRef& bone0, const BoneRef& bone1, float weight0,
                      const float position[3], const float normal[3],
                      Skinned& out) {
    const float* matrix0 = bone0.matrix;
    const float* matrix1 = bone1.matrix;
    for (int component = 0; component < 3; ++component) {
        out.position[component] = BlendTransformComponent(
            position, matrix0, matrix1, component, weight0, true);
    }
    const bool affine =
        ((bone0.aux->flags | bone1.aux->flags) & kBoneFlagAffine) != 0;
    if (!affine) {
        for (int component = 0; component < 3; ++component) {
            out.normal[component] = BlendTransformComponent(
                normal, matrix0, matrix1, component, weight0, false);
        }
        return;
    }
    const float weight1 = 1.0f - weight0;
    float transformed0[3];
    float transformed1[3];
    TransformNormalBone(transformed0, normal, matrix0, *bone0.aux);
    TransformNormalBone(transformed1, normal, matrix1, *bone1.aux);
    for (int component = 0; component < 3; ++component) {
        out.normal[component] =
            transformed1[component] * weight1 + transformed0[component] * weight0;
    }
    Normalize3(out.normal);
}

// x64 worker 0x14011FB7F (BDEF4): positions accumulate bone 1 first and then
// bones 0, 2 and 3; normals start from bone 0. Both orders are preserved.
inline void SkinBdef4(const BoneRef bones[4], const float weight[4],
                      const float position[3], const float normal[3],
                      Skinned& out) {
    const float* matrix0 = bones[0].matrix;
    const float* matrix1 = bones[1].matrix;
    const float* matrix2 = bones[2].matrix;
    const float* matrix3 = bones[3].matrix;
    for (int component = 0; component < 3; ++component) {
        float value =
            TransformComponent(position, matrix1, component, true) * weight[1];
        value +=
            TransformComponent(position, matrix0, component, true) * weight[0];
        value +=
            TransformComponent(position, matrix2, component, true) * weight[2];
        value +=
            TransformComponent(position, matrix3, component, true) * weight[3];
        out.position[component] = value;
    }
    const bool affine =
        ((bones[0].aux->flags | bones[1].aux->flags | bones[2].aux->flags |
          bones[3].aux->flags) & kBoneFlagAffine) != 0;
    if (!affine) {
        for (int component = 0; component < 3; ++component) {
            float value = TransformComponent(normal, matrix0, component, false) *
                          weight[0];
            value += TransformComponent(normal, matrix1, component, false) *
                     weight[1];
            value += TransformComponent(normal, matrix2, component, false) *
                     weight[2];
            value += TransformComponent(normal, matrix3, component, false) *
                     weight[3];
            out.normal[component] = value;
        }
        return;
    }
    float transformed[4][3];
    for (int bone = 0; bone < 4; ++bone)
        TransformNormalBone(transformed[bone], normal, bones[bone].matrix,
                            *bones[bone].aux);
    for (int component = 0; component < 3; ++component) {
        float value = transformed[0][component] * weight[0];
        value += transformed[1][component] * weight[1];
        value += transformed[2][component] * weight[2];
        value += transformed[3][component] * weight[3];
        out.normal[component] = value;
    }
    Normalize3(out.normal);
}

// x64 worker 0x140120285 (SDEF). The centre/arms pivot keeps the exact legacy
// folding: the loader has rebased R0/R1 onto the weighted SDEF midpoint, so
// weight0 * r0 + (1 - weight0) * r1 = 0 and the pivot degenerates to the
// affine image of the centre whenever both bones carry the same transform -
// with or without scaling. The rotation branch is the only change: the legacy
// path runs D3DX through ops and stays untouched for rigid bones, the affine
// path uses the polar rotation and the blended stretch.
inline void SkinSdef(const BoneRef& bone0, const BoneRef& bone1, float weight0,
                     const float position[3], const float normal[3],
                     const float center[3], const float r0Offset[3],
                     const float r1Offset[3], const RotationOps& ops,
                     Skinned& out) {
    const float* matrix0 = bone0.matrix;
    const float* matrix1 = bone1.matrix;
    const float weight1 = 1.0f - weight0;

    float pivot[3];
    for (int component = 0; component < 3; ++component) {
        const float blendedCenter =
            TransformComponent(center, matrix0, component, true) * weight0 +
            TransformComponent(center, matrix1, component, true) * weight1;
        const float blendedArms =
            TransformComponent(r0Offset, matrix0, component, false) * weight0 +
            TransformComponent(r1Offset, matrix1, component, false) * weight1;
        pivot[component] = ((blendedArms + blendedCenter) + blendedCenter) *
                           0.5f;
    }

    const bool affine =
        ((bone0.aux->flags | bone1.aux->flags) & kBoneFlagAffine) != 0;
    float quaternion0[4];
    float quaternion1[4];
    float quaternion[4];
    if (affine) {
        for (int i = 0; i < 4; ++i) {
            quaternion0[i] = bone0.aux->rotation[i];
            quaternion1[i] = bone1.aux->rotation[i];
        }
    } else {
        ops.quatFromMatrix(quaternion0, matrix0);
        ops.quatFromMatrix(quaternion1, matrix1);
    }
    BlendSdefQuaternions(quaternion, quaternion0, quaternion1, weight1);

    const float delta[3] = {
        position[0] - center[0],
        position[1] - center[1],
        position[2] - center[2]};
    if (!affine) {
        float blended[16];
        ops.matrixFromQuaternion(blended, quaternion);
        const float* rotation = blended;
        for (int component = 0; component < 3; ++component) {
            out.position[component] =
                TransformComponent(delta, rotation, component, false) +
                pivot[component];
            out.normal[component] =
                TransformComponent(normal, rotation, component, false);
        }
        return;
    }

    float stretch[9];
    for (int i = 0; i < 9; ++i) {
        stretch[i] = bone0.aux->stretch[i] * weight0 +
                     bone1.aux->stretch[i] * weight1;
    }
    float deformed[3];
    MulVec3(deformed, delta, stretch);
    float rotated[3];
    RotateByQuaternion(rotated, deformed, quaternion);
    for (int component = 0; component < 3; ++component)
        out.position[component] = rotated[component] + pivot[component];
    float inverseTransposed[3];
    SymmetricAdjugateMul(inverseTransposed, normal, stretch);
    RotateByQuaternion(out.normal, inverseTransposed, quaternion);
    Normalize3(out.normal);
}

}  // namespace mikumikustudio::affine
