# 物理骨骼的显式重置接管

日期：2026-10-23。ABI 基线为 `47eba21`，本轮开始及构建核对的 main 为 `fed9768f9fe726c4b5e068849808803f1713ad42`。本批同步 main 重置命令将骨骼标记为已编辑的行为，同时保留 ABI 的事务和无变化约定。main 工作区与稳定版产物未修改。

## 接管条件与历史

骨骼在 `PlaybackPhysicsMode()==3`、带刚体且 `bonePhysicsState[bone]==0` 时使用物理备份作为面板读数。即使其作者位移已经为零、旋转已经为单位四元数，用户的显式重置仍需要把该标记改成 1，使编辑姿态接管。是否接管不另加 `physicsDisabled` 条件，也不取决于备份与作者值是否相等。

本批覆盖绑定的位置轴、旋转轴、菜单 302，以及绑定和普通未绑定的整体重置 495。位置轴重置保持其他作者分量。旋转轴在目标显示角已经为零时仍按原 `RotationTarget` 计算，使其他显示角参与目标旋转；保留主窗口和分离窗口各自原有的浮点计算顺序。菜单 302 恢复单位旋转。495 将选中骨骼的局部位置、旋转和缩放恢复零位与单位值。

接管与作者值修改属于同一次编辑，不能在刷新阶段单独补写标记。绑定入口沿用 `NativePoseScope` 的 Dirty 差异记录。轴和 302 保持 Adjustment 策略：全部操作模式记录历史，显式提交模式不额外记录。495 保持 ExplicitCommit，两种模式均记录一次。普通 495 沿用原 30 槽撤销环，将仅标记变化的骨骼纳入预备快照，完成全部准备后发布。

撤销恢复原作者值与 dirty=0，重做恢复目标值与 dirty=1。物理备份 `ikBackup`、刚体开关、当前帧和其他对象保持。非物理模式、不带刚体或已经由编辑姿态接管的零位重置仍是无变化操作，保留 redo。缩放轴、数字编辑、粘贴、镜像和原始兼容配置不扩大本批行为。

成功后沿用既有草稿取消及读数发布规则；失败保留作者数据、标记、草稿与历史。非单位缩放尚未应用时，既有完整读数刷新保护仍生效，不为显示强制执行场景求值或安装缩放变换。

## 本机验收

证据目录为 `build/verification/abi-physics-takeover-20261023`。`candidate-01/manifest.json` 固定七个源码与测试文件。运行时检查串行执行，继承 Default 桌面，只操作自有隐藏窗口。

| 检查 | 结果与证据 |
| --- | --- |
| 完整及原始兼容产品构建 | `build-full-01.json`、`build-original-01.json` 均成功。 |
| 绑定轴及旋转菜单 | `axis-01/report.json` 与 `axis-restored-01/report.json` 各通过 8,200 项，包含两种历史模式、主／分离窗口、位置与旋转三轴、302、两处旋转提前返回、真实焦点与回填值。 |
| 绑定整体重置 | `commands-01/report.json` 与 `commands-restored-01/report.json` 各通过 7,741 项。 |
| 普通撤销环 | `legacy-01/report.json` 与 `legacy-restored-01/report.json` 各通过 6,231 项，保留独立安装标记和普通初始化约束。 |
| 相邻数值编辑 | `numeric-01/report.json` 通过 310 项。 |
| 失败与混合选择 | 新用例包含有变化骨骼、仅接管骨骼、普通无变化骨骼和未选骨骼；检查分配失败、忙碌、预算拒绝、失焦和实际撤销重做。分配扫描上限为 512。 |
| 旧轴实现反例 | `counter-old-axis-1/report.json` 退出码为 1，在零值物理重置必须接管的断言失败。 |
| 旧整体重置反例 | `counter-old-reset-1/report.json`、`counter-old-reset-2/report.json` 分别在绑定和普通入口的 dirty-only 接管断言失败。 |
| 恢复验证 | `counterexamples.py` 在 finally 中校验并恢复源码哈希，再重新构建；上述三个 restored 运行全部通过。 |
| 兼容构建核对 | `pe-reviewed.json` 确认两产品为 x64、子系统 6.1，导入表无增减，内置效果资源保持；`original-case-static.json` 确认原始 495 分支除换行规范外逐字相同。 |

产品副本为 `candidate-full.exe` 和 `candidate-original.exe`。稳定版 `build/release/MikuMikuDance.exe` 的 SHA256 保持 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。

轴修改由 DSH 会话 `f01d69c5-f132-4452-86e2-8a5ae434657b` 在实际工作目录 `D:/MMD-Next/MMS-abi-dev` 完成，仅修改 `src/plugins/native_pose_axis.cpp`。Codex 负责整体重置、测试、整合和验收；正式结果保存在 `dsh-result.json`，completed 本身未作为通过依据。

## 验收边界与下一批

本批证明命令、作者姿态、来源标记、原有历史及隐藏原生控件的行为，不证明真实物理模拟、GPU 画面、Win7、AMD 或混合 DPI 真机表现。撤销后物理读数及重做后作者读数的选择规则另由同一轴专项内既有物理读数用例回归；新测试没有启动物理引擎。`physicsDisabled=1` 的专门对照未在本批新增。

普通未绑定旋转轴及菜单 302 的草稿和原撤销环适配仍待实现。后续迁移普通旋转算法入口时，须先冻结独立数值 oracle：本批 `TakeoverLegacyRotation` 使用现有普通入口作为旧精度对照，不能让被测路径和对照一起变成新实现。下一批方案保存在 `next-unbound-rotation-plan.txt`。

手动编号保存、磁盘缓存、完整 UI 目录及其他 ABI 缺项继续按剩余清单推进，普通公共 `mms.edit` 仍关闭。
