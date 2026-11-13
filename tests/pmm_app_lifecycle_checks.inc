// Opt in with MMS_PMM_APP_LIFECYCLE_ONLY=1 through the unchanged staged runner.
// MMS_PMM_APP_LIFECYCLE_CASE=normal-exit (default), save-close, save-close-failure,
// or video-close-mf (also requires an explicit MMS_PMM_APP_VIDEO_FIXTURE).
// This file is included inside the host fixture's anonymous namespace.
} // namespace
#include "mikumikustudio/texture_prepare_cache.hpp"
#include "mikumikustudio/native_media.hpp"
#include <mfapi.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);
namespace {

bool AppLifecycleRequested() {
    const auto* value = _wgetenv(L"MMS_PMM_APP_LIFECYCLE_ONLY");
    if (!value) return false;
    Check(!std::wcscmp(value, L"1") && !MMD_ORIGINAL_PROFILE,
          "app lifecycle requires explicit value 1 and full profile");
    Check(!_wgetenv(L"MMS_PMM_CACHE_SAVE_ONLY") && !_wgetenv(L"MMS_PMM_CACHE_RESET_ONLY"),
          "app lifecycle, private save and real Reset modes are mutually exclusive");
    return true;
}

#if !MMD_ORIGINAL_PROFILE
namespace host_app_lifecycle {
fs::path root, cacheRoot, source, payload;
std::vector<unsigned char> publishedBytes;
cache_manager::SceneToken initialToken;
HWND hostWindow = nullptr;
DWORD thread = 0;
unsigned initCalls = 0, updateCalls = 0, cleanupCalls = 0, cacheShutdownCalls = 0, quitCalls = 0;
bool initialized = false, produced = false, closeRequested = false, cleaned = false, cacheStopped = false;
bool saveCloseMode = false;
bool saveCloseFailureMode = false;
bool videoCloseMode = false;
namespace save_close { bool FailureMessage(HWND, LPCWSTR, LPCWSTR, UINT); }
decltype(&InitMainWindowAndD3D) realInitialize = nullptr;
decltype(&updates::Initialize) realUpdates = nullptr;
decltype(&ShutdownCleanup) realCleanup = nullptr;
decltype(&cache_manager::Shutdown) realCacheShutdown = nullptr;

// UI confinement is the only behavior replacement. Unexpected dialogs fail
// without showing UI or fabricating an affirmative response.
int WINAPI UnexpectedW(HWND window, LPCWSTR text, LPCWSTR caption, UINT flags) {
    if (saveCloseFailureMode && save_close::FailureMessage(window, text, caption, flags)) return IDOK;
    std::fprintf(stderr, "FAIL APP_LIFECYCLE unexpected dialog: %ls: %ls\n",
                 caption ? caption : L"", text ? text : L"");
    std::_Exit(1);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR caption, UINT) {
    std::fprintf(stderr, "FAIL APP_LIFECYCLE unexpected dialog: %s: %s\n",
                 caption ? caption : "", text ? text : "");
    std::_Exit(1);
}

void Boundary() {
    Check(GetCurrentThreadId() == thread, "lifecycle observers stay on the actual WinMain thread");
    host_cache_save::HiddenResetBoundary();
}

DWORD LeaseProbe() {
    // Request access only: never delete or mark the file for deletion.
    const HANDLE handle = CreateFileW(payload.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    Check(CloseHandle(handle) != FALSE, "close the nonmutating lease probe");
    return ERROR_SUCCESS;
}

#include "pmm_app_video_lifecycle_checks.inc"

namespace save_close {
// Reuse the save fixture's Options worker-gate mechanism, but never its
// Worker::Finish/Wait: the actual WinMain message loop must deliver completion.
// Only worker callbacks wait. The UI polls events with a zero timeout.
constexpr UINT_PTR pollTimer = 0x43415331;
constexpr UINT checkpointMessage = WM_APP + 0x6f3;
enum class Phase { Idle, AwaitWorker, ClosePosted, AwaitCheckpoint, Writing, Completing, Completed,
    FailedCallback, FailedNotified, RetryWriting, RetryCompleted, RetryClosePosted, Closing };
Phase phase = Phase::Idle;
HANDLE entered = nullptr, releaseWrite = nullptr, written = nullptr, releaseCompletion = nullptr;
std::atomic<DWORD> writerThread{0};
ULONGLONG deadline = 0;
unsigned closeMessages = 0, deferrals = 0, completions = 0, event = 0;
bool insideClose = false;
fs::path destination;
std::vector<unsigned char> diskBytes;
mdl::CameraKey frozenCamera{};
std::uint64_t generation = 0, revision = 0;
HANDLE failureLock = INVALID_HANDLE_VALUE;
std::vector<unsigned char> beforeDisk;
unsigned failureMessages = 0, settledTurns = 0;
decltype(&MainWndProc) realWindow = nullptr;
decltype(&background_save::DeferClose) realDefer = nullptr;

void Event(const char* name) {
    std::printf("APP_SAVE_CLOSE event=%u %s\n", ++event, name);
}
bool Signaled(HANDLE handle) {
    const auto result = WaitForSingleObject(handle, 0);
    Check(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "nonblocking save gate poll succeeds");
    return result == WAIT_OBJECT_0;
}
bool BeforePublication() {
    return saveCloseFailureMode ? Bytes(destination) == beforeDisk : !fs::exists(destination);
}
void StillOpen() {
    Boundary();
    Check(IsWindow(hostWindow) && !quitCalls && !cleanupCalls && !cacheShutdownCalls && !cleaned && !cacheStopped &&
          cache_manager::CaptureActiveScene() && LeaseProbe() == ERROR_SHARING_VIOLATION,
          "pending save keeps the real window, cache scene and lease alive before all shutdown stages");
}
void Owner(bool saved) {
    const auto view = cache_manager::Inspect();
    Check(view.error.empty(), "inspect real save ownership after actual texture production");
    if (!saved) {
        Check(view.projects.empty(), "pending first save has not bound any project cache owner");
    } else {
        Check(view.projects.size() == 1 && view.projects[0].current &&
              !_wcsicmp(view.projects[0].path.c_str(), destination.c_str()) &&
              view.projects[0].totalBytes >= publishedBytes.size(),
              "real save completion binds the produced texture to precisely the saved project");
    }
}
void ReadFrozenSave() {
    int fd = -1;
    Check(_wsopen_s(&fd, destination.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, 0) == 0 && fd >= 0,
          "open the real background save publication");
    pmm_io::Input input(fd);
    pmm_extension::Document document; std::uint64_t standard = 0;
    Check(pmm_extension::Read(fd, document, standard) == pmm_extension::ReadResult::Loaded,
          "existing PMM trailer reader validates the real background publication");
    diskBytes = Bytes(destination);
    Check(standard > 55 && standard <= diskBytes.size() && diskBytes[54] == 0 &&
          !std::memcmp(diskBytes.data(), "Polygon Movie maker 0002", 24),
          "background writer publishes a complete native empty-model PMM and validated extension");
    float fov = 0; std::memcpy(&fov, diskBytes.data() + 42, sizeof(fov));
    Check(fov == 51.f, "saved header retains the nondefault captured FOV");
    Check(input.EnableCheckedReads() && pmm_io::Seek(fd, 55, SEEK_SET) == 55,
          "read the frozen camera section with strict production segment reads");
    mdl::CameraKey camera{};
    pmm_io::ReadPmmCameraKey<pmm_io::PmmStream::V2>(fd, camera);
    std::int32_t sparse = -1; pmm_io::Rd(fd, &sparse, sizeof(sparse));
    Check(!camera.frame && !camera.previous && !camera.next && sparse == 0 &&
          camera.distance == frozenCamera.distance && camera.fov == frozenCamera.fov &&
          camera.perspective == frozenCamera.perspective && camera.selected == frozenCamera.selected &&
          camera.parentModel == frozenCamera.parentModel && camera.parentBone == frozenCamera.parentBone &&
          !std::memcmp(camera.eye, frozenCamera.eye, sizeof(camera.eye)) &&
          !std::memcmp(camera.target, frozenCamera.target, sizeof(camera.target)) &&
          !std::memcmp(camera.interpolation, frozenCamera.interpolation, sizeof(camera.interpolation)),
          "published camera fields match the frozen nondefault scene captured before close");
}
void Completed(bool success) {
    if (saveCloseFailureMode && phase == Phase::Writing) {
        Check(GetCurrentThreadId() == thread && ++completions == 1 && !success && !failureMessages &&
              failureLock != INVALID_HANDLE_VALUE && !Signaled(written),
              "real publication failure completes once before the manual-save error notification");
        StillOpen(); Owner(false);
        Check(SceneSaveInProgress() && SceneSaveBlocksEdits() && app->SceneModified() && !*app->EnvFileName() &&
              app->SceneGeneration() == generation && app->SceneRevision() == revision && BeforePublication(),
              "failed actual writer preserves old PMM, anonymous ownership, dirty revision and cache lease through callback");
        phase = Phase::FailedCallback; deadline = GetTickCount64() + 10000;
        Event("real-save-failed window-alive=1 old-PMM-retained=1");
        return;
    }
    const bool retry = saveCloseFailureMode;
    Check(GetCurrentThreadId() == thread && phase == (retry ? Phase::RetryWriting : Phase::Completing) &&
          ++completions == (retry ? 2u : 1u) && success,
          "real successful save completion arrives exactly once on the WinMain thread");
    if (retry) {
        Check(failureMessages == 1 && failureLock == INVALID_HANDLE_VALUE && Bytes(destination) != beforeDisk,
              "retry uses the real unlocked writer and replaces the distinct old PMM only on success");
        ReadFrozenSave();
    }
    StillOpen();
    Check(SceneSaveInProgress() && SceneSaveBlocksEdits(), "production keeps both gates through the completion callback");
    // ModifiedFlag advances the revision even when CompleteSave clears dirty.
    // The publication checkpoint below requires the original revision first.
    Check(app->SceneGeneration() == generation && app->SceneRevision() == revision + 1 && !app->SceneModified() &&
          !std::wcscmp(app->EnvFileName(), destination.c_str()) && Bytes(destination) == diskBytes,
          "real CompleteSave accepts frozen disk data, binds its path and clears only the captured revision");
    Owner(true);
    phase = retry ? Phase::RetryCompleted : Phase::Completed; deadline = GetTickCount64() + 10000;
    Event(retry ? "retry-save-completed window-alive=1" : "real-save-completed window-alive=1 cleanup=0 cache-shutdown=0");
    if (retry) {
        settledTurns = 0;
        Check(PostMessageW(hostWindow, checkpointMessage, 0, 0) != FALSE,
              "observe retry completion on a later actual UI turn without requesting close");
    }
    // Finish will clear its gates and post WM_CLOSE itself after this returns.
}
bool FailureMessage(HWND window, LPCWSTR text, LPCWSTR caption, UINT flags) {
    if (phase != Phase::FailedCallback) return false;
    Check(window == hostWindow && GetCurrentThreadId() == thread && app->EnglishUI() == 1 &&
          text && caption && !std::wcscmp(caption, L"PMM") && flags == (MB_OK | MB_ICONERROR) &&
          !std::wcscmp(text, L"The PMM file could not be saved completely. Check free space and write access.") &&
          ++failureMessages == 1 && completions == 1 && closeMessages == 1 &&
          SceneSaveInProgress() && SceneSaveBlocksEdits() && app->SceneModified() && BeforePublication(),
          "only the expected fixture-induced MB_OK save error is acknowledged while both gates and old data remain intact");
    StillOpen(); Owner(false);
    // This acknowledgement is a test double for this exact error notification,
    // never a user permission, picker, overwrite decision or real desktop input.
    phase = Phase::FailedNotified; settledTurns = 0;
    Event("expected-save-error-notified gates-held=1 lease-held=1");
    Check(PostMessageW(hostWindow, checkpointMessage, 0, 0) != FALSE,
          "observe failed Finish after its notification returns to the real UI pump");
    return true;
}
void Start(MMDApp* value) {
    Check(phase == Phase::Idle && value == app && !SceneSaveInProgress(), "start one isolated background-save close case");
    entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    releaseWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    written = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    releaseCompletion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(entered && releaseWrite && written && releaseCompletion, "create bounded real-save worker gates");
    destination = root / L"close-pending.pmm";
    Check(!fs::exists(destination) && !*app->EnvFileName() && app->CameraKeys(),
          "first background save starts from the anonymous startup scene with its real camera pool");
    if (saveCloseFailureMode) {
        app->EnglishUI() = 1;
        Check(SaveSceneCopy(app, destination.c_str()) && !*app->EnvFileName() && !app->SceneModified(),
              "write an actual prior PMM copy without binding or dirtying the anonymous scene");
        beforeDisk = Bytes(destination);
        Check(!beforeDisk.empty(), "retain complete prior PMM bytes before an actual publication failure");
        failureLock = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(failureLock != INVALID_HANDLE_VALUE,
              "hold only the fixture PMM without write/delete sharing to cause a real replacement failure");
        Owner(false);
    }
    frozenCamera = CameraRoot(); frozenCamera.distance = -62.5f; frozenCamera.fov = 51;
    frozenCamera.target[0] = 3.25f;
    app->CameraKeys()[0] = frozenCamera; app->CameraFov() = 51.f; app->SceneModified() = 1;
    generation = app->SceneGeneration(); revision = app->SceneRevision();
    background_save::Options options;
    options.beforeWrite = [] {
        writerThread.store(GetCurrentThreadId());
        return SetEvent(entered) && WaitForSingleObject(releaseWrite, 15000) == WAIT_OBJECT_0;
    };
    options.afterWrite = [] {
        return SetEvent(written) && WaitForSingleObject(releaseCompletion, 15000) == WAIT_OBJECT_0;
    };
    options.completed = &Completed;
    Check(background_save::Request(value, destination.c_str(), std::move(options)),
          "production Request synchronously freezes the real scene and schedules its writer");
    Check(SceneSaveInProgress() && !SceneSaveBlocksEdits() && !completions &&
          app->SceneGeneration() == generation && app->SceneRevision() == revision,
          "captured save is pending without blocking editing or the UI pump");
    phase = Phase::AwaitWorker; deadline = GetTickCount64() + 10000;
    Check(SetTimer(hostWindow, pollTimer, 10, nullptr) == pollTimer, "arm bounded nonblocking UI save-gate polling");
}
void Poll() {
    Check(GetCurrentThreadId() == thread && GetTickCount64() <= deadline,
          "background-save close stage completes within its bounded deadline");
    if (phase == Phase::AwaitWorker && Signaled(entered)) {
        Check(writerThread.load() != thread && writerThread.load() != 0 && !Signaled(releaseWrite),
              "actual worker is held before writing on a different thread");
        StillOpen(); Owner(false);
        phase = Phase::ClosePosted; closeRequested = true;
        Event("worker-gated posting-initial-WM_CLOSE");
        Check(PostMessageW(hostWindow, WM_CLOSE, 0, 0) != FALSE, "post first real WM_CLOSE while the save worker is gated");
    } else if (phase == Phase::Writing && Signaled(written)) {
        StillOpen();
        Check(SceneSaveInProgress() && !SceneSaveBlocksEdits() && !completions && closeMessages == 1 &&
              app->SceneGeneration() == generation && app->SceneRevision() == revision &&
              app->SceneModified() && !*app->EnvFileName() && !Signaled(releaseCompletion),
              "actual disk publication alone cannot close the window or commit the live scene");
        ReadFrozenSave(); Owner(false);
        Event("disk-published completion-gated window-alive=1");
        phase = Phase::Completing; deadline = GetTickCount64() + 10000;
        Check(SetEvent(releaseCompletion) != FALSE, "allow worker to report its actual disk result to the real message loop");
    }
}
bool Defer(MMDApp* value) {
    const bool result = realDefer(value);
    if (insideClose && value == app) {
        ++deferrals;
        Check((closeMessages == 1 && deferrals == 1 && result) ||
              (closeMessages == 2 && deferrals == 2 && !result),
              "production DeferClose waits for the first request and accepts only the post-completion replay");
    }
    return result;
}
LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (window != hostWindow) return realWindow(window, message, wp, lp);
    if (message == WM_TIMER && wp == pollTimer) { Poll(); return 0; }
    if (message == checkpointMessage) {
        if (saveCloseFailureMode && (phase == Phase::FailedNotified || phase == Phase::RetryCompleted)) {
            const bool retry = phase == Phase::RetryCompleted;
            Check(!SceneSaveInProgress() && !SceneSaveBlocksEdits() && failureMessages == 1 &&
                  closeMessages == 1 && deferrals == 1 && completions == (retry ? 2u : 1u),
                  "failed-close request stays consumed after gates clear and after a later successful retry");
            StillOpen(); Owner(retry);
            if (!retry) Check(app->SceneModified() && !*app->EnvFileName() &&
                app->SceneGeneration() == generation && app->SceneRevision() == revision && BeforePublication(),
                "failure leaves the still-open scene editable with unchanged dirty revision, path and original PMM");
            else Check(!app->SceneModified() && !std::wcscmp(app->EnvFileName(), destination.c_str()) &&
                Bytes(destination) == diskBytes, "successful retry is accepted without an inherited close request");
            if (++settledTurns < 2) {
                Check(PostMessageW(hostWindow, checkpointMessage, 0, 0) != FALSE,
                      "allow an incorrectly queued close to run before the second independent UI checkpoint");
                return 0;
            }
            if (!retry) {
                Event("failed-close-settled no-replay=1 dirty-retained=1");
                Check(CloseHandle(failureLock) != FALSE, "release only the fixture PMM lock for a real retry");
                failureLock = INVALID_HANDLE_VALUE;
                app->CameraKeys()[0].distance -= 4.f; app->SceneModified() = 1;
                frozenCamera = app->CameraKeys()[0]; generation = app->SceneGeneration(); revision = app->SceneRevision();
                background_save::Options options; options.completed = &Completed;
                phase = Phase::RetryWriting; deadline = GetTickCount64() + 10000;
                Check(background_save::Request(app, destination.c_str(), std::move(options)),
                      "a new edit and real retry save are accepted after the failed close without resetting the cache scene");
                Event("retry-save-requested no-new-close=1");
            } else {
                Event("retry-save-settled no-implicit-close=1");
                phase = Phase::RetryClosePosted;
                Check(PostMessageW(hostWindow, WM_CLOSE, 0, 0) != FALSE,
                      "request a fresh clean close only after proving retry does not replay the old failed close");
            }
            return 0;
        }
        Check(phase == Phase::AwaitCheckpoint && closeMessages == 1 && deferrals == 1 && !completions &&
              SceneSaveInProgress() && !SceneSaveBlocksEdits() && BeforePublication(),
              "UI pump returned from deferred WM_CLOSE without writing, completing or destroying the scene");
        StillOpen(); Owner(false);
        Event("deferred-close-returned-to-pump releasing-write-gate");
        phase = Phase::Writing; deadline = GetTickCount64() + 10000;
        Check(SetEvent(releaseWrite) != FALSE, "release only the disk scheduling gate after close has genuinely returned");
        return 0;
    }
    if (message != WM_CLOSE) return realWindow(window, message, wp, lp);
    ++closeMessages;
    if (closeMessages == 1) {
        Check(phase == Phase::ClosePosted && SceneSaveInProgress() && !SceneSaveBlocksEdits() &&
              Signaled(entered) && !Signaled(releaseWrite) && !completions && BeforePublication(),
              "actual MainWndProc receives WM_CLOSE while the frozen save is genuinely in progress");
        StillOpen(); Event("initial-WM_CLOSE-received save-in-progress=1");
    } else {
        Check(closeMessages == 2 && phase == (saveCloseFailureMode ? Phase::RetryClosePosted : Phase::Completed) &&
              completions == (saveCloseFailureMode ? 2u : 1u) &&
              !SceneSaveInProgress() && !SceneSaveBlocksEdits() && !app->SceneModified(),
              "only production's completed-save close replay may reach window destruction");
        StillOpen(); Owner(true);
        Check(KillTimer(hostWindow, pollTimer) != FALSE, "stop only the fixture polling timer before normal destruction");
        phase = Phase::Closing;
        Event(saveCloseFailureMode ? "fresh-clean-WM_CLOSE after-failed-close-and-retry=1" : "production-WM_CLOSE-replay save-completed=1");
    }
    insideClose = true;
    const auto result = realWindow(window, message, wp, lp);
    insideClose = false;
    if (closeMessages == 1) {
        Check(deferrals == 1 && IsWindow(window) && SceneSaveInProgress() && !completions,
              "real first close returns with the window alive and its save pending");
        phase = Phase::AwaitCheckpoint;
        Check(PostMessageW(window, checkpointMessage, 0, 0) != FALSE, "queue a separate UI turn to prove close deferral");
    }
    return result;
}
void BeforeQuit() {
    Check(phase == Phase::Closing && closeMessages == 2 && deferrals == 2 &&
          completions == (saveCloseFailureMode ? 2u : 1u) && !SceneSaveInProgress(),
          "real quit follows completed save and both genuine close decisions");
    Event("normal-quit-after-save");
}
void AfterCacheShutdown() {
    Check(phase == Phase::Closing && completions == (saveCloseFailureMode ? 2u : 1u) &&
          !diskBytes.empty() && Bytes(destination) == diskBytes,
          "actual completed PMM remains unchanged through native cleanup and final cache shutdown");
    Event("cache-shutdown-after-completed-save PMM-retained=1");
}
void Finish() {
    Check(closeMessages == 2 && deferrals == 2 && completions == (saveCloseFailureMode ? 2u : 1u) &&
          event == (saveCloseFailureMode ? 12u : 8u) && failureLock == INVALID_HANDLE_VALUE &&
          phase == Phase::Closing && cacheStopped,
          "save-close case ended with exactly one completion and production shutdown");
    for (HANDLE handle : {entered, releaseWrite, written, releaseCompletion})
        Check(CloseHandle(handle) != FALSE, "release fixture events after production has joined its writer");
    entered = releaseWrite = written = releaseCompletion = nullptr;
}
} // namespace save_close

void ProduceTexture(MMDApp& value) {
    // The source is ordinary TGA data; only the production loader writes MTC.
    std::vector<unsigned char> tga(18 + 37 * 25 * 4, 0);
    tga[2] = 2; tga[12] = 37; tga[14] = 25; tga[16] = 32; tga[17] = 0x28;
    for (std::size_t i = 18; i < tga.size(); i += 4) {
        tga[i] = static_cast<unsigned char>(i); tga[i + 1] = 54;
        tga[i + 2] = 97; tga[i + 3] = 127;
    }
    const HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create isolated lifecycle TGA source");
    DWORD written = 0;
    const bool wrote = WriteFile(file, tga.data(), static_cast<DWORD>(tga.size()), &written, nullptr) &&
                       written == tga.size();
    Check(CloseHandle(file) != FALSE && wrote, "write and close actual TGA source");
    const auto before = texture_prepare_cache::GetStatistics();
    auto mutablePath = source.wstring();
    Check(LoadTextureShared(reinterpret_cast<unsigned char*>(value.Renderer()), mutablePath.data()) == 1,
          "production LoadTextureShared uses the real initialized host renderer");
    const auto after = texture_prepare_cache::GetStatistics();
    Check(after.stores == before.stores + 1 && after.nativeCalls > before.nativeCalls,
          "real cold texture decode commits one cache publication, not just GPU success");
    unsigned files = 0;
    Check(fs::exists(cacheRoot / L"Textures"), "producer created its isolated texture cache");
    for (const auto& entry : fs::recursive_directory_iterator(cacheRoot / L"Textures")) {
        if (!entry.is_regular_file() || entry.path().extension() != L".mtc") continue;
        ++files; payload = entry.path();
    }
    Check(files == 1 && cache_manager::Managed(payload), "exactly one real MTC in the fresh managed root");
    publishedBytes = Bytes(payload);
    std::uint32_t magic = 0, version = 0;
    Check(publishedBytes.size() > 76, "published MTC has header and decoded texture data");
    std::memcpy(&magic, publishedBytes.data(), 4); std::memcpy(&version, publishedBytes.data() + 4, 4);
    Check(magic == 0x4354504d && version == 2, "real producer wrote the recognized MTC format");
    Check(LeaseProbe() == ERROR_SHARING_VIOLATION, "real producer acquired a local cache use lease");
    // Inspect only after the real producer established disk admission.
    const auto view = cache_manager::Inspect();
    Check(view.error.empty() && view.textureBytes >= publishedBytes.size() && view.projects.empty(),
          "actual anonymous startup use is registered without an invented project owner");
    produced = true;
    std::printf("APP_LIFECYCLE producer stores_delta=1 leased=1 payload=%ls\n", payload.c_str());
}

bool Initialize(MMDApp* value, void* instance, int show) {
    Check(++initCalls == 1 && !initialized && !produced && value == g_Block && show == SW_HIDE,
          "real WinMain enters actual hidden initialization exactly once");
    app = value; // Borrow only. WinMain creates and destroys this app.
    Boundary();
    initialToken = cache_manager::CaptureActiveScene();
    Check(bool(initialToken) && !*value->EnvFileName() && !value->Renderer() && !value->Physics(),
          "WinMain established its anonymous active cache scene before any device initialization");
    Check(!fs::exists(cacheRoot), "startup token check did not initialize the disk cache");
    const bool result = realInitialize(value, instance, show);
    Check(result, "actual hidden initialization succeeded; no fallback or fake result");
    hostWindow = value->MainWindow();
    Check(hostWindow && value->Renderer() && value->Renderer()->device && value->Physics() &&
          value->Physics()->world && value->Physics()->groundBody,
          "actual startup supplies the hidden window, D3D device and Bullet world");
    Check(!GetModuleHandleW(L"MMHack.dll") && !GetModuleHandleW(L"MMEffect.dll") &&
          !GetModuleHandleW(L"MMSAdapter.dll") && !GetModuleHandleW(L"MMV4MMS.dll"),
          "fresh lifecycle host did not load MME bridges or MMV");
    D3DDEVICE_CREATION_PARAMETERS creation{};
    Check(SUCCEEDED(value->Renderer()->device->GetCreationParameters(&creation)) &&
          creation.DeviceType == D3DDEVTYPE_HAL, "lifecycle fixture requires real HAL, never REF or CPU fallback");
    Boundary();
    initialized = true;
    auto settings = autosave::Current();
    settings.enabled = settings.keepPrevious = settings.numberedManualVersions = false;
    std::wstring error;
    Check(autosave::Configure(value, settings, error), "disable periodic saves only in the staged lifecycle profile");
    ProduceTexture(*value);
    Check(!value->SceneModified() && !value->EnhancedModelDirty() && !*value->EnvFileName(),
          "texture-only stimulus leaves the initial scene clean and anonymous");
    Check(!value->AviFile() && !value->AviStream() && !value->AviFrameReader() && !SceneSaveInProgress(),
          "startup has no video resources or preexisting background save");
    return result;
}

bool InitializeUpdates(MMDApp* value) {
    Check(++updateCalls == 1 && initialized && produced && value == app && !closeRequested,
          "WinMain reaches its real update coordinator after initialization and texture production");
    const bool result = realUpdates(value);
    Check(result, "actual update coordinator initializes successfully");
    Boundary();
    if (saveCloseMode) {
        save_close::Start(value);
        return result;
    }
    if (videoCloseMode) video_close::Start(value);
    closeRequested = true;
    Check(PostMessageW(hostWindow, WM_CLOSE, 0, 0) != FALSE,
          "request ordinary close through WinMain's real message loop");
    return result;
}

void WINAPI Quit(int code) {
    if (saveCloseMode) save_close::BeforeQuit();
    Check(++quitCalls == 1 && closeRequested && produced && !cleanupCalls && code == 0,
          "normal window destruction posts successful quit before production cleanup");
    realPostQuit(code);
}

void Cleanup(MMDApp* value) {
    if (saveCloseMode)
        Check(save_close::completions == (saveCloseFailureMode ? 2u : 1u) && save_close::phase == save_close::Phase::Closing,
              "production cleanup cannot precede actual save completion and close replay");
    Check(++cleanupCalls == 1 && quitCalls == 1 && value == app && value == g_Block &&
          !IsWindow(hostWindow) && !cacheShutdownCalls, "WinMain cleans up only after normal window destruction");
    Boundary();
    Check(cache_manager::CaptureActiveScene() && LeaseProbe() == ERROR_SHARING_VIOLATION,
          "cache scene and use lease remain alive at production cleanup entry");
    realCleanup(value);
    Check(!value->Renderer() && !value->Physics() && !SceneSaveInProgress() && !updates::Running(),
          "real ShutdownCleanup returned after stopping host work and releasing devices");
    Check(cache_manager::CaptureActiveScene() && LeaseProbe() == ERROR_SHARING_VIOLATION,
          "producer cleanup did not prematurely retire the cache scene or its lease");
    cleaned = true;
    Boundary();
    std::puts("APP_LIFECYCLE cleanup-returned=1 cache-lease-still-held=1");
}

void CacheShutdown() noexcept {
    Check(++cacheShutdownCalls == 1 && cleaned && cleanupCalls == 1 && !cacheStopped,
          "real WinMain invokes cache Shutdown strictly after ShutdownCleanup returns");
    Check(LeaseProbe() == ERROR_SHARING_VIOLATION, "cache Shutdown owns the final use-lease retirement");
    if (videoCloseMode) video_close::BeforeCacheShutdown();
    realCacheShutdown();
    Check(!cache_manager::CaptureActiveScene() &&
          !cache_manager::BindSavedScene(initialToken, (root / L"never-saved.pmm").c_str()),
          "production cache Shutdown invalidates the original startup token");
    Check(LeaseProbe() == ERROR_SUCCESS && fs::exists(payload) && Bytes(payload) == publishedBytes,
          "production cache Shutdown releases local lease while preserving published bytes");
    cacheStopped = true;
    if (saveCloseMode) save_close::AfterCacheShutdown();
    std::puts("APP_LIFECYCLE cache-shutdown-returned=1 token-retired=1 lease-released=1 payload-retained=1");
}

int Run(int argc) {
    const auto* submode = _wgetenv(L"MMS_PMM_APP_LIFECYCLE_CASE");
    if (submode && !std::wcscmp(submode, L"save-close")) saveCloseMode = true;
    else if (submode && !std::wcscmp(submode, L"save-close-failure")) saveCloseMode = saveCloseFailureMode = true;
    else if (submode && !std::wcscmp(submode, L"video-close-mf")) videoCloseMode = true;
    else if (submode && std::wcscmp(submode, L"normal-exit"))
        Check(false, "lifecycle submode must be normal-exit, save-close, save-close-failure or video-close-mf");
    Check(argc == 1 && !g_Block && !app, "lifecycle mode accepts no scene arguments or preexisting application");
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    Check(length && length < _countof(executable), "resolve lifecycle staged executable");
    // Reuse only the fresh-directory/environment guard and UI confinement,
    // never the Reset cases, CPU host, manual app allocation or cleanup.
    PrepareCacheResetHost(fs::path(executable));
    const auto installation = fs::path(executable).parent_path();
    Check(!fs::exists(installation / L"data") &&
          system_settings::Path() == installation / L"data" / L"settings.json",
          "lifecycle settings belong solely to the unused staged installation");
    root = installation / L"data" / L"pmm-app-lifecycle";
    Check(root.native().size() + 48 < 256 && fs::create_directories(root), "create fresh lifecycle fixture directory");
    cacheRoot = root / L"Cache"; source = root / L"lifecycle-source.tga";
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR", cacheRoot.c_str()) &&
          SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS", (root / L"cache-settings.json").c_str()) &&
          SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR", (cacheRoot / L"Textures").c_str()) &&
          SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", (cacheRoot / L"MME").c_str()) &&
          SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", L"0") &&
          SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE", L"0") &&
          SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1") &&
          SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", (root / L"recent.txt").c_str()) &&
          SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", (root / L"autosave.ini").c_str()) &&
          SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX", L"0") &&
          SetEnvironmentVariableW(L"MMS_UI_METRICS", nullptr) &&
          _putenv_s("MIKUMIKUSTUDIO_PUMP_STATS", "") == 0,
          "isolate startup settings and all cache roots; enable only the real texture producer");
    thread = GetCurrentThreadId();
    Check(MH_Initialize() == MH_OK, "initialize lifecycle forwarding observers");
    Hook(&ShowWindow, &Hidden, reinterpret_cast<void**>(&realShow));
    Hook(&SetWindowPos, &HiddenPosition, reinterpret_cast<void**>(&realSetWindowPos));
    Hook(&SetForegroundWindow, &host_cache_save::DenyForeground);
    Hook(&SetActiveWindow, &host_cache_save::DenyActivation);
    Hook(&SetWindowPlacement, &host_cache_save::HiddenPlacement,
         reinterpret_cast<void**>(&host_cache_save::realPlacement));
    Hook(&MessageBoxW, &UnexpectedW); Hook(&MessageBoxA, &UnexpectedA);
    Hook(&InitMainWindowAndD3D, &Initialize, reinterpret_cast<void**>(&realInitialize));
    Hook(&updates::Initialize, &InitializeUpdates, reinterpret_cast<void**>(&realUpdates));
    Hook(&PostQuitMessage, &Quit, reinterpret_cast<void**>(&realPostQuit));
    Hook(&ShutdownCleanup, &Cleanup, reinterpret_cast<void**>(&realCleanup));
    Hook(&cache_manager::Shutdown, &CacheShutdown, reinterpret_cast<void**>(&realCacheShutdown));
    if (saveCloseMode) {
        Hook(&MainWndProc, &save_close::Window, reinterpret_cast<void**>(&save_close::realWindow));
        Hook(&background_save::DeferClose, &save_close::Defer, reinterpret_cast<void**>(&save_close::realDefer));
    }
    if (videoCloseMode) video_close::Hooks();
    const auto hide = SetWindowsHookExW(WH_CBT, NoActivation, nullptr, thread);
    Check(hide != nullptr, "prevent lifecycle window creation from showing or activating top-level windows");
    Boundary();
    if (videoCloseMode)
        std::puts("STAGE APP_LIFECYCLE real WinMain; case=video-close-mf; actual LoadAviFile worker and cache shutdown");
    else if (saveCloseFailureMode)
        std::puts("STAGE APP_LIFECYCLE real WinMain; case=save-close-failure; real locked PMM, error notification and retry; video pending");
    else if (saveCloseMode)
        std::puts("STAGE APP_LIFECYCLE real WinMain; case=save-close; real frozen save and deferred WM_CLOSE; video pending");
    else
        std::puts("STAGE APP_LIFECYCLE real WinMain; texture producer; normal WM_CLOSE; no video/save-gate coverage");
    char command[] = "";
    const int result = ::WinMain(GetModuleHandleW(nullptr), nullptr, command, SW_HIDE);
    // app is only a borrowed pointer and is now dangling; never dereference it.
    app = nullptr;
    Check(result == 0 && !g_Block && initialized && produced && closeRequested && cleaned && cacheStopped &&
          initCalls == 1 && updateCalls == 1 && quitCalls == 1 && cleanupCalls == 1 && cacheShutdownCalls == 1,
          "actual WinMain returned after exactly one complete native startup and exit");
    Boundary();
    if (saveCloseMode) save_close::Finish();
    if (videoCloseMode) video_close::Finish();
    Check(UnhookWindowsHookEx(hide) != FALSE, "remove lifecycle thread visibility guard");
    Check(MH_DisableHook(MH_ALL_HOOKS) == MH_OK && MH_Uninitialize() == MH_OK, "remove lifecycle observers after WinMain returns");
    if (videoCloseMode)
        std::printf("PASS actual WinMain cache lifecycle: %u checks; scope=texture+legacy-path-MF-video-close; unified-video-source and FFmpeg retirement pending\n", checks);
    else if (saveCloseFailureMode)
        std::printf("PASS actual WinMain cache lifecycle: %u checks; scope=texture+background-save-close-failure-retry; video ordering pending\n", checks);
    else if (saveCloseMode)
        std::printf("PASS actual WinMain cache lifecycle: %u checks; scope=texture+background-save-close; video ordering pending\n", checks);
    else
        std::printf("PASS actual WinMain cache lifecycle: %u checks; scope=texture+normal-exit; background-save/video ordering pending\n", checks);
    return result;
}
} // namespace host_app_lifecycle
#endif

int RunAppLifecycle(int argc) {
#if !MMD_ORIGINAL_PROFILE
    return host_app_lifecycle::Run(argc);
#else
    (void)argc;
    Check(false, "actual app cache lifecycle requires the full profile");
    return 1;
#endif
}
