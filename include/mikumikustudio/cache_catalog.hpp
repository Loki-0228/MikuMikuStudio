#pragma once
#include "cache_files.hpp"

// Internal durable snapshot publication, not cache capacity or eviction policy.
namespace mikumikustudio::cache_catalog {
enum class Result { Ok, Missing, Invalid, Busy, Io, Conflict, Limit, RecoveryRequired, Indeterminate };
inline constexpr std::size_t MaximumSnapshotBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaximumCommits = 64;
inline constexpr std::size_t MaximumSnapshots = 128;
inline constexpr std::uint64_t MaximumMetadataBytes = 64ull * 1024 * 1024;
// One checkpoint publication can use this bounded reserve; ordinary Commit
// cannot. Collection must reclaim retired names before the reserve is reused.
inline constexpr std::uint64_t MaintenanceReserveBytes = 32 * 1024;

struct CollectionReport {
    std::uint32_t attempted = 0;
    std::uint32_t absent = 0;
    std::uint32_t busy = 0;
    // Removal was accepted/already absent, but a subsequent check could not
    // confirm namespace absence (including a failed check).
    std::uint32_t pending = 0;
    std::uint32_t failed = 0;
    // Absence from this namespace, not a claim about volume free space.
    std::uint64_t absentBytes = 0;
};

class Store;
class Snapshot {
public:
    bool IsValid() const noexcept { return valid_; }
    std::uint64_t Revision() const noexcept { return revision_; }
    const cache_files::Owner& StoreId() const noexcept { return owner_; }
    const std::vector<std::uint8_t>& Bytes() const noexcept { return bytes_; }
private:
    friend class Store;
    void Swap(Snapshot&) noexcept;
    bool valid_ = false;
    std::filesystem::path root_;
    std::uint64_t revision_ = 0;
    cache_files::Owner owner_{};
    cache_files::BlobInfo head_;
    std::vector<std::uint8_t> bytes_;
};

class Store {
public:
    explicit Store(std::filesystem::path absoluteMetadataRoot);
    // Read never initializes a missing store. A valid older record is not a
    // fallback when any present commit or active referenced snapshot is damaged.
    // Only an exact retirement list permits missing historical metadata.
    Result Read(Snapshot& output) const noexcept;
    // Only an empty catalog can initialize. Failed or interrupted initialization
    // that leaves files requires recovery; it is never silently reinitialized.
    Result Initialize(const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept;
    // Initializes an empty catalog with a caller-prepared nonzero store identity,
    // so an initial payload can already contain that same identity. This grants
    // no permission to replace an existing catalog or recover unknown residue.
    // A zero owner is Invalid; every failure preserves output.
    Result InitializeOwned(const cache_files::Owner& owner,
        const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept;
    // Cross-process CAS under one file lock. expected and output may alias.
    // Every failure preserves output. Indeterminate means a commit write was
    // attempted: callers must reread rather than blindly retry the mutation.
    Result Commit(const Snapshot& expected, const std::vector<std::uint8_t>& bytes,
        Snapshot& output) const noexcept;
    // Publishes the same payload at revision+1 with a precise retirement list.
    // The new checkpoint is durable before any collection; no file is deleted.
    // Failures retain output; post-publication-attempt failure is Indeterminate.
    Result Checkpoint(const Snapshot& expected, Snapshot& output) const noexcept;
    // Collects only files retired by the current verified checkpoint. Read and
    // publication never collect implicitly. Once collection starts, Ok returns
    // a report including Busy/pending/failures, not an all-or-nothing promise.
    // Failed setup preserves report. Does not repair unknown/corrupt files.
    Result CollectRetired(CollectionReport& report) const noexcept;
private:
    std::filesystem::path root_;
    Result Publish(const Snapshot* expected, const std::vector<std::uint8_t>& bytes,
        Snapshot& output, const cache_files::Owner* initialOwner = nullptr) const noexcept;
};
}
