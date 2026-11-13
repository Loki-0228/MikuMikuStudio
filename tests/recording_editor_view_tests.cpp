// MAINTENANCE: regression-keep. Independent view oracle; native exclusive is explicit opt-in.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <vfw.h>
#include <MinHook.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/model_layout.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/default_effect.hpp"
#include "mikumikustudio/frame_sampling.hpp"
#include "mikumikustudio/recording_output_path.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "export_save_workflow_checks.hpp"
#include "../src/app/device_lost_wait.hpp"

namespace mikumikustudio {
void RefreshAfterFrameApply(MMDApp*);
int FillCodecCombo(DShowRecorder*, HWND, char);
void RebindCodecOnComboChange(DShowRecorder*, int, HWND, char);
bool BuildRecordingGraph(DShowRecorder*,HWND,unsigned char,void*,void*,float,std::uint32_t,
    std::uint32_t,std::uint64_t,std::uint64_t,const WaveAudioContext*,unsigned char,
    const recording_output_path::Selection*);
HRESULT ChooseStillImagePath(MMDApp*, HWND, wchar_t*, std::size_t,
    std::optional<image_sequence::StillFileIdentity>&);
}

using namespace mikumikustudio;
namespace fs=std::filesystem;
namespace seq=mikumikustudio::image_sequence;
namespace {
unsigned checks=0;
DWORD hostThread=0;
fs::path output,installation;
HWND exclusiveWindow=nullptr;
HWND lockedOutputOwner=nullptr;
fs::path lockedOutputPath;
unsigned lockedOutputWarnings=0;
HWND pendingEffectOwner=nullptr;
unsigned pendingEffectWarnings=0;
bool expectingMissingCompressor=false;
unsigned missingCompressorWarnings=0;
HWND captureTargetErrorOwner=nullptr;
unsigned captureTargetErrors=0;
void Check(bool value,const char* label) {
    ++checks;
    if(!value) { std::fprintf(stderr,"FAIL recording-view checks=%u: %s\n",checks,label); std::exit(1); }
}
bool Owned(HWND window) {
    DWORD process=0; const auto thread=GetWindowThreadProcessId(window,&process);
    return window&&process==GetCurrentProcessId()&&thread==hostThread;
}
bool ExclusiveWindowOrChild(HWND window) {
    return exclusiveWindow&&(window==exclusiveWindow||IsChild(exclusiveWindow,window));
}
void HiddenWindows() {
    Check(!GetFocus()&&!GetActiveWindow()&&!GetCapture(),"no test focus, activation or capture");
    EnumThreadWindows(hostThread,[](HWND window,LPARAM)->BOOL {
        Check(Owned(window)&&!IsWindowVisible(window),"all owned top-level windows stay hidden"); return TRUE;
    },0);
    for(const auto* name:{L"MMEffect.dll",L"MMHack.dll",L"MMV4MMS.dll"})
        Check(!GetModuleHandleW(name),"no third-party effect or compatibility module");
}
void Sandbox(const wchar_t* argument) {
    wchar_t desktop[256]{}; DWORD size=0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof desktop,&size)&&
        !_wcsicmp(desktop,L"Default"),"inherit Default desktop without switching");
    hostThread=GetCurrentThreadId();
    installation=local_data::ExecutableDirectory(); Check(!installation.empty(),"actual copied executable location");
    std::ifstream marker(installation/L"recording-view-test-installation.txt",std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(marker),std::istreambuf_iterator<char>()};
    Check(text=="MMS recording view test installation v1\n","exact independent installation marker");
    Check(!fs::exists(installation/L"data")&&!fs::exists(installation/L"PLUGINS"),"fresh installation has no data or plugins");
    for(const auto* name:{L"d3d9.dll",L"MMEffect.dll",L"MMHack.dll",L"MMV4MMS.dll"})
        Check(!fs::exists(installation/name),"installation has no graphics or effect proxy");
    output=fs::path(argument).lexically_normal();
    Check(output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path())&&output!=installation&&
        output!=installation/L"data","new absolute evidence directory distinct from installation");
    Check(fs::create_directory(output),"create owned output directory");
    for(const auto* name:{L"cwd",L"temp",L"local",L"roaming",L"profile"})
        Check(fs::create_directory(output/name),"create owned process sandbox");
    Check(SetCurrentDirectoryW((output/L"cwd").c_str()),"use owned working directory");
    for(const auto* name:{L"TEMP",L"TMP"}) {
        Check(SetEnvironmentVariableW(name,(output/L"temp").c_str()),"isolate Win32 temporary directory");
        Check(_wputenv_s(name,(output/L"temp").c_str())==0,"isolate CRT temporary directory");
    }
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA",(output/L"local").c_str())&&
        SetEnvironmentVariableW(L"APPDATA",(output/L"roaming").c_str())&&
        SetEnvironmentVariableW(L"USERPROFILE",(output/L"profile").c_str()),"isolate profile fallbacks");
    Check(SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX",L"0"),"disable vendor-specific diagnostic path");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
}

// This layout is test-owned; neither production's snapshot nor its helper is the oracle.
struct View {
    std::array<float,10> numbers{};
    std::uint8_t perspective=0,suppressed=0;
    int parent=-1,bone=0;
    CameraAttachmentReference reference=CameraAttachmentReference::None;
    std::array<float,16> basis{};
};
View Read(MMDApp& a) {
    View v; v.numbers={a.CameraPositionX(),a.CameraPositionY(),a.CameraPositionZ(),
        a.CameraPitch(),a.CameraYaw(),a.CameraRoll(),a.ViewOffsetX(),a.ViewOffsetY(),a.CameraDistance(),a.CameraFov()};
    v.perspective=a.CameraPerspective();v.suppressed=a.CameraAttachmentTransformSuppressed();
    v.parent=a.CameraParentModel();v.bone=a.CameraParentBone();v.reference=a.CameraReferenceMode();
    std::memcpy(v.basis.data(),&a.CameraAttachmentBasis(),sizeof(D3DMATRIX));return v;
}
void Write(MMDApp& a,const View& v) {
    for(int i=0;i<3;++i){a.CameraPositionAxis(i)=v.numbers[i];a.CameraRotation()[i]=v.numbers[i+3];}
    a.ViewOffsetX()=v.numbers[6];a.ViewOffsetY()=v.numbers[7];a.CameraDistance()=v.numbers[8];a.CameraFov()=v.numbers[9];
    a.CameraPerspective()=v.perspective;a.CameraAttachmentTransformSuppressed()=v.suppressed;
    a.CameraParentModel()=v.parent;a.CameraParentBone()=v.bone;a.CameraReferenceMode()=v.reference;
    std::memcpy(&a.CameraAttachmentBasis(),v.basis.data(),sizeof(D3DMATRIX));
}
bool Same(const View& a,const View& b) {
    return a.numbers==b.numbers&&a.perspective==b.perspective&&a.suppressed==b.suppressed&&
        a.parent==b.parent&&a.bone==b.bone&&a.reference==b.reference&&a.basis==b.basis;
}
View Seed(float offset,int reference=0,bool renderable=false) {
    View v; v.numbers={3+offset,19+offset,-4-offset,-.15f,.21f,.03f,2.5f,11.f,-53.f,44.f};
    v.reference=static_cast<CameraAttachmentReference>(reference);v.perspective=static_cast<std::uint8_t>(reference%2);
    v.suppressed=static_cast<std::uint8_t>(reference%2);
    for(unsigned i=0;i<v.basis.size();++i)v.basis[i]=renderable?(i%5==0?1.f:0.f):float(i+1)*.125f+offset;
    return v;
}
std::unique_ptr<MMDApp> PlainApp() {
    auto a=std::make_unique<MMDApp>();new (&a->state) MMDAppState{};a->InitDefaults();return a;
}
void Fields() {
    auto a=PlainApp(),other=PlainApp();
    for(int reference=0;reference!=3;++reference)for(int perspective=0;perspective!=2;++perspective) {
        auto before=Seed(1.f,reference);before.perspective=static_cast<std::uint8_t>(perspective);
        auto changed=Seed(9.f,(reference+1)%3);changed.perspective=static_cast<std::uint8_t>(1-perspective);
        a->CameraMode()=0;a->state.followCameraEnabled=0;Write(*a,before);
        const auto revision=a->SceneRevision();const bool modified=a->SceneModified()!=0;
        a->CaptureRecordingEditorView();Write(*a,changed);
        a->CaptureRecordingEditorView(); // Reentry must not replace the first capture.
        a->RestoreRecordingEditorView();
        Check(Same(Read(*a),before),"all editor fields restored from first capture, including matrix and flags");
        Check(a->SceneRevision()==revision&&a->SceneModified()==modified,"view restoration is not a project edit");
        Write(*a,changed);a->RestoreRecordingEditorView();
        Check(Same(Read(*a),changed),"second restore does not overwrite subsequent view edits");
    }
    for(int mode:{0,1})for(int follow:{0,1})if(mode||follow) {
        a->CameraMode()=mode;a->state.followCameraEnabled=follow;
        Write(*a,Seed(2));a->CaptureRecordingEditorView();const auto changed=Seed(12);Write(*a,changed);
        a->RestoreRecordingEditorView();Check(Same(Read(*a),changed),"camera mode and follow mode are not captured");
        Check(a->CameraMode()==mode&&a->state.followCameraEnabled==follow,"excluded mode and follow flags unchanged");
    }
    a->CameraMode()=0;a->state.followCameraEnabled=0;
    Write(*a,Seed(3));a->CaptureRecordingEditorView();a->AdvanceSceneGeneration();
    const auto replacement=Seed(13);Write(*a,replacement);a->RestoreRecordingEditorView();
    Check(Same(Read(*a),replacement),"scene replacement rejects stale capture");
    Write(*a,Seed(14));a->RestoreRecordingEditorView();Check(Same(Read(*a),Seed(14)),"stale capture is consumed once");
    Write(*a,Seed(15));a->CaptureRecordingEditorView();a->AdvanceSceneGeneration();
    Write(*a,Seed(16));a->CaptureRecordingEditorView();Write(*a,Seed(17));a->RestoreRecordingEditorView();
    Check(Same(Read(*a),Seed(16)),"new scene can replace a stale capture without an intervening restore");
    Write(*a,Seed(4));a->CaptureRecordingEditorView();
    other->CameraMode()=0;other->state.followCameraEnabled=0;Write(*other,Seed(5));other->CaptureRecordingEditorView();
    Write(*a,Seed(40));Write(*other,Seed(50));a->RestoreRecordingEditorView();other->RestoreRecordingEditorView();
    Check(Same(Read(*a),Seed(4))&&Same(Read(*other),Seed(5)),"two application instances retain independent captures");
    HiddenWindows();Check(!fs::exists(installation/L"data"),"field-only checks do not initialize product storage");
}

decltype(&ShowWindow) realShow=nullptr;
decltype(&SetWindowPos) realPosition=nullptr;
decltype(&SetWindowPlacement) realPlacement=nullptr;
decltype(&SetFocus) realFocus=nullptr;
decltype(&SetActiveWindow) realActive=nullptr;
decltype(&SetForegroundWindow) realForeground=nullptr;
decltype(&MessageBoxW) realMessageW=nullptr;
decltype(&MessageBoxA) realMessageA=nullptr;
struct AviSaveRetryProbe {
    MMDApp* app = nullptr;
    fs::path video;
    DLGPROC settingsProc = nullptr;
    WNDPROC windowProc = nullptr;
    bool allowSettingsWindow = false;
    unsigned pickerCalls = 0, settingsWindows = 0, readbacks = 0, closePosts = 0, closeDeliveries = 0;
};
AviSaveRetryProbe aviSaveRetry;
export_save_workflow::SaveRetry aviProjectSave;
MMDApp* earlyWindowApp=nullptr;
unsigned rejectedRecordWindows=0,expectedWindowErrors=0,earlyStopCalls=0,earlyApplyCalls=0;
decltype(&CreateWindowExA) realCreateA=nullptr;
decltype(&StopPlayback) realStop=nullptr;
HWND WINAPI FailRecordWindow(DWORD extended,LPCSTR className,LPCSTR title,DWORD style,
    int x,int y,int width,int height,HWND parent,HMENU menu,HINSTANCE instance,LPVOID parameter){
    if(GetCurrentThreadId()==hostThread&&earlyWindowApp&&
        reinterpret_cast<std::uintptr_t>(className)>0xffff&&std::strcmp(className,"RecWindow")==0&&
        instance==static_cast<HINSTANCE>(earlyWindowApp->HInstance())&&!parent&&!menu&&!parameter){
        ++rejectedRecordWindows;
        Check(earlyWindowApp->RecordingStartGeneration()!=0&&!earlyWindowApp->RecordingPlaybackEntered(),
            "injected window failure occurs during a real admitted start before playback");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;
    }
    return realCreateA(extended,className,title,style,x,y,width,height,parent,menu,instance,parameter);
}
void ObserveStop(MMDApp* app){
    if(GetCurrentThreadId()==hostThread&&app==earlyWindowApp)++earlyStopCalls;
    realStop(app); // Observe rather than suppress a potentially destructive production call.
}
MMDApp* deferredFinishObserver=nullptr;unsigned deferredFinishShows=0;
BOOL WINAPI HiddenShow(HWND w,int command){
    if(GetCurrentThreadId()==hostThread&&deferredFinishObserver&&w==deferredFinishObserver->MainWindow()&&command==SW_SHOW){
        Check(!recording_state::Guard::Active(deferredFinishObserver,recording_state::Operation::GraphBuild),
            "finish side effects occur only after the graph-build guard exits");
        ++deferredFinishShows;
    }
    return realShow(w,GetCurrentThreadId()==hostThread&&Owned(w)&&!ExclusiveWindowOrChild(w)?SW_HIDE:command);
}
BOOL WINAPI HiddenPosition(HWND w,HWND after,int x,int y,int width,int height,UINT flags){
    if(GetCurrentThreadId()==hostThread&&Owned(w)&&!ExclusiveWindowOrChild(w))
        flags=(flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
    return realPosition(w,after,x,y,width,height,flags);
}
BOOL WINAPI HiddenPlacement(HWND w,const WINDOWPLACEMENT* p){
    if(GetCurrentThreadId()==hostThread&&Owned(w)&&p&&!ExclusiveWindowOrChild(w)){auto v=*p;v.showCmd=SW_HIDE;return realPlacement(w,&v);}
    return realPlacement(w,p);
}
HWND WINAPI NoFocus(HWND w){return GetCurrentThreadId()==hostThread&&(!w||Owned(w))&&!ExclusiveWindowOrChild(w)?GetFocus():realFocus(w);}
HWND WINAPI NoActive(HWND w){return GetCurrentThreadId()==hostThread&&(!w||Owned(w))&&!ExclusiveWindowOrChild(w)?GetActiveWindow():realActive(w);}
BOOL WINAPI NoForeground(HWND w){return GetCurrentThreadId()==hostThread&&Owned(w)&&!ExclusiveWindowOrChild(w)?FALSE:realForeground(w);}
int WINAPI RejectMessageW(HWND w,LPCWSTR text,LPCWSTR caption,UINT flags){
    if(aviProjectSave.AcceptFailure(w,text,caption,flags))return IDOK;
    if (captureTargetErrorOwner && w == captureTargetErrorOwner &&
        GetCurrentThreadId() == hostThread && flags == (MB_OK | MB_ICONERROR) &&
        text && caption && !wcscmp(caption, L"MMS") &&
        !wcscmp(text, L"Could not prepare the export render target. Export was canceled.")) {
        ++captureTargetErrors;
        return IDOK;
    }
    if(GetCurrentThreadId()==hostThread&&expectingMissingCompressor&&Owned(w)&&
        text&&caption&&flags==(MB_OK|MB_ICONERROR)&&!wcscmp(caption,L"DirectShow")&&
        !wcscmp(text,L"The selected AVI compressor is unavailable. Select another codec and retry.")){
        ++missingCompressorWarnings;
        return IDOK;
    }
    if(GetCurrentThreadId()==hostThread&&pendingEffectOwner&&w==pendingEffectOwner&&
        text&&caption&&flags==(MB_OK|MB_ICONINFORMATION)&&!wcscmp(caption,L"MMS")&&
        !wcscmp(text,L"The self-shadow shader is compiling. Please try exporting again shortly.")){
        ++pendingEffectWarnings;
        return IDOK;
    }
    if(GetCurrentThreadId()==hostThread&&lockedOutputOwner&&w==lockedOutputOwner&&
        text&&caption&&flags==(MB_OK|MB_ICONERROR)&&
        !wcscmp(caption,recording_output_path::FailureCaption(1))&&
        wcsstr(text,lockedOutputPath.c_str())&&wcsstr(text,L"Win32 32:")){
        ++lockedOutputWarnings;
        return IDOK;
    }
    if(GetCurrentThreadId()==hostThread&&(!w||Owned(w))){
        std::fwprintf(stderr,L"Unexpected owned dialog: %ls / %ls\n",caption?caption:L"",text?text:L"");
        Check(false,"unexpected dialog is refused, never automatically answered");
    }return realMessageW(w,text,caption,flags);
}
int WINAPI RejectMessageA(HWND w,LPCSTR text,LPCSTR caption,UINT flags){
    if(GetCurrentThreadId()==hostThread&&earlyWindowApp&&w==earlyWindowApp->MainWindow()&&
        rejectedRecordWindows==1&&expectedWindowErrors==0&&text&&caption&&flags==0&&
        std::strcmp(text,"CreateWindow failed")==0&&std::strcmp(caption,"create main window")==0){
        ++expectedWindowErrors;return IDOK; // This exact injected error only; no dialog is created.
    }
    if(GetCurrentThreadId()==hostThread&&(!w||Owned(w))){
        std::fprintf(stderr,"Unexpected owned dialog: %s / %s\n",caption?caption:"",text?text:"");
        Check(false,"unexpected ANSI dialog is refused, never automatically answered");
    }return realMessageA(w,text,caption,flags);
}
LRESULT CALLBACK HiddenCreate(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_CREATEWND){
        auto* c=reinterpret_cast<CBT_CREATEWND*>(lp);c->lpcs->style&=~WS_VISIBLE;c->lpcs->dwExStyle|=WS_EX_NOACTIVATE;
        wchar_t cls[64]{};GetClassNameW(reinterpret_cast<HWND>(wp),cls,64);
        if(wcscmp(cls,L"#32770")==0){
            Check(aviSaveRetry.allowSettingsWindow&&aviSaveRetry.app&&
                  c->lpcs->hwndParent==aviSaveRetry.app->MainWindow()&&aviSaveRetry.settingsWindows==0,
                  "no unexpected modal dialog may be created");
            ++aviSaveRetry.settingsWindows;
        }
    }
    if((code==HCBT_ACTIVATE||code==HCBT_SETFOCUS)&&
        !ExclusiveWindowOrChild(reinterpret_cast<HWND>(wp)))return 1;
    return CallNextHookEx(nullptr,code,wp,lp);
}
struct Hooks {
    std::vector<void*> targets;HHOOK cbt=nullptr;
    template<class F>void Add(F target,F replacement,F& original){
        Check(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),
            reinterpret_cast<void**>(&original))==MH_OK,"create owned process-local hook");
        targets.push_back(reinterpret_cast<void*>(target));
        Check(MH_EnableHook(reinterpret_cast<void*>(target))==MH_OK,"enable explicit hook");
    }
    Hooks(){
        Check(MH_Initialize()==MH_OK,"initialize test detours");
        Add(&ShowWindow,&HiddenShow,realShow);Add(&SetWindowPos,&HiddenPosition,realPosition);
        Add(&SetWindowPlacement,&HiddenPlacement,realPlacement);Add(&SetFocus,&NoFocus,realFocus);
        Add(&SetActiveWindow,&NoActive,realActive);Add(&SetForegroundWindow,&NoForeground,realForeground);
        Add(&MessageBoxW,&RejectMessageW,realMessageW);Add(&MessageBoxA,&RejectMessageA,realMessageA);
        cbt=SetWindowsHookExW(WH_CBT,HiddenCreate,nullptr,hostThread);Check(cbt!=nullptr,"only current-thread CBT hook");
    }
    ~Hooks(){
        Check(UnhookWindowsHookEx(cbt),"remove owned CBT hook");
        for(auto at=targets.rbegin();at!=targets.rend();++at)
            Check(MH_DisableHook(*at)==MH_OK&&MH_RemoveHook(*at)==MH_OK,"remove owned detour");
        Check(MH_Uninitialize()==MH_OK,"release detour runtime after host shutdown");
    }
};

void OwnBytes(const fs::path& path,const std::vector<unsigned char>& bytes){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"create new fixture without replacing an existing file");
    DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=FALSE;
    const bool closed=CloseHandle(file)!=FALSE;
    Check(ok&&written==bytes.size()&&closed,"write complete owned fixture");
}
fs::path ModelFixture(){
    std::vector<unsigned char> b;
    const auto u8=[&](unsigned x){b.push_back(static_cast<unsigned char>(x));};
    const auto u16=[&](unsigned x){u8(x);u8(x>>8);};
    const auto u32=[&](std::uint32_t x){for(unsigned i=0;i<4;++i)u8(x>>(8*i));};
    const auto f32=[&](float x){std::uint32_t bits;std::memcpy(&bits,&x,4);u32(bits);};
    const auto text=[&](const char* x,unsigned n){const auto len=std::strlen(x);for(unsigned i=0;i<n;++i)u8(i<len?x[i]:0);};
    text("Pmd",3);f32(1);text("view fixture",20);text("",256);u32(4);
    const float vertices[4][4]={{-6,4,0,1},{-6,16,0,0},{6,4,1,1},{6,16,1,0}};
    for(const auto& v:vertices){for(float x:{v[0],v[1],0.f,0.f,0.f,-1.f,v[2],v[3]})f32(x);u16(0);u16(0);u8(100);u8(0);}
    u32(6);for(unsigned i:{0,1,2,2,1,3})u16(i);u32(1);
    for(float x:{.8f,.25f,.1f,1.f,1.f,0.f,0.f,0.f,.4f,.4f,.4f})f32(x);
    u8(255);u8(0);u32(6);text("",20);
    u16(1);text("root",20);u16(65535);u16(0);u8(0);u16(0);f32(0);f32(0);f32(0);
    u16(0);u16(0);u8(0);u8(0);u32(0);u8(0);text("",1000);u32(0);u32(0);
    const auto path=output/L"fixture.pmd";OwnBytes(path,b);return path;
}
void WaitForEffect(MMDApp* app){
    if(DefaultEffectReadyForCapture(app))return;
    std::puts("WAIT for background shader compilation");
    const auto deadline=GetTickCount64()+900000;
    while(!DefaultEffectReadyForCapture(app)){
        Check(GetTickCount64()<deadline,"source compiler finishes within the test watchdog");
        Sleep(100);
    }
    std::puts("READY background shader compilation");
}
struct Host {
    std::unique_ptr<MMDApp> app=PlainApp();MMDApp* previous=g_Block;bool initialized=false;
    explicit Host(bool prepareEffect=true){
        g_Block=app.get();processor::Configuration config;config.vertices.mode=processor::Mode::Cpu;config.mmv.mode=processor::Mode::Cpu;
        std::wstring error;Check(processor::SaveRequested(config,error)&&processor::Initialize(error),"explicit CPU preference before production startup");
        app->EnglishUI()=1;
        Check(InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE),"initialize real hidden production host");initialized=true;
        HandleWindowSize(app.get());
        Check(app->Renderer()&&app->Renderer()->device,"real D3D9 device available");
        Check(skinning::GetPreference().mode==skinning::Mode::Cpu&&skinning::GetStatus().automaticCalibrations==0,
            "no automatic GPU vertex calibration");
        if(const auto module=GetModuleHandleW(L"d3d9.dll")){
            wchar_t path[32768]{},system[32768]{};
            Check(GetModuleFileNameW(module,path,32768)&&GetSystemDirectoryW(system,32768)&&
                fs::equivalent(fs::path(path),fs::path(system)/L"d3d9.dll"),"native system D3D9 only");
        }
        app->antialiasing.enabled=false;
        const auto model=ModelFixture();LoadModelFile(app.get(),model.c_str(),false);
        Check(app->SelectedModel()!=nullptr,"load self-generated original PMD model");HiddenWindows();
        if(prepareEffect)WaitForEffect(app.get());
    }
    ~Host(){if(initialized){device_lost_wait::Forget(app.get());ShutdownCleanup(app.get());}g_Block=previous;}
};
void Frame(MMDApp* app){
    app->DeltaTime()=1.f/30;app->state.messageSeen=1;FrameDriver(app);
    if(!exclusiveWindow)HiddenWindows();
}

using EffectCompiler=HRESULT(WINAPI*)(LPCVOID,UINT,const void*,const void*,DWORD,void**,void**);
EffectCompiler realCompiler=nullptr;
HANDLE compilerEntered=nullptr,allowCompilation=nullptr;
HRESULT WINAPI HoldCompiler(LPCVOID source,UINT size,const void* defines,const void* include,
    DWORD flags,void** compiler,void** errors){
    if(GetCurrentThreadId()==hostThread)return E_UNEXPECTED;
    SetEvent(compilerEntered);
    if(WaitForSingleObject(allowCompilation,15000)!=WAIT_OBJECT_0)return E_ABORT;
    return realCompiler(source,size,defines,include,flags,compiler,errors);
}
decltype(&RecordingReadbackPass) realAviReadback=nullptr;
unsigned shadedReadbacks=0;
bool ObserveShadedReadback(MMDApp* app){
    Check(app->Renderer()->effect&&app->SelfShadowMode()>0,
        "the exported frame uses the attached effect with self-shadow enabled");
    ++shadedReadbacks;
    return realAviReadback(app);
}

constexpr int kStereoEyeWidth=160, kStereoEyeHeight=120;
using StereoReset=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using StereoStretch=HRESULT(WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,
    IDirect3DSurface9*,const RECT*,D3DTEXTUREFILTERTYPE);
StereoReset realStereoReset=nullptr;
StereoStretch realStereoStretch=nullptr;
unsigned exclusiveRequests=0,stereoStages=0;
enum class StereoScenario { Off, Pack, RightCopyFailure, DpiRequest, NativeExclusive };
StereoScenario stereoScenario=StereoScenario::Off;
unsigned failedRightCopies=0;
bool exclusiveUsesDisplayPixels=false;

D3DPRESENT_PARAMETERS ActualPresentation(IDirect3DDevice9* device) {
    IDirect3DSwapChain9* chain=nullptr;
    Check(SUCCEEDED(device->GetSwapChain(0,&chain))&&chain,"read the real implicit swap chain");
    D3DPRESENT_PARAMETERS actual{};
    const auto hr=chain->GetPresentParameters(&actual);
    chain->Release();
    Check(SUCCEEDED(hr),"read the real swap-chain presentation");
    return actual;
}

HRESULT WINAPI ObserveStereoReset(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters){
    if(parameters->Windowed)return realStereoReset(device,parameters);
    ++exclusiveRequests;
    if(stereoScenario==StereoScenario::DpiRequest){
        D3DDISPLAYMODE display{};
        Check(SUCCEEDED(device->GetDisplayMode(0,&display)),"query the device's physical display mode");
        RECT client{};
        Check(GetClientRect(g_Block->MainWindow(),&client)!=FALSE,"read the virtualized fullscreen client");
        exclusiveUsesDisplayPixels=parameters->BackBufferWidth==display.Width&&parameters->BackBufferHeight==display.Height;
        std::printf("EXCLUSIVE request=%ux%u display=%ux%u client=%ldx%ld\n",
            parameters->BackBufferWidth,parameters->BackBufferHeight,display.Width,display.Height,client.right,client.bottom);
        Check(UINT(client.right)!=display.Width||UINT(client.bottom)!=display.Height,
            "the DPI fixture has different logical and physical dimensions");
    }
    if(stereoScenario==StereoScenario::NativeExclusive){
        D3DDISPLAYMODE display{};
        Check(SUCCEEDED(device->GetDisplayMode(0,&display)),"read native display mode before exclusive entry");
        std::printf("EXCLUSIVE native request=%ux%u format=%u samples=%u display=%ux%u refresh=%u\n",
            parameters->BackBufferWidth,parameters->BackBufferHeight,unsigned(parameters->BackBufferFormat),
            unsigned(parameters->MultiSampleType),display.Width,display.Height,display.RefreshRate);
        Check(GetForegroundWindow()==exclusiveWindow,"own the foreground at native exclusive reset");
        Check(parameters->BackBufferWidth==display.Width&&parameters->BackBufferHeight==display.Height,
            "exclusive fixture requests the current physical resolution");
        const auto hr=realStereoReset(device,parameters);
        std::printf("EXCLUSIVE native Reset=0x%08lx\n",static_cast<unsigned long>(hr));
        return hr;
    }
    auto windowed=*parameters;
    windowed.Windowed=TRUE;
    windowed.FullScreen_RefreshRateInHz=0;
    return realStereoReset(device,&windowed);
}
HRESULT WINAPI SupplyStereoEyes(IDirect3DDevice9* device,IDirect3DSurface9* src,const RECT* srcRect,
    IDirect3DSurface9* dst,const RECT* dstRect,D3DTEXTUREFILTERTYPE filter){
    D3DSURFACE_DESC from{},to{};
    const bool described=src&&dst&&SUCCEEDED(src->GetDesc(&from))&&SUCCEEDED(dst->GetDesc(&to));
    if(stereoScenario==StereoScenario::RightCopyFailure&&described&&to.Width==2*kStereoEyeWidth&&
        to.Height==kStereoEyeHeight&&dstRect&&dstRect->left==kStereoEyeWidth){
        ++failedRightCopies;
        return E_FAIL;
    }
    const auto result=realStereoStretch(device,src,srcRect,dst,dstRect,filter);
    if(SUCCEEDED(result)&&described&&
        to.Width==2*from.Width&&to.Height==from.Height&&from.Width>=kStereoEyeWidth&&from.Height>=kStereoEyeHeight){
        const RECT left{0,0,LONG(from.Width),LONG(from.Height)};
        const RECT right{left.right,0,LONG(to.Width),left.bottom};
        Check(SUCCEEDED(device->ColorFill(dst,&left,0xffff0000))&&
            SUCCEEDED(device->ColorFill(dst,&right,0xff0000ff)),"supply distinct synthetic eyes in the real staging surface");
        ++stereoStages;
    }
    return result;
}

void CheckFinishedAvi(const fs::path& path,bool stereo,unsigned expectedFrames) {
    std::ifstream file(path,std::ios::binary);
    Check(static_cast<bool>(file),"open the AVI output");
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file),{}};
    if(expectedFrames==0&&bytes.empty())return; // The mux may leave a zero-byte output.
    const auto tag=[&](size_t offset,const char* value){
        return offset<=bytes.size()&&bytes.size()-offset>=4&&!std::memcmp(bytes.data()+offset,value,4);
    };
    const auto number=[&](size_t offset){
        Check(offset<=bytes.size()&&bytes.size()-offset>=4,"AVI field fits inside the completed file");
        const auto* value=bytes.data()+offset;
        return std::uint32_t(value[0])|(std::uint32_t(value[1])<<8)|
            (std::uint32_t(value[2])<<16)|(std::uint32_t(value[3])<<24);
    };
    Check(tag(0,"RIFF")&&tag(8,"AVI "),"retry replaces the sentinel with a real AVI file");
    const size_t end=8ull+number(4);
    Check(end>=12&&end<=bytes.size(),"AVI RIFF size was finalized");
    unsigned frames=0,videoChunks=0;
    for(size_t chunk=12;end-chunk>=8;){
        const size_t payload=chunk+8,size=number(chunk+4);
        Check(size+(size&1)<=end-payload,"AVI chunk fits inside RIFF");
        if(tag(chunk,"LIST")&&size>=4&&tag(payload,"hdrl")){
            Check(size>=36&&tag(payload+4,"avih")&&number(payload+8)>=24,"AVI has a main video header");
            frames=number(payload+28);
            if(stereo)Check(size>=52&&number(payload+44)==2*kStereoEyeWidth&&number(payload+48)==kStereoEyeHeight,
                "AVI main header contains the two-eye dimensions");
        }
        if(tag(chunk,"LIST")&&size>=4&&tag(payload,"movi")){
            const size_t listEnd=payload+size;
            for(size_t video=payload+4;listEnd-video>=8;){
                const size_t length=number(video+4);
                Check(length+(length&1)<=listEnd-video-8,"video payload fits inside movi");
                if(length&&bytes[video+2]=='d'&&(bytes[video+3]=='b'||bytes[video+3]=='c')){
                    ++videoChunks;
                    if(stereo){
                        Check(length==2*kStereoEyeWidth*kStereoEyeHeight*4,"AVI holds a complete RGB32 stereo sample");
                        const size_t row=video+8+2*kStereoEyeWidth*4*(kStereoEyeHeight/2);
                        const auto rgb=[&](int x){return number(row+4*x)&0xffffff;};
                        std::printf("STEREO frame=%u eyes=%06x/%06x\n",videoChunks,rgb(kStereoEyeWidth-1),rgb(kStereoEyeWidth));
                        Check(rgb(0)==0xff0000&&rgb(kStereoEyeWidth-1)==0xff0000&&
                            rgb(kStereoEyeWidth)==0x0000ff&&rgb(2*kStereoEyeWidth-1)==0x0000ff,
                            "encoded sample contains both eyes with the seam at one eye width");
                    }
                }
                video+=8+length+(length&1);
            }
        }
        chunk=payload+size+(size&1);
    }
    std::printf("AVI bytes=%zu declared_frames=%u video_chunks=%u\n",bytes.size(),frames,videoChunks);
    Check(frames==expectedFrames&&videoChunks==frames,"AVI contains exactly the expected complete frames");
}

constexpr std::array<D3DCOLOR,3> kCompressedFrameColors{0xffee2244,0xff22dd44,0xff2233ee};
StereoStretch realCodecStretch=nullptr;
unsigned codecCaptures=0;
HRESULT WINAPI SupplyCodecFrame(IDirect3DDevice9* device,IDirect3DSurface9* src,const RECT* srcRect,
    IDirect3DSurface9* dst,const RECT* dstRect,D3DTEXTUREFILTERTYPE filter){
    const auto result=realCodecStretch(device,src,srcRect,dst,dstRect,filter);
    if(SUCCEEDED(result)&&g_Block&&dst&&dst==g_Block->CaptureRenderTarget()&&g_Block->RecordingWindow()){
        Check(codecCaptures<kCompressedFrameColors.size(),"one capture for each requested sample");
        Check(SUCCEEDED(device->ColorFill(dst,nullptr,kCompressedFrameColors[codecCaptures])),
            "write a distinct test color to the real recording capture surface");
        const RECT bottom{0,kStereoEyeHeight/2,kStereoEyeWidth,kStereoEyeHeight};
        Check(SUCCEEDED(device->ColorFill(dst,&bottom,0xff335577)),"mark the bottom half of the captured image");
        ++codecCaptures;
    }
    return result;
}

LRESULT FindUtVideoRgb(HWND combo) {
    const auto count=SendMessageW(combo,CB_GETCOUNT,0,0);
    for(LRESULT row=0;row<count;++row){
        const auto length=SendMessageW(combo,CB_GETLBTEXTLEN,row,0);
        if(length<=0)continue;
        std::wstring label(static_cast<size_t>(length)+1,L'\0');
        SendMessageW(combo,CB_GETLBTEXT,row,reinterpret_cast<LPARAM>(label.data()));
        const auto selection=SendMessageW(combo,CB_GETITEMDATA,row,0);
        if(selection>=0&&wcsstr(label.c_str(),L"UtVideo RGB")&&!wcsstr(label.c_str(),L"RGBA")){
            std::printf("CODEC %ls index=%lld\n",label.c_str(),static_cast<long long>(selection));
            return row;
        }
    }
    Check(false,"production codec enumeration includes installed UtVideo RGB");
    return CB_ERR;
}

void SelectUtVideoRgb(MMDApp* app) {
    const auto instance=GetModuleHandleW(nullptr);
    const auto combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,200,100,
        app->MainWindow(),nullptr,instance,nullptr);
    const auto button=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD,0,0,80,25,app->MainWindow(),nullptr,instance,nullptr);
    Check(combo&&button,"create owned codec selector controls");
    FillCodecCombo(app->Recorder(),combo,1);
    const auto codec=static_cast<int>(SendMessageW(combo,CB_GETITEMDATA,FindUtVideoRgb(combo),0));
    RebindCodecOnComboChange(app->Recorder(),codec,button,1);
    Check(app->Recorder()->compressor&&app->Recorder()->selectedCodec==codec,
        "production codec selection binds the real compressor");
    DestroyWindow(button);
    DestroyWindow(combo);
}

void CheckCompressedAvi(const fs::path& path) {
    AVIFileInit();
    PAVIFILE file=nullptr;
    Check(AVIFileOpenW(&file,path.c_str(),OF_READ|OF_SHARE_DENY_WRITE,nullptr)==AVIERR_OK,"reopen the encoded AVI");
    PAVISTREAM stream=nullptr;
    Check(AVIFileGetStream(file,&stream,streamtypeVIDEO,0)==AVIERR_OK,"find the real AVI video stream");
    AVISTREAMINFOW info{};
    Check(AVIStreamInfoW(stream,&info,sizeof info)==AVIERR_OK,"read compressed stream metadata");
    std::printf("AVI compressor=%c%c%c%c frames=%lu rate=%lu/%lu\n",
        char(info.fccHandler),char(info.fccHandler>>8),char(info.fccHandler>>16),char(info.fccHandler>>24),
        info.dwLength,info.dwRate,info.dwScale);
    // DirectShow carries the frame period in 100 ns ticks, so 30 FPS is
    // 10^7/30 = 333333.33 ticks: the graph stores 10000000/333333 rather than
    // the exact 30/1. Both mean 333333 whole ticks, so compare the period in
    // ticks within one rounding tick; 29 or 31 FPS are thousands of ticks away.
    const long long nominalTick=10000000LL/30;
    const long long frameTick=(info.dwScale!=0&&info.dwRate!=0)
        ?static_cast<long long>(info.dwScale)*10000000LL/static_cast<long long>(info.dwRate)
        :-1LL;
    Check(info.fccHandler==mmioFOURCC('U','L','R','G')&&info.dwLength==3&&
        info.dwScale!=0&&info.dwRate!=0&&
        frameTick>=nominalTick-1&&frameTick<=nominalTick+1,
        "AVI records UtVideo RGB, three frames and 30 FPS");
    const auto first=AVIStreamStart(stream);
    PGETFRAME decoder=AVIStreamGetFrameOpen(stream,nullptr);
    Check(decoder!=nullptr&&first>=0,"open a real VfW decoder for the encoded video");
    for(size_t i=0;i<kCompressedFrameColors.size();++i){
        const auto* dib=static_cast<const BITMAPINFOHEADER*>(AVIStreamGetFrame(decoder,first+static_cast<LONG>(i)));
        Check(dib&&dib->biSize>=sizeof(*dib)&&dib->biWidth==kStereoEyeWidth&&
            (dib->biHeight==kStereoEyeHeight||dib->biHeight==-kStereoEyeHeight)&&
            (dib->biBitCount==24||dib->biBitCount==32)&&dib->biCompression==BI_RGB,
            "decode each compressed frame to the requested RGB dimensions");
        const auto* bits=reinterpret_cast<const unsigned char*>(dib)+dib->biSize;
        const unsigned stride=((kStereoEyeWidth*dib->biBitCount+31)/32)*4;
        const auto pixel=[&](int y){
            const unsigned row=dib->biHeight>0?kStereoEyeHeight-1-y:y;
            const auto* p=bits+row*stride+(kStereoEyeWidth/2)*(dib->biBitCount/8);
            return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16);
        };
        std::printf("DECODE frame=%zu top=%06x bottom=%06x\n",i,pixel(20),pixel(100));
        Check(pixel(20)==(kCompressedFrameColors[i]&0xffffff)&&pixel(100)==0x335577,
            "decoded frames preserve the distinct colors, order and orientation");
    }
    AVIStreamGetFrameClose(decoder);
    AVIStreamRelease(stream);
    AVIFileRelease(file);
    AVIFileExit();
}

void OutputRecording(bool lockFirst,bool waitForPendingEffect,StereoScenario scenario,bool compressed=false){
    stereoScenario=scenario;
    const bool stereo=scenario!=StereoScenario::Off;
    const bool failRightEye=scenario==StereoScenario::RightCopyFailure;
    const bool nativeExclusive=scenario==StereoScenario::NativeExclusive;
    Hooks hooks;
    Host host(!waitForPendingEffect);
    auto* app=host.app.get();
    const auto main=app->MainWindow();
    if(nativeExclusive){
        exclusiveWindow=main;
        SetWindowLongPtrW(main,GWL_EXSTYLE,GetWindowLongPtrW(main,GWL_EXSTYLE)&~WS_EX_NOACTIVATE);
        ShowWindow(main,SW_SHOWNORMAL);
        SetForegroundWindow(main);
        Check(IsWindowVisible(main)&&GetForegroundWindow()==main,
            "native exclusive test could not activate its isolated window");
    }
    app->EnglishUI()=1; // Startup has already loaded the installation language.
    app->CurrentFrame()=waitForPendingEffect?5:0;
    if(waitForPendingEffect){
        Check(app->state.selfShadowEnabled&&app->ShadowKeys(),"self-shadow enabled with an editable track");
        app->SelfShadowMode()=0;
        app->ShadowKeys()[0].mode=1;
    }
    app->CameraMode()=0;
    app->state.followCameraEnabled=0;
    Write(*app,Seed(0,0,true));
    RefreshMainWindowViewport(app);
    const auto view=Read(*app);
    const int width=app->RenderWidth(),height=app->RenderHeight();
    const auto revision=app->SceneRevision(),generation=app->SceneGeneration();
    const auto dirty=app->SceneModified();
    auto* recorder=app->Recorder();
    Check(recorder&&!recorder->graph,"recording graph is initially idle");
    recorder->selectedCodec=-1;
    app->AviOutputWidth()=kStereoEyeWidth;app->AviOutputHeight()=kStereoEyeHeight;
    app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=2;app->AviRecordFps()=30;
    app->AviIncludeWave()=0;app->AviCloseAfterExport()=0;app->AviStereoOutput()=0;
    lockedOutputPath=output/L"locked-retry.avi";
    Check(wcscpy_s(app->AviOutputPath(),256,lockedOutputPath.c_str())==0,"test path fits recording settings");
    const std::vector<unsigned char> sentinel{'e','x','i','s','t','i','n','g'};
    OwnBytes(lockedOutputPath,sentinel);
    const auto idle=[&]{
        return !app->FullscreenMode()&&!app->RecordingWindow()&&!app->AviRenderResolutionOverridden()&&
            !app->RecordingStartGeneration()&&!app->FrameStepPlayback()&&!app->state.recordPlaybackStartPending&&
            !app->CaptureRenderTarget()&&!app->CaptureSystemSurface()&&!app->CaptureReadbackPixels()&&
            !recorder->graph&&app->RenderWidth()==width&&app->RenderHeight()==height&&
            app->SceneRevision()==revision&&app->SceneGeneration()==generation&&app->SceneModified()==dirty&&
            Same(Read(*app),view);
    };
    if(waitForPendingEffect){
        const auto d3dx=GetModuleHandleW(L"d3dx9_43.dll");
        const auto compiler=d3dx?GetProcAddress(d3dx,"D3DXCreateEffectCompiler"):nullptr;
        Check(compiler&&!app->Renderer()->effect,"start with a deferred source effect");
        compilerEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        allowCompilation=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        Check(compilerEntered&&allowCompilation,"create process-local compiler gate");
        hooks.Add(reinterpret_cast<EffectCompiler>(compiler),&HoldCompiler,realCompiler);
        pendingEffectOwner=app->MainWindow();
        StartAviRecordWindow(app);
        Check(pendingEffectWarnings==1&&idle()&&app->CurrentFrame()==5&&app->SelfShadowMode()==0,
            "a shadowed export range is deferred even when the editor frame has mode zero");
        Check(WaitForSingleObject(compilerEntered,3000)==WAIT_OBJECT_0,"export scheduled the background compiler");
        const auto newPath=output/L"not-created.avi";
        Check(wcscpy_s(app->AviOutputPath(),256,newPath.c_str())==0,"set a fresh output path");
        app->AviStereoOutput()=1;
        StartAviRecordFullscreen(app);
        Check(pendingEffectWarnings==2&&idle()&&!fs::exists(newPath),
            "fullscreen also defers before creating a new output or changing display state");
        app->AviStereoOutput()=0;
        Check(wcscpy_s(app->AviOutputPath(),256,lockedOutputPath.c_str())==0,"retry the original path");
        {
            std::ifstream previous(lockedOutputPath,std::ios::binary);
            const std::vector<unsigned char> existing{std::istreambuf_iterator<char>(previous),{}};
            Check(existing==sentinel,"shader preparation leaves the previous output byte-identical");
        }
        HiddenWindows();
        Check(SetEvent(allowCompilation),"release the real compiler");
        pendingEffectOwner=nullptr;
        WaitForEffect(app);
        Check(CloseHandle(allowCompilation)&&CloseHandle(compilerEntered),"close finished compiler gate handles");
        hooks.Add(&RecordingReadbackPass,&ObserveShadedReadback,realAviReadback);
    }
    if(lockFirst||scenario==StereoScenario::DpiRequest){
        const HANDLE lock=CreateFileW(lockedOutputPath.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(lock!=INVALID_HANDLE_VALUE,"hold a real exclusive handle to the existing output");
        lockedOutputOwner=app->MainWindow();
        std::puts("START locked windowed request");
        StartAviRecordWindow(app);
        Check(lockedOutputWarnings==1&&idle(),"windowed path rejects sharing denial and leaves the editor ready");
        app->AviStereoOutput()=1;
        if(scenario==StereoScenario::DpiRequest){
            D3DDISPLAYMODE display{};
            Check(SUCCEEDED(app->Renderer()->device->GetDisplayMode(0,&display)),
                "query the physical display for the output-size challenge");
            const int logicalWidth=GetSystemMetrics(SM_CXSCREEN);
            Check(logicalWidth>0&&static_cast<UINT>(logicalWidth)<display.Width,
                "the physical display admits an output wider than the virtual screen");
            app->AviOutputWidth()=logicalWidth+1;
            std::printf("EXCLUSIVE output_size_check=%d logical_width=%d physical_width=%u\n",
                app->AviOutputWidth(),logicalWidth,display.Width);
        }
        std::puts("START locked fullscreen request");
        StartAviRecordFullscreen(app);
        Check(lockedOutputWarnings==2&&idle(),"fullscreen path rejects sharing denial before entering fullscreen");
        app->AviOutputWidth()=kStereoEyeWidth;
        lockedOutputOwner=nullptr;
        std::vector<unsigned char> existing(sentinel.size()+1);
        DWORD read=0;
        Check(ReadFile(lock,existing.data(),static_cast<DWORD>(existing.size()),&read,nullptr)&&
            read==sentinel.size()&&!std::memcmp(existing.data(),sentinel.data(),sentinel.size()),
            "both refusals preserve every byte of the existing file");
        Check(CloseHandle(lock)!=FALSE,"release the exclusive handle before retry");
        app->AviStereoOutput()=0;
    }
    const auto mainStyle=GetWindowLongPtrW(main,GWL_STYLE);
    const auto mainMenu=GetMenu(main);
    RECT editorRect{};
    D3DPRESENT_PARAMETERS editorPresentation{};
    if(nativeExclusive){
        Check(GetWindowRect(main,&editorRect)!=FALSE,"capture the editor window placement");
        editorPresentation=ActualPresentation(app->Renderer()->device);
        Check(editorPresentation.Windowed,"the editor starts with a native windowed swap chain");
    }
    if(compressed){
        recorder->selectedCodec=(std::numeric_limits<int>::max)();
        expectingMissingCompressor=true;
        std::puts("START with an unavailable compressor");
        StartAviRecordWindow(app);
        expectingMissingCompressor=false;
        Check(missingCompressorWarnings==1&&idle(),"an unavailable compressor does not start raw AVI");
        {
            std::ifstream previous(lockedOutputPath,std::ios::binary);
            const std::vector<unsigned char> existing{std::istreambuf_iterator<char>(previous),{}};
            Check(existing==sentinel,"unavailable compressor preserves the previous destination");
        }
        SelectUtVideoRgb(app);
        auto** table=*reinterpret_cast<void***>(app->Renderer()->device);
        hooks.Add(reinterpret_cast<StereoStretch>(table[34]),&SupplyCodecFrame,realCodecStretch);
    }
    if(stereo){
        auto** table=*reinterpret_cast<void***>(app->Renderer()->device);
        hooks.Add(reinterpret_cast<StereoReset>(table[16]),&ObserveStereoReset,realStereoReset);
        hooks.Add(reinterpret_cast<StereoStretch>(table[34]),&SupplyStereoEyes,realStereoStretch);
        app->AviStereoOutput()=1;
        app->AviStereoWidthMultiplier()=2;
        std::puts(nativeExclusive?"START stereo recording with native exclusive presentation":
            "START stereo recording with a hidden windowed device");
        StartAviRecordFullscreen(app);
        Check(exclusiveRequests==1&&app->FullscreenMode()&&app->RecordingWindow()==main,
            "production recording enters its fullscreen path and requests one exclusive reset");
        if(scenario==StereoScenario::DpiRequest)
            Check(exclusiveUsesDisplayPixels,"exclusive reset requests the physical adapter display dimensions");
        if(nativeExclusive){
            const auto actual=ActualPresentation(app->Renderer()->device);
            Check(!actual.Windowed,"native swap chain entered exclusive presentation");
            std::printf("EXCLUSIVE actual=%ux%u Windowed=%d\n",
                actual.BackBufferWidth,actual.BackBufferHeight,actual.Windowed);
        }
    }else{
        std::puts(lockFirst?"START windowed retry after releasing the file":"START unlocked windowed control");
        StartAviRecordWindow(app);
    }
    Check(app->RecordingWindow()&&recorder->graph&&app->RecordingPlaybackEntered(),
        "retry on the same path starts actual AVI recording");
    if(compressed)Check(recorder->compressor&&recorder->compressorOutput,"recording graph includes the bound compressor");
    std::puts("START driving actual video frames");
    for(unsigned i=0;i<120&&app->RecordingWindow();++i){
        if(nativeExclusive&&GetForegroundWindow()!=main){
            FinishAviRecord(app);
            Check(false,"native exclusive test lost the foreground during export");
        }
        std::printf("FRAME %u before recorded=%d\n",i,app->state.recordedFrameCount);
        Frame(app);
        std::printf("FRAME %u after recorded=%d\n",i,app->state.recordedFrameCount);
    }
    if(!idle()){
        std::fprintf(stderr,"FINISH fullscreen=%u window=%d override=%d entered=%d step=%u pending=%u graph=%d "
            "size=%dx%d/%dx%d revision=%d generation=%d dirty=%d view=%d\n",
            unsigned(app->FullscreenMode()),app->RecordingWindow()!=nullptr,app->AviRenderResolutionOverridden(),
            app->RecordingPlaybackEntered(),unsigned(app->FrameStepPlayback()),unsigned(app->state.recordPlaybackStartPending),
            recorder->graph!=nullptr,app->RenderWidth(),app->RenderHeight(),width,height,
            app->SceneRevision()==revision,app->SceneGeneration()==generation,app->SceneModified()==dirty,Same(Read(*app),view));
    }
    Check(idle(),"natural AVI completion restores the editing state");
    if(failRightEye)Check(failedRightCopies==2&&app->state.recordedFrameCount==0,
        "right-eye copy failure and its unfiltered retry abort before advancing the recording");
    if(nativeExclusive){
        const auto actual=ActualPresentation(app->Renderer()->device);
        RECT restoredRect{};
        Check(GetWindowRect(main,&restoredRect)!=FALSE,"read the restored editor window bounds");
        std::printf("EXCLUSIVE finish stages=%u style=%08lx/%08lx menu=%d actual=%ux%u Windowed=%d editor=%ux%u rect=%ld,%ld,%ld,%ld/%ld,%ld,%ld,%ld\n",
            stereoStages,static_cast<unsigned long>(GetWindowLongPtrW(main,GWL_STYLE)),
            static_cast<unsigned long>(mainStyle),GetMenu(main)==mainMenu,actual.BackBufferWidth,
            actual.BackBufferHeight,actual.Windowed,editorPresentation.BackBufferWidth,
            editorPresentation.BackBufferHeight,restoredRect.left,restoredRect.top,restoredRect.right,
            restoredRect.bottom,editorRect.left,editorRect.top,editorRect.right,editorRect.bottom);
        Check(EqualRect(&editorRect,&restoredRect),"exclusive recording restores the editor window bounds");
        Check(actual.Windowed&&actual.BackBufferWidth==editorPresentation.BackBufferWidth&&
            actual.BackBufferHeight==editorPresentation.BackBufferHeight,
            "exclusive recording restores the actual windowed back buffer");
        std::printf("EXCLUSIVE restored=%ux%u Windowed=%d\n",
            actual.BackBufferWidth,actual.BackBufferHeight,actual.Windowed);
    }
    if(stereo){
        Check(stereoStages==(failRightEye?1u:3u),"stereo recording stages the expected eye pairs");
        Check(IsWindow(main)&&GetMenu(main)==mainMenu,"stereo recording restores the editor menu");
        Check(GetWindowLongPtrW(main,GWL_STYLE)==mainStyle,"stereo recording restores the editor window style");
        Check(app->Renderer()->presentParameters.Windowed,"stereo recording restores windowed presentation parameters");
    }
    CheckFinishedAvi(lockedOutputPath,stereo,failRightEye?0:3);
    if(compressed){
        Check(codecCaptures==3,"send all three real captures through the encoder");
        CheckCompressedAvi(lockedOutputPath);
    }
    if(waitForPendingEffect)Check(shadedReadbacks==3,"all three frames use the completed self-shadow effect");
    if(!nativeExclusive)HiddenWindows();
}

void PrepareSequence(MMDApp* app,const std::wstring& prefix,int end){
    seq::Options options;options.directory=output.wstring();options.prefix=prefix;options.format=0;
    options.width=160;options.height=120;
    options.start=0;options.end=end;options.fps=30;
    std::wstring error;Check(seq::Prepare(app,options,error),"prepare actual BMP image sequence without a codec driver");
    app->AviOutputWidth()=options.width;app->AviOutputHeight()=options.height;
    app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=end;app->AviRecordFps()=30;
    app->AviIncludeWave()=0;app->AviStereoOutput()=0;app->AviCloseAfterExport()=false;
}
void StartSequence(MMDApp* app,const std::wstring& prefix,int end){
    PrepareSequence(app,prefix,end);
    StartAviRecordWindow(app);Check(app->RecordingWindow()&&app->state.recordPlaybackStartPending,"real recording start arms the lifecycle");
}
unsigned CountImages(const std::wstring& prefix){
    unsigned count=0;for(const auto& p:fs::directory_iterator(output))
        if(p.path().filename().wstring().find(prefix)==0&&p.path().extension()==L".bmp"){
            Check(fs::file_size(p.path())>54,"image is more than an empty bitmap header");++count;
        }return count;
}

using CreateTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using BindTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
CreateTarget realCreateTarget = nullptr;
BindTarget realBindTarget = nullptr;
struct CaptureTargetFault {
    IDirect3DDevice9* device;
    IDirect3DSurface9* target = nullptr;
    bool failAllocation = false;
    unsigned failures = 0;
    UINT width = 160;
    UINT height = 120;
};
CaptureTargetFault* captureFault = nullptr;
decltype(&ChooseStillImagePath) realChooseStillPath = nullptr;
fs::path captureStillPath;

HRESULT CaptureFixtureStillPath(MMDApp* app, HWND owner, wchar_t* destination, std::size_t capacity,
                               std::optional<seq::StillFileIdentity>& overwrite) {
    if (captureStillPath.empty() || owner != captureTargetErrorOwner || GetCurrentThreadId() != hostThread)
        return realChooseStillPath(app, owner, destination, capacity, overwrite);
    overwrite = seq::ReadStillFileIdentity(captureStillPath.c_str());
    Check(wcscpy_s(destination, capacity, captureStillPath.c_str()) == 0, "accept the isolated still destination");
    return S_OK;
}

HRESULT WINAPI ObserveCreateTarget(IDirect3DDevice9* device, UINT width, UINT height,
    D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable,
    IDirect3DSurface9** target, HANDLE* shared) {
    auto* fault = captureFault;
    const bool selected = fault && device == fault->device && width == fault->width &&
        height == fault->height && samples == D3DMULTISAMPLE_NONE && lockable;
    if (selected && fault->failAllocation) {
        ++fault->failures;
        *target = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    const auto result = realCreateTarget(device, width, height, format, samples, quality,
        lockable, target, shared);
    if (selected && SUCCEEDED(result))
        fault->target = *target;
    return result;
}

HRESULT WINAPI ObserveBindTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* target) {
    auto* fault = captureFault;
    if (fault && device == fault->device && index == 0 && target && target == fault->target) {
        ++fault->failures;
        return D3DERR_INVALIDCALL;
    }
    return realBindTarget(device, index, target);
}

void CaptureTargetFailures() {
    Hooks hooks;
    Host host;
    auto* app = host.app.get();
    app->EnglishUI() = 1;
    auto* renderer = app->Renderer();
    renderer->presentParameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    renderer->presentParameters.MultiSampleQuality = 0;
    renderer->multisampleAvailable = 0;
    Check(PostDeviceReset(app), "reset to a real no-MSAA device and matching depth buffer");
    const auto vtable = *reinterpret_cast<void***>(renderer->device);
    hooks.Add(reinterpret_cast<CreateTarget>(vtable[28]), &ObserveCreateTarget, realCreateTarget);
    hooks.Add(reinterpret_cast<BindTarget>(vtable[37]), &ObserveBindTarget, realBindTarget);
    const int previewWidth = renderer->screenWidth, previewHeight = renderer->screenHeight;
    const int editorWidth = app->RenderWidth(), editorHeight = app->RenderHeight();
    const auto editorView = Read(*app);
    const auto scene = app->SceneRevision();
    const auto dirty = app->SceneModified();
    captureTargetErrorOwner = app->MainWindow();

    enum class Scenario { NewBind, Allocation, CachedBind, ResetBind };
    for (const auto scenario : {Scenario::NewBind, Scenario::Allocation, Scenario::CachedBind, Scenario::ResetBind}) {
        const auto prefix = L"capture_" + std::to_wstring(static_cast<int>(scenario)) + L"_";
        CaptureTargetFault fault{renderer->device};
        fault.failAllocation = scenario == Scenario::Allocation;
        PrepareSequence(app, prefix, 2);
        if (scenario == Scenario::CachedBind) {
            // Keep the pre-existing target through startup, as happens when the output
            // and preview sizes already match.
            renderer->screenWidth = renderer->presentParameters.BackBufferWidth = 160;
            renderer->screenHeight = renderer->presentParameters.BackBufferHeight = 120;
            Check(PostDeviceReset(app) && !renderer->captureSurface, "prepare an already matching output size");
            Check(SUCCEEDED(renderer->device->CreateRenderTarget(160, 120, renderer->backbufferFormat,
                D3DMULTISAMPLE_NONE, 0, TRUE, &renderer->captureSurface, nullptr)),
                "seed a valid cached target without binding it");
            fault.target = renderer->captureSurface;
        }
        if (scenario == Scenario::ResetBind) {
            StartAviRecordWindow(app);
            Check(app->RecordingWindow() && PostDeviceReset(app) && !renderer->captureSurface,
                "force capture target recreation after recording has started");
        }
        const auto errors = captureTargetErrors;
        captureFault = &fault;
        if (scenario == Scenario::ResetBind)
            Frame(app);
        else
            StartAviRecordWindow(app);
        captureFault = nullptr;
        std::printf("CAPTURE scenario=%d injected=%u notices=%u active=%d no-msaa=%d\n",
            static_cast<int>(scenario), fault.failures, captureTargetErrors - errors,
            int(app->RecordingWindow() != nullptr), int(renderer->multisampleAvailable == 0));
        Check(fault.failures == 1, "exactly one allocation or binding call reached the fault");
        Check(captureTargetErrors == errors + 1 && !app->RecordingWindow() &&
            !seq::Active(app) && !app->FrameStepPlayback() && !app->AviRenderResolutionOverridden(),
            "render-target failure cancels export with one notice and clears recording state");
        Check(!CountImages(prefix) && renderer->multisampleAvailable == 0,
            "failed export leaves no output and preserves the no-MSAA retry path");
        Check(app->RenderWidth() == editorWidth && app->RenderHeight() == editorHeight &&
            Same(Read(*app), editorView) && app->SceneRevision() == scene && app->SceneModified() == dirty,
            "failed export restores editor resolution and view without editing the project");
        if (scenario != Scenario::CachedBind)
            Check(renderer->screenWidth == previewWidth && renderer->screenHeight == previewHeight,
                "failed export restores preview backbuffer dimensions");

        StartSequence(app, prefix, 2);
        IDirect3DSurface9* bound = nullptr;
        Check(SUCCEEDED(renderer->device->GetRenderTarget(0, &bound)) && bound == renderer->captureSurface,
            "retry binds the actual no-MSAA capture surface");
        bound->Release();
        for (unsigned step = 0; seq::Active(app) && step < 30; ++step) Frame(app);
        Check(!seq::Active(app) && !app->RecordingWindow() && CountImages(prefix) == 3,
            "retry produces the full three-image sequence at the same destination");
        if (scenario == Scenario::CachedBind) {
            renderer->screenWidth = renderer->presentParameters.BackBufferWidth = previewWidth;
            renderer->screenHeight = renderer->presentParameters.BackBufferHeight = previewHeight;
            Check(PostDeviceReset(app), "restore original test backbuffer after matching-size case");
        }
    }
    hooks.Add(&ChooseStillImagePath, &CaptureFixtureStillPath, realChooseStillPath);
    captureStillPath = output / L"still-retry.bmp";
    const std::vector<unsigned char> original{'p','r','e','v','i','o','u','s'};
    OwnBytes(captureStillPath, original);
    CaptureTargetFault stillFault{renderer->device};
    const bool resizeStill = editorWidth > previewWidth || editorHeight > previewHeight;
    stillFault.width = resizeStill ? editorWidth : previewWidth;
    stillFault.height = resizeStill ? editorHeight : previewHeight;
    const auto notices = captureTargetErrors;
    captureFault = &stillFault;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    captureFault = nullptr;
    std::printf("CAPTURE still injected=%u notices=%u active=%d scheduled=%d size=%ux%u\n",
        stillFault.failures, captureTargetErrors - notices, int(seq::StillActive(app)),
        int(app->state.timelineAdvanceDue || app->state.timelineAdvanceRequested), stillFault.width, stillFault.height);
    Check(stillFault.failures == 1 && captureTargetErrors == notices + 1 &&
        !app->RecordingWindow() && !seq::StillActive(app) &&
        !app->state.timelineAdvanceDue && !app->state.timelineAdvanceRequested,
        "still target binding failure cancels with one notice and clears the scheduled capture");
    {
        std::ifstream file(captureStillPath, std::ios::binary);
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
        Check(bytes == original, "still target binding failure preserves the existing destination");
    }
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(app->RecordingWindow() && seq::StillActive(app), "retry still export through the same menu command");
    for (unsigned step = 0; seq::StillActive(app) && step < 12; ++step) Frame(app);
    std::ifstream image(captureStillPath, std::ios::binary);
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER info{};
    image.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    image.read(reinterpret_cast<char*>(&info), sizeof(info));
    Check(!seq::StillActive(app) && !app->RecordingWindow() && image &&
        fileHeader.bfType == 0x4d42 && info.biWidth == editorWidth &&
        std::abs(info.biHeight) == editorHeight && fs::file_size(captureStillPath) > fileHeader.bfOffBits,
        "same-path still retry replaces the sentinel with an image of the requested dimensions");
    std::puts("CAPTURE still failed bind preserved destination and retry replaced it with a BMP");

    captureStillPath = output / L"still-alpha.png";
    const auto alphaNotices = captureTargetErrors;
    CaptureTargetFault alphaFault{renderer->device};
    alphaFault.width = stillFault.width;
    alphaFault.height = stillFault.height;
    alphaFault.failAllocation = true;
    captureFault = &alphaFault;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(app->RecordingWindow() && seq::StillActive(app), "transparent still begins on its own target path");
    for (unsigned step = 0; seq::StillActive(app) && step < 12; ++step) Frame(app);
    captureFault = nullptr;
    std::ifstream png(captureStillPath, std::ios::binary);
    std::array<unsigned char, 8> signature{};
    png.read(reinterpret_cast<char*>(signature.data()), signature.size());
    constexpr std::array<unsigned char, 8> expectedPng{137, 'P', 'N', 'G', 13, 10, 26, 10};
    Check(!alphaFault.failures && captureTargetErrors == alphaNotices &&
        !seq::StillActive(app) && !app->RecordingWindow() && png && signature == expectedPng,
        "transparent still writes a PNG without requesting the legacy lockable target");
    captureStillPath.clear();
    captureTargetErrorOwner = nullptr;
}
void CameraKeys(MMDApp& app){
    auto* k=app.CameraKeys();Check(k!=nullptr,"real camera key pool initialized");
    k[0].next=1;k[0].eye[0]=2;k[0].eye[1]=10;k[0].eye[2]=0;k[0].distance=-45;k[0].fov=30;k[0].parentModel=-1;
    std::memset(k[0].target,0,sizeof k[0].target);k[1]=k[0];k[1].frame=30;k[1].previous=0;k[1].next=0;k[1].eye[0]=12;
    for(int c=0;c<6;++c){k[1].interpolation[0][c]=k[1].interpolation[1][c]=20;k[1].interpolation[2][c]=k[1].interpolation[3][c]=107;}
}
using Graph=decltype(&BuildRecordingGraph);
Graph realGraph=nullptr;MMDApp* graphApp=nullptr;unsigned graphFailures=0;bool nestedStart=false;
bool finishInGraph=false,sceneInGraph=false,finishedGraphResult=false;
View graphCapturedView{},graphSubsequentView{};
std::uint64_t graphFinishedGeneration=0;
constexpr int newSceneWidth=333,newSceneHeight=222;
bool FailGraph(DShowRecorder* recorder,HWND window,unsigned char english,void* record,void* config,float fps,
    std::uint32_t one,std::uint32_t bitrate,std::uint64_t first,std::uint64_t count,const WaveAudioContext* audio,
    unsigned char language,const recording_output_path::Selection* approved){
    if(GetCurrentThreadId()==hostThread&&graphApp&&graphApp->Recorder()==recorder){
        ++graphFailures;
        if(sceneInGraph){
            Check(graphApp->RecordingWindow()&&graphApp->AviRenderResolutionOverridden(),
                "scene-change graph callback starts with owned recording resources and resolution override");
            graphApp->AdvanceSceneGeneration();Write(*graphApp,graphSubsequentView);graphApp->CurrentFrame()=17;
            graphApp->RenderWidth()=newSceneWidth;graphApp->RenderHeight()=newSceneHeight;
            return finishedGraphResult;
        }
        if(finishInGraph){
            Check(graphApp->RecordingWindow()!=nullptr,"graph callback entered a real live recording window");
            const auto generation=graphApp->RecordingStartGeneration();
            const HWND recordingWindow=graphApp->RecordingWindow();
            Check(recording_state::Guard::Active(graphApp,recording_state::Operation::GraphBuild),
                "actual graph callback is protected by its application's graph-build guard");
            Write(*graphApp,graphSubsequentView);
            graphApp->CurrentFrame()=13;
            FinishAviRecord(graphApp);
            FinishAviRecord(graphApp);
            if(!finishedGraphResult){
                const HWND main=graphApp->MainWindow();
                SendMessageW(main,WM_CLOSE,0,0);SendMessageW(main,WM_CLOSE,0,0);
                Check(IsWindow(main)&&graphApp->MainWindow()==main&&g_Block==graphApp,
                    "real repeated main-window close requests cannot destroy the host inside graph construction");
            }
            Check(graphApp->RecordingWindow()==recordingWindow&&IsWindow(recordingWindow)&&
                graphApp->AviRenderResolutionOverridden(),"repeated finish requests do not tear down a graph still under construction");
            Check(Same(Read(*graphApp),graphSubsequentView)&&graphApp->CurrentFrame()==13,
                "graph callback cancellation does not restore fields or sample a frame before graph return");
            graphFinishedGeneration=graphApp->RecordingStartGeneration();
            Check(graphFinishedGeneration==generation&&deferredFinishShows==0,
                "deferred cancellation retains the in-flight start token and has no finish-window side effects");
            return finishedGraphResult;
        }
        if(nestedStart){nestedStart=false;StartAviRecordWindow(graphApp);}
        Write(*graphApp,Seed(22));return false;
    }return realGraph(recorder,window,english,record,config,fps,one,bitrate,first,count,audio,language,approved);
}
using Reset=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
Reset realReset=nullptr;IDirect3DDevice9* resetDevice=nullptr;unsigned rejectedResets=0;
HRESULT WINAPI FailReset(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters){
    if(GetCurrentThreadId()==hostThread&&device==resetDevice){++rejectedResets;return D3DERR_DEVICELOST;}
    return realReset(device,parameters);
}
enum class BoundaryAction{None,Finish,NewScene};
MMDApp* boundaryApp=nullptr;
BoundaryAction tailAction=BoundaryAction::None,projectionAction=BoundaryAction::None;
unsigned tailEvents=0,projectionEvents=0;
View boundaryCaptured{},boundaryEdited{};
decltype(&RefreshAfterFrameApply) realApply=nullptr;
decltype(&RefreshMainWindowViewport) realProjection=nullptr;
void BoundaryEvent(MMDApp* app,BoundaryAction action){
    if(action==BoundaryAction::Finish){
        FinishAviRecord(app);
        Check(!app->RecordingWindow()&&!app->FrameStepPlayback()&&!app->state.recordPlaybackStartPending,
            "real finish at callback boundary stops recording before returning to the old start");
        Check(Same(Read(*app),boundaryCaptured),"callback finish restores actual start snapshot before later edits");
    }else{
        app->AdvanceSceneGeneration();app->RenderWidth()=newSceneWidth;app->RenderHeight()=newSceneHeight;
    }
    Write(*app,boundaryEdited);app->CurrentFrame()=17;
}
void ApplyThenEvent(MMDApp* app){
    if(GetCurrentThreadId()==hostThread&&app==earlyWindowApp)++earlyApplyCalls;
    realApply(app);
    if(GetCurrentThreadId()==hostThread&&app==boundaryApp&&tailAction!=BoundaryAction::None&&
        app->RecordingWindow()&&app->state.recordSavedFrame==13&&app->CurrentFrame()==0&&app->AviRecordStartFrame()==0){
        const auto action=tailAction;tailAction=BoundaryAction::None;++tailEvents;BoundaryEvent(app,action);
    }
}
void ProjectThenEvent(MMDApp* app){
    realProjection(app);
    if(GetCurrentThreadId()==hostThread&&app==boundaryApp&&projectionAction!=BoundaryAction::None&&
        app->RecordingWindow()&&app->AviRenderResolutionOverridden()){
        const auto action=projectionAction;projectionAction=BoundaryAction::None;++projectionEvents;BoundaryEvent(app,action);
    }
}
void ReleasedAttempt(MMDApp* app){
    Check(!app->RecordingWindow()&&!app->FrameStepPlayback()&&!app->state.recordPlaybackStartPending,
        "abandoned attempt cannot retain a window or rearm playback");
    Check(!app->AviRenderResolutionOverridden()&&!seq::Active(app),"abandoned attempt clears resolution override and sequence ownership");
    Check(!app->CaptureRenderTarget()&&!app->CaptureSystemSurface()&&!app->CaptureReadbackPixels(),
        "abandoned attempt leaves no recording readback resources");
}
void CallbackBoundaries(MMDApp* app,Hooks& hooks){
    hooks.Add(&RefreshAfterFrameApply,&ApplyThenEvent,realApply);
    hooks.Add(&RefreshMainWindowViewport,&ProjectThenEvent,realProjection);
    const int width=app->RenderWidth(),height=app->RenderHeight();
    for(bool result:{false,true}){
        app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=13;
        Write(*app,Seed(6,0,true));graphSubsequentView=Seed(36,0,true);
        const auto path=output/(result?L"graph-scene-return-true.avi":L"graph-scene-return-false.avi");
        Check(!fs::exists(path),"new graph scene-change output");wcscpy_s(app->AviOutputPath(),256,path.c_str());
        app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=2;
        const auto calls=graphFailures;const auto scene=app->SceneGeneration();
        graphApp=app;sceneInGraph=true;finishedGraphResult=result;StartAviRecordWindow(app);
        graphApp=nullptr;sceneInGraph=false;
        Check(graphFailures==calls+1&&app->SceneGeneration()==scene+1,"single graph callback changes the scene exactly once");
        ReleasedAttempt(app);
        Check(app->CurrentFrame()==17&&Same(Read(*app),graphSubsequentView),"graph scene replacement retains the new view and playhead");
        Check(app->RenderWidth()==newSceneWidth&&app->RenderHeight()==newSceneHeight,"graph abandonment does not restore old scene dimensions");
        app->RestoreRecordingEditorView();Check(Same(Read(*app),graphSubsequentView),"old scene snapshot cannot subsequently restore");
        app->RenderWidth()=width;app->RenderHeight()=height;RefreshMainWindowViewport(app);
    }
    for(const auto action:{BoundaryAction::Finish,BoundaryAction::NewScene}){
        app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=13;
        Write(*app,Seed(7,0,true));boundaryCaptured=Read(*app);boundaryEdited=Seed(37,0,true);
        const auto events=tailEvents;const auto scene=app->SceneGeneration();
        PrepareSequence(app,action==BoundaryAction::Finish?L"tail_finish_":L"tail_scene_",2);
        boundaryApp=app;tailAction=action;StartAviRecordWindow(app);boundaryApp=nullptr;
        Check(tailEvents==events+1&&tailAction==BoundaryAction::None,"event occurs inside the real start-tail frame-apply return boundary");
        ReleasedAttempt(app);
        Check(app->CurrentFrame()==17&&Same(Read(*app),boundaryEdited),"old start tail cannot overwrite post-callback edits or seek again");
        Check(app->SceneGeneration()==scene+(action==BoundaryAction::NewScene?1:0),"tail callback scene transition is isolated");
        Check(app->RenderWidth()==(action==BoundaryAction::NewScene?newSceneWidth:width)&&
            app->RenderHeight()==(action==BoundaryAction::NewScene?newSceneHeight:height),
            "same-scene cancellation restores old dimensions; replacement keeps new dimensions");
        Check(CountImages(action==BoundaryAction::Finish?L"tail_finish_":L"tail_scene_")==0,"cancelled start-tail emits no output frames");
        app->RenderWidth()=width;app->RenderHeight()=height;RefreshMainWindowViewport(app);
    }
    app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=13;
    Write(*app,Seed(8,0,true));boundaryCaptured=Read(*app);boundaryEdited=Seed(38,0,true);
    const auto path=output/L"projection-finish.avi";Check(!fs::exists(path),"fresh projection-boundary output");
    wcscpy_s(app->AviOutputPath(),256,path.c_str());app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=2;
    const auto calls=graphFailures,events=projectionEvents;
    graphApp=app;boundaryApp=app;projectionAction=BoundaryAction::Finish;
    StartAviRecordWindow(app);boundaryApp=nullptr;graphApp=nullptr;
    Check(projectionEvents==events+1&&projectionAction==BoundaryAction::None,"cancel occurs after actual projection refresh with live override");
    Check(graphFailures==calls,"cancel after projection refresh must not enter graph creation");
    ReleasedAttempt(app);
    Check(Same(Read(*app),boundaryEdited)&&app->CurrentFrame()==17,"projection-boundary cancellation preserves subsequent view and playhead");
    Check(app->RenderWidth()==width&&app->RenderHeight()==height,"projection-boundary same-scene finish restores dimensions");
    HiddenWindows();
}
void EarlyWindowFailure(MMDApp* app,Hooks& hooks){
    // CallbackBoundaries installed the transparent frame-apply observer already.
    hooks.Add(&CreateWindowExA,&FailRecordWindow,realCreateA);
    hooks.Add(&StopPlayback,&ObserveStop,realStop);
    auto* model=mdl::Mdl(app->ModelSlot(0));
    Check(model&&model->boneCount>0&&model->boneTable&&model->bonePhysicsState,
        "early-failure fixture has actual author bone storage");
    auto& bone=model->boneTable[0];
    std::array<float,3> previousPosition{};std::array<float,4> previousRotation{};
    std::memcpy(previousPosition.data(),bone.trans,sizeof bone.trans);
    std::memcpy(previousRotation.data(),bone.rotQuat,sizeof bone.rotQuat);
    const auto previousPhysics=model->bonePhysicsState[0];
    const std::array<float,3> position{7.25f,-2.5f,9.f};
    const std::array<float,4> rotation{0.f,.6f,0.f,.8f};
    std::memcpy(bone.trans,position.data(),sizeof bone.trans);
    std::memcpy(bone.rotQuat,rotation.data(),sizeof bone.rotQuat);model->bonePhysicsState[0]=1;
    app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=13;
    Write(*app,Seed(42,0,true));const auto view=Read(*app);
    const auto revision=app->SceneRevision();const bool modified=app->SceneModified()!=0;
    Check(!app->RecordingWindow()&&!seq::Active(app)&&!app->state.playbackActive,
        "early-failure case begins idle without a prepared sequence");
    const auto file=output/L"create-window-failure.avi";
    Check(!fs::exists(file),"early-failure output is a new owned file");
    wcscpy_s(app->AviOutputPath(),256,file.c_str());
    // Avoid reset-induced evaluation: the only injected boundary is window creation.
    app->AviOutputWidth()=app->Renderer()->screenWidth;app->AviOutputHeight()=app->Renderer()->screenHeight;
    app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=2;app->AviRecordFps()=30;
    app->AviIncludeWave()=0;app->AviStereoOutput()=0;app->AviCloseAfterExport()=false;
    rejectedRecordWindows=expectedWindowErrors=earlyStopCalls=earlyApplyCalls=0;
    deferredFinishObserver=app;deferredFinishShows=0;earlyWindowApp=app;
    StartAviRecordWindow(app);
    earlyWindowApp=nullptr;deferredFinishObserver=nullptr;
    Check(rejectedRecordWindows==1&&expectedWindowErrors==1,
        "actual record-window creation fails once and reports only the exact expected error");
    Check(deferredFinishShows==1,"early window failure reaches real finish cleanup exactly once");
    Check(earlyStopCalls==0&&earlyApplyCalls==0,
        "failure before playback never stops playback or reevaluates the author frame");
    Check(std::memcmp(bone.trans,position.data(),sizeof bone.trans)==0&&
        std::memcmp(bone.rotQuat,rotation.data(),sizeof bone.rotQuat)==0&&model->bonePhysicsState[0]==1,
        "early failure preserves actual unregistered bone translation rotation and dirty state");
    Check(Same(Read(*app),view)&&app->CurrentFrame()==13&&app->SceneRevision()==revision&&
        (app->SceneModified()!=0)==modified,"early failure preserves editing view playhead and project state");
    Check(app->RecordingStartGeneration()==0&&!app->RecordingPlaybackEntered(),
        "early-failure attempt is retired without entering playback");
    ReleasedAttempt(app);
    auto edited=view;edited.numbers[0]+=1;Write(*app,edited);app->RestoreRecordingEditorView();
    Check(Same(Read(*app),edited),"failed window creation leaves no stale view capture");
    std::memcpy(bone.trans,previousPosition.data(),sizeof bone.trans);
    std::memcpy(bone.rotQuat,previousRotation.data(),sizeof bone.rotQuat);model->bonePhysicsState[0]=previousPhysics;
    HiddenWindows();
}
void ParentValidity(MMDApp* app){
    auto* raw=app->ModelSlot(0);Check(raw!=nullptr,"parent fixture occupies slot zero");
    auto* model=mdl::Mdl(raw);Check(model->boneTable&&model->boneCount,"parent fixture has a real bone table");
    for(int mutation=0;mutation<4;++mutation){
        auto original=Seed(0,2,true);original.parent=0;original.bone=0;original.suppressed=1;
        original.basis[12]=2;original.basis[13]=3;
        app->CameraMode()=0;app->state.followCameraEnabled=0;Write(*app,original);
        const auto oldIdentity=plugins::SceneObjectIdentity(raw);
        app->CaptureRecordingEditorView();Write(*app,Seed(30,0,true));
        const auto count=model->boneCount;auto* table=model->boneTable;
        if(mutation==0)app->ModelSlot(0)=nullptr;
        if(mutation==1)model->boneCount=0;
        if(mutation==2)model->boneTable=nullptr;
        if(mutation==3){
            plugins::ForgetSceneObject(raw);const auto replacement=plugins::SceneObjectIdentity(raw);
            Check(replacement.generation!=oldIdentity.generation||
                std::memcmp(&replacement.id,&oldIdentity.id,sizeof replacement.id)!=0,"same pointer now has a distinct stable object identity");
        }
        app->RestoreRecordingEditorView();const auto actual=Read(*app);
        app->ModelSlot(0)=raw;model->boneCount=count;model->boneTable=table;
        auto expected=original;expected.parent=-1;expected.bone=0;expected.suppressed=0;expected.basis.fill(0);
        for(unsigned i=0;i<16;i+=5)expected.basis[i]=1;
        Check(Same(actual,expected),"invalid parent detaches safely while restoring unrelated view fields");
        Write(*app,Seed(31,0,true));app->RestoreRecordingEditorView();
        Check(Same(Read(*app),Seed(31,0,true)),"invalid-parent capture is consumed once");
    }
}
void Lifecycle(){
    Hooks hooks;Host host;auto* app=host.app.get();CameraKeys(*app);
    ParentValidity(app);
    unsigned scenario=0;
    for(int reference=0;reference<3;++reference)for(bool cancel:{false,true}){
        app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=0;
        auto view=Seed(0,reference,true);view.parent=reference?0:-1;view.bone=0;Write(*app,view);
        RefreshMainWindowViewport(app);RenderFrameScene(app);
        const auto before=Read(*app);D3DMATRIX projection{};
        Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_PROJECTION,&projection)),"read actual pre-export projection");
        const auto revision=app->SceneRevision();const bool dirty=app->SceneModified()!=0;
        const auto prefix=L"view_"+std::to_wstring(scenario++)+L"_";StartSequence(app,prefix,cancel?30:2);
        unsigned steps=0;bool requested=false,sampled=false;
        while(seq::Active(app)&&steps++<120){
            Frame(app);
            sampled|=!Same(Read(*app),before);
            if(cancel&&!requested&&CountImages(prefix)>0){requested=true;FinishAviRecord(app);}
        }
        Check(!seq::Active(app)&&!app->RecordingWindow()&&!app->FrameStepPlayback(),"real completion/cancellation releases recording");
        Check(sampled,"recording really sampled a view different from editor state");
        const auto images=CountImages(prefix);Check(cancel?(requested&&images>0&&images<31):images==3,"real image count distinguishes completion from cancellation");
        Check(Same(Read(*app),before),"lifecycle restores all editor fields including parent reference");
        D3DMATRIX after{};app->Renderer()->device->GetTransform(D3DTS_PROJECTION,&after);
        if(std::memcmp(&projection,&after,sizeof after)!=0) {
            std::fprintf(stderr,"projection mismatch reference=%d cancel=%d render=%dx%d screen=%dx%d aspect=%g\n",reference,int(cancel),app->RenderWidth(),app->RenderHeight(),app->Renderer()->screenWidth,app->Renderer()->screenHeight,app->Renderer()->aspectRatio);
            for(int i=0;i<4;++i)for(int j=0;j<4;++j)std::fprintf(stderr,"[%d,%d] before=%.9g after=%.9g\n",i,j,double(projection.m[i][j]),double(after.m[i][j]));
        }
        Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_PROJECTION,&after))&&
            std::memcmp(&projection,&after,sizeof after)==0,"projection already restored before the next frame");
        Check(app->SceneRevision()==revision&&app->SceneModified()==dirty,"export view restoration does not create project edits");
        Frame(app);Check(Same(Read(*app),before),"next idle frame retains restored editing view");
        auto edited=before;edited.numbers[0]+=1;Write(*app,edited);app->RestoreRecordingEditorView();
        Check(Same(Read(*app),edited),"lifecycle consumed snapshot exactly once");
    }
    // The graph hook fails a real production start; it never replaces capture/restore.
    hooks.Add(&BuildRecordingGraph,&FailGraph,realGraph);
    app->CameraMode()=0;app->state.followCameraEnabled=0;Write(*app,Seed(0,0,true));
    const auto beforeFailure=Read(*app);const auto file=output/L"graph-start-failure.avi";
    Check(!fs::exists(file),"graph failure output belongs to this new test root");
    wcscpy_s(app->AviOutputPath(),256,file.c_str());graphApp=app;nestedStart=true;
    StartAviRecordWindow(app);graphApp=nullptr;
    Check(graphFailures==1&&!app->RecordingWindow()&&Same(Read(*app),beforeFailure),"graph-start failure invokes actual finish and restores captured fields");
    Write(*app,Seed(3,0,true));app->RestoreRecordingEditorView();Check(Same(Read(*app),Seed(3,0,true)),"failed start leaves no reusable snapshot");
#if MMD_ORIGINAL_PROFILE
    constexpr UINT closeMessage=WM_CLOSE;
#else
    constexpr UINT closeMessage=kSceneCloseReplayMessage;
#endif
    for(bool result:{false,true}){
        app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=0;
        Write(*app,Seed(5,0,true));graphCapturedView=Read(*app);graphSubsequentView=Seed(35,0,true);
        const auto path=output/(result?L"graph-finish-return-true.avi":L"graph-finish-return-false.avi");
        Check(!fs::exists(path),"reentrant graph scenario has a fresh owned output path");
        wcscpy_s(app->AviOutputPath(),256,path.c_str());
        app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=2;
        const auto calls=graphFailures;graphFinishedGeneration=0;
        MSG close{};
        if(!result)Check(!PeekMessageW(&close,app->MainWindow(),closeMessage,closeMessage,PM_NOREMOVE),
            "close-deferral case starts without a queued close for its own main window");
        graphApp=app;finishInGraph=true;finishedGraphResult=result;
        deferredFinishObserver=app;deferredFinishShows=0;
        StartAviRecordWindow(app);
        graphApp=nullptr;finishInGraph=false;deferredFinishObserver=nullptr;
        if(!result){
            const HWND main=app->MainWindow();
#if !MMD_ORIGINAL_PROFILE
            Check(app->deferredSceneMainClose==main,"close requests are deferred to the recording owner's main window");
            RetryDeferredSceneCloses(*app);
            Check(app->sceneMainClosePosted,"the coordinator's safe point posts the replay after startup");
#endif
            Check(IsWindow(main)&&PeekMessageW(&close,main,closeMessage,closeMessage,PM_REMOVE)&&
                close.hwnd==main&&close.message==closeMessage,"deferred close posts exactly one message to its owner");
            Check(!PeekMessageW(&close,main,closeMessage,closeMessage,PM_NOREMOVE),
                "repeated close requests coalesce to one posted close, removed without dispatch");
#if !MMD_ORIGINAL_PROFILE
            // Discard the intercepted test request to keep the fixture alive.
            app->sceneMainClosePosted=false;
            app->deferredSceneMainClose=nullptr;
#endif
        }
        Check(graphFailures==calls+1,"reentrant-finish scenario reaches the graph boundary exactly once");
        Check(app->RecordingStartGeneration()!=graphFinishedGeneration&&deferredFinishShows==1,
            "coalesced requests perform one finish-window restoration and invalidate the token after graph return");
        Check(!app->RecordingWindow()&&!app->FrameStepPlayback()&&!app->state.recordPlaybackStartPending,
            "outer start does not rearm recording after deferred finish regardless of graph result");
        Check(Same(Read(*app),graphCapturedView)&&!app->AviRenderResolutionOverridden(),
            "deferred finish restores the start view rather than the internal busy-period edits");
        Write(*app,graphSubsequentView);
        app->RestoreRecordingEditorView();
        Check(Same(Read(*app),graphSubsequentView),"deferred finish consumed the snapshot; later user edits remain untouched");
        HiddenWindows();
    }
    CallbackBoundaries(app,hooks);
    EarlyWindowFailure(app,hooks);
    // A real PostDeviceReset receives an injected failure. No physical device loss is claimed.
    auto* device=app->Renderer()->device;auto** table=*reinterpret_cast<void***>(device);
    hooks.Add(reinterpret_cast<Reset>(table[16]),&FailReset,realReset);
    app->CurrentFrame()=0;Write(*app,Seed(0,0,true));const auto beforeLost=Read(*app);
    StartSequence(app,L"pending_",30);Frame(app);
    resetDevice=device;Check(!PostDeviceReset(app)&&rejectedResets==1&&device_lost_wait::Pending(app),"real reset failure establishes pending recovery");
    FinishAviRecord(app);
    Check(!app->RecordingWindow()&&Same(Read(*app),beforeLost),"pending-device finish restores CPU view without successful device reset");
    Check(device_lost_wait::Pending(app),"view restoration does not falsely clear pending device loss");
    resetDevice=nullptr;Check(PostDeviceReset(app)&&!device_lost_wait::Pending(app),"real device reset can recover after injection ends");
    Check(Same(Read(*app),beforeLost),"successful recovery keeps restored editor fields");
    Frame(app);Check(Same(Read(*app),beforeLost),"recovered idle frame keeps editing view");HiddenWindows();
    std::puts("BOUNDARY windowed BMP lifecycle and injected Reset HRESULT; no actual hardware loss, fullscreen, H264 or MME claim");
}
}
void WaveRecording() {
    Hooks hooks;
    Host host;
    auto* app=host.app.get();
    app->EnglishUI()=1;
    const auto wav=installation/L"marker.wav";
    Check(wcscpy_s(app->WavePath(),256,wav.c_str())==0,"fixture WAV path fits");
    LoadWaveFile(app);
    Check(app->WaveEnabled() && fs::path(app->Audio()->path)==wav,"load fixture PCM with the production WAV loader");
    const auto sourceOffset=app->Audio()->dataOffset,sourceSize=app->Audio()->dataSize;
    struct Case {const wchar_t* name; int first,rate,fps; bool audio; int authoredFrames=0; bool compressed=false;};
    const Case cases[]={
        {L"from-zero",0,30,30,true},
        {L"nonzero",17,30,30,true},
        {L"playback-37",27,37,24,true},
        {L"unchecked",17,30,30,false},
        {L"short-source",54,30,30,true},
        {L"past-end",90,30,30,true},
        {L"five-frames",17,30,24,true,6},
        {L"utvideo",17,30,30,true,30,true},
    };
    for(const auto& test:cases){
        std::printf("AVI_WAVE begin=%ls\n",test.name);
        const auto path=output/(std::wstring(test.name)+L".avi");
        Check(wcscpy_s(app->AviOutputPath(),256,path.c_str())==0,"output path fits");
        app->CurrentFrame()=0;
        app->PlaybackCursorSeconds()=0;
        app->PlaybackTiming().playbackFps=test.rate;
        app->Recorder()->selectedCodec=-1;
        if(test.compressed)SelectUtVideoRgb(app);
        app->AviOutputWidth()=160;app->AviOutputHeight()=120;
        app->AviRecordStartFrame()=test.first;
        app->AviRecordEndFrame()=test.first+(test.authoredFrames?test.authoredFrames:test.rate)-1;
        app->AviRecordFps()=float(test.fps);app->AviIncludeWave()=test.audio;
        app->AviStereoOutput()=0;app->AviCloseAfterExport()=0;
        StartAviRecordWindow(app);
        Check(app->RecordingWindow() && app->Recorder()->graph,"start actual windowed AVI export");
        for(unsigned i=0;i<120&&app->RecordingWindow();++i)Frame(app);
        Check(!app->RecordingWindow() && !app->Recorder()->graph && !app->FrameStepPlayback(),
            "natural completion tears down graph and restores editing");
        Check(app->WaveEnabled() && app->Audio()->dataOffset==sourceOffset && app->Audio()->dataSize==sourceSize,
            "the loaded WAV range is unchanged after export");
        Check(fs::is_regular_file(path) && fs::file_size(path)>0,"natural completion produces an AVI");
    }
}

namespace {
constexpr int kRetryFirst = 17, kRetryLast = 22, kRetryFps = 24;
constexpr int kRetryWidth = 160, kRetryHeight = 120;
decltype(&GetUiSaveFileNameW) realAviRetryPicker = nullptr;
decltype(&UiDialogBoxParamA) realAviRetrySettings = nullptr;
decltype(&RecordingReadbackPass) realAviRetryReadback = nullptr;
decltype(&PostMessageW) realAviRetryPost = nullptr;

BOOL AviRetryPicker(LPOPENFILENAMEW picker) {
    if (aviProjectSave.OwnsPicker(picker)) return aviProjectSave.Pick(picker);
    if (!aviSaveRetry.app || GetCurrentThreadId() != hostThread || !picker ||
        picker->hwndOwner != aviSaveRetry.app->MainWindow())
        return realAviRetryPicker(picker);
    Check(picker->lpstrDefExt && (!wcscmp(picker->lpstrDefExt, L"avi") || !wcscmp(picker->lpstrDefExt, L"mp4")),
        "the actual video output picker owns the AVI destination");
    ++aviSaveRetry.pickerCalls;
    return wcscpy_s(picker->lpstrFile, picker->nMaxFile, aviSaveRetry.video.c_str()) == 0;
}

INT_PTR CALLBACK AviRetryDriveSettings(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    const auto result = aviSaveRetry.settingsProc(dialog, message, wp, lp);
    if (message != WM_INITDIALOG) return result;
    auto& app = *aviSaveRetry.app;
    const auto codec = GetDlgItem(dialog, panel::kAviCodecCombo);
    const auto row = FindUtVideoRgb(codec);
    const auto codecId = SendMessageW(codec, CB_GETITEMDATA, row, 0);
    SendMessageW(codec, CB_SETCURSEL, row, 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(panel::kAviCodecCombo, CBN_SELCHANGE), reinterpret_cast<LPARAM>(codec));
    Check(codecId >= 0 && app.Recorder()->selectedCodec == codecId && app.Recorder()->compressor,
        "select the actual UtVideo RGB compressor in the production settings");
    SetDlgItemInt(dialog, panel::kAviWidthEdit, kRetryWidth, FALSE);
    SetDlgItemInt(dialog, panel::kAviHeightEdit, kRetryHeight, FALSE);
    SetDlgItemInt(dialog, panel::kAviStartFrameEdit, kRetryFirst, FALSE);
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, kRetryLast, FALSE);
    SetDlgItemInt(dialog, panel::kAviFpsEdit, kRetryFps, FALSE);
    Check(IsWindowEnabled(GetDlgItem(dialog, panel::kAviWaveCheckbox)), "the real AVI options expose the loaded WAV");
    CheckDlgButton(dialog, panel::kAviWaveCheckbox, BST_CHECKED);
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, BST_CHECKED);
    using Parameter = export_save_workflow::Parameter;
    aviProjectSave.Drive(app, dialog, output / L"project-save",
        {{panel::kAviWidthEdit}, {panel::kAviHeightEdit}, {panel::kAviStartFrameEdit}, {panel::kAviEndFrameEdit},
         {panel::kAviFpsEdit}, {panel::kAviCodecCombo, Parameter::Choice}, {panel::kAviWaveCheckbox, Parameter::CheckBox},
         {panel::kAvi3dVisionCheckbox, Parameter::CheckBox}, {panel::kAviCloseAfterExportCheckbox, Parameter::CheckBox}}, Check);
    Check(SendMessageW(codec, CB_GETITEMDATA, SendMessageW(codec, CB_GETCURSEL, 0, 0), 0) == codecId &&
        app.Recorder()->selectedCodec == codecId, "saving retains the actual selected compressor identity");
    PostMessageW(dialog, WM_COMMAND, IDOK, 0);
    return result;
}

INT_PTR AviRetrySettings(HINSTANCE instance, LPCSTR resource, HWND owner, DLGPROC proc, LPARAM data) {
    if (!aviSaveRetry.app || GetCurrentThreadId() != hostThread || owner != aviSaveRetry.app->MainWindow() ||
        !IS_INTRESOURCE(resource) || reinterpret_cast<ULONG_PTR>(resource) != 0x28e)
        return realAviRetrySettings(instance, resource, owner, proc, data);
    aviSaveRetry.settingsProc = proc;
    aviSaveRetry.allowSettingsWindow = true;
    const auto result = realAviRetrySettings(instance, resource, owner, &AviRetryDriveSettings, data);
    aviSaveRetry.allowSettingsWindow = false;
    return result;
}

bool AviRetryReadback(MMDApp* app) {
    if (app != aviSaveRetry.app || !app->RecordingWindow() || !app->Recorder()->graph)
        return realAviRetryReadback(app);
    const double cursor = app->PlaybackCursorSeconds();
    const bool written = realAviRetryReadback(app);
    if (written) {
        const double expected = kRetryFirst / 30.0 + aviSaveRetry.readbacks / double(kRetryFps);
        Check(std::abs(cursor - expected) < .0001, "AVI readback follows the nonzero requested timeline range");
        ++aviSaveRetry.readbacks;
    }
    return written;
}

BOOL WINAPI AviRetryPost(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (aviSaveRetry.app && window == aviSaveRetry.app->MainWindow() && message == WM_CLOSE) {
        Check(GetCurrentThreadId() == hostThread && ++aviSaveRetry.closePosts == 1,
            "the completed AVI posts one close request");
        auto& app = *aviSaveRetry.app;
        Check(!app.SceneModified() && !SceneSaveInProgress() && !app.RecordingWindow() && !app.Recorder()->graph &&
            !app.FrameStepPlayback() && !app.AviRenderResolutionOverridden(),
            "close follows the saved project and full AVI teardown");
    }
    return realAviRetryPost(window, message, wp, lp);
}

LRESULT CALLBACK AviRetryWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE) {
        Check(aviSaveRetry.closePosts == 1 && ++aviSaveRetry.closeDeliveries == 1, "deliver the single AVI close");
    }
    return CallWindowProcW(aviSaveRetry.windowProc, window, message, wp, lp);
}

void AviSaveRetry(bool shadowOff) {
    Hooks hooks;
    Host host(!shadowOff);
    auto* app = host.app.get();
    const auto main = app->MainWindow();
    if (shadowOff) {
        if (app->state.selfShadowEnabled) SendMessageW(main, WM_COMMAND, 279, 0);
        Check(!app->state.selfShadowEnabled && DefaultEffectReadyForCapture(app),
            "AVI save retry uses the production self-shadow-off mode");
    }
    const auto wav = installation / L"marker.wav";
    Check(wcscpy_s(app->WavePath(), 256, wav.c_str()) == 0, "fixture WAV path fits");
    LoadWaveFile(app);
    Check(app->WaveEnabled() && fs::path(app->Audio()->path) == wav, "load the real PCM fixture");
    app->EnglishUI() = 1;
    app->RenderWidth() = 640;
    app->RenderHeight() = 480;
    app->CurrentFrame() = 0;
    app->PlaybackCursorSeconds() = 0;
    app->SceneModified() = 1;
    aviSaveRetry.app = app;
    aviSaveRetry.video = output / L"save-retry.avi";
    aviSaveRetry.windowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(main, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&AviRetryWindow)));
    Check(aviSaveRetry.windowProc != nullptr, "observe the editor's real close delivery");
    hooks.Add(&GetUiSaveFileNameW, &AviRetryPicker, realAviRetryPicker);
    hooks.Add(&UiDialogBoxParamA, &AviRetrySettings, realAviRetrySettings);
    hooks.Add(&RecordingReadbackPass, &AviRetryReadback, realAviRetryReadback);
    hooks.Add(&PostMessageW, &AviRetryPost, realAviRetryPost);

    SendMessageW(main, WM_COMMAND, 223, 0);
    Check(aviSaveRetry.pickerCalls == 1 && aviSaveRetry.settingsWindows == 1 && app->RecordingWindow() &&
        app->Recorder()->graph && app->AviOutputWidth() == kRetryWidth && app->AviOutputHeight() == kRetryHeight &&
        app->AviRecordStartFrame() == kRetryFirst && app->AviRecordEndFrame() == kRetryLast &&
        app->AviRecordFps() == kRetryFps && app->AviIncludeWave() && app->AviCloseAfterExport(),
        "the original AVI options start the actual compressed DirectShow export");
    for (unsigned i = 0; i < 120 && app->RecordingWindow(); ++i) Frame(app);
    Check(!app->RecordingWindow() && !app->Recorder()->graph && aviSaveRetry.readbacks == 5 &&
        fs::is_regular_file(aviSaveRetry.video), "the production graph writes and finalizes five AVI frames");
    bool quit = false;
    const auto deadline = GetTickCount64() + 15000;
    while (!quit && GetTickCount64() < deadline) {
        MSG message{};
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { Sleep(1); continue; }
        if (message.message == WM_QUIT) {
            Check(message.wParam == 0, "the editor exits normally");
            quit = true;
        } else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    Check(quit && !IsWindow(main) && aviSaveRetry.closePosts == 1 && aviSaveRetry.closeDeliveries == 1,
        "the saved project closes the real editor only after the completed AVI");
    std::puts("AVI_SAVE_RETRY frames=5 closePosts=1 closeDeliveries=1 realQuit=1");
    aviSaveRetry.app = nullptr;
}
} // namespace

int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Check(argc==3&&(!wcscmp(argv[1],L"--fields")||!wcscmp(argv[1],L"--lifecycle")||
        !wcscmp(argv[1],L"--locked-output")||!wcscmp(argv[1],L"--open-output")||!wcscmp(argv[1],L"--pending-effect")||
        !wcscmp(argv[1],L"--fullscreen-stereo")||!wcscmp(argv[1],L"--fullscreen-stereo-copy-failure")||
        !wcscmp(argv[1],L"--fullscreen-dpi-request")||!wcscmp(argv[1],L"--fullscreen-exclusive")||
        !wcscmp(argv[1],L"--compressed-output")||!wcscmp(argv[1],L"--capture-target-failures")||
        !wcscmp(argv[1],L"--avi-wave")||!wcscmp(argv[1],L"--avi-save-retry")||
        !wcscmp(argv[1],L"--avi-save-retry-shadow-off")),
        "--fields|--lifecycle|--locked-output|--open-output|--pending-effect|--fullscreen-stereo|--fullscreen-stereo-copy-failure|--fullscreen-dpi-request|--fullscreen-exclusive|--compressed-output|--capture-target-failures|--avi-wave|--avi-save-retry|--avi-save-retry-shadow-off NEW_ABSOLUTE_DIRECTORY");
    const bool dpiRequest=!wcscmp(argv[1],L"--fullscreen-dpi-request");
    if(dpiRequest){
        const auto setContext=reinterpret_cast<HANDLE(WINAPI*)(HANDLE)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext"));
        Check(setContext&&setContext(DPI_AWARENESS_CONTEXT_UNAWARE),"create a DPI-unaware test window");
    }
    Sandbox(argv[2]);
    if(!wcscmp(argv[1],L"--fields"))Fields();
    else if(!wcscmp(argv[1],L"--lifecycle"))Lifecycle();
    else if(!wcscmp(argv[1],L"--avi-wave"))WaveRecording();
    else if(!wcscmp(argv[1],L"--avi-save-retry"))AviSaveRetry(false);
    else if(!wcscmp(argv[1],L"--avi-save-retry-shadow-off"))AviSaveRetry(true);
    else if(!wcscmp(argv[1],L"--capture-target-failures"))CaptureTargetFailures();
    else {
        auto stereo=StereoScenario::Off;
        if(!wcscmp(argv[1],L"--fullscreen-stereo"))stereo=StereoScenario::Pack;
        if(!wcscmp(argv[1],L"--fullscreen-stereo-copy-failure"))stereo=StereoScenario::RightCopyFailure;
        if(dpiRequest)stereo=StereoScenario::DpiRequest;
        if(!wcscmp(argv[1],L"--fullscreen-exclusive"))stereo=StereoScenario::NativeExclusive;
        OutputRecording(!wcscmp(argv[1],L"--locked-output"),!wcscmp(argv[1],L"--pending-effect"),
            stereo,!wcscmp(argv[1],L"--compressed-output"));
    }
    std::printf("PASS recording-view mode=%ls checks=%u\n",argv[1],checks);return 0;
}
