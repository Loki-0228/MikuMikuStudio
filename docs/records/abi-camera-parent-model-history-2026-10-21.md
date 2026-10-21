# 相机父模型共同历史实现与验收记录（2026-10-21）

控件 `449` 的父模型切换已接入内部共同历史路由。最终完整专项 6,332 项通过，复审补充的列表恢复问题已修正；父模型、父骨骼及八组相邻回归共 97,214 项通过，两套产品构建通过。父骨骼 `450` 的既有范围见[父骨骼共同历史记录](abi-camera-parent-bone-history-2026-10-21.md)。

## 入口与记录模式

[command_dispatch.cpp](../../src/window/command_dispatch.cpp) 的父模型 `CBN_SELCHANGE` 先调用 `TrySelectBoundCameraParentModel`。有共同历史绑定时，成功、无改动、忙碌、来源失效及失败均消费通知，不回落到直接写入。未绑定和原始兼容配置保留原生入口，并补上旧父索引、目标列表及原生消息重入检查。

[native_camera_parent_history.hpp](../../src/plugins/native_camera_parent_history.hpp) 增加以下内部接口。它们不新增公共 C ABI 服务。

| 接口 | 行为 |
| --- | --- |
| `PlanCameraParentModel` | 私有准备父模型调整；无改动返回空记录。 |
| `ApplyBoundCameraParentModel` | 按 Adjustment 规则提交，并报告是否实际改变数据。 |
| `TrySelectBoundCameraParentModel` | 检查原生控件和 item data，消费受控通知。 |

「全部操作」模式保存一个共同历史节点。「仅显式提交」模式发布调整，但不新增节点或清空已有 redo。同值不标记工程修改，不改变上次发布的物理重置标记，也不消费 redo。初次接受保留原生相机轨道激活；撤销和重做只更新读回，不重复改变其他选区。

## 附着、切换与解除

`ModelChange` 保存父模型、父骨骼、位置三轴、旋转三轴和距离，共九个标量。浮点字段按原始字节比较和保存，保留 NaN payload、负零及其他原值。

从无父模型附着时，目标骨骼为第一个可选骨骼，位置和旋转保持原生初始化规则。已附着模型之间切换时，不采样或补偿世界视角。解除附着时，准备阶段只采样一次旧骨骼的 `position` 与 `matInit`，计算并保存原生解除结果。后来骨骼移动或播放头改变，不重新计算已保存的撤销和重做结果。

父模型实际改变时，父骨骼必须作为同一绑定对保存和重放，即使前后骨骼索引数值相同。其他位置、旋转和距离只拥有实际改变的分量。原先的同模型骨骼重置及 `450` 仍要求当前父模型匹配，不允许将旧模型索引写入另一个模型。

该调整不登记关键帧，不移动播放头，不运行场景求值，不切换跟随，不重置物理。PMM、VMD 及公共结构布局没有改动；本批未新增 `449` 专属的文件往返验收。

## 来源、预算与发布

历史保留旧父和新父对象，重复对象只计一次。来源固定工程代次、共同历史绑定、相机键池逻辑身份、slot、稳定对象 ID、模型指针、骨骼表和数量。验证先确认当前成员及 owner，再读取骨骼表。同一指针重复占用 slot 拒绝。

初次操作另固定帧号、捕获、全部九字段、历史 revision 和对象 generation。每次撤销或重做的短候选重新检查当前来源和九字段，防止准备后重入改变数据。模型删除并恢复后，可在同一 owner 与结构恢复时继续遍历；旧短候选不会因恢复而重新有效。

准备阶段计入实际保活账本并完成分配。最终发布只写保存的标量和记录状态，不分配或调用窗口。预算拒绝、取消和准备失败不留下部分字段，也不移动历史游标。

## 原生界面边界

`ModelInput` 在第一次原生读取前固定模型列表及其完整映射。`ModelListWatch` 跟踪原生实例、内容和选择变化；所有消息返回后重新检查，不能把一次旧选择解释为新来源。模型与骨骼都按 item data 解析，允许重复标签和合法重排。

草稿保存期间，模型身份、列表映射、选择、控件实例、播放状态或插件 pending 事务发生变化时，拒绝本次调整。Unicode 未完成输入、文本选区和修改状态保留。捕获授权只覆盖已验证的实际 combo 或 ComboLBox，不能借此接纳其他窗口的捕获。

未绑定入口先私有准备标签和解除结果，再重建原生列表。每个 `CB_*` 返回后确认旧父、目标表和数量仍相同，避免把旧模型的首骨骼索引写入较短的替换模型。拒绝后的模型选择恢复额外检测读后重排，并有界重新扫描；恢复只改 UI，不写工程。

普通读回由 `ModelReadout` 统一处理有父模型和无父模型两条路径。它沿用完整模型身份、结构和顺序快照，逐次检查原生列表的内容及选择版本；最后一次读取后重排也会触发重扫。最多重扫三轮，仍不稳定时转已有延迟修复，不能重复提交历史。

损坏映射才私有准备全部标签与文本来源。重建只允许每条自有 `RESET`、`ADD`、`SETITEMDATA` 各改变一次相应版本，嵌套的额外变更不被接纳。健康列表不准备标签容器。读回与编辑授权分开，原 `ModelInput::Current` 对列表变化的严格拒绝规则没有放宽。

## 已取得的证据

证据目录为 `build/verification/abi-curve-construction-20261021`，测试入口在 [native_camera_parent_model_history_checks.inc](../../tests/native_camera_parent_model_history_checks.inc)。本批不运行会移动全局光标的原生历史全集。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 绑定对越界反例 | 修复前登记撤销求值得到 `A/3`，父模型节点撤销仍为 `A/3`，重做为只有两个骨骼的 `B/3`，退出 `1`。 | [越界日志](../../build/verification/abi-curve-construction-20261021/native-parent-model-red-02/0-camera-parent-model-registration-pair-out-of-range-only/result.log) |
| 绑定对合法错误反例 | 修复前 `A/1` 被带入 `B/1`，虽然索引合法仍绑定错误骨骼，退出 `1`。 | [合法错误日志](../../build/verification/abi-curve-construction-20261021/native-parent-model-red-02/1-camera-parent-model-registration-pair-in-range-only/result.log) |
| 修复后的两个绑定对反例 | 共 160 项通过；独立未绑定实例通过真实 `ReloadModels` 求值，撤销和重做分别要求 `A/0` 与 `B/0`。 | [绑定对报告](../../build/verification/abi-curve-construction-20261021/native-parent-model-green-03/report.json) |
| 父骨骼、跟随、投影、同模型跟随 | 2,367、10,976、197、2,410 项分别通过。 | [相邻回归报告](../../build/verification/abi-curve-construction-20261021/native-parent-model-green-03/report.json) |
| 首次完成前十一组后的父模型专项 | 删除和登记交错组退出 `0xC0000409`，该次未完成验收。后续诊断及最终结果分别列出。 | [当次日志](../../build/verification/abi-curve-construction-20261021/native-parent-model-green-03/1-camera-parent-model-only/result.log) |
| 删除与登记独立诊断 | 删除夹具在 `CaptureRemoval` 中抛出 `model parent table`，未进入准备或发布；独立登记用例 57 项通过。 | [拆分诊断报告](../../build/verification/abi-curve-construction-20261021/native-parent-model-diag-04/report.json) |
| 最终真实删除用例 | 37 项通过，含 A 指向 B 的骨依赖与显示选择器在删除、撤销、重做中的清除和恢复。 | [删除日志](../../build/verification/abi-curve-construction-20261021/native-parent-model-final-05/0-camera-parent-model-removal-only/result.log) |
| UI 追加前的父模型完整专项 | 6,195 项通过，含两种模式、分配失败、无分配发布、来源重入、真实 ComboLBox、插件交错及登记绑定对。后续列表反例另列，不能用本次通过代替新增验收。 | [当次专项报告](../../build/verification/abi-curve-construction-20261021/native-parent-model-final-05/report.json) |
| 相邻登记、历史模式、滑杆读回、相机文本、附件文本 | 53,321、123、19,727、1,036、725 项分别通过。 | [相邻回归报告](../../build/verification/abi-curve-construction-20261021/native-parent-model-final-05/report.json) |
| 无父模型时的拒绝恢复反例 | 实际拒绝保留字段和历史，但模型列表仍为重复 item data，未安排恢复计时器，退出 `1`。 | [无父模型恢复日志](../../build/verification/abi-curve-construction-20261021/native-parent-ui-red-06/0-camera-parent-model-no-parent-recovery-only/result.log) |
| 成功切换后的读后重排反例 | 第二次扫描最后一次读取后重排，数据已成功绑定 B 且只有一个节点，但模型下拉仍显示 A，退出 `1`。 | [成功读回日志](../../build/verification/abi-curve-construction-20261021/native-parent-ui-red-06/1-camera-parent-model-accepted-swap-only/result.log) |
| 统一读回后的第一次运行 | 读后重排 29 项通过；无父模型用例已修复完整映射并恰好提交 B，一次重新挂接的数值预期错误使该次专项退出 `1`。450 和八组相邻回归均通过。 | [当次报告](../../build/verification/abi-curve-construction-20261021/native-parent-ui-green-07/report.json) |
| 最终列表恢复与完整专项 | 无父模型恢复 108 项、成功读后重排 29 项、完整父模型专项 6,332 项分别通过，三个进程均退出 `0`。 | [最终报告](../../build/verification/abi-curve-construction-20261021/native-parent-ui-final-08/report.json) |
| 最终完整配置构建 | native 测试及 `MikuMikuStudio` 产品目标退出 `0`。 | [完整配置构建日志](../../build/verification/abi-curve-construction-20261021/build-parent-readout-full-12.log) |
| 最终原始兼容配置构建 | `MikuMikuStudio` 产品目标退出 `0`。 | [原始兼容构建日志](../../build/verification/abi-curve-construction-20261021/build-parent-readout-original-12.log) |

首轮 dirty 断言错误地要求无改动也将上次发布的物理重置标记改为 false。实际日志显示无改动前后均为 true、实际 `PhysicsResetPending == 0`、节点和 dirty 均未变化。测试现要求无改动保留先前发布标记，实际物理标志与 dirty 检查仍严格保留。

完整专项中的异常来自夹具缺少依赖表。基础模型夹具声明 `boneOrderCount == 1`，却没有 `boneOrderTable`。真实移除会检查剩余模型的外部绑定，因此在捕获阶段拒绝该无效结构。诊断打印原始异常后继续抛出，没有将异常改记为通过。最终夹具保留非零 count，补齐 A、B 两个有效表及真实显示选择器，并检查删除和遍历行为；存储覆盖 Scene 和历史全部寿命，清空历史后恢复原引用，再销毁夹具。产品删除实现未改。

统一读回后的无父模型失败也来自新增测试预期。解除后再附着会执行原生位置和旋转初始化，不能只修改预期中的父模型和骨索引。最终夹具先把完整相机值复制给独立、未绑定共同历史的 Scene，强验种子相同，再通过其真实原生通知取得预期；被测路径继续严格比较完整数值、不变量和恰好一个新节点，没有删除断言。

[产品清单](../../build/verification/abi-curve-construction-20261021/pe-inventory-parent-model-controls-12.json)记录两套 EXE 均为 x64、子系统 6.1，未直接导入检查清单中的可选渲染或厂商 DLL。完整配置 SHA256 为 `77d0071f7dc6120947b3de8d7c8239695a3b60463f374bfd30a06dd9a46edfb8`，原始兼容配置为 `e6d1bdbdb175cfcfb2862575ed9b90710fb48fe04a46a0d77540333f66d48231`。开发产品也包含[原生控件寿命修复](abi-curve-native-control-lifetime-2026-10-21.md)，不是仅含 449 的独立产物。正式 `build/release/MikuMikuDance.exe` 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，没有部署开发版本。

Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求继续保留。本机原生窗口回归不能替代这些真机、GPU、IME、多 DPI 或完整鼠标验收。正式 release 和 main 均未更新，普通公共编辑服务仍未开放。
