#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include "mikumikustudio/nvidia_antialias_fix.hpp"

namespace mikumikustudio::nvidia_antialias_fix {
namespace {
const wchar_t* Localized(int language, const wchar_t* chinese, const wchar_t* english, const wchar_t* japanese) {
    return language == 2 ? chinese : language == 1 ? english : japanese;
}
std::wstring PatchPath() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return {};
    const std::wstring module(path, length);
    return module.substr(0, module.find_last_of(L"\\/") + 1) + L"MMSNvidiaAAFix.dll";
}
}

bool ShouldOfferInstall() {
    const auto path = PatchPath();
    const auto attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
bool QueryInstalled(bool& installed, std::wstring& error, int language) {
    installed = false;
    error.clear();
    const auto path = PatchPath();
    const HMODULE module = path.empty() ? nullptr : LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Query = HRESULT (WINAPI*)(BOOL*);
    const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "MMSNvidiaAAFixQueryInstalled")) : nullptr;
    BOOL value = FALSE;
    const HRESULT result = query ? query(&value) : E_NOINTERFACE;
    if (module) FreeLibrary(module);
    if (result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
        error = Localized(language,
            L"此补丁仅适用于 NVIDIA 显卡。\n当前未检测到可用的 NVIDIA 驱动，无法安装或卸载补丁。AMD / Intel 显卡无需安装此补丁。",
            L"This fix is only for NVIDIA graphics cards.\nNo usable NVIDIA driver was detected, so the fix cannot be installed or uninstalled. AMD and Intel graphics cards do not need it.",
            L"この修正は NVIDIA グラフィックスカード専用です。\n利用可能な NVIDIA ドライバーが検出されないため、インストールやアンインストールはできません。AMD / Intel のグラフィックスカードには不要です。");
        return false;
    }
    if (FAILED(result)) {
        error = Localized(language, L"无法读取 NVIDIA 补丁状态。请确认补丁文件完整，且 NVIDIA 驱动可用。",
            L"Could not read the NVIDIA patch status. Check that the patch file is intact and the NVIDIA driver is available.",
            L"NVIDIA 修正の状態を確認できません。修正ファイルが破損していないことと、NVIDIA ドライバーが利用可能なことを確認してください。");
        return false;
    }
    installed = value != FALSE;
    return true;
}
namespace {
bool ChangeForCurrentExecutable(void* window, bool install, std::wstring& error, int language) {
    error.clear();
    if (!ShouldOfferInstall()) {
        error = Localized(language, L"程序目录中没有 MMSNvidiaAAFix.dll。",
            L"MMSNvidiaAAFix.dll is not in the program folder.",
            L"プログラムのフォルダーに MMSNvidiaAAFix.dll がありません。");
        return false;
    }
    wchar_t system[32768]{};
    const auto length = GetSystemDirectoryW(system, 32768);
    if (!length || length >= 32768) {
        error = Localized(language, L"无法找到 Windows 注册工具。", L"Could not find the Windows registration tool.",
            L"Windows の登録ツールが見つかりません。");
        return false;
    }
    const std::wstring executable = std::wstring(system, length) + L"\\regsvr32.exe";
    const auto patch = PatchPath();
    const std::wstring parameters = std::wstring(install ? L"/s \"" : L"/s /u \"") + patch + L"\"";
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.hwnd = static_cast<HWND>(window);
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) {
        const DWORD code = GetLastError();
        if (code != ERROR_CANCELLED)
            error = std::wstring(Localized(language, L"无法启动补丁操作。", L"Could not start the patch operation.",
                L"修正の操作を開始できませんでした。")) + L"\nWindows " + std::to_wstring(code);
        return false;
    }
    if (!launch.hProcess) {
        error = Localized(language, L"无法取得补丁进程的状态。", L"Could not check the patch process's status.",
            L"修正プロセスの状態を確認できませんでした。");
        return false;
    }
    const DWORD waited = WaitForSingleObject(launch.hProcess, 30000);
    DWORD code = ERROR_GEN_FAILURE;
    const bool completed = waited == WAIT_OBJECT_0 && GetExitCodeProcess(launch.hProcess, &code);
    CloseHandle(launch.hProcess);
    if (!completed || code != ERROR_SUCCESS) {
        error = waited == WAIT_TIMEOUT ? Localized(language,
            L"补丁操作尚未完成，暂时不要删除 MMSNvidiaAAFix.dll。",
            L"The patch operation has not finished. Keep MMSNvidiaAAFix.dll until it completes.",
            L"修正の操作はまだ完了していません。MMSNvidiaAAFix.dll はまだ削除しないでください。") :
            std::wstring(Localized(language, L"补丁操作失败，请保留 MMSNvidiaAAFix.dll 后重试。",
                L"The patch operation failed. Keep MMSNvidiaAAFix.dll and try again.",
                L"修正の操作に失敗しました。MMSNvidiaAAFix.dll を残したまま、もう一度お試しください。")) +
                L"\nWindows " + std::to_wstring(code);
        return false;
    }
    return true;
}
}
bool InstallForCurrentExecutable(void* window, std::wstring& error, int language) {
    return ChangeForCurrentExecutable(window, true, error, language);
}
bool UninstallForCurrentExecutable(void* window, std::wstring& error, int language) {
    return ChangeForCurrentExecutable(window, false, error, language);
}
}
