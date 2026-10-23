# P1 业务事件通知：UI 注册表与布局变更

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `45e6327`。本批补 P1「全部业务事件通知」中由插件运行时自身负责的两个来源：`MMS_EVENT_UI_REGISTRY_CHANGED` 与 `MMS_EVENT_LAYOUT_CHANGED`。其余业务来源仍列在下方。

## 来源行为

- 注册表变更（命令、元素、属性、布局偏好等）会推进 UI 修订号；布局事务提交成功也会推进同一修订号。
- 这两个事件必须在变更完成之后、注册表回调之外投递：插件在 `onEvent` 中读取快照时不能看到半提交的布局，也不能重入注册表回调。
- 事件负载使用 `MmsRevisionEventV1`（版本 1，包含最终修订号）。

## ABI 实现

- [ui_registry.hpp](../../src/plugins/ui_registry.hpp)：`UiRegistry` 记录可选的布局变更回调 `SetLayoutChanged`；提交成功并 `Changed()` 后以最终修订号调用。修订号读取沿用已有公开 `Revision()`。
- [ui_registry.cpp](../../src/plugins/ui_registry.cpp)：只在布局事务完整应用、偏好文档已验证之后触发布局回调；失败或提前返回不触发。
- [manager.cpp](../../src/plugins/manager.cpp)：注册表原来的宿主变更通知被包一层，只置位「待投递」标记；布局回调只记录修订号。`Drain` 在 UI 线程、任务批处理之后、注册表回调之外合并投递一次 `UI_REGISTRY_CHANGED` 和一次 `LAYOUT_CHANGED`，负载为最终修订号。这样同一轮多次变更只产生一条事件，且不会在插件注册回调里重入。
- [lifecycle_plugin.cpp](../../tests/plugins/lifecycle_plugin.cpp)：事件探针校验负载版本和大小、修订号非零，并记录 `EVENT_UI_REGISTRY`／`EVENT_LAYOUT`；负载错误记录 `REVISION_PAYLOAD_FAIL`／`REVISION_ZERO_FAIL`。
- 新增独立宿主探针 [business_event_tests.cpp](../../tests/plugins/business_event_tests.cpp)：加载真实 lifecycle 插件，提交一个空布局事务，执行 `Drain`，断言两条事件各一条且负载合法。使用独立目标而非 `plugin_abi_tests`，避免被该目标既有的编译问题阻塞。

## 验证

证据目录 `build/verification/abi-business-events-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `business_event_tests` | 通过，7 项检查：插件协商成功、空布局提交后投递 `EVENT_UI_REGISTRY` 至少一次、`EVENT_LAYOUT` 恰好多一次、负载版本/大小/修订号合法、无负载失败标记、插件正常停止 |
| 完整产品 | 构建通过，EXE SHA-256 `d52ba22974efdb898c012fd4ac19c3809358e891446313cbd0b47ba2f8fae4e8` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `508ec620ff6b9b0546efa36d3ddd2524140b6237306c5b34d85a6d0fd3d25f60` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成

P1「全部业务事件通知」仍有以下来源未接入：`PROJECT_CLOSED`、`OBJECT_REMOVED`、`DEVICE_LOST`、`DEVICE_RESTORED`、`LANGUAGE_CHANGED`、`PROPERTY_CHANGED`、`COMPUTE_POLICY_CHANGED`、`COMMAND_STATE_CHANGED`、`DPI_CHANGED`。这些事件需要宿主窗口/设备/工程生命周期接线，其中设备丢失与 DPI 还需要可重复的真实触发环境；本批不声称它们可用。

## 发现的既有问题

尝试重建 `plugin_abi_tests` 时，`tests/plugins/storage_checks.inc` 的 `PublishProjectPair` 调用传 7 个参数，而 `src/plugins/project_storage.hpp` 只声明 6 个参数，目标无法编译。该问题与本批事件改动无关，本轮未修；新增探针改用独立目标。后续需单独核对存储 API 与测试调用。

## 后续

下一批接入宿主侧事件来源：工程关闭、对象删除、语言切换与设备代次/设备丢失恢复；同时修复上述既有测试编译问题。
