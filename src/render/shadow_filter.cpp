#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/shadow_depth_filter.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <utility>

namespace mikumikustudio::shadow_filter {
using Microsoft::WRL::ComPtr;
namespace {
HRESULT FirstFailure(HRESULT first, HRESULT next) {
    return FAILED(first) ? first : next;
}

struct AttemptKey {
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* coarse = nullptr; // Identity only; hdrTexture owns it.
    UINT width = 0, height = 0, requested = 0;
    UINT maxWidth = 0, maxHeight = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    HRESULT deviceStatus = E_FAIL;
    bool operator==(const AttemptKey& other) const noexcept {
        return device == other.device && coarse == other.coarse &&
            width == other.width && height == other.height &&
            requested == other.requested && maxWidth == other.maxWidth &&
            maxHeight == other.maxHeight && format == other.format &&
            deviceStatus == other.deviceStatus;
    }
};


struct State {
    FineDepthTarget fine;
    DepthPrefilter filtered;
    bool filteredValid = false;
    AttemptKey key;
    bool hasKey = false;
    bool failed = false;
    bool pending = false;
    bool valid = false;
    bool directionalCoarse = false;
};
std::unordered_map<D3DRenderer*, State> states;
struct DetailState {
    FineDepthTarget target;
    std::array<float,16> projection{}, coverageToDetail{};
    std::array<float,4> depthBlend{};
    UINT requested = 0, failedSize = 0;
    ULONGLONG failedAt = 0;
    float densityFloor = 0;
    float projectionScale = 0, coverageScale = 0;
    float nearDepth = 0, farDepth = 0, blendStart = 0, logBlend = 0;
    float fixedBlendStart = 0, screenDemand = 0;
    bool orthographic = false;
    bool planned = false, valid = false;
};
std::unordered_map<D3DRenderer*, std::array<DetailState,3>> details;
std::unordered_map<D3DRenderer*, std::unordered_map<unsigned char*,DetailState>> modelDetails;
bool modelShadowsEnabled=true;

bool Enabled(MMDApp* app) {
    if constexpr (build_profile::original) return false;
    return app && app->ShadowFilterEnhanced() && app->ShadowMode() > 0 &&
        app->Renderer() && app->Renderer()->device &&
        app->Renderer()->shaderModelCaps >= 3;
}

bool ReadCoarse(D3DRenderer* renderer, D3DSURFACE_DESC& desc) {
    return renderer && renderer->hdrTexture &&
        SUCCEEDED(renderer->hdrTexture->GetLevelDesc(0, &desc)) &&
        desc.Width && desc.Height && desc.Format == D3DFMT_R32F;
}

bool Current(const State& state, D3DRenderer* renderer) {
    return state.hasKey && state.key.device == renderer->device &&
        state.key.coarse == renderer->hdrTexture;
}

// All three maps use affine light projections. Use the smaller singular value
// of their XY mapping so a layer is never preferred for only one light axis.
float LightPlaneScale(const std::array<float,16>& matrix) {
    if (!std::isfinite(matrix[15]) || std::fabs(matrix[15]) < 1e-6f ||
        matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0) return 0;
    double xx=0, yy=0, xy=0;
    for (int row=0;row<3;++row) {
        const double x=double(matrix[row*4])/matrix[15];
        const double y=double(matrix[row*4+1])/matrix[15];
        xx+=x*x; yy+=y*y; xy+=x*y;
    }
    const double discriminant=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy);
    const double scale=std::sqrt((std::max)(0.0,(xx+yy-discriminant)*.5));
    return std::isfinite(scale) ? float(scale) : 0;
}

UINT CoverageTextureSize(MMDApp* app, D3DRenderer* renderer) {
    const auto found=states.find(renderer);
    if (Enabled(app) && found!=states.end() && Current(found->second,renderer)) {
        const auto& state=found->second;
        if (state.filteredValid) return state.filtered.Size();
        if (state.valid) return state.fine.Size();
    }
    return static_cast<UINT>((std::max)(0,(std::min)(renderer->renderTargetWidth,
        renderer->renderTargetHeight)));
}

void Fail(State& state) {
    state.pending = state.valid = state.directionalCoarse = state.filteredValid = false;
    state.fine.Release();
    state.filtered.Release();
    state.failed = true;
}
} // namespace

void FineDepthTarget::Release() noexcept {
    depth_.Reset();
    surface_.Reset();
    texture_.Reset();
    device_.Reset();
    size_ = 0;
}

HRESULT FineDepthTarget::Ensure(IDirect3DDevice9* device, UINT coarseSize,
                                UINT requestedFineSize) {
    if (!device || !coarseSize || requestedFineSize <= coarseSize)
        return E_INVALIDARG;
    if (device_.Get() == device && size_ == requestedFineSize &&
        texture_ && surface_ && depth_) return S_OK;

    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface, depth;
    HRESULT hr = device->CreateTexture(requestedFineSize, requestedFineSize, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &texture, nullptr);
    if (SUCCEEDED(hr)) hr = texture->GetSurfaceLevel(0, &surface);
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilSurface(requestedFineSize,
        requestedFineSize, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth, nullptr);
    // Allocation never changes the native target. Do not retain an obsolete
    // fine allocation after a failed resize, even if it rendered last frame.
    Release();
    if (FAILED(hr)) return hr;
    device_ = device;
    texture_ = std::move(texture);
    surface_ = std::move(surface);
    depth_ = std::move(depth);
    size_ = requestedFineSize;
    return S_OK;
}

void TargetScope::ClearSaved() noexcept {
    state_.Reset();
    depth_.Reset();
    for (auto& target : targets_) target.Reset();
    device_.Reset();
    targetCount_ = 0;
    captured_ = false;
}

HRESULT TargetScope::Begin(IDirect3DDevice9* device, const FineDepthTarget& target) {
    if (captured_ || !device || !target.Surface() || !target.DepthSurface())
        return E_INVALIDARG;
    ClearSaved();
    device_ = device;
    D3DCAPS9 caps{};
    HRESULT hr = device->GetDeviceCaps(&caps);
    if (FAILED(hr)) { ClearSaved(); return hr; }
    targetCount_ = (std::clamp)(static_cast<UINT>(caps.NumSimultaneousRTs), 1u, 4u);
    hr = device->CreateStateBlock(D3DSBT_ALL, &state_);
    if (SUCCEEDED(hr)) hr = state_->Capture();
    if (SUCCEEDED(hr)) hr = device->GetViewport(&viewport_);
    if (SUCCEEDED(hr)) hr = device->GetRenderTarget(0, &targets_[0]);
    if (FAILED(hr)) { ClearSaved(); return hr; }
    for (UINT i = 1; i < targetCount_; ++i) {
        hr = device->GetRenderTarget(i, &targets_[i]);
        if (FAILED(hr) && hr != D3DERR_NOTFOUND) { ClearSaved(); return hr; }
    }
    hr = device->GetDepthStencilSurface(&depth_);
    if (FAILED(hr) && hr != D3DERR_NOTFOUND) { ClearSaved(); return hr; }
    captured_ = true;

    // D3DSBT_ALL restores these bindings. Detach aliases before attaching the
    // fine texture as a target; external effects may use any pixel sampler.
    hr = S_OK;
    for (DWORD i = 0; i < 16; ++i)
        hr = FirstFailure(hr, device->SetTexture(i, nullptr));
    if (D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion) >= 3 && caps.VertexTextureFilterCaps) {
        for (DWORD i = 0; i < 4; ++i)
            hr = FirstFailure(hr, device->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, nullptr));
    }
    hr = FirstFailure(hr, device->SetDepthStencilSurface(nullptr));
    for (UINT i = 1; i < targetCount_; ++i)
        hr = FirstFailure(hr, device->SetRenderTarget(i, nullptr));
    hr = FirstFailure(hr, device->SetRenderTarget(0, target.Surface()));
    hr = FirstFailure(hr, device->SetDepthStencilSurface(target.DepthSurface()));
    D3DVIEWPORT9 viewport{0, 0, target.Size(), target.Size(), 0.0f, 1.0f};
    hr = FirstFailure(hr, device->SetViewport(&viewport));
    hr = FirstFailure(hr, device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                      0xffffffffu, 1.0f, 0));
    if (FAILED(hr)) Restore();
    return hr;
}

HRESULT TargetScope::Restore() noexcept {
    if (!captured_) { ClearSaved(); return S_OK; }
    captured_ = false;
    HRESULT hr = device_->SetDepthStencilSurface(nullptr);
    for (UINT i = 1; i < targetCount_; ++i)
        hr = FirstFailure(hr, device_->SetRenderTarget(i, nullptr));
    hr = FirstFailure(hr, device_->SetRenderTarget(0, targets_[0].Get()));
    for (UINT i = 1; i < targetCount_; ++i)
        if (targets_[i]) hr = FirstFailure(hr, device_->SetRenderTarget(i, targets_[i].Get()));
    hr = FirstFailure(hr, device_->SetDepthStencilSurface(depth_.Get()));
    hr = FirstFailure(hr, state_->Apply());
    // SetRenderTarget resets the viewport; restore it after the state block.
    hr = FirstFailure(hr, device_->SetViewport(&viewport_));
    ClearSaved();
    return hr;
}

void Invalidate(MMDApp* app) {
    if (!app || !app->Renderer()) return;
    if (app->ShadowMode() <= 0 || !app->AdaptiveShadowResolution()) {
        if (app->Renderer()->device)
            for (DWORD stage=3;stage<=6;++stage) app->Renderer()->device->SetTexture(stage,nullptr);
        details.erase(app->Renderer());
    }
    if (const auto detail = details.find(app->Renderer()); detail != details.end())
        for(auto& layer:detail->second)layer.planned=layer.valid=false;
    if(app->ShadowMode()<=0 || app->AdaptiveShadowResolution()) {
        if(app->Renderer()->device && modelDetails.find(app->Renderer())!=modelDetails.end())
            app->Renderer()->device->SetTexture(3,nullptr);
        modelDetails.erase(app->Renderer());
    }
    else if(auto found=modelDetails.find(app->Renderer());found!=modelDetails.end())
        for(auto& entry:found->second)entry.second.planned=entry.second.valid=false;
    if (!Enabled(app)) {
        // Filtering can be disabled while the independent detail map is active.
        states.erase(app->Renderer());
        return;
    }
    const auto found = states.find(app->Renderer());
    if (found == states.end()) return;
    auto& state = found->second;
    state.pending = state.valid = state.directionalCoarse = state.filteredValid = false;
}

FineDepthTarget* Prepare(MMDApp* app, bool nativeSucceeded) {
    if (!Enabled(app)) return nullptr;
    auto* renderer = app->Renderer();
    auto& state = states.try_emplace(renderer).first->second;
    state.pending = state.valid = state.directionalCoarse = state.filteredValid = false;
    D3DSURFACE_DESC coarse{};
    if (!nativeSucceeded || !ReadCoarse(renderer, coarse) || coarse.Width != coarse.Height) {
        state.fine.Release();
    state.filtered.Release();
        return nullptr;
    }
    D3DCAPS9 caps{};
    if (FAILED(renderer->device->GetDeviceCaps(&caps))) {
        state.fine.Release();
    state.filtered.Release();
        return nullptr;
    }
    const UINT cap = viewport_render_scale::ShadowSize(app,
        (std::min)({8192u, static_cast<UINT>(caps.MaxTextureWidth),
                   static_cast<UINT>(caps.MaxTextureHeight)}));
    if (!cap) { state.fine.Release(); return nullptr; }
    // The shared projection already carries the slider's world-space density.
    // Filtering must use that grid; a second, larger texture with the same
    // projection would silently override coarse precision again.
    const UINT requested = (std::min)(cap, coarse.Width);
    const AttemptKey key{renderer->device, renderer->hdrTexture, coarse.Width,
        coarse.Height, requested, caps.MaxTextureWidth, caps.MaxTextureHeight,
        coarse.Format, renderer->device->TestCooperativeLevel()};
    if (!state.hasKey || !(state.key == key)) {
        state.fine.Release();
    state.filtered.Release();
        state.key = key;
        state.hasKey = true;
        state.failed = false;
    }
    if (FAILED(key.deviceStatus)) { Fail(state); return nullptr; }
    if (requested <= coarse.Width) {
        state.fine.Release();
        state.directionalCoarse = true;
        return nullptr;
    }
    if (state.failed) return nullptr;
    if (FAILED(state.fine.Ensure(renderer->device, coarse.Width, requested))) {
        Fail(state);
        return nullptr;
    }
    state.pending = true;
    return &state.fine;
}

void MarkRendered(MMDApp* app, bool succeeded) {
    if (!app || !app->Renderer()) return;
    const auto found = states.find(app->Renderer());
    if (found == states.end() || !found->second.pending) return;
    auto& state = found->second;
    if (!succeeded || !Enabled(app) || !Current(state, app->Renderer())) {
        Fail(state);
        return;
    }
    state.pending = false;
    state.valid = state.fine.Texture() != nullptr;
}

void FinalizeDepth(MMDApp* app, bool succeeded) {
    if(!Enabled(app)) return;
    auto* renderer=app->Renderer();auto found=states.find(renderer);
    if(found==states.end())return;
    auto& state=found->second;
    if(!succeeded || !Current(state,renderer) || state.failed ||
       (!state.valid && !state.directionalCoarse))return;
    auto* input=state.valid?state.fine.Texture():renderer->hdrTexture;
    const HRESULT hr=state.filtered.Apply(renderer->device,input,state.key.width);
    state.filteredValid=SUCCEEDED(hr);
    if(FAILED(hr)) {
        runtime_log::Write("SHARED_DEPTH failure hr=0x%08lX coarse=%u",hr,state.key.width);
        Fail(state);
    }
}

IDirect3DTexture9* VisibleTexture(MMDApp* app) {
    if (!app || !app->Renderer()) return nullptr;
    auto* renderer = app->Renderer();
    const auto found = states.find(renderer);
    if (Enabled(app) && found != states.end() && Current(found->second, renderer) &&
        found->second.valid) return found->second.filteredValid?found->second.filtered.Texture():found->second.fine.Texture();
    if(Enabled(app) && found!=states.end() && Current(found->second,renderer) && found->second.filteredValid) return found->second.filtered.Texture();
    return renderer->hdrTexture;
}

SamplingState ReadSamplingState(MMDApp* app) {
    SamplingState result;
    bool enabled = Enabled(app);
    // zw=0 selects the released dynamic spline fallback, never the C kernel.
    float size[4]{1.0f, 1.0f, 0.0f, 0.0f};
    auto* renderer = app ? app->Renderer() : nullptr;
    D3DSURFACE_DESC coarse{}, visible{};
    if (ReadCoarse(renderer, coarse)) {
        size[0] = static_cast<float>(coarse.Width);
        size[1] = static_cast<float>(coarse.Height);
        const auto found = states.find(renderer);
        if (enabled && found != states.end() && Current(found->second, renderer)) {
            auto& state = found->second;
            bool directional = state.directionalCoarse;
            if (state.valid) {
                if (SUCCEEDED(state.fine.Texture()->GetLevelDesc(0, &visible)) &&
                    visible.Width && visible.Height && visible.Format == D3DFMT_R32F) {
                    size[0] = static_cast<float>(visible.Width);
                    size[1] = static_cast<float>(visible.Height);
                    directional = true;
                } else Fail(state);
            }
            if (directional) {
                size[2] = static_cast<float>(coarse.Width);
                size[3] = static_cast<float>(coarse.Height);
            }
        }
    } else {
        enabled = false; // No reliable texel size: retain the original lookup.
    }
    const auto final = states.find(renderer);
    if (enabled && final != states.end() && Current(final->second, renderer) &&
        final->second.filteredValid) {
        // Sampling is shared by native materials, MME and MMV. A prefiltered
        // depth texture must not receive the per-material kernel again.
        enabled = false;
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(final->second.filtered.Texture()->GetLevelDesc(0, &desc))) {
            size[0] = static_cast<float>(desc.Width);
            size[1] = static_cast<float>(desc.Height);
            size[2] = size[3] = 0.0f;
            result.prefiltered = true;
            result.coarseSize[0] = static_cast<float>(coarse.Width);
            result.coarseSize[1] = static_cast<float>(coarse.Height);
        }
    }
    result.texture = VisibleTexture(app);
    result.enhanced = enabled;
    std::copy_n(size, 4, result.mapSize);
    return result;
}

SamplingState ReadNativeSamplingState(MMDApp* app) {
    if(!Enabled(app))return ReadSamplingState(app);
    D3DSURFACE_DESC raw{};
    if(!ReadCoarse(app->Renderer(),raw))return ReadSamplingState(app);
    SamplingState result;
    result.texture=app->Renderer()->hdrTexture;
    result.enhanced=true;
    result.mapSize[0]=float(raw.Width);result.mapSize[1]=float(raw.Height);
    return result;
}

void ConfigureEffect(MMDApp* app, void* effect) {
    if (!effect) return;
    const auto state = ReadNativeSamplingState(app);
    d3dx::FxSetInt(effect, "MdsLayerFilterEnhanced", Enabled(app) ? 1 : 0);
    d3dx::FxSetFloatArray(effect, "MdsShadowFilterSideWeight", &kDepthFilterSideWeight, 1);
    d3dx::FxSetInt(effect, "MdsShadowFilterEnhanced", state.enhanced ? 1 : 0);
    d3dx::FxSetFloatArray(effect, "MdsShadowMapSize", state.mapSize, 4);
    ConfigureDetailReceiver(app, effect, false);
}

void PlanDetail(MMDApp* app, const std::array<float,16>& projection,
                const std::array<float,16>& coverage, const float view[16],
                float farDepth, UINT size, unsigned layer, float densityFloor, float nearDepth,
                float blendFraction,float screenDemand,bool orthographic) {
    auto* renderer = app->Renderer();
    if (!app->AdaptiveShadowResolution() || !renderer || !renderer->effect || renderer->shaderModelCaps < 3 || size < 512 ||
        FAILED(d3dx::FxSetInt(renderer->effect, "MdsDetailEnabled", 0))) return;
    if(layer>=3 || !std::isfinite(nearDepth) || !std::isfinite(farDepth) || farDepth<=nearDepth)return;
    auto& detail = details[renderer][layer];
    using Matrix = d3dx::D3DXMATRIXF;
    Matrix inverse{}, transform{};
    if (!d3dx::Get().inverse(&inverse, nullptr, reinterpret_cast<const Matrix*>(coverage.data()))) return;
    d3dx::Get().multiply(&transform, &inverse, reinterpret_cast<const Matrix*>(projection.data()));
    std::copy_n(&transform.m[0][0], 16, detail.coverageToDetail.begin());
    d3dx::Get().multiply(&transform, &inverse, reinterpret_cast<const Matrix*>(view));
    // A narrow close layer must still be fully weighted at its near plane.
    const float fraction=std::isfinite(blendFraction) ? (std::clamp)(blendFraction,.001f,1.f) : .2f;
    const float blendWidth = (std::max)(1e-6f,(farDepth-nearDepth)*fraction);
    for(int row=0;row<4;++row) detail.depthBlend[row]=-transform.m[row][2]/blendWidth;
    detail.depthBlend[3]+=farDepth/blendWidth;
    detail.projection = projection;
    detail.projectionScale = LightPlaneScale(projection);
    detail.coverageScale = LightPlaneScale(coverage);
    detail.requested = size;
    detail.densityFloor = densityFloor;
    detail.nearDepth = nearDepth;
    detail.farDepth = farDepth;
    detail.blendStart = farDepth-blendWidth;
    detail.fixedBlendStart = detail.blendStart;
    detail.screenDemand = screenDemand;
    detail.orthographic = orthographic;
    detail.logBlend = ShadowTransitionLogRatio(detail.blendStart,farDepth);
    detail.planned = true;
}

FineDepthTarget* PrepareDetail(MMDApp* app, bool nativeSucceeded,
                              std::array<float,16>& projection, unsigned layer) {
    auto* renderer = app->Renderer();
    const auto found = details.find(renderer);
    if (found == details.end() || layer>=3) return nullptr;
    auto& detail = found->second[layer];
    detail.valid = false;
    if (!nativeSucceeded || !detail.planned) return nullptr;
    // Temporary memory pressure must not disable this size until device reset.
    // Retry at most once a second; a changed request may be tried immediately.
    if (detail.failedSize == detail.requested && GetTickCount64()-detail.failedAt < 1000)
        return nullptr;
    const HRESULT allocation=detail.target.Ensure(renderer->device, 1, detail.requested);
    if (FAILED(allocation)) {
        detail.failedSize = detail.requested;
        detail.failedAt = GetTickCount64();
        runtime_log::Write("SHADOW_DETAIL allocation failed layer=%u size=%u hr=0x%08lX retry_ms=1000",
            layer,detail.requested,static_cast<unsigned long>(allocation));
        return nullptr;
    }
    detail.failedSize = 0;
    detail.failedAt = 0;
    projection = detail.projection;
    return &detail.target;
}

void MarkDetailRendered(MMDApp* app, bool succeeded, unsigned layer) {
    const auto found = details.find(app->Renderer());
    if (found != details.end() && layer<3) found->second[layer].valid = succeeded && found->second[layer].planned;
}

void PlanModel(MMDApp* app,unsigned char* model,const std::array<float,16>& projection,
               const std::array<float,16>& coverage,UINT size,float density) {
    if(!modelShadowsEnabled || !app || !model || app->AdaptiveShadowResolution() || app->ShadowMode()<=0 || size<512)return;
    auto* renderer=app->Renderer();
    if(!renderer || !renderer->effect || renderer->shaderModelCaps<3 ||
        FAILED(d3dx::FxSetInt(renderer->effect,"MdsDetailEnabled",0)))return;
    using Matrix=d3dx::D3DXMATRIXF;
    Matrix inverse{},transform{};
    if(!d3dx::Get().inverse(&inverse,nullptr,reinterpret_cast<const Matrix*>(coverage.data())))return;
    d3dx::Get().multiply(&transform,&inverse,reinterpret_cast<const Matrix*>(projection.data()));
    auto& state=modelDetails[renderer][model];
    state.projection=projection;
    std::copy_n(&transform.m[0][0],16,state.coverageToDetail.begin());
    state.projectionScale=LightPlaneScale(projection);state.coverageScale=LightPlaneScale(coverage);
    state.requested=size;state.densityFloor=density;state.depthBlend={0,0,0,1};
    state.planned=true;
}

void SetModelShadowsEnabledForTests(bool enabled) {modelShadowsEnabled=enabled;}

std::vector<unsigned char*> PlannedModels(MMDApp* app) {
    std::vector<unsigned char*> result;
    if(!app || !app->Renderer())return result;
    auto found=modelDetails.find(app->Renderer());if(found==modelDetails.end())return result;
    for(auto i=found->second.begin();i!=found->second.end();) {
        if(!i->second.planned)i=found->second.erase(i);
        else {result.push_back(i->first);++i;}
    }
    return result;
}

FineDepthTarget* PrepareModel(MMDApp* app,unsigned char* model,bool nativeSucceeded,
                             std::array<float,16>& projection) {
    auto found=modelDetails.find(app->Renderer());if(found==modelDetails.end())return nullptr;
    auto entry=found->second.find(model);if(entry==found->second.end())return nullptr;
    auto& state=entry->second;state.valid=false;
    if(!nativeSucceeded || !state.planned || app->AdaptiveShadowResolution())return nullptr;
    if(state.failedSize==state.requested && GetTickCount64()-state.failedAt<1000)return nullptr;
    const HRESULT allocation=state.target.Ensure(app->Renderer()->device,1,state.requested);
    if(FAILED(allocation)) {
        state.failedSize=state.requested;state.failedAt=GetTickCount64();
        runtime_log::Write("SHADOW_MODEL allocation failed model=%p size=%u hr=0x%08lX retry_ms=1000",
            model,state.requested,static_cast<unsigned long>(allocation));
        return nullptr;
    }
    state.failedSize=0;projection=state.projection;return &state.target;
}

void MarkModelRendered(MMDApp* app,unsigned char* model,bool succeeded) {
    auto found=modelDetails.find(app->Renderer());if(found==modelDetails.end())return;
    auto entry=found->second.find(model);if(entry!=found->second.end())
        entry->second.valid=succeeded && entry->second.planned;
}

DetailInfo ReadModelInfo(MMDApp* app,unsigned char* model) {
    if(!app || !app->Renderer())return {};
    const auto found=modelDetails.find(app->Renderer());if(found==modelDetails.end())return {};
    const auto entry=found->second.find(model);if(entry==found->second.end())return {};
    const auto& state=entry->second;
    return {state.valid,state.target.Size(),state.projection,state.densityFloor,0,0,0};
}

void ConfigureDetailReceiver(MMDApp* app, void* effect, bool receivesShadow,unsigned char* model) {
    if(!app || !effect || !app->Renderer())return;
    auto* renderer=app->Renderer();
    if(!renderer->device)return;
    // MME inherits sampler 0 through its captured device state. Keep its
    // shared depth texture there and bind raw coverage only for native draws.
    D3DSURFACE_DESC raw{};
    bool nativeCoverage=receivesShadow && Enabled(app) && ReadCoarse(renderer,raw);
    const HRESULT bound=renderer->device->SetTexture(6,nativeCoverage?renderer->hdrTexture:nullptr);
    if(FAILED(bound)) {
        nativeCoverage=false;
        renderer->device->SetTexture(6,nullptr);
    }
    const auto sampling=nativeCoverage?ReadNativeSamplingState(app):ReadSamplingState(app);
    d3dx::FxSetInt(effect,"MdsShadowFilterEnhanced",nativeCoverage?1:0);
    d3dx::FxSetFloatArray(effect,"MdsShadowMapSize",sampling.mapSize,4);
    const auto found=details.find(renderer);
    DetailState* fixedModel=nullptr;
    if(receivesShadow && !app->AdaptiveShadowResolution() && app->ShadowMode()>0 && model) {
        const auto owner=modelDetails.find(renderer);
        if(owner!=modelDetails.end()) {
            const auto entry=owner->second.find(model);
            if(entry!=owner->second.end() && entry->second.valid)fixedModel=&entry->second;
        }
    }
    const char* enabledNames[]{"MdsDetailEnabled","MdsNearEnabled","MdsBridgeEnabled"};
    const char* matrixNames[]{"MdsCoverageToDetail","MdsCoverageToNear","MdsCoverageToBridge"};
    const char* blendNames[]{"MdsDetailDepthBlend","MdsNearDepthBlend","MdsBridgeDepthBlend"};
    const char* texelNames[]{"MdsDetailTexelSize","MdsNearTexelSize","MdsBridgeTexelSize"};
    const float coverageSize=fixedModel ? float(renderer->renderTargetWidth) : float(CoverageTextureSize(app,renderer));
    float density[3]{};
    float logBlend[3]{};
    bool enabled[3]{};
    float coarseDensity=0.f;
    // Process outer to inner, skipping invalid or failed maps. The transition
    // then uses the texture actually available to receive the cast shadow.
    for(unsigned layer:{2u,0u,1u}) {
        DetailState* selected=layer==0?fixedModel:nullptr;
        if (receivesShadow && app->AdaptiveShadowResolution() && app->ShadowMode()>0 && found!=details.end() && found->second[layer].valid)
            selected=&found->second[layer];
        if (selected) {
            const auto& state=*selected;
            density[layer]=state.projectionScale*float(state.target.Size())*.5f;
            const float coverageDensity=state.coverageScale*coverageSize*.5f;
            if(!(coarseDensity>0))coarseDensity=coverageDensity;
            // Allow only rounding noise when comparing equivalent light axes.
            enabled[layer]=density[layer]>0 && coverageDensity>0 &&
                density[layer]>=coverageDensity*(1.f-1e-5f);
        }
        d3dx::FxSetInt(effect,enabledNames[layer],enabled[layer]?1:0);
        if(enabled[layer]) {
            auto& state=*selected;
            auto depthBlend=state.depthBlend;
            state.blendStart=state.fixedBlendStart;
            logBlend[layer]=state.logBlend;
            if(state.screenDemand>0 && coarseDensity>0) {
                state.blendStart=ShadowScreenBlendStartForDemand(state.nearDepth,state.farDepth,
                    state.fixedBlendStart,coarseDensity,state.screenDemand,state.orthographic);
                const float scale=(state.farDepth-state.fixedBlendStart)/(state.farDepth-state.blendStart);
                for(float& value:depthBlend)value*=scale;
                logBlend[layer]=ShadowTransitionLogRatio(state.blendStart,state.farDepth);
            }
            d3dx::FxSetMatrix(effect,matrixNames[layer],reinterpret_cast<const d3dx::D3DXMATRIXF*>(state.coverageToDetail.data()));
            d3dx::FxSetFloatArray(effect,blendNames[layer],depthBlend.data(),4);
            const float texel[]{1.f/state.target.Size(),1.f/state.target.Size(),0,0};
            d3dx::FxSetFloatArray(effect,texelNames[layer],texel,4);
            renderer->device->SetTexture(3+layer,state.target.Texture());
            coarseDensity=density[layer];
        } else renderer->device->SetTexture(3+layer,nullptr);
    }
    d3dx::FxSetFloatArray(effect,"MdsDetailLogBlend",logBlend,3);
    d3dx::FxSetInt(effect,"MdsProgressiveBridge",app->AdaptiveShadowResolution()?1:0);
    // Nested boxes grow near->middle->bridge while requested density falls.
    // Padding absorbs allocation rounding; with the common hardware cap their
    // actual densities retain that order. A failed detail allocation is disabled,
    // not downscaled. Never reorder equal grids because of float roundoff.
    d3dx::FxSetInt(effect,"MdsPreferMiddle",0);
    constexpr float rank[]{1,2,0};
    d3dx::FxSetFloatArray(effect,"MdsDetailRank",rank,3);
}

DetailInfo ReadDetailInfo(MMDApp* app, unsigned layer) {
    if (!app || layer>=3) return {};
    const auto found = details.find(app->Renderer());
    if (found == details.end()) return {};
    const auto& state = found->second[layer];
    return {state.valid, state.target.Size(), state.projection, state.densityFloor,
        state.nearDepth,state.farDepth,state.blendStart};
}

void ReleaseRenderer(D3DRenderer* renderer, bool forget) {
    if(renderer && renderer->device)
        for(DWORD stage=3;stage<=6;++stage) renderer->device->SetTexture(stage,nullptr);
    details.erase(renderer);
    modelDetails.erase(renderer);
    const auto found = states.find(renderer);
    if (found == states.end()) return;
    auto& state = found->second;
    state.pending = state.valid = state.directionalCoarse = state.filteredValid = false;
    state.fine.Release();
    state.filtered.Release();
    state.key = {};
    state.hasKey = state.failed = false;
    if (forget) states.erase(found);
}
} // namespace mikumikustudio::shadow_filter
