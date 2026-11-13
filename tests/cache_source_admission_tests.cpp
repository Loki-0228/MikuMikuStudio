// MAINTENANCE: regression-keep. Actual catalog CAS, source credit and disk quota.
// No production video decoder, automatic maintenance or second quota engine.
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

void Refusals(const fs::path& parent){const auto absentBase=parent/L"absent";Fresh(absentBase);const auto absentRoot=absentBase/L"Cache";Policy(absentRoot);std::unique_ptr<A> none;Check(A::Open(none)==R::Missing&&!none&&!fs::exists(absentRoot/L"Video"),"admission never initializes missing Video state");
    Check(CreateDirectoryW((absentRoot/L"Video").c_str(),nullptr),"create only incomplete owned Video fixture");Check(A::Open(none)==R::RecoveryRequired&&!none&&fs::is_empty(absentRoot/L"Video"),"incomplete Video is not silently bootstrapped");
    Fixture fixture(parent/L"refusals");{m::WriteReservation writer(Entry(fixture.root,1),MiB);Check(bool(writer)&&A::Open(none)==R::Busy&&!none,"existing legacy writer excludes a source mutation session");}
    auto session=Open();const auto expected=session->Current().Business().Source();auto proposed=session->Current().Business().Sources();proposed.operations.push_back(Reserved(1,8*MiB));auto output=expected;const auto evidence=CatalogFiles(fixture.Catalog());
    Check(!m::WriteReservation(Entry(fixture.root,2),MiB),"source session excludes nested legacy writer approval");Check(A::Open(none)==R::Busy&&!none,"source session excludes a second metadata admission session");
    c::CollectionReport premature;premature.attempted=71;Check(session->Collect(premature)==R::Conflict&&!session->IsConsumed()&&premature.attempted==71&&CatalogFiles(fixture.Catalog())==evidence,"ordinary head cannot authorize collection or consume session without trusted checkpoint retirement list");
    std::unique_ptr<b::SourceCapture> capture;Check(fixture.journal->CreateCapture(Id<b::OperationId>(44),Id<b::SourceId>(144),MiB,capture)==b::JournalResult::Ok&&capture->Reserve()==b::JournalResult::Busy,"actual journal cannot mutate while admission holds control");
    R threaded=R::Ok;std::thread worker([&]{threaded=session->Commit(expected,proposed,output);});worker.join();Check(threaded==R::Thread&&!session->IsConsumed()&&Same(output,expected)&&CatalogFiles(fixture.Catalog())==evidence,"wrong-thread mutation preserves outputs, catalog and usable session");
    c::Store foreign(fixture.base/L"foreign-catalog");c::Snapshot foreignHead;Check(foreign.InitializeOwned(expected.StoreId(),expected.Bytes(),foreignHead)==c::Result::Ok&&Same(foreignHead,expected),"foreign physical catalog deliberately has matching public owner, revision and payload");
    Check(session->Commit(foreignHead,proposed,output)==R::Conflict&&!session->IsConsumed()&&Same(output,expected)&&CatalogFiles(fixture.Catalog())==evidence,"matching public fields cannot substitute for exact expected catalog origin and head");
    auto bad=proposed;bad.storeOwner[0]^=1;Check(session->Commit(expected,bad,output)==R::Invalid&&!session->IsConsumed()&&Same(output,expected)&&CatalogFiles(fixture.Catalog())==evidence,"wrong owner cannot publish or consume session");
    bad=proposed;bad.operations[0].directoryId=999;Check(session->Commit(expected,bad,output)==R::Invalid&&!session->IsConsumed()&&Same(output,expected),"proposed raw binding must match actual trusted directory");
    Check(session->Commit(expected,proposed,output)==R::Ok&&session->IsConsumed()&&!session->Current().IsValid()&&output.Revision()==expected.Revision()+1,"same session remains usable after pure preflight refusals and hides spent facts after success");const auto committed=output;
    Check(session->Commit(committed,proposed,output)!=R::Ok&&Same(output,committed),"spent session cannot publish twice using old facts");session.reset();Check(Same(fixture.Head(),committed),"successful returned snapshot matches actual catalog");
}
void StaleCannotEraseDebt(const fs::path& parent){Fixture fixture(parent/L"stale");const auto old=fixture.Head();auto stale=fixture.initial.sources;stale.operations.push_back(Reserved(2,8*MiB));auto external=fixture.initial;external.sources.operations.push_back(Reserved(1,16*MiB));fixture.External(external);const auto current=fixture.Head();const auto evidence=CatalogFiles(fixture.Catalog());auto session=Open();auto output=old;
    Check(session->Commit(old,stale,output)==R::Conflict&&!session->IsConsumed()&&Same(output,old)&&CatalogFiles(fixture.Catalog())==evidence,"old expected proposal cannot erase intervening durable sixteen MiB debt");
    auto next=session->Current().Business().Sources();Check(next.operations.size()==1&&next.operations[0].reservedBytes==16*MiB,"fresh actual registry retains concurrent obligation");next.operations.push_back(Reserved(2,8*MiB));
    Check(session->Commit(current,next,output)==R::Ok&&session->IsConsumed(),"fresh proposal extends real current obligations through admitted CAS");session.reset();auto verified=Open();Check(verified->Current().Credit().rawReservedBytes==24*MiB,"reopened facts reconstruct all retained source obligations");}
void DirectCatalogAdvance(const fs::path& parent){
    {Fixture fixture(parent/L"session-head-advance");auto session=Open();const auto original=session->Current().Business().Source();auto stale=session->Current().Business().Sources();stale.operations.push_back(Reserved(2,8*MiB));
        auto external=fixture.initial;external.sources.operations.push_back(Reserved(1,16*MiB));fixture.External(external);const auto advanced=fixture.Head();const auto files=CatalogFiles(fixture.Catalog());auto output=original;
        Check(session->Commit(advanced,stale,output)==R::Conflict&&!session->IsConsumed()&&Same(output,original)&&CatalogFiles(fixture.Catalog())==files,"caller cannot pair newer direct-catalog head with old session facts to overwrite an intervening obligation");
        Check(session->Checkpoint(advanced,output)==R::Conflict&&!session->IsConsumed()&&Same(output,original)&&CatalogFiles(fixture.Catalog())==files,"checkpoint cannot silently substitute caller's newer head for session-bound facts");
        Check(session->Commit(original,stale,output)==R::Conflict&&!session->IsConsumed()&&CatalogFiles(fixture.Catalog())==files,"session's own old head is also rejected by actual catalog CAS");session.reset();auto fresh=Open();Check(fresh->Current().Credit().rawReservedBytes==16*MiB,"reopen preserves directly published debt after all stale attempts");
    }
    {Fixture fixture(parent/L"collection-head-advance");auto before=fixture.Head();c::Snapshot firstCheckpoint;Check(fixture.Store().Checkpoint(before,firstCheckpoint)==c::Result::Ok,"create real first checkpoint before admission opens");auto session=Open();Check(session->Current().Business().Source().IsCheckpoint(),"old session genuinely owns checkpoint facts");
        auto external=fixture.initial;external.sources.operations.push_back(Reserved(1,16*MiB));fixture.External(external);const auto intermediate=fixture.Head();c::Snapshot newerCheckpoint;Check(fixture.Store().Checkpoint(intermediate,newerCheckpoint)==c::Result::Ok,"direct catalog participant publishes newer distinct retirement authority");
        const auto files=CatalogFiles(fixture.Catalog());c::CollectionReport report;report.attempted=71;report.absent=72;report.busy=73;report.pending=74;report.failed=75;report.absentBytes=76;
        Check(session->Collect(report)==R::Conflict&&report.attempted==71&&report.absent==72&&report.busy==73&&report.pending==74&&report.failed==75&&report.absentBytes==76&&CatalogFiles(fixture.Catalog())==files,"stale collection session cannot delete according to a newer checkpoint or alter report on CAS conflict");
    }
}
void LimitsAndPeak(const fs::path& parent){Fixture fixture(parent/L"limits");auto session=Open();const auto head=session->Current().Business().Source();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,64*MiB));auto output=head;const auto before=CatalogFiles(fixture.Catalog());
    Check(session->Commit(head,next,output)==R::Limit&&!session->IsConsumed()&&Same(output,head)&&CatalogFiles(fixture.Catalog())==before,"raw declaration alone filling limit leaves no credit for actual/future metadata and is refused before publication");next.operations[0].reservedBytes=40*MiB;
    Check(session->Commit(head,next,output)==R::Ok&&session->IsConsumed(),"smaller valid proposal uses same unconsumed session");session.reset();Check(!m::WriteReservation(Entry(fixture.root,1),25*MiB),"admitted durable source credits constrain existing texture writer");
    Fixture peak(parent/L"metadata-peak");const auto physical=m::Inspect().totalBytes;const auto unit=Unit(peak.root);session=Open();const auto baseline=session->Current().Business().Source();next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,8*MiB));b::SourceCreditRequirement future;
    Check(session->Current().Project(next,future)==b::SourceBudgetResult::Ok,"derive future obligations from actual verified pages");b::BusinessRoot proposed{session->Current().Business().Description(),next};Bytes encoded;c::WriteFootprint plan;
    Check(b::EncodeBusinessRoot(proposed,encoded)==b::Result::Ok&&c::ApprovedPublicationFootprint(encoded.size(),plan)==c::Result::Ok,"obtain real approved protocol file sizes for proposed publication");std::uint64_t thisWrite=0;for(unsigned i=0;i<plan.count;++i)thisWrite+=Round(plan.files[i].maximumFileBytes,unit);
    Check(physical+future.rawAllocationReservedBytes+future.metadataBytes+thisWrite<Limit&&thisWrite>unit,"fixture leaves positive room for exact metadata-only peak boundary");const auto fill=Limit-physical-future.rawAllocationReservedBytes-future.metadataBytes-thisWrite+unit;session.reset();Sparse(peak.root/L"unknown-filler.bin",fill);
    session=Open();output=baseline;const auto oldFiles=CatalogFiles(peak.Catalog());Check(session->Commit(baseline,next,output)==R::Limit&&!session->IsConsumed()&&Same(output,baseline)&&CatalogFiles(peak.Catalog())==oldFiles,"raw plus future credit fits but current old/new metadata peak exceeds limit and cannot publish");
    next.operations[0].reservedBytes=7*MiB;Check(session->Commit(baseline,next,output)==R::Ok,"one MiB smaller proposal fits actual peak without deleting unknown filler");Check(fs::file_size(peak.root/L"unknown-filler.bin")==fill,"approval preserves unknown filler responsibility");}
void LoweredAndDisabled(const fs::path& parent){Fixture fixture(parent/L"lowered");Policy(fixture.root,128*MiB);auto session=Open();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,80*MiB));c::Snapshot head;
    const auto initial=session->Current().Business().Source();Check(session->Commit(initial,next,head)==R::Ok,"admit eighty MiB under actual larger original policy");session.reset();Policy(fixture.root,Limit);session=Open();next=session->Current().Business().Sources();auto output=head;next.operations[0].reservedBytes=81*MiB;
    Check(session->Commit(head,next,output)==R::Limit&&!session->IsConsumed()&&Same(output,head),"lowered policy cannot approve an increased outstanding responsibility");next.operations[0].reservedBytes=79*MiB;
    Check(session->Commit(head,next,output)==R::Ok,"prepaid responsibility may shrink even while remaining above the newly lowered limit");session.reset();head=output;session=Open();next=session->Current().Business().Sources();next.operations[0].reservedBytes=60*MiB;
    Check(session->Commit(head,next,output)==R::Ok,"lowered policy permits non-incremental responsibility reduction including its actual publication cost");session.reset();head=output;Policy(fixture.root,Limit,false);session=Open();next=session->Current().Business().Sources();next.operations[0].reservedBytes=61*MiB;
    Check(session->Commit(head,next,output)==R::Limit&&!session->IsConsumed()&&Same(output,head),"disabled policy denies growth even when nominal configured limit has room");next.operations[0].reservedBytes=40*MiB;
    Check(session->Commit(head,next,output)==R::Ok,"disabled policy still permits real non-incremental reduction rather than stranding old obligations");session.reset();session=Open();next=session->Current().Business().Sources();next.operations.clear();head=output;
    Check(session->Commit(head,next,output)==R::Ok,"disabled policy permits cancelling existing Reserved metadata with no raw file to guess or delete");}

void PersistPolicyWhileOpen(const Fixture& fixture,std::uint32_t limitMiB,bool enabled){
    namespace p=mikumikustudio::cache_policy;
    p::Store store(fixture.root,fixture.base/L"settings.json");p::Snapshot before,after,verified;
    Check(store.Read(before)==p::Result::Ok,"read real policy for concurrent persisted change");auto value=before.Value();value.limitMiB=limitMiB;value.enabled=enabled;
    Check(store.Commit(before,value,after)==p::Result::Ok&&store.Read(verified)==p::Result::Ok&&verified.Value().limitMiB==limitMiB&&verified.Value().enabled==enabled,"direct policy CAS publishes and verifies actual concurrent setting without manager reentry");
}
void LivePolicyChanges(const fs::path& parent){
    {Fixture fixture(parent/L"live-policy-limit");Policy(fixture.root,128*MiB);auto session=Open();const auto head=session->Current().Business().Source();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,80*MiB));auto output=head;
        PersistPolicyWhileOpen(fixture,64,true);const auto files=CatalogFiles(fixture.Catalog());
        Check(session->Commit(head,next,output)==R::Limit&&!session->IsConsumed()&&Same(output,head)&&CatalogFiles(fixture.Catalog())==files,"session opened at 128 MiB cannot approve eighty MiB after actual persisted limit drops to 64 MiB");
        next.operations[0].reservedBytes=8*MiB;Check(session->Commit(head,next,output)==R::Ok,"policy refusal preserves session for a smaller currently authorized proposal");
    }
    {Fixture fixture(parent/L"live-policy-disabled");auto session=Open();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,8*MiB));const auto initial=session->Current().Business().Source();c::Snapshot head;
        Check(session->Commit(initial,next,head)==R::Ok,"create actual existing responsibility before disabling policy");session.reset();session=Open();next=session->Current().Business().Sources();next.operations[0].reservedBytes=9*MiB;auto output=head;
        PersistPolicyWhileOpen(fixture,64,false);const auto files=CatalogFiles(fixture.Catalog());
        Check(session->Commit(head,next,output)==R::Limit&&!session->IsConsumed()&&Same(output,head)&&CatalogFiles(fixture.Catalog())==files,"session opened enabled cannot increase responsibility after actual persisted policy is disabled");
        next.operations[0].reservedBytes=4*MiB;Check(session->Commit(head,next,output)==R::Ok,"same session still permits real responsibility reduction under newly disabled policy");
    }
    {Fixture fixture(parent/L"live-policy-unreadable");auto session=Open();const auto head=session->Current().Business().Source();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,MiB));auto output=head;const auto files=CatalogFiles(fixture.Catalog());const auto settings=fixture.base/L"settings.json";
        {Handle lock(CreateFileW((settings.native()+L".lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));OVERLAPPED overlap{};
            Check(lock.value!=INVALID_HANDLE_VALUE&&LockFileEx(lock.value,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlap),"hold actual policy byte-range lock while admission session is open");
            Check(session->Commit(head,next,output)==R::Io&&!session->IsConsumed()&&Same(output,head)&&CatalogFiles(fixture.Catalog())==files,"busy policy read refuses approval without consuming session, changing output or publishing catalog files");
        }
        {Handle denied(CreateFileW(settings.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(denied.value!=INVALID_HANDLE_VALUE,"hold exclusive real settings handle to deny policy read");
            Check(session->Commit(head,next,output)==R::Io&&!session->IsConsumed()&&Same(output,head)&&CatalogFiles(fixture.Catalog())==files,"unreadable actual policy refuses approval without consuming session or losing previous output");
        }
        Check(session->Commit(head,next,output)==R::Ok,"same session can approve only after actual policy becomes readable again");
    }
}

void ChildCredit(const fs::path& root,std::uint64_t checkpoints){wchar_t exe[32768]{};const auto n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"resolve current test child executable");auto command=L"\""+std::wstring(exe,n)+L"\" --read-credit \""+root.native()+L"\" "+std::to_wstring(checkpoints);STARTUPINFOW start{};start.cb=sizeof start;PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&process),"launch fresh process to reconstruct checkpoint credit");Handle child(process.hProcess),thread(process.hThread);const auto waited=WaitForSingleObject(child.value,30000);if(waited!=WAIT_OBJECT_0){TerminateProcess(child.value,99);WaitForSingleObject(child.value,5000);}DWORD exit=999;Check(waited==WAIT_OBJECT_0&&GetExitCodeProcess(child.value,&exit)&&exit==0,"maintenance-credit consumption persists across process restart");}
void Maintenance(const fs::path& parent){Fixture fixture(parent/L"maintenance");c::Snapshot pinnedHead;f::ReadLease oldLease;Check(fixture.Store().ReadPinned(pinnedHead,oldLease)==c::Result::Ok,"hold actual old snapshot lease across later retirement");const auto oldFile=fixture.Catalog()/L"snapshots"/oldLease.Info().name;
    auto session=Open();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,8*MiB));c::Snapshot head;const auto initial=session->Current().Business().Source();Check(session->Commit(initial,next,head)==R::Ok,"publish real pending source operation");session.reset();session=Open();const auto credit=session->Current().Credit();Check(credit.checkpointPublications==5,"ordinary Reserved head includes four possible transitions and terminal maintenance");
    Check(session->Checkpoint(head,head)==R::Ok&&session->IsConsumed()&&head.IsCheckpoint(),"explicit admitted checkpoint consumes one bounded maintenance publication");session.reset();session=Open();const auto after=session->Current().Credit();Check(after.checkpointPublications==4&&credit.metadataBytes-after.metadataBytes==credit.checkpointBytesPerStep,"new verified checkpoint consumes exactly one standby credit, without refunding retired files");
    const auto evidence=CatalogFiles(fixture.Catalog());auto output=head;Check(session->Checkpoint(head,output)==R::Conflict&&!session->IsConsumed()&&Same(output,head)&&CatalogFiles(fixture.Catalog())==evidence,"same checkpoint head cannot repeatedly spend standby credit");session.reset();ChildCredit(fixture.root,4);
    session=Open();c::CollectionReport report;Check(session->Collect(report)==R::Ok&&session->IsConsumed()&&report.busy>=1&&report.absent>=1&&fs::exists(oldFile),"explicit collection retains leased old metadata while collecting only authorized other retirees");session.reset();const auto still=m::Inspect();Check(still.error.empty()&&still.totalBytes>=oldLease.Info().identity.fileBytes,"busy retired snapshot remains physical responsibility on reopened manager inventory");Bytes retained;Check(oldLease.ReadAll(retained)==f::Result::Ok&&retained==pinnedHead.Bytes(),"retired lease retains actual bytes after partial collection");oldLease={};
    session=Open();report={};Check(session->Collect(report)==R::Ok&&report.absent>=1&&!report.busy&&!report.pending&&!report.failed&&!fs::exists(oldFile),"only confirmed collection after final lease release reclaims old metadata");session.reset();
    session=Open();next=session->Current().Business().Sources();const auto checkpoint=session->Current().Business().Source();Check(session->Commit(checkpoint,next,head)==R::Ok&&!head.IsCheckpoint(),"ordinary metadata commit after checkpoint creates ordinary head");session.reset();session=Open();Check(session->Current().Credit().checkpointPublications==5,"ordinary commit restores standby instead of reusing consumed checkpoint discount");session.reset();ChildCredit(fixture.root,5);}

struct Fault{std::wstring prefix;bool partial=false;unsigned hits=0;bool Match(HANDLE h)const noexcept{wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);return n>prefix.size()&&n<32768&&!_wcsnicmp(path,prefix.c_str(),prefix.size());}};
thread_local Fault* fault=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&WriteFile) realWrite=nullptr;
BOOL WINAPI FlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();if(result&&fault&&!fault->partial&&!fault->hits&&fault->Match(h)){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{LARGE_INTEGER zero{},position{};if(fault&&fault->partial&&!fault->hits&&!overlap&&count>1&&fault->Match(h)&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto result=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(result){++fault->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return result;}return realWrite(h,bytes,count,done,overlap);}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"inject only exact real catalog commit failure");}~Hooks(){fault=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
std::wstring Prefix(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t text[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,text,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"bind fault to actual fixture commit directory");return std::wstring(text,n)+L'\\';}
void Uncertain(const fs::path& parent){for(unsigned mode=0;mode<2;++mode){Fixture fixture(parent/(L"uncertain-"+std::to_wstring(mode)));auto session=Open();const auto head=session->Current().Business().Source();auto next=session->Current().Business().Sources();next.operations.push_back(Reserved(1,8*MiB));auto output=head;Fault injected;injected.prefix=Prefix(fixture.Catalog()/L"commits");injected.partial=mode!=0;fault=&injected;const auto result=session->Commit(head,next,output);fault=nullptr;
    Check(injected.hits==1&&result==R::Indeterminate&&session->IsConsumed()&&Same(output,head),"actual ambiguous catalog commit consumes session and preserves old output");
    {c::WritePlan recovery;Check(fixture.Store().PlanRecovery(recovery)==c::Result::Ok&&recovery.RecoveryApproval(),"actual manager publication persists explicit approval in its recoverable operation");
        const auto& approval=*recovery.RecoveryApproval();f::DirectoryIdentity manager;
        Check(fixture.manager.GetIdentity(manager)==f::Result::Ok&&approval.directories[static_cast<std::size_t>(c::ApprovalArea::Manager)].volume==manager.volume&&approval.directories[static_cast<std::size_t>(c::ApprovalArea::Manager)].fileIndex==manager.fileIndex&&approval.afterRawResidual==8*MiB&&approval.beforeRawResidual==0&&approval.afterMetadata>0&&approval.policyEnabled&&approval.policyLimitBytes==Limit&&approval.approvedPeak<=Limit,"persisted approval carries actual manager identity and the original approved obligation and policy");}
    const auto files=CatalogFiles(fixture.Catalog());Check(session->Commit(head,next,output)!=R::Ok&&Same(output,head)&&CatalogFiles(fixture.Catalog())==files,"uncertain session cannot blindly retry or discard already-written responsibility");session.reset();
    std::unique_ptr<A> blocked;Check(A::Open(blocked)==R::RecoveryRequired&&!blocked&&!m::WriteReservation(Entry(fixture.root,1),MiB)&&CatalogFiles(fixture.Catalog())==files,"fresh admission and legacy writer both refuse unresolved metadata without implicit recovery or refund");}}
} // namespace

int wmain(int argc,wchar_t**argv){try{if(argc==4&&std::wstring(argv[1])==L"--read-credit"){Select(argv[2]);auto session=Open();const auto wanted=std::stoull(argv[3]);Check(session->Current().Credit().checkpointPublications==wanted&&session->Current().Business().Source().IsCheckpoint()==(wanted==4),"child reads persistent exact head type and credit");return 0;}
    Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_source_admission_tests --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;
    Refusals(root);StaleCannotEraseDebt(root);DirectCatalogAdvance(root);LimitsAndPeak(root);LoweredAndDisabled(root);LivePolicyChanges(root);Maintenance(root);Uncertain(root);std::printf("PASS cache source admission checks=%u\n",checks);return 0;
}catch(const std::exception&error){fault=nullptr;std::fprintf(stderr,"FAIL cache source admission checks=%u: %s\n",checks,error.what());return 1;}}
