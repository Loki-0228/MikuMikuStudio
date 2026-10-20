# 相机父骨骼共同历史实现与验收记录（2026-10-21）

控件 `450` 的父骨骼切换已接入内部共同历史路由。完整配置父骨骼专项 2,367 项及相邻八组回归均通过，两套产品构建通过。本记录对应[父骨骼迁移计划](abi-camera-parent-bone-plan-2026-10-21.md)的实施代码；旧计划保留当时状态，实际范围以本记录为准。

本批只记录 `CameraParentBone()` 的一个 `int32_t`。它不补偿相机世界位置，不调用 `PlanCameraFollow(true)`，不注册相机关键帧，也不开放普通公共 `mms.edit`。原始配置和完整配置的未绑定入口保留旧逻辑。正式 release 未覆盖。

## 原生入口与内部接口

[command_dispatch.cpp](../../src/window/command_dispatch.cpp) 的 `DefaultSelChangeChain` 在收到 `450` 的 `CBN_SELCHANGE` 时，先调用 `TrySelectBoundCameraParentBone`。存在共同历史绑定时，成功、同值、忙碌、过期和失败均消费该通知，不回落到旧的直接写入。未绑定及 `MMD_ORIGINAL_PROFILE` 仍通过 `SelectedComboBone` 读取索引，写字段并执行旧选择语义。

[native_camera_parent_history.hpp](../../src/plugins/native_camera_parent_history.hpp) 声明四个 C++ 内部接口。

| 接口 | 行为 |
| --- | --- |
| `PlanCameraParentBone` | 检查当前来源和目标索引，准备私有 `HistoryChange`。同值成功时返回空计划。 |
| `ApplyBoundCameraParentBone` | 按显式目标提交一个 Adjustment，并通过 `changed` 区分实际发布和同值。 |
| `TrySelectBoundCameraParentBone` | 验证原生下拉框和项目数据，消费绑定入口的成功或拒绝。 |
| `RefreshBoundCameraParentBoneReadout` | 读取当前作者值，同步 `449`、`450`，不写工程字段。 |

入口检查 `449` 的父模型映射和 `450` 的完整列表。列表允许过滤骨骼、非连续索引、重复名称及合法重排，但 item data 必须唯一，并指向当前父模型的可选骨骼。行号不作为骨骼索引。无效索引、被过滤类型、重复 item data、过期父模型映射和错误 HWND 均拒绝。

## 历史只拥有父骨骼索引

`ParentChange` 保存完整的 before、after `int32_t`，包括原先不可列出的负值。撤销不归一原值。新目标必须是当前父模型中的合法可选骨骼。

该变化不采样骨骼姿态。渲染仍通过当前骨骼的 `position` 和 `matInit` 取得相机焦点，因此切换后画面可以随父骨骼变化。相机位置、角度、offset、distance、FOV、perspective、attachment basis、suppressed、follow、播放头和模型未登记姿态均不属于本记录。

两种模式共用 `SharedHistory::NativeIntent::Adjustment`。

| 记录模式 | 实际变化 |
| --- | --- |
| `AllOperations` | 发布索引并新增一个共同历史节点。 |
| `ExplicitOnly` | 发布索引，不新增节点，保留既有 redo。 |

同值不提交，不标记工程修改，也不清除 redo。插件事务 pending、历史忙碌、播放、外部鼠标捕获、工程保存及受限模态状态都会阻止写入。绑定入口的拒绝只恢复控件，不改变轨道选择或取消其他插件的事务。

## 长期来源与短期候选

[native_camera_parent_history.cpp](../../src/plugins/native_camera_parent_history.cpp) 的 `ParentSource` 固定历史绑定、工程 `SceneGeneration()`、相机键池逻辑身份、父模型 slot、原始模型指针、稳定对象 ID、`boneTable` 和 `boneCount`。历史用 `RetainedSceneObject` 保活模型。检查先确认当前 slot 成员、owner 和稳定身份，再解引用骨骼表；同一指针出现在多个模型 slot 也会拒绝。

长期来源不锁定首次 runtime generation。同一 retained owner 删除后恢复可以重新准备重放，正常相机键池扩容也保留记录。替换 owner、骨骼表、骨骼数量、工程或相机键池逻辑身份会使记录过期。历史重放不要求回到最初播放头。

首次提交额外检查原字段、history revision、binding generation 和对象 generation。每个短期 `Candidate` 重新固定当时的 frame、对象 generation、binding、history revision、预期索引、捕获 HWND、记录 serial 和消费状态。准备后删除再恢复同一 owner，也会使已经存在的短候选失效。

准备阶段通过 `MeasureBookkeeping` 收集实际模型保活账本。`TargetRetainedBytes` 包含记录自身及动态账本，发布前用 `CoversBookkeeping` 确认预算身份仍覆盖当前来源。预算不只计算索引的四字节。

候选和账本的分配均发生在准备阶段。最终 `Publish` 只写父骨骼索引及记录自身状态，不分配、不求值、不 seek、不重置物理。`RequiresPhysicsReset()` 返回 false。取消候选、准备失败、预算拒绝及 C++ 分配失败不发布字段，也不移动历史游标。

## 原生捕获采用短期 token

原生 ComboLBox 的选择通知可能处于鼠标捕获期间。可信的 `450` 入口先调用一次 `GetComboBoxInfo`，确认真实 combo 和 list HWND，再建立 `CaptureAdmission`。只有本次精确的 combo 或 list 捕获可以进入准备过程，其他窗口的捕获继续拒绝。

短期授权写在 combo 的 `MMS.Native.CameraParentCapture.V1` 窗口 property 中。候选检查只核对精确捕获、主窗口和该 token，不在最终无失败发布中重新查询 combo。窗口销毁会移除 property，同值 HWND 重建也不能沿用旧授权。离开入口时只清理仍属于该 scope 的 token。

`ActualComboList` 测试向真实 ComboLBox 发送局部鼠标消息，记录 `CBN_SELCHANGE` 到达时的捕获。若 Windows 已经释放捕获，普通选择成功只证明通知路径，不作为捕获分支通过的证据。另两组测试分别对真实 combo 和真实 list 执行 `SetCapture`，强断言捕获非空且精确匹配，再经 `CommandDispatch` 验证一次提交和 token 清理。测试没有全局鼠标注入。

## 共同读回、缓存与失败修复

[native_camera_follow_ui.cpp](../../src/plugins/native_camera_follow_ui.cpp) 的 `RefreshBoundCameraFollowReadout` 调用新的父骨骼读回。因此，共同撤销、重做及既有延后刷新都能按当前 item data 更新 `450`。读回不重复初次轨道选择，不恢复旧 selection，不覆盖其他未登记姿态。

稳定读回复用现有[骨骼下拉框缓存](../../src/window/accessory_bone_combo_cache.hpp)。缓存属于控件 HWND，跟踪原生列表修改，并核对骨骼表、过滤类型、原始名称、宽名称、语言和翻译来源。稳定路径不为每个骨骼重新生成 `wstring`，也不重写原生列表。`CB_GETLBTEXT` 的读取受既有 `ReadGuard` 保护。

测试定义包含一个真实 `4000` 骨骼列表。预热后连续执行 `12` 次共同读回，分别统计 C++ 分配、原生列表写入和标签读取，要求三项均为零。名称、语言、骨骼过滤、item data 或列表内容改变后必须失效并修复，不能只按当前选中索引跳过检查。该测试在首次失败运行中尚未执行。

无父模型或父模型不存在时，读回清空 `450` 并恢复 `449` 的无父选择。最终选择读回还检查 canonical `CameraParentBone()`，防止 `CB_SETCURSEL` 重入修改字段后错误发布旧 UI 状态。

拒绝或分配不足时，`RestoreFast` 不生成标签、不分配。它固定稳定对象 ID、runtime generation、工程、键池、表、数量和窗口后，按 item data 恢复现有健康列表。损坏列表交给 `RepairProc` 的控件 subclass 与 timer 修复。`WM_NCDESTROY` 清理 timer 和 subclass，延后回调先检查绑定及当前 HWND，再访问 app。修复失败不回滚已经提交的索引，也不重复提交工程变化。

## 初次选择效果与真实 GDI 高亮

旧 `RefreshRequest(-1)` 的作用是进入相机全局轨并清除附件行选择，不是泛用刷新。新路径只在已接受的初次动作中保留该语义，包括合法同值动作。相机轨已经选中时，不再次清除其他轨道或附件选择。拒绝、撤销、重做和读回都不执行初次选择效果。

主窗口的旧 `WM_PAINT` 只复制 `PanelDC`。单独 `InvalidateRect` 不会更新其中的轨道高亮。实现仅在本次确实切入相机轨、当前为相机模式且 `Renderer()`、`PanelDC()` 有效时调用一次 `PostLanguageSweep`。模型模式不调用该 helper，避免它调整 `selectedBone`。该可选通知位于成功提交之后，并受输入 gate、草稿保护和绑定来源复核约束。

`AcceptedPanelHighlight` 测试定义用真实内存 DC、两张 32 位 DIB 和实际 `D3DRenderer` 包装对象检查 `PanelRowFlags` 及像素变化。包装对象没有 D3D device，也没有伪造 COM 指针。模型模式、已选中相机轨的后续修改和共同撤销必须保留当前选择及面板呈现。该检查只验证 GDI 面板，不证明 GPU 视口呈现；首次失败运行尚未执行此检查。

实际变化提交后标记工程修改，更新历史按钮及下拉框，再请求 `updates::RequestViewport(&app, false)`。该请求排队 `RenderDirty | PresentDirty`，不追加 `EvaluateDirty`，不设置 projection pending。已有的其他求值请求不属于本次承诺。共同撤销沿用现有只读 UI 收尾。发布后的 UI 异常不会把已提交的数据报告成可重试失败。

## 草稿读取前固定完整来源

保存未提交文本需要向真实 EDIT 发送 `EM_GETMODIFY`、`WM_GETTEXT` 和 `EM_GETSEL`。这些调用可以同步重入，并改变父模型、工程、骨骼表或 owner，而历史绑定保持不变。

`ApplyBoundCameraParentBone` 在任何草稿读取前固定 `ParentSource`、稳定 owner、runtime generation、scene、pool、表、数量、原 scalar、frame、binding、history revision 和目标骨骼类型，并预建原计划。草稿读取后只验证这个原来源，不对新来源重新 `Plan`。同值路径也执行这些检查，不能跨来源执行初次选择效果。

`DirectDraftSourceGuards` 包含 `3 × 4 × 2 = 24` 组反例：三种原生草稿读取分别改变 parent、scene、table 或 owner identity，并同时覆盖真实变化和同值。前后目标索引均合法，不能用参数拒绝替代来源检查。断言要求 `MMS_E_STALE`、`changed == false`、零历史、零 dirty、零初次选择效果、原草稿仍在，且保留回调自身的来源变化。最终专项已实际执行并通过这些反例。

## 标准 PMM 测试边界

PMM、VMD 和其他标准序列化布局没有改动。当前父骨骼仍由 [pmm_save.cpp](../../src/io/pmm_save.cpp) 的 config2 写入有符号四字节字段，[pmm_load_v2.cpp](../../src/io/pmm_load_v2.cpp) 对应读取。相机注册继续独立记录自己的父模型和父骨骼。

`StandardPmmBytes` 定义真实 `SaveSceneCopy` 的前后两份文件。测试先用保存兼容检查确认夹具可由现有标准 PMM 表达，再解析 MMS trailer 边界，只比较完整的标准 PMM v2 prefix。两份 prefix 的头部和长度必须相同，唯一允许变化的是既有父骨骼字段的四字节。测试还检查实际写入值从 `0` 变为 `2`，并拦截意外保存拒绝对话框以使断言失败。

这不是 `LoadScene` 往返测试，也不重开夹具中不存在的模型源文件。它不能证明全模型重新导入、外部资源读取或 GPU 加载。最终专项实际执行并通过标准 prefix 检查；早期失败运行没有到达该组。

## 验收入口与现有证据

[native_camera_parent_history_checks.inc](../../tests/native_camera_parent_history_checks.inc) 的入口为 `NativeCameraParentHistoryChecks()`。[native_edit_history_tests.cpp](../../tests/native_edit_history_tests.cpp) 将该入口同时加入 `--camera-parent-only` 和全集。除上述测试外，专项还定义两种记录模式、真实共同历史 UI 路由、预算及完整 C++ 分配失败注入、取消与无分配发布、模型删除恢复、根身份变化、插件 pending、跨模型与插件交错、follow 和相机注册交错、未登记姿态及播放头保留。

证据目录为 `build/verification/abi-transport-parent-20261021`。以下状态只采用本记录写入时已经取得的结果。

| 检查 | 当前结果 | 证据 |
| --- | --- | --- |
| 首次完整配置 `--camera-parent-only` | 退出码 `1`，在混合历史组的模型 A 姿态准备失败。不是专项通过。 | [native-parent-full-01/report.json](../../build/verification/abi-transport-parent-20261021/native-parent-full-01/report.json)、[原生日志](../../build/verification/abi-transport-parent-20261021/native-parent-full-01/0-camera-parent-only/result.log) |
| 第二次 native 与产品构建调用 | native 测试可执行文件已编译，随后产品目标名称错误使整次调用退出码 `1`。不是产品构建通过。 | [build-native-products-full-02.log](../../build/verification/abi-transport-parent-20261021/build-native-products-full-02.log) |
| 正确目标 `MikuMikuStudio` 的完整配置重建 | native 与产品构建退出码 `0`。正式 release 未覆盖。 | [build-native-products-full-03.log](../../build/verification/abi-transport-parent-20261021/build-native-products-full-03.log) |
| 第二次完整配置 `--camera-parent-only` | 退出码 `1`。真实 ComboLBox 通知提交一次成功，随后显式捕获夹具的非空强断言失败。 | [native-parent-full-02/report.json](../../build/verification/abi-transport-parent-20261021/native-parent-full-02/report.json)、[原生日志](../../build/verification/abi-transport-parent-20261021/native-parent-full-02/0-camera-parent-only/result.log) |
| 相邻 `--camera-follow-only` | `10976` 项通过，退出码 `0`。 | [follow 日志](../../build/verification/abi-transport-parent-20261021/native-parent-full-02/1-camera-follow-only/result.log) |
| 相邻 `--camera-projection-only` | `197` 项通过，退出码 `0`。 | [projection 日志](../../build/verification/abi-transport-parent-20261021/native-parent-full-02/2-camera-projection-only/result.log) |
| 相邻 `--same-model-follow-only` | `2410` 项通过，退出码 `0`。 | [同模型 follow 日志](../../build/verification/abi-transport-parent-20261021/native-parent-full-02/3-same-model-follow-only/result.log) |
| 第三次父骨骼专项 | 捕获、高亮、4000 骨骼缓存和来源反例均已越过，随后因登记撤销的错误测试预期退出 `1`。 | [native-parent-full-03/report.json](../../build/verification/abi-transport-parent-20261021/native-parent-full-03/report.json) |
| 登记撤销定点诊断 | 实际 `result=0`、`keysMatch=1`、`parentBone=1`、undo/redo 为 `2/1`。原键已精确恢复，前一父骨骼节点未消费。 | [诊断日志](../../build/verification/abi-transport-parent-20261021/native-parent-registration-diag-04/0-camera-parent-only/result.log) |
| 最终 `--camera-parent-only` | 2,367 项检查全部通过，退出码 `0`，包含真实保存和标准 prefix 检查。 | [native-parent-full-05/report.json](../../build/verification/abi-transport-parent-20261021/native-parent-full-05/report.json)、[日志](../../build/verification/abi-transport-parent-20261021/native-parent-full-05/0-camera-parent-only/result.log) |
| 相邻登记、历史模式、参数读回、相机文本及附件文本 | 53,321／123／19,694／1,036／725 项分别通过，五个进程退出码均为 `0`。 | [native-parent-neighbors-06/report.json](../../build/verification/abi-transport-parent-20261021/native-parent-neighbors-06/report.json) |
| 最新原始兼容配置产品 | 正确目标构建退出码 `0`。 | [build-products-original-04.log](../../build/verification/abi-transport-parent-20261021/build-products-original-04.log) |

最终专项与相邻八组共 90,849 项检查通过。这不表示执行了原生历史全集：本轮没有运行其中会移动全局光标的 HUD 组。开发产物还包含另一批曲线只读查询保护，不能将开发 EXE 的哈希解释为仅含父骨骼改动的独立构建。

[产物检查](../../build/verification/abi-transport-parent-20261021/pe-inventory-products-02.json)记录两套 EXE 均为 x64、子系统 6.1，没有可选新图形后端或厂商 DLL 的直接导入。完整配置 SHA256 为 `d8bd037cdc9936d0c01f1532d20c369cdda732798020df58adc71be38f3a65e3`，原始兼容配置为 `c05ed10c130f506719e24ace21bc43f9e36d277a895f9f0937b49ec1a8383445`。正式 `build/release/MikuMikuDance.exe` 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，未部署开发产物。

首次失败为测试夹具错误。`BusyAndMixedPlugin` 原先通过 `SetWindowTextA` 设置模型文本后直接调用提交尾，却没有设置 `EM_SETMODIFY(TRUE)`。[ui_edit_commit.cpp](../../src/window/ui_edit_commit.cpp) 的 `CommitBoundPoseNumber` 在 `EM_GETMODIFY == 0` 时按既有规则消费并不提交，因此断言 `seed model A pose before camera parent` 失败。夹具现改用既有 `f.Commit`，要求实际姿态值和历史节点同时变化，保留原混合历史强断言，并在失败时输出接口准入、数值、节点数和修改标志。第二次运行已越过该组，整个专项随后在显式捕获夹具失败。

首次日志只到混合历史组的准备阶段，尚未到真实 ComboLBox、捕获 token、GDI 高亮、4000 骨骼缓存、延后修复、直接 Apply 草稿来源反例、follow 与注册交错及标准 PMM prefix 检查。不能将日志中已经打印的前段组名扩展为整个专项通过。

第二次专项已越过混合历史组。真实 ComboLBox 鼠标通知到达时 `capture == nullptr`，一次索引提交及历史节点断言通过，日志没有把它当成捕获授权证明。随后，显式捕获夹具在已打开的下拉框上转移捕获，`GetCapture()` 非空断言失败。原生下拉框取消后再次释放捕获是诊断推断，没有单独跟踪该内部机制。夹具移除这两项 gate 用例中打开下拉框的步骤，在闭合 combo 及实际 native list HWND 上设置捕获，保留非空、精确 HWND、一次提交和 token 清理强断言；原先的真实开框鼠标通知测试保留。第三次及最终专项均越过该组，最终也实际通过 GDI、4000 骨骼、24 组草稿来源及 PMM 检查。

第三次失败来自新测试对既有登记语义的错误假设。`native_global_registration.cpp` 在撤销或重做已发布登记时，恢复目标轨道并在当前帧求值，写回其拥有的相机姿态及父关系。夹具原关键帧的父骨骼均为 `1`，因此撤销登记后得到 `1` 合法，不能要求保留未登记调整的 `0`。定点诊断确认完整键数据已恢复、历史为 `2/1`。最终测试用独立未绑定 `FollowFixture` 和真实 `ReloadModels` 对原轨及登记后目标轨分别求值，比对 position、rotation、distance、FOV、perspective 和 parent；每一步另检查精确键数据、节点计数、当前帧与后来模型姿态。登记产品实现未改，没有删除原有回归或仅放宽父骨骼数值断言。

第二次构建的 native 编译结束后，调用了不存在的 `MikuMikuDance.vcxproj` 目标，MSBuild 报 `MSB1009`。产品正确构建目标是 `MikuMikuStudio`，其输出文件仍为 `MikuMikuDance.exe`。目标名称错误与源码编译失败分别记录，不能因 native 编译成功而将整次调用登记为成功。

专项启动继承 `Default` 桌面，`SW_HIDE` 只是进程启动请求，窗口显示由夹具管理。它不是真实独立桌面隔离。局部窗口消息、真实 CPU 焦点解析和 GDI 检查均不能替代 Win7 SP1 x64、AMD 真机、IME、多 DPI、完整鼠标交互或 GPU 呈现验收。实现保留 Win7、CPU 和 AMD 兼容要求，没有增加特定厂商或新系统接口作为基础依赖。
