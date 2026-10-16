#include "native_pose_axis.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include <cmath>
#include <cstring>

namespace mikudancestudio::plugins {
namespace {
MmsResult RotationTarget(MMDApp& app, int axis, NativePoseAxisSource source,
                         float target[4]) {
    const float degrees[3] = {app.BoneRotationEditDegreesX(),
        app.BoneRotationEditDegreesY(), app.BoneRotationEditDegreesZ()};
    float x = 0, y = 0, z = 0;
    if (source == NativePoseAxisSource::FloatingWindow) {
        // Preserve the separate window's float intermediates and constants.
        constexpr float low = 3.141592025756836f, high = 3.141594886779785f;
        if (axis != 0) x = degrees[0] * high / 180.0f;
        if (axis != 1) y = -degrees[1] * low / 180.0f;
        if (axis != 2) z = -degrees[2] * low / 180.0f;
    } else {
        constexpr double pi = 3.1415926535897931, piFloat = 3.1415927410125732;
        if (axis != 0) x = static_cast<float>(double(degrees[0]) * piFloat / 180.0);
        if (axis != 1) y = static_cast<float>(-double(degrees[1]) * pi / 180.0);
        if (axis != 2) z = static_cast<float>(-double(degrees[2]) * pi / 180.0);
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return MMS_E_ARGUMENT;
    auto& math = d3dx::Get();
    if (!math.Load() || !math.rotX || !math.rotY || !math.rotZ ||
        !math.multiply || !math.quatFromMatrix) return MMS_E_NOT_READY;
    if (source == NativePoseAxisSource::FloatingWindow) {
        d3dx::D3DXMATRIXF rz{}, rx{}, ry{}, rotation{};
        math.rotZ(&rz, z); math.rotX(&rx, x);
        math.multiply(&rotation, &rz, &rx);
        math.rotY(&ry, y); math.multiply(&rotation, &rotation, &ry);
        math.quatFromMatrix(target, &rotation);
    } else {
        d3dx::D3DXMATRIXF rotation{}, axisRotation{};
        math.rotZ(&rotation, z); math.rotX(&axisRotation, x);
        math.multiply(&rotation, &rotation, &axisRotation);
        math.rotY(&axisRotation, y);
        math.multiply(&rotation, &rotation, &axisRotation);
        math.quatFromMatrix(target, &rotation);
    }
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(target[i])) return MMS_E_ARGUMENT;
    return MMS_OK;
}

MmsResult ScaleTarget(MMDApp& app, unsigned char* model, int bone, int axis,
                      bone_scale::Matrix& target) {
    // Resolve view-axis D3DX calls and the scale readout before capture. The
    // readout may rebuild derived matrices, but does not author a pose.
    float values[3]{};
    if (!SelectedBoneScaleReadout(&app, values) ||
        !std::isfinite(values[axis]) || values[axis] <= 1.0e-8f)
        return MMS_E_ARGUMENT;
    const auto scale = bone_scale::Pose(model, bone);
    target = scale;
    if (std::fabs(values[axis] - 1.0f) < 1.0e-6f) return MMS_OK;
    bone_scale::ApplyTransforms(model);
    const auto current = bone_scale::Linear(mdl::Bones(model)[bone].matInit);
    bone_scale::Matrix inverseScale, inverseBasis;
    if (!bone_scale::Inverse(scale, inverseScale)) return MMS_E_ARGUMENT;
    const auto basis = bone_scale::Multiply(inverseScale, current);
    if (!bone_scale::Valid(basis) || !bone_scale::Inverse(basis, inverseBasis))
        return MMS_E_ARGUMENT;
    float direction[3]{};
    const int mode = EffectiveCoordinateState(&app);
    if (mode == 2) ViewWorldAxis(&app, axis, direction);
    else if (mode == 1) direction[axis] = 1.0f;
    else {
        float local[16]{};
        BoneLocalAxes(&app, local);
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                direction[c] += local[axis * 4 + k] * current.v[k * 3 + c];
    }
    // Same axis-conjugation and float rounding as SetSelectedBoneScaleAxis /
    // bone_scale::Scale, with SetPose deferred until the scope has captured it.
    double lengthSquared = 0;
    for (float value : direction) lengthSquared += double(value) * value;
    const float factor = 1.0f / values[axis];
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-20 ||
        !std::isfinite(factor) || factor <= 0) return MMS_E_ARGUMENT;
    bone_scale::Matrix delta;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            delta.v[i * 3 + j] += float((factor - 1) * double(direction[i]) *
                direction[j] / lengthSquared);
    delta = bone_scale::Multiply(bone_scale::Multiply(basis, delta), inverseBasis);
    target = bone_scale::Multiply(scale, delta);
    return bone_scale::Valid(target) ? MMS_OK : MMS_E_ARGUMENT;
}

MmsResult ResetAxis(MMDApp& app, SharedHistory& history, NativePoseAxisKind kind,
                   int axis, NativePoseAxisSource source, bool& changed) {
    changed = false;
    if (axis < 0 || axis > 2) return MMS_E_ARGUMENT;
    if (app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress() ||
        app.SelectedModelSlot() >= kModelSlotCount) return MMS_E_NOT_READY;
    // Scale readout preparation can rebuild derived caches. Refuse a reserved
    // journal before doing that work, as well as before authoring the pose.
    if (const auto result = history.Enter()) return result;
    history.Leave();
    auto* model = app.SelectedModel();
    auto* record = mdl::Mdl(model);
    if (!record || !record->boneTable || !record->bonePhysicsState ||
        record->selectedBone < 0 ||
        static_cast<unsigned>(record->selectedBone) >= record->boneCount)
        return MMS_E_NOT_READY;
    const int bone = record->selectedBone;
    auto& pose = record->boneTable[bone];
    float quaternion[4]{};
    bone_scale::Matrix scale;
    if (kind == NativePoseAxisKind::Position) {
        if (pose.trans[axis] == 0.0f) return MMS_OK;
    } else if (kind == NativePoseAxisKind::Rotation) {
        const float degrees[3] = {app.BoneRotationEditDegreesX(),
            app.BoneRotationEditDegreesY(), app.BoneRotationEditDegreesZ()};
        // Resetting an already zero readout must not round-trip the other
        // Euler axes and discard redo merely through conversion rounding.
        if (degrees[axis] == 0.0f) return MMS_OK;
        if (const auto result = RotationTarget(app, axis, source, quaternion)) return result;
        if (!std::memcmp(pose.rotQuat, quaternion, sizeof(quaternion))) return MMS_OK;
    } else if (kind == NativePoseAxisKind::Scale) {
        if (!bone_scale::Enabled(&app)) return MMS_E_NOT_READY;
        if (const auto result = ScaleTarget(app, model, bone, axis, scale)) return result;
        if (scale == bone_scale::Pose(model, bone)) return MMS_OK;
    } else return MMS_E_ARGUMENT;

    NativePoseScope scope;
    const char* label = kind == NativePoseAxisKind::Position ? "Reset bone position axis" :
        kind == NativePoseAxisKind::Rotation ? "Reset bone rotation axis" : "Reset bone scale axis";
    if (const auto result = scope.Begin(app, history, app.SelectedModelSlot(), label)) return result;
    if (kind == NativePoseAxisKind::Position) pose.trans[axis] = 0.0f;
    else if (kind == NativePoseAxisKind::Rotation)
        std::memcpy(pose.rotQuat, quaternion, sizeof(quaternion));
    else if (!bone_scale::SetPose(model, bone, scale)) return MMS_E_ARGUMENT;
    record->bonePhysicsState[bone] = 1;
    return scope.Commit(changed);
}
}

bool TryResetBoundPoseAxis(MMDApp* app, NativePoseAxisKind kind, int axis,
                          NativePoseAxisSource source) noexcept {
    if (!app || app->CameraMode() != 0) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    bool changed = false;
    const auto result = Boundary([&] { return ResetAxis(*app, *history, kind, axis, source, changed); });
    if (result == MMS_OK && changed) PublishBoundPoseEdit(*app, *history);
    return true;
}
}
#endif
