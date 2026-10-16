#include "shared_history.hpp"
#include <limits>

namespace mikudancestudio::plugins {
struct SharedHistory::Node {
    std::shared_ptr<const EditRecord> plugins;
    std::shared_ptr<HistoryChange> native;
    uint64_t bytes;
};
struct SharedHistory::Publication final : EditPublication {
    SharedHistory& owner;
    uint64_t revision;
    Nodes node;
    std::unique_ptr<PreparedHistoryChange> candidate;
    explicit Publication(SharedHistory& h) : owner(h), revision(h.revision_) {}
    bool IsCurrent() const noexcept override {
        return owner.revision_ == revision && !node.empty() && (!candidate || candidate->IsCurrent());
    }
    void Publish() noexcept override {
        if (node.empty()) return;
        if (candidate) candidate->Publish();
        owner.Publish(*this);
    }
};
namespace {
struct Unlock { SharedHistory& h; ~Unlock() { h.Leave(); } };
struct CancelRestore { StorageStore& s; ~CancelRestore() { s.Cancel(); } };
uint64_t RecordBytes(const EditRecord& record) {
    uint64_t bytes = sizeof(EditRecord) + record.label.size();
    for (const auto* records : {&record.before, &record.after}) {
        if (records->size() > 4096) throw std::invalid_argument("history provider limit");
        for (const auto& [id, value] : *records) {
            if (!ValidId(id) || !value.schema || value.bytes.size() > 64ull * 1024 * 1024)
                throw std::invalid_argument("history provider data");
            bytes += sizeof(StoredData) + id.size() + value.bytes.size();
        }
    }
    if (record.label.empty() || record.before.size() != record.after.size())
        throw std::invalid_argument("history record");
    for (const auto& [id, value] : record.before)
        if (!record.after.count(id)) throw std::invalid_argument("history participant mismatch");
    return bytes;
}
}
SharedHistory::SharedHistory(StorageStore& storage, std::function<MmsResult()> gate,
                             size_t entries, uint64_t bytes)
    : storage_(storage), gate_(std::move(gate)), maxBytes_(bytes), maxEntries_(entries) {
    if (!entries || !bytes) throw std::invalid_argument("history limits");
}
SharedHistory::~SharedHistory() = default;
MmsResult SharedHistory::Ready() const {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (poisoned_ || storage_.Poisoned() || revision_ == std::numeric_limits<uint64_t>::max()) return MMS_E_NOT_READY;
    return MMS_OK;
}
MmsResult SharedHistory::Revision(uint64_t& out) {
    if (auto r = Ready()) return r;
    out = revision_; return MMS_OK;
}
MmsResult SharedHistory::Enter() {
    if (auto r = Ready()) return r;
    if (entered_ || storage_.InCallback() || storage_.Prepared()) return MMS_E_BUSY;
    // Block reentry from the gate as well as from native/plugin callbacks.
    entered_ = true;
    auto result = Boundary([&] { return gate_ ? gate_() : MMS_OK; });
    if (result) entered_ = false;
    return result;
}
void SharedHistory::Leave() noexcept { entered_ = false; }
MmsResult SharedHistory::ReserveNode(std::shared_ptr<const EditRecord> record,
    std::shared_ptr<HistoryChange> native, std::unique_ptr<Publication>& out) {
    if (!entered_) return MMS_E_BUSY;
    if (!record || (!native && record->before.empty())) return MMS_E_ARGUMENT;
    const auto pluginBytes = RecordBytes(*record), nativeBytes = native ? native->RetainedBytes() : 0;
    if (pluginBytes > maxBytes_ || nativeBytes > maxBytes_ - pluginBytes) return MMS_E_MEMORY;
    // Copy the record: a caller retaining a mutable shared_ptr cannot alter a
    // published history node after preparation.
    auto copy = std::make_shared<const EditRecord>(*record);
    auto publication = std::make_unique<Publication>(*this);
    publication->node.push_back(std::make_shared<Node>(Node{std::move(copy), std::move(native), pluginBytes + nativeBytes}));
    if (const auto& change = publication->node.front()->native) {
        if (auto r = change->Prepare(true, publication->candidate)) return r;
        if (!publication->candidate) return MMS_E_FAILED;
    }
    out = std::move(publication); return MMS_OK;
}
MmsResult SharedHistory::Reserve(std::shared_ptr<const EditRecord> record,
    std::unique_ptr<EditPublication>& out) {
    if (auto r = Ready()) return r;
    return Boundary([&] {
        std::unique_ptr<Publication> candidate;
        auto native = record ? record->native : nullptr;
        auto result = ReserveNode(std::move(record), std::move(native), candidate);
        if (!result) out = std::move(candidate);
        return result;
    });
}
void SharedHistory::Publish(Publication& p) noexcept {
    for (const auto& node : redo_) bytes_ -= node->bytes;
    retired_.splice(retired_.end(), redo_);
    const auto newBytes = p.node.front()->bytes;
    // Evict before adding to avoid unsigned overflow at a caller's large cap.
    while (!undo_.empty() && (undo_.size() >= maxEntries_ || bytes_ > maxBytes_ - newBytes)) {
        bytes_ -= undo_.front()->bytes;
        retired_.splice(retired_.end(), undo_, undo_.begin());
    }
    bytes_ += newBytes;
    undo_.splice(undo_.end(), p.node); ++revision_;
}
MmsResult SharedHistory::History(bool redo) {
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    return Boundary([&]() -> MmsResult {
        auto& source = redo ? redo_ : undo_;
        if (source.empty()) return MMS_E_NOT_FOUND;
        const auto& node = *source.back();
        std::unique_ptr<PreparedHistoryChange> native;
        if (node.native) {
            if (auto r = node.native->Prepare(redo, native)) return r;
            if (!native) return MMS_E_FAILED;
        }
        if (auto r = storage_.PrepareRestore(redo ? node.plugins->after : node.plugins->before)) return r;
        CancelRestore cancel{storage_};
        // Plugin prepareLoad runs before the final native validation. A stale
        // native candidate cancels all plugin candidates, leaving the cursor.
        if (native && !native->IsCurrent()) return MMS_E_STALE;
        if (!storage_.Commit()) { poisoned_ = true; return MMS_E_FAILED; }
        if (native) native->Publish();
        auto& destination = redo ? undo_ : redo_;
        destination.splice(destination.end(), source, std::prev(source.end()));
        ++revision_; return MMS_OK;
    });
}
SharedHistory::NativeTransaction::NativeTransaction(SharedHistory& owner,
    std::unique_ptr<Publication> p)
    : owner_(&owner), revision_(owner.revision_), publication_(std::move(p)) {}
SharedHistory::NativeTransaction::~NativeTransaction() = default;
MmsResult SharedHistory::PrepareNative(std::string label, std::shared_ptr<HistoryChange> change,
    std::unique_ptr<NativeTransaction>& out) {
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    return Boundary([&]() -> MmsResult {
        if (!change) return MMS_E_ARGUMENT;
        auto record = std::make_shared<EditRecord>(); record->label = std::move(label);
        std::unique_ptr<Publication> publication;
        if (auto r = ReserveNode(std::move(record), change, publication)) return r;
        out.reset(new NativeTransaction(*this, std::move(publication)));
        return MMS_OK;
    });
}
MmsResult SharedHistory::CommitNative(std::unique_ptr<NativeTransaction> t) {
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    if (!t || t->owner_ != this) return MMS_E_ARGUMENT;
    if (t->revision_ != revision_ || !t->publication_->IsCurrent()) return MMS_E_STALE;
    t->publication_->Publish();
    // Finalize retired physics contacts while Enter still excludes reentry.
    // Resource cleanup is after publication, never part of Publish itself.
    t.reset(); return MMS_OK;
}
MmsResult SharedHistory::Reset() {
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    retired_.splice(retired_.end(), undo_); retired_.splice(retired_.end(), redo_);
    bytes_ = 0; ++revision_; return MMS_OK;
}
void SharedHistory::Collect() noexcept {
    if (GetCurrentThreadId() != thread_ || entered_ || storage_.InCallback() || storage_.Prepared()) return;
    // Resource destructors are never called by publication. Guard callbacks
    // from destructors too; callers perform this at a safe idle point.
    entered_ = true; retired_.clear(); entered_ = false;
}
const std::string* SharedHistory::NextLabel(bool redo) const noexcept {
    const auto& nodes = redo ? redo_ : undo_;
    return nodes.empty() ? nullptr : &nodes.back()->plugins->label;
}
}
