// MAINTENANCE: regression-keep. CPU-only tests; no user cache or application startup.
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
    Check(p.is_absolute()&&p.native().size()>3,"test directory must be absolute non-root");
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
codec::Record Example(std::uint64_t revision){
    codec::Record r;r.revision=revision;r.owner=Owner();
    r.snapshot.name=L"0123456789abcdef0123456789abcdef.blob";r.snapshot.spec=Spec(false,r.owner);
    for(unsigned i=0;i<32;++i)r.snapshot.payloadDigest[i]=static_cast<std::uint8_t>(128+i);
    r.snapshot.payloadBytes=0x010203;r.snapshot.identity={0x12345678,0x0102030405060708ull,128+0x010203};
    if(revision>1){r.previous=r.snapshot;r.previous.name=L"fedcba9876543210fedcba9876543210.blob";
        r.previous.spec=Spec(true,NamespaceOwner());r.previous.payloadBytes=368;r.previous.identity.fileBytes=496;}
    return r;
}
void CodecChecks(){
    Check(codec::RecordBytes==368&&codec::NamespaceOwner==NamespaceOwner(),"independent protocol constants");
    for(auto revision:{1ull,2ull,0xffffffffffffffffull}){
        const auto r=Example(revision);const auto expected=Golden(r);Bytes output{7,8,9};
        Check(codec::Encode(r,output)==codec::Result::Ok&&output==expected,"encode all 368 independent golden bytes");
        auto decoded=Example(9);Check(codec::Decode(expected.data(),expected.size(),decoded)==codec::Result::Ok&&Equal(decoded,r),"decode independent golden fields");
    }
    const auto good=Golden(Example(2));const auto sentinel=Example(19);
    const auto reject=[&](const Bytes& b){auto out=sentinel;Check(codec::Decode(b.data(),b.size(),out)==codec::Result::Invalid&&Equal(out,sentinel),"invalid wire preserves every Record field");};
    for(std::size_t at:{0u,15u,16u,20u,21u,22u,23u,85u,86u,87u,92u,93u,94u,95u,188u,189u,190u,191u,245u,246u,247u,252u,253u,254u,255u,348u,349u,350u,351u}){
        auto b=good;b[at]^=0x80;reject(b);
    }
    for(std::size_t at:{48u,80u,84u,208u,240u,244u}){auto b=good;b[at]='Z';reject(b);}
    for(std::size_t at:{88u,96u,112u,248u,256u,272u}){auto b=good;b[at]^=0x40;reject(b);}
    {auto b=good;Put(b,24,0,8);reject(b);}
    {auto b=good;std::fill_n(b.begin()+32,16,0);reject(b);}
    {auto b=good;Put(b,176,16*1024*1024ull+1,8);reject(b);}
    for(std::size_t at:{200u,336u,360u}){auto b=good;b[at]^=1;reject(b);}
    {auto b=good;b.push_back(0);reject(b);b=good;b.pop_back();reject(b);}
    {auto out=sentinel;Check(codec::Decode(nullptr,368,out)==codec::Result::Invalid&&Equal(out,sentinel),"null wire rejected without output mutation");}
    for(unsigned i=208;i<368;++i){auto b=Golden(Example(1));b[i]=1;reject(b);}
    std::vector<codec::Record> invalid;
    auto r=Example(2);r.revision=0;invalid.push_back(r);r=Example(2);r.owner={};invalid.push_back(r);
    r=Example(2);r.snapshot.name[0]=L'A';invalid.push_back(r);r=Example(2);r.snapshot.name+=L"x";invalid.push_back(r);
    r=Example(2);r.snapshot.spec.owner[0]^=1;invalid.push_back(r);r=Example(2);r.snapshot.spec.key[1]=1;invalid.push_back(r);
    r=Example(2);r.snapshot.spec.kind=files::Kind::Effect;invalid.push_back(r);
    r=Example(2);r.snapshot.payloadBytes=16*1024*1024ull+1;r.snapshot.identity.fileBytes=128+r.snapshot.payloadBytes;invalid.push_back(r);
    r=Example(2);r.snapshot.identity.fileBytes++;invalid.push_back(r);
    r=Example(2);r.previous.spec.owner[0]^=1;invalid.push_back(r);r=Example(2);r.previous.spec.key[0]=1;invalid.push_back(r);
    r=Example(2);r.previous.payloadBytes++;invalid.push_back(r);r=Example(1);r.previous.identity.volume=1;invalid.push_back(r);
    for(const auto& bad:invalid){Bytes out{9,8,7};Check(codec::Encode(bad,out)==codec::Result::Invalid&&out==Bytes({9,8,7}),"invalid input Record preserves output vector");}
    r=Example(2);r.snapshot.identity.volume=0;r.snapshot.identity.fileIndex=0;r.previous.identity.volume=0;r.previous.identity.fileIndex=0;
    Bytes out;Check(codec::Encode(r,out)==codec::Result::Ok&&out==Golden(r),"zero platform identity fields are representable");
}
using Disk=std::vector<std::pair<fs::path,Bytes>>;
Disk SnapshotDisk(const fs::path& root){Disk out;if(fs::exists(root))for(const auto& e:fs::recursive_directory_iterator(root))if(e.is_regular_file())out.emplace_back(fs::relative(e.path(),root),ReadBytes(e.path()));std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first<b.first;});return out;}
catalog::Snapshot Init(const fs::path& root,const Bytes& bytes={0,0xff,0x80,3}){catalog::Snapshot s;Check(catalog::Store(root).Initialize(bytes,s)==Result::Ok,"initialize empty catalog");Check(s.IsValid()&&s.Revision()==1&&s.Bytes()==bytes&&s.StoreId()!=files::Owner{},"initial snapshot content and persistent identity");return s;}
void PreserveProof(const catalog::Store& store,catalog::Snapshot& output,const catalog::Snapshot& before){
    Check(Same(output,before),"failed operation retains visible snapshot fields");
    catalog::Snapshot next;Check(store.Commit(output,output.Bytes(),next)==Result::Ok&&next.Revision()==before.Revision()+1,"failed output retains private origin and CAS head");output=std::move(next);
}
void Basics(const fs::path& root){
    const auto path=root/L"basic";catalog::Store store(path);catalog::Snapshot s;
    Check(store.Read(s)==Result::Missing&&!s.IsValid()&&!fs::exists(path),"Read never initializes missing catalog");
    s=Init(path);auto before=s;const auto firstId=s.StoreId();
    Check(store.Initialize({},s)==Result::Conflict&&Same(s,before),"existing catalog cannot initialize again");
    PreserveProof(store,s,before);auto stale=before;
    Check(store.Commit(s,s.Bytes(),s)==Result::Ok&&s.Revision()==3&&s.StoreId()==firstId,"same-value expected/output/bytes alias still commits new revision");
    before=s;auto disk=SnapshotDisk(path);Check(store.Commit(stale,Bytes{5},s)==Result::Conflict&&Same(s,before)&&SnapshotDisk(path)==disk,"stale CAS preserves disk and output");
    PreserveProof(store,s,before);
    const auto otherPath=root/L"other";auto other=Init(otherPath,{});catalog::Store otherStore(otherPath);before=s;disk=SnapshotDisk(otherPath);
    Check(otherStore.Commit(s,Bytes{7},s)==Result::Conflict&&Same(s,before)&&SnapshotDisk(otherPath)==disk,"cross-store CAS cannot publish");
    PreserveProof(store,s,before);
    catalog::Snapshot reopened;Check(catalog::Store(path).Read(reopened)==Result::Ok&&Same(reopened,s),"reopen preserves bytes revision and store ID");
    before=s;Check(catalog::Store(L"relative-catalog").Read(s)==Result::Invalid&&Same(s,before),"relative root rejected and output preserved");
    {auto d=Open(path,false);files::FileLock lock;Check(d.Lock(0,lock)==files::Result::Ok,"hold actual metadata root byte lock");
        Check(store.Read(s)==Result::Busy&&Same(s,before),"Read respects real root byte lock");
        Check(store.Commit(s,Bytes{6},s)==Result::Busy&&Same(s,before),"aliased Commit Busy preserves snapshot");}
    PreserveProof(store,s,before);
}
struct Head {files::BlobInfo info;Bytes bytes;};
Head Latest(const fs::path& root){
    auto commits=Open(root/L"commits",false);std::vector<std::wstring> names;Check(commits.Enumerate(100,names)==files::Result::Ok,"enumerate independent commit inspection");
    Head best;std::uint64_t revision=0;
    for(const auto& name:names){files::ReadLease lease;Check(commits.Inspect(name,NamespaceOwner(),368,lease)==files::Result::Ok,"inspect outer commit independently");
        Bytes bytes;Check(lease.ReadAll(bytes)==files::Result::Ok&&bytes.size()==368,"read complete actual commit payload");
        if(Get(bytes,24,8)>revision){revision=Get(bytes,24,8);best={lease.Info(),std::move(bytes)};}}
    Check(revision!=0,"published commit exists");return best;
}
void RejectRecovery(const fs::path& path,catalog::Snapshot& output,bool crossRootExpected=false){
    const auto before=output;const auto disk=SnapshotDisk(path);catalog::Store store(path);
    Check(store.Read(output)==Result::RecoveryRequired&&Same(output,before),"damaged metadata Read requires recovery, never old revision");
    Check(store.Initialize({},output)==Result::RecoveryRequired&&Same(output,before),"damaged metadata cannot reinitialize");
    const auto commitResult=store.Commit(output,Bytes{1},output);
    Check(commitResult==(crossRootExpected?Result::Conflict:Result::RecoveryRequired)&&Same(output,before),
        crossRootExpected?"cross-root expected is rejected before damaged-store recovery and retains aliased output":"same-root damaged metadata cannot append or mutate aliased output");
    Check(SnapshotDisk(path)==disk,"recovery refusal leaves every metadata byte untouched");
}
void Recovery(const fs::path& root){
    // A correct blob SHA around a bad record distinguishes decoder/chain rejection
    // from the lower-level envelope hash check. No production codec builds it.
    for(unsigned kind=0;kind<4;++kind){
        const auto path=root/(L"recovery-"+std::to_wstring(kind));auto s=Init(path);catalog::Store store(path);
        Check(store.Commit(s,Bytes{7,8},s)==Result::Ok,"prepare two valid committed generations");const auto head=Latest(path);
        if(kind==0){auto d=Open(path/L"commits",false);auto bad=head.bytes;Put(bad,16,2,4);Create(d,Spec(true,NamespaceOwner()),bad);}
        if(kind==1){auto d=Open(path/L"snapshots",false);Check(d.Remove(Ref(head.bytes,48))==files::Result::DeletePending,"remove exact latest referenced snapshot by identity");}
        if(kind>=2){codec::Record r;r.revision=kind==2?3:4;r.owner=s.StoreId();r.snapshot=Ref(head.bytes,48);r.previous=head.info;
            if(kind==2)r.previous.identity.fileIndex^=1;
            auto d=Open(path/L"commits",false);Create(d,Spec(true,NamespaceOwner()),Golden(r));}
        RejectRecovery(path,s);
    }
    {const auto path=root/L"orphan-initialization";NewDirectory(path);NewBytes(path/L".lock",{});{auto d=Open(path/L"snapshots");Create(d,Spec(false,Owner()),Bytes{3});}
        auto sentinel=Init(root/L"orphan-sentinel");RejectRecovery(path,sentinel,true);}
    {const auto path=root/L"allowed-orphan";auto s=Init(path);const auto old=s;{auto d=Open(path/L"snapshots",false);Create(d,Spec(false,s.StoreId()),Bytes{99});}
        Check(catalog::Store(path).Read(s)==Result::Ok&&Same(s,old),"complete unreferenced snapshot does not change published revision");}
    for(unsigned place=0;place<3;++place){
        const auto path=root/(L"foreign-"+std::to_wstring(place));auto s=Init(path);
        const auto foreign=place==0?path/L"foreign.txt":path/(place==1?L"snapshots":L"commits")/L"future-format.dat";
        const Bytes bytes{0x11,0xff,0,0x80};NewBytes(foreign,bytes);RejectRecovery(path,s);Check(ReadBytes(foreign)==bytes,"foreign metadata retained verbatim");
    }
    // Also prove a foreign file cannot make a never-initialized store look empty.
    for(unsigned place=0;place<3;++place){const auto path=root/(L"foreign-empty-"+std::to_wstring(place));NewDirectory(path);NewBytes(path/L".lock",{});
        auto folder=path;if(place){folder/=place==1?L"snapshots":L"commits";NewDirectory(folder);}NewText(folder/L"foreign.txt","foreign\n");
        auto sentinel=Init(root/(L"foreign-output-"+std::to_wstring(place)));RejectRecovery(path,sentinel,true);}
}
void Limits(const fs::path& root){
    {const auto path=root/L"commit-limit";auto s=Init(path,{});catalog::Store store(path);
        for(unsigned revision=2;revision<=64;++revision)Check(store.Commit(s,Bytes{static_cast<std::uint8_t>(revision)},s)==Result::Ok&&s.Revision()==revision,"commit count within fixed 64-record budget");
        const auto before=s;const auto disk=SnapshotDisk(path);Check(store.Commit(s,Bytes{65},s)==Result::Limit&&Same(s,before)&&SnapshotDisk(path)==disk,"65th commit cannot publish");}
    {const auto path=root/L"snapshot-limit";auto s=Init(path,{});{auto d=Open(path/L"snapshots",false);for(unsigned i=1;i<128;++i)Create(d,Spec(false,s.StoreId()),{});}
        const auto before=s;Check(catalog::Store(path).Read(s)==Result::Ok&&Same(s,before),"128 total snapshots including orphans remain readable");
        {auto d=Open(path/L"snapshots",false);Create(d,Spec(false,s.StoreId()),{});}const auto disk=SnapshotDisk(path);
        Check(catalog::Store(path).Read(s)==Result::Limit&&Same(s,before)&&SnapshotDisk(path)==disk,"129 snapshots exceed bounded metadata accounting");}
    {const auto path=root/L"payload-limit";auto s=Init(path);const auto before=s;const auto disk=SnapshotDisk(path);Bytes large(16*1024*1024+1,0);
        Check(catalog::Store(path).Commit(s,large,s)==Result::Limit&&Same(s,before)&&SnapshotDisk(path)==disk,"over-16MiB payload rejected before publication");
        const auto fresh=root/L"oversized-init";catalog::Snapshot out=s;Check(catalog::Store(fresh).Initialize(large,out)==Result::Limit&&Same(out,s),"oversized initialization preserves output");}
}
void ChildChecks(const fs::path& root){
    const auto path=root/L"child-guards";auto parent=Open(path);files::Directory child;
    Check(parent.Child(L"child",true,child)==files::Result::Ok,"open child through pinned parent");
    const Bytes bytes{9,0,0xfe};auto info=Create(child,Spec(false,Owner()),bytes);
    std::vector<std::wstring> bad{L"",L".",L"..",L"a/b",L"a\\b",L"C:\\x",L"x:y",L"NUL",L"trail.",L"trail ",L"bad?",std::wstring(L"a\0b",3)};
    for(const auto& name:bad){Check(parent.Child(name,true,child)==files::Result::Invalid&&child.IsValid(),"invalid Child input retains old directory");files::ReadLease lease;Check(child.Acquire(info,lease)==files::Result::Ok,"failed Child output retains old origin");}
    files::ReadLease lease;Check(child.Acquire(info,lease)==files::Result::Ok,"derive child lease");parent={};
    auto deletionAccess=[&](bool denied){Handle h(CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        const DWORD error=h.value==INVALID_HANDLE_VALUE?GetLastError():0;Check(denied?h.value==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION:h.value!=INVALID_HANDLE_VALUE,"parent lifetime guard checked by nonmutating DELETE access");};
    deletionAccess(true);files::ReadLease again;Check(child.Acquire(info,again)==files::Result::Ok,"child remains usable after parent destruction");again={};child={};
    Bytes out;Check(lease.ReadAll(out)==files::Result::Ok&&out==bytes,"lease survives parent and child destruction");deletionAccess(true);lease={};deletionAccess(false);
}
std::wstring Quote(const fs::path& p){Check(p.native().find(L'"')==std::wstring::npos,"safe own child command argument");return L"\""+p.native()+L"\"";}
void WaitFile(const fs::path& p){const auto start=GetTickCount64();while(!fs::exists(p)){Check(GetTickCount64()-start<15000,"bounded worker barrier wait");Sleep(10);}}
int Worker(const fs::path& root,const fs::path& control,unsigned id){
    Check(root.is_absolute()&&control.is_absolute()&&id<2,"worker owned absolute arguments");
    Check(ReadBytes(control/L"marker")==Bytes({'M','M','S',' ','c','a','t','a','l','o','g',' ','r','a','c','e','\n'}),"worker owner marker");
    catalog::Store store(root);catalog::Snapshot expected;Result result=Result::Busy;
    const auto start=GetTickCount64();while(result==Result::Busy&&GetTickCount64()-start<10000){result=store.Read(expected);if(result==Result::Busy)Sleep(5);}
    Check(result==Result::Ok&&expected.Revision()==1,"worker captures same initial revision before barrier");
    NewText(control/(L"ready-"+std::to_wstring(id)),"ready\n");WaitFile(control/L"go");
    result=Result::Busy;const auto commitStart=GetTickCount64();
    while(result==Result::Busy&&GetTickCount64()-commitStart<10000){result=store.Commit(expected,Bytes{static_cast<std::uint8_t>(0xa0+id)},expected);if(result==Result::Busy)Sleep(5);}
    const int code=result==Result::Ok?20:result==Result::Conflict?21:22;
    NewText(control/(L"result-"+std::to_wstring(id)),std::to_string(static_cast<int>(result))+"\n");return code;
}
struct Process {
    HANDLE handle=nullptr;DWORD id=0;
    ~Process(){if(handle){if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT){TerminateProcess(handle,98);WaitForSingleObject(handle,5000);}CloseHandle(handle);}}
    Process(const Process&)=delete;Process& operator=(const Process&)=delete;Process()=default;
};
void Race(const fs::path& root){
    const auto path=root/L"race-catalog",control=root/L"race-control";auto initial=Init(path);NewDirectory(control);NewText(control/L"marker","MMS catalog race\n");
    std::array<wchar_t,32768> executable{};const DWORD n=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));Check(n&&n<executable.size(),"get own executable for workers");
    std::array<Process,2> processes;
    for(unsigned i=0;i<2;++i){auto command=Quote(executable.data())+L" --worker "+Quote(path)+L" "+Quote(control)+L" "+std::to_wstring(i);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION p{};
        Check(CreateProcessW(executable.data(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,control.c_str(),&startup,&p)!=FALSE,"start only owned hidden self worker");
        CloseHandle(p.hThread);processes[i].handle=p.hProcess;processes[i].id=p.dwProcessId;}
    WaitFile(control/L"ready-0");WaitFile(control/L"ready-1");NewText(control/L"go","go\n");std::array<DWORD,2> codes{};
    for(unsigned i=0;i<2;++i){Check(WaitForSingleObject(processes[i].handle,20000)==WAIT_OBJECT_0,"owned worker completes within bound");Check(GetExitCodeProcess(processes[i].handle,&codes[i])!=FALSE,"retain every worker exit code");}
    NewText(control/L"exit-codes.txt",std::to_string(codes[0])+"\n"+std::to_string(codes[1])+"\n");
    Check((codes[0]==20&&codes[1]==21)||(codes[0]==21&&codes[1]==20),"same-revision race has exactly one success and one conflict");
    catalog::Snapshot final;Check(catalog::Store(path).Read(final)==Result::Ok&&final.Revision()==2&&final.StoreId()==initial.StoreId()&&final.Bytes()==Bytes{static_cast<std::uint8_t>(codes[0]==20?0xa0:0xa1)},"winning worker bytes are the unique revision two");
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        if(argc==5&&std::wcscmp(argv[1],L"--worker")==0){Check(std::wcscmp(argv[4],L"0")==0||std::wcscmp(argv[4],L"1")==0,"worker ID must be exact");return Worker(argv[2],argv[3],argv[4][0]-L'0');}
        Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage: --store NEW_ABSOLUTE_DIR");const fs::path root(argv[2]);NewDirectory(root);
        CodecChecks();ChildChecks(root);Basics(root);Recovery(root);Limits(root);Race(root);
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\n64MiB aggregate boundary and interruption/IAT faults not executed by this suite\n");
        std::printf("PASS cache catalog checks=%u evidence=%ls\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog unexpected exception\n");return 2;}
}
