#include "native_camera_hud_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/update_coordinator.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mikudancestudio {
// Native cursor/interaction cleanup, implemented beside the HUD press path.
void FinishCameraHudInteraction(MMDApp*) noexcept;
}
namespace mikudancestudio::plugins {
namespace {
using Values = std::array<float, 6>;
Values Read(MMDApp& app) noexcept {
    return {app.CameraPositionX(), app.CameraPositionY(), app.CameraPositionZ(),
        app.CameraPitch(), app.CameraYaw(), app.CameraRoll()};
}
bool Equal(const Values& a, const Values& b) noexcept { return !std::memcmp(a.data(), b.data(), sizeof(a)); }
bool Finite(const Values& values) noexcept { for (float value : values) if (!std::isfinite(value)) return false; return true; }
unsigned Mask(const Values& a, const Values& b) noexcept {
    unsigned result = 0;
    for (unsigned i = 0; i < a.size(); ++i) if (std::memcmp(&a[i], &b[i], sizeof(float))) result |= 1u << i;
    return result;
}
void Write(MMDApp& app, const Values& values, unsigned mask = 63) noexcept {
    for (unsigned i = 0; i < values.size(); ++i) if (mask & (1u << i)) {
        if (i < 3) app.CameraPositionAxis(i) = values[i];
        else app.CameraRotation()[i - 3] = values[i];
    }
}
bool Ready(MMDApp& app, bool replay = false) noexcept {
    const bool authorized = replay && NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        (!app.SceneMutationInProgress() || authorized) && (!bake::UiLocked() || authorized) &&
        !SceneSaveInProgress() && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.RecordingWindow() && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
struct Change final : HistoryChange, std::enable_shared_from_this<Change> {
    MMDApp& app;
    const std::uint64_t pool, binding;
    const int frame, parentModel, parentBone, coordinate;
    const Values before;
    Values after;
    std::shared_ptr<RetainedSceneObject> parent;
    unsigned char* parentPointer = nullptr;
    mdl::BoneRecord* parentBones = nullptr;
    std::uint32_t parentCount = 0;
    HWND owner = nullptr;
    unsigned mask = 0;
    std::uint64_t generation = 1;
    bool preview = true, applied = false;
    explicit Change(MMDApp& value) : app(value), pool(mdl::KeyPoolIdentity(value.CameraKeys())),
        binding(BoundNativePoseGeneration(&value)), frame(value.CurrentFrame()),
        parentModel(value.CameraParentModel()), parentBone(value.CameraParentBone()),
        coordinate(EffectiveCoordinateState(&value)), before(Read(value)), after(before) {
        if (parentModel == -1) return;
        if (parentModel < 0 || unsigned(parentModel) >= kModelSlotCount || !app.ModelSlot(parentModel))
            throw std::invalid_argument("camera HUD parent model");
        parentPointer = app.ModelSlot(parentModel);
        const auto* record = mdl::Mdl(parentPointer);
        parentBones = record->boneTable; parentCount = record->boneCount;
        if (!parentBones || parentBone < 0 || unsigned(parentBone) >= parentCount)
            throw std::invalid_argument("camera HUD parent bone");
        parent = RetainSceneObject(app, MMS_OBJECT_MODEL, unsigned(parentModel));
    }
    bool SameScene() const noexcept {
        if (!pool || mdl::KeyPoolIdentity(app.CameraKeys()) != pool) return false;
        if (!parent) return true;
        return parent->Present() && app.ModelSlot(parentModel) == parentPointer &&
            SceneObjectPresent(parentPointer, parent->Id()) &&
            mdl::Mdl(parentPointer)->boneTable == parentBones && mdl::Mdl(parentPointer)->boneCount == parentCount;
    }
    bool AttachmentCurrent() const noexcept {
        return app.CameraParentModel() == parentModel && app.CameraParentBone() == parentBone;
    }
    bool CaptureCurrent() const noexcept {
        const auto capture = GetCapture(); return !capture || (preview && owner && capture == owner);
    }
    bool SourceCurrent() const noexcept {
        return SameScene() && AttachmentCurrent() && app.CurrentFrame() == frame && app.CameraMode() != 0 &&
            EffectiveCoordinateState(&app) == coordinate && BoundNativePoseGeneration(&app) == binding;
    }
    bool OwnedPreviewDomain() const noexcept {
        // Mode and coordinate selection only control further input. They do
        // not transfer ownership of an otherwise unchanged camera preview.
        // A frame change can replace the evaluated camera pose even when its
        // numbers happen to equal our last sample; never restore into that
        // new evaluation domain, or across a replaced pool/parent/binding.
        return SameScene() && AttachmentCurrent() && app.CurrentFrame() == frame &&
            BoundNativePoseGeneration(&app) == binding;
    }
    bool InitialCurrent() const noexcept {
        return generation != 1 || (Ready(app) && CaptureCurrent() && SourceCurrent() && Equal(Read(app), after));
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<Change> source;
        const bool forward, initialPreview;
        const std::uint64_t generation, binding;
        const int frame, parentModel, parentBone;
        const Values expected;
        bool consumed = false;
        Candidate(std::shared_ptr<Change> value, bool direction) : source(std::move(value)), forward(direction),
            initialPreview(source->preview), generation(source->generation), binding(BoundNativePoseGeneration(&source->app)),
            frame(source->app.CurrentFrame()), parentModel(source->app.CameraParentModel()),
            parentBone(source->app.CameraParentBone()), expected(Read(source->app)) {}
        bool IsCurrent() const noexcept override {
            return !consumed && generation == source->generation && forward != source->applied &&
                source->SameScene() && source->AttachmentCurrent() && source->InitialCurrent() &&
                Ready(source->app, generation != 1) && source->CaptureCurrent() &&
                binding == BoundNativePoseGeneration(&source->app) && frame == source->app.CurrentFrame() &&
                parentModel == source->app.CameraParentModel() && parentBone == source->app.CameraParentBone() &&
                Equal(Read(source->app), initialPreview ? source->after : expected);
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            Write(source->app, forward ? source->after : source->before, source->mask);
            source->preview = false; source->applied = forward; ++source->generation; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!SameScene() || !AttachmentCurrent() || !InitialCurrent() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, generation != 1) || !CaptureCurrent()) return MMS_E_BUSY;
            if (!Finite(Read(app))) return MMS_E_ARGUMENT;
            out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
struct Gesture {
    MMDApp* app = nullptr;
    SharedHistory* history = nullptr;
    HWND owner = nullptr;
    int operation = 0;
    std::uint64_t revision = 0;
    std::shared_ptr<Change> change;
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    bool Current() const noexcept {
        std::uint64_t current = 0;
        return !history->Revision(current) && current == revision && change->SourceCurrent() && Equal(Read(*app), change->after);
    }
    bool Rollback() noexcept {
        // A foreign evaluation domain cannot be restored into. Within the
        // same domain, cancel each field we still own independently: a plugin
        // changing position must not strand our rotation preview without a
        // journal node, nor can a same-field author be overwritten by cancel.
        if (!change->OwnedPreviewDomain()) return false;
        const auto current=Read(*app);const auto changed=Mask(change->before,change->after);
        unsigned restore=0;
        for(unsigned i=0;i<current.size();++i)if((changed&(1u<<i))&&
            !std::memcmp(&current[i],&change->after[i],sizeof(float)))restore|=1u<<i;
        Write(*app, change->before, restore);return restore!=0||changed==0;
    }
};
thread_local std::unique_ptr<Gesture> gesture;
}
bool CameraHudOperation(MMDApp& app, int operation) noexcept {
    return operation >= 15 && operation <= 20 && app.CameraMode() != 0 && EffectiveCoordinateState(&app) != 2;
}
bool CameraHudGestureActive(const MMDApp& app) noexcept { return gesture && gesture->app == &app; }
MmsResult BeginCameraHudGesture(MMDApp& app, SharedHistory& history, HWND owner, int operation) noexcept {
    if (!CameraHudOperation(app, operation)) return MMS_E_NOT_READY;
    if (auto result = history.CheckNativeNavigation()) return result;
    if (auto result = history.CheckNativeEdit()) return result;
    if (gesture || !Ready(app) || (GetCapture() && GetCapture() != owner)) return MMS_E_BUSY;
    if (!Finite(Read(app))) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        auto next = std::make_unique<Gesture>(); next->app = &app; next->history = &history;
        next->owner = owner; next->operation = operation; next->change = std::make_shared<Change>(app);
        next->change->owner = owner;
        if (auto result = history.Revision(next->revision)) return result;
        if (auto result = history.PrepareNative("Drag camera HUD axis", next->change, next->transaction,
            SharedHistory::NativeIntent::Adjustment)) return result;
        gesture = std::move(next); return MMS_OK;
    });
}
bool CancelCameraHudGesture(MMDApp& app) noexcept {
    if (!CameraHudGestureActive(app)) return false;
    auto current = std::move(gesture);
    const bool restored = current->Rollback(); current->transaction.reset();
    FinishCameraHudInteraction(&app);
    if (current->owner && GetCapture() == current->owner) ReleaseCapture();
    if (restored) updates::RequestViewport(&app);
    return restored;
}
bool AllowCameraHudPreview(MMDApp& app, int operation) noexcept {
    if (!CameraHudGestureActive(app) || gesture->operation != operation) return false;
    if (!Ready(app) || !CameraHudOperation(app, operation) ||
        (gesture->owner && GetCapture() != gesture->owner) || !gesture->Current()) {
        CancelCameraHudGesture(app); return false;
    }
    return true;
}
bool AcceptCameraHudPreview(MMDApp& app) noexcept {
    if (!CameraHudGestureActive(app)) return false;
    if (!gesture->change->SourceCurrent()) { CancelCameraHudGesture(app); return false; }
    const auto values = Read(app);
    if (!Finite(values)) {
        Write(app, gesture->change->after); CancelCameraHudGesture(app); return false;
    }
    gesture->change->after = values;
    gesture->change->mask = Mask(gesture->change->before, values);
    return true;
}
MmsResult EndCameraHudGesture(MMDApp& app, bool& changed) noexcept {
    changed = false;
    if (!CameraHudGestureActive(app)) return MMS_E_NOT_READY;
    auto current = std::move(gesture);
    if (!Ready(app) || (current->owner && GetCapture() != current->owner) || !current->Current()) {
        current->Rollback(); return MMS_E_STALE;
    }
    if (!current->change->mask) return MMS_OK;
    const auto result = current->history->CommitNative(std::move(current->transaction));
    if (result) { current->Rollback(); return result; }
    changed = true; return MMS_OK;
}
bool HandleCameraHudMessage(MMDApp& app, HWND owner, UINT message, WPARAM wp, LPARAM lp) noexcept {
    if (!CameraHudGestureActive(app)) return false;
    if (message == WM_CANCELMODE || message == WM_KILLFOCUS || message == WM_CLOSE ||
        message == WM_DESTROY || message == WM_NCDESTROY ||
        (message == WM_ACTIVATEAPP && !wp) || (message == WM_KEYDOWN && wp == VK_ESCAPE) ||
        (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lp) != gesture->owner)) {
        CancelCameraHudGesture(app); RefreshBoundPoseReadout(app);
        return message == WM_KEYDOWN;
    }
    if (message == WM_LBUTTONUP && gesture->owner == owner) {
        const auto source = gesture->change;
        const int operation = gesture->operation;
        const bool allowed = AllowCameraHudPreview(app, operation);
        if (!CameraHudGestureActive(app) || gesture->change != source) return true;
        if (allowed) {
            // Centered HUD drags warp on press and at viewport edges. A queued
            // release can still carry pre-warp client coordinates, so follow
            // MouseInteractionBegin's live screen->owner-client sampling. If
            // the OS sample fails, retain the last valid native preview.
            POINT point{};
            if(owner&&GetCursorPos(&point)&&ScreenToClient(owner,&point)){
                app.MouseX()=point.x;app.MouseY()=point.y;
                app.ShiftModifierState() = (wp & MK_SHIFT) ? 3 : 0;
                app.CtrlModifierState() = (wp & MK_CONTROL) ? 3 : 0;
                if (operation <= 17) ModeCameraAdjust(&app, operation - 15);
                else ModeAngleAdjust(&app, operation - 18);
                // A native readout callback can cancel this release and begin
                // another HUD gesture. Its cursor snapshots and capture belong
                // to that new gesture; swallow this old mouse-up entirely.
                if (!CameraHudGestureActive(app) || gesture->change != source) return true;
                app.PreviousMouseX() = app.MouseX(); app.PreviousMouseY() = app.MouseY();
            }
        }
        bool changed = false;
        if (CameraHudGestureActive(app)) {
            auto* history = gesture->history;
            const auto result = EndCameraHudGesture(app, changed);
            if (!result && changed) PublishBoundPoseEdit(app, *history, false);
            else RefreshBoundPoseReadout(app);
        }
        // The usual window mouse-up path still releases capture and finishes
        // the native cursor and interaction state. It sees no pending journal.
    }
    return false;
}
}
#endif
