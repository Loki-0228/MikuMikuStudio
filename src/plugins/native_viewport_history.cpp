#include "native_viewport_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/autosave.hpp"
#include <array>
#include <cmath>
#include <cstring>
namespace mikudancestudio {
void ConsumeRightButtonDrag(MMDApp*);
void ConsumeMiddleButtonPan(MMDApp*);
}
namespace mikudancestudio::plugins {
namespace {
using Values=std::array<float,7>;
Values Read(MMDApp& a) noexcept {return {a.CameraPositionX(),a.CameraPositionY(),a.CameraPositionZ(),a.CameraPitch(),a.CameraYaw(),a.CameraRoll(),a.CameraDistance()};}
bool Equal(const Values& a,const Values& b) noexcept {return !std::memcmp(a.data(),b.data(),sizeof(a));}
bool Finite(const Values& a) noexcept {for(float v:a)if(!std::isfinite(v))return false;return true;}
void Write(MMDApp& a,const Values& v,unsigned mask=127) noexcept {
    for(unsigned i=0;i<7;++i)if(mask&(1u<<i)){
        if(i<3)a.CameraPositionAxis(i)=v[i];else if(i<6)a.CameraRotation()[i-3]=v[i];else a.CameraDistance()=v[i];
    }
}
bool Ready(MMDApp& a) noexcept {
    return a.CameraMode()!=0 && !a.PlaybackActive() && !a.FrameStepPlayback() &&
        !a.state.recordPlaybackStartPending && !a.SceneMutationInProgress() && !bake::UiLocked() &&
        !SceneSaveInProgress() && !a.FrameRangeDialog() && !a.state.frameCopyDialog &&
        !a.RecordingWindow() && !a.GravitySettingDialog() && !a.GroundShadowColorDialog() &&
        !a.AccessoryOrderArray() && !a.state.accessoryEditDialogOpen && !a.state.edgeThicknessDialog;
}
struct CameraGestureChange final:HistoryChange,std::enable_shared_from_this<CameraGestureChange>{
    MMDApp& app; const std::uint64_t pool; const Values before; Values after;
    const int initialFrame,initialParentModel,initialParentBone;
    const std::uint64_t initialBinding;
    std::uint64_t generation=1; unsigned mask=127; bool preview,applied=false;
    explicit CameraGestureChange(MMDApp& a,bool livePreview=true):app(a),pool(mdl::KeyPoolIdentity(a.CameraKeys())),before(Read(a)),after(before),
        initialFrame(a.CurrentFrame()),initialParentModel(a.CameraParentModel()),initialParentBone(a.CameraParentBone()),
        initialBinding(BoundNativePoseGeneration(&a)),preview(livePreview){}
    bool SameScene() const noexcept {return pool && mdl::KeyPoolIdentity(app.CameraKeys())==pool;}
    bool InitialCurrent() const noexcept {
        // Wheel values are prepared privately. A gate may invalidate the
        // source before reservation or publication; never rebase that edit.
        return preview || generation!=1 || (Ready(app) && !GetCapture() &&
            app.CurrentFrame()==initialFrame && app.CameraParentModel()==initialParentModel &&
            app.CameraParentBone()==initialParentBone && BoundNativePoseGeneration(&app)==initialBinding && Equal(Read(app),before));
    }
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<CameraGestureChange> source;const bool forward,initial;
        const std::uint64_t generation;const int frame,parentModel,parentBone;const Values expected;bool consumed=false;
        Candidate(std::shared_ptr<CameraGestureChange> s,bool f):source(std::move(s)),forward(f),initial(source->preview),generation(source->generation),frame(source->app.CurrentFrame()),parentModel(source->app.CameraParentModel()),parentBone(source->app.CameraParentBone()),expected(Read(source->app)){}
        bool IsCurrent() const noexcept override {
            return !consumed && generation==source->generation && forward!=source->applied && source->SameScene() && source->InitialCurrent() &&
                source->app.CurrentFrame()==frame && source->app.CameraParentModel()==parentModel && source->app.CameraParentBone()==parentBone && Equal(Read(source->app),initial?source->after:expected);
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();
            Write(source->app,forward?source->after:source->before,source->mask);
            source->preview=false;source->applied=forward;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();return Boundary([&]()->MmsResult{
            if(!SameScene() || !InitialCurrent() || forward==applied)return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            if(!Finite(Read(app)))return MMS_E_ARGUMENT;
            out=std::make_unique<Candidate>(shared_from_this(),forward);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{return sizeof(*this)+4*sizeof(void*);}
};
struct Gesture {
    MMDApp* app; SharedHistory* history; HWND owner;UINT button;int frame,parentModel,parentBone;
    std::uint64_t revision=0;
    std::shared_ptr<CameraGestureChange> change;
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    bool Current()const noexcept {
        std::uint64_t live=0;
        return !history->Revision(live) && live==revision && change->SameScene() &&
            app->CurrentFrame()==frame && app->CameraParentModel()==parentModel && app->CameraParentBone()==parentBone &&
            Equal(Read(*app),change->after);
    }
    bool Rollback() noexcept {
        // A seek or another author may have replaced the pose. Never overwrite it.
        if(!change->SameScene() || app->CurrentFrame()!=frame || !Equal(Read(*app),change->after))return false;
        Write(*app,change->before);return true;
    }
};
thread_local std::unique_ptr<Gesture> gesture;
void SampleMessage(MMDApp& a,WPARAM wp,LPARAM lp) noexcept {
    a.PreviousMouseX()=a.MouseX();a.PreviousMouseY()=a.MouseY();
    a.MouseX()=static_cast<short>(LOWORD(lp));a.MouseY()=static_cast<short>(HIWORD(lp));
    a.ShiftModifierState()=(wp&MK_SHIFT)?3:0;a.CtrlModifierState()=(wp&MK_CONTROL)?3:0;
    const auto active=a.ViewportInputActive();a.ViewportInputActive()=1;
    if(gesture->button==WM_RBUTTONDOWN){a.state.rightMouseButtonState=3;ConsumeRightButtonDrag(&a);}
    else {a.state.middleMouseButtonState=3;ConsumeMiddleButtonPan(&a);}
    a.ViewportInputActive()=active;
    a.PreviousMouseX()=a.MouseX();a.PreviousMouseY()=a.MouseY();
}
}
MmsResult BeginViewportCameraGesture(MMDApp& a,SharedHistory& h,HWND owner,UINT button) noexcept {
    if(button!=WM_RBUTTONDOWN && button!=WM_MBUTTONDOWN)return MMS_E_ARGUMENT;
    if(gesture || !Ready(a) || GetCapture())return MMS_E_BUSY;
    if(!Finite(Read(a)))return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        auto next=std::make_unique<Gesture>();next->app=&a;next->history=&h;next->owner=owner;next->button=button;
        next->frame=a.CurrentFrame();next->parentModel=a.CameraParentModel();next->parentBone=a.CameraParentBone();
        next->change=std::make_shared<CameraGestureChange>(a);
        if(auto r=h.Revision(next->revision))return r;
        if(auto r=h.PrepareNative("Drag camera",next->change,next->transaction,SharedHistory::NativeIntent::Adjustment))return r;
        gesture=std::move(next);return MMS_OK;
    });
}
bool CancelViewportCameraGesture(MMDApp& a) noexcept {
    if(!gesture || gesture->app!=&a)return false;
    auto current=std::move(gesture);const bool restored=current->Rollback();current->transaction.reset();
    a.state.rightMouseButtonState=0;a.state.middleMouseButtonState=0;
    if(current->owner && GetCapture()==current->owner)ReleaseCapture();
    return restored;
}
bool AllowViewportCameraPreview(MMDApp& a,UINT button) noexcept {
    if(!gesture || gesture->app!=&a || gesture->button!=button)return false;
    if(!Ready(a) || (gesture->owner && GetCapture()!=gesture->owner) || !gesture->Current()){CancelViewportCameraGesture(a);return false;}
    return true;
}
bool AcceptViewportCameraPreview(MMDApp& a) noexcept {
    if(!gesture || gesture->app!=&a)return false;
    const auto values=Read(a);
    if(!Finite(values)){Write(a,gesture->change->after);CancelViewportCameraGesture(a);return false;}
    gesture->change->after=values;return true;
}
MmsResult EndViewportCameraGesture(MMDApp& a,bool& changed) noexcept {
    changed=false;if(!gesture || gesture->app!=&a)return MMS_E_NOT_READY;
    auto current=std::move(gesture);
    if(GetCapture()==current->owner)ReleaseCapture();
    a.state.rightMouseButtonState=0;a.state.middleMouseButtonState=0;
    if(!Ready(a) || !current->Current()){current->Rollback();return MMS_E_STALE;}
    auto& c=*current->change;c.mask=0;
    for(unsigned i=0;i<7;++i)if(std::memcmp(&c.before[i],&c.after[i],sizeof(float)))c.mask|=1u<<i;
    if(!c.mask)return MMS_OK;
    const auto result=current->history->CommitNative(std::move(current->transaction));
    if(result){current->Rollback();return result;}
    changed=true;return MMS_OK;
}
MmsResult AdjustViewportCameraWheel(MMDApp& a,SharedHistory& h,std::int16_t delta,bool floating,bool& changed) noexcept {
    changed=false;
    // Validate thread/plugin admission before reading the native scene.
    if(auto r=h.CheckNativeEdit())return r;
    if(gesture || GetCapture() || !Ready(a))return MMS_E_BUSY;
    const auto before=Read(a);if(!Finite(before))return MMS_E_ARGUMENT;
    if(!delta)return MMS_OK;
    auto after=before;const unsigned axis=!floating && a.CameraParentModel()>=0?2:6;
    if(floating)after[axis]=static_cast<float>(static_cast<double>(delta)*0.05000000074505806+static_cast<double>(before[axis]));
    else after[axis]=static_cast<float>(delta)*0.05f+before[axis];
    if(!Finite(after))return MMS_E_ARGUMENT;
    if(Equal(before,after))return MMS_OK;
    return Boundary([&]()->MmsResult {
        auto change=std::make_shared<CameraGestureChange>(a,false);change->after=after;change->mask=1u<<axis;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(auto r=h.PrepareNative("Wheel camera",change,transaction,SharedHistory::NativeIntent::Adjustment))return r;
        if(auto r=h.CommitNative(std::move(transaction)))return r;
        changed=true;return MMS_OK;
    });
}
bool HandleBoundViewportMessage(MMDApp& a,HWND owner,UINT msg,WPARAM wp,LPARAM lp) noexcept {
    if(msg==WM_CANCELMODE || msg==WM_KILLFOCUS || msg==WM_NCDESTROY ||
        (msg==WM_ACTIVATEAPP && !wp) || (msg==WM_KEYDOWN && wp==VK_ESCAPE) ||
        (msg==WM_CAPTURECHANGED && gesture && reinterpret_cast<HWND>(lp)!=gesture->owner)){
        const bool active=gesture && gesture->app==&a;
        if(active){CancelViewportCameraGesture(a);if(GetCapture()==owner)ReleaseCapture();RefreshBoundPoseReadout(a);}
        return active && msg==WM_KEYDOWN;
    }
    auto* history=BoundNativePoseHistory(&a);if(!history)return false;
    if(msg==WM_MOUSEWHEEL){
        const bool floating=owner && owner==a.FloatingWindow();
        if(!floating && a.MouseX()<=a.SidebarWidth())return false;
        bool changed=false;
        if(!AdjustViewportCameraWheel(a,*history,static_cast<std::int16_t>(HIWORD(wp)),floating,changed) && changed)
            PublishBoundPoseEdit(a,*history,false);
        return true;
    }
    if(msg==WM_RBUTTONDOWN || msg==WM_MBUTTONDOWN){
        POINT p{static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp))};
        if(!PtInRect(&a.ViewportRect(),p))return false;
        SetFocus(owner);
        if(BeginViewportCameraGesture(a,*history,owner,msg))return true;
        a.MouseX()=a.PreviousMouseX()=p.x;a.MouseY()=a.PreviousMouseY()=p.y;
        SetCapture(owner);if(GetCapture()!=owner)CancelViewportCameraGesture(a);
        return true;
    }
    if(!gesture || gesture->app!=&a || gesture->owner!=owner)return false;
    if(msg==WM_MOUSEMOVE){SampleMessage(a,wp,lp);return true;}
    if((msg==WM_RBUTTONUP && gesture->button==WM_RBUTTONDOWN) || (msg==WM_MBUTTONUP && gesture->button==WM_MBUTTONDOWN)){
        SampleMessage(a,wp,lp);bool changed=false;
        if(gesture){const auto r=EndViewportCameraGesture(a,changed);if(!r && changed)PublishBoundPoseEdit(a,*history,false);else RefreshBoundPoseReadout(a);}
        return true;
    }
    return false;
}
}
#endif
