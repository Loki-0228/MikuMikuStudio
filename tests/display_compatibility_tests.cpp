// Hidden production startup and recording. Never capture the desktop.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/wic_mov_recorder.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include <commctrl.h>
#include <cstring>
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace mikumikustudio;
namespace mikumikustudio {
void DisablePlaybackMenus(HWND);
void RestorePlaybackMenus(MMDApp*);
}
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void Require(bool ok, const char* label) {
    Check(ok, label);
    if (!ok) std::exit(2);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected dialog: %s\n", text); std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected dialog: %ls\n", text); std::exit(2);
}
decltype(&ShowWindow) originalShow = nullptr;
BOOL WINAPI KeepHidden(HWND window, int) { return originalShow(window, SW_HIDE); }
decltype(&GetMonitorInfoW) originalMonitorInfo = nullptr;
const RECT workArea{-960, 40, 0, 580};
bool fitWorkArea = false;
BOOL WINAPI MonitorInfo(HMONITOR monitor, LPMONITORINFO info) {
    const BOOL result = originalMonitorInfo(monitor, info);
    if (result && fitWorkArea) info->rcWork = workArea;
    return result;
}
using PresentFunction = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFunction originalPresent = nullptr;
unsigned presents = 0;
bool fullSource = true, fitsClient = true;
HRESULT WINAPI ObservePresent(IDirect3DDevice9* device, const RECT* source,
                              const RECT* destination, HWND window, const RGNDATA* dirty) {
    if (g_Block && window && window == g_Block->RecordingWindow()) {
        ++presents;
        fullSource &= source && source->left == 0 && source->top == 0 &&
            source->right == 1920 && source->bottom == 1080;
        RECT client{};
        fitsClient &= GetClientRect(window, &client) &&
            (!destination || EqualRect(destination, &client));
    }
    return originalPresent(device, source, destination, window, dirty);
}
template<class Function> void Hook(Function target, Function detour, Function* original = nullptr) {
    Require(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original)) == MH_OK, "create test hook");
    Require(MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK, "enable test hook");
}

void CheckShadowCheckboxPainting(MMDApp* app, HWND checkbox) {
    RECT client{}, main{};
    GetClientRect(checkbox, &client);
    GetClientRect(app->MainWindow(), &main);
    POINT origin{};
    MapWindowPoints(checkbox, app->MainWindow(), &origin, 1);
    HDC painted = CreateCompatibleDC(nullptr), panel = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), main.right, -main.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP paintedBitmap = CreateDIBSection(painted, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP panelBitmap = CreateDIBSection(panel, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(paintedBitmap && panelBitmap, "create offscreen theme test surfaces");
    const auto oldPainted = SelectObject(painted, paintedBitmap), oldPanel = SelectObject(panel, panelBitmap);
    const COLORREF savedTop = app->ThemeColorAt(20), savedBottom = app->ThemeColorAt(11);
    // Exercise distinct panel colors and a live palette change; no desktop capture.
    for (COLORREF top : {RGB(36, 40, 48), RGB(40, 110, 160)}) {
        app->ThemeColorAt(20) = top;
        app->ThemeColorAt(11) = RGB(80, 50, 70);
        HandlePaletteChanged(panel);
        SendMessageW(checkbox, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(painted), PRF_CLIENT);
        bool matches = true;
        for (int y = 1; y < client.bottom - 1; ++y) {
            const int x = client.right - 2;
            matches &= GetPixel(painted, x, y) == GetPixel(panel, origin.x + x, origin.y + y);
            if (y == 1) std::printf("THEME origin=%ld,%ld size=%ld,%ld main=%ld,%ld flags=%d,%d actual=%06lx panel=%06lx\n",
                origin.x, origin.y, client.right, client.bottom, main.right, main.bottom,
                app->state.optflag[0], app->state.optflag[6], GetPixel(painted, x, y), GetPixel(panel, origin.x+x, origin.y+y));
        }
        Check(matches, "dynamic checkbox background matches its shadow panel after theme change");
        unsigned white = 0;
        for (int y = 1; y < client.bottom - 1; ++y)
            for (int x = 20; x < client.right - 4; ++x) {
                const COLORREF color = GetPixel(painted, x, y);
                white += GetRValue(color) > 220 && GetGValue(color) > 220 && GetBValue(color) > 220;
            }
        Check(white > 8, "dynamic checkbox caption stays readable on themed panels");
    }
    app->ThemeColorAt(20) = savedTop;
    app->ThemeColorAt(11) = savedBottom;
    SelectObject(painted, oldPainted); SelectObject(panel, oldPanel);
    DeleteObject(paintedBitmap); DeleteObject(panelBitmap);
    DeleteDC(painted); DeleteDC(panel);
}
}

#include "bottom_panel_layout_checks.hpp"

int main(int argc, char** argv) {
    Require(argc == 3 || argc == 4, "expected startup width and height arguments");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDPIAware();
    Require(MH_Initialize() == MH_OK, "initialize hidden-window guards");
    Hook(&ShowWindow, &KeepHidden, &originalShow);
    Hook(&MessageBoxA, &UnexpectedA);
    Hook(&MessageBoxW, &UnexpectedW);
    auto* app = new MMDApp;
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    Require(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize production host");
    HandleWindowSize(app); // Hidden ShowWindow does not send the visible startup resize.
    Check(!IsWindowVisible(app->MainWindow()), "startup window stays hidden");
    if (argc == 4 && std::strcmp(argv[3], "--bottom-panels") == 0) {
        bottom_panel_checks::Run(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        return failures ? 1 : 0;
    }
    const float shadowRange = app->ShadowDistance();
    const auto light = app->SceneLight();
    const HWND dynamic = GetDlgItem(app->MainWindow(), panel::kAdaptiveShadowCheckbox);
    const HWND shadowSlider = GetDlgItem(app->MainWindow(), panel::kSelfShadowRangeSlider);
    const HWND shadowEdit = GetDlgItem(app->MainWindow(), panel::kSelfShadowRangeEdit);
    const auto checkPrecision = [&](int expected) {
        char text[32]{};
        GetWindowTextA(shadowEdit, text, sizeof(text));
        Check(SendMessageW(shadowSlider, TBM_GETPOS, 0, 0) == expected &&
              std::atoi(text) == expected && ShadowSliderPosition(app) == expected,
              "mode switch preserves the precision value and slider position");
    };
    CheckShadowCheckboxPainting(app, dynamic);
    RECT dynamicRect{}; GetWindowRect(dynamic, &dynamicRect);
    for (int id : {560, 561, 562, 563, 564, 565}) {
        RECT other{}, overlap{}; GetWindowRect(GetDlgItem(app->MainWindow(), id), &other);
        Check(!IntersectRect(&overlap, &dynamicRect, &other), "dynamic checkbox does not overlap existing shadow controls");
    }
    const int initialPrecision = static_cast<int>(SendMessageW(shadowSlider, TBM_GETPOS, 0, 0));
    SendMessageW(dynamic, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(app->MainWindow(), WM_COMMAND, MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED), reinterpret_cast<LPARAM>(dynamic));
    Check(app->AdaptiveShadowResolution(), "real checkbox enables dynamic scaling");
    checkPrecision(initialPrecision);
    Check(app->ShadowDistance() == shadowRange, "enabling adaptive resolution retains exact shadow coverage");
    for (int precision : {0, 7500, 9999, 5000}) {
        SendMessageW(shadowSlider, TBM_SETPOS, TRUE, precision);
        SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(shadowSlider));
        SyncSelfShadowControls(app);
        checkPrecision(precision);
        const float expectedRange = float((10000.0 - precision) / 100000.0);
        Check(app->ShadowDistance() == expectedRange && app->SceneModified(),
              "dynamic slider changes the project's actual shadow coverage");
        Check(std::memcmp(&light, &app->SceneLight(), sizeof(light)) == 0,
              "shadow coverage edits leave the light unchanged");
    }
    SendMessageW(dynamic, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(app->MainWindow(), WM_COMMAND, MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED), reinterpret_cast<LPARAM>(dynamic));
    Check(!app->AdaptiveShadowResolution() && SendMessageW(shadowSlider, TBM_GETPOS, 0, 0) == ShadowSliderPosition(app),
          "disabling dynamic scaling uses the displayed precision as the fixed range");
    checkPrecision(5000);
    for (int precision : {0, 9999, 7500}) {
        SendMessageW(shadowSlider, TBM_SETPOS, TRUE, precision);
        SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(shadowSlider));
        const float fixedRange = app->ShadowDistance();
        app->SceneModified() = 0;
        for (bool enabled : {true, false, true, false}) {
            SendMessageW(dynamic, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(app->MainWindow(), WM_COMMAND,
                MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED), reinterpret_cast<LPARAM>(dynamic));
            SyncSelfShadowControls(app);
            Check(app->AdaptiveShadowResolution() == enabled, "dynamic scaling follows repeated checkbox clicks");
            checkPrecision(precision);
            Check(app->ShadowDistance() == fixedRange && !app->SceneModified(),
                  "unchanged precision keeps exact fixed range without marking the scene modified");
        }
    }
    // Reproduce a tight fixed range followed by dragging right in dynamic mode.
    // The same off-centre receiver must project identically in both modes.
    const float focus[3]{0, 10, 0}, direction[3]{-.5f, -1, .5f};
    for (int shadowMode : {1, 2}) {
        std::array<float, 16> sharedProjection{};
        for (bool enabled : {false, true}) {
            app->AdaptiveShadowResolution() = enabled;
            SetShadowSliderPosition(app, 0);
            SendMessageW(shadowSlider, TBM_SETPOS, TRUE, 9500);
            SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(shadowSlider));
            const auto projection = NativeShadowProjection(focus, direction, app->ShadowDistance(), shadowMode);
            if (!enabled) sharedProjection = projection;
            Check(projection == sharedProjection, "fixed and adaptive sliders produce identical coverage matrices");
            for (float x : {-15.f, 15.f}) for (float y : {0.f, 25.f}) {
                const float px = x*projection[0] + y*projection[4] + projection[12];
                const float py = x*projection[1] + y*projection[5] + projection[13];
                Check(std::abs(px) < 1 && std::abs(py) < 1,
                      "dragging right removes clipping from the former tight coverage");
            }
        }
    }
    app->ShadowDistance() = .01234567f; // A keyframe's unrounded value.
    SyncSelfShadowControls(app);
    checkPrecision(8765);
    app->SceneModified() = 0;
    for (bool enabled : {false, true, false}) {
        SendMessageW(dynamic, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(app->MainWindow(), WM_COMMAND, MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED), reinterpret_cast<LPARAM>(dynamic));
        Check(app->ShadowDistance() == .01234567f && !app->SceneModified(),
              "mode switches preserve fractional keyframe coverage without dirtying the project");
    }
    const auto preference = std::filesystem::path(app->ExeDir()) / L"Data/adaptive_shadow.ini";
    { std::ofstream old(preference); old << "1 0\n"; }
    LoadShadowResolutionPreference(app);
    Check(app->AdaptiveShadowResolution() && app->ShadowDistance() == .01234567f,
          "legacy independent precision cannot overwrite project coverage");
    SyncSelfShadowControls(app);
    checkPrecision(8765);
    const float nearSize = RequiredShadowMapSize(.01f, 1, 1000.f, 25.f, false);
    const float farSize = RequiredShadowMapSize(.01f, 1, 1000.f, 100.f, false);
    Check(std::abs(nearSize - 2500.f) < .01f && std::abs(farSize - 625.f) < .01f,
          "adaptive demand matches projected coverage and decreases with camera distance");
    Check(RequiredShadowMapSize(.01f, 1, 1000.f, 25.f, true) ==
          RequiredShadowMapSize(.01f, 1, 1000.f, 100.f, true),
          "orthographic demand is independent of camera distance");
    Check(RequiredShadowMapSize(.01f, 2, 1000.f, 25.f, false) < nearSize &&
          RequiredShadowMapSize(.005f, 1, 1000.f, 25.f, false) > nearSize,
          "adaptive allocation uses the same mode and range as the actual projection");
    if (argc == 4 && std::strcmp(argv[3], "--shadow-controls") == 0) {
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        return failures ? 1 : 0;
    }
    SetModelNameAutoTranslation(false);
    DisablePlaybackMenus(app->MainWindow());
    Check((GetMenuState(GetMenu(app->MainWindow()), 311, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) == 0,
          "playback does not grey the translation preference");
    RestorePlaybackMenus(app);
    for (bool expected : {true, false, true, false}) {
        SendMessageW(app->MainWindow(), WM_COMMAND, 311, 0);
        Check(gModelNameAutoTranslation == expected, "real menu command toggles translation both ways");
        const UINT state = GetMenuState(GetMenu(app->MainWindow()), 311, MF_BYCOMMAND);
        Check(state != UINT(-1) && ((state & MF_CHECKED) != 0) == expected,
              "menu check mark follows real translation clicks");
    }
    std::printf("STARTUP %d %d\n", app->RenderWidth(), app->RenderHeight());
    Check(app->RenderWidth() == std::atoi(argv[1]) && app->RenderHeight() == std::atoi(argv[2]),
          "startup preserves saved dimensions or uses 1080p defaults");
    if (argc == 4) {
        const int savedWidth = app->RenderWidth(), savedHeight = app->RenderHeight();
        app->DeltaTime() = 1.f / 30;
        app->state.messageSeen = 1;
        FrameDriver(app);
        Hook(&GetMonitorInfoW, &MonitorInfo, &originalMonitorInfo);
        auto* device = app->Renderer()->device;
        auto** vtable = *reinterpret_cast<void***>(device);
        Hook(reinterpret_cast<PresentFunction>(vtable[17]), &ObservePresent, &originalPresent);
        const auto output = std::filesystem::current_path() / L"preview-1080p.mov";
        wcscpy_s(app->AviOutputPath(), 256, output.c_str());
        app->Recorder()->selectedCodec = kNativePngMovCodec;
        app->AviOutputWidth() = 1920; app->AviOutputHeight() = 1080;
        app->AviRecordStartFrame() = 0; app->AviRecordEndFrame() = 2;
        app->AviRecordFps() = 30;
        app->AviIncludeWave() = 0; app->AviCloseAfterExport() = 0;
        fitWorkArea = true;
        StartAviRecordWindow(app);
        fitWorkArea = false;
        Require(IsPngMovRecording(app->Recorder()), "start production MOV recording");
        const HWND preview = app->RecordingWindow();
        Check(!IsWindowVisible(preview), "recording preview stays hidden");
        RECT bounds{}, client{};
        GetWindowRect(preview, &bounds); GetClientRect(preview, &client);
        std::printf("PREVIEW %ld,%ld %ldx%ld client=%ldx%ld\n", bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top, client.right, client.bottom);
        Check(bounds.left >= workArea.left && bounds.top >= workArea.top &&
              bounds.right <= workArea.right && bounds.bottom <= workArea.bottom,
              "preview fits monitor work area with negative coordinates");
        Check(std::llabs(1080LL * client.right - 1920LL * client.bottom) <= 1920 + 1080,
              "preview preserves export aspect ratio within one pixel rounding");
        Check(app->RenderWidth() == 1920 && app->RenderHeight() == 1080,
              "capture keeps requested 1080p dimensions");
        Check((GetWindowLongPtrW(preview, GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX)) ==
              (WS_THICKFRAME | WS_MAXIMIZEBOX), "preview has resize border and size-toggle button");
        fitWorkArea = true;
        SendMessageW(preview, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        GetClientRect(preview, &client);
        Check(client.right == 1920 && client.bottom == 1080, "title button restores original export pixel size");
        SendMessageW(preview, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        GetWindowRect(preview, &bounds);
        Check(bounds.left >= workArea.left && bounds.top >= workArea.top && bounds.right <= workArea.right &&
              bounds.bottom <= workArea.bottom, "second title click fits the screen again");
        RECT dragged{bounds.left, bounds.top, bounds.left + 480, bounds.top + 480};
        Check(SendMessageW(preview, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&dragged)),
              "preview handles border dragging");
        SetWindowPos(preview, nullptr, dragged.left, dragged.top, dragged.right - dragged.left,
                     dragged.bottom - dragged.top, SWP_NOACTIVATE | SWP_NOZORDER);
        GetClientRect(preview, &client);
        Check(std::llabs(1080LL * client.right - 1920LL * client.bottom) <= 1920 + 1080,
              "drag resizing preserves export aspect ratio");
        Check(app->RenderWidth() == 1920 && app->RenderHeight() == 1080,
              "preview resizing leaves capture resolution unchanged");
        fitWorkArea = false;
        for (int i = 0; i < 30 && IsPngMovRecording(app->Recorder()); ++i) {
            app->state.messageSeen = 1;
            FrameDriver(app);
        }
        Check(presents > 0 && fullSource, "present uses full recording source");
        Check(presents > 0 && fitsClient, "present scales to preview client area without cropping");
        Check(!IsPngMovRecording(app->Recorder()) && std::filesystem::exists(output),
              "recording completes and publishes MOV");
        Check(!app->RecordingWindow() && app->RenderWidth() == savedWidth && app->RenderHeight() == savedHeight,
              "recording restores existing render resolution");
    }
    Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
    ShutdownCleanup(app);
    g_Block = nullptr;
    delete app;
    return failures ? 1 : 0;
}
