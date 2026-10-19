// MAINTENANCE: regression-keep; capture pixels and invalid-size handling.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
// Independent area-overlap oracle. Each stage rounds down separately, as the
// capture format requires; no shared phase/pointer algorithm with production.
std::vector<DWORD> Resize(const std::vector<DWORD>& input, int sw, int sh, int tw, int th) {
    std::vector<DWORD> horizontal(tw * sh), output(tw * th);
    for (int y = 0; y < sh; ++y) for (int x = 0; x < tw; ++x) {
        DWORD value = 0;
        for (int channel = 0; channel < 4; ++channel) {
            int sum = 0;
            for (int sx = 0; sx < sw; ++sx) {
                int weight = std::max(0, std::min((x + 1) * sw, (sx + 1) * tw) - std::max(x * sw, sx * tw));
                sum += ((input[y * sw + sx] >> (8 * channel)) & 255) * weight;
            }
            value |= DWORD(sum / sw) << (8 * channel);
        }
        horizontal[y * tw + x] = value;
    }
    for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) {
        DWORD value = 0;
        for (int channel = 0; channel < 4; ++channel) {
            int sum = 0;
            for (int sy = 0; sy < sh; ++sy) {
                int weight = std::max(0, std::min((y + 1) * sh, (sy + 1) * th) - std::max(y * sh, sy * th));
                sum += ((horizontal[sy * tw + x] >> (8 * channel)) & 255) * weight;
            }
            value |= DWORD(sum / sh) << (8 * channel);
        }
        output[y * tw + x] = value;
    }
    return output;
}
void Run(MMDApp* app, int sw, int sh, int tw, int th, bool invalid = false) {
    auto* r = app->Renderer();
    auto* device = r->device;
    r->screenWidth = sw; r->screenHeight = sh;
    r->backbufferFormat = D3DFMT_A8R8G8B8;
    app->RenderWidth() = tw; app->RenderHeight() = th;
    Check(SUCCEEDED(device->CreateRenderTarget(sw, sh, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, TRUE, &r->captureSurface, nullptr)), "source surface");
    Check(SUCCEEDED(device->CreateRenderTarget(sw, sh, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, TRUE, &r->backbufferSurface, nullptr)), "destination surface");
    std::vector<DWORD> pixels(sw * sh);
    for (int y = 0; y < sh; ++y) for (int x = 0; x < sw; ++x)
        pixels[y * sw + x] = 0xFF000000u | DWORD((y * 23 + x * 7) % 256) |
            (DWORD((y * 11 + x * 19) % 256) << 8) | (DWORD((y * 31 + x * 3) % 256) << 16);
    D3DLOCKED_RECT lock{};
    Check(SUCCEEDED(r->captureSurface->LockRect(&lock, nullptr, 0)), "lock source");
    for (int y = 0; y < sh; ++y)
        std::copy_n(pixels.data() + y * sw, sw, reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits) + y * lock.Pitch));
    r->captureSurface->UnlockRect();
    Check(SUCCEEDED(device->ColorFill(r->backbufferSurface, nullptr, 0xFF123456u)), "fill destination");
    DownsampleCaptureSurface(app);
    Check(SUCCEEDED(r->backbufferSurface->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "read destination");
    const auto expected = invalid ? std::vector<DWORD>(sw * sh, 0xFF123456u) : Resize(pixels, sw, sh, tw, th);
    const int width = invalid ? sw : tw, height = invalid ? sh : th;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
        if (row[x] != expected[y * width + x]) {
            std::fprintf(stderr, "%dx%d -> %dx%d (%d,%d) actual=%08lX expected=%08lX\n", sw, sh, tw, th, x, y, row[x], expected[y * width + x]);
            Check(false, "capture filter matches independent area oracle");
        }
    }
    r->backbufferSurface->UnlockRect();
    r->captureSurface->Release(); r->captureSurface = nullptr;
    r->backbufferSurface->Release(); r->backbufferSurface = nullptr;
}
}
int main() {
    HWND window = CreateWindowW(L"STATIC", L"capture regression", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "D3D9");
    auto app = std::make_unique<MMDApp>();
    auto renderer = std::make_unique<D3DRenderer>();
    app->state = MMDAppState{};
    *renderer = D3DRenderer{};
    app->Renderer() = renderer.get();
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &pp, &renderer->device)), "GPU device");
    Run(app.get(), 16, 8, 16, 8);
    Run(app.get(), 16, 8, 8, 4);
    Run(app.get(), 15, 9, 10, 6);
    Run(app.get(), 16, 8, 1, 1);
    for (const auto size : {std::pair{0, 4}, {-1, 4}, {4, 0}, {4, -1}, {17, 4}, {4, 9}})
        Run(app.get(), 16, 8, size.first, size.second, true);
    renderer->device->Release(); d3d->Release(); DestroyWindow(window);
    std::puts("capture filter and invalid dimensions: PASS");
}
