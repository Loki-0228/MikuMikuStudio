#include "native_display_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "native_bone_evaluation.hpp"
#include "native_bone_registration.hpp"
#include "native_range_scale.hpp"
#include "native_model_paste.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/extended_names.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <map>
#include <set>
#include <array>
#include <cmath>
#include <string>

namespace mikudancestudio::plugins {
namespace {
// Compare explicit members rather than pointer values or ABI padding. Nested
// buffers belong to the current pool and are never replaced by history data.
struct Value {
    std::uint32_t frame, previous, next;
    unsigned char visible, allocated;
    unsigned char reserved0[3], reserved1[3];
};
Value Read(const mdl::DisplayKey& key) noexcept {
    Value value{key.frame,key.previous,key.next,key.visible,key.allocated,{},{}};
    std::memcpy(value.reserved0,key.reserved0,sizeof(value.reserved0));
    std::memcpy(value.reserved1,key.reserved1,sizeof(value.reserved1));return value;
}
bool Equal(const Value& a,const Value& b,bool marks) noexcept {
    return a.frame==b.frame&&a.previous==b.previous&&a.next==b.next&&a.visible==b.visible&&
        (!marks||a.allocated==b.allocated)&&!std::memcmp(a.reserved0,b.reserved0,sizeof(a.reserved0))&&
        !std::memcmp(a.reserved1,b.reserved1,sizeof(a.reserved1));
}
void Write(mdl::DisplayKey& key,const Value& value) noexcept {
    key.frame=value.frame;key.previous=value.previous;key.next=value.next;
    key.visible=value.visible;key.allocated=value.allocated;
    std::memcpy(key.reserved0,value.reserved0,sizeof(key.reserved0));
    std::memcpy(key.reserved1,value.reserved1,sizeof(key.reserved1));
}
struct Payload {
    Value value{};
    std::vector<unsigned char> ik;
    std::vector<mdl::BoneReference> parents;
    static Payload Empty(unsigned ikCount,unsigned parentCount) {
        Payload result;result.value.visible=1;result.ik.assign(ikCount,1);
        result.parents.assign(parentCount,mdl::BoneReference{-1,0});return result;
    }
    static Payload Capture(const mdl::DisplayKey& key,unsigned ikCount,unsigned parentCount) {
        if((ikCount&&!key.ikStates)||(parentCount&&!key.selectorStates))
            throw std::invalid_argument("missing display key payload");
        Payload result;result.value=Read(key);
        if(ikCount)result.ik.assign(key.ikStates,key.ikStates+ikCount);
        if(parentCount)result.parents.assign(key.selectorStates,key.selectorStates+parentCount);
        return result;
    }
    bool Equals(const mdl::DisplayKey& key,bool marks) const noexcept {
        return Equal(value,Read(key),marks)&&
            (ik.empty()||(key.ikStates&&!std::memcmp(ik.data(),key.ikStates,ik.size())))&&
            (parents.empty()||(key.selectorStates&&!std::memcmp(parents.data(),key.selectorStates,parents.size()*sizeof(mdl::BoneReference))));
    }
    void Publish(mdl::DisplayKey& key) const noexcept {
        Write(key,value);
        if(!ik.empty())std::memcpy(key.ikStates,ik.data(),ik.size());
        if(!parents.empty())std::memcpy(key.selectorStates,parents.data(),parents.size()*sizeof(mdl::BoneReference));
    }
};
struct Patch { unsigned slot;Payload before,after; };
struct Dependency { unsigned slot;Value value; };
struct DeepDependency { unsigned slot;Payload payload; };
std::vector<Dependency> Chain(const mdl::ModelRecord& model) {
    const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
    std::vector<Dependency> chain;std::set<unsigned> visited;
    unsigned slot=0,previous=0,frame=0;
    for(std::size_t hops=0;hops<capacity;++hops) {
        if(slot>=capacity||!visited.insert(slot).second)throw std::invalid_argument("invalid display key chain");
        const auto value=Read(model.displayKeys[slot]);
        if(value.previous>=capacity||value.next>=capacity||value.frame>unsigned(frame_limits::maximum)||
            (!hops&&value.frame)||(hops&&(value.previous!=previous||value.frame<=frame)))
            throw std::invalid_argument("invalid display key link or frame");
        chain.push_back({slot,value});if(!value.next)return chain;
        previous=slot;frame=value.frame;slot=value.next;
    }
    throw std::invalid_argument("unterminated display key chain");
}
void CheckPayloadStorage(const mdl::ModelRecord& model) {
    struct Range { std::uintptr_t begin,end; };
    std::vector<Range> ranges;
    const auto add=[&](const void* pointer,std::size_t count,std::size_t stride) {
        if(!pointer||!count)return;
        const auto begin=reinterpret_cast<std::uintptr_t>(pointer);
        if(count>(UINTPTR_MAX-begin)/stride)throw std::invalid_argument("display key payload overflow");
        ranges.push_back({begin,begin+count*stride});
    };
    const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
    // A standalone display edit must not alias another native participant's
    // storage. Only registered key-pool extents provide trustworthy bounds.
    add(&model,1,sizeof(model));
    add(model.boneTable,model.boneCount,sizeof(mdl::BoneRecord));
    add(model.morphs,model.morphCount,sizeof(mdl::MorphRecord));
    add(model.ikChains,model.ikChainCount,sizeof(mdl::IkChain));
    add(model.boneOrderTable,model.boneOrderCount,sizeof(mdl::BoneOrderEntry));
    add(model.bonePhysicsState,model.boneCount,1);add(model.boneSelection,model.boneCount,1);
    if(model.boneKeys&&mdl::BoneKeyPoolIdentity(model.boneKeys))add(model.boneKeys,mdl::BoneKeyCapacity(model.boneKeys),sizeof(mdl::BoneKey));
    if(model.morphKeys&&mdl::MorphKeyPoolIdentity(model.morphKeys))add(model.morphKeys,mdl::MorphKeyCapacity(model.morphKeys),sizeof(mdl::MorphKey));
    add(model.displayKeys,capacity,sizeof(mdl::DisplayKey));
    for(std::size_t slot=0;slot<capacity;++slot) {
        add(model.displayKeys[slot].ikStates,model.ikChainCount,1);
        add(model.displayKeys[slot].selectorStates,model.boneOrderCount,sizeof(mdl::BoneReference));
    }
    std::sort(ranges.begin(),ranges.end(),[](const Range& a,const Range& b){return a.begin<b.begin;});
    for(std::size_t index=1;index<ranges.size();++index)
        if(ranges[index].begin<ranges[index-1].end)throw std::invalid_argument("aliased display key payload");
}
template<class Key> bool Marked(const Key* keys,std::size_t capacity) noexcept {
    for(std::size_t slot=0;slot<capacity;++slot)if(keys[slot].allocated)return true;return false;
}
void AddBytes(std::uint64_t& total,std::size_t count,std::size_t stride) noexcept {
    if(count>(UINT64_MAX-total)/stride)total=UINT64_MAX;else total+=std::uint64_t(count)*stride;
}
// Existing nested buffers are shared only by the old and candidate top-level
// arrays. The candidate owns newly allocated tail payloads until publication;
// the native model then owns every buffer. Cancellation never frees old ones.
struct DisplayGrowth {
    mdl::ModelRecord& model;mdl::DisplayKey* original;mdl::DisplayKey* grown=nullptr;
    std::size_t oldCapacity,targetCapacity;std::uint64_t identity;
    DisplayGrowth(mdl::ModelRecord& m,std::size_t required):model(m),original(m.displayKeys),
        oldCapacity(mdl::KeyPoolCapacity(original)),targetCapacity(oldCapacity),identity(mdl::KeyPoolIdentity(original)) {
        if(!identity)throw std::invalid_argument("borrowed display pool growth");
        if(required<=oldCapacity)return;
        if(required>mdl::KeyPoolLimit<mdl::DisplayKey>())throw std::bad_alloc();
        targetCapacity=(std::min)(mdl::KeyPoolLimit<mdl::DisplayKey>(),
            (std::max)(required,oldCapacity+oldCapacity/2+256));
        grown=mdl::KeyPoolCreate<mdl::DisplayKey>(targetCapacity);
        try {
            for(std::size_t index=oldCapacity;index<targetCapacity;++index) {
                auto& key=grown[index];key.visible=1;
                if(m.ikChainCount) {
                    key.ikStates=static_cast<unsigned char*>(std::malloc(m.ikChainCount));
                    if(!key.ikStates)throw std::bad_alloc();std::memset(key.ikStates,1,m.ikChainCount);
                }
                if(m.boneOrderCount) {
                    if(std::size_t(m.boneOrderCount)>SIZE_MAX/sizeof(mdl::BoneReference))throw std::bad_alloc();
                    key.selectorStates=static_cast<mdl::BoneReference*>(std::calloc(m.boneOrderCount,sizeof(mdl::BoneReference)));
                    if(!key.selectorStates)throw std::bad_alloc();
                    for(unsigned parent=0;parent<m.boneOrderCount;++parent)key.selectorStates[parent].modelIndex=-1;
                }
            }
            std::memcpy(grown,original,oldCapacity*sizeof(mdl::DisplayKey));
        } catch(...) {Clear();throw;}
    }
    void Clear() noexcept {
        if(!grown)return;
        for(std::size_t index=oldCapacity;index<targetCapacity;++index) {
            std::free(grown[index].ikStates);std::free(grown[index].selectorStates);
        }
        mdl::KeyPoolDestroy(grown);
    }
    ~DisplayGrowth(){Clear();}
    bool IsCurrent() const noexcept {
        return model.displayKeys==original&&mdl::KeyPoolCapacity(original)==oldCapacity&&mdl::KeyPoolIdentity(original)==identity;
    }
    mdl::DisplayKey* Target() const noexcept {return grown?grown:original;}
    void Publish() noexcept {if(grown){mdl::KeyPoolCommitGrowth(model.displayKeys,grown);grown=nullptr;}}
};
struct DisplayDeletion final : DisplayKeyDeletion,std::enable_shared_from_this<DisplayDeletion> {
    MMDApp& app;unsigned modelSlot;unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    std::uint64_t identity;std::size_t capacity;
    unsigned ikCount,parentCount;
    const void* ikTable;const void* parentTable;
    bool evaluateBones;
    struct NameRule { unsigned bone;std::array<char,20> name; };
    std::vector<NameRule> names;
    std::vector<int> ikBones,parentBones;
    const mdl::BoneRecord* nameTable=nullptr;
    unsigned nameCount=0;
    struct PasteReference {
        unsigned slot;unsigned char* model;const mdl::BoneRecord* bones;unsigned count;
        std::shared_ptr<RetainedSceneObject> owner;
    };
    std::vector<PasteReference> pasteReferences;
    std::vector<Patch> patches;
    bool registration=false,paste=false,aggregatePaste=false,publishedOnce=false;std::uint64_t generation=1;
    unsigned registeredSlot=0;
    std::vector<unsigned char> boneMarks,morphMarks,displayMarks;
    std::uint64_t boneMarkIdentity=0,morphMarkIdentity=0;
    std::uint32_t maxBefore=0,maxAfter=0,lastBefore=0,lastAfter=0;
    int searchBefore=0,searchAfter=0;
    DisplayDeletion(MMDApp& a,unsigned slot,bool evaluate):app(a),modelSlot(slot),model(a.ModelSlot(slot)),
        lifetime(RetainSceneObject(a,MMS_OBJECT_MODEL,slot)),identity(mdl::KeyPoolIdentity(mdl::Mdl(model)->displayKeys)),
        capacity(mdl::KeyPoolCapacity(mdl::Mdl(model)->displayKeys)),ikCount(mdl::Mdl(model)->ikChainCount),
        parentCount(mdl::Mdl(model)->boneOrderCount),ikTable(mdl::Mdl(model)->ikChains),parentTable(mdl::Mdl(model)->boneOrderTable),evaluateBones(evaluate) {}
    bool Structure() const noexcept {
        if(!lifetime||!lifetime->Present()||app.ModelSlot(modelSlot)!=model||!SceneObjectPresent(model,lifetime->Id()))return false;
        const auto& m=*mdl::Mdl(model);
        if(mdl::KeyPoolIdentity(m.displayKeys)!=identity||mdl::KeyPoolCapacity(m.displayKeys)<capacity||
            m.ikChainCount!=ikCount||m.boneOrderCount!=parentCount||m.ikChains!=ikTable||m.boneOrderTable!=parentTable)return false;
        if(registration&&((!boneMarks.empty()&&(mdl::BoneKeyPoolIdentity(m.boneKeys)!=boneMarkIdentity||mdl::BoneKeyCapacity(m.boneKeys)<boneMarks.size()))||
            (!morphMarks.empty()&&(mdl::MorphKeyPoolIdentity(m.morphKeys)!=morphMarkIdentity||mdl::MorphKeyCapacity(m.morphKeys)<morphMarks.size()))))return false;
        if(nameTable) {
            if(m.boneTable!=nameTable||m.boneCount!=nameCount)return false;
            for(const auto& name:names)if(std::memcmp(m.boneTable[name.bone].name,name.name.data(),name.name.size()))return false;
            for(unsigned index=0;index<ikBones.size();++index)if(m.ikChains[index].boneIndex!=ikBones[index])return false;
            const auto* order=static_cast<const mdl::BoneOrderEntry*>(m.boneOrderTable);
            for(unsigned index=0;index<parentBones.size();++index)if(order[index].boneIndex!=parentBones[index])return false;
        }
        for(const auto& reference:pasteReferences) {
            if(!reference.owner->Present()||app.ModelSlot(reference.slot)!=reference.model||
                !SceneObjectPresent(reference.model,reference.owner->Id()))return false;
            const auto& target=*mdl::Mdl(reference.model);
            if(target.boneTable!=reference.bones||target.boneCount!=reference.count)return false;
        }
        return true;
    }
    struct FrameState {
        unsigned char visible=0;
        std::vector<unsigned char> ik;
        std::vector<mdl::BoneOrderEntry> selectors;
        std::vector<unsigned> references;
    };
    // Materialize the reachable target track from sparse patches and unchanged
    // live records. This also validates references before any frame publication.
    MmsResult Sample(bool forward,FrameState& out) const {
        const auto& m=*mdl::Mdl(model);
        if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        if((ikCount&&!m.ikChains)||(parentCount&&!m.boneOrderTable))return MMS_E_NOT_READY;
        const auto liveCapacity=mdl::KeyPoolCapacity(m.displayKeys);
        const auto cap=patches.empty()?liveCapacity:(std::max)(liveCapacity,std::size_t(patches.back().slot)+1);
        std::vector<DeepDependency> chain;std::set<unsigned> seen,referenced;
        unsigned slot=0,previous=0,frame=0;
        for(std::size_t hops=0;;++hops) {
            if(hops>=cap||slot>=cap||!seen.insert(slot).second)return MMS_E_ARGUMENT;
            const auto found=std::lower_bound(patches.begin(),patches.end(),slot,
                [](const Patch& patch,unsigned index){return patch.slot<index;});
            if(slot>=liveCapacity&&(found==patches.end()||found->slot!=slot))return MMS_E_ARGUMENT;
            auto payload=found!=patches.end()&&found->slot==slot?(forward?found->after:found->before):
                Payload::Capture(m.displayKeys[slot],ikCount,parentCount);
            const auto& value=payload.value;
            if(value.previous>=cap||value.next>=cap||value.frame>unsigned(frame_limits::maximum)||
                (!hops&&value.frame)||(hops&&(value.previous!=previous||value.frame<=frame)))return MMS_E_ARGUMENT;
            for(const auto& pair:payload.parents) {
                if(pair.modelIndex< -1||pair.modelIndex>=int(kModelSlotCount))return MMS_E_ARGUMENT;
                if(pair.modelIndex>=0) {
                    const auto* target=app.ModelSlot(unsigned(pair.modelIndex));
                    if(!target)return MMS_E_NOT_READY;
                    const auto& other=*mdl::Mdl(target);
                    if(pair.boneIndex<0||unsigned(pair.boneIndex)>=other.boneCount)return MMS_E_ARGUMENT;
                    if(!other.boneTable)return MMS_E_NOT_READY;
                    referenced.insert(unsigned(pair.modelIndex));
                }
            }
            const auto next=value.next;frame=value.frame;previous=slot;
            chain.push_back({slot,std::move(payload)});
            if(!next)break;slot=next;
        }
        const auto current=unsigned(app.CurrentFrame());std::size_t lower=0;
        while(lower+1<chain.size()&&chain[lower+1].payload.value.frame<=current)++lower;
        FrameState sampled;sampled.visible=chain[lower].payload.value.visible;sampled.ik=chain[lower].payload.ik;
        sampled.references.assign(referenced.begin(),referenced.end());
        if(parentCount) {
            const auto* selectors=static_cast<const mdl::BoneOrderEntry*>(m.boneOrderTable);
            sampled.selectors.assign(selectors,selectors+parentCount);
        }
        for(unsigned index=0;index<parentCount;++index) {
            auto& selector=sampled.selectors[index];const auto& pair=chain[lower].payload.parents[index];
            selector.linkedModel=pair.modelIndex;selector.linkedBone=pair.boneIndex;
            const auto same=[&](std::size_t item) {
                const auto& other=chain[item].payload.parents[index];
                return pair.modelIndex==other.modelIndex&&pair.boneIndex==other.boneIndex;
            };
            auto first=lower;while(first&&same(first-1))--first;
            selector.windowStart=first?chain[first].payload.value.frame:0;
            auto end=lower+1;while(end<chain.size()&&same(end))++end;
            selector.windowEnd=end<chain.size()?chain[end].payload.value.frame:0;
        }
        out=std::move(sampled);return MMS_OK;
    }
    MmsResult TargetSelectors(bool forward,std::vector<mdl::BoneOrderEntry>& out) const noexcept override {
        if(!Structure())return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            const auto& m=*mdl::Mdl(model);CheckPayloadStorage(m);
            for(const auto& patch:patches) {
                if(patch.slot>=mdl::KeyPoolCapacity(m.displayKeys)) {if((!registration&&!aggregatePaste)||!forward||publishedOnce)return MMS_E_STALE;continue;}
                if(!(forward?patch.before:patch.after).Equals(m.displayKeys[patch.slot],false))return MMS_E_STALE;
            }
            FrameState state;if(auto status=Sample(forward,state))return status;
            out.swap(state.selectors);return MMS_OK;
        });
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<DisplayDeletion> source;bool forward;
        mdl::DisplayKey* pool;std::size_t capacity;
        std::vector<Payload> current;
        std::vector<std::pair<unsigned char*,mdl::BoneReference*>> pointers;
        std::vector<DeepDependency> dependencies;
        FrameState before,after;int frame;
        struct Reference {
            unsigned slot;unsigned char* model;mdl::BoneRecord* bones;unsigned count;
            std::shared_ptr<RetainedSceneObject> owner;
        };
        std::vector<Reference> references;
        mdl::BoneKey* bonePool=nullptr;std::size_t boneCapacity=0;std::uint64_t boneIdentity=0;
        mdl::BoneRecord* bones=nullptr;unsigned boneCount=0;
        std::vector<int> boneSelectors;
        std::vector<std::pair<unsigned,mdl::BoneKey>> boneKeys;
        // These read-only restore candidates freeze authoring and hold any
        // required empty scale-storage lease. They never publish authoring.
        std::unique_ptr<bone_scale::AuthoringRestore> scaleGuard;
        std::unique_ptr<bone_curve::AuthoringRestore> curveGuard;
        std::unique_ptr<PreparedHistoryChange> evaluation;
        std::unique_ptr<DisplayGrowth> growth;
        bool initial=false;std::uint64_t generation;
        mdl::BoneKey* markedBones=nullptr;mdl::MorphKey* markedMorphs=nullptr;
        std::size_t markedBoneCapacity=0,markedMorphCapacity=0;
        std::vector<unsigned char> currentBoneMarks,currentMorphMarks,currentDisplayMarks;
        std::uint32_t currentMax=0,currentLast=0;int currentSearch=0;
        bool published=false;
        Candidate(std::shared_ptr<DisplayDeletion> s,bool f):source(std::move(s)),forward(f),
            pool(mdl::Mdl(source->model)->displayKeys),capacity(mdl::KeyPoolCapacity(pool)),frame(source->app.CurrentFrame()),generation(source->generation) {}
        bool IsCurrent() const noexcept override {
            if(published||source->generation!=generation||!source->Structure()||source->app.CurrentFrame()!=frame||
                mdl::Mdl(source->model)->displayKeys!=pool||mdl::KeyPoolCapacity(pool)!=capacity)return false;
            const auto& m=*mdl::Mdl(source->model);
            if(growth&&!growth->IsCurrent())return false;
            if(source->registration) {
                if(m.boneKeys!=markedBones||m.morphKeys!=markedMorphs||mdl::BoneKeyCapacity(markedBones)!=markedBoneCapacity||
                    mdl::MorphKeyCapacity(markedMorphs)!=markedMorphCapacity||m.maxFrame!=currentMax||m.searchCursor!=currentSearch||
                    source->app.state.lastRegisteredFrame!=currentLast)return false;
                for(std::size_t i=0;i<currentBoneMarks.size();++i)if(markedBones[i].allocated!=currentBoneMarks[i])return false;
                for(std::size_t i=0;i<currentMorphMarks.size();++i)if(markedMorphs[i].allocated!=currentMorphMarks[i])return false;
                for(std::size_t i=0;i<currentDisplayMarks.size();++i)if(pool[i].allocated!=currentDisplayMarks[i])return false;
            }
            if(m.loadComplete!=before.visible)return false;
            for(unsigned index=0;index<source->ikCount;++index)if(m.ikChains[index].enabled!=before.ik[index])return false;
            if(source->parentCount&&std::memcmp(m.boneOrderTable,before.selectors.data(),source->parentCount*sizeof(mdl::BoneOrderEntry)))return false;
            for(const auto& ref:references) {
                if(!ref.owner->Present()||source->app.ModelSlot(ref.slot)!=ref.model||!SceneObjectPresent(ref.model,ref.owner->Id()))return false;
                const auto& other=*mdl::Mdl(ref.model);
                if(other.boneTable!=ref.bones||other.boneCount!=ref.count)return false;
            }
            // Also freeze unmodified buffers: replacing one with an alias of
            // a written payload must not silently turn this into a wider edit.
            for(std::size_t index=0;index<capacity;++index)
                if(pool[index].ikStates!=pointers[index].first||pool[index].selectorStates!=pointers[index].second)return false;
            for(std::size_t index=0;index<source->patches.size();++index) {
                const auto slot=source->patches[index].slot;
                const auto& key=slot<capacity?pool[slot]:growth->Target()[slot];
                if(!current[index].Equals(key,true))return false;
            }
            for(const auto& dependency:dependencies)if(!dependency.payload.Equals(pool[dependency.slot],true))return false;
            if(bones) {
                if(m.boneTable!=bones||m.boneCount!=boneCount||m.boneKeys!=bonePool||
                    mdl::BoneKeyPoolIdentity(bonePool)!=boneIdentity||mdl::BoneKeyCapacity(bonePool)!=boneCapacity)return false;
                for(unsigned index=0;index<boneCount;++index)if(bones[index].slotIndex!=boneSelectors[index])return false;
                for(const auto& key:boneKeys)if(std::memcmp(&bonePool[key.first],&key.second,sizeof(mdl::BoneKey)))return false;
            }
            return (!scaleGuard||scaleGuard->IsCurrent())&&(!curveGuard||curveGuard->IsCurrent())&&
                (!evaluation||evaluation->IsCurrent());
        }
        void Publish() noexcept override {
            // Its strict old-selector guard runs before the display state is
            // replaced. Aggregates instead publish their bone participant first.
            if(evaluation)evaluation->Publish();
            if(growth)growth->Publish();
            auto& m=*mdl::Mdl(source->model);
            if(source->registration) {
                for(std::size_t i=0;i<source->boneMarks.size();++i)m.boneKeys[i].allocated=forward?0:source->boneMarks[i];
                for(std::size_t i=0;i<source->morphMarks.size();++i)m.morphKeys[i].allocated=forward?0:source->morphMarks[i];
                for(std::size_t i=0;i<source->displayMarks.size();++i)m.displayKeys[i].allocated=forward?0:source->displayMarks[i];
                m.maxFrame=forward?source->maxAfter:source->maxBefore;m.searchCursor=forward?source->searchAfter:source->searchBefore;
                source->app.state.lastRegisteredFrame=forward?source->lastAfter:source->lastBefore;
            }
            for(const auto& patch:source->patches) {
                const auto mark=m.displayKeys[patch.slot].allocated;
                (forward?patch.after:patch.before).Publish(m.displayKeys[patch.slot]);
                if(source->aggregatePaste)m.displayKeys[patch.slot].allocated=mark;
            }
            m.loadComplete=after.visible;
            for(unsigned index=0;index<source->ikCount;++index)m.ikChains[index].enabled=after.ik[index];
            if(source->parentCount)std::memcpy(m.boneOrderTable,after.selectors.data(),source->parentCount*sizeof(mdl::BoneOrderEntry));
            source->publishedOnce=true;++source->generation;published=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();if(!Structure())return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            auto& m=*mdl::Mdl(model);CheckPayloadStorage(m);
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            candidate->initial=registration&&!paste&&!publishedOnce;
            if(aggregatePaste&&!publishedOnce&&!forward)return MMS_E_STALE;
            if(registration||aggregatePaste) {
                const auto required=patches.empty()?capacity:std::size_t(patches.back().slot)+1;
                candidate->growth=std::make_unique<DisplayGrowth>(m,required);
            }
            if(registration) {
                if((!publishedOnce&&!forward)||m.maxFrame!=(forward?maxBefore:maxAfter)||m.searchCursor!=(forward?searchBefore:searchAfter)||
                    app.state.lastRegisteredFrame!=(forward?lastBefore:lastAfter))return MMS_E_STALE;
                candidate->markedBones=m.boneKeys;candidate->markedMorphs=m.morphKeys;
                candidate->markedBoneCapacity=mdl::BoneKeyCapacity(m.boneKeys);
                candidate->markedMorphCapacity=mdl::MorphKeyCapacity(m.morphKeys);
                candidate->currentMax=m.maxFrame;candidate->currentLast=app.state.lastRegisteredFrame;candidate->currentSearch=m.searchCursor;
                for(std::size_t i=0;i<boneMarks.size();++i)candidate->currentBoneMarks.push_back(m.boneKeys[i].allocated);
                for(std::size_t i=0;i<morphMarks.size();++i)candidate->currentMorphMarks.push_back(m.morphKeys[i].allocated);
                for(std::size_t i=0;i<displayMarks.size();++i)candidate->currentDisplayMarks.push_back(m.displayKeys[i].allocated);
                if(!publishedOnce) {
                    if(candidate->currentBoneMarks!=boneMarks||candidate->currentMorphMarks!=morphMarks||candidate->currentDisplayMarks!=displayMarks)return MMS_E_STALE;
                }
                if(candidate->initial) {
                    const auto found=std::lower_bound(patches.begin(),patches.end(),registeredSlot,
                        [](const Patch& patch,unsigned index){return patch.slot<index;});
                    if(found==patches.end()||found->slot!=registeredSlot||m.loadComplete!=found->after.value.visible)return MMS_E_STALE;
                    for(unsigned i=0;i<ikCount;++i)if(m.ikChains[i].enabled!=found->after.ik[i])return MMS_E_STALE;
                    const auto* order=static_cast<const mdl::BoneOrderEntry*>(m.boneOrderTable);
                    for(unsigned i=0;i<parentCount;++i)if(order[i].linkedModel!=found->after.parents[i].modelIndex||order[i].linkedBone!=found->after.parents[i].boneIndex)return MMS_E_STALE;
                }
            }
            for(const auto& entry:Chain(m))candidate->dependencies.push_back({entry.slot,Payload::Capture(m.displayKeys[entry.slot],ikCount,parentCount)});
            candidate->current.reserve(patches.size());candidate->pointers.reserve(candidate->capacity);
            for(std::size_t index=0;index<candidate->capacity;++index)
                candidate->pointers.emplace_back(m.displayKeys[index].ikStates,m.displayKeys[index].selectorStates);
            for(const auto& patch:patches) {
                if(patch.slot>=candidate->capacity&&!candidate->growth)return MMS_E_STALE;
                const auto& key=patch.slot<candidate->capacity?m.displayKeys[patch.slot]:candidate->growth->Target()[patch.slot];
                if(!(forward?patch.before:patch.after).Equals(key,false))return MMS_E_STALE;
                candidate->current.push_back(Payload::Capture(key,ikCount,parentCount));
            }
            if(auto status=Sample(forward,candidate->after))return status;
            candidate->before.visible=m.loadComplete;
            for(unsigned index=0;index<ikCount;++index)candidate->before.ik.push_back(m.ikChains[index].enabled);
            if(parentCount) {
                const auto* selectors=static_cast<const mdl::BoneOrderEntry*>(m.boneOrderTable);
                candidate->before.selectors.assign(selectors,selectors+parentCount);
            }
            if(candidate->initial) {
                candidate->after.visible=candidate->before.visible;candidate->after.ik=candidate->before.ik;
                candidate->after.selectors=candidate->before.selectors;
            }
            // Freeze every external owner whose bounds were read while
            // validating target pairs, including keys outside this interval.
            for(auto reference:candidate->after.references) {
                auto* target=app.ModelSlot(reference);const auto& other=*mdl::Mdl(target);
                candidate->references.push_back({reference,target,other.boneTable,other.boneCount,
                    RetainSceneObject(app,MMS_OBJECT_MODEL,reference)});
            }
            if(evaluateBones&&parentCount&&!candidate->initial) {
                if(!m.boneTable||!m.bonePhysicsState||!mdl::BoneKeyPoolIdentity(m.boneKeys))return MMS_E_NOT_READY;
                candidate->bones=m.boneTable;candidate->boneCount=m.boneCount;
                candidate->bonePool=m.boneKeys;candidate->boneCapacity=mdl::BoneKeyCapacity(m.boneKeys);
                candidate->boneIdentity=mdl::BoneKeyPoolIdentity(m.boneKeys);
                std::vector<unsigned> affected;std::set<unsigned> visited;
                for(unsigned index=0;index<m.boneCount;++index) {
                    candidate->boneSelectors.push_back(m.boneTable[index].slotIndex);
                    if(m.boneTable[index].slotIndex<0)continue;
                    if(unsigned(m.boneTable[index].slotIndex)>=parentCount)return MMS_E_ARGUMENT;
                    affected.push_back(index);if(m.bonePhysicsState[index])continue;
                    unsigned key=index;
                    for(std::size_t hops=0;;++hops) {
                        if(hops>=candidate->boneCapacity||key>=candidate->boneCapacity||!visited.insert(key).second)return MMS_E_ARGUMENT;
                        candidate->boneKeys.emplace_back(key,m.boneKeys[key]);
                        if(!m.boneKeys[key].next)break;key=m.boneKeys[key].next;
                    }
                }
                if(!affected.empty()) {
                    const auto scale=bone_scale::AuthoringState::Capture(model);
                    const auto curves=bone_curve::AuthoringState::Capture(model);
                    candidate->scaleGuard=bone_scale::AuthoringRestore::Prepare(model,scale,candidate->boneCapacity);
                    candidate->curveGuard=bone_curve::AuthoringRestore::Prepare(model,*curves);
                    BoneEvaluationView view;view.context=candidate.get();view.keyCapacity=candidate->boneCapacity;
                    view.key=[](const void* context,std::uint32_t index) noexcept -> const mdl::BoneKey* {
                        const auto& candidate=*static_cast<const Candidate*>(context);
                        return index<candidate.boneCapacity?&candidate.bonePool[index]:nullptr;
                    };
                    view.scale=&scale;view.curves=curves.get();view.selectors=candidate->after.selectors.data();view.selectorCount=parentCount;
                    if(auto status=PrepareBoneEvaluation(app,modelSlot,affected,view,candidate->evaluation))return status;
                }
            }
            if(!candidate->IsCurrent())return MMS_E_STALE;
            out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);AddBytes(bytes,patches.capacity(),sizeof(Patch));
        AddBytes(bytes,names.capacity(),sizeof(NameRule));AddBytes(bytes,ikBones.capacity(),sizeof(int));AddBytes(bytes,parentBones.capacity(),sizeof(int));
        AddBytes(bytes,pasteReferences.capacity(),sizeof(PasteReference));
        AddBytes(bytes,boneMarks.capacity(),1);AddBytes(bytes,morphMarks.capacity(),1);AddBytes(bytes,displayMarks.capacity(),1);
        for(const auto& patch:patches)for(const auto* payload:{&patch.before,&patch.after}) {
            AddBytes(bytes,payload->ik.capacity(),sizeof(unsigned char));
            AddBytes(bytes,payload->parents.capacity(),sizeof(mdl::BoneReference));
        }
        return bytes;
    }
};
}
MmsResult PlanDisplayKeyDeletion(MMDApp& app,unsigned slot,std::shared_ptr<HistoryChange>& out,bool allowOtherMarks,bool evaluateBones) noexcept {
    out.reset();
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult {
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!mdl::KeyPoolIdentity(model.displayKeys)||
            (model.boneKeys&&!mdl::BoneKeyPoolIdentity(model.boneKeys))||
            (model.morphKeys&&!mdl::MorphKeyPoolIdentity(model.morphKeys)))return MMS_E_NOT_READY;
        if(!allowOtherMarks&&(Marked(model.boneKeys,mdl::BoneKeyCapacity(model.boneKeys))||
            Marked(model.morphKeys,mdl::MorphKeyCapacity(model.morphKeys))))return MMS_E_NOT_READY;
        const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
        if(!Marked(model.displayKeys,capacity))return MMS_OK;
        const auto chain=Chain(model);CheckPayloadStorage(model);
        std::set<unsigned> reachable;for(const auto& dependency:chain)reachable.insert(dependency.slot);
        auto change=std::make_shared<DisplayDeletion>(app,slot,evaluateBones);
        std::map<unsigned,Patch> planned;
        const auto write=[&](unsigned index)->Payload& {
            auto found=planned.find(index);
            if(found==planned.end()) {
                Patch patch;patch.slot=index;patch.before=Payload::Capture(model.displayKeys[index],model.ikChainCount,model.boneOrderCount);
                patch.after=patch.before;found=planned.emplace(index,std::move(patch)).first;
            }
            return found->second.after;
        };
        for(std::size_t index=0;index<capacity;++index)if(model.displayKeys[index].allocated) {
            if(!reachable.count(unsigned(index)))return MMS_E_ARGUMENT;
            auto& payload=write(unsigned(index));auto& key=payload.value;
            const auto previous=key.previous,next=key.next;
            key.frame=0;
            if(index){write(previous).value.next=next;write(next).value.previous=previous;}
            key.allocated=0;key.visible=1;
            std::fill(payload.ik.begin(),payload.ik.end(),1);
            for(auto& parent:payload.parents){parent.modelIndex=-1;parent.boneIndex=0;}
            if(index)key.next=0;key.previous=0;
        }
        change->patches.reserve(planned.size());
        for(auto& pair:planned)change->patches.push_back(std::move(pair.second));
        out=std::move(change);return MMS_OK;
    });
}
MmsResult PlanDisplayRegistration(MMDApp& app,unsigned slot,int frame,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if(frame<0||frame>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult {
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!mdl::KeyPoolIdentity(model.displayKeys)||
            (model.boneKeys&&!mdl::BoneKeyPoolIdentity(model.boneKeys))||
            (model.morphKeys&&!mdl::MorphKeyPoolIdentity(model.morphKeys))||
            (model.ikChainCount&&!model.ikChains)||(model.boneOrderCount&&!model.boneOrderTable))return MMS_E_NOT_READY;
        const auto chain=Chain(model);CheckPayloadStorage(model);
        const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
        std::size_t position=0;
        while(position+1<chain.size()&&chain[position].value.frame<unsigned(frame))++position;
        const unsigned current=chain[position].slot;const auto currentValue=chain[position].value;
        unsigned destination=current;const bool inserting=currentValue.frame!=unsigned(frame);
        if(inserting) {
            std::size_t free=1;while(free<capacity&&model.displayKeys[free].frame)++free;
            if(free>=mdl::KeyPoolLimit<mdl::DisplayKey>())return MMS_E_MEMORY;
            destination=unsigned(free);
        }
        std::map<unsigned,Patch> planned;
        const auto write=[&](unsigned index)->Payload& {
            auto found=planned.find(index);
            if(found==planned.end()) {
                Patch patch;patch.slot=index;
                patch.before=index<capacity?Payload::Capture(model.displayKeys[index],model.ikChainCount,model.boneOrderCount):
                    Payload::Empty(model.ikChainCount,model.boneOrderCount);
                patch.after=patch.before;patch.after.value.allocated=0;
                found=planned.emplace(index,std::move(patch)).first;
            }
            return found->second.after;
        };
        auto& target=write(destination);
        if(inserting) {
            if(currentValue.frame<unsigned(frame)) {
                write(current).value.next=destination;target.value.previous=current;
            } else {
                write(currentValue.previous).value.next=destination;target.value.previous=currentValue.previous;
                write(current).value.previous=destination;target.value.next=current;
            }
            target.value.frame=unsigned(frame);
        }
        target.value.visible=model.loadComplete;target.value.allocated=1;
        for(unsigned index=0;index<model.ikChainCount;++index)target.ik[index]=model.ikChains[index].enabled;
        const auto* order=static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
        for(unsigned index=0;index<model.boneOrderCount;++index)
            target.parents[index]={order[index].linkedModel,order[index].linkedBone};
        // Repeating registration does not create a selection-only history item.
        // In particular it must not discard an existing redo branch.
        if(!inserting&&target.Equals(model.displayKeys[destination],false))return MMS_OK;
        auto change=std::make_shared<DisplayDeletion>(app,slot,true);change->registration=true;
        change->registeredSlot=destination;change->maxBefore=model.maxFrame;
        change->maxAfter=inserting?(std::max)(model.maxFrame,unsigned(frame)):model.maxFrame;
        change->searchBefore=change->searchAfter=model.searchCursor;change->lastBefore=app.state.lastRegisteredFrame;
        change->lastAfter=(std::max)(change->lastBefore,change->maxAfter);
        change->boneMarkIdentity=mdl::BoneKeyPoolIdentity(model.boneKeys);
        change->morphMarkIdentity=mdl::MorphKeyPoolIdentity(model.morphKeys);
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(model.boneKeys);++i)change->boneMarks.push_back(model.boneKeys[i].allocated);
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(model.morphKeys);++i)change->morphMarks.push_back(model.morphKeys[i].allocated);
        for(std::size_t i=0;i<capacity;++i)change->displayMarks.push_back(model.displayKeys[i].allocated);
        change->patches.reserve(planned.size());
        for(auto& entry:planned)change->patches.push_back(std::move(entry.second));
        DisplayDeletion::FrameState sampled;if(auto result=change->Sample(true,sampled))return result;
        out=std::move(change);return MMS_OK;
    });
}
MmsResult PlanDisplayKeyPaste(MMDApp& app,unsigned slot,std::shared_ptr<HistoryChange>& out,ModelPasteFamilyResult* aggregate) noexcept {
    out.reset();if(aggregate)*aggregate={};
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    ModelPasteSource source;
    if(auto result=CaptureModelPasteSource(app,source,ModelPasteDisplays))return result;
    return PlanDisplayKeyPaste(app,slot,source,out,aggregate);
}
MmsResult PlanDisplayKeyPaste(MMDApp& app,unsigned slot,const ModelPasteSource& input,
    std::shared_ptr<HistoryChange>& out,ModelPasteFamilyResult* aggregate) noexcept {
    out.reset();
    if(aggregate)*aggregate={};
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult {
        if(auto result=ValidateModelPasteSource(input,ModelPasteDisplays))return result;
        const auto count=input.displays.size();
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(aggregate){aggregate->maxFrame=model.maxFrame;aggregate->searchCursor=model.searchCursor;}
        if(!count)return MMS_OK;
        if(count>mdl::KeyPoolLimit<mdl::DisplayKey>()||
            app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        if(!mdl::KeyPoolIdentity(model.displayKeys)||
            (model.boneKeys&&!mdl::BoneKeyPoolIdentity(model.boneKeys))||
            (model.morphKeys&&!mdl::MorphKeyPoolIdentity(model.morphKeys))||
            (model.ikChainCount&&!model.ikChains)||(model.boneOrderCount&&!model.boneOrderTable)||
            extended_names::importedIkTargets)return MMS_E_NOT_READY;
        // Display payloads apply flags to each IK/selector entry by name.
        // They do not choose one bone destination, so duplicate helper or IK
        // names are valid and retain the legacy broadcast matching below.
        if(model.boneCount&&!model.boneTable)return MMS_E_NOT_READY;
        const bool resolved=std::all_of(input.displays.begin(),input.displays.end(),[](const auto& source){
            return source.ikTargetsResolved&&source.selectors.empty();});
        Chain(model);CheckPayloadStorage(model);
        const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
        auto change=std::make_shared<DisplayDeletion>(app,slot,aggregate==nullptr);
        change->registration=aggregate==nullptr;change->paste=true;change->aggregatePaste=aggregate!=nullptr;
        change->maxBefore=change->maxAfter=model.maxFrame;
        change->lastBefore=change->lastAfter=app.state.lastRegisteredFrame;
        change->searchBefore=model.searchCursor;
        unsigned free=1;while(free<capacity&&model.displayKeys[free].frame)++free;
        change->searchAfter=int(free);
        std::set<unsigned> named;
        const auto name=[&](int bone,bool terminated)->const char* {
            if(!model.boneTable||bone<0||unsigned(bone)>=model.boneCount)
                throw std::invalid_argument("display paste mapping bone outside model");
            const auto* text=model.boneTable[bone].name;
            if(terminated&&!std::memchr(text,0,sizeof(model.boneTable[bone].name)))
                throw std::invalid_argument("unterminated display paste parent name");
            if(named.insert(unsigned(bone)).second) {
                DisplayDeletion::NameRule rule{unsigned(bone),{}};
                std::memcpy(rule.name.data(),text,rule.name.size());change->names.push_back(rule);
            }
            return text;
        };
        std::vector<const char*> ikNames,parentNames;
        for(unsigned i=0;i<model.ikChainCount;++i) {
            const auto bone=model.ikChains[i].boneIndex;
            ikNames.push_back(name(bone,false));change->ikBones.push_back(bone);
        }
        const auto* order=static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
        const bool selectorNames=!resolved||std::any_of(input.displays.begin(),input.displays.end(),[](const auto& source){return !source.selectors.empty();});
        for(unsigned i=0;i<model.boneOrderCount;++i) {
            change->parentBones.push_back(order[i].boneIndex);
            parentNames.push_back(i&&selectorNames?name(order[i].boneIndex,true):"");
        }
        if(!change->names.empty()){change->nameTable=model.boneTable;change->nameCount=model.boneCount;}
        std::map<unsigned,Patch> planned;
        const auto write=[&](unsigned index)->Payload& {
            if(index>=mdl::KeyPoolLimit<mdl::DisplayKey>())throw std::bad_alloc();
            auto found=planned.find(index);
            if(found==planned.end()) {
                Patch patch;patch.slot=index;
                patch.before=index<capacity?Payload::Capture(model.displayKeys[index],model.ikChainCount,model.boneOrderCount):
                    Payload::Empty(model.ikChainCount,model.boneOrderCount);
                patch.after=patch.before;patch.after.value.allocated=0;
                found=planned.emplace(index,std::move(patch)).first;
            }
            return found->second.after;
        };
        const auto read=[&](unsigned index) {
            const auto found=planned.find(index);
            return found!=planned.end()?found->second.after.value:
                index<capacity?Read(model.displayKeys[index]):Payload::Empty(0,0).value;
        };
        std::set<unsigned> pastedSlots;
        for(unsigned index=0;index<count;++index) {
            const auto& source=input.displays[index];unsigned frame;
            if(!frame_limits::Add(int(input.frameOffset),source.frame,frame))return MMS_E_ARGUMENT;
            if(source.ikTargetsResolved)for(int target:source.ikTargets)
                if(target>=int(model.boneCount))return MMS_E_ARGUMENT;
            std::vector<mdl::BoneReference> parents;
            for(const auto& entry:source.selectors) {
                if(!std::memchr(entry.boneName,0,sizeof(entry.boneName)))return MMS_E_ARGUMENT;
                mdl::BoneReference pair{entry.modelIndex,entry.boneIndex};
                if(pair.modelIndex< -1||pair.modelIndex>=int(kModelSlotCount))return MMS_E_ARGUMENT;
                if(pair.modelIndex>=0) {
                    const auto* target=app.ModelSlot(unsigned(pair.modelIndex));
                    if(!target){pair.modelIndex=-1;pair.boneIndex=0;}
                    else if(pair.boneIndex<0)return MMS_E_ARGUMENT;
                    else if(unsigned(pair.boneIndex)>=mdl::Mdl(target)->boneCount) {
                        if(!mdl::Mdl(target)->boneCount)return MMS_E_ARGUMENT;
                        pair.boneIndex=0;
                    }
                }
                parents.push_back(pair);
            }
            unsigned current=0;std::size_t hops=0;
            while(read(current).frame<frame&&read(current).next) {
                if(++hops>=capacity+std::size_t(count))return MMS_E_ARGUMENT;
                current=read(current).next;
            }
            const auto currentValue=read(current);unsigned destination=current;
            if(currentValue.frame!=frame) {
                destination=1;
                while(std::size_t(destination)<mdl::KeyPoolLimit<mdl::DisplayKey>()&&read(destination).frame)++destination;
                if(std::size_t(destination)>=mdl::KeyPoolLimit<mdl::DisplayKey>())return MMS_E_MEMORY;
                auto& target=write(destination).value;
                if(currentValue.frame<frame) {
                    write(current).value.next=destination;target.previous=current;target.next=0;
                } else {
                    write(currentValue.previous).value.next=destination;target.previous=currentValue.previous;
                    write(current).value.previous=destination;target.next=current;
                }
                target.frame=frame;change->maxAfter=(std::max)(change->maxAfter,frame);
            }
            auto& target=write(destination);target.value.visible=source.visible;target.value.allocated=1;
            pastedSlots.insert(destination);
            for(unsigned i=0;i<ikNames.size();++i)for(unsigned entry=0;entry<source.ik.size();++entry)
                if(source.ikTargetsResolved?source.ikTargets[entry]==model.ikChains[i].boneIndex:
                    !std::strncmp(ikNames[i],source.ik[entry].boneName,sizeof(mdl::BoneRecord::name))) {
                    target.ik[i]=source.ik[entry].enabled;break;
                }
            for(unsigned i=0;i<parentNames.size();++i) {
                if(parents.empty())continue;
                if(!i){target.parents[0]=parents[0];continue;}
                for(unsigned entry=0;entry<source.selectors.size();++entry)
                    if(!std::strcmp(parentNames[i],source.selectors[entry].boneName)) {
                        target.parents[i]=parents[entry];break;
                    }
            }
        }
        bool changed=false;
        for(const auto& entry:planned)
            changed|=entry.first>=capacity||!entry.second.after.Equals(model.displayKeys[entry.first],false);
        if(!changed&&!aggregate)return MMS_OK;
        if(!aggregate) {
            change->boneMarkIdentity=mdl::BoneKeyPoolIdentity(model.boneKeys);
            change->morphMarkIdentity=mdl::MorphKeyPoolIdentity(model.morphKeys);
            for(std::size_t i=0;i<mdl::BoneKeyCapacity(model.boneKeys);++i)change->boneMarks.push_back(model.boneKeys[i].allocated);
            for(std::size_t i=0;i<mdl::MorphKeyCapacity(model.morphKeys);++i)change->morphMarks.push_back(model.morphKeys[i].allocated);
            for(std::size_t i=0;i<capacity;++i)change->displayMarks.push_back(model.displayKeys[i].allocated);
        }
        for(auto& entry:planned)change->patches.push_back(std::move(entry.second));
        DisplayDeletion::FrameState sampled,original;
        if(auto result=change->Sample(true,sampled))return result;
        if(auto result=change->Sample(false,original))return result;
        std::set<unsigned> references(sampled.references.begin(),sampled.references.end());
        references.insert(original.references.begin(),original.references.end());
        for(auto reference:references) {
            auto* target=app.ModelSlot(reference);const auto& state=*mdl::Mdl(target);
            change->pasteReferences.push_back({reference,target,state.boneTable,state.boneCount,
                RetainSceneObject(app,MMS_OBJECT_MODEL,reference)});
        }
        if(aggregate) {
            aggregate->hasKeys=true;aggregate->dataChanged=changed;aggregate->maxFrame=change->maxAfter;
            aggregate->searchCursor=change->searchAfter;aggregate->markedSlots.assign(pastedSlots.begin(),pastedSlots.end());
            aggregate->change=change;
        }
        out=std::move(change);return MMS_OK;
    });
}
MmsResult PlanDisplayRangeScale(MMDApp& app,unsigned slot,const BoneRangeScaleOptions& options,RangeScaleResult& out) noexcept {
    if(options.start>=options.end||options.end>unsigned(frame_limits::maximum)||!std::isfinite(options.scale)||options.scale<=0)
        return MMS_E_ARGUMENT;
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult {
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!mdl::KeyPoolIdentity(model.displayKeys)||
            (model.boneKeys&&!mdl::BoneKeyPoolIdentity(model.boneKeys))||
            (model.morphKeys&&!mdl::MorphKeyPoolIdentity(model.morphKeys)))return MMS_E_NOT_READY;
        RangeScaleResult result;result.maxFrame=model.maxFrame;result.searchCursor=model.searchCursor;
        const auto chain=Chain(model);CheckPayloadStorage(model);
        const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
        std::set<unsigned> reachable;for(const auto& entry:chain)reachable.insert(entry.slot);
        struct Copy { unsigned frame;Payload payload; };
        std::vector<Copy> copies;
        const auto mapped=[&](unsigned frame) {
            const auto span=double(frame<=options.end?frame-options.start:options.end-options.start)*options.scale;
            if(!std::isfinite(span)||span<0||span>double(frame_limits::maximum))throw std::invalid_argument("display range overflow");
            const auto target=std::uint64_t(options.start)+std::uint64_t(span)+
                (frame>options.end?std::uint64_t(frame-options.end):0);
            if(target>std::uint64_t(frame_limits::maximum))throw std::invalid_argument("display range overflow");
            return unsigned(target);
        };
        for(std::size_t index=0;index<capacity;++index) {
            const auto& key=model.displayKeys[index];if(!key.frame||key.frame<options.start)continue;
            if(!reachable.count(unsigned(index)))return MMS_E_ARGUMENT;
            const auto frame=mapped(key.frame);result.moved|=frame!=key.frame;
            copies.push_back({frame,Payload::Capture(key,model.ikChainCount,model.boneOrderCount)});
        }
        result.hasKeys=!copies.empty();
        if(!result.hasKeys){out=std::move(result);return MMS_OK;}
        if(extended_names::importedIkTargets)return MMS_E_NOT_READY;
        if((model.ikChainCount&&!model.ikChains)||(model.boneOrderCount&&!model.boneOrderTable))return MMS_E_NOT_READY;
        auto change=std::make_shared<DisplayDeletion>(app,slot,false);
        std::vector<unsigned> ikMapping,parentMapping;
        std::vector<const char*> ikNames,parentNames;
        std::set<unsigned> named;
        const auto name=[&](int bone)->const char* {
            if(bone<0||unsigned(bone)>=model.boneCount)throw std::invalid_argument("display mapping bone outside model");
            if(!model.boneTable)return nullptr;
            const char* text=model.boneTable[bone].name;
            // The legacy range producer uses strcpy_s(...,20), so a full
            // nonterminated name has no defined transferable representation.
            if(!std::memchr(text,0,sizeof(model.boneTable[bone].name)))return nullptr;
            if(named.insert(unsigned(bone)).second) {
                DisplayDeletion::NameRule rule{unsigned(bone),{}};std::memcpy(rule.name.data(),text,rule.name.size());
                change->names.push_back(rule);
            }
            return text;
        };
        for(unsigned index=0;index<model.ikChainCount;++index) {
            const auto bone=model.ikChains[index].boneIndex;const auto* text=name(bone);if(!text)return MMS_E_NOT_READY;
            change->ikBones.push_back(bone);ikNames.push_back(text);
        }
        const auto* selectors=static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
        for(unsigned index=0;index<model.boneOrderCount;++index) {
            change->parentBones.push_back(selectors[index].boneIndex);
            const auto* text=index?name(selectors[index].boneIndex):"";if(!text)return MMS_E_NOT_READY;
            parentNames.push_back(text);
        }
        if(!change->names.empty()){change->nameTable=model.boneTable;change->nameCount=model.boneCount;}
        // The registrar searches from the first encoded entry. Duplicate IK
        // names therefore share its first flag; unnamed parent bones match
        // the model-wide slot-zero record, whose encoded name is empty.
        for(const auto* text:ikNames) {
            unsigned first=0;while(std::strcmp(text,ikNames[first]))++first;ikMapping.push_back(first);
        }
        for(unsigned index=0;index<parentNames.size();++index) {
            unsigned first=0;if(index)while(std::strcmp(parentNames[index],parentNames[first]))++first;
            parentMapping.push_back(first);
        }
        std::map<unsigned,Patch> planned;
        const auto write=[&](unsigned index)->Payload& {
            if(index>=capacity)throw std::invalid_argument("display range slot outside pool");
            auto found=planned.find(index);
            if(found==planned.end()) {
                Patch patch;patch.slot=index;patch.before=Payload::Capture(model.displayKeys[index],model.ikChainCount,model.boneOrderCount);
                patch.after=patch.before;found=planned.emplace(index,std::move(patch)).first;
            }
            return found->second.after;
        };
        const auto read=[&](unsigned index) {
            if(index>=capacity)throw std::invalid_argument("display range link outside pool");
            const auto found=planned.find(index);return found==planned.end()?Read(model.displayKeys[index]):found->second.after.value;
        };
        for(std::size_t index=0;index<capacity;++index)if(model.displayKeys[index].allocated)write(unsigned(index)).value.allocated=0;
        for(std::size_t index=1;index<capacity;++index) {
            const auto& original=model.displayKeys[index];if(!original.frame||original.frame<options.start)continue;
            auto& payload=write(unsigned(index));auto& key=payload.value;const auto previous=key.previous,next=key.next;
            key.frame=0;write(previous).value.next=next;write(next).value.previous=previous;
            key.allocated=0;key.visible=1;key.next=0;key.previous=0;
            std::fill(payload.ik.begin(),payload.ik.end(),1);
            for(auto& parent:payload.parents){parent.modelIndex=-1;parent.boneIndex=0;}
        }
        unsigned cursor=1;while(cursor<capacity&&read(cursor).frame)++cursor;
        result.searchCursor=int(cursor); // DisplayKeyFreeSlot never advances this field.
        for(const auto& copy:copies) {
            unsigned current=0;std::size_t hops=0;
            while(read(current).frame<copy.frame&&read(current).next) {
                if(++hops>=capacity)return MMS_E_ARGUMENT;current=read(current).next;
            }
            const auto currentValue=read(current);unsigned destination=current;
            if(currentValue.frame!=copy.frame) {
                destination=1;while(destination<capacity&&read(destination).frame)++destination;
                if(destination>=capacity)return MMS_E_NOT_READY;
                auto& target=write(destination).value;
                if(currentValue.frame<copy.frame) {
                    write(current).value.next=destination;target.previous=current;
                } else {
                    write(currentValue.previous).value.next=destination;target.previous=currentValue.previous;
                    write(current).value.previous=destination;target.next=current;
                }
                target.frame=copy.frame;result.maxFrame=(std::max)(result.maxFrame,copy.frame);
            }
            auto& target=write(destination);target.value.visible=copy.payload.value.visible;target.value.allocated=1;
            for(unsigned index=0;index<ikMapping.size();++index)target.ik[index]=copy.payload.ik[ikMapping[index]];
            for(unsigned index=0;index<parentMapping.size();++index)target.parents[index]=copy.payload.parents[parentMapping[index]];
        }
        for(std::size_t index=0;index<capacity;++index)if(read(unsigned(index)).allocated)result.markedSlots.push_back(unsigned(index));
        change->patches.reserve(planned.size());
        for(auto& entry:planned)if(!entry.second.after.Equals(model.displayKeys[entry.first],true))change->patches.push_back(std::move(entry.second));
        // Validate the target track and external references before returning a
        // usable record; this does not publish its derived state or selectors.
        DisplayDeletion::FrameState target;if(auto status=change->Sample(true,target))return status;
        result.change=std::move(change);out=std::move(result);return MMS_OK;
    });
}
}
#endif
