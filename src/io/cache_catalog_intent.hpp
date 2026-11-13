#pragma once
#include "cache_catalog_codec.hpp"
#include "mikumikustudio/cache_approval.hpp"

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
inline constexpr cache_files::Digest ApprovedKey{6};
inline constexpr cache_files::Digest ApprovedRollbackKey{7};
inline constexpr std::size_t ApprovedRecordBytes = 1008;
inline constexpr cache_files::Digest ApprovedCheckpointKey{8};
inline constexpr cache_files::Digest ApprovedCheckpointRollbackKey{9};
inline constexpr std::size_t ApprovedCheckpointHeaderBytes = 440;
inline constexpr std::size_t MaximumApprovedCheckpointBytes = ApprovedCheckpointHeaderBytes + codec::MaximumCheckpointBytes;
inline constexpr std::size_t MaximumOperationBytes = MaximumApprovedCheckpointBytes;
constexpr std::size_t ApprovedCheckpointRecordSize(std::size_t retiredCount) noexcept {
    return retiredCount && retiredCount <= codec::MaximumRetired ?
        ApprovedCheckpointHeaderBytes + codec::CheckpointBaseBytes + retiredCount * 160 : 0;
}
struct ApprovedCheckpointRecord {
    cache_files::Owner operation{};
    codec::CheckpointRecord publication;
    cache_files::WritePlan checkpoint;
    PublicationApproval approval;
};
// Fixed header: MMSCacheCkAp0001 (16 ASCII bytes), version u32
// at 16, zero at 20:4, operation at 24:16, zero at 40:8, plan at 48:168,
// approval at 216:224, then the exact canonical checkpoint payload at 440.
// Approval has the same field order as the original approved record. Failures
// preserve output. A decoded record is a fact, never deletion/quota authority.
Result ValidateCheckpointApproval(std::size_t retiredCount, const PublicationApproval&) noexcept;
Result ValidateCheckpointApproval(const ApprovedCheckpointRecord&) noexcept;
Result EncodeApprovedCheckpoint(const ApprovedCheckpointRecord&, std::vector<std::uint8_t>& output) noexcept;
Result DecodeApprovedCheckpoint(const void* bytes, std::size_t size, ApprovedCheckpointRecord& output) noexcept;
struct ApprovedRecord {
    Record operation;
    PublicationApproval approval;
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

// Checks the original operation plus all approval identities and arithmetic.
// Peak = physical + legacy + afterRaw + afterMetadata + the separately rounded
// completed snapshot, completed commit, and two (HeaderBytes + 1008) records.
// Baseline = physical + legacy + beforeRaw + beforeMetadata. Peak must equal
// approvedPeak and fit baseline, or fit an enabled policy limit. Overflow fails.
// This validates facts, not the caller's authority or current filesystem state.
Result ValidateApproval(const Record&, const PublicationApproval&) noexcept;
// Pre-creation form: no hashing, allocation or filesystem access. Checks the
// nine directory identities and arithmetic before actual candidate IDs exist.
// The record overload additionally validates original intent relationships and
// verifies both candidate identities against all nine directories.
Result ValidateApproval(std::size_t snapshotPayloadBytes, const PublicationApproval&) noexcept;
// Fixed little-endian approved envelope, independent of the unchanged old wire:
// 0:16 ASCII MMSCacheApprv001; 16:u32 version=1; 20:12 zero;
// 32:752 original Encode bytes; 784:9 directory records in ApprovalArea order,
// each volume:u32, zero:4, fileIndex:u64; 928:9 u64 values in declaration order;
// 1000:u32 policyEnabled (0 or 1); 1004:4 zero. All failures preserve output.
Result EncodeApproved(const ApprovedRecord&, std::vector<std::uint8_t>& output) noexcept;
Result DecodeApproved(const void* bytes, std::size_t size, ApprovedRecord& output) noexcept;
}
