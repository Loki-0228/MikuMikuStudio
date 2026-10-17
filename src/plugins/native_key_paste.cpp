#include "native_key_paste.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_morph_keys.hpp"
#include "native_display_keys.hpp"
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
namespace mikudancestudio::plugins {
bool TryPasteBoundKeys(MMDApp* app, HWND window, bool mirror) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    // Unsupported families are rejected as a whole, never partly committed.
    const auto& copied = app->ClipboardCounts();
    const unsigned families = unsigned(copied.bones != 0) + unsigned(copied.morphs != 0) + unsigned(copied.displays != 0);
    if (app->state.optflag[0] || (mirror ? !copied.bones : families != 1) ||
        app->PlaybackActive() || app->FrameStepPlayback() ||
        app->state.recordPlaybackStartPending || app->SceneMutationInProgress() ||
        bake::UiLocked() || SceneSaveInProgress() || GetCapture() ||
        app->FrameRangeDialog() || app->state.frameCopyDialog || app->RecordingWindow() ||
        app->GravitySettingDialog() || app->GroundShadowColorDialog() || app->AccessoryOrderArray() ||
        app->state.accessoryEditDialogOpen || app->state.edgeThicknessDialog ||
        history->CheckNativeEdit() != MMS_OK) return true;
    Boundary([&]() -> MmsResult {
        std::shared_ptr<HistoryChange> change;
        const auto slot = app->SelectedModelSlot();
        const bool morph = !mirror && copied.morphs != 0;
        const bool display = !mirror && copied.displays != 0;
        const auto result = morph ? PlanMorphKeyPaste(*app, slot, change)
            : display ? PlanDisplayKeyPaste(*app, slot, change)
            : PlanBoneKeyPaste(*app, slot, mirror, change);
        if (result) return result;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        const char* label = mirror ? "Mirror paste bone keys" : morph ? "Paste morph keys"
            : display ? "Paste display keys" : "Paste bone keys";
        if (auto result = history->PrepareNative(label,
            std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(*app, *history);
        if (!display) curve_host::NotifyModelRegistered(app, morph);
        SelectionReeval(app);
        if (app->MainWindow() && app->Renderer()) PanelPaint(app);
        if (window) SetFocus(window);
        return MMS_OK;
    });
    return true;
}
}
#endif
