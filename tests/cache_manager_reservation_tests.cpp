#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <MinHook.h>
#include "mikumikustudio/cache_manager.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

namespace { thread_local bool failNextAllocation=false; std::atomic<bool> allocationFailed{false}; }
void* operator new(std::size_t size) {
    if(failNextAllocation){failNextAllocation=false;allocationFailed=true;throw std::bad_alloc();}
    if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

namespace fs=std::filesystem;
namespace cache=mikumikustudio::cache_manager;
namespace {
constexpr std::uint64_t MiB=1024ull*1024;
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAIL %s error=%lu\n",message,GetLastError());throw std::runtime_error(message);}}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
using MoveFn=BOOL(WINAPI*)(LPCWSTR,LPCWSTR,DWORD);
using MutexFn=HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES,BOOL,LPCWSTR);
using WaitFn=DWORD(WINAPI*)(HANDLE,DWORD);
using AttributesFn=BOOL(WINAPI*)(LPCWSTR,GET_FILEEX_INFO_LEVELS,LPVOID);
using DeleteFn=decltype(&SetFileInformationByHandle);
MoveFn realMove=nullptr;MutexFn realMutex=nullptr;WaitFn realWait=nullptr;
AttributesFn realAttributes=nullptr;DeleteFn realDelete=nullptr;
decltype(&CreateFileW) realCreate=nullptr;
std::wstring swapPath, displacedPath;
bool swapOnDeleteOpen=false, swappedAtDeleteOpen=false;
constexpr char replacementBytes[] = "unrelated file, keep these bytes";

HANDLE WINAPI CreateFault(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,
                          DWORD disposition,DWORD flags,HANDLE templateFile) {
    if (swapOnDeleteOpen && (access & DELETE) && path && _wcsicmp(path,swapPath.c_str())==0) {
        swapOnDeleteOpen=false;
        Check(MoveFileW(path,displacedPath.c_str())!=FALSE,"move the observed owned cache file at the delete boundary");
        Handle replacement(realCreate(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        DWORD written=0;
        Check(replacement.value!=INVALID_HANDLE_VALUE &&
              ::WriteFile(replacement.value,replacementBytes,sizeof replacementBytes,&written,nullptr) &&
              written==sizeof replacementBytes,"place a different file at the same cache name");
        LARGE_INTEGER length{};
        length.QuadPart=MiB;
        Check(SetFilePointerEx(replacement.value,length,nullptr,FILE_BEGIN) && SetEndOfFile(replacement.value),
              "replacement has the same length but a different file identity");
        swappedAtDeleteOpen=true;
    }
    return realCreate(path,access,share,security,disposition,flags,templateFile);
}
std::wstring blockedIndex;
std::atomic<unsigned> blockedIndexMoves{0};
decltype(&DeviceIoControl) realIoControl = nullptr;
bool denyOwnerJournal = false;
unsigned deniedJournalReads = 0;
BOOL WINAPI JournalFault(HANDLE h,DWORD code,LPVOID input,DWORD inSize,LPVOID output,DWORD outSize,
                         LPDWORD returned,LPOVERLAPPED async) {
    if (denyOwnerJournal && code == FSCTL_QUERY_USN_JOURNAL) {
        ++deniedJournalReads; SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
    }
    return realIoControl(h,code,input,inSize,output,outSize,returned,async);
}
bool rejectFinalIndexRead = false;
unsigned renamedBeforeReadFault = 0;
std::wstring uncertainDeletePath;
std::wstring unreadableAttributesPath;
std::atomic<bool> uncertainDelete{false},deleteObserved{false};
std::atomic<bool> unreadableAttributes{false};
std::atomic<unsigned> unreadableAttributesHits{0};
std::atomic<bool> failIndex{false},injectAfterMutex{false};
std::atomic<HANDLE> mutexToFault{nullptr};
BOOL WINAPI MoveFault(LPCWSTR from,LPCWSTR to,DWORD flags){
    if(failIndex.load()&&to&&blockedIndex==to){++blockedIndexMoves;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    const auto moved = realMove(from,to,flags);
    if (moved && rejectFinalIndexRead && to && blockedIndex == to) {
        ++renamedBeforeReadFault;
        unreadableAttributes = true;
    }
    return moved;
}
HANDLE WINAPI MutexFault(LPSECURITY_ATTRIBUTES security,BOOL owned,LPCWSTR name){
    const auto result=realMutex(security,owned,name);
    if(injectAfterMutex.load()&&name&&std::wcsncmp(name,L"Local\\MMS-cache-",16)==0)mutexToFault=result;
    return result;
}
DWORD WINAPI WaitFault(HANDLE handle,DWORD timeout){
    const auto result=realWait(handle,timeout);
    if(handle==mutexToFault.load()&&(result==WAIT_OBJECT_0||result==WAIT_ABANDONED)&&injectAfterMutex.exchange(false))
        failNextAllocation=true;
    return result;
}
BOOL WINAPI DeleteFault(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID info,DWORD size) {
    bool target=false;
    if (kind==FileDispositionInfo && uncertainDelete.load()) {
        wchar_t path[1024]{};
        const auto n=GetFinalPathNameByHandleW(file,path,_countof(path),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        target=n>4 && n<_countof(path) && _wcsicmp(path+4,uncertainDeletePath.c_str())==0;
    }
    const auto result=realDelete(file,kind,info,size);
    if(result&&target)deleteObserved=true;
    return result;
}
BOOL WINAPI AttributesFault(LPCWSTR path,GET_FILEEX_INFO_LEVELS level,LPVOID output){
    if(unreadableAttributes.load()&&path&&_wcsicmp(path,unreadableAttributesPath.c_str())==0){
        unreadableAttributesHits.fetch_add(1);
        SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    if(uncertainDelete.load()&&deleteObserved.load()&&path&&_wcsicmp(path,uncertainDeletePath.c_str())==0){
        SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    return realAttributes(path,level,output);
}
struct Hooks {
    Hooks(){
        Check(MH_Initialize()==MH_OK,"initialize scoped cache fault hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&DeviceIoControl),reinterpret_cast<void*>(&JournalFault),
              reinterpret_cast<void**>(&realIoControl))==MH_OK,"inject only unavailability of the owner journal witness");
        Check(MH_CreateHook(reinterpret_cast<void*>(&MoveFileExW),reinterpret_cast<void*>(&MoveFault),reinterpret_cast<void**>(&realMove))==MH_OK,"hook only index publication failure");
        Check(MH_CreateHook(reinterpret_cast<void*>(&CreateMutexW),reinterpret_cast<void*>(&MutexFault),reinterpret_cast<void**>(&realMutex))==MH_OK,"identify real cache mutex");
        Check(MH_CreateHook(reinterpret_cast<void*>(&WaitForSingleObject),reinterpret_cast<void*>(&WaitFault),reinterpret_cast<void**>(&realWait))==MH_OK,"inject allocation after real mutex acquisition");
        Check(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&DeleteFault),reinterpret_cast<void**>(&realDelete))==MH_OK,"observe only fixture deletion");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetFileAttributesExW),reinterpret_cast<void*>(&AttributesFault),reinterpret_cast<void**>(&realAttributes))==MH_OK,"inject uncertain post-delete observation");
        Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateFault),
              reinterpret_cast<void**>(&realCreate))==MH_OK,"hook the precise cache deletion open");
        Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable cache-only fault hooks");
    }
    ~Hooks(){failIndex=false;injectAfterMutex=false;uncertainDelete=false;unreadableAttributes=false;unreadableAttributesPath.clear();unreadableAttributesHits=0;failNextAllocation=false;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}
};
void SetRoot(const fs::path& root){
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",root.c_str())!=FALSE,"isolate cache root");
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(root.parent_path()/L"settings.json").c_str())!=FALSE,"isolate central settings");
}
fs::path Case(const fs::path& output,const wchar_t* name,bool automatic=false){
    const auto root=output/name/L"Cache";SetRoot(root);
    cache::Settings settings;settings.limitBytes=64*MiB;settings.automaticCleanup=automatic;
    std::wstring error;Check(cache::Configure(settings,error),"configure isolated 64 MiB limit");return root;
}
fs::path Entry(unsigned id){wchar_t name[65]{};swprintf_s(name,L"%064x",id);return cache::Root()/L"Textures"/L"shared-texture"/(std::wstring(name)+L".mtc");}
void WriteFile(const fs::path& path,std::uint64_t bytes,cache::WriteReservation& token){
    const fs::path temporary=path.wstring()+L".tmp-fixture";
    fs::create_directories(path.parent_path());
    Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(file.value!=INVALID_HANDLE_VALUE,"create own cache payload");
    Check(token.TrackTemporary(temporary),"track own empty temporary before writing");
    DWORD count=0;Check(DeviceIoControl(file.value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&count,nullptr)!=FALSE,"use sparse fixture without allocating full payload");
    std::uint32_t header[19]{};header[0]=0x4354504d;header[1]=2;header[2]=static_cast<std::uint32_t>(bytes-76);
    Check(::WriteFile(file.value,header,sizeof(header),&count,nullptr)&&count==sizeof(header),"write recognized texture cache header");
    LARGE_INTEGER length{};length.QuadPart=bytes;
    Check(SetFilePointerEx(file.value,length,nullptr,FILE_BEGIN)&&SetEndOfFile(file.value),"set logical sparse payload size");
    Check(CloseHandle(file.value)!=FALSE,"close staged payload");file.value=INVALID_HANDLE_VALUE;
    Check(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"publish tracked payload");
}
std::uint64_t IndexCharge(){
    Handle file(CreateFileW((cache::Root()/L"Management"/L"index.bin").c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(file.value!=INVALID_HANDLE_VALUE,"open index metadata for independent accounting");
    FILE_STANDARD_INFO info{};Check(GetFileInformationByHandleEx(file.value,FileStandardInfo,&info,sizeof info)!=FALSE,"read actual index allocation");
    return (std::max)(static_cast<std::uint64_t>(info.EndOfFile.QuadPart),static_cast<std::uint64_t>(info.AllocationSize.QuadPart));
}
void Store(unsigned id,std::uint64_t bytes){const auto path=Entry(id);cache::WriteReservation token(path,bytes);Check(bool(token),"reserve initial payload");WriteFile(path,bytes,token);Check(token.Commit(),"persist initial payload registration");}
void PeakAndNested(const fs::path& output){
    const auto root=Case(output,L"peak");Store(1,40*MiB);cache::ResetScene();
    Check(!cache::WriteReservation(Entry(1),40*MiB),"rewrite counts both old target and new staging payload");
    auto settings=cache::Current();settings.automaticCleanup=true;std::wstring error;
    Check(cache::Configure(settings,error),"enable cleanup for protected rewrite target");
    Check(!cache::WriteReservation(Entry(1),40*MiB)&&fs::file_size(Entry(1))==40*MiB,"automatic eviction cannot delete rewrite target to make its own room");
    Case(output,L"nested");
    auto first=std::make_unique<cache::WriteReservation>(Entry(1),40*MiB);Check(bool(*first),"reserve first uncommitted payload");
    Check(!cache::WriteReservation(Entry(2),40*MiB),"same-thread nested reservations count pending bytes");
    Check(!cache::WriteReservation(Entry(1),MiB),"active same-name publication is rejected");
    cache::WriteReservation moved(std::move(*first));Check(bool(moved)&&!bool(*first),"move transfers reservation responsibility");first.reset();
    {
        cache::WriteReservation second(Entry(2),23*MiB);Check(bool(second),"pending data leaves room for both index generations");
        Check(!cache::WriteReservation(Entry(3),MiB),"pending data plus metadata rejects another MiB");
        second=std::move(moved);Check(bool(second)&&!bool(moved),"move assignment refunds replaced reservation once");
        cache::WriteReservation spare(Entry(3),23*MiB);Check(bool(spare),"move-assigned charge is neither leaked nor duplicated");
    }
    Check(bool(cache::WriteReservation(Entry(4),63*MiB)),"abandoned reservations return capacity except metadata peak");
    Case(output,L"commit");
    cache::WriteReservation publishing(Entry(1),40*MiB);Check(bool(publishing),"reserve committed case");
    WriteFile(Entry(1),40*MiB,publishing);Check(publishing.Commit()&&publishing.Commit(),"successful commit is idempotent");
    cache::WriteReservation remaining(Entry(2),23*MiB);Check(bool(remaining),"commit replaces staging charge with registered bytes");
    Check(!cache::WriteReservation(Entry(3),MiB),"commit does not refund registered file bytes");
}
void RootLifetime(const fs::path& output){
    const auto a=Case(output,L"root-a");
    auto first=std::make_unique<cache::WriteReservation>(Entry(1),40*MiB);Check(bool(*first),"retain root A pending writer");
    Case(output,L"root-b");
    cache::WriteReservation other(Entry(1),63*MiB);Check(bool(other),"root B has independent capacity");
    SetRoot(a);
    Check(!cache::WriteReservation(Entry(2),40*MiB),"returning to A reuses its still-live pending accounting");
    WriteFile(Entry(1),40*MiB,*first);Check(first->Commit(),"old guard stays valid after root switch");first.reset();
    Check(cache::Inspect().totalBytes==40*MiB+IndexCharge(),"published file belongs to original root");
}
void FailedRegistration(const fs::path& output){
    const auto root=Case(output,L"registration-failure");blockedIndex=(root/L"Management"/L"index.bin").wstring();
    auto token=std::make_unique<cache::WriteReservation>(Entry(1),20*MiB);Check(bool(*token),"reserve registration failure candidate");
    WriteFile(Entry(1),20*MiB,*token);failIndex=true;
    Check(!token->Commit(),"index publication failure is observable");
    Check(!cache::WriteReservation(Entry(2),MiB),"live unregistered publication blocks new reservations");
    token.reset();
    Check(!cache::WriteReservation(Entry(2),MiB),"destruction does not refund published unregistered bytes");
    Case(output,L"registration-other-root");SetRoot(root);
    Check(!cache::WriteReservation(Entry(2),MiB),"root changes cannot discard unresolved publication responsibility");
    failIndex=false;
    cache::WriteReservation remaining(Entry(2),43*MiB);Check(bool(remaining),"successful reconciliation persists file before releasing provisional charge");
    Check(!cache::WriteReservation(Entry(3),MiB)&&cache::Inspect().totalBytes==20*MiB+IndexCharge(),"reconciled payload remains charged exactly once");
}
void OrphanRetryOwnership(const fs::path& output){
    // Regression: a failed index publication leaves an orphaned reservation.
    // Reopening scene B must not let the ordinary reconciliation retry count as
    // B's use. Both B's empty project row and the historical baseline H are
    // persisted before the fault, so the test does not depend on replaying state
    // that a failed index move never wrote to disk.
    struct FileIdentity{DWORD volume=0,high=0,low=0;};
    const auto identity=[](const fs::path& path){
        Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        Check(file.value!=INVALID_HANDLE_VALUE,"identify fixture payload without taking a cache lease");
        BY_HANDLE_FILE_INFORMATION info{};Check(GetFileInformationByHandle(file.value,&info)!=FALSE,"read fixture volume and file index");
        return FileIdentity{info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};
    };
    const auto sameFile=[](const FileIdentity& a,const FileIdentity& b){return a.volume==b.volume&&a.high==b.high&&a.low==b.low;};
    const auto canDelete=[](const fs::path& path,DWORD& error){
        error=ERROR_SUCCESS;
        Handle probe(CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(probe.value==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
        return true;
    };
    const auto projectRow=[](const cache::Snapshot& snapshot,const fs::path& project){
        const auto expected=fs::absolute(project).lexically_normal().wstring();
        for(const auto& row:snapshot.projects)if(_wcsicmp(fs::path(row.path).lexically_normal().c_str(),expected.c_str())==0)return row;
        Check(false,"inspect exposes expected project identity row");return cache::Project{};
    };
    const auto root=Case(output,L"orphan-retry");
    const auto projectA=output/L"orphan-retry"/L"ProjectA";
    const auto projectB=output/L"orphan-retry"/L"ProjectB";
    const auto path=Entry(1);
    const auto history=Entry(10);
    // Persist B's empty row while the index is healthy so it stays queryable even
    // if a retry later reloads the index from disk.
    cache::BeginProject(projectB.c_str());cache::ProjectOpened(projectB.c_str());
    cache::ResetScene();
    // Persist the historical baseline H owned by A before introducing the fault.
    cache::BeginProject(projectA.c_str());cache::ProjectOpened(projectA.c_str());
    Store(10,MiB);
    const auto historyIdentity=identity(history);
    const auto baseline=cache::Inspect();
    Check(baseline.error.empty()&&baseline.textureBytes==MiB&&baseline.totalBytes==MiB+IndexCharge(),"baseline persists one historical MiB and metadata");
    const auto baselineA=projectRow(baseline,projectA);const auto baselineB=projectRow(baseline,projectB);
    Check(baselineA.current&&baselineA.totalBytes==MiB&&baselineA.sharedBytes==0,"historical baseline is owned by current A");
    Check(!baselineB.current&&baselineB.totalBytes==0&&baselineB.sharedBytes==0,"empty B row is persisted before the fault");
    cache::ResetScene();
    // P is published without a current scene, so it deliberately has no owner.
    auto token=std::make_unique<cache::WriteReservation>(path,20*MiB);Check(bool(*token),"reserve failed-publication candidate");
    WriteFile(path,20*MiB,*token);
    const auto orphanIdentity=identity(path);Check(fs::file_size(path)==20*MiB,"published fixture keeps declared size");
    blockedIndex=(root/L"Management"/L"index.bin").wstring();failIndex=true;
    Check(!token->Commit(),"index publication failure stays observable");
    Check(!cache::WriteReservation(Entry(2),MiB),"failed publication blocks reservations while responsibility pending");
    Check(fs::exists(path)&&fs::file_size(path)==20*MiB&&sameFile(orphanIdentity,identity(path)),"failed publication leaves the original physical payload");
    // Once the publication is orphaned while the injected index fault is active,
    // Guard admission is false and lifecycle calls would no-op. So ResetScene and
    // BeginProject run while the failed Commit still owns its live token; Reset
    // clears that attempt's seen set and lease and Begin leaves B pending. No
    // ProjectOpened is attempted under the failed orphan Save; it runs only as the
    // successful retry after the fault clears.
    cache::ResetScene();cache::BeginProject(projectB.c_str());
    // Public-API prerequisite: the transition must be real, not a silent no-op,
    // before the orphan retry is attempted.
    {
        const auto pending=cache::Inspect();
        const auto pendingA=projectRow(pending,projectA);const auto pendingB=projectRow(pending,projectB);
        Check(pendingB.current&&pendingB.totalBytes==0&&pendingB.sharedBytes==0,"B is current with zero ownership before the orphan retry");
        Check(pendingA.totalBytes==MiB&&pendingA.sharedBytes==0,"A owns only the historical baseline before the orphan retry");
    }
    DWORD probeError=0;Check(canDelete(path,probeError),"failed Commit lease is cleared before the orphan retry");
    token.reset();
    Check(!cache::WriteReservation(Entry(2),MiB),"orphaned publication keeps capacity denied while B is pending");
    Check(fs::exists(path)&&fs::file_size(path)==20*MiB&&sameFile(orphanIdentity,identity(path)),"token destruction keeps the orphan physical payload");
    // The failed pending retry may pollute B's seen/lease set but cannot finish
    // the index Save. ProjectOpened below is the successful retry: its guard sync
    // settles P before the body merges pending seen/lease into owners.
    failIndex=false;
    cache::ProjectOpened(projectB.c_str());
    const auto retried=cache::Inspect();
    Check(retried.error.empty(),"ProjectOpened guard settled the orphan retry and inspection is healthy");
    const auto retriedB=projectRow(retried,projectB);const auto retriedA=projectRow(retried,projectA);
    Check(retriedB.current&&retriedB.totalBytes==0&&retriedB.sharedBytes==0,"orphan retry is not current-scene use");
    Check(retriedA.totalBytes==MiB&&retriedA.sharedBytes==0,"orphan retry leaves the historical owner untouched");
    Check(retried.textureBytes==21*MiB&&retried.totalBytes==21*MiB+IndexCharge(),"retry counts both physical payloads exactly once");
    Check(fs::file_size(path)==20*MiB&&sameFile(orphanIdentity,identity(path)),"orphan retry keeps the orphan payload");
    Check(fs::file_size(history)==MiB&&sameFile(historyIdentity,identity(history)),"orphan retry keeps the historical payload");
    probeError=0;Check(canDelete(path,probeError),"orphan retry opens no read lease on the orphan payload");
    probeError=0;Check(canDelete(history,probeError),"orphan retry opens no read lease on the historical payload");
    {
        cache::WriteReservation temporary(Entry(2),42*MiB);Check(bool(temporary),"reconciled capacity admits a 42 MiB in-scope reservation");
        Check(!cache::WriteReservation(Entry(3),MiB),"temporary reservation counts against the quota");
        Check(cache::Inspect().totalBytes==21*MiB+IndexCharge(),"temporary reservation leaves the physical total unchanged");
    }
    {cache::WriteReservation released(Entry(2),42*MiB);Check(bool(released),"destroyed reservation returns capacity");}
    cache::Touch(path);
    const auto touched=cache::Inspect();
    const auto touchedA=projectRow(touched,projectA);const auto touchedB=projectRow(touched,projectB);
    Check(touchedB.current&&touchedB.totalBytes==20*MiB&&touchedB.sharedBytes==0,"current touch owns only the touched payload");
    Check(touchedA.totalBytes==MiB&&touchedA.sharedBytes==0,"current touch does not claim the historical baseline");
    probeError=0;Check(!canDelete(path,probeError)&&probeError==ERROR_SHARING_VIOLATION,"current read lease forbids deletion");
    Store(2,MiB);
    const auto stored=cache::Inspect();
    const auto storedA=projectRow(stored,projectA);const auto storedB=projectRow(stored,projectB);
    Check(storedA.totalBytes==MiB&&storedA.sharedBytes==0&&storedB.totalBytes==21*MiB&&storedB.sharedBytes==0,
        "current scene accounts only its own publications");
    Check(stored.textureBytes==22*MiB&&stored.totalBytes==22*MiB+IndexCharge(),"global accounting includes every leased payload");
    probeError=0;Check(!canDelete(Entry(2),probeError)&&probeError==ERROR_SHARING_VIOLATION,"current entry remains leased");
    cache::ResetScene();
    probeError=0;Check(canDelete(path,probeError),"scene reset closes the orphan consumer");
    probeError=0;Check(canDelete(Entry(2),probeError),"scene reset closes the current consumer");
    const auto preserved=cache::Inspect();
    Check(projectRow(preserved,projectA).totalBytes==MiB&&projectRow(preserved,projectA).sharedBytes==0,
        "scene reset preserves the historical owner row");
    Check(projectRow(preserved,projectB).totalBytes==21*MiB&&projectRow(preserved,projectB).sharedBytes==0,
        "scene reset preserves the current owner row");
    Check(fs::file_size(history)==MiB&&sameFile(historyIdentity,identity(history)),"scene reset keeps the historical payload identity");
}

void PolicyFailures(const fs::path& output){
    const auto root=Case(output,L"policy");const auto settings=root.parent_path()/L"settings.json";
    {std::ofstream file(settings,std::ios::binary|std::ios::trunc);file<<"{broken";}
    Check(!cache::Current().enabled&&!cache::Enabled()&&!cache::WriteReservation(Entry(1),MiB),"invalid settings cannot enable default quota");
    const auto invalidView=cache::Inspect();
    Check(!invalidView.settings.enabled&&invalidView.error==L"settings-unreadable","inspection reports unreadable policy instead of stale enabled state");
    {std::ofstream file(settings,std::ios::binary|std::ios::trunc);file<<"{\"schemaVersion\":1,\"sections\":{\"cacheManager\":{\"enabled\":true,\"automaticCleanup\":false,\"limitMiB\":64,\"recentProjects\":1}}}";}
    Check(cache::Current().enabled,"restored valid settings are readable");
    {
        Handle busy(CreateFileW((settings.wstring()+L".lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        Check(busy.value!=INVALID_HANDLE_VALUE,"hold own settings lock file exclusively");
        Check(!cache::Current().enabled&&!cache::WriteReservation(Entry(1),MiB),"Busy settings deny new writes");
    }
    {
        Handle unreadable(CreateFileW(settings.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        Check(unreadable.value!=INVALID_HANDLE_VALUE,"deny own settings file read without changing permissions");
        Check(!cache::Current().enabled&&!cache::WriteReservation(Entry(1),MiB),"IO settings failure cannot widen quota");
    }
    Check(bool(cache::WriteReservation(Entry(1),MiB)),"writes resume only after valid policy read");
    SetRoot(output/L"missing-policy"/L"Cache");Check(cache::Current().enabled&&cache::Current().limitBytes==4096*MiB,"truly missing settings retain initial defaults");
}
void UnderReservedPublication(const fs::path& output){
    Case(output,L"under-reserved");Store(1,40*MiB);
    auto token=std::make_unique<cache::WriteReservation>(Entry(2),MiB);Check(bool(*token),"approve only one MiB of new payload");
    WriteFile(Entry(2),30*MiB,*token);
    Check(!token->Commit(),"actual payload cannot exceed approved reservation");
    Check(!cache::WriteReservation(Entry(3),MiB),"under-reserved publication retains unresolved responsibility");
    token.reset();
    const auto view=cache::Inspect();
    Check(view.totalBytes==70*MiB+IndexCharge()&&!cache::WriteReservation(Entry(3),1),"reconciliation accounts violation without enlarging configured quota");
}
void UnconfirmedDeletion(const fs::path& output){
    Case(output,L"delete-pending");Store(1,24*MiB);cache::ResetScene();
    uncertainDeletePath=Entry(1).wstring();deleteObserved=false;
    {
        Handle reader(CreateFileW(Entry(1).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        Check(reader.value!=INVALID_HANDLE_VALUE,"hold real share-delete reader over fixture");
        uncertainDelete=true;
        const auto clean=cache::Clean(cache::Cleanup::All);
        Check(deleteObserved&&clean.files==0&&clean.bytes==0&&clean.skipped==1,"successful disposition with uncertain observation is not claimed as reclaimed");
        const auto pendingView=cache::Inspect();
        const auto expectedPending=24*MiB+IndexCharge();
        const bool pendingAdmission=bool(cache::WriteReservation(Entry(2),MiB));
        std::fprintf(stderr,"pending namespace bytes=%llu expected=%llu error=%ls admitted=%u\n",
            pendingView.totalBytes,expectedPending,pendingView.error.c_str(),unsigned(pendingAdmission));
        Check(pendingView.totalBytes==expectedPending&&!pendingView.error.empty()&&!pendingAdmission,
            "pending namespace retains last observed charge, reports failed observation and denies new writes");
    }
    uncertainDelete=false;
    const auto releasedView=cache::Inspect();
    const auto releasedIndex=IndexCharge();
    std::fprintf(stderr,"confirmed missing bytes=%llu index=%llu error=%ls\n",
        releasedView.totalBytes,releasedIndex,releasedView.error.c_str());
    Check(releasedView.totalBytes==releasedIndex&&releasedView.error.empty(),
        "confirmed missing namespace after reader close can release charge");
}
std::wstring MutexName(const fs::path& root){
    auto path=fs::absolute(root).lexically_normal().wstring();std::wstring lower(path.size(),L'\0');
    Check(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,path.data(),int(path.size()),lower.data(),int(lower.size()),nullptr,nullptr,0)>0,"canonicalize own mutex identity");
    std::uint64_t hash=1469598103934665603ull;for(auto c:lower){hash^=c;hash*=1099511628211ull;}
    return L"Local\\MMS-cache-"+std::to_wstring(hash);
}
void ChangedCleanupCandidate(const fs::path& output) {
    const auto root=Case(output,L"changed-cleanup-candidate");
    Store(1,MiB); cache::ResetScene();
    const auto destination=Entry(1);
    const auto displaced=root.parent_path()/L"displaced-owned.mtc";
    const auto protectedFile=root.parent_path()/L"protected-replacement.bin";
    swapPath=destination.wstring();
    displacedPath=displaced.wstring();
    swappedAtDeleteOpen=false;
    swapOnDeleteOpen=true;
    const auto clean=cache::Clean(cache::Cleanup::All);
    swapOnDeleteOpen=false;
    Check(swappedAtDeleteOpen,"replace the file only after production cleanup inventories it");
    {
        std::ifstream input(destination,std::ios::binary);
        char contents[sizeof replacementBytes]{};
        Check(clean.files==0 && clean.bytes==0 && clean.skipped==1 &&
              input.read(contents,sizeof contents) && std::memcmp(contents,replacementBytes,sizeof contents)==0 &&
              fs::file_size(destination)==MiB,
              "cleanup must preserve a different file at the inventoried name");
    }
    Check(MoveFileW(destination.c_str(),protectedFile.c_str()) &&
          MoveFileW(displaced.c_str(),destination.c_str()),"restore only the owned cache object for retry");
    const auto retry=cache::Clean(cache::Cleanup::All);
    Check(retry.files==1 && !fs::exists(destination) && fs::file_size(protectedFile)==MiB,
          "retry removes the restored cache while preserving the unrelated file");

    Store(2,MiB); cache::ResetScene();
    const auto payload=Entry(2);
    DWORD flags=0;
    {
        Handle file(CreateFileW(payload.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,
                                OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        Check(file.value!=INVALID_HANDLE_VALUE &&
              GetVolumeInformationByHandleW(file.value,nullptr,0,nullptr,nullptr,&flags,nullptr,0),
              "identify whether the fixture volume supports named streams");
    }
    if (flags & FILE_NAMED_STREAMS) {
        const auto stream=payload.wstring()+L":user-note";
        {
            std::ofstream note(fs::path(stream),std::ios::binary);
            note << "keep this stream";
            Check(note.good(),"add a real named stream to the indexed payload");
        }
        const auto refused=cache::Clean(cache::Cleanup::All);
        {
            std::ifstream note(fs::path(stream),std::ios::binary);
            const std::string contents{std::istreambuf_iterator<char>(note),std::istreambuf_iterator<char>()};
            Check(refused.files==0 && refused.skipped==1 && contents=="keep this stream" && fs::exists(payload),
                  "a known extra stream preserves both the payload and named stream");
        }
        Check(DeleteFileW(stream.c_str()) && cache::Clean(cache::Cleanup::All).files==1 && !fs::exists(payload),
              "after removing the test stream, retry cleans the unchanged owned payload");
    } else {
        std::puts("SKIP observed stream cleanup: fixture volume does not support named streams");
    }
}
void ConstructorFailureUnlocks(const fs::path& output){
    const auto root=Case(output,L"mutex-failure");const auto name=MutexName(root);
    Handle observer(CreateMutexW(nullptr,FALSE,name.c_str()));Check(observer.value!=nullptr,"keep real named mutex alive for external observer");
    const auto path=Entry(1);allocationFailed=false;injectAfterMutex=true;
    {cache::WriteReservation failed(path,MiB);Check(!failed,"allocation after acquired mutex rejects write");}
    injectAfterMutex=false;failNextAllocation=false;
    Check(allocationFailed,"fault occurred inside acquired guard construction");
    wchar_t exe[32768]{};const auto n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"resolve own child executable");
    auto command=L"\""+std::wstring(exe)+L"\" --mutex-check \""+name+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"launch only own mutex observer child");
    Handle child(process.hProcess),thread(process.hThread);
    const auto wait=WaitForSingleObject(child.value,10000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(child.value,99);WaitForSingleObject(child.value,1000);}
    DWORD code=99;GetExitCodeProcess(child.value,&code);
    Check(wait==WAIT_OBJECT_0&&code==0,"constructor exception releases real mutex to another process");
    Check(bool(cache::WriteReservation(path,MiB)),"normal reservation recovers after constructor failure");
}
struct LeaseFileIdentity{DWORD volume=0,high=0,low=0;};
LeaseFileIdentity IdentifyPayload(const fs::path& path){
    Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(file.value!=INVALID_HANDLE_VALUE,"open fixture attributes without taking a cache lease");
    BY_HANDLE_FILE_INFORMATION info{};Check(GetFileInformationByHandle(file.value,&info)!=FALSE,"read fixture volume and file index");
    return LeaseFileIdentity{info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};
}
bool SameLeaseIdentity(const LeaseFileIdentity& a,const LeaseFileIdentity& b){
    return a.volume==b.volume&&a.high==b.high&&a.low==b.low;
}
// Returns ERROR_SUCCESS when a DELETE-open would be admitted, otherwise the
// exact CreateFileW error. The handle is closed by RAII without deleting.
DWORD ProbeDeletion(const fs::path& path){
    Handle probe(CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    return probe.value==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
}
cache::Project FindProjectRow(const cache::Snapshot& snapshot,const fs::path& project){
    const auto expected=fs::absolute(project).lexically_normal().wstring();
    for(const auto& row:snapshot.projects)if(_wcsicmp(fs::path(row.path).lexically_normal().c_str(),expected.c_str())==0)return row;
    Check(false,"inspect exposes the expected project identity row");return cache::Project{};
}
bool MemoryOnlyEnabled(){
    wchar_t value[2]{};return GetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",value,2)==1&&value[0]==L'1';
}
// Restores the previous existence/value of MMS_CACHE_MEMORY_ONLY even when a
// later Check throws, so the process never carries the mode into other groups.
struct ScopedMemoryOnly {
    std::wstring previous;bool had=false;
    ScopedMemoryOnly(){
        // Capture the original state before any throwing allocation can leave
        // the mode enabled without a constructed guard. A zero return is only
        // absence when GetLastError says ERROR_ENVVAR_NOT_FOUND; ERROR_SUCCESS
        // is an existing empty value and must round-trip as such.
        SetLastError(ERROR_SUCCESS);
        const DWORD capacity=GetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",nullptr,0);
        const DWORD capacityError=GetLastError();
        if(capacity){
            std::wstring value(capacity,L'\0');
            SetLastError(ERROR_SUCCESS);
            const DWORD length=GetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",value.data(),capacity);
            const DWORD lengthError=GetLastError();
            Check(length+1==capacity&&(length||lengthError==ERROR_SUCCESS),
                "preserve the existing memory-only environment setting without losing its original length");
            value.resize(length);previous=std::move(value);had=true;
        }else if(capacityError==ERROR_SUCCESS){
            had=true;previous.clear();
        }else{
            Check(capacityError==ERROR_ENVVAR_NOT_FOUND,"read the original memory-only environment state");
        }
        Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1")!=FALSE,"enable memory-only cache mode");
    }
    ~ScopedMemoryOnly(){SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",had?previous.c_str():nullptr);}
};
// Restores the new exact-path unreadable mode even when a later Check throws.
struct ScopedAttributesUnreadable {
    explicit ScopedAttributesUnreadable(const fs::path& path){
        unreadableAttributesPath=path.wstring();
        unreadableAttributesHits=0;
        unreadableAttributes=true;
    }
    ~ScopedAttributesUnreadable(){unreadableAttributes=false;unreadableAttributesPath.clear();}
};
struct ShutdownLeaseSetup {fs::path project,path;LeaseFileIdentity identity;cache::Snapshot baseline;};
ShutdownLeaseSetup PrepareShutdownLeaseScenario(const fs::path& output,const wchar_t* name){
    Case(output,name);
    ShutdownLeaseSetup setup;setup.project=output/name/L"ProjectA";setup.path=Entry(1);
    // Persist a committed 20 MiB payload and its owner while both faults are off.
    cache::BeginProject(setup.project.c_str());cache::ProjectOpened(setup.project.c_str());Store(1,20*MiB);
    setup.baseline=cache::Inspect();
    Check(setup.baseline.error.empty(),"baseline inspection is healthy before shutdown");
    const auto owner=FindProjectRow(setup.baseline,setup.project);
    Check(owner.current&&owner.totalBytes==20*MiB&&owner.sharedBytes==0,"baseline store persists a current 20 MiB owner");
    Check(setup.baseline.textureBytes==20*MiB&&setup.baseline.totalBytes==20*MiB+IndexCharge(),
        "baseline physical accounting includes payload and index");
    setup.identity=IdentifyPayload(setup.path);
    Check(fs::file_size(setup.path)==20*MiB,"baseline payload keeps its declared size");
    Check(ProbeDeletion(setup.path)==ERROR_SHARING_VIOLATION,"baseline cache lease blocks deletion before shutdown");
    return setup;
}
struct ShutdownLeaseResult {DWORD firstProbeError=ERROR_SUCCESS,repeatProbeError=ERROR_SUCCESS;bool current=false;};
ShutdownLeaseResult VerifyShutdownLeaseAftermath(const ShutdownLeaseSetup& setup){
    ShutdownLeaseResult result;
    const auto view=cache::Inspect();
    Check(view.error.empty(),"inspection recovers after the shutdown fault is released");
    const auto owner=FindProjectRow(view,setup.project);
    Check(owner.totalBytes==20*MiB&&owner.sharedBytes==0,"the persisted 20 MiB owner survives shutdown");
    Check(view.textureBytes==20*MiB&&view.totalBytes==20*MiB+IndexCharge(),
        "shutdown keeps payload and metadata physical accounting");
    Check(fs::file_size(setup.path)==20*MiB&&SameLeaseIdentity(setup.identity,IdentifyPayload(setup.path)),
        "shutdown keeps the payload physical identity and size");
    result.current=owner.current;
    const auto index=IndexCharge();
    {
        cache::WriteReservation temporary(Entry(2),43*MiB);Check(bool(temporary),"shutdown leaves 43 MiB of in-scope capacity");
        Check(!cache::WriteReservation(Entry(3),MiB),"shutdown accounting rejects one more MiB");
        Check(cache::Inspect().totalBytes==20*MiB+index,"temporary reservation leaves post-shutdown physical accounting unchanged");
    }
    {cache::WriteReservation released(Entry(2),43*MiB);Check(bool(released),"released post-shutdown reservation returns capacity");}
    return result;
}
ShutdownLeaseResult RunShutdownLeaseMemoryOnly(const fs::path& output){
    const auto setup=PrepareShutdownLeaseScenario(output,L"shutdown-memory-only");
    ShutdownLeaseResult result;
    {
        ScopedMemoryOnly scoped;
        Check(MemoryOnlyEnabled(),"memory-only mode is observable before shutdown");
        cache::Shutdown();
        result.firstProbeError=ProbeDeletion(setup.path);
        cache::Shutdown();
        result.repeatProbeError=ProbeDeletion(setup.path);
    }
    std::fprintf(stderr,"shutdown-memory-only first=%s error=%lu repeat=%s error=%lu\n",
        result.firstProbeError==ERROR_SUCCESS?"released":"blocked",result.firstProbeError,
        result.repeatProbeError==ERROR_SUCCESS?"released":"blocked",result.repeatProbeError);
    const auto aftermath=VerifyShutdownLeaseAftermath(setup);result.current=aftermath.current;
    std::fprintf(stderr,"shutdown-memory-only current after shutdown=%u\n",unsigned(result.current));
    return result;
}
ShutdownLeaseResult RunShutdownLeaseInventoryRejected(const fs::path& output){
    const auto setup=PrepareShutdownLeaseScenario(output,L"shutdown-inventory");
    ShutdownLeaseResult result;unsigned hits=0;
    {
        ScopedAttributesUnreadable fault(setup.path);
        const auto blocked=cache::Inspect();
        hits=unreadableAttributesHits.load();
        Check(blocked.error==L"inventory-unreadable","inventory failure surfaces as inventory-unreadable");
        Check(blocked.totalBytes==setup.baseline.totalBytes,"inventory failure preserves the last physical total");
        Check(hits>0,"inventory fault hook observes the payload path");
        Check(!cache::WriteReservation(Entry(2),MiB),"inventory guard rejection denies a new reservation");
        cache::Shutdown();
        result.firstProbeError=ProbeDeletion(setup.path);
        cache::Shutdown();
        result.repeatProbeError=ProbeDeletion(setup.path);
    }
    std::fprintf(stderr,"shutdown-inventory hits=%u first=%s error=%lu repeat=%s error=%lu\n",
        hits,result.firstProbeError==ERROR_SUCCESS?"released":"blocked",result.firstProbeError,
        result.repeatProbeError==ERROR_SUCCESS?"released":"blocked",result.repeatProbeError);
    const auto aftermath=VerifyShutdownLeaseAftermath(setup);result.current=aftermath.current;
    std::fprintf(stderr,"shutdown-inventory current after shutdown=%u\n",unsigned(result.current));
    return result;
}
// Regression E-test: a real Shutdown must release this process's existing
// cache lease even when MemoryOnly returns early or an inventory failure makes
// Guard admission false. Both scenarios run before the release/current
// expectations are checked, so the first RED reports both diagnostics instead
// of hiding the second scenario behind an exception.
void ShutdownReleasesExistingLease(const fs::path& output){
    const auto memoryOnly=RunShutdownLeaseMemoryOnly(output);
    const auto inventory=RunShutdownLeaseInventoryRejected(output);
    Check(memoryOnly.firstProbeError==ERROR_SUCCESS,"shutdown releases an existing cache lease in memory-only mode");
    Check(memoryOnly.repeatProbeError==ERROR_SUCCESS,"repeated memory-only shutdown keeps the lease released");
    Check(!memoryOnly.current,"memory-only shutdown leaves no current project");
    Check(inventory.firstProbeError==ERROR_SUCCESS,"shutdown releases an existing cache lease after inventory guard rejection");
    Check(inventory.repeatProbeError==ERROR_SUCCESS,"repeated inventory-rejected shutdown keeps the lease released");
    Check(!inventory.current,"inventory guard rejection leaves no current project after shutdown");
}
// A root whose only unresolved responsibility is an unregistered publication.
// Commit deliberately publishes 20 MiB through a 10 MiB reservation, so it
// refuses the file before Use creates an entry owner or a read lease. Destroying
// the token leaves the reservation orphaned, and the self-owned persistence
// responsibility keeps this root's state (including the baseline lease) alive
// across later root switches. The returned fixture owns no handle or token.
struct LiveOrphanRoot {
    fs::path root,project,baseline,orphan;
    LeaseFileIdentity baselineIdentity,orphanIdentity;
};
LiveOrphanRoot PrepareLiveOrphanRoot(const fs::path& output,const wchar_t* name){
    const auto root=Case(output,name);
    LiveOrphanRoot fixture;
    fixture.root=fs::absolute(root);
    fixture.project=output/name/L"ProjectH";
    fixture.baseline=fs::absolute(Entry(10));
    fixture.orphan=fs::absolute(Entry(1));
    // Persist baseline H and its owner while the cache is healthy.
    cache::BeginProject(fixture.project.c_str());cache::ProjectOpened(fixture.project.c_str());
    Store(10,MiB);
    const auto baseline=cache::Inspect();
    const auto baselineOwner=FindProjectRow(baseline,fixture.project);
    const auto baselineIndex=IndexCharge();
    fixture.baselineIdentity=IdentifyPayload(fixture.baseline);
    std::fprintf(stderr,"live-orphan %ls baseline error=%ls total=%llu expected=%llu limit=%llu texture=%llu owner=%llu shared=%llu current=%u size=%llu\n",
        name,baseline.error.c_str(),static_cast<unsigned long long>(baseline.totalBytes),
        static_cast<unsigned long long>(MiB+baselineIndex),static_cast<unsigned long long>(baseline.settings.limitBytes),
        static_cast<unsigned long long>(baseline.textureBytes),static_cast<unsigned long long>(baselineOwner.totalBytes),
        static_cast<unsigned long long>(baselineOwner.sharedBytes),unsigned(baselineOwner.current),
        static_cast<unsigned long long>(fs::file_size(fixture.baseline)));
    Check(baseline.error.empty(),"live orphan root baseline inspection is healthy");
    Check(baseline.settings.enabled&&baseline.settings.limitBytes==64*MiB&&!baseline.settings.automaticCleanup,
        "live orphan root keeps the approved 64 MiB manual-cleanup policy");
    Check(baseline.textureBytes==MiB&&baseline.totalBytes==MiB+baselineIndex,
        "live orphan root baseline charges one payload and its index once");
    Check(baselineOwner.current&&baselineOwner.totalBytes==MiB&&baselineOwner.sharedBytes==0,
        "live orphan root baseline persists one current 1 MiB owner");
    Check(fs::file_size(fixture.baseline)==MiB,"live orphan root baseline keeps its declared size");
    // Positive admission control: real capacity exists before the orphan debt.
    {cache::WriteReservation control(Entry(2),MiB);Check(bool(control),"live orphan root admits a normal 1 MiB reservation before the orphan");}
    {
        cache::WriteReservation token(Entry(1),10*MiB);
        Check(bool(token),"live orphan root reserves a deliberately under-sized 10 MiB publication");
        WriteFile(fixture.orphan,20*MiB,token);
        Check(!token.Commit(),"under-reserved publication is refused before registration");
    }
    fixture.orphanIdentity=IdentifyPayload(fixture.orphan);
    const auto view=cache::Inspect();
    const auto viewIndex=IndexCharge();
    const auto viewOwner=FindProjectRow(view,fixture.project);
    const bool denied=!cache::WriteReservation(Entry(12),MiB);
    std::fprintf(stderr,"live-orphan %ls orphan error=%ls total=%llu expected=%llu limit=%llu texture=%llu index=%llu denied=%u baselineSize=%llu orphanSize=%llu\n",
        name,view.error.c_str(),static_cast<unsigned long long>(view.totalBytes),
        static_cast<unsigned long long>(21*MiB+viewIndex),static_cast<unsigned long long>(view.settings.limitBytes),
        static_cast<unsigned long long>(view.textureBytes),static_cast<unsigned long long>(viewIndex),unsigned(denied),
        static_cast<unsigned long long>(fs::file_size(fixture.baseline)),
        static_cast<unsigned long long>(fs::file_size(fixture.orphan)));
    Check(view.error.empty(),"orphaned responsibility keeps inspection healthy");
    Check(view.settings.enabled&&view.settings.limitBytes==64*MiB&&!view.settings.automaticCleanup,
        "orphaned responsibility keeps the approved 64 MiB manual-cleanup policy");
    Check(view.textureBytes==MiB,"unregistered orphan publication is not registered as a texture entry");
    Check(view.totalBytes==21*MiB+viewIndex,"orphan namespace charges both physical payloads and metadata exactly once");
    Check(view.totalBytes<32*MiB,"orphan namespace keeps more than half the 64 MiB budget free");
    Check(viewOwner.totalBytes==MiB&&viewOwner.sharedBytes==0,"unregistered orphan publication claims no project owner");
    Check(SameLeaseIdentity(fixture.baselineIdentity,IdentifyPayload(fixture.baseline))&&fs::file_size(fixture.baseline)==MiB,
        "orphan preparation keeps the baseline payload identity and size");
    Check(SameLeaseIdentity(fixture.orphanIdentity,IdentifyPayload(fixture.orphan))&&fs::file_size(fixture.orphan)==20*MiB,
        "orphan preparation keeps the unregistered payload identity and size");
    Check(denied,"unregistered orphan responsibility denies a new 1 MiB reservation with ample budget");
    return fixture;
}
// Public-API aftermath for a live-orphan root after Shutdown. The caller holds
// no token or handle for this fixture; SetRoot alone selects the root again.
void VerifyLiveOrphanAfterShutdown(const LiveOrphanRoot& fixture,const char* label){
    SetRoot(fixture.root);
    const auto index=IndexCharge();
    const auto view=cache::Inspect();
    const auto owner=FindProjectRow(view,fixture.project);
    const bool denied=!cache::WriteReservation(Entry(12),MiB);
    std::fprintf(stderr,"%s error=%ls total=%llu expected=%llu limit=%llu texture=%llu owner=%llu shared=%llu current=%u denied=%u baselineSize=%llu orphanSize=%llu\n",
        label,view.error.c_str(),static_cast<unsigned long long>(view.totalBytes),
        static_cast<unsigned long long>(21*MiB+index),static_cast<unsigned long long>(view.settings.limitBytes),
        static_cast<unsigned long long>(view.textureBytes),static_cast<unsigned long long>(owner.totalBytes),
        static_cast<unsigned long long>(owner.sharedBytes),unsigned(owner.current),unsigned(denied),
        static_cast<unsigned long long>(fs::file_size(fixture.baseline)),
        static_cast<unsigned long long>(fs::file_size(fixture.orphan)));
    Check(view.error.empty(),"shutdown leaves the live orphan root inspectable");
    Check(view.settings.enabled&&view.settings.limitBytes==64*MiB&&!view.settings.automaticCleanup,
        "shutdown keeps the live orphan root policy");
    Check(!owner.current,"shutdown clears the live orphan root current project");
    Check(owner.totalBytes==MiB&&owner.sharedBytes==0,"shutdown keeps the baseline owner and invents no orphan owner");
    Check(view.textureBytes==MiB,"shutdown keeps the unregistered orphan out of texture registration");
    Check(view.totalBytes==21*MiB+index,"shutdown keeps both physical payloads and metadata accounting");
    Check(view.totalBytes<32*MiB,"post-shutdown orphan root keeps ample budget headroom");
    Check(SameLeaseIdentity(fixture.baselineIdentity,IdentifyPayload(fixture.baseline))&&fs::file_size(fixture.baseline)==MiB,
        "shutdown keeps the baseline payload identity and size");
    Check(SameLeaseIdentity(fixture.orphanIdentity,IdentifyPayload(fixture.orphan))&&fs::file_size(fixture.orphan)==20*MiB,
        "shutdown keeps the unregistered orphan payload identity and size");
    Check(denied,"shutdown retains the unresolved orphan responsibility");
}
// Shutdown contract 1: independent live roots keep their own baseline leases,
// and one Shutdown releases every root's local leases, including a root that is
// no longer the configured one, without touching physical accounting.
void ShutdownMultipleLiveRoots(const fs::path& output){
    const auto first=PrepareLiveOrphanRoot(output,L"shutdown-live-root-a");
    const auto second=PrepareLiveOrphanRoot(output,L"shutdown-live-root-b");
    const auto firstBaseline=ProbeDeletion(first.baseline);
    const auto secondBaseline=ProbeDeletion(second.baseline);
    const auto firstOrphan=ProbeDeletion(first.orphan);
    const auto secondOrphan=ProbeDeletion(second.orphan);
    std::fprintf(stderr,"shutdown-live-roots before firstBaseline=%lu secondBaseline=%lu firstOrphan=%lu secondOrphan=%lu\n",
        firstBaseline,secondBaseline,firstOrphan,secondOrphan);
    Check(firstBaseline==ERROR_SHARING_VIOLATION,"earlier live root keeps its baseline lease after root switching");
    Check(secondBaseline==ERROR_SHARING_VIOLATION,"configured live root keeps its baseline lease before shutdown");
    Check(firstOrphan==ERROR_SUCCESS,"earlier unregistered orphan publication holds no lease");
    Check(secondOrphan==ERROR_SUCCESS,"configured unregistered orphan publication holds no lease");
    DWORD firstAfter=ERROR_SUCCESS,secondAfter=ERROR_SUCCESS;
    cache::Shutdown();
    firstAfter=ProbeDeletion(first.baseline);
    secondAfter=ProbeDeletion(second.baseline);
    std::fprintf(stderr,"shutdown-live-roots first release firstBaseline=%lu secondBaseline=%lu\n",firstAfter,secondAfter);
    Check(firstAfter==ERROR_SUCCESS,"shutdown releases the earlier live root baseline lease");
    Check(secondAfter==ERROR_SUCCESS,"shutdown releases the configured live root baseline lease");
    cache::Shutdown();
    firstAfter=ProbeDeletion(first.baseline);
    secondAfter=ProbeDeletion(second.baseline);
    std::fprintf(stderr,"shutdown-live-roots repeat release firstBaseline=%lu secondBaseline=%lu\n",firstAfter,secondAfter);
    Check(firstAfter==ERROR_SUCCESS,"repeated shutdown keeps the earlier root lease released");
    Check(secondAfter==ERROR_SUCCESS,"repeated shutdown keeps the configured root lease released");
    VerifyLiveOrphanAfterShutdown(first,"shutdown-live-roots earlier");
    VerifyLiveOrphanAfterShutdown(second,"shutdown-live-roots configured");
}
// Shutdown contract 2: an unknown configured root must not materialize a cache
// directory or settings file, and must still release the known root's leases.
void ShutdownUnknownConfiguredRoot(const fs::path& output){
    const auto setup=PrepareShutdownLeaseScenario(output,L"shutdown-unknown-root");
    const auto original=cache::Root();
    const auto unknownParent=output/L"shutdown-unknown-root"/L"absent-parent";
    const auto unknownRoot=unknownParent/L"Cache";
    const bool parentBefore=fs::exists(unknownParent),rootBefore=fs::exists(unknownRoot);
    std::fprintf(stderr,"shutdown-unknown-root configured=%ls parentBefore=%u rootBefore=%u\n",
        unknownRoot.c_str(),unsigned(parentBefore),unsigned(rootBefore));
    Check(!parentBefore&&!rootBefore,"unknown configured root is absent before shutdown");
    SetRoot(unknownRoot);
    DWORD firstProbe=ERROR_SUCCESS,repeatProbe=ERROR_SUCCESS;
    cache::Shutdown();
    firstProbe=ProbeDeletion(setup.path);
    const bool parentAfterFirst=fs::exists(unknownParent),rootAfterFirst=fs::exists(unknownRoot);
    cache::Shutdown();
    repeatProbe=ProbeDeletion(setup.path);
    const bool parentAfterRepeat=fs::exists(unknownParent),rootAfterRepeat=fs::exists(unknownRoot);
    std::fprintf(stderr,"shutdown-unknown-root first probe=%lu parentAfter=%u rootAfter=%u repeat probe=%lu parentAfter=%u rootAfter=%u\n",
        firstProbe,unsigned(parentAfterFirst),unsigned(rootAfterFirst),repeatProbe,
        unsigned(parentAfterRepeat),unsigned(rootAfterRepeat));
    Check(firstProbe==ERROR_SUCCESS,"shutdown releases the known root lease through an unknown configuration");
    Check(!parentAfterFirst&&!rootAfterFirst,"shutdown creates no unknown root or settings parent");
    Check(repeatProbe==ERROR_SUCCESS,"repeated shutdown still releases the known root lease");
    Check(!parentAfterRepeat&&!rootAfterRepeat,"repeated shutdown still creates no unknown root or settings parent");
    SetRoot(original);
    const auto aftermath=VerifyShutdownLeaseAftermath(setup);
    std::fprintf(stderr,"shutdown-unknown-root aftermath current=%u\n",unsigned(aftermath.current));
    Check(!aftermath.current,"unknown-root shutdown leaves the known root with no current project");
}
// Shutdown contract 3: unresolved orphan responsibility survives the root being
// dropped from the global state, not merely because that root is still current.
void ShutdownOrphanResponsibility(const fs::path& output){
    const auto fixture=PrepareLiveOrphanRoot(output,L"shutdown-orphan-responsibility");
    const auto baselineBefore=ProbeDeletion(fixture.baseline);
    const auto orphanBefore=ProbeDeletion(fixture.orphan);
    std::fprintf(stderr,"shutdown-orphan-responsibility before baseline=%lu orphan=%lu\n",baselineBefore,orphanBefore);
    Check(baselineBefore==ERROR_SHARING_VIOLATION,"orphan-root baseline lease blocks deletion before shutdown");
    Check(orphanBefore==ERROR_SUCCESS,"unregistered orphan publication has no lease before shutdown");
    DWORD firstProbe=ERROR_SUCCESS,repeatProbe=ERROR_SUCCESS,orphanFirst=ERROR_SUCCESS,orphanRepeat=ERROR_SUCCESS;
    cache::Shutdown();
    firstProbe=ProbeDeletion(fixture.baseline);
    orphanFirst=ProbeDeletion(fixture.orphan);
    cache::Shutdown();
    repeatProbe=ProbeDeletion(fixture.baseline);
    orphanRepeat=ProbeDeletion(fixture.orphan);
    std::fprintf(stderr,"shutdown-orphan-responsibility first=%lu orphanFirst=%lu repeat=%lu orphanRepeat=%lu\n",
        firstProbe,orphanFirst,repeatProbe,orphanRepeat);
    Check(firstProbe==ERROR_SUCCESS,"shutdown releases the orphan-root baseline lease");
    Check(orphanFirst==ERROR_SUCCESS,"shutdown keeps the unregistered orphan payload lease-free");
    Check(repeatProbe==ERROR_SUCCESS,"repeated shutdown keeps the orphan-root baseline lease released");
    Check(orphanRepeat==ERROR_SUCCESS,"repeated shutdown keeps the unregistered orphan payload lease-free");
    // Move the global state to an unrelated healthy root and drop it again, so a
    // lost pendingPersistenceOwner would rebuild A without its orphan debt.
    Case(output,L"shutdown-orphan-other");
    const auto other=cache::Inspect();
    std::fprintf(stderr,"shutdown-orphan-responsibility other error=%ls total=%llu projects=%llu\n",
        other.error.c_str(),static_cast<unsigned long long>(other.totalBytes),
        static_cast<unsigned long long>(other.projects.size()));
    Check(other.error.empty()&&other.projects.empty(),"independent root replaces the global state before returning to the orphan root");
    VerifyLiveOrphanAfterShutdown(fixture,"shutdown-orphan-responsibility");
}
void SceneOperationIdentity(const fs::path& output) {
    using cache::SceneOperation;
    using cache::SceneOutcome;
    const auto setup = PrepareShutdownLeaseScenario(output, L"scene-operation");
    const auto projectB = output / L"scene-operation" / L"B.pmm";
    const auto projectC = output / L"scene-operation" / L"C.pmm";
    const auto latePath = Entry(2);
    auto late = std::make_unique<cache::WriteReservation>(latePath, MiB);
    Check(bool(*late), "reserve a real publication in A");
    WriteFile(latePath, MiB, *late);

    // A refused reset has no accepted ticket. The outer load can still finish.
    const auto first = cache::AcceptScene(SceneOperation::Load);
    Check(bool(first), "accept first load");
    Check(!cache::FinishScene({}, SceneOutcome::ResetComplete), "unaccepted reset cannot finish");
    Check(!cache::FinishScene(first, SceneOutcome::ResetComplete), "reset completion cannot consume load ticket");
    cache::Touch(setup.path);
    Check(cache::FinishScene(first, SceneOutcome::Loaded, projectB.c_str()), "load survives reset refusal");
    const auto b = FindProjectRow(cache::Inspect(), projectB);
    Check(b.current && b.sharedBytes == 20 * MiB && b.totalBytes == b.sharedBytes,
        "successful B binds its real pending touch");

    const auto outer = cache::AcceptScene(SceneOperation::Load);
    {
        ScopedMemoryOnly memory;
        const auto reset = cache::AcceptScene(SceneOperation::Reset);
        Check(bool(outer) && bool(reset), "nested reset accepted while memory-only");
        Check(!cache::FinishScene(outer, SceneOutcome::Failed), "outer failure cannot finish pending reset");
        Check(cache::FinishScene(reset, SceneOutcome::Failed), "accepted reset fails in isolation");
        Check(ProbeDeletion(setup.path) == ERROR_SHARING_VIOLATION, "failed reset preserves old protection");
        Check(!cache::FinishScene(reset, SceneOutcome::ResetComplete), "failed reset is terminal");
        const auto recovered = cache::AcceptScene(SceneOperation::Reset);
        Check(cache::FinishScene(recovered, SceneOutcome::ResetComplete), "full reset succeeds while memory-only");
        Check(ProbeDeletion(setup.path) == ERROR_SUCCESS, "completed reset releases protection before mode restore");
    }

    const auto next = cache::AcceptScene(SceneOperation::Load);
    cache::Touch(setup.path);
    {
        ScopedMemoryOnly memory;
        Check(cache::FinishScene(next, SceneOutcome::Loaded, projectC.c_str()), "remember successful load without disk admission");
    }
    Check(!cache::FinishScene(outer, SceneOutcome::Loaded, projectB.c_str()), "stale successful load cannot rebind C");
    Check(!cache::FinishScene(next, SceneOutcome::Failed), "scope destruction after success is inert");
    Check(late->Commit(), "settle old tracked publication after replacement");
    late.reset();
    Check(ProbeDeletion(latePath) == ERROR_SUCCESS, "late publication acquires no scene lease");
    Check(ProbeDeletion(setup.path) == ERROR_SHARING_VIOLATION, "outer cleanup preserves new scene lease");
    const auto snapshot = cache::Inspect();
    const auto c = FindProjectRow(snapshot, projectC);
    Check(snapshot.error.empty() && c.current && c.totalBytes == 20 * MiB && c.sharedBytes == c.totalBytes,
        "only genuine current use belongs to C");
    Check(!FindProjectRow(snapshot, projectB).current && FindProjectRow(snapshot, setup.project).totalBytes == 20 * MiB,
        "replacement preserves the historical owners");
    Check(snapshot.textureBytes == 21 * MiB && snapshot.totalBytes == snapshot.textureBytes + IndexCharge(),
        "late publication still registers and accounts its physical payload");
    cache::Touch(latePath);
    Check(FindProjectRow(cache::Inspect(), projectC).totalBytes == 21 * MiB &&
        ProbeDeletion(latePath) == ERROR_SHARING_VIOLATION, "actual new-scene touch acquires ownership and lease");

    Case(output, L"scene-operation-second-root");
    Store(3, MiB);
    const auto otherPath = Entry(3);
    Check(ProbeDeletion(setup.path) == ERROR_SHARING_VIOLATION &&
        ProbeDeletion(otherPath) == ERROR_SHARING_VIOLATION, "both participating roots retain their leases");
    {
        ScopedAttributesUnreadable fault(otherPath);
        Check(!cache::WriteReservation(Entry(4), MiB) && unreadableAttributesHits > 0,
            "configured root demonstrably rejects inventory before reset");
        const auto reset = cache::AcceptScene(SceneOperation::Reset);
        Check(cache::FinishScene(reset, SceneOutcome::ResetComplete), "local reset completes despite refused inventory");
        Check(ProbeDeletion(setup.path) == ERROR_SUCCESS && ProbeDeletion(otherPath) == ERROR_SUCCESS,
            "reset retires both roots while the fault remains active");
    }
}

// Both files are proven uses of B. Only the first physical file survives until
// the original root can persist B's owners. An empty B row predates the fault.
void DeferredLoadedOwners(const fs::path& output) {
    using cache::SceneOperation;
    using cache::SceneOutcome;
    const auto root = Case(output, L"deferred-loaded-owners");
    const auto projectB = root.parent_path() / L"B.pmm";
    const auto projectC = root.parent_path() / L"C.pmm";
    cache::BeginProject(projectB.c_str());
    cache::ProjectOpened(projectB.c_str());
    cache::ResetScene();
    Store(1, MiB);
    Store(2, 2 * MiB);
    cache::ResetScene();
    const auto keep = Entry(1), replaced = Entry(2);
    const auto keepId = IdentifyPayload(keep), oldId = IdentifyPayload(replaced);
    const auto baseline = cache::Inspect();
    Check(baseline.error.empty() && FindProjectRow(baseline, projectB).totalBytes == 0 &&
        baseline.textureBytes == 3 * MiB, "persist independent empty B row and two unowned payloads");
    Check(ProbeDeletion(keep) == ERROR_SUCCESS && ProbeDeletion(replaced) == ERROR_SUCCESS,
        "both fixtures start without a manager read lease");
    const auto index = root / L"Management" / L"index.bin";
    const auto savedIndex = root.parent_path() / L"pre-load-index.bin";
    Check(fs::copy_file(index, savedIndex), "save the healthy pre-load index outside the cache");

    const auto b = cache::AcceptScene(SceneOperation::Load);
    cache::Touch(keep);
    cache::Touch(replaced);
    Check(ProbeDeletion(keep) == ERROR_SHARING_VIOLATION && ProbeDeletion(replaced) == ERROR_SHARING_VIOLATION,
        "both registered files were actually touched during B");
    {
        ScopedAttributesUnreadable fault(keep);
        Check(cache::FinishScene(b, SceneOutcome::Loaded, projectB.c_str()), "accept B while its root is unreadable");
        const auto denied = cache::Inspect();
        Check(denied.error == L"inventory-unreadable" && unreadableAttributesHits.load() > 0,
            "real Guard refusal separates the two Loaded events");
        const auto c = cache::AcceptScene(SceneOperation::Load);
        Check(cache::FinishScene(c, SceneOutcome::Loaded, projectC.c_str()), "accept C before B can persist");
    }
    {
        blockedIndex = index.wstring();
        blockedIndexMoves = 0;
        failIndex = true;
        struct RestoreFault { ~RestoreFault() { failIndex = false; } } restore;
        (void)cache::Inspect();
        Check(blockedIndexMoves.load() > 0, "admitted access reaches a failed index publication");
    }

    const auto other = Case(output, L"deferred-owners-other-root");
    Store(1, MiB); // Same relative name as keep, in a different physical root.
    const auto elsewhere = cache::Inspect();
    Check(elsewhere.error.empty() && elsewhere.textureBytes == MiB, "other root contains its own payload");
    for (const auto& row : elsewhere.projects)
        Check(row.path != projectB.wstring(), "unrelated root cannot inherit B's pending projection");
    const auto reset = cache::AcceptScene(SceneOperation::Reset);
    Check(cache::FinishScene(reset, SceneOutcome::ResetComplete), "retire scene usage while original root is switched out");
    Check(ProbeDeletion(keep) == ERROR_SUCCESS && ProbeDeletion(replaced) == ERROR_SUCCESS,
        "pending owner evidence does not keep a no-delete scene lease after reset");

    const auto replacement = root.parent_path() / L"replacement.mtc";
    Check(fs::copy_file(replaced, replacement), "construct same-size same-content replacement");
    const auto newId = IdentifyPayload(replacement);
    Check(!SameLeaseIdentity(oldId, newId), "replacement has distinct physical identity before rename");
    Check(MoveFileExW(replacement.c_str(), replaced.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
        "publish replacement at the exact old cache name");
    Check(!SameLeaseIdentity(IdentifyPayload(index), IdentifyPayload(savedIndex)), "saved index is a distinct file");
    Check(MoveFileExW(savedIndex.c_str(), index.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
        "force verified reload from the pre-load index without any historical B ownership");

    SetRoot(root);
    const auto recovered = cache::Inspect();
    const auto owner = FindProjectRow(recovered, projectB);
    Check(recovered.error.empty() && recovered.textureBytes == 3 * MiB &&
        recovered.totalBytes == 3 * MiB + IndexCharge(), "owner retry preserves exact physical accounting");
    Check(SameLeaseIdentity(keepId, IdentifyPayload(keep)) && SameLeaseIdentity(newId, IdentifyPayload(replaced)),
        "the unchanged file and the replacement still have their independent identities");
    std::fprintf(stderr, "deferred B owner bytes=%llu shared=%llu current=%d expected=%llu\n",
        static_cast<unsigned long long>(owner.totalBytes), static_cast<unsigned long long>(owner.sharedBytes),
        int(owner.current), static_cast<unsigned long long>(MiB));
    Check(!owner.current && owner.totalBytes == MiB && owner.sharedBytes == 0,
        "B retains only the unchanged physical use across consecutive Loaded, failed Save and index reload");
    Check(ProbeDeletion(keep) == ERROR_SUCCESS && ProbeDeletion(replaced) == ERROR_SUCCESS,
        "historical replay does not acquire a current scene lease");

    SetRoot(other);
    (void)cache::Inspect();
    SetRoot(root);
    const auto reopened = cache::Inspect();
    const auto replayed = FindProjectRow(reopened, projectB);
    Check(reopened.error.empty() && !replayed.current && replayed.totalBytes == MiB && !replayed.sharedBytes,
        "revisiting the root is idempotent after owner settlement");
}
void RetiredPayloadRebuild(const fs::path& output) {
    const auto root = Case(output, L"owner-retired-name");
    const auto project = root.parent_path() / L"B.pmm";
    cache::BeginProject(project.c_str()); cache::ProjectOpened(project.c_str()); cache::ResetScene();
    Store(1, MiB); cache::ResetScene();
    const auto path = Entry(1);
    const auto original = IdentifyPayload(path);
    const auto b = cache::AcceptScene(cache::SceneOperation::Load);
    cache::Touch(path);
    Check(ProbeDeletion(path) == ERROR_SHARING_VIOLATION, "genuine scene lease denies DELETE before retirement");
    cache::WriteReservation writer(path, MiB);
    Check(bool(writer), "reserve actual same-name rebuild before scene replacement");
    Check(cache::FinishScene(b, cache::SceneOutcome::Loaded, project.c_str()), "retain B owner evidence before rebuild");
    const auto reset = cache::AcceptScene(cache::SceneOperation::Reset);
    Check(cache::FinishScene(reset, cache::SceneOutcome::ResetComplete), "retire scene before cache name reuse");
    Check(ProbeDeletion(path) == ERROR_SUCCESS, "ordinary no-delete lease retired");
    Check(DeleteFileW(path.c_str()) != FALSE, "ordinary DeleteFileW accepts retired payload");
    {
        Handle rebuilt(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(rebuilt.value != INVALID_HANDLE_VALUE, "CREATE_NEW can reuse the exact retired name immediately");
    }
    const auto recreated = IdentifyPayload(path);
    WriteFile(path, MiB, writer); // TrackTemporary, close, REPLACE_EXISTING|WRITE_THROUGH.
    const auto published = IdentifyPayload(path);
    std::fprintf(stderr, "retired-name old=%lu/%lu:%lu recreated=%lu/%lu:%lu published=%lu/%lu:%lu\n",
        original.volume,original.high,original.low,recreated.volume,recreated.high,recreated.low,
        published.volume,published.high,published.low);
    Check(!SameLeaseIdentity(recreated, published) && fs::file_size(path) == MiB,
        "production-style tracked rename replaced the recreated name with the staged instance");
    Check(writer.Commit(), "stale writer can persist rebuilt physical payload");
    const auto view = cache::Inspect();
    Check(view.error.empty() && FindProjectRow(view, project).totalBytes == 0 && view.textureBytes == MiB &&
        ProbeDeletion(path) == ERROR_SUCCESS, "retired B has no ownership or lease on the rebuilt payload");
}

void UncertainOwnerRecovery(const fs::path& output) {
    const auto root = Case(output, L"owner-uncertain");
    const auto project = root.parent_path() / L"B.pmm";
    cache::BeginProject(project.c_str()); cache::ProjectOpened(project.c_str()); cache::ResetScene();
    Store(1, MiB); cache::ResetScene();
    const auto path = Entry(1);
    const auto id = IdentifyPayload(path);
    const auto b = cache::AcceptScene(cache::SceneOperation::Load);
    cache::Touch(path);
    Check(cache::FinishScene(b, cache::SceneOutcome::Loaded, project.c_str()), "retain a verified B use");
    const auto reset = cache::AcceptScene(cache::SceneOperation::Reset);
    Check(cache::FinishScene(reset, cache::SceneOutcome::ResetComplete), "release both live witnesses before retry");
    {
        struct Restore { ~Restore() { denyOwnerJournal = false; } } restore;
        deniedJournalReads = 0; denyOwnerJournal = true;
        const auto uncertain = cache::Inspect();
        Check(deniedJournalReads && uncertain.error == L"owner-identity-unconfirmed" &&
            FindProjectRow(uncertain, project).totalBytes == 0 && ProbeDeletion(path) == ERROR_SUCCESS,
            "matching name, file ID and birth time alone cannot confirm a retired use");
        Store(2, MiB);
        Check(cache::Inspect().textureBytes == 2*MiB, "uncertainty permits independent tracked publication");
    }
    const auto verified = cache::Inspect();
    Check(verified.error.empty() && FindProjectRow(verified, project).totalBytes == MiB &&
        SameLeaseIdentity(id, IdentifyPayload(path)), "restored journal evidence settles the stable historical use");
}

void PublishedOwnerVerification(const fs::path& output) {
    const auto root = Case(output, L"owner-final-read");
    const auto project = root.parent_path() / L"B.pmm";
    cache::BeginProject(project.c_str()); cache::ProjectOpened(project.c_str()); cache::ResetScene();
    Store(1, MiB); cache::ResetScene();
    const auto path = Entry(1);
    const auto oldId = IdentifyPayload(path);
    const auto b = cache::AcceptScene(cache::SceneOperation::Load);
    {
        Handle blocker(CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Check(blocker.value != INVALID_HANDLE_VALUE, "hold DELETE access during genuine B use");
        cache::Touch(path);
    }
    Check(ProbeDeletion(path) == ERROR_SUCCESS, "verified use survived the failed scene read lease");
    allocationFailed = false;
    failNextAllocation = true;
    const bool accepted = cache::FinishScene(b, cache::SceneOutcome::Loaded, project.c_str());
    failNextAllocation = false;
    Check(!accepted && allocationFailed.load(), "allocation refusal leaves the B operation retryable");
    Check(cache::FinishScene(b, cache::SceneOutcome::Loaded, project.c_str()), "retry B before final-proof fault");
    const auto index = root / L"Management" / L"index.bin";
    const auto before = IdentifyPayload(index);
    const auto baselineBytes = fs::file_size(index);
    {
        struct Restore { ~Restore() { rejectFinalIndexRead=false; unreadableAttributes=false; unreadableAttributesPath.clear(); } } restore;
        blockedIndex = unreadableAttributesPath = index.wstring();
        unreadableAttributesHits = renamedBeforeReadFault = 0;
        rejectFinalIndexRead = true;
        (void)cache::Inspect();
        Check(renamedBeforeReadFault == 1 && unreadableAttributesHits.load() > 0 &&
            !SameLeaseIdentity(before, IdentifyPayload(index)), "atomic replace succeeded before final index validation failed");
        auto reset = cache::AcceptScene(cache::SceneOperation::Reset);
        Check(cache::FinishScene(reset, cache::SceneOutcome::ResetComplete), "reset before exact-index recovery");
        const auto replacement = root.parent_path() / L"new-payload.mtc";
        Check(fs::copy_file(path, replacement) && !SameLeaseIdentity(oldId, IdentifyPayload(replacement)),
            "make a different physical file after the unverified index rename");
        Check(MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) != FALSE,
            "replace payload before verification can resume");
    }
    Check(fs::file_size(index) == baselineBytes + 4 + sizeof(wchar_t)*project.wstring().size(),
        "the renamed index contains one new owner string for the genuine lease-free use");
    const auto view = cache::Inspect();
    const auto row = FindProjectRow(view, project);
    Check(view.error.empty() && !row.current && !row.totalBytes && view.textureBytes == MiB &&
        fs::file_size(index) == baselineBytes,
        "own unverified index additions cannot become historical owners of a replacement");
}

void SavedSceneOwners(const fs::path& output) {
    const auto root = Case(output, L"saved-scene-owners");
    cache::Shutdown();
    const auto a = root.parent_path() / L"A.pmm", b = root.parent_path() / L"B.pmm";
    const auto c = root.parent_path() / L"C.pmm", d = root.parent_path() / L"D.pmm";
    Store(1, MiB); // Unowned until the anonymous scene first saves.
    cache::BeginProject(a.c_str()); cache::ProjectOpened(a.c_str());
    Store(2, 2*MiB); // Historical A use; only entry 1 is used after reset.
    const auto reset = cache::AcceptScene(cache::SceneOperation::Reset);
    Check(!cache::CaptureActiveScene() && cache::FinishScene(reset, cache::SceneOutcome::ResetComplete),
          "only a completed reset exposes an anonymous save ticket");
    const auto first = cache::CaptureActiveScene();
    cache::Touch(Entry(1));
    Check(cache::BindSavedScene(first, a.c_str()), "first save binds anonymous real use to A");
    Store(3, 4*MiB);
    const auto before = cache::Inspect(); // Settle A's owner debt before either save-as.
    Check(FindProjectRow(before, a).current && FindProjectRow(before, a).totalBytes == 7*MiB,
          "named A retains historical ownership after first save");
    Check(cache::BindSavedScene(cache::CaptureActiveScene(), b.c_str()), "same-scene save-as to B");
    {
        blockedIndex = (root / L"Management" / L"index.bin").wstring();
        blockedIndexMoves = 0; failIndex = true;
        struct Restore { ~Restore() { failIndex = false; } } restore;
        (void)cache::Inspect();
        Check(blockedIndexMoves > 0, "B's owner debt reaches the refused index publication");
        Check(cache::BindSavedScene(cache::CaptureActiveScene(), c.c_str()), "second save-as before B settles");
    }
    auto view = cache::Inspect();
    Check(FindProjectRow(view,b).totalBytes == 5*MiB && FindProjectRow(view,b).sharedBytes == 5*MiB,
          "B keeps its own debt, excluding A's historical-only entry");
    Check(FindProjectRow(view,c).current && FindProjectRow(view,c).totalBytes == 5*MiB &&
          !FindProjectRow(view,b).current, "settlement does not consume the scene's uses");

    const auto load = cache::AcceptScene(cache::SceneOperation::Load);
    cache::Touch(Entry(1));
    Check(cache::FinishScene(load, cache::SceneOutcome::Loaded, a.c_str()), "open A in a new generation");
    cache::Touch(Entry(2)); // Previously owned by A; must still count as actual use.
    const auto active = cache::CaptureActiveScene();
    Check(!cache::BindSavedScene(first, c.c_str()) && FindProjectRow(cache::Inspect(),a).current,
          "stale completion cannot detach or rebind the active project");
    allocationFailed = false;
    failNextAllocation = true;
    const bool bound = cache::BindSavedScene(active, d.c_str());
    failNextAllocation = false;
    Check(!bound && allocationFailed, "inject real allocation failure after document publication");
    Store(4, 8*MiB);
    view = cache::Inspect();
    Check(!FindProjectRow(view,a).current && FindProjectRow(view,a).totalBytes == 7*MiB,
          "failed rebind detaches A before subsequent use");
    Check(cache::BindSavedScene(active, d.c_str()), "retry a local bind for the same published document");
    view = cache::Inspect();
    Check(view.error.empty() && FindProjectRow(view,d).current && FindProjectRow(view,d).totalBytes == 11*MiB &&
          FindProjectRow(view,d).sharedBytes == 3*MiB && view.textureBytes == 15*MiB,
          "D owns load-time, previously owned and detached-period uses but not inherited leases");
}

void DeferredOwnerBudget(const fs::path& output) {
    const auto root = Case(output, L"owner-index-peak");
    cache::Shutdown();
    Store(1, MiB); Store(2, MiB); cache::ResetScene();
    wchar_t volume[32768]{};
    DWORD sectors=0, bytes=0, freeClusters=0, clusters=0;
    Check(GetVolumePathNameW(root.c_str(), volume, 32768) &&
        GetDiskFreeSpaceW(volume, &sectors, &bytes, &freeClusters, &clusters), "measure fixture allocation unit");
    const auto unit = std::uint64_t(sectors)*bytes;
    Check(unit != 0, "volume has a usable allocation unit");
    if (unit > 65536) { std::puts("SKIP owner page boundary: volume allocation unit exceeds fixture geometry"); return; }
    fs::path project = root.parent_path();
    while (project.native().size() < unit*2/5 + 512) project /= std::wstring(96, L'p');
    project /= L"B.pmm";
    const auto token = cache::AcceptScene(cache::SceneOperation::Load);
    cache::Touch(Entry(1)); cache::Touch(Entry(2));
    // Measure admission, including the old on-disk index and its replacement page.
    std::uint64_t lo=0, hi=64*MiB/unit;
    while (lo < hi) {
        const auto n = lo + (hi-lo+1)/2;
        if (cache::WriteReservation(Entry(3), n*unit)) lo=n; else hi=n-1;
    }
    Check(lo && !cache::WriteReservation(Entry(3), (lo+1)*unit), "old projection has a measured last admissible page");
    auto held = std::make_unique<cache::WriteReservation>(Entry(3), lo*unit);
    Check(bool(*held), "hold the last pre-binding payload reservation");
    const auto index = root / L"Management" / L"index.bin";
    const auto original = IdentifyPayload(index);
    Check(cache::FinishScene(token, cache::SceneOutcome::Loaded, project.c_str()), "locally accept owner additions at budget limit");
    (void)cache::Inspect();
    Check(SameLeaseIdentity(original, IdentifyPayload(index)), "larger owner index cannot publish over the held peak");
    Check(!cache::WriteReservation(Entry(4), unit), "pending metadata cannot grant another page");
    held.reset();
    const auto view = cache::Inspect();
    Check(view.error.empty() && FindProjectRow(view, project).totalBytes == 2*MiB &&
        !SameLeaseIdentity(original, IdentifyPayload(index)), "owner index publishes after physical reservation is refunded");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc==3&&std::wcscmp(argv[1],L"--mutex-check")==0){
        Handle mutex(OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,argv[2]));if(!mutex.value)return 3;
        const auto wait=WaitForSingleObject(mutex.value,1500);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)return 4;
        ReleaseMutex(mutex.value);return 0;
    }
    try{
        if (argc == 3 && std::wcscmp(argv[1], L"--verify-deferred") == 0) {
            const fs::path output(argv[2]);
            const auto root = output / L"deferred-loaded-owners" / L"Cache";
            Check(output.is_absolute() && fs::is_regular_file(root / L"Management" / L"index.bin"),
                "require the existing deferred-owner fixture index for fresh-process verification");
            SetRoot(root);
            const auto disk = cache::Inspect();
            const auto owner = FindProjectRow(disk, root.parent_path() / L"B.pmm");
            Check(disk.error.empty() && !owner.current && owner.totalBytes == MiB && !owner.sharedBytes &&
                disk.textureBytes == 3 * MiB, "fresh process reads only the surviving B use from the settled index");
            std::puts("PASS persisted deferred owner in fresh process");
            return 0;
        }
        Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"require --store NEW_ABSOLUTE_DIR");
        const fs::path output=argv[2];Check(output.is_absolute()&&!fs::exists(output),"refuse reused or relative output directory");
        Check(fs::create_directories(output),"create isolated output directory");
        Hooks hooks;PeakAndNested(output);RootLifetime(output);FailedRegistration(output);OrphanRetryOwnership(output);PolicyFailures(output);
        UnderReservedPublication(output);ChangedCleanupCandidate(output);UnconfirmedDeletion(output);ConstructorFailureUnlocks(output);
        ShutdownReleasesExistingLease(output);
        ShutdownMultipleLiveRoots(output);ShutdownUnknownConfiguredRoot(output);ShutdownOrphanResponsibility(output);
        SceneOperationIdentity(output);
        DeferredLoadedOwners(output);
        RetiredPayloadRebuild(output);
        UncertainOwnerRecovery(output);
        PublishedOwnerVerification(output);
        DeferredOwnerBudget(output);
        SavedSceneOwners(output);
        cache::Shutdown();std::printf("PASS cache manager reservation %u checks; sparse real files and isolated mutex observer\n",checks);return 0;
    }catch(const std::exception& error){failNextAllocation=false;std::fprintf(stderr,"FAIL cache manager reservation: %s\n",error.what());return 1;}
}
