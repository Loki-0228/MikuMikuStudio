// MAINTENANCE: regression-keep; rejected PMM candidates never become save targets.
// The shell, dialogs, recent-menu dispatch, HDROP, project-pair store and bound
// journal are real. Only modal dialogs, the final save call, effect/localization
// callbacks and accepted native bodies are intercepted. Accepted-body probes
// model the destructive boundary; they are not native PMM parser round trips.
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "mikudancestudio/plugin_storage.hpp"
#include "mikudancestudio/recent_projects.hpp"
#include "mikudancestudio/effect_api.hpp"
#include "mikudancestudio/scene_ownership.hpp"
#include "mikudancestudio/ui_translation.hpp"
#include "../src/io/project_open.hpp"
#include "../src/io/pmm_io_common.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/project_storage.hpp"
#endif
#include <MinHook.h>
#include <commctrl.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace mikudancestudio;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0, messages = 0, browsers = 0, saves = 0, effects = 0, localized = 0;
MMDApp* app = nullptr;
fs::path root, currentPath, browserPath;
std::wstring savedPath;
bool cancelBrowser = false, cancelConfirmation = false, reenter = false;
enum class NativeMode { RefuseBoundary, ThrowMemory, ThrowShortRead, ThrowRecord, Accept, CancelAfterBoundary };
NativeMode nativeMode = NativeMode::RefuseBoundary;
using Body = bool (*)(MMDApp*, int, bool&);
Body originalV1 = nullptr, originalV2 = nullptr;
using Begin = bool (*)(MMDApp*);
Begin originalBegin = nullptr;

void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::printf("FAIL %s\n", why); std::exit(1); }
}
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close(); Check(bool(output), "write disposable project fixture");
}
std::string Header(const char* version = "0002") {
    std::string bytes(30, '\0');
    std::memcpy(bytes.data(), "Polygon Movie maker", 19);
    std::memcpy(bytes.data() + 20, version, 4);
    return bytes;
}
#if !MMD_ORIGINAL_PROFILE
plugins::PluginProject expectedProject;
void Pair(const fs::path& path, const std::string& bytes, const char* owner) {
    const auto stage = fs::path(path.native() + L".stage");
    Write(stage, bytes);
    plugins::PluginProject data;
    data.projectId = plugins::StorageId();
    data.records.emplace(owner, plugins::StoredData{29, {0, 255, 128, 42, 0}});
    data.records.emplace(std::string(owner) + ".empty", plugins::StoredData{31, {}});
    plugins::PublishProjectPair(path, stage, data, false);
    fs::remove(stage);
}
void CheckPluginIdentity() {
    static unsigned copy = 0;
    const auto destination = root / (L"probe-" + std::to_wstring(++copy) + L".pmm");
    const auto stage = fs::path(destination.native() + L".stage");
    Write(stage, Header());
    plugins::ProjectSave capture(app);
    Check(capture.Publish(stage.c_str(), destination.c_str(), true), "capture live plugin data through the paired copy writer");
    const auto restored = plugins::ReadPluginProject(destination);
    Check(restored.projectId == expectedProject.projectId && restored.records.size() == expectedProject.records.size(),
        "failed candidate preserves the live plugin project and unknown records");
    for (const auto& [id, record] : expectedProject.records) {
        const auto found = restored.records.find(id);
        Check(found != restored.records.end() && found->second.schema == record.schema && found->second.bytes == record.bytes,
            "failed candidate preserves plugin schemas and binary payloads");
    }
    fs::remove(stage);
}
class ScalarChange final : public plugins::HistoryChange {
    int& value_;
    class Candidate final : public plugins::PreparedHistoryChange {
        int& value_; int source_, target_;
    public:
        Candidate(int& value, int source, int target) : value_(value), source_(source), target_(target) {}
        bool IsCurrent() const noexcept override { return value_ == source_; }
        void Publish() noexcept override { value_ = target_; }
    };
public:
    explicit ScalarChange(int& value) : value_(value) {}
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        if (value_ != (forward ? 0 : 1)) return MMS_E_STALE;
        out = std::make_unique<Candidate>(value_, forward ? 0 : 1, forward ? 1 : 0); return MMS_OK;
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
int journalValue = 0;
void SeedHistory() {
    auto* history = plugins::NativeHistory(app); Check(history != nullptr, "real bound project journal exists");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> candidate;
    Check(history->PrepareNative("project-open-identity", std::make_shared<ScalarChange>(journalValue), candidate) == MMS_OK,
        "prepare real native history fixture");
    Check(history->CommitNative(std::move(candidate)) == MMS_OK && journalValue == 1,
        "publish native history fixture");
}
#else
void Pair(const fs::path& path, const std::string& bytes, const char*) { Write(path, bytes); }
void CheckPluginIdentity() {}
#endif

int WINAPI DialogW(HWND, LPCWSTR, LPCWSTR, UINT flags) {
    ++messages;
    if (reenter) {
        reenter = false;
        Check(!LoadSceneFile(currentPath.c_str()), "a modal-error callback cannot reenter project open");
    }
    return cancelConfirmation && (flags & MB_OKCANCEL) == MB_OKCANCEL ? IDCANCEL : IDOK;
}
int WINAPI DialogA(HWND, LPCSTR, LPCSTR, UINT flags) {
    ++messages;
    return cancelConfirmation && (flags & MB_OKCANCEL) == MB_OKCANCEL ? IDCANCEL : IDOK;
}
BOOL Browser(LPOPENFILENAMEW parameters) {
    ++browsers;
    Check(parameters && parameters->hwndOwner == app->MainWindow() &&
        parameters->lpstrDefExt && !std::wcscmp(parameters->lpstrDefExt, L"pmm"),
        "actual Open menu reaches the PMM browser");
    if (cancelBrowser) return FALSE;
    Check(browserPath.native().size() < parameters->nMaxFile, "browser fixture fits the native buffer");
    wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, browserPath.c_str()); return TRUE;
}
void Save(MMDApp* value) { ++saves; savedPath = value->EnvFileName(); }
void Effect(const wchar_t*) { ++effects; }
void Localize(MMDApp*) { ++localized; }
bool BeginReplacement(MMDApp* value) {
    return nativeMode == NativeMode::RefuseBoundary ? false : originalBegin(value);
}
bool LoadBody(Body original, MMDApp* value, int fd, bool& replaced) {
    if (nativeMode == NativeMode::RefuseBoundary) {
#if MMD_ORIGINAL_PROFILE
        // Original's BeginProjectSceneReplacement is a constant true stub and
        // can be shorter than a detour. The full profile exercises both real
        // V1/V2 bodies through the actual rejected Begin boundary below.
        throw std::runtime_error("fixture boundary refusal in original profile");
#else
        return original(value, fd, replaced);
#endif
    }
    if (nativeMode == NativeMode::ThrowMemory) throw std::bad_alloc();
    if (nativeMode == NativeMode::ThrowShortRead) throw pmm_io::ShortRead(30, 4, 0);
    if (nativeMode == NativeMode::ThrowRecord) throw std::runtime_error("fixture record rejection before replacement");
    Check(originalBegin(value), "accepted-body probe crosses the real common-history replacement boundary");
    replaced = true;
    AcceptProjectSceneReplacement(value);
    Check(value->SceneModified() == 0 && value->WindowLayoutReady() == 0,
        "identity acceptance occurs only at the destructive boundary");
    _close(fd);
    if (nativeMode == NativeMode::CancelAfterBoundary) {
        Check(plugins::ResetPluginProject(value), "simulate the native loader's empty-scene reset after cancellation");
        value->EnvFileName()[0] = 0;
        SetWindowTextW(value->MainWindow(), L"MikuMikuDance");
        return false;
    }
    SetWindowTextW(value->MainWindow(), value->EnvFileName());
    value->WindowLayoutReady() = 1;
    plugins::CommitProjectLoad(value);
    recent_projects::Record(value, value->EnvFileName());
    return true;
}
bool LoadV1(MMDApp* value, int fd, bool& replaced) { return LoadBody(originalV1, value, fd, replaced); }
bool LoadV2(MMDApp* value, int fd, bool& replaced) { return LoadBody(originalV2, value, fd, replaced); }

template<class Function, class Replacement>
void Hook(Function function, Replacement replacement, void** original = nullptr) {
    auto* address = reinterpret_cast<void*>(function);
    Check(MH_CreateHook(address, reinterpret_cast<void*>(replacement), original) == MH_OK, "install isolated regression hook");
    Check(MH_EnableHook(address) == MH_OK, "enable isolated regression hook");
}
struct Identity {
    std::array<wchar_t, 256> file{}, project{}, executable{};
    std::array<wchar_t, 1000> user{};
    std::wstring title;
    unsigned char dirty, layout;
    int frame;
    float camera;
    unsigned effectCount = effects, localizeCount = localized;
#if !MMD_ORIGINAL_PROFILE
    std::uint64_t revision = 0;
    std::size_t undo = 0, redo = 0;
#endif
    Identity() : dirty(app->SceneModified()), layout(app->WindowLayoutReady()),
        frame(app->CurrentFrame()), camera(app->CameraDistance()) {
        std::memcpy(file.data(), app->EnvFileName(), sizeof file);
        std::memcpy(project.data(), app->PathWorkspace().projectDirectory, sizeof project);
        std::memcpy(executable.data(), app->PathWorkspace().executableDirectory, sizeof executable);
        std::memcpy(user.data(), app->DirUser(), sizeof user);
        wchar_t text[512]{}; GetWindowTextW(app->MainWindow(), text, 512); title = text;
#if !MMD_ORIGINAL_PROFILE
        auto* history = plugins::NativeHistory(app);
        Check(history && history->Revision(revision) == MMS_OK, "capture common-history source revision");
        undo = history->UndoCount(); redo = history->RedoCount();
#endif
    }
    void Same() const {
        Check(!std::memcmp(file.data(), app->EnvFileName(), sizeof file), "rejected open preserves all save-target bytes");
        Check(!std::memcmp(project.data(), app->PathWorkspace().projectDirectory, sizeof project), "rejected open preserves project-relative asset base");
        Check(!std::memcmp(executable.data(), app->PathWorkspace().executableDirectory, sizeof executable), "rejected open does not use executable base as directory scratch");
        Check(!std::memcmp(user.data(), app->DirUser(), sizeof user), "rejected open preserves the remembered PMM directory");
        wchar_t text[512]{}; GetWindowTextW(app->MainWindow(), text, 512);
        Check(title == text, "rejected open preserves the exact visible project title");
        Check(app->SceneModified() == dirty && app->WindowLayoutReady() == layout &&
            app->CurrentFrame() == frame && app->CameraDistance() == camera, "rejected open preserves dirty, layout, playhead and live pose");
        Check(effects == effectCount && localized == localizeCount, "rejected open sends no accepted-project effect or localization callbacks");
#if !MMD_ORIGINAL_PROFILE
        auto* history = plugins::NativeHistory(app); std::uint64_t now = 0;
        Check(history && history->Revision(now) == MMS_OK && now == revision &&
            history->UndoCount() == undo && history->RedoCount() == redo && journalValue == 1,
            "rejected open preserves journal revision, nodes and authored state");
#endif
        const auto previousSaves = saves;
        CommandDispatch(nullptr, 207);
        Check(saves == previousSaves + 1 && savedPath == file.data(), "actual quick-save command still selects the original PMM");
        CheckPluginIdentity();
    }
};
void Released(const fs::path& path) {
    if (!fs::exists(path)) return;
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "rejected open releases the PMM descriptor and pair read lock");
    CloseHandle(handle);
}
void Drop(const fs::path& path) {
    const auto bytes = sizeof(DROPFILES) + (path.native().size() + 2) * sizeof(wchar_t);
    auto handle = GlobalAlloc(GHND, bytes); Check(handle != nullptr, "allocate a real Unicode HDROP");
    auto* data = static_cast<DROPFILES*>(GlobalLock(handle));
    data->pFiles = sizeof(DROPFILES); data->fWide = TRUE;
    std::memcpy(reinterpret_cast<unsigned char*>(data) + sizeof(DROPFILES), path.c_str(), (path.native().size() + 1) * sizeof(wchar_t));
    GlobalUnlock(handle); HandleDropFiles(static_cast<HDROP>(handle));
}
LRESULT CALLBACK Owner(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t temporary[MAX_PATH]{}; Check(GetTempPathW(MAX_PATH, temporary) != 0, "locate test temp directory");
    root = fs::path(temporary) / (L"MMS-project-open-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"候选目录");
    currentPath = root / L"当前原稿.pmm";
    const auto unsupported = root / L"候选目录/不支持版本.pmm";
    const auto badMagic = root / L"错误头.pmm", shortHeader = root / L"短头.pmm";
    const auto v1 = root / L"原生版本一.pmm", v2 = root / L"原生版本二.pmm";
    const auto missing = root / L"不存在.pmm";
    Pair(currentPath, Header(), "test.current"); Pair(unsupported, Header("9999"), "test.candidate");
    Pair(badMagic, std::string(30, '!'), "test.bad"); Pair(shortHeader, Header().substr(0, 12), "test.short");
    Pair(v1, Header("0001"), "test.v1"); Pair(v2, Header(), "test.v2");
    const auto originalMain = Bytes(currentPath), rejectedMain = Bytes(unsupported);
#if !MMD_ORIGINAL_PROFILE
    expectedProject = plugins::ReadPluginProject(currentPath);
    const auto originalSide = Bytes(plugins::PluginSidecar(currentPath)), rejectedSide = Bytes(plugins::PluginSidecar(unsupported));
#endif
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", (root / L"recent.txt").c_str());
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    Check(MH_Initialize() == MH_OK, "initialize isolated function interception");
    Hook(&MessageBoxW, &DialogW); Hook(&MessageBoxA, &DialogA); Hook(&GetUiOpenFileNameW, &Browser);
    Hook(&SaveSceneFile, &Save); Hook(&NotifyEffectFileOpen, &Effect); Hook(&LocalizeUI, &Localize);
#if !MMD_ORIGINAL_PROFILE
    Hook(&plugins::BeginProjectSceneReplacement, &BeginReplacement, reinterpret_cast<void**>(&originalBegin));
#else
    originalBegin = &plugins::BeginProjectSceneReplacement;
#endif
    Hook(&TryLoadSceneV1, &LoadV1, reinterpret_cast<void**>(&originalV1));
    Hook(&TryLoadSceneV2, &LoadV2, reinterpret_cast<void**>(&originalV2));

    auto instance = std::make_unique<MMDApp>(); app = instance.get(); app->state = MMDAppState{}; g_Block = app;
    const auto module = GetModuleHandleW(nullptr); app->HInstance() = module;
    wchar_t clsName[80]{}; swprintf_s(clsName, L"MMS-project-open-%lu", GetCurrentProcessId());
    WNDCLASSW cls{}; cls.hInstance = module; cls.lpszClassName = clsName; cls.lpfnWndProc = Owner;
    Check(RegisterClassW(&cls) != 0, "register isolated owner window");
    const auto window = CreateWindowW(clsName, L"当前原稿 - MMS", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, module, nullptr);
    Check(window != nullptr, "create hidden real owner window"); app->Hwnd() = window;
    auto menu = CreateMenu(), file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 205, L"Open"); AppendMenuW(file, MF_STRING, 207, L"Save");
    AppendMenuW(file, MF_STRING | MF_CHECKED, 301, L"Remember directory");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File"); SetMenu(window, menu);
    GetCurrentDirectoryW(256, app->ExeDir());
    wcscpy_s(app->EnvFileName(), 256, currentPath.c_str());
    wcscpy_s(app->PathWorkspace().projectDirectory, 256, L"D:\\old-project-base\\");
    wcscpy_s(app->PathWorkspace().executableDirectory, 256, L"D:\\untouched-executable-base\\");
    wcscpy_s(app->DirUser(), 1000, L"D:\\old-dialog-directory\\");
    app->EnglishUI() = 1; app->CameraMode() = 1; app->SidebarWidth() = 260;
    app->WindowLayoutReady() = 1; app->SceneModified() = 1; app->CurrentFrame() = 37; app->CameraDistance() = -53.25f;
#if !MMD_ORIGINAL_PROFILE
    Check(plugins::InitializeHistoryTestRuntime(app), "initialize the production bound journal and project runtime");
    nativeMode = NativeMode::Accept;
    Check(plugins::PrepareProjectLoad(app, currentPath.c_str()) && originalBegin(app), "seed the current sidecar through real preparation and replacement");
    plugins::CommitProjectLoad(app); SeedHistory();
#endif
    nativeMode = NativeMode::RefuseBoundary;

    std::puts("STAGE rejected candidates and all UI entry points");
    for (const auto& candidate : {missing, shortHeader, badMagic, unsupported}) {
        const Identity before;
        reenter = true;
        Check(!LoadSceneFile(candidate.c_str()), "candidate preflight reports failure"); before.Same(); Released(candidate);
    }
    for (const auto& candidate : {v1, v2}) for (const auto mode : {NativeMode::RefuseBoundary, NativeMode::ThrowMemory,
        NativeMode::ThrowShortRead, NativeMode::ThrowRecord}) {
        const Identity before; nativeMode = mode;
        Check(!LoadSceneFile(candidate.c_str()), "native refusal or exception before replacement reports failure");
        before.Same(); Released(candidate);
    }
    nativeMode = NativeMode::RefuseBoundary;
    for (unsigned entry = 0; entry != 3; ++entry) {
        const Identity before; browserPath = unsupported;
        if (entry == 0) CmdOpenScene(app);
        else if (entry == 1) Drop(unsupported);
        else {
#if !MMD_ORIGINAL_PROFILE
            recent_projects::Record(app, unsupported.c_str());
            recent_projects::HandleMessage(app, WM_ENTERMENULOOP, 0, 0);
            Check(recent_projects::HandleMessage(app, WM_COMMAND, 34800, 0), "actual recent-project item consumes the selected candidate");
            recent_projects::HandleMessage(app, WM_EXITMENULOOP, 0, 0);
#endif
        }
        before.Same(); Released(unsupported);
    }
    { const Identity before; cancelBrowser = true; CmdOpenScene(app); cancelBrowser = false; before.Same(); }
    { const Identity before; const auto old = browsers; cancelConfirmation = true; CmdOpenScene(app); cancelConfirmation = false;
      Check(browsers == old, "canceling unsaved-scene confirmation never opens the browser"); before.Same(); }
    { const Identity before; Check(!LoadSceneFile(nullptr) && !LoadSceneFile(L""), "invalid empty candidate is refused");
      std::wstring longPath(256, L'x'); Check(!LoadSceneFile(longPath.c_str()), "oversized candidate is refused without truncation"); before.Same(); }
    {
        const Identity before;
        { ScopedSceneMutation active(*app);
          Check(!LoadSceneFile(v2.c_str()), "an existing scene mutation excludes nested project replacement"); }
        before.Same();
    }
    {
        const auto locked = CreateFileW(v2.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "hold an inaccessible native PMM fixture");
        const Identity before; Check(!LoadSceneFile(v2.c_str()), "sharing failure cannot accept a new save target");
        before.Same(); CloseHandle(locked); Released(v2);
    }
#if !MMD_ORIGINAL_PROFILE
    std::puts("STAGE plugin pair refusal and canceled prepared identity");
    const auto badPair = root / L"不匹配的扩展.pmm";
    Pair(badPair, Header(), "test.wrong-pair"); Write(badPair, Header() + "changed after sidecar publication");
    { const Identity before; Check(!LoadSceneFile(badPair.c_str()), "mismatched PMM and sidecar are refused before replacement"); before.Same(); Released(badPair); }
    {
        const Identity before;
        Check(plugins::PrepareProjectLoad(app, v2.c_str()), "prepare a paired candidate without accepting it");
        Check(!LoadSceneFile(v1.c_str()), "an existing prepared project excludes a second open");
        plugins::CancelProjectLoad(app); before.Same(); Released(v2);
    }
    auto* history = plugins::NativeHistory(app);
    Check(history->History(false) == MMS_OK && journalValue == 0, "old journal still undoes after every rejected candidate");
    Check(history->History(true) == MMS_OK && journalValue == 1, "old journal still redoes after every rejected candidate");
#endif
    Check(Bytes(currentPath) == originalMain && Bytes(unsupported) == rejectedMain, "failed opens and quick-save routing probes never rewrite either PMM");
#if !MMD_ORIGINAL_PROFILE
    Check(Bytes(plugins::PluginSidecar(currentPath)) == originalSide && Bytes(plugins::PluginSidecar(unsupported)) == rejectedSide,
        "failed opens preserve original and candidate sidecars byte-for-byte");
#endif
    std::puts("STAGE accepted boundary and destructive cancellation policy");
    nativeMode = NativeMode::Accept; browserPath = v2;
    CmdOpenScene(app);
    Check(!std::wcscmp(app->EnvFileName(), v2.c_str()) && app->SceneModified() == 0, "successful Open menu accepts the new save target");
    wchar_t directory[256]{}; ExtractDirFromPath(directory, v2.c_str(), 256);
    Check(!std::wcscmp(app->DirUser(), directory), "successful Open menu updates its remembered directory");
#if !MMD_ORIGINAL_PROFILE
    Check(history->UndoCount() == 0 && history->RedoCount() == 0, "accepted replacement clears the former scene journal");
#endif
    nativeMode = NativeMode::CancelAfterBoundary;
    Check(!LoadSceneFile(v1.c_str()), "destructive cancellation reports failure");
    wchar_t title[256]{}; GetWindowTextW(window, title, 256);
    Check(!app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0] && !std::wcscmp(title, L"MikuMikuDance"),
        "destructive cancellation leaves an unnamed empty scene rather than reusing either save target");
    Released(v1); Released(v2);
    plugins::Shutdown(); recent_projects::HandleMessage(app, WM_DESTROY, 0, 0);
    SetMenu(window, nullptr); DestroyMenu(menu); DestroyWindow(window); UnregisterClassW(clsName, module);
    g_Block = nullptr; app = nullptr; instance.reset(); MH_Uninitialize();
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", nullptr);
    std::printf("PASS project-open identity: %u checks; messages=%u browsers=%u save-routing=%u\n", checks, messages, browsers, saves);
    // Keep the disposable directory as evidence. The parent chooses its lifetime.
    return 0;
}
