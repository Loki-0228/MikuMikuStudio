// Real hidden-host PMM descriptor regressions. V1/V2 native parser bodies and
// the success tail remain real. Only tail notifications/errors are faulted;
// CRT observers always forward the real operation and never hide invalid fds.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "../src/io/pmm_io_common.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/project_storage.hpp"
#endif

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0, tailCalls = 0, messages = 0, missingDialogs = 0, resetThrows = 0;
MMDApp* app = nullptr;
fs::path directory, candidate, sentinelPath;
enum class Fault { None, Memory, Runtime, AbortReset };
Fault fault = Fault::None;
bool monitor = false, reuse = false, inputOpen = false, sentinelLive = false, faultThrown = false;
bool tailObserved = false;
unsigned viewRefreshCalls = 0, preCloseRefreshCalls = 0, recoveryRefreshCalls = 0;
int inputDescriptor = -1, sentinelDescriptor = -1;
unsigned inputCloses = 0, sentinelCloses = 0;
int checkedAtClose = -2, checkedBefore = -1;
using Open = errno_t (__cdecl*)(int*, const wchar_t*, int, int, int);
using Close = int (__cdecl*)(int);
Open realOpen = nullptr;
Close realClose = nullptr;
decltype(&ShowWindow) realShow = nullptr;
#if MMD_ORIGINAL_PROFILE
decltype(&PostViewRefresh) realViewRefresh = nullptr;
constexpr const char* tailSource = "PostViewRefresh in native SuccessTail";
#else
decltype(&native_mesh::ProjectOpened) realProjectOpened = nullptr;
constexpr const char* tailSource = "native_mesh::ProjectOpened";
#endif
decltype(&ResetAppState) realReset = nullptr;

void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
template<class Function, class Replacement>
void Hook(Function function, Replacement replacement, void** original = nullptr) {
    auto* address = reinterpret_cast<void*>(function);
    Check(MH_CreateHook(address, reinterpret_cast<void*>(replacement), original) == MH_OK,
        "create a passive observer or isolated fault hook");
    Check(MH_EnableHook(address) == MH_OK, "enable an isolated host hook");
}
BOOL WINAPI Hidden(HWND window, int) { return realShow(window, SW_HIDE); }
LRESULT CALLBACK NoActivation(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* creation = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(creation->style & WS_CHILD)) creation->style &= ~WS_VISIBLE;
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
errno_t __cdecl ObserveOpen(int* descriptor, const wchar_t* path, int flags, int sharing, int mode) {
    const auto status = realOpen(descriptor, path, flags, sharing, mode);
    // Observe the shell's exact read-only binary open, not a hash/sidecar
    // std::ifstream used by plugin pair preparation before that open.
    if (monitor && status == 0 && flags == 0x8000 && sharing == 0x40 && mode == 0x80 &&
        path && !std::wcscmp(path, candidate.c_str())) {
        Check(inputDescriptor == -1 && descriptor && *descriptor >= 0,
            "observe the actual shell PMM open once without replacing the parser");
        inputDescriptor = *descriptor; inputOpen = true;
    }
    return status;
}
int __cdecl ObserveClose(int descriptor) {
    if (monitor && descriptor == inputDescriptor) {
        if (inputOpen) {
            ++inputCloses; checkedAtClose = pmm_io::checkedDescriptor; inputOpen = false;
        } else if (sentinelLive) ++sentinelCloses;
    }
    // An invalid close is deliberately forwarded, so a CRT invalid-parameter
    // failure still fails the child process. No handler masks the defect.
    return realClose(descriptor);
}
int WINAPI ErrorW(HWND, LPCWSTR text, LPCWSTR caption, UINT) {
    ++messages;
    if (!monitor || fault != Fault::Memory || !faultThrown) {
        std::fwprintf(stderr, L"Unexpected PMM modal: %ls / %ls\n", caption ? caption : L"", text ? text : L"");
        Check(false, "only the requested post-close allocation error may show the native PMM memory message");
    }
    return IDOK;
}
int WINAPI ErrorA(HWND, LPCSTR text, LPCSTR caption, UINT) {
    std::fprintf(stderr, "Unexpected PMM ANSI modal: %s / %s\n", caption ? caption : "", text ? text : "");
    Check(false, "no unexpected native ANSI dialog blocks the hidden fixture"); return IDOK;
}
int ErrorMessage(MMDApp*, HWND, UiMessage message, UINT, const wchar_t*) {
    ++messages;
    Check(monitor && faultThrown && (fault == Fault::Runtime || fault == Fault::AbortReset) &&
        message == UiMessage::InvalidProjectFile, "the real shell reports a post-close runtime failure through its existing error branch");
    return IDOK;
}
INT_PTR MissingModel(MMDApp*, HINSTANCE, HWND, ProjectModelIssue issue, const wchar_t*) {
    Check(monitor && fault == Fault::AbortReset && issue == ProjectModelIssue::MissingFile,
        "the real V2 model loader reaches its missing-source abort dialog");
    ++missingDialogs; return 2;
}
void ReuseInputNumber() {
    Check(inputDescriptor >= 0 && !inputOpen && inputCloses == 1 && !sentinelLive,
        "the real native body has consumed the PMM stream before reusing its descriptor number");
    int descriptor = -1;
    Check(_wsopen_s(&descriptor, sentinelPath.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_TRUNC,
        _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 && descriptor >= 0, "open an independent real CRT sentinel");
    if (descriptor != inputDescriptor) {
        Check(_dup2(descriptor, inputDescriptor) == 0, "force the sentinel into the exact consumed PMM descriptor slot");
        Check(_close(descriptor) == 0, "close only the extra sentinel duplicate");
        descriptor = inputDescriptor;
    }
    sentinelDescriptor = descriptor; sentinelLive = true;
    Check(sentinelDescriptor == inputDescriptor, "the post-close sentinel uses the actual observed shell descriptor number");
}
void TailDiagnostic(const char* stage, MMDApp* value, const wchar_t* path) {
    const bool appMatches = value == app;
    const bool pathMatches = path && !std::wcscmp(path, candidate.c_str());
    std::fprintf(stderr,
        "DIAG PMM tail stage=%s source=%s app=%p expectedApp=%p appMatches=%u "
        "path=%p pathMatches=%u descriptor=%d inputOpen=%u inputCloses=%u "
        "checkedAtClose=%d checkedBefore=%d checkedNow=%d monitor=%u fault=%u "
        "faultThrown=%u tailObserved=%u sentinelLive=%u sentinelCloses=%u "
        "viewRefreshCalls=%u preCloseRefreshCalls=%u recoveryRefreshCalls=%u\n",
        stage, tailSource, static_cast<void*>(value), static_cast<void*>(app), unsigned(appMatches),
        static_cast<const void*>(path), unsigned(pathMatches), inputDescriptor, unsigned(inputOpen), inputCloses,
        checkedAtClose, checkedBefore, pmm_io::checkedDescriptor, unsigned(monitor), unsigned(fault),
        unsigned(faultThrown), unsigned(tailObserved), unsigned(sentinelLive), sentinelCloses,
        viewRefreshCalls, preCloseRefreshCalls, recoveryRefreshCalls);
    std::fprintf(stderr, "DIAG PMM tail actualPath=%ls candidate=%ls\n", path ? path : L"(null)", candidate.c_str());
}
void PostCloseTail(MMDApp* value, const wchar_t* path) {
    ++tailCalls;
    const bool valid = value == app && path && !std::wcscmp(path, candidate.c_str()) && inputDescriptor >= 0 &&
        !inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore;
    if (!valid) TailDiagnostic("post-close assertion", value, path);
    Check(valid,
#if MMD_ORIGINAL_PROFILE
        "the real PMM parser reaches SuccessTail PostViewRefresh after its sole close and TLS read-context release");
#else
        "the real PMM parser reaches ProjectOpened after its sole close and TLS read-context release");
#endif
    tailObserved = true;
    if (fault == Fault::Memory || fault == Fault::Runtime) {
        if (reuse) ReuseInputNumber();
        faultThrown = true;
        if (fault == Fault::Memory) throw std::bad_alloc();
        throw std::runtime_error(tailSource);
    }
    Check(fault == Fault::None, "a missing-model abort cannot accidentally reach the successful tail");
}
#if MMD_ORIGINAL_PROFILE
void ViewRefresh(MMDApp* value) {
    // Both original-profile ProjectOpened notifications and recent-project
    // Record are empty and can share one ICF RET with unrelated stubs. Keep
    // a real, non-noexcept C++ call boundary with its exact one-argument ABI.
    realViewRefresh(value);
    if (!monitor || value != app) return;
    ++viewRefreshCalls;
    // Reset's error-recovery readout is not the parser success tail. The
    // missing-model case faults Reset itself before this readout can run.
    if (faultThrown) { ++recoveryRefreshCalls; return; }
    if (inputOpen || inputCloses != 1) { ++preCloseRefreshCalls; return; }
    if (!tailObserved) PostCloseTail(value, value->EnvFileName());
}
#else
void ProjectOpened(MMDApp* value, const wchar_t* path) {
    if (monitor) PostCloseTail(value, path);
    realProjectOpened(value, path);
}
#endif
void Reset(MMDApp* value) {
    if (monitor && fault == Fault::AbortReset && !faultThrown) {
        Check(value == app && !inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore &&
            pmm_io::checkedDescriptor == checkedBefore && missingDialogs == 1,
            "the real model Abort closes its stream and consumes workspace storage before Reset");
        if (reuse) ReuseInputNumber();
        faultThrown = true; ++resetThrows;
        throw std::runtime_error("one-shot Reset failure after the real V2 missing-model Abort");
    }
    realReset(value);
}

struct Wire {
    std::vector<unsigned char> bytes;
    template<class T> void Value(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* first = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    }
    void Zero(std::size_t count) { bytes.insert(bytes.end(), count, 0); }
    void Text(const char* text, std::size_t extent) {
        const auto length = std::strlen(text); Check(length < extent, "legacy fixture text fits its actual wire field");
        bytes.insert(bytes.end(), text, text + length); Zero(extent - length);
    }
    void Header(const char* version) {
        char header[30]{}; std::memcpy(header, "Polygon Movie maker ", 20); std::memcpy(header + 20, version, 4);
        for (const auto byte : header) Value(byte);
    }
    void Write(const fs::path& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close(); Check(bool(output), "write the independently assembled PMM wire fixture");
    }
};
mdl::CameraKey CameraRoot() {
    mdl::CameraKey key{}; key.parentModel = -1; key.distance = -31.5f;
    key.target[0] = 2.25f; key.target[1] = -4.5f; key.target[2] = 8.75f;
    key.eye[0] = .125f; key.eye[1] = -.25f; key.eye[2] = .375f;
    key.fov = 43; key.perspective = 1;
    for (unsigned lane = 0; lane < 6; ++lane) {
        key.interpolation[0][lane] = key.interpolation[1][lane] = static_cast<unsigned char>(20 + lane);
        key.interpolation[2][lane] = key.interpolation[3][lane] = static_cast<unsigned char>(107 - lane);
    }
    return key;
}
mdl::LightKey LightRoot() {
    mdl::LightKey key{}; key.color[0] = .25f; key.color[1] = .5f; key.color[2] = .75f;
    key.direction[0] = .125f; key.direction[1] = -1; key.direction[2] = .375f; return key;
}
void WriteV1(const fs::path& path) {
    // A genuine V1 no-model/no-accessory stream. V1 has six option bytes and
    // a 70-byte camera record without parent dwords. Stop precisely at the
    // first optional ReadGatedTail field, where ordinary V1 EOF is supported.
    Wire wire; wire.Header("0001");
    wire.Value(std::int32_t(640)); wire.Value(std::int32_t(480)); wire.Value(std::int32_t(360)); wire.Value(43.f);
    for (const unsigned char option : {1, 0, 1, 0, 0, 0}) wire.Value(option);
    wire.Value(std::uint8_t(0)); wire.Value(std::uint8_t(0)); // selected model, model count
    const auto camera = CameraRoot();
    const auto* head = reinterpret_cast<const unsigned char*>(&camera);
    wire.bytes.insert(wire.bytes.end(), head, head + 40);
    for (unsigned lane = 0; lane < 6; ++lane) for (unsigned row = 0; row < 4; ++row) wire.Value(camera.interpolation[row][lane]);
    wire.Value(camera.perspective); wire.Value(camera.fov); wire.Value(std::uint8_t(0)); wire.Value(std::int32_t(0));
    for (const float value : {2.25f, -4.5f, 8.75f, 0.f, 0.f, 31.5f, .125f, -.25f, .375f}) wire.Value(value);
    wire.Value(std::uint8_t(1));
    const auto light = LightRoot();
    wire.Value(light.frame); wire.Value(light.previous); wire.Value(light.next);
    for (const auto value : light.color) wire.Value(value);
    for (const auto value : light.direction) wire.Value(value);
    wire.Value(std::uint8_t(0)); wire.Value(std::int32_t(0));
    for (const auto value : light.color) wire.Value(value);
    for (const auto value : light.direction) wire.Value(value);
    wire.Value(std::uint8_t(0)); wire.Value(std::int32_t(0)); wire.Value(std::uint8_t(0)); // accessory selection/scroll/count
    wire.Value(std::int32_t(12)); wire.Value(std::int32_t(0)); wire.Value(std::int32_t(0));
    wire.Value(std::int32_t(0)); // edit mode
    for (unsigned i = 0; i < 4; ++i) wire.Value(std::uint8_t(0)); // camera reference and playback flags
    wire.Value(std::int32_t(0)); wire.Value(std::int32_t(0));
    wire.Value(std::uint8_t(0)); wire.Zero(256); // no wave
    wire.Zero(12); wire.Zero(256); wire.Value(std::int32_t(0)); // no AVI: 3 dwords/path/flag
    wire.Zero(12); wire.Zero(256); wire.Value(std::uint8_t(0)); // no picture: 3 dwords/path/flag
    wire.Value(std::uint8_t(0)); wire.Value(std::uint8_t(1)); wire.Value(std::uint8_t(0)); // FPS/grid/shadow
    wire.Value(30.f); wire.Value(std::int32_t(0)); // FPS limit and capture mode
    Check(wire.bytes.size() == 1073, "V1 fixture has the exact mandatory no-object layout and a legal optional-tail EOF");
    wire.Write(path);
}
void WriteMissingV2(const fs::path& path) {
    // Valid V2 prefix through the first missing model path. The real missing
    // model dialog selects Abort before subsequent model records are read.
    Wire wire; wire.Header("0002");
    wire.Value(std::int32_t(640)); wire.Value(std::int32_t(480)); wire.Value(std::int32_t(360)); wire.Value(43.f);
    for (const unsigned char option : {1, 0, 1, 0, 0, 0, 0}) wire.Value(option);
    wire.Value(std::uint8_t(0)); wire.Value(std::uint8_t(1)); wire.Value(std::uint8_t(0)); // selected/count/model slot
    constexpr char name[] = "missing";
    wire.Value(std::uint8_t(sizeof(name) - 1));
    for (unsigned i = 0; i < sizeof(name) - 1; ++i) wire.Value(name[i]);
    wire.Value(std::uint8_t(sizeof(name) - 1));
    for (unsigned i = 0; i < sizeof(name) - 1; ++i) wire.Value(name[i]);
    wire.Text("missing-input-host-model.pmd", 256); wire.Write(path);
}
std::vector<unsigned char> Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); Check(bool(input), "read a disposable source for exact unchanged-file comparison");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Released(const fs::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "the PMM CRT stream and plugin read-lock are both released after shell return");
    CloseHandle(file);
    const auto moved = fs::path(path.native() + L".move-probe");
    Check(MoveFileExW(path.c_str(), moved.c_str(), 0) && MoveFileExW(moved.c_str(), path.c_str(), 0),
        "a fully released project can be renamed and restored without touching its bytes");
    const auto lock = fs::path(path.native() + L".mms-save.lock");
    const HANDLE pairLock = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    Check(pairLock != INVALID_HANDLE_VALUE, "the plugin PMM/sidecar pair recovery lock is not leaked");
    CloseHandle(pairLock);
#if !MMD_ORIGINAL_PROFILE
    const auto sidecar = plugins::PluginSidecar(path);
    if (fs::exists(sidecar)) {
        const HANDLE side = CreateFileW(sidecar.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(side != INVALID_HANDLE_VALUE, "the real plugin sidecar is also independently accessible"); CloseHandle(side);
    }
#endif
}
void Arm(const fs::path& path, Fault target, bool reuseNumber) {
    candidate = path; fault = target; reuse = reuseNumber; inputDescriptor = sentinelDescriptor = -1;
    inputCloses = sentinelCloses = 0; checkedAtClose = -2; inputOpen = sentinelLive = faultThrown = false;
    tailObserved = false; viewRefreshCalls = preCloseRefreshCalls = recoveryRefreshCalls = 0;
    checkedBefore = pmm_io::checkedDescriptor; monitor = true;
}
void Finish(bool loaded, bool success) {
    monitor = false;
    const bool valid = loaded == success && inputDescriptor >= 0 && !inputOpen && inputCloses == 1 &&
        checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore;
    if (!valid) {
        std::fprintf(stderr, "DIAG PMM shell loaded=%u expectedSuccess=%u\n", unsigned(loaded), unsigned(success));
        TailDiagnostic("shell-return assertion", app, app->EnvFileName());
    }
    Check(valid,
        "real PMM success and failure routes close their stream once and restore the prior read context");
    Check(!sentinelCloses, "shell cleanup never closes the independently owned sentinel during the exact reuse window");
    if (sentinelLive) {
        constexpr char text[] = "sentinel-survives";
        Check(_write(sentinelDescriptor, text, sizeof(text) - 1) == sizeof(text) - 1 &&
            _lseek(sentinelDescriptor, 0, SEEK_SET) == 0, "the reused descriptor remains writable after actual parser-tail failure");
        char actual[32]{}; bool strict = false; int count = -1;
        try { count = pmm_io::Rd(sentinelDescriptor, actual, sizeof(actual)); }
        catch (const pmm_io::ShortRead&) { strict = true; }
        Check(!strict && count == sizeof(text) - 1 && std::memcmp(actual, text, sizeof(text) - 1) == 0,
            "the reused descriptor remains readable without inheriting the consumed PMM TLS check");
        Check(_close(sentinelDescriptor) == 0, "the sentinel is closed only after the shell observation window by its real test owner");
        sentinelDescriptor = -1; sentinelLive = false;
    }
    Check(!app->SceneMutationInProgress() && !IsWindowVisible(app->MainWindow()), "the actual host leaves its mutation scope and stays hidden");
    if (!success) {
        wchar_t title[256]{}; GetWindowTextW(app->MainWindow(), title, 256);
        Check(faultThrown && !app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0] &&
            !std::wcscmp(title, L"MikuMikuDance"), "failure after real replacement leaves an unnamed scene and never restores the old save target");
        for (unsigned slot = 0; slot < 255; ++slot)
            Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot), "the real error-reset leaves no partially loaded native objects");
    }
#if !MMD_ORIGINAL_PROFILE
    auto* history = plugins::NativeHistory(app);
    Check(history && history->CheckNativeEdit() == MMS_OK && !history->UndoCount() && !history->RedoCount(),
        "the real project transaction releases its plugin/history gates after both outcomes");
#endif
    Released(candidate);
}
void MatchRoots(const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto& actual = app->CameraKeys()[0]; const auto& actualLight = app->LightKeys()[0];
    Check(actual.frame == camera.frame && actual.previous == camera.previous && actual.next == camera.next &&
        actual.distance == camera.distance && actual.fov == camera.fov && actual.perspective == camera.perspective &&
        std::memcmp(actual.target, camera.target, sizeof(camera.target)) == 0 &&
        std::memcmp(actual.eye, camera.eye, sizeof(camera.eye)) == 0 &&
        std::memcmp(actual.interpolation, camera.interpolation, sizeof(camera.interpolation)) == 0,
        "normal real V1/V2 parsing preserves the camera payload and all 24 interpolation bytes");
    Check(actualLight.frame == light.frame && actualLight.previous == light.previous && actualLight.next == light.next &&
        std::memcmp(actualLight.color, light.color, sizeof(light.color)) == 0 &&
        std::memcmp(actualLight.direction, light.direction, sizeof(light.direction)) == 0,
        "normal real V1/V2 parsing preserves the light root payload");
}
void Normal(const fs::path& path, const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto before = Bytes(path); const auto tails = tailCalls; Arm(path, Fault::None, false);
    const bool loaded = LoadSceneFile(path.c_str()); Finish(loaded, true);
    const bool valid = tailCalls == tails + 1 && !std::wcscmp(app->EnvFileName(), path.c_str()) && app->CurrentFrame() == 12;
    if (!valid) {
        std::fprintf(stderr, "DIAG PMM normal tailsBefore=%u tailsAfter=%u frame=%u\n", tails, tailCalls, unsigned(app->CurrentFrame()));
        TailDiagnostic("normal-tail assertion", app, app->EnvFileName());
    }
    Check(valid,
        "a normal real PMM load reaches the untouched success tail and accepts its exact path and playhead");
    MatchRoots(camera, light); Check(Bytes(path) == before, "a real normal PMM open never rewrites its source");
}
void TailFailure(const fs::path& path, Fault type, bool reuseNumber) {
    const auto before = Bytes(path); const auto tails = tailCalls; const auto oldMessages = messages;
    Arm(path, type, reuseNumber);
    const bool loaded = LoadSceneFile(path.c_str()); Finish(loaded, false);
    Check(tailCalls == tails + 1 && messages == oldMessages + 1 && Bytes(path) == before,
        "the actual post-close tail fault takes exactly one existing error branch and preserves the PMM file");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wchar_t executable[32768]{}; const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    Check(count > 0 && count < 32768, "resolve the isolated test executable directory");
    directory = fs::path(executable).parent_path() / L"data" / L"pmm-input-host";
    fs::create_directories(directory);
    Check(directory.native().size() + 32 < 256, "all real loader fixture paths fit the existing PMM path ABI");
    sentinelPath = directory / L"sentinel.bin";
    SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX", L"0");
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", (directory / L"recent.txt").c_str());
    Check(MH_Initialize() == MH_OK, "initialize the isolated real-host observers");
    Hook(&ShowWindow, &Hidden, reinterpret_cast<void**>(&realShow));
    Hook(&MessageBoxW, &ErrorW); Hook(&MessageBoxA, &ErrorA); Hook(&ShowUiMessage, &ErrorMessage);
    Hook(&ShowProjectModelDialog, &MissingModel);
    Hook(&_wsopen_s, &ObserveOpen, reinterpret_cast<void**>(&realOpen));
    Hook(&_close, &ObserveClose, reinterpret_cast<void**>(&realClose));
#if MMD_ORIGINAL_PROFILE
    // The real SuccessTail calls this after Input::Close in both V1 and V2.
    // The replacement forwards the complete readout before injecting a fault.
    Hook(&PostViewRefresh, &ViewRefresh, reinterpret_cast<void**>(&realViewRefresh));
#else
    Hook(&native_mesh::ProjectOpened, &ProjectOpened, reinterpret_cast<void**>(&realProjectOpened));
#endif
    Hook(&ResetAppState, &Reset, reinterpret_cast<void**>(&realReset));
    const HHOOK hide = SetWindowsHookExW(WH_CBT, NoActivation, nullptr, GetCurrentThreadId());
    Check(hide != nullptr, "prevent the isolated real host from showing or activating top-level windows");
    auto instance = std::make_unique<MMDApp>(); app = instance.get(); g_Block = app;
    std::memset(&app->state, 0, sizeof(app->state)); app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize the actual hidden production host");
    app->EnglishUI() = 2;
    const auto v2 = directory / L"native-v2.pmm", v1 = directory / L"native-v1.pmm";
    app->state.optflag[0] = 1; app->SetSelectedModelSlot(0); app->CurrentFrame() = 12;
    const auto camera = CameraRoot(); const auto light = LightRoot();
    app->CameraKeys()[0] = camera; app->LightKeys()[0] = light;
    app->CameraFov() = 43.f;
    for (unsigned i = 0; i < 3; ++i) { app->LightColor()[i] = light.color[i]; app->LightDirection()[i] = light.direction[i]; }
    wcscpy_s(app->EnvFileName(), 256, v2.c_str()); app->SceneModified() = 1;
    SaveSceneFile(app);
    const auto saved = Bytes(v2);
    Check(!app->SceneModified() && saved.size() > 30 && !std::memcmp(saved.data(), "Polygon Movie maker 0002", 24),
        "the actual native writer supplies a complete ordinary V2 PMM fixture");
    WriteV1(v1);
    std::puts("STAGE real V2 writer/parser and genuine V1 mandatory-wire/optional-EOF normal loads");
    Normal(v2, camera, light); Normal(v1, camera, light);
    for (const auto& path : {v2, v1}) for (const auto type : {Fault::Memory, Fault::Runtime})
        for (const bool reuseNumber : {false, true}) {
            std::printf("STAGE real PMM tail version=%u fault=%s reuse=%u\n", path == v1 ? 1u : 2u,
                type == Fault::Memory ? "bad_alloc" : "runtime_error", unsigned(reuseNumber));
            TailFailure(path, type, reuseNumber);
        }
    const auto missing = directory / L"missing-model-v2.pmm"; WriteMissingV2(missing);
    std::puts("STAGE real V2 missing-model Abort with one-shot post-close Reset exception and exact fd reuse");
    const auto oldTails = tailCalls; const auto beforeMissing = Bytes(missing);
    Arm(missing, Fault::AbortReset, true);
    const bool loaded = LoadSceneFile(missing.c_str()); Finish(loaded, false);
    Check(missingDialogs == 1 && resetThrows == 1 && tailCalls == oldTails && Bytes(missing) == beforeMissing,
        "the actual V2 Abort consumes its workspace once before a throwing Reset and shell recovery");
    Normal(v2, camera, light); Normal(v1, camera, light);
    ShutdownCleanup(app); instance.reset(); app = nullptr; g_Block = nullptr;
    UnhookWindowsHookEx(hide); MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
    std::printf("PASS real PMM input host: %u checks; native tails=%u; errors=%u; missing-model aborts=%u\n",
        checks, tailCalls, messages, missingDialogs);
    return 0;
}
