#include "native_model_outline_ui.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_model_outline.hpp"
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_control_readout.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace mikumikustudio::plugins {
namespace {
constexpr UINT_PTR kOutlineSubclass=0x4d4d534f;
constexpr wchar_t kControlled[]=L"MMS.Outline.Controlled.V1";
struct Gesture {
    NativeModelOutlinePlan plan;
    SharedHistory* history=nullptr;
    std::uint64_t binding=0,revision=0;
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    ~Gesture(){plan.Cancel();token.reset();}
};
struct Session {
    MMDApp& app;HWND dialog,edit,slider,main;DWORD thread=GetCurrentThreadId();
    unsigned slot=0;unsigned char* target=nullptr;MmsId id{};
    std::uint64_t scene=0;
    std::unique_ptr<Gesture> gesture;
    unsigned cycle=0,keyMask=0;
    bool suppress=false,busy=false,closing=false,releasing=false,draft=false,closeMain=false,closeDialog=false;
    float baseline=0,origin=0;
    int originTick=0;
    int pointerX=0,pointerY=0;
    bool pointerSample=false,pointerAtOrigin=false,pointerOrigin=false;
    Session(MMDApp& a,HWND h):app(a),dialog(h),edit(GetDlgItem(h,panel::kEdgeThicknessEdit)),slider(GetDlgItem(h,panel::kEdgeThicknessSlider)),main(a.MainWindow()){}
};
thread_local std::shared_ptr<Session> session;
bool Same(float a,float b)noexcept{return !std::memcmp(&a,&b,sizeof a);}
bool Own(const Session& s)noexcept{
    return !s.closing&&GetCurrentThreadId()==s.thread&&s.app.state.edgeThicknessDialog==s.dialog&&IsWindow(s.dialog)&&
        GetParent(s.edit)==s.dialog&&GetParent(s.slider)==s.dialog;
}
bool Target(const Session& s)noexcept{
    return s.scene==s.app.SceneGeneration()&&s.slot<kModelSlotCount&&s.target&&s.app.ModelSlot(s.slot)==s.target&&
        SceneObjectPresent(s.target,s.id);
}
bool Selected(const Session& s)noexcept{return Target(s)&&!s.app.CameraMode()&&s.app.SelectedModelSlot()==s.slot;}
bool Ready(const Session& s)noexcept{
    auto& a=s.app;const auto capture=GetCapture();
    return Own(s)&&Selected(s)&&!s.busy&&!bake::UiLocked()&&!SceneSaveBlocksEdits()&&!a.SceneMutationInProgress()&&
        !NativeObjectUiPending(&a)&&!a.PlaybackActive()&&!a.FrameStepPlayback()&&!a.state.recordPlaybackStartPending&&
        !a.RecordingWindow()&&!a.FrameRangeDialog()&&!a.state.frameCopyDialog&&!a.GravitySettingDialog()&&
        !a.GroundShadowColorDialog()&&!a.AccessoryOrderArray()&&!a.state.accessoryEditDialogOpen&&(!capture||capture==s.slider);
}
struct Busy {
    Session& s;bool before;
    explicit Busy(Session& value):s(value),before(value.busy){s.busy=true;}
    ~Busy(){s.busy=before;if(!before&&!s.closing){
        if(s.closeMain&&IsWindow(s.main)){s.closeMain=false;PostMessageW(s.main,WM_CLOSE,0,0);}
        if(s.closeDialog&&IsWindow(s.dialog)){s.closeDialog=false;PostMessageW(s.dialog,WM_CLOSE,0,0);}
    }}
};
struct Suppress { Session& s;bool before;explicit Suppress(Session& value):s(value),before(s.suppress){s.suppress=true;}~Suppress(){s.suppress=before;} };
int PointerX(LPARAM lp)noexcept{return static_cast<short>(LOWORD(lp));}
int PointerY(LPARAM lp)noexcept{return static_cast<short>(HIWORD(lp));}
// Win32 maps thumb pixels through a quantized table that is not the inverse of
// tick/100, so leaving and returning to the mouse-down pixel can report the
// neighbouring tick (the 0.625=62 thumb centre reports 61). While one real
// pointer message and its synchronous WM_HSCROLL run, the exact mouse-down
// pixel means the precise origin value again. The RAII restores the flags so a
// key or wheel notification never inherits a stale pointer sample.
struct PointerSample {
    Session& s;bool sample,atOrigin;
    PointerSample(Session& value,bool pointer,LPARAM point):s(value),sample(s.pointerSample),atOrigin(s.pointerAtOrigin){
        s.pointerSample=pointer&&s.cycle==1;
        s.pointerAtOrigin=s.pointerSample&&s.pointerOrigin&&PointerX(point)==s.pointerX&&PointerY(point)==s.pointerY;
    }
    ~PointerSample(){s.pointerSample=sample;s.pointerAtOrigin=atOrigin;}
};
// The original controller addresses the trackbar in whole percent ticks. An
// authored value that is exactly float(tick/100) must echo on that same tick,
// otherwise the next native step is swallowed (1.39f echoed as 138, so Right
// to 139 produced 1.39f again and wrote no history). Nearest wins only when it
// round-trips to the exact float; every other value keeps the inherited
// truncation that shows 0.625 as 62. Clamping in double first keeps a
// maximum-float value from overflowing the integer conversion.
int DisplayTick(float value)noexcept{
    const double scaled=(std::clamp)(double(value)*100.0,0.0,200.0);
    const int nearest=static_cast<int>(std::lround(scaled));
    return Same(float(double(nearest)/100.0),value)?nearest:static_cast<int>(scaled);
}
void Echo(Session& s,bool force=false)noexcept{
    if(!Own(s)||!Target(s))return;
    const auto value=mdl::Mdl(s.target)->edgeScale;if(!std::isfinite(value))return;
    Suppress suppress(s);
    if(force||!s.draft){char text[96]{};sprintf_s(text,"%3.2f",double(value));ui::SetReadoutTextIfChangedA(s.edit,text);
        if(!Own(s))return;SendMessageA(s.edit,EM_SETMODIFY,FALSE,0);}
    if(!Own(s))return;
    ui::SetTrackbarPositionIfChanged(s.slider,DisplayTick(value));
}
// One user cycle anchors the exact authored float and the tick the controller
// displayed for it. Native press/release and boundary navigation still notify,
// and never leaving the start tick is not a sample: tick/100 would round a
// non-percentile value (or overwrite a clamped negative/large one) and publish
// a history step for a no-op.
void BeginCycle(Session& s,unsigned cycle)noexcept{
    s.origin=mdl::Mdl(s.target)->edgeScale;
    s.originTick=int(SendMessageA(s.slider,TBM_GETPOS,0,0));
    s.cycle=cycle;
}
bool Cancel(Session& s)noexcept{
    Busy busy(s);auto gesture=std::move(s.gesture);s.cycle=0;s.keyMask=0;s.origin=0;s.originTick=0;
    s.pointerX=0;s.pointerY=0;s.pointerSample=false;s.pointerAtOrigin=false;s.pointerOrigin=false;
    const bool restored=gesture&&gesture->plan.Cancel();gesture.reset();
    if(GetCapture()==s.slider)ReleaseCapture();
    if(restored){Echo(s);if(Own(s))updates::RequestViewport(&s.app);}return restored;
}
void Rebind(Session& s)noexcept{
    if(Selected(s))return;
    Cancel(s);s.draft=false;s.target=nullptr;s.scene=s.app.SceneGeneration();s.slot=s.app.SelectedModelSlot();
    if(!s.app.CameraMode()&&s.slot<kModelSlotCount){s.target=s.app.ModelSlot(s.slot);
        if(s.target&&Boundary([&]()->MmsResult{s.id=SceneObjectIdentity(s.target).id;return MMS_OK;}))s.target=nullptr;}
    if(!Own(s))return;
    Suppress suppress(s);const bool enabled=Selected(s);
    EnableWindow(s.edit,enabled);EnableWindow(s.slider,enabled);if(enabled)Echo(s,true);
}
void Published(Session& s,bool undo,bool redo)noexcept{
    // Property authoring does not register animation, seek or evaluate a pose.
    if(!Own(s))return;
    bake::ScopedUiInputGate gate;
    EnableWindow(GetDlgItem(s.main,panel::kUndoButton),undo);if(!Own(s))return;
    EnableWindow(GetDlgItem(s.main,panel::kRedoButton),redo);if(Own(s))updates::RequestViewport(&s.app);
}
void Refused(MmsResult result)noexcept{runtime_log::Write("PLUGIN_ABI model outline refused: %d",result);MessageBeep(MB_ICONWARNING);}
MmsResult Finish(Session& s)noexcept{
    if(!s.gesture)return MMS_OK;
    if(!Ready(s)){Cancel(s);return MMS_E_STALE;}
    Busy busy(s);auto current=std::move(s.gesture);
    if(BoundNativePoseHistory(&s.app)!=current->history||BoundNativePoseGeneration(&s.app)!=current->binding){current.reset();Echo(s);return MMS_E_STALE;}
    std::uint64_t revision=0;
    if(current->history->Revision(revision)||revision!=current->revision){current.reset();Echo(s);return MMS_E_STALE;}
    if(!current->plan.Changed()){current.reset();Echo(s,true);return MMS_OK;}
    const auto result=current->history->CommitNative(std::move(current->token));
    if(result){current.reset();Echo(s,true);return result;}
    const bool undo=current->history->UndoCount()!=0,redo=current->history->RedoCount()!=0;
    s.app.SceneModified()=1;s.app.state.messageSeen=1;
    current.reset();s.draft=false;Echo(s,true);Published(s,undo,redo);return MMS_OK;
}
MmsResult Preview(Session& s,float value)noexcept{
    if(!Ready(s))return MMS_E_BUSY;
    // The first sample can arrive long after mouse-down. A foreign author
    // must not become the new baseline while the cycle still owns an old
    // precise origin to restore when the pointer returns.
    if(!s.gesture&&s.cycle&&!Same(mdl::Mdl(s.target)->edgeScale,s.origin))return MMS_E_STALE;
    return Boundary([&]()->MmsResult{
        Busy busy(s);
        if(!s.gesture){
            auto* history=BoundNativePoseHistory(&s.app);if(!history)return MMS_E_NOT_READY;
            if(auto r=history->CheckNativeNavigation())return r;if(auto r=history->CheckNativeEdit())return r;
            if(Same(mdl::Mdl(s.target)->edgeScale,value)){s.draft=false;Echo(s,true);return MMS_OK;}
            auto next=std::make_unique<Gesture>();next->history=history;next->binding=BoundNativePoseGeneration(&s.app);
            if(auto r=history->Revision(next->revision))return r;
            if(auto r=next->plan.Begin(s.app,s.slot))return r;if(auto r=next->plan.Set(value))return r;
            std::shared_ptr<HistoryChange> change;if(auto r=next->plan.Take(change))return r;if(!change)return MMS_OK;
            if(auto r=history->PrepareNative("Model outline thickness",change,next->token,SharedHistory::NativeIntent::Adjustment))return r;
            s.gesture=std::move(next);
        }
        auto& g=*s.gesture;
        if(BoundNativePoseHistory(&s.app)!=g.history||BoundNativePoseGeneration(&s.app)!=g.binding){Cancel(s);return MMS_E_STALE;}
        if(auto r=g.plan.Preview(value)){Cancel(s);return r;}
        s.draft=false;Echo(s,true);if(Own(s))updates::RequestViewport(&s.app);return MMS_OK;
    });
}
MmsResult CommitText(Session& s)noexcept{
    if(!s.draft)return MMS_OK;
    if(!Ready(s))return MMS_E_BUSY;
    if(!Same(mdl::Mdl(s.target)->edgeScale,s.baseline))return MMS_E_STALE;
    char text[8]{};const int size=GetWindowTextA(s.edit,text,sizeof text);if(!size)return MMS_E_ARGUMENT;
    const char* first=text;const char* last=text+size;
    while(first!=last&&std::isspace(static_cast<unsigned char>(*first)))++first;
    while(last!=first&&std::isspace(static_cast<unsigned char>(last[-1])))--last;
    if(first!=last&&*first=='+'){++first;if(first!=last&&(*first=='-'||*first=='+'))return MMS_E_ARGUMENT;}
    if(first==last)return MMS_E_ARGUMENT;
    double parsed=0;const auto r=std::from_chars(first,last,parsed,std::chars_format::general);
    if(r.ec!=std::errc{}||r.ptr!=last||!std::isfinite(parsed)||std::fabs(parsed)>(std::numeric_limits<float>::max)())return MMS_E_ARGUMENT;
    const auto preview=Preview(s,float(parsed));if(preview){Cancel(s);Echo(s);return preview;}
    return Finish(s);
}
LRESULT CALLBACK Edit(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    auto current=session;if(!current||current->edit!=window)return DefSubclassProc(window,msg,wp,lp);auto& s=*current;
    if(!s.suppress&&!s.busy){
        if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){Cancel(s);s.draft=false;Echo(s,true);return 0;}
        if(msg==WM_KEYDOWN&&wp==VK_RETURN){
            Rebind(s);const auto result=CommitText(s);if(result)Refused(result);return 0;
        }
    }else if(msg==WM_KEYDOWN&&wp==VK_RETURN)return 0;
    return DefSubclassProc(window,msg,wp,lp);
}
LRESULT CALLBACK Slider(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    auto current=session;if(!current||current->slider!=window)return DefSubclassProc(window,msg,wp,lp);auto& s=*current;
    const auto keyBit=[&]()->unsigned{switch(wp){case VK_LEFT:return 1;case VK_RIGHT:return 2;case VK_UP:return 4;case VK_DOWN:return 8;
        case VK_PRIOR:return 16;case VK_NEXT:return 32;case VK_HOME:return 64;case VK_END:return 128;default:return 0;}};
    const unsigned key=(msg==WM_KEYDOWN||msg==WM_KEYUP)?keyBit():0;
    bool release=msg==WM_MOUSEWHEEL&&s.cycle==0;
    if(!s.suppress&&!s.busy){
        if(msg==WM_LBUTTONDOWN){
            Cancel(s);Rebind(s);if(!Ready(s))return 0;BeginCycle(s,1);s.pointerX=PointerX(lp);s.pointerY=PointerY(lp);
            RECT thumb{};SendMessageW(window,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
            const POINT point{s.pointerX,s.pointerY};s.pointerOrigin=PtInRect(&thumb,point)!=FALSE;
        }
        if(msg==WM_KEYDOWN&&key){Rebind(s);if(!Ready(s))return 0;if(!s.cycle)BeginCycle(s,2);if(s.cycle==2)s.keyMask|=key;}
        // A wheel outside any mouse/key cycle is its own single-message cycle;
        // inside one it stays a sample of that cycle and never splits it.
        if(msg==WM_MOUSEWHEEL&&!s.cycle){Rebind(s);if(Ready(s))BeginCycle(s,3);}
        if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){Cancel(s);Echo(s,true);return 0;}
        if(msg==WM_CANCELMODE||msg==WM_KILLFOCUS||(msg==WM_CAPTURECHANGED&&!s.releasing)){Cancel(s);Echo(s);}
        if(msg==WM_LBUTTONUP&&s.cycle==1)release=true;
        if(msg==WM_KEYUP&&s.cycle==2&&(s.keyMask&key)){s.keyMask&=~key;release=s.keyMask==0;}
    }
    const bool previous=s.releasing;if(release)s.releasing=true;
    const bool pointer=msg==WM_LBUTTONDOWN||msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP;
    PointerSample pointerSample(s,pointer,lp);
    const auto result=DefSubclassProc(window,msg,wp,lp);
    if(release){if(!s.busy){const auto status=Finish(s);if(status)Refused(status);}s.cycle=0;s.keyMask=0;s.releasing=previous;}
    return result;
}
void Retire(Session&)noexcept;
LRESULT CALLBACK Main(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    auto current=session;if(!current||current->app.MainWindow()!=window)return DefSubclassProc(window,msg,wp,lp);auto& s=*current;
    if(s.busy&&(msg==WM_CLOSE||(msg==WM_SYSCOMMAND&&(wp&0xfff0)==SC_CLOSE))){s.closeMain=true;return 0;}
    if(msg==WM_DESTROY||msg==WM_NCDESTROY){Retire(s);return DefSubclassProc(window,msg,wp,lp);}
    if((msg==WM_ACTIVATEAPP&&!wp)||msg==WM_CANCELMODE||msg==WM_CLOSE||(msg==WM_SYSCOMMAND&&(wp&0xfff0)==SC_CLOSE)||
        (msg==WM_COMMAND&&LOWORD(wp)==436&&HIWORD(wp)==CBN_SELCHANGE)){Cancel(s);if(msg==WM_COMMAND)s.draft=false;}
    return DefSubclassProc(window,msg,wp,lp);
}
void Retire(Session& s)noexcept{
    if(s.closing)return;s.closing=true;Cancel(s);
    if(IsWindow(s.edit))RemoveWindowSubclass(s.edit,Edit,kOutlineSubclass);
    if(IsWindow(s.slider))RemoveWindowSubclass(s.slider,Slider,kOutlineSubclass);
    if(IsWindow(s.app.MainWindow()))RemoveWindowSubclass(s.app.MainWindow(),Main,kOutlineSubclass);
    if(session.get()==&s)session.reset();
}
}
bool NativeModelOutlineDialogBlocks(const MMDApp& app)noexcept{
    if(!app.state.edgeThicknessDialog)return false;
    const auto& s=session;return !s||&s->app!=&app||!BoundNativePoseHistory(const_cast<MMDApp*>(&app))||!Own(*s)||s->busy||s->cycle||bool(s->gesture);
}
bool InitializeBoundModelOutline(MMDApp& app,HWND dialog)noexcept{
    if(!BoundNativePoseHistory(&app))return false;
    // This is a lifetime label, not a current-binding permission. Initialization
    // failure or later unbinding must never expose a legacy writable window.
    if(!SetPropW(dialog,kControlled,reinterpret_cast<HANDLE>(INT_PTR(1)))){DestroyWindow(dialog);return true;}
    const auto result=Boundary([&]()->MmsResult{
        if(session)return MMS_E_BUSY;
        auto s=std::make_shared<Session>(app,dialog);
        if(!s->edit||!s->slider||!Own(*s))return MMS_E_ARGUMENT;
        session=s;Busy busy(*s);
        if(!SetWindowSubclass(s->edit,Edit,kOutlineSubclass,0)||!SetWindowSubclass(s->slider,Slider,kOutlineSubclass,0)||
            !SetWindowSubclass(app.MainWindow(),Main,kOutlineSubclass,0)){Retire(*s);return MMS_E_FAILED;}
        Rebind(*s);return MMS_OK;
    });
    if(result){EnableWindow(GetDlgItem(dialog,panel::kEdgeThicknessEdit),FALSE);EnableWindow(GetDlgItem(dialog,panel::kEdgeThicknessSlider),FALSE);Refused(result);}
    return true;
}
bool ControlledModelOutlineDialog(HWND dialog)noexcept{return dialog&&GetPropW(dialog,kControlled)!=nullptr;}
bool RefreshBoundModelOutline(MMDApp& app)noexcept{
    auto s=session;
    if(!s||&s->app!=&app||!Own(*s)){
        const auto dialog=app.state.edgeThicknessDialog;if(!ControlledModelOutlineDialog(dialog))return false;
        if(BoundNativePoseHistory(&app))InitializeBoundModelOutline(app,dialog);
        else{EnableWindow(GetDlgItem(dialog,panel::kEdgeThicknessEdit),FALSE);EnableWindow(GetDlgItem(dialog,panel::kEdgeThicknessSlider),FALSE);}
        return true;
    }
    if(!BoundNativePoseHistory(&app)){Retire(*s);EnableWindow(s->edit,FALSE);EnableWindow(s->slider,FALSE);return true;}
    if(!s->busy){Busy busy(*s);Rebind(*s);Echo(*s);}return true;
}
bool CancelBoundModelOutlineGesture(MMDApp& app)noexcept{
    auto s=session;if(!s||&s->app!=&app)return false;return Cancel(*s);
}
bool HandleBoundModelOutlineDialog(MMDApp& app,HWND dialog,UINT msg,WPARAM wp,LPARAM lp,INT_PTR& out)noexcept{
    auto s=session;if(!s||&s->app!=&app||s->dialog!=dialog){if((ControlledModelOutlineDialog(dialog)||BoundNativePoseHistory(&app))&&msg==WM_HSCROLL){out=1;return true;}return false;}
    out=0;
    if(s->busy&&(msg==WM_CLOSE||(msg==WM_COMMAND&&LOWORD(wp)==IDCANCEL)||(msg==WM_SYSCOMMAND&&(wp&0xfff0)==SC_CLOSE))){s->closeDialog=true;out=1;return true;}
    if(msg==WM_DESTROY||msg==WM_NCDESTROY){Retire(*s);return false;}
    if(msg==WM_CLOSE||(msg==WM_COMMAND&&LOWORD(wp)==IDCANCEL)||(msg==WM_SYSCOMMAND&&(wp&0xfff0)==SC_CLOSE)){
        s->draft=false;Cancel(*s);return false;
    }
    if(msg==WM_CANCELMODE||(msg==WM_ACTIVATE&&!LOWORD(wp))){Cancel(*s);Echo(*s);return false;}
    if(msg==WM_COMMAND&&LOWORD(wp)==panel::kEdgeThicknessEdit&&HIWORD(wp)==EN_CHANGE){
        if(!s->suppress&&!s->busy){Rebind(*s);if(Selected(*s)){if(!s->draft)s->baseline=mdl::Mdl(s->target)->edgeScale;s->draft=true;}}
        return true;
    }
    if(msg!=WM_HSCROLL)return false;
    out=1;if(reinterpret_cast<HWND>(lp)!=s->slider||s->busy||s->suppress)return true;
    // A notification from the old target can arrive while selection changes.
    // Rebind for display, but never reinterpret that old notification as an
    // authored value on the replacement model.
    const bool selected=Selected(*s);
    Rebind(*s);
    if(!selected)return true;
    if(LOWORD(wp)==TB_ENDTRACK){if(s->cycle&&!s->releasing)return true;const auto result=Finish(*s);if(result)Refused(result);return true;}
    const auto tick=int(SendMessageA(s->slider,TBM_GETPOS,0,0));
    const bool atOrigin=s->cycle&&(tick==s->originTick||(s->cycle==1&&s->pointerSample&&s->pointerAtOrigin));
    // A gestureless no-op owns no field write and no reservation. Busy plus an
    // unforced echo snap the native thumb back to the precise origin tick and
    // leave an unsubmitted text draft untouched.
    if(atOrigin&&!s->gesture){Busy busy(*s);Echo(*s);return true;}
    const float value=atOrigin?s->origin:float(double(tick)/100.0);
    const auto result=Preview(*s,value);
    if(result){Cancel(*s);Echo(*s,true);Refused(result);return true;}
    if(!s->cycle){const auto finished=Finish(*s);if(finished)Refused(finished);}return true;
}
}
#endif
