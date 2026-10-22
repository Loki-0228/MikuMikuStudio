// R3: real 495 must record a mode-3 rigid bone's first dirty flag even when
// its authored pose already equals the reset target. No physics simulation.
// Included after the legacy reset helpers; both narrow suites call this file.
namespace {
using ResetTakeoverMode = plugins::SharedHistory::RecordingMode;
void ResetTakeoverSeed(PoseCommandFixture& f) {
    f.app->PlaybackPhysicsMode() = 3; f.app->CurrentFrame() = 37;
    f.first.m->selectedBone = 0; f.first.selected[0] = 1; f.first.selected[1] = 0;
    for (int i = 0; i < 2; ++i) {
        auto& bone = f.first.bones[i]; bone.parent = -1; bone.hasRigidBody = 1;
        for (int j = 0; j < 7; ++j) bone.ikBackup[j] = float(20 + i * 10 + j);
    }
}
bool ResetTakeoverUndo(PoseCommandFixture& f, bool bound) {
    return bound ? f.history.History(false) == MMS_OK : TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame());
}
bool ResetTakeoverRedo(PoseCommandFixture& f, bool bound) {
    return bound ? f.history.History(true) == MMS_OK : TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame());
}
std::unique_ptr<plugins::NativePoseTestBinding> ResetTakeoverBind(PoseCommandFixture& f, bool bound, ResetTakeoverMode mode) {
    Check(f.history.SetMode(mode) == MMS_OK, "set dirty-only reset recording policy");
    if (bound) return std::make_unique<plugins::NativePoseTestBinding>(*f.app, f.history);
    LegacyPoseResetUnbound(f); return {};
}
void ResetTakeoverSuccess(bool bound, ResetTakeoverMode mode) {
    for (bool floating : {false, true}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); PoseResetDraftPanel panel(f, floating);
        auto binding = ResetTakeoverBind(f, bound, mode); ResetTakeoverSeed(f);
        const auto bones = f.first.bones; const auto second = f.second.Digest();
        const auto ring = PoseRingDigest(f.first); const auto cursor = f.first.m->undoState[0];
        const auto selection = f.first.selected;
        std::vector<mdl::BoneKey> keys(f.first.m->boneKeys, f.first.m->boneKeys + mdl::BoneKeyCapacity(f.first.m->boneKeys));
        panel.Drafts(0); f.Command(495); panel.Expected(false);
        Check(f.first.dirty[0] == 1 && !f.first.dirty[1] &&
            !std::memcmp(bones.data(), f.first.bones.data(), sizeof bones) &&
            bone_scale::Identity(bone_scale::Pose(f.first.raw(), 0)),
            "zero authored rigid mode-3 reset takes ownership without changing pose or ikBackup or unselected bone");
        Check(bound ? (f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring) :
            (f.first.m->undoState[0] == (cursor + 1) % 30 && f.first.m->undoState[1] == f.first.m->undoState[0] && !f.history.UndoCount()),
            "dirty-only 495 creates exactly one explicit common-history or native-ring step");
        Check(second == f.second.Digest() && selection == f.first.selected && f.SelectionUnchanged() &&
            f.app->CurrentFrame() == 37 && !f.app->PhysicsResetPending() &&
            !std::memcmp(keys.data(), f.first.m->boneKeys, keys.size() * sizeof(mdl::BoneKey)),
            "dirty-only reset preserves other model selection frame keys and physics-reset request");
        const auto after = f.first.Digest(), afterRing = PoseRingDigest(f.first); const auto count = f.history.UndoCount();
        panel.LoseFocus(0);
        Check(after == f.first.Digest() && afterRing == PoseRingDigest(f.first) && count == f.history.UndoCount(),
            "real focus loss cannot reapply canceled dirty-only reset draft");
        Check(ResetTakeoverUndo(f, bound) && !f.first.dirty[0] && !f.first.dirty[1] &&
            !std::memcmp(bones.data(), f.first.bones.data(), sizeof bones), "dirty-only reset undo restores physics ownership and preserves ikBackup");
        Check(ResetTakeoverRedo(f, bound) && f.first.dirty[0] == 1 && !f.first.dirty[1] &&
            !std::memcmp(bones.data(), f.first.bones.data(), sizeof bones), "dirty-only reset redo restores ownership without changing author pose or ikBackup");
    }
}
void ResetTakeoverNoop(bool bound, ResetTakeoverMode mode) {
    for (int reason = 0; reason < 3; ++reason) for (bool floating : {false, true}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); PoseResetDraftPanel panel(f, floating);
        auto binding = ResetTakeoverBind(f, bound, mode); ResetTakeoverSeed(f);
        f.first.bones[0].trans[0] = 8; f.Command(495);
        Check(ResetTakeoverUndo(f, bound), "seed genuine redo before excluded takeover no-op");
        f.first.bones[0].trans[0] = 0;
        if (reason == 0) f.first.bones[0].hasRigidBody = 0;
        if (reason == 1) f.app->PlaybackPhysicsMode() = 2;
        if (reason == 2) f.first.dirty[0] = 1;
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto redo = f.history.RedoCount();
        panel.Drafts(3); failAfter = 0; f.Command(495); failAfter = -1; panel.Expected(false);
        Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && redo == f.history.RedoCount() && !f.history.UndoCount(),
            "nonrigid mode-2 and already-owned zero resets stay allocation-free no-ops preserving redo");
        panel.LoseFocus(3); Check(before == f.first.Digest(), "excluded takeover no-op clears real drafts without reauthoring on blur");
        Check(ResetTakeoverRedo(f, bound) && f.first.dirty[0] == 1 && LegacyPoseResetZero(f.first, 0),
            "redo remains executable after excluded takeover no-op");
    }
}
// Four real model rows let a single command contain all three selected kinds
// and one unselected rigid row. Restore borrowed fixture pointers before exit.
struct ResetTakeoverRows {
    PoseCommandFixture& f;
    std::array<mdl::BoneRecord, 4> bones{};
    std::array<unsigned char, 4> dirty{}, selected{1, 1, 1, 0};
    std::array<std::uint32_t, 4> rows{};
    std::uint32_t* oldRows;
    explicit ResetTakeoverRows(PoseCommandFixture& fixture) : f(fixture), oldRows(f.first.m->boneKeyIndices) {
        for (int i = 0; i < 4; ++i) {
            bones[i].parent = -1; bones[i].rotQuat[3] = 1;
            for (int j = 0; j < 4; ++j) bones[i].matInit[j * 5] = 1;
            for (int j = 0; j < 7; ++j) bones[i].ikBackup[j] = float(30 + i * 10 + j);
        }
        bones[1].hasRigidBody = bones[3].hasRigidBody = 1; bones[2].trans[1] = 9;
        f.first.m->boneCount = 4; f.first.m->boneTable = bones.data(); f.first.m->bonePhysicsState = dirty.data();
        f.first.m->boneSelection = selected.data(); f.first.m->boneKeyIndices = rows.data();
    }
    ~ResetTakeoverRows() {
        f.first.m->boneCount = 2; f.first.m->boneTable = f.first.bones.data(); f.first.m->bonePhysicsState = f.first.dirty.data();
        f.first.m->boneSelection = f.first.selected.data(); f.first.m->boneKeyIndices = oldRows;
    }
};
void ResetTakeoverMixed(bool bound, ResetTakeoverMode mode) {
    for (bool floating : {false, true}) for (int shown : {0, 1, 2}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); ResetTakeoverRows rows(f); PoseResetDraftPanel panel(f, floating);
        auto binding = ResetTakeoverBind(f, bound, mode); f.app->PlaybackPhysicsMode() = 3; f.app->CurrentFrame() = 37; f.first.m->selectedBone = shown;
        Check(bone_scale::SetPose(f.first.raw(), 2, CommandScale()), "mixed takeover includes a selected authored scale change");
        const auto before = rows.bones; const auto scales = bone_scale::PoseValues(f.first.raw());
        const auto second = f.second.Digest(), ring = PoseRingDigest(f.first); const auto cursor = f.first.m->undoState[0];
        panel.Drafts(6); f.Command(495); panel.Expected(false);
        Check(rows.dirty == std::array<unsigned char, 4>{0, 1, 1, 0} && rows.bones[2].trans[1] == 0 &&
            bone_scale::Identity(bone_scale::Pose(f.first.raw(), 2)) &&
            !std::memcmp(&before[0], &rows.bones[0], sizeof before[0]) && !std::memcmp(&before[1], &rows.bones[1], sizeof before[1]) &&
            !std::memcmp(&before[3], &rows.bones[3], sizeof before[3]) &&
            !std::memcmp(before[2].ikBackup, rows.bones[2].ikBackup, sizeof before[2].ikBackup),
            "one mixed reset distinguishes ordinary no-op physics takeover authored change and unselected rigid row");
        Check(second == f.second.Digest() && rows.selected == std::array<unsigned char, 4>{1, 1, 1, 0} &&
            (bound ? f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring :
                !f.history.UndoCount() && f.first.m->undoState[0] == (cursor + 1) % 30), "mixed takeover is one history step and preserves model and selection");
        const auto after = rows.bones; const auto flags = rows.dirty; panel.LoseFocus(6);
        Check(!std::memcmp(after.data(), rows.bones.data(), sizeof after) && flags == rows.dirty, "mixed current selected drafts do not return on actual blur");
        Check(ResetTakeoverUndo(f, bound) && rows.dirty == std::array<unsigned char, 4>{} &&
            !std::memcmp(before.data(), rows.bones.data(), sizeof before) && scales == bone_scale::PoseValues(f.first.raw()),
            "single mixed undo restores authored scale and dirty-only ownership independently");
        Check(ResetTakeoverRedo(f, bound) && rows.dirty == flags && !std::memcmp(after.data(), rows.bones.data(), sizeof after),
            "single mixed redo preserves ordinary no-op and unselected ownership");
    }
}
void ResetTakeoverFailures(bool bound, ResetTakeoverMode mode) {
    for (bool floating : {false, true}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); PoseResetDraftPanel panel(f, floating);
        auto binding = ResetTakeoverBind(f, bound, mode); ResetTakeoverSeed(f);
        f.first.selected[1] = 1; f.first.bones[1].hasRigidBody = 0; f.first.bones[1].trans[0] = 9;
        panel.Drafts(4); const auto before = f.first.Digest(), ring = PoseRingDigest(f.first), second = f.second.Digest();
        const auto cursor = f.first.m->undoState[0]; const auto degrees = PoseResetDraftAngles(*f.app);
        unsigned failures = 0; bool succeeded = false;
        // Cold native scale storage includes 183 MSVC empty-map sentinel allocations.
        for (long limit = 0; limit < 512; ++limit) {
            failAfter = limit; f.Command(495); failAfter = -1;
            if (bound ? f.history.UndoCount() != 0 : f.first.m->undoState[0] != cursor) { succeeded = true; break; }
            ++failures;
            Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && second == f.second.Digest() &&
                !bone_scale::HasPoseStorage(f.first.raw()) && panel.DraftsPreserved() && degrees == PoseResetDraftAngles(*f.app) &&
                !f.history.UndoCount() && !f.history.RedoCount() && !f.app->SceneModified() && !f.app->PhysicsResetPending(),
                "failed mixed reset cannot partially claim physics ownership change author data or cancel drafts");
        }
        Check(succeeded && failures > 3 && f.first.dirty[0] == 1 && f.first.dirty[1] == 1 &&
            LegacyPoseResetZero(f.first, 0) && LegacyPoseResetZero(f.first, 1), "allocation sweep eventually commits dirty-only and authored reset together");
        panel.Expected(false);
        Check(ResetTakeoverUndo(f, bound) && !f.first.dirty[0] && !f.first.dirty[1] && f.first.bones[1].trans[0] == 9,
            "first successful combined reset after allocation failures remains undoable");
    }
}
void ResetTakeoverGuards(bool bound, ResetTakeoverMode mode) {
    for (int reason = 0; reason < (bound ? 3 : 1); ++reason) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); PoseResetDraftPanel panel(f, false);
        auto binding = ResetTakeoverBind(f, bound, mode); ResetTakeoverSeed(f); panel.Drafts(0);
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first);
        std::unique_ptr<bake::ScopedUiInputGate> gate;
        if (reason == 0) gate = std::make_unique<bake::ScopedUiInputGate>();
        if (reason == 1) Check(f.history.Enter() == MMS_OK, "hold real journal busy gate before dirty-only reset");
        if (reason == 2) Check(f.history.Configure(mode, 128, 1) == MMS_OK, "limit actual journal budget before dirty-only reset");
        f.Command(495);
        Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && panel.DraftsPreserved() &&
            !f.history.UndoCount() && !f.history.RedoCount() && !f.first.dirty[0],
            "dirty-only ownership is not changed and drafts remain intact when input busy or history budget rejects reset");
        if (reason == 1) f.history.Leave();
    }
}
void ResetTakeoverSuite(bool bound, ResetTakeoverMode mode) {
    Check(d3dx::Get().Load(), "takeover reset uses real math and owned CPU host for actual focus loss");
    ResetTakeoverSuccess(bound, mode); ResetTakeoverNoop(bound, mode);
    ResetTakeoverMixed(bound, mode); ResetTakeoverFailures(bound, mode); ResetTakeoverGuards(bound, mode);
}
}
void NativeBoundPoseResetTakeoverChecks() {
    for (auto mode : {ResetTakeoverMode::AllOperations, ResetTakeoverMode::ExplicitOnly}) ResetTakeoverSuite(true, mode);
}
void NativeLegacyPoseResetTakeoverChecks() {
    ResetTakeoverSuite(false, ResetTakeoverMode::AllOperations);
}
