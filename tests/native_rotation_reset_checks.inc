// Production menu 302, using the same hidden-window fixture as numeric edits.
namespace mikumikustudio { void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t); }
namespace {
void RotationResetCommand(PoseNumericFixture& f) {
    CmdViewMenu(f.app.get(), f.app->MainWindow(), 302, 0);
}
void RotationResetHistoryChecks() {
    PoseNumericFixture f;
    const std::array<float, 4> rotation{.1f, .2f, .3f, .9f};
    std::memcpy(f.first.bones[0].rotQuat, rotation.data(), sizeof(rotation));
    f.first.bones[0].trans[0] = 7;
    const auto scale = Scale(2); bone_scale::SetPose(f.first.raw(), 0, scale);
    const auto firstRing = PoseRingDigest(f.first), secondRing = PoseRingDigest(f.second);
    const auto other = f.first.bones[1]; const auto selection = f.first.selected;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    RotationResetCommand(f);
    const std::array<float, 4> identity{0, 0, 0, 1};
    Check(f.history.UndoCount() == 1 && !std::memcmp(f.first.bones[0].rotQuat, identity.data(), sizeof(identity)) &&
        f.first.dirty[0] == 1 && f.first.bones[0].trans[0] == 7 && bone_scale::Pose(f.first.raw(), 0) == scale,
        "menu rotation reset joins one common node and preserves translation and scale");
    Check(!std::memcmp(&other, &f.first.bones[1], sizeof(other)) && f.first.selected == selection &&
        f.first.m->selectedBone == 0 && f.SelectionUnchanged() && f.app->PhysicsResetPending() == 0 &&
        PoseRingDigest(f.first) == firstRing && PoseRingDigest(f.second) == secondRing,
        "rotation reset preserves other bones selections physics request and both legacy rings");
    f.app->SceneModified() = 0; f.app->RecordedFrameCountByte1() = 0;
    RotationResetCommand(f);
    Check(f.history.UndoCount() == 1 && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
        "repeating identity reset does not dirty or append history");
    f.app->SelectedModelSlot() = 1; f.Commit(panel::kReadoutPosYEdit, "9");
    f.app->CurrentFrame() = 23; f.app->CameraMode() = 1;
    CmdControl400(f.app.get(), f.app->MainWindow(), 400, 0);
    Check(f.second.bones[0].trans[1] == 0 && f.history.UndoCount() == 1,
        "camera-mode common undo reverses later edit on another model first");
    CmdControl400(f.app.get(), f.app->MainWindow(), 400, 0);
    Check(!std::memcmp(f.first.bones[0].rotQuat, rotation.data(), sizeof(rotation)) && !f.first.dirty[0] &&
        f.app->CurrentFrame() == 23 && f.app->SelectedModelSlot() == 1 && f.history.RedoCount() == 2,
        "rotation reset undo restores exact quaternion and dirty flag across model and frame selection");
    f.app->CameraMode() = 0; f.app->SelectedModelSlot() = 0; f.first.m->selectedBone = 1;
    f.app->SceneModified() = 0; f.app->RecordedFrameCountByte1() = 0;
    RotationResetCommand(f);
    Check(f.history.RedoCount() == 2 && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
        "already-identity bone with clear dirty flag preserves redo and scene flags");
    CmdControl400(f.app.get(), f.app->MainWindow(), 401, 0);
    CmdControl400(f.app.get(), f.app->MainWindow(), 401, 0);
    Check(!std::memcmp(f.first.bones[0].rotQuat, identity.data(), sizeof(identity)) &&
        f.second.bones[0].trans[1] == 9 && f.history.UndoCount() == 2 && f.app->CurrentFrame() == 23 &&
        PoseRingDigest(f.first) == firstRing && PoseRingDigest(f.second) == secondRing,
        "cross-model redo restores rotation and later pose without seeking or old-ring writes");
}
void RotationResetGuardChecks() {
    PoseNumericFixture f;
    Check(!plugins::TryResetBoundBoneRotation(nullptr) && !plugins::TryResetBoundBoneRotation(f.app.get()),
        "rotation reset route leaves unbound and null apps unowned");
    f.first.bones[0].rotQuat[1] = .6f; f.first.bones[0].rotQuat[3] = .8f;
    const auto before = f.first.Digest();
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    for (int gate = 0; gate < 4; ++gate) {
        if (gate == 0) f.app->PlaybackActive() = 1;
        if (gate == 1) f.app->FrameStepPlayback() = 1;
        if (gate == 2) f.app->state.recordPlaybackStartPending = 1;
        if (gate == 3) { Check(f.history.Enter() == MMS_OK, "reserve history before guarded rotation reset"); }
        RotationResetCommand(f);
        if (gate == 3) f.history.Leave();
        Check(f.first.Digest() == before && !f.history.UndoCount() && !f.app->SceneModified(),
            "rotation reset owns busy playback recording and scene-mutation rejection without legacy fallback");
        f.app->PlaybackActive() = f.app->FrameStepPlayback() = 0;
        f.app->state.recordPlaybackStartPending = false;
    }
    { ScopedSceneMutation mutation(*f.app); RotationResetCommand(f); }
    Check(f.first.Digest() == before && !f.history.UndoCount(), "scene mutation rejects rotation reset before authoring");
    f.first.m->selectedBone = 2000; RotationResetCommand(f);
    f.first.m->selectedBone = -1; RotationResetCommand(f);
    f.first.m->selectedBone = 0; f.app->SelectedModelSlot() = 255; RotationResetCommand(f);
    f.app->SelectedModelSlot() = 2; RotationResetCommand(f);
    f.app->SelectedModelSlot() = 0;
    Check(f.first.Digest() == before && !f.history.UndoCount(), "rotation reset rejects absent models and invalid bone indices");
    f.app->CameraMode() = 1;
    Check(!plugins::TryResetBoundBoneRotation(f.app.get()), "camera rotation authoring remains explicitly outside this migration");
}
void RotationResetFailureChecks() {
    PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Commit(panel::kReadoutPosYEdit, "3");
    Check(f.history.History(false) == MMS_OK, "create redo branch for rotation reset allocation sweep");
    f.first.bones[0].rotQuat[2] = .6f; f.first.bones[0].rotQuat[3] = .8f;
    const auto before = f.first.Digest(), ring = PoseRingDigest(f.first);
    PoseAxisMessageProbe messages(f);
    f.app->SceneModified() = 0; f.app->RecordedFrameCountByte1() = 0; f.app->PhysicsResetPending() = 0;
    unsigned failures = 0; bool succeeded = false;
    for (long limit = 0; limit < 256; ++limit) {
        failAfter = limit; RotationResetCommand(f); failAfter = -1;
        if (f.history.UndoCount()) { succeeded = true; break; }
        ++failures;
        Check(f.first.Digest() == before && f.history.RedoCount() == 1 &&
            !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && !f.app->PhysicsResetPending() &&
            PoseRingDigest(f.first) == ring, "failed reset capture or inverse preparation rolls back all authored data and retains redo");
        Check(f.history.Enter() == MMS_OK, "failed rotation reset releases its reservation"); f.history.Leave();
    }
    Check(succeeded && failures > 8 && !f.history.RedoCount() && !messages.observedBusy,
        "rotation reset allocation sweep reaches adoption then succeeds with UI notifications outside reservation");
    Check(f.history.History(false) == MMS_OK && f.first.Digest() == before,
        "rotation reset after allocation failures remains exactly undoable");
}
void RotationResetLegacyChecks() {
    PoseNumericFixture f;
    Check(d3dx::Get().Load(), "load existing D3DX implementation for legacy rotation comparison");
    f.first.bones[0].rotQuat[2] = .6f; f.first.bones[0].rotQuat[3] = .8f;
    const auto cursor = f.first.m->undoState[0];
    RotationResetCommand(f);
    const std::array<float, 4> identity{0, 0, 0, 1};
    Check(!std::memcmp(f.first.bones[0].rotQuat, identity.data(), sizeof(identity)) &&
        f.first.m->undoState[0] == (cursor + 1) % 30 && !f.history.UndoCount(),
        "unbound menu reset retains the old ring and matches the bound identity quaternion exactly");
}
}
void NativeRotationResetChecks() {
    RotationResetHistoryChecks(); RotationResetGuardChecks(); RotationResetFailureChecks(); RotationResetLegacyChecks();
}
