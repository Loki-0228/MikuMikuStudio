struct EditHostFixture : EditHost {
    StorageStore& storage;
    uint64_t revision=1;
    bool locked=false,failReserve=false,poisoned=false,detachedPending=false;
    EditStore* attached=nullptr;
    unsigned attaches=0,detaches=0;
    std::weak_ptr<const EditRecord> reserved;
    unsigned entries=0;size_t cursor=0;
    std::vector<std::shared_ptr<const EditRecord>> history;
    explicit EditHostFixture(StorageStore& s):storage(s){}
    void AttachStore(EditStore* store) override{attached=store;++attaches;}
    void DetachStore(EditStore* store) noexcept override{
        detachedPending=store->HasPending();if(attached==store)attached=nullptr;++detaches;
    }
    void Poison() noexcept override{poisoned=true;}
    MmsResult Revision(uint64_t& r) override{r=revision;return MMS_OK;}
    MmsResult Enter() override{if(locked)return MMS_E_BUSY;locked=true;return MMS_OK;}
    void Leave() noexcept override{locked=false;}
    struct Publication:EditPublication{
        EditHostFixture& host;std::shared_ptr<const EditRecord> record;
        Publication(EditHostFixture& h,std::shared_ptr<const EditRecord> r):host(h),record(std::move(r)){}
        void Publish() noexcept override{
            host.history.resize(host.cursor);host.history.push_back(std::move(record));++host.cursor;++host.revision;++host.entries;
        }
    };
    MmsResult Reserve(std::shared_ptr<const EditRecord> record,std::unique_ptr<EditPublication>& out) override{
        CHECK(locked);if(failReserve)return MMS_E_MEMORY;
        history.reserve(history.size()+1);reserved=record;out=std::make_unique<Publication>(*this,std::move(record));return MMS_OK;
    }
    MmsResult History(bool redo) override{
        if(locked)return MMS_E_BUSY;if(redo?cursor==history.size():!cursor)return MMS_E_NOT_FOUND;
        auto& record=*history[redo?cursor:cursor-1];auto r=storage.PrepareRestore(redo?record.after:record.before);if(r)return r;
        if(!storage.Commit())return MMS_E_FAILED;if(redo)++cursor;else --cursor;++revision;return MMS_OK;
    }
};
struct EditParticipantFixture {
    SerializerFixture* state;
    int before=0,after=0,prepares=0,commits=0,cancels=0,releases=0,reads=0;
    bool failPrepare=false,failRead=false,badBytes=false,throwCommit=false,throwCancel=false;
    std::function<void()> onCancel,onCommit,onPrepare,onRead,onRelease,onLeasedRead;
    MmsEditApiV1 api{};MmsHandle transaction=0;
    MmsTransactionParticipantV1 Api(const char* id){
        MmsTransactionParticipantV1 p{Header<MmsTransactionParticipantV1>(),this,S(id)};
        p.prepare=[](void* context)->MmsResult{auto& f=*static_cast<EditParticipantFixture*>(context);++f.prepares;
            if(f.api.prepare)CHECK(f.api.prepare(f.api.context,f.transaction)==MMS_E_BUSY);
            if(f.onPrepare)f.onPrepare();
            return f.failPrepare?MMS_E_NOT_READY:MMS_OK;};
        p.commit=[](void* context){auto& f=*static_cast<EditParticipantFixture*>(context);++f.commits;f.state->value=f.after;if(f.onCommit)f.onCommit();if(f.throwCommit)throw std::runtime_error("nonconforming plugin");};
        p.cancel=[](void* context){auto& f=*static_cast<EditParticipantFixture*>(context);++f.cancels;if(f.onCancel)f.onCancel();if(f.throwCancel)throw std::runtime_error("nonconforming cancel");};
        p.getUndoState=[](void* context,MmsBytes* before,MmsBytes* after,uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& f=*static_cast<EditParticipantFixture*>(context);++f.reads;if(f.onRead)f.onRead();if(f.failRead)return MMS_E_FAILED;
            *before={&f.before,sizeof(int)};*after={f.badBytes?nullptr:&f.after,sizeof(int)};*schema=7;*lease=77;if(f.onLeasedRead)f.onLeasedRead();return MMS_OK;};
        p.releaseUndoState=[](void* context,MmsHandle h){auto& f=*static_cast<EditParticipantFixture*>(context);CHECK(h==77);++f.releases;if(f.onRelease)f.onRelease();};return p;
    }
};
static void EditLifecycleChecks(){
    StorageStore storage;StorageClient sc;sc.id="example.lifecycle";storage.Api(sc);
    SerializerFixture state;auto serializer=state.Api();MmsHandle provider=0;
    CHECK(storage.Register(sc,S("example.lifecycle"),&serializer,sizeof(serializer),&provider)==MMS_OK);
    EditHostFixture host(storage);
    EditParticipantFixture prepared{&state,0,1},unprepared{&state,0,2},atDestruction{&state,0,3};
    {
        EditStore store(host,storage);EditClient client;client.id=sc.id;auto api=store.Api(client);
        CHECK(host.attached==&store && host.attaches==1 && !store.HasPending());
        MmsHandle first=919,second=0;
        host.locked=true;
        CHECK(api.begin(api.context,S("locked begin"),host.revision,&first)==MMS_E_BUSY && first==919);
        host.locked=false;
        CHECK(api.begin(api.context,S(""),host.revision,&first)==MMS_E_ARGUMENT && first==919 && !host.locked);
        auto p=prepared.Api("example.lifecycle"),u=unprepared.Api("example.lifecycle");
        CHECK(api.begin(api.context,S("prepared"),host.revision,&first)==MMS_OK);
        CHECK(api.join(api.context,first,&p)==MMS_OK && api.prepare(api.context,first)==MMS_OK);
        CHECK(api.begin(api.context,S("unprepared"),host.revision,&second)==MMS_OK);
        CHECK(api.join(api.context,second,&u)==MMS_OK && store.HasPending() && !host.reserved.expired());
        MmsResult fromWorker=MMS_OK;
        std::thread worker([&]{fromWorker=store.CancelPending();});worker.join();
        CHECK(fromWorker==MMS_E_THREAD && store.HasPending() && !prepared.cancels && !unprepared.cancels);
        prepared.onCancel=[&]{
            CHECK(store.InCallback() && store.CancelPending()==MMS_E_BUSY);
            MmsHandle unchanged=733;
            CHECK(api.begin(api.context,S("cancel reentry"),host.revision,&unchanged)==MMS_E_BUSY && unchanged==733);
        };
        // The caller may already hold the host gate at a safe lifecycle boundary.
        host.locked=true;
        CHECK(store.CancelPending()==MMS_OK && host.locked);
        host.locked=false;
        CHECK(!store.HasPending() && host.reserved.expired() && !host.entries);
        CHECK(prepared.cancels==1 && unprepared.cancels==1 && !prepared.commits && !unprepared.commits && state.value==0);
        CHECK(store.CancelPending()==MMS_OK && prepared.cancels==1 && unprepared.cancels==1);
        CHECK(api.prepare(api.context,first)==MMS_E_STALE && api.prepare(api.context,second)==MMS_E_STALE);
        auto d=atDestruction.Api("example.lifecycle");
        CHECK(api.begin(api.context,S("destructor cancellation"),host.revision,&first)==MMS_OK);
        CHECK(api.join(api.context,first,&d)==MMS_OK && api.prepare(api.context,first)==MMS_OK);
    }
    CHECK(!host.attached && host.detaches==1 && !host.detachedPending && host.reserved.expired());
    CHECK(atDestruction.cancels==1 && !atDestruction.commits);
    EditParticipantFixture bad{&state,0,4},remaining{&state,0,5};bad.throwCancel=true;
    {
        EditStore store(host,storage);EditClient client;client.id=sc.id;auto api=store.Api(client);
        MmsHandle first=0,second=0;auto p=bad.Api("example.lifecycle"),q=remaining.Api("example.lifecycle");
        CHECK(api.begin(api.context,S("throwing cancel"),host.revision,&first)==MMS_OK);
        CHECK(api.join(api.context,first,&p)==MMS_OK && api.prepare(api.context,first)==MMS_OK);
        CHECK(api.begin(api.context,S("remaining cancel"),host.revision,&second)==MMS_OK && api.join(api.context,second,&q)==MMS_OK);
        CHECK(store.CancelPending()==MMS_E_FAILED && !store.HasPending() && host.poisoned && host.reserved.expired());
        CHECK(bad.cancels==1 && remaining.cancels==1 && !host.entries && state.value==0);
        CHECK(api.begin(api.context,S("after failed cancel"),host.revision,&first)==MMS_E_NOT_READY);
        CHECK(store.CancelPending()==MMS_OK && bad.cancels==1 && remaining.cancels==1);
    }
    CHECK(!host.attached && host.attaches==2 && host.detaches==2 && !host.detachedPending);
}
struct PropertyStageHostFixture:EditHostFixture {
    unsigned callbacks=0,callbackDepth=0;
    std::function<void()> onReserve;
    explicit PropertyStageHostFixture(StorageStore& storage):EditHostFixture(storage){}
    MmsResult WithParticipantCallback(MmsResult (*call)(void*),void* context) override {
        CHECK(attached&&attached->InCallback());++callbacks;++callbackDepth;
        const auto result=Boundary([&]{return call(context);});--callbackDepth;return result;
    }
    MmsResult Reserve(std::shared_ptr<const EditRecord> record,std::unique_ptr<EditPublication>& out) override {
        const auto result=EditHostFixture::Reserve(std::move(record),out);if(!result&&onReserve)onReserve();return result;
    }
};
struct PropertyStageCase {
    StorageStore storage;StorageClient sa,sb;SerializerFixture stateA,stateB;
    EditClient a,b;PropertyStageHostFixture host;EditStore store;
    MmsEditApiV1 api,other;MmsHandle providerA=0,providerB=0;
    PropertyStageCase():host(storage),store(host,storage){
        sa.id="example.stage.a";sb.id="example.stage.b";storage.Api(sa);storage.Api(sb);a.id=sa.id;b.id=sb.id;
        auto serialA=stateA.Api(),serialB=stateB.Api();
        CHECK(storage.Register(sa,S("example.stage.a"),&serialA,sizeof(serialA),&providerA)==MMS_OK);
        CHECK(storage.Register(sb,S("example.stage.b"),&serialB,sizeof(serialB),&providerB)==MMS_OK);
        api=store.Api(a);other=store.Api(b);
    }
    MmsHandle Begin(){MmsHandle tx=0;CHECK(api.begin(api.context,S("property stage"),host.revision,&tx)==MMS_OK);return tx;}
    void ReplaceSerializer(){auto serial=stateA.Api();CHECK(storage.Unregister(sa,providerA)==MMS_OK);
        CHECK(storage.Register(sa,S("example.stage.a"),&serial,sizeof(serial),&providerA)==MMS_OK);}
};
struct PropertyRegistrationGuard:EditStageGuard {
    const bool& current;std::function<void()> released;
    explicit PropertyRegistrationGuard(const bool& value):current(value){}
    ~PropertyRegistrationGuard(){if(released)released();}
    bool IsCurrent()const noexcept override{return current;}
};
struct PropertyStageCapture {
    EditStore& store;PropertyStageHostFixture& host;MmsEditApiV1 api;unsigned& released;
    PropertyStageCapture(EditStore& value,PropertyStageHostFixture& owner,MmsEditApiV1 calls,unsigned& result):store(value),host(owner),api(calls),released(result){}
    ~PropertyStageCapture(){released=(store.InCallback()?1u:0u)|(host.locked?2u:0u);MmsHandle output=317;
        if(api.begin(api.context,S("capture destructor"),host.revision,&output)==MMS_E_BUSY&&output==317)released|=4;}
};
static void PropertyStageChecks(){
    {
        PropertyStageCase c;bool current=true;unsigned guardReleased=0,captureReleased=0;
        auto registration=std::make_shared<PropertyRegistrationGuard>(current);
        registration->released=[&]{guardReleased=(c.store.InCallback()?1u:0u)|(c.host.locked?2u:0u);};
        std::weak_ptr<const EditStageGuard> retained=registration;
        EditParticipantFixture participant{&c.stateA,0,10};auto p=participant.Api("example.stage.a");
        const auto tx=c.Begin(),untouched=c.Begin();
        auto capture=std::make_shared<PropertyStageCapture>(c.store,c.host,c.api,captureReleased);
        std::function<MmsResult(PropertyStageToken&)> stage=[&,capture](PropertyStageToken& token)->MmsResult {
            CHECK(c.store.InCallback()&&c.host.locked);
            MmsHandle output=713;
            CHECK(c.api.begin(c.api.context,S("stage reentry"),c.host.revision,&output)==MMS_E_BUSY&&output==713);
            CHECK(c.api.join(c.api.context,tx,&p)==MMS_E_BUSY);
            CHECK(c.api.prepare(c.api.context,tx)==MMS_E_BUSY&&c.api.commit(c.api.context,tx)==MMS_E_BUSY);
            CHECK(c.api.undo(c.api.context)==MMS_E_BUSY&&c.api.redo(c.api.context)==MMS_E_BUSY);
            c.api.cancel(c.api.context,tx);CHECK(c.store.HasPending());
            CHECK(c.store.StageProperty(c.a,untouched,registration,[](PropertyStageToken&){return MMS_OK;})==MMS_E_BUSY);
            CHECK(c.store.JoinPropertyStage(token,c.b,&p)==MMS_E_STALE);
            CHECK(c.store.JoinPropertyStage(token,c.a,&p)==MMS_OK);
            CHECK(c.store.JoinPropertyStage(token,c.a,&p)==MMS_E_CONFLICT);
            return MMS_OK;
        };
        capture.reset();CHECK(c.store.StageProperty(c.a,tx,registration,std::move(stage))==MMS_OK);
        CHECK(captureReleased==7&&!c.host.locked&&!c.store.InCallback()&&c.stateA.value==0&&!c.host.entries);
        CHECK(c.store.StageProperty(c.a,tx,registration,[&](PropertyStageToken&){participant.after=20;return MMS_OK;})==MMS_OK);
        registration.reset();CHECK(!retained.expired()&&!guardReleased);
        CHECK(c.api.prepare(c.api.context,tx)==MMS_OK&&c.host.callbacks==3&&!retained.expired());
        auto another=std::make_shared<PropertyRegistrationGuard>(current);
        CHECK(c.store.StageProperty(c.a,tx,another,[](PropertyStageToken&){return MMS_OK;})==MMS_E_CONFLICT);
        CHECK(c.api.commit(c.api.context,tx)==MMS_OK&&c.host.callbacks==4&&retained.expired()&&guardReleased==3);
        CHECK(c.stateA.value==20&&participant.commits==1&&!participant.cancels&&c.host.entries==1);
        CHECK(c.api.prepare(c.api.context,untouched)==MMS_E_STALE); // its expected revision became stale, not cancelled by stage.
        CHECK(!c.store.HasPending()&&c.api.undo(c.api.context)==MMS_OK&&c.stateA.value==0);
        CHECK(c.api.redo(c.api.context)==MMS_OK&&c.stateA.value==20);
    }
    {
        PropertyStageCase c;bool current=true;unsigned released=0,captureReleased=0;auto registration=std::make_shared<PropertyRegistrationGuard>(current);
        registration->released=[&]{MmsHandle output=177;released=(c.store.InCallback()?1u:0u)|(c.host.locked?2u:0u);
            if(c.api.begin(c.api.context,S("guard destructor"),c.host.revision,&output)==MMS_E_BUSY&&output==177)released|=4;};
        std::weak_ptr<const EditStageGuard> retained=registration;
        EditParticipantFixture first{&c.stateA,0,11},foreign{&c.stateB,0,12},second{&c.stateA,0,13};
        auto p=first.Api("example.stage.a"),q=foreign.Api("example.stage.b"),r=second.Api("example.stage.a");
        const auto tx=c.Begin(),other=c.Begin();CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        CHECK(c.other.join(c.other.context,tx,&q)==MMS_OK&&c.api.join(c.api.context,other,&r)==MMS_OK);
        CHECK(c.store.StageProperty(c.a,tx,registration,[](PropertyStageToken&){return MMS_OK;})==MMS_OK);
        unsigned blocked=0;first.onCancel=[&]{blocked=(c.store.InCallback()?1u:0u)|(c.host.locked?2u:0u);
            CHECK(c.host.callbackDepth==1&&c.store.CancelPending()==MMS_E_BUSY);};
        auto capture=std::make_shared<PropertyStageCapture>(c.store,c.host,c.api,captureReleased);
        std::function<MmsResult(PropertyStageToken&)> failure=[capture](PropertyStageToken&){return MMS_E_ARGUMENT;};capture.reset();
        CHECK(c.store.StageProperty(c.a,tx,std::move(registration),std::move(failure))==MMS_E_ARGUMENT);
        CHECK(retained.expired()&&released==7&&captureReleased==7);
        CHECK(first.cancels==1&&foreign.cancels==1&&!second.cancels&&blocked==3&&c.host.callbacks==2);
        CHECK(c.api.prepare(c.api.context,tx)==MMS_E_STALE&&c.api.prepare(c.api.context,other)==MMS_OK);
        CHECK(c.api.commit(c.api.context,other)==MMS_OK&&c.stateA.value==13&&c.host.entries==1);
    }
    {
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,1};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        const auto noop=[](PropertyStageToken&){return MMS_OK;};EditClient alias;alias.id=c.a.id;c.store.Api(alias);
        CHECK(c.store.StageProperty(c.b,tx,guard,noop)==MMS_E_STALE);
        CHECK(c.store.StageProperty(alias,tx,guard,noop)==MMS_E_STALE);
        CHECK(c.store.StageProperty(c.a,tx+71,guard,noop)==MMS_E_STALE);
        CHECK(c.store.StageProperty(c.a,tx,{},noop)==MMS_E_ARGUMENT);
        CHECK(c.store.StageProperty(c.a,tx,guard,{})==MMS_E_ARGUMENT);
        MmsResult worker=MMS_OK;std::thread thread([&]{worker=c.store.StageProperty(c.a,tx,guard,noop);});thread.join();
        CHECK(worker==MMS_E_THREAD&&!participant.cancels);
        CHECK(c.api.prepare(c.api.context,tx)==MMS_OK&&c.api.commit(c.api.context,tx)==MMS_OK);
    }
    // A stage must have its own participant. A native/empty candidate or a
    // different plugin's participant cannot satisfy that requirement.
    for(unsigned scenario=0;scenario<2;++scenario){
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture foreign{&c.stateB,0,1};auto p=foreign.Api("example.stage.b");const auto tx=c.Begin();
        if(scenario)CHECK(c.other.join(c.other.context,tx,&p)==MMS_OK);
        CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_OK;})==MMS_E_ARGUMENT);
        CHECK(!c.store.HasPending()&&!c.host.entries&&foreign.cancels==int(scenario));
    }
    // Registration identity is checked throughout preparation and again before
    // the first void commit. No stale phase may publish or advance history.
    for(unsigned phase=0;phase<8;++phase){
        PropertyStageCase c;bool current=phase!=6;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,9};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        const auto staged=c.store.StageProperty(c.a,tx,guard,[&](PropertyStageToken&){if(phase==7)current=false;return MMS_OK;});
        if(phase>=6)CHECK(staged==MMS_E_STALE);
        else{
            CHECK(staged==MMS_OK);
            if(phase==0)current=false;
            if(phase==1)participant.onPrepare=[&]{current=false;};
            if(phase==2)participant.onRead=[&]{current=false;};
            if(phase==3)participant.onRelease=[&]{current=false;};
            if(phase==4)c.host.onReserve=[&]{current=false;};
            if(phase==5){CHECK(c.api.prepare(c.api.context,tx)==MMS_OK);current=false;
                CHECK(c.api.commit(c.api.context,tx)==MMS_E_STALE);}
            else CHECK(c.api.prepare(c.api.context,tx)==MMS_E_STALE);
        }
        CHECK(participant.cancels==1&&!participant.commits&&!c.store.HasPending()&&!c.host.entries&&c.stateA.value==0);
        CHECK(!c.host.locked&&!c.store.InCallback());
    }
    for(unsigned phase=0;phase<4;++phase){
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,9};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        if(phase==0)c.ReplaceSerializer();
        const auto staged=c.store.StageProperty(c.a,tx,guard,[&](PropertyStageToken&){if(phase==3)c.ReplaceSerializer();return MMS_OK;});
        if(phase==0||phase==3)CHECK(staged==MMS_E_STALE);
        else{CHECK(staged==MMS_OK);if(phase==2)CHECK(c.api.prepare(c.api.context,tx)==MMS_OK);
            c.ReplaceSerializer();CHECK((phase==1?c.api.prepare(c.api.context,tx):c.api.commit(c.api.context,tx))==MMS_E_STALE);}
        CHECK(participant.cancels==1&&!participant.commits&&!c.store.HasPending()&&!c.host.entries);
    }
    for(unsigned scenario=0;scenario<5;++scenario){
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,9};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);if(scenario==0)++c.host.revision;
        const auto result=c.store.StageProperty(c.a,tx,guard,[&](PropertyStageToken&)->MmsResult {
            if(scenario==1)++c.host.revision;
            if(scenario==2)c.a.stopping=true;
            if(scenario==3)throw std::bad_alloc();
            if(scenario==4)throw std::runtime_error("stage exception");
            return MMS_OK;
        });
        CHECK(result==(scenario<2?MMS_E_STALE:scenario==2?MMS_E_NOT_READY:scenario==3?MMS_E_MEMORY:MMS_E_FAILED));
        CHECK(participant.cancels==1&&!participant.commits&&!c.store.HasPending()&&!c.host.entries&&!c.host.poisoned);
    }
    {
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,9};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_OK;})==MMS_OK);
        participant.onLeasedRead=[] {throw std::runtime_error("read after acquired lease");};
        CHECK(c.api.prepare(c.api.context,tx)==MMS_E_FAILED&&participant.releases==1&&participant.cancels==1);
        CHECK(c.host.callbacks==4&&!c.store.HasPending()&&!c.host.entries&&!c.host.poisoned);
    }
    {
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,9};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_OK;})==MMS_OK);
        participant.onCommit=[] {throw std::bad_alloc();};
        CHECK(c.api.prepare(c.api.context,tx)==MMS_OK&&c.api.commit(c.api.context,tx)==MMS_E_FAILED);
        CHECK(participant.commits==1&&!participant.cancels&&c.host.poisoned&&!c.store.HasPending()&&!c.host.entries);
        CHECK(c.stateA.value==9&&c.host.callbacks==4); // A violated void commit freezes history; its context is already consumed.
    }
    {
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,1};auto p=participant.Api("example.stage.a");const auto tx=c.Begin(),other=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);c.host.locked=true;
        participant.onCancel=[&]{CHECK(c.store.InCallback()&&c.host.locked&&c.host.callbackDepth==1);};
        CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_OK;})==MMS_E_BUSY);
        CHECK(c.host.locked&&participant.cancels==1&&c.store.HasPending());c.host.locked=false;
        c.api.cancel(c.api.context,other);CHECK(!c.store.HasPending());
    }
    {
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture first{&c.stateA,0,1},second{&c.stateB,0,2};first.throwCancel=true;
        auto p=first.Api("example.stage.a"),q=second.Api("example.stage.b");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK&&c.other.join(c.other.context,tx,&q)==MMS_OK);
        CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_E_MEMORY;})==MMS_E_FAILED);
        CHECK(first.cancels==1&&second.cancels==1&&c.host.poisoned&&!c.store.HasPending()&&!c.host.entries);
        MmsHandle output=99;CHECK(c.api.begin(c.api.context,S("poisoned stage"),c.host.revision,&output)==MMS_E_NOT_READY&&output==99);
    }
    {
        PropertyStageCase c;bool current=true;auto guard=std::make_shared<PropertyRegistrationGuard>(current);
        EditParticipantFixture participant{&c.stateA,0,1};auto p=participant.Api("example.stage.a");const auto tx=c.Begin();
        CHECK(c.api.join(c.api.context,tx,&p)==MMS_OK);
        for(unsigned n=0;n<4096;++n)CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_OK;})==MMS_OK);
        CHECK(c.store.StageProperty(c.a,tx,guard,[](PropertyStageToken&){return MMS_OK;})==MMS_E_BUSY);
        CHECK(participant.cancels==1&&!c.store.HasPending()&&!c.host.entries);
    }
    std::cout<<"property stage exact transaction/guard/callback lifetime PASS\n";
}
static void EditChecks(){
    PropertyStageChecks();
    EditLifecycleChecks();
    StorageStore storage;StorageClient sa,sb;sa.id="example.a";sb.id="example.b";storage.Api(sa);storage.Api(sb);
    SerializerFixture stateA,stateB;auto serialA=stateA.Api(),serialB=stateB.Api();MmsHandle providerA=0,providerB=0;
    CHECK(storage.Register(sa,S("example.a"),&serialA,sizeof(serialA),&providerA)==MMS_OK);
    CHECK(storage.Register(sb,S("example.b"),&serialB,sizeof(serialB),&providerB)==MMS_OK);
    StoredRecords saved{{sa.id,StoredInt(0)},{sb.id,StoredInt(0)}};CHECK(storage.Prepare(saved)==MMS_OK && storage.Commit());
    EditHostFixture host(storage);EditStore store(host,storage);EditClient a,b,unknown;a.id=sa.id;b.id=sb.id;unknown.id="example.unknown";
    auto api=store.Api(a),other=store.Api(b),unregistered=store.Api(unknown);MmsHandle h=919;
    CHECK(api.begin(api.context,S("edit"),0,&h)==MMS_E_ARGUMENT && h==919);
    CHECK(api.begin(api.context,S("edit"),2,&h)==MMS_E_STALE && h==919);
    CHECK(api.begin(api.context,S("edit"),1,&h)==MMS_OK);
    EditParticipantFixture pa{&stateA,0,10},pb{&stateB,0,20};auto p1=pa.Api("example.a"),p2=pb.Api("example.b");
    pa.api=api;pa.transaction=h;
    CHECK(api.join(api.context,h,&p2)==MMS_E_ARGUMENT);
    auto invalid=p1;invalid.pluginId=S("example.unknown");CHECK(unregistered.join(unregistered.context,h,&invalid)==MMS_E_NOT_READY);
    CHECK(api.join(api.context,h,&p1)==MMS_OK);
    CHECK(api.join(api.context,h,&p1)==MMS_E_CONFLICT);
    CHECK(other.join(other.context,h,&p2)==MMS_OK);
    CHECK(other.prepare(other.context,h)==MMS_E_STALE);
    CHECK(api.commit(api.context,h)==MMS_E_CONFLICT && !pa.commits);
    CHECK(api.prepare(api.context,h)==MMS_OK && pa.prepares==1 && pb.prepares==1 && pa.releases==1 && pb.releases==1);
    CHECK(stateA.value==0 && stateB.value==0 && host.entries==0);
    CHECK(api.undo(api.context)==MMS_E_BUSY);
    CHECK(api.prepare(api.context,h)==MMS_E_CONFLICT);
    CHECK(api.join(api.context,h,&p1)==MMS_E_CONFLICT);
    CHECK(api.commit(api.context,h)==MMS_OK && stateA.value==10 && stateB.value==20 && host.entries==1 && host.revision==2);
    api.cancel(api.context,h);CHECK(pa.cancels==0 && pb.cancels==0 && pa.commits==1 && pb.commits==1);
    CHECK(api.commit(api.context,h)==MMS_E_STALE);
    pa.before=123;pa.after=456;CHECK(SavedInt(host.history.front()->before.at(sa.id))==0 && SavedInt(host.history.front()->after.at(sa.id))==10);
    CHECK(api.undo(api.context)==MMS_OK && stateA.value==0 && stateB.value==0);
    CHECK(api.redo(api.context)==MMS_OK && stateA.value==10 && stateB.value==20);
    CHECK(SavedInt(storage.Records().at(sa.id))==0);
    CHECK(storage.Unregister(sb,providerB)==MMS_OK);
    CHECK(api.undo(api.context)==MMS_E_NOT_READY && stateA.value==10 && stateB.value==20 && host.cursor==1);
    CHECK(storage.Register(sb,S("example.b"),&serialB,sizeof(serialB),&providerB)==MMS_OK);
    stateB.value=20;stateB.fail=true;
    CHECK(api.undo(api.context)==MMS_E_NOT_READY && stateA.value==10 && stateB.value==20 && host.cursor==1);
    stateB.fail=false;CHECK(api.undo(api.context)==MMS_OK && stateA.value==0 && stateB.value==0);
    // Each failure consumes the accepted contexts exactly once and never adds
    // history. Failure before getUndoState must not release a nonexistent lease.
    for(unsigned scenario=0;scenario<6;++scenario){
        EditParticipantFixture x{&stateA,0,31},y{&stateB,0,32};auto xp=x.Api("example.a"),yp=y.Api("example.b");
        CHECK(api.begin(api.context,S("failure"),host.revision,&h)==MMS_OK);
        CHECK(api.join(api.context,h,&xp)==MMS_OK && other.join(other.context,h,&yp)==MMS_OK);
        x.failPrepare=scenario==0;y.failRead=scenario==1;x.badBytes=scenario==2;host.failReserve=scenario==3;
        if(scenario<4){CHECK(api.prepare(api.context,h)!=MMS_OK);CHECK(api.commit(api.context,h)==MMS_E_STALE);}
        else{CHECK(api.prepare(api.context,h)==MMS_OK);if(scenario==4)++host.revision;else host.locked=true;
            CHECK(api.commit(api.context,h)==(scenario==4?MMS_E_STALE:MMS_E_BUSY));host.locked=false;}
        host.failReserve=false;api.cancel(api.context,h);
        CHECK(x.cancels==1 && y.cancels==1 && !x.commits && !y.commits && host.entries==1 && stateA.value==0 && stateB.value==0);
        CHECK(scenario==0?(x.releases==0&&y.releases==0):x.releases==1);
    }
    CHECK(api.begin(api.context,S("retire participant"),host.revision,&h)==MMS_OK);
    EditParticipantFixture retiring{&stateB,0,55};auto rp=retiring.Api("example.b");CHECK(other.join(other.context,h,&rp)==MMS_OK);
    CHECK(store.Retire(b) && retiring.cancels==1);CHECK(api.prepare(api.context,h)==MMS_E_STALE);
    EditParticipantFixture replaced{&stateB,0,66};auto replacedApi=replaced.Api("example.b");
    CHECK(api.begin(api.context,S("changed serializer"),host.revision,&h)==MMS_OK && other.join(other.context,h,&replacedApi)==MMS_OK);
    CHECK(api.prepare(api.context,h)==MMS_OK);
    CHECK(storage.Unregister(sb,providerB)==MMS_OK && storage.Register(sb,S("example.b"),&serialB,sizeof(serialB),&providerB)==MMS_OK);
    CHECK(api.commit(api.context,h)==MMS_E_STALE && replaced.cancels==1 && !replaced.commits);
    MmsResult threadResult=0;std::thread worker([&]{threadResult=api.begin(api.context,S("thread"),host.revision,&h);});worker.join();CHECK(threadResult==MMS_E_THREAD);
    a.stopping=true;CHECK(api.begin(api.context,S("stopped"),host.revision,&h)==MMS_E_NOT_READY);
    a.stopping=false;
    EditParticipantFixture retiredPrepared{&stateA,0,91};auto retiredApi=retiredPrepared.Api("example.a");
    CHECK(api.begin(api.context,S("retire prepared"),host.revision,&h)==MMS_OK && api.join(api.context,h,&retiredApi)==MMS_OK && api.prepare(api.context,h)==MMS_OK);
    CHECK(store.Retire(a) && retiredPrepared.cancels==1 && !retiredPrepared.commits && host.entries==1);
    EditParticipantFixture nonconforming{&stateA,0,73},uncommitted{&stateB,0,74};nonconforming.throwCommit=true;
    auto bad=nonconforming.Api("example.a"),pending=uncommitted.Api("example.b");
    CHECK(api.begin(api.context,S("throwing commit"),host.revision,&h)==MMS_OK && api.join(api.context,h,&bad)==MMS_OK && other.join(other.context,h,&pending)==MMS_OK);
    CHECK(api.prepare(api.context,h)==MMS_OK && api.commit(api.context,h)==MMS_E_FAILED);
    CHECK(nonconforming.commits==1 && nonconforming.cancels==0 && uncommitted.commits==0 && uncommitted.cancels==1 && host.entries==1 && !host.locked);
    CHECK(api.begin(api.context,S("poisoned"),host.revision,&h)==MMS_E_NOT_READY && !store.Retire(a));
    std::cout<<"two-phase edit engine PASS (native history not published)\n";
}
