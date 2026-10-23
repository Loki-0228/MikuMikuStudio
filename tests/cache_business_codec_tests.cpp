// MAINTENANCE: regression-keep. Pure CPU/memory codec checks; no file identities are verified.
#include "../src/io/cache_business_codec.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
using Bytes=b::Bytes;
using Result=b::Result;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
bool Same(const f::BlobInfo& a,const f::BlobInfo& z){return a.name==z.name&&a.spec.owner==z.spec.owner&&a.spec.kind==z.spec.kind&&a.spec.key==z.spec.key&&a.payloadDigest==z.payloadDigest&&a.payloadBytes==z.payloadBytes&&a.identity.volume==z.identity.volume&&a.identity.fileIndex==z.identity.fileIndex&&a.identity.fileBytes==z.identity.fileBytes;}
bool Same(const b::LogicalKey& a,const b::LogicalKey& z){return a.kind==z.kind&&a.consumerFormat==z.consumerFormat&&a.key==z.key;}
bool Same(const b::Project& a,const b::Project& z){return a.id==z.id&&a.normalizedPath==z.normalizedPath&&a.displayPath==z.displayPath&&a.lastOpened==z.lastOpened&&a.pinned==z.pinned;}
bool Same(const b::Entry& a,const b::Entry& z){return Same(a.blob,z.blob)&&a.consumerFormat==z.consumerFormat&&a.lastUse==z.lastUse&&a.availability==z.availability&&a.owners==z.owners;}
bool Same(const b::Current& a,const b::Current& z){return Same(a.key,z.key)&&a.generation==z.generation;}
bool Same(const b::Page&,const b::Page&);
bool Same(const b::PageRef&,const b::PageRef&);
template<class T>bool SameVector(const std::vector<T>& a,const std::vector<T>& z){if(a.size()!=z.size())return false;for(std::size_t i=0;i<a.size();++i)if(!Same(a[i],z[i]))return false;return true;}
bool Same(const b::Page& a,const b::Page& z){return a.storeOwner==z.storeOwner&&a.kind==z.kind&&a.ordinal==z.ordinal&&SameVector(a.projects,z.projects)&&SameVector(a.entries,z.entries)&&SameVector(a.current,z.current);}
bool Same(const b::Index& a,const b::Index& z){return a.storeOwner==z.storeOwner&&a.maintenanceEpoch==z.maintenanceEpoch&&a.nextProjectId==z.nextProjectId&&SameVector(a.projects,z.projects)&&SameVector(a.entries,z.entries)&&SameVector(a.current,z.current);}
bool Same(const b::PageRef& a,const b::PageRef& z){return a.kind==z.kind&&a.ordinal==z.ordinal&&a.recordCount==z.recordCount&&Same(a.blob,z.blob);}
bool Same(const b::Manifest& a,const b::Manifest& z){return a.storeOwner==z.storeOwner&&a.maintenanceEpoch==z.maintenanceEpoch&&a.nextProjectId==z.nextProjectId&&a.projectCount==z.projectCount&&a.entryCount==z.entryCount&&a.currentCount==z.currentCount&&a.liveEncodedBytes==z.liveEncodedBytes&&SameVector(a.pages,z.pages);}
f::Owner Owner(){f::Owner a{};for(unsigned i=0;i<16;++i)a[i]=static_cast<std::uint8_t>(0x51+i);return a;}
std::wstring Name(std::uint32_t n){const wchar_t hex[]=L"0123456789abcdef";std::wstring name(32,L'0');for(unsigned i=0;i<8;++i){name[31-i]=hex[n&15];n>>=4;}return name+L".blob";}
b::GenerationId Generation(std::uint32_t n){b::GenerationId id{};for(unsigned i=0;i<4;++i)id[15-i]=static_cast<std::uint8_t>(n>>(8*i));return id;}
f::Digest Key(std::uint32_t n){f::Digest key{};for(unsigned i=0;i<4;++i)key[i]=static_cast<std::uint8_t>(n>>(24-8*i));return key;}
b::Entry Entry(std::uint32_t n,f::Kind kind=f::Kind::Effect){
    b::Entry e;e.blob.name=Name(n);e.blob.spec.owner=Owner();e.blob.spec.kind=kind;e.blob.spec.key=Key(n);
    e.blob.payloadBytes=17+n%31;e.blob.identity={0x10203040,0x1234567800000000ull+n,128+e.blob.payloadBytes};
    for(unsigned i=0;i<32;++i)e.blob.payloadDigest[i]=static_cast<std::uint8_t>(n+i);
    e.consumerFormat=7;e.lastUse=0x0102030405060708ull+n;return e;
}
b::Current Current(const b::Entry& e,std::uint32_t n){b::Current c;c.key={e.blob.spec.kind,e.consumerFormat,e.blob.spec.key};c.generation=Generation(n);return c;}
b::Index Small(){
    b::Index i;i.storeOwner=Owner();i.maintenanceEpoch=0x0102030405060708ull;i.nextProjectId=10;
    b::Project dos;dos.id=2;dos.normalizedPath=u"C:\\Models\\";dos.normalizedPath+=char16_t(0xd800);dos.normalizedPath+=u".pmm";
    dos.displayPath=u"c:\\models\\";dos.displayPath+=char16_t(0xd800);dos.displayPath+=u".PMM";dos.lastOpened=0x8877665544332211ull;dos.pinned=true;
    b::Project unc;unc.id=9;unc.normalizedPath=u"\\\\server\\share\\Scene.pmm";unc.displayPath=u"\\\\SERVER\\SHARE\\scene.PMM";unc.lastOpened=0;
    i.projects={dos,unc};auto a=Entry(1);a.owners={2,9};auto retired=Entry(2);retired.availability=b::Availability::Retired;retired.owners={2};
    auto texture=Entry(1,f::Kind::Texture);texture.consumerFormat=0xffffffffu;texture.owners={9};i.entries={a,retired,texture};i.current={Current(a,1),Current(texture,1)};return i;
}
void Put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));}
void PutString(Bytes& bytes,std::size_t& at,const std::u16string& text){for(char16_t c:text){Put(bytes,at,c,2);at+=2;}}
void Blob(Bytes& bytes,std::size_t at,const f::BlobInfo& v){
    for(unsigned i=0;i<37;++i)bytes.at(at+i)=static_cast<std::uint8_t>(v.name.at(i));Put(bytes,at+40,static_cast<std::uint32_t>(v.spec.kind),4);
    std::copy(v.spec.owner.begin(),v.spec.owner.end(),bytes.begin()+at+48);std::copy(v.spec.key.begin(),v.spec.key.end(),bytes.begin()+at+64);std::copy(v.payloadDigest.begin(),v.payloadDigest.end(),bytes.begin()+at+96);
    Put(bytes,at+128,v.payloadBytes,8);Put(bytes,at+136,v.identity.volume,4);Put(bytes,at+144,v.identity.fileIndex,8);Put(bytes,at+152,v.identity.fileBytes,8);
}
std::uint64_t RecordSize(const b::Project& p){return 40+2ull*(p.normalizedPath.size()+p.displayPath.size());}
std::uint64_t RecordSize(const b::Entry& e){return 192+8ull*e.owners.size();}
std::uint64_t RecordSize(const b::Current&){return 64;}
std::size_t Count(const b::Page& p){return p.kind==b::PageKind::Projects?p.projects.size():p.kind==b::PageKind::Entries?p.entries.size():p.current.size();}
std::uint64_t PageSize(const b::Page& p){std::uint64_t n=64;for(const auto& x:p.projects)n+=RecordSize(x);for(const auto& x:p.entries)n+=RecordSize(x);for(const auto& x:p.current)n+=64;return n;}
// These builders use independently written offsets. No production wire helper,
// RecordBytes/PageBytes, encoder or decoder computes their expected bytes.
Bytes GoldenPage(const b::Page& page){
    Bytes bytes(static_cast<std::size_t>(PageSize(page)),0);const char magic[16]={'M','M','S','C','a','c','h','e','P','a','g','e','0','0','0','1'};std::copy_n(magic,16,bytes.begin());
    Put(bytes,16,1,4);Put(bytes,20,static_cast<std::uint32_t>(page.kind),4);Put(bytes,24,page.ordinal,4);Put(bytes,28,Count(page),4);Put(bytes,32,bytes.size()-64,8);std::copy(page.storeOwner.begin(),page.storeOwner.end(),bytes.begin()+40);
    std::size_t at=64;
    for(const auto& p:page.projects){Put(bytes,at,RecordSize(p),4);Put(bytes,at+4,p.pinned?1:0,4);Put(bytes,at+8,p.id,8);Put(bytes,at+16,p.lastOpened,8);Put(bytes,at+24,p.normalizedPath.size(),4);Put(bytes,at+28,p.displayPath.size(),4);at+=40;PutString(bytes,at,p.normalizedPath);PutString(bytes,at,p.displayPath);}
    for(const auto& e:page.entries){const auto size=RecordSize(e);Put(bytes,at,size,4);Put(bytes,at+4,static_cast<std::uint32_t>(e.availability),4);Put(bytes,at+8,e.consumerFormat,4);Put(bytes,at+12,e.owners.size(),4);Put(bytes,at+16,e.lastUse,8);Blob(bytes,at+32,e.blob);for(std::size_t j=0;j<e.owners.size();++j)Put(bytes,at+192+8*j,e.owners[j],8);at+=static_cast<std::size_t>(size);}
    for(const auto& c:page.current){Put(bytes,at,64,4);Put(bytes,at+4,static_cast<std::uint32_t>(c.key.kind),4);Put(bytes,at+8,c.key.consumerFormat,4);std::copy(c.key.key.begin(),c.key.key.end(),bytes.begin()+at+16);std::copy(c.generation.begin(),c.generation.end(),bytes.begin()+at+48);at+=64;}
    Check(at==bytes.size(),"independent page builder consumes every expected byte");return bytes;
}
Bytes GoldenManifest(const b::Manifest& m){
    Bytes bytes(96+176*m.pages.size(),0);const char magic[16]={'M','M','S','C','a','c','h','e','I','n','d','e','x','0','0','1'};std::copy_n(magic,16,bytes.begin());Put(bytes,16,1,4);Put(bytes,20,96,4);std::copy(m.storeOwner.begin(),m.storeOwner.end(),bytes.begin()+24);
    Put(bytes,40,m.maintenanceEpoch,8);Put(bytes,48,m.nextProjectId,8);Put(bytes,56,m.projectCount,4);Put(bytes,60,m.entryCount,4);Put(bytes,64,m.currentCount,4);Put(bytes,68,m.pages.size(),4);Put(bytes,72,m.liveEncodedBytes,8);
    for(std::size_t n=0;n<m.pages.size();++n){const auto& r=m.pages[n];const auto at=96+n*176;Put(bytes,at,static_cast<std::uint32_t>(r.kind),4);Put(bytes,at+4,r.ordinal,4);Put(bytes,at+8,r.recordCount,4);Blob(bytes,at+16,r.blob);}return bytes;
}
b::Manifest Manifest(const b::Index& index,const std::vector<b::Page>& pages){
    b::Manifest m;m.storeOwner=index.storeOwner;m.maintenanceEpoch=index.maintenanceEpoch;m.nextProjectId=index.nextProjectId;
    m.liveEncodedBytes=96+176*pages.size();
    for(std::size_t n=0;n<pages.size();++n){const auto& page=pages[n];b::PageRef r;r.kind=page.kind;r.ordinal=page.ordinal;r.recordCount=static_cast<std::uint32_t>(Count(page));
        r.blob.name=Name(static_cast<std::uint32_t>(n+0x100000));r.blob.spec.owner=index.storeOwner;r.blob.spec.kind=f::Kind::IndexPage;r.blob.spec.key[0]=4;r.blob.payloadBytes=PageSize(page);r.blob.identity={0x11223344,0x1234567890000000ull+n,128+r.blob.payloadBytes};r.blob.payloadDigest[0]=static_cast<std::uint8_t>(n+1);
        m.liveEncodedBytes+=r.blob.payloadBytes;m.projectCount+=static_cast<std::uint32_t>(page.projects.size());m.entryCount+=static_cast<std::uint32_t>(page.entries.size());m.currentCount+=static_cast<std::uint32_t>(page.current.size());m.pages.push_back(std::move(r));}return m;
}
std::vector<b::Page> SingleRecordPages(const b::Index& index){
    std::vector<b::Page> pages;std::uint32_t ordinal=0;
    for(const auto& p:index.projects){b::Page page;page.storeOwner=index.storeOwner;page.kind=b::PageKind::Projects;page.ordinal=ordinal++;page.projects.push_back(p);pages.push_back(std::move(page));}ordinal=0;
    for(const auto& e:index.entries){b::Page page;page.storeOwner=index.storeOwner;page.kind=b::PageKind::Entries;page.ordinal=ordinal++;page.entries.push_back(e);pages.push_back(std::move(page));}ordinal=0;
    for(const auto& c:index.current){b::Page page;page.storeOwner=index.storeOwner;page.kind=b::PageKind::Current;page.ordinal=ordinal++;page.current.push_back(c);pages.push_back(std::move(page));}return pages;
}
void RejectIndex(const b::Index& input,Result expected){const auto sentinel=SingleRecordPages(Small());auto pages=sentinel;Check(b::Validate(input)==expected,"invalid/limited index returns explicit expected category");Check(b::Partition(input,pages)==expected&&SameVector(pages,sentinel),"failed Partition preserves complete prior output");}
void RejectAssemble(const b::Manifest& manifest,const std::vector<b::Page>& pages,Result expected=Result::Invalid){auto out=Small();out.maintenanceEpoch=93;const auto before=out;Check(b::Assemble(manifest,pages,out)==expected&&Same(out,before),"failed Assemble preserves all prior index fields");}
void GoldenChecks(){
    const auto index=Small();Check(b::Validate(index)==Result::Ok,"DOS and UNC paths, lone surrogate, same-name different-kind entries are valid");
    std::vector<b::Page> pages(3);for(unsigned n=0;n<3;++n){pages[n].storeOwner=index.storeOwner;pages[n].kind=static_cast<b::PageKind>(n+1);}pages[0].projects=index.projects;pages[1].entries=index.entries;pages[2].current=index.current;
    for(const auto& page:pages){const auto bytes=GoldenPage(page);Check(bytes[15]=='1'&&bytes[16]==1&&bytes[17]==0,"page magic is 16 ASCII bytes without NUL and little-endian version");Bytes actual{1,2};Check(b::EncodePage(page,actual)==Result::Ok&&actual==bytes,"each page type matches complete independent byte oracle");auto out=pages[0];Check(b::DecodePage(bytes.data(),bytes.size(),out)==Result::Ok&&Same(out,page),"decode hand-built page without normalizing UTF16 or dropping fields");}
    const auto manifest=Manifest(index,pages);const auto bytes=GoldenManifest(manifest);Check(bytes[15]=='1'&&bytes[20]==96,"manifest independent 16-byte ASCII magic and header size");Bytes actual{8,9};Check(b::EncodeManifest(manifest,actual)==Result::Ok&&actual==bytes,"manifest matches complete independent byte oracle");auto decoded=manifest;decoded.maintenanceEpoch=1;Check(b::DecodeManifest(bytes.data(),bytes.size(),decoded)==Result::Ok&&Same(decoded,manifest),"decode hand-built manifest including full page references");
    b::Index assembled;Check(b::Assemble(decoded,pages,assembled)==Result::Ok&&Same(assembled,index),"assemble hand-built independent page graph");
    // File references are deliberately synthetic: this only checks representation.
    Check(manifest.pages[0].blob.spec.kind==static_cast<f::Kind>(6)&&manifest.pages[0].blob.spec.key==f::Digest{4},"business pages have their own file kind and key");
}
void WireRejections(){
    const auto index=Small();const auto pages=SingleRecordPages(index);const auto pageSentinel=pages.front();
    auto rejectPage=[&](const Bytes& bytes,Result expected=Result::Invalid){auto out=pageSentinel;Check(b::DecodePage(bytes.data(),bytes.size(),out)==expected&&Same(out,pageSentinel),"bad page bytes preserve prior Page output");};
    for(const auto& page:pages){const auto bytes=GoldenPage(page);
        {auto bad=bytes;bad[15]=0;rejectPage(bad);}
        for(unsigned at:{0u,15u,56u,57u,58u,59u,60u,61u,62u,63u}){auto bad=bytes;bad[at]^=0x40;rejectPage(bad);}
        {auto bad=bytes;Put(bad,16,2,4);rejectPage(bad,Result::Unsupported);}
        {auto bad=bytes;Put(bad,32,(std::numeric_limits<std::uint64_t>::max)(),8);rejectPage(bad);}
        {auto bad=bytes;bad.push_back(0);rejectPage(bad);}
        for(std::size_t size=0;size<bytes.size();++size){Bytes truncated(bytes.begin(),bytes.begin()+size);rejectPage(truncated);}
        {auto bad=bytes;Put(bad,64,0xffffffffu,4);rejectPage(bad);}
    }
    {auto out=pageSentinel;Check(b::DecodePage(nullptr,64,out)==Result::Invalid&&Same(out,pageSentinel),"null page pointer rejected");}
    {auto bad=GoldenPage(pages[0]);Put(bad,64+4,2,4);rejectPage(bad);bad=GoldenPage(pages[0]);bad[64+32]=1;rejectPage(bad);}
    {auto bad=GoldenPage(pages[2]);bad[64+24]=1;rejectPage(bad);for(unsigned at:{37u,44u,140u}){bad=GoldenPage(pages[2]);bad[64+32+at]=1;rejectPage(bad);}}
    {auto bad=GoldenPage(pages[5]);bad[64+12]=1;rejectPage(bad);}
    const auto manifest=Manifest(index,pages);const auto bytes=GoldenManifest(manifest);
    auto rejectManifest=[&](const Bytes& bad,Result expected=Result::Invalid){auto out=manifest;Check(b::DecodeManifest(bad.data(),bad.size(),out)==expected&&Same(out,manifest),"bad manifest bytes preserve all old output fields");};
    {auto bad=bytes;bad[15]=0;rejectManifest(bad);}
    for(unsigned at:{0u,15u,80u,81u,82u,83u,84u,85u,86u,87u,88u,89u,90u,91u,92u,93u,94u,95u,108u,109u,110u,111u,149u,156u,252u}){auto bad=bytes;bad[at]^=0x40;rejectManifest(bad);}
    {auto bad=bytes;Put(bad,16,2,4);rejectManifest(bad,Result::Unsupported);bad=bytes;Put(bad,20,95,4);rejectManifest(bad);bad=bytes;bad.push_back(0);rejectManifest(bad);}
    for(std::size_t size=0;size<bytes.size();++size){Bytes truncated(bytes.begin(),bytes.begin()+size);rejectManifest(truncated);}
    {auto out=manifest;Check(b::DecodeManifest(nullptr,96,out)==Result::Invalid&&Same(out,manifest),"null manifest rejected");}
    for(unsigned offset:{56u,60u,64u}){auto bad=bytes;Put(bad,offset,(offset==56?manifest.projectCount:offset==60?manifest.entryCount:manifest.currentCount)+1,4);rejectManifest(bad);}
    {auto bad=manifest;bad.pages[0].blob.identity.fileBytes++;Bytes out{4,5};Check(b::EncodeManifest(bad,out)==Result::Invalid&&out==Bytes({4,5}),"bad reference EncodeManifest preserves output");}
    {auto large=manifest;const auto prior=large.pages[0].blob.payloadBytes;large.pages[0].blob.payloadBytes=1024*1024+1;large.pages[0].blob.identity.fileBytes=128+large.pages[0].blob.payloadBytes;large.liveEncodedBytes+=large.pages[0].blob.payloadBytes-prior;Bytes out{4,5};
        Check(b::ValidateManifest(large)==Result::Limit&&b::EncodeManifest(large,out)==Result::Limit&&out==Bytes({4,5}),"single referenced page over 1MiB returns Limit without other inconsistent fields");rejectManifest(GoldenManifest(large),Result::Limit);}
    {auto bad=pages[0];bad.entries.push_back(index.entries[0]);Bytes out{4,5};Check(b::ValidatePage(bad)==Result::Invalid&&b::EncodePage(bad,out)==Result::Invalid&&out==Bytes({4,5}),"mixed-kind page rejected without dropping wrong vector");}
}
void PathAndRelationshipChecks(){
    const auto good=Small();
    const std::vector<std::u16string> badPaths={u"relative.pmm",u"C:relative.pmm",u"C:/x.pmm",u"C:\\a\\..\\x",u"C:\\.\\x",u"C:\\a\\x.",u"C:\\a\\x ",u"C:\\a:x",u"C:\\NUL.pmm",u"C:\\COM1.txt",u"C:\\LPT9",u"C:\\a?b",u"C:\\a*b",u"C:\\a|b",u"C:\\a<b",u"C:\\a>b",u"C:\\a\"b",u"\\\\?\\C:\\x",u"\\\\.\\C:\\x",u"\\\\server",u"\\\\server\\",std::u16string(u"C:\\a\0b",6)};
    for(const auto& path:badPaths){auto bad=good;bad.projects[0].normalizedPath=path;bad.projects[0].displayPath=path;RejectIndex(bad,Result::Invalid);}
    {auto bad=good;bad.projects[0].displayPath=u"C:\\Different.pmm";RejectIndex(bad,Result::Invalid);}
    {auto bad=good;bad.projects[1].normalizedPath=bad.projects[0].displayPath;bad.projects[1].displayPath=bad.projects[0].normalizedPath;RejectIndex(bad,Result::Invalid);}
    {auto goodLow=good;goodLow.projects[0].normalizedPath.back()=char16_t(0xdc00);goodLow.projects[0].displayPath.back()=char16_t(0xdc00);Check(b::Validate(goodLow)==Result::Ok,"unpaired low surrogate also preserved as a path unit");}
    {auto unicode=good;unicode.projects[1].normalizedPath=u"\\\\server\\share\\\u00c4.pmm";unicode.projects[1].displayPath=u"\\\\SERVER\\SHARE\\\u00e4.PMM";Check(b::Validate(unicode)==Result::Ok,"ordinal case comparison supports non-ASCII path letters");}
    {auto bad=good;bad.maintenanceEpoch=0;RejectIndex(bad,Result::Invalid);bad=good;bad.nextProjectId=9;RejectIndex(bad,Result::Invalid);}
    {auto bad=good;bad.entries[0].owners={9,2};RejectIndex(bad,Result::Invalid);bad=good;bad.entries[0].owners={2,2};RejectIndex(bad,Result::Invalid);bad=good;bad.entries[0].owners={8};RejectIndex(bad,Result::Invalid);}
    {auto bad=good;bad.entries[0].blob.identity.fileBytes++;RejectIndex(bad,Result::Invalid);bad=good;bad.entries[0].blob.name[0]=L'A';RejectIndex(bad,Result::Invalid);bad=good;bad.entries[0].blob.spec.owner[0]^=1;RejectIndex(bad,Result::Invalid);}
    auto split=SingleRecordPages(good);const auto base=Manifest(good,split);b::Index assembled;Check(b::Assemble(base,split,assembled)==Result::Ok&&Same(assembled,good),"single-record pages form valid cross-page graph");
    const auto rejectChanged=[&](const std::vector<b::Page>& changed){RejectAssemble(Manifest(good,changed),changed);};
    {auto bad=split;bad[1].projects[0].id=2;rejectChanged(bad);bad=split;bad[1].projects[0].normalizedPath=bad[0].projects[0].displayPath;bad[1].projects[0].displayPath=bad[0].projects[0].normalizedPath;rejectChanged(bad);}
    {auto bad=split;bad[3].entries[0]=bad[2].entries[0];rejectChanged(bad);bad=split;std::swap(bad[2].entries[0],bad[3].entries[0]);rejectChanged(bad);}
    {auto bad=split;bad[2].entries[0].owners={8};rejectChanged(bad);}
    for(auto state:{b::Availability::Invalid,b::Availability::Retired}){auto bad=split;bad[2].entries[0].availability=state;rejectChanged(bad);}
    {auto bad=split;bad[5].current[0].key.consumerFormat++;rejectChanged(bad);bad=split;bad[5].current[0].key.key[31]^=1;rejectChanged(bad);bad=split;bad[5].current[0].generation=Generation(77);rejectChanged(bad);}
    {auto bad=split;bad[6].current[0]=bad[5].current[0];rejectChanged(bad);}
    {auto bad=split;bad.pop_back();RejectAssemble(base,bad);bad=split;std::swap(bad[0],bad[1]);RejectAssemble(base,bad);bad=split;bad[0].storeOwner[0]^=1;RejectAssemble(base,bad);}
    {auto m=base;m.pages[1].ordinal=0;RejectAssemble(m,split);m=base;m.liveEncodedBytes++;RejectAssemble(m,split);m=base;m.pages[0].blob.payloadBytes++;m.pages[0].blob.identity.fileBytes++;m.liveEncodedBytes++;RejectAssemble(m,split);}
}
struct SizePlan {std::uint64_t live=96;std::size_t pages=0;};
template<class T>void PlanRecords(const std::vector<T>& records,SizePlan& plan){std::uint64_t used=0;for(const auto& r:records){const auto n=RecordSize(r);if(!used||used+n>1024*1024){++plan.pages;plan.live+=64+176;used=64;}plan.live+=n;used+=n;}}
SizePlan Plan(const b::Index& index){SizePlan p;PlanRecords(index.projects,p);PlanRecords(index.entries,p);PlanRecords(index.current,p);return p;}
void CheckGreedy(const b::Index& index,const std::vector<b::Page>& pages){
    const auto plan=Plan(index);Check(pages.size()==plan.pages,"Partition page count matches independent greedy packing");std::uint64_t total=96+176*pages.size();
    std::uint32_t lastKind=0,nextOrdinal=0;for(std::size_t n=0;n<pages.size();++n){const auto& page=pages[n];const auto kind=static_cast<std::uint32_t>(page.kind);if(kind!=lastKind){Check(kind>lastKind,"page kinds ordered");lastKind=kind;nextOrdinal=0;}Check(page.ordinal==nextOrdinal++&&Count(page)>0,"nonempty pages have contiguous per-kind ordinal");const auto size=PageSize(page);Check(size<=1024*1024,"each complete page stays within 1MiB");total+=size;
        if(n+1<pages.size()&&pages[n+1].kind==page.kind){const auto& next=pages[n+1];const auto first=page.kind==b::PageKind::Projects?RecordSize(next.projects.front()):page.kind==b::PageKind::Entries?RecordSize(next.entries.front()):64;Check(size+first>1024*1024,"page boundary is greedy without splitting records");}}
    Check(total==plan.live,"independent full payload plus manifest byte accounting");
}
void RoundTrip(const b::Index& input,bool large){
    std::vector<b::Page> pages;Check(b::Partition(input,pages)==Result::Ok,"Partition complete index");CheckGreedy(input,pages);auto manifest=Manifest(input,pages);
    if(large)Check(manifest.liveEncodedBytes>16*1024*1024&&manifest.liveEncodedBytes<=64*1024*1024&&96+176*pages.size()<23*1024,"large index exceeds single catalog payload while manifest stays small");
    std::vector<b::Page> decoded;decoded.reserve(pages.size());for(auto& page:pages){const auto expected=GoldenPage(page);Bytes encoded;Check(b::EncodePage(page,encoded)==Result::Ok&&encoded==expected,"partitioned page bytes match independent offset oracle");b::Page out;Check(b::DecodePage(encoded.data(),encoded.size(),out)==Result::Ok&&Same(out,page),"each partitioned page decodes losslessly");decoded.push_back(std::move(out));page={};}
    pages.clear();pages.shrink_to_fit();const auto expectedManifest=GoldenManifest(manifest);Bytes encodedManifest;Check(b::EncodeManifest(manifest,encodedManifest)==Result::Ok&&encodedManifest==expectedManifest,"full manifest independent byte comparison");b::Manifest loaded;Check(b::DecodeManifest(encodedManifest.data(),encodedManifest.size(),loaded)==Result::Ok&&Same(loaded,manifest),"full manifest decode");
    b::Index output;Check(b::Assemble(loaded,decoded,output)==Result::Ok&&Same(output,input),"complete graph survives Partition encode decode manifest Assemble");
}
b::Project NumberedProject(std::uint64_t n){b::Project p;p.id=n;p.normalizedPath=u"C:\\Projects\\";for(char c:std::to_string(n))p.normalizedPath+=char16_t(c);p.normalizedPath+=u".pmm";p.displayPath=p.normalizedPath;p.pinned=(n%3==0);p.lastOpened=n;return p;}
void LargeChecks(){
    b::Index index;index.storeOwner=Owner();index.maintenanceEpoch=1;index.nextProjectId=10001;index.projects.reserve(10000);for(unsigned n=1;n<=10000;++n)index.projects.push_back(NumberedProject(n));
    index.entries.reserve(100000);index.current.reserve(100000);for(unsigned n=1;n<=100000;++n){auto e=Entry(n);e.owners={1+(n-1)%10000};index.current.push_back(Current(e,n));index.entries.push_back(std::move(e));}
    Check(b::Validate(index)==Result::Ok,"100000 entries and current mappings with 10000 projects remain valid");RoundTrip(index,true);
    auto more=Entry(100001);index.entries.push_back(std::move(more));RejectIndex(index,Result::Limit);index.entries.pop_back();
    // A local Current page has no cross-page foreign-key requirement. This
    // avoids constructing an impossible >100000 globally unique mapping over
    // only 100000 entries and confusing Limit with a dangling-reference error.
    {b::Page page;page.storeOwner=index.storeOwner;page.kind=b::PageKind::Current;page.current=index.current;page.current.push_back(Current(Entry(100001),100001));Bytes encoded{9,7};
        Check(b::ValidatePage(page)==Result::Limit&&b::EncodePage(page,encoded)==Result::Limit&&encoded==Bytes({9,7}),"over-limit real Current page preserves encoded output");
        const auto bytes=GoldenPage(page);auto out=SingleRecordPages(Small()).front();const auto before=out;Check(b::DecodePage(bytes.data(),bytes.size(),out)==Result::Limit&&Same(out,before),"over-limit complete Current payload preserves decoded output");}
    index.projects.push_back(NumberedProject(10001));index.nextProjectId=10002;RejectIndex(index,Result::Limit);
}
void Limits(){
    {auto index=Small();auto& p=index.projects[0];p.normalizedPath=u"C:\\";p.normalizedPath.append(32768-3,u'x');p.displayPath=p.normalizedPath;Check(b::Validate(index)==Result::Ok,"exact 32768 UTF16 units preserved without truncation");p.normalizedPath+=u'x';p.displayPath+=u'x';RejectIndex(index,Result::Limit);}
    {auto page=SingleRecordPages(Small())[2];page.entries[0].owners.clear();for(unsigned n=1;n<=10001;++n)page.entries[0].owners.push_back(n);Bytes out{8,1};
        Check(b::ValidatePage(page)==Result::Limit&&b::EncodePage(page,out)==Result::Limit&&out==Bytes({8,1}),"over-limit owner vector remains unique and sorted; local page has no external FK check");}
    // Real owner vectors bring the valid encoded graph to within one owner ID
    // of 64MiB. No giant byte buffer or fabricated integer bypasses validation.
    b::Index index;index.storeOwner=Owner();index.maintenanceEpoch=1;index.nextProjectId=10001;for(unsigned n=1;n<=10000;++n)index.projects.push_back(NumberedProject(n));
    std::vector<b::ProjectId> owners;owners.reserve(10000);for(unsigned n=1;n<=10000;++n)owners.push_back(n);
    unsigned generation=1;
    for(;;){auto entry=Entry(generation++);entry.owners=owners;index.entries.push_back(std::move(entry));if(Plan(index).live>64ull*1024*1024){index.entries.pop_back();--generation;break;}}
    auto last=Entry(generation);index.entries.push_back(std::move(last));std::size_t low=0,high=10000;
    while(low<high){const auto mid=(low+high+1)/2;index.entries.back().owners.assign(owners.begin(),owners.begin()+mid);if(Plan(index).live<=64ull*1024*1024)low=mid;else high=mid-1;}
    index.entries.back().owners.assign(owners.begin(),owners.begin()+low);const auto near=Plan(index);const auto gap=64ull*1024*1024-near.live;
    Check(near.live<=64ull*1024*1024&&gap<248&&gap%4==0&&low<10000,"independent near-budget gap is bounded by one owner ID plus one new page");
    index.projects.front().normalizedPath.append(static_cast<std::size_t>(gap/4),u'x');index.projects.front().displayPath=index.projects.front().normalizedPath;
    const auto size=Plan(index);Check(size.live==64ull*1024*1024,"legal preserved path units bring actual encoded graph exactly to 64MiB");
    Check(b::Validate(index)==Result::Ok,"complete near-64MiB semantic index is valid");{std::vector<b::Page> pages;Check(b::Partition(index,pages)==Result::Ok,"Partition accepts complete near-limit graph");CheckGreedy(index,pages);Check(Manifest(index,pages).liveEncodedBytes==size.live,"manifest charges exact independent near-budget size");}
    index.entries.back().owners.push_back(owners[low]);Check(Plan(index).live>64ull*1024*1024,"one extra actual owner ID crosses encoded budget");RejectIndex(index,Result::Limit);
    std::printf("near-budget valid-bytes=%llu pages=%llu entries=%llu\n",static_cast<unsigned long long>(size.live),static_cast<unsigned long long>(size.pages),static_cast<unsigned long long>(index.entries.size()));
}
}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{GoldenChecks();WireRejections();PathAndRelationshipChecks();RoundTrip(Small(),false);{b::Index empty;empty.storeOwner=Owner();empty.maintenanceEpoch=1;empty.nextProjectId=1;RoundTrip(empty,false);}LargeChecks();Limits();std::printf("PASS cache business codec checks=%u; CPU/memory only; synthetic page references do not establish disk identity\n",checks);return 0;}
    catch(const std::exception& e){std::fprintf(stderr,"FAIL cache business codec checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache business codec unexpected exception\n");return 2;}
}
