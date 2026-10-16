#pragma once
#include "edit_store.hpp"
#include <list>

namespace mikudancestudio::plugins {
// Internal native participants. A change owns the resources needed by both
// directions, including absent objects. It must never recover an object by
// reopening its source file. Public plugins use edit.h and serializer bytes.
class PreparedHistoryChange {
public:
    virtual ~PreparedHistoryChange() = default;
    virtual bool IsCurrent() const noexcept = 0;
    virtual void Publish() noexcept = 0;
};
class HistoryChange {
public:
    virtual ~HistoryChange() = default;
    virtual MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>&) = 0;
    virtual uint64_t RetainedBytes() const noexcept = 0;
};

// One journal for native and plugin changes. The application supplies its
// scene-mutation gate. It must route ALL editing paths here before exposing
// mms.edit; creating a separate instance for plugins is not supported.
class SharedHistory final : public EditHost {
public:
    class NativeTransaction;
    class AppliedTransaction;
    class ResetTransaction;
private:
    struct Node;
    struct Publication;
    using Nodes = std::list<std::shared_ptr<const Node>>;
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
    MmsResult Ready() const;
    MmsResult ReserveNode(std::shared_ptr<const EditRecord>, std::shared_ptr<HistoryChange>,
                         std::unique_ptr<Publication>&);
    void Publish(Publication&) noexcept;
    void InvalidatePending() noexcept;
    void ResetLocked() noexcept;
public:
    explicit SharedHistory(StorageStore&, std::function<MmsResult()> gate = {},
                           size_t maxEntries = 128, uint64_t maxBytes = 256ull * 1024 * 1024);
    ~SharedHistory();
    MmsResult Revision(uint64_t&) override;
    MmsResult Enter() override;
    void Leave() noexcept override;
    MmsResult Reserve(std::shared_ptr<const EditRecord>, std::unique_ptr<EditPublication>&) override;
    MmsResult History(bool redo) override;
    void AttachStore(EditStore*) override;
    void DetachStore(EditStore*) noexcept override;
    void Poison() noexcept override { poisoned_ = true; }
    // Prepare a native operation before changing live state. Cancellation is
    // destruction. Commit rechecks revision and source state, then consumes it.
    MmsResult PrepareNative(std::string label, std::shared_ptr<HistoryChange>,
                            std::unique_ptr<NativeTransaction>&);
    MmsResult CommitNative(std::unique_ptr<NativeTransaction>);
    // Synchronous native authors reserve BEFORE changing live state. The token
    // holds the journal gate until destruction; do not pump messages or invoke
    // plugins during this scope. Prepare validates the already-authored after
    // state using an inverse candidate. Commit only records it, without replay.
    // On any failure the caller must restore its captured state BEFORE dropping
    // the token. Token destruction cancels the reservation, not the live edit.
    MmsResult ReserveAppliedNative(std::string label, uint64_t maximumNativeBytes,
                                  std::unique_ptr<AppliedTransaction>&);
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
