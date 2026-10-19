// MAINTENANCE: regression-keep; real native outline coverage and AA state isolation.
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../src/render/outline_multisample.hpp"
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <vector>
using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned side=128;
void Check(bool ok,const char* message) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
}
struct Fixture {
    HWND window=CreateWindowW(L"STATIC",L"Hidden outline coverage regression",WS_POPUP,
        0,0,side,side,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ComPtr<IDirect3D9> d3d;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DVertexBuffer9> body,edge;
    ComPtr<IDirect3DIndexBuffer9> indices;
    ComPtr<IDirect3DSurface9> readback;
    struct MsaaTargets { ComPtr<IDirect3DSurface9> color,depth,resolved; };
    std::map<unsigned,MsaaTargets> msaa;
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    Fixture() {
        Check(window!=nullptr,"create hidden window");d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth=pp.BackBufferHeight=side;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24S8;
        Check(d3d && SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device)),"create GPU device");
        app->state=MMDAppState{};std::memset(renderer.get(),0,sizeof(*renderer));
        app->Renderer()=renderer.get();renderer->device=device.Get();renderer->d3d9=d3d.Get();
        renderer->presentParameters=pp;
        app->antialiasing.initialized=true;app->antialiasing.algorithm=antialias::Algorithm::Fxaa;
        app->antialiasing.projectOverride=true;
        model->vertexCount=4;
        Check(SUCCEEDED(device->CreateVertexBuffer(4*96,0,0,D3DPOOL_MANAGED,&body,nullptr)) &&
              SUCCEEDED(device->CreateVertexBuffer(4*sizeof(mdl::EdgeVertex),0,0,D3DPOOL_MANAGED,&edge,nullptr)) &&
              SUCCEEDED(device->CreateIndexBuffer(6*sizeof(WORD),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&indices,nullptr)),"create geometry");
        model->vertexBuffer=body.Get();model->vertexBuffer2=edge.Get();model->indexBuffer=indices.Get();
        void* data=nullptr;const WORD faces[]={0,1,2,2,1,3};
        Check(SUCCEEDED(indices->Lock(0,0,&data,0)),"lock indices");std::memcpy(data,faces,sizeof(faces));indices->Unlock();
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(side,side,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr)),"create readback");
        D3DMATRIX identity{};identity._11=identity._22=identity._33=identity._44=1;
        for(auto transform:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION})device->SetTransform(transform,&identity);
        const auto deadline=GetTickCount64()+30000;
        do {antialias::FrameScope frame(app.get());if(!antialias::Initializing(app.get()))break;Sleep(10);}
        while(GetTickCount64()<deadline);
        Check(!antialias::Initializing(app.get()),"compile AA shaders");
    }
    ~Fixture(){antialias::Release(app.get());DestroyWindow(window);}
    unsigned char* Raw(){return reinterpret_cast<unsigned char*>(model.get());}
    std::vector<DWORD> Read(IDirect3DSurface9* source=nullptr) {
        ComPtr<IDirect3DSurface9> target=source;
        if(!target)Check(SUCCEEDED(device->GetRenderTarget(0,&target)),"get scene");
        Check(SUCCEEDED(device->GetRenderTargetData(target.Get(),readback.Get())),"read actual GPU pixels");
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"lock pixels");
        std::vector<DWORD> pixels(side*side);
        for(unsigned y=0;y<side;++y)std::memcpy(pixels.data()+y*side,static_cast<char*>(lock.pBits)+y*lock.Pitch,side*4);
        readback->UnlockRect();return pixels;
    }
    std::vector<DWORD> Draw(float width,float slope,float phase,bool coverage,bool effect=false,bool disabled=false,bool occluded=false,DWORD color=0xff000000,bool resolved=false,unsigned referenceSamples=0,bool effectMrt=false) {
        mdl::SkinnedVertexBase vertices[4]{};mdl::EdgeVertex outlines[4]{};
        // White front face and black inverted hull. The right boundary is a
        // straight line with known fractional width, independent of shaders.
        for(unsigned i=0;i<4;++i) {
            const float y=(i&2)?108.f:20.f;
            const float x=(i&1)?64.f+slope*(y-64)+phase:8.f;
            auto& v=vertices[i];v.position[0]=2*x/side-1;v.position[1]=1-2*y/side;v.position[2]=.4f;
            v.normal[0]=1;
            outlines[i].position[0]=v.position[0]+2*(disabled?-width:width)/side;
            outlines[i].position[1]=v.position[1];outlines[i].position[2]=.5f;
            outlines[i].diffuse=color;
        }
        void* data=nullptr;Check(SUCCEEDED(body->Lock(0,0,&data,0)),"write body");
        const UINT bodyStride=32+16*model->pmxAdditionalUvCount;
        for(unsigned i=0;i<4;++i)std::memcpy(static_cast<char*>(data)+i*bodyStride,&vertices[i],sizeof(vertices[i]));
        body->Unlock();
        Check(SUCCEEDED(edge->Lock(0,0,&data,0)),"write hull");std::memcpy(data,outlines,sizeof(outlines));edge->Unlock();
        std::unique_ptr<antialias::FrameScope> frame;
        ComPtr<IDirect3DSurface9> previousTarget,previousDepth;
        if(referenceSamples) {
            auto& target=msaa[referenceSamples];
            if(!target.color) {
                const auto samples=static_cast<D3DMULTISAMPLE_TYPE>(referenceSamples);
                Check(SUCCEEDED(device->CreateRenderTarget(side,side,D3DFMT_A8R8G8B8,samples,0,FALSE,&target.color,nullptr)) &&
                    SUCCEEDED(device->CreateDepthStencilSurface(side,side,D3DFMT_D24S8,samples,0,TRUE,&target.depth,nullptr)) &&
                    SUCCEEDED(device->CreateRenderTarget(side,side,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target.resolved,nullptr)),
                    "create real MSAA reference targets");
            }
            device->GetRenderTarget(0,&previousTarget);device->GetDepthStencilSurface(&previousDepth);
            device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,target.color.Get());
            device->SetDepthStencilSurface(target.depth.Get());
        } else {
            frame=std::make_unique<antialias::FrameScope>(app.get());Check(frame->Ready(),"begin post-process scene");
            // Retain coverage/ink tests for the single-sample fallback. The
            // real MSAA outline layer is tested against per-pixel hardware
            // coverage in outline_multisample_tests, not this expanded hull.
            antialias::outline_multisample::Cancel(app.get());
        }
        Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xffffffff,1,0)),"clear scene");
        device->SetVertexShader(nullptr);device->SetPixelShader(nullptr);device->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_LIGHTING,FALSE);
        device->SetRenderState(D3DRS_ZENABLE,TRUE);device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS);device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetTexture(0,nullptr);
        device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,referenceSamples?TRUE:FALSE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR,0xffffffff);
        device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TFACTOR);
        Check(SUCCEEDED(device->BeginScene()),"begin draws");
        device->SetStreamSource(0,body.Get(),0,bodyStride);device->SetIndices(indices.Get());
        Check(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2)),"draw occluding front face");
        if(occluded)Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_ZBUFFER,0,.25f,0)),"place foreground occluder");
        device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE);device->SetStreamSource(0,edge.Get(),0,sizeof(outlines[0]));
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
        const float uniform[]={float((color>>16)&255)/255,float((color>>8)&255)/255,float(color&255)/255,float(color>>24)/255};
        DWORD extraWritesBefore=0;
        D3DVIEWPORT9 viewportBefore{};
        Check(SUCCEEDED(device->GetRenderState(D3DRS_COLORWRITEENABLE1,&extraWritesBefore)) &&
              SUCCEEDED(device->GetViewport(&viewportBefore)),"read outline MRT state");
        const bool drawn=coverage && antialias::DrawOutline(app.get(),Raw(),0,6,effect?uniform:nullptr);
        Check(!coverage || drawn,"production coverage draw succeeds");
        if(!drawn)Check(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2)),"legacy outline draw");
        ComPtr<IDirect3DVertexShader9> vs;ComPtr<IDirect3DPixelShader9> ps;ComPtr<IDirect3DVertexBuffer9> stream;
        UINT offset=0,stride=0;DWORD fvf=0;
        device->GetVertexShader(&vs);device->GetPixelShader(&ps);device->GetStreamSource(1,&stream,&offset,&stride);device->GetFVF(&fvf);
        Check(!vs&&!ps&&!stream&&fvf==(D3DFVF_XYZ|D3DFVF_DIFFUSE),"coverage restores shaders, declaration and secondary stream");
        ComPtr<IDirect3DSurface9> extraAfter;
        DWORD extraWritesAfter=0;
        D3DVIEWPORT9 viewportAfter{};
        const auto extraResult=device->GetRenderTarget(1,&extraAfter);
        Check(extraResult==D3DERR_NOTFOUND && !extraAfter &&
              SUCCEEDED(device->GetRenderState(D3DRS_COLORWRITEENABLE1,&extraWritesAfter)) &&
              extraWritesBefore==extraWritesAfter && SUCCEEDED(device->GetViewport(&viewportAfter)) &&
              std::memcmp(&viewportBefore,&viewportAfter,sizeof(viewportBefore))==0,
              "outline mask restores secondary target, write mask and viewport");
        Check(SUCCEEDED(device->EndScene()),"finish geometry");
        if(referenceSamples) {
            auto& target=msaa[referenceSamples];
            device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,previousTarget.Get());
            device->SetDepthStencilSurface(previousDepth.Get());
            Check(SUCCEEDED(device->StretchRect(target.color.Get(),nullptr,target.resolved.Get(),nullptr,D3DTEXF_NONE)),
                "resolve real MSAA reference");
            return Read(target.resolved.Get());
        }
        auto pixels=Read();
        ComPtr<IDirect3DSurface9> extraTarget;
        constexpr DWORD marker=0x7f203060;
        if(effectMrt) {
            Check(SUCCEEDED(device->CreateRenderTarget(side,side,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&extraTarget,nullptr)) &&
                  SUCCEEDED(device->ColorFill(extraTarget.Get(),nullptr,marker)) &&
                  SUCCEEDED(device->SetRenderTarget(1,extraTarget.Get())),"bind an effect MRT before AA resolve");
        }
        Check(frame->Complete(),"complete actual FXAA resolve");
        if(effectMrt) {
            ComPtr<IDirect3DSurface9> restored;
            Check(SUCCEEDED(device->GetRenderTarget(1,&restored)) && restored.Get()==extraTarget.Get(),
                  "resolve restores the effect MRT binding");
            const auto extraPixels=Read(extraTarget.Get());
            for(auto value:extraPixels)Check(value==marker,"AA resolve does not write to the effect MRT");
            Check(SUCCEEDED(device->SetRenderTarget(1,nullptr)),"unbind test effect MRT");
        }
        return resolved?Read():pixels;
    }
};
double Ink(const std::vector<DWORD>& pixels,unsigned y) {
    double sum=0;for(unsigned x=16;x<112;++x)sum+=1-(pixels[y*side+x]&255)/255.;return sum;
}
void Save(const std::filesystem::path& path,const std::vector<DWORD>& pixels) {
    std::ofstream out(path,std::ios::binary);out<<"P6\n"<<side<<' '<<side<<"\n255\n";
    for(auto p:pixels){const char rgb[]={char(p>>16),char(p>>8),char(p)};out.write(rgb,3);}
}
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);Fixture f;
    unsigned cases=0,oldGaps=0,newGaps=0;double maxError=0;
    for(float width:{.125f,.25f,.5f,.75f}) {
    double msaa4=0,msaa8=0,filtered=0;unsigned widthCases=0;
    for(float slope:{-.5f,0.f,.25f,.5f,1.f})for(unsigned phaseIndex=0;phaseIndex<16;++phaseIndex) {
        const float phase=phaseIndex/16.f;
        const auto old=f.Draw(width,slope,phase,false),fixed=f.Draw(width,slope,phase,true);
        double ink=0;
        for(unsigned y=24;y<104;++y){oldGaps+=Ink(old,y)<.01;newGaps+=Ink(fixed,y)<.01;ink+=Ink(fixed,y);}
        const double error=std::abs(ink/80-width);maxError=(std::max)(maxError,error);
        Check(error<.012,"continuous coverage preserves the authored subpixel width");++cases;++widthCases;
        const auto resolved=f.Draw(width,slope,phase,true,false,false,false,0xff000000,true);
        const auto reference4=f.Draw(width,slope,phase,false,false,false,false,0xff000000,false,4);
        const auto reference8=f.Draw(width,slope,phase,false,false,false,false,0xff000000,false,8);
        for(unsigned y=24;y<104;++y) {
            Check(Ink(resolved,y)>.01,"thin outline remains continuous after the actual FXAA filter");
            filtered+=Ink(resolved,y);msaa4+=Ink(reference4,y);msaa8+=Ink(reference8,y);
        }
        if(argc==2&&width==.25f&&slope==.25f&&phase==.25f) {
            const std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);
            Save(dir/L"legacy.ppm",old);Save(dir/L"coverage.ppm",fixed);
        }
    }
    const double divisor=widthCases*80;
    std::printf("WIDTH authored=%.3f fxaa=%.6f msaa4=%.6f msaa8=%.6f\n",width,filtered/divisor,msaa4/divisor,msaa8/divisor);
    Check(std::abs(filtered/divisor-msaa4/divisor)<.025 && std::abs(filtered/divisor-msaa8/divisor)<.025,
        "filtered outline weight matches real MSAA across slopes and subpixel phases");
    }
    std::printf("COVERAGE cases=%u legacy_missing_rows=%u fixed_missing_rows=%u max_opacity_error=%.6f\n",cases,oldGaps,newGaps,maxError);
    Check(oldGaps>1000&&newGaps==0,"thin outlines remain continuous across slopes and subpixel phases");
    Check(f.Draw(2,.25f,.5f,false)==f.Draw(2,.25f,.5f,true),"thick authored outlines remain pixel identical");
    Check(f.Draw(.25f,.25f,.5f,true)==f.Draw(.25f,.25f,.5f,true,true),"fixed and effect colors receive the same coverage");
    const auto reference=f.Draw(.25f,.25f,.5f,true);
    for(unsigned uv=1;uv<=4;++uv) {
        f.model->pmxAdditionalUvCount=uv;
        Check(f.Draw(.25f,.25f,.5f,true)==reference,"PMX additional UV streams preserve outline positions");
    }
    f.model->pmxAdditionalUvCount=0;
    constexpr DWORD color=0x80e02060;
    const auto colored=f.Draw(.25f,.25f,.5f,true,false,false,false,color);
    Check(colored==f.Draw(.25f,.25f,.5f,true,true,false,false,color),"colored semitransparent PMX effect and vertex colors agree");
    for(unsigned shift:{0u,8u,16u}) {
        double ink=0;
        for(unsigned y=24;y<104;++y)for(unsigned x=16;x<112;++x)ink+=1-((colored[y*side+x]>>shift)&255)/255.;
        const double expected=.25*(128./255)*(1-((color>>shift)&255)/255.);
        Check(std::abs(ink/80-expected)<.004,"coverage preserves authored RGB and alpha");
    }
    for(bool inset:{false,true}) {
        auto pixels=f.Draw(.25f,.25f,.5f,true,false,inset,!inset);
        double ink=0;for(unsigned y=24;y<104;++y)ink+=Ink(pixels,y);
        Check(ink==0,"disabled edges and foreground occlusion stay invisible");
    }
    Check(f.Draw(.25f,.25f,.5f,true,false,false,false,0xff000000,true)==
          f.Draw(.25f,.25f,.5f,true,false,false,false,0xff000000,true,0,true),
          "effect MRT isolation preserves the resolved scene");
    std::puts("PASS native outline coverage, opacity, thick lines, disabled edges, occlusion and state restoration");
}
