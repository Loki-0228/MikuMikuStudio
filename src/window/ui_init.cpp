#include "mikumikustudio/key_pool.hpp"
// ===========================================================================
// VA 0x00466D20 - CreateUIControls  (original: sub_466D20, 0x436B bytes)
// ===========================================================================
// Called from WM_CREATE in the main WndProc (0x004C3A10); returning false
// terminates the process (original: `if (!sub_466D20(hWnd)) exit(1);`).
//
// PHASE A (this file): drag-and-drop acceptance + the full 168-control
// creation sequence (table generated from the raw decompilation by
// scripts/gen_ui_controls.py - ids, classes, texts, styles, geometry and
// sidebar-relative positions are the original's, in original order).
//
// Remaining phases of the original, now ported:
//   - HUD sprite sheet (the original's "splite.tga", shipped as
//     res/assets/hud_sprites.png) + accessory-helper texture: loaded below
//     via LoadPngTexture (0x467509 / 0x46759E).
//   - InitToonTextures (0x00424DC0) - src/render/toon_textures.cpp
//   - InitSceneFontTexture (0x0042AE80) - src/render/scene_font.cpp
//
// All combo boxes use Unicode so imported names and translated entries do
// not pass through the Windows ANSI code page. Other controls retain their
// original ANSI/wide creation variants.
// ===========================================================================
#include "mikumikustudio/animation_curve_host.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <shellapi.h>   // DragAcceptFiles
#include <objbase.h>    // CoInitialize (0x408EF0 DirectShowInit)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <cstdio>
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/model.hpp"
#include "../render/viewport_cursor_style.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "ui_controls.inc"
#include "transform_edit_layout.hpp"

namespace mikumikustudio {

static int EvalPos(const ui::PosExpr& e, int sidebar) {
    switch (e.kind) {
        case 1:  return sidebar + e.off;        // sidebar + off
        case 2:  return sidebar / 2 + e.off;    // sidebar/2 + off
        default: return e.off;                  // fixed literal
    }
}

// Shared with the hidden-window language regression so it exercises the
// production control creation path without initializing a render device.
HWND CreateUiPanelControl(const ui::ControlSpec& c, HWND hwnd,
                          HINSTANCE hInst, int sidebar) {
    // Legacy combo captions are empty; their contents are populated later.
    // Even SendMessageW loses names if the receiving combo is an ANSI window.
    const bool ansiCombo = c.acls && std::strcmp(c.acls, "COMBOBOX") == 0;
    const DWORD style=ui::TransformEditShowsPrefix(c.id)
        ? c.style & ~(ES_CENTER|ES_RIGHT) : c.style;
    return (c.wide || ansiCombo)
        ? CreateWindowExW(c.ex, ansiCombo ? L"COMBOBOX" : c.wcls,
                          ansiCombo ? L"" : c.wtext, style,
                          EvalPos(c.x, sidebar), EvalPos(c.y, sidebar),
                          c.w, c.h, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)),
                          hInst, nullptr)
        : CreateWindowExA(c.ex, c.acls, c.atext, style,
                          EvalPos(c.x, sidebar), EvalPos(c.y, sidebar),
                          c.w, c.h, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)),
                          hInst, nullptr);
}

// j_??2@YAPAXI@Z - the VC9 throwing operator new(uint) used for the track
// key arrays, followed by the original's immediate memset-zero.
static unsigned char* NewZeroed(std::size_t bytes) {
    unsigned char* p = static_cast<unsigned char*>(::operator new(bytes));
    std::memset(p, 0, bytes);
    return p;
}

// Porting-era trace under MIKUMIKUSTUDIO_PMM_TRACE_DIR (CMake option
// MIKUMIKUSTUDIO_DIAG, default OFF); the OFF stub keeps the call site
// valid and inlines away to nothing.
#ifdef MIKUMIKUSTUDIO_DIAG
static void TraceInitialAccessoryTrack(const MMDApp& app) {
    const char* directory = std::getenv("MIKUMIKUSTUDIO_PMM_TRACE_DIR");
    if (directory == nullptr || directory[0] == '\0')
        return;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\pmm_model_load.log", directory);
    FILE* stream = nullptr;
    if (fopen_s(&stream, path, "ab") != 0 || stream == nullptr)
        return;
    fprintf(stream,
            "stage=ui-initial-accessory-track-174 image=%p track=%p\r\n",
            GetModuleHandleW(nullptr), app.AccessoryKeyTracks()[174]);
    fclose(stream);
}
#else
static inline void TraceInitialAccessoryTrack(const MMDApp&) {}
#endif

// VA 0x0040AE00 - was Sub40AE00.  Blank the timeline strip and the
// interpolation-curve box: white rectangles over both GDI surfaces (the
// timeline spans the backbuffer width at 49 px, the curve is 127x127) plus
// the black centre line at timeline y=25.
void ClearTimelineAndCurveDCs(MMDApp* app) {
    HDC timeline = app->TimelineDC();
    HDC curve = app->CurveDC();
    D3DRenderer* r = app->Renderer();
    const int backbufferWidth = r->screenWidth;

    HPEN whitePen = CreatePen(PS_SOLID, 1, 0xFFFFFF);
    HBRUSH whiteBrush = CreateSolidBrush(0xFFFFFF);
    HGDIOBJ oldPen = SelectObject(timeline, whitePen);
    HGDIOBJ oldBrush = SelectObject(timeline, whiteBrush);
    SelectObject(curve, whitePen);
    SelectObject(curve, whiteBrush);
    Rectangle(timeline, 0, 0, backbufferWidth, 49);
    Rectangle(curve, 0, 0, 127, 127);
    SelectObject(timeline, oldPen);
    SelectObject(curve, oldPen);
    DeleteObject(whitePen);
    SelectObject(timeline, oldBrush);
    SelectObject(curve, oldBrush);
    DeleteObject(whiteBrush);

    HPEN blackPen = CreatePen(PS_SOLID, 1, 0);
    SelectObject(timeline, blackPen);
    MoveToEx(timeline, 0, 25, nullptr);
    LineTo(timeline, backbufferWidth, 25);
    SelectObject(timeline, oldPen);
    DeleteObject(blackPen);
}

static bool LoadPngTexture(MMDApp* app, IDirect3DDevice9* device,
                           WORD resourceId, IDirect3DTexture9** output) {
    HMODULE module = static_cast<HMODULE>(app->HInstance());
    HRSRC info = FindResourceA(module, MAKEINTRESOURCEA(resourceId), "PNG");
    if (info == nullptr)
        return false;
    HGLOBAL resource = LoadResource(module, info);
    const void* bytes = resource != nullptr ? LockResource(resource) : nullptr;
    const DWORD size = SizeofResource(module, info);
    auto& api = d3dx::Get();
    if (bytes == nullptr || size == 0 || !api.Load() || api.fromMemEx == nullptr)
        return false;
    return SUCCEEDED(api.fromMemEx(
        device, bytes, size, static_cast<UINT>(-1), static_cast<UINT>(-1),
        1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
        static_cast<DWORD>(-1), static_cast<DWORD>(-1), 0,
        nullptr, nullptr, output));
}

void CmdControl500(MMDApp* app, HWND hwnd, std::uint16_t id, std::uint16_t notify);

bool CreateUIControls(MMDApp* app, HWND hwnd) {
    auto& s = *app;

    // D3D bootstrap first (original call site 0x00466D5B, top of UI init).
    if (!InitD3D(app, hwnd, app->EnglishUI() != 0,
                 static_cast<HMODULE>(app->HInstance())))
        return false;

    // Physics scene construction (original call site 0x00466D78, directly
    // after the D3D bootstrap): gizmo buffers + the Bullet world and its
    // static ground body inside the Sub048 wrapper.
    if (!SceneConstruct(app->Physics(), app->Renderer()))
        return false;

    // Phase 1 of the original: enable drag-and-drop of model/scene files.
    DragAcceptFiles(hwnd, TRUE);

    // ---- 0x466D8A..0x466E69: off-screen GDI surfaces ------------------------
    // Four DCs track the D3D back-buffer size (wrapper at this+0xA06C4,
    // dims +0x1D4E4/+0x1D4E8): the horizontal frame ruler strip (bbW x 50,
    // this+0x2E0), the main panel double buffer (bbW x bbH, this+0x2D4, with
    // a spare bitmap at this+0x2D8), the 128x128 interpolation-curve cache
    // (this+0x2E8) and a temporary scratch DC the pre-draw fills below use
    // to paint the spare bitmap.  WM_PAINT blits these DCs
    // (wm_paint.cpp:396) - without them the timeline area stays black.
    HDC hdc = GetDC(hwnd);
    D3DRenderer* wrap = app->Renderer();
    const int bbW = wrap->screenWidth;
    const int bbH = wrap->screenHeight;

    s.TimelineDC() = CreateCompatibleDC(hdc);                          // 0x466D97
    HBITMAP ruler = CreateCompatibleBitmap(hdc, bbW, 0x32);            // 0x466DB3
    s.TimelineBitmap() = ruler;
    SelectObject(s.TimelineDC(), ruler);

    s.PanelDC() = CreateCompatibleDC(hdc);                            // 0x466DD0
    HBITMAP panel = CreateCompatibleBitmap(hdc, bbW, bbH);             // 0x466DF1
    s.PanelBitmap() = panel;
    HBITMAP spare = CreateCompatibleBitmap(hdc, bbW, bbH);             // 0x466E12
    s.PanelSpareBitmap() = spare;
    SelectObject(s.PanelDC(), panel);

    s.CurveDC() = CreateCompatibleDC(hdc);                             // 0x466E2F
    HBITMAP curve = CreateCompatibleBitmap(hdc, 0x80, 0x80);           // 0x466E46
    s.CurveBitmap() = curve;
    SelectObject(s.CurveDC(), curve);

    HDC scratch = CreateCompatibleDC(hdc);                             // 0x466E5D
    SelectObject(scratch, spare);

    // ---- 0x466E7A..0x467023: pre-draw fills on the spare bitmap -------------
    // Pen+brush of the same COLORREF, Rectangle, restore, delete - three
    // passes with the color.txt palette fields.
    auto fill = [&](COLORREF color, void (*rects)(HDC, int, int)) {
        HPEN pen = CreatePen(PS_SOLID, 1, color);                      // 0x466E7A
        HBRUSH brush = CreateSolidBrush(color);                        // 0x466E8B
        HGDIOBJ oldPen = SelectObject(scratch, pen);                   // 0x466E97
        HGDIOBJ oldBrush = SelectObject(scratch, brush);               // 0x466EA3
        rects(scratch, bbW, bbH);
        SelectObject(scratch, oldBrush);                               // 0x466ECE
        SelectObject(scratch, oldPen);                                 // 0x466ED6
        DeleteObject(pen);                                             // 0x466EE1
        DeleteObject(brush);                                           // 0x466EEC
    };
    fill(s.ThemeColor(UiThemeColor::TimelineBase),                     // base fill
         [](HDC dc, int w, int h) { Rectangle(dc, 0, 0, w, h); });
    fill(s.ThemeColor(UiThemeColor::HeaderBand),                       // header bands
         [](HDC dc, int w, int h) {
             Rectangle(dc, 1, 0xF, 0x5A, h);      // 0x466F3C
             Rectangle(dc, 0x5A, 1, w, 0xF);      // 0x466F56
         });
    fill(s.ThemeColor(UiThemeColor::RowBand),                          // row bands
         [](HDC dc, int w, int h) {
             for (int i = 0xF; i + 0xF < h; i += 0x1C)                // 0x466FE4
                 Rectangle(dc, 1, i, 0x5A, i + 0xF);
         });

    // ---- 0x467023..0x46725F: remaining palette fills and grid lines -------
    fill(ColorLerp(s.ThemeColor(UiThemeColor::ControlLight),
                   s.ThemeColor(UiThemeColor::ControlDark),
                   0.08281938f),
         [](HDC dc, int, int) { Rectangle(dc, 0, 0, 0x59, 0xF); });
    fill(s.ThemeColor(UiThemeColor::TimelineRows),
         [](HDC dc, int w, int h) {
             for (int y = 0xF; y < h; y += 0x1C)
                 Rectangle(dc, 0x5B, y, w, y + 0xF);
         });

    auto drawRows = [&](COLORREF color, int x0, int x1) {
        HPEN rowPen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ previous = SelectObject(scratch, rowPen);
        for (int y = 0xF; y < bbH; y += 0xE) {
            MoveToEx(scratch, x0, y, nullptr);
            LineTo(scratch, x1, y);
        }
        SelectObject(scratch, previous);
        DeleteObject(rowPen);
    };
    drawRows(s.ThemeColor(UiThemeColor::LabelGrid), 1, 0x5A);
    drawRows(s.ThemeColor(UiThemeColor::TimelineGrid), 0x5B, bbW);

    // The original releases the window DC and destroys the temporary DC,
    // then copies the completed spare bitmap into the persistent panel DC.
    ReleaseDC(hwnd, hdc);                                             // 0x46726B
    DeleteDC(scratch);                                                // 0x467272
    HDC copyDc = CreateCompatibleDC(nullptr);                          // 0x46727A
    SelectObject(copyDc, spare);
    BitBlt(s.PanelDC(), 0, 0, bbW, bbH,
           copyDc, 0, 0, SRCCOPY);                                    // 0x4672B5
    DeleteDC(copyDc);

    // A new project has no audio. Bind the waveform drawing context without
    // opening a sound device; LoadWaveFile handles imports and PMM audio.
    ClearTimelineAndCurveDCs(app);
    s.Audio()->englishUI = app->EnglishUI() != 0 ? 1 : 0;
    s.Audio()->mainWindow = hwnd;
    s.Audio()->timelineDC = s.TimelineDC();

    SetCurrentDirectoryW(app->ExeDir());                              // 0x4672F1
    s.state.bmpRes101 = LoadBitmapA(
        static_cast<HMODULE>(s.HInstance()), MAKEINTRESOURCEA(0x65)); // 0x467303
    s.state.bmpRes119 = LoadBitmapA(
        static_cast<HMODULE>(s.HInstance()), MAKEINTRESOURCEA(0x77)); // 0x467311

    // 0x467320: DirectShowInit (sub_408EF0, this = the 0x6C AVI-codec
    // subsystem object at app+0xA06C0 which only supplies the failure
    // MessageBox parent).  Failure terminates CreateUIControls.
    if (CoInitialize(nullptr) < 0) {
        LegacyUiMessageBoxA(hwnd, "Failed CoInitialize!", "DirectShowInit", 0);
        return false;
    }

    // 0x46732D..0x467336: the recording step-flag byte.  operator new(1)
    // with NO initialization in the original; the wait loops at 0x46E8F5 /
    // 0x46F08C and FinishAviRecord:0x464A28 all dereference app+0x9EDD4
    // unconditionally, so this allocation is what keeps those paths alive.
    // The fresh-heap byte reads as 0 in practice; we zero it explicitly so
    // the port stays deterministic.
    s.RecordingCompletionFlag() = new unsigned char(0);

    // 0x46733C..0x467375: material used for projected accessory shadows.
    // The original memsets the whole D3DMATERIAL9 at 0xA0CE0 and then
    // seeds a half-transparent diffuse term, white ambient term, and a
    // specular alpha; only those promoted fields are ever read back
    // (ProjectedShadowMaterial() rebuilds the struct from them), so the
    // setters alone are observably identical to the memset + setters.
    s.SetProjectedShadowAmbient(1.0f);
    s.ProjectedShadowDiffuseAlpha() = 0.5f;
    s.ProjectedShadowSpecularAlpha() = 1.0f;

    // 0x46737B: colored world axes and 5-unit ground grid.
    if (!MakeLineGeometry(app))
        return false;

    // 0x467384..0x4674A6: dynamic text/sprite/line batches plus the two
    // post-process quads. RefreshMainWindowViewport rewrites the latter after every resize.
    IDirect3DDevice9* device = wrap->device;
    if (device != nullptr) {
        if (FAILED(device->CreateVertexBuffer(
                0x6D60, D3DUSAGE_WRITEONLY, 0x144, D3DPOOL_MANAGED,
                &s.OverlayVertices(),
                nullptr)))
            return false;
        device->CreateVertexBuffer(6 * 28, D3DUSAGE_WRITEONLY, 0x144,
                                   D3DPOOL_MANAGED,
                                   &s.LeftViewportVertices(),
                                   nullptr);
        device->CreateVertexBuffer(6 * 28, D3DUSAGE_WRITEONLY, 0x144,
                                   D3DPOOL_MANAGED,
                                   &s.RightViewportVertices(),
                                   nullptr);
        // PMM picture/AVI background refreshes write one 6-vertex textured
        // quad per layer.  These are distinct scene resources (and are
        // released independently at shutdown), not aliases of the viewport
        // post-process quads.
        device->CreateVertexBuffer(0xA8, D3DUSAGE_WRITEONLY, 0x144,
                                   D3DPOOL_MANAGED,
                                   &s.AviOverlayVertices(), nullptr);
        device->CreateVertexBuffer(0xA8, D3DUSAGE_WRITEONLY, 0x144,
                                   D3DPOOL_MANAGED,
                                   &s.PictureOverlayVertices(), nullptr);
        device->CreateVertexBuffer(
            0x445C0, D3DUSAGE_WRITEONLY, 0x144, D3DPOOL_MANAGED,
            &s.SpriteOverlayVertices(), nullptr);
        device->CreateVertexBuffer(
            0x30D40, D3DUSAGE_WRITEONLY, 0x44, D3DPOOL_MANAGED,
            &wrap->lineVertexBuffer,
            nullptr);

        // 0x467509 and 0x46759E: the HUD sprite sheet and accessory helper
        // texture are PNG resources 102 and 114 in the original image.
        // PNG 102 load failure gets the original's error box (x64
        // 0x7FF7CB4328EE, caption "InitFont"): EN "cannot load splite.tga"
        // / JP "splite.tga読込失敗" (0x7FF7CB54B4C0/B4D9).  The original
        // asset was named splite.tga; the port ships the same sheet as
        // res/assets/hud_sprites.png, so the message names that file.
        // Non-fatal: the original shows the box and falls through to
        // PNG 114 (only 102 has a failure box).
        if (!LoadPngTexture(app, device, 0x66, &s.OverlayTexture())) {
            static const char kJpHudSheetLoadFailed[] =
                "hud_sprites.png\x93\xC7\x8D\x9E\x8E\xB8\x94\x73";
            LegacyUiMessageBoxA(hwnd,
                        app->EnglishUI() != 0
                            ? "cannot load hud_sprites.png"
                            : kJpHudSheetLoadFailed,
                        "InitFont", MB_OK);
        }
        // Repaint the sheet's viewport rotate/move cursor cells (the whole
        // right half) into the left half's icon language: solid fill, a
        // one-pixel black stroke and a light outer edge, with the drag hand
        // dropped and the arrow heads flattened. Left-half texels and the
        // shipped PNG are untouched; the lock happens once, at init.
        RestyleViewportCursorAtlas(s.OverlayTexture());
        LoadPngTexture(app, device, 0x72,
            &s.ProjectedShadowRestoreTexture());
    }

    // ---- 0x467637..0x467674: default-checked menu items -------------------
    HMENU menu = GetMenu(hwnd);
    CheckMenuItem(menu, 0xD7, MF_CHECKED);
    CheckMenuItem(menu, 0xDD, MF_CHECKED);
    CheckMenuItem(menu, 0xEC, MF_CHECKED);
    CheckMenuItem(menu, 0xF3, MF_CHECKED);
    CheckMenuItem(menu, 0xFE, MF_CHECKED);

    // ---- 0x46767B..0x467747: the four global track key arrays -------------
    // 10000-key arrays consumed by PlaybackPoseAdvance (0x4175A0): camera
    // 84B rec, light 40B rec, self-shadow 24B rec, physics-gravity 36B rec,
    // plus 255 accessory slots of 10000 x 60B records (the 256th consumer
    // slot at app+0x780 is deliberately left unallocated by the original -
    // it reads the first model-slot pointer there, gated off by its zero
    // active flag in a fresh app).
    unsigned char* cam = reinterpret_cast<unsigned char*>(mdl::KeyPoolCreate<mdl::CameraKey>());                    // 840000
    s.state.cameraKeyTrack = cam;                               // 0x374
    unsigned char* trkLight = reinterpret_cast<unsigned char*>(mdl::KeyPoolCreate<mdl::LightKey>());               // 400000
    s.state.lightKeyTrack = trkLight;                           // 0x378
    unsigned char* trkShadow = reinterpret_cast<unsigned char*>(mdl::KeyPoolCreate<mdl::SelfShadowKey>());              // 240000
    s.state.selfShadowKeyTrack = trkShadow;                     // 0x37C
    unsigned char* trkPhys = reinterpret_cast<unsigned char*>(mdl::KeyPoolCreate<mdl::GravityKey>());                // 360000
    s.state.gravityKeyTrack = trkPhys;                          // 0x380
    for (int i = 0; i < 255; ++i) {                             // 0x467705
        auto* acc = reinterpret_cast<mdl::AccessoryKey*>(
            reinterpret_cast<unsigned char*>(mdl::KeyPoolCreate<mdl::AccessoryKey>()));
        s.AccessoryKeys(i) = acc;
        acc[0].visible = 1;
        acc[0].parentModel = -1;
        acc[0].scale = 1.0f;
        acc[0].opacity = 1.0f;
        s.ObjectSlot(i) = nullptr;                              // 0x9DD70[i]
    }
    TraceInitialAccessoryTrack(s);
#ifdef MIKUMIKUSTUDIO_DIAG
    if (std::getenv("MIKUMIKUSTUDIO_PMM_GUARD_ACCESSORY_TRACKS") != nullptr) {
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
            &s.AccessoryKeyTracks()[174]);
        const std::uintptr_t page = address & ~(
            static_cast<std::uintptr_t>(systemInfo.dwPageSize) - 1);
        DWORD previousProtection = 0;
        VirtualProtect(reinterpret_cast<void*>(page), systemInfo.dwPageSize,
                       PAGE_READONLY, &previousProtection);
    }
#endif

    // ---- 0x467749..0x46787B: camera work record + per-key defaults --------
    auto* cameraKeys = s.CameraKeys();
    cameraKeys[0].distance = -45.0f;
    cameraKeys[0].eye[1] = 10.0f;
    cameraKeys[0].fov = 30;
    for (std::size_t k = 0; k < mdl::kTimelineKeyCapacity; ++k) {
        auto& key = cameraKeys[k];
        key.parentModel = -1;
        for (int c = 0; c < 6; ++c) {
            key.interpolation[0][c] = key.interpolation[1][c] = 20;
            key.interpolation[2][c] = key.interpolation[3][c] = 107;
        }
    }

    // ---- 0x46787E..0x467967: light cluster + device light 0 ---------------
    s.CameraFov() = 30.0f;
    float* direction = s.LightDirection();
    direction[0] = -0.5f;
    direction[1] = -1.0f;
    direction[2] = 0.5f;
    D3DLIGHT9& light = s.SceneLight();
    std::memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    for (int j = 0; j < 3; ++j) {
        // Match New scene: the live values feed both the lighting panel and
        // subsequent rendering, independently of the device light/key head.
        s.LightColor()[j] = 0.602f;
        reinterpret_cast<float*>(&light.Specular)[j] = 0.602f;
        reinterpret_cast<float*>(&light.Ambient)[j] = 0.602f;
    }
    std::memcpy(&light.Direction, direction, sizeof(light.Direction));
    D3DRenderer* w = app->Renderer();
    IDirect3DDevice9* dev = w->device;                          // +0x1D4E0
    dev->SetLight(0, &light);
    dev->LightEnable(0, TRUE);                       // vtable 0xD4

    // ---- 0x467984..0x467A2E: track work records ---------------------------
    auto& lightHead = s.LightKeys()[0];
    for (int c = 0; c < 3; ++c) {
        lightHead.color[c] = 0.602f;
        lightHead.direction[c] = direction[c];
    }
    auto& shadowHead = s.ShadowKeys()[0];
    shadowHead.mode = w->postProcessEnabled != 0 ? 1 : 0;
    shadowHead.distance = 0.01125f;
    auto& gravityHead = s.GravityKeys()[0];
    gravityHead.noise = 10;
    gravityHead.acceleration = 9.8000002f;
    gravityHead.direction[1] = -1.0f;

    const int sidebar = app->SidebarWidth();                            // 657096
    HINSTANCE hInst = static_cast<HINSTANCE>(app->HInstance());          // this+0

    // The font object is created before the first UI child in the original.
    // Every child then receives its font/range/subclass messages immediately
    // at its own creation site.
    CreateUiFont(app, hwnd);
    const auto initializeTrackRange = [](HWND control, int id) {
        int lo = 0, hi = 0;
        switch (id) {
        case 447: lo = 1; hi = 125; break;
        case 455: case 456: case 457: lo = 0; hi = 255; break;
        case 458: case 459: case 460: lo = -100; hi = 100; break;
        case 505: case 510: case 515: case 520: case 534:
            lo = 0; hi = 100; break;
        case 560: lo = 0; hi = 9999; break;
        default: return;
        }
        SendMessageA(control, TBM_SETRANGEMIN, 0, lo);
        SendMessageA(control, TBM_SETRANGEMAX, 0, hi);

        // The tick frequency is part of the original creation sequence, not
        // cosmetic post-processing.  Without it, comctl32 uses its default
        // one-unit interval; the 201/10000 densely packed tick marks then
        // merge into two horizontal rules.  Original sites:
        //   Light X/Y/Z  0x468B34 / 0x468BF9 / 0x468CBE
        //   Shadow range 0x46A2F2
        if (id == 458 || id == 459 || id == 460 || id == 560)
            SendMessageA(control, TBM_SETTICFREQ, 1000, 0);
    };

    for (const ui::ControlSpec& c : ui::kControls) {
        // Added bone-scale readouts have no counterpart in MMD 9.32.
        if (MMD_ORIGINAL_PROFILE && c.id >= 570 && c.id <= 577) continue;
        HWND control = CreateUiPanelControl(c, hwnd, hInst, sidebar);
        if (control == nullptr)
            return false;   // original exits the process from WM_CREATE
        ApplyUiFontToControl(app, control, c.id);
        initializeTrackRange(control, c.id);
        InstallControlSubclass(app, control, c.id);
        InstallPanelCheckboxPaint(control);
    }

    SetWindowTextA(GetDlgItem(hwnd,panel::kPlayStopFrameEdit),"-1");
    curve_host::CreateControls(app, hwnd);
    // 0x46A8ED..0x46A8F7: the current-frame edit is initialized only
    // after its numeric-edit subclass has been installed.  Leaving the
    // CreateWindow caption empty made the top overlay differ by one glyph.
    SetWindowTextA(GetDlgItem(hwnd, panel::kGotoFrameEdit), "0");

#if !MMD_ORIGINAL_PROFILE
    HWND forcePaste = CreateWindowExW(0, L"BUTTON", L"強制貼付",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 72, 18, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kForcePasteButton)), hInst, nullptr);
    if (!forcePaste) return false;
    ApplyUiFontToControl(app, forcePaste, panel::kForcePasteButton);
    InstallControlSubclass(app, forcePaste, panel::kForcePasteButton);
    SetWindowPos(forcePaste, GetDlgItem(hwnd, 423), 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    HWND adaptiveShadow = CreateWindowExW(0, L"BUTTON", L"動的精度",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 84, 18, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kAdaptiveShadowCheckbox)), hInst, nullptr);
    if (!adaptiveShadow) return false;
    LoadShadowResolutionPreference(app);
    SendMessageW(adaptiveShadow, BM_SETCHECK, app->AdaptiveShadowResolution() ? BST_CHECKED : BST_UNCHECKED, 0);
    ApplyUiFontToControl(app, adaptiveShadow, panel::kAdaptiveShadowCheckbox);
    InstallControlSubclass(app, adaptiveShadow, panel::kAdaptiveShadowCheckbox);
    InstallPanelCheckboxPaint(adaptiveShadow);
    HWND shadowFilter = CreateWindowExW(0, L"BUTTON", L"平滑化",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 84, 18, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kShadowFilterCheckbox)), hInst, nullptr);
    if (!shadowFilter) return false;
    SendMessageW(shadowFilter, BM_SETCHECK, app->ShadowFilterEnhanced() ? BST_CHECKED : BST_UNCHECKED, 0);
    D3DCAPS9 shadowCaps{};
    EnableWindow(shadowFilter, app->Renderer()->device &&
        SUCCEEDED(app->Renderer()->device->GetDeviceCaps(&shadowCaps)) &&
        shadowCaps.PixelShaderVersion >= D3DPS_VERSION(3, 0));
    ApplyUiFontToControl(app, shadowFilter, panel::kShadowFilterCheckbox);
    InstallControlSubclass(app, shadowFilter, panel::kShadowFilterCheckbox);
    InstallPanelCheckboxPaint(shadowFilter);
#endif

    // UI-init tail (original 0x004675FB / 0x00467602, adjacent calls):
    // toon gradient textures, then the in-scene HUD font atlas.  Both are
    // device-guarded no-ops if the device could not be created.
    InitToonTextures(app);
    InitSceneFontTexture(app);
    if (!InitAxisMesh(app))
        return false;

    // ---- 0x46A99F..0x46AC5E: edit/trackbar mirrors of the work records ---
    char text[0x100];
    auto setEdit = [&](int id, const char* t) {
        SendMessageA(GetDlgItem(hwnd, id), EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(t));
    };
    auto setTrack = [&](int id, int pos) {
        SendMessageA(GetDlgItem(hwnd, id), TBM_SETPOS, 1, pos);
    };
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(static_cast<double>(s.LightColor()[0]) *
                               256.0));
    setEdit(461, text);
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(static_cast<double>(s.LightColor()[1]) *
                               256.0));
    setEdit(462, text);
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(static_cast<double>(s.LightColor()[2]) *
                               256.0));
    setEdit(463, text);
    sprintf_s(text, 0x100, "%+3.1f", direction[0]);
    setEdit(464, text);
    sprintf_s(text, 0x100, "%+3.1f", direction[1]);
    setEdit(465, text);
    sprintf_s(text, 0x100, "%+3.1f", direction[2]);
    setEdit(466, text);
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(s.CameraFov()));
    setEdit(448, text);
    setTrack(447, static_cast<int>(s.CameraFov()));
    setEdit(417, "0");
    setTrack(455, static_cast<int>(static_cast<double>(s.LightColor()[0]) *
                                  256.0));
    setTrack(456, static_cast<int>(static_cast<double>(s.LightColor()[1]) *
                                  256.0));
    setTrack(457, static_cast<int>(static_cast<double>(s.LightColor()[2]) *
                                  256.0));
    setTrack(458, static_cast<int>(static_cast<double>(direction[0]) *
                                  100.0));
    setTrack(459, static_cast<int>(static_cast<double>(direction[1]) *
                                  100.0));
    setTrack(460, static_cast<int>(static_cast<double>(direction[2]) *
                                  100.0));

    // ---- 0x46AC60..0x46AF22: initial command availability/state ----------
    // MF_GRAYED is passed by command ID in the original.  These commands
    // become available later when a model, accessory, motion or media file
    // establishes the corresponding editing context.
    static constexpr UINT kInitiallyDisabled[] = {
        0xFD, 0xD9, 0xDC, 0xDA, 0xCA, 0xCB, 0xDB, 0xDE,
        0xF9, 0xFA, 0xFB, 0xFC, 0x120, 0x121, 0x124,
    };
    for (UINT id : kInitiallyDisabled)
        EnableMenuItem(menu, id, MF_BYCOMMAND | MF_GRAYED);
    for (UINT id = 0xE0; id <= 0xE7; ++id)
        EnableMenuItem(menu, id, MF_BYCOMMAND | MF_GRAYED);
    for (UINT id = 0x111; id <= 0x113; ++id)
        EnableMenuItem(menu, id, MF_BYCOMMAND | MF_GRAYED);

    if (w->multisampleAvailable == 0)
        EnableMenuItem(menu, 0x115, MF_BYCOMMAND | MF_GRAYED);
    else
        CheckMenuItem(menu, 0x115, MF_BYCOMMAND | MF_CHECKED);
    antialias::Initialize(app);

    if (w->postProcessEnabled == 0) {
        EnableMenuItem(menu, 0x117, MF_BYCOMMAND | MF_GRAYED);
        s.SelfShadowMode() = 0;
        s.state.selfShadowEnabled = 0;
    } else {
        CheckMenuItem(menu, 0x117, MF_BYCOMMAND | MF_CHECKED);
        s.SelfShadowMode() = 1;
        s.state.selfShadowEnabled = 1;
    }

    if (s.FrameVolumeControlEnabled() != 0) {
        CheckMenuItem(menu, 0x12B, MF_BYCOMMAND | MF_CHECKED);
        SendMessageA(GetDlgItem(hwnd, panel::kFrameVolumeCheckbox), BM_SETCHECK, BST_CHECKED, 0);
    }
    static constexpr UINT kInitiallyChecked[] = {
        0x11D, 0x125, 0x127, 0x12A, 0x10D,
    };
    for (UINT id : kInitiallyChecked)
        CheckMenuItem(menu, id, MF_BYCOMMAND | MF_CHECKED);

    setTrack(0x216, 100 - s.FrameNormalization());
    SendMessageA(GetDlgItem(hwnd, panel::kCoordAxisCheckbox), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kPhysicsCheckbox), BM_SETCHECK, BST_CHECKED, 0);

    // Match the checked startup state to the actual 4096 shadow-map size.
    SendMessageA(GetDlgItem(hwnd, panel::kSelfShadowCheckbox),
                 BM_SETCHECK, BST_CHECKED, 0);
    CmdControl500(app, hwnd, panel::kSelfShadowCheckbox, BN_CLICKED);

    // ---- 0x46AF24..0x46AFCE: submenu state and dynamic full-screen label --
    MENUITEMINFOA mii{};
    mii.cbSize = sizeof(mii);
    SetModelExtensionMenuEnabled(app, false);
    DrawMenuBar(hwnd);

    static const char kJpFullScreen[] =
        "\x83\x74\x83\x8B\x83\x58\x83\x4E\x83\x8A\x81\x5B"
        "\x83\x93\x95\x5C\x8E\xA6(Alt+Enter)";
    static const char kJpNvidia3D[] =
        "NVIDIA 3D Vision\x95\x5C\x8E\xA6(Alt+Enter)";
    const bool stereoCapable = w->stereoEnabled != 0;
    if (app->EnglishUI() == 2) {
        static const wchar_t kZhFullScreen[] = L"全屏(Alt+Enter)";
        static const wchar_t kZhNvidia3D[] = L"NVIDIA 3D Vision(Alt+Enter)";
        MENUITEMINFOW zhMii{};
        zhMii.cbSize = sizeof(zhMii);
        zhMii.fMask = MIIM_STRING;
        zhMii.dwTypeData = const_cast<wchar_t*>(
            stereoCapable ? kZhNvidia3D : kZhFullScreen);
        SetMenuItemInfoW(GetSubMenu(menu, 2), 0x1D, TRUE, &zhMii);
    } else {
    const char* fullScreenText;
    if (app->EnglishUI() != 0)
        fullScreenText = stereoCapable
            ? "NVIDIA 3D Vision(Alt+Enter)"
            : "Full screen(Alt+Enter)";
    else
        fullScreenText = stereoCapable ? kJpNvidia3D : kJpFullScreen;
    mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = const_cast<char*>(fullScreenText);
    SetMenuItemInfoA(GetSubMenu(menu, 2), 0x1D, TRUE, &mii);
    }

    // ---- 0x46AFD4..0x46B06E: self-shadow mode/range mirrors ---------------
    SendMessageA(GetDlgItem(hwnd, panel::kEditMode1Checkbox), BM_SETCHECK, BST_CHECKED, 0);
    const double rangeValue =
        10000.0 - static_cast<double>(s.state.physicsInterval) *
                      100000.0;
    const int rangePos = static_cast<int>(rangeValue + 0.5);
    setTrack(0x230, rangePos);
    sprintf_s(text, 0x100, "%d", rangePos);
    SetWindowTextA(GetDlgItem(hwnd, panel::kSelfShadowRangeEdit), text);
    return true;
}

// ---------------------------------------------------------------------------
// VA 0x00410040 - was Sub410040.  RefillBoneRegisterCombo(app, slot):
// bone-register combo (control 450) refill.  CB_RESETCONTENT, then for the
// model in slot app+0x780[slot] every bone whose type byte (+484) is < 7
// or == 8 is appended (CB_ADDSTRING) with the English (+20) or Japanese
// (+0) name selected by the UI flag; finishes with PostLanguageSweep
// (0x40D070).  Negative slot only clears.  (Return value is the sweep's
// BOOL; callers ignore it.)
// The original's UI flag is two-valued, so its `!= 0` test means "English
// UI"; the port's third language (2 = Chinese) is not English, hence the
// shared UiUsesEnglishNames() test.  Control 450 is the follow-bone list of
// the model/camera panel (0x1C2, filled on scene load from the camera's
// parent model and re-filled by RepopulateBoneCombo when that model changes) -
// with `!= 0` the Chinese UI showed English bone names there while the four
// morph combos (post_load_init.cpp, `== 1`) showed the model's own names.
// ---------------------------------------------------------------------------
void RefillBoneRegisterCombo(MMDApp* app, int slot) {
    HWND combo = GetDlgItem(static_cast<HWND>(app->Hwnd()), panel::kBoneRegisterCombo);
    const auto* model = slot >= 0 && slot < kModelSlotCount && app->ModelSlot(slot)
        ? mdl::Mdl(app->ModelSlot(slot)) : nullptr;
    FillBoneCombo(combo, model, UiUsesEnglishNames(app->EnglishUI()));
    PostLanguageSweep2(app);                                     // 0x410132
}

}  // namespace mikumikustudio
