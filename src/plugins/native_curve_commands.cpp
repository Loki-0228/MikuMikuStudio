#include "native_curve_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_curve_write.hpp"
#include "native_morph_curve_write.hpp"
#include "native_bone_curve_write.hpp"
#include "native_global_commands.hpp"
#include "native_morph_commands.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "history_runtime.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/runtime_log.hpp"

namespace mikudancestudio::plugins {
namespace {
const wchar_t* Text(const MMDApp& app, const wchar_t* ja, const wchar_t* en, const wchar_t* zh) noexcept {
    return app.EnglishUI() == 0 ? ja : app.EnglishUI() == 1 ? en : zh;
}
bool Fail(std::wstring& error, const wchar_t* message) noexcept {
    try { error = message; } catch (...) { error.clear(); }
    return false;
}
bool Ready(MMDApp& app) noexcept {
    const auto main = app.MainWindow();
    return main && IsWindow(main) && IsWindowEnabled(main) &&
        GetWindowThreadProcessId(main, nullptr) == GetCurrentThreadId() &&
        !InCallback() && !NativeObjectOperationActive(&app) && !NativeObjectUiPending(&app) &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.SceneMutationInProgress() && !bake::UiLocked() && !SceneSaveInProgress() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
template<class Plan>
bool CommitCurveWrite(MMDApp& app, const char* label, Plan&& plan, std::wstring& error) noexcept {
    auto* history = BoundNativePoseHistory(&app);
    if (!history || !Ready(app)) return Fail(error, Text(app, L"処理中のためカーブを変更できません。", L"Wait for the active operation before editing curves.", L"当前操作尚未结束，无法修改曲线。"));
    const auto binding = BoundNativePoseGeneration(&app);
    bool changed = false;
    const auto result = Boundary([&]() -> MmsResult {
        if (auto r = history->CheckNativeEdit()) return r;
        std::shared_ptr<HistoryChange> change;
        if (auto r = plan(change)) return r;
        if (!change) return MMS_OK;
        if (BoundNativePoseHistory(&app) != history || BoundNativePoseGeneration(&app) != binding || !Ready(app)) return MMS_E_STALE;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto r = history->PrepareNative(label, std::move(change), transaction,
            SharedHistory::NativeIntent::ExplicitCommit)) return r;
        if (auto r = history->CommitNative(std::move(transaction))) return r;
        changed = true; return MMS_OK;
    });
    if (result != MMS_OK) {
        runtime_log::Write("PLUGIN_ABI curve write refused label=%s result=%d", label, result);
        return Fail(error, result == MMS_E_MEMORY ? Text(app, L"メモリまたは履歴の上限を超えました。元データは変更されていません。", L"Curve changes exceed available memory or the history budget. Original data is unchanged.", L"曲线修改超过可用内存或历史预算，原数据已保留。") :
            result == MMS_E_STALE ? Text(app, L"カーブの参照元が変更されました。再読込してください。", L"The curve source changed. Refresh before retrying.", L"曲线来源已经变化，请刷新后重试。") :
            Text(app, L"カーブの変更を確定できません。元データは変更されていません。", L"Cannot commit curve changes. Original data is unchanged.", L"当前无法提交曲线修改，原数据已保留。"));
    }
    if (changed) {
        // Publication has finished. These are current-state readouts, never a
        // frame re-evaluation that could erase unrelated unregistered poses.
        Boundary([&]() -> MmsResult {
            bake::ScopedUiInputGate lock;
            PublishBoundPoseEdit(app, *history, history->LastPublicationRequiresPhysicsReset());
            RefreshBoundGlobalReadout(app);
            RefreshBoundMorphReadout(app);
            if (app.PanelDC()) PanelPaint(&app);
            curve_editor::NotifyOwnedChanged(app.MainWindow());
            return MMS_OK;
        });
        RequestBoundHistoryUiRefresh(&app);
    }
    return true;
}
}
bool WriteBoundGlobalCurves(MMDApp& app, curve_host::Kind kind, int slot,
    const std::vector<curve_editor::Channel>& channels, std::wstring& error) noexcept {
    return CommitCurveWrite(app, "Edit animation curves", [&](auto& change) {
        return PlanGlobalCurveWrite(app, kind, slot, channels, change);
    }, error);
}
bool WriteBoundMorphCurves(MMDApp& app, unsigned slot,
    const std::vector<animation_curve::NamedTrack>& tracks, std::wstring& error) noexcept {
    return CommitCurveWrite(app, "Edit morph curves", [&](auto& change) {
        return PlanMorphCurveWrite(app, slot, tracks, change);
    }, error);
}
bool WriteBoundBoneCurves(MMDApp& app, unsigned slot,
    const std::vector<BoneCurveEdit>& edits, const std::vector<DisplayCurveRow>* display,
    std::wstring& error) noexcept {
    return CommitCurveWrite(app, "Edit bone curves", [&](auto& change) {
        return PlanBoneCurveWrite(app, slot, edits, display, change);
    }, error);
}
}
#endif
