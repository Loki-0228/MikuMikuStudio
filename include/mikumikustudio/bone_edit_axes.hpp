#pragma once

#include "mikumikustudio/bone_layout.hpp"

namespace mikumikustudio {
class MMDApp;

// Coordinate helpers shared by bone editing and its HUD previews. These do
// not change the active selection, pose, dirty flags, or undo state.
void BoneLocalAxesForBone(unsigned char* model, int index, float out[16]);

// Actual normalized world-space axis driven by the X/Y/Z rotation HUD cell.
// Includes the target's local basis and fixed-axis constraint. False means
// the target/axis is invalid and no rotation preview should be shown.
bool BoneRotationAxisWorld(MMDApp* app, unsigned char* model, int index,
                           int axis, float out[3]);

// Move/IK controls orbit the active pivot; other editable rotation bones turn
// at their individual joints, including selected parent/child pairs.
inline bool BoneRotatesInPlace(mdl::BoneType type) {
    return type != mdl::BoneType::Move && type != mdl::BoneType::Ik &&
        (type <= mdl::BoneType::Effector || type == mdl::BoneType::FixedAxis);
}
}  // namespace mikumikustudio
