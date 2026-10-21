# 范围自动订阅与兼容回归（2026-10-22）

已实现 `mms.ui.refresh.subscribe` 1.1。一个已有显式 DATA binding 的发布可以驱动注册子树或显式依赖图，触发目标与范围种子分别声明。GROUP 使用原 UI 句柄，无需建立虚假来源或 binding。接口字段及开发规则见[范围订阅参考](../../sdk/mms/scoped-refresh-subscriptions.md)。

本批仅修改 `MMS-abi-dev`。普通生产 `mms.edit` 继续关闭；main 的未提交修改及正式 `build/release/MikuMikuDance.exe` 未由本批改写。

## 已实现范围

1.0 的 48 字节服务表、72 字节状态及 SELF 能力保持。1.1 在同一服务尾部追加 `subscribeScoped` 和 `queryScoped`，表大小为 64 字节。新增描述为 80 字节，扩展状态为 160 字节；全局 `MMS_ABI_MINOR` 仍为 0。新旧查询与注销使用同一订阅池和句柄。

UI 安全点展开 SUBTREE 或 DEPENDENTS。后台发布只更新有界通知状态，不遍历注册树、不执行 UI 提供者。一张原复合票据在一次 binding 锁内捕获准确触发版本和全部显式成员来源集合。整组依次执行 DATA、STATE、一次 LAYOUT 和 REPAINT；不将多个 SELF 请求拼接成范围刷新。触发控件只有实际属于范围时才消费显示。

来源发布推进工作代次，图或 binding 的变化另推进失效代次。后续数据发布不能改写已接受的旧集合；新版本由后续安全点消费。增删相关边可以在同一触发版本下重新安排工作。同版本已消费显示继续使用缓存，新增成员或失败成员按实际需要准备。

等待原因区分图、目标、队列容量、来源、消费者、binding、宿主唤醒及显示容量。重复 Drain 和无关票据完成不反复运行相同失败。来源恢复事件使用计数追赶捕获与停放之间的发布。种子失效、触发 binding 解除、来源注销、工程代次改变及停止清理原订阅；原显示租约和 DLL 保活规则保持。

DATA 发布保持原 1.0 线程规则。UI 只读回调直接发布仍返回 `BUSY`；工作线程可以发布不可变来源，即使同客户端旧版本正在 UI 上消费。宿主不把客户端相同等同于存在因果关系，也不因此阻断独立后台计算。该发布不直接执行 UI 回调，递归 Drain 无效。原属性通知服务继续使用自己的已发布协议。

`ScopedRefreshSubscriptionServices` 和可移动 `ScopedRefreshSubscription` 包装同一 C 表，提供子树、依赖图、原种子查询、基础及扩展状态、释放与所有权移交。未协商到 1.1 时不把多个 SELF 请求伪装为该能力。

## 审查与失败复现

证据目录为 `build/verification/abi-scoped-subscription-20261022/`。所有构建均等待全部作者停止写入；两套树各自构建，构建进程终止后再冻结源码和产物。早期候选及失败日志保留。

| 记录 | 实际结果与处理 |
| --- | --- |
| `candidate-01`、`scoped-full-01` | 第 1,183 项失败。真实字符串分配失败使注册树暂不可用；下一次成功 `SyncTopology` 在设置 `topologyReady_` 前观察订阅，导致仍等待 TARGET 且没有唤醒。修正为成功交换注册树后先发布就绪，再观察。回归要求单独恢复即可唤醒，不借 `SyncTargets` 或用户事件。 |
| `candidate-02`、`scoped-full-02` | 范围、SDK 与真实 DLL 专项 2,159 项通过。该结果未代表旧契约通过。 |
| `regressions-full-02` | 主任务误写 `--property-target-recovery-only`，第 1 项因默认入口要求 `argc == 4` 失败。实际参数为 `--property-refresh-recovery-only`；更正命令并保留原日志，未修改产品来适应错误命令。 |
| `regressions-full-02-corrected` | 原属性恢复、属性 DLL、历史信号通过；完整 ABI 在第 24,615 项失败。新增全客户端 worker 写入限制阻断原 `ExactAcceptedCohorts` 的合法后台发布。撤回该限制，原 1.0 测试不变；新增范围测试分别断言旧集合完成、递归无效及下轮新版本。 |
| 真实 DLL 阶段检查 | 独立审查发现只检查“出现过 STATE”不足以证明全部成员刷新。现要求实际 STATE 成员集合等于预期集合，同时保持全部 DATA 先于 STATE，以及同批显示值、版本检查。 |

工作线程发布恰落在捕获失败与等待登记之间的窄竞争窗口，以及计数器耗尽，尚未强制制造。首次订阅唤醒失败已经覆盖；不能据此宣称内部票据创建后的每一种唤醒回滚或所有分配失败路径均已验收。

## 最终构建与验证

最终源码与二进制为 `candidate-03/manifest.json`，共 35 个文件。构建日志为 `build-full-03.log` 和 `build-original-03.log`，退出码均为 0；包括产品、ABI C/C++、真实 DLL，完整配置另包含原生历史信号及实际侧栏宿主。构建仍有既有编译和中间目录警告，不是零警告结果。

最终运行清单为 `regressions-full-03/report.json` 的 11 组及 `regressions-original-03/report.json` 的 8 组。19 组均退出 0，执行前后 EXE 哈希一致。

| 检查 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| 范围订阅 C、C++17、原队列与真实 DLL | 2,176 项 | 2,176 项 |
| 完整 ABI 契约 | 38,336 项 | 38,336 项 |
| 原属性目标恢复 | 81 项 | 81 项 |
| 原属性 SDK 与真实 DLL | 2,751 项 | 2,751 项 |
| 原 DATA 与真实 DLL | 6,814 项 | 6,814 项 |
| 原 1.0 自动订阅与真实 DLL | 6,096 项 | 6,096 项 |
| 左右挂载 | 3,159 项 | 3,159 项 |
| 既有刷新恢复 | 390 项 | 390 项 |
| 共同历史发布信号 | 184 项 | 此目标未运行 |
| 实际 MMS 侧栏宿主 | ordinary、bound 均通过 | 此目标未运行 |

范围队列测试使用真实 64 票据预算延后已接受集合，验证后续同源、异源发布均不能替换旧快照。真实 DLL 仅查询公开 C 服务，使用原 toolbar 和属性根，覆盖空图、增删边、后台连续发布、同版本新增目标、移除成员、只读回调、原种子注销与租约停止；不提交显式刷新票据，也不通过重建 NativeUi 代替订阅执行。

测试继承 Default 桌面，窗口显示由夹具控制，不是独立桌面、全程隐藏、真实鼠标、IME 或多 DPI 验收。既有实际侧栏宿主两种模式各运行 7 个任务，属性读取分别 246、366，记录 `scene=0`、`physics=0`，同时核对原应用、键池及历史不变。该宿主结果是相邻集成回归，不能冒充范围 DLL 自身的场景求值实测。

`pe-products-final-03.json` 确认两套产品均为 x64、子系统 6.1，直接及延迟导入相对上一批没有增删。完整配置产品 SHA-256 为 `d237059ad645ce4c0df8f4439598e6c8781ca1e550be23516358364fa3ec87ae`，原始兼容配置为 `0f27129a93796899b325b37ce45863477018f071f2e0e647990419822e2a98ab`。正式 release 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。

## DSH 原子代码任务

本批使用 [dsh-delegate](C:/Users/lq/.agents/skills/dsh-delegate/SKILL.md)。真实会话为 `e34075b0-d14a-4715-827b-bd327746385e`，实际 `cwd` 为 `D:/MMD-Next/MMS-abi-dev`，来源绑定当前 Codex 线程。正式 result 及通知保存在证据目录；最终游标为 57，已明确停止写入并读取正式完成结果。

DSH 只追加 `tests/plugins/abi_c.c` 与 `abi_layout.cpp` 中的协议断言：自然对齐、固定 scope 与等待位、全局 minor 保持、仅追加两个槽，以及含 `MMS_CALL` 和 const 的精确函数类型。C11 新增 17 条断言，C++17 新增 12 条；原断言未删除。文档、运行时整合、图像及最终验收由 Codex 负责。

通知中曾出现 `run_code failed: CodeRunFailedError/CODE_RUN_FAILED`，另一次只读 diff 出现 `ResourceUnavailable: 程序'git.exe'运行失败： 拒绝访问`。这些不是编译通过证据；主任务重新核对实际文件增量，并在两套配置编译 C11 `_Generic`、`_Alignof` 及 C++17 `std::is_same`、`alignof`，均通过。没有重启 DSH、绕过认证或执行回收清理。

## 剩余范围

此批开放显式 DATA 来源的范围自动传播。后续[原属性与历史范围刷新](abi-scoped-property-2026-10-22.md)已补齐相应来源。通用事件自动传播、更多本体控件登记、布局会话、独立工作任务、多视口、任意时间独立求值、洋葱皮及数据编辑工具仍按[剩余清单](abi-remaining-work-2026-10-17.md)推进。普通生产编辑服务不因本批开放。

Win7 SP1 x64、CPU 基础路径、AMD 兼容及程序根目录 `data` 约束保持。实际 Win7、AMD 和不同 DPI 多屏环境仍待验收。已有曲线堆故障、未解释超时及菜单选择间歇超时没有因此关闭。
