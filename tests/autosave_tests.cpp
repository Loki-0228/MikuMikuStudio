#include "mikumikustudio/background_save.hpp"
// Explicit hidden production test: no windows are shown or activated.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "../src/app/settings_json.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "../src/window/autosave_dialog.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/project_storage.hpp"
#endif
#include <MinHook.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <fcntl.h>
#include <io.h>
#include <share.h>

// Affect only this test thread, and only while an explicit failure probe runs.
// The production path still performs its real std::filesystem/string work.
namespace autosave_allocation_fault {
thread_local bool enabled = false;
thread_local unsigned failures = 0;
void* Allocate(std::size_t bytes) {
    if (enabled) { ++failures; throw std::bad_alloc(); }
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t bytes) { return autosave_allocation_fault::Allocate(bytes); }
void* operator new[](std::size_t bytes) { return autosave_allocation_fault::Allocate(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace as = mikumikustudio::autosave;
namespace {
unsigned checks = 0, warnings = 0;
bool expectedWarning = false, crashChild = false;
bool expectedFullWarning = false;
bool failFullWarningCreation = false;
unsigned failedFullWarnings = 0;
unsigned fullWarnings = 0;
std::wstring fullWarningText;
fs::path root, crashDirectory;
void Check(bool good, const char* reason) {
    ++checks;
    if (!good) { std::printf("FAIL %s\n", reason); std::exit(1); }
}
template<class Operation>
void FailNextAllocation(Operation operation, const char* reason) {
    const auto before = autosave_allocation_fault::failures;
    bool escaped = false;
    autosave_allocation_fault::enabled = true;
    try { operation(); } catch (...) { escaped = true; }
    autosave_allocation_fault::enabled = false;
    Check(autosave_allocation_fault::failures > before, "failure probe reached a real C++ allocation");
    Check(!escaped, reason);
}
std::string Bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
#if !MMD_ORIGINAL_PROFILE
plugins::PluginProject expectedPluginProject;
std::string manualPluginBytes;
void CheckPluginData(const fs::path& path) {
    Check(fs::exists(plugins::PluginSidecar(path)), "backup includes its plugin sidecar");
    const auto restored = plugins::ReadPluginProject(path);
    Check(restored.projectId == expectedPluginProject.projectId &&
        restored.records.size() == expectedPluginProject.records.size(),
        "backup pair passes hash validation and preserves plugin project identity");
    for (const auto& [id, expected] : expectedPluginProject.records) {
        const auto found = restored.records.find(id);
        Check(found != restored.records.end() && found->second.schema == expected.schema &&
            found->second.bytes == expected.bytes, "backup preserves every plugin schema and binary payload");
    }
}
void SeedPluginData(MMDApp* app, const fs::path& manual) {
    // Missing plugins must retain their data too. Use the production pair writer
    // and loader, with no test DLL or serializer replacing the live save path.
    auto data = plugins::ReadPluginProject(manual);
    data.records.emplace("test.autosave.binary", plugins::StoredData{17, {0, 1, 128, 255, 0, 42}});
    data.records.emplace("test.autosave.empty", plugins::StoredData{23, {}});
    const fs::path staged(manual.native() + L".seed.tmp");
    Check(fs::copy_file(manual, staged), "stage original PMM for plugin data fixture");
    plugins::PublishProjectPair(manual, staged, data, false);
    Check(fs::remove(staged), "remove completed plugin fixture staging file");
    expectedPluginProject = std::move(data);
    manualPluginBytes = Bytes(plugins::PluginSidecar(manual));
    LoadSceneFile();
    Check(app->CurrentFrame() == 11, "plugin fixture loads through the production project reader");
    CheckPluginData(manual);
}
void CheckManualPluginData() {
    Check(Bytes(plugins::PluginSidecar(root / L"工程原稿.pmm")) == manualPluginBytes,
        "automatic copies never rewrite the manual project's plugin sidecar");
}
void CheckRestoredPluginData(MMDApp* app) {
    static unsigned sequence = 0;
    const auto copy = root / (L"restored-plugin-data-" + std::to_wstring(++sequence) + L".pmm");
    Check(SaveSceneCopy(app, copy.c_str()), "save a copy of the restored live plugin project");
    CheckPluginData(copy);
    CheckManualPluginData();
}
#else
void CheckPluginData(const fs::path&) {}
void SeedPluginData(MMDApp*, const fs::path&) {}
void CheckManualPluginData() {}
void CheckRestoredPluginData(MMDApp*) {}
#endif
int WINAPI MessageW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    if (crashChild && title && wcsstr(title, L"Fatal error")) {
        const auto backups = as::Backups(crashDirectory);
        Check(backups.size() == 1 && backups[0].filename().wstring().find(L"_crash.pmm") != std::wstring::npos,
            "production unhandled exception filter saved a crash backup before reporting");
        Check(Bytes(root / L"工程原稿.pmm") == Bytes(root / L"original-before-crash.bin"), "crash preserved manual project bytes");
        CheckPluginData(backups[0]);
        CheckManualPluginData();
        Check(fs::path(as::Current().directory) == crashDirectory || as::Backups(as::Current().directory).empty(),
            "crash backup ignores the periodic autosave folder override");
        unsigned logs = 0;
        for (const auto& entry : fs::directory_iterator(as::Current().directory)) {
            if (entry.path().extension() != L".log") continue;
            const auto log = Bytes(entry.path());
            Check(entry.path().filename().wstring().find(L"error-") == 0 &&
                log.find("0xE1234567") != std::string::npos && log.find("crash_backup=") != std::string::npos &&
                log.find("CRASH_BACKUP saved") != std::string::npos, "persisted error log includes exception and backup result");
            Check(text && wcsstr(text, entry.path().c_str()), "fatal dialog identifies the saved error log");
            ++logs;
        }
        Check(logs == 1, "one timestamped error log saved in the configured backup folder");
#if !MMD_ORIGINAL_PROFILE
        // This child raises a deliberate non-corrupting exception. Restore its
        // actual crash pair, then serialize the live state to prove adoption.
        wcscpy_s(g_Block->EnvFileName(), 256, backups[0].c_str());
        LoadSceneFile();
        Check(g_Block->CurrentFrame() == 222 && g_Block->CameraDistance() == -77,
            "default and disabled crash backups restore the native project state");
        CheckRestoredPluginData(g_Block);
#endif
        std::puts("PASS actual unhandled-exception recovery");
        // Only the disposable test child terminates; bypass the OS crash UI.
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (failFullWarningCreation) {
        Check(owner == g_Block->MainWindow() && title && flags == (MB_OK | MB_ICONWARNING) &&
            !wcscmp(title, as::Text(g_Block, L"备份文件夹已满", L"Backup folder is full",
                L"バックアップフォルダーがいっぱいです")), "failed window creation probe reached only the capacity warning");
        ++failedFullWarnings;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (expectedFullWarning) {
        ++fullWarnings;
        Check(fullWarnings == 1, "full warning appears at most once per process");
        Check(owner == g_Block->MainWindow(), "full warning belongs to the main window");
        Check(flags == (MB_OK | MB_ICONWARNING), "full warning is informational and does not change backup policy");
        const auto language = g_Block->EnglishUI();
        Check(title && std::wstring(title) == (language == 2 ? L"备份文件夹已满" : language == 0 ?
            L"バックアップフォルダーがいっぱいです" : L"Backup folder is full"), "full warning caption follows UI language");
        fullWarningText = text ? text : L"";
        as::Tick(g_Block, GetTickCount64());
        Check(fullWarnings == 1, "nested idle work does not repeat the modal warning");
        return IDOK;
    }
    if (expectedWarning) { ++warnings; return IDOK; }
    std::fwprintf(stderr, L"FAIL unexpected message: %ls\n", text); std::exit(2);
}
int WINAPI MessageA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected message: %s\n", text); std::exit(2);
}
BOOL WINAPI NoBeep(UINT) { return TRUE; }
decltype(&ShowWindow) originalShow;
BOOL WINAPI Hide(HWND window, int) { return originalShow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
HMENU Help(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    for (int i = 0; i < GetMenuItemCount(bar); ++i) {
        const auto sub = GetSubMenu(bar, i);
        if (sub && GetMenuState(sub, 201, MF_BYCOMMAND) != UINT(-1)) return sub;
    }
    return nullptr;
}
void Configure(MMDApp* app, const as::Settings& value) {
    std::wstring error;
    Check(as::Configure(app, value, error), "persist autosave settings in isolated profile");
}
void SettingsCodecChecks() {
    const auto path = root / L"settings-codec.ini";
    for (bool enabled : {false, true}) for (bool keepPrevious : {false, true}) for (bool numbered : {false, true}) {
        as::Settings value; value.directory = crashDirectory.wstring();
        value.enabled = enabled; value.keepPrevious = keepPrevious; value.numberedManualVersions = numbered;
        Check(as::SaveSettings(path, value), "serialize independent periodic and previous-version preferences");
        const auto decoded = as::LoadSettings(path);
        Check(decoded.enabled == enabled && decoded.keepPrevious == keepPrevious && decoded.numberedManualVersions == numbered,
            "legacy override roundtrip preserves all eight periodic previous-version and numbered-save combinations");
    }
    { std::ofstream out(path); out << "[autosave]\nenabled=1\ndirectoryMode=executable\n"; }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    const auto old = as::LoadSettings(path);
    Check(old.enabled && !old.keepPrevious && !old.numberedManualVersions, "legacy settings without manual-version keys keep both disabled");
}
void CheckNoManualBackupVersions(const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring(); const auto at = name.find(L".pmm");
        Check(at == std::wstring::npos || at + 4 == name.size() || name[at + 4] < L'0' || name[at + 4] > L'9',
            "automatic backup directory contains no manual numbered or repeated-one generations or their sidecars");
    }
}
pmm_extension::Document Extension(const fs::path& path, bool manual = false) {
    int fd = -1;
    Check(!_wsopen_s(&fd, path.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYWR, 0), "read snapshot");
    pmm_extension::Document document; std::uint64_t size;
    const auto expected = manual && MMD_ORIGINAL_PROFILE ? pmm_extension::ReadResult::Absent :
        pmm_extension::ReadResult::Loaded;
    Check(pmm_extension::Read(fd, document, size) == expected,
        "backup has a valid extension; manual save follows the selected profile format");
    _close(fd); return document;
}
std::wstring Title(HWND window) { wchar_t text[512]{}; GetWindowTextW(window, text, 512); return text; }
#include "backup_dialog_checks.hpp"
int dialogAction = 0, effectProbe = -1;
INT_PTR CALLBACK Probe(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_INITDIALOG) return as::DialogProc(dialog, msg, wp, lp);
    as::DialogProc(dialog, msg, wp, lp);
    const auto* c = reinterpret_cast<as::DialogContext*>(lp);
    BackupDialogChecks(dialog, *c);
    if (!dialogAction && effectProbe < 0) CaptureBackupDialog(dialog, c->app->EnglishUI());
    Check(!IsWindowVisible(dialog), "settings dialog never becomes visible during test");
    Check(GetDlgItemInt(dialog, as::Interval, nullptr, FALSE) == c->value.intervalMinutes, "interval is displayed in minutes");
    Check((IsDlgButtonChecked(dialog, as::Enabled) == BST_CHECKED) == c->value.enabled, "enabled checkbox reflects saved setting");
    Check((IsDlgButtonChecked(dialog, as::KeepPrevious) == BST_CHECKED) == c->value.keepPrevious,
        "PMM1 checkbox reflects saved setting");
    const HWND numbered = GetDlgItem(dialog, as::NumberedManual);
    Check(numbered && (IsDlgButtonChecked(dialog, as::NumberedManual) == BST_CHECKED) == c->value.numberedManualVersions,
        "numbered manual-save checkbox exists and reflects its independent saved setting");
    RECT numberedBounds{}; GetClientRect(numbered, &numberedBounds);
    HDC numberedDc = GetDC(numbered); SIZE numberedSize{};
    const auto numberedFont = SelectObject(numberedDc, reinterpret_cast<HFONT>(SendMessageW(numbered, WM_GETFONT, 0, 0)));
    const auto numberedText = Title(numbered);
    GetTextExtentPoint32W(numberedDc, numberedText.c_str(), int(numberedText.size()), &numberedSize);
    SelectObject(numberedDc, numberedFont); ReleaseDC(numbered, numberedDc);
    Check(!numberedText.empty() && numberedSize.cx + 24 <= numberedBounds.right, "numbered-save caption fits in each real localized dialog");
    const auto keep = GetDlgItem(dialog, as::KeepPrevious);
    RECT bounds{}; GetClientRect(keep, &bounds);
    HDC labelDc = GetDC(keep); SIZE labelSize{};
    auto labelFont = SelectObject(labelDc, reinterpret_cast<HFONT>(SendMessageW(keep, WM_GETFONT, 0, 0)));
    const auto keepText = Title(keep);
    GetTextExtentPoint32W(labelDc, keepText.c_str(), static_cast<int>(keepText.size()), &labelSize);
    SelectObject(labelDc, labelFont); ReleaseDC(keep, labelDc);
    Check(labelSize.cx + 24 <= bounds.right, "PMM1 label fits in every language");
    Check(Title(GetDlgItem(dialog, as::Enabled)) == as::Text(c->app, L"启用定时自动备份", L"Enable automatic backups", L"定期バックアップを有効にする"),
        "checkbox controls periodic saving only");
    const HWND explanation = GetDlgItem(dialog, as::Explanation);
    const auto text = Title(explanation);
    if (effectProbe >= 0) {
        const bool enabled = effectProbe == 2;
        Check(text == as::Text(c->app, enabled ? L"当前 .emm 保存已开启。" : L"当前 .emm 保存未开启。",
            enabled ? L".emm saving is on." : L".emm saving is off.", enabled ? L".emm の同時保存：有効" : L".emm の同時保存：無効"),
            "real explanation distinguishes checked unchecked and missing MME menu state without loading MME");
    }
    RECT available{}; GetClientRect(explanation, &available);
    RECT measured = available; HDC dc = GetDC(explanation);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(explanation, WM_GETFONT, 0, 0));
    const auto oldFont = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont); ReleaseDC(explanation, dc);
    Check(measured.bottom <= available.bottom, "concise backup explanation fits in all languages");
    if (dialogAction == 0) {
        SetDlgItemInt(dialog, as::Interval, 22, FALSE);
        CheckDlgButton(dialog, as::Enabled, BST_UNCHECKED);
        CheckDlgButton(dialog, as::KeepPrevious, c->value.keepPrevious ? BST_UNCHECKED : BST_CHECKED);
        CheckDlgButton(dialog, as::NumberedManual, c->value.numberedManualVersions ? BST_UNCHECKED : BST_CHECKED);
        as::DialogProc(dialog, WM_COMMAND, IDCANCEL, 0);
    } else {
        if (dialogAction == 1) {
            SetDlgItemInt(dialog, as::Interval, 0, FALSE);
            expectedWarning = true; as::DialogProc(dialog, WM_COMMAND, IDOK, 0); expectedWarning = false;
            Check(as::Current().intervalMinutes != 0, "invalid zero interval is not committed");
        }
        SetDlgItemInt(dialog, as::Interval, 2, FALSE);
        SetDlgItemInt(dialog, as::Limit, 3, FALSE);
        CheckDlgButton(dialog, as::Overwrite, BST_CHECKED);
        const int combination = dialogAction >= 3 ? dialogAction - 3 : -1;
        CheckDlgButton(dialog, as::Enabled, (combination >= 0 ? (combination & 1) != 0 : dialogAction == 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, as::KeepPrevious, (combination < 0 || (combination & 2)) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, as::NumberedManual, (combination < 0 || (combination & 4)) ? BST_CHECKED : BST_UNCHECKED);
        as::DialogProc(dialog, WM_COMMAND, IDOK, 0);
    }
    return TRUE;
}
void DialogTests(MMDApp* app) {
    for (int language = 0; language < 3; ++language) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        RefreshMenuLanguage(app);
        const auto help = Help(app->MainWindow());
        Check(help != nullptr, "settings belongs to Help menu");
        wchar_t caption[80]{};
        GetMenuStringW(help, as::kSettingsCommand, caption, 80, MF_BYCOMMAND);
        Check(std::wstring(caption) == (language == 2 ? L"自动备份设置…" : language == 0 ? L"自動バックアップ設定…" : L"Automatic backup settings…"), "single settings item translated");
        Check(GetMenuState(help, 32812, MF_BYCOMMAND) == UINT(-1), "no separate toggle menu item");
        const auto count = GetMenuItemCount(help);
        for (int n = 0; n < 3; ++n) as::RefreshMenu(app);
        Check(count == GetMenuItemCount(help), "refresh never duplicates menu item");
        for (int action : {0, 3, 4, 5, 6, 7, 8, 9, 10, 1, 2}) {
            dialogAction = action;
            as::DialogContext c{app, as::Current()};
            const auto before = as::Current();
            const auto beforeFile = Bytes(root / L"autosave.ini");
            const auto result = UiDialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(as::kDialog),
                app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&c));
            Check(result == (dialogAction ? IDOK : IDCANCEL), "settings dialog commits or cancels correctly");
            const int combination = dialogAction >= 3 ? dialogAction - 3 : -1;
            Check(dialogAction ? as::Current().enabled == (combination >= 0 ? (combination & 1) != 0 : dialogAction == 1) &&
                as::Current().intervalMinutes == 2 && as::Current().maxBackups == 3 && as::Current().overwriteOldest &&
                as::Current().keepPrevious == (combination < 0 || (combination & 2) != 0) &&
                as::Current().numberedManualVersions == (combination < 0 || (combination & 4) != 0) :
                as::Current().enabled == before.enabled && as::Current().intervalMinutes == before.intervalMinutes &&
                as::Current().keepPrevious == before.keepPrevious && as::Current().numberedManualVersions == before.numberedManualVersions &&
                beforeFile == Bytes(root / L"autosave.ini"),
                "localized dialog commits all independent flag combinations while cancel preserves cached and persisted preferences");
            const auto reloaded = as::LoadSettings(root / L"autosave.ini");
            Check(reloaded.enabled == as::Current().enabled && reloaded.keepPrevious == as::Current().keepPrevious &&
                reloaded.numberedManualVersions == as::Current().numberedManualVersions, "dialog options survive actual INI reload after commit or cancellation");
        }
        for (effectProbe = 0; effectProbe < 3; ++effectProbe) {
            struct MenuSwap {
                HWND window; HMENU old, temporary;
                explicit MenuSwap(HWND hwnd) : window(hwnd), old(GetMenu(hwnd)), temporary(CreateMenu()) {
                    Check(temporary && SetMenu(window, temporary), "install only an owned temporary menu for MME status fixture");
                }
                ~MenuSwap() { Check(SetMenu(window, old) != FALSE, "restore original complete menu after MME status fixture"); DestroyMenu(temporary); }
            } menu(app->MainWindow());
            if (effectProbe) Check(AppendMenuW(menu.temporary, MF_STRING | (effectProbe == 2 ? MF_CHECKED : MF_UNCHECKED), 40022, L"owned effect-save state"),
                "create a checked or unchecked menu item without any MME DLL");
            Check((GetMenuState(menu.temporary,40022,MF_BYCOMMAND)==UINT(-1))==(effectProbe==0),"missing MME status case has no fake command");
            dialogAction = 0; as::DialogContext context{app,as::Current()}; const auto before = Bytes(root/L"autosave.ini");
            Check(UiDialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(as::kDialog),app->MainWindow(),Probe,
                reinterpret_cast<LPARAM>(&context))==IDCANCEL&&Bytes(root/L"autosave.ini")==before,
                "MME status inspection and cancellation do not change persistent settings");
        }
        effectProbe = -1;
    }
    app->EnglishUI() = 2;
    RefreshMenuLanguage(app);
}
unsigned RedMenuPixels(MMDApp* app) {
    RECT rect{}; GetWindowRect(app->MainWindow(), &rect);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    HDC dc = GetWindowDC(app->MainWindow()), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    PatBlt(memory, 0, 0, w, h, WHITENESS);
    as::PaintMenuWarning(app, memory);
    unsigned red = 0;
    auto* colors = static_cast<unsigned*>(pixels);
    for (int i = 0; i < w * h; ++i) {
        const unsigned c = colors[i];
        if (((c >> 16) & 255) > 120 && ((c >> 8) & 255) < 80 && (c & 255) < 80) ++red;
    }
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(app->MainWindow(), dc);
    return red;
}
#include "installation_settings_checks.inc"
#include "autosave_warning_checks.hpp"
#include "autosave_empty_checks.hpp"
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc >= 2, "disposable artifact folder required"); root = fs::absolute(argv[1]);
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "find actual test EXE");
    crashDirectory = fs::path(executable).parent_path() / L"data" / L"backups";
    const std::wstring mode = argc == 3 ? argv[2] : L"";
    if(mode.find(L"--settings-")==0){SettingsLocationChecks(executable,mode);return 0;}
    const bool fullWarningMode = mode.find(L"--full-") == 0;
    Check(fullWarningMode || !fs::exists(crashDirectory), "test requires a fresh isolated EXE directory");
    fs::create_directories(root);
    const auto config = root / L"autosave.ini";
    SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", config.c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"profile").c_str());
    as::Result allocationResult = as::Result::Busy;
    FailNextAllocation([&] { allocationResult = as::BackupNow(nullptr, true); },
        "crash backup catches allocation failure during first settings load");
    Check(allocationResult == as::Result::Failed, "failed initial settings load reports failed backup");
    FailNextAllocation([&] { as::Tick(nullptr, GetTickCount64()); },
        "idle processing catches allocation failure during first settings load");
    const bool defaultCrash = mode == L"--crash-default";
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "install background test guards");
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&MessageW), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&MessageA), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBeep), reinterpret_cast<void*>(&NoBeep), nullptr);
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), reinterpret_cast<void*>(&Hide), reinterpret_cast<void**>(&originalShow)) == MH_OK, "hide all test windows");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable background guards");
    auto hook = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "prevent activation");
    auto* app = new MMDApp; g_Block = app; app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production app");
    app->EnglishUI() = 2; RefreshMenuLanguage(app);
    SetWindowPos(app->MainWindow(), nullptr, 0, 0, 1600, 800, SWP_NOACTIVATE | SWP_NOZORDER);
    as::Initialize(app);
    if (mode == L"--empty-project") {
        CheckEmptyProjectBackup(app);
        as::Shutdown(); ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS empty-project autosave checks=%u\n", checks);
        return 0;
    }
    if (fullWarningMode) {
        CheckFullWarnings(app, config, mode);
        Check(!IsWindowVisible(app->MainWindow()), "warning test stays hidden");
        as::Shutdown(); ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS backup full warning checks=%u\n", checks);
        return 0;
    }
    Check(!as::Current().enabled && !as::Current().keepPrevious && !as::Current().numberedManualVersions && as::Current().intervalMinutes == 1,
        "periodic and previous-version saving both default off with one minute interval");
    SettingsCodecChecks();
    BackupDirectoryChecks(app, config);
#if !MMD_ORIGINAL_PROFILE
    // The original-compatibility profile keeps the original antialias surface;
    // central settings are a full-build behavior.
    std::wstring aaError;
    SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG", nullptr);
    Check(antialias::Select(app, antialias::Algorithm::MmdOriginal, aaError),
        "select actual antialias preference through production settings path");
    std::string aaBody;
    Check(system_settings::ReadSection("antialias", aaBody) == system_settings::Result::Ok &&
        fs::is_regular_file(fs::path(executable).parent_path()/L"data"/L"settings.json") &&
        !fs::exists(fs::path(executable).parent_path()/L"data"/L"antialias.ini") &&
        settings_json::UInt(settings_json::Object(aaBody), "algorithm", unsigned(antialias::Algorithm::Count)) ==
            unsigned(antialias::Algorithm::MmdOriginal),
        "actual antialias selection persists in central settings without recreating separate INI");
#endif
    as::Settings value; value.enabled = true; value.directory = (root / L"自动备份").wstring(); value.maxBackups = 2;
    if (!defaultCrash) Configure(app, value);
    else Check(!fs::exists(config) && as::Current().directory == (fs::path(app->ExeDir()) / L"data" / L"backups").wstring(),
        "first-run crash recovery uses backups beside the executable with no saved configuration");
    const auto manual = root / L"工程原稿.pmm";
    LoadModelFile(app, (root / L"fixture" / L"fixture.pmd").c_str(), false);
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneCount == 1, "load real model fixture");
    wcscpy_s(app->EnvFileName(), 256, manual.c_str());
    app->CurrentFrame() = 11; app->SceneModified() = 1;
    SaveSceneFile(app);
    Check(app->SceneModified() == 0 && fs::exists(manual), "manual save remains user-triggered");
    SeedPluginData(app, manual);
    const auto original = Bytes(manual);
    app->CurrentFrame() = 222; app->CameraDistance() = -77; app->SceneModified() = 1;
    mdl::Mdl(app->ModelSlot(0))->boneTable[0].trans[0] = 3.5f;
    mdl::Mdl(app->ModelSlot(0))->boneKeys[0].position[0] = 2.25f;
    app->PmmProjectSettings().sortingPrecisionEdited = true;
    const auto title = Title(app->MainWindow());
    const auto layout = app->WindowLayoutReady();
    crashChild = mode == L"--crash" || mode == L"--crash-disabled" || defaultCrash;
    if (crashChild) {
        if (mode == L"--crash-disabled") {
            value.enabled = false; Configure(app, value);
            Check(!as::LoadSettings(config).enabled, "periodic saving is explicitly disabled in persisted settings");
        }
        std::ofstream(root / L"original-before-crash.bin", std::ios::binary) << original;
        runtime_log::Initialize(); as::Initialize(app);
        RaiseException(0xE1234567, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 3;
    }
    value.keepPrevious = true; value.numberedManualVersions = true; Configure(app, value);
    Check(as::BackupNow(app) == as::Result::Saved, "first real PMM backup");
    auto backups = as::Backups(value.directory);
    Check(backups.size() == 1 && backups[0].filename().wstring().find(L"工程原稿_autosave_") == 0, "original name and timestamp in backup filename");
    Check(Bytes(manual) == original && std::wstring(app->EnvFileName()) == manual.wstring(), "backup never overwrites or retargets manual project");
    Check(app->SceneModified() == 1 && app->PmmProjectSettings().sortingPrecisionEdited &&
        app->WindowLayoutReady() == layout && Title(app->MainWindow()) == title, "backup preserves dirty flags, layout and title");
    Check(!pmm_extension::Find(app->PmmProjectSettings().document, as::kOwnershipKey), "backup marker does not pollute live project");
    Check(pmm_extension::Find(Extension(backups[0]), as::kOwnershipKey) != nullptr, "backup is verifiably owned by MMS");
    CheckPluginData(backups[0]);
    CheckManualPluginData();
    const auto first = backups[0];
    Check(!SaveSceneCopy(app, first.c_str()), "copy writer refuses existing destination");
    Check(as::BackupNow(app) == as::Result::Saved && as::FolderFull(), "reaching capacity sets full warning immediately");
    Check(RedMenuPixels(app) > 0, "warning paints red pixels in menu bar without showing a window");
    Check(as::BackupNow(app) == as::Result::Full && as::Backups(value.directory).size() == 2, "default capacity policy stops saving");
    const auto sentinel = fs::path(value.directory) / L"manual_autosave_20261012_000000_000.pmm";
    std::ofstream(sentinel) << "ordinary user file";
    const fs::path sentinelSide(sentinel.native() + L".mms-plugins.json");
    std::ofstream(sentinelSide) << "ordinary user sidecar";
    Check(as::Backups(value.directory).size() == 2, "filename imitation is not an owned backup");
    value.overwriteOldest = true; Configure(app, value);
    Check(as::BackupNow(app) == as::Result::Saved && as::Backups(value.directory).size() == 2, "opt-in rotation maintains capacity");
    Check(!fs::exists(first) && Bytes(sentinel) == "ordinary user file", "rotation deletes oldest backup and protects unrelated files");
    Check(!fs::exists(first.native()+L".mms-plugins.json"),"rotation removes only the matching plugin sidecar");
    Check(Bytes(sentinelSide) == "ordinary user sidecar", "rotation preserves unrelated manual sidecars");
    for (const auto& retained : as::Backups(value.directory)) CheckPluginData(retained);
    CheckNoManualBackupVersions(value.directory);
    CheckManualPluginData();
    const auto beforeFailure = as::Backups(value.directory);
    const auto sidecarCount = [&]() {
        unsigned count = 0;
        for (const auto& entry : fs::directory_iterator(value.directory))
            if (entry.path().filename().wstring().find(L".mms-plugins.json") != std::wstring::npos) ++count;
        return count;
    };
    const auto sidecarsBeforeFailure = sidecarCount();
    const auto protectedBytes = Bytes(beforeFailure.front());
    HANDLE protectedBackup = CreateFileW(beforeFailure.front().c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(protectedBackup != INVALID_HANDLE_VALUE, "prevent deletion of the oldest backup during rotation");
    const auto failedRotation = as::BackupNow(app);
    CloseHandle(protectedBackup);
    Check(failedRotation == as::Result::Full && as::Backups(value.directory) == beforeFailure &&
        Bytes(beforeFailure.front()) == protectedBytes && sidecarCount() == sidecarsBeforeFailure,
        "failed rotation preserves old backups and removes both files of the new pair");
    for (const auto& retained : beforeFailure) CheckPluginData(retained);
    std::wstring longName(220, L'a'); longName += L".pmm";
    wcscpy_s(app->EnvFileName(), 256, longName.c_str());
    Check(as::BackupNow(app) == as::Result::Failed && as::Backups(value.directory) == beforeFailure, "failed new save never removes previous backups");
    wcscpy_s(app->EnvFileName(), 256, manual.c_str());
    const auto lockPath = fs::path(value.directory) / L".mms-autosave.lock";
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    Check(lock != INVALID_HANDLE_VALUE && as::BackupNow(app) == as::Result::Busy, "another instance's folder lock prevents concurrent rotation");
    CloseHandle(lock);
    { ScopedSceneMutation mutation(*app); Check(as::BackupNow(app) == as::Result::Busy, "partial project mutations defer backup"); }
    app->PlaybackActive() = 1; Check(as::BackupNow(app) == as::Result::Busy, "playback defers periodic backup"); app->PlaybackActive() = 0;
    value.directory = (root / L"timer").wstring(); value.maxBackups = 1; value.overwriteOldest = false; Configure(app, value);
    const auto now = GetTickCount64();
    as::Tick(app, now + 59900); Check(as::Backups(value.directory).empty(), "one-minute timer does not save early");
    expectedFullWarning = true;
    as::Tick(app, now + 60001);
    Check(SceneSaveInProgress(), "timer schedules a background snapshot");
    background_save::Wait(app);
    as::Tick(app, now + 60002);
    expectedFullWarning = false;
    Check(as::Backups(value.directory).size() == 1, "one-minute timer writes a backup when due");
    CheckNoManualBackupVersions(value.directory);
    Check(!fs::exists(manual.wstring()+L"1")&&!fs::exists(manual.wstring()+L"2"),
        "periodic backups never rotate the manual project even when both manual-version switches are enabled");
    Check(fullWarnings == 1 && as::FolderFull(), "timer warns immediately when its new backup fills the folder");
    as::TryCrashBackup(EXCEPTION_STACK_OVERFLOW); as::TryCrashBackup(0xC0000374);
    std::thread worker([] { as::TryCrashBackup(EXCEPTION_ACCESS_VIOLATION); }); worker.join();
    Check(as::Backups(value.directory).size() == 1 && as::Backups(crashDirectory).empty(), "unsafe heap/stack and foreign-thread faults skip crash serialization");
    as::TryCrashBackup(EXCEPTION_ACCESS_VIOLATION);
    backups = as::Backups(crashDirectory);
    Check(backups.size() == 1 && backups.back().filename().wstring().find(L"_crash") != std::wstring::npos &&
        as::Backups(value.directory).size() == 1, "safe crash path uses EXE backups instead of the periodic folder");
    CheckPluginData(backups.back());
    CheckNoManualBackupVersions(crashDirectory);
    value.keepPrevious = false; value.numberedManualVersions = false; Configure(app, value);
    wcscpy_s(app->EnvFileName(), 256, backups.back().c_str());
    LoadSceneFile();
    Check(app->CurrentFrame() == 222 && app->CameraDistance() == -77, "production loader restores current frame and camera from crash backup");
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneKeys[0].position[0] == 2.25f &&
        mdl::Mdl(app->ModelSlot(0))->boneTable[0].trans[0] == 3.5f, "production loader restores keyframes and unregistered model pose");
    Check(Bytes(manual) == original, "roundtrip never touches the manual original");
    CheckRestoredPluginData(app);
    // An explicit Save of a recovered copy makes it a user-controlled file.
    app->SceneModified() = 1; SaveSceneFile(app);
    Check(!pmm_extension::Find(Extension(backups.back(), true), as::kOwnershipKey), "manual save removes rotation ownership");
    Check(as::Backups(crashDirectory).empty(), "manually saved recovered file is excluded from automatic rotation");
    const auto recoveredManual = backups.back();
    const auto recoveredManualBytes = Bytes(recoveredManual);
    const fs::path recoveredSide(recoveredManual.native() + L".mms-plugins.json");
    const auto recoveredSideBytes = Bytes(recoveredSide);
    CheckPluginData(recoveredManual);
    NamingTests(app);
    DialogTests(app);
    Check(warnings == 3, "invalid settings rejected in each language");
    value = as::Current(); value.enabled = false; Configure(app, value);
    Check(!(GetMenuState(Help(app->MainWindow()), as::kSettingsCommand, MF_BYCOMMAND) & MF_CHECKED), "unchecked settings item when disabled");
    Check(!as::FolderFull() && !RedMenuPixels(app), "folder below limit shows no warning");
    Check(as::BackupNow(app) == as::Result::Disabled, "disabled setting prevents periodic backups only");
    const auto beforeDisabled = as::Backups(value.directory).size();
    as::Tick(app, GetTickCount64() + 180000);
    Check(as::Backups(value.directory).size() == beforeDisabled, "timer never saves when periodic checkbox is off");
    as::TryCrashBackup(EXCEPTION_ACCESS_VIOLATION);
    Check(as::Backups(crashDirectory).size() == 1 && as::Backups(value.directory).size() == beforeDisabled,
        "crash recovery stays armed in EXE backups after disabling periodic saving");
    const auto disabledCrash = as::Backups(crashDirectory).front();
    CheckPluginData(disabledCrash);
    Check(Bytes(manual) == original, "always-on crash copy preserves manual original");
    value.maxBackups = static_cast<unsigned>(beforeDisabled); value.overwriteOldest = false; Configure(app, value);
    Check(as::FolderFull() && RedMenuPixels(app), "full-folder warning remains visible with periodic saving off");
    value.maxBackups = 1; Configure(app, value);
    Check(as::BackupNow(app, true) == as::Result::Full, "crash recovery respects its own folder's no-overwrite capacity policy");
    value.overwriteOldest = true; Configure(app, value);
    Check(as::BackupNow(app, true) == as::Result::Saved && as::Backups(crashDirectory).size() == value.maxBackups,
        "crash recovery rotates old backups only when authorized");
    Check(!fs::exists(disabledCrash) && !fs::exists(disabledCrash.native() + L".mms-plugins.json"),
        "crash rotation removes the old PMM and its plugin sidecar together");
    CheckPluginData(as::Backups(crashDirectory).front());
    Check(Bytes(recoveredManual) == recoveredManualBytes && Bytes(recoveredSide) == recoveredSideBytes,
        "crash rotation preserves the manually saved recovery pair");
    CheckManualPluginData();
    value.maxBackups = 3; Configure(app, value);
    const auto persisted = as::LoadSettings(config);
    Check(!persisted.enabled && persisted.keepPrevious && persisted.numberedManualVersions && persisted.intervalMinutes == 2 && persisted.maxBackups == 3 && persisted.overwriteOldest,
        "all settings survive profile reload");
    auto invalid = value; invalid.intervalMinutes = 0; std::wstring error;
    Check(!as::Configure(app, invalid, error), "zero minutes cannot be saved");
    invalid = value; invalid.maxBackups = 0; Check(!as::Configure(app, invalid, error), "zero capacity cannot be saved");
    wcscpy_s(app->EnvFileName(),256,manual.c_str());LoadSceneFile();
    value.directory=(root/L"custom-names").wstring();value.backupPattern=L"自定义_[/NAME]_[/YYYY]_[/N]";
    value.maxBackups=1;value.overwriteOldest=true;value.enabled=true;Configure(app,value);
    Check(as::BackupNow(app)==as::Result::Saved,"custom naming writes a real backup");
    auto custom=as::Backups(value.directory);
    Check(custom.size()==1 && custom[0].filename().wstring().find(L"自定义_")==0 &&
        custom[0].filename().wstring().find(L"_autosave_")==std::wstring::npos,"ownership recognizes custom names without the old autosave substring");
    const auto foreign=fs::path(value.directory)/L"自定义_user_001.pmm";
    std::ofstream(foreign)<<"user file";
    Check(as::QueueBackup(app),"custom named periodic save queues successfully");background_save::Wait(app);
    Check(as::Backups(value.directory).size()==1 && !fs::exists(custom[0]) && Bytes(foreign)=="user file",
        "custom named rotation only removes owned backups");
    const auto recovered=as::Backups(value.directory).front();
    wcscpy_s(app->EnvFileName(),256,recovered.c_str());LoadSceneFile();
    Check(as::BackupNow(app)==as::Result::Full,"active recovery file stays protected when it is the only backup");
    value.maxBackups=2;Configure(app,value);
    Check(as::BackupNow(app)==as::Result::Saved,"recovered custom backup can be backed up again");
    Check(as::Backups(value.directory).front().filename().wstring().find(L"自定义_自定义_")==std::wstring::npos,
        "recovery retains original stem without nesting the custom pattern");
    Check(!IsWindowVisible(app->MainWindow()), "production test stayed hidden throughout");
    as::Shutdown(); ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
    std::printf("PASS autosave checks=%u\n", checks);
}
