# 曲线原始兼容配置的堆故障现场（2026-10-21）

原始兼容配置的曲线全集仍有 `0xC0000374`，尚未确定损坏来源。本次在已冻结的插帧候选上捕获了堆链表不一致，但没有获得首次错误写入记录。该结果不能作为修复验收。

## 候选与运行条件

本次只启动自有测试子进程，没有附加现有 MMS 进程，也没有修改系统堆设置、注册表或缓解策略。运行继承 Default 桌面，不发送全局输入。启动请求隐藏窗口，夹具仍可能显示测试窗口。

使用的 EXE、PDB 和源码来自[插帧候选清单](../../build/verification/abi-curve-subdivision-20261021/subdivision-candidate-02/manifest.json)，运行前核验全部 17 项。EXE 和 PDB 的 GUID 均为 `1c6c2f85-c62c-4069-819f-ba7b48600a11`，age 为 `19`。本次没有重新构建候选。

本次调试运行耗时 78.121 秒，包含启动器和转储生成时间。子进程以 `0xC0000374` 退出，没有超时。两份异常转储各为 86,391,544 字节。命令、桌面、工具哈希、候选身份和退出信息见[运行报告](../../build/verification/abi-curve-subdivision-20261021/original-candidate-debug-01-scratch/report.json)。

## 已确认的现场

本次最后输出为 `comparison lifetime kind=1 mouse=0 phase=0 action=0 response=1`，与此前非调试全集末条不同。匹配 PDB 的调用栈指向冻结测试第 4202 行的初始 `Open`，随后进入 `WM_CREATE`、`CreateControls` 和 `MakeControl`。检测发生在创建 `CompareAllGroups` 原生 BUTTON 的系统分配路径中。

该用例初始 `state.armed` 为 false，尚未武装比较写回调。因此，这份转储不能归因于该用例尚未进入的回调，也不能把按钮创建当作损坏来源。[完整调用栈](../../build/verification/abi-curve-subdivision-20261021/original-candidate-debug-01/fault-0001-C0000374-first/stack.txt)中的源码行号必须对应冻结副本，而非后续工作区源码。

离线读取转储得到以下关系。地址仅适用于本次进程，不能在下一次运行复用。

| 字段 | 本次值 |
| --- | --- |
| 失败记录中的 heap | `0x187544F0000`，与 PEB.ProcessHeap 相同。 |
| 待检查节点 N | `0x187545386A0`。 |
| N.Flink | `0x1875456DF50`。 |
| N.Blink | `0x1875450E680`。 |
| N.Blink 的 Flink | 等于 N。 |
| N.Flink 的 Blink | `0x18754538680`，等于 N 减 `0x20`，不等于 N。 |

第一次和第二次异常保存的节点字节一致。当前系统 `ntdll.dll` 的对应指令字节与转储一致，双向链接检查的失败调用位于 RVA `0x5458B`，返回地址 RVA `0x54590` 出现在有效展开栈中。这证明系统检测到了空闲链表反向链接不一致。原始字段、字节核验和分析边界见[离线分析](../../build/verification/abi-curve-subdivision-20261021/original-candidate-debug-01-analysis.json)。

## 尚缺的证据

转储没有记录该字段的最后写入者，也没有该块先前的分配与释放调用栈。当前证据不能区分越界写、失效指针写入、错误释放或更早的分配器状态破坏。填充值和已加载模块列表不能代替这些证据。

后续观察需要在同一自有测试子进程中保留原用例顺序，定位最早出现错误链接的边界。如果观察字段写入，必须动态识别本次节点并跟踪其分配生命周期；分配器正常维护也会写入该字段。仅捕获下一次 `CreateWindowExW` 的检测栈不足以找到来源。

此项继续保持未解决。普通采样保护、独立专项通过或完整配置通过均不能关闭该故障；Win7、AMD 和真实交互验收也不由本次转储替代。main 和正式 release 未修改。
