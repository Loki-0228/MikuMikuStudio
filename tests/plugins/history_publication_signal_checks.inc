#include "../../src/plugins/shared_history.hpp"
#include <type_traits>

namespace history_publication_signal_checks {
using namespace mikumikustudio::plugins;
constexpr const char* pluginId="example.history-signal";
static_assert(std::is_trivially_copyable_v<HistoryPublicationSignal>);
static_assert(noexcept(std::declval<SharedHistory&>().SetPublicationSignal(HistoryPublicationSignal{})));
static MmsString Literal(const char* value){return {value,std::strlen(value)};}
static uint64_t Revision(SharedHistory& history){
    uint64_t revision=0;Check(history.Revision(revision)==MMS_OK,"signal fixture revision readable");return revision;
}
struct NativeValue final : HistoryChange {
    int& live;int before,after;unsigned publications=0;
    bool stale=false,armNoAllocation=false;
    // Conservative default keeps every earlier test's published mask at All;
    // preparation freezes it and publication/traversal must not query again.
    HistoryEffects effects=HistoryEffects::All;
    mutable unsigned effectsQueries=0;
    MmsResult prepareResult=MMS_OK;
    std::function<void()> preparing,destroyingCandidate;
    NativeValue(int& value,int a,int b):live(value),before(a),after(b){}
    HistoryEffects Effects()const noexcept override{++effectsQueries;return effects;}
    struct Candidate final : PreparedHistoryChange {
        NativeValue& owner;int expected,target;
        Candidate(NativeValue& value,bool forward):owner(value),expected(forward?value.before:value.after),target(forward?value.after:value.before){}
        ~Candidate(){if(owner.destroyingCandidate)owner.destroyingCandidate();}
        bool IsCurrent()const noexcept override{return !owner.stale&&owner.live==expected;}
        void Publish()noexcept override{
            owner.live=target;++owner.publications;
            if(owner.armNoAllocation)failAfter=0;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        if(preparing)preparing();
        if(prepareResult)return prepareResult;
        out=std::make_unique<Candidate>(*this,forward);return MMS_OK;
    }
    uint64_t RetainedBytes()const noexcept override{return sizeof(*this);}
};
struct Sink {
    struct Sample {uint64_t revision=0;int native=0,plugin=0;size_t undo=0,redo=0;HistoryEffects effects=HistoryEffects::All;};
    SharedHistory* history=nullptr;StorageStore* storage=nullptr;int* native=nullptr;int* plugin=nullptr;
    std::array<Sample,32> samples{};unsigned count=0;bool valid=true;
    HistoryPublicationSignal Signal()noexcept{return {this,Published};}
    static void Published(void* context,uint64_t revision)noexcept{
        auto& self=*static_cast<Sink*>(context);uint64_t current=0;
        self.valid=self.valid&&self.history&&self.storage&&self.count<self.samples.size();
        if(!self.valid)return;
        self.valid=self.history->Revision(current)==MMS_OK&&current==revision&&
            !self.storage->Prepared()&&!self.storage->InCallback();
        auto& sample=self.samples[self.count++];sample={revision,self.native?*self.native:0,self.plugin?*self.plugin:0,
            self.history->UndoCount(),self.history->RedoCount(),self.history->LastPublicationEffects()};
        // These admission probes deliberately attempt no edit. Publication
        // must still hold its normal reentry exclusion throughout the signal.
        self.valid=self.valid&&self.history->SetPublicationSignal(self.Signal())==MMS_E_BUSY&&
            self.history->SetPublicationSignal({})==MMS_E_BUSY&&self.history->CheckNativeEdit()==MMS_E_BUSY&&
            self.history->History(false)==MMS_E_BUSY&&self.history->History(true)==MMS_E_BUSY&&self.history->Reset()==MMS_E_BUSY;
    }
    void Last(unsigned expected,uint64_t revision,int nativeValue,int pluginValue,size_t undo,size_t redo,
              HistoryEffects effects=HistoryEffects::All)const{
        Check(valid&&count==expected,"exactly one trusted signal per successful publication");
        const auto& last=samples[expected-1];
        Check(last.revision==revision&&last.native==nativeValue&&last.plugin==pluginValue&&last.undo==undo&&last.redo==redo&&
            last.effects==effects,"signal sees advanced revision, published state/history and frozen effects");
    }
};
static MmsResult Commit(SharedHistory& history,std::unique_ptr<SharedHistory::NativeTransaction> transaction){
    failAfter=0;const auto result=history.CommitNative(std::move(transaction));failAfter=-1;return result;
}
static void Native(SharedHistory& history,int& live,int target,SharedHistory::NativeIntent intent=SharedHistory::NativeIntent::ExplicitCommit){
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(history.PrepareNative("signal native",std::make_shared<NativeValue>(live,live,target),transaction,intent)==MMS_OK,
        "prepare signal native operation");
    Check(Commit(history,std::move(transaction))==MMS_OK,"native publication and signal do not allocate");
}
struct PluginValue {
    int value=0,candidate=0,before=0,after=0;unsigned commits=0,cancels=0,loads=0,releases=0;
    bool failPrepare=false,failLoad=false,throwCommit=false,throwLoad=false;
    std::function<void()> loading;
    MmsSerializerV1 Serializer(){
        MmsSerializerV1 table{};table.h=Header<MmsSerializerV1>();table.context=this;
        table.prepareLoad=[](void* p,MmsBytes bytes,uint32_t schema,MmsHandle* out)->MmsResult{
            auto& self=*static_cast<PluginValue*>(p);
            if(self.loading)self.loading();
            if(self.failLoad)return MMS_E_NOT_READY;
            if(schema&&(schema!=1||bytes.bytes!=sizeof(int)||!bytes.data))return MMS_E_ARGUMENT;
            self.candidate=0;if(schema)std::memcpy(&self.candidate,bytes.data,sizeof(int));*out=71;return MMS_OK;
        };
        table.commitLoad=[](void* p,MmsHandle handle){
            auto& self=*static_cast<PluginValue*>(p);Check(handle==71,"serializer load candidate identity");
            self.value=self.candidate;++self.loads;if(self.throwLoad)throw std::runtime_error("controlled serializer commit failure");
        };
        table.cancelLoad=[](void*,MmsHandle handle){Check(handle==71,"serializer cancellation candidate identity");};
        table.save=[](void* p,MmsBytes* bytes,uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& self=*static_cast<PluginValue*>(p);*bytes={&self.value,sizeof(int)};*schema=1;*lease=72;return MMS_OK;
        };
        table.releaseBytes=[](void*,MmsHandle handle){Check(handle==72,"serializer save lease identity");};return table;
    }
    MmsTransactionParticipantV1 Participant(){
        MmsTransactionParticipantV1 participant{};participant.h=Header<MmsTransactionParticipantV1>();participant.context=this;participant.pluginId=Literal(pluginId);
        participant.prepare=[](void* p)->MmsResult{return static_cast<PluginValue*>(p)->failPrepare?MMS_E_NOT_READY:MMS_OK;};
        participant.commit=[](void* p){auto& self=*static_cast<PluginValue*>(p);self.value=self.after;++self.commits;
            if(self.throwCommit)throw std::runtime_error("controlled participant commit failure");};
        participant.cancel=[](void* p){++static_cast<PluginValue*>(p)->cancels;};
        participant.getUndoState=[](void* p,MmsBytes* beforeBytes,MmsBytes* afterBytes,uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& self=*static_cast<PluginValue*>(p);*beforeBytes={&self.before,sizeof(int)};*afterBytes={&self.after,sizeof(int)};*schema=1;*lease=73;return MMS_OK;
        };
        participant.releaseUndoState=[](void* p,MmsHandle lease){Check(lease==73,"participant state lease identity");++static_cast<PluginValue*>(p)->releases;};
        return participant;
    }
};
static void InstallationAndGate(){
    StorageStore storage;bool ready=false;unsigned gates=0;SharedHistory history(storage,[&]{++gates;return ready?MMS_OK:MMS_E_NOT_READY;});
    int live=0;Sink sink;sink.history=&history;sink.storage=&storage;sink.native=&live;
    failAfter=0;const auto installed=history.SetPublicationSignal(sink.Signal());failAfter=-1;
    Check(installed==MMS_OK&&!gates,"signal installation neither allocates nor calls the application readiness gate");
    Check(history.SetPublicationSignal({nullptr,Sink::Published})==MMS_E_ARGUMENT&&
        history.SetPublicationSignal({&sink,nullptr})==MMS_E_ARGUMENT,"signal requires a valid callback/context pair");
    MmsResult workerInstall=MMS_OK,workerDetach=MMS_OK;
    std::thread worker([&]{workerInstall=history.SetPublicationSignal(sink.Signal());workerDetach=history.SetPublicationSignal({});});worker.join();
    Check(workerInstall==MMS_E_THREAD&&workerDetach==MMS_E_THREAD,"worker cannot install or detach history signal");
    Check(history.CheckNativeEdit()==MMS_E_NOT_READY&&!sink.count,"failed application gate emits no signal");ready=true;
    Check(history.Enter()==MMS_OK&&history.SetPublicationSignal({})==MMS_E_BUSY,"entered history cannot replace the signal");history.Leave();
    auto change=std::make_shared<NativeValue>(live,0,1);std::unique_ptr<SharedHistory::NativeTransaction> native;
    Check(history.PrepareNative("pending signal",change,native)==MMS_OK&&history.SetPublicationSignal({})==MMS_E_BUSY&&!sink.count,
        "prepared native reservation blocks sink replacement without signalling");native.reset();
    std::unique_ptr<SharedHistory::AppliedTransaction> applied;
    Check(history.ReserveAppliedNative("applied signal",1024,applied)==MMS_OK&&history.SetPublicationSignal({})==MMS_E_BUSY,
        "applied reservation blocks sink replacement");applied.reset();
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    Check(history.PrepareReset(reset)==MMS_OK&&history.SetPublicationSignal({})==MMS_E_BUSY,"reset reservation blocks sink replacement");reset.reset();
    Check(storage.Prepare({})==MMS_OK&&history.SetPublicationSignal({})==MMS_E_BUSY,"prepared storage blocks sink replacement");storage.Cancel();
    EditStore edits(history,storage);EditClient client;client.id=pluginId;auto api=edits.Api(client);MmsHandle transaction=0;
    Check(api.begin(api.context,Literal("pending plugin signal"),Revision(history),&transaction)==MMS_OK&&
        history.SetPublicationSignal({})==MMS_E_BUSY,"pending plugin transaction blocks sink replacement");api.cancel(api.context,transaction);
    MmsResult duringConfigure=MMS_OK;
    Check(history.Configure(SharedHistory::RecordingMode::ExplicitOnly,64,1024*1024,[&]{duringConfigure=history.SetPublicationSignal({});return MMS_OK;})==MMS_OK&&
        duringConfigure==MMS_E_BUSY&&!sink.count&&Revision(history)==1,"configuration callback cannot swap sink or generate value publication");
    Native(history,live,1);sink.Last(1,2,1,0,1,0);
    Check(history.SetPublicationSignal({})==MMS_OK,"UI can explicitly detach the sink at an idle boundary");
}
static void NativeFailuresAndHistory(){
    StorageStore storage;SharedHistory history(storage);int live=0;Sink sink;sink.history=&history;sink.storage=&storage;sink.native=&live;
    Check(history.SetPublicationSignal(sink.Signal())==MMS_OK,"install native signal sink");
    auto change=std::make_shared<NativeValue>(live,0,1);std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    change->prepareResult=MMS_E_NOT_READY;
    Check(history.PrepareNative("failed signal",change,transaction)==MMS_E_NOT_READY&&!transaction&&!sink.count&&Revision(history)==1,
        "failed preparation has no publication signal");change->prepareResult=MMS_OK;
    std::string allocationLabel="allocation signal";failAfter=0;
    const auto allocationFailure=history.PrepareNative(std::move(allocationLabel),change,transaction);failAfter=-1;
    Check(allocationFailure==MMS_E_MEMORY&&!transaction&&!sink.count&&Revision(history)==1,"allocation failure before preparation emits no signal");
    MmsResult preparingSwap=MMS_OK,cancellingSwap=MMS_OK;
    change->preparing=[&]{preparingSwap=history.SetPublicationSignal({});};
    change->destroyingCandidate=[&]{cancellingSwap=history.SetPublicationSignal({});};
    Check(history.PrepareNative("cancelled signal",change,transaction)==MMS_OK&&preparingSwap==MMS_E_BUSY&&!sink.count,
        "candidate preparation is not a publication and cannot replace sink");transaction.reset();
    Check(cancellingSwap==MMS_E_BUSY&&!sink.count&&Revision(history)==1,"candidate cancellation remains guarded and silent");
    change->preparing={};change->destroyingCandidate={};
    Check(history.PrepareNative("recorded signal",change,transaction)==MMS_OK&&!sink.count&&Commit(history,std::move(transaction))==MMS_OK,
        "recorded commit signals only after prepare");sink.Last(1,2,1,0,1,0);
    Check(history.History(true)==MMS_E_NOT_FOUND&&sink.count==1&&Revision(history)==2,"empty redo does not signal");
    change->armNoAllocation=true;const auto undo=history.History(false);failAfter=-1;
    Check(undo==MMS_OK,"undo publication tail and signal do not allocate");sink.Last(2,3,0,0,0,1);
    const auto redo=history.History(true);failAfter=-1;
    Check(redo==MMS_OK,"redo publication tail and signal do not allocate");sink.Last(3,4,1,0,1,0);change->armNoAllocation=false;
    change->prepareResult=MMS_E_NOT_READY;
    Check(history.History(false)==MMS_E_NOT_READY&&sink.count==3&&Revision(history)==4,"failed undo preparation does not signal");change->prepareResult=MMS_OK;
    auto stale=std::make_shared<NativeValue>(live,1,2);
    Check(history.PrepareNative("stale signal",stale,transaction)==MMS_OK,"prepare stale signal candidate");stale->stale=true;
    Check(Commit(history,std::move(transaction))==MMS_E_STALE&&live==1&&sink.count==3&&Revision(history)==4,"stale commit never signals");
    Check(history.SetPublicationSignal({})==MMS_OK,"detach native sink");
}
static void RecordingModes(){
    StorageStore storage;SharedHistory history(storage);int live=0;Sink sink;sink.history=&history;sink.storage=&storage;sink.native=&live;
    Check(history.SetPublicationSignal(sink.Signal())==MMS_OK,"install recording mode sink");Native(history,live,1);
    Check(history.History(false)==MMS_OK,"seed a redo branch for non-recorded adjustment");sink.Last(2,3,0,0,0,1);
    Check(history.SetMode(SharedHistory::RecordingMode::ExplicitOnly)==MMS_OK&&sink.count==2,"mode switch is not a value publication");
    Native(history,live,2,SharedHistory::NativeIntent::Adjustment);sink.Last(3,4,2,0,0,1);
    Check(history.SetMode(SharedHistory::RecordingMode::AllOperations)==MMS_OK,"enable recording all operations");
    Native(history,live,3,SharedHistory::NativeIntent::Adjustment);sink.Last(4,5,3,0,1,0);
    Check(history.SetMode(SharedHistory::RecordingMode::ExplicitOnly)==MMS_OK,"restore explicit-only mode");
    Native(history,live,4);sink.Last(5,6,4,0,2,0);
    Check(history.SetPublicationSignal({})==MMS_OK,"detach recording mode sink");
}
static void AppliedPublication(){
    StorageStore storage;SharedHistory history(storage);int live=0;Sink sink;sink.history=&history;sink.storage=&storage;sink.native=&live;
    Check(history.SetPublicationSignal(sink.Signal())==MMS_OK,"install authored native sink");
    auto change=std::make_shared<NativeValue>(live,0,7);std::unique_ptr<SharedHistory::AppliedTransaction> transaction;
    Check(history.ReserveAppliedNative("authored signal",1024,transaction)==MMS_OK&&!sink.count,"applied reservation is silent");live=7;
    Check(history.PrepareAppliedNative(*transaction,change)==MMS_OK&&!sink.count&&Revision(history)==1,"inverse verification is silent");
    failAfter=0;const auto committed=history.CommitAppliedNative(*transaction);failAfter=-1;
    Check(committed==MMS_OK&&!change->publications,"authored commit signals without replay or allocation");sink.Last(1,2,7,0,1,0);
    Check(history.CommitAppliedNative(*transaction)==MMS_E_CONFLICT&&sink.count==1&&history.SetPublicationSignal({})==MMS_E_BUSY,
        "consumed applied token cannot signal twice or detach its active sink");transaction.reset();
    Check(history.History(false)==MMS_OK,"undo authored publication");sink.Last(2,3,0,0,0,1);
    Check(history.History(true)==MMS_OK,"redo authored publication");sink.Last(3,4,7,0,1,0);
    auto failed=std::make_shared<NativeValue>(live,7,9);failed->prepareResult=MMS_E_NOT_READY;
    Check(history.ReserveAppliedNative("failed authored signal",1024,transaction)==MMS_OK,"reserve failed authored signal");live=9;
    Check(history.PrepareAppliedNative(*transaction,failed)==MMS_E_NOT_READY&&sink.count==3,"failed authored verification does not publish");
    live=7;transaction.reset();Check(sink.count==3&&Revision(history)==4,"cancelled authored state is restored before releasing token");
    Check(history.ReserveAppliedNative("stale authored signal",1024,transaction)==MMS_OK,"reserve stale authored signal");live=9;failed->prepareResult=MMS_OK;
    Check(history.PrepareAppliedNative(*transaction,failed)==MMS_OK,"prepare stale authored signal");live=10;
    Check(history.CommitAppliedNative(*transaction)==MMS_E_STALE&&sink.count==3&&Revision(history)==4,"stale authored commit never signals");live=7;transaction.reset();
    Check(history.SetMode(SharedHistory::RecordingMode::ExplicitOnly)==MMS_OK&&
        history.ReserveAppliedNative("unrecorded authored signal",1024,transaction,SharedHistory::NativeIntent::Adjustment)==MMS_OK,
        "reserve unrecorded authored adjustment");auto preview=std::make_shared<NativeValue>(live,7,8);live=8;
    Check(history.PrepareAppliedNative(*transaction,preview)==MMS_OK,"prepare unrecorded authored adjustment");
    failAfter=0;const auto adjusted=history.CommitAppliedNative(*transaction);failAfter=-1;
    Check(adjusted==MMS_OK,"unrecorded authored adjustment publishes without allocation");sink.Last(4,5,8,0,1,0);transaction.reset();
    Check(history.SetPublicationSignal({})==MMS_OK,"detach authored native sink");
}
static void PluginPublication(){
    StorageStore storage;SharedHistory history(storage);PluginValue value;StorageClient provider;provider.id=pluginId;storage.Api(provider);
    auto serializer=value.Serializer();MmsHandle registration=0;
    Check(storage.Register(provider,Literal(pluginId),&serializer,sizeof(serializer),&registration)==MMS_OK,"register actual history serializer");
    EditStore edits(history,storage);EditClient client;client.id=pluginId;auto api=edits.Api(client);int native=0;
    Sink sink;sink.history=&history;sink.storage=&storage;sink.native=&native;sink.plugin=&value.value;
    Check(history.SetPublicationSignal(sink.Signal())==MMS_OK,"install plugin publication sink");
    MmsResult storageSwap=MMS_OK;value.loading=[&]{storageSwap=history.SetPublicationSignal({});};
    StoredData initial;initial.schema=1;initial.bytes.resize(sizeof(int));
    Check(storage.PrepareRestore({{pluginId,initial}})==MMS_OK&&storageSwap==MMS_E_BUSY&&!sink.count,
        "standalone serializer callback cannot replace sink or signal publication");storage.Cancel();value.loading={};
    const auto begin=[&](int after){value.before=value.value;value.after=after;MmsHandle transaction=0;auto participant=value.Participant();
        Check(api.begin(api.context,Literal("plugin signal"),Revision(history),&transaction)==MMS_OK&&api.join(api.context,transaction,&participant)==MMS_OK,
            "begin and join actual EditStore publication");return transaction;};
    value.failPrepare=true;auto transaction=begin(10);
    Check(api.prepare(api.context,transaction)==MMS_E_NOT_READY&&!sink.count&&Revision(history)==1,"failed plugin prepare does not signal");value.failPrepare=false;
    transaction=begin(10);Check(api.prepare(api.context,transaction)==MMS_OK&&!sink.count,"prepared plugin publication stays silent");api.cancel(api.context,transaction);
    Check(!sink.count&&value.value==0&&Revision(history)==1,"cancelled plugin publication stays silent");
    transaction=begin(10);Check(api.prepare(api.context,transaction)==MMS_OK&&!sink.count,"prepare actual plugin value change");
    failAfter=0;const auto committed=api.commit(api.context,transaction);failAfter=-1;
    Check(committed==MMS_OK,"plugin commit and signal do not allocate");sink.Last(1,2,0,10,1,0);
    MmsResult loadSwap=MMS_OK;value.loading=[&]{loadSwap=history.SetPublicationSignal({});};
    Check(api.undo(api.context)==MMS_OK&&loadSwap==MMS_E_BUSY,"serializer restoration cannot replace signal");sink.Last(2,3,0,0,0,1);
    Check(api.redo(api.context)==MMS_OK,"plugin redo emits trusted signal");sink.Last(3,4,0,10,1,0);value.loading={};
    auto mixed=std::make_shared<NativeValue>(native,0,5);mixed->effects=HistoryEffects::Viewport;transaction=begin(20);
    Check(edits.JoinNative(client,transaction,mixed)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK&&sink.count==3&&
        mixed->effectsQueries==1,
        "mixed plugin/native prepare is silent and freezes the renderer mask");
    failAfter=0;const auto combined=api.commit(api.context,transaction);failAfter=-1;
    Check(combined==MMS_OK,"mixed plugin/native publication does not allocate");
    sink.Last(4,5,5,20,2,0,HistoryEffects::All);
    value.failLoad=true;Check(api.undo(api.context)==MMS_E_NOT_READY&&sink.count==4&&Revision(history)==5&&native==5&&value.value==20,
        "failed serializer restore leaves live state revision and signal unchanged");value.failLoad=false;
    auto stale=std::make_shared<NativeValue>(native,5,6);transaction=begin(30);
    Check(edits.JoinNative(client,transaction,stale)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK,"prepare stale mixed publication");stale->stale=true;
    Check(api.commit(api.context,transaction)==MMS_E_STALE&&sink.count==4&&native==5&&value.value==20&&Revision(history)==5,
        "failed mixed commit emits no signal");
    transaction=begin(99);Check(api.prepare(api.context,transaction)==MMS_OK,"prepare nonconforming participant");value.throwCommit=true;
    Check(api.commit(api.context,transaction)==MMS_E_FAILED&&sink.count==4,"partly failed participant cannot publish a trusted revision signal");
    Check(history.SetPublicationSignal({})==MMS_OK,"poisoned idle history still allows safe explicit sink detach");
}
static void FailedSerializerCommit(){
    StorageStore storage;SharedHistory history(storage);PluginValue value;StorageClient provider;provider.id=pluginId;storage.Api(provider);
    auto serializer=value.Serializer();MmsHandle registration=0;
    Check(storage.Register(provider,Literal(pluginId),&serializer,sizeof(serializer),&registration)==MMS_OK,"register failed-restore serializer fixture");
    EditStore edits(history,storage);EditClient client;client.id=pluginId;auto api=edits.Api(client);
    Sink sink;sink.history=&history;sink.storage=&storage;sink.plugin=&value.value;
    Check(history.SetPublicationSignal(sink.Signal())==MMS_OK,"install failed-restore signal sink");
    value.before=0;value.after=10;auto participant=value.Participant();MmsHandle transaction=0;
    Check(api.begin(api.context,Literal("serializer failure signal"),Revision(history),&transaction)==MMS_OK&&
        api.join(api.context,transaction,&participant)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK&&
        api.commit(api.context,transaction)==MMS_OK,"seed actual serializer-backed history publication");sink.Last(1,2,0,10,1,0);
    value.throwLoad=true;
    Check(api.undo(api.context)==MMS_E_FAILED&&storage.Poisoned()&&sink.count==1&&history.UndoCount()==1&&!history.RedoCount(),
        "failed void serializer commit never advances journal or emits trusted signal");
    Check(history.SetPublicationSignal({})==MMS_OK,"poisoned serializer history permits idle sink detach");
}
static void ResetAndDetach(){
    StorageStore storage;SharedHistory history(storage);int live=0;Sink first;first.history=&history;first.storage=&storage;first.native=&live;
    Check(history.SetPublicationSignal(first.Signal())==MMS_OK,"install project boundary signal sink");Native(history,live,1);first.Last(1,2,1,0,1,0);
    Check(history.Reset()==MMS_OK&&Revision(history)==3&&first.count==1&&!history.UndoCount(),"project reset advances epoch revision without value notification");
    std::unique_ptr<SharedHistory::ResetTransaction> reset;Check(history.PrepareReset(reset)==MMS_OK&&first.count==1,"prepare project reset stays silent");
    Check(reset->Commit()==MMS_OK&&Revision(history)==4&&first.count==1,"destructive project boundary is not a value publication");reset.reset();
    Native(history,live,2);first.Last(2,5,2,0,1,0);
    failAfter=0;const auto detached=history.SetPublicationSignal({});failAfter=-1;Check(detached==MMS_OK,"detaching history sink does not allocate");
    Native(history,live,3);Check(first.count==2&&Revision(history)==6,"detached borrowed context receives no later publication");
    Sink replacement;replacement.history=&history;replacement.storage=&storage;replacement.native=&live;
    Check(history.SetPublicationSignal(replacement.Signal())==MMS_OK,"install replacement history sink");Native(history,live,4);
    replacement.Last(1,7,4,0,3,0);Check(first.count==2,"replacement preserves old sink lifetime boundary");
    Check(history.SetPublicationSignal({})==MMS_OK,"detach replacement before its context is destroyed");
    Sink surviving;surviving.storage=&storage;surviving.native=&live;
    auto temporary=std::make_unique<SharedHistory>(storage);surviving.history=temporary.get();
    Check(temporary->SetPublicationSignal(surviving.Signal())==MMS_OK,"install destructor silence probe");
    std::unique_ptr<SharedHistory::NativeTransaction> pending;
    Check(temporary->PrepareNative("destroyed candidate",std::make_shared<NativeValue>(live,4,5),pending)==MMS_OK,"prepare candidate owned by expiring history");
    temporary.reset();pending.reset();Check(!surviving.count,"history destruction invalidates pending work without signalling a dead host");
}
static void EffectsPublication(){
    StorageStore storage;SharedHistory history(storage);int live=0;Sink sink;sink.history=&history;sink.storage=&storage;sink.native=&live;
    Check(history.SetPublicationSignal(sink.Signal())==MMS_OK,"install effects publication sink");
    // Renderer-only recorded native: the mask is frozen during preparation so
    // neither the allocation-checked commit nor undo/redo may query it again.
    auto change=std::make_shared<NativeValue>(live,0,1);change->effects=HistoryEffects::Viewport;
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(history.PrepareNative("effects renderer",change,transaction)==MMS_OK&&change->effectsQueries==1,
        "renderer effects are frozen once during preparation");
    change->effects=HistoryEffects::ProjectData;
    failAfter=0;const auto committed=history.CommitNative(std::move(transaction));failAfter=-1;
    Check(committed==MMS_OK&&change->effectsQueries==1,
        "recorded commit publishes the frozen renderer mask without querying or allocating");
    sink.Last(1,2,1,0,1,0,HistoryEffects::Viewport);
    change->armNoAllocation=true;
    const auto undo=history.History(false);failAfter=-1;
    Check(undo==MMS_OK&&change->effectsQueries==1,
        "undo publishes the frozen renderer mask from the candidate tail without querying or allocating");
    sink.Last(2,3,0,0,0,1,HistoryEffects::Viewport);
    const auto redo=history.History(true);failAfter=-1;
    Check(redo==MMS_OK&&change->effectsQueries==1,
        "redo publishes the frozen renderer mask from the candidate tail without querying or allocating");
    sink.Last(3,4,1,0,1,0,HistoryEffects::Viewport);
    change->armNoAllocation=false;

    // Applied native with a mask deliberately different from the previous
    // renderer publication; the placeholder must be replaced during prepare.
    auto applied=std::make_shared<NativeValue>(live,1,3);applied->effects=HistoryEffects::None;
    std::unique_ptr<SharedHistory::AppliedTransaction> authored;
    Check(history.ReserveAppliedNative("effects applied",1024,authored)==MMS_OK&&sink.count==3&&applied->effectsQueries==0,
        "applied reservation publishes and queries nothing");
    live=3;
    Check(history.PrepareAppliedNative(*authored,applied)==MMS_OK&&applied->effectsQueries==1&&sink.count==3,
        "applied preparation freezes the actual mask without publishing");
    applied->effects=HistoryEffects::SceneReadout;
    failAfter=0;const auto appliedCommitted=history.CommitAppliedNative(*authored);failAfter=-1;
    Check(appliedCommitted==MMS_OK&&applied->effectsQueries==1,
        "applied commit publishes the frozen None mask without querying or allocating");
    sink.Last(4,5,3,0,2,0,HistoryEffects::None);
    Check(history.CommitAppliedNative(*authored)==MMS_E_CONFLICT&&sink.count==4,
        "repeated applied commit cannot publish or query a second mask");
    authored.reset();
    applied->armNoAllocation=true;
    Check(history.History(false)==MMS_OK&&history.LastPublicationEffects()==HistoryEffects::None,
        "applied undo publishes the frozen None mask without allocation");failAfter=-1;
    sink.Last(5,6,1,0,1,1,HistoryEffects::None);
    Check(history.History(true)==MMS_OK&&history.LastPublicationEffects()==HistoryEffects::None,
        "applied redo publishes the frozen None mask without allocation");failAfter=-1;
    sink.Last(6,7,3,0,2,0,HistoryEffects::None);
    applied->armNoAllocation=false;
    Check(applied->effectsQueries==1,"publication and traversal never re-query the effects getter");
    Check(history.SetPublicationSignal({})==MMS_OK,"detach effects publication sink");
}
} // namespace history_publication_signal_checks
static void HistoryPublicationSignalChecks(){
    history_publication_signal_checks::InstallationAndGate();
    history_publication_signal_checks::NativeFailuresAndHistory();
    history_publication_signal_checks::RecordingModes();
    history_publication_signal_checks::AppliedPublication();
    history_publication_signal_checks::EffectsPublication();
    history_publication_signal_checks::PluginPublication();
    history_publication_signal_checks::FailedSerializerCommit();
    history_publication_signal_checks::ResetAndDetach();
}
