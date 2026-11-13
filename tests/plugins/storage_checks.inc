#include "mikumikustudio/pmm_paths.hpp"
#include <algorithm>
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
struct RetainedPairSeed {
    PluginProject project;
    std::string oldestSave, middleSave;
    std::vector<std::string> oldest, middle;
};
static std::vector<fs::path> VersionMembers(const fs::path& project) {
    return {project, mikumikustudio::pmm_paths::Effect(project), PluginSidecar(project)};
}
static std::vector<std::string> VersionBytes(const fs::path& project) {
    std::vector<std::string> result;
    for(const auto& path:VersionMembers(project)) result.push_back(ReadFile(path,96*1024*1024));
    return result;
}
static RetainedPairSeed SeedRetainedPair(const fs::path& target,const fs::path& stage) {
    RetainedPairSeed seed;
    auto& project=seed.project;
    project.projectId=StorageId();project.records["example.a"]=StoredInt(901);
    Write(stage,"oldest retained PMM");PublishProjectPair(target,stage,project,true);
    Write(mikumikustudio::pmm_paths::Effect(target),"oldest EMM");
    seed.oldest=VersionBytes(target);seed.oldestSave=ReadPluginProject(target).saveId;
    Write(stage,"middle retained PMM");project.records["example.a"]=StoredInt(902);
    PublishProjectPair(target,stage,project,false,{},true);
    Write(mikumikustudio::pmm_paths::Effect(target),"middle EMM");
    seed.middle=VersionBytes(target);seed.middleSave=ReadPluginProject(target).saveId;
    return seed;
}
static void VersionStorageChecks(const fs::path& root){
    const auto directory=root/L"retained-pair-recovery";fs::create_directories(directory);
    const auto group=VersionMembers;
    const auto bytes=VersionBytes;
    const auto absent=[&](const fs::path& project){for(const auto& file:group(project))if(fs::exists(file))return false;return true;};
    for(unsigned fail=0;fail<4;++fail){
        const auto target=directory/(L"checkpoint-"+std::to_wstring(fail)+L".pmm"),stage=directory/L"stage.pmm";
        const auto prior=fs::path(target.native()+L"1"),older=fs::path(target.native()+L"11");
        auto seed=SeedRetainedPair(target,stage);
        auto& project=seed.project;
        const auto& oldest=seed.oldest;const auto& middle=seed.middle;
        const auto& oldestSave=seed.oldestSave;const auto& middleSave=seed.middleSave;
        CHECK(bytes(prior)==oldest && absent(older));
        Write(stage,"new interrupted PMM");project.records["example.a"]=StoredInt(903);
        bool interrupted=false;
        CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,[&](unsigned n){if(n==fail){
            interrupted=true;throw std::runtime_error("retained save interruption");}},true);}));
        CHECK(interrupted);
        if(!fail){
            CHECK(bytes(target)==middle && bytes(prior)==oldest && absent(older));
            CHECK(!fs::exists(target.native()+L".mms-save.json"));
        }else{
            CHECK(fs::exists(target.native()+L".mms-save.json"));
            CHECK(bytes(prior)==middle && bytes(older)==oldest);
        }
        RecoverProjectPair(target);const auto loaded=ReadPluginProject(target);
        CHECK(!fs::exists(target.native()+L".mms-save.json"));
        if(!fail){
            CHECK(bytes(target)==middle && bytes(prior)==oldest && loaded.saveId==middleSave && absent(older));
        }else{
            CHECK(FileHash(target)==FileHash(stage) && SavedInt(loaded.records.at("example.a"))==903);
            CHECK(bytes(prior)==middle && bytes(older)==oldest && absent(target.native()+L"111"));
            const auto p=ReadPluginProject(prior),o=ReadPluginProject(older);
            CHECK(p.saveId==middleSave && o.saveId==oldestSave && p.projectId==loaded.projectId && o.projectId==loaded.projectId);
            CHECK(p.mainHash==FileHash(prior) && o.mainHash==FileHash(older));
        }
    }
    const auto target=directory/L"active.pmm",stage=directory/L"locked-stage.pmm";
    const auto prior=fs::path(target.native()+L"1"),older=fs::path(target.native()+L"11");
    PluginProject project;project.projectId=StorageId();project.records["example.a"]=StoredInt(910);
    Write(stage,"first lock fixture");PublishProjectPair(target,stage,project,true);
    Write(mikumikustudio::pmm_paths::Effect(target),"first lock EMM");
    Write(stage,"second lock fixture");PublishProjectPair(target,stage,project,false,{},true);
    Write(mikumikustudio::pmm_paths::Effect(target),"second lock EMM");
    const auto activeBytes=bytes(target),priorBytes=bytes(prior);
    const auto lockPath=prior.native()+L".mms-save.lock";
    HANDLE held=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    CHECK(held!=INVALID_HANDLE_VALUE);
    Write(stage,"must wait for other version owner");
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,{},true);}));
    CHECK(bytes(target)==activeBytes && bytes(prior)==priorBytes && absent(older));
    CHECK(!fs::exists(target.native()+L".mms-save.json"));
    CHECK(CloseHandle(held)!=FALSE && !fs::exists(lockPath));
    // A prior version with its own interrupted publication must also be left
    // untouched; its journal belongs to that version's recovery path.
    const auto recovery=prior.native()+L".mms-save.json";
    Write(recovery,"version owner's recovery evidence");
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,{},true);}));
    CHECK(bytes(target)==activeBytes && bytes(prior)==priorBytes && absent(older) && ReadFile(recovery)=="version owner's recovery evidence");
    CHECK(DeleteFileW(recovery.c_str())!=FALSE);
    PublishProjectPair(target,stage,project,false,{},true);
    CHECK(bytes(prior)==activeBytes && bytes(older)==priorBytes && FileHash(target)==FileHash(stage));
    CHECK(ReadPluginProject(prior).mainHash==FileHash(prior) && ReadPluginProject(older).mainHash==FileHash(older));
}
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
    // An ordinary, reported IO failure is different from the durable crash
    // checkpoints above. If the first replacement never happened, opening the
    // old pair later must not commit the save which the caller saw fail.
    const auto rejectedHash=FileHash(target),rejectedSide=ReadFile(PluginSidecar(target),96*1024*1024);
    const auto rejectedSave=ReadPluginProject(target).saveId;
    Write(stage,"rejected replacement must stay rejected");project.records["example.a"]=StoredInt(701);
    HANDLE locked=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(locked!=INVALID_HANDLE_VALUE);
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,false);}));
    CHECK(FileHash(target)==rejectedHash && ReadFile(PluginSidecar(target),96*1024*1024)==rejectedSide);
    CHECK(!fs::exists(target.native()+L".mms-save.json"));
    const auto rejectedBase=target.native()+L".mms-"+Wide(project.saveId);
    for(const auto* suffix:{L".new-pmm",L".new-json",L".old-pmm",L".old-json",L".journal"})CHECK(!fs::exists(rejectedBase+suffix));
    CHECK(CloseHandle(locked)!=FALSE);
    RecoverProjectPair(target);loaded=ReadPluginProject(target);
    CHECK(FileHash(target)==rejectedHash && loaded.saveId==rejectedSave && SavedInt(loaded.records.at("example.a"))!=701);
    CHECK(ReadFile(stage)=="rejected replacement must stay rejected");

    // A legacy PMM without a plugin sidecar is a valid previous state too.
    const auto legacy=dir/L"legacy-without-sidecar.pmm";
    Write(legacy,"legacy old PMM");const auto legacyHash=FileHash(legacy);
    locked=CreateFileW(legacy.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(locked!=INVALID_HANDLE_VALUE);
    CHECK(Throws([&]{PublishProjectPair(legacy,stage,project,false);}));
    CHECK(!fs::exists(legacy.native()+L".mms-save.json") && !fs::exists(PluginSidecar(legacy)));
    CHECK(CloseHandle(locked)!=FALSE);RecoverProjectPair(legacy);
    CHECK(FileHash(legacy)==legacyHash && ReadPluginProject(legacy).saveId.empty() && !fs::exists(PluginSidecar(legacy)));

    // No previous files: remove only the private staged copy immediately before
    // the first Move, producing a real FILE_NOT_FOUND error after journaling.
    const auto absent=dir/L"first-save-rejected.pmm";
    CHECK(Throws([&]{PublishProjectPair(absent,stage,project,true,[&](unsigned n){if(n==1)
        CHECK(DeleteFileW((absent.native()+L".mms-"+Wide(project.saveId)+L".new-pmm").c_str())!=FALSE);});}));
    CHECK(!fs::exists(absent) && !fs::exists(PluginSidecar(absent)) && !fs::exists(absent.native()+L".mms-save.json"));
    RecoverProjectPair(absent);
    CHECK(!fs::exists(absent) && !fs::exists(PluginSidecar(absent)));

    // If an external writer changed a destination, the failed attempt must
    // retain its evidence. Recovery must refuse, rather than erase that file.
    const auto uncertain=dir/L"uncertain-first-move.pmm";
    PublishProjectPair(uncertain,stage,project,true);
    Write(stage,"new uncertain snapshot");
    bool removedUncertainSource=false;
    CHECK(Throws([&]{PublishProjectPair(uncertain,stage,project,false,[&](unsigned n){if(n==1){
        Write(uncertain,"unrelated external bytes");
        removedUncertainSource=DeleteFileW((uncertain.native()+L".mms-"+Wide(project.saveId)+L".new-pmm").c_str())!=FALSE;
    }});}));
    CHECK(removedUncertainSource && fs::exists(uncertain.native()+L".mms-save.json"));
    CHECK(Throws([&]{RecoverProjectPair(uncertain);}) && ReadFile(uncertain)=="unrelated external bytes");
    const auto rollbackHash=FileHash(target),rollbackSave=ReadPluginProject(target).saveId;
    Write(stage,"must roll back");
    fs::path changedStage;
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,[&](unsigned n){if(n==2){
        changedStage=target.native()+L".mms-"+Wide(project.saveId)+L".new-json";
        Write(changedStage,"corrupt staged sidecar");throw std::runtime_error("interruption");}});}));
    const fs::path rollbackJournal=target.native()+L".mms-save.json";
    CHECK(!changedStage.empty() && fs::exists(rollbackJournal));
    CHECK(Throws([&]{RecoverProjectPair(target);}));
    CHECK(FileHash(target)==rollbackHash && ReadFile(changedStage)=="corrupt staged sidecar" && fs::exists(rollbackJournal));
    CHECK(DeleteFileW(changedStage.c_str())!=FALSE);
    RecoverProjectPair(target);CHECK(FileHash(target)==rollbackHash && ReadPluginProject(target).saveId==rollbackSave);
    CHECK(!fs::exists(rollbackJournal));
    const auto sideBefore=ReadFile(PluginSidecar(target),96*1024*1024);Write(target,"external replacement");
    CHECK(Throws([&]{ReadPluginProject(target);}));CHECK(Throws([&]{PublishProjectPair(target,stage,project,false);}));CHECK(ReadFile(PluginSidecar(target),96*1024*1024)==sideBefore);
    VersionStorageChecks(root);
    std::cout<<"storage and paired-save recovery PASS\n";
}
