#pragma once
// C++17 ownership over the original property, seed and subscription handles.
// Service contexts must outlive these borrowed tables and their subscriptions,
// including destruction. No property value, source, target or queue is created.
#include <mms/cpp/property_refresh.hpp>

namespace mms::sdk {
namespace detail {
template<class T> constexpr MmsHeader ScopedPropertyValueHeader() noexcept {
    return {static_cast<std::uint32_t>(sizeof(T)), 1, 0};
}
template<class T> bool ScopedPropertyValueValid(const T& value) noexcept {
    return value.h.size >= sizeof(T) && value.h.major == 1 && value.h.minor == 0;
}
} // namespace detail

class ScopedPropertyRefreshServices;

class ScopedPropertyRefreshSubscription {
    MmsUiPropertyRefreshApiV1_1 api_{};
    MmsHandle handle_ = 0;
    std::thread::id thread_{};
    friend class ScopedPropertyRefreshServices;
public:
    ScopedPropertyRefreshSubscription() noexcept = default;
    ScopedPropertyRefreshSubscription(const ScopedPropertyRefreshSubscription&) = delete;
    ScopedPropertyRefreshSubscription& operator=(const ScopedPropertyRefreshSubscription&) = delete;
    ScopedPropertyRefreshSubscription(ScopedPropertyRefreshSubscription&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_, 0)), thread_(other.thread_) {
        other.api_ = {}; other.thread_ = {};
    }
    ScopedPropertyRefreshSubscription& operator=(ScopedPropertyRefreshSubscription&&) = delete;
    // Release explicitly on the original UI thread during stop. A failed
    // release retains ownership for retry; destruction is best effort.
    ~ScopedPropertyRefreshSubscription() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    // Worker-safe after publishing this owner without concurrent moves or
    // cleanup. Failed calls and malformed results leave out unchanged.
    MmsResult State(MmsUiPropertyRefreshScopedSubscriptionStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiPropertyRefreshScopedSubscriptionStateV1 candidate{};
        candidate.h = detail::ScopedPropertyValueHeader<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
        const auto result = detail::Call([&] { return api_.queryScoped(api_.context, handle_, &candidate); });
        if (result) return result;
        if (!detail::ScopedPropertyValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult BaseState(MmsUiPropertyRefreshSubscriptionStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiPropertyRefreshSubscriptionStateV1 candidate{};
        candidate.h = detail::ScopedPropertyValueHeader<MmsUiPropertyRefreshSubscriptionStateV1>();
        const auto result = detail::Call([&] { return api_.query(api_.context, handle_, &candidate); });
        if (result) return result;
        if (!detail::ScopedPropertyValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.unsubscribe(api_.context, handle_); });
        if (result && result != MMS_E_STALE) return result;
        handle_ = 0; api_ = {}; thread_ = {}; return MMS_OK;
    }
    // Transfer the same C handle. Original 1.0 query/unsubscribe callbacks can
    // operate it; their original context and UI-thread rules still apply.
    MmsHandle detach() noexcept {
        api_ = {}; thread_ = {}; return std::exchange(handle_, 0);
    }
};

class ScopedPropertyRefreshServices {
    MmsUiPropertyRefreshApiV1_1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
    MmsResult SubscribeScope(const MmsUiPropertyRefV1& property, const MmsUiTargetRefV1& seed,
        std::uint32_t scope, std::uint32_t kinds, std::uint32_t flags,
        ScopedPropertyRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        MmsUiPropertyRefreshScopedSubscriptionDescV1 desc{};
        desc.h = detail::ScopedPropertyValueHeader<MmsUiPropertyRefreshScopedSubscriptionDescV1>();
        desc.property = property; desc.seed = seed; desc.scope = scope; desc.kinds = kinds; desc.flags = flags;
        return Subscribe(desc, out);
    }
    MmsResult SubscribeElement(const RefreshServices& refresh, MmsHandle property, MmsHandle seed,
        std::uint32_t scope, std::uint32_t kinds, std::uint32_t flags,
        ScopedPropertyRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (const auto result = refresh.CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        if (!property || !seed) return MMS_E_ARGUMENT;
        MmsUiPropertyRefreshScopedCapabilitiesV1 caps{};
        if (const auto result = Capabilities(caps)) return result;
        if (!(caps.supportedScopes & scope)) return MMS_E_UNSUPPORTED;
        MmsUiPropertyRefV1 ref{};
        if (const auto result = GetPropertyRef(property, ref)) return result;
        MmsUiRefreshTargetV1 target{};
        if (const auto result = refresh.Target(seed, target)) return result;
        if (!detail::ScopedPropertyValueValid(target) || !detail::ScopedPropertyValueValid(target.target)) return MMS_E_VERSION;
        return SubscribeScope(ref, target.target, scope, kinds, flags, out);
    }
public:
    // Request the actual 1.1 table on the UI thread. There is no 1.0 fallback
    // and no emulation with multiple SELF subscriptions.
    static MmsResult Query(const MmsHostV1& host, ScopedPropertyRefreshServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiPropertyRefreshApiV1_1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.refresh.property"),
                1, 1, static_cast<std::uint32_t>(sizeof(table)), &table);
            if (result) return result;
            if (table.h.size < sizeof(table) || table.h.major != 1 || table.h.minor < 1) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.getPropertyRef || !table.subscribe || !table.notify ||
                !table.query || !table.unsubscribe || !table.getScopedCapabilities ||
                !table.subscribeScoped || !table.queryScoped) return MMS_E_UNSUPPORTED;
            ScopedPropertyRefreshServices candidate;
            candidate.api_ = table; candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
            out = candidate; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiPropertyRefreshApiV1_1& api() const noexcept { return api_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    MmsResult Capabilities(MmsUiPropertyRefreshScopedCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiPropertyRefreshScopedCapabilitiesV1 candidate{};
        candidate.h = detail::ScopedPropertyValueHeader<MmsUiPropertyRefreshScopedCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getScopedCapabilities(api_.context, &candidate); });
        if (result) return result;
        if (!detail::ScopedPropertyValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult BaseCapabilities(MmsUiPropertyRefreshCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiPropertyRefreshCapabilitiesV1 candidate{};
        candidate.h = detail::ScopedPropertyValueHeader<MmsUiPropertyRefreshCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
        if (result) return result;
        if (!detail::ScopedPropertyValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult GetPropertyRef(MmsHandle property, MmsUiPropertyRefV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (!property) return MMS_E_ARGUMENT;
        MmsUiPropertyRefV1 candidate{}; candidate.h = detail::ScopedPropertyValueHeader<MmsUiPropertyRefV1>();
        const auto result = detail::Call([&] { return api_.getPropertyRef(api_.context, property, &candidate); });
        if (result) return result;
        if (!detail::ScopedPropertyValueValid(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    // The host validates original identities and scope. FOLLOW_HISTORY must
    // be explicitly requested and actually advertised by this service.
    MmsResult Subscribe(const MmsUiPropertyRefreshScopedSubscriptionDescV1& desc,
        ScopedPropertyRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        if (desc.flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) {
            MmsUiPropertyRefreshScopedCapabilitiesV1 caps{};
            if (const auto result = Capabilities(caps)) return result;
            if (!(caps.supportedFlags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY)) return MMS_E_UNSUPPORTED;
        }
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.subscribeScoped(api_.context, &desc, &handle); });
        if (result) return result;
        if (!handle) return MMS_E_FAILED;
        out.api_ = api_; out.handle_ = handle; out.thread_ = thread_; return MMS_OK;
    }
    MmsResult SubscribeSubtree(const MmsUiPropertyRefV1& property, const MmsUiTargetRefV1& seed,
        ScopedPropertyRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, std::uint32_t flags = 0) const noexcept {
        return SubscribeScope(property, seed, MMS_UI_REFRESH_SUBTREE, kinds, flags, out);
    }
    MmsResult SubscribeDependents(const MmsUiPropertyRefV1& property, const MmsUiTargetRefV1& seed,
        ScopedPropertyRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT,
        std::uint32_t flags = 0) const noexcept {
        return SubscribeScope(property, seed, MMS_UI_REFRESH_DEPENDENTS, kinds, flags, out);
    }
    // Resolve original registrations through this same property table and
    // the normal refresh service. Service capabilities allow registration
    // before graph edges exist; the seed need not be a DATA consumer.
    MmsResult SubscribeSubtree(const RefreshServices& refresh, MmsHandle property, MmsHandle seed,
        ScopedPropertyRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT, std::uint32_t flags = 0) const noexcept {
        return SubscribeElement(refresh, property, seed, MMS_UI_REFRESH_SUBTREE, kinds, flags, out);
    }
    MmsResult SubscribeDependents(const RefreshServices& refresh, MmsHandle property, MmsHandle seed,
        ScopedPropertyRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT,
        std::uint32_t flags = 0) const noexcept {
        return SubscribeElement(refresh, property, seed, MMS_UI_REFRESH_DEPENDENTS, kinds, flags, out);
    }
    // Worker-safe with an existing reference and a service copy published
    // without concurrent replacement. Notify records intent, not a value copy.
    MmsResult Notify(const MmsUiPropertyRefV1& property) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return detail::Call([&] { return api_.notify(api_.context, &property); });
    }
};
} // namespace mms::sdk
