#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/mme_render_startup.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/frame_rate.hpp"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <wrl/client.h>
#include "../src/io/project_open.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace mikumikustudio;
namespace {
unsigned confirmations=0;
unsigned fileDialogs=0;
BOOL FileDialog(LPOPENFILENAMEW) { ++fileDialogs;return FALSE; }
decltype(&ShowWindow) show;
decltype(&CommandDispatch) dispatch;
BOOL WINAPI Hidden(HWND window,int command) { return show(window,GetParent(window)?command:SW_HIDE); }
void Dispatch(HWND control,WPARAM command) {
  const auto before=g_Block->state.sceneModified;
  dispatch(control,command);
  if(!before&&g_Block->state.sceneModified)
    std::printf("DIRTY command=%u notification=%u\n",LOWORD(command),HIWORD(command));
}
int Message(MMDApp*,HWND,UiMessage message,UINT,const wchar_t* detail) {
  if(message==UiMessage::OpenUnsaved){++confirmations;return IDCANCEL;}
  std::printf("Unexpected UI message=%u detail=%ls\n",unsigned(message),detail?detail:L"");return IDCANCEL;
}
void check(bool value,const char* why) { if(!value)throw std::runtime_error(why); }
void Hook(void* target,void* replacement,void** original=nullptr) {
  check(MH_CreateHook(target,replacement,original)==MH_OK&&MH_EnableHook(target)==MH_OK,"install isolated UI hooks");
}
void Clean(MMDApp& app,const char* phase) {
  std::printf("%s dirty=%u revision=%llu\n",phase,unsigned(app.state.sceneModified),app.SceneRevision());
  check(!app.SceneModified(),phase);
  { ProjectOpenUiScope open(&app);
    check(open.Prepare()&&open.ConfirmDiscard(false)&&confirmations==0,"unchanged empty scene opens without discard confirmation"); }
  const auto before=fileDialogs;
  CommandDispatch(nullptr,205);
  check(fileDialogs==before+1&&confirmations==0,"File Open reaches the picker without confirmation");
}
void ShadowThumb(MMDApp& app) {
  const auto project=std::filesystem::current_path()/L"shadow-readout.pmm";
  app.ShadowKeys()[0].distance=.00001f;
  app.ShadowDistance()=.00001f;
  check(SaveSceneCopy(&app,project.c_str()),"save a real PMM with shadow value 9999");
  HWND slider=GetDlgItem(app.MainWindow(),panel::kSelfShadowRangeSlider);
  HDC dc=GetDC(slider),memory=CreateCompatibleDC(dc);
  RECT bounds{};GetClientRect(slider,&bounds);
  HBITMAP bitmap=CreateCompatibleBitmap(dc,bounds.right,bounds.bottom);
  auto previous=SelectObject(memory,bitmap);ReleaseDC(slider,dc);
  SendMessageW(slider,TBM_SETPOS,TRUE,8750);
  CallWindowProcA(app.OriginalTrackbarProc(),slider,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT);
  RECT before{},after{};SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&before));
  check(LoadSceneFile(project.c_str()),"load the real PMM shadow setting");
  SendMessageW(slider,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT);
  SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&after));
  std::printf("shadow pos=%lld thumb before=%ld after=%ld\n",SendMessageW(slider,TBM_GETPOS,0,0),before.left,after.left);
  SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);
  check(ShadowSliderPosition(&app)==9999&&after.left>before.left,"loaded shadow readout moves the painted thumb to its new position");
  const auto revision=app.SceneRevision();
  for(const int position:{0,5000,9999}) {
    app.ShadowDistance()=float((10000.0-position)/100000.0);
    SyncSelfShadowControls(&app);
    RECT actual{},expected{};
    SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&actual));
    // Compare against the native control after forcing a distinct position.
    SendMessageW(slider,TBM_SETPOS,TRUE,position==0?9999:0);
    SendMessageW(slider,TBM_SETPOS,TRUE,position);
    SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&expected));
    char text[32]{};GetDlgItemTextA(app.MainWindow(),panel::kSelfShadowRangeEdit,text,sizeof(text));
    check(EqualRect(&actual,&expected)&&std::atoi(text)==position,"readout text and thumb match at both ends and midpoint");
  }
  check(app.SceneRevision()==revision&&!app.SceneModified(),"shadow readout refresh does not modify the project");
}
void ProjectChecks(MMDApp& app,const wchar_t* path) {
  const auto output=std::filesystem::current_path();
  for(unsigned load=0;load<2;++load) {
    std::printf("PROJECT load=%u begin\n",load);
    check(LoadSceneFile(path),"load the same actual project twice");
    std::printf("PROJECT load=%u complete\n",load);
    for(unsigned initial=0;initial<30;++initial) {
      MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
      app.MessageSeen()=1;FrameDriver(&app);Sleep(16);
    }
    for(const auto size:{SIZE{1200,820},SIZE{1000,1300},SIZE{1800,1000},SIZE{1200,820}}) {
      Microsoft::WRL::ComPtr<IDirect3DStateBlock9> oldFrame;
      check(SUCCEEDED(app.Renderer()->device->CreateStateBlock(D3DSBT_ALL,&oldFrame)),"capture pre-resize render state");
      check(SetWindowPos(app.MainWindow(),nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"resize project window");
      check(SUCCEEDED(oldFrame->Apply()),"restore an effect state captured before resize");
      for(int frame=0;frame<3;++frame) {
        app.MessageSeen()=1;app.DeltaTime()=0;RenderFrameScene(&app);
        D3DVIEWPORT9 actual{};app.Renderer()->device->GetViewport(&actual);
        const auto logical=app.ViewportRect();
        std::printf("VIEW size=%ld,%ld frame=%d logical=%ld,%ld,%ld,%ld actual=%lu,%lu,%lu,%lu camera=%.6g,%.6g,%.6g dist=%.6g fov=%.6g\n",
          size.cx,size.cy,frame,logical.left,logical.top,logical.right-logical.left,logical.bottom-logical.top,
          actual.X,actual.Y,actual.Width,actual.Height,app.CameraPositionX(),app.CameraPositionY(),app.CameraPositionZ(),app.CameraDistance(),app.CameraFov());
        check(actual.X==DWORD(logical.left)&&actual.Y==DWORD(logical.top)&&
          actual.Width==DWORD(logical.right-logical.left)&&actual.Height==DWORD(logical.bottom-logical.top),
          "scene render uses current layout after old effect state is restored");
      }
      using Microsoft::WRL::ComPtr;
      auto* device=app.Renderer()->device;
      ComPtr<IDirect3DSurface9> back,resolved,system;
      check(SUCCEEDED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back)),"read rendered frame");
      D3DSURFACE_DESC desc{};back->GetDesc(&desc);
      check(SUCCEEDED(device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr))&&
        SUCCEEDED(device->StretchRect(back.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE))&&
        SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&system,nullptr))&&
        SUCCEEDED(device->GetRenderTargetData(resolved.Get(),system.Get())),"capture completed GPU frame");
      const auto rect=app.ViewportRect();D3DLOCKED_RECT locked{};
      check(SUCCEEDED(system->LockRect(&locked,&rect,D3DLOCK_READONLY)),"read scene pixels");
      std::ofstream image(output/("scene-"+std::to_string(load)+"-"+std::to_string(size.cx)+"-"+std::to_string(size.cy)+".ppm"),std::ios::binary);
      image<<"P6\n"<<rect.right-rect.left<<' '<<rect.bottom-rect.top<<"\n255\n";
      for(int y=0;y<rect.bottom-rect.top;++y)for(int x=0;x<rect.right-rect.left;++x) {
        const auto* pixel=static_cast<unsigned char*>(locked.pBits)+y*locked.Pitch+x*4;
        const char rgb[]{char(pixel[2]),char(pixel[1]),char(pixel[0])};image.write(rgb,3);
      }
      system->UnlockRect();
    }
  }
}
}
int main() {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  if(MH_Initialize()!=MH_OK)return 2;
  const bool visible=GetEnvironmentVariableW(L"MMS_STARTUP_TEST_VISIBLE",nullptr,0)!=0;
  if(!visible)Hook(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(&Hidden),reinterpret_cast<void**>(&show));
  Hook(reinterpret_cast<void*>(&CommandDispatch),reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&dispatch));
  Hook(reinterpret_cast<void*>(&ShowUiMessage),reinterpret_cast<void*>(&Message));
  Hook(reinterpret_cast<void*>(&GetUiOpenFileNameW),reinterpret_cast<void*>(&FileDialog));
  auto* app=new MMDApp;g_Block=app;std::memset(&app->state,0,sizeof(app->state));app->InitDefaults();
  bool initialized=false;int code=0;
  try {
    std::wstring processorError;
    check(processor::Initialize(processorError),"initialize processor settings");
    EnableD3D12Native(true);
    plugins::SelectMmeRenderBackend(true);
    initialized=InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),visible?SW_SHOW:SW_HIDE);
    check(initialized,"initialize real startup host");
    check(updates::Initialize(app),"initialize update coordinator");
    wchar_t project[32768]{};
    if(GetEnvironmentVariableW(L"MMS_TEST_PROJECT",project,_countof(project))) {
      ProjectChecks(*app,project);
    } else {
    Clean(*app,"after startup");
    const auto settings=pmm_settings::Capture(*app);
    std::printf("default extension records=%zu\n",settings.records.size());
    Clean(*app,"after capturing default settings");
    for(unsigned i=0;i<120;++i){
      MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
      updates::SafePoint(app);Sleep(16);
    }
    Clean(*app,"after initial frames");
    check(ResetAppState(app),"new empty scene reset");
    Clean(*app,"after scene reset");
    ShadowThumb(*app);
    Clean(*app,"after project load and readout refresh");
    check(ResetAppState(app),"reset before authoring in an empty scene");
    SetShadowSliderPosition(app,6000);
    { ProjectOpenUiScope open(app);check(open.Prepare()&&!open.ConfirmDiscard(false)&&confirmations==1,
        "edited empty scene still prompts and cancellation is respected"); }
    std::puts("PASS startup/default-settings/reset/load remain clean; shadow thumb matches; actual edits retain confirmation");
    }
  }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());code=1;}
  if(initialized)ShutdownCleanup(app);
  g_Block=nullptr;delete app;MH_Uninitialize();return code;
}
