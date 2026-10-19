// Actual hidden-window and D3D9 coverage. No replacement layout math is used.
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/scene_sampling.hpp"
#include <array>
#include <memory>
#include <stdexcept>

namespace small_viewport_host_checks {
inline void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct DeviceProbe {
    using SetViewportFn=HRESULT (WINAPI*)(IDirect3DDevice9*,const D3DVIEWPORT9*);
    using BeginSceneFn=HRESULT (WINAPI*)(IDirect3DDevice9*);
    using PresentFn=HRESULT (WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
    inline static DeviceProbe* active=nullptr;
    IDirect3DDevice9* device;
    SetViewportFn set=nullptr;BeginSceneFn begin=nullptr;PresentFn present=nullptr;
    void* targets[3]{};
    unsigned installed=0,viewports=0,invalid=0,begins=0,presents=0,forced=0;
    bool failNext=false;
    explicit DeviceProbe(IDirect3DDevice9* value):device(value){
        Require(!active&&device,"one real-device viewport observer");active=this;
        auto** table=*reinterpret_cast<void***>(device);
        targets[0]=table[47];targets[1]=table[41];targets[2]=table[17];
        try{
            Install(targets[0],reinterpret_cast<void*>(Set),reinterpret_cast<void**>(&set));
            Install(targets[1],reinterpret_cast<void*>(Begin),reinterpret_cast<void**>(&begin));
            Install(targets[2],reinterpret_cast<void*>(Present),reinterpret_cast<void**>(&present));
        }catch(...){Remove();active=nullptr;throw;}
    }
    void Install(void* target,void* detour,void** original){
        Require(MH_CreateHook(target,detour,original)==MH_OK,"install real D3D method observation");
        ++installed;Require(MH_EnableHook(target)==MH_OK,"enable real D3D method observation");
    }
    void Remove() noexcept {while(installed){auto* target=targets[--installed];MH_DisableHook(target);MH_RemoveHook(target);}}
    ~DeviceProbe(){Remove();active=nullptr;}
    static HRESULT WINAPI Set(IDirect3DDevice9* device,const D3DVIEWPORT9* viewport){
        auto& self=*active;
        if(device==self.device){
            ++self.viewports;IDirect3DSurface9* target=nullptr;D3DSURFACE_DESC desc{};
            const bool surface=SUCCEEDED(device->GetRenderTarget(0,&target))&&target;
            const bool described=surface&&SUCCEEDED(target->GetDesc(&desc));if(target)target->Release();
            const bool valid=viewport&&described&&viewport->Width&&viewport->Height&&
                viewport->X<=desc.Width&&viewport->Y<=desc.Height&&
                viewport->Width<=desc.Width-viewport->X&&viewport->Height<=desc.Height-viewport->Y;
            self.invalid+=!valid;
            if(self.failNext){self.failNext=false;++self.forced;return D3DERR_INVALIDCALL;}
        }
        return self.set(device,viewport);
    }
    static HRESULT WINAPI Begin(IDirect3DDevice9* device){
        auto& self=*active;if(device==self.device)++self.begins;return self.begin(device);
    }
    static HRESULT WINAPI Present(IDirect3DDevice9* device,const RECT* source,const RECT* destination,HWND window,const RGNDATA* dirty){
        auto& self=*active;if(device==self.device)++self.presents;return self.present(device,source,destination,window,dirty);
    }
};
struct SceneInvariant {
    MMDApp& app;
    mdl::CameraKey* camera;mdl::LightKey* light;
    std::uint64_t cameraId,lightId;
    std::vector<mdl::CameraKey> cameraBytes;std::vector<mdl::LightKey> lightBytes;
    int frame;std::array<float,7> parameters;
    plugins::SharedHistory* history;std::uint64_t revision=0,retained=0;std::size_t undo=0,redo=0;
    explicit SceneInvariant(MMDApp& value):app(value),camera(value.CameraKeys()),light(value.LightKeys()),
        cameraId(mdl::KeyPoolIdentity(camera)),lightId(mdl::KeyPoolIdentity(light)),
        cameraBytes(camera,camera+mdl::KeyPoolCapacity(camera)),lightBytes(light,light+mdl::KeyPoolCapacity(light)),
        frame(value.CurrentFrame()),parameters(Values(value)),history(plugins::NativeHistory(&value)){
        if(history){Require(history->Revision(revision)==MMS_OK,"capture real viewport-test history revision");
            retained=history->RetainedBytes();undo=history->UndoCount();redo=history->RedoCount();}
    }
    static std::array<float,7> Values(MMDApp& a){return {a.CameraFov(),a.LightColor()[0],a.LightColor()[1],a.LightColor()[2],
        a.LightDirection()[0],a.LightDirection()[1],a.LightDirection()[2]};}
    void Check(){
        const auto after=Values(app);
        Require(app.CurrentFrame()==frame&&!std::memcmp(after.data(),parameters.data(),sizeof(after)),
            "viewport layout/render guards preserve authored parameters and playhead");
        Require(app.CameraKeys()==camera&&app.LightKeys()==light&&mdl::KeyPoolIdentity(camera)==cameraId&&mdl::KeyPoolIdentity(light)==lightId&&
            mdl::KeyPoolCapacity(camera)==cameraBytes.size()&&mdl::KeyPoolCapacity(light)==lightBytes.size()&&
            !std::memcmp(camera,cameraBytes.data(),cameraBytes.size()*sizeof(mdl::CameraKey))&&
            !std::memcmp(light,lightBytes.data(),lightBytes.size()*sizeof(mdl::LightKey)),
            "viewport layout/render guards preserve VMD key allocations identities capacities and every byte");
        Require(plugins::NativeHistory(&app)==history,"viewport layout preserves actual journal identity");
        if(history){std::uint64_t current=0;Require(history->Revision(current)==MMS_OK&&current==revision&&history->RetainedBytes()==retained&&
            history->UndoCount()==undo&&history->RedoCount()==redo,"viewport layout/render guards add no history or retained payload");}
    }
};
inline void ClientSize(MMDApp& app,int width,int height){
    const HWND window=app.MainWindow();const WindowDpiScope dpi(window);
    RECT extent{0,0,width,height};
    Require(AdjustWindowRectEx(&extent,DWORD(GetWindowLongPtrW(window,GWL_STYLE)),GetMenu(window)!=nullptr,
        DWORD(GetWindowLongPtrW(window,GWL_EXSTYLE)))!=FALSE,"calculate real hidden host client size");
    int outerWidth=extent.right-extent.left,outerHeight=extent.bottom-extent.top;
    RECT actual{};
    // AdjustWindowRectEx assumes a single menu row. Narrow windows can wrap
    // the real application menu; correct only the measured client difference.
    for(unsigned attempt=0;attempt<5;++attempt){
        Require(SetWindowPos(window,nullptr,0,0,outerWidth,outerHeight,
            SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"resize actual host without showing or activating it");
        Require(GetClientRect(window,&actual)!=FALSE,"read the actual client after native menu wrapping");
        if(actual.right==width&&actual.bottom==height)break;
        outerWidth+=width-actual.right;outerHeight+=height-actual.bottom;
    }
    Require(actual.right==width&&actual.bottom==height,
        "actual client size matches the viewport boundary fixture");
    app.SidebarWidth()=250;HandleWindowSize(&app);RefreshMainWindowViewport(&app);
}
inline void Valid(MMDApp& app){
    Require(FrameViewportRenderable(&app),"restored real client has a renderable scene viewport");
    const auto view=app.ViewportRect(),present=FramePresentationRect(&app);
    RECT client{};GetClientRect(app.MainWindow(),&client);
    Require(view.left>=0&&view.top>=0&&view.left<view.right&&view.top<view.bottom&&view.right<=client.right&&view.bottom<=client.bottom&&
        present.left<=view.left&&present.top<=view.top&&present.right>=view.right&&present.bottom>=view.bottom,
        "real scene and presentation rectangles are positive nested client regions");
    for(int id:{447,455,456,457,458,459,460}){
        RECT bounds{},intersection{};auto control=GetDlgItem(app.MainWindow(),id);
        Require(control&&GetWindowRect(control,&bounds),"observe actual camera and light trackbar geometry");
        MapWindowPoints(nullptr,app.MainWindow(),reinterpret_cast<POINT*>(&bounds),2);
        Require(!IntersectRect(&intersection,&present,&bounds),"real present destination excludes every native parameter slider");
    }
    D3DVIEWPORT9 viewport{};Require(SUCCEEDED(app.Renderer()->device->GetViewport(&viewport))&&viewport.X==DWORD(view.left)&&
        viewport.Y==DWORD(view.top)&&viewport.Width==DWORD(view.right-view.left)&&viewport.Height==DWORD(view.bottom-view.top),
        "published scene rectangle equals the viewport accepted by the actual D3D9 device");
}
inline void Empty(MMDApp& app,DeviceProbe& probe,const D3DVIEWPORT9& previous){
    const auto present=FramePresentationRect(&app);
    Require(!FrameViewportRenderable(&app)&&IsRectEmpty(&app.ViewportRect())&&IsRectEmpty(&present),
        "unusable client has no logical rendering or presentation rectangle");
    D3DVIEWPORT9 viewport{};Require(SUCCEEDED(app.Renderer()->device->GetViewport(&viewport))&&!std::memcmp(&viewport,&previous,sizeof(viewport)),
        "unusable layout never submits a zero viewport or replaces the last valid device viewport");
    const auto begins=probe.begins,presents=probe.presents;
    app.MessageSeen()=1;app.frameRate.ready=true;RenderFrameScene(&app);BakePreview(&app).Present();
    Require(probe.begins==begins&&probe.presents==presents&&!app.frameRate.ready,
        "invalid editor viewport starts no native scene and presents no stale completed frame");
}
struct SamplingState {
    MMDApp& app;
    std::unique_ptr<MMDAppState> saved;
    double cursor,start,end;
    explicit SamplingState(MMDApp& a):app(a),saved(new MMDAppState(a.state)),
        cursor(a.PlaybackCursorSeconds()),start(a.PlaybackStartSeconds()),end(a.PlaybackEndSeconds()){}
    void Check() const {
        const auto& s=*saved;
        Require(app.CurrentFrame()==s.currentFrame&&app.CameraMode()==s.optflag[0]&&
            app.PlaybackActive()==s.playbackActive&&app.PlaybackCursorSeconds()==cursor&&
            app.PlaybackStartSeconds()==start&&app.PlaybackEndSeconds()==end,
            "actual Visit restores original editor mode playhead and playback clock on every exit");
        const std::array<float,14> before{s.cameraPosX,s.cameraPosY,s.cameraPosZ,s.cameraPitch,s.cameraYaw,s.cameraRoll,
            s.cameraDistance,s.cameraFov,s.lightColor[0],s.lightColor[1],s.lightColor[2],
            s.lightDirection[0],s.lightDirection[1],s.lightDirection[2]};
        const std::array<float,14> after{app.CameraPositionX(),app.CameraPositionY(),app.CameraPositionZ(),
            app.CameraPitch(),app.CameraYaw(),app.CameraRoll(),app.CameraDistance(),app.CameraFov(),
            app.LightColor()[0],app.LightColor()[1],app.LightColor()[2],
            app.LightDirection()[0],app.LightDirection()[1],app.LightDirection()[2]};
        Require(!std::memcmp(before.data(),after.data(),sizeof(before))&&
            app.state.renderW==s.renderW&&app.state.renderH==s.renderH&&
            app.state.cameraParentModel==s.cameraParentModel&&app.state.cameraParentBone==s.cameraParentBone,
            "actual Visit restores unregistered camera/light pose parents and output dimensions exactly");
    }
};
struct EditorGeometry {
    RECT view{};int width=0,separate=0;float ratio=0;unsigned char ready=0;
    void Capture(MMDApp& app){view=app.ViewportRect();width=app.SidebarWidth();separate=app.SeparateWindowSidebarWidth();
        ratio=app.SidebarRatio();ready=app.WindowLayoutReady();}
    void Check(MMDApp& app) const {
        Require(EqualRect(&view,&app.ViewportRect())&&app.SidebarWidth()==width&&app.SeparateWindowSidebarWidth()==separate&&
            !std::memcmp(&ratio,&app.SidebarRatio(),sizeof(ratio))&&app.WindowLayoutReady()==ready,
            "Visit keeps actual editor refresh geometry and exact sidebar publication through restoration");
    }
};
inline void VisitPublicationChecks(MMDApp& app,DeviceProbe& probe,SceneInvariant& invariant){
    enum Operation {Grow,Shrink,SameRectangleRetry,DeviceRefusal,NoDevice,Sidebar};
    unsigned cases=0;
    for(const auto operation:{Grow,Shrink,SameRectangleRetry,DeviceRefusal,NoDevice,Sidebar})for(unsigned exit=0;exit<3;++exit){
        ClientSize(app,1200,820);Valid(app);
        if(operation==Grow)ClientSize(app,760,160);
        if(operation==SameRectangleRetry){probe.failNext=true;RefreshMainWindowViewport(&app);
            Require(!probe.failNext&&IsRectEmpty(&app.ViewportRect()),"prepare an actual empty editor with retained device geometry for Visit retry");}
        SamplingState original(app);EditorGeometry geometry;bool reached=false;unsigned finishedSets=0;
        D3DVIEWPORT9 deviceBefore{};Require(SUCCEEDED(app.Renderer()->device->GetViewport(&deviceBefore)),"capture the real device viewport before Visit callback");
        const auto begins=probe.begins,presents=probe.presents,sets=probe.viewports;
        std::string error;
        const bool complete=scene_sampling::Visit(app,0,0,30,[&](double,bool output){
            Require(output,"actual Visit publication callback is an output sample");reached=true;
            if(operation==Grow)ClientSize(app,1200,820);
            else if(operation==Shrink)ClientSize(app,760,160);
            else if(operation==SameRectangleRetry){
                FrameSceneOutputScope borrowed(&app);Require(borrowed.Ready(),"same-rectangle retry starts with actual borrowed device viewport");
                const auto temporary=app.ViewportRect();RefreshMainWindowViewport(&app);
                Require(EqualRect(&temporary,&app.ViewportRect()),"real successful retry is byte-identical to borrowed geometry (ABA)");
            }else if(operation==DeviceRefusal){probe.failNext=true;RefreshMainWindowViewport(&app);
                Require(!probe.failNext&&IsRectEmpty(&app.ViewportRect())&&probe.viewports==sets+1,
                    "real SetViewport refusal explicitly invalidates logical editor viewport inside Visit");}
            else if(operation==NoDevice){
                auto* renderer=app.Renderer();auto* device=renderer->device;renderer->device=nullptr;
                RefreshMainWindowViewport(&app);renderer->device=device;
                Require(IsRectEmpty(&app.ViewportRect())&&probe.viewports==sets,"editor refresh with unavailable device publishes empty geometry inside Visit");
            }else{
                // Enter through the production sidebar press/move/release path,
                // without showing the host, taking capture or synthesizing UI.
                app.MouseX()=app.SidebarWidth()+2;app.MouseY()=100;
                HandleLButtonDown(&app);
                Require(app.sidebarSizing.active&&app.SidebarResizeDragging(),"real sidebar press enters its native resize operation");
                app.MouseX()=420;UpdateSidebarResize(&app);CommitSidebarResize(&app);
                Require(!app.sidebarSizing.active&&!app.SidebarResizeDragging()&&app.SidebarWidth()==420,
                    "native sidebar gesture publishes new exact width and ratio inside Visit");
            }
            geometry.Capture(app);
            // The real rendering consumers establish their output scope after
            // their progress callback pumped resize messages, never before it.
            FrameSceneOutputScope frameOutput(&app);Require(frameOutput.Ready(),"post-progress output scope borrows an actual valid target when needed");
            RenderFrameScene(&app);BakePreview(&app).Present();
            finishedSets=probe.viewports;
            if(exit==2)throw std::runtime_error("intentional viewport publication callback failure");
            return exit==0;
        },error);
        Require(reached&&complete==(exit==0)&&(exit!=2||error=="intentional viewport publication callback failure"),
            "actual Visit preserves its true false and throwing callback exit semantics");
        geometry.Check(app);original.Check();invariant.Check();
        Require(probe.begins>begins,"dynamic viewport changes do not skip the actual sampled BeginScene");
        const bool unavailable=operation==Shrink||operation==DeviceRefusal||operation==NoDevice;
        if(unavailable){
            Require(IsRectEmpty(&app.ViewportRect())&&!FrameViewportRenderable(&app)&&probe.presents==presents,
                "Visit never revives an old editor rectangle or presents over controls after explicit invalidation");
            D3DVIEWPORT9 after{};Require(SUCCEEDED(app.Renderer()->device->GetViewport(&after))&&!std::memcmp(&after,&deviceBefore,sizeof(after)),
                "UI restoration leaves the last accepted device viewport intact after callback invalidation");
        }else Valid(app);
        // In particular, restoring UI does not silently retry a refused or
        // missing viewport. This count only includes explicit callback work.
        Require(probe.viewports==finishedSets,"Visit UI restoration makes no extra SetViewport retry after callback work");
        ++cases;
    }
    ClientSize(app,1200,820);Valid(app);
    for(unsigned exit=0;exit<3;++exit){
        SamplingState original(app);std::string error;const auto sets=probe.viewports;
        const bool complete=scene_sampling::Visit(app,0,0,30,[&](double,bool){
            if(exit==2)throw std::runtime_error("intentional unchanged callback failure");return exit==0;
        },error);
        Require(complete==(exit==0)&&!std::memcmp(&app.state,original.saved.get(),sizeof(app.state))&&probe.viewports==sets,
            "no editor publication restores every state byte exactly without UI rebuild or device retry");
        original.Check();invariant.Check();
    }
    // Notifications and rebuild suppression belong to the matching app,
    // including nested watches; no window or device is needed by a watch.
    auto other=std::make_unique<MMDApp>();
    {
        FrameViewportPublicationWatch outer(&app),unrelated(other.get());
        {FrameViewportPublicationWatch inner(&app);RefreshMainWindowViewport(&app);
            Require(inner.Observed()&&outer.Observed()&&!unrelated.Observed(),"explicit editor refresh notifies every nested same-app watch and no unrelated app");}
        Require(!FrameEditorViewportRebuilding(&app)&&!FrameEditorViewportRebuilding(other.get()),"publication watchers do not leave a UI rebuild flag behind");
    }
    std::printf("PASS VISIT_VIEWPORT_PUBLICATION cases=%u no_publication_exits=3 explicit_invalidation=1 same_rect_ABA=1 dynamic_render=1\n",cases);
}
inline void Run(MMDApp* app){
    Require(app&&app==g_Block&&app->Renderer()&&app->Renderer()->device,"small viewport uses a real initialized isolated host");
    Require(!IsWindowVisible(app->MainWindow())&&!app->FloatingWindow()&&!app->RecordingWindow(),"viewport boundary fixture remains hidden and windowed");
    app->CameraMode()=1;PostLanguageSweep2(app);PostViewRefresh(app);
    ClientSize(*app,1200,820);Valid(*app);
    Require(app->CameraKeys()&&app->LightKeys()&&mdl::KeyPoolIdentity(app->CameraKeys())&&mdl::KeyPoolIdentity(app->LightKeys())&&
        mdl::KeyPoolCapacity(app->CameraKeys())&&mdl::KeyPoolCapacity(app->LightKeys()),"real viewport fixture has valid original key pools");
    SceneInvariant invariant(*app);DeviceProbe probe(app->Renderer()->device);
    for(const auto dimensions:{POINT{760,160},POINT{260,820},POINT{1200,218},POINT{760,160}}){
        D3DVIEWPORT9 previous{};Require(SUCCEEDED(app->Renderer()->device->GetViewport(&previous)),"retain real viewport before shrinking host");
        ClientSize(*app,dimensions.x,dimensions.y);Empty(*app,probe,previous);invariant.Check();
        ClientSize(*app,1200,820);Valid(*app);invariant.Check();
    }
    ClientSize(*app,760,160);
    const auto unavailable=app->ViewportRect();
    const auto sampleBegins=probe.begins,samplePresents=probe.presents;
    std::string samplingError;unsigned sampled=0;
    Require(scene_sampling::Visit(*app,0,0,30,[&](double,bool output){
        Require(output,"single-frame export sampling reaches its actual output callback");
        FrameSceneOutputScope frameOutput(app);Require(frameOutput.Ready(),"actual render opts into a private device viewport after progress");
        RenderFrameScene(app);BakePreview(app).Present();++sampled;return true;
    },samplingError)&&sampled==1,"actual private scene sampling remains available with an unusable editor viewport");
    Require(probe.begins>sampleBegins&&probe.presents==samplePresents&&!FrameViewportRenderable(app)&&
        EqualRect(&unavailable,&app->ViewportRect()),
        "geometry export renders to a valid device viewport without presenting over controls or leaving temporary logical geometry");
    invariant.Check();ClientSize(*app,1200,820);Valid(*app);
    D3DVIEWPORT9 previous{};Require(SUCCEEDED(app->Renderer()->device->GetViewport(&previous)),"retain viewport before injected D3D failure");
    probe.failNext=true;RefreshMainWindowViewport(app);
    Require(probe.forced==1&&!probe.failNext,"inject one actual SetViewport refusal without changing the native device");
    Empty(*app,probe,previous);invariant.Check();
    {
        FrameSceneOutputScope outer(app);
        Require(outer.Ready()&&!FrameViewportRenderable(app),"private output borrows the last accepted viewport after a device refusal");
        {
            FrameSceneOutputScope inner(app);
            Require(inner.Ready(),"nested private output shares a valid device viewport");
            RefreshMainWindowViewport(app);
            Require(app->ViewportRect().left==LONG(previous.X)&&app->ViewportRect().top==LONG(previous.Y)&&
                app->ViewportRect().right==LONG(previous.X+previous.Width)&&app->ViewportRect().bottom==LONG(previous.Y+previous.Height)&&
                FrameSceneRenderable(app)&&!FrameViewportRenderable(app),
                "same-rectangle editor retry publishes real geometry while private output still suppresses presentation");
        }
        Require(!IsRectEmpty(&app->ViewportRect())&&!FrameViewportRenderable(app),
            "inner scope cannot clear same-rectangle geometry successfully republished by the editor");
    }
    Valid(*app);invariant.Check();
    VisitPublicationChecks(*app,probe,invariant);
    const auto begins=probe.begins,presents=probe.presents;
    app->MessageSeen()=1;RenderFrameScene(app);BakePreview(app).Present();
    Require(probe.begins>begins&&probe.presents>presents,"restored hidden client renders and presents normally again");
    Require(!probe.invalid,"all observed production SetViewport calls fit their actual active render target");
    invariant.Check();
    std::printf("PASS SMALL_VIEWPORT_HOST cases=4 actual_device_refusal=1 private_output=1 nested_same_geometry_retry=1 restored_render=1 invalid_native_viewports=0 journal_checked=%u\n",invariant.history?1u:0u);
}
}
