#include "native_history_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_model_outline_ui.hpp"
#include "native_object_commands.hpp"
#include "native_camera_follow_ui.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio::plugins {
void RefreshBoundPoseReadout(MMDApp& app) noexcept {
    // PostViewRefresh assumes a valid selected model/bone in model mode.
    // A common undo may instead affect another model, or run in camera mode.
    bool canRead = app.CameraMode() != 0;
    if (!canRead && app.SelectedModelSlot() < kModelSlotCount) {
        const auto* model = mdl::Mdl(app.SelectedModel());
        canRead = model && (model->selectedBone < 0 || (model->boneTable &&
            static_cast<std::uint32_t>(model->selectedBone) < model->boneCount));
        // Reading a nonidentity scale normally installs its pending bone
        // transforms. History and cancellation refreshes must stay read-only.
        if (canRead && model->selectedBone >= 0)
            canRead = bone_scale::Identity(bone_scale::Pose(app.SelectedModel(), model->selectedBone)) ||
                bone_scale::TransformsApplied(app.SelectedModel());
    }
    Boundary([&]() -> MmsResult {
        bake::ScopedUiInputGate input;
        if (canRead) PostViewRefresh(&app);
        RefreshBoundCameraFollowReadout(app);
        RefreshBoundModelOutline(app);
        return MMS_OK;
    });
}
void PublishBoundPoseEdit(MMDApp& app, SharedHistory& history, bool physicsReset) noexcept {
    app.SceneModified() = 1;
    app.RecordedFrameCountByte1() = 1;
    if (physicsReset) app.PhysicsResetPending() = 1;
    const auto main = app.MainWindow();
    if (!main) return;
    EnableWindow(GetDlgItem(main, panel::kUndoButton), history.UndoCount() != 0);
    EnableWindow(GetDlgItem(main, panel::kRedoButton), history.RedoCount() != 0);
    InvalidateRect(main, nullptr, FALSE);
    RefreshBoundPoseReadout(app);
}
bool TryBoundHistoryCommand(MMDApp* app, bool redo) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    CancelBoundModelOutlineGesture(*app);
    RunBoundObjectHistory(*app, *history, redo);
    return true;
}
}
#endif
