#include "mikumikustudio/shadow_depth_filter.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <algorithm>

namespace mikumikustudio::shadow_filter {
using Microsoft::WRL::ComPtr;
namespace {
HRESULT FirstFailure(HRESULT first, HRESULT next) {
    return FAILED(first) ? first : next;
}
} // namespace

void DepthPrefilter::Release() noexcept {
    scratch.Reset();final.Reset();scratchSurface.Reset();finalSurface.Reset();
    shader.Reset();size=0;frames=0;
}

HRESULT DepthPrefilter::Ensure(IDirect3DDevice9* device,UINT requested) {
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

HRESULT DepthPrefilter::Apply(IDirect3DDevice9* device,IDirect3DTexture9* input,UINT coarse) {
    D3DSURFACE_DESC desc{};HRESULT hr=input?input->GetLevelDesc(0,&desc):E_INVALIDARG;
    if(FAILED(hr)||!coarse)return FAILED(hr)?hr:E_INVALIDARG;
    if(FAILED(hr=Ensure(device,desc.Width)))return hr;
    constexpr float scale=.75f*1.240967365f;
    constexpr float side=kDepthFilterSideWeight;
    // Keep integer sample offsets, including the receiver-side reconstruction.
    // Fractionally paired taps introduce a second interpolation phase that can
    // become visible after the native mode-2 depth comparison.
    const float constants[17][4]{{-1.f,side,0,0},
        {0.f,1.f-2.f*side,0,0},{1.f,side,0,0}};
    constexpr unsigned count=3;
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

} // namespace mikumikustudio::shadow_filter
