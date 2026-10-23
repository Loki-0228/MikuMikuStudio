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
unsigned checks=0,recoveryRequired=0,indeterminate=0,orphanCases=0;
constexpr std::size_t MaximumOrdinals=768;
const Bytes OldBytes{0x00,0xff,0x80,0x61,0x62,0x63};
const Bytes NewBytes{0x99,0x01,0xfe,0x00,0x55,0xaa,0x7f};
constexpr files::Owner RequestedOwner{0x31,0x07,0xd4,0x5a,0x02,0x86,0x9f,0xe1,
    0x48,0xbc,0x63,0x10,0x7d,0xa2,0x05,0xf6};
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
enum class Operation {Read,Initialize,InitializeOwned,Commit};
bool Initializing(Operation op){return op==Operation::Initialize||op==Operation::InitializeOwned;}
const wchar_t* Label(Operation op){return op==Operation::Read?L"read":op==Operation::Initialize?L"initialize":op==Operation::InitializeOwned?L"initialize-owned":L"commit";}
struct Call {Result result=Result::Io;std::size_t attempts=0,hits=0;};
Call Invoke(Operation op,const catalog::Store& store,const catalog::Snapshot& expected,catalog::Snapshot& output,std::size_t failAt){
    Call call;
    {allocation::Scope scope(failAt);
        switch(op){case Operation::Read:call.result=store.Read(output);break;
            case Operation::Initialize:call.result=store.Initialize(NewBytes,output);break;
            case Operation::InitializeOwned:call.result=store.InitializeOwned(RequestedOwner,NewBytes,output);break;
            case Operation::Commit:call.result=store.Commit(expected,NewBytes,output);break;}
        call.attempts=scope.state.attempts;call.hits=scope.state.hits;}
    return call;
}
void Record(const fs::path& casePath,const Call& call,Result reread,std::size_t ordinal,std::size_t snapshots,std::size_t commits){
    NewText(casePath/L"result.txt","ordinal="+std::to_string(ordinal)+"\nattempts="+std::to_string(call.attempts)+"\nhits="+std::to_string(call.hits)+"\nresult="+std::to_string(static_cast<int>(call.result))+"\nreread="+std::to_string(static_cast<int>(reread))+"\nsnapshots="+std::to_string(snapshots)+"\ncommits="+std::to_string(commits)+"\n");
}
Result VerifyAfter(Operation op,const fs::path& path,const catalog::Snapshot& old,const Call& call,bool injected){
    catalog::Store store(path);catalog::Snapshot observed=old;
    const auto diskBefore=DiskState(path);const auto result=store.Read(observed);const auto diskAfter=DiskState(path);
    // Read may create its own empty .lock only when initialization failed before
    // locking. Existing bytes must always survive; no repair is attempted.
    ContainsOld(diskBefore,diskAfter);
    LockReleased(path);
    if(result==Result::RecoveryRequired){
        Check(injected&&op!=Operation::Read&&Same(observed,old),"only failed writes may demand explicit recovery, retaining read output");
        Check(BlobCount(diskAfter,L"snapshots")+BlobCount(diskAfter,L"commits")>0,"recovery refusal has actual retained metadata evidence");
        ++recoveryRequired;catalog::Snapshot attempt=old;
        Check(store.Initialize(NewBytes,attempt)==Result::RecoveryRequired&&Same(attempt,old),"partial metadata is never silently reinitialized");
        if(op==Operation::InitializeOwned)
            Check(store.InitializeOwned(RequestedOwner,NewBytes,attempt)==Result::RecoveryRequired&&Same(attempt,old),
                "owned initialization cannot reuse its identity to overwrite partial metadata");
        Check(DiskState(path)==diskAfter,"recovery-required metadata remains untouched by refused reinitialization");
        return result;
    }
    if(Initializing(op)&&result==Result::Missing){
        Check(injected&&Same(observed,old)&&BlobCount(diskAfter,L"snapshots")==0&&BlobCount(diskAfter,L"commits")==0,"Missing initialization has no hidden blob or published head");return result;
    }
    Check(result==Result::Ok,"reread returns complete committed head or explicit initialization recovery/missing");
    if(op==Operation::Read){Check(Same(observed,old),"failed Read cannot change previously committed snapshot");return result;}
    if(Initializing(op)){
        Check(observed.IsValid()&&observed.Revision()==1&&observed.Bytes()==NewBytes&&observed.StoreId()!=files::Owner{},"recovered initialization is complete revision one");
        if(op==Operation::InitializeOwned)
            Check(observed.StoreId()==RequestedOwner,"owned initialization rereads the exact caller identity even after an indeterminate result");
        Check(!injected||call.result==Result::Indeterminate,"failed caller may observe published initialization only as Indeterminate");return result;
    }
    Check(observed.StoreId()==old.StoreId(),"failed commit never replaces persistent store identity");
    if(observed.Revision()==old.Revision()){
        Check(Same(observed,old)&&injected,"failed Commit rereads complete unchanged old head");
        if(BlobCount(diskAfter,L"snapshots")>BlobCount(diskAfter,L"commits"))++orphanCases;
    }else{
        Check(observed.Revision()==old.Revision()+1&&observed.Bytes()==NewBytes,"reread new head has exact revision and full requested bytes");
        Check(!injected||call.result==Result::Indeterminate,"published new head after caller failure is reported Indeterminate");
    }
    return result;
}
struct Fixture {
    fs::path casePath,path;
    catalog::Snapshot old;
    Fixture(const fs::path& group,std::size_t ordinal,Operation op):casePath(group/Number(ordinal)),path(casePath/L"metadata"){
        NewDirectory(casePath);
        if(!Initializing(op)){old=Initialize(path,OldBytes);
            if(op==Operation::Read){Check(catalog::Store(path).Commit(old,NewBytes,old)==Result::Ok&&old.Revision()==2,"Read scan includes a real two-record chain");}}
    }
};
std::size_t Scan(Operation op,const fs::path& root,OutputOrigin& origins){
    const auto group=root/Label(op);NewDirectory(group);std::size_t total=0;
    {Fixture fixture(group,0,op);catalog::Store store(fixture.path);auto output=origins.Prepare();
        const auto call=Invoke(op,store,fixture.old,output,allocation::CountOnly);
        Check(call.result==Result::Ok&&call.hits==0,"unfaulted baseline operation succeeds");total=call.attempts;
        if(op==Operation::Read)Check(Same(output,fixture.old),"baseline Read publishes exact requested snapshot");
        else Check(output.IsValid()&&output.Bytes()==NewBytes&&output.Revision()==(Initializing(op)?1:fixture.old.Revision()+1)&&
            (Initializing(op)?output.StoreId()!=files::Owner{}:output.StoreId()==fixture.old.StoreId()),"baseline write publishes complete output snapshot");
        if(op==Operation::InitializeOwned)
            Check(output.StoreId()==RequestedOwner,"successful owned initialization publishes precisely the requested identity");
        Check(total>0&&total<=MaximumOrdinals,"successful allocation count is nonzero and bounded");
        const auto read=VerifyAfter(op,fixture.path,fixture.old,call,false);Record(fixture.casePath,call,read,allocation::CountOnly,BlobCount(DiskState(fixture.path),L"snapshots"),BlobCount(DiskState(fixture.path),L"commits"));}
    std::printf("allocation baseline %ls=%llu\n",Label(op),static_cast<unsigned long long>(total));
    for(std::size_t ordinal=0;ordinal<total;++ordinal){
        Fixture fixture(group,ordinal+1,op);catalog::Store store(fixture.path);auto output=origins.Prepare();const auto before=output;const auto disk=DiskState(fixture.path);
        const auto call=Invoke(op,store,fixture.old,output,ordinal);
        Check(call.hits==1,"every requested current-thread allocation ordinal really faults exactly once");
        Check(call.result==Result::Io||((op!=Operation::Read)&&call.result==Result::Indeterminate),"injected allocation failure reports Io or post-attempt Indeterminate");
        if(call.result==Result::Indeterminate)++indeterminate;
        origins.Prove(output,before);const auto after=DiskState(fixture.path);ContainsOld(disk,after);
        if(op==Operation::Read)Check(disk==after,"failed Read leaves every directory entry and byte unchanged");
        const auto read=VerifyAfter(op,fixture.path,fixture.old,call,true);
        Record(fixture.casePath,call,read,ordinal,BlobCount(after,L"snapshots"),BlobCount(after,L"commits"));
    }
    return total;
}
void AliasChecks(const fs::path& root){
    const auto group=root/L"alias";NewDirectory(group);std::size_t commitAllocations=0;
    // Calibrate the actual alias call under equal-length fixture paths. The
    // non-alias group's path storage can require a different allocation count.
    {Fixture fixture(group,0,Operation::Commit);catalog::Store store(fixture.path);auto output=fixture.old;
        const auto call=Invoke(Operation::Commit,store,output,output,allocation::CountOnly);commitAllocations=call.attempts;
        NewText(fixture.casePath/L"call.txt","ordinal="+std::to_string(allocation::CountOnly)+"\nattempts="+std::to_string(call.attempts)+"\nhits="+std::to_string(call.hits)+"\nresult="+std::to_string(static_cast<int>(call.result))+"\n");
        Check(call.result==Result::Ok&&call.hits==0&&commitAllocations>0&&commitAllocations<=MaximumOrdinals&&
            output.IsValid()&&output.Revision()==fixture.old.Revision()+1&&output.StoreId()==fixture.old.StoreId()&&output.Bytes()==NewBytes,"independent alias allocation baseline succeeds");
        const auto read=VerifyAfter(Operation::Commit,fixture.path,fixture.old,call,false);
        const auto disk=DiskState(fixture.path);Record(fixture.casePath,call,read,allocation::CountOnly,BlobCount(disk,L"snapshots"),BlobCount(disk,L"commits"));}
    std::printf("allocation baseline alias=%llu\n",static_cast<unsigned long long>(commitAllocations));
    std::vector<std::size_t> points{0,commitAllocations/2,commitAllocations-1};std::sort(points.begin(),points.end());points.erase(std::unique(points.begin(),points.end()),points.end());
    for(std::size_t i=0;i<points.size();++i){Fixture fixture(group,i+1,Operation::Commit);catalog::Store store(fixture.path);auto output=fixture.old;const auto before=output;
        NewText(fixture.casePath/L"planned.txt","ordinal="+std::to_string(points[i])+"\nbaselineAttempts="+std::to_string(commitAllocations)+"\n");
        const auto call=Invoke(Operation::Commit,store,output,output,points[i]);
        NewText(fixture.casePath/L"call.txt","ordinal="+std::to_string(points[i])+"\nattempts="+std::to_string(call.attempts)+"\nhits="+std::to_string(call.hits)+"\nresult="+std::to_string(static_cast<int>(call.result))+"\n");
        Check(call.hits==1&&(call.result==Result::Io||call.result==Result::Indeterminate)&&Same(output,before),"aliased expected/output retains public snapshot after an actual failure");
        const auto read=VerifyAfter(Operation::Commit,fixture.path,before,call,true);catalog::Snapshot current;
        if(read==Result::Ok){Check(store.Read(current)==Result::Ok,"reread alias head before private-token check");catalog::Snapshot proof;
            if(current.Revision()==before.Revision())Check(store.Commit(output,output.Bytes(),proof)==Result::Ok&&proof.Revision()==before.Revision()+1,"failed alias retains usable full current CAS token");
            else{const auto disk=DiskState(fixture.path);Check(store.Commit(output,output.Bytes(),proof)==Result::Conflict&&!proof.IsValid()&&DiskState(fixture.path)==disk,"failed alias remains stale after already-published commit");}}
        Record(fixture.casePath,call,read,points[i],BlobCount(DiskState(fixture.path),L"snapshots"),BlobCount(DiskState(fixture.path),L"commits"));
    }
}
bool SameInfo(const files::BlobInfo& a,const files::BlobInfo& b){return a.name==b.name&&a.spec.owner==b.spec.owner&&a.spec.kind==b.spec.kind&&a.spec.key==b.spec.key&&a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;}
bool SameRecord(const codec::Record& a,const codec::Record& b){return a.revision==b.revision&&a.owner==b.owner&&SameInfo(a.snapshot,b.snapshot)&&SameInfo(a.previous,b.previous);}
void CodecScans(const fs::path& root){
    codec::Record input;input.revision=2;input.owner[0]=1;
    input.snapshot.name=L"0123456789abcdef0123456789abcdef.blob";input.snapshot.spec.owner=input.owner;input.snapshot.spec.kind=files::Kind::CatalogSnapshot;input.snapshot.spec.key[0]=1;input.snapshot.identity.fileBytes=128;
    input.previous.name=L"fedcba9876543210fedcba9876543210.blob";input.previous.spec.owner=codec::NamespaceOwner;input.previous.spec.kind=files::Kind::CatalogCommit;input.previous.spec.key[0]=2;input.previous.payloadBytes=368;input.previous.identity.fileBytes=496;
    Bytes encoded;codec::Result result;std::size_t encodeCount=0,decodeCount=0;
    {allocation::Scope scope(allocation::CountOnly);result=codec::Encode(input,encoded);encodeCount=scope.state.attempts;}
    Check(result==codec::Result::Ok&&encodeCount>0&&encodeCount<64,"codec Encode baseline allocations");
    for(std::size_t i=0;i<encodeCount;++i){Bytes output{0xab,0xcd};std::size_t hits=0;
        {allocation::Scope scope(i);result=codec::Encode(input,output);hits=scope.state.hits;}
        Check(hits==1&&result==codec::Result::Io&&output==Bytes({0xab,0xcd}),"every Encode allocation fault preserves output");}
    codec::Record decoded;
    {allocation::Scope scope(allocation::CountOnly);result=codec::Decode(encoded.data(),encoded.size(),decoded);decodeCount=scope.state.attempts;}
    Check(result==codec::Result::Ok&&SameRecord(decoded,input)&&decodeCount>0&&decodeCount<64,"codec Decode baseline allocations");
    for(std::size_t i=0;i<decodeCount;++i){auto output=input;output.revision=17;output.snapshot.payloadDigest[4]=33;const auto before=output;std::size_t hits=0;
        {allocation::Scope scope(i);result=codec::Decode(encoded.data(),encoded.size(),output);hits=scope.state.hits;}
        Check(hits==1&&result==codec::Result::Io&&SameRecord(output,before),"every Decode allocation fault preserves every Record field");}
    NewText(root/L"codec-counts.txt","Encode="+std::to_string(encodeCount)+"\nDecode="+std::to_string(decodeCount)+"\n");
}
void Calibration(){
    using Allocate=void*(*)(std::size_t);Allocate volatile allocate=static_cast<Allocate>(&::operator new);
    bool caught=false;std::size_t hits=0;
    {allocation::Scope scope(0);try{void* p=allocate(71);::operator delete(p);}catch(const std::bad_alloc&){caught=true;}hits=scope.state.hits;}
    Check(caught&&hits==1,"calibrate actual ordinary allocation fault");
    {allocation::Scope scope(1);void* first=allocate(73);::operator delete(first);bool failed=false;
        try{void* second=allocate(79);::operator delete(second);}catch(const std::bad_alloc&){failed=true;}
        void* third=allocate(83);::operator delete(third);caught=failed;hits=scope.state.hits;}
    Check(caught&&hits==1,"fault is ordinal-specific and cleanup allocations remain available");
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage: --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);NewDirectory(root);
        Calibration();CodecScans(root);OutputOrigin origins(root/L"output-origins");
        const auto read=Scan(Operation::Read,root,origins);const auto initialize=Scan(Operation::Initialize,root,origins);
        const auto owned=Scan(Operation::InitializeOwned,root,origins);const auto commit=Scan(Operation::Commit,root,origins);AliasChecks(root);
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\nreadOrdinals="+std::to_string(read)+"\ninitializeOrdinals="+std::to_string(initialize)+"\ninitializeOwnedOrdinals="+std::to_string(owned)+"\ncommitOrdinals="+std::to_string(commit)+"\nindeterminate="+std::to_string(indeterminate)+"\nrecoveryRequired="+std::to_string(recoveryRequired)+"\ncompleteOrphanCases="+std::to_string(orphanCases)+"\nCoverage: ordinary current-thread new/new[] only; no aligned allocations, CRT malloc, Win32/CNG/system allocator faults or power-loss claims. Recovery-required evidence is retained, never repaired/deleted.\n");
        std::printf("PASS cache catalog allocation checks=%u ownedOrdinals=%llu recoveryRequired=%u indeterminate=%u evidence=%ls\n",checks,static_cast<unsigned long long>(owned),recoveryRequired,indeterminate,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog allocation checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog allocation unexpected exception\n");return 2;}
}
