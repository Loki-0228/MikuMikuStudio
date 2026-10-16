#pragma once
#include "common.hpp"
#include <mms/storage.h>
#include <map>
#include <atomic>

namespace mikudancestudio::plugins {
struct StoredData { uint32_t schema = 0; std::vector<unsigned char> bytes; };
using StoredRecords = std::map<std::string, StoredData>;
class StorageStore;
struct StorageClient { StorageStore* store = nullptr; std::string id; std::atomic<bool> stopping{false}; };
// All operations and callbacks are serialized on the creating UI thread.
class StorageStore {
    struct Provider { StorageClient* owner; MmsSerializerV1 table; };
    struct Lease { StorageClient* owner; StoredData data; };
    struct Candidate { MmsSerializerV1 table; MmsHandle value; };
    DWORD thread_ = GetCurrentThreadId();
    MmsHandle next_ = 1;
    bool busy_ = false, poisoned_ = false, prepared_ = false;
    StoredRecords records_, pending_;
    std::map<MmsHandle, Provider> providers_;
    std::map<MmsHandle, Lease> leases_;
    std::vector<Candidate> candidates_;
    MmsResult Ready() const;
    void CancelCandidates() noexcept;
public:
    MmsStorageApiV1 Api(StorageClient&);
    MmsResult Register(StorageClient&, MmsString id, const void*, uint32_t, MmsHandle*);
    MmsResult Unregister(StorageClient&, MmsHandle);
    bool HasLeases(StorageClient&) const;
    bool Retire(StorageClient&);
    bool InCallback() const { return busy_; }
    bool Prepared() const { return prepared_; }
    const StoredRecords& Records() const { return records_; }
    MmsResult Capture(StoredRecords&);
    MmsResult Prepare(StoredRecords);
    bool Commit() noexcept;
    void Cancel() noexcept;
    // The caller has already durably published the file pair. No allocation.
    void Saved(StoredRecords& data) noexcept { records_.swap(data); }
};
}
