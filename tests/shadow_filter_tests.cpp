// Hidden production-host acceptance test; run each invocation in its own directory.
// Entry: MikuMikuDance.exe <model-without-FX.pmx> <new-output-directory> --mme 0|1
// Link like mme_shadow_filter_tests: mikumikustudio_core, MinHook, winmm,
// dsound, generated resources, res/MMD.rc and exports/MikuMikuDance.def.
// Keep OUTPUT_NAME=MikuMikuDance and ENABLE_EXPORTS=ON for the real MME case.
// Place the real MME DLLs beside only the --mme 1 copy. Use the default desktop;
// all windows created by this process are hidden. No other process is touched.
#define NOMINMAX
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace mikumikustudio;
using namespace mikumikustudio::d3dx;
using Microsoft::WRL::ComPtr;
namespace sf = mikumikustudio::shadow_filter;
namespace {
unsigned checks = 0;
void Check(bool good, const char* why) {
    ++checks;
    std::printf("%s %s\n", good ? "PASS" : "FAIL", why);
    if (!good) throw std::runtime_error(why);
}
void Hr(HRESULT hr, const char* why) {
    if (FAILED(hr)) std::printf("HRESULT %08lx\n", hr);
    Check(SUCCEEDED(hr), why);
}

decltype(&ShowWindow) realShow = nullptr;
BOOL WINAPI Hide(HWND window, int) { return realShow(window, SW_HIDE); }
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::fprintf(stderr, "FAIL unexpected dialog: %s\n", text);
    std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected dialog: %ls\n", text);
    std::exit(2);
}
LRESULT CALLBACK Guard(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            create->x = create->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

MMDApp* host = nullptr;
IDirect3DDevice9* ownedDevice = nullptr;
UINT deviceLimit = 0;
void* createEntry = nullptr;
void* drawEntry = nullptr;
std::filesystem::path output;
std::ofstream draws, summary;

// A private-data IUnknown lets the test observe actual COM resource destruction
// without keeping the texture alive or assuming a driver's AddRef return value.
const GUID lifetimeGuid = {0x68145d8e,0xbac0,0x48c5,{0x88,0xf8,0x8a,0x09,0x61,0x94,0xe7,0x1d}};
using DeathFlag = std::shared_ptr<std::atomic<bool>>;
struct LifetimeProbe final : IUnknown {
    std::atomic<ULONG> references{1};
    DeathFlag died;
    explicit LifetimeProbe(DeathFlag value) : died(std::move(value)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references;
        if (!left) { died->store(true); delete this; }
        return left;
    }
};
std::unordered_map<IDirect3DTexture9*, DeathFlag> lifetimes;
using CreateTexture = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD,
                                      D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
CreateTexture realCreateTexture = nullptr;
bool probeAllocations = false, failOneAllocation = false;
UINT probeSize = 0;
unsigned allocationAttempts = 0, injectedFailures = 0, probeErrors = 0;
HRESULT WINAPI CreateTextureProbe(IDirect3DDevice9* device, UINT width, UINT height,
    UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** texture, HANDLE* shared) {
    const bool match = probeAllocations && device == ownedDevice &&
        width == probeSize && height == probeSize && format == D3DFMT_R32F &&
        (usage & D3DUSAGE_RENDERTARGET) && pool == D3DPOOL_DEFAULT;
    if (match) {
        ++allocationAttempts;
        if (failOneAllocation) {
            failOneAllocation = false;
            ++injectedFailures;
            if (texture) *texture = nullptr;
            return D3DERR_OUTOFVIDEOMEMORY;
        }
    }
    const HRESULT hr = realCreateTexture(device, width, height, levels, usage,
                                         format, pool, texture, shared);
    if (match && SUCCEEDED(hr) && texture && *texture) {
        auto died = std::make_shared<std::atomic<bool>>(false);
        auto* probe = new LifetimeProbe(died);
        const HRESULT marked = (*texture)->SetPrivateData(lifetimeGuid, probe,
                                                          sizeof(IUnknown*), D3DSPD_IUNKNOWN);
        probe->Release();
        if (FAILED(marked)) ++probeErrors;
        else lifetimes[*texture] = std::move(died);
    }
    return hr;
}
DeathFlag Life(IDirect3DTexture9* texture) {
    const auto found = lifetimes.find(texture);
    Check(found != lifetimes.end() && probeErrors == 0, "fine texture has a destruction probe");
    return found->second;
}

UINT HardwareLimit(IDirect3DDevice9* device) {
    D3DCAPS9 caps{}; Hr(device->GetDeviceCaps(&caps), "read actual device limits");
    return (std::min)({8192u, UINT(caps.MaxTextureWidth), UINT(caps.MaxTextureHeight)});
}
UINT FineSize(UINT coarse, UINT cap) { return (std::min)(cap, (std::max)(4096u, coarse * 2)); }
D3DSURFACE_DESC TextureDesc(IDirect3DTexture9* texture) {
    D3DSURFACE_DESC desc{};
    Check(texture != nullptr, "texture exists");
    Hr(texture->GetLevelDesc(0, &desc), "read real texture dimensions");
    return desc;
}
bool SameObject(IUnknown* a, IUnknown* b) {
    if (!a || !b) return a == b;
    ComPtr<IUnknown> ia, ib;
    if (FAILED(a->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(ia.GetAddressOf()))) ||
        FAILED(b->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(ib.GetAddressOf())))) return false;
    return ia.Get() == ib.Get();
}

// Independent snapshot used to check TargetScope rather than trusting it to
// verify itself. The state block also keeps the host usable after fixture work.
struct DeviceSnapshot {
    IDirect3DDevice9* device;
    ComPtr<IDirect3DStateBlock9> state;
    std::array<ComPtr<IDirect3DSurface9>,4> targets;
    ComPtr<IDirect3DSurface9> depth;
    ComPtr<IDirect3DBaseTexture9> sampler0;
    D3DVIEWPORT9 viewport{};
    DWORD z = 0, cull = 0, mag = 0;
    UINT count = 0;
    bool active = true;
    explicit DeviceSnapshot(IDirect3DDevice9* value) : device(value) {
        D3DCAPS9 caps{}; Hr(device->GetDeviceCaps(&caps), "snapshot caps");
        count = (std::clamp)(UINT(caps.NumSimultaneousRTs),1u,4u);
        Hr(device->CreateStateBlock(D3DSBT_ALL,&state), "snapshot state block");
        Hr(state->Capture(), "capture independent device state");
        for (UINT i=0;i<count;++i) {
            const HRESULT hr=device->GetRenderTarget(i,&targets[i]);
            Check(SUCCEEDED(hr) || (i && hr==D3DERR_NOTFOUND), "snapshot render targets");
        }
        const HRESULT hr=device->GetDepthStencilSurface(&depth);
        Check(SUCCEEDED(hr)||hr==D3DERR_NOTFOUND, "snapshot depth");
        Hr(device->GetViewport(&viewport), "snapshot viewport");
        Hr(device->GetTexture(0,&sampler0), "snapshot sampler texture");
        Hr(device->GetRenderState(D3DRS_ZENABLE,&z), "snapshot depth state");
        Hr(device->GetRenderState(D3DRS_CULLMODE,&cull), "snapshot cull state");
        Hr(device->GetSamplerState(0,D3DSAMP_MAGFILTER,&mag), "snapshot filter state");
    }
    void Verify() {
        for (UINT i=0;i<count;++i) {
            ComPtr<IDirect3DSurface9> value;
            const HRESULT hr=device->GetRenderTarget(i,&value);
            Check((SUCCEEDED(hr)||hr==D3DERR_NOTFOUND) && SameObject(value.Get(),targets[i].Get()),
                  "TargetScope restored render target identity");
        }
        ComPtr<IDirect3DSurface9> liveDepth; device->GetDepthStencilSurface(&liveDepth);
        Check(SameObject(liveDepth.Get(),depth.Get()), "TargetScope restored depth identity");
        D3DVIEWPORT9 live{}; Hr(device->GetViewport(&live), "read restored viewport");
        Check(std::memcmp(&live,&viewport,sizeof(live))==0, "TargetScope restored exact viewport");
        ComPtr<IDirect3DBaseTexture9> texture; Hr(device->GetTexture(0,&texture), "read restored sampler");
        Check(SameObject(texture.Get(),sampler0.Get()), "TargetScope restored sampler texture");
        DWORD value=0;
        Hr(device->GetRenderState(D3DRS_ZENABLE,&value), "read restored depth state");
        Check(value==z, "TargetScope restored depth enable");
        Hr(device->GetRenderState(D3DRS_CULLMODE,&value), "read restored cull state");
        Check(value==cull, "TargetScope restored cull mode");
        Hr(device->GetSamplerState(0,D3DSAMP_MAGFILTER,&value), "read restored sampler filter");
        Check(value==mag, "TargetScope restored sampler filter");
    }
    void Restore() noexcept {
        if (!active) return;
        active=false;
        device->SetDepthStencilSurface(nullptr);
        for(UINT i=1;i<count;++i)device->SetRenderTarget(i,nullptr);
        device->SetRenderTarget(0,targets[0].Get());
        for(UINT i=1;i<count;++i)if(targets[i])device->SetRenderTarget(i,targets[i].Get());
        device->SetDepthStencilSurface(depth.Get());
        state->Apply();device->SetViewport(&viewport);
    }
    ~DeviceSnapshot() { Restore(); }
};

struct CoarseFixture {
    D3DRenderer* renderer;
    IDirect3DTexture9* oldTexture;
    IDirect3DSurface9* oldSurface;
    IDirect3DSurface9* oldDepth;
    int oldWidth,oldHeight;
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface,depth;
    explicit CoarseFixture(UINT size) : renderer(host->Renderer()),
        oldTexture(renderer->hdrTexture),oldSurface(renderer->shadowSurface),
        oldDepth(renderer->shadowDepthSurface),oldWidth(renderer->renderTargetWidth),
        oldHeight(renderer->renderTargetHeight) {
        probeAllocations=false;
        sf::ReleaseRenderer(renderer,true);
        auto* device=renderer->device;
        Hr(device->CreateTexture(size,size,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,
                                 D3DPOOL_DEFAULT,&texture,nullptr), "create actual coarse texture");
        Hr(texture->GetSurfaceLevel(0,&surface), "coarse level-zero surface");
        Hr(device->CreateDepthStencilSurface(size,size,D3DFMT_D24X8,D3DMULTISAMPLE_NONE,
                                            0,FALSE,&depth,nullptr), "create actual coarse depth");
        // A cleared, current-frame coarse map is enough for API lifetime tests.
        // The later FrameDriver section separately exercises real scene casters.
        {
            DeviceSnapshot saved(device);
            Hr(device->SetDepthStencilSurface(nullptr), "detach depth for coarse clear");
            for(UINT i=1;i<saved.count;++i)Hr(device->SetRenderTarget(i,nullptr), "detach MRT for coarse clear");
            Hr(device->SetRenderTarget(0,surface.Get()), "bind fixture coarse target");
            Hr(device->SetDepthStencilSurface(depth.Get()), "bind fixture coarse depth");
            Hr(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xffffffff,1,0), "initialize current coarse depth");
        }
        renderer->hdrTexture=texture.Get();renderer->shadowSurface=surface.Get();
        renderer->shadowDepthSurface=depth.Get();renderer->renderTargetWidth=renderer->renderTargetHeight=size;
        probeSize=FineSize(size,HardwareLimit(device));probeAllocations=true;
    }
    ~CoarseFixture() {
        probeAllocations=false;
        sf::ReleaseRenderer(renderer,true);
        renderer->hdrTexture=oldTexture;renderer->shadowSurface=oldSurface;
        renderer->shadowDepthSurface=oldDepth;renderer->renderTargetWidth=oldWidth;renderer->renderTargetHeight=oldHeight;
    }
};

std::array<float,4> EffectSize() {
    sf::ConfigureEffect(host,host->Renderer()->effect);
    std::array<float,4> result{};
    using GetArray=HRESULT(WINAPI*)(void*,const char*,float*,UINT);
    Hr(EffectMethod<GetArray,33>(host->Renderer()->effect)(host->Renderer()->effect,
        "MdsShadowMapSize",result.data(),4), "effect exposes actual sample/coarse dimensions");
    return result;
}
void CheckSize(UINT sample,UINT coarse) {
    const auto size=EffectSize();
    Check(size[0]==float(sample)&&size[1]==float(sample)&&
          size[2]==float(coarse)&&size[3]==float(coarse), "effect dimensions match actual textures and fallback mode");
}
bool EffectEnabled() {
    sf::ConfigureEffect(host,host->Renderer()->effect);
    BOOL value=FALSE;
    using GetBool=HRESULT(WINAPI*)(void*,const char*,BOOL*);
    Hr(EffectMethod<GetBool,25>(host->Renderer()->effect)(host->Renderer()->effect,
        "MdsShadowFilterEnhanced",&value), "read effective enhancement switch");
    return value!=FALSE;
}
void ClearFine(sf::FineDepthTarget* fine) {
    sf::TargetScope scope;
    Hr(scope.Begin(ownedDevice,*fine), "begin optional same-projection target scope");
    Hr(scope.Restore(), "restore optional target scope");
    sf::MarkRendered(host,true);
}
void ScopeTests(sf::FineDepthTarget* fine) {
    DeviceSnapshot original(ownedDevice);
    ComPtr<IDirect3DSurface9> color0,color1,depth;
    Hr(ownedDevice->CreateRenderTarget(128,96,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&color0,nullptr), "scope fixture RT0");
    Hr(ownedDevice->CreateDepthStencilSurface(128,96,D3DFMT_D24X8,D3DMULTISAMPLE_NONE,0,FALSE,&depth,nullptr), "scope fixture depth");
    Hr(ownedDevice->SetDepthStencilSurface(nullptr), "detach original depth");
    for(UINT i=1;i<original.count;++i)Hr(ownedDevice->SetRenderTarget(i,nullptr), "detach original MRT");
    Hr(ownedDevice->SetRenderTarget(0,color0.Get()), "install scope fixture RT0");
    if(original.count>1) {
        Hr(ownedDevice->CreateRenderTarget(128,96,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&color1,nullptr), "scope fixture RT1");
        Hr(ownedDevice->SetRenderTarget(1,color1.Get()), "install scope fixture RT1");
    }
    Hr(ownedDevice->SetDepthStencilSurface(depth.Get()), "install scope fixture depth");
    const D3DVIEWPORT9 view{7,5,80,60,.1f,.9f};
    Hr(ownedDevice->SetViewport(&view), "install non-default viewport");
    Hr(ownedDevice->SetTexture(0,host->Renderer()->hdrTexture), "install sampler fixture");
    Hr(ownedDevice->SetRenderState(D3DRS_ZENABLE,FALSE), "install depth state");
    Hr(ownedDevice->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE), "install cull state");
    Hr(ownedDevice->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR), "install sampler state");
    DeviceSnapshot expected(ownedDevice);
    {
        sf::TargetScope scope;
        Hr(scope.Begin(ownedDevice,*fine), "scope replaces custom RT/depth/viewport");
        ComPtr<IDirect3DSurface9> active;Hr(ownedDevice->GetRenderTarget(0,&active), "read active fine RT");
        Check(SameObject(active.Get(),fine->Surface()), "active RT is the fine texture surface");
        Hr(ownedDevice->SetRenderState(D3DRS_ZENABLE,TRUE), "mutate scoped depth state");
        Hr(ownedDevice->SetRenderState(D3DRS_CULLMODE,D3DCULL_CW), "mutate scoped cull state");
        Hr(ownedDevice->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT), "mutate scoped sampler state");
        Hr(scope.Restore(), "explicit TargetScope restore");
        Hr(scope.Restore(), "TargetScope restore is idempotent");
    }
    expected.Verify();
    { sf::TargetScope scope;Hr(scope.Begin(ownedDevice,*fine), "begin destructor restoration check"); }
    expected.Verify();
    sf::FineDepthTarget invalid;
    { sf::TargetScope scope;Check(FAILED(scope.Begin(ownedDevice,invalid)), "invalid scope target rejected"); }
    expected.Verify();
    expected.Restore();original.Restore();
}

void ApiTests() {
    auto* renderer=host->Renderer();const UINT cap=HardwareLimit(ownedDevice);
    Check(cap>=8192, "acceptance device supports all requested 512..8192 size cases");
    host->ShadowFilterEnhanced()=true;host->ShadowMode()=2;
    for(UINT coarse:{512u,1024u,2048u,4096u,8192u}) {
        CoarseFixture fixture(coarse);
        const UINT expected=FineSize(coarse,cap);
        const auto before=allocationAttempts;
        sf::Invalidate(host);
        Check(sf::VisibleTexture(host)==renderer->hdrTexture, "new frame starts on coarse texture");
        auto* fine=sf::Prepare(host,true);
        if(expected<=coarse) {
            Check(!fine&&allocationAttempts==before, "large coarse map avoids redundant pass/allocation");
            CheckSize(coarse,coarse);
        } else {
            Check(fine&&fine->Size()==expected, "coarse size selects real requested fine allocation");
            const auto desc=TextureDesc(fine->Texture());
            Check(desc.Width==expected&&desc.Height==expected&&desc.Format==D3DFMT_R32F, "allocated fine R32F has expected size");
            Check(allocationAttempts==before+1, "exactly one allocation for a new key");
            Check(sf::VisibleTexture(host)==renderer->hdrTexture, "unrendered fine texture stays invisible");
            auto died=Life(fine->Texture());
            if(coarse==512)ScopeTests(fine);
            ClearFine(fine);
            Check(sf::VisibleTexture(host)==fine->Texture(), "successful current fine render becomes visible");
            CheckSize(expected,coarse);
            Check(EffectEnabled(), "user-enabled filter remains enabled");
            sf::Invalidate(host);
            Check(sf::VisibleTexture(host)==renderer->hdrTexture, "invalidation never reuses last frame's fine image");
            CheckSize(coarse,0);
            fine=sf::Prepare(host,true);
            Check(fine&&allocationAttempts==before+1, "invalidation reuses allocation but not contents");
            sf::MarkRendered(host,false);
            Check(died->load(), "failed fine render releases its texture");
            Check(sf::VisibleTexture(host)==renderer->hdrTexture, "failed fine render falls back immediately");
            CheckSize(coarse,0);
            Check(EffectEnabled(), "optional failure retains user-enabled dynamic spline fallback");
            sf::Invalidate(host);
            Check(!sf::Prepare(host,true)&&allocationAttempts==before+1, "failed pass caches attempt for unchanged key");
            sf::ReleaseRenderer(renderer);
            fine=sf::Prepare(host,true);
            Check(fine&&allocationAttempts==before+2, "reset release clears failed-attempt cache");
            died=Life(fine->Texture());ClearFine(fine);
            host->ShadowFilterEnhanced()=false;sf::Invalidate(host);
            Check(died->load()&&sf::VisibleTexture(host)==renderer->hdrTexture, "toggle off releases fine resource and invalidates image");
            CheckSize(coarse,0);
            Check(!EffectEnabled(), "toggle off selects original shader branch");
            host->ShadowFilterEnhanced()=true;
            fine=sf::Prepare(host,true);Check(fine!=nullptr, "toggle on permits a fresh allocation");
            died=Life(fine->Texture());ClearFine(fine);
            sf::ReleaseRenderer(renderer,true);
            Check(died->load()&&sf::VisibleTexture(host)==renderer->hdrTexture, "shutdown-style release forgets state and destroys fine texture");
        }
        sf::Invalidate(host);
        const auto noNative=allocationAttempts;
        Check(!sf::Prepare(host,false)&&allocationAttempts==noNative, "failed native pass never allocates a fine map");
        Check(sf::VisibleTexture(host)==renderer->hdrTexture, "failed native pass cannot expose old fine contents");
        CheckSize(coarse,0);
        summary<<"api,"<<coarse<<','<<expected<<",passed\n";
    }
    {
        CoarseFixture fixture(512);
        const auto before=allocationAttempts,failures=injectedFailures;
        failOneAllocation=true;
        Check(!sf::Prepare(host,true), "one injected owned-device allocation failure falls back");
        Check(allocationAttempts==before+1&&injectedFailures==failures+1&&!failOneAllocation, "injection hit exactly one matching fine allocation");
        for(int i=0;i<3;++i){sf::Invalidate(host);Check(!sf::Prepare(host,true), "allocation failure remains cached across frames");}
        Check(allocationAttempts==before+1, "cached failure does not allocate every frame");CheckSize(512,0);
        // Change the actual coarse identity/dimensions without clearing module state.
        probeAllocations=false;
        ComPtr<IDirect3DTexture9> replacement;
        Hr(ownedDevice->CreateTexture(1024,1024,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&replacement,nullptr), "create different coarse key");
        renderer->hdrTexture=replacement.Get();probeAllocations=true;
        auto* fine=sf::Prepare(host,true);
        Check(fine&&allocationAttempts==before+2, "changed coarse key retries a previously failed allocation");
        auto died=Life(fine->Texture());ClearFine(fine);
        host->ShadowMode()=0;sf::Invalidate(host);
        Check(died->load()&&!sf::Prepare(host,true), "shadow mode zero releases and bypasses optional resources");
        Check(!EffectEnabled(), "shadow mode zero disables enhancement parameter");
        host->ShadowMode()=2;
        fine=sf::Prepare(host,true);Check(fine!=nullptr, "shadow mode restore retries");died=Life(fine->Texture());
        const int caps=renderer->shaderModelCaps;renderer->shaderModelCaps=2;sf::Invalidate(host);
        Check(died->load()&&!sf::Prepare(host,true), "SM2 capability bypasses optional resources");
        Check(!EffectEnabled(), "SM2 capability disables enhancement parameter");
        renderer->shaderModelCaps=caps;
        renderer->hdrTexture=fixture.texture.Get();
    }
    probeAllocations=false;failOneAllocation=false;
}

using Draw=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
Draw realDraw=nullptr;
bool observingFrame=false;
std::string frameTag;
struct Counts { unsigned shadowCoarse=0,shadowFine=0,visible=0,bad=0,fine=0,accessory=0; } counts;
struct ShaderInfo { unsigned id=0; bool nativeShadow=false; };
std::unordered_map<IDirect3DPixelShader9*,ShaderInfo> shaders;
unsigned shaderSerial=0;
ShaderInfo Classify(IDirect3DPixelShader9* shader) {
    if(!shader)return {};
    auto found=shaders.find(shader);if(found!=shaders.end())return found->second;
    ShaderInfo info{++shaderSerial,false};
    UINT bytes=0;
    if(SUCCEEDED(shader->GetFunction(nullptr,&bytes))&&bytes) {
        std::vector<DWORD> code((bytes+3)/4);
        if(SUCCEEDED(shader->GetFunction(code.data(),&bytes))) {
            using Disassemble=HRESULT(WINAPI*)(const DWORD*,BOOL,LPCSTR,void**);
            const auto disassemble=reinterpret_cast<Disassemble>(GetProcAddress(d3dx::Get().module,"D3DXDisassembleShader"));
            void* text=nullptr;
            if(disassemble&&SUCCEEDED(disassemble(code.data(),FALSE,nullptr,&text))) {
                using Data=const char*(WINAPI*)(void*);using Size=DWORD(WINAPI*)(void*);
                const std::string assembly(EffectMethod<Data,3>(text)(text),EffectMethod<Size,4>(text)(text));
                static_cast<IUnknown*>(text)->Release();
                info.nativeShadow=assembly.find("MdsShadowMapSize")!=std::string::npos;
                std::ofstream file(output/("shader-"+std::to_string(info.id)+".txt"));file<<assembly;
            }
        }
    }
    shaders.emplace(shader,info);return info;
}
HRESULT WINAPI ObserveDraw(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,
                           UINT min,UINT vertices,UINT start,UINT primitives) {
    if(!observingFrame)return realDraw(device,type,base,min,vertices,start,primitives);
    ComPtr<IDirect3DSurface9> target;D3DSURFACE_DESC rt{},sample{},coarse{},expected{};
    if(SUCCEEDED(device->GetRenderTarget(0,&target))&&target)target->GetDesc(&rt);
    ComPtr<IDirect3DBaseTexture9> texture;device->GetTexture(0,&texture);
    if(texture&&texture->GetType()==D3DRTYPE_TEXTURE)static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0,&sample);
    auto* renderer=host->Renderer();if(renderer->hdrTexture)renderer->hdrTexture->GetLevelDesc(0,&coarse);
    auto* visible=sf::VisibleTexture(host);if(visible)visible->GetLevelDesc(0,&expected);
    ComPtr<IDirect3DPixelShader9> shader;device->GetPixelShader(&shader);const auto info=Classify(shader.Get());
    const bool shadow=rt.Format==D3DFMT_R32F;
    // PMX materials without the receive-self-shadow flag deliberately bind
    // spriteTexture with the same shader. They are not shadow-map samples.
    const bool unshadowed = texture && SameObject(texture.Get(), renderer->spriteTexture);
    const bool nativeVisible=!shadow&&info.nativeShadow&&!unshadowed;
    const bool sharedVisible=!shadow&&!unshadowed&&sample.Format==D3DFMT_R32F;
    unsigned sharedTag=0x51444653,readTag=0;
    DWORD tagBytes=sizeof(readTag);
    const GUID sharedGuid={0x775d5621,0xd8a7,0x4d7f,{0x93,0x4f,0x6a,0x11,0x72,0x03,0x52,0x29}};
    bool sameShared=false;
    if(sharedVisible&&visible) {
        visible->SetPrivateData(sharedGuid,&sharedTag,sizeof sharedTag,0);
        sameShared=SUCCEEDED(texture->GetPrivateData(sharedGuid,&readTag,&tagBytes))&&readTag==sharedTag;
        visible->FreePrivateData(sharedGuid);
        if(!sameShared)++counts.bad;
    }
    const HRESULT result=realDraw(device,type,base,min,vertices,start,primitives);
    if(shadow) {
        if(rt.Width==coarse.Width)++counts.shadowCoarse;else ++counts.shadowFine;
    }
    if(nativeVisible) {
        ++counts.visible;
        if(host->ActiveRenderObject()==host->AccessorySlot(0))++counts.accessory;
        const UINT desired=host->ShadowFilterEnhanced()?FineSize(coarse.Width,deviceLimit):coarse.Width;
        const bool fine=sample.Width>coarse.Width;
        counts.fine+=fine;
        if(FAILED(result)||sample.Format!=D3DFMT_R32F||sample.Width!=expected.Width||
           sample.Height!=expected.Height||sample.Width!=desired)++counts.bad;
    }
    draws<<frameTag<<','<<(shadow?"shadow":nativeVisible?"visible-native":sharedVisible?"visible-external":"other")<<','<<info.id<<','
         <<rt.Width<<','<<rt.Height<<','<<unsigned(rt.Format)<<','<<sample.Width<<','<<sample.Height<<','
         <<unsigned(sample.Format)<<','<<coarse.Width<<','<<expected.Width<<','<<long(result)<<','
         <<texture.Get()<<','<<visible<<','<<sameShared<<'\n';
    return result;
}

void ConfigureFrame(bool enhanced,int mode) {
    MSG message{};
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    host->EditMode()=ViewportEditMode::Camera;
    host->ShadowFilterEnhanced()=enhanced;host->ShadowMode()=mode;
    host->state.selfShadowEnabled=1;host->ShadowDistance()=mode==1?.005f:.001f;
    host->AdaptiveShadowResolution()=false;host->ShadowResolutionLimit()=4096;
    host->NativePhysicsOverridden()=true;host->PlaybackPhysicsMode()=0;
    host->DeltaTime()=1.f/30;host->state.messageSeen=10;
}
void Frame(bool enhanced,int mode,const char* tag) {
    frameTag=tag;counts={};
    ConfigureFrame(enhanced,mode);
    observingFrame=true;FrameDriver(host);observingFrame=false;
    Check(counts.shadowCoarse>0, "real scene casters drew into coarse R32F");
    Check(counts.visible>0, "actual visible native shadow shader draws were observed");
    Check(counts.accessory>0, "receiving X accessory uses the same correctly sized shadow map");
    Check(counts.bad==0, "actual visible DrawIndexedPrimitive sampled the expected production texture");
    if(enhanced)Check(counts.shadowFine>0&&counts.fine>0, "same frame produced and visibly sampled fine depth");
    else Check(counts.shadowFine==0&&counts.fine==0, "disabled filter neither renders nor samples a sidecar");
    std::printf("FRAME %s shadow_coarse=%u shadow_fine=%u visible=%u fine_bindings=%u errors=%u\n",
        tag,counts.shadowCoarse,counts.shadowFine,counts.visible,counts.fine,counts.bad);
    summary<<"frame,"<<tag<<','<<counts.visible<<','<<counts.fine<<','<<counts.bad<<"\n";draws.flush();summary.flush();
    ComPtr<IDirect3DSurface9> surface;
    Hr(ownedDevice->GetRenderTarget(0,&surface), "read frame output target");
    Hr(d3dx::Get().saveSurfaceToFileW((output/(std::string(tag)+".png")).c_str(),3,surface.Get(),nullptr,nullptr), "save actual frame evidence");
    Check(!IsWindowVisible(host->MainWindow()), "owned host remained hidden");
}
} // namespace

using DrawUp=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
DrawUp realUp=nullptr;
bool failUp=false;
HRESULT WINAPI UpProbe(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT count,const void* data,UINT stride) {
    if(d==ownedDevice&&failUp){failUp=false;return E_FAIL;}
    return realUp(d,type,count,data,stride);
}
void SharedChecks() {
    auto* renderer=host->Renderer();
    host->ShadowFilterEnhanced()=true;host->ShadowMode()=2;
    sf::ReleaseRenderer(renderer);
    sf::Invalidate(host);
    auto* fine=sf::Prepare(host,true);Check(fine!=nullptr,"prepare private fine input for shared pass");
    ClearFine(fine);
    Hr(ownedDevice->BeginScene(),"warm shared resources before timing");
    sf::FinalizeDepth(host,true);
    Hr(ownedDevice->EndScene(),"complete shared warmup");
    DeviceSnapshot snapshot(ownedDevice);
    ComPtr<IDirect3DQuery9> start,end,frequency,disjoint;
    bool timing=SUCCEEDED(ownedDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&start))&&
        SUCCEEDED(ownedDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&end))&&
        SUCCEEDED(ownedDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,&frequency))&&
        SUCCEEDED(ownedDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,&disjoint));
    Hr(ownedDevice->BeginScene(),"begin isolated shared depth preprocessing");
    if(timing){disjoint->Issue(D3DISSUE_BEGIN);frequency->Issue(D3DISSUE_END);start->Issue(D3DISSUE_END);}
    sf::FinalizeDepth(host,true);
    if(timing){end->Issue(D3DISSUE_END);disjoint->Issue(D3DISSUE_END);}
    Hr(ownedDevice->EndScene(),"end isolated shared depth preprocessing");
    snapshot.Verify();
    auto* visible=sf::VisibleTexture(host);Check(visible!=renderer->hdrTexture && visible!=fine->Texture(),"published separate filtered R32F, not raw coarse or fine");
    Check(!EffectEnabled(),"native receiver kernel disabled only after shared output succeeds");
    if(timing) {
        UINT64 a=0,b=0,f=0;BOOL invalid=TRUE;const auto until=GetTickCount64()+10000;
        HRESULT ready=S_FALSE;
        while((ready=end->GetData(&b,sizeof b,D3DGETDATA_FLUSH))==S_FALSE&&GetTickCount64()<until)Sleep(1);
        if(ready==S_OK&&start->GetData(&a,sizeof a,0)==S_OK&&frequency->GetData(&f,sizeof f,0)==S_OK&&
           disjoint->GetData(&invalid,sizeof invalid,0)==S_OK&&!invalid&&f)
            std::printf("SHARED_GPU warmed_two_pass_ms=%.6f note=single_isolated_timestamp_not_FPS\n",1000.*double(b-a)/double(f));
        else std::puts("SHARED_GPU timing unavailable");
    }
    sf::Invalidate(host);Check(sf::VisibleTexture(host)==renderer->hdrTexture,"frame invalidation cannot reuse prior prefiltered texture");
    fine=sf::Prepare(host,true);Check(fine!=nullptr,"prepare next shared frame");ClearFine(fine);
    auto** vt=*reinterpret_cast<void***>(ownedDevice);
    Check(MH_CreateHook(vt[83],&UpProbe,reinterpret_cast<void**>(&realUp))==MH_OK&&MH_EnableHook(vt[83])==MH_OK,"hook only owned DrawPrimitiveUP failure");
    DeviceSnapshot failedSnapshot(ownedDevice);
    failUp=true;Hr(ownedDevice->BeginScene(),"begin injected shared draw failure");sf::FinalizeDepth(host,true);Hr(ownedDevice->EndScene(),"end injected failure");
    failedSnapshot.Verify();Check(!failUp,"injected draw failure consumed by shared pass");
    Check(sf::VisibleTexture(host)==renderer->hdrTexture,"failed shared draw publishes coarse fallback, not old filtered frame");
    Check(sf::Prepare(host,true)==nullptr,"shared failure cached for unchanged device and size");
    MH_DisableHook(vt[83]);MH_RemoveHook(vt[83]);
    sf::ReleaseRenderer(renderer);fine=sf::Prepare(host,true);Check(fine!=nullptr,"reset clears shared failure cache");ClearFine(fine);
    probeSize=fine->Size();probeAllocations=true;failOneAllocation=true;unsigned before=allocationAttempts;
    Hr(ownedDevice->BeginScene(),"begin injected shared allocation failure");sf::FinalizeDepth(host,true);Hr(ownedDevice->EndScene(),"end allocation failure");
    probeAllocations=false;
    Check(!failOneAllocation&&allocationAttempts==before+1,"first shared R32F allocation safely failed");
    Check(sf::VisibleTexture(host)==renderer->hdrTexture,"allocation failure returns current coarse depth");
    Check(sf::Prepare(host,true)==nullptr,"allocation failure cached without repeat allocation");
    host->ShadowFilterEnhanced()=false;sf::Invalidate(host);Check(sf::VisibleTexture(host)==renderer->hdrTexture,"toggle off releases shared resources and returns native map");
    sf::ReleaseRenderer(renderer);
}

void SharedSizeChecks() {
    host->ShadowFilterEnhanced()=true;host->ShadowMode()=2;
    for(UINT coarse:{512u,1024u,2048u,4096u,8192u}) {
        CoarseFixture fixture(coarse);
        sf::Invalidate(host);auto* fine=sf::Prepare(host,true);
        if(fine)ClearFine(fine);
        const unsigned before=allocationAttempts;
        Hr(ownedDevice->BeginScene(),"begin shared size sample");sf::FinalizeDepth(host,true);Hr(ownedDevice->EndScene(),"end shared size sample");
        auto* visible=sf::VisibleTexture(host);D3DSURFACE_DESC desc{};
        Check(visible!=host->Renderer()->hdrTexture&&SUCCEEDED(visible->GetLevelDesc(0,&desc))&&desc.Width==FineSize(coarse,deviceLimit),"shared filtered allocation uses current actual size, including capped coarse");
        Check(!EffectEnabled(),"shared output disables receiver kernel at current size");
        Check(allocationAttempts==before+2,"shared size creates exactly scratch and final maps");
        sf::Invalidate(host);Check(sf::VisibleTexture(host)==host->Renderer()->hdrTexture,"resized shared frame starts invalid");
        fine=sf::Prepare(host,true);if(fine)ClearFine(fine);
        Hr(ownedDevice->BeginScene(),"begin repeated shared size sample");sf::FinalizeDepth(host,true);Hr(ownedDevice->EndScene(),"end repeated shared size sample");
        Check(allocationAttempts==before+2,"unchanged shared size reuses allocations without per-frame recreate");
        summary<<"shared-size,"<<coarse<<','<<desc.Width<<",passed\n";
    }
    sf::ReleaseRenderer(host->Renderer());
}

int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc!=5||std::wcscmp(argv[3],L"--mme")!=0||
       (std::wcscmp(argv[4],L"0")!=0&&std::wcscmp(argv[4],L"1")!=0)) {
        std::fwprintf(stderr,L"Usage: MikuMikuDance.exe model-without-FX.pmx new-output-directory --mme 0|1\n");return 64;
    }
    HHOOK guard=nullptr;HMODULE systemD3d=nullptr;
    ComPtr<IDirect3D9> raw;ComPtr<IDirect3DDevice9> dummy;
    bool hooks=false,initializedHost=false;int exitCode=1;
    try {
        output=std::filesystem::absolute(argv[2]);
        Check(!std::filesystem::exists(output), "output directory is new; prior evidence will not be overwritten");
        std::filesystem::create_directories(output);
        draws.open(output/"draw-bindings.csv");summary.open(output/"summary.csv");
        draws<<"frame,pass,shader,rt_width,rt_height,rt_format,s0_width,s0_height,s0_format,coarse_width,expected_width,hresult,s0_pointer,visible_pointer,same_resource_tag\n";
        Check(bool(draws)&&bool(summary), "open evidence outputs");
        Check(MH_Initialize()==MH_OK, "initialize process-local hooks");hooks=true;
        Check(MH_CreateHook(&ShowWindow,&Hide,reinterpret_cast<void**>(&realShow))==MH_OK, "hook own window visibility");
        Check(MH_CreateHook(&MessageBoxA,&UnexpectedA,nullptr)==MH_OK&&MH_CreateHook(&MessageBoxW,&UnexpectedW,nullptr)==MH_OK, "reject initialization and runtime dialogs");
        Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK, "enable hidden guards");
        guard=SetWindowsHookExW(WH_CBT,Guard,nullptr,GetCurrentThreadId());Check(guard!=nullptr, "install current-thread hidden guard");
        host=new MMDApp;g_Block=host;host->state=MMDAppState{};host->InitDefaults();
        Check(InitMainWindowAndD3D(host,GetModuleHandleW(nullptr),SW_HIDE), "initialize real hidden production host");
        initializedHost=true;
        const bool mme=GetModuleHandleW(L"MMEffect.dll")!=nullptr;
        Check(mme==(argv[4][0]==L'1'), "actual MME presence matches explicit run mode");
        wchar_t modulePath[32768]{};
        if(mme)GetModuleFileNameW(GetModuleHandleW(L"MMEffect.dll"),modulePath,32768);
        std::printf("MME enabled=%d\n",mme);
        std::ofstream modules(output/"modules.txt");
        modules<<"MME enabled="<<mme<<"\n"<<std::filesystem::path(modulePath).u8string()<<"\n";
        Check(host->Renderer()->shaderModelCaps>=3, "production device exposes SM3");
        Check(std::filesystem::exists(argv[1]), "no-FX model fixture exists");
        wcscpy_s(host->EnvFileName(),256,argv[1]);LoadModelFile(host,argv[1],false);
        Check(host->ModelSlot(0)!=nullptr, "real model fixture loaded");
        // A receiving .x accessory shares the default effect with PMX draws.
        // It must use the same fine texture as the dimensions in that effect.
        const auto receiver=output/L"receiver.x";
        std::ofstream(receiver)<<R"(xof 0303txt 0032
Mesh Receiver {
4; -10;0;-10;, -10;0;10;, 10;0;10;, 10;0;-10;;
2; 3;0,1,2;, 3;0,2,3;;
MeshMaterialList { 1;2;0,0;; Material {0.6;0.6;0.6;1.0;;5.0;0.0;0.0;0.0;;0.1;0.1;0.1;;} }
}
)";
        LoadAccessoryFile(receiver.c_str());
        Check(host->AccessorySlot(0)!=nullptr, "receiving X accessory fixture loaded");
        host->AccessorySlot(0)->shadowEnabled=1;
        host->AccessoryKeys(0)[0].shadowEnabled=1;
        ownedDevice=host->Renderer()->device;
        deviceLimit=HardwareLimit(ownedDevice);
        auto** ownedTable=*reinterpret_cast<void***>(ownedDevice);
        createEntry=ownedTable[23];
        Check(MH_CreateHook(createEntry,&CreateTextureProbe,reinterpret_cast<void**>(&realCreateTexture))==MH_OK&&
              MH_EnableHook(createEntry)==MH_OK, "observe and selectively fail only owned-device fine allocations");

        wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
        systemD3d=LoadLibraryW((std::filesystem::path(system)/L"d3d9.dll").c_str());Check(systemD3d!=nullptr, "load actual system D3D9");
        auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(systemD3d,"Direct3DCreate9"));Check(create!=nullptr, "resolve actual D3D9 factory");
        raw.Attach(create(D3D_SDK_VERSION));Check(raw!=nullptr, "create system D3D9 interface");
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=host->MainWindow();
        pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        Hr(raw->CreateDevice(0,D3DDEVTYPE_HAL,host->MainWindow(),D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dummy), "obtain actual driver draw entry");
        auto** table=*reinterpret_cast<void***>(dummy.Get());
        drawEntry=table[82];
        Check(MH_CreateHook(drawEntry,&ObserveDraw,reinterpret_cast<void**>(&realDraw))==MH_OK&&MH_EnableHook(drawEntry)==MH_OK, "audit final driver DrawIndexedPrimitive bindings");
        ConfigureFrame(false,2);FrameDriver(host); // Finish deferred model/viewport initialization.
        Frame(false,2,"disabled-before");
        ApiTests();
        SharedChecks();
        SharedSizeChecks();
        Frame(true,1,"enhanced-mode1");
        Frame(true,2,"enhanced-mode2");
        shaders.clear(); // Shader COM identities may be reused after device reset.
        Check(PostDeviceReset(host), "production reset succeeds with no retained optional default-pool resources");
        Check(sf::VisibleTexture(host)==host->Renderer()->hdrTexture, "reset invalidates prior fine image");
        Frame(true,2,"enhanced-after-reset");
        Frame(false,2,"disabled-after");
        summary<<"checks,"<<checks<<",passed\n";summary.flush();exitCode=0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL shadow filter acceptance: %s\n",error.what());
    }
    observingFrame=probeAllocations=failOneAllocation=false;
    if(hooks&&createEntry)MH_DisableHook(createEntry);
    if(hooks&&drawEntry)MH_DisableHook(drawEntry);
    dummy.Reset();raw.Reset();
    if(host){if(initializedHost)ShutdownCleanup(host);delete host;host=nullptr;g_Block=nullptr;}
    ownedDevice=nullptr;
    if(guard)UnhookWindowsHookEx(guard);
    if(hooks){MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}
    if(systemD3d)FreeLibrary(systemD3d);
    std::printf("%s shadow_filter_tests checks=%u\n",exitCode?"FAIL":"PASS",checks);
    return exitCode;
}
