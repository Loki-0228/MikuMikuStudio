#pragma once
// C++17 ownership of subscriptions to the original registered property and
// INPUT target. The host service context must outlive these borrowed tables
// and every subscription, including its destructor.
#include <mms/cpp/refresh.hpp>
#include <mms/property_refresh.h>

namespace mms::sdk {

class PropertyRefreshServices;

class PropertyRefreshSubscription {
    MmsUiPropertyRefreshApiV1 api_{};
    MmsHandle handle_ = 0;
    std::thread::id thread_{};
    friend class PropertyRefreshServices;
public:
    PropertyRefreshSubscription() noexcept = default;
    PropertyRefreshSubscription(const PropertyRefreshSubscription&) = delete;
    PropertyRefreshSubscription& operator=(const PropertyRefreshSubscription&) = delete;
    PropertyRefreshSubscription(PropertyRefreshSubscription&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_, 0)), thread_(other.thread_) {
        other.api_ = {}; other.thread_ = {};
    }
    PropertyRefreshSubscription& operator=(PropertyRefreshSubscription&&) = delete;
    // Explicitly release on the UI thread during stop. On failure retain the
    // owner and retry; destruction can only make a best-effort attempt.
    ~PropertyRefreshSubscription() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    // Worker-safe after the owner has been published without concurrent moves
    // or cleanup. An error or exception leaves the caller's output unchanged.
    MmsResult State(MmsUiPropertyRefreshSubscriptionStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiPropertyRefreshSubscriptionStateV1 candidate{};
        candidate.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>();
        const auto result = detail::Call([&] { return api_.query(api_.context, handle_, &candidate); });
        if (result) return result;
        if (!detail::TableHeader(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.unsubscribe(api_.context, handle_); });
        if (result && result != MMS_E_STALE) return result;
        handle_ = 0; api_ = {}; thread_ = {}; return MMS_OK;
    }
    // Transfer the same C handle. The recipient must use its original service
    // on the UI thread, or rely on host lifecycle cleanup to retire it.
    MmsHandle detach() noexcept {
        api_ = {}; thread_ = {}; return std::exchange(handle_, 0);
    }
};

class PropertyRefreshServices {
    MmsUiPropertyRefreshApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    // Call on the host UI thread. Failed negotiation retains out unchanged.
    static MmsResult Query(const MmsHostV1& host, PropertyRefreshServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiPropertyRefreshApiV1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.refresh.property"),
                1, 0, static_cast<std::uint32_t>(sizeof(table)), &table);
            if (result) return result;
            if (!detail::TableHeader(table)) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.getPropertyRef || !table.subscribe ||
                !table.notify || !table.query || !table.unsubscribe) return MMS_E_UNSUPPORTED;
            PropertyRefreshServices candidate;
            candidate.api_ = table; candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
            out = candidate; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiPropertyRefreshApiV1& api() const noexcept { return api_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    MmsResult Capabilities(MmsUiPropertyRefreshCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiPropertyRefreshCapabilitiesV1 candidate{};
        candidate.h = Header<MmsUiPropertyRefreshCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
        if (result) return result;
        if (!detail::TableHeader(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    MmsResult GetPropertyRef(MmsHandle property, MmsUiPropertyRefV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (!property) return MMS_E_ARGUMENT;
        MmsUiPropertyRefV1 candidate{}; candidate.h = Header<MmsUiPropertyRefV1>();
        const auto result = detail::Call([&] { return api_.getPropertyRef(api_.context, property, &candidate); });
        if (result) return result;
        if (!detail::TableHeader(candidate)) return MMS_E_VERSION;
        out = candidate; return MMS_OK;
    }
    // No registration, source or target is duplicated. Host validation owns
    // target/property compatibility; explicit flags are forwarded unchanged.
    MmsResult Subscribe(const MmsUiPropertyRefreshSubscriptionDescV1& desc,
        PropertyRefreshSubscription& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        if (desc.flags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY) {
            MmsUiPropertyRefreshCapabilitiesV1 caps{};
            if (const auto result = Capabilities(caps)) return result;
            if (!(caps.supportedFlags & MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY)) return MMS_E_UNSUPPORTED;
        }
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.subscribe(api_.context, &desc, &handle); });
        if (result) return result;
        if (!handle) return MMS_E_FAILED;
        out.api_ = api_; out.handle_ = handle; out.thread_ = thread_; return MMS_OK;
    }
    // Resolve both references on the UI thread from existing registrations.
    // Default flags deliberately omit FOLLOW_HISTORY; ordinary mms.edit hosts
    // need not support history notifications for this service to be useful.
    MmsResult SubscribeInput(const RefreshServices& refresh, MmsHandle input, MmsHandle property,
        PropertyRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT,
        std::uint32_t flags = 0) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (const auto result = refresh.CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        if (!input || !property) return MMS_E_ARGUMENT;
        MmsUiRefreshTargetV1 target{};
        if (const auto result = refresh.Target(input, target)) return result;
        if (!detail::TableHeader(target) || !detail::TableHeader(target.target)) return MMS_E_VERSION;
        if ((target.supportedKinds & kinds) != kinds || !(target.supportedScopes & MMS_UI_REFRESH_SELF)) return MMS_E_UNSUPPORTED;
        MmsUiPropertyRefreshSubscriptionDescV1 desc{};
        desc.h = Header<MmsUiPropertyRefreshSubscriptionDescV1>(); desc.target = target.target;
        if (const auto result = GetPropertyRef(property, desc.property)) return result;
        desc.kinds = kinds; desc.flags = flags;
        return Subscribe(desc, out);
    }
    MmsResult WatchInput(const RefreshServices& refresh, MmsHandle input, MmsHandle property,
        PropertyRefreshSubscription& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT,
        std::uint32_t flags = 0) const noexcept {
        return SubscribeInput(refresh, input, property, out, kinds, flags);
    }
    // Worker-safe with an already captured property reference. Publish a copy
    // of this service first; do not overwrite it while another thread calls it.
    MmsResult Notify(const MmsUiPropertyRefV1& property) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return detail::Call([&] { return api_.notify(api_.context, &property); });
    }
};
} // namespace mms::sdk
