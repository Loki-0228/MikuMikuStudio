# 共享曲线选择回调的会话保护

日期：2026-10-21。分支：`codex/plugin-abi`。

本批将 `Source.readKeySelection` 和 `Source.writeKeySelection` 的实际调用统一到 `EditorLease`、共享 `Session` 版本及 `InvokeProvider`。闭包复制、执行或最终释放期间关闭窗口、同窗口替换会话、关闭后重建窗口，旧调用均停止处理旧选择。持有旧身份 token 的强引用不延长旧会话的有效期。

实现位于 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)，回归位于 [animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp)。本批不增加公开 C ABI，沿用此前的[来源读取保护](abi-curve-source-lifetime-2026-10-21.md)和[写入及历史保护](abi-curve-write-lifetime-2026-10-21.md)。

## 选择接口的原有协议

`readKeySelection` 接收通道 ID，返回 `vector<int>` 帧号。空向量表示合法的空选择。`writeKeySelection` 接收通道 ID 和帧号向量，返回 `void`。两者均没有 `bool` 成功或拒绝协议。

内部 `PullKeySelection` 的布尔返回值表示选择是否发生变化。返回 `false` 不表示 provider 拒绝。选择不变时，数值编辑可以继续。选择变化时，编辑器先接收新选择，再拒绝针对旧选择填写的数值。空选择会清除当前选择，不会继续删除先前选中的键。

读取闭包复制失败或读取回调抛异常时，当前界面调用结束，不把不完整的结果写回选择，也不继续提交旧编辑。写入回调异常沿现有异常边界处理，不增加布尔拒绝状态。已经完成的宿主提交仍按写入保护中的接受规则保留。

本批保留原有写入后由后续读取确认选择的顺序，没有增加每次 `writeKeySelection` 后立即调用 `readKeySelection` 的协议。通知或定时器的读回只接收选择，不递归发布选择，也不创建工程历史。

## 私有参数与返回后校验

`PublishKeySelection` 在调用前复制通道 ID，并把当前选中键转换为私有帧号向量。provider 不持有实时 `Channel::id` 或选择容器的引用。关闭窗口或替换来源后，正在执行的回调及闭包副本析构仍可读取这批参数。

`PullKeySelection` 同样私有持有通道 ID，并记录活动通道索引及键数。`InvokeProvider` 固定原会话存储，复制闭包后先校验身份，再执行回调。闭包副本与会话存储引用释放后再次校验，失效结果不进入新会话。

读取返回后，`PullKeySelection` 重新获取当前通道，核对活动索引、通道 ID 和键数，再根据当前键的帧号映射选择。函数不跨 provider 调用借用通道或键向量。选择集合准备完成后通过 `swap` 接收，准备失败不留下部分选择。

## 同来源递归与历史发布

两个选择回调的复制、调用和释放全过程均处于 `externalCallbackActive` 保护中。普通命令、画布鼠标及键盘、MMD 编辑、通道树输入和来源刷新不能在回调中递归修改该来源。`NotifySourceChanged` 保留待处理通知，`ProcessSourceChanges` 延后读取。

`ownedOperation` 仅允许当前提交内部的选择发布或规范值读取跨过 `externalOperationActive`。该参数不能绕过 `externalCallbackActive`。外部共同历史的规范值读回仍可接收选择，选择 provider 不能借此再次进入选择或数据写入。

本地 `UndoOnce` 和 `RedoOnce` 的实际入口均由 `WM_COMMAND` 分派，键盘快捷键也转到该入口。撤销和重做内部不再发布一次选择，处理结束后统一由命令尾部发布。回归要求一次成功撤销或重做只调用一次选择 writer。

## 稳定定时刷新不复制整条轨道

实现准备期间曾为读取后的校验复制全部键的帧号。该做法会使每次稳定定时刷新都分配一条与轨道长度成正比的数组，已在最终版本中移除。

当前方案依靠完整的回调忙碌保护阻止实时轨道修改，依靠会话身份校验拒绝 `Open` 替换，再重新获取并核对通道。选择不变时直接比较选中键的帧号，无需创建全轨帧号数组或选择集合。

最终专项比较 2 键与 100,000 键两个夹具。两者预热后各执行 8 次稳定的空选择定时刷新。完整配置与原始兼容配置的日志均记录：

| 键数 | 定时刷新次数 | C++ 分配总字节 `bytes` | 最大分配 `largest` |
| --- | --- | --- | --- |
| 2 | 8 | 0 | 0 |
| 100,000 | 8 | 0 | 0 |

夹具同时检查所有键的帧号和值保持不变，`Source.read`、`apply` 和选择 writer 没有执行。计数来自夹具对 C++ `operator new` 的观测，仅证明该稳定空选择路径没有随轨道长度增长的分配，不代表全部选择接口无分配，也不是速度提升基准。provider 自身返回非空向量或选择发生变化时仍可分配。

## 首次失败与双击夹具修正

[selection-full-01/report.json](../../build/verification/abi-menu-selection-20261021/selection-full-01/report.json)保留首次失败。选择专项及全集以退出码 `1` 结束，失败断言是 `selection copy, invocation and final release retire exactly one original callback`。最后用例是写入入口 `entry=5` 的闭包复制场景。

该入口双击了仅两键轨道的最后一键。`FitAll` 把时间范围终点设为最后一键的帧号，`KeyPoint.x` 因此等于 `plot.right`。原有 `InsertCurveKeyAtPoint` 用 `PtInRect` 检查图内命中，矩形右边界不包含在内，所以该双击不会进入选择发布。普通点击使用不同命中路径，预选成功不能证明双击已命中。

修正只改测试夹具，没有将该失败归为生产缺陷。双击用例增加后续第三键，使目标键位于图内。夹具先读取真实绘图区并检查 `PtInRect`，再以未武装的真实 `WM_LBUTTONDBLCLK` 验证一次选择发布、零 `apply`、键数保持为 3。随后才武装原有关闭及替换矩阵。全部生命周期断言继续保留，没有直接调用或伪造 provider。

## 验证

主任务统一构建并运行。本记录只核对源码、日志及已结束的报告，没有另行构建或运行测试。

| 范围 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| `--selection-lifetime` | [selection-full-02](../../build/verification/abi-menu-selection-20261021/selection-full-02/report.json)：5,158 项通过。 | [selection-original-01](../../build/verification/abi-menu-selection-20261021/selection-original-01/report.json)：5,158 项通过。 |
| 编辑器全集 | [selection-full-03](../../build/verification/abi-menu-selection-20261021/selection-full-03/report.json)：13,380 项通过。 | [selection-original-01](../../build/verification/abi-menu-selection-20261021/selection-original-01/report.json)：13,380 项通过。 |

上述最终条目的 `exitCode` 均为 `0`、`error` 均为 `null`。新增专项覆盖 405 个真实入口及退休组合，包含初次创建、通知刷新、定时器、父窗口与子控件快捷键、数值 EDIT、通道树、画布点击及双击、内部本地及外部提交。闭包复制、调用和释放分别覆盖关闭、同窗口替换、关闭后重建、普通结果、空读取及异常。

其他断言覆盖私有参数在退休期间可读、强持旧 token 仍失效、新会话可编辑、已完成编辑不重复、复制失败及异常恢复、延后的读回、空选择清除、数值编辑的选择变化拒绝、同来源递归拦截和撤销重做只发布一次。既有写入、来源及 motion 断言保留在全集中。

两套曲线源码构建及最终测试构建均成功，主任务记录退出码均为 `0`。日志分别为 [build-selection-full-01.log](../../build/verification/abi-menu-selection-20261021/build-selection-full-01.log)、[build-selection-original-01.log](../../build/verification/abi-menu-selection-20261021/build-selection-original-01.log)、[build-selection-tests-full-04.log](../../build/verification/abi-menu-selection-20261021/build-selection-tests-full-04.log)和 [build-selection-tests-original-04.log](../../build/verification/abi-menu-selection-20261021/build-selection-tests-original-04.log)。

本轮产品构建 [build-menu-full-05.log](../../build/verification/abi-menu-selection-20261021/build-menu-full-05.log)及 [build-menu-original-05.log](../../build/verification/abi-menu-selection-20261021/build-menu-original-05.log)也以退出码 `0` 结束。菜单状态已在[后续菜单记录](abi-menu-state-2026-10-21.md)中独立验收，不计入本记录的曲线完成范围。

运行继承桌面 `Default`，`realPrivateDesktop=false`。`SW_HIDE` 只是进程启动请求，窗口显示由夹具控制，不能称为独立桌面隔离或整轮隐藏窗口验收。本批没有完成真实 IME、多 DPI 或目标硬件验收，详见[本机窗口回归更正](abi-verification-desktops-2026-10-21.md)。

## 剩余范围

`readComparisonMode`、`writeComparisonMode` 和 `linkAdjacentControls` 已在[后续批次](abi-curve-comparison-lifetime-2026-10-21.md)接入同一保护。`Session.currentFrame` 和 `Session.isPlaying` 随后完成[只读查询保护](abi-curve-transport-query-lifetime-2026-10-21.md)，两套配置查询专项各 8,566 项、完整配置编辑器全集 24,620 项通过。

原始兼容配置全集首次以 `0xC0000374` 退出，堆损坏根因尚未解释。单独比较专项 2,674 项、ASan 全集及逐组堆校验全集各 24,620 项随后通过，均不代表该问题已修复。后续诊断结果见查询记录。

`Session.seek` 和 `Session.setPlaying` 写回调、`Source.subdivide` 及一般 `Channel.sample` 调用仍未统一到同一复制、调用及最终释放检查，不能据此认为这些回调也支持任意重入或自毁。实际宿主停止与跳帧路径的重复场景求值也仍待处理。

本批不改变曲线或 VMD 格式兼容规则，不开放普通生产宿主的 `mms.edit`。Win7 SP1 x64、CPU 基础路径、AMD 兼容及程序旁 `data` 的要求保持不变。正式 release 未更新。
