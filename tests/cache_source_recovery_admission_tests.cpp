// MAINTENANCE: regression-keep. Real interrupted publication and manager recovery admission.
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

struct Fixture {fs::path base,root;f::Directory manager,video,control,pages,raw;std::unique_ptr<b::SourceJournal> journal;b::BusinessRoot initial;
    explicit Fixture(const fs::path& p):base(p),root(base/L"Cache"){Fresh(base);Policy(root);Check(f::Directory::Open(root,false,manager)==f::Result::Ok&&manager.Child(L"Video",true,video)==f::Result::Ok&&video.Child(L"control",true,control)==f::Result::Ok&&video.Child(L"pages",true,pages)==f::Result::Ok&&video.Child(L"raw",true,raw)==f::Result::Ok,"create actual trusted Video layout");
        initial.legacy.storeOwner=Id<f::Owner>(1);initial.legacy.maintenanceEpoch=1;initial.legacy.nextProjectId=2;initial.sources.storeOwner=initial.legacy.storeOwner;initial.sources.maintenanceEpoch=1;b::DirectoryBinding binding;binding.id=1;Check(raw.GetIdentity(binding.identity)==f::Result::Ok,"capture real raw directory identity");initial.sources.directories.push_back(binding);
        b::Page page;page.storeOwner=initial.legacy.storeOwner;b::Project project;project.id=1;project.normalizedPath=project.displayPath=u"C:\\source-admission-test\\project.pmm";page.projects.push_back(project);Bytes encoded;Check(b::EncodePage(page,encoded)==b::Result::Ok,"encode actual project page");f::Spec spec;spec.owner=page.storeOwner;spec.kind=f::Kind::IndexPage;spec.key=b::PageKey;f::BlobInfo blob;
        Check(pages.Create(spec,encoded.data(),encoded.size(),blob)==f::Result::Ok,"write actual checksummed page");b::PageRef reference;reference.recordCount=1;reference.blob=blob;initial.legacy.pages.push_back(reference);initial.legacy.projectCount=1;initial.legacy.liveEncodedBytes=b::ManifestHeaderBytes+b::PageRefBytes+encoded.size();c::Snapshot first;
        Check(b::EncodeBusinessRoot(initial,encoded)==b::Result::Ok&&Store().InitializeOwned(initial.legacy.storeOwner,encoded,first)==c::Result::Ok,"persist valid source catalog with real page and raw references");Check(b::SourceJournal::Open(control,pages,raw,journal)==b::JournalResult::Ok,"establish existing source control lock through actual journal");}
    fs::path Catalog()const{return root/L"Video"/L"control"/L"catalog";}
    c::Store Store()const{return c::Store(Catalog());}
    c::Snapshot Head()const{c::Snapshot result;Check(Store().Read(result)==c::Result::Ok,"read real authoritative catalog head");return result;}
    void External(const b::BusinessRoot& value){const auto before=Head();Bytes bytes;c::Snapshot out;Check(b::EncodeBusinessRoot(value,bytes)==b::Result::Ok&&Store().Commit(before,bytes,out)==c::Result::Ok,"external fixture participant publishes real CAS change");}
};

void PersistPolicyWhileOpen(const Fixture& fixture,std::uint32_t limitMiB,bool enabled){
    namespace p=mikumikustudio::cache_policy;
    p::Store store(fixture.root,fixture.base/L"settings.json");p::Snapshot before,after,verified;
    Check(store.Read(before)==p::Result::Ok,"read real policy for concurrent persisted change");auto value=before.Value();value.limitMiB=limitMiB;value.enabled=enabled;
    Check(store.Commit(before,value,after)==p::Result::Ok&&store.Read(verified)==p::Result::Ok&&verified.Value().limitMiB==limitMiB&&verified.Value().enabled==enabled,"direct policy CAS publishes and verifies actual concurrent setting without manager reentry");
}
struct Fault{std::wstring prefix;bool partial=false,retainFailed=false;unsigned hits=0,cleanupHits=0;bool Match(HANDLE h)const noexcept{wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);return n>prefix.size()&&n<32768&&!_wcsnicmp(path,prefix.c_str(),prefix.size());}};
thread_local Fault* fault=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&WriteFile) realWrite=nullptr;decltype(&SetFileInformationByHandle) realSet=nullptr;
BOOL WINAPI FlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();if(result&&fault&&!fault->partial&&!fault->hits&&fault->Match(h)){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{LARGE_INTEGER zero{},position{};if(fault&&fault->partial&&!fault->hits&&!overlap&&count>1&&fault->Match(h)&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto result=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(result){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return result;}return realWrite(h,bytes,count,done,overlap);}
BOOL WINAPI SetHook(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID value,DWORD bytes)noexcept{
    if(fault&&fault->retainFailed&&fault->hits&&kind==FileDispositionInfo&&bytes>=sizeof(FILE_DISPOSITION_INFO)&&static_cast<FILE_DISPOSITION_INFO*>(value)->DeleteFile&&fault->Match(handle)){
        ++fault->cleanupHits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realSet(handle,kind,value,bytes);
}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&SetHook),reinterpret_cast<void**>(&realSet))==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"inject only exact real catalog file failure");}~Hooks(){fault=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
std::wstring Prefix(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t text[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,text,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"bind fault to actual fixture commit directory");return std::wstring(text,n)+L'\\';}
using Recovery=m::SourceRecoveryAdmission;
std::unique_ptr<Recovery> RecoverOpen(){std::unique_ptr<Recovery> session;Check(Recovery::Open(session)==R::Ok&&session&&!session->IsConsumed(),"open explicit source recovery with real manager and control exclusion");return session;}
struct Pending {c::Snapshot before;bool partial;std::uint64_t bytes;};
Pending Interrupt(Fixture& fixture,bool partial,bool approved=true,std::uint64_t bytes=8*MiB){
    const auto before=fixture.Head();auto desired=fixture.initial;desired.sources.operations.push_back(Reserved(1,bytes));
    Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"commits");injected.partial=partial;
    auto output=before;
    if(approved){auto session=Open();fault=&injected;const auto result=session->Commit(before,desired.sources,output);fault=nullptr;
        Check(result==R::Indeterminate&&session->IsConsumed(),"real approved publication becomes indeterminate after actual commit I/O failure");}
    else {Bytes encoded;Check(b::EncodeBusinessRoot(desired,encoded)==b::Result::Ok,"encode ordinary unapproved pending source metadata");fault=&injected;const auto result=fixture.Store().Commit(before,encoded,output);fault=nullptr;
        Check(result==c::Result::Indeterminate,"real ordinary catalog publication becomes indeterminate without manager receipt");}
    Check(injected.hits==1&&Same(output,before),"exactly one real fault leaves caller snapshot unchanged");
    {c::WritePlan plan;Check(fixture.Store().PlanRecovery(plan)==c::Result::Ok&&bool(plan.RecoveryApproval())==approved&&
        plan.Recovery()==(partial?c::RecoveryAction::Rollback:c::RecoveryAction::ConfirmPublication),"actual persisted operation produces intended recovery disposition and approval presence");}
    return {before,partial,bytes};
}
void Finish(Recovery& session,const Pending& pending){
    const auto action=pending.partial?c::RecoveryAction::Rollback:c::RecoveryAction::ConfirmPublication;
    Check(session.Action()==action&&session.Target().IsValid()&&session.Target().Credit().rawReservedBytes==(pending.partial?0:pending.bytes),"recovery target describes exact surviving obligation rather than current ambiguous head");
    c::RecoveryReport report;report.revision=777;
    Check(session.Execute(report)==R::Ok&&session.IsConsumed()&&report.rolledBack==pending.partial&&report.confirmedPublication!=pending.partial&&
        report.revision==(pending.partial?pending.before.Revision():pending.before.Revision()+1),"execute one exact approved recovery and publish matching disposition");
    Check(!session.Target().IsValid(),"consumed recovery does not expose stale budget facts");
    const auto retained=report;
    Check(session.Execute(report)!=R::Ok&&report.revision==retained.revision&&report.rolledBack==retained.rolledBack&&report.confirmedPublication==retained.confirmedPublication,"consumed recovery cannot replay or change caller report");
}
void Verify(Fixture& fixture,const Pending& pending){
    const auto head=fixture.Head();Check(pending.partial?Same(head,pending.before):head.Revision()==pending.before.Revision()+1,"actual post-recovery catalog is original head or exact completed commit");
    auto session=Open();Check(session->Current().Credit().rawReservedBytes==(pending.partial?0:pending.bytes),"ordinary admission reconstructs actual residual responsibility after recovery scope is closed");
}
void BasicAndExclusion(const fs::path& root){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"rollback":L"confirm"));const auto pending=Interrupt(fixture,partial);const auto evidence=CatalogFiles(fixture.Catalog());
    auto recovery=RecoverOpen();Check(CatalogFiles(fixture.Catalog())==evidence,"opening recovery never publishes, removes or rewrites catalog files");
    std::unique_ptr<Recovery> second;std::unique_ptr<A> normal;
    Check(Recovery::Open(second)==R::Busy&&!second&&A::Open(normal)==R::Busy&&!normal&&!m::WriteReservation(Entry(fixture.root,10),MiB),"recovery scope excludes nested recovery, ordinary source approval and legacy writer");
    std::unique_ptr<b::SourceCapture> capture;Check(fixture.journal->CreateCapture(Id<b::OperationId>(44),Id<b::SourceId>(144),MiB,capture)==b::JournalResult::Ok&&capture->Reserve()==b::JournalResult::Busy,"existing journal cannot mutate recovery target while control is retained");
    c::RecoveryReport report;report.revision=777;R wrong=R::Ok;bool targetVisible=true;
    std::thread worker([&]{targetVisible=recovery->Target().IsValid();wrong=recovery->Execute(report);});worker.join();
    Check(wrong==R::Thread&&!targetVisible&&!recovery->IsConsumed()&&report.revision==777&&CatalogFiles(fixture.Catalog())==evidence,"wrong-thread recovery preserves report, disk and usable session");
    Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);
}}
void PrepaidDespitePolicy(const fs::path& root){for(bool partial:{false,true})for(bool disabled:{false,true}){
    Fixture fixture(root/(L"prepaid-"+std::to_wstring(partial)+L"-"+std::to_wstring(disabled)));Policy(fixture.root,128*MiB);
    const auto pending=Interrupt(fixture,partial,true,80*MiB);PersistPolicyWhileOpen(fixture,64,!disabled);
    auto recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);
}}
void UnknownChargeSurvives(const fs::path& root){Fixture fixture(root/L"unknown-charge");const auto pending=Interrupt(fixture,true);
    // Unknown post-approval bytes are still inventory obligations. They cannot
    // revoke the bounded cleanup already approved for this particular intent.
    Sparse(fixture.root/L"unknown.bin",80*MiB);PersistPolicyWhileOpen(fixture,64,false);
    auto recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();
    Check(fs::file_size(fixture.root/L"unknown.bin")==80*MiB,"approved rollback does not delete foreign post-approval inventory");
    const auto inventory=m::Inspect();Check(inventory.error.empty()&&inventory.totalBytes>=80*MiB&&!m::WriteReservation(Entry(fixture.root,1),MiB),"reopened total still charges unknown actual bytes and grants no fresh writer quota");Verify(fixture,pending);
}
void OrdinaryPolicy(const fs::path& root){
    {Fixture fixture(root/L"ordinary-rollback");const auto pending=Interrupt(fixture,true,false);PersistPolicyWhileOpen(fixture,64,false);auto recovery=RecoverOpen();const auto evidence=CatalogFiles(fixture.Catalog());c::RecoveryReport report;report.revision=777;
        Check(recovery->Execute(report)==R::Limit&&!recovery->IsConsumed()&&report.revision==777&&CatalogFiles(fixture.Catalog())==evidence,"unapproved rollback cannot manufacture a paid decision while policy is disabled");
        PersistPolicyWhileOpen(fixture,64,true);Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);}
    {Fixture fixture(root/L"ordinary-confirm");const auto pending=Interrupt(fixture,false,false);PersistPolicyWhileOpen(fixture,64,false);auto recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);}
    {Fixture fixture(root/L"ordinary-over-limit");const auto pending=Interrupt(fixture,true,false);Sparse(fixture.root/L"unknown.bin",80*MiB);auto recovery=RecoverOpen();const auto evidence=CatalogFiles(fixture.Catalog());c::RecoveryReport report;report.revision=777;
        Check(recovery->Execute(report)==R::Limit&&!recovery->IsConsumed()&&report.revision==777&&CatalogFiles(fixture.Catalog())==evidence,"unapproved recovery decision includes unknown physical inventory in its fresh quota request");
        PersistPolicyWhileOpen(fixture,128,true);Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);}
}
void NoopAndMissing(const fs::path& root){
    const auto missing=root/L"missing";Fresh(missing);const auto cache=missing/L"Cache";Policy(cache);std::unique_ptr<Recovery> output;
    Check(Recovery::Open(output)!=R::Ok&&!output&&!fs::exists(cache/L"Video"),"recovery does not initialize missing Video storage");
    Fixture fixture(root/L"noop");PersistPolicyWhileOpen(fixture,64,false);const auto before=fixture.Head();const auto evidence=CatalogFiles(fixture.Catalog());auto recovery=RecoverOpen();c::RecoveryReport report;
    Check(recovery->Action()==c::RecoveryAction::Noop&&recovery->Execute(report)==R::Ok&&!report.confirmedPublication&&!report.rolledBack&&CatalogFiles(fixture.Catalog())==evidence,"clean no-op recovery works disabled without disk mutation or invented publication");recovery.reset();Check(Same(fixture.Head(),before),"no-op retains actual head");
    Policy(fixture.root);m::WriteReservation writer(Entry(fixture.root,4),MiB);Check(bool(writer)&&Recovery::Open(output)==R::Busy&&!output,"existing ordinary writer excludes recovery even for clean catalog");
}
void CopiedIdentity(const fs::path& root){Fixture fixture(root/L"copied-source");Interrupt(fixture,true);const auto copiedBase=root/L"copied-destination";Fresh(copiedBase);const auto copied=copiedBase/L"Cache";
    fs::copy(fixture.root,copied,fs::copy_options::recursive);Select(copied);const auto evidence=CatalogFiles(copied/L"Video"/L"control"/L"catalog");std::unique_ptr<Recovery> recovery;
    Check(Recovery::Open(recovery)!=R::Ok&&!recovery&&CatalogFiles(copied/L"Video"/L"control"/L"catalog")==evidence,"copied bytes cannot transfer approval to different physical directories");
}
void ExternalTargetPin(const fs::path& root){Fixture fixture(root/L"external-target-pin");const auto pending=Interrupt(fixture,false);auto recovery=RecoverOpen();f::Directory directory;f::ReadLease external;
    Check(f::Directory::Open(fixture.Catalog()/L"snapshots",false,directory)==f::Result::Ok,"open actual candidate payload directory beside recovery plan");std::vector<std::wstring> names;
    Check(directory.Enumerate(128,names)==f::Result::Ok,"enumerate actual verified snapshot generations");
    for(const auto& name:names){f::ReadLease candidate;Bytes bytes;Check(directory.Inspect(name,fixture.initial.legacy.storeOwner,c::MaximumSnapshotBytes,candidate)==f::Result::Ok&&candidate.ReadAll(bytes)==f::Result::Ok,"inspect candidate payload while its plan holds a compatible read pin");
        if(bytes==recovery->Target().Business().Source().Bytes()){Check(!external.IsValid(),"fixture target payload is unique");external=std::move(candidate);}}
    Check(external.IsValid(),"hold separate actual candidate read lease without making another exclusive recovery observation");
    const auto evidence=CatalogFiles(fixture.Catalog());c::RecoveryReport report;report.revision=777;
    Check(recovery->Execute(report)==R::Busy&&recovery->IsConsumed()&&report.revision==777&&CatalogFiles(fixture.Catalog())==evidence,"external pin prevents exclusive confirm after internal pin transfer, consuming attempt without mutation or output changes");
    recovery.reset();external={};recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);
}
void MissingLocks(const fs::path& root){for(bool control:{false,true}){Fixture fixture(root/(control?L"missing-control-lock":L"missing-catalog-lock"));const auto pending=Interrupt(fixture,true);const auto lock=(control?fixture.root/L"Video"/L"control":fixture.Catalog())/L".lock";
    Check(DeleteFileW(lock.c_str())!=FALSE,"remove only owned idle fixture lock before recovery");const auto evidence=CatalogFiles(fixture.Catalog());std::unique_ptr<Recovery> recovery;
    Check(Recovery::Open(recovery)!=R::Ok&&!recovery&&!fs::exists(lock)&&CatalogFiles(fixture.Catalog())==evidence,"failed recovery Open never recreates missing catalog or control lock and leaves operation intact");
    f::Directory directory;f::FileLock restored;Check(f::Directory::Open(lock.parent_path(),false,directory)==f::Result::Ok&&directory.Lock(0,restored)==f::Result::Ok,"explicit fixture owner restores missing exclusion file");restored={};directory={};
    recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);
}}
void OuterManagerIdentity(const fs::path& root){Fixture fixture(root/L"outer-manager-identity");const auto pending=Interrupt(fixture,true);const auto evidence=CatalogFiles(fixture.Catalog());
    // Move the original complete Video directory, preserving every catalog,
    // page and raw identity. Only the containing manager identity changes.
    fixture.journal.reset();fixture.raw={};fixture.pages={};fixture.control={};fixture.video={};fixture.manager={};
    const auto otherBase=root/L"other-manager";Fresh(otherBase);const auto other=otherBase/L"Cache";Policy(other);
    Check(MoveFileExW((fixture.root/L"Video").c_str(),(other/L"Video").c_str(),0)!=FALSE,"move original Video subtree without copying any identity-bearing content");
    {c::Store catalog(other/L"Video"/L"control"/L"catalog");c::WritePlan plan;Check(catalog.PlanRecoveryExistingLocks(plan)==c::Result::Ok&&plan.RecoveryApproval(),"catalog-level identity remains valid after only manager parent changes");}
    std::unique_ptr<Recovery> recovery;Check(Recovery::Open(recovery)!=R::Ok&&!recovery&&CatalogFiles(other/L"Video"/L"control"/L"catalog")==evidence,"manager recovery rejects persisted outer identity mismatch despite valid unchanged catalog, pages and raw");
    Check(MoveFileExW((other/L"Video").c_str(),(fixture.root/L"Video").c_str(),0)!=FALSE,"restore exact original Video namespace after refused opening releases all pins");Select(fixture.root);
    recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);
}
void RecoveryWriteFailures(const fs::path& root){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"recovery-decision-partial":L"recovery-decision-flush"));const auto pending=Interrupt(fixture,true);auto recovery=RecoverOpen();const auto before=CatalogFiles(fixture.Catalog());
    Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"operations");injected.partial=partial;injected.retainFailed=true;c::RecoveryReport report;report.revision=777;report.confirmedPublication=true;
    fault=&injected;const auto result=recovery->Execute(report);fault=nullptr;
    Check(result!=R::Ok&&injected.hits==1&&injected.cleanupHits==1&&recovery->IsConsumed()&&report.revision==777&&report.confirmedPublication&&!report.rolledBack,"actual recovery decision I/O plus disposal failure consumes session and preserves report");
    const auto after=CatalogFiles(fixture.Catalog());Check(after.size()==before.size()+1,"failed recovery retains exactly one new decision file without deleting candidates or intent");
    for(const auto& old:before){const auto found=after.find(old.first);Check(found!=after.end()&&found->second==old.second,"failed decision never alters old operation, candidates or previous metadata");}
    std::size_t retainedBytes=0;for(const auto& file:after)if(!before.count(file.first)){Check(file.first.find(L"operations\\")==0,"new residual belongs only to exact operations namespace");retainedBytes=file.second.size();}
    Check(retainedBytes==(partial?145u:1136u),"actual retained decision is the injected partial body or complete approved record");
    std::fprintf(stderr,"DIAG recovery decision partial=%u retained=%zu write_or_flush_hits=%u cleanup_hits=%u\n",partial?1u:0u,retainedBytes,injected.hits,injected.cleanupHits);
    recovery.reset();PersistPolicyWhileOpen(fixture,64,false);
    if(partial){c::WritePlan plan;Check(fixture.Store().PlanRecoveryExistingLocks(plan)!=c::Result::Ok&&!plan.IsValid(),"partial decision is not a recoverable approval or rollback authority");
        std::unique_ptr<Recovery> refused;Check(Recovery::Open(refused)!=R::Ok&&!refused&&CatalogFiles(fixture.Catalog())==after,"fresh manager recovery refuses unknown partial decision without deleting or rewriting evidence");
        Check(!m::WriteReservation(Entry(fixture.root,99),MiB)&&CatalogFiles(fixture.Catalog())==after,"unresolved partial decision cannot refund capacity to an ordinary writer");
    }else{{c::WritePlan plan;Check(fixture.Store().PlanRecoveryExistingLocks(plan)==c::Result::Ok&&plan.Recovery()==c::RecoveryAction::Rollback&&plan.RecoveryApproval()&&plan.Footprint().count==0,"complete surviving approved decision plans zero additional file allocation");}
        recovery=RecoverOpen();Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);}
}}
void ChildRecover(const fs::path& root,const wchar_t* mode=L"--recover-child");
void EarlierMaintenanceCredit(const fs::path& root){for(bool partial:{false,true})for(bool malformed:{false,true}){
    Fixture fixture(root/(L"earlier-credit-"+std::to_wstring(partial)+L"-"+std::to_wstring(malformed)));
    const auto before=fixture.Head();auto next=fixture.initial;next.sources.operations.push_back(Reserved(1,8*MiB));
    b::SourceBudgetSnapshot facts;Check(b::ReadSourceBudget(fixture.manager,Unit(fixture.root),facts)==b::SourceBudgetResult::Ok,"read actual source before reconstructing earlier maintenance-credit approval");
    b::SourceCreditRequirement proposed;Check(facts.Project(next.sources,proposed)==b::SourceBudgetResult::Ok,"derive proposed complete source obligations");
    c::WriteFootprint oldMaintenance;Check(c::MaximumCheckpointFootprint(oldMaintenance)==c::Result::Ok,"obtain unchanged original one-file maintenance protocol");
    const auto unit=Unit(fixture.root);std::uint64_t oldStep=0;for(unsigned i=0;i<oldMaintenance.count;++i)oldStep+=Round(oldMaintenance.files[i].maximumFileBytes,unit);
    c::PublicationApproval approval;approval.allocationUnit=unit;
    const wchar_t* paths[]={L"",L"Video",L"Video/control",L"Video/control/catalog",L"Video/pages",L"Video/raw",L"Video/control/catalog/snapshots",L"Video/control/catalog/commits",L"Video/control/catalog/operations"};
    for(unsigned i=0;i<approval.directories.size();++i){f::Directory directory;const auto path=i?fixture.root/paths[i]:fixture.root;
        Check(f::Directory::Open(path,false,directory)==f::Result::Ok&&directory.GetIdentity(approval.directories[i])==f::Result::Ok,"bind earlier receipt to actual present directory identities");}
    approval.physicalBytes=m::Inspect().totalBytes;approval.beforeMetadata=facts.Credit().pendingMetadataBytes+oldStep*facts.Credit().checkpointPublications;
    approval.afterRawResidual=8*MiB;approval.afterMetadata=proposed.pendingMetadataBytes+oldStep*proposed.checkpointPublications;
    if(malformed){if(partial)approval.beforeMetadata-=unit;else approval.afterMetadata-=unit;}
    approval.policyEnabled=true;approval.policyLimitBytes=Limit;
    Bytes bytes;Check(b::EncodeBusinessRoot(next,bytes)==b::Result::Ok,"encode earlier-format approved publication");c::WritePlan plan;
    Check(fixture.Store().PlanApprovedCommit(before,bytes,plan)==c::Result::Ok,"plan exact ordinary approved candidate");
    approval.approvedPeak=approval.physicalBytes+approval.afterRawResidual+approval.afterMetadata;
    for(unsigned i=0;i<plan.Footprint().count;++i)approval.approvedPeak+=Round(plan.Footprint().files[i].maximumFileBytes,unit);
    auto output=before;Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"commits");injected.partial=partial;
    fault=&injected;const auto result=fixture.Store().ExecuteApproved(plan,approval,output);fault=nullptr;
    Check(result==c::Result::Indeterminate&&injected.hits==1&&Same(output,before),"persist real earlier-credit receipt through actual failed publication");
    PersistPolicyWhileOpen(fixture,64,false);std::unique_ptr<Recovery> recovery;const auto evidence=CatalogFiles(fixture.Catalog());
    if(malformed){Check(Recovery::Open(recovery)==R::RecoveryRequired&&!recovery&&CatalogFiles(fixture.Catalog())==evidence,"arbitrary smaller metadata claim is not mistaken for the known old credit derivation");}
    else{recovery=RecoverOpen();Check(recovery->Target().Credit().metadataBytes>(partial?approval.beforeMetadata:approval.afterMetadata),"current ledger charges new complete maintenance despite old bounded recovery permission");
        const Pending pending{before,partial,8*MiB};Finish(*recovery,pending);recovery.reset();Verify(fixture,pending);
        ChildRecover(fixture.root,L"--legacy-credit-child");
        auto upgrade=Open();const auto oldHead=upgrade->Current().Business().Source();auto upgraded=oldHead;
        Check(upgrade->Current().Business().Sources().maintenanceProtocol==1&&
            upgrade->Current().Credit().fundedMetadataBytes<upgrade->Current().Credit().metadataBytes,
            "reopened old registry keeps persistent original paid credit separate from new required bytes");
        Check(upgrade->Checkpoint(oldHead,upgraded)==R::Limit&&!upgrade->IsConsumed(),
            "old paid credit cannot silently finance the new checkpoint protocol while disabled");
        Check(upgrade->Commit(oldHead,upgrade->Current().Business().Sources(),upgraded)==R::Limit&&!upgrade->IsConsumed()&&upgraded.SameVersion(oldHead),
            "disabled policy refuses same-data maintenance upgrade without invented credit");
        PersistPolicyWhileOpen(fixture,64,true);
        Check(upgrade->Commit(oldHead,upgrade->Current().Business().Sources(),upgraded)==R::Ok,
            "current policy explicitly funds and durably publishes new credit version with unchanged source data");upgrade.reset();
        PersistPolicyWhileOpen(fixture,64,false);upgrade=Open();
        Check(upgrade->Current().Business().Sources().maintenanceProtocol==2&&
            upgrade->Current().Credit().fundedMetadataBytes==upgrade->Current().Credit().metadataBytes&&
            upgrade->Checkpoint(upgraded,upgraded)==R::Ok,
            "only persisted approved upgrade permits a later disabled prepaid checkpoint");
    }
}}
void ChildRecover(const fs::path& root,const wchar_t* mode){wchar_t exe[32768]{};const auto n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"resolve recovery child executable");auto command=L"\""+std::wstring(exe,n)+L"\" "+mode+L" \""+root.native()+L"\"";STARTUPINFOW start{};start.cb=sizeof start;PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&process),"launch new process with no in-memory publication approval");Handle child(process.hProcess),thread(process.hThread);const auto wait=WaitForSingleObject(child.value,30000);if(wait!=WAIT_OBJECT_0){TerminateProcess(child.value,99);WaitForSingleObject(child.value,5000);}DWORD exit=999;
    Check(wait==WAIT_OBJECT_0&&GetExitCodeProcess(child.value,&exit)&&exit==0,"fresh process recovers disabled-policy transaction from bound durable approval");
}
void Restart(const fs::path& root){for(bool partial:{false,true}){Fixture fixture(root/(partial?L"restart-rollback":L"restart-confirm"));const auto pending=Interrupt(fixture,partial);PersistPolicyWhileOpen(fixture,64,false);ChildRecover(fixture.root);Verify(fixture,pending);}}
} // namespace
int wmain(int argc,wchar_t**argv){try{
    if(argc==3&&std::wstring(argv[1])==L"--legacy-credit-child"){
        Select(argv[2]);auto session=Open();const auto& facts=session->Current();const auto before=facts.Business().Source();auto out=before;
        Check(facts.Business().Sources().maintenanceProtocol==1&&facts.Credit().fundedMetadataBytes<facts.Credit().metadataBytes,
            "fresh process reconstructs old paid credit from persisted protocol rather than prior session memory");
        Check(session->Checkpoint(before,out)==R::Limit&&session->Commit(before,facts.Business().Sources(),out)==R::Limit&&out.SameVersion(before),
            "fresh process cannot fund new maintenance or credit upgrade from disabled old allowance");return 0;
    }
    if(argc==3&&std::wstring(argv[1])==L"--recover-child"){Select(argv[2]);auto recovery=RecoverOpen();c::RecoveryReport report;Check(recovery->Execute(report)==R::Ok&&(report.rolledBack||report.confirmedPublication),"child performs one actual recovery");return 0;}
    Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_source_recovery_admission_tests --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;
    BasicAndExclusion(root);PrepaidDespitePolicy(root);UnknownChargeSurvives(root);OrdinaryPolicy(root);NoopAndMissing(root);CopiedIdentity(root);ExternalTargetPin(root);MissingLocks(root);OuterManagerIdentity(root);RecoveryWriteFailures(root);EarlierMaintenanceCredit(root);Restart(root);
    std::printf("PASS cache source recovery admission checks=%u\n",checks);return 0;
}catch(const std::exception& error){fault=nullptr;std::fprintf(stderr,"FAIL cache source recovery admission checks=%u: %s\n",checks,error.what());return 1;}}
