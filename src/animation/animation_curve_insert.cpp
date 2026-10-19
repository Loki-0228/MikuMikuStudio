
#include "mikumikustudio/animation_curve.hpp"
#include <algorithm>
#include <cmath>

// Transactional subdivision of the stored scalar curve.
// Do not round handles to MMD's 0..127 integers: that would change its shape.
namespace mikumikustudio::animation_curve {
namespace ac = mikumikustudio::animation_curve;

struct Point { double x, y; };
inline Point Lerp(Point a, Point b, double t) {
    return {(1-t)*a.x+t*b.x,(1-t)*a.y+t*b.y};
}
inline void Freeze(ac::Key& key) {
    if(key.tangent==ac::Tangent::Automatic)key.tangent=ac::Tangent::Free;
}
InsertResult InsertPreservingShape(ac::Track& track,int frame,std::size_t& index,std::string& error) {
    if(frame<0 || track.keys.empty() || !ac::Validate(track,error)) {
        if(error.empty())error="no valid interval";
        return InsertResult::Rejected;
    }
    const auto at=std::lower_bound(track.keys.begin(),track.keys.end(),frame,
        [](const ac::Key& key,int f){return key.frame<f;});
    index=std::size_t(at-track.keys.begin());
    if(at!=track.keys.end() && at->frame==frame)return InsertResult::Existing;
    if(track.keys.size()>=ac::kMaximumKeys){error="key count limit";return InsertResult::Rejected;}
    ac::Track next=track;
    ac::Key key;key.frame=frame;key.tangent=ac::Tangent::Free;
    if(index==0){
        key.value=next.keys.front().value;
        auto& b=next.keys.front();b.interpolation=ac::Interpolation::Linear;
        b.inSlope=0;b.inWeight=1.0/3;Freeze(b);
    }else if(index==next.keys.size()){
        key.value=next.keys.back().value;key.interpolation=ac::Interpolation::Linear;
        auto& a=next.keys.back();a.outSlope=0;a.outWeight=1.0/3;Freeze(a);
    }else{
        auto& a=next.keys[index-1];auto& b=next.keys[index];
        key.interpolation=b.interpolation;
        const double span=double(b.frame)-a.frame,u=(double(frame)-a.frame)/span;
        if(track.discrete || b.interpolation!=ac::Interpolation::Bezier){
            const bool step=track.discrete||b.interpolation==ac::Interpolation::Step;
            key.value=step?a.value:(1-u)*a.value+u*b.value;
            key.inSlope=key.outSlope=step?0:(b.value-a.value)/span;
            // These weights do not affect a linear/step segment.
            a.outWeight=b.inWeight=1.0/3;
        }else{
            const Point p0{0,a.value},p1{a.outWeight,a.value+a.outSlope*a.outWeight*span},
                p2{1-b.inWeight,b.value-b.inSlope*b.inWeight*span},p3{1,b.value};
            double low=0,high=1;
            for(int i=0;i<64;++i){
                const double t=(low+high)*.5;
                const Point q0=Lerp(p0,p1,t),q1=Lerp(p1,p2,t),q2=Lerp(p2,p3,t);
                const double x=Lerp(Lerp(q0,q1,t),Lerp(q1,q2,t),t).x;
                if(x<u)low=t;else high=t;
            }
            const double t=(low+high)*.5;
            const Point q0=Lerp(p0,p1,t),q1=Lerp(p1,p2,t),q2=Lerp(p2,p3,t);
            const Point left=Lerp(q0,q1,t),right=Lerp(q1,q2,t),middle=Lerp(left,right,t);
            key.value=middle.y;
            key.inSlope=key.outSlope=(right.y-left.y)/((right.x-left.x)*span);
            a.outWeight=q0.x/u;
            key.inWeight=(u-left.x)/u;
            key.outWeight=(right.x-u)/(1-u);
            b.inWeight=(1-q2.x)/(1-u);
        }
        // Inserting a neighbour must not let automatic tangents reshape either
        // half or either outside interval on the next edit/recalculation.
        Freeze(a);Freeze(b);
    }
    next.keys.insert(next.keys.begin()+index,key);
    if(!ac::Validate(next,error))return InsertResult::Rejected;
    track=std::move(next);
    return InsertResult::Inserted;
}
}
