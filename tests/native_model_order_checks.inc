#include "../src/plugins/native_model_order.hpp"
#include "../src/plugins/edit_store.hpp"
#include <limits>
namespace {
namespace native_model_order_checks {
using namespace plugins;
struct PluginState {
    int value=0, pending=0, before=0, after=42;
    MmsSerializerV1 Serializer() {
        MmsSerializerV1 table{Header<MmsSerializerV1>(),this};
        table.prepareLoad=[](void* p,MmsBytes bytes,std::uint32_t schema,MmsHandle* out)->MmsResult {
            auto& self=*static_cast<PluginState*>(p);
            if(schema&&(schema!=1||bytes.bytes!=sizeof(int)||!bytes.data))return MMS_E_ARGUMENT;
            self.pending=0;if(schema)std::memcpy(&self.pending,bytes.data,sizeof(int));*out=1;return MMS_OK;
        };
        table.commitLoad=[](void* p,MmsHandle){auto& self=*static_cast<PluginState*>(p);self.value=self.pending;};
        table.cancelLoad=[](void*,MmsHandle){};
        table.save=[](void* p,MmsBytes* bytes,std::uint32_t* schema,MmsHandle* lease)->MmsResult {
            *bytes={&static_cast<PluginState*>(p)->value,sizeof(int)};*schema=1;*lease=1;return MMS_OK;
        };
        table.releaseBytes=[](void*,MmsHandle){};return table;
    }
    MmsTransactionParticipantV1 Participant(MmsString id) {
        MmsTransactionParticipantV1 table{Header<MmsTransactionParticipantV1>(),this,id};
        table.prepare=[](void* p)->MmsResult{auto& self=*static_cast<PluginState*>(p);return self.value==self.before?MMS_OK:MMS_E_STALE;};
        table.commit=[](void* p){auto& self=*static_cast<PluginState*>(p);self.value=self.after;};
        table.cancel=[](void*){};
        table.getUndoState=[](void* p,MmsBytes* before,MmsBytes* after,std::uint32_t* schema,MmsHandle* lease)->MmsResult {
            auto& self=*static_cast<PluginState*>(p);*before={&self.before,sizeof(int)};*after={&self.after,sizeof(int)};
            *schema=1;*lease=1;return MMS_OK;
        };
        table.releaseUndoState=[](void*,MmsHandle){};return table;
    }
};
struct Scene {
    ObjectCommandFixture f;
    int expectedFrame=137;
    Scene() {
        f.app->ModelSlot(254) = f.app->ModelSlot(1); f.app->ModelSlot(1) = nullptr;
        f.app->CameraParentModel() = 254; f.app->CameraParentBone() = 0;
        f.app->AccessorySlot(0)->parentModel = 254; f.app->AccessorySlot(0)->parentBone = 0;
        Check(scene_history_ui::RefreshObjectLists(*f.app, scene_history_ui::CaptureSelection(*f.app), false, false) ==
            scene_history_ui::Result::Ok, "model order fixture uses actual sparse slot 254 and bound parent selectors");
    }
    ~Scene() {
        Check(f.history.Reset() == MMS_OK, "model order clears retained nodes before restoring fixture slot"); f.history.Collect();
        f.app->ModelSlot(1) = f.app->ModelSlot(254); f.app->ModelSlot(254) = nullptr;
    }
    std::array<std::uint8_t,4> Orders() const {
        const auto& a = *mdl::Mdl(f.app->ModelSlot(0)); const auto& b = *mdl::Mdl(f.app->ModelSlot(254));
        return {a.comboSelIndex,b.comboSelIndex,a.comboSelIndex2,b.comboSelIndex2};
    }
    void Untouched() const {
        Check(f.app->CurrentFrame() == expectedFrame && f.app->CameraPositionX() == 27.f && f.app->CameraDistance() == -81.f &&
            mdl::Mdl(f.app->ModelSlot(0))->lightDir[0] == 11.f && mdl::Mdl(f.app->ModelSlot(254))->lightDir[0] == 12.f &&
            mdl::Mdl(f.app->ModelSlot(0))->morphKeys[0].value == .25f &&
            mdl::Mdl(f.app->ModelSlot(254))->morphKeys[0].value == 1.25f &&
            f.app->CameraParentModel() == 254 && f.app->AccessorySlot(0)->parentModel == 254,
            "order transactions preserve current frame, live pose payloads and incoming parent bindings");
    }
    std::shared_ptr<HistoryChange> Change(ModelOrderKind kind, bool swap = true) {
        NativeModelOrderPlan plan; std::shared_ptr<HistoryChange> result;
        Check(plan.Begin(*f.app,kind) == MMS_OK, "capture complete dual model order by retained identities");
        const auto& a = *mdl::Mdl(f.app->ModelSlot(0));
        const bool aFirst = (kind == ModelOrderKind::Draw ? a.comboSelIndex : a.comboSelIndex2) == 1;
        std::array<std::int32_t,3> slots{0, aFirst ? 1 : 255, aFirst ? 255 : 1};
        if (swap) std::swap(slots[1],slots[2]);
        Check(plan.Read(slots.data(),2) == MMS_OK && plan.Take(result) == MMS_OK,
              "read checked one-based native slot permutation privately");
        Check(bool(result) == swap, "unchanged order produces no history participant"); return result;
    }
};
void ModesAndUi() {
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly}) {
        Scene x; auto& f=x.f; NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"choose model-order recording mode");
        f.app->SceneModified()=0;f.app->PhysicsResetPending()=0;
        Check(CommitBoundModelOrder(*f.app,f.history,x.Change(ModelOrderKind::Draw),false)==MMS_OK &&
            f.history.UndoCount()==1 && x.Orders()==std::array<std::uint8_t,4>{2,1,1,2} && f.app->SceneModified(),
            "one explicit draw-order OK records one node in both modes and marks dirty");
        x.Untouched();
        Check(SendDlgItemMessageW(f.app->MainWindow(),436,CB_GETITEMDATA,1,0)==254 &&
            SendDlgItemMessageW(f.app->MainWindow(),436,CB_GETCURSEL,0,0)==2 &&
            SendDlgItemMessageW(f.app->MainWindow(),449,CB_GETCURSEL,0,0)==1,
            "published same-count draw permutation updates row identity and preserves selected/parent objects");
        f.app->SetSelectedModelSlot(254);
        f.app->CurrentFrame()=164;x.expectedFrame=164;
        mdl::Mdl(f.app->ModelSlot(0))->lightDir[1]=87.f;
        mdl::Mdl(f.app->ModelSlot(254))->morphKeys[1].value=.67f;
        Check(scene_history_ui::RefreshObjectLists(*f.app,scene_history_ui::CaptureSelection(*f.app),true,false)==
            scene_history_ui::Result::Ok,"user later selects the other model before undo");
        Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK && x.Orders()==std::array<std::uint8_t,4>{1,2,1,2} &&
            f.app->SelectedModelSlot()==254 && SendDlgItemMessageW(f.app->MainWindow(),436,CB_GETCURSEL,0,0)==2,
            "draw undo refreshes the same-count lists and follows the current model selection");
        Check(!x.Change(ModelOrderKind::Draw,false) && f.history.RedoCount()==1,"no-op does not trim the redo branch");
        { NativeModelOrderPlan cancelled; Check(cancelled.Begin(*f.app,ModelOrderKind::Draw)==MMS_OK,"begin cancellable order editor"); }
        Check(f.history.RedoCount()==1 && !f.history.UndoCount(),"cancelled order capture retains redo and journal counts");
        Check(RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK,"redo the original draw order");
        Check(CommitBoundModelOrder(*f.app,f.history,x.Change(ModelOrderKind::Calculate),true)==MMS_OK &&
            f.history.UndoCount()==2 && x.Orders()==std::array<std::uint8_t,4>{2,1,2,1},
            "calculation order is a separate explicit node without changing draw order");
        Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK && x.Orders()==std::array<std::uint8_t,4>{2,1,1,2},
            "calculation undo touches only the calculation permutation");
        Check(!f.app->PhysicsResetPending() && !f.history.LastPublicationRequiresPhysicsReset(),
            "both order directions avoid seek and full physics reset");
        Check(mdl::Mdl(f.app->ModelSlot(0))->lightDir[1]==87.f&&mdl::Mdl(f.app->ModelSlot(254))->morphKeys[1].value==.67f,
            "undo and redo preserve unregistered pose changes authored after the order node");x.Untouched();
    }
    Scene x;auto& f=x.f;NativePoseTestBinding binding(*f.app,f.history);
    f.failInsert=436;
    Check(CommitBoundModelOrder(*f.app,f.history,x.Change(ModelOrderKind::Draw),false)==MMS_OK &&
        f.history.UndoCount()==1 && NativeObjectUiPending(f.app.get()) && x.Orders()[0]==2,
        "failed initial UI update keeps the single committed history node and queues retry");
    Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_E_BUSY,"pending object UI gates history until its mapping is repaired");
    Check(RefreshBoundObjectUi(*f.app,f.history)&&!NativeObjectUiPending(f.app.get()),"retry publishes current row identities without repeating the edit");
    f.failInsert=436;
    Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK && f.history.RedoCount()==1 && NativeObjectUiPending(f.app.get()),
        "failed undo UI update never repeats or rolls back the successful journal traversal");
    Check(RefreshBoundObjectUi(*f.app,f.history)&&RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK,
        "redo remains functional after actual control-write failure and retry");x.Untouched();
}
void BudgetAndFailures() {
    Scene x;auto& f=x.f;NativePoseTestBinding binding(*f.app,f.history);
    auto change=x.Change(ModelOrderKind::Draw);std::unique_ptr<PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK,"prepare independent model-order budget candidate");
    std::uint64_t native=0;Check(candidate->TargetRetainedBytes(native)&&native==change->RetainedBytes(),
        "fixed and actual initial charges include the same real retained-owner bookkeeping");
    resource_budget::Collector owners;
    for(unsigned slot:{0u,254u})Check(RetainSceneObject(*f.app,MMS_OBJECT_MODEL,slot)->MeasureBookkeeping(owners)==MMS_OK,
        "independently measure each real owner with one deduplicating ledger");
    Check(owners.Result().Complete()&&owners.Result().totalBytes&&native>owners.Result().totalBytes,
        "model order includes actual owner allocations plus its own record/vector payload");candidate.reset();
    const std::string label="order-budget";const auto bytes=native+sizeof(EditRecord)+label.size();
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,8,bytes-1)==MMS_OK,"set one byte below real complete budget");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative(label,change,token)==MMS_E_MEMORY&&!token&&!f.history.UndoCount()&&x.Orders()[0]==1,
        "budget refusal is atomic even in explicit-only mode");
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,8,bytes)==MMS_OK &&
        f.history.PrepareNative(label,change,token)==MMS_OK,"the exact measured budget admits the order change");
    failAfter=0;const auto committed=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(committed==MMS_OK&&f.history.RetainedBytes()==bytes&&x.Orders()[0]==2,
        "order publication performs no allocations and charges its complete persistent payload");
    failAfter=0;const auto undone=f.history.History(false);failAfter=-1;
    Check(undone==MMS_E_MEMORY&&f.history.UndoCount()==1&&x.Orders()[0]==2,"undo allocation refusal leaves order and journal unchanged");
    Check(f.history.History(false)==MMS_OK,"undo succeeds after memory is available");
    failAfter=0;const auto redone=f.history.History(true);failAfter=-1;
    Check(redone==MMS_E_MEMORY&&f.history.RedoCount()==1&&x.Orders()[0]==1,"redo allocation refusal leaves order and journal unchanged");
    unsigned failures=0;bool success=false;
    for(long limit=0;limit<256;++limit) {
        NativeModelOrderPlan plan;std::shared_ptr<HistoryChange> next;std::array<std::int32_t,3> slots{0,255,1};
        failAfter=limit;
        auto result=plan.Begin(*f.app,ModelOrderKind::Draw);
        if(result==MMS_OK)result=plan.Read(slots.data(),2);
        if(result==MMS_OK)result=plan.Take(next);
        if(result==MMS_OK)result=f.history.PrepareNative(label,std::move(next),token);
        failAfter=-1;
        Check(x.Orders()[0]==1&&f.history.RedoCount()==1&&!f.history.UndoCount(),"every failed/cancelled prepare preserves redo and all model order bytes");
        if(result==MMS_OK){token.reset();success=true;break;}
        Check(result==MMS_E_MEMORY&&!token,"allocation failures are reported without a half publication");++failures;
    }
    Check(success&&failures>8,"allocation sweep reaches all plan, ownership, ledger and journal preparation stages");x.Untouched();
}
void StaleAndPluginTransactions() {
    Scene x;auto& f=x.f;NativePoseTestBinding binding(*f.app,f.history);
    Check(CommitBoundModelOrder(*f.app,f.history,x.Change(ModelOrderKind::Draw),false)==MMS_OK&&
        RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&f.history.RedoCount()==1,"seed redo before testing invalid slot arithmetic");
    const auto retained=f.history.RetainedBytes();
    {
        NativeModelOrderPlan stale;auto journal=std::make_unique<SharedHistory>(f.storage);
        {NativePoseTestBinding temporary(*f.app,*journal);Check(stale.Begin(*f.app,ModelOrderKind::Draw)==MMS_OK,"capture against a temporary runtime journal");}
        journal.reset();std::array<std::int32_t,3> slots{0,255,1};
        Check(stale.Read(slots.data(),2)==MMS_E_STALE&&f.history.RedoCount()==1,
            "a rebound or destroyed journal is rejected before dereferencing the captured history");
    }
    for(unsigned invalidOrder:{0u,3u}) {
        auto change=x.Change(ModelOrderKind::Draw);f.app->AccessorySlot(1)->order=static_cast<std::uint8_t>(invalidOrder);
        f.app->SceneModified()=0;
        Check(CommitBoundModelOrder(*f.app,f.history,std::move(change),false)==MMS_E_STALE&&
            x.Orders()[0]==1&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.RetainedBytes()==retained&&
            !f.app->SceneModified()&&!NativeObjectUiPending(f.app.get()),
            "duplicate or gapped accessory order cannot publish model order or create a permanent UI pending gate");
        f.app->AccessorySlot(1)->order=1;
    }
    for(int invalid=0;invalid<6;++invalid) {
        NativeModelOrderPlan plan;Check(plan.Begin(*f.app,ModelOrderKind::Draw)==MMS_OK,"capture invalid-order source");
        std::array<std::int32_t,3> slots{0,255,1};
        if(invalid==0)slots[2]=255;if(invalid==1)slots[1]=256;
        if(invalid==3)slots[1]=std::numeric_limits<std::int32_t>::min();
        if(invalid==4)slots[1]=std::numeric_limits<std::int32_t>::max();
        if(invalid==5)slots[1]=0;
        std::shared_ptr<HistoryChange> refused;
        Check(plan.Read(slots.data(),invalid==2?1:2)==MMS_E_ARGUMENT&&
            plan.Take(refused)==MMS_E_NOT_READY&&!refused&&x.Orders()==std::array<std::uint8_t,4>{1,2,1,2}&&
            !f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.RetainedBytes()==retained,
            "duplicate, incomplete, zero, 256, INT32_MIN/MAX input preserves models, budget, history and redo");
    }
    auto change=x.Change(ModelOrderKind::Draw);std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("stale",change,token)==MMS_OK,"reserve a real checked order candidate");
    mdl::Mdl(f.app->ModelSlot(0))->comboSelIndex2=2;
    Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&x.Orders()[0]==1&&!f.history.UndoCount(),
        "the last publication check rejects changes to the initially captured second order field");
    mdl::Mdl(f.app->ModelSlot(0))->comboSelIndex2=1;
    PluginState plugin;StorageClient storageClient;storageClient.id="test.model-order";f.storage.Api(storageClient);
    auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storageClient,Text(storageClient.id),&serializer,sizeof(serializer),&provider)==MMS_OK,
        "register an actual plugin serializer for private undo payloads");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storageClient.id;auto api=edits.Api(client);
    std::uint64_t revision=0;MmsHandle transaction=0;
    constexpr char label[]="plugin model order";
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,{label,sizeof(label)-1},revision,&transaction)==MMS_OK,
        "begin a real plugin EditStore transaction on the common journal");
    NativeModelOrderPlan busy;
    Check(busy.Begin(*f.app,ModelOrderKind::Draw)==MMS_E_BUSY,"a plugin draft excludes a second native order editor");
    auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&
        api.prepare(api.context,transaction)==MMS_OK&&api.commit(api.context,transaction)==MMS_OK&&
        plugin.value==42&&x.Orders()[0]==2&&f.history.UndoCount()==1,
        "native order and real plugin-private payload publish together through the plugin transaction ABI");
    Check(CommitBoundModelOrder(*f.app,f.history,x.Change(ModelOrderKind::Calculate),true)==MMS_OK&&f.history.UndoCount()==2,
        "a native modal order follows the plugin transaction in one journal");
    Check(api.undo(api.context)==MMS_OK&&plugin.value==42&&x.Orders()==std::array<std::uint8_t,4>{2,1,1,2}&&
        api.undo(api.context)==MMS_OK&&plugin.value==0&&x.Orders()==std::array<std::uint8_t,4>{1,2,1,2},
        "plugin history undoes the later native calculation and earlier plugin order in sequence");
    Check(api.redo(api.context)==MMS_OK&&api.redo(api.context)==MMS_OK&&plugin.value==42&&x.Orders()==std::array<std::uint8_t,4>{2,1,2,1},
        "plugin history replays both native order directions without rereading a file");x.Untouched();
    Check(f.history.Reset()==MMS_OK,"retire mixed plugin nodes before unregistering their live serializer");f.history.Collect();
    Check(f.storage.Unregister(storageClient,provider)==MMS_OK,"unregister the test-only plugin serializer after history cleanup");
}
}
void NativeModelOrderChecks() {
    native_model_order_checks::ModesAndUi();native_model_order_checks::BudgetAndFailures();
    native_model_order_checks::StaleAndPluginTransactions();
}
}
