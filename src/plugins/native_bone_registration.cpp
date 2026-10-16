#include "native_bone_registration.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_pose_safety.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace mikudancestudio::plugins {
namespace {
struct KeyPatch { unsigned slot; mdl::BoneKey before{}, after{}; bool data = false; };
struct FlagPatch { unsigned slot; unsigned char before; };
struct PoseValue { float position[3], rotation[4]; unsigned char dirty; };
struct PosePatch { unsigned bone; PoseValue before{}, after{}; unsigned fields = 0; unsigned char physics; };
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
    std::vector<PosePatch> poses;
    std::vector<FlagPatch> morphFlags, displayFlags;
    std::unique_ptr<bone_curve::AuthoringState> curveBefore, curveAfter;
    bone_scale::AuthoringState scaleBefore, scaleAfter;
    std::uint32_t maxBefore, maxAfter, lastBefore, lastAfter;
    int searchBefore, searchAfter;
    bool scaleChanged = false, curvesChanged = false;
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
        std::vector<mdl::BoneKey> currentKeys;
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
                m.maxFrame != currentMax || m.searchCursor != currentSearch ||
                source->app.state.lastRegisteredFrame != currentLast) return false;
            const auto capacity = mdl::BoneKeyCapacity(m.boneKeys);
            for (std::size_t n = 0; n < source->keys.size(); ++n) {
                const auto& patch = source->keys[n];
                const auto live = patch.slot < capacity ? m.boneKeys[patch.slot] : mdl::BoneKey{};
                if (patch.data ? std::memcmp(&live, &currentKeys[n], sizeof(live)) != 0 :
                    live.allocated != currentKeys[n].allocated) return false;
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
            return (!scale || scale->IsCurrent()) && (!curves || curves->IsCurrent());
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
            m.maxFrame = forward ? source->maxAfter : source->maxBefore;
            m.searchCursor = forward ? source->searchAfter : source->searchBefore;
            source->app.state.lastRegisteredFrame = forward ? source->lastAfter : source->lastBefore;
            if (scale && !scale->Publish()) std::terminate();
            if (curves && !curves->Publish()) std::terminate();
            if (!growth->Accept()) std::terminate();
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure()) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto& m = *mdl::Mdl(model);
            if (m.maxFrame != (forward ? maxBefore : maxAfter) ||
                m.searchCursor != (forward ? searchBefore : searchAfter) ||
                app.state.lastRegisteredFrame != (forward ? lastBefore : lastAfter)) return MMS_E_STALE;
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
            for (const auto& patch : poses) {
                candidate->currentPoses.push_back(ReadPose(m, patch.bone));
                candidate->currentScales.push_back(bone_scale::Pose(model, int(patch.bone)));
            }
            for (const auto& patch : morphFlags) candidate->currentMorph.push_back(m.morphKeys[patch.slot].allocated);
            for (const auto& patch : displayFlags) candidate->currentDisplay.push_back(m.displayKeys[patch.slot].allocated);
            if (scaleChanged && !bone_scale::AuthoringState::Capture(model).Equals(forward ? scaleBefore : scaleAfter)) return MMS_E_STALE;
            if (curvesChanged && !bone_curve::AuthoringState::Capture(model)->Equals(forward ? *curveBefore : *curveAfter)) return MMS_E_STALE;
            if (!mdl::BoneKeyPoolGrowth::Prepare(m, needed, candidate->growth)) return MMS_E_MEMORY;
            if (scaleChanged) candidate->scale = bone_scale::AuthoringRestore::Prepare(model,
                forward ? scaleAfter : scaleBefore, candidate->growth->CandidateCapacity());
            if (curvesChanged) candidate->curves = bone_curve::AuthoringRestore::Prepare(model, forward ? *curveAfter : *curveBefore);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t result = sizeof(*this) + 4 * sizeof(void*);
        AddBytes(result, keys.capacity() * sizeof(KeyPatch)); AddBytes(result, poses.capacity() * sizeof(PosePatch));
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
    explicit KeyPlan(const mdl::ModelRecord& m) : model(m), capacity(mdl::BoneKeyCapacity(m.boneKeys)) {}
    mdl::BoneKey Read(unsigned slot) const {
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
