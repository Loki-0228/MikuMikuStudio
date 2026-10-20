#include "native_floor_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/history_physics.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "btBulletDynamicsCommon.h"
#include <cstdint>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool own = replay && NativeObjectPublicationAuthorized(&app);
    return (!app.SceneMutationInProgress() || own) && (!bake::UiLocked() || own) &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app);
}
int EnabledFlags(bool enabled) noexcept { return enabled ? int(btCollisionObject::CF_STATIC_OBJECT) : -1; }
// Establish membership through the current world's live pointer array before
// dereferencing a remembered body. A removed/freed historical pointer is never
// used to query its broadphase handle, shape, or flags.
bool Member(const btDiscreteDynamicsWorld& world, const btRigidBody* body) noexcept {
    unsigned count = 0;
    const auto& objects = world.getCollisionObjectArray();
    for (int i = 0; i < objects.size(); ++i) if (objects[i] == body) ++count;
    return count == 1;
}
struct FloorChange final : HistoryChange, std::enable_shared_from_this<FloorChange> {
    MMDApp& app;
    SharedHistory& history;
    const std::uint64_t sceneGeneration, pool, initialBinding;
    PhysicsScene* const physics;
    DynamicsWorld* const world;
    btRigidBody* const ground;
    history_physics::Broadphase* const broadphase;
    btDispatcher* const dispatcher;
    btBroadphaseProxy* const proxy;
    const int proxyId;
    btBroadphaseProxy* const rayProxy;
    const int rayId;
    btCollisionShape* const shape;
    btMotionState* const motion;
    const short group, mask;
    const unsigned char before, after;
    const int beforeFlags, afterFlags;
    std::uint64_t initialRevision = 0, serial = 1;
    bool applied = false, published = false;
    FloorChange(MMDApp& a, SharedHistory& h, DynamicsWorld& w, history_physics::Broadphase& sweep, bool enabled)
        : app(a), history(h), sceneGeneration(a.SceneGeneration()), pool(mdl::KeyPoolIdentity(a.CameraKeys())),
          initialBinding(BoundNativePoseGeneration(&a)), physics(a.Physics()), world(&w), ground(physics->groundBody),
          broadphase(&sweep), dispatcher(w.getDispatcher()), proxy(ground->getBroadphaseHandle()),
          proxyId(proxy->m_uniqueId), rayProxy(history_physics::Broadphase::RayProxy(proxy)), rayId(rayProxy->m_uniqueId),
          shape(ground->getCollisionShape()), motion(ground->getMotionState()),
          group(proxy->m_collisionFilterGroup), mask(proxy->m_collisionFilterMask), before(a.state.floorVisible),
          after((before != 0) == enabled ? before : static_cast<unsigned char>(enabled ? 1 : 0)),
          beforeFlags(ground->getCollisionFlags()), afterFlags(EnabledFlags(enabled)) {}
    bool Structure(bool initial) const noexcept {
        if (BoundNativePoseHistory(&app) != &history || app.SceneGeneration() != sceneGeneration ||
            !pool || mdl::KeyPoolIdentity(app.CameraKeys()) != pool || app.Physics() != physics ||
            (initial && BoundNativePoseGeneration(&app) != initialBinding)) return false;
        if (physics->world != world || physics->groundBody != ground || world->getBroadphase() != broadphase ||
            world->getDispatcher() != dispatcher || !Member(*world, ground)) return false;
        // Only the current, still-member object can be read beyond this point.
        const auto* liveProxy = ground->getBroadphaseHandle();
        if (liveProxy != proxy || !liveProxy || liveProxy->m_uniqueId != proxyId || liveProxy->m_clientObject != ground ||
            liveProxy->m_collisionFilterGroup != group || liveProxy->m_collisionFilterMask != mask ||
            ground->getCollisionShape() != shape || ground->getMotionState() != motion || ground->getInvMass() != 0) return false;
        // Sweep handles/UIDs are reused on removal. The native ray DBVT has an
        // independent generated UID, so reconstructing a body in the same
        // allocation and sweep slot cannot revive a historical ground source.
        const auto* liveRay = history_physics::Broadphase::RayProxy(ground->getBroadphaseHandle());
        if (liveRay != rayProxy || !liveRay || liveRay->m_uniqueId != rayId || liveRay->m_clientObject != ground) return false;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (const auto* model = mdl::Mdl(app.ModelSlot(slot))) {
            if (model->rigidCount && !model->rigidTable) return false;
            for (unsigned i = 0; i < model->rigidCount; ++i) if (model->rigidTable[i].body == ground) return false;
        }
        return true;
    }
    bool InitialCurrent() const noexcept {
        std::uint64_t revision = 0;
        return Structure(true) && history.Revision(revision) == MMS_OK && revision == initialRevision &&
            app.state.floorVisible == before && ground->getCollisionFlags() == beforeFlags;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<FloorChange> source;
        const bool forward;
        const std::uint64_t serial, binding, step;
        const int frame;
        const unsigned char expected;
        const int expectedFlags;
        std::vector<btCollisionObject*> bodies;
        std::vector<btTypedConstraint*> constraints;
        bool consumed = false;
        Candidate(std::shared_ptr<FloorChange> state, bool direction)
            : source(std::move(state)), forward(direction), serial(source->serial), binding(BoundNativePoseGeneration(&source->app)),
              step(source->world->StepSerial()), frame(source->app.CurrentFrame()), expected(source->app.state.floorVisible),
              expectedFlags(source->ground->getCollisionFlags()) {
            const auto& objects = source->world->getCollisionObjectArray(); bodies.reserve(objects.size());
            for (int i = 0; i < objects.size(); ++i) bodies.push_back(objects[i]);
            constraints.reserve(source->world->getNumConstraints());
            for (int i = 0; i < source->world->getNumConstraints(); ++i) constraints.push_back(source->world->getConstraint(i));
        }
        bool IsCurrent() const noexcept override {
            const auto& s = *source;
            if (consumed || serial != s.serial || forward == s.applied || !s.Structure(!s.published) || !Ready(s.app, true) ||
                BoundNativePoseGeneration(&s.app) != binding || s.app.CurrentFrame() != frame || s.world->StepSerial() != step ||
                s.app.state.floorVisible != expected || s.ground->getCollisionFlags() != expectedFlags ||
                s.world->getNumCollisionObjects() != int(bodies.size()) || s.world->getNumConstraints() != int(constraints.size())) return false;
            const auto& objects = s.world->getCollisionObjectArray();
            for (std::size_t i = 0; i < bodies.size(); ++i) if (objects[int(i)] != bodies[i]) return false;
            for (std::size_t i = 0; i < constraints.size(); ++i) if (s.world->getConstraint(int(i)) != constraints[i]) return false;
            return s.published || s.InitialCurrent();
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& s = *source;
            // Bullet 2.75 setCollisionFlags is a single integer assignment.
            // Do not reinsert bodies, clean pair caches, activate peers or step.
            s.ground->setCollisionFlags(forward ? s.afterFlags : s.beforeFlags);
            s.app.state.floorVisible = forward ? s.after : s.before;
            s.applied = forward; s.published = true; ++s.serial; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (forward == applied || !Structure(!published) || (!published && !InitialCurrent())) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    // The live app owns the resident world and ground. A record retains only
    // these scalar source descriptors, not a second Bullet resource bundle.
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
}
MmsResult PlanFloor(MMDApp& app, bool enabled, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (auto r = history->CheckNativeNavigation()) return r;
        if (auto r = history->CheckNativeEdit()) return r;
        if (!Ready(app, false)) return MMS_E_BUSY;
        auto* physics = app.Physics();
        if (!physics || !physics->world || !physics->groundBody || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        auto* world = dynamic_cast<DynamicsWorld*>(physics->world);
        if (!world || !Member(*world, physics->groundBody)) return MMS_E_NOT_READY;
        auto* sweep = dynamic_cast<history_physics::Broadphase*>(world->getBroadphase());
        if (!sweep) return MMS_E_NOT_READY;
        auto* ground = physics->groundBody; const auto* proxy = ground->getBroadphaseHandle();
        if (!proxy || proxy->m_clientObject != ground || !ground->getCollisionShape() || ground->getInvMass() != 0) return MMS_E_NOT_READY;
        const auto* ray = history_physics::Broadphase::RayProxy(ground->getBroadphaseHandle());
        if (!ray || ray->m_clientObject != ground) return MMS_E_NOT_READY;
        auto change = std::make_shared<FloorChange>(app, *history, *world, *sweep, enabled);
        if (auto r = history->Revision(change->initialRevision)) return r;
        if (!change->InitialCurrent()) return MMS_E_STALE;
        if (change->before == change->after && change->beforeFlags == change->afterFlags) return MMS_OK;
        out = std::move(change); return MMS_OK;
    });
}
void RefreshBoundFloorReadout(MMDApp& app) noexcept {
    if (const auto main = app.MainWindow()) CheckMenuItem(GetMenu(main), 285,
        MF_BYCOMMAND | (app.state.floorVisible ? MF_CHECKED : MF_UNCHECKED));
}
MmsResult ApplyBoundFloor(MMDApp& app, bool enabled, bool& changed) noexcept {
    changed = false; return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        std::shared_ptr<HistoryChange> change; if (auto r = PlanFloor(app, enabled, change)) return r;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto r = history->PrepareNative("Floor collision", std::move(change), transaction, SharedHistory::NativeIntent::Adjustment)) return r;
        if (auto r = history->CommitNative(std::move(transaction))) return r;
        changed = true;
        // Notifications are outside the publication. Their failure cannot
        // undo the committed fields or invite a caller to commit them twice.
        Boundary([&]() -> MmsResult {
            bake::ScopedUiInputGate input;
            app.SceneModified() = 1; app.RecordedFrameCountByte1() = 1;
            const bool undo = history->UndoCount() != 0, redo = history->RedoCount() != 0;
            if (const auto main = app.MainWindow()) {
                EnableWindow(GetDlgItem(main, panel::kUndoButton), undo);
                EnableWindow(GetDlgItem(main, panel::kRedoButton), redo);
            }
            RefreshBoundFloorReadout(app); updates::RequestViewport(&app, false); return MMS_OK;
        });
        return MMS_OK;
    });
}
bool TryToggleBoundFloor(MMDApp* app) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool changed = false; ApplyBoundFloor(*app, app->state.floorVisible == 0, changed);
    if (!changed) RefreshBoundFloorReadout(*app);
    return true;
}
}
#endif
