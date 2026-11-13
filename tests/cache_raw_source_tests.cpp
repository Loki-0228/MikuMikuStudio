// MAINTENANCE: regression-keep
// Actual CPU/Win32 raw sources in a newly claimed fixture directory. No product
// startup, GUI, GPU, user-media writes, recursive cleanup or cache ownership grant.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <MinHook.h>
#include "mikumikustudio/cache_files.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace allocation {
thread_local long remaining=-1;
thread_local unsigned hits=0;
struct Fail{explicit Fail(long n){hits=0;remaining=n;}~Fail(){remaining=-1;}};
}
void* operator new(std::size_t n){
    if(allocation::remaining==0){++allocation::hits;throw std::bad_alloc();}
    if(allocation::remaining>0)--allocation::remaining;
    if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
using files::Result;
using Bytes=std::vector<std::uint8_t>;
namespace {
unsigned checks=0,skips=0;
void Check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
struct Handle{
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
};
void NewDirectory(const fs::path& p){
    Check(p.is_absolute()&&p.has_filename()&&p!=p.root_path()&&p==p.lexically_normal(),"require absolute normalized non-root fixture path");
    Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"exclusively claim new fixture directory");
}
void Write(const fs::path& p,const Bytes& bytes,DWORD mode=CREATE_NEW){
    Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,mode,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"open only owned fixture file for writing");DWORD done=0;
    Check(bytes.size()<=MAXDWORD&&WriteFile(h.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size(),"write all owned fixture bytes");
    Check(SetEndOfFile(h.value)!=FALSE,"truncate owned fixture exactly");
}
Bytes Read(const fs::path& p){
    std::ifstream f(p,std::ios::binary);Check(bool(f),"open real raw file reader");
    Bytes bytes{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};Check(!f.bad(),"read real raw file completely");return bytes;
}
std::uint64_t Length(const fs::path& p){
    WIN32_FILE_ATTRIBUTE_DATA data{};Check(GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&data)!=FALSE,"query actual raw size");
    return (std::uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
}
bool Same(const files::RawSourceInfo& a,const files::RawSourceInfo& b){
    return a.name==b.name&&a.payloadBytes==b.payloadBytes&&a.payloadDigest==b.payloadDigest&&
        a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;
}
bool Same(const files::RawSourcePlan& a,const files::RawSourcePlan& b){
    return a.name==b.name&&a.sourceBytes==b.sourceBytes&&a.createdIdentity.volume==b.createdIdentity.volume&&
        a.createdIdentity.fileIndex==b.createdIdentity.fileIndex&&a.createdIdentity.fileBytes==b.createdIdentity.fileBytes;
}
files::Directory Open(const fs::path& p){files::Directory d;Check(files::Directory::Open(p,true,d)==Result::Ok,"open pinned raw directory");return d;}
Result Prepare(files::Directory& d,const wchar_t* path,std::uint64_t size,files::RawSourceWrite& write){
    files::RawSourceCleanup cleanup;const auto result=d.PrepareRawSource(path,size,write,cleanup);
    Check(!cleanup.IsValid(),"ordinary Prepare returns no unconsumed cleanup receipt");return result;
}
files::RawSourceLease Capture(files::Directory& d,const fs::path& source,std::uint64_t size){
    files::RawSourceWrite write;files::RawSourceLease lease;
    Check(Prepare(d,source.c_str(),size,write)==Result::Ok&&write.IsValid(),"prepare actual raw source");
    Check(write.Complete(lease)==Result::Ok&&!write.IsValid()&&lease.IsValid(),"complete actual raw source and consume write");return lease;
}
void Removed(files::Directory& d,const files::RawSourceInfo& info,const fs::path& root){
    Check(d.RemoveRawSource(info)==Result::DeletePending,"same-identity raw disposition succeeds after leases close");
    Check(GetFileAttributesW((root/info.name).c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,
        "raw name is absent after accepted disposition and handle close");
}

enum class Mode{Read,Write,Flush,Reopen,Cancel,IdentityOnly,IdentityPersistent,IdentityAndDisposition,Disposition};
struct Fault{
    Mode mode;std::wstring source,destination;std::atomic_bool* cancel=nullptr;
    unsigned hits=0,writes=0,identityFaults=0,dispositionFaults=0,dispositionCalls=0;DWORD thread=GetCurrentThreadId();
    wchar_t createdPath[32768]{};HANDLE created=INVALID_HANDLE_VALUE;
    bool Matches(HANDLE h,const std::wstring& path)const noexcept{
        if(GetCurrentThreadId()!=thread)return false;
        wchar_t buffer[32768]{};const DWORD n=GetFinalPathNameByHandleW(h,buffer,32768,VOLUME_NAME_GUID);
        return n&&n<32768&&path.size()==n&&!_wcsicmp(buffer,path.c_str());
    }
};
thread_local Fault* active=nullptr;
decltype(&ReadFile) realRead=nullptr;
decltype(&WriteFile) realWrite=nullptr;
decltype(&FlushFileBuffers) realFlush=nullptr;
decltype(&CreateFileW) realCreate=nullptr;
decltype(&GetFileInformationByHandle) realIdentity=nullptr;
decltype(&SetFileInformationByHandle) realInformation=nullptr;
BOOL WINAPI ReadHook(HANDLE h,LPVOID data,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{
    auto* f=active;if(f&&f->mode==Mode::Read&&f->Matches(h,f->source)){
        ++f->hits;if(done)*done=0;SetLastError(ERROR_READ_FAULT);return FALSE;}
    return realRead(h,data,count,done,overlap);
}
BOOL WINAPI WriteHook(HANDLE h,LPCVOID data,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{
    auto* f=active;if(!f||!f->Matches(h,f->destination))return realWrite(h,data,count,done,overlap);
    ++f->writes;if(f->mode==Mode::Write){++f->hits;if(done)*done=0;SetLastError(ERROR_WRITE_FAULT);return FALSE;}
    const auto result=realWrite(h,data,count,done,overlap);
    if(result&&f->mode==Mode::Cancel&&f->writes==1){++f->hits;f->cancel->store(true);}
    return result;
}
BOOL WINAPI FlushHook(HANDLE h)noexcept{
    auto* f=active;if(f&&f->mode==Mode::Flush&&f->Matches(h,f->destination)){
        ++f->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return realFlush(h);
}
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templateFile)noexcept{
    auto* f=active;if(f&&f->mode==Mode::Reopen&&GetCurrentThreadId()==f->thread&&path&&
        creation==OPEN_EXISTING&&(access&GENERIC_READ)&&!_wcsicmp(path,f->destination.c_str())){
        ++f->hits;SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}
    const auto result=realCreate(path,access,share,security,creation,flags,templateFile);
    if(f&&(f->mode==Mode::IdentityOnly||f->mode==Mode::IdentityPersistent||f->mode==Mode::IdentityAndDisposition)&&GetCurrentThreadId()==f->thread&&path&&
        creation==CREATE_NEW&&result!=INVALID_HANDLE_VALUE&&f->destination.size()<32760&&
        !_wcsnicmp(path,f->destination.c_str(),f->destination.size())&&
        std::wcslen(path)>f->destination.size()+37&&std::wcsstr(path+f->destination.size(),L".raw.")){
        const size_t count=std::wcslen(path);if(count<32768){std::wmemcpy(f->createdPath,path,count+1);f->created=result;}
    }
    return result;
}
BOOL WINAPI IdentityHook(HANDLE h,LPBY_HANDLE_FILE_INFORMATION out)noexcept{
    auto* f=active;if(f&&(f->mode==Mode::IdentityOnly||f->mode==Mode::IdentityPersistent||f->mode==Mode::IdentityAndDisposition)&&
        (f->mode==Mode::IdentityPersistent||!f->identityFaults)&&GetCurrentThreadId()==f->thread&&h==f->created&&h!=INVALID_HANDLE_VALUE){
        ++f->hits;++f->identityFaults;SetLastError(ERROR_READ_FAULT);return FALSE;}
    return realIdentity(h,out);
}
BOOL WINAPI InformationHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID value,DWORD bytes)noexcept{
    auto* f=active;if(f&&GetCurrentThreadId()==f->thread&&
        h==f->created&&h!=INVALID_HANDLE_VALUE&&kind==FileDispositionInfo&&bytes==sizeof(FILE_DISPOSITION_INFO)&&value&&
        static_cast<FILE_DISPOSITION_INFO*>(value)->DeleteFile){
        ++f->dispositionCalls;
        if(f->mode==Mode::IdentityAndDisposition||f->mode==Mode::Disposition){
            ++f->hits;++f->dispositionFaults;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}}
    return realInformation(h,kind,value,bytes);
}
struct Hooks{
    Hooks(){Check(MH_Initialize()==MH_OK,"initialize process-local raw fault hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&ReadFile),reinterpret_cast<void*>(&ReadHook),reinterpret_cast<void**>(&realRead))==MH_OK,"hook raw reader");
        Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK,"hook raw writer");
        Check(MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK,"hook raw flush");
        Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"hook raw reopen");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetFileInformationByHandle),reinterpret_cast<void*>(&IdentityHook),reinterpret_cast<void**>(&realIdentity))==MH_OK,"hook only newly created raw identity failure");
        Check(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&InformationHook),reinterpret_cast<void**>(&realInformation))==MH_OK,"hook only owned raw disposition failure");
        Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable only this process's fault hooks");}
    ~Hooks(){active=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}
};
struct Arm{explicit Arm(Fault& f){Check(!active,"fault scope is nonrecursive");active=&f;}~Arm(){active=nullptr;}};
std::wstring Canonical(const fs::path& p,bool directory=false){
    Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        directory?FILE_FLAG_BACKUP_SEMANTICS:0,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"resolve owned injection path");wchar_t buffer[32768]{};
    const auto n=GetFinalPathNameByHandleW(h.value,buffer,32768,VOLUME_NAME_GUID);
    Check(n&&n<32768,"resolve actual volume path");return std::wstring(buffer,n);
}
void Basic(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto cache=root/L"basic";auto d=Open(cache);files::RawSourceWrite write;
    Check(Prepare(d,source.c_str(),bytes.size(),write)==Result::Ok,"prepare exact-limit source");
    const auto plan=write.Plan();Check(plan.sourceBytes==bytes.size()&&plan.createdIdentity.fileBytes==0&&
        plan.name.size()==40&&plan.name.substr(32)==L".raw.avi"&&Length(cache/plan.name)==0,"prepare publishes zero-file plan with lowercase original extension");
    Check(std::all_of(plan.name.begin(),plan.name.begin()+32,[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"raw generation has opaque lowercase hex name");
    {Handle writer(CreateFileW(source.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Check(writer.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"prepare pins source against writing");}
    Check(!DeleteFileW(source.c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,"prepare pins source against deletion");
    files::RawSourceLease lease;Check(write.Complete(lease)==Result::Ok&&!write.IsValid(),"complete consumes write");
    const auto info=lease.Info();Check(info.payloadBytes==bytes.size()&&info.identity.fileBytes==bytes.size()&&
        info.name==plan.name&&info.identity.fileIndex==plan.createdIdentity.fileIndex&&Read(lease.Path())==bytes,
        "raw bytes have no cache envelope and retain created file identity");
    Check(lease.Path().find(L"\\\\?\\Volume{")==0,"decoder path uses pinned volume GUID namespace");
    Write(source,Bytes{2,4,6},CREATE_ALWAYS);Check(DeleteFileW(source.c_str())!=FALSE,"source can be overwritten then deleted after capture");
    Check(Read(lease.Path())==bytes,"captured bytes do not depend on deleted original source");
    files::RawSourceLease second;Check(d.AcquireRawSource(info,second)==Result::Ok&&Same(second.Info(),info),"independent lease verifies same generation");
    {Handle reader(CreateFileW(lease.Path().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Check(reader.value!=INVALID_HANDLE_VALUE,"external decoder-style read handle coexists with source leases");}
    {Handle writer(CreateFileW(lease.Path().c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Check(writer.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"raw immutable lease denies write");}
    Check(!DeleteFileW(lease.Path().c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,"raw immutable lease denies delete");
    Check(d.RemoveRawSource(info)==Result::Busy,"removal refuses live leases");
    files::RawSourceLease moved(std::move(lease));Check(!lease.IsValid()&&Same(moved.Info(),info),"move transfers exact source lease");
    second={};d={};
    Check(!MoveFileExW(cache.c_str(),(root/L"renamed-basic").c_str(),0),"lease keeps directory guards alive after Directory destruction");
    Check(Read(moved.Path())==bytes,"moved lease remains readable after directory owner destruction");
    {Handle decoder(CreateFileW(moved.Path().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
        Check(decoder.value!=INVALID_HANDLE_VALUE,"open independent decoder reader before final lease release");
        moved={};d=Open(cache);Check(d.RemoveRawSource(info)==Result::Busy,
            "even a delete-sharing external reader prevents exclusive source reclamation");}
    Removed(d,info,cache);Write(source,bytes);
}
void InvalidInputs(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto cache=root/L"invalid";auto d=Open(cache);files::RawSourceWrite retained;
    Check(Prepare(d,source.c_str(),bytes.size(),retained)==Result::Ok,"retain valid write output for failed prepares");const auto original=retained.Plan();
    const auto empty=root/L"empty.avi",noext=root/L"noextension",unsafe=root/L"unsafe.a-b",longext=root/L"long.abcdefghijklmnopq",folder=root/L"folder.avi";
    Write(empty,{});Write(noext,{1});Write(unsafe,{1});Write(longext,{1});NewDirectory(folder);
    for(const auto& path:{empty,noext,unsafe,longext,folder}){
        Check(Prepare(d,path.c_str(),bytes.size(),retained)==Result::Invalid&&Same(retained.Plan(),original),"invalid source preserves prior prepared output");}
    Check(Prepare(d,nullptr,bytes.size(),retained)==Result::Invalid&&Same(retained.Plan(),original),"null source cannot replace write output");
    Check(Prepare(d,source.c_str(),bytes.size()-1,retained)==Result::Limit&&Same(retained.Plan(),original),"source cap refusal preserves plan");
    Check(retained.Abort()==Result::DeletePending&&!retained.IsValid()&&!fs::exists(cache/original.name),"abort removes prepared zero file via owned identity");
    auto lease=Capture(d,source,bytes.size());const auto info=lease.Info();
    for(unsigned kind=0;kind!=4;++kind){auto bad=info;
        if(kind==0)++bad.identity.fileIndex;if(kind==1)++bad.payloadBytes;if(kind==2)bad.payloadDigest[0]^=1;if(kind==3)++bad.identity.fileBytes;
        files::RawSourceLease output;Check(d.AcquireRawSource(info,output)==Result::Ok,"prepare acquire output sentinel");
        Check(d.AcquireRawSource(bad,output)!=Result::Ok&&Same(output.Info(),info),"bad identity length or digest preserves acquire output");
        output={};lease={};Check(d.RemoveRawSource(bad)!=Result::DeletePending&&Read(cache/info.name)==bytes,"bad identity length or digest never deletes raw file");
        Check(d.AcquireRawSource(info,lease)==Result::Ok,"correct raw metadata remains usable after rejected operations");}
    auto foreign=info;foreign.name=L"user.avi";Write(cache/foreign.name,bytes);lease={};
    Check(d.RemoveRawSource(foreign)==Result::Invalid&&Read(cache/foreign.name)==bytes,"ordinary user filename cannot be removed by raw primitive");
    Removed(d,info,cache);
}
void Failures(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto cache=root/L"failures";auto d=Open(cache);auto sentinel=Capture(d,source,bytes.size());const auto sentinelInfo=sentinel.Info();
    const auto canonicalSource=Canonical(source),canonicalRoot=Canonical(cache,true);
    for(auto mode:{Mode::Read,Mode::Write,Mode::Flush,Mode::Reopen,Mode::Cancel})for(bool abort:{false,true}){
        files::RawSourceWrite write;Check(Prepare(d,source.c_str(),bytes.size(),write)==Result::Ok,"prepare fault candidate");const auto plan=write.Plan();
        std::atomic_bool cancelled{false};Fault fault{mode,canonicalSource,canonicalRoot+L"\\"+plan.name,&cancelled};Result result;
        {Arm arm(fault);result=write.Complete(sentinel,&cancelled);}
        Check(fault.hits>0&&result==(mode==Mode::Cancel?Result::Cancelled:Result::Io)&&write.IsValid()&&Same(write.Plan(),plan)&&Same(sentinel.Info(),sentinelInfo),
            "real read write flush reopen or mid-copy cancel preserves output and retryable plan");
        {Handle writer(CreateFileW(source.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            Check(writer.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"failed completion retains original-source read pin for safe retry");}
        if(mode==Mode::Cancel)Check(fault.writes==1&&Length(cache/plan.name)>0&&Length(cache/plan.name)<bytes.size(),"mid-copy cancellation follows actual partial disk write");
        if(abort){Check(write.Abort()==Result::DeletePending&&!write.IsValid()&&!fs::exists(cache/plan.name),"failed candidate can abort without leaking its file");}
        else {cancelled=false;files::RawSourceLease completed;Check(write.Complete(completed,&cancelled)==Result::Ok&&Read(completed.Path())==bytes,
                "failed candidate retries full real copy from byte zero");const auto info=completed.Info();completed={};Removed(d,info,cache);}
    }
    files::RawSourceWrite write;Check(Prepare(d,source.c_str(),bytes.size(),write)==Result::Ok,"prepare pre-cancelled candidate");const auto plan=write.Plan();
    std::atomic_bool cancelled{true};Check(write.Complete(sentinel,&cancelled)==Result::Cancelled&&Same(sentinel.Info(),sentinelInfo)&&
        Same(write.Plan(),plan)&&Length(cache/plan.name)==0,"cancel before copying keeps zero candidate and output unchanged");
    Check(write.Abort()==Result::DeletePending,"abort pre-cancelled candidate");
    const auto info=sentinel.Info();sentinel={};Removed(d,info,cache);
}
void CleanupReceipts(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto cache=root/L"cleanup-receipts";auto d=Open(cache);files::RawSourceWrite prior;
    Check(Prepare(d,source.c_str(),bytes.size(),prior)==Result::Ok,"prepare unchanged output for created-file cleanup failures");
    const auto original=prior.Plan();const auto canonicalRoot=Canonical(cache,true)+L"\\";
    for(auto mode:{Mode::IdentityOnly,Mode::IdentityPersistent,Mode::IdentityAndDisposition}){
        std::vector<std::wstring> before,after;Check(d.EnumerateAll(100,before)==Result::Ok,"snapshot owned directory before faulted prepare");
        files::RawSourceCleanup cleanup;Fault fault{mode,{},canonicalRoot};Result result;
        {Arm arm(fault);result=d.PrepareRawSource(source.c_str(),bytes.size(),prior,cleanup);}
        const bool persistent=mode==Mode::IdentityPersistent;
        Check(fault.identityFaults==(persistent?2u:1u)&&fault.created!=INVALID_HANDLE_VALUE&&fault.createdPath[0]&&Same(prior.Plan(),original),
            "newly created raw identity fault leaves prior write unchanged and captures exact same handle");
        const auto name=fs::path(fault.createdPath).filename().wstring();
        Check(d.EnumerateAll(100,after)==Result::Ok,"inspect exact directory after failed identity publication");
        if(mode==Mode::IdentityOnly){
            Check(result==Result::Io&&fault.hits==1&&fault.dispositionCalls==1&&!fault.dispositionFaults&&
                !cleanup.IsValid()&&before==after&&!fs::exists(cache/name),
                "one identity failure permits verified same-handle cleanup and leaves no file or receipt");continue;
        }
        Check(result==Result::CleanupRequired&&fault.hits==2&&fault.dispositionFaults==(persistent?0u:1u)&&
            fault.dispositionCalls==(persistent?0u:1u)&&cleanup.IsValid()&&cleanup.Name()==name&&
            after.size()==before.size()+1&&std::count(after.begin(),after.end(),name)==1&&Length(cache/name)==0,
            "persistent identity failure avoids disposition; actual double failure reaches disposition; both retain the exact zero-file receipt");
        for(const auto& existing:before)Check(std::count(after.begin(),after.end(),existing)==1,"double failure preserves every pre-existing owned file");
        {Handle outsider(CreateFileW((cache/name).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,0,nullptr));
            Check(outsider.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"cleanup receipt keeps the exact newly created file exclusively pinned");}
        Check(!DeleteFileW((cache/name).c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,"outsider cannot delete retained cleanup file");
        Check(!MoveFileExW((cache/name).c_str(),(cache/L"replacement.avi").c_str(),0)&&fs::exists(cache/name),
            "outsider cannot replace the cleanup identity through renaming");
        Check(d.PrepareRawSource(source.c_str(),bytes.size(),prior,cleanup)==Result::Busy&&Same(prior.Plan(),original)&&
            cleanup.IsValid()&&cleanup.Name()==name,"occupied cleanup receipt refuses another prepare without overwriting either output");
        files::RawSourceCleanup moved(std::move(cleanup));
        Check(!cleanup.IsValid()&&moved.IsValid()&&moved.Name()==name,"move transfers cleanup authority without reopening by path");
        if(persistent){
            {Arm arm(fault);result=moved.Abort();}
            Check(result!=Result::DeletePending&&fault.identityFaults==3&&fault.hits==3&&
                !fault.dispositionCalls&&!fault.dispositionFaults&&moved.IsValid()&&moved.Name()==name&&Length(cache/name)==0,
                "persistent unreadable identity retains receipt on explicit retry without ever attempting deletion");
        }
        fault.mode=Mode::Disposition;const auto failures=fault.dispositionFaults;
        const auto calls=fault.dispositionCalls,identities=fault.identityFaults,hits=fault.hits;
        {Arm arm(fault);result=moved.Abort();}
        Check(result!=Result::DeletePending&&fault.dispositionFaults==failures+1&&fault.dispositionCalls==calls+1&&
            fault.identityFaults==identities&&fault.hits==hits+1&&Same(prior.Plan(),original)&&moved.IsValid()&&moved.Name()==name&&Length(cache/name)==0,
            "repeated disposition failure preserves receipt and tracked file for later retry");
        Check(moved.Abort()==Result::DeletePending&&!moved.IsValid()&&!fs::exists(cache/name),
            "cleanup retry deletes through retained handle then consumes receipt");
        Check(d.EnumerateAll(100,after)==Result::Ok&&before==after,"successful receipt cleanup restores exact original namespace");
    }
    Check(prior.Abort()==Result::DeletePending&&!prior.IsValid(),"explicitly abort original preserved candidate");
}
void CleanupStreams(const fs::path& root,const fs::path& source,const Bytes& bytes){
    wchar_t mount[32768]{};DWORD flags=0;
    Check(GetVolumePathNameW(root.c_str(),mount,32768)&&GetVolumeInformationW(mount,nullptr,0,nullptr,nullptr,&flags,nullptr,0),
        "query actual named-stream support for raw cleanup coverage");
    if(!(flags&FILE_NAMED_STREAMS)){++skips;std::printf("SKIP raw cleanup ADS: volume has no named streams\n");return;}
    const auto cache=root/L"cleanup-streams";auto d=Open(cache);files::DirectoryIdentity directory;
    Check(d.GetIdentity(directory)==Result::Ok,"bind cleanup ADS fixture to actual directory identity");
    for(bool captured:{false,true})for(bool empty:{false,true}){
        files::RawSourcePlan plan;files::RawSourceInfo info;std::wstring name;
        if(captured){auto lease=Capture(d,source,bytes.size());info=lease.Info();name=info.name;}
        else {files::RawSourceWrite write;Check(Prepare(d,source.c_str(),bytes.size(),write)==Result::Ok,"create actual zero candidate before recovery ADS");plan=write.Plan();name=plan.name;}
        const auto base=cache/name,ads=fs::path(base.native()+L":foreign-note");
        const Bytes extra=empty?Bytes{}:Bytes{3,9,27,81};Write(ads,extra);
        files::RawSourceCleanup cleanup;
        const auto recover=[&]{return captured?d.RecoverRawSourceCleanup(directory,info,cleanup):d.RecoverRawSourceCleanup(directory,plan,cleanup);};
        Check(recover()==Result::Ok&&cleanup.IsValid()&&cleanup.Name()==name,"acquire exact default-stream recovery receipt without adopting ADS");
        files::FileObservation observed;Check(cleanup.Observe(observed)==Result::Ok&&observed.streamCount>1&&
            observed.streamLogicalBytes==(captured?bytes.size():0)+extra.size(),"same retained handle observes default data and actual foreign ADS including empty stream");
        SetLastError(ERROR_SUCCESS);const auto removed=cleanup.Abort();const auto error=GetLastError();
        Check(removed==Result::Io&&error==ERROR_INVALID_DATA&&cleanup.IsValid()&&cleanup.Name()==name&&
            Length(base)==(captured?bytes.size():0),"raw cleanup refuses empty or populated ADS with explicit error and retains its receipt and file");
        // Relinquish the exclusive receipt before removing our own fixture ADS;
        // no product recovery path is permitted to remove it on our behalf.
        cleanup={};Check(Read(base)==(captured?bytes:Bytes{})&&Read(ads)==extra,"failed cleanup preserved complete default bytes and exact foreign stream");
        Check(DeleteFileW(ads.c_str())!=FALSE,"test owner removes only its own ADS");
        Check(recover()==Result::Ok&&cleanup.Abort()==Result::DeletePending&&!cleanup.IsValid()&&!fs::exists(base),
            "same trusted plan or captured identity cleans up after ADS owner explicitly resolves the obstruction");
    }
}
void AllocationChecks(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto cache=root/L"allocations";auto d=Open(cache);auto sentinel=Capture(d,source,bytes.size());const auto original=sentinel.Info();
    files::RawSourceWrite write;Check(Prepare(d,source.c_str(),bytes.size(),write)==Result::Ok,"prepare allocation-sweep candidate");const auto plan=write.Plan();Result result;
    files::RawSourceCleanup cleanup;
    {allocation::Fail failure(0);result=d.PrepareRawSource(source.c_str(),bytes.size(),write,cleanup);}
    Check(allocation::hits&&result!=Result::Ok&&Same(write.Plan(),plan)&&!cleanup.IsValid(),"Prepare allocation refusal preserves prior write without cleanup debt");
    // Prepare deliberately preallocates the first pair of hashers. A genuine
    // interrupted copy consumes them; retry is the path which must reallocate.
    Fault interrupted{Mode::Read,Canonical(source),Canonical(cache,true)+L"\\"+plan.name};
    {Arm arm(interrupted);result=write.Complete(sentinel);}
    Check(interrupted.hits&&result==Result::Io&&Same(write.Plan(),plan)&&Same(sentinel.Info(),original),
        "real interrupted copy reaches retry allocation boundary without replacing output");
    bool success=false;unsigned refusals=0;
    for(long index=0;index!=512;++index){
        {allocation::Fail failure(index);result=write.Complete(sentinel);}
        if(result==Result::Ok){Check(!allocation::hits&&!write.IsValid()&&Read(sentinel.Path())==bytes,"allocation sweep reaches actual uninjected Complete success");success=true;break;}
        ++refusals;Check(allocation::hits&&write.IsValid()&&Same(write.Plan(),plan)&&Same(sentinel.Info(),original),"Complete allocation failure retains identity output and retry plan");
    }
    Check(success&&refusals>0,"bounded Complete sweep covers refusal and success");
    const auto info=sentinel.Info();
    {allocation::Fail failure(0);result=d.AcquireRawSource(info,sentinel);}
    Check(allocation::hits&&result!=Result::Ok&&Same(sentinel.Info(),info),"Acquire allocation failure preserves existing source lease");
    sentinel={};Removed(d,info,cache);Removed(d,original,cache);
}
void Concurrency(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto cache=root/L"concurrent";auto d=Open(cache);files::RawSourceLease leases[2];Result results[2]{Result::Invalid,Result::Invalid};
    std::atomic_bool noCleanup{true};
    std::thread workers[2];for(unsigned i=0;i!=2;++i)workers[i]=std::thread([&,i]{files::RawSourceWrite write;files::RawSourceCleanup cleanup;
        results[i]=d.PrepareRawSource(source.c_str(),bytes.size(),write,cleanup);if(cleanup.IsValid())noCleanup=false;
        if(results[i]==Result::Ok)results[i]=write.Complete(leases[i]);});
    for(auto& worker:workers)worker.join();
    Check(noCleanup.load()&&results[0]==Result::Ok&&results[1]==Result::Ok&&leases[0].Info().name!=leases[1].Info().name&&
        leases[0].Info().identity.fileIndex!=leases[1].Info().identity.fileIndex,"concurrent same-source capture creates independent real generations");
    std::atomic_bool correct{true};for(unsigned i=0;i!=2;++i)workers[i]=std::thread([&,i]{try{
        std::ifstream input(fs::path(leases[i].Path()),std::ios::binary);Bytes got{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        if(!input||got!=bytes)correct=false;}catch(...){correct=false;}});
    for(auto& worker:workers)worker.join();Check(correct.load(),"independent decoder-style readers read concurrently");
    for(auto& lease:leases){const auto info=lease.Info();lease={};Removed(d,info,cache);}
}
void Put(Bytes& bytes,size_t offset,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[offset+i]=std::uint8_t(value>>(i*8));}
void Reparse(const fs::path& root,const fs::path& source,const Bytes& bytes){
    const auto target=root/L"junction-target",link=root/L"junction.avi";NewDirectory(target);NewDirectory(link);
    const auto substitute=L"\\??\\"+target.wstring(),print=target.wstring();const auto subBytes=substitute.size()*2,printBytes=print.size()*2;
    Bytes data(16+subBytes+2+printBytes+2);Put(data,0,IO_REPARSE_TAG_MOUNT_POINT,4);Put(data,4,data.size()-8,2);
    Put(data,10,subBytes,2);Put(data,12,subBytes+2,2);Put(data,14,printBytes,2);
    std::memcpy(data.data()+16,substitute.data(),subBytes);std::memcpy(data.data()+18+subBytes,print.data(),printBytes);
    {Handle h(CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));DWORD done=0;
        Check(h.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(h.value,FSCTL_SET_REPARSE_POINT,data.data(),DWORD(data.size()),nullptr,0,&done,nullptr),"create owned directory junction for real reparse rejection");}
    files::Directory refused;Check(files::Directory::Open(link,false,refused)==Result::Invalid&&!refused.IsValid(),"raw directory roots reject actual junctions");
    Check(files::Directory::Open(link/L"child",true,refused)==Result::Invalid&&!fs::exists(target/L"child"),"reparse ancestor never redirects child creation");
    auto d=Open(root/L"reparse-cache");files::RawSourceWrite write;
    Check(Prepare(d,link.c_str(),bytes.size(),write)==Result::Invalid&&!write.IsValid(),"directory reparse source is rejected");
    Check(RemoveDirectoryW(link.c_str())!=FALSE&&fs::is_directory(target),"remove only own junction without deleting its target");
    const auto fileLink=root/L"file-link.avi";
    if(CreateSymbolicLinkW(fileLink.c_str(),source.c_str(),2)||CreateSymbolicLinkW(fileLink.c_str(),source.c_str(),0)){
        Check(Prepare(d,fileLink.c_str(),bytes.size(),write)==Result::Invalid&&!write.IsValid(),"file reparse source is rejected");
        Check(DeleteFileW(fileLink.c_str())!=FALSE&&Read(source)==bytes,"remove only own file symlink without modifying target");
    }else{++skips;std::printf("SKIP file symlink creation unavailable error=%lu; junction rejection still tested\n",GetLastError());}
    // Establish adversarial names before pinning their ancestors. Keeping d
    // alive here would test protection of the setup operation, not capture.
    Check(!write.IsValid(),"source hardlink setup has no prepared writer");d={};
    const auto alias=root/L"hardlink.avi";
    const bool sourceLinked=CreateHardLinkW(alias.c_str(),source.c_str(),nullptr)!=FALSE;
    const DWORD sourceLinkError=sourceLinked?ERROR_SUCCESS:GetLastError();
    if(!sourceLinked)std::fprintf(stderr,"source hardlink fixture error=%lu\n",sourceLinkError);
    Check(sourceLinked,"create owned source hardlink fixture before directory pin");
    d=Open(root/L"reparse-cache");
    Check(Prepare(d,source.c_str(),bytes.size(),write)==Result::Ok&&write.IsValid(),"source capture accepts legitimate multiple-link input");
    for(const auto& path:{source,alias}){
        {Handle writer(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            Check(writer.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"source read pin blocks writes through either hardlink");}
    }
    const bool sourceDeleted=DeleteFileW(source.c_str())!=FALSE;
    const DWORD sourceDeleteError=sourceDeleted?ERROR_SUCCESS:GetLastError();
    if(sourceDeleted||sourceDeleteError!=ERROR_SHARING_VIOLATION)
        std::fprintf(stderr,"opened source delete deleted=%u error=%lu\n",unsigned(sourceDeleted),sourceDeleteError);
    Check(!sourceDeleted&&sourceDeleteError==ERROR_SHARING_VIOLATION,"read pin blocks deletion of the actually opened source name");
    // Windows permits unlinking an unopened hardlink name while another name
    // has a read pin. The immutable bytes and open source identity must survive.
    Check(DeleteFileW(alias.c_str())!=FALSE&&!fs::exists(alias)&&Read(source)==bytes,
        "unlinking the unopened alias preserves every byte of the pinned source");
    files::RawSourceLease lease;Check(write.Complete(lease)==Result::Ok,"capture linked source into separate owned generation");
    const auto info=lease.Info();
    Check(Read(lease.Path())==bytes&&info.payloadBytes==bytes.size(),"copy through the original source handle remains complete after alias unlink");
    files::RawSourceLease verified;
    Check(d.AcquireRawSource(info,verified)==Result::Ok&&Same(verified.Info(),info)&&Read(verified.Path())==bytes,
        "independent digest-verifying acquisition confirms the post-unlink copy");
    verified={};
    Check(DeleteFileW(source.c_str())&&Read(lease.Path())==bytes,"removing the final original source name leaves captured bytes intact");
    lease={};Check(!write.IsValid(),"cache hardlink setup has consumed writer");d={};
    const auto cacheAlias=root/L"cache-alias.avi";
    const bool cacheLinked=CreateHardLinkW(cacheAlias.c_str(),(root/L"reparse-cache"/info.name).c_str(),nullptr)!=FALSE;
    const DWORD cacheLinkError=cacheLinked?ERROR_SUCCESS:GetLastError();
    if(!cacheLinked)std::fprintf(stderr,"cache hardlink fixture error=%lu\n",cacheLinkError);
    Check(cacheLinked,"create own cache hardlink fault before directory pin");
    d=Open(root/L"reparse-cache");
    Check(d.AcquireRawSource(info,lease)==Result::Corrupt&&!lease.IsValid()&&d.RemoveRawSource(info)==Result::Corrupt&&Read(cacheAlias)==bytes,
        "cache generation with an extra link cannot be acquired or removed");
    Check(DeleteFileW(cacheAlias.c_str())!=FALSE,"remove only own cache hardlink fault");
    Check(d.AcquireRawSource(info,lease)==Result::Ok&&Read(lease.Path())==bytes,"single-link cache recovers after external link is removed");
    lease={};Removed(d,info,root/L"reparse-cache");
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);NewDirectory(root);
        Bytes bytes(2*1024*1024+37);for(size_t i=0;i<bytes.size();++i)bytes[i]=std::uint8_t((i*43+17)%251);
        const auto source=root/L"source.AVI";Write(source,bytes);
        Hooks hooks;Basic(root,source,bytes);InvalidInputs(root,source,bytes);Failures(root,source,bytes);CleanupReceipts(root,source,bytes);
        CleanupStreams(root,source,bytes);AllocationChecks(root,source,bytes);Concurrency(root,source,bytes);Reparse(root,source,bytes);
        std::printf("PASS cache raw sources checks=%u skips=%u evidence=%ls\n",checks,skips,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache raw sources checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache raw sources unexpected exception\n");return 2;}
}
