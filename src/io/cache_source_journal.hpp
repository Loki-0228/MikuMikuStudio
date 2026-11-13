#pragma once
#include "cache_source_registry.hpp"
#include <atomic>
#include <memory>

namespace mikumikustudio::cache_business {
enum class JournalResult {Ok,Invalid,Busy,Io,Conflict,Limit,RecoveryRequired,
    Indeterminate,NeedsCleanup,Cancelled,NotReady,Missing,AllocationFailure,Thread};
enum class CaptureState {New,Reserved,Prepared,Captured,Published,Cancelled};
struct SourceJournalState;
struct SourceCaptureState;
class SourceCapture;
// Internal durable responsibility journal, NOT a disk quota grant. A future
// unified coordinator must approve capacity before these production actions.
// This implementation is not connected to video import/decoding/history.
// All methods/destruction on one object are confined to its creating thread.
class SourceJournal {
public:
    ~SourceJournal();
    SourceJournal(const SourceJournal&)=delete;
    SourceJournal& operator=(const SourceJournal&)=delete;
    // All directories already exist and remain pinned. catalog is the existing
    // control/catalog child; the four physical directories must be distinct.
    // Reads the actual catalog/pages and checks its sole Video binding. Never
    // initializes or adopts a missing/unverified catalog. Failure preserves out.
    static JournalResult Open(const cache_files::Directory& control,
        const cache_files::Directory& pages,const cache_files::Directory& raw,
        std::unique_ptr<SourceJournal>& out) noexcept;
    // Explicit crash-recovery open, including after process restart. Only the
    // existing catalog's verified RecoverOwned protocol is allowed; no missing
    // root, unknown intent or invalid business/page binding is adopted.
    static JournalResult OpenForRecovery(const cache_files::Directory& control,
        const cache_files::Directory& pages,const cache_files::Directory& raw,
        std::unique_ptr<SourceJournal>& out) noexcept;
    // No disk I/O. Nonempty out is Busy. IDs and declaration are immutable.
    // Declaration is an operation record, never evidence of quota approval.
    JournalResult CreateCapture(OperationId,SourceId,std::uint64_t declaredReservationBytes,
        std::unique_ptr<SourceCapture>& out) noexcept;
    JournalResult Recover(OperationId) noexcept;
    JournalResult Acquire(SourceId,cache_files::RawSourceLease& out) noexcept;
    JournalResult Retire(SourceId) noexcept;
    // Retirement is metadata only and preserves owners. Indeterminate must be
    // resolved explicitly against the saved before/after source record.
    JournalResult ResolveRetirement(SourceId) noexcept;
private:
    static JournalResult OpenImpl(const cache_files::Directory&,
        const cache_files::Directory&,const cache_files::Directory&,
        std::unique_ptr<SourceJournal>&,bool recovery) noexcept;
    explicit SourceJournal(std::shared_ptr<SourceJournalState>);
    std::shared_ptr<SourceJournalState> state_;
};
class SourceCapture {
public:
    ~SourceCapture();
    SourceCapture(const SourceCapture&)=delete;
    SourceCapture& operator=(const SourceCapture&)=delete;
    CaptureState State() const noexcept;
    bool NeedsResolution() const noexcept;
    const OperationId& Id() const noexcept;
    JournalResult Reserve() noexcept;
    // Actual length must be <= declaration; durable Prepared reserve becomes
    // exactly that length. Retained unpublished files require Cancel before
    // trying a different source. No original path is needed for recovery.
    JournalResult Prepare(const wchar_t* originalPath) noexcept;
    // Holds the control lock through copy, handle transition and registration;
    // other processes see Busy, so recovery cannot seize a live copy's reopen gap.
    JournalResult Complete(const std::atomic_bool* cancelled=nullptr) noexcept;
    JournalResult Publish(const std::vector<ProjectId>& owners,std::uint64_t lastUse,
        cache_files::RawSourceLease& out) noexcept;
    JournalResult Cancel() noexcept;
    // Rechecks the exact attempted operation/source footprint. Uses existing
    // catalog RecoverOwned only if that catalog needs recovery. Never blindly
    // retries an uncertain mutation. A rolled-back mutation returns Conflict;
    // owned file responsibility remains available to Cancel/retry.
    JournalResult Resolve() noexcept;
    // Destruction performs no deletion or catalog I/O. Unfinished local file
    // responsibility and unresolved publications are retained by shared state, recoverable through
    // Recover(Id) on a later Open of the same physical binding in this process.
    // Call Cancel explicitly; abandoning objects does not refund any capacity.
private:
    friend class SourceJournal;
    SourceCapture(std::shared_ptr<SourceJournalState>,std::shared_ptr<SourceCaptureState>);
    std::shared_ptr<SourceJournalState> journal_;
    std::shared_ptr<SourceCaptureState> capture_;
};
}
