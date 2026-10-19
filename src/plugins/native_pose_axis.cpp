#include "native_pose_axis.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_global_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_parent_coordinates.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mikumikustudio::plugins {
namespace {
bool PoseMenuReady(MMDApp& app) noexcept {
    return !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.SceneMutationInProgress() &&
        !bake::UiLocked() && !SceneSaveInProgress() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog &&
        !app.CameraMode() && app.SelectedModelSlot() < kModelSlotCount;
}
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

struct BoneNumericEdit::Impl {
    MMDApp& app;
    SharedHistory* history;
    std::uint64_t revision = 0;
    MmsObjectRef object{};
    unsigned slot = 0, count = 0;
    int bone = -1, parent = -1, frame = 0;
    unsigned char* model = nullptr;
    mdl::BoneRecord* bones = nullptr;
    unsigned char* flags = nullptr;
    unsigned char dirty = 0;
    std::array<float, 3> position{}, offset{};
    std::array<float, 4> rotation{};
    std::array<float, 6> values{};
    bone_scale::Matrix scale;
    Impl(MMDApp& a, SharedHistory& h) : app(a), history(&h) {}
    bool Current() const {
        // Test the binding and stable membership before following any saved
        // native pointer. A modal loop can replace the entire scene/runtime.
        if (BoundNativePoseHistory(&app) != history || !PoseMenuReady(app) ||
            app.SelectedModelSlot() != slot || app.CurrentFrame() != frame ||
            app.ModelSlot(slot) != model || !SceneObjectPresent(model, object.id)) return false;
        const auto live = SceneObjectIdentity(model);
        if (live.generation != object.generation) return false;
        std::uint64_t now = 0;
        if (history->Revision(now) != MMS_OK || now != revision) return false;
        const auto* record = mdl::Mdl(model);
        if (record->boneTable != bones || record->bonePhysicsState != flags || record->boneCount != count ||
            record->selectedBone != bone || bones[bone].parent != parent || flags[bone] != dirty ||
            std::memcmp(bones[bone].trans, position.data(), sizeof(position)) ||
            std::memcmp(bones[bone].rotQuat, rotation.data(), sizeof(rotation)) ||
            bone_scale::Pose(model, bone) != scale) return false;
        for (int axis = 0; axis < 3; ++axis)
            if (BoneParentBindOffset(model, bone, axis) != offset[axis]) return false;
        return true;
    }
};
BoneNumericEdit::BoneNumericEdit() = default;
BoneNumericEdit::~BoneNumericEdit() = default;
std::array<float, 6> BoneNumericEdit::Values() const noexcept { return impl_ ? impl_->values : std::array<float, 6>{}; }
MmsResult BoneNumericEdit::Begin(MMDApp& app, SharedHistory& history) noexcept {
    if (impl_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&app) != &history || !PoseMenuReady(app)) return MMS_E_NOT_READY;
        if (const auto result = history.CheckNativeEdit()) return result;
        auto* record = mdl::Mdl(app.SelectedModel());
        if (!record || !record->boneTable || !record->bonePhysicsState || record->selectedBone < 0 ||
            static_cast<unsigned>(record->selectedBone) >= record->boneCount) return MMS_E_NOT_READY;
        auto& math = d3dx::Get();
        if (!math.Load() || !math.matrixRotationQuaternion || !math.rotX || !math.rotY ||
            !math.rotZ || !math.multiply || !math.quatFromMatrix) return MMS_E_NOT_READY;
        auto next = std::make_unique<Impl>(app, history);
        if (const auto result = history.Revision(next->revision)) return result;
        next->slot = app.SelectedModelSlot(); next->frame = app.CurrentFrame();
        next->model = app.SelectedModel(); next->object = SceneObjectIdentity(next->model);
        next->bone = record->selectedBone; next->count = record->boneCount;
        next->bones = record->boneTable; next->flags = record->bonePhysicsState;
        const auto& bone = record->boneTable[next->bone]; next->parent = bone.parent;
        next->dirty = next->flags[next->bone]; next->scale = bone_scale::Pose(next->model, next->bone);
        std::memcpy(next->position.data(), bone.trans, sizeof(next->position));
        std::memcpy(next->rotation.data(), bone.rotQuat, sizeof(next->rotation));
        for (float value : next->position) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        for (float value : next->rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        if (!bone_scale::Valid(next->scale)) return MMS_E_ARGUMENT;
        d3dx::D3DXMATRIXF matrix{}; math.matrixRotationQuaternion(&matrix, bone.rotQuat);
        for (const auto& row : matrix.m) for (float value : row) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        constexpr float toDegrees = 180.0f / 3.14159265358979323846f;
        const float x = std::asin(std::clamp(-matrix.m[2][1], -1.0f, 1.0f));
        float y = 0, z = 0;
        if (std::fabs(std::cos(x)) > 1e-6f) {
            y = std::atan2(matrix.m[2][0], matrix.m[2][2]);
            z = std::atan2(matrix.m[0][1], matrix.m[1][1]);
        } else y = std::atan2(-matrix.m[0][2], matrix.m[0][0]);
        for (int axis = 0; axis < 3; ++axis) {
            next->offset[axis] = BoneParentBindOffset(next->model, next->bone, axis);
            next->values[axis] = next->position[axis] + next->offset[axis];
        }
        next->values[3] = x * toDegrees; next->values[4] = -y * toDegrees; next->values[5] = -z * toDegrees;
        for (float value : next->values) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        impl_ = std::move(next); return MMS_OK;
    });
}
MmsResult BoneNumericEdit::Apply(const std::array<float, 6>& values) noexcept {
    if (!impl_) return MMS_E_NOT_READY;
    auto session = std::move(impl_);
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&session->app) != session->history) return MMS_E_STALE;
        if (const auto result = session->history->CheckNativeEdit()) return result;
        if (!session->Current()) return MMS_E_STALE;
        for (float value : values) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        auto position = session->position;
        auto rotation = session->rotation;
        // Preserve untouched components exactly, including the original
        // quaternion when the dialog's Euler values were not edited.
        for (int axis = 0; axis < 3; ++axis) if (values[axis] != session->values[axis])
            position[axis] = values[axis] - session->offset[axis];
        if (values[3] != session->values[3] || values[4] != session->values[4] || values[5] != session->values[5]) {
            constexpr float toRadians = 3.14159265358979323846f / 180.0f;
            auto& math = d3dx::Get(); d3dx::D3DXMATRIXF result{}, temporary{};
            math.rotZ(&result, -values[5] * toRadians); math.rotX(&temporary, values[3] * toRadians);
            math.multiply(&result, &result, &temporary); math.rotY(&temporary, -values[4] * toRadians);
            math.multiply(&result, &result, &temporary); math.quatFromMatrix(rotation.data(), &result);
        }
        for (float value : position) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        for (float value : rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        if (position == session->position && rotation == session->rotation) return MMS_OK;
        NativePoseScope scope;
        if (const auto result = scope.Begin(session->app, *session->history, session->slot, "Edit bone numeric pose")) return result;
        std::memcpy(session->bones[session->bone].trans, position.data(), sizeof(position));
        std::memcpy(session->bones[session->bone].rotQuat, rotation.data(), sizeof(rotation));
        session->flags[session->bone] = 1;
        bool changed = false;
        if (const auto result = scope.Commit(changed)) return result;
        if (changed) PublishBoundPoseEdit(session->app, *session->history);
        return MMS_OK;
    });
}
bool TryResetBoundPoseAxis(MMDApp* app, NativePoseAxisKind kind, int axis,
                          NativePoseAxisSource source) noexcept {
    if (!app) return false;
    if (app->CameraMode()) {
        if (kind == NativePoseAxisKind::Position || kind == NativePoseAxisKind::Rotation)
            return TryResetBoundCameraField(app, axis >= 0 && axis < 3 ?
                unsigned(axis) + (kind == NativePoseAxisKind::Rotation ? 3u : 0u) : 7u);
        return false;
    }
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    bool changed = false;
    const auto result = Boundary([&] { return ResetAxis(*app, *history, kind, axis, source, changed); });
    if (result == MMS_OK && changed) PublishBoundPoseEdit(*app, *history);
    return true;
}
bool TryResetBoundBoneRotation(MMDApp* app) noexcept {
    if (!app || app->CameraMode() != 0) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    Boundary([&]() -> MmsResult {
        if (!PoseMenuReady(*app)) return MMS_E_NOT_READY;
        if (const auto result = history->Enter()) return result;
        history->Leave();
        auto* model = mdl::Mdl(app->SelectedModel());
        if (!model || !model->boneTable || !model->bonePhysicsState || model->selectedBone < 0 ||
            static_cast<unsigned>(model->selectedBone) >= model->boneCount) return MMS_E_NOT_READY;
        const auto bone = model->selectedBone;
        constexpr float identity[4]{0, 0, 0, 1};
        // An identity pose is a no-op even when its unregistered flag is clear.
        // Do not manufacture a history step just by changing that flag.
        if (!std::memcmp(model->boneTable[bone].rotQuat, identity, sizeof(identity))) return MMS_OK;
        NativePoseScope scope;
        if (const auto result = scope.Begin(*app, *history, app->SelectedModelSlot(), "Reset bone rotation")) return result;
        std::memcpy(model->boneTable[bone].rotQuat, identity, sizeof(identity));
        model->bonePhysicsState[bone] = 1;
        bool changed = false;
        if (const auto result = scope.Commit(changed)) return result;
        if (changed) PublishBoundPoseEdit(*app, *history);
        return MMS_OK;
    });
    return true;
}
}
#endif
