// MAINTENANCE: regression-keep. Pure memory, ordinary current-thread new faults.
#include "../src/io/cache_business_codec.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace allocation {
struct State { std::size_t failAt, attempts=0, hits=0; };
thread_local State* active=nullptr;
constexpr auto CountOnly=(std::numeric_limits<std::size_t>::max)();
struct Scope {
    State state; State* previous;
    explicit Scope(std::size_t ordinal) noexcept:state{ordinal},previous(active){active=&state;}
    ~Scope(){active=previous;}
};
}
void* operator new(std::size_t size){
    if(auto* s=allocation::active){const auto ordinal=s->attempts++;if(ordinal==s->failAt){++s->hits;throw std::bad_alloc();}}
    if(void* p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

namespace {
namespace business=mikumikustudio::cache_business;
namespace files=mikumikustudio::cache_files;
using namespace business;
unsigned checks=0,scans=0;
std::size_t injected=0;
constexpr std::size_t MaximumOrdinals=1024;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}

bool Same(const files::BlobInfo& a,const files::BlobInfo& b){
    return a.name==b.name&&a.spec.kind==b.spec.kind&&a.spec.owner==b.spec.owner&&a.spec.key==b.spec.key&&
        a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&
        a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;
}
bool Same(const LogicalKey& a,const LogicalKey& b){return a.kind==b.kind&&a.consumerFormat==b.consumerFormat&&a.key==b.key;}
bool Same(const Project& a,const Project& b){return a.id==b.id&&a.normalizedPath==b.normalizedPath&&a.displayPath==b.displayPath&&a.lastOpened==b.lastOpened&&a.pinned==b.pinned;}
bool Same(const Entry& a,const Entry& b){return Same(a.blob,b.blob)&&a.consumerFormat==b.consumerFormat&&a.lastUse==b.lastUse&&a.availability==b.availability&&a.owners==b.owners;}
bool Same(const Current& a,const Current& b){return Same(a.key,b.key)&&a.generation==b.generation;}
bool Same(const Bytes& a,const Bytes& b){return a==b;}
bool Same(const Page&,const Page&);
bool Same(const PageRef&,const PageRef&);
template<class T> bool SameVector(const std::vector<T>& a,const std::vector<T>& b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(!Same(a[i],b[i]))return false;
    return true;
}
bool Same(const Index& a,const Index& b){return a.storeOwner==b.storeOwner&&a.maintenanceEpoch==b.maintenanceEpoch&&a.nextProjectId==b.nextProjectId&&SameVector(a.projects,b.projects)&&SameVector(a.entries,b.entries)&&SameVector(a.current,b.current);}
bool Same(const Page& a,const Page& b){return a.storeOwner==b.storeOwner&&a.kind==b.kind&&a.ordinal==b.ordinal&&SameVector(a.projects,b.projects)&&SameVector(a.entries,b.entries)&&SameVector(a.current,b.current);}
bool Same(const std::vector<Page>& a,const std::vector<Page>& b){return SameVector(a,b);}
bool Same(const PageRef& a,const PageRef& b){return a.kind==b.kind&&a.ordinal==b.ordinal&&a.recordCount==b.recordCount&&Same(a.blob,b.blob);}
bool Same(const Manifest& a,const Manifest& b){return a.storeOwner==b.storeOwner&&a.maintenanceEpoch==b.maintenanceEpoch&&a.nextProjectId==b.nextProjectId&&a.projectCount==b.projectCount&&a.entryCount==b.entryCount&&a.currentCount==b.currentCount&&a.liveEncodedBytes==b.liveEncodedBytes&&SameVector(a.pages,b.pages);}
struct Assembly {Manifest manifest;std::vector<Page> pages;};
bool Same(const Assembly& a,const Assembly& b){return Same(a.manifest,b.manifest)&&Same(a.pages,b.pages);}

std::wstring Name(std::uint8_t value){
    constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring name;name.reserve(37);
    for(unsigned i=0;i<16;++i){name+=hex[value>>4];name+=hex[value&15];}
    return name+L".blob";
}
files::BlobInfo Blob(files::Kind kind,const files::Owner& owner,std::uint8_t generation,std::size_t bytes){
    files::BlobInfo b;b.name=Name(generation);b.spec.kind=kind;b.spec.owner=owner;b.spec.key.fill(generation);
    b.payloadDigest.fill(std::uint8_t(generation+7));b.payloadBytes=bytes;
    b.identity.volume=0x1234;b.identity.fileIndex=0x1000+generation;b.identity.fileBytes=128+bytes;return b;
}
Index AuthoredIndex(){
    Index x;x.storeOwner.fill(0x37);x.maintenanceEpoch=7;x.nextProjectId=3;
    Project first;first.id=1;first.normalizedPath=u"C:\\codec-fixtures\\first-project.pmm";
    first.displayPath=u"c:\\CODEC-fixtures\\First-Project.pmm";first.lastOpened=0x123456789ull;first.pinned=true;
    Project second;second.id=2;second.normalizedPath=u"C:\\codec-fixtures\\second-project.pmm";second.displayPath=second.normalizedPath;second.lastOpened=0x99887766;
    x.projects={first,second};
    for(unsigned i=0;i<2;++i){
        const auto kind=i?files::Kind::Texture:files::Kind::Effect;const auto generation=std::uint8_t(0x12+i*0x22);
        Entry e;e.blob=Blob(kind,x.storeOwner,generation,23+i);e.consumerFormat=41+i;e.lastUse=0x33445566+i;e.owners={1,2};
        Current c;c.key.kind=kind;c.key.consumerFormat=e.consumerFormat;c.key.key=e.blob.spec.key;c.generation.fill(generation);
        x.entries.push_back(e);x.current.push_back(c);
    }
    return x;
}
std::vector<Page> AuthoredPages(const Index& index){
    std::vector<Page> result(3);for(unsigned i=0;i<3;++i){result[i].storeOwner=index.storeOwner;result[i].kind=static_cast<PageKind>(i+1);}
    result[0].projects=index.projects;result[1].entries=index.entries;result[2].current=index.current;return result;
}
Page OldPage(const Index& index){
    Page p;p.storeOwner.fill(0xc1);p.kind=PageKind::Current;p.ordinal=19;
    p.projects=index.projects;p.entries=index.entries;p.current=index.current;return p;
}
Manifest OldManifest(const Manifest& manifest){
    auto m=manifest;m.storeOwner.fill(0xee);m.maintenanceEpoch=991;m.nextProjectId=992;
    m.projectCount=81;m.entryCount=82;m.currentCount=83;m.liveEncodedBytes=123456;
    m.pages[0].ordinal=77;m.pages[0].blob.identity.fileIndex=998;m.pages[0].blob.payloadDigest.fill(0x71);return m;
}
Index OldIndex(const Index& index){auto x=index;x.storeOwner.fill(0xc2);x.maintenanceEpoch=878;x.nextProjectId=879;x.projects[0].pinned=false;x.entries[0].availability=Availability::Retired;return x;}
std::uint64_t Little(const Bytes& bytes,std::size_t at,unsigned n){std::uint64_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint64_t(bytes.at(at+i))<<(i*8);return value;}
std::size_t AuthoredPageBytes(const Page& p){
    std::size_t size=64;
    if(p.kind==PageKind::Projects)for(const auto& r:p.projects)size+=40+2*(r.normalizedPath.size()+r.displayPath.size());
    else if(p.kind==PageKind::Entries)for(const auto& r:p.entries)size+=192+8*r.owners.size();
    else size+=64*p.current.size();
    return size;
}
void PageWire(const Bytes& bytes,const Page& expected){
    constexpr char magic[16]={'M','M','S','C','a','c','h','e','P','a','g','e','0','0','0','1'};
    Check(bytes.size()==AuthoredPageBytes(expected),"page wire length equals independent authored record-size arithmetic");
    Check(std::equal(bytes.begin(),bytes.begin()+16,reinterpret_cast<const std::uint8_t*>(magic)),"page wire exact 16 ASCII bytes including final 1, without NUL");
    Check(Little(bytes,16,4)==1&&Little(bytes,20,4)==std::uint32_t(expected.kind)&&Little(bytes,24,4)==expected.ordinal&&Little(bytes,28,4)==2&&Little(bytes,32,8)==bytes.size()-64,"page header fields match authored fixture");
    Check(std::equal(expected.storeOwner.begin(),expected.storeOwner.end(),bytes.begin()+40),"page wire owner matches authored identity");
    Page decoded;Check(DecodePage(bytes.data(),bytes.size(),decoded)==Result::Ok&&Same(decoded,expected),"encoded page decodes into every authored field");
}
struct Fixture {
    Index index=AuthoredIndex();
    std::vector<Page> pages=AuthoredPages(index);
    std::vector<Bytes> encodedPages;
    Manifest manifest;
    Bytes encodedManifest;
    Fixture(){
        Check(Validate(index)==Result::Ok,"authored three-kind fixture is valid");
        manifest.storeOwner=index.storeOwner;manifest.maintenanceEpoch=index.maintenanceEpoch;manifest.nextProjectId=index.nextProjectId;
        manifest.projectCount=manifest.entryCount=manifest.currentCount=2;manifest.liveEncodedBytes=96+3*176;
        for(unsigned i=0;i<3;++i){
            Bytes bytes;Check(EncodePage(pages[i],bytes)==Result::Ok,"prepare real page encoding before fault injection");PageWire(bytes,pages[i]);
            PageRef ref;ref.kind=pages[i].kind;ref.recordCount=2;ref.blob=Blob(files::Kind::IndexPage,index.storeOwner,std::uint8_t(0x50+i),bytes.size());ref.blob.spec.key=PageKey;
            manifest.pages.push_back(ref);manifest.liveEncodedBytes+=bytes.size();encodedPages.push_back(std::move(bytes));
        }
        Check(ValidateManifest(manifest)==Result::Ok&&EncodeManifest(manifest,encodedManifest)==Result::Ok,"prepare coherent manifest before injection");
    }
};
struct Call {Result result=Result::Invalid;std::size_t attempts=0,hits=0;};
template<class F> Call Invoke(F&& operation,std::size_t ordinal){
    Call c;{allocation::Scope scope(ordinal);c.result=operation();c.attempts=scope.state.attempts;c.hits=scope.state.hits;}return c;
}
template<class Input,class Output,class Operation,class Success>
void Scan(const char* name,const Input& source,const Output& sentinel,Operation operation,Success success){
    ++scans;std::size_t count=0;
    {auto input=source;const auto before=input;auto output=sentinel;
        const auto c=Invoke([&]{return operation(input,output);},allocation::CountOnly);count=c.attempts;
        Check(c.result==Result::Ok&&c.hits==0&&count>0&&count<=MaximumOrdinals,"output operation has bounded successful allocation calibration");
        Check(Same(input,before),"healthy operation preserves every input field");success(output);}
    std::printf("allocation baseline %s=%llu\n",name,static_cast<unsigned long long>(count));
    for(std::size_t ordinal=0;ordinal<count;++ordinal){
        auto input=source;const auto beforeInput=input;auto output=sentinel;const auto beforeOutput=output;
        const auto c=Invoke([&]{return operation(input,output);},ordinal);
        if(c.hits!=1||c.result!=Result::AllocationFailure)std::printf("FAULT %s ordinal=%llu attempts=%llu hits=%llu result=%u\n",name,static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(c.attempts),static_cast<unsigned long long>(c.hits),unsigned(c.result));
        Check(c.hits==1,"each calibrated allocation ordinal actually fails once");
        Check(c.result==Result::AllocationFailure,"bad_alloc reports AllocationFailure, not Invalid or partial success");
        Check(Same(input,beforeInput)&&Same(output,beforeOutput),"failed operation preserves every input and old output field");++injected;
        Check(operation(input,output)==Result::Ok,"same failed input/output objects support an uninjected retry");
        Check(Same(input,beforeInput),"retry preserves every input field");success(output);
    }
}
template<class Input,class Operation> void ScanValidation(const char* name,const Input& source,Operation operation){
    ++scans;auto input=source;const auto before=input;
    const auto baseline=Invoke([&]{return operation(input);},allocation::CountOnly);
    Check(baseline.result==Result::Ok&&baseline.hits==0&&baseline.attempts<=MaximumOrdinals&&Same(input,before),"validator healthy calibration retains input");
    std::printf("allocation baseline %s=%llu%s\n",name,static_cast<unsigned long long>(baseline.attempts),baseline.attempts?"":" (no ordinary allocations; no injection claimed)");
    for(std::size_t ordinal=0;ordinal<baseline.attempts;++ordinal){
        auto candidate=source;const auto snapshot=candidate;const auto c=Invoke([&]{return operation(candidate);},ordinal);
        Check(c.hits==1&&c.result==Result::AllocationFailure,"validator allocation fault is hit and classified");
        Check(Same(candidate,snapshot),"validator failure preserves every field");++injected;
        Check(operation(candidate)==Result::Ok&&Same(candidate,snapshot),"validator retry succeeds without mutating input");
    }
}
void Calibration(){
    using Allocate=void*(*)(std::size_t);Allocate volatile allocate=static_cast<Allocate>(&::operator new);
    bool caught=false;std::size_t hits=0,attempts=0;
    {allocation::Scope scope(0);try{auto* p=allocate(79);::operator delete(p);}catch(const std::bad_alloc&){caught=true;}
        auto* p=allocate(83);::operator delete(p);hits=scope.state.hits;attempts=scope.state.attempts;}
    Check(caught&&hits==1&&attempts==2&&!allocation::active,"ordinary new hook fails once and leaves cleanup allocations available");
}
}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Calibration();const Fixture f;const Bytes sentinel{0x91,0x00,0xff,0x45};
        const char* encodeNames[]={"EncodePage.projects","EncodePage.entries","EncodePage.current"};
        const char* decodeNames[]={"DecodePage.projects","DecodePage.entries","DecodePage.current"};
        const char* validateNames[]={"ValidatePage.projects","ValidatePage.entries","ValidatePage.current"};
        for(unsigned i=0;i<3;++i){
            Scan(encodeNames[i],f.pages[i],sentinel,[](const Page& p,Bytes& out){return EncodePage(p,out);},[&](const Bytes& out){PageWire(out,f.pages[i]);});
            Scan(decodeNames[i],f.encodedPages[i],OldPage(f.index),[](const Bytes& b,Page& out){return DecodePage(b.data(),b.size(),out);},[&](const Page& out){Check(Same(out,f.pages[i]),"decoded output equals all independently authored page fields");});
            ScanValidation(validateNames[i],f.pages[i],[](const Page& p){return ValidatePage(p);});
        }
        Scan("Partition",f.index,std::vector<Page>{OldPage(f.index)},[](const Index& in,std::vector<Page>& out){return Partition(in,out);},[&](const std::vector<Page>& out){Check(Same(out,f.pages),"partition equals three independently authored sorted pages");});
        Scan("EncodeManifest",f.manifest,sentinel,[](const Manifest& in,Bytes& out){return EncodeManifest(in,out);},[&](const Bytes& out){
            constexpr char magic[16]={'M','M','S','C','a','c','h','e','I','n','d','e','x','0','0','1'};
            Check(out.size()==96+3*176&&std::equal(out.begin(),out.begin()+16,reinterpret_cast<const std::uint8_t*>(magic))&&Little(out,68,4)==3&&Little(out,72,8)==f.manifest.liveEncodedBytes,"manifest wire uses independent magic/size/count/total expectation");
            Manifest decoded;Check(DecodeManifest(out.data(),out.size(),decoded)==Result::Ok&&Same(decoded,f.manifest),"encoded manifest retains every authored reference field");});
        Scan("DecodeManifest",f.encodedManifest,OldManifest(f.manifest),[](const Bytes& in,Manifest& out){return DecodeManifest(in.data(),in.size(),out);},[&](const Manifest& out){Check(Same(out,f.manifest),"decoded manifest matches every authored field");});
        Scan("Assemble",Assembly{f.manifest,f.pages},OldIndex(f.index),[](const Assembly& in,Index& out){return Assemble(in.manifest,in.pages,out);},[&](const Index& out){Check(Same(out,f.index),"assembled output equals every independently authored Index field");});
        ScanValidation("Validate.Index",f.index,[](const Index& in){return Validate(in);});
        ScanValidation("ValidateManifest",f.manifest,[](const Manifest& in){return ValidateManifest(in);});
        std::printf("PASS cache business allocation checks=%u scans=%u injected=%llu; ordinary current-thread new/new[] single faults only, no aligned/CRT/Win32/CNG allocations, disk I/O or business Store coverage\n",checks,scans,static_cast<unsigned long long>(injected));return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache business allocation checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache business allocation unexpected exception\n");return 2;}
}
