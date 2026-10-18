#include "mikudancestudio/mmv4mms.hpp"
#include "mikudancestudio/viewport_render_scale.hpp"
#include "mikudancestudio/transparent_capture.hpp"
#include "mikudancestudio/image_sequence_export.hpp"
#include "mikudancestudio/native_mesh.hpp"
#include "mikudancestudio/deformed_mesh_capture.hpp"
#include "mikudancestudio/mesh_bake_cache.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/perf_timing.hpp"
#include "mikudancestudio/d3d12_native.hpp"
// ===========================================================================
// VA 0x0046DC00..0x0046E787 - FrameDriver D3D scene envelope
// ===========================================================================
// This is the non-model shell of sub_46B090's render stage: exact clear
// selection, scene lifetime, and the three dynamic overlay batches.  The
// model/shadow/background helpers remain separate nodes in the recovered
// call graph and are intentionally not approximated here.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/frame_presentation.hpp"
#include "mikudancestudio/frame_rate.hpp"
#include "mikudancestudio/d3d_object_guard.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/transparent_triangles.hpp"
#endif
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/effect_api.hpp"
#include "mikudancestudio/default_effect.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/shadow_filter.hpp"

namespace mikudancestudio {
namespace {

struct TexturedScreenVertex {
    float x;
    float y;
    float z;
    float reciprocalW;
    D3DCOLOR color;
    float u;
    float v;
};

struct ColoredScreenVertex {
    float x;
    float y;
    float z;
    float reciprocalW;
    D3DCOLOR color;
};

constexpr DWORD kTexturedScreenVertexFvf =
    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr DWORD kColoredScreenVertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

static_assert(sizeof(TexturedScreenVertex) == 28);
static_assert(sizeof(ColoredScreenVertex) == 20);

void DrawTextOverlay(MMDApp* app, IDirect3DDevice9* device) {
    const UINT count = app->TextOverlayPrimitiveCount();
    auto* vb = app->OverlayVertices();
    if (count == 0 || vb == nullptr)
        return;

    device->SetTexture(0, app->TextOverlayTexture());
    device->SetStreamSource(0, vb, 0, sizeof(TexturedScreenVertex));
    device->SetFVF(kTexturedScreenVertexFvf);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, count);
}

void DrawLineOverlay(MMDApp* app, D3DRenderer* sub,
                     IDirect3DDevice9* device) {
    const UINT count = app->LineOverlayPrimitiveCount();
    // The renderer owns the selection and guide-line vertex buffer.
    auto* vb = sub->lineVertexBuffer;
    if (count == 0 || vb == nullptr)
        return;

    // Lines contain diffuse colors but no UVs. The preceding text draw leaves
    // its mostly transparent font atlas bound, which can erase the entire V.
    device->SetTexture(0, nullptr);
    device->SetStreamSource(0, vb, 0, sizeof(ColoredScreenVertex));
    device->SetFVF(kColoredScreenVertexFvf);
    device->DrawPrimitive(D3DPT_LINELIST, 0, count);
}

void DrawSpriteOverlay(MMDApp* app, IDirect3DDevice9* device, bool presentation) {
    const UINT count = app->SpriteOverlayPrimitiveCount();
    auto* vb = app->SpriteOverlayVertices();
    if (count == 0 || vb == nullptr)
        return;

    // Disabled scale controls reuse the icon alpha as a clipping mask. D3D9's
    // default alpha SELECTARG1 ignores diffuse alpha, so set both operations
    // explicitly and restore the caller's texture-stage state afterwards.
    const D3DTEXTURESTAGESTATETYPE stages[] = {D3DTSS_COLOROP, D3DTSS_COLORARG1,
        D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2};
    const DWORD values[] = {D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE,
        D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE};
    DWORD saved[6]{};
    for (int i=0; i<6; ++i) {
        device->GetTextureStageState(0, stages[i], &saved[i]);
        device->SetTextureStageState(0, stages[i], values[i]);
    }
    // MME effects may leave point filtering active. Smooth the HUD atlas at
    // its screen size, then restore the caller's sampler with its other state.
    const D3DSAMPLERSTATETYPE filters[] = {
        D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER};
    const DWORD filterValues[] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE};
    DWORD savedFilters[3]{};
    for (int i = 0; i < 3; ++i) {
        device->GetSamplerState(0, filters[i], &savedFilters[i]);
        device->SetSamplerState(0, filters[i], filterValues[i]);
    }
    device->SetTexture(0, app->OverlayTexture());
    device->SetStreamSource(0, vb, 0, sizeof(TexturedScreenVertex));
    device->SetFVF(kTexturedScreenVertexFvf);
    if (presentation) {
        const RECT scene = app->ViewportRect(), frame = FrameOverlayRect(app);
        const UINT first = std::min(count, app->hudSprites.first);
        const UINT end = first + std::min(count - first, app->hudSprites.count);
        device->SetScissorRect(&scene);
        if (first) device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, first);
        device->SetScissorRect(&frame);
        if (end > first) device->DrawPrimitive(D3DPT_TRIANGLELIST, first * 3, end - first);
        device->SetScissorRect(&scene);
        if (count > end) device->DrawPrimitive(D3DPT_TRIANGLELIST, end * 3, count - end);
    } else {
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, count);
    }
    for (int i=0; i<6; ++i) device->SetTextureStageState(0, stages[i], saved[i]);
    for (int i = 0; i < 3; ++i) device->SetSamplerState(0, filters[i], savedFilters[i]);
}

void DrawFullscreenQuad(IDirect3DDevice9* device,
                        IDirect3DBaseTexture9* texture,
                        IDirect3DVertexBuffer9* vertices) {
    device->SetTexture(0, texture);
    device->SetStreamSource(0, vertices, 0, 28);
    device->SetFVF(0x144);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
}

// Porting-era screen-texture readback under
// MIKUDANCESTUDIO_SCREEN_TEXTURE_DUMP_DIR (CMake option MIKUDANCESTUDIO_DIAG,
// default OFF); the OFF stub below keeps the call sites valid and inlines
// away to nothing.
#ifdef MIKUDANCESTUDIO_DIAG
#pragma pack(push, 1)
struct DumpBitmapFileHeader {
    std::uint16_t type;
    std::uint32_t size;
    std::uint16_t reserved1;
    std::uint16_t reserved2;
    std::uint32_t bitsOffset;
};
#pragma pack(pop)

// Test-only readback of the texture exposed to screen.bmp.  GetRenderTargetData
// is deliberately issued after EndScene; doing it inside 0x46E025 changes the
// D3D command boundary and can fail on real D3D9 drivers.
void DumpAccessoryScreenTexture(MMDApp* app, IDirect3DDevice9* device) {
    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_SCREEN_TEXTURE_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    char stableCapture[2]{};
    if (GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_STABLE_CAPTURE", stableCapture,
                                sizeof(stableCapture)) == 1 &&
        stableCapture[0] == '1') {
        char gatePath[MAX_PATH]{};
        std::snprintf(gatePath, sizeof(gatePath), "%s\\vb.capture.ready",
                      directory);
        const DWORD attributes = GetFileAttributesA(gatePath);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return;
    }

    static LONG dumped = 0;
    if (InterlockedCompareExchange(&dumped, 1, 0) != 0)
        return;

    IDirect3DTexture9* texture = app->CaptureTexture();
    if (texture == nullptr)
        return;

    D3DSURFACE_DESC description{};
    IDirect3DSurface9* source = nullptr;
    IDirect3DSurface9* system = nullptr;
    D3DLOCKED_RECT locked{};
    bool isLocked = false;
    if (FAILED(texture->GetLevelDesc(0, &description)) ||
        FAILED(texture->GetSurfaceLevel(0, &source)) || source == nullptr ||
        FAILED(device->CreateOffscreenPlainSurface(
            description.Width, description.Height, description.Format,
            D3DPOOL_SYSTEMMEM, &system, nullptr)) || system == nullptr ||
        FAILED(device->GetRenderTargetData(source, system)) ||
        FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        if (system != nullptr)
            system->Release();
        if (source != nullptr)
            source->Release();
        return;
    }
    isLocked = true;

    CreateDirectoryA(directory, nullptr);
    char rawPath[MAX_PATH]{};
    char bitmapPath[MAX_PATH]{};
    char metaPath[MAX_PATH]{};
    std::snprintf(rawPath, sizeof(rawPath), "%s\\screen_texture.bgra",
                  directory);
    std::snprintf(bitmapPath, sizeof(bitmapPath), "%s\\screen_texture.bmp",
                  directory);
    std::snprintf(metaPath, sizeof(metaPath), "%s\\screen_texture.meta.json",
                  directory);

    const DWORD rowBytes = description.Width * 4;
    FILE* stream = nullptr;
    if (fopen_s(&stream, rawPath, "wb") == 0 && stream != nullptr) {
        for (UINT y = 0; y < description.Height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(locked.pBits) +
                              y * locked.Pitch;
            std::fwrite(row, 1, rowBytes, stream);
        }
        std::fclose(stream);
    }

    if (fopen_s(&stream, bitmapPath, "wb") == 0 && stream != nullptr) {
        const DWORD pixelBytes = rowBytes * description.Height;
        DumpBitmapFileHeader fileHeader{
            0x4D42, static_cast<DWORD>(sizeof(DumpBitmapFileHeader) +
                                      sizeof(BITMAPINFOHEADER) + pixelBytes),
            0, 0, static_cast<DWORD>(sizeof(DumpBitmapFileHeader) +
                                     sizeof(BITMAPINFOHEADER))};
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(description.Width);
        info.biHeight = static_cast<LONG>(description.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = pixelBytes;
        std::fwrite(&fileHeader, 1, sizeof(fileHeader), stream);
        std::fwrite(&info, 1, sizeof(info), stream);
        for (UINT y = description.Height; y-- > 0;) {
            const auto* row = static_cast<const std::uint8_t*>(locked.pBits) +
                              y * locked.Pitch;
            std::fwrite(row, 1, rowBytes, stream);
        }
        std::fclose(stream);
    }

    if (fopen_s(&stream, metaPath, "wb") == 0 && stream != nullptr) {
        const int mode = static_cast<int>(app->CaptureMode());
        RECT capture = app->RecordingWindow() != nullptr
            ? RECT{0, 0, app->RenderWidth(), app->RenderHeight()}
            : app->ViewportRect();
        if (mode == 2) {
            const double width =
                static_cast<double>(capture.right - capture.left);
            const double height =
                static_cast<double>(capture.bottom - capture.top);
            const double targetWidth = height * 4.0 / 3.0;
            if (targetWidth < width) {
                const double center =
                    static_cast<double>(capture.left + capture.right) * 0.5;
                capture.left = static_cast<LONG>(center - targetWidth * 0.5);
                capture.right = static_cast<LONG>(center + targetWidth * 0.5);
            } else {
                const double targetHeight = width * 3.0 / 4.0;
                const double center =
                    static_cast<double>(capture.top + capture.bottom) * 0.5;
                capture.top =
                    static_cast<LONG>(center - targetHeight * 0.5);
                capture.bottom =
                    static_cast<LONG>(center + targetHeight * 0.5);
            }
        }
        std::fprintf(stream,
            "{\"schema\":2,\"width\":%u,\"height\":%u,"
            "\"format\":%u,\"pitch\":%d,\"mode\":%d,"
            "\"source_rect\":[%ld,%ld,%ld,%ld]}\n",
            description.Width, description.Height,
            static_cast<unsigned>(description.Format), locked.Pitch,
            mode, capture.left, capture.top, capture.right, capture.bottom);
        std::fclose(stream);
    }

    if (isLocked)
        system->UnlockRect();
    system->Release();
    source->Release();
}
#else
inline void DumpAccessoryScreenTexture(MMDApp*, IDirect3DDevice9*) {}
#endif

void ComposeSelfShadow(MMDApp* app, D3DRenderer* sub,
                       IDirect3DDevice9* device) { // 0x46DE61
    if (sub->effect == nullptr || app->SelfShadowCompositionEnabled() == 0 ||
        app->SelfShadowMode() <= 0)
        return;
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    DrawFullscreenQuad(device,
        sub->hdrTexture,
        app->LeftViewportVertices());
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
}

void ComposeCallbackTexture(MMDApp* app,
                            IDirect3DDevice9* device) { // 0x46DF2A
    if (app->DepthTextureCompositionEnabled() == 0 ||
        app->DepthDeviceEnabled() == 0 ||
        app->FrameStepPlayback() != 0)
        return;
    DepthTextureProvider callback = app->DepthTextureCallback();
    if (callback == nullptr)
        return;
    IDirect3DBaseTexture9* texture = nullptr;
    callback(&texture);
    DrawFullscreenQuad(device, texture,
        app->RightViewportVertices());
}

void CaptureAccessoryScreenTexture(MMDApp* app, D3DRenderer* sub,
                                   IDirect3DDevice9* device) { // 0x46E025
    const int mode = static_cast<int>(app->CaptureMode());
    if (mode != 1 && mode != 2)
        return;
    auto*& texture = app->CaptureTexture();
    if (texture == nullptr) {
        HRESULT result = device->CreateTexture(1024, 1024, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
            &texture, nullptr);
        if (result == D3DERR_OUTOFVIDEOMEMORY || result == E_OUTOFMEMORY)
            device->CreateTexture(512, 512, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr);
        if (texture == nullptr)
            return;
    }

    IDirect3DSurface9* textureSurface = nullptr;
    texture->GetSurfaceLevel(0, &textureSurface);
    auto** backBuffer = &sub->backbufferSurface;
    if (*backBuffer == nullptr)
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer);
    if (mode == 1 && textureSurface != nullptr && *backBuffer != nullptr) {
        const RECT* sourceRect;
        RECT captureRect{};
        if (app->RecordingWindow() != nullptr) {
            captureRect.right = app->RenderWidth();
            captureRect.bottom = app->RenderHeight();
            sourceRect = &captureRect;
        } else {
            sourceRect = &app->ViewportRect();
        }
        if (FAILED(device->StretchRect(*backBuffer, sourceRect,
                                       textureSurface, nullptr,
                                       D3DTEXF_LINEAR)))
            device->StretchRect(*backBuffer, sourceRect, textureSurface,
                                nullptr, D3DTEXF_NONE);
    } else if (mode == 2 && textureSurface != nullptr &&
               *backBuffer != nullptr) {
        RECT source = app->RecordingWindow() != nullptr
            ? RECT{0, 0, app->RenderWidth(), app->RenderHeight()}
            : app->ViewportRect();
        const double width = static_cast<double>(source.right - source.left);
        const double height = static_cast<double>(source.bottom - source.top);
        const double targetWidth = height * 4.0 / 3.0;
        if (targetWidth < width) {
            const double center =
                static_cast<double>(source.left + source.right) * 0.5;
            source.left = static_cast<LONG>(center - targetWidth * 0.5);
            source.right = static_cast<LONG>(center + targetWidth * 0.5);
        } else {
            const double targetHeight = width * 3.0 / 4.0;
            const double center =
                static_cast<double>(source.top + source.bottom) * 0.5;
            source.top = static_cast<LONG>(center - targetHeight * 0.5);
            source.bottom = static_cast<LONG>(center + targetHeight * 0.5);
        }
        if (FAILED(device->StretchRect(*backBuffer, &source,
                                       textureSurface, nullptr,
                                       D3DTEXF_LINEAR)))
            device->StretchRect(*backBuffer, &source, textureSurface,
                                nullptr, D3DTEXF_NONE);
    }
    if (textureSurface != nullptr)
        textureSurface->Release();
}

void DrawOverlayBatches(MMDApp* app, IDirect3DDevice9* device, bool presentation = false) {
    auto* sub = app->Renderer();
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // Draw the sprite controls first; the coordinate-space caption is part
    // of the text batch and must remain on top of the atlas background.
    DrawSpriteOverlay(app, device, presentation);
    const RECT scene = app->ViewportRect(), frame = FrameOverlayRect(app);
    if (presentation) device->SetScissorRect(&scene);
    DrawLineOverlay(app, sub, device);
    if (presentation) device->SetScissorRect(&frame);
    DrawTextOverlay(app, device);
}

#if !MMD_ORIGINAL_PROFILE
void DrawPresentationOverlays(MMDApp* app) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDirect3DSurface9> back, previous, depth;
    ComPtr<IDirect3DDevice9> native;
    if (FAILED(app->Renderer()->device->GetBackBuffer(0, 0,
            D3DBACKBUFFER_TYPE_MONO, &back)) || FAILED(back->GetDevice(&native))) return;
    // This pass belongs to the completed display frame. Calling BeginScene on
    // the MME proxy would run another effect frame and overwrite its result.
    auto* device = native.Get();
    ComPtr<IDirect3DStateBlock9> saved;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) ||
        FAILED(device->GetRenderTarget(0, &previous))) return;
    device->GetDepthStencilSurface(&depth);
    device->SetDepthStencilSurface(nullptr);
    HRESULT hr = device->SetRenderTarget(0, back.Get());
    const RECT rect = FrameOverlayRect(app);
    const D3DVIEWPORT9 viewport{DWORD(rect.left), DWORD(rect.top),
        DWORD(rect.right - rect.left), DWORD(rect.bottom - rect.top), 0, 1};
    if (SUCCEEDED(hr)) hr = device->SetViewport(&viewport);
    if (SUCCEEDED(hr)) hr = device->BeginScene();
    if (SUCCEEDED(hr)) {
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFFu);
        device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
        device->SetScissorRect(&rect);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        DrawOverlayBatches(app, device, true);
        hr = device->EndScene();
    }
    device->SetRenderTarget(0, previous.Get());
    device->SetDepthStencilSurface(depth.Get());
    // SetRenderTarget resets the viewport; restore the state block last.
    saved->Apply();
    if (FAILED(hr)) runtime_log::Write("HUD presentation failed hr=0x%08lX", hr);
}
#endif

}  // namespace

void DrawFrameOverlayBatches(MMDApp* app) {
    auto* sub = app->Renderer();
    if (sub && sub->device) DrawOverlayBatches(app, sub->device);
}

void RenderFrameScene(MMDApp* app) {
    frame_rate::BeginScene(app);
    MMD_PERF_SCOPE(Scene);
    shadow_filter::Invalidate(app);
    D3DRenderer* sub = app->Renderer();
    if (sub == nullptr)
        return;

    IDirect3DDevice9* device = sub->device;
    if (device == nullptr)
        return;

    antialias::PrepareFrame(app);
    UpdateDefaultEffect(app);

    for (int slot=0; slot<kModelSlotCount; ++slot)
        if (auto* model=app->ModelSlot(slot)) bone_scale::ApplyTransforms(model);
    SetupFrameWorldTransform(app);
    UpdateFrameStereo(app);
    PrepareFrameSpriteOverlay(app); // 0x46BD79, after transform setup
    D3DMATRIX frameWorld{};
    device->GetTransform(D3DTS_WORLD, &frameWorld);
    deformed_mesh_capture::SetCamera(device, reinterpret_cast<const float*>(&frameWorld));

    // 0x46DB41: the whole block - per-model VB update (inner gate
    // app+0xA0665 == 0) AND the Clear - runs only when app+0xA0D6C
    // (messageSeen) != 0; frames without a message leave both untouched.
    if (app->state.messageSeen == 0)
        return;
    // The optional native preview bypasses DX9/MME.
    native_mesh::Initialize(app);
    mmv4mms::Initialize(app);
    const bool transparent = TransparentExportActive(app);
    if (!mmv4mms::Active() && !transparent && !NativeBakePreviewActive() && !deformed_mesh_capture::Active() && mesh_bake_cache::Empty() && RenderD3D12Native(app, reinterpret_cast<const float*>(&frameWorld))) {
        frame_rate::CompleteScene(app);
        frame_rate::Presented(app, true);
        return;
    }
    // Device reset can release the legacy recording target. Establish it
    // before the shadow pass snapshots the active scene target.
    if (!transparent && app->RecordingWindow() && sub->multisampleAvailable == 0)
        EnsureCaptureRenderTarget(app);
    TransparentFrameScope transparentFrame(app);
    if (!transparentFrame.Ready()) {
        FinishAviRecord(app);
        app->state.timelineAdvanceDue=app->state.timelineAdvanceRequested=0;
        MessageBoxW(static_cast<HWND>(app->Hwnd()), image_sequence::Text(app,
            L"无法创建透明背景的渲染目标，本次导出已取消。",
            L"Could not create a transparent render target. Export was canceled.",
            L"透明なレンダーターゲットを作成できません。出力を中止しました。"), L"MMS", MB_OK | MB_ICONERROR);
        return;
    }
    D3dObjectValidationScope validatedObjects;
#if !MMD_ORIGINAL_PROFILE
    TransparentSortFrameCache transparentSortCache(
        app->AutoTransparentMaterialSortingEnabled() && GetModuleHandleW(L"MMEffect.dll") != nullptr);
#endif
    if (app->state.accessoryEditDialogOpen == 0) {
        for (int slot = 0; slot < kModelSlotCount; ++slot) {
            auto* model = app->ModelSlot(slot);
            if (model != nullptr)
                UpdateModelVertexBuffers(
                    app, model, reinterpret_cast<const float*>(&frameWorld));
        }
    }

    // Apply native mesh caches before MME caches object data.
    if(!NativeBakePreviewActive())native_mesh::Evaluate(app, reinterpret_cast<const float*>(&frameWorld));

    viewport_render_scale::FrameScope previewScale(app);
    if (!previewScale.Ready()) return;
    antialias::FrameScope antialiasFrame(app);
    if (!antialiasFrame.Ready()) return;

    DWORD clearFlags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;
    if (sub->d3dInitialized != 0)
        clearFlags |= D3DCLEAR_STENCIL;
    // Clear the base scene to the user's background choice.
    const D3DCOLOR clearColor = transparent ? 0 :
        app->state.blackBackgroundEnabled != 0
            ? D3DCOLOR_XRGB(0, 0, 0)
            : D3DCOLOR_XRGB(255, 255, 255);
    { MMD_PERF_SCOPE(Clear); device->Clear(0, nullptr, clearFlags, clearColor, 1.0f, 0); }

    // A completed MMV4MMS frame replaces the native scene before BeginScene.
    // The usual capture and UI composition below consume this same texture.
    const int mmvResult = NativeBakePreviewActive() ? 0 :
        mmv4mms::Render(app, reinterpret_cast<const float*>(&frameWorld));
    if (mmvResult < 0) return; // Do not present a stale or mixed renderer frame.
    app->state.renderPassCount = 1;
    // MME caches both material-name languages during its first BeginScene.
    InstallEffectMaterialNameCompatibility();
    BeginEffectObjectRegistration();
    // Frame-stage phases: a fault inside Direct3D then reports which stage of
    // the frame was running (the crash window shows the phase name).
    runtime_log::SetPhase("frame: BeginScene");

    {
        MMD_PERF_SCOPE(BeginScene);
        if (FAILED(device->BeginScene())) return;
    }
    InstallEffectMenuCompatibility(app);

    const bool effectRenderer = mmvResult == 0 && sub->effect != nullptr &&
        UseEffectModelRenderer(app);
    if (effectRenderer) {
        runtime_log::SetPhase("frame: shadow map");
        RenderShadowMap(app, reinterpret_cast<const float*>(&frameWorld));
    }

    // 0x46DDF6..0x46DE5F is deliberately a loop: render callbacks may add
    // another pass by incrementing A0270 while a pass is in progress.
    runtime_log::SetPhase(effectRenderer ? "frame: models effect draw"
                                         : "frame: models fixed draw");
    while (mmvResult == 0 && app->state.renderPassCount > 0) {
        --app->state.renderPassCount;
        if (effectRenderer)
            RenderModelsEffect(app,
                reinterpret_cast<const float*>(&frameWorld));
        else
            RenderModelsFixed(app);
    }

    runtime_log::SetPhase("frame: compose self shadow");

    {
        MMD_PERF_SCOPE(Composition);
        if (mmvResult == 0) {
            ComposeSelfShadow(app, sub, device);
            ComposeCallbackTexture(app, device);
        } else if (!mmv4mms::Present(app)) {
            device->EndScene();
            return;
        }
    }

    // 0x46DFDE: mirror the full-size capture RT into the secondary-window
    // back buffer. The original reacquires the back buffer after a reset.
    // A reset also releases the capture target, so recreate it before
    // the mirror; if that fails, skip the mirror rather than read a
    // released surface.
    // Defect RB-04.
    if (!transparent && sub->multisampleAvailable == 0 &&
        app->RecordingWindow() != nullptr &&
        EnsureCaptureRenderTarget(app)) {
        auto** backBufferSlot = &sub->backbufferSurface;
        if (*backBufferSlot == nullptr)
            device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                                  backBufferSlot);
        DownsampleCaptureSurface(app);
    }

    { MMD_PERF_SCOPE(ScreenCapture); CaptureAccessoryScreenTexture(app, sub, device); }

    // 0x46E208: the recording/sub-window path ends the scene here; it never
    // draws debug geometry or the dynamic overlay batches.
    if (app->RecordingWindow() != nullptr) {
        device->EndScene();
        if (antialiasFrame.Complete()) {
            if (!transparent && sub->multisampleAvailable == 0 && EnsureCaptureRenderTarget(app))
                DownsampleCaptureSurface(app);
            CaptureAccessoryScreenTexture(app, sub, device);
        }

        DumpAccessoryScreenTexture(app, device);
        return;
    }

    // 0x46E214..0x46E554: accessory-dialog or Bullet collision debug pass.
    if (app->state.rigidBodyDisplayEnabled == 1 ||
        app->state.frameCopyDialog != 0) {
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        if (app->state.frameCopyDialog != 0)
            DrawAccessoryDebug(app);
        else
            DrawPhysicsCollisionDebug(app->Physics());
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_LIGHTING, TRUE);
    }

    // 0x46E556..0x46E592: selected-bone local/global operation axis.
    if (app->state.optflag[0] == 0 &&
        app->PlaybackActive() == 0 &&
        app->state.frameCopyDialog == 0) {
        DrawBoneOperationAxis(app,
            reinterpret_cast<const float*>(&frameWorld));
    }

    {
        MMD_PERF_SCOPE(Overlays);
#if MMD_ORIGINAL_PROFILE
        DrawFrameOverlayBatches(app);
        FillFramePresentationMargins(app, device);
#endif
    }
    { MMD_PERF_SCOPE(EndScene); if (SUCCEEDED(device->EndScene())) {
        antialiasFrame.Complete();
        if (previewScale.Complete()) {
#if !MMD_ORIGINAL_PROFILE
            // Keep text and controls out of scene scaling and temporal history.
            // FPS still comes from FrameDriver's one-count-per-scene counter.
            MMD_PERF_SCOPE(Overlays);
            FillFramePresentationMargins(app, device);
            DrawPresentationOverlays(app);
#endif
            if(antialiasFrame.Succeeded())frame_rate::CompleteScene(app);
        }
    } }

    DumpAccessoryScreenTexture(app, device);
}

}  // namespace mikudancestudio
