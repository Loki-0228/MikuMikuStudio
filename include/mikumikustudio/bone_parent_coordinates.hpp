#pragma once

#include "mikumikustudio/model.hpp"

namespace mikumikustudio {

// PMD/PMX animation stores displacement from the bind pose. The inspector
// shows the actual local position relative to the parent bone's origin.
inline float BoneParentBindOffset(const unsigned char* model, int boneIndex,
                                  int axis) {
    const auto* record = mdl::Mdl(model);
    if (!record || !record->boneTable || boneIndex < 0 ||
        boneIndex >= static_cast<int>(record->boneCount) || axis < 0 || axis > 2)
        return 0.0f;
    const auto& bone = record->boneTable[boneIndex];
    if (bone.parent < 0 || bone.parent >= static_cast<int>(record->boneCount) ||
        bone.parent == boneIndex)
        return 0.0f;
    return bone.position[axis] - record->boneTable[bone.parent].position[axis];
}

} // namespace mikumikustudio
