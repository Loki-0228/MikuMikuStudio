#include "mikumikustudio/frame_limits.hpp"
#include "../plugins/scene_resource_budget.hpp"
#include "mikumikustudio/key_pool.hpp"
#define NOMINMAX
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/animation_curve_motion.hpp"
#include "native_curve_sample.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/runtime_log.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../plugins/native_curve_commands.hpp"
#include "../plugins/native_pose_scope.hpp"
#include "../plugins/native_accessory_parent_history.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace mikumikustudio::curve_host {
namespace {
namespace ac = animation_curve;
using Channel = curve_editor::Channel;
constexpr double pi = 3.14159265358979323846;
struct Field {
    const char* id;
    const wchar_t* ja; const wchar_t* en; const wchar_t* zh;
    bool discrete = false;
    bool step = false;
    double minimum = -1.e8, maximum = 1.e8;
};
const std::vector<Field> camera = {
    {"x",L"位置 X",L"Position X",L"位置 X"},
    {"y",L"位置 Y",L"Position Y",L"位置 Y"},
    {"z",L"位置 Z",L"Position Z",L"位置 Z"},
    {"rx",L"回転 X",L"Rotation X",L"旋转 X"},
    {"ry",L"回転 Y",L"Rotation Y",L"旋转 Y"},
    {"rz",L"回転 Z",L"Rotation Z",L"旋转 Z"},
    {"distance",L"距離",L"Distance",L"距离"},
    {"fov",L"視野角",L"View angle",L"视角",false,false,1,179},
    {"perspective",L"投影方式",L"Projection",L"投影方式",true,true,0,1},
    {"parentModel",L"親モデル",L"Parent model",L"父模型",true,true,-1,254},
    {"parentBone",L"親ボーン",L"Parent bone",L"父骨骼",true,true,0,1000000}};
const std::vector<Field> light = {
    {"x",L"方向 X",L"Direction X",L"方向 X"},
    {"y",L"方向 Y",L"Direction Y",L"方向 Y"},
    {"z",L"方向 Z",L"Direction Z",L"方向 Z"},
    {"r",L"赤",L"Red",L"红",false,false,0,1},
    {"g",L"緑",L"Green",L"绿",false,false,0,1},
    {"b",L"青",L"Blue",L"蓝",false,false,0,1}};
const std::vector<Field> shadow = {
    {"mode",L"モード",L"Mode",L"模式",true,true,0,2},
    {"distance",L"距離",L"Distance",L"距离",false,true,0,1}};
const std::vector<Field> gravity = {
    {"strength",L"加速度",L"Acceleration",L"加速度",false,false,0,100000},
    {"x",L"方向 X",L"Direction X",L"方向 X"},
    {"y",L"方向 Y",L"Direction Y",L"方向 Y"},
    {"z",L"方向 Z",L"Direction Z",L"方向 Z"},
    {"noise",L"ノイズ量",L"Noise",L"噪声量",true,false,0,1000000},
    {"noiseEnabled",L"ノイズ有効",L"Noise enabled",L"启用噪声",true,true,0,1}};
const std::vector<Field> accessory = {
    {"x",L"位置 X",L"Position X",L"位置 X",false,true},
    {"y",L"位置 Y",L"Position Y",L"位置 Y",false,true},
    {"z",L"位置 Z",L"Position Z",L"位置 Z",false,true},
    {"rx",L"回転 X",L"Rotation X",L"旋转 X",false,true},
    {"ry",L"回転 Y",L"Rotation Y",L"旋转 Y",false,true},
    {"rz",L"回転 Z",L"Rotation Z",L"旋转 Z",false,true},
    {"scale",L"サイズ",L"Scale",L"缩放",false,false,0.000001,1.e6},
    {"opacity",L"透明度",L"Opacity",L"透明度",false,false,0,1},
    {"visible",L"表示",L"Visible",L"显示",true,true,0,1},
    {"shadow",L"影",L"Shadow",L"阴影",true,true,0,1},
    {"parentModel",L"親モデル",L"Parent model",L"父模型",true,true,-1,254},
    {"parentBone",L"親ボーン",L"Parent bone",L"父骨骼",true,true,0,1000000}};
const std::vector<Field>& Fields(Kind k) {
    switch(k) { case Kind::Camera:return camera; case Kind::Light:return light;
    case Kind::Shadow:return shadow; case Kind::Gravity:return gravity; default:return accessory; }
}
std::wstring Error(MMDApp& app, const wchar_t* ja, const wchar_t* en, const wchar_t* zh) {
    return app.EnglishUI()==0?ja:app.EnglishUI()==1?en:zh;
}
bool CurvePreparationFailure(std::wstring& error,const wchar_t* text) noexcept {
    // Reporting an allocation failure must not allocate past a Win32 callback
    // boundary. The caller can show its generic failure message if no storage
    // remains even for this diagnostic.
    try {error.assign(text);}catch(const std::exception&){error.clear();}
    return false;
}
double Value(const mdl::CameraKey& k, size_t c) {
    if(c<3)return k.eye[c]; if(c<6)return k.target[c-3]*180.0/pi;
    switch(c){case 6:return k.distance;case 7:return k.fov;case 8:return k.perspective;
    case 9:return k.parentModel;default:return k.parentBone;}
}
double Value(const mdl::LightKey& k, size_t c){return c<3?k.direction[c]:k.color[c-3];}
double Value(const mdl::SelfShadowKey& k, size_t c){return c==0?k.mode:k.distance;}
double Value(const mdl::GravityKey& k, size_t c){return c==0?k.acceleration:c<4?k.direction[c-1]:c==4?k.noise:k.noiseEnabled;}
double Value(const mdl::AccessoryKey& k, size_t c){
    if(c<3)return k.position[c];if(c<6)return k.rotation[c-3]*180.0/pi;
    switch(c){case 6:return k.scale;case 7:return k.opacity;case 8:return k.visible;
    case 9:return k.shadowEnabled;case 10:return k.parentModel;default:return k.parentBone;}
}
void Put(mdl::CameraKey& k,size_t c,double v){
    if(c<3)k.eye[c]=float(v);else if(c<6)k.target[c-3]=float(v*pi/180);
    else switch(c){case 6:k.distance=float(v);break;case 7:k.fov=int(std::lround(v));break;
    case 8:k.perspective=uint8_t(v);break;case 9:k.parentModel=int(v);break;case 10:k.parentBone=int(v);break;}
}
void Put(mdl::LightKey& k,size_t c,double v){if(c<3)k.direction[c]=float(v);else k.color[c-3]=float(v);}
void Put(mdl::SelfShadowKey& k,size_t c,double v){if(c==0)k.mode=uint8_t(v);else k.distance=float(v);}
void Put(mdl::GravityKey& k,size_t c,double v){if(c==0)k.acceleration=float(v);else if(c<4)k.direction[c-1]=float(v);else if(c==4)k.noise=int(v);else k.noiseEnabled=uint8_t(v);}
void Put(mdl::AccessoryKey& k,size_t c,double v){
    if(c<3)k.position[c]=float(v);else if(c<6)k.rotation[c-3]=float(v*pi/180);
    else switch(c){case 6:k.scale=float(v);break;case 7:k.opacity=float(v);break;
    case 8:k.visible=uint8_t(v);break;case 9:k.shadowEnabled=uint8_t(v);break;
    case 10:k.parentModel=int(v);break;case 11:k.parentBone=int(v);break;}
}
template<class K> bool Native(K* pool,Kind kind,std::vector<Channel>& out){
    if(!pool)return false;
    out.clear(); const auto& f=Fields(kind); out.resize(f.size());
    for(size_t c=0;c<f.size();++c){out[c].id=f[c].id;out[c].track.discrete=f[c].discrete;}
    uint32_t i=0,previousFrame=0;
    for(size_t visited=0;visited<mdl::KeyPoolCapacity(pool);++visited){
        if(i>=mdl::KeyPoolCapacity(pool) || pool[i].frame>int32_t(mikumikustudio::frame_limits::maximum) ||
           (visited && pool[i].frame<=previousFrame))return false;
        for(size_t c=0;c<f.size();++c){
            ac::Key key;key.frame=int(pool[i].frame);key.value=Value(pool[i],c);
            key.tangent=ac::Tangent::Free;
            key.interpolation=f[c].step?ac::Interpolation::Step:ac::Interpolation::Linear;
            if(!std::isfinite(key.value))return false;
            out[c].track.keys.push_back(key);
        }
        previousFrame=pool[i].frame;
        if(pool[i].next==0)return true;
        i=pool[i].next;
    }return false;
}
// Match the native camera's single-precision, 12-step time inversion.
float CameraCurveTime(const std::array<double,4>& control,float t,bool transport=false){
    if(transport&&std::abs(control[0]-control[1])<=1e-12&&std::abs(control[2]-control[3])<=1e-12)return t;
    if(control[0]==control[1]&&control[2]==control[3])return t;
    const float x1=float(control[0])/127.0f,x2=float(control[2])/127.0f;
    float u=0.5f,half=0.25f;
    for(int i=0;i<12;++i){
        const float v=1.0f-u,v3=v*3.0f;
        const float x=(v3*v*u)*x1+(v3*u*u)*x2+u*u*u;
        if(x==t)break;
        u=x>=t?u-half:u+half;half*=0.5f;
    }
    const float v=1.0f-u,v3=v*3.0f;
    return (v3*v*u)*float(control[1])/127.0f+(v3*u*u)*float(control[3])/127.0f+u*u*u;
}
#include "native_camera_intervals.inc"
bool NativeGeometry(const Channel& channel,std::size_t end){
    if(end==0||end>=channel.track.keys.size()||end>=channel.nativeControls.size()||end>=channel.controlGeometry.size())return false;
    const auto&a=channel.track.keys[end-1];const auto&b=channel.track.keys[end];
    return b.interpolation==ac::Interpolation::Bezier&&
        channel.controlGeometry[end]==std::array<double,4>{a.outWeight,a.outSlope,b.inWeight,b.inSlope};
}
ac::Sample CameraSampleImpl(const Channel& channel,double frame,bool interpolateAdjacent){
    ac::Sample preserved;
    if(SamplePreservedInterval(channel,frame,preserved))return preserved;
    const auto&keys=channel.track.keys;
    if(keys.size()<2)return ac::Evaluate(channel.track,frame);
    const auto value=[&](double at){
        if(at<=keys.front().frame)return keys.front().value;
        if(at>=keys.back().frame)return keys.back().value;
        const auto bound=std::upper_bound(keys.begin(),keys.end(),at,[](double f,const ac::Key&k){return f<k.frame;});
        const auto end=std::size_t(bound-keys.begin());
        if(!NativeGeometry(channel,end))return ac::Evaluate(channel.track,at).value;
        const auto&a=keys[end-1];const auto&b=keys[end];
        // Match the native player's exact-key branch. The fixed-step Bezier
        // inverse only approximates t=0 and must not perturb keys on commit.
        if(at==a.frame)return a.value;
        const double span=double(b.frame)-a.frame;
        if(span==1.0&&!interpolateAdjacent)return a.value;
        return a.value+(b.value-a.value)*CameraCurveTime(channel.nativeControls[end],float((at-a.frame)/span));
    };
    double derivative=0;
    if(frame>=keys.front().frame&&frame<=keys.back().frame){
        auto bound=std::upper_bound(keys.begin(),keys.end(),frame,[](double f,const ac::Key& k){return f<k.frame;});
        const auto end=std::min(std::size_t(bound-keys.begin()),keys.size()-1);
        if(end&&NativeGeometry(channel,end)){
            const auto&a=keys[end-1];const auto&b=keys[end];const double span=double(b.frame)-a.frame;
            if(span>1.0||interpolateAdjacent)derivative=(b.value-a.value)/span*
                native_curve_sample::Evaluate(channel.nativeControls[end],(frame-a.frame)/span).slope;
        }else derivative=ac::Evaluate(channel.track,frame).derivative;
    }
    return {value(frame),derivative};
}
ac::Sample CameraSample(const Channel& channel,double frame){
    return CameraSampleImpl(channel,frame,false);
}
double SampleValue(const Channel&channel,double frame){
    return channel.sample?channel.sample(channel,frame).value:ac::Evaluate(channel.track,frame).value;
}
bool CameraIntervalControls(const Channel&channel,int start,int end,std::array<double,4>&out){
    const auto&keys=channel.track.keys;
    const auto at=std::lower_bound(keys.begin(),keys.end(),end,[](const auto&key,int f){return key.frame<f;});
    const auto n=size_t(at-keys.begin());
    if(at==keys.end()||n==0||at->frame!=end||keys[n-1].frame!=start)return false;
    if(NativeGeometry(channel,n)){out=channel.nativeControls[n];return true;}
    if(at->interpolation==ac::Interpolation::Linear){out={20,20,107,107};return true;}
    return ac::GetMmdControls(channel.track,n,out);
}
bool NativeRead(MMDApp& app,Kind kind,int slot,std::vector<Channel>& out){
    bool ok=false;
    switch(kind){
    case Kind::Camera:ok=Native(app.CameraKeys(),kind,out);break;
    case Kind::Light:ok=Native(app.LightKeys(),kind,out);break;
    case Kind::Shadow:ok=Native(app.ShadowKeys(),kind,out);break;
    case Kind::Gravity:ok=Native(app.GravityKeys(),kind,out);break;
    case Kind::Accessory:ok=slot>=0&&slot<255&&app.AccessorySlot(slot)&&Native(app.AccessoryKeys(slot),kind,out);break;
    }
    if(!ok)return false;
    if(kind==Kind::Camera){
        for(size_t c=0;c<8;++c){
            out[c].nativeControls.resize(out[c].track.keys.size());
            out[c].controlGeometry.resize(out[c].track.keys.size());
            out[c].sample=CameraSample;
            if(c>=3&&c<6)out[c].interpolationGroup="camera/rotation";
            // The root's controls do not affect playback, but they are still
            // bytes of the original standard camera record on raw export.
            const int lane=c<3?int(c):c<6?3:c==6?4:5;
            for(int j=0;j<4;++j)out[c].nativeControls[0][j]=app.CameraKeys()[0].interpolation[j][lane];
        }
        uint32_t i=app.CameraKeys()[0].next;size_t end=1;
        while(i){
            const auto& k=app.CameraKeys()[i];
            for(size_t c=0;c<8;++c){
                const int lane=c<3?int(c):c<6?3:c==6?4:5;
                auto& tr=out[c].track;
                auto&ctrl=out[c].nativeControls[end];
                for(int j=0;j<4;++j){if(k.interpolation[j][lane]>127)return false;ctrl[j]=k.interpolation[j][lane];}
                // Keep raw controls authoritative. Scalar handles are a safe
                // proxy for endpoints/canvas editing, not a native read gate.
                auto&a=tr.keys[end-1];auto&b=tr.keys[end];
                const double x1=std::clamp(ctrl[0]/127.0,0.0002,0.9996);
                const double x2=std::clamp(ctrl[2]/127.0,x1+0.0002,0.9998);
                const double rate=(b.value-a.value)/double(b.frame-a.frame);
                a.outWeight=x1;b.inWeight=1.0-x2;
                a.outSlope=ctrl[1]/127.0*rate/x1;
                b.inSlope=(1.0-ctrl[3]/127.0)*rate/(1.0-x2);
                b.interpolation=ac::Interpolation::Bezier;
                out[c].controlGeometry[end]={a.outWeight,a.outSlope,b.inWeight,b.inSlope};
            }
            ++end;i=k.next;
        }
    }
    return true;
}
bool Same(const ac::Key&a,const ac::Key&b){return a.frame==b.frame&&a.value==b.value&&a.inSlope==b.inSlope&&a.outSlope==b.outSlope&&a.inWeight==b.inWeight&&a.outWeight==b.outWeight&&a.interpolation==b.interpolation;}
bool Same(const std::vector<Channel>&a,const std::vector<Channel>&b){
    if(a.size()!=b.size())return false;
    for(size_t c=0;c<a.size();++c){if(a[c].track.keys.size()!=b[c].track.keys.size()||a[c].nativeControls!=b[c].nativeControls||
        a[c].preservedCameraIntervals!=b[c].preservedCameraIntervals)return false;
        for(size_t i=0;i<a[c].track.keys.size();++i)if(!Same(a[c].track.keys[i],b[c].track.keys[i]))return false;}
    return true;
}
std::uint64_t NextAuthoringRevision() noexcept {
    static std::uint64_t revision = 0;
    return ++revision;
}
struct State {
    std::vector<Channel> channels, baseline;
    void* identity = nullptr;
    uint64_t fingerprint = 0;
    // Also identifies writes that only change editor metadata or callbacks.
    // Such writes need not change the native pool or the authored key values.
    std::uint64_t revision = NextAuthoringRevision();
#if !MMD_ORIGINAL_PROFILE
    // The allocation may move while a registered pool keeps its identity.
    // Borrowed pools have no such guarantee and retain the address guard.
    std::uint64_t poolIdentity = 0;
    void* poolAllocation = nullptr;
    // Native reconciliation changes the effective layer, not its author.
    std::uint64_t author = revision;
#endif
};
using Address=std::tuple<MMDApp*,Kind,int>;
std::map<Address,State> states;
#include "native_camera_intervals_codec.inc"
template<class K> uint64_t Fingerprint(K*pool){
    if(!pool)return 0;uint64_t hash=14695981039346656037ull;uint32_t index=0;
    for(size_t hops=0;hops<mdl::KeyPoolCapacity(pool);++hops){
        if(index>=mdl::KeyPoolCapacity(pool))return 0;
        const auto*bytes=reinterpret_cast<const uint8_t*>(&pool[index]);
        for(size_t i=0;i<sizeof(K);++i){hash^=bytes[i];hash*=1099511628211ull;}
        if(pool[index].next==0)return hash;index=pool[index].next;
    }return 0;
}
uint64_t Fingerprint(MMDApp&app,Kind k,int slot){
    switch(k){case Kind::Camera:return Fingerprint(app.CameraKeys());case Kind::Light:return Fingerprint(app.LightKeys());
    case Kind::Shadow:return Fingerprint(app.ShadowKeys());case Kind::Gravity:return Fingerprint(app.GravityKeys());
    default:return Fingerprint(app.AccessoryKeys(slot));}
}
void* Identity(MMDApp&app,Kind kind,int slot){
    if(kind==Kind::Accessory)return slot>=0&&slot<255?app.AccessorySlot(slot):nullptr;
    switch(kind){case Kind::Camera:return app.CameraKeys();case Kind::Light:return app.LightKeys();case Kind::Shadow:return app.ShadowKeys();default:return app.GravityKeys();}
}
#if !MMD_ORIGINAL_PROFILE
void* TrackPoolAllocation(MMDApp& app, Kind kind, int slot) noexcept {
    switch(kind) {
    case Kind::Camera:return app.CameraKeys();
    case Kind::Light:return app.LightKeys();
    case Kind::Shadow:return app.ShadowKeys();
    case Kind::Gravity:return app.GravityKeys();
    default:return slot>=0&&slot<255?app.AccessoryKeys(slot):nullptr;
    }
}
std::uint64_t PoolIdentity(Kind kind, const void* pool) {
    switch(kind) {
    case Kind::Camera:return mdl::KeyPoolIdentity(static_cast<const mdl::CameraKey*>(pool));
    case Kind::Light:return mdl::KeyPoolIdentity(static_cast<const mdl::LightKey*>(pool));
    case Kind::Shadow:return mdl::KeyPoolIdentity(static_cast<const mdl::SelfShadowKey*>(pool));
    case Kind::Gravity:return mdl::KeyPoolIdentity(static_cast<const mdl::GravityKey*>(pool));
    default:return mdl::KeyPoolIdentity(static_cast<const mdl::AccessoryKey*>(pool));
    }
}
std::uint64_t TrackPoolIdentity(MMDApp& app, Kind kind, int slot) {
    return PoolIdentity(kind, TrackPoolAllocation(app, kind, slot));
}
#endif
void CaptureStateIdentity(State& state, MMDApp& app, Kind kind, int slot) {
    state.identity=Identity(app,kind,slot);
#if !MMD_ORIGINAL_PROFILE
    state.poolAllocation=TrackPoolAllocation(app,kind,slot);
    state.poolIdentity=PoolIdentity(kind,state.poolAllocation);
#endif
}
bool StateIdentityCurrent(const State& state, MMDApp& app, Kind kind, int slot) {
#if !MMD_ORIGINAL_PROFILE
    const auto identity=Identity(app,kind,slot);
    if(!identity||(kind==Kind::Accessory&&state.identity!=identity))return false;
    const auto* pool=TrackPoolAllocation(app,kind,slot);
    const auto poolIdentity=PoolIdentity(kind,pool);
    if(state.poolIdentity)return state.poolIdentity==poolIdentity;
    return !poolIdentity&&state.poolAllocation==pool&&state.identity==identity;
#else
    return state.identity==Identity(app,kind,slot);
#endif
}
bool SameStateIdentity(const State& left, const State& right, Kind kind) noexcept {
#if !MMD_ORIGINAL_PROFILE
    if(kind==Kind::Accessory&&left.identity!=right.identity)return false;
    if(left.poolIdentity||right.poolIdentity)return left.poolIdentity==right.poolIdentity;
    return left.poolAllocation==right.poolAllocation&&left.identity==right.identity;
#else
    (void)kind;
    return left.identity==right.identity;
#endif
}
// Detect edits made by the existing registration/delete/paste/undo commands.
// Only changed native keys replace authored keys; unrelated tangents survive.
void ValidateRegistrationFrames(const std::vector<int>& frames) {
    for(const int frame:frames)
        if(frame<0||frame>frame_limits::maximum)throw std::invalid_argument("explicit registration frame");
}
bool MissingRegistrationFrame(const State& state,const std::vector<int>& frames) {
    for(const auto& channel:state.channels)for(const int frame:frames){
        const auto& keys=channel.track.keys;
        const auto found=std::lower_bound(keys.begin(),keys.end(),frame,[](const ac::Key& key,int at){return key.frame<at;});
        if(found==keys.end()||found->frame!=frame)return true;
    }
    return false;
}
void ValidateTargetRegistrationFrames(const std::vector<Channel>& native,const std::vector<int>& frames) {
    for(const auto& channel:native){
        const auto& actual=channel.track.keys;
        for(const int frame:frames){
            const auto present=std::lower_bound(actual.begin(),actual.end(),frame,[](const ac::Key& key,int at){return key.frame<at;});
            if(present==actual.end()||present->frame!=frame)throw std::invalid_argument("missing registered native frame");
        }
    }
}
void ForceRegistrationFrames(State& state,const std::vector<Channel>& native,const std::vector<int>& frames) {
    if(frames.empty())return;
    if(state.channels.size()!=native.size()||state.baseline.size()!=native.size())
        throw std::invalid_argument("explicit registration channels");
    ValidateTargetRegistrationFrames(native,frames);
    std::vector<int> missing;missing.reserve(frames.size());
    for(std::size_t c=0;c<native.size();++c){
        // A native row belongs to all lanes. Removing only the absent authored
        // lane's comparison entry makes normal reconciliation insert its key,
        // even when the native row itself did not change at all.
        const auto& authored=state.channels[c].track.keys;
        auto& baseline=state.baseline[c].track.keys;
        missing.clear();
        for(const int frame:frames){
            const auto found=std::lower_bound(authored.begin(),authored.end(),frame,[](const ac::Key& key,int at){return key.frame<at;});
            if(found==authored.end()||found->frame!=frame)missing.push_back(frame);
        }
        // Remove a batch in one pass. Repeated vector erases here would make
        // a large explicit motion import quadratic in the number of keys.
        std::sort(missing.begin(),missing.end());missing.erase(std::unique(missing.begin(),missing.end()),missing.end());
        baseline.erase(std::remove_if(baseline.begin(),baseline.end(),[&](const ac::Key& key){
            return std::binary_search(missing.begin(),missing.end(),key.frame);
        }),baseline.end());
    }
}
void Reconcile(State&s,const std::vector<Channel>&native){
    if(Same(s.baseline,native))return;
    for(size_t c=0;c<s.channels.size();++c){
        Channel previousChannel;
        if(!s.channels[c].preservedCameraIntervals.empty())previousChannel=s.channels[c];
        auto& keys=s.channels[c].track.keys;const auto& old=s.baseline[c].track.keys;
        const auto& fresh=native[c].track.keys;
        keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const auto&k){
            auto before=std::lower_bound(old.begin(),old.end(),k.frame,[](const auto&a,int f){return a.frame<f;});
            auto after=std::lower_bound(fresh.begin(),fresh.end(),k.frame,[](const auto&a,int f){return a.frame<f;});
            return before!=old.end()&&before->frame==k.frame&&(after==fresh.end()||after->frame!=k.frame);
        }),keys.end());
        for(const auto& k:fresh){
            auto o=std::lower_bound(old.begin(),old.end(),k.frame,[](const auto&a,int f){return a.frame<f;});
            if(o!=old.end()&&Same(*o,k))continue;
            auto at=std::lower_bound(keys.begin(),keys.end(),k.frame,[](const auto&a,int f){return a.frame<f;});
            if(at!=keys.end()&&at->frame==k.frame){
                // Float storage is the legacy authority for value changes.
                at->value=k.value;
                if(o!=old.end()&&o->frame==k.frame&&o->interpolation==k.interpolation&&o->inSlope==k.inSlope&&o->outSlope==k.outSlope)continue;
                *at=k;
            }else keys.insert(at,k);
        }
        ac::UpdateAutomatic(s.channels[c].track);
        // Metadata is indexed by interval; remap it by both endpoint frames
        // so independent authored keys cannot inherit another native interval.
        auto&channel=s.channels[c];
        channel.nativeControls.assign(keys.size(),{});
        channel.controlGeometry.assign(keys.size(),{});
        if(native[c].nativeControls.empty()){
            channel.nativeControls.clear();channel.controlGeometry.clear();
        }else {
            if(!keys.empty()&&!fresh.empty()&&keys.front().frame==fresh.front().frame)
                channel.nativeControls.front()=native[c].nativeControls.front();
            for(size_t end=1;end<keys.size();++end){
                const auto at=std::lower_bound(fresh.begin(),fresh.end(),keys[end].frame,[](const auto&key,int f){return key.frame<f;});
                const auto n=size_t(at-fresh.begin());
                if(at==fresh.end()||n==0||at->frame!=keys[end].frame||fresh[n-1].frame!=keys[end-1].frame)continue;
                channel.nativeControls[end]=native[c].nativeControls[n];
                channel.controlGeometry[end]=native[c].controlGeometry[n];
            }
        }
        channel.sample=native[c].sample;
        channel.interpolationGroup=native[c].interpolationGroup;
        // A native easing edit is intentional, even when its endpoints stay
        // unchanged. Release the old exact slices touched by that edit.
        auto& saved=previousChannel.preservedCameraIntervals;
        saved.erase(std::remove_if(saved.begin(),saved.end(),[&](const auto& interval){
            for(std::size_t n=1;n<fresh.size();++n){
                const auto at=std::lower_bound(old.begin(),old.end(),fresh[n].frame,[](const auto& k,int f){return k.frame<f;});
                const auto before=std::size_t(at-old.begin());
                if(at==old.end()||!before||at->frame!=fresh[n].frame||old[before-1].frame!=fresh[n-1].frame)continue;
                if(before>=s.baseline[c].nativeControls.size()||n>=native[c].nativeControls.size()||
                   s.baseline[c].nativeControls[before]==native[c].nativeControls[n])continue;
                if(interval.first<fresh[n].frame&&interval.last>fresh[n-1].frame)return true;
            }return false;
        }),saved.end());
        if(!saved.empty()){
            std::vector<int> visibleFrames;for(const auto& key:keys)visibleFrames.push_back(key.frame);
            RemapPreserved(previousChannel,channel,visibleFrames,false);
        }else channel.preservedCameraIntervals.clear();
    }s.baseline=native;
}
State* Current(MMDApp&app,Kind k,int slot){
    FlushLegacyRegistrations(&app);
    auto it=states.find({&app,k,slot});if(it==states.end())return nullptr;
    if(!StateIdentityCurrent(it->second,app,k,slot)){states.erase(it);return nullptr;}
    const uint64_t fingerprint=Fingerprint(app,k,slot);
    if(fingerprint&&it->second.fingerprint==fingerprint)return &it->second;
    // The authored extension is the authority for custom curves. A legacy
    // proxy that cannot be decoded must not make valid authoring disappear.
    // Keep the old fingerprint so reconciliation retries after native repair.
    std::vector<Channel> native;if(!NativeRead(app,k,slot,native))return &it->second;
    Reconcile(it->second,native);it->second.fingerprint=fingerprint;
    it->second.revision=NextAuthoringRevision();return &it->second;
}
struct PoolTransaction {
    void* address=nullptr;void* before=nullptr;void* after=nullptr;
    std::size_t beforeBytes=0;
    void (*restore)(void*,void*)=nullptr;void (*release)(void*)=nullptr;
    bool committed=false,reuseBefore=false;
    template<class K> void Begin(K*& pool) {
        auto* candidate=mdl::KeyPoolCreate<K>(mdl::KeyPoolCapacity(pool));
        address=&pool;before=pool;after=candidate;
        beforeBytes=mdl::KeyPoolCapacity(pool)*sizeof(K);reuseBefore=true;
        restore=[](void* a,void* value){*static_cast<K**>(a)=static_cast<K*>(value);};
        release=[](void* value){mdl::KeyPoolReleaseOwned(static_cast<K*>(value));};
        pool=candidate;
    }
    // Mirror may grow the staged pool; refresh its address before unwinding.
    template<class K> void Updated(K* pool){after=pool;reuseBefore=mdl::KeyPoolCapacity(pool)*sizeof(K)<=beforeBytes;}
    void* FinalIdentity() const {return reuseBefore?before:after;}
    ~PoolTransaction(){
        if(!address)return;
        if(committed&&!reuseBefore){release(before);return;}
        if(committed)std::memcpy(before,after,beforeBytes);
        restore(address,before);release(after);
    }
};
template<class K> std::vector<K> MirroredRecords(const K* previous,const std::vector<Channel>&channels,const std::set<int>&frames){
    // Curve values and native timeline selection are independent. Rebuilding
    // the pool must not turn a handle edit into a selection of the whole track.
    std::set<std::uint32_t> selected;
    for(std::size_t i=0,capacity=mdl::KeyPoolCapacity(previous);i<capacity;++i)
        if((i==0||previous[i].frame!=0)&&previous[i].selected)selected.insert(previous[i].frame);
    std::vector<K> records; records.reserve(frames.size());
    for(int frame:frames){
        K key{};key.frame=uint32_t(frame);key.selected=selected.count(key.frame)!=0;
        for(size_t c=0;c<channels.size();++c)Put(key,c,SampleValue(channels[c],frame));
        records.push_back(key);
    }
    for(size_t i=0;i<records.size();++i){records[i].previous=uint32_t(i?i-1:0);records[i].next=uint32_t(i+1<records.size()?i+1:0);}
    return records;
}
template<class K> void Mirror(K*& pool,const K* previous,const std::vector<Channel>&channels,const std::set<int>&frames){
    const auto records=MirroredRecords(previous,channels,frames);
    if(!mdl::KeyPoolEnsure(pool,records.size()))throw std::bad_alloc();
    std::memset(pool,0,sizeof(K)*mdl::KeyPoolCapacity(pool));
    std::copy(records.begin(),records.end(),pool);
}
void MirrorCameraControls(mdl::CameraKey* keys,std::size_t count,const std::vector<Channel>& input){
    constexpr std::size_t lanes[6]={0,1,2,3,6,7};
    for(std::size_t i=0;i<count;++i)for(int c=0;c<6;++c){keys[i].interpolation[0][c]=20;keys[i].interpolation[1][c]=20;
        keys[i].interpolation[2][c]=107;keys[i].interpolation[3][c]=107;}
    if(count)for(std::size_t lane=0;lane<6;++lane){
        if(lanes[lane]>=input.size())continue;const auto& source=input[lanes[lane]];
        if(source.track.keys.empty()||source.track.keys.front().frame!=int(keys[0].frame)||source.nativeControls.empty())continue;
        const auto& raw=source.nativeControls.front();
        if(!std::all_of(raw.begin(),raw.end(),[](double value){return std::isfinite(value)&&value>=0&&value<=255&&std::floor(value)==value;}))continue;
        for(std::size_t component=0;component<4;++component)keys[0].interpolation[component][lane]=static_cast<std::uint8_t>(raw[component]);
    }
    for(std::size_t i=1;i<count;++i){auto& key=keys[i];const int start=int(keys[i-1].frame),end=int(key.frame);
        for(std::size_t lane=0;lane<6;++lane){std::array<double,4> controls{};
            if(!CameraIntervalControls(input[lanes[lane]],start,end,controls))continue;
            if(lane==3){std::array<double,4> y{},z{};
                if(!CameraIntervalControls(input[4],start,end,y)||!CameraIntervalControls(input[5],start,end,z)||controls!=y||controls!=z)continue;}
            for(std::size_t component=0;component<4;++component)key.interpolation[component][lane]=std::uint8_t(std::lround(controls[component]));
        }
    }
}
}

bool Read(MMDApp& app,Kind kind,int slot,std::vector<Channel>&out,std::wstring&error){
#if !MMD_ORIGINAL_PROFILE
    if(plugins::BoundNativePoseHistory(&app)){
        const auto found=states.find({&app,kind,slot});
        std::vector<Channel> native;
        if(found!=states.end()&&StateIdentityCurrent(found->second,app,kind,slot)){
            State detached=found->second;
            if(!NativeRead(app,kind,slot,native)){
                error=Error(app,L"トラックが無効です。",L"Track unavailable.",L"轨道不可用。");return false;
            }
            Reconcile(detached,native);
            out=std::move(detached.channels);
        }else if(!NativeRead(app,kind,slot,out)){
            error=Error(app,L"トラックが無効です。",L"Track unavailable.",L"轨道不可用。");return false;
        }
    }else
#endif
    {
    auto* state=Current(app,kind,slot);
    if(state)out=state->channels;
    else if(!NativeRead(app,kind,slot,out)){
        error=Error(app,L"トラックが無効、または既存の補間を表示できません。",L"Track unavailable or existing interpolation cannot be represented.",L"轨道不可用，或现有插值无法表示。");return false;}
    }
    const auto&f=Fields(kind);
    for(size_t c=0;c<out.size();++c){out[c].label=Error(app,f[c].ja,f[c].en,f[c].zh);
        out[c].minimum=f[c].minimum;out[c].maximum=f[c].maximum;
        out[c].base=out[c].track.keys.empty()?0:out[c].track.keys.front().value;
        out[c].unit=(out[c].id=="rx"||out[c].id=="ry"||out[c].id=="rz"||out[c].id=="fov")?L"°":L"";
    }return true;
}
void InstallImportedCameraTiming(Channel& channel) noexcept {
    channel.sample=CameraSample;
}
namespace {
bool CanonicalCameraTiming(const Channel& channel) noexcept {
    const auto sampler=channel.sample.target<ac::Sample(*)(const Channel&,double)>();
    return sampler&&*sampler==CameraSample;
}
void PreserveCameraTiming(Channel& channel) {
    if(!CanonicalCameraTiming(channel))return;
    PrunePreserved(channel);
    const auto preservedCount=channel.preservedCameraIntervals.size();
    for(std::size_t end=1;end<channel.track.keys.size();++end){
        if(!NativeGeometry(channel,end))continue;
        const auto& a=channel.track.keys[end-1];const auto& b=channel.track.keys[end];
        const auto found=std::lower_bound(channel.preservedCameraIntervals.begin(),channel.preservedCameraIntervals.begin()+preservedCount,a.frame,
            [](const auto& value,int f){return value.first<f;});
        if(found!=channel.preservedCameraIntervals.begin()+preservedCount&&found->first==a.frame&&found->last==b.frame)continue;
        PreservedCameraInterval value;value.first=value.sourceFirst=a.frame;value.last=value.sourceLast=b.frame;
        value.sampleFirst=a.frame;value.sampleLast=b.frame;value.sourceValueFirst=a.value;value.sourceValueLast=b.value;
        value.controls=channel.nativeControls[end];value.geometry=IntervalGeometry(a,b);channel.preservedCameraIntervals.push_back(value);
    }
    PrunePreserved(channel);
}
void RemapCameraControlRows(const Channel& source,Channel& target) {
    target.nativeControls.clear();target.controlGeometry.clear();
    if(!source.nativeControls.empty())target.nativeControls.resize(target.track.keys.size());
    if(!source.controlGeometry.empty())target.controlGeometry.assign(target.track.keys.size(),{-1,0,-1,0});
    if(!target.nativeControls.empty()&&!source.track.keys.empty()&&
        source.track.keys.front().frame==target.track.keys.front().frame)
        target.nativeControls.front()=source.nativeControls.front();
    for(std::size_t end=1;end<target.track.keys.size();++end){
        const auto original=std::lower_bound(source.track.keys.begin(),source.track.keys.end(),target.track.keys[end].frame,
            [](const auto& key,int f){return key.frame<f;});
        const auto index=std::size_t(original-source.track.keys.begin());
        if(original==source.track.keys.end()||!index||original->frame!=target.track.keys[end].frame||
            source.track.keys[index-1].frame!=target.track.keys[end-1].frame)continue;
        if(index<source.nativeControls.size())target.nativeControls[end]=source.nativeControls[index];
        if(index<source.controlGeometry.size())target.controlGeometry[end]=source.controlGeometry[index];
    }
}
void SplitCameraBakeBoundary(Channel& channel,int frame) {
    if(!CanonicalCameraTiming(channel))return;
    const auto at=std::lower_bound(channel.track.keys.begin(),channel.track.keys.end(),frame,
        [](const auto& key,int f){return key.frame<f;});
    if(at==channel.track.keys.begin()||at==channel.track.keys.end()||at->frame==frame)return;
    const auto end=std::size_t(at-channel.track.keys.begin());
    const auto found=std::lower_bound(channel.preservedCameraIntervals.begin(),channel.preservedCameraIntervals.end(),channel.track.keys[end-1].frame,
        [](const auto& value,int f){return value.first<f;});
    if(found==channel.preservedCameraIntervals.end()||found->last!=at->frame||!PreservedCurrent(channel,*found))return;
    const auto interval=*found;
    auto next=channel;
    const double value=SampleValue(channel,frame);
    auto a=channel.track.keys[end-1],b=channel.track.keys[end];
    ac::Key middle;middle.frame=frame;middle.value=value;
    middle.interpolation=ac::Interpolation::Bezier;middle.tangent=ac::Tangent::Free;
    // These are editable, finite canvas handles. The saved source domain is
    // authoritative outside the bake, so no analytic inverse replaces the
    // player's fixed 12-step float inverse at either new boundary.
    a.outWeight=middle.inWeight=middle.outWeight=b.inWeight=1.0/3.0;
    a.outSlope=middle.inSlope=(value-a.value)/(frame-a.frame);
    middle.outSlope=b.inSlope=(b.value-value)/(b.frame-frame);
    next.track.keys[end-1]=a;next.track.keys[end]=b;
    next.track.keys.insert(next.track.keys.begin()+end,middle);
    RemapCameraControlRows(channel,next);
    auto& intervals=next.preservedCameraIntervals;
    intervals.erase(std::remove_if(intervals.begin(),intervals.end(),[&](const auto& old){return old.first==interval.first&&old.last==interval.last;}),intervals.end());
    const auto position=[&](int f){return interval.sampleFirst==interval.first&&interval.sampleLast==interval.last?double(f):
        interval.sampleFirst+(double(f)-interval.first)/(interval.last-interval.first)*(interval.sampleLast-interval.sampleFirst);};
    for(const auto index:{end,end+1}){
        auto part=interval;part.first=next.track.keys[index-1].frame;part.last=next.track.keys[index].frame;
        part.sampleFirst=position(part.first);part.sampleLast=position(part.last);
        part.geometry=IntervalGeometry(next.track.keys[index-1],next.track.keys[index]);intervals.push_back(part);
    }
    PrunePreserved(next);
    std::string detail;if(!ac::Validate(next.track,detail))throw std::invalid_argument("invalid camera bake boundary");
    channel=std::move(next);
}
}
bool PrepareCameraBakeSources(const std::vector<Channel>& input,const std::vector<bool>& fields,int first,int last,
                             std::vector<Channel>& output,std::wstring& error) {
    try{
        if(fields.size()!=input.size()||first<0||last<first||last>frame_limits::maximum)
            return CurvePreparationFailure(error,L"相机烧录通道或范围无效。");
        auto next=input;
        // A new native camera row belongs to every lane. Preserve peer timing
        // before the standard mirror's union can split an unselected lane.
        for(auto& channel:next)PreserveCameraTiming(channel);
        for(std::size_t c=0;c<next.size();++c)if(fields[c]){
            SplitCameraBakeBoundary(next[c],first);SplitCameraBakeBoundary(next[c],last);
        }
        output.swap(next);return true;
    }catch(const std::exception&){return CurvePreparationFailure(error,L"无法准备相机烧录边界，原数据已保留。");}
}
bool FinishCameraBakeChannel(const Channel& source,const ac::Track& sampled,Channel& output,std::wstring& error) {
    try{
        auto next=source;next.track=sampled;RemapCameraControlRows(source,next);PrunePreserved(next);
        std::string detail;if(!ac::Validate(next.track,detail))throw std::invalid_argument("invalid sampled camera channel");
        output=std::move(next);return true;
    }catch(const std::exception&){return CurvePreparationFailure(error,L"无法准备相机烧录结果，原数据已保留。");}
}
bool InsertPreservingShape(std::vector<Channel>& channels,std::size_t selected,int frame,std::size_t& index,std::wstring& error) {
    try {
        std::size_t resultIndex=index;
        if(selected>=channels.size()||frame<0||frame>frame_limits::maximum)return CurvePreparationFailure(error,L"通道或帧号无效。");
        const auto& current=channels[selected].track.keys;
        auto at=std::lower_bound(current.begin(),current.end(),frame,[](const ac::Key& k,int f){return k.frame<f;});
        if(at!=current.end()&&at->frame==frame){index=std::size_t(at-current.begin());return true;}
        auto prepared=channels;
        // Publishing one camera row also creates native proxy rows in every
        // other lane. Freeze their intact native evaluators before that union
        // can split a peer's original interval in the standard PMM body.
        for(auto& channel:prepared){
            const auto sampler=channel.sample.target<ac::Sample(*)(const Channel&,double)>();
            if(!sampler||*sampler!=CameraSample)continue;
            PrunePreserved(channel);
            const auto preservedCount=channel.preservedCameraIntervals.size();
            for(std::size_t end=1;end<channel.track.keys.size();++end){
                if(!NativeGeometry(channel,end))continue;
                const auto& a=channel.track.keys[end-1];const auto& b=channel.track.keys[end];
                const auto found=std::lower_bound(channel.preservedCameraIntervals.begin(),channel.preservedCameraIntervals.begin()+preservedCount,a.frame,
                    [](const auto& value,int f){return value.first<f;});
                if(found!=channel.preservedCameraIntervals.begin()+preservedCount&&found->first==a.frame&&found->last==b.frame)continue;
                PreservedCameraInterval value;value.first=value.sourceFirst=a.frame;value.last=value.sourceLast=b.frame;
                value.sampleFirst=a.frame;value.sampleLast=b.frame;value.sourceValueFirst=a.value;value.sourceValueLast=b.value;
                value.controls=channel.nativeControls[end];value.geometry=IntervalGeometry(a,b);channel.preservedCameraIntervals.push_back(value);
            }
            PrunePreserved(channel);
        }
        auto next=prepared;
        for(std::size_t c=0;c<next.size();++c) {
            if(c!=selected&&(channels[selected].interpolationGroup.empty()||
               channels[c].interpolationGroup!=channels[selected].interpolationGroup))continue;
            const auto& before=prepared[c];auto& target=next[c];
            const auto sampler=before.sample.target<ac::Sample(*)(const Channel&,double)>();
            const bool camera=sampler&&*sampler==CameraSample;
            if((before.sample&&!camera)||(!camera&&(!before.nativeControls.empty()||!before.interpolationGroup.empty()))) {
                return CurvePreparationFailure(error,L"此通道的宿主尚未提供原生保形细分。");
            }
            const auto bound=std::lower_bound(before.track.keys.begin(),before.track.keys.end(),frame,
                [](const ac::Key& key,int f){return key.frame<f;});
            if(bound!=before.track.keys.end()&&bound->frame==frame)continue;
            const auto end=std::size_t(bound-before.track.keys.begin());
            PreservedCameraInterval preserved;bool native=false;
            if(camera&&end&&end<before.track.keys.size()) {
                for(const auto& value:before.preservedCameraIntervals)
                    if(value.first==before.track.keys[end-1].frame&&value.last==bound->frame&&PreservedCurrent(before,value)) {
                        preserved=value;native=true;break;
                    }
                if(!native&&NativeGeometry(before,end)) {
                    preserved.first=preserved.sourceFirst=before.track.keys[end-1].frame;
                    preserved.last=preserved.sourceLast=bound->frame;
                    preserved.sampleFirst=preserved.first;preserved.sampleLast=preserved.last;
                    preserved.sourceValueFirst=before.track.keys[end-1].value;preserved.sourceValueLast=bound->value;
                    preserved.controls=before.nativeControls[end];native=true;
                }
            }
            std::string detail;std::size_t inserted=0;
            if(ac::InsertPreservingShape(target.track,frame,inserted,detail)!=ac::InsertResult::Inserted) {
                return CurvePreparationFailure(error,L"此位置无法在当前权重限制内保持原曲线。");
            }
            if(native)target.track.keys[inserted].value=CameraSample(before,frame).value;
            if(camera) {
                target.nativeControls.assign(target.track.keys.size(),{});
                target.controlGeometry.assign(target.track.keys.size(),{-1,0,-1,0});
                for(std::size_t i=1;i<target.track.keys.size();++i) {
                    const auto original=std::lower_bound(before.track.keys.begin(),before.track.keys.end(),target.track.keys[i].frame,
                        [](const ac::Key& key,int f){return key.frame<f;});
                    const auto n=std::size_t(original-before.track.keys.begin());
                    if(original==before.track.keys.end()||!n||original->frame!=target.track.keys[i].frame||
                       before.track.keys[n-1].frame!=target.track.keys[i-1].frame)continue;
                    if(n<before.nativeControls.size())target.nativeControls[i]=before.nativeControls[n];
                    if(n<before.controlGeometry.size())target.controlGeometry[i]=before.controlGeometry[n];
                }
                std::vector<int> frames;for(const auto& key:target.track.keys)frames.push_back(key.frame);
                RemapPreserved(before,target,frames,false);
                if(native)for(const std::size_t i:{inserted,inserted+1}) {
                    auto part=preserved;part.first=target.track.keys[i-1].frame;part.last=target.track.keys[i].frame;
                    const auto position=[&](int f){return preserved.sampleFirst==preserved.first&&preserved.sampleLast==preserved.last?double(f):
                        preserved.sampleFirst+(double(f)-preserved.first)/
                        (preserved.last-preserved.first)*(preserved.sampleLast-preserved.sampleFirst);};
                    part.sampleFirst=position(part.first);part.sampleLast=position(part.last);
                    part.geometry=IntervalGeometry(target.track.keys[i-1],target.track.keys[i]);
                    auto& slices=target.preservedCameraIntervals;
                    slices.erase(std::remove_if(slices.begin(),slices.end(),[&](const auto& value){return value.first==part.first&&value.last==part.last;}),slices.end());
                    slices.push_back(part);
                }
            }
            PrunePreserved(target);
            if(!ac::Validate(target.track,detail))return CurvePreparationFailure(error,L"细分结果无效，原曲线已保留。");
            if(c==selected)resultIndex=inserted;
        }
        channels.swap(next);index=resultIndex;return true;
    }catch(const std::exception&){return CurvePreparationFailure(error,L"无法准备保形插帧，原曲线已保留。");}
}
bool PrepareMotionChannels(const std::vector<Channel>& input,std::vector<Channel>& output,std::wstring& error,bool selectedOnly,
                           std::uint64_t maximumSampleBytes) {
    try {
        bool canonical=true;
        for(const auto& channel:input)if((!selectedOnly||channel.selected)&&channel.sample&&!CanonicalCameraTiming(channel))canonical=false;
        const bool raw=canonical&&curve_motion::CanCaptureNativeCamera(input,selectedOnly);
        const auto exactSlice=[](const Channel& channel){
            return std::any_of(channel.preservedCameraIntervals.begin(),channel.preservedCameraIntervals.end(),
                [&](const auto& value){return PreservedCurrent(channel,value);});
        };
        const auto sampled=[&](const Channel& channel){
            if((selectedOnly&&!channel.selected)||channel.track.keys.empty())return false;
            if(channel.sample&&!CanonicalCameraTiming(channel))return true;
            if(raw)return false;
            if(CanonicalCameraTiming(channel)&&curve_motion::CanTransportCameraTiming(channel))return false;
            if(exactSlice(channel)){
                if(!channel.sample)throw std::invalid_argument("native camera slice has no sampler");
                return true;
            }
            if(CanonicalCameraTiming(channel))for(std::size_t end=1;end<channel.track.keys.size();++end)
                if(NativeGeometry(channel,end))return true;
            return false;
        };
        std::uint64_t sampleBytes=0;
        for(const auto& channel:input){
            if(!sampled(channel))continue;
            const auto first=channel.track.keys.front().frame,last=channel.track.keys.back().frame;
            if(first<0||last<first||last>frame_limits::maximum)throw std::invalid_argument("native camera sample range");
            const auto count=std::uint64_t(last)-std::uint64_t(first)+1;
            if(count>(std::numeric_limits<std::size_t>::max)()/sizeof(ac::Key))throw std::bad_alloc();
            // Keep the existing explicit-slice/opaque callback preparation
            // budget. Ordinary native VMD did not have this extra frame cap:
            // mixed native/scalar output remains bounded by the standard
            // codec's unchanged addressable-memory/frame/record limits.
            if((exactSlice(channel)||!CanonicalCameraTiming(channel))&&count>(maximumSampleBytes-sampleBytes)/sizeof(ac::Key)){
                return CurvePreparationFailure(error,L"原生相机逐帧采样超过本次准备内存预算，请缩短导出范围或先显式烘焙。");
            }
            if(exactSlice(channel)||!CanonicalCameraTiming(channel))sampleBytes+=count*sizeof(ac::Key);
        }
        auto next=input;
        for(auto& channel:next) {
            if(!sampled(channel))continue;
            PrunePreserved(channel);
            ac::Track baked;baked.discrete=channel.track.discrete;
            const auto first=channel.track.keys.front().frame,last=channel.track.keys.back().frame;
            baked.keys.reserve(std::size_t(last)-first+1);
            for(std::int64_t frame=first;frame<=last;++frame) {
                ac::Key key;key.frame=int(frame);key.value=SampleValue(channel,double(frame));
                key.interpolation=baked.discrete?ac::Interpolation::Step:ac::Interpolation::Linear;
                key.tangent=ac::Tangent::Free;baked.keys.push_back(key);
            }
            std::string detail;if(!ac::Validate(baked,detail))throw std::invalid_argument("invalid baked camera samples");
            channel.track=std::move(baked);channel.sample={};channel.nativeControls.clear();channel.controlGeometry.clear();
            channel.preservedCameraIntervals.clear();
        }
        output.swap(next);return true;
    }catch(const std::exception&){return CurvePreparationFailure(error,L"无法准备原生曲线的逐帧导出，原数据已保留。");}
}
bool RetimePreservedChannel(const Channel& source,Channel& transformed,std::wstring& error) {
    try {
        if(source.preservedCameraIntervals.empty())return true;
        std::string detail;
        if(source.track.keys.size()!=transformed.track.keys.size()||!ac::Validate(transformed.track,detail)) {
            return CurvePreparationFailure(error,L"精确保形相机的区间边界发生合并或删除，请先显式烘焙后再转换。");
        }
        auto next=transformed;std::vector<int> oldFrames;oldFrames.reserve(source.track.keys.size());
        for(const auto& key:source.track.keys)oldFrames.push_back(key.frame);
        RemapPreserved(source,next,oldFrames,true);
        next.nativeControls=source.nativeControls;next.controlGeometry=source.controlGeometry;next.sample=source.sample;
        for(std::size_t i=1;i<next.controlGeometry.size()&&i<next.track.keys.size();++i){
            const double ratio=double(source.track.keys[i].frame-source.track.keys[i-1].frame)/
                (next.track.keys[i].frame-next.track.keys[i-1].frame);
            next.controlGeometry[i][1]*=ratio;next.controlGeometry[i][3]*=ratio;
        }
        transformed=std::move(next);return true;
    }catch(const std::exception&){return CurvePreparationFailure(error,L"无法映射精确保形相机区间，请先显式烘焙后再转换。");}
}
bool Write(MMDApp&app,Kind kind,int slot,const std::vector<Channel>&input,std::wstring&error){
#if !MMD_ORIGINAL_PROFILE
    if(plugins::BoundNativePoseHistory(&app))return plugins::WriteBoundGlobalCurves(app,kind,slot,input,error);
#endif
    if(!Identity(app,kind,slot))return false;
    const auto&f=Fields(kind);
    if(input.size()!=f.size())return false;
    std::set<int> frames{0};std::string detail;
    for(size_t c=0;c<input.size();++c){
        const auto&t=input[c].track;
        if(input[c].id!=f[c].id||t.discrete!=f[c].discrete||!ac::Validate(t,detail)||t.keys.empty()||t.keys.front().frame!=0){
            error=Error(app,L"各トラックの 0 フレームキーを保持してください。",L"Keep a valid frame-zero key in every track.",L"每个轨道须保留有效的第 0 帧关键帧。");return false;}
        for(const auto&k:t.keys){
            if(k.frame>mikumikustudio::frame_limits::maximum||k.value<f[c].minimum||k.value>f[c].maximum||(f[c].discrete&&std::floor(k.value)!=k.value)){
                error=Error(app,L"値またはフレームが範囲外です。",L"Value or frame is outside the allowed range.",L"数值或帧号超出允许范围。");return false;}
            frames.insert(k.frame);
        }
    }
    const auto stateKey=std::make_tuple(&app,kind,slot);
    auto stateSlot=states.try_emplace(stateKey);
    PoolTransaction transaction;
    try {
    State next;next.identity=Identity(app,kind,slot);next.channels=input;
    for(auto& channel:next.channels)PrunePreserved(channel);
    switch(kind){
    case Kind::Camera:
        transaction.Begin(app.CameraKeys());
        Mirror(app.CameraKeys(),static_cast<const mdl::CameraKey*>(transaction.before),input,frames);transaction.Updated(app.CameraKeys());
        MirrorCameraControls(app.CameraKeys(),frames.size(),input);
        break;
    case Kind::Light:transaction.Begin(app.LightKeys());Mirror(app.LightKeys(),static_cast<const mdl::LightKey*>(transaction.before),input,frames);transaction.Updated(app.LightKeys());break;
    case Kind::Shadow:transaction.Begin(app.ShadowKeys());Mirror(app.ShadowKeys(),static_cast<const mdl::SelfShadowKey*>(transaction.before),input,frames);transaction.Updated(app.ShadowKeys());break;
    case Kind::Gravity:transaction.Begin(app.GravityKeys());Mirror(app.GravityKeys(),static_cast<const mdl::GravityKey*>(transaction.before),input,frames);transaction.Updated(app.GravityKeys());break;
    case Kind::Accessory:transaction.Begin(app.AccessoryKeys(slot));Mirror(app.AccessoryKeys(slot),static_cast<const mdl::AccessoryKey*>(transaction.before),input,frames);transaction.Updated(app.AccessoryKeys(slot));break;
    }
    next.identity=kind==Kind::Accessory ? app.AccessorySlot(slot) : transaction.FinalIdentity();
#if !MMD_ORIGINAL_PROFILE
    // A same-capacity write copies back into the old pool on destruction;
    // only a larger write publishes the staged allocation.
    next.poolAllocation=transaction.FinalIdentity();
    next.poolIdentity=PoolIdentity(kind,next.poolAllocation);
#endif
    if (!NativeRead(app,kind,slot,next.baseline)) throw std::runtime_error("Invalid mirrored key track");
    next.fingerprint=Fingerprint(app,kind,slot);
    stateSlot.first->second=std::move(next);
    transaction.committed=true;
    switch(kind){
    case Kind::Camera:app.CameraTrackCursor()=0;app.CameraTrackActive()=1;break;
    case Kind::Light:app.LightTrackCursor()=0;app.LightTrackActive()=1;break;
    case Kind::Shadow:app.ShadowTrackCursor()=0;app.ShadowTrackActive()=1;break;
    case Kind::Gravity:app.GravityTrackCursor()=0;app.GravityTrackActive()=1;break;
    case Kind::Accessory:app.AccessoryTrackCursor(slot)=0;app.AccessoryTrackActive(slot)=1;break;
    }
    app.LastRegisteredFrame()=std::max(app.LastRegisteredFrame(),uint32_t(*frames.rbegin()));
    app.SceneModified()=1;
    return true;
    } catch(const std::exception&) {
        // If Mirror grew its candidate before a later allocation failed, the
        // current identity still owns that new candidate.
        transaction.after=kind==Kind::Accessory ? static_cast<void*>(app.AccessoryKeys(slot)) : Identity(app,kind,slot);
        if(stateSlot.second)states.erase(stateSlot.first);
        error=Error(app,L"メモリ不足のため編集を適用できません。",L"Not enough memory to apply the edit.",L"内存不足，无法应用编辑，原关键帧已保留。");
        return false;
    }
}
void Apply(MMDApp*app,Kind kind,double frame,int slot){
    if(!app||!std::isfinite(frame)||states.empty())return;
#if !MMD_ORIGINAL_PROFILE
    plugins::AccessoryAttachmentEvaluation attachment(kind==Kind::Accessory?app:nullptr,unsigned(slot));
    if(!attachment.CanApply())return;
    if(kind==Kind::Accessory&&plugins::BoundNativePoseHistory(app)){
        const auto found=states.find({app,kind,slot});
        if(found!=states.end())for(const auto* channels:{&found->second.channels,&found->second.baseline})
            for(const auto& channel:*channels)if(channel.sample&&!CanonicalCameraTiming(channel))return;
    }
#endif
    State*s=Current(*app,kind,slot);if(!s)return;
#if !MMD_ORIGINAL_PROFILE
    if(!attachment.CanApply())return;
#endif
    double v[12]{};const auto&fields=Fields(kind);
    const bool continuousCamera = kind == Kind::Camera && app->PlaybackActive() && app->PlaybackTiming().interpolate;
    for(size_t c=0;c<s->channels.size();++c){
        const auto& channel = s->channels[c];
        const auto sampler = channel.sample.target<ac::Sample(*)(const Channel&,double)>();
        v[c] = continuousCamera && sampler && *sampler == CameraSample
            ? CameraSampleImpl(channel, frame, true).value : SampleValue(channel, frame);
#if !MMD_ORIGINAL_PROFILE
        if(!attachment.CanApply())return;
#endif
        if(!std::isfinite(v[c]))return;v[c]=std::clamp(v[c],fields[c].minimum,fields[c].maximum);}
    switch(kind){
    case Kind::Camera:
        for(int i=0;i<3;++i){app->CameraPositionAxis(i)=float(v[i]);app->CameraRotation()[i]=float(v[i+3]*pi/180);}
        app->CameraDistance()=float(v[6]);app->CameraFov()=float(v[7]);app->CameraPerspective()=uint8_t(v[8]);
        app->CameraParentModel()=int(v[9]);app->CameraParentBone()=int(v[10]);break;
    case Kind::Light:
        for(int i=0;i<3;++i){app->LightDirection()[i]=float(v[i]);app->LightColor()[i]=float(v[i+3]);}
        app->ApplyTimelineLightState();
        if(app->Renderer()&&app->Renderer()->device)app->Renderer()->device->SetLight(0,&app->SceneLight());break;
    case Kind::Shadow:app->ShadowMode()=int(v[0]);app->ShadowDistance()=float(v[1]);break;
    case Kind::Gravity:app->GravityMagnitude()=float(v[0]);for(int i=0;i<3;++i)app->GravityDirection()[i]=float(v[i+1]);
        app->GravityNoise()=int(v[4]);app->GravityNoiseEnabled()=uint8_t(v[5]);break;
    case Kind::Accessory:{auto&a=*app->AccessorySlot(slot);
        for(int i=0;i<3;++i){a.position[i]=float(v[i]);a.rotation[i]=float(v[i+3]*pi/180);}
        a.scale=float(v[6]);a.opacity=float(v[7]);a.visible=uint8_t(v[8]);a.shadowEnabled=uint8_t(v[9]);a.parentModel=int(v[10]);a.parentBone=int(v[11]);
#if !MMD_ORIGINAL_PROFILE
        attachment.Complete({int(v[10]),int(v[11])});
#endif
        break;}
    }
}
bool Has(MMDApp*app,Kind kind,int slot){return states.find({app,kind,slot})!=states.end();}
void Reset(MMDApp*app){
    if(app)curve_editor::CloseOwned(app->Hwnd());
    for(auto it=states.begin();it!=states.end();)if(std::get<0>(it->first)==app)it=states.erase(it);else ++it;
}
void ForgetAccessory(MMDApp*app,int slot){states.erase({app,Kind::Accessory,slot});if(app)curve_editor::CloseOwned(app->Hwnd());}
void Capture(MMDApp&app,pmm_extension::Document&doc){
    std::vector<ac::NamedTrack> tracks;
    PreservedCameraTable preserved;
    for(auto&entry:states){if(std::get<0>(entry.first)!=&app)continue;
        Kind k=std::get<1>(entry.first);int slot=std::get<2>(entry.first);
        if(!StateIdentityCurrent(entry.second,app,k,slot))continue;
        const auto* current=Current(app,k,slot);
        if(!current)continue; // Identity was checked above; no deleted object is saved.
        for(std::size_t i=0;i<current->channels.size();++i){const auto&c=current->channels[i];
            tracks.push_back({std::to_string(int(k))+"/"+std::to_string(slot)+"/"+c.id,c.track});
            for(const auto& interval:c.preservedCameraIntervals)if(PreservedCurrent(c,interval))
                preserved[{int(k),slot,int(i)}].push_back(interval);
        }
    }
    std::vector<uint8_t> bytes;std::string error;
    if(ac::Encode(tracks,bytes,error))pmm_extension::Set(doc,"org.mikumikustudio.animation_curves.v1",std::move(bytes));
    else throw std::runtime_error("Curve serialization failed: "+error);
    if(preserved.empty())pmm_extension::Erase(doc,preservedCameraRecord);
    else pmm_extension::Set(doc,preservedCameraRecord,EncodePreservedCamera(preserved));
}
namespace {
using PmmParentRemaps = std::array<const PmmModelAdaptation*, 255>;

PmmParentRemaps ParentRemaps(const std::vector<PmmModelAdaptation>& adaptations) {
    PmmParentRemaps result{};
    std::array<bool, 255> seen{};
    for (const auto& adaptation : adaptations) {
        if (adaptation.slot < 0 || adaptation.slot >= int(result.size()) || seen[adaptation.slot])
            throw std::runtime_error("PMM model adaptation slot");
        seen[adaptation.slot] = true;
        if (adaptation.remapBones) result[adaptation.slot] = &adaptation;
    }
    return result;
}

double RemappedParentBone(const MMDApp& app, const PmmParentRemaps& remaps,
                          double model, double bone) noexcept {
    if (model < 0 || model >= double(remaps.size())) return bone;
    const auto* remap = remaps[std::size_t(model)];
    if (!remap) return bone;
    if (bone < 0 || bone >= double(remap->boneIndices.size())) return 0;
    const int target = remap->boneIndices[std::size_t(bone)];
    const auto* object = app.ModelSlot(remap->slot);
    if (target < 0 || !object || std::uint32_t(target) >= mdl::Mdl(object)->boneCount) return 0;
    return target;
}

// A parent-model change can change the mapped bone without a parent-bone key.
// Sweep both original step tracks once. Never sample a partially remapped copy
// or apply one model's mapping to the result of another model's mapping.
void RemapPmmParents(const MMDApp& app, Kind kind, std::vector<Channel>& channels,
                     const PmmParentRemaps& remaps) {
    if (kind != Kind::Camera && kind != Kind::Accessory) return;
    const std::size_t modelChannel = kind == Kind::Camera ? 9 : 10;
    const std::size_t boneChannel = modelChannel + 1;
    if (channels.size() <= boneChannel) throw std::runtime_error("PMM parent channels");
    const auto& models = channels[modelChannel].track;
    const auto& bones = channels[boneChannel].track;
    if (!models.discrete || !bones.discrete || models.keys.empty() || bones.keys.empty())
        throw std::runtime_error("PMM parent step tracks");
    const bool affected = std::any_of(models.keys.begin(), models.keys.end(), [&](const auto& key) {
        return key.value >= 0 && key.value < double(remaps.size()) && remaps[std::size_t(key.value)];
    });
    if (!affected) return;

    std::vector<ac::Key> mapped;
    if (models.keys.size() > mapped.max_size() - bones.keys.size())
        throw std::runtime_error("PMM parent key count");
    mapped.reserve(models.keys.size() + bones.keys.size());
    std::size_t m = 0, b = 0;
    double model = models.keys.front().value, bone = bones.keys.front().value;
    while (m < models.keys.size() || b < bones.keys.size()) {
        const auto frame = m == models.keys.size() ? bones.keys[b].frame :
            b == bones.keys.size() ? models.keys[m].frame :
            (std::min)(models.keys[m].frame, bones.keys[b].frame);
        if (m < models.keys.size() && models.keys[m].frame == frame) model = models.keys[m++].value;
        const bool authoredBone = b < bones.keys.size() && bones.keys[b].frame == frame;
        ac::Key key;
        if (authoredBone) { key = bones.keys[b++]; bone = key.value; }
        const double value = RemappedParentBone(app, remaps, model, bone);
        if (!authoredBone && !mapped.empty() && mapped.back().value == value) continue;
        key.frame = frame; key.value = value;
        if (!authoredBone) {
            key.interpolation = ac::Interpolation::Step; key.tangent = ac::Tangent::Free;
            if (b && b < bones.keys.size()) {
                // Reuse the surrounding interval's valid handle sums. Handles
                // do not affect a step track, but the curve codec validates them.
                key.inWeight = bones.keys[b].inWeight;
                key.outWeight = bones.keys[b - 1].outWeight;
            } else if (!mapped.empty() && mapped.back().outWeight + key.inWeight > .9999) {
                // The former terminal out-handle had no interval or bound on
                // its sum. Only that previously unused handle needs adjustment.
                mapped.back().outWeight = 1.0 / 3.0;
            }
        }
        mapped.push_back(key);
    }
    channels[boneChannel].track.keys.swap(mapped);
}
}
void Load(MMDApp&app,const pmm_extension::Document&doc,
          const std::vector<PmmModelAdaptation>& adaptations){
    const auto parentRemaps = ParentRemaps(adaptations);
    Reset(&app);const auto*r=pmm_extension::Find(doc,"org.mikumikustudio.animation_curves.v1");if(!r)return;
    std::vector<ac::NamedTrack> tracks;std::string error;
    if(!ac::Decode(r->value.data(),r->value.size(),tracks,error)){runtime_log::Write("Invalid PMM curves: %s",error.c_str());return;}
    PreservedCameraTable preserved;
    if(const auto* exact=pmm_extension::Find(doc,preservedCameraRecord))
        if(!DecodePreservedCamera(exact->value,preserved))runtime_log::Write("Invalid PMM native camera intervals; exact interval record ignored");
    std::map<std::pair<Kind,int>,std::vector<ac::NamedTrack>> groups;
    for(const auto&t:tracks){
        auto first=t.id.find('/'),last=t.id.rfind('/');if(first==std::string::npos||first==last)continue;
        try{int k=std::stoi(t.id.substr(0,first)),s=std::stoi(t.id.substr(first+1,last-first-1));
            if(k<0||k>4||s<0||s>254)continue;auto copy=t;copy.id=t.id.substr(last+1);groups[{Kind(k),s}].push_back(std::move(copy));
        }catch(...){continue;}
    }
    for(const auto&g:groups){State next;auto k=g.first.first;int slot=g.first.second;
        if(!Identity(app,k,slot)||g.second.size()!=Fields(k).size())continue;
        const bool nativeAvailable=NativeRead(app,k,slot,next.baseline);
        if(!nativeAvailable){
            // Restore extension tracks even if native interpolation is not
            // representable. Empty baselines allow later native edits to merge.
            next.baseline.clear();next.baseline.resize(Fields(k).size());
            for(size_t c=0;c<next.baseline.size();++c){
                next.baseline[c].id=Fields(k)[c].id;
                next.baseline[c].track.discrete=Fields(k)[c].discrete;
            }
            runtime_log::Write("Restoring authored PMM curves without native interpolation: kind=%d slot=%d",int(k),slot);
        }
        next.channels=next.baseline;bool valid=true;
        for(auto&c:next.channels){auto t=std::find_if(g.second.begin(),g.second.end(),[&](const auto&v){return v.id==c.id;});
            if(t==g.second.end()||t->track.keys.empty()||t->track.keys[0].frame!=0||t->track.discrete!=c.track.discrete){valid=false;break;}
            const auto index=size_t(&c-next.channels.data());const auto&field=Fields(k)[index];
            for(const auto&key:t->track.keys)if(key.frame>mikumikustudio::frame_limits::maximum||key.value<field.minimum||key.value>field.maximum||
                (field.discrete&&std::floor(key.value)!=key.value)){valid=false;break;}
            if(!valid)break;c.track=t->track;
            const auto exact=preserved.find({int(k),slot,int(index)});
            if(exact!=preserved.end()){
                c.preservedCameraIntervals=exact->second;
                for(const auto& interval:c.preservedCameraIntervals)if(!PreservedCurrent(c,interval)||
                    interval.sourceValueFirst<field.minimum||interval.sourceValueFirst>field.maximum||
                    interval.sourceValueLast<field.minimum||interval.sourceValueLast>field.maximum||
                    SamplePreservedSource(interval,interval.first).value!=interval.geometry[0]||
                    SamplePreservedSource(interval,interval.last).value!=interval.geometry[3]){valid=false;break;}
                c.sample=CameraSample;
            }
        }
        if(valid){
            // Everything remains private until both transformations succeed.
            // The native success tail remaps its own records afterwards. Give
            // reconciliation that same expected baseline, while retaining the
            // old fingerprint so the first post-tail read checks the real pool.
            RemapPmmParents(app,k,next.channels,parentRemaps);
            if(nativeAvailable)RemapPmmParents(app,k,next.baseline,parentRemaps);
            CaptureStateIdentity(next,app,k,slot);next.fingerprint=nativeAvailable?Fingerprint(app,k,slot):0;states[{&app,k,slot}]=std::move(next);
        }
    }
}

// ---------------------------------------------------------------------------
// Native frame retime support (see animation_curve_host.hpp)
// ---------------------------------------------------------------------------
//
// The authoring of one application is exchanged as a whole table, and the
// exchange moves map nodes rather than copying them: installing is one exchange
// and rolling a commit back is the same exchange called again, so neither
// allocates and neither can fail half way.  Without this the baseline would keep
// the pre-conversion frames and the next read would reconcile the whole authored
// curve away.

struct AuthoringTable::Impl {
    std::map<Address, State> entries;
};

AuthoringTable::AuthoringTable() : impl_(new Impl()) {}
AuthoringTable::~AuthoringTable() = default;
AuthoringTable::AuthoringTable(AuthoringTable&&) noexcept = default;
AuthoringTable& AuthoringTable::operator=(AuthoringTable&&) noexcept = default;

namespace {

std::uint64_t AuthoringDigest(const State& state) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) { hash ^= value; hash *= 1099511628211ull; };
    const auto mixChannels = [&](const std::vector<Channel>& channels) {
        for (const Channel& channel : channels) {
            for (const char character : channel.id) mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(character)));
            mix(channel.track.discrete ? 1u : 0u);
#if !MMD_ORIGINAL_PROFILE
            // These change evaluation/linking without necessarily changing any
            // native field. Keep them in history's semantic source fingerprint.
            mix(channel.interpolationGroup.size());
            for(const unsigned char character:channel.interpolationGroup)mix(character);
            const auto sampler=channel.sample.target<ac::Sample(*)(const Channel&,double)>();
            mix(!channel.sample?0u:sampler&&*sampler==CameraSample?1u:2u);
            mix(std::uint32_t(channel.group));mix(channel.collections.size());
            for(const auto& collection:channel.collections){
                mix(collection.id.size());for(const unsigned char character:collection.id)mix(character);
                mix(collection.label.size());for(const wchar_t character:collection.label)mix(std::uint32_t(character));
            }
#endif
            for(const auto&controls:channel.nativeControls)for(double value:controls){
                std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));mix(bits);
            }
            for(const auto&geometry:channel.controlGeometry)for(double value:geometry){
                std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));mix(bits);
            }
            mix(channel.preservedCameraIntervals.size());
            for(const auto& interval:channel.preservedCameraIntervals){
                for(int value:{interval.first,interval.last,interval.sourceFirst,interval.sourceLast})mix(std::uint32_t(value));
                const auto number=[&](double value){std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof bits);mix(bits);};
                for(double value:{interval.sampleFirst,interval.sampleLast,interval.sourceValueFirst,interval.sourceValueLast})number(value);
                for(double value:interval.controls)number(value);for(double value:interval.geometry)number(value);
                mix(interval.oneFrameHold?1u:0u);mix(interval.transportTiming?1u:0u);
            }
            for (const ac::Key& key : channel.track.keys) {
                mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.frame)));
                std::uint64_t bits = 0;
                std::memcpy(&bits, &key.value, sizeof(bits));
                mix(bits);
                std::memcpy(&bits, &key.inSlope, sizeof(bits));
                mix(bits);
                std::memcpy(&bits, &key.outSlope, sizeof(bits));
                mix(bits);
                std::memcpy(&bits, &key.inWeight, sizeof(bits));
                mix(bits);
                std::memcpy(&bits, &key.outWeight, sizeof(bits));
                mix(bits);
                mix(static_cast<std::uint64_t>(key.interpolation));
                mix(static_cast<std::uint64_t>(key.tangent));
            }
        }
    };
    mixChannels(state.channels);
    mixChannels(state.baseline);
    return hash;
}

}  // namespace

void ReconcileRegisteredFrame(MMDApp& app,Kind kind,int slot,int frame) {
    ReconcileRegisteredFrames(app,kind,slot,{frame});
}
void ReconcileRegisteredFrames(MMDApp& app,Kind kind,int slot,const std::vector<int>& frames) {
    ValidateRegistrationFrames(frames);if(frames.empty())return;
    const auto found=states.find({&app,kind,slot});
    if(found==states.end()||!StateIdentityCurrent(found->second,app,kind,slot))return;
    std::vector<Channel> native;
    if(!NativeRead(app,kind,slot,native))throw std::invalid_argument("registered native track");
    State next=found->second;
    ForceRegistrationFrames(next,native,frames);Reconcile(next,native);
    if(AuthoringDigest(next)==AuthoringDigest(found->second))return;
    next.revision=NextAuthoringRevision();next.fingerprint=Fingerprint(app,kind,slot);
    found->second=std::move(next);
}

bool CaptureAuthoring(MMDApp& app, AuthoringTable& out, std::wstring& error) {
    error.clear();
    if (!out.impl_) out.impl_.reset(new AuthoringTable::Impl());
    out.impl_->entries.clear();
    for (const auto& entry : states) {
        if (std::get<0>(entry.first) != &app) continue;
        const Kind kind = std::get<1>(entry.first);
        const int slot = std::get<2>(entry.first);
        // A track whose object is gone is not this app's authoring any more.
        if (!StateIdentityCurrent(entry.second, app, kind, slot)) continue;
        out.impl_->entries.insert(entry);
    }
    return true;
}

bool AuthoringEntries(const AuthoringTable& table, std::vector<AuthoringEntry>& out) {
    out.clear();
    if (!table.impl_) return true;
    for (const auto& entry : table.impl_->entries) {
        AuthoringEntry item;
        item.kind = std::get<1>(entry.first);
        item.slot = std::get<2>(entry.first);
        State effective = entry.second;
        std::vector<Channel> native;
        if (!NativeRead(*std::get<0>(entry.first), item.kind, item.slot, native)) return false;
        Reconcile(effective, native);
        item.channels = std::move(effective.channels);
        item.baseline = std::move(effective.baseline);
        out.push_back(std::move(item));
    }
    return true;
}

bool SampleRetimedKey(const AuthoringEntry& entry, void* record, std::size_t bytes) {
    const auto& fields = Fields(entry.kind);
    if (!record || entry.channels.size() != fields.size()) return false;
    const auto sample = [&](auto key) {
        if (bytes != sizeof(key)) return false;
        std::memcpy(&key, record, sizeof(key));
        for (std::size_t channel = 0; channel < fields.size(); ++channel) {
            if (entry.channels[channel].id != fields[channel].id) return false;
            const double value = SampleValue(entry.channels[channel], key.frame);
            if (!std::isfinite(value)) return false;
            Put(key, channel, std::clamp(value, fields[channel].minimum, fields[channel].maximum));
        }
        std::memcpy(record, &key, sizeof(key));
        return true;
    };
    switch (entry.kind) {
    case Kind::Camera: return sample(mdl::CameraKey{});
    case Kind::Light: return sample(mdl::LightKey{});
    case Kind::Shadow: return sample(mdl::SelfShadowKey{});
    case Kind::Gravity: return sample(mdl::GravityKey{});
    case Kind::Accessory: return sample(mdl::AccessoryKey{});
    }
    return false;
}

bool RebuildAuthoring(MMDApp& app, const std::vector<AuthoringEntry>& entries,
                      AuthoringTable& out, std::wstring& error) {
    error.clear();
    if (!out.impl_) out.impl_.reset(new AuthoringTable::Impl());
    out.impl_->entries.clear();
    for (const AuthoringEntry& item : entries) {
        if (Identity(app, item.kind, item.slot) == nullptr) continue;
        if (item.channels.size() != Fields(item.kind).size()) continue;
        State next;
        CaptureStateIdentity(next, app, item.kind, item.slot);
        next.channels = item.channels;
        std::vector<Channel> baseline;
        if (!NativeRead(app, item.kind, item.slot, baseline)) continue;
        next.baseline = std::move(baseline);
        // The retimed pools are installed by the time this runs, so the
        // fingerprint follows them and the next read finds no difference.
        next.fingerprint = Fingerprint(app, item.kind, item.slot);
        out.impl_->entries[{&app, item.kind, item.slot}] = std::move(next);
    }
    return true;
}

struct AccessoryAuthoring::Impl {
    Address key;
    decltype(states)::node_type node;
    bool present = true;
    std::uint64_t generation = 1;
    Impl(MMDApp& app, int slot) : key(&app, Kind::Accessory, slot) {}
};
struct AccessoryAuthoring::Exchange::Impl {
    std::shared_ptr<AccessoryAuthoring::Impl> retainedOwner;
    AccessoryAuthoring::Impl& owner;
    const State* before;
    void* identity;
    mdl::AccessoryKey* keys;
    std::uint64_t digest, revision, fingerprint, generation;
    bool present, consumed = false;
    Impl(std::shared_ptr<AccessoryAuthoring::Impl> value, bool direction)
        : retainedOwner(std::move(value)), owner(*retainedOwner), present(direction), generation(owner.generation) {
        if (present == owner.present) throw std::invalid_argument("accessory authoring presence");
        const auto found = states.find(owner.key);
        before = found == states.end() ? nullptr : &found->second;
        if (present && before) throw std::invalid_argument("accessory slot authoring occupied");
        digest = before ? AuthoringDigest(*before) : 0;
        revision = before ? before->revision : 0;
        auto& app = *std::get<0>(owner.key);
        const auto slot = std::get<2>(owner.key);
        identity = Identity(app, Kind::Accessory, slot);
        keys = app.AccessoryKeys(slot);
        fingerprint = Fingerprint(keys);
    }
    bool Current() const noexcept {
        if (consumed || owner.generation != generation || present == owner.present) return false;
        auto& app = *std::get<0>(owner.key);
        const auto slot = std::get<2>(owner.key);
        if (Identity(app, Kind::Accessory, slot) != identity || app.AccessoryKeys(slot) != keys ||
            Fingerprint(keys) != fingerprint) return false;
        const auto found = states.find(owner.key);
        const auto* now = found == states.end() ? nullptr : &found->second;
        return now == before && (!now || (now->revision == revision && AuthoringDigest(*now) == digest));
    }
    bool Publish() noexcept {
        if (!Current()) return false;
        if (present) { if (!owner.node.empty()) states.insert(std::move(owner.node)); }
        else owner.node = states.extract(owner.key);
        owner.present = present; ++owner.generation; consumed = true;
        return true;
    }
};
AccessoryAuthoring::AccessoryAuthoring(MMDApp& app, int slot, bool initiallyPresent) : impl_(std::make_shared<Impl>(app, slot)) {
    if (slot < 0 || slot >= 255) throw std::invalid_argument("accessory authoring slot");
    if (!initiallyPresent && (app.AccessorySlot(slot) || states.count(impl_->key)))
        throw std::invalid_argument("import accessory authoring slot occupied");
    impl_->present = initiallyPresent;
}
AccessoryAuthoring::~AccessoryAuthoring() = default;
#if !MMD_ORIGINAL_PROFILE
void AccessoryAuthoring::MeasureResources(plugins::resource_budget::Collector& out) const {
    out.Array(this, 1); out.Array(impl_.get(), 1);
    const State* state = nullptr;
    if (impl_->present) {
        const auto found = states.find(impl_->key);
        if (found != states.end()) state = &found->second;
    } else if (!impl_->node.empty()) state = &impl_->node.mapped();
    if (!state) return;
    out.Add(state, 1, sizeof(State) + sizeof(Address) + 4 * sizeof(void*));
    for (const auto* channels : {&state->channels, &state->baseline}) {
        out.Vector(*channels);
        for (const auto& c : *channels) {
            out.String(c.id); out.String(c.label); out.String(c.unit); out.String(c.interpolationGroup);
            out.Vector(c.track.keys); out.Vector(c.nativeControls); out.Vector(c.controlGeometry);out.Vector(c.preservedCameraIntervals);
            out.Vector(c.collections);
            for (const auto& group : c.collections) { out.String(group.id); out.String(group.label); }
            if (c.sample) out.MissingResource(plugins::resource_budget::CurveCallback);
        }
    }
}
#endif
std::unique_ptr<AccessoryAuthoring::Exchange> AccessoryAuthoring::Prepare(bool present) {
    return std::unique_ptr<Exchange>(new Exchange(std::make_unique<Exchange::Impl>(impl_, present)));
}
AccessoryAuthoring::Exchange::Exchange(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AccessoryAuthoring::Exchange::~Exchange() = default;
bool AccessoryAuthoring::Exchange::IsCurrent() const noexcept { return impl_->Current(); }
bool AccessoryAuthoring::Exchange::Publish() noexcept { return impl_->Publish(); }

void SwapAuthoring(MMDApp& app, AuthoringTable& table) noexcept {
    if (!table.impl_) return;
    std::map<Address, State> mine;
    for (auto it = states.begin(); it != states.end();) {
        if (std::get<0>(it->first) == &app) mine.insert(states.extract(it++));
        else ++it;
    }
    for (auto it = table.impl_->entries.begin(); it != table.impl_->entries.end();) {
        try {
            states.insert(table.impl_->entries.extract(it++));
        } catch (...) {
            return;
        }
    }
    table.impl_->entries.swap(mine);
}

bool AuthoringUnchanged(MMDApp& app, const AuthoringTable& table) {
    if (!table.impl_) return true;
    std::map<Address, State> live;
    for (const auto& entry : states) {
        if (std::get<0>(entry.first) == &app &&
            StateIdentityCurrent(entry.second, app, std::get<1>(entry.first), std::get<2>(entry.first)))
            live.insert(entry);
    }
    if (live.size() != table.impl_->entries.size()) return false;
    auto left = live.begin();
    auto right = table.impl_->entries.begin();
    for (; left != live.end(); ++left, ++right) {
        if (left->first != right->first) return false;
        if (!SameStateIdentity(left->second, right->second, std::get<1>(left->first))) return false;
        if (left->second.fingerprint != right->second.fingerprint) return false;
        if (AuthoringDigest(left->second) != AuthoringDigest(right->second)) return false;
    }
    return true;
}

#if !MMD_ORIGINAL_PROFILE
namespace {
template<class K> std::vector<Channel> TargetChannels(const NativeKeyView& view, Kind kind) {
    if(!view.key||!view.capacity)throw std::invalid_argument("target native track");
    const auto& fields=Fields(kind);std::vector<Channel> result(fields.size());
    for(std::size_t c=0;c<fields.size();++c){result[c].id=fields[c].id;result[c].track.discrete=fields[c].discrete;}
    std::uint32_t index=0,previous=0,lastFrame=0;std::vector<K> keys;
    for(std::size_t hops=0;;++hops) {
        if(hops>=view.capacity||index>=view.capacity)throw std::invalid_argument("target native chain");
        const auto* key=static_cast<const K*>(view.key(view.context,index));
        if(!key||key->frame>frame_limits::maximum||(!hops&&key->frame)||
           (hops&&(key->frame<=lastFrame||key->previous!=previous)))throw std::invalid_argument("target native order");
        keys.push_back(*key);
        for(std::size_t c=0;c<fields.size();++c){
            ac::Key point;point.frame=int(key->frame);point.value=Value(*key,c);point.tangent=ac::Tangent::Free;
            point.interpolation=fields[c].step?ac::Interpolation::Step:ac::Interpolation::Linear;
            if(!std::isfinite(point.value))throw std::invalid_argument("target native value");
            result[c].track.keys.push_back(point);
        }
        if(!key->next)break;previous=index;lastFrame=key->frame;index=key->next;
    }
    if constexpr(std::is_same_v<K,mdl::CameraKey>) {
        for(std::size_t c=0;c<8;++c) {
            auto& channel=result[c];channel.nativeControls.resize(keys.size());channel.controlGeometry.resize(keys.size());
            channel.sample=CameraSample;if(c>=3&&c<6)channel.interpolationGroup="camera/rotation";
            const int rootLane=c<3?int(c):c<6?3:c==6?4:5;
            for(int j=0;j<4;++j)channel.nativeControls[0][j]=keys[0].interpolation[j][rootLane];
            for(std::size_t end=1;end<keys.size();++end) {
                const int lane=c<3?int(c):c<6?3:c==6?4:5;
                auto& controls=channel.nativeControls[end];
                for(int j=0;j<4;++j){if(keys[end].interpolation[j][lane]>127)throw std::invalid_argument("camera controls");controls[j]=keys[end].interpolation[j][lane];}
                auto& a=channel.track.keys[end-1];auto& b=channel.track.keys[end];
                const double x1=std::clamp(controls[0]/127.0,0.0002,0.9996);
                const double x2=std::clamp(controls[2]/127.0,x1+0.0002,0.9998),rate=(b.value-a.value)/double(b.frame-a.frame);
                a.outWeight=x1;b.inWeight=1.0-x2;a.outSlope=controls[1]/127.0*rate/x1;
                b.inSlope=(1.0-controls[3]/127.0)*rate/(1.0-x2);b.interpolation=ac::Interpolation::Bezier;
                channel.controlGeometry[end]={a.outWeight,a.outSlope,b.inWeight,b.inSlope};
            }
        }
    }
    return result;
}
std::vector<Channel> TargetChannels(const NativeKeyView& view,Kind kind) {
    switch(kind){
    case Kind::Camera:return TargetChannels<mdl::CameraKey>(view,kind);
    case Kind::Light:return TargetChannels<mdl::LightKey>(view,kind);
    case Kind::Shadow:return TargetChannels<mdl::SelfShadowKey>(view,kind);
    case Kind::Gravity:return TargetChannels<mdl::GravityKey>(view,kind);
    default:return TargetChannels<mdl::AccessoryKey>(view,kind);
    }
}
std::uint64_t RetainedChannelsBytes(const std::vector<Channel>& channels) noexcept {
    std::uint64_t bytes=0;
    const auto add=[&](std::uint64_t n,std::uint64_t stride){bytes=stride&&n>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+n*stride;};
    const auto string=[&](const auto& s){add(s.capacity()+1,sizeof(typename std::decay_t<decltype(s)>::value_type));};
        add(channels.capacity(),sizeof(Channel));
        for(const auto& c:channels){string(c.id);string(c.label);string(c.unit);string(c.interpolationGroup);
            add(c.track.keys.capacity(),sizeof(ac::Key));add(c.nativeControls.capacity(),sizeof(std::array<double,4>));
            add(c.controlGeometry.capacity(),sizeof(std::array<double,4>));add(c.collections.capacity(),sizeof(curve_editor::Collection));
            add(c.preservedCameraIntervals.capacity(),sizeof(PreservedCameraInterval));
            for(const auto& group:c.collections){string(group.id);string(group.label);}
            if(c.sample){const auto p=c.sample.target<ac::Sample(*)(const Channel&,double)>();if(!p||*p!=CameraSample)return UINT64_MAX;}
        }
    return bytes;
}
std::uint64_t RetainedStateBytes(const State& state) noexcept {
    std::uint64_t bytes=sizeof(State);
    for(const auto* channels:{&state.channels,&state.baseline}){
        const auto amount=RetainedChannelsBytes(*channels);bytes=amount>UINT64_MAX-bytes?UINT64_MAX:bytes+amount;
    }return bytes;
}
}
struct AuthoredReplacement::Impl {
    MMDApp& app;Kind kind;int slot;Address address;std::uint64_t poolIdentity,generation=1;
    void* object;bool beforePresent=false,changed=false,published=false,forwardState=false;
    State before,after;std::uint64_t initialRevision=0,initialDigest=0;
    std::vector<Channel> effectiveBefore;
    std::vector<mdl::CameraKey> camera;
    std::vector<mdl::LightKey> light;
    std::vector<mdl::SelfShadowKey> shadow;
    std::vector<mdl::GravityKey> gravity;
    std::vector<mdl::AccessoryKey> accessory;
    NativeKeyView View() const noexcept {
        switch(kind){
        case Kind::Camera:return {&camera,camera.size(),[](const void* p,unsigned i)noexcept->const void*{const auto& v=*static_cast<const std::vector<mdl::CameraKey>*>(p);return i<v.size()?&v[i]:nullptr;}};
        case Kind::Light:return {&light,light.size(),[](const void* p,unsigned i)noexcept->const void*{const auto& v=*static_cast<const std::vector<mdl::LightKey>*>(p);return i<v.size()?&v[i]:nullptr;}};
        case Kind::Shadow:return {&shadow,shadow.size(),[](const void* p,unsigned i)noexcept->const void*{const auto& v=*static_cast<const std::vector<mdl::SelfShadowKey>*>(p);return i<v.size()?&v[i]:nullptr;}};
        case Kind::Gravity:return {&gravity,gravity.size(),[](const void* p,unsigned i)noexcept->const void*{const auto& v=*static_cast<const std::vector<mdl::GravityKey>*>(p);return i<v.size()?&v[i]:nullptr;}};
        default:return {&accessory,accessory.size(),[](const void* p,unsigned i)noexcept->const void*{const auto& v=*static_cast<const std::vector<mdl::AccessoryKey>*>(p);return i<v.size()?&v[i]:nullptr;}};
        }
    }
    Impl(MMDApp& a,Kind k,int s,const std::vector<Channel>& input):app(a),kind(k),slot(s),address(&a,k,s),
        poolIdentity(TrackPoolIdentity(a,k,s)),object(k==Kind::Accessory?Identity(a,k,s):nullptr){
        if(int(k)<0||int(k)>4||!poolIdentity||!Identity(a,k,s))throw std::invalid_argument("unowned curve replacement");
        const auto& fields=Fields(k);if(input.size()!=fields.size())throw std::invalid_argument("curve replacement channels");
        std::set<int> frames{0};std::string error;
        after.channels=input;
        for(std::size_t c=0;c<input.size();++c){const auto& channel=input[c];const auto& track=channel.track;
            if(channel.id!=fields[c].id||track.discrete!=fields[c].discrete||track.keys.empty()||track.keys.front().frame!=0||
               !ac::Validate(track,error))throw std::invalid_argument("curve replacement data");
            for(const auto& key:track.keys){if(key.frame>frame_limits::maximum||key.value<fields[c].minimum||key.value>fields[c].maximum||
                (fields[c].discrete&&std::floor(key.value)!=key.value))throw std::invalid_argument("curve replacement value");frames.insert(key.frame);}
            PrunePreserved(after.channels[c]);
        }
        std::vector<Channel> native;if(!NativeRead(a,k,s,native))throw std::invalid_argument("curve replacement native source");
        const auto found=states.find(address);
        if(found!=states.end()){
            if(!StateIdentityCurrent(found->second,a,k,s))throw std::invalid_argument("stale curve replacement author");
            beforePresent=true;before=found->second;initialRevision=found->second.revision;initialDigest=AuthoringDigest(found->second);
            State detached=before;Reconcile(detached,native);effectiveBefore=std::move(detached.channels);after.author=before.author;
        }else effectiveBefore=native;
        const auto& previous=effectiveBefore;
        changed=!Same(previous,after.channels);
        if(!changed)for(std::size_t c=0;c<previous.size()&&!changed;++c){
            changed=previous[c].sample.target_type()!=after.channels[c].sample.target_type()||
                previous[c].interpolationGroup!=after.channels[c].interpolationGroup||
                previous[c].controlGeometry!=after.channels[c].controlGeometry||previous[c].group!=after.channels[c].group||
                previous[c].collections.size()!=after.channels[c].collections.size();
            for(std::size_t i=0;i<previous[c].collections.size()&&!changed;++i)
                changed=previous[c].collections[i].id!=after.channels[c].collections[i].id||previous[c].collections[i].label!=after.channels[c].collections[i].label;
            for(std::size_t i=0;i<previous[c].track.keys.size()&&!changed;++i)
                changed=previous[c].track.keys[i].tangent!=after.channels[c].track.keys[i].tangent;
        }
        switch(k){
        case Kind::Camera:camera=MirroredRecords(a.CameraKeys(),after.channels,frames);MirrorCameraControls(camera.data(),camera.size(),after.channels);break;
        case Kind::Light:light=MirroredRecords(a.LightKeys(),after.channels,frames);break;
        case Kind::Shadow:shadow=MirroredRecords(a.ShadowKeys(),after.channels,frames);break;
        case Kind::Gravity:gravity=MirroredRecords(a.GravityKeys(),after.channels,frames);break;
        case Kind::Accessory:accessory=MirroredRecords(a.AccessoryKeys(s),after.channels,frames);break;
        }
        after.baseline=TargetChannels(View(),k);
        if(RetainedStateBytes(after)==UINT64_MAX||(beforePresent&&RetainedStateBytes(before)==UINT64_MAX))
            throw std::invalid_argument("unmeasurable curve replacement callback");
    }
    bool Structure() const noexcept {
        return TrackPoolIdentity(app,kind,slot)==poolIdentity&&
            (kind!=Kind::Accessory||Identity(app,kind,slot)==object);
    }
    bool Matches(bool forward) const noexcept {
        if(!Structure())return false;
        const auto found=states.find(address);const bool present=forward?beforePresent:true;
        if(!present)return found==states.end();
        if(found==states.end()||!StateIdentityCurrent(found->second,app,kind,slot))return false;
        if(!published)return found->second.revision==initialRevision&&AuthoringDigest(found->second)==initialDigest;
        const auto& expected=forward?before:after;
        return found->second.author==expected.author&&AuthoringDigest(found->second)==AuthoringDigest(expected);
    }
};
struct AuthoredReplacement::Target::Impl {
    std::shared_ptr<AuthoredReplacement::Impl> source;bool forward,published=false;std::uint64_t generation;
    const State* previous=nullptr;std::uint64_t revision=0,digest=0;void* allocation;void* identity;
    std::vector<Channel> native;decltype(states)::node_type target,retired;
    Impl(std::shared_ptr<AuthoredReplacement::Impl> s,const NativeKeyView& view,bool f):source(std::move(s)),forward(f),generation(source->generation),
        allocation(TrackPoolAllocation(source->app,source->kind,source->slot)),identity(Identity(source->app,source->kind,source->slot)){
        if(!source->Matches(f)||(!source->published&&!f)||source->generation==UINT64_MAX)throw std::invalid_argument("changed curve replacement source");
        const auto found=states.find(source->address);if(found!=states.end()){previous=&found->second;revision=previous->revision;digest=AuthoringDigest(*previous);}
        native=TargetChannels(view,source->kind);
        if(f||source->beforePresent){State state=f?source->after:source->before;
            if(f){state.baseline=native;state.fingerprint=0;}
            state.revision=NextAuthoringRevision();
            decltype(states) staged;staged.emplace(source->address,std::move(state));target=staged.extract(staged.begin());}
    }
    bool Current() const noexcept {
        if(published||source->generation!=generation||!source->Matches(forward)||
           TrackPoolAllocation(source->app,source->kind,source->slot)!=allocation||Identity(source->app,source->kind,source->slot)!=identity)return false;
        const auto found=states.find(source->address);const auto* current=found==states.end()?nullptr:&found->second;
        return current==previous&&(!current||(current->revision==revision&&AuthoringDigest(*current)==digest));
    }
};
AuthoredReplacement::AuthoredReplacement(MMDApp& app,Kind kind,int slot,const std::vector<Channel>& channels):impl_(std::make_shared<Impl>(app,kind,slot,channels)){}
AuthoredReplacement::~AuthoredReplacement()=default;
bool AuthoredReplacement::Changed() const noexcept{return impl_->changed;}
bool AuthoredReplacement::IsCurrent() const noexcept{return impl_->Matches(!impl_->forwardState);}
NativeKeyView AuthoredReplacement::TargetKeys() const noexcept{return impl_->View();}
const std::vector<Channel>& AuthoredReplacement::BeforeChannels()const noexcept{return impl_->effectiveBefore;}
AuthoredReplacement::Target::Target(std::unique_ptr<Impl> p):impl_(std::move(p)){}
AuthoredReplacement::Target::~Target()=default;
std::unique_ptr<AuthoredReplacement::Target> AuthoredReplacement::Prepare(const NativeKeyView& view,bool forward)const{
    return std::unique_ptr<Target>(new Target(std::make_unique<Target::Impl>(impl_,view,forward)));
}
bool AuthoredReplacement::Target::IsCurrent() const noexcept{return impl_->Current();}
bool AuthoredReplacement::Target::Authored() const noexcept{return !impl_->target.empty();}
bool AuthoredReplacement::Target::Sample(double frame,std::array<double,12>& out)const{
    if(!std::isfinite(frame))return false;const auto& fields=Fields(impl_->source->kind);
    // Undo republishes the original raw author node. Evaluation must still
    // follow current native authority without modifying that retained node.
    State effective;
    if(!impl_->forward&&!impl_->target.empty()){
        effective=impl_->target.mapped();Reconcile(effective,impl_->native);
    }
    const auto& channels=!effective.channels.empty()?effective.channels:impl_->target.empty()?impl_->native:impl_->target.mapped().channels;
    std::array<double,12> values{};if(channels.size()!=fields.size())return false;
    for(std::size_t c=0;c<channels.size();++c){const double value=SampleValue(channels[c],frame);if(!std::isfinite(value))return false;
        values[c]=impl_->target.empty()?value:std::clamp(value,fields[c].minimum,fields[c].maximum);}
    out=values;return true;
}
void AuthoredReplacement::Target::Publish(void* finalIdentity)noexcept{
    auto& p=*impl_;if(p.published||p.source->generation!=p.generation)std::terminate();
    if(!p.target.empty()){auto& state=p.target.mapped();state.identity=finalIdentity;
        state.poolAllocation=TrackPoolAllocation(p.source->app,p.source->kind,p.source->slot);
        state.poolIdentity=PoolIdentity(p.source->kind,state.poolAllocation);}
    p.retired=states.extract(p.source->address);if(!p.target.empty())states.insert(std::move(p.target));
    p.source->published=true;p.source->forwardState=p.forward;++p.source->generation;p.published=true;
}
std::uint64_t AuthoredReplacement::RetainedBytes()const noexcept{
    auto bytes=std::uint64_t(sizeof(*this)+sizeof(Impl)+4*sizeof(void*));
    const auto add=[&](std::uint64_t n,std::uint64_t stride){bytes=stride&&n>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+n*stride;};
    add(impl_->camera.capacity(),sizeof(mdl::CameraKey));add(impl_->light.capacity(),sizeof(mdl::LightKey));
    add(impl_->shadow.capacity(),sizeof(mdl::SelfShadowKey));add(impl_->gravity.capacity(),sizeof(mdl::GravityKey));add(impl_->accessory.capacity(),sizeof(mdl::AccessoryKey));
    add(1,RetainedStateBytes(impl_->before));add(1,RetainedStateBytes(impl_->after));
    add(1,RetainedChannelsBytes(impl_->effectiveBefore));return bytes;
}
struct TrackAuthoring::Impl {
    MMDApp& app;Kind kind;int slot;Address address;std::uint64_t poolIdentity;
    void* objectIdentity;bool authored=false,retimingCaptured=false;State seed;
    Impl(MMDApp& a,Kind k,int s):app(a),kind(k),slot(s),address(&a,k,s),poolIdentity(TrackPoolIdentity(a,k,s)),
        objectIdentity(k==Kind::Accessory?Identity(a,k,s):nullptr){
        if(!poolIdentity||!Identity(a,k,s))throw std::invalid_argument("unowned authoring track");
        const auto found=states.find(address);
        if(found!=states.end()&&StateIdentityCurrent(found->second,a,k,s)){seed=found->second;authored=true;}
    }
    bool Current() const noexcept {
        if(TrackPoolIdentity(app,kind,slot)!=poolIdentity||(kind==Kind::Accessory&&Identity(app,kind,slot)!=objectIdentity))return false;
        const auto found=states.find(address);
        // Identity-preserving pool growth may precede the next curve target's
        // publication. Its old allocation address does not end the captured
        // author lineage; a different author (or pool identity) still does.
        // An unauthored record accepts only absence, never an arbitrary stale
        // state whose allocation happens not to match the current pool.
        return authored?found!=states.end()&&StateIdentityCurrent(found->second,app,kind,slot)&&
            found->second.author==seed.author:found==states.end();
    }
};
struct TrackAuthoring::Target::Impl {
    std::shared_ptr<TrackAuthoring::Impl> source;
    const State* before=nullptr;std::uint64_t revision=0,digest=0;
    void* identity=nullptr;void* poolAllocation=nullptr;bool published=false;
    std::vector<Channel> native;
    decltype(states)::node_type target,retired;
    explicit Impl(std::shared_ptr<TrackAuthoring::Impl> s):source(std::move(s)){}
    bool Current() const noexcept {
        if(published||!source->Current()||Identity(source->app,source->kind,source->slot)!=identity||
            TrackPoolAllocation(source->app,source->kind,source->slot)!=poolAllocation)return false;
        const auto found=states.find(source->address);const auto* live=found==states.end()?nullptr:&found->second;
        return live==before&&(!live||(live->revision==revision&&AuthoringDigest(*live)==digest));
    }
};
TrackAuthoring::TrackAuthoring(MMDApp& app,Kind kind,int slot):impl_(std::make_shared<Impl>(app,kind,slot)){}
TrackAuthoring::~TrackAuthoring()=default;
bool TrackAuthoring::IsCurrent() const noexcept{return impl_->Current();}
bool TrackAuthoring::CapturedCurrent() const noexcept {
    if(impl_->retimingCaptured||!impl_->Current())return false;
    if(!impl_->authored)return true;
    const auto found=states.find(impl_->address);
    return found!=states.end()&&found->second.revision==impl_->seed.revision&&
        AuthoringDigest(found->second)==AuthoringDigest(impl_->seed);
}
TrackAuthoring::Target::Target(std::unique_ptr<Impl> p):impl_(std::move(p)){}
TrackAuthoring::Target::~Target()=default;
std::unique_ptr<TrackAuthoring::Target> TrackAuthoring::Prepare(const NativeKeyView& view) const {
    return Prepare(view,{},true);
}
bool TrackAuthoring::HasRegistrationChanges(const std::vector<int>& frames) const {
    ValidateRegistrationFrames(frames);
    if(!impl_->Current())throw std::invalid_argument("changed authoring track");
    return impl_->authored&&MissingRegistrationFrame(impl_->seed,frames);
}
std::unique_ptr<TrackAuthoring::Target> TrackAuthoring::Prepare(const NativeKeyView& view,const std::vector<int>& frames,bool forward) const {
    ValidateRegistrationFrames(frames);
    if(!impl_->Current())throw std::invalid_argument("changed authoring track");
    auto candidate=std::make_unique<Target::Impl>(impl_);candidate->identity=Identity(impl_->app,impl_->kind,impl_->slot);
    candidate->poolAllocation=TrackPoolAllocation(impl_->app,impl_->kind,impl_->slot);
    const auto found=states.find(impl_->address);
    if(found!=states.end()){candidate->before=&found->second;candidate->revision=found->second.revision;candidate->digest=AuthoringDigest(found->second);}
    candidate->native=TargetChannels(view,impl_->kind);
    if(forward)ValidateTargetRegistrationFrames(candidate->native,frames);
    if(impl_->authored){
        State target=impl_->seed;
        if(forward)ForceRegistrationFrames(target,candidate->native,frames);
        Reconcile(target,candidate->native);target.revision=NextAuthoringRevision();target.fingerprint=0;
        decltype(states) pending;pending.emplace(impl_->address,std::move(target));candidate->target=pending.extract(pending.begin());
    }
    return std::unique_ptr<Target>(new Target(std::move(candidate)));
}
namespace {
using RetimeMap=std::map<int,int>;
RetimeMap ValidateRetimeMap(const std::vector<std::pair<int,int>>& frames,const State* seed,
    const std::vector<Channel>& native) {
    RetimeMap result;std::set<int> targets;
    for(const auto& frame:frames){
        if(frame.first<0||frame.first>frame_limits::maximum||frame.second< -1||frame.second>frame_limits::maximum||
            !result.emplace(frame).second)throw std::invalid_argument("retime source frame");
    }
    int previous=-1;
    for(const auto& frame:result)if(frame.second>=0){
        if(frame.second<=previous||!targets.insert(frame.second).second)throw std::invalid_argument("retime target frame");
        previous=frame.second;
    }
    if(!targets.count(0))throw std::invalid_argument("retime frame zero");
    if(seed)for(const auto* channels:{&seed->channels,&seed->baseline})for(const auto& channel:*channels)
        for(const auto& key:channel.track.keys)if(!result.count(key.frame))throw std::invalid_argument("incomplete retime map");
    for(const auto& channel:native)for(const auto& key:channel.track.keys)
        if(!targets.count(key.frame))throw std::invalid_argument("unmapped native target frame");
    if(seed)for(std::size_t c=0;c<seed->baseline.size();++c)for(const auto& key:seed->baseline[c].track.keys){
        const int mapped=result.at(key.frame);if(mapped<0)continue;
        if(c>=native.size())throw std::invalid_argument("retime native channels");
        const auto& keys=native[c].track.keys;
        const auto found=std::lower_bound(keys.begin(),keys.end(),mapped,[](const ac::Key& k,int f){return k.frame<f;});
        if(found==keys.end()||found->frame!=mapped)throw std::invalid_argument("missing retained native frame");
    }
    return result;
}
void RemapIntervals(const Channel& source,Channel& target,const std::vector<int>& oldFrames,bool deleteKeys=false) {
    const auto& original=source.track.keys;const auto& keys=target.track.keys;
    target.nativeControls.clear();target.controlGeometry.clear();
    if(!source.nativeControls.empty())target.nativeControls.resize(keys.size());
    if(!source.controlGeometry.empty())target.controlGeometry.assign(keys.size(),{-1,0,-1,0});
    if(!target.nativeControls.empty()&&!original.empty()&&!oldFrames.empty()&&oldFrames.front()==original.front().frame)
        target.nativeControls.front()=source.nativeControls.front();
    // Root geometry has no incoming interval, but remains part of the exact
    // authored snapshot. Deleting another lane's union key does not own that
    // metadata. Keep it when the original root survives; ordinary retiming's
    // existing placeholder behavior is unchanged.
    if(deleteKeys&&!target.controlGeometry.empty()&&!original.empty()&&!oldFrames.empty()&&oldFrames.front()==original.front().frame)
        target.controlGeometry.front()=source.controlGeometry.front();
    for(std::size_t i=1;i<keys.size();++i){
        const auto found=std::lower_bound(original.begin(),original.end(),oldFrames[i],[](const ac::Key& k,int f){return k.frame<f;});
        const auto n=std::size_t(found-original.begin());
        if(found==original.end()||!n||found->frame!=oldFrames[i]||original[n-1].frame!=oldFrames[i-1])continue;
        if(n<source.nativeControls.size())target.nativeControls[i]=source.nativeControls[n];
        if(n<source.controlGeometry.size()){
            auto geometry=source.controlGeometry[n];
            const double ratio=double(oldFrames[i]-oldFrames[i-1])/double(keys[i].frame-keys[i-1].frame);
            geometry[1]*=ratio;geometry[3]*=ratio;target.controlGeometry[i]=geometry;
        }
    }
    RemapPreserved(source,target,oldFrames,true,deleteKeys);
}
void ClipNativeCameraTail(const Channel& source,int rootSource,Channel& target,const std::vector<int>& oldFrames) {
    if(target.track.keys.size()>1&&std::any_of(target.preservedCameraIntervals.begin(),target.preservedCameraIntervals.end(),
        [&](const auto& value){return value.first==target.track.keys[0].frame&&value.last==target.track.keys[1].frame;}))return;
    const auto sampler=source.sample.target<ac::Sample(*)(const Channel&,double)>();
    if(!sampler||*sampler!=CameraSample||target.track.keys.size()<2||oldFrames.front()!=rootSource)return;
    const auto& keys=source.track.keys;
    const auto end=std::upper_bound(keys.begin(),keys.end(),rootSource,[](int f,const ac::Key& k){return f<k.frame;});
    const auto n=std::size_t(end-keys.begin());
    if(end==keys.end()||!n||oldFrames[1]!=end->frame||!NativeGeometry(source,n))return;
    struct Point{double x,y;};
    const auto lerp=[](Point a,Point b,double t){return Point{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};};
    const auto& raw=source.nativeControls[n];
    const Point p0{0,0},p1{raw[0]/127.0,raw[1]/127.0},p2{raw[2]/127.0,raw[3]/127.0},p3{1,1};
    const double cut=double(rootSource-keys[n-1].frame)/double(end->frame-keys[n-1].frame);
    double low=0,high=1;
    for(int i=0;i<64;++i){const double t=(low+high)*.5;
        const auto q0=lerp(p0,p1,t),q1=lerp(p1,p2,t),q2=lerp(p2,p3,t);
        const auto point=lerp(lerp(q0,q1,t),lerp(q1,q2,t),t);
        if(point.x<cut)low=t;else high=t;
    }
    const double t=(low+high)*.5;
    const auto q0=lerp(p0,p1,t),q1=lerp(p1,p2,t),q2=lerp(p2,p3,t);
    const auto right=lerp(q1,q2,t),middle=lerp(lerp(q0,q1,t),right,t);
    auto& a=target.track.keys[0];auto& b=target.track.keys[1];
    // The new root keeps the player's sampled value. Its 12-step float
    // inverse differs slightly from the analytic subdivision parameter.
    // Keep the absolute tail control values while anchoring that sampled root.
    const double delta=end->value-keys[n-1].value;
    const double origin=delta?(a.value-keys[n-1].value)/delta:middle.y;
    if(1-origin<=1e-12)return;
    const auto control=[](double value){return std::clamp(value*127.0,0.0,127.0);};
    const std::array<double,4> controls={control((right.x-cut)/(1-cut)),control((right.y-origin)/(1-origin)),
        control((q2.x-cut)/(1-cut)),control((q2.y-origin)/(1-origin))};
    const double x1=std::clamp(controls[0]/127.0,0.0002,0.9996);
    const double x2=std::clamp(controls[2]/127.0,x1+0.0002,0.9998);
    const double rate=(b.value-a.value)/double(b.frame-a.frame);
    a.outWeight=x1;a.outSlope=controls[1]/127.0*rate/x1;
    b.inWeight=1-x2;b.inSlope=(1-controls[3]/127.0)*rate/(1-x2);b.interpolation=ac::Interpolation::Bezier;
    target.nativeControls[1]=controls;target.controlGeometry[1]={a.outWeight,a.outSlope,b.inWeight,b.inSlope};
}
Channel RetimedChannel(const Channel& source,const RetimeMap& frames,int rootSource,bool deleteKeys) {
    Channel target=source;auto working=source.track;
    const auto root=std::lower_bound(working.keys.begin(),working.keys.end(),rootSource,[](const ac::Key& k,int f){return k.frame<f;});
    const bool insertedRoot=root==working.keys.end()||root->frame!=rootSource;
    if(insertedRoot){
        const double value=SampleValue(source,rootSource);std::size_t at=0;std::string error;
        if(!std::isfinite(value)||ac::InsertPreservingShape(working,rootSource,at,error)==ac::InsertResult::Rejected)
            throw std::invalid_argument("retime source anchor");
        // A native camera callback is authoritative at the new root as well.
        // The clipped interval no longer carries an intact native control tuple.
        working.keys[at].value=value;
    }
    target.track.keys.clear();std::vector<int> oldFrames;
    for(std::size_t i=0;i<working.keys.size();++i){
        const auto& original=working.keys[i];const auto moved=frames.find(original.frame);
        if(moved==frames.end()||moved->second<0)continue;
        auto key=original;key.frame=moved->second;
        if(i){const auto previous=frames.find(working.keys[i-1].frame);
            if(previous!=frames.end()&&previous->second>=0)key.inSlope*=double(original.frame-working.keys[i-1].frame)/(key.frame-previous->second);}
        if(i+1<working.keys.size()){const auto next=frames.find(working.keys[i+1].frame);
            if(next!=frames.end()&&next->second>=0)key.outSlope*=double(working.keys[i+1].frame-original.frame)/(next->second-key.frame);}
        target.track.keys.push_back(key);oldFrames.push_back(original.frame);
    }
    if(target.track.keys.empty()||target.track.keys.front().frame!=0)throw std::invalid_argument("retime authored frame zero");
    for(std::size_t i=1;i<target.track.keys.size();++i){auto& a=target.track.keys[i-1];auto& b=target.track.keys[i];
        // Joined intervals may inherit opposing handles from different old
        // intervals. Preserve slopes while fitting their normalized time span.
        const double sum=a.outWeight+b.inWeight;if(sum>0.9999){
            a.outWeight=std::clamp(a.outWeight*0.9998/sum,0.0001,0.9997);b.inWeight=0.9998-a.outWeight;}
    }
    RemapIntervals(source,target,oldFrames,deleteKeys);
    if(insertedRoot)ClipNativeCameraTail(source,rootSource,target,oldFrames);
    std::string error;if(!ac::Validate(target.track,error))throw std::invalid_argument("invalid retimed authored track");
    return target;
}
void ReconcileRetimed(State& target,const State& source,const std::vector<Channel>& native,const RetimeMap& frames,bool deleteKeys) {
    if(source.channels.size()!=native.size()||source.baseline.size()!=native.size())throw std::invalid_argument("retime channels");
    std::map<int,int> sources;for(const auto& frame:frames)if(frame.second>=0)sources.emplace(frame.second,frame.first);
    const int rootSource=sources.at(0);
    for(std::size_t c=0;c<source.channels.size();++c){
        auto channel=RetimedChannel(source.channels[c],frames,rootSource,deleteKeys);
        const auto& baseline=source.baseline[c];
        for(std::size_t i=0;i<native[c].track.keys.size();++i){
            const auto& fresh=native[c].track.keys[i];auto oldFrame=sources.at(fresh.frame);
            // An authored-only frame can win the new root while the native
            // frame-one row is absent. Native keeps its old root in that case;
            // compare that unchanged proxy to its own baseline, not the new
            // high-precision authored root.
            if(!fresh.frame){const auto winner=std::lower_bound(baseline.track.keys.begin(),baseline.track.keys.end(),oldFrame,
                    [](const ac::Key& k,int f){return k.frame<f;});
                if(winner==baseline.track.keys.end()||winner->frame!=oldFrame)oldFrame=0;}
            const auto old=std::lower_bound(baseline.track.keys.begin(),baseline.track.keys.end(),oldFrame,[](const ac::Key& k,int f){return k.frame<f;});
            if(old==baseline.track.keys.end()||old->frame!=oldFrame)throw std::invalid_argument("missing retime native source");
            const auto n=std::size_t(old-baseline.track.keys.begin());
            if(i&&!native[c].nativeControls.empty()&&(n>=baseline.nativeControls.size()||native[c].nativeControls[i]!=baseline.nativeControls[n]))
                throw std::invalid_argument("retime changes native controls");
            if(fresh.value==old->value)continue;
            // A root collision can copy only part of a native record. Compare
            // each field to the actual winning source, never to the old root.
            auto at=std::lower_bound(channel.track.keys.begin(),channel.track.keys.end(),fresh.frame,[](const ac::Key& k,int f){return k.frame<f;});
            if(at==channel.track.keys.end()||at->frame!=fresh.frame){
                const Channel before=channel;std::size_t index=0;std::string error;
                if(ac::InsertPreservingShape(channel.track,fresh.frame,index,error)==ac::InsertResult::Rejected)
                    throw std::invalid_argument("retime native value insertion");
                std::vector<int> oldFrames;for(const auto& key:channel.track.keys)oldFrames.push_back(key.frame);
                RemapIntervals(before,channel,oldFrames,deleteKeys);at=channel.track.keys.begin()+index;
            }
            at->value=fresh.value;
        }
        // A deleted frame zero may be replaced by a different native row.
        // Its unused control bytes belong to that actual winning row, rather
        // than the discarded root or the interval remapper's empty root slot.
        if(!channel.nativeControls.empty()&&!native[c].nativeControls.empty()&&
            !channel.track.keys.empty()&&!native[c].track.keys.empty()&&
            channel.track.keys.front().frame==native[c].track.keys.front().frame)
            channel.nativeControls.front()=native[c].nativeControls.front();
        // Deletion is an intentional topology/value edit. A joined interval
        // loses its exact source above; changed native endpoints release only
        // their own slice, leaving every still-current slice untouched.
        if(deleteKeys)PrunePreserved(channel);
        auto automatic=channel.track;ac::UpdateAutomatic(automatic);
        for(std::size_t i=0;i<channel.track.keys.size();++i){auto& key=channel.track.keys[i];
            if(key.tangent!=ac::Tangent::Automatic)continue;
            const auto& computed=automatic.keys[i];
            const double tolerance=1e-9*std::max({1.0,std::abs(computed.inSlope),std::abs(computed.outSlope)});
            if((i&&std::abs(key.inSlope-computed.inSlope)>tolerance)||
                (i+1<channel.track.keys.size()&&std::abs(key.outSlope-computed.outSlope)>tolerance))key.tangent=ac::Tangent::Free;
        }
        std::string error;if(!ac::Validate(channel.track,error))throw std::invalid_argument("retime native values");
        target.channels[c]=std::move(channel);
    }
    target.baseline=native;
}
}
void TrackAuthoring::RetimingFrames(std::vector<int>& out) const {
    if(!impl_->Current())throw std::invalid_argument("changed authoring track");
    std::vector<int> frames;State effective;
    const State* source=&impl_->seed;
    if(impl_->authored&&!impl_->retimingCaptured){
        effective=impl_->seed;std::vector<Channel> native;
        if(!NativeRead(impl_->app,impl_->kind,impl_->slot,native))throw std::invalid_argument("retime source native track");
        Reconcile(effective,native);source=&effective;
    }
    if(impl_->authored)for(const auto* channels:{&source->channels,&source->baseline})for(const auto& channel:*channels)
        for(const auto& key:channel.track.keys)frames.push_back(key.frame);
    std::sort(frames.begin(),frames.end());frames.erase(std::unique(frames.begin(),frames.end()),frames.end());
    if(impl_->authored&&!impl_->retimingCaptured)std::swap(impl_->seed,effective);
    impl_->retimingCaptured=true;out.swap(frames);
}
std::unique_ptr<TrackAuthoring::Target> TrackAuthoring::PrepareRetimed(const NativeKeyView& view,
    const std::vector<std::pair<int,int>>& frameMap,bool forward,std::uint32_t resetRootChannels,bool deleteKeys) const {
    if(!impl_->retimingCaptured){std::vector<int> frames;RetimingFrames(frames);}
    if(!impl_->Current())throw std::invalid_argument("changed authoring track");
    auto candidate=std::make_unique<Target::Impl>(impl_);candidate->identity=Identity(impl_->app,impl_->kind,impl_->slot);
    candidate->poolAllocation=TrackPoolAllocation(impl_->app,impl_->kind,impl_->slot);
    const auto found=states.find(impl_->address);
    if(found!=states.end()){candidate->before=&found->second;candidate->revision=found->second.revision;candidate->digest=AuthoringDigest(found->second);}
    candidate->native=TargetChannels(view,impl_->kind);
    if(impl_->authored){
        State target=impl_->seed;
        if(forward){const auto frames=ValidateRetimeMap(frameMap,&impl_->seed,candidate->native);
            ReconcileRetimed(target,impl_->seed,candidate->native,frames,deleteKeys);
            for(std::size_t c=0;c<target.channels.size()&&c<32;++c){
                if(!(resetRootChannels&(std::uint32_t(1)<<c)))continue;
                auto& channel=target.channels[c];const auto& native=candidate->native[c].track.keys;
                if(channel.track.keys.empty()||native.empty()||channel.track.keys.front().frame||native.front().frame)
                    throw std::invalid_argument("reset authored frame zero");
                auto& root=channel.track.keys.front();
                if(root.value==native.front().value)continue;
                root.value=native.front().value;
                auto& intervals=channel.preservedCameraIntervals;
                intervals.erase(std::remove_if(intervals.begin(),intervals.end(),[](const auto& value){return value.first==0;}),intervals.end());
                // Free handles stay owned by the author. Only automatic
                // tangents whose neighbourhood includes this root change.
                auto automatic=channel.track;ac::UpdateAutomatic(automatic);
                for(std::size_t i=0;i<(std::min)(std::size_t(2),channel.track.keys.size());++i)
                    if(channel.track.keys[i].tangent==ac::Tangent::Automatic){
                        channel.track.keys[i].inSlope=automatic.keys[i].inSlope;
                        channel.track.keys[i].outSlope=automatic.keys[i].outSlope;
                    }
                std::string error;if(!ac::Validate(channel.track,error))throw std::invalid_argument("invalid reset root track");
            }
        }
        target.revision=NextAuthoringRevision();target.fingerprint=0;
        decltype(states) pending;pending.emplace(impl_->address,std::move(target));candidate->target=pending.extract(pending.begin());
    }else if(forward)(void)ValidateRetimeMap(frameMap,nullptr,candidate->native);
    return std::unique_ptr<Target>(new Target(std::move(candidate)));
}
bool TrackAuthoring::Target::IsCurrent() const noexcept{return impl_->Current();}
bool TrackAuthoring::Target::Authored() const noexcept{return impl_->source->authored;}
bool TrackAuthoring::Target::Sample(double frame,std::array<double,12>& out) const {
    if(!std::isfinite(frame))return false;
    const auto& fields=Fields(impl_->source->kind);
    const auto& channels=impl_->target.empty()?impl_->native:impl_->target.mapped().channels;
    if(channels.size()!=fields.size())return false;
    std::array<double,12> result{};
    for(std::size_t c=0;c<channels.size();++c){
        const auto& keys=channels[c].track.keys;
        const auto exact=std::lower_bound(keys.begin(),keys.end(),frame,[](const ac::Key& key,double f){return key.frame<f;});
        const double value=exact!=keys.end()&&exact->frame==frame?exact->value:SampleValue(channels[c],frame);
        if(!std::isfinite(value))return false;
        result[c]=impl_->target.empty()?value:std::clamp(value,fields[c].minimum,fields[c].maximum);
    }
    out=result;return true;
}
void TrackAuthoring::Target::Publish(void* identity) noexcept {
    auto& p=*impl_;if(p.published)std::terminate();
    if(!p.target.empty()){
        p.target.mapped().identity=identity;
        p.target.mapped().poolAllocation=TrackPoolAllocation(p.source->app,p.source->kind,p.source->slot);
        p.target.mapped().poolIdentity=PoolIdentity(p.source->kind,p.target.mapped().poolAllocation);
        p.retired=states.extract(p.source->address);
        states.insert(std::move(p.target));
    }
    p.published=true;
}
std::uint64_t TrackAuthoring::RetainedBytes() const noexcept {
    const auto bytes=impl_->authored?RetainedStateBytes(impl_->seed):0;
    const std::uint64_t overhead=sizeof(*this)+sizeof(Impl)+4*sizeof(void*);
    return bytes>UINT64_MAX-overhead?UINT64_MAX:bytes+overhead;
}
#endif
}
