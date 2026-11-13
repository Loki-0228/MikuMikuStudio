// MAINTENANCE: regression-keep. Real checkpoint capacity, durable approval and recovery admission.
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
c::Snapshot Seed(Fixture& fixture,std::uint64_t bytes=8*MiB){auto session=Open();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,bytes));c::Snapshot head;
    Check(session->Commit(session->Current().Business().Source(),next,head)==R::Ok,"publish real source responsibility before checkpoint test");return head;}
void CreditSizing(Fixture& fixture){auto session=Open();const auto credit=session->Current().Credit();c::WriteFootprint maximum;
    Check(c::MaximumApprovedCheckpointFootprint(maximum)==c::Result::Ok&&maximum.count==3,"approved checkpoint reserves one candidate plus its intent and rollback decision");std::uint64_t expected=0;const auto unit=Unit(fixture.root);for(unsigned n=0;n<maximum.count;++n)expected+=Round(maximum.files[n].maximumFileBytes,unit);
    Check(credit.checkpointBytesPerStep==expected&&credit.checkpointPublications==5,"source future credit uses separately rounded complete approved checkpoint protocol");}
void RetireAndResume(Fixture& fixture,const c::Snapshot& checkpoint){auto session=Open();Check(session->Current().Business().Source().SameVersion(checkpoint),"reopen exact checkpoint before authorized collection");c::CollectionReport report;
    Check(session->Collect(report)==R::Ok&&report.absent>0&&!report.failed&&!report.pending,"explicit checkpoint retirement confirms actual deletion before further publication");session.reset();session=Open();auto next=session->Current().Business().Sources();c::Snapshot after;
    Check(session->Commit(checkpoint,next,after)==R::Ok&&!after.IsCheckpoint()&&after.Revision()==checkpoint.Revision()+1,"normal publication resumes using collected actual catalog and restores ordinary head");session.reset();session=Open();Check(session->Current().Credit().checkpointPublications==5,"next normal publication restores standby rather than reusing consumed checkpoint discount");}
void RealCapacityBoundary(const fs::path& root){Fixture fixture(root/L"capacity");c::Snapshot pinned;f::ReadLease original;
    Check(fixture.Store().ReadPinned(pinned,original)==c::Result::Ok,"retain old real snapshot before chain fills");const auto pinnedFile=fixture.Catalog()/L"snapshots"/original.Info().name;auto head=Seed(fixture);CreditSizing(fixture);
    while(head.Revision()<c::MaximumCommits){auto session=Open();auto next=session->Current().Business().Sources();c::Snapshot out;Check(session->Commit(head,next,out)==R::Ok,"append one real admitted commit toward actual chain bound");head=out;}
    auto session=Open();auto next=session->Current().Business().Sources();auto out=head;const auto files=CatalogFiles(fixture.Catalog());const auto beforeCredit=session->Current().Credit();
    Check(session->Commit(head,next,out)==R::Limit&&!session->IsConsumed()&&Same(out,head)&&CatalogFiles(fixture.Catalog())==files,"64-commit boundary rejects ordinary publication without implicit checkpoint or writes");
    Check(session->Checkpoint(head,out)==R::Ok&&out.IsCheckpoint()&&session->IsConsumed(),"same live unconsumed admission performs explicit approved checkpoint at full chain boundary");session.reset();
    const auto checkpointFiles=CatalogFiles(fixture.Catalog());for(const auto& old:files){const auto found=checkpointFiles.find(old.first);Check(found!=checkpointFiles.end()&&found->second==old.second,"checkpoint retains every old generation until explicit collection, no metadata refund by declaration");}
    Check(checkpointFiles.size()==files.size()+1,"successful checkpoint publishes only one permanent candidate and settles temporary intent");
    session=Open();const auto after=session->Current().Credit();Check(after.checkpointPublications==4&&beforeCredit.metadataBytes-after.metadataBytes==beforeCredit.checkpointBytesPerStep,"confirmed checkpoint consumes exactly one complete standby allowance");
    c::CollectionReport collected;Check(session->Collect(collected)==R::Ok&&collected.busy>=1&&collected.absent>0&&fs::exists(pinnedFile),"collection cannot refund a retired payload still held by actual reader");session.reset();Bytes held;
    Check(original.ReadAll(held)==f::Result::Ok&&held==pinned.Bytes(),"retired snapshot remains readable after other generations are removed");original={};RetireAndResume(fixture,out);Check(!fs::exists(pinnedFile),"final lease release permits exact retired generation removal");
}
void InterruptedCheckpoint(const fs::path& root){for(bool partial:{false,true})for(bool disabled:{false,true}){
    Fixture fixture(root/(L"interrupted-"+std::to_wstring(partial)+L"-"+std::to_wstring(disabled)));Policy(fixture.root,128*MiB);const auto head=Seed(fixture,80*MiB);auto session=Open();const auto credit=session->Current().Credit();auto output=head;
    const auto files=CatalogFiles(fixture.Catalog());Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"commits");injected.partial=partial;fault=&injected;const auto result=session->Checkpoint(head,output);fault=nullptr;
    Check(injected.hits==1&&result==R::Indeterminate&&session->IsConsumed()&&Same(output,head),"real checkpoint candidate failure consumes admitted attempt and preserves previous caller snapshot");session.reset();
    {c::WritePlan plan;Check(fixture.Store().PlanRecoveryExistingLocks(plan)==c::Result::Ok&&plan.RecoveryIsCheckpoint()&&plan.RecoveryApproval(),"persisted checkpoint intent exposes verified checkpoint recovery and durable manager approval");
        const auto* approval=plan.RecoveryApproval();Check(approval->beforeRawResidual==80*MiB&&approval->afterRawResidual==80*MiB&&approval->beforeMetadata==credit.metadataBytes&&approval->beforeMetadata-approval->afterMetadata==credit.checkpointBytesPerStep,"checkpoint receipt retains raw responsibility and funds exactly one standby consumption");
        Check(plan.Recovery()==(partial?c::RecoveryAction::Rollback:c::RecoveryAction::ConfirmPublication)&&plan.RecoveryTarget().IsCheckpoint()==!partial,"actual candidate completeness chooses checkpoint confirmation or original ordinary head");}
    PersistPolicyWhileOpen(fixture,64,!disabled);auto recovery=RecoverOpen();const auto beforeRecovery=CatalogFiles(fixture.Catalog());
    Check(recovery->Target().Credit().rawReservedBytes==80*MiB&&recovery->Target().Credit().checkpointPublications==(partial?5u:4u),"recovery target retains eighty MiB responsibility and action-specific standby facts above lowered policy");c::RecoveryReport report;
    Check(recovery->Execute(report)==R::Ok&&recovery->IsConsumed()&&report.rolledBack==partial&&report.confirmedPublication!=partial,"already approved checkpoint recovers under lower or disabled current policy");recovery.reset();const auto actual=fixture.Head();
    Check(partial?actual.SameVersion(head):(actual.IsCheckpoint()&&actual.Revision()==head.Revision()+1&&actual.Bytes()==head.Bytes()),"recovery publishes no new business data and preserves exact rollback head or checkpoint payload");
    const auto recovered=CatalogFiles(fixture.Catalog());for(const auto& old:files){const auto found=recovered.find(old.first);Check(found!=recovered.end()&&found->second==old.second,"checkpoint recovery cannot collect old retired generations or silently refund them");}
    Check(recovered.size()==files.size()+(partial?0u:1u)&&beforeRecovery.size()>recovered.size(),"recovery settles only transaction metadata and the exact failed candidate");
    auto verified=Open();Check(verified->Current().Credit().metadataBytes==credit.metadataBytes-(partial?0:credit.checkpointBytesPerStep),"reopened source credit applies rollback or one confirmed standby consumption exactly once");verified.reset();
    if(!partial){PersistPolicyWhileOpen(fixture,128,true);RetireAndResume(fixture,actual);}
}}
void IntentWriteFailure(const fs::path& root){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"intent-partial":L"intent-full"));const auto head=Seed(fixture);const auto old=CatalogFiles(fixture.Catalog());auto session=Open();auto output=head;
    Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"operations");injected.partial=partial;injected.retainFailed=true;fault=&injected;const auto result=session->Checkpoint(head,output);fault=nullptr;
    Check(injected.hits==1&&injected.cleanupHits==1&&result!=R::Ok&&session->IsConsumed()&&Same(output,head),"checkpoint intent I/O and cleanup failure consumes attempt without publishing a checkpoint");session.reset();const auto after=CatalogFiles(fixture.Catalog());bool candidate=false,intent=false;
    for(const auto& entry:after)if(!old.count(entry.first)){if(entry.first.find(L"commits\\")==0){candidate=true;Check(entry.second.empty(),"no checkpoint candidate body is written before durable complete intent succeeds");}else if(entry.first.find(L"operations\\")==0)intent=true;else Check(false,"checkpoint must not create a second snapshot candidate");}
    Check(candidate&&intent,"intent failure retains actual empty single candidate and failed intent evidence");
    if(partial){std::unique_ptr<Recovery> refused;Check(Recovery::Open(refused)!=R::Ok&&!refused&&CatalogFiles(fixture.Catalog())==after,"partial checkpoint approval is not guessed or repaired by unified recovery");}
    else {PersistPolicyWhileOpen(fixture,64,false);auto recovery=RecoverOpen();c::RecoveryReport report;Check(recovery->Action()==c::RecoveryAction::Rollback&&recovery->Execute(report)==R::Ok&&report.rolledBack,"complete checkpoint intent funds rollback of its still-empty candidate while disabled");recovery.reset();Check(fixture.Head().SameVersion(head)&&CatalogFiles(fixture.Catalog())==old,"prebody rollback restores exact original head and file evidence");}
}}
c::Snapshot LegacySeed(Fixture& fixture,bool fill){
    auto legacy=fixture.initial;legacy.sources.maintenanceProtocol=1;legacy.sources.operations.push_back(Reserved(1,8*MiB));Bytes bytes;
    Check(b::EncodeBusinessRoot(legacy,bytes)==b::Result::Ok,"encode real old maintenance protocol without new admission upgrade");auto head=fixture.Head();
    do{c::Snapshot next;Check(fixture.Store().Commit(head,bytes,next)==c::Result::Ok,"append genuine v1 catalog publication outside v2 admission upgrade");head=next;}while(fill&&head.Revision()<c::MaximumCommits);
    return head;
}
void LegacyFullChainMigration(const fs::path& root){Fixture fixture(root/L"legacy-full-chain");const auto old=LegacySeed(fixture,true);auto session=Open();const auto credit=session->Current().Credit();
    Check(old.Revision()==c::MaximumCommits&&session->Current().Business().Sources().maintenanceProtocol==1&&credit.fundedMetadataBytes<credit.metadataBytes&&credit.fundedCheckpointBytesPerStep<credit.checkpointBytesPerStep,"full v1 chain distinguishes old paid bytes from larger current requirement");
    auto unchanged=session->Current().Business().Sources();auto output=old;const auto before=CatalogFiles(fixture.Catalog());PersistPolicyWhileOpen(fixture,64,false);
    Check(session->Checkpoint(old,output)==R::Limit&&!session->IsConsumed()&&Same(output,old)&&CatalogFiles(fixture.Catalog())==before,"disabled old chain cannot spend unfunded extra checkpoint intent allowance");
    PersistPolicyWhileOpen(fixture,64,true);
    Check(session->Checkpoint(old,output)==R::Ok&&session->IsConsumed()&&output.IsCheckpoint()&&output.Bytes()==old.Bytes(),"enabled full v1 chain uses freshly approved checkpoint without rewriting its business payload");session.reset();const auto checkpoint=output;session=Open();const auto checkpointCredit=session->Current().Credit();
    Check(session->Current().Business().Sources().maintenanceProtocol==1&&checkpointCredit.metadataBytes==credit.metadataBytes-credit.checkpointBytesPerStep&&checkpointCredit.fundedMetadataBytes==credit.fundedMetadataBytes-credit.fundedCheckpointBytesPerStep&&checkpointCredit.fundedMetadataBytes<checkpointCredit.metadataBytes,"v1 checkpoint consumes old funded standby without granting protocol2 funding to remaining work");
    // Clear the actual catalog retirement pressure before testing the next
    // refusal: the following Limit must come from unpaid upgrade, not count.
    c::CollectionReport collected;Check(session->Collect(collected)==R::Ok&&collected.absent>0&&!collected.failed&&!collected.pending&&!collected.busy,"collect v1 checkpoint generations before isolated upgrade quota check");session.reset();session=Open();unchanged=session->Current().Business().Sources();output=checkpoint;
    const auto collectedFiles=CatalogFiles(fixture.Catalog());PersistPolicyWhileOpen(fixture,64,false);
    Check(session->Commit(checkpoint,unchanged,output)==R::Limit&&!session->IsConsumed()&&Same(output,checkpoint)&&CatalogFiles(fixture.Catalog())==collectedFiles,"unchanged v1 publication still requires newly funded v2 maintenance and is refused while disabled after collection");
    PersistPolicyWhileOpen(fixture,64,true);Check(session->Commit(checkpoint,unchanged,output)==R::Ok&&session->IsConsumed()&&!output.IsCheckpoint(),"same retained session can atomically upgrade unchanged v1 registry once present policy funds all required obligations");session.reset();const auto upgraded=output;session=Open();const auto paid=session->Current().Credit();
    Check(session->Current().Business().Sources().maintenanceProtocol==2&&paid.fundedMetadataBytes==paid.metadataBytes&&paid.fundedCheckpointBytesPerStep==paid.checkpointBytesPerStep&&paid.checkpointPublications==5,"successful upgrade persists protocol2 and fully paid ordinary standby");
    PersistPolicyWhileOpen(fixture,64,false);Check(session->Checkpoint(upgraded,output)==R::Ok&&output.IsCheckpoint(),"new protocol2 can consume already paid checkpoint allowance after policy is disabled");session.reset();
    auto verified=Open();Check(verified->Current().Business().Sources().maintenanceProtocol==2&&verified->Current().Credit().fundedMetadataBytes==paid.fundedMetadataBytes-paid.fundedCheckpointBytesPerStep,"reopened disabled v2 checkpoint retains exact paid credit consumption");
}
void LegacyCheckpointRecovery(const fs::path& root){for(bool partial:{false,true}){
    Fixture fixture(root/(partial?L"legacy-checkpoint-rollback":L"legacy-checkpoint-confirm"));const auto head=LegacySeed(fixture,false);auto session=Open();const auto credit=session->Current().Credit();auto output=head;
    Check(credit.fundedMetadataBytes<credit.metadataBytes&&session->Current().Business().Sources().maintenanceProtocol==1,"real legacy pending work has measurable unpaid maintenance difference");
    Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"commits");injected.partial=partial;fault=&injected;const auto result=session->Checkpoint(head,output);fault=nullptr;
    Check(injected.hits==1&&result==R::Indeterminate&&session->IsConsumed()&&Same(output,head),"interrupted newly approved legacy checkpoint preserves old caller output");session.reset();
    {c::WritePlan plan;Check(fixture.Store().PlanRecoveryExistingLocks(plan)==c::Result::Ok&&plan.RecoveryIsCheckpoint()&&plan.RecoveryApproval(),"legacy checkpoint has actual durable recovery approval");const auto& receipt=*plan.RecoveryApproval();
        Check(receipt.beforeMetadata==credit.fundedMetadataBytes&&receipt.afterMetadata==credit.fundedMetadataBytes-credit.fundedCheckpointBytesPerStep&&receipt.afterMetadata<credit.metadataBytes-credit.checkpointBytesPerStep,"legacy checkpoint receipt records funded endpoint credit without pretending all required future bytes were already funded");}
    PersistPolicyWhileOpen(fixture,64,false);auto recovery=RecoverOpen();const auto targetCredit=recovery->Target().Credit();
    Check(recovery->Target().Business().Sources().maintenanceProtocol==1&&targetCredit.fundedMetadataBytes==credit.fundedMetadataBytes-(partial?0:credit.fundedCheckpointBytesPerStep)&&targetCredit.metadataBytes==credit.metadataBytes-(partial?0:credit.checkpointBytesPerStep),"legacy recovery derives separate required and funded facts at exact rollback or confirmation target");c::RecoveryReport report;
    Check(recovery->Execute(report)==R::Ok&&report.rolledBack==partial&&report.confirmedPublication!=partial&&recovery->IsConsumed(),"approved legacy checkpoint recovers while policy is disabled without upgrading or stranding its paid transition");recovery.reset();const auto actual=fixture.Head();
    Check(actual.Bytes()==head.Bytes()&&(partial?actual.SameVersion(head):actual.IsCheckpoint()),"legacy recovery preserves v1 payload and exact selected head");auto verified=Open();
    Check(verified->Current().Business().Sources().maintenanceProtocol==1&&verified->Current().Credit().fundedMetadataBytes==targetCredit.fundedMetadataBytes&&verified->Current().Credit().metadataBytes==targetCredit.metadataBytes,"reopening legacy recovery retains the same funding deficit rather than granting new credit");
}}
} // namespace
int wmain(int argc,wchar_t** argv){try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_checkpoint_admission_tests --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;
    RealCapacityBoundary(root);InterruptedCheckpoint(root);IntentWriteFailure(root);LegacyFullChainMigration(root);LegacyCheckpointRecovery(root);std::printf("PASS cache checkpoint admission checks=%u\n",checks);return 0;
}catch(const std::exception& error){fault=nullptr;std::fprintf(stderr,"FAIL cache checkpoint admission checks=%u: %s\n",checks,error.what());return 1;}}
