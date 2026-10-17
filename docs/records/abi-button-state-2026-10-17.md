# 按钮状态刷新验证（2026-10-17）

本批基于 `codex/plugin-abi` 的 `71ed6f7`，工作目录为 `D:/MMD-Next/MMS-abi-dev`。按统一刷新需求接入 STATE 首阶段，仅覆盖插件已创建宿主按钮的启用与选中。完整接口规则见 [C 参考](../../sdk/mms/refresh.md)和 [C++ SDK](../../sdk/mms/cpp-refresh.md)。

## 范围与实现

原 C 表、结构大小和版本保持不变。全局 supportedKinds 返回 REPAINT/STATE 并集，实际目标能力分别查询：宿主按钮支持二者，原生 surface 面板只支持 REPAINT。STATE 与原票据使用相同权限、配额、工程代次、实例身份、取消和停止规则。

STATE 在原消息队列的 UI 安全点只读查询目标命令 getState，再修改原 HWND 的 enabled 和 checked。C++ RefreshState 仅组合原 request。单独 STATE 不接受损伤矩形；与 REPAINT 组合时先计算状态，后提交失效区域。完成位按实际阶段记录，重绘失败不能抹掉已完成的 STATE 位。

同 owner、目标、合并键和 kinds 的请求只计算一次。不同 kinds 分开，避免 STATE 失败影响独立纯重绘。同值不重复发送原生状态修改。此路径不调用旧 Refresh、布局、FrameDriver、工程写入或共同历史入口。

显隐变化会影响尺寸和相邻排列，因此目前要求 visible 与最近成功布局时的命令值一致；不一致时在修改 enabled/checked 前返回 UNSUPPORTED。标签、属性内容、菜单、面板状态、显隐依赖、DATA、LAYOUT 和依赖范围均未开放，不将本阶段标为完整 STATE。

## 稳定性审查

队列锁不跨插件回调或原生窗口调用。getState 与原生状态应用保持只读回调分类，注册、注销和命令执行不能借此获得编辑权限。STATE 的 UI 调用链再次提交 STATE 返回 BUSY；批次重入也被拒绝。

新增回调后，原来只含异步失效的批次可能处理嵌套消息。现在每组执行前、状态回调后及原生状态操作之间重新检查目标。工程替换、实例或几何变化、布局暂停和停止会使旧工作失败，不继续使用旧目标。Callback 或 apply 失败后释放回调保护，后续合法请求仍可执行。

Review 另发现旧 NativeUi 刷新和布局可能在状态回调泵消息时再次查询 getState。两入口现延后呈现，合并通知且不修改注册表修订；宿主忙碌时用原 UI 定时器在回调结束后处理。实际布局完成后再发布几何，不把延后的窗口移动算成 STATE 完成条件。

宿主扩展按钮使用手动选中、推按钮外观。选中来源仍为 getState，点击不自行切换；Enter 显式路由原命令，Space 继续走原生按钮通知。未修改 MMS 本体既有参数按钮、菜单命令或编辑服务开放状态。

## 验证结果

证据路径相对于 ABI 工作目录。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 构建 | 完整宿主、契约程序、真实测试 DLL、侧栏与场景历史目标通过；保留 Win7 x64 配置。 | `build/refresh-state-safe-layout-build.log` 的宿主部分、`build/refresh-state-safe-layout-tests-build.log` |
| 刷新专项 | 累计 7,523 次断言通过，含状态轮询。新增后台 STATE、合并、取消、幂等、组合请求、STATE 失败与纯重绘分离、非法输出/异常、面板拒绝、显隐拒绝前状态不变、回调权限和递归门禁。 | `build/refresh-state-safe-layout-final/result.log`、`report.json` |
| 回调后过期 | 工程代次、实际几何及暂停在 getState 中变化后，旧操作失败；同批后续纯重绘也重新校验。 | 同上，`tests/plugins/state_refresh_checks.inc` |
| 旧刷新重入 | 测试用真实 NativeUi；getState 内调用 Refresh 和 Layout 不再次查询状态。回调结束后正常刷新恢复目标就绪，注册修订不变。 | 同上，DeferredNativeLayout 用例 |
| 真实 DLL 与 Manager | DLL 经 SDK 在后台提交 STATE；未调用命令或旧 Refresh/Layout 即改变真实宿主按钮的 enabled/checked。忙碌时延后，更新前后几何修订不变，停止保活及释放卸载继续通过。 | 同上及 `plugin.log`、`tests/plugins/manager_refresh_checks.inc` |
| 键盘与原控件路径 | Enter、Space 和原 WM_COMMAND 各经真实按钮/命令路径通过。原名与中文名宿主启动、菜单、工具栏、双面板和 SDK 共享状态通过。 | 刷新专项、`build/refresh-state-smoke-final.log`、`build/refresh-state-sdk-final.log` |
| ABI 与历史 UI | 既有 ABI 1,694 项、场景历史 UI 回归通过。 | `build/refresh-state-regressions-final.log` |
| 真实混合历史 | 原名 PMX、中文改名 PMD 各通过插件与原生交错编辑、模态回调后 UI、修订、保存重开和停止清理。 | `build/refresh-state-history-final.log`、`build/refresh-state-history-final/report.json` |
| 帧调度回归 | 纯 UI 绘制的零场景增量、视口暴露恢复、动态刚体保持、忙碌及播放调度通过。 | `build/refresh-state-scheduler-final.log` |
| PE 导入检查 | 四个二进制均为 x64，无所检查的 DX12、DXGI、d3dcompiler_47、CUDA、NVAPI 直接或延迟导入；宿主子系统 6.1。 | `build/refresh-state-pe-final.json` |

STATE 专项使用真实窗口与 Manager，但不包含完整场景/物理模型。因此不把该专项称为 STATE 的场景求值计数实测；宿主不会从该路径自动求值的结论依据源码审查及独立调度/历史回归。未注入 GDI 分配或原生状态调用失败，组合票据部分完成位的失败分支仍主要依靠审查。UI 状态不是工程事务，不承诺原生状态调用已发生后的事务回滚。

新增延后布局测试的首次编译因同一 auto 声明混用 unsigned 与 uint64_t 失败，拆分声明后通过。该失败构建不作为整组成功证据；最终测试使用 safe-layout-tests-build 构建。

## 重现与平台

原[私有桌面运行器](../../tests/plugins/run_refresh_checks.py)继续运行原刷新、SDK、STATE 和 Manager 专项；测试桌面不激活，用户窗口不被操作。普通 CTest、隐藏宿主及混合历史运行器沿用既有步骤。

ABI 宿主已重建，SHA256 为 `1B17556022C736C41E7B660D3ED3B3C67D787A4EA8F1406B7133FE697FCAF3EE`。main 仍为 `04d3dec`、工作树无修改；稳定程序 `D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe` 的 SHA256 仍为 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`。

Win7 SP1 x64、CPU 基础路径和 AMD 兼容要求保留，没有新增图形后端或厂商依赖。Win7、AMD 真机和运行库部署尚未验收，静态导入清单不替代这些结果。
