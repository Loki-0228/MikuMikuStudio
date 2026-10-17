#include "../src/plugins/native_global_commands.hpp"
namespace {
std::array<float, 3> CameraResetRotation(MMDApp& app) {
    return {app.CameraPitch(), app.CameraYaw(), app.CameraRoll()};
}
void CameraResetPlanChecks() {
    GlobalRegistrationFixture f;
    const auto original = CameraResetRotation(*f.app); const auto before = f.Digest();
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanCameraRotationReset(*f.app, change) == MMS_OK && change && f.Digest() == before,
        "camera rotation reset privately plans without changing any state keys or selection");
    std::unique_ptr<plugins::PreparedHistoryChange> cancelled;
    Check(change->Prepare(true, cancelled) == MMS_OK && cancelled->IsCurrent(), "prepare cancellable camera reset");
    cancelled.reset(); Check(f.Digest() == before, "cancel camera reset candidate without any live writes");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("cancel camera reset", change, transaction) == MMS_OK, "reserve cancellable camera reset history");
    transaction.reset();
    Check(f.Digest() == before && !f.history.UndoCount() && !f.history.RedoCount(), "cancel prepared camera transaction without publication");
    Check(f.history.PrepareNative("camera rotation reset", change, transaction) == MMS_OK, "reserve camera reset history");
    failAfter = 0; const auto committed = f.history.CommitNative(std::move(transaction)); failAfter = -1;
    Check(committed == MMS_OK && CameraResetRotation(*f.app) == std::array<float, 3>{},
        "camera rotation and its journal publish without allocation");
    std::memcpy(f.app->CameraRotation(), original.data(), sizeof(original));
    Check(f.Digest() == before, "camera reset publication changes only rotation components");
    for (unsigned axis = 0; axis < 3; ++axis) f.app->CameraRotation()[axis] = 0;
    const auto applied = f.Digest();
    failAfter = 0; const auto failedUndo = f.history.History(false); failAfter = -1;
    Check(failedUndo == MMS_E_MEMORY && f.Digest() == applied && f.history.UndoCount() == 1 && !f.history.RedoCount(),
        "camera undo preparation allocation failure preserves pose and journal cursor");
    f.app->CurrentFrame() = 28; f.app->CameraPositionX() = 17; f.app->CameraDistance() = -94;
    f.app->CameraParentModel() = 19; f.app->CameraParentBone() = 23;
    const auto count = mdl::KeyPoolCapacity(f.app->CameraKeys());
    auto* grown = mdl::KeyPoolCreate<mdl::CameraKey>(count + 4);
    std::memcpy(grown, f.app->CameraKeys(), count * sizeof(mdl::CameraKey)); mdl::KeyPoolCommitGrowth(f.app->CameraKeys(), grown);
    Check(f.history.History(false) == MMS_OK && CameraResetRotation(*f.app) == original &&
        f.app->CurrentFrame() == 28 && f.app->CameraPositionX() == 17 && f.app->CameraDistance() == -94 &&
        f.app->CameraParentModel() == 19 && f.app->CameraParentBone() == 23,
        "camera reset undo survives key-pool growth and preserves new frame position distance and binding");
    const auto undone = f.Digest();
    failAfter = 0; const auto failedRedo = f.history.History(true); failAfter = -1;
    Check(failedRedo == MMS_E_MEMORY && f.Digest() == undone && !f.history.UndoCount() && f.history.RedoCount() == 1,
        "camera redo preparation allocation failure preserves pose and journal cursor");
    Check(f.history.History(true) == MMS_OK && CameraResetRotation(*f.app) == std::array<float, 3>{},
        "camera reset redo targets the same global camera after growth");
}
void CameraResetPartialAndStaleChecks() {
    GlobalRegistrationFixture f;
    f.app->CameraPitch() = 0; f.app->CameraYaw() = .75f; f.app->CameraRoll() = -0.0f;
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanCameraRotationReset(*f.app, change) == MMS_OK && change, "plan a camera reset with one affected axis");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("partial camera reset", change, tx) == MMS_OK && f.history.CommitNative(std::move(tx)) == MMS_OK,
        "commit partial camera reset");
    f.app->CameraPitch() = 1.25f; f.app->CameraRoll() = -.25f;
    Check(f.history.History(false) == MMS_OK && f.app->CameraYaw() == .75f &&
        f.app->CameraPitch() == 1.25f && f.app->CameraRoll() == -.25f,
        "camera undo restores only reset axes and preserves later values on untouched axes");
    Check(f.history.History(true) == MMS_OK && f.app->CameraYaw() == 0 && f.app->CameraPitch() == 1.25f,
        "camera redo also leaves untouched rotation axes unchanged");
    // Replace the pool while preserving its contents. New identity is a new
    // scene source, even if the allocator happens to reuse the same address.
    auto* replacement = mdl::KeyPoolCreate<mdl::CameraKey>(4);
    std::memcpy(replacement, f.app->CameraKeys(), 4 * sizeof(mdl::CameraKey));
    mdl::KeyPoolDestroy(f.app->CameraKeys()); f.app->CameraKeys() = replacement;
    const auto before = f.Digest();
    Check(f.history.History(false) == MMS_E_STALE && f.Digest() == before && f.history.UndoCount() == 1,
        "camera history rejects a replaced scene key-pool identity without moving the journal");
    for (int boundary = 0; boundary < 3; ++boundary) {
        GlobalRegistrationFixture source; std::shared_ptr<plugins::HistoryChange> plan;
        Check(plugins::PlanCameraRotationReset(*source.app, plan) == MMS_OK && plan, "capture camera source for stale preparation");
        if (boundary == 0) {
            source.app->CameraPitch() += .5f; std::unique_ptr<plugins::PreparedHistoryChange> candidate;
            const auto digest = source.Digest();
            Check(plan->Prepare(true, candidate) == MMS_E_STALE && !candidate && source.Digest() == digest,
                "camera source change before preparation rejects an obsolete plan");
        } else {
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> pending;
            Check(source.history.PrepareNative("stale camera reset", plan, pending) == MMS_OK, "prepare camera reset before source invalidation");
            if (boundary == 1) ++source.app->CurrentFrame(); else source.app->CameraRoll() += .25f;
            const auto digest = source.Digest();
            Check(source.history.CommitNative(std::move(pending)) == MMS_E_STALE && source.Digest() == digest && !source.history.UndoCount(),
                "camera frame or rotation change after prepare prevents any partial publication");
        }
    }
}
void CameraResetCommandChecks() {
    GlobalRegistrationFixture f;
    Check(!plugins::TryResetBoundCameraRotation(nullptr) && !plugins::TryResetBoundCameraRotation(f.app.get()),
        "camera reset leaves null and unbound command ownership unchanged");
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto rotation = CameraResetRotation(*f.app); const auto frame = f.app->CurrentFrame();
    CmdViewMenu(f.app.get(), nullptr, 302, 0);
    Check(f.history.UndoCount() == 1 && CameraResetRotation(*f.app) == std::array<float, 3>{} &&
        f.app->SceneModified() && f.app->RecordedFrameCountByte1() && !f.app->PhysicsResetPending() && f.app->CurrentFrame() == frame,
        "production camera menu joins one common step and publishes dirty state after commit");
    // A later light registration supplies an unrelated redo branch without
    // resampling the unregistered camera rotation during its own undo.
    std::shared_ptr<plugins::HistoryChange> registered;
    Check(plugins::PlanLightRegistration(*f.app, frame, registered) == MMS_OK && registered,
        "plan light registration after camera pose reset");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("light key", registered, tx) == MMS_OK && f.history.CommitNative(std::move(tx)) == MMS_OK &&
        f.history.History(false) == MMS_OK, "make redo by undoing later light registration");
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    CmdViewMenu(f.app.get(), nullptr, 302, 0);
    Check(f.history.UndoCount() == 1 && f.history.RedoCount() == 1 &&
        !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(), "zero camera rotation is a no-op that preserves key-registration redo");
    Check(f.history.History(false) == MMS_OK && CameraResetRotation(*f.app) == rotation,
        "common undo reverses pose reset independently of the later registration");
    f.app->CameraMode() = 0; f.app->SelectedModelSlot() = 255;
    Check(f.history.History(true) == MMS_OK && f.history.History(true) == MMS_OK,
        "camera reset and registration redo remain independent of current editor mode");
}
void CameraResetFailureAndGuardChecks() {
    GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    std::shared_ptr<plugins::HistoryChange> earlier;
    Check(plugins::PlanCameraRegistration(*f.app, 19, false, earlier) == MMS_OK, "plan earlier camera edit for redo failure test");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("earlier key", earlier, tx) == MMS_OK && f.history.CommitNative(std::move(tx)) == MMS_OK &&
        f.history.History(false) == MMS_OK, "prepare redo before camera reset failures");
    const auto before = f.Digest();
    unsigned failures = 0; bool succeeded = false;
    for (long limit = 0; limit < 128; ++limit) {
        failAfter = limit; CmdViewMenu(f.app.get(), nullptr, 302, 0); failAfter = -1;
        if (f.history.UndoCount()) { succeeded = true; break; }
        ++failures;
        Check(f.Digest() == before && f.history.RedoCount() == 1,
            "camera reset allocation failure preserves every state byte all keys and redo");
        Check(f.history.Enter() == MMS_OK, "camera reset failure releases its journal gate"); f.history.Leave();
    }
    Check(succeeded && failures >= 5 && f.history.RedoCount() == 0, "camera reset allocation sweep reaches history preparation before successful commit");
    Check(f.history.History(false) == MMS_OK, "camera reset remains undoable after allocation failures");
    for (int gate = 0; gate < 4; ++gate) {
        if (gate == 0) f.app->PlaybackActive() = 1;
        if (gate == 1) f.app->FrameStepPlayback() = 1;
        if (gate == 2) f.app->state.recordPlaybackStartPending = 1;
        if (gate == 3) Check(f.history.Enter() == MMS_OK, "reserve history before camera reset rejection");
        const auto digest = f.Digest(); CmdViewMenu(f.app.get(), nullptr, 302, 0);
        Check(f.Digest() == digest && f.history.RedoCount() == 1 && !f.history.UndoCount(),
            "camera reset consumes playback recording or busy rejection without legacy fallthrough");
        if (gate == 3) f.history.Leave();
        f.app->PlaybackActive() = f.app->FrameStepPlayback() = 0; f.app->state.recordPlaybackStartPending = false;
    }
    { ScopedSceneMutation mutation(*f.app); const auto digest = f.Digest(); CmdViewMenu(f.app.get(), nullptr, 302, 0);
      Check(f.Digest() == digest && f.history.RedoCount() == 1, "camera reset rejects active scene mutation"); }
    f.app->CameraPitch() = std::numeric_limits<float>::quiet_NaN(); const auto invalid = f.Digest();
    CmdViewMenu(f.app.get(), nullptr, 302, 0);
    Check(f.Digest() == invalid && f.history.RedoCount() == 1, "non-finite camera source rejects reset without destroying redo");
}
}
void NativeCameraResetChecks() {
    CameraResetPlanChecks(); CameraResetPartialAndStaleChecks(); CameraResetCommandChecks(); CameraResetFailureAndGuardChecks();
}
