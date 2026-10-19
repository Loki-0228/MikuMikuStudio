#include "native_accessory_hud_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "native_global_commands.hpp"
#include "native_object_commands.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mikumikustudio { void FinishCameraHudInteraction(MMDApp*) noexcept; }
namespace mikumikustudio::plugins {
namespace {
using Values=std::array<float,6>;
Values Read(const mdl::AccessoryRecord& a)noexcept{return {a.position[0],a.position[1],a.position[2],a.rotation[0],a.rotation[1],a.rotation[2]};}
bool Equal(const Values& a,const Values& b)noexcept{return !std::memcmp(a.data(),b.data(),sizeof(a));}
bool Finite(const Values& values)noexcept{for(float value:values)if(!std::isfinite(value))return false;return true;}
unsigned Mask(const Values& a,const Values& b)noexcept{unsigned result=0;for(unsigned i=0;i<a.size();++i)
    if(std::memcmp(&a[i],&b[i],sizeof(float)))result|=1u<<i;return result;}
void Write(mdl::AccessoryRecord& a,const Values& values,unsigned mask=63)noexcept{for(unsigned i=0;i<values.size();++i)if(mask&(1u<<i)){
    if(i<3)a.position[i]=values[i];else a.rotation[i-3]=values[i];}}
bool Ready(MMDApp& app,bool replay=false)noexcept{
    const bool authorized=replay&&NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive()&&!app.FrameStepPlayback()&&!app.state.recordPlaybackStartPending&&
        (!app.SceneMutationInProgress()||authorized)&&(!bake::UiLocked()||authorized)&&!SceneSaveInProgress()&&
        !app.FrameRangeDialog()&&!app.state.frameCopyDialog&&!app.RecordingWindow()&&!app.GravitySettingDialog()&&
        !app.GroundShadowColorDialog()&&!app.AccessoryOrderArray()&&!app.state.accessoryEditDialogOpen&&!app.state.edgeThicknessDialog;
}
struct Change final:HistoryChange,std::enable_shared_from_this<Change>{
    MMDApp& app;const unsigned slot;mdl::AccessoryRecord* const object;const MmsObjectRef identity;
    const std::uint64_t pool,binding;const int frame,parentModel,parentBone;
    unsigned char* parent=nullptr;MmsObjectRef parentIdentity{};mdl::BoneRecord* bones=nullptr;std::uint32_t boneCount=0;
    const Values before;Values after;HWND owner=nullptr;unsigned mask=0;std::uint64_t generation=1;bool preview=true,applied=false;
    explicit Change(MMDApp& value):app(value),slot(value.SelectedAccessorySlot()),object(value.AccessorySlot(slot)),identity(SceneObjectIdentity(object)),
        pool(mdl::KeyPoolIdentity(value.AccessoryKeys(slot))),binding(BoundNativePoseGeneration(&value)),frame(value.CurrentFrame()),
        parentModel(object->parentModel),parentBone(object->parentBone),before(Read(*object)),after(before){
        // Pose records borrow the active objects. Import/removal records own
        // their mesh/key resources and restore presence before older edits.
        // Never dereference a captured pointer until live membership and its
        // stable identity have both been validated.
        if(parentModel==-1)return;
        if(parentModel<0||unsigned(parentModel)>=kModelSlotCount||!value.ModelSlot(parentModel))throw std::invalid_argument("accessory HUD parent model");
        parent=value.ModelSlot(parentModel);parentIdentity=SceneObjectIdentity(parent);const auto& record=*mdl::Mdl(parent);
        bones=record.boneTable;boneCount=record.boneCount;
        if(!bones||parentBone<0||unsigned(parentBone)>=boneCount)throw std::invalid_argument("accessory HUD parent bone");
    }
    bool SameScene()const noexcept{
        if(!pool||app.AccessorySlot(slot)!=object||!SceneObjectPresent(object,identity.id)||mdl::KeyPoolIdentity(app.AccessoryKeys(slot))!=pool)return false;
        if(parent&&(app.ModelSlot(parentModel)!=parent||!SceneObjectPresent(parent,parentIdentity.id)||
            mdl::Mdl(parent)->boneTable!=bones||mdl::Mdl(parent)->boneCount!=boneCount))return false;
        return true;
    }
    bool AttachmentCurrent()const noexcept{return object->parentModel==parentModel&&object->parentBone==parentBone;}
    bool InitialIdentityCurrent()const noexcept{return SceneObjectIdentity(object).generation==identity.generation&&
        (!parent||SceneObjectIdentity(parent).generation==parentIdentity.generation);}
    bool CaptureCurrent()const noexcept{const auto capture=GetCapture();return !capture||(preview&&owner&&capture==owner);}
    bool SourceCurrent()const noexcept{return SameScene()&&AttachmentCurrent()&&InitialIdentityCurrent()&&app.CurrentFrame()==frame&&
        app.CameraMode()!=0&&EffectiveCoordinateState(&app)==2&&app.SelectedAccessorySlot()==slot&&BoundNativePoseGeneration(&app)==binding;}
    bool OwnedPreviewDomain()const noexcept{
        // Selection, coordinate and editor changes stop input, but leave our
        // old object's still-owned fields cancellable. Frame/identity/parent
        // changes may have evaluated a new pose and must not be restored into.
        return SameScene()&&AttachmentCurrent()&&InitialIdentityCurrent()&&app.CurrentFrame()==frame&&BoundNativePoseGeneration(&app)==binding;
    }
    bool InitialCurrent()const noexcept{return generation!=1||(Ready(app)&&CaptureCurrent()&&SourceCurrent()&&Equal(Read(*object),after));}
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<Change> source;const bool forward,initialPreview;const std::uint64_t generation,binding,objectGeneration,parentGeneration;
        const int frame;const Values expected;bool consumed=false;
        Candidate(std::shared_ptr<Change> value,bool direction):source(std::move(value)),forward(direction),initialPreview(source->preview),
            generation(source->generation),binding(BoundNativePoseGeneration(&source->app)),objectGeneration(SceneObjectIdentity(source->object).generation),
            parentGeneration(source->parent?SceneObjectIdentity(source->parent).generation:0),frame(source->app.CurrentFrame()),expected(Read(*source->object)){}
        bool IsCurrent()const noexcept override{return !consumed&&source->generation==generation&&forward!=source->applied&&source->SameScene()&&
            source->AttachmentCurrent()&&source->InitialCurrent()&&Ready(source->app,generation!=1)&&source->CaptureCurrent()&&
            BoundNativePoseGeneration(&source->app)==binding&&source->app.CurrentFrame()==frame&&SceneObjectIdentity(source->object).generation==objectGeneration&&
            (!source->parent||SceneObjectIdentity(source->parent).generation==parentGeneration)&&Equal(Read(*source->object),initialPreview?source->after:expected);}
        void Publish()noexcept override{if(!IsCurrent())std::terminate();Write(*source->object,forward?source->after:source->before,source->mask);
            source->preview=false;source->applied=forward;++source->generation;consumed=true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();return Boundary([&]()->MmsResult{
            if(!SameScene()||!AttachmentCurrent()||!InitialCurrent()||forward==applied)return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            if(!Ready(app,generation!=1)||!CaptureCurrent())return MMS_E_BUSY;
            if(!Finite(Read(*object)))return MMS_E_ARGUMENT;
            out=std::make_unique<Candidate>(shared_from_this(),forward);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{return sizeof(*this)+4*sizeof(void*);}
    bool RequiresPhysicsReset()const noexcept override{return false;}
};
struct Gesture{
    MMDApp* app=nullptr;SharedHistory* history=nullptr;HWND owner=nullptr;int operation=0;std::uint64_t revision=0;
    std::shared_ptr<Change> change;std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    bool Current()const noexcept{std::uint64_t current=0;return !history->Revision(current)&&current==revision&&change->SourceCurrent()&&Equal(Read(*change->object),change->after);}
    bool Rollback()noexcept{
        if(!change->OwnedPreviewDomain())return false;
        const auto current=Read(*change->object);const auto changed=Mask(change->before,change->after);unsigned restore=0;
        for(unsigned i=0;i<current.size();++i)if((changed&(1u<<i))&&!std::memcmp(&current[i],&change->after[i],sizeof(float)))restore|=1u<<i;
        Write(*change->object,change->before,restore);return restore!=0||changed==0;
    }
};
thread_local std::unique_ptr<Gesture> gesture;
void Refresh(MMDApp& app)noexcept{RefreshBoundPoseReadout(app);RefreshBoundGlobalReadout(app);}
}
bool AccessoryHudOperation(MMDApp& app,int operation)noexcept{return operation>=15&&operation<=20&&app.CameraMode()!=0&&
    EffectiveCoordinateState(&app)==2&&app.SelectedAccessorySlot()<kModelSlotCount&&app.AccessorySlot(app.SelectedAccessorySlot());}
bool AccessoryHudGestureActive(const MMDApp& app)noexcept{return gesture&&gesture->app==&app;}
std::shared_ptr<const HistoryChange> AccessoryHudPreviewSource(MMDApp& app)noexcept{
    return AccessoryHudGestureActive(app)?gesture->change:nullptr;
}
MmsResult BeginAccessoryHudGesture(MMDApp& app,SharedHistory& history,HWND owner,int operation)noexcept{
    if(!AccessoryHudOperation(app,operation))return MMS_E_NOT_READY;
    if(auto result=history.CheckNativeNavigation())return result;if(auto result=history.CheckNativeEdit())return result;
    if(gesture||!Ready(app)||(GetCapture()&&GetCapture()!=owner))return MMS_E_BUSY;
    if(!Finite(Read(*app.AccessorySlot(app.SelectedAccessorySlot()))))return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        auto next=std::make_unique<Gesture>();next->app=&app;next->history=&history;next->owner=owner;next->operation=operation;
        next->change=std::make_shared<Change>(app);next->change->owner=owner;
        if(auto result=history.Revision(next->revision))return result;
        if(auto result=history.PrepareNative("Drag accessory HUD axis",next->change,next->transaction,SharedHistory::NativeIntent::Adjustment))return result;
        gesture=std::move(next);return MMS_OK;
    });
}
bool CancelAccessoryHudGesture(MMDApp& app)noexcept{
    if(!AccessoryHudGestureActive(app))return false;
    auto current=std::move(gesture);const bool restored=current->Rollback();current->transaction.reset();FinishCameraHudInteraction(&app);
    if(current->owner&&GetCapture()==current->owner)ReleaseCapture();if(restored)updates::RequestViewport(&app);return restored;
}
bool AllowAccessoryHudPreview(MMDApp& app,int operation)noexcept{
    if(!AccessoryHudGestureActive(app)||gesture->operation!=operation)return false;
    if(!Ready(app)||!AccessoryHudOperation(app,operation)||(gesture->owner&&GetCapture()!=gesture->owner)||!gesture->Current()){
        CancelAccessoryHudGesture(app);return false;
    }
    return true;
}
bool AcceptAccessoryHudPreview(MMDApp& app)noexcept{
    if(!AccessoryHudGestureActive(app))return false;
    if(!gesture->change->SourceCurrent()){CancelAccessoryHudGesture(app);return false;}
    const auto values=Read(*gesture->change->object);
    const unsigned field=unsigned(gesture->operation-15);bool foreign=false;
    for(unsigned i=0;i<values.size();++i)if(i!=field&&std::memcmp(&values[i],&gesture->change->after[i],sizeof(float)))foreign=true;
    if(!std::isfinite(values[field])){
        // The native operation writes one axis only. Recover that invalid
        // sample without replacing a concurrent author's other pose fields.
        Write(*gesture->change->object,gesture->change->after,1u<<field);CancelAccessoryHudGesture(app);return false;
    }
    if(foreign){
        // A reentrant readout notification may have changed an independent
        // field after Allow. Retain just our new axis sample for field-wise
        // rollback, leaving every foreign finite or nonfinite value alone.
        gesture->change->after[field]=values[field];gesture->change->mask=Mask(gesture->change->before,gesture->change->after);
        CancelAccessoryHudGesture(app);return false;
    }
    gesture->change->after=values;gesture->change->mask=Mask(gesture->change->before,values);return true;
}
MmsResult EndAccessoryHudGesture(MMDApp& app,bool& changed)noexcept{
    changed=false;if(!AccessoryHudGestureActive(app))return MMS_E_NOT_READY;auto current=std::move(gesture);
    if(!Ready(app)||(current->owner&&GetCapture()!=current->owner)||!current->Current()){current->Rollback();return MMS_E_STALE;}
    if(!current->change->mask)return MMS_OK;
    const auto result=current->history->CommitNative(std::move(current->transaction));if(result){current->Rollback();return result;}
    changed=true;return MMS_OK;
}
bool HandleAccessoryHudMessage(MMDApp& app,HWND owner,UINT message,WPARAM wp,LPARAM lp)noexcept{
    if(!AccessoryHudGestureActive(app))return false;
    if(message==WM_CANCELMODE||message==WM_KILLFOCUS||message==WM_CLOSE||message==WM_DESTROY||message==WM_NCDESTROY||
        (message==WM_ACTIVATEAPP&&!wp)||(message==WM_KEYDOWN&&wp==VK_ESCAPE)||
        (message==WM_CAPTURECHANGED&&reinterpret_cast<HWND>(lp)!=gesture->owner)){
        CancelAccessoryHudGesture(app);Refresh(app);return message==WM_KEYDOWN;
    }
    if(message==WM_LBUTTONUP&&gesture->owner==owner){
        const auto source=gesture->change;
        const int operation=gesture->operation;
        const bool allowed=AllowAccessoryHudPreview(app,operation);
        if(!AccessoryHudGestureActive(app)||gesture->change!=source)return true;
        if(allowed){
            POINT point{};
            if(owner&&GetCursorPos(&point)&&ScreenToClient(owner,&point)){
                app.MouseX()=point.x;app.MouseY()=point.y;app.ShiftModifierState()=(wp&MK_SHIFT)?3:0;app.CtrlModifierState()=(wp&MK_CONTROL)?3:0;
                if(operation<=17)ModeCameraAdjust(&app,operation-15);else ModeAngleAdjust(&app,operation-18);
                // EchoEdit may have synchronously replaced this gesture. The
                // old release must not update or end the new one, including
                // the native mouse-up cleanup in the outer window procedure.
                if(!AccessoryHudGestureActive(app)||gesture->change!=source)return true;
                app.PreviousMouseX()=app.MouseX();app.PreviousMouseY()=app.MouseY();
            }
        }
        if(AccessoryHudGestureActive(app)){auto* history=gesture->history;bool changed=false;const auto result=EndAccessoryHudGesture(app,changed);
            if(!result&&changed)PublishBoundPoseEdit(app,*history,false);else RefreshBoundPoseReadout(app);}
        // Native mouse-up housekeeping still owns cursor and capture cleanup.
        RefreshBoundGlobalReadout(app);
    }
    return false;
}
}
#endif
