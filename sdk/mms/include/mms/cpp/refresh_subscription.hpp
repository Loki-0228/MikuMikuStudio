#pragma once
// C++17 ownership of the original C subscription and DATA binding handles.
// The host service context must outlive these objects and their destructors.
#include <mms/cpp/data_refresh.hpp>
#include <mms/refresh_subscription.h>

namespace mms::sdk {

class RefreshSubscriptionServices;

class RefreshSubscription {
	MmsUiRefreshSubscriptionApiV1 api_{};
	MmsHandle handle_ = 0;
	std::thread::id thread_{};
	friend class RefreshSubscriptionServices;
public:
	RefreshSubscription() noexcept = default;
	RefreshSubscription(const RefreshSubscription&) = delete;
	RefreshSubscription& operator=(const RefreshSubscription&) = delete;
	RefreshSubscription(RefreshSubscription&& other) noexcept
		: api_(other.api_), handle_(std::exchange(other.handle_, 0)), thread_(other.thread_) {
		other.api_ = {}; other.thread_ = {};
	}
	RefreshSubscription& operator=(RefreshSubscription&&) = delete;
	// Call Release on the UI thread during stop. On failure retain the object
	// and retry there; destruction is only a best-effort fallback.
	~RefreshSubscription() { Release(); }
	explicit operator bool() const noexcept { return handle_ != 0; }
	MmsHandle get() const noexcept { return handle_; }
	// State queries may run on a worker. A failure leaves out unchanged.
	MmsResult State(MmsUiRefreshSubscriptionStateV1& out) const noexcept {
		if (!handle_) return MMS_E_NOT_READY;
		MmsUiRefreshSubscriptionStateV1 candidate{};
		candidate.h = Header<MmsUiRefreshSubscriptionStateV1>();
		const auto result = detail::Call([&] { return api_.query(api_.context, handle_, &candidate); });
		if (!result) out = candidate;
		return result;
	}
	MmsResult Release() noexcept {
		if (!handle_) return MMS_OK;
		if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
		const auto result = detail::Call([&] { return api_.unsubscribe(api_.context, handle_); });
		if (result && result != MMS_E_STALE) return result;
		handle_ = 0; api_ = {}; thread_ = {}; return MMS_OK;
	}
	// Transfer the same C handle. Its new owner must unsubscribe through the
	// original service on the UI thread, or let host lifecycle cleanup retire it.
	MmsHandle detach() noexcept {
		api_ = {}; thread_ = {}; return std::exchange(handle_, 0);
	}
};

class RefreshSubscriptionServices {
	MmsUiRefreshSubscriptionApiV1 api_{};
	std::thread::id thread_{};
	bool ready_ = false;
public:
	static MmsResult Query(const MmsHostV1& host, RefreshSubscriptionServices& out) noexcept {
		if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
		if (!host.queryService) return MMS_E_ARGUMENT;
		return detail::Call([&]() -> MmsResult {
			MmsUiRefreshSubscriptionApiV1 table{};
			const auto result = host.queryService(host.context, Text("mms.ui.refresh.subscribe"),
				1, 0, static_cast<std::uint32_t>(sizeof(table)), &table);
			if (result) return result;
			if (!detail::TableHeader(table)) return MMS_E_VERSION;
			if (!table.getCapabilities || !table.subscribe || !table.query || !table.unsubscribe)
				return MMS_E_UNSUPPORTED;
			RefreshSubscriptionServices candidate;
			candidate.api_ = table; candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
			out = candidate; return MMS_OK;
		});
	}
	bool ready() const noexcept { return ready_; }
	const MmsUiRefreshSubscriptionApiV1& api() const noexcept { return api_; }
	MmsResult CheckThread() const noexcept {
		if (!ready_) return MMS_E_NOT_READY;
		return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
	}
	MmsResult Capabilities(MmsUiRefreshSubscriptionCapabilitiesV1& out) const noexcept {
		if (const auto result = CheckThread()) return result;
		MmsUiRefreshSubscriptionCapabilitiesV1 candidate{};
		candidate.h = Header<MmsUiRefreshSubscriptionCapabilitiesV1>();
		const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
		if (!result) out = candidate;
		return result;
	}
	// Subscribe to an existing binding. No source, UI element or refresh queue
	// is duplicated. Publishing unchanged bytes does not schedule another refresh.
	MmsResult Subscribe(MmsHandle binding, RefreshSubscription& out,
		std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) const noexcept {
		if (const auto result = CheckThread()) return result;
		if (!binding) return MMS_E_ARGUMENT;
		if (out) return MMS_E_BUSY;
		MmsUiRefreshSubscriptionDescV1 desc{};
		desc.h = Header<MmsUiRefreshSubscriptionDescV1>(); desc.binding = binding; desc.kinds = kinds;
		MmsHandle handle = 0;
		const auto result = detail::Call([&] { return api_.subscribe(api_.context, &desc, &handle); });
		if (result) return result;
		if (!handle) return MMS_E_FAILED;
		out.api_ = api_; out.handle_ = handle; out.thread_ = thread_; return MMS_OK;
	}
	MmsResult Subscribe(const DataBinding& binding, RefreshSubscription& out,
		std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT) const noexcept {
		if (!binding) return MMS_E_NOT_READY;
		return Subscribe(binding.get(), out, kinds);
	}
};
} // namespace mms::sdk
