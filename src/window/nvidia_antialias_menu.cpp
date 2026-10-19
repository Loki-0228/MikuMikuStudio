#include "mikumikustudio/nvidia_antialias_menu.hpp"
#include "mikumikustudio/nvidia_antialias_fix.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "nvidia_antialias_uninstall_dialog.hpp"

namespace mikumikustudio::nvidia_antialias_menu {
namespace {
constexpr UINT kAboutCommand = 201;

HMENU FindHelp(MMDApp* app) {
    const HMENU bar = app->FullscreenMode() ? app->state.savedMenu : GetMenu(app->MainWindow());
    for (int i = 0; i < GetMenuItemCount(bar); ++i) {
        const HMENU popup = GetSubMenu(bar, i);
        if (popup && GetMenuState(popup, kAboutCommand, MF_BYCOMMAND) != UINT(-1))
            return popup;
    }
    return nullptr;
}

void Refresh(MMDApp* app) {
    const HMENU help = FindHelp(app);
    if (!help) return;
    const bool exists = GetMenuState(help, kInstallCommand, MF_BYCOMMAND) != UINT(-1);
    if (!nvidia_antialias_fix::ShouldOfferInstall()) {
        if (exists) DeleteMenu(help, kInstallCommand, MF_BYCOMMAND);
        return;
    }
    bool installed = false;
    std::wstring error;
    nvidia_antialias_fix::QueryInstalled(installed, error, app->EnglishUI());
    const wchar_t* label = installed ? (app->EnglishUI() == 2 ? L"卸载 NVIDIA 双面绘制补丁…"
        : app->EnglishUI() == 1 ? L"Uninstall NVIDIA double-sided rendering fix…"
        : L"NVIDIA 両面描画の修正をアンインストール…")
        : app->EnglishUI() == 2 ? L"安装 NVIDIA 双面绘制补丁…"
        : app->EnglishUI() == 1 ? L"Install NVIDIA double-sided rendering fix…"
        : L"NVIDIA 両面描画の修正をインストール…";
    if (exists) {
        ModifyMenuW(help, kInstallCommand, MF_BYCOMMAND | MF_STRING, kInstallCommand, label);
    } else {
        // About identifies Help across language changes and plugin menu insertion.
        InsertMenuW(help, kAboutCommand, MF_BYCOMMAND | MF_STRING, kInstallCommand, label);
    }
}
}

bool HandleMessage(MMDApp* app, UINT message, WPARAM wp, LPARAM lp) {
    if (build_profile::original || !app || !app->MainWindow()) return false;
    if (message == WM_INITMENU) {
        Refresh(app);
    } else if (message == WM_COMMAND && HIWORD(wp) == 0 && lp == 0 && LOWORD(wp) == kInstallCommand) {
        std::wstring error;
        bool wasInstalled = false;
        if (!nvidia_antialias_fix::QueryInstalled(wasInstalled, error, app->EnglishUI())) {
            MessageBoxW(app->MainWindow(), error.c_str(), L"NVIDIA", MB_OK | MB_ICONWARNING);
            return true;
        }
        if (wasInstalled && !uninstall_dialog::Confirm(app->MainWindow(), app->EnglishUI())) return true;
        const bool completed = wasInstalled
            ? nvidia_antialias_fix::UninstallForCurrentExecutable(app->MainWindow(), error, app->EnglishUI())
            : nvidia_antialias_fix::InstallForCurrentExecutable(app->MainWindow(), error, app->EnglishUI());
        Refresh(app);
        const wchar_t* title = L"NVIDIA";
        if (completed) {
            std::wstring text = wasInstalled ? (app->EnglishUI() == 2
                ? L"NVIDIA 双面绘制补丁已卸载。下次启动 MMS 时生效。保留 MMSNvidiaAAFix.dll 可通过帮助菜单重新安装。"
                : app->EnglishUI() == 1
                ? L"The NVIDIA double-sided rendering fix has been uninstalled. The change takes effect the next time MMS starts. Keep MMSNvidiaAAFix.dll to reinstall it from Help."
                : L"NVIDIA 両面描画の修正をアンインストールしました。次回の MMS 起動時に反映されます。MMSNvidiaAAFix.dll を残しておくと、ヘルプから再インストールできます。")
                : app->EnglishUI() == 2
                ? L"NVIDIA 双面绘制修复已安装，下次启动 MMS 时生效。\n现在可以删除 MMSNvidiaAAFix.dll；若希望以后通过帮助菜单卸载，请保留该文件。"
                : app->EnglishUI() == 1
                ? L"The NVIDIA double-sided rendering fix is installed. It takes effect the next time MMS starts.\nYou can now delete MMSNvidiaAAFix.dll, or keep it to uninstall the fix from Help later."
                : L"NVIDIA 両面描画の修正をインストールしました。次回の MMS 起動時に反映されます。\nMMSNvidiaAAFix.dll は削除してかまいません。後でヘルプからアンインストールする場合は、このファイルを残してください。";
            if (!error.empty()) text += L"\n\n" + error;
            MessageBoxW(app->MainWindow(), text.c_str(), title,
                MB_OK | (error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
        } else if (!error.empty()) {
            MessageBoxW(app->MainWindow(), error.c_str(), title, MB_OK | MB_ICONERROR);
        }
        return true;
    }
    return false;
}
}
