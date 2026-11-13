#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_files.hpp"
#include "cache_files_crypto.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace mikumikustudio::cache_files {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() noexcept = default;
    explicit Handle(HANDLE h) noexcept : value(h) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Close(); value = other.value; other.value = INVALID_HANDLE_VALUE; }
        return *this;
    }
    void Close() noexcept { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    bool Valid() const noexcept { return value != INVALID_HANDLE_VALUE; }
};
Result Error(DWORD error = GetLastError()) noexcept {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return Result::Missing;
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_DELETE_PENDING)
        return Result::Busy;
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) return Result::Conflict;
    return Result::Io;
}
bool OwnerValid(const Owner& owner) noexcept {
    return std::any_of(owner.begin(), owner.end(), [](auto byte) { return byte != 0; });
}
bool SpecValid(const Spec& spec) noexcept {
    return OwnerValid(spec.owner) && (spec.kind == Kind::Effect || spec.kind == Kind::Texture ||
        spec.kind == Kind::CatalogSnapshot || spec.kind == Kind::CatalogCommit || spec.kind == Kind::CatalogCheckpoint || spec.kind == Kind::IndexPage || spec.kind == Kind::CatalogIntent);
}
bool NameValid(const std::wstring& name) noexcept {
    if (name.size() != 37 || name.compare(32, 5, L".blob") != 0) return false;
    return std::all_of(name.begin(), name.begin()+32, [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
bool PlanValid(const WritePlan& plan) noexcept {
    return NameValid(plan.name) && SpecValid(plan.spec) && plan.payloadBytes <= MaximumPayloadBytes &&
        plan.createdIdentity.fileBytes == 0 && plan.completedBytes == HeaderBytes + plan.payloadBytes;
}
bool SameFile(const Identity& a, const Identity& b) noexcept {
    return a.volume == b.volume && a.fileIndex == b.fileIndex;
}
std::wstring Name(const Owner& generation) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name; name.reserve(37);
    for (auto byte : generation) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    name += L".blob"; return name;
}
bool ComponentValid(const std::wstring& part) {
    if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
    for (wchar_t c : part)
        if (c < 32 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*' || c == L':') return false;
    auto stem = part.substr(0, part.find(L'.'));
    for (auto& c : stem) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" || stem == L"CONOUT$") return false;
    if (stem.size() == 4 && (stem.compare(0,3,L"COM") == 0 || stem.compare(0,3,L"LPT") == 0) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xb9 || stem[3] == 0xb2 || stem[3] == 0xb3)) return false;
    return true;
}
bool InputPath(const std::filesystem::path& path, std::wstring& drive, std::vector<std::wstring>& parts) {
    auto text = path.wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.size() < 4 || !((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) ||
        text[1] != L':' || text[2] != L'\\') return false;
    drive = text.substr(0,3);
    std::size_t begin = 3;
    while (begin < text.size()) {
        const auto end = text.find(L'\\', begin);
        auto part = text.substr(begin, end == std::wstring::npos ? end : end-begin);
        if (!ComponentValid(part)) return false;
        parts.push_back(std::move(part));
        if (end == std::wstring::npos) return true;
        begin = end+1;
    }
    return false;
}
Result Guard(const std::wstring& path, Handle& output) noexcept {
    // Metadata-only opens do not participate in share-access checks. Directory
    // read access is required to prevent rename and reparse conversion.
    Handle candidate(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!candidate.Valid()) return Error();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(candidate.value, &info)) return Result::Io;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Result::Invalid;
    output = std::move(candidate); return Result::Ok;
}
// Only called after the immediate child has been strictly guarded and stored.
// Denying DELETE keeps that child present, so the ancestor cannot become empty
// and acquire a reparse point. Allowing WRITE on this proven nonempty ancestor
// permits atomic replacement of unrelated siblings (for example settings.json).
// A public Directory's final root must never use this weaker ancestor guard.
Result ShareAncestorWrites(const std::wstring& path, Handle& original) noexcept {
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!original.Valid() || !GetFileInformationByHandle(original.value, &before)) return Result::Io;
    if (!(before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Result::Invalid;
    Handle replacement(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!replacement.Valid()) return Error();
    if (!GetFileInformationByHandle(replacement.value, &after)) return Result::Io;
    if (!(after.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (after.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Result::Invalid;
    if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
        before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow)
        return Result::Conflict;
    // Both parent handles and the strict child remain live until this exchange.
    original = std::move(replacement);
    return Result::Ok;
}
bool Seek(HANDLE file, std::uint64_t offset) noexcept {
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
}
bool ReadExact(HANDLE file, void* output, std::size_t length) noexcept {
    auto bytes = static_cast<std::uint8_t*>(output);
    while (length) {
        const DWORD count = static_cast<DWORD>((std::min)(length, std::size_t(65536)));
        DWORD done = 0;
        if (!ReadFile(file, bytes, count, &done, nullptr) || !done || done > count) return false;
        bytes += done; length -= done;
    }
    return true;
}
bool WriteExact(HANDLE file, const void* input, std::size_t length) noexcept {
    auto bytes = static_cast<const std::uint8_t*>(input);
    while (length) {
        const DWORD count = static_cast<DWORD>((std::min)(length, std::size_t(65536)));
        DWORD done = 0;
        if (!WriteFile(file, bytes, count, &done, nullptr) || !done || done > count) return false;
        bytes += done; length -= done;
    }
    return true;
}
void Put(std::uint8_t* bytes, std::uint64_t value, unsigned length) noexcept {
    for (unsigned i=0; i<length; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (i*8));
}
std::uint64_t Get(const std::uint8_t* bytes, unsigned length) noexcept {
    std::uint64_t value = 0;
    for (unsigned i=0; i<length; ++i) value |= std::uint64_t(bytes[i]) << (i*8);
    return value;
}
bool Equal(const BlobInfo& a, const BlobInfo& b) noexcept {
    return a.name == b.name && a.spec.owner == b.spec.owner && a.spec.kind == b.spec.kind && a.spec.key == b.spec.key &&
        a.payloadBytes == b.payloadBytes && a.payloadDigest == b.payloadDigest && a.identity.volume == b.identity.volume &&
        a.identity.fileIndex == b.identity.fileIndex && a.identity.fileBytes == b.identity.fileBytes;
}
void Swap(BlobInfo& a, BlobInfo& b) noexcept {
    a.name.swap(b.name); std::swap(a.spec,b.spec); std::swap(a.payloadDigest,b.payloadDigest);
    std::swap(a.payloadBytes,b.payloadBytes); std::swap(a.identity,b.identity);
}
Result FileIdentity(HANDLE file, Identity& output, bool requireSingleLink = true) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return Result::Io;
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !info.nNumberOfLinks || (requireSingleLink && info.nNumberOfLinks != 1)) return Result::Corrupt;
    output.volume = info.dwVolumeSerialNumber;
    output.fileIndex = (std::uint64_t(info.nFileIndexHigh)<<32) | info.nFileIndexLow;
    output.fileBytes = (std::uint64_t(info.nFileSizeHigh)<<32) | info.nFileSizeLow;
    return Result::Ok;
}
Result Header(HANDLE file, const std::wstring& name, const Owner& owner, std::uint64_t cap, BlobInfo& info) {
    const auto result = FileIdentity(file, info.identity);
    if (result != Result::Ok) return result;
    if (info.identity.fileBytes < HeaderBytes) return Result::Corrupt;
    std::array<std::uint8_t,128> header{};
    if (!Seek(file,0) || !ReadExact(file,header.data(),header.size())) return Result::Io;
    if (std::memcmp(header.data(),"MMSCacheBlob0001",16) || Get(header.data()+16,4) != 1) return Result::Corrupt;
    info.spec.kind = static_cast<Kind>(Get(header.data()+20,4));
    std::copy_n(header.data()+24,16,info.spec.owner.begin());
    std::copy_n(header.data()+40,32,info.spec.key.begin());
    info.payloadBytes = Get(header.data()+72,8);
    std::copy_n(header.data()+80,32,info.payloadDigest.begin());
    Owner generation{}; std::copy_n(header.data()+112,16,generation.begin());
    if (!SpecValid(info.spec) || info.spec.owner != owner || Name(generation) != name ||
        info.payloadBytes > MaximumPayloadBytes || info.identity.fileBytes != HeaderBytes+info.payloadBytes) return Result::Corrupt;
    if (info.payloadBytes > cap) return Result::Limit;
    info.name = name; return Result::Ok;
}
Result VerifyPayload(HANDLE file, const BlobInfo& info) {
    crypto::Sha256 hash;
    if (!hash.Start() || !Seek(file,HeaderBytes)) return Result::Io;
    std::array<std::uint8_t,65536> buffer{};
    auto remaining = info.payloadBytes;
    while (remaining) {
        const auto count = static_cast<std::size_t>((std::min)(remaining,std::uint64_t(buffer.size())));
        if (!ReadExact(file,buffer.data(),count) || !hash.Update(buffer.data(),count)) return Result::Io;
        remaining -= count;
    }
    Digest digest{};
    if (!hash.Finish(digest)) return Result::Io;
    return digest == info.payloadDigest ? Result::Ok : Result::Corrupt;
}
bool Disposition(HANDLE file, bool requireSingleLink = true) noexcept;
bool ObservationLimitsValid(const ObservationLimits& limits) noexcept {
    return limits.maximumStreams&&limits.maximumStreams<=4096&&
        limits.maximumStreamBufferBytes>=32&&limits.maximumStreamBufferBytes<=4u*1024*1024;
}
struct ObservationFailure { Result result; };
struct StreamObservation { std::wstring name;std::uint64_t logical=0,allocation=0; };
int Ordinal(const std::wstring& a,const std::wstring& b) {
    const auto result=CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE);
    if(!result)throw ObservationFailure{Result::Io};return result;
}
bool SameBasic(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwFileAttributes==b.dwFileAttributes&&a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&
        a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
        a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&a.nNumberOfLinks==b.nNumberOfLinks&&
        a.ftCreationTime.dwHighDateTime==b.ftCreationTime.dwHighDateTime&&a.ftCreationTime.dwLowDateTime==b.ftCreationTime.dwLowDateTime&&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
bool SameStandard(const FILE_STANDARD_INFO& a,const FILE_STANDARD_INFO& b) noexcept {
    return a.AllocationSize.QuadPart==b.AllocationSize.QuadPart&&a.EndOfFile.QuadPart==b.EndOfFile.QuadPart&&
        a.NumberOfLinks==b.NumberOfLinks&&a.DeletePending==b.DeletePending&&a.Directory==b.Directory;
}
bool DeletionStreamOnly(HANDLE file,const FILE_STANDARD_INFO& standard) noexcept {
    // Deletion also removes ADS which no blob/raw plan owns. A single default
    // stream fits this fixed buffer; larger/multiple lists need no allocation
    // because they cannot authorize deletion. Keep CREATE_NEW failure cleanup
    // allocation-free, including under injected allocation failures.
    std::array<unsigned char,128> bytes{};
    SetLastError(ERROR_SUCCESS);
    const bool ok=GetFileInformationByHandleEx(file,FileStreamInfo,bytes.data(),static_cast<DWORD>(bytes.size()))!=FALSE;
    const auto error=GetLastError();
    const bool empty=error==ERROR_HANDLE_EOF&&(!ok||
        std::all_of(bytes.begin(),bytes.end(),[](unsigned char value){return value==0;}));
    if(empty&&!standard.EndOfFile.QuadPart&&!standard.AllocationSize.QuadPart)return true;
    if(!ok&&!empty)return false; // includes MORE_DATA / unsupported observation
    constexpr wchar_t name[]=L"::$DATA";
    const std::size_t fixed=offsetof(FILE_STREAM_INFO,StreamName);
    FILE_STREAM_INFO entry{};std::memcpy(&entry,bytes.data(),fixed);
    if(empty||entry.NextEntryOffset||entry.StreamNameLength!=sizeof(name)-sizeof(wchar_t)||
       std::memcmp(bytes.data()+fixed,name,sizeof(name)-sizeof(wchar_t))||
       entry.StreamSize.QuadPart!=standard.EndOfFile.QuadPart||
       entry.StreamAllocationSize.QuadPart!=standard.AllocationSize.QuadPart){
        SetLastError(ERROR_INVALID_DATA);return false;
    }
    return true;
}
bool Disposition(HANDLE file, bool requireSingleLink) noexcept {
    // All callers hold this exact file handle without write/delete sharing. Verify the complete
    // stream namespace immediately before disposition, not during ordinary
    // reads; reopening a trusted default stream does not own later-added ADS.
    BY_HANDLE_FILE_INFORMATION before{},after{};FILE_STANDARD_INFO first{},last{};
    if(!GetFileInformationByHandle(file,&before)||
       !GetFileInformationByHandleEx(file,FileStandardInfo,&first,sizeof first))return false;
    if((before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
       !before.nNumberOfLinks||before.nNumberOfLinks!=first.NumberOfLinks||
       (requireSingleLink&&first.NumberOfLinks!=1)||first.Directory||first.DeletePending||
       first.EndOfFile.QuadPart<0||first.AllocationSize.QuadPart<0||
       ((std::uint64_t(before.nFileSizeHigh)<<32)|before.nFileSizeLow)!=static_cast<std::uint64_t>(first.EndOfFile.QuadPart)){
        SetLastError(ERROR_INVALID_DATA);return false;
    }
    DWORD volume=0,component=0,flags=0;
    if(!GetVolumeInformationByHandleW(file,nullptr,0,&volume,&component,&flags,nullptr,0))return false;
    if(volume!=before.dwVolumeSerialNumber){SetLastError(ERROR_INVALID_DATA);return false;}
    if((flags&FILE_NAMED_STREAMS)&&(!DeletionStreamOnly(file,first)||!DeletionStreamOnly(file,first)))return false;
    if(!GetFileInformationByHandleEx(file,FileStandardInfo,&last,sizeof last)||
       !GetFileInformationByHandle(file,&after))return false;
    if(!SameBasic(before,after)||!SameStandard(first,last)){SetLastError(ERROR_INVALID_DATA);return false;}
    FILE_DISPOSITION_INFO info{};info.DeleteFile=TRUE;
    return SetFileInformationByHandle(file,FileDispositionInfo,&info,sizeof info)!=FALSE;
}
Result Streams(HANDLE file,const FILE_STANDARD_INFO& standard,const ObservationLimits& limits,
    std::vector<StreamObservation>& output) {
    std::vector<unsigned char> bytes((std::min)(limits.maximumStreamBufferBytes,std::size_t(4096)));
    for(;;) {
        std::fill(bytes.begin(),bytes.end(),0);
        SetLastError(ERROR_SUCCESS);
        const bool ok=GetFileInformationByHandleEx(file,FileStreamInfo,bytes.data(),static_cast<DWORD>(bytes.size()))!=FALSE;
        const auto error=GetLastError();
        // FileStreamInfo documents a successful empty result with HANDLE_EOF.
        // A valid returned list still wins over a leftover error value: never
        // turn a successful ADS record into a zero-byte observation.
        const bool empty=error==ERROR_HANDLE_EOF&&(!ok||
            std::all_of(bytes.begin(),bytes.end(),[](unsigned char value){return value==0;}));
        if(empty) {
            if(!standard.Directory&&(standard.EndOfFile.QuadPart||standard.AllocationSize.QuadPart))return Result::Conflict;
            output.clear();return Result::Ok;
        }
        if(ok)break;
        if(error!=ERROR_MORE_DATA&&error!=ERROR_INSUFFICIENT_BUFFER)return Error(error);
        if(bytes.size()==limits.maximumStreamBufferBytes)return Result::Limit;
        bytes.resize((std::min)(limits.maximumStreamBufferBytes,bytes.size()*2));
    }
    const std::size_t fixed=offsetof(FILE_STREAM_INFO,StreamName);
    std::vector<StreamObservation> streams;
    std::size_t offset=0;
    for(;;) {
        if(offset>bytes.size()||bytes.size()-offset<fixed)return Result::Corrupt;
        FILE_STREAM_INFO record{};std::memcpy(&record,bytes.data()+offset,fixed);
        const auto nameBytes=static_cast<std::size_t>(record.StreamNameLength);
        if(!nameBytes||(nameBytes%sizeof(wchar_t))||nameBytes>bytes.size()-offset-fixed||
            record.StreamSize.QuadPart<0||record.StreamAllocationSize.QuadPart<0)return Result::Corrupt;
        if(streams.size()==limits.maximumStreams)return Result::Limit;
        StreamObservation stream;
        stream.name.resize(nameBytes/sizeof(wchar_t));std::memcpy(stream.name.data(),bytes.data()+offset+fixed,nameBytes);
        if(stream.name.find(L'\0')!=std::wstring::npos)return Result::Corrupt;
        stream.logical=static_cast<std::uint64_t>(record.StreamSize.QuadPart);
        stream.allocation=static_cast<std::uint64_t>(record.StreamAllocationSize.QuadPart);
        streams.push_back(std::move(stream));
        if(!record.NextEntryOffset)break;
        const auto next=static_cast<std::size_t>(record.NextEntryOffset);
        if(next<fixed+nameBytes||next%8||next>bytes.size()-offset||bytes.size()-offset-next<fixed)return Result::Corrupt;
        offset+=next;
    }
    std::sort(streams.begin(),streams.end(),[](const auto&a,const auto&b){return Ordinal(a.name,b.name)==CSTR_LESS_THAN;});
    for(std::size_t i=1;i<streams.size();++i)if(Ordinal(streams[i-1].name,streams[i].name)==CSTR_EQUAL)return Result::Corrupt;
    output.swap(streams);return Result::Ok;
}
Result ObserveHandle(HANDLE file,const ObservationLimits& limits,FileObservation& output) noexcept {
    try {
        if(file==INVALID_HANDLE_VALUE||!file||!ObservationLimitsValid(limits))return Result::Invalid;
        if(GetFileType(file)!=FILE_TYPE_DISK)return Result::Invalid;
        BY_HANDLE_FILE_INFORMATION before{},after{};FILE_STANDARD_INFO first{},last{};
        if(!GetFileInformationByHandle(file,&before)||!GetFileInformationByHandleEx(file,FileStandardInfo,&first,sizeof(first)))return Error();
        if(before.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return Result::Invalid;
        if(first.AllocationSize.QuadPart<0||first.EndOfFile.QuadPart<0)return Result::Corrupt;
        const auto logical=(std::uint64_t(before.nFileSizeHigh)<<32)|before.nFileSizeLow;
        if(logical!=static_cast<std::uint64_t>(first.EndOfFile.QuadPart)||before.nNumberOfLinks!=first.NumberOfLinks||
            bool(before.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=bool(first.Directory))return Result::Conflict;
        DWORD volume=0,component=0,flags=0;
        if(!GetVolumeInformationByHandleW(file,nullptr,0,&volume,&component,&flags,nullptr,0))return Error();
        if(volume!=before.dwVolumeSerialNumber)return Result::Conflict;
        FileObservation candidate;
        candidate.identity={volume,(std::uint64_t(before.nFileIndexHigh)<<32)|before.nFileIndexLow,logical};
        candidate.allocationBytes=static_cast<std::uint64_t>(first.AllocationSize.QuadPart);
        candidate.attributes=before.dwFileAttributes;candidate.links=first.NumberOfLinks;
        candidate.kind=first.Directory?ObservationKind::Directory:ObservationKind::File;
        candidate.deletePending=first.DeletePending!=FALSE;candidate.namedStreamsSupported=(flags&FILE_NAMED_STREAMS)!=0;
        if(candidate.namedStreamsSupported) {
            std::vector<StreamObservation> a,b;
            auto result=Streams(file,first,limits,a);if(result!=Result::Ok)return result;
            result=Streams(file,first,limits,b);if(result!=Result::Ok)return result;
            if(a.size()!=b.size())return Result::Conflict;
            bool defaultSeen=false;
            for(std::size_t i=0;i<a.size();++i) {
                if(a[i].name!=b[i].name||a[i].logical!=b[i].logical||a[i].allocation!=b[i].allocation)return Result::Conflict;
                if(a[i].logical>UINT64_MAX-candidate.streamLogicalBytes||a[i].allocation>UINT64_MAX-candidate.streamAllocationBytes)return Result::Limit;
                candidate.streamLogicalBytes+=a[i].logical;candidate.streamAllocationBytes+=a[i].allocation;
                if(Ordinal(a[i].name,L"::$DATA")==CSTR_EQUAL) {
                    defaultSeen=true;
                    if(!first.Directory&&(a[i].logical!=logical||a[i].allocation!=candidate.allocationBytes))return Result::Conflict;
                }
            }
            if(!first.Directory&&!defaultSeen&&(logical||candidate.allocationBytes))return Result::Conflict;
            candidate.streamCount=static_cast<std::uint32_t>(a.size());
        } else if(!first.Directory) {
            candidate.streamLogicalBytes=logical;candidate.streamAllocationBytes=candidate.allocationBytes;candidate.streamCount=1;
        }
        if(!GetFileInformationByHandleEx(file,FileStandardInfo,&last,sizeof(last))||!GetFileInformationByHandle(file,&after))return Error();
        if(!SameBasic(before,after)||!SameStandard(first,last))return Result::Conflict;
        output=candidate;return Result::Ok;
    } catch(const ObservationFailure& failure) {return failure.result;}
      catch(...) {return Result::Io;}
}
// This owner is used only for our own CREATE_NEW handle. Failure cleanup does
// not trust a pathname or require that the partial envelope be valid.
struct NewFile {
    Handle file;
    bool committed = false;
    ~NewFile() { if (!committed && file.Valid()) Disposition(file.value); }
    Result Fail(Result result) noexcept {
        if (file.Valid()) {
            if (!Disposition(file.value)) result = Result::Io;
            file.Close();
        }
        return result;
    }
};
}

struct DirectoryState {
    std::shared_ptr<DirectoryState> parent;
    std::wstring path;
    std::vector<Handle> guards;
    ~DirectoryState() {
        // Close broadened ancestors before their protected descendants, also
        // on failed Open. Do not depend on vector element destruction order.
        for (auto& guard : guards) guard.Close();
    }
};
struct InventoryState {
    std::shared_ptr<DirectoryState> root;
    DirectoryIdentity identity;
    std::vector<Handle> guards;
    std::vector<InventoryEntry> entries;
};
struct LeaseState {
    std::shared_ptr<DirectoryState> directory;
    Handle file;
    BlobInfo info;
};
struct PreparedWriteState {
    std::shared_ptr<DirectoryState> directory;
    Handle file;
    WritePlan plan;
};
struct RawSourceLeaseState {
    std::shared_ptr<DirectoryState> directory;
    Handle file;
    RawSourceInfo info;
    std::wstring path;
};
struct RawSourceCleanupState {
    std::shared_ptr<DirectoryState> directory;
    Handle file;
    std::wstring name;
};
struct RawSourceWriteState {
    std::shared_ptr<DirectoryState> directory;
    Handle source, file;
    Identity sourceIdentity{};
    RawSourcePlan plan;
    std::unique_ptr<RawSourceLeaseState> candidate;
    std::array<std::uint8_t, 65536> buffer{};
    std::unique_ptr<crypto::Sha256> copyHash, verifyHash;
};
namespace {
bool RawExtension(const std::wstring& extension) noexcept {
    return !extension.empty() && extension.size() <= 16 &&
        std::all_of(extension.begin(), extension.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
        });
}
bool RawNameValid(const std::wstring& name) noexcept {
    if (name.size() < 38 || name.size() > 53 || name.compare(32, 5, L".raw.")) return false;
    for (size_t i=0; i<32; ++i) if (!((name[i]>=L'0' && name[i]<=L'9') || (name[i]>=L'a' && name[i]<=L'f'))) return false;
    for (size_t i=37; i<name.size(); ++i) if (!((name[i]>=L'0' && name[i]<=L'9') || (name[i]>=L'a' && name[i]<=L'z'))) return false;
    return true;
}
bool RawInfoValid(const RawSourceInfo& info) noexcept {
    return RawNameValid(info.name) && info.payloadBytes && info.payloadBytes <= INT64_MAX &&
        info.identity.fileBytes == info.payloadBytes;
}
bool Cancelled(const std::atomic_bool* value) noexcept {
    return value && value->load(std::memory_order_relaxed);
}
Result RawIdentity(HANDLE file, Identity& identity) noexcept {
    if (GetFileType(file) != FILE_TYPE_DISK) return Result::Invalid;
    return FileIdentity(file, identity);
}
Result SourceIdentity(HANDLE file,Identity& identity) noexcept {
    if(GetFileType(file)!=FILE_TYPE_DISK)return Result::Invalid;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info))return Result::Io;
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))return Result::Invalid;
    identity.volume=info.dwVolumeSerialNumber;
    identity.fileIndex=(std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
    identity.fileBytes=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    return Result::Ok;
}
Result SourceMatches(HANDLE file,const Identity& expected) noexcept {
    Identity actual{};const auto result=SourceIdentity(file,actual);
    if(result!=Result::Ok)return result;
    return SameFile(actual,expected)&&actual.fileBytes==expected.fileBytes?Result::Ok:Result::Conflict;
}
Result RawMatches(HANDLE file, const Identity& expected) noexcept {
    Identity actual{}; const auto result=RawIdentity(file,actual);
    if(result!=Result::Ok)return result;
    return SameFile(actual,expected) && actual.fileBytes==expected.fileBytes ? Result::Ok : Result::Conflict;
}
Result RawHash(HANDLE file, std::uint64_t bytes, crypto::Sha256& hash,
    std::array<std::uint8_t,65536>& buffer, Digest& output, const std::atomic_bool* cancelled=nullptr) noexcept {
    if(!Seek(file,0))return Result::Io;
    while(bytes){
        if(Cancelled(cancelled))return Result::Cancelled;
        const auto count=static_cast<size_t>((std::min)(bytes,std::uint64_t(buffer.size())));
        if(!ReadExact(file,buffer.data(),count)||!hash.Update(buffer.data(),count))return Result::Io;
        bytes-=count;
    }
    if(Cancelled(cancelled))return Result::Cancelled;
    return hash.Finish(output)?Result::Ok:Result::Io;
}
Result ReopenRawWriter(RawSourceWriteState& state) noexcept {
    if(state.file.Valid())return Result::Ok;
    Handle candidate(CreateFileW(state.candidate->path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(!candidate.Valid())return Error();
    Identity actual{}; const auto result=RawIdentity(candidate.value,actual);
    if(result!=Result::Ok)return result;
    if(!SameFile(actual,state.plan.createdIdentity)||actual.fileBytes>state.plan.sourceBytes)return Result::Conflict;
    state.file=std::move(candidate); return Result::Ok;
}
}
struct LockState {
    std::shared_ptr<DirectoryState> directory;
    Handle file;
    OVERLAPPED overlap{};
    bool held = false;
    ~LockState() { if (held) UnlockFileEx(file.value,0,1,0,&overlap); }
};

Directory::Directory() noexcept = default;
Result DeleteObservedFile(const std::filesystem::path& path, const Identity& expected) noexcept {
    Handle file(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.Valid()) return Error();
    Identity actual{};
    // The manager owns this directory entry. Other links stay outside its
    // cleanup authorization and remain charged by its physical inventory.
    const auto inspected = FileIdentity(file.value, actual, false);
    if (inspected != Result::Ok) return inspected;
    if (!SameFile(actual, expected) || actual.fileBytes != expected.fileBytes)
        return Result::Conflict;
    return Disposition(file.value, false) ? Result::Ok : Error();
}
Directory::~Directory() = default;
Directory::Directory(Directory&&) noexcept = default;
Directory& Directory::operator=(Directory&&) noexcept = default;
bool Directory::IsValid() const noexcept { return bool(state_); }
Result Directory::Share(Directory& output) const noexcept {
    if (!state_) return Result::Invalid;
    output.state_ = state_;
    return Result::Ok;
}
Result Directory::GetIdentity(DirectoryIdentity& output) const noexcept {
    if (!state_ || state_->guards.empty() || !state_->guards.back().Valid()) return Result::Invalid;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(state_->guards.back().value, &info)) return Error();
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Result::Invalid;
    DirectoryIdentity candidate;
    candidate.volume = info.dwVolumeSerialNumber;
    candidate.fileIndex = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    output = candidate;
    return Result::Ok;
}
ReadLease::ReadLease() noexcept = default;
ReadLease::~ReadLease() = default;
ReadLease::ReadLease(ReadLease&&) noexcept = default;
ReadLease& ReadLease::operator=(ReadLease&&) noexcept = default;
bool ReadLease::IsValid() const noexcept { return bool(state_); }
const BlobInfo& ReadLease::Info() const noexcept { static const BlobInfo empty; return state_ ? state_->info : empty; }
FileLock::FileLock() noexcept = default;
FileLock::~FileLock() = default;
FileLock::FileLock(FileLock&&) noexcept = default;
FileLock& FileLock::operator=(FileLock&&) noexcept = default;
bool FileLock::IsValid() const noexcept { return state_ && state_->held; }
Result FileLock::Observe(FileObservation& output,const ObservationLimits& limits) const noexcept {
    if(!state_)return Result::Invalid;
    if(!state_->file.Valid()||!state_->held)return Result::Busy;
    return ObserveHandle(state_->file.value,limits,output);
}
PreparedWrite::PreparedWrite() noexcept = default;
PreparedWrite::~PreparedWrite() = default;
PreparedWrite::PreparedWrite(PreparedWrite&&) noexcept = default;
PreparedWrite& PreparedWrite::operator=(PreparedWrite&&) noexcept = default;
bool PreparedWrite::IsValid() const noexcept { return state_ && state_->file.Valid(); }
const WritePlan& PreparedWrite::Plan() const noexcept { static const WritePlan empty; return state_ ? state_->plan : empty; }
Result PreparedWrite::Observe(FileObservation& output,const ObservationLimits& limits) const noexcept {
    if(!state_)return Result::Invalid;
    if(!state_->file.Valid())return Result::Busy;
    return ObserveHandle(state_->file.value,limits,output);
}

RawSourceLease::RawSourceLease() noexcept = default;
RawSourceLease::~RawSourceLease() = default;
RawSourceLease::RawSourceLease(RawSourceLease&&) noexcept = default;
RawSourceLease& RawSourceLease::operator=(RawSourceLease&&) noexcept = default;
bool RawSourceLease::IsValid() const noexcept { return state_ && state_->file.Valid(); }
const RawSourceInfo& RawSourceLease::Info() const noexcept { static const RawSourceInfo empty; return state_?state_->info:empty; }
const std::wstring& RawSourceLease::Path() const noexcept { static const std::wstring empty; return state_?state_->path:empty; }
RawSourceCleanup::RawSourceCleanup() noexcept = default;
RawSourceCleanup::~RawSourceCleanup() = default;
RawSourceCleanup::RawSourceCleanup(RawSourceCleanup&&) noexcept = default;
RawSourceCleanup& RawSourceCleanup::operator=(RawSourceCleanup&&) noexcept = default;
bool RawSourceCleanup::IsValid() const noexcept { return state_&&state_->file.Valid(); }
const std::wstring& RawSourceCleanup::Name() const noexcept { static const std::wstring empty; return state_?state_->name:empty; }
Result RawSourceCleanup::Observe(FileObservation& output,const ObservationLimits& limits) const noexcept {
    if(!state_)return Result::Invalid;
    if(!state_->file.Valid())return Result::Busy;
    return ObserveHandle(state_->file.value,limits,output);
}
Result RawSourceCleanup::Abort() noexcept {
    if(!IsValid())return Result::Invalid;
    if(!Disposition(state_->file.value))return Error();
    state_.reset();return Result::DeletePending;
}
RawSourceWrite::RawSourceWrite() noexcept = default;
RawSourceWrite::~RawSourceWrite() = default;
RawSourceWrite::RawSourceWrite(RawSourceWrite&&) noexcept = default;
RawSourceWrite& RawSourceWrite::operator=(RawSourceWrite&&) noexcept = default;
bool RawSourceWrite::IsValid() const noexcept { return state_ && state_->source.Valid(); }
const RawSourcePlan& RawSourceWrite::Plan() const noexcept { static const RawSourcePlan empty; return state_?state_->plan:empty; }
Result RawSourceWrite::Observe(FileObservation& output,const ObservationLimits& limits) const noexcept {
    if(!state_)return Result::Invalid;
    if(!state_->file.Valid())return Result::Busy;
    return ObserveHandle(state_->file.value,limits,output);
}
Result RawSourceWrite::Complete(RawSourceLease& output,const std::atomic_bool* cancelled) noexcept {
    try {
        if(!IsValid())return Result::Invalid;
        if(Cancelled(cancelled))return Result::Cancelled;
        auto& s=*state_;
        // Allocate and initialize hashes before modifying any candidate bytes.
        // A retry receives fresh hashes while retaining the same trusted plan.
        if(!s.copyHash){s.copyHash=std::make_unique<crypto::Sha256>();if(!s.copyHash->Start()){s.copyHash.reset();return Result::Io;}}
        if(!s.verifyHash){s.verifyHash=std::make_unique<crypto::Sha256>();if(!s.verifyHash->Start()){s.verifyHash.reset();return Result::Io;}}
        auto copyHash=std::move(s.copyHash),verifyHash=std::move(s.verifyHash);
        auto result=ReopenRawWriter(s);if(result!=Result::Ok)return result;
        Identity current{};result=RawIdentity(s.file.value,current);if(result!=Result::Ok)return result;
        if(!SameFile(current,s.plan.createdIdentity)||current.fileBytes>s.plan.sourceBytes)return Result::Conflict;
        result=SourceMatches(s.source.value,s.sourceIdentity);if(result!=Result::Ok)return result;
        if(!Seek(s.source.value,0)||!Seek(s.file.value,0)||!SetEndOfFile(s.file.value))return Result::Io;
        auto remaining=s.plan.sourceBytes;
        while(remaining){
            if(Cancelled(cancelled))return Result::Cancelled;
            const auto count=static_cast<size_t>((std::min)(remaining,std::uint64_t(s.buffer.size())));
            if(!ReadExact(s.source.value,s.buffer.data(),count)||!copyHash->Update(s.buffer.data(),count)||
                !WriteExact(s.file.value,s.buffer.data(),count))return Result::Io;
            remaining-=count;
        }
        Digest digest{};
        if(!copyHash->Finish(digest)||!FlushFileBuffers(s.file.value))return Result::Io;
        result=SourceMatches(s.source.value,s.sourceIdentity);if(result!=Result::Ok)return result;
        auto expected=s.plan.createdIdentity;expected.fileBytes=s.plan.sourceBytes;
        result=RawMatches(s.file.value,expected);if(result!=Result::Ok)return result;
        if(Cancelled(cancelled))return Result::Cancelled;
        // The read pin has no WRITE/DELETE desired access, so decoder readers
        // need not share DELETE (or WRITE). Reopening is not assumed atomic:
        // verify exact identity, length AND content after obtaining the pin.
        s.file.Close();
        Handle pinned(CreateFileW(s.candidate->path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if(!pinned.Valid())return Error();
        result=RawMatches(pinned.value,expected);if(result!=Result::Ok)return result;
        Digest verified{};result=RawHash(pinned.value,s.plan.sourceBytes,*verifyHash,s.buffer,verified,cancelled);
        if(result!=Result::Ok)return result;
        if(verified!=digest)return Result::Corrupt;
        result=RawMatches(pinned.value,expected);if(result!=Result::Ok)return result;
        if(Cancelled(cancelled))return Result::Cancelled;
        s.candidate->info.identity=expected;s.candidate->info.payloadDigest=digest;
        s.candidate->file=std::move(pinned);
        output.state_.swap(s.candidate);
        state_.reset();return Result::Ok;
    }catch(...){return Result::Io;}
}
Result RawSourceWrite::Abort() noexcept {
    if(!IsValid())return Result::Invalid;
    auto result=ReopenRawWriter(*state_);if(result!=Result::Ok)return result;
    Identity current{};result=RawIdentity(state_->file.value,current);if(result!=Result::Ok)return result;
    if(!SameFile(current,state_->plan.createdIdentity)||current.fileBytes>state_->plan.sourceBytes)return Result::Conflict;
    if(!Disposition(state_->file.value))return Error();
    state_.reset();return Result::DeletePending;
}

Result PreparedWrite::Inspect(bool& complete) const noexcept {
    try {
        if (!IsValid()) return Result::Invalid;
        const auto& plan = state_->plan;
        Identity actual;
        auto result = FileIdentity(state_->file.value,actual);
        if (result != Result::Ok) return result;
        if (!SameFile(actual,plan.createdIdentity) || actual.fileBytes > plan.completedBytes) return Result::Conflict;
        if (actual.fileBytes < plan.completedBytes) { complete = false; return Result::Ok; }
        BlobInfo verified;
        result = Header(state_->file.value,plan.name,plan.spec.owner,MaximumPayloadBytes,verified);
        if (result != Result::Ok) return result;
        if (verified.spec.kind != plan.spec.kind || verified.spec.key != plan.spec.key ||
            verified.payloadBytes != plan.payloadBytes || verified.payloadDigest != plan.payloadDigest ||
            !SameFile(verified.identity,plan.createdIdentity) || verified.identity.fileBytes != plan.completedBytes)
            return Result::Corrupt;
        result = VerifyPayload(state_->file.value,verified);
        if (result != Result::Ok) return result;
        complete = true;
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result PreparedWrite::Flush() noexcept {
    bool complete = false;
    const auto inspected = Inspect(complete);
    if (inspected != Result::Ok) return inspected;
    if (!complete) return Result::Invalid;
    return FlushFileBuffers(state_->file.value) ? Result::Ok : Result::Io;
}
Result PreparedWrite::Complete(const void* bytes, std::size_t size, BlobInfo& output) noexcept {
    try {
        if (!IsValid() || (size && !bytes) || size != state_->plan.payloadBytes) return Result::Invalid;
        const auto& plan = state_->plan;
        crypto::Sha256 hash; Digest digest{};
        if (!hash.Start() || !hash.Update(bytes,size) || !hash.Finish(digest)) return Result::Io;
        if (digest != plan.payloadDigest) return Result::Conflict;
        Identity before;
        auto result = FileIdentity(state_->file.value,before);
        if (result != Result::Ok) return result;
        if (!SameFile(before,plan.createdIdentity) || before.fileBytes > plan.completedBytes) return Result::Conflict;

        // Prepare all owned output storage before touching the file. A failed
        // verification after writing still leaves the same handle retryable.
        BlobInfo candidate;
        candidate.name = plan.name; candidate.spec = plan.spec;
        candidate.payloadDigest = digest; candidate.payloadBytes = size;
        std::array<std::uint8_t,128> header{};
        std::memcpy(header.data(),"MMSCacheBlob0001",16);
        Put(header.data()+16,1,4); Put(header.data()+20,static_cast<std::uint32_t>(plan.spec.kind),4);
        std::copy(plan.spec.owner.begin(),plan.spec.owner.end(),header.begin()+24);
        std::copy(plan.spec.key.begin(),plan.spec.key.end(),header.begin()+40);
        Put(header.data()+72,size,8);
        std::copy(digest.begin(),digest.end(),header.begin()+80);
        const auto nibble = [](wchar_t c) noexcept -> unsigned { return c <= L'9' ? c-L'0' : c-L'a'+10; };
        for (unsigned i=0; i<16; ++i)
            header[112+i] = static_cast<std::uint8_t>((nibble(plan.name[2*i]) << 4) | nibble(plan.name[2*i+1]));
        if (!Seek(state_->file.value,0) || !WriteExact(state_->file.value,header.data(),header.size()) ||
            !WriteExact(state_->file.value,bytes,size) || !FlushFileBuffers(state_->file.value)) return Result::Io;
        result = FileIdentity(state_->file.value,candidate.identity);
        if (result != Result::Ok) return result;
        if (!SameFile(candidate.identity,plan.createdIdentity) || candidate.identity.fileBytes != plan.completedBytes)
            return Result::Conflict;
        BlobInfo verified;
        result = Header(state_->file.value,plan.name,plan.spec.owner,MaximumPayloadBytes,verified);
        if (result != Result::Ok) return result;
        if (!Equal(candidate,verified)) return Result::Corrupt;
        result = VerifyPayload(state_->file.value,verified);
        if (result != Result::Ok) return result;
        state_.reset();
        Swap(output,candidate);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result PreparedWrite::Abort() noexcept {
    if (!IsValid()) return Result::Invalid;
    if (!Disposition(state_->file.value)) return Error();
    state_.reset();
    return Result::DeletePending;
}

Result Directory::Open(const std::filesystem::path& root, bool create, Directory& output) noexcept {
    try {
        std::wstring drive; std::vector<std::wstring> parts;
        if (!InputPath(root,drive,parts)) return Result::Invalid;
        const auto type = GetDriveTypeW(drive.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return Result::Invalid;
        const auto device = drive.substr(0,2);
        std::array<wchar_t,32768> buffer{};
        if (!QueryDosDeviceW(device.c_str(),buffer.data(),static_cast<DWORD>(buffer.size()))) return Result::Io;
        // SUBST points at another DOS path. Only a real local device is eligible.
        if (std::wstring_view(buffer.data()).substr(0,8) != L"\\Device\\") return Result::Invalid;
        auto candidate = std::make_shared<DirectoryState>();
        candidate->guards.reserve(parts.size()+1);
        Handle volume;
        auto result = Guard(drive,volume);
        if (result != Result::Ok) return result;
        const DWORD count = GetFinalPathNameByHandleW(volume.value,buffer.data(),static_cast<DWORD>(buffer.size()),VOLUME_NAME_GUID);
        if (!count || count >= buffer.size()) return Result::Io;
        candidate->path.assign(buffer.data(),count);
        if (candidate->path.compare(0,11,L"\\\\?\\Volume{") != 0 || candidate->path.back() != L'\\') return Result::Io;
        candidate->path.pop_back();
        candidate->guards.push_back(std::move(volume));
        for (const auto& part : parts) {
            auto ancestorPath = candidate->path;
            // The volume handle names its root directory, not the bare volume.
            if (candidate->guards.size() == 1) ancestorPath += L'\\';
            candidate->path += L'\\'; candidate->path += part;
            Handle directory;
            result = Guard(candidate->path,directory);
            if (result == Result::Missing && create) {
                if (!CreateDirectoryW(candidate->path.c_str(),nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Error();
                result = Guard(candidate->path,directory);
            }
            if (result != Result::Ok) return result;
            candidate->guards.push_back(std::move(directory));
            result = ShareAncestorWrites(ancestorPath,
                candidate->guards[candidate->guards.size()-2]);
            if (result != Result::Ok) return result;
        }
        output.state_.swap(candidate); return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::Child(const std::wstring& name, bool create, Directory& output) const noexcept {
    try {
        if (!state_ || name.find_first_of(L"\\/") != std::wstring::npos || !ComponentValid(name)) return Result::Invalid;
        auto candidate = std::make_shared<DirectoryState>();
        candidate->parent = state_;
        candidate->path = state_->path + L'\\' + name;
        candidate->guards.reserve(1);
        Handle directory;
        auto result = Guard(candidate->path, directory);
        if (result == Result::Missing && create) {
            if (!CreateDirectoryW(candidate->path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Error();
            result = Guard(candidate->path, directory);
        }
        if (result != Result::Ok) return result;
        candidate->guards.push_back(std::move(directory));
        output.state_.swap(candidate);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::Lock(std::uint32_t waitMilliseconds, FileLock& output) const noexcept {
    return LockImpl(waitMilliseconds, output, true);
}
Result Directory::LockExisting(std::uint32_t waitMilliseconds, FileLock& output) const noexcept {
    return LockImpl(waitMilliseconds, output, false);
}
Result Directory::LockImpl(std::uint32_t waitMilliseconds, FileLock& output, bool create) const noexcept {
    try {
        if (!state_ || waitMilliseconds > 2000) return Result::Invalid;
        auto candidate = std::make_unique<LockState>(); candidate->directory = state_;
        const auto path = state_->path + L"\\.lock";
        candidate->file = Handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,create ? OPEN_ALWAYS : OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if (!candidate->file.Valid()) return Error();
        Identity identity;
        const auto checked = FileIdentity(candidate->file.value,identity);
        if (checked != Result::Ok) return checked;
        const auto start = GetTickCount64();
        for (;;) {
            if (LockFileEx(candidate->file.value,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&candidate->overlap)) {
                candidate->held = true; output.state_.swap(candidate); return Result::Ok;
            }
            const auto error = GetLastError();
            if (error != ERROR_LOCK_VIOLATION) return Error(error);
            const auto elapsed = GetTickCount64()-start;
            if (elapsed >= waitMilliseconds) return Result::Busy;
            Sleep(static_cast<DWORD>((std::min)(std::uint64_t(10),std::uint64_t(waitMilliseconds)-elapsed)));
        }
    } catch (...) { return Result::Io; }
}
Result Directory::Create(const Spec& spec, const void* bytes, std::size_t size, BlobInfo& output) const noexcept {
    try {
        if (!state_ || !SpecValid(spec) || (size && !bytes)) return Result::Invalid;
        if (size > MaximumPayloadBytes) return Result::Limit;
        BlobInfo candidate; candidate.spec = spec; candidate.payloadBytes = size;
        crypto::Sha256 hash;
        if (!hash.Start() || !hash.Update(bytes,size) || !hash.Finish(candidate.payloadDigest)) return Result::Io;
        std::array<std::uint8_t,128> header{};
        std::memcpy(header.data(),"MMSCacheBlob0001",16);
        Put(header.data()+16,1,4); Put(header.data()+20,static_cast<std::uint32_t>(spec.kind),4);
        std::copy(spec.owner.begin(),spec.owner.end(),header.begin()+24);
        std::copy(spec.key.begin(),spec.key.end(),header.begin()+40);
        Put(header.data()+72,size,8);
        std::copy(candidate.payloadDigest.begin(),candidate.payloadDigest.end(),header.begin()+80);
        NewFile created;
        for (unsigned attempt=0; attempt<32; ++attempt) {
            Owner generation{};
            if (!crypto::Random(generation)) return Result::Io;
            candidate.name = Name(generation);
            std::copy(generation.begin(),generation.end(),header.begin()+112);
            const auto path = state_->path + L'\\' + candidate.name;
            created.file = Handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if (created.file.Valid()) break;
            const auto error = Error();
            if (error != Result::Conflict) return error;
        }
        if (!created.file.Valid()) return Result::Conflict;
        if (!WriteExact(created.file.value,header.data(),header.size()) || !WriteExact(created.file.value,bytes,size) ||
            !FlushFileBuffers(created.file.value)) return created.Fail(Result::Io);
        const auto result = FileIdentity(created.file.value,candidate.identity);
        if (result != Result::Ok) return created.Fail(result);
        if (candidate.identity.fileBytes != HeaderBytes+size) return created.Fail(Result::Io);
        created.committed = true;
        Swap(output,candidate);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::Prepare(const Spec& spec, const void* bytes, std::size_t size, PreparedWrite& output) const noexcept {
    try {
        if (!state_ || !SpecValid(spec) || (size && !bytes)) return Result::Invalid;
        if (size > MaximumPayloadBytes) return Result::Limit;
        auto candidate = std::make_unique<PreparedWriteState>(); candidate->directory = state_;
        auto& plan = candidate->plan;
        plan.spec = spec; plan.payloadBytes = size; plan.completedBytes = HeaderBytes + size;
        crypto::Sha256 hash;
        if (!hash.Start() || !hash.Update(bytes,size) || !hash.Finish(plan.payloadDigest)) return Result::Io;
        NewFile created;
        for (unsigned attempt=0; attempt<32; ++attempt) {
            Owner generation{};
            if (!crypto::Random(generation)) return Result::Io;
            plan.name = Name(generation);
            const auto path = state_->path + L'\\' + plan.name;
            created.file = Handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if (created.file.Valid()) break;
            const auto error = Error();
            if (error != Result::Conflict) return error;
        }
        if (!created.file.Valid()) return Result::Conflict;
        const auto result = FileIdentity(created.file.value,plan.createdIdentity);
        if (result != Result::Ok) return created.Fail(result);
        if (plan.createdIdentity.fileBytes != 0) return created.Fail(Result::Conflict);
        candidate->file = std::move(created.file); created.committed = true;
        output.state_.swap(candidate);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::PrepareRawSource(const wchar_t* originalPath,std::uint64_t maximumBytes,RawSourceWrite& output,RawSourceCleanup& cleanup) const noexcept {
    try {
        if(cleanup.IsValid())return Result::Busy;
        if(!state_||!originalPath||!*originalPath)return Result::Invalid;
        auto extension=std::filesystem::path(originalPath).extension().wstring();
        if(extension.empty()||extension.front()!=L'.')return Result::Invalid;
        extension.erase(extension.begin());
        for(auto& c:extension)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';
        if(!RawExtension(extension))return Result::Invalid;
        auto candidate=std::make_unique<RawSourceWriteState>();candidate->directory=state_;
        candidate->source=Handle(CreateFileW(originalPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
        if(!candidate->source.Valid())return Error();
        auto result=SourceIdentity(candidate->source.value,candidate->sourceIdentity);
        if(result!=Result::Ok)return result;
        const auto bytes=candidate->sourceIdentity.fileBytes;
        if(!bytes)return Result::Invalid;
        if(bytes>maximumBytes||bytes>INT64_MAX)return Result::Limit;
        candidate->plan.sourceBytes=bytes;
        candidate->candidate=std::make_unique<RawSourceLeaseState>();candidate->candidate->directory=state_;
        candidate->candidate->info.payloadBytes=bytes;
        candidate->copyHash=std::make_unique<crypto::Sha256>();candidate->verifyHash=std::make_unique<crypto::Sha256>();
        if(!candidate->copyHash->Start()||!candidate->verifyHash->Start())return Result::Io;
        auto created=std::make_unique<RawSourceCleanupState>();created->directory=state_;
        for(unsigned attempt=0;attempt<32;++attempt){
            Owner generation{};if(!crypto::Random(generation))return Result::Io;
            candidate->plan.name=Name(generation).substr(0,32)+L".raw."+extension;
            candidate->candidate->info.name=candidate->plan.name;
            candidate->candidate->path=state_->path+L'\\'+candidate->plan.name;
            created->name=candidate->plan.name;
            created->file=Handle(CreateFileW(candidate->candidate->path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,
                nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(created->file.Valid())break;
            const auto error=Error();if(error!=Result::Conflict)return error;
        }
        if(!created->file.Valid())return Result::Conflict;
        // Every allocation precedes CREATE_NEW. From here both success and
        // double-failure publication only exchange preallocated owners.
        result=RawIdentity(created->file.value,candidate->plan.createdIdentity);
        if(result==Result::Ok&&candidate->plan.createdIdentity.fileBytes)result=Result::Conflict;
        if(result!=Result::Ok){
            if(Disposition(created->file.value))return result;
            cleanup.state_.swap(created);return Result::CleanupRequired;
        }
        candidate->file=std::move(created->file);
        output.state_.swap(candidate);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result Directory::AcquireRawSource(const RawSourceInfo& expected,RawSourceLease& output) const noexcept {
    try {
        if(!state_||!RawInfoValid(expected))return Result::Invalid;
        auto candidate=std::make_unique<RawSourceLeaseState>();candidate->directory=state_;candidate->info=expected;
        candidate->path=state_->path+L'\\'+expected.name;
        auto buffer=std::make_unique<std::array<std::uint8_t,65536>>();
        crypto::Sha256 hash;if(!hash.Start())return Result::Io;
        candidate->file=Handle(CreateFileW(candidate->path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if(!candidate->file.Valid())return Error();
        auto result=RawMatches(candidate->file.value,expected.identity);if(result!=Result::Ok)return result;
        Digest actual{};result=RawHash(candidate->file.value,expected.payloadBytes,hash,*buffer,actual);
        if(result!=Result::Ok)return result;
        if(actual!=expected.payloadDigest)return Result::Corrupt;
        result=RawMatches(candidate->file.value,expected.identity);if(result!=Result::Ok)return result;
        output.state_.swap(candidate);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result Directory::RecoverRawSourceCleanup(const DirectoryIdentity& expectedRoot,
    const RawSourcePlan& plan,RawSourceCleanup& output) const noexcept {
    try {
        if(output.IsValid())return Result::Busy;
        if(!state_||!RawNameValid(plan.name)||!plan.sourceBytes||plan.sourceBytes>INT64_MAX||
            plan.createdIdentity.fileBytes)return Result::Invalid;
        DirectoryIdentity currentRoot{};
        const auto rootResult=GetIdentity(currentRoot);if(rootResult!=Result::Ok)return rootResult;
        if(currentRoot.volume!=expectedRoot.volume||currentRoot.fileIndex!=expectedRoot.fileIndex)return Result::Conflict;
        auto candidate=std::make_unique<RawSourceCleanupState>();
        candidate->directory=state_;candidate->name=plan.name;
        const auto path=state_->path+L'\\'+plan.name;
        // No allocations after obtaining the handle whose responsibility must
        // be transferred. Failed checks close it without deleting anything.
        candidate->file=Handle(CreateFileW(path.c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if(!candidate->file.Valid())return Error();
        Identity actual{};const auto checked=RawIdentity(candidate->file.value,actual);
        if(checked!=Result::Ok)return checked;
        if(!SameFile(actual,plan.createdIdentity)||actual.fileBytes>plan.sourceBytes)return Result::Conflict;
        output.state_.swap(candidate);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result Directory::RecoverRawSourceCleanup(const DirectoryIdentity& expectedRoot,
    const RawSourceInfo& captured,RawSourceCleanup& output) const noexcept {
    try {
        if(output.IsValid())return Result::Busy;
        if(!state_||!RawInfoValid(captured))return Result::Invalid;
        DirectoryIdentity currentRoot{};
        auto result=GetIdentity(currentRoot);if(result!=Result::Ok)return result;
        if(currentRoot.volume!=expectedRoot.volume||currentRoot.fileIndex!=expectedRoot.fileIndex)return Result::Conflict;
        auto candidate=std::make_unique<RawSourceCleanupState>();
        candidate->directory=state_;candidate->name=captured.name;
        const auto path=state_->path+L'\\'+captured.name;
        auto buffer=std::make_unique<std::array<std::uint8_t,65536>>();
        crypto::Sha256 hash;if(!hash.Start())return Result::Io;
        // All user-space allocations precede the exclusive open. A failed
        // verification closes this handle without disposition or path cleanup.
        candidate->file=Handle(CreateFileW(path.c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if(!candidate->file.Valid())return Error();
        result=RawMatches(candidate->file.value,captured.identity);if(result!=Result::Ok)return result;
        Digest actual{};result=RawHash(candidate->file.value,captured.payloadBytes,hash,*buffer,actual);
        if(result!=Result::Ok)return result;
        if(actual!=captured.payloadDigest)return Result::Corrupt;
        result=RawMatches(candidate->file.value,captured.identity);if(result!=Result::Ok)return result;
        output.state_.swap(candidate);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result Directory::RemoveRawSource(const RawSourceInfo& expected) const noexcept {
    try {
        if(!state_||!RawInfoValid(expected))return Result::Invalid;
        const auto path=state_->path+L'\\'+expected.name;
        auto buffer=std::make_unique<std::array<std::uint8_t,65536>>();
        crypto::Sha256 hash;if(!hash.Start())return Result::Io;
        Handle file(CreateFileW(path.c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if(!file.Valid())return Error();
        auto result=RawMatches(file.value,expected.identity);if(result!=Result::Ok)return result;
        Digest actual{};result=RawHash(file.value,expected.payloadBytes,hash,*buffer,actual);
        if(result!=Result::Ok)return result;
        if(actual!=expected.payloadDigest)return Result::Corrupt;
        result=RawMatches(file.value,expected.identity);if(result!=Result::Ok)return result;
        if(!Disposition(file.value))return Error();return Result::DeletePending;
    }catch(...){return Result::Io;}
}
Result Directory::ReopenPrepared(const WritePlan& plan, PreparedWrite& output) const noexcept {
    try {
        if (!state_ || !PlanValid(plan)) return Result::Invalid;
        auto candidate = std::make_unique<PreparedWriteState>(); candidate->directory = state_;
        candidate->plan = plan;
        const auto path = state_->path + L'\\' + plan.name;
        candidate->file = Handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if (!candidate->file.Valid()) return Error();
        Identity actual;
        const auto result = FileIdentity(candidate->file.value,actual);
        if (result != Result::Ok) return result;
        if (!SameFile(actual,plan.createdIdentity) || actual.fileBytes > plan.completedBytes) return Result::Conflict;
        output.state_.swap(candidate);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::Inspect(const std::wstring& name, const Owner& owner, std::uint64_t maxPayload, ReadLease& output) const noexcept {
    try {
        if (!state_ || !NameValid(name) || !OwnerValid(owner) || maxPayload > MaximumPayloadBytes) return Result::Invalid;
        auto candidate = std::make_unique<LeaseState>(); candidate->directory = state_;
        const auto path = state_->path + L'\\' + name;
        candidate->file = Handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if (!candidate->file.Valid()) return Error();
        auto result = Header(candidate->file.value,name,owner,maxPayload,candidate->info);
        if (result != Result::Ok) return result;
        result = VerifyPayload(candidate->file.value,candidate->info);
        if (result != Result::Ok) return result;
        output.state_.swap(candidate); return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::Acquire(const BlobInfo& expected, ReadLease& output) const noexcept {
    try {
        if (!state_ || !NameValid(expected.name) || !SpecValid(expected.spec) || expected.payloadBytes > MaximumPayloadBytes) return Result::Invalid;
        ReadLease candidate;
        const auto result = Inspect(expected.name,expected.spec.owner,MaximumPayloadBytes,candidate);
        if (result != Result::Ok) return result;
        if (!Equal(candidate.Info(),expected)) return Result::Conflict;
        output.state_.swap(candidate.state_); return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result ReadLease::ReadAll(std::vector<std::uint8_t>& output) const noexcept {
    try {
        if (!state_) return Result::Invalid;
        BlobInfo current;
        auto result = Header(state_->file.value,state_->info.name,state_->info.spec.owner,MaximumPayloadBytes,current);
        if (result != Result::Ok) return result;
        if (!Equal(current,state_->info)) return Result::Conflict;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(current.payloadBytes));
        if (!Seek(state_->file.value,HeaderBytes) || !ReadExact(state_->file.value,bytes.data(),bytes.size())) return Result::Io;
        crypto::Sha256 hash; Digest digest{};
        if (!hash.Start() || !hash.Update(bytes.data(),bytes.size()) || !hash.Finish(digest)) return Result::Io;
        if (digest != current.payloadDigest) return Result::Corrupt;
        output.swap(bytes); return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Directory::Remove(const BlobInfo& expected) const noexcept {
    try {
        if (!state_ || !NameValid(expected.name) || !SpecValid(expected.spec) || expected.payloadBytes > MaximumPayloadBytes) return Result::Invalid;
        const auto path = state_->path + L'\\' + expected.name;
        Handle file(CreateFileW(path.c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if (!file.Valid()) return Error();
        BlobInfo current;
        auto result = Header(file.value,expected.name,expected.spec.owner,MaximumPayloadBytes,current);
        if (result != Result::Ok) return result;
        result = VerifyPayload(file.value,current);
        if (result != Result::Ok) return result;
        if (!Equal(current,expected)) return Result::Conflict;
        if (!Disposition(file.value)) return Error();
        return Result::DeletePending;
    } catch (...) { return Result::Io; }
}
Result Directory::Enumerate(std::size_t maximumEntries, std::vector<std::wstring>& output) const noexcept {
    return EnumerateNames(maximumEntries, output, true);
}
Result Directory::EnumerateAll(std::size_t maximumEntries, std::vector<std::wstring>& output) const noexcept {
    return EnumerateNames(maximumEntries, output, false);
}
Result Directory::EnumerateNames(std::size_t maximumEntries, std::vector<std::wstring>& output, bool candidatesOnly) const noexcept {
    try {
        if (!state_ || !maximumEntries || maximumEntries > 65536) return Result::Invalid;
        std::vector<std::wstring> names;
        const auto path = state_->path + L"\\*";
        WIN32_FIND_DATAW info{};
        struct Find {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Find() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
        } find;
        find.value = FindFirstFileW(path.c_str(),&info);
        if (find.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return Error();
        } else {
            for (;;) {
                std::wstring name(info.cFileName);
                if (name != L"." && name != L".." && (!candidatesOnly || NameValid(name))) {
                    if (names.size() == maximumEntries) return Result::Limit;
                    names.push_back(std::move(name));
                }
                if (!FindNextFileW(find.value,&info)) {
                    if (GetLastError() != ERROR_NO_MORE_FILES) return Error();
                    break;
                }
            }
        }
        std::sort(names.begin(),names.end()); output.swap(names); return Result::Ok;
    } catch (...) { return Result::Io; }
}

InventorySnapshot::InventorySnapshot() noexcept = default;
InventorySnapshot::~InventorySnapshot() = default;
InventorySnapshot::InventorySnapshot(InventorySnapshot&&) noexcept = default;
InventorySnapshot& InventorySnapshot::operator=(InventorySnapshot&&) noexcept = default;
bool InventorySnapshot::IsValid() const noexcept {return bool(state_);}
const DirectoryIdentity& InventorySnapshot::RootIdentity() const noexcept {
    static const DirectoryIdentity empty;return state_?state_->identity:empty;
}
const std::vector<InventoryEntry>& InventorySnapshot::Entries() const noexcept {
    static const std::vector<InventoryEntry> empty;return state_?state_->entries:empty;
}
namespace {
bool InventoryLimitsValid(const InventoryLimits& limits) noexcept {
    return limits.maximumEntries&&limits.maximumEntries<=65536&&limits.maximumDepth<=64&&
        limits.maximumNameCodeUnits<=16u*1024*1024&&limits.maximumDirectoryGuards&&
        limits.maximumDirectoryGuards<=4096&&ObservationLimitsValid(limits.observation);
}
bool SameObservation(const FileObservation& a,const FileObservation& b) noexcept {
    return a.identity.volume==b.identity.volume&&a.identity.fileIndex==b.identity.fileIndex&&
        a.identity.fileBytes==b.identity.fileBytes&&a.allocationBytes==b.allocationBytes&&
        a.streamLogicalBytes==b.streamLogicalBytes&&a.streamAllocationBytes==b.streamAllocationBytes&&
        a.attributes==b.attributes&&a.links==b.links&&a.streamCount==b.streamCount&&a.kind==b.kind&&
        a.deletePending==b.deletePending&&a.namedStreamsSupported==b.namedStreamsSupported;
}
// Names come from the pinned extended-volume namespace, not DOS parsing. Keep
// otherwise valid foreign names (including trailing dots/spaces and device-like
// stems) while rejecting separators/streams/traversal as child components.
bool InventoryNameValid(const std::wstring& name) noexcept {
    return !name.empty()&&name!=L"."&&name!=L".."&&name.find_first_of(L"\\/:")==std::wstring::npos;
}
Result InventoryNames(const std::wstring& path,std::size_t parentUnits,const InventoryLimits& limits,
    const std::atomic_bool* cancelled,std::vector<std::wstring>& output) {
    if(Cancelled(cancelled))return Result::Cancelled;
    struct Find { HANDLE value=INVALID_HANDLE_VALUE;~Find(){if(value!=INVALID_HANDLE_VALUE)FindClose(value);} } find;
    const auto pattern=path+L"\\*";WIN32_FIND_DATAW data{};
    find.value=FindFirstFileW(pattern.c_str(),&data);
    if(find.value==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND)return Error(error);
        output.clear();return Result::Ok;
    }
    std::vector<std::wstring> names;std::size_t units=0;
    for(;;) {
        if(Cancelled(cancelled))return Result::Cancelled;
        std::wstring name(data.cFileName);
        if(name!=L"."&&name!=L"..") {
            if(!InventoryNameValid(name))return Result::Invalid;
            if(names.size()==limits.maximumEntries)return Result::Limit;
            const auto prefix=parentUnits?parentUnits+1:0;
            if(prefix>limits.maximumNameCodeUnits||name.size()>limits.maximumNameCodeUnits-prefix)return Result::Limit;
            const auto length=prefix+name.size();
            if(length>limits.maximumNameCodeUnits-units)return Result::Limit;
            units+=length;names.push_back(std::move(name));
        }
        if(!FindNextFileW(find.value,&data)) {
            const auto error=GetLastError();if(error!=ERROR_NO_MORE_FILES)return Error(error);break;
        }
    }
    std::sort(names.begin(),names.end());
    if(std::adjacent_find(names.begin(),names.end())!=names.end())return Result::Conflict;
    output.swap(names);return Result::Ok;
}
struct InventoryWork {std::size_t entry=0;HANDLE directory=INVALID_HANDLE_VALUE;std::vector<std::wstring> names;};
}
Result Directory::Inventory(const InventoryLimits& limits,InventorySnapshot& output,
    const std::atomic_bool* cancelled) const noexcept {
    try {
        if(!state_||state_->guards.empty()||!state_->guards.back().Valid()||!InventoryLimitsValid(limits))return Result::Invalid;
        if(Cancelled(cancelled))return Result::Cancelled;
        auto candidate=std::make_unique<InventoryState>();candidate->root=state_;
        InventoryEntry root;
        auto result=ObserveHandle(state_->guards.back().value,limits.observation,root.observed);
        if(result!=Result::Ok)return result;
        if(root.observed.kind!=ObservationKind::Directory)return Result::Invalid;
        candidate->identity={root.observed.identity.volume,root.observed.identity.fileIndex};
        candidate->entries.push_back(std::move(root));
        std::vector<InventoryWork> work;work.push_back({0,state_->guards.back().value,{}});
        std::size_t nameUnits=0;
        for(std::size_t current=0;current<work.size();++current) {
            if(Cancelled(cancelled))return Result::Cancelled;
            const auto parentEntry=work[current].entry;
            const auto relative=candidate->entries[parentEntry].relativePath;
            const auto depth=candidate->entries[parentEntry].depth;
            const auto path=relative.empty()?state_->path:state_->path+L'\\'+relative;
            std::vector<std::wstring> names;
            result=InventoryNames(path,relative.size(),limits,cancelled,names);if(result!=Result::Ok)return result;
            for(const auto& name:names) {
                if(Cancelled(cancelled))return Result::Cancelled;
                if(candidate->entries.size()==limits.maximumEntries||depth==limits.maximumDepth)return Result::Limit;
                const auto prefix=relative.empty()?0:relative.size()+1;
                if(prefix>limits.maximumNameCodeUnits||name.size()>limits.maximumNameCodeUnits-prefix)return Result::Limit;
                const auto length=prefix+name.size();
                if(length>limits.maximumNameCodeUnits-nameUnits)return Result::Limit;
                InventoryEntry entry;entry.relativePath=relative.empty()?name:relative+L'\\'+name;entry.depth=depth+1;
                const auto child=state_->path+L'\\'+entry.relativePath;
                // Metadata access deliberately does not request data read access
                // and does not pin immutable bytes. It coexists with own active
                // exclusive writers and byte-locked control/catalog files.
                Handle metadata(CreateFileW(child.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
                if(!metadata.Valid()) {result=Error();return result==Result::Missing?Result::Conflict:result;}
                result=ObserveHandle(metadata.value,limits.observation,entry.observed);if(result!=Result::Ok)return result;
                if(entry.observed.kind==ObservationKind::Directory) {
                    if(candidate->guards.size()+1>=limits.maximumDirectoryGuards)return Result::Limit;
                    Handle guard;result=Guard(child,guard);if(result!=Result::Ok)return result==Result::Missing?Result::Conflict:result;
                    FileObservation pinned;result=ObserveHandle(guard.value,limits.observation,pinned);if(result!=Result::Ok)return result;
                    if(!SameObservation(entry.observed,pinned))return Result::Conflict;
                    // Reparse traversal is rejected above; also reject a repeated
                    // directory identity instead of walking a cycle/alias.
                    for(const auto& item:candidate->entries)if(item.observed.kind==ObservationKind::Directory&&
                        item.observed.identity.volume==pinned.identity.volume&&item.observed.identity.fileIndex==pinned.identity.fileIndex)return Result::Conflict;
                    const auto handle=guard.value;candidate->guards.push_back(std::move(guard));
                    work.push_back({candidate->entries.size(),handle,{}});
                }
                nameUnits+=length;candidate->entries.push_back(std::move(entry));
            }
            work[current].names=std::move(names);
        }
        // Revisit every directory after the entire walk, so additions while a
        // descendant was sampled do not silently disappear from this result.
        for(const auto& item:work) {
            const auto& entry=candidate->entries[item.entry];
            const auto path=entry.relativePath.empty()?state_->path:state_->path+L'\\'+entry.relativePath;
            std::vector<std::wstring> again;
            result=InventoryNames(path,entry.relativePath.size(),limits,cancelled,again);if(result!=Result::Ok)return result;
            if(again!=item.names)return Result::Conflict;
            FileObservation current;result=ObserveHandle(item.directory,limits.observation,current);if(result!=Result::Ok)return result;
            if(!SameObservation(entry.observed,current))return Result::Conflict;
        }
        if(Cancelled(cancelled))return Result::Cancelled;
        std::sort(candidate->entries.begin(),candidate->entries.end(),[](const auto&a,const auto&b){return a.relativePath<b.relativePath;});
        output.state_.swap(candidate);return Result::Ok;
    }catch(...) {return Result::Io;}
}
}
