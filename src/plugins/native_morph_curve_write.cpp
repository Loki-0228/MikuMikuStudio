#include "native_morph_curve_write.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mikumikustudio::plugins {
namespace {
using Key=mdl::MorphKey;using Pool=std::vector<Key>;
void Add(std::uint64_t& bytes,std::uint64_t n,std::uint64_t stride=1)noexcept{bytes=stride&&n>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+n*stride;}
bool Same(Key a,Key b,bool marks=true)noexcept{if(!marks)a.allocated=b.allocated=0;return !std::memcmp(&a,&b,sizeof a);}
struct Mark{unsigned morph,frame;unsigned char value;bool operator==(const Mark& b)const noexcept{return morph==b.morph&&frame==b.frame&&value==b.value;}};
std::vector<Mark> Marks(const Pool& pool,unsigned count){
    std::vector<unsigned char> visited(pool.size());std::vector<Mark> marks;
    for(unsigned morph=0;morph<count;++morph){unsigned slot=morph,previous=0,frame=0;
        for(;;){if(slot>=pool.size()||visited[slot]||(slot<count&&slot!=morph))throw std::invalid_argument("shared morph curve native key");
            const auto& key=pool[slot];if(key.next>=pool.size()||key.previous>=pool.size()||!std::isfinite(key.value)||key.frame>unsigned(frame_limits::maximum)||
                (slot==morph?key.frame!=0:key.previous!=previous||key.frame<=frame))throw std::invalid_argument("invalid morph curve native chain");
            visited[slot]=1;marks.push_back({morph,key.frame,key.allocated});if(!key.next)break;previous=slot;frame=key.frame;slot=key.next;}
    }
    for(std::size_t i=count;i<pool.size();++i)if(!visited[i]&&pool[i].frame)throw std::invalid_argument("orphan morph curve native key");return marks;
}
morph_curve::NativeKeyView View(const Pool& pool){return {&pool,[](const void* p,std::uint32_t i)noexcept{const auto& v=*static_cast<const Pool*>(p);return i<v.size()?&v[i]:nullptr;},pool.size()};}
bool Exclusive(MMDApp& app,unsigned slot,const Key* pool,std::size_t capacity)noexcept{
    struct Range{const void* data;std::size_t count,stride;};
    const auto separate=[](Range a,Range b){if(!a.data||!a.count||!b.data||!b.count)return true;
        const auto first=reinterpret_cast<std::uintptr_t>(a.data),other=reinterpret_cast<std::uintptr_t>(b.data);
        return a.stride&&b.stride&&a.count<=(UINTPTR_MAX-first)/a.stride&&b.count<=(UINTPTR_MAX-other)/b.stride&&
            !(first<other+b.count*b.stride&&other<first+a.count*a.stride);};
    if(!pool)return false;const auto& own=*mdl::Mdl(app.ModelSlot(slot));const Range native{pool,capacity,sizeof(Key)};
    const std::array<Range,3> writes={{{own.morphs,own.morphCount,sizeof(mdl::MorphRecord)},{own.morphKeyCursors,own.morphCount,sizeof(std::uint32_t)},{own.morphTrackActive,own.morphCount,1}}};
    const Range appState{&app.state,1,sizeof app.state};if(!separate(native,appState)||!separate({&own,1,sizeof own},appState))return false;
    for(const auto& write:writes)if(!separate(write,appState))return false;
    for(unsigned i=0;i<kModelSlotCount;++i)if(app.ModelSlot(i)){const auto& m=*mdl::Mdl(app.ModelSlot(i));
        const std::array<Range,16> protectedRanges={{{&m,1,sizeof m},{m.morphs,m.morphCount,sizeof(mdl::MorphRecord)},{m.boneTable,m.boneCount,sizeof(mdl::BoneRecord)},
            {m.boneKeys,mdl::BoneKeyCapacity(m.boneKeys),sizeof(mdl::BoneKey)},{m.displayKeys,mdl::KeyPoolCapacity(m.displayKeys),sizeof(mdl::DisplayKey)},
            {m.morphKeyCursors,m.morphCount,sizeof(std::uint32_t)},{m.morphTrackActive,m.morphCount,1},
            {m.boneKeyCursors,m.boneCount,sizeof(std::uint32_t)},{m.boneTrackActive,m.boneCount,1},{m.morphKeys,mdl::MorphKeyCapacity(m.morphKeys),sizeof(Key)},
            {m.boneSelection,m.boneCount,1},{m.bonePhysicsState,m.boneCount,1},{m.boneKeyIndices,m.boneCount,sizeof(std::uint32_t)},
            {m.morphKeyIndices,m.morphCount,sizeof(std::uint32_t)},{m.ikChains,m.ikChainCount,sizeof(mdl::IkChain)},
            {m.boneOrderTable,m.boneOrderCount,sizeof(mdl::BoneOrderEntry)}}};
        for(unsigned r=0;r<protectedRanges.size();++r){if(!(i==slot&&r==9)&&!separate(native,protectedRanges[r]))return false;
            for(unsigned w=0;w<writes.size();++w)if(!(i==slot&&r==(w==0?1:w==1?5:6))&&!separate(writes[w],protectedRanges[r]))return false;}
        for(unsigned chain=0;m.ikChains&&chain<m.ikChainCount;++chain){const Range links{m.ikChains[chain].links,m.ikChains[chain].linkCount,sizeof(std::uint16_t)};
            if(!separate(native,links))return false;for(const auto& write:writes)if(!separate(write,links))return false;}
        for(std::size_t k=0;m.displayKeys&&k<mdl::KeyPoolCapacity(m.displayKeys);++k){
            const std::array<Range,2> nested={{{m.displayKeys[k].ikStates,m.ikChainCount,1},{m.displayKeys[k].selectorStates,m.boneOrderCount,sizeof(mdl::BoneReference)}}};
            for(const auto& item:nested){if(!separate(native,item))return false;for(const auto& write:writes)if(!separate(write,item))return false;}}
    }
    return true;
}
struct CurveWrite final:HistoryChange,std::enable_shared_from_this<CurveWrite>{
    MMDApp& app;unsigned slot;unsigned char* model;std::shared_ptr<RetainedSceneObject> lifetime;std::uint64_t objectGeneration,identity,generation=1;
    mdl::MorphRecord* morphs;unsigned count;std::uint32_t* cursors;unsigned char* active;std::size_t capacity;int initialFrame,searchBefore;
    std::uint32_t maxBefore,maxAfter,lastBefore,lastAfter;bool published=false,preserveSelection=false;
    morph_curve::AuthoredReplacement authoring;Pool original;
    struct Patch{unsigned slot;Key before,after;};std::vector<Patch> patches;
    std::vector<Mark> beforeMarks,afterMarks;std::vector<float> initialValues;std::vector<std::uint32_t> initialCursors;std::vector<unsigned char> initialActive;
    CurveWrite(MMDApp& a,unsigned s,const std::vector<animation_curve::NamedTrack>& input):app(a),slot(s),model(a.ModelSlot(s)),
        lifetime(RetainSceneObject(a,MMS_OBJECT_MODEL,s)),objectGeneration(SceneObjectIdentity(model).generation),identity(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)),
        morphs(mdl::Mdl(model)->morphs),count(mdl::Mdl(model)->morphCount),cursors(mdl::Mdl(model)->morphKeyCursors),active(mdl::Mdl(model)->morphTrackActive),
        capacity(mdl::MorphKeyCapacity(mdl::Mdl(model)->morphKeys)),initialFrame(a.CurrentFrame()),searchBefore(mdl::Mdl(model)->searchCursor),
        maxBefore(mdl::Mdl(model)->maxFrame),maxAfter(maxBefore),lastBefore(a.LastRegisteredFrame()),lastAfter(lastBefore),authoring(model,input),
        original(mdl::Mdl(model)->morphKeys,mdl::Mdl(model)->morphKeys+capacity){
        beforeMarks=Marks(original,count);const auto view=authoring.TargetKeys();Pool target(view.capacity);
        for(unsigned i=0;i<target.size();++i)target[i]=*view.key(view.context,i);afterMarks=Marks(target,count);
        const auto unrelated=[&](const Mark& mark){const auto& owned=authoring.ChangedMorphs();return std::find(owned.begin(),owned.end(),mark.morph)==owned.end();};
        beforeMarks.erase(std::remove_if(beforeMarks.begin(),beforeMarks.end(),unrelated),beforeMarks.end());afterMarks.erase(std::remove_if(afterMarks.begin(),afterMarks.end(),unrelated),afterMarks.end());
        for(std::size_t i=0;i<(std::max)(original.size(),target.size());++i){const Key a=i<original.size()?original[i]:Key{},b=i<target.size()?target[i]:Key{};
            if(!Same(a,b))patches.push_back({unsigned(i),a,b});}
        for(unsigned morph:authoring.ChangedMorphs()){initialValues.push_back(morphs[morph].value);if(cursors)initialCursors.push_back(cursors[morph]);if(active)initialActive.push_back(active[morph]);
            unsigned index=morph;while(target[index].next)index=target[index].next;maxAfter=(std::max)(maxAfter,target[index].frame);lastAfter=(std::max)(lastAfter,target[index].frame);}
    }
    bool Structure()const noexcept{
        if(!lifetime->Present()||app.ModelSlot(slot)!=model||!SceneObjectPresent(model,lifetime->Id()))return false;const auto& m=*mdl::Mdl(model);
        return m.morphs==morphs&&m.morphCount==count&&m.morphKeyCursors==cursors&&m.morphTrackActive==active&&mdl::MorphKeyPoolIdentity(m.morphKeys)==identity&&
            mdl::MorphKeyCapacity(m.morphKeys)>=capacity&&Exclusive(app,slot,m.morphKeys,mdl::MorphKeyCapacity(m.morphKeys));
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<CurveWrite> source;bool forward,consumed=false,preserveSelection=false;std::uint64_t generation,objectGeneration;int frame,search;
        Key* pool;std::size_t capacity;Pool current,target;std::uint32_t maximum,last;
        std::vector<float> beforeValues,afterValues;std::vector<std::uint32_t> beforeCursors,afterCursors;std::vector<unsigned char> beforeActive,afterActive;
        std::unique_ptr<mdl::MorphKeyPoolGrowth> growth;std::unique_ptr<morph_curve::AuthoredReplacement::Target> layers;
        Candidate(std::shared_ptr<CurveWrite> owner,bool direction):source(std::move(owner)),forward(direction),generation(source->generation),objectGeneration(SceneObjectIdentity(source->model).generation),
            frame(source->app.CurrentFrame()),search(mdl::Mdl(source->model)->searchCursor),pool(mdl::Mdl(source->model)->morphKeys),capacity(mdl::MorphKeyCapacity(pool)),current(pool,pool+capacity),target(current),
            maximum(mdl::Mdl(source->model)->maxFrame),last(source->app.LastRegisteredFrame()){}
        bool IsCurrent()const noexcept override{
            if(consumed||source->generation!=generation||!source->Structure()||SceneObjectIdentity(source->model).generation!=objectGeneration||source->app.CurrentFrame()!=frame||
                mdl::Mdl(source->model)->searchCursor!=search||mdl::Mdl(source->model)->maxFrame!=maximum||source->app.LastRegisteredFrame()!=last||
                mdl::Mdl(source->model)->morphKeys!=pool||mdl::MorphKeyCapacity(pool)!=capacity||std::memcmp(pool,current.data(),capacity*sizeof(Key))||
                !growth||!growth->IsCurrent()||!layers||!layers->IsCurrent())return false;
            const auto& changed=source->authoring.ChangedMorphs();for(std::size_t i=0;i<changed.size();++i){const auto morph=changed[i];
                if(std::memcmp(&source->morphs[morph].value,&beforeValues[i],sizeof(float))||(source->cursors&&source->cursors[morph]!=beforeCursors[i])||
                    (source->active&&source->active[morph]!=beforeActive[i]))return false;}
            return true;
        }
        void Publish()noexcept override{
            if(!IsCurrent()||!growth->Publish())std::terminate();auto& model=*mdl::Mdl(source->model);
            for(const auto& patch:source->patches)model.morphKeys[patch.slot]=target[patch.slot];layers->Publish();
            const auto& changed=source->authoring.ChangedMorphs();for(std::size_t i=0;i<changed.size();++i){const auto morph=changed[i];source->morphs[morph].value=afterValues[i];
                if(source->cursors)source->cursors[morph]=afterCursors[i];if(source->active)source->active[morph]=afterActive[i];}
            if(source->maxBefore!=source->maxAfter)model.maxFrame=forward?source->maxAfter:source->maxBefore;
            if(source->lastBefore!=source->lastAfter)source->app.LastRegisteredFrame()=forward?source->lastAfter:source->lastBefore;
            if(source->searchBefore!=int(source->count))model.searchCursor=forward?int(source->count):source->searchBefore;
            source->published=true;source->preserveSelection=preserveSelection;++source->generation;consumed=true;if(!growth->Accept())std::terminate();
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();if(!Structure()||!authoring.IsCurrent()||generation==UINT64_MAX)return MMS_E_STALE;
        return Boundary([&]()->MmsResult{
            auto& modelState=*mdl::Mdl(model);if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            if(!published&&(!forward||app.CurrentFrame()!=initialFrame||SceneObjectIdentity(model).generation!=objectGeneration||modelState.maxFrame!=maxBefore||app.LastRegisteredFrame()!=lastBefore||modelState.searchCursor!=searchBefore))return MMS_E_STALE;
            if(maxBefore!=maxAfter&&modelState.maxFrame!=(forward?maxBefore:maxAfter)||lastBefore!=lastAfter&&app.LastRegisteredFrame()!=(forward?lastBefore:lastAfter)||
                searchBefore!=int(count)&&modelState.searchCursor!=(forward?searchBefore:int(count)))return MMS_E_STALE;
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);auto currentMarks=Marks(candidate->current,count);
            const auto& affected=authoring.ChangedMorphs();currentMarks.erase(std::remove_if(currentMarks.begin(),currentMarks.end(),[&](const Mark& mark){return std::find(affected.begin(),affected.end(),mark.morph)==affected.end();}),currentMarks.end());
            if(!published&&(candidate->capacity!=original.size()||std::memcmp(candidate->pool,original.data(),original.size()*sizeof(Key))))return MMS_E_STALE;
            std::size_t needed=candidate->capacity;
            for(const auto& patch:patches){const auto live=patch.slot<candidate->capacity?candidate->pool[patch.slot]:Key{};
                if(!Same(live,forward?patch.before:patch.after,!published))return MMS_E_STALE;needed=(std::max)(needed,std::size_t(patch.slot)+1);}
            candidate->target.resize(needed);for(const auto& patch:patches)candidate->target[patch.slot]=forward?patch.after:patch.before;
            (void)Marks(candidate->target,count);candidate->preserveSelection=published&&(preserveSelection||currentMarks!=(forward?beforeMarks:afterMarks));
            if(candidate->preserveSelection)for(unsigned morph:authoring.ChangedMorphs()){unsigned index=morph;for(;;){auto& key=candidate->target[index];
                const auto at=std::lower_bound(currentMarks.begin(),currentMarks.end(),std::pair<unsigned,unsigned>{morph,key.frame},[](const auto& mark,const auto& id){return std::pair<unsigned,unsigned>{mark.morph,mark.frame}<id;});
                key.allocated=at!=currentMarks.end()&&at->morph==morph&&at->frame==key.frame?at->value:0;if(!key.next)break;index=key.next;}}
            if(!mdl::MorphKeyPoolGrowth::Prepare(modelState,needed,candidate->growth))return MMS_E_MEMORY;
            candidate->layers=authoring.Prepare(View(candidate->target),forward);
            const auto& changed=authoring.ChangedMorphs();for(std::size_t i=0;i<changed.size();++i){const auto morph=changed[i];
                const float value=morphs[morph].value;if(!std::isfinite(value))return MMS_E_ARGUMENT;
                if(!published&&(std::memcmp(&value,&initialValues[i],sizeof value)||(cursors&&cursors[morph]!=initialCursors[i])||(active&&active[morph]!=initialActive[i])))return MMS_E_STALE;
                float after=0;if(!candidate->layers->Sample(int(morph),candidate->frame,after))return MMS_E_ARGUMENT;
                candidate->beforeValues.push_back(value);candidate->afterValues.push_back(after);
                unsigned index=morph;while(candidate->target[index].frame<unsigned(candidate->frame)&&candidate->target[index].next)index=candidate->target[index].next;
                if(cursors){candidate->beforeCursors.push_back(cursors[morph]);candidate->afterCursors.push_back(!forward&&candidate->frame==initialFrame?initialCursors[i]:index);}
                if(active){candidate->beforeActive.push_back(active[morph]);candidate->afterActive.push_back(!forward&&candidate->frame==initialFrame?initialActive[i]:std::uint8_t(candidate->target[index].frame>=unsigned(candidate->frame)));}
            }
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);Add(bytes,original.capacity(),sizeof(Key));Add(bytes,patches.capacity(),sizeof(Patch));
        Add(bytes,beforeMarks.capacity(),sizeof(Mark));Add(bytes,afterMarks.capacity(),sizeof(Mark));Add(bytes,initialValues.capacity(),sizeof(float));Add(bytes,initialCursors.capacity(),sizeof(std::uint32_t));
        Add(bytes,initialActive.capacity());Add(bytes,authoring.RetainedBytes());return bytes;}
};
}
MmsResult PlanMorphCurveWrite(MMDApp& app,unsigned slot,const std::vector<animation_curve::NamedTrack>& input,std::shared_ptr<HistoryChange>& out)noexcept{
    if(out||slot>=kModelSlotCount||input.empty()||app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(!app.ModelSlot(slot))return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!model.morphKeys||!mdl::MorphKeyPoolIdentity(model.morphKeys)||!Exclusive(app,slot,model.morphKeys,mdl::MorphKeyCapacity(model.morphKeys)))return MMS_E_NOT_READY;
        auto change=std::make_shared<CurveWrite>(app,slot,input);if(change->authoring.Changed())out=std::move(change);return MMS_OK;});
}
}
#endif
