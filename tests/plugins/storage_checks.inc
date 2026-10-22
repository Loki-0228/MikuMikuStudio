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
// These file-level checks use explicit generation pairs, not the production
// suffix parser or incrementer, as their oracle.
static void NumberedVersionStorageChecks(const fs::path& root){
    const auto base=root/L"numbered-pair-storage";fs::create_directories(base);
    const auto version=[](const fs::path& p,const wchar_t* n){return fs::path(p.native()+n);};
    const auto group=[](const fs::path& p){return std::vector<fs::path>{p,mikumikustudio::pmm_paths::Effect(p),PluginSidecar(p)};};
    using Image=std::vector<std::pair<bool,std::string>>;
    const auto image=[&](const fs::path& p){Image result;for(const auto& f:group(p)){
        const bool exists=fs::exists(f);result.emplace_back(exists,exists?ReadFile(f,96*1024*1024):std::string{});
    }return result;};
    const auto absent=[&](const fs::path& p){return image(p)==Image(3,{false,{}});};
    const auto inventory=[](const fs::path& dir){
        std::vector<std::pair<std::wstring,std::string>> result;
        for(const auto& entry:fs::directory_iterator(dir)){
            const auto name=entry.path().filename().wstring();
            if(name.size()>=14 && name.substr(name.size()-14)==L".mms-save.lock")continue;
            result.emplace_back(name,entry.is_directory()?"directory":ReadFile(entry.path(),96*1024*1024));
        }
        std::sort(result.begin(),result.end());return result;
    };
    const auto seed=[&](const fs::path& p,int value){
        PluginProject data;data.projectId=StorageId();data.records["example.a"]=StoredInt(value);
        data.records["missing.numbered-plugin"]={93,{0,1,255,0,128}};
        const auto stage=p.parent_path()/L"seed-stage.pmm";Write(stage,"numbered PMM "+std::to_string(value));
        PublishProjectPair(p,stage,data,true);
        Write(mikumikustudio::pmm_paths::Effect(p),"numbered EMM "+std::to_string(value));
        return ReadPluginProject(p);
    };
    const auto verify=[&](const fs::path& p,const PluginProject& expected){
        const auto actual=ReadPluginProject(p);
        CHECK(actual.projectId==expected.projectId && actual.saveId==expected.saveId && actual.mainHash==FileHash(p));
        CHECK(actual.mainHash==expected.mainHash && actual.records.size()==expected.records.size());
        for(const auto& entry:expected.records){const auto& value=actual.records.at(entry.first);
            CHECK(value.schema==entry.second.schema && value.bytes==entry.second.bytes);}
    };
    // Both switches together must select numeric increments, including carry
    // beyond machine integer range. Missing generations must stay missing.
    for(bool keep:{false,true}){
        const auto dir=base/(keep?L"numeric-precedence":L"numeric-only");fs::create_directories(dir);
        const auto target=dir/L"scene.pmm",stage=dir/L"stage.pmm";
        auto active=seed(target,100);const auto oldActive=image(target);
        const std::vector<std::pair<std::wstring,std::wstring>> moves={
            {L"1",L"2"},{L"2",L"3"},{L"9",L"10"},{L"99",L"100"},
            {L"999999999999999999999999999999",L"1000000000000000000000000000000"}};
        std::vector<Image> old;std::vector<PluginProject> metadata;int value=200;
        for(const auto& move:moves){const auto p=version(target,move.first.c_str());metadata.push_back(seed(p,value++));old.push_back(image(p));}
        const auto orphanEmm=version(target,L"5"),orphanSide=version(target,L"7");
        Write(mikumikustudio::pmm_paths::Effect(orphanEmm),"orphan EMM only");
        Write(PluginSidecar(orphanSide),std::string("opaque orphan plugin bytes\0tail",31));
        const auto oldEmm=image(orphanEmm),oldSide=image(orphanSide);
        Write(version(target,L"40"),"PMM without companions");const auto oldMainOnly=image(version(target,L"40"));
        const std::vector<std::wstring> invalid={L"0",L"01",L"-1",L"+1",L"1x"};
        std::vector<Image> invalidBytes;
        for(const auto& suffix:invalid){const auto p=version(target,suffix.c_str());for(const auto& f:group(p))Write(f,"invalid suffix stays");invalidBytes.push_back(image(p));}
        Write(stage,"new numbered active");auto fresh=active;fresh.records["example.a"]=StoredInt(300);
        PublishProjectPair(target,stage,fresh,false,{},keep,true);
        CHECK(FileHash(target)==FileHash(stage) && fresh.saveId!=active.saveId);verify(target,fresh);
        CHECK(image(target)[1]==oldActive[1]);
        CHECK(image(version(target,L"1"))==oldActive);verify(version(target,L"1"),active);
        for(size_t i=0;i<moves.size();++i){const auto p=version(target,moves[i].second.c_str());CHECK(image(p)==old[i]);verify(p,metadata[i]);}
        CHECK(image(version(target,L"6"))==oldEmm && image(version(target,L"8"))==oldSide);
        CHECK(image(version(target,L"41"))==oldMainOnly && absent(version(target,L"40")));
        for(const auto* missing:{L"4",L"5",L"7",L"9",L"11",L"99",L"999999999999999999999999999999"})CHECK(absent(version(target,missing)));
        for(size_t i=0;i<invalid.size();++i)CHECK(image(version(target,invalid[i].c_str()))==invalidBytes[i]);
        CHECK(!fs::exists(target.native()+L".mms-save.json"));
        const auto finished=inventory(dir);RecoverProjectPair(target);RecoverProjectPair(target);CHECK(inventory(dir)==finished);
    }
    // An absent current PMM is a first save, even if older names exist.
    for(bool createOnly:{false,true}){
        const auto dir=base/(createOnly?L"first-create":L"first-overwrite");fs::create_directories(dir);
        const auto target=dir/L"first.pmm",stage=dir/L"stage.pmm";
        const auto previous=seed(version(target,L"9"),409);const auto old=image(version(target,L"9"));
        Write(mikumikustudio::pmm_paths::Effect(version(target,L"5")),"first orphan");
        const auto orphan=image(version(target,L"5"));PluginProject fresh;fresh.records["example.a"]=StoredInt(400);
        Write(stage,"first current PMM");PublishProjectPair(target,stage,fresh,createOnly,{},true,true);
        verify(target,fresh);verify(version(target,L"9"),previous);
        CHECK(image(version(target,L"9"))==old && image(version(target,L"5"))==orphan);
        CHECK(absent(version(target,L"1")) && absent(version(target,L"10")) && absent(version(target,L"6")));
    }
    // Exceptions at existing durable checkpoints are in-process injection,
    // not an assertion that a process termination has been exercised.
    for(unsigned fail=0;fail<4;++fail){
        const auto dir=base/(L"checkpoint-"+std::to_wstring(fail));fs::create_directories(dir);
        const auto target=dir/L"active.pmm",stage=dir/L"stage.pmm";
        const auto active=seed(target,500),prior=seed(version(target,L"1"),501),distant=seed(version(target,L"9"),509);
        const auto oldActive=image(target),oldPrior=image(version(target,L"1")),oldDistant=image(version(target,L"9"));
        auto fresh=active;fresh.records["example.a"]=StoredInt(599);Write(stage,"recover numbered current");
        const auto before=inventory(dir);bool reached=false;
        CHECK(Throws([&]{PublishProjectPair(target,stage,fresh,false,[&](unsigned n){if(n==fail){reached=true;throw std::runtime_error("numbered checkpoint interruption");}},false,true);}));
        CHECK(reached);
        if(!fail)CHECK(inventory(dir)==before);
        else CHECK(fs::exists(target.native()+L".mms-save.json") && image(version(target,L"1"))==oldActive &&
            image(version(target,L"2"))==oldPrior && image(version(target,L"10"))==oldDistant);
        RecoverProjectPair(target);
        if(!fail){CHECK(inventory(dir)==before);verify(target,active);}
        else{
            verify(target,fresh);verify(version(target,L"1"),active);verify(version(target,L"2"),prior);verify(version(target,L"10"),distant);
            CHECK(FileHash(target)==FileHash(stage) && absent(version(target,L"3")) && absent(version(target,L"9")) && absent(version(target,L"11")));
        }
        CHECK(!fs::exists(target.native()+L".mms-save.json"));
        const auto recovered=inventory(dir);RecoverProjectPair(target);RecoverProjectPair(target);CHECK(inventory(dir)==recovered);
    }
    // Lock the source generation, destination generation or active data;
    // refuse pending version journals and directory-shaped members as well.
    for(unsigned fault=0;fault<14;++fault){
        const auto dir=base/(L"refusal-"+std::to_wstring(fault));fs::create_directories(dir);
        const auto target=dir/L"active.pmm",stage=dir/L"stage.pmm";
        auto active=seed(target,600);seed(version(target,L"1"),601);
        if(fault<4 || fault>=10)seed(version(target,L"9"),609);
        Write(stage,"refused numbered save");active.records["example.a"]=StoredInt(699);
        const auto source=version(target,L"9"),destination=version(target,L"10");
        if(fault==2 || fault==3)Write((fault==2?source:destination).native()+L".mms-save.json","other owner's journal");
        if(fault>=4 && fault<10){const auto p=group(fault<7?source:destination)[(fault-4)%3];fs::create_directory(p);}
        const auto before=inventory(dir);
        struct Held {HANDLE value=INVALID_HANDLE_VALUE;~Held(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} held;
        if(fault<2){const auto path=(fault?destination:source).native()+L".mms-save.lock";
            held.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        }else if(fault>=10){const auto path=fault==10?target:group(source)[fault-11];
            held.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        }
        if(fault<2 || fault>=10)CHECK(held.value!=INVALID_HANDLE_VALUE);
        CHECK(Throws([&]{PublishProjectPair(target,stage,active,false,{},false,true);}));
        CHECK(inventory(dir)==before && !fs::exists(target.native()+L".mms-save.json"));
        if(held.value!=INVALID_HANDLE_VALUE){CHECK(CloseHandle(held.value)!=FALSE);held.value=INVALID_HANDLE_VALUE;}
        RecoverProjectPair(target);CHECK(inventory(dir)==before);
    }
}
static void VersionStorageChecks(const fs::path& root){
    const auto directory=root/L"retained-pair-recovery";fs::create_directories(directory);
    const auto group=[](const fs::path& project){return std::vector<fs::path>{project,mikumikustudio::pmm_paths::Effect(project),PluginSidecar(project)};};
    const auto bytes=[&](const fs::path& project){std::vector<std::string> result;for(const auto& file:group(project))result.push_back(ReadFile(file,96*1024*1024));return result;};
    const auto absent=[&](const fs::path& project){for(const auto& file:group(project))if(fs::exists(file))return false;return true;};
    for(unsigned fail=0;fail<4;++fail){
        const auto target=directory/(L"checkpoint-"+std::to_wstring(fail)+L".pmm"),stage=directory/L"stage.pmm";
        const auto prior=fs::path(target.native()+L"1"),older=fs::path(target.native()+L"11");
        PluginProject project;project.projectId=StorageId();project.records["example.a"]=StoredInt(901);
        Write(stage,"oldest retained PMM");PublishProjectPair(target,stage,project,true);
        Write(mikumikustudio::pmm_paths::Effect(target),"oldest EMM");
        const auto oldest=bytes(target);const auto oldestSave=ReadPluginProject(target).saveId;
        Write(stage,"middle retained PMM");project.records["example.a"]=StoredInt(902);
        PublishProjectPair(target,stage,project,false,{},true);
        Write(mikumikustudio::pmm_paths::Effect(target),"middle EMM");
        const auto middle=bytes(target);const auto middleSave=ReadPluginProject(target).saveId;
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
    NumberedVersionStorageChecks(root);
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
    CHECK(Throws([&]{PublishProjectPair(target,stage,project,false,[&](unsigned n){if(n==2){
        Write(fs::path(target.native()+L".mms-"+Wide(project.saveId)+L".new-json"),"corrupt staged sidecar");throw std::runtime_error("interruption");}});}));
    RecoverProjectPair(target);CHECK(FileHash(target)==rollbackHash && ReadPluginProject(target).saveId==rollbackSave);
    const auto sideBefore=ReadFile(PluginSidecar(target),96*1024*1024);Write(target,"external replacement");
    CHECK(Throws([&]{ReadPluginProject(target);}));CHECK(Throws([&]{PublishProjectPair(target,stage,project,false);}));CHECK(ReadFile(PluginSidecar(target),96*1024*1024)==sideBefore);
    VersionStorageChecks(root);
    std::cout<<"storage and paired-save recovery PASS\n";
}
