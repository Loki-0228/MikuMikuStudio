// MAINTENANCE: regression-keep; real native outline coverage and AA state isolation.
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../src/render/outline_multisample.hpp"
#include <MinHook.h>
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
namespace resolve_restore {
enum class Failure { ExtraTarget, StateBlock, InitialProjection, FinalProjection, AntialiasState };
using SetTarget=HRESULT(WINAPI*)(IDirect3DDevice9*,DWORD,IDirect3DSurface9*);
using SetTransform=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DTRANSFORMSTATETYPE,const D3DMATRIX*);
using SetState=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DRENDERSTATETYPE,DWORD);
using CreateBlock=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DSTATEBLOCKTYPE,IDirect3DStateBlock9**);
using ApplyBlock=HRESULT(WINAPI*)(IDirect3DStateBlock9*);
using Copy=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const RECT*,D3DTEXTUREFILTERTYPE);
SetTarget originalTarget=nullptr;
SetTransform originalTransform=nullptr;
SetState originalState=nullptr;
CreateBlock originalCreate=nullptr;
ApplyBlock originalApply=nullptr;
Copy originalCopy=nullptr;
struct Injection {
    IDirect3DDevice9* device=nullptr;
    IDirect3DSurface9* extra=nullptr;
    IDirect3DStateBlock9* outer=nullptr;
    Failure failure=Failure::ExtraTarget;
    bool armed=false,applied=false;
    unsigned rejected=0,copied=0;
} injection;
HRESULT WINAPI Target(IDirect3DDevice9* device,DWORD index,IDirect3DSurface9* surface) {
    if(injection.armed && device==injection.device && injection.failure==Failure::ExtraTarget &&
       index==1 && surface==injection.extra) {
        ++injection.rejected;return D3DERR_INVALIDCALL;
    }
    return originalTarget(device,index,surface);
}
HRESULT WINAPI Transform(IDirect3DDevice9* device,D3DTRANSFORMSTATETYPE state,const D3DMATRIX* matrix) {
    if(injection.armed && device==injection.device && state==D3DTS_PROJECTION &&
       ((injection.failure==Failure::InitialProjection && !injection.applied) ||
        (injection.failure==Failure::FinalProjection && injection.applied))) {
        ++injection.rejected;return D3DERR_INVALIDCALL;
    }
    return originalTransform(device,state,matrix);
}
HRESULT WINAPI RenderState(IDirect3DDevice9* device,D3DRENDERSTATETYPE state,DWORD value) {
    if(injection.armed && device==injection.device && injection.failure==Failure::AntialiasState &&
       injection.applied && state==D3DRS_MULTISAMPLEANTIALIAS) {
        ++injection.rejected;return D3DERR_INVALIDCALL;
    }
    return originalState(device,state,value);
}
HRESULT WINAPI Create(IDirect3DDevice9* device,D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9** block) {
    const auto result=originalCreate(device,type,block);
    // Complete's outer block precedes the motion pass's private state block.
    if(injection.armed && device==injection.device && !injection.outer && SUCCEEDED(result))injection.outer=*block;
    return result;
}
HRESULT WINAPI Apply(IDirect3DStateBlock9* block) {
    const bool outer=injection.armed && block==injection.outer;
    HRESULT result;
    if(outer && injection.failure==Failure::StateBlock) {
        ++injection.rejected;result=D3DERR_INVALIDCALL;
    } else result=originalApply(block);
    if(outer)injection.applied=true;
    return result;
}
HRESULT WINAPI Stretch(IDirect3DDevice9* device,IDirect3DSurface9* source,const RECT* sourceRect,
    IDirect3DSurface9* destination,const RECT* destinationRect,D3DTEXTUREFILTERTYPE filter) {
    const auto result=originalCopy(device,source,sourceRect,destination,destinationRect,filter);
    if(injection.armed && device==injection.device && SUCCEEDED(result) && source && destination) {
        D3DSURFACE_DESC from{},to{};
        if(SUCCEEDED(source->GetDesc(&from)) && SUCCEEDED(destination->GetDesc(&to)) &&
           from.Format==D3DFMT_A32B32G32R32F && to.Format==from.Format)++injection.copied;
    }
    return result;
}
struct Hooks {
    std::array<void*,6> targets{};
    explicit Hooks(IDirect3DDevice9* device) {
        const auto initialized=MH_Initialize();
        Check(initialized==MH_OK || initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize AA restoration fault hooks");
        ComPtr<IDirect3DStateBlock9> block;
        Check(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&block)),"read real state-block Apply entry");
        auto** table=*reinterpret_cast<void***>(device);
        auto** states=*reinterpret_cast<void***>(block.Get());
        // D3D9 COM slots from the Windows SDK interface declarations.
        targets={table[37],table[44],table[57],table[59],states[5],table[34]};
        void* callbacks[]={reinterpret_cast<void*>(&Target),reinterpret_cast<void*>(&Transform),
            reinterpret_cast<void*>(&RenderState),reinterpret_cast<void*>(&Create),reinterpret_cast<void*>(&Apply),
            reinterpret_cast<void*>(&Stretch)};
        void** originals[]={reinterpret_cast<void**>(&originalTarget),reinterpret_cast<void**>(&originalTransform),
            reinterpret_cast<void**>(&originalState),reinterpret_cast<void**>(&originalCreate),
            reinterpret_cast<void**>(&originalApply),reinterpret_cast<void**>(&originalCopy)};
        for(unsigned i=0;i<targets.size();++i)Check(MH_CreateHook(targets[i],callbacks[i],originals[i])==MH_OK &&
            MH_EnableHook(targets[i])==MH_OK,"install a real D3D9 restoration fault");
    }
    ~Hooks() {
        injection.armed=false;
        for(auto target:targets)Check(MH_DisableHook(target)==MH_OK && MH_RemoveHook(target)==MH_OK,
            "remove AA restoration fault without disturbing draw observers");
    }
};
void CheckFailures(Fixture& fixture) {
    auto* app=fixture.app.get();auto* device=fixture.device.Get();Hooks hooks(device);
    const auto warm=[&](antialias::Algorithm algorithm) {
        antialias::Release(app);app->antialiasing.algorithm=algorithm;
        const auto deadline=GetTickCount64()+30000;
        do {antialias::FrameScope frame(app);if(!antialias::Initializing(app))break;Sleep(10);}
        while(GetTickCount64()<deadline);
        Check(!antialias::Initializing(app),"compile requested restoration regression algorithm");
    };
    const auto draw=[&] {
        antialias::FrameScope frame(app);Check(frame.Ready(),"begin empty temporal scene");
        Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff607080,1,0)) &&
              SUCCEEDED(device->BeginScene()) && SUCCEEDED(device->EndScene()),"submit empty native temporal scene");
        Check(frame.Complete() && frame.Succeeded(),"resolve a healthy temporal scene");
    };
    ComPtr<IDirect3DSurface9> extra;
    Check(SUCCEEDED(device->CreateRenderTarget(side,side,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&extra,nullptr)),
        "create external MRT restoration fixture");
    for(auto algorithm:{antialias::Algorithm::Taa,antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
        Check(antialias::Supported(app,algorithm),"restoration fixture supports the requested temporal algorithm");
        warm(algorithm);
        for(auto failure:{Failure::ExtraTarget,Failure::StateBlock,Failure::InitialProjection,Failure::FinalProjection,Failure::AntialiasState}) {
            antialias::ResetHistory(app);draw();draw();
            Check(antialias::AccumulatedSamples(app)==2,"healthy temporal history is established before fault");
            ComPtr<IDirect3DSurface9> output,depth;
            D3DVIEWPORT9 viewport{};D3DMATRIX projection{};
            Check(SUCCEEDED(device->GetRenderTarget(0,&output)) && SUCCEEDED(device->GetDepthStencilSurface(&depth)) &&
                  SUCCEEDED(device->GetViewport(&viewport)) && SUCCEEDED(device->GetTransform(D3DTS_PROJECTION,&projection)),
                "capture caller output before restoration fault");
            {
                antialias::FrameScope frame(app);Check(frame.Ready(),"begin restoration failure scene");
                Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff804020,1,0)) &&
                      SUCCEEDED(device->BeginScene()) && SUCCEEDED(device->EndScene()) &&
                      SUCCEEDED(device->SetRenderTarget(1,extra.Get())),"submit scene before arming exact restoration failure");
                injection={device,extra.Get(),nullptr,failure,true,false,0,0};
                const bool completed=frame.Complete();injection.armed=false;
                Check(injection.rejected!=0 && injection.applied,"the selected real restoration call was rejected and cleanup continued");
                Check(injection.copied==(failure==Failure::InitialProjection?0u:1u),
                    "late restoration failures occur after the real temporal history depth copy");
                Check(!completed && !frame.Succeeded(),"a failed required restoration rejects the resolved frame");
                Check(antialias::AccumulatedSamples(app)==0,"a rejected frame discards temporal samples before returning");
                ComPtr<IDirect3DSurface9> actualOutput,actualDepth;D3DVIEWPORT9 actualViewport{};
                Check(SUCCEEDED(device->GetRenderTarget(0,&actualOutput)) && actualOutput.Get()==output.Get() &&
                      SUCCEEDED(device->GetDepthStencilSurface(&actualDepth)) && actualDepth.Get()==depth.Get() &&
                      SUCCEEDED(device->GetViewport(&actualViewport)) && std::memcmp(&viewport,&actualViewport,sizeof viewport)==0,
                    "independent target, depth and viewport restoration still runs after the injected failure");
                Check(SUCCEEDED(device->SetRenderTarget(1,nullptr)),"release effect MRT after restoration failure");
                // A failed restoration is allowed to leave that state unset.
                // Repair the injected projection failure before the next case.
                Check(SUCCEEDED(device->SetTransform(D3DTS_PROJECTION,&projection)),"repair caller projection after fault injection");
            }
            Check(antialias::AccumulatedSamples(app)==0,"scope cleanup does not resurrect rejected temporal samples");
            draw();Check(antialias::AccumulatedSamples(app)==1,"the next healthy frame restarts temporal accumulation");
            draw();Check(antialias::AccumulatedSamples(app)==2,"temporal accumulation resumes after restoration recovers");
        }
    }
    warm(antialias::Algorithm::Fxaa);
    std::puts("PASS temporal AA rejects target/state/projection restoration failures before history publication");
}
}
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
    resolve_restore::CheckFailures(f);
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
