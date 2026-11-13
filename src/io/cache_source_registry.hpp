#pragma once
#include "cache_business_codec.hpp"

// Internal encoding/validation only. No quota grant, file ownership proof,
// catalog publication, decoder activation or recovery permission is implied.
namespace mikumikustudio::cache_business {
using SourceId=std::array<std::uint8_t,16>;
using OperationId=std::array<std::uint8_t,16>;
enum class SourceArea:std::uint32_t {Video=1};
enum class SourceState:std::uint32_t {Published=1,Retired=2};
enum class SourceStage:std::uint32_t {
    Reserved=1,Prepared=2,Captured=3,AbortPrepared=4,AbortCaptured=5
};
struct DirectoryBinding {
    std::uint64_t id=0;
    SourceArea area=SourceArea::Video;
    cache_files::DirectoryIdentity identity{};
};
struct SourceRecord {
    SourceId id{};
    OperationId createdBy{};
    std::uint64_t directoryId=0;
    cache_files::RawSourceInfo file;
    SourceState state=SourceState::Published;
    std::uint64_t lastUse=0;
    std::vector<ProjectId> owners;
};
struct SourceOperation {
    OperationId id{};
    SourceId sourceId{};
    std::uint64_t directoryId=0;
    SourceStage stage=SourceStage::Reserved;
    std::uint64_t reservedBytes=0;
    cache_files::RawSourcePlan plan;
    cache_files::RawSourceInfo captured;
};
struct SourceRegistry {
    // Persisted maintenance-credit protocol, not a quota grant. Version 1
    // describes single-file checkpoints; version 2 includes approval records.
    // The admission coordinator authorizes upgrades and rejects downgrades.
    std::uint32_t maintenanceProtocol=1;
    cache_files::Owner storeOwner{};
    std::uint64_t maintenanceEpoch=0;
    std::vector<DirectoryBinding> directories;
    std::vector<SourceRecord> sources;
    std::vector<SourceOperation> operations;
};
struct BusinessRoot {Manifest legacy;SourceRegistry sources;};

inline constexpr std::size_t MaximumSourceRegistryBytes=8*1024*1024;
inline constexpr std::size_t MaximumBusinessRootBytes=16*1024*1024;
inline constexpr std::size_t MaximumSourceDirectories=16,MaximumSources=8192,MaximumSourceOperations=256;
// Owners use the existing MaximumOwnersPerEntry limit and total byte bound.
// Canonical little-endian, no implicit padding; every reserved byte is zero.
namespace source_wire {
inline constexpr char RegistryMagic[16]={'M','M','S','C','a','c','h','e','S','o','u','r','c','e','0','1'};
inline constexpr char RootMagic[16]={'M','M','S','C','a','c','h','e','B','u','s','R','o','o','t','1'};
inline constexpr std::uint32_t Version=1;
inline constexpr std::size_t RegistryHeaderBytes=80,RootHeaderBytes=80;
inline constexpr std::size_t DirectoryBytes=32,SourceFixedBytes=144,OperationFixedBytes=176;
// Registry header: magic@0, maintenanceProtocol:u32@16 (1 or 2), headerBytes:u32@20,
// owner:16@24, epoch:u64@40, directoryCount:u32@48, sourceCount:u32@52,
// operationCount:u32@56, zero:4@60, totalBytes:u64@64, zero:8@72.
// Then directories, sources, operations; each group is strictly ID ordered.
// Directory: recordBytes:u32@0=32, area:u32@4, id:u64@8,
// volume:u32@16, zero:4@20, fileIndex:u64@24.
// Source: recordBytes:u32@0, state:u32@4, id:16@8, createdBy:16@24,
// directoryId:u64@40, lastUse:u64@48, ownerCount:u32@56, nameBytes:u32@60,
// volume:u32@64, zero:4@68, fileIndex:u64@72, fileBytes:u64@80,
// payloadBytes:u64@88, digest:32@96, zero:16@128;
// followed by nameBytes ASCII bytes (no NUL), then ownerCount u64 IDs.
// Operation: recordBytes:u32@0, stage:u32@4, id:16@8, sourceId:16@24,
// directoryId:u64@40, reservedBytes:u64@48, planNameBytes:u32@56, zero:4@60,
// plan volume:u32@64, zero:4@68, fileIndex:u64@72, createdBytes:u64@80,
// sourceBytes:u64@88, captured volume:u32@96, zero:4@100,
// fileIndex:u64@104, fileBytes:u64@112, payloadBytes:u64@120,
// digest:32@128, zero:16@160; then plan name ASCII, no NUL.
// Captured/AbortCaptured reuse this name as captured.name. Other stages require
// captured entirely empty. Reserved also requires plan entirely empty.
// Root header: magic@0, version:u32@16, headerBytes:u32@20, owner:16@24,
// epoch:u64@40, legacyBytes:u64@48, registryBytes:u64@56,
// totalBytes:u64@64, zero:8@72. Then exact v1 Manifest and v1/v2 Registry bytes.
}
// A directory area may occur once; names and physical file identities are
// globally unique within their appropriate namespace. IDs are nonzero; source
// IDs and creator/operation IDs cannot overlap their corresponding groups.
// Known bytes (sources + complete reservations) must not overflow uint64_t.
// No input is normalized, sorted, truncated or silently discarded.
Result ValidateSources(const SourceRegistry&) noexcept;
// Validate both objects, exact store owner/epoch, and every project reference.
// Empty owners are allowed and do NOT constitute eviction permission.
Result ValidateSourcesAgainstIndex(const SourceRegistry&,const Index&) noexcept;
Result EncodeSources(const SourceRegistry&,Bytes& output) noexcept;
Result DecodeSources(const void*,std::size_t,SourceRegistry& output) noexcept;
Result ValidateBusinessRoot(const BusinessRoot&) noexcept;
Result EncodeBusinessRoot(const BusinessRoot&,Bytes& output) noexcept;
// Also accepts a complete existing v1 Manifest as a legacy root, producing an
// empty SourceRegistry with matching owner/epoch. Encoding always writes the
// new composite form. Unknown versions/enums are Unsupported. All failures
// preserve outputs; decode/validation never grants disk recovery authority.
Result DecodeBusinessRoot(const void*,std::size_t,BusinessRoot& output) noexcept;
}
