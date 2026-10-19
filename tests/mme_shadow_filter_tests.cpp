#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include <MinHook.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
using namespace mikumikustudio;
using namespace mikumikustudio::d3dx;
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool ok, const char* text) { std::printf("%s %s\n",ok?"PASS":"FAIL",text);if(!ok)std::exit(1); }
decltype(&ShowWindow) realShow;
BOOL WINAPI Hide(HWND window,int) { return realShow(window,SW_HIDE); }
int WINAPI UnexpectedA(HWND,LPCSTR text,LPCSTR,UINT) { std::printf("FAIL dialog %s\n",text);std::exit(2); }
int WINAPI UnexpectedW(HWND,LPCWSTR text,LPCWSTR,UINT) { std::fwprintf(stderr,L"FAIL dialog %ls\n",text);std::exit(2); }
LRESULT CALLBACK Guard(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND){auto* c=reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;if(!(c->style&WS_CHILD)){c->style&=~WS_VISIBLE;c->x=c->y=-32000;}}
    if(code==HCBT_ACTIVATE)return 1;
    return CallNextHookEx(nullptr,code,wp,lp);
}
std::filesystem::path output;
// The supplied fixture must mix the built-in material with an MME material
// that declares sampler s0 without overriding its inherited filter states.
std::unordered_map<IDirect3DPixelShader9*, int> seen;
unsigned shadowDraws=0, inheritedDraws=0, wrongFilters=0;
using Draw=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
Draw realDraw;
HRESULT WINAPI Observe(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT count) {
    ComPtr<IDirect3DPixelShader9> shader;device->GetPixelShader(&shader);
    if(shader && !seen.count(shader.Get())){
        seen.emplace(shader.Get(),0);
        UINT bytes=0;shader->GetFunction(nullptr,&bytes);std::vector<DWORD> code((bytes+3)/4);shader->GetFunction(code.data(),&bytes);
        using Disassemble=HRESULT(WINAPI*)(const DWORD*,BOOL,LPCSTR,void**);
        void* text=nullptr;auto disassemble=reinterpret_cast<Disassemble>(GetProcAddress(d3dx::Get().module,"D3DXDisassembleShader"));
        if(disassemble && SUCCEEDED(disassemble(code.data(),FALSE,nullptr,&text))){
            using Data=const char*(WINAPI*)(void*);using Size=DWORD(WINAPI*)(void*);
            std::ofstream file(output/("shader-"+std::to_string(seen.size())+".txt"));
            const std::string assembly(EffectMethod<Data,3>(text)(text),EffectMethod<Size,4>(text)(text));
            file.write(assembly.data(),assembly.size());static_cast<IUnknown*>(text)->Release();
            if(assembly.find("DefSampler")!=std::string::npos)
                seen[shader.Get()]=assembly.find("ToonColor")!=std::string::npos?1:2;
        }
        float constants[224][4]{};device->GetPixelShaderConstantF(0,constants[0],224);
        std::ofstream file(output/("constants-"+std::to_string(seen.size())+".txt"));
        for(unsigned i=0;i<224;++i)file<<i<<" "<<constants[i][0]<<" "<<constants[i][1]<<" "<<constants[i][2]<<" "<<constants[i][3]<<"\n";
        DWORD mag=0,minimum=0;device->GetSamplerState(0,D3DSAMP_MAGFILTER,&mag);device->GetSamplerState(0,D3DSAMP_MINFILTER,&minimum);
        std::printf("DRAW shader=%zu min=%lu mag=%lu\n",seen.size(),minimum,mag);
    }
    if(shader && seen[shader.Get()]) {
        DWORD mag=0,minimum=0;device->GetSamplerState(0,D3DSAMP_MAGFILTER,&mag);device->GetSamplerState(0,D3DSAMP_MINFILTER,&minimum);
        ++shadowDraws;inheritedDraws+=seen[shader.Get()]==2;
        wrongFilters+=mag!=D3DTEXF_LINEAR||minimum!=D3DTEXF_LINEAR;
    }
    return realDraw(device,type,base,min,vertices,start,count);
}
}
int wmain(int argc,wchar_t** argv) {
    Check(argc==3,"model and output supplied");output=argv[2];std::filesystem::create_directories(output);
    std::setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize()==MH_OK,"initialize hidden guards");
    MH_CreateHook(&ShowWindow,&Hide,reinterpret_cast<void**>(&realShow));
    MH_CreateHook(&MessageBoxA,&UnexpectedA,nullptr);MH_CreateHook(&MessageBoxW,&UnexpectedW,nullptr);MH_EnableHook(MH_ALL_HOOKS);
    auto guard=SetWindowsHookExW(WH_CBT,Guard,nullptr,GetCurrentThreadId());
    auto* app=new MMDApp;g_Block=app;app->state=MMDAppState{};app->InitDefaults();
    Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize hidden host");
    Check(GetModuleHandleW(L"MMEffect.dll")!=nullptr,"real MME loaded");
    wcscpy_s(app->EnvFileName(),256,argv[1]);
    Check(std::filesystem::exists(argv[1]),"model exists");
    LoadModelFile(app,argv[1],false);
    Check(app->ModelSlot(0)!=nullptr,"model loaded");
    int mode=2;
    auto frame=[&]{MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        app->EditMode()=ViewportEditMode::Camera;app->SelfShadowMode()=mode;app->state.selfShadowEnabled=1;app->ShadowDistance()=.005f;
        app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);};
    frame();
    auto* renderer=app->Renderer();float size[4]={4096,4096,0,0},read[4]{};
    using GetArray=HRESULT(WINAPI*)(void*,const char*,float*,UINT);
    const auto set=FxSetFloatArray(renderer->effect,"MdsShadowMapSize",size,4);
    const auto get=EffectMethod<GetArray,33>(renderer->effect)(renderer->effect,"MdsShadowMapSize",read,4);
    std::printf("PARAM set=%08lx get=%08lx size=%f,%f\n",set,get,read[0],read[1]);
    Check(SUCCEEDED(set)&&SUCCEEDED(get)&&read[0]==4096&&read[1]==4096,"MME forwards native shadow map dimensions");
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    auto library=LoadLibraryW((std::filesystem::path(system)/L"d3d9.dll").c_str());
    Check(library!=nullptr,"load system D3D9");
    auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(library,"Direct3DCreate9"));
    Check(create!=nullptr,"resolve system D3D9 factory");
    ComPtr<IDirect3D9> raw;raw.Attach(create(D3D_SDK_VERSION));ComPtr<IDirect3DDevice9> dummy;
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=app->MainWindow();
    pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    Check(SUCCEEDED(raw->CreateDevice(0,D3DDEVTYPE_HAL,app->MainWindow(),D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dummy)),"obtain real D3D9 draw entry");
    auto** vt=*reinterpret_cast<void***>(dummy.Get());
    Check(MH_CreateHook(vt[82],&Observe,reinterpret_cast<void**>(&realDraw))==MH_OK && MH_EnableHook(vt[82])==MH_OK,"observe real draw shaders");
    for(bool reset:{false,true}) {
        if(reset)Check(PostDeviceReset(app),"reset production MME device");
        for(int requested:{1,2}) {
            mode=requested;seen.clear();shadowDraws=inheritedDraws=wrongFilters=0;
            for(int i=0;i<3;++i)frame();
            std::printf("SHADOW mode=%d reset=%d draws=%u inherited=%u filter_errors=%u\n",mode,reset,shadowDraws,inheritedDraws,wrongFilters);
            Check(shadowDraws>inheritedDraws&&inheritedDraws>0,"native and automatically assigned MME materials both draw");
            Check(wrongFilters==0,"inherited MME shadow sampling retains original linear filtering");
        }
    }
    MH_DisableHook(vt[82]);
    Check(!IsWindowVisible(app->MainWindow()),"host stayed hidden");
    dummy.Reset();raw.Reset();FreeLibrary(library);
    ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);MH_Uninitialize();return 0;
}
