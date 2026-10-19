#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace mikumikustudio::viewport_render_scale {
using Microsoft::WRL::ComPtr;
namespace {
struct Target {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> resolved, color, depth;
    D3DSURFACE_DESC source{};
    Size size{};
};
std::unordered_map<MMDApp*, Target> targets;
MMDApp* rendering = nullptr;

HRESULT EnsureTarget(MMDApp* app, Size size) {
    auto* device = app->Renderer()->device;
    ComPtr<IDirect3DSurface9> back, depth;
    HRESULT hr = device->GetRenderTarget(0, &back);
    if (FAILED(hr)) return hr;
    D3DSURFACE_DESC desc{}, depthDesc{};
    if (FAILED(hr = back->GetDesc(&desc))) return hr;
    if (FAILED(hr = device->GetDepthStencilSurface(&depth))) return hr;
    if (FAILED(hr = depth->GetDesc(&depthDesc))) return hr;
    const auto found = targets.find(app);
    if (found != targets.end()) {
        const auto& t = found->second;
        if (t.size.width == size.width && t.size.height == size.height &&
            t.source.Format == desc.Format && t.source.MultiSampleType == desc.MultiSampleType &&
            t.source.MultiSampleQuality == desc.MultiSampleQuality) return S_OK;
    }
    Target next;
    next.size = size; next.source = desc;
    hr = device->CreateTexture(size.width, size.height, 1, D3DUSAGE_RENDERTARGET,
        desc.Format, D3DPOOL_DEFAULT, &next.texture, nullptr);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = next.texture->GetSurfaceLevel(0, &next.resolved))) return hr;
    if (desc.MultiSampleType == D3DMULTISAMPLE_NONE) next.color = next.resolved;
    else {
        hr = device->CreateRenderTarget(size.width, size.height, desc.Format,
            desc.MultiSampleType, desc.MultiSampleQuality, FALSE, &next.color, nullptr);
        if (FAILED(hr)) return hr;
    }
    hr = device->CreateDepthStencilSurface(size.width, size.height, depthDesc.Format,
        desc.MultiSampleType, desc.MultiSampleQuality, TRUE, &next.depth, nullptr);
    if (FAILED(hr)) return hr;
    targets[app] = std::move(next);
    return S_OK;
}

HRESULT Composite(IDirect3DDevice9* device, Target& target, const RECT& rect) {
    // Surface ownership supplies the native D3D device. A proxy's second
    // BeginScene starts another MME frame and can replace the completed image;
    // this presentation-only blit must not enter the effect scene again.
    ComPtr<IDirect3DDevice9> owner;
    const HRESULT ownership = target.color->GetDevice(&owner);
    if (FAILED(ownership)) return ownership;
    device = owner.Get();
    if (target.color.Get() != target.resolved.Get()) {
        const HRESULT hr = device->StretchRect(target.color.Get(), nullptr,
            target.resolved.Get(), nullptr, D3DTEXF_NONE);
        if (FAILED(hr)) return hr;
    }
    ComPtr<IDirect3DStateBlock9> saved;
    HRESULT hr = device->CreateStateBlock(D3DSBT_ALL, &saved);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = device->BeginScene())) return hr;
    struct Vertex { float x, y, z, rhw, u, v; };
    const float l = float(rect.left) - .5f, t = float(rect.top) - .5f;
    const float r = float(rect.right) - .5f, b = float(rect.bottom) - .5f;
    const Vertex vertices[] = {{l,t,0,1,0,0}, {r,t,0,1,1,0}, {l,b,0,1,0,1}, {r,b,0,1,1,1}};
    device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, target.texture.Get());
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    // Keep low-resolution scene pixels discrete when enlarged for display.
    // Minification still uses linear filtering for supersampled previews.
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    device->EndScene();
    saved->Apply();
    return hr;
}
}

bool ParsePercent(const wchar_t* text, double& percent) {
    if (!text) return false;
    errno = 0;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    if (end == text || errno == ERANGE || !std::isfinite(value) || value <= 0) return false;
    while (std::iswspace(*end)) ++end;
    if (*end == L'%') { ++end; while (std::iswspace(*end)) ++end; }
    if (*end) return false;
    percent = value;
    return true;
}
bool CalculateSize(unsigned width, unsigned height, double percent, Size& size) {
    if (!width || !height || !std::isfinite(percent) || percent <= 0) return false;
    const double w = std::round(double(width) * (percent / 100));
    const double h = std::round(double(height) * (percent / 100));
    if (!std::isfinite(w) || !std::isfinite(h) ||
        w > std::numeric_limits<int>::max() || h > std::numeric_limits<int>::max()) return false;
    size = {unsigned(std::max(1.0, w)), unsigned(std::max(1.0, h))};
    return true;
}
bool Above4K(Size size) { return size.width > 3840 || size.height > 2160; }
Size PreviewSize(MMDApp* app, bool automatic, double percent) {
    const RECT rect = app->ViewportRect();
    Size size;
    CalculateSize(unsigned(std::max<LONG>(1, rect.right - rect.left)),
        unsigned(std::max<LONG>(1, rect.bottom - rect.top)), automatic ? 100 : percent, size);
    return size;
}
unsigned ShadowSize(MMDApp* app, unsigned fullSize) {
    if (build_profile::original || !app || app->viewportRender.automatic || app->RecordingWindow()) return fullSize;
    const double percent = app->viewportRender.percent;
    if (!std::isfinite(percent) || percent <= 0 || percent >= 100) return fullSize;
    const double required = std::max(512.0, fullSize * (percent / 100));
    unsigned size = fullSize;
    while (size / 2 >= required) size /= 2;
    return size;
}
bool Configure(MMDApp* app, bool automatic, double percent, std::wstring& error) {
    error.clear();
    const auto text = [&](const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
        return app->EnglishUI() == 2 ? zh : app->EnglishUI() == 0 ? ja : en;
    };
    if (!std::isfinite(percent) || percent <= 0) {
        error = text(L"请输入大于 0 的有效百分比。", L"Enter a percentage greater than 0.", L"0より大きい数値を入力してください。"); return false;
    }
    auto* renderer = app->Renderer();
    const Size size = PreviewSize(app, automatic, percent);
    if (!automatic && (!size.width || !size.height || (renderer &&
        ((renderer->maxTextureWidth > 0 && size.width > unsigned(renderer->maxTextureWidth)) ||
         (renderer->maxTextureHeight > 0 && size.height > unsigned(renderer->maxTextureHeight)))))) {
        error = text(L"计算后的渲染尺寸超过显卡支持的上限，请降低比例。", L"The render resolution exceeds the GPU limit. Lower the percentage.", L"描画解像度がGPUの上限を超えています。倍率を下げてください。"); return false;
    }
    if (app->RecordingWindow()) {
        error = text(L"请在导出完成后调整预览比例。", L"Wait until the export finishes before changing the preview scale.", L"出力が完了してからプレビューの倍率を変更してください。"); return false;
    }
    if (!automatic && percent != 100 && renderer && renderer->device) {
        const HRESULT hr = EnsureTarget(app, size);
        if (FAILED(hr)) {
            runtime_log::Write("VIEWPORT_SCALE allocation %ux%u hr=0x%08lX", size.width, size.height, hr);
            error = text(L"无法创建此尺寸的预览画面，请降低比例。", L"Unable to create a preview at this resolution. Lower the percentage.", L"この解像度ではプレビューを描画できません。倍率を下げてください。"); return false;
        }
    }
    app->viewportRender.automatic = automatic;
    app->viewportRender.percent = percent;
    if (automatic || percent == 100) Release(app);
    return true;
}
void Release(MMDApp* app) { targets.erase(app); }
bool Rendering(MMDApp* app) { return rendering == app; }

bool FrameScope::ScaleBuffer(IDirect3DVertexBuffer9* buffer, unsigned count, unsigned stride) {
    if (!buffer || !count) return true;
    void* data = nullptr;
    if (FAILED(buffer->Lock(0, count * stride, &data, 0))) return false;
    auto* bytes = static_cast<unsigned char*>(data);
    buffers_.push_back({buffer, {bytes, bytes + count * stride}});
    for (unsigned i = 0; i < count; ++i) {
        auto* xy = reinterpret_cast<float*>(bytes + i * stride);
        xy[0] = (xy[0] - display_.left + .5f) * scaleX_ - .5f;
        xy[1] = (xy[1] - display_.top + .5f) * scaleY_ - .5f;
    }
    buffer->Unlock();
    return true;
}
FrameScope::FrameScope(MMDApp* app) {
    if (build_profile::original || app->viewportRender.automatic || app->viewportRender.percent == 100 ||
        app->RecordingWindow() || !app->Renderer() || !app->Renderer()->device) return;
    const Size size = PreviewSize(app, false, app->viewportRender.percent);
    auto* r = app->Renderer();
    if (!size.width || !size.height || FAILED(EnsureTarget(app, size))) { ready_ = false; return; }
    auto* device = r->device;
    if (FAILED(device->GetRenderTarget(0, &previous_)) ||
        FAILED(device->GetDepthStencilSurface(&depth_)) || FAILED(device->GetViewport(&viewport_))) {
        ready_ = false; return;
    }
    app_ = app; device_ = device; display_ = app->ViewportRect(); viewScale_ = r->viewScale;
    previousSlot_ = r->backbufferSurface; depthSlot_ = r->depthStencilSurface;
    device->GetScissorRect(&scissor_);
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorEnabled_);
    auto& target = targets.at(app);
    D3DVIEWPORT9 scaled{0, 0, size.width, size.height, viewport_.MinZ, viewport_.MaxZ};
    device->SetDepthStencilSurface(nullptr);
    ready_ = SUCCEEDED(device->SetRenderTarget(0, target.color.Get())) &&
        SUCCEEDED(device->SetDepthStencilSurface(target.depth.Get())) && SUCCEEDED(device->SetViewport(&scaled));
    r->backbufferSurface = target.color.Get(); r->depthStencilSurface = target.depth.Get();
    scaleX_ = float(size.width) / std::max<LONG>(1, display_.right - display_.left);
    scaleY_ = float(size.height) / std::max<LONG>(1, display_.bottom - display_.top);
    app->ViewportRect() = {0, 0, LONG(size.width), LONG(size.height)};
    r->viewScale *= scaleX_;
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    rendering = app;
    // Screen text, selection lines and HUD controls stay in display pixels.
    // Only geometry composited as part of the scene follows render resolution.
    // Do not dirty managed buffers for layers that will not be drawn. Locking
    // and restoring them every frame adds uploads even to an empty preview.
    if (app->AviBackgroundEnabled() == 1 && app->AviBackgroundTexture())
        ready_ = ready_ && ScaleBuffer(app->AviOverlayVertices(), 6, 28);
    if (app->PictureBackgroundEnabled() && app->PictureBackgroundTexture())
        ready_ = ready_ && ScaleBuffer(app->PictureOverlayVertices(), 6, 28);
    if (r->effect && app->SelfShadowCompositionEnabled() && app->SelfShadowMode() > 0)
        ready_ = ready_ && ScaleBuffer(app->LeftViewportVertices(), 6, 28);
    if (app->DepthTextureCompositionEnabled() && app->DepthDeviceEnabled() &&
        !app->FrameStepPlayback() && app->DepthTextureCallback())
        ready_ = ready_ && ScaleBuffer(app->RightViewportVertices(), 6, 28);
}
FrameScope::~FrameScope() { Restore(false); }
bool FrameScope::Complete() { return Restore(true); }
bool FrameScope::Restore(bool present) {
    if (!app_) return ready_;
    auto* r = app_->Renderer();
    for (auto& backup : buffers_) {
        void* data = nullptr;
        if (SUCCEEDED(backup.buffer->Lock(0, UINT(backup.bytes.size()), &data, 0))) {
            std::memcpy(data, backup.bytes.data(), backup.bytes.size()); backup.buffer->Unlock();
        }
    }
    app_->ViewportRect() = display_; r->viewScale = viewScale_;
    r->backbufferSurface = previousSlot_; r->depthStencilSurface = depthSlot_;
    rendering = nullptr;
    device_->SetDepthStencilSurface(nullptr);
    device_->SetRenderTarget(0, previous_.Get());
    device_->SetDepthStencilSurface(depth_.Get());
    device_->SetViewport(&viewport_);
    bool success = ready_;
    if (ready_ && present) {
        const HRESULT hr = Composite(device_, targets.at(app_), display_);
        success = SUCCEEDED(hr);
        if (FAILED(hr)) runtime_log::Write("VIEWPORT_SCALE composite hr=0x%08lX", hr);
        FillFramePresentationMargins(app_, device_);
    }
    device_->SetScissorRect(&scissor_);
    device_->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorEnabled_);
    app_ = nullptr;
    buffers_.clear();
    return success;
}
}
