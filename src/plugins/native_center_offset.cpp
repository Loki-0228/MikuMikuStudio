#include "native_center_offset.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <cmath>
#include <cstring>

namespace mikumikustudio {
INT_PTR __stdcall ModelOffsetDlgProc(HWND, UINT, WPARAM, LPARAM);
}
namespace mikumikustudio::plugins {
namespace {
thread_local bool dialogActive = false;
std::uint64_t AddRetained(std::uint64_t first, std::uint64_t second) noexcept {
    return first > UINT64_MAX - second ? UINT64_MAX : first + second;
}
bool Ready(MMDApp& app) noexcept {
    return !app.CameraMode() && app.SelectedModelSlot() < kModelSlotCount &&
        app.ModelSlot(app.SelectedModelSlot()) && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.SceneMutationInProgress() && !bake::UiLocked() &&
        !SceneSaveInProgress() && !GetCapture() && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.RecordingWindow() && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
struct ParentDependencies final : HistoryChange, std::enable_shared_from_this<ParentDependencies> {
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    mdl::BoneRecord* bones;
    unsigned count;
    std::shared_ptr<RetainedSceneObject> lifetime;
    std::shared_ptr<HistoryChange> inner;
    std::vector<int> parents;
    ParentDependencies(MMDApp& a, unsigned s, std::shared_ptr<HistoryChange> change)
        : app(a), slot(s), model(a.ModelSlot(s)), bones(mdl::Mdl(model)->boneTable),
          count(mdl::Mdl(model)->boneCount), lifetime(RetainSceneObject(a, MMS_OBJECT_MODEL, s)), inner(std::move(change)) {
        parents.reserve(count);
        for (unsigned bone = 0; bone < count; ++bone) parents.push_back(bones[bone].parent);
    }
    bool Current() const noexcept {
        if (app.ModelSlot(slot) != model || !lifetime->Present() || !SceneObjectPresent(model, lifetime->Id())) return false;
        const auto& record = *mdl::Mdl(model);
        if (record.boneTable != bones || record.boneCount != count) return false;
        for (unsigned bone = 0; bone < count; ++bone) if (bones[bone].parent != parents[bone]) return false;
        return true;
    }
    std::uint64_t OwnBytes() const noexcept {
        return AddRetained(sizeof(*this) + 4 * sizeof(void*),
            std::uint64_t(parents.capacity()) * sizeof(int));
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ParentDependencies> source;
        std::unique_ptr<PreparedHistoryChange> inner;
        bool consumed = false;
        Candidate(std::shared_ptr<ParentDependencies> owner, std::unique_ptr<PreparedHistoryChange> value)
            : source(std::move(owner)), inner(std::move(value)) {}
        bool IsCurrent() const noexcept override { return !consumed && source->Current() && inner->IsCurrent(); }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            inner->Publish(); consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& bytes) const noexcept override {
            std::uint64_t innerBytes = 0;
            if (!inner->TargetRetainedBytes(innerBytes)) return false;
            bytes = AddRetained(innerBytes, source->OwnBytes()); return true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!Current()) return MMS_E_STALE;
            std::unique_ptr<PreparedHistoryChange> candidate;
            if (auto result = inner->Prepare(forward, candidate)) return result;
            if (!candidate) return MMS_E_FAILED;
            auto next = std::make_unique<Candidate>(shared_from_this(), std::move(candidate));
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        return AddRetained(inner->RetainedBytes(), OwnBytes());
    }
    bool RequiresPhysicsReset() const noexcept override { return inner->RequiresPhysicsReset(); }
};
}
MmsResult WrapCenterOffsetParentDependencies(MMDApp& app, unsigned slot, std::shared_ptr<HistoryChange>& change) noexcept {
    if (!change) return MMS_OK;
    if (slot >= kModelSlotCount || !app.ModelSlot(slot)) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto& record = *mdl::Mdl(app.ModelSlot(slot));
        if (!record.boneTable || !record.boneCount) return MMS_E_NOT_READY;
        auto next = std::make_shared<ParentDependencies>(app, slot, change);
        if (!next->Current()) return MMS_E_STALE;
        change = std::move(next); return MMS_OK;
    });
}
struct CenterOffsetEdit::Impl {
    MMDApp& app;
    SharedHistory* history;
    std::uint64_t binding, revision = 0;
    unsigned slot;
    unsigned char* model;
    mdl::BoneRecord* bones;
    unsigned count;
    std::shared_ptr<RetainedSceneObject> lifetime;
    int frame;
    std::uint64_t poolIdentity;
    std::vector<int> parents;
    std::vector<mdl::BoneKey> keys;
    bone_scale::AuthoringState scale;
    std::unique_ptr<bone_curve::AuthoringState> curves;
    Impl(MMDApp& a, SharedHistory& h)
        : app(a), history(&h), binding(BoundNativePoseGeneration(&a)), slot(a.SelectedModelSlot()),
          model(a.ModelSlot(slot)), bones(mdl::Mdl(model)->boneTable), count(mdl::Mdl(model)->boneCount),
          lifetime(RetainSceneObject(a, MMS_OBJECT_MODEL, slot)), frame(a.CurrentFrame()),
          poolIdentity(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),
          scale(bone_scale::AuthoringState::Capture(model)), curves(bone_curve::AuthoringState::Capture(model)) {
        const auto& record = *mdl::Mdl(model);
        if (!bones || !count || !record.bonePhysicsState || !poolIdentity ||
            count > mdl::BoneKeyCapacity(record.boneKeys)) throw std::invalid_argument("center offset source");
        parents.reserve(count);
        for (unsigned bone = 0; bone < count; ++bone) parents.push_back(bones[bone].parent);
        keys.assign(record.boneKeys, record.boneKeys + mdl::BoneKeyCapacity(record.boneKeys));
    }
    bool Current() const {
        // Compare the binding before calling the borrowed history pointer. A
        // modal loop may have destroyed it and installed a replacement journal.
        if (BoundNativePoseHistory(&app) != history || BoundNativePoseGeneration(&app) != binding ||
            !Ready(app) || app.SelectedModelSlot() != slot || app.ModelSlot(slot) != model ||
            app.CurrentFrame() != frame || !lifetime->Present() || !SceneObjectPresent(model, lifetime->Id())) return false;
        const auto& record = *mdl::Mdl(model);
        if (record.boneTable != bones || record.boneCount != count ||
            mdl::BoneKeyPoolIdentity(record.boneKeys) != poolIdentity || mdl::BoneKeyCapacity(record.boneKeys) != keys.size()) return false;
        for (unsigned bone = 0; bone < count; ++bone) if (bones[bone].parent != parents[bone]) return false;
        if (std::memcmp(record.boneKeys, keys.data(), keys.size() * sizeof(mdl::BoneKey)) ||
            !bone_scale::AuthoringState::Capture(model).Equals(scale) ||
            !bone_curve::AuthoringState::Capture(model)->Equals(*curves)) return false;
        std::uint64_t current = 0;
        return history->Revision(current) == MMS_OK && current == revision;
    }
};
CenterOffsetEdit::CenterOffsetEdit() = default;
CenterOffsetEdit::~CenterOffsetEdit() = default;
MmsResult CenterOffsetEdit::Begin(MMDApp& app, SharedHistory& history) noexcept {
    if (impl_) return MMS_E_BUSY;
    if (BoundNativePoseHistory(&app) != &history || !Ready(app)) return MMS_E_NOT_READY;
    if (auto result = history.CheckNativeNavigation()) return result;
    if (auto result = history.CheckNativeEdit()) return result;
    const auto& record = *mdl::Mdl(app.SelectedModel());
    if (!record.boneTable || !record.boneCount || !record.bonePhysicsState ||
        !mdl::BoneKeyPoolIdentity(record.boneKeys) || record.boneCount > mdl::BoneKeyCapacity(record.boneKeys)) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto next = std::make_unique<Impl>(app, history);
        if (auto result = history.Revision(next->revision)) return result;
        if (!next->Current()) return MMS_E_STALE;
        impl_ = std::move(next); return MMS_OK;
    });
}
MmsResult CenterOffsetEdit::Apply(const std::array<float, 3>& offset) noexcept {
    if (!impl_) return MMS_E_NOT_READY;
    auto session = std::move(impl_);
    return Boundary([&]() -> MmsResult {
        if (BoundNativePoseHistory(&session->app) != session->history ||
            BoundNativePoseGeneration(&session->app) != session->binding) return MMS_E_STALE;
        if (auto result = session->history->CheckNativeNavigation()) return result;
        if (auto result = session->history->CheckNativeEdit()) return result;
        if (!session->Current()) return MMS_E_STALE;
        for (float value : offset) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanBoneCenterOffset(session->app, session->slot, offset, change)) return result;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto result = session->history->PrepareNative("Offset model root keys", std::move(change), transaction,
            SharedHistory::NativeIntent::ExplicitCommit)) return result;
        if (auto result = session->history->CommitNative(std::move(transaction))) return result;
        // Optional Win32/editor refresh cannot turn an already published edit
        // into a failure that invites the caller to repeat the operation.
        Boundary([&]() -> MmsResult {
            bake::ScopedUiInputGate input;
            PublishBoundPoseEdit(session->app, *session->history, true);
            curve_host::NotifyModelRegistered(&session->app, false);
            if (session->app.Renderer() && session->app.PanelDC()) {
                PanelPaint(&session->app); SelectionReeval(&session->app);
            }
            return MMS_OK;
        });
        return MMS_OK;
    });
}
bool TryEditBoundCenterOffset(MMDApp* app) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app); if (!history) return false;
    if (dialogActive) return true;
    const auto result = Boundary([&]() -> MmsResult {
        CenterOffsetEdit edit; if (auto result = edit.Begin(*app, *history)) return result;
        struct DialogScope {
            MMDApp& app;
            std::array<float, 3> before;
            explicit DialogScope(MMDApp& value) : app(value), before{value.state.modelOffsetX, value.state.modelOffsetY, value.state.modelOffsetZ} {
                dialogActive = true;
            }
            ~DialogScope() {
                // The old dialog writes application scratch fields on OK. They
                // are input only, including when post-modal validation refuses.
                app.state.modelOffsetX = before[0]; app.state.modelOffsetY = before[1]; app.state.modelOffsetZ = before[2];
                dialogActive = false;
            }
        } scope(*app);
        app->state.dialogFlags[6] = 1; app->state.enterKeyState = 1;
        const auto response = UiDialogBoxParamA(static_cast<HINSTANCE>(app->HInstance()),
            MAKEINTRESOURCEA(app->EnglishUI() != 0 ? 0x28B : 0x258), app->MainWindow(), ModelOffsetDlgProc, 0);
        if (response == -1) return MMS_E_FAILED;
        if (response != 1) return MMS_OK;
        return edit.Apply({app->state.modelOffsetX, app->state.modelOffsetY, app->state.modelOffsetZ});
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI center offset refused: %d", result);
    return true;
}
}
#endif
