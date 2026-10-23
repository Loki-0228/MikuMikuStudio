// MAINTENANCE: regression-keep. Ordinary C++ allocation failures, not OS allocation faults.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "mikumikustudio/cache_catalog.hpp"
#include "../src/io/cache_catalog_codec.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace allocation {
struct State {std::size_t failAt,attempts=0,hits=0;};
thread_local State* active=nullptr;
constexpr std::size_t CountOnly=(std::numeric_limits<std::size_t>::max)();
struct Scope {
    State state;
    explicit Scope(std::size_t failAt) noexcept:state{failAt}{active=&state;}
    ~Scope(){active=nullptr;}
};
}
void* operator new(std::size_t size){
    if(auto* state=allocation::active){const auto ordinal=state->attempts++;if(ordinal==state->failAt){++state->hits;throw std::bad_alloc();}}
    if(auto* p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

namespace {
namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
namespace catalog=mikumikustudio::cache_catalog;
namespace codec=mikumikustudio::cache_catalog::codec;
using Bytes=std::vector<std::uint8_t>;
using Result=catalog::Result;
unsigned checks=0,recoveryRequired=0,indeterminate=0,partialCollections=0;
constexpr std::size_t MaximumOrdinals=1024;
const Bytes OldBytes{0x00,0xff,0x80,0x61,0x62,0x63};
const Bytes NewBytes{0x99,0x01,0xfe,0x00,0x55,0xaa,0x7f};
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
std::wstring Number(std::size_t n){wchar_t text[32]{};std::swprintf(text,32,L"%06llu",static_cast<unsigned long long>(n));return text;}
void NewDirectory(const fs::path& path){
    Check(path.is_absolute()&&path.has_filename()&&path!=path.root_path()&&path==path.lexically_normal(),"new test root must be a normalized absolute non-root path");
    Check(fs::is_directory(path.parent_path()),"new fixture parent must already exist");
    Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"claim exclusively new test directory");
}
void NewText(const fs::path& path,const std::string& text){
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"create only new owned evidence");DWORD done=0;
    Check(text.size()<=MAXDWORD&&WriteFile(h.value,text.data(),static_cast<DWORD>(text.size()),&done,nullptr)&&done==text.size(),"write owned evidence outside injection scope");
}
Bytes ReadFile(const fs::path& path){std::ifstream f(path,std::ios::binary);Check(bool(f),"read owned file bytes");Bytes b(std::istreambuf_iterator<char>(f),{});Check(!f.bad(),"complete owned file read");return b;}
using Disk=std::vector<std::pair<fs::path,Bytes>>;
Disk DiskState(const fs::path& path){
    Disk out;if(fs::exists(path))for(const auto& entry:fs::recursive_directory_iterator(path)){
        Check(!entry.is_symlink(),"fixture contains no unexpected symlink");
        if(entry.is_regular_file())out.emplace_back(fs::relative(entry.path(),path),ReadFile(entry.path()));}
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first<b.first;});return out;
}
void ContainsOld(const Disk& before,const Disk& after){for(const auto& file:before){auto at=std::find_if(after.begin(),after.end(),[&](const auto& x){return x.first==file.first;});Check(at!=after.end()&&at->second==file.second,"every preexisting metadata file retains exact bytes");}}
std::size_t BlobCount(const Disk& disk,const wchar_t* folder){std::size_t count=0;for(const auto& entry:disk)if(entry.first.parent_path()==folder&&entry.first.extension()==L".blob")++count;return count;}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
catalog::Snapshot Initialize(const fs::path& path,const Bytes& bytes){catalog::Snapshot s;Check(catalog::Store(path).Initialize(bytes,s)==Result::Ok&&s.IsValid()&&s.Revision()==1&&s.Bytes()==bytes,"prepare real initialized store outside injection");return s;}
void LockReleased(const fs::path& path){
    if(!fs::exists(path))return;
    files::Directory d;Check(files::Directory::Open(path,false,d)==files::Result::Ok,"open actual metadata directory after injected call");
    files::FileLock lock;Check(d.Lock(0,lock)==files::Result::Ok&&lock.IsValid(),"failed operation released actual metadata byte lock");
}
// A distinct healthy origin makes a failed output's private root/head usable,
// even when the attempted target write left indeterminate/recovery metadata.
class OutputOrigin {
    fs::path base_,path_;
    catalog::Snapshot current_;
    unsigned generation_=0;
public:
    explicit OutputOrigin(fs::path base):base_(std::move(base)){NewDirectory(base_);}
    catalog::Snapshot Prepare(){
        if(!current_.IsValid()||current_.Revision()>=32){path_=base_/(L"origin-"+Number(generation_++));current_=Initialize(path_,Bytes{0x3c,0x71});}
        return current_;
    }
    void Prove(const catalog::Snapshot& failedOutput,const catalog::Snapshot& before){
        Check(Same(failedOutput,before),"failure preserves all public Snapshot fields");catalog::Snapshot next;
        Check(catalog::Store(path_).Commit(failedOutput,failedOutput.Bytes(),next)==Result::Ok&&next.Revision()==before.Revision()+1&&next.StoreId()==before.StoreId()&&next.Bytes()==before.Bytes(),"actual failed output retains private origin and full CAS identity");
        current_=std::move(next);
    }
};
enum class Operation {Checkpoint,Collect,Read};
const wchar_t* Label(Operation op){return op==Operation::Checkpoint?L"checkpoint":op==Operation::Collect?L"collect":L"read-checkpoint";}
struct Call {Result result=Result::Io;std::size_t attempts=0,hits=0;};
catalog::CollectionReport SentinelReport(){return {21,22,23,24,25,0x123456789ull};}
bool SameReport(const catalog::CollectionReport& a,const catalog::CollectionReport& b){return a.attempted==b.attempted&&a.absent==b.absent&&a.busy==b.busy&&a.pending==b.pending&&a.failed==b.failed&&a.absentBytes==b.absentBytes;}
void ReportPartition(const catalog::CollectionReport& r){Check(std::uint64_t(r.absent)+r.busy+r.pending+r.failed==r.attempted,"report accounts for every attempted retiree exactly once");}
std::uint64_t Get(const Bytes& b,std::size_t at,unsigned width){std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t(b.at(at+i))<<(8*i);return value;}
files::Owner NamespaceOwner(){files::Owner owner{};const char text[]="MMSCatalogHead01";std::copy_n(text,16,owner.begin());return owner;}
struct ActiveFiles {fs::path head,snapshot;Bytes headBytes,snapshotBytes;};
ActiveFiles Active(const fs::path& path){
    files::Directory d;Check(files::Directory::Open(path/L"commits",false,d)==files::Result::Ok,"open active metadata inspection");
    std::vector<std::wstring> names;Check(d.Enumerate(100,names)==files::Result::Ok,"enumerate actual heads");
    ActiveFiles active;std::uint64_t revision=0;
    for(const auto& name:names){files::ReadLease lease;Check(d.Inspect(name,NamespaceOwner(),32768,lease)==files::Result::Ok,"inspect actual head envelope");Bytes bytes;Check(lease.ReadAll(bytes)==files::Result::Ok&&bytes.size()>=368,"read active record bytes");
        if(Get(bytes,24,8)>revision){revision=Get(bytes,24,8);active.head=path/L"commits"/name;std::wstring snapshot;for(unsigned i=0;i<37;++i)snapshot+=static_cast<wchar_t>(bytes.at(48+i));active.snapshot=path/L"snapshots"/snapshot;}}
    Check(revision!=0,"active head exists");active.headBytes=ReadFile(active.head);active.snapshotBytes=ReadFile(active.snapshot);return active;
}
void ActiveUnchanged(const ActiveFiles& before){Check(ReadFile(before.head)==before.headBytes&&ReadFile(before.snapshot)==before.snapshotBytes,"active checkpoint and snapshot retain exact bytes during collection");}
void ReadSame(const fs::path& path,const catalog::Snapshot& before){catalog::Snapshot now;Check(catalog::Store(path).Read(now)==Result::Ok&&Same(now,before),"fresh actual Read retains complete published head");}
void CompleteCollection(const fs::path& path,const catalog::Snapshot& current){
    catalog::CollectionReport report=SentinelReport();Check(catalog::Store(path).CollectRetired(report)==Result::Ok,"uninjected collection completes remaining authorized work");ReportPartition(report);
    Check(report.busy==0&&report.failed==0&&report.pending==0,"no internal lease or failed handle remains after injection");
    const auto disk=DiskState(path);Check(BlobCount(disk,L"commits")==1&&BlobCount(disk,L"snapshots")==1,"all retired files actually absent after collection retry");ReadSame(path,current);
}
struct Fixture {
    fs::path casePath,path;
    catalog::Snapshot current;
    Fixture(const fs::path& group,std::size_t ordinal,Operation op):casePath(group/Number(ordinal)),path(casePath/L"metadata"){
        NewDirectory(casePath);current=Initialize(path,OldBytes);catalog::Store store(path);
        Check(store.Commit(current,NewBytes,current)==Result::Ok&&store.Commit(current,OldBytes,current)==Result::Ok&&current.Revision()==3,"prepare real three-revision history");
        if(op!=Operation::Checkpoint){Check(store.Checkpoint(current,current)==Result::Ok&&current.Revision()==4,"publish real checkpoint outside injected call");
            if(op==Operation::Read){CompleteCollection(path,current);Check(store.Commit(current,NewBytes,current)==Result::Ok&&current.Revision()==5,"Read fixture has collected checkpoint plus active ordinary tail");}}
    }
};
Call Invoke(Operation op,const catalog::Store& store,const catalog::Snapshot& expected,catalog::Snapshot& output,catalog::CollectionReport& report,std::size_t ordinal){
    Call call;{allocation::Scope scope(ordinal);
        switch(op){case Operation::Checkpoint:call.result=store.Checkpoint(expected,output);break;case Operation::Collect:call.result=store.CollectRetired(report);break;case Operation::Read:call.result=store.Read(output);break;}
        call.attempts=scope.state.attempts;call.hits=scope.state.hits;}return call;
}
void Log(const fs::path& casePath,const Call& call,std::size_t ordinal,Result reread,const catalog::CollectionReport& report){
    NewText(casePath/L"result.txt","ordinal="+std::to_string(ordinal)+"\nattempts="+std::to_string(call.attempts)+"\nhits="+std::to_string(call.hits)+"\nresult="+std::to_string(static_cast<int>(call.result))+"\nreread="+std::to_string(static_cast<int>(reread))+"\nreport.attempted="+std::to_string(report.attempted)+"\nreport.absent="+std::to_string(report.absent)+"\nreport.busy="+std::to_string(report.busy)+"\nreport.pending="+std::to_string(report.pending)+"\nreport.failed="+std::to_string(report.failed)+"\nreport.absentBytes="+std::to_string(report.absentBytes)+"\n");
}
Result VerifyCheckpoint(const Fixture& fixture,const Disk& before,const Call& call,bool injected){
    const auto after=DiskState(fixture.path);ContainsOld(before,after);
    std::vector<fs::path> beforePayloads,afterPayloads;for(const auto& f:before)if(f.first.parent_path()==L"snapshots")beforePayloads.push_back(f.first);for(const auto& f:after)if(f.first.parent_path()==L"snapshots")afterPayloads.push_back(f.first);
    Check(beforePayloads==afterPayloads,"Checkpoint never creates or removes a payload even when allocation fails");
    catalog::Snapshot observed=fixture.current;const auto result=catalog::Store(fixture.path).Read(observed);LockReleased(fixture.path);
    if(result==Result::RecoveryRequired){Check(injected&&Same(observed,fixture.current)&&BlobCount(after,L"commits")>BlobCount(before,L"commits"),"partial checkpoint explicitly requires recovery and retains read output");++recoveryRequired;
        auto retry=fixture.current;Check(catalog::Store(fixture.path).Checkpoint(retry,retry)==Result::RecoveryRequired&&Same(retry,fixture.current),"partial checkpoint is not silently overwritten");
        Check(DiskState(fixture.path)==after,"recovery-required checkpoint evidence is retained without repair");return result;}
    Check(result==Result::Ok&&observed.StoreId()==fixture.current.StoreId()&&observed.Bytes()==fixture.current.Bytes(),"checkpoint reread is complete old/new same-value head");
    if(observed.Revision()==fixture.current.Revision())Check(injected&&Same(observed,fixture.current),"failed checkpoint may leave exactly old head");
    else Check(observed.Revision()==fixture.current.Revision()+1&&(!injected||call.result==Result::Indeterminate),"new checkpoint after caller failure requires Indeterminate");
    return result;
}
void VerifyCollection(const Fixture& fixture,const Disk& before,const ActiveFiles& active,const Call& call,const catalog::CollectionReport& report,bool injected){
    const auto after=DiskState(fixture.path);
    if(call.result==Result::Io){Check(injected&&SameReport(report,SentinelReport())&&before==after,"setup allocation failure retains exact report and performs no deletion");}
    else{
        Check(call.result==Result::Ok,"after collection begins, partial operation returns its report");ReportPartition(report);
        Check(report.busy==0&&report.attempted<=5,"no external leases and at most five authorized present retirees");
        std::uint64_t disappearedBytes=0;unsigned disappeared=0;
        for(const auto& old:before){auto at=std::find_if(after.begin(),after.end(),[&](const auto& x){return x.first==old.first;});
            if(at==after.end()){++disappeared;disappearedBytes+=old.second.size();}
            else Check(at->second==old.second,"collection never rewrites a retained file");}
        for(const auto& now:after)Check(std::any_of(before.begin(),before.end(),[&](const auto& x){return x.first==now.first;}),"collection creates no new metadata file");
        Check(report.absent<=disappeared&&report.absentBytes<=disappearedBytes,"reported confirmed absence cannot exceed actual namespace disappearance");
        Check(disappeared<=report.absent+report.pending,"only confirmed or pending deletion can explain missing files");
        if(report.pending==0)Check(report.absent==disappeared&&report.absentBytes==disappearedBytes,"without pending confirmation report matches exact vanished names and bytes");
        if(injected&&(report.failed||report.pending||report.busy))++partialCollections;
    }
    ActiveUnchanged(active);ReadSame(fixture.path,fixture.current);LockReleased(fixture.path);CompleteCollection(fixture.path,fixture.current);ActiveUnchanged(active);
}
std::size_t Scan(Operation op,const fs::path& root,OutputOrigin& origins){
    const auto group=root/Label(op);NewDirectory(group);std::size_t count=0;
    {Fixture fixture(group,0,op);catalog::Store store(fixture.path);auto output=origins.Prepare();auto report=SentinelReport();const auto disk=DiskState(fixture.path);const auto active=Active(fixture.path);
        const auto call=Invoke(op,store,fixture.current,output,report,allocation::CountOnly);Check(call.result==Result::Ok&&call.hits==0,"uninjected maintenance baseline succeeds");count=call.attempts;
        Check(count>0&&count<MaximumOrdinals,"successful maintenance allocation count is bounded below 1024");
        if(op==Operation::Collect)VerifyCollection(fixture,disk,active,call,report,false);
        else if(op==Operation::Checkpoint){Check(output.IsValid()&&output.Revision()==4&&output.Bytes()==fixture.current.Bytes()&&output.StoreId()==fixture.current.StoreId(),"baseline checkpoint output is exact");VerifyCheckpoint(fixture,disk,call,false);}
        else Check(Same(output,fixture.current)&&DiskState(fixture.path)==disk,"baseline Read returns exact checkpoint-tail state without disk writes");
        Log(fixture.casePath,call,allocation::CountOnly,Result::Ok,report);}
    std::printf("maintenance allocation baseline %ls=%llu\n",Label(op),static_cast<unsigned long long>(count));
    for(std::size_t ordinal=0;ordinal<count;++ordinal){Fixture fixture(group,ordinal+1,op);catalog::Store store(fixture.path);auto output=origins.Prepare();const auto beforeOutput=output;auto report=SentinelReport();const auto disk=DiskState(fixture.path);const auto active=Active(fixture.path);
        const auto call=Invoke(op,store,fixture.current,output,report,ordinal);Check(call.hits==1,"each calibrated ordinary C++ allocation fault is actually hit once");Result reread=Result::Ok;
        if(op==Operation::Collect)VerifyCollection(fixture,disk,active,call,report,true);
        else{Check(call.result==Result::Io||(op==Operation::Checkpoint&&call.result==Result::Indeterminate),"allocation failure returns Io or checkpoint post-attempt Indeterminate");origins.Prove(output,beforeOutput);
            if(call.result==Result::Indeterminate)++indeterminate;
            if(op==Operation::Checkpoint)reread=VerifyCheckpoint(fixture,disk,call,true);
            else{Check(DiskState(fixture.path)==disk,"failed Read with a checkpoint and tail never changes disk");ReadSame(fixture.path,fixture.current);LockReleased(fixture.path);}}
        Log(fixture.casePath,call,ordinal,reread,report);
    }
    return count;
}
void AliasChecks(const fs::path& root){
    const auto group=root/L"alias";NewDirectory(group);std::size_t checkpointCount=0;
    // Calibrate the actual alias call under equal-length fixture paths. Path
    // storage allocations may differ from the non-alias scan's longer group.
    {Fixture fixture(group,0,Operation::Checkpoint);catalog::Store store(fixture.path);auto output=fixture.current;auto report=SentinelReport();
        const auto call=Invoke(Operation::Checkpoint,store,output,output,report,allocation::CountOnly);checkpointCount=call.attempts;
        Log(fixture.casePath,call,allocation::CountOnly,Result::Ok,report);
        Check(call.result==Result::Ok&&call.hits==0&&checkpointCount>0&&checkpointCount<MaximumOrdinals&&output.Revision()==4&&output.Bytes()==fixture.current.Bytes()&&output.StoreId()==fixture.current.StoreId(),"independent alias allocation baseline succeeds");}
    std::printf("maintenance allocation baseline alias=%llu\n",static_cast<unsigned long long>(checkpointCount));
    std::vector<std::size_t> ordinals{0,checkpointCount/2,checkpointCount-1};std::sort(ordinals.begin(),ordinals.end());ordinals.erase(std::unique(ordinals.begin(),ordinals.end()),ordinals.end());
    for(std::size_t i=0;i<ordinals.size();++i){Fixture fixture(group,i+1,Operation::Checkpoint);catalog::Store store(fixture.path);auto output=fixture.current;auto report=SentinelReport();const auto disk=DiskState(fixture.path);
        const auto call=Invoke(Operation::Checkpoint,store,output,output,report,ordinals[i]);
        NewText(fixture.casePath/L"call.txt","hits="+std::to_string(call.hits)+"\nattempts="+std::to_string(call.attempts)+"\nresult="+std::to_string(static_cast<int>(call.result))+"\n");
        Check(call.hits==1&&(call.result==Result::Io||call.result==Result::Indeterminate)&&Same(output,fixture.current),"failed checkpoint alias retains prior public snapshot");
        const auto result=VerifyCheckpoint(fixture,disk,call,true);if(result==Result::Ok){catalog::Snapshot current;Check(store.Read(current)==Result::Ok,"resolve alias call through real fresh Read");catalog::Snapshot proof;
            if(current.Revision()==fixture.current.Revision())Check(store.Checkpoint(output,proof)==Result::Ok&&proof.Revision()==current.Revision()+1&&proof.Bytes()==current.Bytes(),"failed alias retains usable full current CAS token");
            else{const auto after=DiskState(fixture.path);Check(store.Checkpoint(output,proof)==Result::Conflict&&!proof.IsValid()&&DiskState(fixture.path)==after,"published checkpoint leaves failed alias stale, never blindly replays");}}
        Log(fixture.casePath,call,ordinals[i],result,report);}
}
void Calibration(){using Allocate=void*(*)(std::size_t);Allocate volatile allocate=static_cast<Allocate>(&::operator new);bool caught=false;std::size_t hits=0;
    {allocation::Scope scope(0);try{void* p=allocate(61);::operator delete(p);}catch(const std::bad_alloc&){caught=true;}void* after=allocate(67);::operator delete(after);hits=scope.state.hits;}
    Check(caught&&hits==1,"calibrated current-thread single ordinal fault allows cleanup allocations");}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage: --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);NewDirectory(root);Calibration();OutputOrigin origins(root/L"output-origins");
        const auto checkpoint=Scan(Operation::Checkpoint,root,origins);const auto collect=Scan(Operation::Collect,root,origins);const auto read=Scan(Operation::Read,root,origins);AliasChecks(root);
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\ncheckpointOrdinals="+std::to_string(checkpoint)+"\ncollectOrdinals="+std::to_string(collect)+"\nreadOrdinals="+std::to_string(read)+"\npartialCollections="+std::to_string(partialCollections)+"\nindeterminate="+std::to_string(indeterminate)+"\nrecoveryRequired="+std::to_string(recoveryRequired)+"\nOnly ordinary current-thread new/new[] single faults; no aligned/CRT/Win32/CNG allocation coverage, no power-loss or physical free-space claim. Partial checkpoint evidence is retained without repair.\n");
        std::printf("PASS cache maintenance allocation checks=%u partialCollections=%u recoveryRequired=%u evidence=%ls\n",checks,partialCollections,recoveryRequired,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache maintenance allocation checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache maintenance allocation unexpected exception\n");return 2;}
}
