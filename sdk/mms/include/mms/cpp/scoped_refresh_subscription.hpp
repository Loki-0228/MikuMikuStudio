#pragma once
// C++17 ownership of the original scoped subscription handle. Source DATA,
// binding, seed, targets and queue remain owned by their existing C services.
// The service context must outlive this object, including its destructor.
#include <mms/cpp/refresh_subscription.hpp>

namespace mms::sdk {
namespace detail {
template<class T> constexpr MmsHeader ScopedSubscriptionValueHeader() noexcept {
    return {static_cast<std::uint32_t>(sizeof(T)), 1, 0};
}
template<class T> bool ScopedSubscriptionValueValid(const T& value) noexcept {
    return value.h.size >= sizeof(T) && value.h.major == 1 && value.h.minor == 0;
}
} // namespace detail

class ScopedRefreshSubscriptionServices;

class ScopedRefreshSubscription {
    MmsUiRefreshSubscriptionApiV1_1 api_{};
    MmsHandle handle_ = 0;
    std::thread::id thread_{};
    friend class ScopedRefreshSubscriptionServices;
public:
    ScopedRefreshSubscription() noexcept = default;
    ScopedRefreshSubscription(const ScopedRefreshSubscription&) = delete;
    ScopedRefreshSubscription& operator=(const ScopedRefreshSubscription&) = delete;
    ScopedRefreshSubscription(ScopedRefreshSubscription&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_, 0)), thread_(other.thread_) {
        other.api_ = {}; other.thread_ = {};
    }
    ScopedRefreshSubscription& operator=(ScopedRefreshSubscription&&) = delete;
    // Explicitly Release on the original UI thread at stop. Failed cleanup
    // retains this owner for retry; destruction is a best-effort fallback.
    ~ScopedRefreshSubscription() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    // Both queries permit workers after publishing ownership without concurrent
    // moves/cleanup. Error, exception or malformed output leaves out unchanged.
    MmsResult State(MmsUiRefreshScopedSubscriptionStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiRefreshScopedSubscriptionStateV1 candidate{};
        candidate.h = detail::ScopedSubscriptionValueHeader<MmsUiRefreshScopedSubscriptionStateV1>();
        const auto result = detail::Call([&] { return api_.queryScoped(api_.context, handle_, &candidate); });
        if (result) return result;
        if (!detail::ScopedSubscriptionValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult BaseState(MmsUiRefreshSubscriptionStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiRefreshSubscriptionStateV1 candidate{};
        candidate.h = detail::ScopedSubscriptionValueHeader<MmsUiRefreshSubscriptionStateV1>();
        const auto result = detail::Call([&] { return api_.query(api_.context, handle_, &candidate); });
        if (result) return result;
        if (!detail::ScopedSubscriptionValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.unsubscribe(api_.context, handle_); });
        if (result && result != MMS_E_STALE) return result;
        handle_ = 0; api_ = {}; thread_ = {}; return MMS_OK;
    }
    // Transfer this same handle to a lower-level owner. The original 1.0
    // query/unsubscribe can operate it; this SDK owner no longer touches it.
    MmsHandle detach() noexcept {
        api_ = {}; thread_ = {}; return std::exchange(handle_, 0);
    }
};

class ScopedRefreshSubscriptionServices {
    MmsUiRefreshSubscriptionApiV1_1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
    MmsResult SubscribeScope(MmsHandle binding, const MmsUiTargetRefV1& seed, std::uint32_t scope,
        std::uint32_t kinds, ScopedRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        if (!binding) return MMS_E_ARGUMENT;
        MmsUiRefreshScopedSubscriptionDescV1 desc{};
        desc.h = detail::ScopedSubscriptionValueHeader<MmsUiRefreshScopedSubscriptionDescV1>();
        desc.binding = binding; desc.seed = seed; desc.scope = scope; desc.kinds = kinds;
        return Subscribe(desc, out);
    }
    MmsResult SubscribeElement(const RefreshServices& refresh, MmsHandle binding, MmsHandle seed,
        std::uint32_t scope, std::uint32_t kinds, ScopedRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (const auto result = refresh.CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        if (!binding || !seed) return MMS_E_ARGUMENT;
        MmsUiRefreshTargetV1 target{};
        if (const auto result = refresh.Target(seed, target)) return result;
        if (!detail::ScopedSubscriptionValueValid(target) || !detail::ScopedSubscriptionValueValid(target.target)) return MMS_E_VERSION;
        if (!(target.supportedScopes & scope)) return MMS_E_UNSUPPORTED;
        return SubscribeScope(binding, target.target, scope, kinds, out);
    }
public:
    // UI-thread negotiation requires the actual 1.1 table. Never fall back to
    // 1.0 and infer scoped support from a service name or a larger buffer.
    static MmsResult Query(const MmsHostV1& host, ScopedRefreshSubscriptionServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiRefreshSubscriptionApiV1_1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.refresh.subscribe"),
                1, 1, static_cast<std::uint32_t>(sizeof(table)), &table);
            if (result) return result;
            if (table.h.size < sizeof(table) || table.h.major != 1 || table.h.minor < 1) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.subscribe || !table.query || !table.unsubscribe ||
                !table.subscribeScoped || !table.queryScoped) return MMS_E_UNSUPPORTED;
            ScopedRefreshSubscriptionServices candidate;
            candidate.api_ = table; candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
            out = candidate; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiRefreshSubscriptionApiV1_1& api() const noexcept { return api_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    MmsResult Capabilities(MmsUiRefreshSubscriptionCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiRefreshSubscriptionCapabilitiesV1 candidate{};
        candidate.h = detail::ScopedSubscriptionValueHeader<MmsUiRefreshSubscriptionCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
        if (result) return result;
        if (!detail::ScopedSubscriptionValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult Subscribe(const MmsUiRefreshScopedSubscriptionDescV1& desc, ScopedRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.subscribeScoped(api_.context, &desc, &handle); });
        if (result) return result;
        if (!handle) return MMS_E_FAILED;
        out.api_ = api_; out.handle_ = handle; out.thread_ = thread_; return MMS_OK;
    }
    MmsResult SubscribeSubtree(MmsHandle binding, const MmsUiTargetRefV1& seed, ScopedRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) const noexcept {
        return SubscribeScope(binding, seed, MMS_UI_REFRESH_SUBTREE, kinds, out);
    }
    MmsResult SubscribeDependents(MmsHandle binding, const MmsUiTargetRefV1& seed, ScopedRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT) const noexcept {
        return SubscribeScope(binding, seed, MMS_UI_REFRESH_DEPENDENTS, kinds, out);
    }
    // Resolve the existing seed on the UI thread through the regular service.
    // No source, binding, UI element, target or queue is registered here.
    MmsResult SubscribeSubtree(const RefreshServices& refresh, MmsHandle binding, MmsHandle seed,
        ScopedRefreshSubscription& out, std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) const noexcept {
        return SubscribeElement(refresh, binding, seed, MMS_UI_REFRESH_SUBTREE, kinds, out);
    }
    MmsResult SubscribeDependents(const RefreshServices& refresh, MmsHandle binding, MmsHandle seed,
        ScopedRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT) const noexcept {
        return SubscribeElement(refresh, binding, seed, MMS_UI_REFRESH_DEPENDENTS, kinds, out);
    }
};
} // namespace mms::sdk
