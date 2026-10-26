// Actual hidden DX9 host/FrameDriver paths. Only Present failure and process-
// local input/visibility are controlled; no plugin frame entry is called here.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include <mms/plugin.h>
#include <MinHook.h>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned checks=0;
MMDApp* app=nullptr;
IDirect3DDevice9* device=nullptr;
bool armed=false, failPresent=false;
unsigned presents=0, repaintPresents=0;
using Present=HRESULT(WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
Present originalPresent=nullptr;
decltype(&ShowWindow) originalShow=nullptr;
decltype(&IsWindowVisible) originalVisible=nullptr;
decltype(&GetKeyState) originalKey=nullptr;
decltype(&GetAsyncKeyState) originalAsyncKey=nullptr;
decltype(&GetKeyboardState) originalKeyboard=nullptr;
decltype(&GetForegroundWindow) originalForeground=nullptr;
decltype(&GetFocus) originalFocus=nullptr;
using HookCallback=void(MMS_CALL*)(void*);
using SetProbe=void(MMS_CALL*)(HookCallback,void*);
using GetCounts=void(MMS_CALL*)(uint64_t*);
SetProbe setProbe=nullptr;
GetCounts getCounts=nullptr;

void Check(bool value,const char* text) {
    ++checks;
    std::printf("%s %s\n",value?"PASS":"FAIL",text);
    if(!value) throw std::runtime_error(text);
}
template<class F> void Hook(F target,F detour,F* original=nullptr) {
    Check(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original))==MH_OK,"create process-local hook");
    Check(MH_EnableHook(reinterpret_cast<void*>(target))==MH_OK,"enable process-local hook");
}
BOOL WINAPI Hidden(HWND window,int) {return originalShow(window,SW_HIDE);}
BOOL WINAPI Available(HWND window) {
    return armed && app && window==app->MainWindow()?TRUE:originalVisible(window);
}
SHORT WINAPI Key(int key) {return armed?0:originalKey(key);}
SHORT WINAPI AsyncKey(int key) {return armed?0:originalAsyncKey(key);}
BOOL WINAPI Keyboard(PBYTE state) {if(armed){std::memset(state,0,256);return TRUE;}return originalKeyboard(state);}
HWND WINAPI Foreground() {return armed?nullptr:originalForeground();}
HWND WINAPI Focus() {return armed?nullptr:originalFocus();}
int WINAPI UnexpectedA(HWND,LPCSTR text,LPCSTR,UINT) {std::fprintf(stderr,"FAIL unexpected dialog: %s\n",text);std::exit(2);}
int WINAPI UnexpectedW(HWND,LPCWSTR text,LPCWSTR,UINT) {std::fwprintf(stderr,L"FAIL unexpected dialog: %ls\n",text);std::exit(2);}
HRESULT WINAPI OnPresent(IDirect3DDevice9* current,const RECT* source,const RECT* target,HWND window,const RGNDATA* dirty) {
    if(armed && current==device) {
        ++presents;
        if(updates::RepaintOnly())++repaintPresents;
        if(failPresent)return D3DERR_INVALIDCALL;
    }
    return originalPresent(current,source,target,window,dirty);
}
void MMS_CALL Noop(void*) {}
void Pump() {MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
std::array<uint64_t,3> Counts() {std::array<uint64_t,3> c{};getCounts(c.data());return c;}
void ResetProbe() {setProbe(Noop,nullptr);presents=repaintPresents=0;failPresent=false;}
void Frame(unsigned message) {
    app->FrameVolumeControlEnabled()=1;
    app->PlaybackActive()=0;
    app->FrameRangeDialog()=nullptr;
    app->MessageSeen()=message;
    app->DeltaTime()=1.f/30;
    FrameDriver(app);
}
void AssertCounts(uint64_t frames,uint64_t begins,uint64_t ends,const char* label) {
    const auto counts=Counts();
    std::printf("COUNTS %s frame=%llu begin=%llu end=%llu presents=%u repaint=%u\n",label,
        static_cast<unsigned long long>(counts[0]),static_cast<unsigned long long>(counts[1]),
        static_cast<unsigned long long>(counts[2]),presents,repaintPresents);
    Check(counts==std::array<uint64_t,3>{frames,begins,ends},label);
}
void Write(const fs::path& p,const char* text) {
    std::ofstream f(p,std::ios::binary);f<<text;f.close();Check(bool(f),"write fixture manifest");
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=3){std::fputs("usage: frame_phase_tests <output-parent> <lifecycle.dll>\n",stderr);return 2;}
    try {
        const auto root=fs::absolute(argv[1])/("run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        Check(!fs::exists(root),"new isolated fixture path");
        const auto packages=root/"PLUGINS",package=packages/"phase";
        Check(fs::create_directories(package),"create isolated plugin package");
        const auto dll=package/"lifecycle.dll";
        Check(fs::copy_file(fs::absolute(argv[2]),dll),"copy lifecycle fixture DLL");
        Write(package/"plugin.json",R"({"schemaVersion":1,"id":"org.example.frame-phase","name":"frame phase","version":"1.0.0","kind":"native","abi":{"major":1,"minMinor":0},"entry":"lifecycle.dll","architecture":"x64","minWindows":"6.1.7601","capabilities":["ui.command"],"requiresServices":[{"id":"mms.ui","major":1,"minMinor":0}],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})");
        Check(SetEnvironmentVariableW(L"MMS_PLUGIN_TEST_DIR",packages.c_str())!=FALSE,"set isolated package path");
        Check(SetEnvironmentVariableW(L"MMS_ABI_PROBE_LOG",(root/"plugin.log").c_str())!=FALSE,"set isolated plugin log");
        SetProcessDPIAware();
        Check(MH_Initialize()==MH_OK,"initialize process-local hooks");
        Hook(&ShowWindow,&Hidden,&originalShow);
        Hook(&IsWindowVisible,&Available,&originalVisible);
        Hook(&MessageBoxA,&UnexpectedA);Hook(&MessageBoxW,&UnexpectedW);
        Hook(&GetKeyState,&Key,&originalKey);Hook(&GetAsyncKeyState,&AsyncKey,&originalAsyncKey);
        Hook(&GetKeyboardState,&Keyboard,&originalKeyboard);
        Hook(&GetForegroundWindow,&Foreground,&originalForeground);Hook(&GetFocus,&Focus,&originalFocus);
        auto owned=std::make_unique<MMDApp>();app=owned.get();g_Block=app;
        app->state=MMDAppState{};app->InitDefaults();
        Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize hidden production host");
        Check(!originalVisible(app->MainWindow()),"main window remains hidden");
        // Exercise the empty scene's normal fixed-function renderer. An empty
        // scene intentionally does not attach the self-shadow effect.
        armed=true;
        Frame(1);
        const auto deadline=GetTickCount64()+15000;
        HMODULE module=nullptr;
        while(GetTickCount64()<deadline) {
            Pump();
            module=GetModuleHandleW(dll.c_str());
            if(module && app->Renderer() && app->Renderer()->device)break;
            Sleep(5);
        }
        Check(module!=nullptr,"real lifecycle plugin loaded");
        Check(app->Renderer() && app->Renderer()->device,"real DX9 device ready for empty-scene rendering");
        // Hidden startup does not deliver the visible-window resize. Publish
        // the real layout before asserting that the driver may present.
        HandleWindowSize(app);
        Check(FrameViewportRenderable(app),"hidden host has a valid published viewport");
        setProbe=reinterpret_cast<SetProbe>(GetProcAddress(module,"MmsTestSetRenderReentryHook"));
        getCounts=reinterpret_cast<GetCounts>(GetProcAddress(module,"MmsTestGetRenderProbeCounts"));
        Check(setProbe && getCounts,"test-only cumulative callback counters available");
        device=app->Renderer()->device;
        Check(app->Renderer()->presentParameters.Windowed!=FALSE,"windowed DX9 device only");
        auto** vtable=*reinterpret_cast<void***>(device);
        Hook(reinterpret_cast<Present>(vtable[17]),&OnPresent,&originalPresent);
        armed=true;
        ResetProbe();Frame(1);
        Check(presents==1 && repaintPresents==0,"normal driver made one actual DX9 Present");
        AssertCounts(1,1,1,"normal successful present dispatches all phases once");

        ResetProbe();Frame(0);
        Check(presents==0,"stationary driver did not present");
        AssertCounts(0,1,0,"no-present frame has BEGIN but no END");

        ResetProbe();failPresent=true;Frame(1);failPresent=false;
        Check(presents==1,"failure injected at actual DX9 Present call");
        AssertCounts(0,1,0,"failed Present produces no frame callback or END");

        ResetProbe();
        Check(updates::Initialize(app),"real update coordinator initialized");
        app->MessageSeen()=0;app->FrameVolumeControlEnabled()=1;app->PlaybackActive()=0;
        app->state.timelineAdvanceRequested=app->state.timelineAdvanceDue=0;
        app->state.depthDeviceEnabled=0;app->FrameRangeDialog()=nullptr;
        updates::RequestViewport(app);
        const auto repaintDeadline=GetTickCount64()+2000;
        while(!presents && GetTickCount64()<repaintDeadline){updates::SafePoint(app);if(!presents)Sleep(2);}
        Check(presents==1 && repaintPresents==1,"coordinator executed actual repaint-only Present");
        AssertCounts(1,1,1,"repaint success dispatches BEGIN frame and END");

        ResetProbe();
        {ScopedSceneMutation changing(*app);Frame(1);}
        Check(presents==0,"scene mutation did not present");
        AssertCounts(0,0,0,"scene mutation is rejected before BEGIN");
        setProbe(nullptr,nullptr);armed=false;
        updates::Shutdown(app);plugins::Shutdown();ShutdownCleanup(app);
        g_Block=nullptr;app->Hwnd()=nullptr;app->MainWindow()=nullptr;app=nullptr;device=nullptr;
        Check(MH_DisableHook(MH_ALL_HOOKS)==MH_OK,"disable test hooks");
        Check(MH_Uninitialize()==MH_OK,"uninitialize test hooks");
        std::printf("frame phases PASS checks=%u\n",checks);return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL %s\n",error.what());return 1;
    }
}
