// Hidden production startup, CPU compiler scheduling and unchanged GPU output.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/default_effect.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include <MinHook.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
using namespace mikumikustudio;
using namespace mikumikustudio::d3dx;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
using CreateCompiler = HRESULT(WINAPI*)(LPCVOID, UINT, const void*, const void*, DWORD, void**, void**);
CreateCompiler originalCompiler = nullptr;
HANDLE entered = nullptr, proceed = nullptr;
DWORD uiThread = 0;
std::atomic<unsigned> compilerCalls{0};
std::atomic<bool> wrongThread{false}, failCompiler{false};
HRSRC sourceResource = nullptr;
HGLOBAL sourceHandle = nullptr;
std::vector<char> effectSource;
decltype(&SizeofResource) originalResourceSize = nullptr;
decltype(&LockResource) originalResourceLock = nullptr;
DWORD WINAPI SourceSize(HMODULE module, HRSRC resource) {
    return resource == sourceResource ? DWORD(effectSource.size()) : originalResourceSize(module,resource);
}
LPVOID WINAPI SourceBytes(HGLOBAL resource) {
    return resource == sourceHandle ? effectSource.data() : originalResourceLock(resource);
}
void Check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
    std::printf("PASS %s\n", why);
}
void Hr(HRESULT value, const char* why) { Check(SUCCEEDED(value), why); }
HRESULT WINAPI GateCompiler(LPCVOID source, UINT size, const void* defines,
    const void* include, DWORD flags, void** compiler, void** errors) {
    ++compilerCalls;
    if (GetCurrentThreadId() == uiThread) wrongThread = true;
    SetEvent(entered);
    WaitForSingleObject(proceed, 10000);
    if (failCompiler) return E_FAIL;
    return originalCompiler(source, size, defines, include, flags, compiler, errors);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::fprintf(stderr, "FAIL unexpected dialog %s\n", text); std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected dialog %ls\n", text); std::exit(2);
}
void Hook(void* target, void* hook, void** original = nullptr) {
    Check(MH_CreateHook(target, hook, original) == MH_OK, "install hook");
    Check(MH_EnableHook(target) == MH_OK, "enable hook");
}
void Frame(MMDApp* app) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    app->DeltaTime() = 1.f / 30;
    app->state.messageSeen = 1;
    FrameDriver(app);
}
struct Effect {
    void* value = nullptr;
    ~Effect() { if (value) static_cast<IUnknown*>(value)->Release(); }
};
void CompareShaders(void* reference, void* actual) {
    using Handle = const char*;
    struct Description { const char* creator; UINT parameters, techniques, functions; } desc{};
    using Describe = HRESULT(WINAPI*)(void*, Description*);
    Hr(EffectMethod<Describe, 3>(actual)(actual, &desc), "read compiled effect description");
    Check(desc.techniques == 12, "all twelve techniques retained");
    struct Pass { const char* name; UINT annotations; const DWORD* vs; const DWORD* ps; };
    using Get = Handle(WINAPI*)(void*, UINT);
    using GetPass = Handle(WINAPI*)(void*, Handle, UINT);
    using DescribePass = HRESULT(WINAPI*)(void*, Handle, Pass*);
    using Size = UINT(WINAPI*)(const DWORD*);
    const auto size = reinterpret_cast<Size>(GetProcAddress(GetModuleHandleW(L"d3dx9_43.dll"), "D3DXGetShaderSize"));
    Check(size != nullptr, "resolve shader bytecode size");
    for (unsigned i = 0; i < desc.techniques; ++i) {
        Pass passes[2]{};
        void* effects[] = {reference, actual};
        for (unsigned j = 0; j < 2; ++j) {
            const auto technique = EffectMethod<Get, 12>(effects[j])(effects[j], i);
            const auto pass = EffectMethod<GetPass, 14>(effects[j])(effects[j], technique, 0);
            Hr(EffectMethod<DescribePass, 6>(effects[j])(effects[j], pass, &passes[j]), "read shader pass");
        }
        Check(size(passes[0].vs) == size(passes[1].vs) &&
              !std::memcmp(passes[0].vs, passes[1].vs, size(passes[0].vs)), "vertex bytecode matches synchronous resource effect");
        Check(size(passes[0].ps) == size(passes[1].ps) &&
              !std::memcmp(passes[0].ps, passes[1].ps, size(passes[0].ps)), "pixel bytecode matches synchronous resource effect");
    }
}
std::vector<DWORD> DrawReceiver(IDirect3DDevice9* device, void* effect) {
    constexpr unsigned size = 128;
    ComPtr<IDirect3DSurface9> previous, depth, target, readback;
    D3DVIEWPORT9 viewport{}; device->GetViewport(&viewport);
    Hr(device->GetRenderTarget(0, &previous), "save render target");
    device->GetDepthStencilSurface(&depth);
    Hr(device->CreateRenderTarget(size, size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr), "create comparison target");
    device->SetDepthStencilSurface(nullptr); device->SetRenderTarget(0, target.Get());
    ComPtr<IDirect3DTexture9> shadow;
    Hr(device->CreateTexture(32,32,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&shadow,nullptr), "create comparison shadow");
    D3DLOCKED_RECT lock{}; shadow->LockRect(0,&lock,nullptr,0);
    for (unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
        reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=x+y<32?.2f:.8f;
    shadow->UnlockRect(0);
    D3DXMATRIXF identity{}; for(unsigned i=0;i<4;++i) identity.m[i][i]=1;
    FxSetMatrix(effect,"matWorldViewProj",&identity); FxSetMatrix(effect,"matLightViewProj",&identity);
    const float white[]={1.f,1.f,1.f,.37f},dark[]={.2f,.2f,.2f,1.f},light[]={0.f,0.f,-1.f,1.f},zero[]={0.f,0.f,0.f,1.f},eye[]={0.f,0.f,4.f,1.f},map[]={32.f,32.f,0.f,0.f};
    FxSetFloatArray(effect,"EgColor",white,4); FxSetFloatArray(effect,"ToonColor",dark,4);
    FxSetFloatArray(effect,"LightDir",light,4); FxSetFloatArray(effect,"SpcColor",zero,4);
    FxSetFloatArray(effect,"Place",eye,4); FxSetFloatArray(effect,"MdsShadowMapSize",map,4);
    FxSetInt(effect,"parthf",0); FxSetInt(effect,"transp",0);
    Hr(FxSetTechnique(effect,"BufferShadowTec"), "select receiver material");
    struct Vertex {float p[3],n[3];};
    const Vertex vertices[]={{{-1,1,.5f},{0,0,1}},{{1,1,.5f},{0,0,1}},{{-1,-1,.5f},{0,0,1}},{{1,-1,.5f},{0,0,1}}};
    device->SetTexture(0,shadow.Get()); device->SetRenderState(D3DRS_ZENABLE,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE); device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    device->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL);
    Hr(device->BeginScene(),"begin comparison draw");
    UINT passes=0; Hr(FxBegin(effect,&passes),"begin receiver effect");
    for(UINT i=0;i<passes;++i) { Hr(FxBeginPass(effect,i),"begin pass"); Hr(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)),"draw receiver"); FxEndPass(effect); }
    FxEnd(effect); device->EndScene();
    Hr(device->CreateOffscreenPlainSurface(size,size,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr),"create GPU readback");
    Hr(device->GetRenderTargetData(target.Get(),readback.Get()),"read GPU pixels");
    readback->LockRect(&lock,nullptr,D3DLOCK_READONLY);
    std::vector<DWORD> pixels(size*size);
    for(unsigned y=0;y<size;++y) std::memcpy(pixels.data()+y*size,static_cast<char*>(lock.pBits)+y*lock.Pitch,size*4);
    readback->UnlockRect(); device->SetTexture(0,nullptr);
    device->SetRenderTarget(0,previous.Get()); device->SetDepthStencilSurface(depth.Get()); device->SetViewport(&viewport);
    return pixels;
}
}
int wmain(int argc, wchar_t** argv) {
    if(argc<2 || argc>3) return 2;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const fs::path directory=fs::absolute(argv[1]); fs::create_directories(directory);
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr); proceed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
#ifdef REQUIRE_MME
    SetEvent(proceed);
#endif
    uiThread=GetCurrentThreadId(); Check(MH_Initialize()==MH_OK,"initialize test hooks");
    const auto module=LoadLibraryW(L"d3dx9_43.dll");
    Hook(reinterpret_cast<void*>(GetProcAddress(module,"D3DXCreateEffectCompiler")),reinterpret_cast<void*>(&GateCompiler),reinterpret_cast<void**>(&originalCompiler));
    Hook(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(&UnexpectedA));
    Hook(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(&UnexpectedW));
    auto* app=new MMDApp(); g_Block=app; app->state=MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"hidden production startup");
    auto* renderer=app->Renderer();
#ifdef REQUIRE_MME
    Check(renderer->effect!=nullptr,"MME receives default effect before its first scene");
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),"real MME loaded");
    Frame(app);
#else
    Check(!renderer->effect && compilerCalls==0,"window initialized before shader compilation starts");
    Check(renderer->postProcessEnabled && app->SelfShadowMode()==1,"deferred effect preserves shadow capability and preference");
    Frame(app);
    Check(!renderer->effect && compilerCalls==0,"empty scene does not initialize the default shader");
#endif
    // Import an actual accessory while the compiler is still paused.
    const auto mesh=directory/L"triangle.x";
    std::ofstream(mesh) << "xof 0303txt 0032\nMesh tri {3;-5;5;0;,0;15;0;,5;5;0;;1;3;0,1,2;;MeshMaterialList {1;1;0;;Material {0.8;0.3;0.1;1.0;;1.0;0;0;0;;0;0;0;;}}}";
    LoadAccessoryFile(mesh.c_str());
    Check(app->ObjectSlot(0)!=nullptr,"import accessory");
    Frame(app);
    Check(renderer->effect!=nullptr,"first object frame attaches embedded bytecode");
#ifndef REQUIRE_MME
    Check(compilerCalls==0,"embedded default effect needs no runtime compilation");
#endif
    for(int i=0;i<3;++i)Frame(app);
#ifndef REQUIRE_MME
    Effect reference;
    void* errors=nullptr; auto& api=d3dx::Get();api.Load();
    Check(argc==3,"original shader source supplied for bytecode comparison");
    std::ifstream source(argv[2],std::ios::binary);
    effectSource.assign(std::istreambuf_iterator<char>(source),std::istreambuf_iterator<char>());
    Check(!effectSource.empty(),"read original shader source");
    const auto compilerEntry=reinterpret_cast<void*>(GetProcAddress(module,"D3DXCreateEffectCompiler"));
    Check(MH_DisableHook(compilerEntry)==MH_OK,"exclude synchronous reference from worker gate");
    Hr(api.createEffect(renderer->device,effectSource.data(),UINT(effectSource.size()),nullptr,nullptr,0,nullptr,&reference.value,&errors),"compile independent source reference");
    Check(MH_EnableHook(compilerEntry)==MH_OK,"restore worker gate");
    if(errors)static_cast<IUnknown*>(errors)->Release();
    CompareShaders(reference.value,renderer->effect);
    const auto expected=DrawReceiver(renderer->device,reference.value);
    const auto actual=DrawReceiver(renderer->device,renderer->effect);
    Check(expected==actual,"deferred effect preserves every shadow sample pixel");
    Check(actual[16*128+16]!=actual[110*128+110],"comparison includes lit and shadowed pixels");
    std::ofstream pixels(directory/L"receiver.rgba",std::ios::binary);pixels.write(reinterpret_cast<const char*>(actual.data()),actual.size()*4);
    reinterpret_cast<IUnknown*>(renderer->effect)->Release(); renderer->effect=nullptr;
    // Emulate old source-only resources, preserving the real asynchronous path.
    sourceResource=FindResourceW(nullptr,MAKEINTRESOURCEW(118),MAKEINTRESOURCEW(10));
    sourceHandle=LoadResource(nullptr,sourceResource);
    Hook(reinterpret_cast<void*>(&SizeofResource),reinterpret_cast<void*>(&SourceSize),reinterpret_cast<void**>(&originalResourceSize));
    Hook(reinterpret_cast<void*>(&LockResource),reinterpret_cast<void*>(&SourceBytes),reinterpret_cast<void**>(&originalResourceLock));
    Check(QueueDefaultEffect(renderer,118),"queue legacy source resource");
    app->SelfShadowMode()=0;
    Frame(app);
    Check(compilerCalls==0,"disabled self-shadow does not start legacy shader compilation");
    app->SelfShadowMode()=1;
    Frame(app);
    Check(WaitForSingleObject(entered,3000)==WAIT_OBJECT_0,"enabled scene starts legacy compiler");
    const auto start=GetTickCount64();for(int i=0;i<3;++i)Frame(app);
    Check(GetTickCount64()-start<1000 && !renderer->effect,"loaded scene keeps drawing while compiler is held");
    Check(!wrongThread,"compiler runs off the UI thread");
    Hr(FxOnLostDevice(reference.value),"release independent reference device state");
    Check(PostDeviceReset(app),"reset device while CPU compiler is held");
    Hr(FxOnResetDevice(reference.value),"restore independent reference device state");
    SetEvent(proceed);
    auto until=GetTickCount64()+30000;
    while(!renderer->effect && GetTickCount64()<until){Frame(app);Sleep(16);}
    Check(renderer->effect && compilerCalls==1,"completed legacy shader attaches automatically once");
    CompareShaders(reference.value,renderer->effect);
    reinterpret_cast<IUnknown*>(renderer->effect)->Release();renderer->effect=nullptr;
    ResetEvent(entered); ResetEvent(proceed); failCompiler=true;
    Check(QueueDefaultEffect(renderer,118),"queue compiler failure case");
    StartDefaultEffectCompilation(renderer);
    Check(WaitForSingleObject(entered,3000)==WAIT_OBJECT_0,"failure case reaches worker");
    SetEvent(proceed);
    until=GetTickCount64()+3000;
    while(renderer->postProcessEnabled && GetTickCount64()<until){Frame(app);Sleep(16);}
    Check(!renderer->effect && !renderer->postProcessEnabled && !app->SelfShadowMode(),
          "compiler failure disables unavailable self-shadow safely");
    Frame(app);
    Check(compilerCalls==2,"failed compilation is not retried every frame");
#endif
    Check(!IsWindowVisible(app->MainWindow()),"main window stayed hidden");
    ShutdownCleanup(app); delete app;g_Block=nullptr;
    MH_Uninitialize(); CloseHandle(entered); CloseHandle(proceed); FreeLibrary(module);
    std::puts("PASS deferred default effect integration"); return 0;
}
