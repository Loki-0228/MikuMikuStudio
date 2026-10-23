// MAINTENANCE: regression-keep. Own EXE IAT and process exits, not power loss.
// No UI, GPU, global injection, external data or recursive cleanup.
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
void Check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
};
constexpr wchar_t markerName[]=L"catalog-maintenance-recovery.marker";
constexpr wchar_t caseNames[][32]={L"checkpoint-header",L"checkpoint-flush",L"collect-interrupted",L"collect-failed"};
enum class Mode:unsigned { Header,Flush,CollectExit,CollectFailure };
std::string Marker(DWORD pid){return "MMS cache catalog maintenance recovery test v1\nparent="+std::to_string(pid)+"\n";}
void Absolute(const fs::path& p){Check(p.is_absolute()&&p.has_filename()&&p!=p.root_path()&&p==p.lexically_normal(),"normalized absolute owned root");}
Bytes Read(const fs::path& p){std::ifstream in(p,std::ios::binary);Check(bool(in),"open own file evidence");Bytes b{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};Check(!in.bad(),"read complete owned file");return b;}
void NewFile(const fs::path& p,const void* bytes,DWORD size){
    Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;
    Check(h.value!=INVALID_HANDLE_VALUE&&WriteFile(h.value,bytes,size,&done,nullptr)&&done==size&&FlushFileBuffers(h.value),"create and flush owned control file");
}
Bytes First(){return {1,17,0,255,6};}
Bytes Latest(){Bytes b(8192+137);for(std::size_t i=0;i<b.size();++i)b[i]=std::uint8_t(i*47+23);return b;}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
catalog::Result ReadRetry(catalog::Store& store,catalog::Snapshot& out){
    const auto start=GetTickCount64();catalog::Result result;
    do{result=store.Read(out);if(result!=catalog::Result::Busy)return result;Sleep(5);}while(GetTickCount64()-start<2000);
    return result;
}
std::wstring Prefix(const fs::path& path){
    Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    wchar_t p[32768]{};const DWORD n=GetFinalPathNameByHandleW(h.value,p,32768,VOLUME_NAME_GUID);
    Check(h.value!=INVALID_HANDLE_VALUE&&n&&n<32768,"resolve owned injection directory from actual handle");
    std::wstring result(p,n);if(result.back()!=L'\\')result+=L'\\';return result;
}
using Disk=std::map<std::wstring,Bytes>; // Relative directory/name, not just basename.
Disk DiskFiles(const fs::path& root){
    Disk result;for(const auto* folder:{L"snapshots",L"commits"})for(const auto& item:fs::directory_iterator(root/folder)){
        if(item.path().filename()==L".lock")continue;
        Check(item.is_regular_file()&&item.path().extension()==L".blob","owned catalog contains only expected generation files");
        result.emplace((fs::path(folder)/item.path().filename()).generic_wstring(),Read(item.path()));
    }return result;
}
std::wstring Relative(const files::BlobInfo& ref){return std::wstring(ref.spec.kind==files::Kind::CatalogSnapshot?L"snapshots/":L"commits/")+ref.name;}
codec::CheckpointRecord CheckpointRecord(const fs::path& root,const std::wstring& relative){
    Check(relative.rfind(L"commits/",0)==0,"checkpoint lives in owned commits directory");
    files::Directory directory;Check(files::Directory::Open(root/L"commits",false,directory)==files::Result::Ok,"open checkpoint directory");
    files::ReadLease lease;Check(directory.Inspect(fs::path(relative).filename().wstring(),codec::NamespaceOwner,codec::MaximumCheckpointBytes,lease)==files::Result::Ok,"checkpoint envelope and payload digest validate");
    Check(lease.Info().spec.kind==files::Kind::CatalogCheckpoint&&lease.Info().spec.key==codec::CheckpointKey,"checkpoint has actual maintenance envelope kind/key");
    Bytes bytes;Check(lease.ReadAll(bytes)==files::Result::Ok,"read exact verified checkpoint payload");codec::CheckpointRecord record;
    Check(codec::DecodeCheckpoint(bytes.data(),bytes.size(),record)==codec::Result::Ok,"decode actual complete checkpoint");return record;
}
std::wstring OneAdded(const Disk& before,const Disk& after){
    std::wstring result;for(const auto& item:after)if(!before.count(item.first)){Check(result.empty(),"exactly one new metadata generation");result=item.first;}
    Check(!result.empty(),"new checkpoint generation really exists");return result;
}
void Preserved(const Disk& before,const Disk& after){for(const auto& item:before){const auto it=after.find(item.first);Check(it!=after.end()&&it->second==item.second,"all prior metadata bytes stay unchanged");}}
void AssertHead(catalog::Store& store,const catalog::Snapshot& baseline,std::uint64_t revision){
    catalog::Snapshot now;Check(ReadRetry(store,now)==catalog::Result::Ok&&now.Revision()==revision&&now.StoreId()==baseline.StoreId()&&now.Bytes()==Latest(),"latest checkpoint remains readable with exact original payload/store identity");
}

using WriteFn=BOOL(WINAPI*)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
using FlushFn=BOOL(WINAPI*)(HANDLE);
using InfoFn=BOOL(WINAPI*)(HANDLE,FILE_INFO_BY_HANDLE_CLASS,LPVOID,DWORD);
WriteFn realWrite=nullptr;FlushFn realFlush=nullptr;InfoFn realInfo=nullptr;
struct Evidence {
    DWORD magic=0x434d5231,mode=0,stage=0;
    DWORD headers=0,payloadWrites=0,flushes=0,dispositions=0,deleteSuccess=0,deleteFaults=0;
    DWORD result=0,unchanged=0,attempted=0,absent=0,busy=0,pending=0,failed=0;
    std::uint64_t absentBytes=0;
    DWORD directory=0;
    wchar_t name[38]{};
};
struct Fault {
    std::wstring prefixes[2];
    std::vector<std::wstring> retired; // Fully prepared before arming; no allocation in hooks.
    Mode mode;DWORD thread=GetCurrentThreadId();HANDLE control;
    Evidence evidence{};
    Fault(const fs::path& path,Mode m,HANDLE file,std::vector<std::wstring> allowed):
        prefixes{Prefix(path/L"snapshots"),Prefix(path/L"commits")},retired(std::move(allowed)),mode(m),control(file){evidence.mode=unsigned(m);}
    int Match(HANDLE h,wchar_t (&leaf)[38]) noexcept {
        if(GetCurrentThreadId()!=thread)return -1;
        wchar_t path[32768]{};const DWORD n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);if(!n||n>=32768)return -1;
        for(int k=0;k<2;++k){const auto& prefix=prefixes[k];
            if(n!=prefix.size()+37||_wcsnicmp(path,prefix.c_str(),prefix.size()))continue;
            const auto* name=path+prefix.size();
            for(unsigned i=0;i<32;++i)if(!((name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f')))return -1;
            if(std::wcscmp(name+32,L".blob"))return -1;
            std::copy_n(name,38,leaf);return k;
        }return -1;
    }
    bool Retired(int k,const wchar_t* leaf) const noexcept {
        const auto* folder=k==0?L"snapshots/":L"commits/";const auto size=std::wcslen(folder);
        for(const auto& name:retired)if(name.size()==size+37&&name.compare(0,size,folder)==0&&name.compare(size,37,leaf)==0)return true;
        return false;
    }
    void Save() noexcept {
        LARGE_INTEGER zero{};DWORD written=0;
        if(!SetFilePointerEx(control,zero,nullptr,FILE_BEGIN)||!realWrite(control,&evidence,sizeof evidence,&written,nullptr)||written!=sizeof evidence||!realFlush(control))ExitProcess(239);
    }
    [[noreturn]] void Crash(DWORD stage) noexcept {evidence.stage=stage;Save();ExitProcess(181+unsigned(mode));}
};
thread_local Fault* active=nullptr;
struct Arm{explicit Arm(Fault& f){Check(!active,"one precise armed injection");active=&f;}~Arm(){active=nullptr;}};
BOOL WINAPI WriteHook(HANDLE h,LPCVOID data,DWORD bytes,LPDWORD written,LPOVERLAPPED ov) noexcept {
    auto* f=active;wchar_t leaf[38]{};const int k=f?f->Match(h,leaf):-1;
    if(k!=1||ov||unsigned(f->mode)>1)return realWrite(h,data,bytes,written,ov);
    LARGE_INTEGER zero{},position{};if(!SetFilePointerEx(h,zero,&position,FILE_CURRENT))return realWrite(h,data,bytes,written,ov);
    const BOOL ok=realWrite(h,data,bytes,written,ov);if(!ok||!written||*written!=bytes)return ok;
    if(position.QuadPart==0&&bytes==128){
        ++f->evidence.headers;std::copy_n(leaf,38,f->evidence.name);f->evidence.directory=1;
        if(f->mode==Mode::Header)f->Crash(1);
    }else if(position.QuadPart>=128)++f->evidence.payloadWrites;
    return ok;
}
BOOL WINAPI FlushHook(HANDLE h) noexcept {
    auto* f=active;wchar_t leaf[38]{};const int k=f?f->Match(h,leaf):-1;
    if(k!=1||unsigned(f->mode)>1||!f->evidence.name[0]||std::wcscmp(leaf,f->evidence.name))return realFlush(h);
    const BOOL ok=realFlush(h);if(ok){++f->evidence.flushes;if(f->mode==Mode::Flush)f->Crash(2);}return ok;
}
BOOL WINAPI InfoHook(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size) noexcept {
    auto* f=active;wchar_t leaf[38]{};
    if(!f||unsigned(f->mode)<2||kind!=FileDispositionInfo||size!=sizeof(FILE_DISPOSITION_INFO)||!data||!static_cast<FILE_DISPOSITION_INFO*>(data)->DeleteFile)return realInfo(h,kind,data,size);
    const int k=f->Match(h,leaf);if(k<0||!f->Retired(k,leaf))return realInfo(h,kind,data,size);
    ++f->evidence.dispositions;f->evidence.directory=DWORD(k);std::copy_n(leaf,38,f->evidence.name);
    if(f->mode==Mode::CollectFailure){++f->evidence.deleteFaults;SetLastError(ERROR_WRITE_FAULT);return FALSE;}
    const BOOL ok=realInfo(h,kind,data,size);if(ok){++f->evidence.deleteSuccess;f->Crash(3);}return ok;
}

// Current EXE imports only; all armed hooks additionally require this thread
// and an exact owned volume-GUID directory and valid generation name.
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
        Check(realWrite&&realFlush&&realInfo&&slots.size()>=3,"three actual production imports in own EXE");
        try{for(auto& s:slots){s.patched=true;Check(Set(s,s.after)&&*s.address==s.after,"install own imported pointer wrapper");}}catch(...){Restore();throw;}
    }
    ~Imports(){Restore();}
};

void Worker(const fs::path& root,unsigned mode,DWORD parentPid){
    Absolute(root);Check(mode<4,"known maintenance worker mode");
    const auto marker=Read(root/markerName);const auto expected=Marker(parentPid);
    Check(std::string(marker.begin(),marker.end())==expected,"exact same-parent installation marker");
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));
    Check(parent.value&&parent.value!=INVALID_HANDLE_VALUE&&WaitForSingleObject(parent.value,0)==WAIT_TIMEOUT,"owned launching parent remains alive");
    const auto path=root/caseNames[mode];catalog::Store store(path);catalog::Snapshot baseline;
    Check(ReadRetry(store,baseline)==catalog::Result::Ok&&baseline.Revision()==(mode<2?2:3)&&baseline.Bytes()==Latest(),"worker enters expected actual committed state");
    std::vector<std::wstring> retired;
    if(mode>=2){
        const auto all=DiskFiles(path);unsigned checkpoints=0;
        for(const auto& item:all){
            if(item.first.rfind(L"commits/",0)!=0||item.second.size()<128+16||std::memcmp(item.second.data()+128,"MMSCacheBase0001",16))continue;
            ++checkpoints;const auto record=CheckpointRecord(path,item.first);
            Check(record.state.revision==3&&record.state.owner==baseline.StoreId(),"worker uses actual newest checkpoint");
            for(const auto& ref:record.retired)retired.push_back(Relative(ref));
        }
        Check(checkpoints==1&&retired.size()==3,"three independently prepared old generations are authorized for collection");
    }
    Handle control(CreateFileW((root/(std::wstring(caseNames[mode])+L".control")).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(control.value!=INVALID_HANDLE_VALUE,"create unique worker evidence outside catalog");
    {
        Imports imports;Fault fault(path,Mode(mode),control.value,std::move(retired));
        auto output=baseline;catalog::CollectionReport report{};catalog::Result result;
        {Arm arm(fault);result=mode<2?store.Checkpoint(baseline,output):store.CollectRetired(report);}
        Check(mode==3,"hard-exit injection actually reached its production stage");
        fault.evidence.result=DWORD(result);fault.evidence.unchanged=Same(output,baseline)?1:0;
        fault.evidence.attempted=report.attempted;fault.evidence.absent=report.absent;fault.evidence.busy=report.busy;
        fault.evidence.pending=report.pending;fault.evidence.failed=report.failed;fault.evidence.absentBytes=report.absentBytes;
        fault.evidence.stage=4;fault.Save();
        Check(result==catalog::Result::Ok&&report.attempted==3&&report.failed==3&&report.absent==0&&report.absentBytes==0&&report.busy==0&&report.pending==0,
            "real collection reports three actual failures without pretending removal or returning setup failure");
        Check(fault.evidence.dispositions==3&&fault.evidence.deleteFaults==3&&fault.evidence.deleteSuccess==0,"all and only three actual retired handles hit injected failure");
    }
    Check(!restoreFailed&&!active,"normal worker restores IAT and thread fault pointer");
}

std::wstring Quote(const std::wstring& s){Check(s.find(L'"')==s.npos,"owned argument has no quote");return L"\""+s+L"\"";}
DWORD RunWorker(const fs::path& root,unsigned mode){
    wchar_t exe[32768]{};const DWORD n=GetModuleFileNameW(nullptr,exe,32768);Check(n&&n<32768,"own test executable path");
    auto line=Quote(exe)+L" --worker "+Quote(root.wstring())+L" "+std::to_wstring(mode)+L" "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION p{};
    Check(CreateProcessW(exe,line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&p)!=FALSE,"start only own hidden worker without inherited handles");
    Handle thread(p.hThread),child(p.hProcess);const auto wait=WaitForSingleObject(child.value,30000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(child.value,238);WaitForSingleObject(child.value,5000);Check(false,"owned child finishes within bounded wait");}
    DWORD code=0;Check(GetExitCodeProcess(child.value,&code)!=FALSE,"exact terminal child exit code");return code;
}
void ParentCase(const fs::path& root,unsigned mode){
    const auto path=root/caseNames[mode];catalog::Store store(path);catalog::Snapshot first,baseline;
    Check(store.Initialize(First(),first)==catalog::Result::Ok&&first.Revision()==1,"initialize actual catalog revision one");
    Check(store.Commit(first,Latest(),baseline)==catalog::Result::Ok&&baseline.Revision()==2,"publish a distinct actual latest payload");
    const auto old=DiskFiles(path);Check(old.size()==4,"two actual snapshots and two commit records");
    Disk before=old;std::wstring checkpoint;
    if(mode>=2){catalog::Snapshot compact;
        Check(store.Checkpoint(baseline,compact)==catalog::Result::Ok&&compact.Revision()==3&&compact.Bytes()==Latest(),"real checkpoint publishes same payload at next revision");
        baseline=compact;before=DiskFiles(path);Preserved(old,before);checkpoint=OneAdded(old,before);
    }
    const DWORD exit=RunWorker(root,mode);Check(exit==(mode<3?181+mode:0),"worker exits at exact calibrated fault stage");
    const auto control=Read(root/(std::wstring(caseNames[mode])+L".control"));Check(control.size()==sizeof(Evidence),"complete independent stage evidence");
    Evidence e{};std::memcpy(&e,control.data(),sizeof e);
    Check(e.magic==0x434d5231&&e.mode==mode&&e.stage==mode+1,"evidence identifies exact intended phase");
    const auto after=DiskFiles(path);
    if(mode<2){
        Preserved(old,after);checkpoint=OneAdded(old,after);
        Check(after.size()==5&&checkpoint==std::wstring(L"commits/")+e.name&&e.directory==1&&e.headers==1&&e.dispositions==0,"checkpoint is the only new file and never collects implicitly");
        if(mode==0){
            Check(after.at(checkpoint).size()==128&&e.payloadWrites==0&&e.flushes==0,"hard exit occurs after real checkpoint header, before payload");
            auto unchanged=baseline;Check(ReadRetry(store,unchanged)==catalog::Result::RecoveryRequired&&Same(unchanged,baseline),"partial checkpoint requires recovery and cannot silently return old revision");
            std::printf("CASE %ls exit=%lu partial-checkpoint=128 recovery-required\n",caseNames[mode],exit);return;
        }
        Check(e.payloadWrites>0&&e.flushes==1,"checkpoint payload really wrote and actual Flush succeeded before exit");
        AssertHead(store,baseline,3);
    }else{
        Check(e.headers==0&&e.payloadWrites==0&&e.flushes==0,"collection never rewrites active metadata");
        if(mode==2){Check(e.dispositions==1&&e.deleteSuccess==1&&e.deleteFaults==0&&before.size()==after.size()+1,"hard exit follows exactly one successful real disposition");}
        else {
            Check(e.result==DWORD(catalog::Result::Ok)&&e.attempted==3&&e.failed==3&&e.absent==0&&e.pending==0&&e.busy==0&&e.absentBytes==0,
                "parent confirms exact failed collection report");
            Check(e.dispositions==3&&e.deleteFaults==3&&e.deleteSuccess==0&&after==before,"all three actual failed removals preserve every original byte");
        }
        AssertHead(store,baseline,3);
    }
    const auto record=CheckpointRecord(path,checkpoint);
    Check(record.state.revision==3&&record.state.owner==baseline.StoreId()&&record.retired.size()==3,"checkpoint retires three old generations and keeps exact store identity");
    const auto retained=Relative(record.state.snapshot);
    Check(old.count(retained)==1&&old.at(retained).size()==128+Latest().size(),"checkpoint references existing latest snapshot without rewriting it");
    const auto expectedPayload=Latest();
    Check(std::equal(expectedPayload.begin(),expectedPayload.end(),old.at(retained).begin()+128),"retained snapshot contains independent authored latest bytes");
    Disk expectedRetired=old;expectedRetired.erase(retained);
    Check(expectedRetired.size()==3,"independent prior filesystem gives exact retired set");
    for(const auto& ref:record.retired){const auto rel=Relative(ref);Check(expectedRetired.count(rel)==1&&expectedRetired.at(rel).size()==ref.identity.fileBytes,"each retirement reference names one expected old file and actual byte count");expectedRetired.erase(rel);}
    Check(expectedRetired.empty(),"retirement list exhausts independent expected set without duplicates");
    Check(after.at(checkpoint)==Read(path/checkpoint)&&after.at(retained)==old.at(retained),"active checkpoint and active payload have exact original bytes");
    if(mode==2){
        const auto removed=std::wstring(e.directory==0?L"snapshots/":L"commits/")+e.name;
        Check(removed!=retained&&removed!=checkpoint&&old.count(removed)==1&&after.count(removed)==0,"exact hook-recorded retired file is absent after process handle cleanup");
        for(const auto& item:before)if(item.first!=removed){const auto it=after.find(item.first);Check(it!=after.end()&&it->second==item.second,"interrupted collection preserves every other file");}
    }
    std::uint64_t remainingBytes=0;unsigned remaining=0;
    for(const auto& ref:record.retired)if(after.count(Relative(ref))){++remaining;remainingBytes+=ref.identity.fileBytes;}
    catalog::CollectionReport report{};Check(store.CollectRetired(report)==catalog::Result::Ok,"real collection resumes after process/API failure");
    Check(report.attempted==remaining&&report.absent==remaining&&report.absentBytes==remainingBytes&&report.busy==0&&report.pending==0&&report.failed==0,
        "resumed collection counts only still-present files and confirmed namespace absence");
    const auto final=DiskFiles(path);Check(final.size()==2&&final.at(checkpoint)==after.at(checkpoint)&&final.at(retained)==old.at(retained),"resume removes only retired files and keeps active checkpoint/payload exact");
    AssertHead(store,baseline,3);
    catalog::CollectionReport again{};Check(store.CollectRetired(again)==catalog::Result::Ok&&again.attempted==0&&again.absent==0&&again.absentBytes==0&&again.busy==0&&again.pending==0&&again.failed==0,
        "repeat collection does not attempt or recount previously absent retirements");
    catalog::Snapshot reopened,next;Check(ReadRetry(store,reopened)==catalog::Result::Ok,"reopen collected checkpoint before new commit");
    Check(store.Commit(reopened,First(),next)==catalog::Result::Ok&&next.Revision()==4&&next.Bytes()==First(),"ordinary publication continues after recovered collection");
    catalog::Snapshot read;Check(ReadRetry(store,read)==catalog::Result::Ok&&Same(read,next),"new ordinary commit reads through collected checkpoint chain");
    std::printf("CASE %ls exit=%lu removed-now=%u absent-namespace-bytes=%llu\n",caseNames[mode],exit,report.absent,static_cast<unsigned long long>(report.absentBytes));
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try {
        if(argc==5&&std::wstring(argv[1])==L"--worker"){
            wchar_t* end=nullptr;const auto mode=wcstoul(argv[3],&end,10);Check(end&&!*end&&mode<4,"valid worker mode");
            const auto parent=wcstoul(argv[4],&end,10);Check(end&&!*end&&parent!=0,"valid worker parent PID");Worker(fs::path(argv[2]),unsigned(mode),DWORD(parent));return 0;
        }
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIRECTORY");const fs::path root(argv[2]);Absolute(root);
        Check(fs::is_directory(root.parent_path())&&CreateDirectoryW(root.c_str(),nullptr),"claim fresh isolated maintenance test root");
        const auto marker=Marker(GetCurrentProcessId());NewFile(root/markerName,marker.data(),DWORD(marker.size()));
        for(unsigned mode=0;mode<4;++mode)ParentCase(root,mode);
        std::printf("PASS cache catalog maintenance recovery checks=%u root=%ls; process/API faults, not power loss or disk-full capacity coverage\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog maintenance recovery checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog maintenance recovery unexpected exception\n");return 2;}
}
