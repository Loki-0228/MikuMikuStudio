#include "native_key_paste.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_morph_keys.hpp"
#include "native_display_keys.hpp"
#include "native_global_paste.hpp"
#include "native_model_paste.hpp"
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <array>
namespace mikumikustudio::plugins {
bool TryPasteBoundKeys(MMDApp* app, HWND window, bool mirror, bool* missingTrack) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    // Unsupported families are rejected as a whole, never partly committed.
    const auto& copied = app->ClipboardCounts();
    const bool global = app->CameraMode() != 0;
    const unsigned families = unsigned(copied.bones != 0) + unsigned(copied.morphs != 0) + unsigned(copied.displays != 0);
    if ((global ? mirror : (mirror ? !copied.bones : !families)) ||
        app->PlaybackActive() || app->FrameStepPlayback() ||
        app->state.recordPlaybackStartPending || app->SceneMutationInProgress() ||
        bake::UiLocked() || SceneSaveBlocksEdits() || GetCapture() ||
        app->FrameRangeDialog() || app->state.frameCopyDialog || app->RecordingWindow() ||
        app->GravitySettingDialog() || app->GroundShadowColorDialog() || app->AccessoryOrderArray() ||
        app->state.accessoryEditDialogOpen || NativeModelOutlineDialogBlocks(*app) ||
        history->CheckNativeEdit() != MMS_OK) return true;
    Boundary([&]() -> MmsResult {
        std::shared_ptr<HistoryChange> change;
        const auto slot = app->SelectedModelSlot();
        const bool morph = !global && !mirror && copied.morphs != 0;
        const bool display = !global && !mirror && copied.displays != 0;
        const bool mixed = !global && !mirror && families > 1;
        MmsResult result;
        if (global) result = PlanGlobalKeyPaste(*app, change);
        else if (mirror) result = PlanBoneKeyPaste(*app, slot, true, change);
        else {
            // Clipboard-only identity resolution belongs here, not in the
            // shared VMD capture path or mirror's left/right name mapping.
            ModelPasteSource source;
            const unsigned capture = mixed ? ModelPasteAll : morph ? ModelPasteMorphs :
                display ? ModelPasteDisplays : ModelPasteBones;
            if (auto captured = CaptureModelPasteSource(*app, source, capture)) return captured;
            const auto* model = slot < kModelSlotCount ? app->ModelSlot(slot) : nullptr;
            const auto resolveTargets = [&](auto& records, auto resolve) {
                for (std::size_t i = 0; i < records.size(); ++i) {
                    const int target = resolve(app, model, i);
                    if (target == frame_clipboard::kMissingTarget) {
                        if (missingTrack) *missingTrack = true;
                        return false;
                    }
                    if (target >= 0) {
                        records[i].targetResolved = true;
                        records[i].targetIndex = target;
                    }
                }
                return true;
            };
            if (!resolveTargets(source.bones, frame_clipboard::BoneTarget) ||
                !resolveTargets(source.morphs, frame_clipboard::MorphTarget)) return MMS_E_NOT_FOUND;
            result = mixed ? PlanModelKeyPaste(*app, slot, source, change) : morph ? PlanMorphKeyPaste(*app, slot, source, change) :
                display ? PlanDisplayKeyPaste(*app, slot, source, change) : PlanBoneKeyPaste(*app, slot, false, source, change);
        }
        if (result) return result;
        if (!change) return MMS_OK;
        std::array<bool,255> accessories{};
        if(global)for(unsigned i=0;i<copied.accessories;++i)accessories[app->AccessoryClipboard()[i].slot]=true;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        const char* label = global ? "Paste global keys" : mixed ? "Paste model keys" : mirror ? "Mirror paste bone keys" : morph ? "Paste morph keys"
            : display ? "Paste display keys" : "Paste bone keys";
        if (auto result = history->PrepareNative(label,
            std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(*app, *history, global);
        if(global){
            for(auto kind:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow,curve_host::Kind::Gravity})
                curve_host::NotifyRegistered(app,kind);
            for(unsigned i=0;i<accessories.size();++i)if(accessories[i])curve_host::NotifyRegistered(app,curve_host::Kind::Accessory,int(i));
        }else if(mixed){curve_host::NotifyModelRegistered(app,false);if(copied.morphs)curve_host::NotifyModelRegistered(app,true);}
        else if (!display) curve_host::NotifyModelRegistered(app, morph);
        SelectionReeval(app);
        if (app->MainWindow() && app->Renderer()) PanelPaint(app);
        if (window) SetFocus(window);
        return MMS_OK;
    });
    return true;
}
}
#endif
