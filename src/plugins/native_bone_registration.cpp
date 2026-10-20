#include "native_bone_registration.hpp"
#include "native_key_selection.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_evaluation.hpp"
#include "native_center_offset.hpp"
#include "native_model_multiply.hpp"
#include "native_display_keys.hpp"
#include "native_range_scale.hpp"
#include "native_model_paste.hpp"
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_pose_safety.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace mikumikustudio::plugins {
namespace {
bool ValidateTailCursorStorage(MMDApp&,unsigned) noexcept;
struct KeyPatch { unsigned slot; mdl::BoneKey before{}, after{}; bool data = false; };
struct FlagPatch { unsigned slot; unsigned char before; };
struct TailCursor { unsigned bone; std::uint32_t before; unsigned char active; };
struct PoseValue { float position[3], rotation[4]; unsigned char dirty; };
struct PosePatch { unsigned bone; PoseValue before{}, after{}; bone_scale::Matrix sourceScale; unsigned fields = 0; unsigned char physics; };
struct BoneRule {
    unsigned bone;
    mdl::BoneType type;
    std::uint16_t flags;
    unsigned char hasRigidBody;
    int tailBone;
    float position[3], axis[3];
};
constexpr unsigned Rotation = 8, Dirty = 16;
PoseValue ReadPose(const mdl::ModelRecord& model, unsigned bone) {
    PoseValue value{};
    std::memcpy(value.position, model.boneTable[bone].trans, sizeof(value.position));
    std::memcpy(value.rotation, model.boneTable[bone].rotQuat, sizeof(value.rotation));
    value.dirty = model.bonePhysicsState[bone]; return value;
}
bool SamePose(const PoseValue& a, const PoseValue& b, unsigned fields) noexcept {
    for (unsigned axis = 0; axis < 3; ++axis)
        if ((fields & (1u << axis)) && std::memcmp(&a.position[axis], &b.position[axis], sizeof(float))) return false;
    return (!(fields & Rotation) || !std::memcmp(a.rotation, b.rotation, sizeof(a.rotation))) &&
        (!(fields & Dirty) || a.dirty == b.dirty);
}
bool SameKeyData(mdl::BoneKey a, mdl::BoneKey b) noexcept {
    a.allocated = b.allocated = 0; return !std::memcmp(&a, &b, sizeof(a));
}
bool SameCurveKeyData(mdl::BoneKey a, mdl::BoneKey b) noexcept {
    a.allocated = b.allocated = 0;
    a.reserved[0] = a.reserved[1] = b.reserved[0] = b.reserved[1] = 0;
    return !std::memcmp(&a, &b, sizeof(a));
}
void AddBytes(std::uint64_t& total, std::uint64_t amount) noexcept {
    total = amount > UINT64_MAX - total ? UINT64_MAX : total + amount;
}
struct RegistrationChange final : HistoryChange, std::enable_shared_from_this<RegistrationChange> {
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    mdl::BoneRecord* bones;
    unsigned count;
    std::uint64_t poolIdentity;
    mdl::MorphKey* morphPool;
    mdl::DisplayKey* displayPool;
    std::uint64_t morphIdentity, displayIdentity;
    std::size_t morphCapacity, displayCapacity;
    std::vector<KeyPatch> keys;
    std::vector<unsigned> keyDependencies;
    std::vector<mdl::BoneKey> planningDependencies;
    std::vector<unsigned> evaluationBones;
    std::vector<TailCursor> tailCursors;
    std::uint32_t* tailCursorTable=nullptr;
    unsigned char* tailActiveTable=nullptr;
    int tailInitialFrame=0;
    std::shared_ptr<DisplayKeyDeletion> display;
    std::vector<int> displaySelectors;
    std::vector<BoneRule> rules;
    std::vector<std::array<char, 20>> pasteNames;
    std::vector<PosePatch> poses;
    std::vector<FlagPatch> morphFlags, displayFlags;
    std::unique_ptr<bone_curve::AuthoringState> curveBefore, curveAfter;
    bone_scale::AuthoringState scaleBefore, scaleAfter;
    std::uint32_t maxBefore, maxAfter, lastBefore, lastAfter;
    int searchBefore, searchAfter;
    bool scaleChanged = false, curvesChanged = false;
    bool observeAuthoring = false;
    bool publishMetadata = true;
    bool preserveKeyMarks = false;
    bool published = false;
    unsigned char rulePhysicsMode = 0;
    RegistrationChange(MMDApp& a, unsigned s)
        : app(a), slot(s), model(a.ModelSlot(s)), lifetime(RetainSceneObject(a, MMS_OBJECT_MODEL, s)),
          bones(mdl::Mdl(model)->boneTable), count(mdl::Mdl(model)->boneCount),
          poolIdentity(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),
          morphPool(mdl::Mdl(model)->morphKeys), displayPool(mdl::Mdl(model)->displayKeys),
          morphIdentity(mdl::MorphKeyPoolIdentity(morphPool)), displayIdentity(mdl::KeyPoolIdentity(displayPool)),
          morphCapacity(mdl::MorphKeyCapacity(morphPool)), displayCapacity(mdl::KeyPoolCapacity(displayPool)),
          maxBefore(mdl::Mdl(model)->maxFrame), maxAfter(maxBefore),
          lastBefore(a.state.lastRegisteredFrame), lastAfter(lastBefore),
          searchBefore(mdl::Mdl(model)->searchCursor), searchAfter(searchBefore) {}
    bool Present() const noexcept {
        return lifetime->Present() && app.ModelSlot(slot) == model && SceneObjectPresent(model, lifetime->Id());
    }
    bool Structure() const noexcept {
        if (!Present()) return false;
        const auto& m = *mdl::Mdl(model);
        if (m.boneTable != bones || m.boneCount != count || !m.bonePhysicsState ||
            mdl::BoneKeyPoolIdentity(m.boneKeys) != poolIdentity) return false;
        if(!tailCursors.empty()&&(m.boneKeyCursors!=tailCursorTable||m.boneTrackActive!=tailActiveTable||!ValidateTailCursorStorage(app,slot)))return false;
        for (std::size_t index = 0; index < displaySelectors.size(); ++index)
            if (bones[index].slotIndex != displaySelectors[index]) return false;
        if (!rules.empty() && m.physicsMode != rulePhysicsMode) return false;
        for (std::size_t index = 0; index < pasteNames.size(); ++index)
            if (std::memcmp(bones[index].name, pasteNames[index].data(), pasteNames[index].size())) return false;
        for (const auto& rule : rules) {
            const auto& bone = bones[rule.bone];
            if (bone.type != rule.type || bone.flags != rule.flags || bone.hasRigidBody != rule.hasRigidBody ||
                bone.tailBone != rule.tailBone || std::memcmp(bone.position, rule.position, sizeof(rule.position)) ||
                std::memcmp(bone.axis, rule.axis, sizeof(rule.axis))) return false;
        }
        if (!morphFlags.empty() && (morphIdentity ? mdl::MorphKeyPoolIdentity(m.morphKeys) != morphIdentity ||
            mdl::MorphKeyCapacity(m.morphKeys) < morphCapacity :
            m.morphKeys != morphPool || mdl::MorphKeyCapacity(m.morphKeys) != morphCapacity)) return false;
        if (!displayFlags.empty() && (displayIdentity ? mdl::KeyPoolIdentity(m.displayKeys) != displayIdentity ||
            mdl::KeyPoolCapacity(m.displayKeys) < displayCapacity :
            m.displayKeys != displayPool || mdl::KeyPoolCapacity(m.displayKeys) != displayCapacity)) return false;
        for (const auto& p : poses) if (bones[p.bone].physicsDisabled != p.physics) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<RegistrationChange> source;
        bool forward;
        std::unique_ptr<mdl::BoneKeyPoolGrowth> growth;
        std::unique_ptr<bone_scale::AuthoringRestore> scale;
        std::unique_ptr<bone_curve::AuthoringRestore> curves;
        std::unique_ptr<PreparedHistoryChange> evaluation;
        std::vector<mdl::BoneKey> currentKeys;
        std::vector<mdl::BoneKey> dependencyKeys;
        std::vector<PoseValue> currentPoses;
        std::vector<bone_scale::Matrix> currentScales;
        std::vector<unsigned char> currentMorph, currentDisplay;
        std::vector<std::uint32_t> currentTailCursors,targetTailCursors;
        std::vector<unsigned char> currentTailActive,targetTailActive;
        int tailFrame=0;
        unsigned char* dirty;
        mdl::MorphKey* morphs;
        mdl::DisplayKey* displays;
        std::size_t morphCapacity, displayCapacity;
        std::uint32_t currentMax, currentLast;
        int currentSearch;
        Candidate(std::shared_ptr<RegistrationChange> s, bool f) : source(std::move(s)), forward(f) {}
        bool IsCurrent() const noexcept override {
            if (!source->Structure() || !growth || !growth->IsCurrent()) return false;
            const auto& m = *mdl::Mdl(source->model);
            if (m.bonePhysicsState != dirty || m.morphKeys != morphs || m.displayKeys != displays ||
                mdl::MorphKeyCapacity(morphs) != morphCapacity || mdl::KeyPoolCapacity(displays) != displayCapacity ||
                (source->publishMetadata && (m.maxFrame != currentMax || m.searchCursor != currentSearch ||
                source->app.state.lastRegisteredFrame != currentLast))) return false;
            const auto capacity = mdl::BoneKeyCapacity(m.boneKeys);
            if(!source->tailCursors.empty()&&source->app.CurrentFrame()!=tailFrame)return false;
            for(std::size_t n=0;n<source->tailCursors.size();++n){const auto bone=source->tailCursors[n].bone;
                if((source->tailCursorTable&&source->tailCursorTable[bone]!=currentTailCursors[n])||
                    (source->tailActiveTable&&source->tailActiveTable[bone]!=currentTailActive[n]))return false;}
            for (std::size_t n = 0; n < source->keys.size(); ++n) {
                const auto& patch = source->keys[n];
                const auto live = patch.slot < capacity ? m.boneKeys[patch.slot] : mdl::BoneKey{};
                if (patch.data ? std::memcmp(&live, &currentKeys[n], sizeof(live)) != 0 :
                    live.allocated != currentKeys[n].allocated) return false;
            }
            for (std::size_t n = 0; n < source->keyDependencies.size(); ++n) {
                const auto index = source->keyDependencies[n];
                if (index >= capacity || std::memcmp(&m.boneKeys[index], &dependencyKeys[n], sizeof(mdl::BoneKey))) return false;
            }
            for (std::size_t n = 0; n < source->poses.size(); ++n) {
                const auto bone = source->poses[n].bone;
                // The key payload was derived from the pose, including fields
                // registration itself does not repair. Freeze those inputs for
                // this short candidate as well as the owned output fields.
                if (!SamePose(ReadPose(m, bone), currentPoses[n], 7 | Rotation | Dirty) ||
                    bone_scale::Pose(source->model, int(bone)) != currentScales[n]) return false;
            }
            for (std::size_t n = 0; n < source->morphFlags.size(); ++n)
                if (morphs[source->morphFlags[n].slot].allocated != currentMorph[n]) return false;
            for (std::size_t n = 0; n < source->displayFlags.size(); ++n)
                if (displays[source->displayFlags[n].slot].allocated != currentDisplay[n]) return false;
            return (!scale || scale->IsCurrent()) && (!curves || curves->IsCurrent()) &&
                (!evaluation || evaluation->IsCurrent());
        }
        void Publish() noexcept override {
            // SharedHistory validates the complete aggregate before this call.
            // None of these independent side-table exchanges reads native keys.
            if (!growth->Publish()) std::terminate();
            auto& m = *mdl::Mdl(source->model);
            for (const auto& patch : source->keys) {
                const auto& target = forward ? patch.after : patch.before;
                if (patch.data) {
                    const auto mark = m.boneKeys[patch.slot].allocated;
                    std::memcpy(&m.boneKeys[patch.slot], &target, sizeof(target));
                    if (source->preserveKeyMarks) m.boneKeys[patch.slot].allocated = mark;
                }
                else m.boneKeys[patch.slot].allocated = target.allocated;
            }
            for (const auto& patch : source->poses) {
                const auto& target = forward ? patch.after : patch.before;
                auto& bone = m.boneTable[patch.bone];
                for (unsigned axis = 0; axis < 3; ++axis)
                    if (patch.fields & (1u << axis)) bone.trans[axis] = target.position[axis];
                if (patch.fields & Rotation) std::memcpy(bone.rotQuat, target.rotation, sizeof(target.rotation));
                if (patch.fields & Dirty) m.bonePhysicsState[patch.bone] = target.dirty;
            }
            for (const auto& patch : source->morphFlags) m.morphKeys[patch.slot].allocated = forward ? 0 : patch.before;
            for (const auto& patch : source->displayFlags) m.displayKeys[patch.slot].allocated = forward ? 0 : patch.before;
            if (source->publishMetadata) {
                m.maxFrame = forward ? source->maxAfter : source->maxBefore;
                m.searchCursor = forward ? source->searchAfter : source->searchBefore;
                source->app.state.lastRegisteredFrame = forward ? source->lastAfter : source->lastBefore;
            }
            if (scale && !scale->Publish()) std::terminate();
            if (curves && !curves->Publish()) std::terminate();
            for(std::size_t n=0;n<source->tailCursors.size();++n){const auto bone=source->tailCursors[n].bone;
                if(source->tailCursorTable)source->tailCursorTable[bone]=targetTailCursors[n];
                if(source->tailActiveTable)source->tailActiveTable[bone]=targetTailActive[n];}
            if (evaluation) evaluation->Publish();
            if (!growth->Accept()) std::terminate();
            source->published=true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure()) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto& m = *mdl::Mdl(model);
            if(!published&&!forward)return MMS_E_STALE;
            if (publishMetadata && (m.maxFrame != (forward ? maxBefore : maxAfter) ||
                m.searchCursor != (forward ? searchBefore : searchAfter) ||
                app.state.lastRegisteredFrame != (forward ? lastBefore : lastAfter))) return MMS_E_STALE;
            const auto capacity = mdl::BoneKeyCapacity(m.boneKeys);
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            candidate->dirty = m.bonePhysicsState; candidate->morphs = m.morphKeys; candidate->displays = m.displayKeys;
            candidate->morphCapacity = mdl::MorphKeyCapacity(m.morphKeys); candidate->displayCapacity = mdl::KeyPoolCapacity(m.displayKeys);
            candidate->currentMax = m.maxFrame; candidate->currentSearch = m.searchCursor;
            candidate->currentLast = app.state.lastRegisteredFrame;
            std::uint64_t needed = count;
            for (const auto& patch : keys) {
                const auto live = patch.slot < capacity ? m.boneKeys[patch.slot] : mdl::BoneKey{};
                if (patch.data && !SameKeyData(live, forward ? patch.before : patch.after)) return MMS_E_STALE;
                candidate->currentKeys.push_back(live);
                needed = (std::max)(needed, std::uint64_t(patch.slot) + 1);
            }
            for (std::size_t n=0;n<keyDependencies.size();++n) {const auto index=keyDependencies[n];
                if (index >= capacity) return MMS_E_STALE;
                if(!published&&(n>=planningDependencies.size()||!SameCurveKeyData(m.boneKeys[index],planningDependencies[n])))return MMS_E_STALE;
                candidate->dependencyKeys.push_back(m.boneKeys[index]);
            }
            for (const auto& patch : poses) {
                if(!published&&(!SamePose(ReadPose(m,patch.bone),patch.before,7|Rotation|Dirty)||
                    bone_scale::Pose(model,int(patch.bone))!=patch.sourceScale))return MMS_E_STALE;
                candidate->currentPoses.push_back(ReadPose(m, patch.bone));
                candidate->currentScales.push_back(bone_scale::Pose(model, int(patch.bone)));
            }
            for (const auto& patch : morphFlags) candidate->currentMorph.push_back(m.morphKeys[patch.slot].allocated);
            for (const auto& patch : displayFlags) candidate->currentDisplay.push_back(m.displayKeys[patch.slot].allocated);
            if ((scaleChanged || observeAuthoring) && !bone_scale::AuthoringState::Capture(model).Equals(forward ? scaleBefore : scaleAfter)) return MMS_E_STALE;
            if ((curvesChanged || observeAuthoring) && !bone_curve::AuthoringState::Capture(model)->Equals(forward ? *curveBefore : *curveAfter)) return MMS_E_STALE;
            if (!mdl::BoneKeyPoolGrowth::Prepare(m, needed, candidate->growth)) return MMS_E_MEMORY;
            if (scaleChanged || observeAuthoring) candidate->scale = bone_scale::AuthoringRestore::Prepare(model,
                forward ? scaleAfter : scaleBefore, candidate->growth->CandidateCapacity());
            if (curvesChanged || observeAuthoring) candidate->curves = bone_curve::AuthoringRestore::Prepare(model, forward ? *curveAfter : *curveBefore);
            candidate->tailFrame=app.CurrentFrame();
            for(const auto& entry:tailCursors){
                if(!published&&((tailCursorTable&&tailCursorTable[entry.bone]!=entry.before)||
                    (tailActiveTable&&tailActiveTable[entry.bone]!=entry.active)))return MMS_E_STALE;
                candidate->currentTailCursors.push_back(tailCursorTable?tailCursorTable[entry.bone]:0);
                candidate->currentTailActive.push_back(tailActiveTable?tailActiveTable[entry.bone]:0);
                unsigned cursor=entry.bone;
                if(published&&candidate->tailFrame!=tailInitialFrame){
                    if(candidate->tailFrame<0||candidate->tailFrame>frame_limits::maximum)return MMS_E_NOT_READY;
                    for(std::size_t hop=0;hop<candidate->growth->CandidateCapacity();++hop){
                        const auto found=std::lower_bound(keys.begin(),keys.end(),cursor,[](const KeyPatch& patch,unsigned index){return patch.slot<index;});
                        const auto* key=found!=keys.end()&&found->slot==cursor&&found->data?(forward?&found->after:&found->before):
                            cursor<capacity?&m.boneKeys[cursor]:nullptr;
                        if(!key)return MMS_E_STALE;
                        if(key->frame>=unsigned(candidate->tailFrame)||!key->next)break;
                        cursor=key->next;
                    }
                }else if(!forward)cursor=entry.before;
                candidate->targetTailCursors.push_back(cursor);
                candidate->targetTailActive.push_back(forward?1:entry.active);
            }
            if (!evaluationBones.empty()) {
                struct Target { const RegistrationChange& change; bool forward; } target{*this, forward};
                BoneEvaluationView view;
                view.context = &target; view.keyCapacity = candidate->growth->CandidateCapacity();
                view.key = [](const void* context, std::uint32_t index) noexcept -> const mdl::BoneKey* {
                    const auto& target = *static_cast<const Target*>(context);
                    const auto& patches = target.change.keys;
                    const auto found = std::lower_bound(patches.begin(), patches.end(), index,
                        [](const KeyPatch& patch, unsigned slot) { return patch.slot < slot; });
                    if (found != patches.end() && found->slot == index && found->data)
                        return target.forward ? &found->after : &found->before;
                    const auto* pool = mdl::Mdl(target.change.model)->boneKeys;
                    return index < mdl::BoneKeyCapacity(pool) ? &pool[index] : nullptr;
                };
                view.scale = forward ? &scaleAfter : &scaleBefore;
                view.curves = forward ? curveAfter.get() : curveBefore.get();
                std::vector<mdl::BoneOrderEntry> selectors;
                if (display) {
                    if (auto result = display->TargetSelectors(forward, selectors)) return result;
                    view.selectors = selectors.data(); view.selectorCount = selectors.size();
                }
                if (auto result = PrepareBoneEvaluation(app, slot, evaluationBones, view, candidate->evaluation)) return result;
            }
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t result = sizeof(*this) + 4 * sizeof(void*);
        AddBytes(result, keys.capacity() * sizeof(KeyPatch)); AddBytes(result, poses.capacity() * sizeof(PosePatch));
        AddBytes(result, keyDependencies.capacity() * sizeof(unsigned)); AddBytes(result, rules.capacity() * sizeof(BoneRule));
        AddBytes(result,planningDependencies.capacity()*sizeof(mdl::BoneKey));
        AddBytes(result, evaluationBones.capacity() * sizeof(unsigned));
        AddBytes(result,tailCursors.capacity()*sizeof(TailCursor));
        AddBytes(result, pasteNames.capacity() * sizeof(std::array<char, 20>));
        AddBytes(result, displaySelectors.capacity() * sizeof(int));
        AddBytes(result, morphFlags.capacity() * sizeof(FlagPatch)); AddBytes(result, displayFlags.capacity() * sizeof(FlagPatch));
        AddBytes(result, scaleBefore.RetainedBytes()); AddBytes(result, scaleAfter.RetainedBytes());
        if (curveBefore) AddBytes(result, curveBefore->RetainedBytes());
        if (curveAfter) AddBytes(result, curveAfter->RetainedBytes());
        return result;
    }
};
// A sparse private view: traversal reads untouched slots from the live pool,
// while every write gets a complete before/after key. No native function with
// old-ring, UI or solver side effects runs during planning or publication.
struct KeyPlan {
    const mdl::ModelRecord& model;
    std::size_t capacity;
    const std::size_t initialCapacity;
    std::map<unsigned, KeyPatch> patches;
    bool trackReads;
    mutable std::set<unsigned> reads;
    explicit KeyPlan(const mdl::ModelRecord& m, bool track = false)
        : model(m), capacity(mdl::BoneKeyCapacity(m.boneKeys)), initialCapacity(capacity), trackReads(track) {}
    mdl::BoneKey Read(unsigned slot) const {
        if (trackReads && slot < initialCapacity) reads.insert(slot);
        const auto it = patches.find(slot); if (it != patches.end()) return it->second.after;
        return slot < initialCapacity ? model.boneKeys[slot] : mdl::BoneKey{};
    }
    mdl::BoneKey& Write(unsigned slot, bool data = true) {
        auto found = patches.find(slot);
        if (found == patches.end()) {
            const auto original = slot < initialCapacity ? model.boneKeys[slot] : mdl::BoneKey{};
            found = patches.emplace(slot, KeyPatch{slot, original, original, data}).first;
        }
        found->second.data |= data; return found->second.after;
    }
    void ValidateChain(unsigned bone) const {
        unsigned slot = bone, previous = 0, frame = 0;
        for (std::size_t hops = 0; hops < capacity; ++hops) {
            if (slot >= capacity || (hops && slot < model.boneCount)) throw std::invalid_argument("invalid bone chain slot");
            const auto key = Read(slot);
            if ((!hops && key.frame) || (hops && (key.previous != previous || key.frame <= frame)))
                throw std::invalid_argument("invalid bone chain link");
            if (!key.next) return;
            previous = slot; frame = key.frame; slot = key.next;
        }
        throw std::invalid_argument("cyclic bone key chain");
    }
    void Smooth(unsigned slot, int lane) {
        auto& record = Write(slot);
        const auto reset = [&] { record.interpolation[lane] = record.interpolation[lane + 4] = 20;
            record.interpolation[lane + 8] = record.interpolation[lane + 12] = 107; };
        if (lane == 3 || slot < model.boneCount) { reset(); return; }
        const auto previous = Read(record.previous);
        const float currentValue = record.position[lane], previousValue = previous.position[lane];
        if (previousValue == currentValue) { reset(); return; }
        if (record.previous < model.boneCount) { record.interpolation[lane] = 64; record.interpolation[lane + 4] = 0; }
        else {
            const float beforeValue = Read(previous.previous).position[lane];
            auto& prior = Write(record.previous);
            const bool rising = (previousValue - beforeValue) * (currentValue - previousValue) > 0;
            prior.interpolation[lane + 8] = rising ? 87 : 64;
            prior.interpolation[lane + 12] = rising ? 87 : 127;
            record.interpolation[lane] = rising ? 40 : 64; record.interpolation[lane + 4] = rising ? 40 : 0;
        }
        if (!record.next) { record.interpolation[lane + 8] = 64; record.interpolation[lane + 12] = 127; return; }
        auto& next = Write(record.next);
        const bool rising = (next.position[lane] - currentValue) * (currentValue - previousValue) > 0;
        record.interpolation[lane + 8] = rising ? 87 : 64; record.interpolation[lane + 12] = rising ? 87 : 127;
        next.interpolation[lane] = rising ? 40 : 64; next.interpolation[lane + 4] = rising ? 40 : 0;
    }
};
using KeyOwners = std::map<unsigned, unsigned>;
KeyOwners ValidateOwners(const KeyPlan& plan) {
    KeyOwners owners;
    for (unsigned bone = 0; bone < plan.model.boneCount; ++bone) {
        unsigned index = bone, previous = 0, frame = 0;
        bool ended = false;
        for (std::size_t hops = 0; hops < plan.capacity; ++hops) {
            if (index >= plan.capacity || (hops && index < plan.model.boneCount) ||
                !owners.emplace(index, bone).second) throw std::invalid_argument("shared or invalid bone chain slot");
            const auto key = plan.Read(index);
            // Legacy tail deletion writes bone zero's previous field even for
            // another track. Root previous is therefore a bounded sentinel,
            // not necessarily zero; non-root links must remain reciprocal.
            if (key.previous >= plan.capacity || key.frame > unsigned(frame_limits::maximum) ||
                (!hops && key.frame) || (hops && (key.previous != previous || key.frame <= frame)))
                throw std::invalid_argument("invalid bone chain link or frame");
            if (!key.next) { ended = true; break; }
            previous = index; frame = key.frame; index = key.next;
        }
        if (!ended) throw std::invalid_argument("cyclic bone key chain");
    }
    return owners;
}
template<class Key> bool AnyMarks(const Key* keys, std::size_t capacity) noexcept {
    for (std::size_t index = 0; keys && index < capacity; ++index) if (keys[index].allocated) return true;
    return false;
}
bool ReadyForKeyPlan(MMDApp& app, unsigned slot) {
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return false;
    const auto& model = *mdl::Mdl(app.ModelSlot(slot));
    return model.boneCount && model.boneTable && model.bonePhysicsState &&
        model.boneCount <= mdl::BoneKeyCapacity(model.boneKeys) && mdl::BoneKeyPoolIdentity(model.boneKeys);
}
std::shared_ptr<RegistrationChange> NewKeyChange(MMDApp& app, unsigned slot) {
    auto change = std::make_shared<RegistrationChange>(app, slot);
    change->scaleBefore = bone_scale::AuthoringState::Capture(change->model);
    change->scaleAfter = change->scaleBefore.Clone();
    change->curveBefore = bone_curve::AuthoringState::Capture(change->model);
    change->curveAfter = change->curveBefore->Clone();
    change->observeAuthoring = true;
    change->rulePhysicsMode = mdl::Mdl(change->model)->physicsMode;
    return change;
}
void CaptureRules(RegistrationChange& change, const std::set<unsigned>& indices) {
    change.rules.reserve(indices.size());
    for (auto index : indices) {
        const auto& bone = change.bones[index];
        BoneRule rule{index, bone.type, bone.flags, bone.hasRigidBody, bone.tailBone, {}, {}};
        std::memcpy(rule.position, bone.position, sizeof(rule.position));
        std::memcpy(rule.axis, bone.axis, sizeof(rule.axis));
        change.rules.push_back(rule);
    }
}
void ClearAuxiliaryMarks(RegistrationChange& change) {
    const auto& model = *mdl::Mdl(change.model);
    for (std::size_t index = 0; index < change.morphCapacity; ++index)
        if (model.morphKeys[index].allocated) change.morphFlags.push_back({unsigned(index), model.morphKeys[index].allocated});
    for (std::size_t index = 0; index < change.displayCapacity; ++index)
        if (model.displayKeys[index].allocated) change.displayFlags.push_back({unsigned(index), model.displayKeys[index].allocated});
}
void DeletePlannedKeys(KeyPlan& plan, const std::vector<unsigned>& selected,
                       bone_scale::AuthoringState& scale, bool automaticInterpolation) {
    for (auto index : selected) {
        auto& key = plan.Write(index);
        const auto previous = key.previous, next = key.next;
        if (previous >= plan.capacity || next >= plan.capacity) throw std::invalid_argument("invalid deletion neighbour");
        scale.ClearKey(int(index));
        key.frame = 0;
        if (index >= plan.model.boneCount) {
            plan.Write(previous).next = next;
            plan.Write(next).previous = previous;
        }
        key.allocated = 0;
        key.position[0] = key.position[1] = key.position[2] = 0;
        key.rotation[0] = key.rotation[1] = key.rotation[2] = 0; key.rotation[3] = 1;
        if (automaticInterpolation) for (int lane = 0; lane < 4; ++lane) plan.Smooth(next ? next : previous, lane);
        if (index >= plan.model.boneCount) key.next = 0;
        key.previous = 0;
        if (index < plan.model.boneCount && plan.model.boneTable[index].hasRigidBody) key.physicsDisabled = 0;
    }
}
using CurveRegistrations = std::map<unsigned, std::vector<int>>;
using CurveFrameMaps = std::map<unsigned, std::vector<std::pair<int,int>>>;
void ReconcilePlannedCurves(const KeyPlan& plan, RegistrationChange& change, std::set<unsigned>& affected,
    const CurveRegistrations& registrations, const CurveFrameMaps* frameMaps = nullptr) {
    struct Context { const KeyPlan& plan; const bone_scale::AuthoringState& scale; } context{plan,change.scaleAfter};
    bone_curve::NativeKeyView view;view.context=&context;view.capacity=plan.capacity;
    view.key=[](const void* pointer,std::uint32_t slot) noexcept ->const mdl::BoneKey* {
        const auto& plan=static_cast<const Context*>(pointer)->plan;
        if(slot>=plan.capacity)return nullptr;
        const auto found=plan.patches.find(slot);if(found!=plan.patches.end())return &found->second.after;
        if(slot<plan.initialCapacity)return plan.model.boneKeys+slot;
        static const mdl::BoneKey empty{};return &empty;
    };
    view.scale=[](const void* pointer,std::uint32_t slot,bone_scale::Matrix& matrix,bone_scale::Curve& curve) noexcept {
        const auto& context=*static_cast<const Context*>(pointer);if(slot>=context.plan.capacity)return false;
        matrix=context.scale.Key(int(slot));curve=context.scale.KeyCurve(int(slot));return true;
    };
    auto targets=affected;for(const auto& entry:registrations)targets.insert(entry.first);
    const std::vector<int> none;
    const std::vector<std::pair<int,int>> emptyMap;
    for(const auto bone:targets){
        const auto registered=registrations.find(bone);
        const auto mapped=frameMaps?frameMaps->find(bone):CurveFrameMaps::const_iterator{};
        const auto* map=frameMaps?(mapped==frameMaps->end()?&emptyMap:&mapped->second):nullptr;
        if(change.curveAfter->ReconcileTarget(int(bone),view,registered==registrations.end()?none:registered->second,map))affected.insert(bone);
    }
}
CurveFrameMaps RetainedFrameMaps(const std::map<std::pair<unsigned,unsigned>,unsigned>& winners) {
    CurveFrameMaps result;
    for(const auto& winner:winners)result[winner.first.first].push_back({int(winner.second),int(winner.first.second)});
    return result;
}
bool FinishKeyPlan(KeyPlan& plan, RegistrationChange& change, const KeyOwners& originalOwners,
    const CurveRegistrations& registrations = {}, const CurveFrameMaps* frameMaps = nullptr) {
    const auto owners = ValidateOwners(plan);
    std::set<unsigned> affected;
    const auto affect = [&](const KeyOwners& source, unsigned index) {
        const auto found = source.find(index);
        if (found != source.end()) affected.insert(found->second);
    };
    for (const auto& item : plan.patches) {
        const auto& patch = item.second;
        // Native curve hashes exclude marks/reserved bytes, and include scale
        // matrices and scale interpolation. Rebuild only affected authors,
        // including both owners when a freed slot was reused by another bone.
        if (!SameCurveKeyData(patch.before, patch.after) ||
            change.scaleBefore.Key(int(patch.slot)) != change.scaleAfter.Key(int(patch.slot)) ||
            change.scaleBefore.KeyCurve(int(patch.slot)) != change.scaleAfter.KeyCurve(int(patch.slot))) {
            affect(originalOwners, patch.slot); affect(owners, patch.slot);
        }
        if (std::memcmp(&patch.before, &patch.after, sizeof(mdl::BoneKey))) change.keys.push_back(patch);
    }
    ReconcilePlannedCurves(plan,change,affected,registrations,frameMaps);
    change.scaleChanged = !change.scaleBefore.Equals(change.scaleAfter);
    change.curvesChanged = !change.curveBefore->Equals(*change.curveAfter);
    change.keyDependencies.assign(plan.reads.begin(), plan.reads.end());
    for(const auto index:change.keyDependencies)change.planningDependencies.push_back(plan.model.boneKeys[index]);
    change.evaluationBones.assign(affected.begin(), affected.end());
    return !change.keys.empty() || !change.morphFlags.empty() || !change.displayFlags.empty() ||
        change.scaleChanged || change.curvesChanged;
}
void AttachDisplay(RegistrationChange& change, std::shared_ptr<DisplayKeyDeletion> display) {
    change.display = std::move(display);
    if (!change.display) return;
    for (unsigned index = 0; index < change.count; ++index) {
        change.displaySelectors.push_back(change.bones[index].slotIndex);
        if (change.bones[index].slotIndex >= 0) change.evaluationBones.push_back(index);
    }
    std::sort(change.evaluationBones.begin(), change.evaluationBones.end());
    change.evaluationBones.erase(std::unique(change.evaluationBones.begin(), change.evaluationBones.end()), change.evaluationBones.end());
}
struct RangeKeyCopy {
    unsigned owner, frame;
    mdl::BoneKey key;
    bone_scale::Matrix scale;
    bone_scale::Curve scaleCurve = bone_scale::LinearCurve();
};
unsigned ScaledBoneFrame(unsigned frame, const BoneRangeScaleOptions& options) {
    // Check in floating point before conversion. This preserves the legacy
    // truncate-toward-zero mapping without its intermediate uint32 wraparound.
    const double span = double(frame <= options.end ? frame - options.start : options.end - options.start) * options.scale;
    if (!std::isfinite(span) || span < 0 || span > double(frame_limits::maximum))
        throw std::invalid_argument("scaled bone frame overflow");
    const auto distance = std::uint64_t(span);
    const auto target = std::uint64_t(options.start) + distance + (frame > options.end ? std::uint64_t(frame - options.end) : 0);
    if (target > std::uint64_t(frame_limits::maximum)) throw std::invalid_argument("scaled bone frame overflow");
    return unsigned(target);
}
bool PrepareRangeRotation(const mdl::ModelRecord& model, RangeKeyCopy& copy, std::set<unsigned>& rules,
    bool mirror = false) {
    const auto& bone = model.boneTable[copy.owner];
    if (bone.type != mdl::BoneType::FixedAxis &&
        !(bone.type == mdl::BoneType::UnderIk && (bone.flags & mdl::kBoneFlagFixedAxis))) {
        if (mirror) { copy.key.rotation[1] = -copy.key.rotation[1]; copy.key.rotation[2] = -copy.key.rotation[2]; }
        return true;
    }
    auto& math = d3dx::Get();
    if (!math.Load() || !math.quatToAxisAngle || !math.quatNormalize || !math.vec3Normalize) return false;
    float axis[3];
    if (model.physicsMode == 2) std::memcpy(axis, bone.axis, sizeof(axis));
    else {
        if (bone.tailBone < 0 || unsigned(bone.tailBone) >= model.boneCount)
            throw std::invalid_argument("fixed-axis bone tail outside model");
        rules.insert(unsigned(bone.tailBone));
        const auto& tail = model.boneTable[bone.tailBone];
        for (int coordinate = 0; coordinate < 3; ++coordinate) axis[coordinate] = tail.position[coordinate] - bone.position[coordinate];
        math.vec3Normalize(axis, axis);
    }
    auto* rotation = copy.key.rotation;
    float sourceAxis[3], angle;
    math.quatToAxisAngle(rotation, sourceAxis, &angle);
    if (!std::isfinite(rotation[3])) rotation[3] = 1;
    const float square = rotation[3] * rotation[3];
    float sine = square <= 1 ? std::sqrt(1 - square) : 0;
    if (square > 1) rotation[3] = 1;
    // Follow the mirrored registrar's exact comparison order. Reflecting the
    // input quaternion first differs when the projection dot is exactly zero.
    if (mirror) { sourceAxis[0] = -sourceAxis[0]; sine = -sine; }
    const float dot = axis[0] * sourceAxis[0] + axis[1] * sourceAxis[1] + axis[2] * sourceAxis[2];
    const float lengthA = std::sqrt(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
    const float lengthB = std::sqrt(sourceAxis[0]*sourceAxis[0] + sourceAxis[1]*sourceAxis[1] + sourceAxis[2]*sourceAxis[2]);
    if (dot / (lengthA * lengthB) < 0) sine = -sine;
    for (int coordinate = 0; coordinate < 3; ++coordinate) rotation[coordinate] = sine * axis[coordinate];
    if (mirror) { rotation[1] = -rotation[1]; rotation[2] = -rotation[2]; }
    math.quatNormalize(rotation, rotation);
    // FillMirroredBoneRecord reflects the corrected native payload once more.
    if (mirror) { rotation[1] = -rotation[1]; rotation[2] = -rotation[2]; }
    for (auto value : copy.key.rotation) if (!std::isfinite(value)) throw std::invalid_argument("invalid fixed-axis rotation");
    return true;
}
unsigned InsertRangeCopy(KeyPlan& plan, const RangeKeyCopy& copy, unsigned start, unsigned& cursor,
                     RegistrationChange& change) {
    unsigned current = copy.owner;
    for (std::size_t hops = 0; plan.Read(current).frame < copy.frame && plan.Read(current).next; ++hops) {
        if (hops >= plan.capacity) throw std::invalid_argument("cyclic planned bone chain");
        current = plan.Read(current).next;
    }
    const auto existing = plan.Read(current);
    const bool fresh = existing.frame != copy.frame;
    const bool appended = fresh && existing.frame < copy.frame;
    unsigned target = current;
    if (fresh) {
        while (std::uint64_t(cursor) < mdl::kBoneKeyCapacityLimit && plan.Read(cursor).frame) ++cursor;
        if (std::uint64_t(cursor) >= mdl::kBoneKeyCapacityLimit) throw std::bad_alloc();
        target = cursor;
        plan.capacity = (std::max)(plan.capacity, std::size_t(target) + 1);
        auto& key = plan.Write(target);
        if (appended) { plan.Write(current).next = target; key.previous = current; key.next = 0; }
        else {
            plan.Write(existing.previous).next = target; key.previous = existing.previous;
            plan.Write(current).previous = target; key.next = current;
        }
        key.frame = copy.frame;
    }
    if (!change.scaleAfter.RegisterKey(int(target), copy.scale, fresh)) throw std::invalid_argument("invalid keyed bone scale");
    if (!change.scaleAfter.SetKeyCurve(int(target), copy.scaleCurve)) throw std::invalid_argument("invalid keyed bone scale curve");
    auto& destination = plan.Write(target);
    const auto type = plan.model.boneTable[copy.owner].type;
    if (type == mdl::BoneType::RotateMove || type == mdl::BoneType::Move || type == mdl::BoneType::Ik)
        std::memcpy(destination.position, copy.key.position, sizeof(destination.position));
    std::memcpy(destination.rotation, copy.key.rotation, sizeof(destination.rotation));
    std::memcpy(destination.interpolation, copy.key.interpolation, sizeof(destination.interpolation));
    destination.physicsDisabled = copy.key.physicsDisabled; destination.allocated = 1;
    // RegisterBoneKey bumps by target frame only on append. Its overwrite and
    // insert-before branches retain the original raw-offset maximum quirk.
    change.maxAfter = (std::max)(change.maxAfter, appended ? copy.frame : start);
    return target;
}
struct BoneTailView {
    const KeyPlan& plan;const bone_scale::AuthoringState& scale;
    bone_curve::NativeKeyView View()const noexcept{
        return {this,plan.capacity,[](const void* p,std::uint32_t slot)noexcept->const mdl::BoneKey*{
            const auto& plan=static_cast<const BoneTailView*>(p)->plan;if(slot>=plan.capacity)return nullptr;
            const auto found=plan.patches.find(slot);if(found!=plan.patches.end())return &found->second.after;
            if(slot<plan.initialCapacity)return &plan.model.boneKeys[slot];static const mdl::BoneKey empty{};return &empty;},
            [](const void* p,std::uint32_t slot,bone_scale::Matrix& value,bone_scale::Curve& curve)noexcept{
                const auto& target=*static_cast<const BoneTailView*>(p);if(slot>=target.plan.capacity)return false;
                value=target.scale.Key(int(slot));curve=target.scale.KeyCurve(int(slot));return true;}};
    }
};
std::vector<unsigned> BoneTailChain(const KeyPlan& plan,unsigned bone){
    plan.ValidateChain(bone);std::vector<unsigned> slots;unsigned index=bone;
    for(;;){slots.push_back(index);const auto key=plan.Read(index);if(!key.next)break;index=key.next;}return slots;
}
void BoneTailQuaternion(const bone_curve::Tracks& tracks,int frame,float* q){
    constexpr double pi=3.14159265358979323846;
    const double x=animation_curve::Evaluate(tracks[3],frame).value*pi/360,y=animation_curve::Evaluate(tracks[4],frame).value*pi/360,
        z=animation_curve::Evaluate(tracks[5],frame).value*pi/360;
    const double sx=std::sin(x),cx=std::cos(x),sy=std::sin(y),cy=std::cos(y),sz=std::sin(z),cz=std::cos(z);
    q[0]=float(cy*sx*cz+sy*cx*sz);q[1]=float(sy*cx*cz-cy*sx*sz);
    q[2]=float(cy*cx*sz-sy*sx*cz);q[3]=float(cy*cx*cz+sy*sx*sz);
}
bone_scale::Matrix BoneTailScale(const bone_curve::Tracks& tracks,int frame,bone_scale::Matrix matrix){
    for(unsigned row=0;row<3;++row){double norm=0;for(unsigned c=0;c<3;++c)norm+=double(matrix.v[row*3+c])*matrix.v[row*3+c];
        norm=std::sqrt(norm);const double value=animation_curve::Evaluate(tracks[6+row],frame).value;
        for(unsigned c=0;c<3;++c)matrix.v[row*3+c]=float(norm>1e-12?matrix.v[row*3+c]*value/norm:(c==row?value:0));}
    return matrix;
}
// Reproduce bone_curve::Write against the post-standard private pool. This
// participant never calls a live registrar, side-table swap or TLS import scope.
bool MirrorBoneTail(KeyPlan& plan,bone_scale::AuthoringState& scale,bone_curve::AuthoringState& curves,
    const bone_curve::VmdTrackImport& imported,std::set<unsigned>& marks,unsigned& maximum,std::string& error){
    if(!bone_curve::ValidateFreeTracks(imported.tracks,error))return false;
    const unsigned bone=unsigned(imported.bone);const auto oldSlots=BoneTailChain(plan,bone);
    std::set<int> frames;for(const auto& track:imported.tracks)for(const auto& key:track.keys)frames.insert(key.frame);
    for(auto slot:oldSlots)if(!frames.count(int(plan.Read(slot).frame))){error="The free-curve VMD overlaps existing bone motion.";return false;}
    std::vector<mdl::BoneKey> original;original.reserve(oldSlots.size());
    std::vector<bone_scale::Matrix> matrices;std::vector<bone_scale::Curve> controls;
    for(auto index:oldSlots){original.push_back(plan.Read(index));matrices.push_back(scale.Key(int(index)));controls.push_back(scale.KeyCurve(int(index)));}
    std::vector<unsigned> slots=oldSlots;
    for(std::size_t index=plan.model.boneCount;slots.size()<frames.size()&&index<plan.capacity;++index)
        if(!plan.Read(unsigned(index)).frame&&std::find(oldSlots.begin(),oldSlots.end(),unsigned(index))==oldSlots.end())slots.push_back(unsigned(index));
    while(slots.size()<frames.size()){
        if(plan.capacity>=mdl::kBoneKeyCapacityLimit)throw std::bad_alloc();slots.push_back(unsigned(plan.capacity++));}
    std::size_t at=0,n=0;
    for(int frame:frames){while(at+1<original.size()&&original[at+1].frame<=unsigned(frame))++at;
        auto key=original[at];key.frame=unsigned(frame);key.allocated=1;key.previous=n?slots[n-1]:0;key.next=n+1<frames.size()?slots[n+1]:0;
        for(unsigned c=0;c<3;++c)key.position[c]=float(animation_curve::Evaluate(imported.tracks[c],frame).value);
        BoneTailQuaternion(imported.tracks,frame,key.rotation);
        for(unsigned lane=0;lane<4;++lane)for(unsigned c=0;c<4;++c)key.interpolation[lane+4*c]=c<2?20:107;
        auto matrix=matrices[at];
        if(at+1<original.size()){const float time=float(std::clamp((double(frame)-original[at].frame)/double(original[at+1].frame-original[at].frame),0.0,1.0));
            matrix=bone_scale::Interpolate(matrix,matrices[at+1],bone_scale::CurveTime(controls[at+1],time));}
        matrix=BoneTailScale(imported.tracks,frame,matrix);
        if(!bone_scale::Valid(matrix)){error="Invalid bone scale.";return false;}
        if(!scale.SetKey(int(slots[n]),matrix)||!scale.SetKeyCurve(int(slots[n]),bone_scale::LinearCurve()))throw std::invalid_argument("bone tail scale storage");
        plan.Write(slots[n])=key;marks.insert(slots[n]);++n;
    }
    for(std::size_t index=frames.size();index<oldSlots.size();++index){plan.Write(oldSlots[index])={};scale.ClearKey(int(oldSlots[index]));}
    const BoneTailView view{plan,scale};curves.Replace(imported.bone,imported.tracks,view.View());
    maximum=(std::max)(maximum,unsigned(*frames.rbegin()));return true;
}
struct BoneTailResult {bool applied=false,skipped=false;std::set<unsigned> bones;};
bool ValidateTailCursorStorage(MMDApp& app,unsigned ownSlot)noexcept{
    if(ownSlot>=kModelSlotCount||!app.ModelSlot(ownSlot))return false;
    const auto& own=*mdl::Mdl(app.ModelSlot(ownSlot));
    struct Range{std::uintptr_t first=0,last=0;};std::array<Range,2> writes{};unsigned writeCount=0;
    const auto range=[](const void* p,std::size_t count,std::size_t size,Range& out)noexcept{const auto first=reinterpret_cast<std::uintptr_t>(p);
        if(!size||count>(UINTPTR_MAX-first)/size)return false;out={first,first+count*size};return true;};
    if(own.boneKeyCursors&&!range(own.boneKeyCursors,own.boneCount,sizeof(std::uint32_t),writes[writeCount++]))return false;
    if(own.boneTrackActive&&!range(own.boneTrackActive,own.boneCount,1,writes[writeCount++]))return false;
    if(writeCount==2&&writes[0].first<writes[1].last&&writes[1].first<writes[0].last)return false;
    if(!writeCount)return true;
    const auto protect=[&](const void* p,std::size_t count,std::size_t size)noexcept{if(!p||!count)return true;Range target;
        if(!range(p,count,size,target))return false;for(unsigned i=0;i<writeCount;++i)if(writes[i].first<target.last&&target.first<writes[i].last)return false;return true;};
    if(!protect(&app.state,1,sizeof app.state))return false;
    for(unsigned slot=0;slot<kModelSlotCount;++slot)if(app.ModelSlot(slot)){
        const auto& m=*mdl::Mdl(app.ModelSlot(slot));
        if(!protect(&m,1,sizeof m)||!protect(m.boneTable,m.boneCount,sizeof(mdl::BoneRecord))||
            !protect(m.bonePhysicsState,m.boneCount,1)||!protect(m.boneSelection,m.boneCount,1)||
            !protect(m.boneKeyIndices,m.boneCount,sizeof(std::uint32_t))||!protect(m.morphKeyIndices,m.morphCount,sizeof(std::uint32_t))||
            !protect(m.morphs,m.morphCount,sizeof(mdl::MorphRecord))||!protect(m.morphKeyCursors,m.morphCount,sizeof(std::uint32_t))||!protect(m.morphTrackActive,m.morphCount,1))return false;
        if(slot!=ownSlot&&(!protect(m.boneKeyCursors,m.boneCount,sizeof(std::uint32_t))||!protect(m.boneTrackActive,m.boneCount,1)))return false;
        if(m.boneKeys&&!protect(m.boneKeys,mdl::BoneKeyCapacity(m.boneKeys),sizeof(mdl::BoneKey)))return false;
        if(m.morphKeys&&mdl::MorphKeyPoolIdentity(m.morphKeys)&&!protect(m.morphKeys,mdl::MorphKeyCapacity(m.morphKeys),sizeof(mdl::MorphKey)))return false;
        if(m.displayKeys&&mdl::KeyPoolIdentity(m.displayKeys)){const auto capacity=mdl::KeyPoolCapacity(m.displayKeys);if(!protect(m.displayKeys,capacity,sizeof(mdl::DisplayKey)))return false;
            for(std::size_t i=0;i<capacity;++i)if(!protect(m.displayKeys[i].ikStates,m.ikChainCount,1)||!protect(m.displayKeys[i].selectorStates,m.boneOrderCount,sizeof(mdl::BoneReference)))return false;}
        if(!protect(m.ikChains,m.ikChainCount,sizeof(mdl::IkChain))||!protect(m.boneOrderTable,m.boneOrderCount,sizeof(mdl::BoneOrderEntry)))return false;
        if(m.ikChains)for(unsigned i=0;i<m.ikChainCount;++i)if(!protect(m.ikChains[i].links,m.ikChains[i].linkCount,sizeof(std::uint16_t)))return false;
    }
    return true;
}
BoneTailResult ApplyBoneTail(KeyPlan& plan,RegistrationChange& change,const ModelPasteSource& input,std::set<unsigned>& marks){
    BoneTailResult result;if(input.boneCurveBlock.empty())return result;
    std::string error;std::vector<bone_curve::VmdTrackImport> imports;
    if(!bone_curve::DecodeVmdTracks(change.model,input.boneCurveBlock.data(),input.boneCurveBlock.size(),int(input.frameOffset),
        input.curveTargetsResolved?&input.boneCurveTargets:nullptr,imports,error)){result.skipped=true;return result;}
    if(imports.empty())return result;
    // A failure in any bone abandons the complete author block, while the
    // earlier private standard+scale import remains available for publication.
    KeyPlan target=plan;auto scale=change.scaleAfter.Clone();auto curves=change.curveAfter->Clone();
    auto targetMarks=marks;auto maximum=change.maxAfter;
    try{for(const auto& imported:imports){
            if(!MirrorBoneTail(target,scale,*curves,imported,targetMarks,maximum,error)){result.skipped=true;return result;}
            result.bones.insert(unsigned(imported.bone));
        }}catch(const std::bad_alloc&){throw;}catch(const std::exception&){result.bones.clear();result.skipped=true;return result;}
    plan.patches.swap(target.patches);plan.reads.swap(target.reads);plan.capacity=target.capacity;
    change.scaleAfter=std::move(scale);change.curveAfter=std::move(curves);change.maxAfter=maximum;
    marks.swap(targetMarks);result.applied=true;return result;
}
}
MmsResult PlanBoneKeyPaste(MMDApp& app, unsigned slot, bool mirror,
    std::shared_ptr<HistoryChange>& out, ModelPasteFamilyResult* aggregate,
    std::shared_ptr<DisplayKeyDeletion> display) noexcept {
    out.reset();if(aggregate)*aggregate={};
    if(!ReadyForKeyPlan(app,slot))return MMS_E_NOT_READY;
    ModelPasteSource source;
    if(auto result=CaptureModelPasteSource(app,source,ModelPasteBones))return result;
    return PlanBoneKeyPaste(app,slot,mirror,source,out,aggregate,std::move(display));
}
MmsResult PlanBoneKeyPaste(MMDApp& app,unsigned slot,bool mirror,const ModelPasteSource& input,
    std::shared_ptr<HistoryChange>& out,ModelPasteFamilyResult* aggregate,
    std::shared_ptr<DisplayKeyDeletion> display) noexcept {
    out.reset();
    if(aggregate)*aggregate={};
    if(auto result=ValidateModelPasteSource(input,ModelPasteBones))return result;
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if(aggregate){aggregate->maxFrame=model.maxFrame;aggregate->searchCursor=model.searchCursor;}
        const auto displayOnly=[&]()->MmsResult {
            if(aggregate&&display) {
                if(auto result=PlanBoneDisplayEvaluation(app,slot,display,out))return result;
                aggregate->change=out;
            }
            return MMS_OK;
        };
        const auto count = input.bones.size();
        if (!count&&input.boneCurveBlock.empty()) return displayOnly();
        if (count > mdl::kBoneKeyCapacityLimit || app.CurrentFrame() < 0)
            return MMS_E_ARGUMENT;
        KeyPlan plan(model, true);
        const auto owners = ValidateOwners(plan);
        std::vector<std::string> names;
        const bool resolved=std::all_of(input.bones.begin(),input.bones.end(),[](const auto& record){return record.targetResolved;});
        auto change = NewKeyChange(app, slot);
        change->publishMetadata=aggregate==nullptr;
        change->preserveKeyMarks=aggregate!=nullptr;
        // Freeze the name table which selected the destination, including
        // unmatched names and the native suffix-only left/right search.
        for (unsigned bone = 0; bone < model.boneCount; ++bone) {
            const auto length = strnlen_s(model.boneTable[bone].name, sizeof(model.boneTable[bone].name));
            if (length == sizeof(model.boneTable[bone].name)&&!resolved) return MMS_E_ARGUMENT;
            names.emplace_back(model.boneTable[bone].name, length);
            std::array<char, 20> name{};
            std::memcpy(name.data(), model.boneTable[bone].name, name.size());
            change->pasteNames.push_back(name);
        }
        const auto find = [&](const std::string& source) -> int {
            if (mirror) for (const auto& markers : {std::pair<const char*, const char*>{"\x8d\xb6", "\x89\x45"},
                                                   {"\x89\x45", "\x8d\xb6"}}) {
                const auto hit = source.find(markers.first);
                if (hit == std::string::npos) continue;
                int target = -1;
                for (unsigned bone = 0; bone < names.size(); ++bone) {
                    const auto other = names[bone].find(markers.second);
                    if (other != std::string::npos && source.substr(hit + 2) == names[bone].substr(other + 2)) {
                        if (target >= 0) return -2;
                        target = int(bone);
                    }
                }
                if (target >= 0) return target;
            }
            int target = -1;
            for (unsigned bone = 0; bone < names.size(); ++bone) if (names[bone] == source) {
                if (target >= 0) return -2;
                target = int(bone);
            }
            return target;
        };
        struct Copy { RangeKeyCopy key; bone_scale::Curve curve; };
        std::vector<Copy> copies;
        std::set<unsigned> rules;
        for (unsigned index = 0; index < count; ++index) {
            const auto& record=input.bones[index];const auto& source=record.key;
            const auto length = strnlen_s(source.name, sizeof(source.name));
            unsigned frame;
            if ((!record.targetResolved&&length == sizeof(source.name)) || !frame_limits::Add(int(input.frameOffset), source.frame, frame)) return MMS_E_ARGUMENT;
            const int bone=record.targetResolved?record.targetIndex:find(std::string(source.name,length));
            // Unrelated duplicate names are harmless. Every destination used
            // by this paste, including suffix-based mirror mapping, must be unique.
            if (bone == -2) return MMS_E_ARGUMENT;
            if (bone < 0) continue;
            if(unsigned(bone)>=model.boneCount)return MMS_E_ARGUMENT;
            Copy copy{{unsigned(bone),frame,{},record.scale},record.scaleCurve};
            if(mirror)for(int axis:{1,2,3,6})copy.key.scale.v[axis]=-copy.key.scale.v[axis];
            std::memcpy(copy.key.key.position, source.position, sizeof(source.position));
            std::memcpy(copy.key.key.rotation, source.rotation, sizeof(source.rotation));
            std::memcpy(copy.key.key.interpolation, source.interpolation, sizeof(source.interpolation));
            copy.key.key.physicsDisabled = source.physicsDisabled;
            for (float value : copy.key.key.position) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            for (float value : copy.key.key.rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            for (auto value : copy.key.key.interpolation) if (value > 127) return MMS_E_ARGUMENT;
            for(auto value:copy.curve)if(value>127)return MMS_E_ARGUMENT;
            if (!bone_scale::Valid(copy.key.scale)) return MMS_E_ARGUMENT;
            if (mirror) {
                copy.key.key.position[0] = -copy.key.key.position[0];
            }
            rules.insert(unsigned(bone));
            if (!PrepareRangeRotation(model, copy.key, rules, mirror)) return MMS_E_NOT_READY;
            copies.push_back(copy);
        }
        if (copies.empty()&&input.boneCurveBlock.empty()) {
            if(aggregate&&!display) {
                FinishKeyPlan(plan,*change,owners);
                out=change;aggregate->change=std::move(change);
                return MMS_OK;
            }
            return displayOnly();
        }
        if(!aggregate) {
            ClearAuxiliaryMarks(*change);
            for (unsigned index = 0; index < plan.initialCapacity; ++index)
                if (model.boneKeys[index].allocated) plan.Write(index, false).allocated = 0;
        }
        std::set<unsigned> pastedSlots;
        CurveRegistrations registrations;
        unsigned cursor = model.boneCount;
        while (cursor < plan.capacity && plan.Read(cursor).frame) ++cursor;
        for (const auto& copy : copies) {
            const auto target = InsertRangeCopy(plan, copy.key, input.frameOffset, cursor, *change);
            pastedSlots.insert(target);
            registrations[copy.key.owner].push_back(int(copy.key.frame));
            // Mirror registrar writes translation even for rotation-only bones.
            if (mirror) std::memcpy(plan.Write(target).position, copy.key.key.position, sizeof(copy.key.key.position));
            if (!change->scaleAfter.SetKeyCurve(int(target), copy.curve)) return MMS_E_ARGUMENT;
        }
        const auto registrationMaximum=change->maxAfter;
        const auto tail=ApplyBoneTail(plan,*change,input,pastedSlots);
        bool tailCursorChanged=false;
        if(tail.applied){if(!ValidateTailCursorStorage(app,slot))return MMS_E_ARGUMENT;change->tailCursorTable=model.boneKeyCursors;change->tailActiveTable=model.boneTrackActive;
            change->tailInitialFrame=app.CurrentFrame();
            for(auto bone:tail.bones){registrations.erase(bone);rules.insert(bone);
                change->tailCursors.push_back({bone,model.boneKeyCursors?model.boneKeyCursors[bone]:0,
                    static_cast<unsigned char>(model.boneTrackActive?model.boneTrackActive[bone]:0)});
                tailCursorChanged|=(model.boneKeyCursors&&model.boneKeyCursors[bone]!=bone)||(model.boneTrackActive&&model.boneTrackActive[bone]!=1);}}
        // Ordinary paste resets the shared cursor to the display pool even
        // when that clipboard is empty; mirror paste keeps the bone cursor.
        if (!mirror&&!aggregate) {
            cursor = 1;
            while (cursor < change->displayCapacity && model.displayKeys[cursor].frame) ++cursor;
        }
        change->searchAfter = tail.applied?int(model.boneCount):int(cursor);
        // Pasting does not update the last-registration UI marker.
        CaptureRules(*change, rules);
        const bool changed=FinishKeyPlan(plan,*change,owners,registrations);
        if(tail.applied){change->evaluationBones.insert(change->evaluationBones.end(),tail.bones.begin(),tail.bones.end());
            std::sort(change->evaluationBones.begin(),change->evaluationBones.end());
            change->evaluationBones.erase(std::unique(change->evaluationBones.begin(),change->evaluationBones.end()),change->evaluationBones.end());}
        AttachDisplay(*change,std::move(display));
        if(aggregate) {
            aggregate->hasKeys=!copies.empty()||tail.applied;aggregate->maxFrame=change->maxAfter;aggregate->searchCursor=change->searchAfter;
            aggregate->markedSlots.assign(pastedSlots.begin(),pastedSlots.end());
            aggregate->overridesSearchCursor=tail.applied;aggregate->curveBlockSkipped=tail.skipped;
            if(tail.applied){aggregate->hasRegistrationExtent=true;aggregate->registrationMaxFrame=registrationMaximum;}
            aggregate->dataChanged=change->scaleChanged||change->curvesChanged||tailCursorChanged;
            for(const auto& patch:change->keys)aggregate->dataChanged|=!SameKeyData(patch.before,patch.after);
            // An unchanged source family still participates when another family
            // changes, so its selection and target display sampling are retained.
            out=change;aggregate->change=std::move(change);
        } else if(changed||tailCursorChanged||change->display||change->searchBefore!=change->searchAfter||change->maxBefore!=change->maxAfter)
            out=std::move(change);
        return MMS_OK;
    });
}
MmsResult PlanBoneInterpolationEdit(MMDApp& app, unsigned slot, const BoneInterpolationOptions& options,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (options.channels & ~31u) return MMS_E_ARGUMENT;
    for (unsigned channel = 0; channel < options.values.size(); ++channel)
        if (options.channels & (1u << channel))
            for (auto value : options.values[channel]) if (value > 127) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (!options.channels || !AnyMarks(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys))) return MMS_OK;
        KeyPlan plan(model, true);
        const auto owners = ValidateOwners(plan);
        auto change = NewKeyChange(app, slot);
        for (std::size_t index = 0; index < plan.capacity; ++index) {
            if (!model.boneKeys[index].allocated) continue;
            if (!owners.count(unsigned(index))) return MMS_E_ARGUMENT;
            // Even a scale-only edit must enter this sparse view: FinishKeyPlan
            // uses touched slots to reconcile their actual free-curve owner.
            auto& key = plan.Write(unsigned(index));
            for (unsigned channel = 0; channel < 4; ++channel)
                if (options.channels & (1u << channel))
                    for (unsigned point = 0; point < 4; ++point)
                        key.interpolation[point * 4 + channel] = options.values[channel][point];
            if ((options.channels & 16u) && !change->scaleAfter.SetKeyCurve(int(index), options.values[4]))
                return MMS_E_ARGUMENT;
        }
        const bool changed = FinishKeyPlan(plan, *change, owners);
        // The old apply/reset buttons only repaint the interpolation panel.
        // Keep their no-seek semantics in both history directions as well.
        change->evaluationBones.clear();
        change->publishMetadata = false;
        change->preserveKeyMarks = true;
        if (changed) out = std::move(change);
        return MMS_OK;
    });
}
MmsResult PlanBoneKeyMultiply(MMDApp& app, unsigned slot, const BoneMultiplyOptions& options,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    bool changed = false, rotate = false, resize = false;
    for (unsigned lane = 0; lane < 9; ++lane) {
        if (!std::isfinite(options.scale[lane]) || !std::isfinite(options.offset[lane])) return MMS_E_ARGUMENT;
        if (options.scale[lane] != 1.f || options.offset[lane] != 0.f) {
            changed = true; rotate |= lane >= 3 && lane < 6; resize |= lane >= 6;
        }
    }
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (!changed || !AnyMarks(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys))) return MMS_OK;
        KeyPlan plan(model, true); const auto owners = ValidateOwners(plan);
        auto change = NewKeyChange(app, slot);
        auto& api = d3dx::Get();
        if (rotate && !api.Load()) return MMS_E_NOT_READY;
        constexpr float pi = 3.141592f;
        auto add = options.offset;
        add[3] *= pi / 180.f; add[4] *= -pi / 180.f; add[5] *= -pi / 180.f;
        for (std::size_t index = 0; index < plan.capacity; ++index) {
            const auto original = model.boneKeys[index];
            if (!original.allocated) continue;
            if (!owners.count(unsigned(index))) return MMS_E_ARGUMENT;
            auto target = original;
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (options.scale[axis] != 1.f || add[axis] != 0.f)
                    target.position[axis] = target.position[axis] * options.scale[axis] + add[axis];
                if (!std::isfinite(target.position[axis])) return MMS_E_ARGUMENT;
            }
            auto stretch = change->scaleBefore.Key(int(index));
            if (resize) {
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (options.scale[axis + 6] == 1.f && add[axis + 6] == 0.f) continue;
                    double length = 0;
                    for (unsigned column = 0; column < 3; ++column)
                        length += double(stretch.v[axis * 3 + column]) * stretch.v[axis * 3 + column];
                    length = std::sqrt(length);
                    const double result = length * options.scale[axis + 6] + add[axis + 6];
                    if (!(length > 0) || !(result > 0) || !std::isfinite(result)) return MMS_E_ARGUMENT;
                    for (unsigned column = 0; column < 3; ++column)
                        stretch.v[axis * 3 + column] = float(stretch.v[axis * 3 + column] * result / length);
                }
                if (!bone_scale::Valid(stretch)) return MMS_E_ARGUMENT;
            }
            if (rotate) {
                for (float value : target.rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
                d3dx::D3DXMATRIXF matrix{}, rotation{}, axisMatrix{};
                api.matrixRotationQuaternion(&matrix, target.rotation);
                const float x = std::asin(std::fmax(-1.f, std::fmin(1.f, -matrix.m[2][1])));
                float y = std::atan2(matrix.m[2][0], matrix.m[2][2]);
                float z = std::atan2(matrix.m[0][1], matrix.m[1][1]);
                if (std::fabs(std::cos(x)) < 1e-6f) {
                    z += matrix.m[0][1] > 0.f ? pi : -pi;
                    y += matrix.m[2][0] > 0.f ? pi : -pi;
                }
                api.rotZ(&rotation, z * options.scale[5] + add[5]);
                api.rotX(&axisMatrix, x * options.scale[3] + add[3]);
                api.multiply(&rotation, &rotation, &axisMatrix);
                api.rotY(&axisMatrix, y * options.scale[4] + add[4]);
                api.multiply(&rotation, &rotation, &axisMatrix);
                api.quatFromMatrix(target.rotation, &rotation);
                for (float value : target.rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            }
            const bool scaleChanged = stretch != change->scaleBefore.Key(int(index));
            if (!std::memcmp(&original, &target, sizeof(target)) && !scaleChanged) continue;
            plan.Write(unsigned(index)) = target;
            if (scaleChanged && !change->scaleAfter.SetKey(int(index), stretch)) return MMS_E_ARGUMENT;
        }
        if (plan.patches.empty()) return MMS_OK;
        const bool authored = FinishKeyPlan(plan, *change, owners);
        change->publishMetadata = false; change->preserveKeyMarks = true;
        CaptureRules(*change, std::set<unsigned>(change->evaluationBones.begin(), change->evaluationBones.end()));
        if (authored) out = std::move(change);
        return MMS_OK;
    });
}
MmsResult PlanBoneCenterOffset(MMDApp& app, unsigned slot, const std::array<float, 3>& offset,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    for (float value : offset) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        // Besides avoiding needless work, this preserves negative zero bits.
        if (offset[0] == 0 && offset[1] == 0 && offset[2] == 0) return MMS_OK;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (!AnyMarks(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys))) return MMS_OK;
        KeyPlan plan(model, true);
        const auto owners = ValidateOwners(plan);
        auto change = NewKeyChange(app, slot);
        for (std::size_t index = 0; index < plan.capacity; ++index) {
            if (!model.boneKeys[index].allocated) continue;
            const auto owner = owners.find(unsigned(index));
            if (owner == owners.end()) return MMS_E_ARGUMENT;
            if (model.boneTable[owner->second].parent != -1) continue;
            auto target = model.boneKeys[index];
            for (unsigned axis = 0; axis < offset.size(); ++axis) {
                if (offset[axis] == 0) continue;
                // Keep the legacy float += rounding, without a double step.
                target.position[axis] += offset[axis];
                if (!std::isfinite(target.position[axis])) return MMS_E_ARGUMENT;
            }
            if (std::memcmp(target.position, model.boneKeys[index].position, sizeof(target.position)))
                plan.Write(unsigned(index)) = target;
        }
        if (plan.patches.empty()) return MMS_OK;
        const bool changed = FinishKeyPlan(plan, *change, owners);
        change->publishMetadata = false;
        change->preserveKeyMarks = true;
        if (changed) {
            std::shared_ptr<HistoryChange> guarded = std::move(change);
            if (auto result = WrapCenterOffsetParentDependencies(app, slot, guarded)) return result;
            out = std::move(guarded);
        }
        return MMS_OK;
    });
}
MmsResult PlanBoneFrameLine(MMDApp& app, unsigned slot, int frame, bool insert, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (frame < 0 || frame > frame_limits::maximum) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        KeyPlan plan(model, true); const auto owners = ValidateOwners(plan);
        auto change = NewKeyChange(app, slot);
        std::map<std::pair<unsigned,unsigned>,unsigned> retained;
        for(const auto& owner:owners){const auto& key=model.boneKeys[owner.first];unsigned target=key.frame;
            if(owner.first>=model.boneCount&&key.frame>=unsigned(frame)){
                if(insert){if(key.frame>=unsigned(frame_limits::maximum))return MMS_E_ARGUMENT;target=key.frame+1;}
                else {if(key.frame==unsigned(frame))continue;target=key.frame-1;}
            }
            // Removing line zero may merge the former frame-one row into its
            // root. That source row owns the retained authored endpoint too.
            retained[{owner.second,target}]=key.frame;
        }
        const auto clear = [](mdl::BoneKey& key) {
            key.frame = 0; key.allocated = 0;
            std::memset(key.position, 0, sizeof(key.position));
            key.rotation[0] = key.rotation[1] = key.rotation[2] = 0; key.rotation[3] = 1;
        };
        for (std::size_t index=model.boneCount;index<plan.capacity;++index) {
            const auto key = plan.Read(unsigned(index));
            if (!key.frame || key.frame < unsigned(frame)) continue;
            if (owners.find(unsigned(index)) == owners.end()) return MMS_E_ARGUMENT;
            if (insert) {
                if (key.frame >= unsigned(frame_limits::maximum)) return MMS_E_ARGUMENT;
                plan.Write(unsigned(index)).frame = key.frame + 1;
                change->maxAfter = (std::max)(change->maxAfter, key.frame + 1);
                continue;
            }
            if (key.frame == unsigned(frame)) {
                plan.Write(key.previous).next = key.next; plan.Write(key.next).previous = key.previous;
                change->scaleAfter.ClearKey(int(index)); clear(plan.Write(unsigned(index)));
                if (automaticInterpolation) for (int lane=0;lane<4;++lane) plan.Smooth(key.next ? key.next : key.previous,lane);
                plan.Write(unsigned(index)).next = plan.Write(unsigned(index)).previous = 0;
            } else {
                auto& target = plan.Write(unsigned(index)); target.frame = key.frame - 1;
                if (!target.frame) {
                    auto& previous = plan.Write(key.previous); previous.next = key.next;
                    std::memcpy(previous.interpolation,key.interpolation,sizeof(key.interpolation));
                    std::memcpy(previous.position,key.position,sizeof(key.position));
                    std::memcpy(previous.rotation,key.rotation,sizeof(key.rotation));
                    previous.allocated = key.allocated; previous.physicsDisabled = key.physicsDisabled;
                    std::memcpy(previous.reserved,key.reserved,sizeof(key.reserved));
                    if (!change->scaleAfter.SetKey(int(key.previous),change->scaleAfter.Key(int(index))) ||
                        !change->scaleAfter.SetKeyCurve(int(key.previous),change->scaleAfter.KeyCurve(int(index)))) return MMS_E_ARGUMENT;
                    if (key.next) plan.Write(key.next).previous = key.previous;
                    if (automaticInterpolation) for (int lane=0;lane<4;++lane) plan.Smooth(key.next ? key.next : key.previous,lane);
                    change->scaleAfter.ClearKey(int(index)); clear(target);
                }
            }
        }
        const auto frameMaps=RetainedFrameMaps(retained);
        if (FinishKeyPlan(plan,*change,owners,{},&frameMaps)) out = std::move(change);
        return MMS_OK;
    });
}

MmsResult PlanBoneKeyDeletion(MMDApp& app, unsigned slot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>& out, bool selectedAtCurrentFrame,
    bool allowOtherMarks, std::shared_ptr<DisplayKeyDeletion> display,
    const std::vector<unsigned>* selectedSlots) noexcept {
    out.reset();
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        const bool otherMarks = AnyMarks(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys)) ||
            AnyMarks(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys));
        if (otherMarks && !allowOtherMarks) return MMS_E_NOT_READY;
        const auto capacity = mdl::BoneKeyCapacity(model.boneKeys);
        if (selectedSlots && !ValidKeySelection(*selectedSlots, capacity)) return MMS_E_ARGUMENT;
        const bool explicitMarks = selectedSlots ? !selectedSlots->empty() : AnyMarks(model.boneKeys, capacity);
        const bool fallback = !selectedSlots && !explicitMarks && !otherMarks && selectedAtCurrentFrame && app.CurrentFrame() != 0;
        if (!explicitMarks && !fallback && !display) return MMS_OK;
        if (fallback) {
            if (!model.boneSelection) return MMS_E_NOT_READY;
            if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
        }
        KeyPlan plan(model, true);
        const auto owners = ValidateOwners(plan);
        std::vector<unsigned> selected;
        std::set<unsigned> rules;
        // Preserve native slot-order deletion. Keyboard fallback is a private
        // predicate; it never temporarily changes the live allocation marks.
        for (std::size_t index = 0; index < capacity; ++index) {
            const auto& key = model.boneKeys[index];
            const auto owner = owners.find(unsigned(index));
            const bool wanted = selectedSlots ? SelectedKeySlot(*selectedSlots, unsigned(index)) : explicitMarks ? key.allocated != 0 : fallback &&
                owner != owners.end() && model.boneSelection[owner->second] && key.frame == unsigned(app.CurrentFrame());
            if (!wanted) continue;
            if (owner == owners.end()) return MMS_E_ARGUMENT;
            selected.push_back(unsigned(index));
            if (index < model.boneCount) rules.insert(unsigned(index));
        }
        if (selected.empty() && !display) return MMS_OK;
        auto change = NewKeyChange(app, slot);
        DeletePlannedKeys(plan, selected, change->scaleAfter, automaticInterpolation);
        CaptureRules(*change, rules);
        const bool changed = FinishKeyPlan(plan, *change, owners);
        AttachDisplay(*change, std::move(display));
        if (changed || change->display) out = std::move(change);
        return MMS_OK;
    });
}
MmsResult PlanBoneRangeScale(MMDApp& app, unsigned slot, const BoneRangeScaleOptions& options,
    std::shared_ptr<HistoryChange>& out, RangeScaleResult* aggregate,
    std::shared_ptr<DisplayKeyDeletion> display) noexcept {
    out.reset();
    if (aggregate) *aggregate = {};
    if (options.start >= options.end || options.end > unsigned(frame_limits::maximum) ||
        !std::isfinite(options.scale) || options.scale <= 0) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        if (options.scale == 1 && !aggregate) return MMS_OK;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (aggregate) { aggregate->maxFrame = model.maxFrame; aggregate->searchCursor = model.searchCursor; }
        KeyPlan plan(model, true);
        const auto owners = ValidateOwners(plan);
        std::vector<unsigned> selected;
        std::vector<RangeKeyCopy> copies;
        std::set<unsigned> rules;
        bool moved = false;
        for (std::size_t index = 0; index < plan.capacity; ++index) {
            const auto& key = model.boneKeys[index];
            if ((!key.frame && index >= model.boneCount) || key.frame < options.start) continue;
            const auto owner = owners.find(unsigned(index));
            if (owner == owners.end()) return MMS_E_ARGUMENT;
            const auto frame = ScaledBoneFrame(key.frame, options);
            moved |= frame != key.frame;
            selected.push_back(unsigned(index));
            copies.push_back({owner->second, frame, key, bone_scale::Key(app.ModelSlot(slot), int(index)),
                bone_scale::KeyCurve(app.ModelSlot(slot),int(index))});
            rules.insert(owner->second);
        }
        // Identity after integer truncation leaves selection and redo intact.
        if (!moved && !aggregate) return MMS_OK;
        for (auto& copy : copies) if (!PrepareRangeRotation(model, copy, rules)) return MMS_E_NOT_READY;
        auto change = NewKeyChange(app, slot);
        std::map<std::pair<unsigned,unsigned>,unsigned> retained;
        for(const auto& owner:owners){const auto& key=model.boneKeys[owner.first];
            if(key.frame<options.start)retained[{owner.second,key.frame}]=key.frame;}
        for(const auto& copy:copies)retained[{copy.owner,copy.frame}]=copy.key.frame;
        const auto frameMaps=RetainedFrameMaps(retained);
        change->publishMetadata = aggregate == nullptr;
        if (!aggregate) ClearAuxiliaryMarks(*change);
        unsigned cursor = unsigned(model.searchCursor);
        if (!selected.empty()) {
            for (std::size_t index = 0; index < plan.capacity; ++index)
                if (model.boneKeys[index].allocated) plan.Write(unsigned(index), false).allocated = 0;
            DeletePlannedKeys(plan, selected, change->scaleAfter, options.automaticInterpolation);
            cursor = model.boneCount;
            while (cursor < plan.capacity && plan.Read(cursor).frame) ++cursor;
        }
        // Deliberately preserve ascending original pool slot order, not frame
        // order. When truncation collides, the last source slot wins.
        for (const auto& copy : copies) InsertRangeCopy(plan, copy, options.start, cursor, *change);
        change->searchAfter = int(cursor);
        change->lastAfter = (std::max)(change->lastBefore, change->maxAfter);
        CaptureRules(*change, rules);
        const bool changed = FinishKeyPlan(plan, *change, owners,{},&frameMaps);
        AttachDisplay(*change, std::move(display));
        if (aggregate) {
            aggregate->maxFrame = change->maxAfter; aggregate->searchCursor = change->searchAfter;
            aggregate->hasKeys = !selected.empty(); aggregate->moved = moved;
            for (std::size_t index = 0; index < plan.capacity; ++index)
                if (plan.Read(unsigned(index)).allocated) aggregate->markedSlots.push_back(unsigned(index));
        }
        if (changed || change->display) out = std::move(change);
        if (aggregate) aggregate->change = out;
        return MMS_OK;
    });
}
MmsResult PlanBoneDisplayEvaluation(MMDApp& app, unsigned slot, std::shared_ptr<DisplayKeyDeletion> display,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (!display) return MMS_OK;
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        KeyPlan plan(*mdl::Mdl(app.ModelSlot(slot)), true);
        const auto owners = ValidateOwners(plan);
        auto change = NewKeyChange(app, slot); change->publishMetadata = false;
        FinishKeyPlan(plan, *change, owners); AttachDisplay(*change, std::move(display));
        out = std::move(change); return MMS_OK;
    });
}
MmsResult PlanBoneRegistration(MMDApp& app, unsigned slot, const BoneRegistrationOptions& options,
                               std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || options.frame < 0 ||
        unsigned(options.frame) > frame_limits::maximum || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto* raw = app.ModelSlot(slot); auto& model = *mdl::Mdl(raw);
        if (!model.boneCount || !model.boneTable || !model.boneSelection || !model.bonePhysicsState ||
            model.boneCount > mdl::BoneKeyCapacity(model.boneKeys) || !mdl::BoneKeyPoolIdentity(model.boneKeys)) return MMS_E_NOT_READY;
        std::vector<unsigned> selected;
        for (unsigned bone = 0; bone < model.boneCount; ++bone) if (model.boneSelection[bone]) {
            if (model.boneTable[bone].hasRigidBody &&
                model.boneTable[bone].physicsDisabled != (options.physicsEnabled ? 0 : 1)) return MMS_E_NOT_READY;
            selected.push_back(bone);
        }
        if (selected.empty()) return MMS_OK;
        KeyPlan plan(model,true);
        for (auto bone : selected) plan.ValidateChain(bone);
        auto change = std::make_shared<RegistrationChange>(app, slot);
        change->scaleBefore = bone_scale::AuthoringState::Capture(raw); change->scaleAfter = change->scaleBefore.Clone();
        change->curveBefore = bone_curve::AuthoringState::Capture(raw); change->curveAfter = change->curveBefore->Clone();
        change->observeAuthoring=true;
        for (std::size_t n = 0; n < plan.capacity; ++n) if (model.boneKeys[n].allocated) plan.Write(unsigned(n), false).allocated = 0;
        for (std::size_t n = 0; n < change->morphCapacity; ++n)
            if (model.morphKeys[n].allocated) change->morphFlags.push_back({unsigned(n), model.morphKeys[n].allocated});
        for (std::size_t n = 0; n < change->displayCapacity; ++n)
            if (model.displayKeys[n].allocated) change->displayFlags.push_back({unsigned(n), model.displayKeys[n].allocated});
        unsigned cursor = model.boneCount;
        while (cursor < plan.capacity && plan.Read(cursor).frame) ++cursor;
        for (auto boneIndex : selected) {
            const auto& bone = model.boneTable[boneIndex];
            unsigned current = boneIndex;
            while (plan.Read(current).frame < unsigned(options.frame) && plan.Read(current).next) current = plan.Read(current).next;
            const bool fresh = plan.Read(current).frame != unsigned(options.frame);
            unsigned target = current;
            if (fresh) {
                while (std::uint64_t(cursor) < mdl::kBoneKeyCapacityLimit && plan.Read(cursor).frame) ++cursor;
                if (std::uint64_t(cursor) >= mdl::kBoneKeyCapacityLimit) return MMS_E_MEMORY;
                plan.capacity=(std::max)(plan.capacity,std::size_t(cursor)+1);
                target = cursor; auto& key = plan.Write(target); key = {};
                if (plan.Read(current).frame < unsigned(options.frame)) {
                    plan.Write(current).next = target; key.previous = current;
                } else {
                    const auto previous = plan.Read(current).previous;
                    plan.Write(previous).next = target; key.previous = previous;
                    plan.Write(current).previous = target; key.next = current;
                }
                key.frame = unsigned(options.frame);
            }
            PosePatch pose{}; pose.bone = boneIndex; pose.physics = bone.physicsDisabled;
            pose.before = pose.after = ReadPose(model, boneIndex);
            pose.sourceScale=bone_scale::Pose(raw,int(boneIndex));
            const bool physicsPose = bone.hasRigidBody && (options.physicsMode == 1 ||
                (options.physicsMode >= 2 && !bone.physicsDisabled));
            if (physicsPose) {
                std::memcpy(pose.after.position, bone.ikBackup, sizeof(pose.after.position));
                std::memcpy(pose.after.rotation, bone.ikBackup + 3, sizeof(pose.after.rotation));
            }
            RepairBonePose(pose.after.position, pose.after.rotation, bone.trans, bone.rotQuat);
            pose.after.dirty = 0;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (std::memcmp(&pose.before.position[axis], &pose.after.position[axis], sizeof(float))) pose.fields |= 1u << axis;
            if (std::memcmp(pose.before.rotation, pose.after.rotation, sizeof(pose.before.rotation))) pose.fields |= Rotation;
            if (pose.before.dirty != pose.after.dirty) pose.fields |= Dirty;
            change->poses.push_back(pose);
            auto& key = plan.Write(target);
            std::memcpy(key.position, pose.after.position, sizeof(key.position));
            std::memcpy(key.rotation, pose.after.rotation, sizeof(key.rotation));
            if (bone.hasRigidBody) key.physicsDisabled = bone.physicsDisabled;
            for (int lane = 0; lane < 4; ++lane) {
                if (options.automaticInterpolation) plan.Smooth(target, lane);
                else { key.interpolation[lane] = key.interpolation[lane + 4] = 20;
                    key.interpolation[lane + 8] = key.interpolation[lane + 12] = 107; }
            }
            if (!change->scaleAfter.RegisterKey(int(target), pose.sourceScale, fresh)) return MMS_E_ARGUMENT;
            key.allocated = 1;
        }
        CurveRegistrations registrations;for(const auto bone:selected)registrations[bone].push_back(options.frame);
        std::set<unsigned> affected(selected.begin(),selected.end());ReconcilePlannedCurves(plan,*change,affected,registrations);
        change->scaleChanged = !change->scaleBefore.Equals(change->scaleAfter);
        change->curvesChanged = !change->curveBefore->Equals(*change->curveAfter);
        change->keyDependencies.assign(plan.reads.begin(),plan.reads.end());
        for(const auto index:change->keyDependencies)change->planningDependencies.push_back(model.boneKeys[index]);
        for (auto& pair : plan.patches) if (std::memcmp(&pair.second.before, &pair.second.after, sizeof(mdl::BoneKey)))
            change->keys.push_back(pair.second);
        bool poseChanged = false; for (const auto& pose : change->poses) poseChanged |= pose.fields != 0;
        if (change->keys.empty() && change->morphFlags.empty() && change->displayFlags.empty() &&
            !poseChanged && !change->scaleChanged && !change->curvesChanged) return MMS_OK;
        change->searchAfter = int(cursor); change->maxAfter = (std::max)(change->maxBefore, unsigned(options.frame));
        change->lastAfter = (std::max)(change->lastBefore, change->maxAfter);
        out = std::move(change); return MMS_OK;
    });
}
bool TryRegisterBoundBones(MMDApp* app) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app); if (!history) return false;
    if (app->CameraMode()) return true;
    if (history->Enter() != MMS_OK) return true; history->Leave();
    Boundary([&]() -> MmsResult {
        BoneRegistrationOptions options;
        options.frame = app->CurrentFrame(); options.physicsMode = app->PlaybackPhysicsMode();
        options.automaticInterpolation = IsDlgButtonChecked(app->MainWindow(), panel::kPhysicsFrameCheckbox) == BST_CHECKED;
        options.physicsEnabled = IsDlgButtonChecked(app->MainWindow(), panel::kPhysicsCheckbox) == BST_CHECKED;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanBoneRegistration(*app, app->SelectedModelSlot(), options, change)) return result;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto result = history->PrepareNative("Register bone keys", change, transaction)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        PublishBoundPoseEdit(*app, *history);
        curve_host::NotifyModelRegistered(app, false);
        return MMS_OK;
    });
    return true;
}
}
#endif
