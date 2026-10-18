# SELF DATA 刷新与同一 ABI 的 SDK

日期：2026-10-19。工作目录 `MMS-abi-dev`，分支 `codex/plugin-abi`。本批在原 `mms.ui.refresh` 队列上加入 `DATA + SELF`，新增 96 字节 `mms.ui.refresh.bind` 1.0 表；原刷新表、请求和状态结构尺寸不变。

## 已接入范围

插件在 UI 线程注册来源，将其绑定到已有按钮或 surface 面板的原目标引用。工作线程发布字节时，宿主复制输入并生成来源修订；相同字节不推进修订。DATA 入队捕获精确版本，即使随后发布新版也不改变已接受内容。尚未捕获的旧版本不能冒充当前版本入队。

安全点按 DATA、STATE、REPAINT 执行显式组合。prepare 通过短期 builder 准备一次完整显示候选，目标复核成功后才交换不可变模型；失败保留旧显示，并终止该组合的后续阶段。相同已消费版本不重复 prepare。纯 DATA 不重绘或重新查询状态，显式组合仍执行所请求阶段。

绘制与按钮状态读取当前显示租约，不在 WM_PAINT 中执行业务准备。旧租约不随后续交换改变。目标重建、工程切换、注销和停止均有失效规则；调用、票据或租约尚未结束时保留插件实例、context 和 DLL。

只读保护覆盖准备和交换。由同插件 DATA/STATE 回调安排的 UI run、cancel 及后代任务继续受保护，不能递归请求 DATA/STATE 或申请编辑。STATE 同时保护元素 owner 和实际命令提供者。另一个插件独立后台任务不会因为与回调时间重叠而永久继承只读限制。

`mms/cpp/data_refresh.hpp` 提供 DataServices、DataSource、DataBinding 和 DataDisplay，复用原句柄、C 表及 RefreshTicket。释放失败保留句柄，显式 detach 可移交低层代码；便利层不建立第二套身份、任务或刷新实现。开发接口、环境、版本、配额和清理步骤见 [SDK 参考](../../sdk/mms/data-refresh.md)。

## 配额与边界

来源和显示各有 256 KiB 上限；每插件保留载荷 2 MiB，全进程 32 MiB。旧来源贡献和旧显示租约也计费。每插件来源、绑定、显示租约、builder、贡献及原票据均受独立数量上限约束，运行期读取能力表。

本批不提供来源自动订阅、跨插件绑定、LAYOUT、SUBTREE、DEPENDENTS 或完整 STATE，也没有开放普通宿主的 `mms.edit`。DATA 不移动播放头、不写工程、不求值场景或增加历史。实际含编辑宿主的 DLL 专项验证了 begin、undo、redo 均被拒绝，并保持历史修订、保留字节、pending、发布通知和 dirty 计数不变；本批没有新增场景求值接口。

## 已取得的验证证据

日志均位于本地 `build/abi-data-refresh/`，不作为发行内容。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| DATA 模块 | 666 项通过；精确版本、候选交换、错误保留、租约、配额及分配故障 | `pure-data-2.log` |
| 队列、SDK、真实 DLL | 897 项通过；面板绘制与按钮 STATE 顺序、四类准备失败、真实编辑门禁、取消与停止保活、双 owner 并发 | `data-runtime-2.log` |
| 既有刷新及其 SDK | 8,403 项通过；原重绘、STATE、显隐、焦点、Manager 票据保活和新 DATA 接线 | `refresh-runtime-1.log` |
| 既有 ABI 契约 | 两种配置各 1,804 项通过；真实插件加载、UI、共同历史与任务生命周期 | 下述最终 CTest 记录 |
| 最终完整／原始兼容配置 | 两种 Release 构建、18／16 组回归通过；各含 DATA 模块 666 项与真实 DLL/SDK 897 项 | `build-full-verified.log`、`build-original-verified.log`、`ctest-full-verified.log`、`ctest-original-verified.log` |

首次双 owner 专项在固定“一插件必须创建一个面板”断言失败。实际 NativeUi 只创建活动页；日志确认 A 的面板已创建，B 的非活动页尚未创建。修正为按每个 owner 的实际创建量验证销毁平衡，并保留每个插件只 destroy 一次的断言；没有把未创建页面记作销毁通过。

原始兼容配置的 ABI 与 DATA 结果来自独立契约测试程序。原始产品按既有 profile 禁用 ABI 运行时，本批未改变这一点。

上述结果属于本机软件验证，不代替 Win7、AMD 或多屏混合 DPI 验收。
