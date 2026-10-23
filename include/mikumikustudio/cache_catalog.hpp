#pragma once
#include "cache_files.hpp"

// Internal durable snapshot publication, not cache capacity or eviction policy.
namespace mikumikustudio::cache_catalog {
enum class Result { Ok, Missing, Invalid, Busy, Io, Conflict, Limit, RecoveryRequired, Indeterminate };
inline constexpr std::size_t MaximumSnapshotBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaximumCommits = 64;
inline constexpr std::size_t MaximumSnapshots = 128;
inline constexpr std::uint64_t MaximumMetadataBytes = 64ull * 1024 * 1024;

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
    // fallback when any commit record or referenced snapshot is damaged.
    Result Read(Snapshot& output) const noexcept;
    // Only an empty catalog can initialize. Failed or interrupted initialization
    // that leaves files requires recovery; it is never silently reinitialized.
    Result Initialize(const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept;
    // Cross-process CAS under one file lock. expected and output may alias.
    // Every failure preserves output. Indeterminate means a commit write was
    // attempted: callers must reread rather than blindly retry the mutation.
    Result Commit(const Snapshot& expected, const std::vector<std::uint8_t>& bytes,
        Snapshot& output) const noexcept;
private:
    std::filesystem::path root_;
    Result Publish(const Snapshot* expected, const std::vector<std::uint8_t>& bytes,
        Snapshot& output) const noexcept;
};
}
