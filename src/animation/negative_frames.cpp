#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/morph_display_rows.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/frame_scope.hpp"
#include "mikumikustudio/clipboard_layout.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_motion_snapshot.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "negative_key_sampling.hpp"
#include "negative_key_data.hpp"
#include "negative_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "negative_copy_source.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "../plugins/native_global_key_delete.hpp"
#include "../plugins/native_model_keys.hpp"
#include "../plugins/native_key_commands.hpp"
#include "../plugins/native_object_commands.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#endif
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "../plugins/native_pose_scope.hpp"
#include "../plugins/native_history_commands.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <set>
#include <tuple>
#include <type_traits>
#include <limits>
#include <unordered_map>

namespace mikumikustudio {
void RefreshAfterFrameApply(MMDApp*);
void RegisterCameraState(MMDApp*,int);void RegisterLightState(MMDApp*,int);
void RegisterSelfShadowState(MMDApp*,int);void RegisterGravityKeyCurrent(MMDApp*,std::int32_t);
void RegisterAccessoryKey(MMDApp*,int,int);
}

namespace mikumikustudio::negative_frames {
namespace {
using Values=std::array<double,16>;
using Key=detail::Key;
sampling::Family GlobalFamily(int kind) noexcept {
    return kind==2?sampling::Family::Camera:kind==3?sampling::Family::Light:
        kind==4?sampling::Family::Shadow:kind==5?sampling::Family::Gravity:sampling::Family::Accessory;
}
bool InActiveTimeline(MMDApp& app,const Key& key) noexcept {
    return app.CameraMode()?!key.model:key.model && key.model==app.SelectedModel();
}
struct State {
    std::shared_ptr<const char> lifetime=std::make_shared<const char>(0);
    std::vector<Key> keys,clipboard;
    struct Snapshot {std::vector<Key> keys;const unsigned char* model=nullptr;int nativeCursor=0;bool drag=false;};
    std::vector<Snapshot> undo,redo;
    std::vector<std::pair<std::size_t,int>> dragging;
    std::int64_t dragMin=0,dragMax=0;
    int dragOffset=0;
    bool dragHistory=false;
    // Bumped by every insert/remove/replace of the key vector. BeginDrag stores
    // the value it planned against; Drag refuses stale positions once they no
    // longer describe that vector.
    std::uint64_t keysGeneration=0,dragGeneration=0;
};
std::unordered_map<const MMDApp*,State> states;
bool Current(const MMDApp& app,const Key& key) {
    if(!key.model) {
        if(key.target>=1000 && key.target<1255)return app.AccessorySlot(key.target-1000)==key.identity && key.identity;
        return key.identity==&app && key.target>=2 && key.target<=5;
    }
    for(int slot=0;slot<255;++slot)if(app.ModelSlot(slot)==key.model) {
        const auto* record=mdl::Mdl(key.model);
        if(record && key.identity==record->boneTable && key.target<0) {
            if(key.target==-1)return true;
            if(key.target<=-1000000)return std::int64_t(-1000000)-key.target<record->boneOrderCount;
            return std::int64_t(-2)-key.target<record->ikChainCount;
        }
        return record && key.identity==(key.morph ? static_cast<const void*>(record->morphs) : record->boneTable) &&
            key.target>=0 && key.target<int(key.morph ? record->morphCount : record->boneCount);
    }
    return false;
}
MMDApp* Owner(unsigned char* model) {
    if(!g_Block || !model)return nullptr;
    for(int slot=0;slot<255;++slot)if(g_Block->ModelSlot(slot)==model)return g_Block;
    return nullptr;
}
State::Snapshot Snapshot(const State& state) {
    const auto* model=g_Block && !g_Block->CameraMode() ? g_Block->SelectedModel() : nullptr;
    return {state.keys,model,model ? int(mdl::Mdl(model)->undoState[0]) : 0};
}
void History(State& state) {
    state.undo.push_back(Snapshot(state));
    if(state.undo.size()>30)state.undo.erase(state.undo.begin());
    state.redo.clear();
}
// End the private drag transaction. The pending snapshot is removed only when
// it still sits on top and made no net change; an older drag record the user
// can still undo is never popped.
void AbortDrag(State& state) {
    if(state.dragHistory && state.dragOffset==0 && !state.undo.empty() && state.undo.back().drag)state.undo.pop_back();
    state.dragging.clear();state.dragHistory=false;state.dragOffset=0;state.dragGeneration=0;
}
Values Pose(unsigned char* model,int target,bool morph) {
    Values values{};auto& record=*mdl::Mdl(model);
    if(target<0) {
        if(target==-1)values[0]=record.loadComplete;
        else if(target<=-1000000) {
            const auto& reference=mdl::BoneOrder(model)[-1000000-target];
            values[0]=double(reference.linkedModel);values[1]=double(reference.linkedBone);
        } else values[0]=mdl::IkChains(model)[-2-target].enabled;
        return values;
    }
    if(morph)values[0]=record.morphs[target].value;
    else {
        const auto& bone=record.boneTable[target];
        std::copy_n(bone.trans,3,values.begin());
        std::copy_n(bone.rotQuat,4,values.begin()+3);
        const auto scale=bone_scale::Pose(model,target);
        std::copy(scale.v.begin(),scale.v.end(),values.begin()+7);
    }
    return values;
}
void Apply(unsigned char* model,int target,bool morph,const Values& values) {
    auto& record=*mdl::Mdl(model);
    if(target<0) {
        if(target==-1)record.loadComplete=values[0]!=0;
        else if(target<=-1000000) {
            auto& reference=mdl::BoneOrder(model)[-1000000-target];
            reference.linkedModel=int(values[0]);reference.linkedBone=int(values[1]);
        } else mdl::IkChains(model)[-2-target].enabled=values[0]!=0;
        return;
    }
    if(morph)record.morphs[target].value=values[0];
    else {
        auto& bone=record.boneTable[target];
        std::copy_n(values.begin(),3,bone.trans);
        std::copy_n(values.begin()+3,4,bone.rotQuat);
        bone_scale::Matrix scale;std::copy_n(values.begin()+7,9,scale.v.begin());
        bone_scale::SetPose(model,target,scale);
    }
}
bool Valid(const Values& values,bool morph) {
    for(double value:values)if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return false;
    if(morph)return true;
    double norm=0;for(int i=3;i<7;++i)norm+=double(values[i])*values[i];
    bone_scale::Matrix scale;std::copy_n(values.begin()+7,9,scale.v.begin());
    return norm>1e-20 && bone_scale::Valid(scale);
}
bool Integer(double value,double first,double last) {
    return std::isfinite(value) && value>=first && value<=last && std::trunc(value)==value;
}
bool GlobalValid(const Values& values,int kind) {
    for(double value:values)if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return false;
    if(kind==2)return Integer(values[8],0,1) && Integer(values[9],-1,254) && Integer(values[10],-1,frame_limits::maximum) && values[7]>=0 && values[7]<=180;
    if(kind==3)return true;
    if(kind==4)return Integer(values[0],0,2);
    if(kind==5)return Integer(values[4],std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()) && Integer(values[5],0,1);
    return kind>=1000 && kind<1255 && Integer(values[8],0,1) && Integer(values[9],0,1) &&
        Integer(values[10],-1,254) && Integer(values[11],-1,frame_limits::maximum);
}
void Upsert(State& state,const Key& key) {
    // Every key insertion goes through here; end a live drag before the vector
    // it planned against changes shape.
    AbortDrag(state);
    auto found=std::find_if(state.keys.begin(),state.keys.end(),[&](const Key& existing) {
        return existing.model==key.model && existing.identity==key.identity && existing.morph==key.morph &&
            existing.target==key.target && existing.frame==key.frame;
    });
    if(found==state.keys.end())state.keys.push_back(key);else *found=key;
    // Any insertion or replacement invalidates press-time indices and key
    // identity; the next Drag drops its stale plan instead of writing through
    // it.
    ++state.keysGeneration;
}
int Row(const MMDApp& app,const Key& key) {
    if(!key.model) {
        if(key.target>=1000) {
            for(int band=0;band<200 && app.state.jointLineMap[band]>=0;++band)
                if(app.state.jointLineMap[band]==key.target-1000)return 4+band;
            return -1;
        }
        return key.target-2;
    }
    if(key.target<0)return 0;
    const auto& record=*mdl::Mdl(key.model);
    const auto* rows=key.morph ? record.morphKeyIndices : record.boneKeyIndices;
    if(!rows)return 0;
    return std::abs(reinterpret_cast<const std::int32_t*>(rows)[key.target]);
}
template<class Function>
void ForEachRow(const MMDApp& app,const Key& key,
                const morph_display::Rows& morphRows,Function function) {
    if(key.model && key.morph && key.target>=0) {
        morphRows.ForEach(key.target,[&](int link){function(link<0 ? -link : link);});
    } else function(Row(app,key));
}
template<class Predicate>
bool AnyRow(const MMDApp& app,const Key& key,
            const morph_display::Rows& morphRows,Predicate predicate) {
    bool hit=false;
    ForEachRow(app,key,morphRows,[&](int row){hit=hit || predicate(row);});
    return hit;
}
Values GlobalPose(MMDApp* app,int kind) {
    Values values{};
    if(kind>=1000) {
        const auto* object=app->AccessorySlot(kind-1000);if(!object)return values;
        for(int i=0;i<3;++i){values[i]=object->position[i];values[3+i]=object->rotation[i];}
        values[6]=object->scale;values[7]=object->opacity;values[8]=object->visible;values[9]=object->shadowEnabled;
        values[10]=double(object->parentModel);values[11]=double(object->parentBone);
    } else if(kind==2) {
        for(int i=0;i<3;++i){values[i]=app->CameraPositionAxis(i);values[3+i]=app->CameraRotation()[i];}
        values[6]=app->CameraDistance();values[7]=app->CameraFov();values[8]=app->CameraPerspective();
        values[9]=app->CameraParentModel();values[10]=app->CameraParentBone();
    } else if(kind==3)for(int i=0;i<3;++i){values[i]=app->LightDirection()[i];values[3+i]=app->LightColor()[i];}
    else if(kind==4){values[0]=float(app->SelfShadowMode());values[1]=app->ShadowDistance();}
    else {values[0]=app->GravityMagnitude();for(int i=0;i<3;++i)values[1+i]=app->GravityDirection()[i];
        values[4]=app->GravityNoise();values[5]=float(app->GravityNoiseEnabled());}
    return values;
}
void ApplyGlobal(MMDApp* app,int kind,const Values& values) {
    if(kind>=1000) {
        auto* object=app->AccessorySlot(kind-1000);if(!object)return;
        for(int i=0;i<3;++i){object->position[i]=values[i];object->rotation[i]=values[3+i];}
        object->scale=values[6];object->opacity=values[7];object->visible=values[8]!=0;object->shadowEnabled=values[9]!=0;
        object->parentModel=int(values[10]);object->parentBone=int(values[11]);
    } else if(kind==2) {
        for(int i=0;i<3;++i){app->CameraPositionAxis(i)=values[i];app->CameraRotation()[i]=values[3+i];}
        app->CameraDistance()=values[6];app->CameraFov()=float(values[7]);app->CameraPerspective()=values[8]!=0;
        app->CameraParentModel()=int(values[9]);app->CameraParentBone()=int(values[10]);
    } else if(kind==3){
        for(int i=0;i<3;++i){app->LightDirection()[i]=values[i];app->LightColor()[i]=values[3+i];}
        app->ApplyTimelineLightState();
    }
    else if(kind==4){app->SelfShadowMode()=int(values[0]);app->ShadowDistance()=values[1];}
    else {app->GravityMagnitude()=values[0];for(int i=0;i<3;++i)app->GravityDirection()[i]=values[1+i];
        app->GravityNoise()=int(values[4]);app->GravityNoiseEnabled()=values[5]!=0;}
}
Values GlobalZero(MMDApp* app,int kind) {
    auto values=GlobalPose(app,kind);
    if(kind>=1000 && app->AccessoryKeys(kind-1000)) {
        const auto& key=app->AccessoryKeys(kind-1000)[0];
        for(int i=0;i<3;++i){values[i]=key.position[i];values[3+i]=key.rotation[i];}
        values[6]=key.scale;values[7]=key.opacity;values[8]=key.visible;values[9]=key.shadowEnabled;
        values[10]=double(key.parentModel);values[11]=double(key.parentBone);
    } else if(kind==2 && app->CameraKeys()) {
        const auto& key=app->CameraKeys()[0];
        for(int i=0;i<3;++i){values[i]=key.eye[i];values[3+i]=key.target[i];}
        values[6]=key.distance;values[7]=float(key.fov);values[8]=key.perspective;
        values[9]=double(key.parentModel);values[10]=double(key.parentBone);
    } else if(kind==3 && app->LightKeys()) {
        const auto& key=app->LightKeys()[0];for(int i=0;i<3;++i){values[i]=key.direction[i];values[3+i]=key.color[i];}
    } else if(kind==4 && app->ShadowKeys()){values[0]=app->ShadowKeys()[0].mode;values[1]=app->ShadowKeys()[0].distance;}
    else if(kind==5 && app->GravityKeys()) {
        const auto& key=app->GravityKeys()[0];values[0]=key.acceleration;
        for(int i=0;i<3;++i)values[1+i]=key.direction[i];values[4]=double(key.noise);values[5]=key.noiseEnabled;
    }
    return values;
}
void Changed(MMDApp* app) {app->SceneModified()=1;if(app->MainWindow()){PanelPaint(app);PostViewRefresh(app);RefreshControls(app);}}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value) {
    for(int i=0;i<4;++i)bytes.push_back(std::uint8_t(value>>(8*i)));
}
bool SameAuthoredKey(const Key& a,const Key& b) noexcept {
    // Selection and clipboard offsets are UI state, not authored key data.
    return detail::SameKeyData(a,b);
}
bool SameKeys(const std::vector<Key>& left,const std::vector<Key>& right) noexcept {
    if(left.size()!=right.size())return false;
    for(std::size_t i=0;i<left.size();++i) {
        const auto& a=left[i];const auto& b=right[i];
        if(!SameAuthoredKey(a,b))return false;
    }
    return true;
}
#if !MMD_ORIGINAL_PROFILE
// A retained scene object can be absent while unrelated tracks are edited.
// Its dormant keys must survive unchanged, but cannot themselves be edited.
// Comparing the dormant subsequences also handles active inserts/removals
// without allocating or dereferencing an absent object's storage.
bool SameDormantKeys(const MMDApp& app,const std::vector<Key>& a,const std::vector<Key>& b) noexcept {
    std::size_t i=0,j=0;
    for(;;) {
        while(i<a.size() && Current(app,a[i]))++i;
        while(j<b.size() && Current(app,b[j]))++j;
        if(i==a.size() || j==b.size())return i==a.size() && j==b.size();
        if(!SameAuthoredKey(a[i],b[j]))return false;
        ++i;++j;
    }
}
const void* GlobalPool(MMDApp* app,int kind) noexcept {
    if(kind==2)return app->CameraKeys();if(kind==3)return app->LightKeys();
    if(kind==4)return app->ShadowKeys();if(kind==5)return app->GravityKeys();
    return kind>=1000&&kind<1255?app->AccessoryKeys(kind-1000):nullptr;
}
struct GlobalPosePublication {
    MMDApp* app;int kind,frame;const void* identity;const void* pool;
    Values before,after,zero;
    bool IsCurrent()const noexcept {
        if(app->CurrentFrame()!=frame || GlobalPool(app,kind)!=pool)return false;
        if(kind>=1000 && app->AccessorySlot(kind-1000)!=identity)return false;
        return GlobalPose(app,kind)==before && GlobalZero(app,kind)==zero;
    }
    void Publish()const noexcept {ApplyGlobal(app,kind,after);}
};
bool SameChannel(const Key& a,const Key& b) noexcept {
    return a.model==b.model&&a.identity==b.identity&&a.target==b.target&&a.morph==b.morph;
}
const Key* TargetZero(const std::vector<Key>& zeros,const Key& key) noexcept {
    const auto found=std::find_if(zeros.begin(),zeros.end(),[&](const Key& value){return SameChannel(value,key);});
    return found==zeros.end()?nullptr:&*found;
}
std::vector<Key> CaptureDeletionZeros(MMDApp& app,const DeletionTargets& targets,std::uint64_t& budget) {
    if((targets.globals&&!targets.globals->IsCurrent())||(targets.model&&!targets.model->IsCurrent()))
        throw std::invalid_argument("stale positive deletion target");
    std::size_t count=targets.globals?targets.globals->ZeroTargets().size():0;
    if(targets.model) {
        if(targets.modelSlot>=kModelSlotCount||!app.ModelSlot(targets.modelSlot)||targets.model->ModelObject()!=app.ModelSlot(targets.modelSlot))
            throw std::invalid_argument("negative deletion target model mismatch");
        count+=targets.model->BoneZeroTargets().size()+targets.model->MorphZeroTargets().size();
        if(const auto* display=targets.model->DisplayZeroTarget())count+=1+display->ik.size()+display->selectors.size();
    }
    if(count>budget/(2*sizeof(Key)))throw std::length_error("negative deletion zero target budget");
    budget-=count*2*sizeof(Key);std::vector<Key> result;result.reserve(count);
    const auto add=[&](Key key) {
        if(!Current(app,key)||TargetZero(result,key))throw std::invalid_argument("invalid duplicate deletion zero target");
        if(key.model?(key.target>=0&&!Valid(key.values,key.morph)):!GlobalValid(key.values,key.target))
            throw std::invalid_argument("invalid deletion zero values");
        result.push_back(std::move(key));
    };
    if(targets.globals)for(const auto& target:targets.globals->ZeroTargets()) {
        using Kind=curve_host::Kind;Key key;key.identity=&app;
        switch(target.kind) {
        case Kind::Camera:key.target=2;break;case Kind::Light:key.target=3;break;
        case Kind::Shadow:key.target=4;break;case Kind::Gravity:key.target=5;break;
        case Kind::Accessory:
            if(target.slot>=255)throw std::invalid_argument("negative accessory target slot");
            key.target=1000+int(target.slot);key.identity=target.object;break;
        default:throw std::invalid_argument("negative global target kind");
        }
        std::copy(target.zeroPose.begin(),target.zeroPose.end(),key.values.begin());
        // Negative keys own the enabled byte, not the native gravity word's
        // upper reserved bytes, which remain owned by the positive participant.
        if(key.target==5) {
            if(!Integer(key.values[5],0,UINT32_MAX))throw std::invalid_argument("negative gravity target word");
            key.values[5]=std::uint32_t(key.values[5])&0xffu;
        }
        add(std::move(key));
    }
    if(targets.model) {
        auto* model=app.ModelSlot(targets.modelSlot);const auto& r=*mdl::Mdl(model);
        const auto make=[&](int index,bool morph) {Key key;key.model=model;key.identity=morph?static_cast<const void*>(r.morphs):r.boneTable;key.target=index;key.morph=morph;return key;};
        for(const auto& target:targets.model->BoneZeroTargets()) {
            if(target.bone>=r.boneCount||!target.zeroPose.writePose)throw std::invalid_argument("negative bone target");
            auto key=make(int(target.bone),false);
            std::copy(target.zeroPose.position.begin(),target.zeroPose.position.end(),key.values.begin());
            std::copy(target.zeroPose.rotation.begin(),target.zeroPose.rotation.end(),key.values.begin()+3);
            std::copy(target.zeroPose.scale.v.begin(),target.zeroPose.scale.v.end(),key.values.begin()+7);add(std::move(key));
        }
        for(const auto& target:targets.model->MorphZeroTargets()) {
            if(target.morph>=r.morphCount)throw std::invalid_argument("negative morph target");
            auto key=make(int(target.morph),true);key.values[0]=target.zeroWeight;add(std::move(key));
        }
        if(const auto* display=targets.model->DisplayZeroTarget()) {
            if(display->ik.size()!=r.ikChainCount||display->selectors.size()!=r.boneOrderCount)
                throw std::invalid_argument("negative display target extent");
            auto key=make(-1,false);key.values[0]=display->visible;add(std::move(key));
            for(unsigned i=0;i<r.ikChainCount;++i){auto value=make(-2-int(i),false);value.values[0]=display->ik[i];add(std::move(value));}
            for(unsigned i=0;i<r.boneOrderCount;++i){auto value=make(-1000000-int(i),false);value.values[0]=display->selectors[i].linkedModel;value.values[1]=display->selectors[i].linkedBone;add(std::move(value));}
        }
    }
    return result;
}
bool SameGlobalTrack(const std::vector<Key>& a,const std::vector<Key>& b,int kind,const void* identity) noexcept {
    std::size_t i=0,j=0;
    for(;;) {
        while(i<a.size()&&(a[i].model||a[i].target!=kind||a[i].identity!=identity))++i;
        while(j<b.size()&&(b[j].model||b[j].target!=kind||b[j].identity!=identity))++j;
        if(i==a.size()||j==b.size())return i==a.size()&&j==b.size();
        if(!SameAuthoredKey(a[i],b[j]))return false;
        ++i;++j;
    }
}
std::vector<GlobalPosePublication> PrepareGlobalPoses(MMDApp* app,const std::vector<Key>& source,const std::vector<Key>& target,
    const std::vector<Key>& zeros,std::uint64_t& budget) {
    std::vector<GlobalPosePublication> result;
    if(app->CurrentFrame()>=0)return result;
    for(const auto* list:{&source,&target,&zeros})for(const auto& changed:*list) {
        if(changed.model || !Current(*app,changed) || (!TargetZero(zeros,changed)&&SameGlobalTrack(source,target,changed.target,changed.identity)))continue;
        if(std::any_of(result.begin(),result.end(),[&](const auto& pose){return pose.kind==changed.target&&pose.identity==changed.identity;}))continue;
        const int kind=changed.target,frame=app->CurrentFrame();
        const auto* pool=GlobalPool(app,kind);
        if(!pool)throw std::invalid_argument("negative history requires a native zero-frame baseline");
        const auto sourceZero=GlobalZero(app,kind);
        const auto* replacement=TargetZero(zeros,changed);
        const auto zero=replacement?replacement->values:sourceZero;
        if(!GlobalValid(zero,kind))throw std::invalid_argument("invalid negative history baseline");
        for(const auto& key:target)if(!key.model&&key.target==kind&&key.identity==changed.identity&&Current(*app,key)) {
            if(!GlobalValid(key.values,kind))throw std::invalid_argument("invalid negative history values");
        }
        Key zeroKey;zeroKey.values=zero;if(replacement)zeroKey.author=replacement->author;
        sampling::Result sampled;
        if(!sampling::Track(target,GlobalFamily(kind),frame,zeroKey,sampled,[&](const Key& key){
            return !key.model&&key.target==kind&&key.identity==changed.identity&&Current(*app,key);}))
            throw std::invalid_argument("invalid negative history author sample");
        const auto& after=sampled.values;
        if(!GlobalValid(after,kind))throw std::invalid_argument("invalid negative history sample");
        if(2*sizeof(GlobalPosePublication)>budget)throw std::length_error("negative global pose budget");budget-=2*sizeof(GlobalPosePublication);
        result.push_back({app,kind,frame,changed.identity,pool,GlobalPose(app,kind),after,sourceZero});
    }
    return result;
}
struct PhysicsFlags {
    MMDApp* app;unsigned char* model;const void* table;unsigned char* flags;
    std::vector<std::pair<unsigned,std::uint8_t>> before;
    bool Current(bool forward)const noexcept {
        bool live=false;for(int slot=0;slot<255;++slot)live|=app->ModelSlot(slot)==model;
        if(!live)return false;const auto& record=*mdl::Mdl(model);
        if(record.boneTable!=table || record.bonePhysicsState!=flags)return false;
        for(const auto& item:before)if(item.first>=record.boneCount || flags[item.first]!=(forward?item.second:0))return false;
        return true;
    }
    void Publish(bool forward)const noexcept {for(const auto& item:before)flags[item.first]=forward?0:item.second;}
};
// Frozen native sources and owned pose drafts live only during preparation.
// Publication exchanges scale storage before writing local channels; it never
// calls a seek, solver, SetPose, or an allocating registration API.
struct FrozenPool {
    const void* pointer=nullptr;std::size_t count=0,width=0;
    std::vector<unsigned char> bytes;
    void Capture(const void* data,std::size_t capacity,std::size_t stride) {
        if(!data || !capacity)throw std::invalid_argument("negative pose baseline pool missing");
        if(capacity>(64ull*1024*1024)/stride)throw std::length_error("negative pose source budget");
        pointer=data;count=capacity;width=stride;
        const auto* begin=static_cast<const unsigned char*>(data);bytes.assign(begin,begin+capacity*stride);
    }
    bool Current(const void* data,std::size_t capacity)const noexcept {
        return data==pointer && capacity==count && !std::memcmp(data,bytes.data(),bytes.size());
    }
};
struct ModelPosePublication {
    struct Channel {Key key;Values before,after;unsigned char dirty=0,targetDirty=0;bool needsZero=false;};
    MMDApp* app=nullptr;unsigned char* model=nullptr;
    mdl::BoneRecord* bones=nullptr;mdl::MorphRecord* morphs=nullptr;
    unsigned boneCount=0,morphCount=0;unsigned char* flags=nullptr;
    std::vector<Channel> channels;
    std::vector<mdl::BoneOrderEntry> targetSelectors;
    FrozenPool bonePool,morphPool,displayPool,orderPool,ikPool;
    std::vector<FrozenPool> displayNested;
    std::size_t orderCount=0,ikCount=0;
    bool displaySource=false;
    std::vector<std::pair<mdl::BoneType,int>> metadata;
    bone_scale::Values beforeScales,afterScales;
    std::unique_ptr<bone_scale::PoseStorageLease> lease;
    std::unique_ptr<bone_scale::AuthoringRestore> scaleGuard;
    std::unique_ptr<bone_curve::AuthoringRestore> curveGuard;
    morph_curve::AuthoredLayers layers;
    bool hasBones=false,hasMorphs=false,zeroBones=false,zeroMorphs=false,zeroDisplay=false;
    bool IsCurrent()const noexcept {
        for(const auto& c:channels)if(!Current(*app,c.key))return false;
        const auto& r=*mdl::Mdl(model);
        if(r.boneTable!=bones || r.morphs!=morphs || r.boneCount!=boneCount || r.morphCount!=morphCount)return false;
        if(hasBones) {
            if(r.bonePhysicsState!=flags || !lease->IsCurrent() || !bone_scale::PoseValuesEqual(model,beforeScales))return false;
            if(zeroBones && (!bonePool.Current(r.boneKeys,mdl::BoneKeyCapacity(r)) || !scaleGuard->IsCurrent() || !curveGuard->IsCurrent()))return false;
        }
        if(hasBones || displaySource)for(unsigned i=0;i<boneCount;++i)if(r.boneTable[i].type!=metadata[i].first || r.boneTable[i].slotIndex!=metadata[i].second)return false;
        if(displaySource) {
            if(r.boneOrderCount!=orderCount || r.ikChainCount!=ikCount || !displayPool.Current(r.displayKeys,mdl::KeyPoolCapacity(r.displayKeys)) ||
                (orderCount && !orderPool.Current(r.boneOrderTable,orderCount)) || (ikCount && !ikPool.Current(r.ikChains,ikCount)))return false;
            for(const auto& nested:displayNested)if(!nested.Current(nested.pointer,nested.count))return false;
        }
        if(zeroMorphs) {
            if(!morphPool.Current(r.morphKeys,mdl::MorphKeyCapacity(r)))return false;
            try {if(!morph_curve::LayersUnchanged(model,layers))return false;}catch(...){return false;}
        }
        for(const auto& c:channels) {
            if(Pose(model,c.key.target,c.key.morph)!=c.before)return false;
            if(!c.key.morph && c.key.target>=0 && flags && flags[c.key.target]!=c.dirty)return false;
        }
        return true;
    }
    void Publish()noexcept {
        if(hasBones)bone_scale::ExchangePoseValues(model,afterScales);
        auto& r=*mdl::Mdl(model);
        // Native negative seek first samples registered frame zero, then
        // overlays negative parent references. Preserve that window baseline.
        if(!targetSelectors.empty())std::copy(targetSelectors.begin(),targetSelectors.end(),mdl::BoneOrder(model));
        for(const auto& c:channels) {
            if(c.key.morph)r.morphs[c.key.target].value=float(c.after[0]);
            else if(c.key.target<0)Apply(model,c.key.target,false,c.after);
            else {
                auto& b=r.boneTable[c.key.target];
                std::copy_n(c.after.begin(),3,b.trans);
                std::copy_n(c.after.begin()+3,4,b.rotQuat);
            }
        }
    }
};
bool SameModelTrack(const std::vector<Key>& a,const std::vector<Key>& b,const Key& changed) {
    const auto matches=[&](const Key& k){return k.model==changed.model && k.identity==changed.identity && k.target==changed.target && k.morph==changed.morph;};
    std::size_t n=0,m=0;
    for(const auto& k:a)if(matches(k)) {
        ++n;
        const auto found=std::find_if(b.begin(),b.end(),[&](const Key& other){return matches(other)&&other.frame==k.frame;});
        if(found==b.end() || !SameAuthoredKey(*found,k))return false;
    }
    for(const auto& k:b)if(matches(k))++m;
    return n==m;
}
std::vector<ModelPosePublication> PrepareModelPoses(MMDApp* app,const std::vector<Key>& source,
    const std::vector<Key>& target,const PhysicsFlags* physics,bool forward,const std::vector<Key>& zeros,std::uint64_t& budget,
    const DeletionTargets* deletionTargets) {
    std::vector<ModelPosePublication> result;
    const auto charge=[&](std::uint64_t bytes){if(bytes>budget)throw std::length_error("negative pose transaction budget");budget-=bytes;};
    const auto chargeCopies=[&](std::uint64_t bytes,unsigned copies){if(bytes>budget/copies)throw std::length_error("negative pose source copies budget");charge(bytes*copies);};
    if(app->CurrentFrame()>=0)return result;
    for(const auto* list:{&source,&target,&zeros})for(const auto& changed:*list) {
        if(!changed.model || (!TargetZero(zeros,changed)&&SameModelTrack(source,target,changed)))continue;
        if(!Current(*app,changed))throw std::invalid_argument("negative target model is absent");
        auto pose=std::find_if(result.begin(),result.end(),[&](const auto& p){return p.model==changed.model;});
        if(pose==result.end()) {
            charge(2*sizeof(ModelPosePublication));
            result.emplace_back();pose=std::prev(result.end());pose->app=app;pose->model=changed.model;
            const auto& r=*mdl::Mdl(changed.model);pose->bones=r.boneTable;pose->morphs=r.morphs;
            pose->boneCount=r.boneCount;pose->morphCount=r.morphCount;pose->flags=r.bonePhysicsState;
        }
        if(std::any_of(pose->channels.begin(),pose->channels.end(),[&](const auto& c){return c.key.target==changed.target&&c.key.morph==changed.morph;}))continue;
        ModelPosePublication::Channel c;c.key=changed;c.before=Pose(changed.model,changed.target,changed.morph);
        c.after=c.before;c.dirty=!changed.morph && changed.target>=0 && pose->flags?pose->flags[changed.target]:0;
        charge(2*sizeof(ModelPosePublication::Channel));
        pose->channels.push_back(std::move(c));
        pose->hasBones|=!changed.morph&&changed.target>=0;pose->hasMorphs|=changed.morph;
    }
    for(auto& p:result) {
        const auto& r=*mdl::Mdl(p.model);
        if(deletionTargets&&deletionTargets->model&&deletionTargets->model->ModelObject()==p.model)
            if(const auto* display=deletionTargets->model->DisplayZeroTarget()) {
                chargeCopies(display->selectors.size()*sizeof(mdl::BoneOrderEntry),2);
                p.targetSelectors=display->selectors;
            }
        std::unique_ptr<BoneMotionSnapshot> bones;
        std::unique_ptr<morph_curve::SamplingState> morphs;
        for(auto& c:p.channels) {
            c.targetDirty=c.dirty;
            if(!c.key.morph && c.key.target>=0 && physics && physics->model==p.model)for(const auto& flag:physics->before)
                if(flag.first==unsigned(c.key.target)){c.targetDirty=forward?0:flag.second;break;}
            const auto keyAtOrAfter=std::any_of(target.begin(),target.end(),[&](const Key& k){
                return k.model==p.model&&k.identity==c.key.identity&&k.target==c.key.target&&k.morph==c.key.morph&&k.frame>=app->CurrentFrame();});
            const auto anyKey=std::any_of(target.begin(),target.end(),[&](const Key& k){
                return k.model==p.model&&k.identity==c.key.identity&&k.target==c.key.target&&k.morph==c.key.morph;});
            c.needsZero=c.key.target<0?!anyKey:!keyAtOrAfter;
            if(c.key.morph)p.zeroMorphs|=c.needsZero;else if(c.key.target<0)p.zeroDisplay|=c.needsZero;else p.zeroBones|=c.needsZero&&!c.targetDirty;
        }
        if(p.hasBones) {
            // Charge owned pose maps conservatively before allocating their nodes.
            charge(std::uint64_t(r.boneCount)*384+sizeof(ModelPosePublication));
            p.lease=bone_scale::PoseStorageLease::Prepare(p.model);
            charge(p.lease->RetainedBytes());
            p.beforeScales=bone_scale::PoseValues(p.model);p.afterScales=p.beforeScales;
            for(unsigned i=0;i<r.boneCount;++i)p.metadata.emplace_back(r.boneTable[i].type,r.boneTable[i].slotIndex);
        }
        if(p.zeroBones) {
            charge(std::uint64_t(mdl::BoneKeyCapacity(r))*sizeof(mdl::BoneKey));
            p.bonePool.Capture(r.boneKeys,mdl::BoneKeyCapacity(r),sizeof(mdl::BoneKey));
            bones=BoneMotionSnapshot::Capture(p.model,budget);charge(bones->RetainedBytes());
            const auto scale=bone_scale::AuthoringState::Capture(p.model);chargeCopies(scale.RetainedBytes(),3);
            p.scaleGuard=bone_scale::AuthoringRestore::Prepare(p.model,scale,mdl::BoneKeyCapacity(r));
            const auto curves=bone_curve::AuthoringState::Capture(p.model);chargeCopies(curves->RetainedBytes(),3);
            p.curveGuard=bone_curve::AuthoringRestore::Prepare(p.model,*curves);
        }
        if(p.zeroMorphs) {
            charge(std::uint64_t(mdl::MorphKeyCapacity(r))*sizeof(mdl::MorphKey));
            p.morphPool.Capture(r.morphKeys,mdl::MorphKeyCapacity(r),sizeof(mdl::MorphKey));
            morphs=morph_curve::SamplingState::Capture(p.model,budget);charge(morphs->RetainedBytes());
            std::string error;if(!morph_curve::CaptureLayers(p.model,p.layers,error))throw std::invalid_argument(error);
            charge(p.layers.RetainedBytes());
        }
        std::unique_ptr<DisplayMotionSnapshot> display;
        const auto ensureDisplay=[&]() {
            if(display)return;
            display=DisplayMotionSnapshot::Capture(p.model,budget);charge(display->RetainedBytes());
            p.displaySource=true;p.orderCount=r.boneOrderCount;p.ikCount=r.ikChainCount;
            charge(std::uint64_t(mdl::KeyPoolCapacity(r.displayKeys))*sizeof(mdl::DisplayKey));
            p.displayPool.Capture(r.displayKeys,mdl::KeyPoolCapacity(r.displayKeys),sizeof(mdl::DisplayKey));
            if(p.metadata.empty()){charge(std::uint64_t(r.boneCount)*2*sizeof(p.metadata[0]));for(unsigned i=0;i<r.boneCount;++i)p.metadata.emplace_back(r.boneTable[i].type,r.boneTable[i].slotIndex);}
            if(p.orderCount){charge(p.orderCount*sizeof(mdl::BoneOrderEntry));p.orderPool.Capture(r.boneOrderTable,p.orderCount,sizeof(mdl::BoneOrderEntry));}
            if(p.ikCount){charge(p.ikCount*sizeof(mdl::IkChain));p.ikPool.Capture(r.ikChains,p.ikCount,sizeof(mdl::IkChain));}
            // Capture validated reachable payloads; unused native slots may have
            // no nested allocation. The shallow frozen pool guards each pointer.
            // Nested buffers remain owned by this unchanged native display pool
            // under the shared-history mutation gate. These are equality checks,
            // not independent lifetime leases for arbitrary external writes.
            unsigned index=0;
            do {
                const auto& k=r.displayKeys[index];
                if(p.orderCount){charge(p.orderCount*sizeof(mdl::BoneReference));charge(2*sizeof(FrozenPool));p.displayNested.emplace_back();p.displayNested.back().Capture(k.selectorStates,p.orderCount,sizeof(mdl::BoneReference));}
                if(p.ikCount){charge(p.ikCount);charge(2*sizeof(FrozenPool));p.displayNested.emplace_back();p.displayNested.back().Capture(k.ikStates,p.ikCount,1);}
                index=k.next;
            }while(index);
        };
        if(p.zeroDisplay||!p.targetSelectors.empty())ensureDisplay();
        for(auto& c:p.channels) {
            // Keep unregistered drafts unless this transaction clears their flag.
            if(!c.key.morph && c.key.target>=0 && c.targetDirty)continue;
            Values zero{};
            const auto* replacement=TargetZero(zeros,c.key);
            if(c.needsZero && replacement)zero=replacement->values;
            else if(c.needsZero && c.key.morph) {
                float value=0;if(!morphs->Sample(c.key.target,0,value))throw std::invalid_argument("negative morph baseline sample");zero[0]=value;
            } else if(c.needsZero && c.key.target<0) {
                if(c.key.target<=-1000000) {
                    DisplaySelectorSample value;
                    if(display->SampleSelector(-1000000-c.key.target,0,value)!=DisplayMotionStatus::Ok)throw std::invalid_argument("negative selector baseline");
                    zero[0]=value.modelIndex;zero[1]=value.boneIndex;
                } else {
                    std::uint8_t value=0;
                    const auto status=c.key.target==-1?display->SampleVisibility(0,value):display->SampleIk(-2-c.key.target,0,value);
                    if(status!=DisplayMotionStatus::Ok)throw std::invalid_argument("negative display baseline");zero[0]=value;
                }
            } else if(c.needsZero) {
                BoneMotionSample value;
                auto status=bones->SampleChannels(c.key.target,0,value);
                if(status==BoneMotionStatus::ExternalParent || status==BoneMotionStatus::UnsupportedBone) {
                    ensureDisplay();status=bones->PrepareKinematicChannels(c.key.target,0,*display,value);
                }
                if(status!=BoneMotionStatus::Ok)throw std::invalid_argument("negative registered bone baseline");
                std::copy(value.position.begin(),value.position.end(),zero.begin());
                std::copy(value.rotation.begin(),value.rotation.end(),zero.begin()+3);
                std::copy(value.scale.v.begin(),value.scale.v.end(),zero.begin()+7);
            }
            const auto matches=[&](const Key& k){return k.model==p.model&&k.identity==c.key.identity&&k.target==c.key.target&&k.morph==c.key.morph;};
            if(c.key.target<0)c.after=sampling::Step(target,app->CurrentFrame(),zero,matches);
            else {
                Key zeroKey;zeroKey.values=zero;if(replacement)zeroKey.author=replacement->author;
                sampling::Result sampled;
                if(!sampling::Track(target,c.key.morph?sampling::Family::Morph:sampling::Family::Bone,
                    app->CurrentFrame(),zeroKey,sampled,matches))throw std::invalid_argument("invalid negative model author sample");
                c.after=sampled.values;
            }
            if(c.key.target>=0 && !Valid(c.after,c.key.morph))throw std::invalid_argument("invalid prepared negative model pose");
            if(!c.key.morph && c.key.target>=0) {
                bone_scale::Matrix scale;std::copy_n(c.after.begin()+7,9,scale.v.begin());
                if(!bone_scale::Valid(scale))throw std::invalid_argument("invalid prepared negative scale");
                if(scale==bone_scale::Matrix{})p.afterScales.erase(c.key.target);else p.afterScales[c.key.target]=scale;
            }
        }
    }
    return result;
}
class NegativeChange final:public SelectedDeletion {
    MMDApp* app_;std::shared_ptr<const char> lifetime_;
    std::vector<Key> before_,after_;
    std::shared_ptr<PhysicsFlags> flags_;
    bool publishKeys_=true;
    struct InitialTimeline {
        bool enabled=false,camera=false;unsigned slot=0;unsigned char* model=nullptr;
        bool Matches(MMDApp& app)const noexcept {
            return !enabled || ((app.CameraMode()!=0)==camera &&
                (camera || (app.SelectedModelSlot()==slot && app.SelectedModel()==model)));
        }
    } initialTimeline_;
    struct Prepared final:plugins::PreparedHistoryChange {
        // Identity of one live key at preparation time. Values are authored
        // data and stay in the key vectors; only the transient selection is
        // part of this snapshot.
        struct SelectionStamp {
            unsigned char* model;const void* identity;int target,frame;bool morph,selected,current;
            bool Matches(const Key& key)const noexcept {
                return model==key.model && identity==key.identity && target==key.target && frame==key.frame && morph==key.morph;
            }
        };
        MMDApp* app;std::shared_ptr<const char> lifetime;int frame=0;
        std::uint64_t generation=0;std::vector<Key> keys;InitialTimeline timeline;
        bool statePresent=false,publishKeys=true,consumed=false;
        std::shared_ptr<PhysicsFlags> flags;bool forward=true;
        std::vector<GlobalPosePublication> poses;
        std::vector<ModelPosePublication> modelPoses;
        // Live key order captured in preparation. Publication swaps the prepared
        // keys without allocating, so the merge of surviving live selection is
        // computed in preparation and validated against this stamp.
        std::vector<SelectionStamp> selection;
        bool IsCurrent()const noexcept override {
            const auto found=states.find(app);
            if(consumed || !timeline.Matches(*app) || app->CurrentFrame()!=frame || (flags && !flags->Current(forward)))return false;
            if(statePresent?(found==states.end() || found->second.lifetime!=lifetime || found->second.keysGeneration!=generation):found!=states.end())return false;
            static const std::vector<Key> empty;
            const auto& live=statePresent?found->second.keys:empty;
            if(live.size()!=selection.size())return false;
            for(std::size_t i=0;i<selection.size();++i) {
                const auto& stamp=selection[i];const auto& key=live[i];
                if(!stamp.Matches(key) || stamp.selected!=key.selected || stamp.current!=Current(*app,key))return false;
            }
            return SameDormantKeys(*app,live,keys) &&
                std::all_of(poses.begin(),poses.end(),[](const auto& pose){return pose.IsCurrent();}) &&
                std::all_of(modelPoses.begin(),modelPoses.end(),[](const auto& pose){return pose.IsCurrent();});
        }
        void Publish()noexcept override {
            if(publishKeys) {auto& state=states.find(app)->second;state.keys.swap(keys);++state.keysGeneration;}
            if(flags)flags->Publish(forward);
            for(const auto& pose:poses)pose.Publish();
            for(auto& pose:modelPoses)pose.Publish();
            consumed=true;
        }
    };
    static bool Measure(std::size_t count,std::size_t width,std::uint64_t& bytes)noexcept {
        if(count>std::numeric_limits<std::uint64_t>::max()/width)return false;
        bytes=std::uint64_t(count)*width;return true;
    }
    // Shared body of the initial commit and history replay. Initial publication
    // keeps the planned after-state selection. Replay copies the target keys and
    // keeps the live selection of keys that still exist; restored keys keep the
    // selection recorded by the snapshot.
    MmsResult PrepareCandidate(bool forward,bool initial,std::unique_ptr<plugins::PreparedHistoryChange>& out,
        const DeletionTargets* targets=nullptr,std::uint64_t* remaining=nullptr) {
        out.reset();const auto found=states.find(app_);
        static const std::vector<Key> empty;
        const auto& live=found==states.end()?empty:found->second.keys;
        if((initial && !initialTimeline_.Matches(*app_)) ||
            (lifetime_?(found==states.end() || found->second.lifetime!=lifetime_):!live.empty()) ||
            (!SameKeys(live,forward?before_:after_) || (flags_ && !flags_->Current(forward))))return MMS_E_STALE;
        if(!SameDormantKeys(*app_,before_,after_))return MMS_E_STALE;
        const auto* history=plugins::BoundNativePoseHistory(app_);
        const auto limit=history?history->MaxBytes():256ull*1024*1024;
        const auto retained=RetainedBytes();
        if(retained>limit)return MMS_E_MEMORY;
        // The selection snapshot and the target copy share the transient budget
        // already granted to pose preparation. RetainedBytes stays the record
        // charge; these candidates are transient and never enter it.
        std::uint64_t budget=std::min<std::uint64_t>(64ull*1024*1024,limit-retained);
        if(remaining)budget=std::min(budget,*remaining);
        const auto granted=budget;
        const auto charge=[&](std::uint64_t bytes){if(bytes>budget)return false;budget-=bytes;return true;};
        if(!charge(sizeof(Prepared)))return MMS_E_MEMORY;
        auto next=std::make_unique<Prepared>();next->app=app_;next->lifetime=lifetime_;next->frame=app_->CurrentFrame();
        next->statePresent=found!=states.end();next->publishKeys=publishKeys_;
        if(next->statePresent){next->lifetime=found->second.lifetime;next->generation=found->second.keysGeneration;}
        next->flags=flags_;next->forward=forward;
        if(initial)next->timeline=initialTimeline_;
        const auto& target=forward?after_:before_;
        std::uint64_t bytes=0;
        if(!Measure(target.size(),sizeof(Key),bytes) || !charge(bytes))return MMS_E_MEMORY;
        next->keys=target;
        if(next->keys.capacity()>target.size() &&
            (!Measure(next->keys.capacity()-target.size(),sizeof(Key),bytes) || !charge(bytes)))return MMS_E_MEMORY;
        if(!Measure(live.size(),sizeof(Prepared::SelectionStamp),bytes) || !charge(bytes))return MMS_E_MEMORY;
        next->selection.reserve(live.size());
        if(next->selection.capacity()>live.size() &&
            (!Measure(next->selection.capacity()-live.size(),sizeof(Prepared::SelectionStamp),bytes) || !charge(bytes)))return MMS_E_MEMORY;
        for(const auto& key:live)next->selection.push_back({key.model,key.identity,key.target,key.frame,key.morph,key.selected,Current(*app_,key)});
        if(!initial)for(auto& key:next->keys) {
            const auto match=std::find_if(next->selection.begin(),next->selection.end(),
                [&](const Prepared::SelectionStamp& stamp){return stamp.Matches(key);});
            if(match!=next->selection.end())key.selected=match->selected;
        }
        std::vector<Key> zeros;if(targets)zeros=CaptureDeletionZeros(*app_,*targets,budget);
        next->poses=PrepareGlobalPoses(app_,live,next->keys,zeros,budget);
        next->modelPoses=PrepareModelPoses(app_,live,next->keys,flags_.get(),forward,zeros,budget,targets);
        if(!next->IsCurrent())return MMS_E_STALE;
        if(remaining)*remaining-=granted-budget;
        out=std::move(next);return MMS_OK;
    }
public:
    NegativeChange(MMDApp& app,const State& state,std::vector<Key> next,unsigned char* model,bool trackTimeline=false,bool publishKeys=true):app_(&app),lifetime_(state.lifetime),before_(state.keys),after_(std::move(next)),publishKeys_(publishKeys){
        if(trackTimeline){initialTimeline_.enabled=true;initialTimeline_.camera=app.CameraMode()!=0;
            if(!initialTimeline_.camera){initialTimeline_.slot=app.SelectedModelSlot();initialTimeline_.model=app.SelectedModel();}}
        if(model && mdl::Mdl(model)->bonePhysicsState) {
            const auto& record=*mdl::Mdl(model);flags_=std::make_shared<PhysicsFlags>();
            flags_->app=&app;flags_->model=model;flags_->table=record.boneTable;flags_->flags=record.bonePhysicsState;
            for(unsigned bone=0;bone<record.boneCount;++bone)if(record.boneSelection[bone])flags_->before.emplace_back(bone,record.bonePhysicsState[bone]);
        }
    }
    explicit NegativeChange(MMDApp& app):app_(&app),publishKeys_(false) {
        initialTimeline_.enabled=true;initialTimeline_.camera=app.CameraMode()!=0;
        if(!initialTimeline_.camera){initialTimeline_.slot=app.SelectedModelSlot();initialTimeline_.model=app.SelectedModel();}
    }
    MmsResult Prepare(bool forward,std::unique_ptr<plugins::PreparedHistoryChange>& out)override {
        return PrepareCandidate(forward,false,out);
    }
    MmsResult PrepareInitial(plugins::SharedHistory&,bool,std::unique_ptr<plugins::PreparedHistoryChange>& out)override {
        return PrepareCandidate(true,true,out);
    }
    MmsResult PrepareSigned(bool forward,bool initial,const DeletionTargets& targets,std::uint64_t& remaining,
        std::unique_ptr<plugins::PreparedHistoryChange>& out) noexcept override {
        out.reset();
        if(app_->CurrentFrame()<frame_limits::minimum||app_->CurrentFrame()>frame_limits::maximum||(!forward&&initial))return MMS_E_ARGUMENT;
        if((targets.globals&&targets.globals->OwnerApp()!=app_)||(targets.model&&targets.model->OwnerApp()!=app_))return MMS_E_ARGUMENT;
        if((targets.globals&&!targets.globals->IsCurrent())||(targets.model&&!targets.model->IsCurrent()))return MMS_E_STALE;
        try {return PrepareCandidate(forward,initial,out,&targets,&remaining);}
        catch(const std::bad_alloc&){return MMS_E_MEMORY;}
        catch(const std::length_error&){return MMS_E_MEMORY;}
        catch(const std::invalid_argument&){return MMS_E_ARGUMENT;}
        catch(...){return MMS_E_FAILED;}
    }
    uint64_t RetainedBytes()const noexcept override {
        auto bytes=detail::AddBytes(sizeof(*this)+256,detail::RetainedKeyBytes(before_));
        bytes=detail::AddBytes(bytes,detail::RetainedKeyBytes(after_));
        return detail::AddBytes(bytes,flags_?sizeof(PhysicsFlags)+256+
            flags_->before.capacity()*sizeof(flags_->before[0]):0);
    }
};
#endif
bool CommitRegistration(MMDApp* app,State& state,std::vector<Key> keys,unsigned char* model=nullptr,const char* label="Register negative keys",bool trackTimeline=false) {
    bool physicsChange=false;
    if(model) {const auto& record=*mdl::Mdl(model);if(record.bonePhysicsState)for(unsigned bone=0;bone<record.boneCount;++bone)
        physicsChange|=record.boneSelection[bone] && record.bonePhysicsState[bone]!=0;}
    if(!physicsChange && SameKeys(state.keys,keys)){state.keys.swap(keys);return true;}
#if !MMD_ORIGINAL_PROFILE
    if(auto* history=plugins::BoundNativePoseHistory(app)) {
        try {
            auto change=std::make_shared<NegativeChange>(*app,state,std::move(keys),model,trackTimeline);
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
            if(history->PrepareNative(label,std::move(change),transaction,
                    plugins::SharedHistory::NativeIntent::ExplicitCommit) || history->CommitNative(std::move(transaction)))return false;
            plugins::PublishBoundPoseEdit(*app,*history);return true;
        } catch(...) {return false;}
    }
#endif
    History(state);state.keys.swap(keys);++state.keysGeneration;app->SceneModified()=1;return true;
}
bool RegisterGlobalImpl(MMDApp* app,Global kind,int frame) try {
    if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto values=GlobalPose(app,int(kind));if(!GlobalValid(values,int(kind)))return false;
    auto& state=states[app];AbortDrag(state);State pending;pending.keys=state.keys;
    Upsert(pending,{nullptr,app,int(kind),frame,false,true,values});return CommitRegistration(app,state,std::move(pending.keys));
} catch(...) {
    return false;
}
void EvaluateGlobalsImpl(MMDApp* app,int frame) {
    auto found=states.find(app);if(found==states.end() || frame>=0)return;
    std::vector<int> kinds;
    for(const auto& key:found->second.keys)if(!key.model && Current(*app,key) && std::find(kinds.begin(),kinds.end(),key.target)==kinds.end())kinds.push_back(key.target);
    for(int kind:kinds) {
        std::vector<const Key*> keys;
        for(const auto& key:found->second.keys)if(!key.model && key.target==kind && Current(*app,key))keys.push_back(&key);
        if(keys.empty())continue;
        Key zero;zero.values=GlobalZero(app,kind);sampling::Result sampled;
        if(sampling::Track(keys,GlobalFamily(kind),frame,zero,sampled,[](const Key&){return true;}))
            ApplyGlobal(app,kind,sampled.values);
    }
}
std::uint32_t Read32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8)|(std::uint32_t(bytes[2])<<16)|(std::uint32_t(bytes[3])<<24);
}
std::uint32_t PayloadCrc32(const std::vector<std::uint8_t>& bytes) {
    bone_scale_io::PrefixFingerprint fingerprint;
    fingerprint.Append(bytes.data(),bytes.size());
    return fingerprint.crc32();
}
const std::uint8_t* SelectionFlags(const pmm_extension::Document& document,
                                 const std::vector<std::uint8_t>& keys,std::uint32_t count) {
    const auto* record=pmm_extension::Find(document,kPmmSelectionKey);
    if(!record)return nullptr;
    const auto& bytes=record->value;
    // Older writers preserve unknown records, so flags only apply to the exact
    // v1 key payload captured alongside them. Bad optional metadata never drops keys.
    if(bytes.size()!=std::uint64_t(count)+12 || Read32(bytes.data())!=1 ||
       Read32(bytes.data()+4)!=count || Read32(bytes.data()+8)!=PayloadCrc32(keys))return nullptr;
    if(std::any_of(bytes.begin()+12,bytes.end(),[](std::uint8_t flag){return flag>1;}))return nullptr;
    return bytes.data()+12;
}
std::vector<Key> NativeClipboard(MMDApp* app) {
    std::vector<Key> out;const auto& counts=app->ClipboardCounts();auto* model=app->SelectedModel();
    const auto global=[&](int kind,unsigned frame,const Values& values) {
        if(frame>unsigned(frame_limits::maximum))return;
        const void* identity=kind>=1000 ? static_cast<const void*>(app->AccessorySlot(kind-1000)) : app;
        if(identity)out.push_back({nullptr,identity,kind,int(frame),false,true,values});
    };
    if(model && !app->CameraMode()) {
        const auto& record=*mdl::Mdl(model);
        for(unsigned i=0;app->BoneClipboard() && i<counts.bones;++i) {
            const auto& source=app->BoneClipboard()[i];if(source.frame>unsigned(frame_limits::maximum))continue;
            int target=frame_clipboard::BoneTarget(app,model,i);
            if(target==frame_clipboard::kMissingTarget)return {};
            if(target<0) {
                for(unsigned candidate=0;candidate<record.boneCount;++candidate)
                    if(!std::strncmp(record.boneTable[candidate].name,source.name,20)){
                        if(target>=0)return {};
                        target=int(candidate);
                    }
                if(target<0)return {};
            }
            if(target>=0) {
                Values values{};std::copy_n(source.position,3,values.begin());std::copy_n(source.rotation,4,values.begin()+3);
                const auto scale=bone_scale::Clipboard(app,int(i),false);std::copy(scale.v.begin(),scale.v.end(),values.begin()+7);
                if(Valid(values,false))out.push_back({model,record.boneTable,target,int(source.frame),false,true,values});
            }
        }
        for(unsigned i=0;app->MorphClipboard() && i<counts.morphs;++i) {
            const auto& source=app->MorphClipboard()[i];if(source.frame>unsigned(frame_limits::maximum))continue;
            int target=frame_clipboard::MorphTarget(app,model,i);
            if(target==frame_clipboard::kMissingTarget)return {};
            if(target<0) {
                for(unsigned candidate=0;candidate<record.morphCount;++candidate)
                    if(!std::strncmp(record.morphs[candidate].name,source.name,20)){
                        if(target>=0)return {};
                        target=int(candidate);
                    }
                if(target<0)return {};
            }
            if(target>=0) {
                Values values{};values[0]=source.value;
                if(Valid(values,true))out.push_back({model,record.morphs,target,int(source.frame),true,true,values});
            }
        }
        for(unsigned i=0;app->DisplayClipboard() && i<counts.displays;++i) {
            const auto& source=app->DisplayClipboard()[i];if(source.frame>unsigned(frame_limits::maximum))continue;
            const auto add=[&](int target,Values values){out.push_back({model,record.boneTable,target,int(source.frame),false,true,values});};
            Values values{};values[0]=source.visible;add(-1,values);
            for(int j=0;source.ikStates && j<source.ikCount;++j)for(unsigned k=0;k<record.ikChainCount;++k) {
                const int bone=mdl::IkChains(model)[k].boneIndex;
                if(bone>=0 && bone<int(record.boneCount) && !std::strncmp(record.boneTable[bone].name,source.ikStates[j].boneName,20)) {
                    values={};values[0]=source.ikStates[j].enabled;add(-2-int(k),values);
                }
            }
        }
    }
    if(app->CameraMode()) {
        for(unsigned i=0;app->CameraClipboard() && i<counts.cameras;++i) {
            const auto& source=app->CameraClipboard()[i];Values values{};
            for(int j=0;j<3;++j){values[j]=source.eye[j];values[3+j]=source.target[j];}
            values[6]=source.distance;values[7]=float(source.fov);values[8]=source.perspective;
            values[9]=double(source.parentModel);values[10]=double(source.parentBone);global(2,source.frame,values);
        }
        for(unsigned i=0;app->LightClipboard() && i<counts.lights;++i) {
            const auto& source=app->LightClipboard()[i];Values values{};
            for(int j=0;j<3;++j){values[j]=source.direction[j];values[3+j]=source.color[j];}global(3,source.frame,values);
        }
        for(unsigned i=0;app->ShadowClipboard() && i<counts.shadows;++i) {
            const auto& source=app->ShadowClipboard()[i];Values values{};values[0]=source.mode;values[1]=source.distance;global(4,source.frame,values);
        }
        for(unsigned i=0;app->GravityClipboard() && i<counts.gravity;++i) {
            const auto& source=app->GravityClipboard()[i];Values values{};values[0]=source.acceleration;
            for(int j=0;j<3;++j)values[1+j]=source.direction[j];values[4]=double(source.noise);values[5]=source.noiseEnabled;global(5,source.frame,values);
        }
        for(unsigned i=0;app->AccessoryClipboard() && i<counts.accessories;++i) {
            const auto& source=app->AccessoryClipboard()[i];if(source.slot>=255)continue;Values values{};
            for(int j=0;j<3;++j){values[j]=source.position[j];values[3+j]=source.rotation[j];}
            values[6]=source.scale;values[7]=source.opacity;values[8]=source.visible;values[9]=source.shadowEnabled;
            values[10]=double(source.parentModel);values[11]=double(source.parentBone);global(1000+source.slot,source.frameOffset,values);
        }
    }
    return out;
}
bool PasteTarget(MMDApp* app,Key& key) {
    if(!Current(*app,key))return false;
    if(!key.model)return app->CameraMode()!=0;
    auto* destination=app->CameraMode() ? nullptr : app->SelectedModel();
    if(!destination)return false;
    if(destination==key.model)return true;
    const auto& source=*mdl::Mdl(key.model);
    const auto& target=*mdl::Mdl(destination);
    int index=-1;
    if(key.target>=0) {
        const char* name=key.morph ? source.morphs[key.target].name : source.boneTable[key.target].name;
        const unsigned count=key.morph ? target.morphCount : target.boneCount;
        unsigned wanted=0;
        for(int i=0;i<key.target;++i)
            if(!std::strncmp(name,key.morph ? source.morphs[i].name : source.boneTable[i].name,20))++wanted;
        unsigned occurrence=0;
        for(unsigned i=0;i<count;++i)if(!std::strncmp(name,key.morph ? target.morphs[i].name : target.boneTable[i].name,20)) {
            if(occurrence++==wanted){index=int(i);break;}
        }
        if(index<0)return false;
    } else if(key.target==-1)index=-1;
    else if(key.target>-1000000) {
        const int bone=mdl::IkChains(key.model)[-2-key.target].boneIndex;
        if(bone<0 || bone>=int(source.boneCount))return false;
        for(unsigned i=0;i<target.ikChainCount;++i) {
            const int targetBone=mdl::IkChains(destination)[i].boneIndex;
            if(targetBone>=0 && targetBone<int(target.boneCount) &&
               !std::strncmp(source.boneTable[bone].name,target.boneTable[targetBone].name,20)) {
                index=-2-int(i);break;
            }
        }
        if(index==-1)return false;
    } else return false;
    key.model=destination;key.target=index;
    key.identity=key.morph ? static_cast<const void*>(target.morphs) : target.boneTable;
    return true;
}
int NativeSelectionFirst(MMDApp* app,bool* any=nullptr) {
    if(any)*any=false;
    int first=frame_limits::maximum;
    const auto scan=[&](const auto* keys,std::size_t capacity,bool model) {
        for(std::size_t i=0;keys && i<capacity;++i) {
            bool marked;
            if constexpr (std::is_same_v<std::decay_t<decltype(keys[i])>,mdl::BoneKey> ||
                          std::is_same_v<std::decay_t<decltype(keys[i])>,mdl::MorphKey> ||
                          std::is_same_v<std::decay_t<decltype(keys[i])>,mdl::DisplayKey>)marked=keys[i].allocated!=0;
            else marked=keys[i].selected!=0;
            (void)model;
            if(marked){if(any)*any=true;first=std::min(first,int(keys[i].frame));}
        }
    };
    if(!app->CameraMode() && app->SelectedModel()) {
        const auto& record=*mdl::Mdl(app->SelectedModel());
        scan(record.boneKeys,mdl::BoneKeyCapacity(record),true);
        scan(record.morphKeys,mdl::MorphKeyCapacity(record.morphKeys),true);
        scan(record.displayKeys,mdl::KeyPoolCapacity(record.displayKeys),true);
    } else {
        scan(app->CameraKeys(),mdl::KeyPoolCapacity(app->CameraKeys()),false);
        scan(app->LightKeys(),mdl::KeyPoolCapacity(app->LightKeys()),false);
        scan(app->ShadowKeys(),mdl::KeyPoolCapacity(app->ShadowKeys()),false);
        scan(app->GravityKeys(),mdl::KeyPoolCapacity(app->GravityKeys()),false);
        for(int i=0;i<255;++i)scan(app->AccessoryKeys(i),mdl::KeyPoolCapacity(app->AccessoryKeys(i)),false);
    }
    return first;
}
#include "negative_bound_commands.inc"
}
#if !MMD_ORIGINAL_PROFILE
bool DeletionInteractionReady(MMDApp& app,bool historyTraversal) noexcept try {
    const auto found=states.find(&app);
    const State* state=found==states.end()?nullptr:&found->second;
    return NegativeInteractionReady(app,state,historyTraversal) &&
        app.CurrentFrame()>=frame_limits::minimum && app.CurrentFrame()<=frame_limits::maximum &&
        DeletionSelectionPoolsReady(app);
} catch(...) {
    return false;
}
MmsResult QueryDeletionSelection(MMDApp& app,plugins::SharedHistory& history,DeletionSelection& out) noexcept {
    return plugins::Boundary([&]() -> MmsResult {
        if(plugins::BoundNativePoseHistory(&app)!=&history)return MMS_E_NOT_READY;
        if(const auto result=history.CheckNativeEdit())return result;
        if(!DeletionInteractionReady(app))return MMS_E_NOT_READY;
        const auto found=states.find(&app);
        const State* state=found==states.end()?nullptr:&found->second;
        DeletionSelection selection;
        selection.negative=state && std::any_of(state->keys.begin(),state->keys.end(),
            [&](const Key& key){return key.selected && InActiveTimeline(app,key) && Current(app,key);});
        // maximum is a valid selected frame as well as the search sentinel.
        NativeSelectionFirst(&app,&selection.positive);
        out=selection;
        return MMS_OK;
    });
}
MmsResult PlanSelectedDeletion(MMDApp& app,std::shared_ptr<plugins::HistoryChange>& out) noexcept {
    out.reset();std::shared_ptr<SelectedDeletion> change;
    const auto result=PlanSignedSelectedDeletion(app,change);
    if(!result)out=std::move(change);return result;
}
MmsResult PlanSignedSelectedDeletion(MMDApp& app,std::shared_ptr<SelectedDeletion>& out,bool includeUnchanged) noexcept {
    out.reset();
    return plugins::Boundary([&]() -> MmsResult {
        const auto found=states.find(&app);
        if(found==states.end()) {if(includeUnchanged)out=std::make_shared<NegativeChange>(app);return MMS_OK;}
        const auto& state=found->second;
        if(!state.dragging.empty())return MMS_E_NOT_READY;
        auto keys=state.keys;
        keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const Key& k){return k.selected && InActiveTimeline(app,k) && Current(app,k);}),keys.end());
        const bool changed=keys.size()!=state.keys.size();
        if(changed||includeUnchanged)out=std::make_shared<NegativeChange>(app,state,std::move(keys),nullptr,true,changed);
        return MMS_OK;
    });
}
#endif
bool RegisterGlobal(MMDApp* app,Global kind,int frame) {return RegisterGlobalImpl(app,kind,frame);}
void EvaluateGlobals(MMDApp* app,int frame) {EvaluateGlobalsImpl(app,frame);}
bool RegisterAccessory(MMDApp* app,int slot,int frame) try {
    if(!app || slot<0 || slot>=255 || !app->AccessorySlot(slot) || frame>=0 || frame<frame_limits::minimum)return false;
    auto values=GlobalPose(app,1000+slot);if(!GlobalValid(values,1000+slot))return false;
    auto& state=states[app];AbortDrag(state);State pending;pending.keys=state.keys;
    Upsert(pending,{nullptr,app->AccessorySlot(slot),1000+slot,frame,false,true,values});
    return CommitRegistration(app,state,std::move(pending.keys));
} catch(...) {
    return false;
}
bool RegisterDisplay(unsigned char* model,int frame) try {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& state=states[app];AbortDrag(state);State pending;pending.keys=state.keys;auto& record=*mdl::Mdl(model);
    const auto add=[&](int target){Upsert(pending,{model,record.boneTable,target,frame,false,true,Pose(model,target,false)});};
    add(-1);for(unsigned i=0;i<record.ikChainCount;++i)add(-2-int(i));
    for(unsigned i=0;i<record.boneOrderCount;++i)add(-1000000-int(i));
    return CommitRegistration(app,state,std::move(pending.keys));
} catch(...) {
    return false;
}
bool RegisterBone(unsigned char* model,int bone,int frame) try {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& record=*mdl::Mdl(model);
    if(!record.boneTable || bone<0 || bone>=int(record.boneCount))return false;
    auto values=Pose(model,bone,false);if(!Valid(values,false))return false;
    auto& state=states[app];AbortDrag(state);State pending;pending.keys=state.keys;
    Upsert(pending,{model,record.boneTable,bone,frame,false,true,values});return CommitRegistration(app,state,std::move(pending.keys));
} catch(...) {
    return false;
}
bool RegisterSelectedBones(unsigned char* model,int frame) try {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& record=*mdl::Mdl(model);if(!record.boneTable || !record.boneSelection)return false;
    std::vector<Key> pending;
    for(int bone=0;bone<int(record.boneCount);++bone)if(record.boneSelection[bone]) {
        auto values=Pose(model,bone,false);if(!Valid(values,false))return false;
        pending.push_back({model,record.boneTable,bone,frame,false,true,values});
    }
    if(pending.empty())return true;
    auto& state=states[app];AbortDrag(state);State staged;staged.keys=state.keys;
    for(const auto& key:pending)Upsert(staged,key);
    if(!CommitRegistration(app,state,std::move(staged.keys),model))return false;
    for(const auto& key:pending)if(record.bonePhysicsState)record.bonePhysicsState[key.target]=0;
    return true;
} catch(...) {
    return false;
}
bool RegisterMorph(unsigned char* model,int morph,int frame,float value) try {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& record=*mdl::Mdl(model);
    if(!record.morphs || morph<0 || morph>=int(record.morphCount) || !std::isfinite(value))return false;
    Values values{};values[0]=value;
    auto& state=states[app];AbortDrag(state);State pending;pending.keys=state.keys;
    Upsert(pending,{model,record.morphs,morph,frame,true,true,values});return CommitRegistration(app,state,std::move(pending.keys));
} catch(...) {
    return false;
}
void EvaluateModel(unsigned char* model,int frame) {
    auto* app=Owner(model);if(!app || frame>=0)return;
    auto found=states.find(app);if(found==states.end())return;
    std::map<std::pair<bool,int>,std::vector<const Key*>> tracks;
    for(const auto& key:found->second.keys)if(key.model==model && Current(*app,key))tracks[{key.morph,key.target}].push_back(&key);
    for(auto& pair:tracks) {
        auto& keys=pair.second;
        const int target=pair.first.second;const bool morph=pair.first.first;
        const auto zeroValues=Pose(model,target,morph);
        if(target<0)Apply(model,target,morph,sampling::Step(keys,frame,zeroValues,[](const Key&){return true;}));
        else {
            Key zero;zero.values=zeroValues;sampling::Result sampled;
            if(sampling::Track(keys,morph?sampling::Family::Morph:sampling::Family::Bone,frame,zero,sampled,[](const Key&){return true;}))
                Apply(model,target,morph,sampled.values);
        }
    }
}
void Reset(MMDApp* app) {
#if !MMD_ORIGINAL_PROFILE
    if(app)app->RetireFrameCopies();
#endif
    states.erase(app);
}
void ClearClipboard(MMDApp* app) noexcept {
    const auto found=states.find(app);
    if(found!=states.end())std::vector<Key>().swap(found->second.clipboard);
}
void ForgetModel(unsigned char* model) {
    for(auto& pair:states) {
        AbortDrag(pair.second);
        const auto erase=[&](std::vector<Key>& keys){keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const Key& key){return key.model==model;}),keys.end());};
        const auto size=pair.second.keys.size();
        erase(pair.second.keys);erase(pair.second.clipboard);
        if(pair.second.keys.size()!=size)++pair.second.keysGeneration;
        for(auto& entry:pair.second.undo)erase(entry.keys);
        for(auto& entry:pair.second.redo)erase(entry.keys);
    }
}
void RefreshControls(MMDApp* app) {
    if(!app || !app->MainWindow())return;auto found=states.find(app);if(found==states.end())return;
    const auto& state=found->second;
    const bool selected=std::any_of(state.keys.begin(),state.keys.end(),[&](const Key& key){return key.selected && Current(*app,key);});
    const bool pasted=!state.clipboard.empty(),undo=!state.undo.empty(),redo=!state.redo.empty();
    const bool history=app->CurrentFrame()<0 || (undo && state.undo.back().drag) || (redo && state.redo.back().drag);
    const auto window=app->MainWindow();
    // EnableWindow sends synchronous messages: callbacks may replace State.
    if(selected){EnableWindow(GetDlgItem(window,420),TRUE);EnableWindow(GetDlgItem(window,423),TRUE);}
    if(pasted)EnableWindow(GetDlgItem(window,421),TRUE);
    if(history) {
        if(undo)EnableWindow(GetDlgItem(window,400),TRUE);
        if(redo)EnableWindow(GetDlgItem(window,401),TRUE);
    }
}
std::size_t Count(const MMDApp* app) {auto found=states.find(app);return found==states.end()?0:found->second.keys.size();}
int Earliest(const MMDApp* app) {
    auto found=states.find(app);int first=0;
    if(found!=states.end())for(const auto& key:found->second.keys)if(Current(*app,key))first=std::min(first,key.frame);
    return first;
}
void Capture(const MMDApp& app,pmm_extension::Document& document) {
    auto& mutableApp=const_cast<MMDApp&>(app);
    std::vector<std::uint8_t> cursor;
    U32(cursor,std::uint32_t(mutableApp.CurrentFrame()));U32(cursor,std::uint32_t(app.state.timelineStartFrame));
    pmm_extension::Set(document,"org.mikudancestudio.timeline.signed_cursor.v1",std::move(cursor));
    if(app.state.hwnd) {
        std::vector<std::uint8_t> bounds;
        for(int id:{panel::kPlayStartFrameEdit,panel::kPlayStopFrameEdit}) {
            char text[64]{};GetDlgItemTextA(app.state.hwnd,id,text,sizeof(text));int value=0;
            if(!frame_limits::ParsePlaybackBound(text,value))throw std::runtime_error("Invalid signed playback range");
            U32(bounds,std::uint32_t(value));
        }
        pmm_extension::Set(document,"org.mikudancestudio.timeline.signed_play_range.v1",std::move(bounds));
    }
    const auto found=states.find(&app);std::vector<std::uint8_t> bytes;U32(bytes,1);U32(bytes,0);unsigned count=0;
    // Keep the v1 key rows readable by older programs. Selection is a separate
    // record: u32 version, u32 key count, u32 key-payload CRC32, then one 0/1 per row.
    std::vector<std::uint8_t> selection;U32(selection,1);U32(selection,0);U32(selection,0);
    if(found!=states.end())for(const auto& key:found->second.keys)if(Current(app,key)) {
        int slot=255;if(key.model){slot=0;while(slot<255 && app.ModelSlot(slot)!=key.model)++slot;}
        U32(bytes,unsigned(slot));U32(bytes,!key.model?2:key.target<0?3:key.morph?1:0);U32(bytes,unsigned(key.target));U32(bytes,std::uint32_t(key.frame));
        if(key.model && key.target>=0){const auto& record=*mdl::Mdl(key.model);
            const char* name=key.morph ? record.morphs[key.target].name : record.boneTable[key.target].name;
            bytes.insert(bytes.end(),name,name+20);}
        else bytes.insert(bytes.end(),20,0);
        for(double value:key.values){std::uint64_t bits;std::memcpy(&bits,&value,8);for(int j=0;j<8;++j)bytes.push_back(std::uint8_t(bits>>(8*j)));}
        selection.push_back(key.selected?1:0);++count;
    }
    if(!count){pmm_extension::Erase(document,kPmmKey);pmm_extension::Erase(document,kPmmSelectionKey);return;}
    for(int i=0;i<4;++i)bytes[4+i]=selection[4+i]=std::uint8_t(count>>(8*i));
    const auto crc=PayloadCrc32(bytes);
    for(int i=0;i<4;++i)selection[8+i]=std::uint8_t(crc>>(8*i));
    pmm_extension::Set(document,kPmmKey,std::move(bytes));
    pmm_extension::Set(document,kPmmSelectionKey,std::move(selection));
}
static void RestoreMetadata(MMDApp& app,const pmm_extension::Document& document) {
    if(const auto* cursor=pmm_extension::Find(document,"org.mikudancestudio.timeline.signed_cursor.v1");cursor && cursor->value.size()==8) {
        const auto frame=static_cast<std::int32_t>(Read32(cursor->value.data()));
        const auto scroll=static_cast<std::int32_t>(Read32(cursor->value.data()+4));
        if(frame>=frame_limits::minimum && frame<=frame_limits::maximum && scroll>=frame_limits::minimum && scroll<=frame_limits::maximum) {
            app.CurrentFrame()=frame;app.state.timelineStartFrame=scroll;
        }
    }
    if(const auto* bounds=pmm_extension::Find(document,"org.mikudancestudio.timeline.signed_play_range.v1");bounds && bounds->value.size()==8 && app.MainWindow()) {
        for(int i=0;i<2;++i) {
            const auto value=static_cast<std::int32_t>(Read32(bounds->value.data()+i*4));
            if(value<frame_limits::minimum || value>frame_limits::maximum)continue;
            char text[64]{};sprintf_s(text,"%d",value);
            SetDlgItemTextA(app.MainWindow(),i?panel::kPlayStopFrameEdit:panel::kPlayStartFrameEdit,text);
        }
    }
}
bool Load(MMDApp& app,const pmm_extension::Document& document) {
    const auto* payload=pmm_extension::Find(document,kPmmKey);
    if(!payload){Reset(&app);RestoreMetadata(app,document);return true;}
    const auto& bytes=payload->value;constexpr std::size_t stride=164;
    if(bytes.size()<8 || Read32(bytes.data())!=1)return false;
    const auto count=Read32(bytes.data()+4);
    if(std::uint64_t(count)*stride!=bytes.size()-8)return false;
    const auto* selection=SelectionFlags(document,bytes,count);
    State pending;
    std::set<std::tuple<unsigned,unsigned,unsigned,int>> unique;
    for(unsigned i=0;i<count;++i) {
        const auto* row=bytes.data()+8+std::size_t(i)*stride;
        const auto slot=Read32(row),kind=Read32(row+4),target=Read32(row+8);
        const auto frame=static_cast<std::int32_t>(Read32(row+12));
        const bool selected=selection && selection[i]!=0;
        if(!unique.emplace(slot,kind,target,frame).second)return false;
        if(kind==2) {
            if(slot!=255 || !((target>=2 && target<=5)||(target>=1000 && target<1255)) || frame>=0 || frame<frame_limits::minimum)return false;
            Values values{};for(unsigned j=0;j<16;++j){std::uint64_t bits=0;for(int k=0;k<8;++k)bits|=std::uint64_t(row[36+j*8+k])<<(8*k);std::memcpy(&values[j],&bits,8);if(!std::isfinite(values[j]))return false;}
            if(!GlobalValid(values,int(target)))return false;
            const void* identity=target>=1000 ? static_cast<const void*>(app.AccessorySlot(target-1000)) : &app;
            if(identity)Upsert(pending,{nullptr,identity,int(target),frame,false,selected,values});continue;
        }
        if(kind==3) {
            if(slot>=255 || frame>=0 || frame<frame_limits::minimum)return false;
            if(static_cast<std::int32_t>(target)>=0)return false;
            auto* model=app.ModelSlot(slot);if(!model)continue;
            Values values{};for(unsigned j=0;j<16;++j){std::uint64_t bits=0;for(int k=0;k<8;++k)bits|=std::uint64_t(row[36+j*8+k])<<(8*k);std::memcpy(&values[j],&bits,8);if(!std::isfinite(values[j]))return false;}
            Key key{model,mdl::Mdl(model)->boneTable,static_cast<std::int32_t>(target),frame,false,selected,values};
            if(key.target<=-1000000) {
                if(!Integer(values[0],-1,254) || !Integer(values[1],-1,frame_limits::maximum))return false;
            } else if(!Integer(values[0],0,1))return false;
            if(Current(app,key))Upsert(pending,key);continue;
        }
        if(slot>=255 || kind>1 || frame>=0 || frame<frame_limits::minimum)return false;
        auto* model=app.ModelSlot(slot);if(!model)continue;auto& record=*mdl::Mdl(model);
        if(target>=(kind ? record.morphCount : record.boneCount))continue;
        const char* name=kind ? record.morphs[target].name : record.boneTable[target].name;
        if(std::memcmp(name,row+16,20))continue;
        Values values{};for(unsigned j=0;j<16;++j){std::uint64_t bits=0;for(int k=0;k<8;++k)bits|=std::uint64_t(row[36+j*8+k])<<(8*k);std::memcpy(&values[j],&bits,8);}
        if(!Valid(values,kind!=0))return false;
        Key key{model,kind ? static_cast<const void*>(record.morphs) : record.boneTable,int(target),frame,kind!=0,selected,values};
        const auto before=pending.keys.size();Upsert(pending,key);if(pending.keys.size()==before)return false;
    }
    states[&app]=std::move(pending);
#if !MMD_ORIGINAL_PROFILE
    app.RetireFrameCopies();
#endif
    RestoreMetadata(app,document);return true;
}
void SelectRange(MMDApp* app,int first,int last) {
    auto found=states.find(app);if(found==states.end())return;
    const HWND combo=GetDlgItem(app->MainWindow(),panel::kRegisterScopeCombo);
    const auto row=combo ? SendMessageW(combo,CB_GETCURSEL,0,0) : CB_ERR;
    const auto scope=row==CB_ERR ? FrameScope::AllFrames : static_cast<FrameScope>(SendMessageW(combo,CB_GETITEMDATA,row,0));
    for(auto& key:found->second.keys) {
        key.selected=false;
        if(!Current(*app,key) || key.frame<first || key.frame>last)continue;
        if(app->CameraMode()) {key.selected=!key.model && (row==CB_ERR || Row(*app,key)==row);continue;}
        if(key.model!=app->SelectedModel())continue;
        const auto& record=*mdl::Mdl(key.model);
        if(scope==FrameScope::AllFrames)key.selected=true;
        else if(scope==FrameScope::AllBones)key.selected=!key.morph && key.target>=0;
        else if(scope==FrameScope::AllMorphs)key.selected=key.morph;
        else if(scope==FrameScope::Display)key.selected=key.target<0;
        else if(scope==FrameScope::SelectedBones)key.selected=!key.morph && key.target>=0 && record.boneSelection[key.target];
        else if(scope==FrameScope::SelectedMorphs && key.morph) {
            for(unsigned i=0;i<mdl::FacialFrameCount(record);++i)if(record.displayFrames[i].selected && mdl::FrameTargetIndex(record.displayFrames[i])==key.target)key.selected=true;
        } else if(scope==FrameScope::NamedTrack && key.target>=0) {
            char name[256]{};GetWindowTextA(combo,name,sizeof(name));
            key.selected=std::strcmp(name,key.morph?record.morphs[key.target].name:record.boneTable[key.target].name)==0;
        }
    }
}
CopyResult CopySelected(MMDApp* app, bool refreshControls) noexcept try {
    if(!app)return CopyResult::Failed;
    const auto found=states.find(app);
    if(found==states.end())return CopyResult::Empty;
    auto& state=found->second;
    const bool selected=std::any_of(state.keys.begin(),state.keys.end(),[&](const Key& key){
        return key.selected && InActiveTimeline(*app,key) && Current(*app,key);
    });
    if(!selected){state.clipboard.clear();return CopyResult::Empty;}
    // Preserve the legacy adapter here. It is not a lossless signed source.
    auto clipboard=NativeClipboard(app);
    if(!clipboard.empty()) {
        const int first=NativeSelectionFirst(app);
        for(auto& key:clipboard)key.frame+=first;
    }
    for(auto key:state.keys)
        if(key.selected && InActiveTimeline(*app,key) && Current(*app,key))clipboard.push_back(key);
    int first=0;for(const auto& key:clipboard)first=std::min(first,key.frame);
    for(auto& key:clipboard)key.clipboardOffset=std::int64_t(key.frame)-first;
    state.clipboard.swap(clipboard);
    if(refreshControls)RefreshControls(app);
    return CopyResult::Copied;
} catch(const std::bad_alloc&) {
    ClearClipboard(app);
    return CopyResult::Memory;
} catch(...) {
    ClearClipboard(app);
    return CopyResult::Failed;
}
bool Command(MMDApp* app,unsigned id) try {
    if(id==420)return CopySelected(app)!=CopyResult::Empty;
#if !MMD_ORIGINAL_PROFILE
    if(TryBoundNegativeCommand(app,id))return true;
#endif
    if(id==421 && app->CurrentFrame()<0) {
        auto& state=states[app];if(state.clipboard.empty()) {
            state.clipboard=NativeClipboard(app);
            for(auto& key:state.clipboard)key.clipboardOffset=key.frame;
        }
    }
    auto found=states.find(app);if(found==states.end())return false;auto& state=found->second;
    const bool selected=std::any_of(state.keys.begin(),state.keys.end(),[&](const Key& k){return k.selected && InActiveTimeline(*app,k) && Current(*app,k);});
    if(id==421 && !state.clipboard.empty()) {
        auto pasted=state.clipboard;
        for(auto& key:pasted)if(!PasteTarget(app,key))return true;
        if(pasted.empty())return true;
        for(const auto& key:pasted)if(std::int64_t(app->CurrentFrame())+key.clipboardOffset>frame_limits::maximum ||
            std::int64_t(app->CurrentFrame())+key.clipboardOffset<frame_limits::minimum)return true;
        AbortDrag(state);History(state);for(auto key:pasted){key.frame=int(std::int64_t(app->CurrentFrame())+key.clipboardOffset);
            if(key.frame<0)Upsert(state,key);
            else if(key.model) {
                Apply(key.model,key.target,key.morph,key.values);
                if(key.target<0)RegisterDisplayKeyCurrent(key.model,key.frame);
                else if(key.morph)RegisterMorphKeyCurrent(key.model,key.target,key.frame);
                else {
                    auto& record=*mdl::Mdl(key.model);
                    std::vector<unsigned char> selection(record.boneSelection,record.boneSelection+record.boneCount);
                    std::fill_n(record.boneSelection,record.boneCount,0);record.boneSelection[key.target]=1;
                    RegisterSelectedBoneKeys(key.model,key.frame,0);
                    std::copy(selection.begin(),selection.end(),record.boneSelection);
                }
            } else {
                ApplyGlobal(app,key.target,key.values);
                if(key.target==2)RegisterCameraState(app,key.frame);
                else if(key.target==3)RegisterLightState(app,key.frame);
                else if(key.target==4)RegisterSelfShadowState(app,key.frame);
                else if(key.target==5)RegisterGravityKeyCurrent(app,key.frame);
                else if(key.target>=1000)RegisterAccessoryKey(app,key.frame,key.target-1000);
            }
        }Changed(app);return true;
    }
    if(id==423 && selected) {
        AbortDrag(state);History(state);state.keys.erase(std::remove_if(state.keys.begin(),state.keys.end(),[&](const Key& k){return k.selected && InActiveTimeline(*app,k) && Current(*app,k);}),state.keys.end());
        ++state.keysGeneration;
        if(app->MainWindow())RefreshAfterFrameApply(app);Changed(app);return true;
    }
    if(id==400 || id==401) {
        AbortDrag(state);
        auto& from=id==400 ? state.undo : state.redo;auto& to=id==400 ? state.redo : state.undo;
        if(from.empty())return false;
        if(app->CurrentFrame()>=0 && !from.back().drag)return false;
        auto now=Snapshot(state);now.drag=from.back().drag;
        if(now.model!=from.back().model || (now.model && now.nativeCursor!=from.back().nativeCursor)) {
            if(id==401)state.redo.clear();
            return false;
        }
        to.push_back(now);state.keys=std::move(from.back().keys);from.pop_back();
        ++state.keysGeneration;
        if(app->MainWindow())RefreshAfterFrameApply(app);Changed(app);return true;
    }
    return false;
} catch(...) {
    return id==400 || id==401 || id==420 || id==421 || id==423;
}
void Draw(MMDApp* app,void* dc,void* icons,int columns,int height) {
    auto found=states.find(app);if(found==states.end())return;
    const morph_display::Rows morphRows(app->CameraMode() ? nullptr : mdl::Mdl(app->SelectedModel()));
    for(const auto& key:found->second.keys)if(Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel())) {
        const auto column=std::int64_t(key.frame)-app->state.timelineStartFrame;
        if(column<0 || column>=columns)continue;
        const int x=95+13*int(column);
        ForEachRow(*app,key,morphRows,[&](int row){
            const int y=14*row+17;
            if(row<0 || (key.model && key.target>=0 && row<=0) || y>=height)return;
            BitBlt(HDC(dc),x,y,11,11,HDC(icons),44,0,SRCAND);
            BitBlt(HDC(dc),x,y,11,11,HDC(icons),key.selected?22:0,0,SRCPAINT);
        });
    }
}
bool SelectCell(MMDApp* app,int frame,int row,bool additive) {
    auto found=states.find(app);if(found==states.end())return false;bool hit=false;
    const morph_display::Rows morphRows(app->CameraMode() ? nullptr : mdl::Mdl(app->SelectedModel()));
    for(auto& key:found->second.keys)if(Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel())) {
        if(key.frame==frame && AnyRow(*app,key,morphRows,[&](int keyRow){return keyRow==row;})) {
            key.selected=additive ? !key.selected : true;hit=true;
        }
        else if(!additive)key.selected=false;
    }
    return hit;
}
bool CellSelected(MMDApp* app,int frame,int row) {
    auto found=states.find(app);if(found==states.end())return false;
    const morph_display::Rows morphRows(app->CameraMode() ? nullptr : mdl::Mdl(app->SelectedModel()));
    return std::any_of(found->second.keys.begin(),found->second.keys.end(),[&](const Key& key){
        return Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel()) &&
            key.frame==frame && key.selected &&
            AnyRow(*app,key,morphRows,[&](int keyRow){return keyRow==row;});
    });
}
void BeginDrag(MMDApp* app) {
    auto found=states.find(app);if(found==states.end())return;
    auto& state=found->second;AbortDrag(state);
    for(std::size_t i=0;i<state.keys.size();++i) {
        const auto& key=state.keys[i];
        if(key.selected && Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel()))
            state.dragging.emplace_back(i,key.frame);
    }
    std::vector<bool> moving(state.keys.size(),false);
    for(const auto& item:state.dragging)moving[item.first]=true;
    // Compute collision limits once at press time, not on every mouse move.
    std::int64_t low=frame_limits::minimum,high=frame_limits::maximum;
    for(const auto& [index,frame]:state.dragging) {
        if(index>=state.keys.size()){state.dragging.clear();return;}
        low=std::max(low,std::int64_t(frame_limits::minimum)-frame);
        high=std::min(high,std::int64_t(-1)-frame);
        const auto& key=state.keys[index];
        for(std::size_t i=0;i<state.keys.size();++i) {
            const auto& other=state.keys[i];
            if(other.model!=key.model || other.identity!=key.identity || other.target!=key.target || other.morph!=key.morph)continue;
            if(moving[i])continue;
            if(other.frame<frame)low=std::max(low,std::int64_t(other.frame)+1-frame);
            else if(other.frame>frame)high=std::min(high,std::int64_t(other.frame)-1-frame);
        }
    }
    state.dragMin=low;state.dragMax=high;
    state.dragGeneration=state.keysGeneration;
}
void Drag(MMDApp* app,int offset) {
    auto found=states.find(app);if(found==states.end())return;
    auto& state=found->second;if(state.dragging.empty())return;
    // The plan stores positions from the press-time key vector. If any command
    // inserted, removed or replaced keys since then, those positions are stale:
    // abort instead of touching an unrelated key.
    if(state.dragGeneration!=state.keysGeneration){AbortDrag(state);return;}
    // Preserve track order, spacing and frame-zero boundary. Always use the
    // press snapshot so reversing direction restores the original frames.
    const int effective=int(std::clamp(std::int64_t(offset),state.dragMin,state.dragMax));
    if(effective==state.dragOffset)return;
    // Validate the whole plan before writing anything, so a stale entry can
    // never leave a half-applied drag behind.
    for(const auto& item:state.dragging)if(item.first>=state.keys.size()){AbortDrag(state);return;}
    if(!state.dragHistory){History(state);state.undo.back().drag=true;state.dragHistory=true;}
    for(const auto& [index,frame]:state.dragging)state.keys[index].frame=frame+effective;
    state.dragOffset=effective;app->SceneModified()=1;RefreshControls(app);
}
void EndDrag(MMDApp* app) {
    auto found=states.find(app);if(found==states.end())return;
    AbortDrag(found->second);
}
void SelectBox(MMDApp* app,std::int64_t first,std::int64_t last,int top,int bottom,bool selected,bool additive) {
    auto found=states.find(app);if(found==states.end())return;
    const morph_display::Rows morphRows(app->CameraMode() ? nullptr : mdl::Mdl(app->SelectedModel()));
    for(auto& key:found->second.keys)if(Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel())) {
        const bool hit=AnyRow(*app,key,morphRows,[&](int row){
            return row>=0 && (!key.model || key.target<0 || row>0) && row>=top && row<bottom;
        });
        if(hit && key.frame>=first && key.frame<last)key.selected=selected;
        else if(!additive)key.selected=false;
    }
}
void SelectColumns(MMDApp* app) {
    auto found=states.find(app);if(found==states.end())return;
    const auto active=[&](const Key& key){return Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel());};
    std::vector<int> frames;
    for(const auto& key:found->second.keys)if(active(key) && key.selected)frames.push_back(key.frame);
    for(auto& key:found->second.keys)if(active(key) && std::find(frames.begin(),frames.end(),key.frame)!=frames.end())key.selected=true;
}
#include "negative_copy_source.inc"
}
