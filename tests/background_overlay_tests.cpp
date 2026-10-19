// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include <vfw.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace {
constexpr int width = 160, height = 96;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
std::vector<unsigned char> Pixels(int frame) {
    std::vector<unsigned char> data(width * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto* pixel = &data[((height - 1 - y) * width + x) * 3];
            pixel[0] = static_cast<unsigned char>(40 + frame * 100);
            pixel[1] = static_cast<unsigned char>(32 + y * 2);
            pixel[2] = static_cast<unsigned char>(32 + x);
        }
    return data;
}
struct MediaFiles {
    std::wstring bitmap, video;
    MediaFiles() {
        wchar_t directory[MAX_PATH]{}, name[100]{};
        Check(GetTempPathW(MAX_PATH, directory) > 0, "temporary media directory");
        swprintf_s(name, L"MDS_background_%lu_背景_日本語", GetCurrentProcessId());
        bitmap = std::wstring(directory) + name + L".bmp";
        video = std::wstring(directory) + name + L".avi";
        BITMAPINFOHEADER info{};
        info.biSize = sizeof info; info.biWidth = width; info.biHeight = height;
        info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = width * height * 3;
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42; header.bfOffBits = sizeof header + sizeof info;
        header.bfSize = header.bfOffBits + info.biSizeImage;
        FILE* file = nullptr;
        Check(_wfopen_s(&file, bitmap.c_str(), L"wb") == 0, "create BMP fixture");
        auto pixels = Pixels(0);
        Check(std::fwrite(&header, sizeof header, 1, file) == 1 &&
              std::fwrite(&info, sizeof info, 1, file) == 1 &&
              std::fwrite(pixels.data(), pixels.size(), 1, file) == 1, "write BMP fixture");
        std::fclose(file);
        AVIFileInit();
        PAVIFILE avi = nullptr;
        Check(AVIFileOpenW(&avi, video.c_str(), OF_WRITE | OF_CREATE, nullptr) == 0, "create AVI fixture");
        AVISTREAMINFOW streamInfo{};
        streamInfo.fccType = streamtypeVIDEO; streamInfo.dwScale = 1; streamInfo.dwRate = 30;
        streamInfo.rcFrame = {0, 0, width, height};
        PAVISTREAM stream = nullptr;
        Check(AVIFileCreateStreamW(avi, &stream, &streamInfo) == 0, "create AVI video stream");
        Check(AVIStreamSetFormat(stream, 0, &info, sizeof info) == 0, "set AVI bitmap format");
        for (int frame = 0; frame < 2; ++frame) {
            pixels = Pixels(frame);
            Check(AVIStreamWrite(stream, frame, 1, pixels.data(), static_cast<LONG>(pixels.size()),
                                 AVIIF_KEYFRAME, nullptr, nullptr) == 0, "write AVI frame");
        }
        AVIStreamRelease(stream); AVIFileRelease(avi);
    }
    ~MediaFiles() {
        DeleteFileW(bitmap.c_str()); DeleteFileW(video.c_str()); AVIFileExit();
    }
};

void CheckPixels(MMDApp* app, int frame, float left, float top, float right, float bottom) {
    auto* device = app->Renderer()->device;
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF00FF, 1, 0)), "clear render target");
    Check(SUCCEEDED(device->BeginScene()), "begin background render");
    RenderModelsFixed(app);
    Check(SUCCEEDED(device->EndScene()), "end background render");
    IDirect3DSurface9 *back = nullptr, *read = nullptr;
    Check(SUCCEEDED(device->GetRenderTarget(0, &back)), "get background render target");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(192, 128, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &read, nullptr)), "create background readback");
    Check(SUCCEEDED(device->GetRenderTargetData(back, read)), "read rendered background pixels");
    D3DLOCKED_RECT lock{};
    Check(SUCCEEDED(read->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock rendered background");
    int checked = 0;
    for (int y = static_cast<int>(top) + 5; y < bottom - 5; y += 7) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
        for (int x = static_cast<int>(left) + 5; x < right - 5; x += 7) {
            const float sourceX = (x - left) / (right - left) * width - .5f;
            const float sourceY = (y - top) / (bottom - top) * height - .5f;
            const int red = (row[x] >> 16) & 255, green = (row[x] >> 8) & 255, blue = row[x] & 255;
            if (std::abs(red - (32 + sourceX)) > 4 || std::abs(green - (32 + sourceY * 2)) > 4 ||
                std::abs(blue - (40 + frame * 100)) > 2) {
                std::fprintf(stderr, "pixel (%d,%d)=%08lx expected RGB approx %.1f,%.1f,%d\n",
                             x, y, row[x], 32 + sourceX, 32 + sourceY * 2, 40 + frame * 100);
                Check(false, "both background triangles cover the rectangle with undistorted UVs");
            }
            ++checked;
        }
    }
    const auto* outside = static_cast<const DWORD*>(lock.pBits);
    Check((outside[0] & 0xFFFFFF) == 0xFF00FF, "background placement leaves outside pixels untouched");
    read->UnlockRect(); read->Release(); back->Release();
    std::printf("PASS background frame %d: %d pixel samples\n", frame, checked);
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MediaFiles files;
    HWND window = CreateWindowW(L"STATIC", L"Background media regression", WS_POPUP,
        0, 0, 192, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden render window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get(); app->Hwnd() = window; app->EnglishUI() = 1;
    auto& pp = renderer->presentParameters;
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 192; pp.BackBufferHeight = 128; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &renderer->device)), "create device");
    auto* device = renderer->device;
    renderer->viewScale = 1.2f;
    app->ViewportRect() = {8, 10, 168, 106};
    for (auto** buffer : {&app->PictureOverlayVertices(), &app->AviOverlayVertices()}) {
        Check(SUCCEEDED(device->CreateVertexBuffer(168, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_MANAGED, buffer, nullptr)), "create background vertices");
        void* data = nullptr; (*buffer)->Lock(0, 0, &data, 0);
        std::memset(data, 0, 168); (*buffer)->Unlock();
    }
    wcscpy_s(app->PictureBackgroundPath(), 256, files.bitmap.c_str());
    LoadBackgroundPicture(app.get());
    Check(app->PictureBackgroundEnabled() && app->PictureWidth() == width && app->PictureHeight() == height,
          "actual BMP loader reads dimensions and enables background");
    CheckPixels(app.get(), 0, 7, 9, 168, 106);
    app->PictureOffsetX() = 20; app->PictureOffsetY() = 15; app->PictureScale() = .5f;
    PicBgOverlayRefresh(app.get());
    CheckPixels(app.get(), 0, 27, 24, 108, 73);
    app->PictureBackgroundEnabled() = 0;
    wcscpy_s(app->AviBackgroundPath(), 256, files.video.c_str());
    LoadAviFile(app.get());
    Check(app->AviBackgroundEnabled() && app->AviFrameWidth() == width && app->AviFrameHeight() == height,
          "actual AVI loader reads dimensions and enables background");
    CheckPixels(app.get(), 0, 7, 9, 168, 106);
    app->CurrentFrame() = 1; AviBgOverlayRefresh(app.get());
    CheckPixels(app.get(), 1, 7, 9, 168, 106);
    app->AviOffsetX() = 20; app->AviOffsetY() = 15; app->AviScale() = .5f;
    // Placement must reuse the displayed frame, even if the timeline changed.
    app->CurrentFrame() = 0;
    AviBgPlacementRefresh(app.get());
    CheckPixels(app.get(), 1, 27, 24, 108, 73);
    AVIStreamGetFrameClose(static_cast<PGETFRAME>(app->AviFrameReader()));
    AVIStreamRelease(static_cast<PAVISTREAM>(app->AviStream()));
    AVIFileRelease(static_cast<PAVIFILE>(app->AviFile()));
    if (app->AviDrawDib()) DrawDibClose(static_cast<HDRAWDIB>(app->AviDrawDib()));
    app->PictureBackgroundTexture()->Release(); app->AviBackgroundTexture()->Release();
    app->AviBackgroundSurface()->Release();
    app->PictureOverlayVertices()->Release(); app->AviOverlayVertices()->Release();
    device->Release(); renderer->device = nullptr; d3d->Release(); DestroyWindow(window);
    std::puts("Background BMP/AVI loading, frame advance, placement and complete-quad pixel regressions passed.");
}
