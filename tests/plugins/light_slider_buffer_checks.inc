// Include after the light paint/surface diagnostics in the disposable full host.
#include "../../src/window/light_slider_paint_buffer.hpp"
#include "mikudancestudio/build_profile.hpp"
#include <fstream>
#if !MMD_ORIGINAL_PROFILE
namespace light_slider_buffer_checks {
namespace trace=slider_paint_trace;
namespace prior=slider_paint_trace_host_checks;
namespace surface=light_slider_native_surface_checks;
namespace observed=light_slider_paint_checks;
namespace buffer=mikudancestudio::ui::light_slider_paint_buffer;
using mikudancestudio::MMDApp;
enum class Failure {None,FirstRegion,UpdateRegion,FullRegion,ReferenceDC,MemoryDC,Bitmap,Select,Invalidate,Begin,Transfer,End,CaptureLoss,Resize,RetryProperty};
static const char* Name(Failure failure){switch(failure){
    case Failure::FirstRegion:return "first-region";case Failure::UpdateRegion:return "update-region";
    case Failure::FullRegion:return "full-region";case Failure::ReferenceDC:return "reference-dc";
    case Failure::MemoryDC:return "memory-dc";case Failure::Bitmap:return "bitmap";case Failure::Select:return "select";
    case Failure::Invalidate:return "invalidate-full";case Failure::Begin:return "begin";case Failure::Transfer:return "transfer";
    case Failure::End:return "end";case Failure::CaptureLoss:return "native-sync-capture-loss";case Failure::Resize:return "native-sync-resize";
    case Failure::RetryProperty:return "retry-property";default:return "none";}}
struct Spy {
    struct Dispatch {unsigned id=0,parent=0,depth=0,nativeCalls=0,nativeDepth=0;WPARAM inputWp=0,nativeWp=0;
        LRESULT nativeResult=0,result=0;bool returned=false;};
    struct Event {unsigned dispatch=0,depth=0;WPARAM wp=0;LRESULT result=0;bool returned=false;};
    MMDApp& app;WNDPROC native=nullptr;HWND window=nullptr;Failure failure=Failure::None;
    unsigned paints=0,supplied=0,begins=0,ends=0,copies=0,regionCalls=0,invalidates=0;
    unsigned rootPaints=0,rootSupplied=0,nativeDepth=0,dispatchDepth=0,activeDispatch=0,dispatchCount=0,eventCount=0;
    unsigned erases=0,beginErases=0,memoryErases=0;DWORD eraseDcType=0;bool beginInProgress=false;
    std::array<Dispatch,64> dispatches{};std::array<Event,128> events{};std::array<HWND,7> watches{};
    unsigned regions=0,deletedRegions=0,references=0,released=0,memories=0,deletedDCs=0,bitmaps=0,deletedBitmaps=0;
    bool injected=false,observerFailed=false,copyReturned=false,traceOverflow=false,unmappedNative=false;
    WPARAM nativeWp=0;LRESULT nativeResult=0;
    trace::Snapshot entry,beforeCopy,rendered,afterCopy,beforeErase,afterErase;
    inline static thread_local Spy* current=nullptr;
    explicit Spy(MMDApp& value):app(value),native(value.OriginalTrackbarProc()){
        trace::Require(native&&!current,"one native lighting paint spy");current=this;app.OriginalTrackbarProc()=Forward;
        unsigned installed=0;
        for(int id:prior::kIds){const auto control=GetDlgItem(app.MainWindow(),id);
            if(!control||!SetWindowSubclass(control,DispatchPaint,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this))){
                for(unsigned i=0;i<installed;++i)RemoveWindowSubclass(watches[i],DispatchPaint,reinterpret_cast<UINT_PTR>(this));
                app.OriginalTrackbarProc()=native;current=nullptr;trace::Require(false,"install transparent per-message native paint ledger");}
            watches[installed++]=control;}}
    ~Spy(){for(const auto control:watches)if(control&&IsWindow(control))RemoveWindowSubclass(control,DispatchPaint,reinterpret_cast<UINT_PTR>(this));
        app.OriginalTrackbarProc()=native;current=nullptr;}
    void Reset(HWND target,Failure kind){window=target;failure=kind;paints=supplied=begins=ends=copies=regionCalls=invalidates=0;
        trace::Require(!nativeDepth&&!dispatchDepth&&!activeDispatch,"paint ledger reset outside every synchronous dispatch");
        rootPaints=rootSupplied=dispatchCount=eventCount=0;dispatches={};events={};traceOverflow=unmappedNative=false;
        erases=beginErases=memoryErases=0;eraseDcType=0;beginInProgress=false;
        regions=deletedRegions=references=released=memories=deletedDCs=bitmaps=deletedBitmaps=0;
        injected=observerFailed=copyReturned=false;nativeWp=0;nativeResult=0;
        entry=surface::WindowPixels(target);beforeCopy={};rendered={};afterCopy={};beforeErase={};afterErase={};}
    bool Inject(Failure kind){if(failure!=kind||injected)return false;injected=true;return true;}
    static LRESULT CALLBACK DispatchPaint(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR reference){
        auto& self=*reinterpret_cast<Spy*>(reference);
        if(window!=self.window||message!=WM_PAINT)return DefSubclassProc(window,message,wp,lp);
        const auto previous=self.activeDispatch;const auto id=++self.dispatchCount;const auto depth=++self.dispatchDepth;
        self.activeDispatch=id;
        struct Restore {Spy& spy;unsigned previous;~Restore(){spy.activeDispatch=previous;--spy.dispatchDepth;}}restore{self,previous};
        Dispatch* record=nullptr;
        if(id<=self.dispatches.size()){record=&self.dispatches[id-1];record->id=id;record->parent=previous;record->depth=depth;record->inputWp=wp;}
        else self.traceOverflow=true;
        const auto result=DefSubclassProc(window,message,wp,lp);
        if(record){record->result=result;record->returned=true;}
        return result; // Transparent ledger: original message dispatched once.
    }
    static LRESULT CALLBACK Forward(HWND window,UINT message,WPARAM wp,LPARAM lp){auto& self=*current;
        const bool paint=window==self.window&&message==WM_PAINT;
        const bool erase=window==self.window&&message==WM_ERASEBKGND;
        if(erase){++self.erases;self.beginErases+=self.beginInProgress;self.eraseDcType=GetObjectType(reinterpret_cast<HDC>(wp));
            self.memoryErases+=self.eraseDcType==OBJ_MEMDC;
            if(self.beginInProgress)try{self.beforeErase=surface::WindowPixels(window);}catch(...){self.observerFailed=true;}}
        const bool root=paint&&self.nativeDepth==0;
        Event* event=nullptr;
        if(paint){++self.paints;self.supplied+=wp!=0;++self.nativeDepth;
            if(root){++self.rootPaints;self.rootSupplied+=wp!=0;self.nativeWp=wp;}
            if(self.activeDispatch&&self.activeDispatch<=self.dispatches.size()){auto& d=self.dispatches[self.activeDispatch-1];
                ++d.nativeCalls;d.nativeDepth=self.nativeDepth;d.nativeWp=wp;}else self.unmappedNative=true;
            if(self.eventCount<self.events.size()){event=&self.events[self.eventCount++];event->dispatch=self.activeDispatch;event->depth=self.nativeDepth;event->wp=wp;}
            else self.traceOverflow=true;
        }
        struct RestoreDepth {Spy& spy;bool paint;~RestoreDepth(){if(paint)--spy.nativeDepth;}}restoreDepth{self,paint};
        const auto result=CallWindowProcA(self.native,window,message,wp,lp);
        if(erase&&self.beginInProgress)try{self.afterErase=surface::WindowPixels(window);}catch(...){self.observerFailed=true;}
        if(paint){if(root)self.nativeResult=result;
            if(self.activeDispatch&&self.activeDispatch<=self.dispatches.size())self.dispatches[self.activeDispatch-1].nativeResult=result;
            // Synchronous native-call reentry boundary, before Forward regains
            // control. This is not a too-late final BitBlt injection.
            if(wp&&self.Inject(Failure::CaptureLoss)){if(GetCapture()==window)ReleaseCapture();SetFocus(self.app.MainWindow());}
            if(wp&&self.Inject(Failure::Resize)){RECT area{};if(GetWindowRect(window,&area))
                SetWindowPos(window,nullptr,0,0,area.right-area.left+1,area.bottom-area.top,
                    SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);}
        }
        if(event){event->result=result;event->returned=true;}return result;
    }
    static HRGN WINAPI Region(int left,int top,int right,int bottom){auto& s=*current;++s.regionCalls;
        if((s.regionCalls==1&&s.Inject(Failure::FirstRegion))||(s.regionCalls==2&&s.Inject(Failure::FullRegion)))return nullptr;
        const auto result=CreateRectRgn(left,top,right,bottom);s.regions+=result!=nullptr;return result;}
    static int WINAPI Update(HWND window,HRGN region,BOOL erase){auto& s=*current;
        return s.Inject(Failure::UpdateRegion)?ERROR:GetUpdateRgn(window,region,erase);}
    static HDC WINAPI Reference(HWND window){auto& s=*current;if(s.Inject(Failure::ReferenceDC))return nullptr;
        const auto result=GetDC(window);s.references+=result!=nullptr;return result;}
    static int WINAPI Release(HWND window,HDC dc){++current->released;return ReleaseDC(window,dc);}
    static HDC WINAPI Memory(HDC source){auto& s=*current;if(s.Inject(Failure::MemoryDC))return nullptr;
        const auto result=CreateCompatibleDC(source);s.memories+=result!=nullptr;return result;}
    static HBITMAP WINAPI Bitmap(HDC source,int width,int height){auto& s=*current;if(s.Inject(Failure::Bitmap))return nullptr;
        const auto result=CreateCompatibleBitmap(source,width,height);s.bitmaps+=result!=nullptr;return result;}
    static HGDIOBJ WINAPI Select(HDC dc,HGDIOBJ object){return current->Inject(Failure::Select)?HGDI_ERROR:SelectObject(dc,object);}
    static BOOL WINAPI Delete(HGDIOBJ object){auto& s=*current;const auto type=GetObjectType(object);const auto result=DeleteObject(object);
        if(result){s.deletedRegions+=type==OBJ_REGION;s.deletedBitmaps+=type==OBJ_BITMAP;}return result;}
    static BOOL WINAPI DeleteMemory(HDC dc){const auto result=DeleteDC(dc);current->deletedDCs+=result!=FALSE;return result;}
    static BOOL WINAPI Invalidate(HWND window,HRGN region,BOOL erase){auto& s=*current;++s.invalidates;
        return s.Inject(Failure::Invalidate)?FALSE:InvalidateRgn(window,region,erase);}
    static HDC WINAPI Begin(HWND window,LPPAINTSTRUCT paint){auto& s=*current;++s.begins;
        if(s.Inject(Failure::Begin))return nullptr;
        s.beginInProgress=true;struct Restore{Spy& spy;~Restore(){spy.beginInProgress=false;}}restore{s};
        return BeginPaint(window,paint);}
    static BOOL WINAPI End(HWND window,const PAINTSTRUCT* paint){auto& s=*current;++s.ends;
        if(s.failure==Failure::Begin&&s.injected)return TRUE; // No real BeginPaint was executed by this injected null result.
        const auto result=EndPaint(window,paint);return s.Inject(Failure::End)?FALSE:result;}
    static BOOL WINAPI Property(HWND window,LPCWSTR key,HANDLE value){auto& s=*current;
        return s.Inject(Failure::RetryProperty)?FALSE:SetPropW(window,key,value);}
    static BOOL WINAPI Copy(HDC destination,int x,int y,int width,int height,HDC source,int sx,int sy,DWORD raster){auto& s=*current;
        ++s.copies;
        try{
            trace::Require(GetObjectType(destination)==OBJ_DC&&GetObjectType(source)==OBJ_MEMDC,
                "candidate final copy transfers memory DC to actual display DC");
            s.beforeCopy=surface::WindowPixels(s.window);
            trace::Dib dib(width,height,source);trace::Require(bool(dib)&&BitBlt(dib.dc,0,0,width,height,source,sx,sy,SRCCOPY)&&GdiFlush(),
                "capture full final native memory surface before display transfer");
            s.rendered=s.entry;s.rendered.borrowedDc=true;
            s.rendered.pixels.assign(dib.pixels,dib.pixels+std::size_t(width)*height);
            if(s.failure==Failure::RetryProperty){s.copyReturned=false;return FALSE;}
            if(s.Inject(Failure::Transfer)){s.copyReturned=false;return FALSE;}
            const auto result=BitBlt(destination,x,y,width,height,source,sx,sy,raster);s.copyReturned=result!=FALSE;
            GdiFlush();s.afterCopy=surface::WindowPixels(s.window);return result;
        }catch(...){s.observerFailed=true;return FALSE;}
    }
    buffer::GdiFunctions Functions(){buffer::GdiFunctions functions;
        functions.createRectRgn=Region;functions.getUpdateRgn=Update;functions.getDC=Reference;functions.releaseDC=Release;
        functions.createCompatibleDC=Memory;functions.createCompatibleBitmap=Bitmap;functions.selectObject=Select;
        functions.deleteObject=Delete;functions.deleteDC=DeleteMemory;functions.invalidateRgn=Invalidate;
        functions.beginPaint=Begin;functions.endPaint=End;functions.bitBlt=Copy;functions.setProp=Property;return functions;}
    void Resources()const{trace::Require(regions==deletedRegions&&references==released&&memories==deletedDCs&&bitmaps==deletedBitmaps,
        "candidate releases every owned region, reference DC, memory DC and bitmap on this path");}
    void Dispatches(bool reentry)const{
        trace::Require(!traceOverflow&&!unmappedNative&&!nativeDepth&&!dispatchDepth&&!activeDispatch&&dispatchCount>=1&&
            dispatchCount<=dispatches.size()&&rootPaints==1,"exactly one outer native paint; all nested paints have bounded original message identities");
        trace::Require(reentry||dispatchCount==1,"no unplanned extra paint dispatch in a non-reentry case");
        for(unsigned i=0;i<dispatchCount;++i){const auto& d=dispatches[i];
            trace::Require(d.returned&&d.nativeCalls==1&&d.result==d.nativeResult,
                "each distinct original paint message invokes native once and returns its matching native result");
            trace::Require(i==0?(d.parent==0&&d.depth==1):(d.parent>0&&d.parent<d.id&&d.depth>1&&d.inputWp==0&&d.nativeWp==0),
                "extra paint is a separately dispatched synchronous native message with unchanged zero HDC, never a replay of the outer supplied paint");
        }
    }
};
static void Row(std::ofstream& report,const char* phase,const Spy& spy,LRESULT returned){
    report<<"CASE id="<<GetDlgCtrlID(spy.window)<<" phase="<<phase<<" fault="<<Name(spy.failure)<<" native_paints="<<spy.paints
        <<" supplied="<<spy.supplied<<" outer_native_paints="<<spy.rootPaints<<" outer_supplied="<<spy.rootSupplied<<" dispatches="<<spy.dispatchCount
        <<" begins="<<spy.begins<<" ends="<<spy.ends<<" final_copies="<<spy.copies<<" injected="<<spy.injected
        <<" native_erases="<<spy.erases<<" begin_erases="<<spy.beginErases<<" memory_erases="<<spy.memoryErases<<" erase_dc_type="<<spy.eraseDcType
        <<" observer_failed="<<spy.observerFailed<<" copy_returned="<<spy.copyReturned<<" returned="<<returned<<" native_returned="<<spy.nativeResult
        <<" actual_before_copy_diff="<<(spy.beforeCopy.copied?trace::Changed(spy.entry,spy.beforeCopy):0)
        <<" actual_after_copy_vs_native_diff="<<(spy.afterCopy.copied?trace::Changed(spy.rendered,spy.afterCopy):0)
        <<" actual_before_erase_diff="<<(spy.beforeErase.copied?trace::Changed(spy.entry,spy.beforeErase):0)
        <<" actual_after_erase_diff="<<(spy.afterErase.copied?trace::Changed(spy.entry,spy.afterErase):0)
        <<" owned_regions="<<spy.regions<<'/'<<spy.deletedRegions<<" reference_dc="<<spy.references<<'/'<<spy.released
        <<" memory_dc="<<spy.memories<<'/'<<spy.deletedDCs<<" bitmap="<<spy.bitmaps<<'/'<<spy.deletedBitmaps<<'\n';
    for(unsigned i=0;i<(std::min)(spy.dispatchCount,unsigned(spy.dispatches.size()));++i){const auto& d=spy.dispatches[i];
        report<<"DISPATCH id="<<d.id<<" parent="<<d.parent<<" depth="<<d.depth<<" input_wp="<<d.inputWp<<" native_calls="<<d.nativeCalls
            <<" native_depth="<<d.nativeDepth<<" native_wp="<<d.nativeWp<<" native_result="<<d.nativeResult<<" dispatch_result="<<d.result<<" returned="<<d.returned<<'\n';}
    for(unsigned i=0;i<spy.eventCount;++i){const auto& e=spy.events[i];report<<"NATIVE_ENTRY index="<<i<<" dispatch="<<e.dispatch<<" depth="<<e.depth
        <<" supplied_wp="<<e.wp<<" result="<<e.result<<" returned="<<e.returned<<'\n';}report.flush();
}
static void Paint(Spy& spy,HWND window,Failure failure,std::ofstream& report,const char* phase,bool buffered,bool erase=false){
    spy.Reset(window,failure);const observed::Quiet invariant(spy.app);auto functions=spy.Functions();LRESULT returned=0;
    {buffer::ScopedFunctionsForTesting inject(functions);trace::Require(InvalidateRect(window,nullptr,erase?TRUE:FALSE)!=FALSE,"prepare actual native update region with explicitly recorded erase flag");
        returned=SendMessageW(window,WM_PAINT,0,0);}
    Row(report,phase,spy,returned);invariant.Check();spy.Resources();spy.Dispatches(failure==Failure::CaptureLoss||failure==Failure::Resize);
    trace::Require(returned==spy.nativeResult&&!spy.observerFailed,
        "outer candidate paint preserves its own original native result independently of nested paint results");
    if(erase)trace::Require(buffered&&spy.erases==1&&spy.beginErases==1&&spy.memoryErases==1&&spy.eraseDcType==OBJ_MEMDC&&
        spy.beforeErase.copied&&spy.afterErase.copied&&trace::Changed(spy.entry,spy.beforeErase)==0&&trace::Changed(spy.entry,spy.afterErase)==0,
        "Invalidate(TRUE) reaches one actual native memory-DC erase during BeginPaint while actual window pixels remain unchanged before and after erasure");
    if(failure==Failure::CaptureLoss||failure==Failure::Resize){
        trace::Require(spy.rootSupplied==1&&spy.nativeWp&&spy.begins==1&&spy.ends==1&&!spy.copies,
            "capture/focus loss or changed geometry inside native painting rejects the obsolete final copy without replaying native paint");
        trace::Require(GetPropW(window,buffer::detail::kDirectOnce)==buffer::detail::DirectMarker(),
            "reentrant live-window rejection leaves an explicit later direct retry");
    }else if(buffered){
        trace::Require(spy.rootSupplied==1&&spy.nativeWp&&spy.begins==1&&spy.ends==1&&spy.copies==1&&spy.beforeCopy.copied&&spy.rendered.copied&&
            trace::Changed(spy.entry,spy.beforeCopy)==0,
            "buffered native paint uses supplied memory HDC and leaves actual window unchanged until its single final copy");
        if(failure!=Failure::Transfer&&failure!=Failure::RetryProperty)trace::Require(spy.copyReturned&&spy.afterCopy.copied&&
            trace::Changed(spy.rendered,spy.afterCopy)==0,"actual display after final copy equals every final original-native memory RGB pixel");
        else trace::Require(!spy.copyReturned&&trace::Changed(spy.entry,surface::WindowPixels(window))==0,
            "failed transfer does not leak an intermediate native memory surface into the actual window");
    }else trace::Require(spy.rootSupplied==0&&!spy.nativeWp&&!spy.copies&&spy.begins==(failure==Failure::Begin?1u:0u)&&
        spy.ends==(failure==Failure::Begin?1u:0u),"direct fallback retains original zero HDC argument without an extra buffer transaction");
    if(failure!=Failure::None)trace::Require(spy.injected,"requested resource/failure counterexample actually executed");
}
static void Supplied(Spy& spy,HWND window,std::ofstream& report){
    spy.Reset(window,Failure::None);const observed::Quiet invariant(spy.app);trace::Dib target(spy.entry.width,spy.entry.height);
    trace::Require(bool(target),"prepare caller-owned supplied HDC passthrough");auto functions=spy.Functions();LRESULT returned=0;
    {buffer::ScopedFunctionsForTesting inject(functions);returned=SendMessageW(window,WM_PAINT,reinterpret_cast<WPARAM>(target.dc),0);}
    Row(report,"supplied-HDC-direct",spy,returned);invariant.Check();spy.Resources();spy.Dispatches(false);
    trace::Require(spy.rootPaints==1&&spy.rootSupplied==1&&spy.nativeWp==reinterpret_cast<WPARAM>(target.dc)&&returned==spy.nativeResult&&
        !spy.begins&&!spy.ends&&!spy.copies&&!spy.regions&&trace::Changed(spy.entry,surface::WindowPixels(window))==0,
        "caller supplied native HDC is forwarded once unchanged without touching the display window or invoking the buffer");
}
static void Retry(Spy& spy,HWND window,std::ofstream& report){
    Paint(spy,window,Failure::None,report,"following-direct-retry",false);
}
struct RestoreSize {
    HWND window;RECT rectangle{};
    explicit RestoreSize(HWND target):window(target){trace::Require(GetWindowRect(window,&rectangle)!=FALSE,"save exact live slider size before native-call resize");}
    void Restore()noexcept{if(IsWindow(window))SetWindowPos(window,nullptr,0,0,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
        SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);}
    ~RestoreSize(){Restore();}
};
static void Run(MMDApp* app,const std::filesystem::path& outputDirectory,bool bound){
    trace::Require(app&&app==mikudancestudio::g_Block&&!GetCapture()&&!app->PlaybackActive()&&!app->FrameStepPlayback()&&
        bool(mikudancestudio::plugins::BoundNativePoseHistory(app))==bound,
        "candidate buffer regression requires a disposable idle actual host and requested real history binding");
    const auto output=outputDirectory/"buffer";std::filesystem::create_directories(output);
    std::ofstream report(output/"light-buffer-report.txt");trace::Require(bool(report),"open actual lighting buffer report");
    report<<"Actual six lighting HWNDs through production TrackbarSubclassProc and captured native class procedure; mode="<<(bound?"bound":"ordinary")
        <<". Synthetic down/move/up and navigation are labelled, not hardware or DWM tests. Display GetDC and final native memory HDC remain separate. "
        "Only hidden-focus captured exact-size lighting may buffer; visible keyboard cues, FOV and caller supplied HDC remain native direct. "
        "Failure injection changes individual candidate GDI calls only; no production/global import is replaced. Synchronous native-return reentry tests capture/focus loss and resize; destroy/HWND reuse is not exercised here. "
        "A failed retry property test runs last and leaves this disposable thread safely disabled.\n";
    const observed::Keys keys(*app);surface::Session session(*app);Spy spy(*app);
    try{
        for(int id:surface::kIds){session.Guard();const auto window=GetDlgItem(app->MainWindow(),id);
            trace::Require(window&&IsWindowVisible(window)&&IsWindowEnabled(window),"actual production lighting HWND is visible and enabled");
            SetFocus(app->MainWindow());session.Flush(window);RECT thumb{};
            SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
            const POINT down{(thumb.left+thumb.right)/2,(thumb.top+thumb.bottom)/2};RECT client{};GetClientRect(window,&client);
            const POINT move{(std::clamp)(down.x+(down.x<client.right/2?8:-8),LONG(0),client.right-1),down.y};
            session.ownCapture=window;SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(down.x,down.y));
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(move.x,move.y));
            trace::Require(GetCapture()==window&&(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)&&
                prior::Authored(*app,id,int(SendMessageW(window,TBM_GETPOS,0,0))),
                "real native captured synthetic movement retains original authored arithmetic and hidden focus cue");
            Paint(spy,window,Failure::None,report,"captured-hidden-success",true);keys.Check();
            surface::Bitmap(output/(std::to_string(id)+"-window-before.bmp"),spy.entry);
            surface::Bitmap(output/(std::to_string(id)+"-native-memory.bmp"),spy.rendered);
            surface::Bitmap(output/(std::to_string(id)+"-window-after.bmp"),spy.afterCopy);
            if(id==455){
                Paint(spy,window,Failure::None,report,"captured-hidden-explicit-erase",true,true);keys.Check();
                for(const auto failure:{Failure::FirstRegion,Failure::UpdateRegion,Failure::FullRegion,Failure::ReferenceDC,
                    Failure::MemoryDC,Failure::Bitmap,Failure::Select,Failure::Invalidate}){
                    session.Guard();Paint(spy,window,failure,report,"before-Begin-fallback",false);keys.Check();}
                Paint(spy,window,Failure::Begin,report,"null-Begin-once-fallback",false);Retry(spy,window,report);
                Paint(spy,window,Failure::Transfer,report,"failed-transfer-once",true);Retry(spy,window,report);
                Paint(spy,window,Failure::End,report,"failed-End-once",true);Retry(spy,window,report);
                Paint(spy,window,Failure::CaptureLoss,report,"native-sync-capture-focus-loss",false);Retry(spy,window,report);
                session.Guard();RECT liveThumb{};SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&liveThumb));
                SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM((liveThumb.left+liveThumb.right)/2,(liveThumb.top+liveThumb.bottom)/2));
                trace::Require(GetCapture()==window,"reacquire actual native capture after the controlled loss counterexample");
                {RestoreSize restore(window);Paint(spy,window,Failure::Resize,report,"native-sync-resize",false);
                    Retry(spy,window,report);restore.Restore();}
                Supplied(spy,window,report);
                Paint(spy,GetDlgItem(app->MainWindow(),447),Failure::None,report,"non-lighting-direct",false);
            }
            // Actual production navigation clears HIDEFOCUS. Its legitimate
            // value/history effect happens before the paint-only invariant.
            SendMessageW(window,WM_KEYDOWN,VK_HOME,0);SendMessageW(window,WM_KEYUP,VK_HOME,0);
            trace::Require(!(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)&&
                prior::Authored(*app,id,int(SendMessageW(window,TBM_GETPOS,0,0))),"actual synthetic native navigation keeps visible keyboard cue and exact values");
            Paint(spy,window,Failure::None,report,"keyboard-visible-direct",false);keys.Check();
            if(id==460){
                SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
                Paint(spy,window,Failure::RetryProperty,report,"failed-retry-property-last",true);
                Retry(spy,window,report);keys.Check();
            }
            session.Guard();SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(move.x,move.y));
            trace::Require(!GetCapture(),"unchanged native release relinquishes only its own capture");session.ownCapture=nullptr;
        }
        keys.Check();session.Guard();report<<"PASS all six real lighting routes, native-once fallback, display/memory final parity, VMD and paint-only history invariants. elapsed_ms="
            <<GetTickCount64()-session.started<<'\n';report.flush();
    }catch(const std::exception& error){report<<"FAILED_OR_INCOMPLETE reason="<<error.what()<<" elapsed_ms="<<GetTickCount64()-session.started<<'\n';report.flush();throw;}
}
} // namespace light_slider_buffer_checks
#endif
