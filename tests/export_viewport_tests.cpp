// Hidden production export regression; reads GPU surfaces, never the desktop.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/default_effect.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/mf_h264_recorder.hpp"
#include "mikumikustudio/native_media.hpp"
#include <MinHook.h>
#include <Windows.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
using namespace mikumikustudio;
namespace mikumikustudio { void LayoutViewportPanels(MMDApp* app); }
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
const char* phase="init";
fs::path output;
LONG CALLBACK Fault(EXCEPTION_POINTERS*p){
 if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
 MEMORY_BASIC_INFORMATION m{};VirtualQuery(p->ExceptionRecord->ExceptionAddress,&m,sizeof m);wchar_t name[1024]{};
 GetModuleFileNameW((HMODULE)m.AllocationBase,name,1024);
 std::printf("FAULT phase=%s module=%ls offset=%llx target=%llx\n",phase,name,(unsigned long long)p->ExceptionRecord->ExceptionAddress-(unsigned long long)m.AllocationBase,p->ExceptionRecord->ExceptionInformation[1]);
 void*trace[25];auto n=CaptureStackBackTrace(0,25,trace,nullptr);
 for(USHORT i=0;i<n;++i){VirtualQuery(trace[i],&m,sizeof m);GetModuleFileNameW((HMODULE)m.AllocationBase,name,1024);std::printf("STACK %ls+%llx\n",name,(unsigned long long)trace[i]-(unsigned long long)m.AllocationBase);}
 ExitProcess(86);
}
int failures=0, viewportFailures=0, resetFailures=0;
unsigned resets=0;
void Check(bool ok,const char* label){std::printf("%s %s\n",ok?"PASS":"FAIL",label);if(!ok)++failures;}
void Need(bool ok,const char* label){Check(ok,label);if(!ok)std::exit(2);}
decltype(&ShowWindow) realShow;
BOOL WINAPI Hidden(HWND w,int){return realShow(w,SW_HIDE);}
LRESULT CALLBACK HideCreated(int code,WPARAM wp,LPARAM lp){
 if(code==HCBT_CREATEWND)reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
 if(code==HCBT_ACTIVATE)return 1;
 return CallNextHookEx(nullptr,code,wp,lp);
}
int WINAPI MessageA(HWND,LPCSTR text,LPCSTR,UINT){std::printf("DIALOG %s\n",text);if(std::getenv("PROBE_RESET_BLOCKER")&&std::strstr(text,"Failed to reset MikuMikuEffect"))return IDOK;ExitProcess(3);}
int WINAPI MessageW(HWND,LPCWSTR text,LPCWSTR,UINT){std::fwprintf(stderr,L"DIALOG %ls\n",text);if(std::getenv("PROBE_RESET_BLOCKER")&&std::wcsstr(text,L"Failed to reset MikuMikuEffect"))return IDOK;ExitProcess(3);}
template<class F>void Hook(F target,F replacement,F*original=nullptr){
 Need(MH_CreateHook((void*)target,(void*)replacement,(void**)original)==MH_OK,"install probe");
 Need(MH_EnableHook((void*)target)==MH_OK,"enable probe");
}
using Reset=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using SetView=HRESULT(WINAPI*)(IDirect3DDevice9*,const D3DVIEWPORT9*);
Reset realReset;SetView realView;
HRESULT WINAPI ObserveReset(IDirect3DDevice9*d,D3DPRESENT_PARAMETERS*p){
 const auto w=p->BackBufferWidth,h=p->BackBufferHeight;
 const auto hr=realReset(d,p);++resets;if(FAILED(hr))++resetFailures;
 std::printf("RESET phase=%s requested=%ux%u hr=%08lx recording=%d cached=%dx%d\n",phase,w,h,hr,
    g_Block->RecordingWindow()!=nullptr,g_Block->Renderer()->screenWidth,g_Block->Renderer()->screenHeight);
 return hr;
}
HRESULT WINAPI ObserveView(IDirect3DDevice9*d,const D3DVIEWPORT9*v){
 auto hr=realView(d,v);
 if(FAILED(hr)) {++viewportFailures;std::printf("VIEWPORT_FAIL phase=%s xy=%u,%u size=%ux%u hr=%08lx recording=%d\n",phase,v->X,v->Y,v->Width,v->Height,hr,g_Block->RecordingWindow()!=nullptr);}
 return hr;
}
struct State{D3DSURFACE_DESC back{},target{};D3DVIEWPORT9 viewport{};RECT rect{}, present{};};
State Snapshot(MMDApp*a,const char*label){
 State s;auto*d=a->Renderer()->device;ComPtr<IDirect3DSurface9>b,t;
 Need(SUCCEEDED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&b)),"read backbuffer");b->GetDesc(&s.back);
 Need(SUCCEEDED(d->GetRenderTarget(0,&t)),"read render target");t->GetDesc(&s.target);
 Need(SUCCEEDED(d->GetViewport(&s.viewport)),"read viewport");s.rect=a->ViewportRect();s.present=FramePresentationRect(a);
 std::printf("STATE %s back=%ux%u rt=%ux%u viewport=%u,%u,%u,%u rect=%ld,%ld,%ld,%ld render=%dx%d cache=%dx%d record=%d dpi=%u frame=%d rt_is_back=%d\n",label,
 s.back.Width,s.back.Height,s.target.Width,s.target.Height,s.viewport.X,s.viewport.Y,s.viewport.Width,s.viewport.Height,
 s.rect.left,s.rect.top,s.rect.right,s.rect.bottom,a->RenderWidth(),a->RenderHeight(),a->Renderer()->screenWidth,a->Renderer()->screenHeight,
 a->RecordingWindow()!=nullptr,GetDpiForWindow(a->MainWindow()),a->state.currentFrame,t.Get()==b.Get());return s;
}
std::vector<DWORD> Pixels(MMDApp*a,const char*name){
 auto*d=a->Renderer()->device;ComPtr<IDirect3DSurface9>b,resolved,cpu;
 Need(SUCCEEDED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&b)),"capture backbuffer");
 D3DSURFACE_DESC desc{};b->GetDesc(&desc);auto*src=b.Get();
 if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE){Need(SUCCEEDED(d->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)),"resolve allocation");Need(SUCCEEDED(d->StretchRect(src,nullptr,resolved.Get(),nullptr,D3DTEXF_NONE)),"resolve capture");src=resolved.Get();}
 Need(SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr)),"capture allocation");
 Need(SUCCEEDED(d->GetRenderTargetData(src,cpu.Get())),"GPU readback");D3DLOCKED_RECT lock{};Need(SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"read pixels");
 std::vector<DWORD> pixels(size_t(desc.Width)*desc.Height);
 for(UINT y=0;y<desc.Height;++y){auto*row=(DWORD*)((char*)lock.pBits+y*lock.Pitch);for(UINT x=0;x<desc.Width;++x)pixels[size_t(y)*desc.Width+x]=row[x]&0xffffff;}
 cpu->UnlockRect();
 std::ofstream f(output/(std::string(name)+".bgra"),std::ios::binary);f.write((const char*)pixels.data(),pixels.size()*4);
 std::ofstream(output/(std::string(name)+".txt"))<<desc.Width<<" "<<desc.Height;
 return pixels;
}
void Pump(){MSG m;for(int i=0;i<100&&PeekMessageW(&m,nullptr,0,0,PM_REMOVE);++i){TranslateMessage(&m);DispatchMessageW(&m);}}
void Frame(MMDApp*a){Pump();a->DeltaTime()=1.f/30;a->state.messageSeen=1;FrameDriver(a);}
void Render(MMDApp*a){a->state.messageSeen=1;RenderFrameScene(a);}
decltype(&WriteBuiltInH264Frame) realH264Write;
std::vector<std::vector<unsigned char>> cameraFrames;
unsigned cameraCase=0, cameraFrame=0;
bool ObserveH264Write(DShowRecorder* recorder,const void* data,std::size_t size) {
 const auto* pixels=static_cast<const unsigned char*>(data);
 if(cameraCase==0)cameraFrames.emplace_back(pixels,pixels+size);
 else Check(cameraFrame<cameraFrames.size()&&cameraFrames[cameraFrame].size()==size&&
     std::memcmp(cameraFrames[cameraFrame].data(),pixels,size)==0,
     "MP4 model-mode pixels match camera mode before encoding");
 ++cameraFrame;return realH264Write(recorder,data,size);
}
void CameraModeExports(MMDApp* app) {
 LoadModelFile(app,(output/L"model"/L"fixture.pmd").c_str(),false);
 Need(app->SelectedModel()!=nullptr,"load model for production model-mode export");
 Hook(&WriteBuiltInH264Frame,&ObserveH264Write,&realH264Write);
 auto* keys=app->CameraKeys();
 keys[0].next=1;keys[0].eye[0]=2.f;keys[0].eye[1]=10.f;keys[0].eye[2]=0.f;
 keys[0].distance=-45.f;keys[0].fov=30;keys[0].parentModel=-1;
 std::memset(keys[0].target,0,sizeof keys[0].target);
 keys[1]=keys[0];keys[1].frame=30;keys[1].previous=0;keys[1].next=0;keys[1].eye[0]=5.f;
 for(int channel=0;channel<6;++channel){keys[1].interpolation[0][channel]=keys[1].interpolation[1][channel]=20;
   keys[1].interpolation[2][channel]=keys[1].interpolation[3][channel]=107;}
 for(cameraCase=0;cameraCase<3;++cameraCase){
   const int mode=cameraCase==0?1:0,follow=cameraCase==2?1:0;
   app->CameraMode()=mode;app->state.followCameraEnabled=follow;
   app->CameraReferenceMode()=CameraAttachmentReference::None;
   app->CameraPositionY()=99.f;app->ViewOffsetX()=8.f;app->ViewOffsetY()=7.f;
   const auto path=output/(L"camera-mode-"+std::to_wstring(cameraCase)+L".mp4");
   wcscpy_s(app->AviOutputPath(),256,path.c_str());app->Recorder()->selectedCodec=kBuiltInH264Codec;
   app->AviOutputWidth()=640;app->AviOutputHeight()=360;
   app->AviRecordStartFrame()=0;app->AviRecordEndFrame()=5;app->AviRecordFps()=30;
   app->AviIncludeWave()=0;app->AviCloseAfterExport()=0;cameraFrame=0;
   StartAviRecordWindow(app);Need(app->RecordingWindow()!=nullptr,"start production H264 export");
   for(int step=0;step<60&&app->RecordingWindow();++step)Frame(app);
   Need(!app->RecordingWindow(),"MP4 export finishes normally");
   Check(cameraFrame==6,"MP4 has all six output samples including frame zero");
   Check(app->CameraMode()==mode&&app->state.followCameraEnabled==follow,"MP4 preserves editor and track selection");
   Need(fs::is_regular_file(path)&&fs::file_size(path)>0,"MP4 file is finalized");
   std::wstring error;int width=0,height=0;double duration=0;
   Need(media::OpenVideo(app,path.c_str(),width,height,duration,error,640),"decode finalized MP4");
   Check(width==640&&height==360,"MP4 output dimensions are unchanged");
   Need(media::VideoFrame(app,0,error)!=nullptr&&media::VideoFrame(app,5./30,error)!=nullptr,
        "MP4 first and final frames decode");
   media::ReleaseVideo(app);
   std::printf("MP4 mode=%d follow=%d frames=%u bytes=%llu\n",mode,follow,cameraFrame,
       static_cast<unsigned long long>(fs::file_size(path)));
 }
 MH_DisableHook(reinterpret_cast<void*>(&WriteBuiltInH264Frame));
 MH_RemoveHook(reinterpret_cast<void*>(&WriteBuiltInH264Frame));
}
void SettleTemporal(MMDApp* app) {
 const char* aa=std::getenv("PROBE_AA");
 if(!aa||std::strcmp(aa,"taa")!=0)return;
 // TAA cycles eight jitter positions and retains a moving average. Compare
 // converged output at the same phase, without releasing/recreating targets
 // (which would mask an export-restoration defect).
 for(int frame=0;frame<136;++frame){
   Frame(app);
   if(frame>=127&&antialias::AccumulatedSamples(app)%8==0)break;
 }
 Check(antialias::AccumulatedSamples(app)>=128,"TAA accumulated a complete stable comparison cycle");
}
void Compare(MMDApp*a,const State&before,const std::vector<DWORD>&pixels,const char*name){
 const auto callerDpi=GetThreadDpiAwarenessContext();
 SettleTemporal(a);
 Render(a);auto after=Snapshot(a,name);auto out=Pixels(a,name);
 Check(AreDpiAwarenessContextsEqual(callerDpi,GetThreadDpiAwarenessContext()),"preview preserves caller DPI context");
 Check(EqualRect(&before.present,&after.present),"presentation rectangle restored");
 Check(after.viewport.X+after.viewport.Width<=after.target.Width&&after.viewport.Y+after.viewport.Height<=after.target.Height,"viewport fits actual render target");
 Check(after.back.Width==before.back.Width&&after.back.Height==before.back.Height,"actual backbuffer dimensions restored");
 Check(after.target.Width==before.target.Width&&after.target.Height==before.target.Height,"actual render target dimensions restored");
 Check(std::memcmp(&after.viewport,&before.viewport,sizeof before.viewport)==0,"actual GPU viewport restored");
 Check(EqualRect(&before.rect,&after.rect),"host viewport rectangle restored");
 size_t changed=0,total=0,inkBefore=0,inkAfter=0;
 if(out.size()==pixels.size())for(int y=before.rect.top+30;y<before.rect.bottom-30;++y)for(int x=before.rect.left+10;x<before.rect.right-10;++x){
   size_t i=size_t(y)*before.back.Width+x;if(i>=out.size())continue;
   ++total;changed+=out[i]!=pixels[i];inkBefore+=pixels[i]!=0xffffff;inkAfter+=out[i]!=0xffffff;
 }
 std::printf("PIXELS %s changed=%zu total=%zu ink_before=%zu ink_after=%zu\n",name,changed,total,inkBefore,inkAfter);
 Check(total>0&&inkBefore>0&&inkAfter>0,"scene remains rendered after export");
 Check(out.size()==pixels.size()&&changed==0,"export restores identical static scene pixels");
}
}
int wmain(int argc,wchar_t**argv){
 std::setvbuf(stdout,nullptr,_IONBF,0);Need(argc>=5,"args: dpi large export-width export-height [effect...]");
 output=fs::current_path();
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 AddVectoredExceptionHandler(1,Fault);
 SetThreadDpiAwarenessContext(std::wstring(argv[1])==L"aware"?DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:DPI_AWARENESS_CONTEXT_UNAWARE);
 Need(MH_Initialize()==MH_OK,"initialize probes");Hook(&ShowWindow,&Hidden,&realShow);Hook(&MessageBoxA,&MessageA);Hook(&MessageBoxW,&MessageW);
 auto cbt=SetWindowsHookExW(WH_CBT,HideCreated,nullptr,GetCurrentThreadId());
 auto*a=new MMDApp;g_Block=a;a->state=MMDAppState{};a->InitDefaults();
 Need(InitMainWindowAndD3D(a,GetModuleHandleW(nullptr),SW_HIDE),"initialize production MMS");HandleWindowSize(a);
 for(int i=0;i<5;++i)Frame(a);
 auto*r=a->Renderer();auto*d=r->device;auto**v=*(void***)d;
 Hook((Reset)v[16],&ObserveReset,&realReset);Hook((SetView)v[47],&ObserveView,&realView);
 std::printf("CONFIG MME=%d large=%ls dpi=%ls export=%lsx%ls\n",GetModuleHandleW(L"MMEffect.dll")!=nullptr,argv[2],argv[1],argv[3],argv[4]);
 if(std::getenv("PROBE_REQUIRE_MME"))Need(GetModuleHandleW(L"MMEffect.dll")!=nullptr,"real MME is loaded");
 phase="prepare";
 if(std::wstring(argv[2])==L"large"){
   SetWindowPos(a->MainWindow(),nullptr,20,20,2400,1400,SWP_NOACTIVATE|SWP_NOZORDER);HandleWindowSize(a);
   r->screenWidth=2560;r->screenHeight=1600;r->presentParameters.BackBufferWidth=2560;r->presentParameters.BackBufferHeight=1600;
   Need(PostDeviceReset(a),"prepare real large GPU target");
 }
 LoadAccessoryFile((output/L"fixture.x").c_str());Need(a->AccessorySlot(0)!=nullptr,"load static diagnostic geometry");
 for(int i=5;i<argc;++i){LoadAccessoryFile(argv[i]);Need(a->AccessorySlot(i-4)!=nullptr,"load real effect accessory");}
 if(std::getenv("PROBE_CAMERA_MODES")){
   CameraModeExports(a);ShutdownCleanup(a);g_Block=nullptr;delete a;UnhookWindowsHookEx(cbt);
   return failures?1:0;
 }
 if(const char*aa=std::getenv("PROBE_AA")){
   std::wstring error;
   const auto algorithm=std::strcmp(aa,"taa")==0?antialias::Algorithm::Taa:antialias::Algorithm::Fxaa;
   Need(antialias::Select(a,algorithm,error),"select production antialiasing");
   const auto deadline=GetTickCount64()+10000;
   do{Frame(a);Sleep(10);}while(antialias::Initializing(a)&&GetTickCount64()<deadline);
   Need(!antialias::Initializing(a),"antialiasing shader is ready");
 }
 if(const char*scale=std::getenv("PROBE_SCALE")){std::wstring error;Need(viewport_render_scale::Configure(a,false,std::atof(scale),error),"set production preview scale");std::printf("SCALE percent=%s\n",scale);}
 for(int i=0;i<30;++i){Frame(a);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
 SetCurrentDirectoryW(output.c_str());
 SettleTemporal(a);
 RefreshMainWindowViewport(a);Render(a);
 auto before=Snapshot(a,"before");auto pixels=Pixels(a,"before");
 const int oldW=a->RenderWidth(),oldH=a->RenderHeight();
 const auto windowDpi=GetWindowDpiAwarenessContext(a->MainWindow());
 const auto foreignDpi=std::wstring(argv[1])==L"aware"?DPI_AWARENESS_CONTEXT_UNAWARE:DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
 for(int cycle=0;cycle<2;++cycle){
  SetThreadDpiAwarenessContext(windowDpi);
  phase=cycle==0?"complete-export":"cancel-export";
  auto path=output/(cycle==0?L"complete.avi":L"cancel.avi");wcscpy_s(a->AviOutputPath(),256,path.c_str());
  a->Recorder()->selectedCodec=-1;a->AviOutputWidth()=_wtoi(argv[3]);a->AviOutputHeight()=_wtoi(argv[4]);
  a->AviRecordStartFrame()=0;a->AviRecordEndFrame()=cycle==0?1:30;a->AviRecordFps()=30;a->AviIncludeWave()=0;a->AviCloseAfterExport()=0;
  StartAviRecordWindow(a);Need(a->RecordingWindow()!=nullptr,"production AVI recording started");Snapshot(a,"recording");
  if(cycle==0&&std::getenv("PROBE_DPI_CHANGE")){SetThreadDpiAwarenessContext(foreignDpi);std::puts("INJECT thread DPI context changed before natural completion");}
  for(int i=0;i<120&&a->RecordingWindow();++i){Frame(a);if(cycle==1&&i==1){
    ComPtr<IDirect3DVertexBuffer9>blocker;
    if(std::getenv("PROBE_RESET_BLOCKER"))Need(SUCCEEDED(d->CreateVertexBuffer(64,0,D3DFVF_XYZ,D3DPOOL_DEFAULT,&blocker,nullptr)),"retain actual reset-blocking resource");
    if(std::getenv("PROBE_DPI_CHANGE")){SetThreadDpiAwarenessContext(foreignDpi);std::puts("INJECT thread DPI context changed during export");}
    FinishAviRecord(a);
    blocker.Reset();
  }}
  Need(a->RecordingWindow()==nullptr,"recording exits");Check(a->RenderWidth()==oldW&&a->RenderHeight()==oldH,"saved resolution restored");
  for(int i=0;i<(std::getenv("PROBE_AA")?32:4);++i)Frame(a);
  Compare(a,before,pixels,cycle==0?"after-complete":"after-cancel");
 }
 phase="resize-after";HandleWindowSize(a);Compare(a,before,pixels,"after-layout");
 if(std::getenv("PROBE_DPI_CHANGE")){
   phase="restore-dpi";SetThreadDpiAwarenessContext(windowDpi);HandleWindowSize(a);
   for(int i=0;i<(std::getenv("PROBE_AA")?32:4);++i)Frame(a);
   Compare(a,before,pixels,"after-dpi-restored");
 }
 // Exercise the geometry entry points even when called outside WndProc.
 // Windows temporarily corrects DPI inside WndProc, which can otherwise hide
 // this regression in ordinary resize-only tests.
 SetThreadDpiAwarenessContext(windowDpi);
 HWND floating=CreateWindowExW(0,L"STATIC",L"DPI regression",WS_OVERLAPPEDWINDOW,
     10,10,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 Need(floating!=nullptr,"create hidden floating viewport");
 for(int mode=0;mode<3;++mode){
   a->FullscreenMode()=mode==1;
   a->FloatingWindow()=mode==2?floating:nullptr;
   SetThreadDpiAwarenessContext(windowDpi);
   RefreshMainWindowViewport(a);
   const auto expected=Snapshot(a,"mode-baseline");
   SetThreadDpiAwarenessContext(foreignDpi);
   RefreshMainWindowViewport(a);
   const auto actual=Snapshot(a,"mode-foreign-dpi");
   Check(EqualRect(&expected.rect,&actual.rect),"main/fullscreen/floating scene geometry ignores caller DPI");
   Check(EqualRect(&expected.present,&actual.present),"main/fullscreen/floating presentation ignores caller DPI");
   Check(std::memcmp(&expected.viewport,&actual.viewport,sizeof actual.viewport)==0,"main/fullscreen/floating GPU viewport ignores caller DPI");
   Check(AreDpiAwarenessContextsEqual(foreignDpi,GetThreadDpiAwarenessContext()),"geometry scopes restore caller DPI");
 }
 a->FullscreenMode()=0;a->FloatingWindow()=nullptr;
 DestroyWindow(floating);SetThreadDpiAwarenessContext(windowDpi);HandleWindowSize(a);
 RECT panelBefore{}, panelAfter{};
 const HWND panel=GetDlgItem(a->MainWindow(),557);
 Need(panel!=nullptr,"viewport toolbar exists");
 GetWindowRect(panel,&panelBefore);
 SetThreadDpiAwarenessContext(foreignDpi);
 LayoutViewportPanels(a);
 { const WindowDpiScope dpi(a->MainWindow()); GetWindowRect(panel,&panelAfter); }
 Check(EqualRect(&panelBefore,&panelAfter),"standalone toolbar layout ignores caller DPI");
 const auto savedParameters=r->presentParameters;
 a->FullscreenMode()=1;ApplyFullscreenWindowState(a);
 RECT fullscreenClient{};
 { const WindowDpiScope dpi(a->MainWindow()); GetClientRect(a->MainWindow(),&fullscreenClient); }
 Check(r->presentParameters.BackBufferWidth==static_cast<UINT>(fullscreenClient.right)&&
       r->presentParameters.BackBufferHeight==static_cast<UINT>(fullscreenClient.bottom),
       "fullscreen buffer request uses window DPI coordinates");
 Need(PostDeviceReset(a),"fullscreen device reset");
 a->FullscreenMode()=0;ApplyFullscreenWindowState(a);
 Need(PostDeviceReset(a),"restore ordinary device");
 Check(r->presentParameters.BackBufferWidth==savedParameters.BackBufferWidth&&
       r->presentParameters.BackBufferHeight==savedParameters.BackBufferHeight,
       "fullscreen exit restores buffer dimensions under foreign DPI");
 Check(AreDpiAwarenessContextsEqual(foreignDpi,GetThreadDpiAwarenessContext()),"fullscreen transition preserves caller DPI");
 SetThreadDpiAwarenessContext(windowDpi);
 if(std::getenv("PROBE_RESET_BLOCKER"))Check(resetFailures>0,"reset failure injection actually reached the driver");
 Check(viewportFailures==0,"viewport API calls succeed");
 std::printf("SUMMARY failures=%d viewport_failures=%d reset_failures=%d resets=%u\n",failures,viewportFailures,resetFailures,resets);
 phase="shutdown";ShutdownCleanup(a);g_Block=nullptr;delete a;UnhookWindowsHookEx(cbt);
 return failures?1:0;
}
