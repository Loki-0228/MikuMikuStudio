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
    std::function<void()> onCancel,onCommit;
    MmsEditApiV1 api{};MmsHandle transaction=0;
    MmsTransactionParticipantV1 Api(const char* id){
        MmsTransactionParticipantV1 p{Header<MmsTransactionParticipantV1>(),this,S(id)};
        p.prepare=[](void* context)->MmsResult{auto& f=*static_cast<EditParticipantFixture*>(context);++f.prepares;
            if(f.api.prepare)CHECK(f.api.prepare(f.api.context,f.transaction)==MMS_E_BUSY);
            return f.failPrepare?MMS_E_NOT_READY:MMS_OK;};
        p.commit=[](void* context){auto& f=*static_cast<EditParticipantFixture*>(context);++f.commits;f.state->value=f.after;if(f.onCommit)f.onCommit();if(f.throwCommit)throw std::runtime_error("nonconforming plugin");};
        p.cancel=[](void* context){auto& f=*static_cast<EditParticipantFixture*>(context);++f.cancels;if(f.onCancel)f.onCancel();if(f.throwCancel)throw std::runtime_error("nonconforming cancel");};
        p.getUndoState=[](void* context,MmsBytes* before,MmsBytes* after,uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& f=*static_cast<EditParticipantFixture*>(context);++f.reads;if(f.failRead)return MMS_E_FAILED;
            *before={&f.before,sizeof(int)};*after={f.badBytes?nullptr:&f.after,sizeof(int)};*schema=7;*lease=77;return MMS_OK;};
        p.releaseUndoState=[](void* context,MmsHandle h){auto& f=*static_cast<EditParticipantFixture*>(context);CHECK(h==77);++f.releases;};return p;
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
static void EditChecks(){
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
