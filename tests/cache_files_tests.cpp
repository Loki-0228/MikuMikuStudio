// MAINTENANCE: regression-keep
// Real CPU/Win32 file tests in a new, caller-selected directory. No cache sweep,
// application startup, GPU, UI, DOS-drive remap or recursive cleanup.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include "mikumikustudio/cache_files.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace allocation {
thread_local long remaining = -1;
thread_local unsigned hits = 0;
struct Fail { explicit Fail(long count=0) { hits=0; remaining=count; } ~Fail() { remaining=-1; } };
}
void* operator new(std::size_t size) {
    if (allocation::remaining==0) { ++allocation::hits; throw std::bad_alloc(); }
    if (allocation::remaining>0) --allocation::remaining;
    if (void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
using files::Result;
using Bytes=std::vector<std::uint8_t>;
namespace {
unsigned checks=0, skips=0;
constexpr char Marker[]="MMS cache files worker v1\n";
void Check(bool ok,const char* why) { ++checks; if(!ok) throw std::runtime_error(why); }
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    Handle(const Handle&)=delete;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
};
void Absolute(const fs::path& p) {
    Check(p.is_absolute()&&p.has_filename()&&p!=p.root_path()&&p==p.lexically_normal(),"absolute normalized non-root fixture path required");
}
void NewDirectory(const fs::path& p) {
    Absolute(p); Check(fs::is_directory(p.parent_path()),"fixture parent exists");
    Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"claim new directory exclusively");
}
Bytes Raw(const fs::path& p) {
    std::ifstream input(p,std::ios::binary); Check(bool(input),"read owned bytes");
    Bytes result{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    Check(!input.bad(),"complete owned byte read"); return result;
}
std::string Text(const fs::path& p) { const auto b=Raw(p);return {b.begin(),b.end()}; }
void Write(const fs::path& p,const Bytes& b,DWORD disposition=CREATE_NEW) {
    Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(h.value!=INVALID_HANDLE_VALUE,"open owned fixture for write");
    DWORD done=0;
    Check(b.size()<=MAXDWORD&&WriteFile(h.value,b.data(),static_cast<DWORD>(b.size()),&done,nullptr)&&done==b.size(),"write exact fixture bytes");
    Check(SetEndOfFile(h.value)!=FALSE,"set exact fixture length");
}
void NewText(const fs::path& p,const std::string& text) { Write(p,Bytes(text.begin(),text.end())); }
files::Spec Spec() {
    files::Spec s;
    for(unsigned i=0;i<16;++i)s.owner[i]=static_cast<std::uint8_t>(i+1);
    for(unsigned i=0;i<32;++i)s.key[i]=static_cast<std::uint8_t>(128+i);
    return s;
}
bool Equal(const files::BlobInfo& a,const files::BlobInfo& b) {
    return a.name==b.name&&a.spec.owner==b.spec.owner&&a.spec.kind==b.spec.kind&&a.spec.key==b.spec.key&&
        a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&
        a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;
}
std::uint64_t LE(const Bytes& b,std::size_t offset,unsigned count) {
    Check(offset+count<=b.size(),"independent little-endian read bounds");
    std::uint64_t n=0;for(unsigned i=0;i<count;++i)n|=std::uint64_t(b[offset+i])<<(8*i);return n;
}
void PutLE(Bytes& b,std::size_t offset,std::uint64_t n,unsigned count) {
    Check(offset+count<=b.size(),"fixture little-endian write bounds");
    for(unsigned i=0;i<count;++i)b[offset+i]=static_cast<std::uint8_t>(n>>(8*i));
}
files::Digest FromHex(const char* text) {
    files::Digest d{};
    const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(unsigned i=0;i<32;++i)d[i]=static_cast<std::uint8_t>(digit(text[2*i])*16+digit(text[2*i+1]));
    return d;
}
files::Digest Sha(const Bytes& bytes) {
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
        Bytes object;
        ~Hash(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    } h;
    Check(BCryptOpenAlgorithmProvider(&h.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"independent SHA provider");
    DWORD objectBytes=0,actual=0;
    Check(BCryptGetProperty(h.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&actual,0)>=0&&actual==sizeof(objectBytes),"independent SHA storage size");
    h.object.resize(objectBytes);
    Check(BCryptCreateHash(h.algorithm,&h.hash,h.object.data(),objectBytes,nullptr,0,0)>=0,"independent SHA hash object");
    // Deliberately different chunk boundaries from the production 64 KiB loop.
    for(std::size_t offset=0;offset<bytes.size();offset+=997){
        const auto count=static_cast<ULONG>(std::min<std::size_t>(997,bytes.size()-offset));
        Check(BCryptHashData(h.hash,const_cast<PUCHAR>(bytes.data()+offset),count,0)>=0,"independent SHA stream");
    }
    files::Digest digest{};
    Check(BCryptFinishHash(h.hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,"independent SHA finish");
    return digest; // Hash destructor destroys the handle before its object member.
}
void Envelope(const fs::path& root,const files::BlobInfo& info,const Bytes& payload) {
    const auto raw=Raw(root/info.name);
    Check(raw.size()==128+payload.size(),"exact 128-byte envelope plus payload, no tail");
    const std::array<std::uint8_t,16> magic{'M','M','S','C','a','c','h','e','B','l','o','b','0','0','0','1'};
    Check(std::equal(magic.begin(),magic.end(),raw.begin()),"independent fixed 16-byte magic without NUL");
    Check(LE(raw,16,4)==1&&LE(raw,20,4)==static_cast<unsigned>(info.spec.kind),"header version and kind");
    Check(std::equal(info.spec.owner.begin(),info.spec.owner.end(),raw.begin()+24)&&
        std::equal(info.spec.key.begin(),info.spec.key.end(),raw.begin()+40),"header exact owner and key");
    const auto digest=Sha(payload);
    Check(LE(raw,72,8)==payload.size()&&info.payloadBytes==payload.size()&&info.payloadDigest==digest&&
        std::equal(digest.begin(),digest.end(),raw.begin()+80),"independent payload size and SHA256");
    std::wstring generation;constexpr wchar_t hex[]=L"0123456789abcdef";
    for(unsigned i=112;i<128;++i){generation+=hex[raw[i]>>4];generation+=hex[raw[i]&15];}
    Check(info.name==generation+L".blob","header generation exactly matches lowercase name");
    Check(std::equal(payload.begin(),payload.end(),raw.begin()+128),"actual disk payload bytes");
    Handle h(CreateFileW((root/info.name).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    BY_HANDLE_FILE_INFORMATION identity{};
    Check(h.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h.value,&identity),"query actual Win32 identity");
    Check(info.identity.volume==identity.dwVolumeSerialNumber&&info.identity.fileIndex==
        ((std::uint64_t(identity.nFileIndexHigh)<<32)|identity.nFileIndexLow)&&info.identity.fileBytes==raw.size(),"identity matches actual volume, file index and full size");
}
files::Directory Open(const fs::path& root) {
    files::Directory d;Check(files::Directory::Open(root,true,d)==Result::Ok&&d.IsValid(),"open actual guarded cache directory");return d;
}
files::BlobInfo Create(files::Directory& d,const Bytes& payload,files::Kind kind=files::Kind::Effect) {
    auto spec=Spec();spec.kind=kind;files::BlobInfo info;
    Check(d.Create(spec,payload.empty()?nullptr:payload.data(),payload.size(),info)==Result::Ok,"create real generation");
    Check(info.spec.owner==spec.owner&&info.spec.kind==spec.kind&&info.spec.key==spec.key,"returned generation preserves independent caller Spec");return info;
}
void ReadEquals(const files::ReadLease& lease,const Bytes& expected) {
    Bytes output{91,92,93};Check(lease.ReadAll(output)==Result::Ok&&output==expected,"lease reads verified exact same-handle payload");
}

void Basic(const fs::path& root) {
    const auto path=root/L"basic";auto d=Open(path);
    NewText(path/L"foreign.txt","foreign sentinel\n");
    Bytes large(65536+137);for(std::size_t i=0;i<large.size();++i)large[i]=static_cast<std::uint8_t>(i*37+11);
    const std::array<Bytes,4> payloads{Bytes{},Bytes{'a','b','c'},Bytes{0,0xff,0x80,0xe4,0xb8,0xad,0},large};
    Check(Sha(payloads[0])==FromHex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),"empty SHA known golden");
    Check(Sha(payloads[1])==FromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"abc SHA known golden");
    for(unsigned i=0;i<payloads.size();++i){
        const auto info=Create(d,payloads[i],i%2?files::Kind::Texture:files::Kind::Effect);Envelope(path,info,payloads[i]);
        files::ReadLease lease,second;
        Check(d.Inspect(info.name,Spec().owner,files::MaximumPayloadBytes,lease)==Result::Ok&&Equal(lease.Info(),info),"Inspect validates full envelope and identity");
        Check(d.Acquire(info,second)==Result::Ok,"read leases coexist");ReadEquals(lease,payloads[i]);ReadEquals(second,payloads[i]);
        {
            Handle writer(CreateFileW((path/info.name).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            const DWORD error=writer.value==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
            Check(writer.value==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION,
                "active read lease directly denies writable handle, independently of Remove share-zero");
        }
        Check(d.Remove(info)==Result::Busy,"same-process leases block exclusive Remove");
        files::ReadLease moved(std::move(lease));Check(!lease.IsValid()&&moved.IsValid(),"lease move constructor empties source");
        lease=std::move(moved);Check(!moved.IsValid()&&lease.IsValid(),"lease move assignment empties source");
        const auto before=lease.Info();auto wrong=info;wrong.spec.key[0]^=1;
        Check(d.Acquire(wrong,lease)==Result::Conflict&&Equal(lease.Info(),before),"wrong trusted key rejects and preserves output lease");
        wrong=info;wrong.identity.fileIndex^=1;
        Check(d.Acquire(wrong,lease)==Result::Conflict&&Equal(lease.Info(),before),"wrong trusted identity preserves lease");
        auto owner=Spec().owner;owner[0]^=1;
        Check(d.Inspect(info.name,owner,files::MaximumPayloadBytes,lease)==Result::Corrupt&&Equal(lease.Info(),before),"wrong owner rejects inspect without publishing");
        if(!payloads[i].empty())Check(d.Inspect(info.name,Spec().owner,payloads[i].size()-1,lease)==Result::Limit&&Equal(lease.Info(),before),"caller payload cap enforced");
        ReadEquals(lease,payloads[i]);
        lease={};second={};
        Check(d.Remove(info)==Result::DeletePending,"exclusive same-handle disposition accepted");
        Check(!fs::exists(path/info.name)&&d.Acquire(info,lease)==Result::Missing,"removed generation eventually absent after handle closes");
    }
    {
        const Bytes pagePayload{0x4d,0x4d,0x53,0x00,0xff};
        const auto page=Create(d,pagePayload,files::Kind::IndexPage);
        Envelope(path,page,pagePayload);
        files::ReadLease pageLease;
        Check(d.Acquire(page,pageLease)==Result::Ok&&pageLease.Info().spec.kind==files::Kind::IndexPage,
            "index page file kind survives actual creation and same-handle acquisition");
        ReadEquals(pageLease,pagePayload);
        Check(d.Remove(page)==Result::Busy,"actual index page lease blocks removal");
        pageLease={};
        Check(d.Remove(page)==Result::DeletePending&&!fs::exists(path/page.name),"unleased index page generation can be removed");
    }
    const Bytes oldPayload{1,2,3},newPayload{8,9};
    const auto oldInfo=Create(d,oldPayload);files::ReadLease oldLease;
    Check(d.Acquire(oldInfo,oldLease)==Result::Ok,"lease old generation");
    const auto newInfo=Create(d,newPayload);
    Check(oldInfo.name!=newInfo.name&&oldInfo.identity.fileIndex!=newInfo.identity.fileIndex,"new Create publishes a different live generation");
    files::ReadLease newLease;Check(d.Acquire(newInfo,newLease)==Result::Ok,"lease new generation");
    d={};ReadEquals(oldLease,oldPayload);ReadEquals(newLease,newPayload);
    Check(Text(path/L"foreign.txt")=="foreign sentinel\n","foreign same-directory file untouched");
}

void Arguments(const fs::path& root) {
    auto d=Open(root/L"arguments");const Bytes payload{'o','k'};
    auto info=Create(d,payload);const auto before=info;
    auto bad=Spec();bad.owner={};
    Check(d.Create(bad,payload.data(),payload.size(),info)==Result::Invalid&&Equal(info,before),"zero owner rejected without output change");
    bad=Spec();bad.kind=static_cast<files::Kind>(99);
    Check(d.Create(bad,payload.data(),payload.size(),info)==Result::Invalid&&Equal(info,before),"unknown kind rejected");
    Check(d.Create(Spec(),nullptr,1,info)==Result::Invalid&&Equal(info,before),"nonempty null payload rejected");
    Check(d.Create(Spec(),reinterpret_cast<const void*>(1),static_cast<std::size_t>(files::MaximumPayloadBytes+1),info)==Result::Limit&&Equal(info,before),"over-1GiB rejects without dereferencing pointer");
    files::ReadLease lease;Check(d.Acquire(info,lease)==Result::Ok,"argument output lease sentinel");
    for(const std::wstring& name:std::vector<std::wstring>{L"../foreign",L"x/y.blob",L"x\\y.blob",L"a:stream",L"CON",L"",L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.blob",L"00000000000000000000000000000000.blob."}){
        Check(d.Inspect(name,Spec().owner,files::MaximumPayloadBytes,lease)==Result::Invalid&&Equal(lease.Info(),before),"unsafe blob name rejects with output unchanged");
        auto invalid=info;invalid.name=name;Check(d.Remove(invalid)==Result::Invalid,"unsafe Remove name rejects before IO");
    }
    Check(d.Inspect(info.name,Spec().owner,files::MaximumPayloadBytes+1,lease)==Result::Invalid&&Equal(lease.Info(),before),"oversized Inspect cap rejected");
    files::ReadLease empty;Bytes output{7,8};
    Check(empty.ReadAll(output)==Result::Invalid&&output==Bytes({7,8}),"invalid lease read preserves vector");
    files::Directory invalid;files::FileLock lock;
    Check(invalid.Lock(0,lock)==Result::Invalid&&!lock.IsValid(),"invalid directory lock rejected");
    Check(d.Lock(2001,lock)==Result::Invalid&&!lock.IsValid(),"lock wait is bounded by contract");
    std::vector<std::wstring> names{L"sentinel"};
    Check(d.Enumerate(0,names)==Result::Invalid&&names==std::vector<std::wstring>{L"sentinel"},"zero enumeration bound rejected");
    Check(d.Enumerate(65537,names)==Result::Invalid&&names==std::vector<std::wstring>{L"sentinel"},"oversized enumeration bound rejected");
    const auto another=Create(d,payload);(void)another;
    NewText(root/L"arguments"/L"foreign.txt","keep\n");
    Check(d.Enumerate(1,names)==Result::Limit&&names==std::vector<std::wstring>{L"sentinel"},"enumeration overflow preserves output");
    Check(d.Enumerate(10,names)==Result::Ok&&names.size()==2&&std::is_sorted(names.begin(),names.end()),"enumeration returns only bounded sorted generation candidates");
    ReadEquals(lease,payload);
}

void Corruption(const fs::path& root) {
    const auto path=root/L"corruption";auto d=Open(path);const Bytes payload{0,1,0xfe,3,4};
    const auto sentinelInfo=Create(d,Bytes{9});files::ReadLease output;
    Check(d.Acquire(sentinelInfo,output)==Result::Ok,"corrupt-read output sentinel");
    for(unsigned variant=0;variant<12;++variant){
        const auto info=Create(d,payload);auto bytes=Raw(path/info.name);
        switch(variant){
        case 0:bytes[0]^=1;break;
        case 1:PutLE(bytes,16,2,4);break;
        case 2:PutLE(bytes,20,99,4);break;
        case 3:bytes[24]^=1;break;
        case 4:PutLE(bytes,72,payload.size()+1,8);break;
        case 5:bytes[80]^=1;break;
        case 6:bytes[112]^=1;break;
        case 7:bytes.push_back(77);break;
        case 8:bytes.resize(127);break;
        case 9:bytes.pop_back();break;
        case 10:bytes[128]^=1;break;
        case 11:std::fill(bytes.begin()+24,bytes.begin()+40,0);break;
        }
        Write(path/info.name,bytes,OPEN_EXISTING);
        Check(d.Inspect(info.name,Spec().owner,files::MaximumPayloadBytes,output)==Result::Corrupt&&Equal(output.Info(),sentinelInfo),"corrupt envelope or payload rejected without changing lease");
        Check(d.Remove(info)==Result::Corrupt&&Raw(path/info.name)==bytes,"corrupt generation never removed");
    }
    const auto keyInfo=Create(d,payload);auto altered=Raw(path/keyInfo.name);altered[40]^=1;Write(path/keyInfo.name,altered,OPEN_EXISTING);
    Check(d.Acquire(keyInfo,output)==Result::Conflict&&Equal(output.Info(),sentinelInfo),"valid envelope with changed key conflicts with trusted index");
    Check(d.Remove(keyInfo)==Result::Conflict&&Raw(path/keyInfo.name)==altered,"changed-key file not deleted");
    const auto unchanged=Create(d,payload);auto wrongIdentity=unchanged;wrongIdentity.identity.fileIndex^=1;
    const auto originalBytes=Raw(path/unchanged.name);
    Check(d.Remove(wrongIdentity)==Result::Conflict&&Raw(path/unchanged.name)==originalBytes,"wrong trusted identity cannot delete a valid file");
    std::array<files::BlobInfo,5> mismatches{unchanged,unchanged,unchanged,unchanged,unchanged};
    mismatches[0].identity.volume^=1;
    ++mismatches[1].identity.fileBytes;
    mismatches[2].payloadDigest[0]^=1;
    ++mismatches[3].payloadBytes;
    mismatches[4].spec.kind=files::Kind::Texture;
    const std::array<const char*,5> fields{"volume","fileBytes","payloadDigest","payloadBytes","kind"};
    for(std::size_t i=0;i<mismatches.size();++i){
        const auto message=std::string("trusted ")+fields[i]+" mismatch rejects Acquire without publishing";
        Check(d.Acquire(mismatches[i],output)==Result::Conflict&&Equal(output.Info(),sentinelInfo),message.c_str());
        const auto removeMessage=std::string("trusted ")+fields[i]+" mismatch rejects Remove without altering file";
        Check(d.Remove(mismatches[i])==Result::Conflict&&Raw(path/unchanged.name)==originalBytes,removeMessage.c_str());
    }
    const auto linkInfo=Create(d,payload);const auto link=path/L"foreign-hardlink.bin";
    // Build the adversarial topology before acquiring directory protection;
    // keeping parent guards here would test fixture creation, not rejection.
    output={};d={};
    const bool linked=CreateHardLinkW(link.c_str(),(path/linkInfo.name).c_str(),nullptr)!=FALSE;
    const DWORD linkError=linked?ERROR_SUCCESS:GetLastError();
    NewText(root/L"hardlink-create.txt","created="+std::to_string(linked)+" error="+std::to_string(linkError)+"\n");
    Check(linked,"create own second hardlink before directory protection");
    d=Open(path);
    Check(d.Acquire(sentinelInfo,output)==Result::Ok,"reacquire original output sentinel after hardlink fixture preparation");
    Check(d.Inspect(linkInfo.name,Spec().owner,files::MaximumPayloadBytes,output)==Result::Corrupt&&Equal(output.Info(),sentinelInfo),"multi-hardlink blob rejected");
    Check(d.Remove(linkInfo)==Result::Corrupt&&fs::exists(link)&&fs::exists(path/linkInfo.name)&&Raw(link)==Raw(path/linkInfo.name),"both hardlinks retained");
    ReadEquals(output,Bytes{9});
}

// A self-contained mount-point buffer; no ntifs/private structure dependency.
bool Junction(HANDLE handle,const fs::path& destination,DWORD& error) {
    const std::wstring substitute=L"\\??\\"+destination.wstring(),print=destination.wstring();
    const auto subBytes=static_cast<unsigned>(substitute.size()*sizeof(wchar_t));
    const auto printBytes=static_cast<unsigned>(print.size()*sizeof(wchar_t));
    Bytes data(16+subBytes+sizeof(wchar_t)+printBytes+sizeof(wchar_t),0);
    PutLE(data,0,IO_REPARSE_TAG_MOUNT_POINT,4);PutLE(data,4,data.size()-8,2);
    PutLE(data,8,0,2);PutLE(data,10,subBytes,2);PutLE(data,12,subBytes+sizeof(wchar_t),2);PutLE(data,14,printBytes,2);
    std::memcpy(data.data()+16,substitute.data(),subBytes);
    std::memcpy(data.data()+16+subBytes+sizeof(wchar_t),print.data(),printBytes);
    DWORD done=0;const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,data.data(),static_cast<DWORD>(data.size()),nullptr,0,&done,nullptr)!=FALSE;
    error=ok?ERROR_SUCCESS:GetLastError();return ok;
}
void GuardOperations(const fs::path& path,const fs::path& target,const fs::path& report) {
    const auto renamed=fs::path(path.wstring()+L"-renamed");
    Check(!fs::exists(renamed),"rename target is unoccupied owned name");
    const bool moved=MoveFileExW(path.c_str(),renamed.c_str(),0)!=FALSE;
    const DWORD moveError=moved?ERROR_SUCCESS:GetLastError();
    NewText(report,"rename="+std::to_string(moved)+" error="+std::to_string(moveError)+"\n");
    Check(!moved&&fs::is_directory(path),"live directory guard denies rename");
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    const DWORD openError=h.value==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
    DWORD setError=openError;bool set=false;
    if(h.value!=INVALID_HANDLE_VALUE)set=Junction(h.value,target,setError);
    NewText(fs::path(report.wstring()+L".reparse.txt"),"reparse-open="+std::to_string(openError)+
        " set="+std::to_string(set)+" error="+std::to_string(setError)+"\n");
    Check(h.value==INVALID_HANDLE_VALUE&&openError==ERROR_SHARING_VIOLATION,"directory guard denies writable open itself, not merely the reparse IOCTL");
    Check(!set&&(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)==0,"live directory guard prevents reparse conversion");
}
void PathsAndGuards(const fs::path& root) {
    const auto valid=root/L"guarded",target=root/L"junction-target";
    NewDirectory(target);NewDirectory(target/L"child");
    NewDirectory(valid);
    {
        Handle writable(CreateFileW(valid.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        Check(writable.value!=INVALID_HANDLE_VALUE,"positive control permits writable open before directory protection");
    }
    auto d=Open(valid);
    const auto drive=root.root_path().wstring();
    std::vector<fs::path> bad{L"relative",drive,L"\\\\server\\share\\cache",L"\\\\?\\C:\\cache",L"\\\\.\\C:\\cache",
        root/L"x"/L".."/L"cache",root/L"."/L"cache",fs::path(root.wstring()+L":stream"),root/L"tail.",root/L"tail ",root/L"CON",
        root/L"nul.txt",root/L"PRN",root/L"AUX.bin",root/L"COM1.log",root/L"COM9",root/L"LPT1.txt",root/L"LPT9"};
    for(const wchar_t c:std::array<wchar_t,7>{1,L'<',L'>',L'"',L'|',L'?',L'*'})
        bad.push_back(root/(std::wstring(L"bad")+c+L"name"));
    std::wstring embeddedNull=root.wstring()+L"\\bad";embeddedNull.push_back(L'\0');embeddedNull+=L"tail";
    bad.emplace_back(embeddedNull);
    for(const auto& p:bad)Check(files::Directory::Open(p,false,d)==Result::Invalid&&d.IsValid(),"unsafe root path rejected without replacing Directory");
    Check(files::Directory::Open(root/L"missing-directory",false,d)==Result::Missing&&d.IsValid()&&!fs::exists(root/L"missing-directory"),"missing non-creating Open preserves original directory");
    GuardOperations(valid,target,root/L"guard-directory.txt");
    const auto info=Create(d,Bytes{4,5});files::ReadLease lease;
    Check(d.Acquire(info,lease)==Result::Ok,"lease retains directory chain");d={};
    GuardOperations(valid,target,root/L"guard-lease.txt");ReadEquals(lease,Bytes{4,5});lease={};
    const auto renamed=root/L"guarded-after-release";
    Check(MoveFileExW(valid.c_str(),renamed.c_str(),0)!=FALSE&&MoveFileExW(renamed.c_str(),valid.c_str(),0)!=FALSE,"same rename works after last directory guard released");
    auto locked=Open(root/L"lock-guarded");files::FileLock lock;
    Check(locked.Lock(0,lock)==Result::Ok,"lock holds directory chain");locked={};
    GuardOperations(root/L"lock-guarded",target,root/L"guard-lock.txt");lock={};
    const auto junction=root/L"junction";NewDirectory(junction);
    DWORD error=0;
    {
        Handle h(CreateFileW(junction.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        Check(h.value!=INVALID_HANDLE_VALUE,"open own empty junction directory");
        if(!Junction(h.value,target,error)){
            NewText(root/L"junction-unavailable.txt","Win32="+std::to_string(error)+"\n");
            Check(error==ERROR_PRIVILEGE_NOT_HELD||error==ERROR_NOT_SUPPORTED||error==ERROR_INVALID_FUNCTION,"junction fixture failed unexpectedly");
            ++skips;std::printf("SKIP actual junction capability Win32=%lu\n",error);return;
        }
    }
    files::Directory rejected;
    Check(files::Directory::Open(junction,false,rejected)==Result::Invalid&&!rejected.IsValid(),"reparse root rejected");
    Check(files::Directory::Open(junction/L"child",false,rejected)==Result::Invalid&&!rejected.IsValid(),"reparse ancestor rejected");
    Check(fs::is_directory(target/L"child"),"junction target contents untouched");
}

fs::path Executable() {
    wchar_t buffer[32768]{};const DWORD n=GetModuleFileNameW(nullptr,buffer,32768);
    Check(n>0&&n<32768,"resolve own worker executable");return fs::path(buffer);
}
int Worker(const fs::path& control,const fs::path& target,const std::wstring& mode,const std::wstring& name) {
    Absolute(control);Absolute(target);
    Check(control.parent_path()==target.parent_path()&&Text(control/L"marker")==Marker,"worker restricted to parent-owned sibling directories");
    files::Directory d;Check(files::Directory::Open(target,false,d)==Result::Ok,"worker opens guarded target");
    if(mode==L"busy"){
        files::FileLock denied;Check(d.Lock(0,denied)==Result::Busy&&!denied.IsValid(),"child sees parent's real byte lock");return 0;
    }
    files::FileLock lock;files::ReadLease lease;
    if(mode==L"lease")Check(d.Inspect(name,Spec().owner,files::MaximumPayloadBytes,lease)==Result::Ok,"child owns read lease");
    else Check((mode==L"lock"||mode==L"hard-exit")&&d.Lock(0,lock)==Result::Ok,"child owns real byte lock");
    NewText(control/L"ready","ready\n");
    const auto start=GetTickCount64();
    while(!fs::exists(control/L"release")){Check(GetTickCount64()-start<20000,"child release wait bounded");Sleep(5);}
    if(mode==L"hard-exit")ExitProcess(23); // Intentionally bypass C++ unlock/close; kernel must release ownership.
    return 0;
}
struct Child {
    PROCESS_INFORMATION process{};fs::path control;bool done=false;
    Child(const fs::path& root,const fs::path& target,const std::wstring& label,const std::wstring& mode,const std::wstring& name=L"-")
        :control(root/label){
        NewDirectory(control);NewText(control/L"marker",Marker);
        const auto exe=Executable();auto command=L"\""+exe.wstring()+L"\" --worker \""+control.wstring()+L"\" \""+target.wstring()+L"\" "+mode+L" "+name;
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        const bool ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,control.c_str(),&startup,&process)!=FALSE;
        const DWORD error=ok?ERROR_SUCCESS:GetLastError();
        try {
            NewText(control/L"launch.txt","created="+std::to_string(ok)+" error="+std::to_string(error)+"\n");
            Check(ok,"launch own hidden worker");
        } catch(...) {
            if(process.hProcess){TerminateProcess(process.hProcess,125);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hProcess);}
            if(process.hThread)CloseHandle(process.hThread);
            process={};throw;
        }
    }
    Child(const Child&)=delete;
    ~Child(){
        if(process.hProcess){
            if(!done&&WaitForSingleObject(process.hProcess,0)!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,125);WaitForSingleObject(process.hProcess,5000);}
            if(!done){DWORD code=STILL_ACTIVE;GetExitCodeProcess(process.hProcess,&code);std::FILE* f=nullptr;
                try { if(_wfopen_s(&f,(control/L"aborted-exit.txt").c_str(),L"wb")==0&&f){std::fprintf(f,"exit=%lu\n",code);std::fclose(f);} }catch(...){} }
            CloseHandle(process.hProcess);
        }
        if(process.hThread)CloseHandle(process.hThread);
    }
    void Ready(){
        const auto start=GetTickCount64();
        while(!fs::exists(control/L"ready")){
            Check(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT,"worker remains alive before ready");
            Check(GetTickCount64()-start<10000,"worker ready bounded");Sleep(5);
        }
    }
    void Release(){NewText(control/L"release","release\n");}
    void Wait(DWORD expected){
        const DWORD waited=WaitForSingleObject(process.hProcess,10000);
        if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,124);WaitForSingleObject(process.hProcess,5000);}
        DWORD code=STILL_ACTIVE;const bool queried=GetExitCodeProcess(process.hProcess,&code)!=FALSE;done=queried&&code!=STILL_ACTIVE;
        NewText(control/L"exit.txt","wait="+std::to_string(waited)+" exit="+std::to_string(code)+"\n");
        Check(waited==WAIT_OBJECT_0&&queried&&code==expected,"worker exit code recorded and expected");
    }
};
void LocksAndProcesses(const fs::path& root) {
    const auto target=root/L"cross-process";auto d=Open(target);const auto info=Create(d,Bytes{1,7,2});
    files::FileLock first,second;
    Check(d.Lock(0,first)==Result::Ok&&first.IsValid(),"first byte lock acquired");
    Check(d.Lock(0,second)==Result::Busy&&!second.IsValid(),"second same-process byte lock busy");
    Check(d.Lock(0,first)==Result::Busy&&first.IsValid(),"Busy lock call preserves existing valid output ownership");
    {Child child(root,target,L"worker-busy",L"busy");child.Wait(0);}
    std::thread release([moved=std::move(first)]()mutable{files::FileLock other;other=std::move(moved);});release.join();
    Check(!first.IsValid()&&d.Lock(0,second)==Result::Ok,"cross-thread move/destruction releases byte lock");second={};
    for(const bool hard:{false,true}){
        Child child(root,target,hard?L"worker-hard-exit":L"worker-normal",hard?L"hard-exit":L"lock");child.Ready();
        Check(d.Lock(0,first)==Result::Busy&&!first.IsValid(),"parent sees child byte lock");
        child.Release();
        const auto start=GetTickCount64();const auto result=d.Lock(2000,first);
        Check(result==Result::Ok&&GetTickCount64()-start<5000,"bounded retry acquires after child normal or hard exit");
        child.Wait(hard?23:0);first={};
    }
    {Child child(root,target,L"worker-lease",L"lease",info.name);child.Ready();
        Check(d.Remove(info)==Result::Busy,"cross-process lease blocks Remove");
        files::ReadLease coexist;Check(d.Acquire(info,coexist)==Result::Ok,"cross-process readers coexist");ReadEquals(coexist,Bytes{1,7,2});coexist={};
        child.Release();child.Wait(0);
    }
    Check(d.Remove(info)==Result::DeletePending&&!fs::exists(target/info.name),"released cross-process generation can be removed");
    files::FileLock retained;Check(d.Lock(0,retained)==Result::Ok,"hold lock past Directory lifetime");d={};
    auto other=Open(target);Check(other.Lock(0,first)==Result::Busy,"Directory destruction does not release derived lock");retained={};
    Check(other.Lock(0,first)==Result::Ok,"last lock release restores acquisition");
}

void AllocationChecks(const fs::path& root) {
    bool caught=false;void*(*volatile allocate)(std::size_t)=&::operator new;
    {allocation::Fail fail;try{void* p=allocate(64);::operator delete(p);}catch(const std::bad_alloc&){caught=true;}}
    Check(caught&&allocation::hits==1,"calibrate ordinary current-thread new fault");
    const auto path=root/L"allocations";auto d=Open(path);const Bytes payload{1,2,3};const auto info=Create(d,payload);
    files::ReadLease lease;Check(d.Acquire(info,lease)==Result::Ok,"allocation output lease sentinel");
    Result result;
    const auto oldRoot=root/L"allocation-old-output",openingRoot=root/L"allocation-open-target";
    auto oldDirectory=Open(oldRoot);const auto oldInfo=Create(oldDirectory,Bytes{7});NewDirectory(openingRoot);
    const auto deleteAccess=[&](bool guarded){
        // Request permission only: never mark disposition or mutate the leaf.
        Handle probe(CreateFileW(openingRoot.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        const DWORD error=probe.value==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
        Check(guarded ? probe.value==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION : probe.value!=INVALID_HANDLE_VALUE,
            guarded ? "calibration DELETE access is denied by actual leaf guard" : "DELETE access succeeds with only shared ancestor guards");
    };
    deleteAccess(false); // d, lease and oldDirectory retain common ancestors.
    {auto calibration=Open(openingRoot);deleteAccess(true);}
    deleteAccess(false);
    bool openCompleted=false;unsigned openFailures=0;
    for(long failAfter=0;failAfter<256;++failAfter){
        files::Directory directoryOutput;
        Check(files::Directory::Open(oldRoot,false,directoryOutput)==Result::Ok,"prepare distinct old Directory output before fault");
        {allocation::Fail fail(failAfter);result=files::Directory::Open(openingRoot,false,directoryOutput);}
        if(allocation::hits){
            ++openFailures;
            Check(result==Result::Io&&directoryOutput.IsValid(),"Directory allocation failure preserves prior valid output");
            files::ReadLease acquired;
            Check(directoryOutput.Acquire(oldInfo,acquired)==Result::Ok,"failed Open output still refers to distinct original directory");
            ReadEquals(acquired,Bytes{7});
        }else{
            Check(result==Result::Ok&&directoryOutput.IsValid(),"unfaulted Open endpoint succeeds");
            openCompleted=true;
        }
        // Rename also needs parent-directory mutation and is therefore not a
        // leaf-handle oracle while the other fixtures retain ancestor guards.
        // Calibrated DELETE access checks only the target's sharing state.
        directoryOutput={};
        deleteAccess(false);
        if(openCompleted)break;
    }
    Check(openCompleted&&openFailures>1,"bounded Open scan reaches its first uninjected success");
    {allocation::Fail fail;result=d.Inspect(info.name,Spec().owner,files::MaximumPayloadBytes,lease);}
    Check(allocation::hits&&result==Result::Io&&Equal(lease.Info(),info),"Inspect allocation failure preserves lease identity");ReadEquals(lease,payload);
    Bytes output{9,8};
    {allocation::Fail fail;result=lease.ReadAll(output);}
    Check(allocation::hits&&result==Result::Io&&output==Bytes({9,8}),"ReadAll allocation failure preserves output");
    std::vector<std::wstring> before,after;
    Check(d.Enumerate(100,before)==Result::Ok,"record before failed creation");
    bool createCompleted=false;unsigned createFailures=0;
    for(long failAfter=0;failAfter<256;++failAfter){
        auto creation=info;
        {allocation::Fail fail(failAfter);result=d.Create(Spec(),payload.data(),payload.size(),creation);}
        if(allocation::hits){
            ++createFailures;
            Check(result==Result::Io&&Equal(creation,info),"Create allocation failure preserves output");
            Check(d.Enumerate(100,after)==Result::Ok&&before==after,"every failed Create leaves exact original generation set");
        }else{
            Check(result==Result::Ok&&creation.name!=info.name,"unfaulted Create endpoint publishes exactly a new generation");
            Envelope(path,creation,payload);
            Check(creation.spec.owner==Spec().owner&&creation.spec.key==Spec().key&&creation.spec.kind==Spec().kind,"unfaulted Create preserves requested Spec");
            Check(d.Remove(creation)==Result::DeletePending&&!fs::exists(path/creation.name),"remove only successful scan generation using its returned identity");
            Check(d.Enumerate(100,after)==Result::Ok&&before==after,"successful Create scan cleanup restores exact candidate set");
            createCompleted=true;break;
        }
    }
    Check(createCompleted&&createFailures>1,"bounded Create scan reaches its first uninjected success");
    files::FileLock lock;
    {allocation::Fail fail;result=d.Lock(0,lock);}
    Check(allocation::hits&&result==Result::Io&&!lock.IsValid(),"Lock allocation failure does not publish partial lock");
    Check(d.Lock(0,lock)==Result::Ok,"failed Lock released any acquired byte lock");
}
}

int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        if(argc==6&&std::wstring(argv[1])==L"--worker")return Worker(argv[2],argv[3],argv[4],argv[5]);
        Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIR");
        const fs::path root(argv[2]);NewDirectory(root);
        Basic(root);Arguments(root);Corruption(root);PathsAndGuards(root);LocksAndProcesses(root);AllocationChecks(root);
        NewText(root/L"result.txt","checks="+std::to_string(checks)+"\nskips="+std::to_string(skips)+"\n");
        std::printf("PASS cache files checks=%u skips=%u evidence=%ls\n",checks,skips,root.c_str());return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL cache files checks=%u: %s\n",checks,e.what());return 1;}
      catch(...){std::fprintf(stderr,"FAIL cache files unexpected exception\n");return 2;}
}
