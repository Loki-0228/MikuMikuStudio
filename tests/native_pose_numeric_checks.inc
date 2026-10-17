namespace {
void NumericPoseHistoryChecks() {
    PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.first.m->selectedBone = 1; f.first.bones[1].parent = 0;
    for (int axis = 0; axis < 3; ++axis) {
        f.first.bones[0].position[axis] = float(axis + 2);
        f.first.bones[1].position[axis] = float(axis + 5);
        f.first.bones[1].trans[axis] = float(axis + 7);
    }
    bone_scale::SetPose(f.first.raw(), 1, Scale(3));
    const auto before = f.first.bones[1], other = f.first.bones[0];
    const auto ring = PoseRingDigest(f.first); const auto selection = f.first.selected;
    plugins::BoneNumericEdit edit;
    Check(edit.Begin(*f.app, f.history) == MMS_OK && edit.Begin(*f.app, f.history) == MMS_E_BUSY,
        "numeric session captures once without owning the journal gate");
    const auto original = edit.Values();
    Check(original[0] == 10 && original[1] == 11 && original[2] == 12,
        "numeric dialog exposes local coordinates relative to parent bind pose");
    Check(f.history.Enter() == MMS_OK, "numeric session allows other work during modal input"); f.history.Leave();
    auto input = original; input[0] = 15; input[5] = 90;
    Check(edit.Apply(input) == MMS_OK && edit.Apply(input) == MMS_E_NOT_READY && f.history.UndoCount() == 1,
        "numeric pose publishes one step and consumes the session");
    Check(f.first.bones[1].trans[0] == 12 && f.first.bones[1].trans[1] == before.trans[1] &&
        f.first.bones[1].trans[2] == before.trans[2] && f.first.dirty[1] == 1 &&
        bone_scale::Pose(f.first.raw(), 1) == Scale(3), "numeric input converts only changed coordinates and preserves pose scale");
    d3dx::D3DXMATRIXF matrix{}; d3dx::Get().matrixRotationQuaternion(&matrix, f.first.bones[1].rotQuat);
    Check(std::fabs(matrix.m[0][1] + 1) < 1e-5f && !std::memcmp(&other, &f.first.bones[0], sizeof(other)) &&
        f.first.selected == selection && f.SelectionUnchanged() && PoseRingDigest(f.first) == ring,
        "numeric rotation uses legacy Euler sign while preserving all other bones selection and ring");
    const auto after = f.first.bones[1];
    f.app->SelectedModelSlot() = 1; f.Commit(panel::kReadoutPosYEdit, "6"); f.app->CurrentFrame() = 27;
    Check(f.history.History(false) == MMS_OK && f.history.History(false) == MMS_OK &&
        !std::memcmp(f.first.bones[1].trans, before.trans, sizeof(before.trans)) &&
        !std::memcmp(f.first.bones[1].rotQuat, before.rotQuat, sizeof(before.rotQuat)) && !f.first.dirty[1],
        "numeric edit undoes in shared cross-model order with exact author values and dirty flags");
    Check(f.history.History(true) == MMS_OK && f.history.History(true) == MMS_OK &&
        !std::memcmp(f.first.bones[1].trans, after.trans, sizeof(after.trans)) &&
        !std::memcmp(f.first.bones[1].rotQuat, after.rotQuat, sizeof(after.rotQuat)) &&
        f.second.bones[0].trans[1] == 6 && f.app->CurrentFrame() == 27 && PoseRingDigest(f.first) == ring,
        "numeric edit redo does not depend on current model or playhead and never writes old ring");
}
void NumericPoseNoopChecks() {
    PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Commit(panel::kReadoutPosXEdit, "5"); Check(f.history.History(false) == MMS_OK, "seed numeric no-op redo");
    f.first.bones[0].rotQuat[1] = .6f; f.first.bones[0].rotQuat[3] = .8f;
    const auto before = f.first.Digest();
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    plugins::BoneNumericEdit edit;
    Check(edit.Begin(*f.app, f.history) == MMS_OK && edit.Apply(edit.Values()) == MMS_OK &&
        f.first.Digest() == before && f.history.RedoCount() == 1 &&
        !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
        "unchanged Euler dialog values retain exact quaternion and redo without rounding author data");
    { plugins::BoneNumericEdit cancelled; Check(cancelled.Begin(*f.app, f.history) == MMS_OK, "capture cancellable numeric input"); }
    Check(f.first.Digest() == before && f.history.RedoCount() == 1,
        "destroying uncommitted numeric session changes neither data nor redo");
    plugins::BoneNumericEdit invalid; Check(invalid.Begin(*f.app, f.history) == MMS_OK, "capture invalid numeric input case");
    auto input = invalid.Values(); input[2] = std::numeric_limits<float>::infinity();
    Check(invalid.Apply(input) == MMS_E_ARGUMENT && f.first.Digest() == before && f.history.RedoCount() == 1,
        "non-finite input rejects all fields without truncating redo");
}
void NumericPoseStaleChecks() {
    // Every mutation takes place while no pose transaction is reserved.
    for (int change = 0; change < 12; ++change) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        plugins::BoneNumericEdit edit; Check(edit.Begin(*f.app, f.history) == MMS_OK, "capture numeric stale boundary");
        auto input = edit.Values(); input[0] += 8;
        if (change == 0) f.app->SelectedModelSlot() = 1;
        if (change == 1) f.first.m->selectedBone = 1;
        if (change == 2) f.app->CurrentFrame() = 4;
        if (change == 3) f.first.bones[0].trans[0] = 3;
        if (change == 4) f.first.bones[0].rotQuat[2] = .1f;
        if (change == 5) f.first.dirty[0] = 1;
        if (change == 6) bone_scale::SetPose(f.first.raw(), 0, Scale(2));
        if (change == 7) { f.first.bones[0].parent = 1; f.first.bones[1].position[0] = 2; }
        if (change == 8) { plugins::ForgetSceneObject(f.first.raw()); plugins::SceneObjectIdentity(f.first.raw()); }
        if (change == 9) Check(f.history.Reset() == MMS_OK, "reset journal during numeric modal input");
        if (change == 10) f.first.m->boneTable = nullptr;
        if (change == 11) { f.Commit(panel::kReadoutPosYEdit, "9"); }
        const auto before = f.first.Digest(); const auto count = f.history.UndoCount();
        Check(edit.Apply(input) == MMS_E_STALE && f.first.Digest() == before && f.history.UndoCount() == count,
            "numeric apply rejects changed identity frame selection pose structure or journal without stale writes");
        if (change == 10) f.first.m->boneTable = f.first.bones.data();
    }
    PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    plugins::BoneNumericEdit busy; Check(busy.Begin(*f.app, f.history) == MMS_OK, "capture numeric busy boundary");
    auto input = busy.Values(); input[0] = 10;
    Check(f.history.Enter() == MMS_OK, "reserve journal while numeric dialog is open");
    Check(busy.Apply(input) == MMS_E_BUSY && !f.first.bones[0].trans[0] && !f.history.UndoCount(),
        "numeric apply respects another active transaction"); f.history.Leave();
    plugins::BoneNumericEdit rebound; Check(rebound.Begin(*f.app, f.history) == MMS_OK, "capture numeric binding boundary");
    plugins::StorageStore storage; plugins::SharedHistory replacement(storage);
    { plugins::NativePoseTestBinding other(*f.app, replacement);
      Check(rebound.Apply(input) == MMS_E_STALE && !replacement.UndoCount(), "numeric session cannot write into a replacement binding"); }
}
void NumericPoseFailureChecks() {
    PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Commit(panel::kReadoutPosYEdit, "3"); Check(f.history.History(false) == MMS_OK, "seed numeric failure redo branch");
    const auto before = f.first.Digest();
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = f.app->PhysicsResetPending() = 0;
    unsigned failures = 0; bool succeeded = false; PoseAxisMessageProbe messages(f);
    for (long limit = 0; limit < 256; ++limit) {
        plugins::BoneNumericEdit edit; failAfter = limit;
        auto result = edit.Begin(*f.app, f.history);
        if (result == MMS_OK) { auto input = edit.Values(); input[0] = 12; input[3] = 30; result = edit.Apply(input); }
        failAfter = -1;
        if (result == MMS_OK) { succeeded = true; break; }
        ++failures;
        Check(f.first.Digest() == before && f.history.RedoCount() == 1 && !f.history.UndoCount() &&
            !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && !f.app->PhysicsResetPending(),
            "numeric allocation failure restores all authored data and journal redo before reporting failure");
        Check(f.history.Enter() == MMS_OK, "numeric failure releases pose reservation"); f.history.Leave();
    }
    Check(succeeded && failures > 8 && !messages.observedBusy && f.history.UndoCount() == 1 && !f.history.RedoCount(),
        "numeric allocation sweep reaches adoption then publishes with UI notifications outside reservation");
    Check(f.history.History(false) == MMS_OK && f.first.Digest() == before,
        "successful numeric edit following allocation failures is exactly undoable");
}
}
void NativePoseNumericChecks() {
    NumericPoseHistoryChecks(); NumericPoseNoopChecks(); NumericPoseStaleChecks(); NumericPoseFailureChecks();
}
