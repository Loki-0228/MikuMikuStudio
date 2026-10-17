#include "shared_history.hpp"
#include <limits>

namespace mikudancestudio::plugins {
struct SharedHistory::Node {
    std::shared_ptr<const EditRecord> plugins;
    std::shared_ptr<HistoryChange> native;
    uint64_t bytes;
    uint64_t pluginBytes;
};
struct SharedHistory::Publication final : EditPublication {
    SharedHistory& owner;
    uint64_t revision;
    Nodes node;
    std::unique_ptr<PreparedHistoryChange> candidate;
    bool record = true;
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
SharedHistory::~SharedHistory() {
    entered_ = true;
    InvalidatePending();
    if (applied_) {
        applied_->Release(); applied_->owner_ = nullptr; applied_ = nullptr;
    }
    if (reset_) { reset_->owner_ = nullptr; reset_ = nullptr; }
}
void SharedHistory::AttachStore(EditStore* store) {
    if (!store || edits_ || entered_ || GetCurrentThreadId()!=thread_)
        throw std::invalid_argument("history edit store ownership");
    edits_ = store;
}
void SharedHistory::DetachStore(EditStore* store) noexcept {
    if (edits_ == store) edits_ = nullptr;
}
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
MmsResult SharedHistory::CheckNativeEdit() {
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    Leave(); return MMS_OK;
}
MmsResult SharedHistory::SetMode(RecordingMode mode) {
    return Configure(mode,maxEntries_,maxBytes_);
}
MmsResult SharedHistory::Configure(RecordingMode mode,size_t entries,uint64_t bytes,const std::function<MmsResult()>& persist) {
    if (mode != RecordingMode::AllOperations && mode != RecordingMode::ExplicitOnly) return MMS_E_ARGUMENT;
    if (!entries || !bytes) return MMS_E_ARGUMENT;
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (pending_ || applied_ || reset_) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    if (persist) { const auto result=Boundary(persist); if(result)return result; }
    recordingMode_ = mode;
    maxEntries_=entries;maxBytes_=bytes;
    while(!undo_.empty() && (undo_.size()+redo_.size()>maxEntries_ || bytes_>maxBytes_)) {
        bytes_-=undo_.front()->bytes;retired_.splice(retired_.end(),undo_,undo_.begin());
    }
    while(!redo_.empty() && (undo_.size()+redo_.size()>maxEntries_ || bytes_>maxBytes_)) {
        bytes_-=redo_.front()->bytes;retired_.splice(retired_.end(),redo_,redo_.begin());
    }
    return MMS_OK;
}
MmsResult SharedHistory::ReserveNode(std::shared_ptr<const EditRecord> record,
    std::shared_ptr<HistoryChange> native, std::unique_ptr<Publication>& out, bool retain) {
    if (!entered_) return MMS_E_BUSY;
    if (!record || (!native && record->before.empty())) return MMS_E_ARGUMENT;
    const auto pluginBytes = RecordBytes(*record);
    if (retain && pluginBytes > maxBytes_) return MMS_E_MEMORY;
    // Copy the record: a caller retaining a mutable shared_ptr cannot alter a
    // published history node after preparation.
    auto copy = std::make_shared<const EditRecord>(*record);
    auto publication = std::make_unique<Publication>(*this);
    publication->record=retain;
    publication->node.push_back(std::make_shared<Node>(Node{std::move(copy), std::move(native), pluginBytes, pluginBytes}));
    if (const auto& change = publication->node.front()->native) {
        if (auto r = change->Prepare(true, publication->candidate)) return r;
        if (!publication->candidate) return MMS_E_FAILED;
        uint64_t target = change->RetainedBytes();
        uint64_t dynamicTarget = 0;
        if (publication->candidate->TargetRetainedBytes(dynamicTarget)) target = dynamicTarget;
        if (retain && target > maxBytes_ - pluginBytes) return MMS_E_MEMORY;
        publication->node.front()->bytes = retain ? pluginBytes + target : 0;
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
    // Preview still changes the scene revision, so old candidates cannot
    // publish against it. The journal and redo remain untouched.
    if (!p.record) { ++revision_; return; }
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
    if (auto r = Ready()) return r;
    // EditStore::History holds its callback guard while forwarding here. Only
    // its one-shot request may pass: cancel/resource callbacks also run with
    // that guard and may already have removed their pending transaction.
    if (edits_ && edits_->HasPending()) return MMS_E_BUSY;
    if (edits_ && edits_->InCallback() && !edits_->ConsumeHistoryRequest()) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    return Boundary([&]() -> MmsResult {
        auto& source = redo ? redo_ : undo_;
        if (source.empty()) return MMS_E_NOT_FOUND;
        auto& node = *source.back();
        std::unique_ptr<PreparedHistoryChange> native;
        if (node.native) {
            if (auto r = node.native->Prepare(redo, native)) return r;
            if (!native) return MMS_E_FAILED;
        }
        auto targetBytes = node.bytes;
        uint64_t targetNative = 0;
        if (native && native->TargetRetainedBytes(targetNative)) {
            if (targetNative > maxBytes_ - node.pluginBytes) return MMS_E_MEMORY;
            targetBytes = node.pluginBytes + targetNative;
            // Undo/redo must not silently discard a different history branch
            // to pay for a grown resource snapshot. Reject before any restore.
            if (bytes_ - node.bytes > maxBytes_ - targetBytes) return MMS_E_MEMORY;
        }
        if (auto r = storage_.PrepareRestore(redo ? node.plugins->after : node.plugins->before)) return r;
        CancelRestore cancel{storage_};
        // Plugin prepareLoad runs before the final native validation. A stale
        // native candidate cancels all plugin candidates, leaving the cursor.
        if (native && !native->IsCurrent()) return MMS_E_STALE;
        if (!storage_.Commit()) { poisoned_ = true; return MMS_E_FAILED; }
        if (native) native->Publish();
        bytes_ = bytes_ - node.bytes + targetBytes;
        node.bytes = targetBytes;
        auto& destination = redo ? undo_ : redo_;
        destination.splice(destination.end(), source, std::prev(source.end()));
        ++revision_; return MMS_OK;
    });
}
SharedHistory::NativeTransaction::NativeTransaction(SharedHistory& owner,
    std::unique_ptr<Publication> p)
    : owner_(&owner), next_(owner.pending_), revision_(owner.revision_), publication_(std::move(p)) {
    if (next_) next_->previous_ = this;
    owner.pending_ = this;
}
SharedHistory::NativeTransaction::~NativeTransaction() {
    if (!owner_) return;
    if (previous_) previous_->next_ = next_; else owner_->pending_ = next_;
    if (next_) next_->previous_ = previous_;
    // Cancellation and a rejected by-value CommitNative can destroy this
    // token after the caller's Enter scope has ended. Keep native/resource
    // cleanup inside the same reentry exclusion as a successful commit.
    const bool entered = owner_->entered_;
    owner_->entered_ = true;
    publication_.reset();
    owner_->entered_ = entered;
}
void SharedHistory::InvalidatePending() noexcept {
    while (pending_) {
        auto* transaction = pending_;
        pending_ = transaction->next_;
        if (pending_) pending_->previous_ = nullptr;
        transaction->owner_ = nullptr;
        transaction->previous_ = transaction->next_ = nullptr;
        // Release candidates too: they can retain physics caches and objects
        // even when the caller keeps its cancelled token across a scene reset.
        transaction->publication_.reset();
    }
}
MmsResult SharedHistory::PrepareNative(std::string label, std::shared_ptr<HistoryChange> change,
    std::unique_ptr<NativeTransaction>& out, NativeIntent intent) {
    if (intent != NativeIntent::ExplicitCommit && intent != NativeIntent::Adjustment) return MMS_E_ARGUMENT;
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    return Boundary([&]() -> MmsResult {
        if (!change) return MMS_E_ARGUMENT;
        auto record = std::make_shared<EditRecord>(); record->label = std::move(label);
        std::unique_ptr<Publication> publication;
        if (auto r = ReserveNode(std::move(record), change, publication,
            intent == NativeIntent::ExplicitCommit || recordingMode_ == RecordingMode::AllOperations)) return r;
        out.reset(new NativeTransaction(*this, std::move(publication)));
        return MMS_OK;
    });
}
MmsResult SharedHistory::CommitNative(std::unique_ptr<NativeTransaction> t) {
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    if (!t) return MMS_E_ARGUMENT;
    if (!t->owner_) return MMS_E_STALE;
    if (t->owner_ != this) return MMS_E_ARGUMENT;
    if (t->revision_ != revision_ || !t->publication_->IsCurrent()) return MMS_E_STALE;
    t->publication_->Publish();
    // Finalize retired physics contacts while Enter still excludes reentry.
    // Resource cleanup is after publication, never part of Publish itself.
    t.reset(); return MMS_OK;
}
SharedHistory::AppliedTransaction::AppliedTransaction(SharedHistory& owner, uint64_t maximum,
    std::shared_ptr<Node> node, std::unique_ptr<Publication> publication)
    : owner_(&owner), maximumNativeBytes_(maximum), node_(std::move(node)), publication_(std::move(publication)) {
    owner.applied_ = this;
}
void SharedHistory::AppliedTransaction::Release() noexcept {
    // Candidates may refer to their HistoryChange. Release them first, while
    // the gate still blocks reentry from resource destruction.
    busy_ = true;
    verification_.reset(); publication_.reset(); node_.reset();
}
SharedHistory::AppliedTransaction::~AppliedTransaction() {
    Release();
    if (owner_) { owner_->applied_ = nullptr; owner_->Leave(); }
}
MmsResult SharedHistory::ReserveAppliedNative(std::string label, uint64_t maximumNativeBytes,
    std::unique_ptr<AppliedTransaction>& out, NativeIntent intent) {
    if (intent != NativeIntent::ExplicitCommit && intent != NativeIntent::Adjustment) return MMS_E_ARGUMENT;
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    const auto result = Boundary([&]() -> MmsResult {
        if (out) return MMS_E_ARGUMENT;
        auto record = std::make_shared<EditRecord>(); record->label = std::move(label);
        const auto bytes = RecordBytes(*record);
        const bool retain=intent == NativeIntent::ExplicitCommit || recordingMode_ == RecordingMode::AllOperations;
        if (retain && (bytes > maxBytes_ || maximumNativeBytes > maxBytes_ - bytes)) return MMS_E_MEMORY;
        auto node = std::make_shared<Node>(Node{std::move(record), {}, bytes, bytes});
        auto publication = std::make_unique<Publication>(*this);
        publication->record = retain;
        publication->node.push_back(node);
        out.reset(new AppliedTransaction(*this, maximumNativeBytes, std::move(node), std::move(publication)));
        return MMS_OK;
    });
    if (result) Leave();
    return result;
}
MmsResult SharedHistory::PrepareAppliedNative(AppliedTransaction& t, std::shared_ptr<HistoryChange> change) {
    if (auto r = Ready()) return r;
    if (!t.owner_) return MMS_E_STALE;
    if (t.owner_ != this) return MMS_E_ARGUMENT;
    if (t.busy_ || storage_.InCallback() || storage_.Prepared()) return MMS_E_BUSY;
    if (t.committed_ || t.verification_) return MMS_E_CONFLICT;
    if (!change) return MMS_E_ARGUMENT;
    if (!entered_ || applied_ != &t || !t.publication_->IsCurrent()) return MMS_E_STALE;
    t.busy_ = true;
    const auto result = Boundary([&]() -> MmsResult {
        const auto bytes = change->RetainedBytes();
        if (bytes > t.maximumNativeBytes_) return MMS_E_MEMORY;
        std::unique_ptr<PreparedHistoryChange> verification;
        if (auto r = change->Prepare(false, verification)) return r;
        if (!verification) return MMS_E_FAILED;
        // This is an inverse verification, not the state being recorded.
        // Dynamic resource owners must use the normal prepared publication.
        uint64_t inverseBytes = 0;
        if (verification->TargetRetainedBytes(inverseBytes)) return MMS_E_NOT_READY;
        if (!verification->IsCurrent() || !t.publication_->IsCurrent() || change->RetainedBytes() != bytes)
            return MMS_E_STALE;
        t.nativeBytes_ = bytes; t.node_->bytes += bytes;
        t.node_->native = std::move(change); t.verification_ = std::move(verification);
        return MMS_OK;
    });
    t.busy_ = false;
    return result;
}
MmsResult SharedHistory::CommitAppliedNative(AppliedTransaction& t) noexcept {
    if (auto r = Ready()) return r;
    if (!t.owner_) return MMS_E_STALE;
    if (t.owner_ != this) return MMS_E_ARGUMENT;
    if (t.busy_ || storage_.InCallback() || storage_.Prepared()) return MMS_E_BUSY;
    if (t.committed_ || !t.verification_) return MMS_E_CONFLICT;
    if (!entered_ || applied_ != &t || !t.publication_->IsCurrent()) return MMS_E_STALE;
    t.busy_ = true;
    const bool current = t.verification_->IsCurrent() && t.node_->native->RetainedBytes() == t.nativeBytes_;
    if (current) {
        // The live state is already the after state. Never publish the inverse
        // verification candidate. This splice does not allocate or call users.
        Publish(*t.publication_); t.committed_ = true;
    }
    t.busy_ = false;
    return current ? MMS_OK : MMS_E_STALE;
}
MmsResult SharedHistory::Reset() {
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    Unlock unlock{*this};
    ResetLocked(); return MMS_OK;
}
void SharedHistory::ResetLocked() noexcept {
    InvalidatePending();
    retired_.splice(retired_.end(), undo_); retired_.splice(retired_.end(), redo_);
    bytes_ = 0; ++revision_;
}
SharedHistory::ResetTransaction::ResetTransaction(SharedHistory& owner) : owner_(&owner) {
    owner.reset_ = this;
}
SharedHistory::ResetTransaction::~ResetTransaction() {
    if (owner_) { owner_->reset_ = nullptr; owner_->Leave(); }
}
MmsResult SharedHistory::ResetTransaction::Commit() noexcept {
    if (!owner_) return MMS_E_STALE;
    if (GetCurrentThreadId() != owner_->thread_) return MMS_E_THREAD;
    if (committed_) return MMS_E_ARGUMENT;
    if (owner_->storage_.InCallback()) return MMS_E_BUSY;
    owner_->ResetLocked();
    owner_->retired_.clear();
    committed_ = true;
    return MMS_OK;
}
MmsResult SharedHistory::PrepareReset(std::unique_ptr<ResetTransaction>& out) {
    if (auto r = Ready()) return r;
    if (edits_ && (edits_->InCallback() || edits_->HasPending())) return MMS_E_BUSY;
    if (auto r = Enter()) return r;
    const auto result = Boundary([&] {
        out.reset(new ResetTransaction(*this)); return MMS_OK;
    });
    if (result) Leave();
    return result;
}
void SharedHistory::Collect() noexcept {
    if (GetCurrentThreadId() != thread_ || entered_ || storage_.InCallback() || storage_.Prepared()) return;
    if (edits_ && edits_->InCallback()) return;
    // Resource destructors are never called by publication. Guard callbacks
    // from destructors too; callers perform this at a safe idle point.
    entered_ = true; retired_.clear(); entered_ = false;
}
const std::string* SharedHistory::NextLabel(bool redo) const noexcept {
    const auto& nodes = redo ? redo_ : undo_;
    return nodes.empty() ? nullptr : &nodes.back()->plugins->label;
}
}
