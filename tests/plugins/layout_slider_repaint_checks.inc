// Include after slider_paint_trace_host_checks.inc in a disposable full host.
// Test-only RedrawWindow policy comparison. It never replaces native painting.
#include "../slider_paint_trace.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "../../src/window/light_slider_paint_buffer.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#if !MMD_ORIGINAL_PROFILE
namespace layout_slider_repaint_checks {
namespace trace = slider_paint_trace;
namespace prior = slider_paint_trace_host_checks;
namespace buffer = mikumikustudio::ui::light_slider_paint_buffer;
using mikumikustudio::MMDApp;
inline constexpr std::array<int,13> kIds{447,455,456,457,458,459,460,505,510,515,520,534,560};
inline constexpr std::array<int,8> kRequired{447,455,456,457,458,459,460,560};
inline constexpr std::array<int,3> kPanels{2,4,5};
inline constexpr std::array<int,5> kDeltas{1,17,31,4,0};

template<class Key> struct KeyCopy {
    Key* const pointer;
    const std::uint64_t identity;
    const std::vector<Key> bytes;
    explicit KeyCopy(Key* value):pointer(value),identity(mikumikustudio::mdl::KeyPoolIdentity(value)),
        bytes(value,value+mikumikustudio::mdl::KeyPoolCapacity(value)) {}
    void Check(Key* value) const {
        using namespace mikumikustudio::mdl;
        trace::Require(value==pointer && KeyPoolIdentity(value)==identity && KeyPoolCapacity(value)==bytes.size() &&
            !std::memcmp(value,bytes.data(),bytes.size()*sizeof(Key)),
            "layout-only repaint preserves VMD pool identities, capacities and every byte");
    }
};
struct Invariant {
    MMDApp& app;
    const prior::PumpInvariant history;
    KeyCopy<mikumikustudio::mdl::CameraKey> camera;
    KeyCopy<mikumikustudio::mdl::LightKey> light;
    KeyCopy<mikumikustudio::mdl::SelfShadowKey> shadow;
    KeyCopy<mikumikustudio::mdl::GravityKey> gravity;
    const std::array<float,13> additional;
    const std::uint32_t noiseMode;
    const int noise,shadowMode,modified,physics,recorded,cameraMode;
    static std::array<float,13> Additional(MMDApp& a) {
        return {a.CameraPositionX(),a.CameraPositionY(),a.CameraPositionZ(),a.CameraPitch(),a.CameraYaw(),a.CameraRoll(),
            a.CameraDistance(),a.ShadowDistance(),a.GravityX(),a.GravityY(),a.GravityZ(),a.GravityMagnitude(),a.CameraFov()};
    }
    explicit Invariant(MMDApp& value):app(value),history(value),camera(value.CameraKeys()),light(value.LightKeys()),
        shadow(value.ShadowKeys()),gravity(value.GravityKeys()),additional(Additional(value)),noiseMode(value.GravityNoiseModeWord()),
        noise(value.GravityNoise()),shadowMode(value.SelfShadowMode()),modified(value.SceneModified()),
        physics(value.PhysicsResetPending()),recorded(value.RecordedFrameCountByte1()),cameraMode(value.CameraMode()) {}
    void Check() const {
        history.Check();camera.Check(app.CameraKeys());light.Check(app.LightKeys());shadow.Check(app.ShadowKeys());gravity.Check(app.GravityKeys());
        const auto current=Additional(app);
        trace::Require(!std::memcmp(current.data(),additional.data(),sizeof(current)) && app.GravityNoiseModeWord()==noiseMode &&
            app.GravityNoise()==noise && app.SelfShadowMode()==shadowMode && app.SceneModified()==modified &&
            app.PhysicsResetPending()==physics && app.RecordedFrameCountByte1()==recorded && app.CameraMode()==cameraMode,
            "layout-only repaint preserves live authored parameters, mode, dirty, physics and recording state");
    }
};

struct Counts {
    unsigned paint=0,erase=0,nonclient=0,size=0,position=0,paintDuringRelayout=0;
    unsigned directPaint=0,memoryPaint=0,directDuringRelayout=0,memoryDuringRelayout=0;
};
static Counts Difference(const Counts& after,const Counts& before) {
    return {after.paint-before.paint,after.erase-before.erase,after.nonclient-before.nonclient,
        after.size-before.size,after.position-before.position,after.paintDuringRelayout-before.paintDuringRelayout,
        after.directPaint-before.directPaint,after.memoryPaint-before.memoryPaint,
        after.directDuringRelayout-before.directDuringRelayout,after.memoryDuringRelayout-before.memoryDuringRelayout};
}
struct NativeEvent {
    LONGLONG qpc=0;
    unsigned pass=0,panel=0,step=0;
    int id=0;UINT message=0;WPARAM wp=0;LPARAM lp=0;LRESULT result=0;
    DWORD dcType=0,nativeError=0;bool reference=false,relayout=false,mainCapture=false,returned=false;
};
struct RedrawEvent {
    unsigned pass=0,panel=0,step=0;
    int id=0;UINT requested=0,effective=0;BOOL result=FALSE;
    bool reference=false,qualified=false,policyActive=false,rectangle=false,region=false,returned=false;
};
struct UpdateEvent {LONGLONG qpc=0;unsigned pass=0,panel=0,step=0;int id=0,kind=ERROR;bool reference=false,relayout=false,mainCapture=false;};

class Probe {
    using RedrawFn=BOOL(WINAPI*)(HWND,const RECT*,HRGN,UINT);
    inline static RedrawFn originalRedraw_=nullptr;
    inline static WNDPROC nativeFallback_=nullptr;
    inline static thread_local Probe* active_=nullptr;
    bool created_=false,enabled_=false,closed_=false;
    WNDPROC native_=nullptr;
    static constexpr std::size_t kEvents=8192;
public:
    MMDApp& app;
    std::array<HWND,13> windows{};
    std::array<Counts,13> counts{};
    std::unique_ptr<NativeEvent[]> nativeEvents=std::make_unique<NativeEvent[]>(kEvents);
    std::unique_ptr<RedrawEvent[]> redrawEvents=std::make_unique<RedrawEvent[]>(kEvents);
    std::unique_ptr<UpdateEvent[]> updateEvents=std::make_unique<UpdateEvent[]>(kEvents);
    std::size_t nativeCount=0,redrawCount=0,updateCount=0;
    bool overflow=false,candidate=false,reference=false;
    unsigned pass=0,panel=0,step=0,changedFlags=0,unexpectedFlags=0;
    MH_STATUS disableResult=MH_OK,removeResult=MH_OK;
    explicit Probe(MMDApp& value):native_(value.OriginalTrackbarProc()),app(value) {
        trace::Require(!active_ && native_,"one test-only real native slider repaint observer");
        for(unsigned i=0;i<kIds.size();++i){
            windows[i]=GetDlgItem(app.MainWindow(),kIds[i]);wchar_t name[64]{};
            trace::Require(windows[i] && IsWindow(windows[i]) && GetClassNameW(windows[i],name,64) &&
                !_wcsicmp(name,TRACKBAR_CLASSW) && GetWindowThreadProcessId(windows[i],nullptr)==GetCurrentThreadId(),
                "observe the thirteen actual native trackbar HWNDs on the current UI thread");
        }
        try {
            trace::Require(MH_CreateHook(reinterpret_cast<void*>(&RedrawWindow),reinterpret_cast<void*>(Redraw),
                reinterpret_cast<void**>(&originalRedraw_))==MH_OK,"create isolated test-only RedrawWindow candidate hook");
            created_=true;active_=this;nativeFallback_=native_;
            app.OriginalTrackbarProc()=Native;
            trace::Require(MH_EnableHook(reinterpret_cast<void*>(&RedrawWindow))==MH_OK,
                "enable isolated test-only RedrawWindow candidate hook");
            enabled_=true;
        } catch(...) {Close();throw;}
    }
    Probe(const Probe&)=delete;Probe& operator=(const Probe&)=delete;
    ~Probe(){Close();}
    void Close() noexcept {
        if(closed_)return;closed_=true;
        candidate=false;
        if(app.OriginalTrackbarProc()==Native)app.OriginalTrackbarProc()=native_;
        active_=nullptr;
        if(enabled_){disableResult=MH_DisableHook(reinterpret_cast<void*>(&RedrawWindow));enabled_=false;}
        if(created_){removeResult=MH_RemoveHook(reinterpret_cast<void*>(&RedrawWindow));created_=false;}
    }
    int Index(HWND window) const noexcept {
        for(unsigned i=0;i<windows.size();++i)if(windows[i]==window)return int(i);return -1;
    }
    static HRGN WINAPI FailRegion(int,int,int,int) {return nullptr;}
    static int WINAPI ReadUpdate(HWND window,HRGN region,BOOL erase) {
        const DWORD beforeError=GetLastError();auto* self=active_;SetLastError(beforeError);
        const int result=GetUpdateRgn(window,region,erase);const DWORD nativeError=GetLastError();
        if(self){const int index=self->Index(window);if(index>=0){
            if(self->updateCount<kEvents){auto& e=self->updateEvents[self->updateCount++];LARGE_INTEGER tick{};QueryPerformanceCounter(&tick);e.qpc=tick.QuadPart;
                e.pass=self->pass;e.panel=self->panel;e.step=self->step;
                e.id=kIds[unsigned(index)];e.kind=result;e.reference=self->reference;e.relayout=self->app.bottomPanelSizing.relayout;
                e.mainCapture=GetCapture()==self->app.MainWindow();}else self->overflow=true;}}
        SetLastError(nativeError);return result;
    }
    static BOOL WINAPI Redraw(HWND window,const RECT* rectangle,HRGN region,UINT flags) {
        const DWORD entryError=GetLastError();auto* self=active_;UINT effective=flags;
        RedrawEvent* event=nullptr;
        if(self){const int index=self->Index(window);const bool qualified=index>=0;
            if(self->candidate && qualified){effective&=~UINT(RDW_ERASE|RDW_FRAME);
                self->changedFlags+=effective!=flags;
                self->unexpectedFlags+=(effective!=(flags&~UINT(RDW_ERASE|RDW_FRAME)));}
            if(self->redrawCount<kEvents){event=&self->redrawEvents[self->redrawCount++];
                event->pass=self->pass;event->panel=self->panel;event->step=self->step;
                event->id=qualified?kIds[unsigned(index)]:GetDlgCtrlID(window);
                event->requested=flags;event->effective=effective;event->reference=self->reference;event->qualified=qualified;event->policyActive=self->candidate;
                event->rectangle=rectangle!=nullptr;event->region=region!=nullptr;
            }else self->overflow=true;
        }
        SetLastError(entryError);
        const BOOL result=originalRedraw_(window,rectangle,region,effective);
        const DWORD nativeError=GetLastError();if(event){event->result=result;event->returned=true;}
        SetLastError(nativeError);return result;
    }
    static LRESULT CALLBACK Native(HWND window,UINT message,WPARAM wp,LPARAM lp) {
        const DWORD entryError=GetLastError();auto* self=active_;NativeEvent* event=nullptr;
        if(self){const int index=self->Index(window);
            if(index>=0){auto& count=self->counts[unsigned(index)];
                const bool interesting=message==WM_PAINT || message==WM_ERASEBKGND || message==WM_NCPAINT ||
                    message==WM_SIZE || message==WM_WINDOWPOSCHANGED;
                if(message==WM_PAINT){++count.paint;count.paintDuringRelayout+=self->app.bottomPanelSizing.relayout;
                    const auto type=wp?GetObjectType(reinterpret_cast<HDC>(wp)):0;
                    count.directPaint+=wp==0;count.memoryPaint+=type==OBJ_MEMDC;
                    count.directDuringRelayout+=(wp==0 && self->app.bottomPanelSizing.relayout);
                    count.memoryDuringRelayout+=(type==OBJ_MEMDC && self->app.bottomPanelSizing.relayout);}
                else if(message==WM_ERASEBKGND)++count.erase;
                else if(message==WM_NCPAINT)++count.nonclient;
                else if(message==WM_SIZE)++count.size;
                else if(message==WM_WINDOWPOSCHANGED)++count.position;
                if(interesting){if(self->nativeCount<kEvents){event=&self->nativeEvents[self->nativeCount++];LARGE_INTEGER tick{};QueryPerformanceCounter(&tick);
                    event->qpc=tick.QuadPart;event->pass=self->pass;event->panel=self->panel;event->step=self->step;
                    event->id=kIds[unsigned(index)];event->message=message;event->wp=wp;event->lp=lp;
                    event->reference=self->reference;event->relayout=self->app.bottomPanelSizing.relayout;
                    event->mainCapture=GetCapture()==self->app.MainWindow();
                    if((message==WM_ERASEBKGND || message==WM_PAINT) && wp)event->dcType=GetObjectType(reinterpret_cast<HDC>(wp));
                }else self->overflow=true;}
            }
        }
        SetLastError(entryError);
        // Exactly one original ANSI native call, including nested SIZE painting.
        const auto result=CallWindowProcA(self?self->native_:nativeFallback_,window,message,wp,lp);
        const DWORD nativeError=GetLastError();if(event){event->result=result;event->nativeError=nativeError;event->returned=true;}
        SetLastError(nativeError);return result;
    }
};

static trace::Snapshot Pixels(HWND window) {
    trace::Snapshot result;result.window=window;result.id=GetDlgCtrlID(window);RECT client{};
    trace::Require(GetClientRect(window,&client) && client.left==0 && client.top==0 && client.right>0 && client.bottom>0 &&
        client.right<=2048 && client.bottom<=512,"bounded actual dynamically sized trackbar client");
    result.width=client.right;result.height=client.bottom;result.visible=IsWindowVisible(window)!=FALSE;
    const HDC display=GetDC(window);trace::Require(display!=nullptr,"read actual dynamic slider window DC");
    struct Release {HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}}release{window,display};
    result.dcType=GetObjectType(display);result.mapMode=GetMapMode(display);result.clipKind=GetClipBox(display,&result.clip);
    trace::Require(result.dcType==OBJ_DC && result.mapMode==MM_TEXT && result.clipKind==SIMPLEREGION &&
        result.clip.left<=0 && result.clip.top<=0 && result.clip.right>=client.right && result.clip.bottom>=client.bottom,
        "INCOMPLETE full actual dynamic slider client must be readable without a clipping override");
    trace::Dib dib(client.right,client.bottom,display);
    trace::Require(bool(dib) && BitBlt(dib.dc,0,0,client.right,client.bottom,display,0,0,SRCCOPY) && GdiFlush(),
        "copy actual dynamic native window pixels without supplying a paint HDC");
    result.pixels.assign(dib.pixels,dib.pixels+std::size_t(client.right)*client.bottom);result.copied=true;
    result.tick=int(SendMessageW(window,TBM_GETPOS,0,0));
    trace::Require(result.Live() && result.Varied(),"INCOMPLETE native dynamic slider evidence is visible and nonuniform");
    return result;
}
static std::size_t PixelDifference(const trace::Snapshot& a,const trace::Snapshot& b) {
    trace::Require(a.copied && b.copied && a.width==b.width && a.height==b.height && a.pixels.size()==b.pixels.size(),
        "same actual HWND dynamic geometry is required for full RGB comparison");
    return trace::Changed(a,b);
}
static void Bitmap(const std::filesystem::path& path,const trace::Snapshot& image) {
    BITMAPFILEHEADER file{};BITMAPINFOHEADER header{};file.bfType=0x4d42;
    file.bfOffBits=sizeof(file)+sizeof(header);file.bfSize=file.bfOffBits+DWORD(image.pixels.size()*4);
    header.biSize=sizeof(header);header.biWidth=image.width;header.biHeight=-image.height;
    header.biPlanes=1;header.biBitCount=32;header.biCompression=BI_RGB;
    std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(&file),sizeof(file));
    stream.write(reinterpret_cast<const char*>(&header),sizeof(header));
    stream.write(reinterpret_cast<const char*>(image.pixels.data()),std::streamsize(image.pixels.size()*4));
    trace::Require(bool(stream),"write bounded dynamic slider pixel mismatch evidence");
}

struct Session {
    MMDApp& app;const ULONGLONG started=GetTickCount64();const HWND focus=GetFocus();
    const decltype(MMDApp::bottomPanelSizing) saved;
    const unsigned char ready;
    struct Window { HWND handle=nullptr;RECT bounds{};LRESULT tick=0,ui=0;LONG_PTR style=0;bool prepared=false; };
    std::array<Window,13> windows{};
    bool gesture=false;
    explicit Session(MMDApp& value):app(value),saved(value.bottomPanelSizing),ready(value.WindowLayoutReady()) {
        for(unsigned i=0;i<kIds.size();++i){auto& item=windows[i];item.handle=GetDlgItem(app.MainWindow(),kIds[i]);
            trace::Require(item.handle && GetWindowRect(item.handle,&item.bounds),"capture actual pre-test native slider geometry");
            item.tick=SendMessageW(item.handle,TBM_GETPOS,0,0);item.ui=SendMessageW(item.handle,WM_QUERYUISTATE,0,0);
            item.style=GetWindowLongPtrW(item.handle,GWL_STYLE);}
    }
    void Guard() const {
        trace::Require(GetTickCount64()-started<20000,"INCOMPLETE layout repaint comparison exceeded its 20-second budget");
        trace::Require(!GetCapture() || (gesture && GetCapture()==app.MainWindow()),
            "INCOMPLETE layout repaint comparison never takes or releases a foreign capture");
        BYTE keyboard[256]{};trace::Require(GetKeyboardState(keyboard)!=FALSE,"read current UI-thread key state before synthetic layout messages");
        for(int key=1;key<=255;++key)trace::Require(!(keyboard[key]&0x80) && !(GetAsyncKeyState(key)&0x8000),
            "INCOMPLETE a real held key/button interrupts isolated layout repaint comparison");
    }
    void Cancel() noexcept {
        if(gesture && IsWindow(app.MainWindow())){
            // The production cancel path restores this gesture's original extra.
            mikumikustudio::HandleBottomPanelResize(&app,WM_CANCELMODE,0,0);
            if(GetCapture()==app.MainWindow())ReleaseCapture();
        }
        gesture=false;
    }
    void PrepareCues() {
        // Layout-buffer admission is tested in an explicit hidden-focus mouse
        // state. Preserve every pre-test UI bit and restore them on all exits.
        for(auto& item:windows)if(IsWindowVisible(item.handle)){
            item.prepared=true;
            if(!(item.ui&UISF_HIDEFOCUS))SendMessageW(item.handle,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
            trace::Require((SendMessageW(item.handle,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)!=0,
                "prepare actual per-control hidden-focus mouse cues for layout buffer admission");
            trace::Require(RedrawWindow(item.handle,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW)!=FALSE,
                "complete initial native mouse-state reference before measuring any layout gesture");
        }
    }
    void RestoreCues() noexcept {
        for(auto& item:windows)if(item.prepared && IsWindow(item.handle)){
            const auto current=SendMessageW(item.handle,WM_QUERYUISTATE,0,0);
            const auto set=UINT(item.ui&~current),clear=UINT(current&~item.ui);
            if(set)SendMessageW(item.handle,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,set),0);
            if(clear)SendMessageW(item.handle,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,clear),0);
            item.prepared=false;
            RedrawWindow(item.handle,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW);
        }
    }
    ~Session(){Cancel();RestoreCues();}
    void CheckRestored() const {
        trace::Require(app.bottomPanelSizing.relayout==saved.relayout &&
            app.bottomPanelSizing.extra==saved.extra &&
            app.bottomPanelSizing.dragging==saved.dragging && app.bottomPanelSizing.startX==saved.startX &&
            app.bottomPanelSizing.startExtra==saved.startExtra && app.WindowLayoutReady()==ready &&
            GetFocus()==focus && !GetCapture(),"restore exact original bottom sizing, layout readiness, focus and capture");
        for(unsigned i=0;i<kIds.size();++i){const auto& item=windows[i];RECT bounds{};
            trace::Require(IsWindow(item.handle) && GetDlgItem(app.MainWindow(),kIds[i])==item.handle &&
                GetWindowRect(item.handle,&bounds) && EqualRect(&bounds,&item.bounds) &&
                SendMessageW(item.handle,TBM_GETPOS,0,0)==item.tick &&
                SendMessageW(item.handle,WM_QUERYUISTATE,0,0)==(item.ui|(item.prepared?UISF_HIDEFOCUS:0)) &&
                GetWindowLongPtrW(item.handle,GWL_STYLE)==item.style,
                "restore actual native slider identities, geometry, tick, UI cues and full styles");}
    }
};
struct Result {
    unsigned pass=0,panel=0,step=0;int delta=0,id=0;RECT bounds{};
    trace::Snapshot actual;
    std::size_t referenceDifference=0,baselineDifference=0;
    Counts layout{},reference{};
};
static RECT Bounds(HWND parent,HWND window) {
    RECT rectangle{};trace::Require(GetWindowRect(window,&rectangle)!=FALSE,"read actual final native slider bounds");
    MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rectangle),2);return rectangle;
}
static const Result* FindBaseline(const std::vector<Result>& results,unsigned panel,unsigned step,int id) {
    for(const auto& result:results)if(result.pass==0 && result.panel==panel && result.step==step && result.id==id)return &result;
    return nullptr;
}
static void WriteEvents(const std::filesystem::path& output,const Probe& probe) {
    std::ofstream native(output/"layout-slider-native.csv"),redraw(output/"layout-slider-redraw.csv"),updates(output/"layout-slider-updates.csv");
    trace::Require(bool(native)&&bool(redraw)&&bool(updates),"open fixed native, update-region and RedrawWindow layout ledgers");
    native<<"qpc,pass,panel,step,reference,id,message,wparam,lparam,relayout,main_capture,dc_type,result,native_error,returned\n";
    for(std::size_t i=0;i<probe.nativeCount;++i){const auto& e=probe.nativeEvents[i];
        native<<e.qpc<<','<<e.pass<<','<<e.panel<<','<<e.step<<','<<e.reference<<','<<e.id<<','<<e.message<<','<<e.wp<<','<<e.lp<<','
            <<e.relayout<<','<<e.mainCapture<<','<<e.dcType<<','<<e.result<<','<<e.nativeError<<','<<e.returned<<'\n';}
    redraw<<"pass,panel,step,reference,id,qualified,policy_active,rectangle,region,requested,effective,result,returned\n";
    for(std::size_t i=0;i<probe.redrawCount;++i){const auto& e=probe.redrawEvents[i];
        redraw<<e.pass<<','<<e.panel<<','<<e.step<<','<<e.reference<<','<<e.id<<','<<e.qualified<<','<<e.policyActive<<','<<e.rectangle<<','<<e.region<<','
            <<e.requested<<','<<e.effective<<','<<e.result<<','<<e.returned<<'\n';}
    updates<<"qpc,pass,panel,step,reference,id,region_kind,relayout,main_capture\n";
    for(std::size_t i=0;i<probe.updateCount;++i){const auto& e=probe.updateEvents[i];
        updates<<e.qpc<<','<<e.pass<<','<<e.panel<<','<<e.step<<','<<e.reference<<','<<e.id<<','<<e.kind<<','<<e.relayout<<','<<e.mainCapture<<'\n';}
    trace::Require(bool(native)&&bool(redraw)&&bool(updates),"write all bounded observations after callbacks return");
}

static void Run(MMDApp& app,const std::filesystem::path& output) {
    trace::Require(&app==mikumikustudio::g_Block && app.MainWindow() && IsWindowVisible(app.MainWindow()) && !IsIconic(app.MainWindow()) &&
        app.CameraMode()!=0 && !app.FloatingWindow() && !app.FullscreenMode() && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.SidebarResizeDragging() && app.bottomPanelSizing.dragging<0 && !app.bottomPanelSizing.relayout && !GetCapture() &&
        app.CameraKeys() && app.LightKeys() && app.ShadowKeys() && app.GravityKeys(),
        "layout repaint comparison requires an idle visible disposable camera-mode actual MMS host");
    RECT client{};trace::Require(GetClientRect(app.MainWindow(),&client) && client.right>=1380 && client.bottom>=400,
        "INCOMPLETE use at least 1380 logical client pixels for real bottom-panel expansion");
    for(int id:kRequired)trace::Require(IsWindowVisible(GetDlgItem(app.MainWindow(),id))!=FALSE,
        "INCOMPLETE required FOV, six lighting and shadow native sliders must actually be visible");
    for(int id:kIds)trace::Require(GetFocus()!=GetDlgItem(app.MainWindow(),id),
        "INCOMPLETE enter layout diagnosis with focus outside native slider controls");
    const auto spans=mikumikustudio::BottomPanelSpans(&app);
    int used=0;for(const auto& p:spans)used+=p.extra;
    trace::Require(!spans.empty() && client.right-3-spans.back().right-used>=31,
        "INCOMPLETE bottom layout must have at least 31 actual spare pixels beyond existing expansions");
    for(int panel:kPanels){const auto found=std::find_if(spans.begin(),spans.end(),[&](const auto& p){return p.id==panel;});
        trace::Require(found!=spans.end() && found->right-found->left>20,
            "INCOMPLETE camera, lighting and shadow panels must be expanded for real edge dragging");}
    std::filesystem::create_directories(output);
    std::ofstream report(output/"layout-slider-repaint-report.txt"),rows(output/"layout-slider-results.csv");
    trace::Require(bool(report)&&bool(rows),"open actual dynamic layout repaint comparison output");
    report<<"Test-only RedrawWindow candidate removes RDW_ERASE|RDW_FRAME only on the thirteen captured real native trackbar HWNDs. "
        "Original native WNDPROC is called once per message; no supplied paint HDC, customdraw return, WM_SIZE drawing or move flags are replaced. "
        "Pass0=pure native, pass1=erase/frame-bit candidate with buffer preparation forced to fail, pass2=normal layout buffer and unchanged redraw flags. "
        "Only createRectRgn is forced to fail in passes0/1 and each full native reference; normal pass2 records original GetUpdateRgn returns without changing them. "
        "Real HandleBottomPanelResize down/move/cancel drives panel2,4,5 at deltas1,17,31,4,0. "
        "Each actual final window DC is compared with a full ERASE|FRAME native repaint on the same HWND/geometry; candidate and baseline are also compared. "
        "No SafePoint, general message pumping, file load/save or authored operation is requested. This is final-image/erase evidence, never hardware/DWM/no-flicker PASS.\n";
    rows<<"pass,panel,step,delta,id,left,top,right,bottom,width,height,reference_rgb_diff,baseline_rgb_diff,layout_paint,layout_erase,layout_ncpaint,layout_size,layout_position,layout_paint_during_relayout,reference_paint,reference_erase,reference_ncpaint,direct_paint,memory_paint,direct_SIZE_paint,memory_SIZE_paint\n";
    Session session(app);session.Guard();const Invariant invariant(app);Probe probe(app);
    std::vector<Result> results;results.reserve(3*kPanels.size()*kDeltas.size()*kIds.size());
    std::array<unsigned,13> changedGeometry{},bufferMemoryByControl{};std::array<bool,13> observed{};
    std::size_t failed=0,baselineErases=0,candidateErases=0,baselineNonclient=0,candidateNonclient=0,firstSizePaints=0,retainedBytes=0,artifactPairs=0;
    std::size_t bufferMemory=0,bufferMemorySize=0,bufferDirect=0,bufferDirectSize=0;
    try {
        session.PrepareCues();invariant.Check();
        buffer::GdiFunctions normal;normal.getUpdateRgn=Probe::ReadUpdate;
        buffer::GdiFunctions direct=normal;direct.createRectRgn=Probe::FailRegion;
        for(unsigned pass=0;pass<3;++pass)for(int panel:kPanels){
            buffer::ScopedFunctionsForTesting passFunctions(pass<2?direct:normal);
            session.Guard();invariant.Check();probe.pass=pass;probe.panel=unsigned(panel);probe.step=0;probe.reference=false;probe.candidate=pass==1;
            const auto currentSpans=mikumikustudio::BottomPanelSpans(&app);
            const auto edge=std::find_if(currentSpans.begin(),currentSpans.end(),[&](const auto& p){return p.id==panel;});
            trace::Require(edge!=currentSpans.end(),"find current production bottom-panel edge");
            const int x=mikumikustudio::BottomPanelX(&app,edge->right),y=client.bottom-150;
            trace::Require(mikumikustudio::HitBottomPanelEdge(&app,x,y)==panel,"production hit-test identifies the intended real bottom edge");
            session.gesture=true;
            trace::Require(mikumikustudio::HandleBottomPanelResize(&app,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y)) &&
                app.bottomPanelSizing.dragging==panel && GetCapture()==app.MainWindow(),"actual bottom edge begins production main-window capture");
            const auto beforeBounds=[&]{std::array<RECT,13> value{};for(unsigned i=0;i<kIds.size();++i)value[i]=Bounds(app.MainWindow(),probe.windows[i]);return value;}();
            for(unsigned step=0;step<kDeltas.size();++step){
                session.Guard();probe.step=step;probe.reference=false;probe.candidate=pass==1;
                const auto before=probe.counts;
                trace::Require(mikumikustudio::HandleBottomPanelResize(&app,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+kDeltas[step],y)),
                    "actual production bottom mouse move applies its live layout");
                trace::Require(GetCapture()==app.MainWindow() && app.bottomPanelSizing.dragging==panel,
                    "layout comparison preserves actual main capture while resizing");
                trace::Require(GdiFlush()!=FALSE,"flush actual final geometry painting before comparison");
                invariant.Check();const auto afterLayout=probe.counts;
                for(unsigned i=0;i<kIds.size();++i){const auto window=probe.windows[i];if(!IsWindowVisible(window))continue;
                    Result result;result.pass=pass;result.panel=unsigned(panel);result.step=step;result.delta=kDeltas[step];result.id=kIds[i];
                    result.bounds=Bounds(app.MainWindow(),window);result.actual=Pixels(window);result.layout=Difference(afterLayout[i],before[i]);
                    observed[i]=true;changedGeometry[i]+=!EqualRect(&beforeBounds[i],&result.bounds);
                    const auto beforeReference=probe.counts[i];probe.candidate=false;probe.reference=true;
                    {buffer::ScopedFunctionsForTesting pureNative(direct);
                        trace::Require(RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW)!=FALSE,
                            "draw complete unbuffered native ERASE|FRAME reference on the same actual dynamic HWND");}
                    trace::Require(GdiFlush()!=FALSE,"flush complete final native reference");
                    const auto reference=Pixels(window);result.reference=Difference(probe.counts[i],beforeReference);
                    result.referenceDifference=PixelDifference(result.actual,reference);probe.reference=false;probe.candidate=pass==1;
                    trace::Require(result.reference.memoryPaint==0 && (pass==2 || result.layout.memoryPaint==0),
                        "forced preparation failure keeps passes0/1 and every full native reference unbuffered");
                    if(pass){const auto* baseline=FindBaseline(results,unsigned(panel),step,result.id);
                        trace::Require(baseline && EqualRect(&baseline->bounds,&result.bounds),"candidate repeats the exact baseline native window geometry");
                        result.baselineDifference=PixelDifference(baseline->actual,result.actual);}
                    failed+=result.referenceDifference!=0 || result.baselineDifference!=0;
                    if((result.referenceDifference || result.baselineDifference) && artifactPairs<8){++artifactPairs;
                        const auto stem=std::string(pass==2?"buffer":pass==1?"erase-candidate":"baseline")+"-panel"+std::to_string(panel)+"-step"+std::to_string(step)+"-id"+std::to_string(result.id);
                        Bitmap(output/(stem+"-actual.bmp"),result.actual);Bitmap(output/(stem+"-full-reference.bmp"),reference);
                    }
                    if(pass==1){candidateErases+=result.layout.erase;candidateNonclient+=result.layout.nonclient;}
                    else if(pass==0){baselineErases+=result.layout.erase;baselineNonclient+=result.layout.nonclient;}
                    else {bufferMemory+=result.layout.memoryPaint;bufferMemorySize+=result.layout.memoryDuringRelayout;
                        bufferDirect+=result.layout.directPaint;bufferDirectSize+=result.layout.directDuringRelayout;
                        bufferMemoryByControl[i]+=result.layout.memoryPaint;}
                    firstSizePaints+=result.layout.paintDuringRelayout;
                    rows<<result.pass<<','<<result.panel<<','<<result.step<<','<<result.delta<<','<<result.id<<','
                        <<result.bounds.left<<','<<result.bounds.top<<','<<result.bounds.right<<','<<result.bounds.bottom<<','
                        <<result.actual.width<<','<<result.actual.height<<','<<result.referenceDifference<<','<<result.baselineDifference<<','
                        <<result.layout.paint<<','<<result.layout.erase<<','<<result.layout.nonclient<<','<<result.layout.size<<','<<result.layout.position<<','
                        <<result.layout.paintDuringRelayout<<','<<result.reference.paint<<','<<result.reference.erase<<','<<result.reference.nonclient<<','
                        <<result.layout.directPaint<<','<<result.layout.memoryPaint<<','<<result.layout.directDuringRelayout<<','<<result.layout.memoryDuringRelayout<<'\n';
                    const auto bytes=result.actual.pixels.size()*sizeof(std::uint32_t);
                    trace::Require(bytes<=32u*1024u*1024u && retainedBytes<=32u*1024u*1024u-bytes,
                        "INCOMPLETE retained dynamic layout comparison pixels reached their 32 MiB quota");
                    retainedBytes+=bytes;results.push_back(std::move(result));
                }
                invariant.Check();trace::Require(!probe.overflow,"fixed native and redraw ledgers remain within their declared quota");
            }
            probe.reference=false;probe.candidate=false;session.Cancel();
            // Cancel restores original extra, but transient gesture anchor fields
            // are deliberately not semantic layout. Restore their exact snapshot.
            app.bottomPanelSizing=session.saved;invariant.Check();session.CheckRestored();
        }
        probe.candidate=false;probe.reference=false;session.RestoreCues();probe.Close();
        trace::Require((probe.disableResult==MH_OK || probe.disableResult==MH_ERROR_DISABLED) && probe.removeResult==MH_OK,
            "remove the test-only RedrawWindow hook and restore original native forwarding");
        invariant.Check();session.CheckRestored();WriteEvents(output,probe);
        for(std::size_t i=0;i<probe.redrawCount;++i){const auto& event=probe.redrawEvents[i];
            const UINT expected=event.policyActive && event.qualified ? event.requested&~UINT(RDW_ERASE|RDW_FRAME):event.requested;
            trace::Require(event.returned && event.effective==expected,"every actual RedrawWindow call changes precisely the two qualified candidate bits");}
        for(std::size_t i=0;i<probe.nativeCount;++i)trace::Require(probe.nativeEvents[i].returned,
            "all captured native layout messages completed their exactly-once original forwarding");
        for(unsigned i=0;i<kIds.size();++i){report<<"CONTROL id="<<kIds[i]<<" visible_observed="<<observed[i]<<" changed_geometry_samples="<<changedGeometry[i]
            <<" native_buffer_memory_paints="<<bufferMemoryByControl[i]<<'\n';
            if(std::find(kRequired.begin(),kRequired.end(),kIds[i])!=kRequired.end())
                trace::Require(observed[i] && changedGeometry[i]>0 && bufferMemoryByControl[i]>0,
                    "every required actual slider undergoes dynamic geometry and normal layout-buffer native memory painting");}
        report<<"SUMMARY compared="<<results.size()<<" failed_final_rgb="<<failed<<" candidate_changed_flags="<<probe.changedFlags
            <<" unexpected_effective_flags="<<probe.unexpectedFlags<<" baseline_layout_erase="<<baselineErases
            <<" candidate_layout_erase="<<candidateErases<<" baseline_layout_ncpaint="<<baselineNonclient
            <<" candidate_layout_ncpaint="<<candidateNonclient<<" native_paint_during_SIZE_relayout="<<firstSizePaints
            <<" buffer_memory_paint="<<bufferMemory<<" buffer_memory_SIZE_paint="<<bufferMemorySize
            <<" buffer_direct_paint="<<bufferDirect<<" buffer_direct_SIZE_paint="<<bufferDirectSize
            <<" erase_reduced="<<(candidateErases<baselineErases)<<" elapsed_ms="<<GetTickCount64()-session.started
            <<" retained_pixel_bytes="<<retainedBytes<<" mismatch_bmp_pairs="<<artifactPairs<<" full_state_restored=1 hook_removed=1\n";
        report<<(failed?"FAILED":"PASS_FINAL_PIXELS_ONLY")<<" no hardware/no-flicker assertion; hidden controls (including534 if absent above) are not covered.\n";
        report.flush();rows.flush();trace::Require(bool(report)&&bool(rows),"write actual layout comparison summaries");
        trace::Require(probe.changedFlags>0 && !probe.unexpectedFlags && !probe.overflow,
            "candidate actually removed only the requested erase/frame bits from native layout redraws");
        trace::Require(bufferMemory>0,"normal layout-buffer pass must actually reach native painting with a memory HDC");
        trace::Require(failed==0,"dynamic actual-window candidate and baseline must each equal full native repaint with no pixel exemptions");
    } catch(const std::exception& error) {
        probe.candidate=false;probe.reference=false;session.Cancel();app.bottomPanelSizing=session.saved;session.RestoreCues();probe.Close();
        try{invariant.Check();session.CheckRestored();WriteEvents(output,probe);}
        catch(const std::exception& restoreError){report<<"FAILED restoration="<<restoreError.what()<<'\n';report.flush();throw;}
        report<<"INCOMPLETE_OR_FAILED reason="<<error.what()<<" known_pixel_failures="<<failed<<" elapsed_ms="<<GetTickCount64()-session.started
            <<" disable_result="<<int(probe.disableResult)<<" remove_result="<<int(probe.removeResult)<<" full_state_restored=1\n";
        report.flush();throw;
    }
}
} // namespace layout_slider_repaint_checks
#endif
