# 曲线跳帧与播放写回调的会话保护

日期：2026-10-21。分支：`codex/plugin-abi`。

曲线编辑器的 `seek` 和 `setPlaying` 已改为调用期间保留闭包副本，并在复制、执行和最终释放期间阻止嵌套编辑。回调关闭窗口、更换来源或在同一 HWND 安装新会话后，旧操作停止，不修改新会话的字段、状态、选择、历史或拖动。

实现见 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)，专项见 [animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp) 的 `--transport-write-lifetime`。本批沿用原 Session、窗口身份和 `InvokeProvider`，不增加公共 ABI，不改变曲线、PMM 或 VMD 格式。

## 调用与异常规则

帧号及播放状态仍先执行原有查询；查询失败不能授权导航。准备调用写回调后，`InvokeTransportWriter` 同时设置操作与回调保护，覆盖闭包复制、调用及局部副本销毁。保护对象释放其会话引用后，外层再释放原会话存储并检查身份，避免最后一个闭包的析构重入逃过返回检查。

`void` 写回调可能先改变宿主，再抛出异常。编辑器停止本次尾部更新，不自动重试，也不假设可以撤销宿主已发生的副作用。后续明确的新操作可以正常执行。成功写回后的显示查询失败同样不会重新调用写回调。

停止状态下的同帧跳转保持空操作。播放中的同帧跳转仍调用宿主 `seek`，由宿主负责停止；编辑器没有增加一次 `setPlaying(false)`。缺少可选写回调时保持原有空操作。真实宿主停止播放时的重复求值仍需单独处理，本批的合成宿主检查不能代替该项。

## 鼠标与同窗口重开

点击关键帧时，跨跳帧回调只保留通道 ID 和帧号。返回后核对原会话、重新取得通道与选择状态，再准备拖动；旧轨道和选择的借用引用不跨越回调。鼠标释放捕获及完成标尺跳帧后也检查原会话。

同 owner 的 `Open` 先私有准备新 Session、版本及来源状态。准备成功后，仍在旧会话下取消旧手势、释放旧捕获，并在原生捕获变化通知返回后检查身份，最后才发布新会话。这样，旧标尺的移动或松开消息不能驱动新会话。准备分配失败保留原手势；取消私有控制柄预览用已保存关键点数组交换恢复，不分配、不发布编辑或历史。

## 已取得的证据

证据目录为 `build/verification/abi-curve-transport-write-20261021`。所有窗口测试串行继承 `Default` 桌面，启动请求隐藏，后续显示由夹具控制。测试仅操作自有窗口和当前线程消息，不发送全局输入。

| 检查 | 实际结果与证据 |
| --- | --- |
| 原直接调用的复制反例 | 两个写回调在正常及预设复制失败时均跳过复制，观察到 `copies=0`、`bodies=1`、`live_bodies=1`。收集四个组合后，首次汇总断言失败并退出，下一条断言未执行；此阶段没有销毁或替换窗口。[原实现报告](../../build/verification/abi-curve-transport-write-20261021/copy-red-full-01/report.json)。 |
| 生命周期与异常专项 | 两套配置各 2,628 项通过，退出 `0`。[完整配置](../../build/verification/abi-curve-transport-write-20261021/writer-green-full-03/report.json)、[原始兼容配置](../../build/verification/abi-curve-transport-write-20261021/writer-green-original-03/report.json)。 |
| 相邻回归首轮 | 完整配置查询 8,986 项、构造 1,627 项、时间轴输入 24 项、原生同步 84 项、编辑输入 402 项均通过。原始兼容配置后四组通过，但查询专项在 `entry=1 phase=1 action=1 response=0` 的替换会话编辑断言失败，未完成该项，不能写为相邻回归全部通过。[完整报告](../../build/verification/abi-curve-transport-write-20261021/writer-neighbors-full-05/report.json)、[原始兼容报告](../../build/verification/abi-curve-transport-write-20261021/writer-neighbors-original-05/report.json)。 |
| 首轮恢复诊断 | 测试追加点击前、按下、松开、删除四阶段的查询，保留原点击消息和最终断言。原始兼容查询专项 8,986 项通过，生产未改。[诊断运行](../../build/verification/abi-curve-transport-write-20261021/recovery-original-06/report.json)。复审指出观测中的几何私有消息会更新派生几何，文本快照还增加堆分配；该次通过只能说明诊断版未重现，不能判定前一失败已修复。 |
| 身份修正中间候选全集 | 收窄诊断后，两套配置曲线全集各 29,295 项通过，退出 `0`。此时滚动条专项仍发现重复捕获问题，后续单独修正，不能写为整体全部通过。[完整配置](../../build/verification/abi-curve-transport-write-20261021/curve-final-full-08/report.json)、[原始兼容配置](../../build/verification/abi-curve-transport-write-20261021/curve-final-original-08/report.json)。 |
| 最终曲线全集 | 修正重复捕获后，两套配置各 29,295 项通过，退出 `0`。[最终完整配置](../../build/verification/abi-curve-transport-write-20261021/curve-final-full-09/report.json)、[最终原始兼容配置](../../build/verification/abi-curve-transport-write-20261021/curve-final-original-09/report.json)。 |
| 两套产品构建与导入核对 | 两套产品、曲线测试和控件测试目标均构建成功，见 `build-final-full-09.log` 与 `build-final-original-09.log`。PE 均为 x64、子系统 6.1，普通和延迟导入表与前批一致；正式 release 哈希保持。[PE 清单](../../build/verification/abi-curve-transport-write-20261021/pe-products-final-09.json)。 |

专项包含六个真实入口，即明确跳帧、播放、关键帧点击、标尺按下、捕获中的移动和松开。每个入口覆盖复制、执行、最终释放三个阶段，关闭、同 HWND 换会话、关闭后新建三种方式，以及正常、普通异常、分配异常三种响应，共 162 个组合。还检查旧会话引用仍被外部保留时不能恢复身份、写回至多一次、替换会话随后仍可正常编辑。

附加检查包含复制失败、执行前后异常、成功后的显示异常、各阶段嵌套命令和刷新、同帧语义、缺少回调及正常关键帧拖动。真实 `WM_CAPTURECHANGED` 中关闭或替换会话会使外层 `Open` 中止；准备分配失败则保留原窗口及手势。

上述相邻查询失败的二进制、匹配 PDB、编辑器与测试源码及 helper 已保留在 `writer-before-recovery-01`，见[冻结清单](../../build/verification/abi-curve-transport-write-20261021/writer-before-recovery-01/manifest.json)。该次运行没有转储，不能将其直接归为此前堆损坏或环境干扰。

最终诊断在成功路径只保存固定容量标量，包括按键、焦点、捕获、启用状态、宿主计数及会话身份。几何、历史、比较和文本查询全部移到已经确定失败的分支，保留原坐标查询、连续按下／松开／删除消息和最终断言。两套曲线与控件二进制、PDB 及对应源码保留在 [writer-controls-final-09](../../build/verification/abi-curve-transport-write-20261021/writer-controls-final-09/manifest.json)。

## 仍未关闭的项目

原始兼容配置此前完整曲线套件的 `0xC0000374` 堆损坏，以及本批相邻查询的替换会话编辑失败仍未归因。本批最终全集通过不能关闭间歇故障，也不覆盖一般 `Channel.sample`、`subdivide`、数字拖动或全部主题辅助过程。既有失败、匹配符号及冻结源码见[原生构造诊断](abi-curve-native-construction-2026-10-21.md)与[原生控件寿命记录](abi-curve-native-control-lifetime-2026-10-21.md)。

本批没有新增系统或显卡依赖。Win7 SP1 x64、CPU 基础路径与 AMD 要求保持；当前系统合成窗口结果不能代替对应真机、IME、多 DPI、GPU 或完整鼠标验收。main 和正式 release 未更新。
