#include "cache_catalog_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Canonical encodings for the durable catalog records. Both layouts are fixed
// field maps rather than memory dumps: every field is written little endian
// byte by byte and every reserved gap is checked to be zero.
namespace mikumikustudio::cache_catalog::codec {
namespace {

using cache_files::BlobInfo;
using cache_files::Digest;
using cache_files::Kind;
using cache_files::Owner;

inline constexpr std::size_t ReferenceBytes = 160;
inline constexpr std::size_t NameBytes = 37;
inline constexpr std::size_t HexBytes = 32;
inline constexpr std::size_t MagicBytes = 16;
inline constexpr std::size_t VersionOffset = 16;
inline constexpr std::size_t ReservedOffset = 20;
inline constexpr std::size_t RevisionOffset = 24;
inline constexpr std::size_t OwnerOffset = 32;
inline constexpr std::size_t SnapshotOffset = 48;
inline constexpr std::size_t PreviousOffset = 208;
inline constexpr std::size_t CountOffset = 368;
inline constexpr std::size_t CountReservedOffset = 372;
inline constexpr std::uint32_t Version = 1;

constexpr std::array<std::uint8_t, MagicBytes> Magic{
    'M', 'M', 'S', 'C', 'a', 'c', 'h', 'e', 'H', 'e', 'a', 'd', '0', '0', '0', '1'};
constexpr std::array<std::uint8_t, MagicBytes> CheckpointMagic{
    'M', 'M', 'S', 'C', 'a', 'c', 'h', 'e', 'B', 'a', 's', 'e', '0', '0', '0', '1'};
constexpr std::array<char, 5> NameSuffix{'.', 'b', 'l', 'o', 'b'};

void PutU32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

void PutU64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

std::uint32_t GetU32(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    }
    return value;
}

std::uint64_t GetU64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

bool AllZero(const std::uint8_t* bytes, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

template <typename Bytes>
bool AllZero(const Bytes& bytes) noexcept {
    for (const std::uint8_t byte : bytes) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

bool IsLowerHex(std::uint8_t value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

// Encodes the only accepted name shape: 32 lowercase hex digits plus ".blob".
bool PutName(const std::wstring& name, std::uint8_t* bytes) noexcept {
    if (name.size() != NameBytes) {
        return false;
    }
    for (std::size_t i = 0; i < HexBytes; ++i) {
        const wchar_t value = name[i];
        if (value > 0x7f || !IsLowerHex(static_cast<std::uint8_t>(value))) {
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>(value);
    }
    for (std::size_t i = 0; i < NameSuffix.size(); ++i) {
        if (name[HexBytes + i] != static_cast<wchar_t>(NameSuffix[i])) {
            return false;
        }
        bytes[HexBytes + i] = static_cast<std::uint8_t>(NameSuffix[i]);
    }
    return true;
}

// Validates then converts a stored name. Allocation happens here, inside the
// caller's try block, never while publishing into the caller's record.
bool GetName(const std::uint8_t* bytes, std::wstring& name) {
    for (std::size_t i = 0; i < HexBytes; ++i) {
        if (!IsLowerHex(bytes[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < NameSuffix.size(); ++i) {
        if (bytes[HexBytes + i] != static_cast<std::uint8_t>(NameSuffix[i])) {
            return false;
        }
    }
    std::wstring parsed(NameBytes, L'\0');
    for (std::size_t i = 0; i < NameBytes; ++i) {
        parsed[i] = static_cast<wchar_t>(bytes[i]);
    }
    name = std::move(parsed);
    return true;
}

// Writes one 160 byte reference. The caller zero-initialized the record first,
// so the reserved gaps at +37, +44 and +140 stay zero.
bool PutReference(std::uint8_t* reference, const BlobInfo& info, Kind kind,
    const Owner& owner, const Digest& key) noexcept {
    if (!PutName(info.name, reference)) {
        return false;
    }
    PutU32(reference + 40, static_cast<std::uint32_t>(kind));
    std::copy(owner.begin(), owner.end(), reference + 48);
    std::copy(key.begin(), key.end(), reference + 64);
    std::copy(info.payloadDigest.begin(), info.payloadDigest.end(), reference + 96);
    PutU64(reference + 128, info.payloadBytes);
    PutU32(reference + 136, info.identity.volume);
    PutU64(reference + 144, info.identity.fileIndex);
    PutU64(reference + 152, info.identity.fileBytes);
    return true;
}

// Parses and fully checks one reference against the constraints shared by every
// record. Payload size rules depend on the role and stay with the caller.
bool GetReference(const std::uint8_t* reference, Kind kind, const Owner& owner,
    const Digest& key, BlobInfo& info) {
    if (!AllZero(reference + 37, 3) || !AllZero(reference + 44, 4) ||
        !AllZero(reference + 140, 4)) {
        return false;
    }
    if (GetU32(reference + 40) != static_cast<std::uint32_t>(kind)) {
        return false;
    }
    Owner storedOwner{};
    std::copy(reference + 48, reference + 64, storedOwner.begin());
    if (storedOwner != owner) {
        return false;
    }
    Digest storedKey{};
    std::copy(reference + 64, reference + 96, storedKey.begin());
    if (storedKey != key) {
        return false;
    }
    std::wstring name;
    if (!GetName(reference, name)) {
        return false;
    }
    BlobInfo parsed;
    parsed.name = std::move(name);
    parsed.spec.owner = storedOwner;
    parsed.spec.kind = kind;
    parsed.spec.key = storedKey;
    std::copy(reference + 96, reference + 128, parsed.payloadDigest.begin());
    parsed.payloadBytes = GetU64(reference + 128);
    parsed.identity.volume = GetU32(reference + 136);
    parsed.identity.fileIndex = GetU64(reference + 144);
    parsed.identity.fileBytes = GetU64(reference + 152);
    info = std::move(parsed);
    return true;
}

// Role checks shared by encode validation and decode parsing. They cover kind,
// owner, key, payload size and the exact file size implied by the payload.
bool IsSnapshotInfo(const BlobInfo& info, const Owner& owner) noexcept {
    return info.spec.kind == Kind::CatalogSnapshot && info.spec.owner == owner &&
        info.spec.key == SnapshotKey &&
        info.payloadBytes <= static_cast<std::uint64_t>(MaximumSnapshotBytes) &&
        info.identity.fileBytes == cache_files::HeaderBytes + info.payloadBytes;
}

bool IsCommitInfo(const BlobInfo& info) noexcept {
    return info.spec.kind == Kind::CatalogCommit && info.spec.owner == NamespaceOwner &&
        info.spec.key == CommitKey &&
        info.payloadBytes == static_cast<std::uint64_t>(RecordBytes) &&
        info.identity.fileBytes ==
            cache_files::HeaderBytes + static_cast<std::uint64_t>(RecordBytes);
}

// A checkpoint payload is 376 + 160 * n bytes with 1 <= n <= MaximumRetired.
bool IsCheckpointInfo(const BlobInfo& info) noexcept {
    if (info.spec.kind != Kind::CatalogCheckpoint || info.spec.owner != NamespaceOwner ||
        info.spec.key != CheckpointKey) {
        return false;
    }
    if (info.payloadBytes < static_cast<std::uint64_t>(CheckpointBaseBytes + ReferenceBytes) ||
        info.payloadBytes > static_cast<std::uint64_t>(MaximumCheckpointBytes) ||
        (info.payloadBytes - static_cast<std::uint64_t>(CheckpointBaseBytes)) %
                static_cast<std::uint64_t>(ReferenceBytes) != 0) {
        return false;
    }
    return info.identity.fileBytes == cache_files::HeaderBytes + info.payloadBytes;
}

bool IsHeadInfo(const BlobInfo& info) noexcept {
    return IsCommitInfo(info) || IsCheckpointInfo(info);
}

bool IsRetiredInfo(const BlobInfo& info, const Owner& owner) noexcept {
    switch (info.spec.kind) {
    case Kind::CatalogSnapshot:
        return IsSnapshotInfo(info, owner);
    case Kind::CatalogCommit:
        return IsCommitInfo(info);
    case Kind::CatalogCheckpoint:
        return IsCheckpointInfo(info);
    default:
        return false;
    }
}

// Full immutable identity, not just the directory/name: the retirement list must
// carry the previous head byte for byte.
bool EqualInfo(const BlobInfo& left, const BlobInfo& right) noexcept {
    return left.name == right.name && left.spec.owner == right.spec.owner &&
        left.spec.kind == right.spec.kind && left.spec.key == right.spec.key &&
        left.payloadDigest == right.payloadDigest && left.payloadBytes == right.payloadBytes &&
        left.identity.volume == right.identity.volume &&
        left.identity.fileIndex == right.identity.fileIndex &&
        left.identity.fileBytes == right.identity.fileBytes;
}

bool PutHeadReference(std::uint8_t* reference, const BlobInfo& info) noexcept {
    if (info.spec.kind == Kind::CatalogCommit) {
        return PutReference(reference, info, Kind::CatalogCommit, NamespaceOwner, CommitKey);
    }
    if (info.spec.kind == Kind::CatalogCheckpoint) {
        return PutReference(reference, info, Kind::CatalogCheckpoint, NamespaceOwner,
            CheckpointKey);
    }
    return false;
}

bool PutRetiredReference(std::uint8_t* reference, const BlobInfo& info,
    const Owner& owner) noexcept {
    switch (info.spec.kind) {
    case Kind::CatalogSnapshot:
        return PutReference(reference, info, Kind::CatalogSnapshot, owner, SnapshotKey);
    case Kind::CatalogCommit:
        return PutReference(reference, info, Kind::CatalogCommit, NamespaceOwner, CommitKey);
    case Kind::CatalogCheckpoint:
        return PutReference(reference, info, Kind::CatalogCheckpoint, NamespaceOwner,
            CheckpointKey);
    default:
        return false;
    }
}

bool GetHeadReference(const std::uint8_t* reference, BlobInfo& info) {
    const std::uint32_t kind = GetU32(reference + 40);
    if (kind == static_cast<std::uint32_t>(Kind::CatalogCommit)) {
        return GetReference(reference, Kind::CatalogCommit, NamespaceOwner, CommitKey, info) &&
            IsCommitInfo(info);
    }
    if (kind == static_cast<std::uint32_t>(Kind::CatalogCheckpoint)) {
        return GetReference(reference, Kind::CatalogCheckpoint, NamespaceOwner, CheckpointKey,
                   info) &&
            IsCheckpointInfo(info);
    }
    return false;
}

bool GetRetiredReference(const std::uint8_t* reference, const Owner& owner, BlobInfo& info) {
    const std::uint32_t kind = GetU32(reference + 40);
    if (kind == static_cast<std::uint32_t>(Kind::CatalogSnapshot)) {
        return GetReference(reference, Kind::CatalogSnapshot, owner, SnapshotKey, info) &&
            IsSnapshotInfo(info, owner);
    }
    if (kind == static_cast<std::uint32_t>(Kind::CatalogCommit)) {
        return GetReference(reference, Kind::CatalogCommit, NamespaceOwner, CommitKey, info) &&
            IsCommitInfo(info);
    }
    if (kind == static_cast<std::uint32_t>(Kind::CatalogCheckpoint)) {
        return GetReference(reference, Kind::CatalogCheckpoint, NamespaceOwner, CheckpointKey,
                   info) &&
            IsCheckpointInfo(info);
    }
    return false;
}

void SwapBlobInfo(BlobInfo& left, BlobInfo& right) noexcept {
    using std::swap;
    swap(left.name, right.name);
    swap(left.spec.owner, right.spec.owner);
    swap(left.spec.kind, right.spec.kind);
    swap(left.spec.key, right.spec.key);
    swap(left.payloadDigest, right.payloadDigest);
    swap(left.payloadBytes, right.payloadBytes);
    swap(left.identity.volume, right.identity.volume);
    swap(left.identity.fileIndex, right.identity.fileIndex);
    swap(left.identity.fileBytes, right.identity.fileBytes);
}

// Field swaps only: no allocation and no throwing copy on the success path.
void Publish(Record& source, Record& output) noexcept {
    using std::swap;
    swap(output.revision, source.revision);
    swap(output.owner, source.owner);
    SwapBlobInfo(output.snapshot, source.snapshot);
    SwapBlobInfo(output.previous, source.previous);
}

void PublishCheckpoint(CheckpointRecord& source, CheckpointRecord& output) noexcept {
    Publish(source.state, output.state);
    output.retired.swap(source.retired);
}

bool IsDefaultPrevious(const BlobInfo& previous) noexcept {
    return previous.name.empty() && previous.spec.kind == Kind::Effect &&
        AllZero(previous.spec.owner) && AllZero(previous.spec.key) &&
        AllZero(previous.payloadDigest) && previous.payloadBytes == 0 &&
        previous.identity.volume == 0 && previous.identity.fileIndex == 0 &&
        previous.identity.fileBytes == 0;
}

bool IsValidState(const Record& state) noexcept {
    if (state.revision == 0 || AllZero(state.owner) ||
        !IsSnapshotInfo(state.snapshot, state.owner)) {
        return false;
    }
    return state.revision == 1 ? IsDefaultPrevious(state.previous) : IsHeadInfo(state.previous);
}

// Enforces the canonical retirement list shared by encode and decode: one
// entry per directory/name, heads (commit and checkpoint share a directory)
// sorted by name first, then snapshots sorted by name, the previous head present
// exactly, and the retained snapshot never listed. Retired head revisions stay a
// Store concern; only reference shape is checked here.
bool ValidateRetired(const Record& state, const std::vector<BlobInfo>& retired) noexcept {
    const std::size_t count = retired.size();
    if (count == 0 || count > MaximumRetired) {
        return false;
    }
    bool snapshots = false;
    const std::wstring* previousName = nullptr;
    bool hasPrevious = false;
    for (const BlobInfo& info : retired) {
        if (!IsRetiredInfo(info, state.owner)) {
            return false;
        }
        if (info.spec.kind == Kind::CatalogSnapshot) {
            if (!snapshots) {
                snapshots = true;
                previousName = nullptr;
            }
            // The live snapshot stays referenced by state; it is never retired.
            if (info.name == state.snapshot.name) {
                return false;
            }
        } else if (snapshots) {
            return false;
        }
        if (previousName != nullptr && !(*previousName < info.name)) {
            return false;
        }
        previousName = &info.name;
        if (EqualInfo(info, state.previous)) {
            hasPrevious = true;
        }
    }
    return hasPrevious;
}

// Parses the first 368 bytes shared by both record kinds. The caller has already
// checked magic, version and the reserved word at +20.
bool ParseState(const std::uint8_t* data, Record& state) {
    const std::uint64_t revision = GetU64(data + RevisionOffset);
    Owner owner{};
    std::copy(data + OwnerOffset, data + OwnerOffset + owner.size(), owner.begin());
    if (revision == 0 || AllZero(owner)) {
        return false;
    }
    BlobInfo snapshot;
    if (!GetReference(data + SnapshotOffset, Kind::CatalogSnapshot, owner, SnapshotKey,
            snapshot) ||
        !IsSnapshotInfo(snapshot, owner)) {
        return false;
    }
    BlobInfo previous;
    if (revision == 1) {
        // The initial record carries no previous commit; all 160 bytes are zero.
        if (!AllZero(data + PreviousOffset, ReferenceBytes)) {
            return false;
        }
    } else if (!GetHeadReference(data + PreviousOffset, previous)) {
        return false;
    }
    state.revision = revision;
    state.owner = owner;
    state.snapshot = std::move(snapshot);
    state.previous = std::move(previous);
    return true;
}

} // namespace

Result Encode(const Record& record, std::vector<std::uint8_t>& output) noexcept {
    if (!IsValidState(record)) {
        return Result::Invalid;
    }
    const bool initial = record.revision == 1;
    try {
        // Build locally so a rejected or failing encode leaves output untouched.
        std::vector<std::uint8_t> bytes(RecordBytes, 0);
        std::copy(Magic.begin(), Magic.end(), bytes.begin());
        PutU32(bytes.data() + VersionOffset, Version);
        PutU64(bytes.data() + RevisionOffset, record.revision);
        std::copy(record.owner.begin(), record.owner.end(), bytes.begin() + OwnerOffset);
        if (!PutReference(bytes.data() + SnapshotOffset, record.snapshot,
                Kind::CatalogSnapshot, record.owner, SnapshotKey)) {
            return Result::Invalid;
        }
        if (!initial && !PutHeadReference(bytes.data() + PreviousOffset, record.previous)) {
            return Result::Invalid;
        }
        output.swap(bytes);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

Result Decode(const void* bytes, std::size_t size, Record& output) noexcept {
    if (bytes == nullptr || size != RecordBytes) {
        return Result::Invalid;
    }
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    if (!std::equal(Magic.begin(), Magic.end(), data) ||
        GetU32(data + VersionOffset) != Version || GetU32(data + ReservedOffset) != 0) {
        return Result::Invalid;
    }
    try {
        Record parsed;
        if (!ParseState(data, parsed)) {
            return Result::Invalid;
        }
        Publish(parsed, output);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

Result EncodeCheckpoint(const CheckpointRecord& record,
    std::vector<std::uint8_t>& output) noexcept {
    const Record& state = record.state;
    if (state.revision < 2 || !IsValidState(state) ||
        !ValidateRetired(state, record.retired)) {
        return Result::Invalid;
    }
    try {
        const std::size_t count = record.retired.size();
        // Build locally so a rejected or failing encode leaves output untouched.
        std::vector<std::uint8_t> bytes(CheckpointBaseBytes + ReferenceBytes * count, 0);
        std::copy(CheckpointMagic.begin(), CheckpointMagic.end(), bytes.begin());
        PutU32(bytes.data() + VersionOffset, Version);
        PutU64(bytes.data() + RevisionOffset, state.revision);
        std::copy(state.owner.begin(), state.owner.end(), bytes.begin() + OwnerOffset);
        if (!PutReference(bytes.data() + SnapshotOffset, state.snapshot,
                Kind::CatalogSnapshot, state.owner, SnapshotKey) ||
            !PutHeadReference(bytes.data() + PreviousOffset, state.previous)) {
            return Result::Invalid;
        }
        PutU32(bytes.data() + CountOffset, static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            if (!PutRetiredReference(bytes.data() + CheckpointBaseBytes + ReferenceBytes * i,
                    record.retired[i], state.owner)) {
                return Result::Invalid;
            }
        }
        output.swap(bytes);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

Result DecodeCheckpoint(const void* bytes, std::size_t size,
    CheckpointRecord& output) noexcept {
    if (bytes == nullptr || size < CheckpointBaseBytes) {
        return Result::Invalid;
    }
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    if (!std::equal(CheckpointMagic.begin(), CheckpointMagic.end(), data) ||
        GetU32(data + VersionOffset) != Version || GetU32(data + ReservedOffset) != 0) {
        return Result::Invalid;
    }
    const std::uint32_t count = GetU32(data + CountOffset);
    if (count == 0 || static_cast<std::size_t>(count) > MaximumRetired ||
        GetU32(data + CountReservedOffset) != 0) {
        return Result::Invalid;
    }
    if (size != CheckpointBaseBytes + ReferenceBytes * static_cast<std::size_t>(count)) {
        return Result::Invalid;
    }
    try {
        CheckpointRecord parsed;
        if (!ParseState(data, parsed.state) || parsed.state.revision < 2) {
            return Result::Invalid;
        }
        parsed.retired.resize(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < parsed.retired.size(); ++i) {
            if (!GetRetiredReference(data + CheckpointBaseBytes + ReferenceBytes * i,
                    parsed.state.owner, parsed.retired[i])) {
                return Result::Invalid;
            }
        }
        if (!ValidateRetired(parsed.state, parsed.retired)) {
            return Result::Invalid;
        }
        PublishCheckpoint(parsed, output);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

} // namespace mikumikustudio::cache_catalog::codec
