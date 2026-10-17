# 灯光文本共同历史验证（2026-10-17）

本批基于 `codex/plugin-abi` 的 `df77e11`，仅在 `D:/MMD-Next/MMS-abi-dev` 开发。灯光颜色及方向文本 `461..466` 接入受控共同历史，普通 `mms.edit` 仍关闭。

## 实现和交互

`CommitBoundLightNumber` 在旧作者写入之前接管六个主面板字段。仅修改标志为真时提交，并在失焦过程中由提交入口消费标志。重复失焦和 Enter 不写回已舍入的读数。先验证完整输入为有限数字，再保留原七字符读取窗口：RGB 仍按整数解析后除以 256，方向按旧浮点规则读取。完整非法、超长或非有限输入在写入前拒绝，不使用合法数字前缀掩盖无效尾部。

`ApplyBoundLightField` 通过 `LightPoseChange` 准备六项原始颜色／方向及单字段目标，全部准备和历史预留成功后才发布。发布只修改涉及字段，并通过原 `ApplyTimelineLightState` 重建 CPU `SceneLight` 的 Direction、Ambient、Specular 及方向灯类型；不分配、不调用窗口或设备。没有写入灯光键、曲线或模型旧环，不改变播放头。

成功后按旧交互激活灯光轨道，更新读数和配对滑杆。设备 `SetLight` 在已提交后的全局读数刷新中执行，共同撤销和重做复用该刷新。插件历史回调期间仍只发布 CPU 数据，窗口及设备更新延后到宿主正常安全刷新，不在原子发布阶段调用 GPU。CPU 派生镜像由原始标量重建，不把任意不一致的旧镜像作为独立作者数据恢复。

未修改读数、同值、忙碌、无效输入和准备失败不标 dirty，也不截断 redo。普通文本框没有独立取消对话框；未提交输入不创建工程历史。

## 复核边界

记录保存键池身份及方向代次。正常扩容保留身份，已有历史继续有效；替换池使旧记录过期。短期候选同时检查帧号、六项标量和完整 CPU 灯光镜像，防止准备后覆盖变化。长期撤销只恢复本次涉及字段，其他作者值和镜像中的无关属性保持。

工程提交成功后的可选 UI 通知失败不重新报告成编辑失败。原始兼容 profile 与未绑定路径保留旧实现，本批没有单独重建原始 profile。直接滑杆写入仍未迁移。

## 本机证据

证据路径相对于独立 ABI 工作目录。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 构建 | 完整宿主、原生历史、ABI、侧栏和历史 UI 目标通过 Win7 x64 配置构建；新增宿主及原生回归分别重建。 | `build/light-text-build-final.log`、`build/light-text-host-reviewed-build.log`、`build/light-text-reviewed-tests-build.log` |
| 原生专项 | 580 项通过。六字段旧换算、真实失焦函数、滑杆同步、未修改精度、非法输入、不同帧往返、CPU 镜像一致性、池扩容及替换覆盖。 | `build/light-text-focused-final.log`、`tests/native_light_text_checks.inc` |
| 分配失败 | 六字段逐点 C++ 分配失败保留原始值、CPU 镜像、dirty、旧环及 redo；失败后预留可重用。 | 同上 |
| 完整回归 | 原生历史累计 96,986 项、ABI 1,694 项及历史 UI 回归通过，三组 CTest 全部通过。 | `build/light-text-reviewed-regressions.log` |
| 真实控件与插件 | PMX 原名、PMD 中文名的六个实际 EDIT 接收失焦／Enter；插件未结束事务拒绝、精确转换、重复失焦、同值和非法输入保留 redo，通过真实 DLL 与原生命令交错 undo／redo。 | `build/light-text-host-reviewed/report.json`、各 case 的 `result.log`、`tests/plugins/light_text_host_checks.inc` |
| 实际设备读回 | 提交、插件 undo 及跨模式 redo 后读取真实 DX9 `GetLight`，颜色 Ambient／Specular 和 Direction 与 CPU 镜像及原始值一致；插件方向先完成宿主延后刷新。 | 同上 |
| 保存及生命周期 | 已提交灯光文本和插件状态可保存 PMM，历史保持；既有新建、未结束事务、重开、继续编辑和关闭取消通过。灯光键、模型姿态和播放头保持。 | 同上及 `build/light-text-host-reviewed.log` |
| 普通宿主及 PE | 原名、中文改名隐藏启动、菜单、工具栏、面板和关闭通过；宿主 x64、子系统 6.1，未新增图形或厂商依赖。 | `build/light-text-smoke.log`、`build/light-text-pe.json` |

首版原生测试缺少公共控件头，编译失败；补 `<CommCtrl.h>` 后通过。首版真实宿主在插件 undo 返回后立即读取 GPU，CPU 已正确恢复但延后通知尚未运行，因此失败。最终测试在原宿主安全刷新完成后读取设备，保持既有回调边界，没有把 GPU 更新放进原子发布。失败证据为 `build/light-text-tests-build.log`、`build/light-text-host-final`，不计作最终通过结果。

专项没有注入系统 malloc、设备丢失、SetLight 失败或提交后 UI 通知失败。实际设备读回不是完整桌面视觉验收；没有验收完整浮动窗口的鼠标、输入法或多 DPI。生命周期结果不表示撤销节点跨工程重开保留，重开仍清历史。

## 隔离和剩余工作

ABI 宿主 SHA256 为 `84BB07CF9912DED08139E01D3A829E1692184F2E626487C07814F30B035D2028`。main 工作树干净，稳定程序 SHA256 保持 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`，没有覆盖、发布或部署。

剩余原生迁移包括视角文本、相机／灯光等直接滑杆、鼠标拖动与滚轮、其他姿态及附件编辑、关键帧粘贴／镜像粘贴、全局删键、插帧／删帧、VMD／VPD 导入、物理烘焙、Morph 散布、捕获、图形曲线编辑器及其他工程写入。当前范围见[入口表](../PLUGIN_EDIT_INTEGRATION.md#当前原生撤销入口)，全部原生写入统一受控前普通 `mms.edit` 继续关闭。

其他 ABI 需求仍按[实施表](../PLUGIN_ABI_IMPLEMENTATION.md)推进，包括 DATA／完整 STATE／LAYOUT 与依赖刷新、UI 挂载和本体语义登记、布局历史、多视口及独立求值、渲染和辅助绘制、数据格式与编辑工具、原创旧接口适配。

Win7 SP1 x64、CPU 基础路径与 AMD 要求保留，真机及运行库部署仍待验收。
