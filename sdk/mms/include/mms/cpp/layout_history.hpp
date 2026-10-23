#pragma once

// C++17 convenience wrapper for mms.ui.layout 1.1 layout history. The wrapper
// keeps the host table and forwards the original layout calls; it owns no host
// state. Undo/redo apply globally to the saved layout preferences.
#include <mms/layout.h>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>

namespace mms::sdk {
namespace layout_history_detail {
inline MmsString Text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint64_t>(value.size())};
}
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
} // namespace layout_history_detail

class LayoutHistoryServices {
    MmsHostV1 host_{};
    MmsUiLayoutApiV1_1 api_{};
    bool ready_ = false;
public:
    // Queries major=1 minor=1. A host that only implements 1.0 returns
    // MMS_E_VERSION and leaves out untouched.
    static MmsResult Query(const MmsHostV1& host, LayoutHistoryServices& out) noexcept {
        if (host.h.size < sizeof(MmsHostV1) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return layout_history_detail::Call([&]() -> MmsResult {
            MmsUiLayoutApiV1_1 candidate{};
            const auto result = host.queryService(host.context,
                layout_history_detail::Text("mms.ui.layout"), 1, 1,
                static_cast<std::uint32_t>(sizeof(candidate)), &candidate);
            if (result != MMS_OK) return result;
            if (candidate.h.size < sizeof(candidate) || candidate.h.major != 1) return MMS_E_VERSION;
            if (!candidate.setDefaultHints || !candidate.stageHints || !candidate.acquireSnapshot ||
                !candidate.releaseSnapshot || !candidate.queryHistory || !candidate.undo || !candidate.redo)
                return MMS_E_UNSUPPORTED;
            LayoutHistoryServices services;
            services.host_ = host;
            services.api_ = candidate;
            services.ready_ = true;
            out = services;
            return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiLayoutApiV1_1& api() const noexcept { return api_; }
    // Original 1.0 layout calls remain available through the same table.
    MmsResult SetDefaultHints(std::string_view id, const MmsUiLayoutHintsV1& hints) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return layout_history_detail::Call([&] { return api_.setDefaultHints(api_.context, layout_history_detail::Text(id), &hints); });
    }
    MmsResult Info(MmsUiLayoutHistoryInfoV1& out) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return layout_history_detail::Call([&]() -> MmsResult {
            MmsUiLayoutHistoryInfoV1 candidate{};
            candidate.h = {sizeof(candidate), MMS_ABI_MAJOR, MMS_ABI_MINOR};
            const auto result = api_.queryHistory(api_.context, &candidate);
            if (result == MMS_OK) out = candidate;
            return result;
        });
    }
    MmsResult Undo() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return layout_history_detail::Call([&] { return api_.undo(api_.context); });
    }
    MmsResult Redo() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return layout_history_detail::Call([&] { return api_.redo(api_.context); });
    }
};
} // namespace mms::sdk
