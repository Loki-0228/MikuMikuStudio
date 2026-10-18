#include "native_viewport_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_camera_hud_history.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_viewport_reset_camera.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/autosave.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <stdexcept>
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
// Real common undo/redo holds the object publication's scene and UI guards.
// Only an already-published numpad record opts into that authorized scope;
// initial edits, drags and wheel input retain the ordinary mutation gate.
bool ViewportReady(MMDApp& a,bool allowAuthorizedHistory=false) noexcept {
    const bool authorizedHistory=allowAuthorizedHistory&&NativeObjectPublicationAuthorized(&a);
    return !a.PlaybackActive() && !a.FrameStepPlayback() &&
        !a.state.recordPlaybackStartPending && (!a.SceneMutationInProgress()||authorizedHistory) &&
        (!bake::UiLocked()||authorizedHistory) &&
        !SceneSaveInProgress() && !a.FrameRangeDialog() && !a.state.frameCopyDialog &&
        !a.RecordingWindow() && !a.GravitySettingDialog() && !a.GroundShadowColorDialog() &&
        !a.AccessoryOrderArray() && !a.state.accessoryEditDialogOpen && !a.state.edgeThicknessDialog;
}
bool Ready(MMDApp& a) noexcept {return a.CameraMode()!=0&&ViewportReady(a);}
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
ViewportPresetState ReadPreset(MMDApp& app) noexcept {
    ViewportPresetState value;
    for(unsigned i=0;i<3;++i){value.cameraPosition[i]=app.CameraPositionAxis(i);value.rotation[i]=app.CameraRotation()[i];}
    value.offsetX=app.ViewOffsetX();value.offsetY=app.ViewOffsetY();value.distance=app.CameraDistance();value.fov=app.CameraFov();
    std::memcpy(value.basis.data(),&app.CameraAttachmentBasis(),sizeof(float)*16);
    value.suppressed=app.CameraAttachmentTransformSuppressed();value.perspective=app.CameraPerspective();
    value.parentModel=app.CameraParentModel();value.parentBone=app.CameraParentBone();return value;
}
std::array<float,26> PresetFloats(const ViewportPresetState& value) noexcept {
    std::array<float,26> result{};
    for(unsigned i=0;i<3;++i){result[i]=value.cameraPosition[i];result[i+3]=value.rotation[i];}
    result[6]=value.offsetX;result[7]=value.offsetY;result[8]=value.distance;result[9]=value.fov;
    for(unsigned i=0;i<16;++i)result[i+10]=value.basis[i];return result;
}
bool EqualPreset(const ViewportPresetState& a,const ViewportPresetState& b) noexcept {
    const auto x=PresetFloats(a),y=PresetFloats(b);
    return !std::memcmp(x.data(),y.data(),sizeof(x))&&a.suppressed==b.suppressed&&a.perspective==b.perspective&&
        a.parentModel==b.parentModel&&a.parentBone==b.parentBone;
}
bool FinitePreset(const ViewportPresetState& value) noexcept {
    for(const auto field:PresetFloats(value))if(!std::isfinite(field))return false;return true;
}
unsigned PresetMask(const ViewportPresetState& before,const ViewportPresetState& after) noexcept {
    const auto a=PresetFloats(before),b=PresetFloats(after);unsigned mask=0;
    for(unsigned i=0;i<a.size();++i)if(std::memcmp(&a[i],&b[i],sizeof(float)))mask|=1u<<i;
    if(before.suppressed!=after.suppressed)mask|=1u<<26;
    if(before.perspective!=after.perspective)mask|=1u<<27;
    if(before.parentModel!=after.parentModel)mask|=1u<<28;
    if(before.parentBone!=after.parentBone)mask|=1u<<29;return mask;
}
void WritePreset(MMDApp& app,const ViewportPresetState& value,unsigned mask) noexcept {
    for(unsigned i=0;i<3;++i){if(mask&(1u<<i))app.CameraPositionAxis(i)=value.cameraPosition[i];
        if(mask&(1u<<(i+3)))app.CameraRotation()[i]=value.rotation[i];}
    if(mask&(1u<<6))app.ViewOffsetX()=value.offsetX;
    if(mask&(1u<<7))app.ViewOffsetY()=value.offsetY;
    if(mask&(1u<<8))app.CameraDistance()=value.distance;
    if(mask&(1u<<9))app.CameraFov()=value.fov;
    auto* basis=&app.CameraAttachmentBasis().m[0][0];
    for(unsigned i=0;i<16;++i)if(mask&(1u<<(i+10)))basis[i]=value.basis[i];
    if(mask&(1u<<26))app.CameraAttachmentTransformSuppressed()=value.suppressed;
    if(mask&(1u<<27))app.CameraPerspective()=value.perspective;
    if(mask&(1u<<28))app.CameraParentModel()=value.parentModel;
    if(mask&(1u<<29))app.CameraParentBone()=value.parentBone;
    if(mask&((1u<<8)|(1u<<9)|(1u<<27)))app.cameraProjectionRefreshPending=true;
}
struct PresetContext {
    std::uint8_t cameraMode,follow,reference;unsigned slot;unsigned char* selected;int selectedBone;
    explicit PresetContext(MMDApp& app):cameraMode(app.CameraMode()),follow(app.state.followCameraEnabled),
        reference(static_cast<std::uint8_t>(app.CameraReferenceMode())),slot(app.SelectedModelSlot()),
        selected(slot<kModelSlotCount?app.ModelSlot(slot):nullptr),selectedBone(selected?mdl::Mdl(selected)->selectedBone:-1){}
    bool Current(MMDApp& app)const noexcept {
        return app.CameraMode()==cameraMode&&app.state.followCameraEnabled==follow&&
            static_cast<std::uint8_t>(app.CameraReferenceMode())==reference&&app.SelectedModelSlot()==slot&&
            (slot>=kModelSlotCount||app.ModelSlot(slot)==selected)&&
            (reference!=static_cast<std::uint8_t>(CameraAttachmentReference::SelectedBone)||!selected||mdl::Mdl(selected)->selectedBone==selectedBone);
    }
};
struct PresetModel {
    unsigned slot;unsigned char* model;mdl::BoneRecord* bones;std::uint32_t count;
    std::shared_ptr<RetainedSceneObject> owner;
    PresetModel(MMDApp& app,unsigned index):slot(index),model(app.ModelSlot(index)),
        bones(mdl::Mdl(model)->boneTable),count(mdl::Mdl(model)->boneCount),owner(RetainSceneObject(app,MMS_OBJECT_MODEL,index)){}
    bool Current(MMDApp& app)const noexcept {
        return owner->Present()&&app.ModelSlot(slot)==model&&SceneObjectPresent(model,owner->Id())&&
            mdl::Mdl(model)->boneTable==bones&&mdl::Mdl(model)->boneCount==count;
    }
};
bool ModelReadoutReady(MMDApp& app)noexcept {
    if(app.SelectedModelSlot()>=kModelSlotCount||!app.SelectedModel())return false;
    const auto& model=*mdl::Mdl(app.SelectedModel());
    // The original drag pump calls PostViewRefresh directly after a sample.
    // A negative selected bone uses its empty readout; a real bone needs a
    // valid table and an in-range index before any view component is changed.
    if((model.boneCount&&!model.boneTable)||
        (model.selectedBone>=0&&(!model.boneTable||std::uint32_t(model.selectedBone)>=model.boneCount)))return false;
    // A nonidentity scale readout calls ApplyTransforms. Permit it only when
    // those transforms are already installed: navigation must never allocate
    // a render cache or change any bone matrix as an implicit UI refresh.
    return model.selectedBone<0||bone_scale::Identity(bone_scale::Pose(app.SelectedModel(),model.selectedBone))||
        bone_scale::TransformsApplied(app.SelectedModel());
}
// Presets and model-view input share the same masked, context-checked state
// publication. A live preview retains its prepared node until release.
struct NumpadChange final:HistoryChange,std::enable_shared_from_this<NumpadChange> {
    MMDApp& app;const std::uint64_t pool,binding;const int frame;const PresetContext context;
    const ViewportPresetState before;ViewportPresetState after;unsigned mask=0;std::uint64_t generation=1;bool applied=false,preview=false,modelReadout=false;
    HWND previewOwner=nullptr;
    std::vector<PresetModel> models;std::shared_ptr<ViewportResetCameraSource> resetCamera;
    explicit NumpadChange(MMDApp& a):app(a),pool(mdl::KeyPoolIdentity(a.CameraKeys())),binding(BoundNativePoseGeneration(&a)),
        frame(a.CurrentFrame()),context(a),before(ReadPreset(a)),after(before){}
    void AddModel(unsigned slot){
        for(const auto& model:models)if(model.slot==slot)return;
        if(slot>=kModelSlotCount||!app.ModelSlot(slot))throw std::invalid_argument("preset model identity");
        models.emplace_back(app,slot);
    }
    void AddParent(int model,int bone){
        if(model== -1)return;
        if(model<0||unsigned(model)>=kModelSlotCount||!app.ModelSlot(unsigned(model)))throw std::invalid_argument("preset parent model");
        const auto& parent=*mdl::Mdl(app.ModelSlot(unsigned(model)));
        if(!parent.boneTable||bone<0||unsigned(bone)>=parent.boneCount)throw std::invalid_argument("preset parent bone");
        AddModel(unsigned(model));
    }
    bool SameScene()const noexcept {
        if(!pool||mdl::KeyPoolIdentity(app.CameraKeys())!=pool)return false;
        for(const auto& model:models)if(!model.Current(app))return false;return true;
    }
    bool CaptureAllowed()const noexcept {
        const auto capture=GetCapture();return !capture||(preview&&previewOwner&&capture==previewOwner);
    }
    bool ModelReadoutCurrent()const noexcept {
        return !modelReadout||(ModelReadoutReady(app)&&mdl::Mdl(app.SelectedModel())->selectedBone==context.selectedBone);
    }
    bool InitialCurrent()const noexcept {
        return generation!=1||(ViewportReady(app)&&CaptureAllowed()&&ModelReadoutCurrent()&&frame==app.CurrentFrame()&&context.Current(app)&&
            binding==BoundNativePoseGeneration(&app)&&EqualPreset(ReadPreset(app),preview?after:before)&&(!resetCamera||resetCamera->IsCurrent()));
    }
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<NumpadChange> source;const bool forward,initialPreview;const std::uint64_t generation,binding;
        const int frame;const PresetContext context;const ViewportPresetState expected;bool consumed=false;
        Candidate(std::shared_ptr<NumpadChange> s,bool f):source(std::move(s)),forward(f),initialPreview(source->preview),generation(source->generation),
            binding(BoundNativePoseGeneration(&source->app)),frame(source->app.CurrentFrame()),context(source->app),expected(ReadPreset(source->app)){}
        bool IsCurrent()const noexcept override {
            return !consumed&&generation==source->generation&&forward!=source->applied&&source->SameScene()&&source->InitialCurrent()&&
                ViewportReady(source->app,generation!=1)&&source->CaptureAllowed()&&binding==BoundNativePoseGeneration(&source->app)&&
                frame==source->app.CurrentFrame()&&context.Current(source->app)&&EqualPreset(ReadPreset(source->app),initialPreview?source->after:expected);
        }
        void Publish()noexcept override {
            if(!IsCurrent())std::terminate();WritePreset(source->app,forward?source->after:source->before,source->mask);
            source->preview=false;source->applied=forward;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override {
        out.reset();return Boundary([&]()->MmsResult{
            if(!SameScene()||!InitialCurrent()||forward==applied)return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            if(!ViewportReady(app,generation!=1)||!CaptureAllowed())return MMS_E_BUSY;
            if(!FinitePreset(ReadPreset(app)))return MMS_E_ARGUMENT;
            out=std::make_unique<Candidate>(shared_from_this(),forward);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);
        const auto add=[&](std::uint64_t value){bytes=value>UINT64_MAX-bytes?UINT64_MAX:bytes+value;};
        add(models.capacity()*sizeof(PresetModel));if(resetCamera)add(resetCamera->RetainedBytes());return bytes;
    }
};
bool MatchedModelFollow(MMDApp& app)noexcept {
    return !app.CameraMode()&&app.state.followCameraEnabled&&app.CameraParentModel()>=0&&
        unsigned(app.CameraParentModel())==app.SelectedModelSlot();
}
struct ModelGesture {
    MMDApp* app;SharedHistory* history;HWND owner;UINT button;std::uint64_t revision=0;
    std::shared_ptr<NumpadChange> change;std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    bool SourceCurrent()const noexcept {
        return change->SameScene()&&change->ModelReadoutCurrent()&&change->frame==app->CurrentFrame()&&change->context.Current(*app)&&
            change->binding==BoundNativePoseGeneration(app)&&!MatchedModelFollow(*app);
    }
    bool Current()const noexcept {
        std::uint64_t live=0;
        return !history->Revision(live)&&live==revision&&SourceCurrent()&&EqualPreset(ReadPreset(*app),change->after);
    }
    bool Rollback()noexcept {
        if(!SourceCurrent()||!EqualPreset(ReadPreset(*app),change->after))return false;
        WritePreset(*app,change->before,PresetMask(change->before,change->after));return true;
    }
};
thread_local std::unique_ptr<ModelGesture> modelGesture;
void SampleModelMessage(MMDApp& app,WPARAM wp,LPARAM lp)noexcept {
    const auto button=modelGesture->button;
    app.PreviousMouseX()=app.MouseX();app.PreviousMouseY()=app.MouseY();
    app.MouseX()=static_cast<short>(LOWORD(lp));app.MouseY()=static_cast<short>(HIWORD(lp));
    app.ShiftModifierState()=(wp&MK_SHIFT)?3:0;app.CtrlModifierState()=(wp&MK_CONTROL)?3:0;
    const auto active=app.ViewportInputActive();app.ViewportInputActive()=1;
    if(button==WM_RBUTTONDOWN){app.state.rightMouseButtonState=3;ConsumeRightButtonDrag(&app);}
    else{app.state.middleMouseButtonState=3;ConsumeMiddleButtonPan(&app);}
    app.ViewportInputActive()=active;app.PreviousMouseX()=app.MouseX();app.PreviousMouseY()=app.MouseY();
}
}
MmsResult BeginViewportCameraGesture(MMDApp& a,SharedHistory& h,HWND owner,UINT button) noexcept {
    if(button!=WM_RBUTTONDOWN && button!=WM_MBUTTONDOWN)return MMS_E_ARGUMENT;
    if(gesture || modelGesture || !Ready(a) || GetCapture())return MMS_E_BUSY;
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
    const bool hudRestored=CancelCameraHudGesture(a);
    const bool modelRestored=CancelViewportModelGesture(a);
    if(!gesture || gesture->app!=&a)return modelRestored||hudRestored;
    auto current=std::move(gesture);const bool restored=current->Rollback();current->transaction.reset();
    a.state.rightMouseButtonState=0;a.state.middleMouseButtonState=0;
    if(current->owner && GetCapture()==current->owner)ReleaseCapture();
    return restored||modelRestored||hudRestored;
}
bool AllowViewportCameraPreview(MMDApp& a,UINT button) noexcept {
    if(modelGesture&&modelGesture->app==&a)return AllowViewportModelPreview(a,button);
    if(!gesture || gesture->app!=&a || gesture->button!=button)return false;
    if(!Ready(a) || (gesture->owner && GetCapture()!=gesture->owner) || !gesture->Current()){CancelViewportCameraGesture(a);return false;}
    return true;
}
bool AcceptViewportCameraPreview(MMDApp& a) noexcept {
    if(modelGesture&&modelGesture->app==&a)return AcceptViewportModelPreview(a);
    if(!gesture || gesture->app!=&a)return false;
    const auto values=Read(a);
    if(!Finite(values)){Write(a,gesture->change->after);CancelViewportCameraGesture(a);return false;}
    gesture->change->after=values;return true;
}
MmsResult EndViewportCameraGesture(MMDApp& a,bool& changed) noexcept {
    if(modelGesture&&modelGesture->app==&a)return EndViewportModelGesture(a,changed);
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
MmsResult BeginViewportModelGesture(MMDApp& app,SharedHistory& history,HWND owner,UINT button)noexcept {
    if(button!=WM_RBUTTONDOWN&&button!=WM_MBUTTONDOWN)return MMS_E_ARGUMENT;
    if(auto result=history.CheckNativeEdit())return result;
    if(gesture||modelGesture||app.CameraMode()||!ViewportReady(app)||GetCapture())return MMS_E_BUSY;
    if(MatchedModelFollow(app)||!ModelReadoutReady(app))return MMS_E_NOT_READY;
    if(!mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
    if(!FinitePreset(ReadPreset(app)))return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        auto pending=std::make_unique<ModelGesture>();pending->app=&app;pending->history=&history;pending->owner=owner;pending->button=button;
        pending->change=std::make_shared<NumpadChange>(app);auto& change=*pending->change;
        change.preview=true;change.previewOwner=owner;change.modelReadout=true;
        if(change.context.selected)change.AddModel(change.context.slot);
        change.AddParent(change.before.parentModel,change.before.parentBone);
        if(auto result=history.Revision(pending->revision))return result;
        if(auto result=history.PrepareNative("Drag model view",pending->change,pending->transaction,SharedHistory::NativeIntent::Adjustment))return result;
        modelGesture=std::move(pending);return MMS_OK;
    });
}
bool CancelViewportModelGesture(MMDApp& app)noexcept {
    if(!modelGesture||modelGesture->app!=&app)return false;
    auto pending=std::move(modelGesture);const bool restored=pending->Rollback();pending->transaction.reset();
    app.state.rightMouseButtonState=0;app.state.middleMouseButtonState=0;
    if(pending->owner&&GetCapture()==pending->owner)ReleaseCapture();return restored;
}
bool AllowViewportModelPreview(MMDApp& app,UINT button)noexcept {
    if(!modelGesture||modelGesture->app!=&app||modelGesture->button!=button)return false;
    if(!ViewportReady(app)||(modelGesture->owner&&GetCapture()!=modelGesture->owner)||!modelGesture->Current()){
        CancelViewportModelGesture(app);return false;
    }return true;
}
bool AcceptViewportModelPreview(MMDApp& app)noexcept {
    if(!modelGesture||modelGesture->app!=&app)return false;
    auto& pending=*modelGesture;
    std::uint64_t revision=0;
    if(!ViewportReady(app)||!pending.SourceCurrent()||(pending.owner&&GetCapture()!=pending.owner)||
        pending.history->Revision(revision)||revision!=pending.revision){CancelViewportModelGesture(app);return false;}
    const auto current=ReadPreset(app);
    constexpr unsigned pan=(1u<<6)|(1u<<7),orbit=pan|(1u<<3)|(1u<<4)|(1u<<8);
    const auto allowed=pending.button==WM_RBUTTONDOWN?orbit:pan;
    if(PresetMask(pending.change->after,current)&~allowed){CancelViewportModelGesture(app);return false;}
    if(!FinitePreset(current)){
        WritePreset(app,pending.change->after,allowed);CancelViewportModelGesture(app);return false;
    }
    pending.change->after=current;return true;
}
MmsResult EndViewportModelGesture(MMDApp& app,bool& changed)noexcept {
    changed=false;if(!modelGesture||modelGesture->app!=&app)return MMS_E_NOT_READY;
    auto pending=std::move(modelGesture);
    if(pending->owner&&GetCapture()==pending->owner)ReleaseCapture();
    app.state.rightMouseButtonState=0;app.state.middleMouseButtonState=0;
    if(!ViewportReady(app)||!pending->Current()){pending->Rollback();return MMS_E_STALE;}
    auto& change=*pending->change;change.mask=PresetMask(change.before,change.after);
    if(!change.mask)return MMS_OK;
    const auto result=pending->history->CommitNative(std::move(pending->transaction));
    if(result){pending->Rollback();return result;}changed=true;return MMS_OK;
}
MmsResult AdjustViewportCameraWheel(MMDApp& a,SharedHistory& h,std::int16_t delta,bool floating,bool& changed) noexcept {
    changed=false;
    // Validate thread/plugin admission before reading the native scene.
    if(auto r=h.CheckNativeEdit())return r;
    if(gesture || modelGesture || GetCapture() || !Ready(a))return MMS_E_BUSY;
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
MmsResult AdjustViewportModelWheel(MMDApp& app,SharedHistory& history,std::int16_t delta,bool floating,bool& changed)noexcept {
    changed=false;if(auto result=history.CheckNativeEdit())return result;
    if(gesture||modelGesture||app.CameraMode()||GetCapture()||!ViewportReady(app))return MMS_E_BUSY;
    if((!floating&&MatchedModelFollow(app))||!ModelReadoutReady(app))return MMS_E_NOT_READY;
    if(!mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
    const auto before=ReadPreset(app);if(!FinitePreset(before))return MMS_E_ARGUMENT;
    if(!delta)return MMS_OK;
    auto after=before;
    if(floating)after.distance=static_cast<float>(static_cast<double>(delta)*0.05000000074505806+static_cast<double>(before.distance));
    else after.distance=static_cast<float>(delta)*0.05f+before.distance;
    if(!FinitePreset(after))return MMS_E_ARGUMENT;
    const auto mask=PresetMask(before,after);if(!mask)return MMS_OK;
    return Boundary([&]()->MmsResult{
        auto change=std::make_shared<NumpadChange>(app);
        change->after=after;change->mask=mask;change->modelReadout=true;
        if(change->context.selected)change->AddModel(change->context.slot);
        change->AddParent(change->before.parentModel,change->before.parentBone);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(auto result=history.PrepareNative("Wheel model view",change,transaction,SharedHistory::NativeIntent::Adjustment))return result;
        if(auto result=history.CommitNative(std::move(transaction)))return result;
        changed=true;return MMS_OK;
    });
}
MmsResult PlanViewportNumpadPresets(MMDApp& app,std::uint16_t pressedKeys,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();constexpr unsigned supported=(1u<<2)|(1u<<4)|(1u<<6)|(1u<<8)|(1u<<5)|(1u<<0)|(1u<<1);
    if(pressedKeys&~supported)return MMS_E_ARGUMENT;
    if(!pressedKeys)return MMS_OK;
    if(gesture||modelGesture||GetCapture()||!ViewportReady(app))return MMS_E_BUSY;
    if(!mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
    if(!FinitePreset(ReadPreset(app)))return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        auto change=std::make_shared<NumpadChange>(app);
        const bool cameraStyle=change->context.cameraMode||change->context.follow;
        ViewportPresetContext context{cameraStyle,app.CameraReferenceMode()==CameraAttachmentReference::SelectedBone,
            app.CameraParentModel()>=0&&app.state.followCameraEnabled!=0};
        ViewportPresetState reset;
        if(!cameraStyle&&(pressedKeys&1u)){
            if(auto r=CaptureViewportResetCamera(app,change->resetCamera))return r;
            if(!change->resetCamera||!change->resetCamera->Sample(reset))return MMS_E_ARGUMENT;
        }
        constexpr unsigned order[]={2,4,6,8,5,0,1};
        for(const auto key:order)if(pressedKeys&(1u<<key)){
            ViewportPresetState target;
            if(!ComputeNumpadViewPreset(context,key,change->after,target,change->resetCamera?&reset:nullptr))return MMS_E_NOT_READY;
            change->after=target;
        }
        if(!FinitePreset(change->after))return MMS_E_ARGUMENT;
        change->mask=PresetMask(change->before,change->after);if(!change->mask)return MMS_OK;
        if(change->context.selected)change->AddModel(change->context.slot);
        change->AddParent(change->before.parentModel,change->before.parentBone);
        change->AddParent(change->after.parentModel,change->after.parentBone);
        if(!change->SameScene()||!change->InitialCurrent())return MMS_E_STALE;
        out=std::move(change);return MMS_OK;
    });
}
MmsResult AdjustViewportNumpadPresets(MMDApp& app,SharedHistory& history,std::uint16_t pressedKeys,bool& changed) noexcept {
    changed=false;if(auto r=history.CheckNativeEdit())return r;
    return Boundary([&]()->MmsResult{
        std::shared_ptr<HistoryChange> change;if(auto r=PlanViewportNumpadPresets(app,pressedKeys,change))return r;
        if(!change)return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(auto r=history.PrepareNative("Preset view",change,transaction,SharedHistory::NativeIntent::Adjustment))return r;
        if(auto r=history.CommitNative(std::move(transaction)))return r;
        changed=true;return MMS_OK;
    });
}
bool HandleBoundViewportMessage(MMDApp& a,HWND owner,UINT msg,WPARAM wp,LPARAM lp) noexcept {
    if(HandleCameraHudMessage(a,owner,msg,wp,lp))return true;
    if(msg==WM_CANCELMODE || msg==WM_KILLFOCUS || msg==WM_NCDESTROY ||
        (msg==WM_ACTIVATEAPP && !wp) || (msg==WM_KEYDOWN && wp==VK_ESCAPE) ||
        (msg==WM_CAPTURECHANGED&&((gesture&&reinterpret_cast<HWND>(lp)!=gesture->owner)||
            (modelGesture&&reinterpret_cast<HWND>(lp)!=modelGesture->owner)))){
        const bool active=(gesture&&gesture->app==&a)||(modelGesture&&modelGesture->app==&a);
        if(active){CancelViewportCameraGesture(a);if(GetCapture()==owner)ReleaseCapture();RefreshBoundPoseReadout(a);}
        return active && msg==WM_KEYDOWN;
    }
    auto* history=BoundNativePoseHistory(&a);if(!history)return false;
    if(msg==WM_MOUSEWHEEL){
        const bool floating=owner && owner==a.FloatingWindow();
        if(!floating && a.MouseX()<=a.SidebarWidth())return false;
        bool changed=false;
        const auto result=a.CameraMode()?AdjustViewportCameraWheel(a,*history,static_cast<std::int16_t>(HIWORD(wp)),floating,changed):
            AdjustViewportModelWheel(a,*history,static_cast<std::int16_t>(HIWORD(wp)),floating,changed);
        if(!result&&changed)
            PublishBoundPoseEdit(a,*history,false);
        return true;
    }
    if(msg==WM_RBUTTONDOWN || msg==WM_MBUTTONDOWN){
        POINT p{static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp))};
        if(!PtInRect(&a.ViewportRect(),p))return false;
        SetFocus(owner);
        const auto result=a.CameraMode()?BeginViewportCameraGesture(a,*history,owner,msg):BeginViewportModelGesture(a,*history,owner,msg);
        if(result)return true;
        a.MouseX()=a.PreviousMouseX()=p.x;a.MouseY()=a.PreviousMouseY()=p.y;
        SetCapture(owner);if(GetCapture()!=owner)CancelViewportCameraGesture(a);
        return true;
    }
    if(modelGesture&&modelGesture->app==&a&&modelGesture->owner==owner){
        if(msg==WM_MOUSEMOVE){SampleModelMessage(a,wp,lp);return true;}
        if((msg==WM_RBUTTONUP&&modelGesture->button==WM_RBUTTONDOWN)||(msg==WM_MBUTTONUP&&modelGesture->button==WM_MBUTTONDOWN)){
            SampleModelMessage(a,wp,lp);bool changed=false;
            if(modelGesture){const auto result=EndViewportModelGesture(a,changed);
                if(!result&&changed)PublishBoundPoseEdit(a,*history,false);else RefreshBoundPoseReadout(a);}
            return true;
        }return false;
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
