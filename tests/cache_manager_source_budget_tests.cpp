// MAINTENANCE: regression-keep. Real manager policy, Video catalog and raw files.
// Durable source credit is NOT a second quota implementation in this test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <MinHook.h>
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/cache_catalog.hpp"
#include "../src/io/cache_source_journal.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
namespace fs=std::filesystem;
namespace m=mikumikustudio::cache_manager;
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
namespace c=mikumikustudio::cache_catalog;
using J=b::JournalResult;
using Bytes=b::Bytes;
constexpr std::uint64_t MiB=1024ull*1024;
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value){std::fprintf(stderr,"DIAG check=%u win32=%lu\n",checks,GetLastError());throw std::runtime_error(why);}}
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){Close();}void Close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=INVALID_HANDLE_VALUE;}Handle(const Handle&)=delete;};
template<class T>T Id(unsigned n){T id{};id[14]=std::uint8_t(n>>8);id[15]=std::uint8_t(n);return id;}
void Fresh(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"absolute private test root");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim new test directory without replacing evidence");}
void Parents(const fs::path& p){std::error_code error;fs::create_directories(p.parent_path(),error);Check(!error,"create only fixture parents");}
void Fill(HANDLE file,std::uint64_t bytes){DWORD done=0;Check(DeviceIoControl(file,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&done,nullptr)!=FALSE,"sparse fixture does not fill real disk with zeros");LARGE_INTEGER length{};length.QuadPart=LONGLONG(bytes);Check(SetFilePointerEx(file,length,nullptr,FILE_BEGIN)&&SetEndOfFile(file)&&FlushFileBuffers(file),"set exact owned fixture stream size");}
void Sparse(const fs::path& p,std::uint64_t size){Parents(p);Handle file(CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(file.value!=INVALID_HANDLE_VALUE,"CREATE_NEW sparse source or foreign stream");Fill(file.value,size);}
void Small(const fs::path& p,const Bytes& bytes){Parents(p);Handle file(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;Check(file.value!=INVALID_HANDLE_VALUE&&WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(file.value),"write unique owned corrupt evidence");}
Bytes Read(const fs::path& p){std::ifstream file(p,std::ios::binary);Check(bool(file),"read own evidence");return Bytes(std::istreambuf_iterator<char>(file),{});}
void Select(const fs::path& root){Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",root.c_str())&&SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(root.parent_path()/L"settings.json").c_str()),"select exact isolated manager root and shared policy file");}
void Configure(const fs::path& root){Select(root);m::Settings settings;settings.limitBytes=64*MiB;settings.automaticCleanup=false;std::wstring error;Check(m::Configure(settings,error)&&m::Root()==root,"configure real minimum shared 64 MiB policy");}
fs::path Entry(const fs::path& root,unsigned id,bool effect=false){wchar_t name[65]{};swprintf_s(name,L"%064x",id);return root/(effect?L"MME":L"Textures")/L"shared"/(std::wstring(name)+(effect?L".fxb":L".mtc"));}
void Allows(const fs::path& root,unsigned id,std::uint64_t bytes,bool expected,bool effect=false){m::WriteReservation token(Entry(root,id,effect),bytes);Check(bool(token)==expected,expected?"actual existing writer receives bounded remaining capacity":"actual existing writer refuses durable-source or untrusted-area oversubscription");}

struct Fixture {
    fs::path base,root;f::Directory manager,video,control,pages,raw;std::unique_ptr<b::SourceJournal> journal;b::BusinessRoot initial;
    explicit Fixture(const fs::path& directory):base(directory),root(base/L"Cache"){Fresh(base);Configure(root);
        Check(f::Directory::Open(root,false,manager)==f::Result::Ok&&manager.Child(L"Video",true,video)==f::Result::Ok&&video.Child(L"control",true,control)==f::Result::Ok&&video.Child(L"pages",true,pages)==f::Result::Ok&&video.Child(L"raw",true,raw)==f::Result::Ok,"create real fixed Video directories in manager inventory root");
        initial.legacy.storeOwner=Id<f::Owner>(1);initial.legacy.maintenanceEpoch=1;initial.legacy.nextProjectId=2;initial.sources.storeOwner=initial.legacy.storeOwner;initial.sources.maintenanceEpoch=1;
        b::DirectoryBinding binding;binding.id=1;Check(raw.GetIdentity(binding.identity)==f::Result::Ok,"bind real raw directory identity");initial.sources.directories.push_back(binding);
        b::Page page;page.storeOwner=initial.legacy.storeOwner;b::Project project;project.id=1;project.normalizedPath=project.displayPath=u"C:\\manager-source-test\\project.pmm";page.projects.push_back(project);
        Bytes encoded;Check(b::EncodePage(page,encoded)==b::Result::Ok,"encode real published project page");f::Spec spec;spec.owner=page.storeOwner;spec.kind=f::Kind::IndexPage;spec.key=b::PageKey;f::BlobInfo blob;
        Check(pages.Create(spec,encoded.data(),encoded.size(),blob)==f::Result::Ok,"persist checksummed project page");b::PageRef reference;reference.recordCount=1;reference.blob=blob;initial.legacy.pages.push_back(reference);initial.legacy.projectCount=1;initial.legacy.liveEncodedBytes=b::ManifestHeaderBytes+b::PageRefBytes+encoded.size();
        Check(b::EncodeBusinessRoot(initial,encoded)==b::Result::Ok,"encode source root with actual project and raw references");c::Snapshot first;Check(Store().InitializeOwned(initial.legacy.storeOwner,encoded,first)==c::Result::Ok,"persist actual trusted source catalog");
        Check(b::SourceJournal::Open(control,pages,raw,journal)==J::Ok,"open genuine responsibility journal and establish existing control lock");
    }
    c::Store Store()const{return c::Store(root/L"Video"/L"control"/L"catalog");}
    b::BusinessRoot Root()const{c::Snapshot current;Check(Store().Read(current)==c::Result::Ok,"read actual persisted source root");b::BusinessRoot result;Check(b::DecodeBusinessRoot(current.Bytes().data(),current.Bytes().size(),result)==b::Result::Ok,"decode actual source operation state");return result;}
    void Commit(const b::BusinessRoot& value){c::Snapshot before,after;Bytes bytes;Check(Store().Read(before)==c::Result::Ok&&b::EncodeBusinessRoot(value,bytes)==b::Result::Ok&&Store().Commit(before,bytes,after)==c::Result::Ok,"publish validated crash-state fixture through real catalog CAS");}
    std::unique_ptr<b::SourceCapture> Capture(unsigned id,std::uint64_t bytes){std::unique_ptr<b::SourceCapture> capture;Check(journal->CreateCapture(Id<b::OperationId>(id),Id<b::SourceId>(id+100),bytes,capture)==J::Ok,"create real source capture");return capture;}
};

void Child(const wchar_t* mode,const fs::path& path){wchar_t exe[32768]{};const auto length=GetModuleFileNameW(nullptr,exe,32768);Check(length&&length<32768,"locate exact current test executable");auto command=L"\""+std::wstring(exe,length)+L"\" "+mode+L" \""+path.native()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"launch independent shared-policy reader process");Handle child(process.hProcess),thread(process.hThread);const auto waited=WaitForSingleObject(child.value,30000);if(waited!=WAIT_OBJECT_0){TerminateProcess(child.value,99);WaitForSingleObject(child.value,5000);}DWORD exit=999;Check(waited==WAIT_OBJECT_0&&GetExitCodeProcess(child.value,&exit)&&exit==0,"separate process preserves real durable source budget obligations");}
void CheckReserved(const fs::path& root){Select(root);const auto view=m::Inspect();Check(view.error.empty()&&view.totalBytes<MiB,"display reports actual metadata, not the unmaterialized forty MiB promise");Allows(root,1,25*MiB,false);Allows(root,2,16*MiB,true);Allows(root,3,25*MiB,false,true);}
void NoVideo(const fs::path& parent){const auto base=parent/L"legacy-only";Fresh(base);const auto root=base/L"Cache";Configure(root);Allows(root,1,60*MiB,true);Check(!fs::exists(root/L"Video"),"legacy writer budget does not create Video or its catalog");}
void ReservedAndRestart(const fs::path& parent){Fixture fixture(parent/L"reserved");auto capture=fixture.Capture(1,40*MiB);Check(capture->Reserve()==J::Ok,"persist forty MiB Reserved before checking legacy writers");CheckReserved(fixture.root);Child(L"--check-reserved",fixture.root);
    Check(capture->Cancel()==J::Ok,"explicitly cancel actual durable reservation");Allows(fixture.root,4,60*MiB,true);
    const auto restarted=parent/L"restarted";Child(L"--leave-reserved",restarted);CheckReserved(restarted/L"Cache");
}
void SharedWriterScope(const fs::path& parent){Fixture fixture(parent/L"writer-scope");const auto source=fixture.base/L"tiny.mp4";Small(source,{1,2,3,4,5});auto ready=fixture.Capture(10,5),fresh=fixture.Capture(11,MiB);
    Check(ready->Reserve()==J::Ok&&ready->Prepare(source.c_str())==J::Ok&&ready->Complete()==J::Ok,"prepare real Captured publication before legacy writer exclusion starts");
    auto outer=std::make_unique<m::WriteReservation>(Entry(fixture.root,1),16*MiB);Check(bool(*outer),"outer approved writer holds source verification scope");
    Check(fresh->Reserve()==J::Busy&&fresh->State()==b::CaptureState::New,"same-thread journal cannot add durable debt inside approved legacy writer scope");f::RawSourceLease output;
    Check(ready->Publish({1},1,output)==J::Busy&&!output.IsValid()&&ready->State()==b::CaptureState::Captured,"same-thread source publication cannot race approved writer and preserves capture/output");
    {f::FileLock blocked;Check(fixture.control.LockExisting(0,blocked)==f::Result::Busy&&!blocked.IsValid(),"actual control byte lock remains held for writer lifetime");}
    Child(L"--control-busy",fixture.root);auto inner=std::make_unique<m::WriteReservation>(Entry(fixture.root,2),8*MiB);Check(bool(*inner),"same-root nested writer shares established source scope rather than recursively locking control");
    outer.reset();Check(fresh->Reserve()==J::Busy&&fresh->State()==b::CaptureState::New,"inner writer retains source exclusion after outer writer destruction");Child(L"--control-busy",fixture.root);
    inner.reset();{f::FileLock released;Check(fixture.control.LockExisting(0,released)==f::Result::Ok,"last writer destruction releases real control lock");}Child(L"--control-open",fixture.root);
    Check(fresh->Reserve()==J::Ok&&fresh->Cancel()==J::Ok,"journal can reserve and cancel after shared writer scope ends");Check(ready->Publish({1},1,output)==J::Ok&&output.IsValid(),"previously blocked publication succeeds after final writer releases");
}

struct CopyFault {std::wstring prefix;std::atomic_bool cancelled{false};std::uint64_t written=0;unsigned hits=0;};
thread_local CopyFault* copyFault=nullptr;decltype(&WriteFile) realWrite=nullptr;
BOOL WINAPI WriteHook(HANDLE file,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlapped)noexcept{const auto result=realWrite(file,bytes,count,done,overlapped);const auto error=GetLastError();auto* fault=copyFault;
    if(result&&fault&&!overlapped&&done){wchar_t path[32768]{};const auto length=GetFinalPathNameByHandleW(file,path,32768,VOLUME_NAME_GUID);if(length>fault->prefix.size()&&length<32768&&!_wcsnicmp(path,fault->prefix.c_str(),fault->prefix.size())){fault->written+=*done;if(fault->written>=24*MiB&&!fault->hits){++fault->hits;fault->cancelled=true;}}}SetLastError(error);return result;}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"observe actual raw copy cancellation without replacing production code");}~Hooks(){copyFault=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
std::wstring Prefix(const fs::path& path){Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t text[32768]{};const auto length=file.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(file.value,text,32768,VOLUME_NAME_GUID);Check(length&&length<32768,"resolve exact owned raw directory for copy-only hook");return std::wstring(text,length)+L'\\';}
void Lifecycle(const fs::path& parent){Fixture fixture(parent/L"materialization");const auto source=fixture.base/L"source.mp4";Sparse(source,40*MiB);auto capture=fixture.Capture(2,40*MiB);Check(capture->Reserve()==J::Ok&&capture->Prepare(source.c_str())==J::Ok,"real Prepared owns zero raw file and forty MiB remaining bytes");
    Allows(fixture.root,1,16*MiB,true);Allows(fixture.root,2,25*MiB,false);CopyFault fault;fault.prefix=Prefix(fixture.root/L"Video"/L"raw");copyFault=&fault;const auto cancelled=capture->Complete(&fault.cancelled);copyFault=nullptr;
    Check(cancelled==J::Cancelled&&fault.hits==1&&fault.written>=24*MiB&&fault.written<40*MiB,"actual segmented copy is cancelled after partial bytes reach disk");const auto partial=m::Inspect();Check(partial.error.empty()&&partial.totalBytes>=24*MiB&&partial.totalBytes<26*MiB,"partial physical bytes appear in real manager display");
    Allows(fixture.root,3,16*MiB,true);Allows(fixture.root,4,25*MiB,false);Check(capture->Complete()==J::Ok,"retry actual copy from beginning reaches Captured");const auto complete=m::Inspect();Check(complete.error.empty()&&complete.totalBytes>=40*MiB&&complete.totalBytes<42*MiB,"Captured physical bytes are counted once");
    Allows(fixture.root,5,16*MiB,true);Allows(fixture.root,6,25*MiB,false);Check(DeleteFileW(source.c_str())!=FALSE,"remove only own original source after immutable capture");
    f::RawSourceLease lease;Check(capture->Publish({1},1,lease)==J::Ok,"publish real source ownership without duplicate raw promise");Allows(fixture.root,7,16*MiB,true);Allows(fixture.root,8,25*MiB,false);
}

// Represents a crash after durable Prepared and a partial native write. The
// actual raw plan/identity comes from PrepareRawSource, never fabricated IDs.
fs::path PartialPrepared(Fixture& fixture){const auto source=fixture.base/L"prepared.mp4";Sparse(source,32*MiB);f::RawSourceWrite writer;f::RawSourceCleanup cleanup;Check(fixture.raw.PrepareRawSource(source.c_str(),32*MiB,writer,cleanup)==f::Result::Ok&&!cleanup.IsValid(),"create real raw plan for a closed crash-state writer");const auto plan=writer.Plan();writer={};
    auto value=fixture.Root();b::SourceOperation operation;operation.id=Id<b::OperationId>(20);operation.sourceId=Id<b::SourceId>(120);operation.directoryId=1;operation.stage=b::SourceStage::Prepared;operation.reservedBytes=plan.sourceBytes;operation.plan=plan;value.sources.operations.push_back(operation);fixture.Commit(value);
    const auto path=fixture.root/L"Video"/L"raw"/plan.name;Handle file(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(file.value!=INVALID_HANDLE_VALUE,"reopen only exact fixture raw file after original writer closed");Fill(file.value,8*MiB);return path;}
void NonDefaultCannotOffset(const fs::path& parent){for(unsigned mode=0;mode<3;++mode){Fixture fixture(parent/(L"nondefault-"+std::to_wstring(mode)));const auto raw=PartialPrepared(fixture);Allows(fixture.root,1,24*MiB,true);const auto before=m::Inspect();Check(before.error.empty(),"record valid partial-file source budget before external changes");
    if(mode==0)Sparse(fs::path(raw.native()+L":extra"),16*MiB);
    else if(mode==1)Sparse(fixture.root/L"Video"/L"raw"/L"unknown.bin",16*MiB);
    else {fixture.journal.reset();fixture.raw={};fixture.pages={};fixture.control={};fixture.video={};fixture.manager={};const auto old=fixture.root/L"retained-old-raw.bin";Check(MoveFileExW(raw.c_str(),old.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"preserve old physical identity under unrelated name after releasing fixture directory pins");Sparse(raw,16*MiB);}
    const auto view=m::Inspect();if(mode==2){Check(!view.error.empty()&&view.totalBytes>=before.totalBytes,"same-name foreign generation cannot satisfy planned identity or erase last displayed responsibility");Allows(fixture.root,2,MiB,false);Check(fs::file_size(raw)==16*MiB&&fs::file_size(fixture.root/L"retained-old-raw.bin")==8*MiB,"refused generation change preserves both old and replacement files");
        const auto project=fixture.base/L"changed-raw.pmm";m::BeginProject(project.c_str());m::ProjectOpened(project.c_str());m::ResetScene();
        Check(!fs::exists(fixture.root/L"Management"),"valid catalog with changed raw identity still blocks manager publication before directory creation");continue;}
    Check(view.error.empty()&&view.totalBytes>=24*MiB,"actual raw/ADS/unknown generations remain visible physical responsibility");
    Allows(fixture.root,2,20*MiB,false);Allows(fixture.root,3,4*MiB,true);
    const auto cleaned=m::Clean(m::Cleanup::All);Check(cleaned.files==0&&fs::exists(raw),"legacy cleanup does not adopt or delete video responsibility or unknown files");
}}
void UntrustedVideo(const fs::path& parent){for(unsigned mode=0;mode<4;++mode){Fixture fixture(parent/(L"untrusted-"+std::to_wstring(mode)));Sparse(fixture.root/L"unknown.bin",8*MiB);const auto before=m::Inspect();Check(before.error.empty()&&before.totalBytes>=8*MiB,"record actual occupancy before source verification becomes unavailable");
    f::FileLock held;fs::path evidence;Bytes bytes;
    if(mode==0)Check(fixture.control.LockExisting(0,held)==f::Result::Ok,"hold actual source control byte lock");
    else if(mode==1){evidence=fixture.root/L"Video"/L"control"/L".lock";Check(DeleteFileW(evidence.c_str())!=FALSE,"remove only idle fixture control lock");}
    else if(mode==2){evidence=fixture.root/L"Video"/L"control"/L"catalog"/L".lock";Check(DeleteFileW(evidence.c_str())!=FALSE,"remove only idle fixture catalog lock");}
    else {evidence=fixture.root/L"Video"/L"control"/L"catalog"/L"commits"/L"foreign.bin";bytes={0x42,0x41,0x44};Small(evidence,bytes);}
    const auto blocked=m::Inspect();Check(!blocked.error.empty()&&blocked.totalBytes>=before.totalBytes,"untrusted or busy source area preserves observed occupancy and reports error");Allows(fixture.root,1,MiB,false);
    Check(!fs::exists(fixture.root/L"Management"),"bad-scope fixture has no manager publication directory");
    const auto project=fixture.base/L"untrusted-open.pmm";m::BeginProject(project.c_str());m::ProjectOpened(project.c_str());m::ResetScene();
    Check(!fs::exists(fixture.root/L"Management"),"failed source verification blocks Sync and project publication before directory creation");
    if(mode==1||mode==2)Check(!fs::exists(evidence),"manager verification cannot silently recreate missing Video lock");if(mode==3)Check(Read(evidence)==bytes,"manager refuses without overwriting malformed catalog evidence");
    if(mode==0){held={};Allows(fixture.root,2,40*MiB,true);}
}}
} // namespace

int wmain(int argc,wchar_t**argv){try{
    if(argc==3&&std::wstring(argv[1])==L"--check-reserved"){CheckReserved(argv[2]);return 0;}
    if(argc==3&&std::wstring(argv[1])==L"--leave-reserved"){Fixture fixture(argv[2]);auto capture=fixture.Capture(1,40*MiB);Check(capture->Reserve()==J::Ok,"child leaves real persisted Reserved then exits all in-memory state");return 0;}
    if(argc==3&&(std::wstring(argv[1])==L"--control-busy"||std::wstring(argv[1])==L"--control-open")){f::Directory control;f::FileLock lock;Check(f::Directory::Open(fs::path(argv[2])/L"Video"/L"control",false,control)==f::Result::Ok,"child opens exact existing control directory");const bool busy=std::wstring(argv[1])==L"--control-busy";Check(control.LockExisting(0,lock)==(busy?f::Result::Busy:f::Result::Ok)&&lock.IsValid()!=busy,"independent process observes actual writer control exclusion");return 0;}
    Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_manager_source_budget_tests --store NEW_ABSOLUTE_DIR");const fs::path parent(argv[2]);Fresh(parent);Hooks hooks;
    NoVideo(parent);ReservedAndRestart(parent);SharedWriterScope(parent);Lifecycle(parent);NonDefaultCannotOffset(parent);UntrustedVideo(parent);std::printf("PASS cache manager source budget checks=%u\n",checks);return 0;
}catch(const std::exception& error){copyFault=nullptr;std::fprintf(stderr,"FAIL cache manager source budget checks=%u: %s\n",checks,error.what());return 1;}}
