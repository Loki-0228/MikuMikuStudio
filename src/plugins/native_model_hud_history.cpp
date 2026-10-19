#include "native_model_hud_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace mikumikustudio { void FinishModelHudInteraction(MMDApp*,int,int) noexcept; }
namespace mikumikustudio::plugins {
namespace {
constexpr unsigned kRotation=8,kDirty=16,kScale=32,kGeometry=15|kScale;
struct Pose {
    std::array<float,3> position{};
    std::array<float,4> rotation{};
    unsigned char dirty=0;
    bone_scale::Matrix scale;
    bool scalePresent=false;
};
bool Bits(const void* a,const void* b,std::size_t bytes)noexcept{return !std::memcmp(a,b,bytes);}
unsigned Difference(const Pose& a,const Pose& b)noexcept{
    unsigned mask=0;for(unsigned i=0;i<3;++i)if(!Bits(&a.position[i],&b.position[i],sizeof(float)))mask|=1u<<i;
    if(!Bits(a.rotation.data(),b.rotation.data(),sizeof(a.rotation)))mask|=kRotation;
    if(a.dirty!=b.dirty)mask|=kDirty;
    if(a.scalePresent!=b.scalePresent||a.scale!=b.scale)mask|=kScale;return mask;
}
Pose Read(unsigned char* model,unsigned index)noexcept{
    const auto& m=*mdl::Mdl(model);Pose value;
    std::memcpy(value.position.data(),m.boneTable[index].trans,sizeof(value.position));
    std::memcpy(value.rotation.data(),m.boneTable[index].rotQuat,sizeof(value.rotation));value.dirty=m.bonePhysicsState[index];
    value.scale=bone_scale::Pose(model,int(index));value.scalePresent=bone_scale::HasPoseValue(model,int(index));return value;
}
void CopyFields(Pose& out,const Pose& from,unsigned mask)noexcept{
    for(unsigned i=0;i<3;++i)if(mask&(1u<<i))out.position[i]=from.position[i];
    if(mask&kRotation)out.rotation=from.rotation;if(mask&kDirty)out.dirty=from.dirty;
    if(mask&kScale){out.scale=from.scale;out.scalePresent=from.scalePresent;}
}
void Write(mdl::ModelRecord& m,unsigned index,const Pose& value,unsigned mask)noexcept{
    for(unsigned i=0;i<3;++i)if(mask&(1u<<i))m.boneTable[index].trans[i]=value.position[i];
    if(mask&kRotation)std::memcpy(m.boneTable[index].rotQuat,value.rotation.data(),sizeof(value.rotation));
    if(mask&kDirty)m.bonePhysicsState[index]=value.dirty;
}
bool Finite(const Pose& value)noexcept{
    for(float v:value.position)if(!std::isfinite(v))return false;
    for(float v:value.rotation)if(!std::isfinite(v))return false;
    return bone_scale::Valid(value.scale);
}
bool Ready(MMDApp& app,bool replay=false)noexcept{
    const bool authorized=replay&&NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive()&&!app.FrameStepPlayback()&&!app.state.recordPlaybackStartPending&&
        (!app.SceneMutationInProgress()||authorized)&&(!bake::UiLocked()||authorized)&&!SceneSaveInProgress()&&
        !app.FrameRangeDialog()&&!app.state.frameCopyDialog&&!app.RecordingWindow()&&!app.GravitySettingDialog()&&
        !app.GroundShadowColorDialog()&&!app.AccessoryOrderArray()&&!app.state.accessoryEditDialogOpen&&!app.state.edgeThicknessDialog;
}
std::uint64_t Metadata(const mdl::ModelRecord& model)noexcept{
    std::uint64_t hash=14695981039346656037ull;
    const auto add=[&](const void* data,std::size_t bytes){auto* p=static_cast<const unsigned char*>(data);while(bytes--){hash^=*p++;hash*=1099511628211ull;}};
    add(&model.physicsMode,sizeof(model.physicsMode));add(&model.ikChainCount,sizeof(model.ikChainCount));add(&model.boneOrderCount,sizeof(model.boneOrderCount));
    for(unsigned i=0;i<model.boneCount;++i){const auto& b=model.boneTable[i];
        add(&b.type,sizeof(b.type));add(&b.flags,sizeof(b.flags));add(&b.parent,sizeof(b.parent));add(&b.tailBone,sizeof(b.tailBone));
        add(b.position,sizeof(b.position));add(b.axis,sizeof(b.axis));add(b.localAxes,sizeof(b.localAxes));add(b.name,sizeof(b.name));
        add(&b.hasRigidBody,sizeof(b.hasRigidBody));add(&b.physicsDisabled,sizeof(b.physicsDisabled));
    }return hash;
}
bool NamedLocalAxisTail(const mdl::BoneRecord& bone)noexcept{
    // Match BoneLocalAxes' original SJIS needles and NUL-inclusive exact
    // comparisons. Ordinary terminal bones are allowed to keep tail=-1.
    return !std::memcmp(bone.name,"\x89\x45\x8E\xE8\x8E\xF1",7)||
        !std::memcmp(bone.name,"\x89\x45\x82\xD0\x82\xB6",7)||
        !std::memcmp(bone.name,"\x89\x45\x98\x72",5)||
        !std::memcmp(bone.name,"\x8D\xB6\x8E\xE8\x8E\xF1",7)||
        !std::memcmp(bone.name,"\x8D\xB6\x82\xD0\x82\xB6",7)||
        !std::memcmp(bone.name,"\x8D\xB6\x98\x72",5)||std::strstr(bone.name,"\x8E\x77")!=nullptr;
}
bool ValidModel(MMDApp& app,const mdl::ModelRecord& m,int operation)noexcept{
    if(!m.boneCount||m.boneCount>INT_MAX||!m.boneTable||!m.bonePhysicsState||!m.boneSelection||m.selectedBone<0||unsigned(m.selectedBone)>=m.boneCount)return false;
    for(unsigned i=0;i<m.boneCount;++i){const auto& b=m.boneTable[i];
        if(b.parent< -1||(b.parent>=0&&unsigned(b.parent)>=m.boneCount)||!std::memchr(b.name,0,sizeof(b.name)))return false;
    }
    const auto& selected=m.boneTable[m.selectedBone];
    if(selected.tailBone>=0&&unsigned(selected.tailBone)<m.boneCount)return true;
    const bool rotation=(operation>=3&&operation<=5)||(operation>=9&&operation<=11);
    if(rotation&&(selected.flags&mdl::kBoneFlagFixedAxis)&&m.physicsMode!=2&&
        (selected.type==mdl::BoneType::UnderIk||selected.type==mdl::BoneType::FixedAxis))return false;
    const bool localAxis=EffectiveCoordinateState(&app)==0&&
        ((operation>=9&&operation<=11&&app.state.physicsEditorJointPage!=1)||(operation>=12&&operation<=14)||(operation>=23&&operation<=25));
    const bool explicitAxes=m.physicsMode==2&&(selected.localAxes[0]!=0||selected.localAxes[1]!=0||selected.localAxes[2]!=0);
    return !localAxis||explicitAxes||!NamedLocalAxisTail(selected);
}
struct Change final:HistoryChange,std::enable_shared_from_this<Change>{
    MMDApp& app;const unsigned slot;unsigned char* const model;const MmsObjectRef identity;
    mdl::BoneRecord* const bones;unsigned char* const dirty;unsigned char* const selection;const unsigned count;
    const std::uint64_t keys,metadata,binding;const int frame,selected,coordinate,physicsMode,operation,editMode;
    std::shared_ptr<RetainedSceneObject> owner;
    std::unique_ptr<bone_scale::PoseStorageLease> storage;
    std::vector<Pose> before,after;std::vector<unsigned char> masks,selectionBefore,selectionAfter;
    bone_scale::Values rollbackNodes;
    HWND window=nullptr;std::uint64_t generation=1;bool preview=true,applied=false,autoSelected=false;
    Change(MMDApp& a,int op):app(a),slot(a.SelectedModelSlot()),model(a.ModelSlot(slot)),identity(SceneObjectIdentity(model)),
        bones(mdl::Mdl(model)->boneTable),dirty(mdl::Mdl(model)->bonePhysicsState),selection(mdl::Mdl(model)->boneSelection),count(mdl::Mdl(model)->boneCount),
        keys(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),metadata(Metadata(*mdl::Mdl(model))),binding(BoundNativePoseGeneration(&a)),
        frame(a.CurrentFrame()),selected(mdl::Mdl(model)->selectedBone),coordinate(EffectiveCoordinateState(&a)),physicsMode(a.PlaybackPhysicsMode()),
        operation(op),editMode(int(a.EditMode())),owner(RetainSceneObject(a,MMS_OBJECT_MODEL,slot)),before(count),after(count),masks(count,0),
        selectionBefore(selection,selection+count),selectionAfter(selectionBefore){
        if(!keys||!owner)throw std::invalid_argument("model HUD key pool or retained owner");
        for(unsigned i=0;i<count;++i){before[i]=after[i]=Read(model,i);if(!Finite(before[i]))throw std::invalid_argument("model HUD pose");
            rollbackNodes.emplace(int(i),before[i].scale);}
        if(op>=22)storage=bone_scale::PoseStorageLease::Prepare(model);
    }
    bool SameScene()const noexcept{
        if(app.ModelSlot(slot)!=model||!owner->Present()||!SceneObjectPresent(model,identity.id))return false;
        const auto& m=*mdl::Mdl(model);return m.boneTable==bones&&m.bonePhysicsState==dirty&&m.boneSelection==selection&&m.boneCount==count&&
            mdl::BoneKeyPoolIdentity(m.boneKeys)==keys&&Metadata(m)==metadata;
    }
    bool Domain()const noexcept{return SameScene()&&SceneObjectIdentity(model).generation==identity.generation&&app.CurrentFrame()==frame&&BoundNativePoseGeneration(&app)==binding;}
    bool OwnedCurrent()const noexcept{for(unsigned i=0;i<count;++i)if(Difference(Read(model,i),after[i])&masks[i])return false;return true;}
    bool SourceCurrent()const noexcept{return Domain()&&app.CameraMode()==0&&app.SelectedModelSlot()==slot&&mdl::Mdl(model)->selectedBone==selected&&
        EffectiveCoordinateState(&app)==coordinate&&app.PlaybackPhysicsMode()==physicsMode&&Bits(selection,selectionAfter.data(),count)&&OwnedCurrent();}
    bool CaptureCurrent()const noexcept{const auto capture=GetCapture();return !capture||(preview&&window&&capture==window);}
    bool GeometryChanged()const noexcept{for(unsigned i=0;i<count;++i)if((Difference(before[i],after[i])&masks[i]&kGeometry))return true;return false;}
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<Change> source;const bool forward,initial;const std::uint64_t generation,binding,objectGeneration;const int frame;
        std::vector<Pose> expected;bone_scale::Values scaleBefore,scaleTarget;mutable resource_budget::Collector resources;std::uint64_t bytes=0;bool consumed=false;
        Candidate(std::shared_ptr<Change> s,bool f):source(std::move(s)),forward(f),initial(source->preview),generation(source->generation),
            binding(BoundNativePoseGeneration(&source->app)),objectGeneration(SceneObjectIdentity(source->model).generation),frame(source->app.CurrentFrame()){
            if(!initial){expected.resize(source->count);for(unsigned i=0;i<source->count;++i)expected[i]=Read(source->model,i);
                scaleBefore=bone_scale::PoseValues(source->model);scaleTarget=scaleBefore;
                const auto& target=f?source->after:source->before;for(unsigned i=0;i<source->count;++i)if(source->masks[i]&kScale){
                    if(target[i].scalePresent)scaleTarget[int(i)]=target[i].scale;else scaleTarget.erase(int(i));}
                if(!scaleTarget.empty()&&!bone_scale::HasPoseStorage(source->model))throw std::invalid_argument("model HUD scale storage");
            }
        }
        bool IsCurrent()const noexcept override{
            if(consumed||source->generation!=generation||forward==source->applied||!source->SameScene()||!Ready(source->app,generation!=1)||
                !source->CaptureCurrent()||source->app.CurrentFrame()!=frame||BoundNativePoseGeneration(&source->app)!=binding||
                SceneObjectIdentity(source->model).generation!=objectGeneration||!source->owner->CoversBookkeeping(resources))return false;
            if(initial)return source->SourceCurrent();
            if(!bone_scale::PoseValuesEqual(source->model,scaleBefore))return false;
            for(unsigned i=0;i<source->count;++i)if(Difference(Read(source->model,i),expected[i]))return false;return true;
        }
        void Publish()noexcept override{
            if(!IsCurrent())std::terminate();
            if(!initial){bool scaleChanged=false;for(unsigned i=0;i<source->count;++i)scaleChanged|=bool(source->masks[i]&kScale);
                if(scaleChanged&&!bone_scale::ExchangePoseValues(source->model,scaleTarget))std::terminate();
                const auto& target=forward?source->after:source->before;for(unsigned i=0;i<source->count;++i)Write(*mdl::Mdl(source->model),i,target[i],source->masks[i]);}
            source->preview=false;source->applied=forward;++source->generation;consumed=true;
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override{out=bytes;return true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();return Boundary([&]()->MmsResult{
            if(!SameScene()||forward==applied||(generation==1&&!SourceCurrent()))return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;if(!Ready(app,generation!=1)||!CaptureCurrent())return MMS_E_BUSY;
            auto next=std::make_unique<Candidate>(shared_from_this(),forward);
            if(auto r=owner->MeasureBookkeeping(next->resources))return r;
            if(!next->resources.Result().Complete())return MMS_E_NOT_READY;
            next->bytes=resource_budget::Collector::Sum(RetainedBytes(),next->resources.Result().totalBytes);
            if(!next->IsCurrent())return MMS_E_STALE;out=std::move(next);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);
        const auto add=[&](std::uint64_t n){bytes=n>UINT64_MAX-bytes?UINT64_MAX:bytes+n;};
        add((before.capacity()+after.capacity())*sizeof(Pose));add(masks.capacity()+selectionBefore.capacity()+selectionAfter.capacity());
        if(storage)add(storage->RetainedBytes());
        add((rollbackNodes.size()+1)*(sizeof(bone_scale::Values::value_type)+4*sizeof(void*)));return bytes;
    }
};
struct Draft{
    std::unique_ptr<MMDApp> app;
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;std::vector<unsigned char> dirty,selection;
    std::vector<Pose> before,after;
    explicit Draft(MMDApp& source,unsigned count):app(std::make_unique<MMDApp>(source)),bones(count),dirty(count),selection(count),before(count),after(count){}
    ~Draft(){bone_scale::gizmo::Forget(app.get());bone_scale::Forget(reinterpret_cast<unsigned char*>(model.get()));}
    unsigned char* Raw()const noexcept{return reinterpret_cast<unsigned char*>(model.get());}
    void Run(Change& change){
        *app=change.app;*model=*mdl::Mdl(change.model);std::copy(change.bones,change.bones+change.count,bones.begin());
        std::copy(change.dirty,change.dirty+change.count,dirty.begin());std::copy(change.selection,change.selection+change.count,selection.begin());
        model->boneTable=bones.data();model->bonePhysicsState=dirty.data();model->boneSelection=selection.data();app->ModelSlot(change.slot)=Raw();
        app->InteractionDragMode()=static_cast<ViewportDragMode>(change.operation>=22?change.operation-3:
            change.operation<=5?change.operation-2:change.operation<=8?change.operation+1:
            change.operation<=11?change.operation-5:change.operation-2);
        bone_scale::CopyPoseRuntime(change.model,Raw());for(unsigned i=0;i<change.count;++i)before[i]=Read(change.model,i);
        if(change.operation<22)BoneEditModes(app.get());
        else if(change.operation<=25)bone_scale::Drag(app.get(),change.operation-23);
        else{bone_scale::gizmo::Publish(app.get(),bone_scale::gizmo::Published(&change.app));
            if(change.operation<=28)bone_scale::DragScreen(app.get(),change.operation-26);else bone_scale::DragScreenPlane(app.get());}
        for(unsigned i=0;i<change.count;++i){after[i]=Read(Raw(),i);if(!Finite(after[i]))throw std::invalid_argument("model HUD nonfinite result");}
    }
};
struct Gesture{
    MMDApp* app;SharedHistory* history;HWND owner;std::uint64_t revision=0;std::shared_ptr<Change> change;
    std::unique_ptr<Draft> draft;std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    bone_scale::Values rollbackScale;
    unsigned char dirtyBefore=0,dirtyAfter=0,layoutBefore=0,layoutAfter=0;bool hintTouched=false;
    bool Current()const noexcept{std::uint64_t live=0;return !history->Revision(live)&&live==revision&&change->SourceCurrent();}
    bool Rollback()noexcept{
        if(!change->Domain())return false;
        bool scaleChanged=false;for(unsigned i=0;i<change->count;++i)scaleChanged|=bool(change->masks[i]&kScale);
        if(scaleChanged&&!bone_scale::HasPoseStorage(change->model))return false;
        for(unsigned i=0;i<change->count;++i){const auto current=Read(change->model,i);unsigned restore=change->masks[i]&~Difference(current,change->after[i]);
            if(Difference(current,change->after[i])&kGeometry)restore&=~kDirty;
            Write(*mdl::Mdl(change->model),i,change->before[i],restore);}
        if(scaleChanged){
        auto& scale=rollbackScale;
        if(!bone_scale::ExchangePoseValues(change->model,scale))return false;
        for(unsigned i=0;i<change->count;++i)if(change->masks[i]&kScale){const auto found=scale.find(int(i));
            const bool present=found!=scale.end();const auto value=present?found->second:bone_scale::Matrix{};
            if(present==change->after[i].scalePresent&&value==change->after[i].scale){
                if(!change->before[i].scalePresent){if(present)scale.erase(found);}
                else if(present)found->second=change->before[i].scale;
                else{auto node=change->rollbackNodes.extract(int(i));node.mapped()=change->before[i].scale;scale.insert(std::move(node));}
            }
        }
        if(!bone_scale::ExchangePoseValues(change->model,scale))std::terminate();
        }
        if(change->autoSelected&&change->selection[change->selected]==change->selectionAfter[change->selected])
            change->selection[change->selected]=change->selectionBefore[change->selected];
        return true;
    }
    void CancelHint()noexcept{
        if(!hintTouched)return;if(app->state.viewDirty==dirtyAfter)app->state.viewDirty=dirtyBefore;
        if(app->state.windowLayoutReady==layoutAfter)app->state.windowLayoutReady=layoutBefore;
    }
};
thread_local std::unique_ptr<Gesture> gesture;
}
int ModelHudOperationForMode(int mode)noexcept{return mode>=1&&mode<=3?mode+2:mode>=4&&mode<=6?mode+5:mode>=7&&mode<=9?mode-1:
    mode>=10&&mode<=12?mode+2:mode>=19&&mode<=26?mode+3:0;}
bool ModelHudOperation(MMDApp& app,int operation)noexcept{return ((operation>=3&&operation<=14)||(operation>=22&&operation<=29))&&
    !app.CameraMode()&&app.SelectedModelSlot()<kModelSlotCount&&app.SelectedModel()&&ValidModel(app,*mdl::Mdl(app.SelectedModel()),operation);}
bool ModelHudGestureActive(const MMDApp& app)noexcept{return gesture&&gesture->app==&app;}
std::shared_ptr<const HistoryChange> ModelHudPreviewSource(MMDApp& app)noexcept{return ModelHudGestureActive(app)?gesture->change:nullptr;}
MmsResult BeginModelHudGesture(MMDApp& app,SharedHistory& history,HWND owner,int operation)noexcept{
    if(!ModelHudOperation(app,operation))return MMS_E_NOT_READY;
    if(auto r=history.CheckNativeNavigation())return r;if(auto r=history.CheckNativeEdit())return r;
    if(gesture||!Ready(app)||(GetCapture()&&GetCapture()!=owner))return MMS_E_BUSY;
    return Boundary([&]()->MmsResult{
        auto next=std::make_unique<Gesture>();next->app=&app;next->history=&history;next->owner=owner;
        next->change=std::make_shared<Change>(app,operation);next->change->window=owner;
        next->draft=std::make_unique<Draft>(app,next->change->count);next->dirtyBefore=app.state.viewDirty;next->layoutBefore=app.state.windowLayoutReady;
        if(auto r=history.Revision(next->revision))return r;
        if(auto r=history.PrepareNative("Drag model HUD",next->change,next->transaction,SharedHistory::NativeIntent::Adjustment))return r;
        if(operation>=22&&next->change->selection[next->change->selected]==0){next->change->autoSelected=true;
            next->change->selection[next->change->selected]=next->change->selectionAfter[next->change->selected]=1;}
        gesture=std::move(next);return MMS_OK;
    });
}
bool CancelModelHudGesture(MMDApp& app)noexcept{
    if(!ModelHudGestureActive(app))return false;auto current=std::move(gesture);
    const bool restored=current->Rollback();current->CancelHint();current->transaction.reset();
    FinishModelHudInteraction(&app,current->change->operation,current->change->editMode);
    if(current->owner&&GetCapture()==current->owner)ReleaseCapture();if(restored)updates::RequestViewport(&app);return restored;
}
bool AllowModelHudPreview(MMDApp& app,int operation)noexcept{
    if(!ModelHudGestureActive(app)||gesture->change->operation!=operation)return false;
    if(!Ready(app)||!ModelHudOperation(app,operation)||(gesture->owner&&GetCapture()!=gesture->owner)||!gesture->Current()){
        CancelModelHudGesture(app);return false;}return true;
}
MmsResult ApplyModelHudSample(MMDApp& app,int operation)noexcept{
    if(!AllowModelHudPreview(app,operation))return MMS_E_STALE;const auto source=gesture->change;
    const auto result=Boundary([&]()->MmsResult{
        auto& active=*gesture;active.draft->Run(*source);if(!active.Current())return MMS_E_STALE;
        bool changed=false,scaleChanged=false;for(unsigned i=0;i<source->count;++i){const auto fields=Difference(active.draft->before[i],active.draft->after[i]);
            changed|=bool(fields&kGeometry);scaleChanged|=bool(fields&kScale);}
        if(!changed)return MMS_OK;
        if(scaleChanged){auto scale=bone_scale::PoseValues(active.draft->Raw());if(!active.Current())return MMS_E_STALE;
            if(!bone_scale::ExchangePoseValues(source->model,scale))return MMS_E_STALE;}
        for(unsigned i=0;i<source->count;++i){const auto fields=Difference(active.draft->before[i],active.draft->after[i]);
            CopyFields(source->before[i],active.draft->before[i],fields&~source->masks[i]);source->masks[i]|=static_cast<unsigned char>(fields);
            source->after[i]=active.draft->after[i];Write(*mdl::Mdl(source->model),i,source->after[i],fields);}
        return MMS_OK;
    });
    if(result&&ModelHudPreviewSource(app)==source)CancelModelHudGesture(app);return result;
}
void RecordModelHudRefreshHint(MMDApp& app)noexcept{if(!ModelHudGestureActive(app))return;gesture->hintTouched=true;
    gesture->dirtyAfter=app.state.viewDirty;gesture->layoutAfter=app.state.windowLayoutReady;}
MmsResult EndModelHudGesture(MMDApp& app,bool& changed)noexcept{
    changed=false;if(!ModelHudGestureActive(app))return MMS_E_NOT_READY;auto current=std::move(gesture);
    if(!Ready(app)||(current->owner&&GetCapture()!=current->owner)||!current->Current()){
        current->Rollback();current->CancelHint();return MMS_E_STALE;}
    if(!current->change->GeometryChanged()){current->Rollback();current->CancelHint();return MMS_OK;}
    const auto result=current->history->CommitNative(std::move(current->transaction));
    if(result){current->Rollback();current->CancelHint();return result;}changed=true;return MMS_OK;
}
bool HandleModelHudMessage(MMDApp& app,HWND owner,UINT message,WPARAM wp,LPARAM lp)noexcept{
    if(!ModelHudGestureActive(app))return false;
    if(message==WM_CANCELMODE||message==WM_KILLFOCUS||message==WM_CLOSE||message==WM_DESTROY||message==WM_NCDESTROY||
        (message==WM_ACTIVATEAPP&&!wp)||(message==WM_KEYDOWN&&wp==VK_ESCAPE)||
        (message==WM_CAPTURECHANGED&&reinterpret_cast<HWND>(lp)!=gesture->owner)){
        CancelModelHudGesture(app);RefreshBoundPoseReadout(app);return message==WM_KEYDOWN;}
    if(message==WM_LBUTTONUP&&gesture->owner==owner){const auto source=gesture->change;const int operation=source->operation;
        if(AllowModelHudPreview(app,operation)){
            POINT point{};if(owner&&GetCursorPos(&point)&&ScreenToClient(owner,&point)){
                app.MouseX()=point.x;app.MouseY()=point.y;app.ShiftModifierState()=(wp&MK_SHIFT)?3:0;app.CtrlModifierState()=(wp&MK_CONTROL)?3:0;
                if(operation<=5)ModeRotate(&app,operation-3);
                else if(operation==6)ModeScale(&app);
                else if(operation<=8)ModeBoneRotate(&app,operation-7);
                else if(operation<=11)ModeTranslate(&app,operation-9);
                else if(operation<=14)ModePhysicsBody(&app,operation-12);
                else{ApplyModelHudSample(app,operation);RefreshBoundPoseReadout(app);}
                if(ModelHudPreviewSource(app)!=source)return true;
                app.PreviousMouseX()=app.MouseX();app.PreviousMouseY()=app.MouseY();
            }
        }
        if(ModelHudPreviewSource(app)!=source)return true;
        auto* history=gesture->history;bool changed=false;const auto result=EndModelHudGesture(app,changed);
        // Own the old release cleanup before readout callbacks can create a
        // replacement gesture. No outer legacy mouse-up may then clear it.
        FinishModelHudInteraction(&app,operation,source->editMode);
        if(GetCapture()==owner)ReleaseCapture();
        if(!result&&changed)PublishBoundPoseEdit(app,*history,false);else RefreshBoundPoseReadout(app);
        return true;
    }return false;
}
}
#endif
