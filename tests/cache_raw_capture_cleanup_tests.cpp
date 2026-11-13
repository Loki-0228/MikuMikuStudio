// MAINTENANCE: regression-keep
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "mikumikustudio/cache_files.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace { thread_local bool allocationsForbidden=false; thread_local unsigned allocationFaults=0; }
void* operator new(std::size_t n){if(allocationsForbidden){++allocationFaults;throw std::bad_alloc();}if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
using files::Result;
namespace {
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAIL %s error=%lu\n",message,GetLastError());throw std::runtime_error(message);}}
struct Handle{HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
using CreateFn=HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using InformationFn=BOOL(WINAPI*)(HANDLE,FILE_INFO_BY_HANDLE_CLASS,LPVOID,DWORD);
CreateFn realCreate=nullptr;InformationFn realInformation=nullptr;
std::wstring capturedPath;
HANDLE capturedHandle=INVALID_HANDLE_VALUE;
bool forbidAfterOpen=false,failDisposition=false;
unsigned opened=0,dispositionFailures=0;
HANDLE WINAPI CreateHook(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD mode,DWORD flags,HANDLE model){
    const auto file=realCreate(name,access,share,security,mode,flags,model);
    if(name&&!capturedPath.empty()&&_wcsicmp(name,capturedPath.c_str())==0&&access==(GENERIC_READ|DELETE)&&share==0&&mode==OPEN_EXISTING&&file!=INVALID_HANDLE_VALUE){
        capturedHandle=file;++opened;if(forbidAfterOpen)allocationsForbidden=true;
    }
    return file;
}
BOOL WINAPI InformationHook(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID value,DWORD bytes){
    if(failDisposition&&file==capturedHandle&&kind==FileDispositionInfo&&value&&bytes==sizeof(FILE_DISPOSITION_INFO)&&
        static_cast<FILE_DISPOSITION_INFO*>(value)->DeleteFile){++dispositionFailures;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realInformation(file,kind,value,bytes);
}
struct Hooks{
    Hooks(){Check(MH_Initialize()==MH_OK,"initialize isolated raw cleanup fault hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK,"observe only selected recovery file open");
        Check(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&InformationHook),reinterpret_cast<void**>(&realInformation))==MH_OK,"inject retryable same-handle disposition failure");
        Check(MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"enable isolated cleanup hooks");}
    ~Hooks(){allocationsForbidden=false;forbidAfterOpen=false;failDisposition=false;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}
};
void Write(const fs::path& path,const std::vector<unsigned char>& bytes,DWORD creation=CREATE_NEW){
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,creation,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(file.value!=INVALID_HANDLE_VALUE,"open only fixture source for writing");DWORD done=0;
    Check(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&done,nullptr)&&done==bytes.size()&&SetEndOfFile(file.value),"write complete fixture payload");
}
std::vector<unsigned char> Read(const fs::path& path){std::ifstream file(path,std::ios::binary);Check(bool(file),"read preserved fixture bytes");return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
void Run(const fs::path& root){
    Check(root.is_absolute()&&root==root.lexically_normal()&&root.has_filename()&&root!=root.root_path(),"require normalized absolute fixture directory");
    Check(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"exclusively create new fixture directory");
    const auto source=root/L"original.MP4",cache=root/L"raw";
    std::vector<unsigned char> bytes(150013);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<unsigned char>((i*17+31)%251);
    Write(source,bytes);
    files::Directory directory;Check(files::Directory::Open(cache,true,directory)==Result::Ok,"open pinned cache directory");
    files::DirectoryIdentity identity{};Check(directory.GetIdentity(identity)==Result::Ok,"capture trusted root identity");
    files::RawSourceWrite prepared;files::RawSourceCleanup failedCreation;
    Check(directory.PrepareRawSource(source.c_str(),bytes.size(),prepared,failedCreation)==Result::Ok&&!failedCreation.IsValid(),"prepare actual raw generation");
    files::RawSourceLease lease;Check(prepared.Complete(lease)==Result::Ok,"complete source capture");
    const auto expected=lease.Info();capturedPath=lease.Path();const auto diskPath=cache/expected.name;
    files::RawSourceCleanup cleanup;
    Check(directory.RecoverRawSourceCleanup(identity,expected,cleanup)==Result::Busy&&!cleanup.IsValid(),"live source lease prevents exclusive cleanup capture");
    lease={};Check(DeleteFileW(source.c_str())!=FALSE,"remove original source before recovery");
    auto wrongRoot=identity;++wrongRoot.fileIndex;
    Check(directory.RecoverRawSourceCleanup(wrongRoot,expected,cleanup)==Result::Conflict&&!cleanup.IsValid(),"reject wrong root without consuming output");
    auto wrong=expected;++wrong.identity.fileIndex;
    Check(directory.RecoverRawSourceCleanup(identity,wrong,cleanup)==Result::Conflict&&!cleanup.IsValid(),"reject wrong file identity");
    wrong=expected;++wrong.payloadBytes;++wrong.identity.fileBytes;
    Check(directory.RecoverRawSourceCleanup(identity,wrong,cleanup)==Result::Conflict&&!cleanup.IsValid(),"reject exact-size mismatch");
    wrong=expected;++wrong.payloadBytes;
    Check(directory.RecoverRawSourceCleanup(identity,wrong,cleanup)==Result::Invalid&&!cleanup.IsValid(),"reject internally inconsistent captured size");
    wrong=expected;wrong.payloadDigest[0]^=1;
    Check(directory.RecoverRawSourceCleanup(identity,wrong,cleanup)==Result::Corrupt&&!cleanup.IsValid(),"reject incorrect full digest");
    wrong=expected;wrong.name=L"../foreign.raw.mp4";
    Check(directory.RecoverRawSourceCleanup(identity,wrong,cleanup)==Result::Invalid&&!cleanup.IsValid(),"reject path traversal name");
    Check(Read(diskPath)==bytes,"all failed recovery attempts preserve actual bytes");
    auto changed=bytes;changed.back()^=1;Write(diskPath,changed,OPEN_EXISTING);
    Check(directory.RecoverRawSourceCleanup(identity,expected,cleanup)==Result::Corrupt&&!cleanup.IsValid(),"full digest covers corruption past two streaming chunks");
    Check(Read(diskPath)==changed,"digest rejection does not repair or delete file");Write(diskPath,bytes,OPEN_EXISTING);
    allocationFaults=0;allocationsForbidden=true;
    const auto allocationResult=directory.RecoverRawSourceCleanup(identity,expected,cleanup);allocationsForbidden=false;
    Check(allocationFaults&&allocationResult==Result::Io&&!cleanup.IsValid()&&Read(diskPath)==bytes,"allocation failure preserves output and file");
    allocationFaults=0;forbidAfterOpen=true;const auto before=opened;
    const auto recovered=directory.RecoverRawSourceCleanup(identity,expected,cleanup);
    allocationsForbidden=false;forbidAfterOpen=false;
    Check(recovered==Result::Ok&&cleanup.IsValid()&&cleanup.Name()==expected.name&&opened==before+1&&!allocationFaults,
        "successful full verification transfers exclusive handle without post-open allocation");
    Check(fs::exists(diskPath),"recovery receipt does not itself delete namespace");
    Check(directory.RecoverRawSourceCleanup(wrongRoot,wrong,cleanup)==Result::Busy&&cleanup.Name()==expected.name,"nonempty output is Busy before validation and preserves original receipt");
    files::RawSourceLease denied;Check(directory.AcquireRawSource(expected,denied)==Result::Busy&&!denied.IsValid(),"returned cleanup retains exclusive handle");
    cleanup={};Check(Read(diskPath)==bytes,"receipt destruction only closes and never deletes");
    Check(directory.RecoverRawSourceCleanup(identity,expected,cleanup)==Result::Ok,"recover same trusted capture again");
    failDisposition=true;
    Check(cleanup.Abort()==Result::Io&&cleanup.IsValid()&&cleanup.Name()==expected.name&&dispositionFailures==1,"failed disposition retains exact handle for retry");
    failDisposition=false;
    Check(cleanup.Abort()==Result::DeletePending&&!cleanup.IsValid(),"same-handle disposition retry consumes receipt");
    Check(GetFileAttributesW(diskPath.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"namespace missing after fixture handles close, not a physical-space guarantee");
    Check(directory.RecoverRawSourceCleanup(identity,expected,cleanup)==Result::Missing&&!cleanup.IsValid(),"missing capture cannot mint cleanup ownership");

    // Capture another generation to verify that complete recovery retains the
    // single-link restriction instead of adopting an alias as ownership.
    Write(source,bytes);
    Check(directory.PrepareRawSource(source.c_str(),bytes.size(),prepared,failedCreation)==Result::Ok,"prepare hardlink-adversary generation");
    Check(prepared.Complete(lease)==Result::Ok,"complete hardlink-adversary generation");
    const auto linked=lease.Info();lease={};directory={};
    const auto alias=root/L"external-alias.mp4";
    Check(CreateHardLinkW(alias.c_str(),(cache/linked.name).c_str(),nullptr)!=FALSE,"create alias only while directory guards are released");
    Check(files::Directory::Open(cache,false,directory)==Result::Ok&&directory.GetIdentity(identity)==Result::Ok,"reopen unchanged cache root");
    Check(directory.RecoverRawSourceCleanup(identity,linked,cleanup)==Result::Corrupt&&!cleanup.IsValid(),"multiple-link capture does not receive cleanup receipt");
    Check(Read(alias)==bytes&&Read(cache/linked.name)==bytes,"hardlink rejection preserves both names");
    directory={};Check(DeleteFileW(alias.c_str())!=FALSE,"remove only fixture alias");
    Check(files::Directory::Open(cache,false,directory)==Result::Ok,"reopen single-link generation");
    Check(directory.RecoverRawSourceCleanup(identity,linked,cleanup)==Result::Ok&&cleanup.Abort()==Result::DeletePending,"single-link capture can be explicitly cleaned");
}
}
int wmain(int argc,wchar_t** argv){
    try{Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage --store NEW_ABSOLUTE_DIR");Hooks hooks;Run(fs::path(argv[2]));
        std::printf("PASS cache raw capture cleanup %u checks; real files, full digest, exclusive receipt and retryable disposition\n",checks);return 0;
    }catch(const std::exception& error){allocationsForbidden=false;std::fprintf(stderr,"FAIL cache raw capture cleanup: %s\n",error.what());return 1;}
}
