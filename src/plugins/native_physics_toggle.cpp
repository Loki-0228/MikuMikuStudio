#include "native_physics_toggle.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "btBulletDynamicsCommon.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace mikudancestudio::plugins {
namespace {
using Key = mdl::BoneKey;
bool Ready(MMDApp& app) noexcept {
    return !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.SceneMutationInProgress() &&
        !bake::UiLocked() && !SceneSaveInProgress() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog && !app.CameraMode();
}
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride = 1) noexcept {
    bytes = stride && count > (UINT64_MAX - bytes) / stride ? UINT64_MAX : bytes + count * stride;
}
bool Equal(Key a, Key b, bool marks) noexcept {
    if (!marks) a.allocated = b.allocated = 0;
    return !std::memcmp(&a, &b, sizeof a);
}
std::vector<unsigned> Chain(const std::vector<Key>& pool, unsigned bone, unsigned count) {
    if (bone >= count || count > pool.size()) throw std::invalid_argument("physics bone owner");
    std::vector<unsigned> result;
    unsigned index = bone, previous = 0, frame = 0;
    for (std::size_t hop = 0; hop < pool.size(); ++hop) {
        if (index >= pool.size() || (hop && index < count)) throw std::invalid_argument("physics bone slot");
        const auto& key = pool[index];
        if ((!hop && key.frame) || (hop && (key.previous != previous || key.frame <= frame)) ||
            key.frame > unsigned(frame_limits::maximum) || key.physicsDisabled > 1)
            throw std::invalid_argument("physics bone chain");
        double norm = 0;
        for (float value : key.position) if (!std::isfinite(value)) throw std::invalid_argument("physics bone position");
        for (float value : key.rotation) { if (!std::isfinite(value)) throw std::invalid_argument("physics bone rotation"); norm += double(value) * value; }
        if (norm < 1e-20) throw std::invalid_argument("physics bone quaternion");
        for (auto value : key.interpolation) if (value > 127) throw std::invalid_argument("physics bone interpolation");
        if (hop) {
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(key.position[axis] - pool[previous].position[axis]))
                    throw std::invalid_argument("physics bone position difference overflow");
            const auto& lower = pool[previous];
            const float dot = lower.rotation[0] * key.rotation[0] + lower.rotation[1] * key.rotation[1] +
                lower.rotation[2] * key.rotation[2] + lower.rotation[3] * key.rotation[3];
            if (!std::isfinite(dot)) throw std::invalid_argument("physics quaternion dot overflow");
        }
        result.push_back(index);
        if (!key.next) return result;
        previous = index; frame = key.frame; index = key.next;
    }
    throw std::invalid_argument("physics bone cycle");
}
void ValidateOwners(const std::vector<Key>& keys, unsigned count) {
    std::vector<unsigned char> owners(keys.size());
    for (unsigned bone = 0; bone < count; ++bone) for (auto index : Chain(keys, bone, count))
        if (owners[index]++) throw std::invalid_argument("shared physics bone key");
}
// The command writes native keys, bone runtime fields, rigid-mode bytes and
// Bullet collision flags. Reject storage aliases before any of these writes.
void ValidateStorage(MMDApp& app, unsigned slot) {
    const auto& model = *mdl::Mdl(app.ModelSlot(slot));
    struct Range { std::uintptr_t first, last; };
    std::vector<Range> ranges;
    const auto range = [](const void* pointer, std::size_t count, std::size_t size) {
        const auto first = reinterpret_cast<std::uintptr_t>(pointer);
        if (count > (UINTPTR_MAX - first) / size) throw std::invalid_argument("physics storage overflow");
        return Range{first, first + count * size};
    };
    const auto add = [&](const void* p, std::size_t count, std::size_t size) {
        if (p && count) ranges.push_back(range(p, count, size));
    };
    add(&app.state, 1, sizeof app.state); add(&model, 1, sizeof model);
    add(model.boneTable, model.boneCount, sizeof(mdl::BoneRecord));
    add(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys), sizeof(Key));
    add(model.bonePhysicsState, model.boneCount, 1);
    add(model.rigidTable, model.rigidCount, sizeof(mdl::RigidRecord));
    add(model.morphs, model.morphCount, sizeof(mdl::MorphRecord));
    add(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys), sizeof(mdl::MorphKey));
    add(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys), sizeof(mdl::DisplayKey));
    add(model.boneOrderTable, model.boneOrderCount, sizeof(mdl::BoneOrderEntry));
    add(model.ikChains, model.ikChainCount, sizeof(mdl::IkChain));
    add(model.boneSelection, model.boneCount, 1);
    add(model.boneKeyCursors, model.boneCount, sizeof(std::uint32_t));
    add(model.boneTrackActive, model.boneCount, 1);
    add(model.morphKeyCursors, model.morphCount, sizeof(std::uint32_t));
    add(model.morphTrackActive, model.morphCount, 1);
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].last) throw std::invalid_argument("aliased physics storage");
    const auto peer = [&](const void* p, std::size_t count, std::size_t size) {
        if (!p || !count) return;
        const auto value = range(p, count, size);
        const auto at = std::lower_bound(ranges.begin(), ranges.end(), value.first,
            [](const Range& current, auto first) { return current.last <= first; });
        if (at != ranges.end() && at->first < value.last) throw std::invalid_argument("aliased peer physics storage");
    };
    std::set<void*> bodies;
    for (unsigned rigid = 0; rigid < model.rigidCount; ++rigid)
        if (model.rigidTable[rigid].body && !bodies.insert(model.rigidTable[rigid].body).second)
            throw std::invalid_argument("shared rigid body");
    for (unsigned otherSlot = 0; otherSlot < kModelSlotCount; ++otherSlot) if (otherSlot != slot && app.ModelSlot(otherSlot)) {
        const auto& other = *mdl::Mdl(app.ModelSlot(otherSlot));
        peer(&other, 1, sizeof other); peer(other.boneTable, other.boneCount, sizeof(mdl::BoneRecord));
        peer(other.boneKeys, mdl::BoneKeyCapacity(other.boneKeys), sizeof(Key));
        peer(other.bonePhysicsState, other.boneCount, 1); peer(other.boneSelection, other.boneCount, 1);
        peer(other.rigidTable, other.rigidCount, sizeof(mdl::RigidRecord));
        peer(other.morphs, other.morphCount, sizeof(mdl::MorphRecord));
        peer(other.morphKeys, mdl::MorphKeyCapacity(other.morphKeys), sizeof(mdl::MorphKey));
        peer(other.displayKeys, mdl::KeyPoolCapacity(other.displayKeys), sizeof(mdl::DisplayKey));
        peer(other.boneOrderTable, other.boneOrderCount, sizeof(mdl::BoneOrderEntry));
        peer(other.ikChains, other.ikChainCount, sizeof(mdl::IkChain));
        peer(other.boneKeyCursors, other.boneCount, sizeof(std::uint32_t)); peer(other.boneTrackActive, other.boneCount, 1);
        peer(other.morphKeyCursors, other.morphCount, sizeof(std::uint32_t)); peer(other.morphTrackActive, other.morphCount, 1);
        if (other.rigidCount && !other.rigidTable) throw std::invalid_argument("peer rigid table");
        for (unsigned rigid = 0; rigid < other.rigidCount; ++rigid)
            if (other.rigidTable[rigid].body && bodies.count(other.rigidTable[rigid].body))
                throw std::invalid_argument("aliased peer rigid body");
    }
}
struct TargetView {
    const std::vector<Key>& keys;
    const bone_scale::AuthoringState& scale;
    bone_curve::NativeKeyView View() const noexcept {
        return {this, keys.size(), [](const void* p, std::uint32_t index) noexcept -> const Key* {
            const auto& keys = static_cast<const TargetView*>(p)->keys;
            return index < keys.size() ? &keys[index] : nullptr;
        }, [](const void* p, std::uint32_t index, bone_scale::Matrix& value, bone_scale::Curve& curve) noexcept {
            const auto& source = *static_cast<const TargetView*>(p);
            if (index >= source.keys.size()) return false;
            value = source.scale.Key(int(index)); curve = source.scale.KeyCurve(int(index)); return bone_scale::Valid(value);
        }};
    }
};
std::uint64_t Hash(const std::vector<Key>& keys, const std::vector<unsigned>& chain,
    const bone_scale::AuthoringState& scale) noexcept {
    std::uint64_t value = 14695981039346656037ull;
    const auto add = [&](const void* p, std::size_t count) {
        const auto* bytes = static_cast<const unsigned char*>(p);
        while (count--) { value ^= *bytes++; value *= 1099511628211ull; }
    };
    for (auto index : chain) {
        auto key = keys[index]; key.allocated = 0; key.reserved[0] = key.reserved[1] = 0;
        add(&key, sizeof key); const auto matrix = scale.Key(int(index)); const auto curve = scale.KeyCurve(int(index));
        add(matrix.v.data(), sizeof(float) * 9); add(curve.data(), curve.size());
    }
    return value;
}
struct BoneValue {
    unsigned index;
    unsigned char dirty;
    mdl::BoneRecord before, after;
    bool parent = false;
    mdl::BoneOrderEntry selector{};
};
// Reuse the native seek's exact-hit, terminal, interpolation and mode-3
// branches. The private model has no rigids, morphs, IK chains or writable
// selectors; its one selector is an immutable copy of the live window.
void Sample(BoneValue& value, const std::vector<Key>& keys, unsigned boneCount,
    int frame, int mode, const bone_scale::AuthoringState& scale,
    const bone_curve::AuthoringState& curves, bone_scale::Values& poses) {
    const auto chain = Chain(keys, value.index, boneCount);
    auto scratch = std::make_unique<mdl::ModelRecord>();
    std::vector<Key> privateKeys; privateKeys.reserve(chain.size());
    for (std::size_t i = 0; i < chain.size(); ++i) {
        auto key = keys[chain[i]]; key.previous = i ? unsigned(i - 1) : 0;
        key.next = i + 1 < chain.size() ? unsigned(i + 1) : 0; privateKeys.push_back(key);
    }
    auto bone = value.before;
    bone.slotIndex = value.parent ? 0 : -1;
    mdl::DisplayKey display{}; display.visible = 1;
    scratch->boneCount = 1; scratch->boneTable = &bone; scratch->boneKeys = privateKeys.data();
    scratch->displayKeys = &display;
    // Count zero keeps RefreshIkSelectors from authoring this copied window.
    scratch->boneOrderTable = value.parent ? &value.selector : nullptr;
    SeekModelFrame(reinterpret_cast<unsigned char*>(scratch.get()), frame, mode);
    value.after = bone; value.after.slotIndex = value.before.slotIndex;
    // Unregistered user poses win over track evaluation. Solver switches and
    // mode-3 working/arm metadata still reflect the target physics track.
    if (value.dirty) {
        std::memcpy(value.after.trans, value.before.trans, sizeof value.after.trans);
        std::memcpy(value.after.rotQuat, value.before.rotQuat, sizeof value.after.rotQuat);
        return;
    }
    std::size_t lower = 0;
    while (lower + 1 < chain.size() && keys[chain[lower + 1]].frame <= unsigned(frame)) ++lower;
    auto matrix = scale.Key(int(chain[lower]));
    if (lower + 1 < chain.size()) {
        const auto& a = keys[chain[lower]]; const auto& b = keys[chain[lower + 1]];
        const auto time = float(std::clamp((double(frame) - a.frame) / double(b.frame - a.frame), 0.0, 1.0));
        matrix = bone_scale::Interpolate(matrix, scale.Key(int(chain[lower + 1])),
            bone_scale::CurveTime(scale.KeyCurve(int(chain[lower + 1])), time));
    }
    curves.Sample(int(value.index), frame, Hash(keys, chain, scale), value.after.trans, value.after.rotQuat, matrix);
    for (float component : value.after.trans) if (!std::isfinite(component)) throw std::invalid_argument("physics sampled position");
    for (float component : value.after.rotQuat) if (!std::isfinite(component)) throw std::invalid_argument("physics sampled rotation");
    if (!bone_scale::Valid(matrix)) throw std::invalid_argument("physics sampled scale");
    if (bone_scale::Identity(matrix)) poses.erase(int(value.index)); else poses[int(value.index)] = matrix;
}
struct PhysicsToggle final : HistoryChange, std::enable_shared_from_this<PhysicsToggle> {
    struct RigidRule { void* body; int bone; unsigned char fixed, kinematic; };
    MMDApp& app;
    unsigned slot, count, rigidCount;
    unsigned char* model;
    mdl::BoneRecord* bones;
    unsigned char* dirty;
    mdl::RigidRecord* rigids;
    std::shared_ptr<RetainedSceneObject> owner;
    std::uint64_t identity, generation = 1;
    std::uint64_t initialOwnerBytes = 0;
    bool applied = false, published = false;
    std::vector<Key> before, after;
    std::vector<unsigned> affected, patches;
    std::vector<unsigned char> eligibility;
    std::vector<RigidRule> rigidRules;
    bone_scale::AuthoringState scale;
    std::unique_ptr<bone_curve::AuthoringState> curvesBefore, curvesAfter;
    PhysicsToggle(MMDApp& a, unsigned s) : app(a), slot(s), count(mdl::Mdl(a.ModelSlot(s))->boneCount),
        rigidCount(mdl::Mdl(a.ModelSlot(s))->rigidCount), model(a.ModelSlot(s)), bones(mdl::Mdl(model)->boneTable),
        dirty(mdl::Mdl(model)->bonePhysicsState), rigids(mdl::Mdl(model)->rigidTable), owner(RetainSceneObject(a, MMS_OBJECT_MODEL, s)),
        identity(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),
        before(mdl::Mdl(model)->boneKeys, mdl::Mdl(model)->boneKeys + mdl::BoneKeyCapacity(mdl::Mdl(model)->boneKeys)), after(before),
        scale(bone_scale::AuthoringState::Capture(model)), curvesBefore(bone_curve::AuthoringState::Capture(model)), curvesAfter(curvesBefore->Clone()) {
        rigidRules.reserve(rigidCount);
        for (unsigned i = 0; i < rigidCount; ++i) rigidRules.push_back({rigids[i].body, rigids[i].boneIndex, rigids[i].staticFlag, rigids[i].kinematicFlag});
        eligibility.reserve(count);
        for (unsigned i = 0; i < count; ++i) eligibility.push_back(bones[i].hasRigidBody);
    }
    bool Structure() const noexcept {
        if (!owner || !owner->Present() || app.ModelSlot(slot) != model || !SceneObjectPresent(model, owner->Id())) return false;
        const auto& value = *mdl::Mdl(model);
        const bool same = value.boneTable == bones && value.boneCount == count && value.bonePhysicsState == dirty &&
            value.rigidCount == rigidCount && value.rigidTable == rigids && mdl::BoneKeyPoolIdentity(value.boneKeys) == identity &&
            mdl::BoneKeyCapacity(value.boneKeys) >= before.size();
        if (!same) return false;
        for (unsigned i = 0; i < count; ++i) if (bones[i].hasRigidBody != eligibility[i]) return false;
        for (unsigned i = 0; i < rigidCount; ++i) if (rigids[i].body != rigidRules[i].body || rigids[i].boneIndex != rigidRules[i].bone ||
            rigids[i].staticFlag != rigidRules[i].fixed || rigids[i].kinematicFlag != rigidRules[i].kinematic) return false;
        return true;
    }
    struct RigidValue { unsigned index; mdl::RigidRecord before; btRigidBody* body; int flags, targetFlags; unsigned char targetMode; };
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<PhysicsToggle> source;
        bool forward, consumed = false;
        std::uint64_t generation;
        int frame, mode;
        Key* pool;
        std::vector<Key> current, target;
        std::vector<BoneValue> bones;
        std::vector<RigidValue> rigids;
        bone_scale::Values poseBefore, poseAfter;
        std::unique_ptr<bone_scale::AuthoringRestore> scaleRestore;
        std::unique_ptr<bone_curve::AuthoringRestore> curveRestore;
        mutable resource_budget::Collector resources;
        std::uint64_t targetBytes = 0;
        Candidate(std::shared_ptr<PhysicsToggle> value, bool f) : source(std::move(value)), forward(f), generation(source->generation),
            frame(source->app.CurrentFrame()), mode(source->app.PlaybackPhysicsMode()), pool(mdl::Mdl(source->model)->boneKeys),
            current(pool, pool + mdl::BoneKeyCapacity(pool)), target(current),
            poseBefore(bone_scale::PoseValues(source->model)), poseAfter(poseBefore) {}
        bool IsCurrent() const noexcept override {
            const auto& s = *source;
            if (consumed || generation != s.generation || forward == s.applied || !s.Structure() ||
                s.app.CurrentFrame() != frame || s.app.PlaybackPhysicsMode() != mode || mdl::Mdl(s.model)->boneKeys != pool ||
                mdl::BoneKeyCapacity(pool) != current.size() || !bone_scale::PoseValuesEqual(s.model, poseBefore) ||
                !scaleRestore || !scaleRestore->IsCurrent() || !curveRestore || !curveRestore->IsCurrent() ||
                !s.owner->CoversBookkeeping(resources)) return false;
            if (std::memcmp(pool, current.data(), current.size() * sizeof(Key))) return false;
            for (const auto& bone : bones) {
                if (s.dirty[bone.index] != bone.dirty || std::memcmp(&s.bones[bone.index], &bone.before, sizeof bone.before)) return false;
                if (bone.parent) {
                    const auto& m = *mdl::Mdl(s.model);
                    if (!m.boneOrderTable || bone.before.slotIndex < 0 || unsigned(bone.before.slotIndex) >= m.boneOrderCount ||
                        std::memcmp(&static_cast<const mdl::BoneOrderEntry*>(m.boneOrderTable)[bone.before.slotIndex], &bone.selector, sizeof bone.selector)) return false;
                }
            }
            for (const auto& rigid : rigids)
                if (std::memcmp(&s.rigids[rigid.index], &rigid.before, sizeof rigid.before) || rigid.body->getCollisionFlags() != rigid.flags) return false;
            return true;
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& s = *source;
            if (!scaleRestore->Publish() || !curveRestore->Publish() || !bone_scale::ExchangePoseValues(s.model, poseAfter)) std::terminate();
            for (auto index : s.patches) pool[index].physicsDisabled = target[index].physicsDisabled;
            for (const auto& value : bones) {
                auto& bone = s.bones[value.index]; const auto& after = value.after;
                std::memcpy(bone.trans, after.trans, sizeof bone.trans); std::memcpy(bone.rotQuat, after.rotQuat, sizeof bone.rotQuat);
                bone.physicsDisabled = after.physicsDisabled; bone.rigidIdx = after.rigidIdx;
                std::memcpy(bone.ikWorkingPos, after.ikWorkingPos, sizeof bone.ikWorkingPos);
                std::memcpy(bone.ikWorkingQuat, after.ikWorkingQuat, sizeof bone.ikWorkingQuat);
            }
            for (const auto& rigid : rigids) {
                rigid.body->setCollisionFlags(rigid.targetFlags); s.rigids[rigid.index].mode = rigid.targetMode;
            }
            s.applied = forward; s.published = true; ++s.generation; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& bytes) const noexcept override { bytes = targetBytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!Structure() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum || app.PlaybackPhysicsMode() < 0 || app.PlaybackPhysicsMode() > 3)
                return MMS_E_ARGUMENT;
            ValidateStorage(app, slot);
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            ValidateOwners(candidate->current, count);
            const auto& expected = forward ? before : after;
            const auto& target = forward ? after : before;
            for (auto bone : affected) {
                if (!bones[bone].hasRigidBody) return MMS_E_STALE;
                for (auto index : Chain(expected, bone, count))
                    if (!Equal(candidate->current[index], expected[index], !published)) return MMS_E_STALE;
            }
            if (!bone_scale::AuthoringState::Capture(model).Equals(scale) ||
                !bone_curve::AuthoringState::Capture(model)->Equals(forward ? *curvesBefore : *curvesAfter)) return MMS_E_STALE;
            for (auto index : patches) candidate->target[index].physicsDisabled = target[index].physicsDisabled;
            candidate->scaleRestore = bone_scale::AuthoringRestore::Prepare(model, scale, candidate->target.size());
            candidate->curveRestore = bone_curve::AuthoringRestore::Prepare(model, forward ? *curvesAfter : *curvesBefore);
            for (auto index : affected) {
                BoneValue bone{index, dirty[index], bones[index], bones[index]};
                if (bone.before.physicsDisabled > 1) return MMS_E_ARGUMENT;
                if (bone.before.slotIndex >= 0) {
                    const auto& record = *mdl::Mdl(model);
                    if (!record.boneOrderTable || unsigned(bone.before.slotIndex) >= record.boneOrderCount) return MMS_E_ARGUMENT;
                    bone.parent = true; bone.selector = static_cast<const mdl::BoneOrderEntry*>(record.boneOrderTable)[bone.before.slotIndex];
                }
                for (float value : bone.before.ikBackup) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
                for (float value : bone.before.ikWorkingPos) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
                for (float value : bone.before.ikWorkingQuat) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
                Sample(bone, candidate->target, count, candidate->frame, candidate->mode, scale,
                    forward ? *curvesAfter : *curvesBefore, candidate->poseAfter);
                if (bone.before.physicsDisabled != bone.after.physicsDisabled) for (unsigned i = 0; i < rigidCount; ++i) {
                    const auto& rigid = rigids[i];
                    if (rigid.staticFlag || rigid.boneIndex != int(index) || !rigid.body) continue;
                    auto* body = static_cast<btRigidBody*>(rigid.body); const int flags = body->getCollisionFlags();
                    const int targetFlags = bone.after.physicsDisabled ? flags | btCollisionObject::CF_KINEMATIC_OBJECT : flags & ~btCollisionObject::CF_KINEMATIC_OBJECT;
                    const auto targetMode = static_cast<unsigned char>(bone.after.physicsDisabled ? 0 : rigid.kinematicFlag ? 2 : 1);
                    candidate->rigids.push_back({i, rigid, body, flags, targetFlags, targetMode});
                }
                candidate->bones.push_back(bone);
            }
            if (!candidate->poseAfter.empty() && !bone_scale::HasPoseStorage(model)) return MMS_E_NOT_READY;
            if (const auto result = owner->MeasureBookkeeping(candidate->resources)) return result;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->targetBytes = resource_budget::Collector::Sum(PayloadBytes(), candidate->resources.Result().totalBytes);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t PayloadBytes() const noexcept {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        Charge(bytes, before.capacity() + after.capacity(), sizeof(Key)); Charge(bytes, affected.capacity() + patches.capacity(), sizeof(unsigned));
        Charge(bytes, rigidRules.capacity(), sizeof(RigidRule));
        Charge(bytes, eligibility.capacity());
        Charge(bytes, scale.RetainedBytes()); Charge(bytes, curvesBefore->RetainedBytes()); Charge(bytes, curvesAfter->RetainedBytes()); return bytes;
    }
    std::uint64_t RetainedBytes() const noexcept override {
        auto bytes = PayloadBytes(); Charge(bytes, initialOwnerBytes); return bytes;
    }
    // The native command changes kinematic bits in place. A global reset
    // would additionally disturb unrelated models and is not part of it.
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
}
MmsResult PlanPhysicsKeyToggle(MMDApp& app, unsigned slot, bool disabled, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot)) return MMS_E_ARGUMENT;
    if (!Ready(app)) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (!model.boneCount || !model.boneTable || !model.bonePhysicsState || !mdl::BoneKeyPoolIdentity(model.boneKeys) ||
            model.boneCount > mdl::BoneKeyCapacity(model.boneKeys) || (model.rigidCount && !model.rigidTable)) return MMS_E_NOT_READY;
        ValidateStorage(app, slot);
        auto change = std::make_shared<PhysicsToggle>(app, slot); ValidateOwners(change->before, change->count);
        const unsigned char targetDisabled = disabled ? 1u : 0u;
        for (unsigned bone = 0; bone < model.boneCount; ++bone) if (model.boneTable[bone].hasRigidBody) {
            bool affected = false;
            for (auto index : Chain(change->before, bone, change->count)) if (change->before[index].allocated && change->before[index].physicsDisabled != targetDisabled) {
                change->after[index].physicsDisabled = targetDisabled; change->patches.push_back(index); affected = true;
            }
            if (affected) change->affected.push_back(bone);
        }
        if (change->patches.empty()) return MMS_OK;
        TargetView view{change->after, change->scale};
        for (auto bone : change->affected) change->curvesAfter->ReconcileTarget(int(bone), view.View());
        resource_budget::Collector initialOwner;
        if (const auto result = change->owner->MeasureBookkeeping(initialOwner)) return result;
        if (!initialOwner.Result().Complete()) return MMS_E_NOT_READY;
        change->initialOwnerBytes = initialOwner.Result().totalBytes;
        out = std::move(change); return MMS_OK;
    });
}
bool TryApplyBoundPhysicsKeyToggle(MMDApp* app, bool disabled) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app); if (!history) return false;
    Boundary([&]() -> MmsResult {
        if (const auto result = history->CheckNativeNavigation()) return result;
        if (const auto result = history->CheckNativeEdit()) return result;
        std::shared_ptr<HistoryChange> change;
        if (const auto result = PlanPhysicsKeyToggle(*app, app->SelectedModelSlot(), disabled, change)) return result;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (const auto result = history->PrepareNative("Change physics key markers", change, transaction, SharedHistory::NativeIntent::ExplicitCommit)) return result;
        if (const auto result = history->CommitNative(std::move(transaction))) return result;
        bake::ScopedUiInputGate input;
        PublishBoundPoseEdit(*app, *history, false); return MMS_OK;
    });
    return true;
}
}
#endif
