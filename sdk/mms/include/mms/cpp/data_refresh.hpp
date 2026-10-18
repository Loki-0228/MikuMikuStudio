#pragma once
// C++17 ownership over the SAME DATA bindings, immutable models and refresh
// tickets as the C services. All service contexts must outlive these objects.
#include <mms/cpp/refresh.hpp>
#include <mms/refresh_binding.h>

namespace mms::sdk {
class DataServices;

class DataSource {
    MmsUiRefreshBindingApiV1 api_{};
    MmsHandle handle_ = 0;
    friend class DataServices;
public:
    DataSource() noexcept = default;
    DataSource(const DataSource&) = delete;
    DataSource& operator=(const DataSource&) = delete;
    DataSource(DataSource&& other) noexcept : api_(other.api_), handle_(std::exchange(other.handle_, 0)) { other.api_ = {}; }
    DataSource& operator=(DataSource&&) = delete;
    ~DataSource() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    // Workers may publish an already prepared immutable value. Use the exact
    // last source revision; a stale publisher cannot overwrite a newer value.
    MmsResult Publish(MmsBytes bytes, std::uint64_t expectedRevision, MmsUiDataSourceStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiDataSourcePublishV1 request{}; request.h = Header<MmsUiDataSourcePublishV1>();
        request.source = handle_; request.expectedRevision = expectedRevision; request.bytes = bytes;
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        const auto result = detail::Call([&] { return api_.publishSource(api_.context, &request, &state); });
        if (!result) out = state;
        return result;
    }
    MmsResult State(MmsUiDataSourceStateV1& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        const auto result = detail::Call([&] { return api_.getSource(api_.context, handle_, &state); });
        if (!result) out = state;
        return result;
    }
    // UI thread. Explicitly release during stop; retain this object on failure.
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        const auto result = detail::Call([&] { return api_.unregisterSource(api_.context, handle_); });
        if (result && result != MMS_E_NOT_FOUND && result != MMS_E_STALE) return result;
        handle_ = 0; api_ = {}; return MMS_OK;
    }
    MmsHandle detach() noexcept { api_ = {}; return std::exchange(handle_, 0); }
};

class DataDisplay {
    MmsUiRefreshBindingApiV1 api_{};
    MmsUiDataDisplayV1 value_{};
    friend class DataBinding;
public:
    DataDisplay() noexcept = default;
    DataDisplay(const DataDisplay&) = delete;
    DataDisplay& operator=(const DataDisplay&) = delete;
    DataDisplay(DataDisplay&& other) noexcept : api_(other.api_), value_(other.value_) { other.api_ = {}; other.value_ = {}; }
    DataDisplay& operator=(DataDisplay&&) = delete;
    ~DataDisplay() { Release(); }
    explicit operator bool() const noexcept { return value_.lease != 0; }
    const MmsUiDataDisplayV1& value() const noexcept { return value_; }
    MmsBytes bytes() const noexcept { return value_.bytes; }
    MmsResult Release() noexcept {
        if (!value_.lease) return MMS_OK;
        const auto result = detail::Call([&] { return api_.releaseDisplay(api_.context, value_.lease); });
        if (result && result != MMS_E_NOT_FOUND) return result;
        api_ = {}; value_ = {}; return MMS_OK;
    }
    MmsUiDataDisplayV1 detach() noexcept {
        const auto value = value_; api_ = {}; value_ = {}; return value;
    }
};

class DataBinding {
    MmsUiRefreshBindingApiV1 api_{};
    MmsHandle handle_ = 0;
    friend class DataServices;
public:
    DataBinding() noexcept = default;
    DataBinding(const DataBinding&) = delete;
    DataBinding& operator=(const DataBinding&) = delete;
    DataBinding(DataBinding&& other) noexcept : api_(other.api_), handle_(std::exchange(other.handle_, 0)) { other.api_ = {}; }
    DataBinding& operator=(DataBinding&&) = delete;
    ~DataBinding() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    MmsResult Acquire(DataDisplay& out) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        if (out) return MMS_E_BUSY;
        MmsUiDataDisplayV1 value{}; value.h = Header<MmsUiDataDisplayV1>();
        const auto result = detail::Call([&] { return api_.acquireDisplay(api_.context, handle_, &value); });
        if (result) return result;
        if (!value.lease) return MMS_E_FAILED;
        out.api_ = api_; out.value_ = value; return MMS_OK;
    }
    // The regular refresh service owns the returned ticket. DATA does not
    // implicitly repaint; this convenience explicitly requests both by default.
    MmsResult Refresh(const RefreshServices& refresh, const MmsUiTargetRefV1& target,
        std::uint64_t sourceRevision, RefreshTicket& out,
        std::uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT,
        std::uint64_t coalesceKey = 0) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = target; request.binding = handle_; request.sourceRevision = sourceRevision;
        request.kinds = kinds; request.scope = MMS_UI_REFRESH_SELF; request.coalesceKey = coalesceKey;
        return refresh.Request(request, out);
    }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        const auto result = detail::Call([&] { return api_.unbind(api_.context, handle_); });
        if (result && result != MMS_E_NOT_FOUND && result != MMS_E_STALE) return result;
        handle_ = 0; api_ = {}; return MMS_OK;
    }
    MmsHandle detach() noexcept { api_ = {}; return std::exchange(handle_, 0); }
};

class DataServices {
    MmsUiRefreshBindingApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    static MmsResult Query(const MmsHostV1& host, DataServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiRefreshBindingApiV1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.refresh.bind"), 1, 0, sizeof(table), &table);
            if (result) return result;
            if (!detail::TableHeader(table)) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.registerSource || !table.publishSource || !table.getSource ||
                !table.unregisterSource || !table.bind || !table.unbind || !table.writePrepared ||
                !table.acquireDisplay || !table.releaseDisplay) return MMS_E_UNSUPPORTED;
            DataServices next; next.api_ = table; next.thread_ = std::this_thread::get_id(); next.ready_ = true;
            out = next; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsUiRefreshBindingApiV1& api() const noexcept { return api_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    MmsResult Capabilities(MmsUiDataCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &caps); });
        if (!result) out = caps;
        return result;
    }
    MmsResult RegisterSource(MmsString id, std::uint64_t maxBytes, DataSource& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        MmsUiDataSourceDescV1 desc{}; desc.h = Header<MmsUiDataSourceDescV1>(); desc.id = id; desc.maxBytes = maxBytes;
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.registerSource(api_.context, &desc, &handle); });
        if (result) return result;
        if (!handle) return MMS_E_FAILED;
        out.api_ = api_; out.handle_ = handle; return MMS_OK;
    }
    // Null prepare copies the source unchanged. A complex plugin can prepare
    // a different display model without creating another UI target.
    MmsResult Bind(const MmsUiTargetRefV1& target, const DataSource& source, std::uint64_t maxDisplayBytes,
        DataBinding& out, MmsUiDataPrepareCallbackV1 prepare = nullptr, void* context = nullptr) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (!source) return MMS_E_NOT_READY;
        if (source.api_.context != api_.context) return MMS_E_ARGUMENT;
        if (out) return MMS_E_BUSY;
        MmsUiDataBindingDescV1 desc{}; desc.h = Header<MmsUiDataBindingDescV1>();
        desc.target = target; desc.source = source.get(); desc.maxDisplayBytes = maxDisplayBytes;
        desc.prepare = prepare; desc.context = context;
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.bind(api_.context, &desc, &handle); });
        if (result) return result;
        if (!handle) return MMS_E_FAILED;
        out.api_ = api_; out.handle_ = handle; return MMS_OK;
    }
    MmsResult WritePrepared(MmsHandle builder, MmsBytes bytes) const noexcept {
        if (const auto result = CheckThread()) return result;
        return detail::Call([&] { return api_.writePrepared(api_.context, builder, bytes); });
    }
};
} // namespace mms::sdk
