// MAINTENANCE: diagnostic; real native-MME reassignment and reload capture.
// Run only in a staged runtime. MMS_MME_REAL_PICKER=1 normally requires a private
// desktop. MMS_MME_HIDDEN_DEFAULT=1 permits injected selection on Default;
// adding MMS_MME_OFFSCREEN_REAL_PICKER=1 explicitly permits its real dialog,
// hidden and placed outside the virtual screen. This is not desktop isolation.
// All modes run MME's real editor, assignment, compilation and draws.
// Optional probes: MMS_MME_SECOND_MODEL=<path>, MMS_MME_MODEL_EDIT=1,
// MMS_MME_SWAPCHAIN_BACKBUFFER=1, MMS_MME_REASSIGN_REPEATS=1..20.
// MMS_MME_PICKER_FRAMES=0..8 renders while the injected picker is still open.
// MMS_MME_RESET_PROBE=before|picker|after|drawn resets once during the first assignment;
// it requires injected selection and MMS_MME_SWAPCHAIN_BACKBUFFER=1.
// MMS_MME_REENTRANT_PAINT=1 sends one real WM_PAINT after a successful outer
// BeginScene in manual-first. This is a synthetic trigger, not a file-dialog
// reproduction; failing scene calls are logged without repairing their state.
// MMS_MME_APNG_TRACE=1 observes the verified native MME APNG class in this
// test process only; it never calls extra rendering/resource methods.
// Separate fault injection: MMS_MME_APNG_ZERO_NEW_TEXTURE=1 requires APNG_TRACE
// and RESET_PROBE=drawn, rejects real-picker/reentrant modes, and zeroes at most
// two newly recreated dynamic textures. It does not invalidate cached frames.
// MMS_MME_APNG_INVALIDATE_AFTER_ZERO=1 additionally requires the zero injection;
// its recovery control writes only the verified tree sentinel into the cache.
// MMS_MME_APNG_CONTRACT_TEST=1 requires APNG_TRACE and the installed production
// fix; isolated fake objects exercise its real wrapper without calling D3D.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/effect_apng.hpp"
#include <MinHook.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dlgs.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" char* ExpGetPmdBoneName(int, int);
extern "C" float* ExpGetPmdBoneWorldMat(float*, int, int);
extern "C" int ExpGetCurrentObject();
extern "C" int ExpGetPmdNum();
extern "C" int ExpGetAcsNum();

using namespace mikumikustudio;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT kWidth = 640, kHeight = 480;
const char* phase = "startup";
fs::path modelPath, secondModelPath, effectPath, outputPath;
std::string effectAnsi;
unsigned char* primaryModel = nullptr;
std::uint8_t primarySlot = 0;
int controlBone = -1;
unsigned pickerCalls = 0, compileCalls = 0;
bool assigning = false;
bool realPicker = false;
bool offscreenRealPicker = false;
bool modelEdit = false, swapchainBackbuffer = false;
bool defaultControlPose = false;
bool apngTrace = false;
bool apngZeroNewTexture = false;
bool apngInvalidateAfterZero = false;
bool apngContractTest = false;
unsigned reassignRepeats = 1;
unsigned pickerFrames = 0;
bool insidePickerFrames = false;
enum class ResetProbeTiming { Off, Before, Picker, After, Drawn };
ResetProbeTiming resetProbeTiming = ResetProbeTiming::Off;
bool resetProbeRan = false;
bool insideDrawnReset = false;
unsigned assignmentCalls = 0;
bool reentrantPaint = false, reentrantPaintRan = false, reentrantPaintSending = false;
unsigned nestedPaintFrames = 0, nestedPaintBegins = 0, nestedPaintEnds = 0;
unsigned maximumFrameDepth = 0;
HWND editor = nullptr;
bool editorDone = false, insideEditorTimer = false;
unsigned editorTicks = 0, editorStep = 0;
std::function<void()> captureWhileEditorOpen;
void Frames(MMDApp* app, unsigned count);
void AuditWindows(const char* event);
const char* PickerMode() {
    return offscreenRealPicker ? "offscreen-default" : realPicker ? "real-private" : "injected";
}
const char* ResetProbeName(ResetProbeTiming timing) {
    switch (timing) {
    case ResetProbeTiming::Before: return "before";
    case ResetProbeTiming::Picker: return "picker";
    case ResetProbeTiming::After: return "after";
    case ResetProbeTiming::Drawn: return "drawn";
    default: return "off";
    }
}

void Require(bool ok, const char* text) {
    if (!ok) {
        std::fprintf(stderr, "FAIL phase=%s %s\n", phase, text);
        std::fflush(nullptr);
        ExitProcess(7);
    }
}
std::wstring Setting(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), needed);
    Require(length > 0 && length < needed, "read diagnostic environment option");
    value.resize(length);
    return value;
}
bool Enabled(const wchar_t* name) {
    const auto value = Setting(name);
    Require(value.empty() || value == L"0" || value == L"1", "boolean diagnostic option must be 0 or 1");
    return value == L"1";
}
std::string Utf8(const wchar_t* text) {
    std::string result;
    text_encoding::EncodeExact(text ? text : L"", CP_UTF8, result);
    return result;
}
std::string Hex(const char* text) {
    std::string result;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); p && *p; ++p) {
        char byte[4]{}; std::snprintf(byte, sizeof(byte), "%02X", *p);
        if (!result.empty()) result += ' ';
        result += byte;
    }
    return result;
}
std::string CurrentDirectory() {
    wchar_t path[32768]{};
    Require(GetCurrentDirectoryW(static_cast<DWORD>(std::size(path)), path) > 0,
            "read current directory");
    return Utf8(path);
}
void Environment(const char* event) {
    const char* locale = std::setlocale(LC_ALL, nullptr);
    std::printf("ENV phase=%s event=%s cwd=%s acp=%u crt_locale=%s thread=%lu\n",
        phase, event, CurrentDirectory().c_str(), GetACP(), locale ? locale : "(null)",
        GetCurrentThreadId());
}

void LogCreatedAdapter(MMDApp* app) {
    auto* renderer = app->Renderer();
    Require(renderer && renderer->device, "read actual diagnostic device adapter");
    D3DDEVICE_CREATION_PARAMETERS creation{};
    Require(SUCCEEDED(renderer->device->GetCreationParameters(&creation)),
            "read actual device creation parameters");
    ComPtr<IDirect3D9> direct3D;
    Require(SUCCEEDED(renderer->device->GetDirect3D(direct3D.GetAddressOf())) && direct3D,
            "get device Direct3D interface for adapter identity");
    D3DADAPTER_IDENTIFIER9 identity{};
    Require(SUCCEEDED(direct3D->GetAdapterIdentifier(creation.AdapterOrdinal, 0, &identity)),
            "read actual adapter identity");
    identity.Description[std::size(identity.Description) - 1] = '\0';
    identity.Driver[std::size(identity.Driver) - 1] = '\0';
    const HMODULE executable = GetModuleHandleW(nullptr);
    std::printf("DEVICE_ADAPTER adapter_ordinal=%u device_type=%u behavior_flags=0x%08lx description=%s vendor_id=0x%04lx device_id=0x%04lx driver=%s\n",
        creation.AdapterOrdinal, static_cast<unsigned>(creation.DeviceType),
        static_cast<unsigned long>(creation.BehaviorFlags), identity.Description,
        static_cast<unsigned long>(identity.VendorId), static_cast<unsigned long>(identity.DeviceId), identity.Driver);
    // Production WinMain exports both GPU hints and calls processor::Initialize;
    // this diagnostic wmain does neither. Record the difference, do not fix it
    // while measuring which adapter the unmodified diagnostic configuration uses.
    std::printf("ENTRYPOINT_DIFFERENCE entrypoint=wmain processor_initialize_called=0 nvidia_hint_export=%d amd_hint_export=%d gpu_selection_modified=0\n",
        GetProcAddress(executable, "NvOptimusEnablement") ? 1 : 0,
        GetProcAddress(executable, "AmdPowerXpressRequestHighPerformance") ? 1 : 0);
} // The temporary Direct3D COM reference is released before the first frame.

void SceneState(MMDApp* app, const char* event, unsigned frame = 0) {
    auto* renderer = app->Renderer();
    void* savedObject = app->state.activeRenderObject;
    struct RestoreObject {
        MMDApp* app; void* object;
        ~RestoreObject() { app->state.activeRenderObject = object; }
    } restore{app, savedObject};
    std::printf("SCENE phase=%s event=%s frame=%u selected_slot=%u model_count=%d optflag0=%u edit_mode=%d use_effect_renderer=%d repeat=%d active_object=%p active_current=%d active_pass=%d shared_effect=%p nondisplay=%u selfshadow=%u enabled=%u\n",
        phase, event, frame, unsigned(app->SelectedModelSlot()), ExpGetPmdNum(),
        unsigned(app->state.optflag[0]), int(app->EditMode()), UseEffectModelRenderer(app),
        app->state.renderPassCount, savedObject, ExpGetCurrentObject(), app->state.activeRenderPass,
        renderer ? renderer->effect : nullptr, unsigned(app->ModelNonDisplayMode()),
        unsigned(app->SelfShadowMode()), unsigned(app->state.selfShadowEnabled));
    // ExpGetPmdOrder also registers an object with the MME draw-hook guard.
    // Never call it as an observer: doing so could conceal this failure.
    const int base = (std::min)(ExpGetAcsNum(), app->state.accessoryRenderSplitOrder);
    int ordinal = 0;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* bytes = app->ModelSlot(slot);
        if (!bytes) continue;
        const auto& model = *mdl::Mdl(bytes);
        app->state.activeRenderObject = bytes;
        const int current = ExpGetCurrentObject();
        std::printf("MODEL_STATE phase=%s event=%s frame=%u ordinal=%d slot=%d object=%p path=%s load_complete=%u combo=%d expected_order=%d current_object=%d display=%u toon=%u vertices=%u materials=%u vb=%p ib=%p\n",
            phase, event, frame, ordinal++, slot, static_cast<void*>(bytes), Utf8(model.path).c_str(),
            unsigned(model.loadComplete), int(model.comboSelIndex), int(model.comboSelIndex) + base,
            current, unsigned(model.displayState), unsigned(model.toonFlag), model.vertexCount,
            model.materialCount, model.vertexBuffer, model.indexBuffer);
    }
    if (renderer && renderer->device) {
        ComPtr<IDirect3DSurface9> target, depth, backbuffer;
        const HRESULT rtHr = renderer->device->GetRenderTarget(0, &target);
        const HRESULT dsHr = renderer->device->GetDepthStencilSurface(&depth);
        const HRESULT bbHr = renderer->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        D3DVIEWPORT9 view{}; const HRESULT viewHr = renderer->device->GetViewport(&view);
        DWORD colorWrite = 0, zEnable = 0, zWrite = 0, scissor = 0;
        renderer->device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
        renderer->device->GetRenderState(D3DRS_ZENABLE, &zEnable);
        renderer->device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        renderer->device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        std::printf("DEVICE_STATE phase=%s event=%s frame=%u rt=%p rt_hr=0x%08lx depth=%p depth_hr=0x%08lx swapchain=%p swapchain_hr=0x%08lx cached_rt=%p cached_depth=%p viewport=(%lu,%lu,%lu,%lu) viewport_hr=0x%08lx colorwrite=0x%lx zenable=%lu zwrite=%lu scissor=%lu\n",
            phase, event, frame, static_cast<void*>(target.Get()), static_cast<unsigned long>(rtHr),
            static_cast<void*>(depth.Get()), static_cast<unsigned long>(dsHr), static_cast<void*>(backbuffer.Get()),
            static_cast<unsigned long>(bbHr), static_cast<void*>(renderer->backbufferSurface),
            static_cast<void*>(renderer->depthStencilSurface), view.X, view.Y, view.Width, view.Height,
            static_cast<unsigned long>(viewHr), colorWrite, zEnable, zWrite, scissor);
    }
}

void ResetProbeAt(MMDApp* app, ResetProbeTiming timing) {
    if (resetProbeTiming != timing || resetProbeRan) return;
    Require(!realPicker && swapchainBackbuffer, "reset probe requires injected picker and swapchain capture");
    Require(assignmentCalls == (timing == ResetProbeTiming::Before ? 0u : 1u),
            "reset probe runs only around the first assignment");
    resetProbeRan = true; // Do not repeat if Reset dispatches nested messages.
    std::printf("RESET_PROBE phase=%s event=begin timing=%s assignment=1 assigning=%d editor_timer_active=%d\n",
        phase, ResetProbeName(timing), assigning ? 1 : 0, insideEditorTimer ? 1 : 0);
    // SceneState's temporary surface references have all been released before
    // calling the production reset. Swapchain capture retains CPU pixels only.
    SceneState(app, "reset-probe-before");
    bool reset = false;
    {
        insideDrawnReset = timing == ResetProbeTiming::Drawn;
        struct LeaveReset { ~LeaveReset() { insideDrawnReset = false; } } leaveReset;
        reset = PostDeviceReset(app);
    }
    std::printf("RESET_PROBE phase=%s event=end timing=%s result=%d\n",
        phase, ResetProbeName(timing), reset ? 1 : 0);
    SceneState(app, "reset-probe-after");
    Require(reset, "production PostDeviceReset succeeds for the requested reset probe");
}

decltype(&ShowWindow) originalShow = nullptr;
decltype(&SetWindowPos) originalSetWindowPos = nullptr;
decltype(&MoveWindow) originalMoveWindow = nullptr;
decltype(&SetWindowPlacement) originalSetWindowPlacement = nullptr;
decltype(&GetOpenFileNameA) originalOpenFileA = nullptr;
decltype(&GetOpenFileNameW) originalOpenFileW = nullptr;
bool OwnWindow(HWND window) {
    DWORD process = 0;
    return GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
}
bool OwnTopLevel(HWND window) {
    return OwnWindow(window) && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD);
}
RECT VirtualScreen() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const long long right = static_cast<long long>(x) + width, bottom = static_cast<long long>(y) + height;
    Require(width > 0 && height > 0 && right <= (std::numeric_limits<LONG>::max)() &&
            bottom <= (std::numeric_limits<LONG>::max)(), "read valid virtual-screen bounds");
    return {x, y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
}
RECT OutsideScreen(LONG width, LONG height) {
    Require(width >= 0 && height >= 0, "retain nonnegative window dimensions");
    const RECT screen = VirtualScreen();
    const long long left = static_cast<long long>(screen.left) - width - 64;
    const long long top = static_cast<long long>(screen.top) - height - 64;
    Require(left >= (std::numeric_limits<LONG>::min)() && top >= (std::numeric_limits<LONG>::min)(),
            "offscreen placement fits Windows coordinates");
    return {static_cast<LONG>(left), static_cast<LONG>(top), screen.left - 64, screen.top - 64};
}
BOOL WINAPI PlaceWindow(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if (offscreenRealPicker && OwnTopLevel(window)) {
        RECT current{};
        Require(GetWindowRect(window, &current) != FALSE, "read diagnostic window before SetWindowPos");
        const RECT outside = OutsideScreen(flags & SWP_NOSIZE ? current.right - current.left : width,
            flags & SWP_NOSIZE ? current.bottom - current.top : height);
        x = outside.left; y = outside.top;
        flags = (flags & ~(SWP_NOMOVE | SWP_SHOWWINDOW)) | SWP_NOACTIVATE | SWP_HIDEWINDOW;
    }
    return originalSetWindowPos(window, after, x, y, width, height, flags);
}
BOOL WINAPI MoveOffscreen(HWND window, int x, int y, int width, int height, BOOL repaint) {
    if (offscreenRealPicker && OwnTopLevel(window)) {
        const RECT outside = OutsideScreen(width, height);
        x = outside.left; y = outside.top;
    }
    return originalMoveWindow(window, x, y, width, height, repaint);
}
BOOL WINAPI RestoreOffscreen(HWND window, const WINDOWPLACEMENT* placement) {
    if (!offscreenRealPicker || !OwnTopLevel(window) || !placement)
        return originalSetWindowPlacement(window, placement);
    auto hidden = *placement;
    hidden.showCmd = SW_HIDE;
    hidden.flags &= ~WPF_RESTORETOMAXIMIZED;
    const BOOL result = originalSetWindowPlacement(window, &hidden);
    if (!result) return result;
    // Windows may clamp a saved offscreen placement into the work area.
    // Restore the requested size first while hidden, then move it outside.
    RECT rect{};
    Require(GetWindowRect(window, &rect) != FALSE, "read restored diagnostic window bounds");
    const RECT outside = OutsideScreen(rect.right - rect.left, rect.bottom - rect.top);
    Require(originalSetWindowPos(window, nullptr, outside.left, outside.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW) != FALSE,
            "keep restored diagnostic window hidden and offscreen");
    return result;
}
void AuditWindows(const char* event) {
    if (!offscreenRealPicker) return;
    struct Audit { const char* event; RECT screen; HWND foreground; unsigned count = 0; } audit{
        event, VirtualScreen(), GetForegroundWindow()};
    const BOOL enumerated = EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!OwnTopLevel(window)) return TRUE;
        auto& state = *reinterpret_cast<Audit*>(parameter);
        RECT rect{}, overlap{};
        Require(GetWindowRect(window, &rect) != FALSE, "audit this process's top-level window bounds");
        bool intersects = IntersectRect(&overlap, &rect, &state.screen) != FALSE;
        const bool visible = IsWindowVisible(window) != FALSE;
        const bool foreground = window == state.foreground;
        wchar_t cls[128]{}; GetClassNameW(window, cls, static_cast<int>(std::size(cls)));
        // GDI+ starts this hidden 1x1 helper on its own thread, outside the
        // diagnostic thread's CBT hook. Relocate only that exact helper, then
        // apply the same strict placement check as every other window.
        if (intersects && !visible && !foreground &&
            std::wcscmp(cls, L"GDI+ Hook Window Class") == 0 &&
            rect.right - rect.left == 1 && rect.bottom - rect.top == 1) {
            const RECT outside = OutsideScreen(1, 1);
            Require(SetWindowPos(window, nullptr, outside.left, outside.top, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE &&
                    GetWindowRect(window, &rect) != FALSE,
                    "place hidden GDI+ helper outside the screen");
            intersects = IntersectRect(&overlap, &rect, &state.screen) != FALSE;
        }
        std::printf("WINDOW_AUDIT phase=%s event=%s mode=offscreen-default hwnd=%p class=%s rect=(%ld,%ld,%ld,%ld) visible=%d intersects=%d foreground=%d\n",
            phase, state.event, static_cast<void*>(window), Utf8(cls).c_str(), rect.left, rect.top,
            rect.right, rect.bottom, visible ? 1 : 0, intersects ? 1 : 0, foreground ? 1 : 0);
        ++state.count;
        if (intersects || foreground) {
            originalShow(window, SW_HIDE);
            Require(false, "offscreen-default window escaped placement or became foreground");
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&audit));
    Require(enumerated != FALSE, "enumerate this process's top-level windows for offscreen audit");
    std::printf("WINDOW_AUDIT_COMPLETE phase=%s event=%s mode=offscreen-default count=%u desktop_isolated=0\n",
        phase, event, audit.count);
}
struct PickerDrive {
    bool active = false, insideTimer = false, opened = false;
    UINT_PTR timer = 0;
    ULONGLONG deadline = 0;
    HWND owner = nullptr;
    std::vector<HWND> previousWindows;
} pickerDrive;
BOOL WINAPI Hide(HWND window, int command) {
    return originalShow(window, OwnTopLevel(window) ? SW_HIDE : command);
}
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    const HWND window = reinterpret_cast<HWND>(wp);
    if (code == HCBT_CREATEWND && OwnWindow(window)) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            if (offscreenRealPicker) {
                const RECT screen = VirtualScreen();
                // Keep cx/cy intact, including CW_USEDEFAULT. The screen extent
                // provides room until Windows resolves a default dimension.
                const RECT outside = OutsideScreen(create->cx > 0 ? create->cx : screen.right - screen.left,
                    create->cy > 0 ? create->cy : screen.bottom - screen.top);
                create->x = outside.left; create->y = outside.top;
            } else create->x = create->y = -32000;
        }
    }
    if (OwnTopLevel(window)) {
        if (code == HCBT_ACTIVATE) return 1;
        if (offscreenRealPicker && code == HCBT_MINMAX && LOWORD(lp) == SW_SHOWMAXIMIZED) return 1;
        if (offscreenRealPicker && code == HCBT_MOVESIZE) {
            auto* rect = reinterpret_cast<RECT*>(lp);
            const long long width = static_cast<long long>(rect->right) - rect->left;
            const long long height = static_cast<long long>(rect->bottom) - rect->top;
            Require(width >= 0 && height >= 0 && width <= (std::numeric_limits<LONG>::max)() &&
                    height <= (std::numeric_limits<LONG>::max)(), "validate CBT move/size dimensions");
            // HCBT_MOVESIZE permits replacing these final coordinates. Keep
            // the requested size while changing only its screen position.
            *rect = OutsideScreen(static_cast<LONG>(width), static_cast<LONG>(height));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void GuardSelfTest() {
    if (!offscreenRealPicker) return;
    const char* previousPhase = phase;
    phase = "guard-self-test";
    std::printf("GUARD_SELF_TEST event=begin mode=offscreen-default mme_reproduction=0\n");
    const RECT screen = VirtualScreen();
    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC", L"MME offscreen guard self-test", WS_POPUP,
        screen.left, screen.top, 64, 48, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "create hidden no-activate guard self-test window");
    const auto check = [&](const char* operation, BOOL result, LONG width, LONG height) {
        RECT rect{};
        const BOOL gotRect = GetWindowRect(window, &rect);
        std::printf("GUARD_SELF_TEST event=operation name=%s result=%d hwnd=%p requested=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld) rect_ok=%d visible=%d mme_reproduction=0\n",
            operation, result ? 1 : 0, static_cast<void*>(window), screen.left, screen.top, width, height,
            rect.left, rect.top, rect.right, rect.bottom, gotRect ? 1 : 0, IsWindowVisible(window) ? 1 : 0);
        Require(result != FALSE && gotRect != FALSE, "guard self-test window operation succeeded");
        Require(!IsWindowVisible(window), "guard self-test window remains hidden");
        Require((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
                "guard self-test window retains WS_EX_NOACTIVATE");
        AuditWindows(operation);
        Require(rect.right - rect.left == width && rect.bottom - rect.top == height,
                "guard self-test preserves requested window dimensions");
        std::printf("GUARD_SELF_TEST event=passed name=%s mme_reproduction=0\n", operation);
    };
    check("create-hidden", TRUE, 64, 48);
    check("set-window-pos-noactivate", SetWindowPos(window, nullptr,
        screen.left, screen.top, 80, 56, SWP_NOACTIVATE | SWP_NOZORDER), 80, 56);
    check("move-window-no-repaint", MoveWindow(window, screen.left, screen.top, 96, 64, FALSE), 96, 64);
    check("set-window-pos-nosendchanging", SetWindowPos(window, nullptr,
        screen.left, screen.top, 112, 72, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING), 112, 72);
    const BOOL destroyed = DestroyWindow(window);
    std::printf("GUARD_SELF_TEST event=destroy result=%d mme_reproduction=0\n", destroyed ? 1 : 0);
    Require(destroyed != FALSE && !IsWindow(window), "destroy guard self-test window");
    AuditWindows("guard-self-test-destroyed");
    std::printf("GUARD_SELF_TEST event=complete result=pass mode=offscreen-default desktop_isolated=0 mme_reproduction=0\n");
    phase = previousPhase;
}
void PlaceExistingHiddenWindows() {
    if (!offscreenRealPicker) return;
    // Libraries may create helper windows before wmain installs the CBT hook.
    // Move these hidden windows outside the screen before the strict audit;
    // do not exempt them from visibility, foreground or placement checks.
    Require(EnumWindows([](HWND window, LPARAM) -> BOOL {
        if (!OwnTopLevel(window)) return TRUE;
        Require(!IsWindowVisible(window) && window != GetForegroundWindow(),
                "preexisting diagnostic helper window is hidden and inactive");
        RECT rect{};
        Require(GetWindowRect(window, &rect) != FALSE,
                "read preexisting diagnostic helper window bounds");
        const RECT outside = OutsideScreen(rect.right - rect.left, rect.bottom - rect.top);
        Require(SetWindowPos(window, nullptr, outside.left, outside.top, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                "place preexisting hidden diagnostic helper outside the screen");
        return TRUE;
    }, 0) != FALSE, "place this process's preexisting hidden helper windows");
    AuditWindows("preexisting-hidden-helpers-placed");
}
int WINAPI UnexpectedWide(HWND, LPCWSTR body, LPCWSTR title, UINT) {
    std::fprintf(stderr, "DIALOG phase=%s title=%s body=%s\n", phase,
        Utf8(title).c_str(), Utf8(body).c_str());
    Require(false, "unexpected message box"); return 0;
}
int WINAPI UnexpectedAnsi(HWND, LPCSTR body, LPCSTR title, UINT) {
    std::fprintf(stderr, "DIALOG_A phase=%s title_hex=%s body_hex=%s\n", phase,
        Hex(title).c_str(), Hex(body).c_str());
    Require(false, "unexpected ANSI message box"); return 0;
}
BOOL WINAPI UnexpectedWidePicker(LPOPENFILENAMEW dialog) {
    // Some system versions delegate the ANSI API to its public wide entry.
    // Preserve that real implementation only inside our observed ANSI call.
    if (pickerDrive.active) {
        Require(originalOpenFileW != nullptr, "real ANSI picker wide delegate exists");
        std::printf("PICKER_INTERNAL_W phase=%s flags=0x%08lx\n", phase, dialog ? dialog->Flags : 0ul);
        return originalOpenFileW(dialog);
    }
    Require(false, "unexpected wide file picker (expected native MME ANSI picker)");
    return FALSE;
}
void LogPickerFolder(HWND dialog, const char* event) {
    std::wstring folder;
    LRESULT result = 0;
    if (IsWindowUnicode(dialog)) {
        wchar_t text[32768]{};
        result = SendMessageW(dialog, CDM_GETFOLDERPATH, std::size(text), reinterpret_cast<LPARAM>(text));
        text[std::size(text) - 1] = L'\0';
        if (result > 0 && result <= static_cast<LRESULT>(std::size(text))) folder = text;
    } else {
        char text[32768]{};
        result = SendMessageA(dialog, CDM_GETFOLDERPATH, std::size(text), reinterpret_cast<LPARAM>(text));
        text[std::size(text) - 1] = '\0';
        if (result > 0 && result <= static_cast<LRESULT>(std::size(text)))
            text_encoding::Decode(text, CP_ACP, folder);
    }
    std::printf("PICKER_FOLDER phase=%s event=%s mode=%s hwnd=%p query_result=%lld folder=%s cwd=%s\n",
        phase, event, PickerMode(), static_cast<void*>(dialog), static_cast<long long>(result),
        folder.empty() ? "(unavailable)" : Utf8(folder.c_str()).c_str(), CurrentDirectory().c_str());
}
void CALLBACK DriveRealPicker(HWND, UINT, UINT_PTR timer, DWORD) {
    if (!pickerDrive.active || timer != pickerDrive.timer || pickerDrive.insideTimer) return;
    pickerDrive.insideTimer = true;
    struct Leave { ~Leave() { pickerDrive.insideTimer = false; } } leave;
    AuditWindows("real-picker-timer");
    if (GetTickCount64() >= pickerDrive.deadline) {
        std::fprintf(stderr, "PICKER_TIMEOUT phase=%s mode=%s open_posted=%d cwd=%s\n",
            phase, PickerMode(), pickerDrive.opened ? 1 : 0, CurrentDirectory().c_str());
        KillTimer(nullptr, timer);
        Require(false, "real ANSI file picker timed out");
    }
    HWND fileDialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        if (IsWindowVisible(window)) {
            wchar_t visibleClass[64]{}, visibleTitle[256]{};
            GetClassNameW(window, visibleClass, 64); GetWindowTextW(window, visibleTitle, 256);
            std::printf("VISIBLE_WINDOW class=%s title=%s handle=%p\n", Utf8(visibleClass).c_str(),
                        Utf8(visibleTitle).c_str(), static_cast<void*>(window));
            originalShow(window, SW_HIDE);
            Require(!IsWindowVisible(window), "hide common dialog shown by the system without ShowWindow");
        }
        if (std::find(pickerDrive.previousWindows.begin(), pickerDrive.previousWindows.end(), window) !=
            pickerDrive.previousWindows.end()) return TRUE;
        wchar_t cls[32]{}; GetClassNameW(window, cls, static_cast<int>(std::size(cls)));
        if (std::wcscmp(cls, L"#32770") || !GetDlgItem(window, IDOK) ||
            (!GetDlgItem(window, edt1) && !GetDlgItem(window, cmb13))) return TRUE;
        HWND owner = GetWindow(window, GW_OWNER);
        for (unsigned depth = 0; owner && owner != pickerDrive.owner && depth < 16; ++depth)
            owner = GetWindow(owner, GW_OWNER);
        if (pickerDrive.owner && owner != pickerDrive.owner) return TRUE;
        auto* found = reinterpret_cast<HWND*>(data);
        Require(*found == nullptr, "one owned common-file dialog is present");
        *found = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fileDialog));
    if (!fileDialog || pickerDrive.opened) return;
    const HWND open = GetDlgItem(fileDialog, IDOK);
    if (!IsWindowEnabled(open)) return;
    HWND filename = GetDlgItem(fileDialog, edt1);
    if (!filename) filename = GetDlgItem(fileDialog, cmb13);
    LogPickerFolder(fileDialog, "initialized-before-selection");
    wchar_t initialFile[32768]{};
    GetWindowTextW(filename, initialFile, static_cast<int>(std::size(initialFile)));
    std::printf("PICKER_INITIAL_FILE phase=%s mode=%s value=%s\n", phase, PickerMode(), Utf8(initialFile).c_str());
    Require(SetWindowTextW(filename, effectPath.c_str()) != FALSE,
            "enter requested FX in real file picker");
    pickerDrive.opened = true;
    std::printf("PICKER_OPEN phase=%s mode=%s dialog=%p filename_control=%d path=%s cwd=%s\n",
        phase, PickerMode(), static_cast<void*>(fileDialog), GetDlgCtrlID(filename),
        Utf8(effectPath.c_str()).c_str(), CurrentDirectory().c_str());
    Require(PostMessageW(fileDialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                         reinterpret_cast<LPARAM>(open)) != FALSE,
            "press Open in real file picker");
}
BOOL WINAPI SelectEffect(LPOPENFILENAMEA dialog) {
    Require(assigning && dialog && dialog->lpstrFile, "file picker belongs to reassignment");
    Require(!insidePickerFrames, "nested picker frames must not recursively open a file picker");
    ++pickerCalls;
    Environment("picker-enter");
    std::printf("PICKER phase=%s call=%u mode=%s flags=0x%08lx max_file=%lu initial_dir_hex=%s old_file_hex=%s\n",
        phase, pickerCalls, PickerMode(), dialog->Flags, dialog->nMaxFile,
        Hex(dialog->lpstrInitialDir).c_str(), Hex(dialog->lpstrFile).c_str());
    Require(effectAnsi.size() + 1 < dialog->nMaxFile, "selected FX fits ANSI file buffer");
    if (realPicker) {
        Require(originalOpenFileA && !pickerDrive.active, "real ANSI picker trampoline is ready without reentry");
        pickerDrive = {};
        pickerDrive.owner = dialog->hwndOwner;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
            pickerDrive.previousWindows.push_back(window); return TRUE;
        }, 0);
        pickerDrive.deadline = GetTickCount64() + 12000;
        pickerDrive.active = true;
        pickerDrive.timer = SetTimer(nullptr, 0, 100, DriveRealPicker);
        Require(pickerDrive.timer != 0, "schedule real file picker interaction");
        // Preserve the original lpstrFile, flags, owner and initial directory.
        // The timer enters the new name only after the system dialog exists.
        AuditWindows("before-real-picker");
        const BOOL result = originalOpenFileA(dialog);
        const DWORD error = result ? 0 : CommDlgExtendedError();
        KillTimer(nullptr, pickerDrive.timer);
        pickerDrive.timer = 0; pickerDrive.active = false;
        Require(std::memchr(dialog->lpstrFile, '\0', dialog->nMaxFile) != nullptr,
                "real picker returned a terminated file path");
        std::wstring selected;
        const bool decoded = text_encoding::Decode(dialog->lpstrFile, CP_ACP, selected);
        std::printf("PICKER_RETURN phase=%s mode=%s result=%d error=0x%08lx flags=0x%08lx path=%s path_hex=%s offset=%u extension=%u simulated_cwd_change=0 cwd=%s\n",
            phase, PickerMode(), result, error, dialog->Flags, decoded ? Utf8(selected.c_str()).c_str() : "(decode failed)",
            Hex(dialog->lpstrFile).c_str(), dialog->nFileOffset, dialog->nFileExtension, CurrentDirectory().c_str());
        AuditWindows("after-real-picker");
        Environment("picker-return");
        Require(result != FALSE && pickerDrive.opened, "real ANSI file picker accepted Open");
        Require(decoded, "decode real picker result with its ANSI code page");
        std::error_code pathError;
        Require(fs::equivalent(fs::path(selected), effectPath, pathError) && !pathError,
                "real picker returned the requested FX file");
        return result;
    }
    std::memcpy(dialog->lpstrFile, effectAnsi.c_str(), effectAnsi.size() + 1);
    dialog->lpstrFile[effectAnsi.size() + 1] = '\0';
    const auto slash = effectAnsi.find_last_of("\\/");
    const auto dot = effectAnsi.find_last_of('.');
    dialog->nFileOffset = static_cast<WORD>(slash == std::string::npos ? 0 : slash + 1);
    dialog->nFileExtension = static_cast<WORD>(dot == std::string::npos ? 0 : dot + 1);
    if (dialog->lpstrFileTitle && dialog->nMaxFileTitle) {
        const auto title = effectAnsi.substr(dialog->nFileOffset);
        Require(title.size() < dialog->nMaxFileTitle, "FX title fits picker output");
        std::memcpy(dialog->lpstrFileTitle, title.c_str(), title.size() + 1);
    }
    // This simulates the selection's directory side effect; the mode label
    // distinguishes it from directory handling observed through the real API.
    const bool changeDirectory = !(dialog->Flags & OFN_NOCHANGEDIR);
    if (changeDirectory)
        Require(SetCurrentDirectoryW(effectPath.parent_path().c_str()) != FALSE,
                "simulate selected directory");
    ResetProbeAt(g_Block, ResetProbeTiming::Picker);
    if (pickerFrames) {
        // MME has not received the selected file yet. Exercise the draws that
        // a real common dialog's nested message loop can run in this state.
        // DriveEditor returns immediately while its original timer is active.
        Require(insideEditorTimer && assigning, "nested picker frames remain inside the original assignment transaction");
        insidePickerFrames = true;
        struct LeavePickerFrames { ~LeavePickerFrames() { insidePickerFrames = false; } } leave;
        std::printf("PICKER_NESTED_FRAMES phase=%s event=begin count=%u assigning=1 editor_timer_active=1 cwd=%s\n",
            phase, pickerFrames, CurrentDirectory().c_str());
        Frames(g_Block, pickerFrames);
        std::printf("PICKER_NESTED_FRAMES phase=%s event=end count=%u assigning=%d editor_timer_active=%d\n",
            phase, pickerFrames, assigning ? 1 : 0, insideEditorTimer ? 1 : 0);
    }
    std::printf("PICKER_RETURN phase=%s mode=injected path=%s offset=%u extension=%u simulated_cwd_change=%d\n",
        phase, Utf8(effectPath.c_str()).c_str(), dialog->nFileOffset,
        dialog->nFileExtension, changeDirectory ? 1 : 0);
    Environment("picker-return");
    return TRUE;
}

// ABI-only declarations match D3DX's public file-compile signature, without
// adding a dependency on legacy D3DX SDK headers to this diagnostic.
struct ShaderMacro { const char* name; const char* definition; };
using CreateEffectFile = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR,
    const ShaderMacro*, void*, DWORD, void*, void**, void**);
CreateEffectFile originalCompile = nullptr;
HRESULT WINAPI ObserveCompile(IDirect3DDevice9* device, LPCWSTR path,
    const ShaderMacro* macros, void* include, DWORD flags, void* pool,
    void** effect, void** errors) {
    const unsigned call = ++compileCalls;
    std::printf("COMPILE phase=%s call=%u path=%s flags=0x%08lx pool=%p cwd=%s\n",
        phase, call, Utf8(path).c_str(), flags, pool, CurrentDirectory().c_str());
    if (macros) for (unsigned i = 0; i < 256 && macros[i].name; ++i)
        std::printf("MACRO call=%u name=%s value=%s\n", call, macros[i].name,
                    macros[i].definition ? macros[i].definition : "(null)");
    const HRESULT hr = originalCompile(device, path, macros, include, flags, pool, effect, errors);
    std::printf("COMPILE_RETURN phase=%s call=%u hr=0x%08lx effect=%p\n",
        phase, call, static_cast<unsigned long>(hr), effect ? *effect : nullptr);
    return hr;
}
void InstallCompileObserver() {
    auto& api = d3dx::Get();
    Require(api.Load(), "load production D3DX runtime");
    auto* entry = reinterpret_cast<void*>(GetProcAddress(api.module, "D3DXCreateEffectFromFileW"));
    Require(entry != nullptr, "find D3DXCreateEffectFromFileW");
    Require(MH_CreateHook(entry, &ObserveCompile, reinterpret_cast<void**>(&originalCompile)) == MH_OK &&
            MH_EnableHook(entry) == MH_OK, "observe real effect file compilation");
}

// Verified MMEffect 0.37 x64 ABI: RCX=this, XMM1=double for Update.
// Update(8230) and OnLost(8290) have no defined return value. OnReset(82C0)
// returns the HRESULT from D3DXCreateTexture in EAX without changing it.
using ApngUpdate = void(__fastcall*)(void*, double);
using ApngLost = void(__fastcall*)(void*);
using ApngReset = HRESULT(__fastcall*)(void*);
ApngUpdate originalApngUpdate = nullptr;
ApngLost originalApngLost = nullptr;
ApngReset originalApngReset = nullptr;
DWORD apngTraceThread = 0;
const void* apngExpectedVtable = nullptr;
unsigned apngCalls[3]{}, apngRecords = 0, apngUnreadable = 0;
// Published only while a contract call is in flight. The worker is joined before
// clearing the identity or destroying its fixture; normal MME objects never match.
std::atomic<void*> apngContractObject{nullptr};
std::atomic<HRESULT> apngContractResult{S_OK};
std::atomic<DWORD> apngContractLastError{ERROR_SUCCESS};
std::atomic<unsigned> apngContractOriginalCalls{0};
void* apngZeroObjects[2]{};
unsigned apngZeroAttempts = 0, apngZeroCompleted = 0;
unsigned apngInvalidateAttempts = 0, apngInvalidateCompleted = 0;
constexpr unsigned kApngRecordLimit = 4096;
struct ApngFields {
    const void* node = nullptr;
    const void* texture = nullptr;
    double time = 0;
    unsigned frameCount = 0;
    unsigned char preloaded = 0;
    bool valid = false;
};
// Keep SEH in a leaf with no C++ objects requiring unwinding. A failed read
// makes the observation invalid; it never changes or suppresses the target call.
ApngFields ReadApngFields(const void* object) {
    ApngFields value;
    __try {
        const auto* bytes = static_cast<const unsigned char*>(object);
        const void* vtable = nullptr;
        std::memcpy(&vtable, bytes, sizeof(vtable));
        if (vtable == apngExpectedVtable) {
            std::memcpy(&value.node, bytes + 0x80, sizeof(value.node));
            std::memcpy(&value.texture, bytes + 0x100, sizeof(value.texture));
            std::memcpy(&value.time, bytes + 0x58, sizeof(value.time));
            std::memcpy(&value.frameCount, bytes + 0x48, sizeof(value.frameCount));
            value.preloaded = bytes[0x108];
            value.valid = true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        value.valid = false;
    }
    return value;
}
void LogApng(const char* kind, unsigned call, void* object, const ApngFields& before,
             const ApngFields& after, bool hasTime, double time, bool hasResult, HRESULT result) {
    if (!before.valid || !after.valid) ++apngUnreadable;
    if (apngRecords >= kApngRecordLimit) return;
    ++apngRecords;
    std::printf("APNG_TRACE phase=%s kind=%s call=%u object=%p thread=%lu time_valid=%d time=%.17g hr_valid=%d hr=0x%08lx before_valid=%d after_valid=%d before_node=%p after_node=%p before_texture=%p after_texture=%p before_preloaded=%u after_preloaded=%u before_time=%.17g after_time=%.17g before_frames=%u after_frames=%u\n",
        phase, kind, call, object, apngTraceThread, hasTime ? 1 : 0, time,
        hasResult ? 1 : 0, static_cast<unsigned long>(result),
        before.valid ? 1 : 0, after.valid ? 1 : 0, before.node, after.node,
        before.texture, after.texture, unsigned(before.preloaded), unsigned(after.preloaded),
        before.time, after.time, before.frameCount, after.frameCount);
    if (apngRecords == kApngRecordLimit)
        std::printf("APNG_TRACE_LIMIT records=%u further_records_suppressed=1\n", apngRecords);
}
struct ApngInvalidation {
    std::uintptr_t beforeCache = 0, head = 0, afterCache = 0;
    unsigned sentinelFlag = 0;
    bool writeAttempted = false, written = false, complete = false;
    DWORD exceptionCode = 0;
    const char* status = "not_armed";
};
// This recovery control is a separate SEH leaf. The valid invalidation value is
// object+68's tree sentinel, never nullptr (SelectFrame's fast path tests only
// equality with that sentinel). Volatile reads prevent folding the two samples.
ApngInvalidation InvalidateApngAfterZero(void* object, const ApngFields& expected) {
    ApngInvalidation result;
    if (!apngInvalidateAfterZero || !apngZeroNewTexture || !insideDrawnReset ||
        GetCurrentThreadId() != apngTraceThread || !expected.valid) return result;
    __try {
        auto* bytes = static_cast<unsigned char*>(object);
        const auto vtable = *reinterpret_cast<const volatile std::uintptr_t*>(bytes);
        result.beforeCache = *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x80);
        result.head = *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x68);
        const auto texture = *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x100);
        const auto timeBits = *reinterpret_cast<const volatile std::uint64_t*>(bytes + 0x58);
        const auto frameCount = *reinterpret_cast<const volatile unsigned*>(bytes + 0x48);
        const auto preloaded = *reinterpret_cast<const volatile unsigned char*>(bytes + 0x108);
        std::uint64_t expectedTime = 0;
        std::memcpy(&expectedTime, &expected.time, sizeof(expectedTime));
        result.status = "unexpected_object_state";
        if (vtable != reinterpret_cast<std::uintptr_t>(apngExpectedVtable) ||
            result.beforeCache != reinterpret_cast<std::uintptr_t>(expected.node) ||
            texture != reinterpret_cast<std::uintptr_t>(expected.texture) || !texture ||
            preloaded != 0 || frameCount != expected.frameCount || timeBits != expectedTime) return result;
        result.status = "invalid_tree_sentinel";
        if (result.head < 0x10000 || result.head >= 0x800000000000ull || result.head % 8) return result;
        const auto* head = reinterpret_cast<const volatile unsigned char*>(result.head);
        result.sentinelFlag = head[0x29];
        if (result.sentinelFlag != 1) return result;
        result.status = "state_changed_before_write";
        if (*reinterpret_cast<const volatile std::uintptr_t*>(bytes) != vtable ||
            *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x68) != result.head ||
            *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x80) != result.beforeCache ||
            *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x100) != texture ||
            *reinterpret_cast<const volatile std::uint64_t*>(bytes + 0x58) != timeBits ||
            *reinterpret_cast<const volatile unsigned*>(bytes + 0x48) != frameCount ||
            *reinterpret_cast<const volatile unsigned char*>(bytes + 0x108) != preloaded ||
            head[0x29] != result.sentinelFlag) return result;
        result.writeAttempted = true;
        *reinterpret_cast<volatile std::uintptr_t*>(bytes + 0x80) = result.head;
        result.written = true;
        result.afterCache = *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x80);
        result.complete = result.afterCache == result.head &&
            *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x68) == result.head;
        result.status = result.complete ? "written_verified" : "readback_changed";
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result.exceptionCode = GetExceptionCode();
        result.status = "memory_exception";
    }
    return result;
}
void ZeroNewApngTexture(void* object, const ApngFields& before, const ApngFields& after,
                        HRESULT resetResult) {
    if (!apngZeroNewTexture || !apngTrace || GetCurrentThreadId() != apngTraceThread ||
        !resetProbeRan || !insideDrawnReset || resetProbeTiming != ResetProbeTiming::Drawn ||
        realPicker || reentrantPaint || assigning || assignmentCalls != 1 ||
        std::strcmp(phase, "manual-first") != 0 || resetResult != S_OK ||
        !before.valid || !after.valid || before.preloaded || after.preloaded ||
        before.texture || !after.texture || !after.frameCount) return;
    for (unsigned i = 0; i < apngZeroAttempts; ++i)
        if (apngZeroObjects[i] == object) return;
    if (apngZeroAttempts == std::size(apngZeroObjects)) return;
    // Count attempts before any resource method, so failure cannot cause retries.
    apngZeroObjects[apngZeroAttempts++] = object;
    auto* texture = static_cast<IDirect3DTexture9*>(const_cast<void*>(after.texture));
    D3DSURFACE_DESC description{};
    D3DLOCKED_RECT locked{};
    const UINT mipLevels = texture->GetLevelCount();
    const HRESULT descResult = texture->GetLevelDesc(0, &description);
    HRESULT lockResult = E_PENDING, unlockResult = E_PENDING;
    constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;
    std::size_t filled = 0;
    bool layoutValid = false;
    const bool supported = descResult == S_OK && mipLevels == 1 &&
        description.Format == D3DFMT_A8R8G8B8 && description.Pool == D3DPOOL_DEFAULT &&
        (description.Usage & D3DUSAGE_DYNAMIC) && description.Width && description.Height &&
        description.Width <= kMaximumBytes / 4 &&
        description.Height <= kMaximumBytes / (std::size_t(description.Width) * 4);
    if (supported) {
        lockResult = texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
        if (lockResult == S_OK) {
            const std::size_t rowBytes = std::size_t(description.Width) * 4;
            layoutValid = locked.pBits && locked.Pitch > 0 &&
                std::size_t(locked.Pitch) >= rowBytes &&
                std::size_t(locked.Pitch) <= kMaximumBytes / description.Height;
            if (layoutValid) {
                auto* pixels = static_cast<unsigned char*>(locked.pBits);
                for (UINT row = 0; row < description.Height; ++row) {
                    std::memset(pixels + std::size_t(row) * locked.Pitch, 0, rowBytes);
                    filled += rowBytes;
                }
            }
            // Unlock every successful lock, including an unsupported pitch.
            unlockResult = texture->UnlockRect(0);
        }
    }
    const bool complete = filled != 0 && unlockResult == S_OK;
    if (complete) ++apngZeroCompleted;
    std::printf("APNG_ZERO_INJECTION phase=%s object=%p texture=%p attempt=%u reset_hr=0x%08lx desc_hr=0x%08lx lock_hr=0x%08lx unlock_hr=0x%08lx mip_levels=%u level=0 lock_discard=1 rgba=00000000 width=%u height=%u format=%u type=%u usage=0x%08lx pool=%u multisample=%u quality=%lu pitch=%d supported=%d layout_valid=%d filled_bytes=%zu complete=%d node_preserved=%p synthetic=1 live_root_cause_proven=0\n",
        phase, object, static_cast<void*>(texture), apngZeroAttempts,
        static_cast<unsigned long>(resetResult), static_cast<unsigned long>(descResult),
        static_cast<unsigned long>(lockResult), static_cast<unsigned long>(unlockResult),
        mipLevels, description.Width, description.Height, static_cast<unsigned>(description.Format),
        static_cast<unsigned>(description.Type), static_cast<unsigned long>(description.Usage),
        static_cast<unsigned>(description.Pool), static_cast<unsigned>(description.MultiSampleType),
        static_cast<unsigned long>(description.MultiSampleQuality), locked.Pitch,
        supported ? 1 : 0, layoutValid ? 1 : 0, filled, complete ? 1 : 0, after.node);
    if (complete && apngInvalidateAfterZero) {
        ++apngInvalidateAttempts;
        const auto invalidation = InvalidateApngAfterZero(object, after);
        if (invalidation.complete) ++apngInvalidateCompleted;
        std::printf("APNG_INVALIDATE_CONTROL phase=%s object=%p attempt=%u before_cache=%p head=%p sentinel_flag=%u after_cache=%p write_attempted=%d written=%d complete=%d status=%s exception=0x%08lx synthetic=1 live_root_cause_proven=0\n",
            phase, object, apngInvalidateAttempts,
            reinterpret_cast<void*>(invalidation.beforeCache), reinterpret_cast<void*>(invalidation.head),
            invalidation.sentinelFlag, reinterpret_cast<void*>(invalidation.afterCache),
            invalidation.writeAttempted ? 1 : 0, invalidation.written ? 1 : 0,
            invalidation.complete ? 1 : 0, invalidation.status,
            static_cast<unsigned long>(invalidation.exceptionCode));
    }
}
void __fastcall ObserveApngUpdate(void* object, double time) {
    const DWORD incomingError = GetLastError();
    if (GetCurrentThreadId() != apngTraceThread) {
        SetLastError(incomingError); originalApngUpdate(object, time); return;
    }
    const unsigned call = ++apngCalls[0];
    const auto before = ReadApngFields(object);
    SetLastError(incomingError);
    originalApngUpdate(object, time);
    const DWORD outgoingError = GetLastError();
    const auto after = ReadApngFields(object);
    LogApng("update", call, object, before, after, true, time, false, 0);
    SetLastError(outgoingError);
}
void __fastcall ObserveApngLost(void* object) {
    const DWORD incomingError = GetLastError();
    if (GetCurrentThreadId() != apngTraceThread) {
        SetLastError(incomingError); originalApngLost(object); return;
    }
    const unsigned call = ++apngCalls[1];
    const auto before = ReadApngFields(object);
    SetLastError(incomingError);
    originalApngLost(object);
    const DWORD outgoingError = GetLastError();
    const auto after = ReadApngFields(object);
    LogApng("lost", call, object, before, after, false, 0, false, 0);
    SetLastError(outgoingError);
}
HRESULT __fastcall ObserveApngReset(void* object) {
    if (object && object == apngContractObject.load(std::memory_order_acquire)) {
        apngContractOriginalCalls.fetch_add(1, std::memory_order_relaxed);
        const HRESULT result = apngContractResult.load(std::memory_order_relaxed);
        SetLastError(apngContractLastError.load(std::memory_order_relaxed));
        return result; // Only the isolated fake object bypasses the real D3D method.
    }
    const DWORD incomingError = GetLastError();
    if (GetCurrentThreadId() != apngTraceThread) {
        SetLastError(incomingError); return originalApngReset(object);
    }
    const unsigned call = ++apngCalls[2];
    const auto before = ReadApngFields(object);
    SetLastError(incomingError);
    const HRESULT result = originalApngReset(object);
    const DWORD outgoingError = GetLastError();
    const auto after = ReadApngFields(object);
    LogApng("reset", call, object, before, after, false, 0, true, result);
    ZeroNewApngTexture(object, before, after, result);
    SetLastError(outgoingError);
    return result;
}
void InstallApngObserver() {
    if (!apngTrace) return;
    static_assert(sizeof(void*) == 8, "APNG diagnostic requires the verified x64 MME image");
    const HMODULE module = GetModuleHandleW(L"MMEffect.dll");
    Require(module != nullptr, "APNG trace requires already-loaded native MME");
    const auto* image = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    Require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && dos->e_lfanew < 0x1000,
            "verify APNG observer DOS header");
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    Require(pe->Signature == IMAGE_NT_SIGNATURE && pe->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            pe->FileHeader.TimeDateStamp == 0x5331C51D && pe->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            pe->OptionalHeader.SizeOfImage == 0xE7000 && pe->OptionalHeader.CheckSum == 0xE19D9,
            "APNG trace accepts only the verified MMEffect 0.37 x64 image");
    wchar_t modulePath[32768]{}, executablePath[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    const DWORD executableLength = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    Require(moduleLength && moduleLength < std::size(modulePath) && executableLength &&
            executableLength < std::size(executablePath) &&
            _wcsicmp(fs::path(modulePath).parent_path().c_str(), fs::path(executablePath).parent_path().c_str()) == 0,
            "APNG trace module must belong to this staged diagnostic runtime");
    constexpr unsigned rvas[]{0x8230, 0x8290, 0x82C0};
    constexpr unsigned char signatures[][32]{
        {0x40,0x57,0x48,0x83,0xEC,0x20,0x83,0x79,0x48,0x00,0x48,0x8B,0xF9,0x74,0x49,0x48,
         0x8D,0x54,0x24,0x30,0x66,0x0F,0x28,0xD1,0x48,0x89,0x5C,0x24,0x38,0xE8,0x4E,0xFD},
        {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0x89,0x00,0x01,0x00,0x00,
         0x48,0x85,0xC9,0x74,0x11,0x48,0x8B,0x01,0xFF,0x50,0x10,0x48,0xC7,0x83,0x00,0x01},
        {0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x6C,0x24,0x20,0x56,0x57,0x41,0x54,0x48,0x83,
         0xEC,0x40,0x48,0x8D,0xB9,0x00,0x01,0x00,0x00,0x48,0x8B,0xD9,0x45,0x33,0xE4,0x48}
    };
    for (unsigned i = 0; i < std::size(rvas); ++i)
        Require(std::memcmp(image + rvas[i], signatures[i], sizeof(signatures[i])) == 0,
                "verify APNG function bytes before installing observers");
    constexpr unsigned slots[]{0x7770, 0x6780, 0x8230, 0x8290, 0x82C0};
    const auto* table = reinterpret_cast<const void* const*>(image + 0xB2720);
    for (unsigned i = 0; i < std::size(slots); ++i)
        Require(table[i] == image + slots[i] ||
                (i == 4 && IsEffectApngCompatibilityInstalled()),
                "verify APNG class vtable or the exact installed host Reset wrapper");
    apngExpectedVtable = table;
    apngTraceThread = GetCurrentThreadId();
    void* entries[]{const_cast<unsigned char*>(image + rvas[0]),
                    const_cast<unsigned char*>(image + rvas[1]),
                    const_cast<unsigned char*>(image + rvas[2])};
    Require(MH_CreateHook(entries[0], &ObserveApngUpdate, reinterpret_cast<void**>(&originalApngUpdate)) == MH_OK &&
            MH_CreateHook(entries[1], &ObserveApngLost, reinterpret_cast<void**>(&originalApngLost)) == MH_OK &&
            MH_CreateHook(entries[2], &ObserveApngReset, reinterpret_cast<void**>(&originalApngReset)) == MH_OK,
            "create local APNG observation hooks");
    for (void* entry : entries) Require(MH_EnableHook(entry) == MH_OK, "enable local APNG observation hook");
    std::printf("APNG_TRACE_INSTALLED module=%s base=%p thread=%lu timestamp=0x5331C51D image_size=0xE7000 checksum=0xE19D9 function_signatures=3x32 vtable_slots=5 record_limit=%u\n",
        Utf8(modulePath).c_str(), static_cast<void*>(module), apngTraceThread, kApngRecordLimit);
}

struct ApngContractFixture {
    alignas(8) unsigned char object[0x110]{};
    alignas(8) unsigned char head[0x30]{};
    alignas(8) unsigned char node[0x30]{};
};
template <typename T>
void ContractField(unsigned char* object, std::size_t offset, T value) {
    std::memcpy(object + offset, &value, sizeof(value));
}
std::uintptr_t ContractPointer(const unsigned char* object, std::size_t offset) {
    std::uintptr_t value = 0;
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}
struct ApngContractCall {
    ApngReset method = nullptr;
    void* object = nullptr;
    std::atomic<HRESULT> result{E_UNEXPECTED};
    std::atomic<DWORD> error{ERROR_SUCCESS}, thread{0};
};
DWORD WINAPI CallApngContract(void* context) {
    auto& call = *static_cast<ApngContractCall*>(context);
    call.thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    SetLastError(ERROR_BUSY);
    const HRESULT result = call.method(call.object);
    const DWORD error = GetLastError();
    call.result.store(result, std::memory_order_relaxed);
    call.error.store(error, std::memory_order_relaxed);
    return 0;
}
void RunApngContractTests() {
    if (!apngContractTest) return;
    Require(apngTrace && apngExpectedVtable && IsEffectApngCompatibilityInstalled(),
            "APNG contract test requires its observer and installed production wrapper");
    Require(GetCurrentThreadId() == apngTraceThread && !apngContractObject.load(),
            "APNG contract fixtures start on the observer thread without another fixture");
    const auto* table = static_cast<const void* const*>(apngExpectedVtable);
    const auto method = reinterpret_cast<ApngReset>(const_cast<void*>(table[4]));
    Require(method != nullptr && table[4] != reinterpret_cast<void*>(originalApngReset),
            "contract dispatch uses the installed vtable wrapper, not its original trampoline");
    enum class Kind { SameTexture, FailedReset, OtherSuccess, Preloaded, ZeroFrames,
                      NullTexture, BadSentinel, NullSentinel, WrongClass, ForeignThread };
    struct Case { const char* name; Kind kind; };
    constexpr Case cases[]{
        {"success-same-nonnull-texture", Kind::SameTexture},
        {"failed-reset", Kind::FailedReset},
        {"nonzero-success-result", Kind::OtherSuccess},
        {"preloaded-textures", Kind::Preloaded},
        {"zero-frames", Kind::ZeroFrames},
        {"null-texture", Kind::NullTexture},
        {"bad-sentinel-flag", Kind::BadSentinel},
        {"null-sentinel", Kind::NullSentinel},
        {"wrong-class-vtable", Kind::WrongClass},
        {"foreign-thread", Kind::ForeignThread},
    };
    const char* previousPhase = phase;
    phase = "apng-contract";
    unsigned failures = 0, index = 0;
    for (const auto& test : cases) {
        auto fixture = std::make_unique<ApngContractFixture>();
        const auto originalNode = reinterpret_cast<std::uintptr_t>(fixture->node);
        const auto head = reinterpret_cast<std::uintptr_t>(fixture->head);
        const std::uintptr_t texture = test.kind == Kind::NullTexture ? 0 : 0x10000;
        const HRESULT expectedResult = test.kind == Kind::FailedReset ? E_FAIL :
            test.kind == Kind::OtherSuccess ? S_FALSE : S_OK;
        const DWORD expectedError = 0xE1A00000ul + ++index;
        ContractField(fixture->object, 0x00, test.kind == Kind::WrongClass ? std::uintptr_t{0} :
            reinterpret_cast<std::uintptr_t>(apngExpectedVtable));
        ContractField(fixture->object, 0x48, test.kind == Kind::ZeroFrames ? 0u : 101u);
        ContractField(fixture->object, 0x58, 5.125);
        ContractField(fixture->object, 0x68, test.kind == Kind::NullSentinel ? std::uintptr_t{0} : head);
        ContractField(fixture->object, 0x80, originalNode);
        ContractField(fixture->object, 0x100, texture);
        fixture->object[0x108] = test.kind == Kind::Preloaded ? 1 : 0;
        fixture->head[0x29] = test.kind == Kind::BadSentinel ? 0 : 1;
        apngContractResult.store(expectedResult, std::memory_order_relaxed);
        apngContractLastError.store(expectedError, std::memory_order_relaxed);
        apngContractOriginalCalls.store(0, std::memory_order_relaxed);
        apngContractObject.store(fixture->object, std::memory_order_release);
        ApngContractCall call;
        call.method = method; call.object = fixture->object;
        if (test.kind == Kind::ForeignThread) {
            const HANDLE worker = CreateThread(nullptr, 0, CallApngContract, &call, 0, nullptr);
            Require(worker != nullptr, "create isolated APNG contract worker");
            const DWORD wait = WaitForSingleObject(worker, 10000);
            Require(wait == WAIT_OBJECT_0, "join APNG contract worker before releasing its fixture");
            Require(CloseHandle(worker) != FALSE, "close APNG contract worker handle");
        } else {
            CallApngContract(&call);
        }
        apngContractObject.store(nullptr, std::memory_order_release);
        const auto actualNode = ContractPointer(fixture->object, 0x80);
        const auto expectedNode = test.kind == Kind::SameTexture ? head : originalNode;
        const unsigned calls = apngContractOriginalCalls.load(std::memory_order_relaxed);
        const HRESULT result = call.result.load(std::memory_order_relaxed);
        const DWORD error = call.error.load(std::memory_order_relaxed);
        const DWORD thread = call.thread.load(std::memory_order_relaxed);
        const bool threadMatches = (thread == apngTraceThread) == (test.kind != Kind::ForeignThread);
        const bool passed = calls == 1 && result == expectedResult && error == expectedError &&
            actualNode == expectedNode && ContractPointer(fixture->object, 0x100) == texture && threadMatches;
        failures += passed ? 0 : 1;
        std::printf("APNG_CONTRACT_CASE name=%s original_calls=%u hr=0x%08lx expected_hr=0x%08lx last_error=0x%08lx expected_error=0x%08lx node=%p expected_node=%p texture_unchanged=%d thread=%lu foreign_thread=%d passed=%d production_wrapper=1 synthetic=1\n",
            test.name, calls, static_cast<unsigned long>(result), static_cast<unsigned long>(expectedResult),
            error, expectedError, reinterpret_cast<void*>(actualNode), reinterpret_cast<void*>(expectedNode),
            ContractPointer(fixture->object, 0x100) == texture ? 1 : 0,
            thread, test.kind == Kind::ForeignThread ? 1 : 0, passed ? 1 : 0);
    }
    std::printf("APNG_CONTRACT_SUMMARY cases=%zu failures=%u production_wrapper=1 synthetic=1 live_root_cause_proven=0\n",
        std::size(cases), failures);
    Require(failures == 0, "production APNG wrapper preserves reset and invalidation contracts");
    phase = previousPhase;
}

HMENU MenuWithCommand(HMENU menu, UINT command) {
    if (!menu) return nullptr;
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        if (GetMenuItemID(menu, i) == command) return menu;
        if (auto found = MenuWithCommand(GetSubMenu(menu, i), command)) return found;
    }
    return nullptr;
}
void DisableMappingAutoSave(MMDApp* app) {
    auto menu = MenuWithCommand(GetMenu(app->MainWindow()), 40022);
    Require(menu != nullptr, "find native MME mapping autosave setting");
    const UINT before = GetMenuState(menu, 40022, MF_BYCOMMAND);
    if (before & MF_CHECKED) SendMessageW(app->MainWindow(), WM_COMMAND, 40022, 0);
    menu = MenuWithCommand(GetMenu(app->MainWindow()), 40022);
    Require(menu && !(GetMenuState(menu, 40022, MF_BYCOMMAND) & MF_CHECKED),
            "disable native MME mapping autosave before loading user model");
    std::printf("MME mapping_autosave=0 previous_checked=%d\n", (before & MF_CHECKED) ? 1 : 0);
}
void Pump() {
    AuditWindows("before-message-pump");
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "unexpected WM_QUIT");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    AuditWindows("after-message-pump");
}
std::wstring Row(HWND list, int row, int column, LPARAM* identity = nullptr) {
    wchar_t text[2048]{};
    LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row; item.iSubItem = column;
    item.pszText = text; item.cchTextMax = static_cast<int>(std::size(text));
    Require(SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) != 0,
            "read real MME assignment row");
    if (identity) *identity = item.lParam;
    return text;
}
void CALLBACK DriveEditor(HWND, UINT, UINT_PTR timer, DWORD) {
    if (insideEditorTimer) return;
    insideEditorTimer = true;
    struct Leave { ~Leave() { insideEditorTimer = false; } } leave;
    Require(++editorTicks < 100, "MME editor transaction timed out");
    if (editorStep == 2) return;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        const HMENU menu = GetMenu(window);
        if (menu && MenuWithCommand(menu, 40028) && MenuWithCommand(menu, 40007)) editor = window;
        return TRUE;
    }, 0);
    if (!editor) return;
    HWND list = nullptr;
    EnumChildWindows(editor, [](HWND child, LPARAM data) -> BOOL {
        wchar_t cls[64]{}; GetClassNameW(child, cls, 64);
        if (!_wcsicmp(cls, WC_LISTVIEWW) && ListView_GetItemCount(child) > 1) {
            *reinterpret_cast<HWND*>(data) = child; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    if (!list) return;
    int selected = -1;
    for (int row = 0; row < ListView_GetItemCount(list); ++row) {
        LPARAM identity = 0;
        const auto name = Row(list, row, 0, &identity), fx = Row(list, row, 1);
        std::printf("ROW phase=%s row=%d identity=0x%llx name=%s effect=%s\n", phase,
            row, static_cast<unsigned long long>(identity), Utf8(name.c_str()).c_str(), Utf8(fx.c_str()).c_str());
        if (!_wcsicmp(name.c_str(), modelPath.filename().c_str())) selected = row;
    }
    Require(selected >= 0, "find exact model filename in native MME editor");
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Require(ListView_GetSelectedCount(list) == 1, "select exactly one model root");
    editorStep = 1;
    const unsigned previous = pickerCalls;
    SceneState(g_Block, "before-assignment-command");
    assigning = true;
    SendMessageW(editor, WM_COMMAND, 40007, 0);
    assigning = false;
    ResetProbeAt(g_Block, ResetProbeTiming::After);
    SceneState(g_Block, "after-assignment-command");
    Require(pickerCalls == previous + 1, "real Set Effect command invokes one ANSI picker");
    std::printf("ROW_ASSIGNED phase=%s row=%d effect=%s\n", phase, selected,
                Utf8(Row(list, selected, 1).c_str()).c_str());
    Environment("assignment-command-return");
    if (captureWhileEditorOpen) captureWhileEditorOpen();
    // Exercise MME's own Close command. Hidden-window instrumentation makes
    // IsWindowVisible unsuitable for detecting completion, so wait for this
    // synchronous command to return rather than treating hidden as closed.
    editorStep = 2;
    SendMessageW(editor, WM_COMMAND, 40011, 0);
    editorDone = true;
    KillTimer(nullptr, timer);
}
void Reassign(MMDApp* app) {
    ++assignmentCalls;
    editor = nullptr; editorDone = false; editorTicks = editorStep = 0;
    Environment("editor-open"); SceneState(app, "editor-open");
    const UINT_PTR timer = SetTimer(nullptr, 0, 100, DriveEditor);
    Require(timer != 0, "schedule real MME editor interaction");
    SendMessageW(app->MainWindow(), WM_COMMAND, 40005, 0);
    const ULONGLONG deadline = GetTickCount64() + 12000;
    while (!editorDone) {
        Require(GetTickCount64() < deadline, "editor closure timed out");
        Pump();
        if (!editorDone) MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    KillTimer(nullptr, timer);
    Pump(); Environment("editor-closed"); SceneState(app, "editor-closed");
}

void BoneState(MMDApp* app, bool all) {
    if (!primaryModel) {
        std::printf("BONE phase=%s unavailable=no-model\n", phase);
        return;
    }
    auto* model = mdl::Mdl(primaryModel);
    for (unsigned i = 0; i < model->boneCount; ++i) {
        if (!all && static_cast<int>(i) != controlBone) continue;
        float world[16]{}; ExpGetPmdBoneWorldMat(world, 0, static_cast<int>(i));
        const auto& bone = model->boneTable[i];
        std::printf("BONE phase=%s index=%u name=%s bytes=%s origin=(%.9g,%.9g,%.9g) trans=(%.9g,%.9g,%.9g) world=(%.9g,%.9g,%.9g)\n",
            phase, i, Utf8(text_encoding::BoneName(bone, false).c_str()).c_str(), Hex(ExpGetPmdBoneName(0, i)).c_str(),
            bone.position[0], bone.position[1], bone.position[2], bone.trans[0], bone.trans[1], bone.trans[2], world[12], world[13], world[14]);
    }
}
void FixedFrame(MMDApp* app) {
    app->PlaybackActive() = 0; app->state.currentFrame = 0;
    const bool useModel = modelEdit && primaryModel;
    app->state.optflag[0] = useModel ? 0 : 1;
    app->EditMode() = useModel ? ViewportEditMode::Bone : ViewportEditMode::Camera;
    app->CameraReferenceMode() = CameraAttachmentReference::None;
    app->CameraParentModel() = -1;
    app->CameraPositionX() = 0; app->CameraPositionY() = 10; app->CameraPositionZ() = 0;
    app->CameraPitch() = app->CameraYaw() = app->CameraRoll() = 0;
    app->CameraDistance() = -45; app->CameraFov() = 30; app->CameraPerspective() = 0;
    app->ViewOffsetX() = app->ViewOffsetY() = 0;
    app->GroundGridEnabled() = 0; app->FpsOverlayEnabled() = 0;
    app->state.blackBackgroundEnabled = 1;
    if (controlBone >= 0 && !defaultControlPose) {
        auto* bytes = primaryModel;
        if (!bytes || static_cast<unsigned>(controlBone) >= mdl::Mdl(bytes)->boneCount) {
            std::printf("CONTROLLER phase=%s unavailable=missing-model-or-bone fallback_default_pose=1\n", phase);
            return;
        }
        auto& bone = mdl::Bones(bytes)[controlBone];
        bone.trans[0] = .3f - bone.position[0];
        ModelApplyMorphs(bytes);
        SetPhysicsMode(bytes, 0, app->ModelSlots(), 0);
        SetPhysicsMode(bytes, 1, app->ModelSlots(), 0);
    }
}
void Frames(MMDApp* app, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        Pump(); FixedFrame(app);
        app->state.messageSeen = 1; app->DeltaTime() = 1.f / 30;
        SceneState(app, "before-frame", i);
        FrameDriver(app);
        SceneState(app, "after-frame", i);
    }
    BoneState(app, false);
    std::printf("FRAMES phase=%s count=%u timeline=%d camera=(0,10,0,-45,0,0,0) fov=30 shadow=%d\n",
                phase, count, app->state.currentFrame, app->SelfShadowMode());
}

using DeviceSceneCall = HRESULT(WINAPI*)(IDirect3DDevice9*);
DeviceSceneCall originalBeginScene = nullptr, originalEndScene = nullptr;
decltype(&FrameDriver) originalFrameDriver = nullptr;
MMDApp* sceneProbeApp = nullptr;
IDirect3DDevice9* sceneProbeDevice = nullptr; // Identity only, no retained COM reference.
DWORD sceneProbeThread = 0;
thread_local unsigned frameProbeDepth = 0, sceneCallDepth = 0;
unsigned sceneCallNumber = 0;
int observedSceneBalance = 0; // Successful calls through these hooks, not native device state.
struct ProbeDepth {
    unsigned& value;
    explicit ProbeDepth(unsigned& depth) : value(depth) { ++value; }
    ~ProbeDepth() { --value; }
};
void ObserveFrameDriver(MMDApp* app) {
    if (app != sceneProbeApp || GetCurrentThreadId() != sceneProbeThread) { originalFrameDriver(app); return; }
    ProbeDepth depth(frameProbeDepth);
    Require(frameProbeDepth <= 8, "bound unexpected recursive frame calls in the test process");
    maximumFrameDepth = (std::max)(maximumFrameDepth, frameProbeDepth);
    if (reentrantPaintSending && frameProbeDepth > 1) ++nestedPaintFrames;
    std::printf("SCENE_PROBE phase=%s api=FrameDriver event=enter frame_depth=%u paint_active=%d thread=%lu\n",
        phase, frameProbeDepth, reentrantPaintSending ? 1 : 0, GetCurrentThreadId());
    originalFrameDriver(app);
    std::printf("SCENE_PROBE phase=%s api=FrameDriver event=leave frame_depth=%u paint_active=%d\n",
        phase, frameProbeDepth, reentrantPaintSending ? 1 : 0);
}
HRESULT WINAPI ObserveBeginScene(IDirect3DDevice9* device) {
    if (device != sceneProbeDevice || GetCurrentThreadId() != sceneProbeThread) return originalBeginScene(device);
    ProbeDepth depth(sceneCallDepth);
    const unsigned call = ++sceneCallNumber;
    if (reentrantPaintSending) ++nestedPaintBegins;
    std::printf("SCENE_PROBE phase=%s api=BeginScene event=enter call=%u frame_depth=%u api_depth=%u paint_active=%d observed_balance=%d\n",
        phase, call, frameProbeDepth, sceneCallDepth, reentrantPaintSending ? 1 : 0, observedSceneBalance);
    const HRESULT hr = originalBeginScene(device);
    if (SUCCEEDED(hr)) ++observedSceneBalance;
    std::printf("SCENE_PROBE phase=%s api=BeginScene event=return call=%u frame_depth=%u api_depth=%u paint_active=%d hr=0x%08lx observed_balance=%d\n",
        phase, call, frameProbeDepth, sceneCallDepth, reentrantPaintSending ? 1 : 0,
        static_cast<unsigned long>(hr), observedSceneBalance);
    if (!reentrantPaintRan && SUCCEEDED(hr) && frameProbeDepth == 1 && sceneCallDepth == 1 &&
        assignmentCalls == 1 && std::strcmp(phase, "manual-first") == 0) {
        const HWND window = sceneProbeApp->MainWindow();
        Require(OwnWindow(window) && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId(),
                "reentrant paint targets this test process and current window thread only");
        Require(IsWindowEnabled(window) && sceneProbeApp->WindowLayoutReady() != 0,
                "production paint entry is ready to render");
        reentrantPaintRan = true; // Set before dispatch, so a nested BeginScene cannot trigger again.
        reentrantPaintSending = true;
        struct LeavePaint { ~LeavePaint() { reentrantPaintSending = false; } } leave;
        const unsigned previousFrames = nestedPaintFrames;
        std::printf("REENTRANT_PAINT phase=%s event=send synthetic_trigger=1 window=%p outer_begin_call=%u frame_depth=%u observed_balance=%d\n",
            phase, static_cast<void*>(window), call, frameProbeDepth, observedSceneBalance);
        const LRESULT result = SendMessageW(window, WM_PAINT, 0, 0);
        std::printf("REENTRANT_PAINT phase=%s event=returned result=%lld nested_frames=%u nested_begins=%u nested_ends=%u max_frame_depth=%u observed_balance=%d automatic_repair=0\n",
            phase, static_cast<long long>(result), nestedPaintFrames - previousFrames,
            nestedPaintBegins, nestedPaintEnds, maximumFrameDepth, observedSceneBalance);
        Require(nestedPaintFrames > previousFrames, "WM_PAINT actually entered the production FrameDriver recursively");
    }
    return hr; // Preserve even a failed result; never add EndScene or Reset here.
}
HRESULT WINAPI ObserveEndScene(IDirect3DDevice9* device) {
    if (device != sceneProbeDevice || GetCurrentThreadId() != sceneProbeThread) return originalEndScene(device);
    ProbeDepth depth(sceneCallDepth);
    const unsigned call = ++sceneCallNumber;
    if (reentrantPaintSending) ++nestedPaintEnds;
    std::printf("SCENE_PROBE phase=%s api=EndScene event=enter call=%u frame_depth=%u api_depth=%u paint_active=%d observed_balance=%d\n",
        phase, call, frameProbeDepth, sceneCallDepth, reentrantPaintSending ? 1 : 0, observedSceneBalance);
    const HRESULT hr = originalEndScene(device);
    if (SUCCEEDED(hr)) --observedSceneBalance;
    std::printf("SCENE_PROBE phase=%s api=EndScene event=return call=%u frame_depth=%u api_depth=%u paint_active=%d hr=0x%08lx observed_balance=%d\n",
        phase, call, frameProbeDepth, sceneCallDepth, reentrantPaintSending ? 1 : 0,
        static_cast<unsigned long>(hr), observedSceneBalance);
    return hr;
}
void InstallSceneProbe(MMDApp* app) {
    if (!reentrantPaint) return;
    Require(app && app == g_Block && OwnWindow(app->MainWindow()) && app->Renderer() && app->Renderer()->device,
            "scene probe belongs to the newly created test host");
    sceneProbeApp = app; sceneProbeDevice = app->Renderer()->device;
    sceneProbeThread = GetCurrentThreadId();
    Require(GetWindowThreadProcessId(app->MainWindow(), nullptr) == sceneProbeThread,
            "scene probe installs on the test host's own window thread");
    auto** table = *reinterpret_cast<void***>(sceneProbeDevice);
    void* frameEntry = reinterpret_cast<void*>(&FrameDriver);
    Require(MH_CreateHook(frameEntry, &ObserveFrameDriver, reinterpret_cast<void**>(&originalFrameDriver)) == MH_OK &&
            MH_CreateHook(table[41], &ObserveBeginScene, reinterpret_cast<void**>(&originalBeginScene)) == MH_OK &&
            MH_CreateHook(table[42], &ObserveEndScene, reinterpret_cast<void**>(&originalEndScene)) == MH_OK,
            "observe production frame and current-device scene boundaries");
    Require(MH_EnableHook(frameEntry) == MH_OK && MH_EnableHook(table[41]) == MH_OK &&
            MH_EnableHook(table[42]) == MH_OK, "enable local scene-reentry diagnostic hooks");
    std::printf("REENTRANT_PAINT event=installed device=%p begin_entry=%p end_entry=%p thread=%lu target_phase=manual-first synthetic_trigger=1\n",
        static_cast<void*>(sceneProbeDevice), table[41], table[42], sceneProbeThread);
}

using DevicePresent = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
DevicePresent originalPresent = nullptr;
std::function<void(IDirect3DDevice9*)> captureBeforePresent;
HRESULT WINAPI ObservePresent(IDirect3DDevice9* device, const RECT* source,
    const RECT* destination, HWND window, const RGNDATA* dirty) {
    if (captureBeforePresent && !reentrantPaintSending) captureBeforePresent(device);
    else if (captureBeforePresent)
        std::printf("CAPTURE_DEFER phase=%s reason=reentrant-paint production_present_preserved=1\n", phase);
    const HRESULT hr = originalPresent(device, source, destination, window, dirty);
    std::printf("PRESENT phase=%s hr=0x%08lx window=%p\n", phase,
        static_cast<unsigned long>(hr), static_cast<void*>(window));
    return hr;
}

struct Target {
    MMDApp* app;
    ComPtr<IDirect3DSurface9> oldTarget, oldDepth, cachedTarget, cachedDepth, color, depth;
    int oldWidth, oldHeight;
    UINT width = kWidth, height = kHeight;
    D3DFORMAT format = D3DFMT_A8R8G8B8;
    D3DMULTISAMPLE_TYPE sourceMultisample = D3DMULTISAMPLE_NONE;
    DWORD sourceMultisampleQuality = 0;
    void* presentEntry = nullptr;
    unsigned presentSamples = 0, lastCaptureSample = 0;
    std::vector<unsigned char> presentedPixels;
    explicit Target(MMDApp* value) : app(value), oldWidth(value->RenderWidth()), oldHeight(value->RenderHeight()) {
        auto* renderer = app->Renderer(); auto* device = renderer->device;
        if (swapchainBackbuffer) {
            ComPtr<IDirect3DSurface9> backbuffer;
            Require(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)),
                    "get actual swapchain backbuffer without rebinding");
            D3DSURFACE_DESC desc{};
            Require(SUCCEEDED(backbuffer->GetDesc(&desc)), "read swapchain description");
            std::printf("CAPTURE_TARGET mode=swapchain-before-present width=%u height=%u format=%u multisample=%u quality=%lu copy_only_resolve=%d bindings_changed=0\n",
                desc.Width, desc.Height, unsigned(desc.Format), unsigned(desc.MultiSampleType), desc.MultiSampleQuality,
                desc.MultiSampleType != D3DMULTISAMPLE_NONE ? 1 : 0);
            Require(desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8,
                    "swapchain probe supports native A8R8G8B8 or X8R8G8B8 only");
            width = desc.Width; height = desc.Height; format = desc.Format;
            sourceMultisample = desc.MultiSampleType; sourceMultisampleQuality = desc.MultiSampleQuality;
            presentEntry = (*reinterpret_cast<void***>(device))[17];
            Require(MH_CreateHook(presentEntry, &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK,
                    "observe actual device Present");
            captureBeforePresent = [this](IDirect3DDevice9* current) {
                if (current != app->Renderer()->device) return;
                ComPtr<IDirect3DSurface9> surface;
                Require(SUCCEEDED(current->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)),
                        "get current swapchain before Present");
                presentedPixels = ReadSurface(surface.Get());
                ++presentSamples;
            };
            Require(MH_EnableHook(presentEntry) == MH_OK, "enable read-only pre-Present capture");
            return;
        }
        std::printf("CAPTURE_TARGET mode=diagnostic-target width=%u height=%u bindings_changed=1\n", width, height);
        Require(SUCCEEDED(device->GetRenderTarget(0, &oldTarget)), "retain original render target");
        Require(SUCCEEDED(device->GetDepthStencilSurface(&oldDepth)), "retain original depth");
        Require(SUCCEEDED(device->CreateRenderTarget(kWidth, kHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE,
            0, FALSE, &color, nullptr)), "create diagnostic render target");
        Require(SUCCEEDED(device->CreateDepthStencilSurface(kWidth, kHeight, D3DFMT_D24S8, D3DMULTISAMPLE_NONE,
            0, TRUE, &depth, nullptr)), "create diagnostic depth");
        cachedTarget = renderer->backbufferSurface; cachedDepth = renderer->depthStencilSurface;
        if (renderer->backbufferSurface) renderer->backbufferSurface->Release();
        if (renderer->depthStencilSurface) renderer->depthStencilSurface->Release();
        renderer->backbufferSurface = color.Get(); color->AddRef();
        renderer->depthStencilSurface = depth.Get(); depth->AddRef();
        app->RenderWidth() = kWidth; app->RenderHeight() = kHeight;
        Require(SUCCEEDED(device->SetRenderTarget(0, color.Get())) &&
                SUCCEEDED(device->SetDepthStencilSurface(depth.Get())), "bind diagnostic surfaces");
        const D3DVIEWPORT9 view{0, 0, kWidth, kHeight, 0, 1};
        Require(SUCCEEDED(device->SetViewport(&view)), "set diagnostic viewport");
    }
    ~Target() {
        if (presentEntry) {
            captureBeforePresent = {};
            MH_DisableHook(presentEntry); MH_RemoveHook(presentEntry);
            originalPresent = nullptr;
            return;
        }
        auto* renderer = app->Renderer();
        renderer->backbufferSurface->Release(); renderer->depthStencilSurface->Release();
        renderer->backbufferSurface = cachedTarget.Detach(); renderer->depthStencilSurface = cachedDepth.Detach();
        renderer->device->SetRenderTarget(0, oldTarget.Get());
        renderer->device->SetDepthStencilSurface(oldDepth.Get());
        app->RenderWidth() = oldWidth; app->RenderHeight() = oldHeight;
    }
    std::vector<unsigned char> ReadSurface(IDirect3DSurface9* surface) {
        auto* device = app->Renderer()->device;
        D3DSURFACE_DESC desc{};
        Require(SUCCEEDED(surface->GetDesc(&desc)) && desc.Width == width && desc.Height == height &&
                desc.Format == format && desc.MultiSampleType == sourceMultisample &&
                desc.MultiSampleQuality == sourceMultisampleQuality,
                "capture source retains size, format and multisampling settings");
        ComPtr<IDirect3DSurface9> resolved;
        IDirect3DSurface9* copySource = surface;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            // Resolve only into a new unbound surface. Do not change the live
            // render target, depth, viewport, or renderer's cached surfaces.
            const HRESULT createHr = device->CreateRenderTarget(width, height, format,
                D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
            std::printf("READBACK_RESOLVE phase=%s step=create copy_only_resolve=1 hr=0x%08lx multisample=%u bindings_changed=0\n",
                phase, static_cast<unsigned long>(createHr), unsigned(desc.MultiSampleType));
            Require(SUCCEEDED(createHr), "allocate same-size same-format unbound MSAA resolve surface");
            const HRESULT resolveHr = device->StretchRect(surface, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE);
            std::printf("READBACK_RESOLVE phase=%s step=copy copy_only_resolve=1 hr=0x%08lx bindings_changed=0\n",
                phase, static_cast<unsigned long>(resolveHr));
            Require(SUCCEEDED(resolveHr), "resolve actual swapchain MSAA without changing bindings");
            copySource = resolved.Get();
        }
        ComPtr<IDirect3DSurface9> readback;
        Require(SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, format,
            D3DPOOL_SYSTEMMEM, &readback, nullptr)), "allocate BGRA readback");
        const HRESULT hr = device->GetRenderTargetData(copySource, readback.Get());
        if (FAILED(hr)) std::fprintf(stderr, "READBACK_FAILED phase=%s mode=%s hr=0x%08lx\n",
            phase, swapchainBackbuffer ? "swapchain-before-present" : "diagnostic-target", static_cast<unsigned long>(hr));
        Require(SUCCEEDED(hr), "read actual render target without changing bindings");
        D3DLOCKED_RECT lock{};
        Require(SUCCEEDED(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock captured pixels");
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(pixels.data() + size_t(y) * width * 4,
                        static_cast<const unsigned char*>(lock.pBits) + size_t(y) * lock.Pitch, width * 4);
        Require(SUCCEEDED(readback->UnlockRect()), "unlock captured pixels");
        return pixels;
    }
    std::vector<unsigned char> Capture(const wchar_t* filename) {
        Require(!swapchainBackbuffer || presentSamples > lastCaptureSample,
                "fresh swapchain image sampled before a real Present");
        lastCaptureSample = presentSamples;
        auto pixels = swapchainBackbuffer ? presentedPixels : ReadSurface(color.Get());
        std::ofstream file(outputPath / filename, std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        Require(bool(file), "write full BGRA image");
        size_t nonblack = 0, varied = 0;
        unsigned maximum = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            nonblack += (pixels[i] || pixels[i + 1] || pixels[i + 2]) ? 1 : 0;
            varied += std::memcmp(pixels.data(), pixels.data() + i, 3) != 0 ? 1 : 0;
            maximum = (std::max)(maximum, unsigned((std::max)({pixels[i], pixels[i + 1], pixels[i + 2]})));
        }
        std::printf("CAPTURE phase=%s path=%s width=%u height=%u format=%s mode=%s present_samples=%u copy_only_resolve=%d nonblack=%zu varied=%zu max_rgb=%u\n",
            phase, Utf8((outputPath / filename).c_str()).c_str(), width, height,
            format == D3DFMT_X8R8G8B8 ? "BGRX8" : "BGRA8",
            swapchainBackbuffer ? "swapchain-before-present" : "diagnostic-target",
            presentSamples, sourceMultisample != D3DMULTISAMPLE_NONE ? 1 : 0, nonblack, varied, maximum);
        // An empty result after assignment is the symptom under investigation.
        // Retain it and continue through Reload All instead of aborting early.
        if (std::strcmp(phase, "default-companion") == 0)
            Require(nonblack > 0 && varied > 0, "baseline contains a nonempty image before reassignment");
        if (!nonblack || !varied)
            std::printf("CAPTURE_EMPTY phase=%s nonblack=%zu varied=%zu continuing_to_reload=1\n", phase, nonblack, varied);
        return pixels;
    }
};
void Difference(const char* label, const std::vector<unsigned char>& before,
                const std::vector<unsigned char>& after) {
    Require(before.size() == after.size(), "matching capture dimensions");
    size_t changed = 0; unsigned maximum = 0; unsigned long long sum = 0;
    for (size_t i = 0; i < before.size(); i += 4) {
        bool pixelChanged = false;
        for (unsigned channel = 0; channel < 3; ++channel) {
            const unsigned delta = static_cast<unsigned>(std::abs(int(before[i + channel]) - int(after[i + channel])));
            pixelChanged |= delta != 0; maximum = (std::max)(maximum, delta); sum += delta;
        }
        changed += pixelChanged ? 1 : 0;
    }
    std::printf("DIFFERENCE label=%s changed_rgb_pixels=%zu max_delta=%u mean_abs_rgb=%.9g equality_asserted=0\n",
        label, changed, maximum, double(sum) / (before.size() / 4 * 3));
}
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t realPickerSetting[8]{};
    realPicker = GetEnvironmentVariableW(L"MMS_MME_REAL_PICKER", realPickerSetting,
        static_cast<DWORD>(std::size(realPickerSetting))) == 1 && realPickerSetting[0] == L'1';
    offscreenRealPicker = Enabled(L"MMS_MME_OFFSCREEN_REAL_PICKER");
    Require(argc == 4 || argc == 5, "usage: MikuMikuDance.exe model.pmx effect.fx output [0|1|2]");
    modelPath = fs::absolute(argv[1]).lexically_normal(); modelPath.make_preferred();
    effectPath = fs::absolute(argv[2]).lexically_normal(); effectPath.make_preferred();
    outputPath = fs::absolute(argv[3]).lexically_normal(); outputPath.make_preferred();
    Require(argc == 4 || std::wcscmp(argv[4], L"0") == 0 || std::wcscmp(argv[4], L"1") == 0 ||
            std::wcscmp(argv[4], L"2") == 0, "shadow must be 0, 1 or 2");
    const int shadow = argc == 5 ? _wtoi(argv[4]) : 0;
    modelEdit = Enabled(L"MMS_MME_MODEL_EDIT");
    defaultControlPose = Enabled(L"MMS_MME_DEFAULT_POSE");
    apngTrace = Enabled(L"MMS_MME_APNG_TRACE");
    apngZeroNewTexture = Enabled(L"MMS_MME_APNG_ZERO_NEW_TEXTURE");
    apngInvalidateAfterZero = Enabled(L"MMS_MME_APNG_INVALIDATE_AFTER_ZERO");
    apngContractTest = Enabled(L"MMS_MME_APNG_CONTRACT_TEST");
    Require(!apngContractTest || apngTrace,
            "MMS_MME_APNG_CONTRACT_TEST requires MMS_MME_APNG_TRACE=1");
    swapchainBackbuffer = Enabled(L"MMS_MME_SWAPCHAIN_BACKBUFFER");
    reentrantPaint = Enabled(L"MMS_MME_REENTRANT_PAINT");
    const auto resetProbe = Setting(L"MMS_MME_RESET_PROBE");
    if (resetProbe == L"before") resetProbeTiming = ResetProbeTiming::Before;
    else if (resetProbe == L"picker") resetProbeTiming = ResetProbeTiming::Picker;
    else if (resetProbe == L"after") resetProbeTiming = ResetProbeTiming::After;
    else if (resetProbe == L"drawn") resetProbeTiming = ResetProbeTiming::Drawn;
    else Require(resetProbe.empty() || resetProbe == L"off",
                 "MMS_MME_RESET_PROBE must be off, before, picker, after or drawn");
    Require(resetProbeTiming == ResetProbeTiming::Off || (!realPicker && swapchainBackbuffer),
            "MMS_MME_RESET_PROBE requires injected picker and MMS_MME_SWAPCHAIN_BACKBUFFER=1");
    Require(!apngZeroNewTexture || (apngTrace && resetProbeTiming == ResetProbeTiming::Drawn &&
                                  !realPicker && !reentrantPaint),
            "MMS_MME_APNG_ZERO_NEW_TEXTURE requires APNG_TRACE=1 and RESET_PROBE=drawn, without real-picker or reentrant modes");
    if (apngZeroNewTexture)
        std::printf("APNG_ZERO_OPTION enabled=1 max_objects=2 max_attempts_per_object=1 synthetic=1 live_root_cause_proven=0\n");
    Require(!apngInvalidateAfterZero || apngZeroNewTexture,
            "MMS_MME_APNG_INVALIDATE_AFTER_ZERO requires MMS_MME_APNG_ZERO_NEW_TEXTURE=1");
    if (apngInvalidateAfterZero)
        std::printf("APNG_INVALIDATE_OPTION enabled=1 zero_required=1 invalidation_value=tree_sentinel synthetic=1 live_root_cause_proven=0\n");
    if (const auto second = Setting(L"MMS_MME_SECOND_MODEL"); !second.empty()) {
        secondModelPath = fs::absolute(second).lexically_normal(); secondModelPath.make_preferred();
        Require(fs::is_regular_file(secondModelPath), "second model exists");
        Require(_wcsicmp(modelPath.filename().c_str(), secondModelPath.filename().c_str()) != 0,
                "second model needs a distinct filename for unambiguous MME row selection");
    }
    if (const auto repeats = Setting(L"MMS_MME_REASSIGN_REPEATS"); !repeats.empty()) {
        wchar_t* end = nullptr;
        const unsigned long count = std::wcstoul(repeats.c_str(), &end, 10);
        Require(end == repeats.c_str() + repeats.size() && count >= 1 && count <= 20,
                "MMS_MME_REASSIGN_REPEATS must be 1..20");
        reassignRepeats = static_cast<unsigned>(count);
    }
    if (const auto frames = Setting(L"MMS_MME_PICKER_FRAMES"); !frames.empty()) {
        wchar_t* end = nullptr;
        const unsigned long count = std::wcstoul(frames.c_str(), &end, 10);
        Require(end == frames.c_str() + frames.size() && count <= 8,
                "MMS_MME_PICKER_FRAMES must be 0..8");
        pickerFrames = static_cast<unsigned>(count);
    }
    Require(!realPicker || pickerFrames == 0, "MMS_MME_PICKER_FRAMES applies only to the injected picker");
    Require(fs::is_regular_file(modelPath) && fs::is_regular_file(effectPath), "model and FX exist");
    Require(text_encoding::EncodeExact(effectPath.c_str(), CP_ACP, effectAnsi), "FX path is exactly representable by ANSI picker");
    fs::create_directories(outputPath);
    std::printf("INPUT model=%s effect=%s output=%s shadow=%d picker_mode=%s\n", Utf8(modelPath.c_str()).c_str(),
        Utf8(effectPath.c_str()).c_str(), Utf8(outputPath.c_str()).c_str(), shadow, PickerMode());
    std::printf("OPTIONS second_model=%s edit_mode=%s capture_mode=%s assignments_before_reload=%u picker_nested_frames=%u reset_probe=%s reentrant_paint=%d\n",
        secondModelPath.empty() ? "(none)" : Utf8(secondModelPath.c_str()).c_str(),
        modelEdit ? "model" : "camera", swapchainBackbuffer ? "swapchain-before-present" : "diagnostic-target",
        reassignRepeats, pickerFrames, ResetProbeName(resetProbeTiming), reentrantPaint ? 1 : 0);
    wchar_t desktopName[256]{}; DWORD desktopBytes = 0;
    Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                desktopName, sizeof(desktopName), &desktopBytes) != FALSE,
            "read actual test desktop");
    wchar_t hiddenDefaultSetting[4]{};
    const bool hiddenDefault = GetEnvironmentVariableW(L"MMS_MME_HIDDEN_DEFAULT", hiddenDefaultSetting,
        static_cast<DWORD>(std::size(hiddenDefaultSetting))) == 1 && hiddenDefaultSetting[0] == L'1';
    Require(!offscreenRealPicker || (hiddenDefault && realPicker),
            "MMS_MME_OFFSCREEN_REAL_PICKER requires both hidden-default and real-picker options");
    if (hiddenDefault) {
        Require(_wcsicmp(desktopName, L"Default") == 0 && (!realPicker || offscreenRealPicker),
                "real Default-desktop picker requires explicit MMS_MME_OFFSCREEN_REAL_PICKER=1");
        std::printf("HIDDEN_DEFAULT_DESKTOP name=%s mode=%s real_picker=%d desktop_isolated=0\n",
            Utf8(desktopName).c_str(), PickerMode(), realPicker ? 1 : 0);
        if (offscreenRealPicker) {
            const RECT screen = VirtualScreen();
            std::printf("OFFSCREEN_GUARD mode=offscreen-default virtual_screen=(%ld,%ld,%ld,%ld) synchronous_cbt=1 activation_blocked=1 maximize_blocked=1 desktop_isolated=0\n",
                screen.left, screen.top, screen.right, screen.bottom);
        }
    } else {
        Require(std::wcsncmp(desktopName, L"MMS_REPAIR_", 11) == 0,
                "run real-dialog diagnostic on a dedicated MMS_REPAIR_ private desktop");
        std::printf("PRIVATE_DESKTOP name=%s\n", Utf8(desktopName).c_str());
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Require(MH_Initialize() == MH_OK, "initialize diagnostic hooks");
    Require(MH_CreateHook(&ShowWindow, &Hide, reinterpret_cast<void**>(&originalShow)) == MH_OK, "hide windows");
    Require(MH_CreateHook(&SetWindowPos, &PlaceWindow, reinterpret_cast<void**>(&originalSetWindowPos)) == MH_OK &&
            MH_CreateHook(&MoveWindow, &MoveOffscreen, reinterpret_cast<void**>(&originalMoveWindow)) == MH_OK,
            "keep diagnostic windows offscreen across explicit position changes");
    Require(MH_CreateHook(&SetWindowPlacement, &RestoreOffscreen, reinterpret_cast<void**>(&originalSetWindowPlacement)) == MH_OK,
            "keep restored diagnostic window placements hidden and offscreen");
    Require(MH_CreateHook(&MessageBoxW, &UnexpectedWide, nullptr) == MH_OK &&
            MH_CreateHook(&MessageBoxA, &UnexpectedAnsi, nullptr) == MH_OK, "guard message boxes");
    Require(MH_CreateHook(&GetOpenFileNameA, &SelectEffect, reinterpret_cast<void**>(&originalOpenFileA)) == MH_OK &&
            MH_CreateHook(&GetOpenFileNameW, &UnexpectedWidePicker, reinterpret_cast<void**>(&originalOpenFileW)) == MH_OK,
            "observe native file selection with real or injected picker mode");
    Require(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable diagnostic hooks");
    const HHOOK guard = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Require(guard != nullptr, "prevent top-level window activation");
    PlaceExistingHiddenWindows();
    GuardSelfTest();
    auto* app = new MMDApp; g_Block = app;
    app->state = MMDAppState{}; app->InitDefaults();
    Require(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "start production host");
    AuditWindows("host-initialized");
    LogCreatedAdapter(app);
    Require(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"), "real native MME loaded");
    const bool apngCompatibilityInstalled = IsEffectApngCompatibilityInstalled();
    std::printf("APNG_COMPATIBILITY installed=%d\n", apngCompatibilityInstalled ? 1 : 0);
    const auto expectedApngCompatibility = Setting(L"MMS_MME_EXPECT_APNG_COMPATIBILITY");
    Require(expectedApngCompatibility.empty() || expectedApngCompatibility == L"0" ||
            expectedApngCompatibility == L"1", "expected APNG compatibility is 0 or 1");
    Require(expectedApngCompatibility.empty() ||
            apngCompatibilityInstalled == (expectedApngCompatibility == L"1"),
            "production APNG compatibility matches the requested expectation");
    Require(InstallEffectApngCompatibility() == apngCompatibilityInstalled,
            "repeated APNG compatibility installation is idempotent");
    InstallCompileObserver();
    InstallApngObserver();
    RunApngContractTests();
    phase = "initialize-mme"; Frames(app, 1);
    DisableMappingAutoSave(app);
    phase = "load-model"; Environment("before-load");
    LoadModelFile(app, modelPath.c_str(), false);
    Require(app->SelectedModel() != nullptr, "load real PMX");
    primaryModel = app->SelectedModel(); primarySlot = app->SelectedModelSlot();
    const auto* model = mdl::Mdl(primaryModel);
    for (unsigned i = 0; i < model->boneCount; ++i)
        if (std::strcmp(model->boneTable[i].name, "\x83\x41\x83\x6A\x83\x81\x90\xA7\x8C\xE4") == 0)
            controlBone = static_cast<int>(i);
    std::printf("CONTROLLER index=%d requested_x=%s fallback_default_pose=%d\n", controlBone,
        defaultControlPose ? "unchanged" : "0.3", controlBone < 0 || defaultControlPose ? 1 : 0);
    BoneState(app, true); Environment("after-load");
    if (!secondModelPath.empty()) {
        phase = "load-second-model"; Environment("before-load");
        LoadModelFile(app, secondModelPath.c_str(), false);
        Require(ExpGetPmdNum() == 2 && app->SelectedModel() && app->SelectedModel() != primaryModel,
                "load a distinct second PMX without replacing the first");
        Environment("after-load");
    }
    app->SetSelectedModelSlot(primarySlot);
    if (modelEdit) RebuildModelModePanel(app);
    FixedFrame(app);
    SceneState(app, "models-ready");
    app->SelfShadowMode() = static_cast<unsigned char>(shadow);
    InstallSceneProbe(app); // Observe the device entries after MME's first-frame initialization.
    {
        Target target(app);
        phase = "default-companion"; Frames(app, 8); const auto baseline = target.Capture(L"default.bgra");
        captureWhileEditorOpen = [&] {
            std::printf("EDITOR_DRAW phase=%s host_enabled=%d editor_enabled=%d\n", phase,
                        IsWindowEnabled(app->MainWindow()), IsWindowEnabled(editor));
            Frames(app, 1); const auto first = target.Capture(L"assigned-editor-first.bgra");
            Difference("default-to-editor-first", baseline, first);
            ResetProbeAt(app, ResetProbeTiming::Drawn);
            Frames(app, 7); const auto steady = target.Capture(L"assigned-editor-steady.bgra");
            Difference("default-to-editor-steady", baseline, steady);
        };
        phase = "manual-first";
        ResetProbeAt(app, ResetProbeTiming::Before);
        Reassign(app); captureWhileEditorOpen = {};
        Frames(app, 1); const auto closedFirst = target.Capture(L"assigned-closed-first.bgra");
        Difference("default-to-closed-first", baseline, closedFirst);
        Frames(app, 7); auto assigned = target.Capture(L"assigned.bgra");
        Difference("default-to-assigned", baseline, assigned);
        for (unsigned repeat = 1; repeat < reassignRepeats; ++repeat) {
            phase = "manual-repeat";
            std::printf("REASSIGN_REPEAT index=%u total_before_reload=%u\n", repeat + 1, reassignRepeats);
            Reassign(app); Frames(app, 8);
            const auto name = L"assigned-repeat-" + std::to_wstring(repeat + 1) + L".bgra";
            auto next = target.Capture(name.c_str());
            Difference("previous-to-repeat", assigned, next); Difference("default-to-repeat", baseline, next);
            assigned = std::move(next);
        }
        phase = "reload-all"; Environment("before-reload");
        SceneState(app, "before-reload-command");
        SendMessageW(app->MainWindow(), WM_COMMAND, 40004, 0);
        SceneState(app, "after-reload-command");
        Frames(app, 8); const auto reloaded = target.Capture(L"reloaded.bgra");
        Difference("assigned-to-reloaded", assigned, reloaded); Difference("default-to-reloaded", baseline, reloaded);
        phase = "manual-second"; Reassign(app); Frames(app, 8); const auto assignedAgain = target.Capture(L"assigned-again.bgra");
        Difference("reloaded-to-assigned-again", reloaded, assignedAgain);
        Difference("assigned-to-assigned-again", assigned, assignedAgain);
    }
    Require(resetProbeTiming == ResetProbeTiming::Off || resetProbeRan,
            "requested reset probe ran during the first assignment");
    Require(!reentrantPaint || reentrantPaintRan, "requested one-shot reentrant paint probe ran in manual-first");
    if (reentrantPaint)
        std::printf("REENTRANT_PAINT event=summary ran=%d nested_frames=%u nested_begins=%u nested_ends=%u max_frame_depth=%u observed_balance=%d synthetic_trigger=1 automatic_repair=0\n",
            reentrantPaintRan ? 1 : 0, nestedPaintFrames, nestedPaintBegins, nestedPaintEnds,
            maximumFrameDepth, observedSceneBalance);
    Require(!IsWindowVisible(app->MainWindow()), "host remains hidden");
    AuditWindows("before-shutdown");
    phase = "shutdown";
    sceneProbeApp = nullptr; sceneProbeDevice = nullptr;
    ShutdownCleanup(app); delete app; g_Block = nullptr;
    UnhookWindowsHookEx(guard); MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
    if (apngTrace)
        std::printf("APNG_TRACE_SUMMARY update_calls=%u lost_calls=%u reset_calls=%u records=%u invalid_reads=%u truncated=%d\n",
            apngCalls[0], apngCalls[1], apngCalls[2], apngRecords, apngUnreadable,
            apngCalls[0] + apngCalls[1] + apngCalls[2] > apngRecords ? 1 : 0);
    if (apngZeroNewTexture) {
        std::printf("APNG_ZERO_SUMMARY attempts=%u completed=%u synthetic=1 live_root_cause_proven=0\n",
            apngZeroAttempts, apngZeroCompleted);
        Require(apngZeroAttempts > 0 && apngZeroCompleted == apngZeroAttempts,
                "requested synthetic APNG zero injection completed for every attempted object");
    }
    if (apngInvalidateAfterZero) {
        std::printf("APNG_INVALIDATE_SUMMARY attempts=%u completed=%u zero_completed=%u synthetic=1 live_root_cause_proven=0\n",
            apngInvalidateAttempts, apngInvalidateCompleted, apngZeroCompleted);
        Require(apngInvalidateAttempts == apngZeroCompleted && apngInvalidateCompleted == apngZeroCompleted,
                "recovery control invalidated every successfully zeroed object with its verified tree sentinel");
    }
    std::printf("DIAGNOSTIC_COMPLETE picker_calls=%u compile_calls=%u equality_asserted=0\n", pickerCalls, compileCalls);
    return 0;
}
