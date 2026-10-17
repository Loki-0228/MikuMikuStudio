#pragma once
// C++17 convenience over the original mms.ui.refresh table and ticket handles.
// Service contexts must outlive all calls and tickets, including destructors.
#include <mms/cpp/ui.hpp>
#include <mms/refresh.h>

namespace mms::sdk {

class RefreshServices;

class RefreshTicket {
    MmsUiRefreshApiV1 api_{};
    MmsHandle handle_ = 0;
    friend class RefreshServices;
public:
    RefreshTicket() noexcept = default;
    RefreshTicket(const RefreshTicket&) = delete;
    RefreshTicket& operator=(const RefreshTicket&) = delete;
    RefreshTicket(RefreshTicket&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_, 0)) { other.api_ = {}; }
    RefreshTicket& operator=(RefreshTicket&&) = delete;
    // Best effort. In stop(), Release() explicitly and retain this object on
    // failure. Releasing a pending ticket does NOT cancel its contribution.
    ~RefreshTicket() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    MmsResult Query(MmsUiRefreshStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiRefreshStateV1 candidate{}; candidate.h = Header<MmsUiRefreshStateV1>();
        const auto result = detail::Call([&] { return api_.query(api_.context, handle_, &candidate); });
        if (!result) out = candidate;
        return result;
    }
    MmsResult Cancel() const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        return detail::Call([&] { return api_.cancel(api_.context, handle_); });
    }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        const auto result = detail::Call([&] { return api_.release(api_.context, handle_); });
        if (result && result != MMS_E_NOT_FOUND) return result;
        handle_ = 0; api_ = {}; return MMS_OK;
    }
    MmsResult CancelAndRelease() noexcept {
        if (!handle_) return MMS_OK;
        const auto result = Cancel();
        if (result == MMS_E_NOT_FOUND) { handle_ = 0; api_ = {}; return MMS_OK; }
        if (result) return result;
        return Release();
    }
    // Transfer the SAME C handle to a lower-level owner. That owner must call
    // release through the original service; the SDK no longer touches it.
    MmsHandle detach() noexcept { api_ = {}; return std::exchange(handle_, 0); }
};

class RefreshServices {
    MmsUiRefreshApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    // Call on the host UI thread. Failed negotiation leaves out unchanged.
    static MmsResult Query(const MmsHostV1& host, RefreshServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiRefreshApiV1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.refresh"), 1, 0,
                static_cast<std::uint32_t>(sizeof(table)), &table);
            if (result) return result;
            if (!detail::TableHeader(table)) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.getTarget || !table.request ||
                !table.query || !table.cancel || !table.release) return MMS_E_UNSUPPORTED;
            RefreshServices candidate; candidate.api_ = table;
            candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
            out = candidate; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    const MmsUiRefreshApiV1& api() const noexcept { return api_; }
    MmsResult Capabilities(MmsUiRefreshCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiRefreshCapabilitiesV1 candidate{}; candidate.h = Header<MmsUiRefreshCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
        if (!result) out = candidate;
        return result;
    }
    MmsResult Target(MmsHandle element, MmsUiRefreshTargetV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiRefreshTargetV1 candidate{}; candidate.h = Header<MmsUiRefreshTargetV1>();
        const auto result = detail::Call([&] { return api_.getTarget(api_.context, element, &candidate); });
        if (!result) out = candidate;
        return result;
    }
    // These calls, and ticket operations, may run on a worker. Publish a copy
    // of this service first; do not mutate the same SDK object concurrently.
    MmsResult Request(const MmsUiRefreshRequestV1& request, RefreshTicket& out) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (out) return MMS_E_BUSY;
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.request(api_.context, &request, &handle); });
        if (result) return result;
        if (!handle) return MMS_E_FAILED;
        // No allocation can fail after the host has accepted the contribution.
        out.api_ = api_; out.handle_ = handle; return MMS_OK;
    }
    MmsResult RepaintRects(const MmsUiRefreshTargetV1& target, const MmsUiDirtyRectV1* rects,
        std::size_t count, RefreshTicket& out, std::uint64_t coalesceKey = 0) const noexcept {
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = target.target; request.kinds = MMS_UI_REFRESH_REPAINT;
        request.scope = MMS_UI_REFRESH_SELF; request.coalesceKey = coalesceKey;
        request.expectedGeometryRevision = count ? target.geometryRevision : 0;
        request.dirtyRects = {rects, static_cast<std::uint64_t>(count), sizeof(MmsUiDirtyRectV1), 1};
        return Request(request, out);
    }
    MmsResult Repaint(const MmsUiRefreshTargetV1& target, RefreshTicket& out,
        std::uint64_t coalesceKey = 0) const noexcept {
        return RepaintRects(target, nullptr, 0, out, coalesceKey);
    }
    MmsResult RefreshState(const MmsUiRefreshTargetV1& target, RefreshTicket& out,
        std::uint64_t coalesceKey = 0) const noexcept {
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = target.target; request.kinds = MMS_UI_REFRESH_STATE;
        request.scope = MMS_UI_REFRESH_SELF; request.expectedGeometryRevision = target.geometryRevision;
        request.coalesceKey = coalesceKey;
        return Request(request, out);
    }
    // UI-only convenience for a simple command callback. Worker callers must
    // use an already captured target with Repaint/RepaintRects instead.
    MmsResult RepaintElement(MmsHandle element, RefreshTicket& out,
        std::uint64_t coalesceKey = 0) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        MmsUiRefreshTargetV1 target{};
        if (const auto result = Target(element, target)) return result;
        return Repaint(target, out, coalesceKey);
    }
    MmsResult RepaintRect(const MmsUiRefreshTargetV1& target, const MmsUiDirtyRectV1& rect,
        RefreshTicket& out, std::uint64_t coalesceKey = 0) const noexcept {
        return RepaintRects(target, &rect, 1, out, coalesceKey);
    }
};

} // namespace mms::sdk
