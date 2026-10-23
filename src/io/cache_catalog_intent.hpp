#pragma once
#include "cache_catalog_codec.hpp"

namespace mikumikustudio::cache_catalog::intent {
using Result = codec::Result;
inline constexpr cache_files::Digest Key{4};
inline constexpr std::size_t RecordBytes = 752;
inline constexpr std::size_t MaximumBytes = RecordBytes;
struct Record {
    cache_files::Owner operation{};
    codec::Record publication;
    cache_files::WritePlan snapshot, commit;
};
// Predicts the complete envelope identity from a validated zero-length creation
// plan. This is NOT evidence of disk contents or authorization to read/delete.
// This helper checks one plan; Encode/Decode also check publication relationships.
Result PlannedInfo(const cache_files::WritePlan& plan, cache_files::BlobInfo& output) noexcept;
// Fixed little-endian layout; no native structure dumps. All failures retain output.
// 0:16 ASCII MMSCacheIntent01, 16:u32 version=1, 20:4 zero, 24:16 operation,
// 40:8 zero, 48:368 original codec publication, 416:168 snapshot, 584:168 commit.
// Plan offsets: 0:37 ASCII name, 37:3 zero, 40:16 owner, 56:u32 kind,
// 60:4 zero, 64:32 key, 96:32 digest, 128:u64 payload, 136:u32 volume,
// 140:4 zero, 144:u64 file index, 152:u64 created size=0, 160:u64 completed size.
// Both plans are same-volume distinct files. The snapshot matches publication;
// the commit has NamespaceOwner/CommitKey and SHA-256 of its 368 encoded bytes.
Result Encode(const Record& record, std::vector<std::uint8_t>& output) noexcept;
Result Decode(const void* bytes, std::size_t size, Record& output) noexcept;
}
