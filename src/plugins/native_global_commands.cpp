#include "native_global_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_registration.hpp"
#include "native_accessory_registration.hpp"
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace mikudancestudio::plugins {
namespace {
bool Ready(MMDApp& app) noexcept {
    return !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.SceneMutationInProgress() &&
        !bake::UiLocked() && !SceneSaveInProgress() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
bool ReadLegacyNumber(HWND window, int id, double& value) noexcept {
    const auto edit = GetDlgItem(window, id);
    if (!edit) return false;
    // The original registration path reads at most seven ANSI characters.
    // Reject non-finite and non-numeric input before any authoring is changed.
    char text[8]{}; GetWindowTextA(edit, text, sizeof(text));
    errno = 0; char* end = nullptr; value = std::strtod(text, &end);
    return end != text && errno != ERANGE && std::isfinite(value);
}
bool SelectTrack(MMDApp& app, curve_host::Kind kind, unsigned slot) noexcept {
    bool changed = false;
    for (unsigned index = 0; index < 4; ++index) {
        const auto selected = kind != curve_host::Kind::Accessory && index ==
            (kind == curve_host::Kind::Camera ? 0u : kind == curve_host::Kind::Light ? 1u : 2u);
        changed = changed || app.state.globalTrackSelected[index] != std::uint8_t(selected);
    }
    if (kind == curve_host::Kind::Accessory) app.ClearGlobalTimelineTrackSelection();
    else app.SelectGlobalTimelineTrack(kind == curve_host::Kind::Camera ? GlobalTimelineTrack::Camera :
        kind == curve_host::Kind::Light ? GlobalTimelineTrack::Light : GlobalTimelineTrack::SelfShadow);
    for (unsigned index = 0; index < kModelSlotCount; ++index)
        if (auto* object = app.AccessorySlot(index)) {
            const auto selected = kind == curve_host::Kind::Accessory && index == slot;
            changed = changed || object->rowSelected != std::uint8_t(selected);
            object->rowSelected = selected;
        }
    return changed;
}
template<class Plan>
bool Register(MMDApp* app, curve_host::Kind kind, Plan plan) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!Ready(*app) || history->Enter() != MMS_OK) return true;
    history->Leave();
    Boundary([&]() -> MmsResult {
        const auto slot = unsigned(app->SelectedAccessorySlot());
        std::shared_ptr<HistoryChange> change;
        if (auto result = plan(*app, change)) return result;
        if (!change) return MMS_OK;
        const char* label = kind == curve_host::Kind::Camera ? "Register camera key" :
            kind == curve_host::Kind::Light ? "Register light key" :
            kind == curve_host::Kind::Shadow ? "Register shadow key" : "Register accessory key";
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto result = history->PrepareNative(label, std::move(change), transaction)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        // Track-row selection is a view choice, not authoring restored by undo.
        // Failure/no-op has already returned, so the original selection survives.
        const bool selectionChanged = SelectTrack(*app, kind, slot);
        if (kind == curve_host::Kind::Accessory) app->state.enterKeyState = 3;
        PublishBoundPoseEdit(*app, *history, false);
        if (app->MainWindow()) {
            // The model-mode sweep also normalizes bone selection. A global
            // registration must not change that unrelated edit target.
            if (selectionChanged && app->CameraMode() && app->Renderer() && app->PanelDC()) PostLanguageSweep(app);
            RefreshBoundGlobalReadout(*app);
            if (app->CurveDC()) SelectionReeval(app);
        }
        curve_host::NotifyRegistered(app, kind, kind == curve_host::Kind::Accessory ? int(slot) : 0);
        return MMS_OK;
    });
    return true;
}
}
bool TryRegisterBoundCamera(MMDApp* app, HWND window) noexcept {
    return Register(app, curve_host::Kind::Camera, [window](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        const bool reset = SendMessageW(GetDlgItem(window, panel::kPhysicsFrameCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
        return PlanCameraRegistration(host, host.CurrentFrame(), reset, out);
    });
}
bool TryRegisterBoundLight(MMDApp* app, HWND) noexcept {
    return Register(app, curve_host::Kind::Light, [](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        return PlanLightRegistration(host, host.CurrentFrame(), out);
    });
}
bool TryRegisterBoundShadow(MMDApp* app, HWND window) noexcept {
    return Register(app, curve_host::Kind::Shadow, [window](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        double raw = 0;
        if (!ReadLegacyNumber(window, panel::kSelfShadowRangeEdit, raw)) return MMS_E_ARGUMENT;
        const float value = static_cast<float>(raw);
        if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        const int position = static_cast<int>((std::clamp)(value, 0.f, 9999.f));
        const float distance = static_cast<float>((10000.0 - position) / 100000.0);
        return PlanShadowRegistration(host, host.CurrentFrame(), distance, out);
    });
}
bool TryRegisterBoundAccessory(MMDApp* app, HWND window) noexcept {
    return Register(app, curve_host::Kind::Accessory, [window](MMDApp& host, std::shared_ptr<HistoryChange>& out) {
        AccessoryRegistrationInput input;
        for (unsigned index = 0; index < 7; ++index) {
            double value = 0;
            if (!ReadLegacyNumber(window, 478 + int(index), value)) return MMS_E_ARGUMENT;
            if (index >= 3 && index < 6) value = value / 180.0 * 3.141592025756836;
            const float converted = static_cast<float>(value);
            if (!std::isfinite(converted)) return MMS_E_ARGUMENT;
            if (index < 3) input.position[index] = converted;
            else if (index < 6) input.rotation[index - 3] = converted;
            else input.scale = converted;
        }
        return PlanAccessoryRegistration(host, unsigned(host.SelectedAccessorySlot()), input, out);
    });
}
void RefreshBoundGlobalReadout(MMDApp& app) noexcept {
    if (!app.MainWindow()) return;
    Boundary([&]() -> MmsResult {
        SyncCameraFovControls(&app);
        const double shadowPosition = 10000.0 - double(app.ShadowDistance()) * 100000.0;
        if (std::isfinite(shadowPosition) && shadowPosition >= (std::numeric_limits<int>::min)() &&
            shadowPosition <= (std::numeric_limits<int>::max)()) SyncSelfShadowControls(&app);
        // Synchronize the light's UI from its current value, never from keys.
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double color = double(app.LightColor()[axis]) * 256.0;
            const double direction = app.LightDirection()[axis];
            if (!std::isfinite(color) || !std::isfinite(direction)) continue;
            const auto bounded = [](double value) {
                return static_cast<int>((std::clamp)(value, double((std::numeric_limits<int>::min)()),
                    double((std::numeric_limits<int>::max)())));
            };
            char text[64]{};
            std::snprintf(text, sizeof(text), "%3d", bounded(color));
            SetDlgItemTextA(app.MainWindow(), 461 + int(axis), text);
            std::snprintf(text, sizeof(text), "%+.1f", direction);
            SetDlgItemTextA(app.MainWindow(), 464 + int(axis), text);
            SendDlgItemMessageW(app.MainWindow(), 455 + int(axis), TBM_SETPOS, TRUE, bounded(color));
            SendDlgItemMessageW(app.MainWindow(), 458 + int(axis), TBM_SETPOS, TRUE, bounded(direction * 100.0));
        }
        if (app.SelectedAccessorySlot() < kModelSlotCount) {
            const auto* object = app.AccessorySlot(app.SelectedAccessorySlot());
            if (object && (object->parentModel < 0 ||
                (unsigned(object->parentModel) < kModelSlotCount && app.ModelSlot(object->parentModel))))
                SyncAccessoryEditPanel(&app);
        }
        return MMS_OK;
    });
}
}
#endif
