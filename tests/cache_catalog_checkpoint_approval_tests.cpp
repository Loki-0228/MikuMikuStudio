// MAINTENANCE: regression-keep. Actual approved catalog protocol, not quota policy authorization.
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
enum class Fault{None,Partial,Flush,Abort,IntentPartial,IntentFlush};
struct Observe{std::wstring prefix;Fault fault=Fault::None;unsigned creates=0,writes=0,flushes=0,hits=0,cleanupHits=0,candidateWrites=0;Observe(const fs::path& p,Fault f=Fault::None):prefix(Prefix(p)),fault(f){}
    int Match(HANDLE h)const noexcept{wchar_t text[32768]{};const auto n=GetFinalPathNameByHandleW(h,text,32768,VOLUME_NAME_GUID);if(n<=prefix.size()||n>=32768||_wcsnicmp(text,prefix.c_str(),prefix.size()))return 0;const auto* relative=text+prefix.size();return !std::wcsncmp(relative,L"commits\\",8)?2:!std::wcsncmp(relative,L"operations\\",11)?3:!std::wcsncmp(relative,L"snapshots\\",10)?4:1;}};
thread_local Observe* active=nullptr;decltype(&CreateFileW) realCreate=nullptr;decltype(&WriteFile) realWrite=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&SetFileInformationByHandle) realSet=nullptr;
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD mode,DWORD flags,HANDLE original)noexcept{const auto result=realCreate(path,access,sharing,security,mode,flags,original);const auto error=GetLastError();if(active&&mode==CREATE_NEW&&result!=INVALID_HANDLE_VALUE&&active->Match(result))++active->creates;SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{auto* p=active;const auto kind=p?p->Match(h):0;if(kind)++p->writes;if(kind==2||kind==4)++p->candidateWrites;LARGE_INTEGER zero{},position{};if(((kind==2&&p->fault==Fault::Partial)||(kind==3&&p->fault==Fault::IntentPartial))&&!p->hits&&!overlap&&count>1&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto ok=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(ok){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return ok;}return realWrite(h,bytes,count,done,overlap);}
BOOL WINAPI FlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();auto* p=active;const auto kind=p?p->Match(h):0;if(kind)++p->flushes;if(result&&((kind==2&&p->fault==Fault::Flush)||(kind==3&&p->fault==Fault::IntentFlush))&&!p->hits){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
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
struct Log{f::BlobInfo info;Bytes bytes;};
std::vector<Log> Logs(Fixture& fixture){f::Directory dir;Check(f::Directory::Open(fixture.paths[8],false,dir)==f::Result::Ok,"open actual operations namespace");std::vector<std::wstring> names;Check(dir.Enumerate(16,names)==f::Result::Ok,"enumerate actual operation files");std::vector<Log> result;for(const auto& name:names){f::ReadLease lease;Check(dir.Inspect(name,c::codec::NamespaceOwner,i::MaximumOperationBytes,lease)==f::Result::Ok,"verify actual operation envelope");Log item;item.info=lease.Info();Check(lease.ReadAll(item.bytes)==f::Result::Ok,"read exact persisted operation bytes");result.push_back(std::move(item));}return result;}

c::Snapshot Seed(Fixture& fixture){auto head=fixture.Init();for(unsigned n=0;n<3;++n){c::Snapshot next;Check(fixture.store.Commit(head,Bytes(19+n,std::uint8_t(10+n)),next)==R::Ok,"publish distinct real history before checkpoint");head=std::move(next);}return head;}
void Rejects(const fs::path& root){
    Fixture fixture(root,L"preflight"),foreign(root,L"foreign");const auto head=Seed(fixture);foreign.Init();
    c::WritePlan plan;Check(fixture.store.PlanApprovedCheckpoint(head,plan)==R::Ok&&plan.RequiresApproval()&&plan.Kind()==c::WriteKind::Checkpoint,"plan actual approved checkpoint");
    const auto approval=fixture.Approval(plan);const auto before=Files(fixture.root);auto output=head;
    const auto unchanged=[&](R result,const Observe& watch){Check(result!=R::Ok&&!watch.creates&&!watch.writes&&!watch.flushes&&!plan.IsConsumed()&&!plan.MayHaveWritten()&&output.SameVersion(head)&&Files(fixture.root)==before,"preflight refusal precedes all candidate creation and preserves plan/output/disk");};
    {Observe watch(fixture.paths[3]);R result;{Arm arm(watch);result=fixture.store.Execute(plan,output);}unchanged(result,watch);}
    for(unsigned mode=0;mode<7;++mode){auto bad=approval;switch(mode){case 0:++bad.approvedPeak;break;case 1:bad.allocationUnit=0;break;case 2:bad.directories[1]=bad.directories[0];break;case 3:bad.policyEnabled=false;break;case 4:bad.physicalBytes=UINT64_MAX;break;case 5:bad.directories[static_cast<std::size_t>(c::ApprovalArea::Commits)].fileIndex^=0x8000000000000000ull;break;case 6:bad.policyLimitBytes=bad.approvedPeak-1;break;}
        Observe watch(fixture.paths[3]);R result;{Arm arm(watch);result=fixture.store.ExecuteApproved(plan,bad,output);}unchanged(result,watch);}
    {const auto otherBefore=Files(foreign.root);Observe watch(foreign.paths[3]);R result;{Arm arm(watch);result=foreign.store.ExecuteApproved(plan,approval,output);}Check(result!=R::Ok&&!watch.creates&&!watch.writes&&!plan.IsConsumed()&&Files(foreign.root)==otherBefore&&Files(fixture.root)==before&&output.SameVersion(head),"foreign catalog cannot consume approved checkpoint plan");}
    c::Snapshot next;Check(fixture.store.Commit(head,{91,92},next)==R::Ok,"advance actual head after checkpoint plan");const auto advanced=Files(fixture.root);Observe watch(fixture.paths[3]);R result;{Arm arm(watch);result=fixture.store.ExecuteApproved(plan,approval,output);}
    Check(result==R::Conflict&&!watch.creates&&!watch.writes&&!plan.IsConsumed()&&output.SameVersion(head)&&Files(fixture.root)==advanced,"stale checkpoint cannot retire intervening generation");
}
void VerifyRetired(Fixture& fixture,const i::ApprovedCheckpointRecord& operation,const Evidence& before){
    Check(operation.publication.retired.size()+1==before.size()-1,"retirement list covers exactly old commits and obsolete snapshots, excluding lock and live snapshot");
    std::map<std::wstring,bool> names;
    for(const auto& ref:operation.publication.retired){const auto path=(ref.spec.kind==f::Kind::CatalogSnapshot?L"snapshots\\":L"commits\\")+ref.name;
        Check(before.count(path)&&names.emplace(path,true).second,"every retirement entry names one distinct old real generation");
        f::Directory directory;f::ReadLease lease;Check(f::Directory::Open(fixture.paths[3]/path.substr(0,path.find(L'\\')),false,directory)==f::Result::Ok&&directory.Acquire(ref,lease)==f::Result::Ok,"retirement binds actual complete physical identity and digest");}
    const auto kept=L"snapshots\\"+operation.publication.state.snapshot.name;Check(before.count(kept)&&!names.count(kept),"active payload is never retired");
    for(const auto& file:before)if(file.first!=L".lock"&&file.first!=kept)Check(names.count(file.first)==1,"no old referenced generation omitted from canonical retirement list");
}
void Recovery(const fs::path& root){for(bool partial:{false,true}){Fixture fixture(root,partial?L"rollback":L"confirm");const auto head=Seed(fixture);c::WritePlan plan;
    Check(fixture.store.PlanApprovedCheckpoint(head,plan)==R::Ok,"plan bounded maintenance candidate");const auto approval=fixture.Approval(plan);const auto before=Files(fixture.paths[3]);auto output=head;
    Observe fault(fixture.paths[3],partial?Fault::Partial:Fault::Flush);R result;{Arm arm(fault);result=fixture.store.ExecuteApproved(plan,approval,output);}
    Check(result==R::Indeterminate&&fault.hits==1&&plan.IsConsumed()&&output.SameVersion(head),"actual checkpoint body interruption retains old caller output");
    const auto logs=Logs(fixture);Check(logs.size()==1&&logs.front().info.spec.key==i::ApprovedCheckpointKey,"checkpoint has distinct real approved operation key");i::ApprovedCheckpointRecord operation;
    Check(i::DecodeApprovedCheckpoint(logs[0].bytes.data(),logs[0].bytes.size(),operation)==i::Result::Ok&&operation.publication.state.revision==head.Revision()+1,"decode actual persisted retirement plan and approval");VerifyRetired(fixture,operation,before);
    const auto after=Files(fixture.paths[3]);for(const auto& file:before)Check(after.at(file.first)==file.second,"publication fault never alters or collects any old generation");
    c::WritePlan recovery;Check(fixture.store.PlanRecoveryExistingLocks(recovery)==R::Ok&&recovery.RecoveryIsCheckpoint()&&recovery.RecoveryApproval()&&recovery.Recovery()==(partial?c::RecoveryAction::Rollback:c::RecoveryAction::ConfirmPublication),"real recovery identifies approved checkpoint disposition");
    const auto target=recovery.RecoveryTarget();c::RecoveryReport report;Check(fixture.store.ExecuteRecovery(recovery,report)==R::Ok&&report.rolledBack==partial&&report.confirmedPublication!=partial,"execute explicit checkpoint recovery");c::Snapshot current;
    Check(fixture.store.Read(current)==R::Ok&&current.SameVersion(target)&&current.Bytes()==head.Bytes()&&Logs(fixture).empty(),"recovery keeps exact checkpoint endpoint and user payload");
    if(partial)Check(Files(fixture.paths[3])==before,"rollback removes only exact candidate and operation, never retirement entries");
    else{const auto confirmed=Files(fixture.paths[3]);for(const auto& file:before)Check(confirmed.at(file.first)==file.second,"confirmation itself never collects retired files");c::CollectionReport collected;Check(fixture.store.CollectRetired(current,collected)==R::Ok&&collected.absent==operation.publication.retired.size()&&!collected.failed&&!collected.busy,"explicit collection removes exactly canonical retired entries");c::Snapshot next;Check(fixture.store.Commit(current,{70,71},next)==R::Ok,"ordinary publication continues after real approved checkpoint recovery and collection");}
}}
void Success(const fs::path& root){Fixture fixture(root,L"success");const auto head=Seed(fixture);const auto before=Files(fixture.paths[3]);c::WritePlan plan;Check(fixture.store.PlanApprovedCheckpoint(head,plan)==R::Ok,"plan successful actual maintenance");const auto approval=fixture.Approval(plan);c::Snapshot output;Check(fixture.store.ExecuteApproved(plan,approval,output)==R::Ok&&plan.IsConsumed()&&output.IsCheckpoint()&&output.Revision()==head.Revision()+1&&output.Bytes()==head.Bytes()&&Logs(fixture).empty(),"successful approved checkpoint publishes exact unchanged user payload and removes its owned intent");const auto after=Files(fixture.paths[3]);for(const auto& file:before)Check(after.at(file.first)==file.second,"successful checkpoint never collects before separate authorization");c::CollectionReport report;Check(fixture.store.CollectRetired(output,report)==R::Ok&&report.absent==before.size()-2&&!report.failed&&!report.busy,"successful explicit retirement preserves only active payload and checkpoint");}
void Unknown(const fs::path& root){Fixture fixture(root,L"unreferenced-snapshot");const auto head=Seed(fixture);f::Directory snapshots;Check(f::Directory::Open(fixture.paths[6],false,snapshots)==f::Result::Ok,"open actual snapshot namespace");f::Spec spec;spec.owner=Owner();spec.kind=f::Kind::CatalogSnapshot;spec.key=c::codec::SnapshotKey;f::BlobInfo orphan;const Bytes payload{211,212,213};Check(snapshots.Create(spec,payload.data(),payload.size(),orphan)==f::Result::Ok,"create valid same-owner but never-referenced generation");
    const auto before=Files(fixture.paths[3]);c::WritePlan plan;const auto planned=fixture.store.PlanApprovedCheckpoint(head,plan);
    if(planned!=R::Ok){Check(planned==R::RecoveryRequired&&!plan.IsValid()&&Files(fixture.paths[3])==before,"unknown generation is explicitly refused without mutating or adopting files");return;}
    const auto approval=fixture.Approval(plan);c::Snapshot output;const auto result=fixture.store.ExecuteApproved(plan,approval,output);
    if(result==R::Ok){c::CollectionReport report;Check(fixture.store.CollectRetired(output,report)==R::Ok,"collect only actual authorized retirement entries");}
    f::ReadLease remains;Check(snapshots.Acquire(orphan,remains)==f::Result::Ok,"valid but unreferenced foreign generation must never be adopted into checkpoint retirement");
}
void Missing(const fs::path& root){for(bool lock:{false,true}){Fixture fixture(root,lock?L"missing-lock":L"missing-operations");const auto head=fixture.Init();const auto missing=lock?fixture.paths[3]/L".lock":fixture.paths[8];Check(lock?DeleteFileW(missing.c_str())!=FALSE:RemoveDirectoryW(missing.c_str())!=FALSE,"remove owned idle fixture prerequisite");const auto before=Files(fixture.root);c::WritePlan plan;Observe watch(fixture.paths[3]);R result;{Arm arm(watch);result=fixture.store.PlanApprovedCheckpoint(head,plan);}Check(result!=R::Ok&&!plan.IsValid()&&!fs::exists(missing)&&Files(fixture.root)==before&&!watch.creates&&!watch.writes,"approved checkpoint never bootstraps a missing exclusion prerequisite");}}
}
int wmain(int argc,wchar_t**argv){try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;Rejects(root);Success(root);Recovery(root);Missing(root);Unknown(root);std::printf("PASS cache catalog checkpoint approval checks=%u\n",checks);return 0;}catch(const std::exception&e){active=nullptr;std::fprintf(stderr,"FAIL cache catalog checkpoint approval checks=%u: %s\n",checks,e.what());return 1;}}
