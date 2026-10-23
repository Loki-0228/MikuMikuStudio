// MAINTENANCE: regression-keep
// Real Win7 file operations in one new caller-owned directory. No recursive
// deletion, application startup, UI, GPU or process-wide hooks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_files.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace fs = std::filesystem;
namespace cf = mikumikustudio::cache_files;
using cf::Result;
using Bytes = std::vector<std::uint8_t>;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
void NewDirectory(const fs::path& path) {
    Check(path.is_absolute() && path.has_filename() && path != path.root_path() &&
        path == path.lexically_normal(), "new absolute normalized non-root directory required");
    Check(fs::is_directory(path.parent_path()), "fixture parent exists");
    Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "exclusively create fresh fixture directory");
}
cf::Spec Spec() {
    cf::Spec result;
    result.owner[0] = 13; result.owner[15] = 71;
    result.kind = cf::Kind::CatalogIntent;
    result.key[0] = 128; result.key[31] = 255;
    return result;
}
bool Same(const cf::Identity& a, const cf::Identity& b) {
    return a.volume == b.volume && a.fileIndex == b.fileIndex && a.fileBytes == b.fileBytes;
}
bool Same(const cf::Spec& a, const cf::Spec& b) {
    return a.owner == b.owner && a.kind == b.kind && a.key == b.key;
}
bool Same(const cf::WritePlan& a, const cf::WritePlan& b) {
    return a.name == b.name && Same(a.spec,b.spec) && a.payloadDigest == b.payloadDigest &&
        a.payloadBytes == b.payloadBytes && Same(a.createdIdentity,b.createdIdentity) && a.completedBytes == b.completedBytes;
}
bool Same(const cf::BlobInfo& a, const cf::BlobInfo& b) {
    return a.name == b.name && Same(a.spec,b.spec) && a.payloadDigest == b.payloadDigest &&
        a.payloadBytes == b.payloadBytes && Same(a.identity,b.identity);
}
cf::BlobInfo Sentinel() {
    cf::BlobInfo value;
    value.name = L"unchanged output sentinel"; value.spec = Spec();
    value.payloadDigest.fill(42); value.payloadBytes = 17;
    value.identity = {11,22,33};
    return value;
}
cf::Identity Identity(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Check(file.value != INVALID_HANDLE_VALUE, "open owned file for independent identity");
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(file.value,&info) != FALSE, "query actual owned file identity");
    Check(info.nNumberOfLinks == 1 && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),
        "fixture is a single-link regular file");
    return {info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow,
        (std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow};
}
Bytes Raw(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    Check(bool(file), "open owned bytes");
    Bytes bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    Check(!file.bad(), "read all owned bytes");
    return bytes;
}
void Write(const fs::path& path, const Bytes& bytes, DWORD disposition) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(file.value != INVALID_HANDLE_VALUE, "open owned fixture for direct write");
    DWORD done = 0;
    Check(bytes.size() <= MAXDWORD && WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&done,nullptr) &&
        done == bytes.size() && SetEndOfFile(file.value) && FlushFileBuffers(file.value), "write and flush exact fixture bytes");
}
std::uint64_t LE(const Bytes& bytes, std::size_t offset, unsigned count) {
    Check(offset+count <= bytes.size(), "independent envelope read bounds");
    std::uint64_t value = 0;
    for (unsigned i=0;i<count;++i) value |= std::uint64_t(bytes[offset+i]) << (8*i);
    return value;
}
cf::Digest AbcDigest() {
    constexpr char hex[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    cf::Digest result{};
    const auto digit=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
    for(unsigned i=0;i<32;++i) result[i]=static_cast<std::uint8_t>((digit(hex[2*i])<<4)|digit(hex[2*i+1]));
    return result;
}
cf::Directory Open(const fs::path& path) {
    cf::Directory directory;
    Check(cf::Directory::Open(path,true,directory) == Result::Ok, "open fresh managed fixture");
    return directory;
}
void Envelope(const fs::path& path, const cf::BlobInfo& info, const cf::WritePlan& plan, const Bytes& payload) {
    const auto raw = Raw(path/plan.name);
    Check(raw.size() == 128+payload.size(), "exact original envelope length");
    const std::array<std::uint8_t,16> magic{'M','M','S','C','a','c','h','e','B','l','o','b','0','0','0','1'};
    Check(std::equal(magic.begin(),magic.end(),raw.begin()), "original 16-byte envelope magic");
    Check(LE(raw,16,4)==1 && LE(raw,20,4)==7, "existing envelope version and CatalogIntent kind 7");
    Check(std::equal(plan.spec.owner.begin(),plan.spec.owner.end(),raw.begin()+24) &&
        std::equal(plan.spec.key.begin(),plan.spec.key.end(),raw.begin()+40), "requested owner and key on disk");
    Check(LE(raw,72,8)==payload.size() && std::equal(plan.payloadDigest.begin(),plan.payloadDigest.end(),raw.begin()+80),
        "planned length and independently known digest on disk");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name;
    for(unsigned i=112;i<128;++i) {name+=hex[raw[i]>>4];name+=hex[raw[i]&15];}
    name+=L".blob";
    Check(name==plan.name && std::equal(payload.begin(),payload.end(),raw.begin()+128), "filename generation and payload on disk");
    Check(Same(info.identity,Identity(path/plan.name)) && info.identity.volume==plan.createdIdentity.volume &&
        info.identity.fileIndex==plan.createdIdentity.fileIndex, "returned real identity retains prepared file");
}
void Basic(const fs::path& root) {
    const auto path=root/L"basic"; auto directory=Open(path);
    const Bytes bytes{'a','b','c'};
    cf::PreparedWrite prepared;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),prepared)==Result::Ok && prepared.IsValid(), "prepare abc");
    const auto plan=prepared.Plan();
    Check(Same(plan.spec,Spec()) && plan.payloadDigest==AbcDigest() && plan.payloadBytes==3 && plan.completedBytes==131 &&
        plan.createdIdentity.fileBytes==0, "plan independently records requested content and final length");
    bool complete=true;
    Check(prepared.Inspect(complete)==Result::Ok && !complete, "same-handle inspect reports empty prepared file incomplete");
    auto output=Sentinel();
    Check(prepared.Complete(bytes.data(),bytes.size(),output)==Result::Ok && !prepared.IsValid(), "complete consumes handle");
    Check(output.name==plan.name && Same(output.spec,Spec()) && output.payloadDigest==AbcDigest() && output.payloadBytes==3,
        "complete returns full requested info");
    Envelope(path,output,plan,bytes);
    cf::ReadLease lease;
    Check(directory.Acquire(output,lease)==Result::Ok, "normal existing reader accepts completed envelope");
    Bytes read{9}; Check(lease.ReadAll(read)==Result::Ok && read==bytes, "normal reader gets original payload");
    const auto before=output;
    Check(prepared.Complete(bytes.data(),bytes.size(),output)==Result::Invalid && Same(before,output), "consumed completion preserves output");
    Check(prepared.Abort()==Result::Invalid, "consumed abort rejected");
    complete=true;
    Check(prepared.Inspect(complete)==Result::Invalid && complete, "invalid inspect preserves output");

    cf::PreparedWrite held;
    const Bytes empty;
    Check(directory.Prepare(Spec(),nullptr,0,held)==Result::Ok, "prepare zero payload");
    const auto emptyPlan=held.Plan();
    cf::BlobInfo emptyInfo;
    Check(held.Complete(nullptr,0,emptyInfo)==Result::Ok && fs::file_size(path/emptyPlan.name)==128, "complete zero payload envelope");
    cf::ReadLease emptyLease; Check(directory.Acquire(emptyInfo,emptyLease)==Result::Ok, "existing reader verifies zero payload");
}
void RejectionsAndMoves(const fs::path& root) {
    const auto path=root/L"reject"; auto directory=Open(path);
    const Bytes bytes{'a','b','c'},wrong{'a','b','d'};
    cf::PreparedWrite prepared;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),prepared)==Result::Ok, "prepare rejection target");
    const auto plan=prepared.Plan(); auto output=Sentinel(); const auto before=output;
    Check(prepared.Complete(bytes.data(),2,output)==Result::Invalid && Same(output,before), "size mismatch preserves output");
    Check(prepared.Complete(nullptr,3,output)==Result::Invalid && Same(output,before), "null payload preserves output");
    Check(prepared.Complete(wrong.data(),wrong.size(),output)==Result::Conflict && Same(output,before), "digest mismatch preserves output");
    Check(prepared.IsValid() && Same(prepared.Plan(),plan), "input failures retain exclusive handle and complete plan");
    cf::PreparedWrite moved(std::move(prepared));
    Check(!prepared.IsValid() && moved.IsValid() && Same(moved.Plan(),plan), "move construction transfers exact plan");
    moved=std::move(moved); Check(moved.IsValid() && Same(moved.Plan(),plan), "self move remains usable");
    moved={};
    Check(fs::file_size(path/plan.name)==0 && Same(Identity(path/plan.name),plan.createdIdentity), "rejected writes and destruction leave original empty file");
    Check(directory.ReopenPrepared(plan,moved)==Result::Ok, "trusted empty plan reopens after destruction");

    cf::PreparedWrite other;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),other)==Result::Ok, "prepare replacement move target");
    const auto otherPlan=other.Plan(); other=std::move(moved);
    Check(!moved.IsValid() && Same(other.Plan(),plan), "move assignment transfers source");
    Check(fs::file_size(path/otherPlan.name)==0, "move replacement closes but does not delete old file");
    Check(directory.ReopenPrepared(otherPlan,moved)==Result::Ok, "move-replaced file has no leaked handle");
    Check(moved.Abort()==Result::DeletePending && !moved.IsValid() && !fs::exists(path/otherPlan.name), "explicit abort removes only prepared file");

    auto invalid=Spec(); invalid.owner={};
    Check(directory.Prepare(invalid,bytes.data(),bytes.size(),other)==Result::Invalid && Same(other.Plan(),plan), "invalid prepare preserves occupied output");
    Check(directory.Prepare(Spec(),bytes.data(),static_cast<std::size_t>(cf::MaximumPayloadBytes+1),other)==Result::Limit &&
        Same(other.Plan(),plan), "oversize prepare rejects before reading payload and preserves output");
    auto bad=plan; bad.completedBytes++;
    Check(directory.ReopenPrepared(bad,other)==Result::Invalid && Same(other.Plan(),plan), "bad final length rejected without replacing output");
    bad=plan; bad.createdIdentity.fileBytes=1;
    Check(directory.ReopenPrepared(bad,other)==Result::Invalid && Same(other.Plan(),plan), "nonzero creation length rejected");
    bad=plan; bad.name=L"../outside.blob";
    Check(directory.ReopenPrepared(bad,other)==Result::Invalid && Same(other.Plan(),plan), "path escape plan rejected");
    Check(directory.ReopenPrepared(plan,other)==Result::Busy && Same(other.Plan(),plan), "exclusive same-file reopen preserves occupied output");
    Check(other.Abort()==Result::DeletePending && !other.IsValid() && !fs::exists(path/plan.name), "explicit abort closes exact original handle");
}
void RecoverAndLease(const fs::path& root) {
    const auto path=root/L"recovery"; auto directory=Open(path);
    const Bytes bytes{'a','b','c'};
    cf::PreparedWrite prepared;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),prepared)==Result::Ok, "prepare partial write fixture");
    const auto plan=prepared.Plan(); prepared={};
    Write(path/plan.name,Bytes(37,0x8f),OPEN_EXISTING);
    Check(directory.ReopenPrepared(plan,prepared)==Result::Ok, "trusted identity permits partial envelope overwrite");
    bool complete=true;
    Check(prepared.Inspect(complete)==Result::Ok && !complete, "same-handle inspect reports partial bytes incomplete without requiring header");
    cf::BlobInfo info;
    Check(prepared.Complete(bytes.data(),bytes.size(),info)==Result::Ok, "retry writes complete envelope from offset zero");
    Envelope(path,info,plan,bytes);
    cf::ReadLease lease; Check(directory.Acquire(info,lease)==Result::Ok, "hold real completed read lease");
    cf::PreparedWrite keeper;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),keeper)==Result::Ok, "prepare output preservation sentinel");
    const auto keeperPlan=keeper.Plan();
    Check(directory.ReopenPrepared(plan,keeper)==Result::Busy && Same(keeper.Plan(),keeperPlan), "read lease prevents exclusive reopen and preserves output");
    lease={};
    Check(directory.ReopenPrepared(plan,keeper)==Result::Ok && Same(keeper.Plan(),plan), "completed file also reopens under trusted plan");
    complete=false;
    Check(keeper.Inspect(complete)==Result::Ok && complete, "same-handle inspect verifies complete original envelope and payload");
    Check(fs::file_size(path/keeperPlan.name)==0, "successful replacement leaves former prepared generation");
    auto wrong=plan; wrong.createdIdentity.volume^=1;
    cf::PreparedWrite another;
    Check(directory.ReopenPrepared(keeperPlan,another)==Result::Ok, "reopen previous output residual");
    keeper={};
    Check(directory.ReopenPrepared(wrong,another)==Result::Conflict && Same(another.Plan(),keeperPlan), "volume mismatch preserves existing output");
    wrong=plan; wrong.createdIdentity.fileIndex^=1;
    Check(directory.ReopenPrepared(wrong,another)==Result::Conflict && Same(another.Plan(),keeperPlan), "file index mismatch preserves existing output");
    Write(path/plan.name,Bytes(static_cast<std::size_t>(plan.completedBytes+1),0x41),OPEN_EXISTING);
    Check(directory.ReopenPrepared(plan,another)==Result::Conflict && Same(another.Plan(),keeperPlan), "overlong actual file rejected without replacement");
    Check(fs::file_size(path/plan.name)==plan.completedBytes+1, "rejected recovery neither truncates nor deletes file");
    Check(another.Abort()==Result::DeletePending, "abort only keeper residual");
}
void CorruptComplete(const fs::path& root) {
    const auto path=root/L"corrupt"; auto directory=Open(path);
    const Bytes bytes{'a','b','c'};
    cf::PreparedWrite prepared;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),prepared)==Result::Ok, "prepare corruption fixture");
    const auto plan=prepared.Plan(); cf::BlobInfo info;
    Check(prepared.Complete(bytes.data(),bytes.size(),info)==Result::Ok, "create verified full envelope");
    const auto original=Raw(path/plan.name);
    for (unsigned mode=0;mode<3;++mode) {
        auto changed=original; auto expected=plan;
        if(mode==0) changed[0]^=1;
        if(mode==1) changed[128]^=1;
        if(mode==2) expected.spec.key[0]^=1;
        Write(path/plan.name,changed,OPEN_EXISTING);
        Check(directory.ReopenPrepared(expected,prepared)==Result::Ok, "reopen only checks pinned identity and length before inspecting content");
        for(bool before : {false,true}) {
            bool complete=before;
            Check(prepared.Inspect(complete)==Result::Corrupt && complete==before,
                "full-length bad header, payload or plan metadata is corruption and preserves output");
        }
        Check(prepared.IsValid() && Same(prepared.Plan(),expected), "failed inspection retains pinned recovery handle");
        prepared={};
        Check(Raw(path/plan.name)==changed, "inspection does not repair or delete corrupted full file");
    }
    Write(path/plan.name,original,OPEN_EXISTING);
    Check(directory.ReopenPrepared(plan,prepared)==Result::Ok, "reopen restored complete fixture");
    bool complete=false;
    Check(prepared.Inspect(complete)==Result::Ok && complete, "same handle recognizes restored complete file");
    Check(prepared.Abort()==Result::DeletePending, "explicit cleanup of inspected owned fixture");
}
void ReplacedIdentity(const fs::path& root) {
    const auto path=root/L"identity"; auto directory=Open(path);
    const Bytes bytes{'a','b','c'};
    cf::PreparedWrite prepared;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),prepared)==Result::Ok, "prepare identity replacement fixture");
    const auto plan=prepared.Plan(); prepared={};
    const auto replacement=path/L"owned-replacement.tmp", retained=path/L"owned-original.retained";
    Write(replacement,Bytes{},CREATE_NEW);
    const auto replacementIdentity=Identity(replacement);
    Check(replacementIdentity.fileIndex!=plan.createdIdentity.fileIndex, "two simultaneously existing files have distinct indices");
    directory={}; // Directory guards intentionally prevent namespace renames.
    Check(MoveFileExW((path/plan.name).c_str(),retained.c_str(),0)!=FALSE, "retain owned original under another name");
    Check(MoveFileExW(replacement.c_str(),(path/plan.name).c_str(),0)!=FALSE, "place owned replacement at prepared name");
    directory=Open(path);
    cf::PreparedWrite keeper;
    Check(directory.Prepare(Spec(),bytes.data(),bytes.size(),keeper)==Result::Ok, "prepare replacement rejection output");
    const auto keeperPlan=keeper.Plan();
    Check(directory.ReopenPrepared(plan,keeper)==Result::Conflict && Same(keeper.Plan(),keeperPlan), "same pathname with another real identity rejected");
    Check(Same(Identity(path/plan.name),replacementIdentity) && Same(Identity(retained),plan.createdIdentity), "identity rejection leaves both files unchanged");
    Check(keeper.Abort()==Result::DeletePending, "abort only separately prepared sentinel");
}
}
static_assert(!std::is_copy_constructible_v<cf::PreparedWrite> && !std::is_copy_assignable_v<cf::PreparedWrite>);
static_assert(std::is_nothrow_move_constructible_v<cf::PreparedWrite> && std::is_nothrow_move_assignable_v<cf::PreparedWrite>);
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        Check(argc==3 && std::wstring(argv[1])==L"--store", "usage: --store NEW_ABSOLUTE_DIRECTORY");
        const fs::path root(argv[2]); NewDirectory(root);
        Basic(root); RejectionsAndMoves(root); RecoverAndLease(root); ReplacedIdentity(root); CorruptComplete(root);
        std::printf("PASS prepared cache files checks=%u evidence=%ls\n",checks,root.c_str());
        std::printf("BOUNDARY file primitives only; no trusted intent authorization, hard crash, power loss or injected I/O fault claim\n");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL prepared cache files checks=%u: %s\n",checks,e.what());return 1;}
      catch(...) {std::fprintf(stderr,"FAIL prepared cache files unexpected exception\n");return 2;}
}
