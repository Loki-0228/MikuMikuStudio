// Include at file scope in the actual hidden-host harness. Invoke Run(app)
// after small_viewport_host_checks::Run, outside any UI lock or active gesture.
// This creates the production detached HWND; it never injects system input.
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/frame_presentation.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/scene_sampling.hpp"
#include "mikudancestudio/window_dpi_scope.hpp"
#include "../../src/plugins/history_runtime.hpp"
#include <commctrl.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bake_mic_guard_checks {
using namespace mikudancestudio;
inline void Require(bool condition,const char* text) {
    if(!condition)throw std::runtime_error(text);
}
template<class Key> struct KeyImage {
    Key* pointer;
    std::uint64_t identity;
    std::vector<Key> bytes;
    explicit KeyImage(Key* keys):pointer(keys),identity(mdl::KeyPoolIdentity(keys)) {
        const auto capacity=mdl::KeyPoolCapacity(keys);
        Require(keys&&identity&&capacity&&capacity<=20000,
            "Mic bake guard uses bounded registered real global key pools");
        bytes.assign(keys,keys+capacity);
    }
    void Check(Key* keys)const {
        Require(keys==pointer&&mdl::KeyPoolIdentity(keys)==identity&&
            mdl::KeyPoolCapacity(keys)==bytes.size()&&
            !std::memcmp(keys,bytes.data(),bytes.size()*sizeof(Key)),
            "Mic bake guard preserves every global key byte allocation and stable identity");
    }
};
struct SceneImage {
    MMDApp& app;
    KeyImage<mdl::CameraKey> camera;
    KeyImage<mdl::LightKey> light;
    KeyImage<mdl::SelfShadowKey> shadow;
    KeyImage<mdl::GravityKey> gravity;
    std::array<float,19> parameters;
    int frame,shadowMode;
    unsigned char modified,enhanced;
    double cursor,start,end;
    plugins::SharedHistory* history;
    std::uint64_t revision=0,retained=0;
    std::size_t undo=0,redo=0;
    static std::array<float,19> Values(MMDApp& a) {
        return {a.CameraPositionAxis(0),a.CameraPositionAxis(1),a.CameraPositionAxis(2),
            a.CameraRotation()[0],a.CameraRotation()[1],a.CameraRotation()[2],
            a.CameraDistance(),a.CameraFov(),a.LightColor()[0],a.LightColor()[1],a.LightColor()[2],
            a.LightDirection()[0],a.LightDirection()[1],a.LightDirection()[2],
            a.GravityX(),a.GravityY(),a.GravityZ(),a.GravityMagnitude(),a.ShadowDistance()};
    }
    explicit SceneImage(MMDApp& value):app(value),camera(value.CameraKeys()),light(value.LightKeys()),
        shadow(value.ShadowKeys()),gravity(value.GravityKeys()),parameters(Values(value)),
        frame(value.CurrentFrame()),shadowMode(value.state.selfShadowMode),
        modified(value.state.sceneModified),enhanced(value.state.enhancedModelDirty),
        cursor(value.PlaybackCursorSeconds()),start(value.PlaybackStartSeconds()),end(value.PlaybackEndSeconds()),
        history(plugins::NativeHistory(&value)) {
        if(history){Require(history->Revision(revision)==MMS_OK,"capture real Mic guard journal revision");
            retained=history->RetainedBytes();undo=history->UndoCount();redo=history->RedoCount();}
    }
    void Check()const {
        const auto actual=Values(app);
        Require(!std::memcmp(actual.data(),parameters.data(),sizeof(actual))&&app.CurrentFrame()==frame&&
            app.state.selfShadowMode==shadowMode&&app.state.sceneModified==modified&&
            app.state.enhancedModelDirty==enhanced&&app.PlaybackCursorSeconds()==cursor&&
            app.PlaybackStartSeconds()==start&&app.PlaybackEndSeconds()==end,
            "blocked detached input preserves unregistered pose playhead playback bounds and dirty flags");
        camera.Check(app.CameraKeys());light.Check(app.LightKeys());
        shadow.Check(app.ShadowKeys());gravity.Check(app.GravityKeys());
        Require(plugins::NativeHistory(&app)==history,"Mic guard preserves actual common-history binding");
        if(history){std::uint64_t actualRevision=0;
            Require(history->Revision(actualRevision)==MMS_OK&&actualRevision==revision&&
                history->RetainedBytes()==retained&&history->UndoCount()==undo&&history->RedoCount()==redo,
                "blocked detached input creates no adjustment explicit edit redo truncation or retained history");}
    }
};
inline std::array<int,13> InputValues(MMDApp& a) {
    return {a.MouseX(),a.MouseY(),a.PreviousMouseX(),a.PreviousMouseY(),
        a.LeftMouseButtonState(),a.RightMouseButtonState(),a.MiddleMouseButtonState(),
        a.ShiftModifierState(),a.CtrlModifierState(),int(a.ViewToolDragOperation()),
        int(a.InteractionDragMode()),int(a.ViewportToolOperation()),int(a.ViewportToolHovered())};
}
inline void DispatchPosted(HWND window,UINT kind,WPARAM wp=0,LPARAM lp=0) {
    MSG pending{};
    Require(!PeekMessageW(&pending,window,kind,kind,PM_NOREMOVE),
        "Mic fixture does not consume an unrelated pending message of the tested kind");
    Require(PostMessageW(window,kind,wp,lp)!=FALSE,"queue a real detached-window message");
    Require(PeekMessageW(&pending,window,kind,kind,PM_REMOVE)&&pending.hwnd==window&&pending.message==kind,
        "retrieve exactly the queued detached-window message without a broad message pump");
    DispatchMessageW(&pending);
}
struct Fixture {
    MMDApp& app;
    std::unique_ptr<MMDAppState> saved;
    double cursor,start,end;
    explicit Fixture(MMDApp& a):app(a),saved(new MMDAppState(a.state)),
        cursor(a.PlaybackCursorSeconds()),start(a.PlaybackStartSeconds()),end(a.PlaybackEndSeconds()) {}
    ~Fixture() {
        // Exception cleanup must not leave a newly created top-level window or
        // reparented controls in the next host check. Locks unwind before here.
        try{if(app.FloatingWindow()&&IsWindow(app.FloatingWindow()))SaveFlagSubsystem(&app);}catch(...){}
        app.state=*saved;app.PlaybackCursorSeconds()=cursor;
        app.PlaybackStartSeconds()=start;app.PlaybackEndSeconds()=end;
        try{HandleWindowSize(&app);RefreshMainWindowViewport(&app);PostViewRefresh(&app);}catch(...){}
    }
};
struct HiddenCreateMenu {
    HMENU menu;
    UINT previous;
    explicit HiddenCreateMenu(HWND main):menu(GetMenu(main)),previous(GetMenuState(menu,0x119,MF_BYCOMMAND)) {
        Require(menu&&previous!=UINT(-1),"actual host menu contains the detached topmost option");
        // InitFlagSubsystem's unchecked-topmost branch uses SetWindowPos with
        // 0x43 (including SWP_SHOWWINDOW), beyond the host's ShowWindow detour.
        // Select the existing alternative only during actual HWND creation.
        Require(CheckMenuItem(menu,0x119,MF_BYCOMMAND|MF_CHECKED)!=DWORD(-1),
            "temporarily select the native detached creation branch without SWP_SHOWWINDOW");
    }
    ~HiddenCreateMenu(){CheckMenuItem(menu,0x119,MF_BYCOMMAND|(previous&MF_CHECKED));}
};
inline void Alive(MMDApp& app,HWND mic) {
    Require(IsWindow(mic)&&app.FloatingWindow()==mic,
        "bake-locked close cannot destroy the live detached HWND held by sampling state");
    for(int id=536;id<=557;++id)
        Require(GetDlgItem(mic,id)&&GetParent(GetDlgItem(mic,id))==mic,
            "bake-locked close keeps every production detached control parent unchanged");
}
inline void BlockedInput(MMDApp& app,HWND mic) {
    Require(bake::UiLocked(),"exercise detached rejection inside the actual bake UI gate");
    const auto values=SceneImage::Values(app);
    const auto input=InputValues(app);
    const auto frame=app.CurrentFrame();const auto capture=GetCapture();
    SendMessageW(mic,WM_COMMAND,MAKEWPARAM(537,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(mic,537)));
    SendMessageW(mic,WM_MOUSEWHEEL,MAKEWPARAM(0,120),0);
    SendMessageW(mic,WM_MOUSEMOVE,MK_SHIFT|MK_CONTROL,MAKELPARAM(73,91));
    SendMessageW(mic,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(73,91));
    SendMessageW(mic,WM_LBUTTONUP,0,MAKELPARAM(73,91));
    SendMessageW(mic,WM_RBUTTONDBLCLK,0,MAKELPARAM(73,91));
    SendMessageW(mic,WM_MBUTTONDOWN,MK_MBUTTON,MAKELPARAM(73,91));
    SendMessageW(mic,WM_MBUTTONUP,0,MAKELPARAM(73,91));
    for(UINT message:{WM_KEYDOWN,WM_KEYUP,WM_CHAR,WM_SYSKEYDOWN,WM_SYSKEYUP})
        SendMessageW(mic,message,VK_RIGHT,0);
    SendMessageW(mic,WM_HSCROLL,SB_THUMBTRACK,reinterpret_cast<LPARAM>(GetDlgItem(app.MainWindow(),447)));
    SendMessageW(mic,WM_VSCROLL,SB_LINEDOWN,reinterpret_cast<LPARAM>(GetDlgItem(app.MainWindow(),427)));
    NMHDR header{};header.hwndFrom=GetDlgItem(mic,537);header.idFrom=537;header.code=NM_CLICK;
    SendMessageW(mic,WM_NOTIFY,header.idFrom,reinterpret_cast<LPARAM>(&header));
    auto actual=SceneImage::Values(app);
    Require(!std::memcmp(actual.data(),values.data(),sizeof(actual))&&app.CurrentFrame()==frame&&
        InputValues(app)==input&&GetCapture()==capture,
        "locked command wheel key scroll notify and mouse input changes no scene or input ownership");
    SendMessageW(mic,WM_CLOSE,0,0);Alive(app,mic);
    SendMessageW(mic,WM_SYSCOMMAND,SC_CLOSE|3,0);Alive(app,mic);
    DispatchPosted(mic,WM_COMMAND,MAKEWPARAM(537,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(mic,537)));
    DispatchPosted(mic,WM_MOUSEWHEEL,MAKEWPARAM(0,120));
    DispatchPosted(mic,WM_CLOSE);Alive(app,mic);
    DispatchPosted(mic,WM_SYSCOMMAND,SC_CLOSE|3);Alive(app,mic);
    actual=SceneImage::Values(app);
    Require(!std::memcmp(actual.data(),values.data(),sizeof(actual))&&app.CurrentFrame()==frame&&
        InputValues(app)==input&&GetCapture()==capture,
        "queued detached commands and close requests obey the same bake gate as synchronous input");
}
inline void ResponsiveLayout(MMDApp& app,HWND mic) {
    const WindowDpiScope dpi(mic);
    const auto previous=app.ViewportRect();
    Require(SetWindowPos(mic,nullptr,0,0,672,512,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,
        "real detached client resize remains enabled during the bake gate");
    D3DVIEWPORT9 viewport{};
    Require(!IsRectEmpty(&app.ViewportRect())&&!EqualRect(&previous,&app.ViewportRect())&&
        SUCCEEDED(app.Renderer()->device->GetViewport(&viewport))&&
        viewport.X==DWORD(app.ViewportRect().left)&&viewport.Y==DWORD(app.ViewportRect().top)&&
        viewport.Width==DWORD(app.ViewportRect().right-app.ViewportRect().left)&&
        viewport.Height==DWORD(app.ViewportRect().bottom-app.ViewportRect().top),
        "locked WM_SIZE still publishes real detached client geometry accepted by the device");
    app.ViewportRect()={};SendMessageW(mic,WM_MOVE,0,0);
    Require(!IsRectEmpty(&app.ViewportRect()),"locked WM_MOVE still refreshes the actual device viewport");
    Require(InvalidateRect(mic,nullptr,FALSE)!=FALSE,"invalidate the locked detached native window");
    UpdateWindow(mic);
    Require(!GetUpdateRect(mic,nullptr,FALSE),"locked detached WM_PAINT validates its real update region");
    const auto left=app.LeftMouseButtonState();app.LeftMouseButtonState()=2;
    SendMessageW(mic,WM_CAPTURECHANGED,0,reinterpret_cast<LPARAM>(app.MainWindow()));
    Require(app.LeftMouseButtonState()==0||app.LeftMouseButtonState()==3,
        "bake gate still forwards capture loss to native release cleanup");
    app.LeftMouseButtonState()=left;
}
inline void Run(MMDApp* app) {
    Require(app&&app==g_Block&&app->MainWindow()&&app->Renderer()&&app->Renderer()->device&&
        !app->FloatingWindow()&&!app->RecordingWindow()&&!app->FullscreenMode()&&!app->PlaybackActive()&&
        !GetCapture()&&!bake::UiLocked()&&!IsWindowVisible(app->MainWindow()),
        "Mic bake guard starts in the real hidden idle windowed host without a foreign capture");
    Fixture fixture(*app);
    app->CameraMode()=1;app->CurrentFrame()=37;app->PlaybackCursorSeconds()=37.0/30.0;
    app->CameraPositionAxis(0)=12.5f;app->CameraDistance()=-47.25f;
    app->SeparateWindowWidth()=640;app->SeparateWindowHeight()=480;app->SeparateWindowMaximized()=0;
    {HiddenCreateMenu hiddenCreate(app->MainWindow());SetLastError(ERROR_SUCCESS);InitFlagSubsystem(app);}
    const DWORD creationError=GetLastError();const HWND mic=app->FloatingWindow();
    wchar_t className[64]{};const bool live=mic&&IsWindow(mic),visible=live&&IsWindowVisible(mic);
    if(live)GetClassNameW(mic,className,64);
    if(!live||visible||std::wcscmp(className,L"MicWindow"))
        std::fprintf(stderr,"Mic fixture creation: hwnd=%p live=%u visible=%u class=%ls last_win32_error=%lu style=%llx exstyle=%llx\n",
            static_cast<void*>(mic),live?1u:0u,visible?1u:0u,className,creationError,
            static_cast<unsigned long long>(mic?GetWindowLongPtrW(mic,GWL_STYLE):0),
            static_cast<unsigned long long>(mic?GetWindowLongPtrW(mic,GWL_EXSTYLE):0));
    Require(live,"create a live production Mic HWND under the hidden-host guard");
    Require(!std::wcscmp(className,L"MicWindow"),"created detached host retains the actual registered MicWindow procedure class");
    Require(!visible,"production Mic HWND stays hidden without a SetWindowPos show side effect");
    Alive(*app,mic);SceneImage scene(*app);
    {
        bake::ScopedUiLock lock(nullptr);
        Require(bake::UiLocked()&&!IsWindowEnabled(app->MainWindow())&&!IsWindowEnabled(mic),
            "real bake lock disables both native top-level windows");
        BlockedInput(*app,mic);ResponsiveLayout(*app,mic);scene.Check();
        std::string error;unsigned callbacks=0,outputs=0;
        Require(scene_sampling::Visit(*app,1,1,30,[&](double,bool output){
            Require(app->PlaybackActive()&&app->CameraMode()==1,
                "close protection executes while production sampling actually borrows private scene state");
            BlockedInput(*app,mic);++callbacks;outputs+=output;return true;
        },error)&&callbacks==2&&outputs==1,
            "actual preroll and output sampling both survive queued detached close and input");
        Alive(*app,mic);scene.Check();
    }
    Require(!bake::UiLocked()&&IsWindowEnabled(mic),"bake completion restores the real detached window input state");
    DispatchPosted(mic,WM_CLOSE);
    Require(!app->FloatingWindow()&&!IsWindow(mic)&&IsWindow(app->MainWindow()),
        "normal detached close after the bake unlock destroys only Mic and restores the main host");
    for(int id=536;id<=557;++id)
        Require(GetDlgItem(app->MainWindow(),id)&&GetParent(GetDlgItem(app->MainWindow(),id))==app->MainWindow(),
            "normal close reparents every production viewport control to the live main window");
    RefreshMainWindowViewport(app);
    Require(FrameViewportRenderable(app),"main viewport remains usable after the normally completed detached close");
    scene.Check();
    std::printf("PASS BAKE_MIC_GUARD synchronous_and_queued_input=1 close_ownership=1 sampling_preroll_and_output=1 layout_and_capture_cleanup=1 normal_close=1 journal_checked=%u\n",scene.history?1u:0u);
}
} // namespace bake_mic_guard_checks
