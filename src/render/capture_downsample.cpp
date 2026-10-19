// ===========================================================================
// VA 0x00425970 - DownsampleCaptureSurface (original: sub_425970)
// ===========================================================================
// Reads the full-size capture render target back to system memory, performs
// the original two-pass rational BGRA box filter in place, then uploads the
// requested rectangle to the secondary-window back buffer.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {
namespace {

std::int32_t GcdBySubtraction(std::int32_t a, std::int32_t b) {
    while (a != b) {
        if (a <= b)
            b -= a;
        else
            a -= b;
    }
    return a;
}

std::uint32_t PackAverage(std::int32_t b, std::int32_t g,
                          std::int32_t r, std::int32_t a,
                          std::int32_t divisor) {
    return static_cast<std::uint32_t>(b / divisor) |
           (static_cast<std::uint32_t>(g / divisor) << 8) |
           (static_cast<std::uint32_t>(r / divisor) << 16) |
           (static_cast<std::uint32_t>(a / divisor) << 24);
}

}  // namespace

// PostDeviceReset releases the recording render target (captureSurface),
// and nothing recreates it, so a device loss during a recording left the
// recording consumers with a released surface.  Recreate it using the same
// parameters as the initial creation in d3d_init.cpp, then re-point render
// target 0 at it, since IDirect3DDevice9::Reset cleared that binding.
// If creation fails, keep the back-buffer fallback the initial creation also
// uses, so callers stop mirroring instead of rendering to a null target.
// Defect RB-04.
bool EnsureCaptureRenderTarget(MMDApp* app) {
    D3DRenderer* renderer = app->Renderer();
    if (renderer == nullptr || renderer->device == nullptr)
        return false;
    if (renderer->captureSurface != nullptr)
        return true;

    IDirect3DSurface9* surface = nullptr;
    const HRESULT created = renderer->device->CreateRenderTarget(
        static_cast<UINT>(renderer->screenWidth),
        static_cast<UINT>(renderer->screenHeight), renderer->backbufferFormat,
        D3DMULTISAMPLE_NONE, 0, TRUE /*lockable*/, &surface, nullptr);
    if (FAILED(created) || surface == nullptr) {
        if (surface != nullptr)
            surface->Release();
        renderer->multisampleAvailable = 1;
        return false;
    }
    renderer->captureSurface = surface;
    renderer->device->SetRenderTarget(0, surface);
    return true;
}

ULONG DownsampleCaptureSurface(MMDApp* app) {
    D3DRenderer* sub = app->Renderer();
    if (sub == nullptr || sub->device == nullptr)
        return 0;
    auto* device = sub->device;
    const std::int32_t sourceWidth = sub->screenWidth;
    const std::int32_t sourceHeight = sub->screenHeight;
    const auto format = sub->backbufferFormat;
    const std::int32_t targetWidth = app->state.renderW;
    const std::int32_t targetHeight = app->state.renderH;
    // The filter works in place and its GCD requires positive dimensions.
    // Reject invalid sizes before allocation/readback as well as the loops.
    if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 ||
        targetHeight <= 0 || targetWidth > sourceWidth || targetHeight > sourceHeight)
        return 0;

    // Each device reset releases the readback source; nothing can be copied
    // until a recording consumer recreates it, and the loops below would then read
    // through a null surface.
    // Defect RB-04.
    if (sub->captureSurface == nullptr)
        return 0;

    // The plain-surface allocation, the readback, and the lock all went
    // unchecked, so a failure still ran the pixel loops over an unallocated
    // surface or uninitialised bits.  Exit through the failure path instead.
    // Defect RB-05.
    IDirect3DSurface9* systemSurface = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(
            sourceWidth, sourceHeight, format, D3DPOOL_SYSTEMMEM,
            &systemSurface, nullptr)) ||
        systemSurface == nullptr) {
        if (systemSurface != nullptr)
            systemSurface->Release();
        return 0;
    }

    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetRenderTargetData(sub->captureSurface,
                                           systemSurface)) ||
        FAILED(systemSurface->LockRect(&locked, nullptr, 0))) {
        systemSurface->Release();
        return 0;
    }

    const std::int32_t widthGcd =
        GcdBySubtraction(sourceWidth, targetWidth);
    const std::int32_t targetWidthRatio = targetWidth / widthGcd;
    const std::int32_t sourceWidthRatio = sourceWidth / widthGcd;
    const std::int32_t heightGcd =
        GcdBySubtraction(sourceHeight, targetHeight);
    const std::int32_t targetHeightRatio = targetHeight / heightGcd;
    const std::int32_t sourceHeightRatio = sourceHeight / heightGcd;

    std::int32_t phase = 0;
    auto* bits = static_cast<std::uint8_t*>(locked.pBits);
    for (std::int32_t y = 0; y < sourceHeight; ++y) {
        auto* source = bits + y * locked.Pitch;
        auto* destination = source;
        // Start at this row's first pixel. Carrying the final phase from the
        // previous row skips one pixel and eventually reads past the row end.
        phase = 0;
        for (std::int32_t x = 0; x < targetWidth; ++x) {
            std::int32_t b = source[0];
            std::int32_t g = source[1];
            std::int32_t r = source[2];
            std::int32_t a = source[3];
            std::int32_t sumB = 0;
            std::int32_t sumG = 0;
            std::int32_t sumR = 0;
            std::int32_t sumA = 0;
            for (std::int32_t n = 0; n < sourceWidthRatio; ++n) {
                if (phase >= targetWidthRatio) {
                    b = source[4];
                    g = source[5];
                    r = source[6];
                    a = source[7];
                    source += 4;
                    phase = 0;
                }
                sumB += b;
                sumG += g;
                sumR += r;
                sumA += a;
                ++phase;
            }
            *reinterpret_cast<std::uint32_t*>(destination) =
                PackAverage(sumB, sumG, sumR, sumA, sourceWidthRatio);
            destination += 4;
        }
    }

    const std::int32_t pitchPixels = locked.Pitch / 4;
    for (std::int32_t x = 0; x < targetWidth; ++x) {
        auto* source = bits + x * 4;
        auto* destination = source;
        phase = 0;
        for (std::int32_t y = 0; y < targetHeight; ++y) {
            std::int32_t b = source[0];
            std::int32_t g = source[1];
            std::int32_t r = source[2];
            std::int32_t a = source[3];
            std::int32_t sumB = 0;
            std::int32_t sumG = 0;
            std::int32_t sumR = 0;
            std::int32_t sumA = 0;
            for (std::int32_t n = 0; n < sourceHeightRatio; ++n) {
                if (phase >= targetHeightRatio) {
                    source += locked.Pitch;
                    phase = 0;
                    b = source[0];
                    g = source[1];
                    r = source[2];
                    a = source[3];
                }
                sumB += b;
                sumG += g;
                sumR += r;
                sumA += a;
                ++phase;
            }
            *reinterpret_cast<std::uint32_t*>(destination) =
                PackAverage(sumB, sumG, sumR, sumA, sourceHeightRatio);
            destination += pitchPixels * 4;
        }
    }

    systemSurface->UnlockRect();
    RECT sourceRect{0, 0, targetWidth, targetHeight};
    POINT destinationPoint{0, 0};
    device->UpdateSurface(systemSurface, &sourceRect,
                          sub->backbufferSurface,
                          &destinationPoint);
    return systemSurface->Release();
}

}  // namespace mikumikustudio
