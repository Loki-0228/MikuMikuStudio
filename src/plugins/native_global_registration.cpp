#include "native_global_registration.hpp"
#include "native_global_paste.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace mikudancestudio::plugins {
namespace {
void AddBytes(std::uint64_t& bytes,std::uint64_t n,std::uint64_t stride=1) noexcept {
    bytes=stride&&n>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+n*stride;
}
struct SelectionPool {
    void* pointer=nullptr;std::uint64_t identity=0;std::size_t capacity=0,stride=0,offset=0;
    unsigned char& Mark(std::size_t index) const noexcept {
        return *(static_cast<unsigned char*>(pointer)+index*stride+offset);
    }
};
template<class K> SelectionPool Describe(K* keys) {
    if(keys&&!mdl::KeyPoolIdentity(keys))throw std::invalid_argument("borrowed global selection pool");
    return {keys,mdl::KeyPoolIdentity(keys),mdl::KeyPoolCapacity(keys),sizeof(K),offsetof(K,selected)};
}
SelectionPool SelectionAt(MMDApp& app,unsigned index) {
    switch(index){case 0:return Describe(app.CameraKeys());case 1:return Describe(app.LightKeys());
    case 2:return Describe(app.ShadowKeys());case 3:return Describe(app.GravityKeys());
    default:return Describe(app.AccessoryKeys(index-4));}
}
struct Mark {std::uint32_t slot;unsigned char value;};
}
struct GlobalKeySelection::Impl {
    MMDApp& app;
    struct Pool {std::uint64_t identity=0;std::size_t capacity=0;std::vector<Mark> marks;};
    std::array<Pool,259> pools;
    explicit Impl(MMDApp& a):app(a){
        std::vector<std::pair<std::uintptr_t,std::uintptr_t>> ranges;
        for(unsigned index=0;index<pools.size();++index){
            const auto live=SelectionAt(app,index);auto& pool=pools[index];pool.identity=live.identity;pool.capacity=live.capacity;
            const auto begin=reinterpret_cast<std::uintptr_t>(live.pointer);
            if(live.capacity>(UINTPTR_MAX-begin)/live.stride)throw std::invalid_argument("selection storage overflow");
            if(live.pointer)ranges.emplace_back(begin,begin+live.capacity*live.stride);
            for(std::size_t slot=0;slot<live.capacity;++slot)if(live.Mark(slot))pool.marks.push_back({std::uint32_t(slot),live.Mark(slot)});
        }
        std::sort(ranges.begin(),ranges.end());
        for(std::size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)throw std::invalid_argument("aliased selection pools");
    }
    bool Structure() const noexcept {
        try {for(unsigned i=0;i<pools.size();++i){const auto live=SelectionAt(app,i);
            if(live.identity!=pools[i].identity||live.capacity<pools[i].capacity)return false;}return true;
        }catch(...){return false;}
    }
};
struct GlobalKeySelection::Candidate::Impl {
    std::shared_ptr<GlobalKeySelection::Impl> source;bool forward,published=false;
    std::array<SelectionPool,259> pools;
    std::array<std::vector<unsigned char>,259> marks;
    Impl(std::shared_ptr<GlobalKeySelection::Impl> s,bool f,bool initial):source(std::move(s)),forward(f){
        if(!source->Structure())throw std::invalid_argument("changed selection pool");
        for(unsigned i=0;i<pools.size();++i){pools[i]=SelectionAt(source->app,i);marks[i].reserve(pools[i].capacity);
            std::size_t saved=0;
            for(std::size_t slot=0;slot<pools[i].capacity;++slot){
                const auto value=pools[i].Mark(slot);marks[i].push_back(value);
                if(initial){const auto& before=source->pools[i].marks;
                    const auto expected=saved<before.size()&&before[saved].slot==slot?before[saved++].value:0;
                    if(value!=expected)throw std::invalid_argument("changed initial selection");}
            }
        }
    }
    bool Current() const noexcept {
        if(published||!source->Structure())return false;
        try {for(unsigned i=0;i<pools.size();++i){const auto live=SelectionAt(source->app,i);
            if(live.pointer!=pools[i].pointer||live.capacity!=pools[i].capacity)return false;
            for(std::size_t slot=0;slot<live.capacity;++slot)if(live.Mark(slot)!=marks[i][slot])return false;
        }return true;}catch(...){return false;}
    }
};
GlobalKeySelection::GlobalKeySelection(MMDApp& app):impl_(std::make_shared<Impl>(app)){}
GlobalKeySelection::~GlobalKeySelection()=default;
GlobalKeySelection::Candidate::Candidate(std::unique_ptr<Impl> p):impl_(std::move(p)){}
GlobalKeySelection::Candidate::~Candidate()=default;
std::unique_ptr<GlobalKeySelection::Candidate> GlobalKeySelection::Prepare(bool forward,bool initial) const {
    return std::unique_ptr<Candidate>(new Candidate(std::make_unique<Candidate::Impl>(impl_,forward,initial)));
}
bool GlobalKeySelection::Candidate::IsCurrent() const noexcept{return impl_->Current();}
void GlobalKeySelection::Candidate::Publish() noexcept {
    auto& p=*impl_;if(p.published)std::terminate();
    for(unsigned i=0;i<p.pools.size();++i){const auto live=SelectionAt(p.source->app,i);
        for(std::size_t slot=0;slot<p.source->pools[i].capacity;++slot)live.Mark(slot)=0;
        if(!p.forward)for(const auto& mark:p.source->pools[i].marks)live.Mark(mark.slot)=mark.value;
    }p.published=true;
}
std::uint64_t GlobalKeySelection::RetainedBytes() const noexcept {
    std::uint64_t bytes=sizeof(*this)+sizeof(Impl)+4*sizeof(void*);
    for(const auto& pool:impl_->pools)AddBytes(bytes,pool.marks.capacity(),sizeof(Mark));return bytes;
}

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
template<class K> struct Growth {
    MMDApp& app;K* before;K* grown=nullptr;std::size_t capacity;std::uint64_t identity;
    Growth(MMDApp& a,std::uint64_t required):app(a),before(Pool<K>(a)),capacity(mdl::KeyPoolCapacity(before)),identity(mdl::KeyPoolIdentity(before)){
        if(!identity)throw std::invalid_argument("unowned global track");
        if(required<=capacity)return;if(required>mdl::KeyPoolLimit<K>())throw std::invalid_argument("global key limit");
        const auto target=static_cast<std::size_t>((std::min)(std::uint64_t(mdl::KeyPoolLimit<K>()),(std::max)(required,std::uint64_t(capacity)+capacity/2+256)));
        grown=mdl::KeyPoolCreate<K>(target);std::memcpy(grown,before,capacity*sizeof(K));
    }
    ~Growth(){if(grown)mdl::KeyPoolDestroy(grown);}
    bool Current() const noexcept{return Pool<K>(app)==before&&mdl::KeyPoolIdentity(before)==identity&&mdl::KeyPoolCapacity(before)==capacity;}
    std::size_t Capacity() const noexcept{return grown?mdl::KeyPoolCapacity(grown):capacity;}
    void Publish() noexcept {if(grown){mdl::KeyPoolCommitGrowth(Pool<K>(app),grown);grown=nullptr;}}
};
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
template<class K> struct Registration final:HistoryChange,std::enable_shared_from_this<Registration<K>> {
    MMDApp& app;std::uint64_t identity;std::size_t capacity;int playhead;
    std::uint32_t beforeLast,afterLast;bool published=false;std::uint64_t generation=1;
    Pose beforePose,firstPose;GlobalKeySelection selection;curve_host::TrackAuthoring authoring;
    struct Patch{std::uint32_t slot;K before,after;};std::vector<Patch> patches;
    std::vector<Parent> parents;
    Registration(MMDApp& a):app(a),identity(mdl::KeyPoolIdentity(Pool<K>(a))),capacity(mdl::KeyPoolCapacity(Pool<K>(a))),playhead(a.CurrentFrame()),
        beforeLast(a.LastRegisteredFrame()),afterLast(beforeLast),beforePose(ReadPose<K>(a)),firstPose(beforePose),selection(a),authoring(a,TrackKind<K>()){}
    bool Structure() const noexcept{
        if(mdl::KeyPoolIdentity(Pool<K>(app))!=identity||mdl::KeyPoolCapacity(Pool<K>(app))<capacity)return false;
        for(const auto& parent:parents)if(!parent.Current(app))return false;return true;
    }
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<Registration> source;bool forward,first,consumed=false;std::uint64_t generation;
        int frame;K* pool;std::size_t capacity;Pose before,after;
        std::vector<Dependency<K>> dependencies;std::vector<K> current;
        std::unique_ptr<Growth<K>> growth;std::unique_ptr<GlobalKeySelection::Candidate> selection;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves;std::vector<Parent> parents;
        Candidate(std::shared_ptr<Registration> s,bool f):source(std::move(s)),forward(f),first(!source->published),generation(source->generation),
            frame(source->app.CurrentFrame()),pool(Pool<K>(source->app)),capacity(mdl::KeyPoolCapacity(pool)),before(ReadPose<K>(source->app)),after(before){}
        const K* Target(std::uint32_t slot) const noexcept {
            const auto& patches=source->patches;const auto found=std::lower_bound(patches.begin(),patches.end(),slot,[](const auto& p,unsigned i){return p.slot<i;});
            if(found!=patches.end()&&found->slot==slot)return forward?&found->after:&found->before;
            if(slot<capacity)return &pool[slot];return nullptr;
        }
        bool IsCurrent() const noexcept override {
            try {
                if(consumed||source->generation!=generation||!source->Structure()||source->app.CurrentFrame()!=frame||
                   !(ReadPose<K>(source->app)==before)||Pool<K>(source->app)!=pool||mdl::KeyPoolCapacity(pool)!=capacity||
                   source->app.LastRegisteredFrame()!=(forward?source->beforeLast:source->afterLast)||
                   !growth->Current()||!selection->IsCurrent()||!curves->IsCurrent())return false;
                for(const auto& key:dependencies)if(!SameKey(key.key,pool[key.slot]))return false;
                for(std::size_t i=0;i<source->patches.size();++i){const auto slot=source->patches[i].slot;
                    if(!SameKey(current[i],slot<capacity?pool[slot]:K{}))return false;}
                for(const auto& parent:parents)if(!parent.Current(source->app))return false;return true;
            }catch(...){return false;}
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();growth->Publish();selection->Publish();
            auto* keys=Pool<K>(source->app);
            for(const auto& patch:source->patches){auto target=forward?patch.after:patch.before;
                // The global snapshot owns marks within the original pools;
                // this patch also owns its newly inserted slot after growth.
                const auto mark=patch.slot<source->capacity?keys[patch.slot].selected:patch.before.selected;
                target.selected=forward?patch.after.selected:mark;keys[patch.slot]=target;}
            curves->Publish(keys);source->app.LastRegisteredFrame()=forward?source->afterLast:source->beforeLast;
            if(!first||!(source->beforePose==source->firstPose))WritePose<K>(source->app,after);
            source->published=true;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();if(!Structure()||!authoring.IsCurrent())return MMS_E_STALE;
        if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        if(!published&&(!forward||app.CurrentFrame()!=playhead))return MMS_E_STALE;
        return Boundary([&]()->MmsResult{
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            if(!FinitePose(ReadPose<K>(app)))return MMS_E_ARGUMENT;
            if(app.LastRegisteredFrame()!=(forward?beforeLast:afterLast)||(!published&&!(ReadPose<K>(app)==beforePose)))return MMS_E_STALE;
            auto candidate=std::make_unique<Candidate>(this->shared_from_this(),forward);
            candidate->dependencies=Chain<K>(candidate->capacity,[&](unsigned i){return candidate->pool[i];});
            std::uint64_t needed=capacity;
            for(const auto& patch:patches){const auto live=patch.slot<candidate->capacity?candidate->pool[patch.slot]:K{};
                if(!SameKey(live,forward?patch.before:patch.after))return MMS_E_STALE;
                candidate->current.push_back(live);needed=(std::max)(needed,std::uint64_t(patch.slot)+1);}
            candidate->growth=std::make_unique<Growth<K>>(app,needed);
            try{candidate->selection=selection.Prepare(forward,!published);}
            catch(const std::invalid_argument&){return MMS_E_STALE;}
            curve_host::NativeKeyView view{candidate.get(),candidate->growth->Capacity(),[](const void* p,std::uint32_t slot) noexcept ->const void*{
                return static_cast<const Candidate*>(p)->Target(slot);}};
            candidate->curves=authoring.Prepare(view);
            if(!published)candidate->after=firstPose;
            else if(!candidate->curves->Authored())candidate->after=NativePose<K>(app.CurrentFrame(),[&](unsigned i){return candidate->Target(i);});
            else {std::array<double,12> values{};if(!candidate->curves->Sample(app.CurrentFrame(),values))return MMS_E_ARGUMENT;candidate->after=CurvePose<K>(values);}
            // Finite keys do not guarantee a finite native sample: the light
            // interpolator subtracts in float before its double multiply.
            if(!FinitePose(candidate->after))return MMS_E_ARGUMENT;
            if constexpr(std::is_same_v<K,mdl::CameraKey>){
                const auto parent=[&](int model,int bone){if(model<-1)throw std::invalid_argument("camera parent");if(model>=0)candidate->parents.emplace_back(app,model,bone);};
                parent(candidate->before.parentModel,candidate->before.parentBone);parent(candidate->after.parentModel,candidate->after.parentBone);
                for(const auto& key:candidate->dependencies)parent(key.key.parentModel,key.key.parentBone);
                for(const auto& patch:patches){const auto& key=forward?patch.after:patch.before;if(!patch.slot||key.frame)parent(key.parentModel,key.parentBone);}
            }
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);AddBytes(bytes,patches.capacity(),sizeof(Patch));
        AddBytes(bytes,parents.capacity(),sizeof(Parent));
        AddBytes(bytes,selection.RetainedBytes());AddBytes(bytes,authoring.RetainedBytes());return bytes;
    }
};

// One track family in the global-paste aggregate. Shared marks and the last
// registration frame belong to the aggregate, never to these participants.
template<class K> struct PastePart final:GlobalPastePart,std::enable_shared_from_this<PastePart<K>> {
    MMDApp& app;std::uint64_t identity,generation=1;std::size_t capacity;
    curve_host::TrackAuthoring authoring;
    struct Patch {unsigned slot;K before,after;};std::vector<Patch> patches;
    std::vector<Parent> parents;
    explicit PastePart(MMDApp& a):app(a),identity(mdl::KeyPoolIdentity(Pool<K>(a))),capacity(mdl::KeyPoolCapacity(Pool<K>(a))),authoring(a,TrackKind<K>()){}
    bool Structure() const noexcept {
        if(mdl::KeyPoolIdentity(Pool<K>(app))!=identity||mdl::KeyPoolCapacity(Pool<K>(app))<capacity)return false;
        for(const auto& parent:parents)if(!parent.Current(app))return false;return true;
    }
    struct Candidate final:PreparedGlobalPastePart {
        std::shared_ptr<PastePart> source;bool forward,owned=false,consumed=false;std::uint64_t generation;
        int frame;K* pool;std::size_t capacity;Pose before,after;
        std::vector<Dependency<K>> dependencies;std::vector<K> current;std::vector<Parent> parents;
        std::unique_ptr<Growth<K>> growth;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves,currentCurves;
        Candidate(std::shared_ptr<PastePart> s,bool f):source(std::move(s)),forward(f),generation(source->generation),frame(source->app.CurrentFrame()),
            pool(Pool<K>(source->app)),capacity(mdl::KeyPoolCapacity(pool)),before(ReadPose<K>(source->app)),after(before){}
        const K* Target(unsigned slot) const noexcept {
            const auto& patches=source->patches;const auto found=std::lower_bound(patches.begin(),patches.end(),slot,[](const auto& p,unsigned i){return p.slot<i;});
            if(found!=patches.end()&&found->slot==slot)return forward?&found->after:&found->before;
            return slot<capacity?&pool[slot]:nullptr;
        }
        bool IsCurrent() const noexcept override {
            if(owned||consumed||source->generation!=generation||!source->Structure()||source->app.CurrentFrame()!=frame||
                !(ReadPose<K>(source->app)==before)||Pool<K>(source->app)!=pool||mdl::KeyPoolCapacity(pool)!=capacity||
                !growth||!growth->Current()||!curves||!curves->IsCurrent()||!currentCurves||!currentCurves->IsCurrent())return false;
            for(const auto& entry:dependencies)if(!SameKey(entry.key,pool[entry.slot],true))return false;
            for(std::size_t i=0;i<source->patches.size();++i){const auto index=source->patches[i].slot;
                if(!SameKey(current[i],index<capacity?pool[index]:K{},true))return false;}
            for(const auto& parent:parents)if(!parent.Current(source->app))return false;
            return true;
        }
        void PublishOwned() noexcept override {
            // The aggregate has validated every participant before its first
            // write. Never recheck shared fields after another part publishes.
            if(owned||consumed||source->generation!=generation)std::terminate();
            growth->Publish();auto* keys=Pool<K>(source->app);
            for(const auto& patch:source->patches){auto target=forward?patch.after:patch.before;target.selected=keys[patch.slot].selected;keys[patch.slot]=target;}
            curves->Publish(keys);WritePose<K>(source->app,after);
            ++source->generation;owned=true;
        }
        void PublishSelection() noexcept override {
            if(!owned||consumed)std::terminate();auto* keys=Pool<K>(source->app);
            for(const auto& patch:source->patches){
                if(forward)keys[patch.slot].selected=patch.after.selected;
                else if(patch.slot>=source->capacity)keys[patch.slot].selected=patch.before.selected;
            }
            consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedGlobalPastePart>& out) override {
        out.reset();if(!Structure()||!authoring.IsCurrent())return MMS_E_STALE;
        if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        return Boundary([&]() -> MmsResult {
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum||!FinitePose(ReadPose<K>(app)))return MMS_E_ARGUMENT;
            auto candidate=std::make_unique<Candidate>(this->shared_from_this(),forward);
            candidate->dependencies=Chain<K>(candidate->capacity,[&](unsigned i){return candidate->pool[i];});
            std::uint64_t needed=capacity;
            for(const auto& patch:patches){const auto live=patch.slot<candidate->capacity?candidate->pool[patch.slot]:K{};
                if(!SameKey(live,forward?patch.before:patch.after))return MMS_E_STALE;
                candidate->current.push_back(live);needed=(std::max)(needed,std::uint64_t(patch.slot)+1);}
            candidate->growth=std::make_unique<Growth<K>>(app,needed);
            curve_host::NativeKeyView target{candidate.get(),candidate->growth->Capacity(),[](const void* p,unsigned i) noexcept ->const void* {return static_cast<const Candidate*>(p)->Target(i);}};
            candidate->curves=authoring.Prepare(target);
            curve_host::NativeKeyView current{candidate->pool,candidate->capacity,[](const void* p,unsigned i) noexcept ->const void* {return static_cast<const K*>(p)+i;}};
            candidate->currentCurves=authoring.Prepare(current);
            const auto sample=[&](const auto& curves,auto read){
                if(!curves->Authored())return NativePose<K>(app.CurrentFrame(),read);
                std::array<double,12> values{};if(!curves->Sample(app.CurrentFrame(),values))throw std::invalid_argument("global paste sample");return CurvePose<K>(values);
            };
            const auto evaluated=sample(candidate->currentCurves,[&](unsigned i){return candidate->pool+i;});
            if(!FinitePose(evaluated))return MMS_E_ARGUMENT;
            const auto targetValue=sample(candidate->curves,[&](unsigned i){return candidate->Target(i);});
            if(!FinitePose(targetValue))return MMS_E_ARGUMENT;
            if(evaluated==candidate->before)candidate->after=targetValue;
            if(!FinitePose(candidate->after))return MMS_E_ARGUMENT;
            if constexpr(std::is_same_v<K,mdl::CameraKey>){
                const auto parent=[&](int model,int bone){if(model<-1)throw std::invalid_argument("camera parent");if(model>=0)candidate->parents.emplace_back(app,model,bone);};
                parent(candidate->before.parentModel,candidate->before.parentBone);parent(candidate->after.parentModel,candidate->after.parentBone);
            }
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);AddBytes(bytes,patches.capacity(),sizeof(Patch));AddBytes(bytes,parents.capacity(),sizeof(Parent));AddBytes(bytes,authoring.RetainedBytes());return bytes;
    }
};
template<class K,class S,class Fill> MmsResult PlanPastePart(MMDApp& app,const std::vector<S>& records,unsigned offset,Fill fill,GlobalPasteFamilyResult& out) noexcept {
    out={};if(records.empty())return MMS_OK;
    if(offset>unsigned(frame_limits::maximum))return MMS_E_ARGUMENT;
    if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(!mdl::KeyPoolIdentity(Pool<K>(app)))return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        if(!FinitePose(ReadPose<K>(app)))return MMS_E_ARGUMENT;
        auto* keys=Pool<K>(app);const auto capacity=mdl::KeyPoolCapacity(keys);
        const auto chain=Chain<K>(capacity,[&](unsigned i){return keys[i];});
        auto part=std::make_shared<PastePart<K>>(app);std::map<unsigned,typename PastePart<K>::Patch> planned;
        std::size_t targetCapacity=capacity;
        const auto read=[&](unsigned i) -> K {const auto found=planned.find(i);return found!=planned.end()?found->second.after:i<capacity?keys[i]:K{};};
        const auto write=[&](unsigned i) -> K& {auto found=planned.find(i);if(found==planned.end()){
            const auto before=i<capacity?keys[i]:K{};auto after=before;after.selected=0;found=planned.emplace(i,typename PastePart<K>::Patch{i,before,after}).first;}return found->second.after;};
        GlobalPasteFamilyResult result;
        for(const auto& record:records){unsigned frame;if(!frame_limits::Add(int(offset),record.frame,frame))return MMS_E_ARGUMENT;
            unsigned current=0;
            // Frame zero belongs to the immutable root slot. The legacy paste
            // walk skips it when a next key exists and can create a second zero.
            if(frame)while(read(current).frame<frame&&read(current).next)current=read(current).next;
            if(read(current).frame==frame){fill(write(current),record);continue;}
            std::size_t free=1;while(free<targetCapacity&&read(unsigned(free)).frame)++free;
            if(free>=mdl::KeyPoolLimit<K>())return MMS_E_MEMORY;
            targetCapacity=(std::max)(targetCapacity,free+1);const auto existing=read(current);auto& added=write(unsigned(free));
            if(existing.frame<frame){write(current).next=unsigned(free);added.previous=current;added.next=0;}
            else{write(existing.previous).next=unsigned(free);added.previous=existing.previous;write(current).previous=unsigned(free);added.next=current;}
            added.frame=frame;fill(added,record);result.maximumInsertedFrame=(std::max)(result.maximumInsertedFrame,frame);
        }
        Chain<K>(targetCapacity,read);
        // Keep source targets even when identical data only changes their mark:
        // another family may change data in the enclosing atomic transaction.
        for(const auto& item:planned){part->patches.push_back(item.second);result.dataChanged|=!SameKey(item.second.before,item.second.after);}
        if constexpr(std::is_same_v<K,mdl::CameraKey>){
            const auto parent=[&](int model,int bone){if(model==-1)return;Parent dependency(app,model,bone);
                if(std::none_of(part->parents.begin(),part->parents.end(),[&](const auto& old){return old.slot==dependency.slot;}))part->parents.push_back(std::move(dependency));};
            const auto pose=ReadPose<K>(app);parent(pose.parentModel,pose.parentBone);
            for(const auto& entry:chain)parent(entry.key.parentModel,entry.key.parentBone);
            for(const auto& patch:part->patches)if(!patch.slot||patch.after.frame)parent(patch.after.parentModel,patch.after.parentBone);
        }
        result.part=std::move(part);out=std::move(result);return MMS_OK;
    });
}

template<class K,class Fill> MmsResult Plan(MMDApp& app,int frame,Fill fill,const Pose* firstPose,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();if(frame<0||frame>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(!mdl::KeyPoolIdentity(Pool<K>(app)))return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{
        if(!FinitePose(ReadPose<K>(app)))return MMS_E_ARGUMENT;
        auto* keys=Pool<K>(app);const auto capacity=mdl::KeyPoolCapacity(keys);
        const auto chain=Chain<K>(capacity,[&](unsigned i){return keys[i];});
        std::uint32_t current=0;while(keys[current].frame<unsigned(frame)&&keys[current].next)current=keys[current].next;
        auto target=keys[current];fill(target,keys[current].frame==unsigned(frame));
        if(keys[current].frame==unsigned(frame)&&SameKey(target,keys[current])&&(!firstPose||*firstPose==ReadPose<K>(app)))return MMS_OK;
        auto change=std::make_shared<Registration<K>>(app);if(firstPose)change->firstPose=*firstPose;
        std::map<unsigned,typename Registration<K>::Patch> patches;
        const auto write=[&](unsigned slot)->K&{auto found=patches.find(slot);if(found==patches.end()){
            auto original=slot<capacity?keys[slot]:K{};auto after=original;after.selected=0;
            found=patches.emplace(slot,typename Registration<K>::Patch{slot,original,after}).first;}return found->second.after;};
        if(keys[current].frame==unsigned(frame))fill(write(current),true);
        else{
            std::size_t free=1;while(free<capacity&&keys[free].frame)++free;
            if(free>=mdl::KeyPoolLimit<K>())return MMS_E_ARGUMENT;auto& added=write(unsigned(free));
            if(keys[current].frame<unsigned(frame)){write(current).next=unsigned(free);added.previous=current;added.next=0;}
            else{const auto previous=keys[current].previous;write(previous).next=unsigned(free);added.previous=previous;write(current).previous=unsigned(free);added.next=current;}
            added.frame=unsigned(frame);fill(added,false);change->afterLast=(std::max)(change->beforeLast,unsigned(frame));
        }
        for(const auto& item:patches)change->patches.push_back(item.second);
        if constexpr(std::is_same_v<K,mdl::CameraKey>){
            const auto parent=[&](int model,int bone){
                if(model==-1)return;
                // Validate each referenced bone even when its model is already
                // retained. One owner per model preserves its scene identity.
                Parent dependency(app,model,bone);
                if(std::none_of(change->parents.begin(),change->parents.end(),[&](const auto& p){return p.slot==dependency.slot;}))
                    change->parents.push_back(std::move(dependency));
            };
            parent(change->beforePose.parentModel,change->beforePose.parentBone);
            parent(change->firstPose.parentModel,change->firstPose.parentBone);
            for(const auto& key:chain)parent(key.key.parentModel,key.key.parentBone);
            for(const auto& patch:change->patches){
                if(!patch.slot||patch.before.frame)parent(patch.before.parentModel,patch.before.parentBone);
                if(!patch.slot||patch.after.frame)parent(patch.after.parentModel,patch.after.parentBone);
            }
        }
        out=std::move(change);return MMS_OK;
    });
}
}

MmsResult PlanCameraKeyPastePart(MMDApp& app,const GlobalPasteSource& source,GlobalPasteFamilyResult& out) noexcept {
    return PlanPastePart<mdl::CameraKey>(app,source.cameras,source.frameOffset,[](mdl::CameraKey& key,const mdl::CameraClipboardRecord& value){
        if(!std::isfinite(value.distance)||value.fov<1||value.fov>179||value.perspective>1||value.parentModel<-1)throw std::invalid_argument("camera paste value");
        for(unsigned i=0;i<3;++i)if(!std::isfinite(value.eye[i])||!std::isfinite(value.target[i]))throw std::invalid_argument("camera paste pose");
        for(auto byte:value.interpolation)if(byte>127)throw std::invalid_argument("camera paste interpolation");
        key.distance=value.distance;std::memcpy(key.eye,value.eye,sizeof(key.eye));std::memcpy(key.target,value.target,sizeof(key.target));
        std::memcpy(key.interpolation,value.interpolation,sizeof(key.interpolation));key.perspective=value.perspective;key.fov=value.fov;
        key.parentModel=value.parentModel;key.parentBone=value.parentBone;key.selected=1;
    },out);
}
MmsResult PlanLightKeyPastePart(MMDApp& app,const GlobalPasteSource& source,GlobalPasteFamilyResult& out) noexcept {
    return PlanPastePart<mdl::LightKey>(app,source.lights,source.frameOffset,[](mdl::LightKey& key,const mdl::LightClipboardRecord& value){
        for(unsigned i=0;i<3;++i)if(!std::isfinite(value.direction[i])||!std::isfinite(value.color[i]))throw std::invalid_argument("light paste value");
        std::memcpy(key.direction,value.direction,sizeof(key.direction));std::memcpy(key.color,value.color,sizeof(key.color));key.selected=1;
    },out);
}
MmsResult PlanShadowKeyPastePart(MMDApp& app,const GlobalPasteSource& source,GlobalPasteFamilyResult& out) noexcept {
    return PlanPastePart<mdl::SelfShadowKey>(app,source.shadows,source.frameOffset,[](mdl::SelfShadowKey& key,const mdl::ShadowClipboardRecord& value){
        if(value.mode>2||!std::isfinite(value.distance)||value.distance<0||value.distance>1)throw std::invalid_argument("shadow paste value");
        key.mode=value.mode;key.distance=value.distance;key.selected=1;
    },out);
}

MmsResult PlanCameraRegistration(MMDApp& app,int frame,bool reset,std::shared_ptr<HistoryChange>& out) noexcept {
    return Plan<mdl::CameraKey>(app,frame,[&](mdl::CameraKey& key,bool existing){
        const auto pose=ReadPose<mdl::CameraKey>(app);
        if(pose.values[7]<1||pose.values[7]>179||pose.mode<0||pose.mode>1)throw std::invalid_argument("camera registration value");
        for(unsigned i=0;i<3;++i){key.eye[i]=pose.values[i];key.target[i]=pose.values[i+3];}
        key.distance=pose.values[6];key.fov=int(pose.values[7]);key.perspective=std::uint8_t(pose.mode);
        key.parentModel=pose.parentModel;key.parentBone=pose.parentBone;key.selected=1;
        if(!existing||reset){std::memset(key.interpolation,20,12);std::memset(reinterpret_cast<unsigned char*>(key.interpolation)+12,107,12);}
    },nullptr,out);
}
MmsResult PlanLightRegistration(MMDApp& app,int frame,std::shared_ptr<HistoryChange>& out) noexcept {
    return Plan<mdl::LightKey>(app,frame,[&](mdl::LightKey& key,bool){const auto pose=ReadPose<mdl::LightKey>(app);
        for(unsigned i=0;i<3;++i){key.direction[i]=pose.values[i];key.color[i]=pose.values[i+3];}key.selected=1;
    },nullptr,out);
}
MmsResult PlanShadowRegistration(MMDApp& app,int frame,float distance,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();if(!std::isfinite(distance)||distance<0||distance>1)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{auto pose=ReadPose<mdl::SelfShadowKey>(app);pose.values[0]=distance;
        if(pose.mode<0||pose.mode>2)return MMS_E_ARGUMENT;
        return Plan<mdl::SelfShadowKey>(app,frame,[&](mdl::SelfShadowKey& key,bool){key.mode=std::uint8_t(pose.mode);key.distance=distance;key.selected=1;},&pose,out);
    });
}
}
#endif
