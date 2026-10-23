#include "cache_catalog_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Canonical encoding of one cache commit record. The layout is a fixed 368 bytes
// so records are durable history, not a memory dump: every field is written
// little endian byte by byte and padding from C++ layouts never reaches disk.
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
inline constexpr std::uint32_t Version = 1;

constexpr std::array<std::uint8_t, MagicBytes> Magic{
    'M', 'M', 'S', 'C', 'a', 'c', 'h', 'e', 'H', 'e', 'a', 'd', '0', '0', '0', '1'};
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

bool IsDefaultPrevious(const BlobInfo& previous) noexcept {
    return previous.name.empty() && previous.spec.kind == Kind::Effect &&
        AllZero(previous.spec.owner) && AllZero(previous.spec.key) &&
        AllZero(previous.payloadDigest) && previous.payloadBytes == 0 &&
        previous.identity.volume == 0 && previous.identity.fileIndex == 0 &&
        previous.identity.fileBytes == 0;
}

} // namespace

Result Encode(const Record& record, std::vector<std::uint8_t>& output) noexcept {
    if (record.revision == 0 || AllZero(record.owner)) {
        return Result::Invalid;
    }
    const BlobInfo& snapshot = record.snapshot;
    if (snapshot.spec.kind != Kind::CatalogSnapshot || snapshot.spec.owner != record.owner ||
        snapshot.spec.key != SnapshotKey ||
        snapshot.payloadBytes > static_cast<std::uint64_t>(MaximumSnapshotBytes) ||
        snapshot.identity.fileBytes != cache_files::HeaderBytes + snapshot.payloadBytes) {
        return Result::Invalid;
    }
    const bool initial = record.revision == 1;
    if (initial) {
        if (!IsDefaultPrevious(record.previous)) {
            return Result::Invalid;
        }
    } else {
        const BlobInfo& previous = record.previous;
        if (previous.spec.kind != Kind::CatalogCommit || previous.spec.owner != NamespaceOwner ||
            previous.spec.key != CommitKey ||
            previous.payloadBytes != static_cast<std::uint64_t>(RecordBytes) ||
            previous.identity.fileBytes !=
                cache_files::HeaderBytes + static_cast<std::uint64_t>(RecordBytes)) {
            return Result::Invalid;
        }
    }
    try {
        // Build locally so a rejected or failing encode leaves output untouched.
        std::vector<std::uint8_t> bytes(RecordBytes, 0);
        std::copy(Magic.begin(), Magic.end(), bytes.begin());
        PutU32(bytes.data() + 16, Version);
        PutU64(bytes.data() + 24, record.revision);
        std::copy(record.owner.begin(), record.owner.end(), bytes.begin() + 32);
        if (!PutReference(bytes.data() + 48, snapshot, Kind::CatalogSnapshot, record.owner,
                SnapshotKey)) {
            return Result::Invalid;
        }
        if (!initial &&
            !PutReference(bytes.data() + 208, record.previous, Kind::CatalogCommit,
                NamespaceOwner, CommitKey)) {
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
    if (!std::equal(Magic.begin(), Magic.end(), data) || GetU32(data + 16) != Version ||
        GetU32(data + 20) != 0) {
        return Result::Invalid;
    }
    const std::uint64_t revision = GetU64(data + 24);
    Owner owner{};
    std::copy(data + 32, data + 48, owner.begin());
    if (revision == 0 || AllZero(owner)) {
        return Result::Invalid;
    }
    try {
        Record parsed;
        parsed.revision = revision;
        parsed.owner = owner;
        if (!GetReference(data + 48, Kind::CatalogSnapshot, owner, SnapshotKey, parsed.snapshot) ||
            parsed.snapshot.payloadBytes > static_cast<std::uint64_t>(MaximumSnapshotBytes) ||
            parsed.snapshot.identity.fileBytes !=
                cache_files::HeaderBytes + parsed.snapshot.payloadBytes) {
            return Result::Invalid;
        }
        if (revision == 1) {
            // The initial record carries no previous commit; all 160 bytes are zero.
            if (!AllZero(data + 208, ReferenceBytes)) {
                return Result::Invalid;
            }
        } else {
            if (!GetReference(data + 208, Kind::CatalogCommit, NamespaceOwner, CommitKey,
                    parsed.previous) ||
                parsed.previous.payloadBytes != static_cast<std::uint64_t>(RecordBytes) ||
                parsed.previous.identity.fileBytes !=
                    cache_files::HeaderBytes + static_cast<std::uint64_t>(RecordBytes)) {
                return Result::Invalid;
            }
        }
        Publish(parsed, output);
        return Result::Ok;
    } catch (...) {
        return Result::Io;
    }
}

} // namespace mikumikustudio::cache_catalog::codec
