# 视角滑杆共同历史迁移

本批在 `D:/MMD-Next/MMS-abi-dev` 的 `codex/plugin-abi` 迁移主窗口视角滑杆 `447`。普通宿主仍不开放 `mms.edit`，本批没有新增公共 C ABI。

## 实现范围

`HandleHScroll` 在旧写入前调用内部 `TryCommitBoundCameraFovSlider`。已绑定共同历史时，该函数消费成功、无操作及拒绝，不回落旧写入。未绑定与原始兼容 profile 保留旧路径。

滑杆读取实际 `TBM_GETPOS`，按旧路径将整数转换为 float；范围限制仍由原生 trackbar 执行，不增加独立截断规则。滑杆编辑复用 `CameraProjectionChange`，每个有效位置立即记录一条共同历史。重复位置和结束通知不增加节点、不清除 redo。当前没有把整次拖动合并为一个节点。

撤销恢复精确旧 float，例如 `45.125f`，不从整数读数重建。记录只修改 FOV，保留后来切换的透视或正交模式、相机姿态、播放头、关键帧和模型旧撤销环。成功后按既有规则激活相机轨道，UI 与投影安装在共同历史发布完成后执行。

滑杆自身的捕获可用于实时编辑；普通 FOV 和透视 API 不能借用此例外。入口校验实际主窗口滑杆 HWND；播放、录制、其他模态编辑、不相关捕获及未完成插件事务仍拒绝。准备失败时不修改作者值或历史，已移动的滑块恢复当前作者值。历史记录继续校验相机键池身份，扩容可往返，替换后拒绝旧记录。

## 验证结果

| 检查 | 本轮结果 | 日志 |
| --- | --- | --- |
| Win7 x64 配置构建 | 宿主、原生历史、实际宿主测试及 ABI 目标通过。 | `build/abi-fov-slider-build.log`、`build/abi-fov-slider-host-build.log`、`build/abi-fov-slider-host-rebuild.log` |
| 原生专项 | 539 项通过。包含旧路径转换对照、范围外刻度、重复通知、精确撤销、跨模型模式与帧、键池身份、逐点 C++ 分配失败和捕获约束。 | `build/abi-fov-slider-only.log` |
| 共同历史与 ABI 回归 | 原生历史 104,594 项、ABI 契约 1,694 项通过。既有阴影滑杆等检查包含在原生历史中。 | `build/abi-fov-slider-regressions.log` |
| 实际宿主与测试 DLL | PMX 原名及中文改名 PMD 均退出 0。覆盖自身捕获、插件待提交事务拒绝、逐位置记录、交错撤销、透视及正交设备矩阵、PMM 保存和通用工程重开及退出清理。 | `build/abi-fov-slider-reviewed-history-host/report.json` 及对应 `result.log` |
| 独立只读 review | 未发现捕获例外扩大、换算偏差或历史发布顺序的阻断问题。 | 当前会话复核记录。 |

首轮实际宿主测试使用不存在的测试菜单名 `Edit probe begin`，在菜单查询处失败；改为测试插件已有的 `Edit probe prepared` 后重建并在新目录运行，两种夹具均通过。旧失败日志保留于 `build/abi-fov-slider-history-host.log`，不计为通过。

专项文件 `fov-slider-perspective.pmm` 和 `fov-slider-ortho.pmm` 检查保存；完整宿主另有通用工程重开验证，不将其等同为这两个专项文件分别重开。分配故障覆盖准备阶段 C++ 分配，不包含系统 malloc 或 GPU 故障注入。

## 限制与后续

实际宿主使用独立隐藏窗口和消息分派，未新增真实鼠标拖动、Win7 或 AMD 设备验收。继承投影安全刷新对零或极小正交距离的保护；已有 FOV 文本宿主检查仍通过，但本批滑杆专项没有独立覆盖所有异常投影参数。原始兼容 profile 未单独重建。

本批不修改 main、配置路径或稳定版程序，不关闭用户正在使用的实例。Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求保持。灯光、Morph 等原生写入迁移仍需继续，普通 `mms.edit` 保持关闭。
