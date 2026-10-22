// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// CPU contracts for the real cache backend. Wire decoding below is independent
// of both the cache and the optional parallel PMX preparation implementation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "../src/model/model_geometry_cache.hpp"
#include "../src/model/model_geometry_cache_load.hpp"
#include "model_geometry_reference.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <fcntl.h>
#include <io.h>
#include <share.h>
using namespace mikumikustudio;
namespace gc = model_geometry_cache;
namespace ref = geometry_test_reference;
namespace fs = std::filesystem;
namespace {
thread_local long failAfter = -1;
thread_local bool failedAllocation = false;
unsigned checks = 0, serial = 0;
const char* stage = "setup";
fs::path root;
void Check(bool ok,const char* message) {
    ++checks;if(!ok){std::fprintf(stderr,"FAIL [%s] %s error=%lu\n",stage,message,GetLastError());std::exit(1);}
}
void Write(const fs::path& path,const ref::Bytes& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    Check(bool(out),"open only the owned fixture output");
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));out.close();
    Check(bool(out),"write complete fixture bytes");
}
fs::path NewFile(const ref::Fixture& wire,const wchar_t* prefix=L"model") {
    const auto path=root/(std::wstring(prefix)+L"-"+std::to_wstring(++serial)+(wire.pmd?L".pmd":L".pmx"));
    Write(path,wire.bytes);return path;
}
struct File {
    int fd=-1;
    explicit File(const fs::path& path) {Check(!_wsopen_s(&fd,path.c_str(),_O_RDONLY|_O_BINARY,_SH_DENYNO,0)&&fd>=0,"open real fixture descriptor");}
    ~File(){if(fd>=0)Check(!_close(fd),"close only the current descriptor owner");}
};
struct Reader {
    int fd;
    int Read(void* output,unsigned count){return _read(fd,output,count);}
    bool Seek(std::int64_t at){return _lseeki64(fd,at,SEEK_SET)==at;}
    std::int64_t Tell()const{return _telli64(fd);}
};
gc::Layout Layout(const ref::Fixture& f) {
    return {f.pmd?gc::Format::pmd:gc::Format::pmx,f.vertices,static_cast<std::uint8_t>(f.uv),
        static_cast<std::uint8_t>(f.boneWidth),static_cast<std::uint8_t>(f.vertexWidth)};
}
struct Geometry {
    std::vector<mdl::PmxVertex> pmx;
    std::vector<mdl::PmdVertex> pmd;
    std::vector<std::int32_t> indices;
    bool hit=false,enabled=false,complete=false;
    std::int64_t vertexEnd=-1,indexEnd=-1;
};
Geometry Parse(Reader& reader,const ref::Fixture& f,unsigned char fill=0xcd) {
    Geometry result;Check(reader.Seek(f.vertexStart),"seek independent geometry decoder to the exact wire offset");
    if(f.pmd) {
        result.pmd.resize(f.vertices);
        if(f.vertices)std::memset(result.pmd.data(),0,f.vertices*sizeof(mdl::PmdVertex));
        for(auto& v:result.pmd)if(reader.Read(&v,38)!=38)return result;
    } else {
        result.pmx.resize(f.vertices);
        if(f.vertices)std::memset(result.pmx.data(),fill,f.vertices*sizeof(mdl::PmxVertex));
        for(auto& v:result.pmx)if(!ref::OriginalVertex(reader,f.uv,f.boneWidth,v))return result;
    }
    result.vertexEnd=reader.Tell();std::uint32_t count=0;
    if(reader.Read(&count,4)!=4||count>1000000)return result;
    result.indices.resize(count);
    for(auto& v:result.indices) {
        v=0;const auto width=f.pmd?2:f.vertexWidth;
        if(width!=1&&width!=2&&width!=4)return result;
        if(reader.Read(&v,width)!=static_cast<int>(width))return result;
    }
    result.indexEnd=reader.Tell();result.complete=true;return result;
}
void Capture(gc::Session& s,const Geometry& g,const ref::Fixture& f) {
    if(f.pmd)s.CapturePmd(g.pmd.data(),reinterpret_cast<const std::uint16_t*>(g.indices.data()),
        static_cast<std::uint32_t>(g.indices.size()),g.vertexEnd,g.indexEnd);
    else s.CapturePmx(g.pmx.data(),g.indices.data(),static_cast<std::uint32_t>(g.indices.size()),g.vertexEnd,g.indexEnd);
}
Geometry Load(const fs::path& path,const ref::Fixture& f,gc::Cache& cache,bool disabled=false,bool commit=true,unsigned char fill=0xcd) {
    File file(path);Reader r{file.fd};Check(r.Seek(f.vertexStart),"set parser position before cache lookup");
    auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache,disabled);
    Check(r.Tell()==f.vertexStart,"cache lookup never consumes or seeks the parser descriptor");
    Geometry out;out.hit=s.Hit();out.enabled=s.Enabled();
    if(!out.hit) {out=Parse(r,f,fill);out.enabled=s.Enabled();if(out.complete)Capture(s,out,f);}
    else {
        out.pmx.resize(f.pmd?0:f.vertices);out.pmd.resize(f.pmd?f.vertices:0);out.indices.resize(s.IndexCount());
        if(!out.pmx.empty())std::memset(out.pmx.data(),fill,out.pmx.size()*sizeof(mdl::PmxVertex));
        if(!out.pmd.empty())std::memset(out.pmd.data(),0,out.pmd.size()*sizeof(mdl::PmdVertex));
        const bool copied=f.pmd?s.CopyPmdVertices(out.pmd.data())&&s.CopyPmdIndices(reinterpret_cast<std::uint16_t*>(out.indices.data()),s.IndexCount()):
            s.CopyPmxVertices(out.pmx.data())&&s.CopyPmxIndices(out.indices.data(),s.IndexCount());
        if(!copied)return out;
        out.vertexEnd=s.VertexEnd();out.indexEnd=s.IndexEnd();out.complete=r.Seek(out.indexEnd);
    }
    if(out.complete){s.FinishGeometry();if(commit)s.Commit();}
    return out;
}
void Equal(const Geometry& a,const Geometry& b,unsigned uv,bool raw=true) {
    Check(a.complete&&b.complete,"both independent/cache geometry operations complete");
    Check(a.vertexEnd==b.vertexEnd&&a.indexEnd==b.indexEnd&&a.indices==b.indices,"cache preserves decoded indices and exact parser offsets");
    Check(a.pmd.size()==b.pmd.size()&&a.pmx.size()==b.pmx.size(),"cache preserves both vertex array counts");
    if(!a.pmd.empty())Check(!std::memcmp(a.pmd.data(),b.pmd.data(),a.pmd.size()*sizeof(mdl::PmdVertex)),"PMD full 40-byte host records match");
    for(std::size_t i=0;i<a.pmx.size();++i) {
        Check(ref::ParsedFields(a.pmx[i],uv)==ref::ParsedFields(b.pmx[i],uv),"PMX independently decoded parsed fields match");
        if(raw)Check(!std::memcmp(&a.pmx[i],&b.pmx[i],sizeof(mdl::PmxVertex)),"PMX unused fields retain the fresh destination's initialization");
    }
}
struct Environment {
    std::wstring old;bool present=false;
    Environment(){const auto n=GetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",nullptr,0);if(n){old.resize(n);GetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",old.data(),n);old.resize(n-1);present=true;}Set(false);}
    void Set(bool disabled){Check(SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",disabled?L"1":nullptr)!=FALSE,"change only this test process's cache override");}
    ~Environment(){SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",present?old.c_str():nullptr);}
};
FILETIME LastWrite(const fs::path& path) {WIN32_FILE_ATTRIBUTE_DATA data{};Check(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)!=FALSE,"read owned fixture mtime");return data.ftLastWriteTime;}
void RestoreWrite(const fs::path& path,FILETIME time) {
    const auto h=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(h!=INVALID_HANDLE_VALUE,"open owned file timestamp handle");Check(SetFileTime(h,nullptr,nullptr,&time)!=FALSE,"restore old mtime without changing content");CloseHandle(h);
}
struct ReadFileProbe {
    using Fn=BOOL(WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
    static inline Fn original=nullptr;
    static inline std::uint64_t calls=0,requested=0;
    ULONG_PTR* slot=nullptr;ULONG_PTR previous=0;
    static BOOL WINAPI Read(HANDLE h,LPVOID out,DWORD count,LPDWORD actual,LPOVERLAPPED offset) {
        ++calls;requested+=count;return original(h,out,count,actual,offset);
    }
    ReadFileProbe() {
        auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
        const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for(auto* d=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);directory.VirtualAddress&&d->Name&&!slot;++d) {
            if(!d->OriginalFirstThunk)continue;
            auto* names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+d->OriginalFirstThunk);
            auto* addresses=reinterpret_cast<IMAGE_THUNK_DATA*>(base+d->FirstThunk);
            for(;names->u1.AddressOfData;++names,++addresses) {
                if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
                const auto* name=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
                if(!std::strcmp(reinterpret_cast<const char*>(name->Name),"ReadFile")){slot=reinterpret_cast<ULONG_PTR*>(&addresses->u1.Function);break;}
            }
        }
        Check(slot!=nullptr,"locate this executable's actual ReadFile import");DWORD protection=0;
        Check(VirtualProtect(slot,sizeof *slot,PAGE_READWRITE,&protection)!=FALSE,"make only the owned ReadFile IAT slot writable");
        previous=*slot;original=reinterpret_cast<Fn>(previous);*slot=reinterpret_cast<ULONG_PTR>(&Read);
        DWORD ignored=0;Check(VirtualProtect(slot,sizeof *slot,protection,&ignored)!=FALSE,"restore original IAT page protection");calls=requested=0;
    }
    ~ReadFileProbe(){DWORD protection=0;Check(VirtualProtect(slot,sizeof *slot,PAGE_READWRITE,&protection)!=FALSE,"restore ReadFile IAT safely");
        *slot=previous;DWORD ignored=0;Check(VirtualProtect(slot,sizeof *slot,protection,&ignored)!=FALSE,"restore IAT page after forwarding observation");original=nullptr;}
};
void BoundedFingerprintSource() {
    const auto f=ref::Pmx(21);const auto path=NewFile(f,L"large-tail");int fd=-1;
    Check(!_wsopen_s(&fd,path.c_str(),_O_RDWR|_O_BINARY,_SH_DENYNO,0)&&fd>=0,"open only owned fixture for extending ignored tail");
    constexpr std::int64_t extent=128ll*1024*1024+1;
    Check(!_chsize_s(fd,extent)&&!_close(fd),"extend source beyond the sampling ceiling and release the writer prerequisite");fd=-1;
    Check(!_wsopen_s(&fd,path.c_str(),_O_RDONLY|_O_BINARY,_SH_DENYNO,0)&&fd>=0&&
        _lseeki64(fd,f.vertexStart,SEEK_SET)==f.vertexStart,"use a real read-only parser descriptor so writer exclusion cannot mask the source-size guard");
    gc::Cache cache;
    {
        ReadFileProbe probe;auto s=gc::Session::Begin(fd,Layout(f),f.vertexStart,&cache);
        Check(!s.Enabled()&&!s.Hit()&&!ReadFileProbe::calls&&!ReadFileProbe::requested,
            "oversized ignored tail bypasses before any actual cache ReadFile scan");
        Check(_telli64(fd)==f.vertexStart&&_filelengthi64(fd)==extent,"fingerprint bypass preserves parser cursor and complete source extent");
    }
    Check(!_close(fd)&&!cache.GetStatistics().bytes&&!cache.GetStatistics().pendingBytes,"large-tail bypass retains no cache allocation");
}
void RoundTrips() {
    gc::Cache cache;
    for(unsigned uv=0;uv<=4;++uv)for(unsigned bone:{1u,2u,4u})for(unsigned index:{1u,2u,4u}) {
        const auto f=ref::Pmx(21,uv,bone,index);const auto path=NewFile(f);
        const auto cold=Load(path,f,cache),warm=Load(path,f,cache),uncached=Load(path,f,cache,true);
        Check(cold.enabled&&!cold.hit&&warm.hit&&!uncached.enabled&&!uncached.hit,"real PMX miss/hit/disabled paths are distinguished");Equal(cold,warm,uv);Equal(cold,uncached,uv);
        // Reparse the actual file using a different untouched-byte sentinel.
        const auto fresh=Load(path,f,cache,false,true,0x5a),freshReference=Load(path,f,cache,true,true,0x5a);
        Check(fresh.hit,"different mutable initialization still uses a hit");Equal(fresh,freshReference,uv);
        auto modified=warm;modified.pmx[0].position[0]=998.f;modified.pmx[0].materialIndex=123;modified.indices[0]=123456;
        Equal(cold,Load(path,f,cache),uv);
    }
    const auto f=ref::Pmd(31);const auto path=NewFile(f);const auto cold=Load(path,f,cache),warm=Load(path,f,cache);
    Check(!cold.hit&&warm.hit,"PMD geometry uses a real warm cache lookup");Equal(cold,warm,0);
    for(std::size_t i=0;i<warm.indices.size();++i) {
        Check(warm.indices[i]==static_cast<std::int32_t>((i*7+2)%f.vertices),"PMD low16 index remains in its original four-byte CPU slot");
        Check(reinterpret_cast<const std::uint16_t*>(warm.indices.data())[i*2+1]==0,"PMD upper16 CPU index padding stays zero");
    }
    for(bool pmd:{false,true}) {
        const auto zero=pmd?ref::Pmd(0):ref::Pmx(0);const auto path0=NewFile(zero);
        const auto a=Load(path0,zero,cache),b=Load(path0,zero,cache);Check(a.complete&&b.complete,"zero-count arrays need no dereference");Equal(a,b,zero.uv);
    }
}
void LifecycleAndMalformed() {
    const auto f=ref::Pmx(21);const auto path=NewFile(f);gc::Cache cache;
    const auto baseline=Load(path,f,cache,true);
    {File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Capture(s,baseline,f);s.FinishGeometry();}
    Check(cache.GetStatistics().entries==0&&cache.GetStatistics().pendingBytes==0,"abandoned full load does not publish geometry");
    {File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Capture(s,baseline,f);s.Invalidate();s.Commit();
        Check(!s.Hit()&&s.VertexEnd()==-1&&s.IndexEnd()==-1,"invalidate retires hit-facing getters");}
    Check(cache.GetStatistics().entries==0&&!cache.GetStatistics().pendingBytes,"cancelled private candidate cannot publish");
    {File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);s.FinishGeometry();Capture(s,baseline,f);s.Commit();}
    Check(cache.GetStatistics().entries==0,"capture after geometry finish is refused");
    {File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);auto malformed=baseline;
        malformed.indexEnd++;Capture(s,malformed,f);s.Commit();}
    Check(cache.GetStatistics().entries==0&&!cache.GetStatistics().pendingBytes,"invalid wire range cannot be stored");
    const auto warm=Load(path,f,cache);Check(warm.complete,"valid fixture remains usable after rejected candidates");
    {File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Check(s.Hit(),"obtain real hit for argument contracts");
        std::array<std::int32_t,4> sentinel{11,22,33,44};const auto before=sentinel;
        Check(!s.CopyPmxIndices(sentinel.data(),3)&&sentinel==before,"index count mismatch preserves caller output");
        Check(!s.CopyPmxVertices(nullptr)&&!s.CopyPmdVertices(nullptr),"missing/wrong-format destinations are refused before copying");
        s.FinishGeometry();Check(!s.Hit()&&s.VertexEnd()==-1&&s.IndexEnd()==-1&&s.IndexCount()==0,"finish retires the hit lease and getters");}
    auto truncated=f;truncated.bytes.resize(static_cast<std::size_t>(f.vertexStart)+9);Write(path,truncated.bytes);
    const auto failed=Load(path,f,cache);Check(!failed.complete&&!failed.hit,"truncated source cannot be hidden behind a stale cached range");
    Check(!cache.GetStatistics().pendingBytes,"failed parse releases all pending charges");
    auto invalid=Layout(f);invalid.additionalUv=5;
    {File file(path);auto s=gc::Session::Begin(file.fd,invalid,f.vertexStart,&cache);Check(!s.Enabled(),"surplus UV layout bypasses without changing the normal parser");}
    Write(path,f.bytes);Environment env;env.Set(true);Check(!Load(path,f,cache).enabled,"process override bypasses an explicit cache too");
}
void ImmutableCaptureAndClear() {
    const auto f=ref::Pmx(21);const auto path=NewFile(f);gc::Cache cache;auto data=Load(path,f,cache,true);const auto before=data;
    {File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Capture(s,data,f);
        Check(cache.GetStatistics().pendingBytes>0,"private parsed fields reserve a measurable pending charge");
        data.pmx[0].position[0]=63;data.pmx[3].sdef.r0Offset[0]=123;data.pmx[0].bone[0]=197;data.indices[0]=29;
        s.FinishGeometry();s.Commit();}
    const auto restored=Load(path,f,cache);Check(restored.hit,"private snapshot commits independently of the caller's mutable arrays");Equal(before,restored,f.uv);
    cache.Clear();
    {File file(path);auto old=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Capture(old,before,f);const auto reserved=cache.GetStatistics().pendingBytes;
        Check(reserved>0,"prepare a real pending snapshot before Clear");cache.Clear();
        Check(cache.GetStatistics().pendingBytes==reserved,"Clear keeps still-owned pending memory charged");old.FinishGeometry();old.Commit();
        Check(cache.GetStatistics().entries==0&&cache.GetStatistics().stores==0&&!cache.GetStatistics().pendingBytes,"pre-Clear pending work cannot republish into the new epoch");}
    Check(!Load(path,f,cache).hit&&Load(path,f,cache).hit,"a new post-Clear session can populate and hit normally");
}
void BudgetAndRetiredLeases() {
    const auto f=ref::Pmd(31);const auto a=root/L"budget-a.pmd",b=root/L"budget-b.pmd",c=root/L"budget-c.pmd";Write(a,f.bytes);Write(b,f.bytes);Write(c,f.bytes);
    gc::Cache measured;const auto base=Load(a,f,measured);const auto charge=measured.GetStatistics().bytes;Check(charge>0,"measure actual complete entry charge");
    {gc::Cache tiny(charge-1,charge-1,1);Check(Load(a,f,tiny).complete&&tiny.GetStatistics().entries==0&&!tiny.GetStatistics().pendingBytes,"single-entry budget excludes over-limit snapshot without breaking parsing");}
    {gc::Cache none(charge*4,charge*4,0);Check(!Load(a,f,none).enabled&&!none.GetStatistics().bytes,"zero entry budget bypasses without allocating a snapshot");}
    {gc::Cache bounded(charge*2,charge,2);for(int i=0;i<6;++i){const auto path=i%3==0?a:i%3==1?b:c;Check(Load(path,f,bounded).complete,"bounded geometry parse completes");const auto s=bounded.GetStatistics();Check(s.bytes+s.pendingBytes<=charge*2&&s.entries<=2,"resident and pending bytes respect the real allocation limit");}
        Check(bounded.GetStatistics().evictions>0,"multiple real sources exercise resident LRU eviction");}
    {gc::Cache pending(charge*2,charge,2);File fa(a),fb(b),fc(c);
        auto sa=gc::Session::Begin(fa.fd,Layout(f),f.vertexStart,&pending);Capture(sa,base,f);
        auto sb=gc::Session::Begin(fb.fd,Layout(f),f.vertexStart,&pending);Capture(sb,base,f);
        auto sc=gc::Session::Begin(fc.fd,Layout(f),f.vertexStart,&pending);Capture(sc,base,f);
        const auto s=pending.GetStatistics();Check(s.pendingBytes>0&&s.bytes+s.pendingBytes<=charge*2,"simultaneous unpublished sessions cannot exceed total bytes");
        sa.FinishGeometry();sb.FinishGeometry();sc.FinishGeometry();sa.Commit();sb.Commit();sc.Commit();
        Check(!pending.GetStatistics().pendingBytes&&pending.GetStatistics().bytes<=charge*2&&pending.GetStatistics().entries<=2,"commit drains private reservations within resident and byte bounds");}
    {gc::Cache leased(charge,charge,1);Load(a,f,leased);File fa(a);auto lease=gc::Session::Begin(fa.fd,Layout(f),f.vertexStart,&leased);Check(lease.Hit(),"hold an actual cache hit lease");leased.Clear();
        Check(leased.GetStatistics().bytes==charge&&leased.GetStatistics().entries==1,"Clear keeps a leased retired allocation and entry charged");
        Check(Load(b,f,leased).complete,"new source still parses while the retired lease occupies the budget");
        Check(leased.GetStatistics().bytes+leased.GetStatistics().pendingBytes<=charge&&leased.GetStatistics().entries<=1,"retired lease prevents entry and byte budget bypass");
        lease.FinishGeometry();leased.Clear();Check(!leased.GetStatistics().bytes&&!leased.GetStatistics().pendingBytes&&!leased.GetStatistics().entries,"last lease release allows retired storage to be reclaimed");}
}
void SourceChangesAndMapping() {
    auto f=ref::Pmx(21);const auto path=NewFile(f);gc::Cache cache;Load(path,f,cache);Check(Load(path,f,cache).hit,"seed source mutation case");const auto oldTime=LastWrite(path);
    const float x=91.25f;std::memcpy(f.bytes.data()+f.vertexStart,&x,4);Write(path,f.bytes);RestoreWrite(path,oldTime);
    const auto changed=Load(path,f,cache);Check(changed.complete&&!changed.hit&&changed.pmx[0].position[0]==x,"same length and restored mtime still invalidate modified content");
    Check(Load(path,f,cache).hit,"new content can be cached");const auto replacement=NewFile(f,L"replacement");RestoreWrite(replacement,oldTime);
    Check(MoveFileExW(replacement.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE,"replace only the owned source with a distinct file identity");
    Check(!Load(path,f,cache).hit,"same path and same bytes from a replacement file miss identity lookup");
    {
        const auto writer=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        Check(writer!=INVALID_HANDLE_VALUE,"hold a real independently open writer");
        const auto bypass=Load(path,f,cache);Check(bypass.complete&&!bypass.enabled&&!bypass.hit,"an existing writer bypasses cache while ordinary geometry parsing still succeeds");
        CloseHandle(writer);
    }
    // A writable section can outlive its original handle. Windows/filesystems
    // differ in whether that surviving section prevents a new read-only guard;
    // exercise the actual OS outcome rather than requiring weaker sharing.
    HANDLE write=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(write!=INVALID_HANDLE_VALUE,"open owned source to establish a preexisting writable mapping");
    HANDLE mapping=CreateFileMappingW(write,nullptr,PAGE_READWRITE,0,0,nullptr);Check(mapping!=nullptr,"create actual writable file mapping");CloseHandle(write);
    auto* view=static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,0));Check(view!=nullptr,"retain mapping after closing original writer");
    {
        const auto observed=Load(path,f,cache);const auto stored=cache.GetStatistics().stores;
        File file(path);auto lease=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);
        if(lease.Enabled())Check(lease.Hit()&&observed.enabled&&observed.complete,"an OS-allowed stable mapping guard reaches a seeded real hit");
        else Check(!lease.Hit()&&!observed.enabled&&observed.complete&&!cache.GetStatistics().pendingBytes,
            "a surviving writable section may correctly refuse the guard and select ordinary parsing");
        const float mapped=-27.5f;std::memcpy(view+f.vertexStart,&mapped,4);Check(FlushViewOfFile(view,0)!=FALSE,"complete deterministic mapped mutation before copying");
        std::vector<mdl::PmxVertex> out(f.vertices);Check(!lease.CopyPmxVertices(out.data()),"no mapping branch supplies cached geometry after the completed write");
        Check(cache.GetStatistics().stores==stored&&!cache.GetStatistics().pendingBytes,
            "mapping observation and a refused copy never publish a candidate");
        std::printf("DIAG mapping guard=%s; metadata/content detection is not attributed solely to SHA\n",lease.Enabled()?"accepted-copy-refused":"OS-sharing-bypass");
        lease.Invalidate();lease.Commit();Check(!lease.Hit(),"mapping-invalidated lease cannot continue supplying old geometry");
    }
    UnmapViewOfFile(view);CloseHandle(mapping);const auto after=Load(path,f,cache);Check(after.complete&&!after.hit&&after.pmx[0].position[0]==-27.5f,"subsequent parsing observes the completed mapped write");
    cache.Clear();const auto raw=Load(path,f,cache,true);
    {File file(path);auto pending=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Capture(pending,raw,f);pending.FinishGeometry();
        auto later=f;const float z=17.f;std::memcpy(later.bytes.data()+f.vertexStart,&z,4);Write(path,later.bytes);pending.Commit();}
    Check(cache.GetStatistics().entries==0&&!cache.GetStatistics().pendingBytes,"source replacement after geometry finish but before commit prevents publication");
}
struct SourceMetadata {
    BY_HANDLE_FILE_INFORMATION file{};
    FILE_BASIC_INFO basic{};
    std::wstring normalizedPath;
};
SourceMetadata ReadMetadata(const fs::path& path) {
    SourceMetadata result;
    const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(handle!=INVALID_HANDLE_VALUE,"open the owned source only for real metadata observation");
    Check(GetFileInformationByHandle(handle,&result.file)&&
        GetFileInformationByHandleEx(handle,FileBasicInfo,&result.basic,sizeof result.basic),
        "read real source identity including creation, write and change times");
    wchar_t normalized[32768]{};
    const auto count=GetFinalPathNameByHandleW(handle,normalized,static_cast<DWORD>(std::size(normalized)),FILE_NAME_NORMALIZED);
    Check(count&&count<std::size(normalized),"read normalized path from the same actual source handle");
    result.normalizedPath.assign(normalized,count);Check(CloseHandle(handle)!=FALSE,"close metadata-only observation handle");return result;
}
bool SameFingerprintMetadata(const SourceMetadata& a,const SourceMetadata& b) {
    // These are the exact metadata fields used by the production Identity.
    // Access time is intentionally excluded there and may advance on a read.
    return a.normalizedPath==b.normalizedPath&&a.file.dwVolumeSerialNumber==b.file.dwVolumeSerialNumber&&
        a.file.nFileIndexHigh==b.file.nFileIndexHigh&&a.file.nFileIndexLow==b.file.nFileIndexLow&&
        a.file.nFileSizeHigh==b.file.nFileSizeHigh&&a.file.nFileSizeLow==b.file.nFileSizeLow&&
        a.basic.CreationTime.QuadPart==b.basic.CreationTime.QuadPart&&
        a.basic.LastWriteTime.QuadPart==b.basic.LastWriteTime.QuadPart&&a.basic.ChangeTime.QuadPart==b.basic.ChangeTime.QuadPart;
}
void RestoredMetadataContentChange() {
    const auto fixture=ref::Pmx(21);const auto path=NewFile(fixture,L"restored-metadata");gc::Cache cache;
    Check(Load(path,fixture,cache).complete&&Load(path,fixture,cache).hit,"seed and confirm a real cache entry before metadata restoration");
    const auto before=ReadMetadata(path);const auto initial=cache.GetStatistics();
    const auto writer=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(writer!=INVALID_HANDLE_VALUE,"open only the owned source for a four-byte content change");
    LARGE_INTEGER position{};position.QuadPart=fixture.vertexStart;const float changed=123.75f;DWORD actual=0;
    Check(SetFilePointerEx(writer,position,nullptr,FILE_BEGIN)&&WriteFile(writer,&changed,sizeof changed,&actual,nullptr)&&
        actual==sizeof changed&&FlushFileBuffers(writer),"complete a real geometry write without changing file size or identity");
    Check(CloseHandle(writer)!=FALSE,"close the payload writer before attempting timestamp restoration");
    // Use a separate attributes-only handle after the writing handle closed,
    // so delayed close-time write metadata is not mistaken for restored state.
    const auto attributes=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    BOOL restored=FALSE;DWORD restoreError=ERROR_SUCCESS;
    if(attributes!=INVALID_HANDLE_VALUE) {
        auto original=before.basic;restored=SetFileInformationByHandle(attributes,FileBasicInfo,&original,sizeof original);
        restoreError=restored?ERROR_SUCCESS:GetLastError();Check(CloseHandle(attributes)!=FALSE,"close attributes handle before verifying restoration");
    } else restoreError=GetLastError();
    const auto after=ReadMetadata(path);const bool identical=SameFingerprintMetadata(before,after);
    {
        File file(path);Check(_lseeki64(file.fd,fixture.vertexStart,SEEK_SET)==fixture.vertexStart,"position read-only descriptor after all writers closed");
        float bytes=0;Check(_read(file.fd,&bytes,sizeof bytes)==sizeof bytes&&bytes==changed,
            "real source bytes differ from the cached original after metadata restoration attempt");
        Check(_lseeki64(file.fd,fixture.vertexStart,SEEK_SET)==fixture.vertexStart,"restore parser position before actual cache Begin");
        if(identical) {
            ReadFileProbe probe;auto session=gc::Session::Begin(file.fd,Layout(fixture),fixture.vertexStart,&cache);
            Check(session.Enabled()&&!session.Hit()&&ReadFileProbe::calls>0&&ReadFileProbe::requested>0,
                "different real bytes with identical fingerprint metadata require an actual scan and reject the old hit");
            const auto observed=cache.GetStatistics();Check(observed.invalidations==initial.invalidations+1&&observed.misses==initial.misses+1&&
                observed.stores==initial.stores&&!observed.pendingBytes&&_telli64(file.fd)==fixture.vertexStart,
                "content mismatch invalidates without publication or parser-descriptor movement");
            session.Invalidate();
            std::printf("PROOF identical source identity/creation/write/change metadata with different geometry: real scan rejected old hit; SetFileInformation=%d\n",int(restored));
        } else {
            std::printf("LIMIT filesystem did not restore all fingerprint metadata; no SHA-only coverage claimed: set=%d error=%lu creation=%d write=%d change=%d identity=%d\n",
                int(restored),restoreError,int(before.basic.CreationTime.QuadPart==after.basic.CreationTime.QuadPart),
                int(before.basic.LastWriteTime.QuadPart==after.basic.LastWriteTime.QuadPart),int(before.basic.ChangeTime.QuadPart==after.basic.ChangeTime.QuadPart),
                int(before.file.nFileIndexHigh==after.file.nFileIndexHigh&&before.file.nFileIndexLow==after.file.nFileIndexLow));
        }
    }
    const auto parsed=Load(path,fixture,cache);Check(parsed.complete&&!parsed.hit&&parsed.pmx[0].position[0]==changed,
        "ordinary parsing retrieves changed geometry after either explicit metadata-restoration outcome");
    Equal(parsed,Load(path,fixture,cache),fixture.uv);
}
void AllocationAndFreshHitCopy() {
    const auto f=ref::Pmx(21);const auto path=NewFile(f);gc::Cache referenceCache;const auto data=Load(path,f,referenceCache,true);
    for(unsigned operation=0;operation<3;++operation) {
        unsigned exercised=0;bool reachedEnd=false;
        for(long budget=0;budget<128;++budget) {
            gc::Cache cache;File file(path);gc::Session s;
            if(operation!=0)s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);
            if(operation==2)Capture(s,data,f);
            failedAllocation=false;failAfter=budget;
            if(operation==0)s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);
            else if(operation==1)Capture(s,data,f);
            else {s.FinishGeometry();s.Commit();}
            failAfter=-1;const bool tripped=failedAllocation;exercised+=tripped;
            if(operation!=2)s.Invalidate();
            const auto stats=cache.GetStatistics();Check(stats.entries<=1&&stats.bytes+stats.pendingBytes<=256u*1024*1024,"every injected allocation refusal remains within backend bounds");
            if(tripped&&operation==2)Check(!stats.entries&&!stats.stores,"failed final publication does not count a stored entry");
            if(!tripped){reachedEnd=true;break;}
        }
        Check(reachedEnd&&exercised>0,"allocation sweep reaches each stage's first complete execution");
    }
    gc::Cache cache;Load(path,f,cache);File file(path);auto s=gc::Session::Begin(file.fd,Layout(f),f.vertexStart,&cache);Check(s.Hit(),"seed actual hit restoration helper");
    for(long budget:{0L,1L}) {failedAllocation=false;failAfter=budget;gc::HitCopy<mdl::PmxVertex> copy(s,f.vertices);failAfter=-1;
        Check(failedAllocation&&!copy.Ready()&&!copy.ReleaseVertices()&&!copy.ReleaseIndices(),"failure in either fresh-array allocation leaves no half-ready HitCopy");}
    gc::HitCopy<mdl::PmxVertex> first(s,f.vertices),second(s,f.vertices);Check(first.Ready()&&second.Ready(),"fresh helper restores both complete arrays before handoff");
    std::unique_ptr<mdl::PmxVertex,gc::GeometryDelete> av(first.ReleaseVertices()),bv(second.ReleaseVertices());
    std::unique_ptr<std::int32_t,gc::GeometryDelete> ai(first.ReleaseIndices()),bi(second.ReleaseIndices());
    Check(av&&bv&&ai&&bi&&av.get()!=bv.get()&&ai.get()!=bi.get(),"every HitCopy owns independently allocated mutable arrays");
    av.get()[0].position[0]=1234;ai.get()[0]=1234;
    Check(ref::ParsedFields(bv.get()[0],f.uv)==ref::ParsedFields(data.pmx[0],f.uv)&&bi.get()[0]==data.indices[0],"mutating one helper result cannot mutate another or the snapshot");
    first.Reset();second.Reset();s.FinishGeometry();
}
void Run(const char* name,void(*test)()){stage=name;std::printf("STAGE %s\n",name);test();Check(failAfter==-1,"no allocation injector escapes its group");std::printf("PASS %s\n",name);}
}
void* operator new(std::size_t bytes){if(failAfter==0){failedAllocation=true;throw std::bad_alloc();}if(failAfter>0)--failAfter;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int wmain(int argc,wchar_t** argv) {
    static_assert(!std::is_copy_constructible_v<gc::Cache>&&!std::is_copy_constructible_v<gc::Session>);
    std::setvbuf(stdout,nullptr,_IONBF,0);Check(argc==2,"provide an isolated evidence directory");
    root=fs::absolute(argv[1])/(L"cpu-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Check(root.native().size()+48<MAX_PATH&&fs::create_directories(root),"create fresh private fixture directory");Environment environment;
    Run("PMX-layout-and-PMD-stride",RoundTrips);Run("lifecycle-and-malformed",LifecycleAndMalformed);
    Run("immutable-capture-and-clear-epoch",ImmutableCaptureAndClear);Run("budget-and-retired-lease",BudgetAndRetiredLeases);
    Run("source-identity-content-and-existing-mapping",SourceChangesAndMapping);Run("allocation-failure-and-fresh-hit-copy",AllocationAndFreshHitCopy);
    Run("bounded-whole-source-fingerprint",BoundedFingerprintSource);
    Run("restored-fingerprint-metadata-if-supported",RestoredMetadataContentChange);
    std::printf("PASS geometry cache CPU contracts: %u checks\n",checks);return 0;
}
