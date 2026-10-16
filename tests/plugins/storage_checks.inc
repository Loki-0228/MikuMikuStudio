struct SerializerFixture {
    int value=0, candidate=0, prepares=0, commits=0, cancels=0, saves=0, releases=0;
    bool fail=false, badBytes=false;
    StorageStore* store=nullptr;
    MmsSerializerV1 Api(){
        MmsSerializerV1 s{Header<MmsSerializerV1>(),this};
        s.prepareLoad=[](void* p,MmsBytes b,uint32_t schema,MmsHandle* out)->MmsResult{
            auto& f=*static_cast<SerializerFixture*>(p);++f.prepares;
            if(f.fail)return MMS_E_VERSION;
            if(schema && (schema!=7 || b.bytes!=sizeof(int) || !b.data))return MMS_E_ARGUMENT;
            f.candidate=0;if(schema)std::memcpy(&f.candidate,b.data,sizeof(int));
            if(f.store){StoredRecords reentrant;CHECK(f.store->Capture(reentrant)==MMS_E_BUSY);}
            *out=42;return MMS_OK;
        };
        s.commitLoad=[](void* p,MmsHandle h){auto& f=*static_cast<SerializerFixture*>(p);CHECK(h==42);f.value=f.candidate;++f.commits;};
        s.cancelLoad=[](void* p,MmsHandle h){auto& f=*static_cast<SerializerFixture*>(p);CHECK(h==42);++f.cancels;};
        s.save=[](void* p,MmsBytes* bytes,uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& f=*static_cast<SerializerFixture*>(p);++f.saves;if(f.fail)return MMS_E_FAILED;
            *bytes={f.badBytes?nullptr:&f.value,sizeof(int)};*schema=7;*lease=11;return MMS_OK;
        };
        s.releaseBytes=[](void* p,MmsHandle h){auto& f=*static_cast<SerializerFixture*>(p);CHECK(h==11);++f.releases;};return s;
    }
};
static StoredData StoredInt(int value){StoredData d;d.schema=7;d.bytes.resize(sizeof(value));std::memcpy(d.bytes.data(),&value,sizeof(value));return d;}
static int SavedInt(const StoredData& v){int value=-1;CHECK(v.bytes.size()==sizeof(value));std::memcpy(&value,v.bytes.data(),sizeof(value));return value;}
static void StorageChecks(const fs::path& root){
    StorageStore store;StorageClient a,b,other;a.id="example.a";b.id="example.b";other.id="example.other";
    auto api=store.Api(a),apiOther=store.Api(other);SerializerFixture first,second;first.store=&store;second.store=&store;
    auto s1=first.Api(),s2=second.Api();MmsHandle p1=0,p2=0,ignored=919;
    CHECK(store.Register(a,S("example.b"),&s1,sizeof(s1),&ignored)==MMS_E_ARGUMENT && ignored==919);
    CHECK(store.Register(a,S("example.a"),&s1,sizeof(s1),&p1)==MMS_OK);
    CHECK(first.commits==1 && first.value==0);
    CHECK(store.Register(a,S("example.a"),&s1,sizeof(s1),&ignored)==MMS_E_CONFLICT);
    CHECK(store.Register(b,S("example.b"),&s2,sizeof(s2),&p2)==MMS_OK);
    StoredRecords records{{"example.a",StoredInt(10)},{"example.b",StoredInt(20)},{"missing.plugin",{93,{0,1,255,0,128}}}};
    CHECK(store.Prepare(records)==MMS_OK && first.value==0 && second.value==0);
    CHECK(store.Unregister(a,p1)==MMS_E_BUSY && !store.Retire(a));
    CHECK(store.Commit() && first.value==10 && second.value==20);
    MmsBytes bytes{};uint32_t schema=0;MmsHandle lease=0;
    CHECK(api.readSaved(api.context,&bytes,&schema,&lease)==MMS_OK && schema==7);
    int retained=0;std::memcpy(&retained,bytes.data,sizeof(retained));CHECK(retained==10);
    apiOther.releaseSaved(apiOther.context,lease);CHECK(store.HasLeases(a));
    auto failed=records;failed["example.a"]=StoredInt(99);second.fail=true;
    CHECK(store.Prepare(failed)==MMS_E_VERSION && first.value==10 && second.value==20 && first.cancels==1);
    CHECK(!store.Commit());second.fail=false;
    CHECK(store.Prepare(failed)==MMS_OK);store.Cancel();store.Cancel();CHECK(first.value==10 && first.cancels==2);
    CHECK(store.Prepare(failed)==MMS_OK && store.Commit() && first.value==99);
    std::memcpy(&retained,bytes.data,sizeof(retained));CHECK(retained==10);
    api.releaseSaved(api.context,lease);api.releaseSaved(api.context,lease);CHECK(!store.HasLeases(a));
    first.value=12;second.value=34;StoredRecords captured;
    CHECK(store.Capture(captured)==MMS_OK && SavedInt(captured.at("example.a"))==12 && SavedInt(captured.at("example.b"))==34);
    CHECK(captured.at("missing.plugin").bytes==records.at("missing.plugin").bytes && captured.at("missing.plugin").schema==93);
    first.badBytes=true;auto unchanged=captured;
    const int releaseCount=first.releases;
    CHECK(store.Capture(captured)==MMS_E_ARGUMENT && first.releases==releaseCount+1 && SavedInt(captured.at("example.a"))==12);first.badBytes=false;
    second.fail=true;CHECK(store.Capture(captured)==MMS_E_FAILED && SavedInt(captured.at("example.a"))==12);second.fail=false;
    MmsResult threaded=0;std::thread worker([&]{threaded=store.Capture(captured);});worker.join();CHECK(threaded==MMS_E_THREAD);
    CHECK(store.Unregister(other,p1)==MMS_E_NOT_FOUND);
    CHECK(store.Unregister(a,p1)==MMS_OK);CHECK(store.Register(a,S("example.a"),&s1,sizeof(s1),&p1)==MMS_OK && first.value==99);
    a.stopping=true;CHECK(store.Capture(captured)==MMS_E_NOT_READY);CHECK(store.Retire(a));CHECK(store.Retire(b));
    const auto dir=root/L"工程数据";fs::create_directories(dir);const auto target=dir/L"测试.pmm",stage=dir/L"stage.pmm";
    PluginProject project;project.projectId=StorageId();project.records=records;
    Write(stage,"first PMM bytes");PublishProjectPair(target,stage,project,true);
    auto loaded=ReadPluginProject(target);CHECK(loaded.projectId==project.projectId && loaded.records.at("missing.plugin").bytes==records.at("missing.plugin").bytes);
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,true);}));
    auto large=project;large.records["large.plugin"]={1,std::vector<unsigned char>(1024*1024+123,255)};
    CHECK(DecodeProject(EncodeProject(large)).records.at("large.plugin").bytes==large.records.at("large.plugin").bytes);
    auto malformed=EncodeProject(project);const auto at=malformed.find("AAH/");CHECK(at!=std::string::npos);malformed[at]='=';
    CHECK(Throws([&]{DecodeProject(malformed);}));
    // The write interruption occurs after real durable file operations. A new
    // recovery call has no in-memory state from the publisher.
    for(unsigned fail=0;fail<4;++fail){
        const auto oldHash=FileHash(target);const auto oldSave=ReadPluginProject(target).saveId;
        Write(stage,"new PMM "+std::to_string(fail));project.records["example.a"]=StoredInt(100+int(fail));
        CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,[&](unsigned n){if(n==fail)throw std::runtime_error("power interruption");});}));
        RecoverProjectPair(target);loaded=ReadPluginProject(target);
        CHECK(!fs::exists(target.native()+L".mms-save.json"));
        if(!fail)CHECK(FileHash(target)==oldHash && loaded.saveId==oldSave);
        else CHECK(FileHash(target)==FileHash(stage) && SavedInt(loaded.records.at("example.a"))==100+int(fail));
    }
    const auto rollbackHash=FileHash(target),rollbackSave=ReadPluginProject(target).saveId;
    Write(stage,"must roll back");
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,[&](unsigned n){if(n==2){
        Write(fs::path(target.native()+L".mms-"+Wide(project.saveId)+L".new-json"),"corrupt staged sidecar");throw std::runtime_error("interruption");}});}));
    RecoverProjectPair(target);CHECK(FileHash(target)==rollbackHash && ReadPluginProject(target).saveId==rollbackSave);
    const auto sideBefore=ReadFile(PluginSidecar(target),96*1024*1024);Write(target,"external replacement");
    CHECK(Throws([&]{ReadPluginProject(target);}));CHECK(Throws([&]{PublishProjectPair(target,stage,project,false);}));CHECK(ReadFile(PluginSidecar(target),96*1024*1024)==sideBefore);
    std::cout<<"storage and paired-save recovery PASS\n";
}
