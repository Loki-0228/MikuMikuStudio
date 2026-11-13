// MAINTENANCE: regression-keep
// Real isolated CPU files; no product startup, GUI, decoder or recursive cleanup.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "mikumikustudio/cache_files.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>

namespace allocation {thread_local long remaining=-1;thread_local unsigned hits=0;
struct Fail{explicit Fail(long n){hits=0;remaining=n;}~Fail(){remaining=-1;}};}
void* operator new(std::size_t n){if(allocation::remaining==0){++allocation::hits;throw std::bad_alloc();}
    if(allocation::remaining>0)--allocation::remaining;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
using files::Result;
using Bytes=std::vector<std::uint8_t>;
namespace {
unsigned checks=0,skips=0;
void Check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
struct Handle{HANDLE value;explicit Handle(HANDLE h):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;};
void NewDirectory(const fs::path& p){Check(p.is_absolute()&&p!=p.root_path()&&p==p.lexically_normal(),"absolute isolated fixture path");
    Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim brand-new fixture directory");}
void Write(const fs::path& p,const Bytes& bytes,DWORD creation=CREATE_NEW){
    Handle file(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,creation,FILE_ATTRIBUTE_NORMAL,nullptr));Check(file.value!=INVALID_HANDLE_VALUE,"open owned fixture output");
    DWORD done=0;Check(WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&SetEndOfFile(file.value),"write exact fixture bytes");}
Bytes Read(const fs::path& p){std::ifstream file(p,std::ios::binary);Check(bool(file),"open fixture read");
    Bytes bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};Check(!file.bad(),"read complete fixture");return bytes;}
bool Exists(const fs::path& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;}
files::Directory Open(const fs::path& p){files::Directory d;Check(files::Directory::Open(p,true,d)==Result::Ok,"open fixed cache directory");return d;}
files::DirectoryIdentity Root(const files::Directory& d){files::DirectoryIdentity root{};Check(d.GetIdentity(root)==Result::Ok,"read exact protected root identity");return root;}
std::wstring Canonical(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"query owned path");wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h.value,path,32768,VOLUME_NAME_GUID);
    Check(n&&n<32768,"fixed volume path");return std::wstring(path,n);}
struct Fault{std::wstring path;std::atomic_bool* cancel=nullptr;bool disposition=false;unsigned hits=0;};
thread_local Fault* fault=nullptr;
bool Matches(HANDLE h)noexcept{if(!fault)return false;wchar_t p[32768]{};const auto n=GetFinalPathNameByHandleW(h,p,32768,VOLUME_NAME_GUID);
    return n&&n<32768&&fault->path.size()==n&&!_wcsicmp(p,fault->path.c_str());}
decltype(&WriteFile) realWrite=nullptr;decltype(&SetFileInformationByHandle) realInfo=nullptr;
BOOL WINAPI WriteHook(HANDLE h,LPCVOID p,DWORD size,LPDWORD done,LPOVERLAPPED overlap)noexcept{
    const auto result=realWrite(h,p,size,done,overlap);if(result&&fault&&fault->cancel&&Matches(h)){++fault->hits;fault->cancel->store(true);}return result;}
BOOL WINAPI InfoHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID p,DWORD n)noexcept{
    if(fault&&fault->disposition&&kind==FileDispositionInfo&&Matches(h)){++fault->hits;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realInfo(h,kind,p,n);}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK,"initialize local fault hooks");
    Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK,"hook exact raw copy write");
    Check(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&InfoHook),reinterpret_cast<void**>(&realInfo))==MH_OK,"hook exact cleanup disposition");
    Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable process-local hooks");}
    ~Hooks(){fault=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm{explicit Arm(Fault& f){Check(!fault,"nonrecursive fault scope");fault=&f;}~Arm(){fault=nullptr;}};
files::RawSourceWrite Prepare(files::Directory& d,const fs::path& source,std::uint64_t size){
    files::RawSourceWrite write;files::RawSourceCleanup cleanup;
    Check(d.PrepareRawSource(source.c_str(),size,write,cleanup)==Result::Ok&&write.IsValid()&&!cleanup.IsValid(),"prepare real source and identity plan");return write;}
void Removed(files::RawSourceCleanup& cleanup,const fs::path& path){
    Check(cleanup.Abort()==Result::DeletePending&&!cleanup.IsValid(),"explicit same-handle cleanup consumes receipt");
    Check(!Exists(path),"deleted namespace confirmed after close");Check(cleanup.Abort()==Result::Invalid,"duplicate Abort is rejected");}

void Interrupted(const fs::path& base,const Bytes& bytes){
    for(unsigned stage=0;stage<3;++stage){
        const auto source=base/(L"input-"+std::to_wstring(stage)+L".avi");Write(source,bytes);
        const auto cache=base/(L"stage-"+std::to_wstring(stage));auto d=Open(cache);const auto root=Root(d);
        auto write=Prepare(d,source,bytes.size());const auto plan=write.Plan();const auto path=cache/plan.name;
        if(stage==1){std::atomic_bool cancel{false};Fault f{Canonical(path),&cancel};files::RawSourceLease output;
            Result result;{Arm arm(f);result=write.Complete(output,&cancel);}Check(result==Result::Cancelled&&f.hits==1&&!output.IsValid()&&write.IsValid()&&write.Plan().name==plan.name,"actual streaming interruption retains the cleanup plan after first successful write");}
        if(stage==2){files::RawSourceLease output;Check(write.Complete(output)==Result::Ok&&Read(output.Path())==bytes,"complete real encoded bytes before simulated process release");output={};}
        write={};const auto captured=Read(path);Check(captured.size()==(stage==0?0:stage==1?65536:bytes.size()),"zero partial and complete file lengths are real");
        Check(DeleteFileW(source.c_str())&&!Exists(source),"original source is absent before cleanup recovery");
        d={};d=Open(cache);files::RawSourceCleanup cleanup;
        Check(d.RecoverRawSourceCleanup(root,plan,cleanup)==Result::Ok&&cleanup.IsValid()&&cleanup.Name()==plan.name,"recover trusted plan independently of original source");
        {Handle writer(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            Check(writer.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"cleanup receipt pins exact file exclusively");}
        d={};Check(!MoveFileW(cache.c_str(),(base/L"moved-cache").c_str()),"cleanup receipt retains directory guards beyond Directory lifetime");
        files::RawSourceCleanup moved(std::move(cleanup));Check(!cleanup.IsValid()&&moved.IsValid()&&moved.Name()==plan.name,"move transfers cleanup responsibility once");
        Removed(moved,path);
    }
}
void Rejections(const fs::path& base,const Bytes& bytes){
    const auto source=base/L"reject-source.avi";Write(source,bytes);const auto cache=base/L"reject-cache";auto d=Open(cache);const auto root=Root(d);
    auto write=Prepare(d,source,bytes.size());const auto plan=write.Plan();const auto path=cache/plan.name;
    files::RawSourceCleanup output;
    Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Busy&&!output.IsValid(),"active prepared writer prevents recovery");write={};
    Write(path,Bytes{1,2,3},OPEN_EXISTING);
    for(unsigned i=0;i<9;++i){auto bad=plan;auto wrongRoot=root;Result expected=Result::Invalid;
        switch(i){case 0:wrongRoot.fileIndex^=1;expected=Result::Conflict;break;
        case 1:bad.createdIdentity.fileIndex^=1;expected=Result::Conflict;break;
        case 2:bad.createdIdentity.volume^=1;expected=Result::Conflict;break;
        case 3:bad.createdIdentity.fileBytes=1;break;case 4:bad.sourceBytes=0;break;
        case 5:bad.sourceBytes=UINT64_MAX;break;case 6:bad.sourceBytes=2;expected=Result::Conflict;break;
        case 7:bad.name=L"..\\foreign.avi";break;case 8:bad.name.append(100,L'a');break;}
        Check(d.RecoverRawSourceCleanup(wrongRoot,bad,output)==expected&&!output.IsValid(),"invalid root identity plan length or foreign name rejected atomically");
        Check(Read(path)==Bytes({1,2,3}),"rejected recovery does not modify or delete candidate");}
    auto missing=plan;missing.name[0]=missing.name[0]==L'0'?L'1':L'0';
    Check(d.RecoverRawSourceCleanup(root,missing,output)==Result::Missing&&!output.IsValid(),"missing planned generation is not recreated");
    auto foreign=plan;foreign.name=L"00000000000000000000000000000000.raw.avi";
    const auto foreignPath=cache/foreign.name;Write(foreignPath,Bytes{4,5,6});
    Check(d.RecoverRawSourceCleanup(root,foreign,output)==Result::Conflict&&!output.IsValid()&&Read(foreignPath)==Bytes({4,5,6}),"well-formed foreign filename does not authorize deleting a different file identity");
    {Handle reader(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));Check(reader.value!=INVALID_HANDLE_VALUE,"open independent real reader");
        Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Busy&&!output.IsValid(),"independent reader denies recovery DELETE access");}
    Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Ok,"valid receipt after readers close");const auto held=output.Name();
    auto unrelated=plan;unrelated.name=L"untrusted.avi";Check(d.RecoverRawSourceCleanup({},unrelated,output)==Result::Busy&&output.IsValid()&&output.Name()==held,"nonempty output never loses its existing cleanup responsibility");
    Fault f{Canonical(path),nullptr,true};Result failed;{Arm arm(f);failed=output.Abort();}
    Check(failed==Result::Io&&f.hits==1&&output.IsValid()&&output.Name()==held&&Exists(path),"same-handle deletion failure retains recoverable receipt");
    Removed(output,path);
    auto again=Prepare(d,source,bytes.size());const auto otherPlan=again.Plan();const auto otherPath=cache/otherPlan.name;again={};
    Check(d.RecoverRawSourceCleanup(root,otherPlan,output)==Result::Ok,"recover another actual empty generation");
    output={};Check(Exists(otherPath),"receipt destruction closes without implicitly deleting");
    Check(d.RecoverRawSourceCleanup(root,otherPlan,output)==Result::Ok,"closed receipt can be recovered again from trusted intent");Removed(output,otherPath);
}
void Links(const fs::path& base,const Bytes& bytes){
    const auto source=base/L"links-source.avi";Write(source,bytes);const auto cache=base/L"links-cache";auto d=Open(cache);const auto root=Root(d);
    auto write=Prepare(d,source,bytes.size());const auto plan=write.Plan();const auto path=cache/plan.name;write={};d={};
    const auto alias=base/L"raw-alias.avi";Check(CreateHardLinkW(alias.c_str(),path.c_str(),nullptr)!=FALSE,"create cache hardlink before pinning ancestors");d=Open(cache);
    files::RawSourceCleanup output;Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Corrupt&&!output.IsValid()&&Exists(path)&&Exists(alias),"multiple-link cache cannot become cleanup authorization");
    Check(DeleteFileW(alias.c_str())!=FALSE,"remove only owned alias");Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Ok,"single-link trusted generation recovers");Removed(output,path);d={};
    Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"owned directory occupies old raw name");d=Open(cache);
    Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Corrupt&&!output.IsValid()&&fs::is_directory(path),"directory is never accepted as raw generation");d={};Check(RemoveDirectoryW(path.c_str())!=FALSE,"remove exact empty owned directory only");
    if(CreateSymbolicLinkW(path.c_str(),source.c_str(),2)||CreateSymbolicLinkW(path.c_str(),source.c_str(),0)){
        d=Open(cache);Check(d.RecoverRawSourceCleanup(root,plan,output)==Result::Corrupt&&!output.IsValid()&&Read(source)==bytes,"reparse target is never traversed for cleanup");
        d={};Check(DeleteFileW(path.c_str())!=FALSE,"remove only owned file link");
    }else{++skips;std::printf("SKIP raw recovery file symlink unavailable error=%lu\n",GetLastError());}
}
void Allocations(const fs::path& base,const Bytes& bytes){
    const auto source=base/L"allocation-source.avi";Write(source,bytes);const auto cache=base/L"allocation-cache";auto d=Open(cache);const auto root=Root(d);
    auto write=Prepare(d,source,bytes.size());const auto plan=write.Plan();const auto path=cache/plan.name;write={};
    bool success=false;unsigned failures=0;
    for(long point=0;point<64;++point){files::RawSourceCleanup cleanup;Result result;unsigned hits;
        {allocation::Fail failure(point);result=d.RecoverRawSourceCleanup(root,plan,cleanup);hits=allocation::hits;}
        if(result==Result::Ok){Check(!hits&&cleanup.IsValid(),"recovery publication has no post-open allocation failure");cleanup={};success=true;break;}
        ++failures;Check(hits&&result==Result::Io&&!cleanup.IsValid()&&Exists(path)&&Read(path).empty(),"each allocation failure preserves output and actual candidate");
        {Handle independent(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));Check(independent.value!=INVALID_HANDLE_VALUE,"failed recovery does not leak exclusive handle");}
    }
    Check(success&&failures>=3,"sweep crosses every actual recovery allocation");files::RawSourceCleanup cleanup;
    Check(d.RecoverRawSourceCleanup(root,plan,cleanup)==Result::Ok,"recovery after allocation sweep");Removed(cleanup,path);
}
}
int wmain(int argc,wchar_t** argv){std::setvbuf(stdout,nullptr,_IONBF,0);try{
    Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIR");const fs::path base(argv[2]);NewDirectory(base);
    Bytes bytes(131109);for(size_t i=0;i<bytes.size();++i)bytes[i]=std::uint8_t((i*37+11)%251);
    Hooks hooks;Interrupted(base,bytes);Rejections(base,bytes);Links(base,bytes);Allocations(base,bytes);
    std::printf("PASS cache raw recovery checks=%u skips=%u evidence=%ls\n",checks,skips,base.c_str());return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL cache raw recovery checks=%u: %s\n",checks,e.what());return 1;}
catch(...){std::fprintf(stderr,"FAIL cache raw recovery unexpected exception\n");return 2;}}
