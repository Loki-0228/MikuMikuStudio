// Include after slider_paint_trace_host_checks.inc in the disposable MMS host.
#include "../slider_paint_trace.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <fstream>
#include <memory>
#if !MMD_ORIGINAL_PROFILE
namespace slider_focus_cues_host_checks {
namespace trace=slider_paint_trace;
namespace prior=slider_paint_trace_host_checks;
using mikumikustudio::MMDApp;
struct Keys {
    MMDApp& app;const int frame;
    const mikumikustudio::mdl::CameraKey* camera;
    const mikumikustudio::mdl::LightKey* light;
    const std::uint64_t cameraId,lightId;
    const std::vector<mikumikustudio::mdl::CameraKey> cameraBytes;
    const std::vector<mikumikustudio::mdl::LightKey> lightBytes;
    explicit Keys(MMDApp& a):app(a),frame(a.CurrentFrame()),camera(a.CameraKeys()),light(a.LightKeys()),
        cameraId(mikumikustudio::mdl::KeyPoolIdentity(camera)),lightId(mikumikustudio::mdl::KeyPoolIdentity(light)),
        cameraBytes(camera,camera+mikumikustudio::mdl::KeyPoolCapacity(camera)),lightBytes(light,light+mikumikustudio::mdl::KeyPoolCapacity(light)){}
    void Check()const{
        using namespace mikumikustudio::mdl;
        trace::Require(app.CurrentFrame()==frame&&app.CameraKeys()==camera&&app.LightKeys()==light&&
            KeyPoolIdentity(camera)==cameraId&&KeyPoolIdentity(light)==lightId&&
            KeyPoolCapacity(camera)==cameraBytes.size()&&KeyPoolCapacity(light)==lightBytes.size()&&
            !std::memcmp(camera,cameraBytes.data(),cameraBytes.size()*sizeof(*camera))&&
            !std::memcmp(light,lightBytes.data(),lightBytes.size()*sizeof(*light)),"focus cues preserve playhead and original VMD key pointers, identities, capacities and bytes");
    }
};
static UINT Ui(HWND window){return UINT(SendMessageW(window,WM_QUERYUISTATE,0,0));}
struct UiState {
    std::array<HWND,9> windows{};std::array<UINT,9> state{};HWND target=nullptr;std::ostream& report;
    UiState(MMDApp& app,HWND current,std::ostream& log):target(current),report(log){windows[0]=app.MainWindow();
        for(unsigned i=0;i<7;++i)windows[i+1]=GetDlgItem(app.MainWindow(),prior::kIds[i]);
        windows[8]=GetDlgItem(app.MainWindow(),556);
        for(unsigned i=0;i<windows.size();++i){trace::Require(windows[i]!=nullptr,"query actual parent,seven sliders,and checkbox UI state");state[i]=Ui(windows[i]);}}
    void Check(bool hidden)const{bool valid=true;for(unsigned i=0;i<windows.size();++i){const auto actual=Ui(windows[i]);
        const bool same=windows[i]==target?((actual&~UISF_HIDEFOCUS)==(state[i]&~UISF_HIDEFOCUS)&&bool(actual&UISF_HIDEFOCUS)==hidden):actual==state[i];
        if(!same)report<<"UI_DIFF target="<<GetDlgCtrlID(target)<<" observed_id="<<GetDlgCtrlID(windows[i])<<" hwnd="<<reinterpret_cast<std::uintptr_t>(windows[i])
            <<" before="<<state[i]<<" after="<<actual<<" xor="<<(state[i]^actual)<<" expected_target_hidden="<<hidden<<'\n';valid&=same;}
        trace::Require(valid,"only target HIDEFOCUS changes; target ACCEL/ACTIVE and all parent/sibling/checkbox UI bits remain identical");}
};
struct TreeState {
    struct Entry{HWND window=nullptr;UINT state=0;unsigned depth=0;};
    HWND main=nullptr;std::vector<Entry> entries;bool overflow=false;
    static BOOL CALLBACK Add(HWND window,LPARAM data){auto& self=*reinterpret_cast<TreeState*>(data);
        if(self.entries.size()==self.entries.capacity()){self.overflow=true;return FALSE;}
        unsigned depth=0;for(HWND ancestor=window;ancestor&&ancestor!=self.main&&depth<64;ancestor=GetParent(ancestor))++depth;
        self.entries.push_back({window,Ui(window),depth});return TRUE;
    }
    explicit TreeState(HWND window):main(window){entries.reserve(512);entries.push_back({window,Ui(window),0});
        EnumChildWindows(window,Add,reinterpret_cast<LPARAM>(this));trace::Require(!overflow,"bounded complete native UI tree snapshot");
        std::stable_sort(entries.begin(),entries.end(),[](const auto&a,const auto&b){return a.depth<b.depth;});}
    const Entry* Find(HWND window)const{for(const auto& entry:entries)if(entry.window==window)return &entry;return nullptr;}
    void Restore()const{for(const auto& entry:entries){trace::Require(IsWindow(entry.window)&&(entry.state&~7u)==0,"restore only captured live UI tree supported bits");
        const auto actual=Ui(entry.window);const UINT set=entry.state&~actual,clear=actual&~entry.state;
        if(set)SendMessageW(entry.window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,set),0);
        if(clear)SendMessageW(entry.window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,clear),0);}
        for(const auto& entry:entries)trace::Require(Ui(entry.window)==entry.state,"fixture restores every original UI tree bit before production replay");}
    void LogNative(const TreeState& before,std::ostream& report)const{for(const auto& entry:entries){const auto* original=before.Find(entry.window);
        trace::Require(original!=nullptr,"raw native navigation retains complete UI tree identity");if(original->state!=entry.state)
            report<<"RAW_NATIVE_TREE_DIFF id="<<GetDlgCtrlID(entry.window)<<" hwnd="<<reinterpret_cast<std::uintptr_t>(entry.window)
                <<" before="<<original->state<<" after="<<entry.state<<" xor="<<(original->state^entry.state)<<'\n';}}
    void CheckProduction(const TreeState& actual,HWND target,UINT desired,std::ostream& report)const{
        bool valid=entries.size()==actual.entries.size();for(const auto& entry:entries){const auto* result=actual.Find(entry.window);const UINT expected=entry.window==target?desired:entry.state;
            if(!result||result->state!=expected){valid=false;report<<"PRODUCTION_VS_NATIVE_DIFF target="<<GetDlgCtrlID(target)<<" observed_id="<<GetDlgCtrlID(entry.window)
                <<" expected="<<expected<<" actual="<<(result?result->state:~0u)<<'\n';}}
        trace::Require(valid,"production keyboard tree UI effects exactly match same-state raw native navigation except required local target HIDEFOCUS");}
};
struct Pure {
    prior::PumpInvariant invariant;MMDApp& app;
    const int modified,physics,recorded;
    explicit Pure(MMDApp& a):invariant(a),app(a),modified(a.SceneModified()),physics(a.PhysicsResetPending()),recorded(a.RecordedFrameCountByte1()){}
    void Check()const{invariant.Check();trace::Require(app.SceneModified()==modified&&app.PhysicsResetPending()==physics&&
        app.RecordedFrameCountByte1()==recorded,"focus/paint/cue changes add no dirty,physics,recording or history operation");}
};
struct Spy {
    struct Event{HWND window=nullptr;UINT message=0,ui=0;WPARAM key=0;bool oracle=false;};
    struct Storage{std::array<Event,2048> events{};unsigned size=0;};
    MMDApp& app;WNDPROC native=nullptr;std::unique_ptr<Storage> storage=std::make_unique<Storage>();bool truncated=false,oracle=false;
    inline static thread_local Spy* current=nullptr;
    explicit Spy(MMDApp& a):app(a),native(a.OriginalTrackbarProc()){
        trace::Require(native&&current==nullptr,"one real native trackbar forwarding spy");current=this;a.OriginalTrackbarProc()=Forward;}
    ~Spy(){app.OriginalTrackbarProc()=native;current=nullptr;}
    static LRESULT CALLBACK Forward(HWND window,UINT message,WPARAM wp,LPARAM lp){auto& self=*current;
        if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK||message==WM_MOUSEMOVE||message==WM_SETFOCUS||message==WM_PAINT||message==WM_KEYDOWN||message==WM_KEYUP){
            if(self.storage->size<self.storage->events.size()){
                auto& event=self.storage->events[self.storage->size++];event={window,message,
                    UINT(CallWindowProcA(self.native,window,WM_QUERYUISTATE,0,0)),wp,self.oracle};
            }else self.truncated=true;
        }
        return CallWindowProcA(self.native,window,message,wp,lp); // original message once, unmodified result
    }
    static LRESULT CALLBACK RawOracle(HWND window,UINT message,WPARAM wp,LPARAM lp){
        if(message==WM_LBUTTONUP)SetFocus(current->app.MainWindow());
        return Forward(window,message,wp,lp);
    }
    unsigned Count(unsigned begin,HWND window,UINT message,bool hidden,bool requireUi=true)const{
        unsigned count=0;for(unsigned i=begin;i<storage->size;++i){const auto& e=storage->events[i];if(e.window==window&&e.message==message){
            if(requireUi)trace::Require(bool(e.ui&UISF_HIDEFOCUS)==hidden,"native input/focus/paint sees the requested cue before it runs");++count;}}
        return count;
    }
    void Write(const std::filesystem::path& output)const{std::ofstream file(output/"native-cue-entry.csv");
        file<<"index,id,message,wparam,ui_before_native,raw_oracle\n";for(unsigned i=0;i<storage->size;++i){const auto&e=storage->events[i];
            file<<i<<','<<GetDlgCtrlID(e.window)<<','<<e.message<<','<<e.key<<','<<e.ui<<','<<e.oracle<<'\n';}
        trace::Require(bool(file),"write native entry cue ledger");}
};
struct RawOracleScope{
    Spy& spy;HWND window;WNDPROC production=nullptr;const bool previous;
    RawOracleScope(Spy& s,HWND h):spy(s),window(h),previous(s.oracle){
        production=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(h,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Spy::RawOracle)));
        trace::Require(production!=nullptr,"temporarily use the captured native procedure for the raw visible-border counterexample");spy.oracle=true;
    }
    ~RawOracleScope(){if(IsWindow(window))SetWindowLongPtrA(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(production));spy.oracle=previous;}
};
static void Guard(MMDApp& app){
    for(int key=1;key<=255;++key)trace::Require((GetAsyncKeyState(key)&0x8000)==0,"real held key/button leaves cue regression incomplete");
    const auto capture=GetCapture();bool own=!capture;for(int id:prior::kIds)own|=capture==GetDlgItem(app.MainWindow(),id);
    trace::Require(own,"never steal or release foreign capture");
}
static void Flush(MMDApp& app){Guard(app);for(int id:prior::kIds)UpdateWindow(GetDlgItem(app.MainWindow(),id));GdiFlush();}
static trace::Snapshot Pixels(HWND window){
    trace::Snapshot result;result.window=window;result.id=GetDlgCtrlID(window);RECT client{};
    trace::Require(GetClientRect(window,&client)&&client.right>0&&client.bottom>0&&client.right<=2048&&client.bottom<=128,"bounded actual slider pixel capture");
    result.width=client.right;result.height=client.bottom;result.visible=IsWindowVisible(window)!=FALSE;
    const HDC display=GetDC(window);trace::Require(display!=nullptr,"obtain actual slider display DC");
    struct Release{HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}} release{window,display};
    trace::Require(GetObjectType(display)==OBJ_DC,"capture actual display DC,not a supplied HDC");
    result.clipKind=GetClipBox(display,&result.clip);trace::Dib bitmap(result.width,result.height,display);
    trace::Require(bitmap&&BitBlt(bitmap.dc,0,0,result.width,result.height,display,0,0,SRCCOPY),"copy actual slider pixels");GdiFlush();
    result.pixels.assign(bitmap.pixels,bitmap.pixels+std::size_t(result.width)*result.height);result.copied=true;
    trace::Require(result.Live()&&result.Varied(),"only live nonuniform actual-window pixels qualify");return result;
}
static std::array<unsigned,2> Differences(const trace::Snapshot& a,const trace::Snapshot& b){
    trace::Require(a.width==b.width&&a.height==b.height,"compare identical real slider geometry");std::array<unsigned,2> result{};
    for(int y=0;y<a.height;++y)for(int x=0;x<a.width;++x){const auto index=std::size_t(y)*a.width+x;
        if((a.pixels[index]&0xffffffu)!=(b.pixels[index]&0xffffffu))++result[x<2||y<2||x>=a.width-2||y>=a.height-2?0:1];}return result;
}
static void Bitmap(const std::filesystem::path& path,const trace::Snapshot& image){BITMAPFILEHEADER file{};BITMAPINFOHEADER header{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(header);file.bfSize=file.bfOffBits+DWORD(image.pixels.size()*4);
    header.biSize=sizeof(header);header.biWidth=image.width;header.biHeight=-image.height;header.biPlanes=1;header.biBitCount=32;header.biCompression=BI_RGB;
    std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(&file),sizeof(file));stream.write(reinterpret_cast<const char*>(&header),sizeof(header));
    stream.write(reinterpret_cast<const char*>(image.pixels.data()),std::streamsize(image.pixels.size()*4));trace::Require(bool(stream),"write bounded real focus-border bitmap");}
static POINT Center(HWND window){RECT thumb{};SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));return{(thumb.left+thumb.right)/2,(thumb.top+thumb.bottom)/2};}
static void Prime(MMDApp& app,HWND window,int position){
    SetFocus(app.MainWindow());SendMessageW(window,TBM_SETPOS,TRUE,position);
    SendMessageW(app.MainWindow(),WM_HSCROLL,MAKEWPARAM(TB_THUMBTRACK,position),reinterpret_cast<LPARAM>(window));Flush(app);
    trace::Require(prior::Authored(app,GetDlgCtrlID(window),int(SendMessageW(window,TBM_GETPOS,0,0))),"fixture uses unchanged native parameter arithmetic before cue-only comparisons");
}
static void Run(MMDApp* app,const std::filesystem::path& output,bool bound){
    using namespace mikumikustudio;
    trace::Require(app&&app==g_Block&&app->CameraKeys()&&app->LightKeys()&&app->MainWindow()&&!GetCapture()&&!app->PlaybackActive()&&
        !app->FrameStepPlayback()&&bool(plugins::BoundNativePoseHistory(app))==bound,"focus regression requires actual idle disposable host and requested history binding");
    std::filesystem::create_directories(output);std::ofstream report(output/"focus-cues-report.txt");trace::Require(bool(report),"open focus regression report");
    report<<"Actual MMS seven-slider cue regression; mode="<<(bound?"bound":"ordinary")<<". Mouse/key messages are labelled synthetic; external focus entry uses real SetFocus away/back, not fake Tab dispatch. "
        "Actual GetDC pixels only; no WM_PRINT/HDC replacement, no hardware/DWM/flicker-duration claim. Native arithmetic fixture priming uses actual HSCROLL before each cue-only invariant baseline.\n";
    const Keys keys(*app);Spy spy(*app);
    struct Cleanup{MMDApp& app;~Cleanup(){const auto capture=GetCapture();for(int id:prior::kIds)if(capture&&capture==GetDlgItem(app.MainWindow(),id)){
        SendMessageW(capture,WM_CANCELMODE,0,0);if(GetCapture()==capture)ReleaseCapture();break;}}}cleanup{*app};
    try{
        for(int id:prior::kIds){const auto window=GetDlgItem(app->MainWindow(),id);
            trace::Require(window&&IsWindowVisible(window)&&IsWindowEnabled(window),"all seven production sliders visible and enabled");
            Prime(*app,window,int(SendMessageW(window,TBM_GETPOS,0,0)));keys.Check();const auto point=Center(window);const auto parameter=MAKELPARAM(point.x,point.y);
            // The captured raw class procedure bypasses only the new mouse cue
            // policy on this exact HWND. It still paints the actual native window.
            SetFocus(app->MainWindow());SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS|UISF_HIDEACCEL|UISF_ACTIVE),0);
            trace::Require(Ui(window)==0,"raw native border counterexample starts with actual UI state zero");
            report<<"PHASE id="<<id<<" raw-native-mouse\n";const Pure oracleInvariant(*app);const UiState oracleUi(*app,window,report);
            trace::Snapshot rawVisible;
            {RawOracleScope oracle(spy,window);Guard(*app);SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,parameter);Flush(*app);
                trace::Require(GetFocus()==window&&GetCapture()==window,"raw native mouse oracle retains actual focus and capture");oracleUi.Check(false);oracleInvariant.Check();
                rawVisible=Pixels(window);SendMessageW(window,WM_LBUTTONUP,0,parameter);Flush(*app);oracleInvariant.Check();}
            report<<"PHASE id="<<id<<" production-mouse\n";const Pure mouseInvariant(*app);const UiState mouseUi(*app,window,report);const auto start=spy.storage->size;
            Guard(*app);trace::Require(!GetCapture(),"mouse regression starts without taking another capture");
            SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,parameter);Flush(*app);
            trace::Require(GetFocus()==window&&GetCapture()==window&&spy.Count(start,window,WM_LBUTTONDOWN,true)==1&&
                spy.Count(start,window,WM_SETFOCUS,true)==1&&spy.Count(start,window,WM_PAINT,true)>0,"production mouse hides cue before native down,synchronous focus,and first paint; each input forwarded once");
            mouseUi.Check(true);mouseInvariant.Check();const auto mouseHidden=Pixels(window);const auto difference=Differences(rawVisible,mouseHidden);
            trace::Require(difference[0]>0&&difference[1]==0,"raw native whole-slider focus border is removed for mouse while all inner channel/thumb pixels remain native-identical");
            Bitmap(output/(std::to_string(id)+"-raw-native-mouse-visible.bmp"),rawVisible);Bitmap(output/(std::to_string(id)+"-production-mouse-hidden.bmp"),mouseHidden);
            for(unsigned repeat=0;repeat<3;++repeat){const auto move=spy.storage->size;const auto tick=SendMessageW(window,TBM_GETPOS,0,0);
                SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,parameter);Flush(*app);
                trace::Require(spy.Count(move,window,WM_MOUSEMOVE,true)==1&&SendMessageW(window,TBM_GETPOS,0,0)==tick,
                    "stationary captured MOVE forwards once and keeps cue hidden without authored tick changes");mouseUi.Check(true);mouseInvariant.Check();keys.Check();}
            SendMessageW(window,WM_LBUTTONUP,0,parameter);Flush(*app);trace::Require(GetFocus()==app->MainWindow()&&!GetCapture(),"existing release focus/capture behavior preserved");mouseInvariant.Check();
            // Real focus departure/entry models a navigation layer moving back
            // to a previously mouse-hidden HWND; no global keyboard state fake.
            report<<"PHASE id="<<id<<" external-focus-return\n";const auto enter=spy.storage->size;const UiState enterUi(*app,window,report);const Pure enterInvariant(*app);
            SetFocus(window);Flush(*app);trace::Require(GetFocus()==window&&spy.Count(enter,window,WM_SETFOCUS,false)==1,"actual external focus entry restores keyboard cue before native focus");
            enterUi.Check(false);enterInvariant.Check();const auto returned=Pixels(window);Bitmap(output/(std::to_string(id)+"-focus-return-visible.bmp"),returned);
            trace::Require(Differences(mouseHidden,returned)[0]>0,"whole-slider focus border is available again on actual navigation focus entry");
            SetFocus(app->MainWindow());SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEACCEL|UISF_ACTIVE),0);
            report<<"PHASE id="<<id<<" production-double-click\n";const UiState extraUi(*app,window,report);const Pure extraInvariant(*app);const auto dbl=spy.storage->size;
            Guard(*app);SendMessageW(window,WM_LBUTTONDBLCLK,MK_LBUTTON,parameter);Flush(*app);
            trace::Require(spy.Count(dbl,window,WM_LBUTTONDBLCLK,true)==1,"double-click also hides only focus cue before native once");extraUi.Check(true);extraInvariant.Check();
            if(GetCapture()==window)SendMessageW(window,WM_LBUTTONUP,0,parameter);SetFocus(app->MainWindow());Flush(*app);
            for(WPARAM key:{WPARAM(VK_LEFT),WPARAM(VK_RIGHT),WPARAM(VK_UP),WPARAM(VK_DOWN),WPARAM(VK_HOME),WPARAM(VK_END),WPARAM(VK_PRIOR),WPARAM(VK_NEXT),WPARAM(VK_TAB)}){
                const auto minimum=int(SendMessageW(window,TBM_GETRANGEMIN,0,0)),maximum=int(SendMessageW(window,TBM_GETRANGEMAX,0,0));
                const bool low=key==VK_LEFT||key==VK_UP||key==VK_HOME||key==VK_PRIOR;
                const int tick=key==VK_TAB?int(SendMessageW(window,TBM_GETPOS,0,0)):low?minimum:maximum;
                Prime(*app,window,tick);SetFocus(window);SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
                report<<"PHASE id="<<id<<" native-keyboard-oracle key="<<key<<'\n';
                const Pure invariant(*app);const TreeState initialTree(app->MainWindow());const UINT targetBefore=Ui(window);const auto raw=spy.storage->size;
                {RawOracleScope oracle(spy,window);SendMessageW(window,WM_KEYDOWN,key,1);SendMessageW(window,WM_KEYUP,key,LPARAM(0xc0000001u));Flush(*app);}
                trace::Require(spy.Count(raw,window,WM_KEYDOWN,false,false)==1&&spy.Count(raw,window,WM_KEYUP,false,false)==1&&SendMessageW(window,TBM_GETPOS,0,0)==tick,
                    "raw native boundary navigation runs once without authored tick change");invariant.Check();keys.Check();
                const TreeState rawAfter(app->MainWindow());rawAfter.LogNative(initialTree,report);initialTree.Restore();Flush(*app);invariant.Check();
                report<<"PHASE id="<<id<<" production-keyboard key="<<key<<'\n';const auto before=spy.storage->size;
                SendMessageW(window,WM_KEYDOWN,key,1);SendMessageW(window,WM_KEYUP,key,LPARAM(0xc0000001u));Flush(*app);
                trace::Require(spy.Count(before,window,WM_KEYDOWN,false)==1&&spy.Count(before,window,WM_KEYUP,false)==1&&
                    SendMessageW(window,TBM_GETPOS,0,0)==tick,"each boundary navigation key is forwarded once with visible cues and original native clamping");
                const TreeState productionAfter(app->MainWindow());rawAfter.CheckProduction(productionAfter,window,targetBefore&~UISF_HIDEFOCUS,report);invariant.Check();keys.Check();
            }
            report<<"PHASE id="<<id<<" production-non-navigation\n";SendMessageW(window,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);const UiState nonnavUi(*app,window,report);const Pure nonnavInvariant(*app);const auto typing=spy.storage->size;
            SendMessageW(window,WM_KEYDOWN,'A',1);SendMessageW(window,WM_KEYUP,'A',LPARAM(0xc0000001u));Flush(*app);
            trace::Require(spy.Count(typing,window,WM_KEYDOWN,true)==1&&spy.Count(typing,window,WM_KEYUP,true)==1,"non-navigation key leaves mouse cue mode intact and forwards once");nonnavUi.Check(true);nonnavInvariant.Check();keys.Check();
            report<<"CONTROL id="<<id<<" mouse_border_pixels="<<difference[0]<<" mouse_inner_pixels="<<difference[1]<<" captured_same_point_moves=3 authored_tick_step_not_covered=1 nav_keys=9 native_once=1 history_cue_only_unchanged=1\n";
        }
        trace::Require(!spy.truncated,"complete fixed-capacity native cue entry observation");keys.Check();spy.Write(output);
        report<<"PASS controls=7 BMPs=21 key_VMD_bytes_unchanged=1 cue_only_values_history_unchanged=1; native whole-control focus border remains available to keyboard navigation.\n";
    }catch(...){try{spy.Write(output);}catch(...){report<<"ENTRY_LEDGER_WRITE_FAILED; original exception retained.\n";}
        report<<"FAILED/INCOMPLETE; no complete visual or cue-policy claim.\n";report.flush();throw;}
    report.flush();trace::Require(bool(report),"finish real-host focus regression report");
}
}
#endif
