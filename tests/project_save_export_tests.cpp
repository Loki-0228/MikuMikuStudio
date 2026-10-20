// Explicit hidden integration test: real Save/Save As and both export dialogs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "../src/window/image_sequence_dialog.hpp"
#include <MinHook.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace seq = mikumikustudio::image_sequence;
namespace mikumikustudio { INT_PTR __stdcall AviOutDlgProc(HWND, UINT, WPARAM, LPARAM); }
namespace {
unsigned checks = 0, picks = 0, errors = 0;
bool cancelPick = false, expectError = false, sequenceDialog = false;
fs::path root, history, chosen;
HWND expectedOwner = nullptr;
void Check(bool ok, const char* reason) {
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", reason); std::exit(1); }
}
void FinishWrites(MMDApp* app) {
    const auto deadline = GetTickCount64() + 15000;
    while (SceneSaveInProgress() && GetTickCount64() < deadline) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(1);
    }
    Check(!SceneSaveInProgress(), "background save completes through window messages");
}
void SaveCommand(MMDApp* app, unsigned command) {
    SendMessageW(app->MainWindow(), WM_COMMAND, command, 0);
    FinishWrites(app);
}
int WINAPI Message(HWND, LPCWSTR text, LPCWSTR, UINT) {
    Check(expectError, "only expected save failures display a message");
    ++errors;
    std::wprintf(L"Expected save failure: %ls\n", text);
    return IDOK;
}
BOOL WINAPI NoBeep(UINT) { return TRUE; }
BOOL Pick(LPOPENFILENAMEW ofn) {
    ++picks;
    Check(ofn->hwndOwner == expectedOwner, "Save As belongs to the active dialog");
    if (cancelPick) return FALSE;
    Check(wcscpy_s(ofn->lpstrFile, ofn->nMaxFile, chosen.c_str()) == 0, "choose isolated PMM path");
    return TRUE;
}
decltype(&ShowWindow) originalShow = nullptr;
BOOL WINAPI Hide(HWND window, int) { return originalShow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
std::vector<std::wstring> Recent() {
    std::ifstream input(history, std::ios::binary);
    std::vector<std::wstring> paths; std::wstring path; wchar_t c;
    while (input.read(reinterpret_cast<char*>(&c), sizeof(c))) {
        if (c == L'\n') { if (!path.empty()) paths.push_back(path); path.clear(); }
        else if (c != L'\r' && c != 0xfeff) path += c;
    }
    return paths;
}
std::wstring Text(HWND window) {
    wchar_t text[512]{}; GetWindowTextW(window, text, 512); return text;
}
bool Shown(HWND window) { return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0; }
int Height(HWND window) { RECT r{}; GetWindowRect(window, &r); return r.bottom-r.top; }
void Capture(HWND dialog, const fs::path& path) {
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetWindowDC(dialog), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto old = SelectObject(memory, bitmap);
    if (dialog == g_Block->MainWindow()) {
        PatBlt(memory, 0, 0, width, height, BLACKNESS);
        HandlePaletteChanged2(memory);
    } else SendMessageW(dialog, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    stream.write(static_cast<const char*>(pixels), width * height * 4);
    Check(bool(stream), "capture dialog without activating it");
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(dialog, dc);
}
void SaveTests(MMDApp* app) {
    expectedOwner = app->MainWindow();
    const auto first = root / L"新建工程.pmm", second = root / L"另存为.pmm";
    app->EnvFileName()[0] = 0; app->SceneModified() = 1;
    chosen = first;
    SaveCommand(app, 207);
    Check(fs::is_regular_file(first) && !app->SceneModified(), "Save creates the unnamed project");
    Check(Recent() == std::vector<std::wstring>{first.wstring()}, "first Save enters recent projects");
    chosen = second; app->SceneModified() = 1;
    SaveCommand(app, 208);
    Check(fs::is_regular_file(second) && std::wstring(app->EnvFileName()) == second.wstring(), "Save As retargets to a saved project");
    Check(Recent() == std::vector<std::wstring>{second.wstring(), first.wstring()}, "Save As enters history first");
    chosen = first;
    SaveCommand(app, 208);
    SaveCommand(app, 207);
    const auto savedHistory = Recent();
    Check(savedHistory == std::vector<std::wstring>{first.wstring(), second.wstring()}, "repeated Save deduplicates history");
    Check(SaveSceneCopy(app, (root / L"自动备份.pmm").c_str()) && Recent() == savedHistory,
        "automatic copies never enter recent projects");
    cancelPick = true;
    SaveCommand(app, 208);
    cancelPick = false;
    Check(Recent() == savedHistory && std::wstring(app->EnvFileName()) == first.wstring(), "cancel preserves history and current project");
    chosen = root / L"missing" / L"failure.pmm"; expectError = true;
    const auto before = errors;
    SaveCommand(app, 208);
    expectError = false;
    Check(errors == before + 1 && Recent() == savedHistory && std::wstring(app->EnvFileName()) == first.wstring(),
        "failed Save As cannot retarget the project or enter history");
}
std::vector<char> Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
#include "pmm_versions_checks.hpp"
WNDPROC realMain = nullptr;
unsigned deferredCloses = 0;
LRESULT CALLBACK WatchClose(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE && !SceneSaveInProgress()) { ++deferredCloses; return 0; }
    return CallWindowProcW(realMain, window, msg, wp, lp);
}
void AsyncTests(MMDApp* app) {
    app->EnglishUI() = 2;
    const auto baseline = root / L"snapshot-baseline.pmm";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, baseline.c_str());
    app->SceneModified() = 1;
    Check(SaveSceneFile(app), "write synchronous format baseline");
    const auto expected = Bytes(baseline);
    const auto snapshotPath = root / L"snapshot-background.pmm";
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const DWORD mainThread = GetCurrentThreadId();
    std::atomic<DWORD> writer{0};
    background_save::Options options;
    options.beforeWrite = [&] {
        writer.store(GetCurrentThreadId()); SetEvent(entered);
        return WaitForSingleObject(release, 10000) == WAIT_OBJECT_0;
    };
    app->SceneModified() = 1;
    Check(background_save::Request(app, snapshotPath.c_str(), options), "queue immutable snapshot");
    Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0 && writer.load() != mainThread,
        "writer executes on a separate thread");
    Check(!fs::exists(snapshotPath), "blocked worker has not published the destination");
    // Rendering and UI messages are still serviced while the writer is stalled.
    for (int language : {0, 1, 2}) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        Capture(app->MainWindow(), root / (L"saving-" + std::to_wstring(language) + L".bmp"));
        Check(*background_save::StatusText(app), "saving status is localized and persistent");
    }
    Check(!background_save::Request(app, (root / L"must-not-write.pmm").c_str()), "second save is rejected");
    const auto beforePicks = picks;
    Check(!SaveSceneAs(app, app->MainWindow()) && picks == beforePicks, "busy Save As does not open a picker");
    Check(!SaveSceneCopy(app, (root / L"must-not-copy.pmm").c_str()), "synchronous copy shares the save gate");
    const int oldFov = app->CameraFov();
    app->CameraFov() = oldFov + 5; app->SceneModified() = 1;
    SetEvent(release); FinishWrites(app);
    Check(Bytes(snapshotPath) == expected, "worker saved the original snapshot byte for byte after live edits");
    Check(app->SceneModified() && app->CameraFov() == oldFov + 5,
        "edits made during save remain dirty and visible");
    Check(std::wstring(app->EnvFileName()) == snapshotPath.wstring(), "successful Save As commits the new path");
    app->CameraFov() = oldFov; app->SceneModified() = 1;
    Check(background_save::Request(app, snapshotPath.c_str()), "next save accepted after completion");
    FinishWrites(app);
    Check(!app->SceneModified(), "unchanged completed snapshot clears dirty state");
    const auto previousBytes = Bytes(snapshotPath);
    HANDLE locked = CreateFileW(snapshotPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock existing destination against replacement");
    app->SceneModified() = 1; expectError = true;
    Check(background_save::Request(app, snapshotPath.c_str()), "capture a save whose publication will fail");
    FinishWrites(app); expectError = false; CloseHandle(locked);
    Check(Bytes(snapshotPath) == previousBytes && app->SceneModified(), "failed atomic replacement preserves old file and unsaved edits");

    ResetEvent(entered); ResetEvent(release);
    const auto oldScenePath = root / L"old-scene.pmm";
    Check(background_save::Request(app, oldScenePath.c_str(), options), "capture scene before replacing it");
    Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "old scene writer reached gate");
    app->AdvanceSceneGeneration(); app->EnvFileName()[0] = 0; app->SceneModified() = 1;
    SetEvent(release); FinishWrites(app);
    Check(fs::is_regular_file(oldScenePath) && !app->EnvFileName()[0] && app->SceneModified(),
        "old-scene completion cannot retarget or clean a replacement scene");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, snapshotPath.c_str());

    // The UI can request close, but it cannot destroy the application while
    // the disk writer owns a snapshot. Failure must not resume that close.
    realMain = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(app->MainWindow(), GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(WatchClose)));
    ResetEvent(entered); ResetEvent(release);
    Check(background_save::Request(app, snapshotPath.c_str(), options), "start pending-close save");
    Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "pending-close worker reached gate");
    SendMessageW(app->MainWindow(), WM_CLOSE, 0, 0);
    Check(!deferredCloses && IsWindow(app->MainWindow()), "close waits for worker");
    SetEvent(release); FinishWrites(app);
    MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    Check(deferredCloses == 1, "successful save resumes one close request");
    deferredCloses = 0;
    ResetEvent(entered); ResetEvent(release);
    options.afterWrite = [] { return false; };
    expectError = true;
    Check(background_save::Request(app, snapshotPath.c_str(), options), "start failing pending-close save");
    Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "failing worker reached gate");
    SendMessageW(app->MainWindow(), WM_CLOSE, 0, 0);
    SetEvent(release); FinishWrites(app); expectError = false;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    Check(!deferredCloses, "failed manual save keeps application open");
    SetWindowLongPtrW(app->MainWindow(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(realMain));
    CloseHandle(entered); CloseHandle(release);

    autosave::Initialize(app);
    autosave::Settings settings;
    settings.enabled = true; settings.maxBackups = 1; settings.overwriteOldest = true;
    settings.directory = (root / L"backups").wstring();
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "configure isolated backup folder");
    const auto recentBefore = Recent();
    app->SceneModified() = 1;
    Check(autosave::QueueBackup(app), "automatic backup uses shared worker");
    FinishWrites(app);
    auto backups = autosave::Backups(settings.directory);
    Check(backups.size() == 1 && app->SceneModified() && Recent() == recentBefore,
        "background backup preserves dirty state and recent-project list");
    const auto firstBackup = backups.front();
    auto emm = firstBackup; emm.replace_extension(L".emm"); std::ofstream(emm) << "paired sidecar";
    Sleep(2);
    Check(autosave::QueueBackup(app), "queue rotation backup");
    const auto overwriteDeadline = GetTickCount64() + 5000;
    while (std::wstring(background_save::StatusText(app)).find(L"覆盖") == std::wstring::npos &&
        GetTickCount64() < overwriteDeadline) Sleep(1);
    Check(std::wstring(background_save::StatusText(app)).find(L"覆盖") != std::wstring::npos,
        "full-folder backup announces replacement before completion");
    FinishWrites(app);
    backups = autosave::Backups(settings.directory);
    Check(backups.size() == 1 && !fs::exists(firstBackup) && !fs::exists(emm), "rotation removes owned old PMM and EMM together");
    Check(std::wstring(background_save::StatusText(app)).find(L"覆盖") != std::wstring::npos, "backup replacement is reported in top strip");
    for (int language : {0, 1, 2}) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        Capture(app->MainWindow(), root / (L"backup-replaced-" + std::to_wstring(language) + L".bmp"));
    }
    settings.overwriteOldest = false;
    Check(autosave::Configure(app, settings, error), "disable backup overwrites");
    Check(autosave::QueueBackup(app), "queue full-folder check"); FinishWrites(app);
    Check(autosave::Backups(settings.directory) == backups, "full folder without overwrite retains old backup");
    settings.overwriteOldest = true;
    Check(autosave::Configure(app, settings, error), "enable overwrite for active-backup protection test");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, backups.front().c_str());
    Check(autosave::QueueBackup(app), "queue backup while editing recovery project"); FinishWrites(app);
    Check(autosave::Backups(settings.directory) == backups, "rotation cannot delete the opened recovery project");
    settings.enabled = false;
    Check(autosave::Configure(app, settings, error), "disable test backups");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, snapshotPath.c_str());
}
INT_PTR Dispatch(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    return sequenceDialog ? seq::DialogProc(dialog, msg, wp, lp) : AviOutDlgProc(dialog, msg, wp, lp);
}
void Click(HWND dialog, int id) {
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, id)));
    if (id == panel::kExportSaveProjectButton) {
        FinishWrites(g_Block);
        SendMessageW(dialog, WM_TIMER, background_save::kStatusTimer, 0);
    }
}
void CheckLayout(HWND dialog) {
    const HWND warning = GetDlgItem(dialog, panel::kExportUnsavedWarning);
    const HWND save = GetDlgItem(dialog, panel::kExportSaveProjectButton);
    RECT available{}, measured{}; GetClientRect(warning, &available); measured = available;
    HDC dc = GetDC(warning);
    const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(warning, WM_GETFONT, 0, 0)));
    DrawTextW(dc, Text(warning).c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    Check(measured.bottom <= available.bottom && measured.right <= available.right, "localized warning fits without clipping");
    const auto brush = SendMessageW(dialog, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(warning));
    Check(brush && GetTextColor(dc) == RGB(192,32,32), "unsaved explanation uses red text");
    SelectObject(dc, old); ReleaseDC(warning, dc);
    RECT a{}, b{}, footer{}; GetWindowRect(warning, &a); GetWindowRect(save, &b); GetWindowRect(GetDlgItem(dialog, IDOK), &footer);
    Check(a.right <= b.left && a.bottom <= footer.top && b.bottom <= footer.top, "warning and Save button do not overlap the footer");
    dc = GetDC(save); const auto oldButtonFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(save, WM_GETFONT, 0, 0)));
    SIZE size{}; const auto label = Text(save); GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
    Check(size.cx + 12 <= b.right - b.left, "localized Save button label fits");
    SelectObject(dc, oldButtonFont); ReleaseDC(save, dc);
}
INT_PTR CALLBACK Probe(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    const auto result = Dispatch(dialog, msg, wp, lp);
    if (msg != WM_INITDIALOG) return result;
    auto* app = g_Block;
    const HWND warning = GetDlgItem(dialog, panel::kExportUnsavedWarning);
    const HWND save = GetDlgItem(dialog, panel::kExportSaveProjectButton);
    const int compactHeight = Height(dialog);
    Check(!Shown(warning) && !Shown(save), "unchecked close option keeps the dialog compact");
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"48");
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, BST_CHECKED);
    Click(dialog, panel::kAviCloseAfterExportCheckbox);
    if (!app->SceneModified()) {
        Check(!Shown(warning) && !Shown(save) && Height(dialog) == compactHeight,
            "pristine empty project does not ask to save when automatic close is enabled");
        app->SceneModified() = 1;
        Click(dialog, panel::kAviCloseAfterExportCheckbox);
    }
    Check(Shown(warning) && Shown(save) && Height(dialog) > compactHeight, "unsaved project shows warning and Save action");
    CheckLayout(dialog);
    Capture(dialog, root / ((sequenceDialog ? L"sequence-" : L"video-") + std::to_wstring(app->EnglishUI()) + L".bmp"));
    const auto beforeHistory = Recent();
    expectedOwner = dialog; cancelPick = true;
    Click(dialog, panel::kExportSaveProjectButton); cancelPick = false;
    Check(!app->EnvFileName()[0] && Shown(warning) && Recent() == beforeHistory, "cancel from export retains prompt and unnamed state");
    chosen = root / L"missing" / L"export-save.pmm"; expectError = true;
    Click(dialog, panel::kExportSaveProjectButton); expectError = false;
    Check(!app->EnvFileName()[0] && Shown(warning) && Recent() == beforeHistory, "failed save from export retains prompt");
    chosen = root / ((sequenceDialog ? L"图片工程-" : L"视频工程-") + std::to_wstring(app->EnglishUI()) + L".pmm");
    Click(dialog, panel::kExportSaveProjectButton);
    Check(fs::is_regular_file(chosen) && !app->SceneModified() && !Shown(warning) && !Shown(save), "successful save collapses the prompt");
    Check(Height(dialog) == compactHeight && Recent().front() == chosen.wstring(), "saved export project appears in recent list and restores compact layout");
    Check(Text(GetDlgItem(dialog, panel::kAviFpsEdit)) == L"48" &&
        IsDlgButtonChecked(dialog, panel::kAviCloseAfterExportCheckbox) == BST_CHECKED,
        "saving preserves export settings and close checkbox");
    app->SceneModified() = 1;
    Click(dialog, panel::kAviCloseAfterExportCheckbox);
    Check(Shown(warning), "previously saved project with new edits also needs saving");
    const auto beforePicks = picks;
    Click(dialog, panel::kExportSaveProjectButton);
    Check(!app->SceneModified() && !Shown(warning) && picks == beforePicks, "named project saves in place without another picker");
    Click(dialog, IDCANCEL);
    return TRUE;
}
}
#include "model_load_view_checks.hpp"
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc == 2, "isolated artifact directory required");
    root = fs::absolute(argv[1]); fs::create_directories(root);
    history = root / L"recent-pmm.txt";
    Check(!fs::exists(history), "fresh isolated history required");
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", history.c_str());
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize hidden test guards");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), &Message, nullptr) == MH_OK, "guard message boxes");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBeep), &NoBeep, nullptr) == MH_OK, "silence save beep");
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetUiSaveFileNameW), &Pick, nullptr) == MH_OK, "automate only the Save As picker");
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), &Hide, reinterpret_cast<void**>(&originalShow)) == MH_OK, "hide test windows");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable input and visibility guards");
    const auto hook = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "prevent activation");
    auto* app = new MMDApp; g_Block = app; app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production app");
    if (const auto* modelPath = _wgetenv(L"MMS_MODEL_LOAD_VIEW_TEST")) {
        CheckModelLoadView(app, modelPath);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        return 0;
    }
    SaveTests(app);
    AsyncTests(app);
    VersionTests(app);
    for (int language : {0, 1, 2}) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        for (bool sequence : {false, true}) {
            sequenceDialog = sequence;
            // Exercise pristine empty projects as well as unsaved edits.
            app->EnvFileName()[0] = 0; app->SceneModified() = sequence ? 1 : 0;
            seq::DialogContext context{app}; context.options.directory = root.wstring();
            context.options.width = 640; context.options.height = 480;
            Check(UiDialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(sequence ? seq::kDialog : language ? 654 : 608),
                app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&context)) == IDCANCEL, "export dialog remains usable after saving");
        }
    }
    Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
    ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
    std::printf("PASS project-save/export checks=%u\n", checks);
}
