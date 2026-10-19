// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Affine bone skinning tests: legacy rigid behaviour is bit-pinned, the
// uniform / non-uniform / shear affine paths are checked against independent
// double-precision reference math, and the CPU kernels are compared with a
// transcription of src/render/d3d12_skinning.hlsl so the two paths stay
// consistent. The test is pure mathematics - no window, device or model file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/d3d12_skinning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace mikumikustudio;

namespace {

void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

bool Near(double got, double expected, double relative, double absolute) {
    const double difference = std::fabs(got - expected);
    return difference <= absolute + relative * std::fabs(expected);
}

template <typename T, typename U>
bool NearVec(const T got[3], const U expected[3], double relative,
             double absolute) {
    for (int c = 0; c < 3; ++c)
        if (!Near(double(got[c]), double(expected[c]), relative, absolute))
            return false;
    return true;
}

bool BitEqual(const void* a, const void* b, std::size_t bytes) {
    return std::memcmp(a, b, bytes) == 0;
}

// ---------------------------------------------------------------------------
// Double-precision reference math (independent of the shipped code).
// ---------------------------------------------------------------------------
struct DMatrix {
    double m[9];  // row-major rows
};

DMatrix DMul(const DMatrix& a, const DMatrix& b) {
    DMatrix out{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                out.m[row * 3 + col] += a.m[row * 3 + k] * b.m[k * 3 + col];
    return out;
}

bool DInverse(const DMatrix& in, DMatrix& out) {
    const double* m = in.m;
    const double det =
        m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) +
        m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-300)
        return false;
    const double inverse = 1.0 / det;
    out.m[0] = (m[4] * m[8] - m[5] * m[7]) * inverse;
    out.m[1] = (m[2] * m[7] - m[1] * m[8]) * inverse;
    out.m[2] = (m[1] * m[5] - m[2] * m[4]) * inverse;
    out.m[3] = (m[5] * m[6] - m[3] * m[8]) * inverse;
    out.m[4] = (m[0] * m[8] - m[2] * m[6]) * inverse;
    out.m[5] = (m[2] * m[3] - m[0] * m[5]) * inverse;
    out.m[6] = (m[3] * m[7] - m[4] * m[6]) * inverse;
    out.m[7] = (m[1] * m[6] - m[0] * m[7]) * inverse;
    out.m[8] = (m[0] * m[4] - m[1] * m[3]) * inverse;
    return true;
}

DMatrix DTranspose(const DMatrix& in) {
    DMatrix out{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out.m[row * 3 + col] = in.m[col * 3 + row];
    return out;
}

template <typename T>
void DMulVec(double out[3], const T v[3], const DMatrix& m) {
    for (int col = 0; col < 3; ++col)
        out[col] = double(v[0]) * m.m[col] + double(v[1]) * m.m[3 + col] +
                   double(v[2]) * m.m[6 + col];
}

void DNormalize(double v[3]) {
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    Check(length > 0.0, "reference normal is not zero");
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
}

DMatrix DRotation(const float q[4]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    DMatrix out{};
    out.m[0] = 1.0 - 2.0 * (yy + zz);
    out.m[1] = 2.0 * (xy + wz);
    out.m[2] = 2.0 * (xz - wy);
    out.m[3] = 2.0 * (xy - wz);
    out.m[4] = 1.0 - 2.0 * (xx + zz);
    out.m[5] = 2.0 * (yz + wx);
    out.m[6] = 2.0 * (xz + wy);
    out.m[7] = 2.0 * (yz - wx);
    out.m[8] = 1.0 - 2.0 * (xx + yy);
    return out;
}

DMatrix DFrom9(const float m[9]) {
    DMatrix out{};
    for (int i = 0; i < 9; ++i)
        out.m[i] = m[i];
    return out;
}

// ---------------------------------------------------------------------------
// Bone case: the matrix is built from known factors so the reference knows the
// intended stretch and rotation without calling the shipped polar code.
// ---------------------------------------------------------------------------
struct BoneCase {
    float matrix[16];
    float h[9];   // intended stretch (symmetric positive definite)
    float q[4];   // intended rotation quaternion (x, y, z, w)
};

BoneCase MakeBone(const double h[9], const float q[4], const float t[3]) {
    BoneCase out{};
    const DMatrix rotation = DRotation(q);
    DMatrix stretch{};
    for (int i = 0; i < 9; ++i)
        stretch.m[i] = h[i];
    const DMatrix linear = DMul(stretch, rotation);  // A = H * R
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.matrix[row * 4 + col] =
                static_cast<float>(linear.m[row * 3 + col]);
            out.h[row * 3 + col] = static_cast<float>(h[row * 3 + col]);
        }
        out.matrix[row * 4 + 3] = 0.0f;
    }
    out.matrix[12] = t[0];
    out.matrix[13] = t[1];
    out.matrix[14] = t[2];
    out.matrix[15] = 1.0f;
    for (int i = 0; i < 4; ++i)
        out.q[i] = q[i];
    return out;
}

void QuaternionFromAxis(float out[4], float x, float y, float z, float angle) {
    const float half = angle * 0.5f;
    const float s = std::sin(half);
    const float length = std::sqrt(x * x + y * y + z * z);
    out[0] = x / length * s;
    out[1] = y / length * s;
    out[2] = z / length * s;
    out[3] = std::cos(half);
}

// ---------------------------------------------------------------------------
// Legacy rigid kernels: verbatim transcriptions of the code before the affine
// extension (the C path of BlendTransformComponent). They pin the bit-exact
// legacy behaviour of the shared kernels.
// ---------------------------------------------------------------------------
namespace legacy {

void TransformPosition(float out[3], const float in[3], const float* m) {
    out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
    out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
    out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
}

void TransformNormal(float out[3], const float in[3], const float* m) {
    out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8];
    out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9];
    out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10];
}

float BlendTransformComponent(const float* input, const float* matrix0,
                              const float* matrix1, int component,
                              float weight0, bool position) {
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
}

float TransformComponent(const float input[3], const float matrix[16],
                         int component, bool position) {
    float value = input[0] * matrix[component];
    value += input[1] * matrix[component + 4];
    value += input[2] * matrix[component + 8];
    if (position)
        value += matrix[component + 12];
    return value;
}

void BlendSdefQuaternions(float out[4], const float from[4], const float to[4],
                          float t) {
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

void Bdef1(const float* matrix, const float position[3], const float normal[3],
           affine::Skinned& out) {
    TransformPosition(out.position, position, matrix);
    TransformNormal(out.normal, normal, matrix);
}

void Bdef2(const float* matrix0, const float* matrix1, float weight0,
           const float position[3], const float normal[3],
           affine::Skinned& out) {
    for (int component = 0; component < 3; ++component) {
        out.position[component] = BlendTransformComponent(
            position, matrix0, matrix1, component, weight0, true);
        out.normal[component] = BlendTransformComponent(
            normal, matrix0, matrix1, component, weight0, false);
    }
}

void Bdef4(const float* matrix0, const float* matrix1, const float* matrix2,
           const float* matrix3, const float weight[4], const float position[3],
           const float normal[3], affine::Skinned& out) {
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

        float normalValue =
            TransformComponent(normal, matrix0, component, false) * weight[0];
        normalValue +=
            TransformComponent(normal, matrix1, component, false) * weight[1];
        normalValue +=
            TransformComponent(normal, matrix2, component, false) * weight[2];
        normalValue +=
            TransformComponent(normal, matrix3, component, false) * weight[3];
        out.normal[component] = normalValue;
    }
}

void Sdef(const float* matrix0, const float* matrix1, float weight0,
          const float position[3], const float normal[3], const float center[3],
          const float r0[3], const float r1[3], const affine::RotationOps& ops,
          affine::Skinned& out) {
    const float weight1 = 1.0f - weight0;
    float pivot[3];
    for (int component = 0; component < 3; ++component) {
        const float blendedCenter =
            TransformComponent(center, matrix0, component, true) * weight0 +
            TransformComponent(center, matrix1, component, true) * weight1;
        const float blendedArms =
            TransformComponent(r0, matrix0, component, false) * weight0 +
            TransformComponent(r1, matrix1, component, false) * weight1;
        pivot[component] = ((blendedArms + blendedCenter) + blendedCenter) *
                           0.5f;
    }

    float quaternion0[4];
    float quaternion1[4];
    float quaternion[4];
    ops.quatFromMatrix(quaternion0, matrix0);
    ops.quatFromMatrix(quaternion1, matrix1);
    BlendSdefQuaternions(quaternion, quaternion0, quaternion1, weight1);
    float blended[16];
    ops.matrixFromQuaternion(blended, quaternion);
    const float* rotation = blended;

    const float delta[3] = {
        position[0] - center[0],
        position[1] - center[1],
        position[2] - center[2]};
    for (int component = 0; component < 3; ++component) {
        out.position[component] =
            TransformComponent(delta, rotation, component, false) +
            pivot[component];
        out.normal[component] =
            TransformComponent(normal, rotation, component, false);
    }
}

}  // namespace legacy

// ---------------------------------------------------------------------------
// GPU mirror: a transcription of src/render/d3d12_skinning.hlsl over the
// gpu:: records, and the pre-affine shader for the legacy bit pin.
// ---------------------------------------------------------------------------
namespace mirror {

const float* Matrix(const gpu::Bone& bone) { return bone.matrix; }

bool Valid(const std::vector<gpu::Bone>& bones, int index) {
    return index >= 0 && static_cast<unsigned>(index) < bones.size();
}

bool Affine(const std::vector<gpu::Bone>& bones, int index) {
    return Valid(bones, index) && (bones[index].flags & 1u) != 0;
}

void Transform(const std::vector<gpu::Bone>& bones, const float p[3],
               int index, bool position, float out[3]) {
    if (!Valid(bones, index)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const float* m = Matrix(bones[index]);
    float v[3];
    v[0] = p[0] * m[0];
    v[1] = p[0] * m[1];
    v[2] = p[0] * m[2];
    v[0] += p[1] * m[4];
    v[1] += p[1] * m[5];
    v[2] += p[1] * m[6];
    v[0] += p[2] * m[8];
    v[1] += p[2] * m[9];
    v[2] += p[2] * m[10];
    if (position) {
        v[0] += m[12];
        v[1] += m[13];
        v[2] += m[14];
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
}

void TransformNormal(const std::vector<gpu::Bone>& bones, const float p[3],
                     int index, float out[3]) {
    if (!Valid(bones, index)) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    if ((bones[index].flags & 1u) == 0) {
        Transform(bones, p, index, false, out);
        return;
    }
    const float* r0 = bones[index].normalMatrix[0];
    const float* r1 = bones[index].normalMatrix[1];
    const float* r2 = bones[index].normalMatrix[2];
    float v[3];
    v[0] = p[0] * r0[0];
    v[1] = p[0] * r0[1];
    v[2] = p[0] * r0[2];
    v[0] += p[1] * r1[0];
    v[1] += p[1] * r1[1];
    v[2] += p[1] * r1[2];
    v[0] += p[2] * r2[0];
    v[1] += p[2] * r2[1];
    v[2] += p[2] * r2[2];
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
}

void BoneStretch(const std::vector<gpu::Bone>& bones, int index, float out[9]) {
    for (int i = 0; i < 9; ++i)
        out[i] = 0.0f;
    out[0] = out[4] = out[8] = 1.0f;
    if (!Affine(bones, index))
        return;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row * 3 + col] = bones[index].stretch[row][col];
}

void AdjugateMul(const float v[3], const float h[9], float out[3]) {
    const float a = h[0], b = h[4], c = h[8];
    const float d = h[1], e = h[2], f = h[5];
    const float adj0[3] = {b * c - f * f, e * f - d * c, d * f - e * b};
    const float adj1[3] = {e * f - d * c, a * c - e * e, d * e - a * f};
    const float adj2[3] = {d * f - e * b, d * e - a * f, a * b - d * d};
    float value[3];
    value[0] = v[0] * adj0[0];
    value[1] = v[0] * adj0[1];
    value[2] = v[0] * adj0[2];
    value[0] += v[1] * adj1[0];
    value[1] += v[1] * adj1[1];
    value[2] += v[1] * adj1[2];
    value[0] += v[2] * adj2[0];
    value[1] += v[2] * adj2[1];
    value[2] += v[2] * adj2[2];
    out[0] = value[0];
    out[1] = value[1];
    out[2] = value[2];
}

void NormalizeSafe(float v[3]) {
    const float length =
        std::sqrt(((v[0] * v[0]) + (v[1] * v[1])) + (v[2] * v[2]));
    if (!(length > 0.0f))
        return;
    const float inverse = 1.0f / length;
    v[0] *= inverse;
    v[1] *= inverse;
    v[2] *= inverse;
}

void Quaternion(const std::vector<gpu::Bone>& bones, int index, float out[4]) {
    if (Valid(bones, index)) {
        for (int i = 0; i < 4; ++i)
            out[i] = bones[index].quaternion[i];
        return;
    }
    out[0] = 0.5f;
    out[1] = out[2] = out[3] = 0.0f;
}

void Rotate(const float v[3], const float q[4], float out[3]) {
    const float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    const float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2];
    const float wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    const float r0[3] = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)};
    const float r1[3] = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)};
    const float r2[3] = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)};
    float result[3];
    result[0] = v[0] * r0[0];
    result[1] = v[0] * r0[1];
    result[2] = v[0] * r0[2];
    result[0] += v[1] * r1[0];
    result[1] += v[1] * r1[1];
    result[2] += v[1] * r1[2];
    result[0] += v[2] * r2[0];
    result[1] += v[2] * r2[1];
    result[2] += v[2] * r2[2];
    out[0] = result[0];
    out[1] = result[1];
    out[2] = result[2];
}

// One compute-shader thread, skinning part only (positions and normals).
void Run(const gpu::Vertex& v, const std::vector<gpu::Bone>& bones,
         float position[3], float normal[3]) {
    float oPosition[3] = {0.0f, 0.0f, 0.0f};
    float oNormal[3] = {0.0f, 0.0f, 0.0f};
    const int b0 = v.bones[0], b1 = v.bones[1];
    const int b2 = v.bones[2], b3 = v.bones[3];
    const float w0 = v.weights[0];
    if (v.type == 0) {
        Transform(bones, v.position, b0, true, oPosition);
        TransformNormal(bones, v.normal, b0, oNormal);
        if (Affine(bones, b0))
            NormalizeSafe(oNormal);
    } else if (v.type == 1) {
        const float w1 = 1 - w0;
        float a[3], b[3];
        Transform(bones, v.position, b1, true, a);
        Transform(bones, v.position, b0, true, b);
        for (int c = 0; c < 3; ++c)
            oPosition[c] = a[c] * w1 + b[c] * w0;
        TransformNormal(bones, v.normal, b1, a);
        TransformNormal(bones, v.normal, b0, b);
        for (int c = 0; c < 3; ++c)
            oNormal[c] = a[c] * w1 + b[c] * w0;
        if (Affine(bones, b0) || Affine(bones, b1))
            NormalizeSafe(oNormal);
    } else if (v.type == 2) {
        const int index[4] = {b0, b1, b2, b3};
        float t[3];
        Transform(bones, v.position, b1, true, t);
        for (int c = 0; c < 3; ++c)
            oPosition[c] = t[c] * v.weights[1];
        Transform(bones, v.position, b0, true, t);
        for (int c = 0; c < 3; ++c)
            oPosition[c] += t[c] * v.weights[0];
        Transform(bones, v.position, b2, true, t);
        for (int c = 0; c < 3; ++c)
            oPosition[c] += t[c] * v.weights[2];
        Transform(bones, v.position, b3, true, t);
        for (int c = 0; c < 3; ++c)
            oPosition[c] += t[c] * v.weights[3];

        TransformNormal(bones, v.normal, b0, t);
        for (int c = 0; c < 3; ++c)
            oNormal[c] = t[c] * v.weights[0];
        for (int lane = 1; lane < 4; ++lane) {
            TransformNormal(bones, v.normal, index[lane], t);
            for (int c = 0; c < 3; ++c)
                oNormal[c] += t[c] * v.weights[lane];
        }
        if (Affine(bones, b0) || Affine(bones, b1) || Affine(bones, b2) ||
            Affine(bones, b3))
            NormalizeSafe(oNormal);
    } else if (v.type == 3) {
        const float w1 = 1 - w0;
        float q0[4], q1[4], q[4];
        Quaternion(bones, b0, q0);
        Quaternion(bones, b1, q1);
        const float signDot =
            ((q1[0] * q0[0] + q1[3] * q0[3]) + q1[1] * q0[1]) + q1[2] * q0[2];
        for (int i = 0; i < 4; ++i) {
            q[i] = signDot >= 0.0f ? (q1[i] - q0[i]) * w1 + q0[i]
                                   : (q1[i] + q0[i]) * (-w1) + q0[i];
        }
        const float length = std::sqrt(
            ((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3]);
        if (length > 0.0f) {
            const float inverse = 1.0f / length;
            for (int i = 0; i < 4; ++i)
                q[i] *= inverse;
        } else {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
        }
        float pc[3], arms[3], t[3];
        Transform(bones, v.center, b0, true, t);
        Transform(bones, v.center, b1, true, pc);
        for (int c = 0; c < 3; ++c)
            pc[c] = t[c] * w0 + pc[c] * w1;
        Transform(bones, v.r0, b0, false, t);
        Transform(bones, v.r1, b1, false, arms);
        for (int c = 0; c < 3; ++c)
            arms[c] = t[c] * w0 + arms[c] * w1;
        float pivot[3];
        for (int c = 0; c < 3; ++c)
            pivot[c] = ((arms[c] + pc[c]) + pc[c]) * .5f;

        const float delta[3] = {v.position[0] - v.center[0],
                                v.position[1] - v.center[1],
                                v.position[2] - v.center[2]};
        if (!Affine(bones, b0) && !Affine(bones, b1)) {
            Rotate(delta, q, oPosition);
            for (int c = 0; c < 3; ++c)
                oPosition[c] += pivot[c];
            Rotate(v.normal, q, oNormal);
        } else {
            float h0[9], h1[9], h[9];
            BoneStretch(bones, b0, h0);
            BoneStretch(bones, b1, h1);
            for (int i = 0; i < 9; ++i)
                h[i] = h0[i] * w0 + h1[i] * w1;
            float d[3];
            affine::MulVec3(d, delta, h);
            Rotate(d, q, oPosition);
            for (int c = 0; c < 3; ++c)
                oPosition[c] += pivot[c];
            float u[3];
            AdjugateMul(v.normal, h, u);
            Rotate(u, q, oNormal);
            NormalizeSafe(oNormal);
        }
    }
    position[0] = oPosition[0];
    position[1] = oPosition[1];
    position[2] = oPosition[2];
    normal[0] = oNormal[0];
    normal[1] = oNormal[1];
    normal[2] = oNormal[2];
}

// The pre-affine shader, for the "flags 0 means the old data path" pin.
void RunLegacy(const gpu::Vertex& v, const std::vector<gpu::Bone>& bones,
               float position[3], float normal[3]) {
    const int b0 = v.bones[0], b1 = v.bones[1];
    const int b2 = v.bones[2], b3 = v.bones[3];
    const float w0 = v.weights[0];
    float oPosition[3] = {0.0f, 0.0f, 0.0f};
    float oNormal[3] = {0.0f, 0.0f, 0.0f};
    if (v.type == 0) {
        Transform(bones, v.position, b0, true, oPosition);
        Transform(bones, v.normal, b0, false, oNormal);
    } else if (v.type == 1) {
        const float w1 = 1 - w0;
        float a[3], b[3];
        Transform(bones, v.position, b1, true, a);
        Transform(bones, v.position, b0, true, b);
        for (int c = 0; c < 3; ++c)
            oPosition[c] = a[c] * w1 + b[c] * w0;
        Transform(bones, v.normal, b1, false, a);
        Transform(bones, v.normal, b0, false, b);
        for (int c = 0; c < 3; ++c)
            oNormal[c] = a[c] * w1 + b[c] * w0;
    } else if (v.type == 2) {
        const int index[4] = {b0, b1, b2, b3};
        float t[3];
        Transform(bones, v.position, b1, true, t);
        for (int c = 0; c < 3; ++c)
            oPosition[c] = t[c] * v.weights[1];
        for (int lane = 0; lane < 4; ++lane) {
            if (lane == 1)
                continue;
            Transform(bones, v.position, index[lane], true, t);
            for (int c = 0; c < 3; ++c)
                oPosition[c] += t[c] * v.weights[lane];
        }
        for (int lane = 0; lane < 4; ++lane) {
            Transform(bones, v.normal, index[lane], false, t);
            for (int c = 0; c < 3; ++c)
                oNormal[c] = lane == 0 ? t[c] * v.weights[lane]
                                       : oNormal[c] + t[c] * v.weights[lane];
        }
    } else if (v.type == 3) {
        const float w1 = 1 - w0;
        float q0[4], q1[4], q[4];
        Quaternion(bones, b0, q0);
        Quaternion(bones, b1, q1);
        const float signDot =
            ((q1[0] * q0[0] + q1[3] * q0[3]) + q1[1] * q0[1]) + q1[2] * q0[2];
        for (int i = 0; i < 4; ++i) {
            q[i] = signDot >= 0.0f ? (q1[i] - q0[i]) * w1 + q0[i]
                                   : (q1[i] + q0[i]) * (-w1) + q0[i];
        }
        const float length = std::sqrt(
            ((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3]);
        if (length > 0.0f) {
            const float inverse = 1.0f / length;
            for (int i = 0; i < 4; ++i)
                q[i] *= inverse;
        } else {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
        }
        float pc[3], arms[3], t[3];
        Transform(bones, v.center, b0, true, t);
        Transform(bones, v.center, b1, true, pc);
        for (int c = 0; c < 3; ++c)
            pc[c] = t[c] * w0 + pc[c] * w1;
        Transform(bones, v.r0, b0, false, t);
        Transform(bones, v.r1, b1, false, arms);
        for (int c = 0; c < 3; ++c)
            arms[c] = t[c] * w0 + arms[c] * w1;
        const float delta[3] = {v.position[0] - v.center[0],
                                v.position[1] - v.center[1],
                                v.position[2] - v.center[2]};
        Rotate(delta, q, oPosition);
        for (int c = 0; c < 3; ++c)
            oPosition[c] += ((arms[c] + pc[c]) + pc[c]) * .5f;
        Rotate(v.normal, q, oNormal);
    }
    position[0] = oPosition[0];
    position[1] = oPosition[1];
    position[2] = oPosition[2];
    normal[0] = oNormal[0];
    normal[1] = oNormal[1];
    normal[2] = oNormal[2];
}

}  // namespace mirror

// ---------------------------------------------------------------------------
// GPU record packing as PrepareGpuModelInput does it.
// ---------------------------------------------------------------------------
gpu::Bone PackGpuBone(const affine::BoneAux& aux, const float matrix[16]) {
    gpu::Bone bone{};
    std::memcpy(bone.matrix, matrix, sizeof(bone.matrix));
    std::memcpy(bone.quaternion, aux.rotation, sizeof(bone.quaternion));
    for (unsigned row = 0; row < 3; ++row) {
        std::memcpy(bone.stretch[row], &aux.stretch[row * 3], 12);
        std::memcpy(bone.normalMatrix[row], &aux.normalMatrix[row * 3], 12);
        bone.stretch[row][3] = 0.0f;
        bone.normalMatrix[row][3] = 0.0f;
    }
    bone.flags = aux.flags;
    return bone;
}

affine::BoneAux Precompute(const BoneCase& bone) {
    affine::BoneAux aux{};
    affine::PrecomputeBone(bone.matrix, bone.q, aux);
    return aux;
}

void FillVertex(gpu::Vertex& v, unsigned type, const float position[3],
                const float normal[3], const int bones[4], const float weight[4],
                const float center[3], const float r0[3], const float r1[3]) {
    v = gpu::Vertex{};
    v.type = type;
    std::memcpy(v.position, position, sizeof(v.position));
    std::memcpy(v.normal, normal, sizeof(v.normal));
    if (bones != nullptr)
        std::memcpy(v.bones, bones, sizeof(v.bones));
    if (weight != nullptr)
        std::memcpy(v.weights, weight, sizeof(v.weights));
    if (center != nullptr)
        std::memcpy(v.center, center, 12);
    if (r0 != nullptr) {
        std::memcpy(v.r0, r0, 12);
        std::memcpy(v.r1, r1, 12);
    }
}

// Loader semantics (pmx_load.cpp): the SDEF arms are rebased onto the weighted
// midpoint before skinning sees them.
void RebaseSdefArms(float r0[3], float r1[3], float weight0) {
    const float weight1 = 1.0f - weight0;
    for (int c = 0; c < 3; ++c) {
        const float mid = weight0 * r0[c] + weight1 * r1[c];
        r0[c] -= mid;
        r1[c] -= mid;
    }
}

// ---------------------------------------------------------------------------
// Scenarios.
// ---------------------------------------------------------------------------
const float kPosition[3] = {0.35f, -1.25f, 2.05f};
const float kNormal[3] = {0.4364357805f, -0.8728715611f, 0.2182178902f};
const float kCenter[3] = {0.2f, 0.1f, -0.3f};
const float kRawR0[3] = {1.1f, -0.4f, 0.6f};
const float kRawR1[3] = {-0.7f, 1.3f, -0.2f};
const float kIdentity9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

struct Family {
    const char* name;
    double stretch[9];
};

// Uniform scale, non-uniform scale and a sheared stretch: the three families
// the task requires. All are symmetric positive definite, so A = H * R keeps
// det > 0 as the affine contract demands.
const Family kFamilies[] = {
    {"uniform", {2.0, 0, 0, 0, 2.0, 0, 0, 0, 2.0}},
    {"non-uniform", {1.7, 0, 0, 0, 0.55, 0, 0, 0, 1.15}},
    {"shear", {1.2, 0.35, 0.12, 0.35, 1.6, -0.22, 0.12, -0.22, 0.8}},
};

void TestPolarDecomposition() {
    float qA[4], qB[4], qC[4];
    QuaternionFromAxis(qA, 1.0f, 2.0f, -0.5f, 1.1f);
    QuaternionFromAxis(qB, -0.3f, 0.7f, 1.4f, -2.2f);
    QuaternionFromAxis(qC, 0.2f, -1.1f, 0.8f, 0.35f);
    const float t[3] = {0.0f, 0.0f, 0.0f};
    const BoneCase bones[3] = {
        MakeBone(kFamilies[0].stretch, qA, t),
        MakeBone(kFamilies[1].stretch, qB, t),
        MakeBone(kFamilies[2].stretch, qC, t),
    };
    for (const BoneCase& bone : bones) {
        float linear[9];
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                linear[row * 3 + col] = bone.matrix[row * 4 + col];
        float stretch[9];
        float rotation[4];
        Check(affine::PolarDecompose(linear, stretch, rotation),
              "polar decomposition converges");
        // A = H * R reconstruction.
        float r[9];
        affine::RotationMatrixFromQuaternion(rotation, r);
        float rebuilt[9];
        affine::Mat3Mul(stretch, r, rebuilt);
        for (int i = 0; i < 9; ++i)
            Check(Near(rebuilt[i], linear[i], 1e-4, 1e-4),
                  "polar rebuilds the linear part");
        // H symmetric and positive definite (all leading minors positive).
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                Check(std::fabs(stretch[row * 3 + col] -
                                stretch[col * 3 + row]) < 1e-4f,
                      "stretch is symmetric");
        Check(affine::Mat3Det(stretch) > 0.0f, "stretch has positive det");
        Check(stretch[0] > 0.0f && stretch[4] > 0.0f && stretch[8] > 0.0f,
              "stretch diagonal positive");
        // Recovered quaternion matches the constructed one up to sign.
        double dot = 0.0;
        for (int i = 0; i < 4; ++i)
            dot += double(rotation[i]) * bone.q[i];
        Check(std::fabs(std::fabs(dot) - 1.0) < 1e-4,
              "polar rotation matches the constructed rotation");
        // Rigid detection rejects these and the normal matrix is the true
        // inverse transpose.
        affine::BoneAux aux = Precompute(bone);
        Check((aux.flags & affine::kBoneFlagAffine) != 0,
              "scaled bone is flagged affine");
        DMatrix linearD = DFrom9(linear);
        DMatrix inverse{};
        Check(DInverse(linearD, inverse), "reference inverse exists");
        DMatrix expected = DTranspose(inverse);
        for (int i = 0; i < 9; ++i)
            Check(Near(aux.normalMatrix[i], expected.m[i], 1e-4, 1e-5),
                  "normal matrix is the inverse transpose");
    }
}

// Polar accuracy on every rotation branch: X/Y/Z axis rotations (including
// exactly pi, which forces the largest-diagonal extraction branches) and
// arbitrary-axis rotations, each crossed with the three stretch families.
// The determinant check is done in double: extreme composites underflow a
// float determinant to zero even when every eigenvalue is positive.
double Det9(const float m[9]) {
    return double(m[0]) * (double(m[4]) * m[8] - double(m[5]) * m[7]) -
           double(m[1]) * (double(m[3]) * m[8] - double(m[5]) * m[6]) +
           double(m[2]) * (double(m[3]) * m[7] - double(m[4]) * m[6]);
}

void CheckPolarReconstruction(const float linear[9], double tolerance) {
    float stretch[9];
    float rotation[4];
    Check(affine::PolarDecompose(linear, stretch, rotation),
          "polar decomposition converges");
    float rotation3[9];
    affine::RotationMatrixFromQuaternion(rotation, rotation3);
    float rebuilt[9];
    affine::Mat3Mul(stretch, rotation3, rebuilt);
    // The absolute tolerance scales with the matrix magnitude so small-scale
    // bones are checked as tightly as large ones.
    double magnitude = 0.0;
    for (int i = 0; i < 9; ++i)
        magnitude = std::max(magnitude, std::fabs(double(linear[i])));
    for (int i = 0; i < 9; ++i)
        Check(Near(rebuilt[i], linear[i], tolerance, tolerance * magnitude),
              "polar rebuilds A = H * R");
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            Check(std::fabs(stretch[row * 3 + col] - stretch[col * 3 + row]) <
                      1e-4f,
                  "stretch is symmetric");
    Check(Det9(stretch) > 0.0, "stretch has positive det");
}

void TestPolarAxisCoverage() {
    const float pi = 3.14159265358979f;
    struct Rotation {
        float x, y, z, angle;
    };
    const Rotation rotations[] = {
        {1, 0, 0, 0.01f},   {1, 0, 0, pi * 0.5f}, {1, 0, 0, pi},
        {0, 1, 0, 1.3f},    {0, 1, 0, pi},
        {0, 0, 1, -2.2f},   {0, 0, 1, pi},
        {1, 1, 1, pi},      {1, 1, 1, pi - 1e-3f},
        {0.3f, -0.8f, 0.5f, 2.9f}, {1, 2, -0.5f, 0.35f},
    };
    for (const Rotation& rotation : rotations) {
        float q[4];
        QuaternionFromAxis(q, rotation.x, rotation.y, rotation.z,
                           rotation.angle);
        const DMatrix rotationD = DRotation(q);
        float rotationF[9];
        for (int i = 0; i < 9; ++i)
            rotationF[i] = static_cast<float>(rotationD.m[i]);
        // Pure rotation: extract and rebuild without any stretch.
        float extracted[4];
        affine::QuaternionFromRotationMatrix(rotationF, extracted);
        double dot = 0.0;
        for (int i = 0; i < 4; ++i)
            dot += double(extracted[i]) * double(q[i]);
        Check(std::fabs(std::fabs(dot) - 1.0) < 1e-4,
              "axis rotation extracts back to the same rotation");
        float rebuiltQ[9];
        affine::RotationMatrixFromQuaternion(extracted, rebuiltQ);
        for (int i = 0; i < 9; ++i)
            Check(Near(rebuiltQ[i], rotationF[i], 1e-5, 1e-5),
                  "rotation matrix round trip");
        // A = H * R reconstruction with every stretch family.
        for (const Family& family : kFamilies) {
            DMatrix stretchD{};
            for (int i = 0; i < 9; ++i)
                stretchD.m[i] = family.stretch[i];
            const DMatrix linearD = DMul(stretchD, rotationD);
            float linear[9];
            for (int i = 0; i < 9; ++i)
                linear[i] = static_cast<float>(linearD.m[i]);
            CheckPolarReconstruction(linear, 1e-4);
        }
    }
}

// Hierarchical bone composites reach extreme magnitudes. The iteration is
// norm-balanced and must either converge accurately or fail into the legal
// legacy fallback - never NaN, never a silently wrong affine record.
void CheckLegacyFallback(const float matrix[16], const char* message) {
    const float rigidQuat[4] = {0.1f, 0.2f, 0.3f, 0.9f};
    affine::BoneAux aux{};
    affine::PrecomputeBone(matrix, rigidQuat, aux);
    Check(aux.flags == 0, message);
    Check(BitEqual(aux.rotation, rigidQuat, sizeof(aux.rotation)),
          "fallback keeps the supplied rigid quaternion");
    Check(BitEqual(aux.stretch, kIdentity9, sizeof(aux.stretch)),
          "fallback keeps an identity stretch");
    for (int i = 0; i < 9; ++i)
        Check(std::isfinite(aux.normalMatrix[i]),
              "fallback keeps a finite normal matrix");
}

void TestPolarExtremeHierarchy() {
    float qA[4], qB[4], qC[4];
    QuaternionFromAxis(qA, 1.0f, 0.5f, -0.25f, 1.7f);
    QuaternionFromAxis(qB, -0.6f, 1.2f, 0.3f, -2.4f);
    QuaternionFromAxis(qC, 0.2f, -1.1f, 0.8f, 0.35f);
    // Three-level composite: H1*R1*H2*R2*H3*R3 with per-level scale spans of
    // four to five orders of magnitude, then uniform pushes to the float
    // extremes.
    const double levelStretch[3][9] = {
        {1000, 0, 0, 0, 0.1, 0, 0, 0, 30},
        {0.001, 0, 0, 0, 40, 0, 0, 0, 0.02},
        {7, 0, 0, 0, 0.05, 0, 0, 0, 0.0007},
    };
    const float* rotations[3] = {qA, qB, qC};
    const double uniform[3] = {1.0, 1e18, 1e-18};
    for (double factor : uniform) {
        DMatrix composite{};
        composite.m[0] = composite.m[4] = composite.m[8] = 1.0;
        for (int level = 0; level < 3; ++level) {
            DMatrix stretch{};
            for (int i = 0; i < 9; ++i)
                stretch.m[i] = levelStretch[level][i] * (level == 0 ? factor : 1.0);
            composite = DMul(composite, stretch);
            composite = DMul(composite, DRotation(rotations[level]));
        }
        float linear[9];
        for (int i = 0; i < 9; ++i)
            linear[i] = static_cast<float>(composite.m[i]);
        CheckPolarReconstruction(linear, 5e-3);

        float matrix[16] = {};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                matrix[row * 4 + col] = linear[row * 3 + col];
        matrix[15] = 1.0f;
        const float rigidQuat[4] = {0, 0, 0, 1};
        affine::BoneAux aux{};
        affine::PrecomputeBone(matrix, rigidQuat, aux);
        Check((aux.flags & affine::kBoneFlagAffine) != 0,
              "extreme composite takes the affine path");
        for (int i = 0; i < 9; ++i)
            Check(std::isfinite(aux.stretch[i]) &&
                      std::isfinite(aux.normalMatrix[i]),
                  "extreme composite stays finite");
    }

    // Legal fallbacks: singular, mirrored and zero matrices keep the legacy
    // rigid path; a legitimate extreme shrink does not.
    const float zero[16] = {};
    CheckLegacyFallback(zero, "zero matrix falls back to the legacy path");
    float singular[16] = {};
    singular[0] = 1.0f;
    singular[5] = 1.0f;
    singular[15] = 1.0f;  // rank 2: det = 0
    CheckLegacyFallback(singular,
                        "singular matrix falls back to the legacy path");
    float mirrored[16] = {};
    mirrored[0] = 1.0f;
    mirrored[5] = 1.0f;
    mirrored[10] = -1.0f;
    mirrored[15] = 1.0f;
    CheckLegacyFallback(mirrored,
                        "mirrored matrix falls back to the legacy path");

    for (double scale : {0.001, 1e-18}) {
        float matrix[16] = {};
        for (int i = 0; i < 3; ++i)
            matrix[i * 4 + i] = static_cast<float>(scale);
        matrix[15] = 1.0f;
        const float rigidQuat[4] = {0, 0, 0, 1};
        affine::BoneAux aux{};
        affine::PrecomputeBone(matrix, rigidQuat, aux);
        Check((aux.flags & affine::kBoneFlagAffine) != 0,
              "legitimate extreme shrink stays on the affine path");
        Check(Near(aux.stretch[0], scale, 1e-3, 1e-30) &&
                  Near(aux.stretch[4], scale, 1e-3, 1e-30) &&
                  Near(aux.stretch[8], scale, 1e-3, 1e-30),
              "extreme shrink stretch keeps the scale");
    }
}

// Review spot check: diag(10000, 0.0001, 0.0001) is a legitimate anisotropic
// scale (bone_scale::Valid accepts it) and must take the affine path and
// rebuild A = H * R, not fall through a singularity gate.
void TestDiagonalAnisotropicScale() {
    const float diagonal[9] = {10000.0f, 0, 0,
                               0, 0.0001f, 0,
                               0, 0, 0.0001f};
    float stretch[9];
    float rotation[4];
    Check(affine::PolarDecompose(diagonal, stretch, rotation),
          "polar decomposition converges for diag(10000, 0.0001, 0.0001)");
    float rotation3[9];
    affine::RotationMatrixFromQuaternion(rotation, rotation3);
    float rebuilt[9];
    affine::Mat3Mul(stretch, rotation3, rebuilt);
    for (int i = 0; i < 9; ++i)
        Check(Near(rebuilt[i], diagonal[i], 1e-4, 1e-8),
              "polar rebuilds diag(10000, 0.0001, 0.0001) as H * R");
    float matrix[16] = {};
    for (int i = 0; i < 3; ++i)
        matrix[i * 4 + i] = diagonal[i * 3 + i];
    matrix[15] = 1.0f;
    const float rigidQuat[4] = {0, 0, 0, 1};
    affine::BoneAux aux{};
    affine::PrecomputeBone(matrix, rigidQuat, aux);
    Check((aux.flags & affine::kBoneFlagAffine) != 0,
          "diag(10000, 0.0001, 0.0001) takes the affine path");
    Check(Near(aux.stretch[0], 10000.0f, 1e-3, 1e-3) &&
              Near(aux.stretch[4], 0.0001f, 1e-3, 1e-12) &&
              Near(aux.stretch[8], 0.0001f, 1e-3, 1e-12),
          "diag(10000, 0.0001, 0.0001) stretch keeps the anisotropy");
}

// Scale-magnitude regression: the runtime admits any invertible scale, so a
// small-scale bone (det 2.1e-10, far below the old fixed 1e-9 gate) and a
// large-scale bone must both take the affine path with a correct
// double-precision normal matrix and a legal unit rotation.
void TestSmallAndLargeScaleRegression() {
    const double smallH[9] = {0.0005, 0, 0, 0, 0.0006, 0, 0, 0, 0.0007};
    const double largeH[9] = {5000, 0, 0, 0, 6000, 0, 0, 0, 7000};
    float q[4];
    QuaternionFromAxis(q, 0.4f, -1.1f, 0.6f, 1.9f);
    const float t[3] = {0.5f, -0.25f, 1.75f};
    const double* families[2] = {smallH, largeH};
    for (const double* h : families) {
        const BoneCase bone = MakeBone(h, q, t);
        affine::BoneAux aux = Precompute(bone);
        Check((aux.flags & affine::kBoneFlagAffine) != 0,
              "small and large legal scales take the affine path");
        float linear[9];
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                linear[row * 3 + col] = bone.matrix[row * 4 + col];
        CheckPolarReconstruction(linear, 1e-4);
        DMatrix linearD = DFrom9(linear);
        DMatrix inverse{};
        Check(DInverse(linearD, inverse),
              "small and large scales are invertible");
        const DMatrix expected = DTranspose(inverse);
        for (int i = 0; i < 9; ++i)
            Check(Near(aux.normalMatrix[i], expected.m[i], 1e-4, 1e-9),
                  "normal matrix is the double-precision inverse transpose");
        double norm = 0.0;
        for (int i = 0; i < 4; ++i)
            norm += double(aux.rotation[i]) * double(aux.rotation[i]);
        Check(Near(std::sqrt(norm), 1.0, 1e-5, 1e-5),
              "rotation quaternion stays unit");
    }
}

void TestRigidPathUnchanged() {
    const affine::RotationOps ops = affine::DefaultRotationOps();
    float q0[4], q1[4], q2[4], q3[4];
    QuaternionFromAxis(q0, 1.0f, 0.0f, 0.0f, 0.7f);
    QuaternionFromAxis(q1, 0.0f, 1.0f, 0.0f, -1.3f);
    QuaternionFromAxis(q2, 0.0f, 0.0f, 1.0f, 2.4f);
    QuaternionFromAxis(q3, 1.0f, 1.0f, 1.0f, -0.6f);
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float identityF[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float t0[3] = {0.25f, -1.5f, 3.0f};
    const float t1[3] = {-2.0f, 0.75f, -0.5f};
    const float t2[3] = {1.125f, 2.25f, -3.5f};
    const float t3[3] = {0.0f, 1.0f, -1.0f};
    const BoneCase bones[4] = {
        MakeBone(identity, q0, t0),
        MakeBone(identity, q1, t1),
        MakeBone(identity, q2, t2),
        MakeBone(identity, q3, t3),
    };
    affine::BoneAux aux[4];
    for (int i = 0; i < 4; ++i) {
        aux[i] = Precompute(bones[i]);
        Check(aux[i].flags == 0, "rigid bone stays on the legacy path");
        // A rigid bone keeps its quaternion verbatim, an identity stretch and
        // the linear part as its normal matrix.
        Check(BitEqual(aux[i].rotation, bones[i].q, sizeof(aux[i].rotation)),
              "rigid bone keeps the supplied quaternion");
        Check(BitEqual(aux[i].stretch, identityF, sizeof(aux[i].stretch)),
              "rigid bone keeps an identity stretch");
    }

    affine::BoneRef refs[4];
    for (int i = 0; i < 4; ++i)
        refs[i] = {bones[i].matrix, &aux[i]};

    const float weight4[4] = {0.15f, 0.35f, 0.25f, 0.25f};
    const float weight2[4] = {0.625f, 0.375f, 0.0f, 0.0f};
    affine::Skinned got{};
    affine::Skinned expected{};

    affine::SkinBdef1(refs[0], kPosition, kNormal, got);
    legacy::Bdef1(bones[0].matrix, kPosition, kNormal, expected);
    Check(BitEqual(&got, &expected, sizeof(got)), "BDEF1 rigid path bit-exact");

    affine::SkinBdef2(refs[0], refs[1], weight2[0], kPosition, kNormal, got);
    legacy::Bdef2(bones[0].matrix, bones[1].matrix, weight2[0], kPosition,
                  kNormal, expected);
    Check(BitEqual(&got, &expected, sizeof(got)),
          "BDEF2/PMD rigid path bit-exact");

    affine::SkinBdef4(refs, weight4, kPosition, kNormal, got);
    legacy::Bdef4(bones[0].matrix, bones[1].matrix, bones[2].matrix,
                  bones[3].matrix, weight4, kPosition, kNormal, expected);
    Check(BitEqual(&got, &expected, sizeof(got)), "BDEF4 rigid path bit-exact");

    float r0[3], r1[3];
    std::memcpy(r0, kRawR0, sizeof(r0));
    std::memcpy(r1, kRawR1, sizeof(r1));
    RebaseSdefArms(r0, r1, weight2[0]);
    affine::SkinSdef(refs[0], refs[1], weight2[0], kPosition, kNormal, kCenter,
                     r0, r1, ops, got);
    legacy::Sdef(bones[0].matrix, bones[1].matrix, weight2[0], kPosition,
                 kNormal, kCenter, r0, r1, ops, expected);
    Check(BitEqual(&got, &expected, sizeof(got)), "SDEF rigid path bit-exact");

    // A missing bone reference is the zero matrix with rigid aux, exactly what
    // the production lookup hands to the kernels; the zero-matrix legacy
    // behaviour must survive.
    static const float zeroMatrix[16] = {};
    affine::BoneAux missingAux{};
    affine::Mat3Identity(missingAux.stretch);
    missingAux.rotation[0] = 0.5f;
    missingAux.rotation[1] = missingAux.rotation[2] = missingAux.rotation[3] = 0.0f;
    const affine::BoneRef missingRef = {zeroMatrix, &missingAux};
    affine::Skinned gotMissing{};
    affine::SkinBdef2(refs[0], missingRef, weight2[0], kPosition, kNormal,
                      gotMissing);
    legacy::Bdef2(bones[0].matrix, zeroMatrix, weight2[0], kPosition, kNormal,
                  expected);
    Check(BitEqual(&gotMissing, &expected, sizeof(gotMissing)),
          "missing bone reference keeps the legacy zero matrix");
    affine::Skinned gotMissingBdef1{};
    affine::SkinBdef1(missingRef, kPosition, kNormal, gotMissingBdef1);
    legacy::Bdef1(zeroMatrix, kPosition, kNormal, expected);
    Check(BitEqual(&gotMissingBdef1, &expected, sizeof(gotMissingBdef1)),
          "missing bone BDEF1 keeps the legacy zero matrix");
}

// Full reference for the affine BDEF/PMD paths: positions use the full
// matrix, normals use the per-bone inverse transpose, blended and normalized.
void ReferenceBdef(const BoneCase* bones, const float weight[4], int count,
                   const float position[3], const float normal[3],
                   double outPosition[3], double outNormal[3]) {
    for (int c = 0; c < 3; ++c) {
        outPosition[c] = 0.0;
        outNormal[c] = 0.0;
    }
    for (int i = 0; i < count; ++i) {
        DMatrix linear{};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                linear.m[row * 3 + col] = bones[i].matrix[row * 4 + col];
        DMatrix inverse{};
        Check(DInverse(linear, inverse), "reference bone inverse exists");
        const DMatrix normalMatrix = DTranspose(inverse);
        for (int c = 0; c < 3; ++c) {
            double value = 0.0;
            for (int k = 0; k < 3; ++k)
                value += double(position[k]) * linear.m[k * 3 + c];
            outPosition[c] += (value + bones[i].matrix[12 + c]) * weight[i];
            double normalValue = 0.0;
            for (int k = 0; k < 3; ++k)
                normalValue += double(normal[k]) * normalMatrix.m[k * 3 + c];
            outNormal[c] += normalValue * weight[i];
        }
    }
    DNormalize(outNormal);
}

void TestAffineBdef() {
    float q0[4], q1[4], q2[4], q3[4];
    QuaternionFromAxis(q0, 0.0f, 1.0f, 0.0f, 0.9f);
    QuaternionFromAxis(q1, 1.0f, 0.0f, 1.0f, -1.7f);
    QuaternionFromAxis(q2, 0.0f, 0.0f, 1.0f, 2.2f);
    QuaternionFromAxis(q3, 1.0f, -1.0f, 0.5f, 0.4f);
    const float t0[3] = {0.25f, -1.5f, 3.0f};
    const float t1[3] = {-2.0f, 0.75f, -0.5f};
    const float t2[3] = {1.125f, 2.25f, -3.5f};
    const float t3[3] = {0.0f, 1.0f, -1.0f};

    for (const Family& family : kFamilies) {
        const BoneCase bones[4] = {
            MakeBone(family.stretch, q0, t0),
            MakeBone(family.stretch, q1, t1),
            MakeBone(family.stretch, q2, t2),
            MakeBone(family.stretch, q3, t3),
        };
        affine::BoneAux aux[4];
        gpu::Bone gpuBones[4];
        for (int i = 0; i < 4; ++i) {
            aux[i] = Precompute(bones[i]);
            Check((aux[i].flags & affine::kBoneFlagAffine) != 0,
                  "affine family is flagged affine");
            gpuBones[i] = PackGpuBone(aux[i], bones[i].matrix);
        }
        std::vector<gpu::Bone> record(gpuBones, gpuBones + 4);
        affine::BoneRef refs[4];
        for (int i = 0; i < 4; ++i)
            refs[i] = {bones[i].matrix, &aux[i]};

        const float weight2[2][4] = {{0.55f, 0.45f, 0.0f, 0.0f},
                                     {1.0f, 0.0f, 0.0f, 0.0f}};
        const float weight4[4] = {0.1f, 0.4f, 0.3f, 0.2f};

        struct Variant {
            unsigned type;
            const float* weight;
            int count;
        };
        const Variant variants[4] = {
            {0, weight2[1], 1},
            {1, weight2[0], 2},
            {1, weight2[1], 2},
            {2, weight4, 4},
        };
        for (const Variant& variant : variants) {
            gpu::Vertex vertex{};
            FillVertex(vertex, variant.type, kPosition, kNormal, nullptr,
                       variant.weight, nullptr, nullptr, nullptr);
            for (int i = 0; i < variant.count; ++i)
                vertex.bones[i] = i;

            affine::Skinned got{};
            if (variant.type == 0) {
                affine::SkinBdef1(refs[0], kPosition, kNormal, got);
            } else if (variant.type == 1) {
                affine::SkinBdef2(refs[0], refs[1], variant.weight[0],
                                  kPosition, kNormal, got);
            } else {
                affine::SkinBdef4(refs, variant.weight, kPosition, kNormal,
                                  got);
            }

            double expectedPosition[3];
            double expectedNormal[3];
            ReferenceBdef(bones, variant.weight, variant.count, kPosition,
                          kNormal, expectedPosition, expectedNormal);
            Check(NearVec(got.position, expectedPosition, 2e-4, 2e-4),
                  "affine BDEF position matches the double reference");
            Check(NearVec(got.normal, expectedNormal, 2e-4, 2e-4),
                  "affine BDEF normal matches the double reference");

            float gpuPosition[3];
            float gpuNormal[3];
            mirror::Run(vertex, record, gpuPosition, gpuNormal);
            Check(NearVec(gpuPosition, expectedPosition, 2e-4, 2e-4),
                  "GPU BDEF position matches the double reference");
            Check(NearVec(gpuNormal, expectedNormal, 2e-4, 2e-4),
                  "GPU BDEF normal matches the double reference");
            Check(NearVec(gpuPosition, got.position, 2e-5, 2e-5) &&
                      NearVec(gpuNormal, got.normal, 2e-5, 2e-5),
                  "CPU and GPU BDEF results agree");
        }
    }
}

// SDEF reference: the composite is Hblend * Rblend with the loader-rebased
// arms; the normal uses the inverse transpose of that composite.
void ReferenceSdef(const BoneCase& bone0, const BoneCase& bone1, float weight0,
                   const float position[3], const float normal[3],
                   const float center[3], const float r0[3], const float r1[3],
                   double outPosition[3], double outNormal[3]) {
    const double w0 = weight0;
    const double w1 = 1.0 - w0;
    double pivot[3];
    for (int c = 0; c < 3; ++c) {
        double blendedCenter = 0.0;
        double blendedArms = 0.0;
        for (int k = 0; k < 3; ++k) {
            blendedCenter += double(center[k]) * bone0.matrix[k * 4 + c] * w0;
            blendedCenter += double(center[k]) * bone1.matrix[k * 4 + c] * w1;
            blendedArms += double(r0[k]) * bone0.matrix[k * 4 + c] * w0;
            blendedArms += double(r1[k]) * bone1.matrix[k * 4 + c] * w1;
        }
        blendedCenter += (bone0.matrix[12 + c] * w0 + bone1.matrix[12 + c] * w1);
        pivot[c] = ((blendedArms + blendedCenter) + blendedCenter) * 0.5;
    }
    DMatrix stretch{};
    for (int i = 0; i < 9; ++i)
        stretch.m[i] = bone0.h[i] * w0 + bone1.h[i] * w1;
    // Reference quaternion blend (same nlerp as the shipped helper, in double).
    double q0[4], q1[4], q[4];
    for (int i = 0; i < 4; ++i) {
        q0[i] = bone0.q[i];
        q1[i] = bone1.q[i];
    }
    const double dot = ((q1[0] * q0[0] + q1[3] * q0[3]) + q1[1] * q0[1]) +
                       q1[2] * q0[2];
    const double sign = dot >= 0.0 ? 1.0 : -1.0;
    for (int i = 0; i < 4; ++i)
        q[i] = (q1[i] - sign * q0[i]) * w1 + q0[i];
    const double length =
        std::sqrt(((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] /= length;
    DMatrix rotationD{};
    {
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double xx = x * x, yy = y * y, zz = z * z;
        const double xy = x * y, xz = x * z, yz = y * z;
        const double wx = w * x, wy = w * y, wz = w * z;
        rotationD.m[0] = 1.0 - 2.0 * (yy + zz);
        rotationD.m[1] = 2.0 * (xy + wz);
        rotationD.m[2] = 2.0 * (xz - wy);
        rotationD.m[3] = 2.0 * (xy - wz);
        rotationD.m[4] = 1.0 - 2.0 * (xx + zz);
        rotationD.m[5] = 2.0 * (yz + wx);
        rotationD.m[6] = 2.0 * (xz + wy);
        rotationD.m[7] = 2.0 * (yz - wx);
        rotationD.m[8] = 1.0 - 2.0 * (xx + yy);
    }
    const DMatrix composite = DMul(stretch, rotationD);
    double delta[3];
    for (int c = 0; c < 3; ++c)
        delta[c] = double(position[c]) - double(center[c]);
    double deformed[3];
    DMulVec(deformed, delta, composite);
    for (int c = 0; c < 3; ++c)
        outPosition[c] = deformed[c] + pivot[c];
    DMatrix inverse{};
    Check(DInverse(composite, inverse), "SDEF composite inverse exists");
    const DMatrix normalMatrix = DTranspose(inverse);
    DMulVec(outNormal, normal, normalMatrix);
    DNormalize(outNormal);
}

void TestSdefAffine() {
    float q0[4], q1[4];
    QuaternionFromAxis(q0, 0.0f, 1.0f, 0.0f, 0.9f);
    QuaternionFromAxis(q1, 1.0f, 0.0f, 1.0f, -1.7f);
    const float t0[3] = {0.25f, -1.5f, 3.0f};
    const float t1[3] = {-2.0f, 0.75f, -0.5f};
    const affine::RotationOps ops = affine::DefaultRotationOps();

    for (const Family& family : kFamilies) {
        const BoneCase bones[2] = {
            MakeBone(family.stretch, q0, t0),
            MakeBone(family.stretch, q1, t1),
        };
        affine::BoneAux aux[2] = {Precompute(bones[0]), Precompute(bones[1])};
        affine::BoneRef refs[2] = {{bones[0].matrix, &aux[0]},
                                   {bones[1].matrix, &aux[1]}};
        std::vector<gpu::Bone> record = {
            PackGpuBone(aux[0], bones[0].matrix),
            PackGpuBone(aux[1], bones[1].matrix),
        };
        const float weights[2] = {0.375f, 0.625f};
        for (float weight0 : weights) {
            float r0[3], r1[3];
            std::memcpy(r0, kRawR0, sizeof(r0));
            std::memcpy(r1, kRawR1, sizeof(r1));
            RebaseSdefArms(r0, r1, weight0);

            affine::Skinned got{};
            affine::SkinSdef(refs[0], refs[1], weight0, kPosition, kNormal,
                             kCenter, r0, r1, ops, got);

            double expectedPosition[3];
            double expectedNormal[3];
            ReferenceSdef(bones[0], bones[1], weight0, kPosition, kNormal,
                          kCenter, r0, r1, expectedPosition, expectedNormal);
            Check(NearVec(got.position, expectedPosition, 2e-4, 2e-4),
                  "affine SDEF position matches the double reference");
            Check(NearVec(got.normal, expectedNormal, 2e-4, 2e-4),
                  "affine SDEF normal matches the double reference");

            gpu::Vertex vertex{};
            const int vertexBones[2] = {0, 1};
            const float vertexWeight[4] = {weight0, 0.0f, 0.0f, 0.0f};
            FillVertex(vertex, 3, kPosition, kNormal, vertexBones,
                       vertexWeight, kCenter, r0, r1);
            float gpuPosition[3];
            float gpuNormal[3];
            mirror::Run(vertex, record, gpuPosition, gpuNormal);
            Check(NearVec(gpuPosition, expectedPosition, 2e-4, 2e-4),
                  "GPU SDEF position matches the double reference");
            Check(NearVec(gpuNormal, expectedNormal, 2e-4, 2e-4),
                  "GPU SDEF normal matches the double reference");
            Check(NearVec(gpuPosition, got.position, 2e-5, 2e-5) &&
                      NearVec(gpuNormal, got.normal, 2e-5, 2e-5),
                  "CPU and GPU SDEF results agree");
        }
    }
}

// When both bones carry the same transform - including scaling or shear - the
// SDEF centre/arms compensation must degenerate to the single affine map of
// that bone. The loader-rebased arms (weighted offset sum zero) are what makes
// the pivot collapse to the affine image of the centre.
void TestSdefSameTransformDegenerate() {
    const affine::RotationOps ops = affine::DefaultRotationOps();
    float q[4];
    QuaternionFromAxis(q, 1.0f, -2.0f, 0.5f, 1.25f);
    const float t[3] = {0.75f, -0.25f, 1.5f};
    for (const Family& family : kFamilies) {
        const BoneCase bone = MakeBone(family.stretch, q, t);
        affine::BoneAux aux = Precompute(bone);
        const affine::BoneRef ref = {bone.matrix, &aux};
        for (float weight0 : {0.25f, 0.5f, 0.8f}) {
            float r0[3], r1[3];
            std::memcpy(r0, kRawR0, sizeof(r0));
            std::memcpy(r1, kRawR1, sizeof(r1));
            RebaseSdefArms(r0, r1, weight0);

            affine::Skinned got{};
            affine::SkinSdef(ref, ref, weight0, kPosition, kNormal, kCenter, r0,
                             r1, ops, got);

            // Expected: the plain affine transform of the vertex and the
            // inverse-transpose normal of that same transform.
            DMatrix linear{};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    linear.m[row * 3 + col] = bone.matrix[row * 4 + col];
            double expectedPosition[3];
            for (int c = 0; c < 3; ++c) {
                expectedPosition[c] = 0.0;
                for (int k = 0; k < 3; ++k)
                    expectedPosition[c] +=
                        double(kPosition[k]) * linear.m[k * 3 + c];
                expectedPosition[c] += bone.matrix[12 + c];
            }
            DMatrix inverse{};
            Check(DInverse(linear, inverse), "degenerate inverse exists");
            const DMatrix normalMatrix = DTranspose(inverse);
            double expectedNormal[3];
            DMulVec(expectedNormal, kNormal, normalMatrix);
            DNormalize(expectedNormal);

            Check(NearVec(got.position, expectedPosition, 1e-4, 1e-4),
                  "same-transform SDEF degenerates to the affine transform");
            Check(NearVec(got.normal, expectedNormal, 1e-4, 1e-4),
                  "same-transform SDEF normal degenerates to the inverse "
                  "transpose");
        }
    }
}

// A default-constructed GPU record carries flags 0 and must select the legacy
// rigid shader path bit for bit - old submissions keep working unchanged.
void TestGpuLegacyDefaults() {
    const gpu::Bone empty{};
    Check(empty.flags == 0, "default bone record selects the legacy path");
    Check(empty.stretch[0][0] == 0.0f && empty.normalMatrix[0][0] == 0.0f,
          "default bone record has zero affine extension");

    float q0[4], q1[4];
    QuaternionFromAxis(q0, 1.0f, 0.5f, -0.25f, 0.8f);
    QuaternionFromAxis(q1, -0.4f, 1.2f, 0.6f, -1.9f);
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float t0[3] = {0.5f, -1.0f, 2.0f};
    const float t1[3] = {-1.5f, 0.25f, -0.75f};
    const BoneCase bones[2] = {
        MakeBone(identity, q0, t0),
        MakeBone(identity, q1, t1),
    };
    // Legacy records: only matrix and quaternion filled, as before the
    // extension.
    std::vector<gpu::Bone> legacyRecords(2);
    for (int i = 0; i < 2; ++i) {
        legacyRecords[i] = gpu::Bone{};
        std::memcpy(legacyRecords[i].matrix, bones[i].matrix, 64);
        affine::DefaultQuatFromMatrix(legacyRecords[i].quaternion,
                                      bones[i].matrix);
    }
    const float weight2[4] = {0.3f, 0.7f, 0.0f, 0.0f};
    const float weight4[4] = {0.2f, 0.3f, 0.4f, 0.1f};
    struct Variant {
        unsigned type;
        const float* weight;
    };
    const Variant variants[3] = {{1, weight2}, {2, weight4}, {0, weight2}};
    for (const Variant& variant : variants) {
        gpu::Vertex vertex{};
        FillVertex(vertex, variant.type, kPosition, kNormal, nullptr,
                   variant.weight, nullptr, nullptr, nullptr);
        int vertexBones[4] = {0, 1, 0, 1};
        std::memcpy(vertex.bones, vertexBones, sizeof(vertexBones));
        float gotPosition[3];
        float gotNormal[3];
        float expectedPosition[3];
        float expectedNormal[3];
        mirror::Run(vertex, legacyRecords, gotPosition, gotNormal);
        mirror::RunLegacy(vertex, legacyRecords, expectedPosition, expectedNormal);
        Check(BitEqual(gotPosition, expectedPosition, sizeof(gotPosition)) &&
                  BitEqual(gotNormal, expectedNormal, sizeof(gotNormal)),
              "flags 0 GPU record reproduces the legacy shader bit-exactly");
    }

    // SDEF on legacy records: the legacy shader path as well.
    float r0[3], r1[3];
    std::memcpy(r0, kRawR0, sizeof(r0));
    std::memcpy(r1, kRawR1, sizeof(r1));
    RebaseSdefArms(r0, r1, weight2[0]);
    gpu::Vertex vertex{};
    const int vertexBones[4] = {0, 1, 0, 1};
    const float vertexWeight[4] = {weight2[0], 0.0f, 0.0f, 0.0f};
    FillVertex(vertex, 3, kPosition, kNormal, vertexBones, vertexWeight,
               kCenter, r0, r1);
    float gotPosition[3];
    float gotNormal[3];
    float expectedPosition[3];
    float expectedNormal[3];
    mirror::Run(vertex, legacyRecords, gotPosition, gotNormal);
    mirror::RunLegacy(vertex, legacyRecords, expectedPosition, expectedNormal);
    Check(BitEqual(gotPosition, expectedPosition, sizeof(gotPosition)) &&
              BitEqual(gotNormal, expectedNormal, sizeof(gotNormal)),
          "flags 0 GPU SDEF reproduces the legacy shader bit-exactly");
}

// Mixed rigid/affine blends must still match the reference: a rigid bone uses
// its own linear part (a rotation is its own inverse transpose).
void TestMixedRigidAffine() {
    float q0[4], q1[4];
    QuaternionFromAxis(q0, 0.0f, 1.0f, 0.0f, 0.9f);
    QuaternionFromAxis(q1, 1.0f, 0.0f, 1.0f, -1.7f);
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float t0[3] = {0.25f, -1.5f, 3.0f};
    const float t1[3] = {-2.0f, 0.75f, -0.5f};
    const BoneCase bones[2] = {
        MakeBone(identity, q0, t0),
        MakeBone(kFamilies[1].stretch, q1, t1),
    };
    affine::BoneAux aux[2] = {Precompute(bones[0]), Precompute(bones[1])};
    Check(aux[0].flags == 0 && aux[1].flags != 0, "mixed pair flags");
    affine::BoneRef refs[2] = {{bones[0].matrix, &aux[0]},
                               {bones[1].matrix, &aux[1]}};
    std::vector<gpu::Bone> record = {PackGpuBone(aux[0], bones[0].matrix),
                                     PackGpuBone(aux[1], bones[1].matrix)};
    const float weight[4] = {0.45f, 0.55f, 0.0f, 0.0f};

    affine::Skinned got{};
    affine::SkinBdef2(refs[0], refs[1], weight[0], kPosition, kNormal, got);
    double expectedPosition[3];
    double expectedNormal[3];
    ReferenceBdef(bones, weight, 2, kPosition, kNormal, expectedPosition,
                  expectedNormal);
    Check(NearVec(got.position, expectedPosition, 2e-4, 2e-4) &&
              NearVec(got.normal, expectedNormal, 2e-4, 2e-4),
          "mixed rigid/affine BDEF2 matches the reference");

    gpu::Vertex vertex{};
    int vertexBones[4] = {0, 1, 0, 1};
    FillVertex(vertex, 1, kPosition, kNormal, vertexBones, weight, nullptr,
               nullptr, nullptr);
    float gpuPosition[3];
    float gpuNormal[3];
    mirror::Run(vertex, record, gpuPosition, gpuNormal);
    Check(NearVec(gpuPosition, got.position, 2e-5, 2e-5) &&
              NearVec(gpuNormal, got.normal, 2e-5, 2e-5),
          "CPU and GPU mixed BDEF2 results agree");

    // Mixed SDEF: one rigid and one affine bone.
    float r0[3], r1[3];
    std::memcpy(r0, kRawR0, sizeof(r0));
    std::memcpy(r1, kRawR1, sizeof(r1));
    RebaseSdefArms(r0, r1, weight[0]);
    affine::Skinned sdefGot{};
    affine::SkinSdef(refs[0], refs[1], weight[0], kPosition, kNormal, kCenter,
                     r0, r1, affine::DefaultRotationOps(), sdefGot);
    double sdefPosition[3];
    double sdefNormal[3];
    ReferenceSdef(bones[0], bones[1], weight[0], kPosition, kNormal, kCenter,
                  r0, r1, sdefPosition, sdefNormal);
    Check(NearVec(sdefGot.position, sdefPosition, 2e-4, 2e-4) &&
              NearVec(sdefGot.normal, sdefNormal, 2e-4, 2e-4),
          "mixed rigid/affine SDEF matches the reference");
    const float sdefWeight[4] = {weight[0], 0.0f, 0.0f, 0.0f};
    FillVertex(vertex, 3, kPosition, kNormal, vertexBones, sdefWeight, kCenter,
               r0, r1);
    mirror::Run(vertex, record, gpuPosition, gpuNormal);
    Check(NearVec(gpuPosition, sdefGot.position, 2e-5, 2e-5) &&
              NearVec(gpuNormal, sdefGot.normal, 2e-5, 2e-5),
          "CPU and GPU mixed SDEF results agree");
}

}  // namespace

int main() {
    {
        std::uint32_t random = 0x7a4101u;
        const auto value = [&] {
            random = random * 1664525u + 1013904223u;
            return float(int(random % 200001u) - 100000) / 173.f;
        };
        for (unsigned sample = 0; sample < 10000; ++sample) {
            float matrix[16], position[3], normal[3];
            for (auto& x : matrix) x = value();
            for (auto& x : position) x = value();
            for (auto& x : normal) x = value();
            if (sample % 7 == 0) for (auto& x : matrix) x = -0.f;
            affine::BoneAux aux{};
            affine::Skinned expected{}, actual{};
            affine::TransformPosition(expected.position, position, matrix);
            affine::TransformNormal(expected.normal, normal, matrix);
            affine::SkinBdef1({matrix, &aux}, position, normal, actual);
            Check(BitEqual(&expected, &actual, sizeof(actual)),
                  "BDEF1 XYZ vectorization retains scalar output bits");
            for (auto& x : aux.normalMatrix) x = value();
            aux.flags = affine::kBoneFlagAffine;
            affine::TransformNormalBone(expected.normal, normal, matrix, aux);
            affine::Normalize3(expected.normal);
            affine::SkinBdef1({matrix, &aux}, position, normal, actual);
            Check(BitEqual(&expected, &actual, sizeof(actual)),
                  "affine BDEF1 XYZ vectorization retains scalar output bits");
        }
        std::puts("PASS BDEF1 vectorization: 20000 scalar parity cases");
    }
    TestPolarDecomposition();
    std::puts("PASS polar decomposition (A = H * R, H symmetric positive)");
    TestPolarAxisCoverage();
    std::puts("PASS polar A = H * R for X/Y/Z and arbitrary-axis rotations");
    TestPolarExtremeHierarchy();
    std::puts("PASS extreme hierarchical composites and legal fallbacks");
    TestDiagonalAnisotropicScale();
    std::puts("PASS diag(10000, 0.0001, 0.0001) affine path and H * R rebuild");
    TestSmallAndLargeScaleRegression();
    std::puts("PASS small (det 2.1e-10) and large scale affine regression");
    TestRigidPathUnchanged();
    std::puts("PASS rigid BDEF1/2/4 + PMD + SDEF bit-exact legacy path");
    TestAffineBdef();
    std::puts("PASS affine BDEF1/2/4 + PMD (uniform/non-uniform/shear), CPU vs "
              "double reference and CPU vs GPU mirror");
    TestSdefAffine();
    std::puts("PASS affine SDEF (uniform/non-uniform/shear), CPU vs double "
              "reference and CPU vs GPU mirror");
    TestSdefSameTransformDegenerate();
    std::puts("PASS SDEF same-transform degeneration to one affine map");
    TestMixedRigidAffine();
    std::puts("PASS mixed rigid/affine blends");
    TestGpuLegacyDefaults();
    std::puts("PASS default {} GPU bone record keeps the legacy shader path");
    std::puts("PASS affine skinning tests");
    return 0;
}
