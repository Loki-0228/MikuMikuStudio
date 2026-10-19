#include "native_frame_lines.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_morph_keys.hpp"
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <array>
#include <cmath>
#include <map>
#include <vector>
namespace mikumikustudio { void RefreshFrameLineViews(MMDApp*); }
namespace mikumikustudio::plugins {
namespace {
constexpr double pi=3.14159265358979323846;
using Kind=curve_host::Kind;
template<class K> K*& Pool(MMDApp& app){
    if constexpr(std::is_same_v<K,mdl::CameraKey>)return app.CameraKeys();
    else if constexpr(std::is_same_v<K,mdl::LightKey>)return app.LightKeys();
    else return app.ShadowKeys();
}
template<class K> constexpr Kind TrackKind(){
    if constexpr(std::is_same_v<K,mdl::CameraKey>)return Kind::Camera;
    else if constexpr(std::is_same_v<K,mdl::LightKey>)return Kind::Light;
    else return Kind::Shadow;
}
template<class K> bool SameKey(K a,K b,bool marks=false) noexcept {
    if(!marks)a.selected=b.selected=0;return std::memcmp(&a,&b,sizeof(K))==0;
}
template<class K> struct Dependency {std::uint32_t slot;K key;};
template<class K,class Read> std::vector<Dependency<K>> Chain(std::size_t capacity,Read read) {
    if(!capacity)throw std::invalid_argument("empty global pool");
    std::vector<Dependency<K>> chain;std::vector<unsigned char> visited(capacity,0);
    std::uint32_t index=0,previous=0,last=0;
    for(std::size_t hops=0;;++hops){
        if(index>=capacity||visited[index])throw std::invalid_argument("global key cycle");
        const auto key=read(index);
        if(key.frame>frame_limits::maximum||(!hops&&key.frame)||(hops&&(key.frame<=last||key.previous!=previous)))
            throw std::invalid_argument("global key order");
        visited[index]=1;chain.push_back({index,key});if(!key.next)break;last=key.frame;previous=index;index=key.next;
    }
    for(std::size_t i=1;i<capacity;++i)if(!visited[i]&&read(std::uint32_t(i)).frame)throw std::invalid_argument("orphan global key");
    return chain;
}
struct Pose {
    std::array<float,9> values{};int parentModel=-1,parentBone=0,mode=0;
    bool operator==(const Pose& other) const noexcept{return values==other.values&&parentModel==other.parentModel&&parentBone==other.parentBone&&mode==other.mode;}
};
bool FinitePose(const Pose& pose) noexcept {
    for(float value:pose.values)if(!std::isfinite(value))return false;
    return true;
}
template<class K> Pose ReadPose(MMDApp& app) noexcept {
    Pose p;
    if constexpr(std::is_same_v<K,mdl::CameraKey>){for(unsigned i=0;i<3;++i){p.values[i]=app.CameraPositionAxis(i);p.values[i+3]=app.CameraRotation()[i];}
        p.values[6]=app.CameraDistance();p.values[7]=app.CameraFov();p.mode=app.CameraPerspective();p.parentModel=app.CameraParentModel();p.parentBone=app.CameraParentBone();}
    else if constexpr(std::is_same_v<K,mdl::LightKey>){for(unsigned i=0;i<3;++i){p.values[i]=app.LightDirection()[i];p.values[i+3]=app.LightColor()[i];}}
    else{p.values[0]=app.ShadowDistance();p.mode=app.ShadowMode();}
    return p;
}
template<class K> void WritePose(MMDApp& app,const Pose& p) noexcept {
    if constexpr(std::is_same_v<K,mdl::CameraKey>){for(unsigned i=0;i<3;++i){app.CameraPositionAxis(i)=p.values[i];app.CameraRotation()[i]=p.values[i+3];}
        app.CameraDistance()=p.values[6];app.CameraFov()=p.values[7];app.CameraPerspective()=std::uint8_t(p.mode);app.CameraParentModel()=p.parentModel;app.CameraParentBone()=p.parentBone;}
    else if constexpr(std::is_same_v<K,mdl::LightKey>){for(unsigned i=0;i<3;++i){app.LightDirection()[i]=p.values[i];app.LightColor()[i]=p.values[i+3];}app.ApplyTimelineLightState();}
    else{app.ShadowDistance()=p.values[0];app.ShadowMode()=p.mode;}
}
template<class K> Pose CurvePose(const std::array<double,12>& v){
    Pose p;
    if constexpr(std::is_same_v<K,mdl::CameraKey>){for(unsigned i=0;i<3;++i){p.values[i]=float(v[i]);p.values[i+3]=float(v[i+3]*pi/180);}
        p.values[6]=float(v[6]);p.values[7]=float(v[7]);p.mode=int(v[8]);p.parentModel=int(v[9]);p.parentBone=int(v[10]);}
    else if constexpr(std::is_same_v<K,mdl::LightKey>){for(unsigned i=0;i<6;++i)p.values[i]=float(v[i]);}
    else{p.mode=int(v[0]);p.values[0]=float(v[1]);}return p;
}
float NativeCameraEase(const mdl::CameraKey& key,unsigned lane,float t) noexcept {
    const auto& c=key.interpolation;
    if(c[0][lane]==c[1][lane]&&c[2][lane]==c[3][lane])return t;
#if defined(_M_IX86)
    const double x1=double(c[0][lane])/127.0,x2=double(c[2][lane])/127.0;
    float u=.5f,half=.25f;
    for(unsigned i=0;i<12;++i){const float v=1-u;
        const double x=v*3.0*u*u*x2+v*(v*3.0)*u*x1+u*u*u;
        if(float(x)==t)break;u=float(x>=double(t)?u-half:u+half);half*=.5f;}
    const float v=1-u;
    return float(3.0*v*u*u*double(c[3][lane])/127.0+v*(v*3.0)*u*double(c[1][lane])/127.0+u*u*u);
#else
    const float x1=float(c[0][lane])/127.0f,x2=float(c[2][lane])/127.0f;
    float u=.5f,half=.25f;
    for(unsigned i=0;i<12;++i){const float v=1-u,v3=v*3;
        const float x=(v3*v*u)*x1+(v3*u*u)*x2+u*u*u;
        if(x==t)break;u=x>=t?u-half:u+half;half*=.5f;}
    const float v=1-u,v3=v*3;
    return (v3*v*u)*float(c[1][lane])/127.0f+(v3*u*u)*float(c[3][lane])/127.0f+u*u*u;
#endif
}
template<class K,class Read> Pose NativePose(int frame,Read read) {
    unsigned slot=0;while(read(slot)->frame<unsigned(frame)&&read(slot)->next)slot=read(slot)->next;
    const auto& current=*read(slot);const bool between=current.frame>unsigned(frame);
    const auto& previous=between?*read(current.previous):current;
    Pose p;
    if constexpr(std::is_same_v<K,mdl::CameraKey>){
        for(unsigned i=0;i<3;++i){p.values[i]=previous.eye[i];p.values[i+3]=previous.target[i];}
        p.values[6]=previous.distance;p.values[7]=float(previous.fov);p.mode=previous.perspective;
        p.parentModel=previous.parentModel;p.parentBone=previous.parentBone;
        if(between&&current.frame-previous.frame>1){const float t=float(double(unsigned(frame)-previous.frame)/double(current.frame-previous.frame));
            for(unsigned i=0;i<8;++i){const unsigned lane=i<3?i:i<6?3:i==6?4:5;
                const double a=i<3?previous.eye[i]:i<6?previous.target[i-3]:i==6?previous.distance:double(previous.fov);
                const double b=i<3?current.eye[i]:i<6?current.target[i-3]:i==6?current.distance:double(current.fov);
                p.values[i]=float(double(NativeCameraEase(current,lane,t))*(b-a)+a);}
        }
    }else if constexpr(std::is_same_v<K,mdl::LightKey>){
        const double t=between?double(unsigned(frame)-previous.frame)/double(current.frame-previous.frame):0;
        for(unsigned i=0;i<3;++i){p.values[i]=between?float((current.direction[i]-previous.direction[i])*t+previous.direction[i]):current.direction[i];
            p.values[i+3]=between?float((current.color[i]-previous.color[i])*t+previous.color[i]):current.color[i];}
    }else{p.mode=previous.mode;p.values[0]=previous.distance;}
    return p;
}
struct Parent {
    unsigned slot;unsigned char* model;std::shared_ptr<RetainedSceneObject> owner;
    mdl::BoneRecord* bones;std::uint32_t count;
    Parent(MMDApp& app,int modelIndex,int bone):slot(unsigned(modelIndex)),model(nullptr),bones(nullptr),count(0){
        if(modelIndex<0||slot>=kModelSlotCount||!(model=app.ModelSlot(slot)))throw std::invalid_argument("camera parent model");
        const auto& m=*mdl::Mdl(model);bones=m.boneTable;count=m.boneCount;
        if(!bones||bone<0||unsigned(bone)>=count)throw std::invalid_argument("camera parent bone");
        owner=RetainSceneObject(app,MMS_OBJECT_MODEL,slot);
    }
    bool Current(MMDApp& app) const noexcept{return owner->Present()&&app.ModelSlot(slot)==model&&SceneObjectPresent(model,owner->Id())&&
        mdl::Mdl(model)->boneTable==bones&&mdl::Mdl(model)->boneCount==count;}
};

template<class K> struct FrameLine final : HistoryChange, std::enable_shared_from_this<FrameLine<K>> {
    MMDApp& app; std::uint64_t identity, generation = 1; std::size_t capacity;
    std::uint32_t beforeLast, afterLast;
    curve_host::TrackAuthoring authoring;
    struct Patch { unsigned slot; K before, after; }; std::vector<Patch> patches;
    std::vector<Dependency<K>> plannedSource;
    std::vector<std::pair<int,int>> frameMap;
    std::vector<Parent> parents;
    bool published=false;
    FrameLine(MMDApp& a) : app(a), identity(mdl::KeyPoolIdentity(Pool<K>(a))), capacity(mdl::KeyPoolCapacity(Pool<K>(a))),
        beforeLast(a.LastRegisteredFrame()), afterLast(beforeLast), authoring(a, TrackKind<K>()) {}
    bool Structure() const noexcept {
        if (mdl::KeyPoolIdentity(Pool<K>(app)) != identity || mdl::KeyPoolCapacity(Pool<K>(app)) < capacity) return false;
        for (const auto& p : parents) if (!p.Current(app)) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<FrameLine> source; bool forward, consumed = false; std::uint64_t generation;
        int frame; K* pool; std::size_t capacity; Pose before, after;
        std::vector<Dependency<K>> dependencies; std::vector<K> current;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves;
        std::unique_ptr<curve_host::TrackAuthoring::Target> sourceCurves;
        Candidate(std::shared_ptr<FrameLine> s, bool f) : source(std::move(s)), forward(f), generation(source->generation),
            frame(source->app.CurrentFrame()), pool(Pool<K>(source->app)), capacity(mdl::KeyPoolCapacity(pool)), before(ReadPose<K>(source->app)), after(before) {}
        const K* Target(unsigned slot) const noexcept {
            const auto& p = source->patches; const auto found = std::lower_bound(p.begin(), p.end(), slot, [](const auto& entry, unsigned i) { return entry.slot < i; });
            return found != p.end() && found->slot == slot ? (forward ? &found->after : &found->before) : slot < capacity ? &pool[slot] : nullptr;
        }
        bool IsCurrent() const noexcept override {
            if (consumed || source->generation != generation || !source->Structure() || source->app.CurrentFrame() != frame ||
                !(ReadPose<K>(source->app) == before) || Pool<K>(source->app) != pool || mdl::KeyPoolCapacity(pool) != capacity ||
                source->app.LastRegisteredFrame() != (forward ? source->beforeLast : source->afterLast) ||
                !curves->IsCurrent() || !sourceCurves->IsCurrent()) return false;
            for (const auto& dependency : dependencies) if (!SameKey(dependency.key, pool[dependency.slot], true)) return false;
            for (std::size_t i=0;i<source->patches.size();++i) if (!SameKey(current[i], pool[source->patches[i].slot], true)) return false;
            return true;
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (const auto& patch : source->patches) pool[patch.slot] = forward ? patch.after : patch.before;
            curves->Publish(pool); source->app.LastRegisteredFrame() = forward ? source->afterLast : source->beforeLast;
            WritePose<K>(source->app, after);source->published=true;consumed = true; ++source->generation;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure() || !authoring.IsCurrent() || (!published&&!forward)) return MMS_E_STALE;
        if (generation == UINT64_MAX) return MMS_E_NOT_READY;
        return Boundary([&]() -> MmsResult {
            if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum || !FinitePose(ReadPose<K>(app))) return MMS_E_ARGUMENT;
            auto candidate = std::make_unique<Candidate>(this->shared_from_this(), forward);
            candidate->dependencies = Chain<K>(candidate->capacity, [&](unsigned i) { return candidate->pool[i]; });
            if(!published){
                if(candidate->dependencies.size()!=plannedSource.size())return MMS_E_STALE;
                for(std::size_t i=0;i<plannedSource.size();++i)if(candidate->dependencies[i].slot!=plannedSource[i].slot||
                    !SameKey(candidate->dependencies[i].key,plannedSource[i].key,false))return MMS_E_STALE;
            }
            for (const auto& patch : patches) {
                const auto live = candidate->pool[patch.slot];
                if (!SameKey(live, forward ? patch.before : patch.after, false)) return MMS_E_STALE;
                candidate->current.push_back(live);
            }
            curve_host::NativeKeyView target{candidate.get(),candidate->capacity,[](const void* p,unsigned i) noexcept -> const void* {return static_cast<const Candidate*>(p)->Target(i);}};
            candidate->curves = authoring.PrepareRetimed(target,frameMap,forward);
            curve_host::NativeKeyView current{candidate->pool,candidate->capacity,[](const void* p,unsigned i) noexcept -> const void* {return static_cast<const K*>(p)+i;}};
            candidate->sourceCurves = authoring.PrepareRetimed(current,frameMap,!forward);
            const auto sample = [&](const auto& curves, auto read) {
                if (!curves->Authored()) return NativePose<K>(app.CurrentFrame(), read);
                std::array<double,12> values{}; if (!curves->Sample(app.CurrentFrame(), values)) throw std::invalid_argument("frame line sample");
                return CurvePose<K>(values);
            };
            const auto evaluated = sample(candidate->sourceCurves,[&](unsigned i){return candidate->pool+i;});
            // A differing live value is an unregistered authored pose. Keep it;
            // only a value still following this track follows the retimed track.
            if (candidate->before == evaluated) candidate->after = sample(candidate->curves,[&](unsigned i){return candidate->Target(i);});
            if (!FinitePose(candidate->after)) return MMS_E_ARGUMENT;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        auto bytes=std::uint64_t(sizeof(*this)+4*sizeof(void*));
        const auto add=[&](std::uint64_t n){bytes=n>UINT64_MAX-bytes?UINT64_MAX:bytes+n;};
        add(patches.capacity()*sizeof(Patch));add(plannedSource.capacity()*sizeof(Dependency<K>));
        add(frameMap.capacity()*sizeof(frameMap[0]));add(parents.capacity()*sizeof(Parent));add(authoring.RetainedBytes());return bytes;
    }
};
template<class K> MmsResult PlanGlobal(MMDApp& app,int frame,bool insert,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); if(frame<0||frame>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(!mdl::KeyPoolIdentity(Pool<K>(app)))return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        if(!FinitePose(ReadPose<K>(app)))return MMS_E_ARGUMENT;
        auto* keys=Pool<K>(app); const auto capacity=mdl::KeyPoolCapacity(keys);
        const auto chain=Chain<K>(capacity,[&](unsigned i){return keys[i];});
        auto change=std::make_shared<FrameLine<K>>(app);
        change->plannedSource=chain;
        std::vector<int> sourceFrames;change->authoring.RetimingFrames(sourceFrames);
        for(const auto& entry:chain)sourceFrames.push_back(int(entry.key.frame));
        std::sort(sourceFrames.begin(),sourceFrames.end());sourceFrames.erase(std::unique(sourceFrames.begin(),sourceFrames.end()),sourceFrames.end());
        const bool rootOverwritten=!insert&&frame==0&&std::binary_search(sourceFrames.begin(),sourceFrames.end(),1);
        bool authoredRetime=false;
        for(const auto source:sourceFrames){int target=source;
            if(!source){if(rootOverwritten)target=-1;}
            else if(source>=frame){
                if(insert){if(source>=frame_limits::maximum)return MMS_E_ARGUMENT;target=source+1;}
                else if(source==frame)target=-1;
                else{target=source-1;if(!target&&!rootOverwritten)target=-1;}
            }
            authoredRetime|=target!=source;change->frameMap.push_back({source,target});
        }
        auto staged=std::vector<K>(keys,keys+capacity);
        for(std::size_t i=1;i<capacity;++i){auto& key=staged[i];const auto f=key.frame;
            if(!f||f<unsigned(frame))continue;
            if(insert){if(f>=unsigned(frame_limits::maximum))return MMS_E_ARGUMENT;key.frame=f+1;change->afterLast=(std::max)(change->afterLast,f+1);continue;}
            if(f==unsigned(frame)){
                staged[key.previous].next=key.next;staged[key.next].previous=key.previous;
                if constexpr(std::is_same_v<K,mdl::CameraKey>){
                    key.next=key.previous=key.frame=0;key.perspective=0;key.distance=0;key.fov=0;
                    key.eye[0]=key.eye[1]=0;key.target[0]=key.target[1]=0;
                    for(auto& row:key.interpolation)std::memset(row,0,4);key.selected=0;
                }else{key.next=key.previous=key.frame=0;key.selected=0;std::memset(key.reserved1,0,sizeof(key.reserved1));}
            }else{
                key.frame=f-1;
                if(!key.frame){auto& previous=staged[key.previous];previous.next=key.next;
                    if constexpr(std::is_same_v<K,mdl::CameraKey>){previous.distance=key.distance;previous.fov=key.fov;previous.perspective=key.perspective;
                        std::memcpy(previous.eye,key.eye,sizeof(key.eye));std::memcpy(previous.target,key.target,sizeof(key.target));
                        std::memcpy(previous.interpolation,key.interpolation,sizeof(previous.interpolation));
                        previous.parentModel=key.parentModel;previous.parentBone=key.parentBone;
                    }else{std::memcpy(previous.direction,key.direction,sizeof(key.direction));std::memcpy(previous.color,key.color,sizeof(key.color));}
                    if(key.next)staged[key.next].previous=key.previous;
                }
            }
        }
        Chain<K>(capacity,[&](unsigned i){return staged[i];});
        for(std::size_t i=0;i<capacity;++i)if(!SameKey(keys[i],staged[i],true))change->patches.push_back({unsigned(i),keys[i],staged[i]});
        if(change->patches.empty()&&!authoredRetime)return MMS_OK;
        if constexpr(std::is_same_v<K,mdl::CameraKey>){
            const auto parent=[&](int model,int bone){if(model==-1)return;Parent p(app,model,bone);
                if(std::none_of(change->parents.begin(),change->parents.end(),[&](const auto& old){return old.slot==p.slot;}))change->parents.push_back(std::move(p));};
            const auto pose=ReadPose<K>(app);parent(pose.parentModel,pose.parentBone);
            for(const auto& key:chain)parent(key.key.parentModel,key.key.parentBone);
        }
        out=std::move(change);return MMS_OK;
    });
}
}
MmsResult PlanCameraFrameLine(MMDApp& app,int frame,bool insert,std::shared_ptr<HistoryChange>& out) noexcept{return PlanGlobal<mdl::CameraKey>(app,frame,insert,out);}
MmsResult PlanLightFrameLine(MMDApp& app,int frame,bool insert,std::shared_ptr<HistoryChange>& out) noexcept{return PlanGlobal<mdl::LightKey>(app,frame,insert,out);}
bool TryBoundFrameLineCommand(MMDApp* app,unsigned command,int& affected) noexcept {
    if(!app)return false;auto* history=BoundNativePoseHistory(app);if(!history)return false;
    affected=0;
    if(command<255||command>258||app->PlaybackActive()||app->FrameStepPlayback()||app->state.recordPlaybackStartPending||
        app->SceneMutationInProgress()||bake::UiLocked()||SceneSaveInProgress()||GetCapture()||history->CheckNativeEdit()!=MMS_OK)return true;
    Boundary([&]() -> MmsResult {
        const bool insert=command==255||command==257;const bool boneOrCamera=command<=256;
        std::shared_ptr<HistoryChange> change;MmsResult result;
        if(app->state.optflag[0])result=boneOrCamera?PlanCameraFrameLine(*app,app->CurrentFrame(),insert,change):PlanLightFrameLine(*app,app->CurrentFrame(),insert,change);
        else if(app->SelectedModelSlot()>=kModelSlotCount||!app->SelectedModel())return MMS_E_NOT_READY;
        else if(boneOrCamera)result=PlanBoneFrameLine(*app,app->SelectedModelSlot(),app->CurrentFrame(),insert,
            IsDlgButtonChecked(app->MainWindow(),panel::kPhysicsFrameCheckbox)==BST_CHECKED,change);
        else result=PlanMorphFrameLine(*app,app->SelectedModelSlot(),app->CurrentFrame(),insert,change);
        if(result||!change)return result;
        int changed = 0;
        const auto count = [&](const auto* keys, std::size_t capacity, std::size_t heads) {
            for (std::size_t i=heads;i<capacity;++i) if (keys[i].frame && keys[i].frame >= unsigned(app->CurrentFrame())) ++changed;
        };
        if (app->state.optflag[0]) {
            if (boneOrCamera) count(app->CameraKeys(),mdl::KeyPoolCapacity(app->CameraKeys()),1);
            else count(app->LightKeys(),mdl::KeyPoolCapacity(app->LightKeys()),1);
        } else {
            const auto& model=*mdl::Mdl(app->SelectedModel());
            if (boneOrCamera) count(model.boneKeys,mdl::BoneKeyCapacity(model.boneKeys),model.boneCount);
            else count(model.morphKeys,mdl::MorphKeyCapacity(model.morphKeys),model.morphCount);
        }
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        if(auto r=history->PrepareNative(insert?"Insert frame line":"Delete frame line",std::move(change),token))return r;
        if(auto r=history->CommitNative(std::move(token)))return r;
        affected=changed;PublishBoundPoseEdit(*app,*history,false);
        if(app->state.optflag[0])curve_host::NotifyRegistered(app,boneOrCamera?Kind::Camera:Kind::Light);
        else curve_host::NotifyModelRegistered(app,!boneOrCamera);
        SelectionReeval(app);
        if(app->MainWindow() && app->Renderer()) PanelPaint(app);
        RefreshFrameLineViews(app);return MMS_OK;
    });return true;
}
}
#endif
