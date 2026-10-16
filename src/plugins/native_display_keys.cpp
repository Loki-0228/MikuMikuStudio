#include "native_display_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
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
struct DisplayDeletion final : HistoryChange,std::enable_shared_from_this<DisplayDeletion> {
    MMDApp& app;unsigned modelSlot;unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    std::uint64_t identity;std::size_t capacity;
    unsigned ikCount,parentCount;
    const void* ikTable;const void* parentTable;
    std::vector<Patch> patches;
    DisplayDeletion(MMDApp& a,unsigned slot):app(a),modelSlot(slot),model(a.ModelSlot(slot)),
        lifetime(RetainSceneObject(a,MMS_OBJECT_MODEL,slot)),identity(mdl::KeyPoolIdentity(mdl::Mdl(model)->displayKeys)),
        capacity(mdl::KeyPoolCapacity(mdl::Mdl(model)->displayKeys)),ikCount(mdl::Mdl(model)->ikChainCount),
        parentCount(mdl::Mdl(model)->boneOrderCount),ikTable(mdl::Mdl(model)->ikChains),parentTable(mdl::Mdl(model)->boneOrderTable) {}
    bool Structure() const noexcept {
        if(!lifetime||!lifetime->Present()||app.ModelSlot(modelSlot)!=model||!SceneObjectPresent(model,lifetime->Id()))return false;
        const auto& m=*mdl::Mdl(model);
        return mdl::KeyPoolIdentity(m.displayKeys)==identity&&mdl::KeyPoolCapacity(m.displayKeys)>=capacity&&
            m.ikChainCount==ikCount&&m.boneOrderCount==parentCount&&m.ikChains==ikTable&&m.boneOrderTable==parentTable;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<DisplayDeletion> source;bool forward;
        mdl::DisplayKey* pool;std::size_t capacity;
        std::vector<Payload> current;
        std::vector<std::pair<unsigned char*,mdl::BoneReference*>> pointers;
        std::vector<Dependency> dependencies;
        Candidate(std::shared_ptr<DisplayDeletion> s,bool f):source(std::move(s)),forward(f),
            pool(mdl::Mdl(source->model)->displayKeys),capacity(mdl::KeyPoolCapacity(pool)) {}
        bool IsCurrent() const noexcept override {
            if(!source->Structure()||mdl::Mdl(source->model)->displayKeys!=pool||mdl::KeyPoolCapacity(pool)!=capacity)return false;
            // Also freeze unmodified buffers: replacing one with an alias of
            // a written payload must not silently turn this into a wider edit.
            for(std::size_t index=0;index<capacity;++index)
                if(pool[index].ikStates!=pointers[index].first||pool[index].selectorStates!=pointers[index].second)return false;
            for(std::size_t index=0;index<source->patches.size();++index) {
                const auto& key=pool[source->patches[index].slot];
                if(!current[index].Equals(key,true))return false;
            }
            for(const auto& dependency:dependencies)if(!Equal(dependency.value,Read(pool[dependency.slot]),true))return false;
            return true;
        }
        void Publish() noexcept override {
            for(const auto& patch:source->patches)(forward?patch.after:patch.before).Publish(pool[patch.slot]);
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();if(!Structure())return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            auto& m=*mdl::Mdl(model);CheckPayloadStorage(m);
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            candidate->dependencies=Chain(m);candidate->current.reserve(patches.size());candidate->pointers.reserve(candidate->capacity);
            for(std::size_t index=0;index<candidate->capacity;++index)
                candidate->pointers.emplace_back(m.displayKeys[index].ikStates,m.displayKeys[index].selectorStates);
            for(const auto& patch:patches) {
                const auto& key=m.displayKeys[patch.slot];
                if(!(forward?patch.before:patch.after).Equals(key,false))return MMS_E_STALE;
                candidate->current.push_back(Payload::Capture(key,ikCount,parentCount));
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
MmsResult PlanDisplayKeyDeletion(MMDApp& app,unsigned slot,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult {
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!mdl::KeyPoolIdentity(model.displayKeys)||
            (model.boneKeys&&!mdl::BoneKeyPoolIdentity(model.boneKeys))||
            (model.morphKeys&&!mdl::MorphKeyPoolIdentity(model.morphKeys)))return MMS_E_NOT_READY;
        if(Marked(model.boneKeys,mdl::BoneKeyCapacity(model.boneKeys))||
            Marked(model.morphKeys,mdl::MorphKeyCapacity(model.morphKeys)))return MMS_E_NOT_READY;
        const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
        if(!Marked(model.displayKeys,capacity))return MMS_OK;
        const auto chain=Chain(model);CheckPayloadStorage(model);
        std::set<unsigned> reachable;for(const auto& dependency:chain)reachable.insert(dependency.slot);
        auto change=std::make_shared<DisplayDeletion>(app,slot);
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
