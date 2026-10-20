#include "native_physics_mode.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "btBulletDynamicsCommon.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace mikumikustudio::plugins {
namespace {
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool own = replay && NativeObjectPublicationAuthorized(&app);
    return (!app.SceneMutationInProgress() || own) && (!bake::UiLocked() || own) &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride) noexcept {
    bytes = stride && count > (UINT64_MAX - bytes) / stride ? UINT64_MAX : bytes + count * stride;
}
struct Range { std::uintptr_t first, last; };
Range Memory(const void* data, std::size_t count, std::size_t stride) {
    const auto first = reinterpret_cast<std::uintptr_t>(data);
    if (!data || !stride || count > (UINTPTR_MAX - first) / stride) throw std::invalid_argument("global physics storage overflow");
    return {first, first + count * stride};
}
std::vector<Range> WriteRanges(MMDApp& app) {
    std::vector<Range> writes;
    for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (const auto* model = mdl::Mdl(app.ModelSlot(slot))) {
        if (model->rigidCount) writes.push_back(Memory(model->rigidTable, model->rigidCount, sizeof(mdl::RigidRecord)));
        for (unsigned rigid = 0; rigid < model->rigidCount; ++rigid)
            if (auto* body = model->rigidTable[rigid].body) writes.push_back(Memory(body, 1, sizeof(btRigidBody)));
    }
    std::sort(writes.begin(), writes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < writes.size(); ++i)
        if (writes[i].first < writes[i - 1].last) throw std::invalid_argument("shared global physics writable storage");
    return writes;
}
// Stream protected ranges rather than retaining every display-key pointer.
// This same check is allocation-free during IsCurrent, so a newly introduced
// alias after preparation cannot turn a mode byte into an unrelated write.
bool Separate(MMDApp& app, const std::vector<Range>& writes) noexcept {
    const auto protect = [&](const void* data, std::size_t count, std::size_t stride) {
        if (!count) return true;
        const auto first = reinterpret_cast<std::uintptr_t>(data);
        if (!data || !stride || count > (UINTPTR_MAX - first) / stride) return false;
        const auto last = first + count * stride;
        const auto at = std::lower_bound(writes.begin(), writes.end(), first, [](const auto& range, auto value) { return range.last <= value; });
        return at == writes.end() || at->first >= last;
    };
    const auto optional = [&](const void* data, std::size_t count, std::size_t stride) { return !data || protect(data, count, stride); };
    if (!protect(&app.state, 1, sizeof app.state)) return false;
    for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (const auto* model = mdl::Mdl(app.ModelSlot(slot))) {
        if (!protect(model, 1, sizeof *model) || !protect(model->boneTable, model->boneCount, sizeof(mdl::BoneRecord)) ||
            !protect(model->jointTable, model->jointCount, sizeof(mdl::JointRecord)) ||
            !protect(model->morphs, model->morphCount, sizeof(mdl::MorphRecord)) ||
            !optional(model->bonePhysicsState, model->boneCount, 1) || !optional(model->boneSelection, model->boneCount, 1) ||
            !optional(model->boneKeyCursors, model->boneCount, sizeof(std::uint32_t)) || !optional(model->boneTrackActive, model->boneCount, 1) ||
            !optional(model->morphKeyCursors, model->morphCount, sizeof(std::uint32_t)) || !optional(model->morphTrackActive, model->morphCount, 1) ||
            !optional(model->boneKeyIndices, model->boneCount, sizeof(std::uint32_t)) || !optional(model->morphKeyIndices, model->morphCount, sizeof(std::uint32_t)) ||
            !optional(model->boneKeys, mdl::BoneKeyCapacity(model->boneKeys), sizeof(mdl::BoneKey)) ||
            !optional(model->morphKeys, mdl::MorphKeyCapacity(model->morphKeys), sizeof(mdl::MorphKey)) ||
            !optional(model->displayKeys, mdl::KeyPoolCapacity(model->displayKeys), sizeof(mdl::DisplayKey)) ||
            !optional(model->ikChains, model->ikChainCount, sizeof(mdl::IkChain)) ||
            !optional(model->boneOrderTable, model->boneOrderCount, sizeof(mdl::BoneOrderEntry))) return false;
        for (unsigned chain = 0; model->ikChains && chain < model->ikChainCount; ++chain)
            if (!protect(model->ikChains[chain].links, model->ikChains[chain].linkCount, sizeof(std::uint16_t))) return false;
        for (std::size_t key = 0; model->displayKeys && key < mdl::KeyPoolCapacity(model->displayKeys); ++key)
            if (!optional(model->displayKeys[key].ikStates, model->ikChainCount, 1) ||
                !optional(model->displayKeys[key].selectorStates, model->boneOrderCount, sizeof(mdl::BoneReference))) return false;
    }
    return true;
}
void ValidateStorage(MMDApp& app) {
    const auto writes = WriteRanges(app);
    if (!Separate(app, writes)) throw std::invalid_argument("aliased global physics storage");
}
struct BoneRule { unsigned char eligible, disabled; };
struct RigidRule {
    btRigidBody* body;
    int bone;
    unsigned char fixed, kinematic, initialMode;
    int initialFlags;
};
bool Disabled(int mode, unsigned char boneDisabled) noexcept { return mode == 0 || (mode >= 2 && boneDisabled != 0); }
int Flags(int current, bool disabled) noexcept {
    return disabled ? current | btCollisionObject::CF_KINEMATIC_OBJECT : current & ~btCollisionObject::CF_KINEMATIC_OBJECT;
}
unsigned char Mode(bool disabled, unsigned char kinematic) noexcept { return disabled ? 0 : kinematic ? 2 : 1; }
struct PhysicsMode final : HistoryChange, std::enable_shared_from_this<PhysicsMode> {
    struct Model {
        unsigned slot;
        unsigned char* object;
        MmsObjectRef identity;
        std::shared_ptr<RetainedSceneObject> owner;
        mdl::BoneRecord* bones;
        mdl::RigidRecord* rigids;
        unsigned boneCount, rigidCount;
        PhysicsScene* scene;
        btDiscreteDynamicsWorld* world;
        std::vector<BoneRule> boneRules;
        std::vector<RigidRule> rigidRules;
    };
    MMDApp& app;
    SharedHistory& history;
    const int before, after;
    const std::uint64_t binding;
    std::uint64_t revision = 0, serial = 1, ownerBytes = 0;
    std::vector<Model> models;
    bool applied = false, published = false;
    PhysicsMode(MMDApp& a, SharedHistory& h, int target)
        : app(a), history(h), before(a.PlaybackPhysicsMode()), after(target), binding(BoundNativePoseGeneration(&a)) {}
    bool Structure(bool initial) const noexcept {
        if (BoundNativePoseHistory(&app) != &history || (initial && BoundNativePoseGeneration(&app) != binding)) return false;
        std::size_t index = 0;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot)) {
            if (index == models.size()) return false;
            const auto& saved = models[index++];
            if (slot != saved.slot || object != saved.object || !saved.owner->Present() ||
                !SceneObjectPresent(object, saved.identity.id) || (initial && SceneObjectIdentity(object).generation != saved.identity.generation)) return false;
            const auto& live = *mdl::Mdl(object);
            if (live.boneTable != saved.bones || live.boneCount != saved.boneCount || live.rigidTable != saved.rigids ||
                live.rigidCount != saved.rigidCount || live.scenePtr != saved.scene || (saved.scene && saved.scene->world != saved.world)) return false;
            for (unsigned bone = 0; bone < saved.boneCount; ++bone)
                if (live.boneTable[bone].hasRigidBody != saved.boneRules[bone].eligible ||
                    (initial && live.boneTable[bone].physicsDisabled != saved.boneRules[bone].disabled)) return false;
            for (unsigned rigid = 0; rigid < saved.rigidCount; ++rigid) {
                const auto& rule = saved.rigidRules[rigid]; const auto& value = live.rigidTable[rigid];
                if (value.body != rule.body || value.boneIndex != rule.bone || value.staticFlag != rule.fixed || value.kinematicFlag != rule.kinematic ||
                    (initial && (value.mode != rule.initialMode || (rule.body && rule.body->getCollisionFlags() != rule.initialFlags)))) return false;
            }
        }
        return index == models.size();
    }
    bool Source(bool forward, bool initial) const noexcept {
        if (!Structure(initial) || app.PlaybackPhysicsMode() != (forward ? before : after)) return false;
        if (!initial) return true;
        std::uint64_t current = 0; return history.Revision(current) == MMS_OK && current == revision;
    }
    std::uint64_t RecordBytes() const noexcept {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*); Charge(bytes, models.capacity(), sizeof(Model));
        for (const auto& model : models) { Charge(bytes, model.boneRules.capacity(), sizeof(BoneRule)); Charge(bytes, model.rigidRules.capacity(), sizeof(RigidRule)); }
        return bytes;
    }
    struct Candidate final : PreparedHistoryChange {
        struct Rigid { mdl::RigidRecord* record; btRigidBody* body; int flags, targetFlags; unsigned char mode, targetMode; };
        struct World {
            DynamicsWorld* world;
            std::uint64_t step;
            btBroadphaseInterface* broadphase;
            std::vector<btCollisionObject*> bodies;
            std::vector<btTypedConstraint*> constraints;
            explicit World(DynamicsWorld* value) : world(value), step(value->StepSerial()), broadphase(value->getBroadphase()) {
                const auto& objects = value->getCollisionObjectArray(); bodies.reserve(objects.size());
                for (int i = 0; i < objects.size(); ++i) bodies.push_back(objects[i]);
                constraints.reserve(value->getNumConstraints());
                for (int i = 0; i < value->getNumConstraints(); ++i) constraints.push_back(value->getConstraint(i));
            }
            bool Current() const noexcept {
                if (world->StepSerial() != step || world->getBroadphase() != broadphase || world->getNumCollisionObjects() != int(bodies.size()) ||
                    world->getNumConstraints() != int(constraints.size())) return false;
                const auto& objects = world->getCollisionObjectArray();
                for (std::size_t i = 0; i < bodies.size(); ++i) if (objects[int(i)] != bodies[i]) return false;
                for (std::size_t i = 0; i < constraints.size(); ++i) if (world->getConstraint(int(i)) != constraints[i]) return false;
                return true;
            }
        };
        std::shared_ptr<PhysicsMode> source;
        const bool forward, initial;
        const std::uint64_t serial, binding;
        const int frame;
        const unsigned char reset;
        std::vector<std::uint64_t> generations;
        std::vector<std::vector<unsigned char>> disabled;
        std::vector<Rigid> rigids;
        std::vector<World> worlds;
        std::vector<Range> writes;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<PhysicsMode> value, bool direction)
            : source(std::move(value)), forward(direction), initial(!source->published), serial(source->serial),
              binding(BoundNativePoseGeneration(&source->app)), frame(source->app.CurrentFrame()), reset(source->app.PhysicsResetPending()), writes(WriteRanges(source->app)) {
            if (!Separate(source->app, writes)) throw std::invalid_argument("aliased global physics target");
            const int target = forward ? source->after : source->before;
            generations.reserve(source->models.size()); disabled.reserve(source->models.size());
            for (const auto& model : source->models) {
                generations.push_back(SceneObjectIdentity(model.object).generation); disabled.emplace_back(); auto& switches = disabled.back();
                switches.reserve(model.boneCount); for (unsigned bone = 0; bone < model.boneCount; ++bone) switches.push_back(model.bones[bone].physicsDisabled);
                for (unsigned rigid = 0; rigid < model.rigidCount; ++rigid) {
                    const auto& rule = model.rigidRules[rigid];
                    if (rule.fixed || !rule.body || rule.bone < 0 || unsigned(rule.bone) >= model.boneCount || !model.bones[rule.bone].hasRigidBody) continue;
                    const bool off = Disabled(target, switches[rule.bone]); auto& record = model.rigids[rigid]; const int flags = rule.body->getCollisionFlags();
                    rigids.push_back({&record, rule.body, flags, Flags(flags, off), record.mode, Mode(off, rule.kinematic)});
                }
                if (model.world && std::none_of(worlds.begin(), worlds.end(), [&](const auto& world) { return world.world == model.world; })) {
                    auto* world = dynamic_cast<DynamicsWorld*>(model.world); if (!world) throw std::invalid_argument("unsupported global physics world");
                    worlds.emplace_back(world);
                }
            }
        }
        bool IsCurrent() const noexcept override {
            const auto& s = *source;
            if (consumed || serial != s.serial || forward == s.applied || BoundNativePoseGeneration(&s.app) != binding || !Ready(s.app, true) ||
                !s.Source(forward, initial) || s.app.CurrentFrame() != frame || s.app.PhysicsResetPending() != reset || !Separate(s.app, writes)) return false;
            for (std::size_t i = 0; i < s.models.size(); ++i) {
                const auto& model = s.models[i];
                if (SceneObjectIdentity(model.object).generation != generations[i] || !model.owner->CoversBookkeeping(resources)) return false;
                for (unsigned bone = 0; bone < model.boneCount; ++bone) if (model.bones[bone].physicsDisabled != disabled[i][bone]) return false;
            }
            for (const auto& rigid : rigids) if (rigid.record->mode != rigid.mode || rigid.body->getCollisionFlags() != rigid.flags) return false;
            for (const auto& world : worlds) if (!world.Current()) return false;
            return true;
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate(); auto& s = *source;
            for (const auto& rigid : rigids) { rigid.body->setCollisionFlags(rigid.targetFlags); rigid.record->mode = rigid.targetMode; }
            const int target = forward ? s.after : s.before; s.app.PlaybackPhysicsMode() = target;
            // This is a consumed native request. Replaying an enabled mode
            // requests its ordinary settle pass; OFF does not cancel another
            // pending request. Simulation time/contacts/poses are never copied.
            if (target != 0 && s.before != s.after) s.app.PhysicsResetPending() = 1;
            s.applied = forward; s.published = true; ++s.serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (forward == applied || !Source(forward, !published)) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY; if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            for (const auto& model : models) if (auto result = model.owner->MeasureBookkeeping(candidate->resources)) return result;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = resource_budget::Collector::Sum(RecordBytes(), candidate->resources.Result().totalBytes);
            if (!candidate->IsCurrent()) return MMS_E_STALE; out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { const auto own=RecordBytes(); return ownerBytes>UINT64_MAX-own?UINT64_MAX:own+ownerBytes; }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
}
static MmsResult BuildPhysicsMode(MMDApp& app, int mode, std::shared_ptr<HistoryChange>& out, bool allowRepair) noexcept {
    out.reset(); if (mode < 0 || mode > 3) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (auto result = history->CheckNativeNavigation()) return result; if (auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app, false)) return MMS_E_BUSY; if (app.PlaybackPhysicsMode() < 0 || app.PlaybackPhysicsMode() > 3) return MMS_E_ARGUMENT;
        ValidateStorage(app); auto state = std::make_shared<PhysicsMode>(app, *history, mode); bool changed = state->before != mode;
        if (auto result = history->Revision(state->revision)) return result;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot)) {
            const auto& native = *mdl::Mdl(object); PhysicsMode::Model model{};
            model.slot = slot; model.object = object; model.identity = SceneObjectIdentity(object);
            model.owner = RetainSceneObject(app, MMS_OBJECT_MODEL, slot); if (!model.owner) return MMS_E_NOT_READY;
            model.bones = native.boneTable; model.rigids = native.rigidTable; model.boneCount = native.boneCount; model.rigidCount = native.rigidCount;
            model.scene = native.scenePtr; model.world = native.scenePtr ? native.scenePtr->world : nullptr;
            model.boneRules.reserve(native.boneCount); model.rigidRules.reserve(native.rigidCount);
            for (unsigned bone = 0; bone < native.boneCount; ++bone) model.boneRules.push_back({native.boneTable[bone].hasRigidBody, native.boneTable[bone].physicsDisabled});
            for (unsigned rigid = 0; rigid < native.rigidCount; ++rigid) {
                const auto& value = native.rigidTable[rigid]; auto* body = static_cast<btRigidBody*>(value.body); const int flags = body ? body->getCollisionFlags() : 0;
                model.rigidRules.push_back({body, value.boneIndex, value.staticFlag, value.kinematicFlag, value.mode, flags});
                if (value.staticFlag || !body || value.boneIndex < 0 || unsigned(value.boneIndex) >= native.boneCount || !native.boneTable[value.boneIndex].hasRigidBody) continue;
                const bool off = Disabled(mode, native.boneTable[value.boneIndex].physicsDisabled);
                changed |= flags != Flags(flags, off) || value.mode != Mode(off, value.kinematicFlag);
            }
            state->models.push_back(std::move(model));
        }
        // Reapplying the same setting repairs only transient derived state.
        // It is not an authored edit and must not create a journal node whose
        // before/after logical values are equal.
        if (!changed || (state->before == mode && !allowRepair)) return MMS_OK;
        resource_budget::Collector resources;
        for (const auto& model : state->models) if (auto result = model.owner->MeasureBookkeeping(resources)) return result;
        if (!resources.Result().Complete()) return MMS_E_NOT_READY; state->ownerBytes = resources.Result().totalBytes;
        if (!state->Source(true, true)) return MMS_E_STALE; out = std::move(state); return MMS_OK;
    });
}
MmsResult PlanPhysicsMode(MMDApp& app, int mode, std::shared_ptr<HistoryChange>& out) noexcept {
    return BuildPhysicsMode(app, mode, out, false);
}
void RefreshBoundPhysicsModeReadout(MMDApp& app) noexcept {
    const auto window = app.MainWindow(); if (!window) return; const auto menu = GetMenu(window); if (!menu) return;
    const UINT ids[] = {272, 265, 269, 270}; const int mode = app.PlaybackPhysicsMode();
    for (int i = 0; i < 4; ++i) CheckMenuItem(menu, ids[i], MF_BYCOMMAND | (mode == i ? MF_CHECKED : MF_UNCHECKED));
}
bool TrySetBoundPhysicsMode(MMDApp* app, int mode) noexcept {
    if (!app) return false; auto* history = BoundNativePoseHistory(app); if (!history) return false;
    const auto result = Boundary([&]() -> MmsResult {
        std::shared_ptr<HistoryChange> change; if (auto status = BuildPhysicsMode(*app, mode, change, true)) return status;
        if (!change) { RefreshBoundPhysicsModeReadout(*app); return MMS_OK; }
        if (app->PlaybackPhysicsMode() == mode) {
            if (auto status = history->CheckNativeNavigation()) return status;
            if (auto status = history->Enter()) return status;
            struct Unlock { SharedHistory& history; ~Unlock() { history.Leave(); } } unlock{*history};
            std::unique_ptr<PreparedHistoryChange> repair;
            if (auto status = change->Prepare(true, repair)) return status;
            if (!repair || !repair->IsCurrent()) return MMS_E_STALE;
            repair->Publish();
            // No EditStore transaction, dirty write, history revision or new
            // settle request: only repair the setting's current projection.
            RefreshBoundPhysicsModeReadout(*app); return MMS_OK;
        }
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto status = history->PrepareNative("Physics playback mode", std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return status;
        if (auto status = history->CommitNative(std::move(transaction))) return status;
        Boundary([&]() -> MmsResult { bake::ScopedUiInputGate input; PublishBoundPoseEdit(*app, *history, false); RefreshBoundPhysicsModeReadout(*app); return MMS_OK; });
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI physics playback mode refused: %d", result); return true;
}
}
#endif
