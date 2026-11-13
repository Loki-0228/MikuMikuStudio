// Hidden production export regression; reads GPU surfaces, never the desktop.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
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
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/recording_output_path.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "export_save_workflow_checks.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#endif
#include <MinHook.h>
#include "plugins/coordinator_fixture.hpp"
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
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <limits>
#include <type_traits>
#include <xmmintrin.h>
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
// These probes require Windows 10+, while the product keeps its Win7 target.
// Resolve modern DPI entry points without raising WINVER or importing them.
using DpiForWindowProc=UINT(WINAPI*)(HWND);
using ThreadDpiContextProc=DPI_AWARENESS_CONTEXT(WINAPI*)();
using SetThreadDpiContextProc=DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
using WindowDpiContextProc=DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
using DpiContextsEqualProc=BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT,DPI_AWARENESS_CONTEXT);
DpiForWindowProc testDpiForWindow=nullptr;
ThreadDpiContextProc testThreadDpiContext=nullptr;
SetThreadDpiContextProc testSetThreadDpiContext=nullptr;
WindowDpiContextProc testWindowDpiContext=nullptr;
DpiContextsEqualProc testDpiContextsEqual=nullptr;
void LoadDpiTestApis(){
 const auto user32=GetModuleHandleW(L"user32.dll");if(!user32)return;
 testDpiForWindow=reinterpret_cast<DpiForWindowProc>(GetProcAddress(user32,"GetDpiForWindow"));
 testThreadDpiContext=reinterpret_cast<ThreadDpiContextProc>(GetProcAddress(user32,"GetThreadDpiAwarenessContext"));
 testSetThreadDpiContext=reinterpret_cast<SetThreadDpiContextProc>(GetProcAddress(user32,"SetThreadDpiAwarenessContext"));
 testWindowDpiContext=reinterpret_cast<WindowDpiContextProc>(GetProcAddress(user32,"GetWindowDpiAwarenessContext"));
 testDpiContextsEqual=reinterpret_cast<DpiContextsEqualProc>(GetProcAddress(user32,"AreDpiAwarenessContextsEqual"));
}
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
 a->RecordingWindow()!=nullptr,testDpiForWindow(a->MainWindow()),a->state.currentFrame,t.Get()==b.Get());return s;
}
// PROBE_RESTORE_DIAGNOSTIC selects the ordinary-export snapshots below:
// "1" full (text VB + font atlas + device state), "2" device state only,
// "3" text VB + device state, "4" font atlas + device state, "5" device state
// only for the real-Frame post-failure controls. Any other value or unset
// keeps every new observation off, with no extra device I/O. These snapshots
// are diagnostics, not a production fix, and never change the pixel
// comparisons, their assertions or the exit status.
int RestoreDiagnosticMode(){
 const char* value=std::getenv("PROBE_RESTORE_DIAGNOSTIC");
 if(value==nullptr)return 0;
 if(std::strcmp(value,"1")==0)return 1;
 if(std::strcmp(value,"2")==0)return 2;
 if(std::strcmp(value,"3")==0)return 3;
 if(std::strcmp(value,"4")==0)return 4;
 if(std::strcmp(value,"5")==0)return 5;
 return 0;
}
// Independent process endpoints use the historical fixture and mode 2.
// A uniform- prefix enables a controlled background at the two known pixels.
enum class HudEndpoint { off,layoutText,layoutSkip,repeatText,repeatSkip };
HudEndpoint hudEndpoint=HudEndpoint::off;
bool hudUniformBackground=false;
constexpr D3DRECT kHudUniformRect{1111,440,1112,442};
// Match the original resolved background so the control preserves its
// brightness while giving all eight samples the same value at each pixel.
constexpr D3DCOLOR kHudUniformColors[]={0xffbfbfbf,0xffdedede};

bool HudUniformPixel(int x,int y){
 return x>=kHudUniformRect.x1&&x<kHudUniformRect.x2&&
     y>=kHudUniformRect.y1&&y<kHudUniformRect.y2;
}
const char* HudEndpointStage(){
 return hudEndpoint==HudEndpoint::layoutText||hudEndpoint==HudEndpoint::layoutSkip?
     "after-layout":"diag-repeat";
}
bool HudEndpointSkip(){return hudEndpoint==HudEndpoint::layoutSkip||hudEndpoint==HudEndpoint::repeatSkip;}
bool HudEndpointAt(const char* name){
 return hudEndpoint!=HudEndpoint::off&&std::strcmp(name,HudEndpointStage())==0;
}
void ParseHudEndpoint(int argc,wchar_t** argv){
 const char* value=std::getenv("PROBE_RESTORE_HUD_ENDPOINT");
 if(!value||!*value||std::strcmp(value,"0")==0)return;
 constexpr char uniformPrefix[]="uniform-";
 hudUniformBackground=std::strncmp(value,uniformPrefix,sizeof(uniformPrefix)-1)==0;
 if(hudUniformBackground)value+=sizeof(uniformPrefix)-1;
 if(std::strcmp(value,"after-layout-text")==0)hudEndpoint=HudEndpoint::layoutText;
 else if(std::strcmp(value,"after-layout-skip")==0)hudEndpoint=HudEndpoint::layoutSkip;
 else if(std::strcmp(value,"diag-repeat-text")==0)hudEndpoint=HudEndpoint::repeatText;
 else if(std::strcmp(value,"diag-repeat-skip")==0)hudEndpoint=HudEndpoint::repeatSkip;
 else Need(false,"EXPORT-HUD-ENDPOINT rejects unknown endpoint values");
 Need(argc==5&&(std::wcscmp(argv[1],L"aware")==0||std::wcscmp(argv[1],L"unaware")==0),
     "EXPORT-HUD-ENDPOINT requires ordinary args: dpi size export-width export-height");
 Need(RestoreDiagnosticMode()==2,"EXPORT-HUD-ENDPOINT requires historical state-only mode 2");
 // Exclude other observers, alternate entries and fixture-changing overrides.
 LPWCH environment=GetEnvironmentStringsW();
 Need(environment!=nullptr,"EXPORT-HUD-ENDPOINT reads its environment");
 bool allowed=true;
 for(const wchar_t* entry=environment;*entry;entry+=std::wcslen(entry)+1){
  if(_wcsnicmp(entry,L"PROBE_",6)!=0)continue;
  if(_wcsnicmp(entry,L"PROBE_RESTORE_HUD_ENDPOINT=",27)==0||
     _wcsnicmp(entry,L"PROBE_RESTORE_DIAGNOSTIC=",25)==0)continue;
  std::fwprintf(stderr,L"EXPORT-HUD-ENDPOINT conflicting override: %ls\n",entry);
  allowed=false;
 }
 FreeEnvironmentStringsW(environment);
 Need(allowed,"EXPORT-HUD-ENDPOINT rejects all other PROBE overrides");
 for(const char* suffix:{".endpoint.meta",".endpoint-raw.bgra",".endpoint-text-vb.bin",".endpoint-text-atlas.bgra"}){
  std::error_code error;
  const bool exists=fs::exists(fs::current_path()/(std::string(HudEndpointStage())+suffix),error);
  Need(!error&&!exists,"EXPORT-HUD-ENDPOINT requires a fresh output directory for each independent process");
 }
 std::printf("EXPORT-HUD-ENDPOINT requested=%s diagnostic_only=1 full_regression=not_run\n",value);
}
struct HudReadResult { const char* operation;HRESULT hr; };
HudReadResult hudReads[16]{};
unsigned hudReadCount=0;
bool hudReadOpen=false,hudReadDone=false,hudWatchOpen=false;
bool hudPriorLayoutCaptured=false;
IDirect3DSurface9* hudExpectedBack=nullptr;
D3DSURFACE_DESC hudBackDesc{};
INT hudBackPitch=0;
std::vector<DWORD> hudRaw;
HRESULT HudReadHr(const char* operation,HRESULT hr){
 if(hudReadOpen){
  Need(hudReadCount<16,"EXPORT-HUD-ENDPOINT read ledger has room");
  hudReads[hudReadCount++]={operation,hr};
  std::printf("EXPORT-HUD-ENDPOINT read=%u operation=%s hr=0x%08lx\n",
      hudReadCount,operation,static_cast<unsigned long>(hr));
 }
 return hr;
}
std::uint32_t RestoreDiagFloatBits(float value){
 std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof bits);return bits;
}
void RestoreDiagFloats(std::ofstream& meta,const char* label,const float* values,int count){
 meta<<label<<"=";
 for(int i=0;i<count;++i)meta<<(i?",":"")<<values[i];
 meta<<" "<<label<<"_bits=";
 for(int i=0;i<count;++i)meta<<(i?",":"")<<"0x"<<std::hex<<RestoreDiagFloatBits(values[i])<<std::dec;
 meta<<"\n";
}
void RestoreDiagMatrix(std::ofstream& meta,const char* label,const D3DMATRIX& matrix){
 RestoreDiagFloats(meta,label,reinterpret_cast<const float*>(&matrix),16);
}
void RestoreDiagDeviceState(std::ofstream& meta,MMDApp* app){
 auto* renderer=app->Renderer();
 if(renderer){
  meta<<"viewScale="<<renderer->viewScale<<" viewScale_bits=0x"<<std::hex
      <<RestoreDiagFloatBits(renderer->viewScale)<<std::dec<<"\n";
 }else meta<<"renderer=null\n";
 const float overlayScale=FrameOverlayScale(app);
 meta<<"FrameOverlayScale="<<overlayScale<<" FrameOverlayScale_bits=0x"<<std::hex
     <<RestoreDiagFloatBits(overlayScale)<<std::dec<<"\n";
 const RECT viewport=app->ViewportRect();
 const RECT overlay=FrameOverlayRect(app);
 meta<<"ViewportRect="<<viewport.left<<","<<viewport.top<<","<<viewport.right<<","<<viewport.bottom<<"\n";
 meta<<"FrameOverlayRect="<<overlay.left<<","<<overlay.top<<","<<overlay.right<<","<<overlay.bottom<<"\n";
 meta<<"mxcsr=0x"<<std::hex<<_mm_getcsr()<<std::dec<<"\n";
 auto* device=renderer?renderer->device:nullptr;
 if(device==nullptr){meta<<"device=null\n";return;}
 D3DVIEWPORT9 gpuViewport{};
 const HRESULT viewportHr=device->GetViewport(&gpuViewport);
 meta<<"GetViewport_hr=0x"<<std::hex<<static_cast<unsigned long>(viewportHr)<<std::dec
     <<" gpu_viewport="<<gpuViewport.X<<","<<gpuViewport.Y<<","<<gpuViewport.Width<<","<<gpuViewport.Height
     <<","<<gpuViewport.MinZ<<","<<gpuViewport.MaxZ<<"\n";
 const D3DTRANSFORMSTATETYPE transforms[]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION};
 const char* transformNames[]={"world","view","projection"};
 for(int i=0;i<3;++i){
  D3DMATRIX matrix{};
  const HRESULT hr=device->GetTransform(transforms[i],&matrix);
  meta<<"GetTransform_"<<transformNames[i]<<"_hr=0x"<<std::hex<<static_cast<unsigned long>(hr)<<std::dec<<"\n";
  RestoreDiagMatrix(meta,transformNames[i],matrix);
 }
 struct SamplerDiag{D3DSAMPLERSTATETYPE type;const char* name;};
 const SamplerDiag samplers[]={
  {D3DSAMP_MINFILTER,"sampler0_minfilter"},{D3DSAMP_MAGFILTER,"sampler0_magfilter"},
  {D3DSAMP_MIPFILTER,"sampler0_mipfilter"},{D3DSAMP_MAXANISOTROPY,"sampler0_maxanisotropy"}};
 for(const auto& sampler:samplers){
  DWORD value=0;const HRESULT hr=device->GetSamplerState(0,sampler.type,&value);
  meta<<sampler.name<<"="<<value<<" hr=0x"<<std::hex<<static_cast<unsigned long>(hr)<<std::dec<<"\n";
 }
 struct RenderStateDiag{D3DRENDERSTATETYPE type;const char* name;};
 const RenderStateDiag states[]={
  {D3DRS_ALPHABLENDENABLE,"alphablend"},{D3DRS_SRCBLEND,"srcblend"},{D3DRS_DESTBLEND,"destblend"},
  {D3DRS_BLENDOP,"blendop"},{D3DRS_SEPARATEALPHABLENDENABLE,"separatealphablend"},
  {D3DRS_ALPHATESTENABLE,"alphatest"},{D3DRS_DITHERENABLE,"dither"}};
 for(const auto& state:states){
  DWORD value=0;const HRESULT hr=device->GetRenderState(state.type,&value);
  meta<<state.name<<"="<<value<<" hr=0x"<<std::hex<<static_cast<unsigned long>(hr)<<std::dec<<"\n";
 }
}
// Observation only: no font replacement, no frame preparation and no device
// writes. Failures are recorded in the .meta file instead of being reported as
// a successful snapshot. Mode 1 snapshots the text VB and font atlas with the
// device state, mode 2 the state only, mode 3 the VB plus state, mode 4 the
// atlas plus state, and mode 5 the state only for the real-Frame controls,
// whose post-failure stages add no Render or PrepareFrameTextOverlay after the
// Frame calls. Even the read-only flags-0 lock needed for the managed WRITEONLY
// text buffer can dirty and re-upload it, so a snapshot is not side-effect free.
void RestoreDiagnosticSnapshot(MMDApp* app,const char* name,int mode){
 std::ofstream meta(output/(std::string(name)+".text-diag.meta"));
 meta<<"stage="<<name<<" phase="<<phase<<" mode="<<mode<<"\n";
 meta<<"lock_note=when the text VB is locked (modes 1/3), flags0 Lock keeps its bytes unchanged but may still dirty/re-upload the managed buffer; snapshot is not side-effect free\n";
 if(mode==5)meta<<"mode5_note=state-only snapshot; post-failure stages drive two real Frame calls with no extra Render or PrepareFrameTextOverlay after them\n";
 if(mode!=1&&mode!=3)meta<<"text_vb=skipped_by_mode\n";
 else{
  auto* vb=app->OverlayVertices();
  const std::uint64_t expected=std::uint64_t(app->TextOverlayPrimitiveCount())*3*28;
  meta<<"textPrimitiveCount="<<app->TextOverlayPrimitiveCount()<<" text_expected_bytes="<<expected<<"\n";
  if(vb==nullptr)meta<<"text_vb=null\n";
  else{
   D3DVERTEXBUFFER_DESC desc{};
   const HRESULT descHr=vb->GetDesc(&desc);
   meta<<"text_vb_GetDesc_hr=0x"<<std::hex<<static_cast<unsigned long>(descHr)<<std::dec
       <<" desc_size="<<desc.Size<<" desc_format="<<static_cast<unsigned long>(desc.Format)
       <<" desc_type="<<static_cast<unsigned long>(desc.Type)<<" desc_usage=0x"<<std::hex
       <<static_cast<unsigned long>(desc.Usage)<<std::dec<<" desc_pool="<<static_cast<unsigned long>(desc.Pool)
       <<" desc_fvf=0x"<<std::hex<<static_cast<unsigned long>(desc.FVF)<<std::dec<<"\n";
   if(FAILED(descHr))meta<<"text_vb_bytes=unavailable desc_failed=1\n";
   else if(expected==0)meta<<"text_vb_bytes=empty no_text_primitives=1\n";
   else if(expected>desc.Size)meta<<"text_vb_bytes=unavailable range_exceeds_desc expected="<<expected
       <<" desc_size="<<desc.Size<<"\n";
   else{
    void* data=nullptr;
    // The text batch is a managed D3DUSAGE_WRITEONLY buffer, so D3DLOCK_READONLY
    // is invalid. Lock with flags 0, copy without writing and Unlock, matching
    // the buffer backup read used by viewport_render_scale.
    const HRESULT lockHr=vb->Lock(0,static_cast<UINT>(expected),&data,0);
    meta<<"text_vb_Lock_hr=0x"<<std::hex<<static_cast<unsigned long>(lockHr)<<std::dec<<"\n";
    if(SUCCEEDED(lockHr)){
     if(data!=nullptr){
      std::ofstream bytes(output/(std::string(name)+".text-vb.bin"),std::ios::binary);
      bytes.write(static_cast<const char*>(data),static_cast<std::streamsize>(expected));
      bytes.close();
      meta<<"text_vb_bin_bytes="<<expected<<" write_ok="<<(bytes?1:0)<<"\n";
     }else meta<<"text_vb_bytes=unavailable null_pointer=1\n";
     vb->Unlock();
    }else meta<<"text_vb_bytes=unavailable lock_failed=1\n";
   }
  }
 }
 if(mode!=1&&mode!=4)meta<<"text_texture=skipped_by_mode\n";
 else{
  auto* texture=app->TextOverlayTexture();
  if(texture==nullptr)meta<<"text_texture=null\n";
  else{
   D3DSURFACE_DESC desc{};
   const HRESULT descHr=texture->GetLevelDesc(0,&desc);
   meta<<"text_texture_GetLevelDesc_hr=0x"<<std::hex<<static_cast<unsigned long>(descHr)<<std::dec
       <<" width="<<desc.Width<<" height="<<desc.Height
       <<" format="<<static_cast<unsigned long>(desc.Format)
       <<" usage=0x"<<std::hex<<static_cast<unsigned long>(desc.Usage)<<std::dec
       <<" pool="<<static_cast<unsigned long>(desc.Pool)<<"\n";
   if(FAILED(descHr))meta<<"text_texture_bytes=unavailable desc_failed=1\n";
   else if(desc.Format!=D3DFMT_A8R8G8B8)meta<<"text_texture_bytes=unsupported format="
       <<static_cast<unsigned long>(desc.Format)<<" expected_format="<<D3DFMT_A8R8G8B8<<"\n";
   else{
    D3DLOCKED_RECT locked{};
    const HRESULT lockHr=texture->LockRect(0,&locked,nullptr,D3DLOCK_READONLY);
    meta<<"text_texture_LockRect_hr=0x"<<std::hex<<static_cast<unsigned long>(lockHr)<<std::dec
        <<" pitch="<<locked.Pitch<<"\n";
    if(SUCCEEDED(lockHr)){
     if(locked.pBits!=nullptr){
      const std::size_t rowBytes=std::size_t(desc.Width)*4;
      std::ofstream bytes(output/(std::string(name)+".text-atlas.bin"),std::ios::binary);
      for(UINT row=0;row<desc.Height&&bytes;++row)
       bytes.write(static_cast<const char*>(locked.pBits)+std::size_t(row)*locked.Pitch,
           static_cast<std::streamsize>(rowBytes));
      bytes.close();
      meta<<"text_texture_bin_rows="<<desc.Height<<" row_bytes="<<rowBytes<<" pitch="<<locked.Pitch
          <<" write_ok="<<(bytes?1:0)<<" expected_format=A8R8G8B8 byte_order=BGRA\n";
     }else meta<<"text_texture_bytes=unavailable null_pointer=1\n";
     texture->UnlockRect(0);
    }else meta<<"text_texture_bytes=unavailable lock_failed=1\n";
   }
  }
 }
 RestoreDiagDeviceState(meta,app);
 meta.close();
 if(hudEndpoint!=HudEndpoint::off)Check(bool(meta),"EXPORT-HUD-ENDPOINT writes historical state metadata");
}
std::vector<DWORD> Pixels(MMDApp*a,const char*name){
 const bool endpoint=HudEndpointAt(name);
 if(endpoint){
  Need(hudWatchOpen&&!hudReadDone,"EXPORT-HUD-ENDPOINT reads its selected frame exactly once");
  // RenderFrameScene has returned through EndScene and state restoration.
  // The original resolve/readback below is outside the post-text write guard.
  hudWatchOpen=false;hudReadOpen=true;
 }
 auto*d=a->Renderer()->device;ComPtr<IDirect3DSurface9>b,resolved,cpu;
 Need(SUCCEEDED(HudReadHr("GetBackBuffer",d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&b))),"capture backbuffer");
 D3DSURFACE_DESC desc{};const HRESULT descHr=HudReadHr("back.GetDesc",b->GetDesc(&desc));auto*src=b.Get();
 if(endpoint){
  std::printf("EXPORT-HUD-ENDPOINT readback width=%u height=%u format=%u msaa=%u quality=%lu same_target=%d\n",
      desc.Width,desc.Height,unsigned(desc.Format),unsigned(desc.MultiSampleType),
      static_cast<unsigned long>(desc.MultiSampleQuality),b.Get()==hudExpectedBack?1:0);
  Need(b.Get()==hudExpectedBack&&SUCCEEDED(descHr)&&desc.Width&&desc.Height&&
      (desc.Format==D3DFMT_A8R8G8B8||desc.Format==D3DFMT_X8R8G8B8)&&
      std::uint64_t(desc.Width)*desc.Height<=std::numeric_limits<size_t>::max()/4,
      "EXPORT-HUD-ENDPOINT has a bounded 32-bit backbuffer format");
  hudBackDesc=desc;hudRaw.resize(size_t(desc.Width)*desc.Height);
 }
 if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE){Need(SUCCEEDED(HudReadHr("CreateRenderTarget(resolve)",d->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr))),"resolve allocation");Need(SUCCEEDED(HudReadHr("StretchRect(resolve)",d->StretchRect(src,nullptr,resolved.Get(),nullptr,D3DTEXF_NONE))),"resolve capture");src=resolved.Get();}
 Need(SUCCEEDED(HudReadHr("CreateOffscreenPlainSurface",d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr))),"capture allocation");
 Need(SUCCEEDED(HudReadHr("GetRenderTargetData",d->GetRenderTargetData(src,cpu.Get()))),"GPU readback");D3DLOCKED_RECT lock{};Need(SUCCEEDED(HudReadHr("cpu.LockRect",cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY))),"read pixels");
 if(endpoint){
  hudBackPitch=lock.Pitch;
  std::printf("EXPORT-HUD-ENDPOINT readback pitch=%d row_bytes=%llu pointer_valid=%d\n",
      lock.Pitch,static_cast<unsigned long long>(desc.Width)*4,lock.pBits?1:0);
  const bool valid=lock.pBits&&lock.Pitch>0&&std::uint64_t(lock.Pitch)>=std::uint64_t(desc.Width)*4&&
      std::uint64_t(lock.Pitch)*desc.Height<=std::numeric_limits<size_t>::max();
  if(!valid)HudReadHr("cpu.UnlockRect(invalid_pitch)",cpu->UnlockRect());
  Need(valid,"EXPORT-HUD-ENDPOINT readback pointer and pitch cover all rows");
  for(UINT y=0;y<desc.Height;++y)
   std::memcpy(hudRaw.data()+size_t(y)*desc.Width,static_cast<const char*>(lock.pBits)+size_t(y)*lock.Pitch,size_t(desc.Width)*4);
 }
 std::vector<DWORD> pixels(size_t(desc.Width)*desc.Height);
 for(UINT y=0;y<desc.Height;++y){auto*row=(DWORD*)((char*)lock.pBits+y*lock.Pitch);for(UINT x=0;x<desc.Width;++x)pixels[size_t(y)*desc.Width+x]=row[x]&0xffffff;}
 const HRESULT unlockHr=HudReadHr("cpu.UnlockRect",cpu->UnlockRect());
 if(endpoint)Need(SUCCEEDED(unlockHr),"EXPORT-HUD-ENDPOINT unlocks its original readback");
 std::ofstream f(output/(std::string(name)+".bgra"),std::ios::binary);f.write((const char*)pixels.data(),pixels.size()*4);
 if(hudEndpoint!=HudEndpoint::off){f.close();Check(bool(f),"EXPORT-HUD-ENDPOINT writes historical RGB pixels");}
 {std::ofstream dimensions(output/(std::string(name)+".txt"));dimensions<<desc.Width<<" "<<desc.Height;
  if(hudEndpoint!=HudEndpoint::off){dimensions.close();Check(bool(dimensions),"EXPORT-HUD-ENDPOINT writes historical dimensions");}}
 const int diagMode=RestoreDiagnosticMode();
 if(diagMode)RestoreDiagnosticSnapshot(a,name,diagMode);
 if(endpoint){hudReadOpen=false;hudReadDone=true;}
 if(hudEndpoint!=HudEndpoint::off&&!endpoint&&std::strcmp(name,"after-layout")==0)
  hudPriorLayoutCaptured=true;
 return pixels;
}
// PROBE_RESTORE_DRAW_STATE=1 records the real IDirect3DDevice9::DrawPrimitive
// calls the production HUD issues inside the ordinary-export stages named
// below. The existing PROBE_RESTORE_DIAGNOSTIC snapshots read device state
// after a Render has returned, which cannot show the state a draw actually
// used. This probe records draw-time state into a fixed number of preallocated
// CPU records and writes them only after the existing Pixels GPU readback. One
// extra MinHook is installed before the first observed stage and removed
// before ShutdownCleanup. Unset, empty or any value other than "1" keeps the
// ordinary path unchanged: no hook, no added device call, no added output. The
// probe never samples during an export, adds no Render/Frame/Prepare/Sleep or
// event wait, and changes no pixel comparison, existing assertion, stage order
// or exit code. Capacity overflow, failed queries and a missing or misbound
// production text draw are reported through Check so evidence cannot vanish
// silently.
int RestoreDrawStateMode(){
 const char* value=std::getenv("PROBE_RESTORE_DRAW_STATE");
 return value!=nullptr&&std::strcmp(value,"1")==0?1:0;
}
// DrawTextOverlay submits XYZRHW|DIFFUSE|TEX1 vertices with a 28-byte stride.
constexpr DWORD kDrawStateTextFvf=0x144;
constexpr UINT kDrawStateTextStride=28;
// IDirect3DDevice9 vtable order from the Windows SDK d3d9.h: IUnknown 0..2,
// Reset 16 and SetViewport 47 (both already relied on above), DrawPrimitive 81.
constexpr unsigned kDrawStateDrawPrimitiveSlot=81;
// Static storage is the fixed preallocation; the hook never allocates.
constexpr unsigned kDrawStateCapacity=1024;
static_assert(std::is_invocable_r_v<HRESULT,decltype(&IDirect3DDevice9::DrawPrimitive),
    IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT>,
    "draw-state observer must be invocable with the IDirect3DDevice9::DrawPrimitive signature");
using DrawStateDrawPrimitiveProc=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
constexpr unsigned kDrawStateSamplerCount=10;
const D3DSAMPLERSTATETYPE kDrawStateSamplerTypes[kDrawStateSamplerCount]={
 D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,
 D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_ADDRESSW,
 D3DSAMP_SRGBTEXTURE,D3DSAMP_MIPMAPLODBIAS,D3DSAMP_MAXANISOTROPY,
 D3DSAMP_MAXMIPLEVEL};
const char* const kDrawStateSamplerNames[kDrawStateSamplerCount]={
 "minfilter","magfilter","mipfilter","addressu","addressv","addressw",
 "srgbtexture","mipmaplodbias","maxanisotropy","maxmipLevel"};
constexpr unsigned kDrawStateStageStateCount=9;
const D3DTEXTURESTAGESTATETYPE kDrawStateStageStates[kDrawStateStageStateCount]={
 D3DTSS_COLOROP,D3DTSS_COLORARG1,D3DTSS_COLORARG2,
 D3DTSS_ALPHAOP,D3DTSS_ALPHAARG1,D3DTSS_ALPHAARG2,
 D3DTSS_RESULTARG,D3DTSS_TEXCOORDINDEX,D3DTSS_TEXTURETRANSFORMFLAGS};
const char* const kDrawStateStageStateNames[kDrawStateStageStateCount]={
 "colorop","colorarg1","colorarg2","alphaop","alphaarg1","alphaarg2",
 "resultarg","texcoordindex","texturetransformflags"};
constexpr unsigned kDrawStateRenderStateCount=30;
const D3DRENDERSTATETYPE kDrawStateRenderStates[kDrawStateRenderStateCount]={
 D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,
 D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,
 D3DRS_BLENDOPALPHA,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,
 D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_STENCILENABLE,
 D3DRS_STENCILFUNC,D3DRS_STENCILREF,D3DRS_STENCILMASK,D3DRS_STENCILWRITEMASK,
 D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_CULLMODE,D3DRS_FILLMODE,
 D3DRS_COLORWRITEENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,
 D3DRS_MULTISAMPLEMASK,D3DRS_MULTISAMPLEANTIALIAS,D3DRS_CLIPPLANEENABLE,
 D3DRS_DITHERENABLE};
const char* const kDrawStateRenderStateNames[kDrawStateRenderStateCount]={
 "alphablendenable","srcblend","destblend","blendop","separatealphablend",
 "srcblendalpha","destblendalpha","blendopalpha","alphatestenable","alphafunc",
 "alpharef","zenable","zwriteenable","zfunc","stencilenable","stencilfunc",
 "stencilref","stencilmask","stencilwritemask","fogenable","lighting",
 "cullmode","fillmode","colorwriteenable","scissortestenable","srgbwriteenable",
 "multisamplemask","multisampleantialias","clipplaneenable","ditherenable"};
// Trivial type: the fixed array below is zero-initialized in BSS, so an unset
// probe adds no startup initialization, and the hook zeroes one record per draw.
struct DrawStateRecord{
 const char* stage;unsigned sequence;
 D3DPRIMITIVETYPE primitiveType;UINT startVertex,primitiveCount;
 HRESULT drawHr;
 HRESULT streamHr;IDirect3DVertexBuffer9* stream;UINT streamOffset,streamStride;
 HRESULT streamFreqHr;UINT streamFreq;
 HRESULT fvfHr;DWORD fvf;
 HRESULT declarationHr;IDirect3DVertexDeclaration9* declaration;
 HRESULT vertexShaderHr;IDirect3DVertexShader9* vertexShader;
 HRESULT pixelShaderHr;IDirect3DPixelShader9* pixelShader;
 HRESULT textureHr;IDirect3DBaseTexture9* texture;
 HRESULT renderTargetHr;IDirect3DSurface9* renderTarget;
 // A null depth surface with its recorded HRESULT is legal for the HUD pass;
 // this probe never asserts that this query returns S_OK.
 HRESULT depthSurfaceHr;IDirect3DSurface9* depthSurface;
 HRESULT viewportHr;D3DVIEWPORT9 viewport;
 HRESULT scissorHr;RECT scissor;
 HRESULT samplerHr[kDrawStateSamplerCount];DWORD samplerValue[kDrawStateSamplerCount];
 HRESULT stage0Hr[kDrawStateStageStateCount];DWORD stage0Value[kDrawStateStageStateCount];
 HRESULT stage1Hr[kDrawStateStageStateCount];DWORD stage1Value[kDrawStateStageStateCount];
 HRESULT renderStateHr[kDrawStateRenderStateCount];DWORD renderStateValue[kDrawStateRenderStateCount];
};
DrawStateRecord g_drawStateRecords[kDrawStateCapacity];
const char* g_drawStateStage=nullptr;
IDirect3DDevice9* g_drawStateDevice=nullptr;
void* g_drawStateHookTarget=nullptr;
IDirect3DVertexBuffer9* g_drawStateExpectedVb=nullptr;
UINT g_drawStateExpectedPrimitives=0;
IDirect3DBaseTexture9* g_drawStateExpectedTexture=nullptr;
unsigned g_drawStateCount=0,g_drawStateOverflow=0;
bool g_drawStateOpen=false,g_drawStateReady=false,g_drawStateAttempted=false,g_drawStateInstalled=false;
DrawStateDrawPrimitiveProc g_drawStateRealDrawPrimitive=nullptr;
void DrawStateCapture(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,UINT start,UINT primitives,unsigned index){
 DrawStateRecord& record=g_drawStateRecords[index];
 record=DrawStateRecord{};
 record.stage=g_drawStateStage;record.sequence=index+1;
 record.primitiveType=type;record.startVertex=start;record.primitiveCount=primitives;
 // All resource queries AddRef; the reference is released immediately and no
 // surface is retained, locked or used to block Reset.
 IDirect3DVertexBuffer9* stream=nullptr;
 record.streamHr=device->GetStreamSource(0,&stream,&record.streamOffset,&record.streamStride);
 record.stream=stream;if(stream!=nullptr)stream->Release();
 record.streamFreqHr=device->GetStreamSourceFreq(0,&record.streamFreq);
 record.fvfHr=device->GetFVF(&record.fvf);
 IDirect3DVertexDeclaration9* declaration=nullptr;
 record.declarationHr=device->GetVertexDeclaration(&declaration);
 record.declaration=declaration;if(declaration!=nullptr)declaration->Release();
 IDirect3DVertexShader9* vertexShader=nullptr;
 record.vertexShaderHr=device->GetVertexShader(&vertexShader);
 record.vertexShader=vertexShader;if(vertexShader!=nullptr)vertexShader->Release();
 IDirect3DPixelShader9* pixelShader=nullptr;
 record.pixelShaderHr=device->GetPixelShader(&pixelShader);
 record.pixelShader=pixelShader;if(pixelShader!=nullptr)pixelShader->Release();
 IDirect3DBaseTexture9* texture=nullptr;
 record.textureHr=device->GetTexture(0,&texture);
 record.texture=texture;if(texture!=nullptr)texture->Release();
 IDirect3DSurface9* renderTarget=nullptr;
 record.renderTargetHr=device->GetRenderTarget(0,&renderTarget);
 record.renderTarget=renderTarget;if(renderTarget!=nullptr)renderTarget->Release();
 IDirect3DSurface9* depthSurface=nullptr;
 record.depthSurfaceHr=device->GetDepthStencilSurface(&depthSurface);
 record.depthSurface=depthSurface;if(depthSurface!=nullptr)depthSurface->Release();
 record.viewportHr=device->GetViewport(&record.viewport);
 record.scissorHr=device->GetScissorRect(&record.scissor);
 for(unsigned i=0;i<kDrawStateSamplerCount;++i)
  record.samplerHr[i]=device->GetSamplerState(0,kDrawStateSamplerTypes[i],&record.samplerValue[i]);
 for(unsigned i=0;i<kDrawStateStageStateCount;++i){
  record.stage0Hr[i]=device->GetTextureStageState(0,kDrawStateStageStates[i],&record.stage0Value[i]);
  record.stage1Hr[i]=device->GetTextureStageState(1,kDrawStateStageStates[i],&record.stage1Value[i]);
 }
 for(unsigned i=0;i<kDrawStateRenderStateCount;++i)
  record.renderStateHr[i]=device->GetRenderState(kDrawStateRenderStates[i],&record.renderStateValue[i]);
}
unsigned DrawStateQueryFailures(const DrawStateRecord& record){
 unsigned failed=0;
 const HRESULT queries[]={record.streamHr,record.streamFreqHr,record.fvfHr,record.declarationHr,
  record.vertexShaderHr,record.pixelShaderHr,record.textureHr,record.renderTargetHr,
  record.viewportHr,record.scissorHr};
 for(HRESULT query:queries)failed+=FAILED(query)?1u:0u;
 for(unsigned i=0;i<kDrawStateSamplerCount;++i)failed+=FAILED(record.samplerHr[i])?1u:0u;
 for(unsigned i=0;i<kDrawStateStageStateCount;++i)
  failed+=(FAILED(record.stage0Hr[i])?1u:0u)+(FAILED(record.stage1Hr[i])?1u:0u);
 for(unsigned i=0;i<kDrawStateRenderStateCount;++i)failed+=FAILED(record.renderStateHr[i])?1u:0u;
 return failed;
}
HRESULT WINAPI DrawStateObserveDrawPrimitive(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,UINT start,UINT primitives){
 if(g_drawStateOpen&&g_drawStateReady&&device==g_drawStateDevice){
  if(g_drawStateCount<kDrawStateCapacity){
   const unsigned index=g_drawStateCount++;
   DrawStateCapture(device,type,start,primitives,index);
   const HRESULT hr=g_drawStateRealDrawPrimitive(device,type,start,primitives);
   g_drawStateRecords[index].drawHr=hr;
   return hr;
  }
  ++g_drawStateOverflow;
 }
 return g_drawStateRealDrawPrimitive(device,type,start,primitives);
}
bool DrawStateInstall(MMDApp* app){
 auto* renderer=app->Renderer();
 IDirect3DDevice9* device=renderer!=nullptr?renderer->device:nullptr;
 if(device==nullptr){Check(false,"draw-state observation finds the production device");return false;}
 ComPtr<IDirect3DSurface9> back;ComPtr<IDirect3DDevice9> native;
 HRESULT hr=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
 if(FAILED(hr)||back==nullptr){
  Check(false,"draw-state observation reads the production back buffer");
  std::printf("DRAW_STATE backbuffer_hr=0x%08lx\n",static_cast<unsigned long>(hr));return false;
 }
 hr=back->GetDevice(&native);
 if(FAILED(hr)||native==nullptr){
  Check(false,"draw-state observation resolves the native device");
  std::printf("DRAW_STATE getdevice_hr=0x%08lx\n",static_cast<unsigned long>(hr));return false;
 }
 auto** slots=*(void***)native.Get();
 void* target=slots!=nullptr?slots[kDrawStateDrawPrimitiveSlot]:nullptr;
 if(target==nullptr){Check(false,"draw-state observation resolves the DrawPrimitive slot");return false;}
 const MH_STATUS created=MH_CreateHook(target,reinterpret_cast<void*>(&DrawStateObserveDrawPrimitive),
     reinterpret_cast<void**>(&g_drawStateRealDrawPrimitive));
 if(created!=MH_OK){
  Check(false,"draw-state observation creates its DrawPrimitive hook");
  std::printf("DRAW_STATE hook_create_status=%d target=%p\n",static_cast<int>(created),target);return false;
 }
 const MH_STATUS enabled=MH_EnableHook(target);
 if(enabled!=MH_OK){
  MH_RemoveHook(target);
  Check(false,"draw-state observation enables its DrawPrimitive hook");
  std::printf("DRAW_STATE hook_enable_status=%d target=%p\n",static_cast<int>(enabled),target);return false;
 }
 g_drawStateHookTarget=target;g_drawStateDevice=native.Get();
 g_drawStateReady=true;g_drawStateInstalled=true;
 std::printf("DRAW_STATE hook=installed target=%p device=%p slot=%u capacity=%u\n",
     target,static_cast<void*>(native.Get()),kDrawStateDrawPrimitiveSlot,kDrawStateCapacity);
 return true;
}
bool BeginDrawStateObservation(MMDApp* app,const char* stage){
 if(!RestoreDrawStateMode())return false;
 if(!g_drawStateAttempted){
  g_drawStateAttempted=true;
  if(!DrawStateInstall(app))return false;
 }else if(!g_drawStateReady)return false;
 if(g_drawStateOpen){Check(false,"draw-state observation does not nest stages");return false;}
 g_drawStateStage=stage;
 // The expected production text batch comes from the same app state
 // DrawTextOverlay reads; it is not derived from the observed records.
 g_drawStateExpectedVb=app->OverlayVertices();
 g_drawStateExpectedPrimitives=static_cast<UINT>(app->TextOverlayPrimitiveCount());
 g_drawStateExpectedTexture=static_cast<IDirect3DBaseTexture9*>(app->TextOverlayTexture());
 g_drawStateCount=0;g_drawStateOverflow=0;g_drawStateOpen=true;
 return true;
}
void DrawStateHr(std::ofstream& file,HRESULT hr){
 file<<"hr=0x"<<std::hex<<static_cast<unsigned long>(hr)<<std::dec;
}
void DrawStateWriteRecord(std::ofstream& file,const DrawStateRecord& record){
 file<<"draw="<<record.sequence<<" stage="<<(record.stage!=nullptr?record.stage:"?")
     <<" primitive_type="<<static_cast<unsigned long>(record.primitiveType)
     <<" start_vertex="<<record.startVertex<<" primitive_count="<<record.primitiveCount
     <<" draw_hr=0x"<<std::hex<<static_cast<unsigned long>(record.drawHr)<<std::dec
     <<" query_failures="<<DrawStateQueryFailures(record)<<"\n";
 file<<" stream0 vb="<<static_cast<const void*>(record.stream)<<" offset="<<record.streamOffset
     <<" stride="<<record.streamStride<<" ";DrawStateHr(file,record.streamHr);file<<"\n";
 file<<" stream0_freq value="<<record.streamFreq<<" ";DrawStateHr(file,record.streamFreqHr);file<<"\n";
 file<<" fvf value=0x"<<std::hex<<static_cast<unsigned long>(record.fvf)<<std::dec<<" ";
 DrawStateHr(file,record.fvfHr);file<<"\n";
 file<<" declaration value="<<static_cast<const void*>(record.declaration)<<" ";
 DrawStateHr(file,record.declarationHr);file<<"\n";
 file<<" vertex_shader value="<<static_cast<const void*>(record.vertexShader)<<" ";
 DrawStateHr(file,record.vertexShaderHr);file<<"\n";
 file<<" pixel_shader value="<<static_cast<const void*>(record.pixelShader)<<" ";
 DrawStateHr(file,record.pixelShaderHr);file<<"\n";
 file<<" texture0 value="<<static_cast<const void*>(record.texture)<<" ";
 DrawStateHr(file,record.textureHr);file<<"\n";
 file<<" render_target0 value="<<static_cast<const void*>(record.renderTarget)<<" ";
 DrawStateHr(file,record.renderTargetHr);file<<"\n";
 file<<" depth_surface value="<<static_cast<const void*>(record.depthSurface)<<" ";
 DrawStateHr(file,record.depthSurfaceHr);file<<"\n";
 file<<" viewport x="<<record.viewport.X<<" y="<<record.viewport.Y<<" width="<<record.viewport.Width
     <<" height="<<record.viewport.Height<<" minz="<<record.viewport.MinZ<<" maxz="<<record.viewport.MaxZ<<" ";
 DrawStateHr(file,record.viewportHr);file<<"\n";
 file<<" scissor left="<<record.scissor.left<<" top="<<record.scissor.top
     <<" right="<<record.scissor.right<<" bottom="<<record.scissor.bottom<<" ";
 DrawStateHr(file,record.scissorHr);file<<"\n";
 file<<" sampler0";
 for(unsigned i=0;i<kDrawStateSamplerCount;++i)
  file<<" "<<kDrawStateSamplerNames[i]<<"="<<record.samplerValue[i]<<" hr=0x"<<std::hex
      <<static_cast<unsigned long>(record.samplerHr[i])<<std::dec;
 file<<"\n";
 for(int stage=0;stage<2;++stage){
  file<<" texture_stage"<<stage;
  for(unsigned i=0;i<kDrawStateStageStateCount;++i){
   const DWORD value=stage==0?record.stage0Value[i]:record.stage1Value[i];
   const HRESULT hr=stage==0?record.stage0Hr[i]:record.stage1Hr[i];
   file<<" "<<kDrawStateStageStateNames[i]<<"="<<value<<" hr=0x"<<std::hex
       <<static_cast<unsigned long>(hr)<<std::dec;
  }
  file<<"\n";
 }
 file<<" render_state";
 for(unsigned i=0;i<kDrawStateRenderStateCount;++i)
  file<<" "<<kDrawStateRenderStateNames[i]<<"="<<record.renderStateValue[i]<<" hr=0x"<<std::hex
      <<static_cast<unsigned long>(record.renderStateHr[i])<<std::dec;
 file<<"\n";
}
void EndDrawStateObservation(const char* stage){
 if(!g_drawStateOpen)return;
 const char* recordedStage=g_drawStateStage;
 if(recordedStage==nullptr||stage==nullptr||std::strcmp(recordedStage,stage)!=0)
  Check(false,"draw-state observation closes the stage it opened");
 g_drawStateOpen=false;
 const std::string fileStage=stage!=nullptr?std::string(stage):std::string("?");
 const unsigned records=g_drawStateCount;
 const unsigned overflow=g_drawStateOverflow;
 std::ofstream file(output/(fileStage+".draw-state.txt"));
 if(!file)Check(false,"draw-state observation writes its record file");
 else{
  file<<"stage="<<(recordedStage!=nullptr?recordedStage:"?")<<" phase="<<phase
      <<" draw_state=1 capacity="<<kDrawStateCapacity<<" records="<<records
      <<" overflow="<<overflow<<"\n";
  file<<"target_device="<<static_cast<const void*>(g_drawStateDevice)
      <<" vtable_slot="<<kDrawStateDrawPrimitiveSlot<<"\n";
  file<<"expect_text_vb="<<static_cast<const void*>(g_drawStateExpectedVb)
      <<" expect_text_primitive_count="<<g_drawStateExpectedPrimitives
      <<" expect_text_texture="<<static_cast<const void*>(g_drawStateExpectedTexture)
      <<" expect_text_fvf=0x"<<std::hex<<kDrawStateTextFvf<<std::dec
      <<" expect_text_stride="<<kDrawStateTextStride<<"\n";
  for(unsigned i=0;i<records;++i)DrawStateWriteRecord(file,g_drawStateRecords[i]);
  file.close();
 }
 unsigned queryFailures=0,drawFailures=0,textVbDraws=0,textMatches=0,textMismatches=0,textAtlasWithoutVb=0;
 for(unsigned i=0;i<records;++i){
  const DrawStateRecord& record=g_drawStateRecords[i];
  queryFailures+=DrawStateQueryFailures(record);
  drawFailures+=FAILED(record.drawHr)?1u:0u;
  const bool usesExpectedVb=record.stream==g_drawStateExpectedVb;
  if(usesExpectedVb){
   ++textVbDraws;
   const bool matches=record.primitiveType==D3DPT_TRIANGLELIST&&record.startVertex==0&&
       record.primitiveCount==g_drawStateExpectedPrimitives&&record.streamOffset==0&&
       record.streamStride==kDrawStateTextStride&&record.fvf==kDrawStateTextFvf&&
       record.texture==g_drawStateExpectedTexture&&record.vertexShader==nullptr&&
       record.pixelShader==nullptr;
   if(matches)++textMatches;
   else{
    ++textMismatches;
    std::printf("DRAW_STATE text_binding_mismatch stage=%s draw=%u type=%lu start=%u primitives=%u "
        "expected_start=0 expected_primitives=%u offset=%u stride=%u fvf=0x%lx texture=%p "
        "expected_texture=%p vs=%p ps=%p\n",
        recordedStage!=nullptr?recordedStage:"?",record.sequence,
        static_cast<unsigned long>(record.primitiveType),record.startVertex,record.primitiveCount,
        g_drawStateExpectedPrimitives,record.streamOffset,record.streamStride,
        static_cast<unsigned long>(record.fvf),static_cast<void*>(record.texture),
        static_cast<void*>(g_drawStateExpectedTexture),
        static_cast<void*>(record.vertexShader),static_cast<void*>(record.pixelShader));
   }
  }
  const bool textLike=record.primitiveType==D3DPT_TRIANGLELIST&&record.startVertex==0&&
      record.fvf==kDrawStateTextFvf&&record.streamStride==kDrawStateTextStride;
  if(textLike&&g_drawStateExpectedTexture!=nullptr&&record.texture==g_drawStateExpectedTexture&&
      !usesExpectedVb){
   ++textAtlasWithoutVb;
   std::printf("DRAW_STATE text_atlas_without_text_vb stage=%s draw=%u vb=%p expected_vb=%p\n",
       recordedStage!=nullptr?recordedStage:"?",record.sequence,
       static_cast<void*>(record.stream),static_cast<void*>(g_drawStateExpectedVb));
  }
 }
 std::printf("DRAW_STATE stage=%s phase=%s records=%u overflow=%u query_failures=%u "
     "draw_failures=%u text_vb_draws=%u text_matches=%u text_mismatches=%u "
     "text_atlas_without_vb=%u expect_vb=%p expect_primitives=%u expect_texture=%p "
     "capacity=%u file=%s\n",
     recordedStage!=nullptr?recordedStage:"?",phase,records,overflow,queryFailures,drawFailures,
     textVbDraws,textMatches,textMismatches,textAtlasWithoutVb,
     static_cast<void*>(g_drawStateExpectedVb),g_drawStateExpectedPrimitives,
     static_cast<void*>(g_drawStateExpectedTexture),kDrawStateCapacity,
     (fileStage+".draw-state.txt").c_str());
 const bool haveExpectation=g_drawStateExpectedVb!=nullptr&&g_drawStateExpectedPrimitives!=0;
 Check(overflow==0,"draw-state fixed record capacity covers every observed stage");
 Check(queryFailures==0,"draw-state state queries succeed");
 Check(drawFailures==0,"draw-state observed DrawPrimitive calls succeed");
 Check(haveExpectation,"draw-state production text batch expectation is drawable");
 Check(textVbDraws>0,"draw-state observed stage submits the production text vertex buffer");
 Check(textMatches==1,"draw-state observed stage submits exactly one matching text draw");
 Check(textMismatches==0,"draw-state text draw bindings match the production batch");
 Check(textAtlasWithoutVb==0,"draw-state font atlas is only drawn with the production text vertex buffer");
 g_drawStateStage=nullptr;g_drawStateExpectedVb=nullptr;
 g_drawStateExpectedPrimitives=0;g_drawStateExpectedTexture=nullptr;
 g_drawStateCount=0;g_drawStateOverflow=0;
}
void RemoveDrawStateHook(){
 if(!g_drawStateInstalled)return;
 g_drawStateOpen=false;
 const MH_STATUS disabled=MH_DisableHook(g_drawStateHookTarget);
 const MH_STATUS removed=disabled==MH_OK?MH_RemoveHook(g_drawStateHookTarget):MH_ERROR_NOT_CREATED;
 Check(disabled==MH_OK&&removed==MH_OK,"draw-state observation removes its DrawPrimitive hook");
 std::printf("DRAW_STATE hook=removed disable_status=%d remove_status=%d target=%p\n",
     static_cast<int>(disabled),static_cast<int>(removed),g_drawStateHookTarget);
 if(removed==MH_OK){
  g_drawStateInstalled=false;g_drawStateReady=false;
  g_drawStateRealDrawPrimitive=nullptr;g_drawStateDevice=nullptr;g_drawStateHookTarget=nullptr;
 }
}

// Endpoint-only hooks use SDK signatures and are installed at the chosen
// Render only. Conservatively reject ANY later draw/clear/transfer, including
// another target, indexed/UP/patch draws and copies. Never intercept restoration.
struct HudOperation { unsigned slot;HRESULT hr;bool afterText,skipped; };
HudOperation hudOperations[4096]{};
unsigned hudOperationCount=0,hudOverflow=0,hudDrawCount=0,hudMatches=0,hudSkipped=0;
unsigned hudLateWrites=0,hudEndScenes=0,hudBindingErrors=0,hudResetsBefore=0;
ComPtr<IDirect3DDevice9> hudDevice;
ComPtr<IDirect3DSurface9> hudBack;
ComPtr<IDirect3DVertexBuffer9> hudVb;
ComPtr<IDirect3DTexture9> hudAtlas;
UINT hudPrimitives=0;
DrawStateRecord hudTextDraw{};
void* hudHooks[16]{};
unsigned hudHookCount=0;
HRESULT hudBackHr=E_FAIL,hudDeviceHr=E_FAIL,hudUniformClearHr=E_PENDING;
bool HudWatching(IDirect3DDevice9* device){return hudWatchOpen&&device==hudDevice.Get();}
unsigned HudBeginOperation(unsigned slot){
 const bool after=hudMatches!=0;
 if(after&&slot!=41&&slot!=42)++hudLateWrites;
 if(after&&slot==42)++hudEndScenes;
 if(hudOperationCount==4096){++hudOverflow;return 4096;}
 const unsigned index=hudOperationCount++;
 hudOperations[index]={slot,E_PENDING,after,false};return index;
}
void HudEndOperation(unsigned index,HRESULT hr,bool skipped=false){
 if(index<4096){hudOperations[index].hr=hr;hudOperations[index].skipped=skipped;}
}
template<unsigned Slot,class... Args>struct HudTap {
 using Proc=HRESULT(WINAPI*)(IDirect3DDevice9*,Args...);
 static inline Proc real=nullptr;
 static HRESULT WINAPI Call(IDirect3DDevice9* device,Args... args){
  if(!HudWatching(device))return real(device,args...);
  const unsigned index=HudBeginOperation(Slot);
  const HRESULT hr=real(device,args...);
  HudEndOperation(index,hr);return hr;
 }
};
DrawStateDrawPrimitiveProc hudRealDraw=nullptr;
HRESULT WINAPI HudDraw(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,UINT start,UINT primitives){
 if(!HudWatching(device))return hudRealDraw(device,type,start,primitives);
 bool match=false,skip=false;
 unsigned index=kDrawStateCapacity;
 if(hudDrawCount<kDrawStateCapacity){
  index=hudDrawCount++;
  DrawStateCapture(device,type,start,primitives,index);
  const auto& record=g_drawStateRecords[index];
  match=DrawStateQueryFailures(record)==0&&
      record.stream==hudVb.Get()&&record.texture==hudAtlas.Get()&&
      record.primitiveType==D3DPT_TRIANGLELIST&&record.startVertex==0&&
      record.primitiveCount==hudPrimitives&&record.streamOffset==0&&
      record.streamStride==kDrawStateTextStride&&record.streamFreq==1&&
      record.fvf==kDrawStateTextFvf&&record.vertexShader==nullptr&&
      record.pixelShader==nullptr&&record.renderTarget==hudBack.Get();
  if(match&&hudMatches==0){
   if(hudUniformBackground){
    hudUniformClearHr=D3D_OK;
    for(LONG y=kHudUniformRect.y1;y<kHudUniformRect.y2&&SUCCEEDED(hudUniformClearHr);++y){
     const D3DRECT pixel{kHudUniformRect.x1,y,kHudUniformRect.x2,y+1};
     hudUniformClearHr=device->Clear(1,&pixel,D3DCLEAR_TARGET,
         kHudUniformColors[y-kHudUniformRect.y1],1.f,0);
    }
   }
   hudTextDraw=record;
   skip=HudEndpointSkip();
   if(skip)++hudSkipped;
  }else if(!match&&(record.stream==hudVb.Get()||record.texture==hudAtlas.Get()))++hudBindingErrors;
 }else ++hudOverflow;
 const unsigned operation=HudBeginOperation(kDrawStateDrawPrimitiveSlot);
 if(match)++hudMatches;
 // A second match invalidates the entire sample. Only the first exact match
 // can be omitted; all nonmatching calls, EndScene and restoration forward.
 const HRESULT hr=skip?D3D_OK:hudRealDraw(device,type,start,primitives);
 if(index<kDrawStateCapacity)g_drawStateRecords[index].drawHr=hr;
 if(hudMatches==1&&index<kDrawStateCapacity&&hudTextDraw.sequence==index+1)hudTextDraw.drawHr=hr;
 HudEndOperation(operation,hr,skip);return hr;
}
bool RemoveHudHooks(){
 hudWatchOpen=false;bool ok=true;
 for(unsigned i=hudHookCount;i>0;--i){
  const MH_STATUS disabled=MH_DisableHook(hudHooks[i-1]);
  const bool disabledOk=disabled==MH_OK||disabled==MH_ERROR_DISABLED;
  const MH_STATUS removed=disabledOk?MH_RemoveHook(hudHooks[i-1]):MH_ERROR_ENABLED;
  std::printf("EXPORT-HUD-ENDPOINT hook_remove=%u disable=%d remove=%d\n",i,int(disabled),int(removed));
  ok=ok&&disabledOk&&removed==MH_OK;
 }
 hudHookCount=0;return ok;
}
void InstallHudHook(unsigned slot,void* replacement,void** real){
 Need(hudHookCount<16,"EXPORT-HUD-ENDPOINT hook ledger has room");
 void* target=(*reinterpret_cast<void***>(hudDevice.Get()))[slot];
 const MH_STATUS created=MH_CreateHook(target,replacement,real);
 const MH_STATUS enabled=created==MH_OK?MH_EnableHook(target):MH_ERROR_NOT_CREATED;
 std::printf("EXPORT-HUD-ENDPOINT hook_slot=%u create=%d enable=%d\n",slot,int(created),int(enabled));
 if(created==MH_OK)hudHooks[hudHookCount++]=target;
 if(created!=MH_OK||enabled!=MH_OK){
  RemoveHudHooks();Need(false,"EXPORT-HUD-ENDPOINT installs all write and scene guards");
 }
}
template<unsigned Slot,class Signature,class... Args>void InstallHudTap(){
 static_assert(std::is_invocable_r_v<HRESULT,Signature,IDirect3DDevice9*,Args...>,
     "endpoint hook arguments must match the SDK method");
 using Tap=HudTap<Slot,Args...>;
 InstallHudHook(Slot,reinterpret_cast<void*>(&Tap::Call),reinterpret_cast<void**>(&Tap::real));
}
void BeginHudEndpoint(MMDApp* app,const char* stage){
 if(!HudEndpointAt(stage))return;
 Need(!hudDevice&&!hudReadDone,"EXPORT-HUD-ENDPOINT is a single process endpoint");
 if(std::strcmp(stage,"diag-repeat")==0)
  Need(hudPriorLayoutCaptured,"EXPORT-HUD-ENDPOINT repeat follows normal unhooked after-layout rendering and readback");
 Need(!app->RecordingWindow()&&!GetModuleHandleW(L"MMEffect.dll")&&!GetModuleHandleW(L"MMHack.dll"),
     "EXPORT-HUD-ENDPOINT requires the ordinary native fixture without MME");
 hudBackHr=app->Renderer()->device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&hudBack);
 hudDeviceHr=SUCCEEDED(hudBackHr)&&hudBack?hudBack->GetDevice(&hudDevice):E_FAIL;
 std::printf("EXPORT-HUD-ENDPOINT back_hr=0x%08lx native_hr=0x%08lx\n",
     static_cast<unsigned long>(hudBackHr),static_cast<unsigned long>(hudDeviceHr));
 Need(SUCCEEDED(hudBackHr)&&SUCCEEDED(hudDeviceHr)&&hudDevice,
     "EXPORT-HUD-ENDPOINT resolves the native backbuffer device");
 hudExpectedBack=hudBack.Get();
 hudVb=app->OverlayVertices();hudAtlas=app->TextOverlayTexture();
 hudPrimitives=static_cast<UINT>(app->TextOverlayPrimitiveCount());
 Need(hudVb&&hudAtlas&&hudPrimitives,"EXPORT-HUD-ENDPOINT has independent text expectations");
 g_drawStateStage=stage;hudResetsBefore=resets;
 InstallHudHook(81,reinterpret_cast<void*>(&HudDraw),reinterpret_cast<void**>(&hudRealDraw));
 InstallHudTap<82,decltype(&IDirect3DDevice9::DrawIndexedPrimitive),D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT>();
 InstallHudTap<83,decltype(&IDirect3DDevice9::DrawPrimitiveUP),D3DPRIMITIVETYPE,UINT,const void*,UINT>();
 InstallHudTap<84,decltype(&IDirect3DDevice9::DrawIndexedPrimitiveUP),D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT>();
 InstallHudTap<115,decltype(&IDirect3DDevice9::DrawRectPatch),UINT,const float*,const D3DRECTPATCH_INFO*>();
 InstallHudTap<116,decltype(&IDirect3DDevice9::DrawTriPatch),UINT,const float*,const D3DTRIPATCH_INFO*>();
 InstallHudTap<43,decltype(&IDirect3DDevice9::Clear),DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD>();
 InstallHudTap<34,decltype(&IDirect3DDevice9::StretchRect),IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const RECT*,D3DTEXTUREFILTERTYPE>();
 InstallHudTap<35,decltype(&IDirect3DDevice9::ColorFill),IDirect3DSurface9*,const RECT*,D3DCOLOR>();
 InstallHudTap<30,decltype(&IDirect3DDevice9::UpdateSurface),IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const POINT*>();
 InstallHudTap<31,decltype(&IDirect3DDevice9::UpdateTexture),IDirect3DBaseTexture9*,IDirect3DBaseTexture9*>();
 InstallHudTap<85,decltype(&IDirect3DDevice9::ProcessVertices),UINT,UINT,UINT,IDirect3DVertexBuffer9*,IDirect3DVertexDeclaration9*,DWORD>();
 InstallHudTap<17,decltype(&IDirect3DDevice9::Present),const RECT*,const RECT*,HWND,const RGNDATA*>();
 InstallHudTap<41,decltype(&IDirect3DDevice9::BeginScene)>();
 InstallHudTap<42,decltype(&IDirect3DDevice9::EndScene)>();
 hudWatchOpen=true;
}
bool HudWriteBytes(const char* suffix,const void* bytes,size_t size){
 std::ofstream file(output/(std::string(HudEndpointStage())+suffix),std::ios::binary);
 const bool bounded=size<=static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max());
 if(bounded&&file)file.write(static_cast<const char*>(bytes),static_cast<std::streamsize>(size));
 file.close();const bool ok=bounded&&bool(file);
 Check(ok,"EXPORT-HUD-ENDPOINT writes and closes binary output");return ok;
}

bool HudSnapshotResources(std::ofstream& meta){
 // Only after the original final Pixels readback/output. These are frame-end
 // CPU copies, not simultaneous GPU contents. Flags 0 for the managed
 // WRITEONLY VB may dirty/re-upload it; no draw or rebuild follows this read.
 bool ok=true;
 D3DVERTEXBUFFER_DESC vbDesc{};
 const HRESULT vbDescHr=hudVb->GetDesc(&vbDesc);
 const std::uint64_t offset=std::uint64_t(hudTextDraw.streamOffset)+
     std::uint64_t(hudTextDraw.startVertex)*hudTextDraw.streamStride;
 const std::uint64_t bytes=std::uint64_t(hudTextDraw.primitiveCount)*3*hudTextDraw.streamStride;
 meta<<"resource_order=VB.GetDesc,VB.Lock,VB.copy,VB.Unlock,VB.write,atlas.GetLevelDesc,atlas.LockRect,atlas.copy,atlas.UnlockRect,atlas.write\n"
     <<"vb_desc_hr=0x"<<std::hex<<static_cast<unsigned long>(vbDescHr)<<std::dec
     <<" size="<<vbDesc.Size<<" format="<<vbDesc.Format<<" pool="<<vbDesc.Pool
     <<" usage="<<vbDesc.Usage<<" fvf="<<vbDesc.FVF<<" offset="<<offset<<" bytes="<<bytes<<" lock_flags=0\n";
 if(FAILED(vbDescHr)||!bytes||offset>vbDesc.Size||bytes>vbDesc.Size-offset||
    vbDesc.Pool!=D3DPOOL_MANAGED||vbDesc.FVF!=kDrawStateTextFvf){
  meta<<"vb_valid=0\n";ok=false;
 }else{
  std::vector<unsigned char> copy(static_cast<size_t>(bytes));void* data=nullptr;
  const HRESULT lockHr=hudVb->Lock(static_cast<UINT>(offset),static_cast<UINT>(bytes),&data,0);
  meta<<"vb_lock_hr=0x"<<std::hex<<static_cast<unsigned long>(lockHr)<<std::dec<<" pointer_valid="<<(data!=nullptr)<<"\n";
  if(SUCCEEDED(lockHr)){
   if(data)std::memcpy(copy.data(),data,copy.size());
   const HRESULT unlockHr=hudVb->Unlock();
   meta<<"vb_unlock_hr=0x"<<std::hex<<static_cast<unsigned long>(unlockHr)<<std::dec<<"\n";
   const bool valid=data&&SUCCEEDED(unlockHr);
   const bool written=valid&&HudWriteBytes(".endpoint-text-vb.bin",copy.data(),copy.size());
   meta<<"vb_write_ok="<<written<<"\n";ok=ok&&written;
  }else ok=false;
 }
 D3DSURFACE_DESC desc{};const HRESULT descHr=hudAtlas->GetLevelDesc(0,&desc);
 const DWORD levels=hudAtlas->GetLevelCount();
 const std::uint64_t rowBytes=std::uint64_t(desc.Width)*4;
 const bool bounded=desc.Height&&rowBytes<=std::numeric_limits<size_t>::max()/desc.Height;
 meta<<"atlas_desc_hr=0x"<<std::hex<<static_cast<unsigned long>(descHr)<<std::dec
     <<" level=0 levels="<<levels<<" width="<<desc.Width<<" height="<<desc.Height
     <<" format="<<desc.Format<<" pool="<<desc.Pool<<" usage="<<desc.Usage
     <<" msaa="<<desc.MultiSampleType<<" quality="<<desc.MultiSampleQuality<<" row_bytes="<<rowBytes<<"\n";
 // The recorded sampler must use only level 0. Export defined texels of that
 // level, excluding row padding; do not speculate about sampled UV footprints.
 if(FAILED(descHr)||desc.Format!=D3DFMT_A8R8G8B8||desc.MultiSampleType!=D3DMULTISAMPLE_NONE||
    !desc.Width||!bounded||levels==0||
    hudTextDraw.samplerValue[2]!=D3DTEXF_NONE||hudTextDraw.samplerValue[9]!=0){
  meta<<"atlas_valid=0\n";ok=false;
 }else{
  std::vector<unsigned char> copy(static_cast<size_t>(rowBytes)*desc.Height);D3DLOCKED_RECT locked{};
  const HRESULT lockHr=hudAtlas->LockRect(0,&locked,nullptr,D3DLOCK_READONLY);
  meta<<"atlas_lock_hr=0x"<<std::hex<<static_cast<unsigned long>(lockHr)<<std::dec
      <<" pitch="<<locked.Pitch<<" lock_flags="<<D3DLOCK_READONLY<<" pointer_valid="<<(locked.pBits!=nullptr)<<"\n";
  if(SUCCEEDED(lockHr)){
   const bool valid=locked.pBits&&locked.Pitch>0&&std::uint64_t(locked.Pitch)>=rowBytes&&
       std::uint64_t(locked.Pitch)*desc.Height<=std::numeric_limits<size_t>::max();
   if(valid)for(UINT y=0;y<desc.Height;++y)
    std::memcpy(copy.data()+size_t(y)*static_cast<size_t>(rowBytes),
        static_cast<const char*>(locked.pBits)+size_t(y)*locked.Pitch,static_cast<size_t>(rowBytes));
   const HRESULT unlockHr=hudAtlas->UnlockRect(0);
   meta<<"atlas_unlock_hr=0x"<<std::hex<<static_cast<unsigned long>(unlockHr)<<std::dec<<"\n";
   const bool written=valid&&SUCCEEDED(unlockHr)&&HudWriteBytes(".endpoint-text-atlas.bgra",copy.data(),copy.size());
   meta<<"atlas_write_ok="<<written<<" bytes="<<copy.size()<<" byte_order=BGRA padding_excluded=1\n";ok=ok&&written;
  }else ok=false;
 }
 return ok;
}
int FinishHudEndpoint(){
 const bool hooksRemoved=RemoveHudHooks();
 unsigned queryFailures=0,callFailures=0;
 for(unsigned i=0;i<hudDrawCount;++i)queryFailures+=DrawStateQueryFailures(g_drawStateRecords[i]);
 for(unsigned i=0;i<hudOperationCount;++i)callFailures+=FAILED(hudOperations[i].hr)?1u:0u;
 bool valid=hooksRemoved&&hudReadDone&&hudMatches==1&&hudSkipped==(HudEndpointSkip()?1u:0u)&&
     hudOverflow==0&&hudLateWrites==0&&hudEndScenes==1&&hudBindingErrors==0&&
     queryFailures==0&&callFailures==0&&resets==hudResetsBefore;
 if(hudUniformBackground){
  valid=valid&&hudUniformClearHr==D3D_OK&&hudBackDesc.MultiSampleType==D3DMULTISAMPLE_8_SAMPLES&&
      hudBackDesc.Format==D3DFMT_A8R8G8B8&&hudBackDesc.Width>=UINT(kHudUniformRect.x2)&&
      hudBackDesc.Height>=UINT(kHudUniformRect.y2)&&
      hudRaw.size()==std::size_t(hudBackDesc.Width)*hudBackDesc.Height;
  if(valid&&HudEndpointSkip()){
   for(LONG y=kHudUniformRect.y1;y<kHudUniformRect.y2;++y)
    for(LONG x=kHudUniformRect.x1;x<kHudUniformRect.x2;++x)
     valid=valid&&hudRaw[std::size_t(y)*hudBackDesc.Width+x]==
         kHudUniformColors[y-kHudUniformRect.y1];
  }
 }
 std::ofstream meta(output/(std::string(HudEndpointStage())+".endpoint.meta"));
 if(hudUniformBackground)
  meta<<"uniform_rect="<<kHudUniformRect.x1<<","<<kHudUniformRect.y1<<","<<
      kHudUniformRect.x2<<","<<kHudUniformRect.y2<<" uniform_bgra=0x"<<std::hex<<kHudUniformColors[0]<<
      ",0x"<<kHudUniformColors[1]<<" clear_hr=0x"<<static_cast<unsigned long>(hudUniformClearHr)<<std::dec<<"\n";
 meta<<"stage="<<HudEndpointStage()<<" skip="<<HudEndpointSkip()<<" diagnostic_only=1 full_regression=not_run\n"
     <<"shutdown=process_exit_after_closed_outputs;production_cleanup_not_run\n"
     <<"prior_normal_after_layout_capture="<<hudPriorLayoutCaptured<<"\n"
     <<"matches="<<hudMatches<<" omitted="<<hudSkipped<<" late_writes="<<hudLateWrites
     <<" end_scenes_after_text="<<hudEndScenes<<" overflow="<<hudOverflow<<" binding_errors="<<hudBindingErrors
     <<" query_failures="<<queryFailures<<" call_failures="<<callFailures<<" hooks_removed="<<hooksRemoved
     <<" resets_before="<<hudResetsBefore<<" resets_after="<<resets<<"\n"
     <<"expected_vb="<<static_cast<void*>(hudVb.Get())<<" expected_atlas="<<static_cast<void*>(hudAtlas.Get())
     <<" expected_primitives="<<hudPrimitives<<" expected_target="<<static_cast<void*>(hudBack.Get())<<"\n"
     <<"back_hr=0x"<<std::hex<<static_cast<unsigned long>(hudBackHr)
     <<" device_hr=0x"<<static_cast<unsigned long>(hudDeviceHr)<<std::dec<<"\n"
     <<"write_guard=all_native_device_draw_clear_transfer_calls_until_original_Pixels_entry;other_targets_conservatively_rejected\n"
     <<"read_order=original_resolve_if_MSAA,original_GetRenderTargetData,original_LockRect_RGB_and_raw_copy,original_UnlockRect,original_RGB_and_mode2_outputs,endpoint_raw_write,resource_snapshot_if_text\n"
     <<"raw_width="<<hudBackDesc.Width<<" raw_height="<<hudBackDesc.Height<<" raw_format="<<hudBackDesc.Format
     <<" raw_msaa="<<hudBackDesc.MultiSampleType<<" raw_quality="<<hudBackDesc.MultiSampleQuality
     <<" raw_pitch="<<hudBackPitch<<" raw_row_bytes="<<std::uint64_t(hudBackDesc.Width)*4
     <<" raw_bytes="<<hudRaw.size()*4<<" raw_padding_excluded=1 raw_byte_order=BGRA\n"
     <<"alpha_note=A8_alpha_preserved;X8_high_byte_undefined;MSAA_is_resolved_not_per_sample\n"
     <<"limits=frame_end_CPU_visible_resources_not_draw_time_GPU_snapshot;VB_flags0_may_dirty;skipping_draw_changes_submission_timing;no_visual_or_root_cause_acceptance\n";
 for(unsigned i=0;i<hudOperationCount;++i){
  const auto& op=hudOperations[i];
  meta<<"operation="<<i+1<<" slot="<<op.slot<<" after_text="<<op.afterText<<" omitted="<<op.skipped
      <<" hr=0x"<<std::hex<<static_cast<unsigned long>(op.hr)<<std::dec<<"\n";
 }
 for(unsigned i=0;i<hudDrawCount;++i)DrawStateWriteRecord(meta,g_drawStateRecords[i]);
 for(unsigned i=0;i<hudReadCount;++i)meta<<"read="<<i+1<<" operation="<<hudReads[i].operation
     <<" hr=0x"<<std::hex<<static_cast<unsigned long>(hudReads[i].hr)<<std::dec<<"\n";
 const bool rawWritten=HudWriteBytes(".endpoint-raw.bgra",hudRaw.data(),hudRaw.size()*4);
 meta<<"raw_write_ok="<<rawWritten<<"\n";valid=valid&&rawWritten;
 if(valid&&!HudEndpointSkip())valid=HudSnapshotResources(meta);
 else meta<<"resources=not_read reason="<<(HudEndpointSkip()?"skip_endpoint":"invalid_sample")<<"\n";
 valid=valid&&failures==0&&viewportFailures==0&&resetFailures==0;
 meta<<"sample_valid="<<valid<<" prior_assertion_failures="<<failures
     <<" full_regression=not_run diagnostic_exit="<<(valid?20:21)<<"\n";
 meta.close();const bool metaWritten=bool(meta);
 Check(metaWritten,"EXPORT-HUD-ENDPOINT writes and closes endpoint metadata");
 valid=valid&&metaWritten;
 std::printf("EXPORT-HUD-ENDPOINT stage=%s skip=%d sample_valid=%d metadata_written=%d full_regression=NOT_RUN diagnostic_exit=%d\n",
     HudEndpointStage(),HudEndpointSkip()?1:0,valid?1:0,metaWritten?1:0,valid?20:21);
 // EndScene and restoration already returned before Pixels. Ordinary shutdown
 // can dispatch window messages while draining a recorder graph; do not enter
 // that path after resource reads. This independent process endpoint closes
 // every output, removes its hooks and terminates without another frame.
 hudVb.Reset();hudAtlas.Reset();hudBack.Reset();hudDevice.Reset();g_drawStateStage=nullptr;
 phase="endpoint-exit";ExitProcess(valid?20:21);
}

void Pump(){MSG m;for(int i=0;i<100&&PeekMessageW(&m,nullptr,0,0,PM_REMOVE);++i){TranslateMessage(&m);DispatchMessageW(&m);}}
void Frame(MMDApp*a){Pump();a->DeltaTime()=1.f/30;a->state.messageSeen=1;coordinator_fixture::Drive(a);}
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
   if(cameraCase==0){
     Need(cameraFrames.size()==6&&!cameraFrames.front().empty(),"capture the camera-mode reference frames");
     bool colored=false;const auto&first=cameraFrames.front();
     for(size_t pixel=0;pixel+3<first.size();pixel+=4)
       colored|=first[pixel]!=first[pixel+1]||first[pixel+1]!=first[pixel+2];
     Check(colored,"reference MP4 contains colored scene geometry");
     Check(cameraFrames.front()!=cameraFrames.back(),"reference MP4 changes with its camera keys");
   }
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

// MAIN-EXPORT is deliberately separate from export_viewport_runner.py (which
// only hides windows). The supervisor must CreateDesktopW, retain that handle,
// and CreateProcessW with STARTUPINFO.lpDesktop set to its unique name. Invoke:
//   MikuMikuDance.exe --main-export-private-desktop <name> frame-driver
// or use coordinator as the last argument. Never SwitchDesktop. Wait for the
// child process AND its threads to exit before CloseDesktop; MSCTF/MF workers
// may outlive window teardown. An explicit --main-export-desktop-bootstrap mode
// supports hosts whose imported GUI DLLs cannot initialize on a new desktop:
// the supervisor starts on its existing desktop, then wmain creates the named
// private desktop at the child's own integrity level. The supervisor opens and
// holds it after a fixture-file handshake, before any test GUI/device is made.
// Both modes require the same non-input desktop checks; neither falls back.
// Copy the full-profile test executable and its normal runtime dependencies to
// a fresh fixture directory (also cwd); supply Data/mmconfig.ini as in the old
// runner. Do NOT copy MMEffect.dll, MMHack.dll or a d3d9.dll proxy. Redirect
// LOCALAPPDATA, APPDATA, MIKUMIKUSTUDIO_AUTOSAVE_CONFIG, MMS_ANTIALIAS_CONFIG to
// absolute paths below this directory. Capture stdout/stderr and enforce an
// external timeout (e.g. 180 s). This mode needs Windows 10+, D3D9 and the system
// software H264 encoder/decoder; missing prerequisites FAIL, never skip.
// All BMP/MP4 evidence goes into a new child-owned subdirectory and is retained.
#if !MMD_ORIGINAL_PROFILE
// Observation only: the production factory and every CreateDevice retry keep
// their original arguments, output slots, return values and LastError. These
// hooks are installed only by MainExportEntry, never by the older probe modes.
using MainExportCreateDevice=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,
    D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
decltype(&Direct3DCreate9) mainExportRealCreate9=nullptr;
MainExportCreateDevice mainExportRealCreateDevice=nullptr,mainExportCreateDeviceTarget=nullptr;
DWORD mainExportD3DThread=0;
unsigned mainExportDeviceAttempts=0;
// PROBE_MAIN_EXPORT_NO_MSAA is accepted only by the MAIN-EXPORT entry points
// below (private desktop, explicit hidden desktop and desktop bootstrap). The
// exact value "1" forces the production multisample ladder onto its real
// non-MSAA branch by denying capability queries while initialization runs. It
// never edits CreateDevice arguments, renderer flags or the requested mode, and
// an absent variable keeps every existing MAIN-EXPORT behavior unchanged.
bool mainExportNoMsaa=false;
// PROBE_MAIN_EXPORT_VIDEO is accepted only by these MAIN-EXPORT entry points.
// Unset keeps the exact existing picture fixture. A present value names a real
// MP4 background below the isolated fixture root; it is validated before any
// app, window or D3D object exists (see ConfigureMainExportBackground).
std::wstring mainExportVideoPath;
bool mainExportProtectBackground=false;
bool mainExportInitActive=false;
IDirect3D9* mainExportFactory=nullptr;
volatile unsigned mainExportMsaaDenied=0;
volatile unsigned mainExportDownsampleCalls=0;
unsigned mainExportDownsampleAtWrite=0;
bool mainExportCapabilityHookInstalled=false;
bool mainExportCapabilityMismatch=false;
using MainExportCheckMultiSample=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,BOOL,
    D3DMULTISAMPLE_TYPE,DWORD*);
// d3d9.h IDirect3D9 order after IUnknown: RegisterSoftwareDevice(3),
// GetAdapterCount(4), GetAdapterIdentifier(5), GetAdapterModeCount(6),
// EnumAdapterModes(7), GetAdapterDisplayMode(8), CheckDeviceType(9),
// CheckDeviceFormat(10), CheckDeviceMultiSampleType(11). The static_assert
// checks the SDK member is invocable with the observer argument list and
// returns HRESULT; its method decorations are not compared as a type. The
// required post-init denial count separately proves the captured slot is the
// method the production ladder actually calls.
constexpr unsigned kMainExportCheckMultiSampleSlot=11;
static_assert(std::is_invocable_r_v<HRESULT,
    decltype(&IDirect3D9::CheckDeviceMultiSampleType),IDirect3D9*,UINT,D3DDEVTYPE,
    D3DFORMAT,BOOL,D3DMULTISAMPLE_TYPE,DWORD*>,
    "no-MSAA capability observer must be invocable with the IDirect3D9::CheckDeviceMultiSampleType signature");
MainExportCheckMultiSample mainExportRealCheckMultiSample=nullptr,mainExportCheckMultiSampleTarget=nullptr;
int MainExportNoMsaaRequest() {
  const char* value=std::getenv("PROBE_MAIN_EXPORT_NO_MSAA");
  if(value==nullptr)return 0;
  return std::strcmp(value,"1")==0?1:-1;
}
HRESULT WINAPI MainExportObserveCheckMultiSample(IDirect3D9* d3d,UINT adapter,D3DDEVTYPE type,
    D3DFORMAT format,BOOL windowed,D3DMULTISAMPLE_TYPE samples,DWORD* quality) {
  // Tiny gate: no logging, allocation or message pumping. Only the initialization
  // thread, the exact factory InitD3D returned and the forced mode may be denied.
  const DWORD incomingError=GetLastError();
  if(mainExportNoMsaa && mainExportInitActive && GetCurrentThreadId()==mainExportD3DThread &&
     d3d==mainExportFactory && samples!=D3DMULTISAMPLE_NONE) {
    if(quality!=nullptr)*quality=0;
    ++mainExportMsaaDenied;
    SetLastError(incomingError);
    return D3DERR_NOTAVAILABLE;
  }
  return mainExportRealCheckMultiSample(d3d,adapter,type,format,windowed,samples,quality);
}

template<class F>bool MainExportObservationHook(const char* name,F target,F replacement,F* original) {
 const DWORD error=GetLastError();
 const auto created=MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),
     reinterpret_cast<void**>(original));
 const auto enabled=created==MH_OK?MH_EnableHook(reinterpret_cast<void*>(target)):MH_ERROR_NOT_CREATED;
 std::printf("MAIN-EXPORT D3D hook=%s target=%p create_status=%d enable_status=%d\n",
     name,reinterpret_cast<void*>(target),int(created),int(enabled));
 // A missing diagnostic must not replace a real initialization failure with
 // a probe assertion/exit, or leave a disabled hook occupying the target.
 if(created==MH_OK && enabled!=MH_OK)MH_RemoveHook(reinterpret_cast<void*>(target));
 SetLastError(error);
 return created==MH_OK && enabled==MH_OK;
}
void MainExportD3DDesktop(unsigned attempt,const char* role,DWORD thread) {
 const auto desktop=thread?GetThreadDesktop(thread):nullptr;
 const DWORD desktopError=desktop?ERROR_SUCCESS:(thread?GetLastError():ERROR_INVALID_THREAD_ID);
 wchar_t name[256]{};DWORD needed=0;BOOL input=FALSE;
 const BOOL named=desktop && GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof name,&needed);
 const DWORD nameError=named?ERROR_SUCCESS:(desktop?GetLastError():desktopError);
 const BOOL queriedInput=desktop && GetUserObjectInformationW(desktop,UOI_IO,&input,sizeof input,&needed);
 const DWORD inputError=queriedInput?ERROR_SUCCESS:(desktop?GetLastError():desktopError);
 std::printf("MAIN-EXPORT D3D attempt=%u desktop_role=%s tid=%lu desktop=%p name=%ls name_error=%lu input=%d input_error=%lu\n",
     attempt,role,thread,static_cast<void*>(desktop),named?name:L"<unavailable>",nameError,
     queriedInput?int(input):-1,inputError);
 // GetThreadDesktop returns a borrowed handle; never close or switch it.
}
void MainExportD3DWindow(unsigned attempt,const char* role,HWND window) {
 const BOOL valid=IsWindow(window);
 DWORD process=0;const DWORD thread=window?GetWindowThreadProcessId(window,&process):0;
 WINDOWINFO info{};info.cbSize=sizeof info;
 const BOOL hasInfo=valid && GetWindowInfo(window,&info);
 const DWORD infoError=hasInfo?ERROR_SUCCESS:(valid?GetLastError():ERROR_INVALID_WINDOW_HANDLE);
 wchar_t name[256]{};const int named=valid?GetClassNameW(window,name,256):0;
 const DWORD nameError=named?ERROR_SUCCESS:(valid?GetLastError():ERROR_INVALID_WINDOW_HANDLE);
 // InitD3D runs inside WM_CREATE, before CreateWindowExW returns and stores
 // MainWindow(). A null main slot alone does not mean the focus HWND is bad.
 const HWND main=g_Block?g_Block->MainWindow():nullptr;
 const HWND renderer=g_Block && g_Block->Renderer()?g_Block->Renderer()->hwnd:nullptr;
 std::printf("MAIN-EXPORT D3D attempt=%u window_role=%s hwnd=%p valid=%d pid=%lu tid=%lu class=%ls class_error=%lu "
     "main=%p renderer=%p is_main=%d is_renderer=%d visible=%d iconic=%d style=0x%08lx exstyle=0x%08lx "
     "client=%ld,%ld,%ld,%ld window=%ld,%ld,%ld,%ld info_error=%lu dpi=%u dpi_context=%p\n",
     attempt,role,static_cast<void*>(window),int(valid),process,thread,named?name:L"<unavailable>",nameError,
     static_cast<void*>(main),static_cast<void*>(renderer),window && window==main,window && window==renderer,
     int(IsWindowVisible(window)),int(IsIconic(window)),info.dwStyle,info.dwExStyle,
     info.rcClient.left,info.rcClient.top,info.rcClient.right,info.rcClient.bottom,
     info.rcWindow.left,info.rcWindow.top,info.rcWindow.right,info.rcWindow.bottom,infoError,
     valid && testDpiForWindow?testDpiForWindow(window):0,
     valid && testWindowDpiContext?static_cast<void*>(testWindowDpiContext(window)):nullptr);
 MainExportD3DDesktop(attempt,role,thread);
}
void MainExportD3DParameters(unsigned attempt,const char* when,const D3DPRESENT_PARAMETERS* pp) {
 if(!pp) {std::printf("MAIN-EXPORT D3D attempt=%u pp_%s=null\n",attempt,when);return;}
 std::printf("MAIN-EXPORT D3D attempt=%u pp_%s=%p backbuffer=%ux%u format=%u count=%u msaa=%u quality=%lu "
     "swap=%u device_window=%p windowed=%d auto_depth=%d depth=%u flags=0x%08lx refresh=%u interval=0x%08x\n",
     attempt,when,static_cast<const void*>(pp),pp->BackBufferWidth,pp->BackBufferHeight,
     unsigned(pp->BackBufferFormat),pp->BackBufferCount,unsigned(pp->MultiSampleType),pp->MultiSampleQuality,
     unsigned(pp->SwapEffect),static_cast<void*>(pp->hDeviceWindow),int(pp->Windowed),int(pp->EnableAutoDepthStencil),
     unsigned(pp->AutoDepthStencilFormat),pp->Flags,pp->FullScreen_RefreshRateInHz,pp->PresentationInterval);
}
HRESULT WINAPI MainExportObserveCreateDevice(IDirect3D9* d3d,UINT adapter,D3DDEVTYPE type,HWND focus,
    DWORD behavior,D3DPRESENT_PARAMETERS* pp,IDirect3DDevice9** device) {
 const DWORD incomingError=GetLastError();
 // MF workers can share the runtime entry point. Observe only the fixture's
 // initialization thread; other callers go straight through the trampoline.
 if(GetCurrentThreadId()!=mainExportD3DThread) {
   SetLastError(incomingError);
   return mainExportRealCreateDevice(d3d,adapter,type,focus,behavior,pp,device);
 }
 const unsigned attempt=++mainExportDeviceAttempts;
 std::printf("MAIN-EXPORT D3D CreateDevice begin attempt=%u phase=%s d3d=%p adapter=%u type=%u behavior=0x%08lx "
     "focus=%p pp=%p output_slot=%p tid=%lu\n",attempt,phase,static_cast<void*>(d3d),adapter,unsigned(type),behavior,
     static_cast<void*>(focus),static_cast<void*>(pp),static_cast<void*>(device),GetCurrentThreadId());
 // Log before forwarding: D3D9 may update presentation parameters even on a
 // failed attempt. Do not normalize, copy back, or retry anything here.
 MainExportD3DParameters(attempt,"before",pp);
 MainExportD3DDesktop(attempt,"caller",GetCurrentThreadId());
 MainExportD3DWindow(attempt,"focus",focus);
 MainExportD3DWindow(attempt,"device",pp?pp->hDeviceWindow:nullptr);
 SetLastError(incomingError);
 const HRESULT result=mainExportRealCreateDevice(d3d,adapter,type,focus,behavior,pp,device);
 const DWORD returnedError=GetLastError();
 std::printf("MAIN-EXPORT D3D CreateDevice end attempt=%u hr=0x%08lx last_error=%lu device=%p\n",
     attempt,static_cast<unsigned long>(result),returnedError,
     SUCCEEDED(result) && device?static_cast<void*>(*device):nullptr);
 MainExportD3DParameters(attempt,"after",pp);
 std::fflush(stdout);SetLastError(returnedError);
 return result;
}
IDirect3D9* WINAPI MainExportObserveCreate9(UINT sdk) {
 const DWORD incomingError=GetLastError();
 const bool observe=GetCurrentThreadId()==mainExportD3DThread;
 SetLastError(incomingError);
 auto* d3d=mainExportRealCreate9(sdk);
 const DWORD returnedError=GetLastError();
 if(observe) {
   wchar_t stationName[256]{};DWORD needed=0;
   const auto station=GetProcessWindowStation();
   const BOOL named=station && GetUserObjectInformationW(station,UOI_NAME,stationName,sizeof stationName,&needed);
   const DWORD stationError=named?ERROR_SUCCESS:GetLastError();
   auto* renderer=g_Block?g_Block->Renderer():nullptr;
   std::printf("MAIN-EXPORT D3D Direct3DCreate9 sdk=%u result=%p last_error=%lu station=%p name=%ls name_error=%lu "
       "renderer_size=%dx%d thread_dpi_context=%p\n",sdk,static_cast<void*>(d3d),returnedError,
       static_cast<void*>(station),named?stationName:L"<unavailable>",stationError,
       renderer?renderer->screenWidth:0,renderer?renderer->screenHeight:0,
       testThreadDpiContext?static_cast<void*>(testThreadDpiContext()):nullptr);
   MainExportD3DDesktop(0,"factory-caller",GetCurrentThreadId());
   if(d3d) {
     // IDirect3D9 slot 16 (after IUnknown and the adapter/capability methods),
     // taken from the actual returned object, not a dummy device or proxy.
     auto** table=*reinterpret_cast<void***>(d3d);
     const auto target=reinterpret_cast<MainExportCreateDevice>(table[16]);
     if(!mainExportCreateDeviceTarget) {
       if(MainExportObservationHook("IDirect3D9::CreateDevice",target,&MainExportObserveCreateDevice,
           &mainExportRealCreateDevice))mainExportCreateDeviceTarget=target;
     } else if(mainExportCreateDeviceTarget!=target) {
       std::printf("MAIN-EXPORT D3D observation unavailable: different CreateDevice target=%p installed=%p\n",
           reinterpret_cast<void*>(target),reinterpret_cast<void*>(mainExportCreateDeviceTarget));
     }
    // Install the capability observer on this exact returned factory before
    // the production multisample ladder runs, and only for the forced mode.
    if(mainExportNoMsaa && mainExportInitActive) {
      mainExportFactory=d3d;
      const auto capability=reinterpret_cast<MainExportCheckMultiSample>(
          table[kMainExportCheckMultiSampleSlot]);
      if(!mainExportCheckMultiSampleTarget) {
        if(MainExportObservationHook("IDirect3D9::CheckDeviceMultiSampleType",capability,
            &MainExportObserveCheckMultiSample,&mainExportRealCheckMultiSample)) {
          mainExportCheckMultiSampleTarget=capability;
          mainExportCapabilityHookInstalled=true;
        }
      } else if(mainExportCheckMultiSampleTarget!=capability) {
        mainExportCapabilityMismatch=true;
        std::printf("MAIN-EXPECT no-msaa capability hook unavailable: different target=%p installed=%p\n",
            reinterpret_cast<void*>(capability),
            reinterpret_cast<void*>(mainExportCheckMultiSampleTarget));
      }
    }
   }
   std::fflush(stdout);
 }
 SetLastError(returnedError);
 return d3d;
}

enum class ExportDestination { Fresh, Changed, Retry, LateArrival, LateRetry, ApprovalChanged, ApprovalRetry, ApprovalLate, DeclineThenChange };

bool DestinationConflict(ExportDestination destination) {
 return destination==ExportDestination::Changed || destination==ExportDestination::LateArrival;
}
struct MainExportCase {
 const char* name;
 int fps, first, last, samples;
 double duration;
 bool close, cancel, staleScene;
 ExportDestination destination=ExportDestination::Fresh;
 bool includeWave=false, systemH264=false;
 int playbackFps=30;
};
// Independent hand-counted oracles: timeline frames 17..46 occupy one second
// at the editor's 30 Hz. For 17..22, 24 Hz samples occur at timeline positions
// 17,18.25,19.5,20.75,22; 60 Hz samples occur at 17,17.5,...,22.5.
// The final sample owns a full output-frame duration. Do not derive expectations
// from OutputFrameCount, recordedFrameCount or the writer's expectedFrames.
const MainExportCase mainExportCases[] = {
 {"24-complete-close",24,17,46,24,1.0,true,false,false},
 {"60-complete-close",60,17,46,60,1.0,true,false,false},
 {"24-short-close",24,17,22,5,5.0/24,true,false,false},
 {"60-short-close",60,17,22,12,0.2,true,false,false},
 {"24-complete-keep",24,17,46,24,1.0,false,false,false},
 {"60-complete-keep",60,17,46,60,1.0,false,false,false},
 {"24-cancel-close-enabled",24,17,46,3,0.125,true,true,false},
 {"60-cancel-close-enabled",60,17,46,3,0.05,true,true,false},
 {"60-scene-changed-at-finalize",60,17,46,60,1.0,true,false,true},
 {"24-destination-changed",24,31,36,5,5.0/24,true,false,false,ExportDestination::Changed},
 {"24-destination-retry",24,31,36,5,5.0/24,true,false,false,ExportDestination::Retry},
 {"24-destination-late-arrival",24,31,36,5,5.0/24,true,false,false,ExportDestination::LateArrival},
 {"24-destination-late-retry",24,31,36,5,5.0/24,true,false,false,ExportDestination::LateRetry},
 {"24-destination-approval-changed",24,31,36,0,0,true,false,false,ExportDestination::ApprovalChanged},
 {"24-destination-approval-retry",24,31,36,5,5.0/24,true,false,false,ExportDestination::ApprovalRetry},
 {"24-destination-approval-late",24,31,36,0,0,true,false,false,ExportDestination::ApprovalLate},
};
// Six opt-in video-background cases. start=37 keeps every expected source
// second nonzero, so a start-at-zero or static-frame decoder cannot pass. Each
// cancelled run must leave the same loaded source available for a fresh full
// export, which re-seeks backward through the real decoder path.
constexpr int kMainExportVideoFirst=37,kMainExportVideoLast=66;
const MainExportCase mainExportVideoCases[] = {
 {"24-video-complete-keep",24,kMainExportVideoFirst,kMainExportVideoLast,24,1.0,false,false,false},
 {"24-video-cancel",24,kMainExportVideoFirst,kMainExportVideoLast,3,3.0/24,false,true,false},
 {"24-video-retry-complete",24,kMainExportVideoFirst,kMainExportVideoLast,24,1.0,false,false,false},
 {"60-video-complete-keep",60,kMainExportVideoFirst,kMainExportVideoLast,60,1.0,false,false,false},
 {"60-video-cancel",60,kMainExportVideoFirst,kMainExportVideoLast,3,3.0/60,false,true,false},
 {"60-video-retry-complete",60,kMainExportVideoFirst,kMainExportVideoLast,60,1.0,false,false,false},
};
const MainExportCase mainExportWaveCases[] = {
 {"24-wave-from-zero",24,0,29,24,1.0,false,false,false,ExportDestination::Fresh,true},
 {"24-wave-nonzero",24,17,46,24,1.0,false,false,false,ExportDestination::Fresh,true},
 {"60-system-wave-nonzero",60,17,46,60,1.0,false,false,false,ExportDestination::Fresh,true,true},
 {"24-wave-playback-37",24,27,63,24,1.0,false,false,false,ExportDestination::Fresh,true,false,37},
 {"24-wave-unchecked",24,17,46,24,1.0,false,false,false},
 {"24-wave-short-source",24,54,83,24,1.0,false,false,false,ExportDestination::Fresh,true},
 {"24-wave-past-end",24,90,119,24,1.0,false,false,false,ExportDestination::Fresh,true},
};
bool mainExportWave=false;
bool mainExportSaveRetry=false;
export_save_workflow::SaveRetry videoProjectSave;
struct MainExportProbe {
 MMDApp* app=nullptr;
 HWND window=nullptr;
 WNDPROC previous=nullptr;
 const MainExportCase* test=nullptr;
 unsigned writes=0, finishes=0, posts=0, closes=0, confirmations=0, conflictNotices=0;
 bool finishReturned=false, finishSucceeded=false, allowCancel=false;
 bool timelineOk=true, deltaOk=true;
 std::wstring retainedVideo;
 unsigned lateArrivals=0;
 fs::path pickerPath, confirmedPath, alternatePath;
 unsigned pickerCalls=0, settingsCalls=0, approvalPrompts=0, encoderStarts=0;
 HWND settingsWindow=nullptr;
 unsigned alternatePicks=0;
 std::uint8_t videoSceneModified=0;
 std::uint64_t scene=0;
 int savedWidth=0, savedHeight=0;
} mainExport;
decltype(&FinishBuiltInH264Recording) mainExportRealFinish=nullptr;
decltype(&WriteBuiltInH264Frame) mainExportRealWrite=nullptr;
decltype(&PostMessageW) mainExportRealPost=nullptr;
decltype(&ShowUiMessage) mainExportRealMessage=nullptr;
decltype(&MoveFileExW) mainExportRealMove=nullptr;
decltype(&GetUiSaveFileNameW) mainExportRealPicker=nullptr;
decltype(&UiDialogBoxParamA) mainExportRealSettings=nullptr;
decltype(&StartBuiltInH264Recording) mainExportRealStart=nullptr;
DLGPROC mainExportSettingsProc=nullptr;

bool MainExportMenuApproval() {
 if(!mainExport.test) return false;
 const auto destination=mainExport.test->destination;
 return destination==ExportDestination::ApprovalChanged || destination==ExportDestination::ApprovalRetry ||
        destination==ExportDestination::ApprovalLate || destination==ExportDestination::DeclineThenChange;
}

bool MainExportChangeDestination() {
 return mainExport.test && mainExport.test->destination==ExportDestination::DeclineThenChange;
}

void ReplaceApprovedVideo() {
 const auto& path=mainExport.confirmedPath;
 auto previous=path;
 previous += L".previous";
 Need(fs::is_regular_file(path) && !fs::exists(previous),"inject only at a fixture-owned existing video");
 fs::rename(path,previous);
 std::ofstream replacement(path,std::ios::binary);
 replacement << "new video after approval";
 Need(replacement.good(),"write the replacement without altering the previously approved file");
}

BOOL MainExportPicker(LPOPENFILENAMEW dialog) {
 if(videoProjectSave.OwnsPicker(dialog))return videoProjectSave.Pick(dialog);
 if(MainExportChangeDestination() && mainExport.settingsWindow && dialog->hwndOwner==mainExport.settingsWindow) {
   Need((dialog->Flags&OFN_OVERWRITEPROMPT)==0 && std::wstring_view(dialog->lpstrFile)==mainExport.pickerPath.native(),
        "changing output starts from the rejected filename and leaves overwrite to the final container");
   ++mainExport.alternatePicks;
   const bool copied=wcscpy_s(dialog->lpstrFile,dialog->nMaxFile,mainExport.alternatePath.c_str())==0;
   return copied && mainExport.alternatePicks==2;
 }
 if((!mainExportWave && !mainExportSaveRetry && !MainExportMenuApproval()) || dialog->hwndOwner!=mainExport.window) return mainExportRealPicker(dialog);
 ++mainExport.pickerCalls;
 Check((dialog->Flags&OFN_OVERWRITEPROMPT)==0,"the file picker delegates confirmation to the final video settings");
 return wcscpy_s(dialog->lpstrFile,dialog->nMaxFile,mainExport.pickerPath.c_str())==0;
}

INT_PTR CALLBACK MainExportDriveSettings(HWND dialog,UINT message,WPARAM wp,LPARAM lp) {
 const auto result=mainExportSettingsProc(dialog,message,wp,lp);
 if(message==WM_INITDIALOG) {
   ++mainExport.settingsCalls;
   const HWND combo=GetDlgItem(dialog,panel::kAviCodecCombo);
   const auto codec=mainExport.test->systemH264?kSystemH264Codec:kBuiltInH264Codec;
   bool selected=false;
   const auto rows=SendMessageW(combo,CB_GETCOUNT,0,0);
   for(int row=0;row<rows;++row) {
     if(SendMessageW(combo,CB_GETITEMDATA,row,0)!=codec) continue;
     SendMessageW(combo,CB_SETCURSEL,row,0);
     if(mainExportWave || mainExportSaveRetry)
       SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(panel::kAviCodecCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
     selected=true;
     break;
   }
   Need(selected,"settings offer the requested H264 encoder");
   SetDlgItemInt(dialog,panel::kAviStartFrameEdit,mainExport.test->first,FALSE);
   SetDlgItemInt(dialog,panel::kAviEndFrameEdit,mainExport.test->last,FALSE);
   SetDlgItemInt(dialog,panel::kAviFpsEdit,mainExport.test->fps,FALSE);
   SetDlgItemInt(dialog,panel::kAviWidthEdit,640,FALSE);
   SetDlgItemInt(dialog,panel::kAviHeightEdit,360,FALSE);
   CheckDlgButton(dialog,panel::kAvi3dVisionCheckbox,BST_UNCHECKED);
   if(mainExportWave)
     Need(IsWindowEnabled(GetDlgItem(dialog,panel::kAviWaveCheckbox)),"loaded WAV can be selected in video settings");
   CheckDlgButton(dialog,panel::kAviWaveCheckbox,mainExport.test->includeWave?BST_CHECKED:BST_UNCHECKED);
   CheckDlgButton(dialog,panel::kAviCloseAfterExportCheckbox,mainExport.test->close?BST_CHECKED:BST_UNCHECKED);
   if(mainExportSaveRetry) {
     using Parameter=export_save_workflow::Parameter;
     videoProjectSave.Drive(*mainExport.app,dialog,output/L"project-save",
       {{panel::kAviWidthEdit},{panel::kAviHeightEdit},{panel::kAviStartFrameEdit},
        {panel::kAviEndFrameEdit},{panel::kAviFpsEdit},{panel::kAviCodecCombo,Parameter::Choice},
        {panel::kAviWaveCheckbox,Parameter::CheckBox},{panel::kAvi3dVisionCheckbox,Parameter::CheckBox},
        {panel::kAviCloseAfterExportCheckbox,Parameter::CheckBox}},Need);
   }
   if(MainExportChangeDestination()) {
     mainExport.settingsWindow=dialog;
     const HWND change=GetDlgItem(dialog,panel::kVideoChangeDestinationButton);
     Need(change && (GetWindowLongPtrW(change,GWL_STYLE)&WS_TABSTOP), "settings has a keyboard-reachable change-output button");
     const auto previousPath=std::wstring(mainExport.app->AviOutputPath());
     const auto rejected = mainExportProtectBackground ? 0u : 1u;
     const auto requireDeclined=[&](unsigned confirmations) {
       Need(IsWindow(dialog) && !mainExport.app->RecordingWindow() && !mainExport.encoderStarts &&
            mainExport.approvalPrompts==confirmations && mainExport.app->AviOutputPath()==previousPath,
            "refusing overwrite keeps the same settings and does not commit or start an output");
     };
     SendMessageW(dialog,WM_COMMAND,IDOK,0);
     requireDeclined(rejected);
     if(mainExportProtectBackground) Need(mainExport.conflictNotices==1,
         "the settings explicitly refuse replacing the loaded background before overwrite confirmation");
     SendMessageW(change,BM_CLICK,0,0);
     Need(mainExport.alternatePicks==1 && IsWindow(dialog),"cancel the first alternate choice in the same settings");
     SendMessageW(dialog,WM_COMMAND,IDOK,0);
     requireDeclined(rejected*2);
     if(mainExportProtectBackground) Need(mainExport.conflictNotices==2,
         "canceling the alternate picker retains the protected candidate");
     SendMessageW(change,BM_CLICK,0,0);
     Need(mainExport.alternatePicks==2 && IsWindow(dialog) &&
          GetDlgItemInt(dialog,panel::kAviWidthEdit,nullptr,FALSE)==640 &&
          GetDlgItemInt(dialog,panel::kAviHeightEdit,nullptr,FALSE)==360 &&
          GetDlgItemInt(dialog,panel::kAviFpsEdit,nullptr,FALSE)==mainExport.test->fps &&
          GetDlgItemInt(dialog,panel::kAviStartFrameEdit,nullptr,FALSE)==mainExport.test->first &&
          GetDlgItemInt(dialog,panel::kAviEndFrameEdit,nullptr,FALSE)==mainExport.test->last &&
          SendMessageW(combo,CB_GETITEMDATA,SendMessageW(combo,CB_GETCURSEL,0,0),0)==codec &&
          IsDlgButtonChecked(dialog,panel::kAviCloseAfterExportCheckbox)==BST_CHECKED,
          "changing output preserves the original codec, size, range, FPS and close option");
   }
   PostMessageW(dialog,WM_COMMAND,IDOK,0);
 }
 return result;
}

INT_PTR MainExportSettings(HINSTANCE instance,LPCSTR name,HWND owner,DLGPROC dialog,LPARAM data) {
 if((!mainExportWave && !mainExportSaveRetry && !MainExportMenuApproval()) || owner!=mainExport.window || !IS_INTRESOURCE(name) ||
    (reinterpret_cast<ULONG_PTR>(name)!=0x28e && reinterpret_cast<ULONG_PTR>(name)!=0x260))
   return mainExportRealSettings(instance,name,owner,dialog,data);
 mainExportSettingsProc=dialog;
 return mainExportRealSettings(instance,name,owner,MainExportDriveSettings,data);
}

bool MainExportStart(DShowRecorder* recorder,const wchar_t* path,int width,int height,float fps,
                     const WaveAudioContext* audio,bool system,std::uint32_t bitrate,void* owner,
                     unsigned char language,const recording_output_path::Selection* approved,bool* changed) {
 if(mainExportWave) {
   Check(system==mainExport.test->systemH264,"video settings select the requested H264 path");
   Check((mainExport.app->AviIncludeWave()!=0)==mainExport.test->includeWave,"video settings commit the WAV checkbox");
 }
 if(MainExportMenuApproval()) {
   ++mainExport.encoderStarts;
   const bool change=MainExportChangeDestination();
   Need(approved && bool(approved->existing)!=change &&
        approved->path==(change?mainExport.alternatePath:mainExport.confirmedPath).native(),
        "the real H264 entry receives only the destination chosen and approved by the settings");
   if(mainExport.test->destination==ExportDestination::ApprovalLate) ReplaceApprovedVideo();
 }
 return mainExportRealStart(recorder,path,width,height,fps,audio,system,bitrate,owner,language,approved,changed);
}

BOOL WINAPI MainExportMove(LPCWSTR source,LPCWSTR destination,DWORD flags) {
 if(mainExport.test && mainExport.test->destination==ExportDestination::LateArrival &&
     destination && wcscmp(destination,mainExport.app->AviOutputPath())==0) {
   Need(++mainExport.lateArrivals==1,"inject only once at final publication");
   const HANDLE arrival=CreateFileW(destination,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
   Need(arrival!=INVALID_HANDLE_VALUE,"create a competing video at the free destination");
   constexpr char bytes[]="video arriving at publication";
   DWORD written=0;
   const bool saved=WriteFile(arrival,bytes,sizeof(bytes)-1,&written,nullptr) && written==sizeof(bytes)-1;
   CloseHandle(arrival);
   Need(saved,"write the competing video");
 }
 return mainExportRealMove(source,destination,flags);
}

bool MainExportStopped() {
 auto* app=mainExport.app;
 return app && !app->RecordingWindow() && !IsBuiltInH264Recording(app->Recorder()) &&
     !app->PlaybackActive() && !app->FrameStepPlayback() &&
     !app->state.recordPlaybackStartPending && !app->AviRenderResolutionOverridden() &&
     !app->CaptureRenderTarget() && !app->CaptureSystemSurface() && !app->CaptureReadbackPixels();
}
void CheckMainExportClose() {
 const auto* test=mainExport.test;
 Check(test && test->close && !test->cancel && !test->staleScene &&
       !DestinationConflict(test->destination),
       "only the opted-in natural success may request close");
 Check(mainExport.finishReturned && mainExport.finishSucceeded && mainExport.finishes==1,
       "close follows the real successful H264 finalizer return");
 Check(MainExportStopped(),"close follows recording-window, playback and capture teardown");
 Check(mainExport.app->SceneGeneration()==mainExport.scene,"close belongs to the starting scene");
 Check(mainExport.app->RenderWidth()==mainExport.savedWidth &&
       mainExport.app->RenderHeight()==mainExport.savedHeight,"close follows editor resolution restoration");
 if(mainExportSaveRetry)
   Check(!mainExport.app->SceneModified() && fs::path(mainExport.app->EnvFileName())==output/L"project-save/saved-project.pmm",
         "export requests close after the retried project save succeeds");
}
LRESULT CALLBACK MainExportWindow(HWND window,UINT message,WPARAM wp,LPARAM lp) {
 if(message==WM_CLOSE) {
   ++mainExport.closes;CheckMainExportClose();
   Check(mainExport.closes==1,"fixture receives no duplicate close");
   // This HWND was created by this isolated fixture. Never forward WM_CLOSE
   // to the production quit handler; it must remain alive for decoding/assertions.
   return 0;
 }
 return CallWindowProcW(mainExport.previous,window,message,wp,lp);
}
BOOL WINAPI MainExportPost(HWND window,UINT message,WPARAM wp,LPARAM lp) {
 if(window==mainExport.window && message==WM_CLOSE) {
   ++mainExport.posts;CheckMainExportClose();
   Check(mainExport.posts==1,"completion posts no duplicate close");
 }
 return mainExportRealPost(window,message,wp,lp);
}
// Defined with the no-MSAA observation helpers after the probe state below.
bool CheckMainExportNoMsaaState(const char* when,bool requireCapture);
bool MainExportWrite(DShowRecorder* recorder,const void* pixels,std::size_t bytes) {
 Need(mainExport.test && recorder==mainExport.app->Recorder(),"observe only the active fixture encoder");
 const auto& test=*mainExport.test;
 // Observe the live scene clock without replacing readback or encoder input.
 // This catches start-at-zero and 30 Hz stepping even if container timing looks OK.
 const double wanted=(test.first+double(mainExport.writes)*test.playbackFps/test.fps)/30.0;
 if(std::abs(mainExport.app->PlaybackCursorSeconds()-wanted)>=0.0001 ||
    std::abs(mainExport.app->DeltaTime()-1.0/test.fps)>=0.00001)
   std::printf("CLOCK case=%s sample=%u actual=%.9f expected=%.9f dt=%.9f\n",
       test.name,mainExport.writes,mainExport.app->PlaybackCursorSeconds(),wanted,mainExport.app->DeltaTime());
 mainExport.timelineOk &= std::abs(mainExport.app->PlaybackCursorSeconds()-wanted)<0.0001;
 mainExport.deltaOk &= std::abs(mainExport.app->DeltaTime()-1.0/test.fps)<0.00001;
 Check(mainExport.posts==0 && mainExport.closes==0,"no close while submitting output samples");
 if(mainExportNoMsaa) {
  const unsigned calls=mainExportDownsampleCalls;
  const bool fresh=calls>mainExportDownsampleAtWrite;
  mainExportDownsampleAtWrite=calls;
  std::printf("MAIN-EXPECT no-msaa write case=%s sample=%u downsample_calls=%u new_since_previous=%d\n",
      test.name,mainExport.writes,calls,fresh?1:0);
  Check(fresh,"each real no-MSAA write follows a new actual downsample call");
  CheckMainExportNoMsaaState("write",true);
 }
 const bool ok=mainExportRealWrite(recorder,pixels,bytes);
 Need(ok,"real H264 writer accepted the rendered sample");
 if(mainExport.writes==0 && test.destination==ExportDestination::Changed) {
   std::ofstream file(fs::path(mainExport.app->AviOutputPath()),std::ios::binary|std::ios::app);
   file << " edited during export";
   Need(file.good(),"a second writer changes the existing video during actual encoding");
 }
 ++mainExport.writes;return ok;
}
bool MainExportFinish(DShowRecorder* recorder,std::uint64_t expectedFrames,std::int32_t* failure,
                      std::wstring* retained) {
 Need(mainExport.test && recorder==mainExport.app->Recorder(),"finalize only the active fixture encoder");
 ++mainExport.finishes;
 const bool result=mainExportRealFinish(recorder,expectedFrames,failure,retained);
 if(retained)mainExport.retainedVideo=*retained;
 mainExport.finishReturned=true;mainExport.finishSucceeded=result;
 // Model a scene replacement during a finalizer callback, without replacing
 // the encoder, fabricating its result or changing the requested output range.
 if(mainExport.test->staleScene)mainExport.app->AdvanceSceneGeneration();
 return result;
}
int MainExportMessage(MMDApp* app,HWND owner,UiMessage message,UINT flags,const wchar_t* detail) {
 if(mainExport.allowCancel && app==mainExport.app && owner==app->RecordingWindow() &&
    message==UiMessage::StopRecording && (flags&MB_TYPEMASK)==MB_YESNO) {
   ++mainExport.confirmations;return IDYES;
 }
 return mainExportRealMessage(app,owner,message,flags,detail);
}

int WINAPI MainExportMessageBox(HWND owner,LPCWSTR text,LPCWSTR caption,UINT flags) {
 if(videoProjectSave.AcceptFailure(owner,text,caption,flags))return IDOK;
 if(MainExportMenuApproval() && caption && text) {
   if(std::wcscmp(caption,L"Confirm replacement")==0) {
     Need((flags&MB_TYPEMASK)==MB_YESNO && GetWindow(owner,GW_OWNER)==mainExport.window &&
          std::wcsstr(text,mainExport.confirmedPath.c_str()),"confirm the final MP4 in the actual settings dialog");
     ++mainExport.approvalPrompts;
     if(MainExportChangeDestination()) return IDNO;
     if(mainExport.test->destination==ExportDestination::ApprovalChanged) ReplaceApprovedVideo();
     return IDYES;
   }
   if(std::wcscmp(caption,L"Video export failed")==0) {
     if(mainExportProtectBackground) {
       Need(owner==mainExport.settingsWindow && std::wcsstr(text,mainExport.confirmedPath.c_str()) &&
            std::wcsstr(text,L"background video"),"the settings identify the protected background path");
       ++mainExport.conflictNotices;
       return IDOK;
     }
     Check(owner==mainExport.window && std::wcsstr(text,mainExport.confirmedPath.c_str()) &&
           std::wcsstr(text,L"changed after confirmation"),"the stale-approval message identifies the output");
     ++mainExport.conflictNotices;
     return IDOK;
   }
 }
 if(mainExport.test && DestinationConflict(mainExport.test->destination) &&
    owner==mainExport.window && caption && wcscmp(caption,L"H.264")==0) {
   ++mainExport.conflictNotices;
   Check(text && std::wcsstr(text,mainExport.app->AviOutputPath()),"conflict notice identifies the selected destination");
   if(mainExport.test->destination==ExportDestination::LateArrival)
     Check(text && !mainExport.retainedVideo.empty() && std::wcsstr(text,mainExport.retainedVideo.c_str()),
           "conflict notice identifies the preserved previous video");
   Check(MainExportStopped(),"conflict notice follows complete recording teardown");
   return IDOK;
 }
 return MessageW(owner,text,caption,flags);
}

// Forwarding observer for the real non-MSAA capture/downsample path. Only the
// pinned app/UI thread and fixture app are counted; the production call, its
// return value and LastError are preserved and nothing else is touched.
decltype(&DownsampleCaptureSurface) mainExportRealDownsample=nullptr;
bool MainExportObserveDownsample(MMDApp* app) {
 const bool result=mainExportRealDownsample(app);
 const DWORD returnedError=GetLastError();
 if(app==mainExport.app && GetCurrentThreadId()==mainExportD3DThread)++mainExportDownsampleCalls;
 SetLastError(returnedError);
 return result;
}
// Actual non-MSAA facts read from the GPU and the embedded presentation
// parameters. Nothing here is derived from the requested environment.
struct MainExportNoMsaaState {
 D3DPRESENT_PARAMETERS present{};
 D3DSURFACE_DESC back{},capture{};
 unsigned multisampleAvailable=1;
 bool backValid=false,capturePresent=false;
};
MainExportNoMsaaState MainExportNoMsaaActual(MMDApp* app) {
 MainExportNoMsaaState state;
 auto* renderer=app!=nullptr?app->Renderer():nullptr;
 if(renderer==nullptr)return state;
 state.present=renderer->presentParameters;
 state.multisampleAvailable=renderer->multisampleAvailable;
 // Prefer the surface production already acquired; only query a fresh back
 // buffer when a reset cleared it, and never keep the extra reference.
 if(renderer->backbufferSurface!=nullptr) {
  state.backValid=SUCCEEDED(renderer->backbufferSurface->GetDesc(&state.back));
 } else if(renderer->device!=nullptr) {
  ComPtr<IDirect3DSurface9> acquired;
  if(SUCCEEDED(renderer->device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&acquired)) && acquired.Get()!=nullptr)
   state.backValid=SUCCEEDED(acquired->GetDesc(&state.back));
 }
 if(renderer->captureSurface!=nullptr)
  state.capturePresent=SUCCEEDED(renderer->captureSurface->GetDesc(&state.capture));
 return state;
}
bool MainExportNoMsaaStateOk(const MainExportNoMsaaState& state,bool requireCapture) {
 const bool presentNone=state.present.MultiSampleType==D3DMULTISAMPLE_NONE &&
     state.present.MultiSampleQuality==0;
 const bool backNone=state.backValid &&
     state.back.MultiSampleType==D3DMULTISAMPLE_NONE && state.back.MultiSampleQuality==0;
 const bool captureNone=state.capturePresent &&
     state.capture.MultiSampleType==D3DMULTISAMPLE_NONE && state.capture.MultiSampleQuality==0;
 return state.multisampleAvailable==0 && presentNone && backNone &&
     (!requireCapture || state.capturePresent) && (state.capturePresent?captureNone:true);
}
void MainExportLogNoMsaaState(const char* when,const MainExportNoMsaaState& state) {
 std::printf("MAIN-EXPECT no-msaa state=%s present=%ux%u present_sample=%u present_quality=%lu "
     "back=%ux%u back_sample=%u back_quality=%lu back_valid=%d capture=%ux%u capture_sample=%u "
     "capture_quality=%lu capture_valid=%d multisample_available=%u\n",when,
     state.present.BackBufferWidth,state.present.BackBufferHeight,
     unsigned(state.present.MultiSampleType),state.present.MultiSampleQuality,
     state.back.Width,state.back.Height,unsigned(state.back.MultiSampleType),
     state.back.MultiSampleQuality,state.backValid?1:0,
     state.capture.Width,state.capture.Height,unsigned(state.capture.MultiSampleType),
     state.capture.MultiSampleQuality,state.capturePresent?1:0,state.multisampleAvailable);
}
bool CheckMainExportNoMsaaState(const char* when,bool requireCapture) {
 if(!mainExportNoMsaa || mainExport.app==nullptr)return true;
 const auto state=MainExportNoMsaaActual(mainExport.app);
 const bool ok=MainExportNoMsaaStateOk(state,requireCapture);
 if(!ok)MainExportLogNoMsaaState(when,state);
 char label[192]{};
 std::snprintf(label,sizeof label,"real no-MSAA path keeps actual NONE/0 GPU state %s",when);
 Check(ok,label);
 return ok;
}
void RequireMainExportNoMsaaInitialState(MMDApp* app) {
 const auto state=MainExportNoMsaaActual(app);
 MainExportLogNoMsaaState("after-init",state);
 Need(state.multisampleAvailable==0 &&
      state.present.MultiSampleType==D3DMULTISAMPLE_NONE &&
      state.present.MultiSampleQuality==0 &&
      state.backValid && state.back.MultiSampleType==D3DMULTISAMPLE_NONE &&
      state.back.MultiSampleQuality==0 &&
      state.capturePresent && state.capture.Width>0 && state.capture.Height>0 &&
      state.capture.MultiSampleType==D3DMULTISAMPLE_NONE &&
      state.capture.MultiSampleQuality==0,
      "real initialization selected the non-MSAA branch with NONE/0 presentation, backbuffer and capture target");
}
void RequireMainExportNoMsaaInitialization(MMDApp* app) {
 const auto* renderer=app!=nullptr?app->Renderer():nullptr;
 std::printf("MAIN-EXPECT no-msaa init factory=%p renderer_factory=%p hook_installed=%d denied=%u mismatch=%d\n",
     static_cast<void*>(mainExportFactory),
     renderer?static_cast<void*>(renderer->d3d9):nullptr,
     mainExportCapabilityHookInstalled?1:0,mainExportMsaaDenied,mainExportCapabilityMismatch?1:0);
 Need(mainExportCapabilityHookInstalled && !mainExportCapabilityMismatch &&
      mainExportRealCheckMultiSample!=nullptr && mainExportFactory!=nullptr &&
      renderer!=nullptr && renderer->d3d9==mainExportFactory,
      "install the required capability challenge on the exact factory initialization used");
 Need(mainExportMsaaDenied>0,
      "the forced capability challenge denied at least one real non-NONE multisample query");
 if(mainExportCheckMultiSampleTarget!=nullptr) {
  const MH_STATUS stopped=MH_DisableHook(reinterpret_cast<void*>(mainExportCheckMultiSampleTarget));
  std::printf("MAIN-EXPECT no-msaa capability-injection=stopped status=%d\n",static_cast<int>(stopped));
  Check(stopped==MH_OK,"stop capability injection immediately after initialization");
 }
 RequireMainExportNoMsaaInitialState(app);
}

void RequireMainExportDesktop(const wchar_t* requested) {
 wchar_t actual[256]{};DWORD needed=0;BOOL input=TRUE;
 const auto desktop=GetThreadDesktop(GetCurrentThreadId());
 Need(desktop && GetUserObjectInformationW(desktop,UOI_NAME,actual,sizeof actual,&needed),
      "query inherited private desktop before creating any window or GPU device");
 Need(requested[0] && _wcsicmp(requested,L"Default")!=0 && std::wcscmp(actual,requested)==0,
      "inherited desktop exactly matches the supervisor-owned non-default desktop");
 Need(GetUserObjectInformationW(desktop,UOI_IO,&input,sizeof input,&needed) && !input,
      "private desktop is not the input desktop; visible execution is forbidden");
 std::printf("MAIN-EXPORT private-desktop=%ls pid=%lu\n",actual,GetCurrentProcessId());
}
// Explicit hidden-window control, never a fallback from private-desktop mode.
// A non-input desktop can reject D3D9 before any export code runs. This mode
// keeps the inherited desktop and prevents fixture windows from showing or
// activating, while preserving actual renderer and encoder behavior.
decltype(&SetWindowPos) mainExportRealSetWindowPos=nullptr;
bool MainExportOwnWindow(HWND window) {
 DWORD process=0;GetWindowThreadProcessId(window,&process);
 return window && process==GetCurrentProcessId();
}
BOOL WINAPI MainExportHiddenShow(HWND window,int) {
 Need(MainExportOwnWindow(window),"visibility guard only operates on fixture-owned windows");
 return realShow(window,SW_HIDE);
}
BOOL WINAPI MainExportHiddenPosition(HWND window,HWND after,int x,int y,int width,int height,UINT flags) {
 Need(MainExportOwnWindow(window),"position guard only operates on fixture-owned windows");
 return mainExportRealSetWindowPos(window,after,x,y,width,height,(flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE);
}
BOOL WINAPI MainExportNoForeground(HWND) {return FALSE;}
HWND WINAPI MainExportNoActivation(HWND) {return nullptr;}
void RequireMainExportHiddenDesktop(const wchar_t* requested) {
 wchar_t actual[256]{};DWORD needed=0;
 const auto desktop=GetThreadDesktop(GetCurrentThreadId());
 Need(desktop && GetUserObjectInformationW(desktop,UOI_NAME,actual,sizeof actual,&needed),
      "read explicit hidden-window control desktop");
 Need(requested[0] && std::wcscmp(actual,requested)==0,
      "hidden-window control stays on the exact explicitly requested desktop");
 std::printf("MAIN-EXPORT hidden-control-desktop=%ls pid=%lu no-desktop-switch=1\n",actual,GetCurrentProcessId());
}
void MainExportHiddenWindowCheck() {
 bool hidden=true;
 // Coordinator scheduling availability is a fixture hook, so it says nothing
 // about real HWND visibility. Hidden-window control is judged by the unhooked API.
 const auto enumerated=EnumWindows([](HWND window,LPARAM param)->BOOL {
   const auto visible=coordinator_fixture::originalVisible?coordinator_fixture::originalVisible:&IsWindowVisible;
   if(MainExportOwnWindow(window) && visible(window))*reinterpret_cast<bool*>(param)=false;
   return TRUE;
 },reinterpret_cast<LPARAM>(&hidden));
 Need(enumerated!=FALSE,"enumerate fixture-owned top-level windows for hidden-window control");
 Need(hidden,"all fixture-owned top-level windows remain hidden");
}
void BootstrapMainExportDesktop(const wchar_t* requested) {
 HANDLE token=nullptr;
 if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
   DWORD bytes=0;GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&bytes);
   std::vector<unsigned char> data(bytes);
   if(bytes && GetTokenInformation(token,TokenIntegrityLevel,data.data(),bytes,&bytes)) {
     const auto* label=reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(data.data());
     const auto count=*GetSidSubAuthorityCount(label->Label.Sid);
     if(count)std::printf("MAIN-EXPORT integrity=%lu\n",*GetSidSubAuthority(label->Label.Sid,count-1));
   }
   CloseHandle(token);
 }
 Need(requested[0] && _wcsicmp(requested,L"Default")!=0,"bootstrap requires a named private desktop");
 unsigned windows=0;
 Need(EnumThreadWindows(GetCurrentThreadId(),[](HWND,LPARAM value)->BOOL {
   ++*reinterpret_cast<unsigned*>(value);return TRUE;
 },reinterpret_cast<LPARAM>(&windows))!=FALSE && windows==0,
      "bootstrap main thread has no windows before switching desktop");
 // A low-integrity test may not open a medium-integrity supervisor's desktop.
 // Create its own resource without changing either token or any object ACL.
 const auto desktop=CreateDesktopW(requested,nullptr,nullptr,0,GENERIC_ALL,nullptr);
 if(!desktop) {
   const auto error=GetLastError();wchar_t station[256]{};DWORD needed=0;
   GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof station,&needed);
   std::printf("MAIN-EXPORT CreateDesktop error=%lu station=%ls requested=%ls\n",error,station,requested);
 }
 Need(desktop!=nullptr,"create the dedicated desktop at the test process integrity level");
 if(!SetThreadDesktop(desktop)) {
   const auto error=GetLastError();CloseDesktop(desktop);SetLastError(error);
   Need(false,"switch test thread before creating GUI; no visible fallback");
 }
 // Keep this thread's desktop handle until process exit. The supervisor owns
 // the lifetime and verifies release after every child GUI/MF thread exits.
 RequireMainExportDesktop(requested);
 const std::string expected(requested,requested+std::wcslen(requested));
 const auto ready=fs::current_path()/L"desktop-ready.txt";
 const auto held=fs::current_path()/L"desktop-held.txt";
 Need(!fs::exists(ready) && !fs::exists(held),"desktop handshake files are fresh");
 {std::ofstream file(ready);file<<expected;file.close();Need(bool(file),"announce the private desktop to the supervisor");}
 const auto deadline=GetTickCount64()+10000;
 while(!fs::exists(held) && GetTickCount64()<deadline)Sleep(10);
 std::ifstream receipt(held);std::string actual;receipt>>actual;
 Need(bool(receipt) && actual==expected,"supervisor holds the exact desktop before test GUI starts");
}
void RequireMainExportEnvironment(const fs::path& root) {
 wchar_t exe[32768]{};
 const auto length=GetModuleFileNameW(nullptr,exe,32768);
 Need(length && length<32768 && fs::equivalent(fs::path(exe).parent_path(),root),
      "executable and cwd must be in the same fresh isolated fixture directory");
 Need(fs::is_regular_file(root/L"Data"/L"mmconfig.ini"),"supervisor supplies isolated Data/mmconfig.ini");
 for(const auto* name:{L"MMEffect.dll",L"MMHack.dll",L"d3d9.dll"})
   Need(!fs::exists(root/name),"fixture directory contains no MME or D3D9 proxy");
 for(const auto* name:{L"LOCALAPPDATA",L"APPDATA",L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG",L"MMS_ANTIALIAS_CONFIG"}) {
   wchar_t value[32768]{};
   const auto size=GetEnvironmentVariableW(name,value,32768);
   Need(size && size<32768,"supervisor supplies explicit private config paths");
   const fs::path path(value);
   const auto relative=fs::weakly_canonical(path).lexically_relative(fs::weakly_canonical(root));
   Need(path.is_absolute() && !relative.empty() && relative!=L"." &&
        *relative.begin()!=L"..","config path stays below the isolated fixture directory");
 }
 for(const auto* name:{"PROBE_REQUIRE_MME","PROBE_CAMERA_MODES","PROBE_AA","PROBE_SCALE","PROBE_RESET_BLOCKER","PROBE_DPI_CHANGE"})
   Need(!std::getenv(name),"MAIN-EXPORT must not inherit unrelated PROBE overrides");
 Need(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"),"MAIN-EXPORT does not load real MME");
}
// Absent PROBE_MAIN_EXPORT_VIDEO keeps the existing picture-only profile. A
// present value must be a nonempty absolute existing .mp4 whose canonical path
// stays below the isolated fixture root; invalid values fail before the app,
// its window or any D3D device is created. The production 256-wchar media-path
// buffer also bounds the accepted length.
bool MainExportVideoEnvironment(std::wstring& value) {
 const wchar_t* name=L"PROBE_MAIN_EXPORT_VIDEO";
 const DWORD size=GetEnvironmentVariableW(name,nullptr,0);
 if(size==0) {
  Need(GetLastError()==ERROR_ENVVAR_NOT_FOUND,"read PROBE_MAIN_EXPORT_VIDEO without an unexpected error");
  return false;
 }
 value.resize(size);
 const DWORD written=GetEnvironmentVariableW(name,value.data(),size);
 Need(written>0 && written<size,"read the complete PROBE_MAIN_EXPORT_VIDEO value");
 value.resize(written);
 return true;
}
void ConfigureMainExportBackground(const fs::path& root) {
 std::wstring requested;
 if(!MainExportVideoEnvironment(requested))return;
 Need(!requested.empty(),"PROBE_MAIN_EXPORT_VIDEO must not be empty");
 Need(requested.size()<256,"PROBE_MAIN_EXPORT_VIDEO must fit the production 256-wchar media path");
 const fs::path candidate(requested);
 Need(candidate.is_absolute(),"PROBE_MAIN_EXPORT_VIDEO must be an absolute path");
 const auto extension=candidate.extension();
 Need(_wcsicmp(extension.c_str(),L".mp4")==0,"PROBE_MAIN_EXPORT_VIDEO must name an .mp4 file");
 std::error_code error;
 const bool regular=fs::is_regular_file(candidate,error);
 Need(!error && regular,"PROBE_MAIN_EXPORT_VIDEO must name an existing regular file");
 const auto canonical=fs::weakly_canonical(candidate,error);
 Need(!error && !canonical.empty(),"PROBE_MAIN_EXPORT_VIDEO must have a usable canonical path");
 const auto canonicalRoot=fs::weakly_canonical(root,error);
 Need(!error,"PROBE_MAIN_EXPORT_VIDEO root must have a usable canonical path");
 const auto relative=canonical.lexically_relative(canonicalRoot);
 Need(!relative.empty() && relative!=L"." && *relative.begin()!=L"..",
      "PROBE_MAIN_EXPORT_VIDEO must stay below the isolated fixture root");
 mainExportVideoPath=canonical.wstring();
 Need(mainExportVideoPath.size()<256,"PROBE_MAIN_EXPORT_VIDEO canonical path must fit the 256-wchar media path");
}
fs::path MainExportPicture() {
 const auto path=output/L"orientation.bmp";
 constexpr int width=640,height=360;
 BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
 file.bfType=0x4d42;file.bfOffBits=sizeof file+sizeof info;
 file.bfSize=file.bfOffBits+width*height*4;
 info.biSize=sizeof info;info.biWidth=width;info.biHeight=-height;
 info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
 const DWORD colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
 std::vector<DWORD> pixels(width*height);
 for(int y=0;y<height;++y)for(int x=0;x<width;++x)
   pixels[std::size_t(y)*width+x]=colors[(y>=height/2?2:0)+(x>=width/2?1:0)];
 std::ofstream stream(path,std::ios::binary);
 stream.write(reinterpret_cast<const char*>(&file),sizeof file);
 stream.write(reinterpret_cast<const char*>(&info),sizeof info);
 stream.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*sizeof(DWORD));
 stream.close();Need(bool(stream),"write top-down red/green over blue/white fixture BMP");
 return path;
}
// Video-background facts kept outside the picture oracle: the exact loaded
// path, the real modern-decoder identity, the enabled source and the explicit
// fill-width placement. SceneModified is captured per case as a plain value.
struct MainExportVideoState {
 std::wstring path;
 bool hasVideo=false,enabled=false,pictureDisabled=false,modernHandlesClear=false;
 int width=0,height=0;
 std::int32_t offsetX=0,offsetY=0;
 float scale=0;
};
MainExportVideoState mainExportVideoExpected;
MainExportVideoState MainExportVideoActual(MMDApp* app) {
 MainExportVideoState state;
 state.path=app->AviBackgroundPath();
 state.hasVideo=media::HasVideo(app);
 state.enabled=app->AviBackgroundEnabled()!=0;
 state.pictureDisabled=app->PictureBackgroundEnabled()==0;
 state.modernHandlesClear=app->AviFile()==nullptr && app->AviStream()==nullptr &&
     app->AviFrameReader()==nullptr;
 state.width=app->AviFrameWidth();state.height=app->AviFrameHeight();
 state.offsetX=app->AviOffsetX();state.offsetY=app->AviOffsetY();
 state.scale=app->AviScale();
 return state;
}
bool MainExportVideoStateOk(const MainExportVideoState& state) {
 return state.path==mainExportVideoExpected.path && state.hasVideo && state.enabled &&
     state.pictureDisabled && state.modernHandlesClear && state.width==64 && state.height==48 &&
     state.offsetX==0 && state.offsetY==0 &&
     std::memcmp(&state.scale,&mainExportVideoExpected.scale,sizeof(float))==0;
}
void MainExportLogVideoState(const char* when,const MainExportVideoState& state) {
 std::printf("MAIN-EXPECT video-state when=%s path=%ls has_video=%d enabled=%d picture_disabled=%d "
     "modern_handles_clear=%d size=%dx%d offset=%d,%d scale=%.9g scene_generation=%llu scene_modified=%u\n",
     when,state.path.c_str(),state.hasVideo?1:0,state.enabled?1:0,state.pictureDisabled?1:0,
     state.modernHandlesClear?1:0,state.width,state.height,state.offsetX,state.offsetY,state.scale,
     static_cast<unsigned long long>(mainExport.app?mainExport.app->SceneGeneration():0),
     mainExport.app?static_cast<unsigned>(mainExport.app->SceneModified()):0u);
}
void CheckMainExportVideoState(const char* when) {
 if(mainExportVideoPath.empty())return;
 const auto state=MainExportVideoActual(mainExport.app);
 const bool ok=MainExportVideoStateOk(state);
 if(!ok)MainExportLogVideoState(when,state);
 char label[192]{};
 std::snprintf(label,sizeof label,"video source, decoder and placement stay preserved %s",when);
 Check(ok,label);
}
void CheckMainExportVideoSceneModified(const char* when) {
 if(mainExportVideoPath.empty())return;
 const auto actual=static_cast<std::uint8_t>(mainExport.app->SceneModified());
 const bool ok=actual==mainExport.videoSceneModified;
 if(!ok)std::printf("MAIN-EXPECT video scene_modified when=%s case=%s started=%u now=%u\n",
     when,mainExport.test?mainExport.test->name:"?",unsigned(mainExport.videoSceneModified),unsigned(actual));
 char label[192]{};
 std::snprintf(label,sizeof label,"video case keeps its starting SceneModified value %s",when);
 Check(ok,label);
}
MFVideoArea MainExportDisplayArea(IMFMediaType* type,UINT32& width,UINT32& height) {
 GUID format{};
 Need(SUCCEEDED(MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&width,&height)) &&
      SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE,&format)) && format==MFVideoFormat_RGB32,
      "read decoded RGB geometry");
 Need(MFGetAttributeUINT32(type,MF_MT_PAN_SCAN_ENABLED,FALSE)==FALSE,"fixture does not enable pan and scan");
 MFVideoArea area{};UINT32 bytes=0;
 const HRESULT hr=type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,reinterpret_cast<UINT8*>(&area),sizeof area,&bytes);
 // The decoder may pad coded height to a macroblock boundary. Its published
 // display aperture, not a hardcoded crop, identifies the valid picture.
 Need((SUCCEEDED(hr)&&bytes==sizeof area)||hr==MF_E_ATTRIBUTENOTFOUND,"read valid display aperture or an exact-size frame");
 if(hr==MF_E_ATTRIBUTENOTFOUND)area.Area={LONG(width),LONG(height)};
 Need(area.OffsetX.fract==0 && area.OffsetY.fract==0 && area.OffsetX.value>=0 && area.OffsetY.value>=0 &&
      area.Area.cx==640 && area.Area.cy==360 &&
      UINT32(area.OffsetX.value)+640<=width && UINT32(area.OffsetY.value)+360<=height,
      "decoded display aperture is exactly 640x360 within the coded buffer");
 return area;
}
bool MainExportOrientation(IMFSample* sample,IMFMediaType* type,bool capture,
    const MainExportCase* videoCase=nullptr,unsigned sampleIndex=0) {
 UINT32 width=0,height=0;const auto area=MainExportDisplayArea(type,width,height);
 ComPtr<IMFMediaBuffer> buffer;ComPtr<IMF2DBuffer> twoD;
 Need(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)),"access decoded pixels");
 BYTE* data=nullptr;LONG pitch=0;DWORD size=0;
 const bool has2D=SUCCEEDED(buffer.As(&twoD));
 if(has2D)Need(SUCCEEDED(twoD->Lock2D(&data,&pitch)),"lock decoded top scanline");
 else {
   UINT32 stride=0;
   if(SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&stride)))pitch=static_cast<LONG>(stride);
   else Need(SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,width,&pitch)),
             "resolve the decoder's standard RGB stride");
   Need(SUCCEEDED(buffer->Lock(&data,nullptr,&size)),"lock decoded RGB buffer");
   Need(std::abs(LONGLONG(pitch))*height<=size,"decoded RGB buffer is complete");
   if(pitch<0)data+=std::size_t(-LONGLONG(pitch))*(height-1);
 }
 Need(std::abs(LONGLONG(pitch))>=LONGLONG(width)*4,"decoded scanline contains the full coded width");
 data+=std::ptrdiff_t(area.OffsetY.value)*pitch+area.OffsetX.value*4;
 if(capture) {
   BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
   file.bfType=0x4d42;file.bfOffBits=sizeof file+sizeof info;file.bfSize=file.bfOffBits+640*360*4;
   info.biSize=sizeof info;info.biWidth=640;info.biHeight=-360;info.biPlanes=1;info.biBitCount=32;
   std::ofstream image(output/(std::string(mainExport.test->name)+"-decoded.bmp"),std::ios::binary);
   image.write(reinterpret_cast<const char*>(&file),sizeof file);image.write(reinterpret_cast<const char*>(&info),sizeof info);
   for(int y=0;y<360;++y)image.write(reinterpret_cast<const char*>(data+std::ptrdiff_t(y)*pitch),640*4);
   image.close();Need(bool(image),"retain the actual first decoded visible frame");
 }
 bool ok=true;
 if(videoCase) {
   // Independent dynamic oracle: only the immutable case and the decoded
   // sample index fix the source time. No production clock, writer counter or
   // decoded PTS is consulted, and only the 30 Hz integer-second band is
   // claimed, not exact frame precision.
   const double expectedSeconds=double(videoCase->first)/30.0+double(sampleIndex)/videoCase->fps;
   const bool green=static_cast<long long>(std::floor(expectedSeconds+1e-9))%2!=0;
   const char* band=green?"green":"red";
   for(int quadrant=0;quadrant<4;++quadrant) {
     const int cx=(quadrant%2)?480:160,cy=(quadrant/2)?270:90;
     int b=0,g=0,r=0;
     for(int y=cy-8;y<cy+8;++y)for(int x=cx-8;x<cx+8;++x) {
       const auto* pixel=data+std::ptrdiff_t(y)*pitch+x*4;
       b+=pixel[0];g+=pixel[1];r+=pixel[2];
     }
     b/=256;g/=256;r/=256;
     // The known 64x48 fixture keeps its 4:3 aspect and fills the capture
     // width as 640x480 at offset 0. The 16x16 squares at y=90/270 average
     // source rows around 9/27, so quadrants 0/1 are the red-or-green top half
     // and 2/3 the always-blue bottom half.
     const bool top=quadrant<2;
     const bool match=top?(green?g>r+60&&g>b+60:r>g+60&&r>b+60):b>r+60&&b>g+60;
     if(!match)std::printf("VIDEO_CONTENT case=%s sample=%u source_seconds=%.9f band=%s quadrant=%d rgb=%d,%d,%d\n",
         videoCase->name,sampleIndex,expectedSeconds,band,quadrant,r,g,b);
     ok &= match;
   }
 } else {
   for(int quadrant=0;quadrant<4;++quadrant) {
     const int cx=(quadrant%2)?480:160,cy=(quadrant/2)?270:90;
     int b=0,g=0,r=0;
     for(int y=cy-8;y<cy+8;++y)for(int x=cx-8;x<cx+8;++x) {
       const auto* pixel=data+std::ptrdiff_t(y)*pitch+x*4;
       b+=pixel[0];g+=pixel[1];r+=pixel[2];
     }
     b/=256;g/=256;r/=256;
     const bool match=quadrant==0?r>g+60&&r>b+60:
         quadrant==1?g>r+60&&g>b+60:quadrant==2?b>r+60&&b>g+60:
         r>180&&g>180&&b>180&&std::abs(r-b)<35&&std::abs(g-b)<35;
     if(!match)std::printf("ORIENTATION quadrant=%d rgb=%d,%d,%d\n",quadrant,r,g,b);
     ok &= match;
   }
 }
 if(has2D)twoD->Unlock2D();else buffer->Unlock();
 return ok;
}
void DecodeMainExport(const fs::path& path,const MainExportCase& test) {
 // A fresh sequential reader counts ACTUAL decoded samples through EOF. No
 // production OpenVideo/VideoFrame cache, writer counters, seeking or clamping.
 ComPtr<IMFAttributes> attributes;ComPtr<IMFSourceReader> reader;
 Need(SUCCEEDED(MFCreateAttributes(&attributes,2)),"create independent decoder attributes");
 Need(SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE)) &&
      SUCCEEDED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,FALSE)),"use system software decoding");
 Need(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(),attributes.Get(),&reader)),"open finalized MP4 independently");
 Need(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE)) &&
      SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE)),"select encoded video stream");
 ComPtr<IMFMediaType> native;GUID subtype{};UINT32 numerator=0,denominator=0;
 Need(SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&native)),"read actual container video type");
 Check(SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE,&subtype)) && subtype==MFVideoFormat_H264,"output really contains H264");
 UINT32 encodedWidth=0,encodedHeight=0;
 Check(SUCCEEDED(MFGetAttributeSize(native.Get(),MF_MT_FRAME_SIZE,&encodedWidth,&encodedHeight)) &&
       encodedWidth==640 && encodedHeight==360,"container declares the requested 640x360 picture");
 Check(SUCCEEDED(MFGetAttributeRatio(native.Get(),MF_MT_FRAME_RATE,&numerator,&denominator)) &&
       denominator && std::abs(double(numerator)/denominator-test.fps)<0.001,"container declares requested FPS");
 PROPVARIANT duration{};
 Need(SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&duration)) &&
      duration.vt==VT_UI8,"read finalized container duration");
 const double seconds=duration.uhVal.QuadPart/10000000.0;PropVariantClear(&duration);
 // 2.5 ms allows MF/MP4 timescale rounding, but is below a single 60 Hz frame.
 constexpr double tolerance=0.0025;
 // With AAC, the presentation duration can include a final padded packet.
 const double containerTolerance=mainExportWave && test.includeWave ? 1024.0/48000+tolerance : tolerance;
 Check(std::abs(seconds-test.duration)<containerTolerance,"container duration matches independent full-sample expectation");
 ComPtr<IMFMediaType> request;
 Need(SUCCEEDED(MFCreateMediaType(&request)),"create decoded RGB request");
 Need(SUCCEEDED(request->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video)) &&
      SUCCEEDED(request->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32)) &&
      SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,request.Get())),"decode H264 to RGB32");
 unsigned decoded=0;bool eof=false,timing=true,orientation=true;double sampleEnd=0;
 for(unsigned reads=0;reads<256 && !eof;++reads) {
   ComPtr<IMFSample> sample;DWORD flags=0;LONGLONG timestamp=0;
   Need(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&timestamp,&sample)) &&
        !(flags&MF_SOURCE_READERF_ERROR),"sequential H264 decoding succeeds");
   if(sample) {
     ComPtr<IMFMediaType> actual;UINT32 width=0,height=0;GUID format{};
     Need(SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&actual)) &&
          SUCCEEDED(MFGetAttributeSize(actual.Get(),MF_MT_FRAME_SIZE,&width,&height)) &&
          SUCCEEDED(actual->GetGUID(MF_MT_SUBTYPE,&format)),"read the actual decoded media type");
     if(decoded==0 || format!=MFVideoFormat_RGB32)
       std::printf("DECODED_TYPE case=%s width=%u height=%u subtype=%08lx flags=%08lx\n",test.name,width,height,format.Data1,flags);
     Need(format==MFVideoFormat_RGB32,"decoded frames retain RGB32 format");
     LONGLONG span=0;
     Need(SUCCEEDED(sample->GetSampleDuration(&span)) && span>0,"decoded sample has a real duration");
     const double time=timestamp/10000000.0,step=span/10000000.0;
     if(std::abs(time-double(decoded)/test.fps)>=tolerance || std::abs(step-1.0/test.fps)>=tolerance)
       std::printf("TIMESTAMP case=%s sample=%u time=%.7f duration=%.7f\n",test.name,decoded,time,step);
     timing &= std::abs(time-double(decoded)/test.fps)<tolerance && std::abs(step-1.0/test.fps)<tolerance;
     sampleEnd=time+step;
     orientation &= MainExportOrientation(sample.Get(),actual.Get(),decoded==0,
         mainExportVideoPath.empty()?nullptr:&test,decoded);
     ++decoded;
   }
   eof=(flags&MF_SOURCE_READERF_ENDOFSTREAM)!=0;
 }
 Check(eof,"decode reaches actual EOF within the bounded sample budget");
 Check(decoded==unsigned(test.samples),"actual decoded sample count matches independent oracle");
 Check(timing && std::abs(sampleEnd-test.duration)<tolerance,
       "decoded timestamps start at zero, follow output FPS and end at expected duration");
 Check(orientation && decoded!=0,mainExportVideoPath.empty()?
     "all decoded frames keep red/green above blue/white without mirroring":
     "all decoded frames match the independently expected dynamic 30 Hz time band above blue");
 std::printf("MAIN-EXPORT case=%s decoded=%u expected=%d duration=%.7f expected_duration=%.7f sample_end=%.7f file=%ls\n",
     test.name,decoded,test.samples,seconds,test.duration,sampleEnd,path.c_str());
}
void RunMainExportCase(MMDApp* app,const MainExportCase& test) {
 Pump();
 if(mainExportNoMsaa){mainExportDownsampleCalls=0;mainExportDownsampleAtWrite=0;}
 mainExport.test=&test;mainExport.writes=mainExport.finishes=mainExport.posts=mainExport.closes=mainExport.confirmations=0;
 mainExport.conflictNotices=0;
 mainExport.lateArrivals=0;
 mainExport.pickerCalls=mainExport.settingsCalls=mainExport.approvalPrompts=mainExport.encoderStarts=0;
 mainExport.alternatePicks=0;
 mainExport.settingsWindow=nullptr;
 mainExport.retainedVideo.clear();
 mainExport.finishReturned=mainExport.finishSucceeded=mainExport.allowCancel=false;
 mainExport.timelineOk=mainExport.deltaOk=true;mainExport.scene=app->SceneGeneration();
 if(!mainExportVideoPath.empty()) {
  mainExport.videoSceneModified=static_cast<std::uint8_t>(app->SceneModified());
  MainExportLogVideoState("case-start",MainExportVideoActual(app));
  CheckMainExportVideoState("before-start");
 }
 mainExport.savedWidth=app->RenderWidth();mainExport.savedHeight=app->RenderHeight();
 phase=test.name;std::printf("MAIN-EXPORT begin=%s\n",test.name);
 const bool conflict=DestinationConflict(test.destination);
 const bool late=test.destination==ExportDestination::LateArrival;
 const bool lateRetry=test.destination==ExportDestination::LateRetry;
 const bool approvalEarly=test.destination==ExportDestination::ApprovalChanged;
 const bool approvalLate=test.destination==ExportDestination::ApprovalLate;
 const bool approvalRetry=test.destination==ExportDestination::ApprovalRetry;
 const bool retry=test.destination==ExportDestination::Retry || lateRetry || approvalRetry;
 const auto path=output/(approvalEarly || approvalRetry ? "approval-target.mp4" :
     late || lateRetry ? "late-target.mp4" :
     conflict || retry ? "changed-target.mp4" : std::string(test.name)+".mp4");
 Need(fs::exists(path)==retry && path.native().size()<256,"write only a short path owned by this fixture");
 if(conflict || approvalEarly || approvalLate) {
   std::ofstream old(path,std::ios::binary);
   old << "existing video";
   Need(old.good(),"create the existing video for the conflict case");
 }
 wcscpy_s(app->AviOutputPath(),256,path.c_str());
 if(mainExportWave) app->PlaybackTiming().playbackFps=test.playbackFps;
 app->Recorder()->selectedCodec=kBuiltInH264Codec;
 app->AviOutputWidth()=640;app->AviOutputHeight()=360;
 app->AviRecordStartFrame()=test.first;app->AviRecordEndFrame()=test.last;app->AviRecordFps()=float(test.fps);
 app->AviIncludeWave()=0;app->AviCloseAfterExport()=test.close?1:0;
 // Deliberately seek from a different editor frame; the recording starter must
 // initialize the nonzero first pose before its first readback.
 app->CurrentFrame()=0;app->PlaybackCursorSeconds()=0;
 const auto describePlacement=[&](const char* when) {
   const auto rect=app->ViewportRect();
   std::printf("PICTURE_PLACEMENT case=%s when=%s scale=%.9g ratio=%.9g picture=%dx%d offset=%d,%d viewport=%ld,%ld,%ld,%ld render=%dx%d buffer=%dx%d msaa=%u\n",
     test.name,when,app->PictureScale(),app->Renderer()->viewScale,app->PictureWidth(),app->PictureHeight(),
     app->PictureOffsetX(),app->PictureOffsetY(),rect.left,rect.top,rect.right,rect.bottom,app->RenderWidth(),app->RenderHeight(),
     app->Renderer()->screenWidth,app->Renderer()->screenHeight,unsigned(app->Renderer()->multisampleAvailable));
 };
 describePlacement("before-start");
 if(mainExportNoMsaa)CheckMainExportNoMsaaState("before-start",false);
 if(MainExportMenuApproval()) {
   app->EnglishUI()=1;
   const bool change=MainExportChangeDestination();
   mainExport.alternatePath=path;
   mainExport.confirmedPath=mainExportProtectBackground?fs::path(mainExportVideoPath):change?output/L"previous-video.mp4":path;
   if(change && !mainExportProtectBackground) {
     Need(!fs::exists(mainExport.confirmedPath),"own the existing video fixture path");
     std::ofstream old(mainExport.confirmedPath,std::ios::binary);
     old << "existing video";
     Need(old.good(),"seed the old video before the menu opens");
   }
   mainExport.pickerPath=mainExport.confirmedPath;
   if(approvalEarly || mainExportProtectBackground) mainExport.pickerPath.replace_extension(L".avi");
   SendMessageW(mainExport.window,WM_COMMAND,223,0);
   Need(mainExport.pickerCalls==1 && mainExport.settingsCalls==1 &&
        mainExport.approvalPrompts==(mainExportProtectBackground?0u:change?2u:1u),
        "video menu uses one path chooser, real settings and the expected overwrite confirmations");
   if(approvalEarly || approvalLate) {
     Check(MainExportStopped() && mainExport.encoderStarts==unsigned(approvalLate) &&
           mainExport.writes==0 && mainExport.finishes==0 && mainExport.posts==0 && mainExport.closes==0 &&
           mainExport.conflictNotices==1,"stale approval aborts before writing and leaves the editor open");
     auto previous=path;
     previous += L".previous";
     std::ifstream current(path,std::ios::binary),old(previous,std::ios::binary);
     const std::string currentBytes(std::istreambuf_iterator<char>(current),{});
     const std::string oldBytes(std::istreambuf_iterator<char>(old),{});
     Check(currentBytes=="new video after approval" && oldBytes=="existing video",
           "both files remain byte-identical when the confirmed destination changes");
     if(approvalEarly) Check(!fs::exists(mainExport.pickerPath),"confirmation and failure apply to MP4, not the AVI spelling");
     std::printf("MAIN-EXPORT PASS stale approval refused at %s\n",approvalLate?"encoder entry":"start");
     return;
   }
   Check(mainExport.encoderStarts==1,"a new approval allows the encoder to start exactly once");
 } else if(mainExportWave || mainExportSaveRetry) {
   app->EnglishUI()=1;
   mainExport.pickerPath=path;
   SendMessageW(mainExport.window,WM_COMMAND,223,0);
   Need(mainExport.pickerCalls==1 && mainExport.settingsCalls==1,"export uses the real video menu and settings");
 } else {
   StartAviRecordWindow(app);
 }
 describePlacement("after-start");
 Need(app->RecordingWindow() && IsBuiltInH264Recording(app->Recorder()),"real windowed H264 export started");
 if(mainExportNoMsaa)CheckMainExportNoMsaaState("after-start",true);
 Need(mainExport.posts==0 && mainExport.closes==0,"starting export never requests close");
 const HWND recording=app->RecordingWindow();
 for(int step=0;step<180 && app->RecordingWindow();++step) {
   if(test.cancel) {
     // Coordinator wake messages dispatched here enter the real FrameDriver, so
     // one ordinary pump can walk the export through all natural samples. Stop
     // peeking at the exact third accepted sample: never consume the message
     // after that boundary, and never re-enter cancellation from the writer.
     MSG message{};
     for(int pump=0;pump<100 && app->RecordingWindow() && mainExport.writes<3 &&
         PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++pump) {
       TranslateMessage(&message);
       DispatchMessageW(&message);
     }
     // Same tail as Frame(app), minus its unconditional pump: a wake for the
     // next due frame may still be pending.
     if(app->RecordingWindow() && mainExport.writes<3) {
       app->DeltaTime()=1.f/30;app->state.messageSeen=1;
       coordinator_fixture::Drive(app);
     }
     if(mainExport.writes>=3) {
       Need(mainExport.writes==3,"cancel observes exactly three accepted samples at the precise sampling boundary");
       Need(app->RecordingWindow()==recording,"cancel after three actual samples while export is still active");
       std::printf("MAIN-EXPORT cancel case=%s samples=%u step=%d expected=3\n",test.name,mainExport.writes,step);
       // Pump and driver have both returned; deliver the real escape now so
       // cancellation never re-enters the encoder or FrameDriver.
       mainExport.allowCancel=true;
       SendMessageW(recording,WM_KEYDOWN,VK_ESCAPE,0);
       mainExport.allowCancel=false;
       break;
     }
     continue;
   }
   Frame(app);
 }
 Need(MainExportStopped(),"production completion/cancellation performs full recording teardown");
 if(mainExportNoMsaa)CheckMainExportNoMsaaState("after-stop",false);
 if(!mainExportVideoPath.empty()) {
  CheckMainExportVideoState("after-stop");
  CheckMainExportVideoSceneModified("after-stop");
 }
 Check(!IsWindow(recording),"production destroys its recording window");
 Check(mainExport.writes==unsigned(test.samples),"real accepted samples agree with the independent case count");
 if(mainExportNoMsaa)std::printf("MAIN-EXPECT no-msaa case=%s downsample_calls=%u writes=%u\n",test.name,mainExportDownsampleCalls,mainExport.writes);
 Check(mainExport.timelineOk && mainExport.deltaOk,"live scene starts at the requested first timeline frame and advances at the requested output FPS");
 Check(mainExport.finishes==1 && mainExport.finishReturned && mainExport.finishSucceeded==(!test.cancel && !conflict),
       "real finalization reports complete only for the full requested output");
 Check(mainExport.confirmations==unsigned(test.cancel),"cancel traverses the real recording-window confirmation path");
 // Drive idle frames as well as the queue: catch deferred/duplicate close and
 // accidental continued recording, instead of checking only immediately after finish.
 for(int idle=0;idle<8;++idle)Frame(app);
 Pump();
 if(mainExportNoMsaa)CheckMainExportNoMsaaState("after-restore",false);
 if(!mainExportVideoPath.empty()) {
  CheckMainExportVideoState("after-idle");
  CheckMainExportVideoSceneModified("after-idle");
 }
 const unsigned wanted=test.close && !test.cancel && !test.staleScene && !conflict?1:0;
 Check(mainExport.posts==wanted && mainExport.closes==wanted,"exact close-request and fixture-window delivery counts");
 Check(IsWindow(mainExport.window),"fixture intercepts close without destroying the host");
 Check(mainExport.conflictNotices==(mainExportProtectBackground?2u:unsigned(conflict)),
       "only the expected destination conflict messages appear");
 if(conflict) {
   std::ifstream old(path,std::ios::binary);
   const std::string saved(std::istreambuf_iterator<char>(old),{});
   Check(saved==(late ? "video arriving at publication" : "existing video edited during export"),
         "changed video remains byte-identical after failed publication");
   if(late) {
     const fs::path backup(mainExport.retainedVideo);
     Need(mainExport.lateArrivals==1 && backup.parent_path()==path.parent_path() && backup.extension()==L".bak",
          "late arrival preserves the checked video at a separate path in the destination directory");
     std::ifstream previous(backup,std::ios::binary);
     const std::string previousBytes(std::istreambuf_iterator<char>(previous),{});
     Check(previousBytes=="existing video","the recovery file retains the prior video byte-for-byte");
     std::puts("MAIN-EXPORT PASS late arrival preserves both videos, reports recovery path and keeps editor open");
   } else {
     std::puts("MAIN-EXPORT PASS destination conflict preserves video and keeps editor open");
   }
   return;
 }
 Need(fs::is_regular_file(path) && fs::file_size(path)>0,"real encoder finalized a nonempty MP4");
 DecodeMainExport(path,test);
 if(mainExportProtectBackground) {
   Check(!fs::exists(mainExport.pickerPath),"the input rejection and retry never create the AVI spelling");
   std::puts("MAIN-EXPORT PASS loaded background protected, alternate destination decoded");
 } else if(MainExportChangeDestination()) {
   std::ifstream old(mainExport.confirmedPath,std::ios::binary);
   const std::string bytes(std::istreambuf_iterator<char>(old),{});
   Check(bytes=="existing video","declining replacement preserves the old file after alternate export");
   std::puts("MAIN-EXPORT PASS declined replacement then changed destination in the same settings");
 }
 Pump();
 Check(mainExport.posts==wanted && mainExport.closes==wanted && MainExportStopped(),
       "decoding and delayed messages do not restart recording or request another close");
 if(approvalRetry) std::puts("MAIN-EXPORT PASS fresh approval same-path retry decodes and closes once");
 else if(retry) std::puts(lateRetry ? "MAIN-EXPORT PASS late-arrival same-path retry decodes and closes once" :
                               "MAIN-EXPORT PASS same-path retry decodes and closes once");
 if(!mainExportVideoPath.empty()) {
  CheckMainExportVideoState("after-decode");
  CheckMainExportVideoSceneModified("after-decode");
 }
 std::printf("MAIN-EXPORT end=%s writes=%u finalized=%d posts=%u closes=%u scene_changed=%d\n",
     test.name,mainExport.writes,mainExport.finishSucceeded,mainExport.posts,mainExport.closes,
     app->SceneGeneration()!=mainExport.scene);
}
int MainExportEntry(int argc,wchar_t** argv) {
 Need(argc==4,"args: explicit-desktop-mode <desktop-name> <frame-driver|coordinator>");
 // Opt-in only for these MAIN-EXPORT entry variants. Absent keeps the
 // existing MSAA behavior; the exact "1" selects the forced non-MSAA branch;
 // any other present value fails here, before any GUI or D3D device exists.
 const int noMsaaRequest=MainExportNoMsaaRequest();
 Need(noMsaaRequest>=0,"PROBE_MAIN_EXPORT_NO_MSAA accepts only the exact value 1");
 mainExportNoMsaa=noMsaaRequest==1;
 std::printf("MAIN-EXPORT msaa-mode=%s requested=%d\n",mainExportNoMsaa?"forced-none":"actual",noMsaaRequest);
 const bool hiddenControl=std::wstring(argv[1])==L"--main-export-hidden-desktop";
 if(std::wstring(argv[1])==L"--main-export-desktop-bootstrap")BootstrapMainExportDesktop(argv[2]);
 if(hiddenControl)RequireMainExportHiddenDesktop(argv[2]);else RequireMainExportDesktop(argv[2]);
 const std::wstring driver=argv[3];
 Need(driver==L"frame-driver" || driver==L"coordinator","choose an explicit frame-driving mode");
 const auto root=fs::current_path();RequireMainExportEnvironment(root);
 ConfigureMainExportBackground(root);
 const auto wavePath=root/L"export-marker.wav";
 mainExportWave=fs::is_regular_file(wavePath);
 const auto saveRetry=std::getenv("PROBE_MAIN_EXPORT_SAVE_RETRY");
 Need(!saveRetry || (std::strcmp(saveRetry,"1")==0 && !mainExportWave && mainExportVideoPath.empty()),
      "save retry selects the isolated picture-background fixture");
 mainExportSaveRetry=saveRetry!=nullptr;
 const auto changeDestination=std::getenv("PROBE_MAIN_EXPORT_CHANGE_DESTINATION");
 mainExportProtectBackground=changeDestination && std::strcmp(changeDestination,"background")==0;
 Need(!changeDestination || (!mainExportSaveRetry && !mainExportWave &&
      (mainExportProtectBackground ? !mainExportVideoPath.empty() :
       std::strcmp(changeDestination,"1")==0 && mainExportVideoPath.empty())),
      "change destination selects the matching isolated background fixture");
 Need(!mainExportWave || (mainExportVideoPath.empty() && wavePath.native().size()<256),
      "audio test uses a short fixture WAV path and the picture background");
 std::printf("MAIN-EXPORT background=%s path=%ls\n",mainExportVideoPath.empty()?"picture":"video",
     mainExportVideoPath.empty()?L"<generated picture>":mainExportVideoPath.c_str());
 Need(SetEnvironmentVariableW(L"MMS_TEST_COORDINATOR",driver==L"coordinator"?L"1":nullptr)!=FALSE,
      "select the requested real frame driver or coordinator explicitly");
 output=root/(L"main-export-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
 Need(fs::create_directory(output),"create new evidence directory without overwriting earlier results");
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);AddVectoredExceptionHandler(1,Fault);
 LoadDpiTestApis();Need(testSetThreadDpiContext!=nullptr,"Windows 10+ DPI API is available");
 testSetThreadDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 Need(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"initialize isolated COM apartment");
 Need(SUCCEEDED(MFStartup(MF_VERSION)),"initialize real Media Foundation");
 Need(MH_Initialize()==MH_OK,"initialize MAIN-EXPORT observers");
 Hook(&ShowWindow,hiddenControl?&MainExportHiddenShow:&Hidden,&realShow);Hook(&MessageBoxA,&MessageA);Hook(&MessageBoxW,&MainExportMessageBox);
 if(hiddenControl) {
   Hook(&SetWindowPos,&MainExportHiddenPosition,&mainExportRealSetWindowPos);
   Hook(&SetForegroundWindow,&MainExportNoForeground);Hook(&SetActiveWindow,&MainExportNoActivation);
 }
 auto cbt=SetWindowsHookExW(WH_CBT,HideCreated,nullptr,GetCurrentThreadId());
 Need(cbt!=nullptr,"keep fixture windows hidden even on the private desktop");
 auto* app=new MMDApp;g_Block=app;
 // The original wmain already has a large state initialization temporary.
 // Do not add another full state to its nested fixture's thread stack.
 {const auto initial=std::make_unique<MMDAppState>();app->state=*initial;}
 app->InitDefaults();
 mainExportD3DThread=GetCurrentThreadId();
 const bool create9Observed=MainExportObservationHook("Direct3DCreate9",&Direct3DCreate9,
     &MainExportObserveCreate9,&mainExportRealCreate9);
 if(mainExportNoMsaa)Need(create9Observed,"install the required Direct3DCreate9 capability seam");
 mainExportInitActive=mainExportNoMsaa;
 const bool initialized=InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE);
 mainExportInitActive=false;
 Need(initialized,"initialize real isolated MMS host and D3D9");
 if(mainExportNoMsaa)RequireMainExportNoMsaaInitialization(app);
 if(hiddenControl)MainExportHiddenWindowCheck();
 Need(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"),"real MMS host runs without MME");
 mainExport.app=app;mainExport.window=app->MainWindow();
 mainExport.previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(mainExport.window,GWLP_WNDPROC,
     reinterpret_cast<LONG_PTR>(&MainExportWindow)));
 Need(mainExport.previous!=nullptr,"install close-recording procedure on the fixture-owned host window");
 Hook(&PostMessageW,&MainExportPost,&mainExportRealPost);
 Hook(&WriteBuiltInH264Frame,&MainExportWrite,&mainExportRealWrite);
 Hook(&FinishBuiltInH264Recording,&MainExportFinish,&mainExportRealFinish);
 Hook(&MoveFileExW,&MainExportMove,&mainExportRealMove);
 Hook(&GetUiSaveFileNameW,&MainExportPicker,&mainExportRealPicker);
 Hook(&UiDialogBoxParamA,&MainExportSettings,&mainExportRealSettings);
 Hook(&StartBuiltInH264Recording,&MainExportStart,&mainExportRealStart);
 Hook(&ShowUiMessage,&MainExportMessage,&mainExportRealMessage);
 if(mainExportNoMsaa)Hook(&DownsampleCaptureSurface,&MainExportObserveDownsample,&mainExportRealDownsample);
 app->RenderWidth()=640;app->RenderHeight()=360;app->GroundGridEnabled()=0;
 app->CameraMode()=1;app->state.followCameraEnabled=0;
 HandleWindowSize(app);RefreshMainWindowViewport(app);
 if(mainExportVideoPath.empty()) {
  const auto picture=MainExportPicture();
  Need(TryReplaceBackgroundPicture(app,picture.c_str()),"load asymmetric background through the production picture loader");
  Need(app->PictureBackgroundEnabled()!=0,"real background fixture is enabled");
 } else {
  // Production LoadAviFile resolves this absolute path and opens the real
  // modern decoder; there is no fake frame reader, texture or image fallback.
  wcscpy_s(app->AviBackgroundPath(),256,mainExportVideoPath.c_str());
  LoadAviFile(app);
  Need(std::wstring(app->AviBackgroundPath())==mainExportVideoPath,
       "video loader retains the exact isolated fixture path");
  Need(media::HasVideo(app),"real modern video decoder is active for the fixture MP4");
  Need(app->AviBackgroundEnabled()!=0,"loaded video background is enabled");
  Need(app->AviFile()==nullptr && app->AviStream()==nullptr && app->AviFrameReader()==nullptr,
       "video background owns no legacy VFW frame reader");
  Need(app->AviFrameWidth()==64 && app->AviFrameHeight()==48,"loaded source is the known 64x48 fixture");
  Need(app->AviUsesThirtyFpsTiming()!=0 && app->AviStreamStartFrame()==0 &&
       app->AviStreamEndFrame()>kMainExportVideoLast,
       "loaded source covers the whole expected 30 Hz sample range");
  app->PictureBackgroundEnabled()=0;  // The video is the only background source.
 }
 for(int warmup=0;warmup<5;++warmup)Frame(app);
 // Set this orientation fixture to fill the settled preview explicitly. Image
 // auto-import placement is a separate behavior; it may preserve different
 // panel bounds and must not determine our independent quadrant oracle.
 const auto preview=app->ViewportRect();
 Need(preview.right>preview.left && std::isfinite(app->Renderer()->viewScale) && app->Renderer()->viewScale>0,
      "orientation fixture has a settled preview geometry");
 if(mainExportVideoPath.empty()) {
  app->PictureOffsetX()=0;app->PictureOffsetY()=0;
  app->PictureScale()=float(double(preview.right-preview.left)*1.2000000476837158/(640.0*app->Renderer()->viewScale));
  PicBgOverlayRefresh(app);
 } else {
  // The known 64x48 source keeps its 4:3 aspect. Filling the settled preview
  // width draws 640x480 at offset 0, so capture rows 90/270 sample source rows
  // 9/27 and the 24-row red/green-to-blue split stays unambiguous.
  app->AviOffsetX()=0;app->AviOffsetY()=0;
  app->AviScale()=float(double(preview.right-preview.left)*1.2000000476837158/(64.0*app->Renderer()->viewScale));
  AviBgPlacementRefresh(app);
  mainExportVideoExpected=MainExportVideoActual(app);
  Need(mainExportVideoExpected.path==mainExportVideoPath &&
       mainExportVideoExpected.hasVideo && mainExportVideoExpected.enabled &&
       mainExportVideoExpected.pictureDisabled && mainExportVideoExpected.modernHandlesClear &&
       mainExportVideoExpected.width==64 && mainExportVideoExpected.height==48 &&
       mainExportVideoExpected.offsetX==0 && mainExportVideoExpected.offsetY==0 &&
       mainExportVideoExpected.scale>0,
       "video background keeps the real decoder and explicit fill-width placement");
  MainExportLogVideoState("after-load",mainExportVideoExpected);
 }
 std::printf("MAIN-EXPORT driver=%ls output=%ls\n",driver.c_str(),output.c_str());
 if(changeDestination) {
  const MainExportCase test{"change-destination",24,17,22,5,5.0/24,true,false,false,ExportDestination::DeclineThenChange};
  RunMainExportCase(app,test);
  if(hiddenControl)MainExportHiddenWindowCheck();
 } else if(mainExportSaveRetry) {
  const MainExportCase test{"save-retry",24,17,22,5,5.0/24,true,false,false};
  app->SceneModified()=1;
  RunMainExportCase(app,test);
  if(hiddenControl)MainExportHiddenWindowCheck();
 } else if(mainExportWave) {
  wcscpy_s(app->WavePath(),256,wavePath.c_str());
  LoadWaveFile(app);
  Need(app->WaveEnabled()!=0 && app->Audio()->path==wavePath.native() &&
       app->Audio()->format.nSamplesPerSec==48000,"import the isolated 48 kHz marker WAV");
  const auto offset=app->Audio()->dataOffset;
  const auto size=app->Audio()->dataSize;
  for(const auto& test:mainExportWaveCases) {
   RunMainExportCase(app,test);
   Check(app->WaveEnabled()!=0 && app->Audio()->path==wavePath.native() &&
         app->Audio()->dataOffset==offset && app->Audio()->dataSize==size,"export preserves the loaded source WAV range");
   if(hiddenControl)MainExportHiddenWindowCheck();
  }
 } else if(mainExportVideoPath.empty()) {
  for(const auto& test:mainExportCases) {
   RunMainExportCase(app,test);
   if(hiddenControl)MainExportHiddenWindowCheck();
  }
 } else {
  for(const auto& test:mainExportVideoCases) {
   RunMainExportCase(app,test);
   if(hiddenControl)MainExportHiddenWindowCheck();
  }
 }
 if(hiddenControl)RequireMainExportHiddenDesktop(argv[2]);else RequireMainExportDesktop(argv[2]);
 Check(!GetModuleHandleW(L"MMEffect.dll"),"all exports completed without real MME");
 phase="main-export-shutdown";
 // Explicitly destroy only this fixture's window, with the close sink still
 // installed. Follow the host's normal WM_DESTROY-before-ShutdownCleanup order.
 Need(DestroyWindow(mainExport.window)!=FALSE,"destroy the owned fixture host after all close assertions");
 ShutdownCleanup(app);
 g_Block=nullptr;delete app;mainExport.app=nullptr;
 UnhookWindowsHookEx(cbt);MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
 MFShutdown();CoUninitialize();
 std::printf("MAIN-EXPORT SUMMARY failures=%d\n",failures);
 return failures?1:0;
}
#endif
// PROBE_RESTORE_GEOMETRY selects an opt-in, reproducible accessory geometry
// probe for the ordinary export fixture. MAIN-EXPORT returns before this code.
// "legacy" leaves the loaded fixture and its keys untouched, "fitted" only
// rescales accessory 0 and lifts it so the fixture's -10..10 / 0..20 cube is
// framed by the default camera, and "hidden" applies that transform and then
// hides the accessory. Unset or any other value stays off and adds no scene
// change, GPU access, frame, render or text preparation. The assertion below
// reuses only the pixels and State the existing comparisons already captured.
// PROBE_RESTORE_GEOMETRY_TRACE=1 additionally installs the read-only accessory
// DrawSubset observer defined below, and only while one of these modes is on.
enum class RestoreGeometryMode { off, legacy, fitted, hidden };
RestoreGeometryMode restoreGeometryMode=RestoreGeometryMode::off;
RestoreGeometryMode ParseRestoreGeometryMode() {
 const char* value=std::getenv("PROBE_RESTORE_GEOMETRY");
 if(value==nullptr)return RestoreGeometryMode::off;
 if(std::strcmp(value,"legacy")==0)return RestoreGeometryMode::legacy;
 if(std::strcmp(value,"fitted")==0)return RestoreGeometryMode::fitted;
 if(std::strcmp(value,"hidden")==0)return RestoreGeometryMode::hidden;
 return RestoreGeometryMode::off;
}
const char* RestoreGeometryName(RestoreGeometryMode mode) {
 switch(mode) {
  case RestoreGeometryMode::legacy: return "legacy";
  case RestoreGeometryMode::fitted: return "fitted";
  case RestoreGeometryMode::hidden: return "hidden";
  default: return "off";
 }
}
void PrintRestoreGeometry(MMDApp* app,const char* when) {
 if(restoreGeometryMode==RestoreGeometryMode::off)return;
 auto* record=app->AccessorySlot(0);
 auto* keys=app->AccessoryKeys(0);
 if(record==nullptr||keys==nullptr) {
  std::printf("GEOMETRY when=%s mode=%s record=%p keys=%p\n",when,
      RestoreGeometryName(restoreGeometryMode),static_cast<void*>(record),static_cast<void*>(keys));
  return;
 }
 std::printf("GEOMETRY when=%s mode=%s record_visible=%u key_visible=%u record_scale=%.9g "
     "key_scale=%.9g record_position=%.9g,%.9g,%.9g key_position=%.9g,%.9g,%.9g "
     "camera_target=%.9g,%.9g,%.9g camera_distance=%.9g\n",when,
     RestoreGeometryName(restoreGeometryMode),unsigned(record->visible),unsigned(keys[0].visible),
     double(record->scale),double(keys[0].scale),
     double(record->position[0]),double(record->position[1]),double(record->position[2]),
     double(keys[0].position[0]),double(keys[0].position[1]),double(keys[0].position[2]),
     double(app->CameraPositionAxis(0)),double(app->CameraPositionAxis(1)),
     double(app->CameraPositionAxis(2)),double(app->CameraDistance()));
}
void ApplyRestoreGeometry(MMDApp* app) {
 if(restoreGeometryMode==RestoreGeometryMode::off)return;
 auto* record=app->AccessorySlot(0);
 auto* keys=app->AccessoryKeys(0);
 Need(record!=nullptr&&keys!=nullptr,"geometry probe owns accessory 0 and its timeline key");
 if(restoreGeometryMode==RestoreGeometryMode::fitted||restoreGeometryMode==RestoreGeometryMode::hidden) {
  record->scale=0.05f;
  record->position[1]=5.0f;
  keys[0].scale=0.05f;
  keys[0].position[1]=5.0f;
 }
 if(restoreGeometryMode==RestoreGeometryMode::hidden) {
  record->visible=0;
  keys[0].visible=0;
 }
 PrintRestoreGeometry(app,"initialized");
}

// PROBE_RESTORE_GEOMETRY_TRACE must be exactly "1" and only acts while
// PROBE_RESTORE_GEOMETRY selects a non-off mode. It watches accessory 0's own
// ID3DXMesh::DrawSubset (vtable slot 3, HRESULT WINAPI(void*,DWORD)):
//   * the observer is installed after the fixture is loaded and its geometry is
//     set, before the first frame, and the accessory mesh pointer is saved as
//     the only accepted caller identity; the default path installs no hook;
//   * the first matching call in each existing phase records the real HRESULT
//     plus the entry-time viewport, world/view/projection, material, the named
//     render and texture-stage states, bound textures/shaders and mesh counts;
//   * that call is forwarded exactly once with the original arguments and its
//     HRESULT is returned unchanged. Every query below is a Get* reader whose
//     HRESULT is printed, and the COM references taken for the texture and
//     shader probes are released immediately. The observer performs no state
//     write, no VB/IB read or lock, no GPU resource creation and no extra
//     frame, render, prepare or draw.
// The clip/NDC block is plain CPU row-vector arithmetic over the captured
// matrices and the eight fixed fixture corners written by
// tests/export_viewport_runner.py; it is not a visual verdict.
using RestoreTraceDrawSubset=HRESULT(WINAPI*)(void*,DWORD);
RestoreTraceDrawSubset realRestoreTraceDrawSubset=nullptr;
void* restoreTraceMesh=nullptr;
bool RestoreTraceRequested(){
 const char* value=std::getenv("PROBE_RESTORE_GEOMETRY_TRACE");
 return value!=nullptr&&std::strcmp(value,"1")==0;
}
// One record per existing phase: the first matching call claims its phase, and
// later calls in that phase forward without touching the device.
bool RestoreTraceClaimPhase(){
 static const char* claimed[16]{};
 static unsigned count=0;
 for(unsigned i=0;i<count;++i)if(std::strcmp(claimed[i],phase)==0)return false;
 if(count==16)return false;
 claimed[count++]=phase;
 return true;
}
void RestoreTraceMultiply(const float* a,const float* b,float* out){
 for(int row=0;row<4;++row)for(int column=0;column<4;++column){
  float sum=0;
  for(int k=0;k<4;++k)sum+=a[row*4+k]*b[k*4+column];
  out[row*4+column]=sum;
 }
}
void RestoreTraceClip(const float* world,const float* view,const float* projection){
 // Fixed fixture.x cube (tests/export_viewport_runner.py): x,z in -10..10 and
 // y in 0..20. D3D row-vector convention: clip = corner * world * view * projection.
 static const double corners[8][3]={
  {-10,0,-10},{10,0,-10},{10,20,-10},{-10,20,-10},
  {-10,0,10},{10,0,10},{10,20,10},{-10,20,10}};
 float worldView[16]{},combined[16]{};
 RestoreTraceMultiply(world,view,worldView);
 RestoreTraceMultiply(worldView,projection,combined);
 double clipMin[4]{},clipMax[4]{},ndcMin[3]{},ndcMax[3]{};
 bool haveNdc=false;
 int positiveW=0,degenerateW=0,inClip=0;
 for(int corner=0;corner<8;++corner){
  double clip[4]{};
  for(int column=0;column<4;++column)clip[column]=corners[corner][0]*combined[column]+
      corners[corner][1]*combined[4+column]+corners[corner][2]*combined[8+column]+combined[12+column];
  for(int i=0;i<4;++i){
   if(corner==0||clip[i]<clipMin[i])clipMin[i]=clip[i];
   if(corner==0||clip[i]>clipMax[i])clipMax[i]=clip[i];
  }
  // A corner can sit exactly on the projection's w=0 plane; dividing there
  // would print fabricated infinite NDC bounds.
  if(!(std::fabs(clip[3])>1e-9)){++degenerateW;continue;}
  if(clip[3]>0)++positiveW;
  const double ndc[3]={clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]};
  for(int i=0;i<3;++i){
   if(!haveNdc||ndc[i]<ndcMin[i])ndcMin[i]=ndc[i];
   if(!haveNdc||ndc[i]>ndcMax[i])ndcMax[i]=ndc[i];
  }
  haveNdc=true;
  if(clip[3]>0&&clip[0]>=-clip[3]&&clip[0]<=clip[3]&&clip[1]>=-clip[3]&&
     clip[1]<=clip[3]&&clip[2]>=0&&clip[2]<=clip[3])++inClip;
 }
 std::printf("GEOMETRY_DRAW phase=%s clip x=[%.9g,%.9g] y=[%.9g,%.9g] z=[%.9g,%.9g] "
     "w=[%.9g,%.9g] positive_w=%d degenerate_w=%d inside_clip=%d/8\n",phase,
     clipMin[0],clipMax[0],clipMin[1],clipMax[1],clipMin[2],clipMax[2],clipMin[3],clipMax[3],
     positiveW,degenerateW,inClip);
 if(haveNdc)std::printf("GEOMETRY_DRAW phase=%s ndc x=[%.9g,%.9g] y=[%.9g,%.9g] "
     "z=[%.9g,%.9g]\n",phase,ndcMin[0],ndcMax[0],ndcMin[1],ndcMax[1],ndcMin[2],ndcMax[2]);
 else std::printf("GEOMETRY_DRAW phase=%s ndc unavailable: all corners on the w=0 plane\n",phase);
}
// D3DXATTRIBUTERANGE layout (five DWORDs) from the SDK declaration, kept local
// so the observer needs no D3DX header and never writes mesh data.
struct RestoreTraceAttributeRange{
 DWORD attribId,faceStart,faceCount,vertexStart,vertexCount;
};
static_assert(sizeof(RestoreTraceAttributeRange)==5*sizeof(DWORD),"attribute range layout");

void RestoreTraceObserve(MMDApp* app,void* mesh,DWORD index){
 auto* renderer=app?app->Renderer():nullptr;
 auto* device=renderer?renderer->device:nullptr;
 std::printf("GEOMETRY_DRAW phase=%s entry mesh=%p vtbl=%p index=%lu device=%p\n",phase,mesh,
     *reinterpret_cast<void**>(mesh),static_cast<unsigned long>(index),static_cast<void*>(device));
 if(device==nullptr)return;
 D3DVIEWPORT9 viewport{};
 const HRESULT viewportHr=device->GetViewport(&viewport);
 std::printf("GEOMETRY_DRAW phase=%s viewport_hr=0x%08lx x=%lu y=%lu width=%lu height=%lu "
     "minz=%.9g maxz=%.9g\n",phase,static_cast<unsigned long>(viewportHr),
     static_cast<unsigned long>(viewport.X),static_cast<unsigned long>(viewport.Y),
     static_cast<unsigned long>(viewport.Width),static_cast<unsigned long>(viewport.Height),
     double(viewport.MinZ),double(viewport.MaxZ));
 const D3DTRANSFORMSTATETYPE transformTypes[3]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION};
 const char* transformNames[3]={"world","view","projection"};
 float transforms[3][16]{};
 for(int i=0;i<3;++i){
  D3DMATRIX matrix{};
  const HRESULT hr=device->GetTransform(transformTypes[i],&matrix);
  const auto* values=reinterpret_cast<const float*>(&matrix);
  for(int j=0;j<16;++j)transforms[i][j]=values[j];
  std::printf("GEOMETRY_DRAW phase=%s transform=%s hr=0x%08lx m=",phase,transformNames[i],
      static_cast<unsigned long>(hr));
  for(int j=0;j<16;++j)std::printf("%s%.9g",j?",":"",double(values[j]));
  std::printf("\n");
 }
 D3DMATERIAL9 material{};
 const HRESULT materialHr=device->GetMaterial(&material);
 std::printf("GEOMETRY_DRAW phase=%s material_hr=0x%08lx diffuse=%.9g,%.9g,%.9g,%.9g "
     "ambient=%.9g,%.9g,%.9g,%.9g specular=%.9g,%.9g,%.9g,%.9g power=%.9g\n",phase,
     static_cast<unsigned long>(materialHr),
     double(material.Diffuse.r),double(material.Diffuse.g),double(material.Diffuse.b),
     double(material.Diffuse.a),double(material.Ambient.r),double(material.Ambient.g),
     double(material.Ambient.b),double(material.Ambient.a),double(material.Specular.r),
     double(material.Specular.g),double(material.Specular.b),double(material.Specular.a),
     double(material.Power));
 struct RestoreTraceState{D3DRENDERSTATETYPE type;const char* name;};
 const RestoreTraceState states[]={
  {D3DRS_ZENABLE,"ZENABLE"},{D3DRS_ZWRITEENABLE,"ZWRITEENABLE"},{D3DRS_ZFUNC,"ZFUNC"},
  {D3DRS_CULLMODE,"CULLMODE"},{D3DRS_ALPHABLENDENABLE,"ALPHABLENDENABLE"},
  {D3DRS_SRCBLEND,"SRCBLEND"},{D3DRS_DESTBLEND,"DESTBLEND"},
  {D3DRS_ALPHATESTENABLE,"ALPHATESTENABLE"},{D3DRS_ALPHAREF,"ALPHAREF"},
  {D3DRS_ALPHAFUNC,"ALPHAFUNC"},{D3DRS_COLORWRITEENABLE,"COLORWRITEENABLE"},
  {D3DRS_SCISSORTESTENABLE,"SCISSORTESTENABLE"},{D3DRS_LIGHTING,"LIGHTING"},
  {D3DRS_STENCILENABLE,"STENCILENABLE"},{D3DRS_STENCILFUNC,"STENCILFUNC"},
  {D3DRS_STENCILREF,"STENCILREF"},{D3DRS_STENCILMASK,"STENCILMASK"},
  {D3DRS_STENCILWRITEMASK,"STENCILWRITEMASK"},{D3DRS_STENCILFAIL,"STENCILFAIL"},
  {D3DRS_STENCILZFAIL,"STENCILZFAIL"},{D3DRS_STENCILPASS,"STENCILPASS"},
  {D3DRS_CLIPPLANEENABLE,"CLIPPLANEENABLE"}};
 for(const auto& state:states){
  DWORD value=0;
  const HRESULT hr=device->GetRenderState(state.type,&value);
  std::printf("GEOMETRY_DRAW phase=%s state=%s hr=0x%08lx value=%lu\n",phase,state.name,
      static_cast<unsigned long>(hr),static_cast<unsigned long>(value));
 }
 struct RestoreTraceStageState{D3DTEXTURESTAGESTATETYPE type;const char* name;};
 const RestoreTraceStageState stageStates[]={
  {D3DTSS_COLOROP,"COLOROP"},{D3DTSS_COLORARG1,"COLORARG1"},{D3DTSS_COLORARG2,"COLORARG2"},
  {D3DTSS_ALPHAOP,"ALPHAOP"},{D3DTSS_ALPHAARG1,"ALPHAARG1"},{D3DTSS_ALPHAARG2,"ALPHAARG2"}};
 for(DWORD stage=0;stage<2;++stage)for(const auto& st:stageStates){
  DWORD value=0;
  const HRESULT hr=device->GetTextureStageState(stage,st.type,&value);
  std::printf("GEOMETRY_DRAW phase=%s texture_stage=%lu state=%s hr=0x%08lx value=%lu\n",phase,
      static_cast<unsigned long>(stage),st.name,static_cast<unsigned long>(hr),
      static_cast<unsigned long>(value));
 }
 // GetTexture/GetVertexShader/GetPixelShader add a reference on success; release
 // it at once so the observer leaves the reference counts unchanged.
 for(DWORD stage=0;stage<2;++stage){
  IDirect3DBaseTexture9* texture=nullptr;
  const HRESULT hr=device->GetTexture(stage,&texture);
  std::printf("GEOMETRY_DRAW phase=%s texture=%lu hr=0x%08lx null=%d\n",phase,
      static_cast<unsigned long>(stage),static_cast<unsigned long>(hr),texture==nullptr?1:0);
  if(texture)texture->Release();
 }
 IDirect3DVertexShader9* vertexShader=nullptr;
 const HRESULT vertexShaderHr=device->GetVertexShader(&vertexShader);
 std::printf("GEOMETRY_DRAW phase=%s vertex_shader_hr=0x%08lx null=%d\n",phase,
     static_cast<unsigned long>(vertexShaderHr),vertexShader==nullptr?1:0);
 if(vertexShader)vertexShader->Release();
 IDirect3DPixelShader9* pixelShader=nullptr;
 const HRESULT pixelShaderHr=device->GetPixelShader(&pixelShader);
 std::printf("GEOMETRY_DRAW phase=%s pixel_shader_hr=0x%08lx null=%d\n",phase,
     static_cast<unsigned long>(pixelShaderHr),pixelShader==nullptr?1:0);
 if(pixelShader)pixelShader->Release();
 // Read-only mesh counters through the real ID3DXBaseMesh vtable slots 4/5/6
 // (see d3dx_mesh.hpp). No vertex/index buffer is read or locked.
 const DWORD faces=d3dx::MeshMethod<DWORD(WINAPI*)(void*),4>(mesh)(mesh);
 const DWORD vertices=d3dx::MeshMethod<DWORD(WINAPI*)(void*),5>(mesh)(mesh);
 const DWORD fvf=d3dx::MeshMethod<DWORD(WINAPI*)(void*),6>(mesh)(mesh);
 std::printf("GEOMETRY_DRAW phase=%s mesh_faces=%lu mesh_vertices=%lu mesh_fvf=0x%08lx\n",phase,
     static_cast<unsigned long>(faces),static_cast<unsigned long>(vertices),
     static_cast<unsigned long>(fvf));
 // Read-only attribute table through the real ID3DXBaseMesh vtable slot 19:
 // query the size first, then fill a bounded local array of at most 32 ranges.
 DWORD attributeCount=0;
 const HRESULT attributeQueryHr=d3dx::MeshMethod<HRESULT(WINAPI*)(void*,RestoreTraceAttributeRange*,DWORD*),19>(mesh)(
     mesh,nullptr,&attributeCount);
 std::printf("GEOMETRY_DRAW phase=%s attribute_table_query_hr=0x%08lx count=%lu\n",phase,
     static_cast<unsigned long>(attributeQueryHr),static_cast<unsigned long>(attributeCount));
 if(SUCCEEDED(attributeQueryHr)&&attributeCount!=0&&attributeCount<=32){
  RestoreTraceAttributeRange attributes[32]{};
  DWORD queried=32;
  const HRESULT attributeHr=d3dx::MeshMethod<HRESULT(WINAPI*)(void*,RestoreTraceAttributeRange*,DWORD*),19>(mesh)(
      mesh,attributes,&queried);
  std::printf("GEOMETRY_DRAW phase=%s attribute_table_hr=0x%08lx requested=%lu returned=%lu\n",phase,
      static_cast<unsigned long>(attributeHr),static_cast<unsigned long>(attributeCount),
      static_cast<unsigned long>(queried));
  if(SUCCEEDED(attributeHr)){
   if(queried>32)queried=32;
   for(DWORD i=0;i<queried;++i)std::printf("GEOMETRY_DRAW phase=%s attribute=%lu AttribId=%lu "
       "FaceStart=%lu FaceCount=%lu VertexStart=%lu VertexCount=%lu\n",phase,
       static_cast<unsigned long>(i),static_cast<unsigned long>(attributes[i].attribId),
       static_cast<unsigned long>(attributes[i].faceStart),static_cast<unsigned long>(attributes[i].faceCount),
       static_cast<unsigned long>(attributes[i].vertexStart),static_cast<unsigned long>(attributes[i].vertexCount));
  }
 }else if(attributeQueryHr==S_OK&&attributeCount>32){
  std::printf("GEOMETRY_DRAW phase=%s attribute_table=skipped count=%lu cap=32\n",phase,
      static_cast<unsigned long>(attributeCount));
 }
 // Owning-device identity through vtable slot 10; the returned reference is
 // released immediately and compared with the device currently on the renderer.
 IDirect3DDevice9* owner=nullptr;
 const HRESULT ownerHr=d3dx::MeshMethod<HRESULT(WINAPI*)(void*,IDirect3DDevice9**),10>(mesh)(mesh,&owner);
 std::printf("GEOMETRY_DRAW phase=%s mesh_device_hr=0x%08lx owner=%p current=%p match=%d\n",phase,
     static_cast<unsigned long>(ownerHr),static_cast<void*>(owner),static_cast<void*>(device),
     owner==device?1:0);
 if(owner)owner->Release();
 RestoreTraceClip(transforms[0],transforms[1],transforms[2]);
}
HRESULT WINAPI ObserveRestoreTraceDrawSubset(void* mesh,DWORD index){
 if(realRestoreTraceDrawSubset==nullptr)return E_FAIL;
 // Only the saved accessory mesh is observed; every other caller of the shared
 // DrawSubset entry point is forwarded untouched.
 if(mesh!=restoreTraceMesh||!RestoreTraceClaimPhase())
  return realRestoreTraceDrawSubset(mesh,index);
 const DWORD incomingError=GetLastError();
 RestoreTraceObserve(g_Block,mesh,index);
 SetLastError(incomingError);
 const HRESULT result=realRestoreTraceDrawSubset(mesh,index);
 const DWORD returnedError=GetLastError();
 std::printf("GEOMETRY_DRAW phase=%s result hr=0x%08lx index=%lu\n",phase,
     static_cast<unsigned long>(result),static_cast<unsigned long>(index));
 SetLastError(returnedError);
 return result;
}
void EnableRestoreGeometryTrace(MMDApp* app){
 if(!RestoreTraceRequested())return;
 auto* record=app?app->AccessorySlot(0):nullptr;
 Need(record!=nullptr&&record->mesh!=nullptr,"geometry trace owns the loaded accessory 0 mesh");
 restoreTraceMesh=record->mesh;
 const auto target=reinterpret_cast<RestoreTraceDrawSubset>(
     (*reinterpret_cast<void***>(restoreTraceMesh))[3]);
 Need(target!=nullptr,"geometry trace resolves the accessory mesh DrawSubset slot");
 Hook(target,&ObserveRestoreTraceDrawSubset,&realRestoreTraceDrawSubset);
 std::printf("GEOMETRY_DRAW phase=%s installed accessory=%p mesh=%p vtbl=%p draw_subset=%p\n",phase,
     static_cast<void*>(record),restoreTraceMesh,*reinterpret_cast<void**>(restoreTraceMesh),
     reinterpret_cast<void*>(target));
}

// Diagnostics only: called after every existing pixel and geometry assertion
// has run and immediately before SUMMARY. It reads the saved accessory mesh's
// own vertex and index buffers with D3DLOCK_READONLY and prints a bounded
// fixture sample: at most 8 vertices and 36 indices. It never writes a buffer,
// never re-locks with another flag, never re-renders, draws or frames, never
// replaces or repairs the mesh, and adds no Check, so neither the exit status
// nor any earlier acceptance evidence changes. These bytes are diagnostics and
// cannot be used as evidence that the recorded picture is acceptable.
void RestoreTracePostAssertionBuffers(){
 if(!RestoreTraceRequested()||restoreTraceMesh==nullptr)return;
 void* mesh=restoreTraceMesh;
 std::printf("GEOMETRY_DRAW post_assert phase=%s note=diagnostic read after every pixel assertion; "
     "no frame, render or draw follows and this read is not acceptance evidence\n",phase);
 IDirect3DVertexBuffer9* vertexBuffer=nullptr;
 const HRESULT vertexBufferHr=d3dx::MeshMethod<HRESULT(WINAPI*)(void*,IDirect3DVertexBuffer9**),13>(mesh)(
     mesh,&vertexBuffer);
 std::printf("GEOMETRY_DRAW post_assert phase=%s vertex_buffer_hr=0x%08lx null=%d\n",phase,
     static_cast<unsigned long>(vertexBufferHr),vertexBuffer==nullptr?1:0);
 if(SUCCEEDED(vertexBufferHr)&&vertexBuffer){
  D3DVERTEXBUFFER_DESC desc{};
  const HRESULT descHr=vertexBuffer->GetDesc(&desc);
  const DWORD stride=d3dx::MeshMethod<DWORD(WINAPI*)(void*),8>(mesh)(mesh);
  std::printf("GEOMETRY_DRAW post_assert phase=%s vertex_buffer_desc_hr=0x%08lx usage=0x%08lx "
      "pool=%lu type=%lu format=%lu size=%lu fvf=0x%08lx num_bytes_per_vertex=%lu\n",phase,
      static_cast<unsigned long>(descHr),static_cast<unsigned long>(desc.Usage),
      static_cast<unsigned long>(desc.Pool),static_cast<unsigned long>(desc.Type),
      static_cast<unsigned long>(desc.Format),static_cast<unsigned long>(desc.Size),
      static_cast<unsigned long>(desc.FVF),static_cast<unsigned long>(stride));
  if(SUCCEEDED(descHr)&&stride>=12&&desc.Size>=stride){
   const UINT total=desc.Size/stride;
   UINT read=total;if(read>8)read=8;
   const UINT lockBytes=read*stride;
   std::printf("GEOMETRY_DRAW post_assert phase=%s vertex_total=%lu vertex_read=%lu "
       "vertex_lock_bytes=%lu\n",phase,static_cast<unsigned long>(total),
       static_cast<unsigned long>(read),static_cast<unsigned long>(lockBytes));
   void* data=nullptr;
   const HRESULT lockHr=vertexBuffer->Lock(0,lockBytes,&data,D3DLOCK_READONLY);
   std::printf("GEOMETRY_DRAW post_assert phase=%s vertex_lock_hr=0x%08lx readonly=1\n",phase,
       static_cast<unsigned long>(lockHr));
   if(SUCCEEDED(lockHr)){
    if(data!=nullptr)for(UINT i=0;i<read;++i){
     const float* xyz=reinterpret_cast<const float*>(
         static_cast<const unsigned char*>(data)+std::size_t(i)*stride);
     std::printf("GEOMETRY_DRAW post_assert phase=%s vertex=%lu xyz=%.9g,%.9g,%.9g\n",phase,
         static_cast<unsigned long>(i),double(xyz[0]),double(xyz[1]),double(xyz[2]));
    }
    vertexBuffer->Unlock();
   }
  }else std::printf("GEOMETRY_DRAW post_assert phase=%s vertex_read=skipped stride_or_size=0\n",phase);
 }
 if(vertexBuffer)vertexBuffer->Release();
 IDirect3DIndexBuffer9* indexBuffer=nullptr;
 const HRESULT indexBufferHr=d3dx::MeshMethod<HRESULT(WINAPI*)(void*,IDirect3DIndexBuffer9**),14>(mesh)(
     mesh,&indexBuffer);
 std::printf("GEOMETRY_DRAW post_assert phase=%s index_buffer_hr=0x%08lx null=%d\n",phase,
     static_cast<unsigned long>(indexBufferHr),indexBuffer==nullptr?1:0);
 if(SUCCEEDED(indexBufferHr)&&indexBuffer){
  D3DINDEXBUFFER_DESC desc{};
  const HRESULT descHr=indexBuffer->GetDesc(&desc);
  const UINT width=desc.Format==D3DFMT_INDEX32?4:desc.Format==D3DFMT_INDEX16?2:0;
  std::printf("GEOMETRY_DRAW post_assert phase=%s index_buffer_desc_hr=0x%08lx usage=0x%08lx "
      "pool=%lu type=%lu format=%lu size=%lu index_bytes=%lu\n",phase,
      static_cast<unsigned long>(descHr),static_cast<unsigned long>(desc.Usage),
      static_cast<unsigned long>(desc.Pool),static_cast<unsigned long>(desc.Type),
      static_cast<unsigned long>(desc.Format),static_cast<unsigned long>(desc.Size),
      static_cast<unsigned long>(width));
  if(SUCCEEDED(descHr)&&width!=0&&desc.Size>=width){
   const UINT total=desc.Size/width;
   UINT read=total;if(read>36)read=36;
   const UINT lockBytes=read*width;
   std::printf("GEOMETRY_DRAW post_assert phase=%s index_total=%lu index_read=%lu "
       "index_lock_bytes=%lu\n",phase,static_cast<unsigned long>(total),
       static_cast<unsigned long>(read),static_cast<unsigned long>(lockBytes));
   void* data=nullptr;
   const HRESULT lockHr=indexBuffer->Lock(0,lockBytes,&data,D3DLOCK_READONLY);
   std::printf("GEOMETRY_DRAW post_assert phase=%s index_lock_hr=0x%08lx readonly=1\n",phase,
       static_cast<unsigned long>(lockHr));
   if(SUCCEEDED(lockHr)){
    if(data!=nullptr){
     DWORD values[36]{};
     for(UINT i=0;i<read;++i)values[i]=width==2
         ?DWORD(static_cast<const unsigned short*>(data)[i])
         :static_cast<const unsigned int*>(data)[i];
     for(UINT i=0;i<read;i+=3){
      if(i+2<read)std::printf("GEOMETRY_DRAW post_assert phase=%s face=%lu indices=%lu,%lu,%lu\n",phase,
          static_cast<unsigned long>(i/3),static_cast<unsigned long>(values[i]),
          static_cast<unsigned long>(values[i+1]),static_cast<unsigned long>(values[i+2]));
      else std::printf("GEOMETRY_DRAW post_assert phase=%s face=%lu partial_index=%lu value=%lu\n",phase,
          static_cast<unsigned long>(i/3),static_cast<unsigned long>(i),
          static_cast<unsigned long>(values[i]));
     }
    }
    indexBuffer->Unlock();
   }
  }else std::printf("GEOMETRY_DRAW post_assert phase=%s index_read=skipped unsupported_format=%lu\n",
      phase,static_cast<unsigned long>(desc.Format));
 }
 if(indexBuffer)indexBuffer->Release();
}

void CheckRestoreGeometry(const State& state,const std::vector<DWORD>& pixels,const char* stage) {
 if(restoreGeometryMode==RestoreGeometryMode::off)return;
 if(state.back.Width==0||state.back.Height==0||state.viewport.Width==0||state.viewport.Height==0) {
  Check(false,"geometry probe captured readable backbuffer and viewport bounds");
  return;
 }
 const std::size_t expected=std::size_t(state.back.Width)*state.back.Height;
 if(pixels.size()<expected) {
  std::printf("GEOMETRY_PIXELS mode=%s stage=%s pixels=%zu expected=%zu\n",
      RestoreGeometryName(restoreGeometryMode),stage,pixels.size(),expected);
  Check(false,"geometry probe captured the full backbuffer");
  return;
 }
 // The central half of the actual GPU viewport keeps the edge HUD out of the
 // sampled region, but it does not exclude the ground grid in general; the
 // blue-channel test and the 1024-pixel gate provide that separation. Clamp
 // the region to the backbuffer before indexing the readback.
 LONG left=static_cast<LONG>(state.viewport.X)+static_cast<LONG>(state.viewport.Width)/4;
 LONG right=static_cast<LONG>(state.viewport.X)+static_cast<LONG>(state.viewport.Width)-static_cast<LONG>(state.viewport.Width)/4;
 LONG top=static_cast<LONG>(state.viewport.Y)+static_cast<LONG>(state.viewport.Height)/4;
 LONG bottom=static_cast<LONG>(state.viewport.Y)+static_cast<LONG>(state.viewport.Height)-static_cast<LONG>(state.viewport.Height)/4;
 if(left<0)left=0;
 if(top<0)top=0;
 if(right>static_cast<LONG>(state.back.Width))right=static_cast<LONG>(state.back.Width);
 if(bottom>static_cast<LONG>(state.back.Height))bottom=static_cast<LONG>(state.back.Height);
 if(right<=left||bottom<=top) {
  Check(false,"geometry probe has a nonempty central viewport region");
  return;
 }
 std::size_t blue=0;
 LONG minX=0,minY=0,maxX=-1,maxY=-1;
 for(LONG y=top;y<bottom;++y)for(LONG x=left;x<right;++x) {
  const DWORD pixel=pixels[std::size_t(y)*state.back.Width+std::size_t(x)];
  const int b=static_cast<int>(pixel&0xffu);
  const int g=static_cast<int>((pixel>>8)&0xffu);
  const int r=static_cast<int>((pixel>>16)&0xffu);
  if(!(b>r+32&&b>g+12&&b>=64))continue;
  ++blue;
  if(maxX<0) {minX=maxX=x;minY=maxY=y;}
  else {if(x<minX)minX=x;if(x>maxX)maxX=x;if(y<minY)minY=y;if(y>maxY)maxY=y;}
 }
 const LONG width=maxX>=0?maxX-minX+1:0;
 const LONG height=maxY>=0?maxY-minY+1:0;
 std::printf("GEOMETRY_PIXELS mode=%s stage=%s roi=%ld,%ld,%ld,%ld blue=%zu bbox=%ld,%ld,%ld,%ld\n",
     RestoreGeometryName(restoreGeometryMode),stage,left,top,right,bottom,blue,minX,minY,maxX,maxY);
 Check(blue>=1024,"visible fixture geometry keeps at least 1024 blue interior pixels");
 Check(maxX>=0&&width>=16&&height>=16,"visible fixture geometry spans at least 16x16 blue interior pixels");
}
// PROBE_RESTORE_FIRST_FRAME must be exactly "1" and only acts for the ordinary
// geometry fixture (PROBE_RESTORE_GEOMETRY=legacy|fitted|hidden). The existing
// Compare renders before it reads the backbuffer, so that extra Render can hide
// what the first production frame after the final HandleWindowSize actually
// presents. This probe instead:
//   * installs a count-only RenderFrameScene observer before that
//     HandleWindowSize; every call is forwarded exactly once through the saved
//     trampoline and only the target app is counted;
//   * requires the layout itself to enter RenderFrameScene zero times, then
//     sets DeltaTime/messageSeen and calls coordinator_fixture::Drive once
//     (plain FrameDriver, or the real SafePoint when the coordinator fixture is
//     enabled) and requires exactly one scene call;
//   * snapshots and captures the backbuffer immediately, before any Render,
//     Frame, Pump, Prepare, Reset or warm-up, then checks the whole backbuffer
//     and the restored State against the pre-export baseline;
//   * removes its own observer before the existing Compare continues.
// Unset or any other value leaves every existing call and GPU access unchanged.
bool RestoreFirstFrameRequested(){
 const char* value=std::getenv("PROBE_RESTORE_FIRST_FRAME");
 return value!=nullptr&&std::strcmp(value,"1")==0;
}
using RestoreFirstFrameRenderScene=void(*)(MMDApp*);
RestoreFirstFrameRenderScene restoreFirstFrameTarget=nullptr;
RestoreFirstFrameRenderScene realRestoreFirstFrameRenderScene=nullptr;
MMDApp* restoreFirstFrameApp=nullptr;
unsigned restoreFirstFrameSceneCalls=0;
void ObserveRestoreFirstFrameRenderScene(MMDApp* app){
 if(app!=nullptr&&app==restoreFirstFrameApp)++restoreFirstFrameSceneCalls;
 realRestoreFirstFrameRenderScene(app);
}
void EnableRestoreFirstFrameObservation(MMDApp* app){
 restoreFirstFrameApp=app;
 restoreFirstFrameSceneCalls=0;
 restoreFirstFrameTarget=&RenderFrameScene;
 Hook(restoreFirstFrameTarget,&ObserveRestoreFirstFrameRenderScene,&realRestoreFirstFrameRenderScene);
 std::printf("FIRST_FRAME hook=installed target=%p trampoline=%p app=%p\n",
     reinterpret_cast<void*>(restoreFirstFrameTarget),
     reinterpret_cast<void*>(realRestoreFirstFrameRenderScene),static_cast<void*>(app));
}
void DisableRestoreFirstFrameObservation(){
 if(restoreFirstFrameTarget==nullptr||realRestoreFirstFrameRenderScene==nullptr)return;
 const MH_STATUS disabled=MH_DisableHook(reinterpret_cast<void*>(restoreFirstFrameTarget));
 const MH_STATUS removed=disabled==MH_OK?
     MH_RemoveHook(reinterpret_cast<void*>(restoreFirstFrameTarget)):MH_ERROR_NOT_CREATED;
 Check(disabled==MH_OK&&removed==MH_OK,"first-frame probe removes its RenderFrameScene observer");
 std::printf("FIRST_FRAME hook=removed disable_status=%d remove_status=%d target=%p\n",
     static_cast<int>(disabled),static_cast<int>(removed),
     reinterpret_cast<void*>(restoreFirstFrameTarget));
 if(removed==MH_OK)realRestoreFirstFrameRenderScene=nullptr;
 restoreFirstFrameApp=nullptr;
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
void Compare(MMDApp*a,const State&before,const std::vector<DWORD>&pixels,const char*name,bool observeDrawState=false){
 const auto callerDpi=testThreadDpiContext();
 SettleTemporal(a);
 if(observeDrawState)BeginDrawStateObservation(a,name);
 BeginHudEndpoint(a,name);
 Render(a);auto after=Snapshot(a,name);auto out=Pixels(a,name);
 if(observeDrawState)EndDrawStateObservation(name);
 Check(testDpiContextsEqual(callerDpi,testThreadDpiContext()),"preview preserves caller DPI context");
 Check(EqualRect(&before.present,&after.present),"presentation rectangle restored");
 Check(after.viewport.X+after.viewport.Width<=after.target.Width&&after.viewport.Y+after.viewport.Height<=after.target.Height,"viewport fits actual render target");
 Check(after.back.Width==before.back.Width&&after.back.Height==before.back.Height,"actual backbuffer dimensions restored");
 Check(after.target.Width==before.target.Width&&after.target.Height==before.target.Height,"actual render target dimensions restored");
 Check(std::memcmp(&after.viewport,&before.viewport,sizeof before.viewport)==0,"actual GPU viewport restored");
 Check(EqualRect(&before.rect,&after.rect),"host viewport rectangle restored");
 size_t changed=0,total=0,inkBefore=0,inkAfter=0;
 const bool uniformEndpoint=HudEndpointAt(name)&&hudUniformBackground;
 if(out.size()==pixels.size())for(int y=before.rect.top+30;y<before.rect.bottom-30;++y)for(int x=before.rect.left+10;x<before.rect.right-10;++x){
   size_t i=size_t(y)*before.back.Width+x;if(i>=out.size())continue;
   ++total;
   changed+=out[i]!=pixels[i]&&(!uniformEndpoint||!HudUniformPixel(x,y));
   inkBefore+=pixels[i]!=0xffffff;inkAfter+=out[i]!=0xffffff;
 }
 std::printf("PIXELS %s changed=%zu total=%zu ink_before=%zu ink_after=%zu\n",name,changed,total,inkBefore,inkAfter);
 Check(total>0&&inkBefore>0&&inkAfter>0,"scene remains rendered after export");
 if(HudEndpointAt(name)&&HudEndpointSkip()) {
  // The independent endpoint deliberately omits text to expose the bottom
  // image. Comparing it to a baseline containing text cannot be a regression
  // verdict; ordinary and normal-text endpoints retain the exact assertion.
  Check(out.size()==pixels.size(),"EXPORT-HUD-ENDPOINT bottom image preserves dimensions");
  std::printf("EXPORT-HUD-ENDPOINT bottom_image_only=1 text_baseline_equality=not_applicable\n");
 }else Check(out.size()==pixels.size()&&changed==0,"export restores identical static scene pixels");
 CheckRestoreGeometry(after,out,name);
}
}
int wmain(int argc,wchar_t**argv){
 std::setvbuf(stdout,nullptr,_IONBF,0);
 ParseHudEndpoint(argc,argv);
 if(argc>1 && (std::wstring(argv[1])==L"--main-export-private-desktop" ||
               std::wstring(argv[1])==L"--main-export-desktop-bootstrap" ||
               std::wstring(argv[1])==L"--main-export-hidden-desktop")) {
   std::printf("MAIN-EXPORT entered wmain state-bytes=%zu\n",sizeof(MMDAppState));
#if !MMD_ORIGINAL_PROFILE
   return MainExportEntry(argc,argv);
#else
   Need(false,"MAIN-EXPORT requires the full profile with the real H264 encoder");
#endif
 }
 Need(argc>=5,"args: dpi large export-width export-height [effect...]");
 // Reject the unsupported original-profile coordinator combination before the
 // fixture creates its window, D3D device or any Frame, so the clear Need
 // failure cannot be pre-empted by unsupported frame driving. Still only for
 // an exact PROBE_RESTORE_FIRST_FRAME=="1".
 const bool restoreFirstFrame=RestoreFirstFrameRequested();
 if(restoreFirstFrame)
  Need(!(mikumikustudio::build_profile::original&&coordinator_fixture::Enabled()),
      "first-frame probe rejects MMS_TEST_COORDINATOR on the original profile without the ABI coordinator");
 LoadDpiTestApis();Need(testDpiForWindow&&testThreadDpiContext&&testSetThreadDpiContext&&testWindowDpiContext&&testDpiContextsEqual,
     "Windows 10+ DPI APIs are available for the viewport probes");
 output=fs::current_path();
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 AddVectoredExceptionHandler(1,Fault);
 testSetThreadDpiContext(std::wstring(argv[1])==L"aware"?DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:DPI_AWARENESS_CONTEXT_UNAWARE);
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
 restoreGeometryMode=ParseRestoreGeometryMode();
 if(restoreFirstFrame){
  // The strict first production frame only exists for the ordinary geometry
  // fixture. Buffered observations would mask the failure this probe exposes.
  Need(restoreGeometryMode!=RestoreGeometryMode::off,
      "first-frame probe requires PROBE_RESTORE_GEOMETRY=legacy|fitted|hidden");
  Need(RestoreDiagnosticMode()==0,
      "first-frame probe rejects PROBE_RESTORE_DIAGNOSTIC buffer observation");
  Need(!RestoreTraceRequested(),
      "first-frame probe rejects PROBE_RESTORE_GEOMETRY_TRACE buffer observation");
 }
 if(restoreGeometryMode!=RestoreGeometryMode::off){
  // The named synthetic control is only valid for the plain ordinary fixture.
  // An extra effect, AA/scale override, camera-mode path or live MME/proxy
  // would change what the control actually renders, so reject those instead.
  Need(argc==5,"geometry probe loads no extra effect accessories");
  Need(!std::getenv("PROBE_AA")&&!std::getenv("PROBE_SCALE")&&
       !std::getenv("PROBE_CAMERA_MODES")&&!std::getenv("PROBE_REQUIRE_MME"),
       "geometry probe rejects AA, scale, camera-mode and required-MME overrides");
  Need(!GetModuleHandleW(L"MMEffect.dll")&&!GetModuleHandleW(L"MMHack.dll"),
       "geometry probe runs without a live MME or proxy module");
  ApplyRestoreGeometry(a);
  EnableRestoreGeometryTrace(a);
 }
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
 RefreshMainWindowViewport(a);
 BeginDrawStateObservation(a,"before");
 Render(a);
 auto before=Snapshot(a,"before");auto pixels=Pixels(a,"before");
 EndDrawStateObservation("before");
 PrintRestoreGeometry(a,"baseline");
 CheckRestoreGeometry(before,pixels,"before");
 const int oldW=a->RenderWidth(),oldH=a->RenderHeight();
 const auto windowDpi=testWindowDpiContext(a->MainWindow());
 const auto foreignDpi=std::wstring(argv[1])==L"aware"?DPI_AWARENESS_CONTEXT_UNAWARE:DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
 for(int cycle=0;cycle<2;++cycle){
  testSetThreadDpiContext(windowDpi);
  phase=cycle==0?"complete-export":"cancel-export";
  auto path=output/(cycle==0?L"complete.avi":L"cancel.avi");wcscpy_s(a->AviOutputPath(),256,path.c_str());
  a->Recorder()->selectedCodec=-1;a->AviOutputWidth()=_wtoi(argv[3]);a->AviOutputHeight()=_wtoi(argv[4]);
  a->AviRecordStartFrame()=0;a->AviRecordEndFrame()=cycle==0?1:30;a->AviRecordFps()=30;a->AviIncludeWave()=0;a->AviCloseAfterExport()=0;
  StartAviRecordWindow(a);Need(a->RecordingWindow()!=nullptr,"production AVI recording started");Snapshot(a,"recording");
  if(cycle==0&&std::getenv("PROBE_DPI_CHANGE")){testSetThreadDpiContext(foreignDpi);std::puts("INJECT thread DPI context changed before natural completion");}
  for(int i=0;i<120&&a->RecordingWindow();++i){Frame(a);if(cycle==1&&i==1){
    ComPtr<IDirect3DVertexBuffer9>blocker;
    if(std::getenv("PROBE_RESET_BLOCKER"))Need(SUCCEEDED(d->CreateVertexBuffer(64,0,D3DFVF_XYZ,D3DPOOL_DEFAULT,&blocker,nullptr)),"retain actual reset-blocking resource");
    if(std::getenv("PROBE_DPI_CHANGE")){testSetThreadDpiContext(foreignDpi);std::puts("INJECT thread DPI context changed during export");}
    FinishAviRecord(a);
    blocker.Reset();
  }}
  Need(a->RecordingWindow()==nullptr,"recording exits");Check(a->RenderWidth()==oldW&&a->RenderHeight()==oldH,"saved resolution restored");
  for(int i=0;i<(std::getenv("PROBE_AA")?32:4);++i)Frame(a);
  Compare(a,before,pixels,cycle==0?"after-complete":"after-cancel");
 }
 phase="resize-after";
 if(restoreFirstFrame)EnableRestoreFirstFrameObservation(a);
 const unsigned layoutSceneCallsBefore=restoreFirstFrameSceneCalls;
 HandleWindowSize(a);
 const unsigned layoutSceneCallsAfter=restoreFirstFrameSceneCalls;
 if(restoreFirstFrame){
  const unsigned layoutSceneCalls=layoutSceneCallsAfter-layoutSceneCallsBefore;
  Check(layoutSceneCallsAfter==layoutSceneCallsBefore,"first-frame layout does not enter RenderFrameScene");
  const auto firstCallerDpi=testThreadDpiContext();
  const auto coordinatorFramesBefore=coordinator_fixture::frames;
  phase="resize-after-first-frame";
  a->DeltaTime()=1.f/30;a->state.messageSeen=1;
  coordinator_fixture::Drive(a);
  const unsigned driveSceneCalls=restoreFirstFrameSceneCalls-layoutSceneCallsAfter;
  const auto coordinatorFrames=coordinator_fixture::frames-coordinatorFramesBefore;
  const bool coordinatorDriven=coordinator_fixture::Enabled();
  std::printf("FIRST_FRAME drive geometry=%s driver=%s scene_calls_before_layout=%u scene_calls_after_layout=%u "
      "layout_scene_calls=%u drive_scene_calls=%u scene_calls=%u coordinator_frames_delta=%llu\n",
      RestoreGeometryName(restoreGeometryMode),coordinatorDriven?"coordinator":"frame-driver",
      layoutSceneCallsBefore,layoutSceneCallsAfter,layoutSceneCalls,driveSceneCalls,
      restoreFirstFrameSceneCalls-layoutSceneCallsBefore,
      static_cast<unsigned long long>(coordinatorFrames));
  Check(driveSceneCalls==1,"first-frame Drive enters RenderFrameScene exactly once");
  if(coordinatorDriven)
   Check(coordinatorFrames==1,"first-frame coordinator Drive enters FrameDriver exactly once");
  const auto firstState=Snapshot(a,"after-layout-first-frame");
  const auto firstPixels=Pixels(a,"after-layout-first-frame");
  Check(restoreFirstFrameSceneCalls-layoutSceneCallsAfter==driveSceneCalls,
      "first-frame snapshot and capture add no further RenderFrameScene call");
  Check(testDpiContextsEqual(firstCallerDpi,testThreadDpiContext()),
      "first-frame capture preserves caller DPI context");
  Check(EqualRect(&before.present,&firstState.present),"first-frame presentation rectangle restored");
  Check(firstState.viewport.X+firstState.viewport.Width<=firstState.target.Width&&
        firstState.viewport.Y+firstState.viewport.Height<=firstState.target.Height,
        "first-frame viewport fits actual render target");
  Check(firstState.back.Width==before.back.Width&&firstState.back.Height==before.back.Height,
      "first-frame actual backbuffer dimensions restored");
  Check(firstState.target.Width==before.target.Width&&firstState.target.Height==before.target.Height,
      "first-frame actual render target dimensions restored");
  Check(std::memcmp(&firstState.viewport,&before.viewport,sizeof before.viewport)==0,
      "first-frame actual GPU viewport restored");
  Check(EqualRect(&before.rect,&firstState.rect),"first-frame host viewport rectangle restored");
  std::size_t wholeChanged=0;
  if(firstPixels.size()==pixels.size())
   for(std::size_t i=0;i<firstPixels.size();++i)wholeChanged+=firstPixels[i]!=pixels[i];
  std::printf("FIRST_FRAME whole_changed=%zu pixels=%zu baseline=%zu size_match=%d\n",
      wholeChanged,firstPixels.size(),pixels.size(),firstPixels.size()==pixels.size()?1:0);
  if(before.back.Width!=0&&(firstPixels.size()!=pixels.size()||wholeChanged!=0)){
   const std::size_t count=firstPixels.size()<pixels.size()?firstPixels.size():pixels.size();
   unsigned shown=0;
   for(std::size_t i=0;i<count&&shown<12;++i){
    if(firstPixels[i]==pixels[i])continue;
    std::printf("FIRST_FRAME diff%u x=%zu y=%zu baseline=%06lx first=%06lx\n",shown,
        i%before.back.Width,i/before.back.Width,
        static_cast<unsigned long>(pixels[i]),static_cast<unsigned long>(firstPixels[i]));
    ++shown;
   }
  }
  Check(firstPixels.size()==pixels.size()&&wholeChanged==0,
      "first-frame whole backbuffer matches the pre-export baseline");
  CheckRestoreGeometry(firstState,firstPixels,"after-layout-first-frame");
  DisableRestoreFirstFrameObservation();
  phase="resize-after";
 }
 Compare(a,before,pixels,"after-layout",true);
 if(HudEndpointAt("after-layout"))return FinishHudEndpoint();
 // PROBE_RESTORE_DIAGNOSTIC=1|2|3|4|5 adds post-failure causal controls after
 // the comparison above. Modes 1..4 keep the original stages and order. Mode 5
 // observes the real frame driver: one plain Render repeat, then two real
 // Frame calls that must not be followed by any extra Render or
 // PrepareFrameTextOverlay. All modes compare whole frames against the original
 // pre-export baseline without cropping or tolerance, and never replace the
 // earlier assertions or the final failure exit.
 const int diagMode=RestoreDiagnosticMode();
 if(diagMode!=0){
  const auto report=[&](const char* label,const std::vector<DWORD>& stage){
   std::size_t changed=0;
   if(stage.size()==pixels.size()){
    for(std::size_t i=0;i<stage.size();++i)changed+=stage[i]!=pixels[i];
   }
   std::printf("DIAGNOSTIC %s vs baseline size=%zu/%zu changed=%zu\n",label,stage.size(),pixels.size(),changed);
   if(stage.size()!=pixels.size()||before.back.Width==0)return;
   unsigned shown=0;
   for(std::size_t i=0;i<stage.size()&&shown<12;++i){
    if(stage[i]==pixels[i])continue;
    std::printf("DIAGNOSTIC %s diff%u x=%zu y=%zu baseline=%06lx stage=%06lx\n",label,shown,
        i%before.back.Width,i/before.back.Width,
        static_cast<unsigned long>(pixels[i]),static_cast<unsigned long>(stage[i]));
    ++shown;
   }
  };
  const char* savedPhase=phase;
  phase="diag-repeat";BeginDrawStateObservation(a,"diag-repeat");BeginHudEndpoint(a,"diag-repeat");Render(a);report("diag-repeat",Pixels(a,"diag-repeat"));EndDrawStateObservation("diag-repeat");
  if(HudEndpointAt("diag-repeat"))return FinishHudEndpoint();
  if(diagMode==5){
   // The two Frames below must stay the last frame work before their Pixels
   // readback: a Render or PrepareFrameTextOverlay here would hide the real
   // first-frame output this mode exists to observe.
   phase="diag-real-frame-1";Frame(a);report("diag-real-frame-1",Pixels(a,"diag-real-frame-1"));
   phase="diag-real-frame-2";Frame(a);report("diag-real-frame-2",Pixels(a,"diag-real-frame-2"));
  }else{
   phase="diag-text-refresh";PrepareFrameTextOverlay(a);BeginDrawStateObservation(a,"diag-text-refresh");Render(a);report("diag-text-refresh",Pixels(a,"diag-text-refresh"));EndDrawStateObservation("diag-text-refresh");
   phase="diag-frame";Frame(a);Render(a);report("diag-frame",Pixels(a,"diag-frame"));
  }
  phase=savedPhase;
 }
 if(std::getenv("PROBE_DPI_CHANGE")){
   phase="restore-dpi";testSetThreadDpiContext(windowDpi);HandleWindowSize(a);
   for(int i=0;i<(std::getenv("PROBE_AA")?32:4);++i)Frame(a);
   Compare(a,before,pixels,"after-dpi-restored");
 }
 // Exercise the geometry entry points even when called outside WndProc.
 // Windows temporarily corrects DPI inside WndProc, which can otherwise hide
 // this regression in ordinary resize-only tests.
 testSetThreadDpiContext(windowDpi);
 HWND floating=CreateWindowExW(0,L"STATIC",L"DPI regression",WS_OVERLAPPEDWINDOW,
     10,10,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 Need(floating!=nullptr,"create hidden floating viewport");
 for(int mode=0;mode<3;++mode){
   a->FullscreenMode()=mode==1;
   a->FloatingWindow()=mode==2?floating:nullptr;
   testSetThreadDpiContext(windowDpi);
   RefreshMainWindowViewport(a);
   const auto expected=Snapshot(a,"mode-baseline");
   testSetThreadDpiContext(foreignDpi);
   RefreshMainWindowViewport(a);
   const auto actual=Snapshot(a,"mode-foreign-dpi");
   Check(EqualRect(&expected.rect,&actual.rect),"main/fullscreen/floating scene geometry ignores caller DPI");
   Check(EqualRect(&expected.present,&actual.present),"main/fullscreen/floating presentation ignores caller DPI");
   Check(std::memcmp(&expected.viewport,&actual.viewport,sizeof actual.viewport)==0,"main/fullscreen/floating GPU viewport ignores caller DPI");
   Check(testDpiContextsEqual(foreignDpi,testThreadDpiContext()),"geometry scopes restore caller DPI");
 }
 a->FullscreenMode()=0;a->FloatingWindow()=nullptr;
 DestroyWindow(floating);testSetThreadDpiContext(windowDpi);HandleWindowSize(a);
 RECT panelBefore{}, panelAfter{};
 const HWND panel=GetDlgItem(a->MainWindow(),557);
 Need(panel!=nullptr,"viewport toolbar exists");
 GetWindowRect(panel,&panelBefore);
 testSetThreadDpiContext(foreignDpi);
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
 Check(testDpiContextsEqual(foreignDpi,testThreadDpiContext()),"fullscreen transition preserves caller DPI");
 testSetThreadDpiContext(windowDpi);
 if(std::getenv("PROBE_RESET_BLOCKER"))Check(resetFailures>0,"reset failure injection actually reached the driver");
 Check(viewportFailures==0,"viewport API calls succeed");
 RestoreTracePostAssertionBuffers();
 RemoveDrawStateHook();
 std::printf("SUMMARY failures=%d viewport_failures=%d reset_failures=%d resets=%u\n",failures,viewportFailures,resetFailures,resets);
 phase="shutdown";ShutdownCleanup(a);g_Block=nullptr;delete a;UnhookWindowsHookEx(cbt);
 return failures?1:0;
}
