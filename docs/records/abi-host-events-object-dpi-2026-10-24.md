# 宿主侧业务事件：对象删除与 DPI 变化

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `16e634e`。本批继续 P1 业务事件，接入 `OBJECT_REMOVED` 与 `DPI_CHANGED`。

## 对象删除

- [plugin_storage.hpp](../../include/mikumikustudio/plugin_storage.hpp)：新增 `PluginNotifyObjectRemoved(MMDApp*, const void* native)`。没有插件运行时或找不到稳定身份时为空操作。
- [integration.cpp](../../src/plugins/integration.cpp)：通知时通过 `FindSceneObjectIdentity` 取当前对象的稳定 ID 与代次，填入 `MmsEventV1.object`；业务事件队列与投递通道沿用上一批。
- [manager.hpp](../../src/plugins/manager.hpp)、[manager.cpp](../../src/plugins/manager.cpp)：`Event` 增加可选 `MmsObjectRef` 参数并写入事件对象字段。
- 宿主调用点：[command_control_400.cpp](../../src/window/command_control_400.cpp) 模型删除在 `DeleteModel` 前通知；[command_control_450.cpp](../../src/window/command_control_450.cpp) 附件删除在 `DeleteAccessory` 前通知（原始兼容配置编译守卫）。
- 探针用 `SceneObjectIdentity` 创建一个稳定身份后调用同一宿主入口，验证事件对象字段与负载契约。

## DPI 变化

- [integration.cpp](../../src/plugins/integration.cpp)：插件运行时窗口子类处理 `WM_DPICHANGED`，取 `HIWORD(wParam)` 的 DPI 通过 `PluginNotifyDpiChanged` 入队；负载为 `MmsCodeEventV1`（版本 1，DPI 值非零）。
- 探针直接调用同一宿主入口，确定性地验证入队、投递与负载。把合成 `WM_DPICHANGED` 发送到探针的普通 STATIC 窗口时，该消息未在探针中触发子类路径，因此消息到通知这一段仍未端到端验证；生产主窗口当前也没有独立的 DPI 变化处理，系统 DPI 感知模式下该消息是否送达需要另做宿主级检测。

## 验证

证据目录 `build/verification/abi-host-events-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `plugin_abi_host_events` | 通过 4 项：真实窗口、真实 lifecycle 插件、全部事件到达且负载合法、无负载失败 |
| 插件日志 | `NEGOTIATION_PASS`、`EVENT_UI_REGISTRY`、`EVENT_LANGUAGE`、`EVENT_DEVICE_LOST`、`EVENT_DEVICE_RESTORED`、`EVENT_PROJECT_CLOSED`、`EVENT_OBJECT_REMOVED`、`EVENT_DPI`、`UI_CLOSING`、`STOP_OK`、`DESTROY` |
| 相关 CTest | 10 个模式全部通过，合计 8.2 秒 |
| 默认全量契约套件 | 通过，`PASS checks=44752`，22.4 秒，exit 0 |
| 完整产品 | 构建通过，EXE SHA-256 `69b40960a6297678bfaf44cd89148095b17e39fdf2ab116f8fd8653e1187a6f0` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `ea3d93c10c22e6e60e62df870902387c69d7124fa8366922227797f3e83ab674` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成

- 仍未接入 `PROPERTY_CHANGED`、`COMMAND_STATE_CHANGED`、`COMPUTE_POLICY_CHANGED`。
- 对象删除的真实菜单点击/确认框未端到端触发；探针直接调用宿主入口。DPI 的消息来源仍未端到端验证，宿主级 DPI 检测未实现。
- 设备丢失/恢复的真实故障注入、Win7/AMD、真实 MME 与多显示器未验收。
