// Regression: preserve legacy UI layout across DPI-scaled fullscreen transitions.
// --recording-size-only adds the real image-sequence recording variant.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <MinHook.h>

namespace mikumikustudio { void LayoutViewportPanels(MMDApp*); }
using namespace mikumikustudio;
static void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(1);
}
static int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("Unexpected dialog: %s\n", text); std::exit(2);
}
static int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"Unexpected dialog: %ls\n", text); std::exit(2);
}
static bool VisibleStyle(HWND window) {
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
}
static UINT WindowDpi(HWND window) {
    using GetDpi = UINT(WINAPI*)(HWND);
    const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi) return getDpi(window);
    HDC dc = GetDC(window);
    const UINT dpi = dc ? UINT(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi;
}
// Keep the fixture independent of the test machine's desktop track limits.
static WNDPROC sizeTestWindowProc = nullptr;
static LRESULT CALLBACK SizeTestWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto result = CallWindowProcW(sizeTestWindowProc, window, message, wp, lp);
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMaxTrackSize = {32767, 32767};
    }
    return result;
}
static void CheckWindowSizeLimits(MMDApp* app) {
    const HWND window = app->MainWindow();
    auto* renderer = app->Renderer();
    const auto originalParameters = renderer->presentParameters;
    const int originalWidth = renderer->screenWidth, originalHeight = renderer->screenHeight;
    const int maxWidth = renderer->maxTextureWidth, maxHeight = renderer->maxTextureHeight;
    RECT originalBounds{}; GetWindowRect(window, &originalBounds);
    const HWND foreground = GetForegroundWindow();
    Check(maxWidth >= 3840 && maxHeight >= 2400, "test device supports ultrawide and tall window fixtures");
    renderer->screenWidth = renderer->presentParameters.BackBufferWidth = 3840;
    renderer->screenHeight = renderer->presentParameters.BackBufferHeight = 2400;
    Check(PostDeviceReset(app), "create actual backing surface for ultrawide window fixtures");
    sizeTestWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&SizeTestWindowProc)));
    Check(sizeTestWindowProc != nullptr, "allow hidden fixture to exceed the physical desktop");
    auto resize = [&](int width, int height) {
        Check(SetWindowPos(window, nullptr, 80, 80, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "resize hidden production window");
        HandleWindowSize(app);
        RECT outer{}; GetWindowRect(window, &outer);
        return outer;
    };
    auto outer = resize(3440, 1440);
    Check(outer.right - outer.left == 3440 && outer.bottom - outer.top == 1440,
          "ultrawide window is not capped at 2560 pixels");
    outer = resize(3840, 2160);
    Check(outer.right - outer.left == 3840 && outer.bottom - outer.top == 2160,
          "4K window is not capped at 1920 pixels tall");
    RECT client{}; GetClientRect(window, &client);
    const auto viewport = app->ViewportRect();
    Check(app->SidebarWidth() >= 250 && app->SidebarWidth() < client.right &&
          viewport.left >= app->SidebarWidth() && viewport.right <= client.right &&
          viewport.top >= 0 && viewport.bottom <= client.bottom && !IsRectEmpty(&viewport),
          "wide layout keeps sidebar and viewport within the client surface");
    renderer->maxTextureWidth = 1280;
    outer = resize(2000, 1200);
    GetClientRect(window, &client);
    Check(client.right == 1280 && outer.bottom - outer.top == 1200 && outer.left == 80 && outer.top == 80,
          "width cap limits client pixels while preserving height and position");
    renderer->maxTextureWidth = maxWidth;
    renderer->maxTextureHeight = 900;
    outer = resize(2000, 1200);
    GetClientRect(window, &client);
    Check(client.bottom == 900 && outer.right - outer.left == 2000,
          "height cap limits client pixels while preserving width");
    renderer->maxTextureWidth = 1280;
    outer = resize(2000, 1200);
    GetClientRect(window, &client);
    Check(client.right == 1280 && client.bottom == 900 &&
          outer.right - outer.left > 1280 && outer.bottom - outer.top > 900,
          "simultaneous limits retain the complete client surface plus window frame");
    const auto limited = outer;
    HandleWindowSize(app); GetWindowRect(window, &outer);
    Check(EqualRect(&limited, &outer), "repeated layout converges without further window resizing");
    renderer->maxTextureWidth = maxWidth;
    renderer->maxTextureHeight = maxHeight;
    resize(4200, 2700); GetClientRect(window, &client);
    Check(client.right == 3840 && client.bottom == 2400,
          "window cannot exceed the actual editor backing surface");
    renderer->maxTextureWidth = renderer->maxTextureHeight = 0;
    resize(4200, 2700); GetClientRect(window, &client);
    Check(client.right == 3840 && client.bottom == 2400,
          "missing texture caps still respect initialized backing dimensions");
    renderer->maxTextureWidth = maxWidth;
    renderer->maxTextureHeight = maxHeight;
    Check(!IsWindowVisible(window) && GetForegroundWindow() == foreground,
          "size limits keep the hidden host invisible without stealing activation");
    SetWindowPos(window, nullptr, originalBounds.left, originalBounds.top,
        originalBounds.right - originalBounds.left, originalBounds.bottom - originalBounds.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sizeTestWindowProc));
    renderer->presentParameters = originalParameters;
    renderer->screenWidth = originalWidth; renderer->screenHeight = originalHeight;
    Check(PostDeviceReset(app), "restore original backing surface after size checks");
    HandleWindowSize(app);
}
// ---------------------------------------------------------------------------
// --recording-size-only <new-output>
// Runs the real hidden image-sequence recorder and checks the ordinary
// HandleWindowSize boundaries while the editor owns a 1920x1200 backing
// surface and the active capture is 64x48.  The runner copies the test
// executable, its DLLs and read-only resources into <case>/runtime and passes
// a fresh <case>/output.  The mode rejects shared build trees, inherited user
// configuration and test overrides before Direct3D or any window exists.  It
// keeps the production prepare/start/frame-driver/device-reset path intact.
// ---------------------------------------------------------------------------
namespace {

namespace fs = std::filesystem;
namespace image_seq = mikumikustudio::image_sequence;

constexpr int kRecordingEditorWidth = 1920;
constexpr int kRecordingEditorHeight = 1200;
constexpr int kRecordingImageWidth = 64;
constexpr int kRecordingImageHeight = 48;
constexpr int kRecordingEndFrame = 3;
constexpr float kRecordingFps = 30.0f;

std::wstring ModulePath() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        Check(length != 0, "read the running test executable path");
        if (static_cast<std::size_t>(length) < buffer.size())
            return std::wstring(buffer.data(), length);
        Check(buffer.size() < 32768, "test executable path fits the Windows path limit");
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring AnsiToWide(const char* text) {
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    Check(length > 0, "decode the requested new output path");
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    Check(MultiByteToWideChar(CP_ACP, 0, text, -1, value.data(), length) == length,
          "decode the requested new output path");
    value.resize(static_cast<std::size_t>(length) - 1);
    return value;
}

bool SamePath(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

void RequireNoReparse(const fs::path& path, const char* label) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    Check(attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0, label);
}

void RequireAncestorsWithoutReparse(const fs::path& path) {
    for (fs::path current = path; !current.empty(); current = current.parent_path()) {
        RequireNoReparse(current, "isolated recording path cannot redirect through a reparse point");
        if (current == current.root_path() || current.parent_path() == current) break;
    }
}

void CheckRuntimeContents(const fs::path& runtime) {
    std::error_code error;
    fs::recursive_directory_iterator current(runtime, fs::directory_options::none, error);
    const fs::recursive_directory_iterator end;
    Check(!error, "enumerate the disposable runtime directory");
    while (!error && current != end) {
        const fs::path path = current->path();
        const DWORD attributes = GetFileAttributesW(path.c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES &&
              (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
              "runtime files cannot redirect IO through reparse points");
        std::error_code kindError;
        const std::wstring name = path.filename().wstring();
        if (current->is_directory(kindError)) {
            Check(_wcsicmp(name.c_str(), L"Plugins") != 0 &&
                  _wcsicmp(name.c_str(), L"backups") != 0,
                  "runtime contains no plugin or backup directory");
        } else if (current->is_regular_file(kindError)) {
            const std::wstring extension = path.extension().wstring();
            Check(_wcsicmp(extension.c_str(), L".ini") != 0 &&
                  _wcsicmp(extension.c_str(), L".json") != 0 &&
                  _wcsicmp(extension.c_str(), L".pmm") != 0 &&
                  _wcsicmp(extension.c_str(), L".pmm1") != 0 &&
                  _wcsicmp(extension.c_str(), L".bak") != 0,
                  "runtime contains no inherited user configuration, project or backup");
            Check(_wcsicmp(name.c_str(), L"MMEffect.dll") != 0 &&
                  _wcsicmp(name.c_str(), L"MMHack.dll") != 0 &&
                  _wcsicmp(name.c_str(), L"d3d9.dll") != 0,
                  "runtime contains no recording or Direct3D proxy DLL");
        } else {
            Check(false, "runtime contains only directories and regular files");
        }
        Check(!kindError, "inspect disposable runtime entries");
        current.increment(error);
    }
    Check(!error, "scan the complete disposable runtime directory");
}

void CreateIsolatedDirectory(const fs::path& path, const char* label) {
    std::error_code error;
    const bool created = fs::create_directory(path, error);
    Check(!error && created, label);
}

void SetIsolatedEnvironment(const wchar_t* name, const fs::path& value) {
    Check(_wputenv_s(name, value.c_str()) == 0,
          "set the isolated CRT environment for the recording run");
    Check(SetEnvironmentVariableW(name, value.c_str()) != FALSE,
          "set the isolated Win32 environment for the recording run");
}

void ClearInheritedEnvironment() {
    auto* block = GetEnvironmentStringsW();
    Check(block != nullptr, "read the inherited process environment");
    std::vector<std::wstring> names;
    for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
        if (*entry == L'=') continue;  // hidden drive-current-directory entries
        if (_wcsnicmp(entry, L"MMS_", 4) == 0 ||
            _wcsnicmp(entry, L"MIKUMIKUSTUDIO_", 15) == 0 ||
            _wcsnicmp(entry, L"PROBE_", 6) == 0) {
            const wchar_t* separator = wcschr(entry, L'=');
            if (separator != nullptr)
                names.emplace_back(entry, static_cast<std::size_t>(separator - entry));
        }
    }
    FreeEnvironmentStringsW(block);
    for (const auto& name : names) {
        Check(_wputenv_s(name.c_str(), L"") == 0,
              "clear every inherited test override from the CRT environment");
        SetEnvironmentVariableW(name.c_str(), nullptr);
    }
}

fs::path PrepareRecordingIsolation(const std::wstring& requested) {
    std::error_code error;
    const fs::path executable = fs::canonical(fs::path(ModulePath()), error);
    Check(!error && !executable.empty(), "resolve the running test executable");
    const fs::path runtime = executable.parent_path();
    Check(runtime.parent_path() != runtime,
          "test executable must run from a copied runtime directory");
    const fs::path requestedPath = fs::absolute(fs::path(requested), error);
    Check(!error && !requestedPath.empty(), "resolve the requested new output path");
    const fs::path output = fs::weakly_canonical(requestedPath, error);
    Check(!error && !output.filename().empty() && output.filename() != L"." &&
          output.filename() != L"..", "new output must name a directory");
    Check(SamePath(runtime, output.parent_path() / L"runtime"),
          "test executable must run from <case>/runtime next to the new <case>/output");
    Check(fs::is_directory(output.parent_path(), error) && !error,
          "new output parent must be the prepared case directory");
    Check(!fs::exists(output, error) && !error,
          "new output directory must not exist; existing directories are rejected");
    RequireAncestorsWithoutReparse(runtime);
    RequireAncestorsWithoutReparse(output.parent_path());
    CheckRuntimeContents(runtime);
    Check(fs::create_directory(output, error) && !error,
          "create the new output directory without reusing artifacts");
    CreateIsolatedDirectory(output / L"local-profile", "isolate the local application profile");
    CreateIsolatedDirectory(output / L"roaming-profile", "isolate the roaming application profile");
    CreateIsolatedDirectory(output / L"temp", "isolate temporary files");
    CreateIsolatedDirectory(output / L"plugins", "isolate plugin packages");
    CreateIsolatedDirectory(output / L"cache", "isolate the managed cache root");
    CreateIsolatedDirectory(output / L"textures", "isolate the texture cache");
    CreateIsolatedDirectory(output / L"effects", "isolate the effect cache");
    CreateIsolatedDirectory(output / L"images", "create the fresh sequence output directory");

    ClearInheritedEnvironment();
    SetIsolatedEnvironment(L"LOCALAPPDATA", output / L"local-profile");
    SetIsolatedEnvironment(L"APPDATA", output / L"roaming-profile");
    SetIsolatedEnvironment(L"TEMP", output / L"temp");
    SetIsolatedEnvironment(L"TMP", output / L"temp");
    SetIsolatedEnvironment(L"MMS_PLUGIN_TEST_DIR", output / L"plugins");
    SetIsolatedEnvironment(L"MMS_CACHE_MANAGER_DIR", output / L"cache");
    SetIsolatedEnvironment(L"MMS_CACHE_MANAGER_SETTINGS", output / L"settings.json");
    SetIsolatedEnvironment(L"MMS_TEXTURE_CACHE_DIR", output / L"textures");
    SetIsolatedEnvironment(L"MMS_EFFECT_CACHE_DIR", output / L"effects");
    SetIsolatedEnvironment(L"MMS_ANTIALIAS_CONFIG", output / L"antialias.ini");
    SetIsolatedEnvironment(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", output / L"autosave.ini");
    Check(SetCurrentDirectoryW(output.c_str()) != FALSE,
          "enter the disposable output directory");
    return output;
}

decltype(&ShowWindow) recordingOriginalShowWindow = nullptr;
decltype(&SetWindowPos) recordingOriginalSetWindowPos = nullptr;

// Only windows owned by this process are forced hidden; other applications
// keep their normal ShowWindow behavior.
BOOL WINAPI RecordingHiddenShowWindow(HWND window, int command) {
    DWORD process = 0;
    if (window != nullptr && GetWindowThreadProcessId(window, &process) != 0 &&
        process == GetCurrentProcessId())
        return recordingOriginalShowWindow(window, SW_HIDE);
    return recordingOriginalShowWindow(window, command);
}

// Only own top-level windows lose the show bit and the activation side effect;
// geometry, size and z-order arguments pass through unchanged.
BOOL WINAPI RecordingHiddenSetWindowPos(HWND window, HWND insertAfter, int x, int y,
    int width, int height, UINT flags) {
    DWORD process = 0;
    if (window != nullptr && GetWindowThreadProcessId(window, &process) != 0 &&
        process == GetCurrentProcessId() && GetAncestor(window, GA_ROOT) == window)
        flags = (flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE;
    return recordingOriginalSetWindowPos(window, insertAfter, x, y, width, height, flags);
}

LRESULT CALLBACK RecordingCreatedGuard(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWND*>(lparam);
        if (create != nullptr && create->lpcs != nullptr)
            create->lpcs->style &= ~WS_VISIBLE;
    } else if (code == HCBT_ACTIVATE) {
        return 1;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void InstallRecordingGuards() {
    Check(MH_Initialize() == MH_OK, "initialize the checked recording message guards");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA),
        reinterpret_cast<void*>(&UnexpectedA), nullptr) == MH_OK,
        "install the checked MessageBoxA failure guard");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW),
        reinterpret_cast<void*>(&UnexpectedW), nullptr) == MH_OK,
        "install the checked MessageBoxW failure guard");
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow),
        reinterpret_cast<void*>(&RecordingHiddenShowWindow),
        reinterpret_cast<void**>(&recordingOriginalShowWindow)) == MH_OK,
        "install the process-only hidden-window guard");
    Check(MH_CreateHook(reinterpret_cast<void*>(&SetWindowPos),
        reinterpret_cast<void*>(&RecordingHiddenSetWindowPos),
        reinterpret_cast<void**>(&recordingOriginalSetWindowPos)) == MH_OK,
        "install the process-only non-activating placement guard");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable the recording message guards");
}

void RemoveRecordingGuards(HHOOK createdGuard) {
    if (createdGuard != nullptr) UnhookWindowsHookEx(createdGuard);
    for (void* target : {reinterpret_cast<void*>(&MessageBoxA),
             reinterpret_cast<void*>(&MessageBoxW),
             reinterpret_cast<void*>(&ShowWindow),
             reinterpret_cast<void*>(&SetWindowPos)}) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    MH_Uninitialize();
}

BOOL CALLBACK CountVisibleOwnedWindow(HWND window, LPARAM parameter) {
    DWORD process = 0;
    if (window != nullptr && GetWindowThreadProcessId(window, &process) != 0 &&
        process == GetCurrentProcessId() && IsWindowVisible(window) != FALSE)
        ++*reinterpret_cast<unsigned*>(parameter);
    return TRUE;
}

void CheckOwnedWindowsHidden(const char* label) {
    unsigned visible = 0;
    Check(EnumWindows(&CountVisibleOwnedWindow, reinterpret_cast<LPARAM>(&visible)) != FALSE,
          "enumerate the test process top-level windows");
    Check(visible == 0, label);
}

void CheckActivationUnchanged(HWND foreground, HWND active, const char* label) {
    Check(GetForegroundWindow() == foreground && GetActiveWindow() == active, label);
}

void VerifyDeviceBackbuffer(MMDApp* app, UINT width, UINT height, const char* label) {
    bool valid = app != nullptr && app->Renderer() != nullptr &&
                 app->Renderer()->device != nullptr;
    IDirect3DSurface9* surface = nullptr;
    D3DSURFACE_DESC description{};
    if (valid)
        valid = SUCCEEDED(app->Renderer()->device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface != nullptr;
    if (valid) valid = SUCCEEDED(surface->GetDesc(&description));
    if (surface != nullptr) surface->Release();
    Check(valid && description.Width == width && description.Height == height, label);
}

void VerifyCaptureSurface(MMDApp* app, UINT width, UINT height, const char* label) {
    IDirect3DSurface9* surface = app != nullptr ? app->CaptureRenderTarget() : nullptr;
    bool valid = surface != nullptr;
    D3DSURFACE_DESC description{};
    if (valid) valid = SUCCEEDED(surface->GetDesc(&description));
    Check(valid && description.Width == width && description.Height == height, label);
}

bool ReadBmpDimensions(const fs::path& path, int* width, int* height) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    stream.read(reinterpret_cast<char*>(&file), sizeof(file));
    stream.read(reinterpret_cast<char*>(&info), sizeof(info));
    if (!stream || file.bfType != 0x4d42) return false;
    *width = static_cast<int>(info.biWidth);
    *height = static_cast<int>(info.biHeight);
    return true;
}

bool SamePresentParameters(const D3DPRESENT_PARAMETERS& left,
                           const D3DPRESENT_PARAMETERS& right) {
    return left.BackBufferWidth == right.BackBufferWidth &&
           left.BackBufferHeight == right.BackBufferHeight &&
           left.BackBufferFormat == right.BackBufferFormat &&
           left.BackBufferCount == right.BackBufferCount &&
           left.MultiSampleType == right.MultiSampleType &&
           left.MultiSampleQuality == right.MultiSampleQuality &&
           left.SwapEffect == right.SwapEffect &&
           left.hDeviceWindow == right.hDeviceWindow &&
           left.Windowed == right.Windowed &&
           left.EnableAutoDepthStencil == right.EnableAutoDepthStencil &&
           left.AutoDepthStencilFormat == right.AutoDepthStencilFormat &&
           left.Flags == right.Flags &&
           left.FullScreen_RefreshRateInHz == right.FullScreen_RefreshRateInHz &&
           left.PresentationInterval == right.PresentationInterval;
}

bool SamePresentParametersExceptSize(const D3DPRESENT_PARAMETERS& left,
                                     const D3DPRESENT_PARAMETERS& right) {
    return left.BackBufferFormat == right.BackBufferFormat &&
           left.BackBufferCount == right.BackBufferCount &&
           left.MultiSampleType == right.MultiSampleType &&
           left.MultiSampleQuality == right.MultiSampleQuality &&
           left.SwapEffect == right.SwapEffect &&
           left.hDeviceWindow == right.hDeviceWindow &&
           left.Windowed == right.Windowed &&
           left.EnableAutoDepthStencil == right.EnableAutoDepthStencil &&
           left.AutoDepthStencilFormat == right.AutoDepthStencilFormat &&
           left.Flags == right.Flags &&
           left.FullScreen_RefreshRateInHz == right.FullScreen_RefreshRateInHz &&
           left.PresentationInterval == right.PresentationInterval;
}

// Every recording-time layout case keeps the real recorder alive while it
// exercises the ordinary production editor clamp.
void CheckRecordingSizeBoundaries(MMDApp* app, const RECT& initialWindow,
                                  HWND foreground, HWND active) {
    HWND window = app->MainWindow();
    auto* renderer = app->Renderer();
    Check(window != nullptr && renderer != nullptr,
          "recording size checks use the real production editor");
    const int realMaxWidth = renderer->maxTextureWidth;
    const int realMaxHeight = renderer->maxTextureHeight;
    const int savedWidth = app->state.recRTW;
    const int savedHeight = app->state.recRTH;
    Check(savedWidth == kRecordingEditorWidth && savedHeight == kRecordingEditorHeight,
          "recording size checks start from the saved editor backing dimensions");

    auto place = [&](int width, int height, bool first) {
        const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE |
            (first ? 0u : static_cast<UINT>(SWP_NOMOVE));
        Check(SetWindowPos(window, nullptr, first ? 80 : 0, first ? 80 : 0,
            width, height, flags) != FALSE,
            "request a hidden editor size while the real recording is active");
        HandleWindowSize(app);
        RECT outer{}, client{};
        Check(GetWindowRect(window, &outer) != FALSE && GetClientRect(window, &client) != FALSE,
              "read the hidden editor geometry while the real recording is active");
        return std::make_pair(outer, client);
    };

    const auto first = place(1000, 740, true);
    const RECT firstOuter = first.first;
    const RECT firstClient = first.second;
    const int borderWidth = 1000 - firstClient.right;
    const int borderHeight = 740 - firstClient.bottom;
    Check(firstOuter.right - firstOuter.left == 1000 && firstOuter.bottom - firstOuter.top == 740,
          "ordinary recording-time resize is not truncated to the capture size");
    Check(borderWidth > 0 && borderHeight > 0,
          "recording window keeps its real frame and menu");
    Check(firstOuter.left == 80 && firstOuter.top == 80,
          "recording resize keeps the requested top-left position");
    Check(firstClient.right > kRecordingImageWidth && firstClient.bottom > kRecordingImageHeight &&
          firstClient.right < savedWidth && firstClient.bottom < savedHeight,
          "ordinary recording-time client stays above the capture and below the saved backing size");
    std::printf("RECORDING-SIZE first request=1000x740 outer=%ldx%ld client=%ldx%ld border=%dx%d pos=%ld,%ld capture=%dx%d\n",
        firstOuter.right - firstOuter.left, firstOuter.bottom - firstOuter.top,
        firstClient.right, firstClient.bottom, borderWidth, borderHeight,
        firstOuter.left, firstOuter.top, kRecordingImageWidth, kRecordingImageHeight);
    Check(firstOuter.right - firstOuter.left > firstClient.right &&
          firstOuter.bottom - firstOuter.top > firstClient.bottom,
          "ordinary recording-time resize keeps the complete decorated frame");
    Check(image_seq::Active(app) && app->RecordingWindow() != nullptr,
          "ordinary recording-time resize leaves the real image sequence active");
    Check(app->state.recRTW == savedWidth && app->state.recRTH == savedHeight,
          "ordinary recording-time resize does not rewrite the saved editor backing");
    VerifyCaptureSurface(app, kRecordingImageWidth, kRecordingImageHeight,
        "ordinary recording-time resize keeps the real capture target at the export size");
    {
        const RECT beforeRepeat = firstOuter;
        HandleWindowSize(app);
        RECT repeated{};
        Check(GetWindowRect(window, &repeated) != FALSE,
              "read the repeated ordinary recording-time layout");
        Check(EqualRect(&beforeRepeat, &repeated) != FALSE,
              "repeated ordinary recording-time layout is idempotent");
    }
    CheckActivationUnchanged(foreground, active,
        "ordinary recording-time resize never changes foreground or active window");
    CheckOwnedWindowsHidden("ordinary recording-time resize keeps every own top-level window hidden");

    auto limits = [&](int capWidth, int capHeight) {
        const int limitWidth = std::min(capWidth > 0 ? capWidth : 16384, savedWidth);
        const int limitHeight = std::min(capHeight > 0 ? capHeight : 16384, savedHeight);
        return std::make_pair(limitWidth, limitHeight);
    };

    auto verifyCommon = [&](const RECT& outer, const char* label) {
        Check(image_seq::Active(app) && app->RecordingWindow() != nullptr,
              "recording size cases leave the real image sequence active");
        Check(app->state.recRTW == savedWidth && app->state.recRTH == savedHeight,
              "recording size cases do not rewrite the saved editor backing");
        VerifyCaptureSurface(app, kRecordingImageWidth, kRecordingImageHeight,
            "recording size cases keep the real capture target at the export size");
        const RECT beforeRepeat = outer;
        HandleWindowSize(app);
        RECT repeated{};
        Check(GetWindowRect(window, &repeated) != FALSE,
              "read the repeated recording-time layout");
        Check(EqualRect(&beforeRepeat, &repeated) != FALSE,
              "repeated recording-time layout is idempotent");
        CheckActivationUnchanged(foreground, active,
            "recording size cases never change foreground or active window");
        CheckOwnedWindowsHidden("recording size cases keep every own top-level window hidden");
        (void)label;
    };

    auto checkCase = [&](int requestWidth, int requestHeight, int capWidth, int capHeight,
                         const char* label) {
        renderer->maxTextureWidth = capWidth;
        renderer->maxTextureHeight = capHeight;
        const auto limit = limits(capWidth, capHeight);
        const auto result = place(requestWidth, requestHeight, false);
        const RECT outer = result.first;
        const RECT client = result.second;
        const int expectedWidth = std::min(requestWidth - borderWidth, limit.first);
        const int expectedHeight = std::min(requestHeight - borderHeight, limit.second);
        std::printf("RECORDING-SIZE %s request=%dx%d cap=%dx%d saved=%dx%d outer=%ldx%ld client=%ldx%ld pos=%ld,%ld capture=%dx%d\n",
            label, requestWidth, requestHeight, capWidth, capHeight, savedWidth, savedHeight,
            outer.right - outer.left, outer.bottom - outer.top, client.right, client.bottom,
            outer.left, outer.top, kRecordingImageWidth, kRecordingImageHeight);
        Check(client.right == expectedWidth && client.bottom == expectedHeight, label);
        Check(outer.left == 80 && outer.top == 80,
              "recording size cases keep one stable top-left position");
        Check(outer.right - outer.left > client.right &&
              outer.bottom - outer.top > client.bottom,
              "recording size cases keep the complete decorated frame around the client");
        verifyCommon(outer, label);
    };

    // Independent client-axis texture caps.  The other axis must keep the
    // ordinary requested extent instead of inheriting the capped axis value.
    checkCase(2000, 1000, 1280, realMaxHeight,
        "width texture cap limits the client width while preserving height");
    checkCase(1000, 2000, realMaxWidth, 900,
        "height texture cap limits the client height while preserving width");
    checkCase(2000, 2000, 1280, 900,
        "both texture caps limit only their matching client axes");
    renderer->maxTextureWidth = realMaxWidth;
    renderer->maxTextureHeight = realMaxHeight;

    // Real caps restored: the saved editor backing, never the 64x48 capture,
    // still bounds each axis independently.
    checkCase(2400, 1000, realMaxWidth, realMaxHeight,
        "width beyond the saved backing is limited by the saved editor size");
    checkCase(1000, 1600, realMaxWidth, realMaxHeight,
        "height beyond the saved backing is limited by the saved editor size");
    checkCase(2400, 1600, realMaxWidth, realMaxHeight,
        "both axes beyond the saved backing are limited by the saved editor size");
    checkCase(2400, 1600, 0, 0,
        "zero texture caps still respect the saved editor backing");
    renderer->maxTextureWidth = realMaxWidth;
    renderer->maxTextureHeight = realMaxHeight;
    Check(renderer->maxTextureWidth == realMaxWidth && renderer->maxTextureHeight == realMaxHeight,
          "recording size checks restore the real device texture caps");

    Check(SetWindowPos(window, nullptr, initialWindow.left, initialWindow.top,
        initialWindow.right - initialWindow.left, initialWindow.bottom - initialWindow.top,
        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
        "restore the editor window geometry after the recording size checks");
    RECT restored{};
    Check(GetWindowRect(window, &restored) != FALSE &&
          restored.left == initialWindow.left && restored.top == initialWindow.top,
          "editor window position returns after the recording size checks");
    CheckActivationUnchanged(foreground, active,
        "recording size checks never change foreground or active window");
}

int RunRecordingSizeOnly(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 || argv[2] == nullptr || argv[2][0] == '\0') {
        std::fputs("usage: --recording-size-only <new-output>\n", stderr);
        return 2;
    }
    const fs::path output = PrepareRecordingIsolation(AnsiToWide(argv[2]));

    InstallRecordingGuards();
    HHOOK createdGuard = SetWindowsHookExW(WH_CBT, RecordingCreatedGuard, nullptr,
        GetCurrentThreadId());
    Check(createdGuard != nullptr,
          "install the current-thread creation guard before host initialization");
    const HWND foregroundBefore = GetForegroundWindow();
    const HWND activeBefore = GetActiveWindow();

    auto* app = new MMDApp;
    g_Block = app;
    std::memset(&app->state, 0, sizeof(app->state));
    app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE),
          "initialize the hidden production host");
    HWND window = app->MainWindow();
    Check(window != nullptr && !IsWindowVisible(window),
          "hidden production host stays invisible");
    CheckActivationUnchanged(foregroundBefore, activeBefore,
        "host initialization never changes foreground or active window");
    CheckOwnedWindowsHidden("host initialization leaves every own top-level window hidden");

    auto* renderer = app->Renderer();
    Check(renderer != nullptr && renderer->device != nullptr && renderer->d3d9 != nullptr,
          "production Direct3D device is available");
    D3DCAPS9 caps{};
    Check(SUCCEEDED(renderer->device->GetDeviceCaps(&caps)),
          "read the real device capabilities");
    Check(caps.MaxTextureWidth >= static_cast<UINT>(kRecordingEditorWidth) &&
          caps.MaxTextureHeight >= static_cast<UINT>(kRecordingEditorHeight) &&
          renderer->maxTextureWidth >= kRecordingEditorWidth &&
          renderer->maxTextureHeight >= kRecordingEditorHeight,
          "device caps support the 1920x1200 editor backing surface");

    const int initialRenderWidth = app->RenderWidth();
    const int initialRenderHeight = app->RenderHeight();
    const D3DPRESENT_PARAMETERS initialParameters = renderer->presentParameters;
    RECT initialWindow{};
    Check(GetWindowRect(window, &initialWindow) != FALSE,
          "record the initial editor window rectangle");

    renderer->screenWidth = renderer->presentParameters.BackBufferWidth = kRecordingEditorWidth;
    renderer->screenHeight = renderer->presentParameters.BackBufferHeight = kRecordingEditorHeight;
    Check(PostDeviceReset(app), "create the real 1920x1200 editor backing surface");
    VerifyDeviceBackbuffer(app, kRecordingEditorWidth, kRecordingEditorHeight,
        "real device backbuffer is the 1920x1200 editor surface");
    const D3DPRESENT_PARAMETERS editorParameters = renderer->presentParameters;

    sizeTestWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&SizeTestWindowProc)));
    Check(sizeTestWindowProc != nullptr,
          "lift the physical desktop track limit for the hidden fixture");

    const fs::path imageDirectory = output / L"images";
    image_seq::Options options;
    options.directory = imageDirectory.wstring();
    options.width = kRecordingImageWidth;
    options.height = kRecordingImageHeight;
    options.start = 0;
    options.end = kRecordingEndFrame;
    options.fps = kRecordingFps;
    options.format = 0;  // BMP avoids an extra transparent resample chain.
    std::wstring sequenceError;
    if (!image_seq::Prepare(app, options, sequenceError))
        std::fwprintf(stderr, L"%ls\n", sequenceError.c_str());
    Check(image_seq::Active(app), "prepare a real hidden BMP image sequence");
    app->AviOutputWidth() = options.width;
    app->AviOutputHeight() = options.height;
    app->AviRecordStartFrame() = options.start;
    app->AviRecordEndFrame() = options.end;
    app->AviRecordFps() = options.fps;
    app->AviIncludeWave() = 0;
    app->AviStereoOutput() = 0;
    app->AviCloseAfterExport() = false;
    StartAviRecordWindow(app);
    Check(app->RecordingWindow() != nullptr, "start a real hidden recording window");
    Check(image_seq::Active(app) && app->state.recordPlaybackStartPending != 0,
          "real recording is armed and waits for its first frame");
    Check(app->state.recRTW == kRecordingEditorWidth && app->state.recRTH == kRecordingEditorHeight,
          "recording saves the editor backing dimensions instead of the capture size");
    Check(renderer->screenWidth == kRecordingImageWidth &&
          renderer->screenHeight == kRecordingImageHeight,
          "recording resets the real device to the 64x48 export size");
    Check(renderer->presentParameters.BackBufferWidth == static_cast<UINT>(kRecordingImageWidth) &&
          renderer->presentParameters.BackBufferHeight == static_cast<UINT>(kRecordingImageHeight),
          "recording export parameters use the 64x48 capture size");
    VerifyDeviceBackbuffer(app, kRecordingImageWidth, kRecordingImageHeight,
        "real device backbuffer becomes the 64x48 export surface");
    Check(app->RenderWidth() == kRecordingImageWidth &&
          app->RenderHeight() == kRecordingImageHeight,
          "editor render size switches to the export size");
    wchar_t recordingClass[64]{};
    const int recordingClassLength = GetClassNameW(app->RecordingWindow(), recordingClass, 64);
    Check(recordingClassLength > 0 && _wcsicmp(recordingClass, L"RecWindow") == 0,
          "recording starts through the production RecWindow class");
    Check(!IsWindowVisible(app->RecordingWindow()), "recording window stays hidden");
    CheckActivationUnchanged(foregroundBefore, activeBefore,
        "recording start never changes foreground or active window");
    CheckOwnedWindowsHidden("recording start keeps every own top-level window hidden");

    // One real driver pass proves the recording is live before the size checks.
    app->state.messageSeen = 1;
    app->DeltaTime() = 1.0f / kRecordingFps;
    FrameDriver(app);
    Check(image_seq::Active(app) && app->RecordingWindow() != nullptr,
          "one real frame keeps the image sequence recording active");
    Check(app->FrameStepPlayback() != 0 && app->PlaybackActive() != 0,
          "real recording enters frame-step playback");
    Check(app->state.recordedFrameCount >= 1,
          "the first real frame was counted by the production readback");
    Check(!IsWindowVisible(app->RecordingWindow()) && !IsWindowVisible(window),
          "real frame readback keeps host and recorder hidden");
    VerifyCaptureSurface(app, kRecordingImageWidth, kRecordingImageHeight,
        "first real frame creates the 64x48 production capture target");
    CheckActivationUnchanged(foregroundBefore, activeBefore,
        "first real frame never changes foreground or active window");
    CheckOwnedWindowsHidden("first real frame keeps every own top-level window hidden");
    std::error_code fileError;
    const fs::path firstImage(image_seq::FramePath(options, 1));
    Check(fs::is_regular_file(firstImage, fileError) && !fileError,
          "first real frame writes the first consecutive image");

    CheckRecordingSizeBoundaries(app, initialWindow, foregroundBefore, activeBefore);

    // The limited driver loop must finish the sequence through the production
    // beyond-end path; FinishAviRecord is never called from the test.
    unsigned steps = 0;
    while (image_seq::Active(app) && steps < 64) {
        ++steps;
        app->state.messageSeen = 1;
        app->DeltaTime() = 1.0f / kRecordingFps;
        FrameDriver(app);
    }
    std::printf("RECORDING-SIZE drive steps=%u recorded=%d active=%d\n", steps,
        static_cast<int>(app->state.recordedFrameCount),
        static_cast<int>(image_seq::Active(app)));
    Check(!image_seq::Active(app) && app->RecordingWindow() == nullptr &&
          app->FrameStepPlayback() == 0,
          "real recording finishes through the production frame driver");
    Check(app->state.recordPlaybackStartPending == 0,
          "recording completion clears the pending start flag");

    const std::uint64_t expectedFrames = image_seq::FrameCount(options);
    Check(expectedFrames == 4, "recording range produces exactly four images");
    std::uint64_t files = 0;
    fs::directory_iterator entry(imageDirectory, fileError);
    const fs::directory_iterator end;
    while (!fileError && entry != end) {
        std::error_code kindError;
        const std::wstring extension = entry->path().extension().wstring();
        if (entry->is_regular_file(kindError) && !kindError &&
            _wcsicmp(extension.c_str(), L".bmp") == 0)
            ++files;
        entry.increment(fileError);
    }
    Check(!fileError, "enumerate the complete recording output directory");
    Check(files == expectedFrames, "recording writes exactly the expected number of images");
    for (std::uint64_t number = 1; number <= expectedFrames; ++number) {
        const fs::path path(image_seq::FramePath(options, number));
        std::error_code sizeError;
        const auto size = fs::file_size(path, sizeError);
        Check(!sizeError && size > 0, "every consecutive recording image is non-empty");
        int width = 0;
        int height = 0;
        Check(ReadBmpDimensions(path, &width, &height) && width == kRecordingImageWidth &&
              std::abs(height) == kRecordingImageHeight,
              "every recording image is a real 64x48 BMP");
    }

    // The production finish path must restore the device by itself; nothing
    // below rewrites the device size or render size before these checks.
    VerifyDeviceBackbuffer(app, kRecordingEditorWidth, kRecordingEditorHeight,
        "real device backbuffer returns to the 1920x1200 editor surface");
    Check(renderer->screenWidth == kRecordingEditorWidth &&
          renderer->screenHeight == kRecordingEditorHeight,
          "renderer screen size returns to the editor backing dimensions");
    Check(renderer->presentParameters.BackBufferWidth ==
              static_cast<UINT>(kRecordingEditorWidth) &&
          renderer->presentParameters.BackBufferHeight ==
              static_cast<UINT>(kRecordingEditorHeight),
          "editor present parameters return to the 1920x1200 backing size");
    Check(SamePresentParameters(renderer->presentParameters, editorParameters),
          "all meaningful editor present parameters are restored");
    Check(SamePresentParametersExceptSize(renderer->presentParameters, initialParameters),
          "editor present parameters other than the backing size match the pre-test values");
    Check(app->RenderWidth() == initialRenderWidth &&
          app->RenderHeight() == initialRenderHeight,
          "editor render size returns to its pre-recording values");
    Check(!IsWindowVisible(window), "editor host stays hidden after recording");
    CheckActivationUnchanged(foregroundBefore, activeBefore,
        "recording completion never changes foreground or active window");
    CheckOwnedWindowsHidden("recording completion keeps every own top-level window hidden");

    RECT completedWindow{};
    Check(GetWindowRect(window, &completedWindow) != FALSE &&
          completedWindow.left == initialWindow.left &&
          completedWindow.top == initialWindow.top,
          "main window position is unchanged after recording");
    HandleWindowSize(app);
    RECT layoutWindow{}, layoutClient{};
    Check(GetWindowRect(window, &layoutWindow) != FALSE &&
          GetClientRect(window, &layoutClient) != FALSE,
          "read the post-recording layout geometry");
    Check(layoutWindow.left == initialWindow.left && layoutWindow.top == initialWindow.top,
          "repeated layout keeps the main window position after recording");
    Check(layoutClient.right > kRecordingImageWidth &&
          layoutClient.bottom > kRecordingImageHeight &&
          layoutClient.right <= kRecordingEditorWidth &&
          layoutClient.bottom <= kRecordingEditorHeight,
          "repeated layout after recording respects the editor backing, not the capture size");
    Check(renderer->screenWidth == kRecordingEditorWidth &&
          renderer->screenHeight == kRecordingEditorHeight &&
          SamePresentParameters(renderer->presentParameters, editorParameters),
          "repeated layout leaves the restored device surface untouched");
    CheckActivationUnchanged(foregroundBefore, activeBefore,
        "repeated layout never changes foreground or active window");

    Check(SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(sizeTestWindowProc)) != 0,
        "restore the production window procedure");
    ShutdownCleanup(app);
    g_Block = nullptr;
    delete app;
    RemoveRecordingGuards(createdGuard);
    std::puts("Recording editor size regressions passed");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 3) SetProcessDPIAware();
    if (argc > 1 && std::strcmp(argv[1], "--recording-size-only") == 0)
        return RunRecordingSizeOnly(argc, argv);
    const bool limitsOnly = argc > 1 && std::strcmp(argv[1], "--window-size-only") == 0;
    const int width = !limitsOnly && argc > 1 ? std::atoi(argv[1]) : 1000;
    const int height = !limitsOnly && argc > 2 ? std::atoi(argv[2]) : 740;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MH_Initialize();
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&UnexpectedA), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&UnexpectedW), nullptr);
    MH_EnableHook(MH_ALL_HOOKS);
    auto* app = new MMDApp;
    g_Block = app;
    // Avoid the large state temporary on the default 1 MB test-thread stack.
    std::memset(&app->state, 0, sizeof(app->state));
    app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production host");
    HWND window = app->MainWindow();
    Check(!IsWindowVisible(window), "host stays hidden");
    CheckWindowSizeLimits(app);
    if (limitsOnly) {
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        std::puts("Window size regressions passed");
        return 0;
    }
    SetWindowPos(window, nullptr, 80, 80, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    HWND extra = CreateWindowW(L"STATIC", L"visible extension", WS_CHILD | WS_VISIBLE,
        3, 3, 20, 20, window, reinterpret_cast<HMENU>(9001), nullptr, nullptr);
    HWND hidden = CreateWindowW(L"STATIC", L"hidden extension", WS_CHILD,
        3, 3, 20, 20, window, reinterpret_cast<HMENU>(9002), nullptr, nullptr);
    std::vector<std::pair<HWND, bool>> children;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        children.emplace_back(child, VisibleStyle(child));
    std::vector<RECT> controlBounds;
    for (const auto& child : children) { RECT rc{}; GetWindowRect(child.first, &rc); controlBounds.push_back(rc); }
    RECT before{}; GetWindowRect(window, &before);
    RECT beforeView = app->ViewportRect();
    const auto beforeParams = app->Renderer()->presentParameters;
    const auto beforeStyle = GetWindowLongPtrW(window, GWL_STYLE);
    const auto menu = GetMenu(window);
    const int sidebar = app->SidebarWidth();
    std::printf("NORMAL window=%ldx%ld viewport=%ldx%ld sidebar=%d dpi=%u children=%zu\n",
        before.right-before.left, before.bottom-before.top,
        beforeView.right-beforeView.left, beforeView.bottom-beforeView.top,
        sidebar, WindowDpi(window), children.size());
    for (int cycle = 0; cycle < 2; ++cycle) {
        SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, LPARAM(1) << 29);
        Check(app->FullscreenMode() != 0, "Alt+Enter reaches fullscreen handler");
        Check(app->Renderer()->presentParameters.Windowed != FALSE, "interactive fullscreen preserves desktop display mode");
        Check(app->Renderer()->device->TestCooperativeLevel() == D3D_OK, "fullscreen device reset succeeds");
        Check(GetMenu(window) == nullptr, "fullscreen menu is removed");
        Check(!IsWindowVisible(window), "background check does not show its host");
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT full{}; GetWindowRect(window, &full);
        std::printf("FULLSCREEN bounds=%ld,%ld %ldx%ld backbuffer=%ux%u\n", full.left, full.top, full.right-full.left, full.bottom-full.top, app->Renderer()->presentParameters.BackBufferWidth, app->Renderer()->presentParameters.BackBufferHeight);
        Check(EqualRect(&full, &monitor.rcMonitor), "fullscreen covers the monitor in the current DPI coordinate space");
        bool allHidden = true;
        for (const auto& child : children) allHidden &= !VisibleStyle(child.first);
        Check(allHidden, "fullscreen hides every child control");
        ApplyFullscreenWindowState(app); // repeated entry must not overwrite the snapshot
        HandleWindowSize(app);
        LayoutViewportPanels(app);
        RelayoutSidebarControls(app);
        Check(app->SidebarWidth() == sidebar, "fullscreen resize preserves ordinary sidebar width");
        Check(!VisibleStyle(extra) && !VisibleStyle(hidden), "fullscreen relayout keeps extensions hidden");
        const RECT view = app->ViewportRect();
        const long long cross = static_cast<long long>(view.right-view.left)*app->RenderHeight() -
            static_cast<long long>(view.bottom-view.top)*app->RenderWidth();
        Check(std::llabs(cross) <= 2LL*(app->RenderWidth()+app->RenderHeight()), "fullscreen preserves scene aspect ratio");
        auto* device = app->Renderer()->device;
        IDirect3DSurface9* buffer = nullptr;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &buffer)), "read fullscreen backbuffer");
        device->ColorFill(buffer, nullptr, D3DCOLOR_XRGB(255,255,255));
        FillFramePresentationMargins(app, device);
        D3DSURFACE_DESC desc{}; buffer->GetDesc(&desc);
        IDirect3DSurface9* resolved = nullptr;
        Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
            SUCCEEDED(device->StretchRect(buffer, nullptr, resolved, nullptr, D3DTEXF_NONE)), "resolve fullscreen backbuffer");
        IDirect3DSurface9* readback = nullptr;
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &readback, nullptr)) && SUCCEEDED(device->GetRenderTargetData(resolved, readback)), "read back margin pixels");
        D3DLOCKED_RECT pixels{}; readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY);
        if (view.left > 0 || view.top > 0)
            Check((*static_cast<const DWORD*>(pixels.pBits) & 0xffffff) == 0, "fullscreen bars contain black pixels without stale controls");
        readback->UnlockRect(); readback->Release(); resolved->Release(); buffer->Release();
        SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, (LPARAM(1)<<29) | (LPARAM(1)<<30));
        Check(app->FullscreenMode() != 0, "holding Alt+Enter does not toggle repeatedly");
        if (cycle == 0) SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, LPARAM(1)<<29);
        else SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(app->FullscreenMode() == 0, "Alt+Enter and Escape restore ordinary window");
        Check(!IsWindowVisible(window), "restored background host stays hidden");
        RECT restored{}; GetWindowRect(window, &restored);
        Check(EqualRect(&before, &restored) && EqualRect(&beforeView, &app->ViewportRect()), "ordinary window and viewport restore exact dimensions");
        Check(GetWindowLongPtrW(window, GWL_STYLE) == beforeStyle && GetMenu(window) == menu, "ordinary style and menu are restored");
        bool visibilityRestored = true;
        for (const auto& child : children) visibilityRestored &= VisibleStyle(child.first) == child.second;
        Check(visibilityRestored, "original control visibility is restored");
        bool positionsRestored = true;
        for (size_t i = 0; i < children.size(); ++i) {
            RECT rc{}; GetWindowRect(children[i].first, &rc);
            positionsRestored &= EqualRect(&rc, &controlBounds[i]) != FALSE;
        }
        Check(positionsRestored, "all controls restore their original positions and sizes");
        Check(app->Renderer()->presentParameters.BackBufferWidth == beforeParams.BackBufferWidth &&
            app->Renderer()->presentParameters.BackBufferHeight == beforeParams.BackBufferHeight,
            "ordinary backbuffer size is restored");
    }
    ShutdownCleanup(app);
    g_Block = nullptr;
    delete app;
    std::puts("Fullscreen window regressions passed");
}
