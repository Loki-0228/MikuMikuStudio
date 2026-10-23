# 宿主侧业务事件：工程关闭、语言切换、设备丢失/恢复

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `ec70974`。本批在上一批运行时事件（UI 注册表/布局）之外，接入四个宿主生命周期事件：`PROJECT_CLOSED`、`LANGUAGE_CHANGED`、`DEVICE_LOST`、`DEVICE_RESTORED`。

## 来源行为

- 工程关闭：工程上下文结束（新建/重置工程）时通知插件，负载沿用工程事件结构。
- 语言切换：菜单命令 260 在日文→英文→中文循环后触发语言扫描，同时通知插件新语言值。
- 设备丢失：Present 返回 `D3DERR_DEVICELOST` 时进入设备丢失等待前通知插件。
- 设备恢复：`PostDeviceReset` 成功完成资源重建后通知插件；宿主维护单调递增的设备代次。

## ABI 实现

- [plugin_storage.hpp](../../include/mikumikustudio/plugin_storage.hpp)：新增 `PluginNotifyProjectClosed`、`PluginNotifyLanguageChanged`、`PluginNotifyDeviceLost`、`PluginNotifyDeviceRestored`。没有插件运行时拥有该应用时为空操作。
- [integration.cpp](../../src/plugins/integration.cpp)：新增通用业务事件队列。通知在宿主动作线程入队，经同一条 posted message 在安全边界（非插件回调、窗口布局就绪、无场景变更）投递给插件；队列非空且边界不满足时按 16 ms 定时重试。负载：
  - 工程关闭：`MmsProjectEventV1`（版本 1，工程 ID 与路径）；
  - 语言切换：`MmsCodeEventV1`（版本 1，语言 0／1／2）；
  - 设备丢失/恢复：`MmsDeviceEventV1`（版本 1，设备代次；恢复时代次先递增）。
- 调用点：[command_view_menu.cpp](../../src/window/command_view_menu.cpp) 命令 260、[integration.cpp](../../src/plugins/integration.cpp) 的 `ResetPluginProject`、[frame_driver.cpp](../../src/app/frame_driver.cpp) 的 `D3DERR_DEVICELOST` 分支、[device_reset.cpp](../../src/render/device_reset.cpp) 的 `PostDeviceReset` 成功返回前。
- 插件包目录新增仅测试用的 `MMS_PLUGIN_TEST_DIR` 覆盖；生产仍固定解析可执行文件旁的 `PLUGINS`。

## 验证

证据目录 `build/verification/abi-host-events-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 新宿主探针 `plugin_abi_host_events` | 通过：真实窗口上初始化生产插件运行时、加载真实 lifecycle 插件；`PASS hidden production window`、`PASS real lifecycle plugin started`、`PASS queued host events reach the plugin with valid payloads`、`PASS no event payload validation failures` |
| 插件日志 | `NEGOTIATION_PASS`、`EVENT_UI_REGISTRY`、`EVENT_LANGUAGE`、`EVENT_DEVICE_LOST`、`EVENT_DEVICE_RESTORED`、`EVENT_PROJECT_CLOSED`、`UI_CLOSING`、`STOP_OK`、`DESTROY` |
| 注册 CTest | 10 个相关模式全部通过（含 host events、business events 与上一批 8 个模式），合计 8.9 秒 |
| 默认全量契约套件 | 通过，`PASS checks=44752`，23.0 秒，exit 0 |
| 完整产品 | 构建通过，EXE SHA-256 `aec98e53a2b72465183456762b4586d6ddc03330f551b7aa2c7f89a2ba75ea49` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `e5db93d292c5f88f55f1cb04f30298656ac9b27f440b0e93f856f508721b68d9` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 探针直接调用宿主通知入口，验证了入队、边界投递、负载与插件接收；真实设备丢失/重置与真实语言菜单点击未在本机端到端触发。`DEVICE_LOST` 的真实来源是 Present 失败分支，未做故障注入。
- 设备代次目前只由本集成层的丢失/恢复通知维护，P5 的宿主级渲染设备代次与资源租约仍未实现。
- 仍未接入的 P1 事件：`OBJECT_REMOVED`、`PROPERTY_CHANGED`、`COMMAND_STATE_CHANGED`、`COMPUTE_POLICY_CHANGED`、`DPI_CHANGED`。当前主窗口没有 `WM_DPICHANGED` 处理路径，DPI 变化需要先建立宿主级检测。
- Win7/AMD、真实 MME 与多显示器环境未验收。

## 后续

下一批接入对象删除与属性/命令状态事件，并评估宿主级 DPI 变化检测；随后重跑默认契约套件与相关 CTest。
