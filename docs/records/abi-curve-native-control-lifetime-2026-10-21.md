# 曲线原生控件回调寿命与捕获验收（2026-10-21）

分段控件和通道滚动条已修正原生调用同步销毁窗口后的状态访问。独立专项在完整配置和原始兼容配置各通过 756 项检查，独立代码复审未发现本批新的明确生命周期阻塞。完整配置曲线全集通过 26,667 项，原始兼容配置全集仍发生 `0xC0000374`；整体堆故障未关闭。

## 已复现的问题

`SetFocus` 可以在返回前调用旧焦点窗口的 `WM_KILLFOCUS`。如果该回调关闭当前控件的宿主，控件会先收到 `WM_NCDESTROY`，而外层鼠标处理仍未返回。原 `curve_editor_segments::Proc` 随后写 `state.focus`，原滚动条处理随后写 `dragging` 和 `grab`；两处 State 已经释放。

独立测试只隔离被监视的一个 State 的底层 `malloc` 存储。析构照常执行，释放时填入固定字节，外层 `SendMessage` 返回后才检查原始字节并最终释放。它不读取已析构对象。修复前真实焦点与销毁均发生，分段控件和滚动条分别有 4、5 字节在释放后改变；正常选择及真实树滚动均通过。

捕获另有一条保持窗口和 State 存活的路径。前一个 capturer 在 `WM_CAPTURECHANGED` 中将捕获转给第三个窗口，嵌套取消已清理滚动条状态，原外层点击却又启动拖动。追加反例实际取得第三窗口的捕获，同时观察到 `dragging == true`，随后局部鼠标移动仍滚树。

## 状态保活与精确身份

[curve_editor_control_lifetime.hpp](../../src/window/curve_editor_control_lifetime.hpp) 提供内部无分配 `Pin`。原生回调进入时增加活动计数，退役时标记 `retired` 并移除后续入口，最后一个活动回调离开才释放 State。它没有增加公共 ABI 或额外插件对象体系。

原生调用返回后，两个控件检查原来的 subclass procedure、ID 和 refData，不能仅凭 `IsWindow` 接续操作。这样，同一 HWND 换成新 State 时，旧回调也会停止。绘制使用局部 RAII 恢复绘制状态，提前退出仍配对 `BeginPaint` 与 `EndPaint`。

[curve_editor_segments.hpp](../../src/window/curve_editor_segments.hpp) 保护焦点、组合框读取、默认窗口过程和 tooltip 消息。tooltip 也保活同一个 State；退役先断开入口，再拆资源。重复 `Apply` 保留现有状态，避免覆盖并泄漏。

[curve_editor_channel_scroll.hpp](../../src/window/curve_editor_channel_scroll.hpp) 让树与滚动条共用 State，并核对两端及父窗口。树退役时清理其原滚动条；滚动条退役时解除原树子类。默认树过程返回后，不再对替换后的滚动条执行旧 `Layout`。原生遍历期间的内容变化会中止旧滚动。`SetCapture` 返回后还要求捕获仍属于该滚动条，才启动拖动。

## 验收证据

[curve_native_control_lifetime_tests.cpp](../../tests/curve_native_control_lifetime_tests.cpp) 是独立原生夹具，直接包含产品 helper。仅创建自有窗口、发送局部消息、切换当前线程的焦点和捕获；不发送全局输入，不读取桌面图像。

证据目录为 `build/verification/abi-curve-construction-20261021`。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 首次独立夹具初始化 | 未提供 v6 activation context 却请求 `ICC_STANDARD_CLASSES`，初始化失败。修正夹具为实际需要的树与 bar 类后继续；产品初始化未改。 | [首次报告](../../build/verification/abi-curve-construction-20261021/control-lifetime-red-01/report.json) |
| 原焦点销毁反例 | 251 项中两条释放后写入断言失败，分别改变 4、5 字节。 | [红例日志](../../build/verification/abi-curve-construction-20261021/control-lifetime-red-02/0-all/result.log) |
| 首轮修正与附加边界 | 两套配置各 637 项通过，含捕获转移时销毁、默认过程返回后销毁、同 HWND 更换 State、正常 tooltip、键盘、滚动及零 C++ 分配检查。 | [完整配置](../../build/verification/abi-curve-construction-20261021/control-lifetime-full-green-03/report.json)、[原始兼容配置](../../build/verification/abi-curve-construction-20261021/control-lifetime-original-green-03/report.json) |
| 保持 State 存活的捕获重定向 | 原 637 项仍通过，新增取消状态及不滚树两条断言失败，总计 756 项中 2 项失败。 | [捕获红例日志](../../build/verification/abi-curve-construction-20261021/control-capture-red-04/0-all/result.log) |
| 最终控件专项 | 两套配置各 756 项通过，所有释放后写入检查为 0，捕获重定向正确取消，新手势仍可正常拖动。 | [完整配置最终报告](../../build/verification/abi-curve-construction-20261021/control-lifetime-full-final-05/report.json)、[原始兼容最终报告](../../build/verification/abi-curve-construction-20261021/control-lifetime-original-final-05/report.json) |
| 完整配置曲线全集 | 26,667 项通过，退出 `0`。 | [完整配置全集报告](../../build/verification/abi-curve-construction-20261021/control-curve-full-final-05/report.json) |
| 原始兼容配置曲线全集 | 退出 `0xC0000374`，最后上下文为比较回调 `kind=1 phase=1 action=2 response=2`，未完成全集。不能用控件专项通过替代此失败。 | [原始兼容全集报告](../../build/verification/abi-curve-construction-20261021/control-curve-original-final-05/report.json) |

上述二进制、匹配 PDB、编辑器与本批 helper 源码已冻结在 `after-04-controls`，哈希及来源见[冻结清单](../../build/verification/abi-curve-construction-20261021/after-04-controls/manifest.json)。本次普通运行没有生成新的调试转储，既有转储仍只对应各自冻结版本。

## 尚未覆盖的边界

本批只处理 helper State 与原生子类实例的退役。同 owner `Open` 复用同 HWND、同控件 State，却更换 Editor Session 时的输入连续性仍需审计。数字拖动 `curve_editor_scrub.hpp` 及主题辅助过程不属于本批迁移，不能扩展为整个框架已经支持任意重入。

复审另指出箭头或翻页 `Step` 的原生回调取消捕获后，旧分支可能重新启动空转 repeat timer。它是既有行为，本批尚未运行该反例或修正，后续需独立验证；不能将它记为本批新增堆故障或已完成项。

该计时器缺项随后已按[独立手势身份记录](abi-curve-repeat-timer-2026-10-21.md)完成反例、修正及两套配置验收，包含旧消息晚到、同 State 新手势和重复捕获。上述 756 项与堆故障结论仍保留当时范围，最新控件结果以该记录为准。

`seek`、`setPlaying` 写回调、`subdivide`、一般 `Channel.sample` 和真实播放停止的重复求值仍待处理。此前堆故障的冻结二进制、转储和反向对照继续保留，详见[原生构造验收与诊断记录](abi-curve-native-construction-2026-10-21.md)。

`seek`、`setPlaying` 随后已按[写回调记录](abi-curve-transport-write-lifetime-2026-10-21.md)接入保护。细分、一般采样、数字拖动、主题辅助和真实停止求值仍未因此完成，既有故障也未据此关闭。

本批没有新系统或显卡依赖。Win7 SP1 x64、CPU 基础路径与 AMD 要求保持；当前 Windows 的合成窗口专项不能替代对应真机、IME、多 DPI、完整鼠标或 GPU 验收。正式 release 和 main 未更新。
