// ===========================================================================
// Render-output background overlay pair.
//
//   VA 0x004168D0 - AVI background frame -> display texture + overlay quad
//   VA 0x00417130 - picture background overlay quad refresh
//
// Both functions are app methods (thiscall, this = Block) reached from the
// canvas-size dialog (menu 0xD4), the picture renderer (0x114 / case 276),
// the AVI record starters (0x45E820/0x464760 via the 9E428 gate) and the
// per-frame render chain.  The former Sub4168D0/Sub417130 forwarding twins
// that used to live at the bottom of this file were removed; every call
// site in this tree now uses the semantic names directly.
//
// 0x4168D0 decodes the current AVI background frame with the VFW chain
// (AVIStreamTimeToSample / AVIStreamGetFrame / DrawDibDraw) into an
// offscreen surface, stretches it into level 0 of the 1024x1024 display
// texture (D3DUSAGE_RENDERTARGET, X8R8G8B8) and refills the 6-vertex
// (28-byte, XYZRHW|DIFFUSE|TEX1) overlay quad in the vertex buffer at
// app+0x9E3F8.  0x417130 refreshes the identical quad in the buffer at
// app+0x9E430 from the picture-background placement fields.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/native_media.hpp"
#include <Windows.h>
#include <algorithm>
#include <climits>
#include <d3d9.h>
#include <vfw.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../app/device_lost_wait.hpp"

namespace mikumikustudio {
namespace {

// .rdata 0x52BA20 - double 1.2000000476837158, overlay scale divisor used by
// both quad builders (x87 divide runs in double precision, result stored
// as float - reproduced by keeping the arithmetic in double).
constexpr double kOverlayScale = 1.2000000476837158;
constexpr GUID kVideoUploadRevision =
    {0x36f9a7c2, 0x8ef1, 0x41e8, {0xa3, 0x91, 0x2f, 0xc8, 0x5e, 0x8d, 0x10, 0x44}};

// 0x529688 - L"%s%s"; the original calls it with no varargs (VC9 stack
// garbage).  The port passes two empty strings (same idiom as the file
// dialogs) which yields the empty-string write the original produced.
void ClearAviPathEcho(MMDApp* app) {
    swprintf_s(app->AviBackgroundPath(), 0x100, L"%s%s", L"", L"");
}

void ReleaseAviBackgroundHandles(MMDApp* app) {
    media::ReleaseVideo(app);
    if (app->AviFrameReader() != nullptr) {                       // 0x416927
        AVIStreamGetFrameClose(static_cast<PGETFRAME>(app->AviFrameReader()));
        app->AviFrameReader() = nullptr;
    }
    if (app->AviStream() != nullptr) {                            // 0x41693D
        AVIStreamRelease(static_cast<PAVISTREAM>(app->AviStream()));
        app->AviStream() = nullptr;
    }
    if (app->AviFile() != nullptr) {                              // 0x416953
        AVIFileRelease(static_cast<PAVIFILE>(app->AviFile()));
        app->AviFile() = nullptr;
    }
}

HWND OverlayHwnd(MMDApp* app) {
    return static_cast<HWND>(app->state.hwnd);
}

// One complete XYZRHW/DIFFUSE/TEX1 vertex for each triangle-list corner.
// In particular, every reciprocalW must be initialized: a missing value
// changes perspective-correct UV interpolation across the entire triangle.
struct BackgroundVertex {
    float x, y, z, reciprocalW;
    D3DCOLOR color;
    float u, v;
};
static_assert(sizeof(BackgroundVertex) == 28);
constexpr UINT kBackgroundVertexBytes = 6 * sizeof(BackgroundVertex);

void WriteOverlayQuad(void* locked, double left, double top, double right,
                      double bottom) {
    const float l = static_cast<float>(left), t = static_cast<float>(top);
    const float r = static_cast<float>(right), b = static_cast<float>(bottom);
    const BackgroundVertex vertices[] = {
        {r, t, 0.f, 1.f, 0xFFFFFFFF, 1.f, 0.f},
        {r, b, 0.f, 1.f, 0xFFFFFFFF, 1.f, 1.f},
        {l, t, 0.f, 1.f, 0xFFFFFFFF, 0.f, 0.f},
        {l, t, 0.f, 1.f, 0xFFFFFFFF, 0.f, 0.f},
        {r, b, 0.f, 1.f, 0xFFFFFFFF, 1.f, 1.f},
        {l, b, 0.f, 1.f, 0xFFFFFFFF, 0.f, 1.f},
    };
    std::memcpy(locked, vertices, sizeof vertices);
}

// Shared geometry tail of 0x4168D0 (fields 9E414/418/41C/420/424) and
// 0x417130 (fields 9E434/438/43C/440/444): offX, offY, zoom, w, h.
struct OverlayPlacement {
    std::int32_t x;
    std::int32_t y;
    float scale;
    std::int32_t width;
    std::int32_t height;
};

void ComputeOverlayRect(MMDApp* app, const OverlayPlacement& placement,
                        double* leftOut, double* topOut,
                        double* rightOut, double* bottomOut) {
    auto& s = *app;
    D3DRenderer* wrapper = s.Renderer();  // viewScale (0x1D4F0)
    const double ratio = wrapper->viewScale;
    const double k = kOverlayScale;
    const double offXv = static_cast<double>(placement.x);
    const double offYv = static_cast<double>(placement.y);
    const double zoomV = static_cast<double>(placement.scale);
    const double wv = static_cast<double>(placement.width);
    const double hv = static_cast<double>(placement.height);
    const RECT& viewport = s.ViewportRect();
    const std::int32_t rectL = viewport.left;
    const std::int32_t rectT = viewport.top;
    const std::int32_t rectR = viewport.right;
    const std::int32_t rectB = viewport.bottom;

    if (s.RecordingWindow() == nullptr) {                       // 0xA0D24
        // 0x416F13: full-window placement against the A0D40 rect.
        const double left = rectL + offXv * ratio / k - 1.0;
        const double top = offYv * ratio / k + rectT - 1.0;
        *leftOut = left;
        *topOut = top;
        *rightOut = wv * zoomV * ratio / k + left + 1.0;
        *bottomOut = hv * zoomV * ratio / k + top + 1.0;
        return;
    }

    if (wrapper->multisampleAvailable != 0) {  // 0x1D4F8
        // 0x416E23: separate render window - uniform scale.  The x87 pair
        // (fildl + fidivl + fstps) divides in double precision and stores
        // the factor as float; reproduced explicitly.
        const float scale = static_cast<float>(
            static_cast<double>(s.state.renderW) /
            static_cast<double>(rectR - rectL));
        const double sc = scale;
        const double left = sc * offXv * ratio / k - 1.0;
        const double top = sc * offYv * ratio / k - 1.0;
        *leftOut = left;
        *topOut = top;
        *rightOut = wv * zoomV * ratio * sc / k + left + 1.0;
        // This branch pre-divides by k at 0x416FE0 and jumps past the
        // shared divide - a single /k total.
        *bottomOut = hv * zoomV * ratio * sc / k + top + 1.0;
        return;
    }

    // 0x416D0B: per-axis float scales against the separate-window rect
    // (again fildl/fidivl/fstps, not integer division).
    const float sxF = static_cast<float>(
        static_cast<double>(wrapper->screenWidth) /   // 0x1D4E4
        static_cast<double>(rectR - rectL));
    const float syF = static_cast<float>(
        static_cast<double>(wrapper->screenHeight) /  // 0x1D4E8
        static_cast<double>(rectB - rectT));
    const double left = static_cast<double>(sxF) * offXv * ratio / k - 1.0;
    const double top = static_cast<double>(syF) * offYv * ratio / k - 1.0;
    *leftOut = left;
    *topOut = top;
    *rightOut =
        wv * zoomV * ratio * sxF / k + left + 1.0;
    *bottomOut =
        hv * zoomV * ratio * syF / k + top + 1.0;
}

void RefreshOverlayVertexBuffer(MMDApp* app, IDirect3DVertexBuffer9* vb,
                                const OverlayPlacement& placement) {
    // Port-side guard: the 168-byte overlay vertex buffers are created by the
    // D3D init chain; the original walks them unconditionally.
    if (vb == nullptr || app->Renderer() == nullptr)
        return;
    void* locked = nullptr;
    if (FAILED(vb->Lock(0, kBackgroundVertexBytes, &locked, 0)) || locked == nullptr)  // 0x416CE9
        return;
    double left, top, right, bottom;
    ComputeOverlayRect(app, placement, &left, &top, &right, &bottom);
    WriteOverlayQuad(locked, left, top, right, bottom);
    vb->Unlock();                                                // 0x417123
}

}  // namespace

// VA 0x004168D0 - AVI background frame -> texture + overlay quad.
void AviBgOverlayRefresh(MMDApp* app) {
    // Recording cleanup still applies CPU frame/panel state after a failed
    // Reset. Preserve its background source until the next recovered frame.
    if (device_lost_wait::Pending(app)) {
        device_lost_wait::DeferBackgroundRefresh(app);
        return;
    }
    auto& s = *app;
    D3DRenderer* wrapper = s.Renderer();
    if (wrapper == nullptr)
        return;
    IDirect3DDevice9* device = wrapper->device;  // 0x1D4E0
    if (device == nullptr)
        return;

    if (s.AviBackgroundTexture() == nullptr) {                    // 0x4168F5
        IDirect3DTexture9* tex = nullptr;
        const HRESULT hr = device->CreateTexture(                 // 0x41690F
            0x400, 0x400, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
            D3DPOOL_DEFAULT, &tex, nullptr);
        s.AviBackgroundTexture() = tex;
        if (hr != 0) {
            ReleaseAviBackgroundHandles(app);
            MessageBoxA(OverlayHwnd(app),                         // 0x416988
                        app->EnglishUI() != 0
                            ? "cannot make AVIdispTxtr"
                            : "\x41\x56\x49\x95\x5C\x8E\xA6\x97\x70\x83\x65"
                              "\x83\x4E\x83\x58\x83\x60\x82\xAA\x8D\xEC\x90"
                              "\xAC\x82\xC5\x82\xAB\x82\xDC\x82\xB9\x82\xF1",
                        "InitFont", 0);
            s.AviBackgroundEnabled() = 0;
            ClearAviPathEcho(app);                                // 0x4169A4
            return;
        }
    }
    if (s.AviBackgroundSurface() == nullptr) {                    // 0x4169EB
        IDirect3DSurface9* surf = nullptr;
        const HRESULT hr = device->CreateOffscreenPlainSurface(
            s.AviFrameWidth(), s.AviFrameHeight(), D3DFMT_X8R8G8B8,
            D3DPOOL_DEFAULT, &surf, nullptr);
        s.AviBackgroundSurface() = surf;
        if (hr != 0) {
            ReleaseAviBackgroundHandles(app);
            MessageBoxA(OverlayHwnd(app),                         // 0x416A78
                        app->EnglishUI() != 0
                            ? "Cannot make Surface for AVI file"
                            : "\x41\x56\x49\x97\x70\x83\x54\x81\x5B\x83\x74"
                              "\x83\x46\x83\x43\x83\x58\x82\xF0\x8D\xEC\x90"
                              "\xAC\x82\xC5\x82\xAB\x82\xDC\x82\xB9\x82\xF1",
                        app->EnglishUI() != 0
                            ? "open AVI file"
                            : "\x41\x56\x49\x83\x66\x81\x5B\x83\x5E\x93\xC7"
                              "\x8D\x9E",
                        0);
            s.AviBackgroundEnabled() = 0;
            ClearAviPathEcho(app);                                // 0x416BF8
            return;
        }
    }

    // Frame selection (0x416A83..0x416B27).  9E404/9E400 are owned by
    // LoadAviFile (0x433250, ported in src/media/media_load.cpp).
    PAVISTREAM stream = static_cast<PAVISTREAM>(s.AviStream());
    PGETFRAME getFrame = static_cast<PGETFRAME>(s.AviFrameReader());
    const auto milliseconds = [](double seconds) {
        return static_cast<LONG>(std::clamp(seconds * 1000.0, 0.0, double(LONG_MAX)));
    };
    const auto boundedSample = [](std::int64_t value) {
        return static_cast<LONG>(std::clamp(value, std::int64_t(LONG_MIN), std::int64_t(LONG_MAX)));
    };
    LONG sample;
    if (s.PlaybackActive() == 0) {
        if (s.AviUsesThirtyFpsTiming() == 0) {
            sample = stream != nullptr
                ? AVIStreamTimeToSample(
                      stream, milliseconds(s.state.currentFrame / 30.0))                       // 0x416B27
                : 0;
        } else {
            sample = s.state.currentFrame;
        }
    } else if (s.FrameStepPlayback() == 0 ||
               s.AviUsesThirtyFpsTiming() == 0 ||
               s.state.aviRecordFps != g_FrameScale) {
        // 0x416ADE: wall-clock driven sample from the seconds field
        // (9E64C is the frameB float copy; 0x52BA60 = double 1000.0).
        const double secondsF = s.PlaybackCursorSeconds();
        const LONG t = milliseconds(secondsF);
        sample = boundedSample(std::int64_t(stream != nullptr ? AVIStreamTimeToSample(stream, t) : 0) +
                 static_cast<std::int32_t>(s.state.a0B10));
    } else {
        sample = s.state.aviBackgroundSample;      // 0x416ABC
        s.state.aviBackgroundSample = boundedSample(std::int64_t(sample) + 1);
    }
    if (sample < s.AviStreamStartFrame())
        sample = s.AviStreamStartFrame();
    const LONG last = boundedSample(std::int64_t(s.AviStreamEndFrame()) - 1);
    if (last < sample)
        sample = last;

    std::wstring decodeError;
    double videoSeconds = s.state.currentFrame / 30.0;
    if (s.PlaybackActive() != 0) {
        const double cursor = s.PlaybackCursorSeconds();
        videoSeconds = cursor + static_cast<std::int32_t>(s.state.a0B10) / 30.0;
    }
    const auto previousRevision = media::VideoFrameRevision(app);
    void* bits = media::HasVideo(app) ? media::VideoFrame(app, videoSeconds, decodeError, s.RecordingWindow() != nullptr)
                : getFrame != nullptr ? AVIStreamGetFrame(getFrame, sample)
                                     : nullptr;                   // 0x416B4F
    if (bits == nullptr) {
        ReleaseAviBackgroundHandles(app);
        if (!decodeError.empty())
            MessageBoxW(OverlayHwnd(app), decodeError.c_str(), L"Video playback", MB_OK | MB_ICONERROR);
        else MessageBoxA(OverlayHwnd(app),                        // 0x416BD8
                    app->EnglishUI() != 0
                        ? "Cannot open AVI file"
                        : "\x41\x56\x49\x83\x74\x83\x40\x83\x43\x83\x8B\x82"
                          "\xF0\x93\xC7\x82\xDD\x8D\x9E\x82\xDF\x82\xDC\x82"
                          "\xB9\x82\xF1",
                    app->EnglishUI() != 0
                        ? "open AVI file"
                        : "\x41\x56\x49\x83\x66\x81\x5B\x83\x5E\x93\xC7\x8D"
                          "\x9E",
                    0);
        s.AviBackgroundEnabled() = 0;
        ClearAviPathEcho(app);                                    // 0x416BF8
        return;
    }

    const auto revision = media::VideoFrameRevision(app);
    if (revision != previousRevision) s.state.messageSeen = 1;
    unsigned long long uploadedRevision = 0;
    DWORD revisionSize = sizeof(uploadedRevision);
    if (revision && SUCCEEDED(s.AviBackgroundTexture()->GetPrivateData(
            kVideoUploadRevision, &uploadedRevision, &revisionSize)) &&
        revisionSize == sizeof(uploadedRevision) && uploadedRevision == revision) {
        AviBgPlacementRefresh(app);
        device_lost_wait::BackgroundRefreshed(app);
        return;
    }

    // The decoder context is owned by the app and closed at shutdown.
    if (!revision && s.AviDrawDib() == nullptr)
        s.AviDrawDib() = DrawDibOpen();
    if (!revision && s.AviDrawDib() == nullptr) return;

    auto* header = static_cast<LPBITMAPINFOHEADER>(bits);
    DWORD paletteColors = header->biClrUsed;
    if (paletteColors == 0 && header->biBitCount <= 8)
        paletteColors = 1u << header->biBitCount;
    std::size_t pixelOffset = header->biSize + paletteColors * sizeof(RGBQUAD);
    if (header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == BI_BITFIELDS)
        pixelOffset += 3 * sizeof(DWORD);

    IDirect3DSurface9* surface = s.AviBackgroundSurface();
    if (revision) {
        // Media Foundation already supplies RGB32. Avoid the GDI conversion
        // and GetDC synchronization for every full-resolution video frame.
        D3DLOCKED_RECT locked{};
        if (FAILED(surface->LockRect(&locked, nullptr, 0))) return;
        const auto* pixels = static_cast<unsigned char*>(bits) + pixelOffset;
        for (int y = 0; y < header->biHeight; ++y)
            std::memcpy(static_cast<unsigned char*>(locked.pBits) + size_t(y) * locked.Pitch,
                        pixels + size_t(header->biHeight - 1 - y) * header->biWidth * 4,
                        size_t(header->biWidth) * 4);
        surface->UnlockRect();
    } else {
        HDC hdc = nullptr;
        if (FAILED(surface->GetDC(&hdc))) return;
        const BOOL drawn = DrawDibDraw(static_cast<HDRAWDIB>(s.AviDrawDib()), hdc,
            0, 0, -1, -1, header, static_cast<unsigned char*>(bits) + pixelOffset,
            0, 0, -1, -1, 0);
        surface->ReleaseDC(hdc);
        if (!drawn) return;
    }

    IDirect3DTexture9* texture = s.AviBackgroundTexture();
    IDirect3DSurface9* texSurface = nullptr;
    if (FAILED(texture->GetSurfaceLevel(0, &texSurface)) ||       // 0x416C67
        texSurface == nullptr)
        return;
    HRESULT uploaded = device->StretchRect(surface, nullptr, texSurface, nullptr,
                                           D3DTEXF_LINEAR);      // 0x416C8D
    if (FAILED(uploaded))
        uploaded = device->StretchRect(surface, nullptr, texSurface, nullptr,
                                        D3DTEXF_NONE);            // 0x416CB7
    texSurface->Release();
    if (SUCCEEDED(uploaded)) {
        if (revision) texture->SetPrivateData(kVideoUploadRevision, &revision, sizeof(revision), 0);
        device_lost_wait::BackgroundRefreshed(app);
    }
    // (the original clears its stack slot after the release - 0x416CC7)

    AviBgPlacementRefresh(app);
}

void AviBgPlacementRefresh(MMDApp* app) {
    auto& s = *app;
    RefreshOverlayVertexBuffer(
        app, s.AviOverlayVertices(),
        {s.AviOffsetX(), s.AviOffsetY(), s.AviScale(),
         s.AviFrameWidth(), s.AviFrameHeight()});
}

// VA 0x00417130 - picture background overlay quad refresh.
void PicBgOverlayRefresh(MMDApp* app) {
    auto& s = *app;
    RefreshOverlayVertexBuffer(
        app, s.PictureOverlayVertices(),
        {s.PictureOffsetX(), s.PictureOffsetY(), s.PictureScale(),
         s.PictureWidth(), s.PictureHeight()});
}

// (The former Sub4168D0/Sub417130 forwarding twins were removed; every
//  call site now uses AviBgOverlayRefresh/PicBgOverlayRefresh directly.
//  The real bodies above are the parallel-port winner - full VFW decode
//  chain; the background_plane.cpp twin was removed.)

}  // namespace mikumikustudio
