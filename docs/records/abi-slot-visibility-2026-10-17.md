# 保留槽位按钮显隐验证（2026-10-17）

本批基于 `codex/plugin-abi` 的 `cd8f384`，工作目录为 `D:/MMD-Next/MMS-abi-dev`。继续实现统一刷新：按钮 STATE 在有效 `MMS_HINT_PRESERVE_HIDDEN_SLOT` 槽位内支持显隐。接口规则见 [C 参考](../../sdk/mms/refresh.md)和 [C++ SDK](../../sdk/mms/cpp-refresh.md)。

## 实现范围

继续使用原 C 表、结构版本、元素句柄、实例代次和票据队列。已有正尺寸槽位可以隐藏或恢复按钮，位置、尺寸及相邻控件不变。恢复遵守最近布局的祖先可见性，不显示隐藏的父区域。隐藏后焦点仍在按钮时，移到可用的相邻控件或父窗口；恢复不抢回焦点。

缺少有效保留槽位提示时，显隐变化在修改启用、选中前返回 UNSUPPORTED。布局提示修改后尚未重新发布时返回 NOT_READY。请求入队成功不代表操作已完成，插件必须查询票据。

逻辑显隐变化更新刷新目标和公开布局几何修订。新布局快照反映新可见性，旧租约保留原值。UI 修订、偏好修订和序列化布局偏好不变。没有重新测量、重新排版或收缩空位，也没有调用旧 Refresh、FrameDriver、工程编辑和历史入口。

`RefreshStateCurrent(ref, ticket, key)` 是原 request 的 C++17 便利封装。STATE 没有坐标，此方法接受执行时的几何，允许工作线程在显隐后继续使用同一目标引用。实例、工程、ready 和有效布局检查仍然生效。原 `RefreshState` 继续严格匹配捕获的几何。

合并队列现在区分固定几何与接受当前几何的请求。过期局部请求不会使相同键的合法整目标重绘失败。显隐回调中取得的目标仍是上次发布值，后续基于旧几何的局部请求会过期。

## 稳定性审查

队列锁不跨插件回调或原生窗口操作。状态查询与应用保持只读保护，防止递归 STATE 和借用编辑权限。原生显隐、焦点及最终状态读取后重新校验目标。显隐回调泵消息导致工程切换时，暂停同一代次的目标，正常布局重新发布后恢复；不会将旧镜像继续报告为就绪。

公开布局显隐发布不分配内存、不调用回调，仅修改已有几何记录。所有新增原生窗口调用均为已有 Win7 系统接口。C ABI 大小和版本保持不变，未开放普通启动中的 `mms.edit`。

## 验证结果

证据路径相对于 ABI 工作目录。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 构建 | 完整宿主、契约程序、侧栏和场景历史目标通过，使用 Win7 x64 配置。 | `build/refresh-slot-build-final.log` |
| 刷新专项 | 累计 7,764 次断言通过。包括后台显隐、焦点离开及恢复不抢焦点、槽位和相邻控件不动、新旧几何租约、偏好不变、父区域隐藏、过期布局策略、显隐回调中的工程切换、固定与当前几何分组及同一引用的 SDK 后台往返。 | `build/refresh-slot-focus-final/result.log`、`report.json`、`tests/plugins/visibility_refresh_checks.inc` |
| 真实 DLL 与 Manager | 经真实插件注册保留槽位按钮，通过原队列隐藏和恢复；原有状态、停止保活、释放卸载用例继续通过。 | 同上及 `plugin.log`、`tests/plugins/manager_refresh_checks.inc` |
| 既有 ABI 与历史 UI | 既有 ABI 1,694 项、场景历史 UI 回归通过。 | `build/refresh-slot-regressions-focus.log` |
| 原名及中文改名宿主 | 隐藏生产启动、菜单、工具栏、面板、SDK 共享命令状态和退出通过。 | `build/refresh-slot-smoke-focus.log`、`build/refresh-slot-sdk-focus.log` |
| 真实混合历史 | PMX 原名及 PMD 中文名宿主通过插件与原生交错编辑、模态回调、修订、工程重开及停止清理。 | `build/refresh-slot-history-final.log`、`build/refresh-slot-history-final/report.json` |
| 帧调度 | 纯 UI 绘制、忙碌恢复、播放、动态刚体及无消息时的时钟推进回归通过。 | `build/refresh-slot-scheduler.log` |
| PE 清单 | 四个二进制的导入与架构已记录；宿主 x64，子系统 6.1。未新增图形后端或厂商依赖。 | `build/refresh-slot-pe-final.json` |

首次专项 `refresh-slot-contract-1` 的恢复检查失败：测试把目标与相邻按钮绑定到同一显隐命令，显式正常刷新同时隐藏两者。将相邻按钮改为独立常显命令后通过，再增加 SDK 及焦点回归。失败运行不作为成功证据，最终结果来自 focus-final。

刷新专项使用真实窗口及 Manager，不包含完整场景和物理模型。没有将其称为 STATE 场景求值计数实测；该路径不自动求值的判断依据源码审查及独立调度、历史回归。未进行像素残影或真机多 DPI 验收。原生状态应用失败后不承诺事务回滚。

## 构建隔离与待办

ABI 宿主 SHA256 为 `CADA18092445217FA041D37634E51353DF3164839C78AAE66E98FA28F6819580`。main 工作树仍无修改，稳定程序 `D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe` 的 SHA256 仍为 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`。

完整 STATE、DATA、LAYOUT、收缩空位的显隐依赖、刷新绑定和依赖范围仍待实现。Win7 SP1 x64、CPU 基础路径和 AMD 要求保留；真机和运行库部署尚未验收，静态导入清单不替代这些结果。
