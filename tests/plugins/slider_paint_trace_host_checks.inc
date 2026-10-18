// Include into a full, disposable scene host. Invoke before binding for ordinary
// coverage and after real NativePoseTestBinding for bound coverage, with separate
// fresh output folders. This authors live slider values, but never saves a scene.
#include "../slider_paint_trace.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/update_coordinator.hpp"
#include "mikudancestudio/ui_metrics.hpp"
#include "../../src/plugins/history_runtime.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../../src/plugins/native_pose_scope.hpp"
#endif
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

namespace slider_paint_trace_host_checks {
namespace trace = slider_paint_trace;
static constexpr std::array<int, 7> kIds{447, 455, 456, 457, 458, 459, 460};
struct Summary {
    unsigned controls = 0, dragged = 0, liveControls = 0, eraseExposedThumb = 0;
    unsigned parentPixelChanges = 0, peerPixelChanges = 0, paintEvents = 0, eraseEvents = 0;
    unsigned dragPaintEvents = 0, dragEraseEvents = 0, parentErasePixelChanges = 0;
    unsigned baselineCustomDraw = 0, controlledCustomDraw = 0, diagnosticCustomDraw = 0, diagnosticReturnChanges = 0, diagnosticControls = 0;
    unsigned windowThumbClears = 0, memoryThumbClears = 0, windowThumbRestores = 0, memoryThumbRestores = 0;
    unsigned baselineSameTickWindowDiff = 0, diagnosticWindowDiff = 0, diagnosticMemoryDiff = 0;
};
static bool SamePixels(const trace::Snapshot& a, const trace::Snapshot& b) {
    return a.copied && b.copied && a.width == b.width && a.height == b.height && trace::Changed(a, b) == 0;
}
// Query class identity before installing the trace. IsAppThemed alone does not
// identify which side-by-side common-controls class created a particular HWND.
static void ClassIdentity(HWND window, std::ofstream& report) {
    wchar_t name[256]{};
    trace::Require(GetClassNameW(window, name, 256) != 0, "read actual native window class");
    const auto module = reinterpret_cast<HMODULE>(GetClassLongPtrW(window, GCLP_HMODULE));
    std::wstring path(32768, L'\0');
    const auto copied = module ? GetModuleFileNameW(module, path.data(), DWORD(path.size())) : 0;
    trace::Require(copied && copied < path.size(), "read actual native class module path");
    path.resize(copied);
    struct Version { DWORD size, major, minor, build, platform; } version{sizeof(Version),0,0,0,0};
    using GetVersion = HRESULT(CALLBACK*)(Version*);
    const auto getVersion = reinterpret_cast<GetVersion>(GetProcAddress(module, "DllGetVersion"));
    const auto result = getVersion ? getVersion(&version) : E_NOINTERFACE;
    report << "CLASS id=" << GetDlgCtrlID(window) << " hwnd=" << reinterpret_cast<std::uintptr_t>(window)
        << " class=" << std::filesystem::path(name).u8string() << " unicode=" << (IsWindowUnicode(window) != FALSE)
        << " module=" << std::filesystem::path(path).u8string() << " dll_version_result=" << result;
    if (SUCCEEDED(result)) report << " dll_version=" << version.major << '.' << version.minor << '.' << version.build
        << " platform=" << version.platform;
    report << '\n';
}
static void Flush(HWND parent, const std::array<HWND, 7>& controls) {
    UpdateWindow(parent);
    for (auto control : controls) UpdateWindow(control);
    GdiFlush();
}
static bool Authored(mikudancestudio::MMDApp& app, int id, int position) {
    if (id == 447) return app.CameraFov() == float(position);
    const unsigned lane = unsigned(id - 455);
    return lane < 3 ? app.LightColor()[lane] == float(double(position) * .00390625) :
        app.LightDirection()[lane - 3] == float(double(position) / 100.0);
}
#if !MMD_ORIGINAL_PROFILE
static std::array<float,7> Parameters(mikudancestudio::MMDApp& app) {
    return {app.CameraFov(),app.LightColor()[0],app.LightColor()[1],app.LightColor()[2],
        app.LightDirection()[0],app.LightDirection()[1],app.LightDirection()[2]};
}
struct PumpInvariant {
    mikudancestudio::MMDApp& app;
    const std::array<float,7> values;
    const int frame;
    struct History { mikudancestudio::plugins::SharedHistory* value=nullptr; std::uint64_t revision=0,retained=0; std::size_t undo=0,redo=0; };
    std::array<History,2> histories{};
    explicit PumpInvariant(mikudancestudio::MMDApp& value):app(value),values(Parameters(value)),frame(value.CurrentFrame()) {
        histories[0].value=mikudancestudio::plugins::NativeHistory(&app);
        histories[1].value=mikudancestudio::plugins::BoundNativePoseHistory(&app);
        for(auto& h:histories)if(h.value){trace::Require(h.value->Revision(h.revision)==MMS_OK,"read actual history before queued UI work");
            h.retained=h.value->RetainedBytes();h.undo=h.value->UndoCount();h.redo=h.value->RedoCount();}
    }
    void Check() const {
        const auto current=Parameters(app);
        trace::Require(app.CurrentFrame()==frame&&!std::memcmp(values.data(),current.data(),sizeof(values)),
            "queued UI/frame scheduling preserves all seven just-authored parameter values and playhead");
        trace::Require(mikudancestudio::plugins::NativeHistory(&app)==histories[0].value,"queued UI work retains its original host journal");
        trace::Require(mikudancestudio::plugins::BoundNativePoseHistory(&app)==histories[1].value,"queued UI work retains its bound journal");
        for(const auto& h:histories)if(h.value){std::uint64_t revision=0;
            trace::Require(h.value->Revision(revision)==MMS_OK&&revision==h.revision&&h.value->RetainedBytes()==h.retained&&
                h.value->UndoCount()==h.undo&&h.value->RedoCount()==h.redo,
                "queued UI/frame scheduling adds no history entry, revision or retained payload");}
    }
};
// Only known messages belonging to this disposable application are removed.
// In particular, a filtered PeekMessage can still return WM_QUIT: inspect with
// NOREMOVE first, and preserve a quit posted between the two calls as well.
struct UiPump {
    mikudancestudio::MMDApp& app;
    trace::Trace& observer;
    std::ofstream& report;
    HWND main=nullptr,wake=nullptr,initialForeground=nullptr;
    bool ownsCoordinator=false,stopping=false;
    unsigned dispatched=0,uiRefresh=0,wakes=0,timers=0,paints=0,safePoints=0,clockAdvances=0;
    unsigned samples=0,sampleLimit=0;
    std::string phase;
    static constexpr unsigned kSampleMessages=96,kTotalMessages=4096;
    bool FixtureForeground(HWND window) const noexcept {
        if(!window)return false;
        const auto floating=app.FloatingWindow();
        const auto owner=GetAncestor(window,GA_ROOTOWNER);
        return window==main||IsChild(main,window)||owner==main||
            (floating&&(window==floating||IsChild(floating,window)||owner==floating));
    }
    [[noreturn]] void Interrupted(const char* reason,const char* boundary,int key=0,const MSG* message=nullptr) {
        stopping=true;
        report<<"INTERRUPTED status=incomplete reason="<<reason<<" boundary="<<boundary<<" phase="<<phase
            <<" initial_foreground="<<reinterpret_cast<std::uintptr_t>(initialForeground)
            <<" current_foreground="<<reinterpret_cast<std::uintptr_t>(GetForegroundWindow())<<" key="<<key;
        if(message)report<<" queued_hwnd="<<reinterpret_cast<std::uintptr_t>(message->hwnd)
            <<" queued_msg="<<message->message<<" queued_wp="<<static_cast<std::uint64_t>(message->wParam);
        report<<"; no held/toggle key state written and no queued input removed; GetAsyncKeyState may consume its native "
            <<"pressed-since-query low bit; this pass did not complete and is not a PASS.\n";
        report.flush();
        throw std::runtime_error("queued UI diagnosis interrupted by input, activation or non-idle keys; incomplete");
    }
    void CheckForeground(const char* boundary) {
        const auto foreground=GetForegroundWindow();
        if(foreground!=initialForeground)Interrupted("foreground_changed",boundary);
    }
    void CheckPhysicalKeys(const char* boundary) {
        BYTE keys[256]{};
        if(!GetKeyboardState(keys))Interrupted("keyboard_state_unavailable",boundary);
        for(int key=1;key<256;++key){
            if((keys[key]&0x80)!=0)Interrupted("thread_key_down",boundary,key);
            // Only the high bit is used: the low-bit "pressed since query"
            // indication is not a reliable input-isolation signal.
            if((static_cast<unsigned short>(GetAsyncKeyState(key))&0x8000)!=0)
                Interrupted("physical_key_down",boundary,key);
        }
    }
    void CheckInput(const char* boundary) {
        CheckForeground(boundary);CheckPhysicalKeys(boundary);
        // A real press may already have been released by the time we poll.
        // Inspect every thread window and thread-message queue without removing
        // input. Fixture messages are sent synchronously, but native capture
        // changes can still enqueue OS mouse moves; those also interrupt.
        static constexpr UINT ranges[][2]={
            {WM_KEYFIRST,WM_KEYLAST},{WM_MOUSEFIRST,WM_MOUSELAST},
            {WM_NCMOUSEMOVE,WM_NCXBUTTONDBLCLK},{WM_INPUT,WM_INPUT},
            {WM_IME_STARTCOMPOSITION,WM_IME_COMPOSITION},{WM_IME_KEYDOWN,WM_IME_KEYUP},
            {WM_IME_CHAR,WM_IME_CHAR},{WM_HOTKEY,WM_HOTKEY},{WM_APPCOMMAND,WM_APPCOMMAND},
            {WM_GESTURE,WM_GESTURENOTIFY},{WM_TOUCH,WM_TOUCH}
        };
        for(const auto& range:ranges){MSG pending{};
            if(PeekMessageW(&pending,nullptr,range[0],range[1],PM_NOREMOVE))
                Interrupted(pending.message==WM_QUIT?"queued_quit":"queued_input",boundary,0,&pending);
        }
        // PeekMessage may dispatch sent messages. Check again before entering
        // FrameDriver, including the SafePoint called by the real WakeProc.
        CheckForeground(boundary);CheckPhysicalKeys(boundary);
    }
    bool WhiteListedMessage(const MSG& message) const noexcept {
        if(message.message==WM_PAINT)
            return message.hwnd&&(message.hwnd==main||IsChild(main,message.hwnd));
        if(message.hwnd==wake)
            return message.message==WM_APP+0x73||(message.message==WM_TIMER&&message.wParam==1);
        return message.hwnd==main&&(message.message==WM_APP+0x6a||
            (message.message==WM_TIMER&&message.wParam==0x4d4d536c));
    }
    struct PendingDispatch {
        UiPump& pump;
        const MSG message;
        bool armed=true;
        PendingDispatch(UiPump& owner,const MSG& value) noexcept:pump(owner),message(value){}
        PendingDispatch(const PendingDispatch&)=delete;
        PendingDispatch& operator=(const PendingDispatch&)=delete;
        ~PendingDispatch() noexcept {
            if(!armed)return;
            const auto previousError=GetLastError();
            DWORD process=0;
            const auto thread=GetWindowThreadProcessId(message.hwnd,&process);
            const bool allowed=thread==GetCurrentThreadId()&&process==GetCurrentProcessId()&&
                pump.WhiteListedMessage(message);
            const bool posted=allowed&&PostMessageW(message.hwnd,message.message,message.wParam,message.lParam)!=FALSE;
            const DWORD error=posted?ERROR_SUCCESS:allowed?GetLastError():ERROR_INVALID_WINDOW_HANDLE;
            // Do not replace the input/activation exception with a logging or
            // requeue failure. A failure is recorded, never silently a PASS.
            try {
                pump.report<<"PENDING_DISPATCH_RECOVERY allowed="<<allowed<<" posted="<<posted<<" error="<<error
                    <<" hwnd="<<reinterpret_cast<std::uintptr_t>(message.hwnd)<<" msg="<<message.message
                    <<" wp="<<static_cast<std::uint64_t>(message.wParam)<<" lp="<<static_cast<std::int64_t>(message.lParam)
                    <<"; original exception retained; no quit/input message is owned by this guard.\n";
                pump.report.flush();
            }catch(...){}
            SetLastError(previousError);
        }
        void Dispatched() noexcept {armed=false;}
    };
    void CheckInitialKeyCache() {
        struct KeySlot {int key;std::int32_t mikudancestudio::MMDAppState::* value;};
        static constexpr KeySlot slots[]={
            {VK_UP,&mikudancestudio::MMDAppState::upKeyState},
            {VK_DOWN,&mikudancestudio::MMDAppState::downKeyState},
            {VK_LEFT,&mikudancestudio::MMDAppState::leftKeyState},
            {VK_RIGHT,&mikudancestudio::MMDAppState::rightKeyState},
            {VK_SPACE,&mikudancestudio::MMDAppState::spaceKeyState},
            {VK_DELETE,&mikudancestudio::MMDAppState::deleteKeyState},
            {VK_ESCAPE,&mikudancestudio::MMDAppState::escKeyState},
            {VK_TAB,&mikudancestudio::MMDAppState::tabKeyState},
            {VK_RETURN,&mikudancestudio::MMDAppState::enterKeyState},
            {VK_MENU,&mikudancestudio::MMDAppState::menuKeyState},
            {221,&mikudancestudio::MMDAppState::keyState221},
            {226,&mikudancestudio::MMDAppState::keyState226},
            {VK_SHIFT,&mikudancestudio::MMDAppState::shiftModifierState},
            {VK_CONTROL,&mikudancestudio::MMDAppState::ctrlModifierState},
            {VK_LBUTTON,&mikudancestudio::MMDAppState::leftMouseButtonState},
            {VK_RBUTTON,&mikudancestudio::MMDAppState::rightMouseButtonState},
            {VK_MBUTTON,&mikudancestudio::MMDAppState::middleMouseButtonState}
        };
        for(const auto& slot:slots)if(app.state.*slot.value!=0)
            Interrupted("non_idle_native_key_cache","admission",slot.key);
        for(int i=0;i<10;++i)if(app.state.numpadKeyState[i]!=0)
            Interrupted("non_idle_numpad_cache","admission",VK_NUMPAD0+i);
        for(int i=0;i<18;++i)if(app.LetterHotkeys().Value(i)!=0||app.state.dialogFlags[i]!=0)
            Interrupted("non_idle_letter_or_dialog_cache","admission",mikudancestudio::kLetterVirtualKeys[i]);
    }
    static HWND WakeWindow() {
        HWND found=nullptr;
        for(HWND candidate=nullptr;(candidate=FindWindowExW(HWND_MESSAGE,candidate,L"MMS.UpdateCoordinator.V1",nullptr));){
            DWORD process=0;const auto thread=GetWindowThreadProcessId(candidate,&process);
            if(thread==GetCurrentThreadId()&&process==GetCurrentProcessId()){
                trace::Require(!found,"one coordinator message window for the isolated UI thread");found=candidate;}
        }return found;
    }
    UiPump(mikudancestudio::MMDApp& value,trace::Trace& valueObserver,std::ofstream& stream):
        app(value),observer(valueObserver),report(stream),main(value.MainWindow()),initialForeground(GetForegroundWindow()) {
        phase="ui-pump-admission";
        report<<"INPUT_GUARD initial_foreground="<<reinterpret_cast<std::uintptr_t>(initialForeground)
            <<" fixture_foreground="<<FixtureForeground(initialForeground)
            <<" policy=accept_initial_foreground_reject_pressed_keys_queued_input_foreground_changes\n"
            <<"INPUT_GUARD_BOUNDARY point_in_time_checks_only; PM_NOREMOVE can service sent messages, and input/activation can "
            <<"change before native FrameDriver polling; this is not an input sandbox. Initial fixture foreground is accepted. "
            <<"Held/toggle key state is not written and queued input is not removed; native GetAsyncKeyState queries may consume "
            <<"the pressed-since-query low bit. OS mouse moves generated by capture changes still interrupt as incomplete.\n";
        CheckInitialKeyCache();CheckInput("admission");CheckInitialKeyCache();
        const auto existing=WakeWindow();
        trace::Require(mikudancestudio::updates::Initialize(&app),"initialize or reuse the actual production update coordinator");
        ownsCoordinator=!existing;
        try {wake=WakeWindow();trace::Require(wake&&(!existing||wake==existing),"retain the exact application coordinator HWND");}
        catch(...){if(ownsCoordinator)mikudancestudio::updates::Shutdown(&app);throw;}
        report<<"COORDINATOR main="<<reinterpret_cast<std::uintptr_t>(main)<<" wake="<<reinterpret_cast<std::uintptr_t>(wake)
            <<" created_by_pass="<<ownsCoordinator<<" fps_limit="<<app.FpsLimit()<<'\n';
    }
    ~UiPump(){if(ownsCoordinator)mikudancestudio::updates::Shutdown(&app);}
    std::uint64_t Clock() const noexcept {return (std::uint64_t(app.TimeNowHigh())<<32)|app.TimeNowLow();}
    void Safe() {
        CheckInput("before_safepoint");
        const auto before=Clock(),present=app.frameRate.presentedFrames;
        ++safePoints;
        observer.Phase(phase+"-safepoint-"+std::to_string(safePoints));
        mikudancestudio::updates::SafePoint(&app);
        clockAdvances+=Clock()!=before;
        if(Clock()!=before||app.frameRate.presentedFrames!=present)
            report<<"SAFE sample="<<samples<<" index="<<safePoints<<" phase="<<phase
                <<" clock_before="<<before<<" clock_after="<<Clock()<<" present_before="<<present
                <<" present_after="<<app.frameRate.presentedFrames<<" message_seen="<<app.MessageSeen()<<'\n';
    }
    bool Dispatch(HWND window,UINT kind,WPARAM timer=0,bool childPaint=false) {
        if(!window||!IsWindow(window))return false;
        // Match WinMain's safe point before examining/removing one message.
        // Never invoke FrameDriver between the non-removing and removing peek.
        Safe();
        MSG pending{};if(!PeekMessageW(&pending,window,kind,kind,PM_NOREMOVE))return false;
        if(pending.message==WM_QUIT){stopping=true;return false;}
        const bool owner=pending.hwnd==window||(childPaint&&pending.hwnd&&IsChild(window,pending.hwnd));
        if(pending.message!=kind||!owner||(timer&&pending.wParam!=timer))return false;
        trace::Require(dispatched<sampleLimit&&dispatched<kTotalMessages,
            "queued UI diagnosis stops before exceeding its bounded dispatch quota");
        CheckInput("before_remove");
        trace::Require(WhiteListedMessage(pending),"only the isolated application's known UI messages enter pending-dispatch recovery");
        MSG message{};if(!PeekMessageW(&message,window,kind,kind,PM_REMOVE))return false;
        if(message.message==WM_QUIT){PostQuitMessage(int(message.wParam));stopping=true;return false;}
        // The UI thread cannot remove a different queued message between peeks.
        // Defensive preservation also covers a reentrant sent-message handler.
        if(message.message!=kind||!(message.hwnd==window||(childPaint&&message.hwnd&&IsChild(window,message.hwnd)))||
            (timer&&message.wParam!=timer)){
            trace::Require(message.hwnd&&PostMessageW(message.hwnd,message.message,message.wParam,message.lParam),
                "preserve an unexpected queued message instead of dispatching it");
            stopping=true;return false;
        }
        PendingDispatch pendingDispatch(*this,message);
        observer.Phase(phase+"-dispatch-"+std::to_string(kind)+"-"+std::to_string(dispatched+1));
        CheckInput("before_dispatch");
        const auto before=Clock(),present=app.frameRate.presentedFrames;
        // Once dispatched, a handler might already have changed state before
        // failing; replaying it would be incorrect. Only not-yet-dispatched
        // messages are returned to their original HWND during unwinding.
        pendingDispatch.Dispatched();
        DispatchMessageW(&message);++dispatched;
        clockAdvances+=Clock()!=before; // WakeProc can itself call SafePoint.
        if(kind==WM_PAINT)++paints;else if(kind==WM_TIMER)++timers;
        else if(window==wake)++wakes;else ++uiRefresh;
        report<<"DISPATCH sample="<<samples<<" index="<<dispatched<<" phase="<<phase
            <<" hwnd="<<reinterpret_cast<std::uintptr_t>(message.hwnd)<<" msg="<<message.message
            <<" wp="<<static_cast<std::uint64_t>(message.wParam)<<" clock_before="<<before<<" clock_after="<<Clock()
            <<" present_before="<<present<<" present_after="<<app.frameRate.presentedFrames<<'\n';
        return true;
    }
    void Drain(const std::string& name) {
        phase=name;++samples;sampleLimit=(std::min)(dispatched+kSampleMessages,kTotalMessages);
        const PumpInvariant invariant(app);
        const auto started=GetTickCount64(),present=app.frameRate.presentedFrames;
        const auto before=clockAdvances,initialMessages=dispatched;
        unsigned rounds=0;
        for(;rounds<256;++rounds){
            MSG close{};
            if(PeekMessageW(&close,main,WM_CLOSE,WM_CLOSE,PM_NOREMOVE)&&
                (close.message==WM_QUIT||close.message==WM_CLOSE))stopping=true;
            trace::Require(!stopping,"leave queued close/quit untouched and stop the disposable UI diagnosis");
            Safe();
            bool progress=false;
            progress=Dispatch(main,WM_APP+0x6a)||progress;
            progress=Dispatch(wake,WM_APP+0x73)||progress;
            progress=Dispatch(wake,WM_TIMER,1)||progress;
            progress=Dispatch(main,WM_TIMER,0x4d4d536c)||progress;
            progress=Dispatch(main,WM_PAINT,0,true)||progress;
            invariant.Check();
            trace::Require(!stopping,"queued stop remains unconsumed during isolated message dispatch");
            if(clockAdvances-before>=2&&!progress)break;
            if(GetTickCount64()-started>=240)break;
            Sleep(1);
        }
        invariant.Check();
        report<<"DRAIN sample="<<samples<<" phase="<<phase<<" rounds="<<rounds<<" elapsed_ms="<<GetTickCount64()-started
            <<" dispatched="<<dispatched-initialMessages<<" clock_advances="<<clockAdvances-before
            <<" presents="<<app.frameRate.presentedFrames-present<<" invariant=1\n";
        report.flush();
        trace::Require(clockAdvances-before>=2,"queued drag diagnosis actually schedules both passes of the native edit gate");
        observer.Phase(name);
    }
};
#endif
static void ControlledErase(trace::Trace& observer, HWND control, std::ofstream& report, Summary& summary) {
    RECT rect{}; GetClientRect(control, &rect); trace::Dib target(rect.right, rect.bottom);
    trace::Require(bool(target), "create controlled native erase DIB");
    // Calibrate the pixel observer against a deterministic change. This is a
    // sampler self-check, not a substituted native erase or a production fix.
    PatBlt(target.dc, 0, 0, rect.right, rect.bottom, WHITENESS);
    const auto white = observer.Read(control, target.dc);
    PatBlt(target.dc, 0, 0, rect.right, rect.bottom, BLACKNESS);
    const auto black = observer.Read(control, target.dc);
    trace::Require(trace::Changed(white, black) == std::size_t(rect.right) * rect.bottom, "pixel observer detects every calibrated RGB change");
    const auto id = GetDlgCtrlID(control);
    observer.Phase(std::to_string(id) + "-controlled-native-print");
    SendMessageW(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target.dc), PRF_CLIENT);
    const auto drawn = observer.Read(control, target.dc);
    observer.Phase(std::to_string(id) + "-controlled-native-erase");
    const auto erasedResult = SendMessageW(control, WM_ERASEBKGND, reinterpret_cast<WPARAM>(target.dc), 0);
    const auto erased = observer.Read(control, target.dc);
    observer.Phase(std::to_string(id) + "-controlled-native-restore");
    SendMessageW(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target.dc), PRF_CLIENT);
    const auto restored = observer.Read(control, target.dc);
    trace::Require(drawn.copied && drawn.Varied() && restored.copied && restored.Varied(), "native reference and restore contain real track/thumb pixels");
    const bool exposed = !drawn.ThumbUniform() && erased.ThumbUniform() && trace::Changed(drawn, erased) != 0;
    summary.eraseExposedThumb += exposed;
    report << "CONTROLLED id=" << id << " erase_result=" << erasedResult << " changed=" << trace::Changed(drawn, erased)
        << " thumb_uniform_before=" << drawn.ThumbUniform() << " thumb_uniform_after=" << erased.ThumbUniform()
        << " native_restore_matches=" << SamePixels(drawn, restored) << " exposed_thumb=" << exposed
        << " (owned DIB evidence; not proof that this intermediate reaches the screen)\n";
}
static void DetailedPaint(trace::Trace& observer, HWND parent, const std::array<HWND,7>& controls,
    HWND control, std::ofstream& report, Summary& summary, bool requireLivePixels) {
    const auto id = GetDlgCtrlID(control); const auto tick = SendMessageW(control, TBM_GETPOS, 0, 0);
    const auto initial = observer.Read(control);
    observer.Phase(std::to_string(id) + "-baseline-detail-paint");
    trace::Require(RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW) != FALSE,
        "redraw original native baseline without altering any return value");
    Flush(parent, controls); const auto before = observer.Read(control);
    observer.Phase(std::to_string(id) + "-item-notify-only-paint");
    {
        struct Mode { trace::Trace& observer; Mode(trace::Trace& value):observer(value){observer.ItemNotifications(true);}
            ~Mode(){observer.ItemNotifications(false);} } mode{observer};
        trace::Require(RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW) != FALSE,
            "redraw native control with extra item notifications only");
        Flush(parent, controls);
    }
    const auto after = observer.Read(control);
    report << "ITEM_DIAGNOSTIC id=" << id << " original_live=" << before.Live() << " diagnostic_live=" << after.Live()
        << " final_pixels_match=" << SamePixels(before,after) << " final_changed=" << trace::Changed(before,after)
        << " original_paints=" << before.paints-initial.paints << " diagnostic_paints=" << after.paints-before.paints
        << " original_erases=" << before.erases-initial.erases << " diagnostic_erases=" << after.erases-before.erases
        << " tick_before=" << tick << " tick_after=" << SendMessageW(control,TBM_GETPOS,0,0)
        << " (extra notifications requested only in this named pass; native drawing is not replaced)\n";
    trace::Require((!requireLivePixels || (before.Live() && after.Live())) && SamePixels(before,after) &&
        before.paints>initial.paints && before.paints-initial.paints==after.paints-before.paints &&
        before.erases-initial.erases==after.erases-before.erases && SendMessageW(control,TBM_GETPOS,0,0)==tick,
        "item notification diagnostics preserve the actual final window pixels and value");
    summary.diagnosticControls += before.Live() && after.Live();
    // A third, separately named pass removes the observer's reentrant TBM/GDI
    // queries from the native paint stack. Final pixels are read only after the
    // return hook is removed; no synthetic NM_CUSTOMDRAW notification is sent.
    observer.Phase(std::to_string(id) + "-metadata-item-only-paint");
    {
        struct Mode { trace::Trace& observer;
            Mode(trace::Trace& value, int id):observer(value){observer.BeginMetadataItems(id);}
            ~Mode(){observer.EndMetadataItems();} } mode{observer,id};
        trace::Require(RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW) != FALSE,
            "redraw native control with metadata-only item return audit");
        Flush(parent, controls);
    }
    const auto metadata = observer.Read(control);
    report << "METADATA_ITEM_DIAGNOSTIC id=" << id << " final_live=" << metadata.Live()
        << " final_pixels_match=" << SamePixels(after,metadata) << " final_changed=" << trace::Changed(after,metadata)
        << " sampled_paints=" << after.paints-before.paints << " metadata_paints=" << metadata.paints-after.paints
        << " sampled_erases=" << after.erases-before.erases << " metadata_erases=" << metadata.erases-after.erases
        << " tick_before=" << tick << " tick_after=" << SendMessageW(control,TBM_GETPOS,0,0)
        << " (fixed-buffer callbacks and final-return hook; no observer capture/getter in native message stack)\n";
    trace::Require((!requireLivePixels || metadata.Live()) && SamePixels(after,metadata) &&
        metadata.paints-after.paints == after.paints-before.paints && metadata.erases-after.erases == after.erases-before.erases &&
        SendMessageW(control,TBM_GETPOS,0,0)==tick,
        "metadata-only diagnosis preserves the actual final window pixels, native paint count and value");
}
#if !MMD_ORIGINAL_PROFILE
static void PumpedDrags(mikudancestudio::MMDApp& app,const std::array<HWND,7>& controls,
    const std::filesystem::path& output,bool requireLivePixels) {
    using namespace mikudancestudio;
    std::filesystem::create_directories(output);
    std::ofstream report(output/"paint-report.txt");
    trace::Require(bool(report),"create the independent queued-drag report");
    report<<"Separate real-drag-with-ui-pump pass; native control returns remain unchanged. "
        <<"No explicit invalidation, RDW_ERASE, WM_PRINTCLIENT, synthetic TBM_SETPOS or drawing replacement in this pass.\n"
        <<"Whitelist: exact main WM_APP+0x6a, exact message-only coordinator WM_APP+0x73 and WM_TIMER(1), "
        <<"main history-retry WM_TIMER(0x4d4d536c), and WM_PAINT belonging to main/its children. "
        <<"SafePoint runs before retrieval just as in WinMain. Unrelated posted messages, input, foreign HWNDs, "
        <<"project events, main timer100/101, close and quit are not dispatched. PeekMessage may itself deliver native sent messages.\n"
        <<"Bounds: 36 drains maximum, 256 rounds/240ms/96 dispatched messages per drain, 4096 messages per pass; "
        <<"at least two observed production clock advances per drain, with no forced FPS change. "
        <<"The snapshot taken after each native input permits its expected history transaction, but the subsequent pump "
        <<"must preserve all seven parameters, playhead, journal identity/revision/count/retained bytes. "
        <<"The containing test also verifies both original VMD key pools, identities and bytes.\n";
    const bool metrics=*ui_metrics::Output()!=0;
    std::array<std::uint64_t,ui_metrics::Count> beforeMetrics{};
    for(std::size_t i=0;i<beforeMetrics.size();++i)beforeMetrics[i]=ui_metrics::counts[i].load(std::memory_order_relaxed);
    trace::Trace observer(app.MainWindow(),controls);observer.QueuedUiMessages(true);
    unsigned dragged=0,live=0,painted=0,erased=0,thumbClears=0,parentChanges=0,peerChanges=0;
    try {
        const PumpInvariant initialization(app);
        UiPump pump(app,observer,report);
        initialization.Check();
        // Warm only naturally pending work. Startup is kept separate from all
        // native input samples so its erases cannot be attributed to a drag.
        pump.Drain("ui-pump-startup");
        for(auto control:controls){
            const auto initial=observer.Read(control);const int id=GetDlgCtrlID(control);
            const bool qualified=initial.Live()&&initial.Varied();live+=qualified;
            trace::Require(!requireLivePixels||qualified,"pumped-drag diagnosis has actual visible native pixels");
            RECT thumb{},channel{};
            SendMessageW(control,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(control,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
            const int x=(thumb.left+thumb.right)/2,y=(thumb.top+thumb.bottom)/2;
            const auto prefix=std::to_string(id)+"-real-drag-with-ui-pump";
            observer.Phase(prefix+"-button-down");
            SendMessageW(control,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
            SendMessageW(control,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
            trace::Require(GetCapture()==control,"native pumped drag captures its actual trackbar");
            int lastX=x;bool moved=false;unsigned sample=0;
            for(int fraction:{1,3,2,1}){
                const auto phase=prefix+"-move-"+std::to_string(++sample);
                observer.Phase(phase);
                lastX=int(channel.left+(channel.right-channel.left)*fraction/4);
                SendMessageW(control,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(lastX,y));
                const auto tick=int(SendMessageW(control,TBM_GETPOS,0,0));moved|=tick!=initial.tick;
                trace::Require(Authored(app,id,tick),"native pumped drag authors the original exact slider arithmetic");
                pump.Drain(phase);
                trace::Require(GetCapture()==control&&SendMessageW(control,TBM_GETPOS,0,0)==tick&&Authored(app,id,tick),
                    "queued UI work retains active native capture, position and just-authored parameter");
            }
            observer.Phase(prefix+"-button-up");
            SendMessageW(control,WM_LBUTTONUP,0,MAKELPARAM(lastX,y));
            const auto tick=int(SendMessageW(control,TBM_GETPOS,0,0));
            trace::Require(GetCapture()!=control&&moved&&Authored(app,id,tick),"pumped native drag moves and releases normally");
            pump.Drain(prefix+"-released");
            trace::Require(GetCapture()!=control&&SendMessageW(control,TBM_GETPOS,0,0)==tick&&Authored(app,id,tick),
                "queued release refresh retains the actual slider value and released capture");
            ++dragged;
            const auto final=observer.Read(control);
            trace::Require(!requireLivePixels||(final.Live()&&final.Varied()),"pumped native drag ends with actual track/thumb pixels");
            report<<"CONTROL id="<<id<<" tick_before="<<initial.tick<<" tick_after="<<tick
                <<" paints="<<final.paints-initial.paints<<" erases="<<final.erases-initial.erases
                <<" live="<<qualified<<" final_live="<<final.Live()<<" final_varied="<<final.Varied()<<'\n';
        }
        for(const auto& event:observer.Events()){
            trace::Require(!event.diagnosticItems&&event.nativeResult==event.result,
                "queued drag forwards each native message once and preserves all original return values");
            const bool drag=event.phase.find("-real-drag-with-ui-pump")!=std::string::npos;
            painted+=drag&&!event.parent&&event.message==WM_PAINT;
            erased+=drag&&!event.parent&&event.message==WM_ERASEBKGND;
            for(const auto& pair:event.targets){
                const auto& a=pair.before;const auto& b=pair.after;
                if(!a.Live()||!b.Live()||a.tick!=b.tick)continue;
                if(drag&&!event.parent&&event.message==WM_ERASEBKGND&&a.window==event.sourceWindow&&
                    a.ThumbReadable()&&b.ThumbReadable()&&!a.ThumbUniform()&&b.ThumbUniform()&&trace::ChangedIn(a,b,a.thumb)){
                    ++thumbClears;
                    report<<"NATURAL_ERASE seq="<<event.sequence<<" id="<<a.id<<" phase="<<event.phase
                        <<" same_tick="<<a.tick<<" old_thumb_changed="<<trace::ChangedIn(a,b,a.thumb)
                        <<" uniform_rgb="<<(b.pixels[std::size_t(b.thumb.top)*b.width+b.thumb.left]&0xffffffu)
                        <<" (readable real-window intermediate; compositor visibility/duration unproven)\n";
                }
                if(!drag||!trace::Changed(a,b)||a.paints!=b.paints||a.erases!=b.erases)continue;
                if(event.parent&&(event.message==WM_PAINT||event.message==WM_ERASEBKGND))++parentChanges;
                if(!event.parent&&(event.message==WM_PAINT||event.message==WM_ERASEBKGND)&&a.window!=event.sourceWindow)++peerChanges;
            }
        }
        report<<"SUMMARY controls="<<controls.size()<<" dragged="<<dragged<<" live="<<live
            <<" scheduled_clock_advances="<<pump.clockAdvances<<" safepoints="<<pump.safePoints<<" drains="<<pump.samples
            <<" dispatched="<<pump.dispatched<<" ui_refresh="<<pump.uiRefresh<<" coordinator_wake="<<pump.wakes
            <<" timers="<<pump.timers<<" dispatched_paints="<<pump.paints<<" drag_native_paints="<<painted
            <<" drag_native_erases="<<erased<<" same_tick_window_thumb_clears="<<thumbClears
            <<" parent_changes_without_child_paint="<<parentChanges<<" peer_changes_without_peer_paint="<<peerChanges<<'\n';
        report<<"METRICS enabled="<<metrics;
        if(metrics)for(std::size_t i=0;i<beforeMetrics.size();++i)
            report<<" counter"<<i<<'='<<ui_metrics::counts[i].load(std::memory_order_relaxed)-beforeMetrics[i];
        report<<" (disabled counters are unavailable; zero is not proof of no FrameDriver execution)\n"
            <<"Observation only: moving-thumb differences and memory DCs are not flash evidence; zero erase counts do not prove no visible flicker.\n";
        trace::Require(dragged==controls.size()&&pump.samples==36&&observer.Complete(),
            "all seven pumped drags complete without capture failure or trace quota truncation");
        observer.Detach();observer.Write(output);
        report<<"PASS: native input, production scheduling and data invariants complete; no visual-fix claim.\n";
        report.flush();trace::Require(bool(report),"finish the independent queued-drag report");
    }catch(...){observer.Detach();observer.Write(output);report<<"FAILED: queued-drag evidence retained; no flicker conclusion.\n";report.flush();throw;}
}
#endif
static Summary Run(mikudancestudio::MMDApp* app, const std::filesystem::path& output,
    bool expectedBound, bool requireLivePixels = true) {
    using namespace mikudancestudio;
    trace::Require(app && app == g_Block && app->MainWindow() && app->Renderer() && app->Renderer()->device,
        "slider paint tracing requires the actual isolated scene host and device");
    trace::Require(GetWindowThreadProcessId(app->MainWindow(), nullptr) == GetCurrentThreadId() &&
        IsWindowEnabled(app->MainWindow()) && !GetCapture() && !app->PlaybackActive() && !app->FrameStepPlayback(),
        "slider paint tracing requires an idle enabled UI-thread fixture without another capture");
#if !MMD_ORIGINAL_PROFILE
    trace::Require(bool(plugins::BoundNativePoseHistory(app)) == expectedBound, "trace mode matches actual native history binding");
#else
    trace::Require(!expectedBound, "original profile can trace only the actual ordinary route");
#endif
    std::array<HWND, 7> controls{};
    for (std::size_t i = 0; i < controls.size(); ++i) {
        controls[i] = GetDlgItem(app->MainWindow(), kIds[i]); RECT rect{};
        trace::Require(controls[i] && IsWindowVisible(controls[i]) && IsWindowEnabled(controls[i]) && GetClientRect(controls[i], &rect) &&
            rect.right >= 40 && rect.bottom >= 12, "expand camera/light bands and mount all seven real sliders before tracing");
    }
    const auto frame = app->CurrentFrame();
    trace::Require(app->CameraKeys() && app->LightKeys(), "tracing fixture has original camera and light key pools");
    const auto* cameraPool=app->CameraKeys();const auto* lightPool=app->LightKeys();
    const auto cameraIdentity=mdl::KeyPoolIdentity(cameraPool),lightIdentity=mdl::KeyPoolIdentity(lightPool);
    const std::vector<mdl::CameraKey> cameraKeys(app->CameraKeys(), app->CameraKeys() + mdl::KeyPoolCapacity(app->CameraKeys()));
    const std::vector<mdl::LightKey> lightKeys(app->LightKeys(), app->LightKeys() + mdl::KeyPoolCapacity(app->LightKeys()));
    const auto verifyKeys=[&]{
        trace::Require(app->CurrentFrame()==frame&&app->CameraKeys()==cameraPool&&app->LightKeys()==lightPool&&
            mdl::KeyPoolIdentity(app->CameraKeys())==cameraIdentity&&mdl::KeyPoolIdentity(app->LightKeys())==lightIdentity&&
            cameraKeys.size()==mdl::KeyPoolCapacity(app->CameraKeys())&&lightKeys.size()==mdl::KeyPoolCapacity(app->LightKeys())&&
            !std::memcmp(cameraKeys.data(),app->CameraKeys(),cameraKeys.size()*sizeof(mdl::CameraKey))&&
            !std::memcmp(lightKeys.data(),app->LightKeys(),lightKeys.size()*sizeof(mdl::LightKey)),
            "drawing, native drags and queued UI scheduling preserve playhead, VMD key allocation identities and all key bytes");
    };
    std::filesystem::create_directories(output); std::ofstream report(output / "paint-report.txt");
    trace::Require(bool(report), "create slider paint report");
    report << "Native slider erase/paint diagnostic; mode=" << (expectedBound ? "bound" : "ordinary")
        << "; no WM_PAINT substitution; no scene save; no claim of flicker removal.\n";
    report << "update_* is the native pending update bounding rectangle before default processing; "
        << "clip_* belongs to the supplied erase/print/CTLCOLOR/customdraw DC when borrowed_dc=1, otherwise a diagnostic GetDC snapshot. "
        << "The observer never calls BeginPaint before the native procedure.\n";
    report << "Every native message forwards once. baseline native_result==returned_result; item-notify-only changes an original "
        << "CDRF_DODEFAULT to exactly CDRF_NOTIFYITEMDRAW (32) at PREPAINT and CDRF_NOTIFYPOSTPAINT (16) at ITEMPREPAINT. "
        << "It never combines both requests at PREPAINT, skips native drawing, changes styles or substitutes paint. "
        << "dc_type=3 is a display DC, dc_type=10 a memory DC; supplied DC samples are never classified as live window snapshots. "
        << "Thumb-stage counts require an MM_TEXT simple clip containing the thumb; memory DCs also require the mapped thumb inside the selected bitmap. "
        << "Pixel capture flushes GDI and changes timing, so even a window intermediate is not proof of compositor-visible flicker.\n";
    using ThemeQuery = BOOL(WINAPI*)();
    const auto themeModule = GetModuleHandleW(L"uxtheme.dll");
    const auto themed = themeModule ? reinterpret_cast<ThemeQuery>(GetProcAddress(themeModule, "IsAppThemed")) : nullptr;
    report << "app_themed=" << (themed ? int(themed() != FALSE) : -1)
        << " parent_style=" << std::hex << GetWindowLongPtrW(app->MainWindow(), GWL_STYLE) << std::dec << '\n';
    report << "The metadata-item-only pass records native notifications in preallocated fixed storage and audits the final "
        << "parent-procedure LRESULT with WH_CALLWNDPROCRET. It performs no observer allocation, pixel capture or control getter inside "
        << "the native message stack. Original and full-sampling diagnostic passes are retained for comparison. "
        << "Metadata notifications use paint-return-audit.csv; native paint/erase totals below count full-sampling events only.\n";
    ClassIdentity(app->MainWindow(), report);
    for (auto control : controls) ClassIdentity(control, report);
    report.flush();
    Summary summary; trace::Trace observer(app->MainWindow(), controls);
    struct CaptureGuard { std::array<HWND, 7>& windows; ~CaptureGuard() {
        for (auto window : windows) if (GetCapture() == window) { SendMessageW(window, WM_CANCELMODE, 0, 0); if (GetCapture() == window) ReleaseCapture(); }
    } } capture{controls};
    try {
        // Native WM_PAINT warms theme state. WM_PRINTCLIENT is not used here.
        observer.Phase("warm-real-paint");
        for (auto control : controls) RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        Flush(app->MainWindow(), controls);
        for (auto control : controls) {
            const auto initial = observer.Read(control); ++summary.controls;
            const bool qualified = initial.Live() && initial.Varied(); summary.liveControls += qualified;
            report << "LIVE id=" << initial.id << " qualified=" << qualified << " clip=" << initial.clipKind << " varied=" << initial.Varied()
                << " tick=" << initial.tick << " size=" << initial.width << 'x' << initial.height
                << " style=" << std::hex << GetWindowLongPtrW(control, GWL_STYLE) << std::dec << '\n';
            if (requireLivePixels) trace::Require(qualified, "live slider pixels unavailable: use a visible on-screen fixture on a private desktop");
            observer.Phase(std::to_string(initial.id) + "-real-drag");
            RECT thumb{}, channel{}; SendMessageW(control, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(control, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            const int y = (thumb.top + thumb.bottom) / 2, x = (thumb.left + thumb.right) / 2;
            SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            trace::Require(GetCapture() == control, "actual native trackbar begins drag capture");
            bool moved = false; int lastX = x;
            for (int fraction : {1, 3, 2, 1}) {
                lastX = int(channel.left + (channel.right - channel.left) * fraction / 4);
                SendMessageW(control, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(lastX, y));
                Flush(app->MainWindow(), controls);
                const int tick = int(SendMessageW(control, TBM_GETPOS, 0, 0)); moved |= tick != initial.tick;
                trace::Require(Authored(*app, initial.id, tick), "native drag notification authors the exact FOV/RGB/direction value");
            }
            SendMessageW(control, WM_LBUTTONUP, 0, MAKELPARAM(lastX, y)); Flush(app->MainWindow(), controls);
            trace::Require(GetCapture() != control && moved, "native drag moves and releases without synthetic TBM_SETPOS"); ++summary.dragged;
            observer.Phase(std::to_string(initial.id) + "-same-tick-parent-paint");
            InvalidateRect(app->MainWindow(), nullptr, FALSE); UpdateWindow(app->MainWindow());
            ControlledErase(observer, control, report, summary);
        }
        for (auto control : controls) DetailedPaint(observer, app->MainWindow(), controls, control, report, summary, requireLivePixels);
        verifyKeys();
        trace::Require(observer.Complete(), "paint trace completed without capture exceptions or quota truncation");
    } catch (...) { observer.Write(output); report << "FAILED: evidence retained; no flicker conclusion.\n"; report.flush(); throw; }
    observer.Detach();
    observer.Write(output); // Keep raw evidence even if stage validation below fails.
    using StageKey = std::tuple<int,std::uint64_t,std::uintptr_t>;
    const auto finals=observer.FinalPaints();
    std::map<StageKey,const trace::Snapshot*> starts, thumbs;
    std::array<unsigned,7> baselineNotifications{}, itemNotifications{}, itemPrepaints{}, thumbPrepaints{}, thumbPostpaints{},
        itemDrawRequests{}, itemPostpaintRequests{}, memoryDcNotifications{}, windowDcNotifications{}, diagnosticMemoryDc{}, diagnosticWindowDc{};
    std::array<unsigned,7> metadataNotifications{}, metadataItems{}, metadataThumbPre{}, metadataThumbPost{}, metadataReturns{}, metadataReturnMismatches{};
    for (const auto& event : observer.Events()) {
        trace::Require(event.diagnosticItems || event.nativeResult == event.result, "original baseline never changes a native return value");
        summary.paintEvents += !event.parent && event.message == WM_PAINT;
        summary.eraseEvents += !event.parent && event.message == WM_ERASEBKGND;
        const bool dragging = event.phase.find("-real-drag") != std::string::npos && !event.diagnosticItems;
        summary.dragPaintEvents += dragging && !event.parent && event.message == WM_PAINT;
        summary.dragEraseEvents += dragging && !event.parent && event.message == WM_ERASEBKGND;
        if(event.customDraw){
            std::size_t index=0;while(index<kIds.size()&&kIds[index]!=event.dispatchId)++index;
            const bool controlled=event.phase.find("-controlled-native-")!=std::string::npos;
            if(event.diagnosticItems){++summary.diagnosticCustomDraw;if(index<kIds.size())++itemNotifications[index];}
            else if(controlled)++summary.controlledCustomDraw;
            else{++summary.baselineCustomDraw;if(index<kIds.size())++baselineNotifications[index];}
            summary.diagnosticReturnChanges += event.diagnosticItems && event.nativeResult!=event.result;
            if(event.diagnosticItems){
                if(event.nativeResult==CDRF_DODEFAULT&&event.drawStage==CDDS_PREPAINT){
                    trace::Require(event.result==CDRF_NOTIFYITEMDRAW,"diagnostic prepaint requests exactly native item notifications");
                    if(index<kIds.size())++itemDrawRequests[index];
                }else if(event.nativeResult==CDRF_DODEFAULT&&event.drawStage==CDDS_ITEMPREPAINT){
                    trace::Require(event.result==CDRF_NOTIFYPOSTPAINT,"diagnostic item prepaint requests exactly native item postpaint");
                    if(index<kIds.size())++itemPostpaintRequests[index];
                }else trace::Require(event.nativeResult==event.result,"diagnostics preserve all other native customdraw return values");
                if(index<kIds.size()){
                    if(event.drawStage==CDDS_ITEMPREPAINT){++itemPrepaints[index];if(event.item==TBCD_THUMB)++thumbPrepaints[index];}
                    if(event.drawStage==CDDS_ITEMPOSTPAINT&&event.item==TBCD_THUMB)++thumbPostpaints[index];
                }
            }
            for(const auto& pair:event.targets)if(pair.before.window==event.sourceWindow){
                const auto& value=pair.before;
                const auto final=finals.find(trace::Trace::Key(event,value));
                if(final!=finals.end()&&value.ThumbReadable()&&final->second->ThumbReadable()&&value.tick==final->second->tick&&
                    trace::ChangedIn(value,*final->second,value.thumb)){
                    if(event.diagnosticItems){
                        if(value.Live()&&final->second->Live())++summary.diagnosticWindowDiff;
                        else if(value.borrowedDc&&value.dcType==OBJ_MEMDC)++summary.diagnosticMemoryDiff;
                    }else if(event.phase.find("-baseline-detail-paint")!=std::string::npos&&value.Live()&&final->second->Live())
                        ++summary.baselineSameTickWindowDiff;
                }
                if(value.borrowedDc&&index<kIds.size()){
                    if(event.diagnosticItems){diagnosticMemoryDc[index]+=value.dcType==OBJ_MEMDC;diagnosticWindowDc[index]+=value.dcType==OBJ_DC;}
                    else if(!controlled){memoryDcNotifications[index]+=value.dcType==OBJ_MEMDC;windowDcNotifications[index]+=value.dcType==OBJ_DC;}
                }
                if(!event.diagnosticItems)continue;
                const StageKey key{value.id,value.paintSequence,value.borrowedDc?reinterpret_cast<std::uintptr_t>(event.suppliedDc):0};
                const bool memory=value.borrowedDc&&value.dcType==OBJ_MEMDC;
                if(event.drawStage==CDDS_PREPAINT)starts[key]=&value;
                else if(event.drawStage==CDDS_ITEMPREPAINT&&event.item==TBCD_THUMB){
                    thumbs[key]=&value;const auto found=starts.find(key);
                    if(found!=starts.end()&&found->second->ThumbReadable()&&value.ThumbReadable()&& !found->second->ThumbUniform()&&value.ThumbUniform()&&trace::ChangedIn(*found->second,value,value.thumb)){
                        if(value.Live()&&found->second->Live())++summary.windowThumbClears;
                        else if(memory)++summary.memoryThumbClears;
                    }
                }else if(event.drawStage==CDDS_ITEMPOSTPAINT&&event.item==TBCD_THUMB){
                    const auto found=thumbs.find(key);
                    if(found!=thumbs.end()&&found->second->ThumbReadable()&&value.ThumbReadable()&&found->second->ThumbUniform()&&!value.ThumbUniform()&&trace::ChangedIn(*found->second,value,value.thumb)){
                        if(value.Live()&&found->second->Live())++summary.windowThumbRestores;
                        else if(memory)++summary.memoryThumbRestores;
                    }
                }
            }
        }
        if(event.diagnosticItems)continue;
        for (const auto& pair : event.targets) {
            const auto& a = pair.before; const auto& b = pair.after;
            if (!a.Live() || !b.Live() || !trace::Changed(a, b) || a.tick != b.tick || a.paints != b.paints || a.erases != b.erases) continue;
            if (event.parent && event.message == WM_PAINT) ++summary.parentPixelChanges;
            if (event.parent && event.message == WM_ERASEBKGND) ++summary.parentErasePixelChanges;
            if (!event.parent && (event.message == WM_PAINT || event.message == WM_ERASEBKGND) && a.window != event.sourceWindow) ++summary.peerPixelChanges;
        }
    }
    for (std::size_t n=0;n<observer.MetadataCount();++n) {
        const auto& event=observer.MetadataEvents()[n];
        std::size_t index=0;while(index<kIds.size()&&kIds[index]!=event.id)++index;
        trace::Require(index<kIds.size(),"metadata records only the seven original native trackbars");
        if(event.nativeResult==CDRF_DODEFAULT&&event.stage==CDDS_PREPAINT)
            trace::Require(event.returnedResult==CDRF_NOTIFYITEMDRAW,"metadata prepaint requests exactly item notifications");
        if(event.nativeResult==CDRF_DODEFAULT&&event.stage==CDDS_ITEMPREPAINT)
            trace::Require(event.returnedResult==CDRF_NOTIFYPOSTPAINT,"metadata item prepaint requests exactly item postpaint");
        ++metadataNotifications[index];
        metadataItems[index]+=event.stage==CDDS_ITEMPREPAINT;
        metadataThumbPre[index]+=event.stage==CDDS_ITEMPREPAINT&&event.item==TBCD_THUMB;
        metadataThumbPost[index]+=event.stage==CDDS_ITEMPOSTPAINT&&event.item==TBCD_THUMB;
        metadataReturns[index]+=event.finalObserved;
        metadataReturnMismatches[index]+=!event.finalObserved||event.finalResult!=event.returnedResult;
    }
    bool fullStages=true, metadataStages=true, finalReturns=true;
    for(std::size_t i=0;i<kIds.size();++i){
        report<<"CUSTOMDRAW id="<<kIds[i]<<" baseline_notifications="<<baselineNotifications[i]<<" item_diagnostic_notifications="<<itemNotifications[i]
            <<" item_prepaints="<<itemPrepaints[i]<<" thumb_prepaints="<<thumbPrepaints[i]<<" thumb_postpaints="<<thumbPostpaints[i]
            <<" exact_item_requests="<<itemDrawRequests[i]<<" exact_item_post_requests="<<itemPostpaintRequests[i]
            <<" baseline_memory_dc="<<memoryDcNotifications[i]<<" baseline_display_dc="<<windowDcNotifications[i]
            <<" diagnostic_memory_dc="<<diagnosticMemoryDc[i]<<" diagnostic_display_dc="<<diagnosticWindowDc[i]
            <<" metadata_notifications="<<metadataNotifications[i]<<" metadata_item_prepaints="<<metadataItems[i]
            <<" metadata_thumb_prepaints="<<metadataThumbPre[i]<<" metadata_thumb_postpaints="<<metadataThumbPost[i]
            <<" metadata_final_returns="<<metadataReturns[i]<<" metadata_return_mismatches="<<metadataReturnMismatches[i]<<'\n';
        fullStages=fullStages&&baselineNotifications[i]&&itemNotifications[i]&&itemPrepaints[i]&&thumbPrepaints[i]&&thumbPostpaints[i]&&
            itemDrawRequests[i]&&itemPostpaintRequests[i];
        metadataStages=metadataStages&&metadataNotifications[i]&&metadataItems[i]&&metadataThumbPre[i]&&metadataThumbPost[i];
        finalReturns=finalReturns&&metadataNotifications[i]&&metadataReturns[i]==metadataNotifications[i]&&!metadataReturnMismatches[i];
    }
    report << "OBSERVATION full_sampling_item_stages="<<fullStages<<" metadata_only_item_stages="<<metadataStages
        <<" final_parent_returns_verified="<<finalReturns<<'\n';
    if (!metadataStages) report << "LIMITATION: metadata-only diagnosis also lacks required native thumb stages; "
        << "item-stage observation is unavailable in this recorded class/module path. Diagnosis remains FAILED; "
        << "absence of item notifications is not absence of visible flicker, and no drawing replacement is implied.\n";
    else if (!fullStages) report << "LIMITATION: metadata-only native thumb stages are present but full-sampling stages are missing; "
        << "the pixel/getter observer affects item-stage delivery. Full-sampling stage diagnosis remains FAILED.\n";
    if (!finalReturns) report << "LIMITATION: the final parent return audit is incomplete or disagrees with the observer; "
        << "do not infer what flags the trackbar consumed.\n";
    report.flush();
    report << "SUMMARY controls=" << summary.controls << " dragged=" << summary.dragged << " live_qualified=" << summary.liveControls
        << " native_paints=" << summary.paintEvents << " native_erases=" << summary.eraseEvents
        << " parent_changes_without_child_paint=" << summary.parentPixelChanges << " peer_changes_without_peer_paint=" << summary.peerPixelChanges
        << " controlled_erase_exposed_thumb=" << summary.eraseExposedThumb << '\n'
        << "DETAIL drag_paints="<<summary.dragPaintEvents<<" drag_erases="<<summary.dragEraseEvents
        <<" parent_erase_changes_without_child_paint="<<summary.parentErasePixelChanges
        <<" baseline_customdraw="<<summary.baselineCustomDraw<<" controlled_dib_customdraw="<<summary.controlledCustomDraw<<" diagnostic_customdraw="<<summary.diagnosticCustomDraw
        <<" diagnostic_return_changes="<<summary.diagnosticReturnChanges<<" diagnostic_controls="<<summary.diagnosticControls
        <<" diagnostic_window_thumb_clears="<<summary.windowThumbClears<<" diagnostic_memory_thumb_clears="<<summary.memoryThumbClears
        <<" diagnostic_window_thumb_restores="<<summary.windowThumbRestores<<" diagnostic_memory_thumb_restores="<<summary.memoryThumbRestores<<'\n'
        <<"SAME_TICK baseline_window_thumb_stage_diffs="<<summary.baselineSameTickWindowDiff
        <<" diagnostic_window_thumb_stage_diffs="<<summary.diagnosticWindowDiff
        <<" diagnostic_memory_thumb_stage_diffs="<<summary.diagnosticMemoryDiff<<'\n'
        << "Pixel changes identify reproducible drawing stages, not the duration or visibility of a desktop flash. "
        << "Zero counts do not prove absence of flicker; inspect live/DIB classification and before/after artifacts. "
        << "Same-paint final differences during a moving-thumb drag are expected motion and are not counted as same-tick flicker evidence.\n";
    report.flush(); trace::Require(bool(report), "finish slider paint report");
#if !MMD_ORIGINAL_PROFILE
    // This second observer starts only after the baseline observer is removed.
    // Run before the known item-stage assertion so absent native stage support
    // does not prevent the independent queued-refresh evidence being produced.
    try {
        PumpedDrags(*app,controls,output/"ui-pump",requireLivePixels);
        verifyKeys();
        report<<"UI_PUMP PASS; separate evidence in ui-pump/; both VMD key pools retain pointer, identity, capacity and bytes.\n";
    }catch(...){report<<"UI_PUMP FAILED; independent evidence retained in ui-pump/.\n";report.flush();throw;}
#else
    report<<"UI_PUMP N/A: original profile has no production update coordinator; existing native baseline and stage diagnosis remain applicable.\n";
#endif
    report.flush();trace::Require(bool(report),"finish independent queued-refresh status");
    trace::Require(finalReturns,"final native parent return values agree with fixed-buffer metadata observer");
    trace::Require(metadataStages,"metadata-only actual native trackbars report requested thumb pre/post stages");
    trace::Require(fullStages,"actual native trackbar reports original prepaint and requested thumb pre/post stages");
    std::printf("SLIDER_PAINT mode=%s controls=%u dragged=%u live=%u parent_pixel_changes=%u peer_pixel_changes=%u erase_exposed=%u (no visual-fix claim)\n",
        expectedBound ? "bound" : "ordinary", summary.controls, summary.dragged, summary.liveControls,
        summary.parentPixelChanges, summary.peerPixelChanges, summary.eraseExposedThumb);
    return summary;
}
} // namespace slider_paint_trace_host_checks
