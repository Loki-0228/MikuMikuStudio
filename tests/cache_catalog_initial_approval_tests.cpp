// MAINTENANCE: regression-keep. Approved initial publication from an explicitly established empty scaffold.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "mikumikustudio/cache_catalog.hpp"
#include "mikumikustudio/cache_approval.hpp"
#include "../src/io/cache_catalog_intent.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;namespace c=mikumikustudio::cache_catalog;namespace f=mikumikustudio::cache_files;namespace i=c::intent;
using R=c::Result;using Bytes=std::vector<std::uint8_t>;unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"DIAG check=%u win32=%lu\n",checks,GetLastError());throw std::runtime_error(why);}}
struct Handle{HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;};
void Fresh(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"absolute normalized private path");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim new private directory");}
f::Owner Owner(){f::Owner owner{};owner[0]=0x5d;return owner;}
using Evidence=std::map<std::wstring,Bytes>;
Evidence Files(const fs::path& root){Evidence out;for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file()){std::ifstream stream(entry.path(),std::ios::binary);Check(bool(stream),"read actual owned evidence");out.emplace(entry.path().lexically_relative(root).native(),Bytes(std::istreambuf_iterator<char>(stream),{}));}return out;}
std::wstring Prefix(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t text[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,text,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"bind faults to exact physical fixture");return std::wstring(text,n)+L'\\';}
enum class Fault{None,Partial,Flush,Abort,IntentPartial,IntentFlush,SnapshotPartial,SnapshotFlush};
struct Observe{std::wstring prefix;Fault fault=Fault::None;unsigned creates=0,writes=0,flushes=0,hits=0,cleanupHits=0,candidateWrites=0;Observe(const fs::path& p,Fault f=Fault::None):prefix(Prefix(p)),fault(f){}
    int Match(HANDLE h)const noexcept{wchar_t text[32768]{};const auto n=GetFinalPathNameByHandleW(h,text,32768,VOLUME_NAME_GUID);if(n<=prefix.size()||n>=32768||_wcsnicmp(text,prefix.c_str(),prefix.size()))return 0;const auto* relative=text+prefix.size();return !std::wcsncmp(relative,L"commits\\",8)?2:!std::wcsncmp(relative,L"operations\\",11)?3:!std::wcsncmp(relative,L"snapshots\\",10)?4:1;}};
thread_local Observe* active=nullptr;decltype(&CreateFileW) realCreate=nullptr;decltype(&WriteFile) realWrite=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&SetFileInformationByHandle) realSet=nullptr;
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD mode,DWORD flags,HANDLE original)noexcept{const auto result=realCreate(path,access,sharing,security,mode,flags,original);const auto error=GetLastError();if(active&&mode==CREATE_NEW&&result!=INVALID_HANDLE_VALUE&&active->Match(result))++active->creates;SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{auto* p=active;const auto kind=p?p->Match(h):0;if(kind)++p->writes;if(kind==2||kind==4)++p->candidateWrites;LARGE_INTEGER zero{},position{};if(((kind==2&&p->fault==Fault::Partial)||(kind==3&&p->fault==Fault::IntentPartial)||(kind==4&&p->fault==Fault::SnapshotPartial))&&!p->hits&&!overlap&&count>1&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto ok=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(ok){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return ok;}return realWrite(h,bytes,count,done,overlap);}
BOOL WINAPI FlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();auto* p=active;const auto kind=p?p->Match(h):0;if(kind)++p->flushes;if(result&&((kind==2&&p->fault==Fault::Flush)||(kind==3&&p->fault==Fault::IntentFlush)||(kind==4&&p->fault==Fault::SnapshotFlush))&&!p->hits){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
BOOL WINAPI SetHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS type,LPVOID info,DWORD bytes)noexcept{auto* p=active;if(p&&type==FileDispositionInfo&&bytes>=sizeof(FILE_DISPOSITION_INFO)&&static_cast<FILE_DISPOSITION_INFO*>(info)->DeleteFile){const auto kind=p->Match(h);if(p->fault==Fault::Abort&&!p->hits&&kind==2){++p->hits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}if(p->hits&&kind==3&&(p->fault==Fault::IntentPartial||p->fault==Fault::IntentFlush)){++p->cleanupHits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}}return realSet(h,type,info,bytes);}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK,"initialize scoped real IO hooks");Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&SetHook),reinterpret_cast<void**>(&realSet))==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable exact-fixture fault hooks");}~Hooks(){active=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm{explicit Arm(Observe& p){Check(!active,"nonrecursive fault scope");active=&p;}~Arm(){active=nullptr;}};
std::uint64_t Round(std::uint64_t n,std::uint64_t unit){return ((n+unit-1)/unit)*unit;}
struct Fixture{fs::path root;std::array<fs::path,9> paths;c::Store store;
    Fixture(const fs::path& parent,const wchar_t* name):root(parent/name),paths{root,root/L"Video",root/L"Video"/L"control",root/L"Video"/L"control"/L"catalog",root/L"Video"/L"pages",root/L"Video"/L"raw",root/L"Video"/L"control"/L"catalog"/L"snapshots",root/L"Video"/L"control"/L"catalog"/L"commits",root/L"Video"/L"control"/L"catalog"/L"operations"},store(paths[3]){for(const auto& path:paths)Fresh(path);}
    c::Snapshot Init(){c::Snapshot result;Check(store.InitializeOwned(Owner(),{1,2,3},result)==R::Ok,"initialize real ordinary prior head");return result;}
    c::PublicationApproval Approval(const c::WritePlan& plan){c::PublicationApproval result;for(std::size_t n=0;n<paths.size();++n){f::Directory directory;Check(f::Directory::Open(paths[n],false,directory)==f::Result::Ok&&directory.GetIdentity(result.directories[n])==f::Result::Ok,"bind each of nine actual distinct directories");}wchar_t mount[32768]{};DWORD sectors=0,bytes=0,free=0,total=0;Check(GetVolumePathNameW(root.c_str(),mount,32768)&&GetDiskFreeSpaceW(mount,&sectors,&bytes,&free,&total)&&sectors&&bytes,"measure actual allocation unit");result.allocationUnit=std::uint64_t(sectors)*bytes;f::Directory directory;f::InventorySnapshot inventory;f::InventoryLimits limits;Check(f::Directory::Open(root,false,directory)==f::Result::Ok&&directory.Inventory(limits,inventory)==f::Result::Ok,"measure actual physical inventory for caller-supplied approval facts");
        std::map<std::pair<std::uint32_t,std::uint64_t>,std::uint64_t> physical;for(const auto& entry:inventory.Entries()){const auto& o=entry.observed;physical[{o.identity.volume,o.identity.fileIndex}]=(std::max)(o.streamLogicalBytes,o.streamAllocationBytes);}for(const auto& entry:physical)result.physicalBytes+=entry.second;
        result.policyEnabled=true;result.policyLimitBytes=128ull*1024*1024;result.approvedPeak=result.physicalBytes;for(unsigned n=0;n<plan.Footprint().count;++n)result.approvedPeak+=Round(plan.Footprint().files[n].maximumFileBytes,result.allocationUnit);return result;
    }
};

void ScaffoldLock(Fixture& fixture){f::Directory directory;f::FileLock lock;Check(f::Directory::Open(fixture.paths[3],false,directory)==f::Result::Ok&&directory.Lock(0,lock)==f::Result::Ok,"fixture explicitly establishes empty catalog exclusion file");}
c::WritePlan Plan(Fixture& fixture){c::WritePlan plan;const auto before=Files(fixture.root);Observe observation(fixture.paths[3]);R result;
    {Arm arm(observation);result=fixture.store.PlanApprovedInitializeOwned(Owner(),{1,2,3},plan);}
    Check(result==R::Ok&&plan.RequiresApproval()&&plan.Kind()==c::WriteKind::Initialize&&!plan.IsConsumed()&&!observation.creates&&!observation.writes&&!observation.flushes&&Files(fixture.root)==before,"approved initialization plans read-only in existing empty scaffold");return plan;}
void SuccessAndRefusal(const fs::path& root){Fixture fixture(root,L"success");ScaffoldLock(fixture);auto plan=Plan(fixture);const auto before=Files(fixture.root);const auto approval=fixture.Approval(plan);c::Snapshot output;
    Check(fixture.store.Execute(plan,output)==R::Invalid&&!plan.IsConsumed()&&Files(fixture.root)==before,"ordinary execution cannot bypass initial approval");
    auto bad=approval;++bad.approvedPeak;Observe observation(fixture.paths[3]);R result;
    {Arm arm(observation);result=fixture.store.ExecuteApproved(plan,bad,output);}
    Check(result==R::Invalid&&!plan.IsConsumed()&&!output.IsValid()&&!observation.creates&&!observation.writes&&Files(fixture.root)==before,"bad arithmetic fails before any initial candidate creation or body write");
    bad=approval;bad.directories[6].fileIndex^=0x123456;Check(fixture.store.ExecuteApproved(plan,bad,output)!=R::Ok&&!plan.IsConsumed()&&Files(fixture.root)==before,"foreign actual snapshot directory binding cannot initialize");
    Check(fixture.store.ExecuteApproved(plan,approval,output)==R::Ok&&plan.IsConsumed()&&output.IsValid()&&output.Revision()==1&&output.StoreId()==Owner()&&output.Bytes()==Bytes({1,2,3}),"approved first publication writes actual revision one");
    c::Snapshot actual;Check(fixture.store.Read(actual)==R::Ok&&actual.SameVersion(output),"ordinary reader sees verified initial head");c::WritePlan second;const auto published=Files(fixture.root);
    Check(fixture.store.PlanApprovedInitializeOwned(Owner(),{9},second)==R::Conflict&&!second.IsValid()&&Files(fixture.root)==published,"initialization never replaces existing head");
    c::WritePlan recovery;Check(fixture.store.PlanRecoveryExistingLocks(recovery)==R::Ok&&recovery.Recovery()==c::RecoveryAction::Noop&&!recovery.RecoveryIsInitialization(),"clean no-op is not misclassified as pending initialization");
}
void MissingAndUnknown(const fs::path& root){Fixture fixture(root,L"missing-lock");const auto before=Files(fixture.root);c::WritePlan plan;f::Owner zero{};
    Check(fixture.store.PlanApprovedInitializeOwned(zero,{1},plan)==R::Invalid&&!plan.IsValid()&&Files(fixture.root)==before,"zero initial owner is refused without changes");
    Check(fixture.store.PlanApprovedInitializeOwned(Owner(),{1},plan)!=R::Ok&&!plan.IsValid()&&!fs::exists(fixture.paths[3]/L".lock")&&Files(fixture.root)==before,"initial planning does not bootstrap missing lock");
    ScaffoldLock(fixture);Check(RemoveDirectoryW(fixture.paths[8].c_str()),"remove only empty fixture operations directory");const auto noOperations=Files(fixture.root);
    Check(fixture.store.PlanApprovedInitializeOwned(Owner(),{1},plan)!=R::Ok&&!plan.IsValid()&&!fs::exists(fixture.paths[8])&&Files(fixture.root)==noOperations,"initial planning does not create missing operations directory");
    Fresh(fixture.paths[8]);{std::ofstream out(fixture.paths[6]/L"unknown.bin",std::ios::binary);out<<"foreign";}
    const auto unknown=Files(fixture.root);Check(fixture.store.PlanApprovedInitializeOwned(Owner(),{1},plan)!=R::Ok&&!plan.IsValid()&&Files(fixture.root)==unknown,"unknown preexisting candidate blocks initialization without adoption or deletion");
    const auto absent=root/L"absent";c::Store missing(absent);Check(missing.PlanApprovedInitializeOwned(Owner(),{},plan)!=R::Ok&&!fs::exists(absent),"initial planning never creates a missing metadata root");
}
void Interruptions(const fs::path& root){unsigned index=0;for(auto fault:{Fault::Partial,Fault::Flush,Fault::SnapshotPartial,Fault::SnapshotFlush,Fault::IntentPartial,Fault::IntentFlush}){
    const auto name=L"fault-"+std::to_wstring(index++);Fixture fixture(root,name.c_str());ScaffoldLock(fixture);const auto empty=Files(fixture.root);auto plan=Plan(fixture);const auto approval=fixture.Approval(plan);c::Snapshot output;Observe observation(fixture.paths[3],fault);R result;
    {Arm arm(observation);result=fixture.store.ExecuteApproved(plan,approval,output);}
    Check(result!=R::Ok&&plan.IsConsumed()&&!output.IsValid()&&observation.hits==1,"actual first-publication I/O failure consumes plan and preserves output");
    const bool intent=fault==Fault::IntentPartial||fault==Fault::IntentFlush;
    if(intent)Check(!observation.candidateWrites&&observation.cleanupHits==1,"failed durable approval leaves both initial candidate bodies unwritten");
    const auto pendingFiles=Files(fixture.root);c::WritePlan duplicate;Check(fixture.store.PlanApprovedInitializeOwned(Owner(),{7},duplicate)==R::RecoveryRequired&&!duplicate.IsValid()&&Files(fixture.root)==pendingFiles,"pending first approval blocks second initialize before adopting a complete or incomplete candidate");
    c::WritePlan recovery;const auto evidence=Files(fixture.root);const auto recovered=fixture.store.PlanRecoveryExistingLocks(recovery);
    if(fault==Fault::IntentPartial){Check(recovered!=R::Ok&&!recovery.IsValid()&&Files(fixture.root)==evidence,"partial approved intent is not recovery or ownership authority");continue;}
    const bool confirm=fault==Fault::Flush;
    Check(recovered==R::Ok&&recovery.RecoveryIsInitialization()&&!recovery.RecoveryIsCheckpoint()&&recovery.RecoveryApproval()&&recovery.Recovery()==(confirm?c::RecoveryAction::ConfirmPublication:c::RecoveryAction::Rollback)&&recovery.RecoveryTarget().IsValid()==confirm&&Files(fixture.root)==evidence,"pending initial plan preserves approval and distinguishes real head from absent rollback target");
    c::RecoveryReport report;report.revision=777;Check(fixture.store.ExecuteRecovery(recovery,report)==R::Ok&&recovery.IsConsumed()&&report.confirmedPublication==confirm&&report.rolledBack!=confirm&&report.revision==(confirm?1u:0u),"explicit recovery confirms complete initial publication or rolls back to no head");
    if(confirm){c::Snapshot actual;Check(fixture.store.Read(actual)==R::Ok&&actual.Revision()==1&&actual.Bytes()==Bytes({1,2,3}),"confirmed initial payload is actual intended bytes");}
    else {Check(Files(fixture.root)==empty,"rollback deletes only initial owned candidates and logs, preserving empty scaffold");auto retry=Plan(fixture);const auto retryApproval=fixture.Approval(retry);Check(fixture.store.ExecuteApproved(retry,retryApproval,output)==R::Ok&&output.Revision()==1,"new explicit initial plan can publish after confirmed full rollback");}
}}
Bytes ReadExactFile(const fs::path& path){std::ifstream in(path,std::ios::binary);Check(bool(in),"open exact default or named data stream evidence");return Bytes(std::istreambuf_iterator<char>(in),{});}
void ForeignStreams(const fs::path& root){for(unsigned target=0;target<4;++target)for(bool empty:{false,true}){
    const auto name=L"foreign-stream-"+std::to_wstring(target)+L"-"+std::to_wstring(empty);Fixture fixture(root,name.c_str());ScaffoldLock(fixture);auto initial=Plan(fixture);const auto approval=fixture.Approval(initial);c::Snapshot output;Observe failure(fixture.paths[3],Fault::Partial);R interrupted;
    {Arm arm(failure);interrupted=fixture.store.ExecuteApproved(initial,approval,output);}Check(interrupted==R::Indeterminate&&failure.hits==1,"create actual incomplete initial commit before adding foreign stream");
    fs::path intent;for(const auto& entry:fs::directory_iterator(fixture.paths[8])){Check(intent.empty(),"one actual pending intent before rollback decision");intent=entry.path();}
    Check(!intent.empty(),"retain exact intent name for distinct decision selection");
    if(target==3){c::WritePlan recovery;Check(fixture.store.PlanRecoveryExistingLocks(recovery)==R::Ok,"plan authentic rollback before retained decision fixture");Observe abort(fixture.paths[3],Fault::Abort);c::RecoveryReport report;
        {Arm arm(abort);interrupted=fixture.store.ExecuteRecovery(recovery,report);}Check(interrupted!=R::Ok&&abort.hits==1,"actual failed candidate disposal retains complete rollback decision");}
    fs::path victim;if(target==2)victim=intent;else for(const auto& entry:fs::directory_iterator(fixture.paths[target==0?6:target==1?7:8])){if(target==3&&entry.path()==intent)continue;Check(victim.empty(),"uniquely select actual candidate or decision");victim=entry.path();}
    Check(!victim.empty(),"find actual target to which foreign named stream will be attached");const auto before=ReadExactFile(victim);const fs::path stream(victim.native()+L":foreign-evidence");const Bytes value=empty?Bytes{}:Bytes{0x41,0x00,0x73,0x19};
    {Handle file(CreateFileW(stream.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD wrote=0;Check(file.value!=INVALID_HANDLE_VALUE&&WriteFile(file.value,value.data(),DWORD(value.size()),&wrote,nullptr)&&wrote==value.size()&&FlushFileBuffers(file.value),"attach actual empty or nonempty foreign ADS to owned catalog file");}
    Check(ReadExactFile(stream)==value,"foreign ADS exists and is readable independently of default stream");c::WritePlan recovery;const auto planned=fixture.store.PlanRecoveryExistingLocks(recovery);R result=planned;
    if(planned==R::Ok){c::RecoveryReport report;report.revision=777;result=fixture.store.ExecuteRecovery(recovery,report);Check(result!=R::Ok&&report.revision==777,"foreign-stream recovery cannot report successful removal or rewrite caller report");}
    Check(result!=R::Ok&&fs::exists(victim)&&ReadExactFile(victim)==before&&ReadExactFile(stream)==value,"refused recovery preserves both exact default payload and foreign ADS, including empty named stream");
}}

}
int wmain(int argc,wchar_t** argv){try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;SuccessAndRefusal(root);MissingAndUnknown(root);Interruptions(root);ForeignStreams(root);std::printf("PASS cache catalog initial approval checks=%u\n",checks);return 0;}catch(const std::exception& error){active=nullptr;std::fprintf(stderr,"FAIL cache catalog initial approval checks=%u: %s\n",checks,error.what());return 1;}}
