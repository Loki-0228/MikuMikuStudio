// 自动刷新计数示例：命令发布整数，按钮的 checked 取自已消费的 DATA 显示模型。
#include <mms/cpp/refresh_subscription.hpp>
#include <mms/cpp/ui.hpp>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

using mms::sdk::Header;
using mms::sdk::Text;

namespace {

constexpr char kPluginId[] = "org.example.auto-refresh";
constexpr char kSourceId[] = "org.example.auto-refresh.count";
constexpr char kCommandId[] = "org.example.auto-refresh.publish";
constexpr char kMenuId[] = "org.example.auto-refresh.menu";
constexpr char kButtonId[] = "org.example.auto-refresh.toolbar";
constexpr std::uint64_t kModelBytes = 64;
constexpr std::uint32_t kRefreshKinds =
    MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT;

// 所有 C ABI 入口共用同一条异常边界。
template <class F>
MmsResult Guard(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}

struct Plugin {
    MmsHostV1 host{};
    mms::sdk::DataServices data;
    mms::sdk::RefreshServices refresh;
    mms::sdk::RefreshSubscriptionServices subscriptions;
    mms::sdk::CommandUi controls;
    mms::sdk::DataSource source;
    mms::sdk::DataBinding binding;
    mms::sdk::RefreshSubscription subscription;
    std::uint32_t count = 0;
    bool ready = false;
    bool stopping = false;

    Plugin(const MmsHostV1& hostTable, mms::sdk::DataServices dataServices,
        mms::sdk::RefreshServices refreshServices,
        mms::sdk::RefreshSubscriptionServices subscriptionServices,
        mms::sdk::UiServices uiServices) noexcept
        : host(hostTable), data(std::move(dataServices)), refresh(std::move(refreshServices)),
          subscriptions(std::move(subscriptionServices)), controls(std::move(uiServices)) {}
};

void Log(const Plugin& plugin, const char* message) noexcept {
    if (!plugin.host.log) return;
    try { plugin.host.log(plugin.host.context, 2u, Text(message)); } catch (...) {}
}

// 逆序清理：订阅、绑定、来源。失败时停在原对象，不继续释放后续句柄。
MmsResult Cleanup(Plugin& plugin) noexcept {
    if (plugin.subscription) {
        if (const auto result = plugin.subscription.Release()) return result;
    }
    if (plugin.binding) {
        if (const auto result = plugin.binding.Release()) return result;
    }
    if (plugin.source) {
        if (const auto result = plugin.source.Release()) return result;
    }
    return MMS_OK;
}

// 只能在 UI 任务中调用：命令回调是只读边界，不能注册来源或绑定。
MmsResult Initialize(Plugin& plugin) noexcept {
    if (const auto residual = Cleanup(plugin)) return residual;
    const auto button = plugin.controls.button();
    if (!button) return MMS_E_NOT_READY;
    MmsUiRefreshTargetV1 target{};
    auto result = plugin.refresh.Target(button.handle, target);
    if (result) return result;
    if (!target.ready) return MMS_E_NOT_READY;
    result = plugin.data.RegisterSource(Text(kSourceId), kModelBytes, plugin.source);
    if (result) return result;
    result = plugin.data.Bind(target.target, plugin.source, kModelBytes, plugin.binding);
    if (!result) result = plugin.subscriptions.Subscribe(plugin.binding, plugin.subscription, kRefreshKinds);
    if (result) {
        // 组合失败时不留下半个来源或绑定；清理失败时由 stop 继续重试。
        if (const auto cleanup = Cleanup(plugin)) return cleanup;
        return result;
    }
    plugin.ready = true;
    return MMS_OK;
}

MmsResult PublishNext(Plugin& plugin) noexcept {
    if (!plugin.ready || !plugin.source || !plugin.binding || !plugin.subscription) return MMS_E_NOT_READY;
    // 订阅随目标重建失效时先走重新绑定；正常情况只发布递增计数。
    MmsUiRefreshSubscriptionStateV1 state{};
    const auto alive = plugin.subscription.State(state);
    if (alive) return alive;
    MmsUiDataSourceStateV1 current{};
    auto result = plugin.source.State(current);
    if (result) return result;
    const std::uint32_t next = plugin.count + 1u;
    MmsUiDataSourceStateV1 published{};
    result = plugin.source.Publish(MmsBytes{&next, sizeof(next)}, current.revision, published);
    if (!result) plugin.count = next;
    return result;
}

// 首次正常 invoke 排队的 UI 任务：延迟绑定到原按钮目标，然后发布。
void MMS_CALL RunTask(void* context) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || plugin->stopping) return;
    (void)Guard([&]() -> MmsResult {
        if (!plugin->ready) {
            if (const auto initialized = Initialize(*plugin)) {
                Log(*plugin, "自动刷新：初始化未完成");
                return initialized;
            }
        }
        auto published = PublishNext(*plugin);
        if (published == MMS_E_STALE && !plugin->stopping) {
            // 工程代次或目标失效后重新绑定；旧句柄由 Cleanup 按序释放。
            plugin->ready = false;
            if (const auto initialized = Initialize(*plugin)) {
                Log(*plugin, "自动刷新：重新绑定失败");
                return initialized;
            }
            published = PublishNext(*plugin);
        }
        if (published) Log(*plugin, "自动刷新：发布失败");
        return published;
    });
}

void MMS_CALL CancelTask(void*) {}

MmsResult MMS_CALL GetState(void* context, MmsObjectRef, MmsCommandStateV1* out) noexcept {
    if (!context || !out || out->h.size < sizeof(*out)) return MMS_E_ARGUMENT;
    *out = {};
    out->h = Header<MmsCommandStateV1>();
    out->enabled = 1u;
    out->visible = 1u;
    auto& plugin = *static_cast<Plugin*>(context);
    if (plugin.stopping || !plugin.ready || !plugin.binding) return MMS_OK;
    return Guard([&]() -> MmsResult {
        // 只读取已消费的不可变显示值；未准备时保持未选中。
        mms::sdk::DataDisplay display;
        if (plugin.binding.Acquire(display)) return MMS_OK;
        std::uint32_t value = 0;
        const auto bytes = display.bytes();
        if (bytes.data && bytes.bytes == sizeof(value)) std::memcpy(&value, bytes.data, sizeof(value));
        out->checked = value & 1u;
        return display.Release();
    });
}

MmsResult MMS_CALL Invoke(void* context, MmsObjectRef, MmsSpan args, MmsHandle tx) noexcept {
    if (!context || args.count != 0 || tx != 0) return MMS_E_ARGUMENT;
    auto& plugin = *static_cast<Plugin*>(context);
    return Guard([&]() -> MmsResult {
        if (plugin.stopping) return MMS_E_NOT_READY;
        if (!plugin.ready) {
            // 命令回调内只能纯读目标；未就绪时直接返回，不留下注册。
            const auto button = plugin.controls.button();
            if (!button) return MMS_E_NOT_READY;
            MmsUiRefreshTargetV1 target{};
            const auto result = plugin.refresh.Target(button.handle, target);
            if (result) return result;
            if (!target.ready) return MMS_E_NOT_READY;
        }
        if (!plugin.host.enqueueUi) return MMS_E_UNSUPPORTED;
        MmsHandle task = 0;
        return plugin.host.enqueueUi(plugin.host.context, RunTask, CancelTask, &plugin, &task);
    });
}

MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) noexcept {
    if (!out) return MMS_E_ARGUMENT;
    *out = nullptr;
    if (!host || !info || host->h.size < sizeof(*host) || info->h.size < sizeof(*info)) return MMS_E_ARGUMENT;
    if (host->h.major != 1 || info->h.major != 1) return MMS_E_VERSION;
    if (!host->queryService || !host->log || !host->enqueueUi) return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        mms::sdk::UiServices ui;
        auto result = mms::sdk::UiServices::Query(*host, ui);
        if (result) return result;
        mms::sdk::DataServices data;
        result = mms::sdk::DataServices::Query(*host, data);
        if (result) return result;
        mms::sdk::RefreshServices refresh;
        result = mms::sdk::RefreshServices::Query(*host, refresh);
        if (result) return result;
        mms::sdk::RefreshSubscriptionServices subscriptions;
        result = mms::sdk::RefreshSubscriptionServices::Query(*host, subscriptions);
        if (result) return result;
        auto* plugin = new (std::nothrow) Plugin(*host, std::move(data), std::move(refresh),
            std::move(subscriptions), std::move(ui));
        if (!plugin) return MMS_E_MEMORY;
        *out = plugin;
        return MMS_OK;
    });
}

MmsResult MMS_CALL Start(void* instance) noexcept {
    if (!instance) return MMS_E_ARGUMENT;
    auto& plugin = *static_cast<Plugin*>(instance);
    return Guard([&]() -> MmsResult {
        plugin.stopping = false;
        plugin.ready = false;
        plugin.count = 0;
        MmsUiLayoutHintsV1 hints{};
        hints.h = Header<MmsUiLayoutHintsV1>();
        hints.minimum[0] = 60;
        hints.minimum[1] = 24;
        hints.preferred[0] = 160;
        hints.preferred[1] = 24;
        hints.shrink[0] = 1;
        mms::sdk::CommandUiSpec spec;
        spec.id = kCommandId;
        spec.owner = kPluginId;
        spec.label = "自动刷新计数";
        spec.description = "发布递增计数；按钮勾选随已消费计数的奇偶自动刷新";
        spec.context = &plugin;
        spec.getState = GetState;
        spec.invoke = Invoke;
        spec.menu.id = kMenuId;
        spec.button.id = kButtonId;
        spec.button.hints = &hints;
        return plugin.controls.Register(spec);
    });
}

void MMS_CALL OnEvent(void*, const MmsEventV1*) noexcept {}

MmsResult MMS_CALL Stop(void* instance) noexcept {
    if (!instance) return MMS_E_ARGUMENT;
    auto& plugin = *static_cast<Plugin*>(instance);
    return Guard([&]() -> MmsResult {
        plugin.stopping = true;
        // 任一步失败即返回，保留剩余对象和 context 供宿主重试。
        if (const auto cleanup = Cleanup(plugin)) return cleanup;
        plugin.ready = false;
        return plugin.controls.Close();
    });
}

void MMS_CALL Destroy(void* instance) noexcept {
    try { delete static_cast<Plugin*>(instance); } catch (...) {}
}

} // namespace

extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
    uint32_t outBytes, MmsPluginApiV1* out) {
    if (!out || outBytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor != 0) return MMS_E_VERSION;
    const MmsPluginApiV1 api{Header<MmsPluginApiV1>(), Create, Start, OnEvent, Stop, Destroy};
    std::memcpy(out, &api, sizeof(api));
    return MMS_OK;
}
