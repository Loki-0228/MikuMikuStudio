// MAINTENANCE: regression-keep. Independent view oracle; hidden owned lifecycle only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "mikumikustudio/antialias.hpp"
#include "../src/app/device_lost_wait.hpp"

namespace mikumikustudio {
void RefreshAfterFrameApply(MMDApp*);
bool BuildRecordingGraph(DShowRecorder*,HWND,unsigned char,void*,void*,float,std::uint32_t,
    std::uint32_t,const wchar_t*,float,const WaveAudioContext*,unsigned char);
}

using namespace mikumikustudio;
namespace fs=std::filesystem;
namespace seq=mikumikustudio::image_sequence;
namespace {
unsigned checks=0;
DWORD hostThread=0;
fs::path output,installation;
void Check(bool value,const char* label) {
    ++checks;
    if(!value) { std::fprintf(stderr,"FAIL recording-view checks=%u: %s\n",checks,label); std::exit(1); }
}
bool Owned(HWND window) {
    DWORD process=0; const auto thread=GetWindowThreadProcessId(window,&process);
    return window&&process==GetCurrentProcessId()&&thread==hostThread;
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
    return realShow(w,GetCurrentThreadId()==hostThread&&Owned(w)?SW_HIDE:command);
}
BOOL WINAPI HiddenPosition(HWND w,HWND after,int x,int y,int width,int height,UINT flags){
    if(GetCurrentThreadId()==hostThread&&Owned(w))flags=(flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
    return realPosition(w,after,x,y,width,height,flags);
}
BOOL WINAPI HiddenPlacement(HWND w,const WINDOWPLACEMENT* p){
    if(GetCurrentThreadId()==hostThread&&Owned(w)&&p){auto v=*p;v.showCmd=SW_HIDE;return realPlacement(w,&v);}
    return realPlacement(w,p);
}
HWND WINAPI NoFocus(HWND w){return GetCurrentThreadId()==hostThread&&(!w||Owned(w))?GetFocus():realFocus(w);}
HWND WINAPI NoActive(HWND w){return GetCurrentThreadId()==hostThread&&(!w||Owned(w))?GetActiveWindow():realActive(w);}
BOOL WINAPI NoForeground(HWND w){return GetCurrentThreadId()==hostThread&&Owned(w)?FALSE:realForeground(w);}
int WINAPI RejectMessageW(HWND w,LPCWSTR text,LPCWSTR caption,UINT flags){
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
        Check(wcscmp(cls,L"#32770")!=0,"no unexpected modal dialog may be created");
    }
    if(code==HCBT_ACTIVATE||code==HCBT_SETFOCUS)return 1;
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
struct Host {
    std::unique_ptr<MMDApp> app=PlainApp();MMDApp* previous=g_Block;bool initialized=false;
    Host(){
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
    }
    ~Host(){if(initialized){device_lost_wait::Forget(app.get());ShutdownCleanup(app.get());}g_Block=previous;}
};
void Frame(MMDApp* app){app->DeltaTime()=1.f/30;app->state.messageSeen=1;FrameDriver(app);HiddenWindows();}
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
    std::uint32_t one,std::uint32_t bitrate,const wchar_t* wav,float seconds,const WaveAudioContext* audio,unsigned char language){
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
    }return realGraph(recorder,window,english,record,config,fps,one,bitrate,wav,seconds,audio,language);
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
    for(bool result:{false,true}){
        app->CameraMode()=0;app->state.followCameraEnabled=0;app->CurrentFrame()=0;
        Write(*app,Seed(5,0,true));graphCapturedView=Read(*app);graphSubsequentView=Seed(35,0,true);
        const auto path=output/(result?L"graph-finish-return-true.avi":L"graph-finish-return-false.avi");
        Check(!fs::exists(path),"reentrant graph scenario has a fresh owned output path");
        wcscpy_s(app->AviOutputPath(),256,path.c_str());
        app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=2;
        const auto calls=graphFailures;graphFinishedGeneration=0;
        MSG close{};
        if(!result)Check(!PeekMessageW(&close,app->MainWindow(),WM_CLOSE,WM_CLOSE,PM_NOREMOVE),
            "close-deferral case starts without a queued close for its own main window");
        graphApp=app;finishInGraph=true;finishedGraphResult=result;
        deferredFinishObserver=app;deferredFinishShows=0;
        StartAviRecordWindow(app);
        graphApp=nullptr;finishInGraph=false;deferredFinishObserver=nullptr;
        if(!result){
            const HWND main=app->MainWindow();
            Check(IsWindow(main)&&PeekMessageW(&close,main,WM_CLOSE,WM_CLOSE,PM_REMOVE)&&
                close.hwnd==main&&close.message==WM_CLOSE,"start-scope exit posts exactly the owned deferred close");
            Check(!PeekMessageW(&close,main,WM_CLOSE,WM_CLOSE,PM_REMOVE),
                "repeated close requests coalesce to one posted close, removed without dispatch");
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
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Check(argc==3&&(!wcscmp(argv[1],L"--fields")||!wcscmp(argv[1],L"--lifecycle")),"--fields|--lifecycle NEW_ABSOLUTE_DIRECTORY");
    Sandbox(argv[2]);
    if(!wcscmp(argv[1],L"--fields"))Fields();else Lifecycle();
    std::printf("PASS recording-view mode=%ls checks=%u\n",argv[1],checks);return 0;
}
