// MAINTENANCE: regression-keep. Real durable recovery endpoints and payload pins.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "mikumikustudio/cache_catalog.hpp"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace c=mikumikustudio::cache_catalog;
namespace f=mikumikustudio::cache_files;
using R=c::Result;using Action=c::RecoveryAction;using Bytes=std::vector<std::uint8_t>;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"DIAG check=%u win32=%lu\n",checks,GetLastError());throw std::runtime_error(why);}}
struct Handle{HANDLE value=INVALID_HANDLE_VALUE;explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;};
void Fresh(const fs::path& p){Check(p.is_absolute()&&p==p.lexically_normal()&&p!=p.root_path(),"absolute normalized owned fixture path");Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim fresh fixture directory");}
f::Owner Owner(){f::Owner id{};id[0]=51;return id;}
using Evidence=std::map<std::wstring,Bytes>;
Evidence Files(const fs::path& root){Evidence out;for(const auto& item:fs::recursive_directory_iterator(root))if(item.is_regular_file()){if(item.path().filename()==L".lock"){Check(item.file_size()==0,"catalog lock remains empty");continue;}std::ifstream in(item.path(),std::ios::binary);Check(bool(in),"read actual owned metadata");out.emplace(item.path().lexically_relative(root).native(),Bytes(std::istreambuf_iterator<char>(in),{}));}return out;}
bool Same(const c::RecoveryReport&a,const c::RecoveryReport&b){return a.confirmedPublication==b.confirmedPublication&&a.rolledBack==b.rolledBack&&a.revision==b.revision;}
std::wstring Prefix(const fs::path& p){Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));wchar_t path[32768]{};const auto n=h.value==INVALID_HANDLE_VALUE?0:GetFinalPathNameByHandleW(h.value,path,32768,VOLUME_NAME_GUID);Check(n&&n<32768,"bind hook to exact fixture physical directory");return std::wstring(path,n)+L'\\';}
enum class Fault{None,CommitFlush,CommitPartial};
struct Observation{std::wstring prefix;Fault fault=Fault::None;unsigned writes=0,flushes=0,creates=0,hits=0;
    explicit Observation(const fs::path& root,Fault failure=Fault::None):prefix(Prefix(root)),fault(failure){}
    int Match(HANDLE h)const noexcept{wchar_t path[32768]{};const auto n=GetFinalPathNameByHandleW(h,path,32768,VOLUME_NAME_GUID);if(n<=prefix.size()||n>=32768||_wcsnicmp(path,prefix.c_str(),prefix.size()))return 0;return !std::wcsncmp(path+prefix.size(),L"commits\\",8)?2:1;}
};
thread_local Observation* active=nullptr;
decltype(&WriteFile) realWrite=nullptr;decltype(&FlushFileBuffers) realFlush=nullptr;decltype(&CreateFileW) realCreate=nullptr;
BOOL WINAPI WriteHook(HANDLE h,LPCVOID bytes,DWORD count,LPDWORD done,LPOVERLAPPED overlap)noexcept{auto* p=active;const auto kind=p?p->Match(h):0;if(kind)++p->writes;LARGE_INTEGER zero{},position{};
    if(kind==2&&p->fault==Fault::CommitPartial&&!p->hits&&!overlap&&count>1&&SetFilePointerEx(h,zero,&position,FILE_CURRENT)&&position.QuadPart>=128){const auto ok=realWrite(h,bytes,(std::min)(DWORD(17),count-1),done,overlap);if(ok){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}return ok;}return realWrite(h,bytes,count,done,overlap);}
BOOL WINAPI FlushHook(HANDLE h)noexcept{const auto result=realFlush(h);const auto error=GetLastError();auto* p=active;const auto kind=p?p->Match(h):0;if(kind)++p->flushes;if(result&&kind==2&&p->fault==Fault::CommitFlush&&!p->hits){++p->hits;SetLastError(ERROR_WRITE_FAULT);return FALSE;}SetLastError(error);return result;}
HANDLE WINAPI CreateHook(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE original)noexcept{const auto result=realCreate(path,access,share,security,disposition,flags,original);const auto error=GetLastError();if(active&&result!=INVALID_HANDLE_VALUE&&disposition==CREATE_NEW&&active->Match(result))++active->creates;SetLastError(error);return result;}
struct Hooks{Hooks(){Check(MH_Initialize()==MH_OK,"initialize exact-file hooks");Check(MH_CreateHook(reinterpret_cast<void*>(&WriteFile),reinterpret_cast<void*>(&WriteHook),reinterpret_cast<void**>(&realWrite))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&FlushFileBuffers),reinterpret_cast<void*>(&FlushHook),reinterpret_cast<void**>(&realFlush))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(&CreateFileW),reinterpret_cast<void*>(&CreateHook),reinterpret_cast<void**>(&realCreate))==MH_OK&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK,"install actual file observation and bounded faults");}~Hooks(){active=nullptr;MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();}};
struct Arm{explicit Arm(Observation& p){Check(!active,"no nested fault scope");active=&p;}~Arm(){active=nullptr;}};
struct Fixture{fs::path root;c::Store store;Fixture(const fs::path& parent,const wchar_t* name):root(parent/name),store(root){Fresh(root);Fresh(root/L"snapshots");Fresh(root/L"commits");}c::Snapshot Init(){c::Snapshot out;Check(store.InitializeOwned(Owner(),{1,3,5},out)==R::Ok,"initialize actual prior catalog");return out;}};
R Plan(Fixture& f,c::WritePlan& out){const auto before=Files(f.root);Observation observed(f.root);R result;{Arm arm(observed);result=f.store.PlanRecovery(out);}Check(!observed.creates&&!observed.writes&&!observed.flushes&&Files(f.root)==before,"recovery planning creates no candidates and does not write, flush or remove metadata");return result;}
fs::path Payload(Fixture& fixture,const Bytes& bytes){f::Directory directory;Check(f::Directory::Open(fixture.root/L"snapshots",false,directory)==f::Result::Ok,"open actual snapshot directory");std::vector<std::wstring> names;Check(directory.Enumerate(128,names)==f::Result::Ok,"enumerate only owned snapshot generation names");fs::path result;
    for(const auto& name:names){f::ReadLease lease;Check(directory.Inspect(name,Owner(),c::MaximumSnapshotBytes,lease)==f::Result::Ok,"verify real payload envelope and digest");Bytes actual;Check(lease.ReadAll(actual)==f::Result::Ok,"read real payload bytes");if(actual==bytes){Check(result.empty(),"target payload uniquely identifies fixture snapshot");result=fixture.root/L"snapshots"/name;}}
    Check(!result.empty(),"find exact target payload generation");return result;
}
void Pinned(const fs::path& path){Handle attempted(CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));const auto error=GetLastError();Check(attempted.value==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION,"recovery plan holds actual target payload against deletion");}
void Unpinned(const fs::path& path){Handle attempted(CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));Check(attempted.value!=INVALID_HANDLE_VALUE,"destroying last plan releases payload pin without deleting the file");}
void Confirmed(Fixture& fixture,c::WritePlan& plan,const c::Snapshot& target,Action action){c::RecoveryReport report{true,true,999};Check(fixture.store.ExecuteRecovery(plan,report)==R::Ok&&plan.IsConsumed(),"execute plan without self-conflict from its retained target lease");c::Snapshot actual;Check(fixture.store.Read(actual)==R::Ok&&actual.SameVersion(target)&&actual.Bytes()==target.Bytes()&&report.revision==target.Revision(),"actual recovered head and payload equal exact planned endpoint");Check(report.confirmedPublication==(action==Action::ConfirmPublication)&&report.rolledBack==(action==Action::Rollback),"actual recovery report agrees with planned action");}

void NoopAndPreflight(const fs::path& root){Fixture fixture(root,L"noop"),missing(root,L"empty");const auto head=fixture.Init();c::WritePlan empty;Check(!empty.IsValid()&&empty.Recovery()==Action::None&&!empty.RecoveryTarget().IsValid(),"empty plan exposes no recovery authority");
    c::WritePlan publication;Check(fixture.store.PlanCommit(head,{9},publication)==R::Ok&&publication.Recovery()==Action::None&&!publication.RecoveryTarget().IsValid(),"ordinary publication plan never claims recovery target");publication={};
    c::WritePlan plan;Check(Plan(fixture,plan)==R::Ok&&plan.Recovery()==Action::Noop&&plan.RecoveryTarget().SameVersion(head)&&plan.Footprint().count==0,"clean plan exposes exact current head and zero write footprint");const auto path=Payload(fixture,head.Bytes());Pinned(path);
    {f::Directory directory;f::DirectoryIdentity identity;Check(f::Directory::Open(fixture.root,false,directory)==f::Result::Ok&&directory.GetIdentity(identity)==f::Result::Ok&&plan.RootIdentity().volume==identity.volume&&plan.RootIdentity().fileIndex==identity.fileIndex,"recovery facts expose actual bound physical root identity");}
    Check(Plan(missing,plan)==R::Missing&&plan.IsValid()&&plan.Recovery()==Action::Noop&&plan.RecoveryTarget().SameVersion(head)&&!plan.IsConsumed(),"failed planning preserves an existing valid recovery plan and endpoint");
    c::WritePlan moved(std::move(plan));Check(!plan.IsValid()&&plan.Recovery()==Action::None&&!plan.RecoveryTarget().IsValid()&&moved.RecoveryTarget().SameVersion(head),"move construction transfers recovery identity and invalidates source");Pinned(path);plan=std::move(moved);Check(!moved.IsValid()&&moved.Recovery()==Action::None&&plan.RecoveryTarget().SameVersion(head),"move assignment transfers retained recovery endpoint");
    const auto alien=root/L"wrong-empty-root";Fresh(alien);c::Store wrong(alien);c::RecoveryReport report{true,false,123},saved=report;
    Check(wrong.ExecuteRecovery(plan,report)==R::Conflict&&Same(report,saved)&&!plan.IsConsumed()&&!plan.MayHaveWritten()&&fs::is_empty(alien),"wrong-root execution neither consumes plan nor creates even a lock");Pinned(path);
    Confirmed(fixture,plan,head,Action::Noop);Check(!plan.MayHaveWritten(),"no-op execution reports no mutation attempt");plan={};Unpinned(path);
    Check(Plan(fixture,plan)==R::Ok,"prepare independent stale head test");c::Snapshot newer;Check(fixture.store.Commit(head,{7,8},newer)==R::Ok,"actual other writer advances catalog");const auto files=Files(fixture.root);report=saved;
    Check(fixture.store.ExecuteRecovery(plan,report)==R::Conflict&&Same(report,saved)&&!plan.IsConsumed()&&!plan.MayHaveWritten()&&Files(fixture.root)==files&&plan.RecoveryTarget().SameVersion(head),"stale no-op cannot silently adopt a newer head or alter files/output");
}
void Interrupted(const fs::path& root){for(unsigned mode=0;mode<3;++mode){Fixture fixture(root,(L"interrupted-"+std::to_wstring(mode)).c_str());const auto head=fixture.Init();const Bytes candidate(8197,std::uint8_t(0x31+mode));auto output=head;Observation fault(fixture.root,mode==0?Fault::CommitFlush:Fault::CommitPartial);R result;
    {Arm arm(fault);result=fixture.store.Commit(head,candidate,output);}Check(fault.hits==1&&result==R::Indeterminate&&output.SameVersion(head),"real interrupted commit preserves prior caller output");
    c::WritePlan plan;Check(Plan(fixture,plan)==R::Ok,"plan recovery from actual retained intent");const auto action=mode==0?Action::ConfirmPublication:Action::Rollback;const auto target=plan.RecoveryTarget();Check(plan.Recovery()==action&&target.IsValid()&&target.Bytes()==(mode==0?candidate:head.Bytes())&&target.Revision()==head.Revision()+(mode==0?1:0),"plan distinguishes full publication from partial candidate and exposes correct bytes/revision");if(mode)Check(target.SameVersion(head),"rollback endpoint is the exact preexisting head");const auto path=Payload(fixture,target.Bytes());Pinned(path);
    if(mode==2){c::RecoveryReport external;Check(fixture.store.RecoverOwned(external)==R::Ok&&external.rolledBack,"another real recovery resolves exact pending rollback");const auto files=Files(fixture.root);c::RecoveryReport report{true,true,777},saved=report;
        Check(fixture.store.ExecuteRecovery(plan,report)==R::Conflict&&Same(report,saved)&&!plan.IsConsumed()&&!plan.MayHaveWritten()&&Files(fixture.root)==files,"changed intent/decision state invalidates old recovery plan without mutation");continue;}
    if(mode==0){f::Directory directory;f::ReadLease external;Check(f::Directory::Open(fixture.root/L"snapshots",false,directory)==f::Result::Ok&&directory.Inspect(path.filename().native(),Owner(),c::MaximumSnapshotBytes,external)==f::Result::Ok,"hold a separate actual payload reader beside confirming plan");const auto files=Files(fixture.root);c::RecoveryReport report{true,true,888},saved=report;
        Check(fixture.store.ExecuteRecovery(plan,report)==R::Busy&&plan.IsConsumed()&&!plan.MayHaveWritten()&&Same(report,saved)&&Files(fixture.root)==files,"confirm transfers own pin before exclusive reopen; external reader yields consumed Busy without mutation or false output");
        Check(fixture.store.ExecuteRecovery(plan,report)!=R::Ok&&Same(report,saved)&&Files(fixture.root)==files,"consumed confirm plan cannot blindly retry after pin transfer");external={};
        Check(Plan(fixture,plan)==R::Ok&&plan.Recovery()==Action::ConfirmPublication&&plan.RecoveryTarget().SameVersion(target)&&!plan.IsConsumed(),"fresh explicit plan reacquires same authoritative target after reader release");Pinned(path);
    }
    Confirmed(fixture,plan,target,action);plan={};Unpinned(path);
}}
void InitializationRollback(const fs::path& root){Fixture sentinel(root,L"sentinel"),fixture(root,L"initial-rollback");const auto original=sentinel.Init();auto output=original;Observation fault(fixture.root,Fault::CommitPartial);R result;
    {Arm arm(fault);result=fixture.store.InitializeOwned(Owner(),Bytes(4099,0x84),output);}Check(fault.hits==1&&result==R::Indeterminate&&output.SameVersion(original),"interrupted initial publication preserves foreign caller output");c::WritePlan plan;
    Check(Plan(fixture,plan)==R::Ok&&plan.Recovery()==Action::Rollback&&!plan.RecoveryTarget().IsValid(),"initial rollback explicitly targets absence rather than fabricating a snapshot");c::RecoveryReport report{true,true,456};Check(fixture.store.ExecuteRecovery(plan,report)==R::Ok&&plan.IsConsumed()&&report.rolledBack&&!report.confirmedPublication&&report.revision==0,"initial rollback executes and reports absent catalog");Check(fixture.store.Read(output)==R::Missing&&output.SameVersion(original),"read after initial rollback preserves caller output on missing catalog");}
} // namespace
int wmain(int argc,wchar_t**argv){try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage: cache_catalog_recovery_facts_tests --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);Fresh(root);Hooks hooks;NoopAndPreflight(root);Interrupted(root);InitializationRollback(root);std::printf("PASS cache catalog recovery facts checks=%u\n",checks);return 0;}catch(const std::exception& e){active=nullptr;std::fprintf(stderr,"FAIL cache catalog recovery facts checks=%u: %s\n",checks,e.what());return 1;}}
