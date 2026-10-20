# 曲线对比与相邻控制柄回调的会话保护

日期：2026-10-21。分支：`codex/plugin-abi`。

本批将 `Source.readComparisonMode`、`Source.writeComparisonMode` 和 `Source.linkAdjacentControls` 的实际调用接入既有 `EditorLease`、共享 `Session` 版本、`InvokeProvider` 和回调忙碌保护。同时修复范围筛选先改变显示集合、再请求宿主接受模式的顺序，使拒绝或准备失败不会留下部分界面状态。

实现位于 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)，回归位于 [animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp)。沿用此前的[来源读取保护](abi-curve-source-lifetime-2026-10-21.md)、[写入及历史保护](abi-curve-write-lifetime-2026-10-21.md)和[选择回调保护](abi-curve-selection-lifetime-2026-10-21.md)，不增加公开 C ABI。

## 保留原有回调协议

`readComparisonMode` 返回宿主保存的整数。支持对比的来源接受同组、全部组和自由显示三个模式；正常返回负数、内部关闭值 `0` 或越界整数时，仍回落到同组模式。没有读取回调时采用同一默认值。不支持对比的普通来源保留内部关闭状态。

读取闭包复制失败或回调抛异常不等于返回非法整数，不能据此假装成功读取默认模式。异常进入现有窗口或 `Open` 边界；失败的创建或替换只清理对应尝试，不继续处理退休来源。

`writeComparisonMode(int, std::wstring&)` 返回 `false` 表示宿主拒绝模式变化，保留旧模式及显示集合，并沿原有状态提示报告错误。闭包复制失败、普通异常和分配异常同样不发布这次变化。模式未变化时仍不重复调用 writer。已经接受并存储的变化不因后续界面更新失败而重试 writer。

`linkAdjacentControls()` 的 `false` 是合法的“不联动”选项，不是拒绝编辑。一次 MMD 控制柄修改仍可正常提交；`true` 才把相邻区间的对应控制柄合并到同一次编辑。读取该选项失败则拒绝整次修改，保留数据和撤销、重做分支。普通数值曲线控制柄继续遵循 Free/Aligned，不读取这个旧 MMD 面板选项。

## 复制、执行与释放均核对会话

三个回调都在 `externalCallbackActive` 范围内经过 `InvokeProvider`。`EditorLease` 固定原编辑器及共享 `Session` 存储，保护闭包复制全过程；复制后、调用前核对会话身份，闭包副本和存储引用释放后再次核对。关闭窗口、同窗口 `Open` 替换或关闭后重建，会使旧调用停止；持有旧 token 的强引用不会延长旧会话的有效期。

比较模式读写在调用后重新获取当前状态。联动提交只有在 `InvokeProvider` 确认原会话和来源仍有效后，才继续处理本次状态；退休调用不再访问旧 `SourceState`。`ExternalOperation` 仅在原会话仍有效时恢复忙碌标志，不把旧标志写到替换后的会话。

回调复制、执行和释放期间，普通命令、通道树输入、画布及 MMD 编辑不能递归修改同一来源。来源通知仍可入队，实际读取延后。新增回归在回调内部、外层恢复前比较模式、活动曲线、显示集合、选中键字段及本地历史快照，避免只检查最终结果而漏掉中途修改。

## 筛选结果先私有准备，再一次接受

`RunCompareFilter` 先计算私有显示 mask，保持主曲线可见，并筛选范围内已登记且非恒定的其他曲线。范围说明、计数和成功提示字符串均在调用模式 writer 前准备完成。准备失败或 writer 拒绝时，不改实时通道的显示标志。

writer 接受自由模式后，`SetCompareMode` 先以不分配的赋值循环发布模式和整批 mask，再更新原生界面、勾选状态及提示。发布不是工程数据编辑，不调用 `Source.apply`，不增加曲线历史。接受后分配故障的专项要求模式和 mask 完整保留，后续通知刷新恢复界面且不重复调用 writer；这不表示后续原生 UI 更新本身无分配。

相邻控制柄联动只需比较曲线轨道、原生控制点和几何信息。原先复制整个 `Channel` 的准备步骤改为只复制这些字段，避免隐式执行无关 `Channel.sample` 闭包的复制构造。专项把该闭包的复制设为抛异常，真实 MMD 数值编辑仍只提交一次且复制次数为零。这项修改不等于已统一保护所有 `Channel.sample` 调用。

## 真实入口与首次失败

生命周期矩阵有 108 个组合：4 条入口（模式读取、模式写入、MMD 数值 Enter、MMD 控制柄鼠标拖动），分别覆盖闭包复制、执行、最终释放三个阶段，关闭、同窗口替换、关闭后重建三种退休方式，以及正常、替代结果和异常三类响应。回归同时检查旧调用停止、新会话不受污染且仍可编辑、已经完成的模式写入不重复。

模式写入另覆盖 7 条真实界面入口：模式按钮、通道单击、通道双击分派、集合行空格、全部显示、反选和范围筛选。每条分别注入复制失败、`false` 拒绝、普通异常和分配异常，要求模式、活动曲线、显示集合、原始曲线及本地历史均不变。相邻联动通过真实数值 EDIT 的 Enter 与面板鼠标按下、移动、松开两条路径验收，不直接调用 provider 代替入口。

[comparison-full-01/report.json](../../build/verification/abi-comparison-lifetime-20261021/comparison-full-01/report.json)保留首次退出码 `1`。生命周期矩阵已完成，随后失败于 `mode refusal, exception and copy failure keep mode, active curve, display mask, data and history unchanged`。

原因是该故障夹具使用完整双击手势 `DOWN/UP/DBLCLK/UP`。通道标签的 `DOWN` 和 `DBLCLK` 都会激活，故一次性故障仅拒绝第一次，第二次是合法成功的重试。最终夹具改为精确发送 `DBLCLK/UP`，只隔离本次要验收的双击分派。既有完整手势用例及全部强断言保留，没有放宽失败原子性或生命周期规则。回调内部快照也补为外层恢复前的直接断言。

## 已核对的验证证据

主任务统一构建并运行。本记录依据最终源码、日志和已结束的报告编写，没有另行构建或运行测试。

| 范围 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| `--comparison-lifetime` | [comparison-full-02](../../build/verification/abi-comparison-lifetime-20261021/comparison-full-02/report.json)：2,674 项通过。 | [curve-original-01](../../build/verification/abi-comparison-lifetime-20261021/curve-original-01/report.json)：2,674 项通过。 |
| 曲线编辑器全集 | [curve-full-01](../../build/verification/abi-comparison-lifetime-20261021/curve-full-01/report.json)：16,054 项通过。 | [curve-original-01](../../build/verification/abi-comparison-lifetime-20261021/curve-original-01/report.json)：16,054 项通过。 |

最终四条结果均为 `exitCode=0`、`error=null`。专项还覆盖初次及替换 `Open` 的读取异常、来源重访、回调内替换、整数默认规则、联动 `false` 的合法提交、联动异常保留 redo、接受后分配失败和同来源递归拦截。CTest 入口为 `animation_curve_comparison_lifetime`；既有断言继续包含在全集中。

两配置源码与最终测试构建成功，日志为 [build-source-full-01.log](../../build/verification/abi-comparison-lifetime-20261021/build-source-full-01.log)、[build-source-original-01.log](../../build/verification/abi-comparison-lifetime-20261021/build-source-original-01.log)、[build-tests-full-02.log](../../build/verification/abi-comparison-lifetime-20261021/build-tests-full-02.log)和 [build-tests-original-02.log](../../build/verification/abi-comparison-lifetime-20261021/build-tests-original-02.log)。主任务记录两套产品构建退出码均为 `0`，对应 [build-product-full-01.log](../../build/verification/abi-comparison-lifetime-20261021/build-product-full-01.log)和 [build-product-original-01.log](../../build/verification/abi-comparison-lifetime-20261021/build-product-original-01.log)。

[pe-inventory-products-01.json](../../build/verification/abi-comparison-lifetime-20261021/pe-inventory-products-01.json)保存两套产品的路径、哈希及 PE 信息：完整配置为 9,409,536 字节，原始兼容配置为 3,377,152 字节，均为 x64、子系统版本 `6.1`。这些信息仅确认本轮产物，不替代 Win7 真机兼容验收；正式 release 未更新。

运行继承 `Default` 桌面，`realPrivateDesktop=false`。`SW_HIDE` 只是进程启动请求，窗口显示由夹具控制，不能称为独立桌面隔离。本批没有完成 Win7、AMD、真实 IME、多 DPI 或 GPU 硬件验收，参见[本机窗口回归更正](abi-verification-desktops-2026-10-21.md)。

## 保持的边界与后续范围

本批不改变标准 VMD 控制点 `0..127` 的范围、曲线编码或文件序列化，不改变 PMM/VMD/VPD 兼容指标，不开放普通生产宿主的 `mms.edit`。Win7 SP1 x64、CPU 基础路径、AMD 和程序旁 `data` 的要求保持不变。

`Session.currentFrame`、`seek`、`isPlaying`、`setPlaying`、`Source.subdivide` 及一般 `Channel.sample` 调用尚未统一到相同的复制、执行和最终释放检查。回调保护不能据此扩展为整个编辑器已支持任意重入或自毁。[相机父骨骼 450 方案](abi-camera-parent-bone-plan-2026-10-21.md)仍只是源码审计和实施计划，不计入本批实现。
