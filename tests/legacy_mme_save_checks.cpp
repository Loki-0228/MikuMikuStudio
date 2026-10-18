// Native MME's PMM file-notification contract, with an original test-owned
// queue/consumer. Real PMM serialization, publication and backup rotation run
// unchanged. This does not test third-party EMM data or claim synchronous save.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <memory>
#include <string>
#include <vector>
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/effect_api.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/project_storage.hpp"
#endif

using namespace mikudancestudio;
namespace fs = std::filesystem;
namespace as = mikudancestudio::autosave;
namespace {
unsigned checks = 0, notifications = 0, consumed = 0, renderCalls = 0, commitFaults = 0;
bool mockPresent = false, mockAutoSave = true, failCommit = false, freezeClock = false;
std::vector<fs::path> pending;
std::vector<fs::path> removed;
DWORD uiThread = 0;
fs::path root;
decltype(&GetModuleHandleW) realModule = nullptr;
decltype(&CreateFileW) realCreate = nullptr;
decltype(&DeleteFileW) realDelete = nullptr;
decltype(&GetLocalTime) realTime = nullptr;
decltype(&_commit) realCommit = nullptr;
decltype(&ShowWindow) realShow = nullptr;
decltype(&BeginEffectObjectRegistration) realRegistration = nullptr;

void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
template<class Function, class Replacement>
void Hook(Function function, Replacement replacement, void** original = nullptr) {
    const auto address = reinterpret_cast<void*>(function);
    Check(MH_CreateHook(address, reinterpret_cast<void*>(replacement), original) == MH_OK,
        "install an isolated native notification observer");
    Check(MH_EnableHook(address) == MH_OK, "enable the isolated observer");
}
std::string Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
fs::path Emm(fs::path project) { project.replace_extension(L".emm"); return project; }
void Text(const fs::path& path, const char* text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text; file.close(); Check(bool(file), "write an original mock or unrelated fixture");
}
std::wstring Title(HWND window) {
    wchar_t text[512]{}; GetWindowTextW(window, text, 512); return text;
}
bool IsPmm(const wchar_t* path) {
    const auto* extension = path ? std::wcsrchr(path, L'.') : nullptr;
    return extension && !_wcsicmp(extension, L".pmm");
}
HMODULE WINAPI Module(LPCWSTR name) {
    if (mockPresent && name && !_wcsicmp(name, L"MMHack.dll")) return realModule(nullptr);
    return realModule(name);
}
HANDLE WINAPI Create(LPCWSTR filename, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                     DWORD creation, DWORD attributes, HANDLE source) {
    const HANDLE file = realCreate(filename, access, share, security, creation, attributes, source);
    const DWORD error = GetLastError();
    if (mockPresent && file != INVALID_HANDLE_VALUE && access == GENERIC_WRITE &&
        creation == OPEN_EXISTING && IsPmm(filename)) {
        Check(share == (FILE_SHARE_READ | FILE_SHARE_WRITE) && attributes == FILE_ATTRIBUTE_NORMAL &&
            source == nullptr && security == nullptr && GetCurrentThreadId() == uiThread,
            "native notification uses the UI thread and existing nontruncating shared PMM open");
        const auto bytes = Bytes(filename);
        Check(bytes.size() > 30 && !std::memcmp(bytes.data(), "Polygon Movie maker", 19),
            "the notification observes a complete published real PMM, never a temporary write");
        pending.emplace_back(filename); ++notifications;
    }
    SetLastError(error); return file;
}
BOOL WINAPI Delete(LPCWSTR path) {
    const BOOL result = realDelete(path);
    const DWORD error = GetLastError();
    if (result && mockPresent && path) removed.emplace_back(path);
    SetLastError(error); return result;
}
void WINAPI Time(LPSYSTEMTIME time) {
    if (!freezeClock) { realTime(time); return; }
    *time = SYSTEMTIME{2026, 10, 1, 19, 1, 23, 45, 678};
}
int __cdecl Commit(int fd) {
    if (failCommit) { ++commitFaults; _set_errno(EIO); return -1; }
    return realCommit(fd);
}
void Registration() { ++renderCalls; realRegistration(); }
BOOL WINAPI Hide(HWND window, int) { return realShow(window, SW_HIDE); }
LRESULT CALLBACK NoActivation(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) create->style &= ~WS_VISIBLE;
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI MessageW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fprintf(stderr, "Unexpected native dialog: %ls\n", text ? text : L"");
    Check(false, "no modal dialog is required by an isolated save/backup"); return IDOK;
}
int WINAPI MessageA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::fprintf(stderr, "Unexpected native dialog: %s\n", text ? text : "");
    Check(false, "no ANSI dialog is required by an isolated save/backup"); return IDOK;
}
BOOL WINAPI NoBeep(UINT) { return TRUE; }

void ConsumeMockQueue() {
    // The exported callback below belongs to this test. A real native MME
    // consumes the same pending paths in its normal render callback and owns
    // its actual EMM syntax, settings and assignments.
    Check(GetCurrentThreadId() == uiThread, "the original mock consumes on its owning UI thread");
    const auto work = std::move(pending); pending.clear();
    for (const auto& path : work) {
        ++consumed;
        if (mockAutoSave) Text(Emm(path), "original-mock-live-assignment\n");
    }
}
struct Identity {
    std::wstring file, directory, title;
    std::array<std::wstring, 7> browse;
    fs::path cwd;
    int frame;
    unsigned char dirty, layout;
    explicit Identity(MMDApp& app) : file(app.EnvFileName()), directory(app.PathWorkspace().projectDirectory),
        title(Title(app.MainWindow())), browse{app.DirModel(), app.DirUser(), app.DirAccs(), app.DirMotion(),
        app.DirPose(), app.DirWave(), app.DirBg()}, cwd(fs::current_path()), frame(app.CurrentFrame()),
        dirty(app.SceneModified()), layout(app.WindowLayoutReady()) {}
    void Match(MMDApp& app) const {
        const std::array<std::wstring, 7> current{app.DirModel(), app.DirUser(), app.DirAccs(), app.DirMotion(),
            app.DirPose(), app.DirWave(), app.DirBg()};
        Check(file == app.EnvFileName() && directory == app.PathWorkspace().projectDirectory &&
            title == Title(app.MainWindow()) && browse == current && cwd == fs::current_path(),
            "backup notification/rotation preserves project identity, title, all picker directories and CWD");
        Check(frame == app.CurrentFrame() && dirty == app.SceneModified() && layout == app.WindowLayoutReady(),
            "backup notification/rotation preserves playhead, dirty state and layout");
    }
};
void Configure(MMDApp& app, const as::Settings& settings) {
    std::wstring error;
    Check(as::Configure(&app, settings, error), "configure only the isolated backup directory");
}
fs::path SoleBackup(const as::Settings& settings) {
    const auto paths = as::Backups(settings.directory);
    Check(paths.size() == 1, "one owned PMM remains within the requested retention limit");
#if !MMD_ORIGINAL_PROFILE
    Check(fs::is_regular_file(plugins::PluginSidecar(paths.front())), "native EMM does not replace the ABI plugin sidecar");
    (void)plugins::ReadPluginProject(paths.front());
#endif
    return paths.front();
}
void QueueNames(const fs::path& path, unsigned before) {
    Check(notifications == before + 1 && pending.size() == 1 && pending.front() == path,
        "exactly one native notification names the retained backup, including its directory and stem");
    Check(!fs::exists(Emm(path)), "enqueuing alone is not reported as completed EMM serialization");
}
void DeletedPairInOrder(const fs::path& path) {
    const auto pmm = std::find(removed.begin(), removed.end(), path);
    const auto emm = std::find(removed.begin(), removed.end(), Emm(path));
    Check(pmm != removed.end() && emm != removed.end() && pmm < emm,
        "rotation removes the matching EMM only after its owned PMM was successfully deleted");
}
} // namespace

extern "C" __declspec(dllexport) void __cdecl LegacyMockConsumePmmSaves() { ConsumeMockQueue(); }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    Check(count && count < 32768, "resolve this isolated executable's data root");
    root = fs::path(executable).parent_path() / L"data" / L"legacy-mme-save";
    fs::create_directories(root);
    Check(root.native().size() + 9 < 200 && root.native().size() + 64 < 256,
        "legacy backup paths fit existing PMM and settings limits");
    Check(!GetModuleHandleW(L"MMHack.dll") && !GetModuleHandleW(L"MMEffect.dll"),
        "this test neither ships nor loads third-party MME binaries");
    SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX", L"0");
    SetEnvironmentVariableW(L"MIKUDANCESTUDIO_AUTOSAVE_CONFIG", (root / L"autosave.ini").c_str());
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", (root / L"recent.txt").c_str());
    uiThread = GetCurrentThreadId();
    Check(MH_Initialize() == MH_OK, "initialize isolated observers");
    Hook(&ShowWindow, &Hide, reinterpret_cast<void**>(&realShow));
    Hook(&MessageBoxW, &MessageW); Hook(&MessageBoxA, &MessageA); Hook(&MessageBeep, &NoBeep);
    Hook(&GetModuleHandleW, &Module, reinterpret_cast<void**>(&realModule));
    Hook(&CreateFileW, &Create, reinterpret_cast<void**>(&realCreate));
    Hook(&DeleteFileW, &Delete, reinterpret_cast<void**>(&realDelete));
    Hook(&GetLocalTime, &Time, reinterpret_cast<void**>(&realTime));
    Hook(&_commit, &Commit, reinterpret_cast<void**>(&realCommit));
    Hook(&BeginEffectObjectRegistration, &Registration, reinterpret_cast<void**>(&realRegistration));
    const HHOOK hide = SetWindowsHookExW(WH_CBT, NoActivation, nullptr, GetCurrentThreadId());
    Check(hide != nullptr, "hide and prevent activation of the actual production host");
    auto owner = std::make_unique<MMDApp>(); auto* app = owner.get(); g_Block = app;
    std::memset(&app->state, 0, sizeof(app->state)); app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "create the hidden real host");
    app->EnglishUI() = 2; app->state.optflag[0] = 1;
    as::Initialize(app);
    const auto manual = root / L"scene.pmm";
    wcscpy_s(app->EnvFileName(), 256, manual.c_str());
    app->SceneModified() = 1; app->CurrentFrame() = 17;
    SaveSceneFile(app);
    Check(!app->SceneModified() && fs::is_regular_file(manual) && !notifications,
        "manual PMM save works unchanged when native MME is absent");
    Text(Emm(manual), "original-user-assignments\n");
    const auto manualBytes = Bytes(manual), manualEmm = Bytes(Emm(manual));
#if !MMD_ORIGINAL_PROFILE
    const auto manualPlugin = Bytes(plugins::PluginSidecar(manual));
#endif
    wcscpy_s(app->PathWorkspace().projectDirectory, 256, root.c_str());
    for (auto* directory : {app->DirModel(), app->DirUser(), app->DirAccs(), app->DirMotion(),
            app->DirPose(), app->DirWave(), app->DirBg()}) wcscpy_s(directory, 256, root.c_str());
    const auto browse = root / L"last-picker-directory"; fs::create_directories(browse);
    Check(SetCurrentDirectoryW(browse.c_str()), "simulate the user's most recent file picker directory");
    app->SceneModified() = 1;
    const Identity identity(*app);
    as::Settings settings; settings.enabled = true; settings.maxBackups = 1;
    settings.overwriteOldest = true; settings.directory = (root / L"backups").wstring();
    Configure(*app, settings);
    const auto orphan = fs::path(settings.directory) / L"scene_autosave_20261019_012345_678.emm";
    Text(orphan, "unrelated-orphan-emm\n");
    const auto unrelated = fs::path(settings.directory) / L"user_autosave_20261019_012345_678.pmm";
    Text(unrelated, "ordinary user file\n"); Text(Emm(unrelated), "unrelated-user-emm\n");
    const auto consume = reinterpret_cast<void(__cdecl*)()>(GetProcAddress(GetModuleHandleW(nullptr), "LegacyMockConsumePmmSaves"));
    Check(consume != nullptr, "resolve the original test-owned consumer export");
    renderCalls = 0; freezeClock = mockPresent = true;
    auto before = notifications;
    Check(as::BackupNow(app) == as::Result::Saved, "retained automatic PMM backup sends its native notification");
    auto first = SoleBackup(settings); QueueNames(first, before); identity.Match(*app);
    Check(first.stem() == L"scene_autosave_20261019_012345_678_1" && Bytes(orphan) == "unrelated-orphan-emm\n",
        "a preexisting orphan EMM forces a new basename and is never overwritten");
    consume();
    Check(pending.empty() && Bytes(Emm(first)) == "original-mock-live-assignment\n",
        "explicit mock consumption writes the corresponding EMM beside its retained PMM");
    removed.clear(); before = notifications;
    Check(as::BackupNow(app) == as::Result::Saved, "successful rotation queues only the new retained backup");
    auto second = SoleBackup(settings); QueueNames(second, before); DeletedPairInOrder(first);
    Check(!fs::exists(first) && !fs::exists(Emm(first)), "rotation removes both the old owned PMM and its same-name EMM");
#if !MMD_ORIGINAL_PROFILE
    Check(!fs::exists(plugins::PluginSidecar(first)), "rotation also retains the existing plugin-sidecar cleanup");
#endif
    consume(); identity.Match(*app);
    Check(Bytes(Emm(second)) == "original-mock-live-assignment\n" && Bytes(orphan) == "unrelated-orphan-emm\n" &&
        Bytes(unrelated) == "ordinary user file\n" && Bytes(Emm(unrelated)) == "unrelated-user-emm\n",
        "rotation never removes unrelated PMMs or EMM assignments");
    const auto keptPmm = Bytes(second), keptEmm = Bytes(Emm(second));
    HANDLE lock = CreateFileW(second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "prevent deletion of the previous owned PMM");
    before = notifications; removed.clear();
    Check(as::BackupNow(app) == as::Result::Full, "failed rotation rejects and removes the new candidate");
    CloseHandle(lock);
    Check(notifications == before && pending.empty() && SoleBackup(settings) == second &&
        Bytes(second) == keptPmm && Bytes(Emm(second)) == keptEmm &&
        std::find(removed.begin(), removed.end(), Emm(second)) == removed.end(),
        "failed PMM deletion keeps its EMM and does not queue an abandoned candidate");
    failCommit = true; before = notifications;
    Check(as::BackupNow(app) == as::Result::Failed, "a real PMM commit failure cannot publish a backup");
    failCommit = false;
    Check(commitFaults == 1 && notifications == before && pending.empty() && SoleBackup(settings) == second &&
        Bytes(second) == keptPmm && Bytes(Emm(second)) == keptEmm,
        "failed native serialization preserves the retained pair and emits no MME notification");
    settings.overwriteOldest = false; Configure(*app, settings); before = notifications;
    Check(as::BackupNow(app) == as::Result::Full && notifications == before && pending.empty(),
        "no-overwrite capacity refusal never notifies native MME");
    const auto plainCopy = root / L"plain-copy.pmm";
    Check(SaveSceneCopy(app, plainCopy.c_str()) && notifications == before && pending.empty(),
        "plain copy serialization leaves notification ownership with the autosave retention caller");
    identity.Match(*app);
    settings.overwriteOldest = true; Configure(*app, settings);
    before = notifications; mockAutoSave = false;
    Check(as::BackupNow(app) == as::Result::Saved, "native autosave-disabled configuration still retains the PMM");
    auto disabled = SoleBackup(settings); QueueNames(disabled, before); consume();
    Check(pending.empty() && !fs::exists(Emm(disabled)), "the live native autosave policy is not forced on by host notification");
    mockAutoSave = true; mockPresent = false; before = notifications;
    Check(as::BackupNow(app) == as::Result::Saved && notifications == before && pending.empty(),
        "backup and rotation remain usable with no optional MME module loaded");
    identity.Match(*app);
    Check(Bytes(manual) == manualBytes && Bytes(Emm(manual)) == manualEmm,
        "automatic backups never write the manual PMM or its existing EMM");
#if !MMD_ORIGINAL_PROFILE
    Check(Bytes(plugins::PluginSidecar(manual)) == manualPlugin, "automatic EMM handling never rewrites the manual plugin sidecar");
#endif
    mockPresent = true; before = notifications;
    SaveSceneFile(app);
    Check(notifications == before + 1 && pending.size() == 1 && pending.front() == manual &&
        Bytes(Emm(manual)) == manualEmm,
        "manual save still only queues MME work: immediate-exit completion is a separate unresolved requirement");
    Check(renderCalls == 0, "notification and mock consumption never enter host scene rendering recursively");
    consume(); mockPresent = false; freezeClock = false;
    Check(!realModule(L"MMHack.dll") && !realModule(L"MMEffect.dll") && !IsWindowVisible(app->MainWindow()),
        "the host stayed hidden and loaded no third-party MME binaries");
    as::Shutdown(); ShutdownCleanup(app); owner.reset(); g_Block = nullptr;
    UnhookWindowsHookEx(hide); MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
    std::printf("PASS original MME notification mock: %u checks; notifications=%u consumed=%u (no real EMM-state claim)\n",
        checks, notifications, consumed);
    return 0;
}
