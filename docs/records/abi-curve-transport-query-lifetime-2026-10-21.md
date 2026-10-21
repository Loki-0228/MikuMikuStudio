# 曲线帧号与播放状态查询的会话保护

日期：2026-10-21。分支：`codex/plugin-abi`。

本批将 `Session.currentFrame` 和 `Session.isPlaying` 的全部直接调用接入既有 `EditorLease`、共享 `Session` 版本、`InvokeProvider` 及 `ExternalOperation`。查询闭包复制、执行或最终释放期间关闭窗口、同窗口替换会话、关闭后重建，旧调用均停止。查询失败不再作为「未播放」继续跳帧或切换播放。

实现位于 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)，回归位于 [animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp)。本批沿用[来源读取保护](abi-curve-source-lifetime-2026-10-21.md)、[写入及历史保护](abi-curve-write-lifetime-2026-10-21.md)、[选择保护](abi-curve-selection-lifetime-2026-10-21.md)和[比较及联动保护](abi-curve-comparison-lifetime-2026-10-21.md)。不增加公开 ABI，不改变 `Session` 的公开字段或文件格式。

## 查询结果与显示缓存

内部 `TransportQueryResult` 分开表达以下结果。`bool` 查询正常返回 `false` 是有效结果。

| 状态 | 含义 | 显示与导航规则 |
| --- | --- | --- |
| `Missing` | 宿主没有提供该回调。 | 保留可选回调的原有默认行为。缺少 `isPlaying` 时按未播放处理。缺少 `currentFrame` 时不做相对帧导航，但仍允许输入目标帧后显式跳转。 |
| `Busy` | 编辑器正在执行受保护的宿主回调。 | 不递归查询。显示调用可读取当前 `Session` 已有的有效缓存，导航始终拒绝。没有缓存时跳过对应显示。 |
| `Failed` | 闭包复制失败或查询抛异常。 | 不返回旧缓存冒充新查询结果。保留可保留的显示，拒绝依赖本次查询的导航。 |
| `Ready` | 回调正常返回且会话、来源仍有效。 | 帧号还需通过数值检查，播放状态可直接用作本次结果。 |

缓存只服务显示，不能授权跳帧或切换播放。成功的有效帧号及播放状态写入当前编辑器缓存。同窗口 `Open` 更换 `Session` 时清除缓存、旧播放头位置和帧值字段，并重置初始播放按钮文字。新会话第一次查询失败或重入时不会继承旧会话显示。

`currentFrame` 和 `isPlaying` 是 `Session` 级回调。同一会话内切换来源仍可使用该会话的帧缓存，但 MMD 控制点必须根据当前来源重新计算。`Failed` 即使已有缓存也保持失败状态，不转成 `Ready`。导航请求只接受本次有效查询，不读取显示缓存。

## 闭包与界面重入

`InvokeProvider` 固定原编辑器、共享 `Session` 存储及来源身份。闭包复制后、调用前核对身份，调用结束并释放副本后再次核对。复制构造本身关闭或替换窗口时，原闭包存储仍保活，退休闭包的函数体不会继续调用。持有旧 token 的强引用不延长旧会话的有效期。

查询期间的 `externalCallbackActive` 阻止同一来源递归写入。嵌套绘制、定时器和语言更新可使用显示缓存或跳过显示，不再调用 provider。来源变更通知仍可入队，实际来源读取延后。`ExternalOperation` 只恢复原会话的标志，不向替换后的会话写回旧状态。

`ComputeMmdControls`、`UpdateKeyFields` 和关键帧导航不再借用 `Track` 或 `SourceState` 跨越查询。查询确认有效后，消费者重新取得当前状态。创建控件时先设置静态播放按钮文字；本批先保证完整控件树建立后才读取播放状态，[后续构造保护](abi-curve-native-construction-2026-10-21.md)又将全部初始 provider 推迟到外层原生创建成功返回后。

## 帧号范围与选中键显示

`Ready` 只表示回调正常返回，不保证返回的 `double` 可转换为整数。可用帧号必须有限，且处于 `[-2^63, 2^63)`。代码在 `llround` 前检查范围，避免将 `NaN`、无穷大或越界有限值送入整数转换。播放头字段改用 `llround`，避免 Windows 的 32 位 `long` 限制。

未选中键时，非法时钟不覆盖当前帧值字段。有选中键时，字段直接显示该键的帧号和值，不依赖时钟是否合法。显式修改该键仍可正常提交。导航遇到非法帧号则中止，不用选中键或旧缓存替代时钟。原有目标帧范围和跳转限制保持不变。

专项覆盖 `NaN`、正负无穷、`2^63`、小于 `-2^63` 的相邻可表示值、`-2^63` 和小于 `2^63` 的相邻可表示值。断言同时检查无选键字段保留、合法边界转换、选中键独立显示、非法时钟下零跳转及选中键仍能显式编辑。

## 来源切换失败清除 MMD 编辑授权

`mmdEnabled`、`mmdEndKey` 和控制点显示属于编辑器。来源 A 曾建立有效控制点后，如果跟随播放头的查询失败，原先的提前返回会保留 A 的授权和坐标。切换到已保留选中键的来源 B 时，数值 Enter 可以使用 B 的 `ActiveKeyIndex` 配合旧 A 坐标，错误进入 B 的编辑。这一风险依赖 B 已有选择。`MmdMouseDown` 只选择控制柄并捕获鼠标，不会创建关键帧选择。

现在查询失败，或 `Busy` 且没有显示缓存时，`ComputeMmdControls` 清除 `mmdEnabled` 与 `mmdEndKey`，并报告读取失败。旧坐标不再授权当前来源。恢复查询后，面板只接受当前来源重新计算的控制点。

回归为 A、B 设置不同原生控制点，切换时分别注入复制失败和查询异常，要求面板、数值 EDIT 及区间授权均禁用。强行发送字段 Enter 和控制柄消息仍须零 `apply`、历史不变。恢复时先核对 B 自身坐标，再显式选择 B 的键，要求只提交一次 B 的编辑，A 保持不变。空选择不会被误写成可提交条件。

## 绘制资源先于查询保持独立

画布在 `BeginPaint`、缓冲表面 `Resize`、`SaveDC` 和选入编辑器字体之前读取帧号。回调关闭或替换窗口时，没有借用旧编辑器的 DC、位图或字体继续绘制。`DrawPlayhead` 只接收本次帧号快照，不再调用 provider。

`NativePaint` 负责配对 `BeginPaint` 与 `EndPaint`。`ScopedDcState` 保存原始 HDC，`ScopedSelectedObject` 恢复原选入对象，异常或提前退出不会从被替换的 `PaintSurface` 取得另一个 DC 来清理。MMD 绘制沿用相同资源恢复规则。

专项通过真实 `WM_PRINTCLIENT` 和 `WM_PAINT` 检查调用者的字体、位图、裁剪区域及更新区域。首次缓冲表面建立后，重复绘制的 GDI 对象计数保持稳定。帧查询销毁编辑器时，调用者的内存 DC 和选入对象仍保持不变。这些检查只覆盖本批查询造成的退出路径，不代表尚未统一的 `Channel.sample` 回调也能任意替换绘制会话。

## 真实入口与首次夹具失败

退休矩阵包含 14 条真实入口。每条覆盖闭包复制、执行、最终释放三个阶段，关闭、同窗口替换、关闭后重建三种方式，以及正常返回和抛异常两类响应，共 252 个组合。

| 入口 | 查询位置 |
| --- | --- |
| 初次 `Open` | `UpdateKeyFields` 读取帧号。 |
| 初次 `Open` | 完整控件创建后的 `ApplyLanguage` 读取播放状态。 |
| 画布 `WM_PRINTCLIENT` | 绘制前读取帧号。 |
| 画布 `WM_PAINT` | `BeginPaint` 前读取帧号。 |
| 来源通知刷新 | `ReadRegisteredSource` 刷新字段时读取帧号。 |
| 可见窗口定时器 | 主播放头读取帧号。 |
| 下一帧按钮 | 读取当前帧后计算目标。 |
| 下一关键帧按钮 | 读取当前帧后重新取得轨道。 |
| `SetLanguage` | 更新播放按钮文字。 |
| 可见窗口定时器 | 更新播放状态文字。 |
| 播放按钮 | 查询后决定播放切换。 |
| 输入目标帧后跳转 | 查询播放状态。 |
| 输入目标帧后跳转 | 未播放时查询当前帧。 |
| 定时器的后续 MMD 更新 | 主帧查询完成后，再为控制点读取帧号。 |

矩阵要求退休前看到完整控件数量及必要控件，退休后不再跳帧、切换播放、读取旧来源或调用 `apply`。替换会话保持控件、数据及状态文字，随后仍可正常编辑。

[query-full-01/report.json](../../build/verification/abi-transport-parent-20261021/query-full-01/report.json)保留首次退出码 `1`。前四条入口的矩阵已完成，`entry=4` 失败于 `the command button exists`。原因是新夹具调用了只剩测试枚举、产品已无对应控件或命令的 `kIdRefresh`，不是查询生命周期缺陷。

最终测试删除五处该调用，改为 `TransportRefreshSource`。该辅助函数调用公开 `NotifySourceChanged`，再从真实消息队列取出已投递的 `WM_APP+401` 并分派。回调使会话退休后立即停止，不替新会话处理旧刷新工作。忙碌分支同样实际分派，由产品保护延后读取。B 的恢复刷新明确传入来源 ID `second`，坐标提交前显式选择键。全部原调用次数、零写入、历史及来源退休断言保留，并为用例上下文增加即时输出。

## 已结束的验证与待验收范围

下表汇总统一构建、回归和独立诊断的实际结果。诊断通过与故障根因定位分别记录。

| 范围 | 当前记录 |
| --- | --- |
| 完整配置 `--transport-query-lifetime` | [query-full-02/report.json](../../build/verification/abi-transport-parent-20261021/query-full-02/report.json)：8,566 项检查，`exitCode=0`，`error=null`。 |
| 完整配置编辑器全集 | [query-curve-full-01/report.json](../../build/verification/abi-transport-parent-20261021/query-curve-full-01/report.json)：24,620 项检查，退出码 `0`。 |
| 原始兼容配置查询专项 | [query-original-01/report.json](../../build/verification/abi-transport-parent-20261021/query-original-01/report.json)：8,566 项检查，退出码 `0`。 |
| 原始兼容配置全集 | 同一报告的全集退出码为 `3221226356`（`0xC0000374`，堆损坏），未通过。日志在比较模式矩阵中截断，不能据最后一行确定损坏起点。 |
| 原始兼容配置比较专项诊断 | [query-original-comparison-01/report.json](../../build/verification/abi-transport-parent-20261021/query-original-comparison-01/report.json)：2,674 项检查，退出码 `0`。单独通过不能替代全集，也没有解释堆损坏原因。 |
| 原始兼容配置独立 ASan 全集 | [query-original-asan-01/report.json](../../build/verification/abi-transport-parent-20261021/query-original-asan-01/report.json)：24,620 项检查，退出码 `0`，未报告内存错误。诊断构建改变了内存布局，不能据此宣布首次堆损坏已修复。 |
| 原始兼容配置逐组堆校验全集 | [query-original-heap-01/report.json](../../build/verification/abi-transport-parent-20261021/query-original-heap-01/report.json)：24,620 项检查，退出码 `0`，各组 `HeapValidate` 未发现进程堆错误。首次故障未重现，根因仍未定位。 |
| 上一提交的独立原始配置基线 | [query-baseline-original-01/report.json](../../build/verification/abi-transport-parent-20261021/query-baseline-original-01/report.json)：导出 `1df44ae` 的编辑器和测试源码，以相同工具链独立构建；全集退出码 `3221225477`（`0xC0000005`，访问冲突），日志在选择矩阵中截断。构建与源码哈希见[manifest](../../build/verification/abi-transport-parent-20261021/baseline-1df44ae-original-01/manifest.json)。旧基线也失败，仍不足以认定它与本次堆损坏同根。 |
| 当前原始配置的原生调试重现 | [query-debug-current-01-scratch/report.json](../../build/verification/abi-transport-parent-20261021/query-debug-current-01-scratch/report.json)：再次以 `0xC0000374` 退出，未超时，调试器未改变退出码。保存首、二次异常现场及完整内存转储；stdout 最后即时记录为比较 writer 的 `phase=2/action=0/response=1`。 |
| 构造边界修改前的全堆独立诊断 | [before-all-heaps-01/report.json](../../build/verification/abi-curve-construction-20261021/before-all-heaps-01/report.json)：从冻结的修改前源码独立构建，在逐次关闭和各组边界校验 `GetProcessHeaps` 返回的所有堆；初次包含 8 个堆，全集 24,620 项检查，退出码 `0`。本次未捕获损坏，不能抵消已保存的两次堆损坏现场。诊断源码、插入位置及哈希见 [source-manifest.json](../../build/verification/abi-curve-construction-20261021/all-heap-probe-before/source-manifest.json)。 |
| 修正堆栈展开后的第二次调试重现 | [query-debug-current-02-scratch/report.json](../../build/verification/abi-transport-parent-20261021/query-debug-current-02-scratch/report.json)：约 390 秒后同样以 `0xC0000374` 退出，未超时。取得 39 帧合法展开及本地 EXE 的 PDB 行号，见[首机会堆栈](../../build/verification/abi-transport-parent-20261021/query-debug-current-02/fault-0001-C0000374-first/stack.txt)。 |
| 增加原生构造边界后的两套全集 | [完整配置](../../build/verification/abi-curve-construction-20261021/construction-curve-full-01/report.json)、[原始兼容配置](../../build/verification/abi-curve-construction-20261021/construction-curve-original-01/report.json)均为 26,638 项检查、退出 `0`，包含查询保护及新构造专项。首次故障证据保留，单轮通过不证明同根或修复，详见[构造记录](abi-curve-native-construction-2026-10-21.md)。 |
| 两套产品构建 | `build-products-full-01.log`、`build-products-original-01.log` 退出码均为 `0`。产物含相机父骨骼开发中修改，构建成功不代表该功能已验收，正式 release 未更新。 |

全集中的新 `Transport*` 用例排在比较矩阵之后，本次崩溃时尚未执行。共同的窗口创建、字段、语言及绘制路径已使用本批实现，因此也不能仅凭执行顺序排除产品问题。主任务保留首次失败证据，另建 `curve-asan-sdk19041` 诊断构建；该构建不作为 Win7 产品交付或性能证据。随后测试增加可选 `--heap-validate-all`，保持完整执行顺序和全部断言，在各组结束使用 `HeapValidate` 检查进程堆，并即时输出组及比较矩阵边界。内存诊断及全集验收完成前，本批不记为全部回归通过。

原生调试启动器只调试它自己创建的合成测试进程，使用本地符号及本次工作区临时目录，不附加用户进程或更改系统设置。第一版保存了完整转储，但堆栈展开误把第一次返回当前帧当成无进展，只有 `ntdll` 的异常帧可确认。只读原始栈扫描解析到若干 `MakeControl`、`CreateControls` 和 `Open` 地址候选；原始栈字不能当作合法调用链或写坏位置。第二版启动器保留第一版证据，修正遍历后继续取证，尚未以猜测修改产品。最后一行可能只是检测到较早损坏的时机，不能直接认定该条比较回调是写坏来源。

第二版堆栈确认：比较矩阵下一次初始 `Open` 正经 `WM_CREATE → CreateControls → MakeControl → CreateWindowExW` 创建原生控件，系统分配路径报告较早的堆损坏。此时尚未执行该 writer 的已武装回调，不能归因为这一条拒绝或析构响应。系统 DLL 没有下载符号，保留模块与偏移；本地 EXE 的 PDB 与源码位置匹配。初始 `WM_CREATE` 内仍会执行标签、来源及状态 provider，是下一步需要独立核对的构造期重入边界，当前尚未证明它就是写坏起点。主题链也不能作为本次归因：这些夹具未启用 `Theme.enabled`。

[专项日志](../../build/verification/abi-transport-parent-20261021/query-full-02/0-transport-query-lifetime/result.log)记录两条密集轨道结果：2 键和 100,000 键各执行 8 次稳定定时器 tick，C++ 分配的 `bytes` 与 `largest` 均为 `0`。这只证明当前夹具的稳定帧查询路径没有随键数增长的分配，不是全部接口无分配承诺，也不是性能加速基准。隐藏定时器零查询、同帧不重复重绘、零跳帧、零播放切换、来源读取及曲线历史不变均有独立断言。

运行继承 `Default` 桌面，`realPrivateDesktop=false`。`SW_HIDE` 只是进程启动请求，窗口显示由夹具控制，不能称为独立桌面隔离或整轮隐藏窗口验收。本批没有新增 Win7、AMD、真实 IME、多 DPI 或 GPU 硬件验收，详见[本机窗口回归更正](abi-verification-desktops-2026-10-21.md)。

`Session.seek` 和 `Session.setPlaying` 的写回调协议、宿主停止及采样路径、`Source.subdivide` 和一般 `Channel.sample` 调用仍未统一保护。此记录只确认两个只读查询，不能据此认为全部 transport 回调或整个编辑器支持任意重入。Win7 SP1 x64、CPU 基础路径、AMD 兼容、程序旁 `data`、标准 VMD 及现有文件格式兼容要求保持不变。普通生产宿主的 `mms.edit` 未因此开放，正式 release 未更新。
