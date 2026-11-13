#pragma once
#include "negative_key_data.hpp"
#include "native_camera_sampling.hpp"
#include "mikumikustudio/bone_key_sampling.hpp"
#include <algorithm>
#include <cmath>

namespace mikumikustudio::negative_frames::sampling {
using Family = authoring::Family;
using Key = detail::Key;
using Values = std::array<double,16>;
struct Result {
    Values values{};
    // Absent does not mean enabled. The caller retains its registered baseline
    // for legacy keys; the bone dirty byte is a separate editing mechanism.
    bool physicsPresent=false, physicsDisabled=false;
};
namespace detail {
inline const Key& Row(const Key& key) noexcept {return key;}
inline const Key& Row(const Key* key) noexcept {return *key;}
inline bool Frame(double value) noexcept {
    return std::isfinite(value)&&value>=frame_limits::minimum&&value<=frame_limits::maximum;
}
inline bool FamilyKnown(Family family) noexcept {
    switch(family) {
    case Family::Bone:case Family::Morph:case Family::Camera:case Family::Light:
    case Family::Shadow:case Family::Gravity:case Family::Accessory:return true;
    }
    return false;
}
inline bool Finite(const Values& values) noexcept {
    for(double value:values)if(!std::isfinite(value)||std::abs(value)>(std::numeric_limits<float>::max)())return false;
    return true;
}
inline bool StoreFloat(double value,double& out) noexcept {
    if(!std::isfinite(value)||std::abs(value)>(std::numeric_limits<float>::max)())return false;
    out=double(float(value));return true;
}
inline bool Compatible(Family family,const Key& key) noexcept {
    return FamilyKnown(family)&&Frame(key.frame)&&Finite(key.values)&&
        (!key.author||key.author->FamilyKind()==family);
}
inline void Physics(const Key& key,Result& sample) noexcept {
    if(key.author) {
        const auto& native=key.author->Native();
        sample.physicsPresent=native.physicsPresent;sample.physicsDisabled=native.physicsDisabled;
    }
}
inline bone_scale::Matrix Scale(const Values& values) noexcept {
    bone_scale::Matrix result;std::copy_n(values.begin()+7,9,result.v.begin());return result;
}
inline void SetScale(Values& values,const bone_scale::Matrix& scale) noexcept {
    std::copy(scale.v.begin(),scale.v.end(),values.begin()+7);
}
inline Values Legacy(Family family,const Key& a,const Key& b,double frame) noexcept {
    Values out=a.values;
    const double t=(frame-double(a.frame))/(double(b.frame)-double(a.frame));
    const unsigned count=family==Family::Bone||family==Family::Morph?16:
        family==Family::Camera||family==Family::Accessory?8:family==Family::Light?6:
        family==Family::Gravity?4:0;
    // Preserve the old ordinary-seek float stores, including stores BEFORE
    // quaternion normalization. The former history-only double path differed.
    for(unsigned i=0;i<count;++i)out[i]=float(out[i]+t*(b.values[i]-out[i]));
    if(family==Family::Shadow)out[1]=float(out[1]+t*(b.values[1]-out[1]));
    if(family==Family::Bone) {
        double dot=0;for(unsigned i=3;i<7;++i)dot+=a.values[i]*b.values[i];
        double norm=0;
        for(unsigned i=3;i<7;++i){out[i]=float((1-t)*a.values[i]+t*(dot<0?-b.values[i]:b.values[i]));norm+=out[i]*out[i];}
        if(norm>1e-20)for(unsigned i=3;i<7;++i)out[i]=float(out[i]/std::sqrt(norm));
    }
    return out;
}
inline bool Native(Family family,const Key& a,const Key& b,double frame,Values& out) noexcept {
    out=a.values;
    const double span=double(b.frame)-double(a.frame),offset=frame-double(a.frame),t=offset/span;
    const float fraction=float(offset)/float(span);
    const auto& native=b.author->Native();
    switch(family) {
    case Family::Bone: {
        if(!native.boneControlsPresent||!native.scaleControlsPresent)return false;
        const auto ease=[&](unsigned lane){return bone_scale::CurveTime({native.boneControls[lane],
            native.boneControls[lane+4],native.boneControls[lane+8],native.boneControls[lane+12]},fraction);};
        for(unsigned i=0;i<3;++i){const float start=float(a.values[i]),delta=float(b.values[i])-start;
            out[i]=delta==0?start:float(double(ease(i))*delta+double(start));}
        float qa[4],qb[4],q[4];
        for(unsigned i=0;i<4;++i){qa[i]=float(a.values[i+3]);qb[i]=float(b.values[i+3]);}
        bone_key_sampling::SeekSlerp(qa,qb,ease(3),q);std::copy_n(q,4,out.begin()+3);
        const auto scale=bone_scale::Interpolate(Scale(a.values),Scale(b.values),
            bone_scale::CurveTime(native.scaleControls,float(t)));
        SetScale(out,scale);break;
    }
    case Family::Morph:
        // The native morph player rounds time and the float endpoint delta
        // before its double multiply/add; legacy v1 uses a different order.
        out[0]=float(double(float(t))*double(float(b.values[0])-float(a.values[0]))+a.values[0]);break;
    case Family::Camera:
        if(!native.cameraControlsPresent)return false;
        if(span>1)for(unsigned i=0;i<8;++i) {
            const unsigned lane=i<3?i:i<6?3:i==6?4:5;
            const std::array<double,4> controls{double(native.cameraControls[lane]),double(native.cameraControls[lane+6]),
                double(native.cameraControls[lane+12]),double(native.cameraControls[lane+18])};
            out[i]=float(double(native_camera_sampling::CurveTime(controls,float(t)))*(b.values[i]-a.values[i])+a.values[i]);
        }
        break;
    case Family::Light:
        for(unsigned i=0;i<6;++i)out[i]=float(double(float(b.values[i])-float(a.values[i]))*t+a.values[i]);
        break;
    case Family::Shadow:break; // Native shadow distance is stepped, unlike v1 negative data.
    case Family::Gravity: {
        for(unsigned i=0;i<4;++i)out[i]=(float(b.values[i])-float(a.values[i]))*fraction+float(a.values[i]);
        if(a.values[4]<INT32_MIN||a.values[4]>INT32_MAX||b.values[4]<INT32_MIN||b.values[4]>INT32_MAX||
            std::trunc(a.values[4])!=a.values[4]||std::trunc(b.values[4])!=b.values[4])return false;
        const auto delta=std::int64_t(b.values[4])-std::int64_t(a.values[4]);
        if(delta<INT32_MIN||delta>INT32_MAX)return false;
        const float scaled=fraction*float(delta);
        if(!std::isfinite(scaled)||double(scaled)<INT32_MIN||double(scaled)>INT32_MAX)return false;
        const auto noise=std::int64_t(std::int32_t(scaled))+std::int64_t(a.values[4]);
        if(noise<INT32_MIN||noise>INT32_MAX)return false;
        out[4]=double(noise);out[5]=b.values[5];break; // Native upper-key mode selection.
    }
    case Family::Accessory:
        for(unsigned i=6;i<8;++i)out[i]=float(a.values[i])+fraction*(float(b.values[i])-float(a.values[i]));
        break; // Native transforms/parent/visibility are stepped.
    }
    return Finite(out);
}
inline double GlobalBound(Family family,unsigned lane,double value) noexcept {
    // Match the existing global author's field bounds, independent of labels
    // or the current editor's display range.
    double low=-1e8,high=1e8;
    switch(family) {
    case Family::Camera:
        if(lane==7){low=1;high=179;}else if(lane==8){low=0;high=1;}
        else if(lane==9){low=-1;high=254;}else if(lane==10){low=0;high=1000000;}
        break;
    case Family::Light:if(lane>=3){low=0;high=1;}break;
    case Family::Shadow:low=0;high=lane==0?2:1;break;
    case Family::Gravity:
        if(lane==0){low=0;high=100000;}else if(lane==4){low=0;high=1000000;}
        else if(lane==5){low=0;high=1;}break;
    case Family::Accessory:
        if(lane==6){low=.000001;high=1e6;}else if(lane>=7&&lane<=9){low=0;high=1;}
        else if(lane==10){low=-1;high=254;}else if(lane==11){low=0;high=1000000;}
        break;
    default:break;
    }
    return std::clamp(value,low,high);
}
inline bool Authored(Family family,const authoring::Curves& curves,double frame,Values& out) noexcept {
#if MMD_ORIGINAL_PROFILE
    // The original profile does not link the free-curve engine or produce
    // authored negative rows. Refuse unsupported data instead of borrowing a
    // linear fallback or pulling full-profile authoring into that build.
    (void)family;(void)curves;(void)frame;(void)out;return false;
#else
    const auto& data=curves.Data();
    std::array<double,12> sampled{};
    for(const auto& lane:data.lanes) {
        const auto result=lane.nativeCameraTiming?native_camera_sampling::Sample(lane,frame,true):
            animation_curve::Evaluate(lane.track,frame);
        if(!std::isfinite(result.value))return false;
        sampled[lane.index]=result.value;
    }
    constexpr double pi=3.14159265358979323846;
    if(family==Family::Bone) {
        for(unsigned i=0;i<3;++i)if(!StoreFloat(sampled[i],out[i]))return false;
        const double x=sampled[3]*pi/360,y=sampled[4]*pi/360,z=sampled[5]*pi/360;
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
        const double sx=std::sin(x),cx=std::cos(x),sy=std::sin(y),cy=std::cos(y),sz=std::sin(z),cz=std::cos(z);
        out[3]=float(cy*sx*cz+sy*cx*sz);out[4]=float(sy*cx*cz-cy*sx*sz);
        out[5]=float(cy*cx*sz-sy*sx*cz);out[6]=float(cy*cx*cz+sy*sx*sz);
        // Free scale lanes replace row lengths, retaining the sampled native
        // matrix basis/shear. They are not a diagonal scale reconstruction.
        for(unsigned row=0;row<3;++row) {
            double norm=0;for(unsigned col=0;col<3;++col)norm+=out[7+row*3+col]*out[7+row*3+col];
            norm=std::sqrt(norm);
            for(unsigned col=0;col<3;++col)if(!StoreFloat(norm>1e-12?
                out[7+row*3+col]*sampled[6+row]/norm:(col==row?sampled[6+row]:0),out[7+row*3+col]))return false;
        }
    } else if(family==Family::Morph) {
        // Never clamp morph weights to the editor's 0..1 view.
        if(!StoreFloat(sampled[0],out[0]))return false;
    }
    else for(unsigned i=0;i<data.lanes.size();++i) {
        double value=GlobalBound(family,i,sampled[i]);
        if((family==Family::Camera||family==Family::Accessory)&&i>=3&&i<6)value*=pi/180;
        if(!StoreFloat(value,out[i]))return false;
    }
    return Finite(out)&&(family!=Family::Bone||bone_scale::Valid(Scale(out)));
#endif
}
}
inline bool Point(Family family,const Key& key,Result& out) noexcept {
    if(!detail::Compatible(family,key))return false;
    Result result;result.values=key.values;detail::Physics(key,result);
    if(key.author&&key.author->Semantics()==authoring::Mode::Authored&&
        !detail::Authored(family,*key.author->Author(),key.frame,result.values))return false;
    out=result;return true;
}
// The destination owns the incoming interval, as on native positive tracks.
// Inputs are prepared key views in one coordinate system; no current pose,
// scene registry or editor callback can fill a missing dependency. Producers
// must reconcile an authored curve with edited endpoint values before publish.
inline bool Interval(Family family,const Key& a,const Key& b,double frame,Result& out) noexcept {
    if(!detail::Compatible(family,a)||!detail::Compatible(family,b)||!detail::Frame(frame)||b.frame<=a.frame)return false;
    if(frame<=a.frame)return Point(family,a,out);
    if(frame>=b.frame)return Point(family,b,out);
    Result result;detail::Physics(a,result);
    if(!b.author)result.values=detail::Legacy(family,a,b,frame);
    else {
        const auto mode=b.author->Semantics();
        if(mode==authoring::Mode::Native||family==Family::Bone) {
            if(!detail::Native(family,a,b,frame,result.values))return false;
        } else result.values=a.values;
        if(mode==authoring::Mode::Authored&&!detail::Authored(family,*b.author->Author(),frame,result.values))return false;
    }
    if(!detail::Finite(result.values))return false;
    out=result;return true;
}
template<class Keys,class Predicate>
bool Track(const Keys& keys,Family family,double frame,const Key& zero,Result& out,Predicate matches) noexcept {
    if(!detail::Frame(frame)||frame>=0||zero.frame!=0)return false;
    const Key* first=nullptr;const Key* previous=nullptr;const Key* next=nullptr;
    for(const auto& entry:keys) {const auto& key=detail::Row(entry);if(matches(key)) {
        if(!first||key.frame<first->frame)first=&key;
        if(key.frame<=frame&&(!previous||key.frame>previous->frame))previous=&key;
        if(key.frame>frame&&(!next||key.frame<next->frame))next=&key;
    }}
    if(!first)return Point(family,zero,out);
    if(!previous)return Point(family,*first,out);
    if(frame==previous->frame)return Point(family,*previous,out);
    return Interval(family,*previous,next?*next:zero,frame,out);
}
// Display/IK/selector rows are discrete and have no authoring::Family. Keep
// their exact stored integer values; they do not share morph/bone math.
template<class Keys,class Predicate>
Values Step(const Keys& keys,double frame,const Values& zero,Predicate matches) noexcept {
    const Key* first=nullptr;const Key* previous=nullptr;
    for(const auto& entry:keys) {const auto& key=detail::Row(entry);if(matches(key)) {
        if(!first||key.frame<first->frame)first=&key;
        if(key.frame<=frame&&(!previous||key.frame>previous->frame))previous=&key;
    }}
    return previous?previous->values:first?first->values:zero;
}
}
