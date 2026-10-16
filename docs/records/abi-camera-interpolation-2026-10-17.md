# 相机插值共同历史验证（2026-10-17）

本批基于 `codex/plugin-abi` 的 `6ce1444`，仅在 `D:/MMD-Next/MMS-abi-dev` 开发和构建。相机模式的插值粘贴、重置进入显式绑定的共同历史。普通启动仍不开放 `mms.edit`，未迁移的编辑入口继续按原计划处理。

## 实现范围

- `PlanCameraInterpolationEdit` 修改选中的现有相机键，包含第 0 帧。支持六个通道及全部通道，只保存实际变化的插值字节和曲线作者状态。
- 提交、撤销及重做均不求值、不移动播放头，不覆盖当前相机姿态、父骨绑定、后来调整的选中标记或其他轨道。空操作保留 redo；准备失败或取消不写入部分结果。
- `TryApplyBoundInterpolation` 统一分派模型与相机按钮。相机事务在播放、录制、保存、烘焙、相关模态操作或共同历史忙碌时拒绝；已绑定的失败路径不回退旧写入。
- `TrackAuthoring` 与普通曲线读取、保存、捕获统一识别稳定键池身份。正常扩容可以换地址，真实换池仍使旧作者状态失效；附件还校验对象身份，借用池继续严格校验地址。短期候选拒绝准备后的扩容。

独立审查发现并修复了“扩容后先读取曲线，再撤销”的问题：旧判断只比较分配地址，会删除仍有效的自由曲线。新的回归覆盖先读取、先保存、扩容后创建新记录，以及“插值修改→相机登记扩容→读取与保存→连续撤销和重做”。`Write` 记录事务最终存活的池身份，`Load/Rebuild` 记录当前池身份，防止误记临时存储。原始 profile 保留原地址判断。

## 验证结果

证据路径均相对于 ABI 工作目录。下表中的 `reviewed` 结果来自身份修复后的重新构建。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| Win7 CPU 基线构建 | 正式程序及相关测试目标通过；`full`、Win7 基线开启、DX12 关闭。 | `build/abi-camera-interpolation-build-final.log`、`build/abi-camera-interpolation-tests-final-build.log`、`build/abi-camera-interpolation-roundtrip-build.log` |
| 原生共同历史 | 94,232 项全部通过，包含本批新增的 1,959 项。 | `build/abi-camera-interpolation-native-reviewed/result.log`、同目录 `report.json` |
| 真实相机按钮与插件混合历史 | 原名 PMX、中文改名 PMD 均通过。覆盖全部通道、原生及插件交错撤销、修订号、无变化保留 redo、非法选择及忙碌拒绝；输入文件未改变。 | `build/abi-camera-interpolation-host-reviewed/report.json` |
| 曲线宿主 | 全部通过。 | `build/abi-camera-interpolation-curve-reviewed/result.log` |
| 相机 CPU 保存重开 | VMD 与四次独立 PMM 重开均通过，每次 477 个状态采样；四份 PMM 与 VMD 的 1,908 组采样一致。 | `build/abi-main-merge-camera/run-1792193589294478500/verification.json` |
| ABI 契约 | 1,523 项通过。本批后续身份修复不修改该测试链接的插件运行库。 | `build/abi-camera-interpolation-contract.log`、`build/win7-x64/build/Testing/Temporary/LastTest.log` |
| 普通启动与界面 | 原名、中文改名均通过隐藏启动、菜单、工具栏、面板与关闭。 | `build/abi-camera-interpolation-smoke-reviewed.log` |
| PE 导入表 | 正式程序和编辑测试 DLL 均为 x64、子系统 6.1，无 DX12、DXGI、d3dcompiler_47、CUDA 或 NVAPI 直接及延迟导入。 | `build/abi-camera-interpolation-pe-reviewed.json` |

原生相机测试包含 14 组旧按钮字节对照、自由曲线及 Step 恢复、键池增长与替换、作者来源变化、非法链、内存预算、逐分配失败、取消和正反向无分配发布。测试入口见 [编辑整合说明](../PLUGIN_EDIT_INTEGRATION.md#相机插值按钮的受控接入)。

正式程序按正常流程构建。资源输入未修改，资源构建前后的 SHA256 一致；相关测试构建复用已生成资源，源码仍正常编译和链接。早期的 `native-focus`、`native-final` 和不带 `reviewed` 的宿主结果保留作过程证据，不替代修复后的结果。

本批 ABI 程序 SHA256 为 `C829B2E61722426D7BF4AD9D49E7B8173087EA368492ABE394F457EA4987A2B2`。main 保持 `04d3dec`，工作树无修改；稳定版程序 SHA256 仍为 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`。

## 边界

本批未迁移关键帧复制粘贴、插删帧、图形曲线编辑器和插值控制点拖动，也未开放普通宿主的公共编辑服务。真实曲线面板打开后的异步显示刷新、每一种模态阻塞条件尚未逐项验收。

Win7 SP1 x64 和 AMD 真机仍未验收。当前结果验证本机 CPU 路径及静态导入，不替代旧系统、AMD 硬件和发布运行库部署验收。构建缓存关闭 Release PDB，仅用于本机验证。
