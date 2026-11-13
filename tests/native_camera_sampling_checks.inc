// Include at file scope after the parent's Check(bool,const char*) declaration.
// No runner, model, registry or allocation-injection hook is required.
#include "../src/animation/native_camera_sampling.hpp"
#include "../src/animation/negative_authoring.hpp"
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {
namespace native_camera_sampling_checks {
namespace ac = mikumikustudio::animation_curve;
namespace ce = mikumikustudio::curve_editor;
namespace ns = mikumikustudio::native_camera_sampling;
namespace au = mikumikustudio::negative_frames::authoring;
namespace fl = mikumikustudio::frame_limits;

// Deliberately has no callback, identifier, owner or nativeCameraTiming flag.
struct Lane {
    ac::Track track;
    std::vector<std::array<double,4>> nativeControls,controlGeometry;
    std::vector<ce::NativeCameraInterval> preservedCameraIntervals;
};
static_assert(std::int64_t(fl::maximum)-std::int64_t(fl::minimum)==4294967292LL,
              "cover the full signed frame span");
static_assert(noexcept(ns::CurveTime(std::declval<const std::array<double,4>&>(),0.f)), "time sampling is noexcept");
static_assert(noexcept(ns::SamplePreserved(std::declval<const ce::NativeCameraInterval&>(),0)), "source sampling is noexcept");
static_assert(noexcept(ns::Sample(std::declval<const Lane&>(),0)), "minimal lane sampling is noexcept");
static_assert(noexcept(ns::Sample(std::declval<const au::Lane&>(),0,true)), "authoring lane sampling is noexcept");
static_assert(noexcept(ns::Sample(std::declval<const ce::Channel&>(),0)), "channel sampling is noexcept");

void Near(double actual,double expected,double tolerance,const char* why) {
    Check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,why);
}
void Same(ac::Sample actual,ac::Sample expected,const char* why) {
    Check(actual.value==expected.value&&actual.derivative==expected.derivative,why);
}

// Frozen pre-extraction arithmetic. These oracles deliberately never call the
// shared time/source sampler or native_curve_sample (including its derivative).
float LegacyTime(const std::array<double,4>& control,float t,bool transport=false) {
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
double LegacyCubic(double a,double b,double u) {
    const double v=1-u;return 3*v*v*u*a+3*v*u*u*b+u*u*u;
}
double LegacyParameter(double x1,double x2,double time) {
    if(time<=0)return 0;if(time>=1)return 1;
    double low=0,high=1;
    for(int i=0;i<48;++i){const double middle=(low+high)*.5;
        if(LegacyCubic(x1,x2,middle)<time)low=middle;else high=middle;}
    return (low+high)*.5;
}
double LegacySlope(const std::array<double,4>& controls,double time) {
    const double x1=controls[0]/127.0,y1=controls[1]/127.0;
    const double x2=controls[2]/127.0,y2=controls[3]/127.0;
    time=std::clamp(time,0.0,1.0);
    const double u=LegacyParameter(x1,x2,time),v=1-u;
    const double dx=3*v*v*x1+6*v*u*(x2-x1)+3*u*u*(1-x2);
    const double dy=3*v*v*y1+6*v*u*(y2-y1)+3*u*u*(1-y2);
    if(dx>1e-10)return dy/dx;
    const double low=(std::max)(0.0,time-1e-6),high=(std::min)(1.0,time+1e-6);
    return (LegacyCubic(y1,y2,LegacyParameter(x1,x2,high))-
            LegacyCubic(y1,y2,LegacyParameter(x1,x2,low)))/(high-low);
}
ac::Sample LegacySource(const ce::NativeCameraInterval& interval,double frame) {
    // Only positive-domain fixtures call this old implementation: its int
    // subtractions intentionally document the overflow fixed by extraction.
    const double source=interval.sampleLast-interval.sampleFirst==double(interval.last)-interval.first?
        interval.sampleFirst+(frame-interval.first):interval.sampleFirst+(frame-interval.first)/(interval.last-interval.first)*
        (interval.sampleLast-interval.sampleFirst);
    const double span=double(interval.sourceLast)-interval.sourceFirst;
    const bool cut=span==1.0&&interval.oneFrameHold;
    const double ratio=(source-interval.sourceFirst)/span;
    double value=interval.sourceValueFirst;
    if(source>=interval.sourceLast)value=interval.sourceValueLast;
    else if(source>interval.sourceFirst&&!cut)value+=(interval.sourceValueLast-interval.sourceValueFirst)*
        LegacyTime(interval.controls,float(ratio),interval.transportTiming);
    const double derivative=cut?0:(interval.sourceValueLast-interval.sourceValueFirst)/span*
        LegacySlope(interval.controls,ratio)*(interval.sampleLast-interval.sampleFirst)/(interval.last-interval.first);
    return {value,derivative};
}
bool LegacyGeometry(const Lane& lane,std::size_t end) {
    if(end==0||end>=lane.track.keys.size()||end>=lane.nativeControls.size()||end>=lane.controlGeometry.size())return false;
    const auto&a=lane.track.keys[end-1];const auto&b=lane.track.keys[end];
    return b.interpolation==ac::Interpolation::Bezier&&
        lane.controlGeometry[end]==std::array<double,4>{a.outWeight,a.outSlope,b.inWeight,b.inSlope};
}
ac::Sample LegacyRaw(const Lane& lane,double frame) {
    const auto&keys=lane.track.keys;
    if(keys.size()<2)return ac::Evaluate(lane.track,frame);
    const auto value=[&](double at){
        if(at<=keys.front().frame)return keys.front().value;
        if(at>=keys.back().frame)return keys.back().value;
        const auto bound=std::upper_bound(keys.begin(),keys.end(),at,[](double f,const ac::Key&k){return f<k.frame;});
        const auto end=std::size_t(bound-keys.begin());
        if(!LegacyGeometry(lane,end))return ac::Evaluate(lane.track,at).value;
        const auto&a=keys[end-1];const auto&b=keys[end];
        if(at==a.frame)return a.value;
        const double span=double(b.frame)-a.frame;
        if(span==1.0)return a.value;
        return a.value+(b.value-a.value)*LegacyTime(lane.nativeControls[end],float((at-a.frame)/span));
    };
    double derivative=0;
    if(frame>=keys.front().frame&&frame<=keys.back().frame){
        auto bound=std::upper_bound(keys.begin(),keys.end(),frame,[](double f,const ac::Key& k){return f<k.frame;});
        const auto end=(std::min)(std::size_t(bound-keys.begin()),keys.size()-1);
        if(end&&LegacyGeometry(lane,end)){
            const auto&a=keys[end-1];const auto&b=keys[end];const double span=double(b.frame)-a.frame;
            if(span>1.0)derivative=(b.value-a.value)/span*LegacySlope(lane.nativeControls[end],(frame-a.frame)/span);
        }else derivative=ac::Evaluate(lane.track,frame).derivative;
    }
    return {value(frame),derivative};
}

std::array<double,6> Geometry(const ac::Key& a,const ac::Key& b) {
    return {a.value,a.outWeight,a.outSlope,b.value,b.inWeight,b.inSlope};
}
Lane MakeLane(std::int32_t first,std::int32_t last,const std::array<double,4>& controls={100,10,20,115}) {
    Lane lane;
    ac::Key a,b;a.frame=first;b.frame=last;a.value=-2;b.value=6;
    a.tangent=b.tangent=ac::Tangent::Free;
    lane.track.keys={a,b};
    lane.nativeControls={{},controls};
    lane.controlGeometry={{},{a.outWeight,a.outSlope,b.inWeight,b.inSlope}};
    return lane;
}
ce::NativeCameraInterval Retained(const Lane& lane) {
    ce::NativeCameraInterval value;
    const auto&a=lane.track.keys[0];const auto&b=lane.track.keys[1];
    value.first=value.sourceFirst=a.frame;value.last=value.sourceLast=b.frame;
    value.sampleFirst=a.frame;value.sampleLast=b.frame;
    value.sourceValueFirst=a.value;value.sourceValueLast=b.value;
    value.controls=lane.nativeControls[1];value.geometry=Geometry(a,b);
    return value;
}
void Translate(Lane& lane,std::int64_t shift) {
    for(auto& key:lane.track.keys)key.frame=static_cast<std::int32_t>(std::int64_t(key.frame)+shift);
    for(auto& interval:lane.preservedCameraIntervals){
        interval.first=static_cast<std::int32_t>(std::int64_t(interval.first)+shift);
        interval.last=static_cast<std::int32_t>(std::int64_t(interval.last)+shift);
        interval.sourceFirst=static_cast<std::int32_t>(std::int64_t(interval.sourceFirst)+shift);
        interval.sourceLast=static_cast<std::int32_t>(std::int64_t(interval.sourceLast)+shift);
        interval.sampleFirst+=double(shift);interval.sampleLast+=double(shift);
    }
}
ac::Sample ScalarOracle(double first,double last,double u,double span) {
    // Default scalar handles have x(s)=s and zero endpoint slopes.
    return {first+(last-first)*(3*u*u-2*u*u*u),(last-first)*6*u*(1-u)/span};
}
void Scalar(const Lane& lane,double frame,const char* why,bool allowSigned=true) {
    const auto&a=lane.track.keys.front();const auto&b=lane.track.keys.back();
    const double span=double(b.frame)-double(a.frame),u=(frame-a.frame)/span;
    const auto expected=ScalarOracle(a.value,b.value,u,span);
    const auto actual=ns::Sample(lane,frame,allowSigned);
    Near(actual.value,expected.value,2e-11,why);
    Near(actual.derivative,expected.derivative,2e-10/span,why);
}

void PositiveCompatibility() {
    const std::array<double,4> controls[]={
        {20,20,107,107},{100,10,20,115},{0,127,0,127},{127,0,0,127},
        {0,0,127,127},{127,0,127,0},{20,20+5e-13,107,107-5e-13}};
    for(const auto& control:controls){
        for(float time:{0.f,0.000001f,.1234567f,.25f,.5f,.75f,.999999f,1.f})
            for(bool transport:{false,true})
                Check(ns::CurveTime(control,time,transport)==LegacyTime(control,time,transport),
                      "CurveTime preserves independent legacy float arithmetic");
        for(int span:{1,2,37,fl::maximum}){
            const auto lane=MakeLane(0,span,control);
            for(double u:{0.0,.03125,.123456789,.25,.5,.875,1.0})
                Same(ns::Sample(lane,span*u),LegacyRaw(lane,span*u),"positive raw sampling and native derivative match legacy");
            Same(ns::Sample(lane,-.5),{-2,0},"raw camera clamps before its first key");
            Same(ns::Sample(lane,double(span)+.5),{6,0},"raw camera clamps after its last key");
        }
        auto lane=MakeLane(100,148,control);
        auto interval=Retained(lane);
        for(bool retimed:{false,true})for(bool transport:{false,true}){
            auto slice=interval;slice.transportTiming=transport;
            if(retimed){slice.sampleFirst=104;slice.sampleLast=140;}
            lane.preservedCameraIntervals={slice};
            for(double frame:{100.0,100.125,112.0,124.0,147.875,148.0}){
                const auto expected=LegacySource(slice,frame);
                Same(ns::SamplePreserved(slice,frame),expected,"positive source sampling matches frozen legacy arithmetic");
                Same(ns::Sample(lane,frame),expected,"positive retained interval selects the source oracle");
            }
        }
    }
    const std::array<double,4> nearDiagonal{20,20+5e-13,107,107-5e-13};
    Check(ns::CurveTime(nearDiagonal,.1234567f,true)==.1234567f&&
          ns::CurveTime(nearDiagonal,.1234567f,false)!=.1234567f,
          "transport alone enables the near-diagonal shortcut");

    auto lane=MakeLane(0,17);
    auto key=lane.track.keys.back();key.frame=18;key.value=11;
    lane.track.keys.push_back(key);lane.nativeControls.push_back({0,127,0,127});
    lane.controlGeometry.push_back({key.outWeight,key.outSlope,key.inWeight,key.inSlope});
    for(double frame:{0.0,16.999,17.0,17.125,17.999,18.0})
        Same(ns::Sample(lane,frame),LegacyRaw(lane,frame),"exact interior keys and one-frame cuts retain native semantics");
    Same(ns::Sample(lane,17),{6,0},"exact interior key is not perturbed by inverse at zero");
    Same(ns::Sample(lane,17.75),{6,0},"one-frame native interval holds until its endpoint");
}

void SignedTranslation() {
    for(bool preserved:{false,true}){
        auto positive=MakeLane(100,148);
        if(preserved){auto interval=Retained(positive);interval.sampleFirst=104;interval.sampleLast=140;
            positive.preservedCameraIntervals={interval};}
        for(std::int64_t shift:{std::int64_t(-124),std::int64_t(-148),std::int64_t(fl::minimum)-100,
                                std::int64_t(fl::maximum)-148}){
            auto signedLane=positive;Translate(signedLane,shift);
            for(double frame:{100.0,100.125,112.0,124.0,147.875,148.0}){
                const auto expected=preserved?LegacySource(positive.preservedCameraIntervals[0],frame):LegacyRaw(positive,frame);
                Same(ns::Sample(signedLane,frame+double(shift),true),expected,"signed translation preserves legacy native value and derivative");
            }
        }
    }
    const auto minimal=MakeLane(-10,10);
    au::Lane author;author.track=minimal.track;author.nativeControls=minimal.nativeControls;
    author.controlGeometry=minimal.controlGeometry;
    ce::Channel channel;channel.track=minimal.track;channel.nativeControls=minimal.nativeControls;
    channel.controlGeometry=minimal.controlGeometry;
    bool called=false;
    channel.sample=[&](const ce::Channel&,double){called=true;return ac::Sample{999,999};};
    Same(ns::Sample(author,-2.5,true),ns::Sample(minimal,-2.5,true),"real authoring lane instantiates the four-member interface");
    Same(ns::Sample(channel,-2.5,true),ns::Sample(minimal,-2.5,true),"editor channel instantiates the same pure interface");
    auto retained=Retained(minimal);retained.sampleFirst=-8;retained.sampleLast=8;
    author.preservedCameraIntervals={retained};channel.preservedCameraIntervals={retained};
    auto positiveRetained=retained;
    positiveRetained.first+=10;positiveRetained.last+=10;
    positiveRetained.sourceFirst+=10;positiveRetained.sourceLast+=10;
    positiveRetained.sampleFirst+=10;positiveRetained.sampleLast+=10;
    const auto expected=LegacySource(positiveRetained,7.5);
    Same(ns::Sample(author,-2.5,true),expected,"real authoring lane selects retained source metadata");
    Same(ns::Sample(channel,-2.5,true),expected,"editor channel selects the same retained source metadata");
    Check(!called,"shared sampler never invokes the channel callback");
    for(std::int32_t first:{fl::minimum,-1,fl::maximum-1}){
        const auto cut=MakeLane(first,first+1);
        Same(ns::Sample(cut,double(first),true),{-2,0},"signed cut starts at the exact key");
        Same(ns::Sample(cut,double(first)+.5,true),{-2,0},"signed one-frame hold works at both frame limits and across zero");
        Same(ns::Sample(cut,double(first)+1,true),{6,0},"signed cut switches at the exact next key");
    }
}

void WideSpans() {
    const double span=double(fl::maximum)-double(fl::minimum);
    auto lane=MakeLane(fl::minimum,fl::maximum,{20,20,107,107});
    for(double u:{0.0,.125,.25,.5,.75,.875,1.0}){
        const auto actual=ns::Sample(lane,double(fl::minimum)+span*u,true);
        Near(actual.value,-2+8*u,0,"full-span native diagonal agrees with analytic value");
        Near(actual.derivative,8/span,1e-13/span,"full-span native derivative does not overflow or collapse to zero");
    }
    // x(s)=s, y(s)=3s^2-2s^3 gives an independent nonlinear dy/dframe.
    // The value still follows the historical float inverse, not that ideal y.
    const auto nonlinear=MakeLane(fl::minimum,fl::maximum,{127.0/3,0,254.0/3,127});
    for(double u:{.125,.25,.5,.75,.875}){
        const auto actual=ns::Sample(nonlinear,double(fl::minimum)+span*u,true);
        Near(actual.value,-2+8*double(LegacyTime(nonlinear.nativeControls[1],float(u))),0,
             "full-span nonlinear native values retain the old float inverse");
        Near(actual.derivative,48*u*(1-u)/span,1e-12/span,
             "full-span nonlinear native derivative agrees with analytic polynomial");
    }
    auto interval=Retained(lane);
    interval.sourceFirst=-10;interval.sourceLast=10;interval.sampleFirst=-5;interval.sampleLast=5;
    lane.track.keys[0].value=0;lane.track.keys[1].value=4;
    interval.geometry=Geometry(lane.track.keys[0],lane.track.keys[1]);
    lane.preservedCameraIntervals={interval};
    for(double u:{0.0,.125,.25,.5,.75,.875,1.0}){
        const double frame=double(fl::minimum)+span*u;
        const auto direct=ns::SamplePreserved(interval,frame);
        Near(direct.value,4*u,0,"wide retained retime maps to analytic source value");
        Near(direct.derivative,4/span,1e-13/span,"wide retained retime uses widened visible divisor");
        Same(ns::Sample(lane,frame,true),direct,"wide signed retained interval is selected");
    }
    // Both the source and visible domain span all 4294967292 legal frames.
    interval.sourceFirst=fl::minimum;interval.sourceLast=fl::maximum;
    interval.sampleFirst=double(fl::minimum)+64;interval.sampleLast=double(fl::maximum)-64;
    lane.preservedCameraIntervals={interval};
    for(double u:{.125,.25,.5,.75,.875}){
        const double frame=double(fl::minimum)+span*u;
        const double ratio=(64+(span-128)*u)/span;
        const auto actual=ns::Sample(lane,frame,true);
        Near(actual.value,-2+8*double(float(ratio)),0,"full source-domain span preserves float timing conversion");
        Near(actual.derivative,8/span*(span-128)/span,1e-13/span,"full source and visible spans retain derivative chain rule");
    }
    // The old fast mapping branch must also be exercised across both limits.
    interval.sampleFirst=fl::minimum;interval.sampleLast=fl::maximum;
    lane.preservedCameraIntervals={interval};
    Same(ns::Sample(lane,0,true),{2,8/span},"equal full spans use the exact translation mapping branch");
}

void PreservedBoundariesAndCuts() {
    auto lane=MakeLane(0,20,{20,20,107,107});
    const auto left=Retained(lane);
    auto key=lane.track.keys.back();key.frame=40;key.value=22;
    lane.track.keys.push_back(key);
    auto right=left;right.first=right.sourceFirst=20;right.last=right.sourceLast=40;
    right.sampleFirst=20;right.sampleLast=40;right.sourceValueFirst=6;right.sourceValueLast=22;
    right.geometry=Geometry(lane.track.keys[1],lane.track.keys[2]);
    lane.preservedCameraIntervals={left,right};
    lane.nativeControls.clear();lane.controlGeometry.clear();
    for(double frame:{0.0,.125,19.875,20.0,20.125,39.875,40.0})
        Same(ns::Sample(lane,frame),LegacySource(frame<20?left:right,frame),
             "adjacent exact slices choose the right interval at a shared key and include the final endpoint");
    auto signedLane=lane;Translate(signedLane,-20);
    for(double frame:{0.0,10.0,20.0,30.0,40.0})
        Same(ns::Sample(signedLane,frame-20,true),LegacySource(frame<20?left:right,frame),
             "signed retained slices preserve half-open boundary selection");
    signedLane.preservedCameraIntervals[1].geometry[0]+=1;
    Same(ns::Sample(signedLane,0,true),{6,0},"stale right slice releases the shared boundary to scalar authoring");
    signedLane.preservedCameraIntervals.pop_back();
    Same(ns::Sample(signedLane,0,true),{6,0},"nonfinal left slice cannot claim its excluded right boundary");
    Same(ns::Sample(signedLane,20.5,true),{22,0},"retained intervals do not extrapolate after the track");

    auto cut=MakeLane(-10,10,{20,20+5e-13,107,107-5e-13});
    auto interval=Retained(cut);interval.sourceFirst=-1;interval.sourceLast=0;
    interval.sampleFirst=-1;interval.sampleLast=0;
    cut.preservedCameraIntervals={interval};
    for(double frame:{-10.0,-9.5,0.0,9.5})
        Same(ns::Sample(cut,frame,true),{-2,0},"retimed signed one-frame source keeps the native hold");
    Same(ns::Sample(cut,10,true),{6,0},"one-frame source switches exactly at its final endpoint");
    interval.oneFrameHold=false;interval.transportTiming=true;
    cut.preservedCameraIntervals={interval};
    for(double frame:{-10.0,-7.5,0.0,7.5,10.0}){
        const auto actual=ns::Sample(cut,frame,true);
        Near(actual.value,-2+8*((frame+10)/20),0,"transport one-frame source remains continuous and near-diagonal");
        Near(actual.derivative,8.0/20,1e-12,"transport source retains its native analytic derivative");
    }
    interval.transportTiming=false;cut.preservedCameraIntervals={interval};
    cut.nativeControls.clear();cut.controlGeometry.clear();
    Scalar(cut,-7.5,"an unmarked continuous one-frame source is rejected");
}

void RejectionAndFallback() {
    // Scalar expected values are analytic, not obtained from Evaluate or Sample.
    auto lane=MakeLane(-20,0);
    lane.preservedCameraIntervals={Retained(lane)};
    lane.nativeControls.clear();lane.controlGeometry.clear();
    Scalar(lane,-15,"default sampling still rejects negative retained domains",false);
    const auto retained=ns::Sample(lane,-15,true);
    Check(std::abs(retained.value-ScalarOracle(-2,6,.25,20).value)>.01,
          "signed retained fixture distinguishes native sampling from scalar fallback");
    Check(!ns::detail::ValidPreservedData(lane.preservedCameraIntervals[0])&&
           ns::detail::ValidPreservedData(lane.preservedCameraIntervals[0],true),
          "positive-only validation remains the default for existing codec and edit wrappers");

    // A positive visible interval alone must not admit a negative source domain.
    auto positive=MakeLane(100,148,{20,20,107,107});
    auto source=Retained(positive);source.sourceFirst=-10;source.sourceLast=10;
    source.sampleFirst=-10;source.sampleLast=10;
    positive.preservedCameraIntervals={source};positive.nativeControls.clear();positive.controlGeometry.clear();
    Scalar(positive,112,"positive visible frames still reject a negative source by default",false);
    Same(ns::Sample(positive,112,true),{0,8.0/48},"signed opt-in admits the independently negative source domain");

    const double nan=(std::numeric_limits<double>::quiet_NaN)();
    const double inf=(std::numeric_limits<double>::infinity)();
    for(int invalid=0;invalid<19;++invalid){
        auto broken=lane;auto& value=broken.preservedCameraIntervals[0];
        switch(invalid){
        case 0:value.first=fl::minimum-1;break;
        case 1:value.last=fl::maximum+1;break;
        case 2:value.sourceFirst=fl::minimum-1;break;
        case 3:value.sourceLast=fl::maximum+1;break;
        case 4:value.last=value.first;break;
        case 5:value.sourceLast=value.sourceFirst;break;
        case 6:value.sampleFirst=-21;break;
        case 7:value.sampleLast=1;break;
        case 8:value.sampleFirst=value.sampleLast;break;
        case 9:value.sampleFirst=nan;break;
        case 10:value.sampleLast=inf;break;
        case 11:value.sourceValueFirst=nan;break;
        case 12:value.sourceValueLast=inf;break;
        case 13:value.controls[0]=-1;break;
        case 14:value.controls[1]=128;break;
        case 15:value.controls[2]=nan;break;
        case 16:value.geometry[0]=inf;break;
        case 17:value.oneFrameHold=false;value.transportTiming=false;break;
        case 18:value.sourceFirst=1;value.sourceLast=-1;break;
        }
        Check(!ns::detail::ValidPreservedData(value,true),"signed opt-in keeps every non-domain validity restriction");
        Scalar(broken,-15,"invalid retained data releases to analytic scalar authoring");
    }
    for(unsigned component=0;component<6;++component){
        auto broken=lane;broken.preservedCameraIntervals[0].geometry[component]+=1;
        Scalar(broken,-15,"each stale retained geometry component invalidates its slice");
    }
    auto broken=lane;broken.preservedCameraIntervals[0].first=-19;
    Scalar(broken,-15,"mismatched retained start key invalidates its slice");
    broken=lane;broken.preservedCameraIntervals[0].last=-1;
    Scalar(broken,-15,"mismatched retained end key invalidates its slice");
    broken=lane;broken.track.discrete=true;
    Same(ns::Sample(broken,-15,true),{-2,0},"discrete authoring rejects retained Bezier sampling");
    broken=lane;broken.track.keys[1].interpolation=ac::Interpolation::Step;
    Same(ns::Sample(broken,-15,true),{-2,0},"step authoring invalidates retained slices");
    broken.track.keys[1].interpolation=ac::Interpolation::Linear;
    Same(ns::Sample(broken,-15,true),{0,8.0/20},"linear authoring invalidates retained slices");

    // A stale retained slice first falls back to usable raw native metadata.
    auto raw=MakeLane(0,20);raw.preservedCameraIntervals={Retained(raw)};
    raw.preservedCameraIntervals[0].geometry[0]+=1;
    Same(ns::Sample(raw,5),LegacyRaw(raw,5),"stale retained geometry falls back to matching raw native controls");
    raw.preservedCameraIntervals.clear();
    for(unsigned component=0;component<4;++component){
        auto edited=raw;edited.controlGeometry[1][component]+=1;
        Scalar(edited,5,"each stale raw geometry component falls back to analytic scalar authoring");
    }
    auto missing=raw;missing.nativeControls.resize(1);
    Scalar(missing,5,"missing incoming controls release scalar authoring");
    missing=raw;missing.controlGeometry.resize(1);
    Scalar(missing,5,"missing incoming geometry releases scalar authoring");
    auto wide=MakeLane(fl::minimum,fl::maximum);wide.controlGeometry.clear();
    Scalar(wide,double(fl::minimum)+(double(fl::maximum)-double(fl::minimum))*.25,
           "scalar authoring fallback also works across both signed frame limits");

    Lane empty;Same(ns::Sample(empty,0,true),{},"empty lane uses scalar empty sample");
    empty.track.keys.push_back(raw.track.keys[0]);
    Same(ns::Sample(empty,-.5,true),{-2,0},"singleton lane uses its exact scalar value");
    Same(ns::Sample(raw,nan),{-2,0},"nonfinite raw sample retains the scalar fallback behavior");
}

}
void NativeCameraSamplingChecks() {
    native_camera_sampling_checks::PositiveCompatibility();
    native_camera_sampling_checks::SignedTranslation();
    native_camera_sampling_checks::WideSpans();
    native_camera_sampling_checks::PreservedBoundariesAndCuts();
    native_camera_sampling_checks::RejectionAndFallback();
}
}
