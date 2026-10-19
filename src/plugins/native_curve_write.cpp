#include "native_curve_write.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <array>
#include <cmath>
#include <vector>
namespace mikumikustudio::plugins {
namespace {
using Kind=curve_host::Kind;
constexpr double pi=3.14159265358979323846;
void Add(std::uint64_t& bytes,std::uint64_t count,std::uint64_t stride=1)noexcept{bytes=stride&&count>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+count*stride;}
template<class K>K*& Pool(MMDApp& app,unsigned slot){
    if constexpr(std::is_same_v<K,mdl::CameraKey>)return app.CameraKeys();
    else if constexpr(std::is_same_v<K,mdl::LightKey>)return app.LightKeys();
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)return app.ShadowKeys();
    else if constexpr(std::is_same_v<K,mdl::GravityKey>)return app.GravityKeys();
    else return app.AccessoryKeys(slot);
}
template<class K>std::uint32_t& Cursor(MMDApp& app,unsigned slot){
    if constexpr(std::is_same_v<K,mdl::CameraKey>)return app.CameraTrackCursor();
    else if constexpr(std::is_same_v<K,mdl::LightKey>)return app.LightTrackCursor();
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)return app.ShadowTrackCursor();
    else if constexpr(std::is_same_v<K,mdl::GravityKey>)return app.GravityTrackCursor();
    else return app.AccessoryTrackCursor(slot);
}
template<class K>unsigned char& Active(MMDApp& app,unsigned slot){
    if constexpr(std::is_same_v<K,mdl::CameraKey>)return app.CameraTrackActive();
    else if constexpr(std::is_same_v<K,mdl::LightKey>)return app.LightTrackActive();
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)return app.ShadowTrackActive();
    else if constexpr(std::is_same_v<K,mdl::GravityKey>)return app.GravityTrackActive();
    else return app.AccessoryTrackActive(slot);
}
template<class K>bool ExclusivePool(MMDApp& app,Kind kind,unsigned slot)noexcept{
    const auto* target=Pool<K>(app,slot);const auto capacity=mdl::KeyPoolCapacity(target);
    const auto first=reinterpret_cast<std::uintptr_t>(target);
    if(!target||capacity>(UINTPTR_MAX-first)/sizeof(K))return false;const auto last=first+capacity*sizeof(K);
    const unsigned own=kind==Kind::Accessory?4+slot:unsigned(kind);bool valid=true;
    const auto inspect=[&](const auto* keys,unsigned index){if(!keys||index==own)return;
        const auto begin=reinterpret_cast<std::uintptr_t>(keys),count=mdl::KeyPoolCapacity(keys);
        if(count>(UINTPTR_MAX-begin)/sizeof(*keys)){valid=false;return;}
        if(begin<last&&first<begin+count*sizeof(*keys))valid=false;};
    inspect(app.CameraKeys(),0);inspect(app.LightKeys(),1);inspect(app.ShadowKeys(),2);inspect(app.GravityKeys(),3);
    for(unsigned i=0;i<kModelSlotCount;++i)inspect(app.AccessoryKeys(i),4+i);return valid;
}
template<class K>std::array<double,12> ReadPose(MMDApp& app,unsigned slot)noexcept{
    std::array<double,12> v{};
    if constexpr(std::is_same_v<K,mdl::CameraKey>){for(int i=0;i<3;++i){v[i]=app.CameraPositionAxis(i);v[i+3]=app.CameraRotation()[i];}
        v[6]=app.CameraDistance();v[7]=app.CameraFov();v[8]=app.CameraPerspective();v[9]=app.CameraParentModel();v[10]=app.CameraParentBone();}
    else if constexpr(std::is_same_v<K,mdl::LightKey>){for(int i=0;i<3;++i){v[i]=app.LightDirection()[i];v[i+3]=app.LightColor()[i];}}
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>){v[0]=app.ShadowMode();v[1]=app.ShadowDistance();}
    else if constexpr(std::is_same_v<K,mdl::GravityKey>){v[0]=app.GravityMagnitude();for(int i=0;i<3;++i)v[i+1]=app.GravityDirection()[i];v[4]=app.GravityNoise();v[5]=app.GravityNoiseModeWord();}
    else{const auto& a=*app.AccessorySlot(slot);for(int i=0;i<3;++i){v[i]=a.position[i];v[i+3]=a.rotation[i];}
        v[6]=a.scale;v[7]=a.opacity;v[8]=a.visible;v[9]=a.shadowEnabled;v[10]=a.parentModel;v[11]=a.parentBone;}
    return v;
}
template<class K>void WritePose(MMDApp& app,unsigned slot,const std::array<double,12>& v)noexcept{
    if constexpr(std::is_same_v<K,mdl::CameraKey>){
        if(app.CameraDistance()!=float(v[6])||app.CameraFov()!=float(v[7])||app.CameraPerspective()!=std::uint8_t(v[8]))app.cameraProjectionRefreshPending=true;
        for(int i=0;i<3;++i){app.CameraPositionAxis(i)=float(v[i]);app.CameraRotation()[i]=float(v[i+3]);}
        app.CameraDistance()=float(v[6]);app.CameraFov()=float(v[7]);app.CameraPerspective()=std::uint8_t(v[8]);app.CameraParentModel()=int(v[9]);app.CameraParentBone()=int(v[10]);}
    else if constexpr(std::is_same_v<K,mdl::LightKey>){for(int i=0;i<3;++i){app.LightDirection()[i]=float(v[i]);app.LightColor()[i]=float(v[i+3]);}app.ApplyTimelineLightState();}
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>){app.ShadowMode()=int(v[0]);app.ShadowDistance()=float(v[1]);}
    else if constexpr(std::is_same_v<K,mdl::GravityKey>){app.GravityMagnitude()=float(v[0]);for(int i=0;i<3;++i)app.GravityDirection()[i]=float(v[i+1]);app.GravityNoise()=int(v[4]);app.GravityNoiseModeWord()=std::uint32_t(v[5]);}
    else{auto& a=*app.AccessorySlot(slot);for(int i=0;i<3;++i){a.position[i]=float(v[i]);a.rotation[i]=float(v[i+3]);}
        a.scale=float(v[6]);a.opacity=float(v[7]);a.visible=std::uint8_t(v[8]);a.shadowEnabled=std::uint8_t(v[9]);a.parentModel=int(v[10]);a.parentBone=int(v[11]);}
}
template<class K>std::array<double,12> AuthoredPose(std::array<double,12> v)noexcept{
    if constexpr(std::is_same_v<K,mdl::CameraKey>||std::is_same_v<K,mdl::AccessoryKey>)for(int i=3;i<6;++i)v[i]=float(v[i]*pi/180);
    for(auto& value:v)value=double(float(value));return v;
}
bool Finite(const std::array<double,12>& v)noexcept{for(double value:v)if(!std::isfinite(value))return false;return true;}
float CameraEase(const mdl::CameraKey& key,unsigned lane,float time)noexcept{
    const auto& c=key.interpolation;if(c[0][lane]==c[1][lane]&&c[2][lane]==c[3][lane])return time;
    const float x1=float(c[0][lane])/127.f,x2=float(c[2][lane])/127.f;float u=.5f,half=.25f;
    for(int i=0;i<12;++i){const float v=1-u,v3=v*3;const float x=(v3*v*u)*x1+(v3*u*u)*x2+u*u*u;
        if(x==time)break;u=x>=time?u-half:u+half;half*=.5f;}
    const float v=1-u,v3=v*3;return (v3*v*u)*float(c[1][lane])/127.f+(v3*u*u)*float(c[3][lane])/127.f+u*u*u;
}
template<class K>std::array<double,12> NativePose(const std::vector<K>& keys,unsigned frame){
    unsigned slot=0;while(keys[slot].frame<frame&&keys[slot].next)slot=keys[slot].next;
    const auto& b=keys[slot];const bool between=b.frame>frame;const auto& a=between?keys[b.previous]:b;
    std::array<double,12> v{};const double t=between?double(frame-a.frame)/double(b.frame-a.frame):0;
    if constexpr(std::is_same_v<K,mdl::CameraKey>){
        for(int i=0;i<3;++i){v[i]=a.eye[i];v[i+3]=a.target[i];}v[6]=a.distance;v[7]=a.fov;v[8]=a.perspective;v[9]=a.parentModel;v[10]=a.parentBone;
        if(between&&b.frame-a.frame>1)for(unsigned i=0;i<8;++i){const auto lane=i<3?i:i<6?3:i==6?4:5;
            const double first=i<3?a.eye[i]:i<6?a.target[i-3]:i==6?a.distance:double(a.fov);
            const double last=i<3?b.eye[i]:i<6?b.target[i-3]:i==6?b.distance:double(b.fov);
            v[i]=float(double(CameraEase(b,lane,float(t)))*(last-first)+first);}
    }else if constexpr(std::is_same_v<K,mdl::LightKey>){for(int i=0;i<3;++i){
        v[i]=between?float(double(b.direction[i]-a.direction[i])*t+a.direction[i]):a.direction[i];
        v[i+3]=between?float(double(b.color[i]-a.color[i])*t+a.color[i]):a.color[i];}}
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>){v[0]=a.mode;v[1]=a.distance;}
    else if constexpr(std::is_same_v<K,mdl::GravityKey>){
        std::uint32_t mode=0;std::memcpy(&mode,&b.noiseEnabled,sizeof mode);v[5]=mode;
        const float fraction=between?float(frame-a.frame)/float(b.frame-a.frame):0;
        v[0]=between?(b.acceleration-a.acceleration)*fraction+a.acceleration:a.acceleration;
        for(int i=0;i<3;++i)v[i+1]=between?(b.direction[i]-a.direction[i])*fraction+a.direction[i]:a.direction[i];
        const auto delta=std::int64_t(b.noise)-a.noise;
        if(between&&(delta<INT32_MIN||delta>INT32_MAX))throw std::invalid_argument("curve gravity noise difference overflow");
        const float scaled=fraction*float(delta);
        if(between&&(!std::isfinite(scaled)||double(scaled)<INT32_MIN||double(scaled)>INT32_MAX))throw std::invalid_argument("curve gravity noise interpolation overflow");
        const auto noise=between?std::int64_t(std::int32_t(scaled))+a.noise:std::int64_t(a.noise);
        if(noise<INT32_MIN||noise>INT32_MAX)throw std::invalid_argument("curve gravity noise result overflow");v[4]=double(noise);
    }
    else{for(int i=0;i<3;++i){v[i]=a.position[i];v[i+3]=a.rotation[i];}const float fraction=between?float(frame-a.frame)/float(b.frame-a.frame):0;
        v[6]=between?a.scale+fraction*(b.scale-a.scale):a.scale;v[7]=between?a.opacity+fraction*(b.opacity-a.opacity):a.opacity;
        v[8]=a.visible;v[9]=a.shadowEnabled;v[10]=a.parentModel;v[11]=a.parentBone;}
    return v;
}
template<class K>bool Equal(K a,K b,bool marks=true)noexcept{if(!marks)a.selected=b.selected=0;return !std::memcmp(&a,&b,sizeof(K));}
bool EquivalentNativeFreeze(const curve_editor::Channel& before,const curve_editor::Channel& after){
    if(!before.preservedCameraIntervals.empty())return before.preservedCameraIntervals==after.preservedCameraIntervals;
    for(const auto& interval:after.preservedCameraIntervals){
        const auto at=std::lower_bound(before.track.keys.begin(),before.track.keys.end(),interval.last,[](const auto& key,int frame){return key.frame<frame;});
        const auto index=std::size_t(at-before.track.keys.begin());
        if(at==before.track.keys.end()||!index||at->frame!=interval.last||(at-1)->frame!=interval.first||
           index>=before.nativeControls.size()||index>=before.controlGeometry.size())return false;
        const auto& a=*(at-1);const auto& b=*at;
        if(interval.sourceFirst!=a.frame||interval.sourceLast!=b.frame||interval.sampleFirst!=a.frame||interval.sampleLast!=b.frame||
           interval.sourceValueFirst!=a.value||interval.sourceValueLast!=b.value||interval.controls!=before.nativeControls[index]||
           before.controlGeometry[index]!=std::array<double,4>{a.outWeight,a.outSlope,b.inWeight,b.inSlope}||
           interval.geometry!=std::array<double,6>{a.value,a.outWeight,a.outSlope,b.value,b.inWeight,b.inSlope})return false;
    }return true;
}
template<class K>void ValidateChain(const std::vector<K>& keys){
    if(keys.empty())throw std::invalid_argument("curve write missing pool");
    std::vector<unsigned char> visited(keys.size());unsigned index=0,previous=0,last=0;
    for(std::size_t hop=0;;++hop){if(index>=keys.size()||visited[index])throw std::invalid_argument("curve write cyclic keys");
        const auto& key=keys[index];if(key.frame>frame_limits::maximum||(!hop&&key.frame)||(hop&&(key.frame<=last||key.previous!=previous)))throw std::invalid_argument("curve write key order");
        visited[index]=1;if(!key.next)break;previous=index;last=key.frame;index=key.next;}
    for(std::size_t i=1;i<keys.size();++i)if(!visited[i]&&keys[i].frame)throw std::invalid_argument("curve write orphan key");
}
template<class K>void ValidateNativeFallback(const std::vector<K>& keys){
    unsigned index=0;
    while(keys[index].next){const auto& a=keys[index];const auto& b=keys[a.next];
        if(b.frame-a.frame<=1){index=a.next;continue;}
        const auto difference=[](float first,float last){if(!std::isfinite(last-first))throw std::invalid_argument("curve native interpolation difference overflow");};
        if constexpr(std::is_same_v<K,mdl::LightKey>){for(unsigned i=0;i<3;++i){difference(a.direction[i],b.direction[i]);difference(a.color[i],b.color[i]);}}
        else if constexpr(std::is_same_v<K,mdl::GravityKey>){
            difference(a.acceleration,b.acceleration);for(unsigned i=0;i<3;++i)difference(a.direction[i],b.direction[i]);
            const auto delta=std::int64_t(b.noise)-a.noise;
            if(delta<INT32_MIN||delta>INT32_MAX)throw std::invalid_argument("curve native gravity noise difference overflow");
            if(b.frame-a.frame>1){
                // For long spans float division can round the last interior
                // fraction to one. Validate that actual native extreme too.
                const float fraction=float(b.frame-a.frame-1)/float(b.frame-a.frame),scaled=fraction*float(delta);
                if(!std::isfinite(scaled)||double(scaled)<INT32_MIN||double(scaled)>INT32_MAX)
                    throw std::invalid_argument("curve native gravity noise interpolation overflow");
                const auto noise=std::int64_t(std::int32_t(scaled))+a.noise;
                if(noise<INT32_MIN||noise>INT32_MAX)throw std::invalid_argument("curve native gravity noise result overflow");
            }
        }else if constexpr(std::is_same_v<K,mdl::AccessoryKey>){difference(a.scale,b.scale);difference(a.opacity,b.opacity);}
        index=a.next;
    }
}
struct Parent{
    unsigned slot,count;unsigned char* model;mdl::BoneRecord* bones;std::shared_ptr<RetainedSceneObject> owner;
    std::uint64_t initialGeneration;
    bool Current(MMDApp& app)const noexcept{return owner->Present()&&app.ModelSlot(slot)==model&&SceneObjectPresent(model,owner->Id())&&
        mdl::Mdl(model)->boneCount==count&&mdl::Mdl(model)->boneTable==bones;}
};
struct FrameMark {
    std::uint32_t frame;unsigned char selected;
    bool operator==(const FrameMark& other)const noexcept{return frame==other.frame&&selected==other.selected;}
};
template<class Read>std::vector<FrameMark> CaptureFrameMarks(std::size_t capacity,Read read){
    std::vector<FrameMark> marks;unsigned slot=0;
    for(std::size_t hops=0;;++hops){if(slot>=capacity||hops>=capacity)throw std::invalid_argument("curve selection chain");
        const auto& key=read(slot);marks.push_back({key.frame,key.selected});if(!key.next)break;slot=key.next;}
    return marks;
}
void ParentAt(MMDApp& app,int model,int bone,std::vector<Parent>& parents){
    if(model==-1)return;if(model<0||model>=kModelSlotCount||bone<0||!app.ModelSlot(model))throw std::invalid_argument("curve write parent model");
    auto* pointer=app.ModelSlot(model);const auto& value=*mdl::Mdl(pointer);
    if(!value.boneTable||unsigned(bone)>=value.boneCount)throw std::invalid_argument("curve write parent bone");
    if(std::none_of(parents.begin(),parents.end(),[&](const auto& p){return p.slot==unsigned(model);}))
        parents.push_back({unsigned(model),value.boneCount,pointer,value.boneTable,RetainSceneObject(app,MMS_OBJECT_MODEL,unsigned(model)),SceneObjectIdentity(pointer).generation});
}
template<class K>void ParentKey(MMDApp& app,const K& key,std::vector<Parent>& parents){
    if constexpr(std::is_same_v<K,mdl::CameraKey>||std::is_same_v<K,mdl::AccessoryKey>)ParentAt(app,key.parentModel,key.parentBone,parents);
}
template<class K>struct WriteChange final:HistoryChange,std::enable_shared_from_this<WriteChange<K>>{
    MMDApp& app;Kind kind;unsigned slot;std::uint64_t identity,generation=1;std::size_t capacity;int initialFrame;
    std::uint32_t beforeLast,afterLast,beforeCursor;unsigned char beforeActive;bool published=false,preserveSelection=false;
    std::array<double,12> initialPose{};std::array<bool,12> poseOwned{};
    mdl::AccessoryRecord* object=nullptr;std::shared_ptr<RetainedSceneObject> owner;std::uint64_t objectGeneration=0;
    curve_host::AuthoredReplacement authoring;
    std::vector<K> original;
    struct Patch{unsigned slot;K before,after;};std::vector<Patch> patches;
    std::vector<FrameMark> beforeMarks,afterMarks;
    std::vector<Parent> parents;
    WriteChange(MMDApp& a,Kind k,unsigned s,const std::vector<curve_editor::Channel>& channels):app(a),kind(k),slot(s),
        identity(mdl::KeyPoolIdentity(Pool<K>(a,s))),capacity(mdl::KeyPoolCapacity(Pool<K>(a,s))),initialFrame(a.CurrentFrame()),
        beforeLast(a.LastRegisteredFrame()),afterLast(beforeLast),beforeCursor(Cursor<K>(a,s)),beforeActive(Active<K>(a,s)),
        initialPose(ReadPose<K>(a,s)),authoring(a,k,int(s),channels),original(Pool<K>(a,s),Pool<K>(a,s)+capacity){
        if constexpr(std::is_same_v<K,mdl::AccessoryKey>){object=a.AccessorySlot(s);owner=RetainSceneObject(a,MMS_OBJECT_ACCESSORY,s);objectGeneration=SceneObjectIdentity(object).generation;}
        ValidateChain(original);ValidateNativeFallback(original);const auto target=authoring.TargetKeys();
        beforeMarks=CaptureFrameMarks(original.size(),[&](unsigned i)->const K&{return original[i];});
        afterMarks=CaptureFrameMarks(target.capacity,[&](unsigned i)->const K&{return *static_cast<const K*>(target.key(target.context,i));});
        for(std::size_t i=0;i<(std::max)(capacity,target.capacity);++i){const K before=i<capacity?original[i]:K{};
            const K after=i<target.capacity?*static_cast<const K*>(target.key(target.context,unsigned(i))):K{};
            if(!Equal(before,after))patches.push_back({unsigned(i),before,after});
            if(!i||before.frame)ParentKey(a,before,parents);if(i<target.capacity){ParentKey(a,after,parents);afterLast=(std::max)(afterLast,after.frame);}}
        const auto& before=authoring.BeforeChannels();
        for(std::size_t c=0;c<channels.size();++c){const auto& x=before[c];const auto& y=channels[c];
            poseOwned[c]=x.track.keys.size()!=y.track.keys.size()||x.nativeControls!=y.nativeControls||x.controlGeometry!=y.controlGeometry||
                x.interpolationGroup!=y.interpolationGroup||x.sample.target_type()!=y.sample.target_type()||!EquivalentNativeFreeze(x,y);
            for(std::size_t i=0;i<x.track.keys.size()&&!poseOwned[c];++i){const auto& p=x.track.keys[i];const auto& q=y.track.keys[i];
                poseOwned[c]=p.frame!=q.frame||p.value!=q.value||p.inSlope!=q.inSlope||p.outSlope!=q.outSlope||p.inWeight!=q.inWeight||p.outWeight!=q.outWeight||
                    p.interpolation!=q.interpolation||p.tangent!=q.tangent;}}
    }
    bool Structure()const noexcept{
        if(mdl::KeyPoolIdentity(Pool<K>(app,slot))!=identity||mdl::KeyPoolCapacity(Pool<K>(app,slot))<capacity||!ExclusivePool<K>(app,kind,slot))return false;
        if(owner&&(!owner->Present()||app.AccessorySlot(slot)!=object||!SceneObjectPresent(object,owner->Id())))return false;
        for(const auto& parent:parents)if(!parent.Current(app))return false;return true;
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<WriteChange> source;bool forward,first,consumed=false,preserveSelection=false;std::uint64_t generation;
        int frame;K* pool;K* grown=nullptr;std::size_t capacity;std::vector<K> current,target;
        std::array<double,12> beforePose,afterPose;std::uint32_t last,cursor,targetCursor;unsigned char active,targetActive;
        std::unique_ptr<curve_host::AuthoredReplacement::Target> curves;
        std::vector<Parent> parents;std::vector<std::uint64_t> parentGenerations;std::uint64_t objectGeneration=0;
        Candidate(std::shared_ptr<WriteChange> s,bool f):source(std::move(s)),forward(f),first(!source->published),generation(source->generation),
            frame(source->app.CurrentFrame()),pool(Pool<K>(source->app,source->slot)),capacity(mdl::KeyPoolCapacity(pool)),current(pool,pool+capacity),target(current),
            beforePose(ReadPose<K>(source->app,source->slot)),afterPose(beforePose),last(source->app.LastRegisteredFrame()),cursor(Cursor<K>(source->app,source->slot)),
            targetCursor(0),active(Active<K>(source->app,source->slot)),targetActive(f?1:source->beforeActive){}
        ~Candidate(){if(grown)mdl::KeyPoolDestroy(grown);}
        bool IsCurrent()const noexcept override{
            if(consumed||source->generation!=generation||!source->Structure()||!source->authoring.IsCurrent()||source->app.CurrentFrame()!=frame||
               Pool<K>(source->app,source->slot)!=pool||mdl::KeyPoolCapacity(pool)!=capacity||std::memcmp(pool,current.data(),capacity*sizeof(K))||
               ReadPose<K>(source->app,source->slot)!=beforePose||source->app.LastRegisteredFrame()!=last||Cursor<K>(source->app,source->slot)!=cursor||
               Active<K>(source->app,source->slot)!=active||!curves||!curves->IsCurrent())return false;
            if(source->owner&&SceneObjectIdentity(source->object).generation!=objectGeneration)return false;
            for(std::size_t i=0;i<parents.size();++i)if(!parents[i].Current(source->app)||SceneObjectIdentity(parents[i].model).generation!=parentGenerations[i])return false;
            return true;
        }
        void Publish()noexcept override{
            if(!IsCurrent())std::terminate();
            if(grown){mdl::KeyPoolCommitGrowth(Pool<K>(source->app,source->slot),grown);grown=nullptr;}
            auto* keys=Pool<K>(source->app,source->slot);
            for(const auto& patch:source->patches)keys[patch.slot]=target[patch.slot];
            curves->Publish(source->owner?static_cast<void*>(source->object):keys);WritePose<K>(source->app,source->slot,afterPose);
            if(source->beforeLast!=source->afterLast)source->app.LastRegisteredFrame()=forward?source->afterLast:source->beforeLast;
            Cursor<K>(source->app,source->slot)=targetCursor;Active<K>(source->app,source->slot)=targetActive;
            source->preserveSelection=preserveSelection;source->published=true;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();if(!Structure()||!authoring.IsCurrent()||generation==UINT64_MAX)return MMS_E_STALE;
        return Boundary([&]()->MmsResult{
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            if(!published&&(!forward||app.CurrentFrame()!=initialFrame||ReadPose<K>(app,slot)!=initialPose||Cursor<K>(app,slot)!=beforeCursor||Active<K>(app,slot)!=beforeActive))return MMS_E_STALE;
            if(!published&&app.LastRegisteredFrame()!=beforeLast)return MMS_E_STALE;
            if(beforeLast!=afterLast&&app.LastRegisteredFrame()!=(forward?beforeLast:afterLast))return MMS_E_STALE;
            if(!published&&owner&&SceneObjectIdentity(object).generation!=objectGeneration)return MMS_E_STALE;
            for(const auto& parent:parents)if(!published&&SceneObjectIdentity(parent.model).generation!=parent.initialGeneration)return MMS_E_STALE;
            auto candidate=std::make_unique<Candidate>(this->shared_from_this(),forward);
            if(!Finite(candidate->beforePose))return MMS_E_ARGUMENT;
            if(!published&&(candidate->capacity!=original.size()||std::memcmp(candidate->pool,original.data(),original.size()*sizeof(K))))return MMS_E_STALE;
            std::size_t required=candidate->capacity;
            for(const auto& patch:patches){const auto live=patch.slot<candidate->capacity?candidate->pool[patch.slot]:K{};
                if(!Equal(live,forward?patch.before:patch.after,!published))return MMS_E_STALE;required=(std::max)(required,std::size_t(patch.slot)+1);}
            candidate->target.resize(required);
            for(const auto& patch:patches)candidate->target[patch.slot]=forward?patch.after:patch.before;
            ValidateChain(candidate->target);
            if(published){
                const auto currentMarks=CaptureFrameMarks(candidate->current.size(),[&](unsigned i)->const K&{return candidate->current[i];});
                candidate->preserveSelection=preserveSelection||currentMarks!=(forward?beforeMarks:afterMarks);
                if(candidate->preserveSelection){unsigned index=0;
                    // Empty spare slots also have frame zero. Project only
                    // actual chain members, never a root mark into spare data.
                    for(;;){auto& key=candidate->target[index];
                        const auto at=std::lower_bound(currentMarks.begin(),currentMarks.end(),key.frame,[](const auto& mark,unsigned frame){return mark.frame<frame;});
                        key.selected=at!=currentMarks.end()&&at->frame==key.frame?at->selected:0;
                        if(!key.next)break;index=key.next;}
                }
            }
            if(required>candidate->capacity){candidate->grown=mdl::KeyPoolCreate<K>(required);std::memcpy(candidate->grown,candidate->pool,candidate->capacity*sizeof(K));}
            curve_host::NativeKeyView view{&candidate->target,candidate->target.size(),[](const void* p,unsigned i)noexcept->const void*{const auto& v=*static_cast<const std::vector<K>*>(p);return i<v.size()?&v[i]:nullptr;}};
            candidate->curves=authoring.Prepare(view,forward);std::array<double,12> values{};
            if(candidate->curves->Authored()){
                if(!candidate->curves->Sample(candidate->frame,values))return MMS_E_ARGUMENT;values=AuthoredPose<K>(values);
                if constexpr(std::is_same_v<K,mdl::GravityKey>){const auto native=NativePose(candidate->target,unsigned(candidate->frame));
                    values[5]=(std::uint32_t(native[5])&0xffffff00u)|std::uint8_t(values[5]);}
            }else values=NativePose(candidate->target,unsigned(candidate->frame));
            for(std::size_t c=0;c<poseOwned.size();++c)if(poseOwned[c])candidate->afterPose[c]=values[c];
            if(!Finite(candidate->afterPose))return MMS_E_ARGUMENT;
            unsigned index=0;while(candidate->target[index].frame<unsigned(candidate->frame)&&candidate->target[index].next)index=candidate->target[index].next;
            candidate->targetCursor=candidate->frame==initialFrame?(forward?0:beforeCursor):index;
            candidate->parents=parents;
            if constexpr(std::is_same_v<K,mdl::CameraKey>){ParentAt(app,int(candidate->beforePose[9]),int(candidate->beforePose[10]),candidate->parents);ParentAt(app,int(candidate->afterPose[9]),int(candidate->afterPose[10]),candidate->parents);}
            if constexpr(std::is_same_v<K,mdl::AccessoryKey>){ParentAt(app,int(candidate->beforePose[10]),int(candidate->beforePose[11]),candidate->parents);ParentAt(app,int(candidate->afterPose[10]),int(candidate->afterPose[11]),candidate->parents);}
            for(const auto& parent:candidate->parents)candidate->parentGenerations.push_back(SceneObjectIdentity(parent.model).generation);
            if(owner)candidate->objectGeneration=SceneObjectIdentity(object).generation;
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);Add(bytes,original.capacity(),sizeof(K));Add(bytes,patches.capacity(),sizeof(Patch));
        Add(bytes,parents.capacity(),sizeof(Parent));Add(bytes,beforeMarks.capacity(),sizeof(FrameMark));Add(bytes,afterMarks.capacity(),sizeof(FrameMark));Add(bytes,authoring.RetainedBytes());return bytes;}
};
template<class K>MmsResult Plan(MMDApp& app,Kind kind,unsigned slot,const std::vector<curve_editor::Channel>& channels,std::shared_ptr<HistoryChange>& out){
    if(!mdl::KeyPoolIdentity(Pool<K>(app,slot))||!ExclusivePool<K>(app,kind,slot))return MMS_E_NOT_READY;
    auto change=std::make_shared<WriteChange<K>>(app,kind,slot,channels);if(change->authoring.Changed())out=std::move(change);return MMS_OK;
}
}
MmsResult PlanGlobalCurveWrite(MMDApp& app,Kind kind,int slot,const std::vector<curve_editor::Channel>& channels,std::shared_ptr<HistoryChange>& out)noexcept{
    if(out||int(kind)<0||int(kind)>4||slot<0||slot>=kModelSlotCount||app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(kind==Kind::Accessory&&!app.AccessorySlot(slot))return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{switch(kind){
        case Kind::Camera:return Plan<mdl::CameraKey>(app,kind,0,channels,out);
        case Kind::Light:return Plan<mdl::LightKey>(app,kind,0,channels,out);
        case Kind::Shadow:return Plan<mdl::SelfShadowKey>(app,kind,0,channels,out);
        case Kind::Gravity:return Plan<mdl::GravityKey>(app,kind,0,channels,out);
        default:return Plan<mdl::AccessoryKey>(app,kind,unsigned(slot),channels,out);}});
}
}
#endif
