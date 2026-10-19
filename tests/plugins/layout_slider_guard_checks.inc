// Include after layout_slider_repaint_checks.inc in the disposable full host.
// These synchronous guard tests use the actual FOV/vertical native HWNDs. They
// do not replace GWLP_WNDPROC, drive physical input, or prove visible no-flicker.
#include "../../src/window/light_slider_paint_buffer.hpp"
#include "mikumikustudio/build_profile.hpp"
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#if !MMD_ORIGINAL_PROFILE
namespace layout_slider_guard_checks {
namespace buffer=mikumikustudio::ui::light_slider_paint_buffer;
namespace trace=slider_paint_trace;
using mikumikustudio::MMDApp;
enum class Fault {None,NoLayout,VisibleCue,PrepareFlags,NativeFlags,NativeMove,NativeReparent,QueryFlags,QueryMove,QueryCapture};
static const char* Name(Fault value)noexcept{switch(value){
    case Fault::NoLayout:return "main-capture-without-layout";case Fault::VisibleCue:return "visible-keyboard-cue";
    case Fault::PrepareFlags:return "prepare-retire-flags";case Fault::NativeFlags:return "native-retire-flags";
    case Fault::NativeMove:return "native-same-size-move";case Fault::NativeReparent:return "native-reparent";
    case Fault::QueryFlags:return "next-query-retire-flags";case Fault::QueryMove:return "next-query-same-size-move";
    case Fault::QueryCapture:return "next-query-capture-transfer";default:return "layout-success";}}
static bool Dragging(void* context)noexcept{
    auto& app=*static_cast<MMDApp*>(context);return app.SidebarResizeDragging()!=0||app.bottomPanelSizing.dragging>=0;
}
static void Ui(HWND window,UINT wanted)noexcept{
    const UINT current=UINT(SendMessageW(window,WM_QUERYUISTATE,0,0));
    const UINT set=wanted&~current,clear=current&~wanted;
    if(set)SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,set),0);
    if(clear)SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,clear),0);
}
struct Target {
    HWND window=nullptr,parent=nullptr,previous=nullptr;RECT rectangle{};LONG_PTR style=0;UINT ui=0;LRESULT tick=0;bool visible=false;
    explicit Target(HWND value):window(value),parent(GetParent(value)),previous(GetWindow(value,GW_HWNDPREV)),style(GetWindowLongPtrW(value,GWL_STYLE)),
        ui(UINT(SendMessageW(value,WM_QUERYUISTATE,0,0))),tick(SendMessageW(value,TBM_GETPOS,0,0)),visible(IsWindowVisible(value)!=FALSE){
        trace::Require(window&&parent&&GetWindowRect(window,&rectangle),"capture real layout-guard HWND geometry");
        MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rectangle),2);
    }
    void Restore()const noexcept{
        if(!IsWindow(window)||!IsWindow(parent))return;
        if(GetParent(window)!=parent)SetParent(window,parent);
        EnableWindow(window,(style&WS_DISABLED)==0);Ui(window,ui);ShowWindow(window,visible?SW_SHOWNA:SW_HIDE);
        const HWND after=previous&&IsWindow(previous)&&GetParent(previous)==parent?previous:HWND_TOP;
        SetWindowPos(window,after,rectangle.left,rectangle.top,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
            SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);
    }
    void Check()const{
        RECT actual{};trace::Require(GetParent(window)==parent&&GetWindowRect(window,&actual),"layout guard restores actual parent and live HWND");
        MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&actual),2);
        trace::Require(EqualRect(&actual,&rectangle)&&GetWindowLongPtrW(window,GWL_STYLE)==style&&
            GetWindow(window,GW_HWNDPREV)==previous&&UINT(SendMessageW(window,WM_QUERYUISTATE,0,0))==ui&&
            SendMessageW(window,TBM_GETPOS,0,0)==tick&&bool(IsWindowVisible(window))==visible,
            "layout guard restores geometry, z-order, complete style, visibility, UI bits and tick");
    }
};
struct AuxiliaryWindow {
    HWND value=nullptr;
    explicit AuxiliaryWindow(MMDApp& app){value=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,4,4,256,96,app.MainWindow(),nullptr,GetModuleHandleW(nullptr),nullptr);
        trace::Require(value!=nullptr,"create test-only native parent/capture target without a scene object");}
    ~AuxiliaryWindow(){if(value)DestroyWindow(value);}
};
struct Session {
    MMDApp& app;AuxiliaryWindow ownedAuxiliary;HWND auxiliary=nullptr;ULONGLONG started=GetTickCount64();
    const unsigned char sidebar;const decltype(MMDApp::bottomPanelSizing) bottom;
    std::array<Target,2> targets;
    explicit Session(MMDApp& value):app(value),ownedAuxiliary(value),auxiliary(ownedAuxiliary.value),sidebar(value.SidebarResizeDragging()),bottom(value.bottomPanelSizing),
        targets{Target(GetDlgItem(value.MainWindow(),447)),Target(GetDlgItem(value.MainWindow(),534))}{}
    void Clear()noexcept{
        app.SidebarResizeDragging()=0;app.bottomPanelSizing.dragging=-1;
        const auto capture=GetCapture();if(capture==app.MainWindow()||capture==auxiliary)ReleaseCapture();
    }
    void Guard()const{
        trace::Require(GetTickCount64()-started<20000,"INCOMPLETE layout guard test exceeded its 20-second budget");
        trace::Require(!GetCapture(),"INCOMPLETE layout guard never takes another window's capture");
        BYTE keys[256]{};trace::Require(GetKeyboardState(keys)!=FALSE,"read UI-thread key state for isolated layout guard");
        for(int key=1;key<256;++key)trace::Require(!(keys[key]&0x80)&&!(GetAsyncKeyState(key)&0x8000),
            "INCOMPLETE held real key/button interrupts synthetic capture guard test");
    }
    ~Session(){Clear();for(const auto& target:targets)target.Restore();app.SidebarResizeDragging()=sidebar;app.bottomPanelSizing=bottom;}
};
struct Probe {
    inline static thread_local Probe* current=nullptr;
    MMDApp& app;HWND target,auxiliary;WNDPROC native;Fault fault;
    unsigned paints=0,supplied=0,begins=0,ends=0,copies=0,queries=0;
    bool armed=false,injected=false,actionOk=true,mainBefore=false,mainAfter=false,queryHidden=false;
    LRESULT nativeResult=0;DWORD nativeError=0;
    Probe(MMDApp& value,HWND window,HWND other,Fault failure):app(value),target(window),auxiliary(other),native(value.OriginalTrackbarProc()),fault(failure){
        trace::Require(native&&!current,"one actual native layout guard probe");current=this;app.OriginalTrackbarProc()=Query;
    }
    ~Probe(){app.OriginalTrackbarProc()=native;current=nullptr;}
    void Action()noexcept{
        if(injected)return;injected=true;mainBefore=GetCapture()==app.MainWindow();
        if(fault==Fault::PrepareFlags||fault==Fault::NativeFlags||fault==Fault::QueryFlags){
            app.SidebarResizeDragging()=0;app.bottomPanelSizing.dragging=-1;
        }else if(fault==Fault::NativeMove||fault==Fault::QueryMove){
            RECT before{};actionOk=GetWindowRect(target,&before)!=FALSE;const HWND parent=GetParent(target);
            if(actionOk){MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&before),2);
                actionOk=SetWindowPos(target,nullptr,before.left+2,before.top,before.right-before.left,before.bottom-before.top,
                    SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS)!=FALSE;}
        }else if(fault==Fault::NativeReparent){SetLastError(ERROR_SUCCESS);const HWND before=GetParent(target);
            const HWND returned=SetParent(target,auxiliary);actionOk=returned==before&&GetParent(target)==auxiliary;
        }else if(fault==Fault::QueryCapture){SetCapture(auxiliary);actionOk=GetCapture()==auxiliary;}
        mainAfter=GetCapture()==app.MainWindow();
    }
    static LRESULT CALLBACK Query(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto& self=*current;const auto result=CallWindowProcA(self.native,window,message,wp,lp);const DWORD error=GetLastError();
        if(window==self.target&&message==WM_QUERYUISTATE){++self.queries;
            if(self.armed){self.armed=false;self.queryHidden=(result&UISF_HIDEFOCUS)!=0;self.Action();}}
        SetLastError(error);return result;
    }
    static LRESULT CALLBACK Native(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto& self=*current;
        if(message==WM_PAINT){++self.paints;self.supplied+=wp&&GetObjectType(reinterpret_cast<HDC>(wp))==OBJ_MEMDC;}
        const auto result=CallWindowProcA(self.native,window,message,wp,lp);const DWORD error=GetLastError();
        if(window==self.target&&message==WM_PAINT){self.nativeResult=result;self.nativeError=error;
            if(wp&&!self.injected){
                if(self.fault==Fault::NativeFlags||self.fault==Fault::NativeMove||self.fault==Fault::NativeReparent)self.Action();
                else if(self.fault==Fault::QueryFlags||self.fault==Fault::QueryMove||self.fault==Fault::QueryCapture)self.armed=true;
            }}
        SetLastError(error);return result;
    }
    static HDC WINAPI Memory(HDC reference){auto& self=*current;const HDC result=CreateCompatibleDC(reference);
        if(result&&self.fault==Fault::PrepareFlags)self.Action();return result;}
    static HDC WINAPI Begin(HWND window,LPPAINTSTRUCT paint){++current->begins;return BeginPaint(window,paint);}
    static BOOL WINAPI End(HWND window,const PAINTSTRUCT* paint){++current->ends;return EndPaint(window,paint);}
    static BOOL WINAPI Copy(HDC to,int x,int y,int width,int height,HDC from,int sx,int sy,DWORD operation){
        ++current->copies;return BitBlt(to,x,y,width,height,from,sx,sy,operation);
    }
    buffer::GdiFunctions Functions(){buffer::GdiFunctions api;api.createCompatibleDC=Memory;api.beginPaint=Begin;api.endPaint=End;api.bitBlt=Copy;return api;}
};
struct Cleanup {Session& session;const Target& target;~Cleanup(){session.Clear();target.Restore();}};
static void Case(Session& session,const Target& target,Fault fault,std::ofstream& report){
    auto& app=session.app;session.Guard();Cleanup cleanup{session,target};
    ShowWindow(target.window,SW_SHOWNA);Ui(target.window,(target.ui|UISF_HIDEFOCUS)&(fault==Fault::VisibleCue?~UINT(UISF_HIDEFOCUS):~0u));
    app.SidebarResizeDragging()=fault==Fault::NoLayout?0:1;app.bottomPanelSizing.dragging=-1;
    SetCapture(app.MainWindow());trace::Require(GetCapture()==app.MainWindow(),"synthetic main-window capture established for exact layout guard scope");
    Probe probe(app,target.window,session.auxiliary,fault);const auto api=probe.Functions();buffer::ScopedFunctionsForTesting functions(api);
    const buffer::LayoutContext context{app.MainWindow(),&app,Dragging};
    trace::Require(InvalidateRect(target.window,nullptr,TRUE)!=FALSE,"prepare actual native update region for layout guard");
    const auto result=buffer::Forward(Probe::Native,target.window,WM_PAINT,0,0,context);
    const bool direct=fault==Fault::NoLayout||fault==Fault::VisibleCue||fault==Fault::PrepareFlags;
    const bool rejected=!direct&&fault!=Fault::None;
    report<<"CASE id="<<GetDlgCtrlID(target.window)<<" fault="<<Name(fault)<<" native="<<probe.paints<<" supplied="<<probe.supplied
        <<" begins="<<probe.begins<<" ends="<<probe.ends<<" copies="<<probe.copies<<" injected="<<probe.injected<<" action_ok="<<probe.actionOk
        <<" main_before="<<probe.mainBefore<<" main_after="<<probe.mainAfter<<" next_query_hidden="<<probe.queryHidden<<'\n';report.flush();
    trace::Require(probe.paints==1&&probe.supplied==unsigned(!direct)&&result==probe.nativeResult,
        "each explicit layout guard paint invokes real native once with the required HDC and matching result");
    trace::Require(probe.begins==unsigned(!direct)&&probe.ends==unsigned(!direct)&&probe.copies==unsigned(fault==Fault::None),
        "retired/moved/reparented/capture-transferred layout surface is never copied and Begin/End remain paired");
    if(fault!=Fault::None&&fault!=Fault::NoLayout&&fault!=Fault::VisibleCue){
        trace::Require(probe.injected&&probe.actionOk&&probe.mainBefore,"the requested synchronous mutation actually occurred under main capture");
        trace::Require(fault==Fault::QueryCapture?!probe.mainAfter:probe.mainAfter,
            "flags/move/reparent cases retain main capture; only capture-transfer case changes its owner");
        if(fault==Fault::QueryFlags||fault==Fault::QueryMove||fault==Fault::QueryCapture)
            trace::Require(probe.queryHidden&&!probe.armed,"mutation happened in the first post-native QUERY while native still returned HIDEFOCUS");
    }
    if(rejected){
        trace::Require(GetPropW(target.window,buffer::detail::kDirectOnce)==buffer::detail::DirectMarker(),
            "rejected prepared layout surface schedules the existing one-shot native retry");
        const auto paints=probe.paints,begins=probe.begins,ends=probe.ends,copies=probe.copies,supplied=probe.supplied;
        trace::Require(InvalidateRect(target.window,nullptr,TRUE)!=FALSE,"retain a real update for deferred native retry");
        buffer::Forward(Probe::Native,target.window,WM_PAINT,0,0,context);
        trace::Require(probe.paints==paints+1&&probe.supplied==supplied&&probe.begins==begins&&probe.ends==ends&&probe.copies==copies&&
            !GetPropW(target.window,buffer::detail::kDirectOnce),"deferred retry calls native directly once without replay or another buffer transaction");
    }
    session.Clear();target.Restore();target.Check();
}
static trace::Snapshot NativeImage(MMDApp& app,HWND window,const trace::Snapshot& actual,std::uint32_t marker){
    const HDC display=GetDC(window);trace::Require(display!=nullptr,"read reference format for real hidden-model slider");
    struct Release {HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}}release{window,display};
    trace::Dib image(actual.width,actual.height,display);trace::Require(bool(image),"allocate bounded native hidden-model slider image");
    std::fill_n(image.pixels,std::size_t(actual.width)*actual.height,marker);
    CallWindowProcA(app.OriginalTrackbarProc(),window,WM_PAINT,reinterpret_cast<WPARAM>(image.dc),0);
    trace::Require(GdiFlush()!=FALSE,"complete real native supplied-HDC hidden-model drawing");
    auto result=actual;result.borrowedDc=true;result.pixels.assign(image.pixels,image.pixels+std::size_t(actual.width)*actual.height);return result;
}
static void HiddenModelSurfaces(Session& session,std::ofstream& report,const std::filesystem::path& output){
    auto& app=session.app;
    for(const int id:{505,510,515,520}){
        session.Guard();const Target target(GetDlgItem(app.MainWindow(),id));Cleanup cleanup{session,target};
        const auto viewport=app.ViewportRect();
        trace::Require(viewport.right-viewport.left>220&&viewport.bottom-viewport.top>80,
            "INCOMPLETE isolated hidden-model pixel comparison needs an empty viewport area");
        trace::Require(SetWindowPos(target.window,HWND_TOP,viewport.left+20,viewport.top+20,173,25,
            SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS)!=FALSE,"temporarily place actual hidden-model HWND in a visible isolated test area");
        ShowWindow(target.window,SW_SHOWNA);Ui(target.window,target.ui|UISF_HIDEFOCUS);
        report<<"MODEL_TARGET id="<<id<<" originally_visible="<<target.visible<<" forced_visible=1 temporary_geometry=173x25 "
            "same_parent_and_identity=1 no_model_mode_switch=1\n";
        for(const bool disabled:{false,true}){
            session.Guard();EnableWindow(target.window,!disabled);
            trace::Require(bool(IsWindowEnabled(target.window))==!disabled&&
                bool(GetWindowLongPtrW(target.window,GWL_STYLE)&WS_DISABLED)==disabled,
                "hidden-model comparison actually enters the requested enabled/disabled native state");
            app.SidebarResizeDragging()=1;app.bottomPanelSizing.dragging=-1;
            SetCapture(app.MainWindow());trace::Require(GetCapture()==app.MainWindow(),"hold only own main capture for hidden-model native paint comparison");
            {
                Probe probe(app,target.window,session.auxiliary,Fault::None);const auto api=probe.Functions();buffer::ScopedFunctionsForTesting functions(api);
                const buffer::LayoutContext context{app.MainWindow(),&app,Dragging};
                trace::Require(InvalidateRect(target.window,nullptr,TRUE)!=FALSE,"invalidate dynamic hidden-model native client");
                const auto result=buffer::Forward(Probe::Native,target.window,WM_PAINT,0,0,context);
                trace::Require(probe.paints==1&&probe.supplied==1&&probe.begins==1&&probe.ends==1&&probe.copies==1&&result==probe.nativeResult,
                    "real dynamic hidden-model HWND uses one complete buffered native paint even when disabled");
            }
            const auto actual=layout_slider_repaint_checks::Pixels(target.window);
            trace::Require(actual.width==173&&actual.height==25,"hidden-model comparison uses the requested nondefault dynamic client size");
            const auto first=NativeImage(app,target.window,actual,0x0013d55f),second=NativeImage(app,target.window,actual,0x00e82ac4);
            const auto after=layout_slider_repaint_checks::Pixels(target.window);
            const auto marker=layout_slider_repaint_checks::PixelDifference(first,second);
            const auto native=layout_slider_repaint_checks::PixelDifference(actual,first);
            const auto nativeSecond=layout_slider_repaint_checks::PixelDifference(actual,second);
            const auto unchanged=layout_slider_repaint_checks::PixelDifference(actual,after);
            report<<"MODEL_SURFACE id="<<id<<" disabled="<<disabled<<" width="<<actual.width<<" height="<<actual.height
                <<" marker_dependency="<<marker<<" actual_vs_native_a="<<native<<" actual_vs_native_b="<<nativeSecond
                <<" actual_changed_by_supplied_paint="<<unchanged<<'\n';report.flush();
            if(marker||native||nativeSecond||unchanged){const auto prefix="model-"+std::to_string(id)+(disabled?"-disabled":"-normal");
                layout_slider_repaint_checks::Bitmap(output/(prefix+"-actual.bmp"),actual);
                layout_slider_repaint_checks::Bitmap(output/(prefix+"-native-a.bmp"),first);
                layout_slider_repaint_checks::Bitmap(output/(prefix+"-native-b.bmp"),second);}
            trace::Require(!marker&&!native&&!nativeSecond&&!unchanged,
                "hidden-model native drawing covers both markers, matches actual buffered RGB exactly, and does not write the actual HWND when supplied an HDC");
            session.Clear();
        }
        target.Restore();target.Check();
    }
}
inline void Run(MMDApp& app,const std::filesystem::path& output){
    std::filesystem::create_directories(output);std::ofstream report(output/"layout-slider-guards.txt");
    trace::Require(bool(report),"open isolated layout guard report");
    report<<"Actual FOV447 and vertical534 HWNDs; test-scoped visibility, synthetic capture/flags and synchronous API hooks. "
        "No GWLP_WNDPROC or ANSI/Unicode chain replacement. Next-query hook temporarily substitutes the existing original-procedure pointer and forwards native once. "
        "Guard cases make no visual claim; separate MODEL_SURFACE groups compare final window/native RGB under forced visibility. "
        "Neither is physical-input, whole-layout, model-workspace or DWM/no-flicker acceptance. Querying async key state may consume its low-bit history; no held/toggle state is edited.\n";
    trace::Require(!app.SidebarResizeDragging()&&app.bottomPanelSizing.dragging<0&&!app.sidebarSizing.active&&!GetCapture(),
        "INCOMPLETE layout guards require an idle isolated host without an existing gesture");
    layout_slider_repaint_checks::Invariant invariant(app);
    Session session(app);unsigned cases=0;
    for(const auto& target:session.targets){
        trace::Require(GetDlgCtrlID(target.window)!=534||(GetWindowLongPtrW(target.window,GWL_STYLE)&TBS_VERT),"534 remains the actual vertical native trackbar");
        report<<"TARGET id="<<GetDlgCtrlID(target.window)<<" originally_visible="<<target.visible<<" test_shows_for_each_case=1\n";
        for(const auto fault:{Fault::None,Fault::NoLayout,Fault::VisibleCue,Fault::PrepareFlags,Fault::NativeFlags,Fault::NativeMove,
            Fault::NativeReparent,Fault::QueryFlags,Fault::QueryMove,Fault::QueryCapture}){
            Case(session,target,fault,report);invariant.Check();++cases;
        }
    }
    HiddenModelSurfaces(session,report,output);invariant.Check();
    report<<"PASS cases="<<cases<<" hidden_model_surface_groups=8 exact_native_once=1 guard_rechecked=1 old_frame_rejected=1 native_retry=1 checked_history_and_VMD_preserved=1\n";
    trace::Require(bool(report),"finish isolated layout guard report");
    std::printf("PASS LAYOUT_SLIDER_GUARDS cases=%u actual_ids=447,534 next_query_reentry=1\n",cases);
}
} // namespace layout_slider_guard_checks
#endif
