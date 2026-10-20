// Explicit hidden integration test: real Save/Save As and both export dialogs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "../src/window/image_sequence_dialog.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/project_storage.hpp"
#endif
#include <MinHook.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>

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
#if !MMD_ORIGINAL_PROFILE
// A metadata-only participant deliberately changes the common journal without
// calling SceneModified. This catches completion checks based only on PMM's
// scene revision, independently of any particular model command.
class SaveHistoryValue final : public plugins::HistoryChange {
    int& value_;
    int before_, after_;
    struct Candidate final : plugins::PreparedHistoryChange {
        int& value;
        int expected, target;
        Candidate(int& v, int e, int t) : value(v), expected(e), target(t) {}
        bool IsCurrent() const noexcept override { return value == expected; }
        void Publish() noexcept override { value = target; }
    };
public:
    SaveHistoryValue(int& value, int before, int after) : value_(value), before_(before), after_(after) {}
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        const int expected = forward ? before_ : after_;
        if (value_ != expected) return MMS_E_STALE;
        out = std::make_unique<Candidate>(value_, expected, forward ? after_ : before_);
        return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override { return sizeof(*this); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
struct SavePendingTransaction {
    MMDApp& app;
    float fov;
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> native;
    std::unique_ptr<plugins::SharedHistory::ResetTransaction> reset;
    std::unique_ptr<plugins::SharedHistory::AppliedTransaction> applied;
    SavePendingTransaction(MMDApp& a, plugins::SharedHistory& journal, int& value, int kind)
        : app(a), fov(a.CameraFov()) {
        if (kind == 0)
            Check(journal.PrepareNative("save pending", std::make_shared<SaveHistoryValue>(value, value, value + 1), native) == MMS_OK,
                "prepare native transaction around save");
        else if (kind == 1)
            Check(journal.PrepareReset(reset) == MMS_OK, "prepare reset transaction around save");
        else {
            Check(journal.ReserveAppliedNative("save applied preview", 1024, applied) == MMS_OK,
                "reserve applied transaction around save");
            // The preview is cancellable live state, not a committed edit.
            app.CameraFov() = fov + 1;
        }
    }
    void Cancel() {
        if (applied) app.CameraFov() = fov;
        applied.reset(); reset.reset(); native.reset();
    }
    ~SavePendingTransaction() { Cancel(); }
};
void AbiTransactionTests(MMDApp* app) {
    plugins::StorageStore storage;
    plugins::SharedHistory journal(storage, [app] {
        return SceneSaveBlocksEdits() || app->SceneMutationInProgress() ? MMS_E_BUSY : MMS_OK;
    });
    plugins::NativePoseTestBinding binding(*app, journal);
    int value = 0;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(entered && release, "create transaction save worker gates");
    for (const auto mode : {plugins::SharedHistory::RecordingMode::AllOperations,
                           plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        Check(journal.SetMode(mode) == MMS_OK, "select background save recording mode");
        for (int kind = 0; kind != 3; ++kind) {
            const auto suffix = std::to_wstring(static_cast<int>(mode)) + L"-" + std::to_wstring(kind);
            const auto rejected = root / (L"abi-rejected-" + suffix + L".pmm");
            app->SceneModified() = 1;
            const auto oldPath = std::wstring(app->EnvFileName());
            const auto oldSceneRevision = app->SceneRevision();
            const float oldFov = app->CameraFov();
            {
                SavePendingTransaction pending(*app, journal, value, kind);
                Check(!background_save::Request(app, rejected.c_str()), "pending native/reset/applied transaction rejects snapshot");
                Check(!SceneSaveInProgress() && !SceneSaveBlocksEdits() && !fs::exists(rejected) &&
                    !fs::exists(plugins::PluginSidecar(rejected)), "rejected snapshot publishes neither PMM nor sidecar and releases save gates");
                Check(std::wstring(app->EnvFileName()) == oldPath && app->SceneModified() &&
                    app->SceneRevision() == oldSceneRevision, "rejected snapshot does not complete or modify scene identity");
            }
            ResetEvent(entered); ResetEvent(release);
            unsigned completed = 0; bool success = false;
            const auto path = root / (L"abi-deferred-" + suffix + L".pmm");
            background_save::Options options;
            options.beforeWrite = [&] { SetEvent(entered); return WaitForSingleObject(release, 10000) == WAIT_OBJECT_0; };
            options.completed = [&](bool ok) { ++completed; success = ok; };
            Check(background_save::Request(app, path.c_str(), options), "start save before a new transaction");
            Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "transaction save worker reached write gate");
            Check(SceneSaveInProgress() && !SceneSaveBlocksEdits(), "disk IO does not hold the scene edit gate");
            Check(journal.Enter() == MMS_OK, "ordinary history entry remains available during disk IO");
            journal.Leave();
            uint64_t beforeRevision = 0, afterRevision = 0;
            Check(journal.Revision(beforeRevision) == MMS_OK, "read journal revision before cancellable transaction");
            {
                SavePendingTransaction pending(*app, journal, value, kind);
                SetEvent(release);
                // Wait joins the real worker without dispatching its completion.
                background_save::Wait(app);
                Check(background_save::HandleMessage(app, background_save::kComplete, 0), "consume deferred completion message");
                Check(fs::is_regular_file(path) && SceneSaveInProgress() && !SceneSaveBlocksEdits() && completed == 0,
                    "published disk snapshot defers UI completion until transaction cancellation");
                Check(app->SceneModified() && std::wstring(app->EnvFileName()) == oldPath &&
                    app->SceneRevision() == oldSceneRevision &&
                    app->CameraFov() == (kind == 2 ? oldFov + 1 : oldFov),
                    "deferred completion preserves dirty preview and old project path");
            }
            Check(journal.Revision(afterRevision) == MMS_OK && beforeRevision == afterRevision &&
                journal.UndoCount() == 0 && journal.RedoCount() == 0, "cancelled transaction changes neither revision nor undo cursor");
            background_save::HandleMessage(app, background_save::kComplete, 0);
            FinishWrites(app);
            Check(completed == 1 && success && !SceneSaveBlocksEdits() && !app->SceneModified() &&
                std::wstring(app->EnvFileName()) == path.wstring() && app->CameraFov() == oldFov,
                "safe boundary completes exactly once after cancellation");
            background_save::HandleMessage(app, background_save::kComplete, 0);
            Check(completed == 1, "duplicate completion cannot repeat callbacks");
        }
    }

    ResetEvent(entered); ResetEvent(release);
    const auto path = root / L"abi-history-revision.pmm";
    background_save::Options options;
    options.beforeWrite = [&] { SetEvent(entered); return WaitForSingleObject(release, 10000) == WAIT_OBJECT_0; };
    app->SceneModified() = 1;
    const auto sceneRevision = app->SceneRevision();
    uint64_t historyRevision = 0, editedRevision = 0;
    Check(journal.Revision(historyRevision) == MMS_OK, "capture journal revision independently of scene dirty flag");
    Check(background_save::Request(app, path.c_str(), options), "capture snapshot before journal-only publication");
    Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "journal-only snapshot reached worker gate");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> edit;
    Check(journal.PrepareNative("history-only change", std::make_shared<SaveHistoryValue>(value, 0, 1), edit) == MMS_OK &&
        journal.CommitNative(std::move(edit)) == MMS_OK, "publish real common-history change while writer is blocked");
    Check(journal.Revision(editedRevision) == MMS_OK && editedRevision != historyRevision &&
        app->SceneRevision() == sceneRevision && value == 1, "history-only edit advances journal but not native scene revision");
    SetEvent(release); FinishWrites(app);
    Check(app->SceneModified() && journal.UndoCount() == 1 && value == 1,
        "older snapshot cannot clear dirty state after journal-only edit");

    const auto sidecar = plugins::PluginSidecar(path);
    const auto pair = plugins::ReadPluginProject(path);
    Check(fs::is_regular_file(sidecar) && !pair.projectId.empty() && !pair.saveId.empty() &&
        pair.mainHash == plugins::FileHash(path), "background PMM has a verified matching plugin sidecar");
    const auto oldMain = Bytes(path), oldSidecar = Bytes(sidecar);
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock PMM against paired replacement");
    const float oldFov = app->CameraFov(); app->CameraFov() = oldFov + 7; app->SceneModified() = 1;
    const auto oldErrors = errors; expectError = true;
    Check(background_save::Request(app, path.c_str()), "accept replacement whose paired publication must fail");
    FinishWrites(app); expectError = false; CloseHandle(locked);
    const auto preserved = plugins::ReadPluginProject(path);
    Check(errors == oldErrors + 1 && Bytes(path) == oldMain && Bytes(sidecar) == oldSidecar &&
        preserved.saveId == pair.saveId && preserved.mainHash == plugins::FileHash(path) && app->SceneModified(),
        "failed background replacement preserves both old pair members and unsaved edits");
    app->CameraFov() = oldFov;
    Check(journal.Reset() == MMS_OK, "release local transaction regression history");
    journal.Collect();
    CloseHandle(entered); CloseHandle(release);
}
#endif
void SaveOpenAliasTests(MMDApp* app) {
    // Use different path lengths as well as FOV values: completion replaces
    // EnvFileName while LoadSceneFile is waiting for the writer.
    const auto original = root / L"alias-A.pmm";
    const auto pending = root / L"alias-save-as-longer-B.pmm";
    app->CameraFov() = 37; app->SceneModified() = 1;
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, original.c_str());
    Check(SaveSceneFile(app), "write original project for aliased open");
    app->CameraFov() = 71; app->SceneModified() = 1;
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(entered && release, "create aliased-open worker gates");
    background_save::Options options;
    options.beforeWrite = [&] { SetEvent(entered); return WaitForSingleObject(release, 10000) == WAIT_OBJECT_0; };
    Check(background_save::Request(app, pending.c_str(), options), "start pending Save As before aliased open");
    Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0 && std::wstring(app->EnvFileName()) == original.wstring(),
        "Save As leaves original aliased filename until completion");
    SetEvent(release);
    // Deliberately no Wait or message pump before the real loader: it owns
    // the wait and must freeze the caller's original pointer first.
    Check(LoadSceneFile(app->EnvFileName()), "open aliased filename while Save As completes");
    Check(!SceneSaveInProgress() && std::wstring(app->EnvFileName()) == original.wstring() && app->CameraFov() == 37,
        "aliased open loads original A and its FOV rather than completed Save As B");
    Check(fs::is_regular_file(pending) && LoadSceneFile(pending.c_str()) && app->CameraFov() == 71,
        "pending Save As still contains its independent B snapshot");
    CloseHandle(entered); CloseHandle(release);

    fs::create_directories(root / L"path-normalization");
    const auto unnormalized = root / L"path-normalization" / L".." / L"normalized-save-as.pmm";
    const auto normalized = unnormalized.lexically_normal();
    app->SceneModified() = 1;
    Check(background_save::Request(app, unnormalized.c_str()), "accept Save As path containing parent traversal");
    FinishWrites(app);
    Check(fs::is_regular_file(normalized) && std::wstring(app->EnvFileName()) == normalized.wstring() &&
        Recent().front() == normalized.wstring(), "Save As publishes one normalized identity to disk, current path and recent projects");
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
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc == 2, "isolated artifact directory required");
    root = fs::absolute(argv[1]); fs::create_directories(root);
    history = root / L"recent-pmm.txt";
    SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", (root / L"autosave.ini").c_str());
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
    SaveTests(app);
    AsyncTests(app);
#if !MMD_ORIGINAL_PROFILE
    AbiTransactionTests(app);
#endif
    SaveOpenAliasTests(app);
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
