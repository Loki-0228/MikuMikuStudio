// Include after native_accessory_hud_history_checks.inc in the CPU native
// history executable. All input is sent to this fixture's hidden HWNDs; no
// global cursor/keyboard injection, source-file IO or D3D device is required.
#include "../src/plugins/native_accessory_parent_history.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "../src/plugins/native_global_paste.hpp"
#include "../src/plugins/native_curve_write.hpp"
namespace mikumikustudio { void InitExportPlaybackTracks(MMDApp*,double); void SampleGlobalPlaybackTracks(MMDApp*); }
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
    Check(f.history.History(true) == MMS_OK && Pair(f, 1, 1),
        "redo reconciles a trusted unrecorded parent draft before the older bone node");
    Check(f.history.History(false) == MMS_OK && Pair(f, 1, 0) &&
        f.history.History(false) == MMS_OK && Pair(f, 0, 0) &&
        f.history.History(false) == MMS_OK && AccessoryHudRead(f.accessory) == before,
        "undo still traverses paired nodes and the original HUD after cancelling the trusted draft");
}
void TrustedDraftBeforeHudAndRegistrationSampling() {
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); const auto before = AccessoryHudRead(f.accessory);
        Check(BeginAccessoryHudGesture(*f.app, f.history, nullptr, 15) == MMS_OK, "begin original HUD before a trusted unrecorded attachment draft");
        AccessoryHudSample(*f.app, 15, 5); bool changed = false;
        Check(EndAccessoryHudGesture(*f.app, changed) == MMS_OK && changed, "record original HUD on parent zero");
        const auto after = AccessoryHudRead(f.accessory);
        Check(f.history.SetMode(Mode::ExplicitOnly) == MMS_OK, "switch HUD journal to explicit-only"); f.ChooseModel(1);
        Check(f.history.UndoCount() == 1 && Pair(f, 1, 0), "parent draft changes only live attachment and retains original HUD node");
        Check(f.history.History(false) == MMS_OK && Pair(f, 0, 0) && AccessoryHudRead(f.accessory) == before,
            "undo atomically reconciles the trusted parent draft and the original HUD axis");
        Check(f.history.History(true) == MMS_OK && Pair(f, 0, 0) && AccessoryHudRead(f.accessory) == after,
            "redo keeps reconciled attachment and original frozen HUD result");
    }
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        f.app->LightKeys() = mdl::KeyPoolCreate<mdl::LightKey>(4);
        f.app->ShadowKeys() = mdl::KeyPoolCreate<mdl::SelfShadowKey>(4);
        f.app->GravityKeys() = mdl::KeyPoolCreate<mdl::GravityKey>(4);
        Check(CreateWindowExW(0, L"EDIT", L"1", WS_CHILD, 0, 0, 80, 20, f.app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(484)), GetModuleHandleW(nullptr), nullptr) != nullptr,
            "create the seventh real 487 numeric input");
        auto* keys = f.app->AccessoryKeys(7);
        for (std::size_t i = 0; i < mdl::KeyPoolCapacity(keys); ++i) { keys[i].parentModel = 0; keys[i].parentBone = 0; }
        f.ChooseModel(1); Check(f.history.UndoCount() == 1 && Pair(f, 1, 0), "record real 474 before explicit key registration");
        f.Dispatch(nullptr, 487, BN_CLICKED);
        Check(f.history.UndoCount() == 2 && Pair(f, 1, 0), "real menu 487 registers the new parent as a separate explicit node");
        Check(f.history.History(false) == MMS_OK && Pair(f, 0, 0), "undo registration preserves its native old-track resampling rule");
        Check(f.history.History(false) == MMS_OK && Pair(f, 0, 0),
            "previous parent undo recognizes a trusted track resample without relaxing paired corruption checks");
        Check(f.history.History(true) == MMS_OK && Pair(f, 1, 0) && f.history.History(true) == MMS_OK && Pair(f, 1, 0),
            "parent and registration redo share the same logical attachment provenance");
        Check(f.history.Reset() == MMS_OK, "retire key registration before fixture global pools"); f.history.Collect();
        mdl::KeyPoolDestroy(f.app->LightKeys()); f.app->LightKeys() = nullptr;
        mdl::KeyPoolDestroy(f.app->ShadowKeys()); f.app->ShadowKeys() = nullptr;
        mdl::KeyPoolDestroy(f.app->GravityKeys()); f.app->GravityKeys() = nullptr;
    }
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
void RecordedAfterDraftAndLogicalOwnership() {
    Scene f; NativePoseTestBinding binding(*f.app,f.history);
    const auto before=AccessoryHudRead(f.accessory);
    Check(BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"begin HUD logical baseline before trusted draft");
    AccessoryHudSample(*f.app,15,3);bool changed=false;
    Check(EndAccessoryHudGesture(*f.app,changed)==MMS_OK&&changed,"commit baseline HUD before unrecorded parent");
    const auto after=AccessoryHudRead(f.accessory);
    Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"begin unrecorded model-one draft");f.ChooseModel(1);
    Check(f.history.SetMode(Mode::AllOperations)==MMS_OK,"record after the trusted draft");f.ChooseModel(-1);
    Check(f.history.UndoCount()==2&&Pair(f,-1,0),"recorded draft-to-detached change is one additional node");
    Check(f.history.History(false)==MMS_OK&&Pair(f,1,0),"new node undo restores physical draft one but logical parent zero");
    Check(f.history.History(false)==MMS_OK&&Pair(f,0,0)&&AccessoryHudRead(f.accessory)==before,
        "older HUD undo reconciles the preserved logical baseline");
    Check(f.history.History(true)==MMS_OK&&Pair(f,0,0)&&AccessoryHudRead(f.accessory)==after&&
        f.history.History(true)==MMS_OK&&Pair(f,-1,0),"redo new node accepts trusted logical zero after older redo, not only physical draft one");
    auto retained=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
    const auto undo=f.history.UndoCount();auto park=retained->PreparePresence(false);park->Publish();park.reset();
    Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==undo&&Pair(f,-1,0),
        "logical-only model zero remains a validated retained endpoint, not a fresh table lookup during undo");
    auto restore=retained->PreparePresence(true);restore->Publish();restore.reset();
    Check(f.history.History(false)==MMS_OK&&Pair(f,1,0),"exact restored logical owner permits replay without source IO");
    // A raw write which happens to match the historical logical endpoint is
    // still not a receipt: physical one was the last trusted publication.
    f.accessory.parentModel=0;const auto count=f.history.UndoCount();
    Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==count&&Pair(f,0,0),
        "coincidental raw historical pair cannot launder an external write into provenance");
    f.accessory.parentModel=1;
    Check(f.history.History(false)==MMS_OK&&Pair(f,0,0),"restoring exact trusted actual pair allows the original HUD undo");
}
void EvaluationReceiptsAndReset() {
    // These are real CPU evaluation entry points, not a direct invocation of
    // the provenance notifier. They do not claim visible/GPU playback testing.
    for(unsigned evaluator=0;evaluator<3;++evaluator){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);
        auto* keys=f.app->AccessoryKeys(7);
        for(unsigned i=0;i<3;++i){keys[i].parentModel=i==1?1:0;keys[i].parentBone=0;}
        f.ChooseModel(1);Check(f.history.UndoCount()==1,"install one authoring receipt before actual native seek");
        f.app->CurrentFrame()=31;
        const auto revisionBefore=f.app->SceneRevision();const bool dirty=bool(f.app->SceneModified());
        const auto frame=f.app->CurrentFrame();
        if(evaluator==0)ApplyAccessoryTrack(f.app.get(),7);
        else if(evaluator==1){
            // Use the real exported initialization entry. Leave global tracks
            // nonterminal so this CPU fixture does not need a render device.
            Check(!f.app->LightKeys()&&!f.app->ShadowKeys()&&!f.app->GravityKeys(),"export-init fixture owns its three new global pools");
            f.app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(4);
            f.app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(4);
            f.app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(4);
            f.app->CameraKeys()[0].frame=f.app->LightKeys()[0].frame=
                f.app->ShadowKeys()[0].frame=f.app->GravityKeys()[0].frame=100;
            InitExportPlaybackTracks(f.app.get(),32.0);
            mdl::KeyPoolDestroy(f.app->LightKeys());f.app->LightKeys()=nullptr;
            mdl::KeyPoolDestroy(f.app->ShadowKeys());f.app->ShadowKeys()=nullptr;
            mdl::KeyPoolDestroy(f.app->GravityKeys());f.app->GravityKeys()=nullptr;
        }
        else{f.app->AccessoryTrackActive(7)=1;f.app->AccessoryTrackCursor(7)=0;f.app->PlaybackCursorSeconds()=31.0/30.0;
            SampleGlobalPlaybackTracks(f.app.get());}
        Check(Pair(f,0,0)&&f.history.UndoCount()==1&&f.app->CurrentFrame()==frame&&
            f.app->SceneRevision()==revisionBefore&&f.app->SceneModified()==dirty,
            "seek, track initialization and playback advance update actual receipt without authoring side effects");
        Check(f.history.History(false)==MMS_OK&&Pair(f,0,0)&&f.history.History(true)==MMS_OK&&Pair(f,1,0),
            "older parent journal traverses after actual native evaluator changed its live attachment");
    }
    Scene f;NativePoseTestBinding binding(*f.app,f.history);
    AccessoryAttachmentHistory source;Check(source.Begin(*f.app,f.history,7)==MMS_OK,"prepare a private uninstalled domain");
    std::unique_ptr<AccessoryAttachmentHistory::Prepared> prepared;
    Check(source.Prepare({0,0},{1,0},{1,0},prepared)==MMS_OK&&prepared->IsCurrent(),"capture uninstalled receipt without live mutation");
    Check(f.history.Reset()==MMS_OK&&!prepared->IsCurrent()&&Pair(f,0,0),"Reset invalidates even never-installed receipt domains");
    prepared.reset();source=AccessoryAttachmentHistory{};
    f.ChooseModel(1);Check(f.history.UndoCount()==1,"retry after canceled and reset private domains");
    std::weak_ptr<RetainedSceneObject> oldOwner=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
    Check(!oldOwner.expired(),"native node retains original parent independently of metadata cells");
    Check(f.history.Reset()==MMS_OK,"reset installed receipt and native nodes");f.history.Collect();
    Check(oldOwner.expired(),"fixed active metadata domain keeps no heavy object owner after history collection");
    f.ChooseModel(0);Check(f.history.UndoCount()==1,"installed fixed domain remains reusable after reset");
    Check(f.history.Reset()==MMS_OK&&f.history.Configure(Mode::AllOperations,1,256ull*1024*1024)==MMS_OK,
        "configure one retained node to exercise real pruning");f.history.Collect();f.ChooseModel(1);
    std::weak_ptr<RetainedSceneObject> pruned=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
    f.ChooseBone(1);f.history.Collect();
    Check(f.history.UndoCount()==1&&pruned.expired()&&Pair(f,1,1),"pruning the only model-zero node releases its owner while receipt metadata remains bounded");
}
void AuthoredCurveEvaluationReceipt(){
    {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);
    for(unsigned i=0;i<3;++i){auto& key=f.app->AccessoryKeys(7)[i];key.parentModel=0;key.parentBone=0;}
    f.ChooseModel(1);std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,channels,error)&&channels.size()==12,
        "read real accessory curve channels before controlled curve edit");
    Check(!channels[6].track.keys.empty(),"real accessory scale channel has native keys");
    channels[6].track.keys.front().value+=.25;
    std::shared_ptr<HistoryChange> edit;Check(PlanGlobalCurveWrite(*f.app,curve_host::Kind::Accessory,7,channels,edit)==MMS_OK&&edit,
        "plan actual original accessory curve writer");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("accessory authored curve",edit,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK&&Pair(f,1,0),
        "scale-only controlled curve publication preserves unowned parent fields");
    f.app->CurrentFrame()=31;curve_host::Apply(f.app.get(),curve_host::Kind::Accessory,31,7);
    Check(Pair(f,0,0)&&f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK&&Pair(f,0,0)&&
        f.history.History(true)==MMS_OK&&Pair(f,1,0)&&f.history.History(true)==MMS_OK&&Pair(f,1,0),
        "real authored evaluation and curve undo/redo leave the older parent journal traversable");
    Check(f.history.Reset()==MMS_OK,"retire real curve nodes before fixture authoring reset");f.history.Collect();curve_host::Reset(f.app.get());
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);
        for(unsigned i=0;i<3;++i){auto& key=f.app->AccessoryKeys(7)[i];key.parentModel=0;key.parentBone=0;}
        f.ChooseModel(1);std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,channels,error)&&channels.size()==12,"read actual parent model curve lane");
        for(auto& key:channels[10].track.keys)key.value=-1;
        std::shared_ptr<HistoryChange> edit;std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(PlanGlobalCurveWrite(*f.app,curve_host::Kind::Accessory,7,channels,edit)==MMS_OK&&edit&&
            f.history.PrepareNative("owned parent curve",edit,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK&&Pair(f,-1,0),
            "owned parent lane curve publication actually writes and confirms its frozen pair");
        Check(f.history.History(false)==MMS_OK&&Pair(f,0,0)&&f.history.History(false)==MMS_OK&&Pair(f,0,0)&&
            f.history.History(true)==MMS_OK&&Pair(f,1,0)&&f.history.History(true)==MMS_OK&&Pair(f,-1,0),
            "owned parent curve undo/redo preserves older parent provenance across actual pair writes");
        Check(f.history.Reset()==MMS_OK,"retire parent-lane authoring history");f.history.Collect();curve_host::Reset(f.app.get());
    }
}
void VmdMappingAndOpaqueSamplerBoundary(){
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);f.ChooseModel(1);
        strcpy_s(f.accessory.name,"parent-vmd");
        VmdDecodeBytes bytes;VmdDecodeText(bytes,"Vocaloid Motion Data 0002",30);
        VmdDecodeText(bytes,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",20);
        for(unsigned section=0;section<6;++section)VmdDecodePut(bytes,0u);
        accessory_motion_io::Track track;track.name="parent-vmd";
        accessory_motion_io::Key key;key.parentModel=0;key.parentBone=0;track.keys.push_back(key);
        const auto tail=accessory_motion_io::Encode({track});bytes.insert(bytes.end(),tail.begin(),tail.end());
        VmdDecodeFile file;file.Write(bytes);std::shared_ptr<HistoryChange> import;
        Check(PlanGlobalVmdImport(*f.app,nullptr,file.path.c_str(),import)==MMS_OK&&import,"plan real accessory VMD through resolved mapping wrapper");
        std::unique_ptr<SharedHistory::NativeTransaction> pending;
        Check(f.history.PrepareNative("mapped accessory VMD",import,pending)==MMS_OK&&f.history.CommitNative(std::move(pending))==MMS_OK&&Pair(f,0,0),
            "mapping wrapper forwards initial journal context to its actual accessory paste");
        Check(f.history.History(false)==MMS_OK&&Pair(f,-1,0)&&f.history.History(false)==MMS_OK&&Pair(f,0,0)&&
            f.history.History(true)==MMS_OK&&Pair(f,1,0)&&f.history.History(true)==MMS_OK&&Pair(f,0,0),
            "mapped VMD old-track sampling and original parent history traverse in both directions");
    }
    for(bool activeReceipt:{false,true}){
        Scene f;
        if(activeReceipt){NativePoseTestBinding binding(*f.app,f.history);f.ChooseModel(1);}
        std::vector<curve_editor::Channel> channels;std::wstring error;unsigned calls=0;
        Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,channels,error),"read unbound channels for unsupported sampler fixture");
        channels[0].sample=[&](const curve_editor::Channel& channel,double frame){++calls;return animation_curve::Evaluate(channel.track,frame);};
        Check(curve_host::Write(*f.app,curve_host::Kind::Accessory,7,channels,error),"existing unbound opaque sampler retains legacy authoring behavior");
        const auto pose=f.accessory;const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();std::uint64_t revision=0;
        Check(f.history.Revision(revision)==MMS_OK,"capture history before rejecting unsupported bound sampler");calls=0;
        {
            NativePoseTestBinding binding(*f.app,f.history);curve_host::Apply(f.app.get(),curve_host::Kind::Accessory,7,7);std::uint64_t after=0;
            Check(f.history.Revision(after)==MMS_OK&&after==revision&&calls==0&&AccessoryHudSame(f.accessory,pose)&&
                f.history.UndoCount()==undo&&f.history.RedoCount()==redo,"bound opaque sampler is rejected before callbacks or pose/provenance/history writes, with or without an active receipt");
            // Reset only the unsupported authoring fixture; it is not a
            // journal reset and must not invalidate the original receipt.
            curve_host::Reset(f.app.get());
            if(activeReceipt)Check(f.history.History(false)==MMS_OK&&Pair(f,0,0)&&f.history.History(true)==MMS_OK&&Pair(f,1,0),
                "safe sampler refusal leaves the old parent receipt normally traversable");
        }
    }
}
void ReusedAccessorySlotObservesOldReceipt(){
    Scene f;
    AccessoryAttachmentHistory uninstalled;
    {NativePoseTestBinding binding(*f.app,f.history);
        Check(uninstalled.Begin(*f.app,f.history,7)==MMS_OK,"capture old uninstalled handle before any publication");f.ChooseModel(1);}
    auto old=RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,7);
    auto park=old->PreparePresence(false);Check(park->IsCurrent(),"park original receipt source through real retained presence");park->Publish();park.reset();
    auto* blank=f.app->AccessoryKeys(7);
    mdl::AccessoryRecord replacement{};replacement.parentModel=1;replacement.parentBone=0;replacement.scale=replacement.opacity=1;replacement.visible=1;
    auto* keys=mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
    keys[0].parentModel=0;keys[0].parentBone=0;keys[0].scale=keys[0].opacity=1;keys[0].visible=1;
    f.app->AccessorySlot(7)=&replacement;f.app->AccessoryKeys(7)=keys;SceneObjectIdentity(&replacement);
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,channels,error),"read new object and pool in reused accessory slot");
    channels[0].track.keys.front().value=4;
    Check(curve_host::Write(*f.app,curve_host::Kind::Accessory,7,channels,error),"install legal new-object authoring through existing unbound writer");
    {
        NativePoseTestBinding binding(*f.app,f.history);const auto undo=f.history.UndoCount();
        curve_host::Apply(f.app.get(),curve_host::Kind::Accessory,7,7);
        Check(replacement.parentModel==0&&replacement.parentBone==0&&replacement.position[0]==4&&f.history.UndoCount()==undo,
            "old source metadata does not permanently reject legal bound curve evaluation on a new object and pool");
        AccessoryAttachmentHistory newSource;std::unique_ptr<AccessoryAttachmentHistory::Prepared> next;
        Check(newSource.Begin(*f.app,f.history,7)==MMS_OK&&newSource.Prepare({0,0},{1,0},{1,0},next)==MMS_OK&&next->IsCurrent(),
            "new source can publish its own receipt during same-slot identity turnover");next->Publish();
    }
    curve_host::Reset(f.app.get());ForgetSceneObject(&replacement);f.app->AccessorySlot(7)=nullptr;
    mdl::KeyPoolDestroy(keys);f.app->AccessoryKeys(7)=blank;
    auto restore=old->PreparePresence(true);Check(restore->IsCurrent(),"restore exact old accessory source and original owned pool");restore->Publish();restore.reset();
    {
        NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.History(false)==MMS_OK&&Pair(f,0,0),"restore old initialized source history after a different source published in its slot");
        std::unique_ptr<AccessoryAttachmentHistory::Prepared> rejected;
        Check(uninstalled.Prepare({0,0},{1,0},{1,0},rejected)==MMS_E_STALE&&!rejected&&Pair(f,0,0),
            "uninstalled old handle stays stale after real different-source publication and original-object restoration");
        Check(f.history.History(true)==MMS_OK&&Pair(f,1,0),
            "observer-only evaluation never rewrites old receipt, so exact restored old source history remains traversable");
    }
    {
        Scene same;std::vector<curve_editor::Channel> state;
        Check(curve_host::Read(*same.app,curve_host::Kind::Accessory,7,state,error),"read same-source bare corruption fixture");
        state[0].track.keys.front().value=8;
        Check(curve_host::Write(*same.app,curve_host::Kind::Accessory,7,state,error),"install canonical authored curves before same-source receipt");
        NativePoseTestBinding binding(*same.app,same.history);same.ChooseModel(1);same.accessory.parentModel=0;
        const auto unexpected=same.accessory;const auto undo=same.history.UndoCount();
        curve_host::Apply(same.app.get(),curve_host::Kind::Accessory,7,7);
        Check(AccessoryHudSame(same.accessory,unexpected)&&same.history.History(false)==MMS_E_STALE&&same.history.UndoCount()==undo,
            "same-source naked pair corruption is not downgraded to observer-only evaluation or granted new provenance");
        same.accessory.parentModel=1;
        Check(same.history.History(false)==MMS_OK&&Pair(same,0,0),"restoring actual trusted pair preserves original journal behavior");
        Check(same.history.Reset()==MMS_OK,"retire canonical corruption fixture");same.history.Collect();curve_host::Reset(same.app.get());
    }
}
void UninstalledHandlesCannotRenewPublication(){
    Scene f;NativePoseTestBinding binding(*f.app,f.history);
    AccessoryAttachmentHistory first,old;
    Check(first.Begin(*f.app,f.history,7)==MMS_OK&&old.Begin(*f.app,f.history,7)==MMS_OK,"two initial handles share one prepared domain but retain separate unpublished cells");
    std::unique_ptr<AccessoryAttachmentHistory::Prepared> accepted,obsolete;
    Check(first.Prepare({0,0},{1,0},{0,0},accepted)==MMS_OK&&old.Prepare({0,0},{1,0},{1,0},obsolete)==MMS_OK,
        "prepare competing same-slot candidates before first installation");
    accepted->Publish();Check(!obsolete->IsCurrent()&&Pair(f,1,0),"first same-slot publication invalidates the other already prepared token");
    std::unique_ptr<AccessoryAttachmentHistory::Prepared> restore;
    Check(first.Prepare({0,0},{0,0},{0,0},restore)==MMS_OK&&restore->IsCurrent(),"trusted initialized source returns actual pair to old raw expected value");restore->Publish();
    const auto pose=f.accessory;const auto count=f.history.UndoCount();
    std::unique_ptr<AccessoryAttachmentHistory::Prepared> renewed;
    Check(old.Prepare({0,0},{1,0},{1,0},renewed)==MMS_E_STALE&&!renewed&&AccessoryHudSame(f.accessory,pose)&&f.history.UndoCount()==count,
        "reprepare cannot wash a never-installed old handle by capturing the newer slot serial when raw values coincide");
}
struct RemovalScene:Scene {
    std::array<mdl::BoneOrderEntry,1> firstOrder{},secondOrder{};
    std::array<std::shared_ptr<RetainedSceneObject>,2> owners;
    RemovalScene(){
        firstOrder[0].linkedModel=secondOrder[0].linkedModel=-1;
        first.m->boneOrderTable=firstOrder.data();second.m->boneOrderTable=secondOrder.data();
        for(unsigned i=0;i<2;++i)owners[i]=RetainSceneObject(*app,MMS_OBJECT_MODEL,i);
    }
    ~RemovalScene(){
        history.Reset();history.Collect();
        // Fixture storage is borrowed stack/new storage. Restore any parked
        // model before releasing its real retained owner and base fixture.
        for(unsigned i=0;i<2;++i)if(!app->ModelSlot(i)){auto restore=owners[i]->PreparePresence(true);Check(restore->IsCurrent(),"cleanup restores borrowed model fixture");restore->Publish();}
    }
    void RefreshParentControls(){
        SendMessageW(models,CB_RESETCONTENT,0,0);auto none=SendMessageW(models,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"none"));SendMessageW(models,CB_SETITEMDATA,none,-1);
        std::vector<std::pair<unsigned,unsigned>> order;
        for(unsigned slot=0;slot<2;++slot)if(app->ModelSlot(slot))order.push_back({mdl::Mdl(app->ModelSlot(slot))->comboSelIndex,slot});
        std::sort(order.begin(),order.end());for(const auto& item:order){auto at=SendMessageW(models,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"duplicate"));SendMessageW(models,CB_SETITEMDATA,at,item.second);}
        auto* object=app->AccessorySlot(7);const int parent=object?object->parentModel:-1;
        SelectModel(parent);FillBoneCombo(bones,parent>=0&&app->ModelSlot(parent)?mdl::Mdl(app->ModelSlot(parent)):nullptr,true);
        if(object)SelectBoneCombo(bones,object->parentBone);
    }
};
void CommitRemoval(RemovalScene& f,const std::shared_ptr<HistoryChange>& change){
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("real retained model/accessory removal",change,token)==MMS_OK,"prepare real retained removal with binding metadata budget");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"retained membership, raw bindings and direction receipt publish without allocation");
}
void RemovalReceiptChronology(){
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        RemovalScene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure actual deletion chronology mode");
        std::shared_ptr<HistoryChange> parent;Check(PlanAccessoryParentBone(*f.app,1,parent)==MMS_OK&&parent,"plan explicit old parent endpoint");CommitRemoval(f,parent);
        const auto before=AccessoryHudRead(f.accessory);
        if(mode==Mode::AllOperations){Check(BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"record original HUD before parent model removal");
            AccessoryHudSample(*f.app,15,3);bool changed=false;Check(EndAccessoryHudGesture(*f.app,changed)==MMS_OK&&changed,"commit old HUD on removed parent");}
        const auto after=AccessoryHudRead(f.accessory);
        auto removal=f.owners[0]->CaptureRemoval(4096);CommitRemoval(f,removal);
        Check(!f.app->ModelSlot(0)&&Pair(f,-1,0),"actual retained deletion detaches removed live parent");
        f.RefreshParentControls();f.ChooseModel(1);
        Check(Pair(f,1,0)&&f.history.UndoCount()==(mode==Mode::AllOperations?4u:2u),"new real 474 edit works on after-removal canonical receipt in both modes");
        if(mode==Mode::AllOperations)Check(f.history.History(false)==MMS_OK&&Pair(f,-1,0),"undo new recorded binding before undoing deletion");
        Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)==f.first.raw()&&Pair(f,0,1),"undo deletion restores old canonical receipt and retained model together, including an unrecorded new draft");
        if(mode==Mode::AllOperations)Check(f.history.History(false)==MMS_OK&&AccessoryHudRead(f.accessory)==before,"older HUD undo remains valid after full deletion roundtrip");
        Check(f.history.History(false)==MMS_OK&&Pair(f,0,0),"older paired parent undo remains valid after deletion and later edits");
        Check(f.history.History(true)==MMS_OK&&Pair(f,0,1),"redo older paired parent");
        if(mode==Mode::AllOperations)Check(f.history.History(true)==MMS_OK&&AccessoryHudRead(f.accessory)==after,"redo older HUD after restored model");
        Check(f.history.History(true)==MMS_OK&&!f.app->ModelSlot(0)&&Pair(f,-1,0),"redo deletion restores after canonical endpoint");
        if(mode==Mode::AllOperations)Check(f.history.History(true)==MMS_OK&&Pair(f,1,0),"redo later binding uses the same after-removal canonical cell");
    }
}
void LogicalRemovalDynamicActual(){
    for(bool registration:{false,true}){
        RemovalScene f;NativePoseTestBinding binding(*f.app,f.history);const auto before=AccessoryHudRead(f.accessory);
        Check(BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"record logical model-zero HUD baseline");AccessoryHudSample(*f.app,15,4);bool changed=false;
        Check(EndAccessoryHudGesture(*f.app,changed)==MMS_OK&&changed,"commit old logical model-zero HUD node");
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"create trusted model-one draft with logical model zero");f.ChooseModel(1);
        Check(f.history.SetMode(Mode::AllOperations)==MMS_OK,"restore recorded operations after trusted draft");
        auto* keys=f.app->AccessoryKeys(7);for(unsigned i=0;i<3;++i){keys[i].parentModel=1;keys[i].parentBone=1;}
        auto removal=f.owners[0]->CaptureRemoval(4096);CommitRemoval(f,removal);
        Check(!f.app->ModelSlot(0)&&Pair(f,1,0),"logical-only parent removal preserves unrelated live model-one pair");
        if(registration){
            f.app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(4);f.app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(4);f.app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(4);
            Check(CreateWindowExW(0,L"EDIT",L"1",WS_CHILD,0,0,80,20,f.app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(484)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"create actual registration scale input after logical removal");
            f.Dispatch(nullptr,487,BN_CLICKED);Check(f.history.UndoCount()==3,"actual 487 joins logical removal chronology");
            Check(f.history.History(false)==MMS_OK&&Pair(f,1,1),"undo actual 487 resamples old track into third legal pair D");
        }else{f.app->CurrentFrame()=12;ApplyAccessoryTrack(f.app.get(),7);Check(Pair(f,1,1),"real seek changes receipt actual to third pair D");}
        Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)&&Pair(f,1,1),"undo logical-only removal preserves trusted dynamic D and restores logical model zero");
        Check(f.history.History(false)==MMS_OK&&Pair(f,0,0)&&AccessoryHudRead(f.accessory)[0]==before[0],"old HUD undo still follows restored logical baseline after dynamic D and preserves unrelated sampled axes");
        Check(f.history.History(true)==MMS_OK&&Pair(f,0,0)&&f.history.History(true)==MMS_OK&&!f.app->ModelSlot(0)&&Pair(f,-1,0),
            "redo removal clears a newly evaluated removed parent even though first deletion changed only logical metadata");
        Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)&&Pair(f,0,0),"undo restores actual cleared by the latest redo, not the first capture pair B");
        if(registration){Check(f.history.Reset()==MMS_OK,"retire actual registration before destroying its three global pools");f.history.Collect();
            mdl::KeyPoolDestroy(f.app->LightKeys());f.app->LightKeys()=nullptr;mdl::KeyPoolDestroy(f.app->ShadowKeys());f.app->ShadowKeys()=nullptr;mdl::KeyPoolDestroy(f.app->GravityKeys());f.app->GravityKeys()=nullptr;}
    }
    RemovalScene fresh;NativePoseTestBinding binding(*fresh.app,fresh.history);
    for(unsigned i=0;i<3;++i){fresh.app->AccessoryKeys(7)[i].parentModel=0;fresh.app->AccessoryKeys(7)[i].parentBone=1;}
    CommitRemoval(fresh,fresh.owners[0]->CaptureRemoval(4096));
    Check(fresh.history.History(false)==MMS_OK&&fresh.app->ModelSlot(0),"undo first removal initializes controlled synthetic-before source without prior native history");
    fresh.app->CurrentFrame()=12;ApplyAccessoryTrack(fresh.app.get(),7);Check(Pair(fresh,0,1),"real seek updates synthetic-before exact source receipt");
    Check(fresh.history.History(true)==MMS_OK&&!fresh.app->ModelSlot(0)&&Pair(fresh,-1,0)&&
        fresh.history.History(false)==MMS_OK&&fresh.app->ModelSlot(0)&&Pair(fresh,0,1),"synthetic-before redo detects and clears future sampled removed parent, then restores this actual pair");
}
void AccessoryPresenceCanonicalRestore(){
    RemovalScene f;NativePoseTestBinding binding(*f.app,f.history);f.ChooseBone(1);
    auto a=RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,7);CommitRemoval(f,a->CaptureRemoval(4096));Check(!f.app->AccessorySlot(7),"delete original attachment A through real retained binding/presence aggregate");
    auto* blank=f.app->AccessoryKeys(7);auto* b=new mdl::AccessoryRecord{};b->parentModel=0;b->parentBone=0;b->scale=b->opacity=1;b->visible=1;b->order=7;
    auto* bkeys=mdl::KeyPoolCreate<mdl::AccessoryKey>(4);bkeys[0].parentModel=0;bkeys[0].parentBone=0;bkeys[0].scale=bkeys[0].opacity=1;
    f.app->AccessorySlot(7)=b;f.app->AccessoryKeys(7)=bkeys;SceneObjectIdentity(b);mdl::KeyPoolDestroy(blank);
    auto bowner=RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,7);std::shared_ptr<HistoryChange> insertion;std::unique_ptr<PreparedHistoryChange> staging;
    Check(bowner->PrepareInsertion(4096,insertion,staging)==MMS_OK&&staging->IsCurrent(),"prepare real B insertion and private removal staging");staging->Publish();staging.reset();
    CommitRemoval(f,insertion);Check(f.app->AccessorySlot(7)==b,"restore B through real reverse removal insertion participant");
    f.RefreshParentControls();f.ChooseModel(1);Check(b->parentModel==1&&f.history.UndoCount()==4,"real B parent edit publishes a distinct canonical source in reused slot");
    Check(f.history.History(false)==MMS_OK&&b->parentModel==0,"undo B edit leaves its first trusted canonical cell available for the original import participant");
    std::unique_ptr<PreparedHistoryChange> removalProbe;std::uint64_t firstRemovalBytes=0;
    Check(insertion->Prepare(false,removalProbe)==MMS_OK&&removalProbe->IsCurrent()&&removalProbe->TargetRetainedBytes(firstRemovalBytes)&&
        firstRemovalBytes>insertion->RetainedBytes(),"first dynamic canonical capture is included in the complete import/removal target budget");
    removalProbe.reset();const auto beforeRefusal=f.history.RetainedBytes();const auto nativeBeforeRefusal=insertion->RetainedBytes();
    failAfter=0;const auto refused=insertion->Prepare(false,removalProbe);failAfter=-1;
    Check(refused==MMS_E_MEMORY&&!removalProbe&&f.history.RetainedBytes()==beforeRefusal&&insertion->RetainedBytes()==nativeBeforeRefusal&&
        f.app->AccessorySlot(7)==b&&b->parentModel==0,"failed dynamic canonical preparation changes neither persistent metadata charge, live presence nor history accounting");
    Check(f.history.History(false)==MMS_OK&&!f.app->AccessorySlot(7),"undo original import captures B canonical receipt at the real removal publication");
    for(unsigned round=0;round<3;++round){
        Check(f.history.History(true)==MMS_OK&&f.app->AccessorySlot(7)==b&&b->parentModel==0,"redo B presence restores the captured canonical cell before another removal");
        std::uint64_t repeatedBytes=0;
        Check(insertion->Prepare(false,removalProbe)==MMS_OK&&removalProbe->IsCurrent()&&removalProbe->TargetRetainedBytes(repeatedBytes)&&repeatedBytes==firstRemovalBytes,
            "later removal targets retain the first dynamically captured cell charge instead of subtracting a mutable binding base");
        removalProbe.reset();Check(f.history.History(false)==MMS_OK&&!f.app->AccessorySlot(7),"repeat real import undo without losing canonical provenance or cumulative metadata budget");
    }
    Check(
        f.history.History(false)==MMS_OK&&f.app->AccessorySlot(7)==&f.accessory&&Pair(f,0,1),"undo B edit and insertion then original A deletion restores original canonical A cell");
    f.RefreshParentControls();f.ChooseBone(0);Check(Pair(f,0,0)&&f.history.History(false)==MMS_OK&&Pair(f,0,1)&&f.history.History(false)==MMS_OK&&Pair(f,0,0),
        "new restored-A edit and undo do not fork away canonical A from its older parent node");
}
void RemovalAdmissionAndShortTokens(){
    RemovalScene f;NativePoseTestBinding binding(*f.app,f.history);f.ChooseBone(1);
    auto removal=f.owners[0]->CaptureRemoval(4096);std::unique_ptr<SharedHistory::NativeTransaction> token;
    const auto original=f.accessory;const auto undo=f.history.UndoCount();
    Check(f.history.PrepareNative("cancel removal",removal,token)==MMS_OK,"prepare complete removal privately");token.reset();
    Check(f.app->ModelSlot(0)&&AccessoryHudSame(f.accessory,original)&&f.history.UndoCount()==undo,"cancel prepared removal installs no cells or live changes");
    Check(f.history.Configure(Mode::AllOperations,128,1)==MMS_OK,"configure below real removal metadata/resource cost");
    Check(f.history.PrepareNative("budget refused",removal,token)==MMS_E_MEMORY&&!token&&f.app->ModelSlot(0)&&AccessoryHudSame(f.accessory,original),
        "budget refusal cannot install after-receipts or detach live fields");
    Check(f.history.Configure(Mode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore native resource budget");
    // Configure may prune the older node but must not publish any metadata.
    removal=f.owners[0]->CaptureRemoval(4096);
    std::unique_ptr<PreparedHistoryChange> prepared;Check(removal->Prepare(true,prepared)==MMS_OK&&prepared->IsCurrent(),"capture real binding/presence short token");
    {NativePoseTestBinding nested(*f.app,f.history);}Check(!prepared->IsCurrent(),"temporary binding invalidates only the old short removal candidate");prepared.reset();
    // The never-published plan retains its initial binding stamp; recapture
    // after that explicit cancellation rather than laundering its old token.
    removal=f.owners[0]->CaptureRemoval(4096);CommitRemoval(f,removal);
    {NativePoseTestBinding nested(*f.app,f.history);}Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0),"published deletion history reparses a fresh short binding generation on same-journal replay");
    removal=f.owners[0]->CaptureRemoval(4096);Check(removal->Prepare(true,prepared)==MMS_OK,"prepare reset invalidation candidate");
    Check(f.history.Reset()==MMS_OK&&!prepared->IsCurrent()&&f.app->ModelSlot(0),"Reset invalidates removal metadata even before installation");
    prepared.reset();
    bool completed=false;unsigned failures=0;
    for(int limit=0;limit<256;++limit){
        failAfter=limit;MmsResult result=MMS_OK;std::shared_ptr<HistoryChange> candidate;
        try{candidate=f.owners[0]->CaptureRemoval(4096);result=f.history.PrepareNative("allocation refused",candidate,token);}catch(const std::bad_alloc&){result=MMS_E_MEMORY;}
        const auto remaining=failAfter;failAfter=-1;
        Check(f.app->ModelSlot(0)&&AccessoryHudSame(f.accessory,original)&&!f.history.UndoCount(),"every failed/cancelled capture and preparation leaves live bindings, epoch and history unchanged");
        token.reset();if(result==MMS_OK&&remaining>0){completed=true;break;}Check(result==MMS_OK||result==MMS_E_MEMORY,"allocation sweep rejects only at actual allocations");if(result==MMS_E_MEMORY)++failures;
    }
    Check(completed&&failures,"removal metadata allocation sweep reaches a complete uninjected candidate");
}
void ExplicitPluginAndPasteCoordination(){
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);camera_multiply_checks::PluginState plugin;
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"explicit-only plugin/native composition policy");
        StorageClient storage;storage.id="test.accessory-parent";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
        Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof(serializer),&provider)==MMS_OK,"register composed parent test state");
        EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);
        std::shared_ptr<HistoryChange> change;Check(PlanAccessoryParentModel(*f.app,1,change)==MMS_OK&&change,"plan native parent before plugin reservation");
        std::uint64_t revision=0;MmsHandle transaction=0;constexpr char label[]="explicit composed parent";
        Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,{label,sizeof(label)-1},revision,&transaction)==MMS_OK,"begin actual plugin transaction in explicit-only mode");
        auto participant=plugin.Participant(Text(client.id));
        Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&
            api.prepare(api.context,transaction)==MMS_OK&&api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&Pair(f,1,0)&&f.history.UndoCount()==1,
            "actual JoinNative initial record intent overrides adjustment mode inference");
        Check(api.undo(api.context)==MMS_OK&&plugin.value==0&&Pair(f,0,0)&&api.redo(api.context)==MMS_OK&&plugin.value==42&&Pair(f,1,0),
            "explicit plugin and native parent remain one reversible publication");
        Check(f.history.Reset()==MMS_OK,"retire composed parent serializer");f.history.Collect();
        Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister composed test serializer");
    }
    for(bool cancel:{false,true}){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);
        // Build two accessory families in the original aggregate. The builder
        // groups repeated source records by slot, so no overlapping same-slot
        // candidates can reach the publication loop.
        GlobalPasteSource input;input.frameOffset=unsigned(f.app->CurrentFrame());
        for(unsigned slot:{7u,8u}){mdl::AccessoryClipboardKey key{};key.slot=std::uint8_t(slot);key.parentModel=1;key.parentBone=0;
            key.visible=1;key.scale=key.opacity=1;input.accessories.push_back(key);}
        std::shared_ptr<HistoryChange> paste;Check(PlanGlobalKeyPaste(*f.app,input,paste)==MMS_OK&&paste,"plan first two-slot paste against one journal");
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("two accessory paste",paste,token)==MMS_OK,"both accessory parts share the prepared unpublished domain");
        if(cancel){token.reset();paste.reset();Check(!f.history.UndoCount()&&Pair(f,0,0)&&f.other.parentModel==-1,
            "canceling first two-slot reservation installs no receipt or partial pair");
            Check(PlanGlobalKeyPaste(*f.app,input,paste)==MMS_OK&&paste&&f.history.PrepareNative("retry two accessory paste",paste,token)==MMS_OK,
                "expired weak domain reservation is reusable after all canceled owners are gone");}
        failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
        Check(result==MMS_OK&&Pair(f,1,0)&&f.other.parentModel==1&&f.history.UndoCount()==1,"first two-slot paste publishes with no allocation and one journal node");
        Check(f.history.History(false)==MMS_OK&&Pair(f,-1,0)&&f.other.parentModel==-1&&f.history.History(true)==MMS_OK&&Pair(f,1,0)&&f.other.parentModel==1,
            "unbound-origin native paste sampling and both coordinated parts replay as one cohort");
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
    const auto run=[](const char* stage,void(*check)()){std::printf("  accessory parent stage: %s\n",stage);std::fflush(stdout);check();};
    run("native oracle and modes",accessory_parent_checks::NativeOracleAndModes);
    run("HUD chronology and redo",accessory_parent_checks::HudChronologyAndRedo);
    run("trusted draft and actual registration",accessory_parent_checks::TrustedDraftBeforeHudAndRegistrationSampling);
    run("mapping and failure admission",accessory_parent_checks::MappingAndFailureAdmission);
    run("candidates and retained owners",accessory_parent_checks::CandidatesAndRetainedOwners);
    run("reject externally changed binding",accessory_parent_checks::ReplayRejectsChangedBinding);
    run("record after draft and logical ownership",accessory_parent_checks::RecordedAfterDraftAndLogicalOwnership);
    run("real evaluation and Reset",accessory_parent_checks::EvaluationReceiptsAndReset);
    run("actual authored curve evaluation",accessory_parent_checks::AuthoredCurveEvaluationReceipt);
    run("VMD mapping and opaque callback",accessory_parent_checks::VmdMappingAndOpaqueSamplerBoundary);
    run("reused accessory source slot",accessory_parent_checks::ReusedAccessorySlotObservesOldReceipt);
    run("uninstalled competing handles",accessory_parent_checks::UninstalledHandlesCannotRenewPublication);
    run("model removal receipt chronology",accessory_parent_checks::RemovalReceiptChronology);
    run("logical removal dynamic actual",accessory_parent_checks::LogicalRemovalDynamicActual);
    run("accessory presence canonical restore",accessory_parent_checks::AccessoryPresenceCanonicalRestore);
    run("removal admission and short tokens",accessory_parent_checks::RemovalAdmissionAndShortTokens);
    run("explicit plugin and multi-accessory paste",accessory_parent_checks::ExplicitPluginAndPasteCoordination);
    run("allocations and no-allocation publication",accessory_parent_checks::AllocationsAndNoAllocationPublication);
}
