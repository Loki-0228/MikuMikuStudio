// MAINTENANCE: regression-keep; actual physics-mode inspector and reset drafts.
// Included after PositionDraftPanel. Only the fixture's hidden EDIT controls
// receive messages; no renderer, simulation, or global input is involved.
namespace {
void PhysicsReadoutRig(PoseNumericFixture& f) {
    f.app->PlaybackPhysicsMode() = 3; f.app->CurrentFrame() = 37;
    f.first.m->selectedBone = 1; f.first.selected[0] = 0; f.first.selected[1] = 1;
    auto& parent = f.first.bones[0]; auto& bone = f.first.bones[1];
    bone.parent = 0; bone.hasRigidBody = 1;
    const float parentPosition[3] = {2.f, 3.f, 4.f}, position[3] = {5.f, 7.f, 9.f};
    const float authored[3] = {1.25f, -2.5f, 3.75f}, backup[3] = {21.5f, 22.25f, -23.5f};
    std::memcpy(parent.position, parentPosition, sizeof parentPosition);
    std::memcpy(bone.position, position, sizeof position);
    std::memcpy(bone.trans, authored, sizeof authored);
    std::memcpy(bone.ikBackup, backup, sizeof backup);
    // Two independently known pure-X rotations make quaternion-source errors
    // visible in both the actual rotation EDIT and the derived app readout.
    constexpr double pi = 3.14159265358979323846;
    bone.rotQuat[0] = float(std::sin(15. * pi / 180.));
    bone.rotQuat[1] = bone.rotQuat[2] = 0;
    bone.rotQuat[3] = float(std::cos(15. * pi / 180.));
    bone.ikBackup[3] = float(std::sin(-25. * pi / 180.));
    bone.ikBackup[4] = bone.ikBackup[5] = 0;
    bone.ikBackup[6] = float(std::cos(-25. * pi / 180.));
    f.first.dirty[0] = 1; f.first.dirty[1] = 0;
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
}
void PhysicsRotationFields(PositionDraftPanel& panel) {
    if (!panel.floating) return;
    for (int axis = 0; axis < 3; ++axis)
        Check(CreateWindowExA(0, "EDIT", "0", WS_CHILD, axis * 90, 24, 80, 20, panel.owner,
            reinterpret_cast<HMENU>(INT_PTR(panel::kReadoutRotXEdit + axis)), GetModuleHandleW(nullptr), nullptr) != nullptr,
            "create real separate rotation readout EDIT");
}
bool PhysicsFieldText(HWND owner, int id, const char* expected) {
    char actual[64]{}; const HWND field = GetDlgItem(owner, id);
    return field && GetWindowTextA(field, actual, sizeof actual) >= 0 && !std::strcmp(actual, expected);
}
void PhysicsReadoutExpected(PositionDraftPanel& panel, bool authored, bool resetX = false) {
    const char* authoredPosition[3] = {resetX ? "3.00" : "4.25", "1.50", "8.75"};
    const char* backupPosition[3] = {"24.50", "26.25", "-18.50"};
    for (int axis = 0; axis < 3; ++axis)
        Check(panel.Text(axis, authored ? authoredPosition[axis] : backupPosition[axis]),
            "real inspector position EDIT uses the selected pose source plus the parent bind offset");
    Check(PhysicsFieldText(panel.owner, panel::kReadoutRotXEdit, authored ? "30.0" : "-50.0") &&
        PhysicsFieldText(panel.owner, panel::kReadoutRotYEdit, "0.0") &&
        PhysicsFieldText(panel.owner, panel::kReadoutRotZEdit, "0.0"),
        "real inspector rotation EDITs use the same authored or backup quaternion source");
    const auto degrees = PoseAxisReadDegrees(*panel.fixture.app);
    Check(std::abs(degrees[0] - (authored ? 30.f : -50.f)) < .001f &&
        std::abs(degrees[1]) < .001f && std::abs(degrees[2]) < .001f,
        "derived rotation state matches the displayed source without changing its quaternion");
}
void PosePhysicsPureReadoutChecks() {
    Check(d3dx::Get().Load(), "physics readout regression requires the actual D3DX quaternion math runtime");
    struct Case { int mode; bool rigid; unsigned char edited; bool nullFlags; bool authored; };
    const Case cases[] = {
        {3, true, 2, false, true},   // Any nonzero edit flag, not only one.
        {3, true, 0, false, false},
        {3, true, 2, true, false},  // Absent flags retain the safe backup readout.
        {2, true, 0, false, true},
        {3, false, 0, false, true}
    };
    for (bool floating : {false, true}) for (const auto& test : cases) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        PositionDraftPanel panel(f, floating); PhysicsRotationFields(panel);
        // Keep a real redo entry, so a readout accidentally publishing history
        // cannot hide behind an initially empty history stack.
        f.first.bones[0].trans[0] = 9.5f;
        PoseAxisCommand(f, AxisKind::Position, 0, floating);
        Check(f.history.UndoCount() == 1 && f.history.History(false) == MMS_OK && f.history.RedoCount() == 1,
            "pure physics readout starts with a genuine retained redo entry");
        PhysicsReadoutRig(f);
        f.app->PlaybackPhysicsMode() = test.mode; f.first.bones[1].hasRigidBody = test.rigid;
        f.first.dirty[1] = test.edited;
        // The parent flag stays nonzero, catching accidental lookup of bone 0
        // instead of selectedBone. Restore the nullable table before teardown.
        struct RestoreFlags {
            Fixture& fixture;
            ~RestoreFlags() { fixture.m->bonePhysicsState = fixture.dirty.data(); }
        } restore{f.first};
        if (test.nullFlags) f.first.m->bonePhysicsState = nullptr;
        panel.LoseFocus();
        for (int axis = 0; axis < 3; ++axis) {
            for (int base : {panel::kReadoutPosXEdit, panel::kReadoutRotXEdit}) {
                const auto field = GetDlgItem(panel.owner, base + axis);
                SetWindowTextA(field, "stale readout"); SendMessageA(field, EM_SETMODIFY, FALSE, 0);
                if (floating) {
                    const auto inactive = f.Edit(base + axis);
                    SetWindowTextA(inactive, "inactive main"); SendMessageA(inactive, EM_SETMODIFY, FALSE, 0);
                }
            }
        }
        const auto before = f.first.Digest(), other = f.second.Digest();
        const auto ring = PoseRingDigest(f.first), otherRing = PoseRingDigest(f.second);
        const auto selected = f.first.selected;
        const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
        std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        PostViewRefresh(f.app.get());
        PhysicsReadoutExpected(panel, test.authored);
        f.history.Revision(after);
        Check(f.first.Digest() == before && f.second.Digest() == other && f.first.selected == selected &&
            PoseRingDigest(f.first) == ring && PoseRingDigest(f.second) == otherRing && f.SelectionUnchanged(),
            "pure inspector refresh preserves all fixture author data keys flags selection and both legacy history rings");
        Check(f.history.UndoCount() == undo && f.history.RedoCount() == redo && revision == after &&
            f.app->CurrentFrame() == 37 && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
            "pure inspector refresh preserves common history revision redo playback head and dirty indicators");
        if (floating) for (int axis = 0; axis < 3; ++axis)
            Check(PhysicsFieldText(f.app->MainWindow(), panel::kReadoutPosXEdit + axis, "inactive main") &&
                PhysicsFieldText(f.app->MainWindow(), panel::kReadoutRotXEdit + axis, "inactive main"),
                "separate inspector refresh leaves inactive main readout EDITs untouched");
        Check(!IsWindowVisible(f.app->MainWindow()) && !IsWindowVisible(panel.owner),
            "pure readout probes keep both fixture windows hidden");
    }
}
void PosePhysicsScaledResetDraftChecks() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool floating : {false, true}) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        PositionDraftPanel panel(f, floating); PhysicsRotationFields(panel);
        Check(f.history.SetMode(mode) == MMS_OK, "configure physics reset with pending scale history policy");
        PhysicsReadoutRig(f); panel.LoseFocus();
        PostViewRefresh(f.app.get()); PhysicsReadoutExpected(panel, false);
        // A real pending nonidentity scale makes RefreshBoundPoseReadout skip
        // PostViewRefresh. Otherwise its later publication can overwrite a
        // wrong ReplacePositionDraft caption and hide this second defect.
        Check(bone_scale::SetPose(f.first.raw(), 1, Scale(2)) &&
            !bone_scale::TransformsApplied(f.first.raw()), "seed actual unapplied nonidentity scale on the selected physical bone");
        const auto rotationField = GetDlgItem(panel.owner, panel::kReadoutRotXEdit);
        SetWindowTextA(rotationField, "guarded rotation"); SendMessageA(rotationField, EM_SETMODIFY, FALSE, 0);
        f.app->BoneRotationEditDegreesX() = 123.5f;
        const auto beforeBones = f.first.bones;
        auto resetBones = beforeBones; resetBones[1].trans[0] = 0;
        const auto other = f.second.Digest(), ring = PoseRingDigest(f.first);
        const std::vector<mdl::BoneKey> keys(f.first.m->boneKeys,
            f.first.m->boneKeys + mdl::BoneKeyCapacity(f.first.m->boneKeys));
        panel.Draft(0, "123.75"); panel.Reset(0);
        const auto count = mode == Mode::AllOperations ? 1u : 0u;
        Check(f.first.bones[1].trans[0] == 0 && f.first.dirty[1] == 1 &&
            panel.Text(0, "3.00") && !SendMessageA(panel.Field(0), EM_GETMODIFY, 0, 0),
            "physics reset draft uses authored bind-relative zero even when the real pending-scale guard prevents a later full readout refresh");
        Check(PhysicsFieldText(panel.owner, panel::kReadoutRotXEdit, "guarded rotation") &&
            f.app->BoneRotationEditDegreesX() == 123.5f && !bone_scale::TransformsApplied(f.first.raw()) &&
            bone_scale::Pose(f.first.raw(), 1) == Scale(2),
            "real readout guard preserves nonmodified rotation text derived angle and unapplied scale instead of masking the draft replacement");
        Check(f.history.UndoCount() == count && f.history.RedoCount() == 0 && GetFocus() == panel.Field(0),
            "pending-scale reset follows its history policy and keeps the real draft field focused");
        std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        panel.LoseFocus();
        Check(GetFocus() == panel.owner && GetFocus() != panel.Field(0),
            "pending-scale reset test actually moves native focus away from the draft field");
        f.history.Revision(after);
        Check(f.first.bones[1].trans[0] == 0 && f.first.dirty[1] == 1 && panel.Text(0, "3.00") &&
            f.history.UndoCount() == count && f.history.RedoCount() == 0 && revision == after,
            "actual focus loss cannot reapply the typed value or backup position or publish another history revision");
        if (count) {
            Check(f.history.History(false) == MMS_OK && f.first.bones[1].trans[0] == 1.25f && f.first.dirty[1] == 0,
                "pending-scale physics reset undo restores author position and original physics edit flag");
            Check(f.history.History(true) == MMS_OK && f.first.bones[1].trans[0] == 0 && f.first.dirty[1] == 1,
                "pending-scale physics reset redo restores zero and the edited flag");
        }
        Check(!std::memcmp(f.first.bones.data(), resetBones.data(), sizeof(resetBones)) &&
            f.second.Digest() == other && PoseRingDigest(f.first) == ring &&
            mdl::BoneKeyCapacity(f.first.m->boneKeys) == keys.size() &&
            !std::memcmp(f.first.m->boneKeys, keys.data(), keys.size() * sizeof(mdl::BoneKey)) &&
            f.app->CurrentFrame() == 37 && f.SelectionUnchanged(),
            "guarded draft reset and history change only selected displacement and flag, preserving backup transforms keys other model and playback head");
        Check(!bone_scale::TransformsApplied(f.first.raw()) && bone_scale::Pose(f.first.raw(), 1) == Scale(2) &&
            PhysicsFieldText(panel.owner, panel::kReadoutRotXEdit, "guarded rotation") &&
            f.app->BoneRotationEditDegreesX() == 123.5f,
            "focus loss and history do not evaluate the pending scale or bypass the actual readout guard");
    }
}
void PosePhysicsResetDraftChecks() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool floating : {false, true}) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        PositionDraftPanel panel(f, floating); PhysicsRotationFields(panel);
        Check(f.history.SetMode(mode) == MMS_OK, "configure physics draft reset history policy");
        PhysicsReadoutRig(f); panel.LoseFocus();
        PostViewRefresh(f.app.get()); PhysicsReadoutExpected(panel, false);
        const auto ring = PoseRingDigest(f.first), other = f.second.Digest();
        const std::vector<mdl::BoneKey> keys(f.first.m->boneKeys,
            f.first.m->boneKeys + mdl::BoneKeyCapacity(f.first.m->boneKeys));
        std::array<float, 7> backup{}; std::memcpy(backup.data(), f.first.bones[1].ikBackup, sizeof backup);
        std::array<float, 4> rotation{}; std::memcpy(rotation.data(), f.first.bones[1].rotQuat, sizeof rotation);
        panel.Draft(0, "123.75"); panel.Reset(0);
        const auto count = mode == Mode::AllOperations ? 1u : 0u;
        Check(f.first.bones[1].trans[0] == 0 && f.first.dirty[1] == 1 &&
            panel.Text(0, "3.00") && !SendMessageA(panel.Field(0), EM_GETMODIFY, 0, 0),
            "successful physical-bone reset immediately replaces the pending draft with authored bind-relative position, not old IK backup");
        Check(f.history.UndoCount() == count && f.history.RedoCount() == 0,
            "physics draft reset follows all-operations or explicit-only history policy");
        panel.LoseFocus();
        Check(GetFocus() == panel.owner && GetFocus() != panel.Field(0),
            "physics reset test actually moves native focus away from the draft field");
        Check(f.first.bones[1].trans[0] == 0 && f.first.dirty[1] == 1 && f.history.UndoCount() == count,
            "real focus loss cannot reapply the old typed displacement or physics backup after reset");
        PostViewRefresh(f.app.get()); PhysicsReadoutExpected(panel, true, true);
        if (count) {
            Check(f.history.History(false) == MMS_OK && f.first.bones[1].trans[0] == 1.25f && f.first.dirty[1] == 0,
                "physics position reset undo restores the exact author value and unedited flag");
            PostViewRefresh(f.app.get()); PhysicsReadoutExpected(panel, false);
            Check(f.history.History(true) == MMS_OK && f.first.bones[1].trans[0] == 0 && f.first.dirty[1] == 1,
                "physics position reset redo restores authored zero and edited flag");
            PostViewRefresh(f.app.get()); PhysicsReadoutExpected(panel, true, true);
        }
        Check(f.first.bones[1].trans[1] == -2.5f && f.first.bones[1].trans[2] == 3.75f &&
            !std::memcmp(f.first.bones[1].ikBackup, backup.data(), sizeof backup) &&
            !std::memcmp(f.first.bones[1].rotQuat, rotation.data(), sizeof rotation) &&
            f.second.Digest() == other && PoseRingDigest(f.first) == ring &&
            mdl::BoneKeyCapacity(f.first.m->boneKeys) == keys.size() &&
            !std::memcmp(f.first.m->boneKeys, keys.data(), keys.size() * sizeof(mdl::BoneKey)) &&
            f.app->CurrentFrame() == 37 && f.SelectionUnchanged(),
            "physics reset refresh and history keep other axes backup quaternion keys other model and playback head unchanged");
    }
    PosePhysicsScaledResetDraftChecks();
}
} // namespace
