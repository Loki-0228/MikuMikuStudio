#include "mikudancestudio/extended_names.hpp"
#include <charconv>
#include "mikudancestudio/frame_limits.hpp"
#define NOMINMAX
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace mikudancestudio::bone_curve {
namespace {
namespace ac = animation_curve;
constexpr double pi = 3.14159265358979323846;
struct NativeKey {
    unsigned slot;
    mdl::BoneKey key;
    bone_scale::Matrix scale;
    bone_scale::Curve scaleCurve;
};
struct Entry {
    Tracks tracks;
    std::uint64_t nativeHash = 0;
    std::vector<NativeKey> baseline;
    std::set<int> registrations;
};
using Curves = std::map<int, Entry>;
struct State {
    const void* identity = nullptr;
    Curves curves;
    unsigned deferredEdits = 0;
    std::array<Curves, 30> undo, redo;
    std::array<bool, 30> undoValid{}, redoValid{};
};
std::map<const unsigned char*, State> states;
bool Chain(const unsigned char* model, int bone, std::vector<unsigned>& slots) {
    if (!model || bone < 0 || unsigned(bone) >= mdl::Mdl(model)->boneCount) return false;
    const auto* pool = mdl::Mdl(model)->boneKeys;
    const auto capacity = mdl::BoneKeyCapacity(pool);
    if (!pool || unsigned(bone) >= capacity) return false;
    unsigned slot = unsigned(bone);
    for (std::size_t hops = 0; hops < capacity; ++hops) {
        if (slot >= capacity || (!slots.empty() && pool[slot].frame <= pool[slots.back()].frame)) return false;
        slots.push_back(slot);
        if (!pool[slot].next) return true;
        slot = pool[slot].next;
    }
    return false;
}
std::uint64_t Hash(const unsigned char* model, int bone) {
    if(!model||bone<0||unsigned(bone)>=mdl::Mdl(model)->boneCount)return 0;
    const auto* pool=mdl::Mdl(model)->boneKeys;const auto capacity=mdl::BoneKeyCapacity(pool);
    if(!pool||unsigned(bone)>=capacity)return 0;
    std::uint64_t hash = 14695981039346656037ull;
    const auto bytes = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { hash ^= b[i]; hash *= 1099511628211ull; }
    };
    unsigned slot=unsigned(bone),previousFrame=0;
    for(std::size_t hop=0;hop<capacity;++hop) {
        if(slot>=capacity||(hop&&pool[slot].frame<=previousFrame))return 0;
        auto key = pool[slot];previousFrame=key.frame;
        key.allocated = 0; key.reserved[0] = key.reserved[1] = 0;
        bytes(&key, sizeof key);
        const auto scale = bone_scale::Key(model, slot);
        const auto curve = bone_scale::KeyCurve(model, slot);
        bytes(scale.v.data(), sizeof(float) * 9); bytes(curve.data(), curve.size());
        if(!key.next)return hash;
        slot=key.next;
    }
    return 0;
}
State* Current(const unsigned char* model) {
    auto it = states.find(model);
    if (it == states.end()) return nullptr;
    if (!model || it->second.identity != mdl::Mdl(model)->boneTable) {
        states.erase(it); return nullptr;
    }
    return &it->second;
}
bool Reconcile(const unsigned char* model, int bone, Entry& entry);
Entry* Current(const unsigned char* model, int bone) {
    auto* state = Current(model);
    if (!state) return nullptr;
    auto it = state->curves.find(bone);
    if (it == state->curves.end()) return nullptr;
    if (!Reconcile(model,bone,it->second)) return nullptr;
    return &it->second;
}
void Quaternion(const Tracks& tracks, double frame, float* q) {
    const double x = ac::Evaluate(tracks[3], frame).value * pi / 360;
    const double y = ac::Evaluate(tracks[4], frame).value * pi / 360;
    const double z = ac::Evaluate(tracks[5], frame).value * pi / 360;
    const double sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y), sz = std::sin(z), cz = std::cos(z);
    q[0] = float(cy*sx*cz + sy*cx*sz); q[1] = float(sy*cx*cz - cy*sx*sz);
    q[2] = float(cy*cx*sz - sy*sx*cz); q[3] = float(cy*cx*cz + sy*sx*sz);
}
bone_scale::Matrix Scale(const Tracks& tracks, double frame, bone_scale::Matrix matrix) {
    for (int row = 0; row < 3; ++row) {
        double norm = 0; for (int col = 0; col < 3; ++col) norm += double(matrix.v[row*3+col])*matrix.v[row*3+col];
        norm = std::sqrt(norm);
        const double value = ac::Evaluate(tracks[6+row], frame).value;
        for (int col = 0; col < 3; ++col)
            matrix.v[row*3+col] = float(norm > 1e-12 ? matrix.v[row*3+col]*value/norm : (col == row ? value : 0));
    }
    return matrix;
}
bool Snapshot(const unsigned char* model,int bone,std::vector<NativeKey>& out) {
    std::vector<unsigned> slots;
    if(!Chain(model,bone,slots))return false;
    out.reserve(slots.size());
    for(auto slot:slots){const auto& key=mdl::Mdl(model)->boneKeys[slot];
        if(key.frame>unsigned(frame_limits::maximum))return false;
        for(float value:key.position)if(!std::isfinite(value))return false;
        double norm=0;for(float value:key.rotation){if(!std::isfinite(value))return false;norm+=double(value)*value;}
        auto scale=bone_scale::Key(model,slot);
        if(norm<1e-20||!bone_scale::Valid(scale))return false;
        out.push_back({slot,key,scale,bone_scale::KeyCurve(model,slot)});
    }
    return !out.empty()&&out.front().key.frame==0;
}
std::array<double,3> Angles(const float* q,const std::array<double,3>& reference) {
    double norm=0;for(int c=0;c<4;++c)norm+=double(q[c])*q[c];
    const double inverse=1/std::sqrt(norm),x=q[0]*inverse,y=q[1]*inverse,z=q[2]*inverse,w=q[3]*inverse;
    const double rx=std::asin(std::clamp(2*(x*w-y*z),-1.0,1.0));
    const auto nearest=[](double value,double target){return value+360*std::round((target-value)/360);};
    std::array<double,3> angles;
    if(std::abs(std::cos(rx))<1e-7){
        // Keep the existing roll at the singularity instead of resetting it.
        angles={rx*180/pi,std::atan2(2*(y*w-x*z),1-2*(y*y+z*z))*180/pi,reference[2]};
        angles[1]+=rx>=0?reference[2]:-reference[2];
    }else angles={rx*180/pi,std::atan2(2*(x*z+y*w),1-2*(x*x+y*y))*180/pi,
                              std::atan2(2*(x*y+z*w),1-2*(x*x+z*z))*180/pi};
    auto alternate=std::array<double,3>{180-angles[0],angles[1]+180,angles[2]+180};
    double first=0,second=0;
    for(int c=0;c<3;++c){angles[c]=nearest(angles[c],reference[c]);alternate[c]=nearest(alternate[c],reference[c]);
        first+=std::pow(angles[c]-reference[c],2);second+=std::pow(alternate[c]-reference[c],2);}
    return second<first?alternate:angles;
}
bool SameRotation(const float* a,const float* b) {
    if(!std::memcmp(a,b,4*sizeof(float)))return true;
    double dot=0,aa=0,bb=0;for(int c=0;c<4;++c){dot+=double(a[c])*b[c];aa+=double(a[c])*a[c];bb+=double(b[c])*b[c];}
    return aa>0&&bb>0&&dot*dot/(aa*bb)>1-1e-14;
}
bool SamePose(const NativeKey& a,const NativeKey& b) {
    return !std::memcmp(a.key.position,b.key.position,sizeof a.key.position)&&
           SameRotation(a.key.rotation,b.key.rotation)&&a.scale==b.scale;
}
bone_scale::Curve Controls(const NativeKey& key,int channel) {
    if(channel>=6)return key.scaleCurve;
    const int lane=channel<3?channel:3;
    return {key.key.interpolation[lane],key.key.interpolation[4+lane],
            key.key.interpolation[8+lane],key.key.interpolation[12+lane]};
}
double ScaleValue(const bone_scale::Matrix& scale,int axis) {
    double norm=0;for(int c=0;c<3;++c)norm+=double(scale.v[axis*3+c])*scale.v[axis*3+c];
    return std::sqrt(norm);
}
bool FindOrInsert(ac::Track& track,int frame,std::size_t& index,std::string& error) {
    const auto found=std::lower_bound(track.keys.begin(),track.keys.end(),frame,[](const ac::Key& key,int f){return key.frame<f;});
    index=std::size_t(found-track.keys.begin());
    if(found!=track.keys.end()&&found->frame==frame)return true;
    return ac::InsertPreservingShape(track,frame,index,error)!=ac::InsertResult::Rejected;
}
bool Reconcile(const unsigned char* model,int bone,Entry& entry) {
    if (auto* state = Current(model); state && state->deferredEdits) return true;
    const auto hash=Hash(model,bone);
    if(!hash)return false;
    if(hash==entry.nativeHash&&entry.registrations.empty())return true;
    std::vector<NativeKey> fresh;
    if(!Snapshot(model,bone,fresh))return false;
    // Stable native slots distinguish retiming from registration. A recycled
    // slot with a different frame and pose is a deletion followed by insertion.
    std::map<unsigned,std::size_t> bySlot;
    std::map<int,std::size_t> byFrame;
    for(std::size_t i=0;i<entry.baseline.size();++i){bySlot.emplace(entry.baseline[i].slot,i);byFrame.emplace(int(entry.baseline[i].key.frame),i);}
    std::vector<int> match(fresh.size(),-1);std::set<std::size_t> used;
    for(std::size_t i=0;i<fresh.size();++i){auto found=bySlot.find(fresh[i].slot);
        if(found!=bySlot.end()&&(entry.baseline[found->second].key.frame==fresh[i].key.frame||
            (!entry.registrations.count(int(fresh[i].key.frame))&&SamePose(entry.baseline[found->second],fresh[i])))){
            match[i]=int(found->second);used.insert(found->second);}}
    for(std::size_t i=0;i<fresh.size();++i)if(match[i]<0){auto found=byFrame.find(int(fresh[i].key.frame));
        if(found!=byFrame.end()&&!used.count(found->second)){match[i]=int(found->second);used.insert(found->second);}}
    std::map<int,int> frames;
    for(std::size_t i=0;i<fresh.size();++i)if(match[i]>=0)frames.emplace(int(entry.baseline[match[i]].key.frame),int(fresh[i].key.frame));
    Tracks tracks=entry.tracks;
    for(auto& track:tracks){const auto original=track.keys;track.keys.clear();
        for(std::size_t i=0;i<original.size();++i){auto moved=frames.find(original[i].frame);if(moved==frames.end())continue;
            auto key=original[i];key.frame=moved->second;
            // Keep normalized handle positions when stretching an interval.
            if(i){auto previous=frames.find(original[i-1].frame);if(previous!=frames.end()&&previous->second<key.frame)
                key.inSlope*=double(original[i].frame-original[i-1].frame)/(key.frame-previous->second);}
            if(i+1<original.size()){auto next=frames.find(original[i+1].frame);if(next!=frames.end()&&next->second>key.frame)
                key.outSlope*=double(original[i+1].frame-original[i].frame)/(next->second-key.frame);}
            track.keys.push_back(key);
        }
        std::sort(track.keys.begin(),track.keys.end(),[](const auto&a,const auto&b){return a.frame<b.frame;});
        if(track.keys.empty()||track.keys.front().frame!=0)return false;
        // Deleting a middle key joins handles that previously belonged to
        // different intervals. Keep their slopes but fit their time weights.
        for(std::size_t i=1;i<track.keys.size();++i){auto& a=track.keys[i-1];auto& b=track.keys[i];
            const double sum=a.outWeight+b.inWeight;
            if(sum>0.9999){a.outWeight=std::clamp(a.outWeight*0.9998/sum,0.0001,0.9997);b.inWeight=0.9998-a.outWeight;}}
    }
    std::string error;
    for(std::size_t i=0;i<fresh.size();++i){const auto& native=fresh[i];const int frame=int(native.key.frame);
        const NativeKey* old=match[i]<0?nullptr:&entry.baseline[match[i]];
        const bool registered=entry.registrations.count(frame)!=0;
        const bool rotationChanged=!old||!SameRotation(old->key.rotation,native.key.rotation);
        std::array<double,3> reference{};
        for(int axis=0;axis<3;++axis)reference[axis]=ac::Evaluate(tracks[3+axis],frame).value;
        const auto angles=Angles(native.key.rotation,reference);
        for(int c=0;c<9;++c){auto& track=tracks[c];
            const double value=c<3?native.key.position[c]:c<6?angles[c-3]:ScaleValue(native.scale,c-6);
            const bool changed=!old||(c<3?native.key.position[c]!=old->key.position[c]:c<6?rotationChanged:
                ScaleValue(native.scale,c-6)!=ScaleValue(old->scale,c-6));
            const bool controlsChanged=old?Controls(*old,c)!=Controls(native,c):!bone_scale::Linear(Controls(native,c));
            if(!changed&&!controlsChanged&&!registered)continue;
            std::size_t at=0;
            if(!FindOrInsert(track,frame,at,error))return false;
            // Untouched float proxies must not quantize high precision values
            // or remove full Euler revolutions from an existing authored key.
            if(changed){const double tolerance=c<3?0:1e-5;
                if(c<3?float(track.keys[at].value)!=float(value):std::abs(track.keys[at].value-value)>tolerance)
                    track.keys[at].value=value;
            }
            if(controlsChanged&&at){const auto controls=Controls(native,c);
                // The native destination owns the interval beginning at the
                // preceding native row, even when this lane had no key there.
                std::size_t start=0;
                if(!FindOrInsert(track,int(fresh[i-1].key.frame),start,error))return false;
                at=std::size_t(std::lower_bound(track.keys.begin(),track.keys.end(),frame,[](const ac::Key& k,int f){return k.frame<f;})-track.keys.begin());
                auto& a=track.keys[at-1];auto& b=track.keys[at];
                const double x1=std::clamp(controls[0]/127.0,0.0002,0.9996),x2=std::clamp(controls[2]/127.0,x1+0.0002,0.9998);
                const double rate=(b.value-a.value)/double(b.frame-a.frame);
                a.outWeight=x1;b.inWeight=1-x2;a.outSlope=controls[1]/127.0/x1*rate;b.inSlope=(1-controls[3]/127.0)/(1-x2)*rate;
                a.tangent=b.tangent=ac::Tangent::Free;b.interpolation=ac::Interpolation::Bezier;
            }
        }
    }
    for(const auto& track:tracks)if(!ac::Validate(track,error))return false;
    entry.tracks=std::move(tracks);entry.baseline=std::move(fresh);entry.nativeHash=hash;entry.registrations.clear();
    return true;
}
std::string StableId(const unsigned char* model, int bone, int channel) {
    std::string id = "bone/" + std::to_string(bone) + "/" + std::to_string(channel) + "/";
    const auto* name = mdl::Mdl(model)->boneTable[bone].name;
    constexpr char hex[] = "0123456789abcdef";
    for (int i=0; i<20; ++i) { const auto b=static_cast<unsigned char>(name[i]); id+=hex[b>>4]; id+=hex[b&15]; }
    return id;
}
std::string PmmKey(int slot) { return "org.mikudancestudio.model." + std::to_string(slot) + ".boneCurve.v1"; }
}
struct RangeRetime::Impl {
    unsigned char* model = nullptr;
    Curves curves;
    bool ready = false, active = false;
};
RangeRetime::RangeRetime(unsigned char* model, int first, int last, double scale)
    : impl_(std::make_unique<Impl>()) {
    impl_->model = model;
    if (!model || first < 0 || last < first || !std::isfinite(scale) || scale <= 0) return;
    Synchronize(model);
    auto* state = Current(model);
    if (!state) { impl_->ready = true; return; }
    impl_->curves = state->curves;
    const auto mapped = [=](int frame) {
        if (frame < first) return frame;
        if (frame <= last) return first + int(double(frame - first) * scale);
        return frame + int(double(last - first) * scale) - (last - first);
    };
    std::string error;
    for (auto& pair : impl_->curves) for (auto& track : pair.second.tracks) {
        const auto original = track.keys;
        std::vector<std::size_t> chosen;
        track.keys.clear();
        for (std::size_t i = 0; i < original.size(); ++i) {
            auto key = original[i]; key.frame = mapped(key.frame);
            if (key.frame < 0 || key.frame > frame_limits::maximum) return;
            // Match native overwrite semantics, retaining the mandatory root.
            if (!track.keys.empty() && track.keys.back().frame == key.frame) {
                if (key.frame != 0) { track.keys.back() = key; chosen.back() = i; }
            } else { track.keys.push_back(key); chosen.push_back(i); }
        }
        for (std::size_t i = 0; i < track.keys.size(); ++i) {
            auto& key = track.keys[i];
            if (i) key.inSlope *= double(original[chosen[i]].frame-original[chosen[i-1]].frame) /
                double(key.frame-track.keys[i-1].frame);
            if (i+1 < track.keys.size()) key.outSlope *= double(original[chosen[i+1]].frame-original[chosen[i]].frame) /
                double(track.keys[i+1].frame-key.frame);
            if (scale != 1 && key.tangent == ac::Tangent::Automatic) key.tangent = ac::Tangent::Free;
            if (i) {
                auto& previous = track.keys[i-1];
                const double sum = previous.outWeight + key.inWeight;
                if (sum > 0.9999) {
                    previous.outWeight = std::clamp(previous.outWeight * 0.9998 / sum, 0.0001, 0.9997);
                    key.inWeight = 0.9998 - previous.outWeight;
                }
            }
        }
        if (!ac::Validate(track, error)) return;
    }
    ++state->deferredEdits; impl_->active = true; impl_->ready = true;
}
RangeRetime::~RangeRetime() {
    if (impl_ && impl_->active) if (auto* state = Current(impl_->model)) {
        --state->deferredEdits;
        // A partially failed native operation is reconciled against its actual
        // pool; never publish a prepared curve for keys that were not written.
        Synchronize(impl_->model);
    }
}
bool RangeRetime::Ready() const { return impl_->ready; }
bool RangeRetime::Complete() {
    if (!impl_->ready) return false;
    if (!impl_->active) return true;
    auto* state = Current(impl_->model); if (!state) return false;
    for (auto& pair : impl_->curves) {
        pair.second.baseline.clear();
        if (!Snapshot(impl_->model, pair.first, pair.second.baseline)) return false;
        pair.second.nativeHash = Hash(impl_->model, pair.first);
        if (!pair.second.nativeHash) return false;
        pair.second.registrations.clear();
    }
    state->curves = std::move(impl_->curves);
    --state->deferredEdits; impl_->active = false;
    return true;
}
bool Read(const unsigned char* model, int bone, Tracks& out) {
    auto* entry = Current(model, bone); if (!entry) return false;
    out = entry->tracks; return true;
}
bool Has(const unsigned char* model, int bone) { return Current(model, bone) != nullptr; }
void Synchronize(const unsigned char* model) {
    if(auto* state=Current(model))for(auto& pair:state->curves)Reconcile(model,pair.first,pair.second);
}
void ReconcileRegisteredFrame(const unsigned char* model,int bone,int frame) {
    auto* state=Current(model);if(!state||frame<0)return;
    auto found=state->curves.find(bone);if(found!=state->curves.end())found->second.registrations.insert(frame);
}
bool Write(unsigned char* model, int bone, const Tracks& tracks, std::string& error) {
    error.clear();
    std::vector<unsigned> oldSlots;
    if (!Chain(model, bone, oldSlots)) { error="Invalid bone key chain."; return false; }
    std::set<int> frames;
    for (const auto& track : tracks) {
        if (!ac::Validate(track, error)) return false;
        if (track.discrete || track.keys.empty() || track.keys.front().frame != 0 || track.keys.back().frame > mikudancestudio::frame_limits::maximum) {
            error="Bone curves must keep frame zero and use representable nonnegative frame numbers."; return false;
        }
        for (const auto& key : track.keys) {
            if (std::abs(key.value)>1e12 || std::abs(key.inSlope)>1e12 || std::abs(key.outSlope)>1e12) {
                error="Bone curve value or slope is too large."; return false;
            }
            frames.insert(key.frame);
        }
    }
    // A positive control hull also bounds weighted overshoot between keys.
    for(int c=6;c<9;++c){
        const auto& keys=tracks[c].keys;
        const auto valid=[](double v){return std::isfinite(v)&&v>=0.001&&v<=1000;};
        for(std::size_t i=0;i<keys.size();++i){
            if(!valid(keys[i].value)){error="Scale must stay inside 0.001..1000.";return false;}
            if(i&&keys[i].interpolation==ac::Interpolation::Bezier){
                const auto&a=keys[i-1];const auto&b=keys[i];const double span=double(b.frame)-a.frame;
                if(!valid(a.value+a.outSlope*a.outWeight*span)||!valid(b.value-b.inSlope*b.inWeight*span)){
                    error="Scale handles must stay inside 0.001..1000.";return false;
                }
            }
        }
    }
    try {
        auto& record = *mdl::Mdl(model);
        auto* pool = mdl::BoneKeys(model);
        auto capacity = mdl::BoneKeyCapacity(pool);
        std::vector<unsigned> slots = oldSlots;
        for (std::size_t i=record.boneCount; slots.size()<frames.size() && i<capacity; ++i)
            if (!pool[i].frame && std::find(oldSlots.begin(),oldSlots.end(),i)==oldSlots.end()) slots.push_back(unsigned(i));
        const auto missing = frames.size()>slots.size() ? frames.size()-slots.size() : 0;
        if (missing && !mdl::BoneKeyPoolEnsure(record, capacity+missing)) { error="Cannot allocate bone keys."; return false; }
        for (std::size_t i=0; i<missing; ++i) slots.push_back(unsigned(capacity+i));
        pool = mdl::BoneKeys(model);
        std::vector<mdl::BoneKey> keys; keys.reserve(frames.size());
        auto scaleState = bone_scale::Capture(model);
        std::size_t at=0;
        for (int frame : frames) {
            while (at+1<oldSlots.size() && pool[oldSlots[at+1]].frame<=unsigned(frame)) ++at;
            mdl::BoneKey key = pool[oldSlots[at]];
            key.frame=unsigned(frame); key.allocated=1;
            key.previous=keys.empty()?0:slots[keys.size()-1];
            key.next=keys.size()+1<frames.size()?slots[keys.size()+1]:0;
            for (int c=0;c<3;++c) key.position[c]=float(ac::Evaluate(tracks[c],frame).value);
            Quaternion(tracks,frame,key.rotation);
            // Compatibility mirror. Exact fractional evaluation stays in tracks.
            for(int c=0;c<4;++c) for(int k=0;k<4;++k) key.interpolation[c+k*4]=k<2?20:107;
            auto scale=Scale(tracks,frame,bone_scale::Sample(model,bone,frame));
            if(!bone_scale::Valid(scale)){error="Invalid bone scale.";return false;}
            scaleState.keys[int(slots[keys.size()])]=scale;
            scaleState.curves.erase(int(slots[keys.size()]));
            keys.push_back(key);
        }
        for(std::size_t i=frames.size();i<oldSlots.size();++i){scaleState.keys.erase(int(oldSlots[i]));scaleState.curves.erase(int(oldSlots[i]));}
        auto state = Current(model) ? *Current(model) : State{};
        state.identity=record.boneTable;
        Entry authored;authored.tracks=tracks;authored.baseline.reserve(keys.size());
        for(std::size_t i=0;i<keys.size();++i)
            authored.baseline.push_back({slots[i],keys[i],scaleState.keys.at(int(slots[i])),bone_scale::LinearCurve()});
        state.curves[bone]=std::move(authored);
        // Prepare all allocating work before publishing native keys and curves.
        auto& target=states[model];
        if(!bone_scale::Restore(model,std::move(scaleState))){error="Cannot store bone scale.";return false;}
        for(std::size_t i=0;i<keys.size();++i)pool[slots[i]]=keys[i];
        for(std::size_t i=keys.size();i<oldSlots.size();++i)pool[oldSlots[i]]={};
        record.searchCursor=int(record.boneCount);
        record.maxFrame=std::max(record.maxFrame,static_cast<std::uint32_t>(*frames.rbegin()));
        if(record.boneKeyCursors)record.boneKeyCursors[bone]=unsigned(bone);
        if(record.boneTrackActive)record.boneTrackActive[bone]=1;
        target=std::move(state);
        target.curves[bone].nativeHash=Hash(model,bone);
        return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
void Evaluate(unsigned char* model, double frame) {
    if(!model||!std::isfinite(frame)||!mdl::Bones(model))return;
    auto* state=Current(model);if(!state)return;
    for(auto& pair:state->curves){
        if(!Reconcile(model,pair.first,pair.second))continue;
        auto& bone=mdl::Bones(model)[pair.first];const auto& tracks=pair.second.tracks;
        for(int c=0;c<3;++c)bone.trans[c]=float(ac::Evaluate(tracks[c],frame).value);
        Quaternion(tracks,frame,bone.rotQuat);
        bone_scale::SetPose(model,pair.first,Scale(tracks,frame,bone_scale::Pose(model,pair.first)));
    }
}
void Forget(unsigned char* model){states.erase(model);}
void Reset(){states.clear();}
void CaptureUndo(unsigned char* model,unsigned ring){
    if(!model||ring>=30)return;
    Synchronize(model);
    auto& s=states[model];s.identity=mdl::Mdl(model)->boneTable;s.undo[ring]=s.curves;s.undoValid[ring]=true;s.redoValid[ring]=false;
}
void Undo(unsigned char* model,unsigned ring){auto* s=Current(model);if(s&&ring<30&&s->undoValid[ring]){s->redo[ring]=s->curves;s->redoValid[ring]=true;s->curves=s->undo[ring];}}
void Redo(unsigned char* model,unsigned ring){auto* s=Current(model);if(s&&ring<30&&s->redoValid[ring])s->curves=s->redo[ring];}
void CopyState(const unsigned char* source,unsigned char* destination){
    Synchronize(source);Forget(destination);auto* s=Current(source);if(!s||!destination)return;
    State copy;copy.identity=mdl::Mdl(destination)->boneTable;copy.curves=s->curves;
    for(auto& pair:copy.curves){pair.second.nativeHash=Hash(destination,pair.first);pair.second.baseline.clear();Snapshot(destination,pair.first,pair.second.baseline);}
    states[destination]=std::move(copy);
}
void ExchangeState(unsigned char* first,unsigned char* second){
    std::swap(states[first],states[second]);
    states[first].identity=mdl::Mdl(first)->boneTable;states[second].identity=mdl::Mdl(second)->boneTable;
}
void CapturePmm(const MMDApp& app,pmm_extension::Document& document){
    for(int slot=0;slot<255;++slot){
        auto* model=app.ModelSlot(slot);auto* s=Current(model);std::vector<ac::NamedTrack> tracks;
        if(s){std::vector<int> bones;for(const auto& pair:s->curves)bones.push_back(pair.first);
            for(int bone:bones)if(auto* entry=Current(model,bone))for(int c=0;c<9;++c)tracks.push_back({StableId(model,bone,c),entry->tracks[c]});}
        if(tracks.empty()){pmm_extension::Erase(document,PmmKey(slot));continue;}
        std::vector<std::uint8_t> bytes;std::string error;
        if(!ac::Encode(tracks,bytes,error))throw std::runtime_error(error);
        pmm_extension::Set(document,PmmKey(slot),std::move(bytes));
    }
}
void LoadPmm(const MMDApp& app,const pmm_extension::Document& document){
    for(int slot=0;slot<255;++slot){
        auto* model=app.ModelSlot(slot);const auto* payload=pmm_extension::Find(document,PmmKey(slot));
        if(!model||!payload)continue;
        std::vector<ac::NamedTrack> tracks;std::string error;
        if(!ac::Decode(payload->value.data(),payload->value.size(),tracks,error))continue;
        std::map<std::string,const ac::Track*> byId;for(const auto& track:tracks)byId.emplace(track.id,&track.track);
        State state;state.identity=mdl::Mdl(model)->boneTable;
        for(unsigned bone=0;bone<mdl::Mdl(model)->boneCount;++bone){
            Entry entry;bool complete=true;
            for(int c=0;c<9;++c){auto it=byId.find(StableId(model,bone,c));if(it==byId.end()||it->second->keys.empty()||it->second->keys.front().frame!=0){complete=false;break;}entry.tracks[c]=*it->second;}
            if(complete){entry.nativeHash=Hash(model,bone);if(entry.nativeHash&&Snapshot(model,bone,entry.baseline))state.curves[int(bone)]=std::move(entry);}
        }
        states[model]=std::move(state);
    }
}
bool DrawPreview(MMDApp& app){
    auto* model=app.SelectedModel();if(!model)return false;
    const int bone=mdl::Mdl(model)->selectedBone;auto* entry=Current(model,bone);if(!entry)return false;
    const int channel=int(SendMessageW(GetDlgItem(app.MainWindow(),panel::kInterpCurveCombo),CB_GETCURSEL,0,0));
    const int first=channel<3?std::max(0,channel):channel==4?6:3;
    const int count=channel>=3?3:1;
    HDC dc=app.CurveDC();auto pen=CreatePen(PS_SOLID,1,RGB(app.UiTextRed(),app.UiTextGreen(),app.UiTextBlue()));
    auto old=SelectObject(dc,pen);
    for(int c=first;c<first+count;++c){const auto& track=entry->tracks[c];if(track.keys.size()<2)continue;
        auto at=std::lower_bound(track.keys.begin()+1,track.keys.end(),app.CurrentFrame(),[](const ac::Key& k,int f){return k.frame<f;});
        if(at==track.keys.end())at=track.keys.end()-1;
        const double start=(at-1)->frame,end=at->frame;std::array<double,128> values{};
        double low=1e300,high=-1e300;
        for(int x=0;x<128;++x){values[x]=ac::Evaluate(track,start+(end-start)*x/127).value;low=std::min(low,values[x]);high=std::max(high,values[x]);}
        for(int x=0;x<128;++x){const int y=high-low>1e-12?127-int(127*(values[x]-low)/(high-low)):64;if(x)LineTo(dc,x,y);else MoveToEx(dc,x,y,nullptr);}
    }
    SelectObject(dc,old);DeleteObject(pen);return true;
}
std::size_t VmdBlockSize(const std::uint8_t* data,std::size_t size){
    if(!data||size<12||(std::memcmp(data,"MMSBCRV1",8)&&std::memcmp(data,"MDSBCRV1",8)))return 0;
    std::uint32_t bytes=0;for(int i=0;i<4;++i)bytes|=std::uint32_t(data[8+i])<<(8*i);
    return bytes<=16u*1024*1024&&bytes<=size-12?std::size_t(bytes)+12:0;
}
bool CaptureVmd(const unsigned char* model,int offset,std::vector<std::uint8_t>& block,std::string& error){
    block.clear();auto* state=Current(model);if(!state)return true;
    std::vector<ac::NamedTrack> tracks;std::vector<int> bones;
    for(const auto& pair:state->curves)bones.push_back(pair.first);
    for(int bone:bones){auto* entry=Current(model,bone);if(!entry)continue;
        std::vector<unsigned> slots;if(!Chain(model,bone,slots))return false;
        std::size_t selected=0;for(auto slot:slots)selected+=mdl::Mdl(model)->boneKeys[slot].allocated!=0;
        if(!selected)continue;
        if(selected!=slots.size()){error="Select all keyframes of a free-curve bone before exporting VMD.";return false;}
        for(int c=0;c<9;++c){auto track=entry->tracks[c];
            for(auto& key:track.keys){if(key.frame<offset){error="Free bone curves cannot start before the VMD export range.";return false;}key.frame-=offset;}
            tracks.push_back({StableId(model,bone,c),std::move(track)});
        }
    }
    if(tracks.empty())return true;
    std::vector<std::uint8_t> payload;if(!ac::Encode(tracks,payload,error))return false;
    if(payload.size()>16u*1024*1024){error="Bone curve extension is too large.";return false;}
    block.assign({'M','M','S','B','C','R','V','1'});
    for(int i=0;i<4;++i)block.push_back(std::uint8_t(payload.size()>>(8*i)));
    block.insert(block.end(),payload.begin(),payload.end());return true;
}
bool ApplyVmd(unsigned char* model,const std::uint8_t* data,std::size_t size,int offset,std::string& error){
    if(!model||offset<0||VmdBlockSize(data,size)!=size){error="Invalid bone curve extension.";return false;}
    std::vector<ac::NamedTrack> decoded;if(!ac::Decode(data+12,size-12,decoded,error))return false;
    std::map<std::string,const ac::Track*> byName;
    std::map<std::pair<int,int>,const ac::Track*> byIndex;
    const auto* unicodeTargets=extended_names::importedBoneTargets;
    for(const auto& item:decoded){
        const auto lane=item.id.find('/',5);
        if(item.id.rfind("bone/",0)!=0||lane==std::string::npos)continue;
        if(unicodeTargets){
            const auto channelEnd=item.id.find('/',lane+1);
            if(channelEnd==std::string::npos)continue;
            unsigned source=0;int channel=0;
            const auto owner=std::from_chars(item.id.data()+5,item.id.data()+lane,source);
            const auto parsed=std::from_chars(item.id.data()+lane+1,item.id.data()+channelEnd,channel);
            if(owner.ec!=std::errc{}||owner.ptr!=item.id.data()+lane||parsed.ec!=std::errc{}||parsed.ptr!=item.id.data()+channelEnd||source>=unicodeTargets->size()||channel<0||channel>=9)continue;
            const int target=(*unicodeTargets)[source];
            if(target>=0)byIndex.emplace(std::make_pair(target,channel),&item.track);
        }else byName.emplace(item.id.substr(lane+1),&item.track);
    }
    std::vector<std::pair<int,Tracks>> staged;
    for(unsigned bone=0;bone<mdl::Mdl(model)->boneCount;++bone){
        Tracks tracks;bool complete=true;
        for(int c=0;c<9;++c){auto id=StableId(model,bone,c);id.erase(0,id.find('/',5)+1);
            const ac::Track* source=nullptr;
            if(unicodeTargets){auto it=byIndex.find({int(bone),c});if(it!=byIndex.end())source=it->second;}
            else{auto it=byName.find(id);if(it!=byName.end())source=it->second;}
            if(!source){complete=false;break;}tracks[c]=*source;
            for(auto& key:tracks[c].keys){if(key.frame<0||key.frame>mikudancestudio::frame_limits::maximum-offset){error="Bone curve frame is out of range.";return false;}key.frame+=offset;}
            if(tracks[c].keys.empty()){error="Empty bone curve.";return false;}
            if(tracks[c].keys.front().frame>0){auto anchor=tracks[c].keys.front();anchor.frame=0;anchor.inSlope=anchor.outSlope=0;tracks[c].keys.front().interpolation=ac::Interpolation::Step;tracks[c].keys.insert(tracks[c].keys.begin(),anchor);}
        }
        if(!complete)continue;
        std::set<int> frames;for(const auto& track:tracks)for(const auto& key:track.keys)frames.insert(key.frame);
        std::vector<unsigned> slots;if(!Chain(model,bone,slots))return false;
        // A full free-track import must not delete unrelated native motion.
        for(auto slot:slots)if(!frames.count(int(mdl::BoneKeys(model)[slot].frame))){error="The free-curve VMD overlaps existing bone motion. Import it into an empty bone track.";return false;}
        staged.emplace_back(int(bone),std::move(tracks));
    }
    for(const auto& pair:staged)if(!Write(model,pair.first,pair.second,error))return false;
    return true;
}
}
