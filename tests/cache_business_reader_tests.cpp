// MAINTENANCE: regression-keep. Real catalog/page files in a new owned root.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/io/cache_business_reader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace allocation {
struct State {std::size_t failAt,attempts=0,hits=0;};
thread_local State* active=nullptr;
struct Scope {
    State state;
    explicit Scope(std::size_t n):state{n}{active=&state;}
    ~Scope(){active=nullptr;}
};
}
void* operator new(std::size_t n){
    if(auto* a=allocation::active){if(a->attempts++==a->failAt){++a->hits;throw std::bad_alloc();}}
    if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

namespace {
namespace fs=std::filesystem;
namespace b=mikumikustudio::cache_business;
namespace f=mikumikustudio::cache_files;
namespace c=mikumikustudio::cache_catalog;
using Bytes=std::vector<std::uint8_t>;
using Result=b::ReadResult;
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
};
void NewDirectory(const fs::path& path){
    Check(path.is_absolute()&&path==path.lexically_normal()&&path!=path.root_path(),"new normalized absolute directory");
    Check(fs::is_directory(path.parent_path())&&CreateDirectoryW(path.c_str(),nullptr),"claim fresh owned directory");
}
Bytes Raw(const fs::path& path){
    std::ifstream in(path,std::ios::binary);Check(bool(in),"read owned raw fixture");
    return Bytes(std::istreambuf_iterator<char>(in),{});
}
void Write(const fs::path& path,const Bytes& bytes,DWORD creation=OPEN_EXISTING){
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,creation,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"open only owned fixture for authored mutation");
    DWORD written=0;
    Check(WriteFile(h.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&
          SetEndOfFile(h.value)&&FlushFileBuffers(h.value),"write and flush authored fixture");
}
bool Exclusive(const fs::path& path){
    Handle h(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    return h.value!=INVALID_HANDLE_VALUE;
}
std::uint64_t FileIndex(const fs::path& path){
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    Check(h.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.value,&info),"independent actual file identity query");
    return (std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
}
b::GenerationId Generation(const std::wstring& name){
    b::GenerationId id{};const auto nibble=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};
    for(unsigned i=0;i<16;++i)id[i]=std::uint8_t(nibble(name[i*2])*16+nibble(name[i*2+1]));
    return id;
}
enum class Mode {Normal,Empty,ManifestOwner,ManifestMagic,ManifestVersion,ManifestLimit,PageVersion,
    PageOwner,PageOrdinal,PageTrailing,SplitProjects,CrossPagePath,ForeignOwner,WrongGeneration};
struct Fixture {
    fs::path path;
    f::Directory pages,payloads;
    f::Owner owner{0x53,0x31,0x09};
    b::Index index;
    b::Manifest manifest;
    c::Snapshot source;
    explicit Fixture(const fs::path& root,const wchar_t* name,Mode mode=Mode::Normal):path(root/name){
        NewDirectory(path);
        Check(f::Directory::Open(path/L"pages",true,pages)==f::Result::Ok,"open actual page directory");
        Check(f::Directory::Open(path/L"payloads",true,payloads)==f::Result::Ok,"open authored payload directory");
        index.storeOwner=owner;index.maintenanceEpoch=7;index.nextProjectId=3;
        b::Project p;p.id=1;p.normalizedPath=u"C:\\reader\\one.pmm";p.displayPath=u"c:\\READER\\One.pmm";p.pinned=true;p.lastOpened=91;
        b::Project q;q.id=2;q.normalizedPath=u"C:\\reader\\two.pmm";q.displayPath=q.normalizedPath;q.lastOpened=92;
        index.projects={p,q};
        for(unsigned k=0;k<2;++k){
            f::Spec spec;spec.owner=owner;spec.kind=k?f::Kind::Texture:f::Kind::Effect;spec.key[0]=std::uint8_t(11+k);
            Bytes bytes{0,0xff,0x7f,std::uint8_t(k)};b::Entry e;
            Check(payloads.Create(spec,bytes.data(),bytes.size(),e.blob)==f::Result::Ok,"author actual payload identity");
            e.consumerFormat=5+k;e.lastUse=123+k;e.owners={1,2};index.entries.push_back(e);
            b::Current item;item.key.kind=spec.kind;item.key.key=spec.key;item.key.consumerFormat=e.consumerFormat;
            item.generation=Generation(e.blob.name);index.current.push_back(item);
        }
        std::vector<b::Page> list(3);
        for(unsigned k=0;k<3;++k){list[k].storeOwner=owner;list[k].kind=static_cast<b::PageKind>(k+1);}
        list[0].projects=index.projects;list[1].entries=index.entries;list[2].current=index.current;
        if(mode==Mode::Empty){list.clear();index.projects.clear();index.entries.clear();index.current.clear();}
        if(mode==Mode::CrossPagePath||mode==Mode::SplitProjects){
            list[0].projects.resize(1);b::Page second=list[0];second.ordinal=1;second.projects[0].id=2;
            if(mode==Mode::SplitProjects)second.projects[0]=q;
            list.insert(list.begin()+1,std::move(second));
        }
        if(mode==Mode::ForeignOwner)list[1].entries[0].owners={1,99};
        if(mode==Mode::WrongGeneration)list[2].current[0].generation.fill(0x55);
        manifest.storeOwner=owner;manifest.maintenanceEpoch=7;manifest.nextProjectId=3;
        manifest.liveEncodedBytes=96+176*list.size();
        for(std::size_t n=0;n<list.size();++n){
            auto page=list[n];b::PageRef ref;ref.kind=page.kind;ref.ordinal=page.ordinal;
            ref.recordCount=static_cast<std::uint32_t>(page.projects.size()+page.entries.size()+page.current.size());
            if(page.kind==b::PageKind::Projects)manifest.projectCount+=ref.recordCount;
            else if(page.kind==b::PageKind::Entries)manifest.entryCount+=ref.recordCount;
            else manifest.currentCount+=ref.recordCount;
            if(n==0&&mode==Mode::PageOwner)page.storeOwner[0]^=0x40;
            if(n==0&&mode==Mode::PageOrdinal)page.ordinal=7;
            Bytes encoded;Check(b::EncodePage(page,encoded)==b::Result::Ok,"independent authored page is locally valid");
            if(n==0&&mode==Mode::PageVersion)encoded[16]=2;
            if(n==0&&mode==Mode::PageTrailing)encoded.push_back(0);
            f::Spec spec;spec.owner=owner;spec.kind=f::Kind::IndexPage;spec.key=b::PageKey;
            Check(pages.Create(spec,encoded.data(),encoded.size(),ref.blob)==f::Result::Ok,"write actual page envelope");
            manifest.liveEncodedBytes+=encoded.size();manifest.pages.push_back(std::move(ref));
        }
        if(mode==Mode::ManifestOwner){manifest.storeOwner[0]^=0x40;for(auto& ref:manifest.pages)ref.blob.spec.owner=manifest.storeOwner;}
        Bytes encoded;Check(b::EncodeManifest(manifest,encoded)==b::Result::Ok,"encode independently assembled manifest references");
        if(mode==Mode::ManifestMagic)encoded[0]^=0x20;
        if(mode==Mode::ManifestVersion)encoded[16]=2;
        if(mode==Mode::ManifestLimit){encoded[56]=0x11;encoded[57]=0x27;encoded[58]=encoded[59]=0;}
        c::Store store(path/L"catalog");
        Check(store.InitializeOwned(owner,encoded,source)==c::Result::Ok,"publish manifest through real catalog");
        c::Snapshot reread;Check(store.Read(reread)==c::Result::Ok&&reread.Bytes()==encoded,"independently reopen actual manifest publication");
        source=std::move(reread);
    }
};
Bytes EncodedValue(const b::Index& index){
    std::vector<b::Page> pages;Check(b::Partition(index,pages)==b::Result::Ok,"encode valid value for comparison");
    Bytes result;
    for(const auto& page:pages){Bytes bytes;Check(b::EncodePage(page,bytes)==b::Result::Ok,"encode comparison page");result.insert(result.end(),bytes.begin(),bytes.end());}
    return result;
}
struct Sentinel {
    Fixture fixture;
    b::PublishedIndex output;
    Bytes description,value;
    explicit Sentinel(const fs::path& root):fixture(root,L"sentinel"){
        Check(b::LoadPublishedIndex(fixture.source,fixture.pages,output)==Result::Ok,"load output sentinel from actual catalog/pages");
        Check(b::EncodeManifest(output.Description(),description)==b::Result::Ok,"capture full sentinel manifest");
        value=EncodedValue(output.Value());
    }
    void Unchanged(){
        Bytes actual;Check(output.IsValid()&&b::EncodeManifest(output.Description(),actual)==b::Result::Ok&&actual==description,"failure preserves full published description");
        Check(output.Source().IsValid()&&output.Source().Bytes()==fixture.source.Bytes()&&output.Source().StoreId()==fixture.owner&&
              output.Source().Revision()==fixture.source.Revision(),"failure preserves catalog source token and revision");
        Check(EncodedValue(output.Value())==value&&output.Value().maintenanceEpoch==7&&output.Value().nextProjectId==3,"failure preserves full assembled value");
        for(const auto& ref:fixture.manifest.pages)
            Check(fixture.pages.Remove(ref.blob)==f::Result::Busy,"failure retains every previous output page lease");
    }
};
void Success(const fs::path& root){
    Fixture a(root,L"success");b::PublishedIndex loaded;
    Check(!loaded.IsValid()&&!loaded.Source().IsValid()&&loaded.Value().projects.empty(),"default reader output is inert");
    Check(b::LoadPublishedIndex(a.source,a.pages,loaded)==Result::Ok,"read all three actual page kinds");
    Check(EncodedValue(loaded.Value())==EncodedValue(a.index)&&loaded.Value().storeOwner==a.owner&&loaded.Value().maintenanceEpoch==7,"actual projects entries and current mappings match authored data");
    const auto& value=loaded.Value();
    Check(value.projects.size()==2&&value.entries.size()==2&&value.current.size()==2&&value.nextProjectId==3,"independent authored table counts");
    Check(value.projects[0].id==1&&value.projects[0].pinned&&value.projects[0].lastOpened==91&&value.projects[0].normalizedPath==u"C:\\reader\\one.pmm"&&
          value.projects[0].displayPath==u"c:\\READER\\One.pmm"&&value.projects[1].id==2&&!value.projects[1].pinned&&value.projects[1].lastOpened==92,"direct project field and spelling checks");
    for(unsigned k=0;k<2;++k){
        Check(value.entries[k].consumerFormat==5+k&&value.entries[k].lastUse==123+k&&value.entries[k].availability==b::Availability::Valid&&
              value.entries[k].owners==std::vector<b::ProjectId>({1,2})&&value.entries[k].blob.name==a.index.entries[k].blob.name&&
              value.entries[k].blob.identity.fileIndex==a.index.entries[k].blob.identity.fileIndex&&value.entries[k].blob.payloadBytes==4,"direct physical entry fields");
        Check(value.current[k].key.kind==(k?f::Kind::Texture:f::Kind::Effect)&&value.current[k].key.consumerFormat==5+k&&
              value.current[k].key.key[0]==11+k&&value.current[k].generation==Generation(a.index.entries[k].blob.name),"direct current key and generation fields");
    }
    for(const auto& ref:a.manifest.pages)Check(a.pages.Remove(ref.blob)==f::Result::Busy,"successful output holds every actual page lease");
    b::PublishedIndex moved(std::move(loaded));Check(moved.IsValid()&&!loaded.IsValid(),"move construction transfers leases");
    Fixture empty(root,L"empty",Mode::Empty);b::PublishedIndex replacement;
    Check(b::LoadPublishedIndex(empty.source,empty.pages,replacement)==Result::Ok&&replacement.Value().projects.empty(),"actual empty manifest loads without empty pages");
    replacement=std::move(moved);Check(replacement.IsValid()&&!moved.IsValid(),"move assignment transfers same publication");
    // A retained snapshot is intentionally not a claim to be the latest head.
    c::Store catalog(a.path/L"catalog");c::Snapshot newer;
    Check(catalog.Commit(replacement.Source(),empty.source.Bytes(),newer)==c::Result::Ok,"reader preserves genuine private CAS source");
    Check(replacement.Source().Revision()==1&&replacement.Value().projects.size()==2,"new catalog head does not mutate retained old reader");
    Check(b::LoadPublishedIndex(newer,a.pages,replacement)==Result::Ok&&replacement.Source().Revision()==2&&replacement.Value().projects.empty(),"explicit newer load replaces old snapshot");
    for(const auto& ref:a.manifest.pages)Check(a.pages.Remove(ref.blob)==f::Result::DeletePending,"successful replacement releases old page leases");
    Check(loaded.Value().projects.empty()&&moved.Description().pages.empty(),"moved-from getters stay inert");
    Fixture split(root,L"split-projects",Mode::SplitProjects);b::PublishedIndex multiple;
    Check(b::LoadPublishedIndex(split.source,split.pages,multiple)==Result::Ok&&multiple.Description().pages.size()==4&&
          multiple.Value().projects.size()==2&&multiple.Value().projects[1].normalizedPath==u"C:\\reader\\two.pmm","legal multiple pages of the same kind assemble from real files");
    for(const auto& ref:split.manifest.pages)Check(split.pages.Remove(ref.blob)==f::Result::Busy,"every same-kind page remains leased");
}
void Failures(const fs::path& root){
    Sentinel sentinel(root);c::Snapshot invalid;f::Directory invalidPages;
    Check(b::LoadPublishedIndex(invalid,sentinel.fixture.pages,sentinel.output)==Result::Invalid,"reject invalid catalog source");sentinel.Unchanged();
    Check(b::LoadPublishedIndex(sentinel.fixture.source,invalidPages,sentinel.output)==Result::Invalid,"reject invalid page directory");sentinel.Unchanged();
    const Mode modes[]={Mode::ManifestOwner,Mode::ManifestMagic,Mode::ManifestVersion,Mode::ManifestLimit,Mode::PageVersion,Mode::PageOwner,
                        Mode::PageOrdinal,Mode::PageTrailing,Mode::CrossPagePath,Mode::ForeignOwner,Mode::WrongGeneration};
    unsigned ordinal=0;
    for(auto mode:modes){
        Fixture fixture(root,(L"invalid-"+std::to_wstring(ordinal++)).c_str(),mode);
        const auto expected=(mode==Mode::ManifestVersion||mode==Mode::PageVersion)?Result::Unsupported:
                            mode==Mode::ManifestLimit?Result::Limit:Result::RecoveryRequired;
        Check(b::LoadPublishedIndex(fixture.source,fixture.pages,sentinel.output)==expected,"reject manifest or page semantic failure without old-head fallback");sentinel.Unchanged();
        for(const auto& ref:fixture.manifest.pages)Check(Exclusive(fixture.path/L"pages"/ref.blob.name),"failed load releases every candidate page lease");
    }
    Fixture missing(root,L"missing");
    Check(missing.pages.Remove(missing.manifest.pages[1].blob)==f::Result::DeletePending,"remove only authored candidate page");
    Check(b::LoadPublishedIndex(missing.source,missing.pages,sentinel.output)==Result::RecoveryRequired,"missing referenced page is recovery required");sentinel.Unchanged();
    Fixture corrupt(root,L"corrupt");const auto corruptPath=corrupt.path/L"pages"/corrupt.manifest.pages[1].blob.name;
    auto bytes=Raw(corruptPath);bytes.back()^=0x40;Write(corruptPath,bytes);
    Check(b::LoadPublishedIndex(corrupt.source,corrupt.pages,sentinel.output)==Result::RecoveryRequired,"actual digest corruption rejects whole publication");sentinel.Unchanged();
    Fixture busy(root,L"busy");
    {
        Handle h(CreateFileW((busy.path/L"pages"/busy.manifest.pages[1].blob.name).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));
        Check(h.value!=INVALID_HANDLE_VALUE,"hold exclusive actual target page");
        Check(b::LoadPublishedIndex(busy.source,busy.pages,sentinel.output)==Result::Busy,"sharing conflict remains Busy");sentinel.Unchanged();
    }
    b::PublishedIndex retry;Check(b::LoadPublishedIndex(busy.source,busy.pages,retry)==Result::Ok,"busy page succeeds after release");retry={};
    Fixture replaced(root,L"replaced");const auto info=replaced.manifest.pages[0].blob;
    const auto original=replaced.path/L"pages"/info.name,temporary=replaced.path/L"replacement.tmp",retained=replaced.path/L"original.retained";
    Write(temporary,Raw(original),CREATE_NEW);
    const auto originalId=FileIndex(original),replacementId=FileIndex(temporary);
    Check(originalId==info.identity.fileIndex&&originalId!=replacementId,"two simultaneously existing files have distinct identities");
    replaced.pages={};replaced.payloads={};
    Check(MoveFileExW(original.c_str(),retained.c_str(),0)&&MoveFileExW(temporary.c_str(),original.c_str(),0),"same bytes at old name now have different live file identity");
    Check(f::Directory::Open(replaced.path/L"pages",false,replaced.pages)==f::Result::Ok,"reopen after controlled namespace replacement");
    Check(FileIndex(original)==replacementId&&FileIndex(retained)==originalId,"actual replacement retains both calibrated identities");
    Check(b::LoadPublishedIndex(replaced.source,replaced.pages,sentinel.output)==Result::RecoveryRequired,"same filename and bytes cannot replace referenced file identity");sentinel.Unchanged();
    Fixture payload(root,L"payload-boundary");
    for(const auto& e:payload.index.entries)Check(payload.payloads.Remove(e.blob)==f::Result::DeletePending,"remove authored payload outside reader contract");
    b::PublishedIndex metadata;Check(b::LoadPublishedIndex(payload.source,payload.pages,metadata)==Result::Ok,"reader loads metadata but does not claim live effect or texture payload validation");
}
void Allocations(const fs::path& root){
    NewDirectory(root/L"allocation");Sentinel sentinel(root/L"allocation");Fixture candidate(root/L"allocation",L"candidate");
    std::size_t count=0;
    {
        b::PublishedIndex output;Result result;
        {allocation::Scope scope((std::numeric_limits<std::size_t>::max)());result=b::LoadPublishedIndex(candidate.source,candidate.pages,output);count=scope.state.attempts;}
        Check(result==Result::Ok&&count>10&&count<1024,"measure actual successful load allocation path");
    }
    for(std::size_t n=0;n<count;++n){
        Result result;std::size_t hits=0;
        {allocation::Scope scope(n);result=b::LoadPublishedIndex(candidate.source,candidate.pages,sentinel.output);hits=scope.state.hits;}
        Check(hits==1&&(result==Result::Io||result==Result::AllocationFailure),"single ordinary new failure is reported without partial output");
        sentinel.Unchanged();
        for(const auto& ref:candidate.manifest.pages)Check(Exclusive(candidate.path/L"pages"/ref.blob.name),"allocation failure releases all candidate leases");
    }
    Check(b::LoadPublishedIndex(candidate.source,candidate.pages,sentinel.output)==Result::Ok,"healthy load succeeds after all allocation faults");
    std::printf("ALLOCATIONS loadOrdinals=%llu\n",static_cast<unsigned long long>(count));
    // Calibrate the alias path independently; do not reuse another source's
    // count or overwrite the sentinel before testing its preservation.
    NewDirectory(root/L"allocation"/L"alias-parent");
    Sentinel alias(root/L"allocation"/L"alias-parent");
    std::size_t aliasCount=0;Result aliasResult;
    {allocation::Scope scope((std::numeric_limits<std::size_t>::max)());
        aliasResult=b::LoadPublishedIndex(alias.output.Source(),alias.fixture.pages,alias.output);aliasCount=scope.state.attempts;}
    Check(aliasResult==Result::Ok&&aliasCount>10&&aliasCount<1024,"actual source/output alias succeeds and has independent allocation calibration");
    alias.Unchanged();
    for(std::size_t n=0;n<aliasCount;++n){
        std::size_t hits=0;
        {allocation::Scope scope(n);aliasResult=b::LoadPublishedIndex(alias.output.Source(),alias.fixture.pages,alias.output);hits=scope.state.hits;}
        Check(hits==1&&(aliasResult==Result::Io||aliasResult==Result::AllocationFailure),"alias allocation failure preserves live source and output");
        alias.Unchanged();
    }
    Check(b::LoadPublishedIndex(alias.output.Source(),alias.fixture.pages,alias.output)==Result::Ok,"source/output alias succeeds after failure scan");
    alias.output={};
    for(const auto& ref:alias.fixture.manifest.pages)
        Check(alias.fixture.pages.Remove(ref.blob)==f::Result::DeletePending,"alias success and failures leak no page handles after output reset");
    std::printf("ALLOCATIONS aliasOrdinals=%llu\n",static_cast<unsigned long long>(aliasCount));
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        static_assert(!std::is_copy_constructible_v<b::PublishedIndex>);
        static_assert(std::is_nothrow_move_constructible_v<b::PublishedIndex>);
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIR");
        const fs::path root(argv[2]);NewDirectory(root);Success(root);Failures(root);Allocations(root);
        std::printf("PASS business reader checks=%u root=%ls\n",checks,root.c_str());
        std::printf("BOUNDARY page leases only; no manifest lease, latest-head guarantee, payload validation, publisher, capacity grant, UI or power-loss claim\n");return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL business reader checks=%u: %s\n",checks,e.what());return 1;}
}
