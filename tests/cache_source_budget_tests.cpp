#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "../src/io/cache_source_budget.hpp"
#include "../src/io/cache_source_journal.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
namespace fs=std::filesystem;
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
namespace c=mikumikustudio::cache_catalog;
using R=b::SourceBudgetResult;
using J=b::JournalResult;
using Bytes=b::Bytes;
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value){std::fprintf(stderr,"win32=%lu check=%u\n",GetLastError(),checks);throw std::runtime_error(why);}}
template<class T>T Id(unsigned n){T value{};value[14]=std::uint8_t(n>>8);value[15]=std::uint8_t(n);return value;}
void Fresh(const fs::path& p){Check(p.is_absolute()&&!fs::exists(p),"claim fresh absolute fixture directory");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"create only own new directory");}
void Write(const fs::path& p,const Bytes& bytes){HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(h!=INVALID_HANDLE_VALUE,"create own source once");DWORD done=0;
    const bool good=WriteFile(h,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(h);CloseHandle(h);Check(good,"write exact own source bytes");}
std::map<std::wstring,std::uintmax_t> Files(const fs::path& p){std::map<std::wstring,std::uintmax_t> result;for(const auto& v:fs::recursive_directory_iterator(p))if(v.is_regular_file())result.emplace(v.path().lexically_relative(p).native(),v.file_size());return result;}
std::uint64_t Round(std::uint64_t n,std::uint64_t unit){return (n+unit-1)/unit*unit;}
std::uint64_t MetadataCharge(const fs::path& p){std::uint64_t total=0;for(const auto& v:fs::recursive_directory_iterator(p))if(v.is_regular_file()){
    HANDLE h=CreateFileW(v.path().c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    FILE_STANDARD_INFO info{};const bool ok=h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(h,FileStandardInfo,&info,sizeof info);if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    Check(ok,"observe actual metadata allocation independently");total+=(std::max)(std::uint64_t(info.EndOfFile.QuadPart),std::uint64_t(info.AllocationSize.QuadPart));}return total;}
decltype(&CreateFileW) realCreate=nullptr;
decltype(&WriteFile) realWrite=nullptr;
decltype(&FlushFileBuffers) realFlush=nullptr;
struct RecoveryFault{std::wstring prefix;bool partial=false;unsigned hits=0;
    bool Match(HANDLE h)const noexcept{wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);return n>prefix.size()&&n<32768&&!_wcsnicmp(path,prefix.c_str(),prefix.size());}};
thread_local RecoveryFault* recoveryFault=nullptr;
BOOL WINAPI RecoveryWriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{auto* p=recoveryFault;LARGE_INTEGER zero{},position{};
    if(p&&p->partial&&!p->hits&&!overlap&&count>1&&p->Match(h)&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto result=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(result){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return result;}return realWrite(h,bytes,count,done,overlap);}
BOOL WINAPI RecoveryFlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();auto* p=recoveryFault;if(result&&p&&!p->partial&&!p->hits&&p->Match(h)){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
thread_local bool readOnly=false;
unsigned creationAttempts=0;
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD mode,DWORD flags,HANDLE templateFile){
    if(readOnly&&mode!=OPEN_EXISTING)++creationAttempts;
    return realCreate(path,access,share,security,mode,flags,templateFile);
}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK,"initialize read-only observation hook");Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"observe real file-create dispositions");Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&RecoveryWriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&RecoveryFlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK,"install scoped actual recovery publication faults");Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable scoped hook");}~Hooks(){readOnly=false;recoveryFault=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
R ReadBudget(const f::Directory& root,std::uint64_t unit,b::SourceBudgetSnapshot& out){creationAttempts=0;readOnly=true;const auto result=b::ReadSourceBudget(root,unit,out);readOnly=false;Check(creationAttempts==0,"budget read never requests file creation even transiently");return result;}
void ReadInChild(const fs::path& root,std::uint64_t unit,std::uint64_t raw,bool checkpoint=false){
    wchar_t executable[32768]{};Check(GetModuleFileNameW(nullptr,executable,32768)>0,"locate exact budget test executable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --read-existing \""+root.native()+L"\" "+std::to_wstring(unit)+L" "+std::to_wstring(raw)+L" "+(checkpoint?L"1":L"0");
    STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"start independent durable-credit reader process");
    CloseHandle(process.hThread);const auto waited=WaitForSingleObject(process.hProcess,30000);DWORD code=999;
    if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}
    const bool done=GetExitCodeProcess(process.hProcess,&code)!=FALSE;CloseHandle(process.hProcess);
    Check(waited==WAIT_OBJECT_0&&done&&code==0,"independent process reconstructs exact durable promise without parent memory");
}

struct Fixture{
    fs::path path;f::Directory manager,video,control,pages,raw;std::unique_ptr<b::SourceJournal> journal;
    b::BusinessRoot initial;std::uint64_t unit=0;
    Fixture(const fs::path& parent,const wchar_t* name):path(parent/name){Fresh(path);
        Check(f::Directory::Open(path,false,manager)==f::Result::Ok,"pin manager root");
        Check(manager.Child(L"Video",true,video)==f::Result::Ok&&video.Child(L"control",true,control)==f::Result::Ok&&video.Child(L"pages",true,pages)==f::Result::Ok&&video.Child(L"raw",true,raw)==f::Result::Ok,"create distinct fixed source layout");
        wchar_t mount[32768]{};DWORD sectors=0,bytes=0,free=0,total=0;Check(GetVolumePathNameW(path.c_str(),mount,32768)&&GetDiskFreeSpaceW(mount,&sectors,&bytes,&free,&total)&&sectors&&bytes,"read actual allocation granularity");unit=std::uint64_t(sectors)*bytes;
        initial.legacy.storeOwner=Id<f::Owner>(1);initial.legacy.maintenanceEpoch=1;initial.legacy.nextProjectId=2;
        initial.sources.storeOwner=initial.legacy.storeOwner;initial.sources.maintenanceEpoch=1;
        b::DirectoryBinding binding;binding.id=1;Check(raw.GetIdentity(binding.identity)==f::Result::Ok,"bind actual raw directory");initial.sources.directories.push_back(binding);
        b::Page page;page.storeOwner=initial.legacy.storeOwner;b::Project project;project.id=1;project.normalizedPath=project.displayPath=u"C:\\source-budget\\project.pmm";page.projects.push_back(project);
        Bytes encoded;Check(b::EncodePage(page,encoded)==b::Result::Ok,"encode actual project page");f::Spec spec;spec.owner=page.storeOwner;spec.kind=f::Kind::IndexPage;spec.key=b::PageKey;f::BlobInfo blob;
        Check(pages.Create(spec,encoded.data(),encoded.size(),blob)==f::Result::Ok,"write real checksummed page");b::PageRef ref;ref.recordCount=1;ref.blob=blob;initial.legacy.pages.push_back(ref);initial.legacy.projectCount=1;initial.legacy.liveEncodedBytes=b::ManifestHeaderBytes+b::PageRefBytes+encoded.size();
        Check(b::EncodeBusinessRoot(initial,encoded)==b::Result::Ok,"encode validated initial root");c::Snapshot first;Check(Store().InitializeOwned(initial.legacy.storeOwner,encoded,first)==c::Result::Ok,"initialize actual source catalog");
        Check(b::SourceJournal::Open(control,pages,raw,journal)==J::Ok,"open real journal and establish its control lock");
    }
    c::Store Store()const{return c::Store(path/L"Video"/L"control"/L"catalog");}
    b::SourceBudgetSnapshot Read(){const auto before=Files(path);b::SourceBudgetSnapshot out;Check(ReadBudget(manager,unit,out)==R::Ok&&out.IsValid(),"read verified durable credit");Check(Files(path)==before,"read leaves existing files and sizes unchanged");return out;}
    std::unique_ptr<b::SourceCapture> New(unsigned id,std::uint64_t size){std::unique_ptr<b::SourceCapture> out;Check(journal->CreateCapture(Id<b::OperationId>(id),Id<b::SourceId>(id+100),size,out)==J::Ok,"create actual capture");return out;}
    void Commit(const b::BusinessRoot& root){c::Snapshot before,after;Check(Store().Read(before)==c::Result::Ok,"read mutation baseline");Bytes bytes;Check(b::EncodeBusinessRoot(root,bytes)==b::Result::Ok&&Store().Commit(before,bytes,after)==c::Result::Ok,"publish explicit test root mutation");}
};
void ExistingLock(const fs::path& root){const auto path=root/L"existing-lock";Fresh(path);f::Directory directory;Check(f::Directory::Open(path,false,directory)==f::Result::Ok,"open empty lock fixture");f::FileLock lock;
    Check(directory.LockExisting(0,lock)==f::Result::Missing&&!lock.IsValid()&&fs::is_empty(path),"missing existing-only lock cannot create a file");
    Check(directory.Lock(0,lock)==f::Result::Ok,"create regular byte lock");f::FileLock second;Check(directory.LockExisting(0,second)==f::Result::Busy&&!second.IsValid(),"existing-only follows actual byte lock exclusion");lock={};
    Check(directory.LockExisting(0,second)==f::Result::Ok&&second.IsValid(),"existing-only opens previously created lock");
}
RecoveryFault CommitFault(const fs::path& catalog,bool partial){const auto path=catalog/L"commits";HANDLE handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);wchar_t text[32768]{};const auto size=handle==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(handle,text,32768,VOLUME_NAME_GUID);if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);Check(size&&size<32768,"bind recovery fault to exact real commit directory");RecoveryFault fault;fault.prefix=std::wstring(text,size)+L'\\';fault.partial=partial;return fault;}
R ReadRecovery(const f::Directory& root,std::uint64_t unit,const c::WritePlan& plan,b::SourceBudgetSnapshot& out){creationAttempts=0;readOnly=true;const auto result=b::ReadRecoverySourceBudget(root,unit,plan,out);readOnly=false;Check(!creationAttempts,"recovery budget read requests no file creation, including lock creation");return result;}
void RecoveryEndpoints(const fs::path& root){
    constexpr std::uint64_t MiB=1024ull*1024;
    Fixture sentinel(root,L"recovery-output-sentinel");auto preserved=sentinel.Read();const auto sentinelHead=preserved.Business().Source();const auto sentinelCredit=preserved.Credit();
    const auto unchanged=[&]{return preserved.IsValid()&&preserved.Business().Source().SameVersion(sentinelHead)&&preserved.Credit().rawReservedBytes==sentinelCredit.rawReservedBytes&&preserved.Credit().metadataBytes==sentinelCredit.metadataBytes;};
    for(unsigned mode=0;mode<3;++mode){Fixture fixture(root,(L"recovery-endpoint-"+std::to_wstring(mode)).c_str());auto initial=fixture.initial;b::SourceOperation op;op.id=Id<b::OperationId>(70);op.sourceId=Id<b::SourceId>(170);op.directoryId=1;op.reservedBytes=8*MiB;initial.sources.operations.push_back(op);fixture.Commit(initial);c::Snapshot baseline;Check(fixture.Store().Read(baseline)==c::Result::Ok,"read actual eight MiB baseline");
        const auto beforePending=Files(fixture.path);if(mode){auto proposed=initial;proposed.sources.operations.front().reservedBytes=16*MiB;Bytes bytes;Check(b::EncodeBusinessRoot(proposed,bytes)==b::Result::Ok,"encode actual sixteen MiB candidate root");auto out=baseline;auto fault=CommitFault(fixture.path/L"Video"/L"control"/L"catalog",mode==2);recoveryFault=&fault;const auto result=fixture.Store().Commit(baseline,bytes,out);recoveryFault=nullptr;
            Check(fault.hits==1&&result==c::Result::Indeterminate&&out.SameVersion(baseline),"actual partial or complete failed commit keeps baseline output");}
        f::FileLock control;Check(fixture.control.LockExisting(0,control)==f::Result::Ok,"caller holds existing source control lock before planning and budget read");c::WritePlan plan;Check(fixture.Store().PlanRecovery(plan)==c::Result::Ok,"derive real catalog recovery endpoint");const auto expectedAction=mode==0?c::RecoveryAction::Noop:mode==1?c::RecoveryAction::ConfirmPublication:c::RecoveryAction::Rollback;Check(plan.Recovery()==expectedAction,"actual recovery plan selects expected endpoint kind");const auto pending=Files(fixture.path);b::SourceBudgetSnapshot endpoint;
        Check(ReadRecovery(fixture.manager,fixture.unit,plan,endpoint)==R::Ok&&endpoint.IsValid()&&endpoint.Business().Source().SameVersion(plan.RecoveryTarget())&&endpoint.Credit().rawReservedBytes==(mode==1?16*MiB:8*MiB)&&Files(fixture.path)==pending,"recovery read succeeds under caller control lock and derives only exact target responsibility without filesystem mutation");
        f::FileLock competing;Check(fixture.control.LockExisting(0,competing)==f::Result::Busy,"recovery read neither reacquires nor releases caller control lock");f::InventorySnapshot inventory;f::InventoryLimits limits;Check(fixture.manager.Inventory(limits,inventory)==f::Result::Ok,"observe full physical inventory beside recovery endpoint");
        for(const auto& file:pending){const auto found=std::find_if(inventory.Entries().begin(),inventory.Entries().end(),[&](const auto& e){return e.relativePath==file.first;});Check(found!=inventory.Entries().end()&&found->observed.identity.fileBytes==file.second,"full Inventory retains every physically present pending metadata file independently of target credit");}
        if(mode)Check(pending.size()>beforePending.size(),"interrupted fixture actually retains extra physical metadata generations");
        {f::FileLock sentinelControl;Check(sentinel.control.LockExisting(0,sentinelControl)==f::Result::Ok,"hold wrong-root caller control before testing foreign plan");Check(ReadRecovery(sentinel.manager,sentinel.unit,plan,preserved)==R::RecoveryRequired&&unchanged(),"foreign physical catalog plan cannot replace output with unrelated endpoint facts");}
        endpoint={};c::RecoveryReport report;Check(fixture.Store().ExecuteRecovery(plan,report)==c::Result::Ok,"execute real recovery after endpoint observation");Check(ReadRecovery(fixture.manager,fixture.unit,plan,preserved)==R::Invalid&&unchanged(),"consumed recovery plan is not reusable budget authority");control={};
        auto actual=fixture.Read();Check(actual.Credit().rawReservedBytes==(mode==1?16*MiB:8*MiB)&&actual.Business().Source().SameVersion(plan.RecoveryTarget()),"ordinary verified read after actual recovery agrees with planned endpoint obligation");
        c::WritePlan publication;Check(fixture.Store().PlanCommit(actual.Business().Source(),actual.Business().Source().Bytes(),publication)==c::Result::Ok,"create real ordinary publication plan");Check(fixture.control.LockExisting(0,control)==f::Result::Ok,"reacquire caller control before invalid authority checks");Check(ReadRecovery(fixture.manager,fixture.unit,publication,preserved)==R::Invalid&&unchanged(),"non-recovery plan preserves prior output and grants no recovery facts");c::WritePlan empty;Check(ReadRecovery(fixture.manager,fixture.unit,empty,preserved)==R::Invalid&&unchanged(),"empty plan cannot create budget facts");
    }
    {Fixture fixture(root,L"recovery-bad-page");c::WritePlan plan;Check(fixture.Store().PlanRecovery(plan)==c::Result::Ok,"plan target before actual page damage");const auto path=fixture.path/L"Video"/L"pages"/fixture.initial.legacy.pages.front().blob.name;HANDLE writer=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);const bool damaged=writer!=INVALID_HANDLE_VALUE&&SetEndOfFile(writer)&&FlushFileBuffers(writer);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);Check(damaged,"truncate only this fixture's real referenced page");f::FileLock lock;Check(fixture.control.LockExisting(0,lock)==f::Result::Ok,"hold caller control for damaged page check");const auto before=Files(fixture.path);Check(ReadRecovery(fixture.manager,fixture.unit,plan,preserved)==R::RecoveryRequired&&unchanged()&&Files(fixture.path)==before,"verified catalog endpoint cannot bypass validation of damaged actual business pages");}
    {Fixture fixture(root,L"recovery-wrong-raw");auto wrong=fixture.initial;wrong.sources.directories.front().identity.fileIndex^=1;fixture.Commit(wrong);f::FileLock lock;Check(fixture.control.LockExisting(0,lock)==f::Result::Ok,"hold caller control for raw identity check");c::WritePlan plan;Check(fixture.Store().PlanRecovery(plan)==c::Result::Ok,"catalog may hold structurally valid but physically wrong raw binding");const auto before=Files(fixture.path);Check(ReadRecovery(fixture.manager,fixture.unit,plan,preserved)==R::RecoveryRequired&&unchanged()&&Files(fixture.path)==before,"recovery endpoint must still match actual raw directory identity");}
    {const auto path=root/L"recovery-initial-rollback";Fresh(path);f::Directory manager,video,control,pages,raw;Check(f::Directory::Open(path,false,manager)==f::Result::Ok&&manager.Child(L"Video",true,video)==f::Result::Ok&&video.Child(L"control",true,control)==f::Result::Ok&&video.Child(L"pages",true,pages)==f::Result::Ok&&video.Child(L"raw",true,raw)==f::Result::Ok,"create isolated empty actual source layout");const auto catalog=path/L"Video"/L"control"/L"catalog";Fresh(catalog);Fresh(catalog/L"snapshots");Fresh(catalog/L"commits");c::Store store(catalog);c::Snapshot output;auto fault=CommitFault(catalog,true);recoveryFault=&fault;const auto result=store.InitializeOwned(Id<f::Owner>(1),Bytes(2049,0x55),output);recoveryFault=nullptr;Check(fault.hits==1&&result==c::Result::Indeterminate&&!output.IsValid(),"interrupt actual initial publication before any trustworthy prior endpoint");f::FileLock lock;Check(control.Lock(0,lock)==f::Result::Ok,"establish caller control lock for explicit initial rollback");c::WritePlan plan;Check(store.PlanRecovery(plan)==c::Result::Ok&&plan.Recovery()==c::RecoveryAction::Rollback&&!plan.RecoveryTarget().IsValid(),"initial recovery has explicit absence endpoint");const auto before=Files(path);Check(ReadRecovery(manager,4096,plan,preserved)==R::RecoveryRequired&&unchanged()&&Files(path)==before,"absence endpoint cannot fabricate empty zero-debt source budget or replace prior output");}
}
void Lifecycle(const fs::path& root){Fixture f(root,L"lifecycle");auto baseline=f.Read();Check(baseline.Credit().rawReservedBytes==0&&baseline.Credit().remainingPublications==0,"empty registry has no fabricated payload promise");
    const Bytes source(8193,0x5a);const auto sourcePath=f.path/L"source.mp4";Write(sourcePath,source);auto capture=f.New(2,2*1024*1024);
    Check(capture->Reserve()==J::Ok,"publish real durable reservation");auto reserved=f.Read();
    Check(reserved.Credit().remainingPublications==4&&reserved.Credit().rawReservedBytes==2*1024*1024&&reserved.Credit().rawAllocationReservedBytes==Round(2*1024*1024,f.unit),"Reserved persists four-transition cancel-after-progress credit and complete raw declaration");
    ReadInChild(f.path,f.unit,reserved.Credit().rawReservedBytes);
    auto many=reserved.Business().Sources();const auto prototype=many.operations.front();many.operations.clear();
    for(unsigned i=0;i<16;++i){auto operation=prototype;operation.id=Id<b::OperationId>(20+i);operation.sourceId=Id<b::SourceId>(200+i);many.operations.push_back(operation);}
    b::SourceCreditRequirement manyCredit;Check(reserved.Project(many,manyCredit)==R::Ok&&manyCredit.remainingPublications==64&&manyCredit.checkpointPublications==65,"64 pending transitions allow byte-cap maintenance before every publication and terminal standby");
    b::SourceBudgetSnapshot reopened;Check(ReadBudget(f.manager,f.unit,reopened)==R::Ok&&reopened.Business().Sources().operations.size()==1&&reopened.Credit().rawReservedBytes==reserved.Credit().rawReservedBytes,"independent reread recovers promises from actual catalog");reopened={};
    const auto beforeMetadata=MetadataCharge(f.path/L"Video"/L"control"/L"catalog");
    Check(capture->Prepare(sourcePath.c_str())==J::Ok,"prepare actual fixed raw candidate");auto prepared=f.Read();
    Check(prepared.Credit().remainingPublications==3&&prepared.Credit().rawReservedBytes==source.size(),"Prepared shrinks actual raw size and retains three future transitions");
    Check(capture->Complete()==J::Ok,"copy exact bytes into immutable raw lease");auto complete=f.Read();
    Check(complete.Credit().remainingPublications==2&&complete.Credit().rawReservedBytes==source.size(),"Captured still keeps two transitions for cancellation");
    const auto afterMetadata=MetadataCharge(f.path/L"Video"/L"control"/L"catalog");
    Check(afterMetadata>=beforeMetadata&&afterMetadata-beforeMetadata+complete.Credit().metadataBytes<=reserved.Credit().metadataBytes,"two real complete metadata generations plus remaining credit fit original pending credit");
    auto projected=complete.Business().Sources();projected.operations.front().stage=b::SourceStage::AbortCaptured;b::SourceCreditRequirement abortCredit;
    Check(complete.Project(projected,abortCredit)==R::Ok&&abortCredit.remainingPublications==1,"actual validated abort projection has one remaining publication");
    Check(capture->Cancel()==J::Ok,"real cancellation after successful prepare and copy");auto cancelled=f.Read();
    Check(cancelled.Business().Sources().operations.empty()&&cancelled.Credit().rawReservedBytes==0&&cancelled.Credit().remainingPublications==0,"confirmed cancellation removes durable raw promise");
    Check(MetadataCharge(f.path/L"Video"/L"control"/L"catalog")-beforeMetadata+cancelled.Credit().metadataBytes<=reserved.Credit().metadataBytes,"full worst successful-prefix-then-cancel chain respects reserved metadata credit");
    auto publishedCapture=f.New(3,source.size());Check(publishedCapture->Reserve()==J::Ok&&publishedCapture->Prepare(sourcePath.c_str())==J::Ok&&publishedCapture->Complete()==J::Ok,"prepare independent publish path");f::RawSourceLease lease;
    Check(publishedCapture->Publish({1},77,lease)==J::Ok,"publish source with actual project reference");auto published=f.Read();
    Check(published.Business().Sources().sources.size()==1&&published.Credit().remainingPublications==0&&published.Credit().rawReservedBytes==0&&lease.IsValid(),"published bytes are physical inventory responsibility, not a second raw promise");
    auto bad=published.Business().Sources();bad.sources.front().owners={999};b::SourceCreditRequirement unchanged;unchanged.rawReservedBytes=123;
    Check(published.Project(bad,unchanged)==R::Invalid&&unchanged.rawReservedBytes==123,"projection rejects nonexistent real project and preserves output");
    bad=published.Business().Sources();bad.directories.front().identity.fileIndex^=1;
    Check(published.Project(bad,unchanged)!=R::Ok&&unchanged.rawReservedBytes==123,"projection cannot replace trusted raw binding");
}
void ReadFailures(const fs::path& root){const auto emptyPath=root/L"absent";Fresh(emptyPath);f::Directory empty;Check(f::Directory::Open(emptyPath,false,empty)==f::Result::Ok,"open root without source area");b::SourceBudgetSnapshot none;
    Check(ReadBudget(empty,4096,none)==R::Absent&&!none.IsValid()&&fs::is_empty(emptyPath),"absent source area stays absent");
    f::Directory partial;Check(empty.Child(L"Video",true,partial)==f::Result::Ok,"create explicit partial layout");const auto before=Files(emptyPath);
    Check(ReadBudget(empty,4096,none)==R::RecoveryRequired&&Files(emptyPath)==before,"partial layout is not initialized by a read");
    Fixture f(root,L"read-failures");auto valid=f.Read();const auto revision=valid.Business().Source().Revision();
    Check(ReadBudget(f.manager,0,valid)==R::Invalid&&valid.Business().Source().Revision()==revision,"invalid allocation unit preserves old snapshot");
    {f::FileLock held;Check(f.control.LockExisting(0,held)==f::Result::Ok,"hold real control byte lock");Check(ReadBudget(f.manager,f.unit,valid)==R::Busy&&valid.Business().Source().Revision()==revision,"busy source control cannot be interpreted as no debt");}
    const auto controlLock=f.path/L"Video"/L"control"/L".lock";Check(DeleteFileW(controlLock.c_str())!=FALSE,"remove only own idle lock fixture");
    Check(ReadBudget(f.manager,f.unit,valid)==R::RecoveryRequired&&!fs::exists(controlLock)&&valid.Business().Source().Revision()==revision,"missing control lock is not recreated or treated as empty");
    {f::FileLock restored;Check(f.control.Lock(0,restored)==f::Result::Ok,"explicitly restore own lock for following check");}
    const auto catalogLock=f.path/L"Video"/L"control"/L"catalog"/L".lock";Check(DeleteFileW(catalogLock.c_str())!=FALSE,"remove own catalog lock fixture");
    Check(ReadBudget(f.manager,f.unit,valid)==R::RecoveryRequired&&!fs::exists(catalogLock)&&valid.Business().Source().Revision()==revision,"catalog existing-only read cannot recreate missing lock");
}
void ByteCapacityMaintenance(const fs::path& root){
    c::Store store(root/L"byte-capacity");Bytes payload(c::MaximumSnapshotBytes,0x53);c::Snapshot head;
    Check(store.Initialize(payload,head)==c::Result::Ok,"publish real maximum-sized snapshot");unsigned publications=1;
    for(;;){c::WritePlan next;const auto result=store.PlanCommit(head,payload,next);
        if(result==c::Result::Limit)break;
        Check(result==c::Result::Ok&&publications<c::MaximumCommits,"large-root preflight remains bounded");
        Check(store.Execute(next,head)==c::Result::Ok,"publish another real large-root generation");++publications;
    }
    Check(publications>1&&publications<c::MaximumCommits-1,"actual metadata byte cap requires maintenance before count-based 63-step interval");
    Check(store.Checkpoint(head,head)==c::Result::Ok,"one real checkpoint compacts oversized generation history");c::CollectionReport report;
    Check(store.CollectRetired(report)==c::Result::Ok&&report.busy==0&&report.pending==0&&report.failed==0&&report.absent>0,"confirm real retired metadata removal before spending capacity");
    Check(store.Commit(head,payload,head)==c::Result::Ok,"one successful checkpoint and collection suffice for next maximum-sized publication");
}
void CheckpointCredit(const fs::path& root){
    Fixture fixture(root,L"checkpoint-credit");auto capture=fixture.New(30,8193);
    Check(capture->Reserve()==J::Ok,"persist pending capture for maintenance credit");
    auto facts=fixture.Read();const auto ordinary=facts.Credit();const auto payload=facts.Business().Source().Bytes();
    Check(!facts.Business().Source().IsCheckpoint()&&ordinary.remainingPublications==4&&ordinary.checkpointPublications==5,"ordinary head retains pending maintenance and standby");
    facts={};auto store=fixture.Store();c::Snapshot head;
    Check(store.Read(head)==c::Result::Ok,"read real maintenance baseline");
    const auto beforePhysical=MetadataCharge(fixture.path/L"Video"/L"control"/L"catalog");
    Check(store.Checkpoint(head,head)==c::Result::Ok&&head.IsCheckpoint(),"publish actual trusted checkpoint head");
    f::Directory reopened;Check(f::Directory::Open(fixture.path,false,reopened)==f::Result::Ok,"reopen physical root after checkpoint");
    Check(ReadBudget(reopened,fixture.unit,facts)==R::Ok,"reconstruct checkpoint credit from newly opened directory");
    const auto checkpoint=facts.Credit();
    Check(facts.Business().Source().IsCheckpoint()&&checkpoint.checkpointPublications==4&&
        checkpoint.pendingMetadataBytes==ordinary.pendingMetadataBytes&&
        checkpoint.metadataBytes+ordinary.checkpointBytesPerStep==ordinary.metadataBytes,
        "durable checkpoint consumes exactly one maximum checkpoint credit");
    Check(MetadataCharge(fixture.path/L"Video"/L"control"/L"catalog")>=beforePhysical,
        "consumed credit does not pretend uncollected retired metadata was removed");
    ReadInChild(fixture.path,fixture.unit,8193,true);
    b::SourceCreditRequirement next;
    Check(facts.Project(facts.Business().Sources(),next)==R::Ok&&next.checkpointPublications==5&&next.metadataBytes==ordinary.metadataBytes,
        "projection from checkpoint describes ordinary publication and restores standby");
    auto invalid=facts.Business().Sources();invalid.storeOwner[0]^=1;next.rawReservedBytes=123;
    Check(facts.Project(invalid,next)==R::Invalid&&next.rawReservedBytes==123&&next.metadataBytes==ordinary.metadataBytes,
        "invalid checkpoint projection preserves previous output");
    facts={};c::CollectionReport collected;
    Check(store.CollectRetired(collected)==c::Result::Ok&&collected.busy==0&&collected.pending==0&&collected.failed==0&&collected.absent>0,
        "complete actual checkpoint collection before subsequent publication");
    Check(ReadBudget(reopened,fixture.unit,facts)==R::Ok&&facts.Credit().metadataBytes==checkpoint.metadataBytes,
        "collection changes physical files without regranting consumed standby");
    facts={};Check(store.Commit(head,payload,head)==c::Result::Ok&&!head.IsCheckpoint(),"ordinary commit replaces checkpoint head kind");
    Check(ReadBudget(reopened,fixture.unit,facts)==R::Ok&&facts.Credit().metadataBytes==ordinary.metadataBytes&&facts.Credit().checkpointPublications==5,
        "ordinary durable head restores exactly one standby checkpoint");
    ReadInChild(fixture.path,fixture.unit,8193,false);facts={};
    Check(capture->Cancel()==J::Ok,"cancel pending fixture through real journal");
    Check(store.Read(head)==c::Result::Ok&&store.Checkpoint(head,head)==c::Result::Ok,"checkpoint actual empty registry");
    Check(ReadBudget(reopened,fixture.unit,facts)==R::Ok&&facts.Business().Source().IsCheckpoint()&&facts.Credit().remainingPublications==0&&facts.Credit().metadataBytes==0,
        "empty checkpoint head has no unused standby to charge again");
}
void RawProjection(const fs::path& root){
    Fixture fixture(root,L"raw-projection");const Bytes bytes(8193,0x4b);const auto path=fixture.path/L"input.mp4";Write(path,bytes);
    auto capture=fixture.New(10,bytes.size());Check(capture->Reserve()==J::Ok,"reserve projection fixture");
    auto facts=fixture.Read();f::InventorySnapshot inventory;f::InventoryLimits limits;
    Check(fixture.manager.Inventory(limits,inventory)==f::Result::Ok,"measure actual Reserved inventory");b::RawSourceBudgetProjection projected;
    Check(b::ProjectRawSourceBudget(facts,inventory,projected)==R::Ok&&projected.residualBytes==Round(bytes.size(),fixture.unit)&&projected.deductions.empty(),"Reserved cannot deduct any unknown physical file");
    Check(capture->Prepare(path.c_str())==J::Ok,"prepare actual projection raw identity");facts=fixture.Read();
    Check(fixture.manager.Inventory(limits,inventory)==f::Result::Ok&&b::ProjectRawSourceBudget(facts,inventory,projected)==R::Ok&&projected.residualBytes==Round(bytes.size(),fixture.unit)&&projected.deductions.size()==1&&projected.deductedBytes==0,"zero raw file retains full promise but exposes claimed identity");
    Check(capture->Complete()==J::Ok,"capture real projection bytes");facts=fixture.Read();
    Check(fixture.manager.Inventory(limits,inventory)==f::Result::Ok&&b::ProjectRawSourceBudget(facts,inventory,projected)==R::Ok&&projected.residualBytes==0&&projected.deductedBytes==projected.promisedBytes,"captured default allocation consumes raw promise once");
    auto proposed=facts.Business().Sources();proposed.operations.front().stage=b::SourceStage::AbortCaptured;
    b::SourceOperation additional;additional.id=Id<b::OperationId>(90);additional.sourceId=Id<b::SourceId>(190);additional.directoryId=1;additional.reservedBytes=1;proposed.operations.push_back(additional);
    b::RawSourceBudgetProjection future;
    Check(b::ProjectRawSourceBudget(facts,proposed,inventory,future)==R::Ok&&future.promisedBytes==projected.promisedBytes+fixture.unit&&
        future.deductedBytes==projected.deductedBytes&&future.residualBytes==fixture.unit&&future.deductions.size()==1,
        "proposed registry uses same captured identity plus full new durable promise");
    const auto retained=future;
    const auto preserved=[&](){return future.promisedBytes==retained.promisedBytes&&future.deductedBytes==retained.deductedBytes&&
        future.residualBytes==retained.residualBytes&&future.deductions.size()==retained.deductions.size()&&
        future.deductions.front().operation==retained.deductions.front().operation&&
        future.deductions.front().identity.volume==retained.deductions.front().identity.volume&&
        future.deductions.front().identity.fileIndex==retained.deductions.front().identity.fileIndex&&
        future.deductions.front().identity.fileBytes==retained.deductions.front().identity.fileBytes&&
        future.deductions.front().deductedBytes==retained.deductions.front().deductedBytes;};
    auto bad=proposed;bad.storeOwner[0]^=1;
    Check(b::ProjectRawSourceBudget(facts,bad,inventory,future)==R::Invalid&&preserved(),"proposed raw owner mismatch preserves complete projection");
    bad=proposed;++bad.maintenanceEpoch;
    Check(b::ProjectRawSourceBudget(facts,bad,inventory,future)==R::Invalid&&preserved(),"proposed raw epoch mismatch preserves complete projection");
    bad=proposed;bad.directories.front().identity.fileIndex^=1;
    Check(b::ProjectRawSourceBudget(facts,bad,inventory,future)==R::Invalid&&preserved(),"proposed raw binding mismatch preserves complete projection");
    bad=facts.Business().Sources();const auto operation=bad.operations.front();bad.operations.clear();
    b::SourceRecord published;published.id=operation.sourceId;published.createdBy=operation.id;published.directoryId=operation.directoryId;
    published.file=operation.captured;published.owners={999};bad.sources.push_back(published);
    Check(b::ProjectRawSourceBudget(facts,bad,inventory,future)==R::Invalid&&preserved(),"proposed raw source rejects nonexistent actual project reference");
    Fixture other(root,L"projection-other");f::InventorySnapshot wrong;Check(other.manager.Inventory(limits,wrong)==f::Result::Ok,"measure different physical root");const auto saved=projected.deductedBytes;
    Check(b::ProjectRawSourceBudget(facts,wrong,projected)==R::RecoveryRequired&&projected.deductedBytes==saved&&projected.deductions.size()==1,"wrong-root projection preserves complete old output");
    b::SourceBudgetSnapshot locked;Check(b::ReadSourceBudgetLocked(fixture.manager,fixture.unit,locked)==R::Ok,"acquire source exclusion for a full inventory operation");
    f::FileLock attempted;Check(fixture.control.LockExisting(0,attempted)==f::Result::Busy,"locked snapshot retains actual control exclusion");
    auto moved=std::move(locked);Check(fixture.control.LockExisting(0,attempted)==f::Result::Busy&&!locked.IsValid(),"moving locked snapshot preserves exclusion");moved={};
    Check(fixture.control.LockExisting(0,attempted)==f::Result::Ok,"last locked snapshot release permits independent writer");attempted={};
    Check(capture->Cancel()==J::Ok,"release projection capture through real journal");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==6&&std::wstring(argv[1])==L"--read-existing"){
        Hooks hooks;f::Directory root;b::SourceBudgetSnapshot snapshot;
        Check(f::Directory::Open(argv[2],false,root)==f::Result::Ok,"child opens existing manager root");
        const bool checkpoint=std::wstring(argv[5])==L"1";
        Check(ReadBudget(root,std::stoull(argv[3]),snapshot)==R::Ok&&snapshot.Credit().rawReservedBytes==std::stoull(argv[4])&&snapshot.Credit().remainingPublications==4&&
            snapshot.Business().Source().IsCheckpoint()==checkpoint&&snapshot.Credit().checkpointPublications==(checkpoint?4:5),"child verifies exact durable pending operation and checkpoint credit after restart");return 0;
    }
    Check(argc==3&&std::wstring(argv[1])==L"--store","require --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;
    ExistingLock(root);Lifecycle(root);ReadFailures(root);ByteCapacityMaintenance(root);CheckpointCredit(root);RawProjection(root);RecoveryEndpoints(root);std::printf("PASS cache source budget checks=%u\n",checks);return 0;
}catch(const std::exception& e){readOnly=false;std::fprintf(stderr,"FAIL cache source budget checks=%u: %s\n",checks,e.what());return 1;}}
