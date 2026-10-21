# 曲线编辑器的原生构造与插件回调边界

日期：2026-10-21。分支：`codex/plugin-abi`。

首次打开曲线编辑器时，宿主回调现在从原生 `CreateWindowExW` 返回后才开始执行。此前完整控件树虽然已在 `WM_CREATE` 内建立，标签、来源、历史、时钟等回调仍在原生创建调用栈内运行；共享 C++ 会话保活并不能证明原生窗口创建期间任意关闭、替换窗口都安全。本批收紧这一边界，不增加公开 ABI，也不改变曲线、VMD 或工程格式。

实现见 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)，回归见 [animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp)。帧号与播放状态查询的规则和此前内存故障证据保留在[查询生命周期记录](abi-curve-transport-query-lifetime-2026-10-21.md)。

## 初始化顺序

`WM_CREATE` 只创建原生控件、字体与画刷，设置样式并安装子类。外层创建成功返回且原会话仍有效后，`Open` 依次执行原有语言、来源、烘焙字段、步长、布局、状态、定时器和来源进入流程。每步结束核对原会话；旧回调关闭窗口或安装新会话后，外层初始化停止，不清理新会话。

构造期间的绘制验证真实更新区域并绘制背景；命令、输入、焦点、捕获、定时器、激活及会调用 provider 的私有查询不会进入业务回调。`WM_CLOSE`、`WM_DESTROY`、`WM_NCDESTROY` 和子控件清理链继续运行。公开 `SetLanguage` 在此阶段不生效，同 owner 的 `Open` 拒绝替换半成品窗口。

独立复审发现初稿还会吞掉原生 EDIT、combo 和 tree 的维护消息。收尾版本只消费键鼠等业务输入，其余消息跳过自定义编辑逻辑后转交保存的原生窗口过程。必要控件的 `EM_SETMARGINS` 也在返回后立即校验会话，再发送下一条消息，避免重入关闭后继续访问旧控件。

来源通知保留待刷新来源集合。构造期间不投递反复刷新的消息，初始化完成后合并为一次唤醒；正常读取仍遵循已有来源保护。`InvokeProvider` 还设置构造期防线，避免遗漏的调用入口默默穿过边界。

任一必要原生控件创建失败，当前 `Open` 失败并清理半成品。树控件正常清理子类尚未安装时，父窗口销毁路径负责拆下并释放已有状态图像列表；已安装的清理链不会重复释放它。

## 专项怎样证明边界

新增 `--native-construction-lifetime`，并注册为 CTest。测试在当前测试线程安装 `WH_CALLWNDPROC` 与 `WH_CALLWNDPROCRET`，在真实父窗口的 `WM_CREATE` 进入和返回处观察。返回钩子仍处于外层 `CreateWindowExW` 内，此时全部控件已经存在，provider 调用计数仍须为零。只读私有消息返回「原生创建已返回」和「控件安装完成」两个独立状态，避免用控件数量间接替代创建边界证据。

钩子内实际发送绘制、输入、焦点、捕获、定时器、命令、公开语言更新、同 owner 重开和重复来源通知。测试要求无来源读取、写入、跳帧、播放切换或会话替换；绘制更新区域被验证。原生创建返回后，七类 provider 正常初始化，重复来源通知只产生一次实际刷新唤醒。

七类初始 provider 为帧号、播放状态、标签、来源读取、历史查询、选择读取和比较模式读取。每类覆盖闭包复制、执行、最终释放三个阶段，关闭、同窗口换会话、关闭后新建三种退役方式，以及正常与异常两类响应，共 126 个组合。断言保留原闭包存储、禁止旧初始化继续、禁止误清理替换会话，并要求替换窗口随后仍可编辑。

另用当前线程 CBT 钩子实际否决 EDIT、画布及 MMD 控件的原生创建，验证失败后可以重新完整打开；构造返回钩子内的关闭及关闭后重建保留真实销毁处理。测试不使用全局输入注入。

收尾测试在 EDIT 自身 `WM_CREATE` 返回时，先安装继续转交原生过程的观察器；产品随后把该观察器保存为自己的前一层。在父窗口构造返回钩子内实际切换焦点、捕获和显隐，要求观察器确实收到维护消息。它位于产品子类之下，不能仅凭外层消息钩子出现过消息就通过。测试还检查 provider 仍为零、业务键未进入原生编辑、就绪后保持同一 EDIT 焦点，以及销毁链恰好清理一次。

## 当前验证结果

| 范围 | 证据 |
| --- | --- |
| 首次构建 | 两套配置均在新测试的枚举与整数字面量混合列表处报 `C3535`。修正为显式 `std::array<int,5>`，入口与断言保持。原始日志为 `build-full-01.log` 和 `build-original-01.log`。 |
| 修正后构建 | `build-full-02.log`、`build-original-02.log` 均退出 `0`。 |
| 完整配置构造专项 | [construction-full-02/report.json](../../build/verification/abi-curve-construction-20261021/construction-full-02/report.json)：1,598 项检查，退出 `0`。 |
| 原始兼容配置构造专项 | [construction-original-01/report.json](../../build/verification/abi-curve-construction-20261021/construction-original-01/report.json)：1,598 项检查，退出 `0`。 |
| 首次沙箱桌面启动 | [construction-full-01/report.json](../../build/verification/abi-curve-construction-20261021/construction-full-01/report.json)：继承沙箱桌面，进程无测试输出，以 `0xC0000142` 初始化失败结束。不能记为产品断言失败或成功；之后在正常桌面重新运行，保留原记录。 |
| 完整配置编辑器全集 | [construction-curve-full-01/report.json](../../build/verification/abi-curve-construction-20261021/construction-curve-full-01/report.json)：26,638 项检查，退出 `0`。 |
| 原始兼容配置编辑器全集 | [construction-curve-original-01/report.json](../../build/verification/abi-curve-construction-20261021/construction-curve-original-01/report.json)：26,638 项检查，退出 `0`。本轮通过不能单独证明此前堆损坏已修复。 |
| 冻结产物的独立调试 | [after-debug-original-01-scratch/report.json](../../build/verification/abi-curve-construction-20261021/after-debug-original-01-scratch/report.json)：真实子进程约 69 秒后以断言退出码 `1` 结束，未超时、无内存异常现场。选择 writer 的 `entry=0/phase=0/action=2/response=2` 在关闭后新建并抛出复制异常后，替换窗口最初仍有两键且状态正确，但后续点击键并删除未满足一次编辑、剩余一键的断言。此失败与旧堆损坏的关系尚无证据，分别保留。 |
| 增加诊断后的独立全集调试 | [after-debug-original-02-scratch/report.json](../../build/verification/abi-curve-construction-20261021/after-debug-original-02-scratch/report.json)：约 86.7 秒后以 `0xC0000374` 退出，未超时。保存首、二次异常完整转储及 39 帧合法展开，见[首机会堆栈](../../build/verification/abi-curve-construction-20261021/after-debug-original-02/fault-0001-C0000374-first/stack.txt)。选择矩阵本轮通过，故未触发新增选择失败日志。 |
| 两处复审修正后的构建 | `build-full-05.log`、`build-original-05.log` 均退出 `0`。 |
| 两处复审修正后的构造专项 | [完整配置](../../build/verification/abi-curve-construction-20261021/construction-full-03/report.json)、[原始兼容配置](../../build/verification/abi-curve-construction-20261021/construction-original-02/report.json)各 1,627 项检查，退出 `0`。原 126 组合保持。 |
| 两处复审修正后的编辑器全集 | [完整配置](../../build/verification/abi-curve-construction-20261021/construction-curve-full-02/report.json)、[原始兼容配置](../../build/verification/abi-curve-construction-20261021/construction-curve-original-02/report.json)各 26,667 项检查，退出 `0`。独立调试的失败仍须分别追踪。 |

成功的窗口回归继承 `Default` 桌面，`realPrivateDesktop=false`。启动时的 `SW_HIDE` 请求不保证夹具整轮窗口隐藏，不代表独立桌面、真实 IME、多 DPI 或 Win7/AMD 硬件验收。

此前两次 `0xC0000374` 堆损坏已保存完整转储，其中第二次的合法堆栈显示故障在下一用例创建控件的系统分配路径检测到，未证明损坏起点。修改前默认堆、全部私有堆和 ASan 诊断有通过记录，也不能抵消故障。本次构造保护具有独立可验证的契约；在复现和归因完成前，不称为已修复上述堆损坏。两套回归的 EXE、匹配 PDB 和源码已[冻结并记录哈希](../../build/verification/abi-curve-construction-20261021/after-01/manifest.json)，用于继续独立调试，避免后续构建覆盖现场所需符号。

独立调试暴露的选择恢复失败也保持未解决。随后只为该断言增加点击坐标、绘图区、选中字段、按钮状态、焦点、捕获和修饰键日志，保留全部原断言；诊断产物另存 [after-02/manifest.json](../../build/verification/abi-curve-construction-20261021/after-02/manifest.json)。不得将正常全集通过表述为独立调试已经通过。

第二轮故障仍在下一比较用例的 `WM_CREATE → CreateControls → MakeControl` 系统分配路径检测到堆损坏。该用例尚未武装 writer，原生创建期也已禁止 provider，故本次证据排除了「只把初始 provider 移出原生创建就能消除故障」的推断。构造保护未修复该堆损坏，后续诊断继续使用冻结产物，避免将未归因故障隐藏在新的功能修改中。

## 外部扩展对照

部分故障现场记录到 `AudioDevProps2.dll`，但模块存在不等于损坏归因。独立调试工具第 03 版仅为自己创建的测试子进程提供可选旧式扩展限制，默认仍继承原环境；每次核验子进程实际策略，失败不回退。该策略的范围见[微软接口说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)。工具不修改系统设置，也不进入产品构建。

四轮对照都使用相同的冻结 `after-02` EXE、PDB、命令和 Default 桌面，运行顺序为限制、继承、继承、限制。逐轮命令、哈希、实际策略、完整加载事件和转储见 [extension-comparison-01.json](../../build/verification/abi-curve-construction-20261021/extension-comparison-01.json)。

| 运行 | 实际限制位 | 结果 | 加载事件中的 `AudioDevProps2.dll` |
| --- | --- | --- | --- |
| `extension-disabled-01` | `1` | 26,638 项检查，退出 `0`。 | 未记录。 |
| `extension-inherit-01` | `0` | `0xC0000374`，保存两次异常现场。 | 已记录。 |
| `extension-inherit-02` | `0` | `0xC0000374`，保存两次异常现场。 | 已记录。 |
| `extension-disabled-02` | `1` | `0xC0000374`，保存两次异常现场。 | 未记录。 |

反向复核推翻了「限制旧式扩展即可消除故障」的推断；没有该 DLL 的已记录加载事件时仍发生堆损坏。本组结果不能指认该模块，也不能作为产品修复。此处使用的是复审两处维护修正前的冻结产物，后续源码变更不影响已有现场与符号。

## 缩小内存故障范围

另建独立诊断工程，在冻结测试的每次关闭前后校验全部进程堆，并保留原执行顺序、条件分支和断言。在相同调试环境中，前 3,474 次校验均报告 8 个堆有效，下一比较用例的原生创建仍以 `0xC0000374` 结束，见 [after-all-heaps-debug-01-scratch/report.json](../../build/verification/abi-curve-construction-20261021/after-all-heaps-debug-01-scratch/report.json)。这没有找到首次写坏位置，也不能用校验通过排除更早但未被检测到的损坏。插入点和源码哈希见[诊断清单](../../build/verification/abi-curve-construction-20261021/all-heap-probe-after02/source-manifest.json)。

最小独立工程复制并核验 21 个生产依赖，只执行正常来源与时钟回调，没有原夹具的复杂退休矩阵、全局 `new` 替换、失败注入或手工测试消息。它在同一调试工具及继承环境中得到：

| 范围 | 结果 |
| --- | --- |
| 2,000 次创建／关闭 | [minimal-create-debug-01-scratch/report.json](../../build/verification/abi-curve-construction-20261021/minimal-create-debug-01-scratch/report.json)：200,001 项检查，退出 `0`。 |
| 2,000 轮同窗换会话后关闭 | [minimal-replace-debug-01-scratch/report.json](../../build/verification/abi-curve-construction-20261021/minimal-replace-debug-01-scratch/report.json)：234,001 项检查，退出 `0`。 |
| 选择专项单独调试 | [after-debug-selection-01-scratch/report.json](../../build/verification/abi-curve-construction-20261021/after-debug-selection-01-scratch/report.json)：5,158 项检查，退出 `0`，未重现先前恢复断言失败。 |

最小工程的关闭后 GDI／USER 计数在初始化后保持 `8/4`，不随轮数增长；初始化前为 `0/1`。这些结果只缩小调查方向，不能替代原复杂夹具或否定其堆损坏。冻结依赖及差异见 [source_manifest.json](../../build/verification/abi-curve-construction-20261021/minimal-create-loop/source_manifest.json)。

`seek`、`setPlaying`、`subdivide` 和一般 `Channel.sample` 尚未全部接入同一回调保护，真实宿主停止播放的重复求值也仍需处理。Win7 SP1 x64、CPU 基础路径、AMD、程序旁 `data` 和现有文件兼容要求不变。正式 release 未更新。
