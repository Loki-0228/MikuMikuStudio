// Explicit hidden integration test: real Save/Save As and both export dialogs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/save_naming.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "../src/window/image_sequence_dialog.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/project_storage.hpp"
#endif
#include <MinHook.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <utility>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace seq = mikumikustudio::image_sequence;
namespace mikumikustudio { INT_PTR __stdcall AviOutDlgProc(HWND, UINT, WPARAM, LPARAM); }
namespace {
unsigned checks = 0, picks = 0, errors = 0;
bool cancelPick = false, expectError = false, sequenceDialog = false;
fs::path root, history, chosen;
std::function<void()> pickAction;
std::function<void(HWND, LPCWSTR)> messageAction;
bool cacheSaveChecks = false;
std::uint64_t firstPayloadBytes = 0, replacementPayloadBytes = 0;
HWND expectedOwner = nullptr;
void Check(bool ok, const char* reason) {
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", reason); std::exit(1); }
}
void IsolateSaveCache() {
    const auto dir = root / L"save-cache";
    Check(fs::create_directory(dir), "require a new private cache fixture before host initialization");
    const auto managed = dir / L"Cache";
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR", managed.c_str()) &&
          SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS", (dir / L"settings.json").c_str()) &&
          SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", (managed / L"MME").c_str()) &&
          SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR", (managed / L"Textures").c_str()) &&
          SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", L"0") &&
          SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE", L"1") &&
          SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1"),
          "isolate the cache manager and exclude unrelated host texture and effect traffic");
    Check(cache_manager::Root() == managed, "cache root resolves to the fresh fixture directory");
}
void VerifySaveDesktop() {
    wchar_t expected[128]{}, actual[128]{};
    const DWORD length = GetEnvironmentVariableW(L"MMS_CACHE_SAVE_TEST_DESKTOP", expected, 128);
    const auto desktop = GetThreadDesktop(GetCurrentThreadId());
    DWORD needed = 0;
    BOOL receivesInput = TRUE;
    Check(length && length < 128 && std::wstring(expected).rfind(L"MMSCacheSave-", 0) == 0 &&
          desktop && GetUserObjectInformationW(desktop, UOI_NAME, actual, sizeof(actual), &needed) &&
          std::wcscmp(expected, actual) == 0 &&
          GetUserObjectInformationW(desktop, UOI_IO, &receivesInput, sizeof(receivesInput), &needed) && !receivesInput,
          "save host entered its launcher's private non-input desktop");
    std::printf("SAVE_TEST private-desktop=%ls non-input=1\n", actual);
}
std::uint64_t PublishSavePayload(wchar_t digit, unsigned bytes) {
    const auto path = cache_manager::Root() / L"Textures" / L"shared-texture" / (std::wstring(64,digit) + L".mtc");
    fs::create_directories(path.parent_path());
    Check(cache_manager::Managed(path), "save fixture payload is inside managed storage");
    cache_manager::WriteReservation reservation(path, 1024*1024);
    Check(bool(reservation), "reserve managed save fixture bytes");
    const fs::path tmp = path.wstring() + L".tmp-save";
    const HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE && reservation.TrackTemporary(tmp), "track the exact empty save fixture file");
    std::vector<std::uint32_t> data(bytes/4);
    data[0] = 0x4354504d; data[1] = 2; data[2] = bytes - 76;
    DWORD written = 0;
    Check(WriteFile(file, data.data(), bytes, &written, nullptr) && written == bytes, "write recognized texture payload");
    Check(CloseHandle(file) != FALSE, "close staged save fixture");
    Check(MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) && reservation.Commit(),
          "publish and register the save fixture through the cache manager");
    const HANDLE published = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILE_STANDARD_INFO info{};
    Check(published != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(published, FileStandardInfo, &info, sizeof(info)) &&
          info.EndOfFile.QuadPart == bytes && CloseHandle(published), "measure the published payload charge");
    return static_cast<std::uint64_t>((std::max)(info.AllocationSize.QuadPart, info.EndOfFile.QuadPart));
}
void CheckSaveOwner(const char* phase, const fs::path& expected, std::uint64_t bytes,
                    const fs::path& forbidden = {}) {
    if (!cacheSaveChecks) return;
    const auto view = cache_manager::Inspect();
    Check(view.error.empty(), "read healthy cache ownership after save operation");
    unsigned current = 0;
    for (const auto& row : view.projects) {
        if (!forbidden.empty() && !_wcsicmp(row.path.c_str(), forbidden.c_str())) {
            std::fwprintf(stderr, L"unexpected cache project: %ls\n", row.path.c_str());
            Check(false, phase);
        }
        if (!row.current) continue;
        ++current;
        if (expected.empty() || _wcsicmp(row.path.c_str(), expected.c_str()) || row.totalBytes != bytes) {
            std::fwprintf(stderr, L"cache owner=%ls bytes=%llu expected=%ls bytes=%llu\n", row.path.c_str(),
                          static_cast<unsigned long long>(row.totalBytes), expected.c_str(), static_cast<unsigned long long>(bytes));
            Check(false, phase);
        }
    }
    Check(current == (expected.empty() ? 0u : 1u), phase);
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
int WINAPI Message(HWND owner, LPCWSTR text, LPCWSTR, UINT) {
    Check(expectError, "only expected save failures display a message");
    ++errors;
    std::wprintf(L"Expected save failure: %ls\n", text);
    if (auto action = std::exchange(messageAction, nullptr)) action(owner, text);
    return IDOK;
}
BOOL WINAPI NoBeep(UINT) { return TRUE; }
BOOL Pick(LPOPENFILENAMEW ofn) {
    ++picks;
    Check(ofn->hwndOwner == expectedOwner, "Save As belongs to the active dialog");
    if (cancelPick) return FALSE;
    if (auto action = std::exchange(pickAction, nullptr)) action();
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
    if (cacheSaveChecks) {
        Check(ResetAppState(app) && cache_manager::CaptureActiveScene(), "real reset establishes an active anonymous scene");
        firstPayloadBytes = PublishSavePayload(L'1', 64*1024);
        Check(cache_manager::Inspect().textureBytes == firstPayloadBytes, "independently charged payload is registered before first save");
        CheckSaveOwner("unnamed scene has no current cache project", {}, 0);
    }
    app->EnvFileName()[0] = 0; app->SceneModified() = 1;
    chosen = first;
    SaveCommand(app, 207);
    Check(fs::is_regular_file(first) && !app->SceneModified(), "Save creates the unnamed project");
    Check(Recent() == std::vector<std::wstring>{first.wstring()}, "first Save enters recent projects");
    CheckSaveOwner("first background save binds the anonymous scene's bytes", first, firstPayloadBytes);
    chosen = second; app->SceneModified() = 1;
    SaveCommand(app, 208);
    Check(fs::is_regular_file(second) && std::wstring(app->EnvFileName()) == second.wstring(), "Save As retargets to a saved project");
    Check(Recent() == std::vector<std::wstring>{second.wstring(), first.wstring()}, "Save As enters history first");
    CheckSaveOwner("background Save As rebinds the actual payload", second, firstPayloadBytes);
    chosen = first;
    SaveCommand(app, 208);
    SaveCommand(app, 207);
    const auto savedHistory = Recent();
    Check(savedHistory == std::vector<std::wstring>{first.wstring(), second.wstring()}, "repeated Save deduplicates history");
    Check(SaveSceneCopy(app, (root / L"自动备份.pmm").c_str()) && Recent() == savedHistory,
        "automatic copies never enter recent projects");
    CheckSaveOwner("synchronous backup copy creates no cache owner", first, firstPayloadBytes, root / L"自动备份.pmm");
    cancelPick = true;
    SaveCommand(app, 208);
    cancelPick = false;
    Check(Recent() == savedHistory && std::wstring(app->EnvFileName()) == first.wstring(), "cancel preserves history and current project");
    CheckSaveOwner("cancelled Save As keeps cache ownership", first, firstPayloadBytes);
    chosen = root / L"missing" / L"failure.pmm"; expectError = true;
    const auto before = errors;
    SaveCommand(app, 208);
    expectError = false;
    Check(errors == before + 1 && Recent() == savedHistory && std::wstring(app->EnvFileName()) == first.wstring(),
        "failed Save As cannot retarget the project or enter history");
    CheckSaveOwner("failed Save As creates no candidate cache owner", first, firstPayloadBytes, chosen);
}
std::vector<char> Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void SaveFailureThenSaveAsTest(MMDApp* app) {
    const auto current = root / L"active.pmm";
    const auto firstVersion = fs::path(current.native() + L"1");
    const auto secondVersion = fs::path(current.native() + L"11");
    const auto recovered = root / L"recovered.pmm";
    const HWND window = app->MainWindow();
    expectedOwner = window;
    app->EnglishUI() = kUiEnglish;
    autosave::Initialize(app);
    auto settings = autosave::Current();
    settings.enabled = false;
    settings.keepPrevious = true;
    settings.numberedManualVersions = false;
    std::wstring settingsError;
    Check(autosave::Configure(app, settings, settingsError), "enable old-version retention for the owned project");

    const auto edit = [&](float fov) {
        app->CameraFov() = fov;
        app->SceneModified() = 1;
    };
    chosen = current;
    for (const float fov : {37.f, 53.f, 61.f}) {
        edit(fov);
        SaveCommand(app, 207);
        Check(!app->SceneModified() && fs::path(app->EnvFileName()) == current,
              "normal File Save seeds the current project and retained versions");
    }
    const auto historyBefore = Recent();
    const auto activeDirectory = std::wstring(app->PathWorkspace().projectDirectory);
    const auto generation = app->SceneGeneration();
    std::vector<std::pair<fs::path, std::vector<char>>> members;
    for (const auto& path : {current, firstVersion, secondVersion}) {
        auto bytes = Bytes(path);
        Check(!bytes.empty(), "each retained project generation exists on disk");
        members.emplace_back(path, std::move(bytes));
#if !MMD_ORIGINAL_PROFILE
        const auto companion = plugins::PluginSidecar(path);
        bytes = Bytes(companion);
        Check(!bytes.empty() && plugins::ReadPluginProject(path).mainHash == plugins::FileHash(path),
              "each retained project has its matching plugin companion");
        members.emplace_back(companion, std::move(bytes));
#endif
    }
    const auto checkRetained = [&] {
        for (const auto& [path, bytes] : members)
            Check(Bytes(path) == bytes, "preserve the bytes of each active and archived member");
        Check(!fs::exists(current.native() + L".mms-save.json"), "leave no pending rollback for the old project");
    };

    const HANDLE locked = CreateFileW(current.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "let the writer read the project while Windows blocks replacing it");
    edit(79.f);
    const auto errorsBefore = errors;
    bool explained = false;
    messageAction = [&](HWND owner, LPCWSTR message) {
        explained = owner == window && std::wstring_view(message).find(L"write access") != std::wstring_view::npos;
    };
    expectError = true;
    SendMessageW(window, WM_COMMAND, 207, 0);
    Check(SceneSaveInProgress() && !SceneSaveBlocksEdits(), "ordinary File Save starts the background writer");
    FinishWrites(app);
    expectError = false;
    messageAction = {};
    Check(errors == errorsBefore + 1 && explained &&
          std::wstring_view(background_save::StatusText(app)) == L"Save failed. Check the destination.",
          "failed background Save reports the error once and sets the failure banner");
    Check(IsWindow(window) && app->SceneGeneration() == generation &&
          fs::path(app->EnvFileName()) == current && app->CameraFov() == 79.f && app->SceneModified() &&
          activeDirectory == app->PathWorkspace().projectDirectory && Recent() == historyBefore,
          "failure keeps the original project selected and preserves the unsaved edit and recent projects");
    checkRetained();

    const auto picksBefore = picks;
    chosen = recovered;
    SaveCommand(app, 208);
    auto expectedRecent = historyBefore;
    if (!build_profile::original) expectedRecent.insert(expectedRecent.begin(), recovered.native());
    Check(picks == picksBefore + 1 && errors == errorsBefore + 1 &&
          fs::path(app->EnvFileName()) == recovered && app->SceneGeneration() == generation &&
          !app->SceneModified() && fs::is_regular_file(recovered) &&
          Recent() == expectedRecent &&
          std::wstring_view(background_save::StatusText(app)) == L"Saved",
          "ordinary File Save As saves the retained edit to a new destination while the original is still locked");
#if !MMD_ORIGINAL_PROFILE
    Check(plugins::ReadPluginProject(recovered).mainHash == plugins::FileHash(recovered),
          "the recovered project has a matching plugin companion");
#endif
    checkRetained();
    Check(CloseHandle(locked), "release only the owned project fixture handle");

    for (const auto& [path, fov] : std::array<std::pair<fs::path, float>, 4>{{
             {recovered, 79.f}, {current, 61.f}, {firstVersion, 53.f}, {secondVersion, 37.f}}}) {
        Check(LoadSceneFile(path.c_str()) && fs::path(app->EnvFileName()) == path &&
              app->CameraFov() == fov && !app->SceneModified(),
              "reopen the real recovered project and every previous generation with the expected edit");
        std::printf("SAVE-REOPEN %ls fov=%.0f\n", path.filename().c_str(), fov);
    }
    checkRetained();
}

void SaveAsGenerationTests(MMDApp* app) {
    const auto dir = root / L"save-as-generation";
    Check(fs::create_directory(dir), "create private Save As generation fixtures");
    const auto original = dir / L"original.pmm";
    const auto replacement = dir / L"replacement.pmm";
    const auto stale = dir / L"stale-destination.pmm";
    const auto valid = dir / L"valid-destination.pmm";
    Check(ResetAppState(app), "initialize a scene for the Save As generation fixtures");
    app->CameraFov() = 37;
    Check(SaveSceneCopy(app, original.c_str()), "write original project fixture");
    app->CameraFov() = 71;
    Check(SaveSceneCopy(app, replacement.c_str()), "write replacement project fixture");
    Check(LoadSceneFile(original.c_str()) && std::wstring(app->EnvFileName()) == original.wstring() &&
        app->CameraFov() == 37, "open original project before invoking Save As");
    const auto originalGeneration = app->SceneGeneration();

    const std::vector<char> sentinel{'S', 'A', 'V', 'E', '\0', '\r', '\n'};
    std::ofstream(stale, std::ios::binary).write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
    Check(Bytes(stale) == sentinel, "seed existing stale destination with sentinel bytes");
    const bool recordsRecentProjects = !build_profile::original;
    std::vector<std::wstring> expectedRecent;
    if (recordsRecentProjects) expectedRecent = {replacement.wstring(), original.wstring()};
    std::vector<char> replacementHistory;
    expectedOwner = app->MainWindow();
    chosen = stale;
    {
        struct ClearPickAction {
            ~ClearPickAction() { pickAction = nullptr; }
        } clearPickAction;
        // Model a project open dispatched by the modal picker before it returns its selection.
        pickAction = [&] {
            Check(LoadSceneFile(replacement.c_str()) && app->SceneGeneration() != originalGeneration &&
                std::wstring(app->EnvFileName()) == replacement.wstring() && app->CameraFov() == 71,
                "open replacement and change scene generation while Save As is active");
            app->CameraFov() = 79;
            app->SceneModified() = 1;
            Check(fs::exists(history) == recordsRecentProjects && Recent() == expectedRecent,
                "replacement open has the expected recent projects before the picker returns");
            replacementHistory = Bytes(history);
        };
        const auto before = picks;
        SaveCommand(app, 208);
        Check(!pickAction && picks == before + 1, "run the replacement open in exactly one Save As picker");
    }
    Check(Bytes(stale) == sentinel, "stale Save As leaves existing destination byte-exact");
    Check(std::wstring(app->EnvFileName()) == replacement.wstring() && app->SceneModified() &&
        app->CameraFov() == 79, "stale Save As preserves the replacement path and unsaved edit");
    Check(fs::exists(history) == recordsRecentProjects && Bytes(history) == replacementHistory,
        "stale Save As preserves the replacement scene's recent project history");

    chosen = valid;
    const auto before = picks;
    SaveCommand(app, 208);
    Check(picks == before + 1 && fs::is_regular_file(valid) &&
        std::wstring(app->EnvFileName()) == valid.wstring() && !app->SceneModified(),
        "fresh Save As publishes and cleans the replacement scene");
    if (recordsRecentProjects) expectedRecent.insert(expectedRecent.begin(), valid.wstring());
    Check(fs::exists(history) == recordsRecentProjects && Recent() == expectedRecent,
        "fresh Save As has the expected recent projects for this profile");
    Check(LoadSceneFile(valid.c_str()) && app->CameraFov() == 79,
        "fresh Save As contains the edited replacement scene on disk");
}
#include "pmm_versions_checks.hpp"
WNDPROC realMain = nullptr;
unsigned deferredCloses = 0;
LRESULT CALLBACK WatchClose(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
#if !MMD_ORIGINAL_PROFILE
    if (msg == kSceneCloseReplayMessage) { ++deferredCloses; return 0; }
#endif
    if (msg == WM_CLOSE && !SceneSaveInProgress()) { ++deferredCloses; return 0; }
    return CallWindowProcW(realMain, window, msg, wp, lp);
}
void SaveCaptureCloseTest(MMDApp* app, bool synchronous) {
    Check(ResetAppState(app), "initialize scene for capture-close regression");
    app->EnglishUI() = 1;
    const HWND window = app->MainWindow();
    const auto stem = std::wstring(synchronous ? L"capture-close-sync" : L"capture-close-async");
    const auto sentinel = root / (stem + L"-sentinel.pmm");
    const auto later = root / (stem + L"-later.pmm");
    app->CameraFov() = 37;
    Check(wcscpy_s(app->EnvFileName(), kPathFieldCapacity, sentinel.c_str()) == 0 && SaveSceneFile(app),
        "write valid sentinel PMM before the rejected save");
    const auto sentinelBytes = Bytes(sentinel);
    Check(!sentinelBytes.empty(), "read the original sentinel PMM bytes");

    deferredCloses = 0;
    realMain = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(WatchClose)));
    Check(realMain != nullptr, "install capture-close window observer");
    const auto drainCloses = [&] {
#if !MMD_ORIGINAL_PROFILE
        RetryDeferredSceneCloses(*app);
#endif
        MSG msg{};
        while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        return deferredCloses;
    };

    // U+65E5 occupies two bytes in CP932. The path fits WavePath's wide
    // buffer, but the serialized path exceeds the PMM field's 255 bytes.
    const auto tooLong = std::wstring(200, L'\u65e5') + L".wav";
    Check(wcscpy_s(app->WavePath(), kPathFieldCapacity, tooLong.c_str()) == 0,
        "set a PMM reference path that exceeds the encoded field size");
    app->CameraFov() = 71;
    app->SceneModified() = 1;
    messageAction = [&](HWND owner, LPCWSTR text) {
        const wchar_t* diagnostic = build_profile::original ? L"cannot fit an original MMD CP932 field exactly"
                                                            : L"Path exceeds the PMM field size.";
        Check(owner == window && std::wstring(text).find(diagnostic) != std::wstring::npos,
            "observe the production path-encoding refusal dialog");
        Check(SceneSaveBlocksEdits() && SceneSaveInProgress(), "send close while snapshot capture holds both save gates");
        SendMessageW(window, WM_CLOSE, 0, 0);
        Check(IsWindow(window) && deferredCloses == 0, "capture gate defers the close while the refusal dialog is active");
    };
    const auto beforeErrors = errors;
    expectError = true;
    const bool accepted = synchronous ? SaveSceneFile(app) : background_save::Request(app, sentinel.c_str());
    expectError = false;
    Check(!accepted, "manual save is rejected during snapshot capture");
    Check(!messageAction && errors == beforeErrors + 1, "one capture refusal delivered the only close request");
    Check(!SceneSaveInProgress() && !SceneSaveBlocksEdits(), "capture failure releases both save gates");
    Check(Bytes(sentinel) == sentinelBytes && std::wstring(app->EnvFileName()) == sentinel.wstring() &&
        app->SceneModified() && app->CameraFov() == 71, "capture failure preserves the original PMM and dirty scene");
    const auto closesAfterFailure = drainCloses();

    app->WavePath()[0] = 0;
    chosen = later;
    expectedOwner = window;
    const auto beforePicks = picks;
    SaveCommand(app, 208);
    Check(picks == beforePicks + 1 && fs::is_regular_file(later) &&
        std::wstring(app->EnvFileName()) == later.wstring() && !app->SceneModified(),
        "later Save As publishes and clears dirty state without a new close request");
    const auto closesAfterSave = drainCloses();
    app->CameraFov() = 37;
    Check(LoadSceneFile(later.c_str()) && app->CameraFov() == 71, "reopen the later Save As with the edited scene");
    const auto closesAfterReopen = drainCloses();

    // Exercise the successful save and reopen even when the first checkpoint observed a replay.
    Check(closesAfterFailure == 0, "capture failure must not replay close at the next safe point");
    Check(closesAfterSave == 0, "later successful save must not replay close from failed capture");
    Check(closesAfterReopen == 0, "reopen must not replay close from failed capture");
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(realMain));
}
void AsyncTests(MMDApp* app) {
    app->EnglishUI() = 2;
    const auto baseline = root / L"snapshot-baseline.pmm";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, baseline.c_str());
    app->SceneModified() = 1;
    Check(SaveSceneFile(app), "write synchronous format baseline");
    CheckSaveOwner("synchronous SaveSceneFile binds its actual destination", baseline, firstPayloadBytes);
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
    CheckSaveOwner("older snapshot binds its accepted path while the newer edit stays dirty", snapshotPath, firstPayloadBytes);
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
    if (cacheSaveChecks) {
        Check(ResetAppState(app) && cache_manager::CaptureActiveScene(), "reset replaces the scene while its old writer is gated");
        replacementPayloadBytes = PublishSavePayload(L'2', 128*1024);
        CheckSaveOwner("replacement is anonymous before old completion", {}, 0, oldScenePath);
    } else {
        app->AdvanceSceneGeneration();
    }
    app->EnvFileName()[0] = 0; app->SceneModified() = 1;
    SetEvent(release); FinishWrites(app);
    Check(fs::is_regular_file(oldScenePath) && !app->EnvFileName()[0] && app->SceneModified(),
        "old-scene completion cannot retarget or clean a replacement scene");
    CheckSaveOwner("stale worker completion leaves replacement bytes anonymous", {}, 0, oldScenePath);
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
    CheckSaveOwner("background backup preserves the saved project's historical and replacement bytes",
                   snapshotPath, firstPayloadBytes + replacementPayloadBytes, backups.front());
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
constexpr char kAuditMarkerKey[] = "org.mikumikustudio.test.backup-audit-02.marker";
std::vector<std::uint8_t> AuditBytes(const char* text) {
    return std::vector<std::uint8_t>(text, text + std::strlen(text));
}
std::string AuditRecord(const pmm_extension::Document& document, const char* key) {
    const auto* record = pmm_extension::Find(document, key);
    return record ? std::string(record->value.begin(), record->value.end()) : std::string();
}
// Holds the real disk worker at beforeWrite so the main thread can edit the
// live scene, then joins it through the ordinary completion path.
struct AuditWorkerGate {
    HANDLE entered = nullptr;
    HANDLE release = nullptr;
    DWORD mainThread = GetCurrentThreadId();
    std::atomic<DWORD> writer{0};
    unsigned completed = 0;
    bool success = false;
    background_save::Options options;
    AuditWorkerGate() {
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(entered && release, "create background-save audit gates");
        options.beforeWrite = [this] {
            writer.store(GetCurrentThreadId());
            SetEvent(entered);
            return WaitForSingleObject(release, 10000) == WAIT_OBJECT_0;
        };
        options.completed = [this](bool ok) { ++completed; success = ok; };
    }
    AuditWorkerGate(const AuditWorkerGate&) = delete;
    AuditWorkerGate& operator=(const AuditWorkerGate&) = delete;
    ~AuditWorkerGate() { Close(); }
    void Arm() {
        ResetEvent(entered); ResetEvent(release);
        writer.store(0); completed = 0; success = false;
    }
    void Block() {
        Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "worker reached the background-save audit gate");
        Check(writer.load() != mainThread, "background-save audit worker runs off the main thread");
    }
    void Continue() { SetEvent(release); }
    void Close() {
        if (entered) { CloseHandle(entered); entered = nullptr; }
        if (release) { CloseHandle(release); release = nullptr; }
    }
};
void BackupAudit02Tests(MMDApp* app) {
    const auto originalSettings = autosave::Current();
    const std::wstring previousPath = app->EnvFileName();
    const float previousFov = app->CameraFov();
    const auto previousProject = app->PmmProjectSettings();
    const std::uint8_t previousModified = app->SceneModified();
    auto settings = originalSettings;
    settings.enabled = false;
    settings.keepPrevious = false;
    settings.numberedManualVersions = true;
    settings.incrementPattern = L"[/NAME]_[/N]";
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "configure numbered manual saves for the audit regression");
    const auto sceneRoot = root / L"backup-audit-02";
    fs::create_directories(sceneRoot);
    Check(fs::is_directory(sceneRoot), "create an isolated folder for numbered audit saves");
    auto& document = app->PmmProjectSettings().document;
    pmm_extension::Erase(document, save_naming::BaseKey);
    pmm_extension::Erase(document, save_naming::FileKey);
    Check(!pmm_extension::Find(document, save_naming::BaseKey) &&
        !pmm_extension::Find(document, save_naming::FileKey), "audit scene starts without stale naming keys");
    AuditWorkerGate gate;

    // A completed save must carry the naming series forward while keeping the
    // edits made during its disk write.
    const auto firstTarget = sceneRoot / L"async-series.pmm";
    const auto firstSaved = sceneRoot / L"async-series_001.pmm";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, firstTarget.c_str());
    const float firstFov = app->CameraFov() + 4.0f;
    const std::string firstMarker = "audit-02-first-live-edit";
    gate.Arm();
    Check(background_save::Request(app, app->EnvFileName(), gate.options), "queue the first numbered save");
    gate.Block();
    Check(!fs::exists(firstTarget) && !fs::exists(firstSaved), "blocked numbered save has published nothing");
    app->CameraFov() = firstFov;
    app->SceneModified() = 1;
    pmm_extension::Set(document, kAuditMarkerKey, AuditBytes(firstMarker.c_str()));
    app->PmmProjectSettings().processorEdited = true;
    app->PmmProjectSettings().antialiasEdited = true;
    gate.Continue();
    FinishWrites(app);
    Check(gate.completed == 1 && gate.success, "first numbered save completes through the real worker");
    Check(fs::is_regular_file(firstSaved) && std::wstring(app->EnvFileName()) == firstSaved.wstring(),
        "first numbered save publishes its increment and becomes the active project");
    Check(AuditRecord(document, save_naming::BaseKey) == "async-series" &&
        AuditRecord(document, save_naming::FileKey) == "async-series_001.pmm",
        "first numbered save records the base and file keys");
    Check(app->CameraFov() == firstFov && app->SceneModified() &&
        AuditRecord(document, kAuditMarkerKey) == firstMarker,
        "first numbered save preserves live edits made during its disk write");
    Check(app->PmmProjectSettings().processorEdited && app->PmmProjectSettings().antialiasEdited,
        "first numbered save preserves live settings edits");

    // The second save must continue the same series instead of nesting a new
    // suffix around the already numbered filename.
    const auto firstBytes = Bytes(firstSaved);
    const auto secondSaved = sceneRoot / L"async-series_002.pmm";
    const auto nestedSaved = sceneRoot / L"async-series_001_001.pmm";
    const float secondFov = firstFov + 6.0f;
    const std::string secondMarker = "audit-02-second-live-edit";
    gate.Arm();
    Check(background_save::Request(app, app->EnvFileName(), gate.options), "queue the second numbered save from the committed increment");
    gate.Block();
    Check(!fs::exists(secondSaved) && !fs::exists(nestedSaved), "blocked second numbered save has published nothing");
    app->CameraFov() = secondFov;
    app->SceneModified() = 1;
    pmm_extension::Set(document, kAuditMarkerKey, AuditBytes(secondMarker.c_str()));
    gate.Continue();
    FinishWrites(app);
    Check(gate.completed == 1 && gate.success, "second numbered save completes through the real worker");
    Check(fs::is_regular_file(secondSaved) && std::wstring(app->EnvFileName()) == secondSaved.wstring(),
        "second numbered save continues the same series");
    Check(Bytes(firstSaved) == firstBytes, "second numbered save leaves the first increment unchanged");
    Check(!fs::exists(nestedSaved), "second numbered save does not nest a suffix around the increment");
    Check(AuditRecord(document, save_naming::BaseKey) == "async-series" &&
        AuditRecord(document, save_naming::FileKey) == "async-series_002.pmm",
        "second numbered save carries the series keys to the new increment");
    Check(app->CameraFov() == secondFov && app->SceneModified() &&
        AuditRecord(document, kAuditMarkerKey) == secondMarker,
        "second numbered save preserves later live edits");
    Check(app->PmmProjectSettings().processorEdited && app->PmmProjectSettings().antialiasEdited,
        "second numbered save preserves live settings edits");

    // A write failure after capture must not publish a candidate or touch the
    // live project identity, naming keys, extension data, or history.
    const std::wstring failedPath = app->EnvFileName();
    const std::string failedBase = "failed-live-base";
    const std::string failedFile = "failed-live-file.pmm";
    const auto failedHistory = Recent();
    const auto missingDirectory = sceneRoot / L"missing-parent";
    const auto missingTarget = missingDirectory / L"async-series.pmm";
    const auto missingCandidate = missingDirectory / L"async-series_001.pmm";
    const auto nextIncrement = sceneRoot / L"async-series_003.pmm";
    const float failedFov = secondFov + 2.0f;
    const std::string failedMarker = "audit-02-failed-live-edit";
    const auto errorsBeforeFailure = errors;
    expectError = true;
    gate.Arm();
    Check(background_save::Request(app, missingTarget.c_str(), gate.options), "queue a numbered save whose parent folder is absent");
    gate.Block();
    Check(!fs::exists(missingDirectory) && !fs::exists(missingCandidate) && !fs::exists(nextIncrement),
        "blocked failing save has published no candidate");
    // Both keys differ from the captured snapshot, so any completion that
    // synced naming keys without a published file would be caught here.
    pmm_extension::Set(document, save_naming::BaseKey, AuditBytes(failedBase.c_str()));
    pmm_extension::Set(document, save_naming::FileKey, AuditBytes(failedFile.c_str()));
    app->CameraFov() = failedFov;
    app->SceneModified() = 1;
    pmm_extension::Set(document, kAuditMarkerKey, AuditBytes(failedMarker.c_str()));
    gate.Continue();
    FinishWrites(app);
    expectError = false;
    Check(gate.completed == 1 && !gate.success, "write failure reaches the completion callback as a failure");
    Check(errors == errorsBeforeFailure + 1, "write failure reports one save error");
    Check(std::wstring(app->EnvFileName()) == failedPath &&
        AuditRecord(document, save_naming::BaseKey) == failedBase &&
        AuditRecord(document, save_naming::FileKey) == failedFile &&
        AuditRecord(document, kAuditMarkerKey) == failedMarker &&
        Recent() == failedHistory,
        "failed save keeps the live project identity, naming keys, extension data and history");
    Check(app->CameraFov() == failedFov && app->SceneModified(), "failed save keeps live edits dirty");
    Check(!fs::exists(missingDirectory), "failed save publishes no file or folder");

    // A completion that belongs to a replaced scene must not retarget the
    // replacement scene even though the captured snapshot published.
    const auto replacedHistory = Recent();
    const auto replacedTarget = sceneRoot / L"async-series.pmm";
    const auto replacedSaved = sceneRoot / L"async-series_003.pmm";
    const auto replacementPath = sceneRoot / L"replacement-scene.pmm";
    const float replacementFov = failedFov + 5.0f;
    const std::string replacementMarker = "audit-02-replacement-scene";
    gate.Arm();
    Check(background_save::Request(app, replacedTarget.c_str(), gate.options), "queue a numbered save before replacing the scene");
    gate.Block();
    app->AdvanceSceneGeneration();
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, replacementPath.c_str());
    pmm_extension::Set(document, save_naming::BaseKey, AuditBytes("replacement-base"));
    pmm_extension::Set(document, save_naming::FileKey, AuditBytes("replacement-scene.pmm"));
    pmm_extension::Set(document, kAuditMarkerKey, AuditBytes(replacementMarker.c_str()));
    app->CameraFov() = replacementFov;
    app->SceneModified() = 1;
    gate.Continue();
    FinishWrites(app);
    Check(gate.completed == 1 && gate.success, "captured snapshot reports worker success after scene replacement");
    Check(fs::is_regular_file(replacedSaved), "captured snapshot publishes its own numbered file");
    Check(std::wstring(app->EnvFileName()) == replacementPath.wstring(),
        "generation gate keeps the replacement scene filename");
    Check(AuditRecord(document, save_naming::BaseKey) == "replacement-base" &&
        AuditRecord(document, save_naming::FileKey) == "replacement-scene.pmm",
        "generation gate keeps the replacement scene naming keys");
    Check(AuditRecord(document, kAuditMarkerKey) == replacementMarker &&
        app->CameraFov() == replacementFov && app->SceneModified(),
        "generation gate keeps the replacement scene document and dirty state");
    Check(Recent() == replacedHistory, "generation gate records no completed save in project history");

    // A changed scene during a non-numbered save must drop only the stale
    // naming keys while keeping unrelated extension data and edits.
    auto plainSettings = settings;
    plainSettings.numberedManualVersions = false;
    Check(autosave::Configure(app, plainSettings, error), "disable numbering for the missing-key case");
    pmm_extension::Set(document, save_naming::BaseKey, AuditBytes("stale-base"));
    pmm_extension::Set(document, save_naming::FileKey, AuditBytes("stale-file.pmm"));
    const auto plainTarget = sceneRoot / L"plain-series.pmm";
    const auto plainNumbered = sceneRoot / L"plain-series_001.pmm";
    const float plainFov = replacementFov + 3.0f;
    const std::string plainMarker = "audit-02-plain-live-edit";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, plainTarget.c_str());
    gate.Arm();
    Check(background_save::Request(app, app->EnvFileName(), gate.options), "queue a non-numbered save with stale naming keys");
    gate.Block();
    Check(!fs::exists(plainTarget) && !fs::exists(plainNumbered), "blocked non-numbered save has published nothing");
    app->CameraFov() = plainFov;
    app->SceneModified() = 1;
    pmm_extension::Set(document, kAuditMarkerKey, AuditBytes(plainMarker.c_str()));
    gate.Continue();
    FinishWrites(app);
    Check(gate.completed == 1 && gate.success, "non-numbered save completes through the real worker");
    Check(fs::is_regular_file(plainTarget) && !fs::exists(plainNumbered) &&
        std::wstring(app->EnvFileName()) == plainTarget.wstring(),
        "non-numbered save writes its exact target without an increment");
    Check(!pmm_extension::Find(document, save_naming::BaseKey) &&
        !pmm_extension::Find(document, save_naming::FileKey),
        "non-numbered save erases stale naming keys from the live document");
    Check(AuditRecord(document, kAuditMarkerKey) == plainMarker &&
        app->CameraFov() == plainFov && app->SceneModified(),
        "non-numbered save keeps unrelated extension data and dirty state");

    gate.Close();
    app->CameraFov() = previousFov;
    app->PmmProjectSettings() = previousProject;
    app->SceneModified() = previousModified;
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, previousPath.c_str());
    Check(autosave::Configure(app, originalSettings, error), "restore autosave settings after the audit regression");
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
void BackupSceneStatusTests(MMDApp* app) {
    const auto first = root / L"backup-source.pmm";
    const auto second = root / L"backup-next.pmm";
    const auto setFov = [&](float fov) {
        app->CameraFov() = fov;
        app->SceneModified() = 1;
    };
    setFov(49);
    Check(SaveSceneCopy(app, second.c_str()), "write the independent project to open while backing up");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, first.c_str());
    setFov(37);
    Check(SaveSceneFile(app), "save the first real project");
    const auto firstBytes = Bytes(first), secondBytes = Bytes(second);
    autosave::Initialize(app);
    autosave::Settings settings;
    settings.enabled = true;
    settings.directory = (root / L"scene-backups").wstring();
    settings.maxBackups = 1;
    settings.overwriteOldest = true;
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "configure isolated rotating backups");
    app->EnglishUI() = kUiEnglish;

    setFov(61);
    const auto recentBefore = Recent();
    Check(autosave::QueueBackup(app) && std::wstring_view(background_save::StatusText(app)) == L"Backing up…",
          "the active project shows backup progress while the worker owns its captured snapshot");
    setFov(71);
    FinishWrites(app);
    const auto initial = autosave::Backups(settings.directory);
    Check(initial.size() == 1 && app->CameraFov() == 71 && app->SceneModified() &&
          fs::path(app->EnvFileName()) == first && Recent() == recentBefore &&
          std::wstring_view(background_save::StatusText(app)) == L"Backup saved",
          "backup completion preserves the newer unsaved edit, project name and recent list");
    Check(LoadSceneFile(initial.front().c_str()) && app->CameraFov() == 61,
          "the first backup reopens at the captured value before the later edit");
    Check(LoadSceneFile(first.c_str()), "return to the source for the rotation and project switch");
    setFov(71);
    Check(autosave::QueueBackup(app), "queue replacement of the first backup");
    setFov(83);
    Check(LoadSceneFile(second.c_str()) && !SceneSaveInProgress() && fs::path(app->EnvFileName()) == second &&
          app->CameraFov() == 49 && !app->SceneModified(), "opening another project joins the old backup and loads its own clean scene");
    const bool quietAfterOpen = !*background_save::StatusText(app);
    const auto rotated = autosave::Backups(settings.directory);
    Check(rotated.size() == 1 && rotated.front() != initial.front() && !fs::exists(initial.front()),
          "the older project still rotates its committed backup while another project opens");
    Check(LoadSceneFile(rotated.front().c_str()) && app->CameraFov() == 71,
          "rotated backup reopens with the captured edit and excludes the later edit");

    Check(LoadSceneFile(first.c_str()), "reopen the source before testing a replacement during a queued backup");
    setFov(95);
    Check(autosave::QueueBackup(app), "queue an old project backup before a direct scene reset");
    Check(ResetAppState(app), "replace the scene while the old backup completion is pending");
    app->EnvFileName()[0] = 0;
    setFov(44);
    const bool quietWhileWaiting = !*background_save::StatusText(app);
    FinishWrites(app);
    const bool quietAfterCompletion = !*background_save::StatusText(app);
    Check(!app->EnvFileName()[0] && app->CameraFov() == 44 && app->SceneModified(),
          "the old completion preserves the replacement scene's unnamed unsaved edit");
    const auto final = autosave::Backups(settings.directory);
    Check(final.size() == 1 && final.front() != rotated.front() &&
          LoadSceneFile(final.front().c_str()) && app->CameraFov() == 95,
          "the stale UI completion still leaves an independent reopenable backup on disk");
    Check(Bytes(first) == firstBytes && Bytes(second) == secondBytes, "backup and reopen preserve both original PMMs");
    std::printf("BACKUP_STATUS after_open=%d pending_reset=%d completed_reset=%d\n",
                int(quietAfterOpen), int(quietWhileWaiting), int(quietAfterCompletion));
    Check(quietAfterOpen && quietWhileWaiting && quietAfterCompletion,
          "an old backup must not display its status on the replacement scene");
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
decltype(&MoveFileExW) realJournalMove = nullptr;
decltype(&WriteFile) realJournalWrite = nullptr;
fs::path interruptedJournal, interruptionMarker;

BOOL WINAPI ParkDuringDecisionWrite(HANDLE file, LPCVOID bytes, DWORD count, LPDWORD written, LPOVERLAPPED overlapped) {
    constexpr std::string_view forward = "MMS-SAVE-FORWARD\n";
    if (count == forward.size() && std::string_view(static_cast<const char*>(bytes), count) == forward) {
        Check(!overlapped && realJournalWrite(file, bytes, count / 2, written, nullptr) &&
            *written == count / 2 && FlushFileBuffers(file), "write and flush only a prefix of the forward decision");
        {
            std::ofstream marker(interruptionMarker, std::ios::binary);
            marker << interruptedJournal.filename().string() << '\n';
            marker.close();
            Check(bool(marker), "signal that the prefix is on the live journal");
        }
        Sleep(60000);
        ExitProcess(75);
    }
    return realJournalWrite(file, bytes, count, written, overlapped);
}

BOOL WINAPI ParkBeforeJournalPublication(LPCWSTR source, LPCWSTR target, DWORD flags) {
    if (target && target == interruptedJournal.native()) {
        Check(source && flags == MOVEFILE_WRITE_THROUGH, "intercept only the first durable journal publication");
        const fs::path staged(source);
        Check(staged.parent_path() == interruptedJournal.parent_path() && staged.extension() == L".journal",
            "journal is staged beside the owned project");
        {
            std::ofstream marker(interruptionMarker, std::ios::binary);
            marker << staged.filename().string() << '\n';
            marker.close();
            Check(bool(marker), "record the exact staged journal for the supervisor");
        }
        Sleep(60000);
        ExitProcess(75);
    }
    return realJournalMove(source, target, flags);
}

void PrepublicationSaveTest(MMDApp* app, const std::wstring& mode) {
    const auto directory = root.parent_path() / L"project";
    const auto target = directory / L"scene.pmm";
    const auto open = [&](const fs::path& path, float fov) {
        Check(LoadSceneFile(path.c_str()) && fs::path(app->EnvFileName()) == path &&
            app->CameraFov() == fov && !app->SceneModified(), "reopen actual PMM and read the expected camera payload");
    };
    const auto save = [&](float fov) {
        app->CameraFov() = fov;
        app->SceneModified() = 1;
        SaveCommand(app, 207);
        Check(!app->SceneModified() && fs::path(app->EnvFileName()) == target,
            "the actual background Save command completed at the active path");
    };
    autosave::Initialize(app);
    auto settings = autosave::Current();
    settings.enabled = false;
    settings.keepPrevious = true;
    settings.numberedManualVersions = false;
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "enable PMM1 retention in the isolated save host");

    if (mode == L"--prepublication-seed") {
        Check(fs::create_directory(directory), "create the fresh disposable project directory");
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, target.c_str());
        save(37);
        std::ofstream(pmm_paths::Effect(target), std::ios::binary) << "original effect companion";
        save(53);
        std::ofstream(pmm_paths::Effect(target), std::ios::binary | std::ios::trunc) << "active effect companion";
    } else if (mode == L"--prepublication-write" || mode == L"--prepublication-decision-write") {
        open(target, 53);
        interruptedJournal = target.native() + L".mms-save.json";
        interruptionMarker = root / L"journal-ready.txt";
        if (mode == L"--prepublication-decision-write") {
            const auto entry = reinterpret_cast<void*>(&WriteFile);
            Check(MH_CreateHook(entry, &ParkDuringDecisionWrite, reinterpret_cast<void**>(&realJournalWrite)) == MH_OK &&
                MH_EnableHook(entry) == MH_OK, "arm the disposable writer's decision write barrier");
        } else {
            const auto entry = reinterpret_cast<void*>(&MoveFileExW);
            Check(MH_CreateHook(entry, &ParkBeforeJournalPublication, reinterpret_cast<void**>(&realJournalMove)) == MH_OK &&
                MH_EnableHook(entry) == MH_OK, "arm only the disposable writer's publication barrier");
        }
        save(67);
        Check(false, "writer must stop at the armed recovery journal barrier");
    } else if (mode == L"--prepublication-retry") {
        open(target, 53);
        save(79);
    } else if (mode == L"--prepublication-reopen") {
        open(target, 53);
        open(target.native() + L"1", 37);
    } else if (mode == L"--prepublication-archive-missing" || mode == L"--prepublication-archive-replaced") {
        app->CameraFov() = 61;
        app->SceneModified() = 1;
        const auto generation = app->SceneGeneration();
        const auto path = fs::path(app->EnvFileName());
        const auto beforeErrors = errors;
        bool explainedRecovery = false;
        expectError = true;
        messageAction = [&](HWND owner, LPCWSTR message) {
            const std::wstring_view text(message);
            explainedRecovery = owner == app->MainWindow() &&
                text.find(L"工程保存恢复未完成") != text.npos && text.find(target.native()) != text.npos;
        };
        app->EnglishUI() = kUiChinese;
        const bool opened = LoadSceneFile(target.c_str());
        expectError = false;
        messageAction = {};
        Check(!opened && app->SceneGeneration() == generation && fs::path(app->EnvFileName()) == path &&
            app->CameraFov() == 61 && app->SceneModified(), "archive conflict retains the current editing scene");
        std::printf("archive conflict feedback count=%u explained=%d\n", errors - beforeErrors, explainedRecovery);
        Check(errors == beforeErrors + 1 && explainedRecovery, "show one recovery error with the rejected project path");
    } else {
        Check(mode == L"--prepublication-verify", "use the explicit reopen verification mode");
        open(target, 79);
        open(target.native() + L"1", 53);
        open(target.native() + L"11", 37);
    }
}
}
#include "model_load_view_checks.hpp"
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("SAVE_TEST phase=start");
    const bool numberedOnly = argc == 3 && std::wstring(argv[2]) == L"--numbered-only";
    const bool saveAsGenerationOnly = argc == 3 && std::wstring(argv[2]) == L"--save-as-generation-only";
    const bool saveCaptureCloseOnly = argc == 3 && std::wstring(argv[2]) == L"--save-capture-close-only";
    const bool saveFailureRetryOnly = argc == 3 && std::wstring(argv[2]) == L"--save-failure-retry-only";
    const bool backupSceneStatusOnly = argc == 3 && std::wstring(argv[2]) == L"--backup-scene-status";
    const std::wstring mode = argc == 3 ? argv[2] : L"";
    const bool prepublication = mode == L"--prepublication-seed" || mode == L"--prepublication-write" ||
        mode == L"--prepublication-decision-write" || mode == L"--prepublication-reopen" ||
        mode == L"--prepublication-retry" || mode == L"--prepublication-verify" ||
        mode == L"--prepublication-archive-missing" || mode == L"--prepublication-archive-replaced";
    cacheSaveChecks = !build_profile::original && argc == 3 && std::wstring(argv[2]) == L"--cache-save-only";
    Check(argc == 2 || numberedOnly || saveAsGenerationOnly || saveCaptureCloseOnly || saveFailureRetryOnly ||
        backupSceneStatusOnly || cacheSaveChecks || prepublication,
        "isolated artifact directory required");
    if (cacheSaveChecks) VerifySaveDesktop();
    root = fs::absolute(argv[1]).lexically_normal();
    if (prepublication) {
        std::ifstream marker(root.parent_path() / L"owned-save-crash-fixture.txt", std::ios::binary);
        std::string signature;
        std::getline(marker, signature);
        Check(signature == "MMS prepublication fixture v1", "require the isolated supervisor marker");
    }
    const bool newOutput = fs::create_directories(root);
    if (saveFailureRetryOnly) Check(newOutput, "require a fresh Save and Save As fixture directory");
    if (prepublication) Check(newOutput, "require a fresh stage output directory");
    if (cacheSaveChecks) {
        Check(newOutput, "cache save acceptance requires a new output directory");
        IsolateSaveCache();
    }
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
    std::puts("SAVE_TEST phase=initialize-host");
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production app");
    if (prepublication) {
        PrepublicationSaveTest(app, mode);
        Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS prepublication mode=%ls checks=%u\n", mode.c_str(), checks);
        return 0;
    }
    if (saveCaptureCloseOnly) {
        std::puts("SAVE_TEST phase=save-capture-close-async");
        SaveCaptureCloseTest(app, /*synchronous=*/false);
        std::puts("SAVE_TEST phase=save-capture-close-sync");
        SaveCaptureCloseTest(app, /*synchronous=*/true);
        Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS save capture-close checks=%u\n", checks); return 0;
    }
    if (saveAsGenerationOnly) {
        std::puts("SAVE_TEST phase=save-as-generation");
        SaveAsGenerationTests(app);
        Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS Save As generation checks=%u\n", checks); return 0;
    }
    if (saveFailureRetryOnly) {
        SaveFailureThenSaveAsTest(app);
        Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS background Save failure and Save As checks=%u\n", checks); return 0;
    }
    if (backupSceneStatusOnly) {
        BackupSceneStatusTests(app);
        Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS backup scene status checks=%u\n", checks); return 0;
    }
    if (numberedOnly) {
        autosave::Initialize(app);
        NumberedVersionTests(app);
        BackupAudit02Tests(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS numbered save checks=%u\n", checks); return 0;
    }
    if (const auto* modelPath = _wgetenv(L"MMS_MODEL_LOAD_VIEW_TEST"); modelPath && !cacheSaveChecks) {
        CheckModelLoadView(app, modelPath);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        return 0;
    }
    std::puts("SAVE_TEST phase=save-commands"); SaveTests(app);
    std::puts("SAVE_TEST phase=async"); AsyncTests(app);
    if (cacheSaveChecks) {
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hook);
        std::printf("PASS cache save/export checks=%u\n", checks); return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    std::puts("SAVE_TEST phase=abi-transactions"); AbiTransactionTests(app);
#endif
    std::puts("SAVE_TEST phase=save-open-alias"); SaveOpenAliasTests(app);
    VersionTests(app);
    std::puts("SAVE_TEST phase=backup-audit-02");
    BackupAudit02Tests(app);
    std::puts("SAVE_TEST phase=export-dialogs");
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
