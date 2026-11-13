#pragma once

// Thin C++17 access to mms.edit. All operations must run on the UI thread
// that queried the service, at a host edit-safe boundary. The host context,
// participant callbacks and participant data must outlive their use.
// This wrapper does not introduce another history or transaction engine.
#include <mms/edit.h>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace mms::sdk {
namespace edit_detail {
template<class F> MmsResult Call(F&& f) noexcept {
    try { return f(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
inline bool Complete(const MmsEditApiV1& api) noexcept {
    return api.begin && api.prepare && api.join && api.commit && api.cancel && api.undo && api.redo;
}
}

class EditServices;
class EditTransaction {
    MmsEditApiV1 api_{};
    MmsHandle handle_ = 0;
    std::thread::id thread_{};
    friend class EditServices;
    bool OnThread() const noexcept { return thread_ == std::this_thread::get_id(); }
public:
    EditTransaction() noexcept = default;
    EditTransaction(const EditTransaction&) = delete;
    EditTransaction& operator=(const EditTransaction&) = delete;
    EditTransaction(EditTransaction&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_,0)), thread_(other.thread_) { other.api_ = {}; }
    EditTransaction& operator=(EditTransaction&&) = delete;
    // No destructor cancellation: the C cancel entry cannot report whether
    // the host accepted a call made at a forbidden thread or boundary.
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    const MmsEditApiV1& api() const noexcept { return api_; }
    MmsResult Join(const MmsTransactionParticipantV1& participant) noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        if (!OnThread()) return MMS_E_THREAD;
        return edit_detail::Call([&] { return api_.join(api_.context,handle_,&participant); });
    }
    MmsResult Prepare() noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        if (!OnThread()) return MMS_E_THREAD;
        // On failure the host may already have consumed the transaction. Keep
        // the original handle so the caller can request cancellation safely.
        return edit_detail::Call([&] { return api_.prepare(api_.context,handle_); });
    }
    MmsResult Commit() noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        if (!OnThread()) return MMS_E_THREAD;
        const auto result=edit_detail::Call([&] { return api_.commit(api_.context,handle_); });
        if (result==MMS_OK) { handle_=0; api_={}; thread_={}; }
        return result;
    }
    // Merely issues the host's void cancellation request. The return value
    // reports local dispatch only, not that cancellation was accepted. Keep
    // the handle until the caller knows it ran at an edit-safe boundary.
    MmsResult RequestCancel() noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        if (!OnThread()) return MMS_E_THREAD;
        return edit_detail::Call([&]() -> MmsResult { api_.cancel(api_.context,handle_); return MMS_OK; });
    }
    // Transfer/forget the handle only after the caller has handled its host
    // lifecycle. Copy api() first if responsibility transfers elsewhere.
    MmsHandle detach() noexcept { api_={}; thread_={}; return std::exchange(handle_,0); }
};

class EditServices {
    MmsEditApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    static MmsResult Query(const MmsHostV1& host, EditServices& out) noexcept {
        if (host.h.size<sizeof(host) || host.h.major!=1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return edit_detail::Call([&]() -> MmsResult {
            MmsEditApiV1 api{};
            const auto result=host.queryService(host.context,{"mms.edit",8},1,0,sizeof(api),&api);
            if (result!=MMS_OK) return result;
            if (api.h.size<sizeof(api) || api.h.major!=1) return MMS_E_VERSION;
            if (!edit_detail::Complete(api)) return MMS_E_UNSUPPORTED;
            EditServices value; value.api_=api; value.thread_=std::this_thread::get_id(); value.ready_=true;
            out=value; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsEditApiV1& api() const noexcept { return api_; }
    MmsResult Begin(std::string_view label,std::uint64_t revision,EditTransaction& out) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (thread_!=std::this_thread::get_id()) return MMS_E_THREAD;
        if (out) return MMS_E_BUSY;
        MmsHandle candidate=0;
        const auto result=edit_detail::Call([&] { return api_.begin(api_.context,{label.data(),static_cast<std::uint64_t>(label.size())},revision,&candidate); });
        if (result!=MMS_OK) return result;
        if (!candidate) return MMS_E_FAILED;
        out.api_=api_; out.handle_=candidate; out.thread_=thread_; return MMS_OK;
    }
    MmsResult Undo() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (thread_!=std::this_thread::get_id()) return MMS_E_THREAD;
        return edit_detail::Call([&] { return api_.undo(api_.context); });
    }
    MmsResult Redo() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (thread_!=std::this_thread::get_id()) return MMS_E_THREAD;
        return edit_detail::Call([&] { return api_.redo(api_.context); });
    }
};
} // namespace mms::sdk
