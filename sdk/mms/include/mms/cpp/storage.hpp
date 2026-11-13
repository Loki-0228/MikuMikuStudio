#pragma once

// C++17 access to mms.storage 1.0. A saved-data lease reads the last published
// record, not live plugin state. It owns the original host handle and copies
// the C service table; no pointer to the StorageServices wrapper is retained.
// The host service context must outlive the lease. Release on the UI thread
// before completing plugin shutdown, after all users of bytes() have stopped.
#include <mms/storage.h>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mms::sdk {
namespace storage_detail {
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
} // namespace storage_detail

class StorageServices;
class SavedBytesLease {
    MmsStorageApiV1 api_{};
    MmsHandle handle_ = 0;
    MmsBytes bytes_{};
    uint32_t schema_ = 0;
    std::thread::id thread_{};
    bool busy_ = false;
    friend class StorageServices;
    void Clear() noexcept {
        api_ = {}; handle_ = 0; bytes_ = {}; schema_ = 0; thread_ = {};
    }
public:
    SavedBytesLease() noexcept = default;
    SavedBytesLease(const SavedBytesLease&) = delete;
    SavedBytesLease& operator=(const SavedBytesLease&) = delete;
    SavedBytesLease(SavedBytesLease&& other) noexcept {
        // A host call may reenter plugin code. Never transfer a lease whose
        // release is already executing; that call retains its original owner.
        if (other.busy_) return;
        api_ = other.api_; handle_ = other.handle_; bytes_ = other.bytes_;
        schema_ = other.schema_; thread_ = other.thread_; other.Clear();
    }
    SavedBytesLease& operator=(SavedBytesLease&&) = delete;
    // No implicit destructor release: releaseSaved has no error result and
    // silently ignores calls from the wrong thread in the current host.
    ~SavedBytesLease() = default;
    bool valid() const noexcept { return handle_ != 0; }
    explicit operator bool() const noexcept { return valid(); }
    MmsHandle get() const noexcept { return handle_; }
    MmsBytes bytes() const noexcept { return bytes_; }
    uint32_t schema() const noexcept { return schema_; }
    const MmsStorageApiV1& api() const noexcept { return api_; }
    // Transfer into an empty wrapper only. Existing leases require an explicit
    // Release, so replacing a wrapper cannot silently discard owned storage.
    MmsResult TransferFrom(SavedBytesLease& other) noexcept {
        if (busy_ || other.busy_) return MMS_E_BUSY;
        if (this == &other) return MMS_OK;
        if (valid()) return MMS_E_BUSY;
        api_ = other.api_; handle_ = other.handle_; bytes_ = other.bytes_;
        schema_ = other.schema_; thread_ = other.thread_; other.Clear();
        return MMS_OK;
    }
    MmsResult Release() noexcept {
        if (busy_) return MMS_E_BUSY;
        if (!valid()) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        if (!api_.releaseSaved) return MMS_E_UNSUPPORTED;
        busy_ = true;
        const auto result = storage_detail::Call([&]() -> MmsResult {
            api_.releaseSaved(api_.context, handle_); return MMS_OK;
        });
        busy_ = false;
        if (result == MMS_OK) Clear();
        return result;
    }
    // Copy api(), bytes() and schema() before detaching if responsibility is
    // passed to C code. Zero while a Release call is in progress changes nothing.
    MmsHandle Detach() noexcept {
        if (busy_) return 0;
        const auto handle = handle_; Clear(); return handle;
    }
};

class StorageServices {
    MmsStorageApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    static MmsResult Query(const MmsHostV1& host, StorageServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return storage_detail::Call([&]() -> MmsResult {
            MmsStorageApiV1 candidate{};
            const auto result = host.queryService(host.context, {"mms.storage",11},
                1, 0, sizeof(candidate), &candidate);
            if (result != MMS_OK) return result;
            if (candidate.h.size < sizeof(candidate) || candidate.h.major != 1) return MMS_E_VERSION;
            if (!candidate.readSaved || !candidate.releaseSaved) return MMS_E_UNSUPPORTED;
            StorageServices value; value.api_ = candidate;
            value.thread_ = std::this_thread::get_id(); value.ready_ = true;
            out = value; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsStorageApiV1& api() const noexcept { return api_; }
    MmsResult ReadSaved(SavedBytesLease& out) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        if (out.valid() || out.busy_) return MMS_E_BUSY;
        // C callbacks must not publish outputs on failure. Local candidates
        // keep the caller unchanged even if a failing callback writes them.
        const auto api = api_; const auto thread = thread_;
        MmsBytes bytes{}; uint32_t schema = 0; MmsHandle handle = 0;
        out.busy_ = true;
        const auto result = storage_detail::Call([&] {
            return api.readSaved(api.context, &bytes, &schema, &handle);
        });
        out.busy_ = false;
        if (result != MMS_OK) return result;
        if (!handle) return MMS_E_FAILED;
        out.api_ = api; out.handle_ = handle; out.bytes_ = bytes;
        out.schema_ = schema; out.thread_ = thread;
        return MMS_OK;
    }
};
} // namespace mms::sdk
