#pragma once
#include "cache_source_budget.hpp"
#include <memory>

namespace mikumikustudio::cache_manager {
enum class SourceAdmissionResult {
    Ok, Invalid, Busy, Missing, Limit, Conflict, RecoveryRequired,
    Indeterminate, Io, AllocationFailure, Thread
};
// Internal metadata publication gate for an EXISTING trusted Video store.
// Open does not initialize storage, copy/delete raw files, grant ownership by name,
// or connect the unbudgeted SourceJournal to a production decoder.
// Thread-affine, including destruction. One mutation attempt consumes the
// session; close and reopen to obtain fresh facts. Pure preflight refusal does
// not consume it. No callbacks or nested legacy writers while it is open.
class SourceAdmission {
public:
    ~SourceAdmission();
    SourceAdmission(const SourceAdmission&)=delete;
    SourceAdmission& operator=(const SourceAdmission&)=delete;
    static SourceAdmissionResult Open(std::unique_ptr<SourceAdmission>& out) noexcept;
    // Explicit first publication within an already configured manager root.
    // Only an absent or verified empty fixed Video layout
    // can be used. Current policy must fund the complete initial publication and
    // standby credit. Never deletes directories or adopts unknown files; pending
    // publications require explicit recovery. Every failure preserves out.
    static SourceAdmissionResult Initialize(cache_catalog::Snapshot& out) noexcept;
    // Empty after consumption or on a different thread; references from an
    // earlier call describe only that session's pre-publication head.
    const cache_business::SourceBudgetSnapshot& Current() const noexcept;
    bool IsConsumed() const noexcept;
    // Caller supplies the exact expected catalog head; stale proposals cannot
    // erase intervening obligations. Project pages and binding are unchanged.
    // Approval covers actual inventory, remaining obligations and this plan's
    // peak. The trusted caller remains responsible for legal journal stages.
    // Publishes maintenance protocol 2 in this same approved transaction;
    // upgrading older credit pays the difference, never silently grandfathers it.
    SourceAdmissionResult Commit(const cache_catalog::Snapshot& expected,
        const cache_business::SourceRegistry& proposed,cache_catalog::Snapshot& out) noexcept;
    // Explicit maintenance, never an implicit fallback for Commit Limit.
    // Already-checkpoint heads return Conflict; collect their retirees instead.
    // A v1 full chain can be maintained under fresh current policy approval,
    // keeping its v1 payload/credit, then Commit upgraded after collection.
    SourceAdmissionResult Checkpoint(const cache_catalog::Snapshot& expected,
        cache_catalog::Snapshot& out) noexcept;
    // Only the catalog's verified retirement list can authorize deletion.
    // A report's Busy/pending/failed items remain physically charged on reopen.
    // An attempted collection consumes the session even if its setup fails.
    SourceAdmissionResult Collect(cache_catalog::CollectionReport& out) noexcept;
private:
    struct Impl;
    explicit SourceAdmission(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

// Explicit recovery of an EXISTING trusted Video store. Holds the same manager
// and control exclusion as normal admissions. Open only observes; it never
// creates a missing lock, initializes storage, reconciles or cleans legacy data.
// A verified durable approval permits only this operation's planned recovery,
// including its already reserved rollback decision after policy is disabled.
// It does not admit new publications or discount any actual inventory occupant.
// Unapproved recovery that creates a decision requires fresh current capacity.
// Thread-affine, including destruction; a mutation/pin-transfer attempt consumes
// the session. Close and reopen before retrying an uncertain outcome.
class SourceRecoveryAdmission {
public:
    ~SourceRecoveryAdmission();
    SourceRecoveryAdmission(const SourceRecoveryAdmission&)=delete;
    SourceRecoveryAdmission& operator=(const SourceRecoveryAdmission&)=delete;
    static SourceAdmissionResult Open(std::unique_ptr<SourceRecoveryAdmission>& out) noexcept;
    const cache_business::SourceBudgetSnapshot& Target() const noexcept;
    cache_catalog::RecoveryAction Action() const noexcept;
    bool IsConsumed() const noexcept;
    SourceAdmissionResult Execute(cache_catalog::RecoveryReport& out) noexcept;
private:
    struct Impl;
    explicit SourceRecoveryAdmission(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
