# main 源码审查与本地验收记录（2026-10-17）

本轮已完成下列本机验收，结果以对应日志和报告为准。本地证据位于 `D:\MMD-Next\MMS`，不保证随仓库提供；尚未覆盖的环境单独列在文末。

## 源码与产物范围

本轮审查 `D:\MMD-Next\MMS` 的 `main`，基线为 `42d4a71`（`v2026.10.15.2`）。插件 ABI 开发留在独立目录 `D:\MMD-Next\MMS-abi-dev` 的 `codex/plugin-abi`，本轮未将该分支作为 main 的构建来源。

完整档构建目录为 `build/main-review/build`，最小档为 `build/main-review-minimal/build`。配置日志均记录 C++17、MSVC `19.44.35229.0` 和 x64。构建约束及运行库限制见 [Win7 x64 基础构建](../development/win7-build.md)。

完整档程序、核心库和资源先按正常构建流程生成。多个测试目标重复生成相同资源，日志两次确认字节一致。最后一批测试构建复用这份资源，通过本次 MSBuild 调用的 `CustomBuildToolBeforeTargets=`、`CustomBuildToolAfterTargets=` 关闭重复生成，仍重新编译测试源码并链接。前后资源 SHA256 一致，见 `build/main-review-resource-hashes.json`；最终测试构建日志为 `build/main-review-test-build.log`。

稳定程序 `build/Release/MikuMikuDance.exe` 未替换，复核的 SHA256 为：

```text
78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1
```

本轮没有发布。下列结果对应独立验收目录中的产物，不能解释为稳定程序已经包含这些修改。

## 已完成的本机验收

### 真实程序启动与关闭

下列 4 次隐藏启动和正常关闭均退出码 `0`。完整档最终复跑见 `build/main-review-smoke-1792190149362114300/report.json`，未受后续曲线修复影响的最小档见 `build/main-review-smoke-1792188392682292000/report.json`。

| 构建档 | 程序名 | 主菜单数量 | 结果 |
| --- | --- | --- | --- |
| 完整档 | `MikuMikuDance.exe` | 9 | 通过 |
| 完整档 | `舞台编辑器.exe` | 9 | 通过 |
| 最小档 | `MikuMikuDance.exe` | 8 | 通过 |
| 最小档 | `舞台编辑器.exe` | 8 | 通过 |

最终完整档 EXE SHA256 为 `df1c5c301a6fc754577ccc058ae987ed9e5909817018030ece00f54a0d81ced8`，最小档为 `f024e8063c0574a0c1188aef0c16ab44b15401a93911fe85c2d8496574add2c6`。最后一项生产修复只涉及完整档通道树点击后的键盘定位；重新构建并复跑了完整档启动、曲线宿主和相机场景，其余未变模块沿用此前结果。

### 日志、备份与原生功能

| 项目 | 已完成结果 | 本地证据 |
| --- | --- | --- |
| 完整档故障日志 | 115 项检查通过 | `build/main-review-runtime-log.log`、`build/main-review-runtime-log/report.json` |
| 最小档故障日志 | 115 项检查通过 | `build/main-review-minimal-tests-1792187792084460000/runtime-log/result-0.log` |
| 完整档自动备份 | 131 项检查通过 | `build/main-review-tests-1792188769253819800/autosave/result-0.log` |
| 完整档真实崩溃恢复 | 默认配置、定时保存开启、定时保存关闭，共 3 次通过 | `build/main-review-tests-1792188769253819800/report.json` |
| 完整档容量提示 | 5 种条件各启动 2 次，全部通过；条件与下列最小档相同 | 同上报告的 10 条 `full-*` 记录，汇总见 `build/main-review-ui.log` |
| 最小档原生状态 | 32 项检查，0 失败 | `build/main-review-minimal-tests-1792187792084460000/original-state/result-0.log` |
| 最小档原生输入输出 | 5075 项检查，0 失败 | `build/main-review-minimal-tests-1792187792084460000/original-io/result-0.log` |
| 最小档自动备份 | 131 项检查通过 | `build/main-review-minimal-tests-1792187975884449400/autosave/result-0.log` |
| 最小档真实崩溃恢复 | 默认配置、定时保存开启、定时保存关闭，共 3 次通过 | `build/main-review-minimal-tests-1792187975884449400/report.json` |
| 最小档容量提示 | 中文、英文、日文、覆盖旧备份、仅崩溃目录满，共 5 种条件，各启动 2 次，全部通过 | 同上报告的 10 条 `full-*` 记录 |
| 最小档初始视点 | 初始视点、步进及相机求值检查通过 | 同上目录的 `startup-view/result-0.log` |
| 最小档 MME | 12 次独立进程检查通过 | `build/main-review-minimal-mme.log` |
| 完整档 MME | 12 次独立进程检查通过 | `build/main-review-mme.log`、`build/main-review-fixtures/emm-defaults-prepared/verification-1792189541825816500.json` |
| 完整档初始视点 | 初始视点、步进及相机求值检查通过 | `build/main-review-tests-1792189538601901500/report.json` |
| 完整档曲线宿主 | 全部检查通过，包括相机精确键值、单帧保持、原生插值保留及底栏重绘 | `build/main-review-curve-host-final2.log`、`build/main-review-curve-host-final2/report.json` |
| 完整档曲线编辑器 | 完整回归 2,000 项检查通过，包括立即应用、撤销、键盘、通道树、比较绘制、多语言及窗口生命周期 | `build/main-review-curve-editor-final9/result.log`、同目录 `report.json` |

故障日志检查包括致命错误、未捕获异常、`abort`、关闭阶段、目录不可创建、并发写入及持有日志槽时崩溃。容量提示检查包括真实分配失败、提示窗口创建失败后的重试、关闭定时保存后仍提示，以及进程重启后再次提示。

两档 MME 检查均覆盖英文与中文路径、无配置、缺少配置键、已保存关闭状态，以及保存、切换和另存为。本轮保留 PMM 打开时现有的 MME 文件打开通知。

早期最小档测试报告保留了失败记录。自动备份和界面验收以 `main-review-minimal-tests-1792187975884449400/report.json` 的完整复跑为准，不以旧报告中的部分通过项代替完整结果。

### 完整档媒体与导入表

`build/main-review-native-media.log` 记录本机原生音频解码、H.264 视频读取、定位与循环，以及 WIC PNG、MOV 编码、取消和未完成导出的检查通过。该结果不证明旧系统具备相同编码器。

`build/main-review-pe.json` 检查了两档 EXE、完整档日志测试程序和最小档备份测试程序。四个文件均为 x64，子系统版本为 `6.0`，`directOptionalBackendImports` 为空。该报告是导入表清单，不是 Win7 运行验收。

完整档最后一次重建后再次检查导入表，结果一致，见 `build/main-review-pe-final.json`。最终构建日志为 `build/main-review-final-build.log`。

### 相机场景保存与播放

原创夹具包含 0、45、92、93 帧相机键、交叉插值手柄和相邻一帧区间。使用无 MME、CPU 蒙皮的隐藏实例，通过生产 VMD 加载器、曲线读写、PMM 保存重开及帧驱动器。

首进程在四个帧位保存 PMM，随后分别用四个新进程重开。5 个进程退出码均为 0，共记录 34,554 条通过断言；四份 PMM 共 16 次关键帧数据精确比较和 4 次曲线求值往返比较通过。新进程共 1,908 条整数及小数帧状态与首进程一致。所有进程均正常到达末帧并停止，输入文件哈希保持不变。

最终复跑的逐进程日志、状态对照及计数见 `build/main-review-camera-native/run-1792190146812696300/verification.json` 和同目录 `summary.json`。重复采样断言数量不代表相同数量的独立测试场景。

## 审查中修正的边界

自动备份的容量扫描先构造完整结果，再更新提示状态。分配失败保留此前状态。`Tick` 和 `BackupNow` 捕获 C++ 异常，提示构造或窗口创建失败不消耗本次启动的提示机会。main 的备份删除继续调用 `DeleteFileW`。

日志槽由写入和快照读取共用一次性尝试的原子占用标志。竞争时跳过该槽，复制完成后再格式化和写盘。故障线程即使未释放槽位，报告也不等待该线程。该取舍允许丢失竞争记录。

曲线类型和预设立即应用，持续手柄模式与一次性预设分开。完整轨道没有变化时不提交历史；模式控件上的 Enter 不再误用尚未提交的数值文本。原生相机在精确键帧直接返回作者值，保留相邻一帧区间的原有保持规则。

通道树点击标签后同时同步键盘定位，避免下一次空格或回车作用于旧通道。该问题由改用真实 TreeView 消息后的测试复现，修复仅涉及标签命中路径。

扩大回归时修正了旧测试夹具：时间范围不能越过零帧，极小数值范围条的中心会命中端部手柄，图标取样须包含内边距，底栏标签须使用实际布局坐标。范围约束和布局计算未改，范围、作者数据和像素断言保留。

通道测试改用 TreeView 叶节点身份和实际勾选框、标签命中区域。隐藏控件先通过内存绘制初始化原生勾选图像；像素读取前完成 GDI 刷新。比较绘制以只有主曲线的完整图像为基准，检查独立通道颜色、隐藏后的基准图恢复，以及重新显示后的完整图像一致。普通通道的可见轨道集合与比较模式状态分别断言。旧失败日志保留作诊断，最终以完整回归通过记录为准。

## 尚未覆盖的环境

Win7 SP1 x64、AMD CPU/GPU 真机、运行库部署闭包及旧系统媒体编码器尚未验收。本机构建、导入表检查和 CPU 路径测试不替代这些验收。
