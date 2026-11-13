#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Internal Win7 file primitives for the cache manager. This layer does not grant
// cache policy, capacity, pinning or eviction permission. The manager owns those.
namespace mikumikustudio::cache_files {
enum class Result { Ok, Missing, Invalid, Corrupt, Busy, Io, Conflict, Limit, DeletePending, Cancelled, CleanupRequired };
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
// Mark only the inventoried file for deletion on the handle that verifies it.
// The caller supplies the cache policy and the authorized path.
Result DeleteObservedFile(const std::filesystem::path& path, const Identity& expected) noexcept;
struct DirectoryIdentity {
    std::uint32_t volume = 0;
    std::uint64_t fileIndex = 0;
};
enum class ObservationKind { File, Directory };
struct ObservationLimits {
    std::uint32_t maximumStreams = 128;
    std::size_t maximumStreamBufferBytes = 256 * 1024;
};
struct FileObservation {
    Identity identity{}; // fileBytes is the default stream's logical length
    std::uint64_t allocationBytes = 0; // default stream, not whole-volume usage
    // Full reported stream totals, including the default stream when present;
    // do not add identity.fileBytes/allocationBytes to these a second time.
    std::uint64_t streamLogicalBytes = 0, streamAllocationBytes = 0;
    std::uint32_t attributes = 0, links = 0, streamCount = 0;
    ObservationKind kind = ObservationKind::File;
    bool deletePending = false, namedStreamsSupported = false;
};
struct InventoryEntry {
    std::wstring relativePath; // root is empty; all children use backslashes
    std::uint32_t depth = 0;
    FileObservation observed;
};
struct InventoryLimits {
    std::size_t maximumEntries = 4096;
    std::uint32_t maximumDepth = 16;
    std::size_t maximumNameCodeUnits = 1024 * 1024;
    std::size_t maximumDirectoryGuards = 1024;
    ObservationLimits observation;
};
struct InventoryState;
// Metadata snapshots, not immutable data pins or an atomic tree snapshot.
// Holds the original ancestor chain and visited directory guards, but closes
// ordinary file metadata handles after observing them. External writers may
// change files or create new entries after observation. Cooperative admission
// must use its common writer lock. Nothing here grants ownership or deletion.
class InventorySnapshot {
public:
    InventorySnapshot() noexcept;
    ~InventorySnapshot();
    InventorySnapshot(InventorySnapshot&&) noexcept;
    InventorySnapshot& operator=(InventorySnapshot&&) noexcept;
    InventorySnapshot(const InventorySnapshot&) = delete;
    InventorySnapshot& operator=(const InventorySnapshot&) = delete;
    bool IsValid() const noexcept;
    const DirectoryIdentity& RootIdentity() const noexcept;
    const std::vector<InventoryEntry>& Entries() const noexcept;
private:
    friend class Directory;
    std::unique_ptr<InventoryState> state_;
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
struct RawSourceWriteState;
struct RawSourceLeaseState;
struct RawSourceCleanupState;
struct RawSourceInfo {
    std::wstring name;
    Identity identity{};
    Digest payloadDigest{};
    std::uint64_t payloadBytes = 0;
};
struct RawSourcePlan {
    std::wstring name;
    Identity createdIdentity{};
    std::uint64_t sourceBytes = 0;
};
// Receipt for a failed CREATE_NEW operation whose same-handle cleanup failed.
// The upper layer must retain it and account for the pending file. No identity
// query or path lookup is necessary to retry deletion of this exact handle.
class RawSourceCleanup {
public:
    RawSourceCleanup() noexcept;
    ~RawSourceCleanup();
    RawSourceCleanup(RawSourceCleanup&&) noexcept;
    RawSourceCleanup& operator=(RawSourceCleanup&&) noexcept;
    RawSourceCleanup(const RawSourceCleanup&) = delete;
    RawSourceCleanup& operator=(const RawSourceCleanup&) = delete;
    bool IsValid() const noexcept;
    const std::wstring& Name() const noexcept;
    Result Abort() noexcept;
    Result Observe(FileObservation& output, const ObservationLimits& limits = {}) const noexcept;
    // Destruction/move replacement only closes; never discards a file by name.
private:
    friend class Directory;
    std::unique_ptr<RawSourceCleanupState> state_;
};
// Raw encoded bytes have no cache envelope and can be opened by a decoder.
// These primitives grant no quota, ownership recovery or cleanup permission.
// Objects may move between threads; calls on one object must be serialized.
class RawSourceLease {
public:
    RawSourceLease() noexcept;
    ~RawSourceLease();
    RawSourceLease(RawSourceLease&&) noexcept;
    RawSourceLease& operator=(RawSourceLease&&) noexcept;
    RawSourceLease(const RawSourceLease&) = delete;
    RawSourceLease& operator=(const RawSourceLease&) = delete;
    bool IsValid() const noexcept;
    const RawSourceInfo& Info() const noexcept;
    // Fixed volume-GUID absolute path. Keep this lease alive through every
    // decoder open/use; closing it does not remove or authorize removing a file.
    const std::wstring& Path() const noexcept;
private:
    friend class Directory;
    friend class RawSourceWrite;
    std::unique_ptr<RawSourceLeaseState> state_;
};
class RawSourceWrite {
public:
    RawSourceWrite() noexcept;
    ~RawSourceWrite();
    RawSourceWrite(RawSourceWrite&&) noexcept;
    RawSourceWrite& operator=(RawSourceWrite&&) noexcept;
    RawSourceWrite(const RawSourceWrite&) = delete;
    RawSourceWrite& operator=(const RawSourceWrite&) = delete;
    bool IsValid() const noexcept;
    const RawSourcePlan& Plan() const noexcept;
    // Same-handle metadata only. A valid plan with an already closed writer
    // returns Busy; observation never reopens it by name.
    Result Observe(FileObservation& output, const ObservationLimits& limits = {}) const noexcept;
    // Failure preserves output and the cleanup plan, including after closing
    // the exclusive writer. Retrying restarts the bounded streaming copy.
    // Cancellation is polled between chunks, never invokes user code.
    Result Complete(RawSourceLease& output, const std::atomic_bool* cancelled = nullptr) noexcept;
    // Explicit same-handle disposition. A closed writer is reopened only after
    // identity verification. DeletePending is not physical reclamation.
    Result Abort() noexcept;
    // Destruction/move replacement only closes handles: the upper layer must
    // preserve plans and account for every uncompleted or pending file.
private:
    friend class Directory;
    std::unique_ptr<RawSourceWriteState> state_;
};

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
    Result Observe(FileObservation& output, const ObservationLimits& limits = {}) const noexcept;
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
    Result Observe(FileObservation& output, const ObservationLimits& limits = {}) const noexcept;
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
    // Shares the already pinned directory and all ancestor guards without
    // resolving a DOS path again. Failure preserves output; self-share is valid.
    Result Share(Directory& output) const noexcept;
    // Source must be a nonempty disk file without a final reparse
    // point, with a 1..16 ASCII alphanumeric extension. The extension is retained
    // in lowercase: <32 random lowercase hex>.raw.<extension>. Success creates
    // only a zero-length file; persist its intent before Complete. Failure
    // preserves output. cleanup must be empty (otherwise Busy, no changes).
    // CleanupRequired is the ONLY result publishing cleanup: identity capture
    // and same-handle deletion both failed; retain its exclusive handle until
    // explicit Abort succeeds. All other results leave cleanup empty. Never
    // infer ownership from name or silently discard a cleanup receipt.
    // Source hard links are allowed: the read pin blocks writers through every
    // link. Cache generations themselves must always have exactly one link.
    Result PrepareRawSource(const wchar_t* originalPath, std::uint64_t maximumBytes,
        RawSourceWrite& output, RawSourceCleanup& cleanup) const noexcept;
    // Caller must first verify a durable intent authorizing this exact root
    // and plan. Neither a filename nor a matching identity establishes ownership.
    // Opens exclusively with READ|DELETE, checks single-link identity and a
    // length no greater than sourceBytes, then returns same-handle cleanup.
    // Does not read the original source, resume copying or adopt unknown files.
    // A nonempty output is Busy; all failures preserve output and disk contents.
    Result RecoverRawSourceCleanup(const DirectoryIdentity& expectedRoot,
        const RawSourcePlan& plan, RawSourceCleanup& output) const noexcept;
    // Completed capture recovery: the caller supplies a trusted durable record.
    // Verifies root, exact immutable identity/length and the full payload digest
    // under one exclusive READ|DELETE handle. Success only returns that handle
    // as a cleanup receipt; it does not delete or grant ownership by filename.
    // Nonempty output is Busy; every failure preserves output and file bytes.
    Result RecoverRawSourceCleanup(const DirectoryIdentity& expectedRoot,
        const RawSourceInfo& captured, RawSourceCleanup& output) const noexcept;
    Result AcquireRawSource(const RawSourceInfo& expected, RawSourceLease& output) const noexcept;
    // Verifies identity, length and digest under an exclusive handle. Active
    // read leases make this Busy; successful disposition returns DeletePending.
    Result RemoveRawSource(const RawSourceInfo& expected) const noexcept;
    // Identity of the protected directory handle, not a blob's size/identity.
    Result GetIdentity(DirectoryIdentity& output) const noexcept;
    // Opens one validated child using this directory's pinned volume path.
    // Retains parent guards; does not resolve the caller's DOS drive again.
    Result Child(const std::wstring& name, bool create, Directory& output) const noexcept;
    // Per-open byte locks can move between threads. No callbacks under this lock.
    Result Lock(std::uint32_t waitMilliseconds, FileLock& output) const noexcept;
    // Same exclusion protocol, but never creates a missing .lock file. Used by
    // read-only budget inspection; Missing/Busy/errors preserve output.
    Result LockExisting(std::uint32_t waitMilliseconds, FileLock& output) const noexcept;
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
    // Includes root (depth 0, one entry/guard) and unknown ordinary files and
    // directories. Hard links remain separate rows; callers decide accounting
    // by identity. Reparse points, inaccessible data, changing observations and
    // incomplete/unsupported stream queries fail, preserving the old output.
    // Every directory is enumerated twice; metadata and named streams are
    // observed twice on the same handle. This detects changes during sampling,
    // not future changes or a globally simultaneous filesystem state.
    // Limits: entries 1..65536; depth 0..64; summed relative-path UTF-16 code
    // units 0..16 Mi; subtree directory guards 1..4096. Observation limits:
    // streams 1..4096, buffer bytes 32..4 MiB. Exceeded configured bounds =>
    // Limit; invalid parameters => Invalid. No payload reads or implicit retry.
    // Stream totals include the unnamed data stream when reported and all ADS
    // (including directory ADS). They do not measure MFT/whole-volume metadata.
    Result Inventory(const InventoryLimits&, InventorySnapshot& output,
        const std::atomic_bool* cancelled = nullptr) const noexcept;
private:
    Result LockImpl(std::uint32_t waitMilliseconds, FileLock& output, bool create) const noexcept;
    Result EnumerateNames(std::size_t maximumEntries, std::vector<std::wstring>& output, bool candidatesOnly) const noexcept;
    std::shared_ptr<DirectoryState> state_;
};
}
