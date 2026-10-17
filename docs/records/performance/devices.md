# 运算器与 GPU 蒙皮记录

本页按功能归并实现、诊断与验收资料。各记录中的「当前」「已完成」只适用于其注明的源码、构建和测试条件；当前使用入口见[文档导航](../../README.md)。

## 记录索引
- [通用运算器设置方案](#record-processor-settings-proposal)
- [正式 D3D9/MME 蒙皮运算器](#record-gpu-skinning)
- [蒙皮运算器自动选择](#record-skinning-autoselect)
- [跨启动复用自动测速结果](#record-persistent-skinning)
- [DX12 可选运行时与 DX9 回退](#record-dx12-optional-compat)
- [DX12 缺失时的启动兼容验证](#record-dx12-startup-validation)
- [D3D12 skinning migration: first native viewport](#record-d3d12-migration)

<a id="record-processor-settings-proposal"></a>
## 通用运算器设置方案

原始记录：`PROCESSOR_SETTINGS_PROPOSAL.md`。其中的版本、日期和验证范围沿用原记录。

状态：三个分项、按本机用户跨工程保存，以及下次启动生效已实现。设备策略与配置隔离测试已通过。入口已移至「帮助」，菜单刷新保留现有子菜单句柄，修复展开后闪烁的问题。主渲染和顶点计算选项始终显示；MMV 处理设备只在宿主成功加载 MMV.dll 后显示。主程序已加入高性能显卡偏好导出。主渲染菜单已按完整设备状态显示：D3D9 可用设备保持可选，DXGI 检测到而 D3D9 未提供的设备以标注「当前 D3D9 未提供」的不可选信息行列出。Release 构建通过，正式程序的菜单交互和设备选择由用户手动验证；完整场景性能对照尚未验收。

<a id="record-processor-settings-proposal-设置范围"></a>
### 设置范围

保留一个「帮助 → 运算器」入口。主渲染和顶点计算选项始终显示。MMV 处理设备选项只在宿主成功加载 MMV.dll 后显示。

| 选项 | 控制范围 | 可选值 | 默认值 |
| --- | --- | --- | --- |
| 主渲染设备 | 主窗口、阴影、描边、透明绘制、最终 MME 输出 | 自动、具体显卡 | 自动选择可用的高性能显卡 |
| 顶点计算设备 | 原生模型顶点变换的 CPU 和 GPU 后端 | 自动、CPU、具体显卡 | 自动，沿用该阶段的正确性校验和耗时比较 |
| MMV 处理设备 | MMV 的计算着色器、网格处理、纹理与中间绘制 | 跟随主渲染、CPU、具体显卡 | 跟随主渲染 |

「主渲染设备」首版不提供 CPU，避免一次计算设置把整个窗口切换到软件渲染。选择「顶点计算设备 → CPU」不会改变主渲染设备。

MMV 的计算和中间绘制仍共用同一设备。现有 `Dx12` 后端把两类 pass 放在同一设备、命令队列和资源管理中。若进一步拆分，同一效果的中间网格与纹理就需要跨设备传递，不能视为零成本的设备选项。因此建议第一版把 MMV 作为独立项，不在每层或每个 pass 中添加设备选择。

MMV 的 CPU 模式计划使用软件图形后端。DX12 路径对应 WARP，DX9 效果兼容路径对应 REF；可用性、FX 正确性和耗时需要实际验收。不可用时明确显示原因，不能让硬件执行后仍显示 CPU。

<a id="record-processor-settings-proposal-物理求解器与设备选择"></a>
### 物理求解器与设备选择

Bullet3 及其他物理 FX 的算法、分组、层级和描绘顺序继续由效果配置决定。设备选择不更换算法，也不把只有 CPU 实现的求解器自动变成 GPU 实现。

MMV 的设备选项约束 MMV 自己创建的图形与计算设备。第三方原生求解器只有在声明并实现相应设备接口后，才可服从该选择。界面应显示实际运行设备，不把未支持的模块标成 GPU 执行。

<a id="record-processor-settings-proposal-用户配置与工程隔离"></a>
### 用户配置与工程隔离

保存到 `data/processor.ini`，作用域为当前程序目录。多个工程共用该配置。

PMM、VMM、VEM 和附件不保存运算器选择。打开、新建、另存或切换工程均不改变用户设置。旧 PMM 中的运算器字段不再影响运行，重新保存时移除该旧字段；其他项目设置继续按原规则处理。

配置保存设备身份和选择意图。显卡枚举序号与 LUID 不作为跨重启的唯一身份。启动时重新枚举，先匹配完整设备信息；无法唯一匹配时显示原选择失效和实际回退设备，不悄悄覆盖用户保存的意图。

<a id="record-processor-settings-proposal-生效规则"></a>
### 生效规则

菜单同时显示「当前设备」与「下次启动」。修改只更新待生效配置，不重建当前设备，也不在当前会话单独切换顶点或 MMV 后端。

保存成功后提示「下次启动生效」。程序不自动退出或重启。写入失败时保留原配置并明确报错，不显示为已保存。

下次启动先读取用户配置，再初始化各设备。「跟随主渲染」绑定该次启动的实际主渲染设备。设备丢失后的资源重建仍遵守本次启动的选择，不能各模块自行换卡而不报告。

<a id="record-processor-settings-proposal-验收范围"></a>
### 验收范围

<a id="record-processor-settings-proposal-混合显卡的可见设备"></a>
#### 混合显卡的可见设备

主渲染列表仅显示当前 D3D9 接口枚举到的设备，不把 DXGI 的计算设备列表直接作为可用主渲染设备。在本机 Intel UHD / RTX 4070 Laptop 探针中，普通程序的 D3D9 仅列出集显；加入 `NvOptimusEnablement=1` 和 `AmdPowerXpressRequestHighPerformance=1` 后，仅列出独显。系统 D3D9 与真实 MME 代理的结果一致，两侧均成功创建 HAL 设备。原始结果位于 `.local/artifacts/renderer-adapters-20260927/`。

这些导出表达高性能偏好，最终可见设备仍由系统和驱动决定。它们不保证集显、独显同时出现在主渲染列表，也未实现任意混合显卡之间的按配置切换。主程序保留 MME 原有的 `Exp*` 导出序号 1 至 37。用户已明确取消多显卡组合协同渲染。

<a id="record-processor-settings-proposal-主渲染设备菜单的完整设备显示"></a>
#### 主渲染设备菜单的完整设备显示

「主渲染设备」子菜单显示本次启动能观察到的全部设备。当前 D3D9 枚举提供的设备保留为可选项，命令号与真实选择映射完全不变。DXGI 检测到、而当前 D3D9 未提供的设备排在可选项之后，每台一行不可选信息行，标注「当前 D3D9 未提供」（英文 `not provided by the current D3D9`、日文「現在の D3D9 では未提供」）。这类行的命令号为 0、始终置灰，不是菜单命令：它不能被选择，也不存在选择后静默回退到别的设备的路径。仅由 DXGI 检测到不等于验证了 D3D9 支持，因此这类行既不出现在可选项中，也不会被描述为可用的主渲染设备。

两个列表的合并按 LUID 去重：同一台设备在同一启动内 LUID 相同，只保留可选设备行，不重复列出。LUID 未知（为 0）的一侧改用完整硬件身份（vendor/device/subsystem/revision）匹配，且匹配必须唯一——两侧各自只有一条同身份记录时才合并。身份不能唯一区分时不合并：同型号不同卡共用同一身份，错误合并会隐藏一台真实设备。设备名称从不作为合并依据，同名不同卡各自保留。额外检测行之间同样按 LUID 去重；无 LUID 的行不互相合并，因为它们可能是同型号的两台卡。

设备列表与分类结果都使用处理器缓存：`RendererAdapters()` 与 `ComputeAdapters()` 各自在启动时枚举一次，菜单刷新只读缓存，不重新枚举设备。主渲染的「当前设备」「下次启动」以及自动模式显示与之前一致，如实反映本次实际设备与已保存的选择。顶点计算设备与 MMV 处理设备子菜单保持原有设备列表逻辑。

回归测试在 `tests/skinning_backend_tests.cpp` 的 `--menu-only` 模式（CPU、隐藏窗口、无 GPU 性能采样）：纯分类用例覆盖单卡、混合、重复 LUID、同型号不同卡、未知 LUID 的唯一身份匹配与不唯一不合并、同名不合并；消息用例核对实际菜单行的命令号、置灰状态、标注文字与三语言切换。结论只覆盖菜单枚举展示，不构成对卡顿问题的任何结论。

<a id="record-processor-settings-proposal-性能状态"></a>
#### 性能状态

设备配置在启动时读取，设备列表缓存到内存，菜单选择只保存下次启动的设置。MMV 处理设备通过宿主服务在创建实例时获取。当前未发现每帧重新枚举设备或读写配置的路径。默认设备的改变仍可能影响耗时，不能据此断言整帧性能没有回退。性能采样暂停期间，仅保留源码核对和已完成的构建证据。

<a id="record-processor-settings-proposal-后续验收"></a>
#### 后续验收

功能验收覆盖三个分项的实际设备身份、CPU 选择、显卡缺失、重启生效、写入失败，以及跨工程不变。工程文件验收检查 PMM、VMM 和 VEM 均不写入运算器配置。

渲染验收使用隐藏窗口和渲染目标回读，覆盖主渲染、MME、MMV 两种发行版及模型顶点结果。性能验收记录同条件前后构建及原始耗时，分别报告顶点计算和整帧指标，不把顶点测速称为全局渲染性能。

完成实际功能及验收后，再将相关中、英、日菜单名称统一改为通用名称，去掉运算器设置中的「蒙皮」字样。文档中用于说明技术原理的「蒙皮」不做无关替换。

<a id="record-gpu-skinning"></a>
## 正式 D3D9/MME 蒙皮运算器

原始记录：`GPU_SKINNING_2026-09-25.md`。其中的版本、日期和验证范围沿用原记录。

本次把已有 D3D12 计算蒙皮接入正式 D3D9/MME 渲染流程，在「显示 → 蒙皮运算器」中选择设备。没有启用功能不完整的 D3D12 预览窗口。

<a id="record-gpu-skinning-设备与回退"></a>
### 设备与回退

- 2026-09-26 起，默认项「自动（优先高性能）」按每个模型的实测耗时选择 CPU 或 GPU，见[蒙皮运算器自动选择](devices.md#record-skinning-autoselect)。
- 菜单列出支持 D3D12 FL 11_0 的硬件适配器及 CPU。同名适配器加序号区分。
- 按 D3D12 的 UMA 属性区分独显与集显，组内保留 DXGI 高性能顺序。软件、远程及不能确认架构的适配器不列入。
- 手动选卡先尝试指定设备，失败后继续尝试其余硬件，最后回退 CPU。不会每帧重复尝试故障设备；重新选择运算器可再次尝试。
- QDEF、未知权重类型及不满足现有 GPU 输入条件的模型继续走原有 CPU 分支。其他模型仍可用 GPU。
- 「最近运算」显示最后一次模型更新的设备或 CPU 回退原因，不表示多模型场景的所有模型都使用同一设备。

全局设置保存在 EXE 同级的 `data/skinning.ini`，不改位置式 `mmconfig.ini`。启动读取设置，菜单选择即时生效并保存。缺失、损坏或无法匹配的全局设置使用自动模式。并行加入的 PMM v2 工程设置可覆盖全局偏好，按显卡类型恢复；具体格式及保留规则见 [PMM v2 扩展说明](../../reference/pmm-extensions.md)。

系统重启后 GPU 的 LUID 可能改变，此时仅在名称唯一时恢复原设备；同名设备无法确认身份时回到自动模式。本机 Windows 返回一个 NVIDIA RTX 4070 Laptop GPU 适配器和四个不同 LUID 的 Intel UHD 适配器。这是逻辑适配器清单，不能解释成四块物理集显。

设备判定依据：[D3D12 架构属性](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture)、[DXGI 高性能枚举](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgifactory6-enumadapterbygpupreference)、[设备 LUID 有效期](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getadapterluid)。

<a id="record-gpu-skinning-正式渲染接入"></a>
### 正式渲染接入

`UpdateModelVertexBuffers` 按当前运算器选择调用 CPU 或 GPU。D3D12 在一次命令提交中计算蒙皮并复制到回读缓冲，等待完成后写入原有 D3D9 主体和描边顶点缓冲。

保留缓冲对象身份、32～96 字节主体步长、16 字节描边步长和 PMD 已有描边颜色。MME、阴影、透明排序、录制和附件继续使用原有 D3D9 渲染接口。只有计算和两个缓冲锁定都成功后才写入结果；失败由原 CPU 分支重新计算。

每次更新重新打包顶点及 UV morph 输入，避免缓存遗漏编辑或插件修改。骨骼求解、物理和 morph 准备仍在 CPU。该方案需要同步回读及重新上传，不是 GPU 常驻、零拷贝渲染，也不代表所有工作都移到了显卡。

<a id="record-gpu-skinning-验证记录"></a>
### 验证记录

源码基线为 `8bb3a123dc01d2c48df0497bad4f6750338ea6cf`。为排除并行进行的透明排序、美影及 EMM 保存修改，在 `.local/worktrees/gpu-skinning-verify/` 单独应用 GPU 计算、正式缓冲桥接及菜单改动。隔离构建不含并行 PMM 类型保存的接入，该接入随后在 main 整合验证。构建为 VS 2022 x64 Release，`/O2 /Ob2 /DNDEBUG`，`MIKUDANCESTUDIO_PERF=OFF`、`MIKUDANCESTUDIO_DIAG=OFF`。

所有运行采用隐藏窗口，不采集桌面截图。图像来自程序的 D3D9 渲染目标。构建日志位于 `.local/build-gpu-skinning-*.log`；原始日志、JSON、图像、EXE 和哈希清单位于 `.local/artifacts/gpu-skinning/`。

| 验证 | 结果 |
| --- | --- |
| `skinning_backend_tests` | 60 组 CPU/GPU 比较通过；覆盖 PMD、BDEF1/2/4、SDEF、PMX 0～4 组附加 UV、两种描边透明状态、全部枚举设备、切回 CPU、QDEF 回退、保存设置与三语言菜单。 |
| `d3d12_skinning_tests`：洛希极限角色 | 183698 顶点、969 骨骼、三组姿态通过。NVIDIA 计算内核对 CPU 最大绝对误差为 0；各设备的正式桥接比较均在容差内，缓冲对象未更换。 |
| `d3d12_skinning_tests`：PMD 讲座箱子 | 基线和修改版均通过三组姿态比较。 |
| 正式 MME | 确认 `MMEffect.dll` 已加载；CPU/GPU 角色及 HGrass 完整渲染通过，GPU 样本没有 CPU 回退。 |

浮点容差为 `2e-5 * max(1, abs(reference))`，描边颜色必须完全相等。基准新增 `--processor auto|cpu|gpu`；`gpu` 模式在未发生 GPU 运算或发生 CPU 回退时返回失败。JSON 记录计算适配器、GPU 更新数和 CPU 回退数。

<a id="record-gpu-skinning-正式目录整合验证"></a>
#### 正式目录整合验证

main 同时包含并行的透明排序、EMM 和 PMM 设置改动。正式 Release 构建通过，60 组设备/布局比较、角色三组姿态逐卡比较及 MME CPU/GPU 渲染全部通过。PMM 扩展的 CRC 修正落盘后再次增量构建，并补跑 MME GPU 渲染，退出码为 0，渲染目标图像正常写出。此处只确认集成正确性，不用混合改动的样本替代下表的隔离性能对照。

交付程序为 `build/Release/MikuMikuDance.exe`，该次 SHA-256 为 `6CE061F2F7B1FADA58F5F001D437780917D27733D8F6AFD878547F9BE2C4AAA7`。原始构建日志为 `.local/build-gpu-skinning-integrated-4.log` 和 `.local/build-gpu-skinning-integrated-final.log`；源码、未提交状态、二进制哈希及运行命令均保存在 `integrated*-manifest.json`、`integrated-runs.json` 和 `integrated-final-mme-gpu-run.json`。后续其他任务重新构建时，日常 EXE 哈希可能改变。

<a id="record-gpu-skinning-性能限制"></a>
#### 性能限制

下表为包含 GPU 排空的批次毫秒/帧，不是窗口播放 FPS 或纯 GPU 时间。每次预热 30 帧、采样 120 帧，1280×720，自阴影模式 1。基线与 GPU 按 ABBA 顺序交替运行，随后补一次修改版 CPU。保留物理调用、描边、透明排序和阴影。

| 场景 | 基线 CPU 两次 | GPU 两次 | 修改版 CPU 一次 |
| --- | --- | --- | --- |
| 角色近景，183698 顶点、701 刚体、1098 关节 | 16.0582 / 14.8625 ms | 31.7777 / 34.4704 ms | 12.2424 ms |
| 两份 HGrass，spread 布局 | 30.7174 / 30.4320 ms | 139.5830 / 129.3390 ms | 30.3078 ms |

MME 角色样本另测 CPU 12.8150 ms、GPU 36.6539 ms，各一次。样本较短，不能用修改版 CPU 与基线的波动宣称额外加速。

**当前兼容回读方案在这些样本上慢于 CPU，性能收益未通过验收。** 该轮交付时默认 GPU 优先；2026-09-26 起改为实测自动选择，在这两组场景自动选用 CPU。本次交付的是可选择的 GPU 蒙皮能力，不能称为已经完成的蒙皮加速。

<a id="record-gpu-skinning-图像差异与覆盖边界"></a>
#### 图像差异与覆盖边界

未宣称逐像素相同。1280×720 的前后图像比较中，角色有 884 个像素不同，最大通道差 18，平均通道差 0.000607；草地有 453 个像素不同，最大通道差 46，平均通道差 0.000660。MME CPU/GPU 有 415 个像素不同，平均通道差 0.000621。

同一基线重复运行也有差异：角色 523 个像素、最大通道差 24；草地 377 个像素、最大通道差 52。同一 GPU 构建重复运行的角色差异为 950 个像素。尚未定位重复渲染差异来源，不能据此宣称严格像素一致。

已测 GPU 计算、正式缓冲写入和 MME 默认渲染。未穷举第三方 MME 效果、录制、设备热拔插、TDR 或没有可用 GPU 的实机。保留接口不等于逐项实机验收。

<a id="record-gpu-skinning-本地产物与协作"></a>
### 本地产物与协作

- `before/`：未修改基线 EXE、Data、构建参数和 SHA-256。
- `after/`、`final/`：中间验证构建，对应原始正确性日志。
- `release/`、`mme-release/`：ABBA 及 MME 性能样本对应构建。
- `verified/`、`mme-verified/`：复核后加入输入指针和显式缓冲长度检查的最终隔离构建。
- `integrated/`、`mme-integrated/`：首次正式整合构建及回归产物；`integrated-final/`、`mme-integrated-final/` 保留 PMM CRC 修正后的交付构建。
- `release-manifest.json`、`correctness-runs.json`、`compare-runs.json`、`mme-runs.json`、`summary.json`：版本、命令、退出码和结果。
- `run-verification.py`：隐藏窗口串行运行、超时及日志保留脚本。异常样本原样保留。
- `.local/worktrees/gpu-skinning-verify/` 保留供复现，不自动删除或归档。

DSH 实际工作目录均为 `D:/MMD-Next/MikuDanceStudio-MME`。只读审查 `5585511f-b707-4e96-b3d0-8a88a2b68202` 完成。设备模块任务 `c4b2cef1-3374-4367-b7ca-ee48dd30b232` 写入目标文件后由主 agent 接管，其终态为 `aborted`，不把它当作验收成功。主 agent 修正 DXGI 旧接口参数、架构查询失败处理及重复 LUID 过滤，完成构建和运行验证。

<a id="record-skinning-autoselect"></a>
## 蒙皮运算器自动选择

原始记录：`SKINNING_AUTOSELECT_2026-09-26.md`。其中的版本、日期和验证范围沿用原记录。

显示菜单的「运算器（蒙皮）」改名为「蒙皮运算器」，默认项由「自动（独显优先，其次集显，最后 CPU）」改为「自动（优先高性能）」。自动模式不再按设备类别排序，而是逐个模型实测 CPU 和全部可用 GPU 的完整蒙皮耗时，选择结果正确、耗时最低的设备。设备类别只影响菜单文字和 PMM 保存的类型意图，不决定自动模式的胜者。

<a id="record-skinning-autoselect-自动项显示当前设备"></a>
### 自动项显示当前设备

自动菜单项追加冒号与最近一次模型运算的实际设备，例如「自动（优先高性能）：CPU」或「自动（优先高性能）：NVIDIA GeForce RTX 4070 Laptop GPU」。设备名称和下方「最近运算」来自同一运行状态；多模型场景显示最后更新的模型所用设备。兼容回退保留 CPU 原因说明，尚未运算时显示「等待模型运算」。打开菜单时更新文字，原有勾选、运算器选择及设置保存规则不变。

本次修改前后的 Release 构建均通过 60 组硬件回归。修改版另核对未运算、逐卡切换、CPU 兼容回退与中文自动项文字。仅调整菜单格式，不改蒙皮或测速算法，不据测试运行时长声称性能变化。构建日志、前后 EXE、哈希清单、命令与退出码保存在 `.local/artifacts/skinning-device-label/` 和 `.local/build-skinning-device-label-*.log`。

DSH 委派 `1e74a4db-a8de-40dc-9cf5-05d8bb0af6e7` 的实际 cwd 为 `D:/MMD-Next/MikuDanceStudio-MME`，因供应商返回 `429 quota exhausted` 失败且未写入文件。菜单代码、现有回归用例和整合均由主 agent 完成。

<a id="record-skinning-autoselect-测速规则"></a>
### 测速规则

首次处理某个模型时，CPU 和每个候选 GPU 各预热 3 次，随后交替执行 5 轮采样，每轮同时取一个 CPU 样本和一个 GPU 样本，先后顺序逐轮交换。候选的耗时是自身样本排序后的中位数；CPU 的中位数取自全部轮次的 CPU 样本，不是首轮那 5 个。

GPU 计时覆盖输入打包、上传、计算、同步回读，以及写入原有 D3D9 主体和描边顶点缓冲。设备初始化、着色器编译和正确性读回不计入排名。

这是蒙皮阶段的墙钟耗时，不是 GPU 时间、播放帧率或整个渲染器的耗时，后续绘制中的驱动工作不属于这次排名。首次校准会增加加载等待，结果只对校准时的模型、设备负载和供电状态成立。供电或负载变化后，重新选择「自动（优先高性能）」即可重新评估。

正确性判据是 GPU 预热结果与 CPU 结果相符：主体顶点缓冲的全部浮点分量和描边位置按 `2e-5 * max(1, abs(cpu))` 比较，描边颜色必须完全相等。初始化、运算、缓冲读写或比较失败的候选不参与排名，耗时非有限或非正的候选同样被排除。

CPU 胜出属于正常选择，次数记入 `cpu_updates`，不计入 `cpu_fallbacks`。校准次数记入 `automatic_calibrations`。运行日志按候选打印 `Automatic skinning candidate id=... valid=... median_ms=...`，并按模型打印 `Automatic skinning vertices=... cpu_ms=... selected=...`。

<a id="record-skinning-autoselect-缓存与失效"></a>
### 缓存与失效

校准结果按模型缓存，键是模型地址，附带一份签名。签名包含两个顶点缓冲对象、骨骼表、材质表、蒙皮源数组指针、顶点数、骨骼数、材质数、物理模式与附加 UV 组数。这些字段任一变化即重新校准，覆盖模型替换和权重布局改动两种情况。

删除模型时 `ModelDispose` 先调用 `ForgetModel`，释放该模型的校准记录和计算设备。更换运算器、新建或加载工程走 `SetPreference`，它清空全部缓存。缓存条目达到 256 个时整体清空，避免反复单独替换模型时旧指针长期堆积。

GPU 在运行中失败时，把该设备的测速结果标为无效，按剩余有效结果重新排名并重建设备，CPU 也参与这次排名。返回 `Unsupported` 的模型直接走 CPU，不再尝试其他 GPU。不满足 GPU 输入条件的模型保留原有 CPU 路径。

全局 `data/skinning.ini` 和 PMM 继续保存「自动」意图，不把本机测得的 CPU 或 GPU 写成手动选择。手动 CPU、指定显卡和 PMM 指定显卡类型的行为不变，格式见 [PMM v2 项目扩展格式](../../reference/pmm-extensions.md)。

<a id="record-skinning-autoselect-中文文案"></a>
### 中文文案

| 原文 | 新文案 | 位置 |
| --- | --- | --- |
| 运算器（蒙皮） | 蒙皮运算器 | 显示菜单 |
| 自动（独显优先，其次集显，最后 CPU） | 自动（优先高性能） | 显示菜单 |
| 节省CPU功耗 | 节省内存模式 | 显示菜单 |
| 省电 | 省性能 | 主界面复选框 |

前两项的英文同时改为 `Automatic (best performance)`、日文改为 `自動（性能優先）`。后两项只改中文名称，仍共用原有的空闲暂停刷新开关，没有增加内存压缩、内存释放或降低画质的行为。

<a id="record-skinning-autoselect-验证"></a>
### 验证

基线为 `09483c643c7b0fff757cc31fbb0809cfe9087332`。旧侧可执行文件由该提交加上本地化子任务已改动的三个菜单源码构建，清单如实记录这个状态，蒙皮与渲染算法未改。两侧都是同一 main 工作目录的 VS 2022 x64 Release 构建，参数 `/O2 /Ob2 /DNDEBUG`，`MIKUDANCESTUDIO_PERF=OFF`、`MIKUDANCESTUDIO_DIAG=OFF`。所有运行使用隐藏窗口，图像取自渲染目标，不采集桌面截图。

| 验证 | 命令与结果 |
| --- | --- |
| `skinning_backend_tests` | 退出码 0，`PASS 60 GPU/CPU cases, PMD/PMX UV strides, fallback, settings and menus`。 |
| `d3d12_skinning_tests` | 退出码 0，洛希极限角色 183698 顶点、969 骨骼、三组姿态的最大绝对误差为 0，五个适配器的桥接比较都保持缓冲对象身份。该目标不在 CTest 默认集，显式构建运行。 |
| `pmm_project_settings_tests` | 退出码 0，6 项 PASS。 |
| `ctest --test-dir build -C Release` | 注册 29 项，通过 28 项。`model_alpha_occlusion_regressions` 失败 17 像素，与[全量透明排序实验记录](../rendering/transparency.md#record-transparency-global-sort)所记基线 `dbf189f` 的既有失败一致，与本次改动无关。 |
| `frame_benchmark` MME 自动模式 | 退出码 0，`cpu_updates=41`、`automatic_calibrations=1`、`gpu_updates=0`、`cpu_fallbacks=0`，自动模式选中 CPU。 |

`skinning_backend_tests` 新增的判定：低耗时的后序候选可以胜过前序候选；CPU 可以胜出；失败或耗时非法的候选不能胜出；实机自动决策等于全部有效候选的最小值；同一模型第二次更新复用校准；替换顶点缓冲后重新校准；删除模型释放记录；中文菜单标题为「蒙皮运算器」和「自动（优先高性能）」。

<a id="record-skinning-autoselect-性能对照"></a>
### 性能对照

按旧版、新版、新版、旧版的顺序串行运行，每次预热 30 帧、采样 120 帧，1280×720，自阴影模式 1，保留物理、描边、透明排序和 MME 接口。下表是等待 GPU 排空后的批次毫秒/帧，数值越低越好，不换算成窗口播放 FPS。

| 场景 | 旧自动模式两次 | 新自动模式两次 | 新自动模式选择 |
| --- | --- | --- | --- |
| 角色，183698 顶点、701 刚体、1098 关节 | 31.9292 / 38.6524 ms | 12.3192 / 11.9932 ms | CPU |
| 两份 HGrass，共 942244 顶点，spread 布局 | 136.3660 / 117.6650 ms | 24.9445 / 23.9371 ms | 两个模型均为 CPU |

旧自动模式在两组场景里都选了 NVIDIA GeForce RTX 4070 Laptop GPU，两轮差距来自运算器的选择，不是蒙皮算法的改动。MME 角色另测手动 CPU 11.6626 ms、自动 11.3136 ms，各一次。这两个短样本不足以声称自动模式比同一个 CPU 算法更快。

图像未宣称逐像素相同。详情见[正式 GPU 蒙皮记录](devices.md#record-gpu-skinning)。同一构建重复运行的差异与新旧差异同量级，因此不能据此判断算法差异。

<a id="record-skinning-autoselect-本轮复核"></a>
#### 本轮复核

2026-09-26 00:38 在同一工作树重新构建并复跑：`skinning_backend_tests` 退出码 0，PMM 测试 6 项 PASS，`ctest -C Release` 仍是 28/29 且失败项相同，MME 自动模式三次运行退出码 0 且都选择 CPU。宿主当时同时运行沙箱虚拟机 `qemu-system-x86_64-headless`，角色场景三次样本为 37.317 / 37.045 / 36.988 毫秒/帧，约为上表 10.7235 毫秒/帧的三倍。两次采样条件不同，这些数值只作为负载记录，不用于性能结论。

<a id="record-skinning-autoselect-复现产物"></a>
### 复现产物

产物位于 `.local/artifacts/skinning-autoselect/`，不进入 Git：

- `before/`、`before-mme/` 保存旧自动模式的程序，`after/`、`after-mme/` 保存交替采样的版本，`final/`、`final-mme/` 保存补齐生命周期与故障回退后的版本。
- `before-manifest.json`、`after-manifest.json` 记录提交、工作树状态、构建参数和源码或二进制哈希。
- `runs.json`、`final-runs.json`、`summary.json` 及同名日志、JSON、PNG 保留原始命令、退出码、耗时和图像比较。
- `run.py`、`run-final.py` 为隐藏窗口执行脚本；构建日志位于 `.local/build-skinning-autoselect-*.log`。
- `rerun/`、`rerun-mme/` 保存本轮复核的构建副本与输出，`mme-auto.json`、`mme-auto-2.json`、`mme-auto-3.json` 对应三次自动模式运行。

菜单本地化由 DSH 子任务 `7c2c3c37-2428-4241-b424-81490d559f42`（标签「蒙皮 菜单文案」）完成，实际工作目录为 `D:/MMD-Next/MikuDanceStudio-MME`。主 agent 复核三个文件的差异，并负责选择算法、测试、整合与提交。

<a id="record-persistent-skinning"></a>
## 跨启动复用自动测速结果

原始记录：`PERSISTENT_SKINNING_20261009.md`。其中的版本、日期和验证范围沿用原记录。

自动顶点计算只在首次启动时测速，将六组 PMD/PMX 结果保存到
`%LOCALAPPDATA%/MikuDanceStudio/skinning-MMS-v1.bin`。
退出后重新打开、打开空工程或重开项目都读取已有结果，不再测速。
实际模型选用 GPU 时才建立计算管线，空启动不用提前创建全部候选管线。

缓存校验 CPU、线程配置、显卡和驱动身份，不依赖重启后可能改变的 LUID。
硬件、驱动或测速版本变化，以及缓存损坏时才重新测速。缓存保存采用原子替换，
MMS 和 MMN 各用一个文件。顶点计算菜单的自动项显示“首次测速”。

<a id="record-persistent-skinning-验证与位置"></a>
### 验证与位置

- 基线 `ee1148330e6dd4b9c21faf611347da480da84ded`，EXE SHA256：
  `79227e91cfabbc3fa6669d576869a0f41ba71822608bc80dbbd9d75ddeeaab1d`。
  备份保留在 `.local/archives/persistent-skinning/`。
- 同一台机器、隐藏原生程序、独立用户配置：原版两次空启动 8.083 / 7.618 秒，
  每次都测速；新版本首次 7.304 秒，第二次 2.799 秒，第二次测速次数为零。
  这是启动个例记录，不是多轮统计。验证使用 EXE SHA256：
  `f693c5aeef85e76775d152b378c068ff1a8199e40e89b74c3df1d2d4f52cd4af`。
- `skinning_backend_tests`：120 组 GPU/CPU 情况通过，包括磁盘重用、LUID/显卡顺序变化、
  硬件失配、截断缓存及重新保存。真实 White.pmx 五种姿态的一致性检查通过。
- 构建日志：`.local/artifacts/persistent-skinning/build.log`。
  两产品统一测试证据保留在 `D:/MMD-Next/MMN/.local/artifacts/pmm-startup-physics/` 下的
  `hardware-MMS/` 与 `empty-launch/MMS-before/`、`empty-launch/MMS-after/`；包含原始日志、
  参数、哈希和私有配置。测试没有改写用户的 PMM 或实际用户配置。
- 正式入口仍为 `build/Release/MikuMikuDance.exe`。本次缓存改动不修改 MMV4MMS 渲染器。

<a id="record-dx12-optional-compat"></a>
## DX12 可选运行时与 DX9 回退

原始记录：`DX12_OPTIONAL_COMPAT.md`。其中的版本、日期和验证范围沿用原记录。

MMS 的窗口、MME 效果和最终绘制仍运行在 DX9。DX12 只为可选的 GPU 蒙皮
及原生预览提供后端。默认构建按需加载系统目录中的 `d3d12.dll`、
`dxgi.dll` 和 `d3dcompiler_47.dll`，EXE 不再强制导入这三个 DLL。
缺少 DLL、缺少入口点或没有可用 DX12 设备时，程序继续使用 DX9 和 CPU 蒙皮。
支持 DX12 的设备仍可使用 GPU 蒙皮，无需另建发行分支。

原问题发生在进入 `WinMain` 之前：Windows 加载 EXE 的静态导入时找不到
`d3d12.dll`，现有 CPU 回退代码还没有执行机会。运行时加载通过失败的
`HRESULT` 进入原有回退流程。`CreateDXGIFactory2` 不存在时，设备枚举尝试
`CreateDXGIFactory1`；DXGI 完全不可用时，DX9 设备枚举仍可继续。
DLL 句柄保留至进程退出，避免静态 COM 对象析构时访问已卸载的代码。

此修复解决可选 DX12 依赖导致的启动失败，不代表已在所有旧版 Windows 上完成验收。
程序仍需要 DX9、D3DX9 和当前构建使用的 Visual C++ 运行库。

<a id="record-dx12-optional-compat-完全关闭-dx12-的构建"></a>
### 完全关闭 DX12 的构建

使用已有的 Conan 依赖，在宿主项目根目录配置单独的构建目录：

```bat
cmake -S . -B build/dx12-disabled -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=D:/MMD-Next/MMS/build/generators/conan_toolchain.cmake ^
  -DMIKUDANCESTUDIO_ENABLE_DX12=OFF
cmake --build build/dx12-disabled --config Release --target MikuDanceStudio
```

关闭时，CMake 不生成或链接 DX12 shader、DX12 skinning 和 DX12 原生预览实现；
运算器设置中不枚举 DX12 GPU，自动模式改走 CPU。传入 `--d3d12-preview`
也不会启用预览。DX9/MME 设备和 FX 行为保持原有路径。

默认构建已能处理 DX12 runtime 缺失。关闭选项用于明确不需要 DX12 功能的发行构建。

<a id="record-dx12-optional-compat-回归验证"></a>
### 回归验证

`optional_graphics_imports` 检查正式 EXE 的普通导入表和延迟导入表，拒绝三个可选
DLL 的强制依赖。此检查不需要 GPU。

`graphics_runtime_tests` 需要本机具有 DX9 和 DX12 硬件支持。它通过进程内钩子
模拟缺少 DX12、DXGI、着色器编译器，以及缺少设备创建和新 DXGI 入口点。
每个场景单独启动进程，不修改系统 DLL。测试创建隐藏 DX9 窗口，比较自动模式、
指定失效 GPU 与 CPU 模式的主体和描边顶点，并验证 Present 与渲染目标回读。

验证命令：

```bat
cmake --build build --config Release --target MikuDanceStudio graphics_runtime_tests
ctest --test-dir build -C Release -R "^(graphics_runtime_|optional_graphics_imports)" --output-on-failure
```

<a id="record-dx12-startup-validation"></a>
## DX12 缺失时的启动兼容验证

原始记录：`DX12_STARTUP_VALIDATION_20261006.md`。其中的版本、日期和验证范围沿用原记录。

默认发行程序已移除 `d3d12.dll`、`dxgi.dll` 和 `d3dcompiler_47.dll` 的普通、
延迟导入。默认构建仍启用 DX12 功能，运行时不可用时转入 DX9 和 CPU 蒙皮。
正式输出为 `D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe`，集成分支为 `main`。

<a id="record-dx12-startup-validation-版本与产物"></a>
### 版本与产物

修复前源码为 `49020e8`，构建前工作树无改动。修复后源码为本记录所在提交。
两侧均用 Visual Studio 17 2022、x64、Release 和同一 Conan 工具链。
默认构建参数为 `MIKUDANCESTUDIO_ENABLE_DX12=ON`。

| 版本 | EXE SHA-256 |
| --- | --- |
| 修复前 | `075e87c0440d32b6bdab738f61350e07a840f015d4131153eb42c3b2c142d4b4` |
| 修复后默认构建 | `97ed3103f6024c98a6d3f420d9d79421cd6a9f4f01fe5739a9a71020e007ffe0` |
| 修复后关闭 DX12 | `70b8da9dc3cbcc097b4e4ab1db89392d6436538716c7c0abe925123d870f3915` |

原始日志、导入表、哈希和启动检查脚本保存在
`.local/artifacts/dx12-optional-startup/20261006/`。
独立运行快照位于 `.local/releases/dx12-startup-before-49020e8/`、
`.local/releases/dx12-startup-after-20261006/` 和 `.local/releases/dx9-startup-20261006/`。
这些目录用于本次对照，未纳入 Git，也未替代正式发布目录。

关闭 DX12 的构建目录为 `build/dx9-compat-20261006/`。原有
`build/dx12-disabled/` 包含迁移前路径的 CMake 缓存，配置失败日志已保留；
未删除或覆盖该目录。

<a id="record-dx12-startup-validation-验证结果"></a>
### 验证结果

| 检查 | 修复前 | 修复后 |
| --- | --- | --- |
| 正式 EXE 可选图形 DLL 导入检查 | 检出三个 DLL，按预期返回 1 | 无普通或延迟导入，返回 0 |
| 默认程序隐藏启动、消息响应、正常关闭 | 通过 | 通过 |
| 全局运算器设置回归 | 99 项通过 | 99 项通过 |
| 既有 `skinning_backend_tests` | 菜单文字断言失败 | 同一断言失败 |

既有失败为 `FAIL the automatic choice never names a device`。测试在进入主体
蒙皮对照前退出，因此本次不将该完整测试标为通过。未修改这项无关菜单断言。

新增的六个后台场景均通过：系统正常、DX12 DLL 缺失、DXGI DLL 缺失、
编译器 DLL 缺失、`D3D12CreateDevice` 缺失、`CreateDXGIFactory2` 缺失。
测试在独立进程内拦截加载或符号解析，不更改系统 DLL。每个场景均验证隐藏
DX9 硬件设备、实际 CPU 骨骼变换、Present 和渲染目标像素回读。
需要回退的场景还验证自动模式及已保存失效 GPU 选择的主体、描边顶点与 CPU 一致。
系统正常和旧 DXGI 入口场景确认 DX12 硬件初始化仍可成功。

纯 DX9 构建编译通过、导入检查通过，隐藏启动、消息响应和正常关闭也通过。
启动检查使用同一份 Data 的独立副本，三次均无可见窗口和模态错误窗口，退出码为 0。
这证明本机实际发布 EXE 可以初始化主窗口；旧系统缺少 DX12 的行为由故障注入覆盖。

<a id="record-dx12-startup-validation-尚未验收的范围"></a>
### 尚未验收的范围

未在用户截图中的旧系统实机上运行。没有进行完整模型、动作、物理、效果的前后
渲染性能对照，也未报告 FPS、GPU 时间或性能提升。当前结论限于可选图形依赖、
启动和 CPU 回退，不代表所有旧系统依赖均已验证。

实现说明与可重复执行的检查命令见 [DX12 可选运行时与 DX9 回退](devices.md#record-dx12-optional-compat)。

<a id="record-d3d12-migration"></a>
## D3D12 skinning migration: first native viewport

原始记录：`D3D12_MIGRATION_2026-09-20.md`。其中的版本、日期和验证范围沿用原记录。

The complete D3D9/MME renderer now has a separate selectable GPU skinning bridge.
Its readback costs, CPU fallback and validation are described in
[the processor selection report](devices.md#record-gpu-skinning). The preview below
remains a separate opt-in renderer with the listed feature limits.

This is an opt-in model viewport, not a replacement for the complete D3D9
renderer. Launch a clean build without the MME proxy using
`MikuMikuDance.exe --d3d12-preview [model-or-project]`.

The default renderer remains D3D9. Native recording, floating/fullscreen
windows, accessories and QDEF are rejected and fall back to D3D9. The preview
does not yet implement self-shadow maps, projected shadows, ground/grid,
selection overlays or the complete legacy material shader. Consequently its
timings are NOT feature-equivalent end-to-end FPS claims against MMD or NexGiMa.

<a id="record-d3d12-migration-implemented"></a>
### Implemented

- D3D12 compute skinning with 64 threads per group: PMD two-bone skinning,
  PMX BDEF1/2/4 and SDEF, including invalid-bone behavior and additional UVs.
- GPU outline positions and packed edge colors, consumed directly by the
  native vertex shader. No production vertex readback or D3D9 VB upload.
- Three fenced submission slots; graphics consumption extends output lifetime
  before slot reuse. Waiting is limited to reuse, resize, teardown and tests.
- Geometry stays in default-heap GPU memory across unchanged morph states.
  Bones/materials update separately. Morph-weight or model-order changes
  invalidate geometry; scene mutation and model disposal destroy the cache.
  Changing vertex/UV morphs still repacks the affected scene geometry on CPU;
  sparse GPU morph accumulation remains future work.
- Per-material GPU depth calculation and stable bitonic triangle sorting,
  retaining index buffers on GPU. The validation comparison uses the same
  float reciprocal multiplication as HLSL; it does not assert bit identity
  with CPU division for almost equal depths.
- Native texture upload, opaque/translucent/outline passes and wireframe.
  Texture migration currently copies mip level zero only.
- `frame_benchmark --native` requires successful native rendering; fallback
  makes the benchmark fail instead of silently measuring D3D9.

<a id="record-d3d12-migration-why-a-separate-native-viewport"></a>
### Why a separate native viewport

The actual D3D9On12 probe created the device and obtained `IDirect3DDevice9On12`,
but `UnwrapUnderlyingResource` returned `E_INVALIDARG (0x80070057)` for both
managed and default-pool vertex buffers. We therefore did not implement a
per-frame GPU-to-CPU-to-D3D9 transfer disguised as GPU rendering.

API references: [D3D9On12 resource interop](https://devblogs.microsoft.com/directx/coming-to-directx-12-d3d9on12-and-d3d11on12-resource-interop-apis/)
and [D3D12 resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).

<a id="record-d3d12-migration-reproduction"></a>
### Reproduction

Build `d3d12_skinning_tests`, `d3d12_interop_tests` and `frame_benchmark` with
`MIKUDANCESTUDIO_BUILD_EFFECT_TESTS=ON`, Release x64. Test executables require
the project's Data resources and real GPU access. Keep MME proxy DLLs out of
the test directory.

`d3d12_skinning_tests.exe <model>` compares all skinned floats, UVs, edge
positions and exact edge colors against D3D9 CPU buffers at three poses.
Tolerance is 2e-5 relative/absolute for floats. It also recycles submission
slots and compares cached submissions byte-for-byte. Nonzero vertex morphs
exercise geometry revision changes when a model provides vertex morphs.

`frame_benchmark.exe <model> --native <capture.bmp>` enables the D3D12 debug
layer, captures before flip-discard presentation, reads back indices explicitly
for comparison with a stable CPU sort, and fails on debug-layer errors.
Captures and readbacks are diagnostic-only and excluded from production mode.

<a id="record-d3d12-migration-verified-on-rtx-4070-laptop-gpu"></a>
### Verified on RTX 4070 Laptop GPU

Release x64, native 859x482 viewport, no MME:

- PolarisLiveStage: 129,307 vertices, 17 bones, three poses, zero measured
  skinning/outline error; stable GPU triangle order and D3D12 debug checks pass.
- SourRIN White: 37,922 vertices, 344 bones; BDEF1 18,480 / BDEF2 8,592 /
  BDEF4 8,608 / SDEF 2,242. Three poses with a vertex morph; maximum absolute
  difference 9.53674316e-7, exact edge colors, cached submissions identical.
- Synthetic PMD: 60,000 vertices, two bones, three poses, zero measured error.
- Six existing regressions pass: D3DX import hooks, effect API, bone projection,
  alpha drawing, PMM reload and morph runtime. The two device-dependent tests
  require execution outside the sandbox.

The initial native implementation took about 14.68 ms for the stage because
it rebuilt and uploaded all geometry each frame. With unchanged geometry
resident on GPU, a paired run measured 2.86 ms native versus 6.16 ms D3D9.
Later native validation captures measured 3.12 ms for the stage and 2.55 ms for
the character. Validation enables readback/debug work, and native omits the
features above: these numbers establish removal of the upload bottleneck,
not an equivalent-render-quality speedup against the original application.

The loading-performance fixture selected by the user is
`极星动力LIVE舞台 Ray用一键渲染/UserFile/StageNoRay LowEnd.pmm`.
Do not replace it with StageNoRay.pmm or count a missing-model/aborted load as
a complete successful loading benchmark.
