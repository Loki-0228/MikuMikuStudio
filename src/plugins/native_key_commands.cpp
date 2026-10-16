#include "native_key_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_model_keys.hpp"
#include "native_morph_keys.hpp"
#include "native_display_keys.hpp"
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
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
    std::shared_ptr<HistoryChange> change, const char* label, bool morphs = false, bool physicsReset = true) {
    if (!change) return MMS_OK;
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    if (auto result = history.PrepareNative(label, std::move(change), transaction)) return result;
    if (auto result = history.CommitNative(std::move(transaction))) return result;
    PublishBoundPoseEdit(app, history, physicsReset);
    curve_host::NotifyModelRegistered(&app, false);
    if (morphs) curve_host::NotifyModelRegistered(&app, true);
    return MMS_OK;
}
bool TryRegister(MMDApp* app, bool morph, unsigned lane) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!ModelReady(*app) || (morph && lane >= 4) || history->Enter() != MMS_OK) return true;
    history->Leave();
    Boundary([&]() -> MmsResult {
        const auto slot = app->SelectedModelSlot();
        std::shared_ptr<HistoryChange> change;
        const auto result = morph
            ? PlanMorphRegistration(*app, slot, mdl::Mdl(app->ModelSlot(slot))->selectedMorphs[lane],
                                    app->CurrentFrame(), change)
            : PlanDisplayRegistration(*app, slot, app->CurrentFrame(), change);
        if (result != MMS_OK || !change) return result;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto r = history->PrepareNative(morph ? "Register morph key" : "Register display key",
                                           std::move(change), transaction)) return r;
        if (auto r = history->CommitNative(std::move(transaction))) return r;
        // Registration records the current authored state without seeking or
        // resetting physics. Only the affected curve editor needs notification.
        PublishBoundPoseEdit(*app, *history, false);
        if (morph) curve_host::NotifyModelRegistered(app, true);
        return MMS_OK;
    });
    return true;
}
}
bool TryRegisterBoundMorph(MMDApp* app, unsigned lane) noexcept { return TryRegister(app, true, lane); }
bool TryRegisterBoundDisplay(MMDApp* app) noexcept { return TryRegister(app, false, 0); }
bool TryApplyBoundBoneInterpolation(MMDApp* app, HWND window, bool reset) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!ModelReady(*app) || history->Enter() != MMS_OK) return true;
    history->Leave();
    Boundary([&]() -> MmsResult {
        const auto combo = GetDlgItem(window, panel::kInterpCurveCombo);
        if (!combo) return MMS_E_ARGUMENT;
        const auto channel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (channel < 0 || channel > bone_scale::kInterpBoneAll) return MMS_E_ARGUMENT;
        BoneInterpolationOptions options;
        options.channels = channel == bone_scale::kInterpBoneAll ? 31u : std::uint8_t(1u << channel);
        for (unsigned lane = 0; lane < options.values.size(); ++lane) {
            if (!(options.channels & (1u << lane))) continue;
            options.values[lane] = reset ? bone_scale::LinearCurve() : bone_scale::Curve{
                app->state.lightA[lane], app->state.lightB[lane], app->state.lightC[lane], app->state.lightD[lane]};
        }
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanBoneInterpolationEdit(*app, app->SelectedModelSlot(), options, change)) return result;
        if (!change) return MMS_OK;
        if (auto result = CommitKeyChange(*app, *history, std::move(change),
            reset ? "Reset bone interpolation" : "Apply bone interpolation", false, false)) return result;
        // Win32 messages and optional editor callbacks run after publication.
        SelectionReeval(app);
        SetFocus(window);
        return MMS_OK;
    });
    return true;
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
        ModelRangeScaleOptions options; double start, end;
        options.bones = IsDlgButtonChecked(dialog, panel::kScaleBoneCheckbox) == BST_CHECKED;
        options.morphs = IsDlgButtonChecked(dialog, panel::kScaleMorphCheckbox) == BST_CHECKED;
        options.display = IsDlgButtonChecked(dialog, panel::kScaleDispIkCheckbox) == BST_CHECKED;
        if (!ReadNumber(dialog, panel::kScaleFromEdit, start) ||
            !ReadNumber(dialog, panel::kScaleToEdit, end) ||
            !ReadNumber(dialog, panel::kScaleRateEdit, options.scale) ||
            start < 0 || end <= start || end > frame_limits::maximum ||
            std::floor(start) != start || std::floor(end) != end) return MMS_E_ARGUMENT;
        options.start = static_cast<std::uint32_t>(start);
        options.end = static_cast<std::uint32_t>(end);
        options.automaticInterpolation = IsDlgButtonChecked(app->MainWindow(), panel::kPhysicsFrameCheckbox) == BST_CHECKED;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanModelRangeScale(*app, app->SelectedModelSlot(), options, change)) return result;
        const bool changed = bool(change);
        if (auto result = CommitKeyChange(*app, *history, std::move(change), "Scale model key range", options.morphs)) return result;
        if (changed) app->FrameRangeStartFrame() = static_cast<std::int32_t>(options.start);
        return MMS_OK;
    });
    return true;
}
}
#endif
