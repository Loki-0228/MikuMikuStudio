# 宿主探针日志隔离与 DPI 消息路径诊断

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批修复宿主事件探针可能被旧日志误导的问题，并诊断 DPI 消息来源。

## 日志隔离

- 问题：`host_event_tests` 只追加写 `MMS_ABI_PROBE_LOG`，CTest 的路径固定（`build/tests/host-events/plugin.log`）。之前运行留下的 `EVENT_DPI` 等标记可以让本次运行在事件没有真正到达时仍通过 `WaitFor` 检查。
- 修复：探针启动时删除自己的日志文件，所有标记断言都来自本次运行的输出。

## DPI 消息路径

- 此前探针直接调用 `PluginNotifyDpiChanged`，事件入队/投递/负载已验证，但「窗口收到 WM_DPICHANGED 后由运行时子类通知」这一段没有证据。
- 本批在探针改用生产 `InitMainWindowAndD3D` 后，向真实隐藏主窗口 `SendMessageW(WM_DPICHANGED, MAKELPARAM(144,144))`：日志没有出现 `EVENT_DPI`，其余事件正常；说明该消息在本夹具里没有到达运行时子类或未能触发通知。
- 探针恢复为直接调用同一宿主入口，保证事件契约与日志断言确定；DPI 消息来源仍未端到端验证。

## 验证

证据目录 `build/verification/abi-host-probe-final-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `plugin_abi_host_events` | 通过 4 项；新日志包含 `EVENT_UI_REGISTRY`、`EVENT_LANGUAGE`、`EVENT_DEVICE_LOST`、`EVENT_DEVICE_RESTORED`、`EVENT_PROJECT_CLOSED`、`EVENT_OBJECT_REMOVED`、`EVENT_DPI` 与正常停止 |
| 相关 CTest | 10 个模式全部通过，9.1 秒 |
| 默认全量契约套件 | 通过，`PASS checks=44752` |
| DPI 合成消息 | 不产生 `EVENT_DPI`（未计为通过）；与直接调用宿主入口区分记录 |

## 未完成

- WM_DPICHANGED 到运行时子类的路由需要单独诊断（子类安装返回值、消息是否被宿主窗口过程吞掉、是否只在真实 DPI 切换时投递）。在定位前不声称 DPI 消息来源已实现。
- 属性变更与计算策略事件仍未接入；真实桌面 DPI/多显示器验收仍待外部环境。
