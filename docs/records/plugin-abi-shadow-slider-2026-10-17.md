# 自阴影距离滑杆共同历史迁移（2026-10-17）

本批在 `codex/plugin-abi` 的 `D:/MMD-Next/MMS-abi-dev` 中迁移主窗口自阴影距离滑杆 `560`。本记录描述本批实现、实际验证结果与未验收范围。普通 `mms.edit` 仍关闭。

## 接口与数值规则

`HandleHScroll` 在旧写入前调用 `TryCommitBoundShadowSlider(MMDApp*, HWND)`。已绑定共同历史时，该入口消费成功、拒绝和无操作，不回落旧写入。未绑定路径与原始兼容 profile 保留旧行为。这是宿主内部 C++ 接口，没有增加公共 C ABI。

入口只接收主窗口的 `kSelfShadowRangeSlider`，通过 `TBM_GETPOS` 获取整数刻度。刻度限制到 `0..9999` 后，距离按旧规则计算：

```cpp
float((10000.0 - position) / 100000.0)
```

例如，刻度 `4321` 对应距离 `0.05679`。文本 `561` 显示刻度，不直接显示作者距离。安全刷新通过既有 `ShadowSliderPosition` 和 `SyncSelfShadowControls` 更新文本、滑杆与模式按钮。撤销保存原始 float 距离，不从显示整数重建。

## 实时反馈与共同历史

滑杆复用距离文本和模式按钮使用的 `ShadowPoseChange`。每次有效位置变化立即提交一条共同历史，随后更新 dirty 标记及控件。整次拖动尚未合并为一个撤销节点。

相同位置的重复 `TB_LINEUP`、`TB_LINEDOWN`、`TB_THUMBPOSITION` 或 `TB_ENDTRACK` 通知不追加历史，不截断 redo，也不改变 dirty 标记和选择。成功修改按旧规则激活自阴影轨道。该轨道已选中时保留其他选中行，原先未选中时切换为自阴影轨道。

距离记录只恢复距离，保留后来设置的自阴影模式与播放头。插件编辑与滑杆编辑按同一时间顺序撤销和重做。发布阶段不求值关键帧，不修改模型旧撤销环、姿势、附件变换或自阴影键内容。UI 更新在发布完成后执行，不在历史发布阶段调用窗口或 GPU。

## 拒绝、回滚与来源身份

入口检查未完成插件事务、播放、录制、工程修改、保存、其他模态编辑、历史忙碌及不相关鼠标捕获。真实拖动需要保留滑杆自己的鼠标捕获，因此只有主窗口 `560` 控件可以使用该捕获例外。普通距离 API 不能借用此例外。

准备失败时，作者距离尚未修改，历史游标、redo、dirty 标记及轨道选择保持原值。已移动的滑杆拇指与文本恢复为当前作者值。失败不通过重放旧 UI 操作回滚，也不追加补偿历史。

记录检查自阴影键池身份。正常扩容保留身份，历史可继续往返。键池替换后，旧记录返回 `MMS_E_STALE`，不写作者值，也不移动历史游标。短期候选另检查帧号、当前距离、模式、记录代次与发布方向。

## 验证范围与结果

下表仅列本批实际执行的验证；不代表整个项目全部测试通过。

| 检查 | 覆盖范围 | 本轮结果 |
| --- | --- | --- |
| 构建 | 现有 Win7 x64 配置的宿主、原生历史与实际宿主测试目标。 | 通过。 |
| 原生专项 | `tests/native_shadow_slider_checks.inc` 对比实际未绑定 `WM_HSCROLL`，覆盖边界刻度、距离 float 位值、文本及滑杆读数。 | 通过，专项累计 1,206 项。三种模式另覆盖 line/page/top/bottom 与 thumb 通知的实际值变化。 |
| 历史与失败 | 每位置独立历史、重复通知、redo 保留、跨帧与模式往返、轨道选择、逐点 C++ 分配失败、自身与无关捕获、键池扩容及替换。 | 通过，包含准备阶段分配失败扫描。 |
| 实际宿主 | `tests/plugins/shadow_slider_host_checks.inc` 覆盖真实主窗口滑杆消息、插件事务拒绝、插件交错撤销、跨编辑模式重做与 PMM 保存。 | PMX 及中文改名 PMD 两个宿主均退出 0，完整工程重开与关闭生命周期通过。 |
| 相关回归 | 原生历史、ABI 合同、历史 UI 及 VPD codec 的当前构建结果。 | 原生历史 104,055 项、ABI 契约 1,694 项、历史 UI 967 项、VPD codec 81 项通过。 |

验证日志为 `build/abi-shadow-slider-final-build.log`、`build/abi-shadow-slider-final-test-rebuild.log`、`build/abi-shadow-slider-final-host-rebuild.log`、`build/abi-shadow-slider-final-only.log`、`build/abi-shadow-slider-final-native.log`、`build/abi-shadow-slider-final-contract.log`、`build/abi-shadow-slider-final-related.log` 及 `build/abi-shadow-slider-reviewed-host/report.json`。首轮 related 日志中的 ABI 契约测试在系统临时目录保存布局文件时失败；指定工作区 TEMP/TMP 后在 contract 日志中通过，历史 UI 和 VPD 的结果取自 related 日志。

新增 notification 对照测试首轮访问异常，原因是无渲染器夹具未预选阴影轨道而进入旧完整界面刷新。夹具现与既有 parity 对照一致；轨道激活仍由独立检查覆盖。实际宿主首轮则在重开工程时发生 `0xC00000FD` 栈溢出。迁移前宿主在新目录的对照通过，证据为 `build/abi-shadow-slider-host-baseline/report.json`。检查发现测试入口用约 661 KB 的状态临时对象占用默认 1 MB 栈，改为仓库其他测试已有的原位清零方式后，完整 PMX／PMD 宿主通过。该修正仅在 `tests/sidebar_resize_tests.cpp`，没有修改产品加载代码或增大线程栈。

滑杆专项生成的 `shadow-slider.pmm` 只检查保存；完整宿主另有通用工程重开测试，不能等同于该专项文件已独立重开。故障注入范围是准备阶段的 C++ 分配，不包含系统 malloc、GPU 故障或所有提交后窗口通知。

此前运算器测试中的 `compute adapters are published for the menu` 失败，以及菜单测试中的 `private desktop` 失败，没有后续通过证据。本批不处理这些跨分支事项，也不将相关测试归为通过。

## 隔离与未验收项

本批不修改 main 工作区，不续办配置路径修复，不部署或覆盖稳定版 EXE，不关闭用户正在运行的实例。配置路径和 main 的构建／测试对应关系不在本批验收范围。

Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求保持。真实 Win7 与 AMD 设备、浮动窗口真实鼠标操作、IME 和多 DPI 尚未验收。隐藏控件、鼠标捕获设置及消息分派测试不能替代这些验收。原始兼容 profile 的独立重建结果也需单独记录。

其他原生写入迁移完成并通过相应验证前，普通 `mms.edit` 保持关闭。完整刷新、UI 挂载、布局历史、多视口、独立求值与绘制、格式及编辑工具继续按[实施表](../PLUGIN_ABI_IMPLEMENTATION.md)推进。
