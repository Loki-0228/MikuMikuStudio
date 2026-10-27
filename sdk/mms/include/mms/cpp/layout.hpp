#pragma once

// C++17 convenience wrapper for mms.ui.layout 1.0: default/staged layout hints
// and the immutable geometry snapshot lease. The wrapper keeps the host table
// and the original host handles; it owns no layout state.
#include <mms/layout.h>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mms::sdk {
namespace layout_detail {
inline MmsString Text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint64_t>(value.size())};
}
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
} // namespace layout_detail

class LayoutSnapshotLease {
    MmsUiLayoutApiV1 api_{};
    MmsHandle handle_ = 0;
public:
    LayoutSnapshotLease() = default;
    LayoutSnapshotLease(MmsUiLayoutApiV1 api, MmsHandle handle) noexcept : api_(api), handle_(handle) {}
    LayoutSnapshotLease(const LayoutSnapshotLease&) = delete;
    LayoutSnapshotLease& operator=(const LayoutSnapshotLease&) = delete;
    LayoutSnapshotLease(LayoutSnapshotLease&& other) noexcept : api_(other.api_), handle_(other.handle_) {
        other.handle_ = 0;
    }
    LayoutSnapshotLease& operator=(LayoutSnapshotLease&&) = delete;
    MmsResult TransferFrom(LayoutSnapshotLease& other) noexcept {
        if (this == &other) return MMS_OK;
        const auto result = Release();
        if (result != MMS_OK) return result;
        api_ = other.api_; handle_ = other.handle_; other.handle_ = 0;
        return MMS_OK;
    }
    ~LayoutSnapshotLease() = default; // explicit Release: no host call from a destructor
    bool valid() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (!api_.releaseSnapshot) return MMS_E_UNSUPPORTED;
        const MmsHandle handle = handle_;
        handle_ = 0;
        api_.releaseSnapshot(api_.context, handle);
        return MMS_OK;
    }
    MmsHandle Detach() noexcept { const MmsHandle handle = handle_; handle_ = 0; return handle; }
};

class LayoutServices {
    MmsHostV1 host_{};
    MmsUiLayoutApiV1 api_{};
    bool ready_ = false;
public:
    static MmsResult Query(const MmsHostV1& host, LayoutServices& out) noexcept {
        if (host.h.size < sizeof(MmsHostV1) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return layout_detail::Call([&]() -> MmsResult {
            MmsUiLayoutApiV1 candidate{};
            const auto result = host.queryService(host.context, layout_detail::Text("mms.ui.layout"),
                1, 0, static_cast<std::uint32_t>(sizeof(candidate)), &candidate);
            if (result != MMS_OK) return result;
            if (candidate.h.size < sizeof(candidate) || candidate.h.major != 1) return MMS_E_VERSION;
            if (!candidate.setDefaultHints || !candidate.stageHints ||
                !candidate.acquireSnapshot || !candidate.releaseSnapshot) return MMS_E_UNSUPPORTED;
            LayoutServices services;
            services.host_ = host;
            services.api_ = candidate;
            services.ready_ = true;
            out = services;
            return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiLayoutApiV1& api() const noexcept { return api_; }
    MmsResult SetDefaultHints(std::string_view elementId, const MmsUiLayoutHintsV1& hints) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return layout_detail::Call([&] { return api_.setDefaultHints(api_.context, layout_detail::Text(elementId), &hints); });
    }
    MmsResult StageHints(MmsHandle transaction, std::string_view elementId, std::uint64_t generation,
                         const MmsUiLayoutHintsV1& hints) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return layout_detail::Call([&] {
            return api_.stageHints(api_.context, transaction, layout_detail::Text(elementId), generation, &hints);
        });
    }
    MmsResult AcquireSnapshot(LayoutSnapshotLease& out) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (out.valid()) return MMS_E_BUSY;
        return layout_detail::Call([&]() -> MmsResult {
            MmsUiLayoutSnapshotV1 snapshot{};
            snapshot.h = {sizeof(snapshot), MMS_ABI_MAJOR, MMS_ABI_MINOR};
            const auto result = api_.acquireSnapshot(api_.context, &snapshot);
            if (result != MMS_OK) return result;
            LayoutSnapshotLease acquired(api_, snapshot.handle);
            return out.TransferFrom(acquired);
        });
    }
};
} // namespace mms::sdk
