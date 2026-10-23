#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_files.hpp"
#include "cache_files_crypto.hpp"
#include <algorithm>
#include <cstring>
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
Result FileIdentity(HANDLE file, Identity& output) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return Result::Io;
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1) return Result::Corrupt;
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
bool Disposition(HANDLE file) noexcept {
    FILE_DISPOSITION_INFO info{}; info.DeleteFile = TRUE;
    return SetFileInformationByHandle(file,FileDispositionInfo,&info,sizeof(info)) != FALSE;
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
struct LockState {
    std::shared_ptr<DirectoryState> directory;
    Handle file;
    OVERLAPPED overlap{};
    bool held = false;
    ~LockState() { if (held) UnlockFileEx(file.value,0,1,0,&overlap); }
};

Directory::Directory() noexcept = default;
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
PreparedWrite::PreparedWrite() noexcept = default;
PreparedWrite::~PreparedWrite() = default;
PreparedWrite::PreparedWrite(PreparedWrite&&) noexcept = default;
PreparedWrite& PreparedWrite::operator=(PreparedWrite&&) noexcept = default;
bool PreparedWrite::IsValid() const noexcept { return state_ && state_->file.Valid(); }
const WritePlan& PreparedWrite::Plan() const noexcept { static const WritePlan empty; return state_ ? state_->plan : empty; }

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
            candidate->path += L'\\'; candidate->path += part;
            Handle directory;
            result = Guard(candidate->path,directory);
            if (result == Result::Missing && create) {
                if (!CreateDirectoryW(candidate->path.c_str(),nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Error();
                result = Guard(candidate->path,directory);
            }
            if (result != Result::Ok) return result;
            candidate->guards.push_back(std::move(directory));
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
    try {
        if (!state_ || waitMilliseconds > 2000) return Result::Invalid;
        auto candidate = std::make_unique<LockState>(); candidate->directory = state_;
        const auto path = state_->path + L"\\.lock";
        candidate->file = Handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
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
}
