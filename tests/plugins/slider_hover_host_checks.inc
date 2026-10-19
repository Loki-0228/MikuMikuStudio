// Standalone visible, disposable-host diagnostic. Include after the existing
// slider paint checks; this never changes their input policy or stage verdict.
#include "../slider_paint_trace.hpp"

#if !MMD_ORIGINAL_PROFILE
namespace slider_hover_host_checks {
namespace trace=slider_paint_trace;
namespace prior=slider_paint_trace_host_checks;
using mikudancestudio::MMDApp;
static bool Near(POINT a,POINT b) noexcept {return std::abs(a.x-b.x)<=1&&std::abs(a.y-b.y)<=1;}
static bool Inside(const RECT& a,const RECT& b) noexcept {
    return b.left>=a.left&&b.top>=a.top&&b.right<=a.right&&b.bottom<=a.bottom&&b.left<b.right&&b.top<b.bottom;
}
static void DpiAudit(HWND window,std::ofstream& report) {
    const auto user=GetModuleHandleW(L"user32.dll");
    using WindowContext=HANDLE(WINAPI*)(HWND);
    using ContextAwareness=int(WINAPI*)(HANDLE);
    using WindowDpi=UINT(WINAPI*)(HWND);
    const auto context=user?reinterpret_cast<WindowContext>(GetProcAddress(user,"GetWindowDpiAwarenessContext")):nullptr;
    const auto awareness=user?reinterpret_cast<ContextAwareness>(GetProcAddress(user,"GetAwarenessFromDpiAwarenessContext")):nullptr;
    const auto dpi=user?reinterpret_cast<WindowDpi>(GetProcAddress(user,"GetDpiForWindow")):nullptr;
    wchar_t requested[2]{};const bool unawareRequested=GetEnvironmentVariableW(L"MMS_TEST_SLIDER_UNAWARE",requested,2)==1&&requested[0]==L'1';
    report<<"DPI hwnd="<<reinterpret_cast<std::uintptr_t>(window)<<" test_unaware_requested="<<unawareRequested<<" window_context=";
    const auto actual=context?context(window):nullptr;
    if(context)report<<reinterpret_cast<std::uintptr_t>(actual);else report<<"unavailable";
    report<<" actual_window_awareness=";
    if(actual&&awareness){const int value=awareness(actual);report<<value<<'('<<
        (value==0?"unaware":value==1?"system_aware":value==2?"per_monitor_aware":"invalid")<<')';}
    else report<<"unavailable";
    report<<" effective_window_dpi=";
    if(dpi)report<<dpi(window);else report<<"unavailable";
    report<<"; actual HWND queried dynamically; SetProcessDPIAware success is not assumed; missing APIs remain unavailable on Win7.\n";
}
struct Metadata {
    struct State {HWND focus=nullptr,capture=nullptr;POINT cursor{};bool cursorOk=false;};
    struct Event {std::uint64_t sequence=0;DWORD tick=0;HWND window=nullptr;UINT message=0;WPARAM wp=0;LPARAM lp=0;
        unsigned phase=0;bool syntheticStack=false;State before{},after{};LRESULT result=0;};
    struct Storage {std::array<Event,4096> events{};std::size_t size=0;};
    struct Watch {Metadata* owner=nullptr;HWND window=nullptr;};
    std::unique_ptr<Storage> storage=std::make_unique<Storage>();
    std::array<Watch,3> watches{};
    unsigned phase=0;bool synthetic=false,truncated=false;
    static State Read() noexcept {State s;s.focus=GetFocus();s.capture=GetCapture();s.cursorOk=GetCursorPos(&s.cursor)!=FALSE;return s;}
    static bool Interesting(UINT m) noexcept {
        return m==WM_MOUSEMOVE||m==WM_MOUSEHOVER||m==WM_MOUSELEAVE||m==WM_NCMOUSEMOVE||m==WM_NCMOUSEHOVER||m==WM_NCMOUSELEAVE||
            m==WM_CHANGEUISTATE||m==WM_UPDATEUISTATE||m==WM_SETFOCUS||m==WM_KILLFOCUS||m==WM_CAPTURECHANGED||m==WM_CANCELMODE||
            m==WM_LBUTTONDOWN||m==WM_LBUTTONUP;
    }
    static LRESULT CALLBACK Observe(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& self=*reinterpret_cast<Watch*>(data)->owner;
        Event* event=nullptr;
        if(Interesting(m)){
            if(self.storage->size<self.storage->events.size()){
                const auto slot=self.storage->size++;event=&self.storage->events[slot];event->sequence=slot+1;event->tick=GetTickCount();
                event->window=h;event->message=m;event->wp=w;event->lp=l;event->phase=self.phase;
                event->syntheticStack=self.synthetic;event->before=Read();
            }else self.truncated=true;
        }
        const auto result=DefSubclassProc(h,m,w,l); // Every native procedure runs once; return is unchanged.
        if(event){event->after=Read();event->result=result;}
        return result;
    }
    void Attach(HWND main,HWND fov,HWND red) {
        const HWND windows[]={main,fov,red};
        try{for(std::size_t i=0;i<watches.size();++i){watches[i]={this,windows[i]};
            trace::Require(SetWindowSubclass(windows[i],Observe,reinterpret_cast<UINT_PTR>(&watches[i]),
                reinterpret_cast<DWORD_PTR>(&watches[i]))!=FALSE,"install preallocated hover metadata observer");}}
        catch(...){Detach();throw;}
    }
    void Detach() noexcept {for(auto& w:watches)if(w.window&&IsWindow(w.window))
        RemoveWindowSubclass(w.window,Observe,reinterpret_cast<UINT_PTR>(&w));}
    ~Metadata(){Detach();}
    unsigned Count(HWND window,UINT message) const noexcept {
        unsigned count=0;for(std::size_t i=0;i<storage->size;++i)count+=storage->events[i].window==window&&storage->events[i].message==message;return count;
    }
    void Write(const std::filesystem::path& path) const {
        std::ofstream f(path);f<<"# fixed storage; callbacks query only cursor/focus/capture; no GDI/getters/allocation in this metadata observer\n"
            <<"seq,tick,phase,hwnd,msg,wp,lp,synthetic_call_stack,before_focus,after_focus,before_capture,after_capture,"
              "before_cursor_ok,before_x,before_y,after_cursor_ok,after_x,after_y,native_return\n";
        for(std::size_t i=0;i<storage->size;++i){const auto& e=storage->events[i];
            f<<e.sequence<<','<<e.tick<<','<<e.phase<<','<<reinterpret_cast<std::uintptr_t>(e.window)<<','<<e.message<<','
                <<static_cast<std::uint64_t>(e.wp)<<','<<static_cast<std::int64_t>(e.lp)<<','<<e.syntheticStack<<','
                <<reinterpret_cast<std::uintptr_t>(e.before.focus)<<','<<reinterpret_cast<std::uintptr_t>(e.after.focus)<<','
                <<reinterpret_cast<std::uintptr_t>(e.before.capture)<<','<<reinterpret_cast<std::uintptr_t>(e.after.capture)<<','
                <<e.before.cursorOk<<','<<e.before.cursor.x<<','<<e.before.cursor.y<<','<<e.after.cursorOk<<','<<e.after.cursor.x<<','
                <<e.after.cursor.y<<','<<e.result<<'\n';}
        trace::Require(bool(f),"write actual hover/capture metadata");
    }
};
struct Keys {
    MMDApp& app;const int frame;
    const mikudancestudio::mdl::CameraKey* camera;
    const mikudancestudio::mdl::LightKey* light;
    const std::uint64_t cameraId,lightId;
    const std::vector<mikudancestudio::mdl::CameraKey> cameraBytes;
    const std::vector<mikudancestudio::mdl::LightKey> lightBytes;
    explicit Keys(MMDApp& a):app(a),frame(a.CurrentFrame()),camera(a.CameraKeys()),light(a.LightKeys()),
        cameraId(mikudancestudio::mdl::KeyPoolIdentity(camera)),lightId(mikudancestudio::mdl::KeyPoolIdentity(light)),
        cameraBytes(camera,camera+mikudancestudio::mdl::KeyPoolCapacity(camera)),
        lightBytes(light,light+mikudancestudio::mdl::KeyPoolCapacity(light)){}
    void Check() const {
        using namespace mikudancestudio::mdl;
        trace::Require(app.CurrentFrame()==frame&&app.CameraKeys()==camera&&app.LightKeys()==light&&
            KeyPoolIdentity(camera)==cameraId&&KeyPoolIdentity(light)==lightId&&
            KeyPoolCapacity(camera)==cameraBytes.size()&&KeyPoolCapacity(light)==lightBytes.size()&&
            !std::memcmp(camera,cameraBytes.data(),cameraBytes.size()*sizeof(*camera))&&
            !std::memcmp(light,lightBytes.data(),lightBytes.size()*sizeof(*light)),
            "cursor/hover pass preserves playhead and both original VMD key pointers, identities, capacities and bytes");
    }
};
struct Session {
    MMDApp& app;trace::Trace& pixels;Metadata& metadata;std::ofstream& report;
    HWND main=nullptr,wake=nullptr,ownCapture=nullptr,foreground=nullptr,previousForeground=nullptr;
    POINT original{},planned{};bool originalOk=false,ownsCursor=false,takenOver=false,incomplete=false,ownsCoordinator=false,cleaned=false;
    unsigned phase=0,messages=0,phaseMessageLimit=128,clockAdvances=0;std::uint64_t started=GetTickCount64();
    static constexpr unsigned kMessages=4096;
    Session(MMDApp& a,trace::Trace& p,Metadata& m,std::ofstream& r,HWND previous):
        app(a),pixels(p),metadata(m),report(r),main(a.MainWindow()),previousForeground(previous) {
        originalOk=GetCursorPos(&original)!=FALSE;planned=original;foreground=GetForegroundWindow();
    }
    bool Own(HWND h) const noexcept {return h&&(h==main||IsChild(main,h));}
    [[noreturn]] void Stop(const char* reason,bool takeover=false,const MSG* message=nullptr) {
        incomplete=true;takenOver|=takeover;
        report<<"INCOMPLETE phase="<<phase<<" reason="<<reason<<" takeover="<<takenOver;
        if(message)report<<" hwnd="<<reinterpret_cast<std::uintptr_t>(message->hwnd)<<" msg="<<message->message<<" wp="<<message->wParam;
        report<<'\n';report.flush();throw std::runtime_error(reason);
    }
    static bool KeysUp() noexcept {
        BYTE state[256]{};if(!GetKeyboardState(state))return false;
        for(int key=1;key<256;++key)if((state[key]&0x80)||(static_cast<unsigned short>(GetAsyncKeyState(key))&0x8000))return false;
        return true;
    }
    void Guard() {
        if(GetTickCount64()-started>45000)Stop("whole_pass_45_second_deadline");
        if(!Own(GetForegroundWindow())||GetForegroundWindow()!=foreground)Stop("foreign_or_changed_foreground",true);
        if(!KeysUp())Stop("physical_or_thread_key_down",true);
        POINT now{};if(!GetCursorPos(&now))Stop("cursor_query_failed",true);
        if(!Near(now,planned))Stop("cursor_outside_planned_one_pixel_tolerance",true);
        if(GetCapture()&&GetCapture()!=ownCapture)Stop("capture_not_owned_by_this_test",true);
    }
    bool IsMotion(UINT message) const noexcept {return message==WM_MOUSEMOVE||message==WM_MOUSEHOVER||message==WM_MOUSELEAVE||
        message==WM_NCMOUSEMOVE||message==WM_NCMOUSEHOVER||message==WM_NCMOUSELEAVE;}
    bool PermittedMotion(const MSG& message) const noexcept {
        if(!Own(message.hwnd)||!IsMotion(message.message))return false;
        if(message.message==WM_MOUSELEAVE||message.message==WM_NCMOUSELEAVE)return true;
        POINT point{static_cast<short>(LOWORD(message.lParam)),static_cast<short>(HIWORD(message.lParam))};
        if(message.message==WM_MOUSEMOVE||message.message==WM_MOUSEHOVER){if(!ClientToScreen(message.hwnd,&point))return false;}
        if(!Near(point,planned))return false;
        if(message.message==WM_MOUSEMOVE||message.message==WM_MOUSEHOVER){
            const auto allowed=ownCapture&&message.hwnd==ownCapture?WPARAM(MK_LBUTTON):WPARAM(0);
            if(message.wParam&~allowed)return false;
        }return true;
    }
    void InputQueue() {
        Guard();
        static constexpr UINT ranges[][2]={{WM_KEYFIRST,WM_KEYLAST},{WM_MOUSEFIRST,WM_MOUSELAST},
            {WM_NCMOUSEMOVE,WM_NCXBUTTONDBLCLK},{WM_INPUT,WM_INPUT},{WM_HOTKEY,WM_HOTKEY},
            {WM_IME_STARTCOMPOSITION,WM_IME_COMPOSITION},{WM_IME_KEYDOWN,WM_IME_KEYUP},{WM_IME_CHAR,WM_IME_CHAR},
            {WM_APPCOMMAND,WM_APPCOMMAND},{WM_GESTURE,WM_GESTURENOTIFY},{WM_TOUCH,WM_TOUCH},
            {WM_MOUSEHOVER,WM_MOUSELEAVE},{WM_NCMOUSEHOVER,WM_NCMOUSELEAVE},{WM_CLOSE,WM_CLOSE}};
        for(const auto& range:ranges){MSG pending{};
            if(PeekMessageW(&pending,nullptr,range[0],range[1],PM_NOREMOVE)&&!PermittedMotion(pending))
                Stop(pending.message==WM_QUIT?"queued_quit":"unplanned_or_foreign_input_left_queued",true,&pending);}
        Guard(); // PeekMessage can serve sent messages; this is a point-in-time check, not a sandbox.
    }
    void IdleCache() {
        const auto& s=app.state;
        const std::int32_t values[]={s.upKeyState,s.downKeyState,s.leftKeyState,s.rightKeyState,s.spaceKeyState,s.deleteKeyState,
            s.escKeyState,s.tabKeyState,s.enterKeyState,s.menuKeyState,s.keyState221,s.keyState226,s.shiftModifierState,
            s.ctrlModifierState,s.leftMouseButtonState,s.rightMouseButtonState,s.middleMouseButtonState};
        for(auto value:values)if(value)Stop("non_idle_native_key_cache");
        for(auto value:s.numpadKeyState)if(value)Stop("non_idle_numpad_cache");
        for(int i=0;i<18;++i)if(app.LetterHotkeys().Value(i)||s.dialogFlags[i])Stop("non_idle_letter_cache");
    }
    std::uint64_t Clock() const noexcept {return (std::uint64_t(app.TimeNowHigh())<<32)|app.TimeNowLow();}
    void Phase(const std::string& name) {++phase;metadata.phase=phase;pixels.Phase(name);
        report<<"PHASE "<<phase<<' '<<name<<" planned="<<planned.x<<','<<planned.y<<'\n';}
    void Start() {
        if(!originalOk)Stop("initial_cursor_unavailable");
        if(!Own(foreground))Stop("fixture_must_already_own_foreground");
        InputQueue();IdleCache();
        const auto existing=prior::UiPump::WakeWindow();
        if(!mikudancestudio::updates::Initialize(&app))Stop("coordinator_initialize_failed");
        ownsCoordinator=!existing;
        wake=prior::UiPump::WakeWindow();
        if(!wake||(existing&&wake!=existing))Stop("coordinator_window_mismatch");
        report<<"ADMISSION cursor="<<original.x<<','<<original.y<<" foreground="<<reinterpret_cast<std::uintptr_t>(foreground)
            <<" coordinator="<<reinterpret_cast<std::uintptr_t>(wake)<<" created="<<ownsCoordinator<<'\n';
    }
    void Safe() {
        InputQueue();const prior::PumpInvariant invariant(app);const auto before=Clock();
        mikudancestudio::updates::SafePoint(&app);clockAdvances+=Clock()!=before;invariant.Check();
    }
    void NativeEffect(const prior::PumpInvariant& before,HWND window,int oldPosition) {
        const auto values=prior::Parameters(app);const int id=GetDlgCtrlID(window),lane=id==447?0:1;
        trace::Require(id==447||id==455,"native input target retains its actual FOV/R control identity");
        trace::Require(app.CurrentFrame()==before.frame,"native cursor/button input preserves the playhead");
        for(int i=0;i<7;++i)if(i!=lane)trace::Require(!std::memcmp(&values[i],&before.values[i],sizeof(float)),
            "native target input preserves the other six authored parameters");
        const auto tick=int(SendMessageW(window,TBM_GETPOS,0,0));
        // The original RGB raw default .602 is not exactly tick154/256. A
        // no-op native input need not quantize an untouched work value.
        if(tick!=oldPosition||std::memcmp(&values[lane],&before.values[lane],sizeof(float)))
            trace::Require(prior::Authored(app,id,tick),"changed native cursor/button input uses exact original FOV/RGB arithmetic");
    }
    bool Allowed(const MSG& m) const noexcept {
        if(m.hwnd==wake)return m.message==WM_APP+0x73||(m.message==WM_TIMER&&m.wParam==1);
        if(!Own(m.hwnd))return false;
        if(IsMotion(m.message))return PermittedMotion(m);
        if(m.message==WM_PAINT||m.message==WM_UPDATEUISTATE||m.message==WM_CHANGEUISTATE)return true;
        return m.hwnd==main&&(m.message==WM_APP+0x6a||(m.message==WM_TIMER&&m.wParam==0x4d4d536c));
    }
    struct Pending {Session& self;MSG message;bool armed=true;
        ~Pending() noexcept {if(!armed)return;const auto old=GetLastError();
            DWORD process=0;const auto thread=GetWindowThreadProcessId(message.hwnd,&process);
            const bool posted=thread==GetCurrentThreadId()&&process==GetCurrentProcessId()&&self.Allowed(message)&&
                PostMessageW(message.hwnd,message.message,message.wParam,message.lParam)!=FALSE;
            const auto error=posted?ERROR_SUCCESS:GetLastError();
            try{self.report<<"UNDELIVERED_MESSAGE recovery_posted="<<posted<<" error="<<error<<" msg="<<message.message<<'\n';self.report.flush();}catch(...){}
            SetLastError(old);}};
    bool Dispatch(HWND window,UINT kind) {
        MSG pending{};if(!PeekMessageW(&pending,window,kind,kind,PM_NOREMOVE))return false;
        if(pending.message==WM_QUIT)Stop("quit_left_queued",true,&pending);
        if(pending.message!=kind||!Allowed(pending)){
            if(IsMotion(pending.message))Stop("unplanned_motion_left_queued",true,&pending);
            return false;
        }
        InputQueue();if(messages>=kMessages||messages>=phaseMessageLimit)Stop("message_quota");
        MSG message{};if(!PeekMessageW(&message,window,kind,kind,PM_REMOVE))return false;
        if(message.message==WM_QUIT){PostQuitMessage(int(message.wParam));Stop("quit_reposted",true,&message);}
        if(message.message!=kind||!Allowed(message)){
            const bool restored=message.hwnd?PostMessageW(message.hwnd,message.message,message.wParam,message.lParam)!=FALSE:
                PostThreadMessageW(GetCurrentThreadId(),message.message,message.wParam,message.lParam)!=FALSE;
            report<<"UNEXPECTED_MESSAGE restored="<<restored<<'\n';Stop("unexpected_message_not_dispatched",true,&message);
        }
        Pending restoration{*this,message};InputQueue();
        const bool mayAuthor=message.message==WM_MOUSEMOVE&&message.hwnd==ownCapture;
        const prior::PumpInvariant before(app);
        const auto oldPosition=mayAuthor?int(SendMessageW(ownCapture,TBM_GETPOS,0,0)):0;
        restoration.armed=false;DispatchMessageW(&message);++messages;
        if(!mayAuthor)before.Check();
        else NativeEffect(before,ownCapture,oldPosition);
        Safe();return true;
    }
    void Drain(const std::string& name,DWORD dwell=0) {
        Phase(name);const auto begin=GetTickCount64();const auto clocks=clockAdvances,startMessages=messages;
        phaseMessageLimit=(std::min)(messages+128,kMessages);
        for(unsigned round=0;round<16384;++round){
            Safe();bool progress=false;
            for(UINT message:{UINT(WM_MOUSEMOVE),UINT(WM_MOUSEHOVER),UINT(WM_MOUSELEAVE),UINT(WM_NCMOUSEMOVE),
                UINT(WM_NCMOUSEHOVER),UINT(WM_NCMOUSELEAVE),UINT(WM_CHANGEUISTATE),UINT(WM_UPDATEUISTATE),UINT(WM_PAINT),UINT(WM_APP+0x6a)})
                progress=Dispatch(main,message)||progress;
            progress=Dispatch(wake,WM_APP+0x73)||progress;progress=Dispatch(wake,WM_TIMER)||progress;
            progress=Dispatch(main,WM_TIMER)||progress;
            const auto elapsed=GetTickCount64()-begin;
            if(elapsed>=dwell&&clockAdvances-clocks>=2&&!progress){
                report<<"DRAIN phase="<<phase<<" clocks="<<clockAdvances-clocks<<" messages="<<messages-startMessages<<" elapsed_ms="<<elapsed<<'\n';return;}
            if(elapsed>(std::max)(DWORD(500),dwell+250))Stop("phase_deadline_without_quiescent_frames");
            Sleep(1);
        }Stop("phase_round_quota");
    }
    void Move(POINT point) {
        InputQueue();planned=point;ownsCursor=true;
        if(!SetCursorPos(point.x,point.y))Stop("SetCursorPos_failed");
        Guard();
    }
    void Button(HWND window,UINT message,POINT client) {
        InputQueue();metadata.synthetic=true;
        struct Reset {Metadata& value;~Reset(){value.synthetic=false;}} reset{metadata};
        const prior::PumpInvariant before(app);const auto oldPosition=int(SendMessageW(window,TBM_GETPOS,0,0));
        if(message==WM_LBUTTONDOWN)ownCapture=window;
        SendMessageW(window,message,message==WM_LBUTTONDOWN?MK_LBUTTON:0,MAKELPARAM(client.x,client.y));
        NativeEffect(before,window,oldPosition);
        if(message==WM_LBUTTONUP){if(GetCapture()==window)Stop("native_button_up_did_not_release");ownCapture=nullptr;}
        else if(GetCapture()!=window)Stop("native_button_down_did_not_capture");
    }
    void Cleanup() noexcept {
        if(cleaned)return;cleaned=true;
        ++phase;metadata.phase=phase;
        try{pixels.Phase("cleanup-controlled-restore");report<<"PHASE "<<phase<<" cleanup-controlled-restore\n";}catch(...){}
        if(ownCapture&&GetCapture()==ownCapture){SendMessageW(ownCapture,WM_CANCELMODE,0,0);if(GetCapture()==ownCapture)ReleaseCapture();}
        POINT now{};const bool controlled=!takenOver&&Own(GetForegroundWindow())&&GetForegroundWindow()==foreground&&
            GetCursorPos(&now)&&Near(now,planned)&&KeysUp();
        const bool restore=ownsCursor&&controlled;
        const bool restored=restore&&SetCursorPos(original.x,original.y)!=FALSE;
        const bool restoreForeground=controlled&&previousForeground&&previousForeground!=foreground&&IsWindow(previousForeground);
        const bool foregroundRestored=restoreForeground&&SetForegroundWindow(previousForeground)!=FALSE;
        try{report<<"CLEANUP cursor_restore_attempted="<<restore<<" restored="<<restored<<" takeover="<<takenOver
            <<" original="<<original.x<<','<<original.y<<" foreground_restore_attempted="<<restoreForeground
            <<" foreground_restored="<<foregroundRestored<<" (one attempt only; never restores after detected user takeover)\n";report.flush();}catch(...){}
        if(ownsCoordinator)mikudancestudio::updates::Shutdown(&app);
    }
    ~Session(){Cleanup();}
};
// This audit does not dispatch any private/system timer or synthesize motion.
// Querying tracking state and peeking the target's timers helps distinguish a
// canceled subscription from pending work outside the deliberately small pump.
static void TrackingAudit(Session& session,HWND window,const char* boundary) {
    session.InputQueue();const prior::PumpInvariant invariant(session.app);
    TRACKMOUSEEVENT query{sizeof(query),TME_QUERY,window,0};
    const bool queried=TrackMouseEvent(&query)!=FALSE;const auto error=queried?ERROR_SUCCESS:GetLastError();
    POINT cursor{};const bool cursorOk=GetCursorPos(&cursor)!=FALSE;
    session.report<<"TRACKING_AUDIT phase="<<session.phase<<" id="<<GetDlgCtrlID(window)<<" boundary="<<boundary
        <<" query_ok="<<queried<<" error="<<error<<" flags="<<query.dwFlags<<" tracked_hwnd="<<reinterpret_cast<std::uintptr_t>(query.hwndTrack)
        <<" hover_ms="<<query.dwHoverTime<<" cursor_ok="<<cursorOk<<" cursor="<<cursor.x<<','<<cursor.y
        <<" hit="<<reinterpret_cast<std::uintptr_t>(cursorOk?WindowFromPoint(cursor):nullptr)<<'\n';
    for(UINT kind:{UINT(WM_TIMER),UINT(0x0118)}){ // 0x0118 is WM_SYSTIMER, observed only.
        MSG pending{};const bool found=PeekMessageW(&pending,window,kind,kind,PM_NOREMOVE)!=FALSE;
        if(found&&pending.message==WM_QUIT)session.Stop("quit_left_queued_during_tracking_audit",true,&pending);
        session.report<<"TRACKING_PENDING_TIMER phase="<<session.phase<<" id="<<GetDlgCtrlID(window)<<" boundary="<<boundary
            <<" filter="<<kind<<" found="<<found;
        if(found)session.report<<" hwnd="<<reinterpret_cast<std::uintptr_t>(pending.hwnd)<<" msg="<<pending.message
            <<" wp="<<static_cast<std::uint64_t>(pending.wParam)<<" lp="<<static_cast<std::int64_t>(pending.lParam);
        session.report<<"; PM_NOREMOVE only, unknown timer is not dispatched.\n";
    }
    invariant.Check();session.Guard();
}
struct Sample {unsigned phase=0;std::string name;trace::Snapshot window;std::vector<std::uint32_t> screen;
    POINT cursor{},screenOrigin{};HWND focus=nullptr,capture=nullptr;unsigned screenDifferences=0;float authored=0;};
static RECT VisibleSpace(HWND main,HWND window) {
    RECT client{},screen{};trace::Require(IsWindowVisible(window)&&IsWindowEnabled(window)&&!IsIconic(main)&&
        GetClientRect(window,&client)&&client.right>20&&client.bottom>10,"actual enabled on-screen trackbar required");
    screen=client;MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&screen),2);
    MONITORINFO monitor{sizeof(monitor)};const auto device=MonitorFromRect(&screen,MONITOR_DEFAULTTONULL);
    trace::Require(device&&GetMonitorInfoW(device,&monitor)&&Inside(monitor.rcMonitor,screen),"entire slider client fits a physical monitor");
    for(int y:{0,int(client.bottom/2),int(client.bottom-1)})for(int x:{0,int(client.right/2),int(client.right-1)}){
        POINT point{x,y};ClientToScreen(window,&point);
        trace::Require(WindowFromPoint(point)==window,"on-screen nine-point hit-test has no sibling or foreign occluder");
    }return screen;
}
static Sample Capture(Session& session,HWND window,const std::string& name) {
    session.Guard();const auto area=VisibleSpace(session.main,window);
    Sample result;result.phase=session.phase;result.name=name;result.screenOrigin={area.left,area.top};result.window=session.pixels.Read(window);
    result.authored=GetDlgCtrlID(window)==447?session.app.CameraFov():session.app.LightColor()[0];
    trace::Require(result.window.Live()&&result.window.Varied()&&result.window.dcType==OBJ_DC&&!result.window.borrowedDc,
        "hover diagnosis reads real varied window pixels, never a supplied theme memory DC");
    result.focus=GetFocus();result.capture=GetCapture();GetCursorPos(&result.cursor);
    const auto dc=GetDC(nullptr);
    struct Release {HDC dc;~Release(){if(dc)ReleaseDC(nullptr,dc);}} release{dc};
    trace::Require(dc&&GetObjectType(dc)==OBJ_DC,"obtain actual desktop display DC");
    trace::Dib bitmap(result.window.width,result.window.height,dc);
    trace::Require(bool(bitmap)&&BitBlt(bitmap.dc,0,0,result.window.width,result.window.height,dc,area.left,area.top,SRCCOPY),"read actual desktop pixels at this slider");
    GdiFlush();result.screen.assign(bitmap.pixels,bitmap.pixels+result.window.pixels.size());
    for(std::size_t i=0;i<result.screen.size();++i)result.screenDifferences+=(result.screen[i]&0xffffff)!=(result.window.pixels[i]&0xffffff);
    return result;
}
static void Bitmap(const std::filesystem::path& path,int w,int h,const std::vector<std::uint32_t>& pixels) {
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+DWORD(pixels.size()*4);info.biSize=sizeof(info);info.biWidth=w;info.biHeight=-h;info.biPlanes=1;info.biBitCount=32;
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&file),sizeof(file));f.write(reinterpret_cast<const char*>(&info),sizeof(info));
    f.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()*4));trace::Require(bool(f),"write bounded hover window/screen evidence");
}
static void WriteSamples(const std::filesystem::path& output,const std::vector<Sample>& samples) {
    std::ofstream f(output/"hover-samples.csv");f<<"phase,name,id,tick,focus,capture,cursor_x,cursor_y,window_hash,paints,erases,thumb_uniform,"
        <<"screen_different_pixels,screen_x,screen_y,width,height,thumb_l,thumb_t,thumb_r,thumb_b,raw_authored_value,"
        <<"previous_same_tick_sample,previous_full_diff,previous_fixed_thumb_roi_diff\n";
    std::size_t bytes=0;
    for(std::size_t i=0;i<samples.size();++i){const auto& s=samples[i];
        const Sample* previous=i&&samples[i-1].window.id==s.window.id&&samples[i-1].window.tick==s.window.tick?&samples[i-1]:nullptr;
        f<<s.phase<<','<<s.name<<','<<s.window.id<<','<<s.window.tick<<','<<reinterpret_cast<std::uintptr_t>(s.focus)<<','
            <<reinterpret_cast<std::uintptr_t>(s.capture)<<','<<s.cursor.x<<','<<s.cursor.y<<','<<s.window.Hash()<<','<<s.window.paints<<','
            <<s.window.erases<<','<<s.window.ThumbUniform()<<','<<s.screenDifferences<<','<<s.screenOrigin.x<<','<<s.screenOrigin.y<<','
            <<s.window.width<<','<<s.window.height<<','<<s.window.thumb.left<<','<<s.window.thumb.top<<','<<s.window.thumb.right<<','<<s.window.thumb.bottom<<','
            <<std::setprecision(9)<<s.authored<<','<<bool(previous)<<','<<(previous?trace::Changed(previous->window,s.window):0)<<','
            <<(previous?trace::ChangedIn(previous->window,s.window,previous->window.thumb):0)<<'\n';
        bytes+=(s.screen.size()+s.window.pixels.size())*4;trace::Require(bytes<=8*1024*1024,"hover sample BMP quota");
        Bitmap(output/(std::to_string(i)+"-window.bmp"),s.window.width,s.window.height,s.window.pixels);
        Bitmap(output/(std::to_string(i)+"-screen.bmp"),s.window.width,s.window.height,s.screen);
    }trace::Require(bool(f),"write fixed-tick hover sample ledger");
}
static void Run(MMDApp* app,const std::filesystem::path& output,bool bound,HWND previousForeground,
    BOOL foregroundRequested,HWND foregroundAfterRequest) {
    using namespace mikudancestudio;
    trace::Require(app&&app==g_Block&&app->Renderer()&&app->Renderer()->device&&!app->PlaybackActive()&&!app->FrameStepPlayback()&&
        GetWindowThreadProcessId(app->MainWindow(),nullptr)==GetCurrentThreadId()&&IsWindowEnabled(app->MainWindow())&&
        !GetCapture()&&app->CameraKeys()&&app->LightKeys()&&bool(plugins::BoundNativePoseHistory(app))==bound,
        "hover pass requires an actual idle disposable renderer and the requested native history binding");
    std::array<HWND,7> windows{};for(std::size_t i=0;i<windows.size();++i)windows[i]=GetDlgItem(app->MainWindow(),prior::kIds[i]);
    std::filesystem::create_directories(output);std::ofstream report(output/"hover-report.txt");trace::Require(bool(report),"create separate hover diagnostic report");
    report<<"Independent cursor/hover diagnostic; mode="<<(bound?"bound":"ordinary")<<"; targets447,455. "
        <<"SetCursorPos drives real OS cursor motion; button down/up are explicitly SendMessage-generated, not hardware/SendInput. "
        <<"Native drawing/results remain unchanged. Extra test-requested TrackMouseEvent hover/leave notifications are labelled and are not product guarantees.\n"
        <<"Missing hover/leave notifications are logged immediately but do not gate independent press/fixed-tick/motion/release samples. "
        <<"Both targets finish within the same bounds before the required-notification verdict; a missing requirement remains INCOMPLETE.\n"
        <<"Input guard is point-in-time only: PM_NOREMOVE may service sent messages; later input/activation can race native polling. "
        <<"All real held keys, unplanned cursor beyond1px, foreign/changed foreground, foreign input or quit cause INCOMPLETE. "
        <<"No BlockInput/SetKeyboardState, no foreign input removed; native GetAsyncKeyState may consume its low bit. "
        <<"Only this test's capture is canceled; cursor restored only if still at planned point without detected user takeover.\n"
        <<"Limits:45s whole pass; system hover>10s incomplete;4096 metadata/messages;128messages/phase;64sample pairs/8MiB; "
        <<"unchanged Trace has2048events/64MiB and256BMPpairs/32MiB. "
        <<"Window/client monitor containment, nine-point WindowFromPoint and desktop/window RGB equality qualify pixels. "
        <<"Sampling is diagnostic overhead, not latency, hardware mouse or DWM visibility acceptance.\n";
    const Keys keys(*app);Metadata metadata;std::vector<Sample> samples;samples.reserve(40);
    trace::Trace observer(app->MainWindow(),windows);observer.QueuedUiMessages(true);
    Session session(*app,observer,metadata,report,previousForeground);
    bool evidenceWritten=false;
    std::array<unsigned,2> hoverBaselines{},leaveBaselines{};
    const auto write=[&]{metadata.Write(output/"hover-events.csv");WriteSamples(output,samples);observer.Write(output/"paint");evidenceWritten=true;};
    const auto sample=[&](HWND window,const std::string& name){
        if(samples.size()>=64)session.Stop("sample_quota");
        samples.push_back(Capture(session,window,name));
        if(samples.back().screenDifferences)session.Stop("desktop_and_window_pixels_disagree_observation_incomplete");
    };
    try {
        DpiAudit(app->MainWindow(),report);DpiAudit(windows[0],report);DpiAudit(windows[1],report);
        report<<"EXPOSURE previous_foreground="<<reinterpret_cast<std::uintptr_t>(previousForeground)
            <<"; CLI performed one planned SW_SHOW then one initial SetForegroundWindow(self) request; returned="<<foregroundRequested
            <<" actual_foreground_after_request="<<reinterpret_cast<std::uintptr_t>(foregroundAfterRequest)
            <<" fixture="<<reinterpret_cast<std::uintptr_t>(app->MainWindow())
            <<"; visibility/occlusion and foreground admission must still pass; no retry or activation during observation.\n";
        {
            const auto savedError=GetLastError();
            STARTUPINFOW startup{};startup.cb=sizeof(startup);GetStartupInfoW(&startup);
            WINDOWPLACEMENT placement{};placement.length=sizeof(placement);
            const BOOL placementKnown=GetWindowPlacement(app->MainWindow(),&placement);
            report<<"EXPOSURE_AUDIT startup_flags="<<startup.dwFlags<<" startup_show="<<startup.wShowWindow
                <<" startup_use_show="<<bool(startup.dwFlags&STARTF_USESHOWWINDOW)
                <<" visible="<<IsWindowVisible(app->MainWindow())<<" iconic="<<IsIconic(app->MainWindow())
                <<" enabled="<<IsWindowEnabled(app->MainWindow())
                <<" style="<<static_cast<std::uintptr_t>(GetWindowLongPtrW(app->MainWindow(),GWL_STYLE))
                <<" exstyle="<<static_cast<std::uintptr_t>(GetWindowLongPtrW(app->MainWindow(),GWL_EXSTYLE))
                <<" placement_known="<<placementKnown<<" placement_show="<<placement.showCmd
                <<"; state audit only, no further activation or visibility change.\n";
            SetLastError(savedError);
        }
        metadata.Attach(app->MainWindow(),windows[0],windows[1]);session.Start();session.Drain("startup");keys.Check();
        UINT hover=0;if(!SystemParametersInfoW(SPI_GETMOUSEHOVERTIME,0,&hover,0)||hover>10000)session.Stop("system_hover_time_unavailable_or_over_10_seconds");
        report<<"SYSTEM hover_ms="<<hover<<'\n';
        for(auto window:{windows[0],windows[1]}){
            const unsigned target=window==windows[0]?0:1;
            VisibleSpace(app->MainWindow(),window);RECT thumb{},channel{};
            SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));SendMessageW(window,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
            POINT local{(thumb.left+thumb.right)/2,(thumb.top+thumb.bottom)/2},screen=local;ClientToScreen(window,&screen);
            const auto motionBefore=metadata.Count(window,WM_MOUSEMOVE);
            const auto prefix=std::to_string(GetDlgCtrlID(window));session.Move(screen);session.Drain(prefix+"-approach");
            const auto hoverBefore=metadata.Count(window,WM_MOUSEHOVER);
            hoverBaselines[target]=hoverBefore;
            TrackingAudit(session,window,"before-test-hover-arm");
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_HOVER|TME_LEAVE,window,HOVER_DEFAULT};
            if(!TrackMouseEvent(&tracking))session.Stop("test_requested_hover_subscription_failed");
            TrackingAudit(session,window,"after-test-hover-arm");
            session.Drain(prefix+"-test-requested-OS-hover",hover+100);
            TrackingAudit(session,window,"after-bounded-hover-dwell");
            const bool earlyHover=metadata.Count(window,WM_MOUSEHOVER)>hoverBefore;
            report<<"NOTIFICATION_STATUS id="<<GetDlgCtrlID(window)<<" kind=hover bounded_dwell_received="<<earlyHover
                <<"; missing notification is deferred for final strict verdict; independent capture/motion still runs.\n";
            sample(window,prefix+"-hover-dwell");
            session.Phase(prefix+"-synthetic-native-down");session.Button(window,WM_LBUTTONDOWN,local);
            session.Drain(prefix+"-captured");const auto fixed=int(SendMessageW(window,TBM_GETPOS,0,0));
            for(unsigned i=0;i<8;++i){session.Move(screen);session.Drain(prefix+"-fixedtick-"+std::to_string(i));
                if(GetCapture()!=window)session.Stop("synthetic_button_capture_lost_during_fixed_phase");
                trace::Require(SendMessageW(window,TBM_GETPOS,0,0)==fixed,"eight stationary OS cursor samples retain identical authored tick");
                sample(window,prefix+"-fixedtick-"+std::to_string(i));keys.Check();}
            const int direction=local.x+10<channel.right?1:-1;bool moved=false;POINT last=local;
            for(int i=1;i<=8;++i){last={local.x+direction*i,local.y};POINT point=last;ClientToScreen(window,&point);
                session.Move(point);session.Drain(prefix+"-one-pixel-"+std::to_string(i));
                if(GetCapture()!=window)session.Stop("synthetic_button_capture_lost_during_motion_phase");
                const auto tick=int(SendMessageW(window,TBM_GETPOS,0,0));moved|=tick!=fixed;
                if(moved)trace::Require(prior::Authored(*app,GetDlgCtrlID(window),tick),"changed one-pixel native cursor samples keep original authored arithmetic");
                sample(window,prefix+"-one-pixel-"+std::to_string(i));keys.Check();}
            if(!moved)session.Stop("OS_cursor_motion_did_not_move_native_thumb");
            session.Phase(prefix+"-synthetic-native-up");session.Button(window,WM_LBUTTONUP,last);session.Drain(prefix+"-released");
            sample(window,prefix+"-released");
            const auto leaveBefore=metadata.Count(window,WM_MOUSELEAVE);
            leaveBaselines[target]=leaveBefore;
            TRACKMOUSEEVENT leave{sizeof(leave),TME_LEAVE,window,HOVER_DEFAULT};if(!TrackMouseEvent(&leave))session.Stop("test_requested_leave_subscription_failed");
            TrackingAudit(session,window,"after-test-leave-arm");
            POINT park{12,100};ClientToScreen(app->MainWindow(),&park);session.Move(park);session.Drain(prefix+"-test-requested-OS-leave",100);
            TrackingAudit(session,window,"after-bounded-leave-dwell");
            report<<"NOTIFICATION_STATUS id="<<GetDlgCtrlID(window)<<" kind=leave bounded_dwell_received="<<(metadata.Count(window,WM_MOUSELEAVE)>leaveBefore)
                <<"; missing notification is deferred for final strict verdict.\n";keys.Check();
            report<<"CONTROL id="<<GetDlgCtrlID(window)<<" fixed_tick="<<fixed<<" fixed_samples=8 one_pixel_samples=8 moved="<<moved
                <<" mousemove_messages="<<metadata.Count(window,WM_MOUSEMOVE)-motionBefore
                <<" test_requested_hover_messages="<<metadata.Count(window,WM_MOUSEHOVER)-hoverBefore
                <<" test_requested_leave_messages="<<metadata.Count(window,WM_MOUSELEAVE)-leaveBefore<<'\n';
        }
        trace::Require(samples.size()==36&&!metadata.truncated&&observer.Complete(),"complete both independent cursor/hover passes within fixed recording quotas");
        bool notificationsComplete=true;
        for(unsigned i=0;i<2;++i){const auto hovers=metadata.Count(windows[i],WM_MOUSEHOVER)-hoverBaselines[i];
            const auto leaves=metadata.Count(windows[i],WM_MOUSELEAVE)-leaveBaselines[i];
            notificationsComplete=notificationsComplete&&hovers&&leaves;
            report<<"REQUIRED_NOTIFICATIONS id="<<GetDlgCtrlID(windows[i])<<" received_hover="<<hovers<<" received_leave="<<leaves
                <<"; counts frozen before cleanup, which cannot satisfy missing test phases.\n";}
        session.InputQueue();keys.Check();const prior::PumpInvariant cleanupInvariant(*app);
        session.Cleanup();cleanupInvariant.Check();metadata.Detach();observer.Detach();
        trace::Require(!metadata.truncated&&observer.Complete(),"controlled cursor/foreground cleanup also fits the recording quotas");
        write();keys.Check();
        report<<"INDEPENDENT_DATA samples="<<samples.size()<<" dispatched="<<session.messages<<" actual_frame_clock_advances="<<session.clockAdvances
            <<" original_VMD_key_pools_unchanged=1 required_notifications_complete="<<notificationsComplete<<'\n';
        if(!notificationsComplete)session.Stop("required_OS_hover_or_leave_not_observed_after_independent_phases");
        report<<"PASS observation_complete=1 samples="<<samples.size()<<" dispatched="<<session.messages<<" actual_frame_clock_advances="<<session.clockAdvances
            <<" original_VMD_key_pools_unchanged=1; no hardware-mouse, DWM or flicker-removal claim.\n";report.flush();trace::Require(bool(report),"finish hover report");
    }catch(...){session.Cleanup();metadata.Detach();observer.Detach();
        try{if(!evidenceWritten)write();}catch(...){report<<"EVIDENCE_WRITE_FAILED; original exception retained.\n";}
        report<<(session.incomplete?"INCOMPLETE":"FAILED")<<" observation_complete=0; evidence retained; not a PASS.\n";report.flush();throw;}
}
} // namespace slider_hover_host_checks
#endif
