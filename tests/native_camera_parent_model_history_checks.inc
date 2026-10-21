// Include after native_camera_parent_history_checks.inc. No global input is
// injected: notifications use this fixture's actual combo and ComboLBox.
#include <exception>
namespace camera_parent_model_checks {
using namespace plugins;
using Mode = SharedHistory::RecordingMode;
struct Scene : camera_parent_checks::Scene {
    bool modelForward = false;
    unsigned modelNotifications = 0, modelCaptured = 0;
    static LRESULT CALLBACK Window(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto& f = *reinterpret_cast<Scene*>(data);
        if (message == WM_COMMAND && LOWORD(w) == 449 && HIWORD(w) == CBN_SELCHANGE && f.modelForward) {
            ++f.modelNotifications; if (GetCapture()) ++f.modelCaptured;
            f.Dispatch(); return 0;
        }
        return DefSubclassProc(window, message, w, l);
    }
    Scene() {
        for (auto& bone : second.bones) bone.type = mdl::BoneType::RotateMove;
        Check(SetWindowSubclass(app->MainWindow(), Window, 159, reinterpret_cast<DWORD_PTR>(this)), "observe only the real 449 model notification");
    }
    ~Scene() { RemoveWindowSubclass(app->MainWindow(), Window, 159); }
    void Select(int slot) {
        LRESULT found = slot < 0 ? 0 : -1;
        const auto count = SendMessageW(models, CB_GETCOUNT, 0, 0);
        if (slot >= 0) for (LRESULT row = 1; row < count; ++row)
            if (SendMessageW(models, CB_GETITEMDATA, row, 0) == slot) found = row;
        Check(found >= 0 && SendMessageW(models, CB_SETCURSEL, found, 0) == found, "select requested model by native item identity");
    }
    void Dispatch() {
        auto* previous = g_Block; g_Block = app.get(); CommandDispatch(models, MAKEWPARAM(449, CBN_SELCHANGE)); g_Block = previous;
    }
    void ChooseModel(int slot) { Select(slot); Dispatch(); }
    int ModelSelection() const {
        const auto row = SendMessageW(models, CB_GETCURSEL, 0, 0);
        return row <= 0 ? -1 : int(SendMessageW(models, CB_GETITEMDATA, row, 0));
    }
    std::shared_ptr<HistoryChange> PlanModel(int slot) {
        std::shared_ptr<HistoryChange> change;
        Check(PlanCameraParentModel(*app, slot, change) == MMS_OK && change, "prepare private nine-field model attachment");
        return change;
    }
};
struct Invariants {
    camera_parent_checks::Source source;
    const std::uint32_t cursor, registered;
    const unsigned char active, reload;
    explicit Invariants(Scene& f) : source(f), cursor(f.app->CameraTrackCursor()), registered(f.app->LastRegisteredFrame()),
        active(f.app->CameraTrackActive()), reload(f.app->state.modelReloadPending) {}
    void Unchanged(const NumpadView& expected) {
        source.view = expected; source.Unchanged(expected.parentBone);
        Check(source.f.app->CameraTrackCursor() == cursor && source.f.app->CameraTrackActive() == active &&
            source.f.app->LastRegisteredFrame() == registered && source.f.app->state.modelReloadPending == reload,
            "449 does not seek, request reevaluation, register a key or advance any camera cursor");
    }
};
void PrintNine(const char* label, const NumpadView& value) {
    std::array<std::uint32_t, 7> bits{};
    std::memcpy(bits.data(), value.values.data(), sizeof(bits));
    std::printf("  %s parent=%d bone=%d pos=(%.9g/0x%08x,%.9g/0x%08x,%.9g/0x%08x) rot=(%.9g/0x%08x,%.9g/0x%08x,%.9g/0x%08x) distance=%.9g/0x%08x\n",
        label, value.parentModel, value.parentBone,
        double(value.values[0]), bits[0], double(value.values[1]), bits[1], double(value.values[2]), bits[2],
        double(value.values[3]), bits[3], double(value.values[4]), bits[4], double(value.values[5]), bits[5], double(value.values[6]), bits[6]);
}
void NativeOracleAndModes() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool camera : {false, true})
    for (int parent : {-7, -1, 0, 1}) for (int target : {-1, 0, 1}) {
        Scene f, oracle; NativePoseTestBinding binding(*f.app, f.history);
        for (auto* item : {&f, &oracle}) {
            item->app->CameraMode() = camera; item->app->CameraParentModel() = parent;
            item->app->CameraParentBone() = parent < 0 ? INT32_MIN + 87 : parent == 0 ? 3 : 1;
            item->app->state.followCameraEnabled = 0;
        }
        Check(f.history.SetMode(mode) == MMS_OK, "configure 449 adjustment recording policy");
        const auto before = NumpadRead(*f.app); Invariants invariant(f);
        const bool priorPublicationReset = f.history.LastPublicationRequiresPhysicsReset();
        const auto priorPhysicsResetPending = f.app->PhysicsResetPending();
        oracle.ChooseModel(target); const auto expected = NumpadRead(*oracle.app);
        const bool changed = !NumpadSame(before, expected);
        const auto draft = FollowPendingEdit(*f.app, panel::kReadoutPosXEdit);
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light); f.accessory.rowSelected = 1;
        f.ChooseModel(target);
        if (!NumpadSame(NumpadRead(*f.app), expected)) std::printf("  449 oracle mode=%u camera=%u parent=%d target=%d\n", unsigned(mode), unsigned(camera), parent, target);
        Check(NumpadSame(NumpadRead(*f.app), expected) && f.ModelSelection() == target &&
            f.history.UndoCount() == (changed && mode == Mode::AllOperations ? 1 : 0),
            "real bound 449 matches unbound native attach, switch, same-model reset and detach exactly");
        Check(f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera) && !f.accessory.rowSelected && FollowHasPendingEdit(draft),
            "449 retains initial camera-track activation and pending draft without changing model selection");
        const bool dirtyCorrect = bool(f.app->SceneModified()) == changed && bool(f.app->RecordedFrameCountByte1()) == changed &&
            f.app->PhysicsResetPending() == priorPhysicsResetPending &&
            f.history.LastPublicationRequiresPhysicsReset() == (changed ? false : priorPublicationReset);
        if (!dirtyCorrect) {
            std::printf("449_DIRTY mode=%u camera=%u parent=%d target=%d changed=%u sceneModified=%u recordedFrame=%u priorPublicationReset=%u currentPublicationReset=%u priorPhysicsResetPending=%u physicsResetPending=%u undo=%zu\n",
                unsigned(mode), unsigned(camera), parent, target, unsigned(changed), unsigned(f.app->SceneModified()),
                unsigned(f.app->RecordedFrameCountByte1()), unsigned(priorPublicationReset), unsigned(f.history.LastPublicationRequiresPhysicsReset()),
                unsigned(priorPhysicsResetPending), unsigned(f.app->PhysicsResetPending()), f.history.UndoCount());
            PrintNine("before", before); PrintNine("current", NumpadRead(*f.app));
        }
        Check(dirtyCorrect, "changed attachment marks dirty without physics reset; no-op preserves the prior publication flag and actual pending state");
        invariant.Unchanged(expected);
        if (changed && mode == Mode::AllOperations) {
            f.app->CurrentFrame() = 73; f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light); f.accessory.rowSelected = 1;
            Check(TryBoundHistoryCommand(f.app.get(), false) && NumpadSame(NumpadRead(*f.app), before) &&
                f.app->CurrentFrame() == 73 && f.ModelSelection() == (parent < 0 ? -1 : parent),
                "common undo restores all original attachment bits at a later frame");
            Check(f.app->GlobalTrackSelected(GlobalTimelineTrack::Light) && f.accessory.rowSelected && FollowHasPendingEdit(draft),
                "replay readout preserves unrelated selection and draft instead of repeating activation");
            Check(TryBoundHistoryCommand(f.app.get(), true) && NumpadSame(NumpadRead(*f.app), expected) && f.app->CurrentFrame() == 73,
                "common redo restores the frozen result without seeking or resampling");
        }
    }
    for (float distance : {-71.25f, -.01f, 0.f, 13.f, NumpadBits(0x7fc12345), NumpadBits(0x7f800000)}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.app->CameraParentModel() = -1; f.app->CameraDistance() = distance;
        const auto before = NumpadRead(*f.app); f.ChooseModel(1);
        Check(f.app->CameraPositionZ() == (std::isfinite(distance) && distance < -.1f ? distance : -45.f) &&
            !std::memcmp(&f.app->CameraDistance(), &distance, sizeof(float)), "attach fallback preserves even nonfinite original distance bits");
        Check(f.history.History(false) == MMS_OK && NumpadSame(NumpadRead(*f.app), before), "undo keeps signed/raw source values rather than normalizing them");
    }
}
void ChainFrozenPivotAndRedoPolicy() {
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    f.Choose(2); const auto a = NumpadRead(*f.app); f.ChooseModel(1); const auto b = NumpadRead(*f.app); f.Choose(1);
    Check(f.history.UndoCount() == 3 && f.app->CameraParentModel() == 1 && f.app->CameraParentBone() == 1, "450 A, 449 B and 450 B produce three chronological nodes");
    Check(f.history.History(false) == MMS_OK && NumpadSame(NumpadRead(*f.app), b) &&
        f.history.History(false) == MMS_OK && NumpadSame(NumpadRead(*f.app), a) &&
        f.history.History(false) == MMS_OK && f.app->CameraParentModel() == 0 && f.app->CameraParentBone() == 0,
        "449 fills the cross-model history gap without relaxing 450's exact parent authorization");
    for (unsigned i = 0; i < 3; ++i) Check(f.history.History(true) == MMS_OK, "redo each real 450/449 node in order");
    f.ChooseModel(-1); const auto detached = NumpadRead(*f.app);
    Check(detached.parentBone == 1 && detached.values[6] == -20.f, "detach retains the raw bone field and legacy distance");
    f.second.bones[1].matInit[12] += 117.125f; f.second.bones[1].trans[2] = 37.5f; f.app->CurrentFrame() = 97;
    Invariants later(f);
    Check(f.history.History(false) == MMS_OK && f.app->CameraParentModel() == 1 &&
        f.history.History(true) == MMS_OK && NumpadSame(NumpadRead(*f.app), detached), "detach redo uses its one-time private pivot after live bone pose changes");
    later.Unchanged(detached);
    Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "seed redo before same-value model action");
    f.app->CameraParentBone() = 0; f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    f.ChooseModel(1);
    Check(f.history.RedoCount() == 1 && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(), "true same-value 449 preserves redo and dirty flags");
    Check(f.history.SetMode(Mode::ExplicitOnly) == MMS_OK, "switch to explicit-only adjustment policy");
    const auto undo = f.history.UndoCount(), redo = f.history.RedoCount(); f.ChooseModel(0);
    Check(f.history.UndoCount() == undo && f.history.RedoCount() == redo && f.app->CameraParentModel() == 0,
        "explicit-only 449 publishes without adding a node or discarding existing redo");
}
void MappingAndLegacyRefusals() {
    for (bool bound : {false, true}) for (unsigned invalid = 0; invalid < 7; ++invalid) {
        Scene f; std::unique_ptr<NativePoseTestBinding> binding;
        if (bound) binding = std::make_unique<NativePoseTestBinding>(*f.app, f.history);
        auto* table = f.first.m->boneTable; const auto count = f.first.m->boneCount;
        if (invalid == 0) f.app->CameraParentModel() = kModelSlotCount;
        if (invalid == 1) f.app->CameraParentModel() = 253;
        if (invalid == 2) f.app->CameraParentBone() = -1;
        if (invalid == 3) f.app->CameraParentBone() = INT32_MAX;
        if (invalid == 4) f.first.m->boneTable = nullptr;
        if (invalid == 5) f.first.m->boneCount = 0;
        if (invalid == 6) for (auto& bone : f.second.bones) bone.type = mdl::BoneType::InertTip;
        const auto before = NumpadRead(*f.app); const auto boneRows = SendMessageW(f.combo, CB_GETCOUNT, 0, 0);
        f.ChooseModel(invalid == 6 ? 1 : -1);
        Check(NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount() && !f.app->SceneModified(),
            "invalid parent slots, storage, detach indices and all-filtered target refuse without partial camera writes");
        if (!bound) Check(SendMessageW(f.combo, CB_GETCOUNT, 0, 0) == boneRows, "legacy safety refusal happens before clearing the old bone list");
        f.first.m->boneTable = table; f.first.m->boneCount = count;
    }
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.second.bones[0].type = mdl::BoneType::InertTip;
        f.ChooseModel(1); Check(f.app->CameraParentBone() == 1 && f.Selection() == 1, "first selectable target bone is an item identity rather than assumed index zero");
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); const auto before = NumpadRead(*f.app);
        f.Select(1);
        if (invalid == 0) SendMessageW(f.models, CB_SETITEMDATA, 2, 0);
        if (invalid == 1) SendMessageW(f.models, CB_SETITEMDATA, 2, -1);
        if (invalid == 2) SendMessageW(f.models, CB_DELETESTRING, 1, 0);
        if (invalid == 3) SendMessageW(f.models, CB_SETCURSEL, -1, 0);
        if (invalid == 4) f.app->ModelSlot(2) = f.second.raw();
        f.Dispatch();
        Check(NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount() && !f.app->SceneModified(), "bad native mapping or duplicate model membership consumes 449 without fallback");
        if (invalid == 4) f.app->ModelSlot(2) = nullptr;
    }
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    SendMessageW(f.models, CB_RESETCONTENT, 0, 0);
    for (int id : {-1, 1, 0}) { const auto row = SendMessageW(f.models, CB_ADDSTRING, 0, LPARAM(L"duplicate")); SendMessageW(f.models, CB_SETITEMDATA, row, id); }
    f.ChooseModel(1); Check(f.app->CameraParentModel() == 1 && f.history.UndoCount() == 1, "valid reordered duplicate labels preserve model item identity");
    for (bool bound : {false, true}) {
        Scene raw; std::unique_ptr<NativePoseTestBinding> owner;
        if (bound) owner = std::make_unique<NativePoseTestBinding>(*raw.app, raw.history);
        constexpr auto invalidBone = INT32_MIN + 83; raw.app->CameraParentBone() = invalidBone;
        raw.app->CameraPositionX() = NumpadBits(0x80000000); raw.app->CameraRotation()[1] = NumpadBits(0x7fc12345);
        const auto before = NumpadRead(*raw.app); raw.ChooseModel(1); auto expected = before; expected.parentModel = 1; expected.parentBone = 0;
        Check(NumpadSame(NumpadRead(*raw.app), expected), "switching models need not dereference or normalize an invalid old bone or unchanged floating payload");
        if (bound) Check(raw.history.History(false) == MMS_OK && NumpadSame(NumpadRead(*raw.app), before), "undo restores complete noncanonical old bone and raw float bytes");
    }
    Scene same; NativePoseTestBinding sameBinding(*same.app, same.history); same.app->CameraParentBone() = 3; same.ChooseModel(0);
    Check(same.history.UndoCount() == 1 && same.app->CameraParentBone() == 0, "same-model selection owns its real first-bone reset");
    same.app->CameraParentModel() = 1;
    Check(same.history.History(false) == MMS_E_STALE && same.app->CameraParentModel() == 1 && same.app->CameraParentBone() == 0,
        "same-model index-only history cannot overwrite a different current parent's bone");
    same.app->CameraParentModel() = 0;
    Check(same.history.History(false) == MMS_OK && same.app->CameraParentBone() == 3, "index-only history resumes when its exact model returns");
}
void LegacyReplacementAndRecoveryVersion() {
    for (UINT phase : {UINT(CB_RESETCONTENT), UINT(CB_ADDSTRING), UINT(CB_SETITEMDATA)}) for (bool replaceSlot : {false, true}) {
        Scene f; Fixture shorter;
        shorter.m->boneCount = 1; shorter.bones[0].type = mdl::BoneType::RotateMove;
        f.second.bones[0].type = mdl::BoneType::InertTip; // old target's first legal index 1 cannot fit the replacement
        f.Select(1); const auto before = NumpadRead(*f.app);
        auto* oldTable = f.second.m->boneTable; const auto oldCount = f.second.m->boneCount;
        camera_parent_checks::UiProbe probe(f, f.combo); probe.trigger = phase; unsigned calls = 0;
        probe.callback = [&](UINT) { ++calls;
            if (replaceSlot) f.app->ModelSlot(1) = shorter.raw();
            else { f.second.m->boneTable = shorter.bones.data(); f.second.m->boneCount = 1; }
        };
        f.Dispatch();
        Check(calls == 1 && NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount() && !f.app->SceneModified(),
            "unbound legacy native rebuilding cannot write an old first index into a shorter replacement model");
        Check(replaceSlot ? f.app->ModelSlot(1) == shorter.raw() : f.second.m->boneTable == shorter.bones.data(),
            "fallback rejection preserves the nested model replacement rather than reverting it");
        f.app->ModelSlot(1) = f.second.raw(); f.second.m->boneTable = oldTable; f.second.m->boneCount = oldCount;
    }
    struct AfterReadSwap {
        HWND window; unsigned lastReads = 0, swaps = 0;
        static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
            auto& state = *reinterpret_cast<AfterReadSwap*>(data);
            const auto result = DefSubclassProc(window, message, w, l);
            // Entry validation is scan 1 and common readback is scan 2. Swap
            // after the final value of RestoreModelFast's own scan returned.
            if (message == CB_GETITEMDATA && w == 2 && ++state.lastReads == 3) {
                ++state.swaps; SendMessageW(window, CB_SETITEMDATA, 1, 1); SendMessageW(window, CB_SETITEMDATA, 2, 0);
            }
            return result;
        }
        explicit AfterReadSwap(HWND value) : window(value) { Check(SetWindowSubclass(window, Observe, 160, DWORD_PTR(this)), "install post-forward native model mapping mutation"); }
        ~AfterReadSwap() { RemoveWindowSubclass(window, Observe, 160); }
    };
    Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Select(1); f.app->PlaybackActive() = 1;
    const auto before = NumpadRead(*f.app); AfterReadSwap mutation(f.models); f.Dispatch();
    Check(mutation.swaps == 1 && mutation.lastReads >= 4 && NumpadSame(NumpadRead(*f.app), before) &&
        f.ModelSelection() == 0 && SendMessageW(f.models, CB_GETCURSEL, 0, 0) == 2 && !f.history.UndoCount(),
        "no-allocation refusal recovery detects native post-read row changes and selects current canonical item data");
}
bool CompleteModelMapping(const Scene& f) {
    if (SendMessageW(f.models, CB_GETCOUNT, 0, 0) != 3 || SendMessageW(f.models, CB_GETITEMDATA, 0, 0) != -1) return false;
    unsigned seen = 0;
    for (WPARAM row = 1; row != 3; ++row) {
        const auto item = SendMessageW(f.models, CB_GETITEMDATA, row, 0);
        if (item < 0 || item > 1 || (seen & (1u << unsigned(item)))) return false;
        seen |= 1u << unsigned(item);
    }
    return seen == 3;
}
template<class Predicate> unsigned PumpOwnParentRepair(Scene& f, Predicate ready) {
    unsigned timers = 0; const auto start = GetTickCount();
    while (!ready() && GetTickCount() - start < 500) {
        MSG message{};
        if (PeekMessageW(&message, f.combo, WM_TIMER, WM_TIMER, PM_REMOVE)) { ++timers; DispatchMessageW(&message); }
        else Sleep(1);
    }
    return timers;
}
void TraceModelMapping(const char* stage, const Scene& f, unsigned callbacks, unsigned timers) {
    const auto count = SendMessageW(f.models, CB_GETCOUNT, 0, 0);
    const auto row = SendMessageW(f.models, CB_GETCURSEL, 0, 0);
    const auto data = row >= 0 ? SendMessageW(f.models, CB_GETITEMDATA, row, 0) : CB_ERR;
    const auto first = count > 1 ? SendMessageW(f.models, CB_GETITEMDATA, 1, 0) : CB_ERR;
    const auto second = count > 2 ? SendMessageW(f.models, CB_GETITEMDATA, 2, 0) : CB_ERR;
    std::printf("449_MAPPING stage=%s callbacks=%u timers=%u count=%lld row=%lld data=%lld items=(%lld,%lld) parent=%d bone=%d undo=%zu redo=%zu\n",
        stage, callbacks, timers, static_cast<long long>(count), static_cast<long long>(row), static_cast<long long>(data),
        static_cast<long long>(first), static_cast<long long>(second), f.app->CameraParentModel(), f.app->CameraParentBone(),
        f.history.UndoCount(), f.history.RedoCount());
    std::fflush(stdout);
}
void NoParentMappingRecovery() {
    for (bool missingRow : {false, true}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(Mode::AllOperations) == MMS_OK, "record the real detach and subsequent model notification");
        f.ChooseModel(-1);
        Check(f.app->CameraParentModel() == -1 && f.history.UndoCount() == 1 && f.ModelSelection() == -1 && CompleteModelMapping(f),
            "prepare no-parent recovery through an actual accepted 449 detach and intact mapping");
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        const auto before = NumpadRead(*f.app); Invariants untouched(f);
        const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
        std::uint64_t revision = 0; Check(f.history.Revision(revision) == MMS_OK, "capture no-parent history revision before corrupt input");
        // Select B before corruption, so failure can only come from the real
        // notification/readback and never from Select's fixture precondition.
        f.Select(1); unsigned callbacks = 0;
        camera_parent_checks::UiProbe mutation(f, f.models); mutation.trigger = CB_GETCOUNT;
        mutation.callback = [&](UINT) {
            ++callbacks;
            Check(SendMessageW(f.models, CB_GETCURSEL, 0, 0) == 2 && f.app->CameraParentModel() == -1,
                "corruption occurs during the actual selected-B notification with canonical no parent");
            if (missingRow) Check(SendMessageW(f.models, CB_DELETESTRING, 1, 0) == 2, "remove model A only after valid B selection");
            else Check(SendMessageW(f.models, CB_SETITEMDATA, 1, 1) != CB_ERR, "duplicate B item identity only after valid B selection");
        };
        f.Dispatch();
        TraceModelMapping(missingRow ? "none-rejected-missing" : "none-rejected-duplicate", f, callbacks, 0);
        std::uint64_t rejectedRevision = 0;
        Check(callbacks == 1 && NumpadSame(NumpadRead(*f.app), before) && f.history.UndoCount() == undo && f.history.RedoCount() == redo &&
            f.history.Revision(rejectedRevision) == MMS_OK && rejectedRevision == revision && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
            "the first corrupt-mapping notification is consumed without camera, history or dirty changes");
        untouched.Unchanged(before);
        const auto timers = PumpOwnParentRepair(f, [&] { return CompleteModelMapping(f) && SendMessageW(f.models, CB_GETCURSEL, 0, 0) == 0; });
        TraceModelMapping(missingRow ? "none-repaired-missing" : "none-repaired-duplicate", f, callbacks, timers);
        std::uint64_t repairedRevision = 0;
        Check(CompleteModelMapping(f) && SendMessageW(f.models, CB_GETCURSEL, 0, 0) == 0 && f.ModelSelection() == -1 &&
            NumpadSame(NumpadRead(*f.app), before) && f.history.UndoCount() == undo && f.history.RedoCount() == redo &&
            f.history.Revision(repairedRevision) == MMS_OK && repairedRevision == revision && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
            "no-parent refusal repairs the full unique native model mapping at row zero without creating an edit");
        untouched.Unchanged(before);
        NumpadView expected;
        {
            Scene oracle;
            Check(!BoundNativePoseHistory(oracle.app.get()), "the post-repair attachment oracle uses the independent unbound native route");
            oracle.app->CameraMode() = f.app->CameraMode(); oracle.app->EnglishUI() = f.app->EnglishUI();
            oracle.app->CurrentFrame() = f.app->CurrentFrame(); oracle.app->state.followCameraEnabled = f.app->state.followCameraEnabled;
            NumpadWrite(*oracle.app, before);
            Check(NumpadSame(NumpadRead(*oracle.app), before) && oracle.app->CameraMode() == f.app->CameraMode(),
                "seed every original attachment bit and the same camera mode before the native B oracle");
            oracle.ChooseModel(1); expected = NumpadRead(*oracle.app);
            Check(expected.parentModel == 1 && expected.parentBone == 0 && oracle.ModelSelection() == 1 && !oracle.history.UndoCount(),
                "the real unbound no-parent to B operation produces the full expected initialized camera state");
        }
        f.ChooseModel(1);
        TraceModelMapping("none-next-valid-B", f, callbacks, timers);
        if (callbacks != 1 || !NumpadSame(NumpadRead(*f.app), expected) || f.ModelSelection() != 1 || f.history.UndoCount() != undo + 1 ||
            f.history.RedoCount() != 0 || !f.app->SceneModified() || !f.app->RecordedFrameCountByte1()) {
            std::printf("449_NONE_NEXT_B missingRow=%u callbacks=%u timers=%u expectedUndo=%zu actualUndo=%zu redo=%zu\n",
                unsigned(missingRow), callbacks, timers, undo + 1, f.history.UndoCount(), f.history.RedoCount());
            PrintNine("before", before); PrintNine("expected-native", expected); PrintNine("actual", NumpadRead(*f.app)); std::fflush(stdout);
        }
        Check(callbacks == 1 && NumpadSame(NumpadRead(*f.app), expected) && f.ModelSelection() == 1 && f.history.UndoCount() == undo + 1 &&
            f.history.RedoCount() == 0 && f.app->SceneModified() && f.app->RecordedFrameCountByte1(),
            "a subsequent real B selection succeeds exactly once after no-parent mapping repair");
        untouched.Unchanged(expected);
    }
}
void AcceptedReadbackMappingSwap() {
    struct AfterAcceptedRead {
        Scene& f; HWND window; unsigned lastReads = 0, swaps = 0, phase = 0;
        int parentAtSwap = -2, boneAtSwap = -2; std::size_t undoAtSwap = 0, redoAtSwap = 0; LRESULT returnedItem = CB_ERR;
        static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
            auto& state = *reinterpret_cast<AfterAcceptedRead*>(data);
            const auto result = DefSubclassProc(window, message, w, l);
            if (message == CB_GETITEMDATA && w == 2 && ++state.lastReads == 2) {
                state.phase = state.lastReads; ++state.swaps; state.returnedItem = result;
                state.parentAtSwap = state.f.app->CameraParentModel(); state.boneAtSwap = state.f.app->CameraParentBone();
                state.undoAtSwap = state.f.history.UndoCount(); state.redoAtSwap = state.f.history.RedoCount();
                Check(SendMessageW(window, CB_SETITEMDATA, 1, 1) != CB_ERR && SendMessageW(window, CB_SETITEMDATA, 2, 0) != CB_ERR,
                    "swap actual model rows after forwarding accepted readback's final item");
            }
            return result;
        }
        explicit AfterAcceptedRead(Scene& value) : f(value), window(value.models) {
            Check(SetWindowSubclass(window, Observe, 161, DWORD_PTR(this)), "observe the accepted second model mapping scan");
        }
        ~AfterAcceptedRead() { RemoveWindowSubclass(window, Observe, 161); }
    };
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    Check(f.history.SetMode(Mode::AllOperations) == MMS_OK, "record the accepted model edit during native readback mutation");
    f.app->PlaybackActive() = 0; const auto before = NumpadRead(*f.app); Invariants untouched(f);
    Check(before.parentModel == 0 && before.parentBone == 0 && !f.history.UndoCount() && CompleteModelMapping(f),
        "prepare accepted post-read mutation with canonical A and a complete model list");
    f.Select(1); AfterAcceptedRead mutation(f); f.Dispatch();
    const auto readsAtReturn = mutation.lastReads;
    std::printf("449_ACCEPTED_SWAP phase=%u swaps=%u readsAtReturn=%u returnedItem=%lld parentAtSwap=%d boneAtSwap=%d undoAtSwap=%zu redoAtSwap=%zu\n",
        mutation.phase, mutation.swaps, readsAtReturn, static_cast<long long>(mutation.returnedItem), mutation.parentAtSwap,
        mutation.boneAtSwap, mutation.undoAtSwap, mutation.redoAtSwap); std::fflush(stdout);
    auto expected = before; expected.parentModel = 1; expected.parentBone = 0;
    Check(mutation.phase == 2 && mutation.swaps == 1 && readsAtReturn >= 2 && mutation.returnedItem == 1 &&
        mutation.parentAtSwap == 1 && mutation.boneAtSwap == 0 && mutation.undoAtSwap == 1 && !mutation.redoAtSwap &&
        NumpadSame(NumpadRead(*f.app), expected) && f.history.UndoCount() == 1 && !f.history.RedoCount(),
        "the second-scan mutation occurs only after one successful B attachment publication");
    const auto timers = PumpOwnParentRepair(f, [&] { return CompleteModelMapping(f) && f.ModelSelection() == 1; });
    TraceModelMapping("accepted-post-read-repair", f, mutation.swaps, timers);
    Check(CompleteModelMapping(f) && f.ModelSelection() == 1 && NumpadSame(NumpadRead(*f.app), expected) &&
        f.history.UndoCount() == 1 && !f.history.RedoCount() && mutation.swaps == 1,
        "accepted native post-read row mutation converges to B item identity without another history node");
    untouched.Unchanged(expected);
    f.ChooseModel(0); expected.parentModel = 0; expected.parentBone = 0;
    TraceModelMapping("accepted-next-valid-A", f, mutation.swaps, timers);
    Check(NumpadSame(NumpadRead(*f.app), expected) && f.ModelSelection() == 0 && f.history.UndoCount() == 2 && !f.history.RedoCount(),
        "a subsequent real A selection still commits one model change after accepted-readback repair");
    untouched.Unchanged(expected);
}
void OwnerCandidatesAndBudget() {
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); auto change = f.PlanModel(1);
        std::unique_ptr<PreparedHistoryChange> candidate; Check(change->Prepare(true, candidate) == MMS_OK && candidate->IsCurrent(), "prepare exact two-owner model candidate");
        auto* table = f.second.m->boneTable; const auto count = f.second.m->boneCount;
        if (mutation == 0) ++f.app->CurrentFrame();
        if (mutation == 1) f.app->CameraPositionX() += 1;
        if (mutation == 2) f.app->CameraRotation()[1] += 1;
        if (mutation == 3) f.app->CameraDistance() += 1;
        if (mutation == 4) f.app->CameraParentBone() = 3;
        if (mutation == 5) f.second.m->boneCount = 1;
        if (mutation == 6) f.second.m->boneTable = f.first.bones.data();
        if (mutation == 7) f.app->AdvanceSceneGeneration();
        if (mutation == 8) { NativePoseTestBinding nested(*f.app, f.history); }
        if (mutation == 9) { ForgetSceneObject(f.second.raw()); SceneObjectIdentity(f.second.raw()); }
        if (mutation == 10) { auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 1); auto park = owner->PreparePresence(false);
            Check(park->IsCurrent(), "prepare target model parking"); park->Publish(); park.reset();
            Check(!candidate->IsCurrent(), "missing target refuses before dereferencing old storage"); auto restore = owner->PreparePresence(true);
            Check(restore->IsCurrent(), "prepare retained target restoration"); restore->Publish(); }
        Check(!candidate->IsCurrent() && !f.history.UndoCount(), "full initial camera, frame, source, table, identity and binding changes invalidate candidates");
        candidate.reset(); f.second.m->boneTable = table; f.second.m->boneCount = count;
    }
    Scene f; NativePoseTestBinding binding(*f.app, f.history); auto change = f.PlanModel(1);
    std::unique_ptr<PreparedHistoryChange> candidate; Check(change->Prepare(true, candidate) == MMS_OK, "measure both actual owner bookkeepings");
    std::uint64_t target = 0; Check(candidate->TargetRetainedBytes(target) && target > change->RetainedBytes(), "model history charges both retained owner ledgers"); candidate.reset(); change.reset();
    Check(f.history.Configure(Mode::AllOperations, 128, target - 1) == MMS_OK, "admit less than complete model record charge");
    const auto before = NumpadRead(*f.app); f.ChooseModel(1);
    Check(NumpadSame(NumpadRead(*f.app), before) && f.ModelSelection() == 0 && !f.history.UndoCount(), "budget refusal restores native 449 without partial writes");
    Check(f.history.Configure(Mode::AllOperations, 128, target + 1024 * 1024) == MMS_OK, "restore sufficient model budget"); f.ChooseModel(1);
    Check(f.history.UndoCount() == 1 && f.app->CameraParentModel() == 1, "same requested switch succeeds after complete budget admission");
    auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 0); auto park = owner->PreparePresence(false); Check(park->IsCurrent(), "park retained previous parent"); park->Publish(); park.reset();
    Check(f.history.History(false) == MMS_E_STALE && f.app->CameraParentModel() == 1, "absent previous owner refuses history without partial attachment");
    auto restore = owner->PreparePresence(true); Check(restore->IsCurrent(), "restore previous retained parent"); restore->Publish(); restore.reset();
    Check(f.history.History(false) == MMS_OK && f.app->CameraParentModel() == 0 && f.history.History(true) == MMS_OK,
        "long model history allows a legitimate same-owner removal and restoration");
    const auto cap = mdl::KeyPoolCapacity(f.app->CameraKeys()); auto* grown = mdl::KeyPoolCreate<mdl::CameraKey>(cap + 5);
    std::memcpy(grown, f.app->CameraKeys(), cap * sizeof(mdl::CameraKey)); mdl::KeyPoolCommitGrowth(f.app->CameraKeys(), grown);
    Check(f.history.History(false) == MMS_OK && f.history.History(true) == MMS_OK, "logical camera-pool growth keeps model history replayable");
    mdl::KeyPoolDestroy(f.app->CameraKeys()); f.app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(cap + 5);
    Check(f.history.History(false) == MMS_E_STALE, "a replacement logical camera root rejects old attachment history");
}
void AllocationAndPublication() {
    bool complete = false;
    for (int failure = 0; failure < 768; ++failure) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); const auto before = NumpadRead(*f.app); Invariants invariant(f);
        f.Select(1); failAfter = failure; f.Dispatch(); const auto remaining = failAfter; failAfter = -1;
        if (f.app->CameraParentModel() == 1) {
            Check(f.history.UndoCount() == 1 && f.app->CameraParentBone() == 0, "accepted UI allocation failure keeps one complete attachment");
            auto expected = before; expected.parentModel = 1; expected.parentBone = 0; invariant.Unchanged(expected);
            if (remaining > 0) { complete = true; break; }
        } else {
            Check(NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount() && !f.app->SceneModified() &&
                !f.app->RecordedFrameCountByte1() && f.ModelSelection() == 0, "every rejected allocation preserves camera, history, dirty flags and canonical model selection"); invariant.Unchanged(before);
        }
    }
    Check(complete, "449 allocation sweep reaches an uninjected complete notification");
    Scene f; NativePoseTestBinding binding(*f.app, f.history); auto change = f.PlanModel(-1); const auto before = NumpadRead(*f.app);
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("model cancel", change, tx, SharedHistory::NativeIntent::Adjustment) == MMS_OK, "reserve complete attachment transaction"); tx.reset();
    Check(NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount(), "cancelling private nine-field transaction leaves no partial state");
    Check(f.history.PrepareNative("model publish", change, tx, SharedHistory::NativeIntent::Adjustment) == MMS_OK, "reserve model attachment again after cancellation");
    failAfter = 0; const auto committed = f.history.CommitNative(std::move(tx)); failAfter = -1;
    Check(committed == MMS_OK && f.app->CameraParentModel() == -1 && f.history.UndoCount() == 1, "full common model publication allocates nothing");
    for (unsigned direction = 0; direction < 8; ++direction) {
        std::unique_ptr<PreparedHistoryChange> candidate; Check(change->Prepare(direction % 2 != 0, candidate) == MMS_OK, "prepare alternating private model replay");
        failAfter = 0; const bool current = candidate->IsCurrent(); if (current) candidate->Publish(); failAfter = -1;
        Check(current && f.app->CameraParentModel() == (direction % 2 ? -1 : 0), "source and owner coverage checks plus scalar replay remain allocation-free");
    }
}
void NativeReentryAndDrafts() {
    for (UINT phase : {UINT(CB_GETCURSEL), UINT(CB_GETCOUNT), UINT(CB_GETITEMDATA)}) for (unsigned mutation = 0; mutation < 9; ++mutation) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Select(1); const auto before = NumpadRead(*f.app);
        auto replacement = f.second.bones; auto* table = f.second.m->boneTable;
        camera_parent_checks::UiProbe probe(f, f.models); probe.trigger = phase; unsigned calls = 0;
        probe.callback = [&](UINT) { ++calls;
            if (mutation == 0) f.app->CameraPositionY() += 7;
            if (mutation == 1) ++f.app->CurrentFrame();
            if (mutation == 2) f.second.m->boneTable = replacement.data();
            if (mutation == 3) { ForgetSceneObject(f.second.raw()); SceneObjectIdentity(f.second.raw()); }
            if (mutation == 4) f.app->PlaybackActive() = 1;
            if (mutation == 5) f.app->FrameStepPlayback() = 1;
            if (mutation == 6) f.app->state.recordPlaybackStartPending = 1;
            if (mutation == 7) f.second.bones[0].type = mdl::BoneType::InertTip;
            if (mutation == 8) f.app->FrameRangeDialog() = f.app->MainWindow();
        };
        f.Dispatch();
        Check(calls == 1 && f.app->CameraParentModel() == before.parentModel && !f.history.UndoCount() && !f.app->SceneModified(),
            "native model reads cannot replan a request after source, first bone or readiness changes");
        Check((mutation != 0 || f.app->CameraPositionY() == before.values[1] + 7) && (mutation != 2 || f.second.m->boneTable == replacement.data()),
            "449 rejection does not undo the reentrant caller's separate changes"); f.second.m->boneTable = table; f.app->FrameRangeDialog() = nullptr;
    }
    for (UINT phase : {UINT(EM_GETMODIFY), UINT(WM_GETTEXT), UINT(EM_GETSEL)}) for (bool noop : {false, true}) for (unsigned mutation = 0; mutation < 5; ++mutation) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); const auto draft = FollowPendingEdit(*f.app, panel::kReadoutPosXEdit);
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light); f.accessory.rowSelected = 1;
        const auto before = NumpadRead(*f.app); camera_parent_checks::UiProbe probe(f, draft); probe.trigger = phase; unsigned calls = 0;
        probe.callback = [&](UINT) { ++calls;
            if (mutation == 0) f.app->CameraPositionY() += 11;
            if (mutation == 1) f.app->AdvanceSceneGeneration();
            if (mutation == 2) f.app->PlaybackActive() = 1;
            if (mutation == 3) { ForgetSceneObject(f.second.raw()); SceneObjectIdentity(f.second.raw()); }
            if (mutation == 4) f.app->FrameRangeDialog() = f.app->MainWindow();
        };
        bool changed = true; const auto result = ApplyBoundCameraParentModel(*f.app, noop ? 0 : 1, changed);
        // An unused target is deliberately irrelevant to a same-model no-op.
        const bool irrelevant = noop && mutation == 3;
        Check(calls == 1 && (irrelevant ? result == MMS_OK : result != MMS_OK) && !changed && !f.history.UndoCount() &&
            !f.app->SceneModified() && f.app->CameraParentModel() == before.parentModel && FollowHasPendingEdit(draft),
            "direct model apply keeps its pre-draft source and readiness, including no-op");
        if (!irrelevant) Check(f.app->GlobalTrackSelected(GlobalTimelineTrack::Light) && f.accessory.rowSelected,
            "draft refusal does not perform the initial selection side effect");
        f.app->FrameRangeDialog() = nullptr;
    }
}
void UnicodeDraftAndReadout() {
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    const auto input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 80, 140, 20, f.app->MainWindow(),
        reinterpret_cast<HMENU>(INT_PTR(0x7c01)), GetModuleHandleW(nullptr), nullptr);
    Check(input && IsWindowUnicode(input), "create actual Unicode draft control independent of ACP conversion");
    constexpr wchar_t raw[] = L"未確定 -\xd83d\xde00 1.";
    SetWindowTextW(input, raw); SendMessageW(input, EM_SETMODIFY, TRUE, 0); SendMessageW(input, EM_SETSEL, 3, 7); SetFocus(f.app->MainWindow());
    SetWindowTextW(f.Edit(panel::kReadoutPosYEdit), L"obsolete"); SendMessageW(f.Edit(panel::kReadoutPosYEdit), EM_SETMODIFY, FALSE, 0);
    f.ChooseModel(-1);
    wchar_t actual[64]{}; GetWindowTextW(input, actual, 64); DWORD first = 0, last = 0;
    SendMessageW(input, EM_GETSEL, WPARAM(&first), LPARAM(&last));
    Check(!std::wcscmp(actual, raw) && SendMessageW(input, EM_GETMODIFY, 0, 0) && first == 3 && last == 7,
        "attachment readback retains Unicode unfinished draft bytes, selection and modified state after focus loss");
    char value[64]{}, expected[64]{}; GetWindowTextA(f.Edit(panel::kReadoutPosYEdit), value, 64);
    sprintf_s(expected, "%1.2f", double(f.app->CameraPositionY()));
    Check(!std::strcmp(value, expected) && f.history.UndoCount() == 1, "unmodified main camera numeric readout follows the accepted detach without an extra edit");
    DestroyWindow(input);
}
void PendingDuringDraft() {
    for (bool noop : {false, true}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        camera_multiply_checks::PluginState plugin; StorageClient storage; storage.id = "test.model-draft-pending"; f.storage.Api(storage);
        auto serializer = plugin.Serializer(); MmsHandle provider = 0;
        Check(f.storage.Register(storage, Text(storage.id), &serializer, sizeof serializer, &provider) == MMS_OK, "register late-pending draft serializer");
        EditStore edits(f.history, f.storage); EditClient client; client.id = storage.id; const auto api = edits.Api(client);
        std::uint64_t revision = 0; Check(f.history.Revision(revision) == MMS_OK, "capture pending-draft journal revision"); MmsHandle tx = 0;
        const auto draft = FollowPendingEdit(*f.app, panel::kReadoutPosXEdit);
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light); f.accessory.rowSelected = 1;
        camera_parent_checks::UiProbe probe(f, draft); probe.trigger = WM_GETTEXT; unsigned calls = 0;
        probe.callback = [&](UINT) { ++calls; Check(api.begin(api.context, Text("draft pending"), revision, &tx) == MMS_OK, "begin a real transaction during native draft capture"); };
        bool changed = true; const auto before = NumpadRead(*f.app);
        const auto result = ApplyBoundCameraParentModel(*f.app, noop ? 0 : 1, changed);
        Check(calls == 1 && result == MMS_E_BUSY && !changed && edits.HasPending() && !f.history.UndoCount() &&
            NumpadSame(NumpadRead(*f.app), before) && f.app->GlobalTrackSelected(GlobalTimelineTrack::Light) && f.accessory.rowSelected,
            "late pending transaction blocks changed and no-op selection effects without being cancelled");
        api.cancel(api.context, tx); Check(!edits.HasPending(), "only the original caller cancels its pending transaction");
        Check(f.storage.Unregister(storage, provider) == MMS_OK, "release late-pending serializer after exact cancellation");
    }
}
void DraftNativeIdentity() {
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Select(1);
        const auto before = NumpadRead(*f.app);
        const auto draft = FollowPendingEdit(*f.app, panel::kReadoutPosXEdit);
        camera_parent_checks::UiProbe probe(f, draft); probe.trigger = WM_GETTEXT; unsigned calls = 0;
        probe.callback = [&](UINT) { ++calls;
            if (mutation == 0) SendMessageW(f.models, CB_SETCURSEL, 1, 0);
            if (mutation == 1) SendMessageW(f.models, CB_SETITEMDATA, 2, 0);
            if (mutation == 2) {
                DestroyWindow(f.combo); f.combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
                    0, 26, 190, 150, f.app->MainWindow(), reinterpret_cast<HMENU>(INT_PTR(450)), GetModuleHandleW(nullptr), nullptr);
                Check(f.combo != nullptr, "replace the bone-control instance during native draft capture");
            }
        };
        f.Dispatch();
        Check(calls == 1 && NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount() && !f.app->SceneModified() && FollowHasPendingEdit(draft),
            "actual 449 refuses UI-only selection, mapping or control replacement during draft capture");
    }
}
void RealComboCaptureAndMixedPlugin() {
    Scene f; NativePoseTestBinding binding(*f.app, f.history); f.modelForward = true;
    SendMessageW(f.models, CB_SHOWDROPDOWN, TRUE, 0); COMBOBOXINFO info{sizeof(info)};
    Check(GetComboBoxInfo(f.models, &info) && info.hwndList, "obtain actual parent-model ComboLBox"); RECT rect{};
    Check(SendMessageW(info.hwndList, LB_GETITEMRECT, 2, LPARAM(&rect)) != LB_ERR, "obtain actual target model row");
    const auto point = MAKELPARAM((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
    SendMessageW(info.hwndList, WM_LBUTTONDOWN, MK_LBUTTON, point); SendMessageW(info.hwndList, WM_LBUTTONUP, 0, point);
    SendMessageW(f.models, CB_SHOWDROPDOWN, FALSE, 0); ReleaseCapture();
    Check(f.modelNotifications && f.app->CameraParentModel() == 1 && f.history.UndoCount() == 1, "real ComboLBox notifies one actual common-history model edit");
    for (bool popup : {false, true}) {
        Scene held; NativePoseTestBinding heldBinding(*held.app, held.history); held.Select(1); COMBOBOXINFO ci{sizeof(ci)};
        Check(GetComboBoxInfo(held.models, &ci) && ci.hwndList, "get exact capture-owned native model handles");
        const auto target = popup ? ci.hwndList : held.models; SetCapture(target); Check(GetCapture() == target, "hold exact owned combo capture"); held.Dispatch();
        Check(held.app->CameraParentModel() == 1 && held.history.UndoCount() == 1 && !GetPropW(held.models, L"MMS.Native.CameraParentCapture.V1"),
            "model notification admits only its exact native capture and removes authorization after publication"); ReleaseCapture();
    }
    camera_multiply_checks::PluginState plugin; StorageClient storage; storage.id = "test.camera-parent-model"; f.storage.Api(storage);
    auto serializer = plugin.Serializer(); MmsHandle provider = 0;
    Check(f.storage.Register(storage, Text(storage.id), &serializer, sizeof serializer, &provider) == MMS_OK, "register independent mixed model serializer");
    EditStore edits(f.history, f.storage); EditClient client; client.id = storage.id; const auto api = edits.Api(client);
    std::uint64_t revision = 0; MmsHandle tx = 0; Check(f.history.Revision(revision) == MMS_OK && api.begin(api.context, Text("model plugin"), revision, &tx) == MMS_OK, "begin real pending plugin transaction");
    f.ChooseModel(0); Check(f.app->CameraParentModel() == 1 && f.history.UndoCount() == 1 && edits.HasPending() && f.ModelSelection() == 1,
        "pending plugin work consumes 449 without fallback or cancelling the other transaction");
    const auto participant = plugin.Participant(Text(client.id));
    Check(api.join(api.context, tx, &participant) == MMS_OK && api.prepare(api.context, tx) == MMS_OK && api.commit(api.context, tx) == MMS_OK,
        "the refused model change leaves the original plugin transaction usable");
    f.ChooseModel(0); Check(f.history.UndoCount() == 3 && plugin.value == 42, "plugin and two parent-model edits share one chronology");
    Check(f.history.History(false) == MMS_OK && f.app->CameraParentModel() == 1 && f.history.History(false) == MMS_OK && plugin.value == 0 &&
        f.history.History(false) == MMS_OK && f.app->CameraParentModel() == 0, "mixed plugin/model nodes undo in exact reverse order");
    for (unsigned i = 0; i < 3; ++i) Check(f.history.History(true) == MMS_OK, "redo one mixed plugin/model node");
    Check(f.history.Reset() == MMS_OK, "release mixed parent-model history before its serializer"); f.history.Collect();
    Check(f.storage.Unregister(storage, provider) == MMS_OK, "unregister mixed serializer after record release");
}
struct LifecycleMark {
    const char* text;
    ~LifecycleMark() { std::puts(text); }
};
void TraceHistory(const char* stage, Scene& f, const RetainedSceneObject* owner = nullptr) {
    std::uint64_t revision = UINT64_MAX; const auto result = f.history.Revision(revision);
    MmsObjectRef identity{}; const bool known = FindSceneObjectIdentity(f.second.raw(), identity);
    std::printf("449_LIFETIME %s parent=%d/%d frame=%d slots=%p/%p Bexpected=%p owner=%p present=%d undo=%zu redo=%zu revResult=%d revision=%llu scene=%llu binding=%llu pool=%llu Aorder=%u/%p Border=%u/%p Aselectors=%p Bselectors=%p BidKnown=%u Bgen=%llu\n",
        stage, f.app->CameraParentModel(), f.app->CameraParentBone(), f.app->CurrentFrame(),
        static_cast<void*>(f.app->ModelSlot(0)), static_cast<void*>(f.app->ModelSlot(1)), static_cast<void*>(f.second.raw()),
        static_cast<const void*>(owner), owner ? int(owner->Present()) : -1, f.history.UndoCount(), f.history.RedoCount(), int(result),
        static_cast<unsigned long long>(revision), static_cast<unsigned long long>(f.app->SceneGeneration()),
        static_cast<unsigned long long>(BoundNativePoseGeneration(f.app.get())), static_cast<unsigned long long>(mdl::KeyPoolIdentity(f.app->CameraKeys())),
        unsigned(f.first.m->boneOrderCount), static_cast<void*>(f.first.m->boneOrderTable),
        unsigned(f.second.m->boneOrderCount), static_cast<void*>(f.second.m->boneOrderTable),
        static_cast<void*>(f.first.m->displayKeys[0].selectorStates), static_cast<void*>(f.second.m->displayKeys[0].selectorStates),
        unsigned(known), static_cast<unsigned long long>(identity.generation));
}
void RemovalHistory() {
    std::puts("449_REMOVAL before Scene construction");
    LifecycleMark afterScene{"449_REMOVAL Scene destruction returned"};
    {
        // Declared before Scene: even exceptional fixture teardown retains
        // valid table storage until all scene/history owners have released.
        std::array<mdl::BoneOrderEntry, 1> aOrder{{{0, 0, 1, 1, 1}}};
        std::array<mdl::BoneOrderEntry, 1> bOrder{{{0, 0, 1, -1, 0}}};
        Scene f; TraceHistory("removal Scene constructed", f);
        auto* const oldAOrder = f.first.m->boneOrderTable;
        auto* const oldBOrder = f.second.m->boneOrderTable;
        const auto oldACount = f.first.m->boneOrderCount, oldBCount = f.second.m->boneOrderCount;
        auto* const selector = f.first.m->displayKeys[0].selectorStates;
        Check(oldACount == 1 && oldBCount == 1 && selector && f.second.m->displayKeys[0].selectorStates,
            "removal fixture keeps nonzero native order counts and real allocated root selectors");
        const auto oldSelector = selector[0];
        f.first.m->boneOrderTable = aOrder.data(); f.second.m->boneOrderTable = bOrder.data();
        selector[0] = {1, 1};
        const auto bindings = [&](bool linked) {
            return f.first.m->boneOrderCount == 1 && f.second.m->boneOrderCount == 1 &&
                f.first.m->boneOrderTable == aOrder.data() && f.second.m->boneOrderTable == bOrder.data() &&
                aOrder[0].boneIndex == 0 && aOrder[0].windowStart == 0 && aOrder[0].windowEnd == 1 &&
                aOrder[0].linkedModel == (linked ? 1 : -1) && aOrder[0].linkedBone == (linked ? 1 : 0) &&
                bOrder[0].boneIndex == 0 && bOrder[0].windowStart == 0 && bOrder[0].windowEnd == 1 &&
                bOrder[0].linkedModel == -1 && bOrder[0].linkedBone == 0 &&
                f.first.m->displayKeys[0].selectorStates == selector &&
                selector[0].modelIndex == (linked ? 1 : -1) && selector[0].boneIndex == (linked ? 1 : 0);
        };
        Check(bindings(true), "valid A order and root display selector both reference existing B bone 1");
        TraceHistory("removal valid external order/selector fixture installed", f);
        {
            LifecycleMark afterBinding{"449_REMOVAL binding destruction returned"};
            NativePoseTestBinding binding(*f.app, f.history);
            {
                LifecycleMark afterOwners{"449_REMOVAL transaction/change/owner local destruction returned"};
                std::shared_ptr<RetainedSceneObject> owner; std::shared_ptr<HistoryChange> removal;
                std::unique_ptr<SharedHistory::NativeTransaction> tx;
                try {
                    TraceHistory("before real 449 B notification", f); f.ChooseModel(1); TraceHistory("after real 449 B notification", f);
                    TraceHistory("before RetainSceneObject B", f); owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 1); TraceHistory("after RetainSceneObject B", f, owner.get());
                    TraceHistory("before actual CaptureRemoval(64)", f, owner.get()); removal = owner->CaptureRemoval(64); TraceHistory("after actual CaptureRemoval(64)", f, owner.get());
                    Check(bindings(true), "actual removal capture leaves A's external order and selector untouched");
                    TraceHistory("before actual PrepareNative removal", f, owner.get());
                    const auto prepared = f.history.PrepareNative("remove parent B", removal, tx);
                    std::printf("449_REMOVAL PrepareNative returned=%d tx=%p\n", int(prepared), static_cast<void*>(tx.get())); TraceHistory("after actual PrepareNative removal", f, owner.get());
                    Check(prepared == MMS_OK, "real object-removal transaction prepares all incoming camera bindings");
                    Check(bindings(true), "removal preparation remains private for the complete A-to-B binding set");
                    TraceHistory("before actual CommitNative removal", f, owner.get()); const auto committed = f.history.CommitNative(std::move(tx));
                    std::printf("449_REMOVAL CommitNative returned=%d\n", int(committed)); TraceHistory("after actual CommitNative removal", f, owner.get());
                    Check(committed == MMS_OK && !f.app->ModelSlot(1) && f.app->CameraParentModel() == -1,
                        "real object-removal transaction clears incoming camera binding atomically");
                    Check(bindings(false), "actual deletion clears A's bone-order and root selector references to B");
                    TraceHistory("before actual undo deletion", f, owner.get()); const auto undoDelete = f.history.History(false);
                    std::printf("449_REMOVAL undo deletion returned=%d\n", int(undoDelete)); TraceHistory("after actual undo deletion", f, owner.get());
                    Check(undoDelete == MMS_OK && f.app->ModelSlot(1) == f.second.raw() && f.app->CameraParentModel() == 1,
                        "undo deletion restores the retained owner and its camera binding");
                    Check(bindings(true) && owner->Present(), "undo deletion restores exact A-to-B order and selector references with B present");
                    TraceHistory("before actual undo 449", f, owner.get()); const auto undoModel = f.history.History(false);
                    std::printf("449_REMOVAL undo 449 returned=%d\n", int(undoModel)); TraceHistory("after actual undo 449", f, owner.get());
                    Check(undoModel == MMS_OK && f.app->CameraParentModel() == 0, "undo earlier 449 follows the restored model owner");
                    Check(bindings(true), "449 undo leaves the restored model/selector dependency separate from camera authoring");
                    TraceHistory("before actual redo 449", f, owner.get()); const auto redoModel = f.history.History(true);
                    std::printf("449_REMOVAL redo 449 returned=%d\n", int(redoModel)); TraceHistory("after actual redo 449", f, owner.get());
                    Check(redoModel == MMS_OK && f.app->CameraParentModel() == 1, "redo model edit without reopening an original file");
                    Check(bindings(true), "449 redo preserves independent A-to-B order and selector references");
                    TraceHistory("before actual redo deletion", f, owner.get()); const auto redoDelete = f.history.History(true);
                    std::printf("449_REMOVAL redo deletion returned=%d\n", int(redoDelete)); TraceHistory("after actual redo deletion", f, owner.get());
                    Check(redoDelete == MMS_OK && !f.app->ModelSlot(1), "redo actual object deletion without reopening an original file");
                    Check(bindings(false) && !owner->Present(), "deletion redo clears both external references again while parking B");
                    TraceHistory("before final actual undo deletion for fixture ownership", f, owner.get()); const auto restore = f.history.History(false);
                    std::printf("449_REMOVAL final undo deletion returned=%d\n", int(restore)); TraceHistory("after final actual undo deletion", f, owner.get());
                    Check(restore == MMS_OK && f.app->ModelSlot(1) == f.second.raw() && owner->Present() && bindings(true),
                        "restore fixture-owned model and exact external bindings before releasing history");
                    TraceHistory("before transaction/change/owner local destruction", f, owner.get());
                } catch (const std::exception& error) {
                    std::printf("449_REMOVAL caught std::exception: %s\n", error.what()); TraceHistory("removal exception before original rethrow", f, owner.get()); throw;
                } catch (...) {
                    std::puts("449_REMOVAL caught non-standard exception"); TraceHistory("removal exception before original rethrow", f, owner.get()); throw;
                }
            }
            TraceHistory("before removal binding destruction", f);
        }
        TraceHistory("before clearing removal history while private tables/selectors remain valid", f);
        Check(f.app->ModelSlot(0) == f.first.raw() && f.app->ModelSlot(1) == f.second.raw() && bindings(true),
            "both fixture-owned models and all external binding storage remain live before history cleanup");
        const auto reset = f.history.Reset(); std::printf("449_REMOVAL cleanup history Reset returned=%d\n", int(reset));
        Check(reset == MMS_OK, "release removal and model records before restoring borrowed table pointers"); f.history.Collect();
        Check(!f.history.UndoCount() && !f.history.RedoCount() && bindings(true), "history cleanup preserves restored native dependency values");
        f.first.m->boneOrderTable = oldAOrder; f.second.m->boneOrderTable = oldBOrder;
        f.first.m->boneOrderCount = oldACount; f.second.m->boneOrderCount = oldBCount; selector[0] = oldSelector;
        Check(f.first.m->boneOrderTable == oldAOrder && f.second.m->boneOrderTable == oldBOrder &&
            f.first.m->boneOrderCount == oldACount && f.second.m->boneOrderCount == oldBCount &&
            f.first.m->displayKeys[0].selectorStates == selector && !std::memcmp(&selector[0], &oldSelector, sizeof(oldSelector)),
            "restore caller-owned table references and selector contents before normal Scene/Fixture destruction");
        TraceHistory("before removal Scene destruction", f);
    }
}
void RegistrationHistory() {
    std::puts("449_REGISTRATION before Scene construction");
    LifecycleMark afterScene{"449_REGISTRATION Scene destruction returned"};
    Scene f; TraceHistory("registration Scene constructed", f);
    LifecycleMark afterBinding{"449_REGISTRATION binding and registration local destruction returned; next Scene destruction"};
    NativePoseTestBinding binding(*f.app, f.history);
    try {
    for (unsigned i = 0; i < 3; ++i) { f.app->CameraKeys()[i].parentModel = 0; f.app->CameraKeys()[i].parentBone = 0; }
    const auto originalKeys = GlobalRegistrationKeys(f.app->CameraKeys());
    const auto oracle = [&](const std::vector<mdl::CameraKey>& keys) {
        Scene independent; independent.app->CurrentFrame() = f.app->CurrentFrame();
        Check(mdl::KeyPoolEnsure(independent.app->CameraKeys(), keys.size()), "size independent real registration seek oracle");
        std::memcpy(independent.app->CameraKeys(), keys.data(), keys.size() * sizeof(mdl::CameraKey)); ReloadModels(independent.app.get()); return NumpadRead(*independent.app);
    };
    const auto matches = [&](const NumpadView& expected) {
        const auto actual = NumpadRead(*f.app);
        return !std::memcmp(actual.values.data(), expected.values.data(), 7 * sizeof(float)) && actual.values[9] == expected.values[9] &&
            actual.perspective == expected.perspective && actual.parentModel == expected.parentModel && actual.parentBone == expected.parentBone;
    };
    const auto before = NumpadRead(*f.app); TraceHistory("before registration's actual 449 B notification", f);
    f.ChooseModel(1); TraceHistory("after registration's actual 449 B notification", f); const auto adjusted = NumpadRead(*f.app);
    Check(f.app->CameraParentModel() == 1 && GlobalRegistrationSame(f.app->CameraKeys(), originalKeys), "449 B leaves the old registered A track untouched");
    TraceHistory("before actual TryRegisterBoundCamera prepare/commit", f); const auto registered = TryRegisterBoundCamera(f.app.get(), f.app->MainWindow());
    std::printf("449_REGISTRATION TryRegisterBoundCamera returned=%u\n", unsigned(registered)); TraceHistory("after actual TryRegisterBoundCamera prepare/commit", f);
    Check(registered && f.history.UndoCount() == 2, "register B as a separate explicit camera node");
    const auto registeredKeys = GlobalRegistrationKeys(f.app->CameraKeys()); f.app->CurrentFrame() = 59;
    TraceHistory("before independent old registration oracle", f); const auto oldSample = oracle(originalKeys); TraceHistory("after independent old registration oracle destruction", f);
    TraceHistory("before independent new registration oracle", f); const auto newSample = oracle(registeredKeys); TraceHistory("after independent new registration oracle destruction", f);
    TraceHistory("before actual registration undo", f); const auto undoRegister = f.history.History(false);
    std::printf("449_REGISTRATION undo registration returned=%d\n", int(undoRegister)); TraceHistory("after actual registration undo", f);
    Check(undoRegister == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), originalKeys) && matches(oldSample) && f.app->CameraParentModel() == 0,
        "registration undo legitimately evaluates the old A track instead of retaining the unregistered B parent");
    // A->B did not write position, rotation or distance. Preserve that part
    // of registration's seek result, but restore the complete binding pair.
    const auto evaluated = NumpadRead(*f.app); auto undoModel = evaluated, redoModel = evaluated;
    undoModel.parentModel = before.parentModel; redoModel.parentModel = adjusted.parentModel;
    undoModel.parentBone = before.parentBone; redoModel.parentBone = adjusted.parentBone;
    TraceHistory("before registration-chain actual 449 undo", f); const auto undone = f.history.History(false);
    std::printf("449_REGISTRATION undo 449 returned=%d\n", int(undone)); TraceHistory("after registration-chain actual 449 undo", f);
    Check(undone == MMS_OK && NumpadSame(NumpadRead(*f.app), undoModel) && f.app->CurrentFrame() == 59,
        "449 owning parent replays after registration evaluation without demanding an obsolete after-parent");
    TraceHistory("before registration-chain actual 449 redo", f); const auto redone = f.history.History(true);
    std::printf("449_REGISTRATION redo 449 returned=%d\n", int(redone)); TraceHistory("after registration-chain actual 449 redo", f);
    Check(redone == MMS_OK && NumpadSame(NumpadRead(*f.app), redoModel) && f.app->CurrentFrame() == 59,
        "449 redo restores frozen B binding and preserves numeric fields that the original A-to-B switch never wrote");
    TraceHistory("before actual registration redo", f); const auto redoRegister = f.history.History(true);
    std::printf("449_REGISTRATION redo registration returned=%d\n", int(redoRegister)); TraceHistory("after actual registration redo", f);
    Check(redoRegister == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), registeredKeys) && matches(newSample),
        "registration redo still matches the independent existing native seek protocol");
    TraceHistory("before registration locals, binding and Scene destruction", f);
    } catch (const std::exception& error) {
        std::printf("449_REGISTRATION caught std::exception: %s\n", error.what()); TraceHistory("registration exception before original rethrow", f); throw;
    } catch (...) {
        std::puts("449_REGISTRATION caught non-standard exception"); TraceHistory("registration exception before original rethrow", f); throw;
    }
}
void RemovalAndRegistration() { RemovalHistory(); RegistrationHistory(); }
void RegistrationBindingPair(int q) {
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    // A has four bones; B has two. Both q values are valid selectable A
    // sources. q=3 cannot address B, while q=1 is a different valid B bone.
    f.bones[1].type = mdl::BoneType::RotateMove;
    Check((q == 1 || q == 3) && q > 0 && unsigned(q) < f.first.m->boneCount &&
        f.second.m->boneCount == 2 && f.app->CameraParentModel() == 0 && f.app->CameraParentBone() == 0,
        "registration pair fixture begins at A/0 with a distinct valid A/q and two-bone B");
    for (unsigned i = 0; i < 3; ++i) { f.app->CameraKeys()[i].parentModel = 0; f.app->CameraKeys()[i].parentBone = 0; }
    f.app->CameraKeys()[2].parentBone = q; // frame 31 owns A/q at the later replay frame
    const auto originalKeys = GlobalRegistrationKeys(f.app->CameraKeys());
    const auto oracle = [&](const std::vector<mdl::CameraKey>& keys, int frame) {
        Scene independent; independent.bones[1].type = mdl::BoneType::RotateMove;
        independent.app->CurrentFrame() = frame;
        Check(mdl::KeyPoolEnsure(independent.app->CameraKeys(), keys.size()), "size independent binding-pair native seek oracle");
        std::memcpy(independent.app->CameraKeys(), keys.data(), keys.size() * sizeof(mdl::CameraKey));
        ReloadModels(independent.app.get()); return NumpadRead(*independent.app);
    };
    const auto matchesCameraSample = [&](const NumpadView& expected) {
        const auto actual = NumpadRead(*f.app);
        return !std::memcmp(actual.values.data(), expected.values.data(), 7 * sizeof(float)) &&
            actual.values[9] == expected.values[9] && actual.perspective == expected.perspective &&
            actual.parentModel == expected.parentModel && actual.parentBone == expected.parentBone;
    };
    std::printf("449_PAIR begin q=%d bCount=%u case=%s\n", q, unsigned(f.second.m->boneCount), q == 3 ? "out-of-range" : "in-range-nonzero");
    f.ChooseModel(1);
    Check(f.app->CameraParentModel() == 1 && f.app->CameraParentBone() == 0 && f.history.UndoCount() == 1 &&
        GlobalRegistrationSame(f.app->CameraKeys(), originalKeys), "real A/0 to B/0 adjustment leaves the old registered A/q track intact");
    Check(TryRegisterBoundCamera(f.app.get(), f.app->MainWindow()) && f.history.UndoCount() == 2,
        "register actual B/0 as an independent explicit history node");
    const auto registeredKeys = GlobalRegistrationKeys(f.app->CameraKeys());
    unsigned registeredAtSeven = 0;
    for (const auto& key : registeredKeys) if (key.frame == 7) {
        ++registeredAtSeven; Check(key.parentModel == 1 && key.parentBone == 0, "actual registration key contains the complete B/0 pair");
    }
    Check(registeredAtSeven == 1, "exactly one actual B/0 key was registered at the editing frame");
    const auto registeredSample = oracle(registeredKeys, 7);
    Check(registeredSample.parentModel == 1 && registeredSample.parentBone == 0,
        "independent native seek resolves registered B/0 at its exact frame");
    f.app->CurrentFrame() = 59;
    const auto oldSample = oracle(originalKeys, 59), newSample = oracle(registeredKeys, 59);
    Check(oldSample.parentModel == 0 && oldSample.parentBone == q,
        "independent old-track oracle resolves valid A/q at the later replay frame");
    Check(f.history.History(false) == MMS_OK && f.history.UndoCount() == 1 && f.history.RedoCount() == 1 &&
        GlobalRegistrationSame(f.app->CameraKeys(), originalKeys) && matchesCameraSample(oldSample),
        "registration undo genuinely evaluates A/q before the earlier model adjustment replays");
    const auto evaluated = NumpadRead(*f.app);
    auto expectedUndo = evaluated, expectedRedo = evaluated;
    expectedUndo.parentModel = 0; expectedUndo.parentBone = 0;
    expectedRedo.parentModel = 1; expectedRedo.parentBone = 0;
    Check(f.history.History(false) == MMS_OK && f.history.UndoCount() == 0 && f.history.RedoCount() == 2,
        "undo the actual model node after registration's A/q evaluation");
    const auto actualUndo = NumpadRead(*f.app);
    Check(f.history.History(true) == MMS_OK && f.history.UndoCount() == 1 && f.history.RedoCount() == 1,
        "redo the actual model node without resampling the track");
    const auto actualRedo = NumpadRead(*f.app);
    const bool completePair = NumpadSame(actualUndo, expectedUndo) && NumpadSame(actualRedo, expectedRedo);
    if (!completePair) {
        std::printf("449_PAIR mismatch q=%d bCount=%u actualUndo=%d/%d actualRedo=%d/%d redoBoneInRange=%u undo=%zu redo=%zu\n",
            q, unsigned(f.second.m->boneCount), actualUndo.parentModel, actualUndo.parentBone, actualRedo.parentModel, actualRedo.parentBone,
            unsigned(actualRedo.parentBone >= 0 && unsigned(actualRedo.parentBone) < f.second.m->boneCount), f.history.UndoCount(), f.history.RedoCount());
        PrintNine("evaluated A/q", evaluated); PrintNine("undo expected A/0", expectedUndo); PrintNine("undo actual", actualUndo);
        PrintNine("redo expected B/0", expectedRedo); PrintNine("redo actual", actualRedo);
    }
    Check(completePair && f.app->CurrentFrame() == 59 && GlobalRegistrationSame(f.app->CameraKeys(), originalKeys),
        "parent-model replay restores the complete A/0 and B/0 binding pair, never carrying registration's q into B");
    Check(f.history.History(true) == MMS_OK && f.history.UndoCount() == 2 && f.history.RedoCount() == 0 &&
        GlobalRegistrationSame(f.app->CameraKeys(), registeredKeys) && matchesCameraSample(newSample) && f.app->CurrentFrame() == 59,
        "the following registration redo still matches its independent real native seek oracle");
}
}
namespace {
void NativeCameraParentModelRemovalChecks() { camera_parent_model_checks::RemovalHistory(); }
void NativeCameraParentModelRegistrationChecks() { camera_parent_model_checks::RegistrationHistory(); }
void NativeCameraParentModelRegistrationPairChecks() {
    camera_parent_model_checks::RegistrationBindingPair(3);
    camera_parent_model_checks::RegistrationBindingPair(1);
}
void NativeCameraParentModelRegistrationPairOutOfRangeChecks() { camera_parent_model_checks::RegistrationBindingPair(3); }
void NativeCameraParentModelRegistrationPairInRangeChecks() { camera_parent_model_checks::RegistrationBindingPair(1); }
void NativeCameraParentModelNoParentMappingRecoveryChecks() { camera_parent_model_checks::NoParentMappingRecovery(); }
void NativeCameraParentModelAcceptedMappingSwapChecks() { camera_parent_model_checks::AcceptedReadbackMappingSwap(); }
void NativeCameraParentModelHistoryChecks() {
    using namespace camera_parent_model_checks;
    std::puts("  camera parent model: real native oracle, nine fields, both modes and raw values"); NativeOracleAndModes();
    std::puts("  camera parent model: 450/449 chain, frozen pivot and redo policy"); ChainFrozenPivotAndRedoPolicy();
    std::puts("  camera parent model: filtered/reordered identity and safe legacy refusals"); MappingAndLegacyRefusals();
    std::puts("  camera parent model: fallback source replacement and post-read mapping recovery"); LegacyReplacementAndRecoveryVersion();
    std::puts("  camera parent model: no-parent mapping refusal repairs and permits the next real edit"); NoParentMappingRecovery();
    std::puts("  camera parent model: accepted post-read model-row mutation repairs without a second publication"); AcceptedReadbackMappingSwap();
    std::puts("  camera parent model: two-owner lifecycle, generations, logical pool and budgets"); OwnerCandidatesAndBudget();
    std::puts("  camera parent model: allocation sweep, cancellation and allocation-free publication"); AllocationAndPublication();
    std::puts("  camera parent model: native read/draft reentry and busy/no-op rejection"); NativeReentryAndDrafts();
    std::puts("  camera parent model: Unicode unfinished draft and real numeric readout"); UnicodeDraftAndReadout();
    std::puts("  camera parent model: late pending transaction during changed and no-op draft capture"); PendingDuringDraft();
    std::puts("  camera parent model: native control identity retained through draft capture"); DraftNativeIdentity();
    std::puts("  camera parent model: actual ComboLBox/capture and mixed plugin history"); RealComboCaptureAndMixedPlugin();
    std::puts("  camera parent model: actual deletion and registration history protocols"); RemovalAndRegistration();
    std::puts("  camera parent model: registered A/q cannot leak into the restored B/0 binding pair"); NativeCameraParentModelRegistrationPairChecks();
}
}
