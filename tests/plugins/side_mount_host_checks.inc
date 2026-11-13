// Actual integration. No substitute NativeUi, geometry hook or global input.
#include "mikumikustudio/main_panel_layout.hpp"

namespace side_mount_host_checks {
namespace fs=std::filesystem;
using small_viewport_host_checks::Require;
// Shared with the fixture DLL; it exports no HWND or registry pointer.
struct ProbeState {
    uint32_t size,submitted,completed,mask;
    MmsResult result;
    uint32_t reads,invokes,taskRuns,taskCancels,removed,reserved[2];
};
static_assert(sizeof(ProbeState)==48,"side mount test protocol");
using Query=MmsResult(MMS_CALL*)(ProbeState*);
using Change=MmsResult(MMS_CALL*)(uint32_t,uint32_t);
using Target=MmsResult(MMS_CALL*)(uint32_t,uint32_t,MmsUiRefreshTargetV1*,float*);
using Handle=MmsResult(MMS_CALL*)(uint32_t,uint32_t,MmsHandle*);
static fs::path ExecutableDirectory() {
    wchar_t path[32768]{};const DWORD length=GetModuleFileNameW(nullptr,path,32768);
    Require(length&&length<32768,"read dedicated fixture executable path");
    const fs::path executable(path);
    Require(executable.stem()==L"side_mount_host_tests","only the dedicated fixture executable creates its PLUGINS tree");
    return executable.parent_path();
}
static void PreparePackage(const fs::path& source) {
    Require(source.is_absolute()&&fs::is_regular_file(source),"probe DLL argument is an existing absolute file");
    const auto directory=ExecutableDirectory();const auto plugins=directory/L"PLUGINS";
    // create_directory is the exclusive ownership claim. Never reuse, remove,
    // overwrite or repair a preexisting package tree, including failed runs.
    Require(fs::create_directory(plugins),"PLUGINS must not already exist; copy this EXE into a new isolated scratch directory");
    const auto package=plugins/L"side-fixture";
    Require(fs::create_directory(package),"create own side mount fixture package");
    Require(fs::copy_file(source,package/L"side_mount_host_probe.dll",fs::copy_options::none),"copy original fixture DLL without overwriting files");
    std::ofstream manifest(package/L"plugin.json",std::ios::binary|std::ios::out);
    Require(bool(manifest),"create own plugin manifest");
    manifest << R"({"schemaVersion":1,"id":"org.mms.test.sidemount","name":"Original side mount host fixture","version":"1.0.0","kind":"native","abi":{"major":1,"minMinor":0},"entry":"side_mount_host_probe.dll","architecture":"x64","minWindows":"6.1.7601","capabilities":["ui.command","ui.element","ui.property"],"requiresServices":[{"id":"mms.ui","major":1,"minMinor":0},{"id":"mms.ui.layout","major":1,"minMinor":0},{"id":"mms.ui.refresh","major":1,"minMinor":0}],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})";
    manifest.close();Require(bool(manifest),"finish own plugin manifest");
    const auto metrics=directory/L"side-mount-metrics.jsonl";
    Require(!fs::exists(metrics)&&SetEnvironmentVariableW(L"MMS_UI_METRICS",metrics.c_str()),"enable metrics only in the new scratch directory");
}
static void Pump() {
    MSG message{};unsigned dispatched=0;
    while(dispatched++<256&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
        Require(message.message!=WM_QUIT,"fixture keeps its own host alive");
        if(message.hwnd) { DWORD process=0;GetWindowThreadProcessId(message.hwnd,&process);
            Require(process==GetCurrentProcessId(),"dispatch only windows belonging to this fixture process"); }
        TranslateMessage(&message);DispatchMessageW(&message);
    }
}
static void Settle(MMDApp& app) { for(unsigned i=0;i!=4;++i){Pump();plugins::Refresh(&app);}Pump(); }
static void SizeMetadata(MMDApp& app,const char* stage,int requestedWidth,int requestedHeight) {
    const HWND window=app.MainWindow();const auto user=GetModuleHandleW(L"user32.dll");
    using WindowDpi=UINT(WINAPI*)(HWND);
    using WindowContext=HANDLE(WINAPI*)(HWND);
    using ThreadContext=HANDLE(WINAPI*)();
    using Awareness=int(WINAPI*)(HANDLE);
    const auto getDpi=reinterpret_cast<WindowDpi>(GetProcAddress(user,"GetDpiForWindow"));
    const auto getWindowContext=reinterpret_cast<WindowContext>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
    const auto getThreadContext=reinterpret_cast<ThreadContext>(GetProcAddress(user,"GetThreadDpiAwarenessContext"));
    const auto getAwareness=reinterpret_cast<Awareness>(GetProcAddress(user,"GetAwarenessFromDpiAwarenessContext"));
    const HANDLE windowContext=getWindowContext?getWindowContext(window):nullptr;
    const HANDLE threadContext=getThreadContext?getThreadContext():nullptr;
    RECT client{},outer{},work{};const bool clientKnown=GetClientRect(window,&client)!=FALSE;
    const bool outerKnown=GetWindowRect(window,&outer)!=FALSE;
    const bool workKnown=SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)!=FALSE;
    WINDOWPLACEMENT placement{};placement.length=sizeof(placement);const bool placementKnown=GetWindowPlacement(window,&placement)!=FALSE;
    DWORD process=0;const DWORD thread=GetWindowThreadProcessId(window,&process);
    std::printf("SIDE_MOUNT_SIZE stage=%s request_client=%dx%d own_process=%u thread=%lu "
        "client_known=%u client=%ld,%ld,%ld,%ld outer_known=%u outer=%ld,%ld,%ld,%ld "
        "style=%llx exstyle=%llx visible=%d iconic=%d zoomed=%d placement_known=%u show=%u "
        "dpi=%u process_aware=%d window_context=%p thread_context=%p window_awareness=%d thread_awareness=%d "
        "screen=%dx%d virtual=%dx%d max_track=%dx%d work_known=%u work=%ld,%ld,%ld,%ld sidebar=%d ready=%u\n",
        stage,requestedWidth,requestedHeight,unsigned(process==GetCurrentProcessId()),static_cast<unsigned long>(thread),
        unsigned(clientKnown),client.left,client.top,client.right,client.bottom,unsigned(outerKnown),outer.left,outer.top,outer.right,outer.bottom,
        static_cast<unsigned long long>(GetWindowLongPtrW(window,GWL_STYLE)),static_cast<unsigned long long>(GetWindowLongPtrW(window,GWL_EXSTYLE)),
        int(IsWindowVisible(window)),int(IsIconic(window)),int(IsZoomed(window)),unsigned(placementKnown),placement.showCmd,
        getDpi?getDpi(window):0,int(IsProcessDPIAware()),windowContext,threadContext,
        getAwareness&&windowContext?getAwareness(windowContext):-1,getAwareness&&threadContext?getAwareness(threadContext):-1,
        GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),
        GetSystemMetrics(SM_CXMAXTRACK),GetSystemMetrics(SM_CYMAXTRACK),unsigned(workKnown),work.left,work.top,work.right,work.bottom,
        app.SidebarWidth(),unsigned(app.WindowLayoutReady()));
}
struct FixtureSizes { int wideWidth,wideHeight,narrowWidth,narrowHeight; };
static FixtureSizes SelectSizes(MMDApp& app) {
    const HWND window=app.MainWindow();const WindowDpiScope dpi(window);
    SizeMetadata(app,"select-measure",0,0);
    RECT client{},outer{};
    Require(GetClientRect(window,&client)&&GetWindowRect(window,&outer)&&!client.left&&!client.top&&client.right>0&&client.bottom>0,
        "measure the own window and client in the same DPI coordinate system");
    const int nonclientWidth=int(outer.right-outer.left-client.right);
    const int nonclientHeight=int(outer.bottom-outer.top-client.bottom);
    const int maxTrackWidth=GetSystemMetrics(SM_CXMAXTRACK),maxTrackHeight=GetSystemMetrics(SM_CYMAXTRACK);
    Require(nonclientWidth>=0&&nonclientHeight>=0&&maxTrackWidth>nonclientWidth&&maxTrackHeight>nonclientHeight,
        "USER32 max-track dimensions exceed measured nonclient borders");
    const int maxClientWidth=maxTrackWidth-nonclientWidth,maxClientHeight=maxTrackHeight-nonclientHeight;
    FixtureSizes sizes{std::min(1400,maxClientWidth),std::min(820,maxClientHeight),0,0};
    sizes.narrowWidth=sizes.wideWidth-120;sizes.narrowHeight=sizes.wideHeight-60;
    std::printf("SIDE_MOUNT_SIZE_SELECTION max_track=%dx%d nonclient=%dx%d max_client=%dx%d wide=%dx%d narrow=%dx%d\n",
        maxTrackWidth,maxTrackHeight,nonclientWidth,nonclientHeight,maxClientWidth,maxClientHeight,
        sizes.wideWidth,sizes.wideHeight,sizes.narrowWidth,sizes.narrowHeight);
    Require(sizes.wideWidth>=1320&&sizes.wideHeight>=700,
        "actual desktop limits cannot provide the required 1320x700 wide client and real bottom-divider room");
    Require(sizes.wideWidth<=maxClientWidth&&sizes.wideHeight<=maxClientHeight&&sizes.wideWidth<=1400&&sizes.wideHeight<=820&&
        sizes.narrowWidth>0&&sizes.narrowHeight>0&&sizes.narrowWidth<sizes.wideWidth&&sizes.narrowHeight<sizes.wideHeight&&
        sizes.wideWidth-sizes.narrowWidth==120&&sizes.wideHeight-sizes.narrowHeight==60,
        "two distinct exact fixture sizes fit the measured native limits");
    return sizes;
}
static void ClientSize(MMDApp& app,int width,int height) {
    // Keep the existing exact-client-size contract. Record the real USER32
    // boundary before choosing any different dimensions for this fixture.
    const HWND window=app.MainWindow();const WindowDpiScope dpi(window);
    SizeMetadata(app,"before",width,height);
    RECT extent{0,0,width,height};
    Require(AdjustWindowRectEx(&extent,DWORD(GetWindowLongPtrW(window,GWL_STYLE)),GetMenu(window)!=nullptr,
        DWORD(GetWindowLongPtrW(window,GWL_EXSTYLE)))!=FALSE,"calculate exact own-host client size");
    int outerWidth=extent.right-extent.left,outerHeight=extent.bottom-extent.top;RECT actual{};
    for(unsigned attempt=0;attempt<5;++attempt) {
        SetLastError(ERROR_SUCCESS);
        const bool moved=SetWindowPos(window,nullptr,0,0,outerWidth,outerHeight,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE;
        const DWORD moveError=moved?ERROR_SUCCESS:GetLastError();
        const bool measured=GetClientRect(window,&actual)!=FALSE;
        std::printf("SIDE_MOUNT_SIZE_ATTEMPT index=%u request_outer=%dx%d moved=%u error=%lu client_known=%u actual_client=%ldx%ld\n",
            attempt,outerWidth,outerHeight,unsigned(moved),static_cast<unsigned long>(moveError),unsigned(measured),actual.right,actual.bottom);
        SizeMetadata(app,"after",width,height);
        Require(moved&&measured,"resize and measure own hidden host without showing or activating it");
        if(actual.right==width&&actual.bottom==height)break;
        outerWidth+=width-actual.right;outerHeight+=height-actual.bottom;
    }
    Require(actual.right==width&&actual.bottom==height,"actual client size matches the measured side mount fixture");
    app.SidebarWidth()=250;HandleWindowSize(&app);RefreshMainWindowViewport(&app);
}
struct Fixture {
    Query query;Change change;Target target;Handle handle;
    explicit Fixture() {
        const auto module=GetModuleHandleW(L"side_mount_host_probe.dll");Require(module!=nullptr,"production runtime loaded the original fixture DLL");
        query=reinterpret_cast<Query>(GetProcAddress(module,"SideMountProbeQuery"));
        change=reinterpret_cast<Change>(GetProcAddress(module,"SideMountProbeChange"));
        target=reinterpret_cast<Target>(GetProcAddress(module,"SideMountProbeTarget"));
        handle=reinterpret_cast<Handle>(GetProcAddress(module,"SideMountProbeHandle"));
        Require(query&&change&&target&&handle,"negotiate read-only identity/geometry and queued command probes");
    }
    ProbeState State() const { ProbeState out{};out.size=sizeof(out);Require(query(&out)==MMS_OK,"read fixture state");return out; }
    void Command(MMDApp& app,uint32_t operation,uint32_t mask) {
        const auto before=State();Require(change(operation,mask)==MMS_OK,"invoke ordinary command that queues UI mutation");
        const auto submitted=State();Require(submitted.submitted==before.submitted+1&&submitted.completed==before.completed,
            "command does not mutate the registered UI inline");
        const auto start=GetTickCount64();
        while(State().completed!=submitted.submitted&&GetTickCount64()-start<5000){Pump();Sleep(1);}
        const auto after=State();Require(after.completed==submitted.submitted&&after.result==MMS_OK&&after.taskRuns==before.taskRuns+1,
            "one actual enqueueUi task completes the mount change");
        Settle(app);
    }
};
static bool ShownWithin(HWND main,HWND child) {
    if(!IsWindow(child))return false;
    for(HWND current=child;current&&current!=main;current=GetParent(current))
        if(!(GetWindowLongPtrW(current,GWL_STYLE)&WS_VISIBLE))return false;
    return IsChild(main,child)!=FALSE;
}
static RECT In(HWND main,HWND child) {
    RECT box{};Require(IsWindow(child)&&GetWindowRect(child,&box),"read actual fixture control rectangle");
    SetLastError(0);Require(MapWindowPoints(nullptr,main,reinterpret_cast<POINT*>(&box),2)||!GetLastError(),"map actual child to its own main window");return box;
}
struct Search {
    const wchar_t* text;const wchar_t* cls;HWND found=nullptr;unsigned count=0;
    static BOOL CALLBACK Visit(HWND window,LPARAM parameter) {
        auto& s=*reinterpret_cast<Search*>(parameter);wchar_t text[128]{},cls[64]{};
        GetWindowTextW(window,text,128);GetClassNameW(window,cls,64);
        if((!s.text||!std::wcscmp(text,s.text))&&(!s.cls||!_wcsicmp(cls,s.cls))){s.found=window;++s.count;}return TRUE;
    }
};
static HWND Find(HWND root,const wchar_t* text,const wchar_t* cls) {
    Search s{text,cls};EnumChildWindows(root,Search::Visit,reinterpret_cast<LPARAM>(&s));
    Require(s.count==1&&s.found,"find exactly one labelled fixture control under its own host");return s.found;
}
constexpr const wchar_t* buttonLabels[]={L"Side fixture left button",L"Side fixture right button",L"Side fixture property button"};
constexpr const wchar_t* valueLabels[]={L"Side fixture left value",L"Side fixture right value",L"Side fixture property value"};
static float DipFactor(HWND main) {
    using Function=UINT(WINAPI*)(HWND);
    const auto fn=reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    if(fn){const auto dpi=fn(main);Require(dpi!=0,"read own window DPI");return 96.0f/dpi;}
    HDC dc=GetDC(main);Require(dc!=nullptr,"read own legacy DPI context");const auto dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(main,dc);
    Require(dpi>0,"legacy DPI is positive");return 96.0f/dpi;
}
struct Geometry {
    std::array<HWND,3> buttons{},inputs{};
    std::array<MmsUiTargetRefV1,3> refs{},inputRefs{};
    std::array<std::array<MmsHandle,3>,3> registrations{};
    std::array<RECT,3> groups{};
};
static Geometry Bounds(MMDApp& app,const Fixture& fixture) {
    const HWND main=app.MainWindow();const WindowDpiScope dpi(main);Geometry result;
    RECT client{};Require(GetClientRect(main,&client),"read actual docked client");
    const int left=plugins::LeftAreaWidth(&app),right=plugins::RightAreaWidth(&app),property=plugins::PropertyAreaHeight(&app);
    Require(left>0&&right>0&&property>0,"all three production mount reservations are nonempty");
    const auto nativePanels=MainPanelLayout(&app,client.bottom);
    const auto rows=FitViewportToolbarRows(nativePanels.bottomTop,ui_scale::Percent());
    FrameViewportGeometry geometry;
    Require(CalculateFrameViewport(client,app.SidebarWidth(),plugins::ToolbarHeight(&app),FrameViewportMode::Docked,
        app.RenderWidth(),app.RenderHeight(),geometry,property,left,right,nativePanels.bottomExtra,rows),
        "calculate the declared production reservation contract");
    Require(EqualRect(&geometry.scene,&app.ViewportRect()),"actual published scene matches the common reservation geometry");
    const auto present=FramePresentationRect(&app),overlay=FrameOverlayRect(&app);const auto panels=FramePanelLayout(&app);
    Require(FrameRectWithin(present,geometry.presentationBounds)&&FrameRectWithin(overlay,geometry.presentationBounds)&&
        FrameRectWithin(app.ViewportRect(),present),"scene, Present and overlay exclude both reserved side areas");
    Require(panels.topBand.left>=geometry.available.left&&panels.topBand.right<=geometry.available.right&&
        panels.bottomBand.left>=geometry.available.left&&panels.bottomBand.right<=geometry.available.right,
        "native viewport rows use the same available horizontal range");
    const float factor=DipFactor(main);
    for(unsigned root=0;root!=3;++root) {
        result.buttons[root]=Find(main,buttonLabels[root],L"Button");
        const auto label=Find(main,valueLabels[root],L"Static");
        result.inputs[root]=GetParent(Find(GetParent(label),nullptr,L"Edit"));
        Require(ShownWithin(main,result.buttons[root])&&ShownWithin(main,result.inputs[root]),"registered fixture controls remain shown inside the hidden own main window");
        for(unsigned item=0;item!=3;++item) {
            Require(fixture.handle(root,item,&result.registrations[root][item])==MMS_OK,"read original registered element handle");
            MmsUiRefreshTargetV1 target{};target.h={sizeof(target),1,0};float box[4]{};
            Require(fixture.target(root,item,&target,box)==MMS_OK&&target.ready&&target.visible,"query the original published refresh target");
            Require(target.target.element==result.registrations[root][item],"published presentation belongs to the original registration");
            if(item==0) {
                result.groups[root]={LONG(std::lround(box[0]/factor)),LONG(std::lround(box[1]/factor)),
                    LONG(std::lround((box[0]+box[2])/factor)),LONG(std::lround((box[1]+box[3])/factor))};
                continue;
            }
            const auto actual=In(main,item==1?result.buttons[root]:result.inputs[root]);
            Require(std::abs(box[0]-actual.left*factor)<1.1f&&std::abs(box[1]-actual.top*factor)<1.1f&&
                std::abs(box[2]-(actual.right-actual.left)*factor)<1.1f&&std::abs(box[3]-(actual.bottom-actual.top)*factor)<1.1f,
                "public DIP geometry matches actual native child bounds");
            RECT intersection{};Require(!IntersectRect(&intersection,&actual,&present)&&!IntersectRect(&intersection,&actual,&overlay),
                "neither Present nor overlay intersects a fixture BUTTON or INPUT");
            if(item==1)result.refs[root]=target.target;
            else result.inputRefs[root]=target.target;
        }
    }
    RECT intersection{};Require(!IntersectRect(&intersection,&result.groups[0],&result.groups[1]),"left and right registered groups do not overlap");
    const RECT slots[]={{LONG(app.SidebarWidth()+9),panels.topBand.top,geometry.available.left,geometry.presentationBounds.bottom},
        {geometry.available.right,panels.topBand.top,client.right,geometry.presentationBounds.bottom},result.groups[2]};
    for(int id=536;id<=557;++id) {
        HWND child=GetDlgItem(main,id);if(!child||!ShownWithin(main,child))continue;
        const auto box=In(main,child);if(!IntersectRect(&intersection,&box,&client))continue;
        for(const auto& slot:slots)Require(!IntersectRect(&intersection,&box,&slot),"original native viewport controls do not intrude into extension mounts");
    }
    small_viewport_host_checks::Valid(app);return result;
}
struct DamageProbe {
    using Function=BOOL(WINAPI*)(HWND,const RECT*,HRGN,UINT);
    inline static DamageProbe* active=nullptr;
    HWND main;Function real=nullptr;std::array<RECT,512> regions{};size_t count=0;bool overflow=false;
    explicit DamageProbe(HWND window):main(window) {
        Require(!active,"one own-window damage observer");active=this;
        try { Install(reinterpret_cast<void*>(&RedrawWindow),reinterpret_cast<void*>(Observe),reinterpret_cast<void**>(&real)); }
        catch(...){active=nullptr;throw;}
    }
    ~DamageProbe(){MH_DisableHook(reinterpret_cast<void*>(&RedrawWindow));MH_RemoveHook(reinterpret_cast<void*>(&RedrawWindow));active=nullptr;}
    static BOOL WINAPI Observe(HWND window,const RECT* rect,HRGN region,UINT flags) {
        auto& self=*active;
        if(window==self.main&&(flags&RDW_INVALIDATE)) {
            RECT damage{};if(rect)damage=*rect;else if(region)GetRgnBox(region,&damage);else GetClientRect(window,&damage);
            if(self.count<self.regions.size())self.regions[self.count++]=damage;else self.overflow=true;
        }
        return self.real(window,rect,region,flags);
    }
    void Clear(){count=0;overflow=false;}
    bool Touches(const RECT& box)const {
        Require(!overflow,"own damage observer remains bounded");
        for(size_t i=0;i<count;++i){RECT overlap{};if(IntersectRect(&overlap,&regions[i],&box))return true;}return false;
    }
};
struct PresentProbe {
    using Function=HRESULT(WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
    inline static PresentProbe* active=nullptr;
    IDirect3DDevice9* device;Function real=nullptr;void* entry;RECT source{},destination{};HWND window=nullptr;unsigned calls=0;bool exact=false;
    explicit PresentProbe(IDirect3DDevice9* value):device(value),entry((*reinterpret_cast<void***>(value))[17]) {
        Require(!active,"one real Present observer");active=this;
        try{Install(entry,reinterpret_cast<void*>(Observe),reinterpret_cast<void**>(&real));}catch(...){active=nullptr;throw;}
    }
    ~PresentProbe(){MH_DisableHook(entry);MH_RemoveHook(entry);active=nullptr;}
    static HRESULT WINAPI Observe(IDirect3DDevice9* device,const RECT* source,const RECT* destination,HWND window,const RGNDATA* dirty) {
        auto& self=*active;if(device==self.device){++self.calls;self.exact=source&&destination;
            if(source)self.source=*source;if(destination)self.destination=*destination;self.window=window;}
        return self.real(device,source,destination,window,dirty);
    }
    void Check(MMDApp& app) {
        const auto before=calls;const auto expected=FramePresentationRect(&app);BakePreview(&app).Present();
        Require(calls==before+1&&exact&&window==(app.FloatingWindow()?app.FloatingWindow():app.MainWindow())&&
            EqualRect(&source,&expected)&&EqualRect(&destination,&expected),"real D3D9 Present receives exactly the excluded-mount source and destination rectangles");
    }
};
static void SameObjects(const Geometry& before,const Geometry& after) {
    for(unsigned i=0;i!=3;++i)Require(before.buttons[i]==after.buttons[i]&&before.inputs[i]==after.inputs[i]&&
        before.registrations[i]==after.registrations[i]&&before.refs[i].element==after.refs[i].element&&
        before.refs[i].instanceGeneration==after.refs[i].instanceGeneration&&before.inputRefs[i].instanceGeneration==after.inputRefs[i].instanceGeneration,
        "resize and render-window modes retain original controls and published object identity");
}
static void OverlayHit(MMDApp& app,const Geometry& geometry) {
    const auto x=app.MouseX(),y=app.MouseY();const auto input=app.ViewportInputActive();
    const auto hovered=app.ViewportToolHovered();const auto operation=app.ViewportToolOperation();
    const auto restore=[&]() noexcept {
        app.MouseX()=x;app.MouseY()=y;app.ViewportInputActive()=input;
        app.ViewportToolHovered()=hovered;app.ViewportToolOperation()=operation;
    };
    app.ViewportInputActive()=1;bool clear=true;
    try {
        for(auto child:geometry.buttons) {
            const auto box=In(app.MainWindow(),child);
            app.MouseX()=(box.left+box.right)/2;app.MouseY()=(box.top+box.bottom)/2;
            clear&=ViewportToolAtPointForTest(&app)==0;
        }
    } catch(...) { restore();throw; }
    restore();
    Require(clear,"the actual viewport tool hit-test does not claim extension buttons");
}
static void Drag(MMDApp& app,const Fixture& fixture,const Geometry& original) {
    HWND main=app.MainWindow();const auto old=app.ViewportRect();const int width=app.SidebarWidth();const float ratio=app.SidebarRatio();
    SendMessageW(main,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(width+3,100));
    Require(app.SidebarResizeDragging()&&GetCapture()==main,"own native sidebar divider starts a real captured gesture");
    for(int position:{width+35,width+70}) {
        SendMessageW(main,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(position,100));
        Require(app.SidebarWidth()==position&&app.WindowLayoutReady(),"native sidebar geometry updates immediately");
        SameObjects(original,Bounds(app,fixture));
    }
    SendMessageW(main,WM_CANCELMODE,0,0);
    Require(!app.SidebarResizeDragging()&&GetCapture()!=main&&app.SidebarWidth()==width&&app.SidebarRatio()==ratio&&
        EqualRect(&old,&app.ViewportRect()),"cancel restores the exact side reservation and viewport");
    RECT client{};GetClientRect(main,&client);int edge=-1;
    for(const auto& span:BottomPanelSpans(&app))if(span.id==1)edge=BottomPanelX(&app,span.right);
    Require(edge>=0,"locate the actual bottom panel divider");
    const int previous=app.bottomPanelSizing.extra[1];
    SendMessageW(main,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(edge,client.bottom-100));
    Require(app.bottomPanelSizing.dragging==1&&GetCapture()==main,"own bottom divider captures without global mouse input");
    SendMessageW(main,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+35,client.bottom-100));
    Require(app.bottomPanelSizing.extra[1]>previous,"bottom controls follow the actual native divider");
    SameObjects(original,Bounds(app,fixture));
    SendMessageW(main,WM_CANCELMODE,0,0);
    Require(app.bottomPanelSizing.dragging<0&&GetCapture()!=main&&app.bottomPanelSizing.extra[1]==previous,
        "bottom cancellation restores the complete native layout");
}
static void Modes(MMDApp& app,const Fixture& fixture,const Geometry& original) {
    const auto viewport=app.ViewportRect();const auto before=FramePresentationRect(&app);const auto left=plugins::LeftAreaWidth(&app),right=plugins::RightAreaWidth(&app);
    InitFlagSubsystem(&app);Require(app.FloatingWindow()&&IsWindow(app.FloatingWindow()),"create the actual separate render window");
    plugins::Layout(&app);Settle(app);RefreshSeparateWindowViewport(&app);LayoutViewportPanels(&app);
    Require(!plugins::LeftAreaWidth(&app)&&!plugins::RightAreaWidth(&app),"floating mode ignores both side reservations");
    const auto floating=FramePanelLayout(&app);
    Require(floating.topRowHeight==MulDiv(25,ui_scale::Percent(),100)&&
        FrameRectWithin(app.ViewportRect(),floating.overlay),"floating viewport uses the current toolbar scale");
    for(auto child:original.buttons)Require(!ShownWithin(app.MainWindow(),child),"floating mode hides extension mount controls");
    Require(!IsWindowVisible(app.FloatingWindow()),"own floating top-level window remains hidden");
    SaveFlagSubsystem(&app);HandleWindowSize(&app);Settle(app);
    Require(!app.FloatingWindow()&&plugins::LeftAreaWidth(&app)==left&&plugins::RightAreaWidth(&app)==right,
        "docked return restores both actual reservations");
    SameObjects(original,Bounds(app,fixture));
    const LPARAM alt=LPARAM(1)<<29;
    SendMessageW(app.MainWindow(),WM_SYSKEYDOWN,VK_RETURN,alt);
    Require(app.FullscreenMode()!=0,"own Alt+Enter message reaches the production fullscreen handler");
    plugins::Layout(&app);Settle(app);
    Require(!plugins::LeftAreaWidth(&app)&&!plugins::RightAreaWidth(&app),"fullscreen ignores both side reservations");
    for(auto child:original.buttons)Require(!ShownWithin(app.MainWindow(),child),"fullscreen keeps original extension controls hidden");
    SendMessageW(app.MainWindow(),WM_KEYDOWN,VK_ESCAPE,0);
    Require(!app.FullscreenMode(),"own Escape message returns to ordinary mode");
    Settle(app);SameObjects(original,Bounds(app,fixture));
    const auto restored=FramePresentationRect(&app);
    Require(EqualRect(&viewport,&app.ViewportRect())&&EqualRect(&before,&restored),"floating and fullscreen cycles restore exact ordinary scene and Present rectangles");
}
static void Run(MMDApp& app,bool bound) {
    Require(!IsWindowVisible(app.MainWindow())&&!GetCapture(),"only the own hidden host participates");
    Require(*ui_metrics::Output()!=0,"real layout and scene counters are enabled");
    if(bound)Require(plugins::NativeHistory(&app)->SetMode(plugins::SharedHistory::RecordingMode::AllOperations)==MMS_OK,
        "all-operation history still excludes mount changes");
    const auto sizes=SelectSizes(app);
    ClientSize(app,sizes.wideWidth,sizes.wideHeight);app.SidebarRatio()=.25f;HandleWindowSize(&app);Settle(app);
    Fixture fixture;Require(fixture.State().reads>=3,"all INPUTs use one registered public property");
    fixture.Command(app,1,0);
    Require(!plugins::LeftAreaWidth(&app)&&!plugins::RightAreaWidth(&app)&&!plugins::PropertyAreaHeight(&app),"hidden mounts reserve zero space");
    const auto empty=app.ViewportRect(),emptyPresent=FramePresentationRect(&app);
    fixture.Command(app,1,7);auto objects=Bounds(app,fixture);
    Require(ui_scale_popup_fixture::Choose(app.MainWindow(),125),"confirm 125 percent in the actual scale popup");
    Settle(app);
    const auto scaled=FramePanelLayout(&app);
    Require(ui_scale::Percent()==125&&scaled.topRowHeight==31,"Help scale updates the native toolbar beside real plugin mounts");
    SameObjects(objects,Bounds(app,fixture));
    OverlayHit(app,objects);
    Modes(app,fixture,objects);
    Require(ui_scale_popup_fixture::Choose(app.MainWindow(),100),"restore 100 percent in the actual scale popup");
    Settle(app);
    Require(ui_scale::Percent()==100,"Help restores the original toolbar scale with plugins mounted");
    SameObjects(objects,Bounds(app,fixture));
    small_viewport_host_checks::SceneInvariant invariant(app);
    const auto dirty=app.SceneModified();const auto physicsReset=app.PhysicsResetPending();const auto registered=app.RecordedFrameCountByte1();
    const auto scene=ui_metrics::counts[ui_metrics::Scene].load(),physics=ui_metrics::counts[ui_metrics::Physics].load();
    DamageProbe damage(app.MainWindow());PresentProbe present(app.Renderer()->device);present.Check(app);OverlayHit(app,objects);
    for(unsigned side=0;side!=2;++side) {
        const unsigned mask=7u&~(1u<<side);
        damage.Clear();fixture.Command(app,1,mask);
        Require((plugins::LeftAreaWidth(&app)>0)==bool(mask&1)&&(plugins::RightAreaWidth(&app)>0)==bool(mask&2),
            "each side independently returns or releases its actual reservation");
        Require(damage.Touches(objects.groups[side]),"released side mount receives native invalidation for its old region");
        for(unsigned root=0;root!=3;++root)for(unsigned item=0;item!=3;++item) {
            MmsHandle original=0;Require(fixture.handle(root,item,&original)==MMS_OK&&original==objects.registrations[root][item],
                "explicit layout hiding preserves every registered element handle");
        }
        MmsUiRefreshTargetV1 hidden{};hidden.h={sizeof(hidden),1,0};float hiddenRect[4]{};
        Require(fixture.target(side,1,&hidden,hiddenRect)==MMS_E_NOT_READY&&fixture.target(side,2,&hidden,hiddenRect)==MMS_E_NOT_READY,
            "hidden registered controls retire their old native presentations");
        invariant.Check();fixture.Command(app,1,7);const auto current=Bounds(app,fixture);
        for(unsigned root=0;root!=3;++root) {
            Require(current.registrations[root]==objects.registrations[root],"explicit visibility preserves original registry ownership and identity");
            if(root==side)Require(current.refs[root].instanceGeneration!=objects.refs[root].instanceGeneration&&
                current.inputRefs[root].instanceGeneration!=objects.inputRefs[root].instanceGeneration,
                "restored layout-hidden views have fresh instances, invalidating the old references");
            else Require(current.buttons[root]==objects.buttons[root]&&current.inputs[root]==objects.inputs[root]&&
                current.refs[root].instanceGeneration==objects.refs[root].instanceGeneration&&
                current.inputRefs[root].instanceGeneration==objects.inputRefs[root].instanceGeneration,
                "visibility change leaves unrelated mount views intact");
        }
        objects=current;OverlayHit(app,objects);invariant.Check();
    }
    Drag(app,fixture,objects);invariant.Check();
    ClientSize(app,sizes.narrowWidth,sizes.narrowHeight);Settle(app);SameObjects(objects,Bounds(app,fixture));
    ClientSize(app,sizes.wideWidth,sizes.wideHeight);Settle(app);SameObjects(objects,Bounds(app,fixture));
    Modes(app,fixture,objects);
    ClientSize(app,sizes.wideWidth,sizes.wideHeight);Settle(app);SameObjects(objects,Bounds(app,fixture));invariant.Check();present.Check(app);
    damage.Clear();fixture.Command(app,2,0);
    Require(!plugins::LeftAreaWidth(&app)&&!plugins::RightAreaWidth(&app)&&!plugins::PropertyAreaHeight(&app),"removing original registrations releases every reservation");
    const auto restored=FramePresentationRect(&app);
    Require(EqualRect(&empty,&app.ViewportRect())&&EqualRect(&emptyPresent,&restored),"released mount space returns to the original zero-extension geometry");
    for(unsigned root=0;root!=3;++root){Require(!IsWindow(objects.buttons[root])&&!IsWindow(objects.inputs[root]),"removed controls leave no live stale HWND");
        MmsUiRefreshTargetV1 target{};target.h={sizeof(target),1,0};float rect[4]{};
        Require(fixture.target(root,1,&target,rect)==MMS_E_STALE,"removed refresh identity becomes stale");}
    Require(damage.Touches(objects.groups[0])&&damage.Touches(objects.groups[1]),"both removed mount regions are invalidated for repaint without pixel inspection");
    present.Check(app);invariant.Check();
    Require(app.SceneModified()==dirty&&app.PhysicsResetPending()==physicsReset&&app.RecordedFrameCountByte1()==registered&&
        ui_metrics::counts[ui_metrics::Scene].load()==scene&&ui_metrics::counts[ui_metrics::Physics].load()==physics,
        "real layout and modes preserve dirty registration history playhead and scene/physics evaluation counts");
    const auto state=fixture.State();Require(!state.invokes&&!state.taskCancels&&state.removed,"only requested queued mutations ran; no fixture value/button edits occurred");
    std::printf("SIDE_MOUNT_HOST tasks=%u property_reads=%u scene=%llu physics=%llu bound=%u\n",state.taskRuns,state.reads,
        static_cast<unsigned long long>(scene),static_cast<unsigned long long>(physics),unsigned(bound));
}
}
