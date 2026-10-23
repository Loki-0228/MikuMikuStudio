// MAINTENANCE: regression-keep
// Own EXE IAT only. Real cache_files calls and self-owned local files; no global
// injection, application startup, GPU, UI, drive remapping or recursive cleanup.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_files.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
using files::Result;
using Bytes=std::vector<std::uint8_t>;
namespace {
unsigned checks=0;
bool restoreFailed=false;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    Handle(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
};
void NewDirectory(const fs::path& path){
    Check(path.is_absolute()&&path.has_filename()&&path!=path.root_path()&&path==path.lexically_normal(),"new normalized absolute directory required");
    Check(fs::is_directory(path.parent_path()),"new directory parent already exists");
    Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"claim fresh owned directory");
}
Bytes Read(const fs::path& path){
    std::ifstream file(path,std::ios::binary);Check(bool(file),"read owned evidence");
    Bytes bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};Check(!file.bad(),"complete evidence read");return bytes;
}
void NewText(const fs::path& path,const std::string& text){
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(file.value!=INVALID_HANDLE_VALUE,"create new evidence file");DWORD written=0;
    Check(text.size()<=MAXDWORD&&WriteFile(file.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size(),"write evidence outside injection scope");
}
files::Spec Spec(){files::Spec s;for(unsigned i=0;i<16;++i)s.owner[i]=std::uint8_t(i+1);for(unsigned i=0;i<32;++i)s.key[i]=std::uint8_t(i+51);return s;}
bool Same(const files::BlobInfo& a,const files::BlobInfo& b){
    return a.name==b.name&&a.spec.owner==b.spec.owner&&a.spec.kind==b.spec.kind&&a.spec.key==b.spec.key&&
        a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&
        a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;
}
std::vector<std::wstring> Names(const files::Directory& directory){
    std::vector<std::wstring> names;Check(directory.Enumerate(100,names)==Result::Ok,"enumerate exact owned generation set");return names;
}
void ReadEquals(const files::ReadLease& lease,const Bytes& expected){
    Bytes output{91,92};Check(lease.ReadAll(output)==Result::Ok&&output==expected,"real lease payload remains exact");
}
std::wstring RootPrefix(const fs::path& path){
    Handle directory(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    Check(directory.value!=INVALID_HANDLE_VALUE,"resolve only owned injection directory");
    wchar_t name[32768]{};const DWORD n=GetFinalPathNameByHandleW(directory.value,name,32768,VOLUME_NAME_GUID);
    Check(n&&n<32768,"resolve stable owned volume GUID path");
    std::wstring prefix(name,n);if(prefix.back()!=L'\\')prefix+=L'\\';return prefix;
}

using WriteFn=BOOL(WINAPI*)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
using FlushFn=BOOL(WINAPI*)(HANDLE);
using InfoFn=BOOL(WINAPI*)(HANDLE,FILE_INFO_BY_HANDLE_CLASS,LPVOID,DWORD);
WriteFn realWrite=nullptr;FlushFn realFlush=nullptr;InfoFn realInfo=nullptr;
enum class Mode { Observe, HeaderFailure, PayloadFailure, PayloadShort, PayloadZero, FlushFailure, CleanupFailure, RemoveFailure };
struct Fault {
    std::wstring prefix,onlyName;
    Mode mode=Mode::Observe;
    DWORD thread=GetCurrentThreadId();
    unsigned writes=0,payloadWrites=0,flushes=0,dispositions=0,writeFaults=0,flushFaults=0,deleteFaults=0;
    unsigned unexpectedHandles=0;
    HANDLE first=INVALID_HANDLE_VALUE;
    wchar_t capturedName[38]{};
    BY_HANDLE_FILE_INFORMATION capturedIdentity{};
    bool identityValid=false;
    Fault(std::wstring root,Mode m,std::wstring name={}):prefix(std::move(root)),onlyName(std::move(name)),mode(m){}
    bool Matches(HANDLE handle)noexcept{
        if(GetCurrentThreadId()!=thread)return false;
        wchar_t name[32768]{};const DWORD n=GetFinalPathNameByHandleW(handle,name,32768,VOLUME_NAME_GUID);
        if(!n||n>=32768||n!=prefix.size()+37||_wcsnicmp(name,prefix.c_str(),prefix.size()))return false;
        const auto* leaf=name+prefix.size();
        for(unsigned i=0;i<32;++i)if(!((leaf[i]>=L'0'&&leaf[i]<=L'9')||(leaf[i]>=L'a'&&leaf[i]<=L'f')))return false;
        if(std::wcscmp(leaf+32,L".blob")||(!onlyName.empty()&&onlyName!=leaf))return false;
        if(first==INVALID_HANDLE_VALUE){first=handle;std::copy_n(leaf,38,capturedName);}
        else if(first!=handle){++unexpectedHandles;return false;}
        identityValid=GetFileInformationByHandle(handle,&capturedIdentity)!=FALSE;
        return true;
    }
};
thread_local Fault* active=nullptr;
struct Arm {
    explicit Arm(Fault& fault){Check(active==nullptr,"nonrecursive scoped injection");active=&fault;}
    ~Arm(){active=nullptr;}
};
BOOL WINAPI WriteHook(HANDLE h,LPCVOID data,DWORD bytes,LPDWORD done,LPOVERLAPPED overlapped)noexcept{
    auto* fault=active;
    if(!fault||!fault->Matches(h))return realWrite(h,data,bytes,done,overlapped);
    ++fault->writes;LARGE_INTEGER zero{},position{};
    if(overlapped||!SetFilePointerEx(h,zero,&position,FILE_CURRENT))return realWrite(h,data,bytes,done,overlapped);
    const bool payload=position.QuadPart>=128;
    if(payload)++fault->payloadWrites;
    const bool fail=(fault->mode==Mode::HeaderFailure&&position.QuadPart==0)||
        (fault->mode==Mode::PayloadFailure&&payload);
    if(fail){++fault->writeFaults;if(done)*done=0;SetLastError(ERROR_WRITE_FAULT);return FALSE;}
    if(fault->mode==Mode::PayloadZero&&payload){++fault->writeFaults;if(done)*done=0;return TRUE;}
    if(fault->mode==Mode::PayloadShort&&payload&&fault->writeFaults==0){
        ++fault->writeFaults;return realWrite(h,data,(std::min)(bytes,DWORD(7)),done,overlapped);
    }
    return realWrite(h,data,bytes,done,overlapped);
}
BOOL WINAPI FlushHook(HANDLE h)noexcept{
    auto* fault=active;if(!fault||!fault->Matches(h))return realFlush(h);
    ++fault->flushes;
    if(fault->mode==Mode::FlushFailure||fault->mode==Mode::CleanupFailure){++fault->flushFaults;SetLastError(ERROR_WRITE_FAULT);return FALSE;}
    return realFlush(h);
}
BOOL WINAPI InfoHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)noexcept{
    auto* fault=active;
    if(!fault||kind!=FileDispositionInfo||size!=sizeof(FILE_DISPOSITION_INFO)||!data||
        !static_cast<FILE_DISPOSITION_INFO*>(data)->DeleteFile||!fault->Matches(h))return realInfo(h,kind,data,size);
    ++fault->dispositions;
    if(fault->mode==Mode::CleanupFailure||fault->mode==Mode::RemoveFailure){++fault->deleteFaults;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return realInfo(h,kind,data,size);
}

// Identical import-table boundary to the repository's ReadFile observation
// tests, but with RAII restoration and an explicit current-thread/path gate.
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
            if(!it->protectionKnown){it->patched=false;continue;} // First VirtualProtect failed before changing anything.
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
                if(!std::strcmp(name,"WriteFile")){Check(!realWrite||reinterpret_cast<ULONG_PTR>(realWrite)==original,"consistent real WriteFile import");realWrite=reinterpret_cast<WriteFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&WriteHook);}
                else if(!std::strcmp(name,"FlushFileBuffers")){Check(!realFlush||reinterpret_cast<ULONG_PTR>(realFlush)==original,"consistent real Flush import");realFlush=reinterpret_cast<FlushFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&FlushHook);}
                else if(!std::strcmp(name,"SetFileInformationByHandle")){Check(!realInfo||reinterpret_cast<ULONG_PTR>(realInfo)==original,"consistent real disposition import");realInfo=reinterpret_cast<InfoFn>(original);replacement=reinterpret_cast<ULONG_PTR>(&InfoHook);}
                if(replacement)slots_.push_back({reinterpret_cast<ULONG_PTR*>(&addresses->u1.Function),original,replacement,false});
            }
        }
        Check(realWrite&&realFlush&&realInfo&&slots_.size()>=3,"locate all three actual production imports in this EXE");
        try{
            for(auto& slot:slots_){slot.patched=true;Check(Exchange(slot,slot.replacement),"patch only owned EXE import slot and restore page protection");}
        }catch(...){Restore();throw;}
    }
    ~Imports(){Restore();}
    Imports(const Imports&)=delete;
    void Verify()const{for(const auto& slot:slots_)Check(*slot.address==slot.replacement,"actual own IAT still points at calibrated wrapper");}
};

struct Fixture {
    fs::path path;
    files::Directory directory;
    files::BlobInfo sentinel;
    files::ReadLease lease;
    Bytes sentinelPayload{0x81,7,0,0xff};
    Bytes sentinelRaw;
    std::wstring prefix;
    Fixture(const fs::path& root,const wchar_t* name):path(root/name){
        Check(files::Directory::Open(path,true,directory)==Result::Ok,"open fresh real cache Directory");
        Check(directory.Create(Spec(),sentinelPayload.data(),sentinelPayload.size(),sentinel)==Result::Ok,"create protected sentinel through production");
        Check(directory.Acquire(sentinel,lease)==Result::Ok,"hold independent sentinel read lease");
        NewText(path/L"foreign.txt","foreign sentinel\n");sentinelRaw=Read(path/sentinel.name);prefix=RootPrefix(path);
    }
    void Unchanged(){
        Check(Read(path/sentinel.name)==sentinelRaw&&Same(lease.Info(),sentinel),"fault never changes sentinel file or lease output");
        ReadEquals(lease,sentinelPayload);
        const auto foreign=Read(path/L"foreign.txt");Check(std::string(foreign.begin(),foreign.end())=="foreign sentinel\n","foreign file never deleted or altered");
    }
};
std::string Counts(const Fault& f){
    return "writes="+std::to_string(f.writes)+" payloadWrites="+std::to_string(f.payloadWrites)+" flushes="+std::to_string(f.flushes)+
        " dispositions="+std::to_string(f.dispositions)+" writeFaults="+std::to_string(f.writeFaults)+" flushFaults="+std::to_string(f.flushFaults)+
        " deleteFaults="+std::to_string(f.deleteFaults)+" unexpectedHandles="+std::to_string(f.unexpectedHandles)+"\n";
}
void Calibration(const fs::path& root){
    const auto path=root/L"calibration";NewDirectory(path);
    const auto name=L"00000000000000000000000000000001.blob";
    Handle file(CreateFileW((path/name).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(file.value!=INVALID_HANDLE_VALUE,"create own direct IAT calibration fixture");
    const auto prefix=RootPrefix(path);Fault fault(prefix,Mode::HeaderFailure,name);
    DWORD done=99,error=0;BOOL result=TRUE;
    {Arm arm(fault);result=WriteFile(file.value,"x",1,&done,nullptr);error=GetLastError();}
    Check(!result&&done==0&&error==ERROR_WRITE_FAULT&&fault.writeFaults==1&&fault.writes==1,"calibrate actual EXE WriteFile IAT failure");
    Fault isolation(prefix,Mode::HeaderFailure,name);bool otherThread=false;
    {
        Arm arm(isolation);
        std::thread thread([&]{DWORD written=0;otherThread=WriteFile(file.value,"t",1,&written,nullptr)&&written==1;});thread.join();
    }
    Check(otherThread&&isolation.writes==0,"IAT patch forwards other threads without injecting");
    Handle foreign(CreateFileW((path/L"ordinary.txt").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));
    Check(foreign.value!=INVALID_HANDLE_VALUE,"create non-blob isolation file");
    {Arm arm(isolation);result=WriteFile(foreign.value,"f",1,&done,nullptr);}
    Check(result&&done==1&&isolation.writes==0,"IAT patch forwards non-blob helpers in same directory");
    Fault flush(prefix,Mode::FlushFailure,name);
    {Arm arm(flush);result=FlushFileBuffers(file.value);error=GetLastError();}
    Check(!result&&error==ERROR_WRITE_FAULT&&flush.flushFaults==1,"calibrate FlushFileBuffers IAT");
    FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;Fault remove(prefix,Mode::RemoveFailure,name);
    {Arm arm(remove);result=SetFileInformationByHandle(file.value,FileDispositionInfo,&disposition,sizeof(disposition));error=GetLastError();}
    Check(!result&&error==ERROR_ACCESS_DENIED&&remove.deleteFaults==1,"calibrate disposition IAT without deleting calibration file");
    NewText(root/L"calibration.txt",Counts(fault)+Counts(flush)+Counts(remove)+"thread-and-nonblob-isolation=passed\n");
}

void CreateFault(const fs::path& root,const wchar_t* name,Mode mode,const Bytes& payload){
    Fixture fixture(root,name);const auto before=Names(fixture.directory);
    auto output=fixture.sentinel;Fault fault(fixture.prefix,mode);Result result;
    {Arm arm(fault);result=fixture.directory.Create(Spec(),payload.data(),payload.size(),output);}
    NewText(fixture.path/L"fault-counts.txt",Counts(fault)+"result="+std::to_string(static_cast<int>(result))+"\n");
    Check(fault.writes>0&&fault.first!=INVALID_HANDLE_VALUE&&fault.unexpectedHandles==0&&fault.identityValid,
        "real production Create enters scoped hooks on one owned generation handle");
    Check(std::wstring(fault.capturedName)!=fixture.sentinel.name,"Create fault never targets sentinel handle");
    const auto after=Names(fixture.directory);
    if(mode==Mode::PayloadShort){
        Check(result==Result::Ok&&fault.writeFaults==1&&fault.payloadWrites>=3&&fault.flushes==1&&fault.dispositions==0,
            "real short payload write is retried and completes rather than reported as failure");
        Check(output.name==fault.capturedName&&output.payloadBytes==payload.size()&&output.spec.owner==Spec().owner&&output.spec.key==Spec().key,
            "short-write success publishes the actually observed generation");
        files::ReadLease lease;Check(fixture.directory.Acquire(output,lease)==Result::Ok,"short-write result passes actual envelope and digest validation");ReadEquals(lease,payload);lease={};
        const auto raw=Read(fixture.path/output.name);
        Check(raw.size()==128+payload.size()&&std::equal(payload.begin(),payload.end(),raw.begin()+128),"independent disk payload proves short-write retry offset");
        Check(after.size()==before.size()+1&&fixture.directory.Remove(output)==Result::DeletePending&&Names(fixture.directory)==before,
            "remove only own successful short-write generation by returned identity");
    }else{
        Check(result==Result::Io&&Same(output,fixture.sentinel),"Create failure preserves every output field");
        if(mode==Mode::HeaderFailure)Check(fault.writeFaults==1&&fault.writes==1&&fault.payloadWrites==0&&fault.flushes==0,"first header write failure really hit before payload");
        if(mode==Mode::PayloadFailure||mode==Mode::PayloadZero)Check(fault.writeFaults==1&&fault.payloadWrites==1&&fault.flushes==0,"first payload failure or zero progress really hit");
        if(mode==Mode::FlushFailure||mode==Mode::CleanupFailure)Check(fault.flushFaults==1&&fault.payloadWrites>=2,"flush failure occurs after actual complete payload writes");
        Check(fault.dispositions==1,"failed Create attempts same-handle cleanup once");
        if(mode==Mode::CleanupFailure){
            Check(fault.deleteFaults==1&&after.size()==before.size()+1,"failed disposition leaves exactly one observable orphan");
            std::vector<std::wstring> added;std::set_difference(after.begin(),after.end(),before.begin(),before.end(),std::back_inserter(added));
            Check(added.size()==1&&added[0]==fault.capturedName,"orphan name is exactly the handle seen during failed Create");
            files::ReadLease orphan;Check(fixture.directory.Inspect(added[0],Spec().owner,files::MaximumPayloadBytes,orphan)==Result::Ok,"complete but uncommitted orphan can be independently inspected");
            const auto& actual=orphan.Info();
            Check(actual.identity.volume==fault.capturedIdentity.dwVolumeSerialNumber&&actual.identity.fileIndex==
                ((std::uint64_t(fault.capturedIdentity.nFileIndexHigh)<<32)|fault.capturedIdentity.nFileIndexLow),"orphan is same physical file observed by failure hook");
            ReadEquals(orphan,payload);const auto raw=Read(fixture.path/added[0]);
            Check(raw.size()==128+payload.size()&&std::equal(payload.begin(),payload.end(),raw.begin()+128),"orphan contains exact authored payload after flush failure");
            NewText(fixture.path/L"orphan.txt","unpublished generation retained; no durability or index ownership claimed\n");
        }else Check(fault.deleteFaults==0&&after==before&&!fs::exists(fixture.path/fault.capturedName),"successful failure cleanup leaves no new generation");
    }
    fixture.Unchanged();
}
void RemoveFault(const fs::path& root){
    Fixture fixture(root,L"remove-failure");const Bytes payload{1,0xff,2};files::BlobInfo victim;
    Check(fixture.directory.Create(Spec(),payload.data(),payload.size(),victim)==Result::Ok,"create separate Remove victim");
    const auto before=Names(fixture.directory);const auto original=Read(fixture.path/victim.name);
    Fault fault(fixture.prefix,Mode::RemoveFailure,victim.name);Result result;
    {Arm arm(fault);result=fixture.directory.Remove(victim);}
    NewText(fixture.path/L"fault-counts.txt",Counts(fault));
    Check(result==Result::Io&&fault.deleteFaults==1&&fault.dispositions==1&&fault.writes==0&&fault.flushes==0&&fault.unexpectedHandles==0,
        "Remove propagates real disposition failure on only its exclusive victim handle");
    Check(Names(fixture.directory)==before&&Read(fixture.path/victim.name)==original,"failed Remove preserves full file and candidates");
    files::ReadLease lease;Check(fixture.directory.Acquire(victim,lease)==Result::Ok&&Same(lease.Info(),victim),"failed Remove leaves exact trusted identity readable");ReadEquals(lease,payload);
    Fault busy(fixture.prefix,Mode::RemoveFailure,victim.name);
    {Arm arm(busy);result=fixture.directory.Remove(victim);}
    Check(result==Result::Busy&&busy.dispositions==0&&busy.deleteFaults==0,"active lease rejects Remove before fault seam is reached");
    lease={};Check(fixture.directory.Remove(victim)==Result::DeletePending&&!fs::exists(fixture.path/victim.name),"Remove succeeds after disarming disposition fault and releasing lease");
    fixture.Unchanged();
}
}

int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIR");
        const fs::path root(argv[2]);NewDirectory(root);
        {
            Imports imports;imports.Verify();Calibration(root);
            Bytes payload(65536+211);for(std::size_t i=0;i<payload.size();++i)payload[i]=std::uint8_t(i*43+19);
            CreateFault(root,L"header-failure",Mode::HeaderFailure,payload);
            CreateFault(root,L"payload-failure",Mode::PayloadFailure,payload);
            CreateFault(root,L"payload-zero",Mode::PayloadZero,payload);
            CreateFault(root,L"payload-short",Mode::PayloadShort,payload);
            CreateFault(root,L"flush-failure",Mode::FlushFailure,payload);
            CreateFault(root,L"cleanup-failure",Mode::CleanupFailure,payload);
            RemoveFault(root);imports.Verify();
        }
        Check(!restoreFailed&&active==nullptr,"all own IAT slots restored after test scope");
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\n");
        std::printf("PASS cache file IO fault checks=%u evidence=%ls\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache file IO fault checks=%u restoreFailed=%d: %s\n",checks,restoreFailed,e.what());return 1;}
      catch(...){std::fprintf(stderr,"FAIL cache file IO fault unexpected exception\n");return 2;}
}
