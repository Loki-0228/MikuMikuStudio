#pragma once
#include "cache_business_reader.hpp"
#include <memory>
#include <vector>

namespace mikumikustudio::cache_business {
enum class SourceBudgetResult {
    Ok, Absent, Invalid, Busy, Io, RecoveryRequired, Limit, AllocationFailure
};
struct SourceCreditRequirement {
    // Full durable operation declarations, not the yet-unwritten residual.
    // Published/retired sources are physical Inventory charges instead.
    std::uint64_t rawReservedBytes=0, rawAllocationReservedBytes=0;
    std::uint64_t maximumRootBytes=0, remainingPublications=0;
    std::uint64_t publicationBytesPerStep=0, checkpointBytesPerStep=0;
    std::uint64_t checkpointPublications=0;
    std::uint64_t pendingMetadataBytes=0, maintenanceMetadataBytes=0, metadataBytes=0;
    // Required current bytes above are not necessarily prepaid. Registry v1
    // funded only one-file checkpoints; v2 funds their intent and decision too.
    // Only this persisted-version-derived amount may serve as old credit when
    // policy no longer allows new work. A successful approved upgrade writes v2.
    std::uint64_t fundedMetadataBytes=0;
    std::uint64_t fundedCheckpointBytesPerStep=0;
};
struct SourceBudgetState;
struct RawSourceDeduction {
    OperationId operation{};
    cache_files::Identity identity{};
    std::uint64_t deductedBytes=0;
};
struct RawSourceBudgetProjection {
    // Each durable raw promise is rounded separately to the verified unit.
    std::uint64_t promisedBytes=0, deductedBytes=0, residualBytes=0;
    // Includes matched empty files with a zero deduction. The coordinator must
    // exclude these identities from any other writer's residual deduction.
    std::vector<RawSourceDeduction> deductions;
};
// Verified durable facts and required future credit, NOT quota approval. Raw
// reservations still need identity-based residual accounting against Inventory;
// existing raw/catalog/unknown bytes are not included in metadataBytes again.
// Caller supplies the verified volume allocation unit, not a separate limit.
class SourceBudgetSnapshot {
public:
    SourceBudgetSnapshot() noexcept;
    ~SourceBudgetSnapshot();
    SourceBudgetSnapshot(SourceBudgetSnapshot&&) noexcept;
    SourceBudgetSnapshot& operator=(SourceBudgetSnapshot&&) noexcept;
    SourceBudgetSnapshot(const SourceBudgetSnapshot&)=delete;
    SourceBudgetSnapshot& operator=(const SourceBudgetSnapshot&)=delete;
    bool IsValid() const noexcept;
    // Invalid/moved-from snapshots return static empty values. Returned
    // references expire when this snapshot is replaced or destroyed.
    const PublishedIndex& Business() const noexcept;
    const cache_files::DirectoryIdentity& RootIdentity() const noexcept;
    const cache_files::DirectoryIdentity& RawIdentity() const noexcept;
    const SourceCreditRequirement& Credit() const noexcept;
    // Recompute every pending obligation for a proposed registry using the
    // verified project's actual IDs and the same owner/epoch/raw binding.
    // This describes the next ordinary Commit, including its standby credit
    // even when the currently verified head is a checkpoint. It is not
    // publication/ownership permission. Proposals describe maintenance protocol
    // 2, matching SourceAdmission::Commit's atomic paid upgrade.
    SourceBudgetResult Project(const SourceRegistry&,SourceCreditRequirement&) const noexcept;
private:
    friend SourceBudgetResult ReadSourceBudget(const cache_files::Directory&,
        std::uint64_t,SourceBudgetSnapshot&) noexcept;
    friend SourceBudgetResult ReadSourceBudgetLocked(const cache_files::Directory&,
        std::uint64_t,SourceBudgetSnapshot&) noexcept;
    friend SourceBudgetResult ReadRecoverySourceBudget(const cache_files::Directory&,
        std::uint64_t,const cache_catalog::WritePlan&,SourceBudgetSnapshot&) noexcept;
    friend SourceBudgetResult ProjectRawSourceBudget(const SourceBudgetSnapshot&,
        const cache_files::InventorySnapshot&,RawSourceBudgetProjection&) noexcept;
    friend SourceBudgetResult ProjectRawSourceBudget(const SourceBudgetSnapshot&,
        const SourceRegistry&,const cache_files::InventorySnapshot&,RawSourceBudgetProjection&) noexcept;
    static SourceBudgetResult ReadImpl(const cache_files::Directory&,
        std::uint64_t,SourceBudgetSnapshot&,bool retainControlLock,
        const cache_catalog::WritePlan* recovery=nullptr) noexcept;
    std::unique_ptr<SourceBudgetState> state_;
};
// Fixed layout relative to an already pinned manager root:
// Video/{control/catalog,pages,raw}. Only missing Video is Absent. An existing
// partial/untrusted layout, missing locks or damaged catalog/pages is recovery
// required. Never initializes, recovers, creates files or refunds capacity.
// Caller already holds the common manager mutex/state lock; this function takes
// existing control then catalog locks, pins the actual published snapshot and
// validates all real pages and the sole Video directory binding. It does not
// retain locks, so callers must not treat an old result as the latest head.
SourceBudgetResult ReadSourceBudget(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,SourceBudgetSnapshot& output) noexcept;
// Same validation, but success retains the existing control lock until output
// is replaced/destroyed. Acquire before Inventory and keep it for the complete
// managed reservation lifetime; nested same-root work must reuse this snapshot.
// Do not reenter SourceJournal or call either read function for the same control
// while this lock is retained. Only protects an already-existing Video layout:
// no initialization, approval, implicit recovery or new write authority.
SourceBudgetResult ReadSourceBudgetLocked(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,SourceBudgetSnapshot& output) noexcept;

// Derives obligations at an existing, verified recovery endpoint. Caller holds
// the common manager and existing control locks and keeps the unconsumed
// recovery plan alive: that plan pins the catalog payload, this result pins
// actual business pages. Never reacquires control, executes recovery, creates
// files or grants quota. Rejects foreign-root/consumed plans, missing endpoints
// (including initialization rollback), bad pages and mismatched raw binding.
// The result describes the proposed recovery endpoint, NOT the currently
// readable head or proof of a prior quota approval. Physical pending/unknown
// files still require Inventory; the recovery plan's own cost is additional.
// All failure paths preserve output. As with ordinary budget observations,
// callers must not reuse facts across changes to their locked observation.
SourceBudgetResult ReadRecoverySourceBudget(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,const cache_catalog::WritePlan& recovery,
    SourceBudgetSnapshot& output) noexcept;

// Pure in-memory projection, not quota approval or evidence that snapshots are
// current. Caller must establish one coherent observation under its common lock.
// Requires matching manager/raw-directory identities. Missing operation files
// retain the full promise; wrong identities, aliases, links, delete-pending or
// excessive default-stream sizes require recovery. Only max(default logical,
// default allocation) is deducted, never ADS or any other inventory occupant.
// Physical inventory still charges all streams independently. Failure leaves
// output unchanged. No file access, locking, recovery or cleanup occurs here.
SourceBudgetResult ProjectRawSourceBudget(const SourceBudgetSnapshot&,
    const cache_files::InventorySnapshot&,RawSourceBudgetProjection&) noexcept;
// The proposed registry must match the verified owner/epoch, sole raw binding
// and actual project references. Reuses the same physical deduction rules;
// does not approve or publish the proposed change. Failure preserves output.
SourceBudgetResult ProjectRawSourceBudget(const SourceBudgetSnapshot&,
    const SourceRegistry& proposed,const cache_files::InventorySnapshot&,
    RawSourceBudgetProjection&) noexcept;

// Remaining publications include a successful prefix followed by cancellation:
// Reserved=4, Prepared=3, Captured=2, either Abort=1. Root growth allows each
// Reserved raw name up to 53 ASCII bytes and each publish up to all verified
// projects as owners, bounded by the actual source/root encoding limits.
// Future publication footprints use the explicitly approved catalog protocol,
// including its larger intent and rollback record, rounding each file separately.
// Ordinary legacy journals may use fewer bytes; this bound never discounts
// future production transitions to the smaller unapproved format.
// Maintenance reserves a maximum approved checkpoint, including its distinct
// intent and rollback decision, before every future publication
// plus terminal standby for an ordinary head: 1 + remainingPublications.
// A verified checkpoint head has already consumed standby, so actual reads use
// remainingPublications. This fact persists across process restart. Uncollected
// retired files are still physical Inventory charges; no refund is implied.
// Project always describes a new ordinary head and restores the +1 credit.
// Large roots may hit the byte
// ceiling long before the commit-count limit. This is a chain bound only.
// This reserves bytes only: authoritative WritePlan capacity checks and explicit
// checkpoint/collection/recovery still must execute. Busy retained files or
// repeated failed attempts cannot be declared reclaimed by this calculation.
// The bound covers these existing capture/cancel chains with unchanged project
// pages. New operations/project changes require recomputation. Future Retire or
// source deletion publications are not covered; nor is a policy for spending
// and replenishing maintenance credit. Actual disk bytes need Inventory too.
}
