#include "native_model_keys.hpp"
#include "native_key_selection.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_display_keys.hpp"
#include "native_morph_keys.hpp"
#include "native_range_scale.hpp"
#include "native_model_paste.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/clipboard_layout.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
struct Range {
    std::uintptr_t begin, end;
    bool operator==(const Range& b) const noexcept { return begin == b.begin && end == b.end; }
};
// Includes read and write storage across all families. Per-family ownership
// checks alone cannot detect a display payload pointing into another pool.
template<class Visitor> bool VisitStorage(const mdl::ModelRecord& m, Visitor&& visit) {
    const auto add = [&](const void* pointer, std::size_t count, std::size_t stride) {
        const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
        if (count > (UINTPTR_MAX - begin) / stride) return false;
        return visit(Range{begin, pointer ? begin + count * stride : begin});
    };
    if (!add(&m, 1, sizeof(m)) ||
        !add(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), sizeof(mdl::BoneKey)) ||
        !add(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), sizeof(mdl::MorphKey)) ||
        !add(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), sizeof(mdl::DisplayKey)) ||
        !add(m.boneTable, m.boneCount, sizeof(mdl::BoneRecord)) ||
        !add(m.morphs, m.morphCount, sizeof(mdl::MorphRecord)) ||
        !add(m.ikChains, m.ikChainCount, sizeof(mdl::IkChain)) ||
        !add(m.boneOrderTable, m.boneOrderCount, sizeof(mdl::BoneOrderEntry)) ||
        !add(m.bonePhysicsState, m.boneCount, 1) ||
        !add(m.boneSelection, m.boneCount, 1) ||
        !add(m.boneKeyCursors, m.boneCount, sizeof(std::uint32_t)) ||
        !add(m.boneTrackActive, m.boneCount, 1) ||
        !add(m.boneKeyIndices, m.boneCount, sizeof(std::uint32_t)) ||
        !add(m.morphKeyCursors, m.morphCount, sizeof(std::uint32_t)) ||
        !add(m.morphTrackActive, m.morphCount, 1) ||
        !add(m.morphKeyIndices, m.morphCount, sizeof(std::uint32_t))) return false;
    for (unsigned index = 0; m.ikChains && index < m.ikChainCount; ++index)
        if (!add(m.ikChains[index].links, m.ikChains[index].linkCount, sizeof(std::uint16_t))) return false;
    for (std::size_t index = 0; index < mdl::KeyPoolCapacity(m.displayKeys); ++index)
        if (!add(m.displayKeys[index].ikStates, m.ikChainCount, 1) ||
            !add(m.displayKeys[index].selectorStates, m.boneOrderCount, sizeof(mdl::BoneReference))) return false;
    return true;
}
struct Layout {
    struct Peer {const unsigned char* model;std::vector<Range> ranges;};
    std::vector<Range> ranges;
    const MMDApp* peerApp=nullptr;
    unsigned ownSlot=0;
    std::array<unsigned char*,kModelSlotCount> models{};
    std::vector<Peer> peers;
    static std::size_t RangeCount(const mdl::ModelRecord& model) {
        std::size_t count=0;
        if(!VisitStorage(model,[&](const Range&){if(count==SIZE_MAX)return false;++count;return true;}))
            throw std::invalid_argument("model key storage overflow");
        return count;
    }
    static void Charge(const MMDApp& app,unsigned slot,model_delete_budget::Bytes& bytes) {
        const auto count=RangeCount(*mdl::Mdl(app.ModelSlot(slot)));
        bytes.Array<Range>(count); // Retained own ranges.
        bytes.Array<Range>(count); // Sorting scratch, reused by peer validation.
        for(unsigned other=0;other<kModelSlotCount;++other)if(other!=slot&&app.ModelSlot(other)) {
            const auto& peer=*mdl::Mdl(app.ModelSlot(other));
            if((peer.boneKeys&&!mdl::BoneKeyPoolIdentity(peer.boneKeys))||
                (peer.morphKeys&&!mdl::MorphKeyPoolIdentity(peer.morphKeys))||
                (peer.displayKeys&&!mdl::KeyPoolIdentity(peer.displayKeys)))
                throw std::invalid_argument("unmanaged peer model keys");
            bytes.Array<Peer>(1);bytes.Array<Range>(RangeCount(peer));
        }
    }
    explicit Layout(const mdl::ModelRecord& model) {
        ranges.reserve(RangeCount(model));
        if (!VisitStorage(model, [&](const Range& range) { ranges.push_back(range); return true; }))
            throw std::invalid_argument("model key storage overflow");
        auto sorted = ranges;
        sorted.erase(std::remove_if(sorted.begin(), sorted.end(), [](const Range& r) { return r.begin == r.end; }), sorted.end());
        std::sort(sorted.begin(), sorted.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
        for (std::size_t index = 1; index < sorted.size(); ++index)
            if (sorted[index].begin < sorted[index - 1].end)
                throw std::invalid_argument("aliased model key storage");
    }
    void ValidatePeers(const MMDApp& app,unsigned slotOfOwner) {
        peerApp=&app;ownSlot=slotOfOwner;peers.clear();models.fill(nullptr);
        std::size_t peerCount=0;
        for(unsigned slot=0;slot<kModelSlotCount;++slot)if(slot!=ownSlot&&app.ModelSlot(slot))++peerCount;
        peers.reserve(peerCount);
        auto sorted=ranges;
        sorted.erase(std::remove_if(sorted.begin(),sorted.end(),[](const Range& r){return r.begin==r.end;}),sorted.end());
        std::sort(sorted.begin(),sorted.end(),[](const Range& a,const Range& b){return a.begin<b.begin;});
        const auto separate=[&](const Range& other){
            if(other.begin==other.end)return true;
            const auto at=std::lower_bound(sorted.begin(),sorted.end(),other.begin,[](const Range& own,std::uintptr_t value){return own.end<=value;});
            return at==sorted.end()||at->begin>=other.end;
        };
        const auto state=reinterpret_cast<std::uintptr_t>(&app.state);
        if(!separate({state,state+sizeof app.state}))throw std::invalid_argument("model key aliases app state");
        for(unsigned slot=0;slot<kModelSlotCount;++slot)if(slot!=ownSlot&&app.ModelSlot(slot)){
            models[slot]=app.ModelSlot(slot);
            const auto& peer=*mdl::Mdl(app.ModelSlot(slot));
            // Unmanaged peer pools do not provide safe capacities for nested
            // traversal; keep the preparation fail-closed before using them.
            if((peer.boneKeys&&!mdl::BoneKeyPoolIdentity(peer.boneKeys))||
                (peer.morphKeys&&!mdl::MorphKeyPoolIdentity(peer.morphKeys))||
                (peer.displayKeys&&!mdl::KeyPoolIdentity(peer.displayKeys)))throw std::invalid_argument("unmanaged peer model keys");
            Peer snapshot{app.ModelSlot(slot),{}};
            snapshot.ranges.reserve(RangeCount(peer));
            if(!VisitStorage(peer,[&](const Range& range){snapshot.ranges.push_back(range);return separate(range);}))
                throw std::invalid_argument("aliased peer model key storage");
            peers.push_back(std::move(snapshot));
        }
    }
    bool PeersCurrent() const noexcept {
        if(!peerApp)return true;
        for(unsigned slot=0;slot<kModelSlotCount;++slot)
            if(slot!=ownSlot&&peerApp->ModelSlot(slot)!=models[slot])return false;
        for(const auto& peer:peers){std::size_t index=0;
            if(!VisitStorage(*mdl::Mdl(peer.model),[&](const Range& range){return index<peer.ranges.size()&&peer.ranges[index++]==range;})||
                index!=peer.ranges.size())return false;
        }
        return true;
    }
    bool IsCurrent(const mdl::ModelRecord& model) const noexcept {
        std::size_t index = 0;
        return VisitStorage(model, [&](const Range& range) {
            return index < ranges.size() && ranges[index++] == range;
        }) && index == ranges.size() && PeersCurrent();
    }
};
MmsResult PrepareMetadataBudgeted(const std::shared_ptr<HistoryChange>&,bool,std::uint64_t&,
    std::unique_ptr<PreparedHistoryChange>&);
void VisitMetadataModels(const std::shared_ptr<HistoryChange>&,void*,ModelDeletionOwnerVisitor);
struct ModelKeyChange final : ModelKeyDeletion {
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    // Bone publication must precede display: its prepared pose uses target
    // windows but its final guard still checks the live source selectors.
    std::array<std::shared_ptr<HistoryChange>, 4> parts;
    bool signedDeletion = false;
    void VisitRetainedModels(void* context,ModelDeletionOwnerVisitor visit) const override {
        // Deletion and range scaling both install these typed families; model
        // paste, with its separate morph implementation, uses ModelPasteChange.
        if(parts[0])static_cast<const BoneKeyDeletion*>(parts[0].get())->VisitRetainedModels(context,visit);
        if(parts[1])static_cast<const MorphKeyDeletion*>(parts[1].get())->VisitRetainedModels(context,visit);
        if(parts[2])static_cast<const DisplayKeyDeletion*>(parts[2].get())->VisitRetainedModels(context,visit);
        if(parts[3])VisitMetadataModels(parts[3],context,visit);
    }
    ModelKeyChange(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)) {}
    struct Candidate final : PreparedModelKeyDeletion {
        MMDApp& app; unsigned slot; unsigned char* model;
        Layout layout;
        std::array<std::unique_ptr<PreparedHistoryChange>, 4> parts;
        PreparedBoneKeyDeletion* boneTarget = nullptr;
        PreparedMorphKeyDeletion* morphTarget = nullptr;
        PreparedDisplayKeyDeletion* displayTarget = nullptr;
        std::unique_ptr<PreparedDisplayKeyDeletion> unchangedDisplay;
        bool guardFrame = false;
        int frame = 0;
        bool published = false;
        void VisitRetainedModels(void* context,ModelDeletionOwnerVisitor visit) const override {
            // Ordinary Prepare also owns typed children, but does not populate
            // the signed-only zero-target aliases.
            if(parts[0])static_cast<const PreparedBoneKeyDeletion*>(parts[0].get())->VisitRetainedModels(context,visit);
            if(parts[1])static_cast<const PreparedMorphKeyDeletion*>(parts[1].get())->VisitRetainedModels(context,visit);
            if(parts[2])static_cast<const PreparedDisplayKeyDeletion*>(parts[2].get())->VisitRetainedModels(context,visit);
            if(unchangedDisplay)unchangedDisplay->VisitRetainedModels(context,visit);
            if(parts[3])VisitMetadataModels(metadataSource,context,visit);
        }
        std::shared_ptr<HistoryChange> metadataSource;
        MMDApp* OwnerApp() const noexcept override { return &app; }
        unsigned char* ModelObject() const noexcept override { return model; }
        const std::vector<BoneDeletionTarget>& BoneZeroTargets() const noexcept override {
            static const std::vector<BoneDeletionTarget> empty;
            return boneTarget ? boneTarget->ZeroTargets() : empty;
        }
        const std::vector<MorphDeletionTarget>& MorphZeroTargets() const noexcept override {
            static const std::vector<MorphDeletionTarget> empty;
            return morphTarget ? morphTarget->ZeroTargets() : empty;
        }
        const DisplayDeletionTarget* DisplayZeroTarget() const noexcept override {
            return displayTarget ? &displayTarget->ZeroTarget() : nullptr;
        }
        Candidate(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)), layout(*mdl::Mdl(model)) {layout.ValidatePeers(app,slot);}
        bool IsCurrent() const noexcept override {
            if (published || app.ModelSlot(slot) != model || (guardFrame && app.CurrentFrame() != frame)) return false;
            if(unchangedDisplay&&!unchangedDisplay->IsCurrent())return false;
            for (const auto& part : parts) if (part && !part->IsCurrent()) return false;
            return layout.IsCurrent(*mdl::Mdl(model));
        }
        void Publish() noexcept override {
            // Validate the entire write set once, before the first write.
            if (!IsCurrent()) std::terminate();
            for (const auto& part : parts) if (part) part->Publish();
            published = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        if (app.ModelSlot(slot) != model) return MMS_E_STALE;
        // Unknown legacy pools have assumed capacities. Reject them before
        // Layout reads nested display buffers, rather than relying on a later
        // participant's identity check to discover the replacement.
        const auto& live = *mdl::Mdl(model);
        if ((live.boneKeys && !mdl::BoneKeyPoolIdentity(live.boneKeys)) ||
            (live.morphKeys && !mdl::MorphKeyPoolIdentity(live.morphKeys)) ||
            (live.displayKeys && !mdl::KeyPoolIdentity(live.displayKeys))) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto candidate = std::make_unique<Candidate>(app, slot);
            candidate->metadataSource=parts[3];
            for (std::size_t index = 0; index < parts.size(); ++index) if (parts[index])
                if (auto result = parts[index]->Prepare(forward, candidate->parts[index])) return result;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    MmsResult PrepareSigned(bool forward,std::unique_ptr<PreparedModelKeyDeletion>& out) noexcept override {
        return PrepareSignedImpl(forward,nullptr,out);
    }
    MmsResult PrepareSignedBudgeted(bool forward,std::uint64_t& remainingBytes,
        std::unique_ptr<PreparedModelKeyDeletion>& out) noexcept override {
        return PrepareSignedImpl(forward,&remainingBytes,out);
    }
    MmsResult PrepareSignedImpl(bool forward,std::uint64_t* remainingBytes,
        std::unique_ptr<PreparedModelKeyDeletion>& out) noexcept {
        out.reset();if(!signedDeletion)return MMS_E_NOT_READY;
        if(app.ModelSlot(slot)!=model)return MMS_E_STALE;
        if(app.CurrentFrame()<frame_limits::minimum||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        const auto& live=*mdl::Mdl(model);
        if((live.boneKeys&&!mdl::BoneKeyPoolIdentity(live.boneKeys))||
            (live.morphKeys&&!mdl::MorphKeyPoolIdentity(live.morphKeys))||
            (live.displayKeys&&!mdl::KeyPoolIdentity(live.displayKeys)))return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            auto remaining=remainingBytes?*remainingBytes:UINT64_MAX;
            if(remainingBytes) {
                model_delete_budget::Bytes bytes;bytes.Add(sizeof(Candidate));
                Layout::Charge(app,slot,bytes);bytes.Take(remaining);
            }
            auto candidate=std::make_unique<Candidate>(app,slot);
            candidate->metadataSource=parts[3];
            candidate->guardFrame=true;candidate->frame=app.CurrentFrame();
            // Preparation follows data dependencies; publication retains the
            // original bone/morph/display/metadata order and one final guard.
            if(parts[2]) {
                std::unique_ptr<PreparedDisplayKeyDeletion> prepared;
                const auto display=std::static_pointer_cast<DisplayKeyDeletion>(parts[2]);
                if(auto result=remainingBytes?display->PrepareSignedBudgeted(forward,remaining,prepared):
                    display->PrepareSigned(forward,prepared))return result;
                if(!prepared)return MMS_E_NOT_READY;
                candidate->displayTarget=prepared.get();candidate->parts[2]=std::move(prepared);
            }
            if(parts[0]) {
                std::unique_ptr<PreparedBoneKeyDeletion> prepared;
                const auto bone=std::static_pointer_cast<BoneKeyDeletion>(parts[0]);
                if(!candidate->displayTarget&&live.boneOrderCount) {
                    if(auto result=remainingBytes?PrepareDisplayKeyZeroReadBudgeted(app,slot,remaining,candidate->unchangedDisplay):
                        PrepareDisplayKeyZeroRead(app,slot,candidate->unchangedDisplay))return result;
                    if(!candidate->unchangedDisplay)return MMS_E_NOT_READY;
                }
                const auto* displayZero=candidate->displayTarget?candidate->displayTarget:candidate->unchangedDisplay.get();
                const auto* selectors=displayZero?&displayZero->ZeroTarget().selectors:nullptr;
                if(auto result=remainingBytes?bone->PrepareSignedBudgeted(forward,selectors,remaining,prepared):
                    bone->PrepareSigned(forward,selectors,prepared))return result;
                if(!prepared)return MMS_E_NOT_READY;
                candidate->boneTarget=prepared.get();candidate->parts[0]=std::move(prepared);
            }
            if(parts[1]) {
                std::unique_ptr<PreparedMorphKeyDeletion> prepared;
                const auto morph=std::static_pointer_cast<MorphKeyDeletion>(parts[1]);
                if(auto result=remainingBytes?morph->PrepareSignedBudgeted(forward,remaining,prepared):
                    morph->PrepareSigned(forward,prepared))return result;
                if(!prepared)return MMS_E_NOT_READY;
                candidate->morphTarget=prepared.get();candidate->parts[1]=std::move(prepared);
            }
            if(parts[3])if(auto result=remainingBytes?PrepareMetadataBudgeted(parts[3],forward,remaining,candidate->parts[3]):
                parts[3]->Prepare(forward,candidate->parts[3]))return result;
            if(!candidate->IsCurrent())return MMS_E_STALE;
            if(remainingBytes)*remainingBytes=remaining;
            out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        for (const auto& part : parts) if (part) {
            const auto amount = part->RetainedBytes();
            bytes = amount > UINT64_MAX - bytes ? UINT64_MAX : bytes + amount;
        }
        return bytes;
    }
};
struct RangeMetadata final : HistoryChange, std::enable_shared_from_this<RangeMetadata> {
    MMDApp& app; unsigned slot; unsigned char* model;
    std::shared_ptr<RetainedSceneObject> owner;
    std::uint32_t maxBefore, maxAfter, lastBefore, lastAfter;
    int cursorBefore, cursorAfter;
    std::array<std::uint64_t, 3> identities;
    std::array<std::vector<unsigned char>, 3> before, after;
    bool restoreGrownTail = false, requireInitialMarks = false, publishedOnce = false;
    explicit RangeMetadata(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)),
        owner(RetainSceneObject(a, MMS_OBJECT_MODEL, s)), maxBefore(mdl::Mdl(model)->maxFrame), maxAfter(maxBefore),
        lastBefore(a.state.lastRegisteredFrame), lastAfter(lastBefore),
        cursorBefore(mdl::Mdl(model)->searchCursor), cursorAfter(cursorBefore) {
        const auto& m = *mdl::Mdl(model);
        identities = {mdl::BoneKeyPoolIdentity(m.boneKeys), mdl::MorphKeyPoolIdentity(m.morphKeys), mdl::KeyPoolIdentity(m.displayKeys)};
        Capture(before); after = before;
        for (auto& marks : after) std::fill(marks.begin(), marks.end(), 0);
    }
    void Capture(std::array<std::vector<unsigned char>, 3>& marks) const {
        const auto& m = *mdl::Mdl(model);
        const auto copy = [](const auto* keys, std::size_t count, auto& out) {
            out.reserve(count); for (std::size_t i = 0; i < count; ++i) out.push_back(keys[i].allocated);
        };
        copy(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), marks[0]);
        copy(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), marks[1]);
        copy(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), marks[2]);
    }
    bool Structure() const noexcept {
        if (!owner->Present() || app.ModelSlot(slot) != model) return false;
        const auto& m = *mdl::Mdl(model);
        return mdl::BoneKeyPoolIdentity(m.boneKeys) == identities[0] && mdl::BoneKeyCapacity(m.boneKeys) >= before[0].size() &&
            mdl::MorphKeyPoolIdentity(m.morphKeys) == identities[1] && mdl::MorphKeyCapacity(m.morphKeys) >= before[1].size() &&
            mdl::KeyPoolIdentity(m.displayKeys) == identities[2] && mdl::KeyPoolCapacity(m.displayKeys) >= before[2].size();
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<RangeMetadata> source; bool forward, published = false;
        std::array<std::vector<unsigned char>, 3> current;
        std::array<const void*, 3> pools;
        Candidate(std::shared_ptr<RangeMetadata> s, bool f) : source(std::move(s)), forward(f) {
            const auto& m = *mdl::Mdl(source->model); pools = {m.boneKeys, m.morphKeys, m.displayKeys}; source->Capture(current);
        }
        bool IsCurrent() const noexcept override {
            if (published || !source->Structure()) return false;
            const auto& m = *mdl::Mdl(source->model);
            if (m.maxFrame != (forward ? source->maxBefore : source->maxAfter) ||
                m.searchCursor != (forward ? source->cursorBefore : source->cursorAfter) ||
                source->app.state.lastRegisteredFrame != (forward ? source->lastBefore : source->lastAfter)) return false;
            if (pools != std::array<const void*, 3>{m.boneKeys, m.morphKeys, m.displayKeys}) return false;
            const auto same = [](const auto* keys, std::size_t count, const auto& marks) {
                if (count != marks.size()) return false;
                for (std::size_t i = 0; i < count; ++i) if (keys[i].allocated != marks[i]) return false;
                return true;
            };
            return same(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), current[0]) &&
                same(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), current[1]) &&
                same(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), current[2]);
        }
        void Publish() noexcept override {
            // Earlier family participants have already written their own key
            // marks. All guards ran before that first write; never recheck here.
            auto& m = *mdl::Mdl(source->model);
            const auto& target = forward ? source->after : source->before;
            const auto write = [](auto* keys, const auto& marks) {
                for (std::size_t i = 0; i < marks.size(); ++i) keys[i].allocated = marks[i];
            };
            write(m.boneKeys, target[0]); write(m.morphKeys, target[1]); write(m.displayKeys, target[2]);
            if(source->restoreGrownTail&&!forward){
                const auto clear=[](auto* keys,std::size_t first,std::size_t end){for(auto i=first;i<end;++i)keys[i].allocated=0;};
                clear(m.boneKeys,source->before[0].size(),source->after[0].size());
                clear(m.morphKeys,source->before[1].size(),source->after[1].size());
                clear(m.displayKeys,source->before[2].size(),source->after[2].size());
            }
            m.maxFrame = forward ? source->maxAfter : source->maxBefore;
            m.searchCursor = forward ? source->cursorAfter : source->cursorBefore;
            source->app.state.lastRegisteredFrame = forward ? source->lastAfter : source->lastBefore;
            source->publishedOnce = true;
            published = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure()) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if(requireInitialMarks&&!publishedOnce){
                if(!forward)return MMS_E_STALE;
                for(unsigned family=0;family<before.size();++family)
                    for(std::size_t i=0;i<candidate->current[family].size();++i)
                        if(candidate->current[family][i]!=(i<before[family].size()?before[family][i]:0))return MMS_E_STALE;
            }
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t total = sizeof(*this) + 4 * sizeof(void*);
        for (const auto* side : {&before, &after}) for (const auto& marks : *side) {
            const auto bytes = marks.capacity(); total = bytes > UINT64_MAX - total ? UINT64_MAX : total + bytes;
        }
        return total;
    }
};

MmsResult PrepareMetadataBudgeted(const std::shared_ptr<HistoryChange>& part,bool forward,std::uint64_t& remaining,
    std::unique_ptr<PreparedHistoryChange>& out) {
    const auto source=std::static_pointer_cast<RangeMetadata>(part);
    if(!source->Structure())return MMS_E_STALE;
    const auto& model=*mdl::Mdl(source->model);
    model_delete_budget::Bytes bytes;bytes.Add(sizeof(RangeMetadata::Candidate));
    bytes.Add(mdl::BoneKeyCapacity(model.boneKeys));bytes.Add(mdl::MorphKeyCapacity(model.morphKeys));
    bytes.Add(mdl::KeyPoolCapacity(model.displayKeys));
    auto balance=remaining;bytes.Take(balance);
    if(auto result=part->Prepare(forward,out))return result;
    remaining=balance;return MMS_OK;
}
void VisitMetadataModels(const std::shared_ptr<HistoryChange>& part,void* context,ModelDeletionOwnerVisitor visit) {
    visit(context,static_cast<const RangeMetadata*>(part.get())->owner);
}
struct ModelPasteChange final : HistoryChange, std::enable_shared_from_this<ModelPasteChange> {
    MMDApp& app; unsigned slot; unsigned char* model; int playhead;
    std::uint64_t generation=1;bool published=false;
    std::array<std::shared_ptr<HistoryChange>,4> parts;
    ModelPasteChange(MMDApp& a,unsigned s):app(a),slot(s),model(a.ModelSlot(s)),playhead(a.CurrentFrame()){}
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ModelPasteChange> source;std::uint64_t generation;int frame;bool consumed=false;
        Layout layout;std::array<std::unique_ptr<PreparedHistoryChange>,4> parts;
        explicit Candidate(std::shared_ptr<ModelPasteChange> s):source(std::move(s)),generation(source->generation),frame(source->app.CurrentFrame()),
            layout(*mdl::Mdl(source->model)){layout.ValidatePeers(source->app,source->slot);}
        bool IsCurrent()const noexcept override {
            if(consumed||source->generation!=generation||source->app.ModelSlot(source->slot)!=source->model||source->app.CurrentFrame()!=frame)return false;
            for(const auto& part:parts)if(part&&!part->IsCurrent())return false;
            return layout.IsCurrent(*mdl::Mdl(source->model));
        }
        void Publish()noexcept override {
            if(!IsCurrent())std::terminate();
            // Bone target evaluation includes the pending display selectors.
            // Other participants must not recheck shared fields after it runs.
            for(auto& part:parts)if(part)part->Publish();
            source->published=true;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override {
        out.reset();if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        if(app.ModelSlot(slot)!=model||(!published&&(!forward||app.CurrentFrame()!=playhead)))return MMS_E_STALE;
        const auto& m=*mdl::Mdl(model);
        if((m.boneKeys&&!mdl::BoneKeyPoolIdentity(m.boneKeys))||(m.morphKeys&&!mdl::MorphKeyPoolIdentity(m.morphKeys))||
            (m.displayKeys&&!mdl::KeyPoolIdentity(m.displayKeys)))return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            auto candidate=std::make_unique<Candidate>(shared_from_this());
            for(unsigned i=0;i<parts.size();++i)if(parts[i])
                if(auto result=parts[i]->Prepare(forward,candidate->parts[i]))return result;
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);
        for(const auto& part:parts)if(part){const auto amount=part->RetainedBytes();bytes=amount>UINT64_MAX-bytes?UINT64_MAX:bytes+amount;}
        return bytes;
    }
};
template<class Key> bool Marked(const Key* keys, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) if (keys[index].allocated) return true;
    return false;
}
template<class Key> std::vector<std::vector<unsigned>> KeyTracks(const Key* keys, std::size_t capacity, unsigned roots) {
    if (roots > capacity || (roots && !keys)) throw std::invalid_argument("missing model key roots");
    std::vector<std::vector<unsigned>> tracks(roots);
    std::vector<bool> visited(capacity);
    for (unsigned root = 0; root < roots; ++root) {
        unsigned current = root, previous = 0, frame = 0;
        for (;;) {
            if (current >= capacity || visited[current] || (!tracks[root].empty() && current < roots))
                throw std::invalid_argument("invalid or shared model key chain");
            const auto& key = keys[current];
            if (key.next >= capacity || key.previous >= capacity || key.frame > unsigned(frame_limits::maximum) ||
                (tracks[root].empty() ? key.frame != 0 : key.previous != previous || key.frame <= frame))
                throw std::invalid_argument("invalid model key frame or link");
            visited[current] = true; tracks[root].push_back(current);
            if (!key.next) break;
            previous = current; frame = key.frame; current = key.next;
        }
    }
    return tracks;
}
bool ConstantCurveRange(const animation_curve::Track& track, unsigned first, unsigned last) {
    std::string error;
    if (track.keys.empty() || !animation_curve::Validate(track, error)) return false;
    const auto value = animation_curve::Evaluate(track, first).value;
    if (!std::isfinite(value) || animation_curve::Evaluate(track, last).value != value) return false;
    for (std::size_t i = 1; i < track.keys.size(); ++i) {
        const auto& a = track.keys[i - 1]; const auto& b = track.keys[i];
        if (b.frame <= int(first) || a.frame >= int(last)) continue;
        // On any nonempty open subinterval a constant polynomial has all
        // value control points equal. Endpoint samples alone miss overshoot.
        if (a.value != value || b.value != value) return false;
        if (!track.discrete && b.interpolation == animation_curve::Interpolation::Bezier &&
            ((a.outWeight != 0 && a.outSlope != 0) || (b.inWeight != 0 && b.inSlope != 0))) return false;
    }
    return true;
}
}
static MmsResult PlanModelKeyDeletionImpl(MMDApp& app, unsigned slot, bool automatic,
    std::shared_ptr<ModelKeyDeletion>& out, bool fallback, const ModelKeySelection* privateSelection,bool signedPlan) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    if(signedPlan) {
        if(app.CurrentFrame()<frame_limits::minimum||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        if(app.CurrentFrame()<0)fallback=false;
    }
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if ((model.boneKeys && !mdl::BoneKeyPoolIdentity(model.boneKeys)) ||
            (model.morphKeys && !mdl::MorphKeyPoolIdentity(model.morphKeys)) ||
            (model.displayKeys && !mdl::KeyPoolIdentity(model.displayKeys))) return MMS_E_NOT_READY;
        Layout check(model);check.ValidatePeers(app,slot);
        if (privateSelection && (!ValidKeySelection(privateSelection->bones, mdl::BoneKeyCapacity(model.boneKeys)) ||
            !ValidKeySelection(privateSelection->morphs, mdl::MorphKeyCapacity(model.morphKeys)) ||
            !ValidKeySelection(privateSelection->displays, mdl::KeyPoolCapacity(model.displayKeys)))) return MMS_E_ARGUMENT;
        const bool bones = privateSelection ? !privateSelection->bones.empty() : Marked(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys));
        const bool morphs = privateSelection ? !privateSelection->morphs.empty() : Marked(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys));
        const bool displays = privateSelection ? !privateSelection->displays.empty() : Marked(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys));
        if (privateSelection) fallback = false;
        auto change = std::make_shared<ModelKeyChange>(app, slot);
        change->signedDeletion=signedPlan;
        std::shared_ptr<DisplayKeyDeletion> display;
        if (displays) {
            if(signedPlan) {
                if (auto result = PlanSignedDisplayKeyDeletion(app, slot, display, true, false,
                    privateSelection ? &privateSelection->displays : nullptr)) return result;
                change->parts[2]=display;
            }else {
                if(auto result=PlanDisplayKeyDeletion(app,slot,change->parts[2],true,false,
                    privateSelection?&privateSelection->displays:nullptr))return result;
                display=std::static_pointer_cast<DisplayKeyDeletion>(change->parts[2]);
            }
        }
        if (bones || (model.boneCount && display) || (fallback && !morphs && !displays && model.boneCount)) {
            if(signedPlan) {
                std::shared_ptr<BoneKeyDeletion> bone;
                if (auto result = PlanSignedBoneKeyDeletion(app, slot, automatic, bone, fallback, true, display,
                    privateSelection ? &privateSelection->bones : nullptr)) return result;
                change->parts[0]=std::move(bone);
            }else if(auto result=PlanBoneKeyDeletion(app,slot,automatic,change->parts[0],fallback,true,display,
                privateSelection?&privateSelection->bones:nullptr))return result;
        }
        if (morphs) {
            if(signedPlan) {
                std::shared_ptr<MorphKeyDeletion> morph;
                if (auto result = PlanSignedMorphKeyDeletion(app, slot, morph, true,
                    privateSelection ? &privateSelection->morphs : nullptr)) return result;
                change->parts[1]=std::move(morph);
            }else if(auto result=PlanMorphKeyDeletion(app,slot,change->parts[1],true,
                privateSelection?&privateSelection->morphs:nullptr))return result;
        }
        if (privateSelection && (bones || morphs || displays)) {
            auto metadata = std::make_shared<RangeMetadata>(app, slot);
            metadata->requireInitialMarks = true;
            change->parts[3] = std::move(metadata);
        }
        for (const auto& part : change->parts) if (part) { out = std::move(change); break; }
        return MMS_OK;
    });
}
MmsResult PlanModelKeyDeletion(MMDApp& app,unsigned slot,bool automatic,
    std::shared_ptr<HistoryChange>& out,bool fallback,const ModelKeySelection* selection)noexcept {
    out.reset();std::shared_ptr<ModelKeyDeletion> change;
    const auto result=PlanModelKeyDeletionImpl(app,slot,automatic,change,fallback,selection,false);
    if(!result)out=std::move(change);return result;
}
MmsResult PlanSignedModelKeyDeletion(MMDApp& app,unsigned slot,bool automatic,
    std::shared_ptr<ModelKeyDeletion>& out,bool fallback,const ModelKeySelection* selection)noexcept {
    return PlanModelKeyDeletionImpl(app,slot,automatic,out,fallback,selection,true);
}
MmsResult PlanUnusedModelKeyDeletion(MMDApp& app, unsigned slot, bool automatic,
    std::shared_ptr<HistoryChange>& out, std::uint32_t& deleted) noexcept {
    out.reset(); deleted = 0;
    if (slot >= kModelSlotCount || app.SelectedModelSlot() != slot || app.CameraMode() || !app.ModelSlot(slot) ||
        app.PlaybackActive() || app.FrameStepPlayback() || app.state.recordPlaybackStartPending || app.SceneMutationInProgress())
        return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto* raw = app.ModelSlot(slot); const auto& m = *mdl::Mdl(raw);
        if ((m.boneKeys && !mdl::BoneKeyPoolIdentity(m.boneKeys)) ||
            (m.morphKeys && !mdl::MorphKeyPoolIdentity(m.morphKeys)) ||
            (m.displayKeys && !mdl::KeyPoolIdentity(m.displayKeys)) || (m.boneCount && !m.boneTable)) return MMS_E_NOT_READY;
        Layout layout(m); layout.ValidatePeers(app, slot);
        const auto bones = KeyTracks(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), m.boneCount);
        const auto morphs = KeyTracks(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), m.morphCount);
        const auto displays = KeyTracks(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), 1);
        ModelKeySelection selection;
        for (unsigned bone = 0; bone < m.boneCount; ++bone) {
            if (m.boneTable[bone].type == mdl::BoneType::InertTip || m.boneTable[bone].type == mdl::BoneType::Effector) continue;
            bone_curve::Tracks authored; const bool hasAuthoring = bone_curve::ReadDetached(raw, int(bone), authored);
            const auto& chain = bones[bone];
            for (std::size_t i = 1; i + 1 < chain.size(); ++i) {
                const auto& p = m.boneKeys[chain[i - 1]]; const auto& k = m.boneKeys[chain[i]]; const auto& n = m.boneKeys[chain[i + 1]];
                bool same = k.physicsDisabled == p.physicsDisabled && k.physicsDisabled == n.physicsDisabled;
                for (unsigned lane = 0; lane < 3; ++lane) same = same && k.position[lane] == p.position[lane] && k.position[lane] == n.position[lane];
                for (unsigned lane = 0; lane < 4; ++lane) same = same && k.rotation[lane] == p.rotation[lane] && k.rotation[lane] == n.rotation[lane];
                const auto scale = bone_scale::Key(raw, int(chain[i]));
                same = same && scale.v == bone_scale::Key(raw, int(chain[i - 1])).v && scale.v == bone_scale::Key(raw, int(chain[i + 1])).v;
                if (same && hasAuthoring) for (const auto& track : authored) same = same && ConstantCurveRange(track, p.frame, n.frame);
                if (same) selection.bones.push_back(chain[i]);
            }
        }
        std::vector<int> morphIds;
        for (unsigned morph = 0; morph < m.morphCount; ++morph) if (morphs[morph].size() > 2) morphIds.push_back(int(morph));
        std::vector<animation_curve::Track> morphTracks; std::wstring error;
        if (!morphIds.empty() && !morph_curve::ReadManyDetached(app, morphIds, morphTracks, error)) return MMS_E_ARGUMENT;
        for (std::size_t index = 0; index < morphIds.size(); ++index) {
            const auto& chain = morphs[unsigned(morphIds[index])];
            for (std::size_t i = 1; i + 1 < chain.size(); ++i) {
                const auto& p = m.morphKeys[chain[i - 1]]; const auto& k = m.morphKeys[chain[i]]; const auto& n = m.morphKeys[chain[i + 1]];
                if (k.value == p.value && k.value == n.value && ConstantCurveRange(morphTracks[index], p.frame, n.frame))
                    selection.morphs.push_back(chain[i]);
            }
        }
        const auto& chain = displays[0];
        for (std::size_t i = 1; i + 1 < chain.size(); ++i) {
            const auto& p = m.displayKeys[chain[i - 1]]; const auto& k = m.displayKeys[chain[i]]; const auto& n = m.displayKeys[chain[i + 1]];
            if ((m.ikChainCount && (!p.ikStates || !k.ikStates || !n.ikStates)) ||
                (m.boneOrderCount && (!p.selectorStates || !k.selectorStates || !n.selectorStates))) return MMS_E_ARGUMENT;
            bool same = k.visible == p.visible && k.visible == n.visible;
            for (unsigned j = 0; j < m.ikChainCount; ++j) same = same && k.ikStates[j] == p.ikStates[j] && k.ikStates[j] == n.ikStates[j];
            for (unsigned j = 0; j < m.boneOrderCount; ++j) same = same &&
                k.selectorStates[j].modelIndex == p.selectorStates[j].modelIndex && k.selectorStates[j].boneIndex == p.selectorStates[j].boneIndex &&
                k.selectorStates[j].modelIndex == n.selectorStates[j].modelIndex && k.selectorStates[j].boneIndex == n.selectorStates[j].boneIndex;
            if (same) selection.displays.push_back(chain[i]);
        }
        for (auto* selected : {&selection.bones, &selection.morphs, &selection.displays}) std::sort(selected->begin(), selected->end());
        const auto count = std::uint64_t(selection.bones.size()) + selection.morphs.size() + selection.displays.size();
        if (count > UINT32_MAX) return MMS_E_ARGUMENT;
        if (!count) return MMS_OK;
        if (auto result = PlanModelKeyDeletion(app, slot, automatic, out, false, &selection)) return result;
        deleted = out ? std::uint32_t(count) : 0;
        return MMS_OK;
    });
}
MmsResult ValidateModelPasteSource(const ModelPasteSource& source,unsigned families) noexcept {
    if((families&~ModelPasteAll)||source.frameOffset>unsigned(frame_limits::maximum))return MMS_E_ARGUMENT;
    if(((families&ModelPasteBones)&&source.bones.size()>mdl::kBoneKeyCapacityLimit)||
        ((families&ModelPasteMorphs)&&source.morphs.size()>mdl::kMorphKeyCapacityLimit)||
        ((families&ModelPasteDisplays)&&source.displays.size()>mdl::KeyPoolLimit<mdl::DisplayKey>()))return MMS_E_ARGUMENT;
    constexpr std::size_t maximumBoneBlock=16u*1024*1024+12;
    if((families&ModelPasteBones)&&(source.boneCurveBlock.size()>maximumBoneBlock||source.boneCurveTargets.size()>UINT32_MAX))return MMS_E_ARGUMENT;
    if((families&ModelPasteMorphs)&&(source.morphCurveBlock.size()>morph_curve::kMaximumPayloadBytes+16||source.morphCurveTargets.size()>UINT32_MAX))return MMS_E_ARGUMENT;
    if(!source.curveTargetsResolved&&((families&ModelPasteBones)&&!source.boneCurveTargets.empty()||
        (families&ModelPasteMorphs)&&!source.morphCurveTargets.empty()))return MMS_E_ARGUMENT;
    if(families&ModelPasteBones)for(int target:source.boneCurveTargets)if(target< -1)return MMS_E_ARGUMENT;
    if(families&ModelPasteMorphs)for(int target:source.morphCurveTargets)if(target< -1)return MMS_E_ARGUMENT;
    unsigned frame;
    if(families&ModelPasteBones)for(const auto& record:source.bones)
        if(!frame_limits::Add(int(source.frameOffset),record.key.frame,frame)||(record.targetResolved&&record.targetIndex< -1))return MMS_E_ARGUMENT;
    if(families&ModelPasteMorphs)for(const auto& record:source.morphs)
        if(!frame_limits::Add(int(source.frameOffset),record.key.frame,frame)||(record.targetResolved&&record.targetIndex< -1))return MMS_E_ARGUMENT;
    if(families&ModelPasteDisplays)for(const auto& record:source.displays) {
        if(!frame_limits::Add(int(source.frameOffset),record.frame,frame)||record.ik.size()>mdl::kBoneKeyCapacityLimit||
            record.selectors.size()>mdl::kBoneKeyCapacityLimit||record.ikTargets.size()>mdl::kBoneKeyCapacityLimit||
            (record.ikTargetsResolved?record.ikTargets.size()!=record.ik.size():!record.ikTargets.empty()))return MMS_E_ARGUMENT;
        if(record.ikTargetsResolved)for(int target:record.ikTargets)if(target< -1)return MMS_E_ARGUMENT;
    }
    return MMS_OK;
}
MmsResult CaptureModelPastePayload(MMDApp& app,ModelPasteSource& out,unsigned families) noexcept {
    out={};
    if(families&~ModelPasteAll)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult {
        ModelPasteSource source;const auto counts=app.ClipboardCounts();
        if(families&ModelPasteBones) {
            if(counts.bones>mdl::kBoneKeyCapacityLimit||(counts.bones&&!app.BoneClipboard()))return MMS_E_ARGUMENT;
            source.bones.reserve(counts.bones);
            for(unsigned i=0;i<counts.bones;++i)source.bones.push_back({app.BoneClipboard()[i],
                bone_scale::Clipboard(&app,int(i),false),bone_scale::ClipboardCurve(&app,int(i),false)});
        }
        if(families&ModelPasteMorphs) {
            if(counts.morphs>mdl::kMorphKeyCapacityLimit||(counts.morphs&&!app.MorphClipboard()))return MMS_E_ARGUMENT;
            source.morphs.reserve(counts.morphs);
            for(unsigned i=0;i<counts.morphs;++i)source.morphs.push_back({app.MorphClipboard()[i]});
        }
        if(families&ModelPasteDisplays) {
            if(counts.displays>mdl::KeyPoolLimit<mdl::DisplayKey>()||(counts.displays&&!app.DisplayClipboard()))return MMS_E_ARGUMENT;
            source.displays.reserve(counts.displays);
            for(unsigned i=0;i<counts.displays;++i) {
                const auto& record=app.DisplayClipboard()[i];
                if(record.ikCount<0||record.selectorCount<0||unsigned(record.ikCount)>mdl::kBoneKeyCapacityLimit||
                    unsigned(record.selectorCount)>mdl::kBoneKeyCapacityLimit||(record.ikCount&&!record.ikStates)||
                    (record.selectorCount&&!record.selectorStates))return MMS_E_ARGUMENT;
                ModelDisplayPasteRecord owned;owned.frame=record.frame;owned.visible=record.visible;
                if(record.ikCount)owned.ik.assign(record.ikStates,record.ikStates+record.ikCount);
                if(record.selectorCount)owned.selectors.assign(record.selectorStates,record.selectorStates+record.selectorCount);
                source.displays.push_back(std::move(owned));
            }
        }
        if(auto result=ValidateModelPasteSource(source,families))return result;
        out=std::move(source);return MMS_OK;
    });
}
MmsResult CaptureModelPasteSource(MMDApp& app,ModelPasteSource& out,unsigned families) noexcept {
    out={};
    const int frame=app.CurrentFrame();
    if((families&~ModelPasteAll)||frame<0||frame>frame_limits::maximum)return MMS_E_ARGUMENT;
    ModelPasteSource source;
    if(auto result=CaptureModelPastePayload(app,source,families))return result;
    source.frameOffset=unsigned(frame);
    if(auto result=ValidateModelPasteSource(source,families))return result;
    out=std::move(source);return MMS_OK;
}
MmsResult PlanModelKeyPaste(MMDApp& app,unsigned slot,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    ModelPasteSource source;
    if(auto result=CaptureModelPasteSource(app,source))return result;
    return PlanModelKeyPaste(app,slot,source,out);
}
MmsResult PlanModelKeyPaste(MMDApp& app,unsigned slot,const ModelPasteSource& source,std::shared_ptr<HistoryChange>& out,ModelPasteReport* report) noexcept {
    out.reset();if(report)*report={};
    if(slot>=kModelSlotCount||!app.ModelSlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
        app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    if(auto result=ValidateModelPasteSource(source))return result;
    return Boundary([&]()->MmsResult {
        if(source.bones.empty()&&source.morphs.empty()&&source.displays.empty()&&source.boneCurveBlock.empty()&&source.morphCurveBlock.empty())return MMS_OK;
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if((model.boneKeys&&!mdl::BoneKeyPoolIdentity(model.boneKeys))||
            (model.morphKeys&&!mdl::MorphKeyPoolIdentity(model.morphKeys))||
            (model.displayKeys&&!mdl::KeyPoolIdentity(model.displayKeys)))return MMS_E_NOT_READY;
        Layout check(model);check.ValidatePeers(app,slot);
        std::array<ModelPasteFamilyResult,3> results;
        std::shared_ptr<DisplayKeyDeletion> display;
        if(!source.displays.empty()){
            std::shared_ptr<HistoryChange> part;
            if(auto error=PlanDisplayKeyPaste(app,slot,source,part,&results[2]))return error;
            display=std::static_pointer_cast<DisplayKeyDeletion>(results[2].change);
        }
        if(!model.boneCount)for(const auto& record:source.bones)
            if(record.targetResolved&&record.targetIndex>=0)return MMS_E_ARGUMENT;
        if(model.boneCount&&(!source.bones.empty()||!source.boneCurveBlock.empty()||display)){
            std::shared_ptr<HistoryChange> part;
            if(auto error=PlanBoneKeyPaste(app,slot,false,source,part,&results[0],display))return error;
        }
        if(!source.morphs.empty()||!source.morphCurveBlock.empty()){
            std::shared_ptr<HistoryChange> part;
            if(auto error=PlanMorphKeyPaste(app,slot,source,part,&results[1]))return error;
        }
        const bool boneCurvesSkipped=results[0].curveBlockSkipped;
        if(!results[0].dataChanged&&!results[1].dataChanged&&!results[2].dataChanged){
            if(report)report->boneCurvesSkipped=boneCurvesSkipped;return MMS_OK;
        }
        auto change=std::make_shared<ModelPasteChange>(app,slot);
        auto metadata=std::make_shared<RangeMetadata>(app,slot);
        metadata->restoreGrownTail=true;metadata->requireInitialMarks=true;
        unsigned cursor=1;const auto capacity=mdl::KeyPoolCapacity(model.displayKeys);
        while(cursor<capacity&&model.displayKeys[cursor].frame)++cursor;
        metadata->cursorAfter=!source.displays.empty()?results[2].searchCursor:int(cursor);
        if(!source.resetDisplayCursor){
            cursor=model.morphCount;const auto morphCapacity=mdl::MorphKeyCapacity(model.morphKeys);
            while(cursor<morphCapacity&&model.morphKeys[cursor].frame)++cursor;
            metadata->cursorAfter=results[1].hasKeys?results[1].searchCursor:int(cursor);
        }
        std::uint32_t registrationMaximum=metadata->maxBefore;
        for(unsigned family=0;family<results.size();++family){
            auto& result=results[family];change->parts[family]=std::move(result.change);
            if(result.hasKeys)metadata->maxAfter=(std::max)(metadata->maxAfter,result.maxFrame);
            if(result.hasKeys)registrationMaximum=(std::max)(registrationMaximum,
                result.hasRegistrationExtent?result.registrationMaxFrame:result.maxFrame);
            if(result.overridesSearchCursor)metadata->cursorAfter=result.searchCursor;
            const auto limit=family==0?std::size_t(mdl::kBoneKeyCapacityLimit):family==1?
                std::size_t(mdl::kMorphKeyCapacityLimit):mdl::KeyPoolLimit<mdl::DisplayKey>();
            for(auto index:result.markedSlots){
                if(index>=limit)return MMS_E_ARGUMENT;
                if(index>=metadata->after[family].size())metadata->after[family].resize(std::size_t(index)+1,0);
                metadata->after[family][index]=1;
            }
        }
        if(source.advanceLastRegisteredFrame)metadata->lastAfter=(std::max)(metadata->lastBefore,registrationMaximum);
        change->parts[3]=std::move(metadata);out=std::move(change);
        if(report)report->boneCurvesSkipped=boneCurvesSkipped;return MMS_OK;
    });
}
MmsResult PlanModelRangeScale(MMDApp& app, unsigned slot, const ModelRangeScaleOptions& options,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    if (options.start >= options.end || options.end > unsigned(frame_limits::maximum) ||
        !std::isfinite(options.scale) || options.scale <= 0) return MMS_E_ARGUMENT;
    if (options.scale == 1 || (!options.bones && !options.morphs && !options.display)) return MMS_OK;
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if ((model.boneKeys && !mdl::BoneKeyPoolIdentity(model.boneKeys)) ||
            (model.morphKeys && !mdl::MorphKeyPoolIdentity(model.morphKeys)) ||
            (model.displayKeys && !mdl::KeyPoolIdentity(model.displayKeys))) return MMS_E_NOT_READY;
        Layout check(model);check.ValidatePeers(app,slot);
        const BoneRangeScaleOptions family{options.start, options.end, options.scale, options.automaticInterpolation};
        std::array<RangeScaleResult, 3> results;
        std::shared_ptr<DisplayKeyDeletion> display;
        if (options.display) {
            if (auto result = PlanDisplayRangeScale(app, slot, family, results[2])) return result;
            display = std::static_pointer_cast<DisplayKeyDeletion>(results[2].change);
        }
        if (options.bones && model.boneCount) {
            std::shared_ptr<HistoryChange> bones;
            if (auto result = PlanBoneRangeScale(app, slot, family, bones, &results[0], display)) return result;
        } else if (display && model.boneCount) {
            if (auto result = PlanBoneDisplayEvaluation(app, slot, display, results[0].change)) return result;
        }
        if (options.morphs) if (auto result = PlanMorphRangeScale(app, slot, family, results[1])) return result;
        if (!results[0].moved && !results[1].moved && !results[2].moved) return MMS_OK;
        auto change = std::make_shared<ModelKeyChange>(app, slot);
        auto metadata = std::make_shared<RangeMetadata>(app, slot);
        int markedFamily = -1;
        for (unsigned i = 0; i < results.size(); ++i) {
            change->parts[i] = std::move(results[i].change);
            if (!results[i].hasKeys) continue;
            markedFamily = int(i); metadata->maxAfter = (std::max)(metadata->maxAfter, results[i].maxFrame);
            metadata->cursorAfter = results[i].searchCursor;
        }
        if (markedFamily >= 0) for (auto key : results[markedFamily].markedSlots) {
            if (key >= metadata->after[markedFamily].size()) return MMS_E_NOT_READY;
            metadata->after[markedFamily][key] = 1;
        }
        metadata->lastAfter = (std::max)(metadata->lastBefore, metadata->maxAfter);
        change->parts[3] = std::move(metadata); out = std::move(change); return MMS_OK;
    });
}
}
#endif

#include "native_model_copy.inc"
