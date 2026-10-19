// ===========================================================================
// AVI record starters and the fullscreen-window helper they depend on.
//
//   VA 0x0045E820 - StartAviRecordWindow: windowed AVI dump (menu 0xDF,
//                   app+0xA0D61 == 0 path).  Probes the output file with
//                   _wsopen_s/_close, grows the render target and resets the
//                   device when the output size exceeds the current RT,
//                   creates the "RecWindow" recording window at app+0xA0D24,
//                   lazily creates the recording render-target surface
//                   (render-sub+0x1D534, flag 0x1D4F8), hands the recording
//                   config to the DirectShow graph builder 0x409A80 and then
//                   parks the UI: frame 0x198 disabled, main/aux windows
//                   hidden, timeline seek to app+0xA0B00, 9EDD8 armed and
//                   timeBeginPeriod(1).
//   VA 0x00464760 - StartAviRecordFullscreen: the 3D Vision-safe variant
//                   (app+0xA0D61 != 0).  Rejects outputs larger than the
//                   screen, flips app+0xA0274 + 0x4629D0 (borderless main
//                   window), resets the device, reuses the main window as
//                   the record window and runs the same 0x409A80 tail.
//   VA 0x004629D0 - fullscreen enter/restore window manager.
//   VA 0x00401BD0 - Sub048-object physics kick (vtable slot 4 of the object
//                   at obj+0x3C); runs once recording starts.
//
// NOTE: the shared stubs.cpp still exports the names Sub45E820/Sub464760;
// the 0xDF dispatch (command_file_menu.cpp case 223) calls these
// implementations instead.  ApplyFullscreenWindowState previously lived as an inline stub in
// command_view_menu.cpp; the real body replaces it here.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/wic_mov_recorder.hpp"
#include <Windows.h>
#include <d3d9.h>
#include <mmsystem.h>

#include <algorithm>
#include <fcntl.h>
#include <io.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/recording_preview.hpp"

namespace mikumikustudio {

// Bodies live in other translation units (see ported_funcs.hpp / their TUs).
void RefreshAfterFrameApply(MMDApp* app);          // VA 0x00432FA0, was Sub432FA0
void DisablePlaybackMenus(HWND hwnd);              // VA 0x00429790
void RestorePlaybackMenus(MMDApp* app);            // VA 0x004298E0

// VA 0x00401BD0 - "physics kick" through the Sub048 object (app+0x9EDB0).
// The original is a four-instruction TAIL JUMP:
//   ecx = [ecx+0x3C]; eax = [ecx]; edx = [eax+0x10]; jmp edx
// - a thiscall tail-jump into vtable slot 4 (offset 0x10) of the
// scene+0x3C object (the btSequentialImpulseConstraintSolver).  With the
// vendored bullet 2.75 header layout (dtor, dtor, prepareSolve,
// solveGroup, allSolved, reset) offset 0x10 is btConstraintSolver::
// allSolved - an EMPTY default implementation ({};), so the original's
// kick is observably a no-op.  Calling the slot with a guessed signature
// crashed recording right after record start whenever the runtime
// vtable did not match (AVI output stuck at 0 bytes, AV at
// RecordStartTail 0x41C1ED).  Skip the dispatch entirely - the
// observable behaviour (nothing) is identical.
void KickRecordPhysics(MMDApp* app) {  // was Sub401BD0
    (void)app;
}

// VA 0x00409A80 - DirectShow recording graph builder (thiscall on the
// app+0xA06C0 object); defined in src/app/dshow_record_graph.cpp.
bool BuildRecordingGraph(DShowRecorder* recorder, HWND hwnd,
                         unsigned char english,
                         void* recStruct, void* config, float fps,
                         std::uint32_t one,
                         std::uint32_t averageBitrate,
                         const wchar_t* wavPath, float seconds,
                         const WaveAudioContext* audio,
                         unsigned char uiLanguage);  // was Sub409A80

namespace {

// 0x52E040: "録画中…" / 0x52E04C: "Recording..."
const char kRecTitleJp[] = "\x98\x5E\x89\xE6\x92\x86\x81\x63";
const char kRecTitleEn[] = "Recording...";
// 0x52DFF8: "録画ウィンドウ作成" (caption), text 0x52E00C "CreateWindow failed"
const char kCaptionRecWndJp[] =
    "\x98\x5E\x89\xE6\x83\x45\x83\x42\x83\x93\x83\x68\x83\x45\x8D\xEC\x90\xAC";
// 0x52E060/0x52E0B8: "出力ファイルを作成できません。..." / "AVI出力"
const char kMsgSaveFailJp[] =
    "\x8F\x6F\x97\xCD\x83\x74\x83\x40\x83\x43\x83\x8B\x82\xF0\x8D\xEC\x90\xAC"
    "\x82\xC5\x82\xAB\x82\xDC\x82\xB9\x82\xF1\x81\x42\x0A\x91\xBC\x82\xCC\x83"
    "\x41\x83\x76\x83\x8A\x83\x50\x81\x5B\x83\x56\x83\x87\x83\x93\x93\x99\x82"
    "\xC5\x8A\x4A\x82\xA2\x82\xC4\x82\xA2\x82\xC8\x82\xA2\x82\xA9\x8A\x6D\x94"
    "\x46\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2\x81\x42";
const char kCaptionAviOutJp[] = "\x41\x56\x49\x8F\x6F\x97\xCD";
// 0x52E0C0/0x52E108: EN pair for the same failure
const char kMsgSaveFailEn[] =
    "Cannot make save file.\nPlease check whether other application open it.";
const char kCaptionAviOutEn[] = "AVI save failed";
// 0x52E690: "3D Vision出力時は、画面解像度以上のサイズのAVIは出力できません"
const char kMsg3dVisionJp[] =
    "\x33\x44\x20\x56\x69\x73\x69\x6F\x6E\x8F\x6F\x97\xCD\x8E\x9E\x82\xCD\x81"
    "\x41\x89\x66\x96\xCA\x89\xF0\x91\xFC\x93\x78\x88\xC8\x8F\xE3\x82\xCC\x83"
    "\x54\x83\x43\x83\x59\x82\xCC\x41\x56\x49\x82\xCD\x8F\x6F\x97\xCD\x82\xC5"
    "\x82\xAB\x82\xDC\x82\xB9\x82\xF1";

// Recording config block handed to the 0x409A80 DirectShow builder.  The
// field order is fixed by the MMDxShow.dll source-filter protocol.
struct RecConfig {
    std::uint32_t size;    // 0x28
    std::int32_t width;
    std::int32_t height;
    std::uint16_t flag1;   // 1
    std::uint16_t bpp;     // 0x20
    std::uint32_t zero;
    std::uint32_t bytes;   // w*h*4
    std::uint32_t zero2[4];
};
static_assert(sizeof(RecConfig) == 0x28);

bool ProbeWritable(const wchar_t* path) {  // _wsopen_s/_close pair, 0x45E849
    int fd = -1;
    const errno_t r =
        _wsopen_s(&fd, path, 0x8301 /*_O_BINARY|_O_CREAT|_O_TRUNC|_O_WRONLY*/,
                  0x40 /*_SH_DENYNO*/, 0x80 /*_S_IWRITE*/);
    if (r != 0)
        return false;
    if (fd >= 0)
        _close(fd);
    return true;
}

// Resizes the render target to the requested export dimensions when they do
// not match the preview target (shared by 0x45E820 and the 0x114 picture
// renderer).
bool GrowRenderTarget(MMDApp* app, std::int32_t w, std::int32_t h) {
    auto& s = *app;
    D3DRenderer* r = s.Renderer();
    if (r == nullptr)
        return false;
    s.state.recRTW = r->screenWidth;
    s.state.recRTH = r->screenHeight;
    if (w != s.state.recRTW ||
        h != s.state.recRTH) {
        const auto previousParameters = r->presentParameters;
        r->presentParameters.BackBufferWidth = w;
        r->presentParameters.BackBufferHeight = h;
        r->screenWidth = w;
        r->screenHeight = h;
        if (PostDeviceReset(app))    // 0x440DB0 (0x45E94E)
            return true;
        // Restore the preview size so another export attempt cannot mistake
        // a failed resize for an already available render target.
        r->presentParameters = previousParameters;
        r->screenWidth = s.state.recRTW;
        r->screenHeight = s.state.recRTH;
        PostDeviceReset(app);
        return false;
    }
    return true;
}

// Lazy creation of the recording render target (0x45EA68..0x45EACC):
// CreateRenderTarget into captureSurface gated by multisampleAvailable,
// falling back to SetRenderTarget when creation is unavailable.
void EnsureRecordRenderTarget(MMDApp* app) {
    auto& s = *app;
    D3DRenderer* r = s.Renderer();
    if (r == nullptr)
        return;
    if (r->multisampleAvailable == 0) {
        if (r->captureSurface == nullptr) {
            auto* device = r->device;
            if (device == nullptr)
                return;
            IDirect3DSurface9* surf = nullptr;
            const HRESULT hr = device->CreateRenderTarget(
                static_cast<std::uint32_t>(r->screenWidth),
                static_cast<std::uint32_t>(r->screenHeight),
                r->backbufferFormat,
                D3DMULTISAMPLE_NONE, 0, TRUE /*Lockable*/, &surf, nullptr);
            r->captureSurface = surf;
            if (SUCCEEDED(hr) && surf != nullptr)
                r->multisampleAvailable = 1;
        }
        if (r->multisampleAvailable == 0) {
            auto* device = r->device;
            if (device == nullptr)
                return;
            device->SetRenderTarget(0, r->captureSurface);
        }
    }
}

// 0x409A80 - DirectShow recording graph builder (thiscall on the app+0xA06C0
// object).  Still a documented stub: see src/app/dshow_record_graph.cpp.

// Common tail after a successful 0x409A80 handshake: timeline seek to the
// record start frame, arm 9EDD8 and hand control to the physics kick
// (0x45EC27..0x45EC67 / 0x4649AE..0x4649EB).
void RecordStartTail(MMDApp* app) {
    auto& s = *app;
#ifdef MIKUMIKUSTUDIO_DIAG
    if (getenv("MIKUMIKUSTUDIO_TRACE_REC")) {
        FILE* tf = fopen(getenv("MIKUMIKUSTUDIO_TRACE_REC"), "a");
        if (tf) {
            fputs("RecordStartTail enter\n", tf);
            fclose(tf);
        }
    }
#endif
    s.state.recordSavedFrame =
        s.state.currentFrame;               // 0x45EC35
    if (s.AviRecordStartFrame() !=
        s.state.currentFrame) {
        s.state.currentFrame =
            s.AviRecordStartFrame();                            // 0x45EC3D
        RefreshAfterFrameApply(app);                           // 0x432FA0
        PostViewRefresh(app);                                  // 0x40D130
    }
    s.state.recordedFrameCount = 0;
    s.state.recordPlaybackStartPending = 1;              // 0x45EC52
    timeBeginPeriod(1);                                        // 0x45EC58
    KickRecordPhysics(app);                                            // 0x401BD0
#ifdef MIKUMIKUSTUDIO_DIAG
    if (getenv("MIKUMIKUSTUDIO_TRACE_REC")) {
        FILE* tf = fopen(getenv("MIKUMIKUSTUDIO_TRACE_REC"), "a");
        if (tf) {
            fputs("RecordStartTail done\n", tf);
            fclose(tf);
        }
    }
#endif
}

// Shared config/seconds computation and 0x409A80 invocation
// (0x45EB7E..0x45EBCA and 0x46493D..0x464976).
bool StartRecordGraph(MMDApp* app, std::int32_t outW, std::int32_t outH) {
    auto& s = *app;
    if (image_sequence::Active(app)) {
        TeardownDShowGraph(s.Recorder());
        return true;
    }
    RecConfig config;
    std::memset(&config, 0, sizeof config);
    config.size = 0x28;
    config.width = outW;
    config.height = outH;
    config.flag1 = 1;
    config.bpp = 0x20;
    config.bytes =
        static_cast<std::uint32_t>(outW) * outH * 4;

    const std::int32_t frameStart = s.AviRecordStartFrame();
    const std::int32_t frameEnd = s.AviRecordEndFrame();
    std::int32_t frames = frameEnd - frameStart + 1;
    double f = static_cast<double>(frames);
    if (frames < 0)
        f += 4294967296.0f;  // 0x52B9F0 float 2^32 (0x45EB6A)
    // 0x52BA68 is a qword double (30.0) in .rdata; the port keeps the
    // float mirror g_FrameScale (exactly 30.0 until the config loader
    // rewrites it).
    const float seconds = static_cast<float>(f / g_FrameScale);

    float fpsF;
    std::memcpy(&fpsF, &s.AviRecordFps(), sizeof fpsF);
    const wchar_t* wavPath =
        (s.AviIncludeWave() != 0 && frameStart == 0 && s.Audio() != nullptr && s.WaveEnabled() != 0)
            ? s.Audio()->path
            : nullptr;
    DShowRecorder* recorder = s.Recorder();
    if (recorder == nullptr)
        return false;  // port-side guard: 0x466D20 allocation
    return BuildRecordingGraph(recorder,
                     static_cast<HWND>(s.state.hwnd),
                     static_cast<unsigned char>(s.EnglishUI() != 0),
                     s.AviOutputPath(), &config, fpsF, 1,
                     s.H264AverageBitrate(), wavPath,
                     seconds,
                     (wavPath != nullptr ? s.Audio() : nullptr),
                     s.EnglishUI());
}

}  // namespace

// VA 0x0045E820 - windowed AVI record start.
void StartAviRecordWindow(MMDApp* app) {
    auto& s = *app;
    if (!image_sequence::Active(app) && !ProbeWritable(
            s.AviOutputPath())) {                               // 0x45E849
        const HWND main = static_cast<HWND>(s.state.hwnd);
        MessageBoxA(main, s.EnglishUI() != 0 ? kMsgSaveFailEn
                                             : kMsgSaveFailJp,
                    s.EnglishUI() != 0 ? kCaptionAviOutEn : kCaptionAviOutJp,
                    0);                                         // 0x45E871
        return;
    }

    const std::int32_t outW = s.AviOutputWidth() > 0 ? s.AviOutputWidth() : s.RenderWidth();
    const std::int32_t outH = s.AviOutputHeight() > 0 ? s.AviOutputHeight() : s.RenderHeight();
    if (!GrowRenderTarget(app, outW, outH)) {
        MessageBoxW(s.MainWindow(), L"无法重置渲染设备，视频导出未开始。\n"
            L"Could not reset the render device; video export was not started.",
            L"AVI export", MB_OK | MB_ICONERROR);
        return;
    }
    const RECT rc = recording_preview::WindowRect(s.MainWindow(), outW, outH, true);
    char title[0x32];
    strcpy_s(title, 0x32, s.EnglishUI() != 0 ? kRecTitleEn : kRecTitleJp);
    HINSTANCE hInst = static_cast<HINSTANCE>(s.HInstance());  // this+0
    HWND recWnd = CreateWindowExA(                             // 0x45E9C6
        0, "RecWindow", title,
        recording_preview::kStyle,
        rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst,
        nullptr);
    s.RecordingWindow() = recWnd;                              // 0xA0D24
    if (recWnd == nullptr) {
        const HWND main = s.MainWindow();
        if (D3DRenderer* renderer = s.Renderer(); renderer != nullptr &&
            (renderer->screenWidth != s.state.recRTW ||
             renderer->screenHeight != s.state.recRTH)) {
            renderer->presentParameters.BackBufferWidth = s.state.recRTW;
            renderer->presentParameters.BackBufferHeight = s.state.recRTH;
            renderer->screenWidth = s.state.recRTW;
            renderer->screenHeight = s.state.recRTH;
            PostDeviceReset(app);
        }
        MessageBoxA(main, "CreateWindow failed",
                    s.EnglishUI() != 0 ? "create main window"
                                       : kCaptionRecWndJp,
                    0);                                         // 0x45EA14
        return;
    }
    if (image_sequence::Active(app))
        SetWindowTextW(recWnd, image_sequence::Text(app, L"正在导出图片序列…",
            L"Exporting image sequence…", L"連番画像を出力中…"));
    ShowWindow(recWnd, SW_SHOW);                               // 0x45EA38
    UpdateWindow(recWnd);

    s.AviSavedRenderWidth() = s.RenderWidth();
    s.AviSavedRenderHeight() = s.RenderHeight();
    s.AviRenderResolutionOverridden() = true;
    s.RenderWidth() = outW;
    s.RenderHeight() = outH;
    EnsureRecordRenderTarget(app);
    RefreshMainWindowViewport(app);                                            // 0x45EACC
    InvalidateRect(s.MainWindow(), &s.ViewportRect(), FALSE);   // 0x45EAE9

    if (!StartRecordGraph(app, outW, outH)) {
        FinishAviRecord(app);
        return;  // 0x45EC69: original leaves the window parked
    }
    if (s.state.pictureBackgroundEnabled != 0) {
        PicBgOverlayRefresh(app);                              // 0x45EBE0
    }
    HWND main = static_cast<HWND>(s.state.hwnd);
    EnableWindow(GetDlgItem(main, panel::kPlayButton), FALSE);             // 0x45EBFB
    ShowWindow(main, SW_HIDE);                                // 0x45EC0B
    ShowWindow(main, SW_HIDE);                                // 0x45EC18
    if (s.state.floatingWindow != 0) {
        ShowWindow(
            static_cast<HWND>(s.state.floatingWindow),
            SW_HIDE);                                          // 0x45EC25
    }
    RecordStartTail(app);
}

// VA 0x00464760 - fullscreen (3D Vision) AVI record start.
void StartAviRecordFullscreen(MMDApp* app) {
    auto& s = *app;
    const std::int32_t screenW = GetSystemMetrics(SM_CXSCREEN);
    const std::int32_t screenH = GetSystemMetrics(SM_CYSCREEN);
    const std::int32_t w = s.AviOutputWidth() > 0
        ? s.AviOutputWidth() : s.RenderWidth();
    const std::int32_t h = s.AviOutputHeight() > 0
        ? s.AviOutputHeight() : s.RenderHeight();
    const HWND main = static_cast<HWND>(s.state.hwnd);
    if ((h != screenH && screenH <= h) || (w != screenW && screenW <= w)) {
        MessageBoxA(main, s.EnglishUI() != 0
                              ? "output size must less than screen "
                                "resolution in 3D Vision."
                              : kMsg3dVisionJp,
                    s.EnglishUI() != 0 ? "over size" : "3D Vision",
                    0);                                         // 0x4647B9
        return;
    }

    s.FullscreenMode() = 1;
    ApplyFullscreenWindowState(app, true);                                            // 0x4629D0
    if (!PostDeviceReset(app)) {
        s.FullscreenMode() = 0;
        ApplyFullscreenWindowState(app);
        PostDeviceReset(app);
        MessageBoxW(main, L"无法重置渲染设备，视频导出未开始。\n"
            L"Could not reset the render device; video export was not started.",
            L"AVI export", MB_OK | MB_ICONERROR);
        return;
    }
    s.AviSavedRenderWidth() = s.RenderWidth();
    s.AviSavedRenderHeight() = s.RenderHeight();
    s.AviRenderResolutionOverridden() = true;
    s.RenderWidth() = w;
    s.RenderHeight() = h;
    s.RecordingWindow() = main;                                // 0xA0D24

    EnsureRecordRenderTarget(app);
    RefreshMainWindowViewport(app);                                            // 0x464889

    const std::int32_t scale = s.AviStereoWidthMultiplier();
    const std::int32_t scaledW = w * scale;
    if (!StartRecordGraph(app, scaledW, h)) {
        FinishAviRecord(app);
        return;
    }
    // 0x46497A: probe open/close without checking the result, then the
    // picture overlay and the shared tail.
    ProbeWritable(
        s.AviOutputPath());
    if (s.state.pictureBackgroundEnabled != 0) {
        PicBgOverlayRefresh(app);                              // 0x4649A7
    }
    RecordStartTail(app);
}

// VA 0x004629D0 - fullscreen enter/restore window manager (app method).
// Enter (app+0xA0274 != 0): remembers the window placement/menu, strips the
// frame, sizes to the screen, hides the control band and mirrors the client
// size into the present parameters.  Restore: reapplies the saved style,
// placement and menu, shows the control band and restores the RT dimensions.
void ApplyFullscreenWindowState(MMDApp* app, bool exclusive) {
    struct Snapshot {
        HWND window = nullptr;
        LONG_PTR style = 0;
        LONG_PTR exStyle = 0;
        int sidebar = 0;
        D3DPRESENT_PARAMETERS presentation{};
        std::vector<HWND> visibleChildren;
    };
    static Snapshot saved;
    auto& s = *app;
    const HWND main = static_cast<HWND>(s.state.hwnd);
    if (main == nullptr)
        return;
    const WindowDpiScope dpi(main);

    if (s.FullscreenMode() != 0) {
        if (saved.window == main)
            return;
        saved = Snapshot{};
        saved.window = main;
        saved.style = GetWindowLongPtrW(main, GWL_STYLE);
        saved.exStyle = GetWindowLongPtrW(main, GWL_EXSTYLE);
        saved.sidebar = s.FloatingWindow() ? s.SeparateWindowSidebarWidth() : s.SidebarWidth();
        if (D3DRenderer* r = s.Renderer())
            saved.presentation = r->presentParameters;
        s.SavedPlacement().length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(main, &s.SavedPlacement());
        s.state.savedMenu = GetMenu(main);
        if (s.FloatingWindow() != nullptr) {
            SaveFlagSubsystem(app);                              // 0x461FA0
            s.state.fullscreenFlagsSaved = 1;
        }
        // Include the viewport toolbar and plugin controls, and remember
        // which controls were hidden before entry. IDs 400..531 were incomplete.
        for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0) {
                saved.visibleChildren.push_back(child);
                ShowWindow(child, SW_HIDE);
            }
        }
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR targetMonitor = MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST);
        const bool hasMonitor = targetMonitor != nullptr &&
            GetMonitorInfoW(targetMonitor, &monitor) != FALSE;
        const RECT bounds = hasMonitor
            ? monitor.rcMonitor
            : RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                         GetSystemMetrics(SM_CYSCREEN)};
        SetWindowLongPtrW(main, GWL_STYLE,
            WS_POPUP | WS_CLIPCHILDREN | (saved.style & WS_VISIBLE));
        SetMenu(main, nullptr);
        SetWindowPos(main, HWND_NOTOPMOST,
                     bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if ((saved.style & WS_VISIBLE) != 0)
            SetFocus(main);
        ValidateRect(main, nullptr);
        RECT rc;
        GetClientRect(main, &rc);                                // 0x462AD0
        if (D3DRenderer* r = s.Renderer()) {
            r->presentParameters.BackBufferWidth = rc.right;
            r->presentParameters.BackBufferHeight = rc.bottom;
            // Interactive fullscreen must retain Windows DPI virtualization.
            // Logical client sizes are not valid exclusive display modes.
            // Only the existing stereo recording path requests exclusivity.
            r->presentParameters.Windowed = exclusive ? FALSE : TRUE;
            r->presentParameters.FullScreen_RefreshRateInHz = 0;
        }
        s.state.recordFullscreenActive = exclusive ? 1 : 0;
        return;
    }

    if (saved.window != main)
        return;
    s.SidebarWidth() = saved.sidebar;
    if (D3DRenderer* r = s.Renderer())
        r->presentParameters = saved.presentation;
    SetWindowLongPtrW(main, GWL_STYLE, saved.style);
    SetWindowLongPtrW(main, GWL_EXSTYLE, saved.exStyle);
    SetMenu(main, s.state.savedMenu);
    WINDOWPLACEMENT placement = s.SavedPlacement();
    if ((saved.style & WS_VISIBLE) == 0) placement.showCmd = SW_HIDE;
    SetWindowPlacement(main, &placement);
    SetWindowPos(main, nullptr,
                 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    for (HWND child : saved.visibleChildren) {
        if (IsWindow(child)) ShowWindow(child, SW_SHOWNA);
    }
    saved = Snapshot{};
    if (s.state.fullscreenFlagsSaved != 0) {
        s.state.fullscreenFlagsSaved = 0;
        InitFlagSubsystem(app);
    }
    HandleWindowSize(app);
    if (s.PlaybackActive() == 0) {
        RestorePlaybackMenus(app);                               // 0x4298E0
    } else {
        DisablePlaybackMenus(main);                              // 0x429790
    }
    InvalidateRect(main, nullptr, FALSE);
    s.state.stereoActivated = 0;
    s.state.recordFullscreenActive = 0;
}

}  // namespace mikumikustudio
