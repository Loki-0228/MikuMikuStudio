#include "storage_store.hpp"

namespace mikudancestudio::plugins {
namespace {
// A bounded public transfer. The encoded sidecar imposes its own total limit.
constexpr uint64_t kMaxBytes = 64ull * 1024 * 1024;
struct Busy { bool& flag; explicit Busy(bool& b) : flag(b) { flag = true; } ~Busy() { flag = false; } };
StoredData Copy(MmsBytes bytes, uint32_t schema) {
    if (!schema || bytes.bytes > kMaxBytes || (bytes.bytes && !bytes.data)) throw std::invalid_argument("serializer bytes/schema");
    StoredData out; out.schema = schema;
    if (bytes.bytes) { const auto* p = static_cast<const unsigned char*>(bytes.data); out.bytes.assign(p, p + size_t(bytes.bytes)); }
    return out;
}
}
MmsResult StorageStore::Ready() const {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (poisoned_) return MMS_E_NOT_READY;
    return busy_ || prepared_ ? MMS_E_BUSY : MMS_OK;
}
bool StorageStore::CanRestore(const std::string& id) const {
    return RestoreToken(id)!=0;
}
MmsHandle StorageStore::RestoreToken(const std::string& id) const {
    if (GetCurrentThreadId()!=thread_ || poisoned_) return 0;
    for(const auto& [h,p]:providers_)if(p.owner->id==id)return p.owner->stopping?0:h;
    return 0;
}
MmsStorageApiV1 StorageStore::Api(StorageClient& client) {
    client.store = this; MmsStorageApiV1 api{Header<MmsStorageApiV1>(), &client};
    api.readSaved = [](void* context, MmsBytes* bytes, uint32_t* schema, MmsHandle* lease) {
        return Boundary([&]() -> MmsResult {
            auto& c = *static_cast<StorageClient*>(context); auto& s = *c.store;
            if (GetCurrentThreadId() != s.thread_) return MMS_E_THREAD;
            if (c.stopping) return MMS_E_NOT_READY;
            if (!bytes || !schema || !lease) return MMS_E_ARGUMENT;
            auto it = s.records_.find(c.id); if (it == s.records_.end()) return MMS_E_NOT_FOUND;
            if (s.leases_.size() >= 4096) return MMS_E_BUSY;
            auto h = s.next_++; auto& value = s.leases_.emplace(h, Lease{&c, it->second}).first->second.data;
            *bytes = {value.bytes.data(), value.bytes.size()}; *schema = value.schema; *lease = h; return MMS_OK;
        });
    };
    api.releaseSaved = [](void* context, MmsHandle lease) {
        auto& c = *static_cast<StorageClient*>(context); auto& s = *c.store;
        if (GetCurrentThreadId() != s.thread_) return;
        auto it = s.leases_.find(lease); if (it != s.leases_.end() && it->second.owner == &c) s.leases_.erase(it);
    };
    return api;
}
MmsResult StorageStore::Register(StorageClient& client, MmsString name, const void* data, uint32_t bytes, MmsHandle* out) {
    if (auto r = Ready()) return r;
    if (client.stopping) return MMS_E_NOT_READY;
    if (!out || !data || bytes < sizeof(MmsSerializerV1)) return MMS_E_ARGUMENT;
    const auto& table = *static_cast<const MmsSerializerV1*>(data);
    if (!Valid(&table) || table.h.size > bytes || !table.prepareLoad || !table.commitLoad || !table.cancelLoad || !table.save || !table.releaseBytes || String(name) != client.id || !ValidId(client.id)) return MMS_E_ARGUMENT;
    for (auto& [h, p] : providers_) if (p.owner->id == client.id) return MMS_E_CONFLICT;
    // Restore retained state before accepting a late provider. Allocate its map
    // node before the irreversible commit; a failure leaves the data retained.
    const auto h = next_++; auto inserted = providers_.emplace(h, Provider{&client, table});
    Busy guard(busy_); const auto found = records_.find(client.id); MmsHandle candidate = 0;
    const StoredData empty{}; const auto& saved = found == records_.end() ? empty : found->second;
    auto r = Boundary([&] { return table.prepareLoad(table.context, {saved.bytes.data(),saved.bytes.size()}, saved.schema, &candidate); });
    if (r == MMS_OK) {
        r = Boundary([&] { table.commitLoad(table.context, candidate); return MMS_OK; });
        if (r) poisoned_ = true; // A void commit that throws violates the ABI.
    }
    if (r) { providers_.erase(inserted.first); return r; }
    *out = h; return MMS_OK;
}
MmsResult StorageStore::Unregister(StorageClient& client, MmsHandle h) {
    if (auto r = Ready()) return r;
    auto it = providers_.find(h); if (it == providers_.end() || it->second.owner != &client) return MMS_E_NOT_FOUND;
    // Preserve the last published bytes; do not call a stopping provider.
    providers_.erase(it); return MMS_OK;
}
bool StorageStore::HasLeases(StorageClient& c) const {
    for (const auto& [h, l] : leases_) if (l.owner == &c) return true;
    return false;
}
bool StorageStore::Retire(StorageClient& c) {
    if (Ready() != MMS_OK || HasLeases(c)) return false;
    for (auto it = providers_.begin(); it != providers_.end();) if (it->second.owner == &c) it = providers_.erase(it); else ++it;
    return true;
}
MmsResult StorageStore::Capture(StoredRecords& out) {
    if (auto r = Ready()) return r;
    Busy guard(busy_);
    return Boundary([&]() -> MmsResult {
        auto result = records_;
        for (const auto& [h, p] : providers_) {
            if (p.owner->stopping) return MMS_E_NOT_READY;
            MmsBytes bytes{}; uint32_t schema = 0; MmsHandle lease = 0;
            auto r = Boundary([&] { return p.table.save(p.table.context, &bytes, &schema, &lease); });
            if (r) return r;
            // Once save succeeds release exactly once, even if copying throws.
            const auto copied = Boundary([&] { result[p.owner->id] = Copy(bytes, schema); return MMS_OK; });
            const auto released = Boundary([&] { p.table.releaseBytes(p.table.context, lease); return MMS_OK; });
            if (released) { poisoned_ = true; return released; }
            if (copied) return copied;
        }
        out.swap(result); return MMS_OK;
    });
}
void StorageStore::CancelCandidates() noexcept {
    for (auto it = candidates_.rbegin(); it != candidates_.rend(); ++it)
        if (Boundary([&] { it->table.cancelLoad(it->table.context, it->value); return MMS_OK; })) poisoned_ = true;
    candidates_.clear(); pending_.clear(); prepared_ = false;
}
MmsResult StorageStore::Prepare(StoredRecords data) { return PrepareImpl(std::move(data),false); }
MmsResult StorageStore::PrepareRestore(StoredRecords data) { return PrepareImpl(std::move(data),true); }
MmsResult StorageStore::PrepareImpl(StoredRecords data, bool restoreOnly) {
    if (auto r = Ready()) return r;
    Busy guard(busy_);
    const auto r = Boundary([&]() -> MmsResult {
        if(restoreOnly)for(const auto& [id,record]:data)if(!CanRestore(id))return MMS_E_NOT_READY;
        candidates_.reserve(providers_.size()); pending_ = std::move(data); restoreOnly_=restoreOnly;
        for (const auto& [h, p] : providers_) {
            if(restoreOnly && !pending_.count(p.owner->id))continue;
            if (p.owner->stopping) return MMS_E_NOT_READY;
            const auto it = pending_.find(p.owner->id); const StoredData empty{};
            const auto& saved = it == pending_.end() ? empty : it->second;
            MmsHandle candidate = 0;
            auto r = Boundary([&] { return p.table.prepareLoad(p.table.context, {saved.bytes.data(),saved.bytes.size()}, saved.schema, &candidate); });
            if (r) return r;
            candidates_.push_back({p.table, candidate});
        }
        prepared_ = true; return MMS_OK;
    });
    if (r) CancelCandidates(); return r;
}
bool StorageStore::Commit() noexcept {
    if (GetCurrentThreadId() != thread_ || busy_ || !prepared_) return false;
    Busy guard(busy_); bool ok = true;
    for (auto& c : candidates_) if (Boundary([&] { c.table.commitLoad(c.table.context, c.value); return MMS_OK; })) { poisoned_ = true; ok = false; }
    candidates_.clear(); if(!restoreOnly_)records_.swap(pending_); pending_.clear(); prepared_ = false; return ok;
}
void StorageStore::Cancel() noexcept {
    if (GetCurrentThreadId() != thread_ || busy_) return;
    Busy guard(busy_); CancelCandidates();
}
}
