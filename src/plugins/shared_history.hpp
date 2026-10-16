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
    MmsResult Ready() const;
    MmsResult ReserveNode(std::shared_ptr<const EditRecord>, std::shared_ptr<HistoryChange>,
                         std::unique_ptr<Publication>&);
    void Publish(Publication&) noexcept;
public:
    class NativeTransaction;
    explicit SharedHistory(StorageStore&, std::function<MmsResult()> gate = {},
                           size_t maxEntries = 128, uint64_t maxBytes = 256ull * 1024 * 1024);
    ~SharedHistory();
    MmsResult Revision(uint64_t&) override;
    MmsResult Enter() override;
    void Leave() noexcept override;
    MmsResult Reserve(std::shared_ptr<const EditRecord>, std::unique_ptr<EditPublication>&) override;
    MmsResult History(bool redo) override;
    void Poison() noexcept override { poisoned_ = true; }
    // Prepare a native operation before changing live state. Cancellation is
    // destruction. Commit rechecks revision and source state, then consumes it.
    MmsResult PrepareNative(std::string label, std::shared_ptr<HistoryChange>,
                            std::unique_ptr<NativeTransaction>&);
    MmsResult CommitNative(std::unique_ptr<NativeTransaction>);
    // Project boundaries invalidate prepared transactions without reusing a
    // revision. Retired resources are reclaimed outside the no-fail publication.
    MmsResult Reset();
    void Collect() noexcept;
    size_t UndoCount() const noexcept { return undo_.size(); }
    size_t RedoCount() const noexcept { return redo_.size(); }
    uint64_t RetainedBytes() const noexcept { return bytes_; }
    const std::string* NextLabel(bool redo) const noexcept;
};
class SharedHistory::NativeTransaction {
    friend class SharedHistory;
    SharedHistory* owner_;
    uint64_t revision_;
    std::unique_ptr<Publication> publication_;
    NativeTransaction(SharedHistory&, std::unique_ptr<Publication>);
public:
    ~NativeTransaction();
    NativeTransaction(const NativeTransaction&) = delete;
    NativeTransaction& operator=(const NativeTransaction&) = delete;
};
}
