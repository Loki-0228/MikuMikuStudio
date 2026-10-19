#include "native_camera_follow.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_camera_follow_sample.hpp"
#include "native_camera_follow_ui.hpp"
#include "native_viewport_reset_camera.hpp"
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "native_history_commands.hpp"
#include "native_global_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <type_traits>

namespace mikumikustudio::plugins {
namespace {
bool Ready(MMDApp& a,bool history=false) noexcept {
    const bool authorized=history&&NativeObjectPublicationAuthorized(&a);
    return !a.PlaybackActive()&&!a.FrameStepPlayback()&&!a.state.recordPlaybackStartPending&&
        (!a.SceneMutationInProgress()||authorized)&&(!bake::UiLocked()||authorized)&&!SceneSaveInProgress()&&
        !GetCapture()&&!a.FrameRangeDialog()&&!a.state.frameCopyDialog&&!a.RecordingWindow()&&
        !a.GravitySettingDialog()&&!a.GroundShadowColorDialog()&&!a.AccessoryOrderArray()&&
        !a.state.accessoryEditDialogOpen&&!a.state.edgeThicknessDialog&&
        (!NativeObjectOperationActive(&a)||authorized)&&!NativeObjectUiPending(&a);
}
ViewportPresetState View(MMDApp& a) noexcept {
    ViewportPresetState v;
    for(unsigned i=0;i<3;++i){v.cameraPosition[i]=a.CameraPositionAxis(i);v.rotation[i]=a.CameraRotation()[i];}
    v.offsetX=a.ViewOffsetX();v.offsetY=a.ViewOffsetY();v.distance=a.CameraDistance();v.fov=a.CameraFov();
    std::memcpy(v.basis.data(),&a.CameraAttachmentBasis(),sizeof(float)*16);
    v.suppressed=a.CameraAttachmentTransformSuppressed();v.perspective=a.CameraPerspective();
    v.parentModel=a.CameraParentModel();v.parentBone=a.CameraParentBone();return v;
}
void Finite(const ViewportPresetState& v){
    for(float f:v.cameraPosition)if(!std::isfinite(f))throw std::invalid_argument("follow camera position");
    for(float f:v.rotation)if(!std::isfinite(f))throw std::invalid_argument("follow camera rotation");
    for(float f:v.basis)if(!std::isfinite(f))throw std::invalid_argument("follow camera basis");
    for(float f:{v.offsetX,v.offsetY,v.distance,v.fov})if(!std::isfinite(f))throw std::invalid_argument("follow camera value");
}
using Bytes=std::array<unsigned char,4>;
struct Field {
    void* address;unsigned char size;Bytes before{},after{};bool changed=false;
    Bytes Read()const noexcept{Bytes b{};std::memcpy(b.data(),address,size);return b;}
    void Write(bool forward)const noexcept{if(changed)std::memcpy(address,(forward?after:before).data(),size);}
};
struct Model {
    unsigned slot;unsigned char* raw;mdl::BoneRecord* table;unsigned count;std::uint64_t initialGeneration;
    std::shared_ptr<RetainedSceneObject> owner;
    Model(MMDApp& app,unsigned s):slot(s),raw(app.ModelSlot(s)),table(mdl::Mdl(raw)->boneTable),count(mdl::Mdl(raw)->boneCount),
        initialGeneration(SceneObjectIdentity(raw).generation),owner(RetainSceneObject(app,MMS_OBJECT_MODEL,s)){}
    bool Current(MMDApp& app,bool initial)const noexcept{
        return app.ModelSlot(slot)==raw&&owner->Present()&&SceneObjectPresent(raw,owner->Id())&&
            mdl::Mdl(raw)->boneTable==table&&mdl::Mdl(raw)->boneCount==count&&
            (!initial||SceneObjectIdentity(raw).generation==initialGeneration);
    }
};
struct Bone {unsigned model,index;std::array<float,16> matrix;std::array<float,3> position;};
struct FollowChange final:HistoryChange,std::enable_shared_from_this<FollowChange>{
    MMDApp& app;const std::uint64_t pool,binding;const int frame;const HWND focus;
    const unsigned slot;const unsigned char cameraMode,reference,follow;
    unsigned char* selected;int selectedBone=-1;
    std::vector<Model> models;std::vector<Bone> bones;std::vector<Field> fields;
    std::shared_ptr<ViewportResetCameraSource> camera;
    std::shared_ptr<CameraFollowSampleSource> samples;
    std::uint64_t generation=1;bool applied=false,projectionChanged=false;
    explicit FollowChange(MMDApp& a):app(a),pool(mdl::KeyPoolIdentity(a.CameraKeys())),binding(BoundNativePoseGeneration(&a)),
        frame(a.CurrentFrame()),focus(GetFocus()),slot(a.SelectedModelSlot()),cameraMode(a.CameraMode()),
        reference(static_cast<unsigned char>(a.CameraReferenceMode())),follow(a.state.followCameraEnabled),
        selected(slot<kModelSlotCount?a.ModelSlot(slot):nullptr){if(selected)selectedBone=mdl::Mdl(selected)->selectedBone;}
    template<class T,class U>void Add(T& target,U value){
        static_assert(sizeof(T)<=4,"follow scalar width");const T converted=static_cast<T>(value);
        if constexpr(std::is_floating_point_v<T>)if(!std::isfinite(target)||!std::isfinite(converted))throw std::invalid_argument("follow scalar value");
        Field field{&target,static_cast<unsigned char>(sizeof(T))};field.before=field.Read();std::memcpy(field.after.data(),&converted,sizeof(T));field.changed=field.before!=field.after;
        const auto start=reinterpret_cast<std::uintptr_t>(field.address);
        for(const auto& previous:fields){const auto p=reinterpret_cast<std::uintptr_t>(previous.address);
            if(start<p+previous.size&&p<start+field.size){
                if(start==p&&field.size==previous.size&&field.before==previous.before&&field.after==previous.after)return;
                throw std::invalid_argument("aliased follow fields");}}
        fields.push_back(field);
    }
    unsigned AddModel(unsigned s){
        for(unsigned i=0;i<models.size();++i)if(models[i].slot==s)return i;
        if(s>=kModelSlotCount||!app.ModelSlot(s))throw std::invalid_argument("follow model");
        models.emplace_back(app,s);return unsigned(models.size()-1);
    }
    const Bone& AddBone(unsigned s,int index){
        const auto m=AddModel(s);const auto& model=models[m];
        if(!model.table||index<0||unsigned(index)>=model.count)throw std::invalid_argument("follow bone");
        for(const auto& b:bones)if(b.model==m&&b.index==unsigned(index))return b;
        Bone b{m,unsigned(index)};const auto& live=model.table[index];
        std::memcpy(b.matrix.data(),live.matInit,sizeof(float)*16);std::memcpy(b.position.data(),live.position,sizeof(float)*3);
        for(float f:b.matrix)if(!std::isfinite(f))throw std::invalid_argument("follow parent matrix");
        for(float f:b.position)if(!std::isfinite(f))throw std::invalid_argument("follow parent point");
        bones.push_back(b);return bones.back();
    }
    std::array<float,3> Focus(const ViewportPresetState& v,bool tracking){
        const bool cameraView=cameraMode||tracking;
        if(cameraView&&v.parentModel>=0){const auto& b=AddBone(unsigned(v.parentModel),v.parentBone);
            std::array<float,3> out{};for(unsigned i=0;i<3;++i)out[i]=b.matrix[i]*b.position[0]+b.matrix[4+i]*b.position[1]+b.matrix[8+i]*b.position[2]+b.matrix[12+i];return out;}
        if(cameraView)return v.cameraPosition;
        if(reference&&selected){const auto m=AddModel(slot);const auto& ref=models[m];const int index=reference==1?0:selectedBone;
            if(ref.table&&index>=0&&unsigned(index)<ref.count){const auto& b=AddBone(slot,index);std::array<float,3> out{};
                for(unsigned i=0;i<3;++i)out[i]=b.matrix[i]*b.position[0]+b.matrix[4+i]*b.position[1]+b.matrix[8+i]*b.position[2]+b.matrix[12+i];return out;}}
        return {};
    }
    void ViewFields(const ViewportPresetState& v){
        // Undo can restore the dirty pre-sample parent. Keep both endpoints
        // alive, not just the parent read by the newly evaluated camera.
        if(app.CameraParentModel()>=0)AddBone(unsigned(app.CameraParentModel()),app.CameraParentBone());
        if(v.parentModel>=0)AddBone(unsigned(v.parentModel),v.parentBone);
        for(unsigned i=0;i<3;++i){Add(app.CameraPositionAxis(i),v.cameraPosition[i]);Add(app.CameraRotation()[i],v.rotation[i]);}
        Add(app.ViewOffsetX(),v.offsetX);Add(app.ViewOffsetY(),v.offsetY);Add(app.CameraDistance(),v.distance);Add(app.CameraFov(),v.fov);
        for(unsigned i=0;i<16;++i)Add((&app.CameraAttachmentBasis().m[0][0])[i],v.basis[i]);
        Add(app.CameraAttachmentTransformSuppressed(),v.suppressed);Add(app.CameraPerspective(),v.perspective);
        Add(app.CameraParentModel(),v.parentModel);Add(app.CameraParentBone(),v.parentBone);
        const auto before=View(app);projectionChanged=before.distance!=v.distance||before.fov!=v.fov||before.perspective!=v.perspective;
    }
    void TrackFields(){
        const auto& v=samples->Values();auto& light=app.SceneLight();Add(light.Type,D3DLIGHT_DIRECTIONAL);
        for(unsigned i=0;i<3;++i){Add(app.LightDirection()[i],v.light.direction[i]);Add(app.LightColor()[i],v.light.color[i]);
            Add((&light.Direction.x)[i],v.light.direction[i]);Add((&light.Specular.r)[i],v.light.color[i]);Add((&light.Ambient.r)[i],v.light.color[i]);}
        Add(app.ShadowMode(),v.shadow.mode);Add(app.ShadowDistance(),v.shadow.distance);
        Add(app.GravityMagnitude(),v.gravity.magnitude);for(unsigned i=0;i<3;++i)Add(app.GravityDirection()[i],v.gravity.direction[i]);
        Add(app.state.gravityNoise,v.gravity.noise);Add(app.GravityNoiseModeWord(),v.gravity.modeWord);
        for(const auto& a:v.accessories){auto& o=*a.object;
            if(o.parentModel>=0)AddBone(unsigned(o.parentModel),o.parentBone);
            Add(o.visible,a.value.visible);Add(o.shadowEnabled,a.value.shadowEnabled);
            Add(o.parentModel,a.value.parentModel);Add(o.parentBone,a.value.parentBone);
            for(unsigned i=0;i<3;++i){Add(o.position[i],a.value.position[i]);Add(o.rotation[i],a.value.rotation[i]);}
            Add(o.scale,a.value.scale);Add(o.opacity,a.value.opacity);}
    }
    bool SameScene(bool initial=false)const noexcept{
        if(!pool||mdl::KeyPoolIdentity(app.CameraKeys())!=pool)return false;
        for(const auto& m:models)if(!m.Current(app,initial))return false;
        if(samples)for(const auto& a:samples->Values().accessories){
            if(app.AccessorySlot(a.slot)!=a.object||!a.owner->Present()||!SceneObjectPresent(a.object,a.id))return false;
            if(a.parent.owner&&(!a.parent.owner->Present()||app.ModelSlot(a.parent.slot)!=a.parent.model||
                !SceneObjectPresent(a.parent.model,a.parent.id)||
                mdl::Mdl(a.parent.model)->boneTable!=a.parent.bones||mdl::Mdl(a.parent.model)->boneCount!=a.parent.boneCount))return false;}
        return true;
    }
    bool InitialCurrent()const noexcept{
        if(generation!=1)return true;
        if(!Ready(app)||GetFocus()!=focus||BoundNativePoseGeneration(&app)!=binding||app.CurrentFrame()!=frame||
            app.CameraMode()!=cameraMode||app.SelectedModelSlot()!=slot||app.state.followCameraEnabled!=follow||
            static_cast<unsigned char>(app.CameraReferenceMode())!=reference||(slot<kModelSlotCount&&app.ModelSlot(slot)!=selected)||
            (selected&&mdl::Mdl(selected)->selectedBone!=selectedBone)||!SameScene(true)||
            (camera&&!camera->IsCurrent())||(samples&&!samples->IsCurrent()))return false;
        for(const auto& b:bones){const auto& live=models[b.model].table[b.index];if(std::memcmp(b.matrix.data(),live.matInit,sizeof(float)*16)||
            std::memcmp(b.position.data(),live.position,sizeof(float)*3))return false;}
        for(const auto& f:fields)if(f.Read()!=f.before)return false;return true;
    }
    MmsResult MeasureOwners(resource_budget::Collector& measured)const noexcept{
        for(const auto& model:models)if(auto r=model.owner->MeasureBookkeeping(measured))return r;
        if(samples)for(const auto& accessory:samples->Values().accessories){
            if(auto r=accessory.owner->MeasureBookkeeping(measured))return r;
            if(accessory.parent.owner)if(auto r=accessory.parent.owner->MeasureBookkeeping(measured))return r;
        }
        return measured.Result().Complete()?MMS_OK:MMS_E_NOT_READY;
    }
    bool OwnersCovered(resource_budget::Collector& measured)const noexcept{
        for(const auto& model:models)if(!model.owner->CoversBookkeeping(measured))return false;
        if(samples)for(const auto& accessory:samples->Values().accessories){
            if(!accessory.owner->CoversBookkeeping(measured))return false;
            if(accessory.parent.owner&&!accessory.parent.owner->CoversBookkeeping(measured))return false;
        }
        return measured.Result().Complete();
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<FollowChange> source;const std::uint64_t generation,binding;const int frame;const HWND focus;
        const bool forward;const unsigned char cameraMode,reference,follow;const unsigned slot;
        std::vector<Bytes> expected;bool consumed=false;
        mutable resource_budget::Collector resources;
        std::uint64_t retainedBytes=0;
        Candidate(std::shared_ptr<FollowChange> s,bool f):source(std::move(s)),generation(source->generation),binding(BoundNativePoseGeneration(&source->app)),
            frame(source->app.CurrentFrame()),focus(GetFocus()),forward(f),cameraMode(source->app.CameraMode()),
            reference(static_cast<unsigned char>(source->app.CameraReferenceMode())),follow(source->app.state.followCameraEnabled),slot(source->app.SelectedModelSlot()){
            expected.reserve(source->fields.size());for(const auto& field:source->fields)expected.push_back(field.Read());}
        bool IsCurrent()const noexcept override{
            auto& a=source->app;if(consumed||generation!=source->generation||forward==source->applied||!source->SameScene()||!source->InitialCurrent()||
                !Ready(source->app,generation!=1)||binding!=BoundNativePoseGeneration(&a)||GetFocus()!=focus||a.CurrentFrame()!=frame||
                a.CameraMode()!=cameraMode||a.SelectedModelSlot()!=slot||a.state.followCameraEnabled!=follow||
                static_cast<unsigned char>(a.CameraReferenceMode())!=reference)return false;
            for(unsigned i=0;i<expected.size();++i)if(source->fields[i].Read()!=expected[i])return false;
            return source->OwnersCovered(resources);
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override{out=retainedBytes;return true;}
        void Publish()noexcept override{
            if(!IsCurrent())std::terminate();for(const auto& f:source->fields)f.Write(forward);
            if(source->projectionChanged)source->app.cameraProjectionRefreshPending=true;
            source->applied=forward;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();return Boundary([&]()->MmsResult{
            if(!SameScene()||!InitialCurrent()||forward==applied)return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            if(!Ready(app,generation!=1))return MMS_E_BUSY;
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            if(auto r=MeasureOwners(candidate->resources))return r;
            // One ledger spans old/new parents and every sampled accessory.
            // Repeated holders share their actual allocation identities.
            candidate->retainedBytes=resource_budget::Collector::Sum(RetainedBytes(),candidate->resources.Result().totalBytes);
            if(!candidate->IsCurrent())return MMS_E_STALE;
            out=std::move(candidate);return MMS_OK;});
    }
    std::uint64_t RetainedBytes()const noexcept override{
        // Owner storage is measured afresh by Prepare, whose candidate returns
        // the complete dynamic target to the journal's budget admission.
        std::uint64_t n=sizeof(*this)+4*sizeof(void*);const auto add=[&](std::uint64_t v){n=v>UINT64_MAX-n?UINT64_MAX:n+v;};
        add(fields.capacity()*sizeof(Field));add(models.capacity()*sizeof(Model));add(bones.capacity()*sizeof(Bone));
        if(camera)add(camera->RetainedBytes());if(samples)add(samples->RetainedBytes());return n;
    }
    // Like the original toggle, replay changes current global parameters and
    // presentation, without seeking/settling the model physics pose.
    bool RequiresPhysicsReset()const noexcept override{return false;}
};
void EnableView(FollowChange& c,ViewportPresetState& v){
    v.offsetX=v.offsetY=0;v.suppressed=0;for(unsigned i=0;i<16;++i)v.basis[i]=i%5==0?1.f:0.f;
    if(v.parentModel>=0){auto& api=d3dx::Get();if(!api.Load())throw std::invalid_argument("follow parent CPU math");
        const auto& bone=c.AddBone(unsigned(v.parentModel),v.parentBone);
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)v.basis[row*4+col]=bone.matrix[col*4+row];
        v.offsetX=v.cameraPosition[0];v.offsetY=v.cameraPosition[1];v.distance=v.cameraPosition[2];}
}
void DisableView(FollowChange& c,const ViewportPresetState& before,ViewportPresetState& after){
    const auto old=c.Focus(before,c.follow!=0),next=c.Focus(after,false);auto& api=d3dx::Get();
    if(!api.Load()||!api.rotX||!api.rotY||!api.rotZ||!api.multiply||!api.translation)throw std::invalid_argument("follow CPU math");
    d3dx::D3DXMATRIXF y{},r{},z{},a{},b{};api.rotY(&y,after.rotation[1]);api.rotX(&r,after.rotation[0]);api.multiply(&r,&y,&r);
    api.rotZ(&z,after.rotation[2]);api.multiply(&r,&r,&z);api.translation(&a,-old[0],-old[1],-old[2]);api.translation(&b,-next[0],-next[1],-next[2]);
    api.multiply(&a,&a,&r);api.multiply(&b,&b,&r);after.offsetX-=a.m[3][0]-b.m[3][0];after.offsetY-=a.m[3][1]-b.m[3][1];after.distance-=a.m[3][2]-b.m[3][2];
    if(!c.cameraMode&&after.parentModel>=0&&unsigned(after.parentModel)==c.slot)after.suppressed=0;
}
}
MmsResult PlanCameraFollow(MMDApp& app,bool enabled,std::shared_ptr<HistoryChange>& out) noexcept{
    out.reset();if(!Ready(app))return MMS_E_BUSY;if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(!mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{
        auto change=std::make_shared<FollowChange>(app);const auto before=View(app);auto after=before;Finite(before);
        if(enabled){if(auto r=CaptureViewportResetCamera(app,change->camera))return r;
            if(!change->camera->Sample(after))return MMS_E_ARGUMENT;
            if(auto r=CaptureCameraFollowSamples(app,change->samples))return r;
            EnableView(*change,after);change->TrackFields();}
        else DisableView(*change,before,after);
        Finite(after);change->ViewFields(after);change->Add(app.state.followCameraEnabled,enabled?1:0);
        if(!change->InitialCurrent())return MMS_E_STALE;
        if(std::none_of(change->fields.begin(),change->fields.end(),[](const Field& f){return f.changed;}))return MMS_OK;
        out=std::move(change);return MMS_OK;});
}
bool TrySetBoundCameraFollow(MMDApp* app,bool enabled) noexcept{
    if(!app)return false;auto* history=BoundNativePoseHistory(app);if(!history)return false;
    bool changed=false;
    const auto status=Boundary([&]()->MmsResult{
        if(auto r=history->CheckNativeEdit())return r;
        ScopedFollowReadoutDrafts drafts(*app);
        std::shared_ptr<HistoryChange> change;if(auto r=PlanCameraFollow(*app,enabled,change))return r;
        if(change){std::unique_ptr<SharedHistory::NativeTransaction> token;
            if(auto r=history->PrepareNative("Follow camera",change,token,SharedHistory::NativeIntent::Adjustment))return r;
            if(auto r=history->CommitNative(std::move(token)))return r;changed=true;}
        bake::ScopedUiInputGate input;
        if(changed)PublishBoundPoseEdit(*app,*history,false);
        RefreshBoundGlobalReadout(*app);return MMS_OK;});
    if(status)runtime_log::Write("controlled follow camera refused: %d",int(status));
    // A Win32 checkbox has already moved before BN_CLICKED. Even a refused
    // preparation restores its actual state; no legacy writer may run next.
    if(status)RefreshBoundCameraFollowFlags(*app);return true;
}
}
#endif
