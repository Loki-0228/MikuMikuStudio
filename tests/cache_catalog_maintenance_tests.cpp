// MAINTENANCE: regression-keep. Explicit checkpoint/collection; CPU-only owned fixtures.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "mikumikustudio/cache_catalog.hpp"
#include "../src/io/cache_catalog_codec.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
namespace catalog=mikumikustudio::cache_catalog;
namespace codec=mikumikustudio::cache_catalog::codec;
using Bytes=std::vector<std::uint8_t>;
using Result=catalog::Result;
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
void NewDirectory(const fs::path& p){
    Check(p.is_absolute()&&p.native().size()>3&&p!=p.root_path()&&p==p.lexically_normal(),"test directory must be normalized absolute non-root");
    Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"test directory must be newly created");
}
void NewBytes(const fs::path& p,const Bytes& b){
    Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"create owned evidence without replacement");
    DWORD done=0;Check(b.size()<=MAXDWORD&&WriteFile(h.value,b.data(),static_cast<DWORD>(b.size()),&done,nullptr)&&done==b.size(),"write owned evidence");
}
void NewText(const fs::path& p,const std::string& s){NewBytes(p,Bytes(s.begin(),s.end()));}
Bytes ReadBytes(const fs::path& p){std::ifstream f(p,std::ios::binary);Check(bool(f),"read owned fixture");return Bytes(std::istreambuf_iterator<char>(f),{});}
files::Owner Owner(){files::Owner x{};for(unsigned i=0;i<x.size();++i)x[i]=static_cast<std::uint8_t>(i+1);return x;}
files::Owner NamespaceOwner(){files::Owner x{};const char text[]="MMSCatalogHead01";std::copy_n(text,16,x.begin());return x;}
files::Spec Spec(bool commit,const files::Owner& owner){files::Spec x;x.owner=owner;x.kind=commit?files::Kind::CatalogCommit:files::Kind::CatalogSnapshot;x.key[0]=commit?2:1;return x;}
files::Directory Open(const fs::path& p,bool create=true){files::Directory d;Check(files::Directory::Open(p,create,d)==files::Result::Ok,"open owned file directory");return d;}
files::BlobInfo Create(files::Directory& d,const files::Spec& s,const Bytes& b){files::BlobInfo info;Check(d.Create(s,b.data(),b.size(),info)==files::Result::Ok,"create real verified blob");return info;}
bool Equal(const files::BlobInfo& a,const files::BlobInfo& b){return a.name==b.name&&a.spec.kind==b.spec.kind&&a.spec.owner==b.spec.owner&&a.spec.key==b.spec.key&&a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;}
bool Equal(const codec::Record& a,const codec::Record& b){return a.revision==b.revision&&a.owner==b.owner&&Equal(a.snapshot,b.snapshot)&&Equal(a.previous,b.previous);}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();}
void Put(Bytes& b,std::size_t at,std::uint64_t v,unsigned width){for(unsigned i=0;i<width;++i)b.at(at+i)=static_cast<std::uint8_t>(v>>(8*i));}
std::uint64_t Get(const Bytes& b,std::size_t at,unsigned width){std::uint64_t v=0;for(unsigned i=0;i<width;++i)v|=std::uint64_t(b.at(at+i))<<(8*i);return v;}
void GoldenRef(Bytes& b,std::size_t at,const files::BlobInfo& r){
    for(unsigned i=0;i<37;++i)b.at(at+i)=static_cast<std::uint8_t>(r.name.at(i));
    Put(b,at+40,static_cast<std::uint32_t>(r.spec.kind),4);
    std::copy(r.spec.owner.begin(),r.spec.owner.end(),b.begin()+at+48);
    std::copy(r.spec.key.begin(),r.spec.key.end(),b.begin()+at+64);
    std::copy(r.payloadDigest.begin(),r.payloadDigest.end(),b.begin()+at+96);
    Put(b,at+128,r.payloadBytes,8);Put(b,at+136,r.identity.volume,4);
    Put(b,at+144,r.identity.fileIndex,8);Put(b,at+152,r.identity.fileBytes,8);
}
// Independent wire oracle: neither production Encode nor Decode participates.
Bytes Golden(const codec::Record& r){
    Bytes b(368,0);const char magic[]="MMSCacheHead0001";std::copy_n(magic,16,b.begin());
    Put(b,16,1,4);Put(b,24,r.revision,8);std::copy(r.owner.begin(),r.owner.end(),b.begin()+32);
    GoldenRef(b,48,r.snapshot);if(r.revision>1)GoldenRef(b,208,r.previous);return b;
}
files::BlobInfo Ref(const Bytes& b,std::size_t at){
    files::BlobInfo r;for(unsigned i=0;i<37;++i)r.name+=static_cast<wchar_t>(b.at(at+i));
    r.spec.kind=static_cast<files::Kind>(Get(b,at+40,4));
    std::copy_n(b.begin()+at+48,16,r.spec.owner.begin());std::copy_n(b.begin()+at+64,32,r.spec.key.begin());
    std::copy_n(b.begin()+at+96,32,r.payloadDigest.begin());r.payloadBytes=Get(b,at+128,8);
    r.identity.volume=static_cast<std::uint32_t>(Get(b,at+136,4));r.identity.fileIndex=Get(b,at+144,8);r.identity.fileBytes=Get(b,at+152,8);return r;
}
files::Spec CheckpointSpec(){auto s=Spec(true,NamespaceOwner());s.kind=files::Kind::CatalogCheckpoint;s.key[0]=3;return s;}
bool Equal(const codec::CheckpointRecord& a,const codec::CheckpointRecord& b){if(!Equal(a.state,b.state)||a.retired.size()!=b.retired.size())return false;for(std::size_t i=0;i<a.retired.size();++i)if(!Equal(a.retired[i],b.retired[i]))return false;return true;}
bool RetiredLess(const files::BlobInfo& a,const files::BlobInfo& b){const bool as=a.spec.kind==files::Kind::CatalogSnapshot,bs=b.spec.kind==files::Kind::CatalogSnapshot;return as!=bs?!as:a.name<b.name;}
Bytes GoldenCheckpoint(const codec::CheckpointRecord& r){
    auto b=Golden(r.state);b.resize(376+160*r.retired.size(),0);const char magic[]="MMSCacheBase0001";std::copy_n(magic,16,b.begin());
    Put(b,368,r.retired.size(),4);for(std::size_t i=0;i<r.retired.size();++i)GoldenRef(b,376+160*i,r.retired[i]);return b;
}
std::wstring HexName(unsigned n){wchar_t name[64]{};std::swprintf(name,64,L"%032x.blob",n);return name;}
files::BlobInfo ExampleRef(unsigned name,files::Kind kind){
    files::BlobInfo r;r.name=HexName(name);r.spec=kind==files::Kind::CatalogSnapshot?Spec(false,Owner()):kind==files::Kind::CatalogCheckpoint?CheckpointSpec():Spec(true,NamespaceOwner());
    r.payloadBytes=kind==files::Kind::CatalogSnapshot?19:kind==files::Kind::CatalogCommit?368:536;
    r.identity={0x10203040,0x1122334455667788ull+name,128+r.payloadBytes};for(unsigned i=0;i<32;++i)r.payloadDigest[i]=static_cast<std::uint8_t>(i+name);return r;
}
codec::CheckpointRecord Example(){
    codec::CheckpointRecord r;r.state.revision=13;r.state.owner=Owner();r.state.snapshot=ExampleRef(30,files::Kind::CatalogSnapshot);
    r.state.previous=ExampleRef(20,files::Kind::CatalogCheckpoint);
    r.retired={ExampleRef(10,files::Kind::CatalogCommit),r.state.previous,ExampleRef(5,files::Kind::CatalogSnapshot)};return r;
}
void CodecChecks(){
    const auto input=Example();const auto golden=GoldenCheckpoint(input);Bytes encoded{1,2};
    Check(codec::EncodeCheckpoint(input,encoded)==codec::Result::Ok&&encoded==golden,"checkpoint entire byte layout matches independent golden offsets");
    auto decoded=input;decoded.state.revision=99;Check(codec::DecodeCheckpoint(golden.data(),golden.size(),decoded)==codec::Result::Ok&&Equal(decoded,input),"decode independently constructed checkpoint bytes");
    auto ordinary=input.state;ordinary.revision=14;Bytes plain;Check(codec::Encode(ordinary,plain)==codec::Result::Ok&&plain==Golden(ordinary),"ordinary commit may reference a checkpoint with exact wire identity");
    codec::Record ordinaryOut;Check(codec::Decode(plain.data(),plain.size(),ordinaryOut)==codec::Result::Ok&&Equal(ordinaryOut,ordinary),"ordinary previous-checkpoint decoding");
    const auto reject=[&](const Bytes& b){auto out=input;Check(codec::DecodeCheckpoint(b.data(),b.size(),out)==codec::Result::Invalid&&Equal(out,input),"invalid checkpoint retains all prior output fields");};
    for(unsigned at:{0u,15u,16u,20u,21u,22u,23u,372u,373u,374u,375u}){auto bad=golden;bad[at]^=0x80;reject(bad);}
    for(std::size_t ref:{48u,208u,376u,536u,696u})for(unsigned offset:{37u,38u,39u,44u,45u,46u,47u,140u,141u,142u,143u}){auto bad=golden;bad[ref+offset]=1;reject(bad);}
    for(unsigned count:{0u,193u,0xffffffffu}){auto bad=golden;Put(bad,368,count,4);reject(bad);}
    {auto bad=golden;Put(bad,24,1,8);reject(bad);bad=golden;bad.push_back(0);reject(bad);bad=golden;bad.pop_back();reject(bad);}
    {auto out=input;Check(codec::DecodeCheckpoint(nullptr,golden.size(),out)==codec::Result::Invalid&&Equal(out,input),"null checkpoint rejected");}
    std::vector<codec::CheckpointRecord> badRecords;
    auto bad=input;bad.retired.push_back(bad.retired.back());badRecords.push_back(bad);
    bad=input;std::swap(bad.retired[0],bad.retired[1]);badRecords.push_back(bad);
    bad=input;bad.retired.erase(bad.retired.begin()+1);badRecords.push_back(bad);
    bad=input;bad.retired.push_back(bad.state.snapshot);badRecords.push_back(bad);
    bad=input;bad.retired[1].identity.fileIndex^=1;badRecords.push_back(bad);
    bad=input;bad.retired[0].spec.kind=files::Kind::Texture;badRecords.push_back(bad);
    bad=input;bad.retired[2].spec.owner[0]^=1;badRecords.push_back(bad);
    bad=input;bad.retired[0].spec.key[1]=1;badRecords.push_back(bad);
    bad=input;bad.retired[2].identity.fileBytes++;badRecords.push_back(bad);
    bad=input;bad.retired[0].name=bad.retired[1].name;badRecords.push_back(bad); // commit/checkpoint share one directory.
    for(const auto& r:badRecords){Bytes out{0xab};Check(codec::EncodeCheckpoint(r,out)==codec::Result::Invalid&&out==Bytes{0xab},"invalid retirement input retains encoded output");reject(GoldenCheckpoint(r));}
    auto maximum=input;maximum.retired.clear();for(unsigned i=0;i<191;++i)maximum.retired.push_back(ExampleRef(100+i,files::Kind::CatalogCommit));
    maximum.state.previous=ExampleRef(400,files::Kind::CatalogCheckpoint);maximum.retired.push_back(maximum.state.previous);
    Check(codec::EncodeCheckpoint(maximum,encoded)==codec::Result::Ok&&encoded==GoldenCheckpoint(maximum)&&encoded.size()==31096&&encoded.size()+128<=32768,"192-reference maximum fits only declared maintenance reserve");
    decoded={};Check(codec::DecodeCheckpoint(encoded.data(),encoded.size(),decoded)==codec::Result::Ok&&Equal(decoded,maximum),"maximum complete checkpoint roundtrip");
}
struct Entry {files::BlobInfo info;Bytes payload;};
std::vector<Entry> Entries(const fs::path& path,const files::Owner& owner){
    auto d=Open(path,false);std::vector<std::wstring> names;Check(d.Enumerate(256,names)==files::Result::Ok,"enumerate actual metadata blobs");std::vector<Entry> out;
    for(const auto& name:names){files::ReadLease lease;Check(d.Inspect(name,owner,16*1024*1024,lease)==files::Result::Ok,"verify actual metadata envelope");Bytes bytes;Check(lease.ReadAll(bytes)==files::Result::Ok,"read actual metadata payload");out.push_back({lease.Info(),std::move(bytes)});}return out;
}
Entry Head(const fs::path& path){auto entries=Entries(path/L"commits",NamespaceOwner());Check(!entries.empty(),"actual head exists");return *std::max_element(entries.begin(),entries.end(),[](const auto& a,const auto& b){return Get(a.payload,24,8)<Get(b.payload,24,8);});}
std::vector<files::BlobInfo> Retired(const Entry& cp){Check(cp.info.spec.kind==files::Kind::CatalogCheckpoint&&cp.payload.size()>=376,"independent reader observes checkpoint kind and minimum size");const auto n=Get(cp.payload,368,4);Check(n>0&&n<=192&&cp.payload.size()==376+160*n,"independent retirement count and exact size");std::vector<files::BlobInfo> out;for(std::size_t i=0;i<n;++i)out.push_back(Ref(cp.payload,376+160*i));return out;}
fs::path FilePath(const fs::path& root,const files::BlobInfo& info){return root/(info.spec.kind==files::Kind::CatalogSnapshot?L"snapshots":L"commits")/info.name;}
std::vector<std::wstring> Names(const fs::path& path){auto d=Open(path,false);std::vector<std::wstring> out;Check(d.Enumerate(256,out)==files::Result::Ok,"enumerate exact filenames");std::sort(out.begin(),out.end());return out;}
std::uint64_t PhysicalBytes(const fs::path& path){std::uint64_t bytes=0;for(const auto* folder:{L"snapshots",L"commits"})for(const auto& entry:fs::directory_iterator(path/folder))bytes+=entry.file_size();return bytes;}
using Disk=std::vector<std::pair<fs::path,Bytes>>;
Disk DiskState(const fs::path& root){Disk out;for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file())out.emplace_back(fs::relative(entry.path(),root),ReadBytes(entry.path()));std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first<b.first;});return out;}
catalog::Snapshot Init(const fs::path& path,const Bytes& bytes={0xff,0,0x80}){catalog::Snapshot s;Check(catalog::Store(path).Initialize(bytes,s)==Result::Ok&&s.IsValid()&&s.Revision()==1&&s.Bytes()==bytes,"initialize owned maintenance store");return s;}
void ReadEquals(const fs::path& path,const catalog::Snapshot& expected){catalog::Snapshot actual;Check(catalog::Store(path).Read(actual)==Result::Ok&&Same(actual,expected),"fresh Store reads exact current revision owner and bytes");}
bool SameReport(const catalog::CollectionReport& a,const catalog::CollectionReport& b){return a.attempted==b.attempted&&a.absent==b.absent&&a.busy==b.busy&&a.pending==b.pending&&a.failed==b.failed&&a.absentBytes==b.absentBytes;}
catalog::CollectionReport ReportSentinel(){return {11,12,13,14,15,0x123456789ull};}
void CheckReport(const catalog::CollectionReport& report){Check(std::uint64_t(report.absent)+report.busy+report.pending+report.failed==report.attempted,"collection report partitions only actually attempted present retirees");}
void CollectAll(const fs::path& path,const catalog::Snapshot& current){
    const auto head=Head(path);const auto live=Ref(head.payload,48);const auto refs=Retired(head);catalog::CollectionReport report=ReportSentinel();
    Check(catalog::Store(path).CollectRetired(report)==Result::Ok&&report.busy==0&&report.failed==0,"unleased authorized retired generations collect without busy/failure");
    CheckReport(report);
    for(const auto& ref:refs)Check(!fs::exists(FilePath(path,ref)),"retired namespace entry actually disappears");
    Check(fs::exists(FilePath(path,head.info))&&fs::exists(FilePath(path,live)),"collection retains current checkpoint and live snapshot");ReadEquals(path,current);
}
void BoundaryAndLeases(const fs::path& root){
    const auto path=root/L"head-boundary";auto s=Init(path);catalog::Store store(path);const auto first=Head(path);auto snapshots=Open(path/L"snapshots",false),heads=Open(path/L"commits",false);
    files::ReadLease oldSnapshot,oldHead;Check(snapshots.Acquire(Ref(first.payload,48),oldSnapshot)==files::Result::Ok&&heads.Acquire(first.info,oldHead)==files::Result::Ok,"hold actual retired snapshot and commit leases");snapshots={};heads={};
    for(unsigned i=2;i<=64;++i)Check(store.Commit(s,Bytes{static_cast<std::uint8_t>(i)},s)==Result::Ok&&s.Revision()==i,"ordinary records reach original 64 limit");
    auto before=s;const auto names=Names(path/L"snapshots");const auto disk=DiskState(path);
    Check(store.Commit(s,{},s)==Result::Limit&&Same(s,before)&&DiskState(path)==disk,"ordinary 65th head cannot consume maintenance slot");
    Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==65&&s.Bytes()==before.Bytes()&&s.StoreId()==before.StoreId(),"alias checkpoint retains same value and advances global revision");
    Check(Names(path/L"snapshots")==names&&Names(path/L"commits").size()==65,"checkpoint uses exactly one reserved head and no copied snapshot");
    const auto cp=Head(path);const auto live=Ref(cp.payload,48);const auto refs=Retired(cp);Check(refs.size()==127,"64 heads plus 63 old snapshots are explicitly retired");
    const auto after=s;const auto checkpointDisk=DiskState(path);
    Check(store.Checkpoint(before,s)==Result::Conflict&&Same(s,after)&&DiskState(path)==checkpointDisk,"pre-checkpoint expected cannot publish stale checkpoint");
    Check(store.Commit(before,{},s)==Result::Conflict&&Same(s,after),"old expected also conflicts for ordinary commit");
    Check(store.Commit(s,{},s)==Result::Limit&&Same(s,after)&&store.Checkpoint(s,s)==Result::Limit&&Same(s,after),"65 physical heads prevent second reserve and ordinary publication until collection");
    catalog::CollectionReport report=ReportSentinel();Check(store.CollectRetired(report)==Result::Ok&&report.busy==2&&report.failed==0&&report.absent==125&&report.pending==0&&report.attempted==127,"external read leases produce exactly two Busy entries while 125 retired files disappear");CheckReport(report);
    std::uint64_t removedBytes=0;for(const auto& ref:refs)if(!Equal(ref,first.info)&&!Equal(ref,Ref(first.payload,48)))removedBytes+=ref.identity.fileBytes;
    Check(report.absentBytes==removedBytes,"reported absent bytes match exact disappeared immutable files");
    Check(fs::exists(FilePath(path,first.info))&&fs::exists(FilePath(path,Ref(first.payload,48))),"leased retired metadata is not deleted");
    for(const auto& ref:refs)if(!Equal(ref,first.info)&&!Equal(ref,Ref(first.payload,48)))Check(!fs::exists(FilePath(path,ref)),"internal validation leases do not falsely block unleased retirees");
    Check(fs::exists(FilePath(path,cp.info))&&fs::exists(FilePath(path,live)),"busy collection retains active checkpoint and payload");ReadEquals(path,s);
    // Publish a second checkpoint while two prior retirement entries remain.
    Check(store.Commit(s,Bytes{0x73},s)==Result::Ok,"collection reclaimed enough physical slots to resume commits");
    Check(store.Checkpoint(s,s)==Result::Ok,"next checkpoint incorporates remaining old retirees");const auto second=Head(path);const auto secondRefs=Retired(second);
    for(const auto& retained:{first.info,Ref(first.payload,48),cp.info})Check(std::any_of(secondRefs.begin(),secondRefs.end(),[&](const auto& x){return Equal(x,retained);}),"new checkpoint names previous still-present retiree by full identity");
    oldSnapshot={};oldHead={};CollectAll(path,s);
    Check(Names(path/L"commits").size()==1&&Names(path/L"snapshots").size()==1,"release permits actual reclamation to one checkpoint and one snapshot");
    // Later active tail snapshots must never be mistaken for obsolete payloads.
    Check(store.Commit(s,Bytes{1},s)==Result::Ok,"first active tail commit");const auto tail1=Head(path);
    Check(store.Commit(s,Bytes{2},s)==Result::Ok,"second active tail commit");const auto tail2=Head(path);
    report=ReportSentinel();Check(store.CollectRetired(report)==Result::Ok&&report.attempted==0&&report.absent==0&&report.busy==0&&report.pending==0&&report.failed==0&&report.absentBytes==0,"already-absent retirees do not count as new collection attempts");CheckReport(report);
    for(const auto& item:{second.info,Ref(second.payload,48),tail1.info,Ref(tail1.payload,48),tail2.info,Ref(tail2.payload,48)})Check(fs::exists(FilePath(path,item)),"all active chain heads and payloads survive collection");ReadEquals(path,s);
    {auto d=Open(path,false);files::FileLock lock;Check(d.Lock(0,lock)==files::Result::Ok,"hold maintenance root lock");auto out=s;auto original=ReportSentinel();report=original;
        Check(store.Checkpoint(s,out)==Result::Busy&&Same(out,s),"busy checkpoint retains output");Check(store.CollectRetired(report)==Result::Busy&&SameReport(report,original),"setup Busy preserves report");}
}
void ThousandCommits(const fs::path& root){
    const auto path=root/L"thousand";auto s=Init(path,{});catalog::Store store(path);const auto owner=s.StoreId();std::uint64_t revision=1;unsigned checkpoints=0;
    for(unsigned i=1;i<=1000;++i){Bytes bytes(8);Put(bytes,0,i,8);Check(store.Commit(s,bytes,s)==Result::Ok&&s.Revision()==++revision&&s.Bytes()==bytes&&s.StoreId()==owner,"actual commit advances without resetting global revision");
        Check(Names(path/L"commits").size()<=33&&Names(path/L"snapshots").size()<=33&&PhysicalBytes(path)<128*1024,"periodic maintenance keeps real physical metadata bounded");
        if(i%32==0||i==1000){const auto payloads=Names(path/L"snapshots");Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==++revision&&s.Bytes()==bytes&&s.StoreId()==owner,"periodic same-value checkpoint");++checkpoints;
            Check(Names(path/L"snapshots")==payloads,"maintenance does not duplicate payload bytes");CollectAll(path,s);Check(Names(path/L"commits").size()==1&&Names(path/L"snapshots").size()==1,"each cycle truly removes retired namespace entries");}}
    ReadEquals(path,s);NewText(root/L"thousand-result.txt","commits=1000\ncheckpoints="+std::to_string(checkpoints)+"\nrevision="+std::to_string(revision)+"\nphysicalBytes="+std::to_string(PhysicalBytes(path))+"\n");
}
void ByteReserve(const fs::path& root){
    const auto path=root/L"byte-reserve";Bytes large(16*1024*1024,0x59);auto s=Init(path,large);catalog::Store store(path);
    Check(store.Commit(s,large,s)==Result::Ok&&store.Commit(s,large,s)==Result::Ok,"prepare three maximal snapshots below ordinary byte limit");
    const auto used=PhysicalBytes(path);const auto target=64ull*1024*1024-100;Check(target>used+128&&target-used-128<=16*1024*1024,"independent orphan size fits payload cap");
    {auto d=Open(path/L"snapshots",false);Bytes orphan(static_cast<std::size_t>(target-used-128),0x27);Create(d,Spec(false,s.StoreId()),orphan);}
    Check(PhysicalBytes(path)==target,"fixture is exactly 100 bytes below ordinary metadata budget");const auto before=s;const auto beforeNames=Names(path/L"snapshots");
    Check(store.Commit(s,{},s)==Result::Limit&&Same(s,before)&&PhysicalBytes(path)==target,"ordinary commit cannot borrow checkpoint byte reserve");
    Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==before.Revision()+1&&s.Bytes()==large&&Names(path/L"snapshots")==beforeNames,"checkpoint reuses large payload instead of copying it");
    Check(PhysicalBytes(path)>64ull*1024*1024&&PhysicalBytes(path)<=64ull*1024*1024+32768,"only checkpoint enters bounded 32KiB byte reserve");ReadEquals(path,s);
    const auto reserved=PhysicalBytes(path);const auto checkpoint=s;Check(store.Commit(s,{},s)==Result::Limit&&Same(s,checkpoint)&&PhysicalBytes(path)==reserved,"ordinary append remains blocked while physical retired bytes occupy reserve");
    CollectAll(path,s);Check(PhysicalBytes(path)<17ull*1024*1024,"collection releases namespace accounting for large retired payloads");Check(store.Commit(s,{},s)==Result::Ok,"ordinary appends resume after real collection");
}
void RejectDamaged(const fs::path& path,catalog::Snapshot& s){
    const auto before=s;const auto disk=DiskState(path);catalog::Store store(path);auto report=ReportSentinel();const auto original=report;
    Check(store.Read(s)==Result::RecoveryRequired&&Same(s,before),"invalid checkpoint/fork cannot fall back to old valid head");
    Check(store.Checkpoint(s,s)==Result::RecoveryRequired&&Same(s,before),"invalid metadata cannot be legitimized by checkpointing");
    Check(store.CollectRetired(report)==Result::RecoveryRequired&&SameReport(report,original),"invalid metadata cannot authorize partial collection");Check(DiskState(path)==disk,"recovery refusal preserves every original byte");
}
void CorruptionAndForks(const fs::path& root){
    for(unsigned mode=0;mode<5;++mode){const auto path=root/(L"reject-"+std::to_wstring(mode));auto s=Init(path);catalog::Store store(path);Check(store.Checkpoint(s,s)==Result::Ok,"prepare real checkpoint before untrusted addition");const auto cp=Head(path);
        if(mode==0)NewText(path/L"commits"/L"future-format.txt","unknown record\n");
        if(mode==1){auto bytes=cp.payload;Put(bytes,16,2,4);auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),bytes);}
        if(mode==2){auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),cp.payload);} // Same revision, different immutable generation.
        if(mode==3){Check(store.Commit(s,Bytes{0x31},s)==Result::Ok,"create valid tail for higher competing checkpoint");const auto tail=Head(path);
            codec::CheckpointRecord competing;competing.state.revision=s.Revision()+1;competing.state.owner=s.StoreId();competing.state.snapshot=Ref(tail.payload,48);competing.state.previous=tail.info;
            for(const auto& e:Entries(path/L"commits",NamespaceOwner()))if(!Equal(e.info,cp.info))competing.retired.push_back(e.info);
            for(const auto& e:Entries(path/L"snapshots",s.StoreId()))if(!Equal(e.info,competing.state.snapshot))competing.retired.push_back(e.info);
            std::sort(competing.retired.begin(),competing.retired.end(),RetiredLess);auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),GoldenCheckpoint(competing));}
        if(mode==4){auto bytes=cp.payload;bytes[208+144]^=1;bytes[376+144]^=1;auto d=Open(path/L"commits",false);Create(d,CheckpointSpec(),bytes);} // Correct outer SHA and internally matching previous, incorrect actual file identity.
        RejectDamaged(path,s);
    }
}
void SourceAndReserveChecks(const fs::path& root){
    // Exactly one checkpoint: neither defect can be masked by a competing CP.
    for(unsigned mode=0;mode<2;++mode){
        const auto path=root/(L"checkpoint-source-"+std::to_wstring(mode));auto s=Init(path);const auto previous=Head(path);
        codec::CheckpointRecord cp;cp.state.owner=s.StoreId();cp.state.previous=previous.info;cp.state.snapshot=Ref(previous.payload,48);
        cp.state.revision=mode==0?3:2;cp.retired.push_back(previous.info);
        if(mode==1){auto snapshots=Open(path/L"snapshots",false);cp.retired.push_back(cp.state.snapshot);cp.state.snapshot=Create(snapshots,Spec(false,s.StoreId()),Bytes{0x45,0x67});}
        const auto wire=GoldenCheckpoint(cp);codec::CheckpointRecord parsed;
        Check(codec::DecodeCheckpoint(wire.data(),wire.size(),parsed)==codec::Result::Ok&&Equal(parsed,cp),"source mismatch fixture is independently built and format-valid");
        {auto heads=Open(path/L"commits",false);Create(heads,CheckpointSpec(),wire);}
        Check(Names(path/L"commits").size()==2&&fs::exists(FilePath(path,previous.info)),"single candidate checkpoint retains exact source for relational verification");
        RejectDamaged(path,s);
    }
    // Missing authorized previous is the distinct supported self-contained case.
    {const auto path=root/L"checkpoint-source-retired";auto s=Init(path);catalog::Store store(path);const auto previous=Head(path);
        Check(store.Checkpoint(s,s)==Result::Ok,"publish valid self-contained checkpoint");CollectAll(path,s);
        Check(!fs::exists(FilePath(path,previous.info)),"authorized predecessor is actually absent after collection");ReadEquals(path,s);
        Check(store.Commit(s,Bytes{0x99},s)==Result::Ok,"self-contained checkpoint still permits a contiguous ordinary successor");ReadEquals(path,s);}
    // A verified CP exists, but the active head is ordinary. A valid older CP
    // cannot lend its reserved physical slot to that ordinary successor.
    {const auto path=root/L"ordinary-tail-reserve";auto s=Init(path);catalog::Store store(path);
        for(unsigned revision=2;revision<=63;++revision)Check(store.Commit(s,Bytes{static_cast<std::uint8_t>(revision)},s)==Result::Ok,"prepare 63 physical ordinary records");
        Check(store.Checkpoint(s,s)==Result::Ok&&s.Revision()==64&&Names(path/L"commits").size()==64,"checkpoint exists at ordinary physical boundary");ReadEquals(path,s);const auto cp=Head(path);
        codec::Record tail;tail.revision=65;tail.owner=s.StoreId();tail.snapshot=Ref(cp.payload,48);tail.previous=cp.info;
        const auto wire=Golden(tail);codec::Record parsed;Check(codec::Decode(wire.data(),wire.size(),parsed)==codec::Result::Ok&&Equal(parsed,tail),"injected tail is format-valid and contiguous with exact CP identity");
        {auto heads=Open(path/L"commits",false);Create(heads,Spec(true,NamespaceOwner()),wire);}
        Check(Names(path/L"commits").size()==65&&Head(path).info.spec.kind==files::Kind::CatalogCommit&&PhysicalBytes(path)<64ull*1024*1024,"only the ordinary active head consumes physical reserved slot");
        RejectDamaged(path,s);
    }
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage: --store NEW_ABSOLUTE_DIRECTORY");const fs::path root(argv[2]);NewDirectory(root);
        CodecChecks();BoundaryAndLeases(root);ThousandCommits(root);ByteReserve(root);CorruptionAndForks(root);SourceAndReserveChecks(root);
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\nCPU filesystem and codec checks; no hard interruption, cross-process leases or power-loss claim. Namespace absence is not physical free-space evidence.\n");
        std::printf("PASS cache catalog maintenance checks=%u evidence=%ls\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog maintenance checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog maintenance unexpected exception\n");return 2;}
}
