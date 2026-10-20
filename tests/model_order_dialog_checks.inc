// Real menu 288/289 resources, controls and dialog procedures. No synthetic
// WM_INITDIALOG or private apply helper substitutes for the user-facing path.
#include "mikumikustudio/scene_history_ui.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include <limits>
namespace mikumikustudio { void RefreshModelSelectorNames(MMDApp*); }
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/plugin_scene.hpp"
#include "../src/plugins/native_model_order.hpp"
#include "../src/plugins/edit_store.hpp"
#endif

namespace model_order_dialog_checks {
constexpr std::array<int, 3> kSlots{2, 9, 254};
constexpr std::array<int, 3> kCombos{436, 449, 474};
int failures = 0;
int moveRow = -1;
int moveCommand = 0;
bool cancel = false;
int dialogs = 0;
std::array<std::wstring, 3> initialLabels;
bool faultRound = false, rejectInserts = false;
int faultStage = 0;
int listFaultMode = 0, listFaultStage = 0, listInsertions = 0;
bool listFaultEnabled = false;
bool invalidScratchRound = false;
std::int32_t invalidScratchValue = 0;
constexpr UINT kFaultContinue = WM_APP + 0x135;
constexpr UINT_PTR kFaultSubclass = 0x4d4f4435;

void Expect(bool value, const char* message) {
    if (!value) { ++failures; std::fprintf(stderr, "MODEL-ORDER FAIL: %s\n", message); }
}
std::wstring Text(HWND window, UINT lengthMessage, UINT textMessage, int row) {
    const auto size = SendMessageW(window, lengthMessage, row, 0);
    Check(size >= 0, "model-order row exists");
    std::wstring result(static_cast<std::size_t>(size) + 1, L'\0');
    Check(SendMessageW(window, textMessage, row, reinterpret_cast<LPARAM>(result.data())) == size,
          "read complete model-order Unicode row");
    result.resize(static_cast<std::size_t>(size));
    return result;
}
LRESULT CALLBACK RejectInsert(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == CB_INSERTSTRING && rejectInserts) return CB_ERRSPACE;
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK RejectListWrite(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (listFaultEnabled && message == LB_INSERTSTRING) {
        if (listFaultMode == 1) { listFaultEnabled = false; return LB_ERRSPACE; }
        if (++listInsertions > 1) return LB_ERRSPACE;
    }
    if (listFaultEnabled && listFaultMode == 2 && message == LB_DELETESTRING) return LB_ERR;
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK ListFaultDialog(HWND dialog, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == kFaultContinue) {
        listFaultStage = static_cast<int>(wp);
        if (wp == 1) {
            SendMessageW(dialog, WM_COMMAND, 632, 0);
            PostMessageW(dialog, kFaultContinue, 2, 0);
        } else {
            listFaultEnabled = false;
            RemoveWindowSubclass(GetDlgItem(dialog, 628), RejectListWrite, kFaultSubclass);
            SendMessageW(dialog, WM_COMMAND, 632, 0);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(dialog, ListFaultDialog, kFaultSubclass);
    return DefSubclassProc(dialog, message, wp, lp);
}
LRESULT CALLBACK FaultDialog(HWND dialog, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == kFaultContinue) {
        faultStage = static_cast<int>(wp);
        if (wp == 1) {
            SendMessageW(dialog, WM_COMMAND, 632, 0);
            for (int i = 0; i != 3; ++i)
                Expect(mdl::Mdl(g_Block->ModelSlot(kSlots[i]))->comboSelIndex == i + 1,
                       "a failed refresh restores the complete original native draw order");
            Expect(g_Block->SceneModified() == 0, "a refused order publication does not mark the project modified");
            PostMessageW(dialog, kFaultContinue, 2, 0);
        } else if (wp == 2) {
            SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
            SendMessageW(dialog, WM_CLOSE, 0, 0);
            SendMessageW(dialog, WM_SYSCOMMAND, SC_CLOSE, 0);
            Expect(g_Block->AccessoryOrderArray() != nullptr,
                   "failed cancel repair keeps its live modal scratch until recovery");
            PostMessageW(dialog, kFaultContinue, 3, 0);
        } else {
            rejectInserts = false;
            RemoveWindowSubclass(GetDlgItem(g_Block->MainWindow(), 436), RejectInsert, kFaultSubclass);
            SendMessageW(dialog, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(dialog, FaultDialog, kFaultSubclass);
    return DefSubclassProc(dialog, message, wp, lp);
}
LRESULT CALLBACK DialogHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* created = reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if (!(created->lpcs->style & WS_CHILD)) {
            created->lpcs->x = -32000; created->lpcs->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) {
        HWND dialog = reinterpret_cast<HWND>(wp);
        HWND list = GetDlgItem(dialog, 628);
        Check(list && GetDlgItem(dialog, 630) && GetDlgItem(dialog, 631) && GetDlgItem(dialog, 632),
              "model-order command opens its actual expected dialog");
        ++dialogs;
        Check(SendMessageW(list, LB_GETCOUNT, 0, 0) == 3, "model-order dialog contains all three models");
        for (int i = 0; i != 3; ++i)
            initialLabels[i] = Text(list, LB_GETTEXTLEN, LB_GETTEXT, i);
        const auto* scratch=static_cast<const std::int32_t*>(g_Block->AccessoryOrderArray());
        std::array<bool,kModelSlotCount> seen{};
        for(int row=1;row<=3;++row) {
            Check(scratch[row]>0&&scratch[row]<=kModelSlotCount,"actual dialog rows have bounded slot identities");
            const int slot=scratch[row]-1;
            Check(g_Block->ModelSlot(slot)&&!seen[slot],"actual dialog rows cover each live model exactly once");seen[slot]=true;
            Check(initialLabels[row-1]==text_encoding::ModelLabel(*mdl::Mdl(g_Block->ModelSlot(slot)),UiUsesEnglishNames(g_Block->EnglishUI())),
                "actual dialog caption belongs to its repaired scratch identity");
        }
        if (listFaultMode) {
            listFaultEnabled = true; listInsertions = 0; listFaultStage = 0;
            Check(SetWindowSubclass(list, RejectListWrite, kFaultSubclass, 0) != FALSE,
                  "install native list insertion/deletion failure observer");
        }
        if (moveRow >= 0) {
            Check(SendMessageW(list, LB_SETCURSEL, moveRow, 0) == moveRow, "choose actual reorder row");
            SendMessageW(dialog, WM_COMMAND, moveCommand, 0);
        }
        if (invalidScratchRound) {
            static_cast<std::int32_t*>(g_Block->AccessoryOrderArray())[1]=invalidScratchValue;
            PostMessageW(dialog,WM_COMMAND,632,0);PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
        } else if (listFaultMode == 2) {
            Check(SetWindowSubclass(dialog, ListFaultDialog, kFaultSubclass, 0) != FALSE,
                  "observe list repair before a successful modal exit");
            PostMessageW(dialog, kFaultContinue, 1, 0);
        } else if (faultRound) {
            rejectInserts = true; faultStage = 0;
            Check(SetWindowSubclass(GetDlgItem(g_Block->MainWindow(), 436), RejectInsert, kFaultSubclass, 0) != FALSE &&
                  SetWindowSubclass(dialog, FaultDialog, kFaultSubclass, 0) != FALSE,
                  "install bounded native combo failure and modal continuation observers");
            PostMessageW(dialog, kFaultContinue, 1, 0);
        } else PostMessageW(dialog, WM_COMMAND, cancel ? IDCANCEL : 632, 0);
        return 1; // The off-screen fixture must not activate over the user's app.
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
struct Fixture {
    MMDApp* app;
    std::array<mdl::ModelRecord*, 3> models{};
    std::array<wchar_t*, 3> originalNames{};
    std::array<std::wstring, 3> labels{L"一号_模型_髪", L"二号_モデル_腕", L"三号_😀_脚"};
    mdl::AccessoryRecord accessory{};
    const unsigned char originalLanguage;
    const unsigned char originalNamePreference;
    explicit Fixture(MMDApp* value, const wchar_t* modelPath)
        : app(value), originalLanguage(value->EnglishUI()), originalNamePreference(gModelInfoLanguagePreference) {
        app->EnglishUI() = kUiChinese;
        SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
        for (int i = 0; i != 3; ++i) {
            LoadModelFile(app, modelPath, false);
            Check(app->ModelSlot(i) != nullptr && app->SelectedModelSlot() == i,
                  "model-order fixture loads three actual native models");
            models[i] = mdl::Mdl(app->ModelSlot(i));
            originalNames[i] = models[i]->pmxTextBuffers[0];
            models[i]->pmxTextBuffers[0] = labels[i].data();
        }
        for (int i = 0; i != 3; ++i) app->ModelSlot(i) = nullptr;
        for (int i = 0; i != 3; ++i)
            app->ModelSlot(kSlots[i]) = reinterpret_cast<unsigned char*>(models[i]);
        accessory.order = 0; accessory.parentModel = kSlots[0]; accessory.parentBone = 0;
        strcpy_s(accessory.name, "order fixture");
        app->AccessorySlot(17) = &accessory;
        app->SelectedAccessorySlot() = 17;
        Reset();
    }
    ~Fixture() {
        for (int i = 0; i != 3; ++i) models[i]->pmxTextBuffers[0] = originalNames[i];
        app->AccessorySlot(17) = nullptr;
        app->SelectedAccessorySlot() = 0;
        app->EnglishUI() = originalLanguage;
        SetModelInfoLanguagePreference(originalNamePreference);
    }
    void Reset() {
        for (int i = 0; i != 3; ++i) models[i]->comboSelIndex = models[i]->comboSelIndex2 = static_cast<unsigned char>(i + 1);
        for (int i = 0; i != 3; ++i) {
            Check(models[i]->boneCount > 0 && models[i]->boneTable, "native reorder fixture has real editable bone poses");
            models[i]->boneTable[0].trans[0] = 13.5f + static_cast<float>(i);
        }
        app->SetSelectedModelSlot(kSlots[1]); app->CameraMode() = 0;
        app->EditMode() = ViewportEditMode::Scale;
        app->CameraParentModel() = kSlots[2]; app->CameraParentBone() = 0;
        accessory.parentModel = kSlots[0]; accessory.parentBone = 0;
        app->SelectedAccessorySlot() = 17;
        Check(scene_history_ui::RefreshObjectLists(*app, scene_history_ui::CaptureSelection(*app), false, false) ==
              scene_history_ui::Result::Ok, "initialize real model selectors from object identities");
        EnableWindow(GetDlgItem(app->MainWindow(), panel::kUndoButton), TRUE);
        EnableWindow(GetDlgItem(app->MainWindow(), panel::kRedoButton), TRUE);
    }
    void Open(int command, int row, int movement, bool cancelled) {
        std::array<std::array<float, 7>, 3> poses{};
        for (int i = 0; i != 3; ++i) {
            std::copy_n(models[i]->boneTable[0].trans, 3, poses[i].begin());
            std::copy_n(models[i]->boneTable[0].rotQuat, 4, poses[i].begin() + 3);
        }
        const auto frame = app->CurrentFrame();
        moveRow = row; moveCommand = movement; cancel = cancelled;
        const auto count = dialogs;
        HHOOK hook = SetWindowsHookExW(WH_CBT, DialogHook, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install off-screen real model-order dialog driver");
        CmdViewMenu(app, app->MainWindow(), static_cast<std::uint16_t>(command), 0);
        UnhookWindowsHookEx(hook);
        Check(dialogs == count + 1 && app->AccessoryOrderArray() == nullptr,
              "actual model-order dialog completes and frees only its scratch order");
        for (int i = 0; i != 3; ++i)
            Expect(std::equal(poses[i].begin(), poses[i].begin() + 3, models[i]->boneTable[0].trans) &&
                   std::equal(poses[i].begin() + 3, poses[i].end(), models[i]->boneTable[0].rotQuat),
                   "reorder, cancel and retry preserve every model's live unregistered bone pose");
        Expect(app->CurrentFrame() == frame, "model order never seeks or moves the editing frame");
    }
    void Verify(const char* phase, std::array<int, 3> draw, std::array<int, 3> calculate) {
        int captionMismatches = 0;
        for (int row = 1; row <= 3; ++row) {
            const auto index = draw[row - 1];
            Expect(models[index]->comboSelIndex == row, "draw order is the requested complete permutation");
            Expect(models[calculate[row - 1]]->comboSelIndex2 == row,
                   "calculation order is its independently requested complete permutation");
            for (int id : kCombos) {
                HWND combo = GetDlgItem(app->MainWindow(), id);
                Expect(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 4, "reordering keeps all three selector rows");
                const auto caption = Text(combo, CB_GETLBTEXTLEN, CB_GETLBTEXT, row);
                Expect(caption == labels[index], "all selector captions follow draw order, including Unicode and duplicate names");
                Expect(SendMessageW(combo, CB_GETITEMDATA, row, 0) == kSlots[index],
                       "every selector row retains its logical model slot, including duplicate captions");
                for (int logical = 0; logical != 3; ++logical)
                    if (models[logical]->comboSelIndex == row && caption != labels[logical]) ++captionMismatches;
            }
        }
        const auto selectedRow = models[1]->comboSelIndex;
        const auto mainRow = SendDlgItemMessageW(app->MainWindow(), 436, CB_GETCURSEL, 0, 0);
        std::printf("MODEL-ORDER %s: caption/object mismatches=%d mainRow=%lld expected=%u selectedSlot=%u cameraMode=%u parentRows=%lld/%lld\n",
            phase, captionMismatches, static_cast<long long>(mainRow), static_cast<unsigned>(selectedRow),
            static_cast<unsigned>(app->SelectedModelSlot()), static_cast<unsigned>(app->CameraMode()),
            static_cast<long long>(SendDlgItemMessageW(app->MainWindow(),449,CB_GETCURSEL,0,0)),
            static_cast<long long>(SendDlgItemMessageW(app->MainWindow(),474,CB_GETCURSEL,0,0)));
        Expect(captionMismatches == 0, "each displayed model name matches the object resolved by that actual row");
        Expect(mainRow == selectedRow && app->MainModelComboSelection() == selectedRow &&
               app->SelectedModelSlot() == kSlots[1] && app->CameraMode() == 0 &&
               app->EditMode() == ViewportEditMode::Scale,
               "reordering preserves the selected model identity, edit mode and collapsed caption");
        Expect(SendDlgItemMessageW(app->MainWindow(),449,CB_GETCURSEL,0,0) == models[2]->comboSelIndex &&
               SendDlgItemMessageW(app->MainWindow(),474,CB_GETCURSEL,0,0) == models[0]->comboSelIndex &&
               app->CameraParentModel() == kSlots[2] && accessory.parentModel == kSlots[0],
               "parent selectors retain their actual bound model identities after row movement");
        bool canUndo=true,canRedo=true;
#if !MMD_ORIGINAL_PROFILE
        if(auto* journal=plugins::BoundNativePoseHistory(app)) {canUndo=journal->UndoCount()!=0;canRedo=journal->RedoCount()!=0;}
#endif
        Expect((IsWindowEnabled(GetDlgItem(app->MainWindow(), panel::kUndoButton))!=FALSE)==canUndo &&
               (IsWindowEnabled(GetDlgItem(app->MainWindow(), panel::kRedoButton))!=FALSE)==canRedo,
               "order-only UI refresh preserves existing undo and redo availability");
    }
};
struct DamagedCase {
    std::array<unsigned char,3> ranks;
    std::array<int,3> rows;
};
constexpr std::array<DamagedCase,5> kDamaged{{
    {{{2,2,255}},{{1,0,2}}}, {{{0,3,0}},{{0,2,1}}}, {{{1,1,1}},{{0,1,2}}},
    {{{0,0,0}},{{0,1,2}}}, {{{255,0,4}},{{0,1,2}}}
}};
std::array<unsigned char,6> Ranks(const Fixture& f) {
    std::array<unsigned char,6> result{};
    for(int i=0;i<3;++i){result[i]=f.models[i]->comboSelIndex;result[i+3]=f.models[i]->comboSelIndex2;}
    return result;
}
void SetDamaged(Fixture& f,bool calculate,const DamagedCase& sample) {
    for(int i=0;i<3;++i)(calculate?f.models[i]->comboSelIndex2:f.models[i]->comboSelIndex)=sample.ranks[i];
}
void ExpectProjectedUi(const Fixture& f,const std::array<int,3>& rows) {
    for(int row=1;row<=3;++row)for(int id:kCombos) {
        const HWND combo=GetDlgItem(f.app->MainWindow(),id);
        Expect(SendMessageW(combo,CB_GETCOUNT,0,0)==4&&SendMessageW(combo,CB_GETITEMDATA,0,0)==-1,
            "damaged rank UI has a complete explicitly mapped model selector");
        Expect(SendMessageW(combo,CB_GETITEMDATA,row,0)==kSlots[rows[row-1]]&&
            Text(combo,CB_GETLBTEXTLEN,CB_GETLBTEXT,row)==f.labels[rows[row-1]],
            "damaged ranks project to matching object labels and slots without renumbering");
    }
}
void ProjectionRules() {
    std::array<int,255> ranks;ranks.fill(-1);ranks[0]=2;ranks[7]=2;ranks[254]=255;
    const auto damaged=model_order::Project<255>([&](std::size_t slot)noexcept{return ranks[slot];});
    Check(!damaged.originalValid&&damaged.count==3&&damaged.rows[1]==8&&damaged.rows[2]==1&&damaged.rows[3]==255,
        "projection preserves the first valid rank then fills duplicate and excessive gaps in slot order");
    for(int slot=0;slot<255;++slot)ranks[slot]=255-slot;
    const auto full=model_order::Project<255>([&](std::size_t slot)noexcept{return ranks[slot];});
    Check(full.originalValid&&full.count==255&&full.rows[1]==255&&full.rows[255]==1,
        "healthy full 255-row permutation retains both boundary slots and ranks");
    std::array<std::int64_t,256> items{};items[0]=-1;
    for(std::size_t row=1;row<=full.count;++row)items[row]=full.rows[row]-1;
    const auto item=[&](std::size_t row)noexcept{return items[row];};
    Check(model_order::Resolve(full,1,256,item)==254&&model_order::Resolve(full,255,256,item)==0,
        "complete managed mapping distinguishes real slot zero from unregistered default-zero data");
    items[1]=0;Check(model_order::Resolve(full,1,256,item)==-1,"duplicate managed item identities are refused");
    items[1]=-1;Check(model_order::Resolve(full,1,256,item)==-1,"CB_ERR is never read as a model identity");
    items.fill(0);Check(model_order::Resolve(full,1,256,item)==254,
        "healthy older combos without registered identity metadata retain native-rank fallback");
    Check(model_order::Resolve(damaged,1,4,item)==-1&&model_order::Resolve(full,1,255,item)==-1,
        "unmapped damaged order and incomplete combo cannot select an arbitrary model");
}
void DamagedUnboundChecks(Fixture& f) {
    for(bool calculate:{false,true})for(const auto& sample:kDamaged) {
        f.Reset();SetDamaged(f,calculate,sample);const auto before=Ranks(f);f.app->SceneModified()=0;
        f.Open(calculate?289:288,0,631,true);
        Expect(initialLabels==std::array<std::wstring,3>{f.labels[sample.rows[0]],f.labels[sample.rows[1]],f.labels[sample.rows[2]]},
            "damaged rank dialog initializes every row from repaired identities rather than stale combo captions");
        Expect(Ranks(f)==before&&!f.app->SceneModified(),"unbound damaged order and moved scratch cancel without any live renumbering");
        f.Open(calculate?289:288,-1,0,false);
        auto expected=before;for(int row=1;row<=3;++row)expected[(calculate?3:0)+sample.rows[row-1]]=static_cast<unsigned char>(row);
        Expect(Ranks(f)==expected&&f.app->SceneModified(),"unbound OK publishes only its chosen complete repaired family");
        ExpectProjectedUi(f,calculate?std::array<int,3>{0,1,2}:sample.rows);
    }
    f.Reset();
}
#if !MMD_ORIGINAL_PROFILE
void ExerciseMappedSelectors(Fixture& f,const std::array<int,3>& rows,plugins::SharedHistory& history) {
    const auto ranks=Ranks(f);const auto undo=history.UndoCount(),redo=history.RedoCount();
    for(int row=1;row<=3;++row) {
        for(int id:kCombos) {
            HWND combo=GetDlgItem(f.app->MainWindow(),id);
            Check(SendMessageW(combo,CB_SETCURSEL,row,0)==row,"select an actual projected model row");
            CommandDispatch(combo,MAKEWPARAM(id,CBN_SELCHANGE));
            const auto expected=kSlots[rows[row-1]];
            Expect(id==436?f.app->SelectedModelSlot()==expected:id==449?f.app->CameraParentModel()==expected:f.accessory.parentModel==expected,
                "actual model and both parent selection handlers resolve item identity after damaged-rank undo");
        }
        Expect(Ranks(f)==ranks&&history.UndoCount()==undo&&history.RedoCount()==redo,
            "selecting projected identities does not implicitly repair ranks or append a history record");
        Expect((IsWindowEnabled(GetDlgItem(f.app->MainWindow(),panel::kUndoButton))!=FALSE)==(undo!=0)&&
            (IsWindowEnabled(GetDlgItem(f.app->MainWindow(),panel::kRedoButton))!=FALSE)==(redo!=0),
            "actual model selection keeps shared undo and redo availability");
    }
    // Reject a corrupted managed mapping before clearing a parent bone list or
    // selecting default slot zero. Restore it before the next refresh.
    for(int id:kCombos) {
        HWND combo=GetDlgItem(f.app->MainWindow(),id);const auto saved=SendMessageW(combo,CB_GETITEMDATA,1,0);
        const auto selected=f.app->SelectedModelSlot();const auto camera=f.app->CameraParentModel();const auto parent=f.accessory.parentModel;
        SendMessageW(combo,CB_SETITEMDATA,1,-1);SendMessageW(combo,CB_SETCURSEL,1,0);
        CommandDispatch(combo,MAKEWPARAM(id,CBN_SELCHANGE));
        Expect(f.app->SelectedModelSlot()==selected&&f.app->CameraParentModel()==camera&&f.accessory.parentModel==parent,
            "CB_ERR item data in a managed selector cannot select or bind the wrong object");
        SendMessageW(combo,CB_SETITEMDATA,1,saved);
    }
}
void DamagedBoundChecks(Fixture& f) {
    using namespace plugins;
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})
    for(bool calculate:{false,true})for(const auto& sample:kDamaged) {
        f.Reset();StorageStore storage;SharedHistory history(storage);NativePoseTestBinding binding(*f.app,history);
        Check(history.SetMode(mode)==MMS_OK,"damaged actual modal supports either recording mode");
        f.Open(288,2,630,false);Check(RunBoundObjectHistory(*f.app,history,false)==MMS_OK&&history.RedoCount()==1,
            "create a genuine redo branch before damaged modal cancellation");
        SetDamaged(f,calculate,sample);const auto before=Ranks(f);f.app->SceneModified()=0;
        f.Open(calculate?289:288,0,631,true);
        Expect(Ranks(f)==before&&!f.app->SceneModified()&&!history.UndoCount()&&history.RedoCount()==1,
            "bound damaged-order cancellation retains exact native bytes dirty state and redo");
        f.Open(calculate?289:288,-1,0,false);
        auto expected=before;for(int row=1;row<=3;++row)expected[(calculate?3:0)+sample.rows[row-1]]=static_cast<unsigned char>(row);
        Expect(Ranks(f)==expected&&history.UndoCount()==1&&!history.RedoCount()&&!NativeObjectUiPending(f.app),
            "actual damaged modal OK creates one shared node and publishes a complete selector mapping");
        Check(RunBoundObjectHistory(*f.app,history,false)==MMS_OK,"actual damaged modal undo succeeds");
        Expect(Ranks(f)==before&&!NativeObjectUiPending(f.app)&&!history.UndoCount()&&history.RedoCount()==1,
            "actual undo restores abnormal rank bytes without creating a permanent UI repair gate");
        const auto drawRows=calculate?std::array<int,3>{0,1,2}:sample.rows;
        ExpectProjectedUi(f,drawRows);RefreshModelSelectorNames(f.app);ExpectProjectedUi(f,drawRows);
        Expect(Ranks(f)==before,"language/name refresh retains every damaged rank while rebuilding all identities");
        ExerciseMappedSelectors(f,drawRows,history);
        Check(RunBoundObjectHistory(*f.app,history,true)==MMS_OK,"actual damaged modal redo succeeds after selector interaction");
        Expect(Ranks(f)==expected&&!NativeObjectUiPending(f.app)&&history.UndoCount()==1,
            "actual redo repairs the same family after user selection and parent changes");
        Check(history.Reset()==MMS_OK,"release damaged actual modal records before the next fixture");history.Collect();
    }
    f.Reset();
}
LRESULT CALLBACK ConfirmRetainedDeletion(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND) {
        auto* created=reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if(!(created->lpcs->style&WS_CHILD)){created->lpcs->x=-32000;created->lpcs->y=-32000;}
    }
    if(code==HCBT_ACTIVATE) {
        HWND dialog=reinterpret_cast<HWND>(wp);
        Check(GetDlgItem(dialog,IDOK)&&GetDlgItem(dialog,IDCANCEL),"actual model deletion opens its confirmation dialog");
        PostMessageW(dialog,WM_COMMAND,IDOK,0);return 1;
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
void BoundChecks(Fixture& f,const wchar_t* source) {
    using namespace plugins;
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        f.Reset();StorageStore storage;SharedHistory history(storage);NativePoseTestBinding binding(*f.app,history);
        Check(history.SetMode(mode)==MMS_OK,"actual modal chooses its common history recording mode");
        f.Open(288,2,630,false);f.Verify("bound-display",{0,2,1},{0,1,2});
        Expect(history.UndoCount()==1,"actual display-order OK creates exactly one explicit common node in either mode");
        Check(RunBoundObjectHistory(*f.app,history,false)==MMS_OK,"actual modal draw undo");
        f.Verify("bound-display-undo",{0,1,2},{0,1,2});
        for(auto invalid:{std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),0,256}) {
            invalidScratchRound=true;invalidScratchValue=invalid;f.Open(288,-1,0,false);invalidScratchRound=false;
            Expect(!history.UndoCount()&&history.RedoCount()==1,"invalid signed-boundary modal scratch cannot publish or trim redo");
            f.Verify("bound-invalid-scratch",{0,1,2},{0,1,2});
        }
        f.Open(288,0,630,false);f.Open(288,0,631,true);
        Expect(!history.UndoCount()&&history.RedoCount()==1,"actual no-op and cancelled dialogs retain existing redo");
        Check(RunBoundObjectHistory(*f.app,history,true)==MMS_OK,"actual modal draw redo");
        f.Open(289,2,630,false);f.Verify("bound-calculation",{0,2,1},{0,2,1});
        Expect(history.UndoCount()==2,"actual calculation-order OK creates its own explicit node");
        f.app->PhysicsResetPending()=0;
        Check(RunBoundObjectHistory(*f.app,history,false)==MMS_OK&&RunBoundObjectHistory(*f.app,history,false)==MMS_OK,
              "undo both actual modal order directions before releasing their owners");
        Expect(!f.app->PhysicsResetPending(),"order-only history does not request a complete physics reset");
        f.Verify("bound-both-restored",{0,1,2},{0,1,2});
        Check(history.Reset()==MMS_OK,"release actual modal order journal before another binding");history.Collect();
    }
    f.Reset();StorageStore storage;SharedHistory history(storage);NativePoseTestBinding binding(*f.app,history);
    const auto copy=std::filesystem::path(source).parent_path()/
        (L"model-order-owned-import-"+std::to_wstring(GetCurrentProcessId())+L".pmx");
    Check(CopyFileW(source,copy.c_str(),TRUE)!=FALSE,"create an owned copy for the real model import/readback regression");
    LoadModelFile(f.app,copy.c_str(),false);
    auto* imported=f.app->ModelSlot(0);
    Check(imported&&history.UndoCount()==1,"real PMX private loader publishes an import into the same common journal");
    const auto id=SceneObjectIdentity(imported).id;
    Check(DeleteFileW(copy.c_str())!=FALSE,"remove only the owned source copy before any undo or redo");
    std::array<std::int32_t,5> reverse{0,1,255,10,3};
    NativeModelOrderPlan draw;std::shared_ptr<HistoryChange> change;
    Check(draw.Begin(*f.app,ModelOrderKind::Draw)==MMS_OK&&draw.Read(reverse.data(),4)==MMS_OK&&draw.Take(change)==MMS_OK&&
        CommitBoundModelOrder(*f.app,history,std::move(change),false)==MMS_OK&&history.UndoCount()==2,
        "reorder the real imported model together with the three preexisting sparse-slot objects");
    NativeModelOrderPlan calculate;
    Check(calculate.Begin(*f.app,ModelOrderKind::Calculate)==MMS_OK&&calculate.Read(reverse.data(),4)==MMS_OK&&
        calculate.Take(change)==MMS_OK,"capture model calculation order for the real plugin transaction path");
    EditStore edits(history,storage);EditClient client;client.id="test.model-order-host";auto api=edits.Api(client);
    constexpr char label[]="plugin calculation order";std::uint64_t revision=0;MmsHandle transaction=0;
    Check(history.Revision(revision)==MMS_OK&&api.begin(api.context,{label,sizeof(label)-1},revision,&transaction)==MMS_OK&&
        edits.JoinNative(client,transaction,std::move(change))==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK&&
        api.commit(api.context,transaction)==MMS_OK&&history.UndoCount()==3,
        "an actual EditStore transaction follows import and native draw order in the same journal");
    f.app->SetSelectedModelSlot(0);f.app->CameraMode()=0;
    Check(scene_history_ui::RefreshObjectLists(*f.app,scene_history_ui::CaptureSelection(*f.app),true,false)==
        scene_history_ui::Result::Ok,"select the imported identity without reevaluating other model poses");
    HHOOK hook=SetWindowsHookExW(WH_CBT,ConfirmRetainedDeletion,nullptr,GetCurrentThreadId());
    Check(hook!=nullptr,"install actual retained-model deletion confirmation driver");
    TryDeleteBoundModel(f.app,f.app->MainWindow());UnhookWindowsHookEx(hook);
    Check(!f.app->ModelSlot(0)&&history.UndoCount()==4,"actual native deletion follows the plugin and both order records");
    for(int i=0;i<4;++i)Check(RunBoundObjectHistory(*f.app,history,false)==MMS_OK,"undo deletion, plugin order, native order and actual import in reverse sequence");
    Expect(!f.app->ModelSlot(0)&&f.models[0]->comboSelIndex==1&&f.models[1]->comboSelIndex==2&&f.models[2]->comboSelIndex==3,
        "undo interleaving restores the original three-model layout by stable identity");
    for(int i=0;i<4;++i)Check(RunBoundObjectHistory(*f.app,history,true)==MMS_OK,"redo real import, orders and deletion without the source file");
    Expect(!f.app->ModelSlot(0)&&!SceneObjectPresent(imported,id),"redo reaches the same deleted identity without rereading the removed PMX");
    for(int i=0;i<4;++i)Check(RunBoundObjectHistory(*f.app,history,false)==MMS_OK,"restore fixture and park only the retained imported model before cleanup");
    Check(history.Reset()==MMS_OK,"retire interleaved imported-model resources after all original objects are restored");history.Collect();
    f.Reset();
}
#endif
void Run(MMDApp* app, const wchar_t* modelPath) {
    Fixture f(app, modelPath);
    failures = 0;
    ProjectionRules();DamagedUnboundChecks(f);
    f.Open(288, 2, 630, false); f.Verify("display-up", {0, 2, 1}, {0, 1, 2});
    f.Open(288, 1, 631, false); f.Verify("display-down", {0, 1, 2}, {0, 1, 2});
    f.Open(288, 0, 630, false); f.Verify("display-boundary", {0, 1, 2}, {0, 1, 2});
    f.Open(288, 0, 631, true); f.Verify("display-cancel", {0, 1, 2}, {0, 1, 2});
    f.Reset();
    // Deliberately independent calculation order. Its dialog must initialize
    // from this order rather than the model-manipulation selector captions.
    f.models[2]->comboSelIndex2 = 1; f.models[0]->comboSelIndex2 = 2; f.models[1]->comboSelIndex2 = 3;
    f.Open(289, 2, 630, false);
    Expect(initialLabels == std::array<std::wstring,3>{f.labels[2], f.labels[0], f.labels[1]},
           "calculation dialog initially lists independent calculation order");
    f.Verify("calculate-up", {0, 1, 2}, {2, 1, 0});
    f.Open(289, 1, 631, false); f.Verify("calculate-down", {0, 1, 2}, {2, 0, 1});
    f.Open(289, 0, 630, false); f.Verify("calculate-boundary", {0, 1, 2}, {2, 0, 1});
    f.Open(289, 0, 631, true); f.Verify("calculate-cancel", {0, 1, 2}, {2, 0, 1});
    // Repeat with two truly equal captions. Slot identity, rather than a name
    // lookup, must keep the active object and its parent bindings attached.
    f.labels[1] = f.labels[0]; f.models[1]->pmxTextBuffers[0] = f.labels[1].data();
    f.Reset();
    f.Open(288, 2, 630, false); f.Verify("duplicate-display", {0, 2, 1}, {0, 1, 2});
    f.Reset();
    f.Open(289, 2, 630, false); f.Verify("duplicate-calculate", {0, 1, 2}, {0, 2, 1});
    f.Reset(); app->SceneModified() = 0;
    faultRound = true; f.Open(288, 2, 630, false); faultRound = false;
    Expect(faultStage == 3, "persistent insertion failure cannot release the modal before successful cancel repair");
    f.Verify("control-failure-recovery", {0, 1, 2}, {0, 1, 2});
    Expect(app->SceneModified() == 0, "control failure and cancellation retain the original project dirty state");
    f.Reset(); app->SceneModified() = 0;
    listFaultMode = 1; f.Open(288, 2, 630, false); listFaultMode = 0;
    f.Verify("list-insert-failure", {0, 1, 2}, {0, 1, 2});
    Expect(app->SceneModified() == 0, "a rejected list move leaves the original order unmodified");
    f.Reset(); app->SceneModified() = 0;
    listFaultMode = 2; f.Open(288, 2, 630, false); listFaultMode = 0;
    Expect(listFaultStage == 2, "incomplete list recovery prevents OK until its full scratch permutation is visible");
    f.Verify("list-delete-recovery", {0, 1, 2}, {0, 1, 2});
    Expect(app->SceneModified() == 0, "list deletion failure and repair never publish a partial order");
    for(auto invalid:{std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),0,256}) {
        f.Reset();app->SceneModified()=0;invalidScratchRound=true;invalidScratchValue=invalid;
        f.Open(288,-1,0,false);invalidScratchRound=false;
        f.Verify("unbound-invalid-scratch",{0,1,2},{0,1,2});
        Expect(!app->SceneModified(),"unbound signed-boundary rejection leaves source and dirty state intact");
    }
#if !MMD_ORIGINAL_PROFILE
    BoundChecks(f,modelPath);
    DamagedBoundChecks(f);
#endif
    std::printf("MODEL-ORDER actual modal checks: %d failure(s)\n", failures);
    Check(failures == 0, "real 288/289 names, object identities, independent orders and cancellation");
    std::puts("PASS real model-order dialogs: names match resolved objects, independent calculation order, selection and parents preserved");
}
} // namespace model_order_dialog_checks
