#include "mikudancestudio/extended_names.hpp"
#include <charconv>
#include "mikudancestudio/frame_limits.hpp"
#define NOMINMAX
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/native_edit_history.hpp"
#include "../plugins/scene_resource_budget.hpp"
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
struct Entry { Tracks tracks; std::uint64_t nativeHash = 0; };
using Curves = std::map<int, Entry>;
struct State {
    const void* identity = nullptr;
    Curves curves;
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
Entry* Current(const unsigned char* model, int bone) {
    auto* state = Current(model);
    if (!state) return nullptr;
    auto it = state->curves.find(bone);
    if (it == state->curves.end()) return nullptr;
    // Native registration/deletion/retiming takes precedence over an old mirror.
    if (it->second.nativeHash != Hash(model, bone)) {
        state->curves.erase(it); return nullptr;
    }
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
std::string StableId(const unsigned char* model, int bone, int channel) {
    std::string id = "bone/" + std::to_string(bone) + "/" + std::to_string(channel) + "/";
    const auto* name = mdl::Mdl(model)->boneTable[bone].name;
    constexpr char hex[] = "0123456789abcdef";
    for (int i=0; i<20; ++i) { const auto b=static_cast<unsigned char>(name[i]); id+=hex[b>>4]; id+=hex[b&15]; }
    return id;
}
std::string PmmKey(int slot) { return "org.mikudancestudio.model." + std::to_string(slot) + ".boneCurve.v1"; }
}
bool Read(const unsigned char* model, int bone, Tracks& out) {
    auto* entry = Current(model, bone); if (!entry) return false;
    out = entry->tracks; return true;
}
bool Has(const unsigned char* model, int bone) { return Current(model, bone) != nullptr; }
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
        state.curves[bone]={tracks,0};
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
    for(auto it=state->curves.begin();it!=state->curves.end();){
        if(it->second.nativeHash!=Hash(model,it->first)){it=state->curves.erase(it);continue;}
        auto& bone=mdl::Bones(model)[it->first];const auto& tracks=it->second.tracks;
        for(int c=0;c<3;++c)bone.trans[c]=float(ac::Evaluate(tracks[c],frame).value);
        Quaternion(tracks,frame,bone.rotQuat);
        bone_scale::SetPose(model,it->first,Scale(tracks,frame,bone_scale::Pose(model,it->first)));
        ++it;
    }
}
void Forget(unsigned char* model){states.erase(model);}
void Reset(){states.clear();}
void CaptureUndo(unsigned char* model,unsigned ring){
    if(!model||ring>=30)return;
    auto& s=states[model];s.identity=mdl::Mdl(model)->boneTable;s.undo[ring]=s.curves;s.undoValid[ring]=true;s.redoValid[ring]=false;
}
void Undo(unsigned char* model,unsigned ring){auto* s=Current(model);if(s&&ring<30&&s->undoValid[ring]){s->redo[ring]=s->curves;s->redoValid[ring]=true;s->curves=s->undo[ring];}}
void Redo(unsigned char* model,unsigned ring){auto* s=Current(model);if(s&&ring<30&&s->redoValid[ring])s->curves=s->redo[ring];}
namespace {
bool SameCurves(const Curves& a,const Curves& b) noexcept {
    if(a.size()!=b.size())return false;
    auto right=b.begin();
    for(const auto& left:a){
        if(left.first!=right->first||left.second.nativeHash!=right->second.nativeHash)return false;
        for(unsigned c=0;c<9;++c){
            const auto& x=left.second.tracks[c];const auto& y=right->second.tracks[c];
            if(x.discrete!=y.discrete||x.keys.size()!=y.keys.size())return false;
            for(std::size_t i=0;i<x.keys.size();++i){const auto& p=x.keys[i];const auto& q=y.keys[i];
                if(p.frame!=q.frame||p.value!=q.value||p.inSlope!=q.inSlope||p.outSlope!=q.outSlope||
                   p.inWeight!=q.inWeight||p.outWeight!=q.outWeight||p.interpolation!=q.interpolation||p.tangent!=q.tangent)return false;
            }
        }
        ++right;
    }
    return true;
}
bool SameHistory(const State& a,const State& b) noexcept {
    if(a.identity!=b.identity||a.undoValid!=b.undoValid||a.redoValid!=b.redoValid||!SameCurves(a.curves,b.curves))return false;
    for(unsigned i=0;i<30;++i)if(!SameCurves(a.undo[i],b.undo[i])||!SameCurves(a.redo[i],b.redo[i]))return false;
    return true;
}
class HistoryState final:public native_history::SideState {
    unsigned char* model_;
    bool present_;
    State before_,after_;
public:
    HistoryState(unsigned char* model,const std::vector<native_history::RingStep>& steps):model_(model),present_(states.find(model)!=states.end()) {
        if(!present_)return;
        before_=states.find(model)->second;after_=before_;
        if(after_.identity!=mdl::Mdl(model)->boneTable)return;
        for(const auto& step:steps){const auto ring=step.slot;
            if(step.redo){if(after_.redoValid[ring])after_.curves=after_.redo[ring];}
            else if(after_.undoValid[ring]){after_.redo[ring]=after_.curves;after_.redoValid[ring]=true;after_.curves=after_.undo[ring];}
        }
    }
    bool IsCurrent() const noexcept override {
        const auto it=states.find(model_);
        return present_?it!=states.end()&&SameHistory(it->second,before_):it==states.end();
    }
    void Publish() noexcept override {
        if(!present_)return;
        auto& live=states.find(model_)->second;
        live.curves.swap(after_.curves);
        for(unsigned i=0;i<30;++i){live.undo[i].swap(after_.undo[i]);live.redo[i].swap(after_.redo[i]);}
        live.undoValid.swap(after_.undoValid);live.redoValid.swap(after_.redoValid);std::swap(live.identity,after_.identity);
    }
};
}
std::unique_ptr<native_history::SideState> PrepareHistory(unsigned char* model,const std::vector<native_history::RingStep>& steps) {
    return std::make_unique<HistoryState>(model,steps);
}
#if !MMD_ORIGINAL_PROFILE
struct AuthoringState::Impl {
    const unsigned char* model=nullptr;
    const void* bones=nullptr;
    std::uint32_t boneCount=0;
    const void* storedIdentity=nullptr;
    Curves curves;
    bool SameOwner(const unsigned char* value) const noexcept {
        return value&&value==model&&mdl::Mdl(value)->boneTable==bones&&mdl::Mdl(value)->boneCount==boneCount;
    }
    bool MatchesLive() const noexcept {
        if(!SameOwner(model))return false;
        const auto found=states.find(model);
        if(found==states.end())return curves.empty();
        // An empty storage entry can have been created solely for legacy
        // undo. Its identity does not constitute authored free-curve data.
        if(curves.empty())return found->second.curves.empty();
        return storedIdentity==found->second.identity&&SameCurves(curves,found->second.curves);
    }
};
AuthoringState::AuthoringState()=default;
AuthoringState::~AuthoringState()=default;
AuthoringState::AuthoringState(AuthoringState&&) noexcept=default;
AuthoringState& AuthoringState::operator=(AuthoringState&&) noexcept=default;
std::unique_ptr<AuthoringState> AuthoringState::Capture(const unsigned char* model) {
    if(!model)throw std::invalid_argument("bone curve authoring model");
    auto result=std::unique_ptr<AuthoringState>(new AuthoringState);
    result->impl_=std::make_unique<Impl>();auto& value=*result->impl_;
    value.model=model;value.bones=mdl::Mdl(model)->boneTable;value.boneCount=mdl::Mdl(model)->boneCount;
    if(value.boneCount&&!value.bones)throw std::invalid_argument("bone curve authoring bones");
    const auto found=states.find(model);
    if(found!=states.end()) {
        value.curves=found->second.curves;
        if(!value.curves.empty())value.storedIdentity=found->second.identity;
    }
    return result;
}
std::unique_ptr<AuthoringState> AuthoringState::Clone() const {
    if(!impl_)throw std::invalid_argument("bone curve authoring state");
    auto result=std::unique_ptr<AuthoringState>(new AuthoringState);
    result->impl_=std::make_unique<Impl>(*impl_);return result;
}
bool AuthoringState::Equals(const AuthoringState& other) const noexcept {
    if(!impl_||!other.impl_)return !impl_&&!other.impl_;
    return impl_->model==other.impl_->model&&impl_->bones==other.impl_->bones&&impl_->boneCount==other.impl_->boneCount&&
        (impl_->curves.empty()||impl_->storedIdentity==other.impl_->storedIdentity)&&SameCurves(impl_->curves,other.impl_->curves);
}
bool AuthoringState::Invalidate(int bone) noexcept {
    if(!impl_||bone<0||std::uint32_t(bone)>=impl_->boneCount)return false;
    return impl_->curves.erase(bone)!=0;
}
bool AuthoringState::Sample(int bone,double frame,std::uint64_t targetNativeHash,
    float position[3],float rotation[4],bone_scale::Matrix& scale) const noexcept {
    if(!impl_||bone<0||std::uint32_t(bone)>=impl_->boneCount||!std::isfinite(frame)||
        !position||!rotation||!targetNativeHash||impl_->storedIdentity!=impl_->bones)return false;
    const auto found=impl_->curves.find(bone);
    if(found==impl_->curves.end()||found->second.nativeHash!=targetNativeHash)return false;
    const auto& tracks=found->second.tracks;
    for(int channel=0;channel<3;++channel)position[channel]=float(ac::Evaluate(tracks[channel],frame).value);
    Quaternion(tracks,frame,rotation);scale=Scale(tracks,frame,scale);return true;
}
std::uint64_t AuthoringState::RetainedBytes() const noexcept {
    std::uint64_t bytes=sizeof(*this);if(!impl_)return bytes;
    const auto add=[&](std::uint64_t count,std::uint64_t stride) {
        if(stride&&count>(UINT64_MAX-bytes)/stride)bytes=UINT64_MAX;
        else bytes+=count*stride;
    };
    add(1,sizeof(Impl));
    // The allowance covers tree links/alignment; the extra node accounts for
    // the separately allocated sentinel used by the supported MSVC runtime.
    constexpr std::uint64_t nodeBytes=sizeof(Curves::value_type)+4*sizeof(void*);
    add(1,nodeBytes);add(impl_->curves.size(),nodeBytes);
    for(const auto& entry:impl_->curves)for(const auto& track:entry.second.tracks)add(track.keys.capacity(),sizeof(ac::Key));
    return bytes;
}
struct AuthoringRestore::Impl {
    unsigned char* model=nullptr;
    std::unique_ptr<AuthoringState> before,after;
    decltype(states)::node_type storage;
    bool published=false;
};
AuthoringRestore::AuthoringRestore()=default;
AuthoringRestore::~AuthoringRestore()=default;
AuthoringRestore::AuthoringRestore(AuthoringRestore&&) noexcept=default;
AuthoringRestore& AuthoringRestore::operator=(AuthoringRestore&&) noexcept=default;
std::unique_ptr<AuthoringRestore> AuthoringRestore::Prepare(unsigned char* model,const AuthoringState& target) {
    if(!target.impl_||!target.impl_->SameOwner(model))throw std::invalid_argument("bone curve authoring owner");
    auto result=std::unique_ptr<AuthoringRestore>(new AuthoringRestore);
    result->impl_=std::make_unique<Impl>();auto& value=*result->impl_;value.model=model;
    value.before=AuthoringState::Capture(model);value.after=target.Clone();
    if(!value.after->impl_->curves.empty()) {
        // std::map node transfer does not allocate or rehash. Stage a node even
        // if live storage exists: an independently updated empty legacy ring
        // may create/remove storage without changing logical authoring state.
        decltype(states) privateStorage;
        const auto inserted=privateStorage.try_emplace(model);
        value.storage=privateStorage.extract(inserted.first);
    }
    return result;
}
bool AuthoringRestore::IsCurrent() const noexcept {
    return impl_&&!impl_->published&&impl_->before->impl_->MatchesLive();
}
bool AuthoringRestore::Publish() noexcept {
    if(!IsCurrent())return false;
    auto& value=*impl_;auto& target=*value.after->impl_;
    const auto found=states.find(value.model);
    if(found!=states.end()) {
        found->second.curves.swap(target.curves);
        // Empty authoring has no stored identity of its own. Retain the live
        // identity so existing legacy undo snapshots remain reachable.
        if(!found->second.curves.empty())found->second.identity=target.storedIdentity;
    } else if(!target.curves.empty()) {
        auto& prepared=value.storage.mapped();prepared.identity=target.storedIdentity;
        prepared.curves.swap(target.curves);
        states.insert(std::move(value.storage));
    }
    value.published=true;return true;
}

void MeasureResources(const unsigned char* model,plugins::resource_budget::Collector& out) {
    const auto it=states.find(model);if(it==states.end())return;
    const auto& state=it->second;out.Array(&state,1);
    const auto curves=[&](const Curves& values){out.Nodes(values);for(const auto& entry:values)for(const auto& track:entry.second.tracks)out.Vector(track.keys);};
    curves(state.curves);for(const auto& value:state.undo)curves(value);for(const auto& value:state.redo)curves(value);
}
#endif
void CopyState(const unsigned char* source,unsigned char* destination){
    Forget(destination);auto* s=Current(source);if(!s||!destination)return;
    State copy;copy.identity=mdl::Mdl(destination)->boneTable;copy.curves=s->curves;
    for(auto& pair:copy.curves)pair.second.nativeHash=Hash(destination,pair.first);
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
            if(complete){entry.nativeHash=Hash(model,bone);if(entry.nativeHash)state.curves[int(bone)]=std::move(entry);}
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
    if(!data||size<12||std::memcmp(data,"MDSBCRV1",8))return 0;
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
    block.assign({'M','D','S','B','C','R','V','1'});
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
