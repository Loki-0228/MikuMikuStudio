# ABI 对照 MMS main 全仓审查

核对日期：2026-10-18。ABI 基线为 `22ac9425b44cac67950e93a41c5099abccce3e3b`，分支 `codex/plugin-abi`。本轮检查 main 报告中的全部 22 项，没有修改生产代码或正式程序。

## 结论与范围

main 报告的 22 项在 ABI 普通完整配置中均有残留：21 项仍有同类缺陷，另 1 项骨骼范围伸缩已保留 Scale 曲线，但 Free 曲线问题仍在。因此不能将 ABI 分支标为不受影响。

绑定共同历史后的路径需要单独判断：16 项仍受影响；4 项对应写入已实质迁移；1 项 PMX toon 越界由私有模型导入保护；1 项全局零帧删除被入口拦截但功能尚未实现。后三类保护均不能外推为普通路径已修复。部分已迁移项仍缺报告中精确反例的测试。

这里的“普通路径”指未绑定 `BoundNativePoseHistory` 的保留实现；“受控路径”指绑定后的实际入口。普通宿主 `mms.edit` 仍未开放，这不等于普通原生窗口命令不可达。原版配置指 `MMD_ORIGINAL_PROFILE`，与运行时是否绑定共同历史是不同维度。

来源是 `D:/MMD-Next/MMS/.local/reports/full-code-review-20261018.md`，其 main 基线为 `debeb25f89432840e679ccf8708975a0170dbe0a`，同时包含该工作区当时的未提交内容。报告 SHA256：`8539BB66EB9F611333104B8C4EC6EA9D9F591B271EE83497CA87DB98EEC8D718`。核对快照保存在 `build/abi-main-review-probes/main-report-snapshot.md`。

main 当前工作区另有针对部分条目的未提交修复及测试。本轮没有将这些开发中内容合并，也没有据其存在宣称 ABI 已修复。ABI 工作区的相机跟随草案未编译、未验收，与以下已编译基线和结论分开。

## 逐项结果

编号、优先级沿用 main 汇总顺序。源码链接均指 ABI 工作区。本轮除第 18、19 项外为源码、调用链及已有测试覆盖核对，没有新增动态复现。

| 编号 | 优先级与问题 | 普通完整配置 | 受控完整配置 | ABI 证据入口 |
| --- | --- | --- | --- | --- |
| 1 | P1：只删除灯光、自阴影或重力零帧，断开未选中的后继链 | 仍存在 | 按钮和 Delete 被拒绝，删除功能未迁移；不是完整修复 | [command_control_400.cpp:675](../../src/window/command_control_400.cpp#L675)、[native_key_commands.cpp:148](../../src/plugins/native_key_commands.cpp#L148) |
| 2 | P1：删除首刚体后新增关节，提交读取压缩表的 `-1` 下标 | 仍存在 | 共用旧编辑器，仍存在 | [physics_model_dialog.cpp:892](../../src/window/physics_model_dialog.cpp#L892)、[physics_model_dialog.cpp:1321](../../src/window/physics_model_dialog.cpp#L1321) |
| 3 | P1：完整配置保存 PMD 直接截断目标，忽略写入与关闭失败 | 仍存在 | 共用保存路径，仍存在 | [enhance_model_io.cpp:437](../../src/window/enhance_model_io.cpp#L437)、[enhance_model_io.cpp:691](../../src/window/enhance_model_io.cpp#L691) |
| 4 | P1：AVI 空值、0、0.5 FPS 整数除零；29.97 被截为 29 | 仍存在 | 共用帧源，仍存在 | [push_source.cpp:174](../../src/mmdxshow/push_source.cpp#L174) |
| 5 | P1：DirectShow 下游写入失败后无人确认，主线程可能永久等待 | 仍存在 | 共用录制路径，仍存在 | [source_base.cpp:570](../../src/mmdxshow/source_base.cpp#L570)、[record_readback.cpp:201](../../src/app/record_readback.cpp#L201) |
| 6 | P1：录制收尾派发消息时再次停止，重入拆除图 | 仍存在 | 共用收尾路径，仍存在 | [shutdown_cleanup.cpp:374](../../src/app/shutdown_cleanup.cpp#L374)、[wndproc_stubs.cpp:88](../../src/window/wndproc_stubs.cpp#L88) |
| 7 | P1：Kinect 将独立 float 当连续三分量向量读写 | 仍存在 | 共用 Kinect 路径，仍存在 | [query_audio_gaps.cpp:775](../../src/features/query_audio_gaps.cpp#L775) |
| 8 | P2：工程打开失败仍改变保存目标，随后快速保存可能覆盖被拒绝的文件 | 仍存在 | 插件加载取消不恢复原目标，仍存在 | [command_impl.cpp:337](../../src/window/command_impl.cpp#L337)、[pmm_load_shell.cpp:115](../../src/io/pmm_load_shell.cpp#L115)、[integration.cpp:330](../../src/plugins/integration.cpp#L330) |
| 9 | P2：曲线窗口导入原版合法交叉／端点 X 控制柄，静默变线性 | 仍存在 | 共用解码器，仍存在 | [animation_curve_motion.cpp:380](../../src/animation/animation_curve_motion.cpp#L380)、[animation_curve_host_ui.cpp:219](../../src/window/animation_curve_host_ui.cpp#L219) |
| 10 | P2：相机导出／烘焙绕过真实原生采样器，改变运动 | 普通原生导出和烘焙仍存在；保形区间导出已有局部修复 | 共用导出及烘焙，仍存在 | [animation_curve_host.cpp:657](../../src/animation/animation_curve_host.cpp#L657)、[channel_bake.cpp:1030](../../src/animation/channel_bake.cpp#L1030) |
| 11 | P2：含未匹配骨骼的 VPD／跨模型姿态粘贴留下空 Undo 项，撤销改写骨 0 | 仍存在 | 已按真实匹配目标迁移，避免空项 | [vpd_file.cpp:325](../../src/io/vpd_file.cpp#L325)、[native_vpd_import.cpp:151](../../src/plugins/native_vpd_import.cpp#L151)、[command_control_450.cpp:305](../../src/window/command_control_450.cpp#L305) |
| 12 | P2：删除相机零帧、合入第 1 帧时遗漏父模型和父骨骼 | 仍存在 | 私有计划也遗漏，仍存在 | [frame_line_edit.cpp:164](../../src/window/frame_line_edit.cpp#L164)、[native_frame_lines.cpp:268](../../src/plugins/native_frame_lines.cpp#L268) |
| 13 | P2：骨骼范围伸缩先删后注册，丢失 Free／Scale 曲线 | Scale 已修；Free 仍受中间协调影响 | 已通过私有帧映射及 Scale 曲线迁移 | [frame_range_apply.cpp:146](../../src/window/frame_range_apply.cpp#L146)、[frame_range_apply.cpp:174](../../src/window/frame_range_apply.cpp#L174)、[native_bone_registration.cpp:1115](../../src/plugins/native_bone_registration.cpp#L1115) |
| 14 | P2：自动插值覆盖已有骨骼键，邻键手柄不在撤销记录中 | 仍存在 | `Smooth` 捕获全部实际写入，已迁移 | [model_keyframe_edit.cpp:400](../../src/model/model_keyframe_edit.cpp#L400)、[native_bone_registration.cpp:1229](../../src/plugins/native_bone_registration.cpp#L1229) |
| 15 | P2：单独粘贴 Morph／显示 IK 无历史，混合粘贴只撤销骨骼 | 仍存在 | 三家族及其组合共用一次事务，已迁移 | [command_control_400.cpp:2152](../../src/window/command_control_400.cpp#L2152)、[native_key_paste.cpp:39](../../src/plugins/native_key_paste.cpp#L39)、[native_model_keys.cpp:431](../../src/plugins/native_model_keys.cpp#L431) |
| 16 | P2：物理编辑删除首项后，显示选中项与内部 scratch 索引不一致 | 仍存在 | 共用旧编辑器，仍存在 | [physics_model_dialog.cpp:1482](../../src/window/physics_model_dialog.cpp#L1482)、[physics_model_dialog.cpp:1508](../../src/window/physics_model_dialog.cpp#L1508) |
| 17 | P2：物理模式切换未同步 `staticFlag`，重开恢复旧模式 | 仍存在 | 共用旧编辑器，插件快照也读取旧类型 | [physics_model_dialog.cpp:1578](../../src/window/physics_model_dialog.cpp#L1578)、[scene_definitions.cpp:53](../../src/plugins/scene_definitions.cpp#L53) |
| 18 | P2：PMX 独立 toon 索引缺少上界检查 | **已动态复现访问冲突** | 私有 `model_import::Preparing()` 范围内已有上界检查；普通加载仍受影响 | [pmx_load.cpp:916](../../src/model/pmx_load.cpp#L916) |
| 19 | P2：缺失路径恰为 `pmd`／`wav`／`vac`，反向扫描越过字符串起点 | **已动态复现访问冲突** | 共用解析器，绑定不提供修复 | [path_resolve.cpp:104](../../src/model/path_resolve.cpp#L104)、[path_resolve.cpp:125](../../src/model/path_resolve.cpp#L125) |
| 20 | P2：自动备份提前返回，漏发原生 MME 保存通知及对应 EMM | 仍存在 | 插件侧车事务不等于 MME 通知，仍存在 | [pmm_save.cpp:1326](../../src/io/pmm_save.cpp#L1326)、[pmm_save.cpp:1341](../../src/io/pmm_save.cpp#L1341) |
| 21 | P2：非 30 FPS H.264 用时间轴帧数判断输出张数，成功后自动关闭失效 | 仍存在 | 共用完成判断，仍存在 | [stubs.cpp:89](../../src/unported/stubs.cpp#L89)、[mf_h264_recorder.cpp:1367](../../src/app/mf_h264_recorder.cpp#L1367) |
| 22 | P2：Kinect 竖直／重合骨段水平长度为 0，除法产生 NaN 四元数 | 仍存在 | 共用数学路径，仍存在 | [query_audio_gaps.cpp:635](../../src/features/query_audio_gaps.cpp#L635) |

原版配置已对第 3 项使用临时文件、检查写入和原子替换，不能把完整配置的缺陷外推到这条受保护保存路径。第 21 项 H.264 在原版配置明确不可达。三个物理编辑问题对原版中允许编辑的普通 PMD 仍适用；其余项目应按实际入口和功能可用性判断，不能仅凭配置名称认定消失。

## 本轮动态证据

两个最小验证程序链接 `22ac942` 已编译的 ABI 核心库，不包含未编译的跟随草案。源文件、构建记录及结果均位于 `build/abi-main-review-probes/`。

| 程序 | 缺陷输入 | 缺陷结果 | 对照结果 |
| --- | --- | --- | --- |
| `path_short_probe.exe` | 将缺失的 `pmd` 放在可读页起点，前页设为不可访问 | `0xC0000005`；记录后退出码 86，地址落在前页 | `file.pmd`：退出码 0，无访问冲突 |
| `pmx_toon_probe.exe` | 最小 PMX 的纹理数为 0，独立 toon 索引为 0 | `0xC0000005`；记录后退出码 86 | toon 索引改为 -1：退出码 0，无访问冲突；加载器返回 false |

第二项对照只证明不再进入同一越界，**不证明该最小文件被当作完整有效模型接受**。退出码 86 是验证程序记录访问冲突后的约定退出码，是缺陷证据，不是功能测试通过。

有效构建日志为 `build-2.log`，最终结果为 `results.json` 及四份 `*-issue.log`／`*-control.log`。此前 `build-1.log` 对应的命令文件生成错误，没有产生程序；`not-run-results-1.json` 没有有效运行结果，不计入证据。

本轮没有重新构建正式 MMS，也没有运行其他条目的新 GUI、磁盘故障、真实编码或 Kinect 测试。既有 18/18 回归通过只代表原有断言，不覆盖本表新增反例。

## 需要补足的精确回归

- 第 1 项：灯光、自阴影、重力分别建立 `[0,30,60]`，仅选零帧删除；后继必须仍可求值、绘制、撤销和重做。受控入口还需真正实现删除，不能仅吞掉消息。
- 第 2、16、17 项：删除首刚体后新增／粘贴关节、连续删除、刚体删除连带删除关节、空表编辑，以及三种作者模式的新增／粘贴／切换／提交／重开。有效性检查须发生在释放原物理对象之前。
- 第 3、8 项：完整配置 PMD 短写、flush／close／替换失败保留旧字节及旧路径；通过打开菜单、最近工程及拖放拒绝工程后，快速保存仍指向最后成功打开的文件。原子保存不自动保证保存目的地正确。
- 第 4、5、6、21 项：空值／0／0.5／29.97 FPS；下游主动失败；收尾消息派发中再次停止；15／30／60 FPS 成功完成计数。失败应传播且等待可终止，收尾必须幂等。
- 第 9、10 项：将原版合法 `(100,10,20,115)` 和 `(0,127,127,0)` 控制柄直接送入窗口 VMD 解码；对尚未做过保形插帧的普通原生镜头，分别验证导出和烘焙，逐点与真实原生采样比较。已有“保形区间导出”通过不能替代这两条路径。
- 第 11 项：现有全未匹配测试外，补“部分匹配，骨 0 为 sentinel”的 VPD、普通及镜像粘贴撤销；不得产生空记录或改变未命中骨骼。
- 第 12 项：根与第 1 帧采用不同有效父模型／父骨骼，删除零帧后继承第 1 帧绑定；当前帧和换帧后的 undo／redo 均验证完整负载。现有两帧均为 `-1` 的测试无法发现遗漏。
- 第 13、14、15 项：分别保留普通路径的 Free 范围重定时、三个非零键 10／20／30 覆盖中间键、单 Morph／单 Display 及无 Bone 混合粘贴反例；共同历史已有修复不应被修旧环的工作回退。
- 第 18、19 项：将本轮最小崩溃输入转为长期回归，覆盖普通、私有导入及相应原版编译路径。边界检查应位于共用读取点，而不是只保护一种调用方式。
- 第 7、22 项：真正连续的三分量数组、普通方向、竖直／近竖直／重合关节的有限四元数；数学回归不依赖 Kinect 硬件，真实跟踪验收另记。
- 第 20 项：备份与手动保存分别验证通知及同名 EMM。普通保存后立即退出的“通知入队、下一次 BeginScene 才写 EMM”风险还需独立时序测试，不能冒充本轮已复现结论。

## 修复顺序与兼容边界

优先处理七项 P1：关键帧链保护、物理索引及提交前验证、完整配置 PMD 事务保存、录制输入／失败传播／幂等收尾、Kinect 向量布局。随后处理工程保存目的地、两项已复现越界及 VMD／相机数据正确性，再补其余撤销和界面状态问题。

main 的修复须按对应函数和测试精确比较，再决定移植。ABI 已有名称所有权、私有模型导入、作者曲线和共同历史事务，不能用整文件覆盖替代整合。保留 Win7 SP1 x64、CPU 基础路径和 AMD 兼容；此次不引入系统或显卡新依赖。

VMD 修复须保持标准版本、字段、原版合法控制柄及运动含义。不能把拒绝转换后线性化视为兼容，也不能用改动原生 sampler 来使测试通过。相机绑定修复应补全负载，不改变既有文件布局。

本表没有证明 MME 现场崩溃与这些缺陷同源。main 的参考证据定位到 `MMEffect!OnBeginScene` 首次 FX 编译阶段，实际机器的 D3DX、效果池、D3D9 代理链和 FX 仍未确定；不能宣称已找到同一崩溃根因，也没有证据要求回退 PMM v3。

## 核对材料

- main 汇总及详细报告：`D:/MMD-Next/MMS/.local/reports/`，包括 `curve-review-20261018.md`、`render-physics-media-review-20261018.md`、`model-editing-deep-review-20261018.md`、`window-state-deep-review-20261018.md` 及 MME 对比记录。
- 本轮分工笔记：`build/abi-main-review-curves-notes.md`、`build/abi-main-review-media-notes.md`、`build/abi-main-review-physics-notes.md`。
- 两项实际运行：`build/abi-main-review-probes/results.json`。
- [剩余工作核对](abi-remaining-work-2026-10-17.md)、[实现与验收记录](../PLUGIN_ABI_IMPLEMENTATION.md)。
