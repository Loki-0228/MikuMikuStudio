// MAINTENANCE: regression-keep. Real catalog plans, file peaks and failure receipts.
// Uses only a newly claimed --store directory; does not start MMS or decode media.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "mikumikustudio/cache_catalog.hpp"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace c=mikumikustudio::cache_catalog;
namespace f=mikumikustudio::cache_files;
using Bytes=std::vector<std::uint8_t>;
using R=c::Result;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"DIAG check=%u win32=%lu\n",checks,GetLastError());throw std::runtime_error(why);}}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;};
void NewDirectory(const fs::path& path){Check(path.is_absolute()&&path==path.lexically_normal()&&path!=path.root_path(),"absolute normalized private directory");Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"claim fresh private directory without reusing prior evidence");}
Bytes Read(const fs::path& path){std::ifstream in(path,std::ios::binary);Check(bool(in),"read own catalog evidence");return Bytes(std::istreambuf_iterator<char>(in),{});}
f::Owner Owner(){f::Owner id{};for(unsigned i=0;i<id.size();++i)id[i]=std::uint8_t(i+3);return id;}
bool Same(const c::Snapshot&a,const c::Snapshot&b){return a.IsValid()==b.IsValid()&&a.IsCheckpoint()==b.IsCheckpoint()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
bool Same(const c::RecoveryReport&a,const c::RecoveryReport&b){return a.confirmedPublication==b.confirmedPublication&&a.rolledBack==b.rolledBack&&a.revision==b.revision;}
bool Same(const c::CollectionReport&a,const c::CollectionReport&b){return a.attempted==b.attempted&&a.absent==b.absent&&a.busy==b.busy&&a.pending==b.pending&&a.failed==b.failed&&a.absentBytes==b.absentBytes;}
using Evidence=std::map<std::wstring,Bytes>;
Evidence Files(const fs::path& root){Evidence result;for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file()){
    const auto name=entry.path().lexically_relative(root).native();if(entry.path().filename()==L".lock"){Check(entry.file_size()==0,"planning lock file has no payload");continue;}result.emplace(name,Read(entry.path()));}return result;}
std::uint64_t Cluster(const fs::path& path){wchar_t mount[32768]{};DWORD sectors=0,bytes=0,free=0,total=0;Check(GetVolumePathNameW(path.c_str(),mount,32768)&&GetDiskFreeSpaceW(mount,&sectors,&bytes,&free,&total)&&sectors&&bytes,"query actual fixture volume allocation unit");return std::uint64_t(sectors)*bytes;}
std::wstring Prefix(const fs::path& path){Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t text[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,text,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"bind fault scope to actual owned volume path");return std::wstring(text,n)+L'\\';}

// Independent native enumeration, including metadata-only opens of the actual
// CREATE_NEW writer. No product accounting/helper participates in this oracle.
bool Measure(const wchar_t* directory,std::uint64_t& logical,std::uint64_t& charged,unsigned depth=0)noexcept {
    if(depth>12)return false;wchar_t pattern[32768]{};if(swprintf_s(pattern,L"%s\\*",directory)<0)return false;WIN32_FIND_DATAW item{};const auto find=FindFirstFileW(pattern,&item);
    if(find==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;bool ok=true;
    do {if(!std::wcscmp(item.cFileName,L".")||!std::wcscmp(item.cFileName,L".."))continue;wchar_t child[32768]{};if(swprintf_s(child,L"%s\\%s",directory,item.cFileName)<0){ok=false;break;}
        if(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){ok=false;break;}if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!Measure(child,logical,charged,depth+1)){ok=false;break;}continue;}
        const auto h=CreateFileW(child,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);FILE_STANDARD_INFO size{};
        if(h==INVALID_HANDLE_VALUE){ok=false;break;}const bool measured=GetFileInformationByHandleEx(h,FileStandardInfo,&size,sizeof size)!=FALSE;CloseHandle(h);
        if(!measured||size.EndOfFile.QuadPart<0||size.AllocationSize.QuadPart<0){ok=false;break;}logical+=std::uint64_t(size.EndOfFile.QuadPart);charged+=(std::max)(std::uint64_t(size.EndOfFile.QuadPart),std::uint64_t(size.AllocationSize.QuadPart));
    }while(FindNextFileW(find,&item));const auto error=GetLastError();FindClose(find);return ok&&error==ERROR_NO_MORE_FILES;
}
enum class Fault {None,CommitFlush,CommitPartial,IntentFlush,DecisionFlush};
struct Observation {
    fs::path root;std::wstring prefix;Fault fault=Fault::None;unsigned samples=0,hits=0,operationsFlushes=0,writes=0,flushes=0,creates=0;std::uint64_t peakLogical=0,peakCharged=0;bool valid=true;
    explicit Observation(const fs::path& path,Fault failure=Fault::None):root(path),prefix(Prefix(path)),fault(failure){Sample();}
    int Match(HANDLE h)const noexcept {wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);if(!n||n>=32768||n<=prefix.size()||_wcsnicmp(path,prefix.c_str(),prefix.size()))return 0;
        const auto* relative=path+prefix.size();return !std::wcsncmp(relative,L"commits\\",8)?1:!std::wcsncmp(relative,L"operations\\",11)?2:!std::wcsncmp(relative,L"snapshots\\",10)?3:0;}
    void Sample()noexcept{std::uint64_t logical=0,charged=0;if(!Measure(root.c_str(),logical,charged)){valid=false;return;}++samples;peakLogical=(std::max)(peakLogical,logical);peakCharged=(std::max)(peakCharged,charged);}
};
thread_local Observation* active=nullptr;
decltype(&WriteFile) realWrite=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&CreateFileW) realCreate=nullptr;
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE original)noexcept {const auto result=realCreate(path,access,sharing,security,disposition,flags,original);const auto error=GetLastError();auto* p=active;
    if(p&&result!=INVALID_HANDLE_VALUE&&disposition==CREATE_NEW&&p->Match(result)){++p->creates;p->Sample();}SetLastError(error);return result;}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD written,LPOVERLAPPED overlapped)noexcept {auto* p=active;const auto kind=p?p->Match(h):0;LARGE_INTEGER zero{},position{};
    if(p&&kind)++p->writes;
    if(p&&kind==1&&!p->hits&&p->fault==Fault::CommitPartial&&!overlapped&&count>1&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto result=realWrite(h,bytes,(std::min)(DWORD(17),count-1),written,overlapped);if(result){++p->hits;p->Sample();SetLastError(ERROR_WRITE_FAULT);return FALSE;}return result;}
    const auto result=realWrite(h,bytes,count,written,overlapped);const auto error=GetLastError();if(p&&kind)p->Sample();SetLastError(error);return result;}
BOOL WINAPI FlushHook(HANDLE h)noexcept {auto* p=active;const auto kind=p?p->Match(h):0;const auto result=realFlush(h);const auto error=GetLastError();if(p&&kind){++p->flushes;p->Sample();if(result&&kind==2)++p->operationsFlushes;
        if(result&&!p->hits&&((p->fault==Fault::CommitFlush&&kind==1)||((p->fault==Fault::IntentFlush||p->fault==Fault::DecisionFlush)&&kind==2))){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}}
    SetLastError(error);return result;}
struct Hooks {Hooks(){Check(MH_Initialize()==MH_OK,"initialize exact-file catalog test hooks");Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK,"observe actual catalog payload writes");Check(MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK,"observe actual catalog flush and bounded failures");Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"observe ephemeral candidate creation as well as surviving files");Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable catalog test hooks");}~Hooks(){active=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm {explicit Arm(Observation& value){Check(!active,"nonrecursive scoped catalog observation");active=&value;}~Arm(){active=nullptr;}};

struct Fixture {fs::path root;c::Store store;explicit Fixture(const fs::path& parent,const wchar_t* name):root(parent/name),store(root){NewDirectory(root);NewDirectory(root/L"snapshots");NewDirectory(root/L"commits");}
    c::Snapshot Initialize(const Bytes& bytes={1,2,3}){c::Snapshot result;Check(store.InitializeOwned(Owner(),bytes,result)==R::Ok,"initialize real baseline catalog using legacy entry");return result;}};
template<class F>R PlanWithoutWrites(Fixture& fixture,F&& call){const auto before=Files(fixture.root);Observation observed(fixture.root);R result;{Arm arm(observed);result=call();}
    Check(!observed.creates&&!observed.writes&&!observed.flushes&&Files(fixture.root)==before,"planning neither creates ephemeral candidates nor writes/flushes payload or intent");return result;}
std::uint64_t Reserved(const c::WritePlan& plan,std::uint64_t unit,bool allocation){const auto& footprint=plan.Footprint();Check(footprint.count<=footprint.files.size(),"bounded immutable file-footprint count");std::uint64_t total=0;
    for(unsigned i=0;i<footprint.count;++i){const auto bytes=footprint.files[i].maximumFileBytes;Check(bytes&&bytes<=c::MaximumMetadataBytes,"each new metadata file has finite nonzero upper bound");total+=allocation?((bytes+unit-1)/unit)*unit:bytes;}return total;}
void Peak(const c::WritePlan& plan,const Observation& observation,std::uint64_t beforeLogical,std::uint64_t beforeCharged){Check(observation.valid&&observation.samples>1,"all actual in-progress file observations completed");const auto unit=Cluster(observation.root);
    Check(observation.peakLogical<=beforeLogical+Reserved(plan,unit,false),"observed logical write peak fits immutable per-file footprint without subtracting old metadata");
    Check(observation.peakCharged<=beforeCharged+Reserved(plan,unit,true),"observed allocation peak fits independently cluster-rounded per-file reservation");}
void Baseline(const fs::path& path,std::uint64_t& logical,std::uint64_t& charged){logical=charged=0;Check(Measure(path.c_str(),logical,charged),"measure actual preexisting file costs independently");}
void Preserved(const Evidence& old,const Evidence& now){for(const auto& file:old){const auto found=now.find(file.first);Check(found!=now.end()&&found->second==file.second,"old metadata remains intact beside new generation until explicit collection");}}
bool Equal(const c::WriteFootprint&a,const c::WriteFootprint&b){if(a.count!=b.count)return false;for(unsigned i=0;i<a.files.size();++i)if(a.files[i].role!=b.files[i].role||a.files[i].maximumFileBytes!=b.files[i].maximumFileBytes)return false;return true;}
void Written(const c::WritePlan& plan,const c::WriteFootprint& before){Check(plan.IsConsumed()&&plan.MayHaveWritten()&&Equal(plan.Footprint(),before),"actual attempt consumes plan and preserves immutable footprint as evidence");}
void FootprintOracle(){c::WriteFootprint publication;Check(c::PublicationFootprint(123,publication)==R::Ok&&publication.count==4,"publication sizing exposes all four distinct potential files");
    const c::WriteRole roles[]{c::WriteRole::Snapshot,c::WriteRole::Commit,c::WriteRole::Intent,c::WriteRole::RollbackDecision};const std::uint64_t sizes[]{128+123,128+368,128+752,128+752};
    for(unsigned i=0;i<4;++i)Check(publication.files[i].role==roles[i]&&publication.files[i].maximumFileBytes==sizes[i],"independent wire sizes include envelope and separate rollback decision");
    const auto saved=publication;Check(c::PublicationFootprint(c::MaximumSnapshotBytes+1,publication)==R::Limit&&Equal(publication,saved),"oversized sizing preserves prior output");
    c::WriteFootprint checkpoint;Check(c::CheckpointFootprint(3,checkpoint)==R::Ok&&checkpoint.count==1&&checkpoint.files[0].role==c::WriteRole::Checkpoint&&checkpoint.files[0].maximumFileBytes==128+376+3*160,"independent exact checkpoint retirement-list size");
    const auto prior=checkpoint;Check(c::CheckpointFootprint(0,checkpoint)==R::Invalid&&Equal(checkpoint,prior)&&c::CheckpointFootprint(193,checkpoint)==R::Limit&&Equal(checkpoint,prior),"invalid checkpoint counts never alter sizing output");
    Check(c::MaximumCheckpointFootprint(checkpoint)==R::Ok&&checkpoint.count==1&&checkpoint.files[0].role==c::WriteRole::Checkpoint&&checkpoint.files[0].maximumFileBytes==128+376+192*160,"future checkpoint credit uses bounded full retired-list wire size");}

void SnapshotHeadKinds(const fs::path& root){
    Fixture fixture(root,L"head-kinds"),missing(root,L"head-kinds-missing");
    c::Snapshot invalid;
    Check(!invalid.IsValid()&&!invalid.IsCheckpoint(),"invalid snapshot is not a checkpoint");
    auto initial=fixture.Initialize({1,2});
    Check(initial.IsValid()&&!initial.IsCheckpoint(),"initialization publishes a normal commit head");
    c::Snapshot current;
    Check(fixture.store.Commit(initial,{3,4},current)==R::Ok&&!current.IsCheckpoint(),
          "ordinary commit has no checkpoint marker");
    const auto ordinary=current;
    Check(fixture.store.Checkpoint(current,current)==R::Ok&&current.IsCheckpoint(),
          "actual checkpoint publication reports its verified head kind");
    const auto checkpoint=current;
    c::Store reopened(fixture.root);c::Snapshot read;
    Check(reopened.Read(read)==R::Ok&&read.IsCheckpoint()&&Same(read,checkpoint),
          "fresh store read recovers checkpoint kind from the actual persisted head");
    Check(fixture.store.Commit(initial,{9},current)==R::Conflict&&current.IsCheckpoint()&&Same(current,checkpoint),
          "failed stale commit preserves prior checkpoint output");
    Check(missing.store.Read(current)==R::Missing&&current.IsCheckpoint()&&Same(current,checkpoint),
          "failed missing read cannot erase prior verified checkpoint marker");
    c::Snapshot moved(std::move(read));
    Check(moved.IsValid()&&moved.IsCheckpoint()&&Same(moved,checkpoint),
          "snapshot move construction transfers checkpoint head to its destination");
    c::Snapshot assigned=ordinary;assigned=std::move(moved);
    Check(assigned.IsCheckpoint()&&Same(assigned,checkpoint),
          "snapshot move assignment replaces an ordinary head with the checkpoint head");
    Check(fixture.store.Commit(assigned,{5,6},assigned)==R::Ok&&assigned.IsValid()&&!assigned.IsCheckpoint(),
          "normal commit after checkpoint does not inherit ancestor checkpoint kind");
    Check(reopened.Read(current)==R::Ok&&!current.IsCheckpoint()&&Same(current,assigned),
          "reopened store reads the later normal head as normal");
}

void ExpectedCollection(const fs::path& root){
    Fixture fixture(root,L"expected-collection"),foreign(root,L"foreign-collection");
    c::Snapshot invalid;auto initial=fixture.Initialize({4,5});auto alien=foreign.Initialize({4,5});
    Check(!invalid.SameVersion(invalid)&&!invalid.SameVersion(initial)&&!initial.SameVersion(invalid),
          "invalid snapshots never grant an exact-version match");
    Check(initial.Revision()==alien.Revision()&&initial.StoreId()==alien.StoreId()&&initial.Bytes()==alien.Bytes()&&
          !initial.SameVersion(alien)&&!alien.SameVersion(initial),
          "matching owner revision and payload in another physical root are not the same version");
    c::Snapshot read;c::Store reopened(fixture.root);
    Check(reopened.Read(read)==R::Ok&&initial.SameVersion(read)&&read.SameVersion(initial),
          "freshly verified same physical head compares equal in both directions");
    const auto copied=read;c::Snapshot moved(std::move(read));c::Snapshot assigned;assigned=std::move(moved);
    Check(copied.SameVersion(assigned),"copy and move destinations preserve the complete verified version");
    c::Snapshot ordinary,checkpoint;
    Check(fixture.store.Commit(initial,initial.Bytes(),ordinary)==R::Ok&&!ordinary.SameVersion(initial),
          "same payload in a later normal head is a distinct version");
    Check(fixture.store.Checkpoint(ordinary,checkpoint)==R::Ok&&!checkpoint.SameVersion(ordinary),
          "checkpoint version differs even though its payload remains unchanged");
    c::CollectionReport report{17,18,19,20,21,22};const auto saved=report;
    const auto before=Files(fixture.root),foreignBefore=Files(foreign.root);
    Check(fixture.store.CollectRetired(invalid,report)==R::Conflict&&Same(report,saved)&&Files(fixture.root)==before,
          "invalid collection expectation preserves report and every file");
    Check(fixture.store.CollectRetired(alien,report)==R::Conflict&&Same(report,saved)&&Files(fixture.root)==before&&Files(foreign.root)==foreignBefore,
          "foreign collection expectation cannot delete retired files or change its output");
    Check(fixture.store.CollectRetired(ordinary,report)==R::Conflict&&Same(report,saved)&&Files(fixture.root)==before,
          "stale ordinary head cannot authorize collection after checkpoint");
    const auto emptyRoot=root/L"collection-empty-origin";NewDirectory(emptyRoot);f::Directory emptyDirectory;
    Check(f::Directory::Open(emptyRoot,false,emptyDirectory)==f::Result::Ok,"open actual empty foreign directory without creating a catalog lock");
    c::Store emptyStore(std::move(emptyDirectory));
    Check(emptyStore.CollectRetired(checkpoint,report)==R::Conflict&&Same(report,saved)&&fs::is_empty(emptyRoot),
          "bound foreign empty root rejects physical identity before creating even a lock");
    Check(DeleteFileW((fixture.root/L".lock").c_str())!=FALSE,"remove fixture's inactive empty catalog lock for missing-lock case");
    Check(fixture.store.CollectRetired(checkpoint,report)==R::Missing&&Same(report,saved)&&
          !fs::exists(fixture.root/L".lock")&&Files(fixture.root)==before,
          "expected collection refuses missing lock without recreating it or deleting metadata");
    Check(reopened.Read(read)==R::Ok&&read.SameVersion(checkpoint),"legacy read explicitly restores its ordinary lock while preserving verified head");
    c::Snapshot advanced;
    Check(fixture.store.Commit(checkpoint,{9,10},advanced)==R::Ok,"actual independent head advances after expected checkpoint was captured");
    const auto advancedFiles=Files(fixture.root);
    Check(fixture.store.CollectRetired(checkpoint,report)==R::Conflict&&Same(report,saved)&&Files(fixture.root)==advancedFiles,
          "check-then-collect race is rejected by exact head check inside catalog lock");
    Check(fixture.store.CollectRetired(advanced,report)==R::Ok&&report.attempted>0&&report.absent==report.attempted&&
          !report.busy&&!report.pending&&!report.failed&&report.absentBytes>0,
          "matching current normal head authorizes only its validated inherited retirement list");
    Check(reopened.Read(read)==R::Ok&&read.SameVersion(advanced)&&read.Bytes()==Bytes({9,10})&&Files(fixture.root).size()<advancedFiles.size(),
          "expected collection removes retired metadata while preserving exact current publication");
    c::Snapshot nextCheckpoint;Check(fixture.store.Checkpoint(read,nextCheckpoint)==R::Ok,"prepare exact checkpoint-head collection");
    Check(fixture.store.CollectRetired(nextCheckpoint,report)==R::Ok&&report.attempted>0&&report.absent==report.attempted,
          "matching checkpoint head itself also grants exact collection");
    Check(fixture.store.CollectRetired(report)==R::Ok&&!report.attempted,
          "legacy unbound collection remains available and reports an already collected list");
}

void Publications(const fs::path& root){Fixture fixture(root,L"publications");c::WritePlan initialize;Bytes input(32769,0x6d);const auto original=input;const auto empty=Files(fixture.root);
    Check(PlanWithoutWrites(fixture,[&]{return fixture.store.PlanInitializeOwned(Owner(),input,initialize);})==R::Ok&&initialize.IsValid()&&initialize.Kind()==c::WriteKind::Initialize&&!initialize.IsConsumed()&&!initialize.MayHaveWritten(),"plan owned initialization without publishing");
    Check(Files(fixture.root)==empty,"initial planning creates no payload or intent");const auto initFootprint=initialize.Footprint();input.assign(7,0x42);
    std::uint64_t logical=0,charged=0;Baseline(fixture.root,logical,charged);Observation init(fixture.root);c::Snapshot current;R result;{Arm arm(init);result=fixture.store.Execute(initialize,current);}
    Check(result==R::Ok&&current.Revision()==1&&current.StoreId()==Owner()&&current.Bytes()==original,"initialize executes immutable copied payload and chosen owner");Written(initialize,initFootprint);Peak(initialize,init,logical,charged);
    const auto after=Files(fixture.root);const auto saved=current;Check(fixture.store.Execute(initialize,current)==R::Conflict&&Same(current,saved)&&Files(fixture.root)==after,"consumed initialization cannot be executed twice");
    c::WritePlan commit;Bytes next(65539,0x28);const auto desired=next;const auto old=Files(fixture.root);
    Check(PlanWithoutWrites(fixture,[&]{return fixture.store.PlanCommit(current,next,commit);})==R::Ok&&commit.Kind()==c::WriteKind::Commit&&!commit.MayHaveWritten()&&Files(fixture.root)==old,"commit planning preserves all old metadata without creating candidates");
    const auto footprint=commit.Footprint();next.clear();Baseline(fixture.root,logical,charged);Observation observed(fixture.root);{Arm arm(observed);result=fixture.store.Execute(commit,current);}
    Check(result==R::Ok&&current.Revision()==2&&current.Bytes()==desired,"planned commit permits expected/output alias semantics and preserves copied inputs");Written(commit,footprint);Peak(commit,observed,logical,charged);Preserved(old,Files(fixture.root));
    c::Snapshot verified;Check(fixture.store.Read(verified)==R::Ok&&Same(current,verified),"actual catalog reread verifies planned publication");
    c::WritePlan checkpoint;const auto oldCheckpoint=Files(fixture.root);Check(PlanWithoutWrites(fixture,[&]{return fixture.store.PlanCheckpoint(current,checkpoint);})==R::Ok&&checkpoint.Kind()==c::WriteKind::Checkpoint&&Files(fixture.root)==oldCheckpoint,"checkpoint plan computes retirement without publishing or collecting old files");
    const auto checkpointFootprint=checkpoint.Footprint();Baseline(fixture.root,logical,charged);Observation measured(fixture.root);{Arm arm(measured);result=fixture.store.Execute(checkpoint,current);}
    Check(result==R::Ok&&current.Revision()==3&&current.Bytes()==desired,"planned checkpoint keeps published payload");Written(checkpoint,checkpointFootprint);Peak(checkpoint,measured,logical,charged);Preserved(oldCheckpoint,Files(fixture.root));
    c::WritePlan nothing;const auto clean=Files(fixture.root);Check(PlanWithoutWrites(fixture,[&]{return fixture.store.PlanRecovery(nothing);})==R::Ok&&nothing.Kind()==c::WriteKind::Recovery&&nothing.Footprint().count==0&&Files(fixture.root)==clean,"clean recovery plan has zero file allocation and no writes");
    c::RecoveryReport report{true,true,999};Check(fixture.store.ExecuteRecovery(nothing,report)==R::Ok&&report.revision==current.Revision()&&!report.rolledBack&&!report.confirmedPublication&&Files(fixture.root)==clean,"zero-write recovery returns actual current revision without metadata creation");
    Check(nothing.IsConsumed()&&!nothing.MayHaveWritten(),"successful no-op recovery is consumed without claiming a mutation");
}

void PreflightAndOrigins(const fs::path& root){Fixture fixture(root,L"preflight"),other(root,L"other-origin");auto before=fixture.Initialize();auto sentinel=other.Initialize({0x72});c::WritePlan plan;
    Check(fixture.store.PlanCommit(before,{5,6,7},plan)==R::Ok,"prepare immutable commit for origin and head tests");const auto footprint=plan.Footprint();const auto originalFiles=Files(fixture.root),otherFiles=Files(other.root);const auto outputBefore=sentinel;
    Check(other.store.Execute(plan,sentinel)==R::Conflict&&Same(sentinel,outputBefore)&&Files(fixture.root)==originalFiles&&Files(other.root)==otherFiles,"different physical catalog refuses plan without touching either root or output");
    Check(!plan.IsConsumed()&&!plan.MayHaveWritten()&&Equal(plan.Footprint(),footprint),"wrong-origin preflight preserves executable plan");
    const auto emptyRoot=root/L"empty-execution-origin";NewDirectory(emptyRoot);c::Store emptyStore(emptyRoot);Check(fs::is_empty(emptyRoot),"wrong execution root starts entirely empty without a lock or child directories");
    Check(emptyStore.Execute(plan,sentinel)==R::Conflict&&Same(sentinel,outputBefore)&&fs::is_empty(emptyRoot)&&Files(fixture.root)==originalFiles,"wrong empty root rejects execution before creating even a lock file or metadata directory");
    Check(!plan.IsConsumed()&&!plan.MayHaveWritten()&&Equal(plan.Footprint(),footprint),"empty-root preflight preserves original unspent plan and its footprint");
    Fixture initializing(root,L"empty-origin-initialization-source");c::WritePlan initialPlan,checkpointPlan,recoveryPlan;
    Check(initializing.store.PlanInitializeOwned(Owner(),{1},initialPlan)==R::Ok&&fixture.store.PlanCheckpoint(before,checkpointPlan)==R::Ok&&fixture.store.PlanRecovery(recoveryPlan)==R::Ok,"prepare other execution kinds against their legitimate existing roots");
    Check(emptyStore.Execute(initialPlan,sentinel)==R::Conflict&&Same(sentinel,outputBefore)&&fs::is_empty(emptyRoot)&&!initialPlan.IsConsumed()&&!initialPlan.MayHaveWritten(),"initialization plan cannot create a lock or initialize a different empty root");
    Check(emptyStore.Execute(checkpointPlan,sentinel)==R::Conflict&&Same(sentinel,outputBefore)&&fs::is_empty(emptyRoot)&&!checkpointPlan.IsConsumed()&&!checkpointPlan.MayHaveWritten(),"checkpoint plan rejects empty foreign root before any write");
    c::RecoveryReport emptyReport{true,true,871};const auto emptyReportBefore=emptyReport;
    Check(emptyStore.ExecuteRecovery(recoveryPlan,emptyReport)==R::Conflict&&Same(emptyReport,emptyReportBefore)&&fs::is_empty(emptyRoot)&&!recoveryPlan.IsConsumed()&&!recoveryPlan.MayHaveWritten(),"recovery plan also preserves empty foreign namespace and caller report");
    const auto moved=fs::path(fixture.root.native()+L"-moved");const auto renamed=MoveFileExW(fixture.root.c_str(),moved.c_str(),MOVEFILE_WRITE_THROUGH);const auto renameError=GetLastError();
    Check(!renamed&&(renameError==ERROR_SHARING_VIOLATION||renameError==ERROR_ACCESS_DENIED),"plan directory guards prevent live root replacement");
    c::Snapshot newer;Check(fixture.store.Commit(before,{8,9},newer)==R::Ok,"planning does not hold publication lock or prevent independent head advance");const auto advanced=Files(fixture.root);
    Check(fixture.store.Execute(plan,sentinel)==R::Conflict&&Same(sentinel,outputBefore)&&Files(fixture.root)==advanced&&!plan.IsConsumed()&&!plan.MayHaveWritten(),"stale head is rejected before any candidate/intent creation");
    c::WritePlan retained;Check(fixture.store.PlanCommit(newer,{10},retained)==R::Ok,"create valid output sentinel plan");const auto retainedFootprint=retained.Footprint();f::Owner zero{};
    Check(fixture.store.PlanInitializeOwned(zero,{1},retained)==R::Invalid&&retained.IsValid()&&retained.Kind()==c::WriteKind::Commit&&Equal(retained.Footprint(),retainedFootprint)&&!retained.IsConsumed(),"invalid planning input preserves existing immutable output plan");
    Check(fixture.store.PlanCommit(outputBefore,{1},retained)==R::Conflict&&Equal(retained.Footprint(),retainedFootprint)&&Files(fixture.root)==advanced,"foreign expected snapshot rejects planning and preserves output and disk");
    c::WritePlan empty;c::RecoveryReport report{true,false,79};const auto reportBefore=report;
    Check(fixture.store.Execute(empty,sentinel)==R::Invalid&&Same(sentinel,outputBefore),"empty plan preserves snapshot output");
    Check(fixture.store.ExecuteRecovery(retained,report)==R::Invalid&&Same(report,reportBefore)&&!retained.IsConsumed()&&!retained.MayHaveWritten(),"wrong executor preserves report and valid publication plan");
    c::WritePlan movedPlan(std::move(retained));Check(!retained.IsValid()&&movedPlan.IsValid()&&Equal(movedPlan.Footprint(),retainedFootprint),"move transfers sole plan ownership without changing footprint");
    Check(fixture.store.Execute(movedPlan,sentinel)==R::Ok&&sentinel.Bytes()==Bytes({10}),"preflight errors and move do not poison still-current valid plan");
    c::WritePlan recovery;Check(fixture.store.PlanRecovery(recovery)==R::Ok,"plan clean recovery against exact head");const auto recoveryHead=sentinel;
    Check(fixture.store.Commit(recoveryHead,{11},newer)==R::Ok,"advance head after recovery planning");const auto changed=Files(fixture.root);
    Check(fixture.store.ExecuteRecovery(recovery,report)==R::Conflict&&Same(report,reportBefore)&&!recovery.IsConsumed()&&!recovery.MayHaveWritten()&&Files(fixture.root)==changed,"recovery also rejects changed head without implicit replan or mutation");
    Fixture race(root,L"initialize-race");c::WritePlan first;Check(race.store.PlanInitialize({1},first)==R::Ok,"plan generated-owner initialization");c::Snapshot winner;
    Check(race.store.InitializeOwned(Owner(),{2},winner)==R::Ok,"another initializer wins after first plan");const auto raceFiles=Files(race.root);
    Check(race.store.Execute(first,sentinel)==R::Conflict&&!first.IsConsumed()&&!first.MayHaveWritten()&&Files(race.root)==raceFiles,"lost initialization race cannot overwrite winning catalog");
}

void Recovery(const fs::path& root){for(unsigned mode=0;mode<3;++mode){Fixture fixture(root,(L"recovery-"+std::to_wstring(mode)).c_str());const auto baseline=fixture.Initialize();auto output=baseline;c::WritePlan publication;
    const Bytes candidate(32771,std::uint8_t(0xa1+mode));Check(fixture.store.PlanCommit(baseline,candidate,publication)==R::Ok,"plan candidate for actual interrupted publication");const auto footprint=publication.Footprint();
    std::uint64_t logical=0,charged=0;Baseline(fixture.root,logical,charged);Observation fault(fixture.root,mode==0?Fault::CommitFlush:Fault::CommitPartial);R result;
    {Arm arm(fault);result=fixture.store.Execute(publication,output);}Check(fault.hits==1&&result==R::Indeterminate&&Same(output,baseline),"real commit failure keeps old output and reports uncertain publication");Written(publication,footprint);Peak(publication,fault,logical,charged);
    const auto interrupted=Files(fixture.root);c::WritePlan recovery;Check(PlanWithoutWrites(fixture,[&]{return fixture.store.PlanRecovery(recovery);})==R::Ok&&Files(fixture.root)==interrupted,"recovery planning inspects exact intent without flushing, deleting or creating decision");
    const auto recoveryFootprint=recovery.Footprint();c::RecoveryReport report{true,true,918},reportBefore=report;Baseline(fixture.root,logical,charged);Observation recovering(fixture.root,mode==2?Fault::DecisionFlush:Fault::None);
    {Arm arm(recovering);result=fixture.store.ExecuteRecovery(recovery,report);}Written(recovery,recoveryFootprint);Peak(recovery,recovering,logical,charged);
    if(mode==2){Check(recovering.hits==1&&result!=R::Ok&&Same(report,reportBefore),"failed actual rollback-decision flush preserves recovery output and spent-plan evidence");
        const auto failed=Files(fixture.root);c::WritePlan retry;Check(fixture.store.PlanRecovery(retry)==R::Ok&&Files(fixture.root)==failed,"explicit replanning retains persisted rollback evidence without automatic refund");
        Baseline(fixture.root,logical,charged);Observation retried(fixture.root);{Arm arm(retried);result=fixture.store.ExecuteRecovery(retry,report);}Check(result==R::Ok,"new exact recovery plan resumes failed rollback");Peak(retry,retried,logical,charged);
    }else Check(result==R::Ok,"actual recovery executes within planned footprint");
    c::Snapshot actual;Check(fixture.store.Read(actual)==R::Ok,"reread recovered real catalog");
    Check(mode==0?(report.confirmedPublication&&!report.rolledBack&&actual.Bytes()==candidate&&actual.Revision()==baseline.Revision()+1):(report.rolledBack&&!report.confirmedPublication&&Same(actual,baseline)),"confirmed and rolled-back recoveries preserve original publication protocol");
}
}

void InitializationFailures(const fs::path& root){Fixture sentinelFixture(root,L"init-output-sentinel");const auto sentinel=sentinelFixture.Initialize({0x9a});for(unsigned mode=0;mode<2;++mode){Fixture fixture(root,(L"initialize-failure-"+std::to_wstring(mode)).c_str());c::WritePlan initialize;auto output=sentinel;
    Check(fixture.store.PlanInitializeOwned(Owner(),Bytes(4099,0x27),initialize)==R::Ok,"plan isolated initialization fault");const auto footprint=initialize.Footprint();std::uint64_t logical=0,charged=0;Baseline(fixture.root,logical,charged);
    Observation fault(fixture.root,mode?Fault::CommitPartial:Fault::IntentFlush);R result;{Arm arm(fault);result=fixture.store.Execute(initialize,output);}
    Check(fault.hits==1&&result==(mode?R::Indeterminate:R::Io)&&Same(output,sentinel),"initialization failure does not replace prior caller snapshot");Written(initialize,footprint);Peak(initialize,fault,logical,charged);
    c::WritePlan recovery;const auto interrupted=Files(fixture.root);if(!mode){Check(!interrupted.empty()&&PlanWithoutWrites(fixture,[&]{return fixture.store.PlanRecovery(recovery);})==R::RecoveryRequired&&!recovery.IsValid()&&Files(fixture.root)==interrupted,"lost intent leaves unknown prepared files: recovery must refuse rather than invent authority or refund");continue;}
    Check(PlanWithoutWrites(fixture,[&]{return fixture.store.PlanRecovery(recovery);})==R::Ok&&Files(fixture.root)==interrupted,"failed initialization plans recovery only from its exact durable intent");c::RecoveryReport report;
    Check(fixture.store.ExecuteRecovery(recovery,report)==R::Ok&&report.rolledBack&&report.revision==0,"owned initialization rollback returns to missing catalog");
    Check(fixture.store.Read(output)==R::Missing&&Same(output,sentinel),"missing catalog read after rollback preserves caller snapshot");
    c::WritePlan retry;Check(fixture.store.PlanInitializeOwned(Owner(),{4,5},retry)==R::Ok&&fixture.store.Execute(retry,output)==R::Ok&&output.Revision()==1&&output.Bytes()==Bytes({4,5}),"explicit fresh plan can initialize after verified complete rollback");
}}

} // namespace

int wmain(int argc,wchar_t**argv){try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_catalog_plan_tests --store NEW_ABSOLUTE_DIRECTORY");const fs::path root(argv[2]);NewDirectory(root);Hooks hooks;
    FootprintOracle();SnapshotHeadKinds(root);ExpectedCollection(root);Publications(root);PreflightAndOrigins(root);Recovery(root);InitializationFailures(root);std::printf("PASS cache catalog plan checks=%u\n",checks);return 0;
}catch(const std::exception&error){std::fprintf(stderr,"FAIL cache catalog plan checks=%u: %s\n",checks,error.what());return 1;}}
