#pragma once

#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/subrecord_layout.hpp"

namespace mikumikustudio::affine {

inline void QdefMultiply(float out[4], const float a[4], const float b[4]) {
    const float v[4] = {
        a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    std::memcpy(out, v, sizeof v);
}

inline bool QdefRotation(const float* matrix, const BoneAux& aux, float rotation[4]) {
    float rigid[16]{};
    if (aux.flags & kBoneFlagAffine) {
        float inverse[9], linear[9], r[9];
        if (!Mat3Inverse(aux.stretch, inverse)) return false;
        for (int i=0;i<3;++i) for(int j=0;j<3;++j) linear[i*3+j]=matrix[i*4+j];
        Mat3Mul(inverse,linear,r);
        for (int i=0;i<3;++i) for(int j=0;j<3;++j) rigid[i*4+j]=r[i*3+j];
        rigid[15]=1; matrix=rigid;
    }
    DefaultQuatFromMatrix(rotation,matrix);
    for(int i=0;i<3;++i) rotation[i]=-rotation[i];
    float length=0; for(int i=0;i<4;++i) length+=rotation[i]*rotation[i];
    if (!(length>1e-12f) || !std::isfinite(length)) return false;
    for(int i=0;i<4;++i) rotation[i]/=std::sqrt(length);
    return true;
}

// Dual-quaternion blend, following PE's hemisphere alignment and normalized
// rigid transform. Extend PE's diagonal scale blend to the host's polar
// stretch so nonuniform scale/shear use the same normal contract as SDEF.
template<class MatrixAt>
inline void SkinQdef(const float position[3], const float normal[3],
                     const std::int32_t indices[4], const float weights[4],
                     int boneCount, MatrixAt matrixAt, const BoneAux* aux,
                     float outPosition[3], float outNormal[3]) {
    float real[4]{}, dual[4]{}, reference[4]{}, stretch[9]{};
    bool haveReference = false;
    float total = 0;
    for (int i = 0; i < 4; ++i) {
        const int bone = indices[i];
        const float weight = weights[i];
        if (bone < 0 || bone >= boneCount || weight == 0 || !std::isfinite(weight)) continue;
        float rotation[4];
        if (!QdefRotation(matrixAt(bone),aux[bone],rotation)) continue;
        if (!haveReference) { std::memcpy(reference, rotation, sizeof reference); haveReference = true; }
        float dot = 0;
        for (int c = 0; c < 4; ++c) dot += reference[c]*rotation[c];
        const float signedWeight = dot < 0 ? -weight : weight;
        const float* source = matrixAt(bone);
        const float translation[4] = {source[12], source[13], source[14], 0};
        float d[4]; QdefMultiply(d, translation, rotation);
        for (int c = 0; c < 4; ++c) {
            real[c] += signedWeight*rotation[c];
            dual[c] += signedWeight*0.5f*d[c];
        }
        for (int c = 0; c < 9; ++c) stretch[c] += weight*aux[bone].stretch[c];
        total += weight;
    }
    float length = 0;
    for (float v : real) length += v*v;
    if (!(length > 1e-12f) || !std::isfinite(length) || std::fabs(total) < 1e-8f) {
        std::memcpy(outPosition, position, 12); std::memcpy(outNormal, normal, 12); return;
    }
    const float inverseLength = 1 / std::sqrt(length);
    for (int c = 0; c < 4; ++c) { real[c] *= inverseLength; dual[c] *= inverseLength; }
    for (float& v : stretch) v /= total;
    const float conjugate[4] = {-real[0], -real[1], -real[2], real[3]};
    float translation[4]; QdefMultiply(translation, dual, conjugate);
    float rotation[16]; DefaultMatrixFromQuaternion(rotation, real);
    float r3[9], transform[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) r3[r*3+c] = rotation[r*4+c];
    Mat3Mul(stretch, r3, transform);
    MulVec3(outPosition, position, transform);
    for (int c = 0; c < 3; ++c) outPosition[c] += 2*translation[c];
    float inverse[9];
    if (Mat3Inverse(transform, inverse)) {
        for (int c = 0; c < 3; ++c)
            outNormal[c] = normal[0]*inverse[c*3] + normal[1]*inverse[c*3+1] + normal[2]*inverse[c*3+2];
    } else MulVec3(outNormal, normal, r3);
    float norm = 0;
    for (int c = 0; c < 3; ++c) norm += outNormal[c]*outNormal[c];
    if (norm > 1e-12f) for (int c = 0; c < 3; ++c) outNormal[c] /= std::sqrt(norm);
}

} // namespace mikumikustudio::affine
