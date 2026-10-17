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
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
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
struct CameraPoseChange final : HistoryChange, std::enable_shared_from_this<CameraPoseChange> {
    MMDApp& app;
    const std::uint64_t poolIdentity;
    const std::array<float, 7> before, after;
    const unsigned changed;
    std::uint64_t generation = 1;
    bool published = false, applied = false;
    static std::array<float, 7> Read(MMDApp& app) noexcept {
        return {app.CameraPositionX(), app.CameraPositionY(), app.CameraPositionZ(),
            app.CameraPitch(), app.CameraYaw(), app.CameraRoll(), app.CameraDistance()};
    }
    CameraPoseChange(MMDApp& value, std::array<float, 7> source, std::array<float, 7> target, unsigned fields)
        : app(value), poolIdentity(mdl::KeyPoolIdentity(value.CameraKeys())), before(source), after(target), changed(fields) {}
    bool SameScene() const noexcept { return poolIdentity && mdl::KeyPoolIdentity(app.CameraKeys()) == poolIdentity; }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<CameraPoseChange> source;
        const bool forward;
        const std::uint64_t generation;
        const int frame;
        const std::array<float, 7> expected;
        bool consumed = false;
        Candidate(std::shared_ptr<CameraPoseChange> value, bool direction)
            : source(std::move(value)), forward(direction), generation(source->generation),
              frame(source->app.CurrentFrame()), expected(Read(source->app)) {}
        bool IsCurrent() const noexcept override {
            const auto live = Read(source->app);
            return !consumed && source->generation == generation && forward != source->applied &&
                source->SameScene() && source->app.CurrentFrame() == frame &&
                !std::memcmp(live.data(), expected.data(), sizeof(expected));
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (unsigned field = 0; field < 7; ++field) if (source->changed & (1u << field)) {
                const auto value = forward ? source->after[field] : source->before[field];
                if (field < 3) source->app.CameraPositionAxis(field) = value;
                else if (field < 6) source->app.CameraRotation()[field - 3] = value;
                else source->app.CameraDistance() = value;
            }
            source->applied = forward; source->published = true; ++source->generation;
            consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!SameScene() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            const auto current = Read(app);
            for (float value : current) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            if (!published && std::memcmp(current.data(), before.data(), sizeof(before))) return MMS_E_STALE;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
};
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
    // RefreshRequest preserves a multi-row selection when the target row is
    // already active; only entering an unselected row makes it exclusive.
    if (kind == curve_host::Kind::Accessory) {
        const auto* object = app.AccessorySlot(slot);
        if (object && object->rowSelected) return false;
    } else if (app.GlobalTrackSelected(kind == curve_host::Kind::Camera ? GlobalTimelineTrack::Camera :
        kind == curve_host::Kind::Light ? GlobalTimelineTrack::Light : GlobalTimelineTrack::SelfShadow)) return false;
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
struct CameraNumericEdit::Impl {
    MMDApp& app;
    SharedHistory* history;
    std::uint64_t revision = 0, pool = 0;
    int frame = 0, parentModel = -1, parentBone = 0;
    float fov = 0;
    unsigned perspective = 0;
    unsigned char* parent = nullptr;
    MmsObjectRef parentRef{};
    mdl::BoneRecord* parentBones = nullptr;
    unsigned parentCount = 0;
    std::array<float, 7> before{}, values{};
    Impl(MMDApp& a, SharedHistory& h) : app(a), history(&h) {}
    bool Current() const {
        if (BoundNativePoseHistory(&app) != history || !app.CameraMode() || !Ready(app) ||
            app.CurrentFrame() != frame || mdl::KeyPoolIdentity(app.CameraKeys()) != pool ||
            app.CameraParentModel() != parentModel || app.CameraParentBone() != parentBone ||
            app.CameraFov() != fov || app.CameraPerspective() != perspective) return false;
        std::uint64_t current = 0;
        if (history->Revision(current) != MMS_OK || current != revision) return false;
        if (parentModel != -1) {
            if (app.ModelSlot(parentModel) != parent || !SceneObjectPresent(parent, parentRef.id) ||
                SceneObjectIdentity(parent).generation != parentRef.generation) return false;
            const auto* model = mdl::Mdl(parent);
            if (model->boneTable != parentBones || model->boneCount != parentCount) return false;
        }
        const auto pose = CameraPoseChange::Read(app);
        return !std::memcmp(pose.data(), before.data(), sizeof(before));
    }
};
CameraNumericEdit::CameraNumericEdit() = default;
CameraNumericEdit::~CameraNumericEdit() = default;
std::array<float, 7> CameraNumericEdit::Values() const noexcept { return impl_ ? impl_->values : std::array<float, 7>{}; }
MmsResult CameraNumericEdit::Begin(MMDApp& app, SharedHistory& history) noexcept {
    if (impl_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&app) != &history || !app.CameraMode() || !Ready(app) ||
            !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        if (const auto result = history.CheckNativeEdit()) return result;
        auto next = std::make_unique<Impl>(app, history);
        if (const auto result = history.Revision(next->revision)) return result;
        next->pool = mdl::KeyPoolIdentity(app.CameraKeys()); next->frame = app.CurrentFrame();
        next->parentModel = app.CameraParentModel(); next->parentBone = app.CameraParentBone();
        next->fov = app.CameraFov(); next->perspective = app.CameraPerspective();
        if (next->parentModel != -1) {
            if (next->parentModel < 0 || unsigned(next->parentModel) >= kModelSlotCount ||
                !(next->parent = app.ModelSlot(next->parentModel))) return MMS_E_ARGUMENT;
            const auto* parent = mdl::Mdl(next->parent);
            if (!parent->boneTable || next->parentBone < 0 || unsigned(next->parentBone) >= parent->boneCount) return MMS_E_ARGUMENT;
            next->parentRef = SceneObjectIdentity(next->parent); next->parentBones = parent->boneTable; next->parentCount = parent->boneCount;
        }
        next->before = CameraPoseChange::Read(app); next->values = next->before;
        for (float value : next->before) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        constexpr float degrees = 180.0f / 3.14159265358979323846f;
        next->values[3] = -next->before[3] * degrees;
        next->values[4] = next->before[4] * degrees; next->values[5] = next->before[5] * degrees;
        next->values[6] = -next->before[6];
        for (float value : next->values) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        impl_ = std::move(next); return MMS_OK;
    });
}
MmsResult CameraNumericEdit::Apply(const std::array<float, 7>& input) noexcept {
    if (!impl_) return MMS_E_NOT_READY;
    auto session = std::move(impl_);
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&session->app) != session->history) return MMS_E_STALE;
        if (const auto result = session->history->CheckNativeEdit()) return result;
        if (!session->Current()) return MMS_E_STALE;
        auto target = session->before; unsigned changed = 0;
        constexpr float radians = 3.14159265358979323846f / 180.0f;
        for (unsigned field = 0; field < 7; ++field) {
            if (!std::isfinite(input[field])) return MMS_E_ARGUMENT;
            if (input[field] != session->values[field]) {
                target[field] = field < 3 ? input[field] : field == 6 ? -input[field] :
                    (field == 3 ? -input[field] : input[field]) * radians;
                if (!std::isfinite(target[field])) return MMS_E_ARGUMENT;
                if (target[field] != session->before[field]) changed |= 1u << field;
            }
        }
        if (!changed) return MMS_OK;
        auto change = std::make_shared<CameraPoseChange>(session->app, session->before, target, changed);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = session->history->PrepareNative("Edit camera numeric pose", std::move(change), transaction)) return result;
        if (const auto result = session->history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(session->app, *session->history, false); return MMS_OK;
    });
}
MmsResult PlanCameraRotationReset(MMDApp& app, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    return Boundary([&]() -> MmsResult {
        if (!Ready(app) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        const auto before = CameraPoseChange::Read(app); auto after = before; unsigned changed = 0;
        for (float value : before) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        for (unsigned field = 3; field < 6; ++field) if (before[field] != 0.0f) {
            changed |= 1u << field; after[field] = 0.0f;
        }
        if (changed) out = std::make_shared<CameraPoseChange>(app, before, after, changed);
        return MMS_OK;
    });
}
bool TryResetBoundCameraRotation(MMDApp* app) noexcept {
    if (!app || !app->CameraMode()) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    Boundary([&]() -> MmsResult {
        if (!Ready(*app)) return MMS_E_NOT_READY;
        if (const auto result = history->CheckNativeEdit()) return result;
        std::shared_ptr<HistoryChange> change;
        if (const auto result = PlanCameraRotationReset(*app, change)) return result;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Reset camera rotation", std::move(change), transaction)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(*app, *history, false); return MMS_OK;
    });
    return true;
}
bool TryResetBoundCameraField(MMDApp* app, unsigned field, bool activateCameraTrack) noexcept {
    if (!app || !app->CameraMode()) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    Boundary([&]() -> MmsResult {
        if (field >= 7) return MMS_E_ARGUMENT;
        if (!Ready(*app) || !mdl::KeyPoolIdentity(app->CameraKeys())) return MMS_E_NOT_READY;
        if (const auto result = history->CheckNativeEdit()) return result;
        const auto before = CameraPoseChange::Read(*app);
        for (float value : before) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        if (before[field] == 0.0f) return MMS_OK;
        auto after = before; after[field] = 0.0f;
        auto change = std::make_shared<CameraPoseChange>(*app, before, after, 1u << field);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Reset camera pose component", std::move(change), transaction)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        const bool selectionChanged = activateCameraTrack && SelectTrack(*app, curve_host::Kind::Camera, 0);
        PublishBoundPoseEdit(*app, *history, false);
        if (selectionChanged && app->MainWindow() && app->Renderer() && app->PanelDC()) PostLanguageSweep(app);
        return MMS_OK;
    });
    return true;
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
