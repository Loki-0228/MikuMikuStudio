# 物理骨骼编辑读数同步

日期：2026-10-23。基线：`e0351cb`。工作区：`D:/MMD-Next/MMS-abi-dev`。参考 main 已提交 `fed9768`，没有读取或改动 main 未提交源码。

## 行为与实现

物理模式 3 的刚体骨骼原先无条件显示 `ikBackup`。用户修改或重置骨骼后，即使 `bonePhysicsState[选中骨骼]` 已非零，位置和旋转框仍显示旧物理姿态，派生角度也来自旧备份。

`PostViewRefresh` 现与 main 一致：标记非零时读取 `trans/rotQuat`；标记为零或标志表不存在时，只有物理模式 3 且有刚体才读取 `ikBackup`。其他模式和无刚体继续显示作者姿态。主面板及分离面板共用该函数，两个产品配置均包含此修复。

ABI 专有的 `ReplacePositionDraft` 也应用同一条件，防止轴重置成功后把旧物理位置重新写进输入框。该路径仍仅属于增强配置。两处保留已有父绑定偏移、格式化、输入草稿保护、发布顺序和事务边界，不写姿态、关键帧或撤销记录。

`ReplacePositionDraft` 已有模型、骨表、所选索引和轴范围检查。`PostViewRefresh` 仍依赖有效模型与索引的调用前置条件，本批没有补齐其全部输入防护。

DSH 子任务 `7c58f865-ff1a-4560-80b4-e39f814568e5` 仅修改上述两个生产文件，实际 cwd 为本 ABI 工作区，正式结果 `completed`。`sawError=true` 记录了一次局部检索的 `CodeRunFailedError/CODE_RUN_FAILED`，后续检索与修改完成；没有构建、测试或文档工作交给 DSH。Codex 编写测试、独立审查和验收。完整事件及结果保存在 `build/verification/abi-pose-physics-readout-20261023`。

## 本机证据

两配置产品构建通过。`pe-01.json` 确认 x64、子系统 6.1、导入表没有增减，117／118 资源保持。完整产品 SHA256 为 `ba482db5d1b475813ec8901b1c6862b461bfb8a4f8883c385b15208f9eb9816f`，原始兼容产品为 `c32530fd97a2c3079cd46bc1d26f9247777aa85e7253ee8b3fdc46a240e9baf1`；副本保存在 `production-01/products`。

初版专项 `checks-axis-01` 通过 3,088 项，数值编辑回归 `checks-numeric-01` 通过 310 项。专项创建真实隐藏 HWND/EDIT，调用实际刷新、命令和共同历史，不执行渲染或物理模拟。检查主／分离面板、非零／零／空标志、其他模式、无刚体、父绑定偏移、位置和旋转来源，以及模型数据、键池、历史版本、redo、播放头保持。

实际旧代码反例暴露了初版测试的一个缺口。只恢复旧 `PostViewRefresh` 时，`counter-old-view-01` 在位置来源断言失败。只恢复旧 `ReplacePositionDraft` 时，`counter-old-draft-01` 仍通过 3,088 项，因为后续正确刷新覆盖了错误文本；不能把该运行称为已捕获第二个缺陷。反例脚本的 finally 已按哈希恢复最终生产源码并重建测试，未覆盖产品。补充测试使用真实 `SetPose(..., Scale(2))` 且 `TransformsApplied=false` 的场景，旋转文本和派生角哨兵确认后续刷新确实跳过；没有 hook 刷新函数或调用 ApplyTransforms。`candidate-02` 固定最终输入，`counter-old-draft-02` 在新增草稿断言失败，证明第二入口能独立检出。`checks-axis-02` 及源码恢复后 `checks-axis-restored-01` 均通过 3,218 项。新增及既有本批重置夹具均明确断言实际焦点已经从输入框移到自有 owner。

## 尚未完成

旋转轴、整体旋转和整体姿态重置的未提交草稿处理尚未同步。非单位缩放可能使 `RefreshBoundPoseReadout` 跳过刷新，后续修复不能只清文本修改标志，还需要保持派生角度与目标姿态一致。整体重置归零语义、手动编号保存、菜单首次绘制及长文本前缀等仍按[迁移计划](abi-main-sync-plan-2026-10-22.md)继续实施。

本批运行范围是完整配置的隐藏原生控件专项。原始配置完成产品构建及静态产物核对，没有独立 UI 运行验收。没有人工窗口切换、实际物理模拟、视觉、Win7 或 AMD 真机验收。提交前再次核对 main 仍为 `fed9768f9fe726c4b5e068849808803f1713ad42`。正式 release SHA256 保持 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，未覆盖；总 ABI 目标和普通公共编辑服务的开放条件不变。
