// MAINTENANCE: regression-keep. Independent wire oracle and owned real files.
// This validates metadata/catalog primitives, not a complete SourceStore.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/io/cache_source_registry.hpp"
#include "../src/io/cache_business_reader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>

namespace allocation {
thread_local long remaining=-1;
thread_local unsigned hits=0;
struct Fail {explicit Fail(long n){remaining=n;hits=0;}~Fail(){remaining=-1;}};
}
void* operator new(std::size_t n){if(allocation::remaining==0){++allocation::hits;throw std::bad_alloc();}
    if(allocation::remaining>0)--allocation::remaining;
    if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace {
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
namespace c=mikumikustudio::cache_catalog;
namespace fs=std::filesystem;
using Bytes=b::Bytes;
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void Put(Bytes& x,std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)x.at(at+i)=std::uint8_t(value>>(i*8));}
template<class A>void Array(Bytes& x,std::size_t at,const A& a){std::copy(a.begin(),a.end(),x.begin()+at);}
void Name(Bytes& x,std::size_t at,const std::wstring& name){for(auto ch:name)x.at(at++)=std::uint8_t(ch);}
std::wstring RawName(unsigned id){constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring n(32,L'0');n[30]=hex[(id>>4)&15];n[31]=hex[id&15];return n+L".raw.mp4";}
template<class A>A Id(unsigned n){A id{};id[15]=std::uint8_t(n);return id;}
f::RawSourceInfo RawInfo(unsigned n){f::RawSourceInfo i;i.name=RawName(n);i.identity={7,100+n,7};i.payloadBytes=7;i.payloadDigest.fill(std::uint8_t(n));return i;}
b::SourceRegistry Authored(){
    b::SourceRegistry r;r.storeOwner=Id<f::Owner>(3);r.maintenanceEpoch=9;
    b::DirectoryBinding d;d.id=1;d.identity={7,88};r.directories.push_back(d);
    for(unsigned n=1;n<=2;++n){b::SourceRecord s;s.id=Id<b::SourceId>(n);s.createdBy=Id<b::OperationId>(10+n);s.directoryId=1;s.file=RawInfo(n);s.lastUse=91+n;
        s.state=n==1?b::SourceState::Published:b::SourceState::Retired;if(n==1)s.owners={1};r.sources.push_back(s);}
    for(unsigned n=1;n<=5;++n){b::SourceOperation o;o.id=Id<b::OperationId>(20+n);o.sourceId=Id<b::SourceId>(30+n);o.directoryId=1;o.reservedBytes=7;o.stage=static_cast<b::SourceStage>(n);
        if(n!=1){o.plan.name=RawName(40+n);o.plan.createdIdentity={7,140+n,0};o.plan.sourceBytes=7;}
        if(n==3||n==5){o.captured=RawInfo(40+n);}
        r.operations.push_back(o);}
    return r;
}
// The oracle uses the published format's literal offsets, never production
// encoder helpers or sizeof(C++ record). It also preserves invalid values.
Bytes Golden(const b::SourceRegistry& r){
    std::size_t size=80+32*r.directories.size();
    for(const auto& s:r.sources)size+=144+s.file.name.size()+8*s.owners.size();
    for(const auto& o:r.operations)size+=176+o.plan.name.size();
    Bytes x(size);const char magic[]="MMSCacheSource01";std::copy_n(magic,16,x.begin());
    Put(x,16,r.maintenanceProtocol,4);Put(x,20,80,4);Array(x,24,r.storeOwner);Put(x,40,r.maintenanceEpoch,8);
    Put(x,48,r.directories.size(),4);Put(x,52,r.sources.size(),4);Put(x,56,r.operations.size(),4);Put(x,64,size,8);
    std::size_t at=80;
    for(const auto& d:r.directories){Put(x,at,32,4);Put(x,at+4,std::uint32_t(d.area),4);Put(x,at+8,d.id,8);Put(x,at+16,d.identity.volume,4);Put(x,at+24,d.identity.fileIndex,8);at+=32;}
    for(const auto& s:r.sources){const auto bytes=144+s.file.name.size()+8*s.owners.size();Put(x,at,bytes,4);Put(x,at+4,std::uint32_t(s.state),4);
        Array(x,at+8,s.id);Array(x,at+24,s.createdBy);Put(x,at+40,s.directoryId,8);Put(x,at+48,s.lastUse,8);Put(x,at+56,s.owners.size(),4);Put(x,at+60,s.file.name.size(),4);
        Put(x,at+64,s.file.identity.volume,4);Put(x,at+72,s.file.identity.fileIndex,8);Put(x,at+80,s.file.identity.fileBytes,8);Put(x,at+88,s.file.payloadBytes,8);Array(x,at+96,s.file.payloadDigest);
        Name(x,at+144,s.file.name);std::size_t p=at+144+s.file.name.size();for(auto owner:s.owners){Put(x,p,owner,8);p+=8;}at+=bytes;}
    for(const auto& o:r.operations){const auto bytes=176+o.plan.name.size();Put(x,at,bytes,4);Put(x,at+4,std::uint32_t(o.stage),4);Array(x,at+8,o.id);Array(x,at+24,o.sourceId);
        Put(x,at+40,o.directoryId,8);Put(x,at+48,o.reservedBytes,8);Put(x,at+56,o.plan.name.size(),4);Put(x,at+64,o.plan.createdIdentity.volume,4);
        Put(x,at+72,o.plan.createdIdentity.fileIndex,8);Put(x,at+80,o.plan.createdIdentity.fileBytes,8);Put(x,at+88,o.plan.sourceBytes,8);
        Put(x,at+96,o.captured.identity.volume,4);Put(x,at+104,o.captured.identity.fileIndex,8);Put(x,at+112,o.captured.identity.fileBytes,8);Put(x,at+120,o.captured.payloadBytes,8);Array(x,at+128,o.captured.payloadDigest);
        Name(x,at+176,o.plan.name);at+=bytes;}
    return x;
}
Bytes GoldenManifest(const b::Manifest& m){
    Bytes x(96+176*m.pages.size());const char magic[]="MMSCacheIndex001";std::copy_n(magic,16,x.begin());Put(x,16,1,4);Put(x,20,96,4);Array(x,24,m.storeOwner);
    Put(x,40,m.maintenanceEpoch,8);Put(x,48,m.nextProjectId,8);Put(x,56,m.projectCount,4);Put(x,60,m.entryCount,4);Put(x,64,m.currentCount,4);Put(x,68,m.pages.size(),4);Put(x,72,m.liveEncodedBytes,8);
    for(std::size_t i=0;i<m.pages.size();++i){const auto& p=m.pages[i];const auto at=96+i*176;Put(x,at,std::uint32_t(p.kind),4);Put(x,at+4,p.ordinal,4);Put(x,at+8,p.recordCount,4);
        const auto q=at+16;Name(x,q,p.blob.name);Put(x,q+40,std::uint32_t(p.blob.spec.kind),4);Array(x,q+48,p.blob.spec.owner);Array(x,q+64,p.blob.spec.key);Array(x,q+96,p.blob.payloadDigest);
        Put(x,q+128,p.blob.payloadBytes,8);Put(x,q+136,p.blob.identity.volume,4);Put(x,q+144,p.blob.identity.fileIndex,8);Put(x,q+152,p.blob.identity.fileBytes,8);}
    return x;
}
Bytes Golden(const b::BusinessRoot& r){auto m=GoldenManifest(r.legacy),s=Golden(r.sources);Bytes x(80+m.size()+s.size());const char magic[]="MMSCacheBusRoot1";std::copy_n(magic,16,x.begin());
    Put(x,16,1,4);Put(x,20,80,4);Array(x,24,r.legacy.storeOwner);Put(x,40,r.legacy.maintenanceEpoch,8);Put(x,48,m.size(),8);Put(x,56,s.size(),8);Put(x,64,x.size(),8);
    std::copy(m.begin(),m.end(),x.begin()+80);std::copy(s.begin(),s.end(),x.begin()+80+m.size());return x;}
b::BusinessRoot Root(){b::BusinessRoot r;r.sources=Authored();r.legacy.storeOwner=r.sources.storeOwner;r.legacy.maintenanceEpoch=r.sources.maintenanceEpoch;r.legacy.nextProjectId=2;r.legacy.liveEncodedBytes=96;return r;}
bool Same(const b::SourceRegistry& a,const b::SourceRegistry& b){
    // Captured name has no independent wire field: compare it explicitly too.
    if(Golden(a)!=Golden(b)||a.operations.size()!=b.operations.size())return false;
    for(std::size_t i=0;i<a.operations.size();++i)if(a.operations[i].captured.name!=b.operations[i].captured.name)return false;return true;}
void Rejected(const b::SourceRegistry& bad,b::Result expected=b::Result::Invalid){auto output=Authored();const auto before=output;Bytes bytes{0x9a,0x66};
    Check(b::ValidateSources(bad)==expected,"invalid registry rejected by structural validator");
    Check(b::EncodeSources(bad,bytes)==expected&&bytes==Bytes({0x9a,0x66}),"invalid encode preserves byte output");
    const auto wire=Golden(bad);Check(b::DecodeSources(wire.data(),wire.size(),output)==expected&&Same(output,before),"invalid independent wire rejected without changing output");}
void Codec(){
    const auto r=Authored();Check(b::ValidateSources(r)==b::Result::Ok,"all five stages and both source states are valid");Bytes x;
    Check(b::EncodeSources(r,x)==b::Result::Ok&&x==Golden(r),"source registry equals independent complete golden wire");
    b::SourceRegistry decoded;Check(b::DecodeSources(x.data(),x.size(),decoded)==b::Result::Ok&&Same(r,decoded),"decode independently authored source fields");
    const auto root=Root();Bytes composite;Check(b::EncodeBusinessRoot(root,composite)==b::Result::Ok&&composite==Golden(root),"composite root independently includes exact legacy bytes");
    b::BusinessRoot decodedRoot;Check(b::DecodeBusinessRoot(composite.data(),composite.size(),decodedRoot)==b::Result::Ok&&Golden(decodedRoot)==composite,"decode composite complete fields");
    const auto legacy=GoldenManifest(root.legacy);Check(b::DecodeBusinessRoot(legacy.data(),legacy.size(),decodedRoot)==b::Result::Ok&&decodedRoot.sources.sources.empty()&&decodedRoot.sources.operations.empty()&&decodedRoot.sources.directories.empty()&&decodedRoot.sources.storeOwner==root.legacy.storeOwner&&decodedRoot.sources.maintenanceEpoch==9,"legacy v1 manifest accepted with matching empty sources");
    Check(r.maintenanceProtocol==1&&decodedRoot.sources.maintenanceProtocol==1&&x[16]==1&&x[17]==0&&x[18]==0&&x[19]==0,
        "default and legacy manifest retain protocol one and exact original version bytes");
    auto modern=r;modern.maintenanceProtocol=2;Bytes modernWire;
    auto expectedModern=x;Put(expectedModern,16,2,4);
    Check(b::ValidateSources(modern)==b::Result::Ok&&b::EncodeSources(modern,modernWire)==b::Result::Ok&&modernWire==expectedModern,
        "protocol two changes only the registry version field, preserving every v1 record byte");
    Check(b::DecodeSources(modernWire.data(),modernWire.size(),decoded)==b::Result::Ok&&Same(modern,decoded)&&decoded.maintenanceProtocol==2,
        "protocol two survives decode without silently granting or downgrading credit");
    auto modernRoot=root;modernRoot.sources=modern;Bytes modernComposite;
    Check(b::EncodeBusinessRoot(modernRoot,modernComposite)==b::Result::Ok&&modernComposite==Golden(modernRoot)&&modernComposite[16]==1&&
        b::DecodeBusinessRoot(modernComposite.data(),modernComposite.size(),decodedRoot)==b::Result::Ok&&decodedRoot.sources.maintenanceProtocol==2,
        "composite root stays version one while its embedded registry persists protocol two");
    Check(b::DecodeBusinessRoot(legacy.data(),legacy.size(),decodedRoot)==b::Result::Ok&&decodedRoot.sources.maintenanceProtocol==1,
        "legacy manifest replaces a previously modern output with explicit legacy protocol");
    for(std::uint32_t protocol:{0u,3u,UINT32_MAX}){
        auto unsupported=r;unsupported.maintenanceProtocol=protocol;Rejected(unsupported,b::Result::Unsupported);
        auto wire=modernComposite;Put(wire,80+legacy.size()+16,protocol,4);auto output=modernRoot;
        Check(b::DecodeBusinessRoot(wire.data(),wire.size(),output)==b::Result::Unsupported&&Golden(output)==Golden(modernRoot),
            "unknown nested maintenance protocol preserves the whole caller-owned modern root");
    }
    for(std::size_t n=0;n<modernWire.size();++n){auto output=modern;Check(b::DecodeSources(modernWire.data(),n,output)!=b::Result::Ok&&Same(output,modern),
        "every protocol-two truncation preserves output including the maintenance protocol");}
    for(std::size_t n=0;n<x.size();++n){auto out=r;Check(b::DecodeSources(x.data(),n,out)!=b::Result::Ok&&Same(out,r),"every source-wire truncation preserves output");}
    for(std::size_t n=0;n<composite.size();++n){auto out=root;Check(b::DecodeBusinessRoot(composite.data(),n,out)!=b::Result::Ok&&Golden(out)==Golden(root),"every composite truncation preserves output");}
    auto Corrupt=[&](std::size_t offset,std::uint8_t value,b::Result expected=b::Result::Invalid){auto bytes=x;bytes[offset]=value;auto out=r;
        Check(b::DecodeSources(bytes.data(),bytes.size(),out)==expected&&Same(out,r),"wire corruption rejected with output unchanged");};
    Corrupt(16,3,b::Result::Unsupported);Corrupt(20,79);Corrupt(60,1);Corrupt(72,1);Corrupt(80+20,1);Corrupt(112+68,1);Corrupt(112+128,1);
    Corrupt(84,2,b::Result::Unsupported);Corrupt(116,3,b::Result::Unsupported);
    const auto op=112+144+r.sources[0].file.name.size()+8+144+r.sources[1].file.name.size();
    Corrupt(op+4,6,b::Result::Unsupported);Corrupt(op+60,1);Corrupt(op+68,1);Corrupt(op+100,1);Corrupt(op+160,1);
    {auto bytes=x;bytes.push_back(0);auto out=r;Check(b::DecodeSources(bytes.data(),bytes.size(),out)==b::Result::Invalid&&Same(out,r),"trailing registry data rejected");}
    for(auto offset:{16u,72u,80u+16u}){auto bytes=composite;bytes[offset]=offset==72?1:2;auto out=root;const auto expected=offset==72?b::Result::Invalid:b::Result::Unsupported;
        Check(b::DecodeBusinessRoot(bytes.data(),bytes.size(),out)==expected&&Golden(out)==Golden(root),"root header/nested version rejects without output publication");}
    auto bad=r;bad.storeOwner={};Rejected(bad);bad=r;bad.maintenanceEpoch=0;Rejected(bad);bad=r;bad.sources[0].directoryId=2;Rejected(bad);
    bad=r;bad.sources[1].id=bad.sources[0].id;Rejected(bad);bad=r;bad.sources[1].createdBy=bad.sources[0].createdBy;Rejected(bad);
    bad=r;bad.operations[0].sourceId=bad.sources[0].id;Rejected(bad);bad=r;bad.operations[0].id=bad.sources[0].createdBy;Rejected(bad);
    bad=r;bad.sources[1].file.name=bad.sources[0].file.name;Rejected(bad);bad=r;bad.sources[1].file.identity=bad.sources[0].file.identity;Rejected(bad);
    bad=r;std::swap(bad.sources[0],bad.sources[1]);Rejected(bad);bad=r;std::swap(bad.operations[0],bad.operations[1]);Rejected(bad);
    bad=r;bad.sources[0].owners={1,1};Rejected(bad);bad=r;bad.sources[0].file.payloadBytes=8;Rejected(bad);
    bad=r;bad.sources[0].file.name[0]=L'A';Rejected(bad);bad=r;bad.sources[0].file.name[0]=L'\u0100';Rejected(bad);
    bad=r;bad.operations[0].plan.sourceBytes=1;Rejected(bad);bad=r;bad.operations[1].captured.payloadBytes=1;Rejected(bad);
    bad=r;bad.operations[2].captured.identity.fileIndex++;Rejected(bad);bad=r;bad.operations[2].reservedBytes=6;Rejected(bad);
    bad=r;bad.operations[1].plan.name=bad.sources[0].file.name;Rejected(bad);
    bad=r;bad.operations[1].plan.createdIdentity.fileIndex=bad.sources[0].file.identity.fileIndex;Rejected(bad);
    bad=r;bad.sources[0].file.identity.fileIndex=bad.directories[0].identity.fileIndex;Rejected(bad);
    bad=r;bad.operations[1].plan.createdIdentity.fileBytes=1;Rejected(bad);
    for(auto stage:{0u,1u,3u}){bad=r;bad.operations[stage].captured.payloadDigest[3]=1;Rejected(bad);
        bad=r;bad.operations[stage].captured.identity.volume=1;Rejected(bad);}
    bad=r;bad.operations.resize(2);bad.operations[1].stage=b::SourceStage::Reserved;bad.operations[1].plan={};
    for(auto& operation:bad.operations)operation.reservedBytes=std::uint64_t(INT64_MAX);
    Rejected(bad,b::Result::Limit);
    {auto allowed=r;allowed.sources[1].owners={1};allowed.sources[0].file.payloadDigest={};
        Check(b::ValidateSources(allowed)==b::Result::Ok,"retirement retains owner responsibility and zero digest is metadata, not file proof");}
    bad=r;bad.operations[2].captured.name=RawName(99);Bytes out{1};Check(b::ValidateSources(bad)==b::Result::Invalid&&b::EncodeSources(bad,out)==b::Result::Invalid&&out==Bytes({1}),"captured alternate name cannot be silently normalized by encoding");
    bad=r;bad.directories.push_back(bad.directories[0]);bad.directories.back().id=2;Rejected(bad);
    bad=r;bad.sources.resize(b::MaximumSources+1);Check(b::ValidateSources(bad)==b::Result::Limit,"source count hard limit precedes iteration");
    bad=r;bad.operations.resize(b::MaximumSourceOperations+1);Check(b::ValidateSources(bad)==b::Result::Limit,"operation count hard limit");
    bad=r;bad.directories.resize(b::MaximumSourceDirectories+1);Check(b::ValidateSources(bad)==b::Result::Limit,"directory count hard limit");
    bad=r;bad.sources[0].owners.resize(b::MaximumOwnersPerEntry+1);Check(b::ValidateSources(bad)==b::Result::Limit,"owner list count hard limit");
    {Bytes huge(b::MaximumSourceRegistryBytes+1);auto d=r;Check(b::DecodeSources(huge.data(),huge.size(),d)==b::Result::Limit&&Same(d,r),"registry encoded byte cap checked before parsing");}
    {Bytes huge(b::MaximumBusinessRootBytes+1);auto d=root;Check(b::DecodeBusinessRoot(huge.data(),huge.size(),d)==b::Result::Limit&&Golden(d)==Golden(root),"root encoded byte cap checked before parsing");}
    b::Index index;index.storeOwner=r.storeOwner;index.maintenanceEpoch=r.maintenanceEpoch;index.nextProjectId=2;
    b::Project p;p.id=1;p.normalizedPath=p.displayPath=u"C:\\source-codec\\one.pmm";index.projects.push_back(p);
    Check(b::ValidateSourcesAgainstIndex(r,index)==b::Result::Ok,"owner references actual assembled project");
    bad=r;bad.sources[0].owners={2};Check(b::ValidateSourcesAgainstIndex(bad,index)==b::Result::Invalid,"nonexistent project reference rejected");
    bad=r;bad.storeOwner[0]=1;Check(b::ValidateSourcesAgainstIndex(bad,index)==b::Result::Invalid,"store mismatch rejected");
    bad=r;bad.maintenanceEpoch++;Check(b::ValidateSourcesAgainstIndex(bad,index)==b::Result::Invalid,"epoch mismatch rejected");
    {auto collision=index;b::Entry entry;entry.blob.name=std::wstring(32,L'1')+L".blob";entry.blob.spec.owner=r.storeOwner;entry.blob.spec.kind=f::Kind::Effect;entry.blob.spec.key[0]=1;
        entry.blob.payloadBytes=7;entry.blob.payloadDigest[0]=1;entry.blob.identity=r.sources[0].file.identity;entry.blob.identity.fileBytes=135;entry.consumerFormat=1;entry.owners={1};collision.entries.push_back(entry);
        Check(b::Validate(collision)==b::Result::Ok&&b::ValidateSourcesAgainstIndex(r,collision)==b::Result::Invalid,"raw identity cannot alias an otherwise valid legacy payload entry");}
    auto br=root;br.sources.storeOwner[0]=1;Check(b::ValidateBusinessRoot(br)==b::Result::Invalid,"root cannot mix store owners");br=root;br.sources.maintenanceEpoch++;Check(b::ValidateBusinessRoot(br)==b::Result::Invalid,"root cannot mix maintenance epochs");
}
void Allocation(){
    const auto root=Root();const auto wire=Golden(root);const auto registryWire=Golden(root.sources);
    for(unsigned operation=0;operation<4;++operation){bool succeeded=false;unsigned refusals=0;
        for(long n=0;n<1024;++n){Bytes bytes{0xab,0xcd};auto registry=root.sources;auto output=root;b::Result result;unsigned hits;
            {allocation::Fail fault(n);switch(operation){case 0:result=b::EncodeSources(root.sources,bytes);break;case 1:result=b::DecodeSources(registryWire.data(),registryWire.size(),registry);break;
                case 2:result=b::EncodeBusinessRoot(root,bytes);break;default:result=b::DecodeBusinessRoot(wire.data(),wire.size(),output);break;}hits=allocation::hits;}
            if(result==b::Result::Ok){Check(hits==0,"successful codec did not swallow injected allocation failure");succeeded=true;break;}
            Check(hits>0&&result==b::Result::AllocationFailure,"real codec allocation failure reported precisely");++refusals;
            Check(bytes==Bytes({0xab,0xcd})&&Same(registry,root.sources)&&Golden(output)==wire,"every allocation refusal preserves complete output");}
        Check(succeeded&&refusals>0,"allocation sweep reaches refusal and ordinary successful codec path");}
}
struct Handle {HANDLE h;explicit Handle(HANDLE value):h(value){}~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;};
void NewDirectory(const fs::path& path){Check(path.is_absolute()&&path==path.lexically_normal()&&path!=path.root_path(),"normalized absolute owned test root");Check(CreateDirectoryW(path.c_str(),nullptr)!=FALSE,"claim brand-new test directory");}
void Write(const fs::path& path,const Bytes& bytes){Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Check(file.h!=INVALID_HANDLE_VALUE,"create owned source only");DWORD done=0;
    Check(WriteFile(file.h,bytes.data(),DWORD(bytes.size()),&done,nullptr)&&done==bytes.size()&&FlushFileBuffers(file.h),"write exact source bytes");}
Bytes Read(const std::wstring& path){std::ifstream file(fs::path(path),std::ios::binary);Check(bool(file),"read actual leased raw path");return Bytes(std::istreambuf_iterator<char>(file),{});}
f::RawSourceWrite Prepare(const f::Directory& dir,const fs::path& path,std::uint64_t size){f::RawSourceWrite write;f::RawSourceCleanup cleanup;
    Check(dir.PrepareRawSource(path.c_str(),size,write,cleanup)==f::Result::Ok&&write.IsValid()&&!cleanup.IsValid(),"prepare owned raw source with tracked cleanup");return write;}
void Disk(const fs::path& base){
    f::Directory raw,pages;Check(f::Directory::Open(base/L"raw",true,raw)==f::Result::Ok&&f::Directory::Open(base/L"pages",true,pages)==f::Result::Ok,"open actual pinned raw/page directories");
    f::DirectoryIdentity identity;Check(raw.GetIdentity(identity)==f::Result::Ok,"bind source registry to actual directory identity");
    const Bytes payload{0,0xff,1,2,0,7,0x80};const auto source=base/L"source.mp4";Write(source,payload);
    auto write=Prepare(raw,source,payload.size());f::RawSourceLease lease;Check(write.Complete(lease)==f::Result::Ok&&!write.IsValid(),"capture actual immutable raw payload");const auto info=lease.Info();lease={};
    Check(DeleteFileW(source.c_str())!=FALSE,"delete only owned original source after capture");
    auto root=Root();root.sources.sources.resize(1);root.sources.operations.clear();root.sources.sources[0].file=info;root.sources.directories[0].identity=identity;
    b::Page page;page.storeOwner=root.legacy.storeOwner;page.kind=b::PageKind::Projects;b::Project project;project.id=1;project.normalizedPath=project.displayPath=u"C:\\source-registry\\one.pmm";page.projects.push_back(project);
    Bytes pageBytes;Check(b::EncodePage(page,pageBytes)==b::Result::Ok,"encode real project page");b::PageRef ref;ref.kind=b::PageKind::Projects;ref.recordCount=1;
    f::Spec spec;spec.kind=f::Kind::IndexPage;spec.owner=page.storeOwner;spec.key=b::PageKey;
    Check(pages.Create(spec,pageBytes.data(),pageBytes.size(),ref.blob)==f::Result::Ok,"publish real checksummed page");
    root.legacy.pages.push_back(ref);root.legacy.projectCount=1;root.legacy.liveEncodedBytes=96+176+pageBytes.size();
    {auto collision=root;collision.sources.sources[0].file.identity.volume=ref.blob.identity.volume;collision.sources.sources[0].file.identity.fileIndex=ref.blob.identity.fileIndex;
        Check(b::ValidateSources(collision.sources)==b::Result::Ok&&b::ValidateBusinessRoot(collision)==b::Result::Invalid,"raw identity cannot alias actual manifest index page");}
    Bytes encoded;Check(b::EncodeBusinessRoot(root,encoded)==b::Result::Ok&&encoded==Golden(root),"actual page references obey independent root golden");
    c::Store catalog(base/L"catalog");c::Snapshot first;Check(catalog.InitializeOwned(root.legacy.storeOwner,encoded,first)==c::Result::Ok,"real atomic catalog initialization");
    c::Store reopened(base/L"catalog");c::Snapshot current;Check(reopened.Read(current)==c::Result::Ok&&current.Bytes()==encoded,"reopen actual published catalog");
    b::BusinessRoot decoded;Check(b::DecodeBusinessRoot(current.Bytes().data(),current.Bytes().size(),decoded)==b::Result::Ok,"decode actual persisted registry");
    Check(raw.AcquireRawSource(decoded.sources.sources[0].file,lease)==f::Result::Ok&&Read(lease.Path())==payload,"decoded physical identity/digest reacquires exact bytes after original deletion");lease={};
    b::PublishedIndex loaded;Check(b::LoadPublishedIndex(current,pages,loaded)==b::ReadResult::Ok&&Same(loaded.Sources(),root.sources)&&loaded.Value().projects.size()==1,"real composite reader joins source owners to project pages");
    {bool success=false;unsigned failures=0;for(long n=0;n<1024;++n){b::ReadResult result;unsigned hits;
        {allocation::Fail fault(n);result=b::LoadPublishedIndex(current,pages,loaded);hits=allocation::hits;}
        if(result==b::ReadResult::Ok){Check(hits==0,"successful real reader does not conceal allocation refusal");success=true;break;}
        // File primitives deliberately map their private allocation failures to
        // Io; codec/reader allocations retain the more precise public result.
        Check(hits>0&&(result==b::ReadResult::AllocationFailure||result==b::ReadResult::Io),"actual reader allocation failure propagates through documented layer");++failures;
        Check(loaded.Source().Bytes()==current.Bytes()&&loaded.Source().Revision()==current.Revision()&&Same(loaded.Sources(),root.sources)&&loaded.Value().projects.size()==1&&loaded.Value().projects[0].id==1,"real reader OOM preserves previous complete publication");
        Check(pages.Remove(ref.blob)==f::Result::Busy,"real reader OOM retains existing physical page lease");}
        Check(success&&failures>0,"real composite reader allocation sweep reaches both refusal and success");}
    auto invalid=root;invalid.sources.sources[0].owners={2};Bytes bad;Check(b::EncodeBusinessRoot(invalid,bad)==b::Result::Ok,"local metadata alone cannot verify page owner membership");
    c::Snapshot invalidHead;Check(catalog.Commit(current,bad,invalidHead)==c::Result::Ok,"commit structurally valid but cross-page invalid reference");
    const auto priorBytes=loaded.Source().Bytes();Check(b::LoadPublishedIndex(invalidHead,pages,loaded)==b::ReadResult::RecoveryRequired,"real reader rejects missing source owner project");
    Check(loaded.Source().Bytes()==priorBytes&&loaded.Source().Revision()==current.Revision()&&Same(loaded.Sources(),root.sources)&&loaded.Value().projects[0].id==1,"failed reader preserves prior values and CAS snapshot");
    Check(pages.Remove(ref.blob)==f::Result::Busy,"failed reader retains old real page lease");
    root.sources.sources[0].state=b::SourceState::Retired;root.sources.sources[0].owners.clear();Check(b::EncodeBusinessRoot(root,encoded)==b::Result::Ok,"encode retired source metadata");
    c::Snapshot retired;Check(catalog.Commit(invalidHead,encoded,retired)==c::Result::Ok,"commit retired state to real CAS");c::Snapshot untouched=retired;
    Check(catalog.Commit(current,encoded,untouched)==c::Result::Conflict&&untouched.Revision()==retired.Revision()&&untouched.Bytes()==retired.Bytes(),"stale catalog CAS preserves output and head");
    Check(b::LoadPublishedIndex(retired,pages,loaded)==b::ReadResult::Ok&&loaded.Sources().sources[0].state==b::SourceState::Retired,"reader exposes exact retired metadata");
    Check(raw.AcquireRawSource(loaded.Sources().sources[0].file,lease)==f::Result::Ok&&Read(lease.Path())==payload,"retired metadata alone does not delete payload or grant eviction");lease={};
    // Prepared interruption: close the real writer, destroy its source, then
    // reconstruct cleanup only from a newly read trusted catalog publication.
    const auto pendingSource=base/L"interrupted.mov";Write(pendingSource,payload);auto pending=Prepare(raw,pendingSource,payload.size());const auto plan=pending.Plan();
    b::SourceOperation operation;operation.id=Id<b::OperationId>(91);operation.sourceId=Id<b::SourceId>(92);operation.directoryId=1;operation.reservedBytes=payload.size();operation.stage=b::SourceStage::Prepared;operation.plan=plan;root.sources.operations={operation};
    Check(b::EncodeBusinessRoot(root,encoded)==b::Result::Ok,"encode prepared intent with exact zero-file identity");c::Snapshot prepared;Check(catalog.Commit(retired,encoded,prepared)==c::Result::Ok,"persist prepared intent before interruption");
    pending={};Check(DeleteFileW(pendingSource.c_str())!=FALSE,"interruption closes source pin without completing copy");
    c::Snapshot readPrepared;Check(reopened.Read(readPrepared)==c::Result::Ok&&b::DecodeBusinessRoot(readPrepared.Bytes().data(),readPrepared.Bytes().size(),decoded)==b::Result::Ok&&decoded.sources.operations[0].stage==b::SourceStage::Prepared,"fresh catalog reopen recovers prepared metadata");
    decoded.sources.operations[0].stage=b::SourceStage::AbortPrepared;Check(b::EncodeBusinessRoot(decoded,encoded)==b::Result::Ok,"encode explicit recovery abort intent");c::Snapshot aborting;Check(catalog.Commit(readPrepared,encoded,aborting)==c::Result::Ok,"publish abort intent through same real catalog CAS");
    c::Snapshot readAbort;Check(reopened.Read(readAbort)==c::Result::Ok&&b::DecodeBusinessRoot(readAbort.Bytes().data(),readAbort.Bytes().size(),decoded)==b::Result::Ok&&decoded.sources.operations[0].stage==b::SourceStage::AbortPrepared,"fresh read confirms exact abort stage");
    f::RawSourceCleanup cleanup;Check(raw.RecoverRawSourceCleanup(decoded.sources.directories[0].identity,decoded.sources.operations[0].plan,cleanup)==f::Result::Ok&&cleanup.IsValid(),"trusted catalog plan recovers exact interrupted file without original source");
    Check(cleanup.Abort()==f::Result::DeletePending&&!cleanup.IsValid()&&GetFileAttributesW((base/L"raw"/plan.name).c_str())==INVALID_FILE_ATTRIBUTES,"same-handle recovered abort removes only recorded zero file");
    decoded.sources.operations.clear();Check(b::EncodeBusinessRoot(decoded,encoded)==b::Result::Ok,"clear completed abort receipt metadata");c::Snapshot final;Check(catalog.Commit(readAbort,encoded,final)==c::Result::Ok,"retire abort intent after actual removal");
    Check(raw.RemoveRawSource(info)==f::Result::DeletePending,"explicit test cleanup removes owned retired source after all read leases close");
    loaded={};Check(pages.Remove(ref.blob)==f::Result::DeletePending,"explicit reader release relinquishes actual page lease");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc!=3||std::wstring(argv[1])!=L"--store")throw std::runtime_error("usage: cache_source_registry_tests --store NEW_ABS_DIR");
    const fs::path root(argv[2]);NewDirectory(root);Codec();Allocation();Disk(root);
    std::printf("PASS cache source registry checks=%u\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL cache source registry after %u checks: %s\n",checks,e.what());return 1;}}
