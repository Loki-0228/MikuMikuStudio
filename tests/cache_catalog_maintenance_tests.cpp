// MAINTENANCE: regression-keep. Explicit checkpoint/collection; CPU-only owned fixtures.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "mikumikustudio/cache_catalog.hpp"
#include "../src/io/cache_catalog_codec.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
namespace catalog=mikumikustudio::cache_catalog;
namespace codec=mikumikustudio::cache_catalog::codec;
using Bytes=std::vector<std::uint8_t>;
using Result=catalog::Result;
unsigned checks=0;
bool restoreFailed=false;
bool traceMode=false;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
// Trace mode owns this EXE's import table for five Win32 file primitives. The
// wrappers observe only calls made on the current thread inside an explicitly
// armed real Store::Commit invocation; every other call forwards unchanged.
// Fixed-size failure records are filled without printing or allocating inside
// a hook. Only CreateFileW/LockFileEx/ReadFile/WriteFile/FlushFileBuffers are
// covered; this is not a complete Win32 or system-wide error trace.
enum TraceApi : unsigned { TraceCreateFileW=0,TraceLockFileEx,TraceReadFile,TraceWriteFile,TraceFlushFileBuffers,TraceApiIndexCount };
inline constexpr unsigned TraceApiCount=TraceApiIndexCount;
inline constexpr unsigned TraceRecordCapacity=64;
inline constexpr unsigned TracePathCapacity=512;
const char* TraceApiName(unsigned api) noexcept {
    switch(api){
    case TraceCreateFileW:return "CreateFileW";
    case TraceLockFileEx:return "LockFileEx";
    case TraceReadFile:return "ReadFile";
    case TraceWriteFile:return "WriteFile";
    case TraceFlushFileBuffers:return "FlushFileBuffers";
    default:return "unknown";
    }
}
const char* TraceWin32Name(DWORD error) noexcept {
    switch(error){
    case ERROR_SUCCESS:return "ERROR_SUCCESS";
    case ERROR_FILE_NOT_FOUND:return "ERROR_FILE_NOT_FOUND";
    case ERROR_PATH_NOT_FOUND:return "ERROR_PATH_NOT_FOUND";
    case ERROR_ACCESS_DENIED:return "ERROR_ACCESS_DENIED";
    case ERROR_SHARING_VIOLATION:return "ERROR_SHARING_VIOLATION";
    case ERROR_LOCK_VIOLATION:return "ERROR_LOCK_VIOLATION";
    case ERROR_FILE_EXISTS:return "ERROR_FILE_EXISTS";
    case ERROR_DISK_FULL:return "ERROR_DISK_FULL";
    case ERROR_WRITE_FAULT:return "ERROR_WRITE_FAULT";
    case ERROR_READ_FAULT:return "ERROR_READ_FAULT";
    case ERROR_INVALID_HANDLE:return "ERROR_INVALID_HANDLE";
    case ERROR_IO_PENDING:return "ERROR_IO_PENDING";
    default:return "OTHER";
    }
}
struct TraceRecord {
    unsigned api=TraceCreateFileW;
    bool injected=false;
    DWORD win32=0;
    DWORD thread=0;
    DWORD requested=0;
    DWORD returned=0;
    HANDLE handle=INVALID_HANDLE_VALUE;
    bool pathValid=false;
    bool pathTruncated=false;
    wchar_t path[TracePathCapacity]{};
};
struct TraceContext;
thread_local TraceContext* activeTrace=nullptr;
thread_local bool traceAux=false;
struct TraceContext {
    bool injectionEnabled=false;
    unsigned injectionBudget=0;
    unsigned injected=0;
    unsigned failureCount=0;
    unsigned overflow=0;
    unsigned injectionPrefixLength=0;
    wchar_t injectionPrefix[TracePathCapacity]{};
    std::array<unsigned,TraceApiCount> observed{};
    TraceRecord records[TraceRecordCapacity]{};
    void Reset() noexcept {
        injectionEnabled=false;injectionBudget=0;injected=0;failureCount=0;overflow=0;injectionPrefixLength=0;
        observed.fill(0);
    }
    void ConfigureInjection(const std::wstring& prefix) noexcept {
        injectionPrefixLength=static_cast<unsigned>((std::min)(prefix.size(),static_cast<std::size_t>(TracePathCapacity)));
        for(unsigned i=0;i<injectionPrefixLength;++i)injectionPrefix[i]=prefix[i];
        injectionEnabled=true;injectionBudget=1;injected=0;
    }
    bool MatchesInjection(const wchar_t* path,DWORD disposition) const noexcept {
        if(!injectionEnabled||injectionBudget==0||disposition!=CREATE_NEW||!path)return false;
        const std::size_t length=std::wcslen(path);
        if(length!=static_cast<std::size_t>(injectionPrefixLength)+37)return false;
        if(_wcsnicmp(path,injectionPrefix,injectionPrefixLength)!=0)return false;
        const wchar_t* leaf=path+injectionPrefixLength;
        for(unsigned i=0;i<32;++i)if(!((leaf[i]>=L'0'&&leaf[i]<=L'9')||(leaf[i]>=L'a'&&leaf[i]<=L'f')))return false;
        return std::wcscmp(leaf+32,L".blob")==0;
    }
    static void CopyPath(TraceRecord& record,const wchar_t* path) noexcept {
        const std::size_t length=std::wcslen(path);
        const unsigned copied=static_cast<unsigned>((std::min)(length,static_cast<std::size_t>(TracePathCapacity-1)));
        for(unsigned i=0;i<copied;++i)record.path[i]=path[i];
        record.path[copied]=0;record.pathValid=copied>0;record.pathTruncated=length>=TracePathCapacity;
    }
    static void CaptureHandlePath(TraceRecord& record,HANDLE handle) noexcept {
        traceAux=true;const DWORD previous=GetLastError();
        wchar_t buffer[TracePathCapacity]{};
        const DWORD count=GetFinalPathNameByHandleW(handle,buffer,static_cast<DWORD>(TracePathCapacity),VOLUME_NAME_GUID);
        traceAux=false;
        if(count&&count<static_cast<DWORD>(TracePathCapacity)){
            unsigned copied=0;while(copied<count&&buffer[copied]){record.path[copied]=buffer[copied];++copied;}
            record.path[copied]=0;record.pathValid=copied>0;record.pathTruncated=false;
        }
        SetLastError(previous);
    }
    void Record(unsigned api,DWORD error,HANDLE handle,const wchar_t* inputPath,DWORD requested,DWORD returned,bool wasInjected) noexcept {
        if(failureCount>=TraceRecordCapacity){++overflow;return;}
        TraceRecord& record=records[failureCount++];
        record.api=api;record.injected=wasInjected;record.win32=error;record.thread=GetCurrentThreadId();
        record.requested=requested;record.returned=returned;record.handle=handle;
        record.pathValid=false;record.pathTruncated=false;record.path[0]=0;
        if(inputPath)CopyPath(record,inputPath);
        else if(handle&&handle!=INVALID_HANDLE_VALUE)CaptureHandlePath(record,handle);
    }
};
using CreateFileWFn=HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using LockFileExFn=BOOL(WINAPI*)(HANDLE,DWORD,DWORD,DWORD,DWORD,LPOVERLAPPED);
using ReadFileFn=BOOL(WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
using WriteFileFn=BOOL(WINAPI*)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
using FlushFileBuffersFn=BOOL(WINAPI*)(HANDLE);
CreateFileWFn realCreateFileW=nullptr;
LockFileExFn realLockFileEx=nullptr;
ReadFileFn realReadFile=nullptr;
WriteFileFn realWriteFile=nullptr;
FlushFileBuffersFn realFlushFileBuffers=nullptr;
HANDLE WINAPI CreateFileWHook(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES attributes,DWORD disposition,DWORD flags,HANDLE templateFile)noexcept{
    TraceContext* context=activeTrace;
    if(!context||traceAux)return realCreateFileW(name,access,share,attributes,disposition,flags,templateFile);
    const DWORD previous=GetLastError();
    ++context->observed[TraceCreateFileW];
    if(context->MatchesInjection(name,disposition)){
        --context->injectionBudget;++context->injected;
        context->Record(TraceCreateFileW,ERROR_DISK_FULL,INVALID_HANDLE_VALUE,name,disposition,0,true);
        SetLastError(ERROR_DISK_FULL);return INVALID_HANDLE_VALUE;
    }
    SetLastError(previous);
    HANDLE result=realCreateFileW(name,access,share,attributes,disposition,flags,templateFile);
    const DWORD error=GetLastError();
    if(result==INVALID_HANDLE_VALUE||!result)context->Record(TraceCreateFileW,error,INVALID_HANDLE_VALUE,name,disposition,0,false);
    SetLastError(error);return result;
}
BOOL WINAPI LockFileExHook(HANDLE file,DWORD flags,DWORD reserved,DWORD bytesLow,DWORD bytesHigh,LPOVERLAPPED overlapped)noexcept{
    TraceContext* context=activeTrace;
    if(!context||traceAux)return realLockFileEx(file,flags,reserved,bytesLow,bytesHigh,overlapped);
    const DWORD previous=GetLastError();++context->observed[TraceLockFileEx];SetLastError(previous);
    const BOOL result=realLockFileEx(file,flags,reserved,bytesLow,bytesHigh,overlapped);
    const DWORD error=GetLastError();
    if(!result)context->Record(TraceLockFileEx,error,file,nullptr,flags,0,false);
    SetLastError(error);return result;
}
BOOL WINAPI ReadFileHook(HANDLE file,LPVOID buffer,DWORD bytes,LPDWORD done,LPOVERLAPPED overlapped)noexcept{
    TraceContext* context=activeTrace;
    if(!context||traceAux)return realReadFile(file,buffer,bytes,done,overlapped);
    const DWORD previous=GetLastError();++context->observed[TraceReadFile];SetLastError(previous);
    const BOOL result=realReadFile(file,buffer,bytes,done,overlapped);
    const DWORD error=GetLastError();
    const DWORD transferred=done?*done:0;
    if(!result&&error!=ERROR_IO_PENDING)context->Record(TraceReadFile,error,file,nullptr,bytes,transferred,false);
    SetLastError(error);return result;
}
BOOL WINAPI WriteFileHook(HANDLE file,LPCVOID buffer,DWORD bytes,LPDWORD done,LPOVERLAPPED overlapped)noexcept{
    TraceContext* context=activeTrace;
    if(!context||traceAux)return realWriteFile(file,buffer,bytes,done,overlapped);
    const DWORD previous=GetLastError();++context->observed[TraceWriteFile];SetLastError(previous);
    const BOOL result=realWriteFile(file,buffer,bytes,done,overlapped);
    const DWORD error=GetLastError();
    const DWORD transferred=done?*done:0;
    if(!result&&error!=ERROR_IO_PENDING)context->Record(TraceWriteFile,error,file,nullptr,bytes,transferred,false);
    SetLastError(error);return result;
}
BOOL WINAPI FlushFileBuffersHook(HANDLE file)noexcept{
    TraceContext* context=activeTrace;
    if(!context||traceAux)return realFlushFileBuffers(file);
    const DWORD previous=GetLastError();++context->observed[TraceFlushFileBuffers];SetLastError(previous);
    const BOOL result=realFlushFileBuffers(file);
    const DWORD error=GetLastError();
    if(!result)context->Record(TraceFlushFileBuffers,error,file,nullptr,0,0,false);
    SetLastError(error);return result;
}
// Identical import-table boundary to the repository's cache file fault tests,
// extended to all five primitives with RAII page/IAT restoration. Installation
// and restoration failures are never swallowed: they fail the test.
class Imports {
    struct Slot { ULONG_PTR* address;ULONG_PTR original,replacement;bool patched=false;DWORD protection=0;bool protectionKnown=false; };
    std::vector<Slot> slots_;
    static bool Exchange(Slot& slot,ULONG_PTR value)noexcept{
        DWORD before=0;if(!VirtualProtect(slot.address,sizeof(*slot.address),PAGE_READWRITE,&before))return false;
        if(!slot.protectionKnown){slot.protection=before;slot.protectionKnown=true;}
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot.address),reinterpret_cast<PVOID>(value));
        DWORD ignored=0;return VirtualProtect(slot.address,sizeof(*slot.address),slot.protection,&ignored)!=FALSE;
    }
    void Restore()noexcept{
        for(auto it=slots_.rbegin();it!=slots_.rend();++it)if(it->patched){
            if(!it->protectionKnown){it->patched=false;continue;}
            if(!Exchange(*it,it->original)||*it->address!=it->original)restoreFailed=true;
            else it->patched=false;
        }
    }
public:
    Imports(){
        auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        Check(dos&&dos->e_magic==IMAGE_DOS_SIGNATURE,"own EXE DOS header");
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
        Check(nt->Signature==IMAGE_NT_SIGNATURE,"own EXE PE header");
        const auto table=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        Check(table.VirtualAddress!=0,"own EXE ordinary import table");
        for(auto* dll=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+table.VirtualAddress);dll->Name;++dll){
            if(!dll->OriginalFirstThunk)continue;
            auto* names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+dll->OriginalFirstThunk);
            auto* addresses=reinterpret_cast<IMAGE_THUNK_DATA*>(base+dll->FirstThunk);
            for(;names->u1.AddressOfData;++names,++addresses){
                if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
                const auto* import=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
                const char* name=reinterpret_cast<const char*>(import->Name);
                ULONG_PTR replacement=0;
                const auto original=static_cast<ULONG_PTR>(addresses->u1.Function);
                if(!std::strcmp(name,"CreateFileW")){Check(!realCreateFileW||reinterpret_cast<ULONG_PTR>(realCreateFileW)==original,"consistent real CreateFileW import");realCreateFileW=reinterpret_cast<CreateFileWFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&CreateFileWHook);}
                else if(!std::strcmp(name,"LockFileEx")){Check(!realLockFileEx||reinterpret_cast<ULONG_PTR>(realLockFileEx)==original,"consistent real LockFileEx import");realLockFileEx=reinterpret_cast<LockFileExFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&LockFileExHook);}
                else if(!std::strcmp(name,"ReadFile")){Check(!realReadFile||reinterpret_cast<ULONG_PTR>(realReadFile)==original,"consistent real ReadFile import");realReadFile=reinterpret_cast<ReadFileFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&ReadFileHook);}
                else if(!std::strcmp(name,"WriteFile")){Check(!realWriteFile||reinterpret_cast<ULONG_PTR>(realWriteFile)==original,"consistent real WriteFile import");realWriteFile=reinterpret_cast<WriteFileFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&WriteFileHook);}
                else if(!std::strcmp(name,"FlushFileBuffers")){Check(!realFlushFileBuffers||reinterpret_cast<ULONG_PTR>(realFlushFileBuffers)==original,"consistent real FlushFileBuffers import");realFlushFileBuffers=reinterpret_cast<FlushFileBuffersFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&FlushFileBuffersHook);}
                if(replacement)slots_.push_back({reinterpret_cast<ULONG_PTR*>(&addresses->u1.Function),original,replacement,false});
            }
        }
        Check(realCreateFileW&&realLockFileEx&&realReadFile&&realWriteFile&&realFlushFileBuffers&&slots_.size()>=5,
            "locate all five observed production imports in this EXE");
        try{
            for(auto& slot:slots_){slot.patched=true;Check(Exchange(slot,slot.replacement),"patch only owned EXE import slot and restore page protection");}
        }catch(...){Restore();throw;}
    }
    ~Imports(){Restore();}
    Imports(const Imports&)=delete;Imports& operator=(const Imports&)=delete;
    void Verify()const{for(const auto& slot:slots_)Check(*slot.address==slot.replacement,"actual own IAT still points at calibrated wrapper");}
};
struct TraceArm {
    explicit TraceArm(TraceContext& context){Check(activeTrace==nullptr,"trace observation must not nest");activeTrace=&context;}
    ~TraceArm(){activeTrace=nullptr;}
    TraceArm(const TraceArm&)=delete;TraceArm& operator=(const TraceArm&)=delete;
};
Result ObservedCommit(const catalog::Store& store,const catalog::Snapshot& expected,const Bytes& bytes,catalog::Snapshot& output,TraceContext* context){
    if(!context)return store.Commit(expected,bytes,output);
    TraceArm arm(*context);
    return store.Commit(expected,bytes,output);
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
void NewDirectory(const fs::path& p){
    Check(p.is_absolute()&&p.native().size()>3&&p!=p.root_path()&&p==p.lexically_normal(),"test directory must be normalized absolute non-root");
    Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"test directory must be newly created");
}
void NewBytes(const fs::path& p,const Bytes& b){
    Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"create owned evidence without replacement");
    DWORD done=0;Check(b.size()<=MAXDWORD&&WriteFile(h.value,b.data(),static_cast<DWORD>(b.size()),&done,nullptr)&&done==b.size(),"write owned evidence");
}
void NewText(const fs::path& p,const std::string& s){NewBytes(p,Bytes(s.begin(),s.end()));}
Bytes ReadBytes(const fs::path& p){std::ifstream f(p,std::ios::binary);Check(bool(f),"read owned fixture");return Bytes(std::istreambuf_iterator<char>(f),{});}
files::Owner Owner(){files::Owner x{};for(unsigned i=0;i<x.size();++i)x[i]=static_cast<std::uint8_t>(i+1);return x;}
files::Owner NamespaceOwner(){files::Owner x{};const char text[]="MMSCatalogHead01";std::copy_n(text,16,x.begin());return x;}
files::Spec Spec(bool commit,const files::Owner& owner){files::Spec x;x.owner=owner;x.kind=commit?files::Kind::CatalogCommit:files::Kind::CatalogSnapshot;x.key[0]=commit?2:1;return x;}
files::Directory Open(const fs::path& p,bool create=true){files::Directory d;Check(files::Directory::Open(p,create,d)==files::Result::Ok,"open owned file directory");return d;}
files::BlobInfo Create(files::Directory& d,const files::Spec& s,const Bytes& b){files::BlobInfo info;Check(d.Create(s,b.data(),b.size(),info)==files::Result::Ok,"create real verified blob");return info;}
bool Equal(const files::BlobInfo& a,const files::BlobInfo& b){return a.name==b.name&&a.spec.kind==b.spec.kind&&a.spec.owner==b.spec.owner&&a.spec.key==b.spec.key&&a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;}
bool Equal(const codec::Record& a,const codec::Record& b){return a.revision==b.revision&&a.owner==b.owner&&Equal(a.snapshot,b.snapshot)&&Equal(a.previous,b.previous);}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
void Put(Bytes& b,std::size_t at,std::uint64_t v,unsigned width){for(unsigned i=0;i<width;++i)b.at(at+i)=static_cast<std::uint8_t>(v>>(8*i));}
std::uint64_t Get(const Bytes& b,std::size_t at,unsigned width){std::uint64_t v=0;for(unsigned i=0;i<width;++i)v|=std::uint64_t(b.at(at+i))<<(8*i);return v;}
void GoldenRef(Bytes& b,std::size_t at,const files::BlobInfo& r){
    for(unsigned i=0;i<37;++i)b.at(at+i)=static_cast<std::uint8_t>(r.name.at(i));
    Put(b,at+40,static_cast<std::uint32_t>(r.spec.kind),4);
    std::copy(r.spec.owner.begin(),r.spec.owner.end(),b.begin()+at+48);
    std::copy(r.spec.key.begin(),r.spec.key.end(),b.begin()+at+64);
    std::copy(r.payloadDigest.begin(),r.payloadDigest.end(),b.begin()+at+96);
    Put(b,at+128,r.payloadBytes,8);Put(b,at+136,r.identity.volume,4);
    Put(b,at+144,r.identity.fileIndex,8);Put(b,at+152,r.identity.fileBytes,8);
}
// Independent wire oracle: neither production Encode nor Decode participates.
Bytes Golden(const codec::Record& r){
    Bytes b(368,0);const char magic[]="MMSCacheHead0001";std::copy_n(magic,16,b.begin());
    Put(b,16,1,4);Put(b,24,r.revision,8);std::copy(r.owner.begin(),r.owner.end(),b.begin()+32);
    GoldenRef(b,48,r.snapshot);if(r.revision>1)GoldenRef(b,208,r.previous);return b;
}
files::BlobInfo Ref(const Bytes& b,std::size_t at){
    files::BlobInfo r;for(unsigned i=0;i<37;++i)r.name+=static_cast<wchar_t>(b.at(at+i));
    r.spec.kind=static_cast<files::Kind>(Get(b,at+40,4));
    std::copy_n(b.begin()+at+48,16,r.spec.owner.begin());std::copy_n(b.begin()+at+64,32,r.spec.key.begin());
    std::copy_n(b.begin()+at+96,32,r.payloadDigest.begin());r.payloadBytes=Get(b,at+128,8);
    r.identity.volume=static_cast<std::uint32_t>(Get(b,at+136,4));r.identity.fileIndex=Get(b,at+144,8);r.identity.fileBytes=Get(b,at+152,8);return r;
}
files::Spec CheckpointSpec(){auto s=Spec(true,NamespaceOwner());s.kind=files::Kind::CatalogCheckpoint;s.key[0]=3;return s;}
bool Equal(const codec::CheckpointRecord& a,const codec::CheckpointRecord& b){if(!Equal(a.state,b.state)||a.retired.size()!=b.retired.size())return false;for(std::size_t i=0;i<a.retired.size();++i)if(!Equal(a.retired[i],b.retired[i]))return false;return true;}
bool RetiredLess(const files::BlobInfo& a,const files::BlobInfo& b){const bool as=a.spec.kind==files::Kind::CatalogSnapshot,bs=b.spec.kind==files::Kind::CatalogSnapshot;return as!=bs?!as:a.name<b.name;}
Bytes GoldenCheckpoint(const codec::CheckpointRecord& r){
    auto b=Golden(r.state);b.resize(376+160*r.retired.size(),0);const char magic[]="MMSCacheBase0001";std::copy_n(magic,16,b.begin());
    Put(b,368,r.retired.size(),4);for(std::size_t i=0;i<r.retired.size();++i)GoldenRef(b,376+160*i,r.retired[i]);return b;
}
std::wstring HexName(unsigned n){wchar_t name[64]{};std::swprintf(name,64,L"%032x.blob",n);return name;}
files::BlobInfo ExampleRef(unsigned name,files::Kind kind){
    files::BlobInfo r;r.name=HexName(name);r.spec=kind==files::Kind::CatalogSnapshot?Spec(false,Owner()):kind==files::Kind::CatalogCheckpoint?CheckpointSpec():Spec(true,NamespaceOwner());
    r.payloadBytes=kind==files::Kind::CatalogSnapshot?19:kind==files::Kind::CatalogCommit?368:536;
    r.identity={0x10203040,0x1122334455667788ull+name,128+r.payloadBytes};for(unsigned i=0;i<32;++i)r.payloadDigest[i]=static_cast<std::uint8_t>(i+name);return r;
}
codec::CheckpointRecord Example(){
    codec::CheckpointRecord r;r.state.revision=13;r.state.owner=Owner();r.state.snapshot=ExampleRef(30,files::Kind::CatalogSnapshot);
    r.state.previous=ExampleRef(20,files::Kind::CatalogCheckpoint);
    r.retired={ExampleRef(10,files::Kind::CatalogCommit),r.state.previous,ExampleRef(5,files::Kind::CatalogSnapshot)};return r;
}
void CodecChecks(){
    const auto input=Example();const auto golden=GoldenCheckpoint(input);Bytes encoded{1,2};
    Check(codec::EncodeCheckpoint(input,encoded)==codec::Result::Ok&&encoded==golden,"checkpoint entire byte layout matches independent golden offsets");
    auto decoded=input;decoded.state.revision=99;Check(codec::DecodeCheckpoint(golden.data(),golden.size(),decoded)==codec::Result::Ok&&Equal(decoded,input),"decode independently constructed checkpoint bytes");
    auto ordinary=input.state;ordinary.revision=14;Bytes plain;Check(codec::Encode(ordinary,plain)==codec::Result::Ok&&plain==Golden(ordinary),"ordinary commit may reference a checkpoint with exact wire identity");
    codec::Record ordinaryOut;Check(codec::Decode(plain.data(),plain.size(),ordinaryOut)==codec::Result::Ok&&Equal(ordinaryOut,ordinary),"ordinary previous-checkpoint decoding");
    const auto reject=[&](const Bytes& b){auto out=input;Check(codec::DecodeCheckpoint(b.data(),b.size(),out)==codec::Result::Invalid&&Equal(out,input),"invalid checkpoint retains all prior output fields");};
    for(unsigned at:{0u,15u,16u,20u,21u,22u,23u,372u,373u,374u,375u}){auto bad=golden;bad[at]^=0x80;reject(bad);}
    for(std::size_t ref:{48u,208u,376u,536u,696u})for(unsigned offset:{37u,38u,39u,44u,45u,46u,47u,140u,141u,142u,143u}){auto bad=golden;bad[ref+offset]=1;reject(bad);}
    for(unsigned count:{0u,193u,0xffffffffu}){auto bad=golden;Put(bad,368,count,4);reject(bad);}
    {auto bad=golden;Put(bad,24,1,8);reject(bad);bad=golden;bad.push_back(0);reject(bad);bad=golden;bad.pop_back();reject(bad);}
    {auto out=input;Check(codec::DecodeCheckpoint(nullptr,golden.size(),out)==codec::Result::Invalid&&Equal(out,input),"null checkpoint rejected");}
    std::vector<codec::CheckpointRecord> badRecords;
    auto bad=input;bad.retired.push_back(bad.retired.back());badRecords.push_back(bad);
    bad=input;std::swap(bad.retired[0],bad.retired[1]);badRecords.push_back(bad);
    bad=input;bad.retired.erase(bad.retired.begin()+1);badRecords.push_back(bad);
    bad=input;bad.retired.push_back(bad.state.snapshot);badRecords.push_back(bad);
    bad=input;bad.retired[1].identity.fileIndex^=1;badRecords.push_back(bad);
    bad=input;bad.retired[0].spec.kind=files::Kind::Texture;badRecords.push_back(bad);
    bad=input;bad.retired[2].spec.owner[0]^=1;badRecords.push_back(bad);
    bad=input;bad.retired[0].spec.key[1]=1;badRecords.push_back(bad);
    bad=input;bad.retired[2].identity.fileBytes++;badRecords.push_back(bad);
    bad=input;bad.retired[0].name=bad.retired[1].name;badRecords.push_back(bad); // commit/checkpoint share one directory.
    for(const auto& r:badRecords){Bytes out{0xab};Check(codec::EncodeCheckpoint(r,out)==codec::Result::Invalid&&out==Bytes{0xab},"invalid retirement input retains encoded output");reject(GoldenCheckpoint(r));}
    auto maximum=input;maximum.retired.clear();for(unsigned i=0;i<191;++i)maximum.retired.push_back(ExampleRef(100+i,files::Kind::CatalogCommit));
    maximum.state.previous=ExampleRef(400,files::Kind::CatalogCheckpoint);maximum.retired.push_back(maximum.state.previous);
    Check(codec::EncodeCheckpoint(maximum,encoded)==codec::Result::Ok&&encoded==GoldenCheckpoint(maximum)&&encoded.size()==31096&&encoded.size()+128<=32768,"192-reference maximum fits only declared maintenance reserve");
    decoded={};Check(codec::DecodeCheckpoint(encoded.data(),encoded.size(),decoded)==codec::Result::Ok&&Equal(decoded,maximum),"maximum complete checkpoint roundtrip");
}
struct Entry {files::BlobInfo info;Bytes payload;};
std::vector<Entry> Entries(const fs::path& path,const files::Owner& owner){
    auto d=Open(path,false);std::vector<std::wstring> names;Check(d.Enumerate(256,names)==files::Result::Ok,"enumerate actual metadata blobs");std::vector<Entry> out;
    for(const auto& name:names){files::ReadLease lease;Check(d.Inspect(name,owner,16*1024*1024,lease)==files::Result::Ok,"verify actual metadata envelope");Bytes bytes;Check(lease.ReadAll(bytes)==files::Result::Ok,"read actual metadata payload");out.push_back({lease.Info(),std::move(bytes)});}return out;
}
Entry Head(const fs::path& path){auto entries=Entries(path/L"commits",NamespaceOwner());Check(!entries.empty(),"actual head exists");return *std::max_element(entries.begin(),entries.end(),[](const auto& a,const auto& b){return Get(a.payload,24,8)<Get(b.payload,24,8);});}
std::vector<files::BlobInfo> Retired(const Entry& cp){Check(cp.info.spec.kind==files::Kind::CatalogCheckpoint&&cp.payload.size()>=376,"independent reader observes checkpoint kind and minimum size");const auto n=Get(cp.payload,368,4);Check(n>0&&n<=192&&cp.payload.size()==376+160*n,"independent retirement count and exact size");std::vector<files::BlobInfo> out;for(std::size_t i=0;i<n;++i)out.push_back(Ref(cp.payload,376+160*i));return out;}
fs::path FilePath(const fs::path& root,const files::BlobInfo& info){return root/(info.spec.kind==files::Kind::CatalogSnapshot?L"snapshots":L"commits")/info.name;}
std::vector<std::wstring> Names(const fs::path& path){auto d=Open(path,false);std::vector<std::wstring> out;Check(d.Enumerate(256,out)==files::Result::Ok,"enumerate exact filenames");std::sort(out.begin(),out.end());return out;}
std::uint64_t PhysicalBytes(const fs::path& path){std::uint64_t bytes=0;for(const auto* folder:{L"snapshots",L"commits"})for(const auto& entry:fs::directory_iterator(path/folder))bytes+=entry.file_size();return bytes;}
using Disk=std::vector<std::pair<fs::path,Bytes>>;
Disk DiskState(const fs::path& root){Disk out;for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file())out.emplace_back(fs::relative(entry.path(),root),ReadBytes(entry.path()));std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first<b.first;});return out;}
catalog::Snapshot Init(const fs::path& path,const Bytes& bytes={0xff,0,0x80}){catalog::Snapshot s;Check(catalog::Store(path).Initialize(bytes,s)==Result::Ok&&s.IsValid()&&s.Revision()==1&&s.Bytes()==bytes,"initialize owned maintenance store");return s;}
void ReadEquals(const fs::path& path,const catalog::Snapshot& expected){catalog::Snapshot actual;Check(catalog::Store(path).Read(actual)==Result::Ok&&Same(actual,expected),"fresh Store reads exact current revision owner and bytes");}
bool SameReport(const catalog::CollectionReport& a,const catalog::CollectionReport& b){return a.attempted==b.attempted&&a.absent==b.absent&&a.busy==b.busy&&a.pending==b.pending&&a.failed==b.failed&&a.absentBytes==b.absentBytes;}
catalog::CollectionReport ReportSentinel(){return {11,12,13,14,15,0x123456789ull};}
void CheckReport(const catalog::CollectionReport& report){Check(std::uint64_t(report.absent)+report.busy+report.pending+report.failed==report.attempted,"collection report partitions only actually attempted present retirees");}
void CollectAll(const fs::path& path,const catalog::Snapshot& current){
    const auto head=Head(path);const auto live=Ref(head.payload,48);const auto refs=Retired(head);catalog::CollectionReport report=ReportSentinel();
    Check(catalog::Store(path).CollectRetired(report)==Result::Ok&&report.busy==0&&report.failed==0,"unleased authorized retired generations collect without busy/failure");
    CheckReport(report);
    for(const auto& ref:refs)Check(!fs::exists(FilePath(path,ref)),"retired namespace entry actually disappears");
    Check(fs::exists(FilePath(path,head.info))&&fs::exists(FilePath(path,live)),"collection retains current checkpoint and live snapshot");ReadEquals(path,current);
}
void PrintTraceRecords(const char* label,const TraceContext& context){
    std::printf("%s injected=%u apiFailures=%u overflow=%u observedCreateFileW=%u observedLockFileEx=%u observedReadFile=%u observedWriteFile=%u observedFlushFileBuffers=%u\n",
        label,context.injected,context.failureCount,context.overflow,
        context.observed[TraceCreateFileW],context.observed[TraceLockFileEx],context.observed[TraceReadFile],
        context.observed[TraceWriteFile],context.observed[TraceFlushFileBuffers]);
    for(unsigned i=0;i<context.failureCount;++i){
        const auto& record=context.records[i];
        std::printf("TRACE failure index=%u api=%s injected=%d win32=%lu(%s) thread=%lu handle=%p requested=%lu returned=%lu path=%ls%s\n",
            i,TraceApiName(record.api),int(record.injected),static_cast<unsigned long>(record.win32),TraceWin32Name(record.win32),
            static_cast<unsigned long>(record.thread),static_cast<void*>(record.handle),
            static_cast<unsigned long>(record.requested),static_cast<unsigned long>(record.returned),
            record.pathValid?record.path:L"<unavailable>",record.pathTruncated?"...":"");
    }
}
struct TraceScenarioAudit {
    const char* name=nullptr;
    int commitResult=0;
    unsigned injected=0;
    unsigned apiFailures=0;
    unsigned overflow=0;
    std::array<unsigned,TraceApiCount> observed{};
    int retryResult=0;
    bool retryChecked=false;
    unsigned long long retryRevision=0;
};
TraceScenarioAudit tracePositiveAudit,traceBusyAudit,traceCreateNewAudit;
struct TraceThousandAudit {
    unsigned long long calls=0,successes=0,failures=0,apiFailures=0,overflow=0,injected=0;
    std::array<unsigned long long,TraceApiCount> observed{};
};
TraceThousandAudit traceThousandAudit;
void CopyTraceAudit(TraceScenarioAudit& audit,const char* name,int result,const TraceContext& context){
    audit.name=name;audit.commitResult=result;audit.injected=context.injected;
    audit.apiFailures=context.failureCount;audit.overflow=context.overflow;
    for(unsigned api=0;api<TraceApiCount;++api)audit.observed[api]=context.observed[api];
}
void PrintScenarioAudit(const TraceScenarioAudit& audit){
    if(!audit.name)return;
    std::printf("TRACE scenario name=%s commitResult=%d injected=%u apiFailures=%u overflow=%u observedCreateFileW=%u observedLockFileEx=%u observedReadFile=%u observedWriteFile=%u observedFlushFileBuffers=%u retryChecked=%d retryResult=%d retryRevision=%llu\n",
        audit.name,audit.commitResult,audit.injected,audit.apiFailures,audit.overflow,
        audit.observed[TraceCreateFileW],audit.observed[TraceLockFileEx],audit.observed[TraceReadFile],
        audit.observed[TraceWriteFile],audit.observed[TraceFlushFileBuffers],
        int(audit.retryChecked),audit.retryResult,audit.retryRevision);
}
void PrintTraceSummary(){
    std::printf("TRACE summary mode=--trace-store restoreFailed=%d controls=trace-controls/{positive,busy,create-new} scope=current-thread-real-Store::Commit-only implemented=CreateFileW,LockFileEx,ReadFile,WriteFile,FlushFileBuffers\n",int(restoreFailed));
    std::printf("TRACE summary note=only these five imported primitives are observed; other APIs and system-wide errors are not claimed; apiFailureRecords counts every failing observed call including production lookup probes\n");
    PrintScenarioAudit(tracePositiveAudit);
    PrintScenarioAudit(traceBusyAudit);
    PrintScenarioAudit(traceCreateNewAudit);
    std::printf("TRACE thousand calls=%llu successes=%llu failures=%llu apiFailureRecords=%llu overflow=%llu injected=%llu observedCreateFileW=%llu observedLockFileEx=%llu observedReadFile=%llu observedWriteFile=%llu observedFlushFileBuffers=%llu\n",
        traceThousandAudit.calls,traceThousandAudit.successes,traceThousandAudit.failures,traceThousandAudit.apiFailures,
        traceThousandAudit.overflow,traceThousandAudit.injected,
        traceThousandAudit.observed[TraceCreateFileW],traceThousandAudit.observed[TraceLockFileEx],
        traceThousandAudit.observed[TraceReadFile],traceThousandAudit.observed[TraceWriteFile],
        traceThousandAudit.observed[TraceFlushFileBuffers]);
}
std::wstring TraceRootPrefix(const fs::path& path){
    Handle directory(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    Check(directory.value!=INVALID_HANDLE_VALUE,"resolve only owned trace calibration directory");
    wchar_t name[32768]{};const DWORD n=GetFinalPathNameByHandleW(directory.value,name,32768,VOLUME_NAME_GUID);
    Check(n&&n<32768,"resolve stable owned trace volume GUID path");
    std::wstring prefix(name,n);if(prefix.back()!=L'\\')prefix+=L'\\';return prefix;
}
// Bounded calibration around real publications. These controls add API-level
// evidence only; they neither reproduce nor repair any earlier run.
void TraceCalibrate(const fs::path& controls){
    NewDirectory(controls);
    {
        const auto path=controls/L"positive";
        auto s=Init(path,Bytes{0x41,0x42});catalog::Store store(path);const auto before=s;
        const Bytes payload{0x51,0x52,0x53};TraceContext capture;capture.Reset();Result result=Result::Invalid;
        { TraceArm arm(capture);result=store.Commit(s,payload,s); }
        const std::array<unsigned,TraceApiCount> observed=capture.observed;
        const bool correct=result==Result::Ok&&s.IsValid()&&s.Revision()==before.Revision()+1&&s.Bytes()==payload&&s.StoreId()==before.StoreId();
        PrintTraceRecords("TRACE scenario=positive",capture);
        CopyTraceAudit(tracePositiveAudit,"positive",static_cast<int>(result),capture);
        Check(correct,"trace positive control advances one real commit revision and payload");
        Check(capture.injected==0&&observed[TraceCreateFileW]>0&&observed[TraceLockFileEx]>0&&observed[TraceReadFile]>0&&
            observed[TraceWriteFile]>0&&observed[TraceFlushFileBuffers]>0,
            "trace positive control observes all five real imports with no injection");
        bool anyInjected=false;for(unsigned i=0;i<capture.failureCount;++i)anyInjected=anyInjected||capture.records[i].injected;
        Check(!anyInjected,"trace positive control records no injected API rejection");
        // Helper I/O after the arm must forward to the real imports unobserved.
        ReadEquals(path,s);Open(path,false);
        Check(capture.observed==observed,"trace observation is confined to the armed real Commit scope");
    }
    {
        const auto path=controls/L"busy";
        auto s=Init(path,Bytes{0x61,0x62});catalog::Store store(path);const auto before=s;const auto disk=DiskState(path);
        const Bytes payload{0x63,0x64};TraceContext capture;capture.Reset();Result result=Result::Invalid;
        {
            auto directory=Open(path,false);files::FileLock held;
            Check(directory.Lock(0,held)==files::Result::Ok&&held.IsValid(),"trace busy control holds the real catalog root lock");
            { TraceArm arm(capture);result=store.Commit(s,payload,s); }
        }
        Check(result==Result::Busy&&Same(s,before),"trace busy control returns actual Busy and preserves the snapshot");
        Check(capture.injected==0&&capture.failureCount==1&&capture.observed[TraceLockFileEx]>0,
            "trace busy control records exactly one real lock conflict");
        const auto& record=capture.records[0];
        Check(record.api==TraceLockFileEx&&!record.injected&&record.win32==ERROR_LOCK_VIOLATION,
            "trace busy evidence is a real LockFileEx conflict with ERROR_LOCK_VIOLATION");
        Check((record.requested&(LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY))==(LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY),
            "trace busy evidence preserves the production lock flags");
        Check(record.handle!=INVALID_HANDLE_VALUE&&record.handle!=nullptr,"trace busy evidence carries the opened lock handle");
        if(record.pathValid){const std::size_t n=std::wcslen(record.path);Check(n>=5&&std::wcscmp(record.path+n-5,L".lock")==0,"trace busy evidence path names the actual catalog lock file");}
        PrintTraceRecords("TRACE scenario=busy",capture);
        CopyTraceAudit(traceBusyAudit,"busy",static_cast<int>(result),capture);
        Check(DiskState(path)==disk,"trace busy failure leaves every owned name and byte unchanged");
        ReadEquals(path,before);
        const auto expected=before.Revision()+1;const Bytes retryPayload{0x65,0x66,0x67};
        Check(store.Commit(s,retryPayload,s)==Result::Ok&&s.IsValid()&&s.Revision()==expected&&s.Bytes()==retryPayload&&s.StoreId()==before.StoreId(),
            "trace busy control retries one real commit after lock release");
        catalog::Snapshot actual;Check(catalog::Store(path).Read(actual)==Result::Ok&&Same(actual,s),"trace busy retry is readable by a fresh Store");
        traceBusyAudit.retryChecked=true;traceBusyAudit.retryResult=static_cast<int>(Result::Ok);traceBusyAudit.retryRevision=s.Revision();
    }
    {
        const auto path=controls/L"create-new";
        auto s=Init(path,Bytes{0x71,0x72});catalog::Store store(path);const auto before=s;const auto disk=DiskState(path);
        const auto prefix=TraceRootPrefix(path/L"snapshots");const Bytes payload{0x73,0x74,0x75};
        TraceContext capture;capture.Reset();capture.ConfigureInjection(prefix);Result result=Result::Invalid;
        { TraceArm arm(capture);result=store.Commit(s,payload,s); }
        Check(result==Result::Io&&Same(s,before),"trace create-new injection returns actual Io and preserves the snapshot");
        Check(capture.injected==1&&capture.failureCount==1&&capture.overflow==0,
            "trace create-new injection triggers exactly once and records exactly one failure");
        const auto& record=capture.records[0];
        Check(record.api==TraceCreateFileW&&record.injected&&record.win32==ERROR_DISK_FULL,
            "trace create-new evidence is an injected CreateFileW with ERROR_DISK_FULL");
        Check(record.requested==CREATE_NEW,"trace create-new evidence preserves the CREATE_NEW request");
        Check(record.pathValid&&!record.pathTruncated,"trace create-new evidence captures the rejected path");
        const std::size_t prefixLength=prefix.size();
        Check(std::wcslen(record.path)==prefixLength+37&&_wcsnicmp(record.path,prefix.c_str(),prefixLength)==0,
            "trace create-new target is exactly the canonical snapshots directory of this calibration store");
        const wchar_t* leaf=record.path+prefixLength;
        for(unsigned i=0;i<32;++i)Check((leaf[i]>=L'0'&&leaf[i]<=L'9')||(leaf[i]>=L'a'&&leaf[i]<=L'f'),"trace create-new target leaf is 32 lowercase hex digits");
        Check(std::wcscmp(leaf+32,L".blob")==0,"trace create-new target leaf is a 37-character .blob generation name");
        PrintTraceRecords("TRACE scenario=create-new",capture);
        CopyTraceAudit(traceCreateNewAudit,"create-new",static_cast<int>(result),capture);
        Check(DiskState(path)==disk,"trace create-new rejection leaves every owned name and byte unchanged");
        ReadEquals(path,before);
        const auto expected=before.Revision()+1;const Bytes retryPayload{0x76,0x77,0x78};
        Check(store.Commit(s,retryPayload,s)==Result::Ok&&s.IsValid()&&s.Revision()==expected&&s.Bytes()==retryPayload&&s.StoreId()==before.StoreId(),
            "trace create-new control retries one real commit after injection is disabled");
        catalog::Snapshot actual;Check(catalog::Store(path).Read(actual)==Result::Ok&&Same(actual,s),"trace create-new retry is readable by a fresh Store");
        traceCreateNewAudit.retryChecked=true;traceCreateNewAudit.retryResult=static_cast<int>(Result::Ok);traceCreateNewAudit.retryRevision=s.Revision();
    }
}
void BoundaryAndLeases(const fs::path& root){
    const auto path=root/L"head-boundary";auto s=Init(path);catalog::Store store(path);const auto first=Head(path);auto snapshots=Open(path/L"snapshots",false),heads=Open(path/L"commits",false);
    files::ReadLease oldSnapshot,oldHead;Check(snapshots.Acquire(Ref(first.payload,48),oldSnapshot)==files::Result::Ok&&heads.Acquire(first.info,oldHead)==files::Result::Ok,"hold actual retired snapshot and commit leases");snapshots={};heads={};
    for(unsigned i=2;i<=64;++i)Check(store.Commit(s,Bytes{static_cast<std::uint8_t>(i)},s)==Result::Ok&&s.Revision()==i,"ordinary records reach original 64 limit");
    auto before=s;const auto names=Names(path/L"snapshots");const auto disk=DiskState(path);
    Check(store.Commit(s,{},s)==Result::Limit&&Same(s,before)&&DiskState(path)==disk,"ordinary 65th head cannot consume maintenance slot");
    Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==65&&s.Bytes()==before.Bytes()&&s.StoreId()==before.StoreId(),"alias checkpoint retains same value and advances global revision");
    Check(Names(path/L"snapshots")==names&&Names(path/L"commits").size()==65,"checkpoint uses exactly one reserved head and no copied snapshot");
    const auto cp=Head(path);const auto live=Ref(cp.payload,48);const auto refs=Retired(cp);Check(refs.size()==127,"64 heads plus 63 old snapshots are explicitly retired");
    const auto after=s;const auto checkpointDisk=DiskState(path);
    Check(store.Checkpoint(before,s)==Result::Conflict&&Same(s,after)&&DiskState(path)==checkpointDisk,"pre-checkpoint expected cannot publish stale checkpoint");
    Check(store.Commit(before,{},s)==Result::Conflict&&Same(s,after),"old expected also conflicts for ordinary commit");
    Check(store.Commit(s,{},s)==Result::Limit&&Same(s,after)&&store.Checkpoint(s,s)==Result::Limit&&Same(s,after),"65 physical heads prevent second reserve and ordinary publication until collection");
    catalog::CollectionReport report=ReportSentinel();Check(store.CollectRetired(report)==Result::Ok&&report.busy==2&&report.failed==0&&report.absent==125&&report.pending==0&&report.attempted==127,"external read leases produce exactly two Busy entries while 125 retired files disappear");CheckReport(report);
    std::uint64_t removedBytes=0;for(const auto& ref:refs)if(!Equal(ref,first.info)&&!Equal(ref,Ref(first.payload,48)))removedBytes+=ref.identity.fileBytes;
    Check(report.absentBytes==removedBytes,"reported absent bytes match exact disappeared immutable files");
    Check(fs::exists(FilePath(path,first.info))&&fs::exists(FilePath(path,Ref(first.payload,48))),"leased retired metadata is not deleted");
    for(const auto& ref:refs)if(!Equal(ref,first.info)&&!Equal(ref,Ref(first.payload,48)))Check(!fs::exists(FilePath(path,ref)),"internal validation leases do not falsely block unleased retirees");
    Check(fs::exists(FilePath(path,cp.info))&&fs::exists(FilePath(path,live)),"busy collection retains active checkpoint and payload");ReadEquals(path,s);
    // Publish a second checkpoint while two prior retirement entries remain.
    Check(store.Commit(s,Bytes{0x73},s)==Result::Ok,"collection reclaimed enough physical slots to resume commits");
    Check(store.Checkpoint(s,s)==Result::Ok,"next checkpoint incorporates remaining old retirees");const auto second=Head(path);const auto secondRefs=Retired(second);
    for(const auto& retained:{first.info,Ref(first.payload,48),cp.info})Check(std::any_of(secondRefs.begin(),secondRefs.end(),[&](const auto& x){return Equal(x,retained);}),"new checkpoint names previous still-present retiree by full identity");
    oldSnapshot={};oldHead={};CollectAll(path,s);
    Check(Names(path/L"commits").size()==1&&Names(path/L"snapshots").size()==1,"release permits actual reclamation to one checkpoint and one snapshot");
    // Later active tail snapshots must never be mistaken for obsolete payloads.
    Check(store.Commit(s,Bytes{1},s)==Result::Ok,"first active tail commit");const auto tail1=Head(path);
    Check(store.Commit(s,Bytes{2},s)==Result::Ok,"second active tail commit");const auto tail2=Head(path);
    report=ReportSentinel();Check(store.CollectRetired(report)==Result::Ok&&report.attempted==0&&report.absent==0&&report.busy==0&&report.pending==0&&report.failed==0&&report.absentBytes==0,"already-absent retirees do not count as new collection attempts");CheckReport(report);
    for(const auto& item:{second.info,Ref(second.payload,48),tail1.info,Ref(tail1.payload,48),tail2.info,Ref(tail2.payload,48)})Check(fs::exists(FilePath(path,item)),"all active chain heads and payloads survive collection");ReadEquals(path,s);
    {auto d=Open(path,false);files::FileLock lock;Check(d.Lock(0,lock)==files::Result::Ok,"hold maintenance root lock");auto out=s;auto original=ReportSentinel();report=original;
        Check(store.Checkpoint(s,out)==Result::Busy&&Same(out,s),"busy checkpoint retains output");Check(store.CollectRetired(report)==Result::Busy&&SameReport(report,original),"setup Busy preserves report");}
}
void ThousandCommits(const fs::path& root){
    const auto path=root/L"thousand";auto s=Init(path,{});catalog::Store store(path);const auto owner=s.StoreId();std::uint64_t revision=1;unsigned checkpoints=0;
    TraceContext traceBuffer;TraceContext* trace=traceMode?&traceBuffer:nullptr;
    for(unsigned i=1;i<=1000;++i){Bytes bytes(8);Put(bytes,0,i,8);
        const auto beforeRevision=s.Revision();const auto expectedRevision=revision+1;
        if(trace)trace->Reset();
        const auto committed=ObservedCommit(store,s,bytes,s,trace);const auto commitError=GetLastError();const auto commitErrno=errno;
        const bool correct=committed==Result::Ok&&s.Revision()==expectedRevision&&s.Bytes()==bytes&&s.StoreId()==owner;
        if(!correct){
            std::fprintf(stderr,"DIAG thousand i=%u result=%d win32=%lu errno=%d before=%llu expected=%llu after=%llu valid=%d payloadEqual=%d ownerEqual=%d bytes=%zu\n",
                i,static_cast<int>(committed),commitError,commitErrno,static_cast<unsigned long long>(beforeRevision),
                static_cast<unsigned long long>(expectedRevision),static_cast<unsigned long long>(s.Revision()),
                int(s.IsValid()),int(s.Bytes()==bytes),int(s.StoreId()==owner),s.Bytes().size());
            // Diagnostic read only: no recovery, retry or cleanup of the failed
            // publication. Preserve its result and the original strict check.
            catalog::Snapshot actual;const auto read=store.Read(actual);const auto readError=GetLastError();const auto readErrno=errno;
            std::fprintf(stderr,"DIAG thousand reread=%d win32=%lu errno=%d revision=%llu valid=%d payloadEqual=%d ownerEqual=%d bytes=%zu\n",
                static_cast<int>(read),readError,readErrno,static_cast<unsigned long long>(actual.Revision()),
                int(actual.IsValid()),int(actual.Bytes()==bytes),int(actual.StoreId()==owner),actual.Bytes().size());
            if(trace)PrintTraceRecords("TRACE thousand commit-failure",*trace);
        }
        if(trace){
            ++traceThousandAudit.calls;
            if(correct)++traceThousandAudit.successes;else ++traceThousandAudit.failures;
            traceThousandAudit.apiFailures+=trace->failureCount;
            traceThousandAudit.overflow+=trace->overflow;
            traceThousandAudit.injected+=trace->injected;
            for(unsigned api=0;api<TraceApiCount;++api)traceThousandAudit.observed[api]+=trace->observed[api];
        }
        Check(correct,"actual commit advances without resetting global revision");revision=expectedRevision;
        Check(Names(path/L"commits").size()<=33&&Names(path/L"snapshots").size()<=33&&PhysicalBytes(path)<128*1024,"periodic maintenance keeps real physical metadata bounded");
        if(i%32==0||i==1000){const auto payloads=Names(path/L"snapshots");Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==++revision&&s.Bytes()==bytes&&s.StoreId()==owner,"periodic same-value checkpoint");++checkpoints;
            Check(Names(path/L"snapshots")==payloads,"maintenance does not duplicate payload bytes");CollectAll(path,s);Check(Names(path/L"commits").size()==1&&Names(path/L"snapshots").size()==1,"each cycle truly removes retired namespace entries");}}
    ReadEquals(path,s);NewText(root/L"thousand-result.txt","commits=1000\ncheckpoints="+std::to_string(checkpoints)+"\nrevision="+std::to_string(revision)+"\nphysicalBytes="+std::to_string(PhysicalBytes(path))+"\n");
}
void ByteReserve(const fs::path& root){
    const auto path=root/L"byte-reserve";Bytes large(16*1024*1024,0x59);auto s=Init(path,large);catalog::Store store(path);
    Check(store.Commit(s,large,s)==Result::Ok&&store.Commit(s,large,s)==Result::Ok,"prepare three maximal snapshots below ordinary byte limit");
    const auto used=PhysicalBytes(path);const auto target=64ull*1024*1024-100;Check(target>used+128&&target-used-128<=16*1024*1024,"independent orphan size fits payload cap");
    {auto d=Open(path/L"snapshots",false);Bytes orphan(static_cast<std::size_t>(target-used-128),0x27);Create(d,Spec(false,s.StoreId()),orphan);}
    Check(PhysicalBytes(path)==target,"fixture is exactly 100 bytes below ordinary metadata budget");const auto before=s;const auto beforeNames=Names(path/L"snapshots");
    Check(store.Commit(s,{},s)==Result::Limit&&Same(s,before)&&PhysicalBytes(path)==target,"ordinary commit cannot borrow checkpoint byte reserve");
    Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==before.Revision()+1&&s.Bytes()==large&&Names(path/L"snapshots")==beforeNames,"checkpoint reuses large payload instead of copying it");
    Check(PhysicalBytes(path)>64ull*1024*1024&&PhysicalBytes(path)<=64ull*1024*1024+32768,"only checkpoint enters bounded 32KiB byte reserve");ReadEquals(path,s);
    const auto reserved=PhysicalBytes(path);const auto checkpoint=s;Check(store.Commit(s,{},s)==Result::Limit&&Same(s,checkpoint)&&PhysicalBytes(path)==reserved,"ordinary append remains blocked while physical retired bytes occupy reserve");
    CollectAll(path,s);Check(PhysicalBytes(path)<17ull*1024*1024,"collection releases namespace accounting for large retired payloads");Check(store.Commit(s,{},s)==Result::Ok,"ordinary appends resume after real collection");
}
void RejectDamaged(const fs::path& path,catalog::Snapshot& s){
    const auto before=s;const auto disk=DiskState(path);catalog::Store store(path);auto report=ReportSentinel();const auto original=report;
    Check(store.Read(s)==Result::RecoveryRequired&&Same(s,before),"invalid checkpoint/fork cannot fall back to old valid head");
    Check(store.Checkpoint(s,s)==Result::RecoveryRequired&&Same(s,before),"invalid metadata cannot be legitimized by checkpointing");
    Check(store.CollectRetired(report)==Result::RecoveryRequired&&SameReport(report,original),"invalid metadata cannot authorize partial collection");Check(DiskState(path)==disk,"recovery refusal preserves every original byte");
}
void CorruptionAndForks(const fs::path& root){
    for(unsigned mode=0;mode<5;++mode){const auto path=root/(L"reject-"+std::to_wstring(mode));auto s=Init(path);catalog::Store store(path);Check(store.Checkpoint(s,s)==Result::Ok,"prepare real checkpoint before untrusted addition");const auto cp=Head(path);
        if(mode==0)NewText(path/L"commits"/L"future-format.txt","unknown record\n");
        if(mode==1){auto bytes=cp.payload;Put(bytes,16,2,4);auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),bytes);}
        if(mode==2){auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),cp.payload);} // Same revision, different immutable generation.
        if(mode==3){Check(store.Commit(s,Bytes{0x31},s)==Result::Ok,"create valid tail for higher competing checkpoint");const auto tail=Head(path);
            codec::CheckpointRecord competing;competing.state.revision=s.Revision()+1;competing.state.owner=s.StoreId();competing.state.snapshot=Ref(tail.payload,48);competing.state.previous=tail.info;
            for(const auto& e:Entries(path/L"commits",NamespaceOwner()))if(!Equal(e.info,cp.info))competing.retired.push_back(e.info);
            for(const auto& e:Entries(path/L"snapshots",s.StoreId()))if(!Equal(e.info,competing.state.snapshot))competing.retired.push_back(e.info);
            std::sort(competing.retired.begin(),competing.retired.end(),RetiredLess);auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),GoldenCheckpoint(competing));}
        if(mode==4){auto bytes=cp.payload;bytes[208+144]^=1;bytes[376+144]^=1;auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),bytes);} // Correct outer SHA and internally matching previous, incorrect actual file identity.
        RejectDamaged(path,s);
    }
}
void SourceAndReserveChecks(const fs::path& root){
    // Exactly one checkpoint: neither defect can be masked by a competing CP.
    for(unsigned mode=0;mode<2;++mode){
        const auto path=root/(L"checkpoint-source-"+std::to_wstring(mode));auto s=Init(path);const auto previous=Head(path);
        codec::CheckpointRecord cp;cp.state.owner=s.StoreId();cp.state.previous=previous.info;cp.state.snapshot=Ref(previous.payload,48);
        cp.state.revision=mode==0?3:2;cp.retired.push_back(previous.info);
        if(mode==1){auto snapshots=Open(path/L"snapshots",false);cp.retired.push_back(cp.state.snapshot);cp.state.snapshot=Create(snapshots,Spec(false,s.StoreId()),Bytes{0x45,0x67});}
        const auto wire=GoldenCheckpoint(cp);codec::CheckpointRecord parsed;
        Check(codec::DecodeCheckpoint(wire.data(),wire.size(),parsed)==codec::Result::Ok&&Equal(parsed,cp),"source mismatch fixture is independently built and format-valid");
        {auto heads=Open(path/L"commits",false);Create(heads,CheckpointSpec(),wire);}
        Check(Names(path/L"commits").size()==2&&fs::exists(FilePath(path,previous.info)),"single candidate checkpoint retains exact source for relational verification");
        RejectDamaged(path,s);
    }
    // Missing authorized previous is the distinct supported self-contained case.
    {const auto path=root/L"checkpoint-source-retired";auto s=Init(path);catalog::Store store(path);const auto previous=Head(path);
        Check(store.Checkpoint(s,s)==Result::Ok,"publish valid self-contained checkpoint");CollectAll(path,s);
        Check(!fs::exists(FilePath(path,previous.info)),"authorized predecessor is actually absent after collection");ReadEquals(path,s);
        Check(store.Commit(s,Bytes{0x99},s)==Result::Ok,"self-contained checkpoint still permits a contiguous ordinary successor");ReadEquals(path,s);}
    // A verified CP exists, but the active head is ordinary. A valid older CP
    // cannot lend its reserved physical slot to that ordinary successor.
    {const auto path=root/L"ordinary-tail-reserve";auto s=Init(path);catalog::Store store(path);
        for(unsigned revision=2;revision<=63;++revision)Check(store.Commit(s,Bytes{static_cast<std::uint8_t>(revision)},s)==Result::Ok,"prepare 63 physical ordinary records");
        Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==64&&Names(path/L"commits").size()==64,"checkpoint exists at ordinary physical boundary");ReadEquals(path,s);const auto cp=Head(path);
        codec::Record tail;tail.revision=65;tail.owner=s.StoreId();tail.snapshot=Ref(cp.payload,48);tail.previous=cp.info;
        const auto wire=Golden(tail);codec::Record parsed;Check(codec::Decode(wire.data(),wire.size(),parsed)==codec::Result::Ok&&Equal(parsed,tail),"injected tail is format-valid and contiguous with exact CP identity");
        {auto heads=Open(path/L"commits",false);Create(heads,Spec(true,NamespaceOwner()),wire);}
        Check(Names(path/L"commits").size()==65&&Head(path).info.spec.kind==files::Kind::CatalogCommit&&PhysicalBytes(path)<64ull*1024*1024,"only the ordinary active head consumes physical reserved slot");
        RejectDamaged(path,s);
    }
}
// Explicit trace mode: its own exclusive root, bounded calibration in a child
// directory, then the unchanged maintenance suite. Only the real Store::Commit
// calls are armed for observation; failure evidence is printed, never retried.
int TraceStore(int argc,wchar_t** argv){
    if(argc!=3||std::wcscmp(argv[1],L"--trace-store")!=0){
        std::fprintf(stderr,"FAIL trace-store usage: --trace-store NEW_ABSOLUTE_NORMALIZED_NONROOT_DIRECTORY\n");return 2;
    }
    const fs::path root(argv[2]);
    if(!(root.is_absolute()&&root.native().size()>3&&root!=root.root_path()&&root==root.lexically_normal())){
        std::fprintf(stderr,"FAIL trace-store rejects a non-exact absolute normalized non-root directory before opening it\n");return 2;
    }
    traceMode=true;
    NewDirectory(root);
    {
        Imports imports;imports.Verify();
        TraceCalibrate(root/L"trace-controls");
        CodecChecks();BoundaryAndLeases(root);ThousandCommits(root);ByteReserve(root);CorruptionAndForks(root);SourceAndReserveChecks(root);
        imports.Verify();
    }
    Check(!restoreFailed&&activeTrace==nullptr,"all own IAT slots and page protections are restored after trace scope");
    PrintTraceSummary();
    std::printf("PASS cache catalog maintenance trace checks=%u evidence=%ls\n",checks,root.c_str());
    return 0;
}
}
std::string Hex(const std::uint8_t* data,std::size_t size){
    static const char digits[]="0123456789abcdef";std::string out;out.reserve(size*2);
    for(std::size_t i=0;i<size;++i){out.push_back(digits[data[i]>>4]);out.push_back(digits[data[i]&0x0f]);}return out;
}
// Read-only inspection of an existing catalog. Only the production pinned reader
// runs: no bootstrap, planning, publication, recovery, collection, cleanup or
// retry, and no path or file is created. Missing locks/directories come back as
// production failures. SetLastError/errno are fresh per-call observations here;
// they do not promise that the production API preserves any original OS error.
int InspectExisting(int argc,wchar_t** argv){
    if(argc!=3||std::wcscmp(argv[1],L"--inspect-existing")!=0){
        std::fprintf(stderr,"FAIL inspect-existing usage: --inspect-existing ABSOLUTE_NORMALIZED_NONROOT_DIRECTORY\n");return 2;
    }
    const fs::path path(argv[2]);
    if(!(path.is_absolute()&&path.native().size()>3&&path!=path.root_path()&&path==path.lexically_normal())){
        std::fprintf(stderr,"FAIL inspect-existing rejects a non-exact absolute normalized non-root directory before opening it\n");return 2;
    }
    catalog::Snapshot snapshot;files::ReadLease lease;catalog::Store store(path);
    // Bind before clearing the observation errors so only the pinned read
    // contributes to the saved win32/errno pair.
    SetLastError(ERROR_SUCCESS);errno=0;
    const auto result=store.ReadPinnedExistingLocks(snapshot,lease);
    const DWORD win32=GetLastError();const int savedErrno=errno;
    const bool valid=result==Result::Ok&&snapshot.IsValid()&&lease.IsValid();
    const auto& payload=snapshot.Bytes();const std::size_t shown=std::min<std::size_t>(payload.size(),64);
    // lease stays alive until the whole line is written, so the verified current
    // snapshot cannot be retired by production collection during output.
    std::printf("INSPECT result=%d win32=%lu errno=%d revision=%llu valid=%d bytes=%zu owner=%s truncated=%d payload=%s\n",
        static_cast<int>(result),static_cast<unsigned long>(win32),savedErrno,
        static_cast<unsigned long long>(snapshot.Revision()),int(valid),payload.size(),
        Hex(snapshot.StoreId().data(),snapshot.StoreId().size()).c_str(),int(shown!=payload.size()),
        Hex(payload.data(),shown).c_str());
    return valid?0:1;
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        // Read-only inspection branches before --store fixture creation; it never
        // creates or mutates the inspected directory.
        if(argc>=2&&std::wcsncmp(argv[1],L"--inspect-existing",18)==0)return InspectExisting(argc,argv);
        // Trace mode creates its own exclusive root and calibrates only this
        // EXE's five file imports before running the unchanged suite.
        if(argc>=2&&std::wcsncmp(argv[1],L"--trace-store",13)==0)return TraceStore(argc,argv);
        Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage: --store NEW_ABSOLUTE_DIRECTORY");const fs::path root(argv[2]);NewDirectory(root);
        CodecChecks();BoundaryAndLeases(root);ThousandCommits(root);ByteReserve(root);CorruptionAndForks(root);SourceAndReserveChecks(root);
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\nCPU filesystem and codec checks; no hard interruption, cross-process leases or power-loss claim. Namespace absence is not physical free-space evidence.\n");
        std::printf("PASS cache catalog maintenance checks=%u evidence=%ls\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){if(traceMode)PrintTraceSummary();std::fprintf(stderr,"FAIL cache catalog maintenance checks=%u: %s\n",checks,e.what());return 1;}
      catch(...){if(traceMode)PrintTraceSummary();std::fprintf(stderr,"FAIL cache catalog maintenance unexpected exception\n");return 2;}
}
