// MAINTENANCE: regression-keep. Owned publication/recovery process exits; not power-loss coverage.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_catalog.hpp"
#include "../src/io/cache_catalog_codec.hpp"
#include "../src/io/cache_catalog_intent.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs=std::filesystem;
namespace catalog=mikumikustudio::cache_catalog;
namespace files=mikumikustudio::cache_files;
namespace codec=mikumikustudio::cache_catalog::codec;
using Bytes=std::vector<std::uint8_t>;
namespace {
unsigned checks=0;
bool restoreFailed=false;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
};

constexpr wchar_t markerName[]=L"catalog-owned-recovery-test.marker";
enum class Stage:unsigned { IntentHeader,IntentFlush,SnapshotHeader,CommitHeader,CommitFlush,
    DecisionHeader,DecisionFlush,DeleteCommit,DeleteIntent,Count };
std::string Marker(DWORD pid){return "MMS cache catalog owned recovery test v1\nparent="+std::to_string(pid)+"\n";}
Bytes Read(const fs::path& path){std::ifstream in(path,std::ios::binary);Check(bool(in),"open own evidence");Bytes b{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};Check(!in.bad(),"read complete own evidence");return b;}
void NewFile(const fs::path& path,const void* data,DWORD size){
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;
    Check(h.value!=INVALID_HANDLE_VALUE&&WriteFile(h.value,data,size,&done,nullptr)&&done==size&&FlushFileBuffers(h.value),"create and flush own new evidence");
}
void Absolute(const fs::path& p){Check(p.is_absolute()&&p.has_filename()&&p!=p.root_path()&&p==p.lexically_normal(),"normalized absolute owned root");}
Bytes OldBytes(){return {7,0,0xff,19,3};}
Bytes NewBytes(){Bytes b(65536+191);for(std::size_t i=0;i<b.size();++i)b[i]=std::uint8_t(i*31+83);return b;}
files::Owner Owner(){return {0x41,0x42,0x43,0x44,5,6,7,8,9,10,11,12,13,14,15,16};}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
std::wstring Prefix(const fs::path& path){
    Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    wchar_t p[32768]{};const DWORD n=GetFinalPathNameByHandleW(h.value,p,32768,VOLUME_NAME_GUID);
    Check(h.value!=INVALID_HANDLE_VALUE&&n&&n<32768,"resolve owned injection directory by handle");
    std::wstring result(p,n);if(result.back()!=L'\\')result+=L'\\';return result;
}
using WriteFn=BOOL(WINAPI*)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
using FlushFn=BOOL(WINAPI*)(HANDLE);
using InfoFn=BOOL(WINAPI*)(HANDLE,FILE_INFO_BY_HANDLE_CLASS,LPVOID,DWORD);
WriteFn realWrite=nullptr;FlushFn realFlush=nullptr;InfoFn realInfo=nullptr;
struct Evidence {
    DWORD magic=0x43524f31,stage=0,reached=0;
    DWORD headers[4]{},payloads[4]{},flushes[4]{},deletes[4]{},deleteAttempts[4]{};
    wchar_t names[4][38]{};
};
// Kinds 0/1 are the exact prepared snapshot/commit, 2/3 are intent/decision.
// Match accepts only this thread, actual volume-GUID directory and lowercase
// generated names. Recovery further restricts the first three names in advance.
struct Fault {
    std::wstring prefixes[3];DWORD thread=GetCurrentThreadId();Stage stage;
    Evidence evidence{};HANDLE control=INVALID_HANDLE_VALUE;
    // Parent-only returning faults use Stage::Count, never a crash stage.
    // Both rejection before native Flush and false-after-real-success are
    // exercised, without equating verified bytes with successful durability.
    int failingFlush=-1;bool failBeforeFlush=false;DWORD flushFaults=0;
    Fault(const fs::path& root,Stage s,HANDLE file):prefixes{Prefix(root/L"snapshots"),Prefix(root/L"commits"),Prefix(root/L"operations")},stage(s),control(file){evidence.stage=unsigned(s);}
    int Match(HANDLE h,wchar_t* name) noexcept {
        if(GetCurrentThreadId()!=thread)return -1;
        wchar_t path[32768]{};const DWORD n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);
        if(!n||n>=32768)return -1;
        for(int dir=0;dir<3;++dir){const auto& p=prefixes[dir];
            if(n!=p.size()+37||_wcsnicmp(path,p.c_str(),p.size()))continue;
            const auto* leaf=path+p.size();
            for(unsigned i=0;i<32;++i)if(!((leaf[i]>=L'0'&&leaf[i]<=L'9')||(leaf[i]>=L'a'&&leaf[i]<=L'f')))return -1;
            if(std::wcscmp(leaf+32,L".blob"))return -1;
            std::copy_n(leaf,38,name);
            if(dir<2)return !evidence.names[dir][0]||!std::wcscmp(evidence.names[dir],leaf)?dir:-1;
            if(evidence.names[3][0]&&!std::wcscmp(evidence.names[3],leaf))return 3;
            if(!evidence.names[2][0]||!std::wcscmp(evidence.names[2],leaf))return 2;
            return 4; // Unknown operation is classified only by its real header below.
        }return -1;
    }
    void Save() noexcept {
        LARGE_INTEGER zero{};DWORD done=0;
        if(!SetFilePointerEx(control,zero,nullptr,FILE_BEGIN)||!realWrite(control,&evidence,sizeof evidence,&done,nullptr)||
            done!=sizeof evidence||!realFlush(control))ExitProcess(239);
    }
    [[noreturn]] void Crash() noexcept {evidence.reached=1;Save();ExitProcess(170+unsigned(stage));}
};
thread_local Fault* active=nullptr;
struct Arm {explicit Arm(Fault& f){Check(!active,"one scoped fault");active=&f;}~Arm(){active=nullptr;}};
BOOL WINAPI WriteHook(HANDLE h,LPCVOID data,DWORD bytes,LPDWORD written,LPOVERLAPPED ov) noexcept {
    auto* f=active;wchar_t name[38]{};int k=f?f->Match(h,name):-1;
    if(k<0||ov)return realWrite(h,data,bytes,written,ov);
    LARGE_INTEGER zero{},position{};
    if(!SetFilePointerEx(h,zero,&position,FILE_CURRENT))return realWrite(h,data,bytes,written,ov);
    if(k>=2){
        if(position.QuadPart==0&&bytes==128&&data){
            const auto* p=static_cast<const unsigned char*>(data);
            if(std::memcmp(p,"MMSCacheBlob0001",16)||p[20]!=7||p[21]||p[22]||p[23]||(p[40]!=4&&p[40]!=5))return realWrite(h,data,bytes,written,ov);
            for(unsigned i=41;i<72;++i)if(p[i])return realWrite(h,data,bytes,written,ov);
            k=p[40]==4?2:3;
        }else if(k==4)return realWrite(h,data,bytes,written,ov);
    }
    if(f->evidence.names[k][0]&&std::wcscmp(f->evidence.names[k],name))return realWrite(h,data,bytes,written,ov);
    std::copy_n(name,38,f->evidence.names[k]);
    const BOOL ok=realWrite(h,data,bytes,written,ov);
    if(!ok||!written||*written!=bytes)return ok;
    if(position.QuadPart==0&&bytes==128){
        ++f->evidence.headers[k];
        if((k==0&&f->stage==Stage::SnapshotHeader)||(k==1&&f->stage==Stage::CommitHeader)||
            (k==2&&f->stage==Stage::IntentHeader)||(k==3&&f->stage==Stage::DecisionHeader))f->Crash();
    }else if(position.QuadPart>=128)++f->evidence.payloads[k];
    return ok;
}
BOOL WINAPI FlushHook(HANDLE h) noexcept {
    auto* f=active;wchar_t name[38]{};const int k=f?f->Match(h,name):-1;
    if(f&&k>=0&&k<4&&k==f->failingFlush&&f->failBeforeFlush){
        ++f->flushFaults;SetLastError(ERROR_WRITE_FAULT);return FALSE;
    }
    const BOOL ok=realFlush(h);if(!ok||k<0||k>3)return ok;
    ++f->evidence.flushes[k];
    if(k==f->failingFlush){++f->flushFaults;SetLastError(ERROR_WRITE_FAULT);return FALSE;}
    if((k==1&&f->stage==Stage::CommitFlush)||(k==2&&f->stage==Stage::IntentFlush)||(k==3&&f->stage==Stage::DecisionFlush))f->Crash();
    return ok;
}
BOOL WINAPI InfoHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size) noexcept {
    auto* f=active;wchar_t name[38]{};const int k=f?f->Match(h,name):-1;
    const bool deleting=k>=0&&k<4&&kind==FileDispositionInfo&&size==sizeof(FILE_DISPOSITION_INFO)&&data&&static_cast<FILE_DISPOSITION_INFO*>(data)->DeleteFile;
    if(deleting)++f->evidence.deleteAttempts[k];
    const BOOL ok=realInfo(h,kind,data,size);
    if(!ok||!deleting)return ok;
    ++f->evidence.deletes[k];
    if((k==1&&f->stage==Stage::DeleteCommit)||(k==2&&f->stage==Stage::DeleteIntent))f->Crash();
    return ok;
}
// Patch the current EXE's imported pointers only, before arming a strict
// same-thread/volume-GUID/owned-directory/blob-name match. Never patch DLLs.
class Imports {
    struct Slot{ULONG_PTR* address;ULONG_PTR before,after;DWORD protection=0;bool known=false,patched=false;};
    std::vector<Slot> slots;
    static bool Set(Slot& s,ULONG_PTR value) noexcept {
        DWORD before=0;if(!VirtualProtect(s.address,sizeof(*s.address),PAGE_READWRITE,&before))return false;
        if(!s.known){s.protection=before;s.known=true;}
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(s.address),reinterpret_cast<void*>(value));
        DWORD ignored=0;return VirtualProtect(s.address,sizeof(*s.address),s.protection,&ignored)!=FALSE;
    }
    void Restore() noexcept {for(auto it=slots.rbegin();it!=slots.rend();++it)if(it->patched&&it->known){if(!Set(*it,it->before)||*it->address!=it->before)restoreFailed=true;it->patched=false;}}
public:
    Imports(){
        auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);Check(dos->e_magic==IMAGE_DOS_SIGNATURE,"own EXE DOS header");
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);Check(nt->Signature==IMAGE_NT_SIGNATURE,"own EXE PE header");
        const auto table=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];Check(table.VirtualAddress!=0,"own EXE import table");
        for(auto* dll=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+table.VirtualAddress);dll->Name;++dll){
            if(!dll->OriginalFirstThunk)continue;
            auto* names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+dll->OriginalFirstThunk);
            auto* values=reinterpret_cast<IMAGE_THUNK_DATA*>(base+dll->FirstThunk);
            for(;names->u1.AddressOfData;++names,++values){
                if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
                const auto* n=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
                const char* name=reinterpret_cast<const char*>(n->Name);const auto before=ULONG_PTR(values->u1.Function);ULONG_PTR after=0;
                if(!std::strcmp(name,"WriteFile")){Check(!realWrite||ULONG_PTR(realWrite)==before,"consistent WriteFile imports");realWrite=reinterpret_cast<WriteFn>(before);after=ULONG_PTR(&WriteHook);}
                if(!std::strcmp(name,"FlushFileBuffers")){Check(!realFlush||ULONG_PTR(realFlush)==before,"consistent Flush imports");realFlush=reinterpret_cast<FlushFn>(before);after=ULONG_PTR(&FlushHook);}
                if(!std::strcmp(name,"SetFileInformationByHandle")){Check(!realInfo||ULONG_PTR(realInfo)==before,"consistent disposition imports");realInfo=reinterpret_cast<InfoFn>(before);after=ULONG_PTR(&InfoHook);}
                if(after)slots.push_back({reinterpret_cast<ULONG_PTR*>(&values->u1.Function),before,after});
            }
        }
        Check(realWrite&&realFlush&&realInfo&&slots.size()>=3,"three actual own production IAT entries exist");
        try{for(auto& s:slots){s.patched=true;Check(Set(s,s.after)&&*s.address==s.after,"install only own IAT wrapper");}}catch(...){Restore();throw;}
    }
    ~Imports(){Restore();}
};


using Disk=std::map<std::wstring,Bytes>;
Disk DiskFiles(const fs::path& root){
    Disk result;
    for(const auto* dir:{L"snapshots",L"commits",L"operations"}){
        if(!fs::exists(root/dir))continue;
        for(const auto& item:fs::directory_iterator(root/dir)){
            if(item.path().filename()==L".lock")continue;
            Check(item.is_regular_file(),"own metadata entry is a regular file");
            result.emplace(std::wstring(dir)+L"/"+item.path().filename().wstring(),Read(item.path()));
        }
    }return result;
}
void BaselineUnchanged(const Disk& before,const Disk& after){for(const auto& p:before){auto it=after.find(p.first);Check(it!=after.end()&&it->second==p.second,"all original committed bytes retained");}}
struct Intent {mikumikustudio::cache_catalog::intent::Record record;files::BlobInfo info;};
Intent ReadIntent(const fs::path& path){
    files::Directory dir;Check(files::Directory::Open(path/L"operations",false,dir)==files::Result::Ok,"open own operations");
    std::vector<std::wstring> names;Check(dir.EnumerateAll(3,names)==files::Result::Ok&&names.size()==1,"one exact intent before recovery");
    files::ReadLease lease;Check(dir.Inspect(names.front(),codec::NamespaceOwner,752,lease)==files::Result::Ok,"real intent envelope and hash valid");
    Intent out;out.info=lease.Info();Bytes bytes;
    Check(out.info.spec.kind==files::Kind::CatalogIntent&&out.info.spec.key==files::Digest{4}&&lease.ReadAll(bytes)==files::Result::Ok,"exact intent kind/key and readable payload");
    Check(mikumikustudio::cache_catalog::intent::Decode(bytes.data(),bytes.size(),out.record)==codec::Result::Ok,"decode real production intent");
    return out;
}
void Leaf(const std::wstring& s){Check(!s.empty()&&s.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-")==s.npos,"fixed own case/control leaf");}
void Worker(const fs::path& root,const std::wstring& leaf,Stage stage,bool initialize,DWORD parentPid){
    Absolute(root);Leaf(leaf);
    const auto marker=Read(root/markerName);const auto expected=Marker(parentPid);
    Check(std::string(marker.begin(),marker.end())==expected,"worker exact parent marker");
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));
    Check(parent.value&&parent.value!=INVALID_HANDLE_VALUE&&WaitForSingleObject(parent.value,0)==WAIT_TIMEOUT,"own parent remains alive");
    const auto path=root/leaf;catalog::Store store(path);catalog::Snapshot before;
    const bool recovering=unsigned(stage)>=unsigned(Stage::DecisionHeader);
    Intent intent;
    if(recovering)intent=ReadIntent(path);
    else if(!initialize)Check(store.Read(before)==catalog::Result::Ok&&before.Revision()==1&&before.StoreId()==Owner()&&before.Bytes()==OldBytes(),"worker actual initial publication");
    const auto controlPath=root/(leaf+L"-"+std::to_wstring(unsigned(stage))+L".control");
    Handle control(CreateFileW(controlPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(control.value!=INVALID_HANDLE_VALUE,"new own worker evidence");
    Imports imports;Fault fault(path,stage,control.value);
    if(recovering){
        std::copy_n(intent.record.snapshot.name.c_str(),38,fault.evidence.names[0]);
        std::copy_n(intent.record.commit.name.c_str(),38,fault.evidence.names[1]);
        std::copy_n(intent.info.name.c_str(),38,fault.evidence.names[2]);
    }
    auto output=before;const auto payload=NewBytes();catalog::RecoveryReport report;
    {Arm arm(fault);
        if(recovering)(void)store.RecoverOwned(report);
        else if(initialize)(void)store.InitializeOwned(Owner(),payload,output);
        else (void)store.Commit(before,payload,output);
    }
    Check(false,"real production fault stage must hard-exit; returning is never a passing test");
}
std::wstring Quote(const std::wstring& s){Check(s.find(L'"')==s.npos,"safe own argument");return L"\""+s+L"\"";}
Evidence RunWorker(const fs::path& root,const std::wstring& leaf,Stage stage,bool initialize=false){
    Leaf(leaf);wchar_t exe[32768]{};const DWORD n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"own executable path");
    auto line=Quote(exe)+L" --worker "+Quote(root.wstring())+L" "+Quote(leaf)+L" "+std::to_wstring(unsigned(stage))+L" "+(initialize?L"1":L"0")+L" "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{};si.cb=sizeof si;si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    Check(CreateProcessW(exe,line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&si,&pi)!=FALSE,"start only own hidden no-inheritance worker");
    Handle thread(pi.hThread),child(pi.hProcess);
    const auto wait=WaitForSingleObject(child.value,30000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(child.value,238);WaitForSingleObject(child.value,5000);Check(false,"bounded own worker completion");}
    DWORD exit=0;Check(GetExitCodeProcess(child.value,&exit)&&exit==170+unsigned(stage),"exact calibrated real hard-exit stage code");
    auto raw=Read(root/(leaf+L"-"+std::to_wstring(unsigned(stage))+L".control"));Evidence e{};
    Check(raw.size()==sizeof e,"complete own stage evidence");std::memcpy(&e,raw.data(),sizeof e);
    Check(e.magic==0x43524f31&&e.stage==unsigned(stage)&&e.reached==1,"actual hook reached requested stage");
    switch(stage){
    case Stage::IntentHeader:Check(e.headers[2]==1&&e.payloads[2]==0&&e.flushes[2]==0,"intent header before payload");break;
    case Stage::IntentFlush:Check(e.headers[2]==1&&e.payloads[2]>0&&e.flushes[2]==1&&e.headers[0]==0&&e.headers[1]==0,"intent flush precedes prepared data writes");break;
    case Stage::SnapshotHeader:Check(e.headers[0]==1&&e.payloads[0]==0&&e.flushes[0]==0&&e.flushes[2]==1,"snapshot real partial write after durable intent");break;
    case Stage::CommitHeader:Check(e.headers[1]==1&&e.payloads[1]==0&&e.flushes[1]==0&&e.flushes[0]==1,"commit real partial write after snapshot flush");break;
    case Stage::CommitFlush:Check(e.headers[1]==1&&e.payloads[1]>0&&e.flushes[1]==1,"complete real commit flush before process exit");break;
    case Stage::DecisionHeader:Check(e.headers[3]==1&&e.payloads[3]==0&&e.flushes[3]==0,"partial decision is not trusted");break;
    case Stage::DecisionFlush:Check(e.flushes[3]==1&&e.deletes[0]==0&&e.deletes[1]==0,"decision durable before any data deletion");break;
    case Stage::DeleteCommit:Check(e.flushes[3]==1&&e.deletes[1]==1&&e.deletes[0]==0&&e.deletes[2]==0,"commit disposed first after decision flush");break;
    case Stage::DeleteIntent:Check(e.deletes[1]==1&&e.deletes[0]==1&&e.deletes[2]==1&&e.deletes[3]==0,"intent disposed after both data files, decision remains");break;
    default:Check(false,"known stage");
    }return e;
}
catalog::Snapshot Seed(const fs::path& path,bool initialize=false){
    Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"fresh owned case");
    catalog::Snapshot before;
    if(!initialize){catalog::Store store(path);Check(store.InitializeOwned(Owner(),OldBytes(),before)==catalog::Result::Ok&&before.Revision()==1&&before.StoreId()==Owner(),"real InitializeOwned baseline");}
    // Empty directories are valid old-layout-compatible initialization input.
    for(const auto* dir:{L"snapshots",L"commits",L"operations"})if(!fs::exists(path/dir))Check(CreateDirectoryW((path/dir).c_str(),nullptr)!=FALSE,"own empty metadata directory");
    return before;
}
bool Sentinel(const catalog::RecoveryReport& r){return r.confirmedPublication&&r.rolledBack&&r.revision==777;}
void Reject(catalog::Store& store,const fs::path& path,catalog::Result expected){
    const auto disk=DiskFiles(path);catalog::RecoveryReport r;r.confirmedPublication=true;r.rolledBack=true;r.revision=777;
    Check(store.RecoverOwned(r)==expected&&Sentinel(r),"rejected recovery preserves caller report");
    Check(DiskFiles(path)==disk,"rejected recovery preserves every existing named byte");
}
void PendingGates(catalog::Store& store,const catalog::Snapshot& before){
    auto output=before;Check(store.Read(output)==catalog::Result::RecoveryRequired&&Same(output,before),"pending operations prevent fallback Read");
    catalog::CollectionReport report;report.attempted=97;
    Check(store.CollectRetired(report)==catalog::Result::RecoveryRequired&&report.attempted==97,"pending operations prevent collection");
    if(before.IsValid()){
        Check(store.Commit(before,OldBytes(),output)==catalog::Result::RecoveryRequired&&Same(output,before),"pending operations prevent new Commit");
        Check(store.Checkpoint(before,output)==catalog::Result::RecoveryRequired&&Same(output,before),"pending operations prevent checkpoint");
    }
}
void FinishRecovery(catalog::Store& store,const fs::path& path,const catalog::Snapshot& before,const Disk& baseline,bool published){
    catalog::RecoveryReport r;
    Check(store.RecoverOwned(r)==catalog::Result::Ok,"real owned recovery succeeds");
    Check(r.confirmedPublication==published&&r.rolledBack==!published&&r.revision==(published?(before.IsValid()?2:1):(before.IsValid()?1:0)),"recovery independently reports committed or rolled-back revision");
    Check(fs::is_empty(path/L"operations"),"recovery removes all owned intent and decision names");
    const auto disk=DiskFiles(path);BaselineUnchanged(baseline,disk);
    catalog::Snapshot reopened;const auto result=store.Read(reopened);
    if(published){
        Check(result==catalog::Result::Ok&&reopened.StoreId()==Owner()&&reopened.Revision()==(before.IsValid()?2:1)&&reopened.Bytes()==NewBytes(),"committed bytes and exact chosen owner survive recovery");
        Check(disk.size()==baseline.size()+2,"confirmed publication keeps precisely both new generations");
        if(before.IsValid()){auto old=before;Check(store.Commit(before,OldBytes(),old)==catalog::Result::Conflict&&Same(old,before),"stale CAS cannot repeat already-published change");}
    }else{
        Check(disk==baseline,"rollback removes only the two registered generations and operation records");
        Check(before.IsValid()?(result==catalog::Result::Ok&&Same(reopened,before)):(result==catalog::Result::Missing&&!reopened.IsValid()),"rollback leaves exact original head or genuinely empty catalog");
        catalog::Snapshot next;
        Check((before.IsValid()?store.Commit(before,NewBytes(),next):store.InitializeOwned(Owner(),NewBytes(),next))==catalog::Result::Ok,"actual publication works after rollback");
        Check(store.Read(reopened)==catalog::Result::Ok&&Same(reopened,next),"subsequent publication reopens from real disk");
    }
    catalog::RecoveryReport again;
    Check(store.RecoverOwned(again)==catalog::Result::Ok&&!again.confirmedPublication&&!again.rolledBack,"no-operation recovery validates normal head without claiming recovery work");
}
void PublicationCases(const fs::path& root){
    for(bool initialize:{false,true})for(unsigned s=0;s<=unsigned(Stage::CommitFlush);++s){
        const auto leaf=std::wstring(initialize?L"initialize-":L"commit-")+std::to_wstring(s);const auto path=root/leaf;
        auto before=Seed(path,initialize);const auto baseline=DiskFiles(path);catalog::Store store(path);
        RunWorker(root,leaf,Stage(s),initialize);PendingGates(store,before);
        if(s==unsigned(Stage::IntentHeader)){Reject(store,path,catalog::Result::RecoveryRequired);continue;}
        const auto intent=ReadIntent(path);
        Check(intent.record.publication.owner==Owner()&&intent.record.publication.revision==(initialize?1:2),"intent records exact requested owner and next revision");
        const auto snapshot=Read(path/L"snapshots"/intent.record.snapshot.name),commit=Read(path/L"commits"/intent.record.commit.name);
        if(s==unsigned(Stage::IntentFlush))Check(snapshot.empty()&&commit.empty(),"durable intent precedes any prepared file content");
        if(s==unsigned(Stage::SnapshotHeader))Check(snapshot.size()==128&&commit.empty(),"actual partial snapshot and empty prepared commit remain");
        if(s>=unsigned(Stage::CommitHeader)){
            const auto wanted=NewBytes();Check(snapshot.size()==128+wanted.size()&&std::equal(wanted.begin(),wanted.end(),snapshot.begin()+128),"snapshot contains independent exact author bytes");
            Check(commit.size()==(s==unsigned(Stage::CommitHeader)?128:496),"commit has exact partial or full envelope length");
        }
        FinishRecovery(store,path,before,baseline,s==unsigned(Stage::CommitFlush));
    }
}
void RecoveryCrashes(const fs::path& root){
    for(unsigned s=unsigned(Stage::DecisionHeader);s<unsigned(Stage::Count);++s){
        const auto leaf=L"recover-"+std::to_wstring(s);const auto path=root/leaf;
        auto before=Seed(path);const auto baseline=DiskFiles(path);catalog::Store store(path);
        RunWorker(root,leaf,Stage::CommitHeader);const auto intent=ReadIntent(path);
        RunWorker(root,leaf,Stage(s));PendingGates(store,before);
        if(s==unsigned(Stage::DecisionHeader)){Reject(store,path,catalog::Result::RecoveryRequired);continue;}
        if(s==unsigned(Stage::DeleteCommit))Check(!fs::exists(path/L"commits"/intent.record.commit.name)&&fs::exists(path/L"snapshots"/intent.record.snapshot.name),"after crash only commit is absent");
        if(s==unsigned(Stage::DeleteIntent))Check(!fs::exists(path/L"commits"/intent.record.commit.name)&&!fs::exists(path/L"snapshots"/intent.record.snapshot.name)&&!fs::exists(path/L"operations"/intent.info.name),"decision alone authorizes resumed completed rollback");
        FinishRecovery(store,path,before,baseline,false);
    }
}
void FlipLast(const fs::path& path){
    auto bytes=Read(path);Check(!bytes.empty(),"own corruption target nonempty");
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    LARGE_INTEGER p{};p.QuadPart=bytes.size()-1;DWORD done=0;const unsigned char value=bytes.back()^0x80;
    Check(h.value!=INVALID_HANDLE_VALUE&&SetFilePointerEx(h.value,p,nullptr,FILE_BEGIN)&&WriteFile(h.value,&value,1,&done,nullptr)&&done==1&&FlushFileBuffers(h.value),"corrupt actual same-identity byte without resizing");
}
void RefusalCases(const fs::path& root){
    for(unsigned scenario=0;scenario<9;++scenario){
        const auto leaf=L"refuse-"+std::to_wstring(scenario);const auto path=root/leaf;
        auto before=Seed(path);catalog::Store store(path);
        RunWorker(root,leaf,scenario<2?Stage::CommitFlush:Stage::CommitHeader);
        const auto intent=ReadIntent(path);
        if(scenario==0)FlipLast(path/L"commits"/intent.record.commit.name);
        if(scenario==1||scenario==2)FlipLast(path/L"snapshots"/intent.record.snapshot.name);
        if(scenario==3){
            const auto target=path/L"commits"/intent.record.commit.name;const auto bytes=Read(target);
            Check(MoveFileW(target.c_str(),(root/(leaf+L"-original.blob")).c_str())!=FALSE,"retain original file outside catalog to prevent file-ID reuse");
            NewFile(target,bytes.data(),DWORD(bytes.size()));
        }
        if(scenario==4){const char x='x';NewFile(path/L"operations"/L"unexpected.txt",&x,1);}
        if(scenario==5){
            // Namespace-owner corruption with a valid envelope/hash, not merely a broken checksum.
            auto raw=Read(path/L"operations"/intent.info.name);Bytes payload(raw.begin()+128,raw.end());
            files::Directory dir;Check(files::Directory::Open(path/L"operations",false,dir)==files::Result::Ok,"open own wrong-owner fixture");
            files::Spec spec=intent.info.spec;spec.owner=Owner();files::BlobInfo added;
            Check(dir.Create(spec,payload.data(),payload.size(),added)==files::Result::Ok,"create independently valid but wrong-namespace intent");
            Check(DeleteFileW((path/L"operations"/intent.info.name).c_str())!=FALSE,"replace only own original intent fixture");
        }
        if(scenario==6){
            files::Directory dir;files::ReadLease lease;
            Check(files::Directory::Open(path/L"snapshots",false,dir)==files::Result::Ok&&dir.Acquire(intent.record.publication.snapshot,lease)==files::Result::Ok,"hold real read lease on planned complete snapshot");
            Reject(store,path,catalog::Result::Busy);
        }else if(scenario==7){
            Handle held(CreateFileW((path/L"commits"/intent.record.commit.name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            Check(held.value!=INVALID_HANDLE_VALUE,"hold exact partial commit read handle");Reject(store,path,catalog::Result::Busy);
        }else if(scenario==8){
            Check(DeleteFileW((path/L"commits"/intent.record.commit.name).c_str())!=FALSE,"remove own prepared commit without rollback authorization");
            Reject(store,path,catalog::Result::RecoveryRequired);
        }else Reject(store,path,catalog::Result::RecoveryRequired);
        PendingGates(store,before);
        if(scenario==6||scenario==7){
            catalog::RecoveryReport r;Check(store.RecoverOwned(r)==catalog::Result::Ok&&r.rolledBack,"after genuine lease release recovery resumes");
            catalog::Snapshot out;Check(store.Read(out)==catalog::Result::Ok&&Same(out,before),"lease refusal did not damage rollback head");
        }
    }
}
void UnregisteredPreparedCase(const fs::path& root){
    const auto path=root/L"unregistered-prepared";Seed(path,true);catalog::Store store(path);
    {
        files::Directory dir;Check(files::Directory::Open(path/L"snapshots",false,dir)==files::Result::Ok,"open own direct prepare boundary");
        files::Spec spec;spec.owner=Owner();spec.kind=files::Kind::CatalogSnapshot;spec.key=files::Digest{1};
        files::PreparedWrite prepared;const auto payload=NewBytes();
        Check(dir.Prepare(spec,payload.data(),payload.size(),prepared)==files::Result::Ok&&prepared.IsValid()&&prepared.Plan().createdIdentity.fileBytes==0,"real exclusive preparation before any intent");
        // Closing this real object leaves the same unknown zero-length residue
        // as process termination before intent creation. This is a direct
        // primitive boundary check, not claimed as a Publish crash injection.
    }
    Check(fs::is_empty(path/L"operations"),"unregistered preparation has no authorization record");
    Reject(store,path,catalog::Result::RecoveryRequired);
    const auto disk=DiskFiles(path);catalog::Snapshot output;
    Check(store.Read(output)==catalog::Result::RecoveryRequired&&!output.IsValid(),"unregistered zero-length generation cannot silently initialize");
    Check(store.InitializeOwned(Owner(),OldBytes(),output)==catalog::Result::RecoveryRequired&&!output.IsValid()&&DiskFiles(path)==disk,"unknown preparation cannot be overwritten or implicitly deleted");
}
void ContradictoryDecisionCase(const fs::path& root){
    const std::wstring leaf=L"decision-complete-conflict";const auto path=root/leaf;
    auto before=Seed(path);catalog::Store store(path);
    RunWorker(root,leaf,Stage::CommitHeader);const auto intent=ReadIntent(path);
    RunWorker(root,leaf,Stage::DecisionFlush);
    // Deliberately introduce contradictory on-disk evidence through the real
    // prepared-file API. A trusted rollback record is not blanket deletion
    // authority for a now-complete publication.
    {
        files::Directory dir;files::PreparedWrite prepared;
        Check(files::Directory::Open(path/L"commits",false,dir)==files::Result::Ok&&dir.ReopenPrepared(intent.record.commit,prepared)==files::Result::Ok,"open exact own partial commit for contradiction fixture");
        Bytes bytes;Check(codec::Encode(intent.record.publication,bytes)==codec::Result::Ok,"encode intended original commit without changing identity");
        files::BlobInfo completed;
        Check(prepared.Complete(bytes.data(),bytes.size(),completed)==files::Result::Ok&&completed.identity.fileIndex==intent.record.commit.createdIdentity.fileIndex,"same planned commit now complete and independently validated");
    }
    Reject(store,path,catalog::Result::RecoveryRequired);PendingGates(store,before);
}
void RecoveryFlushFailures(const fs::path& root){
    for(unsigned target=0;target<3;++target)for(bool failBefore:{false,true}){
        const int kind=target==2?3:int(target);
        const auto leaf=L"flush-failure-"+std::to_wstring(target)+(failBefore?L"-before":L"-after");
        const auto path=root/leaf;auto before=Seed(path);const auto baseline=DiskFiles(path);catalog::Store store(path);
        // Confirmation starts after a real successful commit flush. Rollback
        // starts after an independently interrupted recovery has already
        // written and flushed its decision; it must refresh that same file.
        RunWorker(root,leaf,target==2?Stage::CommitHeader:Stage::CommitFlush);
        const auto intent=ReadIntent(path);Evidence decision{};
        if(target==2){
            decision=RunWorker(root,leaf,Stage::DecisionFlush);
            Check(decision.names[3][0]&&fs::exists(path/L"operations"/decision.names[3]),"actual existing rollback decision remains for flush retry");
        }
        const auto pending=DiskFiles(path);catalog::RecoveryReport report;
        report.confirmedPublication=true;report.rolledBack=true;report.revision=777;
        {
            Imports imports;Fault fault(path,Stage::Count,INVALID_HANDLE_VALUE);
            std::copy_n(intent.record.snapshot.name.c_str(),38,fault.evidence.names[0]);
            std::copy_n(intent.record.commit.name.c_str(),38,fault.evidence.names[1]);
            std::copy_n(intent.info.name.c_str(),38,fault.evidence.names[2]);
            if(target==2)std::copy_n(decision.names[3],38,fault.evidence.names[3]);
            fault.failingFlush=kind;fault.failBeforeFlush=failBefore;
            catalog::Result result;
            {Arm arm(fault);result=store.RecoverOwned(report);}
            Check(result==catalog::Result::Io&&Sentinel(report),"actual recovery flush failure is Io and preserves caller report");
            Check(fault.flushFaults==1,"exactly one intended same-thread exact-generation FlushFileBuffers failure injected");
            for(unsigned k=0;k<4;++k){
                const DWORD expected=k==unsigned(kind)?(failBefore?0u:1u):(target==1&&k==0?1u:0u);
                Check(fault.evidence.flushes[k]==expected,"native flush successes show exact recovery ordering and rejected target");
                Check(fault.evidence.deleteAttempts[k]==0&&fault.evidence.deletes[k]==0,"flush failure does not even attempt disposition of any plan or operation");
                Check(fault.evidence.headers[k]==0&&fault.evidence.payloads[k]==0,"retry failure neither rewrites evidence nor creates a new publication");
            }
            Check(DiskFiles(path)==pending,"all operation and target names and bytes remain exact after failed flush");
        }
        Check(!restoreFailed&&!active,"returning fault restores own IAT and thread scope before retry");
        PendingGates(store,before);
        Check(DiskFiles(path)==pending,"ordinary entry points do not consume failed-flush recovery evidence");
        FinishRecovery(store,path,before,baseline,target!=2);
        std::printf("FLUSH recovery-target=%d fault=%s hits=1 retry=ok\n",kind,failBefore?"before-native":"after-native-success");
    }
}
void NormalChecks(const fs::path& root){
    const auto path=root/L"normal";catalog::Store store(path);catalog::Snapshot a,b,c,reopened;
    Check(store.Initialize(OldBytes(),a)==catalog::Result::Ok&&a.Revision()==1&&a.Bytes()==OldBytes(),"unchanged default Initialize publishes actual bytes");
    Check(store.Commit(a,NewBytes(),b)==catalog::Result::Ok&&b.Revision()==2&&b.StoreId()==a.StoreId(),"normal actual Commit after default initialization");
    catalog::RecoveryReport report;
    Check(store.RecoverOwned(report)==catalog::Result::Ok&&!report.confirmedPublication&&!report.rolledBack&&report.revision==2,"empty operations recovery validates actual head");
    Check(store.Read(reopened)==catalog::Result::Ok&&Same(reopened,b),"ordinary publication independently rereads");
    Check(store.Checkpoint(b,c)==catalog::Result::Ok&&c.Revision()==3&&c.Bytes()==NewBytes(),"checkpoint remains available after intent cleanup");
    catalog::CollectionReport collection;
    Check(store.CollectRetired(collection)==catalog::Result::Ok&&collection.attempted>0&&collection.absent==collection.attempted&&collection.busy==0&&collection.pending==0&&collection.failed==0,"real retired metadata collection after owned publications");
    Check(store.Read(reopened)==catalog::Result::Ok&&Same(reopened,c),"collection retains verified checkpoint and payload");
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try{
        if(argc==7&&std::wstring(argv[1])==L"--worker"){
            wchar_t* end=nullptr;const auto stage=wcstoul(argv[4],&end,10);Check(end&&!*end&&stage<unsigned(Stage::Count),"valid stage");
            const auto init=wcstoul(argv[5],&end,10);Check(end&&!*end&&init<=1,"valid initialization flag");
            const auto parent=wcstoul(argv[6],&end,10);Check(end&&!*end&&parent,"valid parent PID");
            Worker(fs::path(argv[2]),argv[3],Stage(stage),init!=0,DWORD(parent));return 0;
        }
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIRECTORY");const fs::path root(argv[2]);Absolute(root);
        Check(fs::is_directory(root.parent_path())&&CreateDirectoryW(root.c_str(),nullptr),"claim fresh owned test root");
        const auto marker=Marker(GetCurrentProcessId());NewFile(root/markerName,marker.data(),DWORD(marker.size()));
        NormalChecks(root);PublicationCases(root);RecoveryCrashes(root);RefusalCases(root);UnregisteredPreparedCase(root);ContradictoryDecisionCase(root);RecoveryFlushFailures(root);
        Check(!restoreFailed&&!active,"parent retains no IAT patch or active fault");
        std::printf("PASS cache catalog owned recovery checks=%u root=%ls; real process exits, not power loss\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog owned recovery checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog owned recovery unexpected exception\n");return 2;}
}
