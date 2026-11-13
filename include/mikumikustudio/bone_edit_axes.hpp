#pragma once

#include "mikumikustudio/bone_layout.hpp"

namespace mikumikustudio {
class MMDApp;

// Retire presentation-only sector state when its scene or gesture changes.
void ResetBoneHudPreview(const MMDApp* app) noexcept;
// Capture the pre-edit baseline without requiring a paint before the first sample.
void BeginBoneHudPreview(MMDApp* app, int axis) noexcept;
void ForgetBoneHudPreview(const void* model) noexcept;

// Coordinate helpers shared by bone editing and its HUD previews. These do
// not change the active selection, pose, dirty flags, or undo state.
void BoneLocalAxesForBone(unsigned char* model, int index, float out[16]);

// Actual normalized world-space axis driven by the X/Y/Z rotation HUD cell.
// Includes the target's local basis and fixed-axis constraint. False means
// the target/axis is invalid and no rotation preview should be shown.
// Optional parentAxis receives the normalized axis used by the actual edit
// quaternion, without reconstructing it through a scaled/sheared world basis.
// Both outputs are cleared on failure and must use distinct storage.
bool BoneRotationAxisWorld(MMDApp* app, unsigned char* model, int index,
                           int axis, float out[3], float* parentAxis = nullptr);

// Interactive viewport tools only. Numeric commits and axis resets are
// explicit overrides and do not consult these permissions.
bool SelectedBoneHudAllows(MMDApp* app, bool rotation) noexcept;

// Move/IK controls orbit the active pivot; other editable rotation bones turn
// at their individual joints, including selected parent/child pairs.
inline bool BoneRotatesInPlace(mdl::BoneType type) {
    return type != mdl::BoneType::Move && type != mdl::BoneType::Ik &&
        (type <= mdl::BoneType::Effector || type == mdl::BoneType::FixedAxis);
}
}  // namespace mikumikustudio
