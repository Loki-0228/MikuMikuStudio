#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "mikumikustudio/graphics_runtime.hpp"
#include "../src/render/smaa_shader.hpp"
#include "../src/render/smaa_lookup.hpp"

using Microsoft::WRL::ComPtr;
using namespace mikumikustudio;
namespace {
void Check(bool ok,const char* message) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
    std::printf("PASS: %s\n",message);
}
}
int main() {
    constexpr unsigned side=96;
    HWND window=CreateWindowW(L"STATIC",L"Hidden SMAA shader regression",WS_POPUP,0,0,side,side,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"create hidden test window");
    ComPtr<IDirect3D9> factory;factory.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    Check(factory!=nullptr,"create standard D3D9 factory");
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=side;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    Check(SUCCEEDED(factory->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device)),"create hidden hardware device");
    ComPtr<IDirect3DPixelShader9> shaders[4][4];
    const char* entries[]={"smaaEdges","smaaWeights","smaaResolve","smaaTemporal"};
    for(unsigned variant=0;variant<4;++variant)for(unsigned i=0;i<(variant<2?3u:4u);++i) {
        ComPtr<ID3DBlob> code,error;
        const D3D_SHADER_MACRO macros[]={{"MMS_SMAA_ENHANCED",variant&1?"1":"0"},
            {"MMS_SMAA_TEMPORAL",variant&2?"1":"0"},{nullptr,nullptr}};
        HRESULT hr=graphics_runtime::Compile(antialias::kSmaaShader,sizeof(antialias::kSmaaShader)-1,
            "smaa-regression",macros,nullptr,entries[i],"ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(hr) && error)std::fprintf(stderr,"%s\n",static_cast<char*>(error->GetBufferPointer()));
        Check(SUCCEEDED(hr),entries[i]);
        Check(SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&shaders[variant][i])),
            "create production SMAA pass");
    }
    ComPtr<IDirect3DTexture9> input,edges,weights,result,area,search,current,previous,motion,depth;
    ComPtr<IDirect3DSurface9> edgeSurface,weightSurface,resultSurface,currentSurface,previousSurface,cpu;
    Check(SUCCEEDED(device->CreateTexture(side,side,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&input,nullptr)),
        "allocate scene fixture");
    const auto target=[&](ComPtr<IDirect3DTexture9>& texture,ComPtr<IDirect3DSurface9>& surface) {
        Check(SUCCEEDED(device->CreateTexture(side,side,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,&texture,nullptr)) && SUCCEEDED(texture->GetSurfaceLevel(0,&surface)),"allocate SMAA target");
    };
    target(edges,edgeSurface);target(weights,weightSurface);target(result,resultSurface);
    target(current,currentSurface);target(previous,previousSurface);
    for(auto* texture:{std::addressof(motion),std::addressof(depth)}) {
        Check(SUCCEEDED(device->CreateTexture(side,side,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,
            texture->GetAddressOf(),nullptr)),"allocate standard floating-point motion fixture");
        D3DLOCKED_RECT lock{};Check(SUCCEEDED((*texture)->LockRect(0,&lock,nullptr,0)),"write motion fixture");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            auto* pixel=reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)+x*4;
            pixel[0]=pixel[1]=0;pixel[2]=pixel[3]=10;
        }
        (*texture)->UnlockRect(0);
    }
    Check(SUCCEEDED(antialias::CreateSmaaLookups(device.Get(),area,search)),"upload upstream area and search lookup data");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(side,side,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr)),
        "allocate GPU readback");
    const auto read=[&](IDirect3DSurface9* surface) {
        Check(SUCCEEDED(device->GetRenderTargetData(surface,cpu.Get())),"read rendered SMAA pixels");
        D3DLOCKED_RECT lock{};
        Check(SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"inspect GPU output");
        std::vector<DWORD> pixels(side*side);
        for(unsigned y=0;y<side;++y)std::memcpy(pixels.data()+y*side,
            static_cast<const char*>(lock.pBits)+y*lock.Pitch,side*sizeof(DWORD));
        cpu->UnlockRect();return pixels;
    };
    D3DVIEWPORT9 viewport={0,0,side,side,0,1};
    const auto fill=[&](bool flat,bool chroma) {
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(input->LockRect(0,&lock,nullptr,0)),"write fixture");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            auto* row=reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch);
            const bool outside=x<viewport.X || x>=viewport.X+viewport.Width || y<viewport.Y || y>=viewport.Y+viewport.Height;
            row[x]=outside?0xffff00ff:flat?0xff336699:
                2*(x-viewport.X)+(y-viewport.Y)<viewport.Width? (chroma?0xffd00000:0xff000000):(chroma?0xff002100:0xffffffff);
        }
        input->UnlockRect(0);
    };
    unsigned selectedVariant=0,submitted=0;
    float currentJitter=0,previousJitter=0;
    const auto render=[&] {
        const auto& v=viewport;
        const bool temporal=selectedVariant>=2;
        const float phase=temporal?float(submitted%2+1):0;
        float constants[]={1.f/side,1.f/side,float(side),float(side),
            (v.X+.5f)/side,(v.Y+.5f)/side,(v.X+v.Width-.5f)/side,(v.Y+v.Height-.5f)/side,
            phase,phase,phase,0,submitted?1.f:0,.002f,.002f,0,0,0,0,0,
            currentJitter/side,0,previousJitter/side,0};
        device->SetPixelShaderConstantF(0,constants,6);
        device->SetVertexShader(nullptr);device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
        device->SetDepthStencilSurface(nullptr);
        for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,
            D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE})
            device->SetRenderState(state,FALSE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        for(unsigned sampler=0;sampler<6;++sampler) {
            device->SetSamplerState(sampler,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
            device->SetSamplerState(sampler,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
            device->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
            device->SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,FALSE);
        }
        const auto draw=[&](unsigned pass,IDirect3DSurface9* surface,IDirect3DTexture9* source,
            IDirect3DTexture9* secondary,IDirect3DTexture9* lookup) {
            for(unsigned i=0;i<6;++i)device->SetTexture(i,nullptr);
            Check(SUCCEEDED(device->SetRenderTarget(0,surface)),"bind pass output");
            D3DVIEWPORT9 full={0,0,side,side,0,1};device->SetViewport(&full);
            Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0)),"clear pass including alpha");
            device->SetViewport(&v);device->SetTexture(0,source);device->SetTexture(1,secondary);device->SetTexture(2,lookup);
            if(pass==3) {
                device->SetTexture(3,depth.Get());device->SetTexture(4,input.Get());device->SetTexture(5,weights.Get());
                for(unsigned sampler=2;sampler<4;++sampler) {
                    device->SetSamplerState(sampler,D3DSAMP_MINFILTER,D3DTEXF_POINT);
                    device->SetSamplerState(sampler,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
                }
            }
            device->SetPixelShader(shaders[selectedVariant][pass].Get());
            struct Vertex{float x,y,z,w,u,v;};
            float l=float(v.X),t=float(v.Y),r=float(v.X+v.Width),b=float(v.Y+v.Height);
            const Vertex quad[]={{l-.5f,t-.5f,0,1,l/side,t/side},{r-.5f,t-.5f,0,1,r/side,t/side},
                {l-.5f,b-.5f,0,1,l/side,b/side},{r-.5f,b-.5f,0,1,r/side,b/side}};
            Check(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex))),"run production SMAA pass");
        };
        Check(SUCCEEDED(device->BeginScene()),"begin three-pass SMAA");
        draw(0,edgeSurface.Get(),input.Get(),nullptr,nullptr);
        draw(1,weightSurface.Get(),edges.Get(),area.Get(),search.Get());
        draw(2,temporal?currentSurface.Get():resultSurface.Get(),input.Get(),weights.Get(),temporal?motion.Get():nullptr);
        if(temporal)draw(3,resultSurface.Get(),current.Get(),previous.Get(),motion.Get());
        Check(SUCCEEDED(device->EndScene()),"finish three-pass SMAA");
        for(unsigned i=0;i<6;++i)device->SetTexture(i,nullptr);
        if(temporal) {
            Check(SUCCEEDED(device->StretchRect(currentSurface.Get(),nullptr,previousSurface.Get(),nullptr,D3DTEXF_NONE)),
                "T2x stores the individual phase, not the temporal result");
            ++submitted;
        }
        return read(resultSurface.Get());
    };
    for(selectedVariant=0;selectedVariant<4;++selectedVariant) {
    std::printf("SMAA VARIANT %u\n",selectedVariant);submitted=0;viewport={0,0,side,side,0,1};
    fill(true,false);auto pixels=render();
    Check(std::all_of(pixels.begin(),pixels.end(),[](DWORD pixel){return pixel==0xff336699;}),"flat colors remain exact");
    submitted=0;fill(false,false);pixels=render();
    unsigned smooth=0;for(DWORD pixel:pixels)if((pixel&255)>0 && (pixel&255)<255)++smooth;
    Check(smooth>side/2 && smooth<side*4,"diagonal edge gains fractional coverage without blurring flat regions");
    auto edgePixels=read(edgeSurface.Get()),weightPixels=read(weightSurface.Get());
    Check(std::any_of(edgePixels.begin(),edgePixels.end(),[](DWORD pixel){return (pixel&0xffff00)!=0;}),"edge pass detects visible discontinuities");
    Check(std::any_of(weightPixels.begin(),weightPixels.end(),[](DWORD pixel){return pixel!=0;}),"area and search pass produces nonzero blend weights");
    submitted=0;fill(false,true);pixels=render();
    Check(std::any_of(pixels.begin(),pixels.end(),[](DWORD pixel){return pixel!=0xffd00000 && pixel!=0xff002100;}),
        "color edges are filtered even at similar luminance");
    submitted=0;viewport={8,12,70,60,0,1};fill(false,false);pixels=render();
    bool clean=true;
    for(unsigned y=viewport.Y;y<viewport.Y+viewport.Height;++y)for(unsigned x=viewport.X;x<viewport.X+viewport.Width;++x) {
        const DWORD c=pixels[y*side+x];if((c&255)!=((c>>8)&255) || (c&255)!=((c>>16)&255))clean=false;
    }
    Check(clean,"inset viewport excludes magenta pixels outside the rendered scene");
    if(selectedVariant>=2) {
        auto firstWeights=read(weightSurface.Get());pixels=render();
        auto secondWeights=read(weightSurface.Get());
        Check(submitted==2 && firstWeights!=secondWeights,"T2x alternates the official two area-texture subsample indices");
    }
    }
    viewport={0,0,side,side,0,1};selectedVariant=3;
    const auto solid=[&](DWORD left,DWORD right) {
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(input->LockRect(0,&lock,nullptr,0)),"write temporal color fixture");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
            reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=x<side/2?left:right;
        input->UnlockRect(0);
    };
    const auto velocity=[&](float x,float expectedDepth,float priorDepth) {
        for(auto* texture:{std::addressof(motion),std::addressof(depth)}) {
            D3DLOCKED_RECT lock{};Check(SUCCEEDED((*texture)->LockRect(0,&lock,nullptr,0)),"update temporal motion/depth");
            for(unsigned y=0;y<side;++y)for(unsigned column=0;column<side;++column) {
                auto* pixel=reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)+column*4;
                pixel[0]=x/side;pixel[1]=0;pixel[2]=texture==std::addressof(depth)?priorDepth:10;pixel[3]=expectedDepth;
            }
            (*texture)->UnlockRect(0);
        }
    };
    const auto close=[](DWORD actual,DWORD expected) {
        for(unsigned shift=0;shift<32;shift+=8)
            if(std::abs(int((actual>>shift)&255)-int((expected>>shift)&255))>2)return false;
        return true;
    };
    submitted=0;velocity(0,10,10);solid(0x80c80000,0x80c80000);render();
    solid(0x800000c8,0x800000c8);auto temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80640064),"T2x resolves exactly two static phases and restores image alpha");
    solid(0x8000c800,0x8000c800);temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80006464),"third phase uses only the previous spatial frame, without recursive TAA history");
    velocity(0,10,5);solid(0x80c80000,0x80c80000);temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80c80000),"T2x rejects history at disoccluded depth");
    velocity(0,0,10);solid(0x800000c8,0x800000c8);temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x800000c8),"T2x rejects newly drawn geometry without previous positions");
    velocity(0,10,20);solid(0x80c80000,0x80c80000);temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80640064),"stationary edge blends the farther background seen by the other jitter phase");
    velocity(0,10,0);solid(0x8000c800,0x8000c800);temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80646400),"stationary edge retains uncovered background phase");
    for(float pixels:{4.f,-4.f}) {
        submitted=0;velocity(pixels,10,10);solid(0x80c80000,0x800000c8);render();
        solid(0x8000c800,0x8000c800);temporalPixels=render();
        const unsigned x=pixels>0?46:50;
        Check(close(temporalPixels[48*side+x],pixels>0?0x80006464:0x80646400),
            pixels>0?"T2x reprojects forward object motion":"T2x reprojects backward object motion");
    }
    // Smooth ramp isolates phase alignment from edge detection and velocity.
    D3DLOCKED_RECT ramp{};Check(SUCCEEDED(input->LockRect(0,&ramp,nullptr,0)),"write phase-alignment ramp");
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
        reinterpret_cast<DWORD*>(static_cast<char*>(ramp.pBits)+y*ramp.Pitch)[x]=D3DCOLOR_ARGB(128,x*2,x*2,x*2);
    input->UnlockRect(0);
    submitted=0;velocity(0,10,10);currentJitter=2;previousJitter=-2;
    temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80646464),"T2x first frame removes current jitter");
    temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80606060),"T2x aligns current and previous raw images using separate jitter offsets");
    velocity(0,0,10);temporalPixels=render();
    Check(close(temporalPixels[48*side+48],0x80646464),"T2x history rejection still removes current jitter");
    for(unsigned i=0;i<6;++i)device->SetTexture(i,nullptr);
    device->SetPixelShader(nullptr);device.Reset();DestroyWindow(window);
    std::puts("SMAA GPU regression checks passed");return 0;
}
