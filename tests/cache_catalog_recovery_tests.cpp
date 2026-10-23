// MAINTENANCE: regression-keep. Real process exits and owned EXE IAT faults; not power-loss coverage.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_catalog.hpp"
#include "../src/io/cache_catalog_codec.hpp"
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
constexpr wchar_t markerName[]=L"catalog-recovery-test.marker";
constexpr wchar_t caseNames[][32]={L"snapshot-header",L"snapshot-flush",L"commit-header",L"commit-flush",L"flush-cleanup",L"flush-orphan"};
enum class Mode:unsigned { SnapshotHeader,SnapshotFlush,CommitHeader,CommitFlush,FlushCleanup,FlushOrphan };
std::string Marker(DWORD pid){return "MMS cache catalog recovery test v1\nparent="+std::to_string(pid)+"\n";}
Bytes Read(const fs::path& path){std::ifstream in(path,std::ios::binary);Check(bool(in),"open own evidence");Bytes b{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};Check(!in.bad(),"read complete own evidence");return b;}
void NewFile(const fs::path& path,const void* data,DWORD size){
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;
    Check(h.value!=INVALID_HANDLE_VALUE&&WriteFile(h.value,data,size,&done,nullptr)&&done==size&&FlushFileBuffers(h.value),"create and flush own control evidence");
}
void Absolute(const fs::path& p){Check(p.is_absolute()&&p.has_filename()&&p!=p.root_path()&&p==p.lexically_normal(),"normalized absolute owned root");}
Bytes OldBytes(){return {7,0,0xff,19,3};}
Bytes NewBytes(){Bytes b(65536+191);for(std::size_t i=0;i<b.size();++i)b[i]=std::uint8_t(i*31+83);return b;}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
catalog::Result ReadRetry(catalog::Store& store,catalog::Snapshot& out){
    const auto start=GetTickCount64();catalog::Result r;
    do {r=store.Read(out);if(r!=catalog::Result::Busy)return r;Sleep(5);}while(GetTickCount64()-start<2000);
    return r;
}
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
    DWORD magic=0x43524331,mode=0,stage=0;
    DWORD headers[2]{},payloadWrites[2]{},flushes[2]{},dispositions=0,dispositionSuccess=0,flushFaults=0,deleteFaults=0;
    DWORD commitResult=0,outputUnchanged=0;
    wchar_t names[2][38]{};
};
struct Fault {
    std::wstring prefixes[2];DWORD thread=GetCurrentThreadId();Mode mode;
    Evidence evidence{};HANDLE control=INVALID_HANDLE_VALUE;
    Fault(const fs::path& root,Mode m,HANDLE file):prefixes{Prefix(root/L"snapshots"),Prefix(root/L"commits")},mode(m),control(file){evidence.mode=unsigned(m);}
    int Match(HANDLE h) noexcept {
        if(GetCurrentThreadId()!=thread)return -1;
        wchar_t path[32768]{};const DWORD n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);
        if(!n||n>=32768)return -1;
        for(int kind=0;kind<2;++kind){const auto& prefix=prefixes[kind];
            if(n!=prefix.size()+37||_wcsnicmp(path,prefix.c_str(),prefix.size()))continue;
            const auto* leaf=path+prefix.size();
            for(unsigned i=0;i<32;++i)if(!((leaf[i]>=L'0'&&leaf[i]<=L'9')||(leaf[i]>=L'a'&&leaf[i]<=L'f')))return -1;
            if(std::wcscmp(leaf+32,L".blob"))return -1;
            if(evidence.names[kind][0]&&std::wcscmp(evidence.names[kind],leaf))return -1;
            std::copy_n(leaf,38,evidence.names[kind]);return kind;
        }
        return -1;
    }
    void Save() noexcept {
        LARGE_INTEGER zero{};DWORD done=0;
        if(!SetFilePointerEx(control,zero,nullptr,FILE_BEGIN)||!realWrite(control,&evidence,sizeof evidence,&done,nullptr)||
            done!=sizeof evidence||!realFlush(control))ExitProcess(239);
    }
    [[noreturn]] void Crash(DWORD stage) noexcept {evidence.stage=stage;Save();ExitProcess(171+unsigned(mode));}
};
thread_local Fault* active=nullptr;
struct Arm { explicit Arm(Fault& f){Check(!active,"only one scoped fault");active=&f;}~Arm(){active=nullptr;} };
BOOL WINAPI WriteHook(HANDLE h,LPCVOID data,DWORD bytes,LPDWORD written,LPOVERLAPPED ov) noexcept {
    auto* f=active;const int k=f?f->Match(h):-1;
    if(k<0||ov)return realWrite(h,data,bytes,written,ov);
    LARGE_INTEGER zero{},position{};
    if(!SetFilePointerEx(h,zero,&position,FILE_CURRENT))return realWrite(h,data,bytes,written,ov);
    const BOOL ok=realWrite(h,data,bytes,written,ov);
    if(!ok||!written||*written!=bytes)return ok;
    if(position.QuadPart==0&&bytes==128){
        ++f->evidence.headers[k];
        if((k==0&&f->mode==Mode::SnapshotHeader)||(k==1&&f->mode==Mode::CommitHeader))f->Crash(1);
    }else if(position.QuadPart>=128)++f->evidence.payloadWrites[k];
    return ok;
}
BOOL WINAPI FlushHook(HANDLE h) noexcept {
    auto* f=active;const int k=f?f->Match(h):-1;
    if(k<0)return realFlush(h);
    const BOOL ok=realFlush(h);if(!ok)return ok;
    ++f->evidence.flushes[k];
    if((k==0&&f->mode==Mode::SnapshotFlush)||(k==1&&f->mode==Mode::CommitFlush))f->Crash(2);
    if(k==1&&(f->mode==Mode::FlushCleanup||f->mode==Mode::FlushOrphan)){
        ++f->evidence.flushFaults;SetLastError(ERROR_WRITE_FAULT);return FALSE;
    }
    return ok;
}
BOOL WINAPI InfoHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size) noexcept {
    auto* f=active;
    if(!f||kind!=FileDispositionInfo||size!=sizeof(FILE_DISPOSITION_INFO)||!data||
        !static_cast<FILE_DISPOSITION_INFO*>(data)->DeleteFile||f->Match(h)!=1)return realInfo(h,kind,data,size);
    ++f->evidence.dispositions;
    if(f->mode==Mode::FlushOrphan){++f->evidence.deleteFaults;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    const BOOL ok=realInfo(h,kind,data,size);if(ok)++f->evidence.dispositionSuccess;return ok;
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

void Worker(const fs::path& root,unsigned mode,DWORD parentPid){
    Absolute(root);Check(mode<6,"known worker case only");
    const auto marker=Read(root/markerName);const auto expected=Marker(parentPid);
    Check(std::string(marker.begin(),marker.end())==expected,"worker validates exact parent installation marker");
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));
    Check(parent.value&&parent.value!=INVALID_HANDLE_VALUE&&WaitForSingleObject(parent.value,0)==WAIT_TIMEOUT,"owned launching parent remains alive");
    const auto path=root/caseNames[mode];catalog::Store store(path);catalog::Snapshot before;
    Check(ReadRetry(store,before)==catalog::Result::Ok&&before.Revision()==1&&before.Bytes()==OldBytes(),"worker enters actual committed revision one");
    auto output=before;const auto payload=NewBytes();
    Handle control(CreateFileW((root/(std::wstring(caseNames[mode])+L".control")).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(control.value!=INVALID_HANDLE_VALUE,"create unique own worker control record");
    {
        Imports imports;Fault fault(path,Mode(mode),control.value);catalog::Result result;
        {Arm arm(fault);result=store.Commit(before,payload,output);}
        // Crash modes must never finish Commit: this turns a missing hook into
        // an explicit failure, not a false successful crash test.
        Check(mode>=4,"hard-exit injection reached its exact real write stage");
        fault.evidence.commitResult=DWORD(result);fault.evidence.outputUnchanged=Same(output,before)?1:0;fault.evidence.stage=3;fault.Save();
        Check(result==catalog::Result::Indeterminate&&Same(output,before),"commit attempt failure is Indeterminate and retains previous output");
        Check(fault.evidence.flushFaults==1&&fault.evidence.dispositions==1,"real commit Flush failure triggers one same-handle cleanup");
        Check(mode==4?(fault.evidence.dispositionSuccess==1&&fault.evidence.deleteFaults==0):
            (fault.evidence.dispositionSuccess==0&&fault.evidence.deleteFaults==1),"cleanup success/failure uses the actual chosen commit generation");
    }
    Check(!restoreFailed&&!active,"normal worker restores every IAT patch before exit");
}

std::wstring Quote(const std::wstring& value){Check(value.find(L'"')==value.npos,"owned argument has no quotes");return L"\""+value+L"\"";}
DWORD RunWorker(const fs::path& root,unsigned mode){
    wchar_t exe[32768]{};const DWORD n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"own test executable path");
    auto line=Quote(exe)+L" --worker "+Quote(root.wstring())+L" "+std::to_wstring(mode)+L" "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe,line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&process)!=FALSE,"start only own hidden worker without inherited handles");
    Handle thread(process.hThread),child(process.hProcess);
    const auto wait=WaitForSingleObject(child.value,30000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(child.value,238);WaitForSingleObject(child.value,5000);Check(false,"owned worker finished within bounded wait");}
    DWORD code=0;Check(GetExitCodeProcess(child.value,&code)!=FALSE,"collect exact owned worker terminal exit code");return code;
}
using Disk=std::map<std::wstring,Bytes>;
Disk BlobFiles(const fs::path& dir){
    Disk result;for(const auto& item:fs::directory_iterator(dir)){
        if(item.path().filename()==L".lock")continue;
        Check(item.is_regular_file()&&item.path().extension()==L".blob","only expected real catalog generation files");
        result.emplace(item.path().filename().wstring(),Read(item.path()));
    }return result;
}
void BaselineUnchanged(const Disk& before,const Disk& after){for(const auto& item:before){auto it=after.find(item.first);Check(it!=after.end()&&it->second==item.second,"all previous generation bytes remain exact");}}
std::wstring Added(const Disk& before,const Disk& after){std::wstring name;for(const auto& item:after)if(!before.count(item.first)){Check(name.empty(),"exactly one newly written generation per directory");name=item.first;}Check(!name.empty(),"new real generation exists");return name;}
void Payload(const Bytes& raw,const Bytes& expected){Check(raw.size()==128+expected.size()&&std::equal(expected.begin(),expected.end(),raw.begin()+128),"independent raw disk bytes match authored snapshot payload");}
void CheckRecord(const fs::path& path,const std::wstring& name,const catalog::Snapshot& before,const std::wstring& snapshot){
    files::Directory directory;Check(files::Directory::Open(path/L"commits",false,directory)==files::Result::Ok,"open actual commit directory");
    files::ReadLease lease;Check(directory.Inspect(name,codec::NamespaceOwner,codec::RecordBytes,lease)==files::Result::Ok,"new full commit passes actual envelope and digest validation");
    Bytes bytes;Check(lease.ReadAll(bytes)==files::Result::Ok,"read full actual commit payload");codec::Record record;
    Check(codec::Decode(bytes.data(),bytes.size(),record)==codec::Result::Ok&&record.revision==2&&record.owner==before.StoreId()&&record.snapshot.name==snapshot,
        "full commit names the new snapshot and exact next revision/store owner");
}
void Case(const fs::path& root,unsigned mode){
    const auto path=root/caseNames[mode];catalog::Store store(path);catalog::Snapshot baseline;
    Check(store.Initialize(OldBytes(),baseline)==catalog::Result::Ok&&baseline.Revision()==1,"parent publishes real initial catalog");
    const auto oldSnapshots=BlobFiles(path/L"snapshots"),oldCommits=BlobFiles(path/L"commits");
    Check(oldSnapshots.size()==1&&oldCommits.size()==1,"revision one has actual snapshot and commit");
    const DWORD exit=RunWorker(root,mode);Check(exit==(mode<4?171+mode:0),"worker exits at calibrated stage with exact code");
    const auto control=Read(root/(std::wstring(caseNames[mode])+L".control"));Check(control.size()==sizeof(Evidence),"complete standalone stage evidence");Evidence e{};std::memcpy(&e,control.data(),sizeof e);
    Check(e.magic==0x43524331&&e.mode==mode&&e.stage==(mode==0||mode==2?1:mode<4?2:3),"stage evidence identifies the intended interruption");
    Check(e.headers[0]==1&&e.names[0][0],"production actually wrote one new snapshot header");
    const auto snapshots=BlobFiles(path/L"snapshots"),commits=BlobFiles(path/L"commits");BaselineUnchanged(oldSnapshots,snapshots);BaselineUnchanged(oldCommits,commits);
    const auto snapshot=Added(oldSnapshots,snapshots);Check(snapshot==e.names[0]&&snapshots.size()==2,"parent sees precisely the snapshot recorded by the owned hook");
    if(mode==0){Check(snapshots.at(snapshot).size()==128&&e.payloadWrites[0]==0&&e.flushes[0]==0,"snapshot interruption follows real header before any payload");}
    else {Payload(snapshots.at(snapshot),NewBytes());Check(e.payloadWrites[0]>0&&e.flushes[0]==1,"snapshot payload really written and real Flush succeeded");}
    if(mode<2){Check(commits==oldCommits&&e.headers[1]==0&&e.flushes[1]==0,"snapshot interruption never begins a commit record");}
    else {
        Check(e.headers[1]==1&&e.names[1][0],"production enters commit record write");
        if(mode==4){Check(commits==oldCommits&&!fs::exists(path/L"commits"/e.names[1]),"real successful disposition removed only failed new commit");}
        else {const auto commit=Added(oldCommits,commits);Check(commit==e.names[1]&&commits.size()==2,"parent sees precisely the recorded new commit");
            if(mode==2)Check(commits.at(commit).size()==128&&e.payloadWrites[1]==0&&e.flushes[1]==0,"commit interruption follows real header before payload");
            else {Check(e.payloadWrites[1]>0&&e.flushes[1]==1,"complete commit passes real Flush before reported fault or exit");CheckRecord(path,commit,baseline,snapshot);}
        }
    }
    if(mode>=4){Check(e.commitResult==DWORD(catalog::Result::Indeterminate)&&e.outputUnchanged==1&&e.flushFaults==1&&e.dispositions==1,
        "worker independently observed Indeterminate with unchanged output and one cleanup");
        Check(mode==4?(e.dispositionSuccess==1&&e.deleteFaults==0):(e.dispositionSuccess==0&&e.deleteFaults==1),"controlled disposition result is exact");}
    auto output=baseline;const auto r=ReadRetry(store,output);
    if(mode==0||mode==2){Check(r==catalog::Result::RecoveryRequired&&Same(output,baseline),"partial snapshot/commit demands recovery, never silently falls back or mutates output");}
    else if(mode==1||mode==4){
        Check(r==catalog::Result::Ok&&Same(output,baseline),"unreferenced complete snapshot leaves current revision one intact");
        const auto orphanBytes=snapshots.at(snapshot);catalog::Snapshot next;
        Check(store.Commit(output,NewBytes(),next)==catalog::Result::Ok&&next.Revision()==2&&next.Bytes()==NewBytes(),"after cleanly absent commit a real later Commit succeeds");
        catalog::Snapshot reopened;Check(ReadRetry(store,reopened)==catalog::Result::Ok&&Same(reopened,next),"later successful commit is independently readable from actual files");
        Check(Read(path/L"snapshots"/snapshot)==orphanBytes,"later successful commit keeps the complete original orphan unchanged");
    }else {
        Check(r==catalog::Result::Ok&&output.Revision()==2&&output.StoreId()==baseline.StoreId()&&output.Bytes()==NewBytes(),"real complete commit is revision two even when its caller observed Indeterminate");
        auto unchanged=baseline;Check(store.Commit(baseline,OldBytes(),unchanged)==catalog::Result::Conflict&&Same(unchanged,baseline),"stale retry after persisted commit conflicts instead of applying again");
    }
    std::printf("CASE %ls exit=%lu read=%u snapshot-bytes=%zu commits=%zu\n",caseNames[mode],exit,unsigned(r),snapshots.at(snapshot).size(),commits.size());
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try {
        if(argc==5&&std::wstring(argv[1])==L"--worker"){
            wchar_t* end=nullptr;const auto mode=wcstoul(argv[3],&end,10);Check(end&&!*end&&mode<6,"valid worker mode");
            const auto parent=wcstoul(argv[4],&end,10);Check(end&&!*end&&parent!=0,"valid parent PID");Worker(fs::path(argv[2]),unsigned(mode),DWORD(parent));return 0;
        }
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIRECTORY");const fs::path root(argv[2]);Absolute(root);
        Check(fs::is_directory(root.parent_path())&&CreateDirectoryW(root.c_str(),nullptr),"claim fresh owned test root");
        const auto marker=Marker(GetCurrentProcessId());NewFile(root/markerName,marker.data(),DWORD(marker.size()));
        for(unsigned mode=0;mode<6;++mode)Case(root,mode);
        std::printf("PASS cache catalog recovery checks=%u root=%ls; process/API faults only, not power loss\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog recovery checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog recovery unexpected exception\n");return 2;}
}
