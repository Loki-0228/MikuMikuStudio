// Include after native_pose_scope_checks.inc; call NativePoseAxisChecks().
// Runs the real command dispatchers against PoseNumericFixture's hidden UI.
#include "../src/plugins/native_pose_axis.hpp"
#include "mikumikustudio/scene_ownership.hpp"
namespace mikumikustudio {
void CmdControl500(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void DispatchSeparateWindowCommand(MMDApp*, unsigned short);
}
namespace {
using AxisKind = plugins::NativePoseAxisKind;
void PoseAxisCommand(PoseNumericFixture& f, AxisKind kind, int axis, bool floating) {
    if (kind == AxisKind::Scale) {
        // The scale buttons use this same public function in either panel.
        if (floating) ResetSelectedBoneScaleAxis(f.app.get(), axis);
        else {
            const unsigned short ids[3] = {571, 574, 576};
            CmdControl500(f.app.get(), f.app->MainWindow(), ids[axis], BN_CLICKED);
        }
    } else {
        const auto id = static_cast<unsigned short>((kind == AxisKind::Position ? 537 : 540) + axis);
        if (floating) DispatchSeparateWindowCommand(f.app.get(), id);
        else CmdControl500(f.app.get(), f.app->MainWindow(), id, BN_CLICKED);
    }
}
void PoseAxisDegrees(MMDApp& app) {
    app.BoneRotationEditDegreesX() = 18.123456f;
    app.BoneRotationEditDegreesY() = -27.7777f;
    app.BoneRotationEditDegreesZ() = 33.125678f;
}
std::array<float, 3> PoseAxisReadDegrees(MMDApp& app) {
    return {app.BoneRotationEditDegreesX(), app.BoneRotationEditDegreesY(),
        app.BoneRotationEditDegreesZ()};
}
struct PoseAxisMessageProbe;
thread_local PoseAxisMessageProbe* poseAxisMessageProbe = nullptr;
struct PoseAxisMessageProbe {
    PoseNumericFixture& f;
    WNDPROC previous = nullptr;
    bool observedBusy = false;
    unsigned commands = 0;
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto& probe = *poseAxisMessageProbe;
        if (message == WM_COMMAND) {
            ++probe.commands;
            const auto entered = probe.f.history.Enter();
            if (entered == MMS_OK) probe.f.history.Leave();
            else probe.observedBusy = true;
        }
        return CallWindowProcA(probe.previous, window, message, wp, lp);
    }
    explicit PoseAxisMessageProbe(PoseNumericFixture& value) : f(value) {
        poseAxisMessageProbe = this;
        previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(f.app->MainWindow(), GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(Procedure)));
        Check(previous != nullptr, "axis test installs hidden parent notification probe");
    }
    ~PoseAxisMessageProbe() {
        SetWindowLongPtrA(f.app->MainWindow(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        poseAxisMessageProbe = nullptr;
    }
};
void PoseAxisPositionChecks() {
    for (bool floating : {false, true}) for (int axis = 0; axis < 3; ++axis) {
        PoseNumericFixture f;
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        PoseAxisMessageProbe messages(f);
        const std::array<float, 3> initial{1.123456f, -2.765432f, 3.141592f};
        std::memcpy(f.first.bones[0].trans, initial.data(), sizeof(initial));
        f.first.bones[1].trans[0] = 8.875f;
        f.app->CurrentFrame() = 23;
        const auto ring = PoseRingDigest(f.first), otherRing = PoseRingDigest(f.second);
        PoseAxisCommand(f, AxisKind::Position, axis, floating);
        auto expected = initial; expected[axis] = 0;
        Check(!std::memcmp(f.first.bones[0].trans, expected.data(), sizeof(expected)) &&
            f.first.bones[1].trans[0] == 8.875f && f.first.dirty[0] == 1,
            "real main/floating axis reset changes only selected bone position component");
        Check(f.history.UndoCount() == 1 && f.history.RedoCount() == 0 &&
            f.app->SceneModified() == 1 && f.app->RecordedFrameCountByte1() == 1 &&
            IsWindowEnabled(f.Edit(panel::kUndoButton)),
            "axis publication produces one common step and refreshes dirty flags and undo button");
        Check(f.history.History(false) == MMS_OK &&
            !std::memcmp(f.first.bones[0].trans, initial.data(), sizeof(initial)) && f.first.dirty[0] == 0,
            "common axis undo restores exact position and bone edit flag");
        Check(f.history.History(true) == MMS_OK &&
            !std::memcmp(f.first.bones[0].trans, expected.data(), sizeof(expected)),
            "common axis redo restores exact position");
        Check(f.app->CurrentFrame() == 23 && f.SelectionUnchanged() &&
            PoseRingDigest(f.first) == ring && PoseRingDigest(f.second) == otherRing,
            "axis reset and history preserve frame scene selection and both legacy rings");
        Check(messages.commands != 0 && !messages.observedBusy,
            "axis readout notifications occur only after the common transaction releases its gate");
    }
    PoseNumericFixture f;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.first.bones[0].trans[0] = 3.125f; f.second.bones[0].trans[1] = -4.25f;
    PoseAxisCommand(f, AxisKind::Position, 0, false);
    f.app->SelectedModelSlot() = 1;
    PoseAxisCommand(f, AxisKind::Position, 1, true);
    Check(f.history.UndoCount() == 2 && f.history.History(false) == MMS_OK &&
        f.second.bones[0].trans[1] == -4.25f && f.first.bones[0].trans[0] == 0,
        "main and floating axis commands share cross-model chronological undo");
    Check(f.history.History(false) == MMS_OK && f.first.bones[0].trans[0] == 3.125f &&
        f.app->SelectedModelSlot() == 1,
        "earlier axis edit restores another model without changing current object selection");
}
void PoseAxisRotationChecks() {
    if (!d3dx::Get().Load()) {
        std::puts("SKIP native axis rotation: D3DX math library unavailable");
        return;
    }
    for (bool floating : {false, true}) for (int axis = 0; axis < 3; ++axis) {
        std::array<float, 4> expected{};
        {
            // Compare against the real preserved legacy calculation rather
            // than a second copy of its pi constants in the test.
            PoseNumericFixture legacy;
            PoseAxisDegrees(*legacy.app);
            const auto cursor = legacy.first.m->undoState[0];
            PoseAxisCommand(legacy, AxisKind::Rotation, axis, floating);
            std::memcpy(expected.data(), legacy.first.bones[0].rotQuat, sizeof(expected));
            Check(legacy.first.m->undoState[0] == (cursor + 1) % 30 && legacy.history.UndoCount() == 0,
                "unbound main/floating rotation axis retains the legacy ring path");
        }
        PoseNumericFixture f;
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        PoseAxisMessageProbe messages(f);
        PoseAxisDegrees(*f.app);
        const auto ring = PoseRingDigest(f.first);
        PoseAxisCommand(f, AxisKind::Rotation, axis, floating);
        Check(f.history.UndoCount() == 1 &&
            !std::memcmp(f.first.bones[0].rotQuat, expected.data(), sizeof(expected)),
            "bound axis quaternion exactly matches its main/floating legacy calculation");
        Check(f.history.History(false) == MMS_OK && f.first.bones[0].rotQuat[0] == 0 &&
            f.first.bones[0].rotQuat[1] == 0 && f.first.bones[0].rotQuat[2] == 0 &&
            f.first.bones[0].rotQuat[3] == 1 && f.first.dirty[0] == 0,
            "rotation axis undo restores initial quaternion and bone flag");
        Check(f.history.History(true) == MMS_OK &&
            !std::memcmp(f.first.bones[0].rotQuat, expected.data(), sizeof(expected)),
            "rotation axis redo restores bit-exact quaternion");
        Check(PoseRingDigest(f.first) == ring && f.SelectionUnchanged() && !messages.observedBusy,
            "rotation axis publication preserves native ring and selection and sends no messages under the gate");
        // Keep nonzero other-axis Euler readouts to exercise rounding on a
        // repeated reset. A zero selected readout is already a no-op.
        auto& degree = axis == 0 ? f.app->BoneRotationEditDegreesX() :
            axis == 1 ? f.app->BoneRotationEditDegreesY() : f.app->BoneRotationEditDegreesZ();
        degree = 0;
        PoseAxisCommand(f, AxisKind::Rotation, axis, floating);
        Check(f.history.UndoCount() == 1 &&
            !std::memcmp(f.first.bones[0].rotQuat, expected.data(), sizeof(expected)),
            "repeated rotation axis reset does not round-trip the other Euler components");
    }
    std::puts("PASS native main/floating axis rotation with D3DX math");
}
void PoseAxisScaleChecks() {
    for (int mode = 0; mode < 3; ++mode) for (int axis = 0; axis < 3; ++axis) {
        std::printf("RUN axis scale mode=%d axis=%d fixture\n", mode, axis);
        PoseNumericFixture f;
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        f.app->state.coordinateSystem = static_cast<unsigned char>(mode);
        f.app->CameraPitch() = .173f; f.app->CameraYaw() = -.321f; f.app->CameraRoll() = .117f;
        bone_scale::Matrix initial;
        initial.v[0] = 2.345678f; initial.v[4] = 1.765432f; initial.v[8] = 3.123456f;
        for (auto* object : {&f.first, &f.second}) {
            object->m->physicsMode = 2;
            auto& bone = object->bones[0];
            bone.matInit[0] = .8f; bone.matInit[1] = .6f;
            bone.matInit[4] = -.6f; bone.matInit[5] = .8f;
            bone.localAxes[0] = .6f; bone.localAxes[1] = .8f; bone.localAxes[5] = 1;
            Check(bone_scale::SetPose(object->raw(), 0, initial), "scale axis fixture has anisotropic authoring");
        }
        // Obtain the reference matrix from the ordinary scale setter on an
        // equivalent second object, including local/global/view coordinates.
        f.app->SelectedModelSlot() = 1;
        Check(SetSelectedBoneScaleAxis(f.app.get(), axis, 1.0f), "ordinary scale setter prepares reference axis matrix");
        const auto expected = bone_scale::Pose(f.second.raw(), 0);
        f.app->SelectedModelSlot() = 0;
        const auto ring = PoseRingDigest(f.first), otherRing = PoseRingDigest(f.second);
        PoseAxisMessageProbe messages(f);
        PoseAxisCommand(f, AxisKind::Scale, axis, mode == 2);
        Check(f.history.UndoCount() == 1 && bone_scale::Pose(f.first.raw(), 0) == expected &&
            bone_scale::Identity(bone_scale::Pose(f.first.raw(), 1)),
            "bound scale axis matches exact ordinary matrix in local global and view modes");
        Check(f.history.History(false) == MMS_OK && bone_scale::Pose(f.first.raw(), 0) == initial && f.first.dirty[0] == 0,
            "scale axis undo restores every matrix component and bone edit flag");
        Check(f.history.History(true) == MMS_OK && bone_scale::Pose(f.first.raw(), 0) == expected,
            "scale axis redo restores exact authored matrix");
        Check(f.SelectionUnchanged() && PoseRingDigest(f.first) == ring && PoseRingDigest(f.second) == otherRing &&
            !messages.observedBusy, "scale reset leaves global and accessory selection and both native rings intact");
    }
}
void PoseAxisNoopAndGuardChecks() {
    PoseNumericFixture f;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Commit(panel::kReadoutPosXEdit, "5");
    Check(f.history.History(false) == MMS_OK, "axis no-op fixture retains redo");
    f.app->SceneModified() = 0; f.app->RecordedFrameCountByte1() = 0;
    EnableWindow(f.Edit(panel::kUndoButton), FALSE); EnableWindow(f.Edit(panel::kRedoButton), TRUE);
    const auto before = f.first.Digest(); const auto degrees = PoseAxisReadDegrees(*f.app);
    for (bool floating : {false, true}) for (auto kind : {AxisKind::Position, AxisKind::Rotation, AxisKind::Scale})
        for (int axis = 0; axis < 3; ++axis) PoseAxisCommand(f, kind, axis, floating);
    Check(f.history.UndoCount() == 0 && f.history.RedoCount() == 1 && f.first.Digest() == before &&
        f.app->SceneModified() == 0 && f.app->RecordedFrameCountByte1() == 0 &&
        !IsWindowEnabled(f.Edit(panel::kUndoButton)) && IsWindowEnabled(f.Edit(panel::kRedoButton)) &&
        PoseAxisReadDegrees(*f.app) == degrees && f.SelectionUnchanged(),
        "all no-op axis buttons preserve redo pose Euler cache dirty state selection and buttons");
    f.first.bones[0].trans[0] = 2;
    const auto guarded = f.first.Digest();
    Check(f.history.Enter() == MMS_OK, "axis guard fixture locks history");
    PoseAxisCommand(f, AxisKind::Position, 0, false);
    PoseAxisCommand(f, AxisKind::Position, 0, true);
    f.history.Leave();
    for (int condition = 0; condition < 3; ++condition) {
        if (condition == 0) f.app->PlaybackActive() = 1;
        if (condition == 1) f.app->FrameStepPlayback() = 1;
        if (condition == 2) f.app->state.recordPlaybackStartPending = 1;
        PoseAxisCommand(f, AxisKind::Position, 0, false);
        PoseAxisCommand(f, AxisKind::Position, 0, true);
        f.app->PlaybackActive() = 0; f.app->FrameStepPlayback() = 0;
        f.app->state.recordPlaybackStartPending = 0;
    }
    {
        ScopedSceneMutation mutation(*f.app);
        PoseAxisCommand(f, AxisKind::Position, 0, false);
        PoseAxisCommand(f, AxisKind::Position, 0, true);
    }
    Check(f.first.Digest() == guarded && f.history.UndoCount() == 0 && f.history.RedoCount() == 1 &&
        f.app->SceneModified() == 0 && f.SelectionUnchanged(),
        "busy playback recording and mutation guards do not fall through into native axis history");
    f.first.m->selectedBone = -1;
    PoseAxisCommand(f, AxisKind::Position, 0, false);
    PoseAxisCommand(f, AxisKind::Rotation, 0, true);
    ResetSelectedBoneScaleAxis(f.app.get(), 0);
    f.first.m->selectedBone = 0;
    auto* model = f.app->ModelSlot(0); f.app->ModelSlot(0) = nullptr;
    PoseAxisCommand(f, AxisKind::Position, 0, false);
    PoseAxisCommand(f, AxisKind::Rotation, 0, true);
    f.app->ModelSlot(0) = model;
    Check(f.first.Digest() == guarded && f.history.UndoCount() == 0 && f.history.RedoCount() == 1,
        "missing model or bone selection rejects bound axis commands without legacy dereference");
}
void PoseAxisFailureChecks() {
    const bool mathAvailable = d3dx::Get().Load();
    for (bool floating : {false, true}) for (auto kind : {AxisKind::Position, AxisKind::Rotation, AxisKind::Scale}) {
        if (kind == AxisKind::Rotation && !mathAvailable) continue;
        PoseNumericFixture f;
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        f.first.bones[0].trans[0] = 7.123456f;
        if (kind == AxisKind::Scale) Check(bone_scale::SetPose(f.first.raw(), 0, Scale(2.345678f)),
            "scale failure fixture contains a nonidentity pose");
        f.Commit(panel::kReadoutPosZEdit, "6");
        Check(f.history.History(false) == MMS_OK, "axis allocation sweep starts with redo");
        PoseAxisDegrees(*f.app);
        float values[3]{};
        Check(SelectedBoneScaleReadout(f.app.get(), values), "axis failure fixture rebuilds derived scale matrices");
        const auto initialScale = bone_scale::Pose(f.first.raw(), 0);
        const auto initialDegrees = PoseAxisReadDegrees(*f.app);
        const auto ring = PoseRingDigest(f.first), otherRing = PoseRingDigest(f.second);
        const std::array<float, 3> initialPosition{f.first.bones[0].trans[0], f.first.bones[0].trans[1], f.first.bones[0].trans[2]};
        const std::array<float, 4> initialRotation{0, 0, 0, 1};
        f.app->SceneModified() = 0; f.app->RecordedFrameCountByte1() = 0;
        EnableWindow(f.Edit(panel::kUndoButton), FALSE); EnableWindow(f.Edit(panel::kRedoButton), TRUE);
        PoseAxisMessageProbe messages(f);
        unsigned failures = 0; bool succeeded = false;
        for (long limit = 0; limit < 256; ++limit) {
            failAfter = limit;
            PoseAxisCommand(f, kind, 0, floating);
            failAfter = -1;
            if (f.history.UndoCount()) { succeeded = true; break; }
            ++failures;
            Check(f.history.RedoCount() == 1 &&
                !std::memcmp(f.first.bones[0].trans, initialPosition.data(), sizeof(initialPosition)) &&
                !std::memcmp(f.first.bones[0].rotQuat, initialRotation.data(), sizeof(initialRotation)) &&
                bone_scale::Pose(f.first.raw(), 0) == initialScale && f.first.dirty[0] == 0 &&
                PoseAxisReadDegrees(*f.app) == initialDegrees,
                "failed axis capture or inverse preparation restores precise pose flags scale and Euler cache with redo intact");
            Check(f.app->SceneModified() == 0 && f.app->RecordedFrameCountByte1() == 0 &&
                !IsWindowEnabled(f.Edit(panel::kUndoButton)) && IsWindowEnabled(f.Edit(panel::kRedoButton)) &&
                PoseRingDigest(f.first) == ring && PoseRingDigest(f.second) == otherRing && f.SelectionUnchanged() &&
                messages.commands == 0, "failed axis publication does not dirty refresh write legacy rings or clear selections");
            Check(f.history.Enter() == MMS_OK, "axis failure releases its common history reservation"); f.history.Leave();
            float rebuilt[3]{};
            Check(SelectedBoneScaleReadout(f.app.get(), rebuilt) &&
                !std::memcmp(values, rebuilt, sizeof(values)), "failed axis edit retains rebuildable scale readout");
        }
        Check(succeeded && failures > 8 && f.history.RedoCount() == 0 && !messages.observedBusy,
            "axis allocation sweep reaches capture and inverse staging then succeeds without in-scope notifications");
        Check(f.history.History(false) == MMS_OK &&
            !std::memcmp(f.first.bones[0].trans, initialPosition.data(), sizeof(initialPosition)) &&
            !std::memcmp(f.first.bones[0].rotQuat, initialRotation.data(), sizeof(initialRotation)) &&
            bone_scale::Pose(f.first.raw(), 0) == initialScale && PoseRingDigest(f.first) == ring,
            "first successful axis operation after allocation failures remains exactly undoable");
    }
}
void PoseAxisLegacyChecks() {
    for (bool floating : {false, true}) for (auto kind : {AxisKind::Position, AxisKind::Scale}) {
        PoseNumericFixture f;
        Check(plugins::BoundNativePoseHistory(f.app.get()) == nullptr, "axis migration remains opt-in");
        f.first.bones[0].trans[0] = 4.5f;
        if (kind == AxisKind::Scale) {
            bone_scale::SetPose(f.first.raw(), 0, Scale(2));
            // This legacy route calls RefreshRequest(-1). Keep that track
            // selected so the small fixture need not construct all panels.
            f.app->state.globalTrackSelected[0] = 1;
            f.app->state.globalTrackSelected[1] = 0;
        }
        const auto cursor = f.first.m->undoState[0];
        PoseAxisCommand(f, kind, 0, floating);
        Check(f.first.m->undoState[0] == (cursor + 1) % 30 && f.history.UndoCount() == 0 &&
            (kind == AxisKind::Position ? f.first.bones[0].trans[0] == 0 : bone_scale::Identity(bone_scale::Pose(f.first.raw(), 0))),
            "unbound main/floating position and scale reset retain ordinary ring behavior");
    }
}
}
void NativePoseAxisChecks() {
    std::puts("RUN PoseAxisPositionChecks"); PoseAxisPositionChecks();
    std::puts("RUN PoseAxisRotationChecks"); PoseAxisRotationChecks();
    std::puts("RUN PoseAxisScaleChecks"); PoseAxisScaleChecks();
    std::puts("RUN PoseAxisNoopAndGuardChecks"); PoseAxisNoopAndGuardChecks();
    std::puts("RUN PoseAxisFailureChecks"); PoseAxisFailureChecks();
    std::puts("RUN PoseAxisLegacyChecks"); PoseAxisLegacyChecks();
}
