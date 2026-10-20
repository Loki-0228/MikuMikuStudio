// Explicit hidden production test: no windows are shown or activated.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/local_data_path.hpp"
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
int WINAPI MessageW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    if (crashChild && title && wcsstr(title, L"Fatal error")) {
        const auto backups = as::Backups(crashDirectory);
        Check(backups.size() == 1 && backups[0].filename().wstring().find(L"_crash.pmm") != std::wstring::npos,
            "production unhandled exception filter saved a crash backup before reporting");
        Check(Bytes(root / L"工程原稿.pmm") == Bytes(root / L"original-before-crash.bin"), "crash preserved manual project bytes");
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
int dialogAction = 0;
INT_PTR CALLBACK Probe(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_INITDIALOG) return as::DialogProc(dialog, msg, wp, lp);
    as::DialogProc(dialog, msg, wp, lp);
    const auto* c = reinterpret_cast<as::DialogContext*>(lp);
    BackupDialogChecks(dialog,*c);
    if (!dialogAction) CaptureBackupDialog(dialog,c->app->EnglishUI());
    Check(!IsWindowVisible(dialog), "settings dialog never becomes visible during test");
    Check(GetDlgItemInt(dialog, as::Interval, nullptr, FALSE) == c->value.intervalMinutes, "interval is displayed in minutes");
    Check((IsDlgButtonChecked(dialog, as::Enabled) == BST_CHECKED) == c->value.enabled, "enabled checkbox reflects saved setting");
    Check((IsDlgButtonChecked(dialog, as::KeepPrevious) == BST_CHECKED) == c->value.keepPrevious,
        "PMM1 checkbox reflects saved setting");
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
        as::DialogProc(dialog, WM_COMMAND, IDCANCEL, 0);
    } else {
        SetDlgItemInt(dialog, as::Interval, 0, FALSE);
        expectedWarning = true; as::DialogProc(dialog, WM_COMMAND, IDOK, 0); expectedWarning = false;
        Check(as::Current().intervalMinutes != 0, "invalid zero interval is not committed");
        SetDlgItemInt(dialog, as::Interval, 2, FALSE);
        SetDlgItemInt(dialog, as::Limit, 3, FALSE);
        CheckDlgButton(dialog, as::Overwrite, BST_CHECKED);
        CheckDlgButton(dialog, as::Enabled, BST_CHECKED);
        CheckDlgButton(dialog, as::KeepPrevious, BST_CHECKED);
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
        for (dialogAction = 0; dialogAction < 2; ++dialogAction) {
            as::DialogContext c{app, as::Current()};
            const auto before = as::Current();
            const auto result = UiDialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(as::kDialog),
                app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&c));
            Check(result == (dialogAction ? IDOK : IDCANCEL), "settings dialog commits or cancels correctly");
            Check(dialogAction ? as::Current().intervalMinutes == 2 && as::Current().maxBackups == 3 && as::Current().overwriteOldest && as::Current().keepPrevious :
                as::Current().enabled == before.enabled && as::Current().intervalMinutes == before.intervalMinutes && as::Current().keepPrevious == before.keepPrevious,
                "canceled edits leave settings unchanged");
        }
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
    crashDirectory = fs::path(executable).parent_path() / L"backups";
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
    Check(!as::Current().enabled && as::Current().intervalMinutes == 1, "default off with one minute interval");
    BackupDirectoryChecks(app,config);
    std::wstring aaError;
    Check(antialias::Select(app, antialias::Algorithm::MmdOriginal, aaError) &&
        fs::is_regular_file(fs::path(executable).parent_path()/L"data"/L"antialias.ini"),
        "actual antialias selection persists in this EXE Data folder");
    as::Settings value; value.enabled = true; value.directory = (root / L"自动备份").wstring(); value.maxBackups = 2;
    if (!defaultCrash) Configure(app, value);
    else Check(!fs::exists(config) && as::Current().directory == (fs::path(app->ExeDir()) / L"backups").wstring(),
        "first-run crash recovery uses backups beside the executable with no saved configuration");
    const auto manual = root / L"工程原稿.pmm";
    LoadModelFile(app, (root / L"fixture" / L"fixture.pmd").c_str(), false);
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneCount == 1, "load real model fixture");
    wcscpy_s(app->EnvFileName(), 256, manual.c_str());
    app->CurrentFrame() = 11; app->SceneModified() = 1;
    SaveSceneFile(app);
    Check(app->SceneModified() == 0 && fs::exists(manual), "manual save remains user-triggered");
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
    Check(as::BackupNow(app) == as::Result::Saved, "first real PMM backup");
    auto backups = as::Backups(value.directory);
    Check(backups.size() == 1 && backups[0].filename().wstring().find(L"工程原稿_autosave_") == 0, "original name and timestamp in backup filename");
    Check(Bytes(manual) == original && std::wstring(app->EnvFileName()) == manual.wstring(), "backup never overwrites or retargets manual project");
    Check(app->SceneModified() == 1 && app->PmmProjectSettings().sortingPrecisionEdited &&
        app->WindowLayoutReady() == layout && Title(app->MainWindow()) == title, "backup preserves dirty flags, layout and title");
    Check(!pmm_extension::Find(app->PmmProjectSettings().document, as::kOwnershipKey), "backup marker does not pollute live project");
    Check(pmm_extension::Find(Extension(backups[0]), as::kOwnershipKey) != nullptr, "backup is verifiably owned by MMS");
    const auto first = backups[0];
    Check(!SaveSceneCopy(app, first.c_str()), "copy writer refuses existing destination");
    Check(as::BackupNow(app) == as::Result::Saved && as::FolderFull(), "reaching capacity sets full warning immediately");
    Check(RedMenuPixels(app) > 0, "warning paints red pixels in menu bar without showing a window");
    Check(as::BackupNow(app) == as::Result::Full && as::Backups(value.directory).size() == 2, "default capacity policy stops saving");
    const auto sentinel = fs::path(value.directory) / L"manual_autosave_20261012_000000_000.pmm";
    std::ofstream(sentinel) << "ordinary user file";
    Check(as::Backups(value.directory).size() == 2, "filename imitation is not an owned backup");
    value.overwriteOldest = true; Configure(app, value);
    Check(as::BackupNow(app) == as::Result::Saved && as::Backups(value.directory).size() == 2, "opt-in rotation maintains capacity");
    Check(!fs::exists(first) && Bytes(sentinel) == "ordinary user file", "rotation deletes oldest backup and protects unrelated files");
    const auto beforeFailure = as::Backups(value.directory);
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
    Check(fullWarnings == 1 && as::FolderFull(), "timer warns immediately when its new backup fills the folder");
    as::TryCrashBackup(EXCEPTION_STACK_OVERFLOW); as::TryCrashBackup(0xC0000374);
    std::thread worker([] { as::TryCrashBackup(EXCEPTION_ACCESS_VIOLATION); }); worker.join();
    Check(as::Backups(value.directory).size() == 1 && as::Backups(crashDirectory).empty(), "unsafe heap/stack and foreign-thread faults skip crash serialization");
    as::TryCrashBackup(EXCEPTION_ACCESS_VIOLATION);
    backups = as::Backups(crashDirectory);
    Check(backups.size() == 1 && backups.back().filename().wstring().find(L"_crash") != std::wstring::npos &&
        as::Backups(value.directory).size() == 1, "safe crash path uses EXE backups instead of the periodic folder");
    wcscpy_s(app->EnvFileName(), 256, backups.back().c_str());
    LoadSceneFile();
    Check(app->CurrentFrame() == 222 && app->CameraDistance() == -77, "production loader restores current frame and camera from crash backup");
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneKeys[0].position[0] == 2.25f &&
        mdl::Mdl(app->ModelSlot(0))->boneTable[0].trans[0] == 3.5f, "production loader restores keyframes and unregistered model pose");
    Check(Bytes(manual) == original, "roundtrip never touches the manual original");
    // An explicit Save of a recovered copy makes it a user-controlled file.
    app->SceneModified() = 1; SaveSceneFile(app);
    Check(!pmm_extension::Find(Extension(backups.back(), true), as::kOwnershipKey), "manual save removes rotation ownership");
    Check(as::Backups(crashDirectory).empty(), "manually saved recovered file is excluded from automatic rotation");
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
    Check(Bytes(manual) == original, "always-on crash copy preserves manual original");
    value.maxBackups = static_cast<unsigned>(beforeDisabled); value.overwriteOldest = false; Configure(app, value);
    Check(as::FolderFull() && RedMenuPixels(app), "full-folder warning remains visible with periodic saving off");
    value.maxBackups = 1; Configure(app, value);
    Check(as::BackupNow(app, true) == as::Result::Full, "crash recovery respects its own folder's no-overwrite capacity policy");
    value.overwriteOldest = true; Configure(app, value);
    Check(as::BackupNow(app, true) == as::Result::Saved && as::Backups(crashDirectory).size() == value.maxBackups,
        "crash recovery rotates old backups only when authorized");
    value.maxBackups = 3; Configure(app, value);
    const auto persisted = as::LoadSettings(config);
    Check(!persisted.enabled && persisted.intervalMinutes == 2 && persisted.maxBackups == 3 && persisted.overwriteOldest,
        "all settings survive profile reload");
    auto invalid = value; invalid.intervalMinutes = 0; std::wstring error;
    Check(!as::Configure(app, invalid, error), "zero minutes cannot be saved");
    invalid = value; invalid.maxBackups = 0; Check(!as::Configure(app, invalid, error), "zero capacity cannot be saved");
    Check(!IsWindowVisible(app->MainWindow()), "production test stayed hidden throughout");
    as::Shutdown(); ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
    std::printf("PASS autosave checks=%u\n", checks);
}
