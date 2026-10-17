#include "native_vmd_import.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_vmd_source.hpp"
#include "native_history_commands.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/runtime_log.hpp"
namespace mikudancestudio::plugins {
MmsResult CheckVmdImportHistory(MMDApp* app) noexcept {
    if (!app) return MMS_E_ARGUMENT;
    return BoundNativePoseHistory(app) ? MMS_E_NOT_READY : MMS_OK;
}
bool RefuseBoundVmdImport(MMDApp* app) noexcept {
    if (CheckVmdImportHistory(app) != MMS_E_NOT_READY || app->CameraMode()) return false;
    runtime_log::Write("controlled model VMD import refused: private model import is not implemented");
    const auto owner = app->MainWindow();
    if (owner) {
        const int language = app->EnglishUI();
        const wchar_t* text = language == 1
            ? L"Model VMD motion import is not available in the shared-history test mode yet. No motion data was changed."
            : language == 2
                ? L"共同历史测试模式暂不支持模型 VMD 动作导入。动作数据未修改。"
                : L"共通履歴のテストモードでは、モデル VMD モーションの読み込みは未対応です。モーションデータは変更されていません。";
        const wchar_t* title = language == 1 ? L"Load motion data"
            : language == 2 ? L"载入动作数据" : L"モーション読込";
        MessageBoxW(owner, text, title, MB_OK | MB_ICONINFORMATION);
    }
    return true;
}
MmsResult PlanGlobalVmdImport(MMDApp& app,const char* ansiPath,const wchar_t* widePath,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if(!app.CameraMode()||app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult {
        GlobalVmdSource decoded;
        if(auto status=DecodeGlobalVmdSource(ansiPath,widePath,unsigned(app.CurrentFrame()),decoded))return status;
        return PlanGlobalKeyPaste(app,decoded.source,out);
    });
}
bool TryImportBoundVmd(MMDApp* app,const char* ansiPath,const wchar_t* widePath,int& result) noexcept {
    if(!app)return false;auto* history=BoundNativePoseHistory(app);if(!history)return false;
    result=0;
    if(!app->CameraMode()){RefuseBoundVmdImport(app);return true;}
    if(app->PlaybackActive()||app->FrameStepPlayback()||app->state.recordPlaybackStartPending||app->SceneMutationInProgress()||
        bake::UiLocked()||SceneSaveInProgress()||GetCapture()||app->FrameRangeDialog()||app->state.frameCopyDialog||
        app->RecordingWindow()||app->GravitySettingDialog()||app->GroundShadowColorDialog()||app->AccessoryOrderArray()||
        app->state.accessoryEditDialogOpen||app->state.edgeThicknessDialog||history->CheckNativeEdit()!=MMS_OK)return true;
    bool committed=false;
    const auto status=Boundary([&]()->MmsResult {
        std::shared_ptr<HistoryChange> change;
        if(auto error=PlanGlobalVmdImport(*app,ansiPath,widePath,change))return error;
        if(!change)return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        if(auto error=history->PrepareNative("Import VMD motion",std::move(change),token,SharedHistory::NativeIntent::ExplicitCommit))return error;
        if(auto error=history->CommitNative(std::move(token)))return error;
        committed=true;
        PublishBoundPoseEdit(*app,*history,true);
        for(auto kind:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow})curve_host::NotifyRegistered(app,kind);
        SelectionReeval(app);if(app->MainWindow()&&app->Renderer())PanelPaint(app);
        return MMS_OK;
    });
    if(status!=MMS_OK&&committed){
        runtime_log::Write("controlled VMD import committed; UI refresh incomplete");
        if(app->MainWindow())InvalidateRect(app->MainWindow(),nullptr,FALSE);
        return true;
    }
    if(status!=MMS_OK){
        runtime_log::Write("controlled global VMD import failed before publication");
        if(const auto owner=app->MainWindow()){
            const int language=app->EnglishUI();
            const wchar_t* text=status==MMS_E_NOT_READY
                ? (language==1?L"This VMD data is not supported by shared-history import yet. No scene data was changed.":language==2?
                    L"共同历史导入暂不支持此 VMD 数据。场景数据未修改。":L"この VMD データは共通履歴の読み込みに未対応です。シーンは変更されていません。")
                : status==MMS_E_NOT_FOUND
                ? (language==1?L"Cannot open VMD file. No scene data was changed.":language==2?L"无法打开 VMD 文件。场景数据未修改。":L"VMD ファイルを開けません。シーンは変更されていません。")
                : status==MMS_E_MEMORY
                ? (language==1?L"Not enough memory or history budget to import VMD. No scene data was changed.":language==2?
                    L"内存或历史预算不足，无法导入 VMD。场景数据未修改。":L"VMD の読み込みに必要なメモリまたは履歴容量が不足しています。シーンは変更されていません。")
                : (language==1?L"The VMD file or target contains invalid data. No scene data was changed.":language==2?
                    L"VMD 文件或目标数据无效。场景数据未修改。":L"VMD ファイルまたは対象データが不正です。シーンは変更されていません。");
            MessageBoxW(owner,text,L"MMS VMD",MB_OK|MB_ICONWARNING);
        }
    }
    return true;
}
}
#endif
