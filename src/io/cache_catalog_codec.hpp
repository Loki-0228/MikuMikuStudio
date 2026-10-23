#pragma once
#include "mikumikustudio/cache_catalog.hpp"

namespace mikumikustudio::cache_catalog::codec {
inline constexpr std::size_t RecordBytes = 368;
inline constexpr cache_files::Owner NamespaceOwner{
    0x4d,0x4d,0x53,0x43,0x61,0x74,0x61,0x6c,0x6f,0x67,0x48,0x65,0x61,0x64,0x30,0x31};
inline constexpr cache_files::Digest SnapshotKey{1};
inline constexpr cache_files::Digest CommitKey{2};
struct Record {
    std::uint64_t revision = 0;
    cache_files::Owner owner{};
    cache_files::BlobInfo snapshot;
    cache_files::BlobInfo previous;
};
enum class Result { Ok, Invalid, Io };
// Canonical little endian, fixed 368 bytes, no raw C++ layouts. Failures retain
// output. Decode rejects all reserved bytes, trailing data and invalid refs.
Result Encode(const Record& record, std::vector<std::uint8_t>& output) noexcept;
Result Decode(const void* bytes, std::size_t size, Record& output) noexcept;
}
