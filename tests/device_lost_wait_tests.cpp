// Actual FrameDriver / RecordingReadbackPass regression. Use the identical
// test source with each revision's production core; never copy the recovery
// helper into the baseline. One scenario per process, with an external timeout.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/default_effect.hpp"
#include <MinHook.h>
#include <vfw.h>
#include <dbghelp.h>
#include <intrin.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <filesystem>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace mikumikustudio { void RefreshAfterFrameApply(MMDApp* app); }
namespace {
constexpr UINT kAction = WM_APP + 0x352;
int failures = 0;
std::string entry, scenario, mode;
MMDApp* app = nullptr;
D3DRenderer* renderer = nullptr;
IDirect3DDevice9* device = nullptr;
WNDPROC originalWindow = nullptr;
DWORD testThread = 0;
bool armed = false, actionDone = false, resetFailed = false;
unsigned presents = 0, cooperatives = 0, resets = 0, beginScenes = 0;
unsigned readbacks = 0, copies = 0, invalidCopies = 0, staleCalls = 0;
unsigned workBeforeRecovery = 0, nestedCalls = 0;
unsigned stateCalls = 0;
unsigned backgroundCopies = 0;
unsigned insideOriginalReset = 0, driverResetCalls = 0;
unsigned driverCooperatives = 0;
HMODULE deviceModule = nullptr;
std::chrono::steady_clock::time_point lossStarted;

void Check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
void Require(bool value, const char* label) {
    Check(value, label);
    if (!value) std::exit(2);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected dialog: %s\n", text); std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected dialog: %ls\n", text); std::exit(2);
}
decltype(&ShowWindow) originalShow = nullptr;
BOOL WINAPI KeepHidden(HWND window, int) { return originalShow(window, SW_HIDE); }
bool InFrameCase() { return scenario.rfind("in-frame-", 0) == 0; }
bool InputObserved() { return armed && GetCurrentThreadId() == testThread; }
bool EscPressed() {
    return resets == 0 && (scenario == "in-frame-esc-reset-fail" || scenario == "in-frame-record-esc-reset-fail");
}
decltype(&GetKeyState) originalKeyState = nullptr;
decltype(&GetAsyncKeyState) originalAsyncKeyState = nullptr;
decltype(&GetKeyboardState) originalKeyboardState = nullptr;
decltype(&GetForegroundWindow) originalForeground = nullptr;
decltype(&GetFocus) originalFocus = nullptr;
SHORT WINAPI KeyState(int key) {
    if (InputObserved()) return key == VK_ESCAPE && EscPressed() ? SHORT(0x8000) : 0;
    return originalKeyState(key);
}
SHORT WINAPI AsyncKeyState(int key) {
    return InputObserved() ? 0 : originalAsyncKeyState(key);
}
BOOL WINAPI KeyboardState(PBYTE keys) {
    if (InputObserved()) { std::memset(keys, 0, 256); return TRUE; }
    return originalKeyboardState(keys);
}
HWND WINAPI Foreground() {
    if (InputObserved()) return EscPressed() ? app->MainWindow() : nullptr;
    return originalForeground();
}
HWND WINAPI Focus() { return InputObserved() ? app->MainWindow() : originalFocus(); }
template<class Function>
void Hook(Function target, Function detour, Function* original = nullptr) {
    Require(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original)) == MH_OK, "create test hook");
    Require(MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK, "enable test hook");
}
bool Observed(IDirect3DDevice9* value) {
    return armed && value == device && GetCurrentThreadId() == testThread;
}
HMODULE AddressModule(const void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}
void PrintCallStack(const char* api, const void* caller, bool driverInternal) {
    void* frames[32]{};
    const USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
    // Load only the system symbol reader and use local symbols. No symbol-server
    // path or link dependency is needed by either revision's test executable.
    static const HMODULE symbols = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto initialize = reinterpret_cast<decltype(&SymInitializeW)>(GetProcAddress(symbols, "SymInitializeW"));
    static const auto setOptions = reinterpret_cast<decltype(&SymSetOptions)>(GetProcAddress(symbols, "SymSetOptions"));
    static const auto fromAddress = reinterpret_cast<decltype(&SymFromAddr)>(GetProcAddress(symbols, "SymFromAddr"));
    static const auto fromLine = reinterpret_cast<decltype(&SymGetLineFromAddr64)>(GetProcAddress(symbols, "SymGetLineFromAddr64"));
    static const bool initialized = [] {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto directory = std::filesystem::path(executable).parent_path();
        if (setOptions) setOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                                   SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_NT_SYMPATH);
        return initialize && initialize(GetCurrentProcess(), directory.c_str(), TRUE);
    }();
    std::printf("CALL_STACK api=%s caller=%p inside_original_reset=%u driver_internal=%d\n",
                api, caller, insideOriginalReset, driverInternal ? 1 : 0);
    for (USHORT index = 0; index != count; ++index) {
        const HMODULE module = AddressModule(frames[index]);
        char path[MAX_PATH]{};
        if (module) GetModuleFileNameA(module, path, MAX_PATH);
        std::printf("  #%u %p %s+0x%llX", index, frames[index], path,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[index]) -
                                            reinterpret_cast<std::uintptr_t>(module)));
        alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        const auto address = reinterpret_cast<DWORD64>(frames[index]);
        if (initialized && fromAddress && fromAddress(GetCurrentProcess(), address, &displacement, symbol))
            std::printf(" %s+0x%llX", symbol->Name, static_cast<unsigned long long>(displacement));
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof line;
        DWORD lineDisplacement = 0;
        if (initialized && fromLine && fromLine(GetCurrentProcess(), address, &lineDisplacement, &line))
            std::printf(" %s:%lu", line.FileName, line.LineNumber);
        std::printf("\n");
    }
}
void ObserveCall(const char* api, bool rendering, const void* caller) {
    if (app->Renderer() != renderer || renderer->device != device) ++staleCalls;
    if (rendering && resetFailed) {
        // Only calls made by the device's own DLL inside the actual driver
        // Reset are initialization. Application / window callback callers and
        // every call before or after originalReset remain violations.
        const bool driverInternal = insideOriginalReset != 0 && deviceModule != nullptr &&
                                    AddressModule(caller) == deviceModule;
        if (driverInternal) {
            ++driverResetCalls;
        } else {
            if (++workBeforeRecovery <= 8) PrintCallStack(api, caller, false);
        }
    }
}

using Present = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Cooperative = HRESULT(WINAPI*)(IDirect3DDevice9*);
using Reset = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using BeginScene = HRESULT(WINAPI*)(IDirect3DDevice9*);
using CreateTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using Copy = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
    IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using Readback = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
using SetViewport = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetTransform = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using GetBackBuffer = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
using CreateTexture = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD,
    D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateOffscreen = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DPOOL, IDirect3DSurface9**, HANDLE*);
Present originalPresent = nullptr;
Cooperative originalCooperative = nullptr;
Reset originalReset = nullptr;
BeginScene originalBeginScene = nullptr;
CreateTarget originalCreateTarget = nullptr;
Copy originalCopy = nullptr;
Readback originalReadback = nullptr;
SetViewport originalSetViewport = nullptr;
SetTransform originalSetTransform = nullptr;
SetTarget originalSetTarget = nullptr;
GetBackBuffer originalGetBackBuffer = nullptr;
CreateTexture originalCreateTexture = nullptr;
CreateOffscreen originalCreateOffscreen = nullptr;

bool UsesAction() {
    return scenario == "message-ready" || scenario == "paused-background-seek" || scenario == "cancel" ||
        scenario == "replace-device" || scenario == "replace-slot" ||
        scenario == "invalidate-surfaces" || scenario == "reenter" || scenario == "message-reset-fail";
}
HRESULT WINAPI OnPresent(IDirect3DDevice9* value, const RECT* source,
    const RECT* target, HWND window, const RGNDATA* dirty) {
    if (!Observed(value)) return originalPresent(value, source, target, window, dirty);
    ObserveCall("Present", true, _ReturnAddress());
    ++presents;
    if (!InFrameCase() && scenario != "ok" && scenario != "direct-reset-ok" && scenario != "direct-reset-fail" && presents == 1) {
        lossStarted = std::chrono::steady_clock::now();
        if (scenario == "quit") PostQuitMessage(37);
        if (UsesAction()) Require(PostMessageW(app->MainWindow(), kAction, 0, 0) != FALSE,
                                 "queue action from failing Present");
        return D3DERR_DEVICELOST;
    }
    return originalPresent(value, source, target, window, dirty);
}
HRESULT WINAPI OnCooperative(IDirect3DDevice9* value) {
    if (!Observed(value)) return originalCooperative(value);
    const void* caller = _ReturnAddress();
    const bool driverInternal = insideOriginalReset != 0 && deviceModule != nullptr &&
                                AddressModule(caller) == deviceModule;
    // Confirmed driver-internal queries use the real result and their own
    // counter. Keep stacks for unexpected application-side recovery queries.
    if (scenario == "direct-reset-ok" && !driverInternal)
        PrintCallStack("TestCooperativeLevel", caller, false);
    if (driverInternal) {
        ++driverCooperatives;
        return originalCooperative(value);
    }
    ObserveCall("TestCooperativeLevel", false, caller);
    ++cooperatives;
    if (scenario == "ready") return D3D_OK;
    if (scenario == "lost-ready") return cooperatives <= 2 ? D3DERR_DEVICELOST : D3D_OK;
    if (scenario == "lost-reset") {
        return std::chrono::steady_clock::now() - lossStarted < std::chrono::milliseconds(200)
            ? D3DERR_DEVICELOST : D3DERR_DEVICENOTRESET;
    }
    if (scenario == "quit") return D3DERR_DEVICELOST;
    if (scenario == "fatal") return D3DERR_DRIVERINTERNALERROR;
    if (scenario == "message-ready" || scenario == "paused-background-seek")
        return actionDone ? D3D_OK : D3DERR_DEVICELOST;
    // The next cooperative check after a failed Reset deliberately says S_OK:
    // OnLostDevice/resources still need a successful reset before rendering.
    if ((InFrameCase() || scenario == "reset-fail" || scenario == "message-reset-fail" || scenario == "direct-reset-fail") &&
        resets != 0) return D3D_OK;
    if (UsesAction() && !actionDone) return D3DERR_DEVICELOST;
    return D3DERR_DEVICENOTRESET;
}
HRESULT WINAPI OnReset(IDirect3DDevice9* value, D3DPRESENT_PARAMETERS* parameters) {
    if (!Observed(value)) return originalReset(value, parameters);
    ObserveCall("Reset", false, _ReturnAddress());
    ++resets;
    if ((InFrameCase() || scenario == "reset-fail" || scenario == "message-reset-fail" || scenario == "direct-reset-fail") &&
        resets <= 2) {
        resetFailed = true;
        return D3DERR_DEVICELOST;
    }
    ++insideOriginalReset;
    const HRESULT result = originalReset(value, parameters);
    --insideOriginalReset;
    if (SUCCEEDED(result)) resetFailed = false;
    return result;
}
HRESULT WINAPI OnBeginScene(IDirect3DDevice9* value) {
    if (Observed(value)) { ObserveCall("BeginScene", true, _ReturnAddress()); ++beginScenes; }
    return originalBeginScene(value);
}
HRESULT WINAPI OnCreateTarget(IDirect3DDevice9* value, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9** target, HANDLE* shared) {
    if (Observed(value)) ObserveCall("CreateRenderTarget", true, _ReturnAddress());
    return originalCreateTarget(value, width, height, format, samples, quality, lockable, target, shared);
}
HRESULT WINAPI OnCopy(IDirect3DDevice9* value, IDirect3DSurface9* source, const RECT* sourceRect,
    IDirect3DSurface9* target, const RECT* targetRect, D3DTEXTUREFILTERTYPE filter) {
    if (Observed(value)) {
        ObserveCall("StretchRect", true, _ReturnAddress()); ++copies;
        if (source && source == app->AviBackgroundSurface()) ++backgroundCopies;
        if (!source || !target) { ++invalidCopies; return D3DERR_INVALIDCALL; }
    }
    return originalCopy(value, source, sourceRect, target, targetRect, filter);
}
HRESULT WINAPI OnReadback(IDirect3DDevice9* value, IDirect3DSurface9* source, IDirect3DSurface9* target) {
    if (Observed(value)) {
        ObserveCall("GetRenderTargetData", true, _ReturnAddress()); ++readbacks;
        if (!source || !target) { ++invalidCopies; return D3DERR_INVALIDCALL; }
    }
    return originalReadback(value, source, target);
}
HRESULT WINAPI OnSetViewport(IDirect3DDevice9* value, const D3DVIEWPORT9* viewport) {
    if (Observed(value)) { ObserveCall("SetViewport", true, _ReturnAddress()); ++stateCalls; }
    return originalSetViewport(value, viewport);
}
HRESULT WINAPI OnSetTransform(IDirect3DDevice9* value, D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) {
    if (Observed(value)) { ObserveCall("SetTransform", true, _ReturnAddress()); ++stateCalls; }
    return originalSetTransform(value, type, matrix);
}
HRESULT WINAPI OnSetTarget(IDirect3DDevice9* value, DWORD index, IDirect3DSurface9* target) {
    if (Observed(value)) { ObserveCall("SetRenderTarget", true, _ReturnAddress()); ++stateCalls; }
    return originalSetTarget(value, index, target);
}
HRESULT WINAPI OnGetBackBuffer(IDirect3DDevice9* value, UINT swapChain, UINT index,
    D3DBACKBUFFER_TYPE type, IDirect3DSurface9** target) {
    if (Observed(value)) { ObserveCall("GetBackBuffer", true, _ReturnAddress()); ++stateCalls; }
    return originalGetBackBuffer(value, swapChain, index, type, target);
}
HRESULT WINAPI OnCreateTexture(IDirect3DDevice9* value, UINT width, UINT height, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** target, HANDLE* shared) {
    if (Observed(value)) { ObserveCall("CreateTexture", true, _ReturnAddress()); ++stateCalls; }
    return originalCreateTexture(value, width, height, levels, usage, format, pool, target, shared);
}
HRESULT WINAPI OnCreateOffscreen(IDirect3DDevice9* value, UINT width, UINT height,
    D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9** target, HANDLE* shared) {
    if (Observed(value)) { ObserveCall("CreateOffscreenPlainSurface", true, _ReturnAddress()); ++stateCalls; }
    return originalCreateOffscreen(value, width, height, format, pool, target, shared);
}

void LoadBackgroundFixture(bool twoColors = false) {
    // The same uncompressed VFW fixture format as background_overlay_tests.
    // Keep this test-owned file beside the case's raw logs for reproducibility.
    const auto path = std::filesystem::absolute(L"device-lost-background.avi");
    Require(path.native().size() < 256, "background fixture path fits production path storage");
    AVIFileInit(); // ShutdownCleanup balances the host's VFW lifetime.
    PAVIFILE file = nullptr;
    Require(AVIFileOpenW(&file, path.c_str(), OF_WRITE | OF_CREATE, nullptr) == 0, "create AVI background fixture");
    AVISTREAMINFOW streamInfo{};
    streamInfo.fccType = streamtypeVIDEO; streamInfo.dwScale = 1; streamInfo.dwRate = 30;
    streamInfo.rcFrame = {0, 0, 16, 16};
    PAVISTREAM stream = nullptr;
    Require(AVIFileCreateStreamW(file, &stream, &streamInfo) == 0, "create AVI background stream");
    BITMAPINFOHEADER format{};
    format.biSize = sizeof format; format.biWidth = format.biHeight = 16;
    format.biPlanes = 1; format.biBitCount = 24; format.biSizeImage = 16 * 16 * 3;
    Require(AVIStreamSetFormat(stream, 0, &format, sizeof format) == 0, "set uncompressed AVI format");
    std::vector<unsigned char> pixels(format.biSizeImage, 255);
    for (LONG frame = 0; frame != 2; ++frame) {
        if (twoColors) for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 3) {
            pixels[pixel] = frame == 1 ? 255 : 0;
            pixels[pixel + 1] = 0;
            pixels[pixel + 2] = frame == 0 ? 255 : 0;
        }
        Require(AVIStreamWrite(stream, frame, 1, pixels.data(), static_cast<LONG>(pixels.size()),
                AVIIF_KEYFRAME, nullptr, nullptr) == 0, "write AVI background frame");
    }
    AVIStreamRelease(stream); AVIFileRelease(file);
    wcscpy_s(app->AviBackgroundPath(), 256, path.c_str());
    LoadAviFile(app);
    Require(app->AviBackgroundEnabled() != 0 && app->AviFrameReader() != nullptr &&
            app->AviBackgroundTexture() != nullptr, "actual loader opens the AVI background");
}
void CheckBackgroundPixels(DWORD expected = 0x00FFFFFF) {
    const bool previousArmed = armed;
    armed = false; // Diagnostic readback is not a delivered recording sample.
    IDirect3DSurface9 *source = nullptr, *system = nullptr;
    Require(app->AviBackgroundTexture() != nullptr, "background texture exists after recovery");
    Require(SUCCEEDED(app->AviBackgroundTexture()->GetSurfaceLevel(0, &source)), "get background texture level");
    D3DSURFACE_DESC desc{};
    Require(SUCCEEDED(source->GetDesc(&desc)), "get background texture format");
    Require(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &system, nullptr)), "create diagnostic background readback");
    Require(SUCCEEDED(device->GetRenderTargetData(source, system)), "read actual background GPU texture");
    D3DLOCKED_RECT locked{};
    Require(SUCCEEDED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY)), "lock background diagnostic pixels");
    const auto* row = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(locked.pBits) +
                                                    (desc.Height / 2) * locked.Pitch);
    const DWORD actual = row[desc.Width / 2] & 0x00FFFFFF;
    std::printf("BACKGROUND actual=%06lX expected=%06lX\n", actual, expected);
    Check(actual == expected, "background center matches the requested source frame");
    system->UnlockRect(); system->Release(); source->Release();
    armed = previousArmed;
}

bool Invoke() {
    app->DeltaTime() = 1.f / 30;
    app->state.messageSeen = 1;
    if (entry == "readback") return RecordingReadbackPass(app);
    FrameDriver(app);
    return true; // FrameDriver is void; counters validate its actual work.
}
template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }
LRESULT CALLBACK OnWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (armed && message == kAction) {
        actionDone = true;
        if (scenario == "cancel") FinishAviRecord(app);
        if (scenario == "paused-background-seek") {
            app->CurrentFrame() = 1;
            app->PlaybackCursorSeconds() = 1.f / 30;
            RefreshAfterFrameApply(app);
        }
        if (scenario == "message-reset-fail")
            Check(!PostDeviceReset(app), "window message reaches actual failed PostDeviceReset");
        if (scenario == "replace-device") app->Renderer() = nullptr;
        if (scenario == "replace-slot") renderer->device = nullptr;
        if (scenario == "invalidate-surfaces") {
            Release(app->CaptureRenderTarget());
            Release(app->CaptureSystemSurface());
            Release(renderer->backbufferSurface);
        }
        if (scenario == "reenter") { ++nestedCalls; Invoke(); }
        return 0;
    }
    return CallWindowProcW(originalWindow, window, message, wparam, lparam);
}
void DrainMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "startup did not request exit");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
unsigned long long ThreadTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != FALSE,
            "read test thread CPU time");
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}
bool Known(const std::string& value, std::initializer_list<const char*> choices) {
    for (const char* choice : choices) if (value == choice) return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i += 2) {
        Require(i + 1 < argc, "each option needs a value");
        if (std::strcmp(argv[i], "--entry") == 0) entry = argv[i + 1];
        else if (std::strcmp(argv[i], "--case") == 0) scenario = argv[i + 1];
        else if (std::strcmp(argv[i], "--mode") == 0) mode = argv[i + 1];
        else Require(false, "unknown option");
    }
    Require(Known(entry, {"frame", "readback", "record-frame"}), "entry is frame, readback, or record-frame");
    Require(Known(mode, {"windowed", "fullscreen"}), "mode is windowed or fullscreen");
    Require(Known(scenario, {"ok", "ready", "lost-ready", "lost-reset", "message-ready", "paused-background-seek", "quit",
        "reset-ok", "reset-fail", "message-reset-fail", "direct-reset-ok", "direct-reset-fail",
        "in-frame-esc-reset-fail", "in-frame-timeline-reset-fail", "in-frame-record-end-reset-fail", "in-frame-record-esc-reset-fail",
        "cancel", "replace-device", "replace-slot", "invalidate-surfaces", "reenter", "fatal"}),
        "known scenario");
    Require(entry != "frame" || (scenario != "cancel" && scenario != "invalidate-surfaces"),
            "capture scenarios need a recording entry");
    if (scenario == "in-frame-esc-reset-fail")
        Require(entry == "frame" && mode == "fullscreen", "fullscreen ESC scenario needs frame/fullscreen");
    if (scenario == "in-frame-timeline-reset-fail") Require(entry == "frame", "timeline scenario needs frame entry");
    if (scenario == "paused-background-seek") Require(entry == "frame", "paused seek needs frame entry");
    if (scenario == "in-frame-record-end-reset-fail" || scenario == "in-frame-record-esc-reset-fail")
        Require(entry == "record-frame", "recording completion/cancel needs full production frame entry");
    std::printf("SCENARIO entry=%s case=%s mode=%s (hidden windowed device; fullscreen code branch only)\n",
        entry.c_str(), scenario.c_str(), mode.c_str());
    SetProcessDPIAware();
    testThread = GetCurrentThreadId();
    Require(MH_Initialize() == MH_OK, "initialize hooks");
    Hook(&ShowWindow, &KeepHidden, &originalShow);
    Hook(&MessageBoxA, &UnexpectedA);
    Hook(&MessageBoxW, &UnexpectedW);
    // Supply deterministic input to the real polling path without touching
    // desktop focus or the user's keyboard state.
    Hook(&GetKeyState, &KeyState, &originalKeyState);
    Hook(&GetAsyncKeyState, &AsyncKeyState, &originalAsyncKeyState);
    Hook(&GetKeyboardState, &KeyboardState, &originalKeyboardState);
    Hook(&GetForegroundWindow, &Foreground, &originalForeground);
    Hook(&GetFocus, &Focus, &originalFocus);
    app = new MMDApp;
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    Require(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize actual production host");
    Require(!IsWindowVisible(app->MainWindow()), "host stays hidden");
    app->DeltaTime() = 1.f / 30;
    app->state.messageSeen = 1;
    FrameDriver(app);
    DrainMessages();
    renderer = app->Renderer();
    Require(renderer && renderer->device, "production device exists");
    // The compiler finishes asynchronously after the first presentation. Its
    // normal attachment checks TestCooperativeLevel, so finish that startup
    // work before counting calls made by the scripted recovery scenario.
    const auto effectDeadline = GetTickCount64() + 15000;
    while (!renderer->effect && GetTickCount64() < effectDeadline) {
        UpdateDefaultEffect(app);
        DrainMessages();
        if (!renderer->effect) Sleep(5);
    }
    Require(renderer->effect != nullptr, "default effect is ready before recovery counters are armed");
    device = renderer->device;
    // Only the device is retained; DEFAULT-pool surfaces must remain releasable
    // by the real PostDeviceReset. No stub replaces FrameDriver or Reset flow.
    device->AddRef();
    Require(renderer->presentParameters.Windowed != FALSE, "test does not switch display mode");
    originalWindow = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(app->MainWindow(), GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&OnWindow)));
    Require(originalWindow != nullptr, "subclass real host window");
    auto** vtable = *reinterpret_cast<void***>(device);
    deviceModule = AddressModule(vtable[16]);
    Hook(reinterpret_cast<Cooperative>(vtable[3]), &OnCooperative, &originalCooperative);
    Hook(reinterpret_cast<Reset>(vtable[16]), &OnReset, &originalReset);
    Hook(reinterpret_cast<Present>(vtable[17]), &OnPresent, &originalPresent);
    Hook(reinterpret_cast<CreateTarget>(vtable[28]), &OnCreateTarget, &originalCreateTarget);
    Hook(reinterpret_cast<Readback>(vtable[32]), &OnReadback, &originalReadback);
    Hook(reinterpret_cast<Copy>(vtable[34]), &OnCopy, &originalCopy);
    Hook(reinterpret_cast<BeginScene>(vtable[41]), &OnBeginScene, &originalBeginScene);
    Hook(reinterpret_cast<SetViewport>(vtable[47]), &OnSetViewport, &originalSetViewport);
    Hook(reinterpret_cast<SetTransform>(vtable[44]), &OnSetTransform, &originalSetTransform);
    Hook(reinterpret_cast<SetTarget>(vtable[37]), &OnSetTarget, &originalSetTarget);
    Hook(reinterpret_cast<GetBackBuffer>(vtable[18]), &OnGetBackBuffer, &originalGetBackBuffer);
    Hook(reinterpret_cast<CreateTexture>(vtable[23]), &OnCreateTexture, &originalCreateTexture);
    Hook(reinterpret_cast<CreateOffscreen>(vtable[36]), &OnCreateOffscreen, &originalCreateOffscreen);
    const bool recording = entry != "frame";
    app->FullscreenMode() = mode == "fullscreen" ? 1 : 0;
    if (recording) {
        Require(app->Recorder() && !app->Recorder()->framePush, "recording fixture has no active writer");
        app->AviStereoOutput() = 0;
        app->AviOutputWidth() = 64; app->AviOutputHeight() = 64;
        app->AviRecordStartFrame() = 0; app->AviRecordEndFrame() = 1000;
        app->AviRecordFps() = 30;
        app->AviCloseAfterExport() = 0;
        app->state.recRTW = renderer->screenWidth;
        app->state.recRTH = renderer->screenHeight;
        app->state.frameStepPlayback = 1;
        app->state.playbackActive = 1;
        app->state.playbackReturnsToStartFrame = 0;
        app->state.recordPlaybackStartPending = 0;
        app->state.recordedFrameCount = 7;
        app->CurrentFrame() = 7;
        app->PlaybackCursorSeconds() = 7.f / 30;
    }
    if (InFrameCase()) {
        app->RenderWidth() = 64; app->RenderHeight() = 64;
        app->state.escKeyState = 0;
        Require(renderer->screenWidth > 64 && renderer->screenHeight > 64, "fixture has room for a restore-size Reset");
        app->state.recRTW = renderer->screenWidth - 1;
        app->state.recRTH = renderer->screenHeight - 1;
        if (scenario == "in-frame-record-esc-reset-fail" && mode == "fullscreen") {
            // The fullscreen exit supplies the failing Reset. Cancellation
            // still must clean up even though a second size Reset is unneeded.
            app->state.recRTW = renderer->screenWidth;
            app->state.recRTH = renderer->screenHeight;
        }
        if (scenario == "in-frame-timeline-reset-fail") {
            app->state.timelineAdvanceDue = 1;
            wcscpy_s(app->CaptureSavePath(), 256, L"device-lost-timeline.bmp");
        }
        if (scenario == "in-frame-record-end-reset-fail") {
            app->AviRecordEndFrame() = 7;
            Require(app->RecordingCompletionFlag() != nullptr, "recording completion flag exists");
            *app->RecordingCompletionFlag() = 1;
            LoadBackgroundFixture();
            CheckBackgroundPixels();
        }
    }
    if (scenario == "paused-background-seek") {
        app->CurrentFrame() = 0;
        app->state.playbackActive = 0;
        app->state.frameStepPlayback = 0;
        LoadBackgroundFixture(true);
        CheckBackgroundPixels(0x00FF0000);
    }
    auto* const initialBackground = app->AviBackgroundTexture();
    const int initialCount = app->state.recordedFrameCount;
    const int initialFrame = app->CurrentFrame();
    const auto start = std::chrono::steady_clock::now();
    const auto cpuStart = ThreadTicks();
    armed = true;
    const bool directReset = scenario == "direct-reset-fail" || scenario == "direct-reset-ok";
    if (directReset) {
        const bool reset = PostDeviceReset(app);
        Check(reset == (scenario == "direct-reset-ok"), "direct PostDeviceReset reports actual scripted result");
        Check(resets == 1, "direct path executes one Reset");
    }
    const bool firstResult = Invoke();
    const unsigned firstPresents = presents, firstBegins = beginScenes, firstReadbacks = readbacks;
    const int firstCount = app->state.recordedFrameCount, firstFrame = app->CurrentFrame();
    if (InFrameCase()) {
        Check(resets == 1, "actual frame input/capture/recording tail reaches failed Reset");
        Check(workBeforeRecovery == 0, "failed in-frame Reset stops before the next D3D call");
        const bool completedRecording = scenario == "in-frame-record-end-reset-fail";
        Check(firstPresents == (completedRecording ? 1u : 0u), "no tail Present follows in-frame failure");
        if (scenario == "in-frame-esc-reset-fail" || scenario == "in-frame-record-esc-reset-fail")
            Check(firstBegins == 0 && firstReadbacks == 0, "failed ESC transition cannot begin rendering");
        else Check(firstBegins != 0, "capture or recording tail follows actual scene rendering");
        if (recording) {
            Check(app->state.frameStepPlayback == 0 && app->state.playbackActive == 0,
                  "recording completion/cancel still clears playback flags");
            Check(!app->CaptureRenderTarget() && !app->CaptureSystemSurface() && !app->CaptureReadbackPixels(),
                  "recording completion/cancel still releases capture resources");
            Check(firstCount == initialCount + (completedRecording ? 1 : 0), "only an actually delivered recording sample advances the count");
            Check(firstReadbacks == (completedRecording ? 1u : 0u), "cancel sends no sample; completed sample is read back once");
            if (completedRecording) Check(app->AviBackgroundEnabled() != 0 && app->AviFrameReader() != nullptr &&
                                          app->AviBackgroundTexture() == nullptr,
                                          "failed Reset preserves the video source and defers GPU texture recreation");
        }
        if (scenario == "in-frame-timeline-reset-fail")
            Check(app->state.timelineAdvanceDue == 0 && copies != 0, "capture completes once and clears its pending flag");
        const unsigned firstStates = stateCalls;
        Invoke();
        Check(resets == 2 && presents == firstPresents && beginScenes == firstBegins &&
              readbacks == firstReadbacks && stateCalls == firstStates,
              "next frame retries failed in-frame Reset before any D3D state/render/capture call");
        Check(app->state.recordedFrameCount == firstCount, "failed retry never advances the completed or cancelled recording");
        Invoke();
        Check(resets == 3 && !resetFailed && presents > firstPresents, "successful retry resumes normal rendering");
        Check(app->state.recordedFrameCount == firstCount, "recovery does not restart a completed or cancelled recording");
        if (completedRecording) {
            Check(app->AviBackgroundEnabled() != 0 && app->AviFrameReader() != nullptr,
                  "recording cleanup preserves the loaded background source");
            CheckBackgroundPixels();
        }
    } else if (scenario == "direct-reset-fail") {
        Check(resets == 2, "failed direct Reset is retried on S_OK before frame work");
        Check(presents == 0 && beginScenes == 0 && readbacks == 0,
              "direct failure and failed retry perform no rendering or capture");
        Check(firstCount == initialCount && firstFrame == initialFrame, "direct failure does not advance the sample");
        Invoke();
        Check(resets == 3 && !resetFailed, "direct failure recovers through a successful actual Reset");
        Check(presents != 0, "successful direct recovery resumes production entry");
        if (recording) Check(readbacks == 1 && app->state.frameStepPlayback == 1,
                            "direct recovery preserves recording and delivers one sample");
        if (entry == "record-frame") Check(app->state.recordedFrameCount == initialCount + 1,
                                          "direct recovery advances exactly once");
    } else {
        Check(presents != 0, "real entry reaches Present");
        Check(invalidCopies == 0, "no copy uses released surfaces");
        Check(staleCalls == 0, "no call uses the detached device");
        if (entry != "readback") Check(beginScenes != 0, "real entry renders before initial loss");
        if (scenario == "ok" || scenario == "direct-reset-ok") {
            Check(firstResult && readbacks == (recording ? 1u : 0u), "normal pass completes actual readback when recording");
            Check(cooperatives == 0 && resets == (directReset ? 1u : 0u),
                  "normal pass or successful direct Reset adds no recovery calls");
            if (entry == "record-frame") Check(firstCount == initialCount + 1, "successful recording advances exactly one sample");
        } else if (recording) {
            if (entry == "readback") Check(!firstResult, "lost sample returns false to caller");
            Check(readbacks == 0, "lost sample is not delivered to writer");
            Check(firstCount == initialCount && firstFrame == initialFrame, "lost sample does not advance recording count or timeline");
            Check(app->state.frameStepPlayback == (scenario == "cancel" ? 0 : 1),
                  "temporary loss preserves recording; explicit cancel ends it");
        }
        if (UsesAction()) Check(actionDone, "wait dispatches queued host action");
        if (scenario == "paused-background-seek") {
            Check(resets == 0 && app->CurrentFrame() == 1 && app->state.playbackActive == 0,
                  "message seeks the paused background while S_OK recovers without Reset");
            Check(app->AviBackgroundTexture() == initialBackground && backgroundCopies == 0,
                  "waiting defers the upload while retaining the existing texture");
            CheckBackgroundPixels(0x00FF0000);
            Invoke();
            Check(app->AviBackgroundTexture() == initialBackground && backgroundCopies == 1 && resets == 0,
                  "first recovered paused frame uploads the deferred seek exactly once");
            CheckBackgroundPixels(0x000000FF);
            Invoke();
            Check(backgroundCopies == 1 && resets == 0, "next paused frame does not repeat a completed background upload");
            CheckBackgroundPixels(0x000000FF);
        }
        if (scenario == "ready" || scenario == "lost-ready" || scenario == "message-ready")
            Check(resets == 0, "cooperative S_OK recovers without Reset");
        if (scenario == "reset-ok" || scenario == "lost-reset") Check(resets == 1, "NOTRESET executes one real Reset");
        if (scenario == "quit") {
            MSG message{};
            const bool quit = PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE;
            Check(quit && message.wParam == 37, "WM_QUIT and original exit code remain for outer loop");
        }
        if (scenario == "reenter") {
            Check(nestedCalls == 1, "host message attempts real entry recursively");
            Check(presents == 1 && readbacks == 0, "recursive entry cannot render or capture while waiting");
        }
        if (recording && failures == 0 && Known(scenario,
                {"ready", "lost-ready", "lost-reset", "message-ready", "reset-ok", "reenter"})) {
            Invoke();
            Check(readbacks == 1 && app->state.frameStepPlayback == 1,
                  "next recording pass recreates resources and delivers one sample");
            if (entry == "record-frame") Check(app->state.recordedFrameCount == initialCount + 1,
                                              "retried sample advances the recording counter once");
        }
        if (scenario == "reset-fail") {
            Check(resets == 1, "first failed Reset stops recovery attempt");
            Invoke();
            Check(resets == 2, "next entry retries Reset even when cooperative status is S_OK");
            Check(presents == firstPresents && beginScenes == firstBegins && readbacks == firstReadbacks,
                  "failed retry happens before any rendering, Present, or readback");
            if (recording) Check(app->state.recordedFrameCount == initialCount && app->CurrentFrame() == initialFrame,
                                "failed retry keeps the exact recording sample");
            Invoke();
            Check(resets == 3 && !resetFailed, "third attempt performs successful actual Reset");
            Check(presents > firstPresents, "rendering resumes after successful Reset");
            if (recording) Check(readbacks == 1 && app->state.frameStepPlayback == 1,
                                "recording resumes with one newly rendered sample");
            if (entry == "record-frame") Check(app->state.recordedFrameCount == initialCount + 1,
                                              "recovery advances recording exactly once");
        }
        if (scenario == "message-reset-fail") {
            Check(resets >= 1 && resets <= 2, "message Reset and optional immediate retry both fail");
            if (resets == 1) {
                Invoke();
                Check(resets == 2, "message failure retries Reset despite S_OK");
                Check(presents == firstPresents && beginScenes == firstBegins && readbacks == firstReadbacks,
                      "failed message Reset retry precedes all frame work");
            }
            Invoke();
            Check(resets == 3 && !resetFailed, "message failure requires a successful actual Reset");
            Check(presents > firstPresents, "successful message Reset recovery resumes rendering");
            if (recording) Check(readbacks == 1 && app->state.frameStepPlayback == 1,
                                "message Reset recovery resumes one recording sample");
            if (entry == "record-frame") Check(app->state.recordedFrameCount == initialCount + 1,
                                              "message Reset recovery advances exactly once");
        }
        if (scenario == "fatal") {
            Invoke();
            Check(presents == firstPresents && beginScenes == firstBegins && readbacks == firstReadbacks,
                  "unrecoverable status never falls through to next-frame rendering");
        }
    }
    Check(workBeforeRecovery == 0, "no render or capture work follows failed Reset before recovery");
    Check(invalidCopies == 0 && staleCalls == 0, "all later calls preserve device and surface validity");
    const auto cpuTicks = ThreadTicks() - cpuStart;
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("RESULT {\"entry\":\"%s\",\"case\":\"%s\",\"mode\":\"%s\",\"wall_ms\":%.3f,"
        "\"thread_cpu_ms\":%.3f,\"presents\":%u,\"cooperatives\":%u,\"resets\":%u,\"begin_scenes\":%u,"
        "\"readbacks\":%u,\"copies\":%u,\"background_copies\":%u,\"state_calls\":%u,\"invalid_copies\":%u,\"stale_calls\":%u,"
        "\"work_before_recovery\":%u,\"driver_reset_calls\":%u,\"driver_cooperatives\":%u,\"initial_count\":%d,\"first_count\":%d,\"final_count\":%d,\"failures\":%d}\n",
        entry.c_str(), scenario.c_str(), mode.c_str(), elapsed, cpuTicks / 10000.0,
        presents, cooperatives, resets, beginScenes, readbacks, copies, backgroundCopies, stateCalls, invalidCopies, staleCalls,
        workBeforeRecovery, driverResetCalls, driverCooperatives, initialCount, firstCount, app->state.recordedFrameCount, failures);
    armed = false;
    app->Renderer() = renderer;
    renderer->device = device;
    app->FullscreenMode() = 0;
    app->state.playbackActive = 0;
    app->state.frameStepPlayback = 0;
    SetWindowLongPtrW(app->MainWindow(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWindow));
    device->Release();
    ShutdownCleanup(app);
    g_Block = nullptr;
    delete app;
    MH_Uninitialize();
    return failures ? 1 : 0;
}
