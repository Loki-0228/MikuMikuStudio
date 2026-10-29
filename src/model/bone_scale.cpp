#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/bone_curve.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace mikumikustudio {
void ViewWorldAxis(MMDApp*, int, float[3]);
namespace bone_scale {
namespace {
struct State {
    Snapshot current;
    std::array<Snapshot, 30> undo, redo;
    std::array<bool, 30> undoValid{}, redoValid{};
    std::vector<Transform> rigid;
    bool applied = false;
    std::uint64_t revision = 0;
};
std::unordered_map<const unsigned char*, State> states;
struct ClipboardState { Values pose, keys; Curves poseCurves, keyCurves; };
std::unordered_map<const MMDApp*, ClipboardState> clipboards;
thread_local Matrix registration;
thread_local Curve registrationCurve = LinearCurve();
// Set while a motion import replaces keyframes (DefaultCurveScope): a linear
// payload then means "the default linear curve", not "leave the old one".
thread_local bool registrationDefaultCurve = false;
// True when the payload carries an explicit curve (clipboard paste, import):
// a linear curve then also overwrites the target's older curve.
thread_local bool registrationCurveExplicit = false;
double Determinant(const Matrix& a) {
    const auto& m = a.v;
    return double(m[0])*(double(m[4])*m[8]-double(m[5])*m[7])
         - double(m[1])*(double(m[3])*m[8]-double(m[5])*m[6])
         + double(m[2])*(double(m[3])*m[7]-double(m[4])*m[6]);
}
Matrix Transpose(const Matrix& a) {
    Matrix b;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) b.v[i*3+j]=a.v[j*3+i];
    return b;
}
void Store(Values& values, int index, const Matrix& value) {
    if (Identity(value)) values.erase(index); else values[index]=value;
}
Matrix Lookup(const Values& values, int index) {
    const auto it=values.find(index);
    return it==values.end()?Matrix{}:it->second;
}
// MMS addition: curve entries follow the same "default is absence" rule, so a
// linear curve and no curve are the same state.
void StoreCurve(Curves& values, int index, const Curve& value) {
    if (Linear(value)) values.erase(index); else values[index]=value;
}
Curve LookupCurve(const Curves& values, int index) {
    const auto it=values.find(index);
    return it==values.end()?LinearCurve():it->second;
}
// Polar rotation, with norm balancing to handle very unequal axis lengths.
Matrix Rotation(const Matrix& a) {
    Matrix r=a;
    for (int iteration=0;iteration<32;++iteration) {
        Matrix inverse;
        if (!Inverse(r,inverse)) return {};
        inverse=Transpose(inverse);
        double n=0, ni=0;
        for (int i=0;i<9;++i) { n+=double(r.v[i])*r.v[i]; ni+=double(inverse.v[i])*inverse.v[i]; }
        const double balance=std::sqrt(std::sqrt(ni/n));
        double error=0;
        for (int i=0;i<9;++i) {
            const float next=float(0.5*(balance*r.v[i]+inverse.v[i]/balance));
            error=std::max(error,std::abs(double(next)-r.v[i])); r.v[i]=next;
        }
        if (error<1.e-6) break;
    }
    return r;
}
std::array<double,4> Quaternion(const Matrix& row) {
    const Matrix a=Transpose(row);
    const auto& m=a.v;
    std::array<double,4> q{};
    const double trace=m[0]+m[4]+m[8];
    if (trace>0) {
        const double s=2*std::sqrt(1+trace);
        q={(m[7]-m[5])/s,(m[2]-m[6])/s,(m[3]-m[1])/s,s/4};
    } else {
        int i=m[4]>m[0]?1:0; if (m[8]>m[i*3+i]) i=2;
        const int j=(i+1)%3,k=(i+2)%3;
        const double s=2*std::sqrt(std::max(0.0,1.0+m[i*3+i]-m[j*3+j]-m[k*3+k]));
        if (s<1.e-12) return {0,0,0,1};
        q[i]=s/4; q[j]=(m[i*3+j]+m[j*3+i])/s; q[k]=(m[i*3+k]+m[k*3+i])/s;
        q[3]=(m[k*3+j]-m[j*3+k])/s;
    }
    return q;
}
Matrix FromQuaternion(std::array<double,4> q) {
    double n=0; for (double v:q) n+=v*v;
    if (n<1.e-20) return {};
    n=1/std::sqrt(n); for (double& v:q) v*=n;
    const double x=q[0],y=q[1],z=q[2],w=q[3];
    return {{float(1-2*(y*y+z*z)),float(2*(x*y+z*w)),float(2*(x*z-y*w)),
             float(2*(x*y-z*w)),float(1-2*(x*x+z*z)),float(2*(y*z+x*w)),
             float(2*(x*z+y*w)),float(2*(y*z-x*w)),float(1-2*(x*x+y*y))}};
}
Transform Product(const Transform& a,const Transform& b) {
    Transform c{};
    for (int i=0;i<4;++i) for (int j=0;j<4;++j) {
        double x=0; for (int k=0;k<4;++k) x+=double(a[i*4+k])*b[k*4+j];
        c[i*4+j]=float(x);
    }
    return c;
}
Transform Affine(const Matrix& a) {
    Transform t{}; t[15]=1;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) t[i*4+j]=a.v[i*3+j];
    return t;
}
Transform InverseTransform(const Transform& t) {
    Matrix inv; if (!Inverse(Linear(t.data()),inv)) return Affine({});
    auto result=Affine(inv);
    for (int j=0;j<3;++j) for (int k=0;k<3;++k) result[12+j]-=t[12+k]*inv.v[k*3+j];
    return result;
}
}

bool Identity(const Matrix& a) { return a.v==Matrix{}.v; }
bool Linear(const Curve& curve) {
    return curve[0]==curve[1]&&curve[2]==curve[3];
}
float CurveTime(const Curve& curve,float t) {
    if (Linear(curve)) return t;
    // The scale channel reuses the 0..127 time mapping of the four bone
    // channels (BoneEase, VA 0x004A05A0): x1/x2 drive the X polynomial whose
    // inverse is u, then y1/y2 give the eased fraction.  It is reimplemented
    // here because the scale curve lives in bone_scale's side table rather than
    // in a BoneKey's interpolation bytes, so BoneEase cannot be called for it.
    // Curve order is (x1, y1, x2, y2); the control-byte scale is 3/127.
#if defined(_M_IX86)
    const double scale = 0.02362200058996677;   // flt dword_52xxxx image
    const double x1c=static_cast<double>(static_cast<signed char>(curve[0]))*scale;
    const double x2c=static_cast<double>(static_cast<signed char>(curve[2]))*scale;
    float u=0.5f,step=0.5f;
    for (int i=0;i<12;++i) {
        const float om=1.0f-u;
        const double xu=(double)om*(double)om*(double)u*x1c+
                        (double)om*(double)u*(double)u*x2c+
                        (double)u*(double)u*(double)u;
        if (xu==(double)t) break;
        step*=0.5f;
        u=xu>=(double)t?(float)(u-step):(float)(u+step);
    }
    const float om=1.0f-u;
    const double y1c=static_cast<double>(static_cast<signed char>(curve[1]))*scale;
    const double y2c=static_cast<double>(static_cast<signed char>(curve[3]))*scale;
    return static_cast<float>((double)u*((double)u*(double)u)+
                              (double)om*(double)om*(double)u*y1c+
                              (double)om*(double)u*(double)u*y2c);
#else
    const float scale=0.02362200058996677f;     // kCpScaleF, 0x3CC182ED
    const float x1c=static_cast<float>(static_cast<signed char>(curve[0]))*scale;
    const float x2c=static_cast<float>(static_cast<signed char>(curve[2]))*scale;
    const float y1b=static_cast<float>(static_cast<signed char>(curve[1]));
    const float y2b=static_cast<float>(static_cast<signed char>(curve[3]));
    float u=0.5f,step=0.5f;
    for (int i=0;i<12;++i) {
        const float om=1.0f-u;
        const float gx=(om*om*u)*x1c+(om*u*u)*x2c+u*u*u;
        if (gx==t) break;
        step*=0.5f;
        u=gx>=t?u-step:u+step;
    }
    const float om=1.0f-u;
    return (om*om*u)*y1b*scale+(om*u*u)*y2b*scale+u*u*u;
#endif
}
bool Inverse(const Matrix& a,Matrix& out) {
    const auto& m=a.v; const double d=Determinant(a);
    if (!std::isfinite(d)||std::abs(d)<1.e-18) return false;
    const double cof[9]={double(m[4])*m[8]-double(m[5])*m[7],double(m[2])*m[7]-double(m[1])*m[8],double(m[1])*m[5]-double(m[2])*m[4],
        double(m[5])*m[6]-double(m[3])*m[8],double(m[0])*m[8]-double(m[2])*m[6],double(m[2])*m[3]-double(m[0])*m[5],
        double(m[3])*m[7]-double(m[4])*m[6],double(m[1])*m[6]-double(m[0])*m[7],double(m[0])*m[4]-double(m[1])*m[3]};
    for (int i=0;i<9;++i) out.v[i]=float(cof[i]/d);
    return true;
}
bool Valid(const Matrix& a) {
    for (float v:a.v) if (!std::isfinite(v)||std::abs(v)>10000.f) return false;
    if (Determinant(a)<=1.e-12) return false;
    Matrix inv; if (!Inverse(a,inv)) return false;
    for (float v:inv.v) if (!std::isfinite(v)||std::abs(v)>10000.f) return false;
    return true;
}
Matrix Linear(const float m[16]) {
    Matrix a;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) a.v[i*3+j]=m[i*4+j];
    return a;
}
Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix c;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) {
        double sum=0; for(int k=0;k<3;++k) sum+=double(a.v[i*3+k])*b.v[k*3+j];
        c.v[i*3+j]=float(sum);
    }
    return c;
}
Matrix Interpolate(const Matrix& a,const Matrix& b,float t) {
    if (t<=0 || a.v==b.v) return a;
    if (t>=1) return b;
    const Matrix ra=Rotation(a),rb=Rotation(b);
    const Matrix ha=Multiply(a,Transpose(ra)),hb=Multiply(b,Transpose(rb));
    Matrix h;
    for (int i=0;i<9;++i) h.v[i]=ha.v[i]+t*(hb.v[i]-ha.v[i]);
    auto qa=Quaternion(ra), qb=Quaternion(rb);
    double dot=0; for (int i=0;i<4;++i) dot+=qa[i]*qb[i];
    if (dot<0) { for (double& v:qb) v=-v; dot=-dot; }
    double wa=1-t,wb=t;
    if (dot<0.9995) {
        const double angle=std::acos(std::clamp(dot,0.0,1.0));
        wa=std::sin((1-t)*angle)/std::sin(angle); wb=std::sin(t*angle)/std::sin(angle);
    }
    for (int i=0;i<4;++i) qa[i]=wa*qa[i]+wb*qb[i];
    const Matrix result=Multiply(h,FromQuaternion(qa));
    return Valid(result)?result:(t<0.5f?a:b);
}
Matrix Pose(const unsigned char* model,int bone) {
    const auto it=states.find(model); return it==states.end()?Matrix{}:Lookup(it->second.current.pose,bone);
}
Matrix Key(const unsigned char* model,int index) {
    const auto it=states.find(model); return it==states.end()?Matrix{}:Lookup(it->second.current.keys,index);
}
bool SetPose(unsigned char* model,int bone,const Matrix& value) {
    if (!model||bone<0||bone>=int(mdl::Mdl(model)->boneCount)||!Valid(value)) return false;
    if (Pose(model,bone)==value) return true;
    RestoreTransforms(model); auto& s=states[model]; Store(s.current.pose,bone,value); ++s.revision; return true;
}
void SetKey(unsigned char* model,int index,const Matrix& value) {
    if (!model||index<0||
        static_cast<std::size_t>(index)>=mdl::BoneKeyCapacity(*mdl::Mdl(model))||
        !Valid(value)) return;
    if (Key(model,index)==value) return;
    auto& s=states[model]; Store(s.current.keys,index,value); ++s.revision;
}
void ClearKey(unsigned char* model,int index) {
    if (!model||index<0||
        static_cast<std::size_t>(index)>=mdl::BoneKeyCapacity(*mdl::Mdl(model))) return;
    auto& s=states[model];
    const bool hadScale=s.current.keys.erase(index)>0;
    const bool hadCurve=s.current.curves.erase(index)>0;
    if (hadScale||hadCurve) ++s.revision;
}
void SetKeyFresh(unsigned char* model,int index,const Matrix& value) {
    // The pool just handed this slot out: the previous occupant's curve must
    // not travel with the new key.
    ClearKey(model,index);
    SetKey(model,index,value);
}
Curve KeyCurve(const unsigned char* model,int index) {
    if (!model||index<0||
        static_cast<std::size_t>(index)>=mdl::BoneKeyCapacity(*mdl::Mdl(model)))
        return LinearCurve();
    const auto it=states.find(model);
    return it==states.end()?LinearCurve():LookupCurve(it->second.current.curves,index);
}
void SetKeyCurve(unsigned char* model,int index,const Curve& curve) {
    if (!model||index<0||
        static_cast<std::size_t>(index)>=mdl::BoneKeyCapacity(*mdl::Mdl(model))) return;
    auto& s=states[model];
    if (LookupCurve(s.current.curves,index)==curve) return;
    StoreCurve(s.current.curves,index,curve); ++s.revision;
}
void CopyKey(unsigned char* model,int destination,int source) {
    SetKey(model,destination,Key(model,source));
    SetKeyCurve(model,destination,KeyCurve(model,source));
}
void CopyClipboard(const MMDApp* app,int index,const Matrix& value,
                   bool pose,const Curve& curve) {
    if (index==0) {
        if (pose) { clipboards[app].pose.clear(); clipboards[app].poseCurves.clear(); }
        else { clipboards[app].keys.clear(); clipboards[app].keyCurves.clear(); }
    }
    Store(pose?clipboards[app].pose:clipboards[app].keys,index,value);
    StoreCurve(pose?clipboards[app].poseCurves:clipboards[app].keyCurves,index,curve);
}
Matrix Clipboard(const MMDApp* app,int index,bool pose,bool mirror) {
    const auto it=clipboards.find(app); if (it==clipboards.end()) return {};
    Matrix result=Lookup(pose?it->second.pose:it->second.keys,index);
    if (mirror) for (int i:{1,2,3,6}) result.v[i]=-result.v[i];
    return result;
}
Curve ClipboardCurve(const MMDApp* app,int index,bool pose) {
    const auto it=clipboards.find(app); if (it==clipboards.end()) return LinearCurve();
    return LookupCurve(pose?it->second.poseCurves:it->second.keyCurves,index);
}
Registration::Registration(const Matrix& value)
    :previous(registration),previousCurve(registrationCurve),
     previousExplicit(registrationCurveExplicit) {
    registration=value; registrationCurve=LinearCurve();
    registrationCurveExplicit=false;
}
Registration::Registration(const Matrix& value,const Curve& curve)
    :previous(registration),previousCurve(registrationCurve),
     previousExplicit(registrationCurveExplicit) {
    registration=value; registrationCurve=curve;
    registrationCurveExplicit=true;
}
Registration::~Registration() {
    registration=previous; registrationCurve=previousCurve;
    registrationCurveExplicit=previousExplicit;
}
Matrix RegistrationValue() { return registration; }
Curve RegistrationCurve() { return registrationCurve; }
DefaultCurveScope::DefaultCurveScope() {
    previousDefaultCurve = registrationDefaultCurve;
    registrationDefaultCurve = true;
}
DefaultCurveScope::~DefaultCurveScope() {
    registrationDefaultCurve = previousDefaultCurve;
}
void ApplyRegistration(unsigned char* model,int index,bool fresh) {
    if (fresh) ClearKey(model,index);
    SetKey(model,index,registration);
    // The curve is written when the payload owns one (explicit), when the slot
    // is fresh, or during a motion import; otherwise - an ordinary in-app
    // registration - the key keeps the curve it already had.
    if (!Linear(registrationCurve)) {
        SetKeyCurve(model,index,registrationCurve);
    } else if (fresh||registrationDefaultCurve||registrationCurveExplicit) {
        SetKeyCurve(model,index,LinearCurve());
    }
}
void Register(unsigned char* model,int bone,int key) { SetKey(model,key,Pose(model,bone)); }
Snapshot Capture(const unsigned char* model) {
    const auto it=states.find(model); return it==states.end()?Snapshot{}:it->second.current;
}
std::uint64_t Revision(const unsigned char* model) {
    const auto it=states.find(model); return it==states.end()?0:it->second.revision;
}
bool Restore(unsigned char* model,Snapshot value) {
    if (!model) return false;
    for (const auto& p:value.pose) if (p.first<0||p.first>=int(mdl::Mdl(model)->boneCount)||!Valid(p.second)) return false;
    const std::size_t boneKeyCapacity=mdl::BoneKeyCapacity(*mdl::Mdl(model));
    for (const auto& p:value.keys) if (p.first<0||static_cast<std::size_t>(p.first)>=boneKeyCapacity||!Valid(p.second)) return false;
    for (const auto& p:value.curves) if (p.first<0||static_cast<std::size_t>(p.first)>=boneKeyCapacity) return false;
    RestoreTransforms(model); auto& s=states[model];
    if (s.current.pose!=value.pose||s.current.keys!=value.keys||s.current.curves!=value.curves) ++s.revision;
    s.current=std::move(value); return true;
}
void CaptureUndo(unsigned char* model) {
    if (!model) return;
    auto& s=states[model]; const unsigned ring=mdl::Mdl(model)->undoState[0];
    if (ring>=30) return;
    bone_curve::CaptureUndo(model,ring);
    s.undo[ring]=s.current; s.undoValid[ring]=true; s.redoValid[ring]=false;
}
bool StageBakeState(unsigned char* model,Snapshot current,std::size_t capacity,StagedBakeState& out) {
    if(!model)return false;
    for(const auto& p:current.pose)if(p.first<0||p.first>=int(mdl::Mdl(model)->boneCount)||!Valid(p.second))return false;
    for(const auto& p:current.keys)if(p.first<0||std::size_t(p.first)>=capacity||!Valid(p.second))return false;
    for(const auto& p:current.curves)if(p.first<0||std::size_t(p.first)>=capacity)return false;
    StagedBakeState staged;staged.model=model;staged.ring=(mdl::Mdl(model)->undoState[0]+1)%30;
    staged.undo=Capture(model);staged.current=std::move(current);
    states.try_emplace(model);out=std::move(staged);return true;
}
void PublishBakeState(StagedBakeState&& staged) noexcept {
    RestoreTransforms(staged.model);auto& s=states.find(staged.model)->second;
    s.undo[staged.ring]=std::move(staged.undo);s.undoValid[staged.ring]=true;s.redoValid[staged.ring]=false;
    if(s.current.pose!=staged.current.pose||s.current.keys!=staged.current.keys||s.current.curves!=staged.current.curves)++s.revision;
    s.current=std::move(staged.current);
}
void Undo(unsigned char* model,int ring) {
    bone_curve::Undo(model,unsigned(ring));
    const auto it=states.find(model); if (it==states.end()||ring<0||ring>=30) return;
    auto& s=it->second;
    if (!s.undoValid[ring]) return;
    RestoreTransforms(model); s.redo[ring]=s.current; s.redoValid[ring]=true;
    if (s.current.pose!=s.undo[ring].pose||s.current.keys!=s.undo[ring].keys||
        s.current.curves!=s.undo[ring].curves) ++s.revision;
    s.current=s.undo[ring];
}
void Redo(unsigned char* model,int ring) {
    bone_curve::Redo(model,unsigned(ring));
    const auto it=states.find(model); if (it==states.end()||ring<0||ring>=30) return;
    auto& s=it->second;
    if (s.redoValid[ring]) {
        RestoreTransforms(model);
        if (s.current.pose!=s.redo[ring].pose||s.current.keys!=s.redo[ring].keys||
            s.current.curves!=s.redo[ring].curves) ++s.revision;
        s.current=s.redo[ring];
    }
}
static Matrix SampleInterval(const Values& values,const Curves& curves,const mdl::BoneKey* keys,
                             unsigned previous,unsigned next,double frame) {
    Matrix value=Lookup(values,previous);
    if (next && keys[next].frame>keys[previous].frame) {
        const float raw=float(std::clamp((frame-keys[previous].frame)/double(keys[next].frame-keys[previous].frame),0.0,1.0));
        // The interval belongs to its destination key, exactly like the four
        // bone channels (BoneEase is called with the key being applied): the
        // scale curve of `next` maps the whole 3x3 matrix through one 0..127
        // Bezier time and the existing Interpolate does the rest.
        const float t=CurveTime(LookupCurve(curves,static_cast<int>(next)),raw);
        value=Interpolate(value,Lookup(values,next),t);
    }
    return value;
}
static Matrix SampleKeys(const Values& values,const Curves& curves,const mdl::BoneKey* keys,unsigned bone,double frame,std::size_t capacity) {
    unsigned previous=bone,next=keys[bone].next;
    unsigned guard=0;
    while (next && next<capacity && keys[next].frame<=frame && ++guard<capacity) {
        previous=next; next=keys[next].next;
    }
    if (next>=capacity||guard>=capacity) return {};
    return SampleInterval(values,curves,keys,previous,next,frame);
}
// The native playback pass just found this bone's upper key (or terminal key).
// Check that it brackets this sample before using it. Never cache key values:
// edits to scale matrices/curves are visible on the very next sample.
static bool PlaybackInterval(const mdl::BoneKey* keys,std::size_t capacity,unsigned cursor,
                             double frame,unsigned& previous,unsigned& next) {
    if(cursor>=capacity)return false;
    const auto& key=keys[cursor];
    if(key.frame<=frame) {
        if(key.next>=capacity || (key.next && keys[key.next].frame<=frame))return false;
        previous=cursor;next=key.next;return true;
    }
    if(key.previous>=capacity)return false;
    const auto& before=keys[key.previous];
    if(before.next!=cursor || before.frame>frame || before.frame>=key.frame)return false;
    previous=key.previous;next=cursor;return true;
}
Matrix Sample(const unsigned char* model,int bone,double frame) {
    const auto it=states.find(model);
    if (!model||it==states.end()||!std::isfinite(frame)||bone<0||bone>=int(mdl::Mdl(model)->boneCount)) return {};
    const auto* keys=mdl::Mdl(model)->boneKeys;
    if (!keys||it->second.current.keys.empty()) return {};
    return SampleKeys(it->second.current.keys,it->second.current.curves,keys,unsigned(bone),frame,
                      mdl::BoneKeyCapacity(*mdl::Mdl(model)));
}
static void EvaluateModel(unsigned char* model,double frame,bool playback) {
    const auto it=states.find(model); if (it==states.end()||!std::isfinite(frame)) return;
    auto& s=it->second;
    RestoreTransforms(model); s.current.pose.clear();
    const auto* keys=mdl::BoneKeys(model); if (!keys||s.current.keys.empty()) return;
    // One capacity lookup for the whole per-bone sweep.
    const std::size_t boneKeyCapacity=mdl::BoneKeyCapacity(*mdl::Mdl(model));
    const auto& record=*mdl::Mdl(model);
    for (unsigned bone=0;bone<mdl::Mdl(model)->boneCount;++bone) {
        unsigned previous=0,next=0;
        // Native playback does not initialize or advance inert-tip cursors.
        const bool resolved=playback && record.boneKeyCursors && record.boneTable &&
            record.boneTable[bone].type!=mdl::BoneType::InertTip &&
            PlaybackInterval(keys,boneKeyCapacity,record.boneKeyCursors[bone],frame,previous,next);
        Store(s.current.pose,int(bone),resolved
            ? SampleInterval(s.current.keys,s.current.curves,keys,previous,next,frame)
            : SampleKeys(s.current.keys,s.current.curves,keys,bone,frame,boneKeyCapacity));
    }
}
void Evaluate(unsigned char* model,double frame) { EvaluateModel(model,frame,false); }
void EvaluatePlayback(unsigned char* model,double frame) { EvaluateModel(model,frame,true); }
void RestoreTransforms(unsigned char* model) {
    const auto it=states.find(model); if (it==states.end()||!it->second.applied) return;
    auto& s=it->second; auto* bones=mdl::Bones(model);
    if (bones && s.rigid.size()==mdl::Mdl(model)->boneCount)
        for (unsigned i=0;i<s.rigid.size();++i) std::memcpy(bones[i].matInit,s.rigid[i].data(),64);
    s.applied=false;
}
bool HasPose(const unsigned char* model) {
    const auto it=states.find(model);return it!=states.end()&&!it->second.current.pose.empty();
}
Values PoseValues(const unsigned char* model) {
    const auto it=states.find(model);return it==states.end()?Values{}:it->second.current.pose;
}
static Transform Pivot(const mdl::BoneRecord& bone,const Matrix& scale) {
    Transform pivot=Affine(scale);
    for(int c=0;c<3;++c) {
        pivot[12+c]=bone.position[c];
        for(int k=0;k<3;++k)pivot[12+c]-=bone.position[k]*scale.v[k*3+c];
    }
    return pivot;
}
static Transform WithLinearAtPivot(const mdl::BoneRecord& bone,const Transform& reference,const Matrix& linear) {
    Transform result=reference;
    for(int c=0;c<3;++c)for(int k=0;k<3;++k) {
        result[12+c]+=bone.position[k]*(reference[k*4+c]-linear.v[k*3+c]);
        result[k*4+c]=linear.v[k*3+c];
    }
    return result;
}
static Transform InheritTransform(const mdl::BoneRecord& bone,const Transform& rigid,
                                  const Matrix& parentScale,const Transform& parentDelta) {
    // The pivot follows the parent's full deformation. The shape inherits scale
    // in the unposed model basis, before this bone's rotation (issue #16).
    // Applying parentDelta to the whole child instead would change its lengths
    // and angles whenever it rotates under a non-uniformly scaled parent.
    return WithLinearAtPivot(bone,Product(rigid,parentDelta),Multiply(parentScale,Linear(rigid.data())));
}
void BuildTransforms(const unsigned char* model,const Values& pose,std::vector<Transform>& corrected,const std::vector<Transform>* rigidBaseline) {
    corrected.clear();if(!model)return;
    const auto& m=*mdl::Mdl(model);const int count=int(m.boneCount);const auto* bones=m.boneTable;
    if(!bones||count<=0)return;
    const auto it=states.find(model);
    const bool cached=it!=states.end()&&it->second.applied&&it->second.rigid.size()==m.boneCount;
    std::vector<Transform> rigid(count);
    for(int i=0;i<count;++i) {
        if(rigidBaseline&&rigidBaseline->size()==size_t(count))rigid[i]=(*rigidBaseline)[i];
        else if(cached)rigid[i]=it->second.rigid[i];
        else std::memcpy(rigid[i].data(),bones[i].matInit,64);
    }
    corrected.resize(count);
    std::vector<Matrix> cumulativeScale(count);
    std::vector<unsigned char> done(count);std::vector<bool> changed(count);std::vector<int> chain;
    for(int i=0;i<count;++i) {
        if(done[i]==2)continue;
        int j=i;while(j>=0&&j<count&&done[j]==0){done[j]=1;chain.push_back(j);j=bones[j].parent;}
        while(!chain.empty()) {
            const int index=chain.back();chain.pop_back();const auto& bone=bones[index];
            Transform base=rigid[index];
            const bool inherited=bone.parent>=0&&bone.parent<count&&done[bone.parent]==2&&changed[bone.parent];
            if(inherited) {
                const Transform delta=Product(InverseTransform(rigid[bone.parent]),corrected[bone.parent]);
                base=InheritTransform(bone,base,cumulativeScale[bone.parent],delta);
            }
            const Matrix scale=Lookup(pose,index);
            if(!Identity(scale))base=Product(Pivot(bone,scale),base);
            cumulativeScale[index]=inherited?Multiply(scale,cumulativeScale[bone.parent]):scale;
            corrected[index]=base;done[index]=2;changed[index]=inherited||!Identity(scale);
        }
    }
}
void ApplyTransforms(unsigned char* model) {
    const auto it=states.find(model);if(it==states.end())return;
    auto& s=it->second;if(s.applied||s.current.pose.empty())return;
    auto* bones=mdl::Bones(model);const auto count=mdl::Mdl(model)->boneCount;
    if(!bones||!count)return;
    std::vector<Transform> corrected;BuildTransforms(model,s.current.pose,corrected);
    s.rigid.resize(count);
    for(unsigned i=0;i<count;++i){std::memcpy(s.rigid[i].data(),bones[i].matInit,64);std::memcpy(bones[i].matInit,corrected[i].data(),64);}
    s.applied=true;
}
void ExchangeState(unsigned char* first,unsigned char* second) {
    if(first&&second&&first!=second){std::swap(states[first],states[second]);bone_curve::ExchangeState(first,second);}
}
void CopyState(const unsigned char* source,unsigned char* destination) {
    const auto it=states.find(source);
    if(it==states.end()||!destination){bone_curve::CopyState(source,destination);return;}
    const auto& s=it->second;
    Restore(destination,s.current);
    if(s.applied&&s.rigid.size()==mdl::Mdl(destination)->boneCount)
        for(unsigned i=0;i<s.rigid.size();++i)std::memcpy(mdl::Bones(destination)[i].matInit,s.rigid[i].data(),64);
    // Free-curve validation hashes include the restored scale-key state.
    bone_curve::CopyState(source,destination);
}
void ResolvePhysics(unsigned char* model,const std::map<int,Transform>& world) {
    if(!model||world.empty())return;
    RestoreTransforms(model);auto& s=states[model];
    auto* bones=mdl::Bones(model);const int count=int(mdl::Mdl(model)->boneCount);
    std::vector<Transform> corrected(count),rigid(count);std::vector<unsigned char> done(count);std::vector<int> chain;
    std::vector<Matrix> cumulativeScale(count);
    for(int i=0;i<count;++i)std::memcpy(rigid[i].data(),bones[i].matInit,64);
    for(int i=0;i<count;++i) {
        if(done[i]==2)continue;
        int j=i;while(j>=0&&j<count&&done[j]==0){done[j]=1;chain.push_back(j);j=bones[j].parent;}
        while(!chain.empty()) {
            const int b=chain.back();chain.pop_back();const auto& bone=bones[b];
            Transform inherited=Affine({});
            Matrix parentScale;
            const bool hasParent=bone.parent>=0&&bone.parent<count&&done[bone.parent]==2;
            if(hasParent) {
                inherited=Product(InverseTransform(rigid[bone.parent]),corrected[bone.parent]);
                parentScale=cumulativeScale[bone.parent];
            }
            const auto physical=world.find(b);
            bool resolved=false;
            if(physical!=world.end()) {
                const Matrix linear=Linear(physical->second.data());Matrix stretch,inverseParent;float rotation[4];
                if(affine::PolarDecompose(linear.v.data(),stretch.v.data(),rotation)&&Inverse(parentScale,inverseParent)) {
                    // Forward evaluation is localScale * parentScale * rotation.
                    // Remove parent scale from the stretch, not from the rotated
                    // physical matrix. Undo parent deformation only for the pivot.
                    const Matrix localScale=Multiply(stretch,inverseParent);
                    if(Valid(localScale)) {
                        Transform rotationOnly{};affine::DefaultMatrixFromQuaternion(rotationOnly.data(),rotation);
                        const Transform target=Product(physical->second,InverseTransform(inherited));
                        const Transform base=WithLinearAtPivot(bone,target,Linear(rotationOnly.data()));
                        rigid[b]=base;Store(s.current.pose,b,localScale);corrected[b]=physical->second;
                        std::memcpy(bones[b].matInit,base.data(),64);resolved=true;
                    }
                }
            }
            const Matrix scale=Lookup(s.current.pose,b);
            cumulativeScale[b]=hasParent?Multiply(scale,parentScale):scale;
            if(!resolved) {
                const Transform base=hasParent?InheritTransform(bone,rigid[b],parentScale,inherited):rigid[b];
                corrected[b]=Product(Pivot(bone,scale),base);
            }
            done[b]=2;
        }
    }
}
void Forget(unsigned char* model) { negative_frames::ForgetModel(model);bone_curve::Forget(model); states.erase(model); }
bool Scale(unsigned char* model,int bone,const Matrix& basis,const float direction[3],float factor,bool uniform) {
    if (!std::isfinite(factor)||factor<=0||!Valid(basis)) return false;
    Matrix delta;
    if (uniform) { delta.v[0]=delta.v[4]=delta.v[8]=factor; }
    else {
        double n=0; for(int i=0;i<3;++i) n+=double(direction[i])*direction[i];
        if (!std::isfinite(n)||n<1.e-20) return false;
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) delta.v[i*3+j]+=float((factor-1)*double(direction[i])*direction[j]/n);
        Matrix inv; if(!Inverse(basis,inv)) return false;
        delta=Multiply(Multiply(basis,delta),inv);
    }
    return SetPose(model,bone,Multiply(Pose(model,bone),delta));
}
bool Enabled(MMDApp* app) {
    // Legacy names Camera/Light also denote the bone rotate/move cursors.
    // The panel flag, not that cursor enum, distinguishes scene objects.
    if (!app||app->state.optflag[0]!=0||app->PlaybackActive()!=0) return false;
    auto* model=app->SelectedModel();
    return model && mdl::Bones(model) && mdl::Mdl(model)->selectedBone>=0 &&
        mdl::Mdl(model)->selectedBone<int(mdl::Mdl(model)->boneCount);
}
namespace {
// One multiplicative increment applied to every selected root bone.  An empty
// `directions` list is the uniform (XYZ) increment, one entry is a single axis
// and two entries are the view-plane square.  Every caller shares this body so
// undo, keyframe registration and the PMM/VMD paths stay on one code path.
void ApplyScaleIncrement(MMDApp* app,
                         const std::vector<std::array<float,3>>& directions,
                         float factor) {
    auto* model=app->SelectedModel(); if (!model) return;
    auto* record=mdl::Mdl(model); auto* bones=mdl::Bones(model);
    if (!record||!bones) return;
    const int active=record->selectedBone,count=record->boneCount;
    if (active<0||count<=0||active>=count) return;
    ApplyTransforms(model);
    std::vector<Matrix> bases(count);
    for (int i=0;i<count;++i) {
        Matrix inv; Inverse(Pose(model,i),inv);
        bases[i]=Multiply(inv,Linear(bones[i].matInit));
    }
    bool changed=false;
    for (int i=0;i<count;++i) {
        const auto selected=[&](int b) { return b==active||(record->boneSelection&&record->boneSelection[b]); };
        if (!selected(i)) continue;
        bool child=false; int p=bones[i].parent;
        for (int guard=0;p>=0&&p<count&&guard<count;++guard) {
            if (selected(p)) { child=true; break; } p=bones[p].parent;
        }
        if (child) continue;
        bool applied=false;
        if (directions.empty()) {
            // Uniform: Scale keeps its whole delta on the diagonal and never
            // reads the direction pointer.
            applied=Scale(model,i,bases[i],nullptr,factor,true);
        } else {
            // Orthogonal directions compose into one independent factor per
            // direction: for the view-plane square the two single-axis deltas
            // multiply to (1+(f-1)) on each of the two axes and leave the third
            // axis untouched.
            for (const auto& direction:directions)
                applied=Scale(model,i,bases[i],direction.data(),factor,false)||applied;
        }
        if (applied) {
            if (record->bonePhysicsState) record->bonePhysicsState[i]=1;
            changed=true;
        }
    }
    if (changed) { app->SceneModified()=1; app->RecordedFrameCountByte1()=1; }
}
float DragFactor(MMDApp* app,float amount) {
    const float speed=app->CtrlModifierActive()?0.001f:app->ShiftModifierActive()?0.05f:0.01f;
    return std::exp(std::clamp(amount*speed,-2.f,2.f));
}
// World-space direction of one scale axis for the selected bone.
//
// viewOnly = true   the 缩放 edit mode's viewport handles: a view-axis tool
//                   that always scales along the screen right / up / depth
//                   axes, whatever the caption says, so the handles it draws
//                   and the axes it scales never disagree.
// viewOnly = false  the HUD scale row's X/Y/Z cells: they follow the local /
//                   global / view caption like the rotate and move rows.  The
//                   caller must have run ApplyTransforms first: the local
//                   branch reads the corrected bone matrices, exactly as the
//                   original did.
void AxisDirection(MMDApp* app,int axis,float out[3],bool viewOnly) {
    out[0]=out[1]=out[2]=0.0f;
    if (axis<0||axis>2) return;
    const int mode=viewOnly?2:EffectiveCoordinateState(app);
    if (mode==2) { ViewWorldAxis(app,axis,out); return; }
    if (mode==1) { out[axis]=1.0f; return; }
    float local[16]; BoneLocalAxes(app,local);
    auto* model=app->SelectedModel();
    if (model==nullptr) return;
    const Matrix current=Linear(mdl::Bones(model)[mdl::Mdl(model)->selectedBone].matInit);
    for (int c=0;c<3;++c) for (int k=0;k<3;++k) out[c]+=local[axis*4+k]*current.v[k*3+c];
}
}

void Drag(MMDApp* app,int axis) {
    // HUD row semantics: the amount is the vertical pointer delta, so dragging
    // up grows.  The viewport handles use DragScreen / DragScreenPlane instead.
    if (!Enabled(app)||axis < -1||axis>2) return;
    const int pixels=app->PreviousMouseY()-app->MouseY(); if (!pixels) return;
    const float factor=DragFactor(app,static_cast<float>(pixels));
    auto* model=app->SelectedModel();
    // The local basis below reads the corrected bone matrices, so the display
    // affine transforms have to be installed first, exactly as the original did
    // before it read bones[active].matInit.  ApplyScaleIncrement repeats the
    // call, which is a no-op once the transforms are applied.
    ApplyTransforms(model);
    std::vector<std::array<float,3>> directions;
    if (axis>=0) {
        std::array<float,3> direction{};
        AxisDirection(app,axis,direction.data(),false);
        directions.push_back(direction);
    }
    ApplyScaleIncrement(app,directions,factor);
}
void DragScreen(MMDApp* app,int axis) {
    // One axis handle: the amount is this frame's pointer delta measured along
    // the direction the axis points on screen (bone_scale_gizmo.hpp), so a
    // horizontal axis is driven by a horizontal drag.  An axis the render pass
    // marked unavailable, and a pointer delta perpendicular to the handle, both
    // yield exactly zero and apply nothing.  gizmo::Active is the same gate the
    // hit test uses, so a stray call outside scale mode is inert.
    if (!gizmo::Active(app)||axis<0||axis>2) return;
    const float amount=gizmo::ScreenDragAmount(
        gizmo::Published(app),gizmo::HandleOfAxis(axis),
        static_cast<float>(app->MouseX()-app->PreviousMouseX()),
        static_cast<float>(app->MouseY()-app->PreviousMouseY()));
    if (amount==0.0f) return;
    auto* model=app->SelectedModel();
    ApplyTransforms(model);
    std::array<float,3> direction{};
    AxisDirection(app,axis,direction.data(),true);
    std::vector<std::array<float,3>> directions{direction};
    ApplyScaleIncrement(app,directions,DragFactor(app,amount));
}
void DragScreenPlane(MMDApp* app) {
    // The white square of the viewport gizmo spans the coordinate selector's X
    // and Y axes, so it scales the same local / global / view plane it is drawn
    // in.  The drag amount runs along the square's projected diagonal, so a
    // horizontal and a vertical drag both move it.
    if (!gizmo::Active(app)) return;
    const float amount=gizmo::ScreenDragAmount(
        gizmo::Published(app),gizmo::Handle::Plane,
        static_cast<float>(app->MouseX()-app->PreviousMouseX()),
        static_cast<float>(app->MouseY()-app->PreviousMouseY()));
    if (amount==0.0f) return;
    ApplyTransforms(app->SelectedModel());
    std::vector<std::array<float,3>> directions(2);
    AxisDirection(app,0,directions[0].data(),true);
    AxisDirection(app,1,directions[1].data(),true);
    ApplyScaleIncrement(app,directions,DragFactor(app,amount));
}

// ---- viewport gizmo handles (MMS addition, no original VA) -----------------
// The render pass publishes the projected handles it drew; the input path reads
// them back here.  See bone_scale_gizmo.hpp for why the two halves share this
// table instead of projecting twice.
namespace gizmo {
namespace {
std::unordered_map<const MMDApp*, ScreenGeometry> published;
}

int AxisOf(Handle handle) {
    switch (handle) {
    case Handle::AxisX: return 0;
    case Handle::AxisY: return 1;
    case Handle::AxisZ: return 2;
    default: return -1;
    }
}

Handle HandleOfAxis(int axis) {
    if (axis < 0 || axis > 2) return Handle::None;
    return static_cast<Handle>(static_cast<int>(Handle::AxisX) + axis);
}

Handle HitTest(const ScreenGeometry& geometry,float x,float y) {
    if (!geometry.valid||!(geometry.half>0.0f)) return Handle::None;
    // 1. The three square handles.  They sit at the rod ends, outside the white
    //    square, and win over every other part: in view mode the depth rod
    //    collapses onto the origin, where its square would otherwise be hidden
    //    under the box.  An axis the render pass marked unavailable (its
    //    projection is too short to separate the square from the origin) is
    //    skipped here as well, so it cannot take the box's pixels.
    for (int axis=0;axis<3;++axis) {
        if (!geometry.axisAvailable[axis]) continue;
        if (std::fabs(x-geometry.tipX[axis])<=geometry.half &&
            std::fabs(y-geometry.tipY[axis])<=geometry.half)
            return static_cast<Handle>(static_cast<int>(Handle::AxisX)+axis);
    }
    // 2. The rods, outside the white square.  Inside the square the pointer
    //    belongs to the plane handle, so a rod is only hit once its nearest
    //    point is beyond the square's diagonal.
    const float boxX=geometry.planeX-geometry.originX;
    const float boxY=geometry.planeY-geometry.originY;
    const float boxRadius=std::sqrt(boxX*boxX+boxY*boxY);
    const float rodSquared=geometry.rod*geometry.rod;
    for (int axis=0;axis<3;++axis) {
        if (!geometry.axisAvailable[axis]) continue;
        const float dx=geometry.endX[axis]-geometry.originX;
        const float dy=geometry.endY[axis]-geometry.originY;
        const float lengthSquared=dx*dx+dy*dy;
        if (lengthSquared<=1.e-6f) continue;
        float t=((x-geometry.originX)*dx+(y-geometry.originY)*dy)/lengthSquared;
        t=t<0.0f?0.0f:(t>1.0f?1.0f:t);
        const float closestX=geometry.originX+t*dx;
        const float closestY=geometry.originY+t*dy;
        const float closestBoxX=closestX-geometry.originX;
        const float closestBoxY=closestY-geometry.originY;
        if (std::sqrt(closestBoxX*closestBoxX+closestBoxY*closestBoxY)<boxRadius)
            continue;
        const float distanceX=x-closestX,distanceY=y-closestY;
        if (distanceX*distanceX+distanceY*distanceY<=rodSquared)
            return static_cast<Handle>(static_cast<int>(Handle::AxisX)+axis);
    }
    // 3. The white square.  A skewed square is the parallelogram spanned by
    //    its two edge corners; otherwise it is the axis-aligned box with one
    //    corner on the origin.
    if (geometry.planeSkewed) {
        const float ax=geometry.planeRightX-geometry.originX;
        const float ay=geometry.planeRightY-geometry.originY;
        const float bx=geometry.planeUpX-geometry.originX;
        const float by=geometry.planeUpY-geometry.originY;
        const float det=ax*by-ay*bx;
        if (std::fabs(det)>1.e-3f) {
            const float px=x-geometry.originX,py=y-geometry.originY;
            const float s=(px*by-py*bx)/det;
            const float t=(ax*py-ay*px)/det;
            if (s>=0.0f&&s<=1.0f&&t>=0.0f&&t<=1.0f) return Handle::Plane;
        }
        return Handle::None;
    }
    const float left=std::min(geometry.originX,geometry.planeX);
    const float right=std::max(geometry.originX,geometry.planeX);
    const float top=std::min(geometry.originY,geometry.planeY);
    const float bottom=std::max(geometry.originY,geometry.planeY);
    if (x>=left&&x<=right&&y>=top&&y<=bottom) return Handle::Plane;
    return Handle::None;
}

float ScreenDragAmount(const ScreenGeometry& geometry,Handle handle,float dx,
                       float dy) {
    if (!geometry.valid) return 0.0f;
    float directionX=0.0f;
    float directionY=0.0f;
    if (handle==Handle::Plane) {
        // The square's diagonal: it projects at 45 degrees, so both a
        // horizontal and a vertical drag have a component along it.
        directionX=geometry.planeX-geometry.originX;
        directionY=geometry.planeY-geometry.originY;
    } else {
        const int axis=AxisOf(handle);
        if (axis<0||!geometry.axisAvailable[axis]) return 0.0f;
        directionX=geometry.tipX[axis]-geometry.originX;
        directionY=geometry.tipY[axis]-geometry.originY;
    }
    const float length=std::sqrt(directionX*directionX+
                                 directionY*directionY);
    if (!(length>1.0e-4f)) {
        // An end-on axis is drawn as a square on the origin with no screen
        // direction; dragging it up grows it, like the HUD row cells.
        if (handle==Handle::Plane) return 0.0f;
        return -dy;
    }
    return (dx*directionX+dy*directionY)/length;
}

void Publish(MMDApp* app,const ScreenGeometry& geometry) {
    if (app==nullptr) return;
    published[app]=geometry;
}

const ScreenGeometry& Published(const MMDApp* app) {
    static const ScreenGeometry none;
    const auto it=published.find(app);
    return it==published.end()?none:it->second;
}

bool Active(MMDApp* app) {
    if (app==nullptr||app->EditMode()!=ViewportEditMode::Scale) return false;
    return Enabled(app)&&Published(app).valid;
}

Handle HitTest(MMDApp* app,float x,float y) {
    if (!Active(app)) return Handle::None;
    return HitTest(Published(app),x,y);
}

Highlight HighlightOf(MMDApp* app) {
    Highlight lit;
    if (app==nullptr) return lit;
    // A running drag owns the highlight; otherwise the hovered tool does.
    int handle=-1;  // -1 none, 0..2 axis, 3 plane, 4 uniform (every handle)
    switch (app->InteractionDragMode()) {
    case ViewportDragMode::ScaleUniform: handle=4; break;
    case ViewportDragMode::ScaleX: case ViewportDragMode::ScaleGizmoX: handle=0; break;
    case ViewportDragMode::ScaleY: case ViewportDragMode::ScaleGizmoY: handle=1; break;
    case ViewportDragMode::ScaleZ: case ViewportDragMode::ScaleGizmoZ: handle=2; break;
    case ViewportDragMode::ScaleGizmoPlane: handle=3; break;
    default:
        switch (app->ViewportToolOperation()) {
        case ViewportToolAction::ScaleUniform: handle=4; break;
        case ViewportToolAction::ScaleX: case ViewportToolAction::ScaleGizmoX: handle=0; break;
        case ViewportToolAction::ScaleY: case ViewportToolAction::ScaleGizmoY: handle=1; break;
        case ViewportToolAction::ScaleZ: case ViewportToolAction::ScaleGizmoZ: handle=2; break;
        case ViewportToolAction::ScaleGizmoPlane: handle=3; break;
        default: break;
        }
        break;
    }
    if (handle==4) { lit.plane=true; lit.axis[0]=lit.axis[1]=lit.axis[2]=true; }
    else if (handle==3) lit.plane=true;
    else if (handle>=0) lit.axis[handle]=true;
    return lit;
}
} // namespace gizmo

// ---- native frame retime support (see bone_scale.hpp) ----------------------
// The curve table is addressed by key slot, so the frame conversion replaces
// entries instead of remapping them, and the whole table is exchanged as one
// object: installing is one swap and rolling a commit back is the same swap
// called again, so neither can allocate.
bool CaptureCurveTable(const unsigned char* model, CurveTableState& out) {
    out = CurveTableState{};
    out.model = model;
    if (model == nullptr) return false;
    const auto found = states.find(model);
    if (found == states.end()) return false;
    out.curves = found->second.current.curves;
    out.keys = found->second.current.keys;
    out.revision = found->second.revision;
    return true;
}

void SwapCurveTable(unsigned char* model, CurveTableState& state) noexcept {
    if (model == nullptr) return;
    const auto found = states.find(model);
    // The holder was captured from an existing entry and nothing removes that
    // entry while a plan is alive, so a miss leaves the caller's table alone
    // rather than inventing one (which would have to allocate).
    if (found == states.end()) return;
    found->second.current.curves.swap(state.curves);
    found->second.current.keys.swap(state.keys);
    std::swap(found->second.revision, state.revision);
    state.model = model;
}
}
}
