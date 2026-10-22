// Include after native_accessory_hud_history_checks.inc in the CPU native
// history executable. All input is sent to this fixture's hidden HWNDs; no
// global cursor/keyboard injection, source-file IO or D3D device is required.
#include "../src/plugins/native_accessory_parent_history.hpp"
#include "mikumikustudio/bone_combo.hpp"
namespace accessory_parent_checks {
using namespace plugins;
using Mode = SharedHistory::RecordingMode;
struct Scene : AccessoryHudFixture {
    HWND models = nullptr, bones = nullptr;
    Scene() {
        first.m->comboSelIndex = 1; second.m->comboSelIndex = 2;
        for (auto* model : {first.m.get(), second.m.get()}) for (unsigned i = 0; i < model->boneCount; ++i) {
            model->boneTable[i].type = mdl::BoneType::RotateMove;
            strcpy_s(model->boneTable[i].name, "duplicate"); strcpy_s(model->boneTable[i].nameEn, "duplicate");
        }
        for (int id : {474, 475}) {
            HWND control = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
                0, id == 474 ? 0 : 26, 190, 150, app->MainWindow(), reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            Check(control != nullptr, "create owned actual accessory model and bone combos");
            (id == 474 ? models : bones) = control;
        }
        accessory.parentModel = 0; accessory.parentBone = 0; app->EnglishUI() = 1;
        app->SceneModified() = app->RecordedFrameCountByte1() = app->PhysicsResetPending() = 0;
        Lists();
    }
    void Lists() {
        SendMessageW(models, CB_RESETCONTENT, 0, 0);
        for (int slot : {-1, 0, 1}) {
            const auto at = SendMessageW(models, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"duplicate"));
            SendMessageW(models, CB_SETITEMDATA, at, slot);
        }
        SelectModel(accessory.parentModel);
        FillBoneCombo(bones, accessory.parentModel >= 0 && accessory.parentModel < kModelSlotCount ? mdl::Mdl(app->ModelSlot(accessory.parentModel)) : nullptr, true);
        SelectBoneCombo(bones, accessory.parentBone);
    }
    void SelectModel(int slot) {
        const auto count = SendMessageW(models, CB_GETCOUNT, 0, 0); LRESULT found = slot < 0 ? 0 : -1;
        for (LRESULT row = 1; row < count; ++row) if (SendMessageW(models, CB_GETITEMDATA, row, 0) == slot) found = row;
        Check(found >= 0, "find accessory parent model by original item identity"); SendMessageW(models, CB_SETCURSEL, found, 0);
    }
    void Dispatch(HWND combo, int id, unsigned notification = CBN_SELCHANGE) {
        auto* saved = g_Block; g_Block = app.get(); CommandDispatch(combo, MAKEWPARAM(id, notification)); g_Block = saved;
    }
    void ChooseModel(int slot) { SelectModel(slot); Dispatch(models, 474); }
    void ChooseBone(int bone) { SelectBoneCombo(bones, bone); Dispatch(bones, 475); }
    int ModelSelection() const {
        const auto row = SendMessageW(models, CB_GETCURSEL, 0, 0);
        return row <= 0 ? -1 : int(SendMessageW(models, CB_GETITEMDATA, row, 0));
    }
    int BoneSelection() const {
        const auto row = SendMessageW(bones, CB_GETCURSEL, 0, 0);
        return row < 0 ? -1 : int(SendMessageW(bones, CB_GETITEMDATA, row, 0));
    }
};
bool Pair(const Scene& f, int model, int bone) { return f.accessory.parentModel == model && f.accessory.parentBone == bone; }
struct Invariants {
    const AccessoryHudInvariant resources;
    const std::array<float, 6> pose;
    const float scale, opacity;
    const unsigned char visible, additive, shadow;
    const int frame; const unsigned char physics;
    explicit Invariants(Scene& f) : resources(f), pose(AccessoryHudRead(f.accessory)), scale(f.accessory.scale), opacity(f.accessory.opacity),
        visible(f.accessory.visible), additive(f.accessory.additiveBlend), shadow(f.accessory.shadowEnabled),
        frame(f.app->CurrentFrame()), physics(f.app->PhysicsResetPending()) {}
    void Unchanged(Scene& f) const {
        resources.CheckUnchanged(f);
        Check(pose == AccessoryHudRead(f.accessory) && scale == f.accessory.scale && opacity == f.accessory.opacity &&
            visible == f.accessory.visible && additive == f.accessory.additiveBlend && shadow == f.accessory.shadowEnabled &&
            f.app->CurrentFrame() == frame && f.app->PhysicsResetPending() == physics,
            "parent authoring preserves pose/appearance, all keys, other objects, physics and the playhead");
    }
};
void NativeOracleAndModes() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (int before : {-1, 0, 1}) for (int target : {-1, 0, 1}) {
        Scene f, oracle;
        for (auto* s : {&f, &oracle}) { s->accessory.parentModel = before; s->accessory.parentBone = 1; s->Lists(); }
        NativePoseTestBinding binding(*f.app, f.history); Check(f.history.SetMode(mode) == MMS_OK, "configure accessory-parent adjustment policy");
        const auto beforeBone = f.accessory.parentBone; Invariants unchanged(f);
        oracle.ChooseModel(target); f.ChooseModel(target);
        const int expectedBone = target < 0 ? beforeBone : 0;
        const bool changed = before != target || beforeBone != expectedBone;
        Check(Pair(f, oracle.accessory.parentModel, oracle.accessory.parentBone) && Pair(f, target, expectedBone) &&
            f.ModelSelection() == oracle.ModelSelection() && f.BoneSelection() == oracle.BoneSelection(),
            "real bound 474 preserves native cross-model attach/reset/detach and bone-list selection");
        Check(f.history.UndoCount() == (changed && mode == Mode::AllOperations ? 1u : 0u) &&
            bool(f.app->SceneModified()) == changed && bool(f.app->RecordedFrameCountByte1()) == changed,
            "474 records one whole adjustment only in all-operations mode and publishes dirty only on change");
        unchanged.Unchanged(f);
        if (changed && mode == Mode::AllOperations) {
            Check(f.history.History(false) == MMS_OK && Pair(f, before, beforeBone), "undo restores the frozen complete parent pair");
            Check(f.history.History(true) == MMS_OK && Pair(f, target, expectedBone), "redo restores the same pair without source IO or live-pose sampling");
        }
    }
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (int model : {0, 1}) {
        Scene f; f.accessory.parentModel = model; f.Lists(); NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "configure actual 475 adjustment policy"); Invariants unchanged(f);
        f.ChooseBone(1);
        Check(Pair(f, model, 1) && f.BoneSelection() == 1 && f.history.UndoCount() == (mode == Mode::AllOperations ? 1u : 0u),
            "real 475 resolves duplicate labels through exact bone item data in both modes");
        unchanged.Unchanged(f);
        if (mode == Mode::AllOperations) Check(f.history.History(false) == MMS_OK && Pair(f, model, 0) &&
            f.history.History(true) == MMS_OK && Pair(f, model, 1), "475 replays through the same paired participant");
    }
    Scene filtered; NativePoseTestBinding binding(*filtered.app, filtered.history);
    filtered.second.bones[0].type = mdl::BoneType::InertTip; filtered.ChooseModel(1);
    Check(Pair(filtered, 1, 0) && filtered.BoneSelection() == 1,
        "474 keeps native stored bone zero and first displayed selectable bone distinct");
}
void HudChronologyAndRedo() {
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    const auto before = AccessoryHudRead(f.accessory);
    Check(BeginAccessoryHudGesture(*f.app, f.history, nullptr, 15) == MMS_OK, "reserve original accessory HUD before binding edits");
    AccessoryHudSample(*f.app, 15, 7); bool changed = false;
    Check(EndAccessoryHudGesture(*f.app, changed) == MMS_OK && changed, "commit the real original accessory HUD gesture");
    const auto after = AccessoryHudRead(f.accessory);
    f.ChooseModel(1); f.ChooseBone(1);
    Check(f.history.UndoCount() == 3 && Pair(f, 1, 1), "HUD, real 474 and real 475 share one chronological journal");
    Check(f.history.History(false) == MMS_OK && Pair(f, 1, 0) && f.history.History(false) == MMS_OK && Pair(f, 0, 0) &&
        f.history.History(false) == MMS_OK && AccessoryHudRead(f.accessory) == before,
        "undo both paired edits before HUD, so the original attachment guard never becomes stale");
    Check(f.history.History(true) == MMS_OK && AccessoryHudRead(f.accessory) == after &&
        f.history.History(true) == MMS_OK && f.history.History(true) == MMS_OK && Pair(f, 1, 1),
        "redo the original HUD and both real native parent commands without weakening its guard");
    Check(f.history.History(false) == MMS_OK, "seed actual redo before native same-value requests");
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
    f.ChooseBone(0); f.ChooseModel(1);
    Check(f.history.UndoCount() == undo && f.history.RedoCount() == redo && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
        "same-value bone and model actions preserve both redo and dirty flags");
    Check(f.history.SetMode(Mode::ExplicitOnly) == MMS_OK, "switch existing journal to explicit-only mode");
    f.ChooseModel(0);
    Check(Pair(f, 0, 0) && f.history.UndoCount() == undo && f.history.RedoCount() == redo,
        "explicit-only parent adjustment preserves existing redo under the shared journal policy");
}
void MappingAndFailureAdmission() {
    for (unsigned invalid = 0; invalid < 9; ++invalid) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); const auto original = f.accessory;
        const auto count = f.second.m->boneCount; auto* table = f.second.m->boneTable;
        f.SelectModel(1);
        if (invalid == 0) SendMessageW(f.models, CB_SETITEMDATA, 2, 0);
        if (invalid == 1) SendMessageW(f.models, CB_SETCURSEL, -1, 0);
        if (invalid == 2) SendMessageW(f.models, CB_DELETESTRING, 1, 0);
        if (invalid == 3) f.second.m->boneTable = nullptr;
        if (invalid == 4) f.second.m->boneCount = 0;
        if (invalid == 5) f.app->ModelSlot(2) = f.second.raw();
        if (invalid == 6) f.accessory.parentBone = INT32_MAX;
        if (invalid == 7) f.app->PlaybackActive() = 1;
        if (invalid == 8) f.app->state.recordPlaybackStartPending = 1;
        const auto stored = f.accessory; f.Dispatch(f.models, 474);
        Check(AccessoryHudSame(f.accessory, stored) && !f.history.UndoCount() && !f.app->SceneModified(),
            "invalid map/source/bone or playback consumes bound 474 without a legacy write");
        f.second.m->boneCount = count; f.second.m->boneTable = table; f.app->ModelSlot(2) = nullptr; f.accessory = original;
        f.app->PlaybackActive() = 0; f.app->state.recordPlaybackStartPending = 0;
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); SelectBoneCombo(f.bones, 1);
        if (invalid == 0) SendMessageW(f.bones, CB_SETITEMDATA, 1, INT32_MAX);
        if (invalid == 1) SendMessageW(f.bones, CB_SETITEMDATA, 1, 0);
        if (invalid == 2) SendMessageW(f.bones, CB_SETCURSEL, -1, 0);
        if (invalid == 3) f.first.bones[1].type = mdl::BoneType::InertTip;
        if (invalid == 4) SendMessageW(f.models, CB_SETCURSEL, 2, 0);
        f.Dispatch(f.bones, 475);
        Check(Pair(f, 0, 0) && !f.history.UndoCount() && !f.app->SceneModified() && f.ModelSelection() == 0 && f.BoneSelection() == 0,
            "out-of-range/duplicate/filtered/mismatched real bone input refuses and restores displayed selections");
    }
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    std::shared_ptr<HistoryChange> change; Check(PlanAccessoryParentBone(*f.app, 1, change) == MMS_OK && change, "prepare private original paired adjustment");
    std::unique_ptr<SharedHistory::NativeTransaction> pending;
    Check(f.history.PrepareNative("pending parent", change, pending, SharedHistory::NativeIntent::Adjustment) == MMS_OK, "hold actual journal reservation before a second native combo command");
    f.ChooseModel(1);
    Check(Pair(f, 0, 0) && f.ModelSelection() == 0 && f.BoneSelection() == 0 && !f.history.UndoCount(),
        "a pending transaction rejects actual 474 and restores its native selection without fallback");
    pending.reset();
    Check(!f.history.UndoCount() && Pair(f, 0, 0), "cancelling the prepared pair publishes no partial field or node");
    const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
    f.Dispatch(f.models, 474, CBN_SELENDCANCEL);
    Check(Pair(f, 0, 0) && f.history.UndoCount() == undo && f.history.RedoCount() == redo, "native dropdown cancellation is not a selection-change transaction");
    Check(f.history.Configure(Mode::AllOperations, 128, 1) == MMS_OK, "configure a real too-small shared resource budget");
    f.ChooseModel(1);
    Check(Pair(f, 0, 0) && f.ModelSelection() == 0 && f.BoneSelection() == 0 && !f.history.UndoCount() && !f.app->SceneModified(),
        "budget refusal restores original combo selections with no partial source publication");
    Check(f.history.Configure(Mode::AllOperations, 128, 256ull * 1024 * 1024) == MMS_OK, "restore normal payload budget");
    f.ChooseModel(1); Check(Pair(f, 1, 0) && f.history.UndoCount() == 1, "the same actual command succeeds after real budget recovery");
}
void CandidatesAndRetainedOwners() {
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); std::shared_ptr<HistoryChange> change;
        Check(PlanAccessoryParentModel(*f.app, 1, change) == MMS_OK && change, "plan both old/new parent owners privately");
        std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(true, candidate) == MMS_OK && candidate->IsCurrent(), "prepare paired candidate with a complete retained bookkeeping charge");
        auto* oldTable = f.second.m->boneTable; const auto oldCount = f.second.m->boneCount;
        if (mutation == 0) ++f.app->CurrentFrame();
        if (mutation == 1) f.app->SelectedAccessorySlot() = 8;
        if (mutation == 2) { NativePoseTestBinding transient(*f.app, f.history); }
        if (mutation == 3) f.accessory.parentBone = 1;
        if (mutation == 4) f.second.m->boneTable = nullptr;
        if (mutation == 5) --f.second.m->boneCount;
        if (mutation == 6) f.app->AdvanceSceneGeneration();
        if (mutation == 7) f.accessory.position[0] += 1;
        if (mutation == 8) {
            auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 1); auto park = owner->PreparePresence(false);
            Check(park->IsCurrent(), "prepare actual target-parent retirement"); park->Publish(); park.reset();
            Check(!candidate->IsCurrent(), "absent new parent invalidates the old candidate before table access");
            auto restore = owner->PreparePresence(true); Check(restore->IsCurrent(), "restore the exact retained target parent"); restore->Publish();
        }
        Check(!candidate->IsCurrent() && !f.history.UndoCount(), "source, frame, binding, identity, pose, table or count change invalidates a short candidate");
        candidate.reset(); f.second.m->boneTable = oldTable; f.second.m->boneCount = oldCount;
    }
    Scene f; NativePoseTestBinding binding(*f.app, f.history); f.ChooseModel(1);
    for (unsigned slot : {0u, 1u}) {
        auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, slot);
        auto park = owner->PreparePresence(false); Check(park->IsCurrent(), "park retained old/new parent between journal traversals"); park->Publish(); park.reset();
        const auto undo = f.history.UndoCount();
        Check(f.history.History(false) == MMS_E_STALE && f.history.UndoCount() == undo,
            "an absent owner blocks the paired replay without advancing history or dereferencing retired storage");
        auto restore = owner->PreparePresence(true); Check(restore->IsCurrent(), "restore original retained parent without a source-file load"); restore->Publish(); restore.reset();
        Check(f.history.History(false) == MMS_OK && Pair(f, 0, 0) && f.history.History(true) == MMS_OK && Pair(f, 1, 0),
            "long-term paired history accepts legitimate same-owner restoration and replays both directions");
    }
    f.app->CurrentFrame() = 87; f.app->SelectedAccessorySlot() = 8; f.accessory.scale = 2.5f;
    Check(f.history.History(false) == MMS_OK && Pair(f, 0, 0) && f.accessory.scale == 2.5f && f.app->CurrentFrame() == 87 && f.app->SelectedAccessorySlot() == 8,
        "paired replay follows its original accessory across selection/frame changes and preserves unrelated fields");
}
void ReplayRejectsChangedBinding() {
    for (bool boneOnly : {false, true}) for (bool changeModel : {false, true}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        if (boneOnly) f.ChooseBone(1); else f.ChooseModel(1);
        const auto model = f.accessory.parentModel, bone = f.accessory.parentBone;
        if (changeModel) f.accessory.parentModel = model ? 0 : 1;
        else f.accessory.parentBone = bone ? 0 : 1;
        const auto unexpected = f.accessory;
        const auto undo = f.history.UndoCount(), redo = f.history.RedoCount();
        Check(f.history.History(false) == MMS_E_STALE && AccessoryHudSame(f.accessory, unexpected) &&
            f.history.UndoCount() == undo && f.history.RedoCount() == redo,
            "paired replay refuses an externally changed parent field without overwriting it or advancing history");
        f.accessory.parentModel = model; f.accessory.parentBone = bone;
        Check(f.history.History(false) == MMS_OK && Pair(f, 0, 0) && f.history.History(true) == MMS_OK && Pair(f, model, bone),
            "restoring the expected pair allows the original same-object journal to traverse again");
    }
}
void AllocationsAndNoAllocationPublication() {
    bool complete = false;
    for (int failure = 0; failure < 256; ++failure) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); const auto before = f.accessory;
        f.SelectModel(1); failAfter = failure; f.Dispatch(f.models, 474); const auto remaining = failAfter; failAfter = -1;
        if (f.accessory.parentModel == 1) {
            Check(Pair(f, 1, 0) && f.history.UndoCount() == 1, "post-publication UI allocation failure cannot roll back or duplicate the accepted pair");
            if (remaining > 0) { complete = true; break; }
        } else {
            Check(AccessoryHudSame(f.accessory, before) && !f.history.UndoCount() && !f.app->SceneModified() &&
                !f.app->RecordedFrameCountByte1() && f.ModelSelection() == 0 && f.BoneSelection() == 0,
                "every rejected allocation point preserves complete accessory bytes/history/dirty and restores healthy combo selections");
        }
    }
    Check(complete, "actual 474 allocation sweep reaches one complete uninjected command");
    Scene f; NativePoseTestBinding binding(*f.app, f.history); std::shared_ptr<HistoryChange> change;
    Check(PlanAccessoryParentModel(*f.app, 1, change) == MMS_OK && change, "capture one reusable paired participant");
    for (unsigned direction = 0; direction < 6; ++direction) {
        std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(!(direction % 2), candidate) == MMS_OK && candidate->IsCurrent(), "prepare alternating paired replay entirely before publication");
        failAfter = 0; const auto current = candidate->IsCurrent(); if (current) candidate->Publish(); failAfter = -1;
        Check(current && Pair(f, direction % 2 ? 0 : 1, 0), "final retained validation and paired scalar publication allocate nothing");
    }
}
}
void NativeAccessoryParentHistoryChecks() {
    accessory_parent_checks::NativeOracleAndModes();
    accessory_parent_checks::HudChronologyAndRedo();
    accessory_parent_checks::MappingAndFailureAdmission();
    accessory_parent_checks::CandidatesAndRetainedOwners();
    accessory_parent_checks::ReplayRejectsChangedBinding();
    accessory_parent_checks::AllocationsAndNoAllocationPublication();
}
