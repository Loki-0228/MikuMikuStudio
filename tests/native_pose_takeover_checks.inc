// MAINTENANCE: regression-keep. Bound axis buttons and menu 302 take over a
// mode-3 rigid bone even when its authored reset value is already zero.
// Hidden native EDITs and real D3DX math only; no renderer or simulation.
namespace {
using TakeoverMode = plugins::SharedHistory::RecordingMode;
void TakeoverDegrees(MMDApp& app, int zeroAxis) {
    PoseAxisDegrees(app);
    if (zeroAxis == 0) app.BoneRotationEditDegreesX() = 0;
    if (zeroAxis == 1) app.BoneRotationEditDegreesY() = 0;
    if (zeroAxis == 2) app.BoneRotationEditDegreesZ() = 0;
}
std::array<float, 4> TakeoverLegacyRotation(int axis, bool floating) {
    // The preserved unbound command is the numerical oracle, including the
    // main/separate window constants and intermediate float rounding.
    PoseNumericFixture legacy;
    TakeoverDegrees(*legacy.app, axis);
    PoseAxisCommand(legacy, AxisKind::Rotation, axis, floating);
    std::array<float, 4> result{};
    std::memcpy(result.data(), legacy.first.bones[0].rotQuat, sizeof result);
    return result;
}
void TakeoverSeed(PoseNumericFixture& f, int axis) {
    PhysicsReadoutRig(f);
    auto& bone = f.first.bones[1];
    std::fill(std::begin(bone.trans), std::end(bone.trans), 0.f);
    std::fill(std::begin(bone.rotQuat), std::end(bone.rotQuat), 0.f);
    bone.rotQuat[3] = 1;
    TakeoverDegrees(*f.app, axis);
    Check(bone_scale::SetPose(f.first.raw(), 1, Scale(2)) && !bone_scale::TransformsApplied(f.first.raw()),
        "takeover uses an actual unapplied scale guard instead of renderer-dependent full readout refresh");
}
void TakeoverCommand(PoseNumericFixture& f, RotationDraftPanel& panel, int kind, int axis) {
    if (kind == 0) PoseAxisCommand(f, AxisKind::Position, axis, panel.base.floating);
    else panel.Command(kind == 2, axis);
}
HWND TakeoverField(RotationDraftPanel& panel, int kind, int axis) {
    return kind == 0 ? panel.base.Field(axis) : panel.Field(axis);
}
void TakeoverDraft(RotationDraftPanel& panel, int kind, int axis) {
    if (kind == 0) panel.base.Draft(axis, "99.25");
    else panel.Draft(axis, "99.25");
}
void TakeoverLoseFocus(RotationDraftPanel& panel, int kind, int axis) {
    const HWND field = TakeoverField(panel, kind, axis);
    Check(GetFocus() == field, "takeover field still owns actual keyboard focus immediately before focus loss");
    if (kind == 0) panel.base.LoseFocus(); else panel.LoseFocus(axis);
    Check(GetFocus() == panel.base.owner && GetFocus() != field,
        "takeover focus loss actually reaches the owned panel and leaves the target EDIT");
}
void TakeoverSuccessChecks() {
    for (auto mode : {TakeoverMode::AllOperations, TakeoverMode::ExplicitOnly})
        for (bool floating : {false, true}) for (int kind = 0; kind < 3; ++kind)
            for (int axis = 0; axis < (kind == 2 ? 1 : 3); ++axis)
                for (int same = 0; same < (kind == 1 ? 2 : 1); ++same) {
                    const auto expectedRotation = kind == 1 ? TakeoverLegacyRotation(axis, floating) :
                        std::array<float, 4>{0, 0, 0, 1};
                    PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
                    RotationDraftPanel panel(f, floating);
                    Check(f.history.SetMode(mode) == MMS_OK, "configure takeover history policy");
                    TakeoverSeed(f, axis);
                    if (kind == 1 && same) std::memcpy(f.first.bones[1].rotQuat, expectedRotation.data(), sizeof expectedRotation);
                    TakeoverDraft(panel, kind, axis);
                    const auto before = f.first.bones;
                    const auto other = f.second.Digest();
                    const auto ring = PoseRingDigest(f.first), secondRing = PoseRingDigest(f.second);
                    const auto flags = f.first.dirty, selected = f.first.selected;
                    const auto scale = bone_scale::Pose(f.first.raw(), 1);
                    TakeoverCommand(f, panel, kind, axis);
                    const auto count = mode == TakeoverMode::AllOperations ? 1u : 0u;
                    Check(f.first.dirty[1] == 1 && f.history.UndoCount() == count && !f.history.RedoCount(),
                        "zero-valued physical axis and identity menu reset take ownership with the existing recording policy");
                    if (kind != 0) Check(!std::memcmp(f.first.bones[1].rotQuat, expectedRotation.data(), sizeof expectedRotation),
                        "zero-cache physical rotation still uses the exact main or separate legacy RotationTarget result");
                    auto expectedBones = before;
                    if (kind != 0) std::memcpy(expectedBones[1].rotQuat, expectedRotation.data(), sizeof expectedRotation);
                    Check(!std::memcmp(f.first.bones.data(), expectedBones.data(), sizeof expectedBones) &&
                        f.first.dirty[0] == flags[0] && f.first.selected == selected && f.second.Digest() == other &&
                        PoseRingDigest(f.first) == ring && PoseRingDigest(f.second) == secondRing && f.SelectionUnchanged() &&
                        bone_scale::Pose(f.first.raw(), 1) == scale && !bone_scale::TransformsApplied(f.first.raw()) &&
                        f.app->CurrentFrame() == 37 && !f.app->PhysicsResetPending(),
                        "takeover preserves ikBackup all unrelated bone fields scale selection frame and both legacy rings");
                    const HWND field = TakeoverField(panel, kind, axis);
                    Check(!RotationDraftPanel::Modified(field) && f.app->SceneModified() && f.app->RecordedFrameCountByte1(),
                        "successful physical takeover cancels the targeted draft and publishes project dirty state");
                    if (kind == 0) {
                        // PhysicsReadoutRig's child-minus-parent bind offsets
                        // are independently known; backup values differ.
                        const char* positions[3] = {"3.00", "4.00", "5.00"};
                        Check(panel.base.Text(axis, positions[axis]),
                            "zero-author position takeover displays its authored bind offset rather than the physical backup");
                    } else {
                        const auto degrees = RotationDraftQuaternionDegrees(expectedRotation.data());
                        Check(PoseAxisReadDegrees(*f.app) == degrees,
                            "takeover derived Euler state matches the legacy target through the independent pre-extraction readout oracle");
                        if (kind == 2) for (int component = 0; component < 3; ++component)
                            RotationDraftCaption(panel, component, degrees[component]);
                        else RotationDraftCaption(panel, axis, degrees[axis]);
                    }
                    const auto after = f.first.Digest();
                    TakeoverLoseFocus(panel, kind, axis);
                    Check(f.first.Digest() == after && f.history.UndoCount() == count,
                        "actual focus loss cannot reapply the discarded physical takeover draft");
                    if (count) {
                        Check(f.history.History(false) == MMS_OK && f.first.dirty == flags &&
                            !std::memcmp(f.first.bones.data(), before.data(), sizeof before),
                            "takeover undo restores dirty zero exact author pose and unchanged physical backup");
                        Check(f.history.History(true) == MMS_OK && f.first.dirty[1] == 1 &&
                            !std::memcmp(f.first.bones.data(), expectedBones.data(), sizeof expectedBones),
                            "takeover redo restores dirty one and exact rotation target without touching physical backup");
                    }
                }
}
void TakeoverExcludedNoopChecks() {
    for (auto mode : {TakeoverMode::AllOperations, TakeoverMode::ExplicitOnly})
        for (bool floating : {false, true}) for (int excluded = 0; excluded < 3; ++excluded)
            for (int kind = 0; kind < 3; ++kind) for (int axis = 0; axis < (kind == 2 ? 1 : 3); ++axis) {
                PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
                RotationDraftPanel panel(f, floating);
                f.first.bones[0].trans[0] = 5;
                PoseAxisCommand(f, AxisKind::Position, 0, floating);
                Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1,
                    "excluded takeover begins with a real redo branch");
                Check(f.history.SetMode(mode) == MMS_OK, "configure excluded takeover history policy");
                TakeoverSeed(f, axis);
                if (excluded == 0) f.first.bones[1].hasRigidBody = 0;
                if (excluded == 1) f.app->PlaybackPhysicsMode() = 2;
                if (excluded == 2) f.first.dirty[1] = 2;
                TakeoverDraft(panel, kind, axis);
                const auto before = f.first.Digest(), ring = PoseRingDigest(f.first);
                f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
                failAfter = 0; TakeoverCommand(f, panel, kind, axis); failAfter = -1;
                Check(f.first.Digest() == before && PoseRingDigest(f.first) == ring &&
                    !f.history.UndoCount() && f.history.RedoCount() == 1 &&
                    !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() &&
                    !RotationDraftPanel::Modified(TakeoverField(panel, kind, axis)),
                    "nonrigid mode-two and already-edited zero resets remain allocation-free noops preserving redo");
                TakeoverLoseFocus(panel, kind, axis);
                Check(f.first.Digest() == before && f.history.RedoCount() == 1,
                    "excluded zero reset cannot commit a stale draft after real focus loss");
            }
}
void TakeoverFailureChecks() {
    for (bool floating : {false, true}) for (int kind = 0; kind < 3; ++kind) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        RotationDraftPanel panel(f, floating);
        f.first.bones[0].trans[0] = 5; PoseAxisCommand(f, AxisKind::Position, 0, floating);
        Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "takeover failure starts with redo");
        TakeoverSeed(f, 0); TakeoverDraft(panel, kind, 0);
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first), other = f.second.Digest();
        const auto angles = PoseAxisReadDegrees(*f.app);
        const HWND field = TakeoverField(panel, kind, 0);
        Check(f.history.Enter() == MMS_OK, "reserve journal before dirty-only physical takeover");
        TakeoverCommand(f, panel, kind, 0); f.history.Leave();
        Check(f.first.Digest() == before && f.history.RedoCount() == 1 && RotationDraftPanel::Modified(field),
            "busy journal rejects zero-valued takeover before clearing draft or changing physics ownership");
        unsigned failures = 0; bool succeeded = false;
        for (long limit = 0; limit < 512; ++limit) {
            failAfter = limit; TakeoverCommand(f, panel, kind, 0); failAfter = -1;
            if (f.first.dirty[1] == 1) { succeeded = true; break; }
            ++failures;
            Check(f.first.Digest() == before && f.second.Digest() == other && PoseRingDigest(f.first) == ring &&
                PoseAxisReadDegrees(*f.app) == angles && !f.history.UndoCount() && f.history.RedoCount() == 1 &&
                RotationDraftPanel::Text(field, "99.25") && RotationDraftPanel::Modified(field),
                "failed takeover preparation retains pose physical backup dirty flag redo derived angles and draft");
        }
        Check(succeeded && failures > 3 && f.history.UndoCount() == 1 && !f.history.RedoCount(),
            "allocation sweep eventually publishes one physical takeover after exercising atomic failure paths");
    }
}
void NativePoseTakeoverChecks() {
    Check(d3dx::Get().Load(), "physical takeover regression uses actual D3DX math");
    TakeoverSuccessChecks(); TakeoverExcludedNoopChecks(); TakeoverFailureChecks();
}
}
