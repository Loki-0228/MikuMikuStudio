# DATA 绑定的自动刷新订阅

本批新增 `mms.ui.refresh.subscribe` 1.0。插件给已有 DATA binding 注册订阅，发布新的完整来源快照后，宿主通过原队列和内部票据执行所请求的 DATA、STATE、LAYOUT、REPAINT。C++17 包装继续使用同一来源、绑定、目标及订阅句柄。接口与开发规则见[订阅参考](../../sdk/mms/refresh-subscriptions.md)。

## 实现范围

每订阅最多一张在途票据；未入队的连续发布合并为最新版本。显式旧票据保持原快照，自动订阅不覆盖该票据。来源字节相同时不新增版本或刷新。注册未发布的来源会等待首次发布；注册已有快照的来源会安排现有版本。

发布只在绑定锁内更新有界邮箱，释放锁后才通知原队列。工作线程不调用 UI 提供者。UI 安全点按原阶段顺序执行，并在回调后复核对象、绑定和工程代次。注销来源、解绑、删除目标、工程切换及插件停止清除相应订阅。UI 安全点发现已销毁的原 HWND 时也会清除目标及绑定，来源可以独立存活。

等待条件区分队列容量、显示模型容量、目标重新发布、提供者失败和唤醒失败。已发布快照的真实释放才推进显示容量版本，未提交候选回滚不制造恢复事件。两个失败的提供者不会因彼此票据结束而持续重试。容量在失败与记录等待状态之间释放时，通过版本追赶保留恢复机会。独立依赖及拓扑失效释放票据时补发原宿主唤醒消息。

本服务只支持已有显式 DATA binding 的 SELF。自动展开依赖图、原属性变化监听、插件量测提供者、独立后台作业和动画时钟仍未实现。普通生产编辑服务的授权范围未扩大。

## 验证

证据保存在 `build/verification/abi-refresh-subscription-20261021`。窗口测试串行继承 Default 桌面，仅处理自身窗口和线程消息，不读取桌面像素或发送全局输入。

| 检查 | 已运行结果 |
| --- | --- |
| 队列、SDK 及真实 DLL 专项 | 两配置各 6,096 项，退出 0。[完整配置](../../build/verification/abi-refresh-subscription-20261021/full-final-08/report.json)、[原始兼容配置](../../build/verification/abi-refresh-subscription-20261021/original-final-08/report.json)。 |
| 独立存储合约 | 两配置各 1,512 项，退出 0。[完整配置](../../build/verification/abi-refresh-subscription-20261021/full-store-08/report.json)、[原始兼容配置](../../build/verification/abi-refresh-subscription-20261021/original-store-08/report.json)。 |
| 原创 SDK 示例的实际宿主命令 | 两配置各 22 项，退出 0，见上述专项报告。首次调用延迟绑定，随后三个发布使原按钮勾选交替改变；排队任务在停止前取消。 |
| 完整 ABI 合约 | 两配置各 30,463 项，退出 0，见对应最终报告。 |
| 示例独立构建 | 固定 SDK 10.0.19041.0，独立 CMake 配置和 Release DLL 构建成功，日志 `example-configure-01.log`、`example-build-01.log`。 |

真实 DLL 测试通过公开服务协商，验证 worker 只发布即可自动刷新、相同字节不重做 DATA、连续发布只消费最新版本，以及显式旧快照先于自动新快照消费。prepare 中的编辑 begin、undo、redo、UI 注销、订阅增删和再发布被拒绝；真实共同历史修订、保留字节、undo/redo、dirty 发布和存储计数保持不变。Manager 夹具没有真实 MMDApp 播放头或场景求值适配器，不能据此宣称这些计数已经真机验收。

停止测试用真实显示租约保持 DLL 存活，确认旧订阅 `STALE`、失败输出保持、排队工作取消、后续发布被拒绝及最终卸载。SDK 的独立严格假宿主还验证失败协商、表前缀、异常边界、工作线程查询、跨线程移动不改变 UI 归属、清理失败保留句柄及 detach。

复查追加了未订阅来源缩小快照、取消旧贡献、状态提供者失效、删除依赖、解绑及注销来源释放容量的回归。它们使用真实 2 MiB 预算和自有窗口消息，恢复时不新增来源发布或显式调用 Drain。FailPending 及唤醒失败继续停泊，双失败提供者不空转。正常 Drain 的 groups 尾释放由原有作用域保护追赶。Request 在捕获旧快照后与发布并发的精确窗口，仅完成资源及锁析构顺序审查，未作确定性竞态运行验收。

## 保留的失败记录

首轮 `full-subscription-01` 在 4,244 项后失败。准备代码提交 256 张旧票据却只处理一轮，而原队列每轮最多 64 项。改为按公开预算排空，未修改队列预算来迎合测试。

`full-subscription-02` 在新 DLL 的初始绑定前失败。测试在创建 DATA binding 前错误要求目标已经具有 DATA 能力；现先检查原呈现的 STATE、REPAINT，再实际绑定。`full-subscription-03` 的完成等待失败，因为 `Manager::Drain` 只消费 UI 任务，刷新沿用宿主的独立消息入口。测试现派发自身线程的宿主消息，未直接调用内部刷新回调或改变生产调度。

`candidate-01`、`candidate-02`、`candidate-05` 保留早期源码及二进制。[最终清单](../../build/verification/abi-refresh-subscription-20261021/candidate-08/manifest.json)共 35 项，包含两套产品，运行报告记录 EXE 的运行前后哈希。构建仍包含既有测试析构异常警告，不能记为零警告。

## 原创示例与边界

[auto_refresh](../../sdk/mms/examples/auto_refresh/plugin.cpp) 由 DSH 完成四个原创代码文件，Codex 负责设计约束、整合、独立编译及真实宿主验收。真实会话为 `a23b44a2-e1e0-4dd7-9865-e23d601d2b48`，返回 cwd 为 `D:\MMD-Next\MMS-abi-dev`。正式结果和增量通知保存在证据目录的 `dsh-result-01.json`、`dsh-events-02.txt`、`dsh-events-03.txt`。中途额外 skill 调用报 `CodeRunFailedError/CODE_RUN_FAILED`，随后停止该额外调用；不能把工具失败改写为未发生。

两套产品构建成功。PE 均为 x64、子系统 6.1，普通及延迟导入与前批一致；正式 release 哈希未变，见[产品清单](../../build/verification/abi-curve-insertion-geometry-20261021/pe-products-final-08.json)。未修改 main 或正式 release；不新增第三方配布内容。Win7 SP1 x64、CPU 和 AMD 基础要求保持，未完成对应真机验收。旧的[曲线堆链表故障](abi-curve-heap-observation-2026-10-21.md)不因本批订阅测试通过而关闭。
