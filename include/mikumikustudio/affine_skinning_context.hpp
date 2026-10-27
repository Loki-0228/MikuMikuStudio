#pragma once
#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/bone_transform_context.hpp"

namespace mikumikustudio::affine {
// Borrowed immutable math lease. Keep the owning context alive through every
// call; this adapter neither loads a DLL nor acquires global math state.
struct ContextRotationOps {
    const BoneTransformContext& context;
    void quatFromMatrix(float out[4], const float matrix[16]) const noexcept {
        context.QuaternionFromMatrix(out, matrix);
    }
    void matrixFromQuaternion(float out[16], const float quaternion[4]) const noexcept {
        context.MatrixFromQuaternion(out, quaternion);
    }
};

inline void SkinSdefPrivate(const BoneRef& bone0, const BoneRef& bone1, float weight0,
                            const float position[3], const float normal[3],
                            const float center[3], const float r0Offset[3],
                            const float r1Offset[3], const BoneTransformContext& context,
                            Skinned& out) {
    const ContextRotationOps ops{context};
    SkinSdefWithOps(bone0, bone1, weight0, position, normal, center,
                    r0Offset, r1Offset, ops, out);
}
}
