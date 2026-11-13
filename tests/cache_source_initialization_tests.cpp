// MAINTENANCE: regression-keep. Actual first source store publication and interrupted initialization.
// Exercises actual persisted approval, policy CAS, file inventory and catalog recovery.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <MinHook.h>
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/cache_policy.hpp"
#include "../src/io/cache_source_admission.hpp"
#include "../src/io/cache_source_journal.hpp"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace m=mikumikustudio::cache_manager;
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
namespace c=mikumikustudio::cache_catalog;
using A=m::SourceAdmission;using R=m::SourceAdmissionResult;using Bytes=b::Bytes;
constexpr std::uint64_t MiB=1024ull*1024,Limit=64*MiB;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"DIAG check=%u win32=%lu\n",checks,GetLastError());throw std::runtime_error(why);}}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;};
template<class T>T Id(unsigned n){T value{};value[14]=std::uint8_t(n>>8);value[15]=std::uint8_t(n);return value;}
void Fresh(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"absolute normalized fixture directory");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim new private test root");}
Bytes Read(const fs::path& p){std::ifstream in(p,std::ios::binary);Check(bool(in),"read real owned evidence");return Bytes(std::istreambuf_iterator<char>(in),{});}
void Small(const fs::path& p,const Bytes& bytes){Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;Check(h.value!=INVALID_HANDLE_VALUE&&WriteFile(h.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(h.value),"create exact owned evidence once");}
void Sparse(const fs::path& p,std::uint64_t bytes){Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;Check(h.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(h.value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&done,nullptr),"create bounded-physical sparse quota filler");LARGE_INTEGER size{};size.QuadPart=LONGLONG(bytes);Check(SetFilePointerEx(h.value,size,nullptr,FILE_BEGIN)&&SetEndOfFile(h.value)&&FlushFileBuffers(h.value),"set exact logical filler charge");}
void Select(const fs::path& root){Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",root.c_str())&&SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(root.parent_path()/L"settings.json").c_str()),"isolate manager root and common policy");}
void Policy(const fs::path& root,std::uint64_t limit=Limit,bool enabled=true){Select(root);m::Settings policy;policy.limitBytes=limit;policy.enabled=enabled;policy.automaticCleanup=false;std::wstring error;
    const bool configured=m::Configure(policy,error);const DWORD win32=GetLastError();
    if(!configured)std::fwprintf(stderr,L"DIAG Policy root=%ls limit=%llu enabled=%u error=%ls win32=%lu\n",root.c_str(),static_cast<unsigned long long>(limit),enabled?1u:0u,error.c_str(),win32);
    SetLastError(win32);Check(configured,"persist actual enabled/disabled shared policy");}
std::uint64_t Unit(const fs::path& root){wchar_t mount[32768]{};DWORD sectors=0,bytes=0,free=0,total=0;Check(GetVolumePathNameW(root.c_str(),mount,32768)&&GetDiskFreeSpaceW(mount,&sectors,&bytes,&free,&total)&&sectors&&bytes,"query real allocation granularity");return std::uint64_t(sectors)*bytes;}
std::uint64_t Round(std::uint64_t bytes,std::uint64_t unit){return ((bytes+unit-1)/unit)*unit;}
fs::path Entry(const fs::path& root,unsigned id){wchar_t name[65]{};swprintf_s(name,L"%064x",id);return root/L"Textures"/L"shared"/(std::wstring(name)+L".mtc");}
bool Same(const c::Snapshot&a,const c::Snapshot&d){return a.IsValid()==d.IsValid()&&a.Revision()==d.Revision()&&a.StoreId()==d.StoreId()&&a.Bytes()==d.Bytes()&&a.IsCheckpoint()==d.IsCheckpoint();}
using Evidence=std::map<std::wstring,Bytes>;
Evidence CatalogFiles(const fs::path& root){Evidence files;for(const auto& e:fs::recursive_directory_iterator(root))if(e.is_regular_file())files.emplace(e.path().lexically_relative(root).native(),Read(e.path()));return files;}
b::SourceOperation Reserved(unsigned n,std::uint64_t bytes){b::SourceOperation op;op.id=Id<b::OperationId>(n);op.sourceId=Id<b::SourceId>(n+100);op.directoryId=1;op.reservedBytes=bytes;return op;}
std::unique_ptr<A> Open(){std::unique_ptr<A> out;Check(A::Open(out)==R::Ok&&out&&!out->IsConsumed(),"open trusted existing Video admission session");return out;}

struct Fault{std::wstring prefix;bool partial=false,retainFailed=false;unsigned hits=0,cleanupHits=0;bool Match(HANDLE h)const noexcept{wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);return n>prefix.size()&&n<32768&&!_wcsnicmp(path,prefix.c_str(),prefix.size());}};
thread_local Fault* fault=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&WriteFile) realWrite=nullptr;decltype(&SetFileInformationByHandle) realSet=nullptr;
BOOL WINAPI FlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();if(result&&fault&&!fault->partial&&!fault->hits&&fault->Match(h)){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{LARGE_INTEGER zero{},position{};if(fault&&fault->partial&&!fault->hits&&!overlap&&count>1&&fault->Match(h)&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto result=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(result){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return result;}return realWrite(h,bytes,count,done,overlap);}
BOOL WINAPI SetHook(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID value,DWORD bytes)noexcept{
    if(fault&&fault->retainFailed&&fault->hits&&kind==FileDispositionInfo&&bytes>=sizeof(FILE_DISPOSITION_INFO)&&static_cast<FILE_DISPOSITION_INFO*>(value)->DeleteFile&&fault->Match(handle)){
        ++fault->cleanupHits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realSet(handle,kind,value,bytes);
}
struct LatePolicy {fs::path video,root,settings;bool hit=false,changed=false;bool disable=true,reenter=false,reentryChecked=false;R reentryResult=R::Ok;bool writerAccepted=true;c::Snapshot sentinel;};
thread_local LatePolicy* latePolicy=nullptr;decltype(&CreateDirectoryW) realDirectory=nullptr;
BOOL WINAPI DirectoryHook(LPCWSTR path,LPSECURITY_ATTRIBUTES security)noexcept{const auto result=realDirectory(path,security);const auto error=GetLastError();
    if(result&&latePolicy&&!latePolicy->hit&&path&&!_wcsicmp(path,latePolicy->video.c_str())){auto& change=*latePolicy;change.hit=true;try{namespace p=mikumikustudio::cache_policy;p::Store store(change.root,change.settings);p::Snapshot before,after;
        if(change.reenter){const auto files=CatalogFiles(change.video);auto output=change.sentinel;change.reentryResult=A::Initialize(output);m::WriteReservation nested(Entry(change.root,87),MiB);change.writerAccepted=bool(nested);
            change.reentryChecked=output.SameVersion(change.sentinel)&&CatalogFiles(change.video)==files;}
        if(change.disable&&store.Read(before)==p::Result::Ok){auto value=before.Value();value.enabled=false;change.changed=store.Commit(before,value,after)==p::Result::Ok;}}catch(...){} }
    SetLastError(error);return result;
}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&SetHook),reinterpret_cast<void**>(&realSet))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&CreateDirectoryW),reinterpret_cast<void*>(&DirectoryHook),reinterpret_cast<void**>(&realDirectory))==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"inject only exact real catalog file failure");}~Hooks(){fault=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
std::wstring Prefix(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t text[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,text,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"bind fault to actual fixture commit directory");return std::wstring(text,n)+L'\\';}
using Recovery=m::SourceRecoveryAdmission;
std::unique_ptr<Recovery> RecoverOpen(){std::unique_ptr<Recovery> session;Check(Recovery::Open(session)==R::Ok&&session&&!session->IsConsumed(),"open explicit source recovery with real manager and control exclusion");return session;}
struct Fixture {fs::path base,root;explicit Fixture(const fs::path& path):base(path),root(base/L"Cache"){Fresh(base);Policy(root);}fs::path Catalog()const{return root/L"Video"/L"control"/L"catalog";}c::Store Store()const{return c::Store(Catalog());}};
void Persist(const Fixture& fixture,bool enabled){namespace p=mikumikustudio::cache_policy;p::Store store(fixture.root,fixture.base/L"settings.json");p::Snapshot before,after;Check(store.Read(before)==p::Result::Ok,"read actual initialization policy");auto value=before.Value();value.enabled=enabled;value.limitMiB=64;Check(store.Commit(before,value,after)==p::Result::Ok,"persist policy without reentering source admission");}
void Scaffold(Fixture& fixture){for(const auto* part:{L"Video",L"Video/control",L"Video/pages",L"Video/raw",L"Video/control/catalog",L"Video/control/catalog/snapshots",L"Video/control/catalog/commits",L"Video/control/catalog/operations"})Check(CreateDirectoryW((fixture.root/part).c_str(),nullptr)!=FALSE,"create only empty expected fixture scaffold");}
void VerifyInitial(Fixture& fixture,const c::Snapshot& expected){auto session=Open();const auto& facts=session->Current();const auto& description=facts.Business().Description();const auto& sources=facts.Business().Sources();
    Check(facts.Business().Source().SameVersion(expected)&&expected.Revision()==1&&!expected.IsCheckpoint()&&sources.maintenanceProtocol==2&&sources.storeOwner==description.storeOwner&&description.maintenanceEpoch==1&&sources.maintenanceEpoch==1&&description.nextProjectId==1&&description.projectCount==0&&description.pages.empty()&&sources.operations.empty()&&sources.sources.empty(),"first admitted store is actual empty protocol2 business root with no fabricated project or source");
    Check(sources.directories.size()==1&&sources.directories[0].id==1,"empty store contains its single real raw directory binding");f::Directory raw;f::DirectoryIdentity identity;Check(f::Directory::Open(fixture.root/L"Video"/L"raw",false,raw)==f::Result::Ok&&raw.GetIdentity(identity)==f::Result::Ok&&identity.volume==sources.directories[0].identity.volume&&identity.fileIndex==sources.directories[0].identity.fileIndex,"initial raw binding comes from actual retained directory identity");
    const auto& credit=facts.Credit();Check(credit.rawReservedBytes==0&&credit.checkpointPublications==1&&credit.metadataBytes==credit.checkpointBytesPerStep&&credit.fundedMetadataBytes==credit.metadataBytes,"initial approval funds complete standby without fabricating source debt");
}
c::Snapshot Success(const fs::path& root){Fixture fixture(root/L"fresh");c::Snapshot head;Check(!fs::exists(fixture.root/L"Video")&&A::Initialize(head)==R::Ok&&head.IsValid(),"first source initialization creates and publishes trusted storage through common quota");VerifyInitial(fixture,head);
    const auto files=CatalogFiles(fixture.Catalog());auto output=head;Check(A::Initialize(output)==R::Conflict&&output.SameVersion(head)&&CatalogFiles(fixture.Catalog())==files,"duplicate initialization preserves existing store, output and metadata");
    auto session=Open();auto sources=session->Current().Business().Sources();sources.operations.push_back(Reserved(1,MiB));c::Snapshot committed;Check(session->Commit(head,sources,committed)==R::Ok,"new empty source layout participates in ordinary admitted publication");session.reset();session=Open();c::Snapshot checkpoint;Check(session->Checkpoint(committed,checkpoint)==R::Ok&&checkpoint.IsCheckpoint(),"new source layout supports approved maintenance without a project page");return head;
}
void Preflight(const fs::path& root,const c::Snapshot& sentinel){
    {Fixture fixture(root/L"disabled");Persist(fixture,false);auto output=sentinel;Check(A::Initialize(output)==R::Limit&&output.SameVersion(sentinel)&&!fs::exists(fixture.root/L"Video"),"disabled policy refuses first allocation before creating Video scaffold");}
    {Fixture fixture(root/L"full");Sparse(fixture.root/L"unknown.bin",Limit);auto output=sentinel;Check(A::Initialize(output)==R::Limit&&output.SameVersion(sentinel)&&!fs::exists(fixture.root/L"Video")&&fs::file_size(fixture.root/L"unknown.bin")==Limit,"full physical ledger refuses initialization before first Video directory and retains foreign bytes");}
    for(bool nested:{false,true}){Fixture fixture(root/(nested?L"unknown-nested":L"unknown-root"));Check(CreateDirectoryW((fixture.root/L"Video").c_str(),nullptr)!=FALSE,"create owned incomplete Video fixture");const auto path=nested?fixture.root/L"Video"/L"control":fixture.root/L"Video";
        if(nested)Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"create incomplete control fixture");Small(path/L"foreign.bin",{1,3,7});const auto before=CatalogFiles(fixture.root/L"Video");auto output=sentinel;
        Check(A::Initialize(output)!=R::Ok&&output.SameVersion(sentinel)&&CatalogFiles(fixture.root/L"Video")==before,"unknown nonempty partial scaffold is refused without adoption or deletion");}
    {Fixture fixture(root/L"empty-scaffold");Scaffold(fixture);f::Directory raw;f::DirectoryIdentity identity;Check(f::Directory::Open(fixture.root/L"Video"/L"raw",false,raw)==f::Result::Ok&&raw.GetIdentity(identity)==f::Result::Ok,"pin actual existing empty raw scaffold");c::Snapshot output;
        Check(A::Initialize(output)==R::Ok&&output.StoreId()!=sentinel.StoreId(),"empty legal scaffold is reused with independent fresh owner");VerifyInitial(fixture,output);auto session=Open();Check(session->Current().RawIdentity().volume==identity.volume&&session->Current().RawIdentity().fileIndex==identity.fileIndex,"initialization does not delete and replace existing empty raw directory");}
}
void FinalPolicyRecheck(const fs::path& root,const c::Snapshot& sentinel){Fixture fixture(root/L"policy-during-scaffold");LatePolicy change{fs::path(Prefix(fixture.root)+L"Video"),fixture.root,fixture.base/L"settings.json"};auto output=sentinel;latePolicy=&change;const auto result=A::Initialize(output);latePolicy=nullptr;
    Check(change.hit&&change.changed&&result==R::Limit&&output.SameVersion(sentinel),"policy disabled after actual first directory creation is reread before initial publication");
    for(const auto& item:fs::recursive_directory_iterator(fixture.root/L"Video"))if(item.is_regular_file())Check(item.path().filename()==L".lock","late policy refusal may retain only scaffold exclusion files, never candidate payload or intent");
    Persist(fixture,true);c::Snapshot retry;Check(A::Initialize(retry)==R::Ok,"later explicit approval reuses harmless scaffold after final policy refusal");VerifyInitial(fixture,retry);
}
void Interrupted(const fs::path& root,const c::Snapshot& sentinel){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"partial-commit":L"complete-commit"));Fault injected;injected.prefix=Prefix(fixture.root)+L"Video\\control\\catalog\\commits\\";injected.partial=partial;auto output=sentinel;fault=&injected;const auto result=A::Initialize(output);fault=nullptr;
    Check(injected.hits==1&&result==R::Indeterminate&&output.SameVersion(sentinel),"actual initialization commit ambiguity preserves foreign output snapshot");
    {c::WritePlan plan;Check(fixture.Store().PlanRecoveryExistingLocks(plan)==c::Result::Ok&&plan.RecoveryIsInitialization()&&plan.RecoveryApproval()&&plan.Recovery()==(partial?c::RecoveryAction::Rollback:c::RecoveryAction::ConfirmPublication)&&plan.RecoveryTarget().IsValid()!=partial,"persisted approved initialization distinguishes no previous root from completed first root");const auto& approval=*plan.RecoveryApproval();Check(approval.beforeRawResidual==0&&approval.beforeMetadata==0&&approval.afterRawResidual==0&&approval.afterMetadata>0,"initialization receipt funds first standby from zero previous source obligations");}
    const auto unresolved=CatalogFiles(fixture.Catalog());auto retryOutput=sentinel;Check(A::Initialize(retryOutput)==R::RecoveryRequired&&retryOutput.SameVersion(sentinel)&&CatalogFiles(fixture.Catalog())==unresolved,"second initialization cannot adopt even complete first publication while its approved intent remains pending");
    Persist(fixture,false);auto recovery=RecoverOpen();Check(recovery->Target().IsValid()!=partial,"rollback of first publication does not invent an empty authoritative prior snapshot");c::RecoveryReport report;
    Check(recovery->Execute(report)==R::Ok&&report.rolledBack==partial&&report.confirmedPublication!=partial&&recovery->IsConsumed(),"durable first-publication approval recovers after policy disabled");recovery.reset();
    Check(fs::is_directory(fixture.root/L"Video"/L"raw")&&fs::is_directory(fixture.Catalog()),"initialization recovery preserves scaffold directories");
    if(partial){c::Snapshot absent;Check(fixture.Store().Read(absent)==c::Result::Missing&&!absent.IsValid(),"initialization rollback leaves no published business root");Persist(fixture,true);c::Snapshot retry;Check(A::Initialize(retry)==R::Ok,"explicit fresh initialization can reuse scaffold after verified rollback");VerifyInitial(fixture,retry);}
    else {c::Snapshot head;Check(fixture.Store().Read(head)==c::Result::Ok,"confirmed initial root is readable");VerifyInitial(fixture,head);}
}}
void IntentFailures(const fs::path& root,const c::Snapshot& sentinel){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"partial-intent":L"full-intent"));Fault injected;injected.prefix=Prefix(fixture.root)+L"Video\\control\\catalog\\operations\\";injected.partial=partial;injected.retainFailed=true;auto output=sentinel;fault=&injected;const auto result=A::Initialize(output);fault=nullptr;
    Check(injected.hits==1&&injected.cleanupHits==1&&result!=R::Ok&&output.SameVersion(sentinel),"initialization intent I/O and disposal failure preserves caller output");const auto files=CatalogFiles(fixture.Catalog());unsigned candidates=0;
    for(const auto& file:files)if(file.first.find(L"snapshots\\")==0||file.first.find(L"commits\\")==0){++candidates;Check(file.second.empty(),"initial payload and commit bodies remain unwritten until complete durable intent succeeds");}Check(candidates==2,"first initialization has exactly its two empty prepared candidates");
    if(partial){std::unique_ptr<Recovery> refused;Check(Recovery::Open(refused)!=R::Ok&&!refused&&CatalogFiles(fixture.Catalog())==files,"incomplete approval cannot authorize first-root recovery or erase evidence");}
    else {Persist(fixture,false);auto recovery=RecoverOpen();c::RecoveryReport report;Check(!recovery->Target().IsValid()&&recovery->Execute(report)==R::Ok&&report.rolledBack,"complete first intent permits disabled rollback before candidate bodies were written");recovery.reset();Persist(fixture,true);c::Snapshot retry;Check(A::Initialize(retry)==R::Ok,"fully settled prebody initialization can be retried");VerifyInitial(fixture,retry);}
}}
void MissingRecoveryLock(const fs::path& root,const c::Snapshot& sentinel){Fixture fixture(root/L"missing-recovery-lock");Fault injected;injected.prefix=Prefix(fixture.root)+L"Video\\control\\catalog\\commits\\";injected.partial=true;auto output=sentinel;fault=&injected;const auto result=A::Initialize(output);fault=nullptr;Check(result==R::Indeterminate&&injected.hits==1,"create authentic pending first initialization for lock loss");
    const auto path=fixture.Catalog()/L".lock";Check(DeleteFileW(path.c_str())!=FALSE,"remove only test-owned idle catalog lock");const auto before=CatalogFiles(fixture.Catalog());std::unique_ptr<Recovery> refused;
    Check(Recovery::Open(refused)!=R::Ok&&!refused&&!fs::exists(path)&&CatalogFiles(fixture.Catalog())==before,"initialization recovery never repairs missing catalog lock as an implicit mutation");
}
void ReentryDuringScaffold(const fs::path& root,const c::Snapshot& sentinel){Fixture fixture(root/L"reentry-scaffold");LatePolicy change{fs::path(Prefix(fixture.root)+L"Video"),fixture.root,fixture.base/L"settings.json"};change.disable=false;change.reenter=true;change.sentinel=sentinel;c::Snapshot output;
    latePolicy=&change;const auto result=A::Initialize(output);latePolicy=nullptr;
    Check(change.hit&&change.reentryChecked&&change.reentryResult==R::Busy&&!change.writerAccepted&&result==R::Ok,"synchronous first-directory callback cannot reenter initialization or legacy publication; outer publication remains intact");VerifyInitial(fixture,output);
}
void SpawnRecovery(const Fixture& fixture,bool partial){wchar_t executable[32768]{};const auto length=GetModuleFileNameW(nullptr,executable,32768);Check(length&&length<32768,"resolve exact currently running isolated test executable");
    auto command=L"\""+std::wstring(executable,length)+L"\" --recover-initial-child \""+fixture.root.native()+L"\" "+(partial?L"rollback":L"confirm");STARTUPINFOW start{};start.cb=sizeof start;PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,fixture.base.c_str(),&start,&process),"start fresh recovery process in the current isolated fixture directory");Handle child(process.hProcess),thread(process.hThread);const auto waited=WaitForSingleObject(child.value,30000);if(waited!=WAIT_OBJECT_0){TerminateProcess(child.value,99);WaitForSingleObject(child.value,5000);}DWORD code=999;
    Check(waited==WAIT_OBJECT_0&&GetExitCodeProcess(child.value,&code)&&code==0,"new process reconstructs first-publication approval and recovers under disabled durable policy");
}
void RestartInitialization(const fs::path& root,const c::Snapshot& sentinel){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"restart-initial-rollback":L"restart-initial-confirm"));Fault injected;injected.prefix=Prefix(fixture.root)+L"Video\\control\\catalog\\commits\\";injected.partial=partial;auto output=sentinel;
    fault=&injected;const auto result=A::Initialize(output);fault=nullptr;Check(result==R::Indeterminate&&injected.hits==1&&output.SameVersion(sentinel),"prepare actual first-publication residue for independent process recovery");Persist(fixture,false);SpawnRecovery(fixture,partial);
    c::Snapshot head;const auto read=fixture.Store().Read(head);Check(partial?(read==c::Result::Missing&&!head.IsValid()):(read==c::Result::Ok&&head.Revision()==1),"parent independently observes exact child-recovered empty or initialized catalog");
    if(!partial)VerifyInitial(fixture,head);else {Persist(fixture,true);Check(A::Initialize(head)==R::Ok,"parent can explicitly initialize retained scaffold after child rollback");VerifyInitial(fixture,head);}
}}
void ChildRecover(const fs::path& root,bool partial){Check(root.is_absolute()&&root==root.lexically_normal(),"child receives exact absolute fixture root");Select(root);namespace p=mikumikustudio::cache_policy;p::Store policy(root,root.parent_path()/L"settings.json");p::Snapshot settings;
    Check(policy.Read(settings)==p::Result::Ok&&!settings.Value().enabled,"fresh process verifies on-disk allocation policy is disabled");auto session=RecoverOpen();Check(session->Action()==(partial?c::RecoveryAction::Rollback:c::RecoveryAction::ConfirmPublication)&&session->Target().IsValid()!=partial,"fresh process derives expected initial recovery and optional endpoint from disk alone");
    c::RecoveryReport report;Check(session->Execute(report)==R::Ok&&report.rolledBack==partial&&report.confirmedPublication!=partial&&report.revision==(partial?0u:1u),"fresh process applies existing durable authority without a new initialization or publication request");
}
} // namespace
int wmain(int argc,wchar_t** argv){try{if(argc==4&&std::wstring(argv[1])==L"--recover-initial-child"){Check(std::wstring(argv[3])==L"rollback"||std::wstring(argv[3])==L"confirm","explicit child recovery disposition");ChildRecover(argv[2],std::wstring(argv[3])==L"rollback");return 0;}Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_source_initialization_tests --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;
    const auto sentinel=Success(root);Preflight(root,sentinel);FinalPolicyRecheck(root,sentinel);Interrupted(root,sentinel);IntentFailures(root,sentinel);MissingRecoveryLock(root,sentinel);ReentryDuringScaffold(root,sentinel);RestartInitialization(root,sentinel);std::printf("PASS cache source initialization checks=%u\n",checks);return 0;
}catch(const std::exception& error){fault=nullptr;std::fprintf(stderr,"FAIL cache source initialization checks=%u: %s\n",checks,error.what());return 1;}}
