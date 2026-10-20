#pragma once
// Button and property INPUT providers feed the original STATE phase.
#include <mms/cpp/refresh.hpp>
#include <mms/refresh_state.h>
#include <optional>

namespace mms::sdk {
class StateContentServices;
class StateContentProvider {
    MmsUiRefreshStateContentApiV1 api_{};
    MmsHandle handle_ = 0;
    MmsUiTargetRefV1 target_{};
    std::thread::id thread_{};
    friend class StateContentServices;
public:
    StateContentProvider() noexcept = default;
    StateContentProvider(const StateContentProvider&) = delete;
    StateContentProvider& operator=(const StateContentProvider&) = delete;
    StateContentProvider(StateContentProvider&& other) noexcept
        : api_(other.api_), handle_(std::exchange(other.handle_, 0)), target_(other.target_), thread_(other.thread_) { other.api_ = {}; }
    StateContentProvider& operator=(StateContentProvider&&) = delete;
    ~StateContentProvider() { Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    const MmsUiTargetRefV1& target() const noexcept { return target_; }
    MmsResult Update(const MmsUiStateContentDescV1& desc) noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.updateProvider(api_.context, handle_, &desc); });
        if (!result) target_ = desc.target;
        return result;
    }
    MmsResult Refresh(const RefreshServices& refresh, RefreshTicket& ticket, bool layout = false, uint64_t key = 0) const noexcept {
        if (!handle_) return MMS_E_NOT_READY;
        return layout ? refresh.RefreshStateLayout(target_, ticket, key) : refresh.RefreshStateCurrent(target_, ticket, key);
    }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.unregisterProvider(api_.context, handle_); });
        if (result && result != MMS_E_NOT_FOUND) return result;
        handle_ = 0; api_ = {}; target_ = {}; return MMS_OK;
    }
    MmsHandle detach() noexcept { api_ = {}; target_ = {}; return std::exchange(handle_, 0); }
};
class StateContentServices {
    MmsUiRefreshStateContentApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    static MmsResult Query(const MmsHostV1& host, StateContentServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiRefreshStateContentApiV1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.refresh.state"), 1, 0, uint32_t(sizeof(table)), &table);
            if (result) return result;
            if (!detail::TableHeader(table)) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.registerProvider || !table.updateProvider || !table.unregisterProvider || !table.writePrepared)
                return MMS_E_UNSUPPORTED;
            StateContentServices candidate; candidate.api_ = table; candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
            out = candidate; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    MmsResult CheckThread() const noexcept {
        return !ready_ ? MMS_E_NOT_READY : thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    const MmsUiRefreshStateContentApiV1& api() const noexcept { return api_; }
    MmsResult Capabilities(MmsUiStateContentCapabilitiesV1& out) const noexcept {
        if (const auto check = CheckThread()) return check;
        MmsUiStateContentCapabilitiesV1 next{}; next.h = Header<MmsUiStateContentCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &next); });
        if (!result) out = next;
        return result;
    }
    MmsResult Register(const MmsUiStateContentDescV1& desc, StateContentProvider& out) const noexcept {
        if (const auto check = CheckThread()) return check;
        if (out) return MMS_E_BUSY;
        MmsHandle handle = 0;
        const auto result = detail::Call([&] { return api_.registerProvider(api_.context, &desc, &handle); });
        if (!result) { out.api_ = api_; out.handle_ = handle; out.target_ = desc.target; out.thread_ = thread_; }
        return result;
    }
    MmsResult Register(const MmsUiTargetRefV1& target, uint32_t fields, MmsUiStateContentCallbackV1 prepare,
        void* context, StateContentProvider& out) const noexcept {
        MmsUiStateContentDescV1 desc{}; desc.h = Header<MmsUiStateContentDescV1>();
        desc.target = target; desc.fields = fields; desc.prepare = prepare; desc.context = context;
        return Register(desc, out);
    }
    MmsResult Write(MmsHandle builder, const MmsUiStateContentV1& content) const noexcept {
        if (const auto check = CheckThread()) return check;
        return detail::Call([&] { return api_.writePrepared(api_.context, builder, &content); });
    }
    // This typed overload requires the complete 80-byte structure, including
    // text-only writes. The original Write keeps accepting the 64-byte prefix.
    // Scalar bits are INPUT-only; target/owner checks stay in the host builder.
    MmsResult WriteInput(MmsHandle builder, const MmsUiInputStateContentV1& input) const noexcept {
        if (const auto check = CheckThread()) return check;
        const auto& content = input.content;
        if (content.h.size < sizeof(input)) return MMS_E_ARGUMENT;
        if (content.h.major != 1 || content.h.minor != 0) return MMS_E_VERSION;
        constexpr uint32_t fields = MMS_UI_STATE_LABEL | MMS_UI_STATE_DESCRIPTION | MMS_UI_STATE_DISABLED_REASON |
            MMS_UI_STATE_ENABLED | MMS_UI_STATE_READ_ONLY | MMS_UI_STATE_VISIBLE;
        if ((content.fields & ~fields) || content.reserved || input.reserved ||
            input.enabled > 1 || input.readOnly > 1 || input.visible > 1 ||
            (!(content.fields & MMS_UI_STATE_ENABLED) && input.enabled) ||
            (!(content.fields & MMS_UI_STATE_READ_ONLY) && input.readOnly) ||
            (!(content.fields & MMS_UI_STATE_VISIBLE) && input.visible)) return MMS_E_ARGUMENT;
        MmsUiStateContentCapabilitiesV1 capabilities{};
        if (const auto result = Capabilities(capabilities)) return result;
        if (content.fields & ~capabilities.supportedFields) return MMS_E_UNSUPPORTED;
        return detail::Call([&] { return api_.writePrepared(api_.context, builder, &content); });
    }
    // The host copies before return. Stack strings and temporary UTF-8 buffers
    // are valid here; the builder can only be used inside its prepare callback.
    MmsResult Write(MmsHandle builder, std::string_view label, std::string_view description, std::string_view disabledReason) const noexcept {
        MmsUiStateContentV1 content{}; content.h = Header<MmsUiStateContentV1>();
        content.fields = MMS_UI_STATE_LABEL | MMS_UI_STATE_DESCRIPTION | MMS_UI_STATE_DISABLED_REASON;
        content.label = {label.data(), uint64_t(label.size())};
        content.description = {description.data(), uint64_t(description.size())};
        content.disabledReason = {disabledReason.data(), uint64_t(disabledReason.size())};
        return Write(builder, content);
    }
    MmsResult WriteInput(MmsHandle builder, std::string_view label, std::string_view description, std::string_view disabledReason,
        std::optional<bool> enabled = {}, std::optional<bool> readOnly = {}, std::optional<bool> visible = {}) const noexcept {
        MmsUiInputStateContentV1 input{}; input.content.h = Header<MmsUiInputStateContentV1>();
        input.content.fields = MMS_UI_STATE_LABEL | MMS_UI_STATE_DESCRIPTION | MMS_UI_STATE_DISABLED_REASON;
        input.content.label = {label.data(), uint64_t(label.size())};
        input.content.description = {description.data(), uint64_t(description.size())};
        input.content.disabledReason = {disabledReason.data(), uint64_t(disabledReason.size())};
        if (enabled.has_value()) { input.content.fields |= MMS_UI_STATE_ENABLED; input.enabled = *enabled ? 1u : 0u; }
        if (readOnly.has_value()) { input.content.fields |= MMS_UI_STATE_READ_ONLY; input.readOnly = *readOnly ? 1u : 0u; }
        if (visible.has_value()) { input.content.fields |= MMS_UI_STATE_VISIBLE; input.visible = *visible ? 1u : 0u; }
        return WriteInput(builder, input);
    }
};
}
