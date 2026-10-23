// MAINTENANCE: regression-keep. Real frame gate and downstream D3D9 submissions; no native-renderer stub.
#pragma once
// Include inside transparent_sort_settings_tests.cpp's anonymous namespace.
// Required FILE-SCOPE includes: d3d9.h, mikumikustudio/d3d12_native.hpp,
// mikumikustudio/frame_presentation.hpp, mikumikustudio/deformed_mesh_capture.hpp.
// The target must receive the same MMD_ENABLE_DX12 value as its linked core.
#if !MMD_ORIGINAL_PROFILE && defined(MMD_ENABLE_DX12) && MMD_ENABLE_DX12
namespace d3d12_frame_fallback {
constexpr char fallbackStatus[]="D3D9 fallback: transparent sorting policy";

void ViewportDiagnostic(MMDApp& app,const char* phase) {
    RECT client{};
    const BOOL clientRead=GetClientRect(app.MainWindow(),&client);
    const auto view=app.ViewportRect(),presentation=FramePresentationRect(&app);
    auto* renderer=app.Renderer();
    auto* device=renderer->device;
    IDirect3DSurface9* back=nullptr; IDirect3DSurface9* target=nullptr;
    D3DSURFACE_DESC backDesc{},targetDesc{};
    HRESULT backResult=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
    if (SUCCEEDED(backResult)) backResult=back?back->GetDesc(&backDesc):E_FAIL;
    HRESULT targetResult=device->GetRenderTarget(0,&target);
    if (SUCCEEDED(targetResult)) targetResult=target?target->GetDesc(&targetDesc):E_FAIL;
    if (back) back->Release();
    if (target) target->Release();
    D3DVIEWPORT9 actual{};
    const HRESULT viewportResult=device->GetViewport(&actual);
    std::printf("FRAME layout %s client-ok=%d client=[%ld,%ld,%ld,%ld] view=[%ld,%ld,%ld,%ld] presentation=[%ld,%ld,%ld,%ld]\n",
        phase,int(clientRead),client.left,client.top,client.right,client.bottom,
        view.left,view.top,view.right,view.bottom,presentation.left,presentation.top,presentation.right,presentation.bottom);
    std::printf("FRAME layout %s render=%dx%d renderer=%dx%d requested-backbuffer=%ux%u sidebar=%d ratio=%.9g ready=%u iconic=%d\n",
        phase,int(app.RenderWidth()),int(app.RenderHeight()),int(renderer->screenWidth),int(renderer->screenHeight),
        unsigned(renderer->presentParameters.BackBufferWidth),unsigned(renderer->presentParameters.BackBufferHeight),
        int(app.SidebarWidth()),double(app.SidebarRatio()),unsigned(app.WindowLayoutReady()),int(IsIconic(app.MainWindow())));
    std::printf("FRAME layout %s backbuffer-hr=%08lx actual-backbuffer=%ux%u target-hr=%08lx actual-target=%ux%u viewport-hr=%08lx actual-viewport=[%u,%u,%u,%u] renderable=%d\n",
        phase,static_cast<unsigned long>(backResult),unsigned(backDesc.Width),unsigned(backDesc.Height),
        static_cast<unsigned long>(targetResult),unsigned(targetDesc.Width),unsigned(targetDesc.Height),
        static_cast<unsigned long>(viewportResult),unsigned(actual.X),unsigned(actual.Y),unsigned(actual.Width),unsigned(actual.Height),
        int(FrameViewportRenderable(&app)));
    std::fflush(stdout);
}

void NoNativeChild(HWND owner) {
    bool found=false;
    EnumChildWindows(owner,[](HWND child,LPARAM context)->BOOL {
        wchar_t name[128]{};
        GetClassNameW(child,name,128);
        if (!wcscmp(name,L"MmdNativeD3D12Viewport")) *reinterpret_cast<bool*>(context)=true;
        return TRUE;
    },reinterpret_cast<LPARAM>(&found));
    Check(!found,"sorting fallback creates no native D3D12 viewport child, including a hidden child");
}

struct Restore {
    MMDApp& app;
    TransparentSortPrecision precision;
    bool cross;
    decltype(MMDAppState::messageSeen) message;
    decltype(MMDAppState::groundGridEnabled) grid;
    explicit Restore(MMDApp& value):app(value),precision(value.TransparentMaterialSortPrecision()),
        cross(Scope(value)),message(value.state.messageSeen),grid(value.GroundGridEnabled()) {}
    ~Restore() {
        EnableD3D12Native(false);
        app.TransparentMaterialSortPrecision()=precision;
        app.TransparentSortCrossObjects()=cross;
        app.state.messageSeen=message; app.GroundGridEnabled()=grid;
    }
};

// Observe() shares the production draw interception infrastructure. It does
// not activate an export Session, so deformed_mesh_capture::Active() remains
// false and RenderFrameScene still reaches its real native-preview gate.
// DrawObserver is before-DIP evidence; Begin/End below record native HRESULTs.
class FrameCalls final:public deformed_mesh_capture::DrawObserver {
    using SceneFn=HRESULT(WINAPI*)(IDirect3DDevice9*);
    inline static FrameCalls* active=nullptr;
    SceneFn begin=nullptr,end=nullptr;
    void* targets[2]{};
    DWORD thread=GetCurrentThreadId();
    bool observing=false;
public:
    MMDApp& app;
    IDirect3DDevice9* device;
    bool armed=false;
    unsigned begins=0,ends=0,groundSubmissions=0;
    int depth=0;
    const char* error=nullptr;
    explicit FrameCalls(MMDApp& value):app(value),device(value.Renderer()->device) {
        Check(!active && thread==hostThread,"one same-thread fallback frame observer");
        std::string reason;
        Check(deformed_mesh_capture::Observe(app,this,reason),
              reason.empty()?"install actual native D3D9 draw observer":reason.c_str());
        observing=true;
        Check(!deformed_mesh_capture::Active(),"draw observation does not activate the export-session bypass");
        auto table=*reinterpret_cast<void***>(device);
        targets[0]=table[41]; targets[1]=table[42];
        Check(MH_CreateHook(targets[0],reinterpret_cast<void*>(&Begin),reinterpret_cast<void**>(&begin))==MH_OK,
              "observe owned native D3D9 BeginScene");
        Check(MH_CreateHook(targets[1],reinterpret_cast<void*>(&End),reinterpret_cast<void**>(&end))==MH_OK,
              "observe owned native D3D9 EndScene");
        active=this;
        Check(MH_EnableHook(targets[0])==MH_OK && MH_EnableHook(targets[1])==MH_OK,
              "enable only explicit BeginScene and EndScene observation");
    }
    ~FrameCalls() {
        armed=false;
        for (int n=1;n>=0;--n)
            Check(MH_DisableHook(targets[n])==MH_OK && MH_RemoveHook(targets[n])==MH_OK,
                  "restore only owned D3D9 scene trampolines");
        active=nullptr;
        if (observing) deformed_mesh_capture::StopObserving(this);
    }
    bool Mine(IDirect3DDevice9* d) const noexcept { return armed && d==device && GetCurrentThreadId()==thread; }
    void Reset() { begins=ends=groundSubmissions=0; depth=0; error=nullptr; }
    static HRESULT WINAPI Begin(IDirect3DDevice9* d) {
        auto& self=*active;
        const bool mine=self.Mine(d);
        if (mine && std::strcmp(D3D12NativeStatus(),fallbackStatus))
            self.error="D3D9 BeginScene occurred before the actual sorting fallback gate";
        const auto result=self.begin(d);
        if (mine) {
            if (FAILED(result)) self.error="native D3D9 BeginScene failed";
            else { ++self.begins; if (self.depth++) self.error="nested owned D3D9 BeginScene"; }
        }
        return result;
    }
    static HRESULT WINAPI End(IDirect3DDevice9* d) {
        auto& self=*active;
        const bool mine=self.Mine(d);
        const auto result=self.end(d);
        if (mine) {
            if (FAILED(result)) self.error="native D3D9 EndScene failed";
            else { ++self.ends; if (--self.depth!=0) self.error="unbalanced owned D3D9 EndScene"; }
        }
        return result;
    }
    void Indexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT first,UINT count,UINT start,UINT primitives,
                 const void*,unsigned) noexcept override {
        if (!Mine(d) || type!=D3DPT_LINELIST) return;
        IDirect3DVertexBuffer9* vb=nullptr; IDirect3DIndexBuffer9* ib=nullptr;
        UINT offset=0,stride=0;
        const auto stream=d->GetStreamSource(0,&vb,&offset,&stride),indices=d->GetIndices(&ib);
        if (SUCCEEDED(stream) && SUCCEEDED(indices) && vb==app.GroundGridVertices() && ib==app.GroundGridIndices()) {
            if (depth!=1 || base!=0 || first!=0 || count!=90 || start!=0 || primitives!=45 || offset!=0 || stride!=16)
                error="real production ground draw has unexpected scene state or geometry arguments";
            else ++groundSubmissions;
        }
        if (vb) vb->Release(); if (ib) ib->Release();
    }
    void Unsupported(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT) noexcept override {}
};
} // namespace d3d12_frame_fallback
#endif

void D3D12FrameFallbackChecks(MMDApp& app) {
#if !MMD_ORIGINAL_PROFILE && defined(MMD_ENABLE_DX12) && MMD_ENABLE_DX12
    using namespace d3d12_frame_fallback;
    Check(Owned(app.MainWindow()) && app.Renderer() && app.Renderer()->device,
          "fallback integration requires the real hidden production host and D3D9 device");
    Check(!app.RecordingWindow() && !app.FloatingWindow() && !app.FullscreenMode(),
          "fallback fixture is the ordinary main-window frame path");
    ViewportDiagnostic(app,"before-normal-layout");
    // SW_HIDE startup does not deliver the visible application's first size
    // layout. Use the production layout entry, as export_viewport_tests does:
    // it calculates the sidebar, publishes the real device viewport, and lays
    // out its panels. Never seed a guessed viewport or borrow an export one.
    HandleWindowSize(&app);
    ViewportDiagnostic(app,"after-normal-layout");
    Check(FrameViewportRenderable(&app),"real editor viewport admits the native-preview gate without output borrowing");
    Check(app.GroundGridVertices() && app.GroundGridIndices(),"production startup created the actual ground geometry");
    Check(!deformed_mesh_capture::Active(),"fallback begins outside any export capture session");
    HiddenWindows(); NoNativeChild(app.MainWindow());
    Restore restore(app);
    app.GroundGridEnabled()=1;
    FrameCalls calls(app);
    for (auto precision:{TransparentSortPrecision::Low,TransparentSortPrecision::Standard,
                         TransparentSortPrecision::High,TransparentSortPrecision::Maximum}) {
        for (bool cross:{false,true}) {
            app.TransparentMaterialSortPrecision()=precision;
            app.TransparentSortCrossObjects()=cross;
            const auto revision=app.SceneRevision(),generation=app.SceneGeneration();
            const auto frame=app.CurrentFrame();
            const auto dirty=app.state.sceneModified;
            const auto scopeEdited=app.PmmProjectSettings().sortingCrossObjectsEdited;
            const auto precisionEdited=app.PmmProjectSettings().sortingPrecisionEdited;
            calls.Reset();
            EnableD3D12Native(true);
            Check(!std::strcmp(D3D12NativeStatus(),"requested"),"compiled native preview records a fresh request before each real frame");
            app.state.messageSeen=1;
            calls.armed=true;
            RenderFrameScene(&app);
            calls.armed=false;
            Check(!std::strcmp(D3D12NativeStatus(),fallbackStatus),"actual RenderFrameScene reached the native sorting gate");
            Check(!calls.error,calls.error?calls.error:"owned downstream D3D9 calls succeeded");
            Check(calls.begins>0 && calls.ends==calls.begins && calls.depth==0 && calls.groundSubmissions>0,
                  "after native rejection the real frame submits ground geometry inside completed D3D9 scenes");
            Check(app.CurrentFrame()==frame && app.SceneRevision()==revision && app.SceneGeneration()==generation && app.state.sceneModified==dirty,
                  "fallback rendering preserves playhead, revision, generation, and dirty state");
            Check(Scope(app)==cross && app.TransparentMaterialSortPrecision()==precision &&
                  app.PmmProjectSettings().sortingCrossObjectsEdited==scopeEdited &&
                  app.PmmProjectSettings().sortingPrecisionEdited==precisionEdited,
                  "fallback rendering preserves live sorting choices and edited markers");
            NoNativeChild(app.MainWindow()); HiddenWindows();
            std::printf("D3D12 fallback frame precision=%u cross=%d begin=%u end=%u ground-before-DIP=%u\n",
                unsigned(precision),cross,calls.begins,calls.ends,calls.groundSubmissions);
        }
    }
#else
    (void)app;
    std::puts("SKIP D3D12 real-frame fallback: original profile or linked core built without MMD_ENABLE_DX12; no native-gate claim.");
#endif
}
