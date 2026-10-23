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

const wchar_t* InstallLabel(int language, bool installed) {
    if (!installed)
        return language == 2 ? L"安装 NVIDIA 双面绘制补丁…"
            : language == 1 ? L"Install NVIDIA double-sided rendering fix…"
            : L"NVIDIA 両面描画の修正をインストール…";
    return language == 2 ? L"重新安装 NVIDIA 双面绘制补丁…"
        : language == 1 ? L"Reinstall NVIDIA double-sided rendering fix…"
        : L"NVIDIA 両面描画の修正を再インストール…";
}

const wchar_t* UninstallLabel(int language) {
    return language == 2 ? L"卸载 NVIDIA 双面绘制补丁…"
        : language == 1 ? L"Uninstall NVIDIA double-sided rendering fix…"
        : L"NVIDIA 両面描画の修正をアンインストール…";
}

void RemoveEntries(HMENU help) {
    if (GetMenuState(help, kInstallCommand, MF_BYCOMMAND) != UINT(-1))
        DeleteMenu(help, kInstallCommand, MF_BYCOMMAND);
    if (GetMenuState(help, kUninstallCommand, MF_BYCOMMAND) != UINT(-1))
        DeleteMenu(help, kUninstallCommand, MF_BYCOMMAND);
}

void Refresh(MMDApp* app) {
    const HMENU help = FindHelp(app);
    if (!help) return;
    // Rebuild both entries so labels follow the language and installed state.
    RemoveEntries(help);
    if (!nvidia_antialias_fix::ShouldOfferInstall()) return;
    bool installed = false;
    std::wstring error;
    // An unreadable state keeps the install command visible. Running it reports
    // the same driver error instead of leaving the user without an entry.
    nvidia_antialias_fix::QueryInstalled(installed, error, app->EnglishUI());
    InsertMenuW(help, kAboutCommand, MF_BYCOMMAND | MF_STRING, kInstallCommand,
        InstallLabel(app->EnglishUI(), installed));
    // The fix is bound to this executable path, so an installed program still
    // needs an explicit way to reapply the setting after replacing its EXE.
    if (installed)
        InsertMenuW(help, kAboutCommand, MF_BYCOMMAND | MF_STRING, kUninstallCommand,
            UninstallLabel(app->EnglishUI()));
}

std::wstring ResultText(int language, bool uninstall, bool wasInstalled) {
    if (uninstall)
        return language == 2
            ? L"NVIDIA 双面绘制补丁已卸载。下次启动 MMS 时生效。保留 MMSNvidiaAAFix.dll 可通过帮助菜单重新安装。"
            : language == 1
            ? L"The NVIDIA double-sided rendering fix has been uninstalled. The change takes effect the next time MMS starts. Keep MMSNvidiaAAFix.dll to reinstall it from Help."
            : L"NVIDIA 両面描画の修正をアンインストールしました。次回の MMS 起動時に反映されます。MMSNvidiaAAFix.dll を残しておくと、ヘルプから再インストールできます。";
    if (!wasInstalled)
        return language == 2
            ? L"NVIDIA 双面绘制修复已安装，下次启动 MMS 时生效。\n该设置只作用于当前目录的 MikuMikuDance.exe；换用其他目录的程序时，需要在那个目录重新安装。\n保留 MMSNvidiaAAFix.dll 才能继续使用帮助菜单中的安装和卸载。"
            : language == 1
            ? L"The NVIDIA double-sided rendering fix is installed. It takes effect the next time MMS starts.\nThe setting applies only to MikuMikuDance.exe in this folder; a copy in another folder needs its own installation.\nKeep MMSNvidiaAAFix.dll to keep the install and uninstall commands available."
            : L"NVIDIA 両面描画の修正をインストールしました。次回の MMS 起動時に反映されます。\nこの設定はこのフォルダーの MikuMikuDance.exe にだけ適用されます。別のフォルダーのプログラムには、そのフォルダーでもう一度インストールしてください。\nインストールとアンインストールの項目を残すには MMSNvidiaAAFix.dll を保持してください。";
    return language == 2
        ? L"NVIDIA 双面绘制修复已重新应用，下次启动 MMS 时生效。\n如果仍出现白边，请确认「显示 → 抗锯齿」使用 MMD 原始（MSAA），并确认运行的是当前目录的 MikuMikuDance.exe。"
        : language == 1
        ? L"The NVIDIA double-sided rendering fix has been reapplied. It takes effect the next time MMS starts.\nIf white edges remain, make sure Display > Anti-aliasing uses MMD original (MSAA) and that you are running this folder's MikuMikuDance.exe."
        : L"NVIDIA 両面描画の修正を再適用しました。次回の MMS 起動時に反映されます。\n白い縁が残る場合は、「表示 → アンチエイリアス」が MMD 標準（MSAA）になっていることと、このフォルダーの MikuMikuDance.exe を実行していることを確認してください。";
}
}

bool HandleMessage(MMDApp* app, UINT message, WPARAM wp, LPARAM lp) {
    if (build_profile::original || !app || !app->MainWindow()) return false;
    if (message == WM_INITMENU) {
        Refresh(app);
    } else if (message == WM_COMMAND && HIWORD(wp) == 0 && lp == 0 &&
        (LOWORD(wp) == kInstallCommand || LOWORD(wp) == kUninstallCommand)) {
        const bool uninstall = LOWORD(wp) == kUninstallCommand;
        std::wstring error;
        bool wasInstalled = false;
        if (!nvidia_antialias_fix::QueryInstalled(wasInstalled, error, app->EnglishUI())) {
            MessageBoxW(app->MainWindow(), error.c_str(), L"NVIDIA", MB_OK | MB_ICONWARNING);
            return true;
        }
        if (uninstall) {
            if (!wasInstalled) {
                Refresh(app);
                return true;
            }
            if (!uninstall_dialog::Confirm(app->MainWindow(), app->EnglishUI())) return true;
        }
        const bool completed = uninstall
            ? nvidia_antialias_fix::UninstallForCurrentExecutable(app->MainWindow(), error, app->EnglishUI())
            : nvidia_antialias_fix::InstallForCurrentExecutable(app->MainWindow(), error, app->EnglishUI());
        Refresh(app);
        if (completed) {
            std::wstring text = ResultText(app->EnglishUI(), uninstall, wasInstalled);
            if (!error.empty()) text += L"\n\n" + error;
            MessageBoxW(app->MainWindow(), text.c_str(), L"NVIDIA",
                MB_OK | (error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
        } else if (!error.empty()) {
            MessageBoxW(app->MainWindow(), error.c_str(), L"NVIDIA", MB_OK | MB_ICONERROR);
        }
        return true;
    }
    return false;
}
}
