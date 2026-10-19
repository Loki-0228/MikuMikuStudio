// ===========================================================================
// VA 0x0042C810 - main-window D3D viewport refresh
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"

namespace mikumikustudio {
void PicBgOverlayRefresh(MMDApp* app);  // VA 0x00417130
namespace {

struct ScreenVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
    float u;
    float v;
};
static_assert(sizeof(ScreenVertex) == 28);

}  // namespace

// ===========================================================================
// Camera projection funnel - the single perspective/orthographic builder.
// ===========================================================================
// Original call sites of the perspective build (all five use the same shape:
// fov = cameraFov(0x9F0B4) * 0.01745329238474369, aspect = wrapper+0x3A9D0,
// zn = 1.0f, zf = 100000.0f, then SetTransform(D3DTS_PROJECTION = 3)):
//   447 FOV slider   0x14003ED20..0x14003EE33   (ui_hscroll.cpp)
//   448 FOV edit     0x14005489B..0x140054975   (ui_edit_commit.cpp)
//   camera key seek  0x140059D73..0x140059DEF   (timeline_advance.cpp)
//   viewport refresh 0x1400218B0..0x140021912   (RefreshMainWindowViewport)
//   device init      0x1400379C0..             (d3d_init.cpp, PI/4 seed)
// The per-frame orthographic override (cameraPerspective != 0, byte 0x354)
// is 0x140027E96..0x140027F1F: memset(0), m00 = -2/cameraDistance,
// m11 = aspect*m00, m22 = 0.0013f, m33 = 1.0f, SetTransform(3).
// ===========================================================================

// Perspective matrix of the live camera state into `out` (row-major float[16],
// D3DXMatrixPerspectiveFovLH layout).  Pure: no device required, so the FOV
// data flow is assertable without a window (tests/panel_key_tests.cpp).
bool BuildCameraPerspectiveProjection(const MMDApp* app, float aspect,
                                      float out[16]) {
    if (app == nullptr || out == nullptr || !(aspect > 0.0f)) {
        return false;
    }
    // dbl_52BB20 (0x140132BA8) = 0.01745329238474369, mulss: the original
    // converts degrees to radians in single precision; the two other
    // conversion sites in this file do the same.  (The state member is read directly -
    // CameraFov() has no const overload. Defect B1-07.
    const float fovDegrees = app->state.cameraFov;
    const float fovRadians = fovDegrees * 0.01745329238474369f;
    if (!(fovRadians > 0.0f)) {
        return false;
    }

    auto& api = d3dx::Get();
    if (api.Load()) {
        d3dx::D3DXMATRIXF matrix{};
        // zn = flt_140132984 (1.0f), zf = flt_140132B44 (100000.0f).
        api.perspectiveFovLH(&matrix, fovRadians, aspect, 1.0f, 100000.0f);
        std::memcpy(out, &matrix, sizeof(matrix));
        return true;
    }
    // The original cannot start without the d3dx import; keep the restored
    // build operable with the identical closed form (same constants, same
    // row-major layout D3DXMatrixPerspectiveFovLH writes).
    constexpr float kNear = 1.0f;
    constexpr float kFar = 100000.0f;
    const float yScale = 1.0f / std::tan(fovRadians * 0.5f);
    const float xScale = yScale / aspect;
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = xScale;
    out[5] = yScale;
    out[10] = kFar / (kFar - kNear);
    out[11] = 1.0f;
    out[14] = -kNear * kFar / (kFar - kNear);
    return true;
}

CameraFrameProjection CameraFrameProjectionFor(const MMDApp* app) {
    if (app == nullptr) {
        return CameraFrameProjection::None;
    }
    return app->state.cameraPerspective != 0
        ? CameraFrameProjection::Orthographic
        : CameraFrameProjection::Perspective;
}

CameraFrameProjection ApplyFrameCameraProjection(MMDApp* app) {
    const CameraFrameProjection kind = CameraFrameProjectionFor(app);
    if (kind == CameraFrameProjection::None) {
        return kind;
    }
    D3DRenderer* wrapper = app->Renderer();
    if (wrapper == nullptr) {
        return kind;
    }
    IDirect3DDevice9* device = wrapper->device;
    if (device == nullptr) {
        return kind;
    }
    float aspect = wrapper->aspectRatio;
    if (!(aspect > 0.0f)) {
        aspect = 4.0f / 3.0f;
    }

    float projection[16]{};
    if (kind == CameraFrameProjection::Orthographic) {
        // x64 0x140027E96..0x140027F1F (the port's previous inline copy).
        const float m00 = -2.0f / app->CameraDistance();
        projection[0] = m00;
        projection[5] = aspect * m00;
        projection[10] = 0.0013f;
        projection[15] = 1.0f;
    } else if (!BuildCameraPerspectiveProjection(app, aspect, projection)) {
        return CameraFrameProjection::None;
    }
    device->SetTransform(
        D3DTS_PROJECTION,
        reinterpret_cast<const D3DMATRIX*>(projection));
    return kind;
}

// The 447 slider body (0x14003ED20..0x14003EE33), split from the WM_HSCROLL
// dispatch so the value flow is testable: TBM_GETPOS -> cameraFov ->
// perspective build -> SetTransform.  Returns true when the value changed.
bool ApplyCameraFovSlider(MMDApp* app, int position) {
    if (app == nullptr) {
        return false;
    }
    const float value = static_cast<float>(position);
    const bool changed = app->CameraFov() != value;
    app->CameraFov() = value;
    ApplyFrameCameraProjection(app);
    return changed;
}

namespace {

void WriteQuad(IDirect3DVertexBuffer9* buffer, float left, float top,
               float right, float bottom, float uRight, float vBottom) {
    if (buffer == nullptr)
        return;

    ScreenVertex* out = nullptr;
    if (FAILED(buffer->Lock(0, 6 * sizeof(ScreenVertex),
                            reinterpret_cast<void**>(&out), 0)) ||
        out == nullptr)
        return;

    const ScreenVertex vertices[6] = {
        {right, top,    0.0f, 1.0f, 0xFFFFFFFFu, uRight, 0.0f},
        {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, uRight, vBottom},
        {left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, 0.0f,   0.0f},
        {left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, 0.0f,   0.0f},
        {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, uRight, vBottom},
        {left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f,   vBottom},
    };
    std::copy(std::begin(vertices), std::end(vertices), out);
    buffer->Unlock();
}

}  // namespace

void RefreshMainWindowViewport(MMDApp* app) {  // was Sub42C810
    auto& s = *app;
    RECT& view = s.ViewportRect();                     // 0xA0D40
    const HWND hwnd = app->MainWindow();
    if (hwnd == nullptr)
        return;

    const WindowDpiScope dpi(hwnd);
    GetClientRect(hwnd, &view);                         // 0x42C82D/85F

    // Fullscreen uses the whole main-window client area. Do not reserve the
    // sidebar and bottom panels as the normal window layout does.
    if (s.FullscreenMode() != 0) {
        RefreshSeparateWindowViewport(app);
        return;
    }

    // A0D38 selects the separate render window.  The original delegates the
    // remainder to 0x4290F0; its main-window-independent RECT/sidebar stores
    // still have to happen here before that call.
    if (s.FloatingWindow() != nullptr) {
        s.SidebarWidth() = view.right - 3;
        RefreshSeparateWindowViewport(app);
        return;
    }

    const int renderWidth = s.RenderWidth();
    const int renderHeight = s.RenderHeight();
    if (renderWidth <= 0 || renderHeight <= 0)
        return;

    const int availableHeight = view.bottom - 217;
    const int sidebar = s.SidebarWidth();
    const int availableWidth = view.right - sidebar - 9;
    const int fittedWidth = availableHeight * renderWidth / renderHeight;

    if (availableWidth >= fittedWidth) {                // 0x42C8DD
        view.top += 25;
        view.bottom -= 192;
        const int center = availableWidth / 2 + sidebar + 9;
        view.left = center - fittedWidth / 2;
        view.right = center + fittedWidth / 2;
    } else {                                            // 0x42C89C
        const int fittedHeight =
            availableWidth * renderHeight / renderWidth;
        view.left = sidebar + 9;
        view.top = availableHeight / 2 - fittedHeight / 2 + 25;
        view.bottom = fittedHeight / 2 + availableHeight / 2 + 25;
    }

    if (view.left < 0)
        view.left = 0;
    if (view.right < view.left)
        view.right = view.left + 1;
    if (view.top < 0)
        view.top = 0;
    if (view.bottom < view.top)
        view.bottom = view.top + 1;

    D3DRenderer* wrapper = app->Renderer();
    if (wrapper == nullptr)
        return;
    IDirect3DDevice9* device = wrapper->device;
    if (device == nullptr)
        return;

    D3DVIEWPORT9 viewport{};
    if (s.RecordingWindow() != nullptr) {
        viewport.Width = wrapper->multisampleAvailable == 0
            ? static_cast<DWORD>(wrapper->screenWidth)
            : static_cast<DWORD>(renderWidth);
        viewport.Height = wrapper->multisampleAvailable == 0
            ? static_cast<DWORD>(wrapper->screenHeight)
            : static_cast<DWORD>(renderHeight);
    } else {
        viewport.X = static_cast<DWORD>(view.left);
        viewport.Y = static_cast<DWORD>(view.top);
        viewport.Width = static_cast<DWORD>(view.right - view.left);
        viewport.Height = static_cast<DWORD>(view.bottom - view.top);
    }
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    device->SetViewport(&viewport);                     // 0x42C9E5

    const float aspect = static_cast<float>(renderWidth) /
                         static_cast<float>(renderHeight);
    wrapper->aspectRatio = aspect;
    auto& api = d3dx::Get();
    if (api.Load()) {
        d3dx::D3DXMATRIXF projection{};
        const float fovRadians =
            s.CameraFov() * 0.01745329238474369f;
        api.perspectiveFovLH(&projection, fovRadians, aspect,
                             1.0f, 100000.0f);
        device->SetTransform(D3DTS_PROJECTION,
            reinterpret_cast<const D3DMATRIX*>(&projection));
    }

    wrapper->viewScale = static_cast<float>(
        static_cast<double>(viewport.Width) * 1.2 / 1280.0);

    const float width = static_cast<float>(view.right - view.left);
    WriteQuad(s.LeftViewportVertices(),
              static_cast<float>(view.right) - width / 3.0f,
              static_cast<float>(view.top),
              static_cast<float>(view.right),
              static_cast<float>(view.top) + width / 3.0f,
              1.0f, 1.0f);
    WriteQuad(s.RightViewportVertices(),
              static_cast<float>(view.right) - 220.0f,
              static_cast<float>(view.top),
              static_cast<float>(view.right) - 60.0f,
              static_cast<float>(view.top) + 120.0f,
              0.625f, 0.9375f);
}

void RefreshSeparateWindowViewport(MMDApp* app) {  // was Sub4290F0, 0x4290F0..0x42976B
    auto& s = *app;
    RECT& view = s.ViewportRect();
    const int renderWidth = s.RenderWidth();
    const int renderHeight = s.RenderHeight();
    if (renderWidth <= 0 || renderHeight <= 0)
        return;

    const bool fullScreen = s.FullscreenMode() != 0;
    HWND target = fullScreen
        ? app->MainWindow()
        : s.FloatingWindow();
    const WindowDpiScope dpi(target);
    if (target == nullptr || !GetClientRect(target, &view))
        return;

    int clientWidth = view.right;
    int clientHeight = view.bottom;
    int desiredWidth = clientHeight * renderWidth / renderHeight;
    if (fullScreen) {
        if (clientWidth < desiredWidth) {
            const int fitted = clientWidth * renderHeight / renderWidth;
            const int center = clientHeight / 2;
            view.top = center - fitted / 2;
            view.bottom = center + fitted / 2;
        }
    } else {
        const int availableHeight = clientHeight - 57;
        desiredWidth = availableHeight * renderWidth / renderHeight;
        if (clientWidth >= desiredWidth) {
            view.top += 25;
            view.bottom -= 32;
        } else {
            const int fitted = clientWidth * renderHeight / renderWidth;
            const int center = availableHeight / 2;
            view.top = center - fitted / 2 + 25;
            view.bottom = center + fitted / 2 + 25;
        }
    }
    if (clientWidth >= desiredWidth) {
        const int width = desiredWidth;
        const int center = clientWidth / 2;
        view.left = center - width / 2;
        view.right = center + width / 2;
    }

    D3DRenderer* wrapper = app->Renderer();
    if (wrapper == nullptr)
        return;
    IDirect3DDevice9* device = wrapper->device;
    if (device == nullptr)
        return;

    D3DVIEWPORT9 viewport{};
    if (s.RecordingWindow() != nullptr) {
        viewport.Width = wrapper->multisampleAvailable == 0
            ? static_cast<DWORD>(wrapper->screenWidth)
            : static_cast<DWORD>(renderWidth);
        viewport.Height = wrapper->multisampleAvailable == 0
            ? static_cast<DWORD>(wrapper->screenHeight)
            : static_cast<DWORD>(renderHeight);
    } else {
        viewport.X = static_cast<DWORD>(view.left);
        viewport.Y = static_cast<DWORD>(view.top);
        viewport.Width = static_cast<DWORD>(view.right - view.left);
        viewport.Height = static_cast<DWORD>(view.bottom - view.top);
    }
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    device->SetViewport(&viewport);

    const float aspect = static_cast<float>(renderWidth) /
                         static_cast<float>(renderHeight);
    wrapper->aspectRatio = aspect;
    auto& api = d3dx::Get();
    if (api.Load()) {
        d3dx::D3DXMATRIXF projection{};
        api.perspectiveFovLH(&projection,
            s.CameraFov() * 0.01745329238474369f,
            aspect, 1.0f, 100000.0f);
        device->SetTransform(D3DTS_PROJECTION,
            reinterpret_cast<const D3DMATRIX*>(&projection));
    }
    wrapper->viewScale = static_cast<float>(
        static_cast<double>(view.right - view.left) * 1.2 / 1280.0);

    if (s.WaveEnabled() != 0)
        TimelineDrawTicks(s.TimelineStartFrame(),
                          s.SidebarWidth());
    if (s.PictureBackgroundEnabled() != 0)
        PicBgOverlayRefresh(app);

    const float width = static_cast<float>(view.right - view.left);
    WriteQuad(s.LeftViewportVertices(),
              static_cast<float>(view.right) - width / 3.0f,
              static_cast<float>(view.top),
              static_cast<float>(view.right),
              static_cast<float>(view.top) + width / 3.0f,
              1.0f, 1.0f);
    WriteQuad(s.RightViewportVertices(),
              static_cast<float>(view.right) - 220.0f,
              static_cast<float>(view.top),
              static_cast<float>(view.right) - 60.0f,
              static_cast<float>(view.top) + 120.0f,
              0.625f, 0.9375f);
}

}  // namespace mikumikustudio
