#include "device_probe_base.hpp"
#include "../com_wrappers.hpp"
#include <d3d9on12.h>
#include <iostream>
#include <stdexcept>
using namespace mme_re::mmhack;
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Surface final : IDirect3DSurface9 {
  ULONG refs = 1;
  IUnknown* retained = nullptr;
  D3DSURFACE_DESC desc{D3DFMT_A8R8G8B8,D3DRTYPE_SURFACE,D3DUSAGE_RENDERTARGET,D3DPOOL_DEFAULT,D3DMULTISAMPLE_8_SAMPLES,0,640,480};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { require(refs>0,"Surface underflow");const auto remaining = --refs; if (!remaining && retained) { auto* owner = retained; retained = nullptr; owner->Release(); } return remaining; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID,void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* output) override { *output=desc;return S_OK; }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT*,const RECT*,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return E_NOTIMPL; }
};
struct SwapChain final : IDirect3DSwapChain9 {
  ULONG refs = 1;
  IDirect3DDevice9* retained = nullptr;
  Surface surface;
  D3DPRESENT_PARAMETERS params{};
  unsigned presents = 0;
  RECT lastSource{},lastDestination{};
  bool sourceNull=false,destinationNull=false;
  HWND lastWindow=nullptr;
  const RGNDATA* lastDirty=nullptr;
  HRESULT result=S_OK;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    require(refs>0,"Swapchain underflow");--refs;
    if(!refs && retained){auto* device=retained;retained=nullptr;device->Release();}
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT* source,const RECT* destination,HWND window,const RGNDATA* dirty,DWORD flags) override {
    require(!source && !destination && !window && !dirty,"Child viewport must present its whole buffer");
    require(flags==0,"Unexpected swapchain flags");
    ++presents;sourceNull=!source;destinationNull=!destination;
    if(source)lastSource=*source;if(destination)lastDestination=*destination;
    lastWindow=window;lastDirty=dirty;return result;
  }
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index,D3DBACKBUFFER_TYPE,IDirect3DSurface9** output) override {
    require(index==0,"Wrong backbuffer index");*output=&surface;surface.AddRef();return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* output) override { *output=params;return S_OK; }
};
struct Device final : test::DeviceProbeBase {
  explicit Device(HWND window) : focus(window) {
    implicit.params.BackBufferWidth=640;implicit.params.BackBufferHeight=480;
    implicit.params.BackBufferFormat=D3DFMT_A8R8G8B8;implicit.params.BackBufferCount=1;
    implicit.params.MultiSampleType=D3DMULTISAMPLE_8_SAMPLES;
    implicit.params.SwapEffect=D3DSWAPEFFECT_DISCARD;implicit.params.Windowed=TRUE;
    implicit.params.EnableAutoDepthStencil=TRUE;implicit.params.hDeviceWindow=window;
    copy.refs=0;copy.surface.refs=0;resolved.refs=0;
  }
  ULONG refs=1;
  HWND focus;
  bool on12=true;
  unsigned creates=0,originalPresents=0,resolves=0,crops=0,resets=0,ends=0;
  HRESULT createResult=S_OK,resolveResult=S_OK,presentResult=S_OK,resetResult=S_OK;
  SwapChain implicit,copy;
  Surface resolved;
  RECT cropped{};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
    *output=nullptr;
    if(on12 && iid==__uuidof(IDirect3DDevice9On12)){*output=static_cast<IDirect3DDevice9*>(this);AddRef();return S_OK;}
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { require(refs>0,"Device underflow");return --refs; }
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override {
    *params={};params->hFocusWindow=focus;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,IDirect3DSwapChain9** output) override {
    require(index==0,"Wrong implicit chain");*output=&implicit;implicit.AddRef();return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*,IDirect3DSwapChain9**) override {
    throw std::runtime_error("On12 output must not use a D3D9 swap chain");
  }
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT width,UINT height,D3DFORMAT format,D3DMULTISAMPLE_TYPE samples,DWORD quality,BOOL lockable,IDirect3DSurface9** output,HANDLE*) override {
    *output=nullptr;
    if(FAILED(createResult))return createResult;
    require(samples==D3DMULTISAMPLE_NONE && quality==0 && !lockable,"Invalid staging surface");
    Surface* target=nullptr;
    if(copy.surface.refs==0) {
      ++creates;
      require(copy.refs==0,"Old viewport survived recreation");
      copy.refs=1;copy.retained=this;AddRef();
      copy.surface.retained=&copy;
      copy.params.BackBufferWidth=width;copy.params.BackBufferHeight=height;
      copy.params.BackBufferFormat=format;
      target=&copy.surface;
    } else {
      require(resolved.refs==0,"Old resolve surface survived recreation");
      target=&resolved;
    }
    target->desc=implicit.surface.desc;target->desc.Width=width;target->desc.Height=height;
    target->desc.Format=format;target->desc.MultiSampleType=samples;
    target->refs=1;*output=target;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* source,const RECT* sourceRect,IDirect3DSurface9* destination,const RECT* destinationRect,D3DTEXTUREFILTERTYPE filter) override {
    if(destination==&resolved) {
      require(source==&implicit.surface && !sourceRect && !destinationRect && filter==D3DTEXF_NONE,"MSAA resolve must copy complete matching surfaces");
      ++resolves;return resolveResult;
    }
    require(source==(implicit.surface.desc.MultiSampleType==D3DMULTISAMPLE_NONE?&implicit.surface:&resolved) && destination==&copy.surface,"Wrong crop surfaces");
    require(sourceRect && !destinationRect && filter==D3DTEXF_NONE,"Viewport crop missing");
    cropped=*sourceRect;++crops;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT*,const RECT*,HWND,const RGNDATA*) override { ++originalPresents;return presentResult; }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* params) override {
    require(copy.refs==0 && resolved.refs==0,"Viewport resources survived into Reset");++resets;
    if(SUCCEEDED(resetResult))implicit.params=*params;
    return resetResult;
  }
  HRESULT STDMETHODCALLTYPE EndScene() override { ++ends;return S_OK; }
};
struct Output final : On12Output {
  HRESULT Present(IDirect3DDevice9* base, IDirect3DSurface9* surface, HWND window) override {
    auto& device=*static_cast<Device*>(base);
    require(surface==&device.copy.surface,"Wrong presentation surface");
    require(GetParent(window)!=nullptr && window!=device.implicit.params.hDeviceWindow,"Output must target a child HWND");
    device.copy.params.hDeviceWindow=window;
    return device.copy.Present(nullptr,nullptr,nullptr,nullptr,0);
  }
};
std::unique_ptr<On12Output> createOutput() { return std::make_unique<Output>(); }
struct Window {
  HWND value=CreateWindowExW(0,L"STATIC",L"MMHack present test",WS_POPUP,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  Window(){require(value!=nullptr,"Hidden window creation failed");}
  ~Window(){DestroyWindow(value);}
};
void directPaths() {
  Window window;
  RECT rect{12,20,400,300};
  Device dx9(window.value);dx9.on12=false;
  On12Presenter dx9Presenter(createOutput);
  dx9.presentResult=D3DERR_DEVICELOST;
  require(dx9Presenter.Present(&dx9,&rect,&rect,window.value,nullptr)==D3DERR_DEVICELOST,"DX9 result changed");
  require(dx9.originalPresents==1 && !dx9.creates && !dx9.resolves,"DX9 was redirected");
  Device device(window.value);On12Presenter presenter(createOutput);
  device.implicit.params.hDeviceWindow=nullptr;
  require(presenter.Present(&device,nullptr,nullptr,nullptr,nullptr)==S_OK,"Borderless fullscreen Present failed");
  require(device.creates==1 && device.copy.params.BackBufferWidth==640 && device.copy.params.BackBufferHeight==480 && !device.originalPresents,"Borderless fullscreen used the GDI parent's implicit chain");
  const auto child=device.copy.params.hDeviceWindow;
  device.implicit.params.Windowed=FALSE;
  require(presenter.Present(&device,&rect,nullptr,nullptr,nullptr)==S_OK,"Fullscreen path changed");
  require(device.originalPresents==1 && !IsWindow(child),"Exclusive fullscreen retained the windowed child");
}
void copiesAndFailures() {
  Window window,other;
  Device device(window.value);On12Presenter presenter(createOutput);
  RECT source{10,20,300,250},destination{20,40,600,460};RGNDATA dirty{};
  require(presenter.Present(&device,&source,&destination,nullptr,&dirty)==S_OK,"Partial Present failed");
  require(device.creates==1 && device.resolves==1 && device.crops==1 && device.copy.presents==1 && !device.originalPresents,"Must resolve, crop and present exactly once");
  require(EqualRect(&device.cropped,&source) && device.copy.params.BackBufferWidth==580 && device.copy.params.BackBufferHeight==420,"Viewport crop or size changed");
  const auto child=device.copy.params.hDeviceWindow;
  RECT childRect{};GetWindowRect(child,&childRect);MapWindowPoints(nullptr,window.value,reinterpret_cast<POINT*>(&childRect),2);
  require(EqualRect(&childRect,&destination) && !IsWindowEnabled(child),"Child placement or input transparency failed");
  require(SendMessageW(child,WM_NCHITTEST,0,0)==HTTRANSPARENT,"Child intercepted editor input");
  ShowWindow(child,SW_HIDE);
  require(presenter.Present(&device,&source,&destination,nullptr,nullptr)==S_OK &&
      (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE),"Host-hidden viewport was not restored");
  const RECT invalid{-1,0,100,100};
  require(presenter.Present(&device,&invalid,&destination,nullptr,nullptr)==D3DERR_INVALIDCALL,
      "Out-of-bounds source was accepted");
  source={0,0,320,240};
  require(presenter.Present(&device,&source,nullptr,window.value,nullptr)==S_OK,"Source-only Present failed");
  require(device.creates==2 && EqualRect(&source,&device.cropped),"Client-sized destination was not rebuilt");
  require(presenter.Present(&device,&destination,nullptr,window.value,nullptr)==S_OK && device.creates==2,"Changed source must reuse matching output");
  require(SetWindowPos(window.value,nullptr,0,0,700,500,SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Resize failed");
  require(presenter.Present(&device,nullptr,nullptr,window.value,&dirty)==S_OK && device.creates==3,"Window resize did not rebuild viewport chain");
  require(presenter.Present(&device,&source,&destination,other.value,nullptr)==S_OK && device.creates==4,"Window override did not rebuild viewport chain");
  device.implicit.surface.desc.Width=800;
  require(presenter.Present(&device,&source,&destination,other.value,nullptr)==S_OK && device.creates==5 && device.resolved.desc.Width==800,"Actual buffer resize was ignored");
  const auto prior=device.copy.presents;
  device.resolveResult=D3DERR_DEVICELOST;
  require(presenter.Present(&device,&source,&destination,other.value,nullptr)==D3DERR_DEVICELOST && device.copy.presents==prior && !device.originalPresents,"Resolve failure presented stale pixels or fell back");
  device.resolveResult=S_OK;device.copy.result=D3DERR_DEVICELOST;
  require(presenter.Present(&device,&source,&destination,other.value,nullptr)==D3DERR_DEVICELOST,"DXGI output failure was hidden");
  presenter.Reset();require(device.copy.refs==0 && device.resolved.refs==0 && device.refs==1,"Reset retained viewport resources");
  device.createResult=E_OUTOFMEMORY;
  require(presenter.Present(&device,&source,&destination,other.value,nullptr)==E_OUTOFMEMORY && !device.originalPresents,"Create failure used invalid original Present");
  device.createResult=S_OK;device.copy.result=S_OK;
  device.implicit.surface.desc.MultiSampleType=D3DMULTISAMPLE_NONE;
  device.implicit.surface.desc.Format=D3DFMT_X8R8G8B8;
  device.implicit.params.SwapEffect=D3DSWAPEFFECT_COPY;
  const auto oldResolves=device.resolves;
  require(presenter.Present(&device,&source,&destination,other.value,nullptr)==S_OK && device.resolves==oldResolves && !device.resolved.refs,"Non-MSAA COPY path performed an MSAA resolve");
  require(device.copy.surface.desc.Format==D3DFMT_A8R8G8B8,"XRGB output was not converted to a DXGI flip-compatible format");
  require(presenter.Present(&device,nullptr,nullptr,nullptr,nullptr)==S_OK && !device.originalPresents,"Null rectangles reverted to the GDI parent");
  const auto lastChild=device.copy.params.hDeviceWindow;
  device.implicit.params.Windowed=FALSE;
  require(presenter.Present(&device,nullptr,nullptr,nullptr,nullptr)==S_OK && !IsWindow(lastChild) && device.originalPresents==1,"Exclusive presentation left a stale child covering the parent");
}
unsigned endCallbacks=0,lostCallbacks=0,resetCallbacks=0,cleanupCallbacks=0;
void WINAPI ended(IDirect3DDevice9*) { ++endCallbacks; }
void WINAPI lost(IDirect3DDevice9* value) { require(static_cast<Device*>(value)->copy.refs==0,"Viewport retained before MME lost callback");++lostCallbacks; }
HRESULT WINAPI reset(IDirect3DDevice9*) { ++resetCallbacks; return S_OK; }
void WINAPI cleanup(IDirect3DDevice9* value) { require(static_cast<Device*>(value)->copy.refs==0,"Viewport retained before MME cleanup");++cleanupCallbacks; }
void wrapperLifecycle() {
  Window window;State state;Runtime runtime(state);Device device(window.value);
  runtime.callbacks.end_scene=ended;runtime.callbacks.lost_device=lost;
  runtime.callbacks.reset_device=reset;runtime.callbacks.cleanup=cleanup;
  auto* wrapper=new DeviceWrapper(&device,runtime,createOutput);RECT source{0,0,400,300};
  require(wrapper->EndScene()==S_OK && endCallbacks==1 && device.ends==1,"MME EndScene was bypassed");
  require(wrapper->Present(&source,&source,window.value,nullptr)==S_OK && state.drawn_window==window.value,"Drawn window was not preserved");
  auto params=device.implicit.params;device.resetResult=D3DERR_DEVICELOST;
  require(wrapper->Reset(&params)==D3DERR_DEVICELOST && device.resets==1 && lostCallbacks==1 && resetCallbacks==1,"Failed Reset lifecycle changed");
  device.resetResult=S_OK;require(wrapper->Reset(&params)==S_OK,"Reset failed");
  require(wrapper->Present(&source,&source,nullptr,nullptr)==S_OK && state.drawn_window==window.value,"Present after Reset failed");
  require(wrapper->Release()==0 && device.refs==0 && device.copy.refs==0 && cleanupCallbacks==1 && runtime.device_wrappers.empty(),"Final wrapper release retained viewport/device or skipped cleanup");
}
}
int main() {
  try { directPaths();copiesAndFailures();wrapperLifecycle();std::cout<<"On12 Present passed: child viewport, MSAA resolve/crop, input, resize, DX9/fullscreen, failures, Reset/Release and MME callbacks\n";return 0; }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
