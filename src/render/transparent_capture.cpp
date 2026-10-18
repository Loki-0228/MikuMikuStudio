#include "mikudancestudio/transparent_capture.hpp"
#include "mikudancestudio/wic_mov_recorder.hpp"
#include "mikudancestudio/image_sequence_export.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include <unordered_map>

namespace mikudancestudio {
using Microsoft::WRL::ComPtr;
namespace {
std::unordered_map<MMDApp*, ComPtr<IDirect3DSurface9>> targets;
constexpr D3DRENDERSTATETYPE alphaStates[] = {
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_BLENDOPALPHA, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA};
}
bool TransparentExportActive(MMDApp* app) {
    return app && (IsPngMovRecording(app->Recorder()) || image_sequence::Transparent(app));
}
IDirect3DSurface9* TransparentCaptureSurface(MMDApp* app) {
    const auto it = targets.find(app); return it == targets.end() ? nullptr : it->second.Get();
}
void ReleaseTransparentCapture(MMDApp* app) { targets.erase(app); }
void SetCoverageAlphaBlend(IDirect3DDevice9* device) {
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
}
TransparentFrameScope::TransparentFrameScope(MMDApp* app) {
    if (!TransparentExportActive(app)) return;
    ready_ = false;
    auto* renderer = app->Renderer();
    if (!renderer || !renderer->device) return;
    auto* device = renderer->device;
    ComPtr<IDirect3DSurface9> previous;
    if (FAILED(device->GetRenderTarget(0, &previous))) return;
    D3DSURFACE_DESC desc{};
    if (FAILED(previous->GetDesc(&desc))) return;
    auto& target = targets[app];
    D3DSURFACE_DESC existing{};
    if (target) target->GetDesc(&existing);
    if (!target || existing.Width != desc.Width || existing.Height != desc.Height ||
        existing.MultiSampleType != desc.MultiSampleType || existing.MultiSampleQuality != desc.MultiSampleQuality) {
        target.Reset();
        if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
            desc.MultiSampleType, desc.MultiSampleQuality, FALSE, &target, nullptr))) return;
    }
    device->GetViewport(&viewport_);
    if (FAILED(device->SetRenderTarget(0, target.Get()))) return;
    device->SetViewport(&viewport_);
    previous_ = previous;
    device_ = device;
    for (unsigned i = 0; i < 4; ++i) device->GetRenderState(alphaStates[i], &alpha_[i]);
    SetCoverageAlphaBlend(device);
    ready_ = true;
}
TransparentFrameScope::~TransparentFrameScope() {
    if (!device_) return;
    device_->SetRenderTarget(0, previous_.Get());
    device_->SetViewport(&viewport_);
    for (unsigned i = 0; i < 4; ++i) device_->SetRenderState(alphaStates[i], alpha_[i]);
}
}
