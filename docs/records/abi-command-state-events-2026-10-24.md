# 业务事件：命令状态变化

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `518d715`。本批接入 `COMMAND_STATE_CHANGED`。

## 来源行为

- 宿主每次布局后都会读取插件命令的 `getState`（启用、选中、可见）。当任一已挂载命令的这三项相对上一次观察发生变化时，其他消费者应当收到通知。
- 通知必须在插件回调之外、布局完成之后投递，负载带当前 UI 修订号。

## ABI 实现

- [native_ui.hpp](../../src/plugins/native_ui.hpp)、[native_ui.cpp](../../src/plugins/native_ui.cpp)：新增 `SetCommandStateChanged` 与 `DetectCommandStateChanges`。后者遍历注册表中带 `commandId` 的元素，调用已有 `State` 取启用/选中/可见，与上次快照比较；任一变化时以注册表修订号调用回调一次。
- [integration.cpp](../../src/plugins/integration.cpp)：为 `NativeUi` 安装回调，把变化写入既有业务事件队列（`MmsRevisionEventV1` 版本 1），并在每次布局后调用检测。
- lifecycle 探针：新增 `COMMAND_STATE_CHANGED` 负载校验与日志；id 含 `gui-state` 时命令被调用后选中位递增，用于确定性地制造状态变化。
- [business_event_tests.cpp](../../tests/plugins/business_event_tests.cpp)：真实 `Manager` + `NativeUi` + lifecycle 插件；注册命令与工具栏按钮、记录基线、调用命令改变选中位、再次检测，并断言回调触发、事件到达插件、负载无失败。

## 验证

证据目录 `build/verification/abi-host-events-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `plugin_abi_business_events` | 通过，12 项：注册表/布局事件、命令状态检测回调、事件投递与插件日志 `EVENT_COMMAND_STATE`、无负载失败 |
| `plugin_abi_host_events` | 通过 4 项（非 GUI 探针，避免测试窗口缺少真实主界面控件） |
| 相关 CTest | 10 个模式全部通过，8.8 秒 |
| 默认全量契约套件 | 通过，`PASS checks=44752`，23.3 秒 |
| 完整产品 | 构建通过，EXE SHA-256 `bb121beba7c823ad7d176943b4da9f2fe2712b78f6898b2f1a345bc202fbe19f` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `5eaf8da4655dfd7eba2560479d390c2c29860cb9d2dd820383849998d14cb947` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 检测只覆盖注册表里带 `commandId` 的元素；菜单/工具栏真实宿主中的状态抖动未做端到端运行验证。每次布局会额外调用一次 `getState`，未测量性能影响。
- 把非 GUI 探针改为 GUI 探针时，真实隐藏窗口初始化后立即崩溃（`GUI_REGISTERED` 之后）；原因未隔离，因此命令状态验证放在 Manager + NativeUi 的独立探针里，不据此声称真实主窗口插件 GUI 路径可用。
- 仍缺 `PROPERTY_CHANGED`、`COMPUTE_POLICY_CHANGED`；DPI 消息来源、真实设备丢失与外部设备验收仍未完成。

## 后续

下一批评估属性变更事件与宿主级 DPI 检测，并单独排查真实主窗口加载 GUI 插件后的崩溃。
