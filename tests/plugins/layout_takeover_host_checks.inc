// Actual Manager/NativeUi and mouse dispatch. Only process-local input APIs
// are intercepted; no host layout, preview, refresh or gesture code is replaced.
#include "../../src/plugins/ui_registry.hpp"
#include <mms/layout_session.h>
#include <cstdlib>
#include <new>
namespace layout_takeover_allocation_fault {
thread_local bool armed=false;
thread_local unsigned failures=0;
}
// Replace only this test executable's allocation entry points. USER32 hooks
// merely arm the fault; the exception originates at a real C++ new expression.
void* operator new(std::size_t size){
    if(layout_takeover_allocation_fault::armed){
        layout_takeover_allocation_fault::armed=false;
        ++layout_takeover_allocation_fault::failures;
        throw std::bad_alloc{};
    }
    if(auto* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* memory)noexcept{std::free(memory);}
void operator delete[](void* memory)noexcept{std::free(memory);}
void operator delete(void* memory,std::size_t)noexcept{std::free(memory);}
void operator delete[](void* memory,std::size_t)noexcept{std::free(memory);}
namespace layout_takeover_host_checks {
using namespace mikumikustudio::plugins;
static HWND capture=nullptr, focus=nullptr;
static HCURSOR cursor=nullptr;
static unsigned warps=0;
static void (*layoutObserver)()=nullptr;
static void (*releaseObserver)()=nullptr;
static HWND WINAPI Capture(){return capture;}
static HWND WINAPI SetCaptureLocal(HWND value){auto old=capture;capture=value;return old;}
static BOOL WINAPI ReleaseCaptureLocal(){auto old=capture;capture=nullptr;if(old)SendMessageW(old,WM_CAPTURECHANGED,0,0);
    if(releaseObserver)releaseObserver();return TRUE;}
static HWND WINAPI Focus(){return focus;}
static HWND WINAPI SetFocusLocal(HWND value){auto old=focus;focus=value;return old;}
static HWND WINAPI Foreground(){return g_Block?g_Block->MainWindow():nullptr;}
static BOOL WINAPI SetForegroundLocal(HWND){return TRUE;}
static HCURSOR WINAPI SetCursorLocal(HCURSOR value){auto old=cursor;cursor=value;return old;}
static BOOL WINAPI Warp(int,int){++warps;return TRUE;}
static SHORT WINAPI Key(int){return 0;}
static decltype(&SetWindowPos) originalPosition=nullptr;
static decltype(&EndDeferWindowPos) originalEndDefer=nullptr;
static decltype(&GetClientRect) originalClientRect=nullptr;
static HWND failClientRectWindow=nullptr;
static unsigned clientRectFailures=0;
static BOOL WINAPI FailEarlyLayout(HWND window,LPRECT rect){
    const BOOL result=originalClientRect(window,rect);
    if(window==failClientRectWindow&&failClientRectWindow){
        failClientRectWindow=nullptr;++clientRectFailures;
        layout_takeover_allocation_fault::armed=true;
    }
    return result;
}
static BOOL WINAPI ObserveEndDefer(HDWP batch){
    const BOOL result=originalEndDefer(batch);if(result&&layoutObserver)layoutObserver();return result;
}
static BOOL WINAPI HiddenPosition(HWND window,HWND after,int x,int y,int width,int height,UINT flags){
    DWORD process=0;GetWindowThreadProcessId(window,&process);
    if(process==GetCurrentProcessId()&&!GetParent(window))flags=(flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
    const BOOL result=originalPosition(window,after,x,y,width,height,flags);
    if(result&&layoutObserver)layoutObserver();return result;
}
static void InstallInputGuards(){
    wchar_t path[32768]{};Check(GetModuleFileNameW(nullptr,path,32768)>0,"resolve isolated takeover executable");
    const auto root=std::filesystem::path(path).parent_path();
    std::ifstream marker(root/L"layout-takeover-test-installation.txt",std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(marker)),{});
    Check(contents=="MMS layout takeover test installation v1\n","require dedicated takeover test installation before host startup");
    Hook(&GetCapture,&Capture);Hook(&SetCapture,&SetCaptureLocal);Hook(&ReleaseCapture,&ReleaseCaptureLocal);
    Hook(&GetFocus,&Focus);Hook(&SetFocus,&SetFocusLocal);Hook(&GetForegroundWindow,&Foreground);
    Hook(&SetForegroundWindow,&SetForegroundLocal);Hook(&SetCursor,&SetCursorLocal);Hook(&SetCursorPos,&Warp);
    Hook(&GetKeyState,&Key);Hook(&GetAsyncKeyState,&Key);
    Hook(&SetWindowPos,&HiddenPosition,&originalPosition);
    Hook(&EndDeferWindowPos,&ObserveEndDefer,&originalEndDefer);
    Hook(&GetClientRect,&FailEarlyLayout,&originalClientRect);
}
static void Settle(MMDApp* app){for(int i=0;i<3;++i){layout_refresh_host_checks::Pump();plugins::Refresh(app);}plugins::Layout(app);}
static HWND FindButton(HWND parent,const std::wstring& label){
    struct Search{const std::wstring* label;HWND found; } search{&label,nullptr};
    EnumChildWindows(parent,[](HWND window,LPARAM param)->BOOL{
        auto& search=*reinterpret_cast<Search*>(param);wchar_t text[128]{},cls[32]{};
        GetWindowTextW(window,text,128);GetClassNameW(window,cls,32);
        if(!_wcsicmp(cls,L"BUTTON")&&*search.label==text){search.found=window;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));return search.found;
}
struct Fixture {
    MMDApp* app;UiClient* owner;UiRegistry& registry;MmsUiApiV1 api;
    MmsUiLayoutSessionApiV1 sessions{};
    MmsHandle command=0;std::array<MmsHandle,4> buttons{};
    std::array<std::string,4> ids;std::array<HWND,4> windows{};
    Fixture(MMDApp* a,UiClient* client):app(a),owner(client),registry(*client->registry),api(registry.Api(*client)){
        const auto module=GetModuleHandleW(L"data-refresh-probe.dll");
        const auto negotiate=reinterpret_cast<MmsResult(MMS_CALL*)(MmsUiLayoutSessionApiV1*)>(GetProcAddress(module,"ProbeLayoutSessionApi"));
        const auto roundtrip=reinterpret_cast<MmsResult(MMS_CALL*)(uint32_t*)>(GetProcAddress(module,"ProbeLayoutSessionRoundtrip"));
        const auto sdkRoundtrip=reinterpret_cast<MmsResult(MMS_CALL*)(uint32_t*)>(GetProcAddress(module,"ProbeLayoutSessionSdkRoundtrip"));
        sessions.h=Header<MmsUiLayoutSessionApiV1>();
        Check(negotiate&&roundtrip&&negotiate(&sessions)==MMS_OK,"loaded DLL negotiates public layout session service");
        Check(sessions.h.size>=sizeof(sessions)&&sessions.h.major==1&&!sessions.h.minor&&sessions.context&&
            sessions.queryCapabilities&&sessions.queryTarget&&sessions.begin&&sessions.apply&&sessions.query&&sessions.queryClaim&&
            sessions.rollback&&sessions.commit&&sessions.release,"negotiated public layout session table is complete");
        MmsUiLayoutSessionCapsV1 caps{};caps.h=Header<MmsUiLayoutSessionCapsV1>();
        Check(sessions.queryCapabilities(sessions.context,&caps)==MMS_OK&&caps.fields==7&&caps.mounts==15&&caps.maxTargets&&
            caps.maxSessionsPerPlugin&&caps.maxSessionsGlobal&&!caps.reserved,"public capabilities expose supported fields and native mounts");
        uint32_t steps=0;Check(roundtrip(&steps)==MMS_OK&&steps==255,"DLL itself completes public begin apply query claim rollback and release");
        std::puts("PASS loaded DLL public layout session roundtrip steps=255");
        steps=0;Check(sdkRoundtrip&&sdkRoundtrip(&steps)==MMS_OK&&steps==127,
            "loaded DLL SDK negotiates begins queries applies and closes the real public session");
        std::puts("PASS loaded DLL SDK layout session roundtrip steps=127");
        const auto workspaceVersion=reinterpret_cast<MmsResult(MMS_CALL*)()>(GetProcAddress(module,"ProbeLayoutWorkspaceVersion"));
        Check(workspaceVersion&&workspaceVersion()==MMS_OK,"loaded DLL negotiates workspace 1.1 SDK and retains same 1.0 owner context");
        std::puts("PASS loaded DLL SDK workspace layout version=1.1");
        using ParentObserver=MmsResult(MMS_CALL*)(void*,uint32_t,MmsHandle,MmsHandle,MmsHandle);
        const auto setParentObserver=reinterpret_cast<void(MMS_CALL*)(ParentObserver,void*)>(GetProcAddress(module,"ProbeSetLayoutParentObserver"));
        const auto parentRoundtrip=reinterpret_cast<MmsResult(MMS_CALL*)(uint32_t*)>(GetProcAddress(module,"ProbeLayoutParentRoundtrip"));
        Check(setParentObserver&&parentRoundtrip,"loaded DLL exports real parent session driver and observer");
        struct ParentState{MMDApp* app;UiRegistry* registry;std::string owner;unsigned phase=0;HWND window=nullptr;
            MmsHandle element=0;RECT baseline{},moved{}; } parentState{app,&registry,owner->id};
        setParentObserver([](void* context,uint32_t phase,MmsHandle element,MmsHandle a,MmsHandle b)->MmsResult{
            auto& state=*static_cast<ParentState*>(context);
            Check(phase==state.phase++,"parent DLL observer visits every C history and SDK phase in order");
            Settle(state.app);
            const auto found=state.registry->elements.find(state.owner+".parent.button");
            const auto groupA=state.registry->elements.find(state.owner+".parent.group.a");
            const auto groupB=state.registry->elements.find(state.owner+".parent.group.b");
            Check(found!=state.registry->elements.end()&&groupA!=state.registry->elements.end()&&groupB!=state.registry->elements.end()&&
                found->second.handle==element&&groupA->second.handle==a&&groupB->second.handle==b,
                "DLL parent session uses registered identities in the actual Manager registry");
            const bool moved=phase==1||phase==3||phase==5||phase==7;
            const bool persistent=phase==3||phase==5;
            const auto effective=state.registry->EffectiveParent(found->second);
            Check(std::string(effective.data,size_t(effective.bytes))==(moved?groupB->first:groupA->first)&&
                std::string(found->second.desc.parentId.data,size_t(found->second.desc.parentId.bytes))==(persistent?groupB->first:groupA->first),
                "DLL Parent changes effective ancestry and only commit or redo changes the saved ancestry");
            const HWND window=FindButton(state.app->MainWindow(),L"Parent roundtrip button");RECT bounds{};
            Check(window&&GetWindowRect(window,&bounds)&&(GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE),
                "real NativeUi exposes the DLL parent button with valid visible bounds");
            if(phase==0){state.window=window;state.element=element;state.baseline=bounds;}
            Check(window==state.window&&element==state.element,"DLL parent movement preserves exact HWND and element handle");
            if(phase==1){state.moved=bounds;Check(!EqualRect(&state.baseline,&bounds),"DLL Parent preview really changes native geometry");}
            Check(EqualRect(moved?&state.moved:&state.baseline,&bounds),"C rollback commit history and SDK produce the expected native rectangle");
            Check(state.registry->GeometryReady(),"parent DLL observation uses a current published native layout");
            return MMS_OK;
        },&parentState);
        struct ClearParentObserver{decltype(setParentObserver) clear;~ClearParentObserver(){clear(nullptr,nullptr);}} clearParentObserver{setParentObserver};
        steps=0;Check(parentRoundtrip(&steps)==MMS_OK&&steps==31&&parentState.phase==9,
            "real DLL completes strict 1.2 negotiation C history roundtrip and SDK ownership on the same native object");
        setParentObserver(nullptr,nullptr);Settle(app);
        Check(!registry.elements.count(owner->id+".parent.button")&&!registry.elements.count(owner->id+".parent.group.a")&&
            !registry.elements.count(owner->id+".parent.group.b")&&!IsWindow(parentState.window),
            "DLL parent roundtrip explicitly removes all its temporary registered objects and native button");
        std::puts("PASS loaded DLL layout parent session version=1.2");
        const std::string commandId=owner->id+".takeover.command";
        MmsCommandDescV1 desc{};desc.info.h=Header<MmsCommandDescV1>();
        desc.info.id=Text(commandId);desc.info.ownerPlugin=Text(owner->id);desc.info.label=Text(commandId);
        desc.getState=[](void*,MmsObjectRef,MmsCommandStateV1* out)->MmsResult{*out={Header<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;};
        desc.invoke=[](void*,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{return MMS_OK;};
        Check(api.registerCommand(api.context,&desc,&command)==MMS_OK,"register trusted test command on actual plugin client");
        const char* parents[]={"mms.toolbar.main.extensions","mms.properties.main.extensions","mms.sidebar.left.extensions","mms.sidebar.right.extensions"};
        for(unsigned i=0;i<4;++i){
            ids[i]=owner->id+".takeover."+std::to_string(i);const auto label="Takeover "+std::to_string(i);
            MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=Text(ids[i]);e.ownerPlugin=Text(owner->id);
            e.parentId={parents[i],std::strlen(parents[i])};e.commandId=Text(commandId);e.label=Text(label);e.kind=MMS_UI_BUTTON;
            e.capabilities=MMS_UI_HIDE|MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_RESIZE;
            Check(api.registerElement(api.context,&e,&buttons[i])==MMS_OK,"register four real layout mounts");
            MmsUiLayoutSessionTargetInfoV1 target{};target.h=Header<MmsUiLayoutSessionTargetInfoV1>();
            Check(sessions.queryTarget(sessions.context,buttons[i],&target)==MMS_OK&&target.element==buttons[i]&&
                (target.fields&MMS_LAYOUT_SESSION_VISIBLE)&&target.mount==(1u<<i),"public target query identifies actual mounted element");
        }
        Settle(app);
        for(unsigned i=0;i<4;++i){windows[i]=FindButton(app->MainWindow(),L"Takeover "+std::to_wstring(i));
            Check(windows[i]&&(GetWindowLongPtrW(windows[i],GWL_STYLE)&WS_VISIBLE),"real NativeUi mounted visible test button");}
    }
    MmsResult BeginPreview(unsigned i,MmsHandle& h){MmsUiLayoutSessionTargetV1 target{};target.h=Header<MmsUiLayoutSessionTargetV1>();
        target.element=buttons[i];target.fields=MMS_LAYOUT_SESSION_VISIBLE;
        return sessions.begin(sessions.context,{&target,1,sizeof(target),1},&h);}
    MmsResult ApplyPreview(unsigned i,MmsHandle h,uint64_t revision){MmsUiLayoutSessionValueV1 value{};value.h=Header<MmsUiLayoutSessionValueV1>();
        value.element=buttons[i];value.fields=MMS_LAYOUT_SESSION_VISIBLE;
        return sessions.apply(sessions.context,h,revision,{&value,1,sizeof(value),1});}
    MmsUiLayoutSessionInfoV1 Query(MmsHandle h){MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
        Check(sessions.query(sessions.context,h,&info)==MMS_OK,"query original public session identity");return info;}
    void TerminalClaim(MmsHandle h,const MmsUiLayoutSessionInfoV1& info,MmsHandle element=0){
        Check(info.state!=MMS_LAYOUT_SESSION_ACTIVE&&info.targetCount==1&&!info.activeTargetCount,"terminal public session retains original target count");
        MmsUiLayoutSessionTargetV1 claim{};claim.h=Header<MmsUiLayoutSessionTargetV1>();
        Check(sessions.queryClaim(sessions.context,h,info.revision,0,&claim)==MMS_OK&&claim.element&&
            (!element||claim.element==element)&&!claim.fields&&!claim.reserved,"terminal public claim keeps identity with no remaining fields");}
    MmsHandle Preview(unsigned i){MmsHandle h=0;
        Check(BeginPreview(i,h)==MMS_OK&&h,"begin public session on actual loaded client");
        const auto info=Query(h);Check(info.state==MMS_LAYOUT_SESSION_ACTIVE,"new public session is active");
        Check(ApplyPreview(i,h,info.revision)==MMS_OK,"apply public session through host C ABI");return h;}
    void ClosePreview(MmsHandle h){auto info=Query(h);
        if(info.state==MMS_LAYOUT_SESSION_ACTIVE){Check(sessions.rollback(sessions.context,h,info.revision)==MMS_OK,"public rollback unaffected preview");info=Query(h);}
        TerminalClaim(h,info);
        Check(sessions.release(sessions.context,h)==MMS_OK,"release public test session");}
    void Unlocked(unsigned i){auto h=Preview(i);ClosePreview(h);Settle(app);}
    void Cleanup(){for(auto h:buttons)Check(api.unregisterItem(api.context,h)==MMS_OK,"unregister trusted test button before plugin Stop");
        Check(api.unregisterItem(api.context,command)==MMS_OK,"unregister trusted test command before plugin Stop");Settle(app);}
};
struct Geometry {
    RECT view{};std::array<RECT,4> buttons{};int top=0,bottom=0,left=0,right=0;
    explicit Geometry(Fixture& f){view=f.app->ViewportRect();top=plugins::ToolbarHeight(f.app);bottom=plugins::PropertyAreaHeight(f.app);
        left=plugins::LeftAreaWidth(f.app);right=plugins::RightAreaWidth(f.app);
        for(unsigned i=0;i<4;++i)Check(GetWindowRect(f.windows[i],&buttons[i]),"read actual mounted button rectangle");}
    void Equal(Fixture& f)const{const Geometry now(f);Check(EqualRect(&view,&now.view)&&top==now.top&&bottom==now.bottom&&left==now.left&&right==now.right,
        "takeover synchronously restores native reservations and viewport before movement");
        for(unsigned i=0;i<4;++i)Check(EqualRect(&buttons[i],&now.buttons[i])&&(GetWindowLongPtrW(f.windows[i],GWL_STYLE)&WS_VISIBLE),
            "takeover restores all mounted button rectangles and visibility");}
};
static void Point(Fixture& f,bool bottom,int& x,int& y){
    RECT client{};Check(GetClientRect(f.app->MainWindow(),&client),"read host client for native divider input");
    if(bottom){x=0;for(const auto& span:BottomPanelSpans(f.app))if(span.id==1)x=BottomPanelX(f.app,span.right);y=client.bottom-100;}
    else{x=f.app->SidebarWidth()+3;y=100;}
}
static void Begin(Fixture& f,bool bottom,int& x,int& y){
    Point(f,bottom,x,y);SendMessageW(f.app->MainWindow(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    Check(bottom?f.app->bottomPanelSizing.dragging==1:f.app->sidebarSizing.active,"actual divider press starts gesture");
}
static uint64_t Token(Fixture& f,bool bottom){return bottom?f.app->bottomPanelSizing.layoutToken:f.app->sidebarSizing.layoutToken;}
static void End(Fixture& f,UINT ending,int x,int y){
    if(ending==WM_CAPTURECHANGED)capture=nullptr;
    SendMessageW(f.app->MainWindow(),ending,ending==WM_KEYDOWN?VK_ESCAPE:0,MAKELPARAM(x,y));
    Check(!f.app->sidebarSizing.active&&f.app->bottomPanelSizing.dragging<0&&!capture,"actual ending clears gesture and capture");
}
// The observer calls the real USER32 positioning function first, then injects
// one nested message on its synchronous return. It does not fabricate native
// geometry, readiness, serials or tokens. This covers selected reentry points,
// not every possible Windows notification or physical capture ordering.
static Fixture* reentry=nullptr;
static bool reentryBottom=false;
static UINT reentryMessage=0;
static unsigned reentryCalls=0;
static void InterruptPreparing(){
    auto* app=reentry->app;
    if(!(reentryBottom?app->bottomPanelSizing.preparing:app->sidebarSizing.preparing))return;
    layoutObserver=nullptr;++reentryCalls;
    int x=0,y=0;Point(*reentry,reentryBottom,x,y);
    SendMessageW(app->MainWindow(),reentryMessage,0,MAKELPARAM(x,y));
}
static void FailedPressDuringRestore(){
    auto* app=reentry->app;
    if(app->WindowLayoutReady()||app->sidebarSizing.active||app->sidebarSizing.preparing||
        app->bottomPanelSizing.dragging>=0||app->bottomPanelSizing.preparing)return;
    layoutObserver=nullptr;++reentryCalls;
    int x=0,y=0;Point(*reentry,reentryBottom,x,y);
    SendMessageW(app->MainWindow(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    Check(!app->sidebarSizing.active&&!app->sidebarSizing.preparing&&app->bottomPanelSizing.dragging<0&&
        !app->bottomPanelSizing.preparing,"nested press during restoration cannot start in incomplete layout");
    // Leave capture as the nested input produced it. The outer cancellation
    // must release it; an extra synthetic cancel here would mask that defect.
}
static void StartAfterRelease(){
    releaseObserver=nullptr;++reentryCalls;
    int x=0,y=0;Begin(*reentry,false,x,y);
}
static void ReentrantChecks(Fixture& f){
    auto* app=f.app;reentry=&f;
    for(bool bottom:{false,true})for(UINT message:{WM_CANCELMODE,WM_LBUTTONUP}){
        // These mounts contain only the trusted test button. Hiding it removes
        // a real side/property reservation, so restoration must reposition UI.
        Settle(app);const Geometry baseline(f);auto preview=f.Preview(bottom?1:2);Settle(app);
        reentryBottom=bottom;reentryMessage=message;reentryCalls=0;layoutObserver=&InterruptPreparing;
        int x=0,y=0;Point(f,bottom,x,y);
        SendMessageW(app->MainWindow(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        Check(reentryCalls==1&&!layoutObserver,"prepare interruption runs at an actual synchronous layout point");
        Check(!app->sidebarSizing.active&&!app->sidebarSizing.preparing&&!app->SidebarResizeDragging()&&
            app->bottomPanelSizing.dragging<0&&!app->bottomPanelSizing.preparing&&!capture,
            "nested cancel or release invalidates prepare without continuing a stale gesture");
        baseline.Equal(f);f.ClosePreview(preview);for(unsigned i=0;i<4;++i)f.Unlocked(i);
    }
    for(bool nestedBottom:{false,true}){
        Settle(app);const int extra=app->bottomPanelSizing.extra[1];const Geometry baseline(f);
        int x=0,y=0;Begin(f,true,x,y);const auto retired=Token(f,true);
        SendMessageW(app->MainWindow(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+30,y));
        Check(app->bottomPanelSizing.extra[1]>extra,"reentry fixture first moves actual bottom controls");
        const auto oldSide=app->sidebarSizingSerial,oldBottom=app->bottomPanelSizingSerial;
        reentryBottom=nestedBottom;reentryCalls=0;layoutObserver=&FailedPressDuringRestore;
        SendMessageW(app->MainWindow(),WM_CANCELMODE,0,0);
        Check(reentryCalls==1&&!layoutObserver,"cancel restoration dispatches one nested native press");
        Check(nestedBottom?app->bottomPanelSizingSerial!=oldBottom:app->sidebarSizingSerial!=oldSide,
            "nested failed press reaches actual gesture attempt without mutating serials in test");
        Check(!capture&&!app->sidebarSizing.active&&!app->sidebarSizing.preparing&&app->bottomPanelSizing.dragging<0&&
            !app->bottomPanelSizing.preparing&&app->WindowLayoutReady()&&!app->bottomPanelSizing.relayout&&
            app->bottomPanelSizing.extra[1]==extra&&!f.registry.IsHostLayoutCurrent(2,retired),
            "outer cancellation releases capture and layout barrier after nested failed attempt");
        baseline.Equal(f);for(unsigned i=0;i<4;++i)f.Unlocked(i);
    }
    // Input isolation simulates ReleaseCapture's synchronous loss notification.
    // A fresh real press is dispatched before the old bottom-up handler resumes.
    int x=0,y=0;Begin(f,true,x,y);const auto retired=Token(f,true);
    reentryCalls=0;releaseObserver=&StartAfterRelease;
    SendMessageW(app->MainWindow(),WM_LBUTTONUP,0,MAKELPARAM(x+10,y));
    const auto active=Token(f,false);
    Check(reentryCalls==1&&!releaseObserver&&app->sidebarSizing.active&&active&&active!=retired&&
        capture==app->MainWindow()&&f.registry.IsHostLayoutCurrent(15,active),
        "old bottom release cannot clear new sidebar gesture token or capture");
    MmsHandle untouched=993;Check(f.BeginPreview(0,untouched)==MMS_E_BUSY&&untouched==993,
        "new nested gesture retains its real registry barrier after old cleanup");
    End(f,WM_CANCELMODE,app->SidebarWidth()+3,100);for(unsigned i=0;i<4;++i)f.Unlocked(i);
    reentry=nullptr;
    std::puts("PASS actual layout takeover reentry prepare/restore/release");
}
static void ExplicitLayoutFailureChecks(Fixture& f){
    Settle(f.app);const Geometry baseline(f);
    auto preview=f.Preview(2);Settle(f.app);
    uint64_t token=0;
    Check(f.registry.BeginHostLayout(HostLayoutAll,token)==MMS_OK,"acquire real host barrier before restoring preview");
    plugins::Refresh(f.app);
    Check(f.registry.GeometryReady(),"actual NativeUi refresh publishes restored geometry before explicit host pass");
    Check(f.registry.WithReadOnlyCallback([&]{plugins::Layout(f.app);return MMS_OK;})==MMS_OK&&f.registry.GeometryReady(),
        "rejected callback layout leaves completed geometry intact");
    std::thread wrongThread([&]{plugins::Layout(f.app);});wrongThread.join();
    Check(f.registry.GeometryReady(),"rejected wrong-thread layout leaves completed geometry intact");
    // The real GetClientRect succeeds and its hook returns normally. It arms
    // the next actual C++ allocation during explicit layout preparation;
    // throwing across an extern-C USER32 call would violate /EHsc assumptions.
    const auto before=clientRectFailures,beforeAllocations=layout_takeover_allocation_fault::failures;
    failClientRectWindow=f.app->MainWindow();
    plugins::Layout(f.app);
    const bool faultLeftArmed=layout_takeover_allocation_fault::armed;
    layout_takeover_allocation_fault::armed=false;failClientRectWindow=nullptr;
    Check(!faultLeftArmed&&clientRectFailures==before+1&&layout_takeover_allocation_fault::failures==beforeAllocations+1,
        "explicit host pass consumes exactly one real C++ allocation failure after successful GetClientRect");
    Check(!f.registry.GeometryReady(),"failed explicit host pass cannot retain the preceding refresh snapshot as ready");
    Check(f.registry.IsHostLayoutCurrent(HostLayoutAll,token),"failed layout does not silently release the caller's barrier");
    plugins::EndLayoutInteraction(f.app,token);
    Check(!f.registry.IsHostLayoutCurrent(HostLayoutAll,token),"caller can release barrier after failed explicit pass");
    Settle(f.app);Check(f.registry.GeometryReady(),"normal host refresh recovers after early layout failure");
    baseline.Equal(f);f.ClosePreview(preview);for(unsigned i=0;i<4;++i)f.Unlocked(i);
    std::puts("PASS actual layout refresh followed by explicit-entry failure and recovery");
}
static void Run(MMDApp* app,bool bound){
    if(bound){plugins::Shutdown();Check(plugins::InitializeHistoryTestRuntime(app),"bind actual shared history for takeover");
        Check(plugins::NativeHistory(app)->SetMode(SharedHistory::RecordingMode::AllOperations)==MMS_OK,"all-operation history mode");}
    Check(bool(plugins::BoundNativePoseHistory(app))==bound,"correct real history binding");
    auto module=GetModuleHandleW(L"data-refresh-probe.dll");
    auto context=module?reinterpret_cast<void*(MMS_CALL*)()>(GetProcAddress(module,"ProbeUiContext")):nullptr;
    auto* owner=context?static_cast<UiClient*>(context()):nullptr;
    Check(owner&&owner->registry&&owner->id=="org.example.layout-takeover","obtain actual Manager client from private fixture export");
    SetWindowPos(app->MainWindow(),nullptr,0,0,1500,950,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    app->CameraMode()=1;for(int i=1;i<7;++i)app->UiOptionFlag(i)=0;app->UiOptionFlag(1)=1;
    HandleWindowSize(app);Fixture f(app,owner);
    auto* history=plugins::NativeHistory(app);Check(history!=nullptr,"real host owns common journal");
    const auto undo=history->UndoCount(),redo=history->RedoCount();const auto retained=history->RetainedBytes();
    uint64_t revision=0;Check(history->Revision(revision)==MMS_OK,"read initial common history revision");
    const auto settings=layout_refresh_host_checks::Settings();const auto preferences=f.registry.ExportPreferences();
    const int frame=app->CurrentFrame(),dirty=app->SceneModified();
    for(bool bottom:{false,true})for(UINT ending:{WM_LBUTTONUP,WM_CANCELMODE,WM_CAPTURECHANGED,WM_KEYDOWN}){
        Settle(app);const Geometry baseline(f);const int width=app->SidebarWidth(),extra=app->bottomPanelSizing.extra[1];const float ratio=app->SidebarRatio();
        int effectiveExtra=0;for(const auto& span:BottomPanelSpans(app))if(span.id==1)effectiveExtra=span.extra;
        std::array<MmsHandle,4> previews{};for(unsigned i=0;i<4;++i)if(!bottom||i==1)previews[i]=f.Preview(i);
        Settle(app);for(unsigned i=0;i<4;++i)if(previews[i])Check(!(GetWindowLongPtrW(f.windows[i],GWL_STYLE)&WS_VISIBLE),"preview actually hides real native child");
        int x=0,y=0;Begin(f,bottom,x,y);const auto token=Token(f,bottom);const uint32_t mask=bottom?2:15;
        Check(token&&f.registry.IsHostLayoutCurrent(mask,token),"actual input owns registry layout barrier");baseline.Equal(f);
        Check(bottom?(app->bottomPanelSizing.startExtra==extra&&app->bottomPanelSizing.startX==x+extra-effectiveExtra):
            (app->sidebarSizing.startWidth==width&&app->sidebarSizing.startRatio==ratio),
            "native gesture snapshot uses restored starting geometry");
        for(unsigned i=0;i<4;++i){
            if(previews[i]){const auto info=f.Query(previews[i]);
                Check(info.state==MMS_LAYOUT_SESSION_REVOKED&&info.reasons==MMS_LAYOUT_SESSION_HOST_INTERACTION,"public query identifies native interaction as revocation reason");
                f.TerminalClaim(previews[i],info,f.buttons[i]);
                Check(f.ApplyPreview(i,previews[i],info.revision)==MMS_E_NOT_READY,"revoked public session cannot reclaim native drag");
                MmsHandle untouched=991;Check(f.BeginPreview(i,untouched)==MMS_E_BUSY&&untouched==991,"fresh public conflicting session is busy without output mutation");}
            else f.Unlocked(i);
        }
        SendMessageW(app->MainWindow(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+20,y));
        Check(bottom?app->bottomPanelSizing.extra[1]>extra:app->SidebarWidth()==x+20,"actual divider moves native geometry");
        End(f,ending,x+25,y);
        Check(!f.registry.IsHostLayoutCurrent(mask,token),"all native ending paths unlock their token");
        if(ending==WM_LBUTTONUP)Check(bottom?app->bottomPanelSizing.extra[1]>extra:app->SidebarWidth()==x+25,"release retains final pointer layout");
        else Check(bottom?app->bottomPanelSizing.extra[1]==extra:app->SidebarWidth()==width&&app->SidebarRatio()==ratio,"cancel capture-loss and Escape restore original layout");
        for(unsigned i=0;i<4;++i){if(previews[i])f.ClosePreview(previews[i]);f.Unlocked(i);}
        Begin(f,bottom,x,y);const auto newer=Token(f,bottom);Check(newer&&newer!=token,"next actual gesture has distinct token");
        plugins::EndLayoutInteraction(app,token);Check(f.registry.IsHostLayoutCurrent(mask,newer),"old completion cannot clear new native gesture");
        MmsHandle untouched=992;Check(f.BeginPreview(bottom?1:0,untouched)==MMS_E_BUSY&&untouched==992,"new gesture stays locked after stale end");
        End(f,WM_CANCELMODE,x,y);
    }
    ReentrantChecks(f);
    ExplicitLayoutFailureChecks(f);
    uint64_t finalRevision=0;Check(history->Revision(finalRevision)==MMS_OK&&revision==finalRevision&&undo==history->UndoCount()&&redo==history->RedoCount()&&retained==history->RetainedBytes(),"preview and native layout leave common history unchanged");
    Check(frame==app->CurrentFrame()&&dirty==app->SceneModified()&&preferences==f.registry.ExportPreferences()&&settings==layout_refresh_host_checks::Settings(),"takeover preserves playhead project dirty preferences and settings bytes");
    Check(!IsWindowVisible(app->MainWindow())&&!warps,"host remains hidden without moving user cursor");
    f.Cleanup();std::printf("PASS actual layout takeover mode=%s\n",bound?"bound":"ordinary");
}
}
