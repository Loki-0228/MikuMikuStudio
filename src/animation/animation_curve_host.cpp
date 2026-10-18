#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#define NOMINMAX
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/animation_curve_motion.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/runtime_log.hpp"
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

namespace mikudancestudio::curve_host {
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
        if(i>=mdl::KeyPoolCapacity(pool) || pool[i].frame>int32_t(mikudancestudio::frame_limits::maximum) ||
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
bool NativeGeometry(const Channel& channel,std::size_t end){
    if(end==0||end>=channel.track.keys.size()||end>=channel.nativeControls.size()||end>=channel.controlGeometry.size())return false;
    const auto&a=channel.track.keys[end-1];const auto&b=channel.track.keys[end];
    return b.interpolation==ac::Interpolation::Bezier&&
        channel.controlGeometry[end]==std::array<double,4>{a.outWeight,a.outSlope,b.inWeight,b.inSlope};
}
ac::Sample CameraSample(const Channel& channel,double frame){
    return curve_motion::SampleCameraChannel(channel,frame);
}
double SampleValue(const Channel&channel,double frame){
    return curve_editor::EvaluateChannel(channel,frame).value;
}
bool CameraIntervalControls(const Channel&channel,int start,int end,std::array<double,4>&out){
    const auto&keys=channel.track.keys;
    const auto at=std::lower_bound(keys.begin(),keys.end(),end,[](const auto&key,int f){return key.frame<f;});
    const auto n=size_t(at-keys.begin());
    if(at==keys.end()||n==0||at->frame!=end||keys[n-1].frame!=start)return false;
    if(NativeGeometry(channel,n)){
        out=channel.nativeControls[n];
        // Subdivision of crossed-X controls can need handles outside 0..127.
        // Those stay in MMS authoring; never wrap them into native VMD bytes.
        return std::all_of(out.begin(),out.end(),[](double v){return std::isfinite(v)&&v>=0&&v<=127;});
    }
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
            out[c].nativeOneFrameHolds.resize(out[c].track.keys.size(),true);
            out[c].sample=CameraSample;
            if(c>=3&&c<6)out[c].interpolationGroup="camera/rotation";
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
    for(size_t c=0;c<a.size();++c){if(a[c].track.keys.size()!=b[c].track.keys.size()||a[c].nativeControls!=b[c].nativeControls)return false;
        for(size_t i=0;i<a[c].track.keys.size();++i)if(!Same(a[c].track.keys[i],b[c].track.keys[i]))return false;}
    return true;
}
struct State {std::vector<Channel> channels, baseline;void* identity=nullptr;uint64_t fingerprint=0;std::set<int> registrations;};
using Address=std::tuple<MMDApp*,Kind,int>;
std::map<Address,State> states;
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
// Detect edits made by the existing registration/delete/paste/undo commands.
// Only changed native keys replace authored keys; unrelated tangents survive.
void Reconcile(State&s,const std::vector<Channel>&native){
    // Registration is an explicit key insertion even if a shared native row
    // already has identical bytes. Batch these events until the next read.
    for(size_t c=0;!s.registrations.empty()&&c<s.channels.size();++c){
        const auto& keys=s.channels[c].track.keys;
        std::set<int> missing;
        for(int frame:s.registrations){
            const auto at=std::lower_bound(keys.begin(),keys.end(),frame,[](const auto& key,int f){return key.frame<f;});
            if(at==keys.end()||at->frame!=frame)missing.insert(frame);
        }
        auto& before=s.baseline[c].track.keys;
        before.erase(std::remove_if(before.begin(),before.end(),[&](const auto& key){return missing.count(key.frame)!=0;}),before.end());
    }
    s.registrations.clear();
    if(Same(s.baseline,native))return;
    for(size_t c=0;c<s.channels.size();++c){
        auto& keys=s.channels[c].track.keys;const auto& old=s.baseline[c].track.keys;
        const auto& fresh=native[c].track.keys;
        if(old.size()==fresh.size()&&s.baseline[c].nativeControls==native[c].nativeControls&&
           std::equal(old.begin(),old.end(),fresh.begin(),[](const auto&a,const auto&b){return Same(a,b);}))continue;
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
        channel.nativeOneFrameHolds.assign(keys.size(),true);
        if(native[c].nativeControls.empty()){
            channel.nativeControls.clear();channel.controlGeometry.clear();
        }else for(size_t end=1;end<keys.size();++end){
            const auto at=std::lower_bound(fresh.begin(),fresh.end(),keys[end].frame,[](const auto&key,int f){return key.frame<f;});
            const auto n=size_t(at-fresh.begin());
            if(at==fresh.end()||n==0||at->frame!=keys[end].frame||fresh[n-1].frame!=keys[end-1].frame)continue;
            channel.nativeControls[end]=native[c].nativeControls[n];
            channel.controlGeometry[end]=native[c].controlGeometry[n];
            channel.nativeOneFrameHolds[end]=native[c].nativeOneFrameHolds[n];
        }
        channel.sample=native[c].sample;
        channel.interpolationGroup=native[c].interpolationGroup;
    }s.baseline=native;
}
State* Current(MMDApp&app,Kind k,int slot){
    auto it=states.find({&app,k,slot});if(it==states.end())return nullptr;
    if(it->second.identity!=Identity(app,k,slot)){states.erase(it);return nullptr;}
    const uint64_t fingerprint=Fingerprint(app,k,slot);
    if(fingerprint&&it->second.fingerprint==fingerprint&&it->second.registrations.empty())return &it->second;
    // The authored extension is the authority for custom curves. A legacy
    // proxy that cannot be decoded must not make valid authoring disappear.
    // Keep the old fingerprint so reconciliation retries after native repair.
    std::vector<Channel> native;if(!NativeRead(app,k,slot,native))return &it->second;
    Reconcile(it->second,native);it->second.fingerprint=fingerprint;return &it->second;
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
template<class K> void Mirror(K*& pool,const K* previous,const std::vector<Channel>&channels,const std::set<int>&frames){
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
    if(!mdl::KeyPoolEnsure(pool,records.size()))throw std::bad_alloc();
    std::memset(pool,0,sizeof(K)*mdl::KeyPoolCapacity(pool));
    std::copy(records.begin(),records.end(),pool);
}
}

bool InsertCameraKey(std::vector<Channel>&channels,std::size_t active,int frame,std::wstring&error){
    if(active>=channels.size()||frame<0||frame>frame_limits::maximum)return false;
    auto next=channels;
    const std::string group=next[active].interpolationGroup;
    for(std::size_t c=0;c<next.size();++c){
        if(c!=active&&(group.empty()||next[c].interpolationGroup!=group))continue;
        auto&channel=next[c];auto&keys=channel.track.keys;
        auto at=std::lower_bound(keys.begin(),keys.end(),frame,[](const auto&k,int f){return k.frame<f;});
        const auto index=std::size_t(at-keys.begin());
        if(at!=keys.end()&&at->frame==frame)continue;
        if(keys.empty())return false;
        if(index>0&&index<keys.size()&&NativeGeometry(channel,index)){
            const auto controls=channel.nativeControls[index];
            const double u=double(frame-keys[index-1].frame)/(keys[index].frame-keys[index-1].frame);
            struct Point{double x,y;};
            const auto lerp=[](Point a,Point b,double t){return Point{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};};
            const Point p0{0,0},p1{controls[0]/127,controls[1]/127},p2{controls[2]/127,controls[3]/127},p3{1,1};
            double low=0,high=1;
            for(int step=0;step<64;++step){
                const double t=(low+high)/2;
                if(lerp(lerp(lerp(p0,p1,t),lerp(p1,p2,t),t),lerp(lerp(p1,p2,t),lerp(p2,p3,t),t),t).x<u)low=t;else high=t;
            }
            const double t=(low+high)/2;
            const auto q0=lerp(p0,p1,t),q1=lerp(p1,p2,t),q2=lerp(p2,p3,t);
            const auto left=lerp(q0,q1,t),right=lerp(q1,q2,t),middle=lerp(left,right,t);
            const std::array<double,4> first={q0.x/u*127,q0.y/middle.y*127,left.x/u*127,left.y/middle.y*127};
            const std::array<double,4> second={(right.x-u)/(1-u)*127,(right.y-middle.y)/(1-middle.y)*127,
                (q2.x-u)/(1-u)*127,(q2.y-middle.y)/(1-middle.y)*127};
            ac::Key key;key.frame=frame;key.value=keys[index-1].value+(keys[index].value-keys[index-1].value)*middle.y;
            key.tangent=ac::Tangent::Free;key.interpolation=ac::Interpolation::Bezier;
            keys.insert(keys.begin()+index,key);
            channel.nativeControls.resize(keys.size()-1);channel.controlGeometry.resize(keys.size()-1);
            channel.nativeControls.insert(channel.nativeControls.begin()+index,std::array<double,4>{});
            channel.controlGeometry.insert(channel.controlGeometry.begin()+index,std::array<double,4>{});
            channel.nativeOneFrameHolds.resize(keys.size()-1,true);
            channel.nativeOneFrameHolds.insert(channel.nativeOneFrameHolds.begin()+index,false);
            channel.nativeOneFrameHolds[index+1]=false;
            const auto set=[&](std::size_t end,const std::array<double,4>&ctrl){
                auto&a=keys[end-1];auto&b=keys[end];const double rate=(b.value-a.value)/double(b.frame-a.frame);
                const double x1=std::clamp(ctrl[0]/127,0.0002,0.9996),x2=std::clamp(ctrl[2]/127,x1+0.0002,0.9998);
                a.outWeight=x1;b.inWeight=1-x2;a.outSlope=ctrl[1]/127*rate/x1;b.inSlope=(1-ctrl[3]/127)*rate/(1-x2);
                a.tangent=b.tangent=ac::Tangent::Free;b.interpolation=ac::Interpolation::Bezier;
                channel.nativeControls[end]=ctrl;channel.controlGeometry[end]={a.outWeight,a.outSlope,b.inWeight,b.inSlope};
            };
            set(index,first);set(index+1,second);
        }else{
            std::string detail;std::size_t inserted=0;
            if(ac::InsertPreservingShape(channel.track,frame,inserted,detail)==ac::InsertResult::Rejected){error.assign(detail.begin(),detail.end());return false;}
            if(!channel.nativeControls.empty()){
                channel.nativeControls.resize(keys.size()-1);channel.controlGeometry.resize(keys.size()-1);
                channel.nativeControls.insert(channel.nativeControls.begin()+index,std::array<double,4>{});
                channel.controlGeometry.insert(channel.controlGeometry.begin()+index,std::array<double,4>{});
                channel.nativeOneFrameHolds.resize(keys.size()-1,true);
                channel.nativeOneFrameHolds.insert(channel.nativeOneFrameHolds.begin()+index,false);
            }
        }
    }
    channels=std::move(next);return true;
}

bool Read(MMDApp& app,Kind kind,int slot,std::vector<Channel>&out,std::wstring&error){
    auto* state=Current(app,kind,slot);
    if(state)out=state->channels;
    else if(!NativeRead(app,kind,slot,out)){
        error=Error(app,L"トラックが無効、または既存の補間を表示できません。",L"Track unavailable or existing interpolation cannot be represented.",L"轨道不可用，或现有插值无法表示。");return false;}
    const auto&f=Fields(kind);
    for(size_t c=0;c<out.size();++c){out[c].label=Error(app,f[c].ja,f[c].en,f[c].zh);
        out[c].minimum=f[c].minimum;out[c].maximum=f[c].maximum;
        out[c].base=out[c].track.keys.empty()?0:out[c].track.keys.front().value;
        out[c].unit=(out[c].id=="rx"||out[c].id=="ry"||out[c].id=="rz"||out[c].id=="fov")?L"°":L"";
    }return true;
}
bool Write(MMDApp&app,Kind kind,int slot,const std::vector<Channel>&input,std::wstring&error){
    if(!Identity(app,kind,slot))return false;
    const auto&f=Fields(kind);
    if(input.size()!=f.size())return false;
    std::set<int> frames{0};std::string detail;
    for(size_t c=0;c<input.size();++c){
        const auto&t=input[c].track;
        if(input[c].id!=f[c].id||t.discrete!=f[c].discrete||!ac::Validate(t,detail)||t.keys.empty()||t.keys.front().frame!=0){
            error=Error(app,L"各トラックの 0 フレームキーを保持してください。",L"Keep a valid frame-zero key in every track.",L"每个轨道须保留有效的第 0 帧关键帧。");return false;}
        for(const auto&k:t.keys){
            if(k.frame>mikudancestudio::frame_limits::maximum||k.value<f[c].minimum||k.value>f[c].maximum||(f[c].discrete&&std::floor(k.value)!=k.value)){
                error=Error(app,L"値またはフレームが範囲外です。",L"Value or frame is outside the allowed range.",L"数值或帧号超出允许范围。");return false;}
            frames.insert(k.frame);
        }
    }
    const auto stateKey=std::make_tuple(&app,kind,slot);
    auto stateSlot=states.try_emplace(stateKey);
    PoolTransaction transaction;
    try {
    State next;next.identity=Identity(app,kind,slot);next.channels=input;
    switch(kind){
    case Kind::Camera:
        transaction.Begin(app.CameraKeys());
        Mirror(app.CameraKeys(),static_cast<const mdl::CameraKey*>(transaction.before),input,frames);transaction.Updated(app.CameraKeys());
        // The standard PMM body contains a linear proxy; full curves follow it.
        for(size_t i=0;i<frames.size();++i)for(int c=0;c<6;++c){
            app.CameraKeys()[i].interpolation[0][c]=20;app.CameraKeys()[i].interpolation[1][c]=20;
            app.CameraKeys()[i].interpolation[2][c]=107;app.CameraKeys()[i].interpolation[3][c]=107;
        }
        // Preserve native easing whenever this interval is representable in
        // PMM, including endpoint/crossed-X controls retained during import.
        for(size_t i=1;i<frames.size();++i){
            auto&key=app.CameraKeys()[i];
            const int start=int(app.CameraKeys()[i-1].frame),end=int(key.frame);
            const size_t channels[6]={0,1,2,3,6,7};
            for(size_t lane=0;lane<6;++lane){
                std::array<double,4> controls{};
                if(!CameraIntervalControls(input[channels[lane]],start,end,controls))continue;
                if(lane==3){
                    std::array<double,4> y{},z{};
                    if(!CameraIntervalControls(input[4],start,end,y)||!CameraIntervalControls(input[5],start,end,z)||controls!=y||controls!=z)continue;
                }
                for(size_t component=0;component<4;++component)key.interpolation[component][lane]=uint8_t(std::lround(controls[component]));
            }
        }
        break;
    case Kind::Light:transaction.Begin(app.LightKeys());Mirror(app.LightKeys(),static_cast<const mdl::LightKey*>(transaction.before),input,frames);transaction.Updated(app.LightKeys());break;
    case Kind::Shadow:transaction.Begin(app.ShadowKeys());Mirror(app.ShadowKeys(),static_cast<const mdl::SelfShadowKey*>(transaction.before),input,frames);transaction.Updated(app.ShadowKeys());break;
    case Kind::Gravity:transaction.Begin(app.GravityKeys());Mirror(app.GravityKeys(),static_cast<const mdl::GravityKey*>(transaction.before),input,frames);transaction.Updated(app.GravityKeys());break;
    case Kind::Accessory:transaction.Begin(app.AccessoryKeys(slot));Mirror(app.AccessoryKeys(slot),static_cast<const mdl::AccessoryKey*>(transaction.before),input,frames);transaction.Updated(app.AccessoryKeys(slot));break;
    }
    next.identity=kind==Kind::Accessory ? app.AccessorySlot(slot) : transaction.FinalIdentity();
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
void ReconcileRegisteredFrame(MMDApp& app,Kind kind,int slot,int frame){
    if(frame<0)frame=app.CurrentFrame();
    const auto found=states.find({&app,kind,slot});
    if(found!=states.end())found->second.registrations.insert(frame);
}
void Apply(MMDApp*app,Kind kind,double frame,int slot){
    if(!app||!std::isfinite(frame)||states.empty())return;
    State*s=Current(*app,kind,slot);if(!s)return;
    double v[12]{};const auto&fields=Fields(kind);
    for(size_t c=0;c<s->channels.size();++c){v[c]=SampleValue(s->channels[c],frame);
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
        a.scale=float(v[6]);a.opacity=float(v[7]);a.visible=uint8_t(v[8]);a.shadowEnabled=uint8_t(v[9]);a.parentModel=int(v[10]);a.parentBone=int(v[11]);break;}
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
    std::vector<uint8_t> native(4,0);std::uint32_t intervals=0;
    const auto append=[&](std::uint64_t value,int bytes){for(int n=0;n<bytes;++n)native.push_back(std::uint8_t(value>>(n*8)));};
    for(auto&entry:states){if(std::get<0>(entry.first)!=&app)continue;
        Kind k=std::get<1>(entry.first);int slot=std::get<2>(entry.first);
        if(entry.second.identity!=Identity(app,k,slot))continue;
        const auto* current=Current(app,k,slot);
        if(!current)continue; // Identity was checked above; no deleted object is saved.
        for(const auto&c:current->channels)tracks.push_back({std::to_string(int(k))+"/"+std::to_string(slot)+"/"+c.id,c.track});
        if(k==Kind::Camera)for(std::size_t c=0;c<current->channels.size();++c){
            const auto&channel=current->channels[c];
            for(std::size_t end=1;end<channel.track.keys.size();++end)if(NativeGeometry(channel,end)){
                if(intervals==UINT32_MAX)throw std::runtime_error("Too many camera timing intervals");
                ++intervals;append(c,4);append(channel.track.keys[end-1].frame,4);append(channel.track.keys[end].frame,4);
                for(double control:channel.nativeControls[end]){std::uint64_t bits;std::memcpy(&bits,&control,8);append(bits,8);}
                append(end>=channel.nativeOneFrameHolds.size()||channel.nativeOneFrameHolds[end],1);
            }
        }
    }
    std::vector<uint8_t> bytes;std::string error;
    if(ac::Encode(tracks,bytes,error))pmm_extension::Set(doc,"org.mikudancestudio.animation_curves.v1",std::move(bytes));
    else throw std::runtime_error("Curve serialization failed: "+error);
    for(int n=0;n<4;++n)native[n]=std::uint8_t(intervals>>(n*8));
    // Full precision timing is MMS-only authoring data. The legacy PMM body
    // and VMD exporter retain their original integer control representation.
    if(intervals)pmm_extension::Set(doc,"org.mikudancestudio.camera_controls.v1",std::move(native));
    else pmm_extension::Erase(doc,"org.mikudancestudio.camera_controls.v1");
}
void Load(MMDApp&app,const pmm_extension::Document&doc){
    Reset(&app);const auto*r=pmm_extension::Find(doc,"org.mikudancestudio.animation_curves.v1");if(!r)return;
    std::vector<ac::NamedTrack> tracks;std::string error;
    if(!ac::Decode(r->value.data(),r->value.size(),tracks,error)){runtime_log::Write("Invalid PMM curves: %s",error.c_str());return;}
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
            for(const auto&key:t->track.keys)if(key.frame>mikudancestudio::frame_limits::maximum||key.value<field.minimum||key.value>field.maximum||
                (field.discrete&&std::floor(key.value)!=key.value)){valid=false;break;}
            if(!valid)break;
            const Channel original=c;c.track=t->track;
            if(!original.nativeControls.empty()){
                c.nativeControls.assign(c.track.keys.size(),{});c.controlGeometry.assign(c.track.keys.size(),{});
                c.nativeOneFrameHolds.assign(c.track.keys.size(),true);
                for(std::size_t end=1;end<c.track.keys.size();++end){
                    const auto&keys=original.track.keys;
                    const auto at=std::lower_bound(keys.begin(),keys.end(),c.track.keys[end].frame,[](const auto&key,int f){return key.frame<f;});
                    const auto n=std::size_t(at-keys.begin());
                    if(at==keys.end()||!n||at->frame!=c.track.keys[end].frame||keys[n-1].frame!=c.track.keys[end-1].frame)continue;
                    c.nativeControls[end]=original.nativeControls[n];c.controlGeometry[end]=original.controlGeometry[n];
                    c.nativeOneFrameHolds[end]=original.nativeOneFrameHolds[n];
                }
            }
        }
        if(valid){next.identity=Identity(app,k,slot);next.fingerprint=nativeAvailable?Fingerprint(app,k,slot):0;states[{&app,k,slot}]=std::move(next);}
    }
    if(const auto*record=pmm_extension::Find(doc,"org.mikudancestudio.camera_controls.v1")){
        const auto state=states.find({&app,Kind::Camera,0});const auto&bytes=record->value;
        if(state!=states.end()&&bytes.size()>=4){
            std::size_t offset=0;
            const auto read=[&](int size){std::uint64_t value=0;for(int n=0;n<size;++n)value|=std::uint64_t(bytes[offset++])<<(n*8);return value;};
            const auto count=read(4);bool valid=count==(bytes.size()-4)/45&&(bytes.size()-4)%45==0;
            auto channels=state->second.channels;
            for(std::uint64_t i=0;i<count&&valid;++i){
                const auto c=read(4),start=read(4),end=read(4);std::array<double,4> controls{};
                for(auto&control:controls){const auto bits=read(8);std::memcpy(&control,&bits,8);valid=valid&&std::isfinite(control)&&std::abs(control)<=1e12;}
                const auto hold=read(1);valid=valid&&hold<=1;
                const auto monotone=[](double p,double q){
                    const double a=1+3*p-3*q,b=2*(q-2*p);
                    double minimum=std::min(p,1-q);
                    if(a>0){const double t=-b/(2*a);if(t>0&&t<1)minimum=std::min(minimum,(a*t+b)*t+p);}
                    return minimum>=-1e-12;
                };
                valid=valid&&monotone(controls[0]/127,controls[2]/127)&&monotone(controls[1]/127,controls[3]/127);
                if(!valid||c>=channels.size()){valid=false;break;}
                auto&channel=channels[c];const auto&keys=channel.track.keys;
                const auto at=std::lower_bound(keys.begin(),keys.end(),end,[](const auto&key,std::uint64_t f){return std::uint64_t(key.frame)<f;});
                const auto index=std::size_t(at-keys.begin());
                if(at==keys.end()||index==0||std::uint64_t(at->frame)!=end||std::uint64_t(keys[index-1].frame)!=start){valid=false;break;}
                channel.nativeControls.resize(keys.size());channel.controlGeometry.resize(keys.size());
                channel.nativeControls[index]=controls;
                channel.controlGeometry[index]={keys[index-1].outWeight,keys[index-1].outSlope,at->inWeight,at->inSlope};
                channel.nativeOneFrameHolds.resize(keys.size(),true);channel.nativeOneFrameHolds[index]=hold!=0;
            }
            if(valid)state->second.channels=std::move(channels);
            else runtime_log::Write("Invalid PMM camera timing metadata");
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
            for(const auto&controls:channel.nativeControls)for(double value:controls){
                std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));mix(bits);
            }
            for(const auto&geometry:channel.controlGeometry)for(double value:geometry){
                std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));mix(bits);
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

bool CaptureAuthoring(MMDApp& app, AuthoringTable& out, std::wstring& error) {
    error.clear();
    if (!out.impl_) out.impl_.reset(new AuthoringTable::Impl());
    out.impl_->entries.clear();
    for (const auto& entry : states) {
        if (std::get<0>(entry.first) != &app) continue;
        const Kind kind = std::get<1>(entry.first);
        const int slot = std::get<2>(entry.first);
        // A track whose object is gone is not this app's authoring any more.
        if (entry.second.identity != Identity(app, kind, slot)) continue;
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
            const double value = ac::Evaluate(entry.channels[channel].track, key.frame).value;
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
        next.identity = Identity(app, item.kind, item.slot);
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
            entry.second.identity == Identity(app, std::get<1>(entry.first), std::get<2>(entry.first)))
            live.insert(entry);
    }
    if (live.size() != table.impl_->entries.size()) return false;
    auto left = live.begin();
    auto right = table.impl_->entries.begin();
    for (; left != live.end(); ++left, ++right) {
        if (left->first != right->first) return false;
        if (left->second.identity != right->second.identity) return false;
        if (left->second.fingerprint != right->second.fingerprint) return false;
        if (AuthoringDigest(left->second) != AuthoringDigest(right->second)) return false;
    }
    return true;
}
}
