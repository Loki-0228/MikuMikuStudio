#pragma once
#include "mikumikustudio/cache_catalog.hpp"

namespace mikumikustudio::cache_catalog::codec {
inline constexpr std::size_t RecordBytes = 368;
inline constexpr cache_files::Owner NamespaceOwner{
    0x4d,0x4d,0x53,0x43,0x61,0x74,0x61,0x6c,0x6f,0x67,0x48,0x65,0x61,0x64,0x30,0x31};
inline constexpr cache_files::Digest SnapshotKey{1};
inline constexpr cache_files::Digest CommitKey{2};
inline constexpr cache_files::Digest CheckpointKey{3};
inline constexpr std::size_t CheckpointBaseBytes = 376;
inline constexpr std::size_t MaximumRetired = 192;
inline constexpr std::size_t MaximumCheckpointBytes = CheckpointBaseBytes + MaximumRetired * 160;
static_assert(MaximumCheckpointBytes + cache_files::HeaderBytes <= MaintenanceReserveBytes);
struct Record {
    std::uint64_t revision = 0;
    cache_files::Owner owner{};
    cache_files::BlobInfo snapshot;
    cache_files::BlobInfo previous;
};
struct CheckpointRecord {
    Record state;
    // Canonical order: heads (commit/checkpoint) by name, then snapshots by name.
    // One entry per directory/name. Includes state.previous exactly; excludes
    // the retained state.snapshot. All entries carry full immutable identities.
    std::vector<cache_files::BlobInfo> retired;
};
enum class Result { Ok, Invalid, Io };
// Canonical little endian, fixed 368 bytes, no raw C++ layouts. Failures retain
// output. Decode rejects all reserved bytes, trailing data and invalid refs.
Result Encode(const Record& record, std::vector<std::uint8_t>& output) noexcept;
Result Decode(const void* bytes, std::size_t size, Record& output) noexcept;
// MMSCacheBase0001: common record fields through byte 367, count u32 at 368,
// reserved zero u32 at 372, then count 160-byte references. Revision >= 2,
// 1..MaximumRetired entries, exact size; previous may name a checkpoint.
Result EncodeCheckpoint(const CheckpointRecord& record, std::vector<std::uint8_t>& output) noexcept;
Result DecodeCheckpoint(const void* bytes, std::size_t size, CheckpointRecord& output) noexcept;
}
