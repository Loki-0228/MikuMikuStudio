// MAINTENANCE: regression-keep. Real isolated responsibility journal files.
// No disk-quota approval, application startup, media decoding or user files.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "../src/io/cache_source_journal.hpp"
#include "../src/io/cache_business_reader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <thread>

namespace allocation {thread_local long remaining=-1;thread_local unsigned hits=0;
struct Fail {explicit Fail(long n){remaining=n;hits=0;}~Fail(){remaining=-1;}};}
void* operator new(std::size_t n){if(allocation::remaining==0){++allocation::hits;throw std::bad_alloc();}if(allocation::remaining>0)--allocation::remaining;
    if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace {
namespace fs=std::filesystem;
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
namespace c=mikumikustudio::cache_catalog;
using R=b::JournalResult;
using S=b::CaptureState;
using Bytes=b::Bytes;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class T>T Id(unsigned n){T id{};id[15]=std::uint8_t(n);return id;}
struct Handle {HANDLE value;explicit Handle(HANDLE h):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;};
void NewDirectory(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"normalized absolute fixture directory");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim new isolated journal fixture");}
void Write(const fs::path& p,const Bytes& bytes){Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(h.value!=INVALID_HANDLE_VALUE,"create owned source without replacing anything");DWORD done=0;
    Check(WriteFile(h.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(h.value),"write exact isolated source payload");}
Bytes Read(const std::wstring& p){std::ifstream file(fs::path(p),std::ios::binary);Check(bool(file),"open actual leased raw source");return Bytes(std::istreambuf_iterator<char>(file),{});}
bool Exists(const fs::path& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;}
std::wstring Prefix(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    wchar_t value[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,value,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"resolve exact owned directory for fault scope");std::wstring result(value,n);result+=L'\\';return result;}
bool Same(const f::RawSourceInfo& a,const f::RawSourceInfo& b){return a.name==b.name&&a.payloadBytes==b.payloadBytes&&a.payloadDigest==b.payloadDigest&&a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;}

enum class FaultKind {CommitFlush,CommitPartialWrite,RawCancel};
struct Fault {std::wstring prefix;FaultKind kind;unsigned hits=0;std::atomic_bool* cancel=nullptr;
    bool Match(HANDLE h)const noexcept {wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);
        if(!n||n>=32768||n<prefix.size()+37||_wcsnicmp(path,prefix.c_str(),prefix.size()))return false;
        const auto* leaf=path+prefix.size();for(unsigned i=0;i<32;++i)if(!((leaf[i]>=L'0'&&leaf[i]<=L'9')||(leaf[i]>=L'a'&&leaf[i]<=L'f')))return false;
        return kind==FaultKind::RawCancel?std::wcsncmp(leaf+32,L".raw.",5)==0:std::wcscmp(leaf+32,L".blob")==0;
    }};
thread_local Fault* active=nullptr;
decltype(&FlushFileBuffers) realFlush=nullptr;
decltype(&WriteFile) realWrite=nullptr;
decltype(&CreateFileW) realCreate=nullptr;
struct ReopenProbe {std::wstring path,application,command;unsigned hits=0;DWORD childExit=0,waitResult=WAIT_FAILED;bool started=false;};
thread_local ReopenProbe* reopenProbe=nullptr;
BOOL RunRecoveryChild(ReopenProbe& probe)noexcept {
    STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
    probe.started=CreateProcessW(probe.application.c_str(),probe.command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    if(!probe.started)return FALSE;
    probe.waitResult=WaitForSingleObject(process.hProcess,10000);
    if(probe.waitResult!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,239);WaitForSingleObject(process.hProcess,3000);}
    GetExitCodeProcess(process.hProcess,&probe.childExit);CloseHandle(process.hThread);CloseHandle(process.hProcess);return TRUE;
}
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templateFile)noexcept {
    auto* probe=reopenProbe;
    if(probe&&!probe->hits&&path&&access==GENERIC_READ&&creation==OPEN_EXISTING&&!_wcsicmp(path,probe->path.c_str())){
        ++probe->hits;reopenProbe=nullptr;RunRecoveryChild(*probe);reopenProbe=probe;
    }
    return realCreate(path,access,sharing,security,creation,flags,templateFile);
}
BOOL WINAPI FlushHook(HANDLE h)noexcept {const auto result=realFlush(h);const auto error=GetLastError();
    if(result&&active&&!active->hits&&active->kind==FaultKind::CommitFlush&&active->Match(h)){++active->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}
    SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID p,DWORD n,LPDWORD done,LPOVERLAPPED overlap)noexcept {
    auto* fault=active;bool matches=fault&&!fault->hits&&!overlap&&fault->Match(h);LARGE_INTEGER zero{},position{};
    if(matches&&fault->kind==FaultKind::CommitPartialWrite&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128&&n>1){
        const auto result=realWrite(h,p,(std::min)(n-1,DWORD(17)),done,overlap);if(result){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return result;}
    const auto result=realWrite(h,p,n,done,overlap);const auto error=GetLastError();
    if(result&&matches&&fault->kind==FaultKind::RawCancel&&fault->cancel){++fault->hits;fault->cancel->store(true);}
    SetLastError(error);return result;
}
struct Hooks {Hooks(){Check(MH_Initialize()==MH_OK,"initialize local Win32 failure observation");
    Check(MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK,"hook real catalog flush");
    Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK,"hook real bounded writes");
    Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"observe actual raw writer-to-reader reopen");
    Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable local hooks");}
    ~Hooks(){active=nullptr;reopenProbe=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm {explicit Arm(Fault& f){Check(!active,"nonrecursive exact-path fault scope");active=&f;}~Arm(){active=nullptr;}};

struct Fixture {
    fs::path path;
    f::Directory control,pages,raw;
    b::BusinessRoot initial;
    f::BlobInfo pageInfo;
    std::unique_ptr<b::SourceJournal> journal;
    explicit Fixture(const fs::path& parent,const wchar_t* name):path(parent/name){NewDirectory(path);
        Check(f::Directory::Open(path/L"control",true,control)==f::Result::Ok&&f::Directory::Open(path/L"pages",true,pages)==f::Result::Ok&&f::Directory::Open(path/L"raw",true,raw)==f::Result::Ok,"create distinct pinned control/page/raw directories");
        initial.legacy.storeOwner=Id<f::Owner>(9);initial.legacy.maintenanceEpoch=1;initial.legacy.nextProjectId=2;
        initial.sources.storeOwner=initial.legacy.storeOwner;initial.sources.maintenanceEpoch=1;
        b::DirectoryBinding binding;binding.id=1;Check(raw.GetIdentity(binding.identity)==f::Result::Ok,"bind physical raw directory");initial.sources.directories.push_back(binding);
        b::Page page;page.storeOwner=initial.legacy.storeOwner;b::Project project;project.id=1;project.normalizedPath=project.displayPath=u"C:\\journal-test\\one.pmm";page.projects={project};
        Bytes encoded;Check(b::EncodePage(page,encoded)==b::Result::Ok,"encode real project page");f::Spec spec;spec.owner=page.storeOwner;spec.kind=f::Kind::IndexPage;spec.key=b::PageKey;
        Check(pages.Create(spec,encoded.data(),encoded.size(),pageInfo)==f::Result::Ok,"create actual checksummed project page");b::PageRef ref;ref.recordCount=1;ref.blob=pageInfo;initial.legacy.pages={ref};initial.legacy.projectCount=1;initial.legacy.liveEncodedBytes=96+176+encoded.size();
        Check(b::EncodeBusinessRoot(initial,encoded)==b::Result::Ok,"encode initial bound composite root");c::Snapshot first;Check(Store().InitializeOwned(initial.legacy.storeOwner,encoded,first)==c::Result::Ok,"initialize actual control catalog");
        Check(b::SourceJournal::Open(control,pages,raw,journal)==R::Ok&&journal,"open existing validated journal");
    }
    c::Store Store()const{return c::Store(path/L"control"/L"catalog");}
    c::Snapshot Snapshot()const {c::Snapshot s;Check(Store().Read(s)==c::Result::Ok,"read actual current journal CAS");return s;}
    b::BusinessRoot ReadRoot()const {const auto s=Snapshot();b::BusinessRoot r;Check(b::DecodeBusinessRoot(s.Bytes().data(),s.Bytes().size(),r)==b::Result::Ok,"decode actual journal publication");return r;}
    std::unique_ptr<b::SourceCapture> New(unsigned op,unsigned source,std::uint64_t size){std::unique_ptr<b::SourceCapture> out;
        Check(journal->CreateCapture(Id<b::OperationId>(op),Id<b::SourceId>(source),size,out)==R::Ok&&out&&out->State()==S::New&&!out->NeedsResolution(),"create immutable operation identity without disk mutation");return out;}
    fs::path Source(const wchar_t* name,const Bytes& bytes){const auto p=path/name;Write(p,bytes);return p;}
    void Commit(const b::BusinessRoot& root){const auto before=Snapshot();Bytes bytes;Check(b::EncodeBusinessRoot(root,bytes)==b::Result::Ok,"encode explicit external catalog change");c::Snapshot after;
        Check(Store().Commit(before,bytes,after)==c::Result::Ok,"external real CAS mutation");}
};
void Stage(const Fixture& f,const b::SourceCapture& capture,S state,b::SourceStage stage,std::uint64_t bytes){Check(capture.State()==state&&!capture.NeedsResolution(),"capture state matches completed publication");const auto root=f.ReadRoot();
    Check(root.sources.operations.size()==1&&root.sources.operations[0].id==capture.Id()&&root.sources.operations[0].stage==stage&&root.sources.operations[0].reservedBytes==bytes,"actual catalog records exact stage, identity and reservation");}

void Lifecycle(const fs::path& root){Fixture f(root,L"lifecycle");const Bytes payload{0,1,2,0xff,8,0,9};const auto source=f.Source(L"input.mp4",payload);const auto before=f.Snapshot();
    auto capture=f.New(1,2,64);Check(f.Snapshot().Revision()==before.Revision(),"CreateCapture does no catalog publication");
    Check(capture->Reserve()==R::Ok,"reserve durable declared responsibility");Stage(f,*capture,S::Reserved,b::SourceStage::Reserved,64);
    Check(capture->Prepare(source.c_str())==R::Ok,"prepare bounded real source");Stage(f,*capture,S::Prepared,b::SourceStage::Prepared,payload.size());auto prepared=f.ReadRoot().sources.operations[0];
    Check(prepared.plan.sourceBytes==payload.size()&&prepared.plan.createdIdentity.fileBytes==0&&Exists(f.path/L"raw"/prepared.plan.name),"Prepared names actual zero-length raw file and shrinks declaration");
    Check(capture->Complete()==R::Ok,"complete encoded bytes into immutable read lease");Stage(f,*capture,S::Captured,b::SourceStage::Captured,payload.size());const auto captured=f.ReadRoot().sources.operations[0];
    Check(captured.captured.name==prepared.plan.name&&captured.captured.payloadBytes==payload.size(),"Captured preserves original prepared file identity");
    Check(DeleteFileW(source.c_str())!=FALSE,"captured lease no longer needs original source");
    f::RawSourceLease output;Check(f.raw.AcquireRawSource(captured.captured,output)==f::Result::Ok,"seed existing lease output for failed publication");const auto outputBefore=output.Info();
    const auto revision=f.Snapshot().Revision();Check(capture->Publish({99},11,output)==R::Invalid&&Same(output.Info(),outputBefore)&&Read(output.Path())==payload&&capture->State()==S::Captured&&f.Snapshot().Revision()==revision,"missing project owner cannot publish or replace existing lease output");
    Check(capture->Publish({1,1},11,output)==R::Invalid&&Same(output.Info(),outputBefore),"duplicate project owner rejected without replacing output lease");
    Check(capture->Publish({1},123,output)==R::Ok&&capture->State()==S::Published&&output.IsValid()&&Read(output.Path())==payload,"publish and transfer real readable raw lease");
    auto published=f.ReadRoot();Check(published.sources.operations.empty()&&published.sources.sources.size()==1&&published.sources.sources[0].id==Id<b::SourceId>(2)&&published.sources.sources[0].createdBy==capture->Id()&&published.sources.sources[0].owners==std::vector<b::ProjectId>({1})&&published.sources.sources[0].lastUse==123,"one CAS replaces operation with fully owned source record");
    Check(capture->Cancel()==R::NotReady,"published capture cannot cancel durable source");const auto info=output.Info();f::RawSourceLease acquired;Check(f.journal->Acquire(Id<b::SourceId>(2),acquired)==R::Ok&&Same(acquired.Info(),info),"journal acquires published exact identity and digest");
    auto next=f.New(1,77,32);capture.reset();const auto afterPublish=f.Snapshot();
    Check(f.journal->Recover(next->Id())==R::Busy&&next->State()==S::New&&f.Snapshot().Bytes()==afterPublish.Bytes(),"late Published capture destruction cannot remove a newer same-ID local capture");
    Check(next->Cancel()==R::Ok,"cancel only newer unreserved local capture without altering published source");
    Check(f.journal->Acquire(Id<b::SourceId>(88),acquired)==R::Missing&&Same(acquired.Info(),info),"unknown source preserves existing lease output");
    Check(f.journal->Retire(Id<b::SourceId>(2))==R::Ok,"retire durable source without deletion");published=f.ReadRoot();Check(published.sources.sources[0].state==b::SourceState::Retired&&published.sources.sources[0].owners==std::vector<b::ProjectId>({1}),"retirement preserves project responsibility");
    Check(f.journal->Acquire(Id<b::SourceId>(2),acquired)==R::NotReady&&Same(acquired.Info(),info)&&Read(output.Path())==payload,"retired acquisition rejected but already held decoder-style leases remain valid");
    acquired={};output={};Check(f.raw.RemoveRawSource(info)==f::Result::DeletePending,"fixture explicitly deletes owned retired payload after all leases close");
}

void Cancels(const fs::path& root){for(unsigned stage=0;stage<4;++stage){Fixture f(root,(L"cancel-"+std::to_wstring(stage)).c_str());const Bytes bytes{1,2,3,4};const auto source=f.Source(L"cancel.mov",bytes);auto capture=f.New(1,2,bytes.size());
    if(stage>=1)Check(capture->Reserve()==R::Ok,"prepare cancel Reserved");if(stage>=2)Check(capture->Prepare(source.c_str())==R::Ok,"prepare cancel Prepared");if(stage>=3)Check(capture->Complete()==R::Ok,"prepare cancel Captured");
    const auto prior=f.ReadRoot();std::wstring name;if(stage>=2)name=prior.sources.operations[0].plan.name;
    std::unique_ptr<b::SourceJournal> other;Check(b::SourceJournal::Open(f.control,f.pages,f.raw,other)==R::Ok,"second same-thread handle shares exact journal binding");
    const auto beforeRecovery=f.Snapshot();
    Check(other->Recover(capture->Id())==R::Busy&&capture->State()==static_cast<S>(stage)&&f.Snapshot().Bytes()==beforeRecovery.Bytes(),"live capture cannot be recovered by another journal handle");
    Check(capture->Cancel()==R::Ok&&capture->State()==S::Cancelled&&!capture->NeedsResolution(),"explicit cancel reaches terminal state");const auto after=f.ReadRoot();Check(after.sources.operations.empty()&&after.sources.sources.empty(),"cancel removes only its durable intent");
    Check(name.empty()||!Exists(f.path/L"raw"/name),"cancel physically removes its own unpublished raw name");Check(DeleteFileW(source.c_str())!=FALSE,"cancel releases original source pin");
}}
void BusyCancellation(const fs::path& root){Fixture f(root,L"busy-cancel");const Bytes bytes{8,9,10};const auto source=f.Source(L"busy.mp4",bytes);auto capture=f.New(1,2,bytes.size());
    Check(capture->Reserve()==R::Ok&&capture->Prepare(source.c_str())==R::Ok&&capture->Complete()==R::Ok,"capture real externally pinned cancellation fixture");const auto info=f.ReadRoot().sources.operations[0].captured;f::RawSourceLease external;
    Check(f.raw.AcquireRawSource(info,external)==f::Result::Ok,"actual additional reader protects captured file");const auto snapshot=f.Snapshot();
    Check(capture->Cancel()==R::Busy&&capture->State()==S::Captured&&Read(external.Path())==bytes,"failed exclusive cleanup preserves responsibility and external bytes");
    Check(f.Snapshot().Revision()==snapshot.Revision()+1&&f.ReadRoot().sources.operations[0].stage==b::SourceStage::AbortCaptured,"local cancellation durably records AbortCaptured before surrendering its read pin");
    external={};Check(capture->Cancel()==R::Ok&&f.ReadRoot().sources.operations.empty()&&!Exists(f.path/L"raw"/info.name),"cancel retries successfully only after last external lease closes");
}
void StreamCancellation(const fs::path& root){Fixture f(root,L"stream-cancel");Bytes bytes(3*1024*1024+19);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=std::uint8_t(i*7);const auto source=f.Source(L"large.avi",bytes);auto capture=f.New(1,2,bytes.size());
    Check(capture->Reserve()==R::Ok&&capture->Prepare(source.c_str())==R::Ok,"prepare genuine chunked source");std::atomic_bool cancelled{true};const auto before=f.Snapshot();
    Check(capture->Complete(&cancelled)==R::Cancelled&&capture->State()==S::Prepared&&f.Snapshot().Bytes()==before.Bytes(),"pre-cancelled complete preserves durable Prepared plan");
    cancelled=false;Fault fault{Prefix(f.path/L"raw"),FaultKind::RawCancel,0,&cancelled};R result;{Arm arm(fault);result=capture->Complete(&cancelled);}
    Check(fault.hits==1&&result==R::Cancelled&&capture->State()==S::Prepared&&f.Snapshot().Bytes()==before.Bytes(),"real copy cancellation after write retains recoverable Prepared responsibility");
    cancelled=false;Check(capture->Complete(&cancelled)==R::Ok,"retry restarts complete raw copy");f::RawSourceLease output;Check(capture->Publish({1},9,output)==R::Ok&&Read(output.Path())==bytes,"retry publishes complete exact bytes, not cancelled prefix");
    const auto info=output.Info();output={};Check(f.journal->Retire(Id<b::SourceId>(2))==R::Ok&&f.raw.RemoveRawSource(info)==f::Result::DeletePending,"explicit cleanup of published streaming fixture");
}
void Abandon(const fs::path& root){for(unsigned stage=1;stage<4;++stage){Fixture f(root,(L"abandon-"+std::to_wstring(stage)).c_str());const Bytes bytes{4,5,6,7,8};const auto source=f.Source(L"gone.mov",bytes);auto capture=f.New(1,2,bytes.size());
    Check(capture->Reserve()==R::Ok,"reserve orphan recovery case");if(stage>=2)Check(capture->Prepare(source.c_str())==R::Ok,"prepare orphan recovery case");if(stage>=3)Check(capture->Complete()==R::Ok,"capture orphan recovery case");
    const auto prior=f.Snapshot();const auto record=f.ReadRoot().sources.operations[0];capture.reset();f.journal.reset();
    Check(f.Snapshot().Bytes()==prior.Bytes()&&(stage<2||Exists(f.path/L"raw"/record.plan.name)),"destructors neither delete files nor silently publish cancellation");
    Check(b::SourceJournal::Open(f.control,f.pages,f.raw,f.journal)==R::Ok&&f.journal->Recover(record.id)==R::Ok,"same physical journal reclaims abandoned operation through explicit recovery");
    Check(f.ReadRoot().sources.operations.empty()&&(stage<2||!Exists(f.path/L"raw"/record.plan.name)),"orphan recovery removes exact owned file and responsibility");
    Check(DeleteFileW(source.c_str())!=FALSE,"recovery releases any remaining original read pin");
}}
void ConflictAndThread(const fs::path& root){Fixture f(root,L"conflict-thread");auto capture=f.New(1,2,64);std::unique_ptr<b::SourceCapture> duplicate;
    Check(f.journal->CreateCapture(Id<b::OperationId>(1),Id<b::SourceId>(3),64,duplicate)==R::Busy&&!duplicate,"active operation ID is not claimed twice in-process");
    auto changed=f.ReadRoot();b::SourceOperation foreign;foreign.id=capture->Id();foreign.sourceId=Id<b::SourceId>(9);foreign.directoryId=1;foreign.reservedBytes=32;changed.sources.operations={foreign};f.Commit(changed);const auto before=f.Snapshot();
    Check(capture->Reserve()==R::Conflict&&capture->State()==S::New&&f.Snapshot().Bytes()==before.Bytes(),"real external CAS footprint conflict is not overwritten");
    Check(capture->Cancel()==R::Ok&&capture->State()==S::Cancelled&&f.Snapshot().Bytes()==before.Bytes(),"cancelling unpublished local operation does not erase foreign same-ID intent");
    capture.reset();Check(f.journal->Recover(foreign.id)==R::Ok&&f.ReadRoot().sources.operations.empty(),"explicit journal recovery handles durable foreign reserved intent");
    auto activeCapture=f.New(11,12,32);R reserve=R::Ok,recover=R::Ok,acquire=R::Ok,retire=R::Ok,open=R::Ok,create=R::Ok;
    std::thread worker([&]{std::unique_ptr<b::SourceJournal> other;std::unique_ptr<b::SourceCapture> next;f::RawSourceLease out;
        reserve=activeCapture->Reserve();recover=f.journal->Recover(activeCapture->Id());acquire=f.journal->Acquire(Id<b::SourceId>(12),out);retire=f.journal->Retire(Id<b::SourceId>(12));
        create=f.journal->CreateCapture(Id<b::OperationId>(31),Id<b::SourceId>(32),32,next);open=b::SourceJournal::Open(f.control,f.pages,f.raw,other);});worker.join();
    Check(reserve==R::Thread&&recover==R::Thread&&acquire==R::Thread&&retire==R::Thread&&create==R::Thread&&open==R::Thread,"cross-thread actions including same-root Open cannot assume journal ownership");
    Check(activeCapture->State()==S::New&&f.ReadRoot().sources.operations.empty()&&activeCapture->Cancel()==R::Ok,"wrong-thread attempts leave local operation and persistent head unchanged");
}
void Uncertain(const fs::path& root){for(unsigned phase=0;phase<4;++phase){Fixture f(root,(L"uncertain-"+std::to_wstring(phase)).c_str());const Bytes bytes{7,6,5,4,3};const auto source=f.Source(L"uncertain.mp4",bytes);auto capture=f.New(1,2,bytes.size());
    if(phase>=1)Check(capture->Reserve()==R::Ok,"setup uncertainty Reserved");if(phase>=2)Check(capture->Prepare(source.c_str())==R::Ok,"setup uncertainty Prepared");if(phase>=3)Check(capture->Complete()==R::Ok,"setup uncertainty Captured");
    const auto before=capture->State();Fault fault{Prefix(f.path/L"control"/L"catalog"/L"commits"),FaultKind::CommitFlush};R result;f::RawSourceLease out;
    {Arm arm(fault);switch(phase){case 0:result=capture->Reserve();break;case 1:result=capture->Prepare(source.c_str());break;case 2:result=capture->Complete();break;default:result=capture->Publish({1},71,out);break;}}
    Check(fault.hits==1&&result==R::Indeterminate&&capture->State()==before&&capture->NeedsResolution()&&!out.IsValid(),"real commit flush uncertainty retains previous local state and lease output");
    Check(capture->Reserve()==R::Indeterminate&&capture->NeedsResolution(),"uncertain mutation cannot be blindly retried");
    Check(capture->Resolve()==R::Ok&&!capture->NeedsResolution()&&capture->State()==static_cast<S>(phase+1),"verified complete catalog publication advances exact attempted footprint once");
    if(phase==3){Check(f.journal->Acquire(Id<b::SourceId>(2),out)==R::Ok&&Read(out.Path())==bytes,"resolved Published keeps readable source ownership");const auto info=out.Info();out={};capture.reset();
        Check(f.journal->Retire(Id<b::SourceId>(2))==R::Ok&&f.raw.RemoveRawSource(info)==f::Result::DeletePending,"resolved publication fixture cleanup after capture owner releases");}
    else Check(capture->Cancel()==R::Ok&&f.ReadRoot().sources.operations.empty(),"resolved incomplete capture remains explicitly cancellable");
}}
void RolledBack(const fs::path& root){Fixture f(root,L"rolled-back");auto capture=f.New(1,2,32);Fault fault{Prefix(f.path/L"control"/L"catalog"/L"commits"),FaultKind::CommitPartialWrite};R result;
    {Arm arm(fault);result=capture->Reserve();}Check(fault.hits==1&&result==R::Indeterminate&&capture->NeedsResolution()&&capture->State()==S::New,"actual partial commit retains unresolved New operation");
    Check(capture->Resolve()==R::Conflict&&!capture->NeedsResolution()&&capture->State()==S::New&&f.ReadRoot().sources.operations.empty(),"catalog rollback does not pretend reservation was published");
    Check(capture->Reserve()==R::Ok&&capture->Cancel()==R::Ok,"explicit retry only after verified rollback can complete");
}
void LateDestruction(const fs::path& root){Fixture f(root,L"late-destruction");auto old=f.New(1,2,32);
    Check(old->Reserve()==R::Ok&&old->Cancel()==R::Ok&&old->State()==S::Cancelled,"old cancelled capture remains a live terminal object");
    auto current=f.New(1,3,32);Check(current->Reserve()==R::Ok,"reuse completed operation ID with distinct current capture");const auto before=f.Snapshot();
    old.reset();Check(f.journal->Recover(current->Id())==R::Busy&&current->State()==S::Reserved&&f.Snapshot().Bytes()==before.Bytes(),"late old destructor cannot erase new same-ID active slot or enable recovery");
    Check(current->Cancel()==R::Ok,"new operation remains independently cancellable");
}
void FailedPrepareAbandon(const fs::path& root){Fixture f(root,L"failed-prepare-abandon");const Bytes bytes{1,3,5,7};const auto source=f.Source(L"retained.mp4",bytes);auto capture=f.New(1,2,bytes.size());
    Check(capture->Reserve()==R::Ok,"reserve before failed Prepared catalog publication");
    Fault fault{Prefix(f.path/L"control"/L"catalog"/L"commits"),FaultKind::CommitPartialWrite};R result;
    {Arm arm(fault);result=capture->Prepare(source.c_str());}
    Check(fault.hits==1&&result==R::Indeterminate&&capture->State()==S::Reserved&&capture->NeedsResolution(),"actual Prepared publication interruption keeps owned file and unresolved reservation");
    std::vector<std::wstring> names;Check(f.raw.EnumerateAll(32,names)==f::Result::Ok&&names.size()==1,"failed Prepared registration leaves one explicitly owned raw candidate");const auto rawPath=f.path/L"raw"/names[0];
    capture.reset();f.journal.reset();Check(Exists(rawPath),"abandoning failed registration does not discard unknown file responsibility");
    c::RecoveryReport report;Check(f.Store().RecoverOwned(report)==c::Result::Ok&&report.rolledBack&&!report.confirmedPublication,"explicit catalog recovery verifies incomplete Prepared commit rollback");
    const auto restored=f.ReadRoot();Check(restored.sources.operations.size()==1&&restored.sources.operations[0].stage==b::SourceStage::Reserved,"catalog rollback preserves prior durable reservation");
    Check(b::SourceJournal::Open(f.control,f.pages,f.raw,f.journal)==R::Ok,"same-process reopening retains live unregistered-file responsibility");
    Check(f.journal->Recover(Id<b::OperationId>(1))==R::Ok&&!Exists(rawPath)&&f.ReadRoot().sources.operations.empty(),"recovery uses retained exact handle/plan to register abort and remove candidate");
    Check(DeleteFileW(source.c_str())!=FALSE,"failed registration recovery releases source pin");
}
void UncertainReservationAbandon(const fs::path& root){Fixture f(root,L"uncertain-reservation-abandon");auto capture=f.New(1,2,32);
    Fault fault{Prefix(f.path/L"control"/L"catalog"/L"commits"),FaultKind::CommitFlush};R result;{Arm arm(fault);result=capture->Reserve();}
    Check(fault.hits==1&&result==R::Indeterminate&&capture->NeedsResolution(),"Reserve uncertainty exists before any raw resource has been allocated");
    capture.reset();f.journal.reset();
    Check(b::SourceJournal::Open(f.control,f.pages,f.raw,f.journal)==R::Ok,"unresolved metadata responsibility survives destruction even without raw handles");
    Check(f.journal->Recover(Id<b::OperationId>(1))==R::Ok&&f.ReadRoot().sources.operations.empty(),"same-process known binding resolves and cancels abandoned reservation exactly once");
}
void RetirementUncertainty(const fs::path& root){for(unsigned partial=0;partial<2;++partial){Fixture f(root,(L"retirement-"+std::to_wstring(partial)).c_str());const Bytes bytes{3,6,9,12};const auto source=f.Source(L"retire.mp4",bytes);auto capture=f.New(1,2,bytes.size());
    Check(capture->Reserve()==R::Ok&&capture->Prepare(source.c_str())==R::Ok&&capture->Complete()==R::Ok,"prepare source for retirement uncertainty");f::RawSourceLease caller;
    Check(capture->Publish({1},40,caller)==R::Ok,"publish source before retirement uncertainty");const auto info=caller.Info();capture.reset();
    Fault fault{Prefix(f.path/L"control"/L"catalog"/L"commits"),partial?FaultKind::CommitPartialWrite:FaultKind::CommitFlush};R result;
    {Arm arm(fault);result=f.journal->Retire(Id<b::SourceId>(2));}
    Check(fault.hits==1&&result==R::Indeterminate&&Same(caller.Info(),info)&&Read(caller.Path())==bytes,"uncertain retirement preserves existing immutable source lease");
    Check(f.journal->Retire(Id<b::SourceId>(2))==R::Indeterminate,"retirement cannot blindly repeat an uncertain metadata mutation");
    if(!partial){f.journal.reset();Check(b::SourceJournal::Open(f.control,f.pages,f.raw,f.journal)==R::Ok,"pending metadata-only retirement survives journal destruction");}
    Check(f.journal->ResolveRetirement(Id<b::SourceId>(2))==(partial?R::Conflict:R::Ok),"retirement resolves exact committed or rolled-back footprint");
    const auto after=f.ReadRoot().sources.sources[0];Check(after.state==(partial?b::SourceState::Published:b::SourceState::Retired)&&after.owners==std::vector<b::ProjectId>({1})&&Same(after.file,info)&&Read(caller.Path())==bytes,"retirement resolution changes only confirmed state and retains owners and bytes");
    Check(f.journal->ResolveRetirement(Id<b::SourceId>(2))==R::NotReady,"resolved retirement is not replayed a second time");
    if(partial)Check(f.journal->Retire(Id<b::SourceId>(2))==R::Ok,"explicit retirement retry allowed after verified rollback");
    caller={};Check(f.raw.RemoveRawSource(info)==f::Result::DeletePending,"only explicit fixture cleanup removes retired raw source");
}}
int RecoverChild(const fs::path& path,unsigned op){f::Directory control,pages,raw;
    if(f::Directory::Open(path/L"control",false,control)!=f::Result::Ok||f::Directory::Open(path/L"pages",false,pages)!=f::Result::Ok||f::Directory::Open(path/L"raw",false,raw)!=f::Result::Ok)return 91;
    std::unique_ptr<b::SourceJournal> journal;const auto opened=b::SourceJournal::Open(control,pages,raw,journal);
    if(opened==R::Busy)return 83;if(opened!=R::Ok)return 100+int(opened);
    const auto recovered=journal->Recover(Id<b::OperationId>(op));return recovered==R::Busy?84:130+int(recovered);
}
void ReopenInterprocess(const fs::path& root){Fixture f(root,L"cross-process-reopen");const Bytes bytes{11,22,33,44,55};const auto source=f.Source(L"live.mp4",bytes);auto capture=f.New(1,2,bytes.size());
    Check(capture->Reserve()==R::Ok&&capture->Prepare(source.c_str())==R::Ok,"prepare raw writer whose live ownership must span reopen");
    const auto plan=f.ReadRoot().sources.operations[0].plan;wchar_t module[32768]{};const auto length=GetModuleFileNameW(nullptr,module,32768);Check(length&&length<32768,"resolve this exact test executable");
    ReopenProbe probe;probe.path=Prefix(f.path/L"raw")+plan.name;probe.application.assign(module,length);
    probe.command=L"\""+probe.application+L"\" --recover \""+f.path.native()+L"\" 1";
    R result;reopenProbe=&probe;result=capture->Complete();reopenProbe=nullptr;
    if(probe.childExit!=83&&probe.childExit!=84)std::fprintf(stderr,"DIAG raw reopen child hits=%u started=%u wait=%lu exit=%lu complete=%u\n",probe.hits,unsigned(probe.started),probe.waitResult,probe.childExit,unsigned(result));
    Check(probe.hits==1&&probe.started&&probe.waitResult==WAIT_OBJECT_0&&(probe.childExit==83||probe.childExit==84),"independent process cannot recover live raw file during actual read-lease reopen gap");
    Check(result==R::Ok&&capture->State()==S::Captured&&f.ReadRoot().sources.operations[0].stage==b::SourceStage::Captured,"copy survives interprocess recovery attempt and retains Captured stage");
    f::RawSourceLease out;Check(capture->Publish({1},1,out)==R::Ok&&Read(out.Path())==bytes,"protected reopen publishes intact raw bytes");const auto info=out.Info();out={};
    Check(f.journal->Retire(Id<b::SourceId>(2))==R::Ok&&f.raw.RemoveRawSource(info)==f::Result::DeletePending,"explicit cross-process fixture cleanup");
}
int LeaveUncertain(const fs::path& parent,unsigned mode){Fixture f(parent,(L"child-uncertain-"+std::to_wstring(mode)).c_str());const Bytes bytes{10,20,30};const auto source=f.Source(L"child.mp4",bytes);auto capture=f.New(1,2,bytes.size());
    if(mode)Check(capture->Reserve()==R::Ok,"child reserves before interrupted Prepared publication");
    Fault fault{Prefix(f.path/L"control"/L"catalog"/L"commits"),mode==1?FaultKind::CommitFlush:FaultKind::CommitPartialWrite};R result;
    {Arm arm(fault);result=mode?capture->Prepare(source.c_str()):capture->Reserve();}
    Check(fault.hits==1&&result==R::Indeterminate&&capture->NeedsResolution(),"child leaves actual uncertain catalog mutation");
    // Return normally: all process-local objects/handles vanish on process
    // termination. The parent's later OpenForRecovery cannot use this memory.
    return 0;
}
void CrossProcessRecovery(const fs::path& root){wchar_t module[32768]{};const auto length=GetModuleFileNameW(nullptr,module,32768);Check(length&&length<32768,"resolve exact child recovery test executable");
    for(unsigned mode=0;mode<3;++mode){ReopenProbe child;child.application.assign(module,length);child.command=L"\""+child.application+L"\" --leave-uncertain \""+root.native()+L"\" "+std::to_wstring(mode);
        RunRecoveryChild(child);Check(child.started&&child.waitResult==WAIT_OBJECT_0&&child.childExit==0,"separate process leaves real uncertain publication then fully exits");
        const auto path=root/(L"child-uncertain-"+std::to_wstring(mode));f::Directory control,pages,raw;
        Check(f::Directory::Open(path/L"control",false,control)==f::Result::Ok&&f::Directory::Open(path/L"pages",false,pages)==f::Result::Ok&&f::Directory::Open(path/L"raw",false,raw)==f::Result::Ok,"parent opens existing child directories without initialization");
        std::vector<std::wstring> names;Check(raw.EnumerateAll(32,names)==f::Result::Ok&&names.size()==(mode?1u:0u),"fresh process observes exact count of stranded raw candidates");
        std::unique_ptr<b::SourceJournal> journal;Check(b::SourceJournal::Open(control,pages,raw,journal)==R::RecoveryRequired&&!journal,"ordinary fresh-process Open does not silently recover uncertain catalog");
        Check(b::SourceJournal::OpenForRecovery(control,pages,raw,journal)==R::Ok&&journal,"explicit fresh-process recovery verifies catalog intent and all directory bindings");
        c::Store catalog(path/L"control"/L"catalog");c::Snapshot snapshot;Check(catalog.Read(snapshot)==c::Result::Ok,"read explicit recovery outcome");b::BusinessRoot decoded;
        Check(b::DecodeBusinessRoot(snapshot.Bytes().data(),snapshot.Bytes().size(),decoded)==b::Result::Ok,"decode verified recovered publication");
        if(mode==0){Check(decoded.sources.operations.empty()&&journal->Recover(Id<b::OperationId>(1))==R::Missing,"partial Reserve rolls back without creating file responsibility");}
        else if(mode==1){Check(decoded.sources.operations.size()==1&&decoded.sources.operations[0].stage==b::SourceStage::Prepared&&decoded.sources.operations[0].plan.name==names[0],"complete Prepared commit retains durable exact file identity across process exit");
            Check(journal->Recover(Id<b::OperationId>(1))==R::Ok&&!Exists(path/L"raw"/names[0]),"trusted Prepared plan allows precise cross-process cleanup");}
        else {Check(decoded.sources.operations.size()==1&&decoded.sources.operations[0].stage==b::SourceStage::Reserved,"partial Prepared commit leaves only prior Reserved record");
            Check(journal->Recover(Id<b::OperationId>(1))==R::NeedsCleanup&&Exists(path/L"raw"/names[0]),"unregistered raw candidate is not guessed, deleted or treated as reclaimed responsibility");
            c::Snapshot unchanged;Check(catalog.Read(unchanged)==c::Result::Ok&&unchanged.Bytes()==snapshot.Bytes()&&unchanged.Revision()==snapshot.Revision(),"unknown raw names conservatively preserve Reserved responsibility and CAS head");}
    }
}
void InvalidAndAllocation(const fs::path& root){Fixture f(root,L"invalid-allocation");auto* original=f.journal.get();f::Directory empty;
    Check(b::SourceJournal::Open(f.control,f.pages,f.raw,f.journal)==R::Busy&&f.journal.get()==original,"occupied Open output preserves existing journal");
    std::unique_ptr<b::SourceJournal> invalid;
    Check(b::SourceJournal::Open(empty,f.pages,f.raw,invalid)==R::Invalid&&!invalid,"invalid Open preserves empty output");
    Check(b::SourceJournal::Open(f.control,f.raw,f.raw,invalid)==R::Invalid&&!invalid,"aliased physical directories rejected without output replacement");
    {bool success=false;unsigned failures=0;for(long n=0;n<1024;++n){R result;unsigned hits;
        {allocation::Fail fault(n);result=b::SourceJournal::Open(f.control,f.pages,f.raw,invalid);hits=allocation::hits;}
        if(result==R::Ok){Check(hits==0&&invalid,"successful journal Open did not conceal allocation failure");success=true;invalid.reset();break;}
        Check(hits&&(result==R::AllocationFailure||result==R::Io)&&!invalid&&f.journal.get()==original,"Open allocation refusal preserves both empty output and existing owner");++failures;}
        Check(success&&failures,"real journal Open allocation sweep reaches failure and success");}
    std::unique_ptr<b::SourceCapture> capture;Check(f.journal->CreateCapture({},Id<b::SourceId>(2),32,capture)==R::Invalid&&!capture,"zero operation identity rejected");
    Check(f.journal->CreateCapture(Id<b::OperationId>(1),Id<b::SourceId>(2),0,capture)==R::Invalid&&!capture,"zero declaration rejected");
    R result;unsigned hits;{allocation::Fail fault(0);result=f.journal->CreateCapture(Id<b::OperationId>(1),Id<b::SourceId>(2),32,capture);hits=allocation::hits;}
    Check(hits&&result==R::AllocationFailure&&!capture&&f.ReadRoot().sources.operations.empty(),"actual initial allocation refusal cannot create phantom responsibility");
    capture=f.New(1,2,2);const Bytes bytes{1,2,3};const auto source=f.Source(L"oversized.mp4",bytes);Check(capture->Reserve()==R::Ok,"reserve too-small declaration");const auto before=f.Snapshot();
    Check(capture->Prepare(source.c_str())==R::Limit&&capture->State()==S::Reserved&&f.Snapshot().Bytes()==before.Bytes(),"source larger than declaration cannot silently grow responsibility");
    Check(capture->Prepare((f.path/L"missing.mp4").c_str())==R::Missing&&capture->State()==S::Reserved,"missing original leaves durable reservation available for explicit cancel");
    Check(capture->Cancel()==R::Ok,"invalid input still permits reservation cancellation");
}
void RecoveryRefusals(const fs::path& root){Fixture f(root,L"recovery-refusals");f::Directory missing;
    Check(f::Directory::Open(f.path/L"missing-control",true,missing)==f::Result::Ok,"create isolated control directory without catalog");std::unique_ptr<b::SourceJournal> output;
    Check(b::SourceJournal::OpenForRecovery(missing,f.pages,f.raw,output)==R::Missing&&!output&&!Exists(f.path/L"missing-control"/L"catalog"),"explicit recovery never initializes missing catalog");
    const Bytes foreign{0x12,0x34,0x56};const auto foreignPath=f.path/L"control"/L"catalog"/L"user-owned.txt";Write(foreignPath,foreign);
    Check(b::SourceJournal::OpenForRecovery(f.control,f.pages,f.raw,output)==R::RecoveryRequired&&!output&&Read(foreignPath.native())==foreign,"explicit recovery rejects and preserves foreign catalog residue");
}
void CaseInsensitivePendingRemoval(const fs::path& root){Fixture f(root,L"case-pending-removal");const Bytes bytes{5,10,15,20};const auto source=f.Source(L"case.mp4",bytes);
    f::RawSourceWrite write;f::RawSourceCleanup cleanup;f::RawSourceLease lease;
    Check(f.raw.PrepareRawSource(source.c_str(),bytes.size(),write,cleanup)==f::Result::Ok&&!cleanup.IsValid(),"prepare actual raw case-variant recovery file");const auto plan=write.Plan();
    Check(write.Complete(lease)==f::Result::Ok,"complete real case-variant recovery payload");const auto info=lease.Info();lease={};
    auto registry=f.ReadRoot();b::SourceOperation operation;operation.id=Id<b::OperationId>(1);operation.sourceId=Id<b::SourceId>(2);operation.directoryId=1;
    operation.reservedBytes=bytes.size();operation.stage=b::SourceStage::Captured;operation.plan=plan;operation.captured=info;registry.sources.operations={operation};f.Commit(registry);
    // Establish an actual different directory-entry spelling with no lease or
    // ancestor guard alive. Do not fake directory enumeration or raw identity.
    f.journal.reset();f.control={};f.pages={};f.raw={};std::wstring upper=info.name;
    for(auto& ch:upper)if(ch>=L'a'&&ch<=L'z')ch=wchar_t(ch-L'a'+L'A');const auto lowerPath=f.path/L"raw"/info.name,upperPath=f.path/L"raw"/upper;
    const auto intermediate=f.path/L"raw"/L"owned-case-rename.tmp";
    Check(MoveFileExW(lowerPath.c_str(),intermediate.c_str(),0)&&MoveFileExW(intermediate.c_str(),upperPath.c_str(),0),"rename only owned raw file to genuine uppercase entry without changing identity");
    Check(f::Directory::Open(f.path/L"control",false,f.control)==f::Result::Ok&&f::Directory::Open(f.path/L"pages",false,f.pages)==f::Result::Ok&&f::Directory::Open(f.path/L"raw",false,f.raw)==f::Result::Ok,"reopen unchanged physical directory bindings after case-only rename");
    std::vector<std::wstring> names;Check(f.raw.EnumerateAll(32,names)==f::Result::Ok&&std::find(names.begin(),names.end(),upper)!=names.end()&&upper!=info.name,"real directory enumeration reports uppercase spelling");
    Check(b::SourceJournal::Open(f.control,f.pages,f.raw,f.journal)==R::Ok,"open validated metadata retaining canonical lowercase raw name");
    {
        // Zero requested access does not conflict with the cleanup's exclusive
        // READ|DELETE open, but this real handle keeps accepted disposition
        // pending until it closes. No hook changes Windows deletion semantics.
        Handle metadata(CreateFileW(upperPath.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        Check(metadata.value!=INVALID_HANDLE_VALUE,"hold actual zero-access metadata handle through disposition");const auto result=f.journal->Recover(operation.id);
        FILE_STANDARD_INFO standard{};const bool inspected=GetFileInformationByHandleEx(metadata.value,FileStandardInfo,&standard,sizeof standard)!=FALSE;
        names.clear();const auto listed=f.raw.EnumerateAll(32,names);const bool present=listed==f::Result::Ok&&std::find(names.begin(),names.end(),upper)!=names.end();
        if(result!=R::NeedsCleanup||!inspected||!standard.DeletePending||!present)std::fprintf(stderr,"DIAG case pending result=%u inspected=%u deletePending=%u listed=%u present=%u\n",unsigned(result),unsigned(inspected),unsigned(standard.DeletePending),unsigned(listed),unsigned(present));
        Check(result==R::NeedsCleanup&&inspected&&standard.DeletePending&&present,"case-insensitive absence check never treats still-listed delete-pending file as removed");
        const auto pending=f.ReadRoot();Check(pending.sources.operations.size()==1&&pending.sources.operations[0].stage==b::SourceStage::AbortCaptured&&pending.sources.operations[0].plan.name==info.name,"pending uppercase file retains exact canonical cleanup responsibility");
    }
    names.clear();Check(f.raw.EnumerateAll(32,names)==f::Result::Ok&&std::find(names.begin(),names.end(),upper)==names.end(),"closing final metadata handle completes actual namespace removal");
    Check(f.journal->Recover(operation.id)==R::Ok&&f.ReadRoot().sources.operations.empty(),"retry clears responsibility only after real case-variant name disappears");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==4&&std::wstring(argv[1])==L"--recover")return RecoverChild(fs::path(argv[2]),unsigned(std::wcstoul(argv[3],nullptr,10)));
    if(argc==4&&std::wstring(argv[1])==L"--leave-uncertain"){Hooks hooks;return LeaveUncertain(fs::path(argv[2]),unsigned(std::wcstoul(argv[3],nullptr,10)));}
    if(argc!=3||std::wstring(argv[1])!=L"--store")throw std::runtime_error("usage: cache_source_journal_tests --store NEW_ABS_DIR");
    const fs::path root(argv[2]);NewDirectory(root);Hooks hooks;Lifecycle(root);Cancels(root);BusyCancellation(root);StreamCancellation(root);Abandon(root);ConflictAndThread(root);Uncertain(root);RolledBack(root);LateDestruction(root);FailedPrepareAbandon(root);UncertainReservationAbandon(root);RetirementUncertainty(root);ReopenInterprocess(root);CrossProcessRecovery(root);InvalidAndAllocation(root);RecoveryRefusals(root);CaseInsensitivePendingRemoval(root);
    std::printf("PASS cache source journal checks=%u\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL cache source journal after %u checks: %s\n",checks,e.what());return 1;}}
