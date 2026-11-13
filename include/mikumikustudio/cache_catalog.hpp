#pragma once
#include "cache_files.hpp"
#include "cache_approval.hpp"
#include <array>
#include <memory>

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
// Approved maintenance additionally retains two full operation declarations.
// Ordinary checkpoints retain the original bound above.
inline constexpr std::uint64_t ApprovedMaintenanceReserveBytes = 128 * 1024;

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

struct RecoveryReport {
    bool confirmedPublication = false;
    bool rolledBack = false;
    std::uint64_t revision = 0;
};

enum class WriteKind { Initialize, Commit, Checkpoint, Recovery };
enum class RecoveryAction { None, Noop, ConfirmPublication, Rollback };
enum class WriteRole { Snapshot, Commit, Intent, RollbackDecision, Checkpoint };
struct WriteFileFootprint {
    WriteRole role = WriteRole::Snapshot;
    // Full file size including its envelope. Each slot is a distinct file;
    // callers round each slot separately to the observed volume granularity.
    std::uint64_t maximumFileBytes = 0;
};
struct WriteFootprint {
    std::array<WriteFileFootprint, 4> files{};
    std::uint32_t count = 0;
};
// Pure protocol sizing, shared with real plan execution. No quota approval.
// Failure leaves output unchanged. Checkpoints require 1..MaximumRetired refs.
Result PublicationFootprint(std::size_t payloadBytes, WriteFootprint&) noexcept;
Result ApprovedPublicationFootprint(std::size_t payloadBytes, WriteFootprint&) noexcept;
Result CheckpointFootprint(std::size_t retiredCount, WriteFootprint&) noexcept;
Result MaximumCheckpointFootprint(WriteFootprint&) noexcept;
Result ApprovedCheckpointFootprint(std::size_t retiredCount, WriteFootprint&) noexcept;
Result MaximumApprovedCheckpointFootprint(WriteFootprint&) noexcept;
struct WritePlanState;
class Snapshot;
class WritePlan {
public:
    WritePlan() noexcept;
    ~WritePlan();
    // Same-thread use. Moving from an executing plan leaves the destination
    // empty; move-assignment involving an executing plan preserves both sides.
    // The caller must keep the plan alive until execution returns.
    WritePlan(WritePlan&&) noexcept;
    WritePlan& operator=(WritePlan&&) noexcept;
    WritePlan(const WritePlan&) = delete;
    WritePlan& operator=(const WritePlan&) = delete;
    bool IsValid() const noexcept;
    WriteKind Kind() const noexcept;
    const WriteFootprint& Footprint() const noexcept;
    // Facts, not recovery/ownership/quota authority. Non-recovery or invalid
    // plans return None/an empty snapshot. Initial rollback has no target head.
    RecoveryAction Recovery() const noexcept;
    const Snapshot& RecoveryTarget() const noexcept;
    const cache_files::DirectoryIdentity& RootIdentity() const noexcept;
    bool RequiresApproval() const noexcept;
    // Recovery facts only: false for ordinary publications and invalid plans.
    bool RecoveryIsCheckpoint() const noexcept;
    // Pending first publication only; rollback deliberately has no target head.
    bool RecoveryIsInitialization() const noexcept;
    // Verified approved recovery facts only; old intents never acquire these.
    // Facts alone do not grant ownership or budget authority.
    const PublicationApproval* RecoveryApproval() const noexcept;
    // An unconsumed recovery plan pins its verified target payload. Confirm
    // execution may transfer that pin to exclusive validation/flush under the
    // catalog lock after root/intent checks. This consumes the plan even if
    // exclusive reopening fails; consumed plans retain facts, not a pin promise.
    // All returned references expire when the plan is moved/replaced/destroyed.
    bool IsConsumed() const noexcept;
    // Sticky evidence of a write/flush/removal attempt, even on Io before the
    // commit attempt. Not a deletion receipt or permission to refund capacity.
    bool MayHaveWritten() const noexcept;
private:
    friend class Store;
    std::unique_ptr<WritePlanState> state_;
};

class Store;
class Snapshot {
public:
    bool IsValid() const noexcept { return valid_; }
    // The verified catalog head, not the payload or any historic ancestor.
    bool IsCheckpoint() const noexcept {
        return valid_ && head_.spec.kind == cache_files::Kind::CatalogCheckpoint;
    }
    // Exact verified physical catalog root and complete immutable head identity,
    // owner and revision. Path aliases alone do not make versions different.
    // Invalid snapshots never match.
    bool SameVersion(const Snapshot& other) const noexcept;
    std::uint64_t Revision() const noexcept { return revision_; }
    const cache_files::Owner& StoreId() const noexcept { return owner_; }
    const std::vector<std::uint8_t>& Bytes() const noexcept { return bytes_; }
private:
    friend class Store;
    void Swap(Snapshot&) noexcept;
    bool valid_ = false;
    std::filesystem::path root_;
    cache_files::DirectoryIdentity rootIdentity_{};
    bool boundRoot_ = false;
    std::uint64_t revision_ = 0;
    cache_files::Owner owner_{};
    cache_files::BlobInfo head_;
    std::vector<std::uint8_t> bytes_;
};

class Store {
public:
    explicit Store(std::filesystem::path absoluteMetadataRoot);
    // Retains a fixed directory and its ancestor guards. Copies share that
    // binding. A moved-from or never-opened binding reports Invalid on every
    // operation; it never falls back to resolving a path.
    explicit Store(cache_files::Directory&& directory);
    // Plans copy inputs and bind the physical root and verified catalog state.
    // Require an existing root and snapshots/commits children; only an empty
    // .lock may be created. No payload or intent is created. Failure preserves
    // output. Plans hold directory guards, but not the catalog publication lock.
    Result PlanInitialize(const std::vector<std::uint8_t>& bytes, WritePlan&) const noexcept;
    Result PlanInitializeOwned(const cache_files::Owner&, const std::vector<std::uint8_t>& bytes, WritePlan&) const noexcept;
    Result PlanCommit(const Snapshot&, const std::vector<std::uint8_t>& bytes, WritePlan&) const noexcept;
    // Existing directories and lock only. Sizes approved records, not a quota
    // grant. Ordinary Execute refuses these plans; the trusted caller approves.
    // Existing empty scaffold and locks only: never bootstraps directories.
    Result PlanApprovedInitializeOwned(const cache_files::Owner&, const std::vector<std::uint8_t>&, WritePlan&) const noexcept;
    Result PlanApprovedCommit(const Snapshot&, const std::vector<std::uint8_t>& bytes, WritePlan&) const noexcept;
    Result ExecuteApproved(WritePlan&, const PublicationApproval&, Snapshot&) const noexcept;
    Result PlanCheckpoint(const Snapshot&, WritePlan&) const noexcept;
    // Existing lock and operations directory; publishes one approved candidate.
    Result PlanApprovedCheckpoint(const Snapshot&, WritePlan&) const noexcept;
    Result PlanRecovery(WritePlan&) const noexcept;
    // Read-only planning for the common admission gate: never creates .lock.
    Result PlanRecoveryExistingLocks(WritePlan&) const noexcept;
    // Revalidate under the catalog lock before any mutation; never re-plan or
    // enlarge a footprint. Pure preflight failure preserves plan and outputs.
    // Success or a mutation attempt consumes the plan; failed attempts require
    // explicit recovery/reconciliation, not automatic retry or quota refund.
    Result Execute(WritePlan&, Snapshot&) const noexcept;
    Result ExecuteRecovery(WritePlan&, RecoveryReport&) const noexcept;
    // Read never initializes a missing store. A valid older record is not a
    // fallback when any present commit or active referenced snapshot is damaged.
    // Only an exact retirement list permits missing historical metadata.
    Result Read(Snapshot& output) const noexcept;
    // Pins the verified current snapshot under the same catalog lock as Read.
    // Success replaces lease with the already verified snapshot lease, so the
    // catalog cannot retire that snapshot while the caller holds it; Read
    // itself adds no second file read. Both outputs remain unchanged on
    // failure. The returned lease outlives this Store and other result copies.
    Result ReadPinned(Snapshot& output, cache_files::ReadLease& lease) const noexcept;
    // Read-only budget observation: requires the lock file to already exist.
    Result ReadPinnedExistingLocks(Snapshot& output, cache_files::ReadLease& lease) const noexcept;
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
    // Two path origins require identical path spelling as before. If either
    // origin is explicitly directory-bound, physical root identity replaces
    // that spelling check; all origins still require matching root and head.
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
    // Checks the expected head under the same lock used for collection. Stale or
    // foreign expectations fail without deletion and preserve report.
    Result CollectRetired(const Snapshot& expected, CollectionReport& report) const noexcept;
    // Explicit recovery of a publication with a complete, verified operation
    // record. Unknown/partial operation records are never adopted. A failure
    // preserves report, but may leave a durable rollback decision and partially
    // completed cleanup; another call resumes that decision under the same lock.
    Result RecoverOwned(RecoveryReport& report) const noexcept;
private:
    std::filesystem::path root_;
    bool boundMode_ = false;
    std::shared_ptr<cache_files::Directory> bound_;
    cache_files::Result OpenRoot(bool create, cache_files::Directory&) const noexcept;
    bool AcceptOrigin(const Snapshot&) const noexcept;
    Result ReadImpl(Snapshot&, cache_files::ReadLease*, bool requireExistingLocks = false) const noexcept;
    Result Publish(const Snapshot* expected, const std::vector<std::uint8_t>& bytes,
        Snapshot& output, const cache_files::Owner* initialOwner,
        WritePlanState&, bool planning, const PublicationApproval* approval = nullptr) const noexcept;
    Result CheckpointImpl(const Snapshot&, Snapshot&, WritePlanState&, bool planning, const PublicationApproval* approval = nullptr) const noexcept;
    Result RecoverImpl(RecoveryReport&, WritePlanState&, bool planning) const noexcept;
    Result CollectImpl(const Snapshot* expected, CollectionReport&) const noexcept;
    Result PreparePlan(WriteKind, const Snapshot*, const std::vector<std::uint8_t>&,
        const cache_files::Owner*, WritePlan&, bool approved = false, bool existingLocks = false) const noexcept;
    Result Bootstrap() const noexcept;
};
}
