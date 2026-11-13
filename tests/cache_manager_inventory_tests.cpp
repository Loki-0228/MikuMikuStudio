// MAINTENANCE: regression-keep. Real sparse files under a fresh private root.
// Admission is tested independently of the public physical-usage display.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <MinHook.h>
#include "mikumikustudio/cache_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace cache=mikumikustudio::cache_manager;
using Bytes=std::vector<unsigned char>;
constexpr std::uint64_t MiB=1024ull*1024,Limit=64*MiB;
unsigned checks=0,tempSerial=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"DIAG check=%u win32=%lu\n",checks,GetLastError());throw std::runtime_error(why);}}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){Close();}void Close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=INVALID_HANDLE_VALUE;}Handle(const Handle&)=delete;};
void NewDirectory(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"absolute normalized owned test root");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim fresh owned test directory");}
void Parents(const fs::path& p){std::error_code error;fs::create_directories(p.parent_path(),error);Check(!error,"create only owned fixture parents");}
bool Exists(const fs::path& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;}
Bytes Read(const fs::path& p){std::ifstream file(p,std::ios::binary);Check(bool(file),"read only owned evidence file");return Bytes(std::istreambuf_iterator<char>(file),{});}
void WriteSmall(const fs::path& p,const Bytes& bytes){Parents(p);Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(h.value!=INVALID_HANDLE_VALUE,"create unique small evidence file");DWORD done=0;
    Check(WriteFile(h.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(h.value),"write exact small evidence bytes");}
void Fill(HANDLE h,std::uint64_t size,bool texture){DWORD done=0;Check(DeviceIoControl(h,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&done,nullptr)!=FALSE,"use actual sparse fixture with bounded physical disk consumption");
    LARGE_INTEGER zero{};Check(SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)!=FALSE,"rewind own fixture writer");
    if(texture){Check(size>=76&&size-76<=UINT32_MAX,"recognized texture size fits its header");std::uint32_t header[19]{};header[0]=0x4354504d;header[1]=2;header[2]=std::uint32_t(size-76);
        Check(WriteFile(h,header,sizeof header,&done,nullptr)&&done==sizeof header,"write real recognized texture header");}
    else if(size){const unsigned char value=0x5a;Check(WriteFile(h,&value,1,&done,nullptr)&&done==1,"write sentinel byte into sparse unknown stream");}
    LARGE_INTEGER length{};length.QuadPart=LONGLONG(size);Check(SetFilePointerEx(h,length,nullptr,FILE_BEGIN)&&SetEndOfFile(h)&&FlushFileBuffers(h),"set and flush exact logical sparse length");}
void Sparse(const fs::path& p,std::uint64_t size,bool texture=false){Parents(p);Handle h(CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(h.value!=INVALID_HANDLE_VALUE,"create unique sparse payload without truncating prior file");Fill(h.value,size,texture);}
std::uint64_t Charge(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));FILE_STANDARD_INFO info{};
    Check(h.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(h.value,FileStandardInfo,&info,sizeof info)&&info.EndOfFile.QuadPart>=0&&info.AllocationSize.QuadPart>=0,"independent actual default-stream charge");return (std::max)(std::uint64_t(info.EndOfFile.QuadPart),std::uint64_t(info.AllocationSize.QuadPart));}
std::wstring Canonical(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));wchar_t buffer[32768]{};
    const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,buffer,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"resolve exact owned file for metadata failure");return std::wstring(buffer,n);}
void Select(const fs::path& root){Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",root.c_str())&&SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(root.parent_path()/L"settings.json").c_str()),"isolate all cache and policy state");}
void Configure(const fs::path& root,bool automatic=false){Select(root);cache::Settings settings;settings.limitBytes=Limit;settings.automaticCleanup=automatic;std::wstring error;
    Check(cache::Configure(settings,error),"configure real minimum 64 MiB cache policy");Check(cache::Root()==root,"manager resolves this exact isolated root");}
fs::path Case(const fs::path& parent,const wchar_t* name,bool automatic=false){const auto base=parent/name;NewDirectory(base);const auto root=base/L"Cache";Configure(root,automatic);return root;}
fs::path Entry(const fs::path& root,unsigned id){wchar_t name[65]{};swprintf_s(name,L"%064x",id);return root/L"Textures"/L"shared-texture"/(std::wstring(name)+L".mtc");}
struct Temporary {fs::path path;Handle file;explicit Temporary(const fs::path& final):path(final.native()+L".tmp-inventory-"+std::to_wstring(++tempSerial)){
    Parents(path);file.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(file.value!=INVALID_HANDLE_VALUE,"CREATE_NEW own zero temporary file while keeping original writer open");}
    void Publish(const fs::path& target){file.Close();Check(MoveFileExW(path.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"atomically publish only owned staged payload");}};
void Store(const fs::path& root,unsigned id,std::uint64_t bytes){const auto target=Entry(root,id);cache::WriteReservation token(target,bytes);Check(bool(token),"reserve actual managed payload");Temporary temp(target);
    Check(token.TrackTemporary(temp.path),"bind actual empty CREATE_NEW identity to reservation");Fill(temp.file.value,bytes,true);temp.Publish(target);Check(token.Commit(),"persist actual payload and index publication");cache::ResetScene();}
void Project(const fs::path& root){const auto path=root.parent_path()/L"owned-project.pmm";cache::BeginProject(path.c_str());cache::ProjectOpened(path.c_str());cache::ResetScene();Check(Exists(root/L"Management"/L"index.bin"),"create actual valid project index metadata");}

struct Fault {std::wstring index,blockedMetadata,blockedMissing;wchar_t failedTemp[32768]{};HANDLE cleanupHandle=INVALID_HANDLE_VALUE;bool blockMove=false,blockDelete=false,blockMetadata=false,failMetadataAfterMove=false,metadataAfterMove=false;unsigned moves=0,deletes=0,metadata=0,missing=0;};
struct PublicationProbe {
    std::wstring inventoryName, indexTempPrefix, lateFile;
    unsigned observations=0;
    bool injected=false;
};
thread_local Fault* active=nullptr;
thread_local PublicationProbe* publicationProbe=nullptr;
decltype(&MoveFileExW) realMove=nullptr;
decltype(&DeleteFileW) realDelete=nullptr;
decltype(&CreateFileW) realCreate=nullptr;
decltype(&SetFileInformationByHandle) realSetInformation=nullptr;
decltype(&GetFileAttributesExW) realAttributes=nullptr;
const wchar_t* PlainDosPath(const wchar_t* path)noexcept{return path&&std::wcslen(path)>=7&&!std::wcsncmp(path,L"\\\\?\\",4)&&path[5]==L':'?path+4:path;}
BOOL WINAPI AttributesHook(LPCWSTR path,GET_FILEEX_INFO_LEVELS level,LPVOID output)noexcept {auto* f=active;
    if(f&&!f->blockedMissing.empty()&&path&&!_wcsicmp(PlainDosPath(path),PlainDosPath(f->blockedMissing.c_str()))){++f->missing;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realAttributes(path,level,output);}
BOOL WINAPI MoveHook(LPCWSTR from,LPCWSTR to,DWORD flags)noexcept {auto* f=active;if(f&&f->blockMove&&to&&!_wcsicmp(to,f->index.c_str())){
    ++f->moves;if(from)wcsncpy_s(f->failedTemp,from,_TRUNCATE);SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    const auto result=realMove(from,to,flags);const auto error=GetLastError();if(result&&f&&f->failMetadataAfterMove&&to&&!_wcsicmp(to,f->index.c_str())){++f->moves;f->metadataAfterMove=true;}SetLastError(error);return result;}
BOOL WINAPI DeleteHook(LPCWSTR path)noexcept {auto* f=active;if(f&&f->blockDelete&&f->failedTemp[0]&&path&&!_wcsicmp(path,f->failedTemp)){++f->deletes;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return realDelete(path);}
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE source)noexcept {auto* f=active;
    if(auto* probe=publicationProbe;probe&&path){
        if(access==FILE_READ_ATTRIBUTES&&disposition==OPEN_EXISTING&&!_wcsicmp(path,probe->inventoryName.c_str()))
            ++probe->observations;
        if(!probe->lateFile.empty()&&!probe->injected&&disposition==CREATE_NEW&&
           !_wcsnicmp(path,probe->indexTempPrefix.c_str(),probe->indexTempPrefix.size())){
            Handle foreign(realCreate(probe->lateFile.c_str(),GENERIC_WRITE,0,nullptr,
                CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(foreign.value!=INVALID_HANDLE_VALUE){
                LARGE_INTEGER length{};length.QuadPart=Limit;
                DWORD ignored=0;
                probe->injected=DeviceIoControl(foreign.value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&ignored,nullptr)&&
                    SetFilePointerEx(foreign.value,length,nullptr,FILE_BEGIN)&&SetEndOfFile(foreign.value)&&
                    FlushFileBuffers(foreign.value);
            }
        }
    }
    if(f&&(f->blockMetadata||f->metadataAfterMove)&&path&&access==FILE_READ_ATTRIBUTES&&disposition==OPEN_EXISTING&&!_wcsicmp(path,f->blockedMetadata.c_str())){++f->metadata;f->metadataAfterMove=false;SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}
    const auto result=realCreate(path,access,sharing,security,disposition,flags,source);const auto error=GetLastError();
    if(f&&f->blockDelete&&result!=INVALID_HANDLE_VALUE&&path&&f->failedTemp[0]&&(access&DELETE)&&disposition==OPEN_EXISTING&&!_wcsicmp(path,f->failedTemp))f->cleanupHandle=result;
    SetLastError(error);return result;}
BOOL WINAPI SetInformationHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)noexcept {auto* f=active;
    if(f&&f->blockDelete&&h==f->cleanupHandle&&h!=INVALID_HANDLE_VALUE&&kind==FileDispositionInfo){++f->deletes;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realSetInformation(h,kind,data,size);}
struct Hooks {Hooks(){Check(MH_Initialize()==MH_OK,"initialize scoped manager failure hooks");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MoveFileExW),reinterpret_cast<void*>(&MoveHook),reinterpret_cast<void**>(&realMove))==MH_OK,"hook only this fixture index publication");
    Check(MH_CreateHook(reinterpret_cast<void*>(&DeleteFileW),reinterpret_cast<void*>(&DeleteHook),reinterpret_cast<void**>(&realDelete))==MH_OK,"hook only failed owned index temporary cleanup");
    Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"hook exact metadata scan access failure");
    Check(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&SetInformationHook),reinterpret_cast<void**>(&realSetInformation))==MH_OK,"hook exact retained-index-temp same-handle cleanup failure");
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetFileAttributesExW),reinterpret_cast<void*>(&AttributesHook),reinterpret_cast<void**>(&realAttributes))==MH_OK,"hook only exact unknown-file absence confirmation");Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable local manager test hooks");}
    ~Hooks(){active=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm {explicit Arm(Fault& fault){Check(!active,"nonrecursive scoped manager fault");active=&fault;}~Arm(){active=nullptr;}};
struct ObservePublication {
    explicit ObservePublication(PublicationProbe& probe){Check(!publicationProbe,"one scoped publication probe");publicationProbe=&probe;}
    ~ObservePublication(){publicationProbe=nullptr;}
};

void Unknowns(const fs::path& output){const auto root=Case(output,L"unknowns",true);Project(root);const auto index=root/L"Management"/L"index.bin";const auto indexBefore=Read(index);
    const auto unknown=root/L"foreign"/L"unowned.bin",stream=fs::path(unknown.native()+L":secret"),foreignTemp=root/L"Management"/L"foreign.tmp";
    Sparse(unknown,32*MiB);Sparse(stream,16*MiB);Sparse(foreignTemp,20*MiB);
    const auto state=cache::Inspect();Check(state.totalBytes>=68*MiB+Charge(index),"actual usage includes unknown default/ADS, foreign temp and real index");
    Check(!cache::WriteReservation(Entry(root,1),MiB),"unknown physical responsibility blocks new managed allocation even with automatic cleanup");
    const auto cleaned=cache::Clean(cache::Cleanup::All);Check(cleaned.files==0&&Exists(unknown)&&Exists(stream)&&Exists(foreignTemp)&&Read(index)==indexBefore,"unrecognized payloads/ADS/temp are never made eligible for automatic deletion");
    Check(Charge(unknown)==32*MiB&&Charge(stream)==16*MiB&&Charge(foreignTemp)==20*MiB,"refusal preserves every unknown stream length");
}
void Hardlinks(const fs::path& output){const auto root=Case(output,L"hardlinks");Store(root,1,40*MiB);const auto managed=Entry(root,1),alias=root/L"unknown-alias.bin";
    Check(CreateHardLinkW(alias.c_str(),managed.c_str(),nullptr)!=FALSE,"add unknown hardlink only after legitimate managed publication and lease release");
    const auto before=cache::Inspect();Check(before.totalBytes>=40*MiB&&before.totalBytes<41*MiB,"hardlink names are charged once by physical identity, plus actual metadata");
    {cache::WriteReservation safe(Entry(root,2),16*MiB);Check(bool(safe),"duplicate hardlink name does not consume another forty MiB");}
    Check(!cache::WriteReservation(Entry(root,3),25*MiB),"one physical forty MiB file still consumes capacity");
    const auto cleaned=cache::Clean(cache::Cleanup::All);Check(cleaned.files==1&&!Exists(managed)&&Exists(alias),"explicit managed-name cleanup does not delete unknown hardlink alias");
    Check(cache::Inspect().totalBytes>=40*MiB&&!cache::WriteReservation(Entry(root,4),25*MiB),"removing one known hardlink does not falsely refund underlying unknown allocation");
    const auto again=cache::Clean(cache::Cleanup::All);Check(again.files==0&&Exists(alias)&&Charge(alias)==40*MiB,"unknown remaining alias never inherits cleanup permission");
}
void Peak(const fs::path& output){const auto root=Case(output,L"old-new-peak");Store(root,1,40*MiB);const auto target=Entry(root,1);
    Check(!cache::WriteReservation(target,40*MiB)&&Charge(target)==40*MiB,"rewrite reserves old target and full new candidate simultaneously");
    auto settings=cache::Current();settings.automaticCleanup=true;std::wstring error;Check(cache::Configure(settings,error),"enable cleanup around old target protection");
    Check(!cache::WriteReservation(target,40*MiB)&&Exists(target),"automatic cleanup cannot erase active rewrite target to manufacture headroom");
}
void Tracked(const fs::path& output){const auto root=Case(output,L"tracked-partial");const auto target=Entry(root,1);cache::WriteReservation first(target,40*MiB);Check(bool(first),"reserve forty MiB candidate");Temporary temp(target);
    Check(first.TrackTemporary(temp.path),"associate exact currently empty temporary identity");Fill(temp.file.value,24*MiB,true);
    {cache::WriteReservation second(Entry(root,2),16*MiB);Check(bool(second),"tracked partial actual bytes plus reservation shortfall are not double counted");
        Check(!cache::WriteReservation(Entry(root,3),9*MiB),"nested reservations still account for all remaining promised bytes");}
    Fill(temp.file.value,48*MiB,true);
    Check(!cache::WriteReservation(Entry(root,4),17*MiB),"actual temporary growth beyond declaration cannot be hidden by the smaller reservation");
    {cache::WriteReservation second(Entry(root,5),8*MiB);Check(bool(second),"actual-over-reserved temporary uses maximum rather than sum");}
    // The producer owns cleanup. The manager cannot refund an observed file
    // merely because a reservation object goes away or DeleteFile was tried.
    temp.file.Close();Check(DeleteFileW(temp.path.c_str())!=FALSE,"explicitly remove own completed partial fixture while token stays scoped");
}
void FailedTemporaryCleanup(const fs::path& output){const auto root=Case(output,L"failed-temp-cleanup");const auto target=Entry(root,1);auto token=std::make_unique<cache::WriteReservation>(target,40*MiB);Check(bool(*token),"reserve temp cleanup fixture");Temporary temp(target);
    Check(token->TrackTemporary(temp.path),"track cleanup fixture before writing");Fill(temp.file.value,40*MiB,true);
    Check(!DeleteFileW(temp.path.c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,"real live writer prevents cleanup, without synthetic deletion success");token.reset();
    Check(!cache::WriteReservation(Entry(root,2),25*MiB)&&Exists(temp.path),"failed cleanup plus abandoned token does not refund existing temporary bytes");
    {cache::WriteReservation safe(Entry(root,3),16*MiB);Check(bool(safe),"abandoned temp physical bytes are not duplicated with dead reservation promise");}
    temp.file.Close();Check(DeleteFileW(temp.path.c_str())!=FALSE,"explicit producer cleanup succeeds after writer closes");
    Check(bool(cache::WriteReservation(Entry(root,4),60*MiB)),"confirmed disappearance releases observed temporary occupation");
}
void TrackingFailurePreservesOwner(const fs::path& output){const auto root=Case(output,L"tracking-failure");const auto target=Entry(root,1);cache::WriteReservation token(target,8*MiB);Check(bool(token),"reserve tracked owner identity fixture");Temporary own(target);
    Check(token.TrackTemporary(own.path),"associate original owned empty file");const auto foreign=fs::path(target.native()+L".tmp-existing-foreign");const Bytes original{1,2,3,4,5};WriteSmall(foreign,original);
    Check(!token.TrackTemporary(foreign)&&Read(foreign)==original&&bool(token),"nonempty pre-existing temp cannot replace prior tracked identity or be truncated");Fill(own.file.value,6*MiB,true);
    {cache::WriteReservation remaining(Entry(root,2),55*MiB);Check(bool(remaining),"failed TrackTemporary leaves original partial identity deduction intact");}
    own.file.Close();Check(DeleteFileW(own.path.c_str())!=FALSE,"explicitly release own tracking fixture");
}
void UntrackedCannotAdopt(const fs::path& output){const auto root=Case(output,L"untracked-cannot-adopt");const auto foreign=Entry(root,1);Sparse(foreign,4*MiB,true);
    {cache::WriteReservation token(foreign,4*MiB);Check(bool(token),"headroom alone can reserve a rewrite without granting old-file ownership");
        Check(!token.Commit(),"Commit without tracked created identity cannot adopt an unknown valid-header target");}
    cache::ResetScene();Check(bool(cache::WriteReservation(Entry(root,2),MiB)),"rejected untracked Commit leaves no false pending registration barrier");
    const auto cleaned=cache::Clean(cache::Cleanup::All);Check(cleaned.files==0&&Exists(foreign)&&Charge(foreign)==4*MiB,"valid header and failed Commit never authorize deleting the unknown file");
}
void MetadataPeakAndUnique(const fs::path& output){const auto root=Case(output,L"metadata-peak");Project(root);const auto index=root/L"Management"/L"index.bin";const auto legacyTemp=fs::path(index.native()+L".tmp-"+std::to_wstring(GetCurrentProcessId()));
    const Bytes foreign{9,8,7,6,5};WriteSmall(legacyTemp,foreign);const auto project=root.parent_path()/L"owned-project.pmm";
    Check(cache::Pin(project.native(),true)&&Read(legacyTemp)==foreign,"new index publication uses unique CREATE_NEW temp, preserving existing same-PID temp");
    const auto indexBefore=Read(index);const auto used=cache::Inspect().totalBytes;Check(used>0&&used<MiB,"baseline physical metadata usage is finite and nonzero");
    const auto filler=root/L"unknown-fill.bin";Sparse(filler,Limit-used);
    Check(cache::Inspect().totalBytes>=Limit,"real sparse unknown file fills remaining physical accounting limit");
    Check(!cache::Pin(project.native(),false)&&Read(index)==indexBefore&&Read(legacyTemp)==foreign&&Exists(filler),"metadata-only save requires extra index peak and never overwrites prior committed index at capacity");
    const auto after=cache::Inspect();Check(after.projects.size()==1&&after.projects[0].pinned&&Read(index)==indexBefore,"failed metadata-only Pin also preserves live pin state and cannot auto-publish on Inspect retry");
    Check(!cache::WriteReservation(Entry(root,1),1),"metadata-only over-budget state also refuses payload reservations");
}
void PublicationInventoryBoundary(const fs::path& output){
    const auto root=Case(output,L"publication-inventory");Project(root);
    const auto sentinel=root/L"foreign-small.bin";
    const Bytes sentinelBytes{8,1,5};
    WriteSmall(sentinel,sentinelBytes);
    const auto index=root/L"Management"/L"index.bin";
    {
        const auto target=Entry(root,1);
        cache::WriteReservation token(target,MiB);Check(bool(token),"reserve a first payload");
        Temporary temp(target);
        Check(token.TrackTemporary(temp.path),"track a real empty payload temporary");
        Fill(temp.file.value,MiB,true);temp.Publish(target);
        PublicationProbe probe{Canonical(sentinel)};
        {ObservePublication observe(probe);Check(token.Commit(),"commit the payload and its index");}
        Check(probe.observations==3,
            "one full commit scan and two mandatory index scans observe the unrelated file");
        Check(Read(sentinel)==sentinelBytes,"cache publication leaves unknown files unchanged");
    }
    const auto before=Read(index);
    const auto late=root/L"foreign-after-admission.bin";
    const auto target=Entry(root,2);
    {
        cache::WriteReservation token(target,MiB);Check(bool(token),"reserve a second payload below the limit");
        Temporary temp(target);
        Check(token.TrackTemporary(temp.path),"track the second real payload temporary");
        Fill(temp.file.value,MiB,true);temp.Publish(target);
        PublicationProbe probe{Canonical(sentinel),index.native()+L".tmp-",late.native()};
        bool committed;
        {ObservePublication observe(probe);committed=token.Commit();}
        Check(probe.injected&&!committed&&probe.observations==2,
            "late foreign allocation is observed by the post-write scan and blocks index publication");
        Check(Read(index)==before&&Charge(late)==Limit&&Exists(target),
            "late quota refusal preserves the committed index, unknown file and pending payload");
        for(const auto& entry:fs::directory_iterator(index.parent_path()))
            Check(entry.path()==index,"quota refusal retires only its own staged index");
    }
    Check(MoveFileExW(late.c_str(),(root.parent_path()/L"late-file.evidence").c_str(),0),
        "move the owned injected file outside this cache to allow recovery");
    const auto recovered=cache::Inspect();
    Check(recovered.error.empty()&&recovered.textureBytes==2*MiB&&Read(index)!=before&&
          Read(sentinel)==sentinelBytes&&Charge(root.parent_path()/L"late-file.evidence")==Limit,
        "recover the pending publication without disturbing the unknown file or prior payload");
}
std::wstring MutexName(const fs::path& root){auto path=fs::absolute(root).lexically_normal().native();std::wstring lower(path.size(),L'\0');
    Check(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,path.data(),int(path.size()),lower.data(),int(lower.size()),nullptr,nullptr,0)>0,"canonicalize only owned mutex root");
    std::uint64_t hash=1469598103934665603ull;for(auto c:lower){hash^=c;hash*=1099511628211ull;}return L"Local\\MMS-cache-"+std::to_wstring(hash);}
void ProbeMutex(const std::wstring& name){wchar_t exe[32768]{};const auto n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"resolve own mutex-check executable");
    auto command=L"\""+std::wstring(exe,n)+L"\" --mutex-check \""+name+L"\"";STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"launch independent current-test mutex observer");Handle child(process.hProcess),thread(process.hThread);
    const auto wait=WaitForSingleObject(child.value,10000);if(wait!=WAIT_OBJECT_0){TerminateProcess(child.value,239);WaitForSingleObject(child.value,1000);}DWORD code=239;GetExitCodeProcess(child.value,&code);
    Check(wait==WAIT_OBJECT_0&&code==0,"failed Commit releases publication mutex after owner destruction");}
void FailedIndex(const fs::path& output){const auto root=Case(output,L"failed-index");Project(root);const auto target=Entry(root,1),index=root/L"Management"/L"index.bin";const auto before=Read(index);const auto mutexName=MutexName(root);Handle observer(CreateMutexW(nullptr,FALSE,mutexName.c_str()));Check(observer.value,"retain named mutex for independent release probe");
    auto token=std::make_unique<cache::WriteReservation>(target,20*MiB);Check(bool(*token),"reserve publication whose index save will fail");Temporary temp(target);Check(token->TrackTemporary(temp.path),"track failing publication payload");Fill(temp.file.value,20*MiB,true);temp.Publish(target);
    Fault fault;fault.index=index.native();fault.blockMove=true;fault.blockDelete=true;bool committed;{Arm arm(fault);committed=token->Commit();}
    Check(!committed&&fault.moves&&fault.deletes&&fault.failedTemp[0]&&Exists(fault.failedTemp)&&Read(index)==before,"failed index rename and cleanup keep old index and real unpaid temp");
    const fs::path failedTemp=fault.failedTemp;const auto pendingTempCharge=Charge(failedTemp);Check(pendingTempCharge>0,"failed index temp has actual observed allocation");
    {Arm arm(fault);Check(!cache::WriteReservation(Entry(root,2),MiB),"unregistered final publication prevents silent new writer admission");}
    token.reset();ProbeMutex(mutexName);
    const auto state=cache::Inspect();Check(state.totalBytes>=20*MiB+pendingTempCharge&&Exists(failedTemp)&&Exists(target),"reconciliation retains final file and failed metadata temp in actual physical accounting");
    Check(!cache::WriteReservation(Entry(root,3),45*MiB),"reconciled registration never refunds still-present payload and temp");
    {cache::WriteReservation safe(Entry(root,4),40*MiB);Check(bool(safe),"successful retry removes provisional double charge but preserves real unknown temp");}
    Check(Exists(failedTemp),"unknown failed metadata temp remains outside cleanup authority");
}
bool StoredPin(const Bytes& bytes){auto u32=[&](std::size_t at){Check(at+4<=bytes.size(),"independent index field bound");std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(bytes[at+i])<<(8*i);return value;};
    Check(u32(0)==0x434d534d&&u32(4)==1&&u32(8)==1,"independent fixture index has exactly one project");const auto length=u32(12);Check(length<=32768&&24+std::size_t(length)*2<bytes.size(),"independent serialized pin offset is bounded");
    const auto pin=bytes[24+std::size_t(length)*2];Check(pin<=1,"independent persisted pin byte is Boolean");return pin!=0;}
void PublishedPinReadFailure(const fs::path& output){const auto root=Case(output,L"published-pin-read-failure");Project(root);const auto project=root.parent_path()/L"owned-project.pmm",index=root/L"Management"/L"index.bin";
    Check(!StoredPin(Read(index)),"fixture begins with independently verified unpinned index");Fault fault;fault.index=index.native();fault.blockedMetadata=Canonical(index);fault.failMetadataAfterMove=true;
    bool pinned;{Arm arm(fault);pinned=cache::Pin(project.native(),true);}
    Check(fault.moves==1&&fault.metadata==1&&!fault.metadataAfterMove,"fault happens once only after actual successful index rename");
    const auto published=Read(index);Check(pinned&&StoredPin(published),"Pin confirms already-published desired state by actual index reread after metadata scan failure");
    const auto view=cache::Inspect();Check(view.error.empty()&&view.projects.size()==1&&view.projects[0].pinned&&Read(index)==published,"next rescan keeps new live pin and never writes old pin over successful publication");
    Check(bool(cache::WriteReservation(Entry(root,1),MiB))&&Read(index)==published,"admission resumes after successful inventory recovery without rewriting pin backwards");
}
BY_HANDLE_FILE_INFORMATION PhysicalIdentity(const fs::path& path){Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
    Check(file.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(file.value,&info),"read real fixture file identity without modifying its content");return info;}
bool SamePhysicalFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow;}
void CorruptIndexAuthority(const fs::path& output){const Bytes corrupt{0x42,0x41,0x44,0x21};
    {const auto root=Case(output,L"replaced-corrupt-index");Store(root,1,MiB);const auto payload=Entry(root,1),index=root/L"Management"/L"index.bin",saved=root/L"Management"/L"previous-index.evidence";
        const auto original=Read(index);const auto identity=PhysicalIdentity(index),payloadIdentity=PhysicalIdentity(payload);const auto payloadBefore=Read(payload);
        Check(MoveFileExW(index.c_str(),saved.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"retain verified index under another name before external replacement");WriteSmall(index,corrupt);
        Check(!SamePhysicalFile(identity,PhysicalIdentity(index))&&SamePhysicalFile(identity,PhysicalIdentity(saved)),"CREATE_NEW replacement is a different physical file while old verified identity still exists");
        const auto result=cache::Clean(cache::Cleanup::All);
        Check(result.files==0&&!result.error.empty(),"ClearAll refuses to reconstruct an externally replaced corrupt index using old verified authority");
        Check(Read(index)==corrupt&&Read(saved)==original&&Read(payload)==payloadBefore&&SamePhysicalFile(payloadIdentity,PhysicalIdentity(payload)),"refused ClearAll preserves foreign bad index, prior index evidence and exact managed payload identity and bytes");
        const auto view=cache::Inspect();Check(!view.error.empty()&&Read(index)==corrupt&&Read(payload)==payloadBefore,"Inspect cannot convert rejected replacement into repair or cleanup authority");
    }
    {const auto base=output/L"never-verified-corrupt-index";NewDirectory(base);const auto root=base/L"Cache",index=root/L"Management"/L"index.bin",payload=Entry(root,1);
        // Do not Configure/Project/Store this root: its first index observation
        // must be corrupt, with no successful process-local publication receipt.
        WriteSmall(index,corrupt);Sparse(payload,MiB,true);const auto indexIdentity=PhysicalIdentity(index),payloadIdentity=PhysicalIdentity(payload);const auto payloadBefore=Read(payload);Select(root);
        const auto result=cache::Clean(cache::Cleanup::All);
        Check(result.files==0&&!result.error.empty(),"ClearAll refuses first-seen corrupt index without any verified process-local identity");
        Check(Read(index)==corrupt&&SamePhysicalFile(indexIdentity,PhysicalIdentity(index))&&Read(payload)==payloadBefore&&SamePhysicalFile(payloadIdentity,PhysicalIdentity(payload)),"first-seen corrupt index and unknown valid-header payload remain unchanged after refusal");
    }
}
void UnknownDeletePending(const fs::path& output){const auto root=Case(output,L"unknown-delete-pending");Project(root);const auto unknown=root/L"unknown-pending.bin";Sparse(unknown,40*MiB);
    const auto before=cache::Inspect();Check(before.error.empty()&&before.totalBytes>=40*MiB,"unknown file is observed and charged before deletion without being registered");
    Handle reader(CreateFileW(unknown.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(reader.value!=INVALID_HANDLE_VALUE,"retain real read handle permitting producer deletion");
    Check(DeleteFileW(unknown.c_str())!=FALSE,"producer marks only its own unknown fixture for deletion");FILE_STANDARD_INFO held{};
    Check(GetFileInformationByHandleEx(reader.value,FileStandardInfo,&held,sizeof held)&&held.DeletePending&&held.EndOfFile.QuadPart==40*MiB,"real reader still owns full delete-pending stream");
    WIN32_FIND_DATAW data{};const auto find=FindFirstFileW(unknown.c_str(),&data);const auto findError=GetLastError();if(find!=INVALID_HANDLE_VALUE)FindClose(find);
    Check(find==INVALID_HANDLE_VALUE&&(findError==ERROR_FILE_NOT_FOUND||findError==ERROR_PATH_NOT_FOUND),"actual directory enumeration omits delete-pending unknown name");
    Fault fault;fault.blockedMissing=unknown.native();{Arm arm(fault);const auto blocked=cache::Inspect();
        Check(fault.missing>0&&!blocked.error.empty()&&blocked.totalBytes>=before.totalBytes,"uncertain disappearance preserves last observed unknown charge and reports error");
        Check(!cache::WriteReservation(Entry(root,1),MiB),"unconfirmed unknown disappearance denies admission even when requested bytes would fit");
        reader.Close();const auto stillUnconfirmed=cache::Inspect();Check(!stillUnconfirmed.error.empty()&&stillUnconfirmed.totalBytes>=before.totalBytes,"last reader release alone cannot refund while explicit absence confirmation still fails");}
    const auto recovered=cache::Inspect();Check(recovered.error.empty()&&recovered.totalBytes<=before.totalBytes-40*MiB&&!Exists(unknown),"verified absence after reader release and fault removal finally discharges unknown stream");
    Check(bool(cache::WriteReservation(Entry(root,2),40*MiB)),"writer admission resumes after complete observed-deletion recovery");
}
void ScanFailure(const fs::path& output){const auto root=Case(output,L"scan-failure");Project(root);const auto file=root/L"unreadable-observation.bin";Sparse(file,MiB);const auto index=root/L"Management"/L"index.bin";const auto bytes=Read(index);
    Fault fault;fault.blockedMetadata=Canonical(file);fault.blockMetadata=true;bool admitted;{Arm arm(fault);cache::WriteReservation token(Entry(root,1),MiB);admitted=bool(token);}
    Check(!admitted&&fault.metadata&&Exists(file)&&Read(index)==bytes,"metadata access failure refuses admission without deleting unknown files or modifying index");
    Check(bool(cache::WriteReservation(Entry(root,2),MiB)),"admission can resume after an actual metadata-access fault is removed");
    const auto deepRoot=Case(output,L"depth-limit");fs::path current=deepRoot;
    for(unsigned i=0;i<65;++i){current/=L"d";const auto extended=L"\\\\?\\"+current.native();Check(CreateDirectoryW(extended.c_str(),nullptr)!=FALSE,"create bounded own deep tree using explicit long path");}
    Check(!cache::WriteReservation(Entry(deepRoot,1),MiB),"unscannable depth cannot be treated as zero occupation");
}
void ConfigureAfterScanFailure(const fs::path& output) {
    const auto root = Case(output, L"apply-blocked-inventory");
    Project(root);
    const auto settingsPath = root.parent_path() / L"settings.json";
    const auto index = root / L"Management" / L"index.bin";
    const auto foreign = root / L"external.bin";
    const Bytes contents{1, 2, 3};
    WriteSmall(foreign, contents);
    const auto previousSettings = Read(settingsPath);
    const auto previousIndex = Read(index);
    auto desired = cache::Current();
    desired.limitBytes = 128 * MiB;
    desired.recentProjects = 17;
    Fault fault;
    fault.blockedMetadata = Canonical(foreign);
    fault.blockMetadata = true;
    std::wstring error;
    {
        Arm arm(fault);
        Check(!cache::Configure(desired, error) && !error.empty() && fault.metadata,
              "refuse Apply when required disk accounting cannot read a file");
    }
    Check(Read(settingsPath) == previousSettings && Read(index) == previousIndex &&
          cache::Current().limitBytes == Limit && Read(foreign) == contents,
          "failed Apply keeps saved settings, the index and the external file unchanged");
    Check(cache::Configure(desired, error) && error.empty(), "retry Apply after metadata access recovers");
    const auto updated = cache::Inspect();
    Check(updated.error.empty() && updated.settings.limitBytes == desired.limitBytes &&
          updated.settings.recentProjects == desired.recentProjects && Read(settingsPath) != previousSettings &&
          Read(foreign) == contents, "retry persists both edited settings and preserves foreign data");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc==3&&std::wstring(argv[1])==L"--mutex-check"){Handle mutex(OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,argv[2]));if(!mutex.value)return 3;
        const auto wait=WaitForSingleObject(mutex.value,1500);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)return 4;ReleaseMutex(mutex.value);return 0;}
    try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_manager_inventory_tests --store NEW_ABS_DIR");const fs::path root(argv[2]);NewDirectory(root);Hooks hooks;
        ConfigureAfterScanFailure(root);
        Unknowns(root);Hardlinks(root);Peak(root);Tracked(root);FailedTemporaryCleanup(root);TrackingFailurePreservesOwner(root);UntrackedCannotAdopt(root);MetadataPeakAndUnique(root);PublicationInventoryBoundary(root);FailedIndex(root);PublishedPinReadFailure(root);CorruptIndexAuthority(root);UnknownDeletePending(root);ScanFailure(root);
        cache::Shutdown();std::printf("PASS cache manager inventory checks=%u\n",checks);return 0;
    }catch(const std::exception& e){active=nullptr;std::fprintf(stderr,"FAIL cache manager inventory after %u checks: %s\n",checks,e.what());return 1;}
}
