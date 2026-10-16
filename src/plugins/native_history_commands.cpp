#include "native_history_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"

namespace mikudancestudio::plugins {
void RefreshBoundPoseReadout(MMDApp& app) noexcept {
    // PostViewRefresh assumes a valid selected model/bone in model mode.
    // A common undo may instead affect another model, or run in camera mode.
    bool canRead = app.CameraMode() != 0;
    if (!canRead && app.SelectedModelSlot() < kModelSlotCount) {
        const auto* model = mdl::Mdl(app.SelectedModel());
        canRead = model && (model->selectedBone < 0 || (model->boneTable &&
            static_cast<std::uint32_t>(model->selectedBone) < model->boneCount));
    }
    if (canRead) Boundary([&]() -> MmsResult { PostViewRefresh(&app); return MMS_OK; });
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
    if (app->PlaybackActive() || app->FrameStepPlayback() ||
        app->state.recordPlaybackStartPending || app->SceneMutationInProgress()) return true;
    if (history->History(redo) == MMS_OK) PublishBoundPoseEdit(*app, *history, true);
    return true;
}
}
#endif
