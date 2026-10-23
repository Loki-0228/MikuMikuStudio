#pragma once

// C++17 convenience wrapper for the mms.render 1.0 device-generation and
// device-lease service. The wrapper keeps the host table and returns the same
// handles; it never owns device resources. Leases must be released explicitly
// before the plugin stops; the host also releases them on device loss.
#include <mms/render.h>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mms::sdk {
namespace render_detail {
inline MmsString Text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint64_t>(value.size())};
}
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
} // namespace render_detail

class RenderLease {
    MmsRenderApiV1 api_{};
    MmsHandle handle_ = 0;
public:
    RenderLease() = default;
    RenderLease(MmsRenderApiV1 api, MmsHandle handle) noexcept : api_(api), handle_(handle) {}
    RenderLease(const RenderLease&) = delete;
    RenderLease& operator=(const RenderLease&) = delete;
    RenderLease(RenderLease&& other) noexcept : api_(other.api_), handle_(other.handle_) {
        other.handle_ = 0;
    }
    RenderLease& operator=(RenderLease&& other) noexcept {
        if (this != &other) { Release(); api_ = other.api_; handle_ = other.handle_; other.handle_ = 0; }
        return *this;
    }
    ~RenderLease() = default; // explicit Release: no host call from a destructor
    bool valid() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (!api_.releaseLease) return MMS_E_UNSUPPORTED;
        const MmsHandle handle = handle_;
        handle_ = 0;
        return render_detail::Call([&] { return api_.releaseLease(api_.context, handle); });
    }
    MmsHandle Detach() noexcept { const MmsHandle handle = handle_; handle_ = 0; return handle; }
};

class RenderServices {
    MmsHostV1 host_{};
    MmsRenderApiV1 render_{};
    bool ready_ = false;

    template<class T> MmsResult QueryTable(std::string_view name, T& table) const noexcept {
        return render_detail::Call([&]() -> MmsResult {
            T candidate{};
            const auto result = host_.queryService(host_.context, render_detail::Text(name), 1, 0,
                static_cast<std::uint32_t>(sizeof(T)), &candidate);
            if (result != MMS_OK) return result;
            if (candidate.h.size < sizeof(T) || candidate.h.major != 1) return MMS_E_VERSION;
            table = candidate;
            return MMS_OK;
        });
    }
public:
    static MmsResult Query(const MmsHostV1& host, RenderServices& out) noexcept {
        if (host.h.size < sizeof(MmsHostV1) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        RenderServices candidate;
        candidate.host_ = host;
        const auto result = candidate.QueryTable("mms.render", candidate.render_);
        if (result != MMS_OK) return result;
        const auto& render = candidate.render_;
        if (!render.query || !render.acquireLease || !render.releaseLease) return MMS_E_UNSUPPORTED;
        candidate.ready_ = true;
        out = candidate;
        return MMS_OK;
    }
    bool ready() const noexcept { return ready_; }
    const MmsRenderApiV1& api() const noexcept { return render_; }
    MmsResult Info(MmsRenderInfoV1& out) const noexcept {
        if (!ready_ || !render_.query) return MMS_E_NOT_READY;
        return render_detail::Call([&]() -> MmsResult {
            MmsRenderInfoV1 candidate{};
            candidate.h = {sizeof(candidate), MMS_ABI_MAJOR, MMS_ABI_MINOR};
            const auto result = render_.query(render_.context, &candidate);
            if (result == MMS_OK) out = candidate;
            return result;
        });
    }
    MmsResult Acquire(std::string_view name, void (MMS_CALL *release)(void*), void* context,
                      RenderLease& out) const noexcept {
        if (!ready_ || !render_.acquireLease) return MMS_E_NOT_READY;
        return render_detail::Call([&]() -> MmsResult {
            MmsRenderLeaseDescV1 desc{};
            desc.h = {sizeof(desc), MMS_ABI_MAJOR, MMS_ABI_MINOR};
            desc.name = render_detail::Text(name);
            desc.release = release;
            desc.context = context;
            desc.flags = MMS_RENDER_LEASE_DEVICE;
            MmsHandle handle = 0;
            const auto result = render_.acquireLease(render_.context, &desc, &handle);
            if (result == MMS_OK) out = RenderLease(render_, handle);
            return result;
        });
    }
};
} // namespace mms::sdk
