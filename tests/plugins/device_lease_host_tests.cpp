// Real hidden DX9 host: plugin-owned default-pool resources must be released
// before Reset, and an ordinary plugin event must not re-enter release callbacks.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "../../src/app/device_lost_wait.hpp"
#include <mms/render.h>
#include <MinHook.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace mikumikustudio;
namespace fs=std::filesystem;
namespace {
unsigned checks=0;
MMDApp* app=nullptr;
IDirect3DDevice9* device=nullptr;
MmsRenderApiV1 render{};
bool armed=false, failReset=false, inOrdinaryCallback=false;
unsigned resets=0;
unsigned begins=0;
using Reset=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
Reset originalReset=nullptr;
using Begin=HRESULT(WINAPI*)(IDirect3DDevice9*);
Begin originalBegin=nullptr;
decltype(&ShowWindow) originalShow=nullptr;
decltype(&IsWindowVisible) originalVisible=nullptr;
decltype(&GetKeyState) originalKey=nullptr;
decltype(&GetAsyncKeyState) originalAsyncKey=nullptr;
decltype(&GetKeyboardState) originalKeyboard=nullptr;
decltype(&GetForegroundWindow) originalForeground=nullptr;
decltype(&GetFocus) originalFocus=nullptr;
decltype(&PostMessageW) originalPost=nullptr;
decltype(&SetTimer) originalTimer=nullptr;
bool failDevicePost=false,failDeviceTimer=false;
unsigned deniedPosts=0,deniedTimers=0;
using HookCallback=void(MMS_CALL*)(void*);
using SetProbe=void(MMS_CALL*)(HookCallback,void*);
using GetRenderApi=MmsResult(MMS_CALL*)(MmsRenderApiV1*);
using DeviceEventHook=bool(MMS_CALL*)(void*,const MmsEventV1*);
using SetDeviceEventHook=void(MMS_CALL*)(DeviceEventHook,void*);

void Check(bool value,const char* text) {
    ++checks;
    std::printf("%s %s\n",value?"PASS":"FAIL",text);
    if(!value)throw std::runtime_error(text);
}
template<class F> void Hook(F target,F detour,F* original=nullptr) {
    Check(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original))==MH_OK,"create process-local hook");
    Check(MH_EnableHook(reinterpret_cast<void*>(target))==MH_OK,"enable process-local hook");
}
BOOL WINAPI Hidden(HWND window,int) {return originalShow(window,SW_HIDE);}
BOOL WINAPI Available(HWND window) {return armed&&app&&window==app->MainWindow()?TRUE:originalVisible(window);}
SHORT WINAPI Key(int key) {return armed?0:originalKey(key);}
SHORT WINAPI AsyncKey(int key) {return armed?0:originalAsyncKey(key);}
BOOL WINAPI Keyboard(PBYTE state) {if(armed){std::memset(state,0,256);return TRUE;}return originalKeyboard(state);}
HWND WINAPI Foreground() {return armed?nullptr:originalForeground();}
HWND WINAPI Focus() {return armed?nullptr:originalFocus();}
BOOL WINAPI Post(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(failDevicePost&&app&&window==app->MainWindow()&&message==WM_APP+0x6b){++deniedPosts;return FALSE;}
    return originalPost(window,message,wp,lp);
}
UINT_PTR WINAPI Timer(HWND window,UINT_PTR id,UINT delay,TIMERPROC callback) {
    if(failDeviceTimer&&app&&window==app->MainWindow()&&id==0x4d4d536b){++deniedTimers;return 0;}
    return originalTimer(window,id,delay,callback);
}
int WINAPI UnexpectedA(HWND,LPCSTR text,LPCSTR,UINT) {std::fprintf(stderr,"FAIL unexpected dialog: %s\n",text);std::exit(2);}
int WINAPI UnexpectedW(HWND,LPCWSTR text,LPCWSTR,UINT) {std::fwprintf(stderr,L"FAIL unexpected dialog: %ls\n",text);std::exit(2);}

struct ResourceProbe {
    IDirect3DTexture9* texture=nullptr;
    MmsHandle lease=0;
    unsigned releases=0, resetsAtRelease=0;
    bool releasedInOrdinaryCallback=false, resetWithLiveResource=false;
} resource;
void MMS_CALL ReleaseResource(void* context) {
    auto& p=*static_cast<ResourceProbe*>(context);
    ++p.releases;p.resetsAtRelease=resets;
    p.releasedInOrdinaryCallback=p.releasedInOrdinaryCallback||inOrdinaryCallback;
    if(p.texture){p.texture->Release();p.texture=nullptr;}
    p.lease=0;
}
MmsRenderLeaseDescV1 Description() {
    return {{sizeof(MmsRenderLeaseDescV1),1,0},{"real.default.texture",20},ReleaseResource,&resource,MMS_RENDER_LEASE_DEVICE,0};
}
void AcquireResource() {
    Check(resource.texture==nullptr&&resource.lease==0,"previous default-pool resource was released");
    resource={};
    Check(SUCCEEDED(device->CreateTexture(64,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&resource.texture,nullptr)),
        "create actual plugin-owned default-pool texture");
    const auto desc=Description();
    Check(render.acquireLease(render.context,&desc,&resource.lease)==MMS_OK&&resource.lease,
        "register real texture through loaded plugin render API");
}
MmsRenderInfoV1 Info() {
    MmsRenderInfoV1 info{};info.h={sizeof(info),1,0};
    Check(render.query(render.context,&info)==MMS_OK,"query actual render service");return info;
}
HRESULT WINAPI OnReset(IDirect3DDevice9* current,D3DPRESENT_PARAMETERS* parameters) {
    if(armed&&current==device) {
        ++resets;
        resource.resetWithLiveResource=resource.resetWithLiveResource||resource.texture!=nullptr;
        if(failReset)return D3DERR_INVALIDCALL;
    }
    return originalReset(current,parameters);
}
HRESULT WINAPI OnBegin(IDirect3DDevice9* current) {
    if(armed&&current==device)++begins;
    return originalBegin(current);
}
void Pump() {
    MSG msg{};for(unsigned n=0;n<256&&PeekMessageW(&msg,nullptr,0,0,PM_REMOVE);++n){TranslateMessage(&msg);DispatchMessageW(&msg);}
}
void Frame() {
    app->FrameVolumeControlEnabled()=1;app->PlaybackActive()=0;
    app->FrameRangeDialog()=nullptr;app->MessageSeen()=1;app->DeltaTime()=1.f/30;
    FrameDriver(app);
}
void Write(const fs::path& path,const char* text) {
    std::ofstream file(path,std::ios::binary);file<<text;file.close();Check(bool(file),"write fixture manifest");
}
struct ReentryProbe {
    unsigned calls=0;
    bool resetSucceeded=true, resetCalled=false, releaseCalled=false;
    MmsResult acquireResult=MMS_OK, queryResult=MMS_E_FAILED;
    MmsHandle sentinel=123;
    MmsRenderInfoV1 info{};
};
void MMS_CALL ResetInsideEvent(void* context) {
    auto& p=*static_cast<ReentryProbe*>(context);++p.calls;
    inOrdinaryCallback=true;
    const auto oldResets=resets,oldReleases=resource.releases;
    p.resetSucceeded=PostDeviceReset(app);
    // Duplicate loss during the same callback must remain deferred as well.
    plugins::PluginNotifyDeviceLost(app);
    p.resetCalled=resets!=oldResets;p.releaseCalled=resource.releases!=oldReleases;
    p.info.h={sizeof(p.info),1,0};p.queryResult=render.query(render.context,&p.info);
    const auto desc=Description();p.acquireResult=render.acquireLease(render.context,&desc,&p.sentinel);
    inOrdinaryCallback=false;
}
void MMS_CALL LossInsideEvent(void* context) {
    auto& p=*static_cast<ReentryProbe*>(context);++p.calls;
    inOrdinaryCallback=true;
    const auto oldResets=resets,oldReleases=resource.releases;
    plugins::PluginNotifyDeviceLost(app);
    p.resetCalled=resets!=oldResets;p.releaseCalled=resource.releases!=oldReleases;
    p.info.h={sizeof(p.info),1,0};p.queryResult=render.query(render.context,&p.info);
    const auto desc=Description();p.acquireResult=render.acquireLease(render.context,&desc,&p.sentinel);
    inOrdinaryCallback=false;
}
void WaitForEvent(ReentryProbe& probe) {
    const auto deadline=GetTickCount64()+3000;
    while(!probe.calls&&GetTickCount64()<deadline){
        MSG msg{};
        if(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}else Sleep(2);
    }
}
struct DeviceNotices { unsigned losses=0,restores=0;uint64_t lostGeneration=0,restoredGeneration=0;bool invalid=false; };
bool MMS_CALL Notice(void* context,const MmsEventV1* event) {
    auto& p=*static_cast<DeviceNotices*>(context);
    if(!event||event->payloadVersion!=1||event->payload.bytes!=sizeof(MmsDeviceEventV1)||!event->payload.data){p.invalid=true;return true;}
    const auto generation=static_cast<const MmsDeviceEventV1*>(event->payload.data)->deviceGeneration;
    if(event->kind==MMS_EVENT_DEVICE_LOST){++p.losses;p.lostGeneration=generation;}
    else if(event->kind==MMS_EVENT_DEVICE_RESTORED){++p.restores;p.restoredGeneration=generation;}
    else p.invalid=true;
    return true;
}
struct ResetOnRestore { bool called=false,result=true; };
bool MMS_CALL ResetFromNotice(void* context,const MmsEventV1* event) {
    auto& p=*static_cast<ResetOnRestore*>(context);
    if(event->kind==MMS_EVENT_DEVICE_RESTORED&&!p.called){p.called=true;p.result=PostDeviceReset(app);}
    return true;
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=3){std::fputs("usage: device_lease_host_tests <output-parent> <lifecycle.dll>\n",stderr);return 2;}
    try {
        const auto root=fs::absolute(argv[1])/("run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        Check(!fs::exists(root),"new isolated fixture path");
        const auto packages=root/"PLUGINS",package=packages/"device-lease";
        Check(fs::create_directories(package),"create isolated plugin package");
        const auto dll=package/"lifecycle.dll";
        Check(fs::copy_file(fs::absolute(argv[2]),dll),"copy lifecycle fixture DLL");
        Write(package/"plugin.json",R"({"schemaVersion":1,"id":"org.example.device-lease-host","name":"device lease host","version":"1.0.0","kind":"native","abi":{"major":1,"minMinor":0},"entry":"lifecycle.dll","architecture":"x64","minWindows":"6.1.7601","capabilities":["ui.command"],"requiresServices":[{"id":"mms.ui","major":1,"minMinor":0}],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})");
        Check(SetEnvironmentVariableW(L"MMS_PLUGIN_TEST_DIR",packages.c_str())!=FALSE,"set isolated package path");
        Check(SetEnvironmentVariableW(L"MMS_ABI_PROBE_LOG",(root/"plugin.log").c_str())!=FALSE,"set isolated plugin log");
        SetProcessDPIAware();
        Check(MH_Initialize()==MH_OK,"initialize process-local hooks");
        Hook(&ShowWindow,&Hidden,&originalShow);Hook(&IsWindowVisible,&Available,&originalVisible);
        Hook(&MessageBoxA,&UnexpectedA);Hook(&MessageBoxW,&UnexpectedW);
        Hook(&GetKeyState,&Key,&originalKey);Hook(&GetAsyncKeyState,&AsyncKey,&originalAsyncKey);
        Hook(&GetKeyboardState,&Keyboard,&originalKeyboard);
        Hook(&GetForegroundWindow,&Foreground,&originalForeground);Hook(&GetFocus,&Focus,&originalFocus);
        Hook(&PostMessageW,&Post,&originalPost);Hook(&SetTimer,&Timer,&originalTimer);
        auto owned=std::make_unique<MMDApp>();app=owned.get();g_Block=app;
        app->state=MMDAppState{};app->InitDefaults();
        Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize hidden production host");
        Check(!originalVisible(app->MainWindow()),"main window remains hidden");
        armed=true;Frame();
        const auto deadline=GetTickCount64()+15000;HMODULE module=nullptr;
        while(GetTickCount64()<deadline){Pump();module=GetModuleHandleW(dll.c_str());if(module&&app->Renderer()&&app->Renderer()->device)break;Sleep(5);}
        Check(module!=nullptr,"real lifecycle plugin loaded");
        Check(app->Renderer()&&app->Renderer()->device,"actual DX9 device available");
        HandleWindowSize(app);device=app->Renderer()->device;
        Check(app->Renderer()->presentParameters.Windowed!=FALSE,"windowed device only");
        auto getApi=reinterpret_cast<GetRenderApi>(GetProcAddress(module,"MmsTestGetRenderApi"));
        auto setProbe=reinterpret_cast<SetProbe>(GetProcAddress(module,"MmsTestSetRenderReentryHook"));
        Check(getApi&&setProbe,"test-only API and ordinary event hook available");
        Check(getApi(&render)==MMS_OK&&render.query&&render.acquireLease&&render.releaseLease,"get loaded plugin render API");
        auto** vtable=*reinterpret_cast<void***>(device);Hook(reinterpret_cast<Reset>(vtable[16]),&OnReset,&originalReset);
        Hook(reinterpret_cast<Begin>(vtable[41]),&OnBegin,&originalBegin);

        AcquireResource();auto before=Info();unsigned oldResets=resets;
        Check(PostDeviceReset(app),"normal PostDeviceReset succeeds with leased default-pool texture");
        auto after=Info();
        Check(resets==oldResets+1,"normal reset reaches actual DX9 Reset once");
        Check(resource.releases==1&&resource.texture==nullptr&&resource.resetsAtRelease==oldResets&&!resource.resetWithLiveResource,
            "plugin resource is released before actual normal Reset");
        Check(!resource.releasedInOrdinaryCallback,"normal release is outside ordinary plugin callback");
        Check(!after.lost&&after.deviceGeneration==before.deviceGeneration+1,"successful reset restores and advances generation once");
        Pump();

        AcquireResource();before=Info();oldResets=resets;failReset=true;
        Check(!PostDeviceReset(app),"injected Reset failure is returned");after=Info();
        Check(resets==oldResets+1&&resource.releases==1&&!resource.texture&&!resource.resetWithLiveResource,
            "failed Reset still releases plugin resource before attempt");
        Check(after.lost&&after.deviceGeneration==before.deviceGeneration,"failed Reset keeps device lost and generation unchanged");
        Check(!PostDeviceReset(app),"repeat failed Reset remains failure");
        Check(resource.releases==1,"repeat loss does not repeat release callback");
        const auto desc=Description();MmsHandle sentinel=456;
        Check(render.acquireLease(render.context,&desc,&sentinel)==MMS_E_NOT_READY&&sentinel==456,
            "failed Reset rejects new leases without modifying output");
        failReset=false;Check(PostDeviceReset(app),"later actual Reset recovers");after=Info();
        Check(!after.lost&&after.deviceGeneration==before.deviceGeneration+1,"recovery advances generation once after failures");
        Pump();

        AcquireResource();before=Info();oldResets=resets;
        plugins::PluginNotifyDeviceLost(app);
        Check(resource.releases==1&&!resource.texture,"transient loss releases registered texture once");
        Check(device_lost_wait::Wait(app,device,false)==device_lost_wait::Result::Ready,
            "cooperative D3D_OK recovers transient loss without Reset");
        after=Info();
        Check(resets==oldResets&&!after.lost&&after.deviceGeneration==before.deviceGeneration+1,
            "transient recovery restores render service and advances generation once");
        plugins::PluginNotifyDeviceRestored(app);
        const auto repeated=Info();
        Check(repeated.deviceGeneration==after.deviceGeneration&&!repeated.lost&&resource.releases==1,
            "duplicate restoration does not advance generation or release again");
        Pump();

        AcquireResource();before=Info();oldResets=resets;ReentryProbe reentry;
        setProbe(ResetInsideEvent,&reentry);plugins::PluginNotifyLanguageChanged(app);
        WaitForEvent(reentry);
        setProbe(nullptr,nullptr);
        Check(reentry.calls==1,"reset attempt executed inside real plugin language event");
        Check(!reentry.resetSucceeded&&!reentry.resetCalled&&!reentry.releaseCalled,
            "ordinary callback defers Reset and resource release");
        Check(reentry.queryResult==MMS_OK&&reentry.info.lost&&reentry.info.deviceGeneration==before.deviceGeneration,
            "deferred reset immediately marks lost without advancing generation");
        Check(reentry.acquireResult==MMS_E_NOT_READY&&reentry.sentinel==123,"deferred reset immediately blocks lease acquisition");
        Check(!resource.releasedInOrdinaryCallback,"no release callback nested into language event");
        Check(resets==oldResets,"returning from event has not reset the device yet");
        Check(PostDeviceReset(app),"normal retry completes deferred reset");after=Info();
        Check(resets==oldResets+1&&resource.releases==1&&!resource.texture&&!resource.resetWithLiveResource,
            "deferred resource released exactly once before retry Reset");
        Check(!resource.releasedInOrdinaryCallback&&!after.lost&&after.deviceGeneration==before.deviceGeneration+1,
            "retry restores generation outside ordinary plugin callback");
        Pump();

        AcquireResource();before=Info();oldResets=resets;ReentryProbe lossOnly;
        setProbe(LossInsideEvent,&lossOnly);plugins::PluginNotifyLanguageChanged(app);
        WaitForEvent(lossOnly);setProbe(nullptr,nullptr);
        Check(lossOnly.calls==1&&!lossOnly.resetCalled&&!lossOnly.releaseCalled,
            "loss-only ordinary callback neither resets nor releases recursively");
        Check(lossOnly.queryResult==MMS_OK&&lossOnly.info.lost&&lossOnly.info.deviceGeneration==before.deviceGeneration&&
            lossOnly.acquireResult==MMS_E_NOT_READY&&lossOnly.sentinel==123,
            "loss-only callback publishes lost state and blocks replacement leases immediately");
        const auto releaseDeadline=GetTickCount64()+3000;
        while(!resource.releases&&GetTickCount64()<releaseDeadline){Pump();Sleep(2);}
        Check(resource.releases==1&&!resource.texture&&!resource.releasedInOrdinaryCallback&&resets==oldResets,
            "independent safe-boundary wake releases loss-only lease outside plugin callback");
        Check(device_lost_wait::Wait(app,device,false)==device_lost_wait::Result::Ready,
            "loss-only event can recover through cooperative ready path");
        after=Info();
        Check(!after.lost&&after.deviceGeneration==before.deviceGeneration+1&&resets==oldResets,
            "loss-only recovery restores service without Reset");
        Pump();

        AcquireResource();
        Check(render.releaseLease(render.context,resource.lease)==MMS_OK,"explicit lease cancellation succeeds after recovery");
        resource.lease=0;resource.texture->Release();resource.texture=nullptr;
        Check(resource.releases==0,"explicit cancellation does not invoke release callback");

        auto setNotice=reinterpret_cast<SetDeviceEventHook>(GetProcAddress(module,"MmsTestSetDeviceEventHook"));
        Check(setNotice!=nullptr,"test device notification observer available");
        DeviceNotices notices;setNotice(Notice,&notices);before=Info();
        failDevicePost=true;
        plugins::PluginNotifyDeviceLost(app);plugins::PluginNotifyDeviceRestored(app);
        Check(deniedPosts>=2&&!notices.losses&&!notices.restores,"failed posts retain device notices without synchronous dispatch");
        const auto noticeDeadline=GetTickCount64()+3000;
        while(!notices.restores&&GetTickCount64()<noticeDeadline){Pump();Sleep(2);}
        Check(notices.losses==1&&notices.restores==1&&!notices.invalid&&
            notices.lostGeneration==before.deviceGeneration&&notices.restoredGeneration==before.deviceGeneration+1,
            "retry timer delivers loss and current restoration after PostMessage failure");
        Pump();Check(notices.losses==1&&notices.restores==1,"retry timer does not duplicate completed notices");

        notices={};before=Info();failDeviceTimer=true;
        plugins::PluginNotifyDeviceLost(app);plugins::PluginNotifyDeviceRestored(app);
        Check(deniedTimers>=2&&!notices.losses&&!notices.restores,"post and timer failures preserve notices until next safe frame");
        plugins::PluginFrameBegin(app);
        Check(notices.losses==1&&notices.restores==1&&!notices.invalid&&notices.restoredGeneration==before.deviceGeneration+1,
            "frame boundary delivers retained device notices after both wake mechanisms fail");
        failDevicePost=failDeviceTimer=false;setNotice(nullptr,nullptr);

        const auto beforeBaseline=begins;Frame();
        Check(begins>beforeBaseline,"ordinary frame reaches actual BeginScene before fallback regression");
        ResetOnRestore resetFromNotice;setNotice(ResetFromNotice,&resetFromNotice);
        failDevicePost=failDeviceTimer=true;
        plugins::PluginNotifyDeviceLost(app);plugins::PluginNotifyDeviceRestored(app);
        const auto beforeBlocked=begins;oldResets=resets;
        Frame();
        Check(resetFromNotice.called&&!resetFromNotice.result,"frame fallback delivers restore callback that requests deferred Reset");
        Check(begins==beforeBlocked&&resets==oldResets&&device_lost_wait::Pending(app),
            "frame stops before drawing when delivered callback creates pending recovery");
        failDevicePost=failDeviceTimer=false;setNotice(nullptr,nullptr);
        Check(PostDeviceReset(app),"deferred frame-boundary reset can recover normally");

        armed=false;updates::Shutdown(app);plugins::Shutdown();ShutdownCleanup(app);
        g_Block=nullptr;app->Hwnd()=nullptr;app->MainWindow()=nullptr;app=nullptr;device=nullptr;render={};
        Check(MH_DisableHook(MH_ALL_HOOKS)==MH_OK,"disable test hooks");
        Check(MH_Uninitialize()==MH_OK,"uninitialize test hooks");
        std::printf("device lease host PASS checks=%u\n",checks);return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL %s\n",error.what());return 1;
    }
}
