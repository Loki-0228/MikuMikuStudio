// Detached full-profile sampling checks. The including TU supplies Check and
// failAfter. These do not cover production author registration, zero-key or
// physics publication. No sampling::detail helper is used as an oracle.
#include "../src/animation/negative_key_sampling.hpp"
#include <cstring>
#include <limits>

namespace {
namespace negative_sampling_checks {
namespace ns = mikumikustudio::negative_frames::sampling;
namespace au = mikumikustudio::negative_frames::authoring;
namespace ac = mikumikustudio::animation_curve;
namespace bs = mikumikustudio::bone_scale;
namespace bk = mikumikustudio::bone_key_sampling;
namespace nc = mikumikustudio::native_camera_sampling;
namespace fl = mikumikustudio::frame_limits;
using Family = au::Family;
using Key = ns::Key;
using Values = ns::Values;
constexpr std::array<Family,7> families{Family::Bone,Family::Morph,Family::Camera,
    Family::Light,Family::Shadow,Family::Gravity,Family::Accessory};
constexpr std::array<unsigned,7> counts{9,1,11,6,2,6,12};
constexpr double pi = 3.14159265358979323846;

double F32(double value) { volatile float stored = float(value); return stored; }
bool Bits(const Values& a,const Values& b) {
    for(unsigned i=0;i<16;++i)if(std::memcmp(&a[i],&b[i],sizeof(double)))return false;
    return true;
}
bool Same(const ns::Result& a,const ns::Result& b) {
    return Bits(a.values,b.values)&&a.physicsPresent==b.physicsPresent&&a.physicsDisabled==b.physicsDisabled;
}
bool Near(const Values& a,const Values& b) {
    for(unsigned i=0;i<16;++i)if(!std::isfinite(a[i])||
        std::abs(a[i]-b[i])>2e-6*(1+std::abs(b[i])))return false;
    return true;
}
ns::Result Sentinel() {
    ns::Result r;for(unsigned i=0;i<16;++i)r.values[i]=-1000.125-i;
    r.values[5]=-0.0;r.physicsPresent=true;r.physicsDisabled=true;return r;
}
auto Match() { return [](const Key& k){return k.target==17;}; }
au::NativeData Controls(Family family) {
    au::NativeData n;
    if(family==Family::Bone) {
        n.boneControlsPresent=n.scaleControlsPresent=true;
        n.boneControls={8,27,90,114, 101,7,120,18, 117,100,42,23, 29,119,5,103};
        n.scaleControls={16,110,102,26};
    }
    if(family==Family::Camera) {
        n.cameraControlsPresent=true;
        n.cameraControls={7,19,33,61,93,120, 111,5,97,13,77,29,
                          119,103,89,67,41,9, 23,117,11,109,43,101};
    }
    return n;
}
bs::Matrix Scale(const Values& v) {
    bs::Matrix m;for(unsigned i=0;i<9;++i)m.v[i]=float(v[i+7]);return m;
}
void Scale(Values& v,const bs::Matrix& m) {
    for(unsigned i=0;i<9;++i)v[i+7]=m.v[i];
}
Key Seed(Family f,int frame,bool upper=false) {
    Key k;k.frame=frame;k.target=17;k.morph=f==Family::Morph;
    for(unsigned i=0;i<16;++i)k.values[i]=F32((upper?-2.71:1.13)*(i+1)+.03125*i);
    if(f==Family::Bone) {
        k.values[3]=upper?0:F32(.6);k.values[4]=upper?F32(-.8):0;
        k.values[5]=0;k.values[6]=upper?F32(-.6):F32(.8);
        // A rotating, non-diagonal basis makes polar interpolation observable.
        bs::Matrix m;m.v=upper?std::array<float,9>{0,2,0,-3,0,.25f,0,0,1.5f}:
            std::array<float,9>{2,.5f,0,.5f,3,.25f,0,.25f,1.5f};
        Scale(k.values,m);
    }
    if(f==Family::Gravity){k.values[4]=upper?2:9;k.values[5]=upper?1:0;}
    if(f==Family::Shadow){k.values[0]=upper?2:1;k.values[1]=upper?.85:.15;}
    return k;
}

// Independent transcription of the former ordinary-seek loops in
// negative_frames.cpp, including both float stores around quaternion norm.
Values OldSeek(Family family,const Key& a,const Key& b,double frame) {
    Values r=a.values;
    const double t=(frame-double(a.frame))/double(std::int64_t(b.frame)-a.frame);
    unsigned fields=0;
    switch(family) {
    case Family::Bone:case Family::Morph:fields=16;break;
    case Family::Camera:case Family::Accessory:fields=8;break;
    case Family::Light:fields=6;break;
    case Family::Gravity:fields=4;break;
    case Family::Shadow:r[1]=F32(a.values[1]+t*(b.values[1]-a.values[1]));break;
    }
    for(unsigned i=0;i<fields;++i)r[i]=F32(a.values[i]+t*(b.values[i]-a.values[i]));
    if(family==Family::Bone) {
        double dot=0;for(unsigned j=3;j!=7;++j)dot+=a.values[j]*b.values[j];
        double length2=0;
        for(unsigned j=3;j!=7;++j) {
            const double end=dot<0?-b.values[j]:b.values[j];
            r[j]=F32((1-t)*a.values[j]+t*end);length2+=r[j]*r[j];
        }
        if(length2>1e-20)for(unsigned j=3;j!=7;++j)r[j]=F32(r[j]/std::sqrt(length2));
    }
    return r;
}

// Native math is checked against existing positive-track algorithms, with
// explicit per-family field routing rather than the new sampler's helpers.
Values NativeSeek(Family f,const Key& a,const Key& b,double frame) {
    Values r=a.values;const auto& n=b.author->Native();
    const double span=double(std::int64_t(b.frame)-a.frame),offset=frame-a.frame,t=offset/span;
    const float time=float(offset)/float(span);
    if(f==Family::Bone) {
        mikumikustudio::mdl::BoneKey end{};
        for(unsigned i=0;i<16;++i)end.interpolation[i]=n.boneControls[i];
        float qa[4],qb[4],q[4];
        for(unsigned i=0;i<4;++i){qa[i]=float(a.values[i+3]);qb[i]=float(b.values[i+3]);}
        bk::SeekSlerp(qa,qb,bk::Ease(end,3,time),q);
        for(unsigned i=0;i<4;++i)r[3+i]=q[i];
        for(unsigned i=0;i<3;++i) {
            const float first=float(a.values[i]),delta=float(b.values[i])-first;
            r[i]=delta==0?first:F32(double(bk::Ease(end,i,time))*delta+first);
        }
        Scale(r,bs::Interpolate(Scale(a.values),Scale(b.values),bs::CurveTime(n.scaleControls,float(t))));
    } else if(f==Family::Morph) {
        const float delta=float(b.values[0])-float(a.values[0]);
        r[0]=F32(double(float(t))*delta+a.values[0]);
    } else if(f==Family::Camera&&span>1) {
        constexpr unsigned channel[8]={0,1,2,3,3,3,4,5};
        for(unsigned i=0;i<8;++i) {
            const auto c=channel[i];const std::array<double,4> curve{
                double(n.cameraControls[c]),double(n.cameraControls[6+c]),
                double(n.cameraControls[12+c]),double(n.cameraControls[18+c])};
            r[i]=F32(a.values[i]+double(nc::CurveTime(curve,float(t)))*(b.values[i]-a.values[i]));
        }
    } else if(f==Family::Light) {
        for(unsigned i=0;i<6;++i)r[i]=F32(a.values[i]+t*double(float(b.values[i])-float(a.values[i])));
    } else if(f==Family::Gravity) {
        for(unsigned i=0;i<4;++i)r[i]=float(a.values[i])+(float(b.values[i])-float(a.values[i]))*time;
        r[4]=a.values[4]+std::trunc(double(time*float(std::int64_t(b.values[4])-std::int64_t(a.values[4]))));
        r[5]=b.values[5];
    } else if(f==Family::Accessory) {
        for(unsigned i=6;i<8;++i)r[i]=float(a.values[i])+time*(float(b.values[i])-float(a.values[i]));
    }
    return r;
}

void LegacyAndNative() {
    bool roundingWitness=false;
    for(Family f:families)for(bool wide:{false,true}) {
        Key a=Seed(f,wide?fl::minimum:-101),b=Seed(f,wide?fl::maximum:-3,true);
        const double span=double(std::int64_t(b.frame)-a.frame);
        for(double ratio:{.00001,.17,1.0/3.0,.5,.81,.99999}) {
            const double frame=a.frame+span*ratio;ns::Result got=Sentinel();
            const auto old=OldSeek(f,a,b,frame);
            Check(ns::Interval(f,a,b,frame,got)&&Bits(got.values,old),"legacy seven-family ordinary seek matches every float store exactly");
            Check(!got.physicsPresent&&!got.physicsDisabled,"legacy sampling reports physics absent");
            if(f==Family::Bone) {
                const double t=(frame-a.frame)/span;double norm=0;std::array<double,4> q{};
                for(unsigned i=0;i<4;++i){q[i]=(1-t)*a.values[3+i]-t*b.values[3+i];norm+=q[i]*q[i];}
                for(unsigned i=0;i<4;++i)roundingWitness|=F32(q[i]/std::sqrt(norm))!=old[3+i];
            }
            b.author=au::Make(f,au::Mode::Native,Controls(f));
            const auto native=NativeSeek(f,a,b,frame);
            Check(ns::Interval(f,a,b,frame,got)&&Bits(got.values,native),"native seven-family sampling agrees with existing native algorithms");
            if(!wide&&ratio==.17) {
                if(f==Family::Bone) {
                    Check(got.values[0]!=old[0],"bone incoming ease differs from legacy linear position");
                    Check(got.values[3]!=old[3]||got.values[6]!=old[6],"SeekSlerp differs from legacy normalized linear rotation");
                    bool differs=false;for(unsigned i=7;i<16;++i)differs|=got.values[i]!=old[i];
                    Check(differs,"native scale polar decomposition differs from elementwise legacy scale");
                }
                if(f==Family::Camera)Check(got.values[0]!=old[0]&&got.values[6]!=old[6],"camera 4x6 controls route position and distance independently");
                if(f==Family::Shadow)Check(got.values[1]==a.values[1]&&got.values[1]!=old[1],"native shadow distance steps while legacy distance interpolates");
                if(f==Family::Accessory)Check(got.values[0]==a.values[0]&&got.values[3]==a.values[3]&&got.values[0]!=old[0],"native accessory transforms step while scale and opacity interpolate");
                if(f==Family::Gravity)Check(got.values[4]!=old[4]&&got.values[5]==b.values[5],"native gravity interpolates truncated noise and selects the upper mode");
            }
            // Incoming semantics depend only on the upper key.
            a.author=b.author;b.author.reset();
            Check(ns::Interval(f,a,b,frame,got)&&Bits(got.values,old),"native lower author cannot replace legacy upper semantics");
            a.author.reset();
        }
        for(bool native:{false,true}) {
            if(native){a.author=au::Make(f,au::Mode::Native,Controls(f));b.author=a.author;}
            ns::Result got;
            Check(ns::Point(f,a,got)&&Bits(got.values,a.values),"legacy/native point preserves stored endpoint without rounding");
            Check(ns::Interval(f,a,b,a.frame,got)&&Bits(got.values,a.values),"interval exact lower endpoint");
            Check(ns::Interval(f,a,b,b.frame,got)&&Bits(got.values,b.values),"interval exact upper endpoint");
            if(!wide) {
                Check(ns::Interval(f,a,b,a.frame-10.,got)&&Bits(got.values,a.values),"interval clamps below first");
                Check(ns::Interval(f,a,b,b.frame+10.,got)&&Bits(got.values,b.values),"interval clamps above last");
            }
        }
    }
    Check(roundingWitness,"legacy quaternion fixture distinguishes normalization before versus after float stores");
    Key a=Seed(Family::Bone,-2),b=Seed(Family::Bone,0,true);ns::Result got;
    for(unsigned i=3;i<7;++i)a.values[i]=b.values[i]=0;
    Check(ns::Interval(Family::Bone,a,b,-1,got)&&Bits(got.values,OldSeek(Family::Bone,a,b,-1)),"legacy zero quaternion retains the old tiny-norm branch");
    a=Seed(Family::Camera,-2);b=Seed(Family::Camera,-1,true);
    b.author=au::Make(Family::Camera,au::Mode::Native,Controls(Family::Camera));
    Check(ns::Interval(Family::Camera,a,b,-1.25,got)&&Bits(got.values,a.values),"native camera one-frame interval holds all fields");
    b.author.reset();Check(ns::Interval(Family::Camera,a,b,-1.25,got)&&got.values[0]!=a.values[0],"legacy camera one-frame interval still interpolates");
    a=Seed(Family::Gravity,-4);b=Seed(Family::Gravity,0,true);b.author=au::Make(Family::Gravity,au::Mode::Native,{});
    Check(ns::Interval(Family::Gravity,a,b,-3,got)&&got.values[4]==8&&got.values[5]==1,"negative noise delta truncates -1.75 toward zero before adding 9");
    a.values[4]=2;b.values[4]=9;
    Check(ns::Interval(Family::Gravity,a,b,-3,got)&&got.values[4]==3,"positive noise delta truncates 1.75 before adding 2");
}

void MorphRounding() {
    Key a=Seed(Family::Morph,-4),b=Seed(Family::Morph,0,true);ns::Result r;
    b.author=au::Make(Family::Morph,au::Mode::Native,{});
    a.values[0]=-1;b.values[0]=16777216;
    Check(ns::Interval(Family::Morph,a,b,-2,r)&&r.values[0]==8388607,"native morph rounds endpoint delta before multiply");
    b.author.reset();Check(ns::Interval(Family::Morph,a,b,-2,r)&&r.values[0]==8388607.5,"legacy morph retains the double endpoint delta");
    a.frame=-100;a.values[0]=-.5;b.values[0]=.5;const double frame=-50+1e-6;
    b.author=au::Make(Family::Morph,au::Mode::Native,{});
    Check(ns::Interval(Family::Morph,a,b,frame,r)&&r.values[0]==0,"native morph rounds time before double multiply/add");
    b.author.reset();Check(ns::Interval(Family::Morph,a,b,frame,r)&&r.values[0]>0,"legacy morph retains sub-float time information");
}

bool Discrete(Family f,unsigned i) {
    return (f==Family::Camera&&i>=8)||(f==Family::Shadow&&i==0)||
        (f==Family::Gravity&&i>=4)||(f==Family::Accessory&&i>=8);
}
au::Track Curves(Family f) {
    au::Track t;t.family=f;unsigned count=0;
    for(unsigned i=0;i<families.size();++i)if(f==families[i])count=counts[i];
    for(unsigned i=0;i<count;++i) {
        au::Lane lane;lane.index=i;lane.track.discrete=Discrete(f,i);
        for(unsigned j=0;j<3;++j) {
            ac::Key k;k.frame=j==0?-110-int(i)*3:j==1?-41+int(i)*2:37+int(i)*4;
            k.value=lane.track.discrete?double(j%2):.25+i*.125+j*.4;
            k.inWeight=.23;k.outWeight=.31;k.inSlope=-.003*(i+1);k.outSlope=.007*(i+1);
            k.tangent=ac::Tangent::Free;k.interpolation=lane.track.discrete?ac::Interpolation::Step:ac::Interpolation::Bezier;
            if(f==Family::Bone&&i>=6){k.value=1.5+.2*i+.25*j;k.inSlope=-.001;k.outSlope=.002;}
            if(f==Family::Morph){k.value=j==0?-2.25:j==1?1.75:3.5;k.outSlope=.03;k.inSlope=-.02;}
            if((f==Family::Camera||f==Family::Accessory)&&i>=3&&i<6)k.value=90*(int(j)-1)+30*i;
            if(f==Family::Camera&&i==7)k.value=40+20*j;
            lane.track.keys.push_back(k);
        }
        t.lanes.push_back(std::move(lane));
    }
    return t;
}
au::Ptr Author(const au::Track& t,const au::NativeData& n) {
    return au::Make(t.family,au::Mode::Authored,n,au::Freeze(t));
}
double Bound(Family f,unsigned i,double value) {
    // Independently tabulated from curve_host's public field specifications.
    double lo=-1e8,hi=1e8;
    if(f==Family::Camera) {
        constexpr double lows[4]={1,0,-1,0},highs[4]={179,1,254,1000000};
        if(i>=7){lo=lows[i-7];hi=highs[i-7];}
    }
    if(f==Family::Light&&i>=3){lo=0;hi=1;}
    if(f==Family::Shadow){lo=0;hi=i==0?2:1;}
    if(f==Family::Gravity) {
        if(i==0){lo=0;hi=100000;}if(i==4){lo=0;hi=1000000;}if(i==5){lo=0;hi=1;}
    }
    if(f==Family::Accessory&&i>=6) {
        constexpr double lows[6]={.000001,0,0,0,-1,0},highs[6]={1e6,1,1,1,254,1000000};
        lo=lows[i-6];hi=highs[i-6];
    }
    return (std::max)(lo,(std::min)(hi,value));
}
using Quaternion = std::array<double,4>;
Quaternion Product(const Quaternion& a,const Quaternion& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Values AuthoredExpected(const au::Track& t,double frame,Values base) {
    std::array<double,12> v{};
    for(const auto& lane:t.lanes)v[lane.index]=lane.nativeCameraTiming?
        nc::Sample(lane,frame,true).value:ac::Evaluate(lane.track,frame).value;
    if(t.family==Family::Bone) {
        for(unsigned i=0;i<3;++i)base[i]=F32(v[i]);
        const double x=v[3]*pi/360,y=v[4]*pi/360,z=v[5]*pi/360;
        const auto q=Product(Product({0,std::sin(y),0,std::cos(y)},
            {std::sin(x),0,0,std::cos(x)}),{0,0,std::sin(z),std::cos(z)});
        for(unsigned i=0;i<4;++i)base[3+i]=F32(q[i]);
        for(unsigned row=0;row<3;++row) {
            const unsigned at=7+3*row;
            const double norm=std::sqrt(base[at]*base[at]+base[at+1]*base[at+1]+base[at+2]*base[at+2]);
            for(unsigned col=0;col<3;++col)base[at+col]=F32(base[at+col]*v[6+row]/norm);
        }
    } else if(t.family==Family::Morph)base[0]=F32(v[0]);
    else for(unsigned i=0;i<t.lanes.size();++i) {
        double value=Bound(t.family,i,v[i]);
        if((t.family==Family::Camera||t.family==Family::Accessory)&&i>=3&&i<6)value*=pi/180;
        base[i]=F32(value);
    }
    return base;
}

void AuthoredFamilies() {
    for(Family f:families) {
        const auto curves=Curves(f);Key a=Seed(f,-160),b=Seed(f,100,true);
        b.author=Author(curves,Controls(f));ns::Result got;
        for(double frame:{-150.,-93.25,-41.,-17.5,0.,80.}) {
            const auto base=f==Family::Bone?NativeSeek(f,a,b,frame):a.values;
            Check(ns::Interval(f,a,b,frame,got)&&Near(got.values,AuthoredExpected(curves,frame,base)),"authored seven-family sparse nonlinear lanes use their own signed times");
            Key point=b;point.frame=int(frame);
            Check(ns::Point(f,point,got)&&Near(got.values,AuthoredExpected(curves,point.frame,point.values)),"authored point evaluates lanes at the key frame including curve clamps");
        }
        const auto& lane=curves.lanes[0];const double frame=-73.25;
        const double linear=lane.track.keys[0].value+(frame-lane.track.keys[0].frame)/
            double(lane.track.keys[1].frame-lane.track.keys[0].frame)*(lane.track.keys[1].value-lane.track.keys[0].value);
        if(!lane.track.discrete)Check(std::abs(ac::Evaluate(lane.track,frame).value-linear)>1e-5,"authored fixture is observably nonlinear");
        for(const auto& l:curves.lanes)for(const auto& k:l.track.keys)Check(k.frame!=0,"sparse authored fixtures need no lane zero anchor");
        // Widened key span must not overflow even when curves clamp internally.
        a.frame=fl::minimum;b.frame=fl::maximum;
        const auto base=f==Family::Bone?NativeSeek(f,a,b,-17.5):a.values;
        Check(ns::Interval(f,a,b,-17.5,got)&&Near(got.values,AuthoredExpected(curves,-17.5,base)),"authored sampling accepts the complete signed key span");
        auto wide=curves;
        for(auto& lane:wide.lanes) {
            auto first=lane.track.keys.front(),last=lane.track.keys.back();
            first.frame=fl::minimum;last.frame=fl::maximum;
            first.outSlope=1e-10;last.inSlope=-1e-10;lane.track.keys={first,last};
        }
        b.author=Author(wide,Controls(f));
        Check(ns::Interval(f,a,b,-123456789.25,got)&&Near(got.values,AuthoredExpected(wide,-123456789.25,
            f==Family::Bone?NativeSeek(f,a,b,-123456789.25):a.values)),"authored lane Bezier inversion also spans the complete signed domain");
    }
    auto curves=Curves(Family::Morph);Key p=Seed(Family::Morph,-110);p.author=Author(curves,{});ns::Result r;
    Check(ns::Point(Family::Morph,p,r)&&r.values[0]==-2.25,"authored morph negative weights are not clamped to zero");
    p.frame=37;Check(ns::Point(Family::Morph,p,r)&&r.values[0]==3.5,"authored morph weights greater than one are retained");
}

void BoneEulerAndBasis() {
    auto curves=Curves(Family::Bone);
    for(unsigned lane=0;lane<9;++lane) {
        ac::Key a,b;a.frame=-100;b.frame=-20;
        a.interpolation=b.interpolation=ac::Interpolation::Linear;
        a.tangent=b.tangent=ac::Tangent::Free;
        a.value=b.value=lane>=6?double(lane-4):0;
        if(lane==3)b.value=720;
        curves.lanes[lane].track.keys={a,b};
    }
    Key a=Seed(Family::Bone,-100),b=Seed(Family::Bone,-20,true);
    b.author=Author(curves,Controls(Family::Bone));ns::Result r;
    Check(ns::Interval(Family::Bone,a,b,-90,r),"authored bone interval includes required native controls");
    Check(std::abs(r.values[3]-std::sqrt(.5))<1e-6&&std::abs(r.values[6]-std::sqrt(.5))<1e-6,
        "multi-turn authored Euler reaches 90 degrees instead of shortest quaternion path");
    const auto basis=NativeSeek(Family::Bone,a,b,-90);
    for(unsigned row=0;row<3;++row) {
        const unsigned at=7+row*3;double length2=0;
        for(unsigned col=0;col<3;++col)length2+=r.values[at+col]*r.values[at+col];
        Check(std::abs(std::sqrt(length2)-(row+2))<2e-6,"authored scale replaces the sampled row length");
        for(unsigned col=1;col<3;++col)Check(std::abs(r.values[at]*basis[at+col]-r.values[at+col]*basis[at])<2e-6,
            "authored scale retains sampled non-diagonal basis direction");
    }
    Check(std::abs(r.values[8])+std::abs(r.values[10])+std::abs(r.values[12])>.1,
        "authored scale fixture cannot pass with diagonal reconstruction");
    Check(ns::Interval(Family::Bone,a,b,-60,r)&&std::abs(r.values[3])<1e-6&&r.values[6]<-.999999,
        "360-degree authored Euler retains the quaternion sign of a full turn");
    // An Authored Bone can be represented without controls, but such an
    // interval is incomplete and must not silently use legacy interpolation.
    b.author=Author(curves,{});r=Sentinel();const auto saved=r;
    Check(!ns::Interval(Family::Bone,a,b,-90,r)&&Same(r,saved),"authored bone missing native controls rejects without publishing output");
}

void CameraUnitsAndTiming() {
    for(Family f:{Family::Camera,Family::Accessory}) {
        auto curves=Curves(f);
        for(unsigned axis=0;axis<3;++axis)for(auto& k:curves.lanes[3+axis].track.keys) {
            k.value=(axis+1)*90.;k.inSlope=k.outSlope=0;
        }
        Key a=Seed(f,-160),b=Seed(f,100,true);b.author=Author(curves,Controls(f));ns::Result r;
        Check(ns::Interval(f,a,b,-50,r),"authored rotation units sample successfully");
        for(unsigned axis=0;axis<3;++axis)Check(r.values[3+axis]==F32((axis+1)*90.*(pi/180)),"authored degrees convert to radians exactly once");
        if(f==Family::Camera) {
            b.author=au::Make(f,au::Mode::Native,Controls(f));
            for(unsigned axis=0;axis<3;++axis)a.values[3+axis]=b.values[3+axis]=(axis+1)*.375;
            Check(ns::Interval(f,a,b,-50,r),"native camera radian fixture samples");
            for(unsigned axis=0;axis<3;++axis)Check(r.values[3+axis]==(axis+1)*.375,"native camera radians bypass degree conversion");
            Check(r.values[7]==NativeSeek(f,a,b,-50)[7],"camera FOV uses its scalar lane without radian conversion");
        }
    }
    auto curves=Curves(Family::Camera);
    for(unsigned i=0;i<8;++i) {
        auto& lane=curves.lanes[i];lane.nativeCameraTiming=true;
        ac::Key a,b;a.frame=-3;b.frame=-2;a.value=10+i;b.value=70+i;
        a.tangent=b.tangent=ac::Tangent::Free;a.outWeight=.25;b.inWeight=.25;
        a.outSlope=17;b.inSlope=-9;lane.track.keys={a,b};
        lane.nativeControls={{20,20,107,107},{13,117,104,7}};
        lane.controlGeometry={{0,0,0,0},{a.outWeight,a.outSlope,b.inWeight,b.inSlope}};
    }
    Key a=Seed(Family::Camera,-4),b=Seed(Family::Camera,0,true);ns::Result r;
    b.author=Author(curves,Controls(Family::Camera));
    Check(ns::Interval(Family::Camera,a,b,-2.5,r),"authored camera native timing samples signed one-frame lane");
    Check(r.values[0]==10&&r.values[3]==F32(13*(pi/180)),"authored native camera hold precedes degree-to-radian conversion");
    Check(ns::Interval(Family::Camera,a,b,-2,r)&&r.values[0]==70,"authored native camera exact upper key breaks hold");
    curves.lanes[0].nativeCameraTiming=false;b.author=Author(curves,Controls(Family::Camera));
    Check(ns::Interval(Family::Camera,a,b,-2.5,r)&&r.values[0]!=10,"scalar-authored camera uses free curve instead of native one-frame hold");
    // Signed retained intervals use their source time and geometry as well.
    auto& lane=curves.lanes[0];lane.nativeCameraTiming=true;
    const auto& first=lane.track.keys.front();const auto& last=lane.track.keys.back();
    mikumikustudio::curve_editor::NativeCameraInterval interval;
    interval.first=first.frame;interval.last=last.frame;interval.sourceFirst=-40;interval.sourceLast=-10;
    interval.sampleFirst=-35;interval.sampleLast=-15;interval.sourceValueFirst=10;interval.sourceValueLast=70;
    interval.controls={13,117,104,7};interval.transportTiming=true;
    interval.geometry={first.value,first.outWeight,first.outSlope,last.value,last.inWeight,last.inSlope};
    lane.preservedCameraIntervals={interval};b.author=Author(curves,Controls(Family::Camera));
    const double expected=10+60*double(nc::CurveTime(interval.controls,.5f,true));
    Check(ns::Interval(Family::Camera,a,b,-2.5,r)&&r.values[0]==F32(expected),"authored camera routes signed retained source time before scalar fallback");
}

void TrackStepAndPhysics() {
    for(Family f:families) {
        Key first=Seed(f,-80),middle=Seed(f,-40,true),last=Seed(f,-20),zero=Seed(f,0,true);
        middle.values[0]+=3;last.values[0]+=7;Key ignored=Seed(f,-60);ignored.target=18;
        ignored.values[15]=(std::numeric_limits<double>::quiet_NaN)();
        std::array<Key,4> rows{last,ignored,first,middle};
        const std::array<const Key*,4> refs{&rows[0],&rows[1],&rows[2],&rows[3]};
        for(double frame:{double(fl::minimum),-80.,-71.25,-40.,-27.5,-20.,-.25}) {
            Values expected;
            if(frame<=-80)expected=first.values;
            else if(frame==-40)expected=middle.values;
            else if(frame==-20)expected=last.values;
            else if(frame<-40)expected=OldSeek(f,first,middle,frame);
            else if(frame<-20)expected=OldSeek(f,middle,last,frame);
            else expected=OldSeek(f,last,zero,frame);
            ns::Result r;
            Check(ns::Track(rows,f,frame,zero,r,Match())&&Bits(r.values,expected),"unsorted filtered track handles clamps exact keys gaps and explicit zero endpoint");
            Check(ns::Track(refs,f,frame,zero,r,Match())&&Bits(r.values,expected),"pointer-view track agrees with independent bracket oracle");
        }
        ns::Result r;const std::array<Key,0> empty{};
        Check(ns::Track(empty,f,-1,zero,r,Match())&&Bits(r.values,zero.values),"empty track reads the supplied zero baseline");
        Check(ns::Track(rows,f,-1,zero,r,[](const Key&){return false;})&&Bits(r.values,zero.values),"no matching channel reads the supplied zero baseline");
    }
    std::array<Key,3> rows{};rows[0].frame=-10;rows[1].frame=-90;rows[2].frame=-50;
    for(unsigned i=0;i<3;++i){rows[i].target=17;rows[i].values.fill(1000+i);rows[i].values[0]=double(INT32_MAX)-i;rows[i].values[1]=double(INT32_MIN)+i;}
    Values zero{};zero.fill(-19);
    const std::array<const Key*,3> refs{&rows[0],&rows[1],&rows[2]};
    for(double frame:{double(fl::minimum),-90.,-75.5,-50.,-49.5,-10.,-.5,double(fl::maximum)}) {
        const auto& expected=frame<-50?rows[1].values:frame<-10?rows[2].values:rows[0].values;
        Check(Bits(ns::Step(rows,frame,zero,Match()),expected)&&Bits(ns::Step(refs,frame,zero,Match()),expected),
            "discrete display IK selector rows retain exact integer bits and step at exact keys");
    }
    Check(Bits(ns::Step(rows,-30,zero,[](const Key&){return false;}),zero),"step with no matching rows returns explicit zero values");
    const std::array<Key,0> empty{};Check(Bits(ns::Step(empty,-30,zero,Match()),zero),"empty step view returns explicit zero values");
    for(bool authored:{false,true})for(int lower=0;lower<3;++lower)for(int upper=0;upper<3;++upper) {
        Key a=Seed(Family::Bone,-20),b=Seed(Family::Bone,0,true);
        auto install=[&](Key& k,int state){auto n=Controls(Family::Bone);n.physicsPresent=state!=0;n.physicsDisabled=state==2;
            k.author=authored?Author(Curves(Family::Bone),n):au::Make(Family::Bone,au::Mode::Native,n);};
        install(a,lower);install(b,upper);ns::Result r=Sentinel();
        Check(ns::Point(Family::Bone,a,r)&&r.physicsPresent==(lower!=0)&&r.physicsDisabled==(lower==2),"point distinguishes absent explicit false and true physics");
        Check(ns::Interval(Family::Bone,a,b,-10,r)&&r.physicsPresent==(lower!=0)&&r.physicsDisabled==(lower==2),"interval physics steps from lower independently of upper controls");
        Check(ns::Interval(Family::Bone,a,b,0,r)&&r.physicsPresent==(upper!=0)&&r.physicsDisabled==(upper==2),"exact upper physics uses upper presence and value");
        const std::array<Key,1> track{a};
        Check(ns::Track(track,Family::Bone,-10,b,r,Match())&&r.physicsPresent==(lower!=0)&&r.physicsDisabled==(lower==2),"track preserves lower physics while sampling toward zero");
        a.author.reset();r=Sentinel();
        Check(ns::Interval(Family::Bone,a,b,-10,r)&&!r.physicsPresent&&!r.physicsDisabled,"legacy lower physics stays absent even with native upper author");
    }
}

template<class Operation> void RejectUnchanged(Operation operation,const char* message) {
    auto r=Sentinel();const auto saved=r;
    Check(!operation(r)&&Same(r,saved),message);
}
void Failures() {
    const double nan=(std::numeric_limits<double>::quiet_NaN)(),inf=(std::numeric_limits<double>::infinity)();
    for(Family f:families) {
        Key a=Seed(f,-10),b=Seed(f,0,true);
        for(double frame:{nan,inf,-inf,double(fl::minimum)-1,double(fl::maximum)+1})
            RejectUnchanged([&](ns::Result& r){return ns::Interval(f,a,b,frame,r);},"invalid interval time leaves all output fields unchanged");
        for(unsigned slot:{0u,7u,15u})for(double value:{nan,inf,-inf,double((std::numeric_limits<float>::max)())*2}) {
            Key bad=a;bad.values[slot]=value;
            RejectUnchanged([&](ns::Result& r){return ns::Point(f,bad,r);},"point rejects nonfinite or unrepresentable values atomically");
            RejectUnchanged([&](ns::Result& r){return ns::Interval(f,bad,b,-5,r);},"invalid lower endpoint cannot change output");
            bad.frame=0;RejectUnchanged([&](ns::Result& r){return ns::Interval(f,a,bad,-5,r);},"invalid upper endpoint cannot change output");
        }
        Key bad=b;bad.frame=a.frame;
        RejectUnchanged([&](ns::Result& r){return ns::Interval(f,a,bad,-5,r);},"zero interval span is rejected atomically");
        bad.frame=a.frame-1;RejectUnchanged([&](ns::Result& r){return ns::Interval(f,a,bad,-5,r);},"reversed interval span is rejected atomically");
        for(int frame:{fl::minimum-1,fl::maximum+1}) {
            bad=a;bad.frame=frame;RejectUnchanged([&](ns::Result& r){return ns::Point(f,bad,r);},"point rejects reserved out-of-domain frame values");
        }
        const Family other=f==Family::Morph?Family::Light:Family::Morph;
        bad=a;bad.author=au::Make(other,au::Mode::Native,{});
        RejectUnchanged([&](ns::Result& r){return ns::Point(f,bad,r);},"point rejects an author from another family");
        RejectUnchanged([&](ns::Result& r){return ns::Interval(f,bad,b,-5,r);},"interval rejects an incompatible author");
        const std::array<Key,1> rows{a};
        for(double frame:{0.,1.,nan,inf,double(fl::minimum)-1})
            RejectUnchanged([&](ns::Result& r){return ns::Track(rows,f,frame,b,r,Match());},"track rejects nonnegative or invalid time atomically");
        b.frame=1;RejectUnchanged([&](ns::Result& r){return ns::Track(rows,f,-1,b,r,Match());},"track rejects a nonzero zero-anchor frame");
    }
    Key a=Seed(Family::Morph,-10),b=Seed(Family::Morph,0);
    RejectUnchanged([&](ns::Result& r){return ns::Point(Family(99),a,r);},"unknown family point is rejected");
    RejectUnchanged([&](ns::Result& r){return ns::Interval(Family(99),a,b,-5,r);},"unknown family interval is rejected");
    a=Seed(Family::Gravity,-4);b=Seed(Family::Gravity,0,true);b.author=au::Make(Family::Gravity,au::Mode::Native,{});
    const std::array<std::array<double,2>,6> badNoise{{{.5,2},{2,.5},{double(INT32_MIN)-1,0},
        {0,double(INT32_MAX)+1},{double(INT32_MIN),double(INT32_MAX)},{double(INT32_MAX),double(INT32_MIN)}}};
    for(const auto& pair:badNoise) {
        a.values[4]=pair[0];b.values[4]=pair[1];
        RejectUnchanged([&](ns::Result& r){return ns::Interval(Family::Gravity,a,b,-2,r);},"native gravity rejects fractional noise or overflowing integer delta atomically");
    }
    a.values[4]=0;b.values[4]=INT32_MAX;
    RejectUnchanged([&](ns::Result& r){return ns::Interval(Family::Gravity,a,b,-1e-9,r);},"gravity rounded scaled delta must remain representable as int32");
}

void AuthoredOverflow() {
    for(Family f:{Family::Morph,Family::Bone})for(unsigned lane:{0u,6u}) {
        if(f==Family::Morph&&lane!=0)continue;
        for(double sign:{-1.,1.}) {
            auto curves=Curves(f);auto& target=curves.lanes[lane].track;
            ac::Key a,b;a.frame=-3;b.frame=-1;a.value=b.value=0;
            a.outWeight=b.inWeight=.25;a.outSlope=sign*1e40;b.inSlope=-sign*1e40;
            a.tangent=b.tangent=ac::Tangent::Free;target.keys={a,b};
            const double expected=sign*3.75e39;
            Check(std::abs(ac::Evaluate(target,-2).value/expected-1)<1e-12,"overflow fixture is finite signed 3.75e39 before any float conversion");
            Key first=Seed(f,-3),last=Seed(f,-1,true);last.author=Author(curves,Controls(f));
            RejectUnchanged([&](ns::Result& r){return ns::Interval(f,first,last,-2,r);},"authored morph bone position or scale overflow rejects both signs without changing output");
            Key point=last;point.frame=-2;
            RejectUnchanged([&](ns::Result& r){return ns::Point(f,point,r);},"authored point overflow cannot publish a partial result");
            const std::array<Key,2> rows{first,last};Key zero=Seed(f,0);
            RejectUnchanged([&](ns::Result& r){return ns::Track(rows,f,-2,zero,r,Match());},"authored track overflow cannot publish a partial result");
        }
    }
}

void AllocationFree() {
    for(Family f:families)for(unsigned mode=0;mode<3;++mode) {
        Key a=Seed(f,-100),b=Seed(f,-20,true),zero=Seed(f,0);const auto curves=Curves(f);
        if(mode==1)b.author=au::Make(f,au::Mode::Native,Controls(f));
        if(mode==2)b.author=Author(curves,Controls(f));
        const auto expected=mode==0?OldSeek(f,a,b,-50):mode==1?NativeSeek(f,a,b,-50):
            AuthoredExpected(curves,-50,f==Family::Bone?NativeSeek(f,a,b,-50):a.values);
        const auto pointExpected=mode==2?AuthoredExpected(curves,b.frame,b.values):b.values;
        const std::array<Key,2> rows{b,a};const std::array<const Key*,2> refs{&rows[0],&rows[1]};
        ns::Result point,interval,track,pointers;Values stepped;bool p=false,i=false,t=false,v=false;
        failAfter=0;
        p=ns::Point(f,b,point);i=ns::Interval(f,a,b,-50,interval);
        t=ns::Track(rows,f,-50,zero,track,Match());v=ns::Track(refs,f,-50,zero,pointers,Match());
        stepped=ns::Step(refs,-50,zero.values,Match());
        const long remaining=failAfter;failAfter=-1;
        Check(remaining==0&&p&&i&&t&&v,"Point Interval Track Step allocate nothing with failAfter zero for all seven families and modes");
        Check(Near(point.values,pointExpected)&&Near(interval.values,expected)&&Near(track.values,expected)&&Near(pointers.values,expected)&&Bits(stepped,a.values),
            "allocation injection leaves successful sample values intact");
        auto bad=Sentinel();const auto saved=bad;bool rejected=false;
        failAfter=0;rejected=!ns::Interval(f,a,b,(std::numeric_limits<double>::infinity)(),bad);
        const long failureRemaining=failAfter;failAfter=-1;
        Check(rejected&&failureRemaining==0&&Same(bad,saved),"failed sampling also allocates nothing and preserves output");
    }
}

} // namespace negative_sampling_checks
void NativeNegativeSamplingChecks() {
    using namespace negative_sampling_checks;
    Check(failAfter==-1,"negative sampling starts outside allocation injection");
    LegacyAndNative();MorphRounding();AuthoredFamilies();BoneEulerAndBasis();
    CameraUnitsAndTiming();TrackStepAndPhysics();Failures();AuthoredOverflow();AllocationFree();
    Check(failAfter==-1,"negative sampling restores allocation injection");
}
} // namespace
