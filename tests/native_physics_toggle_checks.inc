// Include after native_bone_curve_write_checks.inc and history_physics_checks.inc.
// Call NativePhysicsToggleChecks(). No renderer, file import or world step.
#include "../src/plugins/native_physics_toggle.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
namespace mikumikustudio { void ApplyPhysicsOnOff(int); }
namespace {
struct PhysicsToggleFixture : BoneRegistrationFixture {
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    PhysicsFixture firstWorld, secondWorld;
    PhysicsScene firstScene{}, secondScene{};
    std::array<mdl::RigidRecord, 4> firstRigids{}, secondRigids{};
    std::array<mdl::JointRecord, 1> firstJoints{}, secondJoints{};
    std::array<mdl::IkChain, 1> firstIk{}, secondIk{};
    std::array<mdl::BoneOrderEntry, 1> firstOrder{}, secondOrder{};
    PhysicsToggleFixture() {
        std::memset(renderer.get(), 0, sizeof *renderer); app->Renderer() = renderer.get();
        firstScene.world = &firstWorld.world; secondScene.world = &secondWorld.world;
        unsigned side = 0;
        for (auto* fixture : {&first, &second}) {
            auto& rigids = side ? secondRigids : firstRigids;
            auto& world = side ? secondWorld : firstWorld;
            auto& joints = side ? secondJoints : firstJoints;
            auto& ik = side ? secondIk : firstIk;
            auto& order = side ? secondOrder : firstOrder;
            SeedRegistrationChain(*fixture);
            fixture->dirty = {}; fixture->m->boneKeys[0].allocated = 1;
            fixture->m->boneKeys[1].allocated = 0; fixture->m->boneKeys[2].allocated = 1;
            fixture->m->boneKeys[3].allocated = 1;
            fixture->bones[0].hasRigidBody = 1; fixture->bones[1].hasRigidBody = 1;
            fixture->bones[0].slotIndex = 0; fixture->bones[1].slotIndex = -1;
            fixture->bones[0].rigidIdx = -296;
            fixture->bones[0].ikBackup[0] = 100; fixture->bones[0].ikBackup[3] = .6f; fixture->bones[0].ikBackup[6] = .8f;
            fixture->bones[0].ikWorkingPos[0] = 42; fixture->bones[0].ikWorkingQuat[3] = 1;
            fixture->m->scenePtr = side ? &secondScene : &firstScene;
            fixture->m->rigidTable = rigids.data(); fixture->m->rigidCount = unsigned(rigids.size());
            for (unsigned index = 0; index < rigids.size(); ++index) {
                rigids[index].body = world.bodies[index].get(); rigids[index].boneIndex = index == 3 ? 1 : 0;
                rigids[index].mode = index == 1 ? 2 : 1; rigids[index].kinematicFlag = index == 1;
            }
            rigids[2].staticFlag = 1; rigids[2].mode = 0;
            world.bodies[2]->setCollisionFlags(world.bodies[2]->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
            joints[0].constraint = world.joint->getUid(); fixture->m->jointTable = joints.data(); fixture->m->jointCount = 1;
            fixture->m->ikChains = ik.data(); fixture->m->ikChainCount = 1;
            fixture->m->boneOrderTable = order.data(); fixture->m->boneOrderCount = 1;
            fixture->m->displayKeys[0].visible = 1;
            fixture->m->displayKeys[0].ikStates[0] = 1;
            fixture->m->displayKeys[0].selectorStates[0].modelIndex = -1;
            fixture->m->displayKeys[0].selectorStates[0].boneIndex = -1;
            ++side;
        }
        app->PlaybackPhysicsMode() = 2; app->CurrentFrame() = 5;
        SeekBoth();
    }
    ~PhysicsToggleFixture() {
        history.Reset(); history.Collect();
        app->Renderer() = nullptr;
        for (auto* fixture : {&first, &second}) {
            fixture->m->scenePtr = nullptr; fixture->m->rigidTable = nullptr; fixture->m->rigidCount = 0;
            fixture->m->jointTable = nullptr; fixture->m->jointCount = 0; fixture->m->ikChains = nullptr;
            fixture->m->boneOrderTable = nullptr; fixture->m->boneOrderCount = fixture->m->ikChainCount = 0;
        }
    }
    void SeekBoth() {
        SeekModelFrame(first.raw(), app->CurrentFrame(), app->PlaybackPhysicsMode());
        SeekModelFrame(second.raw(), app->CurrentFrame(), app->PlaybackPhysicsMode());
    }
    std::shared_ptr<plugins::HistoryChange> Plan(bool disabled) {
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanPhysicsKeyToggle(*app, 0, disabled, change) == MMS_OK,
            "physics toggle plan succeeds without modifying live keys or solver");
        return change;
    }
    void Commit(std::shared_ptr<plugins::HistoryChange> change) {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(change && history.PrepareNative("physics markers", change, transaction) == MMS_OK,
            "prepare complete physics-key and solver publication");
        Check(CommitRegistrationNoAllocation(history, std::move(transaction)) == MMS_OK,
            "physics keys pose IK runtime and Bullet flags commit without C++ allocation");
    }
    void Legacy(bool disabled) {
        app->SelectedModelSlot() = 1; ApplyPhysicsOnOff(disabled); app->SelectedModelSlot() = 0;
    }
};
bool SamePhysicsBone(const mdl::BoneRecord& a, const mdl::BoneRecord& b) {
    return !std::memcmp(a.trans, b.trans, sizeof a.trans) && !std::memcmp(a.rotQuat, b.rotQuat, sizeof a.rotQuat) &&
        a.physicsDisabled == b.physicsDisabled && a.rigidIdx == b.rigidIdx &&
        !std::memcmp(a.ikWorkingPos, b.ikWorkingPos, sizeof a.ikWorkingPos) &&
        !std::memcmp(a.ikWorkingQuat, b.ikWorkingQuat, sizeof a.ikWorkingQuat);
}
void CheckPhysicsParity(PhysicsToggleFixture& f) {
    Check(!std::memcmp(f.first.m->boneKeys, f.second.m->boneKeys, mdl::BoneKeyCapacity(f.first.m->boneKeys) * sizeof(mdl::BoneKey)),
        "physics toggle matches actual unbound native ApplyPhysicsOnOff key bytes");
    Check(SamePhysicsBone(f.first.bones[0], f.second.bones[0]),
        "affected native bone pose physics switch rigid arm and IK working fields match full original seek");
    for (unsigned i = 0; i < f.firstRigids.size(); ++i)
        Check(f.firstRigids[i].mode == f.secondRigids[i].mode &&
            f.firstWorld.bodies[i]->getCollisionFlags() == f.secondWorld.bodies[i]->getCollisionFlags(),
            "dynamic mixed static and unrelated rigid modes match the original notifier");
}
void PhysicsToggleNativeParity() {
    for (int mode : {0, 1, 2, 3}) for (int frame : {0, 5, 10, 20, 30, 40}) for (unsigned variant = 0; variant < 4; ++variant) {
        PhysicsToggleFixture f; f.app->PlaybackPhysicsMode() = mode; f.app->CurrentFrame() = frame;
        for (auto* fixture : {&f.first, &f.second}) {
            for (auto index : {0u, 2u, 3u}) {
                fixture->m->boneKeys[index].physicsDisabled = variant == 0 ? 0 : variant == 1 ? 1 :
                    variant == 3 ? index == 3 : index != 0;
                fixture->m->boneKeys[index].allocated = variant == 3 ? index == 2 : 1;
            }
        }
        f.SeekBoth();
        const bool disabled = variant == 0 || variant == 3;
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first), peer = f.second.Digest();
        const auto sourceScale = bone_scale::AuthoringState::Capture(f.first.raw());
        auto change = f.Plan(disabled);
        Check(change && f.first.Digest() == before && f.second.Digest() == peer && PoseRingDigest(f.first) == ring &&
            sourceScale.Equals(bone_scale::AuthoringState::Capture(f.first.raw())), "physics marker planning preserves all native and side authoring");
        Check(!change->RequiresPhysicsReset(), "in-place physics switch does not reset unrelated worlds");
        f.Commit(change); f.Legacy(disabled); CheckPhysicsParity(f);
        Check(PoseRingDigest(f.first) == ring && f.first.dirty[0] == 0 && f.app->CurrentFrame() == frame,
            "bound physics marker edit keeps legacy rings dirty bytes and playhead intact");
        // Undo/redo also samples the complete native before/after tracks at
        // this frame, including the mode-3 transition restored by undo.
        f.first.bones[0].ikBackup[0] = f.second.bones[0].ikBackup[0] = 101;
        std::vector<unsigned char> sourceFlags(32);
        for (unsigned i = 0; i < 32; ++i) sourceFlags[i] = f.first.m->boneKeys[i].physicsDisabled;
        Check(f.history.History(false) == MMS_OK, "physics marker undo prepares physical switch and IK runtime");
        for (unsigned i = 0; i < 32; ++i) f.second.m->boneKeys[i].physicsDisabled = f.first.m->boneKeys[i].physicsDisabled;
        SeekModelFrame(f.second.raw(), frame, mode); CheckPhysicsParity(f);
        Check(f.history.History(true) == MMS_OK, "physics marker redo uses retained keys without reopening a file");
        for (unsigned i = 0; i < 32; ++i) f.second.m->boneKeys[i].physicsDisabled = sourceFlags[i];
        SeekModelFrame(f.second.raw(), frame, mode); CheckPhysicsParity(f);
    }
}
void PhysicsToggleDraftAndScope() {
    PhysicsToggleFixture f;
    f.first.bones[0].trans[0] = 67; f.first.dirty[0] = 1;
    f.first.bones[1].trans[0] = 89; f.first.dirty[1] = 1; f.first.morphs[0].value = .73f;
    const auto otherBone = f.first.bones[1]; const auto peer = f.second.Digest();
    const auto ik = f.firstIk; const auto selectors = f.firstOrder; const auto visible = f.first.m->loadComplete;
    const auto flags = f.firstWorld.bodies[3]->getCollisionFlags(), activation = f.firstWorld.bodies[0]->getActivationState();
    const auto transform = f.firstWorld.bodies[0]->getWorldTransform(); const auto velocity = f.firstWorld.bodies[0]->getLinearVelocity();
    auto change = f.Plan(true); f.Commit(change);
    for (unsigned direction = 0; direction < 3; ++direction) {
        if (direction) Check(f.history.History(direction == 2) == MMS_OK, "dirty physics marker undo redo succeeds");
        Check(f.first.bones[0].trans[0] == 67 && f.first.dirty[0] == 1 && !std::memcmp(&otherBone, &f.first.bones[1], sizeof otherBone) &&
            f.first.dirty[1] == 1 && f.first.morphs[0].value == .73f && f.second.Digest() == peer,
            "physics marker edit preserves unregistered poses unrelated bones morphs and the peer model");
        Check(!std::memcmp(ik.data(), f.firstIk.data(), sizeof ik) && !std::memcmp(selectors.data(), f.firstOrder.data(), sizeof selectors) &&
            visible == f.first.m->loadComplete, "physics marker command avoids the old whole-model display IK and parent rewrites");
        Check(f.firstWorld.bodies[3]->getCollisionFlags() == flags && f.firstWorld.bodies[0]->getActivationState() == activation &&
            f.firstWorld.bodies[0]->getWorldTransform() == transform && f.firstWorld.bodies[0]->getLinearVelocity() == velocity &&
            f.firstWorld.world.getNumConstraints() == 1 && f.firstWorld.world.getNumCollisionObjects() == 5,
            "kinematic publication preserves body activation transform velocity and world membership without stepping");
    }
}
void PhysicsToggleSeekAndWindows() {
    for (int frame : {5, 15, 20}) {
        PhysicsToggleFixture f; f.app->PlaybackPhysicsMode() = 3; f.app->CurrentFrame() = frame;
        for (auto* fixture : {&f.first, &f.second}) {
            auto& head = fixture->m->displayKeys[0]; auto& tail = fixture->m->displayKeys[1];
            head.next = 1; tail.frame = 15; tail.previous = 0; tail.visible = 1;
            tail.ikStates[0] = 0; tail.selectorStates[0].modelIndex = 1; tail.selectorStates[0].boneIndex = 0;
            fixture->m->boneKeys[2].physicsDisabled = fixture->m->boneKeys[3].physicsDisabled = 1;
        }
        f.SeekBoth(); f.Commit(f.Plan(false)); f.Legacy(false); CheckPhysicsParity(f);
        f.app->CurrentFrame() = frame + 7; f.SeekBoth();
        const auto selectors = f.firstOrder; const auto ik = f.firstIk;
        Check(f.history.History(false) == MMS_OK, "physics undo re-samples at a later playhead with actual parent/IK windows");
        for (unsigned i = 0; i < 32; ++i) f.second.m->boneKeys[i].physicsDisabled = f.first.m->boneKeys[i].physicsDisabled;
        SeekModelFrame(f.second.raw(), f.app->CurrentFrame(), 3); CheckPhysicsParity(f);
        Check(!std::memcmp(selectors.data(), f.firstOrder.data(), sizeof selectors) && !std::memcmp(ik.data(), f.firstIk.data(), sizeof ik),
            "physics undo uses the active parent window while preserving its display/IK owners");
        f.first.m->boneKeys[0].allocated = f.second.m->boneKeys[0].allocated = 19;
        f.first.m->boneKeys[2].allocated = f.second.m->boneKeys[2].allocated = 0;
        Check(f.history.History(true) == MMS_OK && f.first.m->boneKeys[0].allocated == 19 && f.first.m->boneKeys[2].allocated == 0,
            "physics redo retains later independent selection and does not retarget the historical edit");
        for (unsigned i = 0; i < 32; ++i) f.second.m->boneKeys[i].physicsDisabled = f.first.m->boneKeys[i].physicsDisabled;
        SeekModelFrame(f.second.raw(), f.app->CurrentFrame(), 3); CheckPhysicsParity(f);
    }
}
void PhysicsToggleModeAndAdmission() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        PhysicsToggleFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "select physics-marker recording mode");
        ApplyPhysicsOnOff(1);
        Check(f.history.UndoCount() == 1 && f.first.m->boneKeys[0].physicsDisabled == 1 && !f.app->PhysicsResetPending(),
            "dialog confirmation records one explicit action in both history modes without a global physics reset");
        Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "seed physics marker redo");
        ApplyPhysicsOnOff(0);
        Check(f.history.UndoCount() == 0 && f.history.RedoCount() == 1, "same-value physics confirmation preserves redo");
        for (auto index : {0u, 2u, 3u}) f.first.m->boneKeys[index].allocated = 0;
        const auto before = f.first.Digest(); ApplyPhysicsOnOff(1);
        Check(f.first.Digest() == before && f.history.RedoCount() == 1, "no selected physics keys is a nonmutating no-op");
        f.first.m->boneKeys[0].allocated = 1;
        auto change = f.Plan(true); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("pending physics", change, transaction) == MMS_OK, "reserve physics marker publication");
        const auto reserved = f.first.Digest(); ApplyPhysicsOnOff(1);
        Check(f.first.Digest() == reserved && f.history.RedoCount() == 1, "BUSY physics command never falls through to the old native ring");
        transaction.reset();
    }
    PhysicsToggleFixture f;
    Check(!plugins::TryApplyBoundPhysicsKeyToggle(f.app.get(), true), "unbound physics command keeps the original route available");
    ApplyPhysicsOnOff(1);
    Check(f.history.UndoCount() == 0 && f.first.m->boneKeys[0].physicsDisabled == 1 && f.first.m->undoState[0] == 2,
        "unbound ApplyPhysicsOnOff still writes the original operation-2 undo ring");
}
void PhysicsToggleFreeCurves() {
    PhysicsToggleFixture f;
    auto tracks = FreeBoneCurve().tracks; std::string error;
    tracks[0].keys.front().interpolation = tracks[0].keys.back().interpolation = animation_curve::Interpolation::Bezier;
    tracks[0].keys.front().outSlope = 1.8; tracks[0].keys.front().outWeight = .22;
    tracks[0].keys.back().inSlope = -.25; tracks[0].keys.back().inWeight = .38;
    Check(bone_curve::Write(f.first.raw(), 0, tracks, error), "physics marker fixture has authored free handles");
    for (unsigned i = 0; i < mdl::BoneKeyCapacity(f.first.m->boneKeys); ++i)
        if (f.first.m->boneKeys[i].frame || i == 0) f.first.m->boneKeys[i].allocated = 1;
    const auto authors = bone_curve::AuthoringState::Capture(f.first.raw());
    f.Commit(f.Plan(true));
    bone_curve::Tracks actual;
    Check(bone_curve::ReadDetached(f.first.raw(), 0, actual) && actual[0].keys.front().outSlope == tracks[0].keys.front().outSlope &&
        actual[0].keys.back().inSlope == tracks[0].keys.back().inSlope, "physics-only native hashes retain the original free-curve handles");
    Check(f.history.History(false) == MMS_OK && bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*authors),
        "physics marker undo restores the exact authored baseline and native flags together");
    Check(f.history.History(true) == MMS_OK && bone_curve::ReadDetached(f.first.raw(), 0, actual),
        "physics marker redo publishes its retained reconciled authoring snapshot");
}
void PhysicsToggleFailuresAndGuards() {
    unsigned failures = 0; bool reached = false;
    for (long allocation = 0; allocation < 256; ++allocation) {
        PhysicsToggleFixture f; const auto source = f.first.Digest(), peer = f.second.Digest();
        const int flags = f.firstWorld.bodies[0]->getCollisionFlags();
        std::shared_ptr<plugins::HistoryChange> change; std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        failAfter = allocation; auto status = plugins::PlanPhysicsKeyToggle(*f.app, 0, true, change);
        if (status == MMS_OK) status = f.history.PrepareNative("physics failure", change, token); failAfter = -1;
        Check(f.first.Digest() == source && f.second.Digest() == peer && f.firstWorld.bodies[0]->getCollisionFlags() == flags &&
            f.firstRigids[0].mode == 1 && f.history.UndoCount() == 0, "every allocation failure leaves native keys body flags and all history unchanged");
        if (status == MMS_OK) { reached = true; break; }
        Check(status == MMS_E_MEMORY && !token, "physics marker preparation reports allocation failure without publishing"); ++failures;
    }
    Check(reached && failures > 8, "physics marker allocation sweep reaches complete preparation");
    for (unsigned variant = 0; variant < 5; ++variant) {
        PhysicsToggleFixture f; auto change = f.Plan(true); std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("stale physics", change, token) == MMS_OK, "prepare stale physical candidate");
        if (variant == 0) f.firstWorld.bodies[0]->setCollisionFlags(f.firstWorld.bodies[0]->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
        else if (variant == 1) f.firstRigids[0].mode = 2;
        else if (variant == 2) f.first.bones[0].ikBackup[0] += 1;
        else if (variant == 3) f.app->CurrentFrame() += 1;
        else f.first.m->boneKeys[0].allocated = 0;
        const auto before = f.first.Digest(); const auto flags = f.firstWorld.bodies[0]->getCollisionFlags();
        Check(CommitRegistrationNoAllocation(f.history, std::move(token)) == MMS_E_STALE && f.first.Digest() == before &&
            f.firstWorld.bodies[0]->getCollisionFlags() == flags && f.history.UndoCount() == 0,
            "stale body flags rigid metadata IK input playhead and selection reject the complete candidate");
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
        PhysicsToggleFixture f; auto* saved = f.second.m->boneTable; auto* savedKeys = f.second.m->boneKeys;
        auto* savedBody = f.secondRigids[0].body;
        if (variant == 0) f.second.m->boneTable = f.first.m->boneTable;
        else if (variant == 1) f.second.m->boneKeys = f.first.m->boneKeys;
        else if (variant == 2) f.secondRigids[0].body = f.firstRigids[0].body;
        else f.first.m->boneKeys[3].next = 2;
        std::shared_ptr<plugins::HistoryChange> change; const auto before = f.first.Digest();
        const auto status = plugins::PlanPhysicsKeyToggle(*f.app, 0, true, change);
        f.second.m->boneTable = saved; f.second.m->boneKeys = savedKeys; f.secondRigids[0].body = savedBody;
        Check(status == MMS_E_ARGUMENT && !change && f.first.Digest() == before,
            "cross-model bone/key/body aliases and malformed chains reject without partial changes");
    }
    for (bool prepared : {false, true}) {
        PhysicsToggleFixture f; f.first.bones[1].hasRigidBody = 0; f.first.m->boneKeys[1].allocated = 1;
        auto change = f.Plan(true); std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        if (prepared) Check(f.history.PrepareNative("physics eligibility", change, token) == MMS_OK,
            "prepare complete physics target set before a bone becomes eligible");
        f.first.bones[1].hasRigidBody = 1; const auto foreign = f.first.Digest();
        const auto status = prepared ? f.history.CommitNative(std::move(token)) : f.history.PrepareNative("physics stale eligibility", change, token);
        Check(status == MMS_E_STALE && f.first.Digest() == foreign && !f.history.UndoCount(),
            "newly eligible previously skipped bone invalidates both planned and prepared physics target sets");
    }
}
void PhysicsToggleBudgetAndReplayFailures() {
    {
        PhysicsToggleFixture f; auto change = f.Plan(true);
        std::unique_ptr<plugins::PreparedHistoryChange> candidate; std::uint64_t measured = 0;
        Check(change->Prepare(true, candidate) == MMS_OK && candidate->TargetRetainedBytes(measured) &&
            measured == change->RetainedBytes(), "initial fixed and dynamic physics charges include the same retained owner bookkeeping");
        candidate.reset();
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations, 32, measured - 1) == MMS_OK,
            "set physics budget below complete key/author/owner retained bytes");
        const auto before = f.first.Digest(); const auto flags = f.firstWorld.bodies[0]->getCollisionFlags();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("physics over budget", change, token) == MMS_E_MEMORY && !token &&
            f.first.Digest() == before && f.firstWorld.bodies[0]->getCollisionFlags() == flags && !f.history.UndoCount(),
            "physics owner-inclusive budget refusal leaves keys solver and journal unpublished");
    }
    PhysicsToggleFixture f; f.Commit(f.Plan(true));
    for (bool redo : {false, true}) {
        const auto before = f.first.Digest(); const auto flags = f.firstWorld.bodies[0]->getCollisionFlags();
        const auto undo = f.history.UndoCount(), next = f.history.RedoCount();
        failAfter = 0; const auto status = f.history.History(redo); failAfter = -1;
        Check(status == MMS_E_MEMORY && f.first.Digest() == before && f.firstWorld.bodies[0]->getCollisionFlags() == flags &&
            f.history.UndoCount() == undo && f.history.RedoCount() == next,
            "allocation refusal during physics undo and redo preserves the complete existing direction");
        Check(f.history.History(redo) == MMS_OK, "physics undo and redo remain usable after allocation refusal");
    }
}
void PhysicsToggleReadoutReentry() {
    struct Hook {
        MMDApp* app = nullptr;
        bool armed = true;
        unsigned calls = 0;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR context) {
            auto& self = *reinterpret_cast<Hook*>(context);
            if (message == WM_ENABLE && wp && self.armed) {
                self.armed = false; ++self.calls;
                plugins::TryApplyBoundPhysicsKeyToggle(self.app, false);
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    PhysicsToggleFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    Hook hook{f.app.get()}; const auto undo = GetDlgItem(f.app->MainWindow(), panel::kUndoButton);
    Check(SetWindowSubclass(undo, Hook::Proc, 0x70687973, reinterpret_cast<DWORD_PTR>(&hook)) != FALSE,
        "install real undo-button enable reentry fixture");
    ApplyPhysicsOnOff(1);
    Check(hook.calls == 1 && f.history.UndoCount() == 1 && f.first.m->boneKeys[0].physicsDisabled == 1 &&
        f.first.bones[0].physicsDisabled == 1 && f.firstRigids[0].mode == 0,
        "first post-commit WM_ENABLE hint cannot publish a nested opposite physics edit");
    RemoveWindowSubclass(undo, Hook::Proc, 0x70687973);
}
void NativePhysicsToggleChecks() {
    std::puts("RUN physics marker native parity"); PhysicsToggleNativeParity();
    std::puts("RUN physics marker dirty scope and parent windows"); PhysicsToggleDraftAndScope(); PhysicsToggleSeekAndWindows();
    std::puts("RUN physics marker recording modes and admission"); PhysicsToggleModeAndAdmission();
    std::puts("RUN physics marker free curves"); PhysicsToggleFreeCurves();
    std::puts("RUN physics marker failure and source guards"); PhysicsToggleFailuresAndGuards();
    std::puts("RUN physics marker retained budget and replay failures"); PhysicsToggleBudgetAndReplayFailures();
    std::puts("RUN physics marker readout reentry"); PhysicsToggleReadoutReentry();
}
}
