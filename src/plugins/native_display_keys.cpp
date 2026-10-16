#include "native_display_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "native_bone_evaluation.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <map>
#include <set>

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
struct DisplayDeletion final : DisplayKeyDeletion,std::enable_shared_from_this<DisplayDeletion> {
    MMDApp& app;unsigned modelSlot;unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    std::uint64_t identity;std::size_t capacity;
    unsigned ikCount,parentCount;
    const void* ikTable;const void* parentTable;
    bool evaluateBones;
    std::vector<Patch> patches;
    DisplayDeletion(MMDApp& a,unsigned slot,bool evaluate):app(a),modelSlot(slot),model(a.ModelSlot(slot)),
        lifetime(RetainSceneObject(a,MMS_OBJECT_MODEL,slot)),identity(mdl::KeyPoolIdentity(mdl::Mdl(model)->displayKeys)),
        capacity(mdl::KeyPoolCapacity(mdl::Mdl(model)->displayKeys)),ikCount(mdl::Mdl(model)->ikChainCount),
        parentCount(mdl::Mdl(model)->boneOrderCount),ikTable(mdl::Mdl(model)->ikChains),parentTable(mdl::Mdl(model)->boneOrderTable),evaluateBones(evaluate) {}
    bool Structure() const noexcept {
        if(!lifetime||!lifetime->Present()||app.ModelSlot(modelSlot)!=model||!SceneObjectPresent(model,lifetime->Id()))return false;
        const auto& m=*mdl::Mdl(model);
        return mdl::KeyPoolIdentity(m.displayKeys)==identity&&mdl::KeyPoolCapacity(m.displayKeys)>=capacity&&
            m.ikChainCount==ikCount&&m.boneOrderCount==parentCount&&m.ikChains==ikTable&&m.boneOrderTable==parentTable;
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
        const auto cap=mdl::KeyPoolCapacity(m.displayKeys);
        std::vector<DeepDependency> chain;std::set<unsigned> seen,referenced;
        unsigned slot=0,previous=0,frame=0;
        for(std::size_t hops=0;;++hops) {
            if(hops>=cap||slot>=cap||!seen.insert(slot).second)return MMS_E_ARGUMENT;
            const auto found=std::lower_bound(patches.begin(),patches.end(),slot,
                [](const Patch& patch,unsigned index){return patch.slot<index;});
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
            for(const auto& patch:patches)
                if(!(forward?patch.before:patch.after).Equals(m.displayKeys[patch.slot],false))return MMS_E_STALE;
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
        bool published=false;
        Candidate(std::shared_ptr<DisplayDeletion> s,bool f):source(std::move(s)),forward(f),
            pool(mdl::Mdl(source->model)->displayKeys),capacity(mdl::KeyPoolCapacity(pool)),frame(source->app.CurrentFrame()) {}
        bool IsCurrent() const noexcept override {
            if(published||!source->Structure()||source->app.CurrentFrame()!=frame||
                mdl::Mdl(source->model)->displayKeys!=pool||mdl::KeyPoolCapacity(pool)!=capacity)return false;
            const auto& m=*mdl::Mdl(source->model);
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
                const auto& key=pool[source->patches[index].slot];
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
            for(const auto& patch:source->patches)(forward?patch.after:patch.before).Publish(pool[patch.slot]);
            auto& m=*mdl::Mdl(source->model);m.loadComplete=after.visible;
            for(unsigned index=0;index<source->ikCount;++index)m.ikChains[index].enabled=after.ik[index];
            if(source->parentCount)std::memcpy(m.boneOrderTable,after.selectors.data(),source->parentCount*sizeof(mdl::BoneOrderEntry));
            published=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();if(!Structure())return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            auto& m=*mdl::Mdl(model);CheckPayloadStorage(m);
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            for(const auto& entry:Chain(m))candidate->dependencies.push_back({entry.slot,Payload::Capture(m.displayKeys[entry.slot],ikCount,parentCount)});
            candidate->current.reserve(patches.size());candidate->pointers.reserve(candidate->capacity);
            for(std::size_t index=0;index<candidate->capacity;++index)
                candidate->pointers.emplace_back(m.displayKeys[index].ikStates,m.displayKeys[index].selectorStates);
            for(const auto& patch:patches) {
                const auto& key=m.displayKeys[patch.slot];
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
            // Freeze every external owner whose bounds were read while
            // validating target pairs, including keys outside this interval.
            for(auto reference:candidate->after.references) {
                auto* target=app.ModelSlot(reference);const auto& other=*mdl::Mdl(target);
                candidate->references.push_back({reference,target,other.boneTable,other.boneCount,
                    RetainSceneObject(app,MMS_OBJECT_MODEL,reference)});
            }
            if(evaluateBones&&parentCount) {
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
}
#endif
