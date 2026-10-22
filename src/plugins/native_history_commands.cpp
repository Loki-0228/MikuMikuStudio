#include "native_history_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_model_outline_ui.hpp"
#include "native_object_commands.hpp"
#include "native_camera_follow_ui.hpp"
#include "history_runtime.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/update_coordinator.hpp"

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
void PublishBoundHistoryEffects(MMDApp& app, SharedHistory& history, HistoryEffects effects, bool physicsReset) noexcept {
    // The history publication already committed above this function. Scalars are
    // applied first; every later UI or drawing failure may only queue a retry at
    // the normal message boundary and must never undo or fail that publication.
    if (HasHistoryEffect(effects, HistoryEffects::ProjectData)) {
        app.SceneModified() = 1;
        app.RecordedFrameCountByte1() = 1;
    }
    if (physicsReset) app.PhysicsResetPending() = 1;
    const auto main = app.MainWindow();
    if (!main) return;
    bool refreshFailed = false;
    try {
        const auto undo = GetDlgItem(main, panel::kUndoButton);
        const auto redo = GetDlgItem(main, panel::kRedoButton);
        if (undo && redo) {
            EnableWindow(undo, history.UndoCount() != 0);
            EnableWindow(redo, history.RedoCount() != 0);
        } else refreshFailed = true;
        // Only a scene/readout publication may repaint the whole main window and
        // read pose-derived control text. None or a renderer-only mask must not
        // force that full repaint or read the author pose.
        if (HasHistoryEffect(effects, HistoryEffects::SceneReadout)) {
            if (!InvalidateRect(main, nullptr, FALSE)) refreshFailed = true;
            RefreshBoundPoseReadout(app);
        }
        // Drawing request only: never Evaluate, seek the playhead or allocate a
        // second queue. The existing coordinator coalesces this wake.
        if (HasHistoryEffect(effects, HistoryEffects::Viewport)) updates::RequestViewport(&app, false);
    } catch (...) { refreshFailed = true; }
    if (refreshFailed) RequestBoundHistoryUiRefresh(&app, effects);
}
void PublishBoundPoseEdit(MMDApp& app, SharedHistory& history, bool physicsReset) noexcept {
    // Existing native commands keep the original fully conservative behavior.
    PublishBoundHistoryEffects(app, history, HistoryEffects::All, physicsReset);
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
