#pragma once
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "native_curve_sample.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Pure evaluation of an already ordered lane. No host, callbacks or temporary
// containers are involved; scalar authoring retains animation_curve::Evaluate.
namespace mikumikustudio::native_camera_sampling {
// Keep the native player's float operation order and 12-step time inversion.
inline float CurveTime(const std::array<double,4>& control,float t,bool transport=false) noexcept {
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

namespace detail {
inline std::array<double,6> IntervalGeometry(const animation_curve::Key& a,const animation_curve::Key& b) noexcept {
    return {a.value,a.outWeight,a.outSlope,b.value,b.inWeight,b.inSlope};
}
// The signed opt-in is evaluation-only. Codec/edit callers keep the default
// positive domain, including the original source domain admission checks.
inline bool ValidPreservedData(const curve_editor::NativeCameraInterval& value,bool allowSigned=false) noexcept {
    const auto minimum=allowSigned?frame_limits::minimum:0;
    if(value.first<minimum||value.last<=value.first||value.last>frame_limits::maximum||
       value.sourceFirst<minimum||value.sourceLast<=value.sourceFirst||value.sourceLast>frame_limits::maximum||
       !std::isfinite(value.sampleFirst)||!std::isfinite(value.sampleLast)||
       value.sampleFirst<value.sourceFirst||value.sampleLast>value.sourceLast||value.sampleFirst>=value.sampleLast||
       !std::isfinite(value.sourceValueFirst)||!std::isfinite(value.sourceValueLast))return false;
    if(!value.oneFrameHold&&!value.transportTiming)return false;
    for(double component:value.controls)if(!std::isfinite(component)||component<0||component>127)return false;
    for(double component:value.geometry)if(!std::isfinite(component))return false;
    return true;
}
template<class Lane>
inline bool PreservedCurrent(const Lane& lane,const curve_editor::NativeCameraInterval& value,bool allowSigned=false) noexcept {
    if(!ValidPreservedData(value,allowSigned)||lane.track.discrete)return false;
    const auto& keys=lane.track.keys;
    const auto at=std::lower_bound(keys.begin(),keys.end(),value.last,[](const animation_curve::Key& k,int f){return k.frame<f;});
    return at!=keys.end()&&at!=keys.begin()&&at->frame==value.last&&(at-1)->frame==value.first&&
        at->interpolation==animation_curve::Interpolation::Bezier&&IntervalGeometry(*(at-1),*at)==value.geometry;
}
template<class Lane>
inline bool NativeGeometry(const Lane& lane,std::size_t end) noexcept {
    if(end==0||end>=lane.track.keys.size()||end>=lane.nativeControls.size()||end>=lane.controlGeometry.size())return false;
    const auto&a=lane.track.keys[end-1];const auto&b=lane.track.keys[end];
    return b.interpolation==animation_curve::Interpolation::Bezier&&
        lane.controlGeometry[end]==std::array<double,4>{a.outWeight,a.outSlope,b.inWeight,b.inSlope};
}
}

// Evaluate a retained source directly; validity/geometry selection belongs to
// Sample. Widen endpoints before subtraction, including the retimed divisor.
inline animation_curve::Sample SamplePreserved(const curve_editor::NativeCameraInterval& interval,double frame) noexcept {
    const double intervalSpan=double(interval.last)-double(interval.first);
    const double source=interval.sampleLast-interval.sampleFirst==intervalSpan?
        interval.sampleFirst+(frame-interval.first):interval.sampleFirst+(frame-interval.first)/intervalSpan*
        (interval.sampleLast-interval.sampleFirst);
    const double span=double(interval.sourceLast)-double(interval.sourceFirst);
    const bool cut=span==1.0&&interval.oneFrameHold;
    const double ratio=(source-interval.sourceFirst)/span;
    double value=interval.sourceValueFirst;
    if(source>=interval.sourceLast)value=interval.sourceValueLast;
    else if(source>interval.sourceFirst&&!cut)value+=(interval.sourceValueLast-interval.sourceValueFirst)*
        CurveTime(interval.controls,float(ratio),interval.transportTiming);
    const double derivative=cut?0:(interval.sourceValueLast-interval.sourceValueFirst)/span*
        native_curve_sample::Evaluate(interval.controls,ratio).slope*
        (interval.sampleLast-interval.sampleFirst)/intervalSpan;
    return {value,derivative};
}

namespace detail {
template<class Lane>
inline bool SamplePreservedInterval(const Lane& lane,double frame,animation_curve::Sample& out,bool allowSigned) noexcept {
    if(!std::isfinite(frame)||lane.track.keys.empty())return false;
    const auto& intervals=lane.preservedCameraIntervals;
    auto at=std::upper_bound(intervals.begin(),intervals.end(),frame,[](double f,const auto& value){return f<value.first;});
    if(at==intervals.begin())return false;const auto& interval=*--at;
    if(frame>=interval.first&&frame<=interval.last&&
       (frame<interval.last||interval.last==lane.track.keys.back().frame)&&PreservedCurrent(lane,interval,allowSigned)) {
        out=SamplePreserved(interval,frame);return true;
    }
    return false;
}
}

// Lane needs only track/nativeControls/controlGeometry/preservedCameraIntervals.
// allowSigned extends retained interval admission, not codec/edit/UI admission
// or the legacy raw-control and scalar fallback rules.
template<class Lane>
inline animation_curve::Sample Sample(const Lane& lane,double frame,bool allowSigned=false,
                                      bool interpolateAdjacent=false) noexcept {
    animation_curve::Sample preserved;
    if(detail::SamplePreservedInterval(lane,frame,preserved,allowSigned))return preserved;
    const auto&keys=lane.track.keys;
    if(keys.size()<2)return animation_curve::Evaluate(lane.track,frame);
    const auto value=[&](double at){
        if(at<=keys.front().frame)return keys.front().value;
        if(at>=keys.back().frame)return keys.back().value;
        const auto bound=std::upper_bound(keys.begin(),keys.end(),at,[](double f,const animation_curve::Key&k){return f<k.frame;});
        const auto end=std::size_t(bound-keys.begin());
        if(!detail::NativeGeometry(lane,end))return animation_curve::Evaluate(lane.track,at).value;
        const auto&a=keys[end-1];const auto&b=keys[end];
        // Fixed-step inversion only approximates t=0; exact keys stay exact.
        if(at==a.frame)return a.value;
        const double span=double(b.frame)-double(a.frame);
        if(span==1.0&&!interpolateAdjacent)return a.value;
        return a.value+(b.value-a.value)*CurveTime(lane.nativeControls[end],float((at-a.frame)/span));
    };
    double derivative=0;
    if(frame>=keys.front().frame&&frame<=keys.back().frame){
        auto bound=std::upper_bound(keys.begin(),keys.end(),frame,[](double f,const animation_curve::Key& k){return f<k.frame;});
        const auto end=(std::min)(std::size_t(bound-keys.begin()),keys.size()-1);
        if(end&&detail::NativeGeometry(lane,end)){
            const auto&a=keys[end-1];const auto&b=keys[end];const double span=double(b.frame)-double(a.frame);
            if(span>1.0||interpolateAdjacent)derivative=(b.value-a.value)/span*
                native_curve_sample::Evaluate(lane.nativeControls[end],(frame-a.frame)/span).slope;
        }else derivative=animation_curve::Evaluate(lane.track,frame).derivative;
    }
    return {value(frame),derivative};
}
}
