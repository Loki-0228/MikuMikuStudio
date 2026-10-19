// Opt-in manual diagnostic, never a CTest/no-flicker assertion. The user
// activates the window and supplies input; the product update loop stays live.
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/bottom_panel_resize.hpp"
#include "mikudancestudio/plugin_menu.hpp"
#include "mikudancestudio/update_coordinator.hpp"
#include "slider_desktop_capture.hpp"
#include "slider_real_input_metadata.inc"
#include <algorithm>

namespace slider_user_capture_checks {
struct Control {
    bool start=false,cancel=false;
    HWND window=nullptr,main=nullptr;
    ~Control(){if(window&&IsWindow(window))DestroyWindow(window);}
    static LRESULT CALLBACK Main(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR reference){
        auto& self=*reinterpret_cast<Control*>(reference);
        if(message==WM_CLOSE){self.cancel=true;return 0;}
        return DefSubclassProc(hwnd,message,wp,lp);
    }
    static LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Control*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Control*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self){
            if(message==WM_CLOSE){self->cancel=true;return 0;}
            if(message==WM_COMMAND&&LOWORD(wp)==1&&HIWORD(wp)==BN_CLICKED){self->start=true;return 0;}
        }
        return DefWindowProcW(hwnd,message,wp,lp);
    }
    void Create(HWND host){
        main=host;
        WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);
        wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
        wc.lpszClassName=L"MMS.Test.SliderUserCapture";
        small_viewport_host_checks::Require(RegisterClassW(&wc)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS,
            "register manual slider diagnostic controller");
        window=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"MMS 滑杆闪动诊断",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,490,195,host,nullptr,wc.hInstance,this);
        small_viewport_host_checks::Require(window!=nullptr,"create manual slider diagnostic controller");
        const auto label=CreateWindowExW(0,L"STATIC",
            L"点击开始后，拖动侧栏或底部面板的分隔线约 30 秒。\n分隔线处鼠标应变为左右箭头，按住后左右拖动。\n保持 175% 缩放；记录滑杆区域，结束后自动关闭。",
            WS_CHILD|WS_VISIBLE,12,12,460,70,window,nullptr,wc.hInstance,nullptr);
        const auto button=CreateWindowExW(0,L"BUTTON",L"开始记录（30 秒）",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
            110,94,260,30,window,reinterpret_cast<HMENU>(1),wc.hInstance,nullptr);
        small_viewport_host_checks::Require(label&&button,"create manual diagnostic instructions and start button");
        for(auto hwnd:{label,button})SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        small_viewport_host_checks::Require(SetWindowSubclass(main,Main,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this))!=FALSE,
            "observe manual diagnostic close without destroying the product host");
        realShow(window,SW_SHOWNORMAL);
    }
    void Detach()noexcept{if(main&&IsWindow(main))RemoveWindowSubclass(main,Main,reinterpret_cast<UINT_PTR>(this));main=nullptr;}
};

inline void ProductDpi(){
    // Same pre-HWND policy as WinMain. A compatibility environment variable
    // alone cannot establish the actual awareness of this UI thread.
    using Set=HANDLE(WINAPI*)(HANDLE);
    const auto set=reinterpret_cast<Set>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext"));
    const auto old=set?set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-1))):nullptr;
    std::printf("SLIDER_USER_DPI product_unaware_request=%d api_available=%d error=%lu\n",int(old!=nullptr),int(set!=nullptr),old?0ul:GetLastError());
    if(set&&!old)throw std::runtime_error("cannot establish product UI DPI context before creating windows");
}

inline void Describe(MMDApp& app,const std::filesystem::path& output){
    using WindowContext=HANDLE(WINAPI*)(HWND);using Equal=BOOL(WINAPI*)(HANDLE,HANDLE);using Dpi=UINT(WINAPI*)(HWND);
    const auto user=GetModuleHandleW(L"user32.dll");
    const auto context=reinterpret_cast<WindowContext>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
    const auto equal=reinterpret_cast<Equal>(GetProcAddress(user,"AreDpiAwarenessContextsEqual"));
    const auto dpi=reinterpret_cast<Dpi>(GetProcAddress(user,"GetDpiForWindow"));
    std::ofstream f(output/"host.txt");if(!f)throw std::runtime_error("write manual capture host metadata");
    f<<"Manual actual-input diagnostic. Empty isolated scene; production full-profile update coordinator.\n"
        <<"No synthetic input or activation retry. Start requires user click.\n"
        <<"The window's DPI value is not the monitor's scale when it is DPI-unaware.\n"
        <<"No-flicker is not asserted; the recorder and metadata queries may change timing.\n";
    RECT client{};GetClientRect(app.MainWindow(),&client);
    f<<"client="<<client.right<<','<<client.bottom<<" sidebar_width="<<app.SidebarWidth()
        <<" bottom_band_top="<<client.bottom-157<<" camera="<<int(app.CameraMode())<<'\n';
    for(const auto& panel:BottomPanelSpans(&app))f<<"bottom_separator panel="<<panel.id
        <<" x="<<BottomPanelX(&app,panel.right)<<" width="<<panel.right-panel.left<<" extra="<<panel.extra<<'\n';
    for(int id:{0,447,455,456,457,458,459,460,505,510,515,520,534,560}){
        const auto hwnd=id?GetDlgItem(app.MainWindow(),id):app.MainWindow();RECT rect{};GetWindowRect(hwnd,&rect);
        const auto awareness=context?context(hwnd):nullptr;
        f<<"id="<<id<<" hwnd="<<reinterpret_cast<std::uintptr_t>(hwnd)<<" dpi="<<(dpi?dpi(hwnd):0)
            <<" unaware="<<(awareness&&equal?equal(awareness,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-1))):-1)
            <<" unaware_gdi_scaled="<<(awareness&&equal?equal(awareness,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-5))):-1)
            <<" caller_context_rect="<<rect.left<<','<<rect.top<<','<<rect.right<<','<<rect.bottom<<'\n';
    }
    if(!f)throw std::runtime_error("finish manual capture host metadata");
}

inline void Run(MMDApp& app,const std::filesystem::path& output){
    std::filesystem::create_directories(output);
    app.CameraMode()=1;PostLanguageSweep2(&app);PostViewRefresh(&app);
    RECT work{};small_viewport_host_checks::Require(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)!=FALSE,"read diagnostic work area");
    const int width=std::min(1380,int(work.right-work.left)-40),height=std::min(820,int(work.bottom-work.top)-90);
    small_viewport_host_checks::Require(width>=600&&height>=400,"screen has space for diagnostic host");
    small_viewport_host_checks::ClientSize(app,width,height);
    SetWindowPos(app.MainWindow(),nullptr,work.left+10,work.top+10,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    SetWindowTextW(app.MainWindow(),L"MMS 布局拖动诊断：请在提示窗口点击开始");
    realShow(app.MainWindow(),SW_SHOWNOACTIVATE);
    // The first visible menu can wrap and change the final client height.
    // Reconcile child geometry with that actual visible client before input.
    HandleWindowSize(&app);RefreshMainWindowViewport(&app);
    RedrawWindow(app.MainWindow(),nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    Describe(app,output);
    autosave::Initialize(&app);
    small_viewport_host_checks::Require(updates::Initialize(&app),"start actual product update coordinator");
    struct Updates {MMDApp& app;~Updates(){updates::Shutdown(&app);autosave::Shutdown();}}updatesGuard{app};
    app.MilliToSec()=0.001f;
    Control control;
    struct Gate {Control& control;~Gate(){control.Detach();}}gate{control};
    control.Create(app.MainWindow());
    slider_real_input_metadata::Trace trace(app);
    small_viewport_host_checks::Require(trace.Attach(),"attach fixed-ring actual-input trace");
    std::array<HWND,13> sliders{};
    for(unsigned i=0;i<sliders.size();++i)sliders[i]=GetDlgItem(app.MainWindow(),slider_real_input_metadata::kIds[i]);
    slider_desktop_capture::Recorder recorder(app.MainWindow(),GetCurrentThreadId(),sliders,trace.SequenceAddress(),trace.LayoutDragAddress());
    const auto waiting=GetTickCount64();bool armed=false,timeout=false,quit=false;MSG msg{};
    std::printf("SLIDER_USER_WAITING: click the diagnostic Start button; no input will be generated\n");
    while(!control.cancel&&!quit){
        updates::SafePoint(&app);
        if(control.start&&!armed){
            control.start=false;realShow(control.window,SW_HIDE);
            // One request after the user's button click. Never retry or steal
            // focus from an unrelated application; capture checks foreground.
            const BOOL requested=SetForegroundWindow(app.MainWindow());
            SetWindowTextW(app.MainWindow(),L"MMS 布局拖动诊断：拖动侧栏或底部分隔线，30 秒后自动关闭");
            trace.SetPhase(1);
            small_viewport_host_checks::Require(recorder.Start(output),"start bounded independent slider ROI recorder");
            armed=true;
            std::printf("SLIDER_USER_ARMED foreground_requested=%d actual_self=%d\n",int(requested),int(GetForegroundWindow()==app.MainWindow()));
        }
        if(armed&&recorder.Done())break;
        if(!armed&&GetTickCount64()-waiting>=120000){timeout=true;break;}
        if(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT){quit=true;break;}
            if(!bake::UiLocked()&&msg.message==WM_KEYDOWN&&!(msg.lParam&(LPARAM(1)<<30))&&
                (msg.hwnd==app.MainWindow()||IsChild(app.MainWindow(),msg.hwnd))&&
                (plugin_menu::HandleShortcut(&app,unsigned(msg.wParam))||plugins::HandleShortcut(unsigned(msg.wParam),msg.hwnd==app.MainWindow())))continue;
            if(msg.message==WM_SYSKEYDOWN&&msg.wParam==VK_RETURN&&(msg.lParam&(LPARAM(1)<<29))&&
                (msg.hwnd==app.MainWindow()||IsChild(app.MainWindow(),msg.hwnd)||
                 (app.FloatingWindow()&&(msg.hwnd==app.FloatingWindow()||IsChild(app.FloatingWindow(),msg.hwnd))))){
                SendMessageW(app.MainWindow(),msg.message,msg.wParam,msg.lParam);continue;
            }
            if(plugins::HandleDialogMessage(&msg))continue;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }else MsgWaitForMultipleObjectsEx(0,nullptr,std::min<DWORD>(50,updates::WaitMilliseconds(&app)),QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    trace.SetPhase(2);recorder.Stop();
    // A storage limit can be reached while the user is still holding the
    // thumb. End only this diagnostic host's capture before disk I/O, keeping
    // the cancellation in the trace. Never release another window's capture.
    const auto captured=GetCapture();
    const bool ownCapture=captured&&GetWindowThreadProcessId(captured,nullptr)==GetCurrentThreadId()&&
        (captured==app.MainWindow()||IsChild(app.MainWindow(),captured));
    if(ownCapture){SendMessageW(captured,WM_CANCELMODE,0,0);if(GetCapture()==captured)ReleaseCapture();}
    realShow(app.MainWindow(),SW_HIDE);
    realShow(control.window,SW_HIDE);
    trace.Detach();
    if(armed)recorder.Finish();
    trace.Write(output);
    std::ofstream session(output/"session.txt");
    session<<"armed="<<armed<<"\ncancelled="<<control.cancel<<"\nwaiting_timeout="<<timeout<<"\nquit="<<quit
        <<"\nown_capture_cancelled_before_output="<<ownCapture<<"\ninput_messages="<<trace.InputCount()
        <<"\nNo visual pass/fail assertion. See desktop-summary.txt; zero frames means not-observed.\n";
    if(!session)throw std::runtime_error("write actual-input diagnostic session");
    std::printf("SLIDER_USER_FINISHED armed=%d cancelled=%d waiting_timeout=%d input_messages=%llu; observation only, no no-flicker verdict\n",
        int(armed),int(control.cancel),int(timeout),static_cast<unsigned long long>(trace.InputCount()));
}
} // namespace slider_user_capture_checks
