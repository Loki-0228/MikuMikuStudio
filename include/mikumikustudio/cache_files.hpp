#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Internal Win7 file primitives for the cache manager. This layer does not grant
// cache policy, capacity, pinning or eviction permission. The manager owns those.
namespace mikumikustudio::cache_files {
enum class Result { Ok, Missing, Invalid, Corrupt, Busy, Io, Conflict, Limit, DeletePending };
enum class Kind : std::uint32_t { Effect = 1, Texture = 2, CatalogSnapshot = 3, CatalogCommit = 4, CatalogCheckpoint = 5, IndexPage = 6, CatalogIntent = 7 };
using Owner = std::array<std::uint8_t, 16>;
using Digest = std::array<std::uint8_t, 32>;
inline constexpr std::uint64_t HeaderBytes = 128;
inline constexpr std::uint64_t MaximumPayloadBytes = 1024ull * 1024 * 1024;
struct Spec {
    Owner owner{};
    Kind kind = Kind::Effect;
    Digest key{};
};
struct Identity {
    std::uint32_t volume = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t fileBytes = 0;
};
struct BlobInfo {
    std::wstring name;
    Spec spec;
    Digest payloadDigest{};
    std::uint64_t payloadBytes = 0;
    Identity identity;
};
struct WritePlan {
    std::wstring name;
    Spec spec;
    Digest payloadDigest{};
    std::uint64_t payloadBytes = 0;
    Identity createdIdentity{};
    // Planned complete envelope size, not a progress counter. The created file
    // has length zero; an interrupted write can have any length up to this cap.
    std::uint64_t completedBytes = 0;
};
struct DirectoryState;
struct LeaseState;
struct LockState;
struct PreparedWriteState;

class PreparedWrite {
public:
    PreparedWrite() noexcept;
    ~PreparedWrite();
    PreparedWrite(PreparedWrite&&) noexcept;
    PreparedWrite& operator=(PreparedWrite&&) noexcept;
    PreparedWrite(const PreparedWrite&) = delete;
    PreparedWrite& operator=(const PreparedWrite&) = delete;
    bool IsValid() const noexcept;
    const WritePlan& Plan() const noexcept;
    // Checks this pinned handle against the plan. Short files are incomplete;
    // full-length files must pass envelope/digest verification. Failure leaves
    // complete unchanged and never authorizes recovery by itself.
    Result Inspect(bool& complete) const noexcept;
    // Recovery must obtain a successful flush, not just readable complete bytes.
    // The verified, complete generation remains pinned by this same handle.
    Result Flush() noexcept;
    // Failures retain this exclusive handle and preserve output. Success closes
    // and consumes the handle. Destruction/move replacement only closes it;
    // neither operation removes an uncompleted file.
    Result Complete(const void* bytes, std::size_t size, BlobInfo& output) noexcept;
    // Explicit same-handle deletion. Failure retains the handle. DeletePending
    // does not promise physical space reclamation.
    Result Abort() noexcept;
private:
    friend class Directory;
    std::unique_ptr<PreparedWriteState> state_;
};

class ReadLease {
public:
    ReadLease() noexcept;
    ~ReadLease();
    ReadLease(ReadLease&&) noexcept;
    ReadLease& operator=(ReadLease&&) noexcept;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    bool IsValid() const noexcept;
    const BlobInfo& Info() const noexcept;
    // Reads and verifies this same handle; failures preserve output. A lease may
    // move between threads, but calls on the same lease must not overlap.
    Result ReadAll(std::vector<std::uint8_t>& output) const noexcept;
private:
    friend class Directory;
    std::unique_ptr<LeaseState> state_;
};
class FileLock {
public:
    FileLock() noexcept;
    ~FileLock();
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool IsValid() const noexcept;
private:
    friend class Directory;
    std::unique_ptr<LockState> state_;
};
class Directory {
public:
    Directory() noexcept;
    ~Directory();
    Directory(Directory&&) noexcept;
    Directory& operator=(Directory&&) noexcept;
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    bool IsValid() const noexcept;
    // Absolute local drive paths only; no UNC/device paths, reparse points or
    // parent traversal. Internal operations use the opened volume's GUID path.
    // Directory guards deny write/delete sharing throughout
    // this object and every derived lease/lock. Failures preserve output.
    static Result Open(const std::filesystem::path& root, bool create, Directory& output) noexcept;
    // Opens one validated child using this directory's pinned volume path.
    // Retains parent guards; does not resolve the caller's DOS drive again.
    Result Child(const std::wstring& name, bool create, Directory& output) const noexcept;
    // Per-open byte locks can move between threads. No callbacks under this lock.
    Result Lock(std::uint32_t waitMilliseconds, FileLock& output) const noexcept;
    // Creates a unique generation, computes SHA-256, writes and flushes it.
    // The manager publishes its index only after success. A process crash may
    // leave an orphan, which manager recovery must account for before new writes.
    Result Create(const Spec& spec, const void* bytes, std::size_t size, BlobInfo& output) const noexcept;
    // Hashes and allocates before creating an exclusive zero-length generation.
    // The caller persists a trusted intent before calling Complete. Failure
    // preserves output; a crash after creation can still leave an orphan.
    Result Prepare(const Spec& spec, const void* bytes, std::size_t size, PreparedWrite& output) const noexcept;
    // Only a caller-validated, trusted intent authorizes recovery. This method
    // checks the plan and exact file identity/length, not ownership provenance.
    Result ReopenPrepared(const WritePlan& plan, PreparedWrite& output) const noexcept;
    // Inspect verifies a generation's complete envelope/digest, but does not
    // confer index ownership or permission to remove an unindexed file.
    Result Inspect(const std::wstring& name, const Owner& owner, std::uint64_t maxPayload,
        ReadLease& output) const noexcept;
    // Expected immutable identity comes from the manager's trusted index.
    Result Acquire(const BlobInfo& expected, ReadLease& output) const noexcept;
    // Verifies and marks the same exclusive handle for deletion. DeletePending
    // means disposition was accepted, not that physical space was reclaimed.
    Result Remove(const BlobInfo& expected) const noexcept;
    // Bounded candidate names only. Foreign files are never removed or opened.
    Result Enumerate(std::size_t maximumEntries, std::vector<std::wstring>& output) const noexcept;
    // Includes foreign names for strict metadata-store validation, without
    // opening or deleting entries. Excludes only the OS dot entries.
    Result EnumerateAll(std::size_t maximumEntries, std::vector<std::wstring>& output) const noexcept;
private:
    Result EnumerateNames(std::size_t maximumEntries, std::vector<std::wstring>& output, bool candidatesOnly) const noexcept;
    std::shared_ptr<DirectoryState> state_;
};
}
