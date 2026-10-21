#pragma once
// Convenience over the original dependency handles and refresh queue.
// Service contexts must outlive these objects, including their destructors.
#include <mms/cpp/refresh.hpp>
#include <mms/refresh_dependency.h>

namespace mms::sdk {

class RefreshDependencyServices;

class RefreshDependency {
	MmsUiRefreshDependencyApiV1 api_{};
	MmsHandle handle_ = 0;
	friend class RefreshDependencyServices;
public:
	RefreshDependency() noexcept = default;
	RefreshDependency(const RefreshDependency&) = delete;
	RefreshDependency& operator=(const RefreshDependency&) = delete;
	RefreshDependency(RefreshDependency&& other) noexcept
		: api_(other.api_), handle_(std::exchange(other.handle_, 0)) { other.api_ = {}; }
	RefreshDependency& operator=(RefreshDependency&&) = delete;
	// UI thread only. Call Release explicitly during stop and keep the object
	// when release fails; destruction is only a best-effort fallback.
	~RefreshDependency() { Release(); }
	explicit operator bool() const noexcept { return handle_ != 0; }
	MmsHandle get() const noexcept { return handle_; }
	MmsResult Release() noexcept {
		if (!handle_) return MMS_OK;
		const auto result = detail::Call([&] { return api_.unregisterDependency(api_.context, handle_); });
		if (result && result != MMS_E_NOT_FOUND) return result;
		handle_ = 0; api_ = {}; return MMS_OK;
	}
	// Transfer the same C handle. The receiver must unregister it through the
	// original API; this object no longer owns cleanup.
	MmsHandle detach() noexcept { api_ = {}; return std::exchange(handle_, 0); }
};

class RefreshDependencyServices {
	MmsUiRefreshDependencyApiV1 api_{};
	std::thread::id thread_{};
	bool ready_ = false;
public:
	static MmsResult Query(const MmsHostV1& host, RefreshDependencyServices& out) noexcept {
		if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
		if (!host.queryService) return MMS_E_ARGUMENT;
		return detail::Call([&]() -> MmsResult {
			MmsUiRefreshDependencyApiV1 table{};
			const auto result = host.queryService(host.context, Text("mms.ui.refresh.dependencies"),
				1, 0, static_cast<std::uint32_t>(sizeof(table)), &table);
			if (result) return result;
			if (!detail::TableHeader(table)) return MMS_E_VERSION;
			if (!table.getCapabilities || !table.registerDependency || !table.unregisterDependency)
				return MMS_E_UNSUPPORTED;
			RefreshDependencyServices candidate;
			candidate.api_ = table; candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
			out = candidate; return MMS_OK;
		});
	}
	bool ready() const noexcept { return ready_; }
	MmsResult CheckThread() const noexcept {
		if (!ready_) return MMS_E_NOT_READY;
		return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
	}
	const MmsUiRefreshDependencyApiV1& api() const noexcept { return api_; }
	MmsResult Capabilities(MmsUiRefreshDependencyCapabilitiesV1& out) const noexcept {
		if (const auto result = CheckThread()) return result;
		MmsUiRefreshDependencyCapabilitiesV1 candidate{}; candidate.h = Header<MmsUiRefreshDependencyCapabilitiesV1>();
		const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
		if (!result) out = candidate;
		return result;
	}
	MmsResult Connect(const MmsUiTargetRefV1& upstream, const MmsUiTargetRefV1& dependent,
		RefreshDependency& out) const noexcept {
		if (const auto result = CheckThread()) return result;
		if (out) return MMS_E_BUSY;
		MmsUiRefreshDependencyDescV1 desc{}; desc.h = Header<MmsUiRefreshDependencyDescV1>();
		desc.upstream = upstream; desc.dependent = dependent;
		MmsHandle handle = 0;
		const auto result = detail::Call([&] { return api_.registerDependency(api_.context, &desc, &handle); });
		if (result) return result;
		if (!handle) return MMS_E_FAILED;
		out.api_ = api_; out.handle_ = handle; return MMS_OK;
	}
	// Resolve the same registered elements; no extra UI or binding is created.
	MmsResult ConnectElements(const RefreshServices& refresh, MmsHandle upstream, MmsHandle dependent,
		RefreshDependency& out) const noexcept {
		if (const auto result = CheckThread()) return result;
		if (out) return MMS_E_BUSY;
		MmsUiRefreshTargetV1 from{}, to{};
		if (const auto result = refresh.Target(upstream, from)) return result;
		if (const auto result = refresh.Target(dependent, to)) return result;
		return Connect(from.target, to.target, out);
	}
};

} // namespace mms::sdk
