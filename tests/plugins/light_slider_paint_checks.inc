// Include after slider_paint_trace_host_checks.inc in a disposable visible host.
// Observes production drawing; it neither replaces drawing nor judges flicker.
#include "../slider_paint_trace.hpp"
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/globals.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/update_coordinator.hpp"
#include <cstring>

#if !MMD_ORIGINAL_PROFILE
namespace light_slider_paint_checks {
namespace trace=slider_paint_trace;
namespace prior=slider_paint_trace_host_checks;
using mikudancestudio::MMDApp;
static constexpr std::array<int,14> kIds{455,456,457,458,459,460,461,462,463,464,465,466,447,560};
static constexpr std::size_t kMessageLimit=8192,kImageLimit=4096,kPixelBytes=64*1024*1024;
static constexpr unsigned kSampleLimit=192,kBitmapLimit=96;
static bool Slider(int id) noexcept {return (id>=455&&id<=460)||id==447||id==560;}
static std::array<float,8> Values(MMDApp& app){return {app.LightColor()[0],app.LightColor()[1],app.LightColor()[2],
    app.LightDirection()[0],app.LightDirection()[1],app.LightDirection()[2],app.CameraFov(),app.ShadowDistance()};}
template<class Key> struct KeyCopy {
    Key* const pointer;const std::uint64_t identity;const std::vector<Key> bytes;
    explicit KeyCopy(Key* value):pointer(value),identity(mikudancestudio::mdl::KeyPoolIdentity(value)),
        bytes(value,value+mikudancestudio::mdl::KeyPoolCapacity(value)){}
    void Check(Key* value)const{using namespace mikudancestudio::mdl;
        trace::Require(value==pointer&&KeyPoolIdentity(value)==identity&&KeyPoolCapacity(value)==bytes.size()&&
            !std::memcmp(value,bytes.data(),bytes.size()*sizeof(Key)),"light drawing diagnosis preserves original VMD pool identity, capacity and every key byte");}
};
struct Keys {
    MMDApp& app;const int frame;
    KeyCopy<mikudancestudio::mdl::CameraKey> camera;
    KeyCopy<mikudancestudio::mdl::LightKey> light;
    KeyCopy<mikudancestudio::mdl::SelfShadowKey> shadow;
    KeyCopy<mikudancestudio::mdl::GravityKey> gravity;
    explicit Keys(MMDApp& a):app(a),frame(a.CurrentFrame()),camera(a.CameraKeys()),light(a.LightKeys()),shadow(a.ShadowKeys()),gravity(a.GravityKeys()){}
    void Check()const{trace::Require(app.CurrentFrame()==frame,"light drawing diagnosis does not move the playhead");
        camera.Check(app.CameraKeys());light.Check(app.LightKeys());shadow.Check(app.ShadowKeys());gravity.Check(app.GravityKeys());}
};
struct Quiet {
    prior::PumpInvariant invariant;MMDApp& app;const int modified,physics,recorded;const float shadow;
    explicit Quiet(MMDApp& a):invariant(a),app(a),modified(a.SceneModified()),physics(a.PhysicsResetPending()),
        recorded(a.RecordedFrameCountByte1()),shadow(a.ShadowDistance()){}
    void Check()const{invariant.Check();trace::Require(app.SceneModified()==modified&&app.PhysicsResetPending()==physics&&
        app.RecordedFrameCountByte1()==recorded&&app.ShadowDistance()==shadow,
        "observation, UpdateWindow and a single production safe point add no edit, dirty flag, recording state or history");}
};
struct Messages {
    struct Event {
        std::uint64_t sequence=0;LONGLONG begin=0,end=0;unsigned phase=0,depth=0;HWND window=nullptr,source=nullptr;
        UINT message=0,notify=0,drawStage=0,itemState=0;UINT_PTR item=0;WPARAM wp=0;LPARAM lp=0;LRESULT result=0;
        HWND focusBefore=nullptr,focusAfter=nullptr,captureBefore=nullptr,captureAfter=nullptr;
        bool native=false,forwarded=false;
    };
    struct Storage {std::array<Event,kMessageLimit> events{};std::size_t size=0;};
    struct Watch {Messages* owner=nullptr;HWND window=nullptr;unsigned paints=0,erases=0,sets=0,positions=0;};
    MMDApp& app;std::unique_ptr<Storage> storage=std::make_unique<Storage>();std::array<Watch,15> watches{};
    WNDPROC original=nullptr;unsigned phase=0,depth=0;bool truncated=false,attached=false;
    inline static thread_local Messages* current=nullptr;
    static bool Interesting(UINT message)noexcept{
        return message==WM_PAINT||message==WM_ERASEBKGND||message==WM_PRINTCLIENT||message==WM_SETTEXT||message==EM_REPLACESEL||
            message==EM_SETSEL||message==TBM_SETPOS||message==WM_HSCROLL||message==WM_COMMAND||message==WM_NOTIFY||message==WM_SETFOCUS||
            message==WM_KILLFOCUS||message==WM_CHANGEUISTATE||message==WM_UPDATEUISTATE||message==WM_CAPTURECHANGED||
            message==WM_CANCELMODE||message==WM_MOUSEMOVE||message==WM_MOUSEHOVER||message==WM_MOUSELEAVE||
            message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_APP+0x6a||
            (message>=WM_CTLCOLORMSGBOX&&message<=WM_CTLCOLORSTATIC);
    }
    Event* Begin(HWND window,UINT message,WPARAM wp,LPARAM lp,bool native)noexcept{
        if(!Interesting(message))return nullptr;
        if(storage->size==storage->events.size()){truncated=true;return nullptr;}
        auto& e=storage->events[storage->size++];e.sequence=storage->size;e.phase=phase;e.depth=depth;e.window=window;
        e.message=message;e.wp=wp;e.lp=lp;e.native=native;e.focusBefore=GetFocus();e.captureBefore=GetCapture();
        LARGE_INTEGER time{};QueryPerformanceCounter(&time);e.begin=time.QuadPart;
        if(message==WM_NOTIFY&&lp){const auto& header=*reinterpret_cast<const NMHDR*>(lp);e.source=header.hwndFrom;e.notify=header.code;
            if(header.code==UINT(NM_CUSTOMDRAW)){const auto& draw=*reinterpret_cast<const NMCUSTOMDRAW*>(lp);
                e.drawStage=draw.dwDrawStage;e.itemState=draw.uItemState;e.item=draw.dwItemSpec;}}
        else if(message==WM_HSCROLL||message==WM_COMMAND||(message>=WM_CTLCOLORMSGBOX&&message<=WM_CTLCOLORSTATIC)){
            e.source=reinterpret_cast<HWND>(lp);if(message==WM_COMMAND)e.notify=HIWORD(wp);}
        return &e;
    }
    void End(Event* event,LRESULT result)noexcept{if(event){event->result=result;event->forwarded=true;
        event->focusAfter=GetFocus();event->captureAfter=GetCapture();LARGE_INTEGER time{};QueryPerformanceCounter(&time);event->end=time.QuadPart;}}
    static LRESULT CALLBACK Observe(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
        auto& watch=*reinterpret_cast<Watch*>(data);auto& self=*watch.owner;
        if(message==WM_PAINT)++watch.paints;if(message==WM_ERASEBKGND)++watch.erases;
        if(message==WM_SETTEXT||message==EM_REPLACESEL)++watch.sets;if(message==TBM_SETPOS)++watch.positions;
        auto* event=self.Begin(window,message,wp,lp,false);++self.depth;
        const auto result=DefSubclassProc(window,message,wp,lp);--self.depth;self.End(event,result);return result;
    }
    static LRESULT CALLBACK Native(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto& self=*current;auto* event=self.Begin(window,message,wp,lp,true);++self.depth;
        const auto result=CallWindowProcA(self.original,window,message,wp,lp);--self.depth;self.End(event,result);return result;
    }
    Messages(MMDApp& a,const std::array<HWND,14>& controls):app(a),original(a.OriginalTrackbarProc()){
        trace::Require(original&&!current,"one native-procedure forwarding observer for light diagnosis");
        try{for(std::size_t i=0;i<watches.size();++i){auto& watch=watches[i];watch.owner=this;
                watch.window=i<controls.size()?controls[i]:a.MainWindow();
                trace::Require(watch.window&&SetWindowSubclass(watch.window,Observe,reinterpret_cast<UINT_PTR>(&watch),
                    reinterpret_cast<DWORD_PTR>(&watch)),"attach light slider, numeric EDIT and parent message observer");}
            current=this;app.OriginalTrackbarProc()=Native;attached=true;
        }catch(...){Detach();throw;}}
    void Detach()noexcept{
        if(attached){app.OriginalTrackbarProc()=original;current=nullptr;attached=false;}
        for(auto& watch:watches)if(watch.window&&IsWindow(watch.window))
            RemoveWindowSubclass(watch.window,Observe,reinterpret_cast<UINT_PTR>(&watch));
    }
    ~Messages(){Detach();}
    unsigned Count(HWND window,UINT message,bool native)const noexcept{unsigned count=0;
        for(std::size_t i=0;i<storage->size;++i){const auto& e=storage->events[i];count+=e.window==window&&e.message==message&&e.native==native;}return count;}
    const Watch* Find(HWND window)const noexcept{for(const auto& watch:watches)if(watch.window==window)return &watch;return nullptr;}
    void Send(HWND window,UINT message,WPARAM wp,LPARAM lp){const auto before=Count(window,message,true);
        SendMessageW(window,message,wp,lp);trace::Require(!truncated&&Count(window,message,true)==before+1,
            "each requested native mouse message reaches the original trackbar procedure exactly once");}
    void Write(const std::filesystem::path& path)const{
        std::ofstream f(path);f<<"# fixed 8192-event storage; callback performs scalar recording only; no GDI, text/tick getters or allocation; native returns unmodified\n"
            <<"seq,phase,depth,hwnd,id,msg,wp,lp,native_entry,forwarded,source,source_id,notify,draw_stage,item,item_state,begin,end,result,focus_before,focus_after,capture_before,capture_after\n";
        for(std::size_t i=0;i<storage->size;++i){const auto& e=storage->events[i];f<<e.sequence<<','<<e.phase<<','<<e.depth<<','
            <<reinterpret_cast<std::uintptr_t>(e.window)<<','<<GetDlgCtrlID(e.window)<<','<<e.message<<','<<std::uint64_t(e.wp)<<','<<std::int64_t(e.lp)
            <<','<<e.native<<','<<e.forwarded<<','<<reinterpret_cast<std::uintptr_t>(e.source)<<','<<(e.source?GetDlgCtrlID(e.source):0)<<','
            <<e.notify<<','<<e.drawStage<<','<<e.item<<','<<e.itemState<<','<<e.begin<<','<<e.end<<','<<e.result<<','
            <<reinterpret_cast<std::uintptr_t>(e.focusBefore)<<','<<reinterpret_cast<std::uintptr_t>(e.focusAfter)<<','
            <<reinterpret_cast<std::uintptr_t>(e.captureBefore)<<','<<reinterpret_cast<std::uintptr_t>(e.captureAfter)<<'\n';}
        trace::Require(bool(f),"write light message timeline");
    }
};
struct Image {
    HWND window=nullptr;int id=0,target=0,step=0;unsigned phase=0,paints=0,erases=0,textWrites=0,positionWrites=0;
    std::uint64_t sequence=0;int tick=0;UINT ui=0;bool modified=false,visible=false,windowCopy=false,screenCopy=false,probesOwn=false;
    RECT client{},screenRect{},thumb{},channel{};int clipKind=ERROR;RECT clip{};HWND focus=nullptr,capture=nullptr,foreground=nullptr;
    char text[128]{};std::vector<std::uint32_t> pixels,screen;
};
static std::size_t Different(const Image& before,const Image& after,bool display,RECT region)noexcept{
    if(before.client.right!=after.client.right||before.client.bottom!=after.client.bottom)return 0;
    const auto& a=display?before.screen:before.pixels;const auto& b=display?after.screen:after.pixels;
    if(a.empty()||a.size()!=b.size())return 0;
    const int w=before.client.right,h=before.client.bottom;
    region.left=(std::clamp)(int(region.left),0,w);region.right=(std::clamp)(int(region.right),0,w);
    region.top=(std::clamp)(int(region.top),0,h);region.bottom=(std::clamp)(int(region.bottom),0,h);
    std::size_t count=0;for(int y=region.top;y<region.bottom;++y)for(int x=region.left;x<region.right;++x){
        const auto i=std::size_t(y)*w+x;count+=(a[i]&0xffffffu)!=(b[i]&0xffffffu);}return count;
}
static bool Copy(HDC source,int x,int y,int width,int height,std::vector<std::uint32_t>& pixels){
    trace::Dib dib(width,height,source);if(!dib||!BitBlt(dib.dc,0,0,width,height,source,x,y,SRCCOPY))return false;
    GdiFlush();pixels.assign(dib.pixels,dib.pixels+std::size_t(width)*height);return true;
}
static void Bitmap(const std::filesystem::path& path,const Image& image,bool display){const auto& pixels=display?image.screen:image.pixels;
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+DWORD(pixels.size()*sizeof(std::uint32_t));info.biSize=sizeof(info);info.biWidth=image.client.right;
    info.biHeight=-image.client.bottom;info.biPlanes=1;info.biBitCount=32;
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&file),sizeof(file));
    f.write(reinterpret_cast<const char*>(&info),sizeof(info));f.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()*sizeof(std::uint32_t)));
    trace::Require(bool(f),"write light window/display evidence bitmap");}
struct Samples {
    MMDApp& app;Messages& messages;const std::array<HWND,14>& windows;const Keys& keys;std::vector<Image> images;
    const DWORD begin=GetTickCount();std::size_t bytes=0;unsigned phases=0;std::ofstream& report;trace::Trace& paint;
    Samples(MMDApp& a,Messages& m,const std::array<HWND,14>& w,const Keys& k,std::ofstream& r,trace::Trace& p):
        app(a),messages(m),windows(w),keys(k),report(r),paint(p){images.reserve(kSampleLimit*windows.size());}
    void Budget(){trace::Require(DWORD(GetTickCount()-begin)<20000,"light observation reached its 20 second boundary deadline; incomplete");}
    void Phase(const std::string& name){Budget();trace::Require(++phases<=kSampleLimit,"light sample quota not exceeded");
        messages.phase=phases;paint.Phase(name);report<<"PHASE "<<phases<<' '<<name<<'\n';}
    void Read(int target,int step){
        Budget();Quiet invariant(app);
        report<<"PARAM phase="<<phases;for(const auto value:Values(app))report<<' '<<std::setprecision(9)<<value;report<<'\n';
        for(const auto window:windows){trace::Require(images.size()<kImageLimit,"light image record quota not exceeded");Image image;
            image.window=window;image.id=GetDlgCtrlID(window);image.target=target;image.step=step;image.phase=phases;
            image.sequence=messages.storage->size;image.focus=GetFocus();image.capture=GetCapture();image.foreground=GetForegroundWindow();
            image.visible=IsWindowVisible(window)&&!IsIconic(app.MainWindow());image.ui=UINT(SendMessageW(window,WM_QUERYUISTATE,0,0));
            image.modified=SendMessageW(window,EM_GETMODIFY,0,0)!=0;GetWindowTextA(window,image.text,sizeof(image.text));
            if(Slider(image.id)){image.tick=int(SendMessageW(window,TBM_GETPOS,0,0));
                SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&image.thumb));
                SendMessageW(window,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&image.channel));}
            if(const auto* watch=messages.Find(window)){image.paints=watch->paints;image.erases=watch->erases;
                image.textWrites=watch->sets;image.positionWrites=watch->positions;}
            trace::Require(GetClientRect(window,&image.client)&&GetWindowRect(window,&image.screenRect),"read actual light target geometry");
            const int width=image.client.right,height=image.client.bottom;const auto pixels=std::size_t(width)*height;
            trace::Require(width>0&&height>0&&width<=2048&&height<=512&&pixels*8<=kPixelBytes-bytes,
                "light window pixel dimensions and 64 MiB sample quota are bounded");
            if(HDC dc=GetDC(window)){struct Release{HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}}release{window,dc};
                image.clipKind=GetClipBox(dc,&image.clip);image.windowCopy=Copy(dc,0,0,width,height,image.pixels);}
            POINT origin{};const bool mapped=ClientToScreen(window,&origin)!=FALSE;
            image.probesOwn=mapped&&image.visible;
            for(POINT local:{POINT{0,0},POINT{width-1,0},POINT{0,height-1},POINT{width-1,height-1},POINT{width/2,height/2}}){
                POINT screen{origin.x+local.x,origin.y+local.y};const auto hit=WindowFromPoint(screen);
                image.probesOwn&=hit==window||(hit&&IsChild(window,hit));}
            if(mapped&&image.visible)if(HDC dc=GetDC(nullptr)){struct Release{HDC dc;~Release(){ReleaseDC(nullptr,dc);}}release{dc};
                image.screenCopy=Copy(dc,origin.x,origin.y,width,height,image.screen);}
            bytes+=(image.pixels.size()+image.screen.size())*sizeof(std::uint32_t);images.push_back(std::move(image));
        }
        invariant.Check();keys.Check();
    }
    void Write(const std::filesystem::path& output)const{
        std::ofstream f(output/"light-samples.csv");f<<"# window GetDC pixels and desktop display GetDC pixels are separate; five ownership probes do not prove complete non-occlusion or DWM visibility\n"
            <<"phase,target,step,id,hwnd,last_message_seq,tick,ui,modified,visible,focus,capture,foreground,paint,erase,text_writes,position_writes,text,window_copy,screen_copy,ownership_probes,clip_kind,clip_l,clip_t,clip_r,clip_b,previous_same_id_phase,window_diff,display_diff,window_border_diff,display_border_diff,window_old_thumb_diff,window_new_thumb_diff,window_channel_diff,thumb_l,thumb_t,thumb_r,thumb_b\n";
        std::array<const Image*,14> previous{};unsigned bitmaps=0;
        for(const auto& image:images){std::size_t lane=0;while(lane<kIds.size()&&kIds[lane]!=image.id)++lane;
            const auto* before=lane<previous.size()?previous[lane]:nullptr;std::size_t full=0,screen=0,inner=0,screenInner=0;
            if(before){full=Different(*before,image,false,image.client);screen=Different(*before,image,true,image.client);
                RECT inside{2,2,image.client.right-2,image.client.bottom-2};inner=Different(*before,image,false,inside);screenInner=Different(*before,image,true,inside);}
            f<<image.phase<<','<<image.target<<','<<image.step<<','<<image.id<<','<<reinterpret_cast<std::uintptr_t>(image.window)<<','
                <<image.sequence<<','<<image.tick<<','<<image.ui<<','<<image.modified<<','<<image.visible<<','<<reinterpret_cast<std::uintptr_t>(image.focus)<<','
                <<reinterpret_cast<std::uintptr_t>(image.capture)<<','<<reinterpret_cast<std::uintptr_t>(image.foreground)<<','<<image.paints<<','<<image.erases<<','
                <<image.textWrites<<','<<image.positionWrites<<','<<std::quoted(image.text)<<','<<image.windowCopy<<','<<image.screenCopy<<','<<image.probesOwn<<','
                <<image.clipKind<<','<<image.clip.left<<','<<image.clip.top<<','<<image.clip.right<<','<<image.clip.bottom<<','<<(before?before->phase:0)<<','
                <<full<<','<<screen<<','<<(full-inner)<<','<<(screen-screenInner)<<','<<(before?Different(*before,image,false,before->thumb):0)<<','
                <<(before?Different(*before,image,false,image.thumb):0)<<','<<(before?Different(*before,image,false,image.channel):0)<<','
                <<image.thumb.left<<','<<image.thumb.top<<','<<image.thumb.right<<','<<image.thumb.bottom<<'\n';
            // Keep one moving and one repeated-tick sample per target, rather
            // than exporting thousands of normal snapshots. CSV retains all.
            if((image.step==1||image.step==4)&&(image.id==image.target||image.id==image.target+6)){
                for(bool display:{false,true}){const auto& pixels=display?image.screen:image.pixels;
                    if(!pixels.empty()&&bitmaps<kBitmapLimit){Bitmap(output/(std::to_string(image.phase)+"-"+std::to_string(image.id)+
                        (display?"-display.bmp":"-window.bmp")),image,display);++bitmaps;}}}
            if(lane<previous.size())previous[lane]=&image;
        }
        trace::Require(bool(f),"write light sample ledger");
    }
};
struct CaptureCleanup {
    const std::array<HWND,14>& windows;
    ~CaptureCleanup(){const auto capture=GetCapture();if(capture)for(auto window:windows)if(capture==window){
        SendMessageW(capture,WM_CANCELMODE,0,0);if(GetCapture()==capture)ReleaseCapture();break;}}
};
static void Run(MMDApp* app,const std::filesystem::path& output,bool bound,bool oneSafePoint=true){
    using namespace mikudancestudio;
    trace::Require(app&&app==g_Block&&app->MainWindow()&&app->CameraKeys()&&app->LightKeys()&&app->ShadowKeys()&&app->GravityKeys()&&
        !GetCapture()&&!app->PlaybackActive()&&IsWindowVisible(app->MainWindow())&&!IsIconic(app->MainWindow())&&
        bool(plugins::BoundNativePoseHistory(app))==bound,"light paint diagnosis uses a visible disposable idle real host with the requested binding");
    std::filesystem::create_directories(output);std::ofstream report(output/"light-report.txt");trace::Require(bool(report),"create light paint report");
    report<<"Observation only: six light sliders and numeric EDITs, FOV447/shadow560 unchanged controls. Native mouse input uses SendMessage, not hardware/SendInput.\n"
        <<"No foreground requirement, SetCursorPos, synthetic TBM_SETPOS, forced hover notification, forced invalidation, buffer prototype or custom drawing.\n"
        <<"Each phase samples window GetDC and visible desktop GetDC separately. A copied bitmap or five ownership probes do not prove complete exposure, DWM presentation, flicker duration or absence of flicker.\n"
        <<"Native intermediate erase/customdraw pixels remain in separate native-paint trace; memory DC pixels are not asserted displayed.\n"
        <<"Bounds: 8192 fixed message events, 192 phases, 4096 image records, 64 MiB boundary pixels, 96 boundary BMPs, 20 second boundary deadline. Native synchronous calls cannot be preempted.\n";
    std::array<HWND,14> windows{};for(unsigned i=0;i<kIds.size();++i){windows[i]=GetDlgItem(app->MainWindow(),kIds[i]);
        trace::Require(windows[i]!=nullptr,"all real lighting, FOV and shadow controls exist");
        if(i<12)trace::Require(IsWindowVisible(windows[i]),"all six actual light sliders and numeric EDITs are visible");}
    const Keys keys(*app);CaptureCleanup cleanup{windows};
    const std::array<HWND,7> paintWindows{windows[12],windows[0],windows[1],windows[2],windows[3],windows[4],windows[5]};
    trace::Trace nativePaint(app->MainWindow(),paintWindows);Messages messages(*app,windows);Samples samples(*app,messages,windows,keys,report,nativePaint);
    std::unique_ptr<prior::UiPump> pump;
    const auto flush=[&]{Quiet invariant(*app);UpdateWindow(app->MainWindow());for(auto window:windows)UpdateWindow(window);GdiFlush();invariant.Check();keys.Check();};
    const auto checkpoint=[&](int target,int step,const std::string& label){samples.Phase(label);samples.Read(target,step);};
    const auto safe=[&](int target,int step,const std::string& label){if(!pump)return;Quiet invariant(*app);
        samples.Phase(label);pump->phase=label;pump->Safe();invariant.Check();keys.Check();samples.Read(target,step);};
    try{
        if(oneSafePoint){Quiet invariant(*app);pump=std::make_unique<prior::UiPump>(*app,nativePaint,report);invariant.Check();}
        flush();
        for(unsigned lane=0;lane<6;++lane){auto window=windows[lane];const int id=kIds[lane];const auto prefix=std::to_string(id);
            const auto original=Values(*app);RECT thumb{},channel{};SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(window,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
            trace::Require(thumb.left<thumb.right&&thumb.top<thumb.bottom&&channel.left<channel.right,"native light thumb and channel have positive geometry");
            const int x=(thumb.left+thumb.right)/2,y=(thumb.top+thumb.bottom)/2;const auto first=SendMessageW(window,TBM_GETPOS,0,0);
            checkpoint(id,0,prefix+"-before-hover");
            {Quiet invariant(*app);messages.Send(window,WM_MOUSEMOVE,0,MAKELPARAM(x,y));invariant.Check();keys.Check();}
            checkpoint(id,0,prefix+"-after-native-hover");flush();checkpoint(id,0,prefix+"-after-hover-updatewindow");
            messages.Send(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
            trace::Require(GetCapture()==window,"real native light slider owns capture");keys.Check();
            const auto down=Values(*app);for(unsigned i=0;i<down.size();++i)if(i!=lane)
                trace::Require(!std::memcmp(&original[i],&down[i],sizeof(float)),"native thumb press preserves every unrelated authored parameter");
            if(std::memcmp(&original[lane],&down[lane],sizeof(float)))
                trace::Require(prior::Authored(*app,id,int(SendMessageW(window,TBM_GETPOS,0,0))),"native thumb press uses the exact original light arithmetic if it authors the current tick");
            checkpoint(id,0,prefix+"-after-native-down");
            bool moved=false;int lastX=x;
            for(unsigned step=1;step<=4;++step){const auto before=Values(*app);const auto oldTick=SendMessageW(window,TBM_GETPOS,0,0);
                checkpoint(id,int(step),prefix+"-step-"+std::to_string(step)+"-before-move");
                // Three distinct positions, then repeat exactly the third.
                // The native control chooses ticks; the test never writes them.
                if(step<4){const int fractions[]={1,3,2};lastX=channel.left+(channel.right-channel.left)*fractions[step-1]/4;}
                Quiet unchanged(*app);messages.Send(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(lastX,y));
                const auto tick=int(SendMessageW(window,TBM_GETPOS,0,0));moved|=tick!=first;
                const auto now=Values(*app);for(unsigned i=0;i<now.size();++i)if(i!=lane)
                    trace::Require(!std::memcmp(&before[i],&now[i],sizeof(float)),"light input changes only its authored lane; FOV/shadow/control lanes stay exact");
                trace::Require(prior::Authored(*app,id,tick),"native light tick and exact original RGB or direction arithmetic agree");
                if(step==4){trace::Require(tick==oldTick,"same-point native move keeps the identical light tick");unchanged.Check();}
                trace::Require(GetCapture()==window,"native light capture survives each changing and repeated input");keys.Check();
                checkpoint(id,int(step),prefix+"-step-"+std::to_string(step)+"-after-native");
                flush();checkpoint(id,int(step),prefix+"-step-"+std::to_string(step)+"-after-updatewindow");
                safe(id,int(step),prefix+"-step-"+std::to_string(step)+"-after-one-safepoint");
                trace::Require(GetCapture()==window&&SendMessageW(window,TBM_GETPOS,0,0)==tick&&prior::Authored(*app,id,tick),
                    "drawing and one optional production safe point retain just-authored tick and capture");
            }
            checkpoint(id,5,prefix+"-before-up");{Quiet unchanged(*app);messages.Send(window,WM_LBUTTONUP,0,MAKELPARAM(lastX,y));unchanged.Check();}
            trace::Require(GetCapture()!=window&&moved,"all six native light drags actually change tick and release");keys.Check();
            checkpoint(id,5,prefix+"-after-native-up");flush();checkpoint(id,5,prefix+"-after-release-updatewindow");
            report<<"CONTROL id="<<id<<" tick_before="<<first<<" tick_after="<<SendMessageW(window,TBM_GETPOS,0,0)
                <<" changed_tick="<<moved<<" repeated_tick=1 original_lane="<<original[lane]<<" authored_lane="<<Values(*app)[lane]<<'\n';
        }
        trace::Require(!messages.truncated&&nativePaint.Complete(),"light native paint and fixed timeline finish without quota truncation");keys.Check();samples.Budget();
        messages.Detach();nativePaint.Detach();samples.Write(output);messages.Write(output/"light-messages.csv");nativePaint.Write(output/"native-paint");
        report<<"PASS native-input/data/VMD observation completed; no visual-fix or no-flicker claim. phases="<<samples.phases
            <<" fixed_messages="<<messages.storage->size<<" boundary_pixel_bytes="<<samples.bytes<<" elapsed_ms="<<DWORD(GetTickCount()-samples.begin)<<'\n';
        report.flush();trace::Require(bool(report),"finish light observation report");
    }catch(...){messages.Detach();nativePaint.Detach();
        try{samples.Write(output);messages.Write(output/"light-messages.csv");nativePaint.Write(output/"native-paint");
            report<<"INCOMPLETE failed/interrupted diagnosis; available evidence retained; not a PASS or visual-fix claim.\n";report.flush();}catch(...){}
        throw;
    }
}
} // namespace light_slider_paint_checks
#endif
