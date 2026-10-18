#include "native_global_key_delete.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace mikudancestudio::plugins {
namespace {
// Native lane layout, arithmetic and parent ownership match the existing
// private curve-write plans; deletion changes only selected slot payloads.
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

template<class K>void ResetDeleted(MMDApp& app,K& key,bool head)noexcept{
    key.selected=0;
    if constexpr(std::is_same_v<K,mdl::CameraKey>){
        key.distance=-45.f;key.eye[0]=key.eye[2]=0;key.eye[1]=head?10.f:0.f;
        std::memset(key.target,0,sizeof(key.target));key.parentModel=-1;key.parentBone=0;
        std::memset(key.interpolation,20,12);std::memset(reinterpret_cast<unsigned char*>(key.interpolation)+12,107,12);
        // The native delete does not reset FOV, perspective or reserved bytes.
    }else if constexpr(std::is_same_v<K,mdl::LightKey>){
        if(head){key.direction[0]=-.5f;key.direction[1]=-1.f;key.direction[2]=.5f;
            key.color[0]=key.color[1]=key.color[2]=.602f;}
        else{std::memset(key.direction,0,sizeof(key.direction));std::memset(key.color,0,sizeof(key.color));}
    }else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>){
        const auto* renderer=app.Renderer();key.mode=(renderer&&renderer->postProcessEnabled)||app.state.selfShadowMode?1:0;
        key.distance=.01125f;
    }else if constexpr(std::is_same_v<K,mdl::GravityKey>){
        key.noiseEnabled=0;key.noise=10;key.acceleration=9.8000002f;
        key.direction[0]=key.direction[2]=0;key.direction[1]=-1.f;
    }else{key.parentBone=0;key.visible=1;key.shadowEnabled=0;key.parentModel=-1;
        std::memset(key.position,0,sizeof(key.position));std::memset(key.rotation,0,sizeof(key.rotation));key.opacity=key.scale=1.f;}
}
template<class K>constexpr std::uint32_t ResetRootChannels()noexcept{
    // Match ResetDeleted's field ownership. In particular, native camera
    // deletion preserves FOV and perspective, including their authored data.
    if constexpr(std::is_same_v<K,mdl::CameraKey>)return 0x67fu;
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)return 0x3u;
    else if constexpr(std::is_same_v<K,mdl::AccessoryKey>)return 0xfffu;
    else return 0x3fu;
}
class PreparedDeletePart {
public:
    virtual ~PreparedDeletePart()=default;
    virtual bool IsCurrent()const noexcept=0;
    // The aggregate validates every participant before the first publication.
    virtual void PublishOwned()noexcept=0;
};
class DeletePart {
public:
    virtual ~DeletePart()=default;
    virtual MmsResult Prepare(bool,std::unique_ptr<PreparedDeletePart>&)=0;
    virtual std::uint64_t RetainedBytes()const noexcept=0;
    virtual bool Changed()const noexcept=0;
};
template<class K>struct FamilyDelete final:DeletePart,std::enable_shared_from_this<FamilyDelete<K>>{
    MMDApp& app;Kind kind;unsigned slot;std::uint64_t identity,generation=1;std::size_t capacity;
    int initialFrame;std::uint32_t beforeCursor;unsigned char beforeActive;
    bool published=false,preserveSelection=false,rootReset=false,dataChanged=false;
    std::array<double,12> initialPose;
    mdl::AccessoryRecord* object=nullptr;std::shared_ptr<RetainedSceneObject> owner;std::uint64_t objectGeneration=0;
    curve_host::TrackAuthoring authoring,initialAuthoring;
    std::vector<K> original,expected;
    struct Patch{unsigned slot;K before,after;};std::vector<Patch> patches;
    std::vector<std::pair<int,int>> frameMap;
    std::vector<FrameMark> beforeMarks,afterMarks;
    std::vector<Parent> parents;
    FamilyDelete(MMDApp& a,Kind k,unsigned s,bool fallback):app(a),kind(k),slot(s),
        identity(mdl::KeyPoolIdentity(Pool<K>(a,s))),capacity(mdl::KeyPoolCapacity(Pool<K>(a,s))),initialFrame(a.CurrentFrame()),
        beforeCursor(Cursor<K>(a,s)),beforeActive(Active<K>(a,s)),initialPose(ReadPose<K>(a,s)),
        authoring(a,k,int(s)),initialAuthoring(a,k,int(s)),original(Pool<K>(a,s),Pool<K>(a,s)+capacity),expected(original){
        if constexpr(std::is_same_v<K,mdl::AccessoryKey>){object=a.AccessorySlot(s);owner=RetainSceneObject(a,MMS_OBJECT_ACCESSORY,s);objectGeneration=SceneObjectIdentity(object).generation;}
        ValidateChain(original);ValidateNativeFallback(original);
        std::vector<unsigned char> member(capacity);unsigned index=0;
        for(;;){member[index]=1;ParentKey(a,original[index],parents);if(!original[index].next)break;index=original[index].next;}
        ParentPose(initialPose,parents);
        std::set<int> removed;
        for(std::size_t i=0;i<capacity;++i){auto& key=expected[i];
            const bool selected=fallback?(member[i]&&key.frame==unsigned(initialFrame)):original[i].selected!=0;
            if(!selected)continue;
            if(!member[i])throw std::invalid_argument("marked spare global key");
            if(!i)rootReset=true;
            else{removed.insert(int(key.frame));expected[key.previous].next=key.next;expected[key.next].previous=key.previous;
                key.frame=key.previous=key.next=0;}
            ResetDeleted(a,key,i==0);
        }
        ValidateChain(expected);ValidateNativeFallback(expected);
        std::vector<int> frames;authoring.RetimingFrames(frames);
        for(std::size_t i=0;i<capacity;++i)if(!i||member[i])frames.push_back(int(original[i].frame));
        std::sort(frames.begin(),frames.end());frames.erase(std::unique(frames.begin(),frames.end()),frames.end());
        for(const int frame:frames)frameMap.push_back({frame,removed.count(frame)?-1:frame});
        for(std::size_t i=0;i<capacity;++i)if(!Equal(original[i],expected[i])){
            patches.push_back({unsigned(i),original[i],expected[i]});dataChanged|=!Equal(original[i],expected[i],false);}
        beforeMarks=CaptureFrameMarks(original.size(),[&](unsigned i)->const K&{return original[i];});
        afterMarks=CaptureFrameMarks(expected.size(),[&](unsigned i)->const K&{return expected[i];});
        if(rootReset){const auto view=View(expected);auto target=authoring.PrepareRetimed(view,frameMap,true,ResetRootChannels<K>(),true);
            const auto source=authoring.PrepareRetimed(View(original),frameMap,false);
            std::array<double,12> before{},after{};
            if(target->Authored()&&(!source->Sample(0,before)||!target->Sample(0,after)))throw std::invalid_argument("deleted authored root");
            dataChanged|=before!=after;
        }
    }
    static curve_host::NativeKeyView View(const std::vector<K>& keys)noexcept{
        return {&keys,keys.size(),[](const void* value,unsigned i)noexcept->const void*{
            const auto& rows=*static_cast<const std::vector<K>*>(value);return i<rows.size()?&rows[i]:nullptr;}};
    }
    void ParentPose(const std::array<double,12>& pose,std::vector<Parent>& out){
        if constexpr(std::is_same_v<K,mdl::CameraKey>)ParentAt(app,int(pose[9]),int(pose[10]),out);
        else if constexpr(std::is_same_v<K,mdl::AccessoryKey>)ParentAt(app,int(pose[10]),int(pose[11]),out);
    }
    bool Structure()const noexcept{
        if(mdl::KeyPoolIdentity(Pool<K>(app,slot))!=identity||mdl::KeyPoolCapacity(Pool<K>(app,slot))<capacity||!ExclusivePool<K>(app,kind,slot))return false;
        if(owner&&(!owner->Present()||app.AccessorySlot(slot)!=object||!SceneObjectPresent(object,owner->Id())))return false;
        for(const auto& parent:parents)if(!parent.Current(app))return false;return true;
    }
    struct Candidate final:PreparedDeletePart{
        std::shared_ptr<FamilyDelete> source;bool forward,consumed=false,preserveSelection=false;
        std::uint64_t generation,objectGeneration=0;int frame;K* pool;std::size_t capacity;
        std::vector<K> current,target;std::array<double,12> beforePose,afterPose;
        std::uint32_t cursor,targetCursor;unsigned char active;
        std::vector<Parent> parents;std::vector<std::uint64_t> parentGenerations;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves,sourceCurves;
        Candidate(std::shared_ptr<FamilyDelete> s,bool f):source(std::move(s)),forward(f),generation(source->generation),
            frame(source->app.CurrentFrame()),pool(Pool<K>(source->app,source->slot)),capacity(mdl::KeyPoolCapacity(pool)),current(pool,pool+capacity),target(current),
            beforePose(ReadPose<K>(source->app,source->slot)),afterPose(beforePose),cursor(Cursor<K>(source->app,source->slot)),targetCursor(cursor),active(Active<K>(source->app,source->slot)){}
        bool IsCurrent()const noexcept override{
            if(consumed||source->generation!=generation||!source->Structure()||!source->authoring.IsCurrent()||source->app.CurrentFrame()!=frame||
                Pool<K>(source->app,source->slot)!=pool||mdl::KeyPoolCapacity(pool)!=capacity||std::memcmp(pool,current.data(),capacity*sizeof(K))||
                ReadPose<K>(source->app,source->slot)!=beforePose||Cursor<K>(source->app,source->slot)!=cursor||Active<K>(source->app,source->slot)!=active||
                !curves||!curves->IsCurrent()||!sourceCurves||!sourceCurves->IsCurrent())return false;
            if(source->owner&&SceneObjectIdentity(source->object).generation!=objectGeneration)return false;
            for(std::size_t i=0;i<parents.size();++i)if(!parents[i].Current(source->app)||SceneObjectIdentity(parents[i].model).generation!=parentGenerations[i])return false;
            return true;
        }
        void PublishOwned()noexcept override{
            for(const auto& patch:source->patches)pool[patch.slot]=target[patch.slot];
            // A later independent timeline selection is projected by frame,
            // including marks on untouched slots, after the payload exchange.
            if(preserveSelection){unsigned index=0;for(;;){pool[index].selected=target[index].selected;if(!pool[index].next)break;index=pool[index].next;}}
            curves->Publish(source->owner?static_cast<void*>(source->object):pool);
            WritePose<K>(source->app,source->slot,afterPose);Cursor<K>(source->app,source->slot)=targetCursor;
            source->preserveSelection=preserveSelection;source->published=true;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedDeletePart>& out)override{
        out.reset();if(!Structure()||!authoring.IsCurrent()||generation==UINT64_MAX)return MMS_E_STALE;
        return Boundary([&]()->MmsResult{
            if(!published&&(!forward||app.CurrentFrame()!=initialFrame||ReadPose<K>(app,slot)!=initialPose||
                Cursor<K>(app,slot)!=beforeCursor||Active<K>(app,slot)!=beforeActive||!initialAuthoring.CapturedCurrent()))return MMS_E_STALE;
            if(!published&&owner&&SceneObjectIdentity(object).generation!=objectGeneration)return MMS_E_STALE;
            for(const auto& parent:parents)if(!published&&SceneObjectIdentity(parent.model).generation!=parent.initialGeneration)return MMS_E_STALE;
            auto candidate=std::make_unique<Candidate>(this->shared_from_this(),forward);
            if(!Finite(candidate->beforePose))return MMS_E_ARGUMENT;
            const auto& expectedSource=forward?original:expected;
            for(std::size_t i=0;i<capacity;++i)if(!Equal(candidate->current[i],expectedSource[i],!published))return MMS_E_STALE;
            ValidateChain(candidate->current);ValidateNativeFallback(candidate->current);
            for(const auto& patch:patches)candidate->target[patch.slot]=forward?patch.after:patch.before;
            ValidateChain(candidate->target);ValidateNativeFallback(candidate->target);
            if(published){const auto marks=CaptureFrameMarks(candidate->current.size(),[&](unsigned i)->const K&{return candidate->current[i];});
                candidate->preserveSelection=preserveSelection||marks!=(forward?beforeMarks:afterMarks);
                if(candidate->preserveSelection){unsigned index=0;for(;;){auto& key=candidate->target[index];
                    const auto at=std::lower_bound(marks.begin(),marks.end(),key.frame,[](const auto& mark,unsigned frame){return mark.frame<frame;});
                    key.selected=at!=marks.end()&&at->frame==key.frame?at->selected:0;if(!key.next)break;index=key.next;}}
            }
            const auto resetChannels=rootReset?ResetRootChannels<K>():0u;
            candidate->curves=authoring.PrepareRetimed(View(candidate->target),frameMap,forward,resetChannels,true);
            candidate->sourceCurves=authoring.PrepareRetimed(View(candidate->current),frameMap,!forward,resetChannels,true);
            const auto sample=[&](const auto& curves,const auto& keys){
                if(!curves->Authored())return NativePose<K>(keys,unsigned(candidate->frame));
                std::array<double,12> pose{};if(!curves->Sample(candidate->frame,pose))throw std::invalid_argument("deleted track sample");
                pose=AuthoredPose<K>(pose);
                if constexpr(std::is_same_v<K,mdl::GravityKey>){const auto native=NativePose<K>(keys,unsigned(candidate->frame));
                    pose[5]=(std::uint32_t(native[5])&0xffffff00u)|std::uint8_t(pose[5]);}
                return pose;
            };
            if(candidate->beforePose==sample(candidate->sourceCurves,candidate->current))candidate->afterPose=sample(candidate->curves,candidate->target);
            if(!Finite(candidate->afterPose))return MMS_E_ARGUMENT;
            unsigned index=0;while(candidate->target[index].frame<unsigned(candidate->frame)&&candidate->target[index].next)index=candidate->target[index].next;
            candidate->targetCursor=!forward&&candidate->frame==initialFrame?beforeCursor:index;
            candidate->parents=parents;ParentPose(candidate->beforePose,candidate->parents);ParentPose(candidate->afterPose,candidate->parents);
            for(const auto& parent:candidate->parents)candidate->parentGenerations.push_back(SceneObjectIdentity(parent.model).generation);
            if(owner)candidate->objectGeneration=SceneObjectIdentity(object).generation;
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    bool Changed()const noexcept override{return dataChanged;}
    std::uint64_t RetainedBytes()const noexcept override{
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);Add(bytes,original.capacity()+expected.capacity(),sizeof(K));
        Add(bytes,patches.capacity(),sizeof(Patch));Add(bytes,frameMap.capacity(),sizeof(frameMap[0]));
        Add(bytes,beforeMarks.capacity()+afterMarks.capacity(),sizeof(FrameMark));Add(bytes,parents.capacity(),sizeof(Parent));
        Add(bytes,authoring.RetainedBytes());Add(bytes,initialAuthoring.RetainedBytes());return bytes;
    }
};
struct GlobalDelete final:HistoryChange,std::enable_shared_from_this<GlobalDelete>{
    MMDApp& app;int initialFrame;std::uint32_t initialLast;std::uint64_t generation=1;bool published=false,fallback;
    std::array<unsigned char,259> initialRows{};
    std::vector<std::shared_ptr<DeletePart>> parts;
    static std::array<unsigned char,259> Rows(MMDApp& app)noexcept{
        std::array<unsigned char,259> result{};for(unsigned i=0;i<4;++i)result[i]=app.state.globalTrackSelected[i];
        for(unsigned i=0;i<kModelSlotCount;++i)if(const auto* object=app.AccessorySlot(i))result[4+i]=object->rowSelected;return result;
    }
    explicit GlobalDelete(MMDApp& a,bool f):app(a),initialFrame(a.CurrentFrame()),initialLast(a.LastRegisteredFrame()),fallback(f),initialRows(Rows(a)){}
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<GlobalDelete> source;std::uint64_t generation;int frame;std::uint32_t last;bool consumed=false;
        std::array<unsigned char,259> rows{};
        std::vector<std::unique_ptr<PreparedDeletePart>> parts;
        explicit Candidate(std::shared_ptr<GlobalDelete> s):source(std::move(s)),generation(source->generation),frame(source->app.CurrentFrame()),last(source->app.LastRegisteredFrame()),rows(Rows(source->app)){}
        bool IsCurrent()const noexcept override{
            if(consumed||source->generation!=generation||source->app.CurrentFrame()!=frame||source->app.LastRegisteredFrame()!=last)return false;
            if(source->fallback&&!source->published&&Rows(source->app)!=rows)return false;
            for(const auto& part:parts)if(!part->IsCurrent())return false;return true;
        }
        void Publish()noexcept override{
            if(!IsCurrent())std::terminate();for(const auto& part:parts)part->PublishOwned();
            source->published=true;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        if(!published&&(!forward||app.CurrentFrame()!=initialFrame||app.LastRegisteredFrame()!=initialLast))return MMS_E_STALE;
        if(!published&&fallback&&Rows(app)!=initialRows)return MMS_E_STALE;
        return Boundary([&]()->MmsResult{auto candidate=std::make_unique<Candidate>(shared_from_this());
            for(const auto& part:parts){std::unique_ptr<PreparedDeletePart> prepared;if(auto result=part->Prepare(forward,prepared))return result;
                if(!prepared)return MMS_E_NOT_READY;candidate->parts.push_back(std::move(prepared));}
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;});
    }
    std::uint64_t RetainedBytes()const noexcept override{std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);Add(bytes,parts.capacity(),sizeof(parts[0]));
        for(const auto& part:parts)Add(bytes,part->RetainedBytes());return bytes;}
};
template<class K>bool HasMarks(K* keys){const auto count=mdl::KeyPoolCapacity(keys);for(std::size_t i=0;i<count;++i)if(keys[i].selected)return true;return false;}
bool FallbackRow(MMDApp& app,Kind kind,unsigned slot)noexcept{
    if(kind==Kind::Accessory)return app.AccessorySlot(slot)&&app.AccessorySlot(slot)->rowSelected;
    const auto track=kind==Kind::Camera?GlobalTimelineTrack::Camera:kind==Kind::Light?GlobalTimelineTrack::Light:
        kind==Kind::Shadow?GlobalTimelineTrack::SelfShadow:GlobalTimelineTrack::Gravity;
    return app.GlobalTrackSelected(track)!=0;
}
}
MmsResult PlanGlobalKeyDeletion(MMDApp& app,std::shared_ptr<HistoryChange>& out,bool selectedAtCurrentFrame)noexcept{
    out.reset();if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        const bool explicitMarks=HasMarks(app.CameraKeys())||HasMarks(app.LightKeys())||HasMarks(app.ShadowKeys())||HasMarks(app.GravityKeys())||
            [&]{for(unsigned i=0;i<kModelSlotCount;++i)if(HasMarks(app.AccessoryKeys(i)))return true;return false;}();
        const bool fallback=!explicitMarks&&selectedAtCurrentFrame&&app.CurrentFrame()!=0;
        if(!explicitMarks&&!fallback)return MMS_OK;
        auto change=std::make_shared<GlobalDelete>(app,fallback);bool changed=false;
        const auto add=[&](auto* pool,Kind kind,unsigned slot){using K=std::remove_pointer_t<decltype(pool)>;
            const bool wanted=HasMarks(pool)||(fallback&&FallbackRow(app,kind,slot));if(!wanted)return;
            if(!mdl::KeyPoolIdentity(pool)||!ExclusivePool<K>(app,kind,slot))throw std::invalid_argument("unowned or aliased global deletion pool");
            if constexpr(std::is_same_v<K,mdl::AccessoryKey>)if(!app.AccessorySlot(slot))throw std::invalid_argument("marked absent accessory");
            auto part=std::make_shared<FamilyDelete<K>>(app,kind,slot,fallback);changed|=part->Changed();change->parts.push_back(std::move(part));
        };
        add(app.CameraKeys(),Kind::Camera,0);add(app.LightKeys(),Kind::Light,0);add(app.ShadowKeys(),Kind::Shadow,0);add(app.GravityKeys(),Kind::Gravity,0);
        for(unsigned slot=0;slot<kModelSlotCount;++slot)add(app.AccessoryKeys(slot),Kind::Accessory,slot);
        if(changed)out=std::move(change);return MMS_OK;
    });
}
}
#endif
