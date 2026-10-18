#include "native_bone_curve_write.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_evaluation.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace mikudancestudio::plugins {
namespace {
using Key=mdl::BoneKey;
constexpr double pi=3.14159265358979323846;
void Charge(std::uint64_t& bytes,std::uint64_t count,std::uint64_t stride=1)noexcept{
    bytes=stride&&count>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+count*stride;
}
bool Equal(Key a,Key b,bool marks=true)noexcept{if(!marks)a.allocated=b.allocated=0;return !std::memcmp(&a,&b,sizeof a);}
std::vector<unsigned> Chain(const std::vector<Key>& keys,unsigned bone,unsigned count){
    if(bone>=count||count>keys.size())throw std::invalid_argument("bone curve chain owner");
    std::vector<unsigned> slots;unsigned index=bone,previous=0,last=0;
    for(std::size_t hop=0;hop<keys.size();++hop){
        if(index>=keys.size()||(hop&&index<count))throw std::invalid_argument("bone curve chain slot");
        const auto& key=keys[index];double norm=0;
        if(key.frame>unsigned(frame_limits::maximum)||(!hop&&key.frame)||
            (hop&&(key.frame<=last||key.previous!=previous)))throw std::invalid_argument("bone curve chain order");
        for(float value:key.position)if(!std::isfinite(value))throw std::invalid_argument("bone curve position");
        for(float value:key.rotation){if(!std::isfinite(value))throw std::invalid_argument("bone curve rotation");norm+=double(value)*value;}
        if(norm<1e-20)throw std::invalid_argument("bone curve quaternion");
        for(auto value:key.interpolation)if(value>127)throw std::invalid_argument("bone curve interpolation");
        if(hop){const auto& lower=keys[previous];
            for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(key.position[axis]-lower.position[axis]))
                throw std::invalid_argument("bone curve native position difference overflow");
            // The native shortest-path slerp forms this dot in float before
            // clamping. Finite endpoints alone do not guarantee a safe seek.
            const float dot=lower.rotation[0]*key.rotation[0]+lower.rotation[1]*key.rotation[1]+
                lower.rotation[2]*key.rotation[2]+lower.rotation[3]*key.rotation[3];
            if(!std::isfinite(dot))throw std::invalid_argument("bone curve native quaternion dot overflow");}
        slots.push_back(index);if(!key.next)return slots;previous=index;last=key.frame;index=key.next;
    }
    throw std::invalid_argument("bone curve cyclic chain");
}
void ValidateOwners(const std::vector<Key>& keys,unsigned count){
    std::vector<unsigned char> owners(keys.size());
    for(unsigned bone=0;bone<count;++bone)for(auto slot:Chain(keys,bone,count)){
        if(owners[slot])throw std::invalid_argument("shared bone curve key");owners[slot]=1;}
    for(std::size_t slot=count;slot<keys.size();++slot)if(!owners[slot]&&keys[slot].frame)
        throw std::invalid_argument("orphan bone curve key");
}
struct TargetView {
    const std::vector<Key>& keys;const bone_scale::AuthoringState& scale;
    bone_curve::NativeKeyView View()const noexcept{
        return {this,keys.size(),[](const void* p,std::uint32_t slot)noexcept->const Key*{
            const auto& keys=static_cast<const TargetView*>(p)->keys;return slot<keys.size()?&keys[slot]:nullptr;},
            [](const void* p,std::uint32_t slot,bone_scale::Matrix& value,bone_scale::Curve& curve)noexcept{
                const auto& source=*static_cast<const TargetView*>(p);if(slot>=source.keys.size())return false;
                value=source.scale.Key(int(slot));curve=source.scale.KeyCurve(int(slot));return bone_scale::Valid(value);}};
    }
};
bool SameTracks(const bone_curve::Tracks& a,const bone_curve::Tracks& b)noexcept{
    for(unsigned c=0;c<9;++c){if(a[c].discrete!=b[c].discrete||a[c].keys.size()!=b[c].keys.size())return false;
        for(std::size_t i=0;i<a[c].keys.size();++i){const auto& x=a[c].keys[i];const auto& y=b[c].keys[i];
            if(x.frame!=y.frame||x.value!=y.value||x.inSlope!=y.inSlope||x.outSlope!=y.outSlope||x.inWeight!=y.inWeight||
                x.outWeight!=y.outWeight||x.interpolation!=y.interpolation||x.tangent!=y.tangent)return false;}}
    return true;
}
bone_scale::Matrix SampleScale(const std::vector<Key>& keys,const bone_scale::AuthoringState& scale,
    const std::vector<unsigned>& slots,int frame){
    std::size_t lower=0;while(lower+1<slots.size()&&keys[slots[lower+1]].frame<=unsigned(frame))++lower;
    auto value=scale.Key(int(slots[lower]));
    if(lower+1<slots.size()){const auto a=slots[lower],b=slots[lower+1];
        const float time=float((double(frame)-keys[a].frame)/double(keys[b].frame-keys[a].frame));
        value=bone_scale::Interpolate(value,scale.Key(int(b)),bone_scale::CurveTime(scale.KeyCurve(int(b)),time));}
    return value;
}
void Quaternion(const bone_curve::Tracks& tracks,int frame,float* q){
    const double x=animation_curve::Evaluate(tracks[3],frame).value*pi/360,y=animation_curve::Evaluate(tracks[4],frame).value*pi/360,
        z=animation_curve::Evaluate(tracks[5],frame).value*pi/360;
    const double sx=std::sin(x),cx=std::cos(x),sy=std::sin(y),cy=std::cos(y),sz=std::sin(z),cz=std::cos(z);
    q[0]=float(cy*sx*cz+sy*cx*sz);q[1]=float(sy*cx*cz-cy*sx*sz);
    q[2]=float(cy*cx*sz-sy*sx*cz);q[3]=float(cy*cx*cz+sy*sx*sz);
}
bone_scale::Matrix Scale(const bone_curve::Tracks& tracks,int frame,bone_scale::Matrix matrix){
    for(unsigned row=0;row<3;++row){double norm=0;for(unsigned c=0;c<3;++c)norm+=double(matrix.v[row*3+c])*matrix.v[row*3+c];
        norm=std::sqrt(norm);const double value=animation_curve::Evaluate(tracks[6+row],frame).value;
        for(unsigned c=0;c<3;++c)matrix.v[row*3+c]=float(norm>1e-12?matrix.v[row*3+c]*value/norm:(c==row?value:0));}
    return matrix;
}
struct FrameMark {unsigned bone,frame;unsigned char selected;
    bool operator==(const FrameMark& other)const noexcept{return bone==other.bone&&frame==other.frame&&selected==other.selected;}};
std::vector<FrameMark> Marks(const std::vector<Key>& keys,unsigned count,const std::vector<unsigned>& affected){
    std::vector<FrameMark> result;for(auto bone:affected)for(auto index:Chain(keys,bone,count))
        result.push_back({bone,keys[index].frame,keys[index].allocated});return result;
}
struct DisplayValue {
    unsigned slot;mdl::DisplayKey key;std::vector<std::uint8_t> ik;std::vector<mdl::BoneReference> parents;
    bool Current(const mdl::DisplayKey& value,bool marks,bool pointers)const noexcept{
        return key.frame==value.frame&&key.previous==value.previous&&key.next==value.next&&key.visible==value.visible&&
            (!marks||key.allocated==value.allocated)&&(!pointers||(key.ikStates==value.ikStates&&key.selectorStates==value.selectorStates))&&
            !std::memcmp(key.reserved0,value.reserved0,sizeof key.reserved0)&&!std::memcmp(key.reserved1,value.reserved1,sizeof key.reserved1)&&
            (ik.empty()||(value.ikStates&&!std::memcmp(ik.data(),value.ikStates,ik.size())))&&
            (parents.empty()||(value.selectorStates&&!std::memcmp(parents.data(),value.selectorStates,parents.size()*sizeof(mdl::BoneReference))));
    }
};
std::vector<DisplayValue> DisplayChain(const mdl::ModelRecord& model){
    const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);std::vector<DisplayValue> result;
    unsigned slot=0,previous=0,last=0;
    for(std::size_t hop=0;hop<capacity;++hop){
        if(slot>=capacity)throw std::invalid_argument("bone display slot");const auto& key=model.displayKeys[slot];
        if(key.frame>unsigned(frame_limits::maximum)||(!hop&&key.frame)||(hop&&(key.frame<=last||key.previous!=previous))||
            (model.ikChainCount&&!key.ikStates)||(model.boneOrderCount&&!key.selectorStates))throw std::invalid_argument("bone display chain");
        DisplayValue value{slot,key,{},{}};
        if(model.ikChainCount)value.ik.assign(key.ikStates,key.ikStates+model.ikChainCount);
        if(model.boneOrderCount)value.parents.assign(key.selectorStates,key.selectorStates+model.boneOrderCount);
        result.push_back(std::move(value));if(!key.next)return result;previous=slot;last=key.frame;slot=key.next;
    }
    throw std::invalid_argument("bone display cycle");
}
void ValidateStorage(MMDApp& app,unsigned ownSlot,bool display){
    const auto& model=*mdl::Mdl(app.ModelSlot(ownSlot));
    struct Range{std::uintptr_t first,last;};std::vector<Range> ranges;
    const auto add=[&](const void* p,std::size_t count,std::size_t size){if(!p||!count)return;
        const auto first=reinterpret_cast<std::uintptr_t>(p);if(count>(UINTPTR_MAX-first)/size)throw std::invalid_argument("bone curve storage overflow");
        ranges.push_back({first,first+count*size});};
    add(&app.state,1,sizeof app.state);add(&model,1,sizeof model);add(model.boneTable,model.boneCount,sizeof(mdl::BoneRecord));
    add(model.morphs,model.morphCount,sizeof(mdl::MorphRecord));
    add(model.morphKeyCursors,model.morphCount,sizeof(std::uint32_t));add(model.morphTrackActive,model.morphCount,1);
    add(model.bonePhysicsState,model.boneCount,1);add(model.boneSelection,model.boneCount,1);
    add(model.boneKeyCursors,model.boneCount,sizeof(std::uint32_t));add(model.boneTrackActive,model.boneCount,1);
    add(model.boneKeys,mdl::BoneKeyCapacity(model.boneKeys),sizeof(Key));
    if(model.morphKeys&&mdl::MorphKeyPoolIdentity(model.morphKeys))add(model.morphKeys,mdl::MorphKeyCapacity(model.morphKeys),sizeof(mdl::MorphKey));
    if(model.displayKeys)add(model.displayKeys,mdl::KeyPoolCapacity(model.displayKeys),sizeof(mdl::DisplayKey));
    if(display){add(model.ikChains,model.ikChainCount,sizeof(mdl::IkChain));add(model.boneOrderTable,model.boneOrderCount,sizeof(mdl::BoneOrderEntry));
        if(model.ikChains)for(unsigned i=0;i<model.ikChainCount;++i)add(model.ikChains[i].links,model.ikChains[i].linkCount,sizeof(std::uint16_t));
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(model.displayKeys);++i){add(model.displayKeys[i].ikStates,model.ikChainCount,1);
            add(model.displayKeys[i].selectorStates,model.boneOrderCount,sizeof(mdl::BoneReference));}}
    std::sort(ranges.begin(),ranges.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    for(std::size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].last)throw std::invalid_argument("aliased bone curve storage");
    const auto peer=[&](const void* p,std::size_t count,std::size_t size){if(!p||!count)return;
        const auto first=reinterpret_cast<std::uintptr_t>(p);if(count>(UINTPTR_MAX-first)/size)throw std::invalid_argument("peer bone curve storage overflow");
        const auto last=first+count*size;
        const auto at=std::lower_bound(ranges.begin(),ranges.end(),first,[](const Range& range,std::uintptr_t value){return range.last<=value;});
        if(at!=ranges.end()&&at->first<last)throw std::invalid_argument("aliased peer model curve storage");};
    for(unsigned slot=0;slot<kModelSlotCount;++slot)if(slot!=ownSlot&&app.ModelSlot(slot)){
        const auto& other=*mdl::Mdl(app.ModelSlot(slot));peer(&other,1,sizeof other);peer(other.boneTable,other.boneCount,sizeof(mdl::BoneRecord));
        peer(other.morphs,other.morphCount,sizeof(mdl::MorphRecord));peer(other.morphKeyCursors,other.morphCount,sizeof(std::uint32_t));peer(other.morphTrackActive,other.morphCount,1);
        peer(other.bonePhysicsState,other.boneCount,1);peer(other.boneSelection,other.boneCount,1);
        peer(other.boneKeyCursors,other.boneCount,sizeof(std::uint32_t));peer(other.boneTrackActive,other.boneCount,1);
        if(other.boneKeys)peer(other.boneKeys,mdl::BoneKeyCapacity(other.boneKeys),sizeof(Key));
        if(other.morphKeys&&mdl::MorphKeyPoolIdentity(other.morphKeys))peer(other.morphKeys,mdl::MorphKeyCapacity(other.morphKeys),sizeof(mdl::MorphKey));
        if(other.displayKeys&&mdl::KeyPoolIdentity(other.displayKeys)){
            const auto capacity=mdl::KeyPoolCapacity(other.displayKeys);peer(other.displayKeys,capacity,sizeof(mdl::DisplayKey));
            if(display)for(std::size_t i=0;i<capacity;++i){peer(other.displayKeys[i].ikStates,other.ikChainCount,1);
                peer(other.displayKeys[i].selectorStates,other.boneOrderCount,sizeof(mdl::BoneReference));}}
        peer(other.ikChains,other.ikChainCount,sizeof(mdl::IkChain));peer(other.boneOrderTable,other.boneOrderCount,sizeof(mdl::BoneOrderEntry));
        if(display&&other.ikChains)for(unsigned i=0;i<other.ikChainCount;++i)peer(other.ikChains[i].links,other.ikChains[i].linkCount,sizeof(std::uint16_t));
    }
}
struct BoneCurveChange final:HistoryChange,std::enable_shared_from_this<BoneCurveChange>{
    MMDApp& app;unsigned slot;unsigned char* model;std::shared_ptr<RetainedSceneObject> owner;
    mdl::BoneRecord* bones;unsigned count;std::uint64_t identity,generation=1,initialObjectGeneration;
    int initialFrame,searchBefore,searchAfter;std::uint32_t maxBefore,maxAfter,lastBefore,lastAfter;
    bool published=false,preserveSelection=false,metadata=false,displayChanged=false,visibleOwned=false;
    std::vector<unsigned> affected,mirrored,patches;std::vector<Key> before,after;
    std::vector<FrameMark> beforeMarks,afterMarks;
    bone_scale::AuthoringState scaleBefore,scaleAfter;
    std::unique_ptr<bone_curve::AuthoringState> curveBefore,curveAfter;
    std::vector<std::uint32_t> cursorBefore;std::vector<unsigned char> activeBefore;
    std::uint32_t* cursors;unsigned char* active;
    std::vector<DisplayValue> displayBefore,displayAfter;std::vector<unsigned char> ikOwned;
    std::uint64_t displayIdentity=0;unsigned ikCount,parentCount;mdl::IkChain* ikTable;void* parentTable;
    explicit BoneCurveChange(MMDApp& a,unsigned s):app(a),slot(s),model(a.ModelSlot(s)),owner(RetainSceneObject(a,MMS_OBJECT_MODEL,s)),
        bones(mdl::Mdl(model)->boneTable),count(mdl::Mdl(model)->boneCount),identity(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),
        initialObjectGeneration(SceneObjectIdentity(model).generation),initialFrame(a.CurrentFrame()),searchBefore(mdl::Mdl(model)->searchCursor),searchAfter(searchBefore),
        maxBefore(mdl::Mdl(model)->maxFrame),maxAfter(maxBefore),lastBefore(a.LastRegisteredFrame()),lastAfter(lastBefore),
        before(mdl::Mdl(model)->boneKeys,mdl::Mdl(model)->boneKeys+mdl::BoneKeyCapacity(mdl::Mdl(model)->boneKeys)),after(before),
        scaleBefore(bone_scale::AuthoringState::Capture(model)),scaleAfter(scaleBefore.Clone()),curveBefore(bone_curve::AuthoringState::Capture(model)),curveAfter(curveBefore->Clone()),
        cursors(mdl::Mdl(model)->boneKeyCursors),active(mdl::Mdl(model)->boneTrackActive),ikCount(mdl::Mdl(model)->ikChainCount),
        parentCount(mdl::Mdl(model)->boneOrderCount),ikTable(mdl::Mdl(model)->ikChains),parentTable(mdl::Mdl(model)->boneOrderTable){
        if(cursors)cursorBefore.assign(cursors,cursors+count);if(active)activeBefore.assign(active,active+count);
    }
    bool Structure()const noexcept{
        if(!owner->Present()||app.ModelSlot(slot)!=model||!SceneObjectPresent(model,owner->Id()))return false;
        const auto& m=*mdl::Mdl(model);
        return m.boneTable==bones&&m.boneCount==count&&m.bonePhysicsState&&mdl::BoneKeyPoolIdentity(m.boneKeys)==identity&&
            mdl::BoneKeyCapacity(m.boneKeys)>=before.size()&&m.boneKeyCursors==cursors&&m.boneTrackActive==active&&
            (!displayChanged||(mdl::KeyPoolIdentity(m.displayKeys)==displayIdentity&&unsigned(m.ikChainCount)==ikCount&&m.ikChains==ikTable&&
                m.boneOrderCount==parentCount&&m.boneOrderTable==parentTable));
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<BoneCurveChange> source;bool forward,preserveSelection=false,consumed=false;std::uint64_t generation;
        int frame,search;unsigned max,last;Key* pool;std::vector<Key> current,target;
        std::unique_ptr<mdl::BoneKeyPoolGrowth> growth;std::unique_ptr<bone_scale::AuthoringRestore> scale;
        std::unique_ptr<bone_curve::AuthoringRestore> curves;std::unique_ptr<PreparedHistoryChange> evaluation;
        std::vector<std::uint32_t> currentCursors,targetCursors;std::vector<unsigned char> currentActive,targetActive;
        mdl::DisplayKey* display=nullptr;std::size_t displayCapacity=0;std::vector<DisplayValue> displayCurrent;
        std::vector<mdl::DisplayKey> displayHeaders;std::vector<unsigned char> ikBefore,ikAfter;unsigned char visibleBefore=0,visibleAfter=0;
        std::uint64_t objectGeneration;
        Candidate(std::shared_ptr<BoneCurveChange> s,bool f):source(std::move(s)),forward(f),generation(source->generation),frame(source->app.CurrentFrame()),
            search(mdl::Mdl(source->model)->searchCursor),max(mdl::Mdl(source->model)->maxFrame),last(source->app.LastRegisteredFrame()),
            pool(mdl::Mdl(source->model)->boneKeys),current(pool,pool+mdl::BoneKeyCapacity(pool)),target(current),objectGeneration(SceneObjectIdentity(source->model).generation){
            if(source->cursors)currentCursors.assign(source->cursors,source->cursors+source->count);
            if(source->active)currentActive.assign(source->active,source->active+source->count);
            targetCursors=currentCursors;targetActive=currentActive;
        }
        bool IsCurrent()const noexcept override{
            if(consumed||source->generation!=generation||!source->Structure()||source->app.CurrentFrame()!=frame||
                SceneObjectIdentity(source->model).generation!=objectGeneration||!growth||!growth->IsCurrent()||!scale||!scale->IsCurrent()||!curves||!curves->IsCurrent())return false;
            const auto& m=*mdl::Mdl(source->model);
            if(m.boneKeys!=pool||mdl::BoneKeyCapacity(pool)!=current.size()||std::memcmp(pool,current.data(),current.size()*sizeof(Key))||
                m.searchCursor!=search||m.maxFrame!=max||source->app.LastRegisteredFrame()!=last||
                (source->cursors&&std::memcmp(source->cursors,currentCursors.data(),source->count*sizeof(std::uint32_t)))||
                (source->active&&std::memcmp(source->active,currentActive.data(),source->count)))return false;
            if(source->displayChanged){
                if(m.displayKeys!=display||mdl::KeyPoolCapacity(display)!=displayCapacity||m.loadComplete!=visibleBefore||
                    std::memcmp(display,displayHeaders.data(),displayCapacity*sizeof(mdl::DisplayKey)))return false;
                for(const auto& value:displayCurrent)if(!value.Current(display[value.slot],true,true))return false;
                for(unsigned i=0;i<source->ikCount;++i)if(source->ikTable[i].enabled!=ikBefore[i])return false;
            }
            return !evaluation||evaluation->IsCurrent();
        }
        void Publish()noexcept override{
            if(!IsCurrent()||!growth->Publish())std::terminate();auto& m=*mdl::Mdl(source->model);
            for(auto index:source->patches)m.boneKeys[index]=target[index];
            if(!scale->Publish()||!curves->Publish())std::terminate();if(evaluation)evaluation->Publish();
            if(source->metadata){m.searchCursor=forward?source->searchAfter:source->searchBefore;
                if(source->maxBefore!=source->maxAfter)m.maxFrame=forward?source->maxAfter:source->maxBefore;
                if(source->lastBefore!=source->lastAfter)source->app.LastRegisteredFrame()=forward?source->lastAfter:source->lastBefore;}
            for(auto bone:source->mirrored){if(source->cursors)source->cursors[bone]=targetCursors[bone];if(source->active)source->active[bone]=targetActive[bone];}
            if(source->displayChanged){const auto& targetDisplay=forward?source->displayAfter:source->displayBefore;
                for(const auto& value:targetDisplay){auto& key=m.displayKeys[value.slot];key.visible=value.key.visible;
                    if(source->ikCount)std::memcpy(key.ikStates,value.ik.data(),source->ikCount);}
                if(source->visibleOwned)m.loadComplete=visibleAfter;
                for(unsigned i=0;i<source->ikCount;++i)if(source->ikOwned[i])source->ikTable[i].enabled=ikAfter[i];}
            if(!growth->Accept())std::terminate();source->published=true;source->preserveSelection=preserveSelection;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();if(!Structure()||generation==UINT64_MAX)return MMS_E_STALE;
        return Boundary([&]()->MmsResult{
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            ValidateStorage(app,slot,displayChanged);
            auto& m=*mdl::Mdl(model);
            if(!published&&(!forward||app.CurrentFrame()!=initialFrame||SceneObjectIdentity(model).generation!=initialObjectGeneration||
                mdl::BoneKeyCapacity(m.boneKeys)!=before.size()||std::memcmp(m.boneKeys,before.data(),before.size()*sizeof(Key))))return MMS_E_STALE;
            if(metadata&&(m.searchCursor!=(forward?searchBefore:searchAfter)||
                (maxBefore!=maxAfter&&m.maxFrame!=(forward?maxBefore:maxAfter))||
                (lastBefore!=lastAfter&&app.LastRegisteredFrame()!=(forward?lastBefore:lastAfter))))return MMS_E_STALE;
            if(!bone_scale::AuthoringState::Capture(model).Equals(forward?scaleBefore:scaleAfter)||
                !bone_curve::AuthoringState::Capture(model)->Equals(forward?*curveBefore:*curveAfter))return MMS_E_STALE;
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);ValidateOwners(candidate->current,count);
            candidate->target.resize((std::max)(candidate->target.size(),after.size()));
            for(auto index:patches){const auto live=index<candidate->current.size()?candidate->current[index]:Key{};
                const auto& expected=forward?before:after;const auto value=index<expected.size()?expected[index]:Key{};
                if(!Equal(live,value,!published))return MMS_E_STALE;
                const auto& values=forward?after:before;candidate->target[index]=index<values.size()?values[index]:Key{};}
            ValidateOwners(candidate->target,count);
            if(published){const auto marks=Marks(candidate->current,count,affected);
                candidate->preserveSelection=preserveSelection||marks!=(forward?beforeMarks:afterMarks);
                if(candidate->preserveSelection)for(auto bone:affected)for(auto index:Chain(candidate->target,bone,count)){
                    const auto frame=candidate->target[index].frame;
                    const auto at=std::lower_bound(marks.begin(),marks.end(),std::pair<unsigned,unsigned>{bone,frame},
                        [](const FrameMark& mark,const auto& value){return std::pair<unsigned,unsigned>{mark.bone,mark.frame}<value;});
                    candidate->target[index].allocated=at!=marks.end()&&at->bone==bone&&at->frame==frame?at->selected:0;}}
            if(!mdl::BoneKeyPoolGrowth::Prepare(m,candidate->target.size(),candidate->growth))return MMS_E_MEMORY;
            candidate->scale=bone_scale::AuthoringRestore::Prepare(model,forward?scaleAfter:scaleBefore,candidate->growth->CandidateCapacity());
            candidate->curves=bone_curve::AuthoringRestore::Prepare(model,forward?*curveAfter:*curveBefore);
            if(!affected.empty()){
                BoneEvaluationView view;view.context=&candidate->target;view.keyCapacity=candidate->target.size();
                view.key=[](const void* p,std::uint32_t index)noexcept->const Key*{const auto& keys=*static_cast<const std::vector<Key>*>(p);return index<keys.size()?&keys[index]:nullptr;};
                view.scale=forward?&scaleAfter:&scaleBefore;view.curves=forward?curveAfter.get():curveBefore.get();
                if(auto result=PrepareBoneEvaluation(app,slot,affected,view,candidate->evaluation))return result;}
            for(auto bone:mirrored){const auto chain=Chain(candidate->target,bone,count);unsigned cursor=bone;
                if(published&&candidate->frame!=initialFrame){for(auto index:chain){cursor=index;if(candidate->target[index].frame>=unsigned(candidate->frame))break;}}
                else if(!forward&&cursors)cursor=cursorBefore[bone];
                if(cursors)candidate->targetCursors[bone]=cursor;
                if(active)candidate->targetActive[bone]=forward?1:activeBefore[bone];}
            if(displayChanged){const auto& expected=forward?displayBefore:displayAfter;
                const auto& targetDisplay=forward?displayAfter:displayBefore;candidate->display=m.displayKeys;
                candidate->displayCapacity=mdl::KeyPoolCapacity(m.displayKeys);candidate->displayHeaders.resize(candidate->displayCapacity);
                std::memcpy(candidate->displayHeaders.data(),m.displayKeys,candidate->displayCapacity*sizeof(mdl::DisplayKey));
                candidate->displayCurrent=DisplayChain(m);
                if(candidate->displayCurrent.size()!=expected.size())return MMS_E_STALE;
                for(std::size_t i=0;i<expected.size();++i)if(candidate->displayCurrent[i].slot!=expected[i].slot||!expected[i].Current(m.displayKeys[expected[i].slot],false,false))return MMS_E_STALE;
                candidate->visibleBefore=candidate->visibleAfter=m.loadComplete;
                for(unsigned i=0;i<ikCount;++i)candidate->ikBefore.push_back(ikTable[i].enabled);candidate->ikAfter=candidate->ikBefore;
                std::size_t row=0;while(row+1<targetDisplay.size()&&targetDisplay[row+1].key.frame<=unsigned(candidate->frame))++row;
                if(visibleOwned)candidate->visibleAfter=targetDisplay[row].key.visible;
                for(unsigned i=0;i<ikCount;++i)if(ikOwned[i])candidate->ikAfter[i]=targetDisplay[row].ik[i];
            }
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);Charge(bytes,before.capacity(),sizeof(Key));Charge(bytes,after.capacity(),sizeof(Key));
        for(const auto* values:{&affected,&mirrored,&patches})Charge(bytes,values->capacity(),sizeof(unsigned));
        Charge(bytes,beforeMarks.capacity()+afterMarks.capacity(),sizeof(FrameMark));Charge(bytes,cursorBefore.capacity(),sizeof(std::uint32_t));
        Charge(bytes,activeBefore.capacity()+ikOwned.capacity());Charge(bytes,scaleBefore.RetainedBytes());Charge(bytes,scaleAfter.RetainedBytes());
        Charge(bytes,curveBefore->RetainedBytes());Charge(bytes,curveAfter->RetainedBytes());
        for(const auto* values:{&displayBefore,&displayAfter}){Charge(bytes,values->capacity(),sizeof(DisplayValue));for(const auto& value:*values){Charge(bytes,value.ik.capacity());Charge(bytes,value.parents.capacity(),sizeof(mdl::BoneReference));}}
        return bytes;
    }
};
void MirrorFree(BoneCurveChange& change,const BoneCurveEdit& edit){
    auto& keys=change.after;const auto old=Chain(keys,edit.bone,change.count);const auto originalKeys=keys;
    auto originalScale=change.scaleAfter.Clone();std::vector<unsigned> slots=old;std::set<int> frames;
    for(const auto& track:edit.tracks)for(const auto& key:track.keys)frames.insert(key.frame);
    for(std::size_t i=change.count;slots.size()<frames.size()&&i<keys.size();++i)
        if(!keys[i].frame&&std::find(old.begin(),old.end(),unsigned(i))==old.end())slots.push_back(unsigned(i));
    while(slots.size()<frames.size()){if(keys.size()>=mdl::kBoneKeyCapacityLimit)throw std::bad_alloc();slots.push_back(unsigned(keys.size()));keys.push_back({});}
    std::size_t at=0,n=0;
    for(int frame:frames){while(at+1<old.size()&&originalKeys[old[at+1]].frame<=unsigned(frame))++at;
        Key key=originalKeys[old[at]];key.frame=unsigned(frame);key.allocated=1;key.previous=n?slots[n-1]:0;key.next=n+1<frames.size()?slots[n+1]:0;
        for(unsigned c=0;c<3;++c)key.position[c]=float(animation_curve::Evaluate(edit.tracks[c],frame).value);Quaternion(edit.tracks,frame,key.rotation);
        for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)key.interpolation[c+k*4]=k<2?20:107;
        const auto matrix=Scale(edit.tracks,frame,SampleScale(originalKeys,originalScale,old,frame));
        if(!bone_scale::Valid(matrix)||!change.scaleAfter.SetKey(int(slots[n]),matrix)||!change.scaleAfter.SetKeyCurve(int(slots[n]),bone_scale::LinearCurve()))
            throw std::invalid_argument("bone curve mirror scale");
        keys[slots[n++]]=key;
    }
    for(std::size_t i=frames.size();i<old.size();++i){keys[old[i]]={};change.scaleAfter.ClearKey(int(old[i]));}
    TargetView target{keys,change.scaleAfter};change.curveAfter->Replace(int(edit.bone),edit.tracks,target.View());
    change.metadata=true;change.searchAfter=int(change.count);change.maxAfter=(std::max)(change.maxAfter,unsigned(*frames.rbegin()));
    change.lastAfter=(std::max)(change.lastAfter,change.maxAfter);change.mirrored.push_back(edit.bone);
}
}
MmsResult PlanBoneCurveWrite(MMDApp& app,unsigned slot,const std::vector<BoneCurveEdit>& edits,const DisplayCurveRows* display,
    std::shared_ptr<HistoryChange>& out)noexcept{
    if(out||slot>=kModelSlotCount||!app.ModelSlot(slot)||app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{
        auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!model.boneTable||!model.bonePhysicsState||!model.boneCount||model.boneCount>mdl::BoneKeyCapacity(model.boneKeys)||
            !mdl::BoneKeyPoolIdentity(model.boneKeys))return MMS_E_NOT_READY;
        if(display&&(!model.displayKeys||!mdl::KeyPoolIdentity(model.displayKeys)||(model.ikChainCount&&!model.ikChains)))return MMS_E_NOT_READY;
        ValidateStorage(app,slot,display!=nullptr);auto change=std::make_shared<BoneCurveChange>(app,slot);ValidateOwners(change->before,change->count);
        std::set<unsigned> unique;
        for(const auto& edit:edits){if(edit.bone>=model.boneCount||!unique.insert(edit.bone).second)return MMS_E_ARGUMENT;
            TargetView original{change->after,change->scaleAfter};auto effective=change->curveAfter->Clone();
            effective->ReconcileTarget(int(edit.bone),original.View());bone_curve::Tracks previous;
            const bool authored=effective->Read(int(edit.bone),previous);
            if(edit.freeCurves){
                // Validate all tracks against the existing private native view
                // before indexing union frames or allocating mirror slots.
                auto checked=change->curveAfter->Clone();checked->Replace(int(edit.bone),edit.tracks,original.View());
                if(authored&&SameTracks(previous,edit.tracks))continue;
                MirrorFree(*change,edit);change->affected.push_back(edit.bone);
            }else{
                if(authored)return MMS_E_ARGUMENT;const auto chain=Chain(change->after,edit.bone,change->count);
                if(edit.nativeControls.size()!=chain.size())return MMS_E_ARGUMENT;bool changed=false;
                for(std::size_t row=0;row<chain.size();++row){const auto& input=edit.nativeControls[row];auto& key=change->after[chain[row]];
                    if(input.frame!=int(key.frame))return MMS_E_ARGUMENT;
                    for(const auto& controls:input.controls)for(auto value:controls)if(value>127)return MMS_E_ARGUMENT;
                    if(!row)continue;
                    for(unsigned lane=0;lane<4;++lane)for(unsigned c=0;c<4;++c)if(key.interpolation[lane+c*4]!=input.controls[lane][c]){
                        key.interpolation[lane+c*4]=input.controls[lane][c];changed=true;}
                    if(change->scaleAfter.KeyCurve(int(chain[row]))!=input.controls[4]){if(!change->scaleAfter.SetKeyCurve(int(chain[row]),input.controls[4]))return MMS_E_ARGUMENT;changed=true;}
                }
                if(changed)change->affected.push_back(edit.bone);
            }
        }
        if(display){change->displayBefore=DisplayChain(model);change->displayAfter=change->displayBefore;
            if(display->size()!=change->displayBefore.size())return MMS_E_ARGUMENT;change->ikOwned.assign(change->ikCount,0);
            for(std::size_t row=0;row<display->size();++row){const auto& input=(*display)[row];auto& target=change->displayAfter[row];
                if(input.frame!=int(target.key.frame)||input.visible>1||input.ik.size()!=change->ikCount)return MMS_E_ARGUMENT;
                change->visibleOwned|=input.visible!=target.key.visible;target.key.visible=input.visible;
                for(unsigned i=0;i<change->ikCount;++i){if(input.ik[i]>1)return MMS_E_ARGUMENT;change->ikOwned[i]|=input.ik[i]!=target.ik[i];target.ik[i]=input.ik[i];}}
            change->displayChanged=change->visibleOwned||std::any_of(change->ikOwned.begin(),change->ikOwned.end(),[](auto value){return value!=0;});
            if(change->displayChanged)change->displayIdentity=mdl::KeyPoolIdentity(model.displayKeys);
        }
        if(change->affected.empty()&&!change->displayChanged)return MMS_OK;
        std::sort(change->affected.begin(),change->affected.end());std::sort(change->mirrored.begin(),change->mirrored.end());ValidateOwners(change->after,change->count);
        change->beforeMarks=Marks(change->before,change->count,change->affected);change->afterMarks=Marks(change->after,change->count,change->affected);
        for(std::size_t index=0;index<(std::max)(change->before.size(),change->after.size());++index){const auto a=index<change->before.size()?change->before[index]:Key{},b=index<change->after.size()?change->after[index]:Key{};
            if(!Equal(a,b))change->patches.push_back(unsigned(index));}
        out=std::move(change);return MMS_OK;
    });
}
}
#endif
