#include "native_camera_follow_sample.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
constexpr double kPi=3.14159265358979323846;
void Add(std::uint64_t& bytes,std::uint64_t count,std::uint64_t stride=1)noexcept{
    bytes=stride&&count>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+count*stride;
}
template<class K> K* Pool(MMDApp& app,int slot)noexcept{
    if constexpr(std::is_same_v<K,mdl::LightKey>)return app.LightKeys();
    else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)return app.ShadowKeys();
    else if constexpr(std::is_same_v<K,mdl::GravityKey>)return app.GravityKeys();
    else return slot>=0&&slot<255?app.AccessoryKeys(unsigned(slot)):nullptr;
}
template<class K> void ValidatePayload(const K& key){
    if constexpr(std::is_same_v<K,mdl::LightKey>){
        for(float value:key.direction)if(!std::isfinite(value))throw std::invalid_argument("follow light direction");
        for(float value:key.color)if(!std::isfinite(value))throw std::invalid_argument("follow light color");
    }else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>){
        if(!std::isfinite(key.distance))throw std::invalid_argument("follow shadow distance");
    }else if constexpr(std::is_same_v<K,mdl::GravityKey>){
        if(!std::isfinite(key.acceleration))throw std::invalid_argument("follow gravity magnitude");
        for(float value:key.direction)if(!std::isfinite(value))throw std::invalid_argument("follow gravity direction");
    }else{
        for(float value:key.position)if(!std::isfinite(value))throw std::invalid_argument("follow accessory position");
        for(float value:key.rotation)if(!std::isfinite(value))throw std::invalid_argument("follow accessory rotation");
        if(!std::isfinite(key.scale)||!std::isfinite(key.opacity))throw std::invalid_argument("follow accessory scale or opacity");
    }
}
template<class K> bool Same(K a,K b)noexcept{
    // Gravity's selected byte is part of the FOUR bytes copied from the right
    // key into the live noise-mode word, so selection affects this one sample.
    if constexpr(!std::is_same_v<K,mdl::GravityKey>)a.selected=b.selected=0;
    return !std::memcmp(&a,&b,sizeof(K));
}
template<class K> struct TrackSnapshot {
    struct Row{unsigned slot;K key;};
    MMDApp& app;int slot,frame;std::uint64_t identity;std::size_t capacity;
    curve_host::TrackAuthoring authoring;std::vector<Row> rows;
    unsigned current=0,previous=0;
    TrackSnapshot(MMDApp& a,curve_host::Kind kind,int s=0):app(a),slot(s),frame(a.CurrentFrame()),
        identity(mdl::KeyPoolIdentity(Pool<K>(a,s))),capacity(mdl::KeyPoolCapacity(Pool<K>(a,s))),authoring(a,kind,s){
        if(!identity||!capacity||frame<0||frame>frame_limits::maximum)throw std::invalid_argument("unowned follow track");
        const auto* keys=Pool<K>(app,slot);std::vector<unsigned char> visited(capacity,0);
        unsigned index=0,prior=0,last=0;
        for(std::size_t hops=0;;++hops){
            if(index>=capacity||visited[index])throw std::invalid_argument("follow key cycle");
            const auto key=keys[index];
            if(key.frame>unsigned(frame_limits::maximum)||key.previous>=capacity||key.next>=capacity||
                (!hops&&key.frame)||(hops&&(key.frame<=last||key.previous!=prior)))throw std::invalid_argument("follow key chain");
            ValidatePayload(key);visited[index]=1;rows.push_back({index,key});
            if(!key.next)break;prior=index;last=key.frame;index=key.next;
        }
        for(std::size_t i=1;i<capacity;++i)if(!visited[i]&&keys[i].frame)throw std::invalid_argument("orphan follow key");
        auto at=std::lower_bound(rows.begin(),rows.end(),unsigned(frame),[](const Row& row,unsigned f){return row.key.frame<f;});
        if(at==rows.end())at=rows.end()-1;current=at->slot;previous=at->key.frame>unsigned(frame)?(at-1)->slot:at->slot;
        std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){return a.slot<b.slot;});
    }
    const K* At(unsigned index)const noexcept{
        const auto found=std::lower_bound(rows.begin(),rows.end(),index,[](const Row& row,unsigned slot){return row.slot<slot;});
        return found!=rows.end()&&found->slot==index?&found->key:nullptr;
    }
    curve_host::NativeKeyView View()const noexcept{
        return {this,capacity,[](const void* p,unsigned index)noexcept->const void*{return static_cast<const TrackSnapshot*>(p)->At(index);}};
    }
    bool Current()const noexcept{
        const auto* keys=Pool<K>(app,slot);const auto count=mdl::KeyPoolCapacity(keys);
        if(app.CurrentFrame()!=frame||mdl::KeyPoolIdentity(keys)!=identity||count<capacity||!authoring.CapturedCurrent())return false;
        for(const auto& row:rows)if(!Same(keys[row.slot],row.key))return false;
        std::size_t occupied=0;for(std::size_t i=1;i<count;++i)if(keys[i].frame)++occupied;
        return occupied+1==rows.size();
    }
    template<class Apply> bool Overlay(Apply&& apply)const{
        auto target=authoring.Prepare(View());
        if(target->Authored()){std::array<double,12> values{};if(!target->Sample(frame,values))throw std::invalid_argument("follow authored sample");apply(values);}
        return target->IsCurrent();
    }
    std::uint64_t RetainedBytes()const noexcept{
        std::uint64_t bytes=sizeof(*this);Add(bytes,rows.capacity(),sizeof(Row));Add(bytes,authoring.RetainedBytes());return bytes;
    }
};
FollowLightSample NativeLight(const TrackSnapshot<mdl::LightKey>& source){
    const auto& right=*source.At(source.current);const auto& left=*source.At(source.previous);FollowLightSample result;
    const bool between=right.frame>unsigned(source.frame);
    const double t=between?double(unsigned(source.frame)-left.frame)/double(right.frame-left.frame):0;
    for(unsigned i=0;i<3;++i){
        // The subtraction is float, then the multiplication/addition is double.
        result.direction[i]=between?float((right.direction[i]-left.direction[i])*t+left.direction[i]):right.direction[i];
        result.color[i]=between?float((right.color[i]-left.color[i])*t+left.color[i]):right.color[i];
        if(!std::isfinite(result.direction[i])||!std::isfinite(result.color[i]))throw std::invalid_argument("follow light interpolation overflow");
    }return result;
}
FollowShadowSample NativeShadow(const TrackSnapshot<mdl::SelfShadowKey>& source)noexcept{
    const auto& key=*source.At(source.previous);return {std::int32_t(static_cast<std::int8_t>(key.mode)),key.distance};
}
FollowGravitySample NativeGravity(const TrackSnapshot<mdl::GravityKey>& source){
    const auto& right=*source.At(source.current);const auto& left=*source.At(source.previous);FollowGravitySample result;
    result.magnitude=right.acceleration;std::copy(std::begin(right.direction),std::end(right.direction),result.direction.begin());
    result.noise=std::uint32_t(right.noise);std::memcpy(&result.modeWord,&right.noiseEnabled,sizeof result.modeWord);
    if(right.frame>unsigned(source.frame)){
        // Supported frames are nonnegative int32, so these deltas are the same
        // float conversions as ApplyGravityTrack's signed/wrap sequence.
        const float t=float(unsigned(source.frame)-left.frame)/float(right.frame-left.frame);
        // The native int subtraction/addition wrap in 32 bits. Express those
        // operations in unsigned storage rather than C++ signed overflow.
        const std::uint32_t deltaBits=std::uint32_t(right.noise)-std::uint32_t(left.noise);
        std::int32_t delta;std::memcpy(&delta,&deltaBits,sizeof delta);
        const float scaled=t*float(delta);
        if(!std::isfinite(scaled))throw std::invalid_argument("follow gravity noise interpolation overflow");
        // Preserve CVTTSS2SI's indefinite value at its float-rounded boundary.
        const auto integral=double(scaled)>=2147483648.0||double(scaled)<-2147483648.0?
            INT32_MIN:std::int32_t(scaled);
        result.noise=std::uint32_t(integral)+std::uint32_t(left.noise);result.magnitude=(right.acceleration-left.acceleration)*t+left.acceleration;
        for(unsigned i=0;i<3;++i)result.direction[i]=(right.direction[i]-left.direction[i])*t+left.direction[i];
    }
    if(!std::isfinite(result.magnitude))throw std::invalid_argument("follow gravity interpolation overflow");
    for(float value:result.direction)if(!std::isfinite(value))throw std::invalid_argument("follow gravity direction overflow");return result;
}
FollowAccessoryPose NativeAccessory(const TrackSnapshot<mdl::AccessoryKey>& source){
    const auto& right=*source.At(source.current);const auto& left=*source.At(source.previous);const auto& held=right.frame>unsigned(source.frame)?left:right;
    FollowAccessoryPose result;result.visible=held.visible;result.shadowEnabled=held.shadowEnabled;result.parentModel=held.parentModel;result.parentBone=held.parentBone;
    std::copy(std::begin(held.position),std::end(held.position),result.position.begin());std::copy(std::begin(held.rotation),std::end(held.rotation),result.rotation.begin());
    result.scale=held.scale;result.opacity=held.opacity;
    if(right.frame>unsigned(source.frame)){const float t=float(unsigned(source.frame)-left.frame)/float(right.frame-left.frame);
        result.scale=left.scale+t*(right.scale-left.scale);result.opacity=left.opacity+t*(right.opacity-left.opacity);}
    if(!std::isfinite(result.scale)||!std::isfinite(result.opacity))throw std::invalid_argument("follow accessory interpolation overflow");return result;
}
bool ParentCurrent(MMDApp& app,const FollowAccessoryParent& parent)noexcept{
    if(!parent.owner)return true;
    if(!parent.owner->Present()||app.ModelSlot(parent.slot)!=parent.model||!SceneObjectPresent(parent.model,parent.id))return false;
    const auto identity=SceneObjectIdentity(parent.model);const auto& model=*mdl::Mdl(parent.model);
    return identity.generation==parent.initialGeneration&&model.boneTable==parent.bones&&model.boneCount==parent.boneCount&&
        parent.bone>=0&&unsigned(parent.bone)<parent.boneCount;
}
FollowAccessoryParent CaptureParent(MMDApp& app,const FollowAccessoryPose& value){
    FollowAccessoryParent parent;if(value.parentModel==-1)return parent;
    if(value.parentModel<0||value.parentModel>=int(kModelSlotCount)||value.parentBone<0)throw std::invalid_argument("follow accessory parent");
    parent.slot=unsigned(value.parentModel);parent.bone=value.parentBone;parent.model=app.ModelSlot(parent.slot);
    if(!parent.model)throw std::invalid_argument("missing follow accessory parent model");const auto& model=*mdl::Mdl(parent.model);
    if(!model.boneTable||unsigned(parent.bone)>=model.boneCount)throw std::invalid_argument("missing follow accessory parent bone");
    parent.bones=model.boneTable;parent.boneCount=model.boneCount;parent.owner=RetainSceneObject(app,MMS_OBJECT_MODEL,parent.slot);
    const auto identity=SceneObjectIdentity(parent.model);parent.id=identity.id;parent.initialGeneration=identity.generation;return parent;
}
}
struct CameraFollowSampleSource::Impl {
    MMDApp& app;const int frame;bool coherent=true;
    TrackSnapshot<mdl::LightKey> light;TrackSnapshot<mdl::SelfShadowKey> shadow;TrackSnapshot<mdl::GravityKey> gravity;
    std::array<mdl::AccessoryRecord*,255> objects{};
    std::vector<std::unique_ptr<TrackSnapshot<mdl::AccessoryKey>>> tracks;
    FollowTrackSamples values;
    explicit Impl(MMDApp& a):app(a),frame(a.CurrentFrame()),light(a,curve_host::Kind::Light),shadow(a,curve_host::Kind::Shadow),gravity(a,curve_host::Kind::Gravity){
        values.light=NativeLight(light);values.shadow=NativeShadow(shadow);values.gravity=NativeGravity(gravity);
        coherent&=light.Overlay([&](const auto& v){for(unsigned i=0;i<3;++i){values.light.direction[i]=float(v[i]);values.light.color[i]=float(v[i+3]);}});
        coherent&=shadow.Overlay([&](const auto& v){values.shadow.mode=int(v[0]);values.shadow.distance=float(v[1]);});
        coherent&=gravity.Overlay([&](const auto& v){values.gravity.magnitude=float(v[0]);for(unsigned i=0;i<3;++i)values.gravity.direction[i]=float(v[i+1]);
            values.gravity.noise=std::uint32_t(int(v[4]));values.gravity.modeWord=(values.gravity.modeWord&0xffffff00u)|std::uint8_t(v[5]);});
        for(unsigned slot=0;slot<objects.size();++slot)if((objects[slot]=app.AccessorySlot(slot))){
            auto track=std::make_unique<TrackSnapshot<mdl::AccessoryKey>>(app,curve_host::Kind::Accessory,int(slot));FollowAccessorySample sample;
            sample.slot=slot;sample.object=objects[slot];sample.owner=RetainSceneObject(app,MMS_OBJECT_ACCESSORY,slot);
            const auto identity=SceneObjectIdentity(sample.object);sample.id=identity.id;sample.initialGeneration=identity.generation;sample.value=NativeAccessory(*track);
            coherent&=track->Overlay([&](const auto& v){auto& out=sample.value;for(unsigned i=0;i<3;++i){out.position[i]=float(v[i]);out.rotation[i]=float(v[i+3]*kPi/180);}
                out.scale=float(v[6]);out.opacity=float(v[7]);out.visible=std::uint8_t(v[8]);out.shadowEnabled=std::uint8_t(v[9]);out.parentModel=int(v[10]);out.parentBone=int(v[11]);});
            sample.parent=CaptureParent(app,sample.value);values.accessories.push_back(std::move(sample));tracks.push_back(std::move(track));
        }
    }
    bool Current()const noexcept{
        if(!coherent||app.CurrentFrame()!=frame||!light.Current()||!shadow.Current()||!gravity.Current())return false;
        for(unsigned slot=0;slot<objects.size();++slot)if(app.AccessorySlot(slot)!=objects[slot])return false;
        for(std::size_t i=0;i<values.accessories.size();++i){const auto& sample=values.accessories[i];
            if(!sample.owner->Present()||!SceneObjectPresent(sample.object,sample.id))return false;
            if(SceneObjectIdentity(sample.object).generation!=sample.initialGeneration||!tracks[i]->Current()||!ParentCurrent(app,sample.parent))return false;
        }return true;
    }
};
CameraFollowSampleSource::CameraFollowSampleSource(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
CameraFollowSampleSource::~CameraFollowSampleSource()=default;
const FollowTrackSamples& CameraFollowSampleSource::Values()const noexcept{return impl_->values;}
bool CameraFollowSampleSource::IsCurrent()const noexcept{return impl_->Current();}
std::uint64_t CameraFollowSampleSource::RetainedBytes()const noexcept{
    std::uint64_t bytes=sizeof(*this)+sizeof(Impl)+4*sizeof(void*);Add(bytes,impl_->light.RetainedBytes());Add(bytes,impl_->shadow.RetainedBytes());Add(bytes,impl_->gravity.RetainedBytes());
    Add(bytes,impl_->tracks.capacity(),sizeof(decltype(impl_->tracks)::value_type));Add(bytes,impl_->values.accessories.capacity(),sizeof(FollowAccessorySample));
    for(const auto& track:impl_->tracks)Add(bytes,track->RetainedBytes());return bytes;
}
MmsResult CaptureCameraFollowSamples(MMDApp& app,std::shared_ptr<CameraFollowSampleSource>& out)noexcept{
    out.reset();if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(!mdl::KeyPoolIdentity(app.LightKeys())||!mdl::KeyPoolCapacity(app.LightKeys())||
        !mdl::KeyPoolIdentity(app.ShadowKeys())||!mdl::KeyPoolCapacity(app.ShadowKeys())||
        !mdl::KeyPoolIdentity(app.GravityKeys())||!mdl::KeyPoolCapacity(app.GravityKeys()))return MMS_E_NOT_READY;
    for(unsigned slot=0;slot<255;++slot)if(app.AccessorySlot(slot)&&
        (!mdl::KeyPoolIdentity(app.AccessoryKeys(slot))||!mdl::KeyPoolCapacity(app.AccessoryKeys(slot))))return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{
        auto impl=std::make_unique<CameraFollowSampleSource::Impl>(app);
        auto source=std::shared_ptr<CameraFollowSampleSource>(new CameraFollowSampleSource(std::move(impl)));
        if(!source->IsCurrent())return MMS_E_STALE;out=std::move(source);return MMS_OK;
    });
}
}
#endif
