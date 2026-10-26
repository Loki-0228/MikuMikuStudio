#pragma once

#include <cstring>

#include "mikumikustudio/model.hpp"

namespace mikumikustudio {
namespace detail {

// Shared head of SetPhysicsMode: reselect every bone's physics pose and then
// compose the bone-morph offsets. physicsMode selects the pose source
// (ikBackup or trans/rotQuat); model.physicsMode == 2 independently enables
// the morph pass. multiply(out, a, b) supplies the caller's quaternion
// product so the legacy entry keeps its D3DX-or-fallback path while the
// private entry uses its context's frozen table.
template <typename QuaternionMultiply>
inline void PrepareBonePose(mdl::ModelRecord& model, int physicsMode,
                            QuaternionMultiply&& multiply) {
    const int boneCount = model.boneCount;
    mdl::BoneRecord* bones = model.boneTable;
    for (int i = 0; i < boneCount; ++i) {
        mdl::BoneRecord* bone = &bones[i];
        const unsigned char flag = bone->hasRigidBody;
        if (((physicsMode == 1 || (physicsMode >= 2 && bone->physicsDisabled == 0)) && flag) != 0) {
            std::memcpy(bone->physicsOffset, bone->ikBackup, 12);           // pos
            std::memcpy(bone->physicsQuat, bone->ikBackup + 3, 16);           // quat
        } else {
            std::memcpy(bone->physicsOffset, bone->trans, 12);
            std::memcpy(bone->physicsQuat, bone->rotQuat, 16);
        }
    }

    if (model.physicsMode == 2) {
        const int recCount = model.boneMorphCount;
        mdl::BoneMorphOffsetRecord* records = model.boneMorphTable;
        for (int i = 0; i < recCount; ++i) {
            const mdl::BoneMorphOffsetRecord& record = records[i];
            if (record.boneIndex < 0 || record.boneIndex >= boneCount) continue;
            mdl::BoneRecord* bone = &bones[record.boneIndex];
            bone->physicsOffset[0] += record.translation[0];
            bone->physicsOffset[1] += record.translation[1];
            bone->physicsOffset[2] += record.translation[2];
            float out[4];
            multiply(out,
                     reinterpret_cast<const float*>(bone->physicsQuat),
                     record.rotation);
            std::memcpy(bone->physicsQuat, out, 16);
        }
    }
}

}  // namespace detail
}  // namespace mikumikustudio
