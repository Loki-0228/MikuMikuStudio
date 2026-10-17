#include "native_vmd_import.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/runtime_log.hpp"
namespace mikudancestudio::plugins {
MmsResult CheckVmdImportHistory(MMDApp* app) noexcept {
    if (!app) return MMS_E_ARGUMENT;
    return BoundNativePoseHistory(app) ? MMS_E_NOT_READY : MMS_OK;
}
bool RefuseBoundVmdImport(MMDApp* app) noexcept {
    if (CheckVmdImportHistory(app) != MMS_E_NOT_READY) return false;
    runtime_log::Write("controlled VMD import refused: private multi-track import is not implemented");
    const auto owner = app->MainWindow();
    if (owner) {
        const int language = app->EnglishUI();
        const wchar_t* text = language == 1
            ? L"VMD motion import is not available in the shared-history test mode yet. No motion data was changed."
            : language == 2
                ? L"共同历史测试模式暂不支持 VMD 动作导入。动作数据未修改。"
                : L"共通履歴のテストモードでは、VMD モーションの読み込みは未対応です。モーションデータは変更されていません。";
        const wchar_t* title = language == 1 ? L"Load motion data"
            : language == 2 ? L"载入动作数据" : L"モーション読込";
        MessageBoxW(owner, text, title, MB_OK | MB_ICONINFORMATION);
    }
    return true;
}
}
#endif
