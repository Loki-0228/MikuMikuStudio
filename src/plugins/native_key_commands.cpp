#include "native_key_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_model_keys.hpp"
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace mikudancestudio::plugins {
namespace {
bool ModelReady(const MMDApp& app) noexcept {
    return !app.state.optflag[0] && app.SelectedModelSlot() < kModelSlotCount &&
        app.ModelSlot(app.SelectedModelSlot()) && !app.PlaybackActive() &&
        !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.SceneMutationInProgress();
}
bool ReadNumber(HWND dialog, int id, double& value) noexcept {
    const auto edit = GetDlgItem(dialog, id);
    char text[64]{};
    if (!edit || GetWindowTextLengthA(edit) >= sizeof(text) ||
        GetWindowTextA(edit, text, sizeof(text)) <= 0) return false;
    errno = 0; char* end = nullptr;
    value = std::strtod(text, &end);
    if (end == text || errno == ERANGE || !std::isfinite(value)) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return !*end;
}
MmsResult CommitKeyChange(MMDApp& app, SharedHistory& history,
    std::shared_ptr<HistoryChange> change, const char* label, bool morphs = false) {
    if (!change) return MMS_OK;
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    if (auto result = history.PrepareNative(label, std::move(change), transaction)) return result;
    if (auto result = history.CommitNative(std::move(transaction))) return result;
    PublishBoundPoseEdit(app, history, true);
    curve_host::NotifyModelRegistered(&app, false);
    if (morphs) curve_host::NotifyModelRegistered(&app, true);
    return MMS_OK;
}
}
bool TryDeleteBoundKeys(MMDApp* app, bool selectedAtCurrentFrame) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!ModelReady(*app) || history->Enter() != MMS_OK) return true;
    history->Leave();
    Boundary([&]() -> MmsResult {
        const bool automatic = IsDlgButtonChecked(app->MainWindow(), panel::kPhysicsFrameCheckbox) == BST_CHECKED;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanModelKeyDeletion(*app, app->SelectedModelSlot(), automatic,
                                              change, selectedAtCurrentFrame)) return result;
        return CommitKeyChange(*app, *history, std::move(change), "Delete model keys", true);
    });
    return true;
}
bool TryScaleBoundBoneRange(MMDApp* app, HWND dialog) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!ModelReady(*app) || history->Enter() != MMS_OK) return true;
    history->Leave();
    Boundary([&]() -> MmsResult {
        // A mixed-family operation must become one atomic history node. Until
        // its participants exist, reject the whole request before any marks.
        if (IsDlgButtonChecked(dialog, panel::kScaleBoneCheckbox) != BST_CHECKED ||
            IsDlgButtonChecked(dialog, panel::kScaleMorphCheckbox) == BST_CHECKED ||
            IsDlgButtonChecked(dialog, panel::kScaleDispIkCheckbox) == BST_CHECKED) return MMS_E_NOT_READY;
        BoneRangeScaleOptions options; double start, end;
        if (!ReadNumber(dialog, panel::kScaleFromEdit, start) ||
            !ReadNumber(dialog, panel::kScaleToEdit, end) ||
            !ReadNumber(dialog, panel::kScaleRateEdit, options.scale) ||
            start < 0 || end <= start || end > frame_limits::maximum ||
            std::floor(start) != start || std::floor(end) != end) return MMS_E_ARGUMENT;
        options.start = static_cast<std::uint32_t>(start);
        options.end = static_cast<std::uint32_t>(end);
        options.automaticInterpolation = IsDlgButtonChecked(app->MainWindow(), panel::kPhysicsFrameCheckbox) == BST_CHECKED;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanBoneRangeScale(*app, app->SelectedModelSlot(), options, change)) return result;
        const bool changed = bool(change);
        if (auto result = CommitKeyChange(*app, *history, std::move(change), "Scale bone key range")) return result;
        if (changed) app->FrameRangeStartFrame() = static_cast<std::int32_t>(options.start);
        return MMS_OK;
    });
    return true;
}
}
#endif
