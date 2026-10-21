# 通道滚动条的取消与计时器身份

日期：2026-10-21。分支：`codex/plugin-abi`。本记录区分纯取消问题、首轮修正引入的手势交错，以及旧计时消息跨手势执行的问题。它们不等于此前曲线全集堆损坏的根因。

实现范围为 [curve_editor_channel_scroll.hpp](../../src/window/curve_editor_channel_scroll.hpp)，独立夹具为 [curve_native_control_lifetime_tests.cpp](../../tests/curve_native_control_lifetime_tests.cpp)。沿用原树、滚动条和 State，不增加公共 ABI。证据目录为 `build/verification/abi-curve-transport-write-20261021`。

## 已复现的问题

箭头或翻页立即调用原生树滚动。树的同步回调可以把捕获转给第三窗口，此时取消链已清空 repeat，但旧按下分支返回后仍设置 400 ms 计时器。取消后已排队的 `WM_TIMER` 也会重新启动 60 ms 计时器。原实现的专项共 470 项中 4 项失败，实际观察到取消后的计时器调用和后续消息；见[原始红例](../../build/verification/abi-curve-transport-write-20261021/repeat-red-full-01/report.json)。

首轮修正增加捕获及 repeat 检查，并将 60 ms 设置放在滚动返回后。纯取消专项和控件全集通过，但复审指出同 State 的新手势可以在旧原生回调中开始：新手势先设置 400 ms，旧回调随后把它覆盖为 60 ms。保存的旧 `WM_TIMER` 在新手势开始后执行，也会把新计时器改为 60 ms，箭头场景还多滚动一次。

新增反例真实取消捕获，再同步向原滚动条发送新箭头或翻页按下。四个交错组合全部检测到旧尾段再次设置计时器；两种保存旧消息的组合也均失败。未取消的正常重复路径同时通过，真实设置了 60 ms；见[交错红例与正常对照](../../build/verification/abi-curve-transport-write-20261021/repeat-interleave-red-07/report.json)。这些结果不能用首轮通过覆盖。

## 捕获观察的边界

首轮原始兼容专项另有一次失败。回调内和外层立即返回时，捕获已实际转到第三窗口；250 ms 观察结束时变为零，而计时器调用、后续计时消息和额外滚动均为零。该次日志没有足够信息说明哪条原生通知释放了第三窗口的捕获，不能归因为系统或用户行为。

后续测试记录自有 root、host、bar、third 的捕获、取消、焦点及激活消息，并保存回调内、原调用返回后和泵消息后的捕获。立即转移仍用强断言验证。等待结束只要求旧 bar 不重新取得捕获、repeat 保持取消、没有计时或滚动；产品不负责让第三窗口持续保持捕获。追加观察后两套配置的纯取消专项各 605 项、控件全集各 1,359 项通过。这仅是交错修正前的结果，见[原始失败](../../build/verification/abi-curve-transport-write-20261021/repeat-green-original-02/report.json)、[完整配置观察](../../build/verification/abi-curve-transport-write-20261021/repeat-final-full-04/report.json)及[原始兼容观察](../../build/verification/abi-curve-transport-write-20261021/repeat-final-original-04/report.json)。

## 每次手势的独立身份

每次新按下取得当前 UI 线程单调递增的非零 ticket，同时作为该手势的原生计时器请求 ID。State 仅保留当前手势和计时器 ID；计数器耗尽时拒绝新手势，不回绕复用。thumb 拖动保留手势身份，但不启动 repeat。

取消先清除手势、拖动和 repeat 字段，再停止保存的旧计时器 ID。原生测量、焦点、捕获、树滚动返回后，旧分支必须仍匹配原 State 和 ticket 才能继续。同一 State 中的新手势也会使旧分支停止；旧分支不能停止或重新设置新手势的计时器。

`WM_TIMER` 只接受当前非零请求 ID，旧消息直接结束，不滚动、不设置计时器，也不取消当前 ID。首轮 400 ms 和后续 60 ms 使用本手势同一个请求 ID；`SetTimer` 的返回值仅用于判断成功。新按下不主动释放同 bar 已有捕获，避免增加原实现没有的捕获变化通知。身份耗尽时仅释放仍属于本 bar 的捕获并结束。

最终验收又发现重复 `SetCapture(bar)` 本身会发送 `WM_CAPTURECHANGED`，其目标和当前捕获都仍是该 bar，却会取消刚分配的新 ticket。两个新按下反例因此失败，原生日志明确记录了该通知。最终修正只在当前捕获不属于 bar 时调用 `SetCapture`，随后仍检查 ticket 与捕获归属；测试断言未改。见[重复捕获失败报告](../../build/verification/abi-curve-transport-write-20261021/repeat-tickets-final-full-08/report.json)。

## 最终控件结果

| 配置 | 专项 | 控件全集 | 证据 |
| --- | --- | --- | --- |
| 完整配置 | 1,705 项，0 失败 | 2,459 项，0 失败 | [最终完整配置](../../build/verification/abi-curve-transport-write-20261021/repeat-final-full-09/report.json) |
| 原始兼容配置 | 1,705 项，0 失败 | 2,459 项，0 失败 | [最终原始兼容配置](../../build/verification/abi-curve-transport-write-20261021/repeat-final-original-09/report.json) |

专项覆盖纯取消、已排队消息、原生 Step 内取消、同 State 新手势、同 bar 已有捕获的再次按下、旧消息晚到，以及正常 400 ms 转 60 ms。指针移出有效箭头时保持同 ID 的 60 ms 检查且不滚动，返回后继续。身份末值及永久耗尽使用局部计数器检查，不重置产品线程的真实单调序列。

控件原有寿命、同 HWND 更换 State、键盘、tooltip、滚动和无 C++ 分配断言保留。源码、两套二进制及匹配 PDB 保留在[最终冻结清单](../../build/verification/abi-curve-transport-write-20261021/writer-controls-final-09/manifest.json)。最终独立复审未发现本批新的明确阻塞；此结论限本记录范围。

## 验证方式

测试只操作自有隐藏窗口。透明转发记录自有 bar 全部 ID 的真实 `SetTimer`、`KillTimer` 参数和返回值，继续调用原 Win32 API，不改变 400 ms 或 60 ms 延迟。旧消息使用真实请求 ID，先投递到该 bar 并取出保存，建立新手势后才实际分发；不再固定为 ID 1。

需要实际命中鼠标所在箭头的计时测试，只调整自有隐藏子窗口的位置及尺寸，不移动桌面指针，不注入全局输入。纯取消后的观察有界为 250 ms；误启动首轮 400 ms 的检查依据真实 `SetTimer` 调用记录，不依赖等到它超时。

Win7 SP1 x64、CPU 和 AMD 要求保持；合成窗口结果不替代真机输入或 GPU 验收，main 与正式 release 未更新。此前堆损坏和其他未迁移曲线回调继续独立跟踪。
