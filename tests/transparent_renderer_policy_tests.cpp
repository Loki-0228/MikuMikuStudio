// Real-device policy regression. This executable owns its hidden window,
// models and device; it never loads MME or presents to a user's window.
// Full profile only: the original profile does not contain DrawObserver.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/d3d_object_guard.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/transparent_peeling.hpp"

#if MMD_ORIGINAL_PROFILE
#error This real draw-observer test must only be built in the full profile.
#endif

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void Hr(HRESULT value, const char* message) { Check(SUCCEEDED(value), message); }
template<class T> void Release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}
struct Vertex { float x,y,z,nx,ny,nz,u,v; };
static_assert(sizeof(Vertex) == 32);
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::ModelMaterialRecord> materials;
    std::vector<mdl::MaterialMorphPool> add, mul;
    std::vector<WORD> pmd;
    std::vector<INT> pmx;
    std::vector<Vertex> vertices;
    std::vector<std::array<UINT,3>> faces;
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    ~Model() { Release(vb); Release(ib); }
};

// Only CreateIndexBuffer is detoured at native code level (the runtime can
// replace device draw vtables). All data capture is restricted to our device.
// Each returned IB gets its own 14-slot vtable copy, never a global IB hook.
// The shadow is copied BEFORE the real Unlock: no READONLY lock of WRITEONLY
// storage and no synthetic triangle list substituted for uploaded bytes.
struct Uploads {
    enum class Failure { None, Create, Lock, Unlock };
    struct Buffer {
        IDirect3DIndexBuffer9* object = nullptr;
        void** original = nullptr;
        std::array<unsigned char,4096> bytes{};
        UINT size = 0, offset = 0, length = 0;
        void* locked = nullptr;
        DWORD lockFlags = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        bool scratch = false, uploaded = false;
    };
    using CreateFn = HRESULT (WINAPI*)(IDirect3DDevice9*,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DIndexBuffer9**,HANDLE*);
    using LockFn = HRESULT (WINAPI*)(IDirect3DIndexBuffer9*,UINT,UINT,void**,DWORD);
    using UnlockFn = HRESULT (WINAPI*)(IDirect3DIndexBuffer9*);
    using ReleaseFn = ULONG (WINAPI*)(IDirect3DIndexBuffer9*);
    inline static Uploads* active = nullptr;
    // Production accepts COM vtables in mapped images. Keep the private
    // copies in this test EXE's image data, not in Uploads' stack storage.
    // No native/shared vtable is overwritten; one live IB owns each slot.
    inline static std::array<std::array<void*,14>,64> privateTables{};
    IDirect3DDevice9* device;
    void* target = nullptr;
    CreateFn originalCreate = nullptr;
    std::array<Buffer,64> buffers{};
    Failure failure = Failure::None;
    unsigned failureHits = 0, draws = 0, drawsAtFailure = 0, created = 0, freed = 0;
    const char* error = nullptr;

    explicit Uploads(IDirect3DDevice9* owned) : device(owned) {
        Check(!active, "one upload tracker per process");
        MEMORY_BASIC_INFORMATION tableRegion{};
        Check(VirtualQuery(privateTables.data(),&tableRegion,sizeof(tableRegion))!=0 &&
              tableRegion.State==MEM_COMMIT && tableRegion.Type==MEM_IMAGE,
              "private IB vtables reside in the test EXE image");
        std::printf("private-IB-tables=%p region-type=%08lx\n",static_cast<void*>(privateTables.data()),tableRegion.Type);
        const auto status = MH_Initialize();
        Check(status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED, "initialize native hook library");
        target = (*reinterpret_cast<void***>(device))[27];
        Check(MH_CreateHook(target, reinterpret_cast<void*>(&Create),
                           reinterpret_cast<void**>(&originalCreate)) == MH_OK, "hook owned-device IB creation");
        active = this;
        if (MH_EnableHook(target) != MH_OK) {
            active = nullptr; MH_RemoveHook(target); target = nullptr;
            Check(false, "enable owned-device IB creation hook");
        }
    }
    ~Uploads() {
        failure = Failure::None;
        // A failed assertion may leave buffers referenced by device state.
        // Restore only live objects; a zero-return Release clears its entry.
        for (auto& b : buffers) if (b.object)
            *reinterpret_cast<void***>(b.object) = b.original;
        if (target) { MH_DisableHook(target); MH_RemoveHook(target); }
        active = nullptr;
    }
    Buffer* Find(IDirect3DIndexBuffer9* ib) noexcept {
        if (!ib) return nullptr;
        for (auto& b : buffers) if (b.object == ib) return &b;
        return nullptr;
    }
    bool Fail(Failure requested) noexcept {
        if (failure != requested) return false;
        failure = Failure::None; ++failureHits; drawsAtFailure = draws; return true;
    }
    static HRESULT WINAPI Create(IDirect3DDevice9* d, UINT size, DWORD usage,
        D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared) {
        auto& self = *active;
        const bool owned = d == self.device;
        // Exclude legacy DYNAMIC local-material buffers from the fault target.
        const bool scratch = usage == D3DUSAGE_WRITEONLY && pool == D3DPOOL_DEFAULT && format == D3DFMT_INDEX32;
        if (owned && scratch && self.Fail(Failure::Create)) { if (out) *out = nullptr; return E_OUTOFMEMORY; }
        const HRESULT hr = self.originalCreate(d,size,usage,format,pool,out,shared);
        if (!owned || FAILED(hr) || !out || !*out) return hr;
        Buffer* entry = nullptr;
        for (auto& b : self.buffers) if (!b.object) { entry = &b; break; }
        if (!entry || size > entry->bytes.size()) {
            self.error = "IB capture capacity exceeded"; (*out)->Release(); *out = nullptr; return E_OUTOFMEMORY;
        }
        *entry = Buffer{};
        entry->object = *out; entry->size = size; entry->format = format; entry->scratch = scratch;
        entry->original = *reinterpret_cast<void***>(*out);
        auto& table = privateTables[static_cast<std::size_t>(entry-self.buffers.data())];
        std::copy_n(entry->original,table.size(),table.begin());
        table[2] = reinterpret_cast<void*>(&Drop);
        table[11] = reinterpret_cast<void*>(&Lock);
        table[12] = reinterpret_cast<void*>(&Unlock);
        *reinterpret_cast<void***>(*out) = table.data();
        if (!PlausibleD3dObject(*out)) {
            self.error = "captured IB rejected by the production resource guard";
            *reinterpret_cast<void***>(*out) = entry->original;
            entry->object = nullptr;
            (*out)->Release(); *out = nullptr;
            return E_FAIL;
        }
        ++self.created;
        return hr;
    }
    static ULONG WINAPI Drop(IDirect3DIndexBuffer9* ib) {
        auto& self = *active; auto* b = self.Find(ib);
        if (!b) std::terminate(); // Only an owned live IB points at this thunk.
        const auto call = reinterpret_cast<ReleaseFn>(b->original[2]);
        const ULONG remaining = call(ib);
        // ib is invalid here when zero; never restore or query its vtable.
        if (!remaining) { b->object = nullptr; b->locked = nullptr; ++self.freed; }
        return remaining;
    }
    static HRESULT WINAPI Lock(IDirect3DIndexBuffer9* ib, UINT offset, UINT length, void** data, DWORD flags) {
        auto& self = *active; auto* b = self.Find(ib); if (!b) std::terminate();
        if (b->scratch && self.Fail(Failure::Lock)) { if (data) *data = nullptr; return E_FAIL; }
        const HRESULT hr = reinterpret_cast<LockFn>(b->original[11])(ib,offset,length,data,flags);
        if (SUCCEEDED(hr) && data) {
            b->locked = *data; b->offset = offset; b->length = length ? length : b->size-offset; b->lockFlags = flags;
            if (offset > b->size || b->length > b->size-offset) self.error = "invalid native IB lock extent";
        }
        return hr;
    }
    static HRESULT WINAPI Unlock(IDirect3DIndexBuffer9* ib) {
        auto& self = *active; auto* b = self.Find(ib); if (!b) std::terminate();
        if (b->locked && !(b->lockFlags & D3DLOCK_READONLY) && b->offset <= b->size && b->length <= b->size-b->offset) {
            std::memcpy(b->bytes.data()+b->offset,b->locked,b->length); b->uploaded = true;
        }
        const HRESULT hr = reinterpret_cast<UnlockFn>(b->original[12])(ib); b->locked = nullptr;
        // Complete the actual unlock even when reporting a synthetic failure.
        if (b->scratch && self.Fail(Failure::Unlock)) return E_FAIL;
        return hr;
    }
    void Reset(Failure next = Failure::None) {
        Check(!error, error ? error : "upload tracker healthy");
        failure = next; failureHits = draws = drawsAtFailure = 0;
    }
};

struct Draws : deformed_mesh_capture::DrawObserver {
    struct Face { const void* object; unsigned material; std::array<UINT,3> indices; IDirect3DVertexBuffer9* vb; IDirect3DIndexBuffer9* ib; UINT stride; bool scratch; };
    MMDApp& app; Uploads& uploads;
    std::array<Face,256> faces{}; unsigned count = 0;
    const char* error = nullptr; bool observing = false, expectedEffect = false;
    Draws(MMDApp& a, Uploads& u) : app(a), uploads(u) {
        std::string reason;
        Check(deformed_mesh_capture::Observe(app,this,reason), reason.empty() ? "install actual DIP observer" : reason.c_str());
        observing = true;
    }
    ~Draws() { if (observing) deformed_mesh_capture::StopObserving(this); }
    void Reset(bool effect) { count = 0; error = nullptr; expectedEffect = effect; }
    void Indexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT minimum,
                 UINT vertexCount, UINT start, UINT primitives, const void* object, unsigned material) noexcept override {
        if (d != uploads.device || !object) return;
        ++uploads.draws;
        if (type != D3DPT_TRIANGLELIST || primitives > (faces.size()-count)) { error = "unexpected material topology/count"; return; }
        IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr;
        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DPixelShader9* pixelShader = nullptr;
        UINT offset = 0, stride = 0;
        const HRESULT vh = d->GetStreamSource(0,&vb,&offset,&stride), ih = d->GetIndices(&ib);
        const HRESULT vs = d->GetVertexShader(&vertexShader), ps = d->GetPixelShader(&pixelShader);
        auto* shadow = uploads.Find(ib);
        if (FAILED(vh) || FAILED(ih) || !vb || !ib || !shadow || !shadow->uploaded || offset != 0 || base != 0) error = "draw has no captured uploaded source input";
        else if (app.ActiveRenderObject() != object ||
                 app.ActiveRenderPass() != (expectedEffect ? AccessoryRenderPass::Effect : AccessoryRenderPass::FixedFunction)) error = "draw does not use this frame's requested material pass";
        else if (FAILED(vs) || FAILED(ps)) error = "cannot query shaders at actual material draw";
        else if (expectedEffect ? (!vertexShader || !pixelShader) : (vertexShader || pixelShader)) error = "actual bound shaders do not match requested Effect/Fixed renderer";
        else if (mdl::Mdl(static_cast<const unsigned char*>(object))->toonShared != material) error = "draw lost actual material identity";
        else {
            const UINT width = shadow->format == D3DFMT_INDEX32 ? 4 : 2;
            if (start > shadow->size/width || primitives*3 > shadow->size/width-start) error = "draw reads beyond uploaded indices";
            else for (UINT f=0; f<primitives; ++f) {
                Face face{object,material,{},vb,ib,stride,shadow->scratch};
                for (UINT k=0;k<3;++k) {
                    UINT index=0;
                    std::memcpy(&index,shadow->bytes.data()+(start+f*3+k)*width,width);
                    if (index < minimum || index-minimum >= vertexCount) error = "DIP range excludes a submitted source index";
                    face.indices[k]=index;
                }
                faces[count++]=face;
            }
        }
        Release(vertexShader); Release(pixelShader); Release(vb); Release(ib);
    }
    void Unsupported(IDirect3DDevice9* d,D3DPRIMITIVETYPE,UINT) noexcept override {
        if (d==uploads.device) error="unexpected unindexed material submission";
    }
};

struct Host {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    MMDApp* previous = g_Block;
    HWND window=nullptr; HMODULE runtimeModule=nullptr;
    IDirect3D9* runtime=nullptr; IDirect3DDevice9* device=nullptr;
    IDirect3DTexture9* white=nullptr;
    IDirect3DSurface9* target=nullptr; IDirect3DSurface9* readback=nullptr;
    D3DMATRIX identity{};
    DWORD lastPixel=0;
    Host() { try { Initialize(); } catch (...) { Cleanup(); throw; } }
    void Initialize() {
        wchar_t desktop[128]{}; DWORD needed=0;
        Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&needed) &&
              _wcsicmp(desktop,L"Default")==0,"real renderer requires the Default desktop");
        Check(!GetModuleHandleW(L"MMEffect.dll"),"this native fixture must not have MME loaded");
        struct DiagnosticVariable { const char* narrow; const wchar_t* wide; };
        const DiagnosticVariable diagnostics[] = {
            {"MIKUMIKUSTUDIO_TRACE_FILE",L"MIKUMIKUSTUDIO_TRACE_FILE"},
            {"MIKUMIKUSTUDIO_ALPHA_GEOMETRY_DUMP",L"MIKUMIKUSTUDIO_ALPHA_GEOMETRY_DUMP"},
            {"MIKUMIKUSTUDIO_VB_DUMP_DIR",L"MIKUMIKUSTUDIO_VB_DUMP_DIR"},
            {"MIKUMIKUSTUDIO_PEEL_DUMP",L"MIKUMIKUSTUDIO_PEEL_DUMP"}
        };
        for (const auto& name : diagnostics) {
            Check(_putenv_s(name.narrow,"")==0,"clear inherited CRT diagnostic variable");
            SetLastError(ERROR_SUCCESS);
            const BOOL cleared=SetEnvironmentVariableW(name.wide,nullptr);
            const DWORD clearError=GetLastError();
            Check(cleared || clearError==ERROR_ENVVAR_NOT_FOUND,"clear inherited Win32 diagnostic variable");
            const char* remaining=std::getenv(name.narrow);
            Check(!remaining || !*remaining,"CRT diagnostic output is disabled");
            wchar_t remainingWide[2]{}; SetLastError(ERROR_SUCCESS);
            const DWORD remainingSize=GetEnvironmentVariableW(name.wide,remainingWide,2);
            Check(remainingSize==0 && GetLastError()==ERROR_ENVVAR_NOT_FOUND,"Win32 diagnostic output is disabled");
        }
        new (&app->state) MMDAppState{};
        window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"MMS native transparent policy test",WS_POPUP,0,0,96,96,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(window!=nullptr,"create owned hidden window");
        wchar_t system[MAX_PATH]{}; Check(GetSystemDirectoryW(system,MAX_PATH)!=0,"locate system D3D9");
        runtimeModule=LoadLibraryW((std::wstring(system)+L"\\d3d9.dll").c_str());
        Check(runtimeModule!=nullptr,"load system D3D9");
        const auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtimeModule,"Direct3DCreate9"));
        runtime=create?create(D3D_SDK_VERSION):nullptr; Check(runtime!=nullptr,"create real D3D9 factory");
        D3DADAPTER_IDENTIFIER9 adapter{}; Hr(runtime->GetAdapterIdentifier(0,0,&adapter),"identify real adapter");
        std::printf("adapter=%s vendor=%04lx device=%04lx driver=%s version=%08lx:%08lx\n",adapter.Description,adapter.VendorId,adapter.DeviceId,adapter.Driver,adapter.DriverVersion.HighPart,adapter.DriverVersion.LowPart);
        auto& pp=renderer->presentParameters; pp={}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow=window; pp.BackBufferWidth=pp.BackBufferHeight=96; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D16; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        Hr(runtime->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device),"create HAL software-vertex device");
        D3DDEVICE_CREATION_PARAMETERS actual{}; Hr(device->GetCreationParameters(&actual),"read actual device flags");
        Check(actual.DeviceType==D3DDEVTYPE_HAL && (actual.BehaviorFlags&D3DCREATE_SOFTWARE_VERTEXPROCESSING),"no silent device fallback");
        std::printf("profile=full flags=%08lx window=owned-hidden present=never\n",actual.BehaviorFlags);
        renderer->device=device; renderer->d3d9=runtime; renderer->hwnd=window; renderer->aspectRatio=1;
        app->Renderer()=renderer.get(); app->Hwnd()=window; app->HInstance()=GetModuleHandleW(nullptr);
        app->PlaybackActive()=1; app->ModelNonDisplayMode()=0; g_Block=app.get();
        Check(d3dx::Get().Load(),"load D3DX runtime");
        identity._11=identity._22=identity._33=identity._44=1;
        app->LightDirection()[2]=-1; app->SceneLight().Ambient={1,1,1,1}; app->CameraDistance()=-2;
        // Fixed rendering constructs its toon transform from SceneLight,
        // whereas the effect path reads LightDirection. Initialize both, as
        // the existing model_alpha_occlusion fixture does.
        app->SceneLight().Direction={0,0,-1};
        std::memcpy(&app->ViewRotationTransform(),&identity,sizeof(identity));
        std::memcpy(&app->LightViewProjection(),&identity,sizeof(identity));
        Hr(device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&white,nullptr),"create white toon");
        D3DLOCKED_RECT lock{}; Hr(white->LockRect(0,&lock,nullptr,0),"lock white toon");
        *static_cast<DWORD*>(lock.pBits)=0xffffffff; Hr(white->UnlockRect(0),"unlock white toon"); app->ToonTexture(0)=white;
        Hr(device->GetRenderTarget(0,&target),"retain actual target");
        Hr(device->CreateOffscreenPlainSurface(96,96,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr),"create real readback");
    }
    void Unbind() {
        device->SetStreamSource(0,nullptr,0,0); device->SetIndices(nullptr);
        for (DWORD i=0;i<8;++i) device->SetTexture(i,nullptr);
        app->ModelSlot(0)=app->ModelSlot(1)=nullptr;
    }
    void Effect(int resource) {
        if (renderer->effect) { reinterpret_cast<IUnknown*>(renderer->effect)->Release(); renderer->effect=nullptr; }
        renderer->hdrTexture=nullptr;
        if (resource) {
            void* errors=nullptr;
            const HRESULT result=d3dx::Get().createEffectFromResA(device,nullptr,MAKEINTRESOURCEA(resource),nullptr,nullptr,0,nullptr,reinterpret_cast<void**>(&renderer->effect),&errors);
            if(errors) reinterpret_cast<IUnknown*>(errors)->Release();
            Hr(result,"load actual embedded model effect resource"); Check(renderer->effect!=nullptr,"embedded effect exists");
            renderer->shaderModel3=resource==118; renderer->hdrTexture=white;
        }
    }
    void Frame(Draws& draw, Uploads& uploads, int resource) {
        draw.Reset(resource != 0);
        device->SetTransform(D3DTS_WORLD,&identity); device->SetTransform(D3DTS_VIEW,&identity); device->SetTransform(D3DTS_PROJECTION,&identity);
        device->SetRenderState(D3DRS_LIGHTING,TRUE); device->SetRenderState(D3DRS_AMBIENT,0xffffffff);
        device->SetRenderState(D3DRS_SPECULARENABLE,FALSE); device->SetRenderState(D3DRS_ZENABLE,TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE); device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW); device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE); device->SetRenderState(D3DRS_ALPHAREF,1);
        device->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATEREQUAL);
        for (DWORD stage=0;stage<2;++stage) {
            device->SetTextureStageState(stage,D3DTSS_COLOROP,D3DTOP_MODULATE);
            device->SetTextureStageState(stage,D3DTSS_COLORARG1,D3DTA_TEXTURE);
            device->SetTextureStageState(stage,D3DTSS_COLORARG2,stage?D3DTA_CURRENT:D3DTA_DIFFUSE);
            device->SetTextureStageState(stage,D3DTSS_ALPHAOP,D3DTOP_MODULATE);
            device->SetTextureStageState(stage,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
            device->SetTextureStageState(stage,D3DTSS_ALPHAARG2,stage?D3DTA_CURRENT:D3DTA_DIFFUSE);
            device->SetTextureStageState(stage,D3DTSS_TEXCOORDINDEX,0);
            device->SetTextureStageState(stage,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
        }
        Hr(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff000000,1,0),"clear owned frame");
        D3DVIEWPORT9 beforeViewport{}; Hr(device->GetViewport(&beforeViewport),"capture caller viewport");
        D3DSURFACE_DESC targetDescription{}; Hr(target->GetDesc(&targetDescription),"query actual render target dimensions");
        constexpr UINT sampleX=48,sampleY=48;
        std::printf("target=%ux%u format=%u viewport=%lu,%lu,%lu,%lu depth=%g..%g sample=%u,%u\n",
            targetDescription.Width,targetDescription.Height,unsigned(targetDescription.Format),
            beforeViewport.X,beforeViewport.Y,beforeViewport.Width,beforeViewport.Height,
            beforeViewport.MinZ,beforeViewport.MaxZ,sampleX,sampleY);
        Check(targetDescription.Width==96 && targetDescription.Height==96 && targetDescription.Format==D3DFMT_A8R8G8B8,"actual target matches fixture size and pixel format");
        Check(beforeViewport.X==0 && beforeViewport.Y==0 && beforeViewport.Width==96 && beforeViewport.Height==96 &&
              beforeViewport.MinZ==0.f && beforeViewport.MaxZ==1.f,"actual viewport matches fixture projection and samples");
        Check(sampleX<targetDescription.Width && sampleY<targetDescription.Height,"readback sample lies inside actual target");
        IDirect3DSurface9* beforeDepth=nullptr; Hr(device->GetDepthStencilSurface(&beforeDepth),"capture caller depth target");
        // Borrow only the pointer identity; the device retains this surface.
        IDirect3DSurface9* expectedDepth=beforeDepth; Release(beforeDepth);
        Hr(device->BeginScene(),"begin owned frame");
        if(resource) RenderModelsEffect(app.get(),reinterpret_cast<const float*>(&identity)); else RenderModelsFixed(app.get());
        Hr(device->EndScene(),"end owned frame");
        std::printf("before-DIP callbacks=%u captured-triangles=%u fault-hits=%u observer-error=%s\n",
            uploads.draws,draw.count,uploads.failureHits,draw.error?draw.error:"none");
        Check(!draw.error,draw.error?draw.error:"draw observer healthy");
        Check(!uploads.error,uploads.error?uploads.error:"actual upload observer healthy");
        Check(app->ActiveRenderObject()==nullptr && app->ActiveRenderPass()==AccessoryRenderPass::None,"normal frame object/pass tail restored");
        IDirect3DSurface9* after=nullptr; Hr(device->GetRenderTarget(0,&after),"query target after frame");
        const bool same=after==target; Release(after); Check(same,"actual target preserved");
        Hr(device->GetDepthStencilSurface(&after),"query depth target after frame");
        const bool sameDepth=after==expectedDepth; Release(after); Check(sameDepth,"actual depth target preserved");
        D3DVIEWPORT9 afterViewport{}; Hr(device->GetViewport(&afterViewport),"query viewport after frame");
        Check(std::memcmp(&beforeViewport,&afterViewport,sizeof(beforeViewport))==0,"actual viewport preserved");
        for(const auto& b:uploads.buffers) Check(!b.locked,"no IB lock survives frame preparation or fallback");
        Hr(device->GetRenderTargetData(target,readback),"read actual rendered pixels");
        D3DLOCKED_RECT pixels{}; Hr(readback->LockRect(&pixels,nullptr,D3DLOCK_READONLY),"lock rendered readback");
        const DWORD center=*reinterpret_cast<DWORD*>(static_cast<unsigned char*>(pixels.pBits)+sampleY*pixels.Pitch+sampleX*4);
        lastPixel=center;
        Hr(readback->UnlockRect(),"unlock rendered readback");
        std::printf("pixel=%08lx before-DIP-callbacks=%u triangles=%u fault-hits=%u\n",center,uploads.draws,draw.count,uploads.failureHits);
        if((center&0xffffff)==0) for(UINT i=0;i<draw.count;++i) {
            const auto& face=draw.faces[i];
            std::printf("before-DIP-face=%u object=%p material=%u vb=%p stride=%u ib=%p scratch=%d indices=%u,%u,%u\n",
                i,face.object,face.material,static_cast<void*>(face.vb),face.stride,static_cast<void*>(face.ib),face.scratch,
                face.indices[0],face.indices[1],face.indices[2]);
        }
        Check((center&0xffffff)!=0,"real triangles produced non-background pixels");
    }
    void Cleanup() noexcept {
        if(device) Unbind();
        if(renderer->effect) { reinterpret_cast<IUnknown*>(renderer->effect)->Release(); renderer->effect=nullptr; }
        renderer->hdrTexture=nullptr; app->ToonTexture(0)=nullptr;
        ClearTransparentPeelingCache(); Release(readback); Release(target); Release(white);
        renderer->device=nullptr; renderer->d3d9=nullptr; app->Renderer()=nullptr;
        Release(device); Release(runtime); if(runtimeModule) FreeLibrary(runtimeModule);
        if(window) DestroyWindow(window); g_Block=previous;
    }
    ~Host() { Cleanup(); }
};

std::unique_ptr<Model> MakeModel(Host& host, bool pmx, UINT base, const std::vector<float>& depths) {
    auto m=std::make_unique<Model>();
    m->vertices.resize(base+12*depths.size()); m->materials.resize(depths.size());
    for(UINT f=0;f<depths.size();++f) {
        const UINT at=base+12*f;
        const std::array<UINT,3> face={at+9,at+2,at+7}; m->faces.push_back(face);
        // Clockwise in target coordinates; all faces overlap at the center.
        m->vertices[face[0]]={-.8f,-.8f,depths[f],0,0,-1,0,0};
        m->vertices[face[1]]={0,.8f,depths[f],0,0,-1,0,0};
        m->vertices[face[2]]={.8f,-.8f,depths[f],0,0,-1,0,0};
        auto& material=m->materials[f];
        std::fill_n(material.diffuse,4,1.f); std::fill_n(material.diffuseMirror,4,1.f);
        std::fill_n(material.ambient,3,1.f); material.diffuse[3]=.5f; material.faceVertexCount=3; material.toonReference=0xff;
        if(pmx) material.flags=1;
        for(UINT i:face) {
            if(pmx) m->pmx.push_back(static_cast<INT>(i));
            else { Check(i<=65535,"PMD index fits 16 bits"); m->pmd.push_back(static_cast<WORD>(i)); m->pmd.push_back(0x5a5a); }
        }
    }
    auto& record=*m->record; record.vertexCount=static_cast<UINT>(m->vertices.size()); record.indexCount=static_cast<UINT>(depths.size()*3);
    record.materialCount=static_cast<UINT>(m->materials.size()); record.materials=m->materials.data();
    record.indices=pmx?static_cast<void*>(m->pmx.data()):static_cast<void*>(m->pmd.data());
    record.loadComplete=1; record.toonFlag=1; record.physicsMode=pmx?2:0; record.edgeScale=0;
    m->add.resize(depths.size()+1); m->mul.resize(depths.size()+1);
    for(auto& pool:m->mul) {
        std::fill_n(pool.channels.diffuse,4,1.f); std::fill_n(pool.channels.textureTint,4,1.f);
        std::fill_n(pool.channels.sphereTint,4,1.f); std::fill_n(pool.channels.toonTint,4,1.f);
    }
    if(pmx) { mdl::MaterialMorphAdd(reinterpret_cast<unsigned char*>(&record))=m->add.data(); mdl::MaterialMorphMul(reinterpret_cast<unsigned char*>(&record))=m->mul.data(); }
    const UINT size=static_cast<UINT>(m->vertices.size()*sizeof(Vertex));
    Hr(host.device->CreateVertexBuffer(size,0,274,D3DPOOL_MANAGED,&m->vb,nullptr),"create real model VB");
    void* data=nullptr; Hr(m->vb->Lock(0,0,&data,0),"lock real model VB"); std::memcpy(data,m->vertices.data(),size); Hr(m->vb->Unlock(),"upload real model VB");
    Hr(host.device->CreateIndexBuffer(record.indexCount*(pmx?4:2),0,pmx?D3DFMT_INDEX32:D3DFMT_INDEX16,D3DPOOL_MANAGED,&m->ib,nullptr),"create real source IB");
    Hr(m->ib->Lock(0,0,&data,0),"lock real source IB");
    UINT index=0; for(const auto& face:m->faces) for(UINT i:face) { if(pmx) static_cast<UINT*>(data)[index++]=i; else static_cast<WORD*>(data)[index++]=static_cast<WORD>(i); }
    Hr(m->ib->Unlock(),"upload real source IB"); record.vertexBuffer=m->vb; record.indexBuffer=m->ib;
    return m;
}

using Expected = std::pair<Model*,unsigned>;
void CheckFaces(const Draws& draws, const std::vector<Expected>& expected, bool sorted) {
    Check(draws.count==expected.size(),"exactly one actual submitted triangle per expected face");
    for(UINT i=0;i<draws.count;++i) {
        const auto& actual=draws.faces[i]; const auto& e=expected[i];
        Check(actual.object==e.first->record.get(),"actual draw preserves authored object scope/order");
        Check(actual.material==e.second,"actual draw preserves expected material identity/order");
        Check(actual.vb==e.first->vb && actual.stride==sizeof(Vertex),"unsplit draw uses original VB and stride");
        Check(actual.indices==e.first->faces[e.second],"actual uploaded IB keeps nonsequential original indices");
        Check(sorted ? actual.scratch && actual.ib!=e.first->ib : actual.ib==e.first->ib,"policy uses private IB; Off uses restored source IB");
    }
}
void SourceUnchanged(Model& model, bool pmx) {
    void* bytes=nullptr; Hr(model.vb->Lock(0,0,&bytes,D3DLOCK_READONLY),"read source VB after frame");
    const bool same=std::memcmp(bytes,model.vertices.data(),model.vertices.size()*sizeof(Vertex))==0;
    Hr(model.vb->Unlock(),"unlock source VB after frame"); Check(same,"source VB bytes unchanged");
    UINT n=0; for(const auto& face:model.faces) for(UINT i:face) {
        if(pmx) Check(model.pmx[n++]==static_cast<INT>(i),"PMX author indices unchanged");
        else { Check(model.pmd[2*n]==i && model.pmd[2*n+1]==0x5a5a,"PMD author indices and companion words unchanged"); ++n; }
    }
}
void Install(Host& host, Model& a, Model& b) {
    a.record->comboSelIndex=0; b.record->comboSelIndex=1;
    host.app->ModelSlot(0)=reinterpret_cast<unsigned char*>(b.record.get());
    host.app->ModelSlot(1)=reinterpret_cast<unsigned char*>(a.record.get());
}
void ScopeCases(Host& host, Uploads& uploads, Draws& draws, bool pmx, int resource) {
    auto a=MakeModel(host,pmx,pmx?65536:0,{.2f,.8f});
    auto b=MakeModel(host,pmx,pmx?65552:16,{.4f,.6f}); Install(host,*a,*b);
    const std::vector<Expected> authored={{a.get(),0},{a.get(),1},{b.get(),0},{b.get(),1}};
    const std::vector<Expected> local={{a.get(),1},{a.get(),0},{b.get(),1},{b.get(),0}};
    const std::vector<Expected> global={{a.get(),1},{b.get(),1},{b.get(),0},{a.get(),0}};
    for(auto precision:{TransparentSortPrecision::Low,TransparentSortPrecision::Standard,TransparentSortPrecision::High,TransparentSortPrecision::Off}) {
        host.app->TransparentMaterialSortPrecision()=precision;
        // Repeated policy switch with identical geometry catches cache keys.
        for(bool cross:{false,true,false,true}) {
            host.app->TransparentSortCrossObjects()=cross; uploads.Reset();
            std::printf("scope pmx=%d resource=%d precision=%u cross=%d\n",pmx,resource,unsigned(precision),cross);
            host.Frame(draws,uploads,resource);
            CheckFaces(draws,precision==TransparentSortPrecision::Off?authored:(cross?global:local),precision!=TransparentSortPrecision::Off);
            if(!resource) {
                // White, half-alpha materials over black, ZWRITE enabled:
                // authored: only A-near survives; local: A-far then A-near;
                // global: all four survive. This checks rasterized blending,
                // independently of the draw observer and CPU sorter.
                const unsigned layers=precision==TransparentSortPrecision::Off?1:(cross?4:2);
                const int expected=static_cast<int>(std::lround(255.0*(1.0-std::pow(.5,static_cast<int>(layers)))));
                for(unsigned shift:{0u,8u,16u}) Check(std::abs(int((host.lastPixel>>shift)&255)-expected)<=2,"fixed pixel follows independent source-over and depth oracle");
            }
            SourceUnchanged(*a,pmx); SourceUnchanged(*b,pmx);
        }
    }
    host.app->TransparentMaterialSortPrecision()=TransparentSortPrecision::High;
    for(bool cross:{false,true}) {
      host.app->TransparentSortCrossObjects()=cross;
      for(auto fault:{Uploads::Failure::Create,Uploads::Failure::Lock,Uploads::Failure::Unlock}) {
        uploads.Reset(fault); std::printf("prepare-failure pmx=%d resource=%d cross=%d fault=%u\n",pmx,resource,cross,unsigned(fault));
        host.Frame(draws,uploads,resource);
        Check(uploads.failureHits==1,"fault reached private policy IB preparation");
        Check(uploads.drawsAtFailure==0,"no material submitted before failing preparation");
        Check(draws.count==4,"fallback submits each face once, without a partial prefix");
        CheckFaces(draws,authored,false);
        std::array<bool,4> seen{};
        for(UINT i=0;i<draws.count;++i) {
            const auto& face=draws.faces[i]; Model* model=face.object==a->record.get()?a.get():(face.object==b->record.get()?b.get():nullptr);
            Check(model && face.material<2,"fallback retains known source material");
            const unsigned id=(model==a.get()?0:2)+face.material; Check(!seen[id],"fallback has no duplicated face"); seen[id]=true;
            Check(face.vb==model->vb && face.indices==model->faces[face.material],"fallback preserves original VB and indices");
        }
        SourceUnchanged(*a,pmx); SourceUnchanged(*b,pmx);
        uploads.Reset(); host.Frame(draws,uploads,resource); CheckFaces(draws,cross?global:local,true);
      }
    }
    host.Unbind();
}
void BucketCases(Host& host, Uploads& uploads, Draws& draws, bool pmx, int resource) {
    // Visible .1/.9 anchors fix the range. .50000 and .50001 share even a
    // 256-way bucket; exact-depth sort reverses them and is observably wrong.
    auto a=MakeModel(host,pmx,0,{.50000f,.50001f,.1f,.9f});
    auto b=MakeModel(host,pmx,64,{.50002f,.50003f,.1f,.9f}); Install(host,*a,*b);
    host.app->TransparentMaterialSortPrecision()=TransparentSortPrecision::High;
    for(bool cross:{false,true,false}) {
        host.app->TransparentSortCrossObjects()=cross; uploads.Reset();
        std::printf("high-stable-bucket pmx=%d resource=%d cross=%d\n",pmx,resource,cross);
        host.Frame(draws,uploads,resource);
        const std::vector<Expected> local={{a.get(),3},{a.get(),0},{a.get(),1},{a.get(),2},{b.get(),3},{b.get(),0},{b.get(),1},{b.get(),2}};
        const std::vector<Expected> global={{a.get(),3},{b.get(),3},{a.get(),0},{a.get(),1},{b.get(),0},{b.get(),1},{a.get(),2},{b.get(),2}};
        CheckFaces(draws,cross?global:local,true); SourceUnchanged(*a,pmx); SourceUnchanged(*b,pmx);
    }
    host.Unbind();
}
void TierCases(Host& host, Uploads& uploads, Draws& draws, bool pmx, int resource) {
    // Bounds 0..1: .51/.53/.515 share Low bin 8; Standard separates
    // .53 (33) from .51/.515 (32); High separates all three (135/131/130).
    // Explicit expected permutations, not a second copy of bucketOf.
    auto a=MakeModel(host,pmx,0,{.51f,.53f,.515f,0.f,1.f});
    auto b=MakeModel(host,pmx,96,{.51f,.53f,.515f,0.f,1.f}); Install(host,*a,*b);
    host.app->TransparentSortCrossObjects()=false;
    for(auto precision:{TransparentSortPrecision::Low,TransparentSortPrecision::Standard,TransparentSortPrecision::High}) {
        host.app->TransparentMaterialSortPrecision()=precision; uploads.Reset();
        std::printf("tier-buckets pmx=%d resource=%d precision=%u\n",pmx,resource,unsigned(precision));
        host.Frame(draws,uploads,resource);
        std::array<unsigned,5> order{};
        if(precision==TransparentSortPrecision::Low) order={4,0,1,2,3};
        else if(precision==TransparentSortPrecision::Standard) order={4,1,0,2,3};
        else order={4,1,2,0,3};
        std::vector<Expected> expected;
        for(Model* model:{a.get(),b.get()}) for(unsigned index:order) expected.emplace_back(model,index);
        CheckFaces(draws,expected,true); SourceUnchanged(*a,pmx); SourceUnchanged(*b,pmx);
    }
    host.Unbind();
}

#include "transparent_renderer_maximum_checks.hpp"
#include "transparent_renderer_visibility_checks.hpp"
#include "transparent_renderer_uv_checks.hpp"
}

int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        bool fixedOnly=false, maximumOnly=false, visibilityOnly=false, uvOnly=false;
        if(argc==2 && std::strcmp(argv[1],"--fixed-only")==0) fixedOnly=true;
        else if(argc==2 && std::strcmp(argv[1],"--maximum-only")==0) maximumOnly=true;
        else if(argc==2 && std::strcmp(argv[1],"--visibility-only")==0) visibilityOnly=true;
        else if(argc==2 && std::strcmp(argv[1],"--uv-only")==0) uvOnly=true;
        else Check(argc==1,"usage: transparent_renderer_policy_tests [--fixed-only|--maximum-only|--visibility-only|--uv-only]");
        Host host;
        {
            Uploads uploads(host.device);
            {
                Draws draws(*host.app,uploads);
                for(int resource:{0,117,118}) {
                    if(fixedOnly && resource) continue;
                    host.Effect(resource);
                    for(bool pmx:{false,true}) {
                        if(!maximumOnly && !visibilityOnly && !uvOnly) {
                            ScopeCases(host,uploads,draws,pmx,resource);
                            BucketCases(host,uploads,draws,pmx,resource);
                            TierCases(host,uploads,draws,pmx,resource);
                        }
                        if(!visibilityOnly && !uvOnly) MaximumRendererChecks(host,uploads,draws,pmx,resource);
                        if(!maximumOnly && !uvOnly) VisibilityRendererChecks(host,uploads,draws,pmx,resource);
                    }
                    if(!maximumOnly && !visibilityOnly) UvRendererChecks(host,uploads,draws,resource);
                }
                host.Unbind();
            } // Remove native draw observers before releasing upload hooks.
            Check(!uploads.error,uploads.error?uploads.error:"upload capture final health");
            for(const auto& b:uploads.buffers) Check(!b.object && !b.locked,"all owned IB references and locks released");
            Check(uploads.created==uploads.freed,"all captured IB creations reach zero references");
            std::printf("cleanup buffers-created=%u buffers-freed=%u\n",uploads.created,uploads.freed);
        }
        std::printf("PASS transparent_renderer_policy_tests %u checks\n",checks);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL transparent_renderer_policy_tests: %s (%u checks)\n",e.what(),checks); return 1; }
}
