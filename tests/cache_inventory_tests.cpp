// MAINTENANCE: regression-keep. Bounded metadata observation of owned real files.
// An inventory is not a disk quota approval or an immutable file-data snapshot.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <MinHook.h>
#include "mikumikustudio/cache_files.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
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
namespace f=mikumikustudio::cache_files;
namespace fs=std::filesystem;
using R=f::Result;
using Bytes=std::vector<std::uint8_t>;
unsigned checks=0;
void Check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
struct Handle {HANDLE value;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}Handle(const Handle&)=delete;};
void NewDirectory(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"normalized absolute isolated directory");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim fresh owned fixture directory");}
f::Directory Open(const fs::path& p){f::Directory d;Check(f::Directory::Open(p,false,d)==R::Ok,"open existing protected fixture directory");return d;}
void Write(const fs::path& p,const Bytes& bytes){Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(h.value!=INVALID_HANDLE_VALUE,"create owned ordinary file");DWORD done=0;
    Check(WriteFile(h.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(h.value),"write exact owned bytes");}
std::wstring Canonical(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    wchar_t value[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,value,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"resolve fixture volume GUID identity path");return std::wstring(value,n);}
f::FileObservation Query(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION basic{};FILE_STANDARD_INFO standard{};Check(h.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.value,&basic)&&GetFileInformationByHandleEx(h.value,FileStandardInfo,&standard,sizeof standard),"independent Win32 observation oracle");
    Check(standard.EndOfFile.QuadPart>=0&&standard.AllocationSize.QuadPart>=0,"nonnegative native logical and allocation sizes");f::FileObservation out;
    out.identity={basic.dwVolumeSerialNumber,(std::uint64_t(basic.nFileIndexHigh)<<32)|basic.nFileIndexLow,std::uint64_t(standard.EndOfFile.QuadPart)};
    out.allocationBytes=std::uint64_t(standard.AllocationSize.QuadPart);out.attributes=basic.dwFileAttributes;out.links=basic.nNumberOfLinks;out.kind=(basic.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?f::ObservationKind::Directory:f::ObservationKind::File;out.deletePending=standard.DeletePending!=FALSE;
    DWORD volumeFlags=0;Check(GetVolumeInformationByHandleW(h.value,nullptr,0,nullptr,nullptr,&volumeFlags,nullptr,0)!=FALSE,"independent same-handle volume stream capability query");out.namedStreamsSupported=(volumeFlags&FILE_NAMED_STREAMS)!=0;
    if(out.namedStreamsSupported){Bytes streamBuffer(1024*1024);SetLastError(ERROR_SUCCESS);const bool found=GetFileInformationByHandleEx(h.value,FileStreamInfo,streamBuffer.data(),DWORD(streamBuffer.size()))!=FALSE;const auto error=GetLastError();
        // FileStreamInfo can succeed with ERROR_HANDLE_EOF for no streams.
        // Do not reinterpret its zero buffer as a fabricated default stream.
        const bool emptyBuffer=std::all_of(streamBuffer.begin(),streamBuffer.end(),[](auto value){return value==0;});
        const bool noStreams=(!found&&(error==ERROR_HANDLE_EOF||error==ERROR_NO_MORE_FILES))||(found&&error==ERROR_HANDLE_EOF&&emptyBuffer);
        Check((found&&!noStreams)||(noStreams&&(out.kind==f::ObservationKind::Directory||(!out.identity.fileBytes&&!out.allocationBytes))),"independent real stream enumeration succeeds or confirms an empty stream list");
        if(found&&!noStreams){std::size_t at=0;for(;;){Check(at+24<=streamBuffer.size(),"stream oracle record bound");const auto* stream=reinterpret_cast<const FILE_STREAM_INFO*>(streamBuffer.data()+at);
            Check(stream->StreamSize.QuadPart>=0&&stream->StreamAllocationSize.QuadPart>=0&&at+24+stream->StreamNameLength<=streamBuffer.size(),"stream oracle sizes and name remain in real query buffer");
            ++out.streamCount;out.streamLogicalBytes+=std::uint64_t(stream->StreamSize.QuadPart);out.streamAllocationBytes+=std::uint64_t(stream->StreamAllocationSize.QuadPart);
            if(!stream->NextEntryOffset)break;Check(stream->NextEntryOffset>=24&&stream->NextEntryOffset<=streamBuffer.size()-at,"stream oracle progresses within buffer");at+=stream->NextEntryOffset;}}
    }else if(out.kind==f::ObservationKind::File){out.streamCount=1;out.streamLogicalBytes=out.identity.fileBytes;out.streamAllocationBytes=out.allocationBytes;}
    return out;}
bool Same(const f::FileObservation& a,const f::FileObservation& b){return a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes&&a.allocationBytes==b.allocationBytes&&a.attributes==b.attributes&&a.links==b.links&&a.kind==b.kind&&a.deletePending==b.deletePending&&a.streamLogicalBytes==b.streamLogicalBytes&&a.streamAllocationBytes==b.streamAllocationBytes&&a.streamCount==b.streamCount&&a.namedStreamsSupported==b.namedStreamsSupported;}
struct Saved {bool valid;f::DirectoryIdentity root;std::vector<f::InventoryEntry> entries;explicit Saved(const f::InventorySnapshot& s):valid(s.IsValid()),root(s.RootIdentity()),entries(s.Entries()){}
    void CheckSame(const f::InventorySnapshot& s)const{Check(valid==s.IsValid()&&root.volume==s.RootIdentity().volume&&root.fileIndex==s.RootIdentity().fileIndex&&entries.size()==s.Entries().size(),"failed inventory preserves validity, root and entry count");
        for(std::size_t i=0;i<entries.size();++i)Check(entries[i].relativePath==s.Entries()[i].relativePath&&entries[i].depth==s.Entries()[i].depth&&Same(entries[i].observed,s.Entries()[i].observed),"failed inventory preserves every prior path and observed field");}};
const f::InventoryEntry& Find(const f::InventorySnapshot& s,const wchar_t* path){const auto it=std::find_if(s.Entries().begin(),s.Entries().end(),[&](const auto& entry){return entry.relativePath==path;});Check(it!=s.Entries().end(),"inventory contains expected unknown path");return *it;}

enum class Mode {None,Access,Sharing,Information,Streams,NoBody,PartialWrite,CancelCopy,Reopen,Receipt};
struct Fault {Mode mode;std::wstring path,prefix;unsigned hits=0,secondary=0,opens=0;HANDLE created=INVALID_HANDLE_VALUE;std::atomic_bool* cancelled=nullptr;
    bool Matches(HANDLE h)const noexcept {wchar_t value[32768]{};const auto n=GetFinalPathNameByHandleW(h,value,32768,VOLUME_NAME_GUID);return n&&n<32768&&path.size()==n&&!_wcsicmp(value,path.c_str());}};
thread_local Fault* fault=nullptr;
struct Mutation {std::wstring observedPath,changedPath;bool grow=false,stream=false;HANDLE go=nullptr,done=nullptr;unsigned triggers=0;std::atomic<DWORD> error{ERROR_SUCCESS};bool changed=false;};
thread_local Mutation* mutation=nullptr;
decltype(&CreateFileW) realCreate=nullptr;
decltype(&GetFileInformationByHandle) realInformation=nullptr;
decltype(&GetFileInformationByHandleEx) realInformationEx=nullptr;
decltype(&SetFileInformationByHandle) realSetInformation=nullptr;
decltype(&WriteFile) realWrite=nullptr;
decltype(&ReadFile) realRead=nullptr;
void Trigger(Mutation& m)noexcept {++m.triggers;if(!SetEvent(m.go)||WaitForSingleObject(m.done,10000)!=WAIT_OBJECT_0)m.error=ERROR_TIMEOUT;}
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE source)noexcept {
    auto* f=fault;
    const bool match=f&&path&&!f->path.empty()&&!_wcsicmp(path,f->path.c_str());
    if(match){++f->opens;if((f->mode==Mode::Access||f->mode==Mode::Sharing)&&access==FILE_READ_ATTRIBUTES&&creation==OPEN_EXISTING){++f->hits;SetLastError(f->mode==Mode::Access?ERROR_ACCESS_DENIED:ERROR_SHARING_VIOLATION);return INVALID_HANDLE_VALUE;}
        if(f->mode==Mode::Reopen&&access==GENERIC_READ&&creation==OPEN_EXISTING){++f->hits;SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}}
    const auto result=realCreate(path,access,sharing,security,creation,flags,source);const auto error=GetLastError();
    if(result!=INVALID_HANDLE_VALUE&&f&&f->mode==Mode::Receipt&&creation==CREATE_NEW&&path&&!f->prefix.empty()&&!_wcsnicmp(path,f->prefix.c_str(),f->prefix.size()))f->created=result;
    auto* m=mutation;if(result!=INVALID_HANDLE_VALUE&&m&&!m->grow&&!m->triggers&&path&&access==FILE_READ_ATTRIBUTES&&creation==OPEN_EXISTING&&!_wcsicmp(path,m->observedPath.c_str()))Trigger(*m);
    SetLastError(error);return result;
}
BOOL WINAPI InformationHook(HANDLE h,LPBY_HANDLE_FILE_INFORMATION info)noexcept {auto* f=fault;
    if(f&&((f->mode==Mode::Receipt&&h==f->created)||(f->mode==Mode::Information&&f->Matches(h)))){++f->hits;SetLastError(ERROR_READ_FAULT);return FALSE;}
    const auto result=realInformation(h,info);const auto error=GetLastError();auto* m=mutation;
    if(result&&m&&m->grow&&!m->stream&&!m->triggers){wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);if(n&&n<32768&&!_wcsicmp(path,m->observedPath.c_str()))Trigger(*m);}
    SetLastError(error);return result;
}
BOOL WINAPI InformationExHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)noexcept {auto* f=fault;
    if(f&&f->mode==Mode::Streams&&kind==FileStreamInfo&&f->Matches(h)){++f->hits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    const auto result=realInformationEx(h,kind,data,size);const auto error=GetLastError();auto* m=mutation;
    if(result&&kind==FileStreamInfo&&m&&m->stream&&!m->triggers){wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);if(n&&n<32768&&!_wcsicmp(path,m->observedPath.c_str()))Trigger(*m);}
    SetLastError(error);return result;}
BOOL WINAPI SetInformationHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)noexcept {auto* f=fault;
    if(f&&f->mode==Mode::Receipt&&h==f->created&&kind==FileDispositionInfo){++f->secondary;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return realSetInformation(h,kind,data,size);}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept {auto* f=fault;
    if(f&&f->mode==Mode::PartialWrite&&!f->hits&&!overlap&&f->Matches(h)){LARGE_INTEGER zero{},position{};
        if(SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128&&count>1){const auto ok=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(ok){++f->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return ok;}}
    const auto result=realWrite(h,bytes,count,done,overlap);const auto error=GetLastError();
    if(result&&f&&f->mode==Mode::CancelCopy&&!f->hits&&f->cancelled&&f->Matches(h)){++f->hits;f->cancelled->store(true);}SetLastError(error);return result;}
BOOL WINAPI ReadHook(HANDLE h,LPVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept {auto* f=fault;
    if(f&&f->mode==Mode::NoBody&&f->Matches(h)){++f->hits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return realRead(h,bytes,count,done,overlap);}
struct Hooks {Hooks(){Check(MH_Initialize()==MH_OK,"initialize current-process observation fault hooks");
    Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"hook precise metadata open failures");
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetFileInformationByHandle),reinterpret_cast<void*>(&InformationHook),reinterpret_cast<void**>(&realInformation))==MH_OK,"hook precise same-handle information boundary");
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetFileInformationByHandleEx),reinterpret_cast<void*>(&InformationExHook),reinterpret_cast<void**>(&realInformationEx))==MH_OK,"hook exact named-stream observation failure and change boundary");
    Check(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&SetInformationHook),reinterpret_cast<void**>(&realSetInformation))==MH_OK,"hook exact creation receipt deletion failure");
    Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK,"hook real partial payload writes");
    Check(MH_CreateHook(reinterpret_cast<void*>(&ReadFile),reinterpret_cast<void*>(&ReadHook),reinterpret_cast<void**>(&realRead))==MH_OK,"observe absence of payload reads during same-handle metadata query");Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable test-local observation hooks");}
    ~Hooks(){fault=nullptr;mutation=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm {explicit Arm(Fault& f){Check(!fault,"nonrecursive file fault");fault=&f;}~Arm(){fault=nullptr;}};

void Sparse(const fs::path& p){Handle h(CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;
    Check(h.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(h.value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&done,nullptr),"create actual sparse NTFS fixture");LARGE_INTEGER pos{};pos.QuadPart=8*1024*1024;
    Check(SetFilePointerEx(h.value,pos,nullptr,FILE_BEGIN)&&SetEndOfFile(h.value),"set sparse logical EOF without filling holes");pos.QuadPart=3*1024*1024;const std::uint32_t value=0x12345678;
    Check(SetFilePointerEx(h.value,pos,nullptr,FILE_BEGIN)&&WriteFile(h.value,&value,sizeof value,&done,nullptr)&&done==sizeof value&&FlushFileBuffers(h.value),"allocate one real sparse extent");}
void Tree(const fs::path& base){const auto tree=base/L"tree";NewDirectory(tree);NewDirectory(tree/L"nested");NewDirectory(tree/L"nested"/L"deeper");
    Write(tree/L"unknown.dat",Bytes{1,2,3});Write(tree/L"nested"/L"settings.json",Bytes{4,5});Write(tree/L"nested"/L"deeper"/L"misc.bin",Bytes{6});Write(tree/L"empty",{});Write(tree/L"杂项.txt",Bytes{7,8,9,10});Sparse(tree/L"sparse.data");
    Write(fs::path((tree/L"unknown.dat").native()+L":extra"),Bytes{1,2,3,4,5});Write(fs::path((tree/L"nested").native()+L":directory-extra"),Bytes{6,7,8,9});
    Write(fs::path(tree.native()+L":root-extra"),Bytes{11,12});Write(tree/L".lock",{});Write(fs::path((tree/L".lock").native()+L":lock-extra"),Bytes{13,14,15});
    Check(CreateHardLinkW((tree/L"nested"/L"alias.dat").c_str(),(tree/L"unknown.dat").c_str(),nullptr)!=FALSE,"create real hardlink names before ancestor guards exist");
    auto directory=Open(tree);f::FileLock lock;Check(directory.Lock(0,lock)==R::Ok,"create actual active .lock for inventory");f::FileObservation lockInfo;Check(lock.Observe(lockInfo)==R::Ok&&Same(lockInfo,Query(tree/L".lock")),"same-handle lock observation equals independent native metadata");
    f::InventorySnapshot snapshot;Check(directory.Inventory({},snapshot)==R::Ok&&snapshot.IsValid(),"bounded inventory includes foreign nested entries and live .lock");
    Check(snapshot.Entries().size()==11,"root, two directories, eight file names are all counted");std::size_t nameUnits=0;unsigned directories=0,depth=0;
    for(const auto& entry:snapshot.Entries()){nameUnits+=entry.relativePath.size();depth=(std::max)(depth,entry.depth);if(entry.observed.kind==f::ObservationKind::Directory)++directories;
        Check(Same(entry.observed,Query(entry.relativePath.empty()?tree:tree/entry.relativePath)),"each inventory entry equals independent Win32 identity/logical/allocation/attributes/links observation");}
    const auto& root=Find(snapshot,L"");Check(root.depth==0&&root.observed.kind==f::ObservationKind::Directory&&root.observed.identity.volume==snapshot.RootIdentity().volume&&root.observed.identity.fileIndex==snapshot.RootIdentity().fileIndex,"root is depth zero and consumes a real entry/guard");
    const auto& first=Find(snapshot,L"unknown.dat").observed;const auto& alias=Find(snapshot,L"nested\\alias.dat").observed;
    Check(first.links==2&&alias.links==2&&first.identity.volume==alias.identity.volume&&first.identity.fileIndex==alias.identity.fileIndex&&first.identity.fileBytes==3,"hardlinks remain separate names with the same physical identity, not silently dropped");
    Check(first.namedStreamsSupported&&first.streamCount==2&&first.streamLogicalBytes==8&&Same(first,alias),"file ADS are counted for each hardlink observation without confusing default bytes");
    Check(root.observed.namedStreamsSupported&&root.observed.streamLogicalBytes==2&&Find(snapshot,L"nested").observed.streamLogicalBytes==4,"root and nested directory ADS remain part of observed responsibility");
    Check(Find(snapshot,L".lock").observed.streamLogicalBytes==3,"same .lock identity includes actual named-stream bytes");
    const auto& sparse=Find(snapshot,L"sparse.data").observed;Check(sparse.identity.fileBytes==8*1024*1024&&sparse.allocationBytes<sparse.identity.fileBytes&&(sparse.attributes&FILE_ATTRIBUTE_SPARSE_FILE),"sparse allocation is distinguished from logical length using actual filesystem metadata");
    f::InventoryLimits exact;exact.maximumEntries=snapshot.Entries().size();exact.maximumDepth=depth;exact.maximumNameCodeUnits=nameUnits;exact.maximumDirectoryGuards=directories;
    Check(directory.Inventory(exact,snapshot)==R::Ok,"exact observed entry/depth/path-unit/guard bounds succeed");Saved saved(snapshot);
    for(unsigned field=0;field<4;++field){auto limit=exact;if(field==0)--limit.maximumEntries;if(field==1)--limit.maximumDepth;if(field==2)--limit.maximumNameCodeUnits;if(field==3)--limit.maximumDirectoryGuards;
        Check(directory.Inventory(limit,snapshot)==R::Limit,"one-too-small exact resource bound refuses complete inventory");saved.CheckSame(snapshot);}
    for(unsigned field=0;field<6;++field){auto invalid=exact;if(field==0)invalid.maximumEntries=0;if(field==1)invalid.maximumDirectoryGuards=0;if(field==2)invalid.maximumEntries=65537;if(field==3)invalid.maximumDepth=65;if(field==4)invalid.maximumNameCodeUnits=16*1024*1024+1;if(field==5)invalid.maximumDirectoryGuards=4097;
        Check(directory.Inventory(invalid,snapshot)==R::Invalid,"invalid hard-cap parameters rejected");saved.CheckSame(snapshot);}
    for(unsigned field=0;field<6;++field){auto limit=exact;if(field==0)limit.observation.maximumStreams=1;if(field==1)limit.observation.maximumStreamBufferBytes=32;
        if(field==2)limit.observation.maximumStreams=0;if(field==3)limit.observation.maximumStreams=4097;if(field==4)limit.observation.maximumStreamBufferBytes=31;if(field==5)limit.observation.maximumStreamBufferBytes=4*1024*1024+1;
        Check(directory.Inventory(limit,snapshot)==(field<2?R::Limit:R::Invalid),"named-stream count/buffer bounds cannot silently truncate inventory");saved.CheckSame(snapshot);}
    std::atomic_bool cancelled{true};Check(directory.Inventory(exact,snapshot,&cancelled)==R::Cancelled,"pre-cancelled inventory refuses without replacing output");saved.CheckSame(snapshot);
    // Success retains directory guards, not file-data read pins. Later writes
    // are allowed and do not mutate the already returned metadata value.
    {Handle file(CreateFileW((tree/L"unknown.dat").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));DWORD wrote=0;const unsigned char b=11;
        Check(file.value!=INVALID_HANDLE_VALUE&&WriteFile(file.value,&b,1,&wrote,nullptr)&&wrote==1,"successful inventory does not freeze file data");}
    Check(Find(snapshot,L"unknown.dat").observed.identity.fileBytes==3,"old metadata snapshot is not silently refreshed by later file write");
    lock={};directory={};const auto renamed=base/L"tree-renamed";
    Check(!MoveFileExW(tree.c_str(),renamed.c_str(),0)&&GetLastError()==ERROR_SHARING_VIOLATION,"inventory alone retains root directory guard");
    const auto movedChild=tree/L"child-renamed";Check(!MoveFileExW((tree/L"nested").c_str(),movedChild.c_str(),0)&&GetLastError()==ERROR_SHARING_VIOLATION,"inventory alone retains traversed child directory guards");
    f::InventorySnapshot moved(std::move(snapshot));Check(moved.IsValid()&&!snapshot.IsValid(),"snapshot move transfers guards and invalidates source");moved={};
    Check(MoveFileExW(tree.c_str(),renamed.c_str(),0)&&MoveFileExW(renamed.c_str(),tree.c_str(),0),"last snapshot release relinquishes retained directory guards");
    const auto empty=base/L"empty-root";NewDirectory(empty);auto d=Open(empty);f::InventoryLimits only;only.maximumEntries=1;only.maximumDirectoryGuards=1;only.maximumDepth=0;only.maximumNameCodeUnits=0;
    Check(d.Inventory(only,snapshot)==R::Ok&&snapshot.Entries().size()==1&&snapshot.Entries()[0].relativePath.empty(),"zero depth and path budget permit only root");
    Check(snapshot.Entries()[0].observed.streamCount==0&&snapshot.Entries()[0].observed.streamLogicalBytes==0&&snapshot.Entries()[0].observed.streamAllocationBytes==0,"empty directory has no invented default data stream");
}

template<class T>void ObserveFailure(T& object,const fs::path& path){auto original=Query(path),output=original;output.allocationBytes+=123;const auto before=output;
    {Fault metadata{Mode::NoBody,Canonical(path)};auto actual=before;R result;{Arm arm(metadata);result=object.Observe(actual);}
        Check(result==R::Ok&&Same(actual,original)&&!metadata.hits&&!metadata.opens,"same-handle observation neither reads payload nor reopens the file path");}
    for(auto mode:{Mode::Information,Mode::Streams}){Fault f{mode,Canonical(path)};R result;
        {Arm arm(f);result=object.Observe(output);}Check(f.hits&&result==R::Io&&Same(output,before),"same-handle metadata/stream query failure preserves every output field");}
    f::ObservationLimits limits;limits.maximumStreams=0;Check(object.Observe(output,limits)==R::Invalid&&Same(output,before),"same-handle observation validates stream count limit before publication");
    limits.maximumStreams=128;limits.maximumStreamBufferBytes=31;Check(object.Observe(output,limits)==R::Invalid&&Same(output,before),"same-handle observation validates buffer limit before publication");}
void Handles(const fs::path& base){const auto root=base/L"handles";NewDirectory(root);auto d=Open(root);Bytes bytes(200000,0x5a);f::Spec spec;spec.owner[0]=1;spec.key[0]=2;
    f::PreparedWrite prepared;Check(d.Prepare(spec,bytes.data(),bytes.size(),prepared)==R::Ok,"create real exclusively held prepared envelope");const auto plan=prepared.Plan();const auto path=root/plan.name;
    f::FileObservation observed;Check(prepared.Observe(observed)==R::Ok&&observed.identity.fileBytes==0&&observed.identity.fileIndex==plan.createdIdentity.fileIndex,"same exclusive prepared handle observes current zero bytes");ObserveFailure(prepared,path);
    f::InventorySnapshot inventory;const auto whileExclusive=d.Inventory({},inventory);Check(whileExclusive==R::Ok||whileExclusive==R::Busy,"metadata-only inventory either observes own exclusive writer or fails closed with sharing error");
    if(whileExclusive==R::Busy)Check(!inventory.IsValid()&&inventory.Entries().empty(),"real sharing refusal preserves inert inventory output");
    if(whileExclusive==R::Ok)Check(Find(inventory,plan.name.c_str()).observed.identity.fileBytes==0,"metadata-only enumeration of own exclusive writer reports current bytes, not planned size");
    Fault partial{Mode::PartialWrite,Canonical(path)};f::BlobInfo output;R result;{Arm arm(partial);result=prepared.Complete(bytes.data(),bytes.size(),output);}
    Check(result==R::Io&&partial.hits==1&&prepared.IsValid(),"real partial envelope write retains exclusive preparation");Check(prepared.Observe(observed)==R::Ok&&observed.identity.fileBytes==145&&Same(observed,Query(path)),"same-handle observation reports actual partial envelope length/allocation");
    Check(prepared.Abort()==R::DeletePending,"explicitly abort owned incomplete envelope");const auto prior=observed;Check(prepared.Observe(observed)==R::Invalid&&Same(observed,prior),"consumed prepared handle refuses observation without modifying output");
    const auto source=base/L"source.avi";Write(source,bytes);f::RawSourceWrite raw;f::RawSourceCleanup cleanup;Check(d.PrepareRawSource(source.c_str(),bytes.size(),raw,cleanup)==R::Ok&&!cleanup.IsValid(),"prepare own raw writer");
    const auto rawPlan=raw.Plan();const auto rawPath=root/rawPlan.name;Check(raw.Observe(observed)==R::Ok&&observed.identity.fileBytes==0,"raw writer observation uses current handle length");ObserveFailure(raw,rawPath);
    std::atomic_bool cancelled{false};Fault copy{Mode::CancelCopy,Canonical(rawPath)};copy.cancelled=&cancelled;f::RawSourceLease lease;
    {Arm arm(copy);result=raw.Complete(lease,&cancelled);}Check(result==R::Cancelled&&copy.hits==1&&raw.IsValid()&&!lease.IsValid(),"actual raw copy interrupts after first written chunk");
    Check(raw.Observe(observed)==R::Ok&&observed.identity.fileBytes>0&&observed.identity.fileBytes<bytes.size()&&Same(observed,Query(rawPath)),"raw same-handle observation reports partial copied bytes");
    cancelled=false;Fault reopen{Mode::Reopen,Canonical(rawPath)};{Arm arm(reopen);result=raw.Complete(lease,&cancelled);}
    Check(result!=R::Ok&&reopen.hits==1&&raw.IsValid()&&!lease.IsValid(),"read-pin reopen failure leaves source plan but no writer handle");
    const auto before=observed;Fault noReopen{Mode::Reopen,Canonical(rawPath)};{Arm arm(noReopen);result=raw.Observe(observed);}
    Check(result==R::Busy&&noReopen.opens==0&&Same(observed,before),"Observe never silently reopens path after raw writer handle closed");
    Check(raw.Complete(lease)==R::Ok,"retry recovers own raw writer and completes source");const auto info=lease.Info();lease={};f::DirectoryIdentity identity;Check(d.GetIdentity(identity)==R::Ok&&d.RecoverRawSourceCleanup(identity,info,cleanup)==R::Ok,"obtain authorized actual same-handle cleanup receipt");
    Check(cleanup.Observe(observed)==R::Ok&&observed.identity.fileBytes==bytes.size()&&Same(observed,Query(rawPath)),"cleanup handle reports actual complete file size and allocation");ObserveFailure(cleanup,rawPath);
    Check(cleanup.Abort()==R::DeletePending,"explicitly remove owned completed source");const auto deleted=observed;Check(cleanup.Observe(observed)==R::Invalid&&Same(observed,deleted),"consumed cleanup receipt cannot observe another file");
    Fault receipt{Mode::Receipt};receipt.prefix=Canonical(root)+L'\\';{Arm arm(receipt);result=d.PrepareRawSource(source.c_str(),bytes.size(),raw,cleanup);}
    Check(result==R::CleanupRequired&&receipt.hits&&receipt.secondary&&cleanup.IsValid()&&!raw.IsValid(),"identity-plus-disposition failure returns exact original creation receipt");
    const auto receiptPath=root/cleanup.Name();Check(cleanup.Observe(observed)==R::Ok&&observed.identity.fileBytes==0&&Same(observed,Query(receiptPath)),"recovered metadata query observes original unknown-identity receipt handle, without adopting by name");
    Check(cleanup.Abort()==R::DeletePending,"explicitly release observed creation-failure receipt");
    f::FileLock lock;Check(d.Lock(0,lock)==R::Ok&&lock.Observe(observed)==R::Ok&&Same(observed,Query(root/L".lock")),"live byte lock reports actual .lock file metadata");ObserveFailure(lock,root/L".lock");lock={};
    const auto unlocked=observed;Check(lock.Observe(observed)==R::Invalid&&Same(observed,unlocked),"released lock refuses same-handle observation");
}

void Reparse(const fs::path& base){const auto tree=base/L"reparse-tree",outside=base/L"reparse-target";NewDirectory(tree);NewDirectory(outside);Write(outside/L"untouched.bin",Bytes{1,2});const auto link=tree/L"junction";NewDirectory(link);
    const std::wstring substitute=L"\\??\\"+outside.native(),print=outside.native();const auto subBytes=substitute.size()*2,printBytes=print.size()*2;Bytes reparse(16+subBytes+2+printBytes+2);
    auto put=[&](std::size_t offset,std::uint64_t value,unsigned n){for(unsigned i=0;i<n;++i)reparse[offset+i]=std::uint8_t(value>>(8*i));};
    put(0,IO_REPARSE_TAG_MOUNT_POINT,4);put(4,reparse.size()-8,2);put(10,subBytes,2);put(12,subBytes+2,2);put(14,printBytes,2);
    std::memcpy(reparse.data()+16,substitute.data(),subBytes);std::memcpy(reparse.data()+16+subBytes+2,print.data(),printBytes);
    {Handle h(CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));DWORD done=0;
        Check(h.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(h.value,FSCTL_SET_REPARSE_POINT,reparse.data(),DWORD(reparse.size()),nullptr,0,&done,nullptr),"create actual own junction before directory guards");}
    const auto sentinel=base/L"sentinel";NewDirectory(sentinel);Write(sentinel/L"retained.bin",Bytes{9});auto original=Open(sentinel);f::InventorySnapshot output;Check(original.Inventory({},output)==R::Ok,"initialize retained inventory sentinel");Saved saved(output);
    auto d=Open(tree);Check(d.Inventory({},output)==R::Invalid,"inventory rejects nested reparse entry instead of following target");saved.CheckSame(output);
    Check(GetFileAttributesW((outside/L"untouched.bin").c_str())!=INVALID_FILE_ATTRIBUTES,"outside junction target remains intact");
}
void RefusalsAndAllocation(const fs::path& base){const auto path=base/L"failures";NewDirectory(path);Write(path/L"ordinary.dat",Bytes{1,2,3});auto d=Open(path);f::InventorySnapshot output;
    Check(d.Inventory({},output)==R::Ok,"create successful inventory sentinel for bounded failures");Saved saved(output);
    f::Directory invalid;Check(invalid.Inventory({},output)==R::Invalid,"invalid directory cannot publish an inventory");saved.CheckSame(output);
    for(auto mode:{Mode::Access,Mode::Sharing,Mode::Information,Mode::Streams}){Fault f{mode,Canonical(path/L"ordinary.dat")};R result;{Arm arm(f);result=d.Inventory({},output);}
        Check(f.hits&&result==(mode==Mode::Sharing?R::Busy:R::Io),"actual metadata API refusal does not produce a partial successful inventory");saved.CheckSame(output);}
    bool success=false;unsigned failures=0;for(long n=0;n<1024;++n){R result;unsigned hits;{allocation::Fail fail(n);result=d.Inventory({},output);hits=allocation::hits;}
        if(result==R::Ok){Check(!hits,"successful inventory did not swallow injected allocation failure");success=true;break;}
        Check(hits&&result==R::Io,"actual allocation refusal is reported by file-layer contract");++failures;saved.CheckSame(output);}
    Check(success&&failures,"bounded inventory allocation sweep reaches both refusal and success");
    output={};d={};const auto renamed=base/L"failures-renamed";Check(MoveFileExW(path.c_str(),renamed.c_str(),0)&&MoveFileExW(renamed.c_str(),path.c_str(),0),"failed and successful inventory candidates release all directory guards after final owner drops");
}
void ConcurrentChanges(const fs::path& base){for(unsigned mode=0;mode<3;++mode){const bool grow=mode!=0;const auto path=base/(mode==2?L"concurrent-stream":grow?L"concurrent-growth":L"concurrent-name");NewDirectory(path);const auto original=path/L"first.dat";Write(original,Bytes{1,2,3});if(mode==2)Write(fs::path(original.native()+L":side"),Bytes{1,2,3,4});auto d=Open(path);f::InventorySnapshot output;
    Check(d.Inventory({},output)==R::Ok,"capture stable baseline before actual concurrent mutation");Saved saved(output);
    Mutation m;m.observedPath=Canonical(original);m.changedPath=mode==2?original.native()+L":side":grow?original.native():(path/L"new-name.dat").native();m.grow=grow;m.stream=mode==2;
    Handle go(CreateEventW(nullptr,TRUE,FALSE,nullptr)),done(CreateEventW(nullptr,TRUE,FALSE,nullptr));Check(go.value&&done.value,"create precise cross-thread inventory mutation events");m.go=go.value;m.done=done.value;
    std::thread worker([&]{if(WaitForSingleObject(m.go,15000)!=WAIT_OBJECT_0){m.error=ERROR_TIMEOUT;SetEvent(m.done);return;}
        Handle h(CreateFileW(m.changedPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,grow?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(h.value==INVALID_HANDLE_VALUE){m.error=GetLastError();SetEvent(m.done);return;}LARGE_INTEGER zero{};DWORD written=0;const unsigned char bytes[3]{9,8,7};
        const bool positioned=!grow||SetFilePointerEx(h.value,zero,nullptr,FILE_END);m.changed=positioned&&WriteFile(h.value,bytes,3,&written,nullptr)&&written==3&&FlushFileBuffers(h.value);if(!m.changed)m.error=GetLastError();SetEvent(m.done);});
    R result;mutation=&m;result=d.Inventory({},output);mutation=nullptr;SetEvent(m.go);worker.join();
    if(result!=R::Conflict||m.triggers!=1||!m.changed)std::fprintf(stderr,"DIAG concurrent inventory grow=%u result=%u triggers=%u changed=%u error=%lu\n",unsigned(grow),unsigned(result),m.triggers,unsigned(m.changed),m.error.load());
    Check(result==R::Conflict&&m.triggers==1&&m.changed&&m.error==ERROR_SUCCESS,"real concurrent metadata/name mutation is detected, not returned as stable inventory");saved.CheckSame(output);
    Check(d.Inventory({},output)==R::Ok,"fresh explicit inventory succeeds after concurrent writer finishes");
    if(mode==2)Check(Find(output,L"first.dat").observed.identity.fileBytes==3&&Find(output,L"first.dat").observed.streamCount==2&&Find(output,L"first.dat").observed.streamLogicalBytes==10,"unchanged stream names and default size do not hide actual ADS growth");
    else if(grow)Check(Find(output,L"first.dat").observed.identity.fileBytes==6,"new observation sees completed real file growth");else Check(Find(output,L"new-name.dat").observed.identity.fileBytes==3,"new observation sees completed real directory insertion");
}}
}
int wmain(int argc,wchar_t** argv){try{if(argc!=3||std::wstring(argv[1])!=L"--store")throw std::runtime_error("usage: cache_inventory_tests --store NEW_ABS_DIR");
    const fs::path root(argv[2]);NewDirectory(root);Hooks hooks;Tree(root);Handles(root);Reparse(root);RefusalsAndAllocation(root);ConcurrentChanges(root);
    std::printf("PASS cache inventory checks=%u\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL cache inventory after %u checks: %s\n",checks,e.what());return 1;}}
