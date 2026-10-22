#pragma once
#include "edit_store.hpp"
#include <list>
#include <array>

namespace mikumikustudio::plugins {
// Internal effect bits for one successful history publication. They name the
// consumers that must be refreshed; they never replace RequiresPhysicsReset,
// never bypass the edit gate and never suppress a prepared publication.
enum class HistoryEffects : uint32_t {
    None = 0,
    ProjectData = 1,
    SceneReadout = 2,
    Viewport = 4,
    All = 7
};
constexpr HistoryEffects operator|(HistoryEffects left, HistoryEffects right) noexcept {
    return static_cast<HistoryEffects>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}
constexpr HistoryEffects operator&(HistoryEffects left, HistoryEffects right) noexcept {
    return static_cast<HistoryEffects>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}
constexpr bool HasHistoryEffect(HistoryEffects value, HistoryEffects effect) noexcept {
    return (value & effect) != HistoryEffects::None;
}

// Internal native participants. A change owns the resources needed by both
// directions, including absent objects. It must never recover an object by
// reopening its source file. Public plugins use edit.h and serializer bytes.
class PreparedHistoryChange {
public:
    virtual ~PreparedHistoryChange() = default;
    virtual bool IsCurrent() const noexcept = 0;
    virtual void Publish() noexcept = 0;
    // Optional complete native charge AFTER this candidate is published.
    // Calculate it during Prepare, not here; this accessor must not allocate.
    // False uses HistoryChange's fixed charge. Dynamic owners must include all
    // retained record/resources and reject incomplete measurements in Prepare.
    virtual bool TargetRetainedBytes(uint64_t&) const noexcept { return false; }
};
class SharedHistory;
class HistoryChange {
public:
    virtual ~HistoryChange() = default;
    virtual MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>&) = 0;
    // The actual reservation intent is authoritative, including a private
    // native participant joined to an explicit plugin transaction. Default
    // participants retain the existing Prepare contract.
    virtual MmsResult PrepareInitial(SharedHistory&, bool, std::unique_ptr<PreparedHistoryChange>& out) { return Prepare(true, out); }
    virtual uint64_t RetainedBytes() const noexcept = 0;
    // Metadata-only participants may opt out. Unknown native and plugin
    // changes retain the conservative physics invalidation policy.
    virtual bool RequiresPhysicsReset() const noexcept { return true; }
    // Read during preparation only and frozen on the history node. The default
    // is conservative for unknown native participants and derived classes.
    virtual HistoryEffects Effects() const noexcept { return HistoryEffects::All; }
};

// Trusted host wakeup only, never a plugin callback. The borrowed context must
// outlive its installation. published may only record the revision/post work;
// it must not allocate, pump messages, read plugins or reenter scene editing.
struct HistoryPublicationSignal {
    void* context = nullptr;
    void (*published)(void*, uint64_t) noexcept = nullptr;
};
// Trusted native coordination only. A domain owns bounded metadata, never a
// second journal. Preparation creates a private domain; only a successful
// no-fail native publication installs it. Reset invalidates retained receipts.
class NativeHistoryDomain {
public:
    virtual ~NativeHistoryDomain() = default;
    virtual void Reset() noexcept = 0;
};

// One journal for native and plugin changes. The application supplies its
// scene-mutation gate. It must route ALL editing paths here before exposing
// mms.edit; creating a separate instance for plugins is not supported.
class SharedHistory final : public EditHost {
public:
    enum class RecordingMode { AllOperations, ExplicitOnly };
    enum class NativeIntent { ExplicitCommit, Adjustment };
    class NativeTransaction;
    class AppliedTransaction;
    class ResetTransaction;
private:
    struct Node;
    struct Publication;
    using Nodes = std::list<std::shared_ptr<Node>>;
    StorageStore& storage_;
    DWORD thread_ = GetCurrentThreadId();
    std::function<MmsResult()> gate_;
    Nodes undo_, redo_, retired_;
    uint64_t revision_ = 1, bytes_ = 0, maxBytes_;
    size_t maxEntries_;
    bool entered_ = false, poisoned_ = false;
    NativeTransaction* pending_ = nullptr;
    AppliedTransaction* applied_ = nullptr;
    ResetTransaction* reset_ = nullptr;
    EditStore* edits_ = nullptr;
    RecordingMode recordingMode_ = RecordingMode::AllOperations;
    bool lastPhysicsReset_ = true;
    HistoryEffects lastEffects_ = HistoryEffects::All;
    HistoryPublicationSignal publicationSignal_{};
    struct NativeDomainSlot {
        const void* key = nullptr;
        std::shared_ptr<NativeHistoryDomain> value;
        const void* preparingKey = nullptr;
        std::weak_ptr<NativeHistoryDomain> preparing;
    };
    std::array<NativeDomainSlot, 8> nativeDomains_{};
    uint64_t nativeEpoch_ = 1;
    MmsResult Ready() const;
    MmsResult ReserveNode(std::shared_ptr<const EditRecord>, std::shared_ptr<HistoryChange>,
                         std::unique_ptr<Publication>&, bool record = true);
    void Publish(Publication&) noexcept;
    void NotifyPublication() noexcept;
    void InvalidatePending() noexcept;
    void ResetLocked() noexcept;
public:
    explicit SharedHistory(StorageStore&, std::function<MmsResult()> gate = {},
                           size_t maxEntries = 128, uint64_t maxBytes = 256ull * 1024 * 1024);
    ~SharedHistory();
    // UI-thread lifecycle boundary, independent of the application edit gate.
    // Both fields are null to detach; otherwise both must be non-null. Swap
    // only while no edit/reset/storage callback or reservation is active.
    // Reset is an epoch boundary and deliberately does not emit this signal.
    MmsResult SetPublicationSignal(HistoryPublicationSignal) noexcept;
    std::shared_ptr<NativeHistoryDomain> FindNativeDomain(const void*) const noexcept;
    std::shared_ptr<NativeHistoryDomain> PrepareNativeDomain(const void*, const std::shared_ptr<NativeHistoryDomain>&) noexcept;
    bool CanPublishNativeDomain(const void*, const NativeHistoryDomain*) const noexcept;
    void PublishNativeDomain(const void*, const std::shared_ptr<NativeHistoryDomain>&) noexcept;
    uint64_t NativeEpoch() const noexcept { return GetCurrentThreadId() == thread_ ? nativeEpoch_ : 0; }
    MmsResult Revision(uint64_t&) override;
    MmsResult Enter() override;
    void Leave() noexcept override;
    // Check standalone native-edit admission, including pending plugin edits,
    // without keeping a reservation across a modal UI loop. Recheck on apply.
    MmsResult CheckNativeEdit();
    // Read-only transport admission. The caller checks application/UI locks;
    // playback itself is permitted so navigation can stop it. Never bypasses
    // a prepared publication, serializer or edit callback/transaction.
    MmsResult CheckNativeNavigation() const;
    RecordingMode Mode() const noexcept { return recordingMode_; }
    MmsResult SetMode(RecordingMode);
    size_t MaxEntries() const noexcept { return maxEntries_; }
    uint64_t MaxBytes() const noexcept { return maxBytes_; }
    // Persist before any journal eviction. Failure leaves limits and nodes
    // unchanged; callback executes under the normal history reentry gate.
    MmsResult Configure(RecordingMode, size_t entries, uint64_t bytes,
        const std::function<MmsResult()>& persist = {});
    MmsResult Reserve(std::shared_ptr<const EditRecord>, std::unique_ptr<EditPublication>&) override;
    MmsResult History(bool redo) override;
    void AttachStore(EditStore*) override;
    void DetachStore(EditStore*) noexcept override;
    void Poison() noexcept override { poisoned_ = true; }
    // Prepare a native operation before changing live state. Cancellation is
    // destruction. Commit rechecks revision and source state, then consumes it.
    MmsResult PrepareNative(std::string label, std::shared_ptr<HistoryChange>,
                            std::unique_ptr<NativeTransaction>&, NativeIntent = NativeIntent::ExplicitCommit);
    MmsResult CommitNative(std::unique_ptr<NativeTransaction>);
    // Synchronous native authors reserve BEFORE changing live state. The token
    // holds the journal gate until destruction; do not pump messages or invoke
    // plugins during this scope. Prepare validates the already-authored after
    // state using an inverse candidate. Commit only records it, without replay.
    // On any failure the caller must restore its captured state BEFORE dropping
    // the token. Token destruction cancels the reservation, not the live edit.
    MmsResult ReserveAppliedNative(std::string label, uint64_t maximumNativeBytes,
                                  std::unique_ptr<AppliedTransaction>&, NativeIntent = NativeIntent::ExplicitCommit);
    MmsResult PrepareAppliedNative(AppliedTransaction&, std::shared_ptr<HistoryChange>);
    MmsResult CommitAppliedNative(AppliedTransaction&) noexcept;
    // Project boundaries invalidate prepared transactions without reusing a
    // revision. Retired resources are reclaimed outside the no-fail publication.
    MmsResult Reset();
    // Acquire before StorageStore::Prepare for a project replacement. Commit
    // only at the first destructive native step; cancellation preserves the
    // old journal. The token excludes edits until it is destroyed. Commit may
    // run with prepared serializers, but never from a serializer callback.
    MmsResult PrepareReset(std::unique_ptr<ResetTransaction>&);
    void Collect() noexcept;
    size_t UndoCount() const noexcept { return undo_.size(); }
    size_t RedoCount() const noexcept { return redo_.size(); }
    uint64_t RetainedBytes() const noexcept { return bytes_; }
    const std::string* NextLabel(bool redo) const noexcept;
    // Describes the last successful publication, including undo and redo.
    bool LastPublicationRequiresPhysicsReset() const noexcept { return lastPhysicsReset_; }
    // Same thread contract as the physics accessor. Failures, cancellations and
    // rejected gates keep the previous value; reset restores the default.
    HistoryEffects LastPublicationEffects() const noexcept { return lastEffects_; }
    // Read-only preview of the frozen mask that the next successful undo/redo
    // would publish. It never enters the gate, prepares a candidate or consumes
    // a node. Empty journals, non-UI threads and NotReady states report All
    // conservatively. This is not an edit permission; the final result remains
    // LastPublicationEffects.
    HistoryEffects NextPublicationEffects(bool redo) const noexcept;
};
class SharedHistory::NativeTransaction {
    friend class SharedHistory;
    SharedHistory* owner_;
    NativeTransaction* previous_ = nullptr;
    NativeTransaction* next_ = nullptr;
    uint64_t revision_;
    std::unique_ptr<Publication> publication_;
    NativeTransaction(SharedHistory&, std::unique_ptr<Publication>);
public:
    ~NativeTransaction();
    NativeTransaction(const NativeTransaction&) = delete;
    NativeTransaction& operator=(const NativeTransaction&) = delete;
};
class SharedHistory::ResetTransaction {
    friend class SharedHistory;
    SharedHistory* owner_;
    bool committed_ = false;
    explicit ResetTransaction(SharedHistory&);
public:
    ~ResetTransaction();
    ResetTransaction(const ResetTransaction&) = delete;
    ResetTransaction& operator=(const ResetTransaction&) = delete;
    // Reclaims native owners before the caller frees the old scene/world.
    // No allocations. Keeps the journal locked through serializer commit.
    MmsResult Commit() noexcept;
};
class SharedHistory::AppliedTransaction {
    friend class SharedHistory;
    SharedHistory* owner_;
    uint64_t maximumNativeBytes_, nativeBytes_ = 0;
    std::shared_ptr<Node> node_;
    std::unique_ptr<Publication> publication_;
    std::unique_ptr<PreparedHistoryChange> verification_;
    bool busy_ = false, committed_ = false;
    AppliedTransaction(SharedHistory&, uint64_t, std::shared_ptr<Node>, std::unique_ptr<Publication>);
    void Release() noexcept;
public:
    // Main-thread only. The native capture must be rolled back first unless
    // CommitAppliedNative succeeded. Captures must not outlive their scene.
    ~AppliedTransaction();
    AppliedTransaction(const AppliedTransaction&) = delete;
    AppliedTransaction& operator=(const AppliedTransaction&) = delete;
};
}
