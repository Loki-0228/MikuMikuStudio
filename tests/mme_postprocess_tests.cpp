#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <vector>
#include <wrl/client.h>
#include "mikumikustudio/effect_api.hpp"
using namespace mikumikustudio;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
namespace {
int frame=-1;
const char* phase="startup";
LONG CALLBACK Fault(EXCEPTION_POINTERS* p){
    if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
    const auto&r=*p->ExceptionRecord;const auto&c=*p->ContextRecord;
    MEMORY_BASIC_INFORMATION m{};VirtualQuery(r.ExceptionAddress,&m,sizeof m);
    wchar_t name[1024]{};GetModuleFileNameW(static_cast<HMODULE>(m.AllocationBase),name,1024);
    std::printf("FAULT phase=%s frame=%d code=%lx address=%p target=%llx module=%ls offset=%llx\n",phase,frame,r.ExceptionCode,r.ExceptionAddress,r.ExceptionInformation[1],name,c.Rip-reinterpret_cast<DWORD64>(m.AllocationBase));
    std::printf("RAX=%llx RBX=%llx RCX=%llx RDX=%llx RSI=%llx RDI=%llx R8=%llx R9=%llx R10=%llx R11=%llx RSP=%llx\n",c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.Rsp);
    CONTEXT walk=c;
    for(int i=0;i<24&&walk.Rip;++i){
        MEMORY_BASIC_INFORMATION info{};VirtualQuery(reinterpret_cast<void*>(walk.Rip),&info,sizeof info);
        wchar_t file[1024]{};GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase),file,1024);
        std::printf("backtrace %d %ls+%llx\n",i,file,walk.Rip-reinterpret_cast<DWORD64>(info.AllocationBase));
        DWORD64 base=0;auto entry=RtlLookupFunctionEntry(walk.Rip,&base,nullptr);
        if(entry){void*data=nullptr;DWORD64 establisher=0;RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,walk.Rip,entry,&walk,&data,&establisher,nullptr);}
        else{walk.Rip=*reinterpret_cast<DWORD64*>(walk.Rsp);walk.Rsp+=8;}
    }
    const auto*stack=reinterpret_cast<const DWORD64*>(c.Rsp);
    for(int i=0;i<40;++i)std::printf("stack+%02x=%llx\n",i*8,stack[i]);
    std::fflush(stdout);ExitProcess(86);
}
void Require(bool condition,const char* message){
    if(!condition){std::fprintf(stderr,"FAIL %s\n",message);std::exit(7);}
}
std::vector<unsigned char> Capture(IDirect3DDevice9* device,IDirect3DSurface9* target,const fs::path& path){
    ComPtr<IDirect3DSurface9> current;device->GetRenderTarget(0,&current);
    std::printf("TARGET expected=%p actual=%p cached=%p\n",target,current.Get(),g_Block->Renderer()->backbufferSurface);
    D3DSURFACE_DESC desc{};Require(SUCCEEDED(target->GetDesc(&desc)),"target description");
    ComPtr<IDirect3DSurface9> readback;
    Require(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&readback,nullptr)),"allocate readback");
    Require(SUCCEEDED(device->GetRenderTargetData(target,readback.Get())),"read actual GPU target");
    D3DLOCKED_RECT lock{};Require(SUCCEEDED(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"lock readback");
    std::vector<unsigned char> pixels(size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y)std::memcpy(pixels.data()+size_t(y)*desc.Width*4,static_cast<unsigned char*>(lock.pBits)+y*lock.Pitch,size_t(desc.Width)*4);
    readback->UnlockRect();std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
    Require(bool(file),"save BGRA render target");
    std::printf("READBACK %ux%u %ls\n",desc.Width,desc.Height,path.c_str());return pixels;
}
void Pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
void Frames(MMDApp* app,int count){
    const auto start=std::chrono::steady_clock::now();
    for(frame=0;frame<count;++frame){Pump();app->state.messageSeen=1;app->DeltaTime()=1.f/30;FrameDriver(app);}
    std::printf("PASS phase=%s frames=%d wall_ms=%.3f\n",phase,count,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
}
}
int wmain(int argc,wchar_t**argv){
    if(argc<3)return 2;std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto accessory=fs::absolute(argv[1]),out=fs::absolute(argv[2]);fs::create_directories(out);
    const auto handler=AddVectoredExceptionHandler(1,Fault);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    auto*app=new MMDApp;g_Block=app;app->state=MMDAppState{};app->InitDefaults();
    if(!InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE))return 3;
    if(IsWindowVisible(app->MainWindow()))return 4;
    std::printf("MME=%d MMV=%d\n",GetModuleHandleW(L"MMEffect.dll")!=nullptr,(GetModuleHandleW(L"MMV.dll")!=nullptr));
    Frames(app,2);
    phase="load accessory and MME FX";LoadAccessoryFile(accessory.c_str());
    if(!app->AccessorySlot(0))return 5;
    Frames(app,60);
    if(argc>3){
        if(argc>4){Require(fs::exists(argv[4]),"model fixture exists");LoadModelFile(app,fs::absolute(argv[4]).c_str(),false);Require(app->ModelSlot(0)!=nullptr,"model fixture loaded");Frames(app,2);}
        auto* device=app->Renderer()->device;
        ComPtr<IDirect3DSurface9> oldTarget,oldDepth,target,depth;
        device->GetRenderTarget(0,&oldTarget);device->GetDepthStencilSurface(&oldDepth);
        Require(SUCCEEDED(device->CreateRenderTarget(640,480,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr)),"create capture target");
        Require(SUCCEEDED(device->CreateDepthStencilSurface(640,480,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr)),"create capture depth");
        // Shadow rendering restores the renderer's cached scene surfaces.
        // Redirect that cache as well as the device, otherwise readback would
        // inspect an unused clear-only target after the first loaded object.
        auto* renderer=app->Renderer();
        ComPtr<IDirect3DSurface9> cachedTarget=renderer->backbufferSurface,cachedDepth=renderer->depthStencilSurface;
        if(renderer->backbufferSurface)renderer->backbufferSurface->Release();
        if(renderer->depthStencilSurface)renderer->depthStencilSurface->Release();
        renderer->backbufferSurface=target.Get();target->AddRef();renderer->depthStencilSurface=depth.Get();depth->AddRef();
        device->SetRenderTarget(0,target.Get());device->SetDepthStencilSurface(depth.Get());
        D3DVIEWPORT9 viewport{0,0,640,480,0,1};device->SetViewport(&viewport);
        phase="readback baseline";Frames(app,8);const auto baseline=Capture(device,target.Get(),out/L"baseline.bgra");
        bool varied=false;for(size_t i=4;i<baseline.size();i+=4)if(std::memcmp(baseline.data(),baseline.data()+i,3)){varied=true;break;}
        Require(varied,"baseline contains rendered geometry, not just a cleared target");
        auto* accessoryRecord=app->AccessorySlot(0);const auto sourceMesh=accessoryRecord->mesh;
        phase="assign MMV alongside MME";
        const auto fx=fs::absolute(argv[3]),pmm=out/L"test.pmm",vmm=out/L"test.vmm";
        std::ofstream archive(vmm,std::ios::binary);archive<<"[Info]\nType=MMV\nVersion=1\nKind=Project\nAutoLoad=true\nAutoSave=false\n\n[Binding.0]\nObjectKind=1\nObjectPath="<<accessory.generic_u8string()<<"\nInstance=0\nOwner=\nTarget=Main\nMaterial=\nEffect="<<fx.generic_u8string()<<"\nEnabled=true\nController=false\n";archive.close();
        native_mesh::ProjectOpened(app,pmm.c_str());Frames(app,60);
        Require(accessoryRecord->mesh==sourceMesh,"MMV leaves source accessory mesh unchanged");
        Require(native_mesh::RenderAccessory(accessoryRecord)!=accessoryRecord,"MMV actually publishes a render proxy");
        const auto assigned=Capture(device,target.Get(),out/L"assigned.bgra");
        if(fx.filename()==L"identity_mmv.fx")Require(assigned==baseline,"identity assignment preserves actual MME output");
        phase="reload MMV assignment";native_mesh::ProjectOpened(app,pmm.c_str());Frames(app,8);
        Capture(device,target.Get(),out/L"reloaded.bgra");
        SendMessageW(app->MainWindow(),WM_COMMAND,32601,0);Frames(app,30);
        if(auto editor=FindWindowW(L"MMVEditorWindow",nullptr))SendMessageW(editor,WM_CLOSE,0,0);
        auto saved=out/L"roundtrip.pmm";std::ofstream(saved).close();NotifyEffectFileSave(saved.c_str());
        phase="save MME assignments";Frames(app,8);
        auto emm=saved;emm.replace_extension(L".emm");Require(fs::exists(emm),"MME saves its effect assignments after MMV registration");
        std::ifstream effects(emm,std::ios::binary);const std::string text((std::istreambuf_iterator<char>(effects)),{});
        Require(text.find("[Effect@")!=std::string::npos,"MME offscreen target survives assignment transfer");
        renderer->backbufferSurface->Release();renderer->depthStencilSurface->Release();
        renderer->backbufferSurface=cachedTarget.Detach();renderer->depthStencilSurface=cachedDepth.Detach();
        device->SetRenderTarget(0,oldTarget.Get());device->SetDepthStencilSurface(oldDepth.Get());
    }
    phase="shutdown";ResetAppState(app);Frames(app,3);ShutdownCleanup(app);delete app;g_Block=nullptr;
    RemoveVectoredExceptionHandler(handler);std::puts("PASS real MME postprocess scene");return 0;
}
