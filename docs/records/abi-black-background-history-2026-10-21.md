# 黑白背景共同历史验证（2026-10-21）

`codex/plugin-abi` 的菜单 `282` 和 `B` 键已接入受控共同历史。两条入口修改同一个工程字段 `MMDAppState::blackBackgroundEnabled`，与模型编辑、插件事务共用 `SharedHistory`。本批未开放普通公共 `mms.edit`，未更新正式 release。

## 原生入口与记录模式

[command_view_menu.cpp](../../src/window/command_view_menu.cpp) 的 `case 282` 先调用 `TryToggleBoundBlackBackground`。[key_ladder.cpp](../../src/app/key_ladder.cpp) 的 `DispatchLetterHotkeys` 在原有焦点和按键条件通过后调用同一接口。原生 EDIT 和已标记插件 INPUT 子树继续排除字母快捷键，测试直接提供按键快照，不注入全局键鼠输入。

两条入口均从当前作者值决定切换方向：零变为 `1`，任意非零变为 `0`。内部 C++ 接口 `ApplyBoundBlackBackground(MMDApp&, bool, bool&)` 接受显式布尔目标。已经处于相同显示状态时，该接口保留原始字节，不增加历史、不标记工程修改、不清除 redo。例如，原值为 `7` 时设置启用仍保留 `7`。

`TryToggleBoundBlackBackground` 在存在内部历史绑定时始终消费该入口。播放、模态编辑、历史忙碌、插件事务 pending、预算不足或准备失败均不回落旧写入。拒绝后从作者值恢复菜单勾选。未绑定路径和 `MMD_ORIGINAL_PROFILE` 保留旧菜单与字母键逻辑。

记录按 `NativeIntent::Adjustment` 提交，两种历史模式如下。

| 历史模式 | 实际变化的行为 |
| --- | --- |
| `AllOperations` | 发布一个背景字段修改，增加一个共同历史节点。 |
| `ExplicitOnly` | 发布背景字段修改并推进工程版本，不增加节点，保留已有 redo。 |

同值操作在两种模式下均不发布修改。切换没有独立确认对话框，已经提交的变化通过共同历史撤销。

## 字节保存与标准 PMM

[native_global_commands.cpp](../../src/plugins/native_global_commands.cpp) 的 `ProjectDisplayChange` 扩为四个来源字段：坐标显示、地面投影、投影透明混合和黑白背景。每条记录只写本次字段，不覆盖其他后来修改的显示值。背景记录保留原始字节，因此撤销可以恢复 `7`、`255` 等非规范非零值。

原始字节的保留范围是内存中的共同历史。标准 PMM 序列化保持原有规则：[pmm_save.cpp](../../src/io/pmm_save.cpp) 将 `blackBackgroundEnabled != 0` 写成一个 `0` 或 `1` 字节；[pmm_load_v2.cpp](../../src/io/pmm_load_v2.cpp) 读取同一个字节并归一为 `0` 或 `1`。旧版读取路径 [pmm_load_v1.cpp](../../src/io/pmm_load_v1.cpp) 也按布尔值归一。本批未修改 PMM 字段长度、位置或相邻字段，未增加背景扩展数据。

PMM 结论来自保存与加载源码核对。本批背景专项未新增 PMM 文件往返测试，不能将内存中精确恢复原始字节等同于保存后仍保留该非规范字节。

## 发布、绘制与共同撤销

短期候选核对工程相机键池身份、当前帧、四个显示字段及记录代次。正常相机键池扩容保留身份，替换键池使旧记录过期，拒绝时不移动历史游标。每次撤销或重做重新准备候选，长期记录不依赖当前模型选择、相机模式或播放头。

发布只写背景字节和内部记录状态，不分配内存，不调用窗口、插件、文件或渲染器。所有准备和预算检查都在发布前完成。背景记录的 `RequiresPhysicsReset()` 返回 `false`。

首次提交完成后，宿主标记工程修改，更新共同撤销按钮和菜单，再调用 `updates::RequestViewport(&app, false)`。该接口只追加 `RenderDirty | PresentDirty`，不追加 `EvaluateDirty`，也不推进几何版本。首次背景提交不调用 `PublishBoundPoseEdit`、`PostViewRefresh` 或 seek，不修改正式姿态、关键帧、播放头和物理重置标志。该结论描述背景入口自身的请求，不排除帧驱动器处理其他已经存在的求值请求。

共同撤销与重做继续走 `TryBoundHistoryCommand` 或 `RunBoundObjectHistory`。既有收尾调用 `PublishBoundPoseEdit`，其中允许 `PostViewRefresh` 读取并刷新界面。背景专项通过真实共同历史 UI 路由检查模型模式、相机模式和后来未登记的姿态，确认该收尾保留姿态、播放头、其他显示字段及既有 `PhysicsResetPending`。首次提交的“不调用 `PostViewRefresh`”断言不扩展到共同撤销的通用收尾。

## 拒绝、异常与混合历史

专项覆盖菜单和 `B` 键的预算拒绝、逐点 C++ 分配失败、播放及模态互斥、历史忙碌、工程来源替换，以及失败后的菜单恢复。失败保持原始背景字节、已有 undo 和 redo、保留字节数及修改标志。成功发布后的界面通知异常不推翻数据修改，也不将已经提交的变化报告成失败。

插件混合用例创建真实 `EditStore` 事务和 serializer。事务 pending 时，`CheckNativeEdit()` 返回 `MMS_E_BUSY`；`B` 键不改变背景、共同历史节点数、保留字节数或工程版本，也不取消该插件事务。原事务随后仍须完成 `join`、`prepare` 和 `commit`。

混合顺序为模型 A 数值编辑、菜单背景切换、插件私有状态、模型 B 数值编辑、`B` 键背景切换。五个节点逐一撤销和重做，检查模型姿态、插件状态及背景值按实际顺序恢复。原文件不参与这些记录的重放。

故障注入范围是这些共同历史准备路径中的 C++ 分配，以及转发真实绘制请求后抛出的通知异常。该范围不包含系统分配器、GPU 绘制或所有窗口通知的故障。

## 本机验收与夹具修复

下列证据均位于 `build/verification/abi-panel-background-lifetime-20261021`。专项参数及计数保存在 [native-final-02/report.json](../../build/verification/abi-panel-background-lifetime-20261021/native-final-02/report.json)，可据其中 `command` 字段复核对应测试入口。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 完整配置产品构建 | 退出码 `0`。 | [build-full-02.log](../../build/verification/abi-panel-background-lifetime-20261021/build-full-02.log) |
| 原始配置产品构建 | 退出码 `0`，保留旧入口。 | [build-original-01.log](../../build/verification/abi-panel-background-lifetime-20261021/build-original-01.log) |
| 原生专项目标重建 | 退出码 `0`。 | [build-native-full-04.log](../../build/verification/abi-panel-background-lifetime-20261021/build-native-full-04.log) |
| `--black-background-only` | `1463` 项通过。 | [黑背景日志](../../build/verification/abi-panel-background-lifetime-20261021/native-final-02/0-black-background-only/result.log) |
| `--grid-display-only` | `492` 项通过。 | [坐标显示日志](../../build/verification/abi-panel-background-lifetime-20261021/native-final-02/1-grid-display-only/result.log) |
| `--projected-shadow-only` | `540` 项通过。 | [投影开关日志](../../build/verification/abi-panel-background-lifetime-20261021/native-final-02/2-projected-shadow-only/result.log) |

首轮 [native-final-01/report.json](../../build/verification/abi-panel-background-lifetime-20261021/native-final-01/report.json) 在 `CrossModelAndPluginOrder` 的 pending 排他断言失败。原因是测试夹具打开了浮动窗口，但继承的数值 EDIT 建在主窗口。`CommitEditControlTail` 按浮动窗口查找输入框，两次模型编辑都未命中。失败点的背景值仍为 `1`，历史中实际只有一个背景节点，原断言误期望两个节点。

[native_black_background_checks.inc](../../tests/native_black_background_checks.inc) 现仅在该用例的数值提交期间临时选择主面板，提交后恢复浮动窗口上下文。新增前置断言检查模型 A、模型 B 的实际数值和节点数。pending 断言继续要求禁止原生写入，并追加工程版本、预算、菜单及事务仍存活的检查。未修改生产排他规则。修复后第二轮三个专项均通过。

本轮测试继承 `Default` 桌面，`SW_HIDE` 只是进程启动时的请求。隐藏夹具和窗口消息分派不构成真实独立桌面验收，也不等于鼠标、IME、多 DPI 或最终 GPU 画面的验收。原始配置的本轮证据是产品构建，不是上述黑背景专项的原始配置运行。

Win7 SP1 x64、CPU 基础路径和 AMD 兼容要求保持。该标量历史没有新增系统或显卡依赖，本轮构建与无渲染器专项不替代 Win7、AMD 真机验收。正式 release 未覆盖，未部署或推送。
