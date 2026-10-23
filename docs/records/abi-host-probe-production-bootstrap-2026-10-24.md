# 宿主事件探针改用生产窗口启动

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批解决上一轮记录的真实隐藏窗口加载 GUI 插件崩溃问题，并把宿主事件探针改为生产启动。

## 现象与根因

- 现象：`host_event_tests` 用自建 `STATIC` 窗口初始化插件运行时，加载会注册菜单/工具栏按钮的 GUI 插件时，在 `GUI_REGISTERED` 之后进程访问冲突。
- 诊断：先确认崩溃发生在一次布局流程结束之后（`refresh:after-layout` 之后、初始化返回之前）。该探针没有创建生产主窗口与 D3D 设备，插件元素改变工具栏/属性区高度后，宿主走窗口尺寸与视口布局路径，缺失的生产窗口/设备状态导致崩溃。
- 结论：这是探针夹具限制，不是产品 GUI 插件路径缺陷。把探针改为调用生产 `InitMainWindowAndD3D` 后，同一个 GUI 插件加载、注册、投递事件并正常停止。

## 修改

- [host_event_tests.cpp](../../tests/plugins/host_event_tests.cpp)：不再自建 `STATIC` 窗口，改为设置 `g_Block`、`MMDApp::InitDefaults` 后调用生产 `InitMainWindowAndD3D(app, hInstance, SW_HIDE)`；使用真实主窗口与设备初始化插件运行时；退出时 `plugins::Shutdown` 后执行生产 `ShutdownCleanup`。
- [plugin_abi.cmake](../../cmake/plugin_abi.cmake)：宿主探针目标加入 `res/MMD.rc`、生成的嵌入式资源与 `MMD.def`，与其它真实宿主测试一致。
- 探针能力不变：仍加载 lifecycle 插件并验证工程关闭、语言、设备丢失/恢复、对象删除、DPI 事件与负载。

## 验证

证据目录 `build/verification/abi-host-bootstrap-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 非 GUI 探针（生产启动） | 通过：`hidden production window and device`、`production main window available`、插件启动、全部事件到达、无负载失败 |
| GUI 插件探针（同一二进制，插件 id 含 `gui`） | 通过：`NEGOTIATION_PASS`、`GUI_REGISTERED`、`EVENT_UI_REGISTRY`、`EVENT_LANGUAGE`、`EVENT_DEVICE_LOST`、`EVENT_DEVICE_RESTORED`、`EVENT_PROJECT_CLOSED`、`EVENT_OBJECT_REMOVED`、`EVENT_DPI`、`UI_CLOSING`、`STOP_OK`、`DESTROY` |
| 相关 CTest | 10 个模式全部通过，10.0 秒 |
| 产品 | 本批只改测试与测试目标，生产代码与两个产品二进制不变；正式 release 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成

- 探针窗口隐藏（`SW_HIDE`），没有真实桌面视觉、人工点击或多显示器验证。
- 事件探针仍直接调用宿主通知入口，真实菜单删除、设备丢失与 DPI 消息来源未端到端触发。
- 属性变更与计算策略事件仍未接入；透明排序性能缓存、真实 MME/MMD 工程与外部设备验收未完成。
