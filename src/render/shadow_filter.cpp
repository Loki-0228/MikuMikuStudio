#include "mikumikustudio/shadow_filter.hpp"
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

// Experimental receiver-independent depth preprocessing. Two separable passes
// integrate exactly the same positive cubic kernel at output texel centers.
// No receiver ddx/ddy footprint and no directional extension in this baseline.
struct PrefilterTarget {
    ComPtr<IDirect3DTexture9> scratch, final;
    ComPtr<IDirect3DSurface9> scratchSurface, finalSurface;
    ComPtr<IDirect3DPixelShader9> shader;
    UINT size=0;
    unsigned frames=0;
    void Release() {scratch.Reset();final.Reset();scratchSurface.Reset();finalSurface.Reset();shader.Reset();size=0;frames=0;}
    IDirect3DTexture9* Texture() const {return final.Get();}
    HRESULT Ensure(IDirect3DDevice9* device,UINT requested) {
        if(size==requested && scratch && final && shader)return S_OK;
        Release();
        static constexpr char code[]=R"HLSL(
sampler2D depthMap : register(s0);
float4 sizeAxis : register(c0); // xy source dimensions, zw unit pass axis
float4 weights[17] : register(c1); // x paired offset, y combined weight
float4 kernel : register(c18); // x paired sample count
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float value=0;
    [loop]for(int i=0;i<17;++i) {
        if(i>=kernel.x)break;
        value+=weights[i].y*tex2Dlod(depthMap,float4(uv+sizeAxis.zw*weights[i].x/sizeAxis.xy,0,0)).r;
    }
    return float4(value,value,value,1);
}
)HLSL";
        ComPtr<ID3DBlob> bytes,error;
        HRESULT hr=graphics_runtime::Compile(code,sizeof(code)-1,"shared-depth-baseline",nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytes,&error);
        if(FAILED(hr)) {runtime_log::Write("SHARED_DEPTH compile hr=%08lX %s",hr,error?static_cast<const char*>(error->GetBufferPointer()):"");return hr;}
        if(FAILED(hr=device->CreatePixelShader(static_cast<const DWORD*>(bytes->GetBufferPointer()),&shader)))return hr;
        for(int i=0;i<2;++i) {
            auto& texture=i?final:scratch;auto& surface=i?finalSurface:scratchSurface;
            if(FAILED(hr=device->CreateTexture(requested,requested,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&texture,nullptr)) ||
               FAILED(hr=texture->GetSurfaceLevel(0,&surface))) {Release();return hr;}
        }
        size=requested;
        runtime_log::Write("SHARED_DEPTH allocated size=%u extra_bytes=%llu",size,8ull*size*size);
        return S_OK;
    }
    HRESULT Apply(IDirect3DDevice9* device,IDirect3DTexture9* input,UINT coarse) {
        D3DSURFACE_DESC desc{};HRESULT hr=input?input->GetLevelDesc(0,&desc):E_INVALIDARG;
        if(FAILED(hr)||!coarse)return FAILED(hr)?hr:E_INVALIDARG;
        if(FAILED(hr=Ensure(device,desc.Width)))return hr;
        const float scale=(std::min)(8.f,.75f*(std::max)(1.f,1.240967365f*float(coarse)/4096.f)*float(size)/float(coarse));
        const auto cubic=[](float coordinate){float x=std::abs(coordinate),t=(std::max)(2.f-x,0.f);return x<1.f?(4.f-6.f*x*x+3.f*x*x*x)/6.f:t*t*t/6.f;};
        float constants[17][4]{};unsigned count=0;float normalization=0;
        const int reach=int(std::ceil(2.f*scale));
        for(int k=-reach;k<=reach;k+=2) {
            float a=cubic(float(k)/scale),b=cubic(float(k+1)/scale),sum=a+b;
            if(sum<=1.e-8f)continue;
            constants[count][0]=float(k)+b/sum;constants[count][1]=sum;normalization+=sum;++count;
        }
        for(unsigned i=0;i<count;++i)constants[i][1]/=normalization;
        struct Saved {
            IDirect3DDevice9* d;
            ComPtr<IDirect3DStateBlock9> state;
            ComPtr<IDirect3DSurface9> rt[4],depth;
            D3DVIEWPORT9 view{};UINT count=0;bool valid=false;
            HRESULT Restore() {
                if(!valid)return S_OK;valid=false;
                HRESULT h=d->SetTexture(0,nullptr);
                h=FirstFailure(h,d->SetDepthStencilSurface(nullptr));
                for(UINT i=1;i<count;++i)h=FirstFailure(h,d->SetRenderTarget(i,nullptr));
                for(UINT i=0;i<count;++i)if(rt[i]||i==0)h=FirstFailure(h,d->SetRenderTarget(i,rt[i].Get()));
                h=FirstFailure(h,d->SetDepthStencilSurface(depth.Get()));
                h=FirstFailure(h,state->Apply());h=FirstFailure(h,d->SetViewport(&view));return h;
            }
            ~Saved(){Restore();}
        } saved{device};
        D3DCAPS9 caps{};if(FAILED(hr=device->GetDeviceCaps(&caps)))return hr;
        saved.count=(std::clamp)(UINT(caps.NumSimultaneousRTs),1u,4u);
        if(FAILED(hr=device->CreateStateBlock(D3DSBT_ALL,&saved.state)) || FAILED(hr=saved.state->Capture()) ||
           FAILED(hr=device->GetViewport(&saved.view)))return hr;
        for(UINT i=0;i<saved.count;++i){hr=device->GetRenderTarget(i,&saved.rt[i]);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return hr;}
        hr=device->GetDepthStencilSurface(&saved.depth);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return hr;
        saved.valid=true;
        for(DWORD i=0;i<16;++i)device->SetTexture(i,nullptr);
        if(D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion)>=3&&caps.VertexTextureFilterCaps)
            for(DWORD i=0;i<4;++i)device->SetTexture(D3DVERTEXTEXTURESAMPLER0+i,nullptr);
        device->SetDepthStencilSurface(nullptr);for(UINT i=1;i<saved.count;++i)device->SetRenderTarget(i,nullptr);
        device->SetVertexShader(nullptr);device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
        for(auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_ALPHABLENDENABLE,
            D3DRS_ALPHATESTENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,
            D3DRS_CLIPPLANEENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_MULTISAMPLEANTIALIAS})device->SetRenderState(rs,FALSE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
        device->SetPixelShader(shader.Get());device->SetPixelShaderConstantF(1,&constants[0][0],17);
        const float kernel[4]{float(count),0,0,0};device->SetPixelShaderConstantF(18,kernel,1);
        struct Vertex{float x,y,z,w,u,v;};
        const float edge=float(size)-.5f;
        const Vertex quad[]={{-.5f,-.5f,0,1,0,0},{edge,-.5f,0,1,1,0},{-.5f,edge,0,1,0,1},{edge,edge,0,1,1,1}};
        const D3DVIEWPORT9 view{0,0,size,size,0,1};
        for(int pass=0;pass<2;++pass) {
            device->SetTexture(0,nullptr);
            if(FAILED(hr=device->SetRenderTarget(0,pass?finalSurface.Get():scratchSurface.Get())))break;
            device->SetViewport(&view);device->SetTexture(0,pass?scratch.Get():input);
            const float axis[4]{float(size),float(size),pass?0.f:1.f,pass?1.f:0.f};
            device->SetPixelShaderConstantF(0,axis,1);
            if(FAILED(hr=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex))))break;
        }
        hr=FirstFailure(hr,saved.Restore());
        if(frames++==0)runtime_log::Write("SHARED_DEPTH applied coarse=%u fine=%u scale=%.9f pairs=%u passes=2 hr=0x%08lX receiver_kernel=off",coarse,size,scale,count,hr);
        return hr;
    }
};

struct State {
    FineDepthTarget fine;
    PrefilterTarget filtered;
    bool filteredValid = false;
    AttemptKey key;
    bool hasKey = false;
    bool failed = false;
    bool pending = false;
    bool valid = false;
    bool directionalCoarse = false;
};
std::unordered_map<D3DRenderer*, State> states;

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
    if (!Enabled(app)) { ReleaseRenderer(app->Renderer(), true); return; }
    const auto found = states.find(app->Renderer());
    if (found == states.end()) return;
    auto& state = found->second;
    state.pending = state.valid = state.directionalCoarse = state.filteredValid = false;
}

FineDepthTarget* Prepare(MMDApp* app, bool nativeSucceeded) {
    if (!Enabled(app)) { Invalidate(app); return nullptr; }
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
    // coarse.Width is already scaled by UpdateAdaptiveShadowMap. Scale only
    // the quality floor/cap so neither the extra depth pass nor its two filter
    // passes silently restore full-resolution work in a reduced preview.
    const UINT requested = (std::min)(cap, (std::max)(viewport_render_scale::ShadowSize(app, 4096u),
        (std::min)(coarse.Width, 8192u) * 2u));
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

void ConfigureEffect(MMDApp* app, void* effect) {
    if (!effect) return;
    const auto state = ReadSamplingState(app);
    d3dx::FxSetInt(effect, "MdsShadowFilterEnhanced", state.enhanced ? 1 : 0);
    d3dx::FxSetFloatArray(effect, "MdsShadowMapSize", state.mapSize, 4);
}

void ReleaseRenderer(D3DRenderer* renderer, bool forget) {
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
