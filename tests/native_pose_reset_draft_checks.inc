// Bound 495 only. The original/unbound reset is intentionally a later batch.
#include "../src/plugins/native_global_commands.hpp"
#include "../src/plugins/edit_store.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include <commctrl.h>
namespace {
struct PoseResetDraftPanel {
    PoseCommandFixture& f;
    HWND owner = nullptr, previous = nullptr, unrelated = nullptr;
    bool floating;
    static constexpr int ids[9] = {544, 545, 546, 547, 548, 549, 572, 575, 577};
    PoseResetDraftPanel(PoseCommandFixture& fixture, bool separate) : f(fixture), previous(f.app->FloatingWindow()), floating(separate) {
        owner = f.app->MainWindow();
        if (floating) {
            owner = CreateWindowExA(0, "STATIC", "owned reset draft panel", WS_OVERLAPPED,
                0, 0, 400, 140, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(owner != nullptr, "create hidden separate owner for actual 495 readouts"); f.app->FloatingWindow() = owner;
            for (int i = 0; i < 9; ++i) Check(CreateWindowExA(0, "EDIT", "0", WS_CHILD, (i % 3) * 90, (i / 3) * 24, 80, 20,
                owner, reinterpret_cast<HMENU>(INT_PTR(ids[i])), GetModuleHandleW(nullptr), nullptr) != nullptr,
                "create actual separate pose EDIT identity");
        }
        for (int i = 0; i < 9; ++i) InstallControlSubclass(f.app.get(), Field(i), ids[i]);
        unrelated = CreateWindowExA(0, "EDIT", "unrelated pending", WS_CHILD, 0, 100, 90, 20,
            owner, reinterpret_cast<HMENU>(INT_PTR(29001)), GetModuleHandleW(nullptr), nullptr);
        Check(unrelated != nullptr, "create unrelated real EDIT sentinel"); SendMessageA(unrelated, EM_SETMODIFY, TRUE, 0);
    }
    ~PoseResetDraftPanel() {
        Clear(); SetFocus(f.app->MainWindow()); f.app->FloatingWindow() = previous;
        if (floating) DestroyWindow(owner); else DestroyWindow(unrelated);
    }
    HWND Field(int index) const { return GetDlgItem(owner, ids[index]); }
    static bool Modified(HWND field) { return SendMessageA(field, EM_GETMODIFY, 0, 0) != 0; }
    static bool Text(HWND field, const char* expected) {
        char actual[96]{}; GetWindowTextA(field, actual, sizeof actual); return !std::strcmp(actual, expected);
    }
    static void Pending(HWND field, const char* value) { SetWindowTextA(field, value); SendMessageA(field, EM_SETMODIFY, TRUE, 0); }
    void Drafts(int focus) {
        SetFocus(Field(focus)); Check(GetFocus() == Field(focus), "495 draft owns actual keyboard focus");
        for (int i = 0; i < 9; ++i) Pending(Field(i), "99.25");
    }
    void Clear() { for (int i = 0; i < 9; ++i) SendMessageA(Field(i), EM_SETMODIFY, FALSE, 0); }
    void LoseFocus(int index) {
        SetFocus(owner); Check(GetFocus() == owner && GetFocus() != Field(index), "495 draft really loses focus to its owned panel");
    }
    bool DraftsPreserved() const {
        for (int i = 0; i < 9; ++i) if (!Text(Field(i), "99.25") || !Modified(Field(i))) return false;
        return true;
    }
    void Expected(bool parented) const {
        const char* positions[3] = {"3.00", "4.00", "5.00"};
        for (int i = 0; i < 9; ++i) Check(Text(Field(i), i < 3 ? (parented ? positions[i] : "0.00") : i < 6 ? "0.0" : "1.00") && !Modified(Field(i)),
            "495 publishes zero local pose plus independent parent bind offsets and unit scale to all nine cleared drafts");
        Check(Text(unrelated, "unrelated pending") && Modified(unrelated), "495 leaves unrelated input and its draft authorization intact");
    }
};
std::array<float, 3> PoseResetDraftAngles(MMDApp& app) {
    return {app.BoneRotationEditDegreesX(), app.BoneRotationEditDegreesY(), app.BoneRotationEditDegreesZ()};
}
void PoseResetDraftSeed(PoseCommandFixture& f, bool changed, int topology = 1) {
    f.app->CurrentFrame() = 37; f.app->BoneRotationEditDegreesX() = 18; f.app->BoneRotationEditDegreesY() = 27; f.app->BoneRotationEditDegreesZ() = 36;
    const int target = topology == 0 ? 0 : 1;
    f.first.m->selectedBone = target; f.first.selected[0] = target == 0; f.first.selected[1] = target == 1;
    f.first.bones[1].parent = topology == 2 ? -1 : 0;
    for (int axis = 0; axis < 3; ++axis) { f.first.bones[0].position[axis] = float(2 + axis); f.first.bones[1].position[axis] = float(5 + 2 * axis); }
    const int other = 1 - target;
    f.first.bones[other].trans[0] = 17; f.first.bones[other].rotQuat[2] = .6f; f.first.bones[other].rotQuat[3] = .8f;
    Check(bone_scale::SetPose(f.first.raw(), other, CommandScale(3)), "retain unselected nonidentity scale to detect incidental whole-pose transform application");
    if (changed) {
        for (int axis = 0; axis < 3; ++axis) f.first.bones[target].trans[axis] = float(2 + axis);
        f.first.bones[target].rotQuat[2] = .6f; f.first.bones[target].rotQuat[3] = .8f;
        Check(bone_scale::SetPose(f.first.raw(), target, CommandScale()), "seed selected nonidentity reset scale");
    }
    Check(!bone_scale::TransformsApplied(f.first.raw()), "495 seed has no applied scale transforms");
}
void PoseResetDraftSuccessChecks() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool floating : {false, true})
        for (int topology = 0; topology < 3; ++topology) for (int focus : {0, 4, 8}) {
            PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
            Check(f.history.SetMode(mode) == MMS_OK, "495 retains explicit-commit history policy in both modes"); PoseResetDraftSeed(f, true, topology);
            const int target = f.first.m->selectedBone, other = 1 - target;
            const auto before = f.first.bones[target], untouched = f.first.bones[other]; const auto flags = f.first.dirty;
            const auto selection = f.first.selected; const auto second = f.second.Digest(), ring = PoseRingDigest(f.first);
            const auto oldScale = bone_scale::Pose(f.first.raw(), target), otherScale = bone_scale::Pose(f.first.raw(), other);
            const std::vector<mdl::BoneKey> keys(f.first.m->boneKeys, f.first.m->boneKeys + mdl::BoneKeyCapacity(f.first.m->boneKeys));
            panel.Drafts(focus); f.Command(495); panel.Expected(topology == 1);
            const float zero[3]{}, identity[4]{0, 0, 0, 1};
            Check(!std::memcmp(f.first.bones[target].trans, zero, sizeof zero) && !std::memcmp(f.first.bones[target].rotQuat, identity, sizeof identity) &&
                bone_scale::Identity(bone_scale::Pose(f.first.raw(), target)) && f.first.dirty[target] == 1 &&
                f.history.UndoCount() == 1 && PoseResetDraftAngles(*f.app) == std::array<float, 3>{0, 0, 0},
                "actual bound 495 zeros root parented and non-first parentless local pose with one explicit history entry");
            Check(!std::memcmp(&untouched, &f.first.bones[other], sizeof untouched) && f.first.dirty[other] == flags[other] &&
                bone_scale::Pose(f.first.raw(), other) == otherScale && !bone_scale::TransformsApplied(f.first.raw()) &&
                second == f.second.Digest() && selection == f.first.selected && f.first.m->selectedBone == target &&
                f.app->CurrentFrame() == 37 && !f.app->PhysicsResetPending() && PoseRingDigest(f.first) == ring && f.SelectionUnchanged() &&
                mdl::BoneKeyCapacity(f.first.m->boneKeys) == keys.size() && !std::memcmp(keys.data(), f.first.m->boneKeys, keys.size() * sizeof(mdl::BoneKey)),
                "495 local publication preserves unselected bones other model keys selection frame physics flag rings and unapplied transforms");
            const auto after = f.first.Digest(); std::uint64_t revision = 0, later = 0; f.history.Revision(revision);
            panel.LoseFocus(focus); f.history.Revision(later);
            Check(after == f.first.Digest() && revision == later && f.history.UndoCount() == 1, "actual focus loss cannot submit the discarded position rotation or scale draft");
            Check(f.history.History(false) == MMS_OK && !std::memcmp(before.trans, f.first.bones[target].trans, sizeof before.trans) &&
                !std::memcmp(before.rotQuat, f.first.bones[target].rotQuat, sizeof before.rotQuat) && bone_scale::Pose(f.first.raw(), target) == oldScale &&
                f.first.dirty[target] == flags[target], "single 495 undo restores exact pose scale and edited flag");
            Check(f.history.History(true) == MMS_OK && !std::memcmp(f.first.bones[target].trans, zero, sizeof zero) &&
                bone_scale::Identity(bone_scale::Pose(f.first.raw(), target)), "495 redo restores local zero target");
        }
}
void PoseResetDraftNoopChecks() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool floating : {false, true}) for (int focus = 0; focus < 9; ++focus) {
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
        Check(f.history.SetMode(mode) == MMS_OK, "configure allocation-free 495 no-op history mode");
        f.first.bones[0].trans[1] = 8; f.Command(495); Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "seed real 495 redo before no-op");
        PoseResetDraftSeed(f, false); panel.Drafts(focus);
        const auto before = f.first.Digest(); const auto redo = f.history.RedoCount(); const auto scale = bone_scale::PoseValues(f.first.raw());
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        failAfter = 0; f.Command(495); failAfter = -1; f.history.Revision(after); panel.Expected(true);
        Check(before == f.first.Digest() && bone_scale::PoseValues(f.first.raw()) == scale && !f.history.UndoCount() && f.history.RedoCount() == redo &&
            revision == after && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && !f.first.dirty[1] &&
            PoseResetDraftAngles(*f.app) == std::array<float, 3>{0, 0, 0}, "legal 495 no-op needs no allocation and preserves redo dirty-only flag authoring and revision");
        panel.LoseFocus(focus); Check(before == f.first.Digest() && f.history.RedoCount() == redo, "each of nine discarded no-op drafts stays discarded on actual focus loss");
        Check(f.history.Configure(mode, 128, 1) == MMS_OK, "set smallest journal budget for allocation-free 495 no-op");
        panel.Drafts(focus); f.Command(495); panel.Expected(true);
        Check(before == f.first.Digest() && !f.history.UndoCount(), "minimum history budget does not reject data-no-op draft cancellation");
    }
}
void PoseResetDraftSelectionChecks() {
    for (bool floating : {false, true}) for (bool empty : {false, true}) {
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
        PoseResetDraftSeed(f, false); f.first.selected[0] = empty ? 0 : 1; f.first.selected[1] = 0; panel.Drafts(3);
        const auto shown = f.first.bones[1]; const auto degrees = PoseResetDraftAngles(*f.app);
        f.Command(495);
        Check(panel.DraftsPreserved() && degrees == PoseResetDraftAngles(*f.app) && !std::memcmp(&shown, &f.first.bones[1], sizeof shown),
            "empty selection and displayed-but-unselected bone never lose their nine drafts or derived angles");
        Check(f.history.UndoCount() == (empty ? 0u : 1u), "495 still resets another selected bone without treating the displayed bone as selected");
        if (!empty) Check(f.first.bones[0].trans[0] == 0 && f.first.bones[0].rotQuat[3] == 1 && bone_scale::Identity(bone_scale::Pose(f.first.raw(), 0)),
            "selected undisplayed root receives independent local zero target");
    }
}
void PoseResetDraftLocalPublicationChecks() {
    for (bool floating : {false, true}) {
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
        PoseResetDraftSeed(f, true); panel.Drafts(0);
        // PostViewRefresh unconditionally enables the scale EDITs in this
        // stopped-playback branch. The local reset caption writer must not.
        // This field is deliberately not the focused field in this fixture.
        EnableWindow(panel.Field(8), FALSE);
        Check(!IsWindowEnabled(panel.Field(8)), "disable a nonfocused real scale field as independent full-readout sentinel");
        f.Command(495); panel.Expected(true);
        Check(f.history.UndoCount() == 1 && !IsWindowEnabled(panel.Field(8)),
            "changed 495 publishes local captions without PostViewRefresh reenabling the independent scale-control sentinel");
        panel.LoseFocus(0);
    }
    for (auto mode : {plugins::SharedHistory::RecordingMode::AllOperations, plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "configure mixed zero and changed selected bones");
        f.first.selected[0] = f.first.selected[1] = 1; f.first.m->selectedBone = 1;
        f.first.bones[1].trans[0] = 2; f.first.bones[1].rotQuat[2] = .6f; f.first.bones[1].rotQuat[3] = .8f;
        Check(bone_scale::SetPose(f.first.raw(), 1, CommandScale()), "seed only one changed selected bone");
        f.Command(495);
        Check(f.history.UndoCount() == 1 && !f.first.dirty[0] && f.first.dirty[1] == 1 &&
            f.first.bones[1].trans[0] == 0 && bone_scale::Identity(bone_scale::Pose(f.first.raw(), 1)),
            "495 marks only genuinely changed selected bones and leaves ordinary dirty-only no-op for the later physical-takeover batch");
    }
}
void PoseResetDraftFailureChecks() {
    for (bool floating : {false, true}) for (bool noop : {false, true}) for (int reason = 0; reason < 12; ++reason) {
        if (noop && (reason == 8 || reason == 9)) continue;
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
        f.first.bones[0].trans[1] = 8; f.Command(495); Check(f.history.History(false) == MMS_OK, "seed 495 redo before failing command");
        PoseResetDraftSeed(f, !noop); panel.Drafts(7);
        std::unique_ptr<bake::ScopedUiInputGate> input; std::unique_ptr<ScopedSceneMutation> mutation;
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> pending;
        std::unique_ptr<plugins::EditStore> edits; plugins::EditClient client; MmsEditApiV1 api{}; MmsHandle edit = 0;
        if (reason == 0) Check(f.history.Enter() == MMS_OK, "hold actual 495 journal busy gate");
        if (reason == 1) f.app->PlaybackActive() = 1;
        if (reason == 2) f.app->FrameStepPlayback() = 1;
        if (reason == 3) f.app->state.recordPlaybackStartPending = 1;
        if (reason == 4) mutation = std::make_unique<ScopedSceneMutation>(*f.app);
        if (reason == 5) input = std::make_unique<bake::ScopedUiInputGate>();
        if (reason == 6) {
            edits = std::make_unique<plugins::EditStore>(f.history, f.storage); client.id = "test.pose-reset-draft"; api = edits->Api(client);
            std::uint64_t revision = 0; f.history.Revision(revision); constexpr char label[] = "pending plugin edit";
            Check(api.begin(api.context, {label, sizeof label - 1}, revision, &edit) == MMS_OK, "prepare real plugin transaction before 495");
        }
        if (reason == 7) {
            f.app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(4); f.app->CameraPitch() = .25f; f.app->CameraDistance() = -45;
            f.app->CameraFov() = 45; f.app->CameraParentModel() = -1; std::shared_ptr<plugins::HistoryChange> change;
            Check(plugins::PlanCameraRotationReset(*f.app, change) == MMS_OK && change &&
                f.history.PrepareNative("reserved before 495", std::move(change), pending) == MMS_OK && pending,
                "prepare actual native camera publication before 495 no-op or edit");
        }
        if (reason == 8) Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations, 128, 1) == MMS_OK, "restrict 495 capture budget");
        if (reason == 10) {
            Check(GetCapture() == nullptr, "495 capture test starts without external mouse capture");
            SetCapture(panel.owner); Check(GetCapture() == panel.owner, "capture only the test-owned hidden panel");
        }
        if (reason == 11) f.app->FrameRangeDialog() = panel.owner;
        const auto before = f.first.Digest(), second = f.second.Digest(); const auto redo = f.history.RedoCount();
        const auto degrees = PoseResetDraftAngles(*f.app); const auto scale = bone_scale::PoseValues(f.first.raw());
        std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = f.app->PhysicsResetPending() = 0;
        if (reason == 9) failAfter = 0; f.Command(495); failAfter = -1; f.history.Revision(after);
        Check(before == f.first.Digest() && second == f.second.Digest() && scale == bone_scale::PoseValues(f.first.raw()) &&
            degrees == PoseResetDraftAngles(*f.app) && panel.DraftsPreserved() && redo == f.history.RedoCount() && !f.history.UndoCount() &&
            revision == after && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && !f.app->PhysicsResetPending(),
            "495 busy playback recording mutation input plugin preparation budget allocation capture or modal failure preserves all data drafts derived state and redo");
        panel.Clear(); if (reason == 0) f.history.Leave();
        if (reason == 10) { ReleaseCapture(); Check(GetCapture() == nullptr, "release only the test-owned capture"); }
        if (reason == 11) f.app->FrameRangeDialog() = nullptr;
        f.app->PlaybackActive() = f.app->FrameStepPlayback() = f.app->state.recordPlaybackStartPending = 0;
        if (edit) api.cancel(api.context, edit); pending.reset();
        if (reason == 7) { f.history.Reset(); f.history.Collect(); mdl::KeyPoolDestroy(f.app->CameraKeys()); f.app->CameraKeys() = nullptr; }
    }
}
void PoseResetDraftHostGateChecks() {
    for (bool floating : {false, true}) for (bool noop : {false, true}) {
        PoseCommandFixture f; bool reject = false;
        plugins::SharedHistory journal(f.storage, [&]() { return reject ? MMS_E_BUSY : MMS_OK; });
        plugins::NativePoseTestBinding binding(*f.app, journal); PoseResetDraftPanel panel(f, floating);
        f.first.bones[0].trans[1] = 8; f.Command(495);
        Check(journal.History(false) == MMS_OK && journal.RedoCount() == 1, "host-gated journal has a genuine redo branch");
        PoseResetDraftSeed(f, !noop); panel.Drafts(0);
        const auto before = f.first.Digest(); const auto scale = bone_scale::PoseValues(f.first.raw());
        const auto degrees = PoseResetDraftAngles(*f.app); reject = true; f.Command(495);
        Check(before == f.first.Digest() && scale == bone_scale::PoseValues(f.first.raw()) && degrees == PoseResetDraftAngles(*f.app) &&
            panel.DraftsPreserved() && journal.RedoCount() == 1 && !journal.UndoCount(),
            "real host edit gate rejects changed and no-op 495 before draft cancellation");
        panel.Clear(); reject = false; journal.Reset(); journal.Collect();
    }
}
void PoseResetDraftTextFailureChecks() {
    struct Fault {
        bool reject = true; unsigned writes = 0;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Fault*>(data);
            if (message == WM_SETTEXT && self.reject) { ++self.writes; return FALSE; }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (int focus : {0, 4, 8}) {
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
        PoseResetDraftSeed(f, true); panel.Drafts(focus); Fault fault;
        Check(SetWindowSubclass(panel.Field(focus), Fault::Proc, 2191, reinterpret_cast<DWORD_PTR>(&fault)) != FALSE, "reject actual 495 caption publication");
        f.Command(495); const auto after = f.first.Digest();
        Check(fault.writes && f.history.UndoCount() == 1 && PoseResetDraftPanel::Text(panel.Field(focus), "99.25") &&
            !PoseResetDraftPanel::Modified(panel.Field(focus)), "successful 495 cancels commit authorization even when native caption write fails");
        panel.LoseFocus(focus); Check(after == f.first.Digest() && f.history.UndoCount() == 1, "failed reset caption cannot reauthor stale pose through focus loss");
        fault.reject = false; f.Command(495); panel.Expected(true);
        Check(f.history.UndoCount() == 1, "no-op 495 retry repairs caption without another history step");
        RemoveWindowSubclass(panel.Field(focus), Fault::Proc, 2191);
    }
}
void PoseResetDraftReentryChecks() {
    struct Reentry {
        PoseCommandFixture& f; HWND replacement; unsigned action; bool armed = true, fired = false;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Reentry*>(data);
            if (self.armed && message == EM_SETMODIFY && !wp) {
                self.armed = false; self.fired = true; const auto result = DefSubclassProc(window, message, wp, lp);
                if (self.action == 0) self.f.app->SelectedModelSlot() = 1;
                if (self.action == 1) { plugins::NativePoseTestBinding nested(*self.f.app, self.f.history); }
                if (self.action == 2) self.f.app->FloatingWindow() = self.replacement;
                if (self.action == 3) self.f.first.selected[1] = 0;
                self.f.app->BoneRotationEditDegreesX() = 71; self.f.app->BoneRotationEditDegreesY() = 72; self.f.app->BoneRotationEditDegreesZ() = 73;
                PoseResetDraftPanel::Pending(window, "new context draft"); return result;
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (unsigned action = 0; action < 4; ++action) {
        PoseCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); PoseResetDraftPanel panel(f, floating);
        PoseResetDraftSeed(f, false); panel.Drafts(0);
        HWND replacement = CreateWindowExA(0, "STATIC", "owned replacement reset owner", WS_OVERLAPPED, 0, 0, 300, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(replacement != nullptr, "create hidden replacement owner for 495 reentry");
        for (int id : PoseResetDraftPanel::ids) {
            HWND field = CreateWindowExA(0, "EDIT", "replacement pending", WS_CHILD, 0, 0, 80, 20, replacement,
                reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            Check(field != nullptr, "replacement owner has distinct actual pose EDIT identities"); SendMessageA(field, EM_SETMODIFY, TRUE, 0);
        }
        Reentry reentry{f, replacement, action};
        Check(SetWindowSubclass(panel.Field(0), Reentry::Proc, 2192, reinterpret_cast<DWORD_PTR>(&reentry)) != FALSE, "install synchronous 495 context change");
        const auto before = f.first.Digest(); f.Command(495);
        Check(reentry.fired && PoseResetDraftPanel::Text(panel.Field(0), "new context draft") && PoseResetDraftPanel::Modified(panel.Field(0)) &&
            PoseResetDraftAngles(*f.app) == std::array<float, 3>{71, 72, 73} && f.first.Digest() == before && !f.history.UndoCount(),
            "495 stops old readout publication after object binding owner or displayed-selection membership changes");
        for (int i = 1; i < 9; ++i) Check(PoseResetDraftPanel::Text(panel.Field(i), "99.25") && PoseResetDraftPanel::Modified(panel.Field(i)),
            "495 context transition leaves subsequent old-owner drafts untouched");
        for (int id : PoseResetDraftPanel::ids) {
            HWND field = GetDlgItem(replacement, id);
            Check(PoseResetDraftPanel::Text(field, "replacement pending") && PoseResetDraftPanel::Modified(field), "old 495 operation cannot clear replacement owner drafts");
            SendMessageA(field, EM_SETMODIFY, FALSE, 0);
        }
        RemoveWindowSubclass(panel.Field(0), Reentry::Proc, 2192); panel.Clear();
        f.app->SelectedModelSlot() = 0; f.app->FloatingWindow() = floating ? panel.owner : nullptr; f.first.selected[1] = 1; DestroyWindow(replacement);
    }
}
void NativePoseResetDraftChecks() {
    PoseResetDraftSuccessChecks(); PoseResetDraftNoopChecks(); PoseResetDraftSelectionChecks(); PoseResetDraftLocalPublicationChecks();
    PoseResetDraftFailureChecks(); PoseResetDraftHostGateChecks(); PoseResetDraftTextFailureChecks(); PoseResetDraftReentryChecks();
}
}
