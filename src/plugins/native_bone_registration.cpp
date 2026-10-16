#include "native_bone_registration.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_evaluation.hpp"
#include "native_display_keys.hpp"
#include "native_range_scale.hpp"
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_pose_safety.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace mikudancestudio::plugins {
namespace {
struct KeyPatch { unsigned slot; mdl::BoneKey before{}, after{}; bool data = false; };
struct FlagPatch { unsigned slot; unsigned char before; };
struct PoseValue { float position[3], rotation[4]; unsigned char dirty; };
struct PosePatch { unsigned bone; PoseValue before{}, after{}; unsigned fields = 0; unsigned char physics; };
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
    std::vector<unsigned> evaluationBones;
    std::shared_ptr<DisplayKeyDeletion> display;
    std::vector<int> displaySelectors;
    std::vector<BoneRule> rules;
    std::vector<PosePatch> poses;
    std::vector<FlagPatch> morphFlags, displayFlags;
    std::unique_ptr<bone_curve::AuthoringState> curveBefore, curveAfter;
    bone_scale::AuthoringState scaleBefore, scaleAfter;
    std::uint32_t maxBefore, maxAfter, lastBefore, lastAfter;
    int searchBefore, searchAfter;
    bool scaleChanged = false, curvesChanged = false;
    bool observeAuthoring = false;
    bool publishMetadata = true;
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
        for (std::size_t index = 0; index < displaySelectors.size(); ++index)
            if (bones[index].slotIndex != displaySelectors[index]) return false;
        if (!rules.empty() && m.physicsMode != rulePhysicsMode) return false;
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
                if (patch.data) std::memcpy(&m.boneKeys[patch.slot], &target, sizeof(target));
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
            if (evaluation) evaluation->Publish();
            if (!growth->Accept()) std::terminate();
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure()) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto& m = *mdl::Mdl(model);
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
            for (auto index : keyDependencies) {
                if (index >= capacity) return MMS_E_STALE;
                candidate->dependencyKeys.push_back(m.boneKeys[index]);
            }
            for (const auto& patch : poses) {
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
        AddBytes(result, evaluationBones.capacity() * sizeof(unsigned));
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
    std::map<unsigned, KeyPatch> patches;
    bool trackReads;
    mutable std::set<unsigned> reads;
    explicit KeyPlan(const mdl::ModelRecord& m, bool track = false)
        : model(m), capacity(mdl::BoneKeyCapacity(m.boneKeys)), trackReads(track) {}
    mdl::BoneKey Read(unsigned slot) const {
        if (trackReads && slot < capacity) reads.insert(slot);
        const auto it = patches.find(slot); if (it != patches.end()) return it->second.after;
        return slot < capacity ? model.boneKeys[slot] : mdl::BoneKey{};
    }
    mdl::BoneKey& Write(unsigned slot, bool data = true) {
        auto found = patches.find(slot);
        if (found == patches.end()) {
            const auto original = slot < capacity ? model.boneKeys[slot] : mdl::BoneKey{};
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
bool FinishKeyPlan(KeyPlan& plan, RegistrationChange& change, const KeyOwners& originalOwners) {
    const auto owners = ValidateOwners(plan);
    std::set<unsigned> affected;
    const auto invalidate = [&](const KeyOwners& source, unsigned index) {
        const auto found = source.find(index);
        if (found != source.end()) { change.curveAfter->Invalidate(int(found->second)); affected.insert(found->second); }
    };
    for (const auto& item : plan.patches) {
        const auto& patch = item.second;
        // Native curve hashes exclude marks/reserved bytes, and include scale
        // matrices and scale interpolation. Invalidate only affected authors,
        // including both owners when a freed slot was reused by another bone.
        if (!SameCurveKeyData(patch.before, patch.after) ||
            change.scaleBefore.Key(int(patch.slot)) != change.scaleAfter.Key(int(patch.slot)) ||
            change.scaleBefore.KeyCurve(int(patch.slot)) != change.scaleAfter.KeyCurve(int(patch.slot))) {
            invalidate(originalOwners, patch.slot); invalidate(owners, patch.slot);
        }
        if (std::memcmp(&patch.before, &patch.after, sizeof(mdl::BoneKey))) change.keys.push_back(patch);
    }
    change.scaleChanged = !change.scaleBefore.Equals(change.scaleAfter);
    change.curvesChanged = !change.curveBefore->Equals(*change.curveAfter);
    change.keyDependencies.assign(plan.reads.begin(), plan.reads.end());
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
bool PrepareRangeRotation(const mdl::ModelRecord& model, RangeKeyCopy& copy, std::set<unsigned>& rules) {
    const auto& bone = model.boneTable[copy.owner];
    if (bone.type != mdl::BoneType::FixedAxis &&
        !(bone.type == mdl::BoneType::UnderIk && (bone.flags & mdl::kBoneFlagFixedAxis))) return true;
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
    const float dot = axis[0] * sourceAxis[0] + axis[1] * sourceAxis[1] + axis[2] * sourceAxis[2];
    const float lengthA = std::sqrt(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
    const float lengthB = std::sqrt(sourceAxis[0]*sourceAxis[0] + sourceAxis[1]*sourceAxis[1] + sourceAxis[2]*sourceAxis[2]);
    if (dot / (lengthA * lengthB) < 0) sine = -sine;
    for (int coordinate = 0; coordinate < 3; ++coordinate) rotation[coordinate] = sine * axis[coordinate];
    math.quatNormalize(rotation, rotation);
    for (auto value : copy.key.rotation) if (!std::isfinite(value)) throw std::invalid_argument("invalid fixed-axis rotation");
    return true;
}
void InsertRangeCopy(KeyPlan& plan, const RangeKeyCopy& copy, unsigned start, unsigned& cursor,
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
        auto& key = plan.Write(target);
        if (appended) { plan.Write(current).next = target; key.previous = current; }
        else {
            plan.Write(existing.previous).next = target; key.previous = existing.previous;
            plan.Write(current).previous = target; key.next = current;
        }
        key.frame = copy.frame;
    }
    if (!change.scaleAfter.RegisterKey(int(target), copy.scale, fresh)) throw std::invalid_argument("invalid keyed bone scale");
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
}
}
MmsResult PlanBoneKeyDeletion(MMDApp& app, unsigned slot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>& out, bool selectedAtCurrentFrame,
    bool allowOtherMarks, std::shared_ptr<DisplayKeyDeletion> display) noexcept {
    out.reset();
    return Boundary([&]() -> MmsResult {
        if (!ReadyForKeyPlan(app, slot)) return MMS_E_NOT_READY;
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        const bool otherMarks = AnyMarks(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys)) ||
            AnyMarks(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys));
        if (otherMarks && !allowOtherMarks) return MMS_E_NOT_READY;
        const auto capacity = mdl::BoneKeyCapacity(model.boneKeys);
        const bool explicitMarks = AnyMarks(model.boneKeys, capacity);
        const bool fallback = !explicitMarks && !otherMarks && selectedAtCurrentFrame && app.CurrentFrame() != 0;
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
            const bool wanted = explicitMarks ? key.allocated != 0 : fallback &&
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
            copies.push_back({owner->second, frame, key, bone_scale::Key(app.ModelSlot(slot), int(index))});
            rules.insert(owner->second);
        }
        // Identity after integer truncation leaves selection and redo intact.
        if (!moved && !aggregate) return MMS_OK;
        for (auto& copy : copies) if (!PrepareRangeRotation(model, copy, rules)) return MMS_E_NOT_READY;
        auto change = NewKeyChange(app, slot);
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
        const bool changed = FinishKeyPlan(plan, *change, owners);
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
        KeyPlan plan(model);
        for (auto bone : selected) plan.ValidateChain(bone);
        auto change = std::make_shared<RegistrationChange>(app, slot);
        change->scaleBefore = bone_scale::AuthoringState::Capture(raw); change->scaleAfter = change->scaleBefore.Clone();
        change->curveBefore = bone_curve::AuthoringState::Capture(raw); change->curveAfter = change->curveBefore->Clone();
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
            if (!change->scaleAfter.RegisterKey(int(target), bone_scale::Pose(raw, int(boneIndex)), fresh)) return MMS_E_ARGUMENT;
            change->curveAfter->Invalidate(int(boneIndex)); key.allocated = 1;
        }
        change->scaleChanged = !change->scaleBefore.Equals(change->scaleAfter);
        change->curvesChanged = !change->curveBefore->Equals(*change->curveAfter);
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
