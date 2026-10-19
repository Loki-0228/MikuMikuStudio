# 历史功能范围审计

本页按功能归并实现、诊断与验收资料。各记录中的「当前」「已完成」只适用于其注明的源码、构建和测试条件；当前使用入口见[文档导航](../../README.md)。

## 记录索引
- [相对原版 MMD 的近期新增特性](#record-additional-features)
- [MMS 原生增强审计（截至 80bc305）](#record-mms-native-features-audit)

<a id="record-additional-features"></a>
## 相对原版 MMD 的近期新增特性

原始记录：`ADDITIONAL_FEATURES.md`。其中的版本、日期和验证范围沿用原记录。

本文面向使用者，汇总近期已合入主分支的新增能力与界面扩展。每项说明入口、默认行为、保存方式和限制，详细实现与验收证据通过链接查阅。

核对日期：2026-09-27。参照对象为原版 MikuMikuDance 9.32 x64。本文覆盖的 Git 范围为建库快照 `ba6b269`（2026-09-23 02:10）至 `80bc305`（2026-09-27 12:33），共 254 个提交。逐条的提交短哈希、源码位置和未核实项见[原生增强审计](features.md#record-mms-native-features-audit)。

建库快照已经包含部分自定义功能，因此不能把快照内所有内容都归为这几天新增。这些功能在建库快照里就已经存在，无法归属到本区间的提交，审计文档把它们单列为来源不明、待原版核验，本文在下面「建库快照已有的自定义功能」一节给出使用者可见的部分。

透明排序上限现为五档菜单，按档位编号 0 到 4 存一个字节，不与更早的六档测试格式做字节换算，当前档位语义见[三档强度下调](../rendering/transparency.md#record-transparent-sort-strengths)，存档规则见[PMM v2 扩展](../../reference/pmm-extensions.md)；六档格式的合入与验证范围作为历史记录保留在[主分支集成记录](../rendering/transparency.md#record-cloud-sort-main-integration)。

文档里的运行测量只对应各自注明的版本与条件。上述历史审计之外，原生骨骼工具、表情曲线与帧率修正已在 `3d1db3f` 合入 `main` 并覆盖构建；新增验收见[原生编辑器验收](../animation/editing.md#record-mms-native-editors-validation)。

<a id="record-additional-features-分类规则"></a>
### 分类规则

缺陷修复是否属于相对原版的新增特性，取决于缺陷来源。

- 改动修复原版 MMD 自身已有的缺陷时，属于 MMS 增强，计入新增特性。提交前缀写 `fix` 不改变这个结论。
- 改动把移植实现改回原版行为时，属于兼容修复，不计入新增特性。
- 安全性与稳定性修复不整体排除，按上面两条逐项判定。
- 判定需要原版证据，例如原版程序可复现的行为，或对原版可执行文件的反汇编结论。
- 缺原版证据时维持该条目的原有分类，并标注待核。不因为规则变化批量改判，也不把没有证据的条目一律算成新增或一律排除。
- 测试工具、未合入的实验分支与未提交的规划，都不计为已交付功能。

这条规则用于今后的判定，不重算历史条目的证据日期。

<a id="record-additional-features-原版缺陷修复案例"></a>
### 原版缺陷修复案例

按用户报告登记「隐藏表情双击崩溃」为原版缺陷修复案例，归入 MMS 增强。样本 `☆Rou.pmx` 的 `潰れ1` 面板值为 `0`，不出现在眉、目、口、其他四个操作栏，仍显示在时间轴左侧「表情」表示枠；用户报告在时间轴左侧表示枠双击该条目会使原版 MMD 崩溃。MMS 源码在选中时间轴表情条目后同步四个操作栏控件时已有面板范围保护，该保护在建库快照 `ba6b269` 就已存在，不属于该快照之后的新增提交。

2026-09-27 已完成真实样本后台验收：五个隐藏条目的单击、双击和 Shift 选择、四栏权重与关键帧登记、组合表情的顶点/UV/材质及动画求值、未知面板值边界均通过。本次新增回归测试与构建入口，生产逻辑保持已有实现。

分类依据、用户报告的复现步骤、模型事实、保护位置与验收结果见[时间轴隐藏表情双击崩溃](../animation/editing.md#record-hidden-morph-panel-safety)。

<a id="record-additional-features-功能一览"></a>
### 功能一览

| 新增特性 | 使用入口 | 当前默认行为 | 详细说明 |
| --- | --- | --- | --- |
| 时间轴隐藏表情安全处理 | 时间轴左侧「表情」表示枠 | 隐藏条目可安全单击、双击，保留组合与动画驱动，不出现在四个操作栏 | [样本与验收](../animation/editing.md#record-hidden-morph-panel-safety) |
| 原生骨骼缩放 | 视口移动轴下方的整体、X、Y、Z 按钮 | 所有骨骼可缩放；配件和相机禁用 | [骨骼缩放](../../guides/bone-scaling.md) |
| 骨骼缩放手柄与补间 | 骨骼面板「缩放」、左侧补间选择「缩放」 | 单轴方块和视图平面白框拖动，补间随 PMM/VMD 保存 | [骨骼缩放](../../guides/bone-scaling.md) |
| 表情曲线与三态对比 | 四个表情下拉框左侧的曲线入口 | 同组、自由、四组对比；双击设为活动曲线，单击显隐 | [曲线编辑器](../../guides/animation-curves.md) |
| 按帧率修正整个工程 | 编辑 → 根据帧率修正 | 帧 0 为缩放中心，原帧率和现帧率换算，合并报告及自动另存为 | [帧率修正](../../guides/frame-rate.md) |
| 刚体与关节继承骨骼缩放 | 无需额外入口，绑定骨骼的刚体自动继承 | 碰撞形状、相对偏移与关节连接点随之缩放 | [骨骼缩放](../../guides/bone-scaling.md) |
| 物理与关键帧烧录 | 烘焙 → 烧录关键帧… | Bullet 默认参数；支持三语说明、撤销和纯文本预设 | [物理烧录](../../guides/physics-baking.md) |
| 烘焙通道选择与稀疏采样 | 烧录窗口的「通道…」 | 可分别选择相机、灯光、自阴影、重力、附件子参数，以及当前模型的骨骼、表情、显示/IK | [通道烘焙验收](../animation/baking.md#record-baking-channels-validation) |
| 网格烘焙与 ABC 导出 | 烘焙 → 烘焙网格…、烘焙 → 导出 ABC… | 网格项默认只写工程缓存，ABC 项默认只导出文件 | [网格导出与烘焙](../../guides/mesh-export.md) |
| 原生曲线编辑器 | 附件操作区的曲线按钮 | 来源可选当前附件、摄像机、灯光、自阴影或重力 | [曲线编辑器](../../guides/animation-curves.md) |
| VMD 尾部扩展 | 骨骼缩放导出、曲线导出 | 标准段不变，扩展写在同一个文件末尾 | [PMM v2 项目扩展](../../reference/pmm-extensions.md) |
| 通用运算器设置（实施中） | 显示 → 运算器 | 主渲染、顶点计算、MMV 处理三个分项；本机用户配置，下次启动生效 | [已批准方案与实施状态](../performance/devices.md#record-processor-settings-proposal) |
| 顶点自动选择 | 运算器 → 顶点计算设备 | 保留逐模型正确性校验与完整顶点阶段耗时比较 | [自动选择原理与历史验证](../performance/devices.md#record-skinning-autoselect) |
| 透明排序上限 | 显示 → 透明排序上限 | 新建工程默认标准；打开工程可恢复已保存的档位 | [五档上限与 PMM 保存](../rendering/transparency.md#record-transparent-sort-five-tiers) |
| PMM v2 保存新增项目设置 | 正常保存、另存和打开 PMM | 保存透明排序上限和骨骼缩放，上限包含关闭；运算器独立保存 | [PMM v2 项目扩展](../../reference/pmm-extensions.md) |
| 宿主对可选 MMV 插件的加载与交接 | MMV 菜单（安装插件后出现在 MME 菜单左侧） | 未安装插件时不显示菜单，宿主仍用原有 D3D9 绘制 | [MMV 实现记录](../extensions/host.md#record-mmv-implementation) |
| 视口操作按钮外观与配件标识 | 视口旋转、移动按钮 | 配件坐标状态的六个按钮显示 `.x` 角标 | [配件角标](../interface/controls.md#record-accessory-hud) |
| 视口光标图标统一 | 视口旋转环、移动箭头 | 旋转环 70% 不透明度，移动原点为方块 | [光标图标](../interface/controls.md#record-viewport-cursor-style) |

<a id="record-additional-features-原生骨骼缩放"></a>
### 原生骨骼缩放

所有 PMD、PMX 骨骼都支持等比和分轴缩放，包括末端、IK 与物理骨骼。入口是视口右下角移动按钮下方新增的一排四个按钮：左侧为整体，右侧为 XYZ；局部、全局、视图选择器决定分轴方向。按住按钮向上拖动放大，向下拖动缩小，Ctrl 精调，Shift 加速，默认每向上拖动 100 像素约为原来的 2.718 倍。配件、相机、灯光与没有有效骨骼选择时按钮不响应拖动，播放期间不能编辑。

缩放与现有骨骼关键帧一起登记，支持撤销、复制粘贴和镜像。两个关键帧之间对伸缩部分做线性插值，对分解出的旋转部分做最短路径四元数插值。PMM 保存完整变换；VMD 的标准前缀保持原格式，缩放扩展写在同一文件尾部。原版 MMD 只读取标准动作，不表现骨骼缩放。绑定骨骼的刚体与关节会继承缩放，包括碰撞形状、相对骨骼的偏移和关节两端连接点；IK 仍按原有链长求解，不根据缩放重新计算链长。操作和兼容规则见[完整说明](../../guides/bone-scaling.md)。

骨骼面板已合并选择与框选为非选择、选择、框选三态按钮，第二行依次为移动、旋转、缩放。进入缩放模式后，视口显示单轴方块和双轴白框手柄。左侧补间选择支持缩放曲线；关键帧先按该曲线计算缓动进度，再执行原有缩放变换插值。

模型编辑器中的物理骨骼列表按当前界面语言选择骨骼名称：英文界面优先英文名，中文和日文界面优先模型主名称；若首选名称为空，则回退到模型里已有的另一种名称。关联骨骼列表中的“无”选项也随界面语言显示。

<a id="record-additional-features-物理与关键帧烧录"></a>
### 物理与关键帧烧录

选择当前模型并停止播放或录制后，打开「烘焙 → 烧录关键帧…」。程序从第 0 帧在独立物理世界中顺序计算，把指定范围的物理骨骼运动记录为原生关键帧。可以限制为选中的骨骼，显示进度并取消；一次烧录对应一次撤销，撤销与重做包含关键帧、物理禁用标记和模型末帧。结果随 PMM 保存，也可导出 VMD。

高级设置支持 ERP、CFM、物理子步数、内部计算倍率和基准频率，初始值采用 Bullet 默认方案；截图示例参数不作为默认值。全部参数可保存、读取非加密 UTF-8 JSON 预设，预设不写入 PMM 扩展。中文、英文、日文界面均有参数说明。范围边界会按需补充物理状态恢复帧；具体行为和限制见[物理烧录说明](../../guides/physics-baking.md)。

烧录窗口的「通道…」可以选择要重采样的轨道。相机与灯光页覆盖摄像机、灯光、自阴影、重力和每个 `.x` 附件的子参数；模型页只列当前模型，可逐骨骼、表情、显示/IK 选择。起止帧与步长控制采样，总会包含结束帧；骨骼稀疏烘焙替换区间内的旧键，区间外的曲线保留。骨骼关键帧池按需扩容并保留槽位与原文件格式；表情、显示及部分场景轨道仍有既有容量，超限会拒绝提交。验收范围见[通道烘焙验收](../animation/baking.md#record-baking-channels-validation)。

烧录生成的关键帧带原生「禁用物理」标记，回放时使用记录的骨骼运动，而不是重新求解物理。绑定骨骼的刚体形状、相对偏移和关节连接点随缩放一起参与逐子步求值；物理烧录保存求解后的缩放，普通重采样保存原曲线缩放。原版 MMD 只读取位置、旋转等标准动作。

<a id="record-additional-features-运算器"></a>
### 运算器

用户已批准将「显示 → 蒙皮运算器」改为「显示 → 运算器」，下设三个分项。实现与整体验收进行中，进度见[通用运算器方案](../performance/devices.md#record-processor-settings-proposal)。

| 分项 | 作用 | 默认选择 |
| --- | --- | --- |
| 主渲染设备 | 主画面、阴影、描边、透明绘制和最终 MME 输出 | 自动选择可用的高性能显卡 |
| 顶点计算设备 | 原生模型顶点变换 | 自动比较 CPU 与可用 GPU |
| MMV 处理设备 | MMV 计算、纹理与中间绘制，共用一个设备 | 跟随本次启动的实际主渲染设备 |

主渲染可选自动或具体显卡；顶点计算可选自动、CPU 或具体显卡；MMV 可选跟随主渲染、CPU 或具体显卡。MMV 的 CPU 选项对应 DX12 WARP 和兼容路径的 DX9 REF 软件设备。该选项不会把 Bullet3 等原生 CPU 算法变为 GPU 算法。

三项按当前程序目录保存在 `data/processor.ini`。修改只更新待生效配置，全部在下次启动生效；菜单区分本次运行设备和下次启动选择。新建、打开和保存工程不改变这些设置，PMM、VMM、VEM 不保存它们。

顶点自动选择保留逐模型正确性校验及完整阶段耗时比较，包括输入打包、上传、计算、同步回读和写入 D3D9 缓冲。计时只代表顶点阶段，不能据此判断整帧渲染性能。当前设备状态表示最近完成顶点处理的模型，多个模型可以采用不同设备。QDEF、未知权重或不满足 GPU 条件的模型沿用 CPU 分支。既有数学与测速验证见[自动选择原理](../performance/devices.md#record-skinning-autoselect)。

<a id="record-additional-features-透明排序上限"></a>
### 透明排序上限

五档实现与后台双 build 验证见[五档排序记录](../rendering/transparency.md#record-transparent-sort-five-tiers)。原六档实现的合入记录保留为历史。

「显示 → 透明排序上限」提供关闭、低、标准（默认）、高、最高五个互斥选项。程序内部只保留一个档位值，没有单独的开关；关闭本身就是档位，选择关闭会覆盖上次选择的档位，不保留旧选择。旧命令 303 只是宏兼容入口，在关闭和标准之间切换，不恢复历史值，也没有对应的菜单项。

选中一个非关闭档位后，程序对半透明表面重新排序，以改善前后混合顺序。低、标准和高三档只重排整个三角形，不分割几何，也不做逐像素分层。低档用 16 个深度桶，标准档用 64 个深度桶，均从远到近排列，同桶保持提交顺序。高档按三角形中心深度稳定排序，同深度按原始三角形序号。三档都绘制全部有效面，差别在排序精度。只有最高档在适用的内置路径上启用逐像素透明分层。选择关闭会跳过排序，并在随后绘制时保留模型原始提交顺序。排序以外的处理不受影响：程序仍然使用不透明像素预通道、描边及阴影处理，所以关闭排序不等于恢复原版渲染器的全部行为。所选档位随 PMM 保存。

<a id="record-additional-features-档位与分层上限"></a>
#### 档位与分层上限

| 档位 | 行为与上限 |
| --- | --- |
| 低 | 16 桶计数排序，远到近，同桶保持提交顺序；不分层，不分割。 |
| 标准（默认） | 64 桶计数排序，远到近，同桶保持提交顺序；不分层，不分割。 |
| 高 | 按三角形中心深度稳定排序；不分层，不分割。 |
| 最高 | 逐像素分层，上限为当前参与排序且裁剪后的三角形数量加一。 |
| 关闭 | 不做自动透明排序。 |

分层上限不等于必定执行的次数。分层复用已有的遮挡查询，片段耗尽即提前结束；最高档也不做计数预扫描。最高值按「一个三角形在一个像素最多贡献一层」推导，多出的一次用于空查询确认完成，属于安全上界，不是预先算出的最小层数，因此最高档没有单独的数字上限。最高档的分层必须完整完成后才提交，遇到设备或效果错误会丢弃私有中间结果并回退完整几何。

<a id="record-additional-features-分层不可用时的回退"></a>
#### 分层不可用时的回退

最高档只在支持的内置 SM3 分层路径上动态推进。使用自定义 MME shader、SM2、固定管线或资源不足时，程序改用 BSP 近似排序，预算保持额外 4,000,000 个三角形、深度 512、epsilon 2e-13。其余三档始终使用完整三角形的近似顺序。

MSAA 掩码按像素保留覆盖比例，不逐个保存子采样的完整透明链；部分遮挡边界、配件 `.x` 的独立通道与模型之间也没有统一的全局排序保证。相交几何的深度近似解决不了所有循环遮挡。该功能不能保证所有面部、头发与跨模型透明问题都消失。

关闭排序只跳过透明排序本身，不会去掉不透明像素预通道、描边及阴影处理，也不接管第三方 MME 效果的绘制顺序。排序可能增加 CPU 和绘制成本。以上限制只针对透明排序阶段，不是模型可加载的大小上限；全量排序实验仍处于停用状态。详见[五档上限与 PMM 保存](../rendering/transparency.md#record-transparent-sort-five-tiers)、[浮点合成与目标 alpha 修复](../rendering/transparency.md#record-cloud-float-compositing)、[有界排序](../rendering/transparency.md#record-transparent-layer-ordering)与[全量实验的状态记录](../rendering/transparency.md#record-transparency-global-sort)。

<a id="record-additional-features-pmm-v2-项目设置扩展"></a>
### PMM v2 项目设置扩展

正常保存或另存 PMM 时，新增设置自动随工程保存，无需额外导出配置。目前包括：

- 透明排序上限：关闭、低、标准、高或最高，只保存一个档位字段；更早的旧开关标识不作为设置使用。
- 骨骼缩放：每个模型槽保存当前姿态与缩放关键帧的完整 3×3 矩阵，单位变换省略。

运算器独立保存在本机用户配置中。旧 PMM 运算器字段在加载时忽略，重新保存时移除。新建工程以及打开没有扩展的 PMM v1/v2 时，保持本机运算器设置，恢复标准排序上限和单位骨骼缩放。本程序不与更早的排序格式做字节换算：早期六档测试格式写出的同一字段按五档语义读数，旧开关标识只作为普通未知记录原样写回；用户未编辑排序字段时，未知取值的原始字节也不改写。字段字节值与边界见[五档上限与 PMM 保存](../rendering/transparency.md#record-transparent-sort-five-tiers)与[PMM v2 扩展格式](../../reference/pmm-extensions.md)。

文件仍使用标准 PMM v2 文件头，扩展位于完整标准数据之后。未知且有效的扩展记录会在本程序再次保存时保留，后续功能无需逐项增加 PMM 版本号。

原版 MMD 9.32 x64 忽略附加数据的行为已通过静态反汇编核对；这不代表所有第三方读取器均已验证。**用原版 MMD 再次保存会丢失这些扩展设置。** 文件格式、异常处理和验证范围见 [PMM v2 扩展说明](../../reference/pmm-extensions.md)。

<a id="record-additional-features-视口操作按钮与配件状态标识"></a>
### 视口操作按钮与配件状态标识

视口的六个旋转、移动按钮调整了箭头填色。在配件坐标状态下，按钮以 `.x` 角标标识配件操作；角标完整显示，周围保持透明背景。相机状态仍使用相机标识。

这是操作对象的视觉提示，沿用现有坐标状态、按钮命中区域与操作功能，没有增加新的配件坐标模式。最终外观与渲染目标验证见[配件角标记录](../interface/controls.md#record-accessory-hud)。

<a id="record-additional-features-曲线编辑器"></a>
### 曲线编辑器

附件操作区的曲线按钮打开一个原生非模态浮窗。顶部「来源」可以选择当前附件、摄像机、灯光、自阴影或重力。MMV 插件的材质分配页与骨骼分配页在参数区提供同一个曲线编辑入口，但插件自身的界面与存档不属于本文范围。

增加、删除、移动关键帧，以及切线类型、贝塞尔手柄、数值曲线与速度曲线的切换方式见[曲线编辑器](../../guides/animation-curves.md)。浮窗内的撤销和重做以当前来源为范围；原窗口改了轨道后，重新激活浮窗会读取新数据。

导出 VMD 时先写完标准骨骼、表情、相机、灯光、自阴影及显示/IK 段，再追加 `MMVCURV1` 曲线扩展。旧读取器仍能读取自己支持的标准段。附件变换写成 `Accessory` 骨骼轨道，其他参数写为表情代理；完整名称与曲线保存在扩展中。没有扩展的多骨骼 VMD 不会自动猜测附件目标，会报告无法选择唯一轨道。

宿主主页与附件曲线写入 PMM 扩展，MMV 参数关键帧和曲线写入 `.vmm`。已合入 `main` 的入口不包含模型表情曲线；模型表情的曲线对比显示在当前未提交的工作树里，见下面「待集成」一节。验证范围见[曲线与 MMV 窗口验收](../extensions/interface.md#record-mmv-curve-ui-validation)。

<a id="record-additional-features-相机照明-vmd-中的-x-附件动作"></a>
#### 相机・照明 VMD 中的 `.x` 附件动作

在相机・照明・附件面板保存动作时，除选中的相机、灯光、自阴影关键帧外，所有 `.x` 附件中选中的关键帧也一起写入同一个 VMD。只选了附件关键帧也可以保存。标准段照原格式写完，附件数据追加在原结束位置之后，原版 MMD 和其他只读标准段的工具不受影响。附件区原先的「读取」按钮已移除。

附件块以 `MMSXACC1` 开头，随后是版本号、长度，以及每个附件的名称和关键帧（帧号、显示、阴影、父模型、父骨骼、位置、旋转、缩放、透明度），最后是 CRC-32。帧号与相机关键帧一样按保存时的最小帧重新计数。格式细节见 [accessory_motion_io.hpp](../../../include/mikudancestudio/accessory_motion_io.hpp)。

在同一面板读取这类 VMD 时，附件动作按名称分配给场景中的 `.x`，与相机关键帧一样贴到当前帧之后：

- 一个或多个附件都可以；按附件下拉框的顺序匹配。
- 同名的多条动作依次交给同名的附件：第一条给第一个，第二条给第二个。
- 场景中同名附件不够时，多出的动作不会新增附件，直接忽略。
- 场景中同名附件更多时，排在前面的附件得到动作，多出的附件不接收动作。
- 父模型按保存场景的模型槽记录；读取场景中该槽没有模型时，改为不绑定。

附件块损坏或版本不符时整块忽略，相机、灯光和自阴影照常读入。VPD 目前不含附件。

<a id="record-additional-features-网格烘焙与-abc-导出"></a>
### 网格烘焙与 ABC 导出

「烘焙 → 烘焙网格…」默认只把结果写进工程缓存，「烘焙 → 导出 ABC…」默认只导出文件，两项也可以同时勾选。采样的是 MMS 与 MMV 交接处、MME 着色之前的几何；帧范围 0 到 100000，按 MMD 的 30 fps 时间轴解释，采样率 1 到 120，默认 30。操作前要停止播放和录制。

项目缓存把 PMM 里的相对路径映射和旁边的 `<工程名>.<内容标识>.mdsmesh.abc` 文件配对保存；VMM 保存 FX 开关键帧。移动工程时要一起保留 PMM、EMM、VMM、缓存和源素材。清除用「烘焙 → 清除网格烧录…」，可以只清当前模型或全部对象；清除恢复源动画、保留 FX 启停关键帧，不删除已经保存的缓存文件。

ABC 使用 Ogawa 格式，右手系 Y 向上、Z 相对 MMD 反向，UV 的 V 翻转，单位沿用 MMD。原生捕获保留顶点法线，传统绘制捕获重算法线；ABC 不含骨骼、动画曲线或 FX，相机相关变形固定在采样镜头下。原版 MMD 忽略本工程的缓存扩展。旧版没有区间标记的缓存仍按旧规则读取。

已知限制：屏幕后期、自定义投影、依赖历史渲染目标的效果和非标准绘制有明确限制；纯 ABC 输出不支持局部顶点过滤，界面会要求改用工程烘焙；当前 MMV 网格接口没有反向写回骨骼姿态的能力，MMV 物理只能以最终网格烘焙。完整说明见[网格导出与烘焙](../../guides/mesh-export.md)，验证范围见[ABC 导出与网格烘焙验收](../animation/baking.md#record-alembic-export-validation)。

<a id="record-additional-features-宿主对可选-mmv-插件的加载与交接"></a>
### 宿主对可选 MMV 插件的加载与交接

MMV 是可选插件，不安装时宿主行为与原来一致，也不显示 MMV 菜单。插件自身的效果编译、参数面板、透视窗口和 `.uifx`、`.vmm`、`.vem` 存档属于插件，本文不把它们算作 MMS 原生功能。

宿主侧提供这些部分。菜单在插件安装后出现在右上角 MME 菜单左侧，随宿主切换中文、日文、英文；宿主在蒙皮之后、主设备 `BeginScene` 之前取得快照，把插件结果复制到独立渲染记录，模型和附件的阴影、描边、普通绘制、透明排序及 MME 查询都经过这条路径。宿主保持原有的 37 个导出名称与序号不变，不读取或改写 MMHack 与 MMEffect 的私有数据。网格烘焙与 ABC 导出复用同一条交接路径，可以采样 MMS 与 MMV 交接处的几何。

可选的 MMV Bullet3 物理后端也通过宿主服务接入。计算 DLL 与适配器在独立仓库 `D:/MMD-Next/MMV-Bullet3`，本仓库交付的是宿主侧的可选服务与菜单配合：`physicsInput` 返回只读的形变后、蒙皮前顶点与刚体信息，`selectBullet3` 通过现有菜单关闭原生物理并在释放时恢复。宿主继续拥有原生物理世界。Bullet3 视频导出、原生 VMD 物理烧录和第三方效果组合尚未验证，见[Bullet3 接入](../extensions/physics.md#record-mmv-bullet3)与[集成验收](../extensions/host.md#record-mmv-bullet3-main-integration)。

插件的安装位置、两版 DLL 的区别与分配操作见[安装与分配窗口](../extensions/usage.md#record-mmv-install)和[实现与验收范围](../extensions/host.md#record-mmv-implementation)。

<a id="record-additional-features-建库快照已有的自定义功能来源不明待原版核验"></a>
### 建库快照已有的自定义功能（来源不明，待原版核验）

下面几项存在于建库快照 `ba6b269`，无法归属到本文 Git 范围内的 254 个提交。它们相对原版 MMD 9.32 确实属于扩展，但引入时间和原始出处没有记录，也缺少原版核验证据。审计文档把它们单列，判定为来源不明、待核验。

- 简体中文界面。原版只有日文与英文两种界面语言；本程序在原有 `EnglishUI` 字节上增加第三个取值 `2`。语言切换命令 260 按日文 → 英文 → 中文 → 日文循环，启动时无存档值则按 Windows 界面语言选择。
- Unicode 文件名与路径。主窗口使用 Unicode 窗口类和消息处理，PMM 的模型、配件、音频与背景路径按多种代码页回退；无法用旧代码页完整表示的路径改用带 BOM 的 UTF-8 字段。该字段是本程序扩展，旧版 MMD 与第三方插件不保证读取，需要与旧版交换工程时应使用其代码页能够表示的素材路径。详见[文件名编码](../formats/projects.md#record-filename-encoding)。
- 原生 Win64 构建。构建脚本把 `MikuMikuDanceE_v932x64` 记为 x64 参考程序；本程序按 8 字节指针环境构建，寻址布局集中在 `layout_pins.hpp`。
- FPS 覆盖显示。默认关闭的一个覆盖层，建库快照里已有开关和绘制代码，是否有原版对应显示未核实。
- NVIDIA 3D Vision 立体桥、Kinect 运动捕捉、AVI 与 DirectShow 录制、MMdxShow 源滤镜。这四组都在建库快照里，本文 Git 范围内没有新增宿主可见能力，也没有运行验收记录。

<a id="record-additional-features-2026-09-27-原生编辑器补充"></a>
### 原生编辑器补充

以下功能已在 `3d1db3f` 合入 `main`：

- 面板选择与框选的合并按钮：在非选择、选择、框选三种状态之间循环。
- 模型面板的「拡大縮小」缩放模式按钮。
- 进入缩放模式后视口在选中骨骼处绘制的缩放手柄，包括视图平面白框与单轴方块两条拖动路径。
- 与手柄配套的视图平面拖动增量。
- 四个表情组的原生曲线入口和对比显示：同组变化、自由显示、四组对比三个直接选择模式。活动曲线继承打开入口的当前表情；单击列表切换显隐，双击设为活动曲线。另可筛选选定时间段内有变化的曲线。
- 按原帧率与现帧率修正工程。无帧合并时保留原生骨骼和相机补间字节；帧合并时裁切外侧曲线并报告取整与省略的键。原生 `.x` 和表情曲线不依赖 MMV。

13 组相关测试及真实隐藏宿主操作通过。界面使用 `WM_PRINT` 回读，三维画面使用 DX9 回读；同条件 MME 前后三张画面的 RGB 逐像素一致。原始证据及性能采样的限制见[验收记录](../animation/editing.md#record-mms-native-editors-validation)。MMV 新分层架构、运行时帧率事务和全局“应用”仍由协作会话整合，全局撤销列在[待实现计划](../extensions/interface.md#record-mmv-editor-undo-plan)。

<a id="record-additional-features-同期默认值与文案调整"></a>
### 同期默认值与文案调整

以下改动影响使用体验，但不构成原版不存在的独立功能。

| 调整 | 当前行为 | 说明 |
| --- | --- | --- |
| 「美影」默认开启 | 启动使用 4096×4096 阴影贴图；取消勾选恢复 2048×2048 | 原有阴影精度选项的默认值调整，自阴影模式仍由原控件设置。见[默认美影](../rendering/transparency.md#record-auto-transparent-sorting)。 |
| 「运算器（蒙皮）」改为「蒙皮运算器」 | 自动项为「自动（优先高性能）：当前设备」 | 与逐模型实测选择的行为一致。 |
| 「节省CPU功耗」改为「节省内存模式」 | 显示菜单使用新中文文案 | 与主界面下述复选框仍共用原有空闲暂停刷新开关。 |
| 「省电」改为「省性能」 | 主界面复选框使用新中文文案 | 后两项只是改名，没有增加内存压缩、内存释放或降低画质的机制。 |

<a id="record-additional-features-不计入已交付能力的项目"></a>
### 不计入已交付能力的项目

- 鼠标越过视口边缘时重新居中并继续拖动：`70a5c15`、`3033b6c` 恢复原版操作按钮已有的行为，尽管提交前缀为 `feat`，仍应归入兼容修复。
- 保存 PMM 时通知 MME 自动保存同名 EMM：`ad7058e` 修复原有保存通知链路，见 [EMM 自动保存](../effects/compatibility.md#record-emm-autosave)。
- 录制回读的水平采样相位与非法尺寸：`8b6b0aa`、`6a1843f` 修复移植实现自身的缺陷，修改前的基线第二行即失败，见[代码审查记录](audits.md#record-rendering-review)。录制与输出在本 Git 范围内没有新增宿主可见能力。
- MME toon 纹理改走宿主导入槽：`9facf26` 让 MME 能注册 toon 纹理，修复移植侧用 `LoadLibraryA` 加 `GetProcAddress` 绕过导入地址表的写法，见[黑草问题记录](../rendering/materials.md#record-mme-toon-black)。
- 视点保持、坐标选择器双次推进、相机参考模式补偿符号、场景灯光编辑框、PMX 顶点布局等：这一批都是把移植实现改回原版行为，提交包括 `116e3aa`、`1c142c6`、`bb6a9b3`、`96ce788`、`eb1ae97`、`c485cfb`、`bede0b8`、`4c11fa5`、`d03c23b`、`45ae9b2`、`57f68ff`、`2284dc5`、`e3f291e`、`90f6b67`。
- 文件加载器越界、畸形输入、非法 PMX 关节、非有限骨骼姿态、文本长度与资源生命周期处理：属于安全性和稳定性修复。按分类规则，这类改动要逐条确认缺陷来自原版还是来自移植实现。本 Git 范围内没有为每一条补原版证据，因此维持原分类并标注待核，不整体改判。见[物理安全修复](../animation/physics.md#record-physics-joint-pose-safety)及[代码审查记录](audits.md#record-full-review)。
- 实验性 D3D12 预览：建库快照已经包含全部 D3D12 源文件，不能认作本文 Git 范围内才增加，也不能与正式 D3D9/MME GPU 蒙皮混用结论。本范围内只有 `7523928` 把计算结果接进正式蒙皮。启动方式与尚缺少的功能见 [D3D12 迁移记录](../performance/devices.md#record-d3d12-migration)。
- 全量透明排序与 BSP 云排序实验：停用或失败保留，只作为最高档回退预算的来源，不作为当前功能。见[全量实验状态](../rendering/transparency.md#record-transparency-global-sort)。
- 未提交的功能规划、停用实验分支与验收脚本：不作为当前程序已提供的用户功能。

<a id="record-additional-features-git-与源码索引"></a>
### Git 与源码索引

下表列出便于复核的主要提交；分类以最终行为和源码为准，不仅依据提交前缀。

| 范围 | 主要提交 | 主要源码 |
| --- | --- | --- |
| 原生骨骼缩放 | `ad032d9` | [bone_scale.cpp](../../../src/model/bone_scale.cpp)、[bone_scale_io.cpp](../../../src/io/bone_scale_io.cpp)、[bone_scale.hpp](../../../include/mikudancestudio/bone_scale.hpp) |
| 缩放随烧录保存、刚体与关节继承 | `3eedd88`、`d214cd4` | [physics_scale.cpp](../../../src/physics/physics_scale.cpp)、[channel_bake.cpp](../../../src/animation/channel_bake.cpp) |
| 物理烧录、通道选择与动态骨骼键池 | `51e2f64`、`df9f00a`、`80bc305` | [physics_bake.cpp](../../../src/physics/physics_bake.cpp)、[physics_bake_dialog.cpp](../../../src/window/physics_bake_dialog.cpp)、[channel_bake.cpp](../../../src/animation/channel_bake.cpp)、[bone_key_pool.cpp](../../../src/model/bone_key_pool.cpp) |
| 网格烘焙与 ABC 导出 | `54bfe14` | [mesh_bake_cache.cpp](../../../src/io/mesh_bake_cache.cpp)、[alembic_export.cpp](../../../src/io/alembic_export.cpp)、[vertex_capture.cpp](../../../src/render/vertex_capture.cpp)、[alembic_export_dialog.cpp](../../../src/window/alembic_export_dialog.cpp) |
| 曲线编辑器与 VMD 尾部扩展 | `f576e96`、`a017903`、`7d783ca` | [animation_curve_editor.cpp](../../../src/window/animation_curve_editor.cpp)、[animation_curve_motion.cpp](../../../src/animation/animation_curve_motion.cpp)、[animation_curve_host_ui.cpp](../../../src/window/animation_curve_host_ui.cpp) |
| 宿主 MMV 交接与 Bullet3 宿主服务 | `b1de95c`、`8506356`、`b2b5e79` | host.cpp（`src/mmv_host/host.cpp`，历史引用文件不在当前仓库中）、mmv.h（`include/mmv/mmv.h`，历史引用文件不在当前仓库中）、[effect_api.cpp](../../../src/exports/effect_api.cpp) |
| 正式 GPU 蒙皮与菜单 | `7523928` | [skinning_backend.cpp](../../../src/model/skinning_backend.cpp)、[skinning_menu.cpp](../../../src/window/skinning_menu.cpp) |
| 逐模型实测与设备名称 | `d3f2e59`、`cf09d31` | [skinning_backend.cpp](../../../src/model/skinning_backend.cpp)、[skinning_menu.cpp](../../../src/window/skinning_menu.cpp) |
| 有界透明排序、开关与默认美影 | `60595e1`、`ff2e77b`、`f7c6cb0`、`b887694` | [model_renderers.cpp](../../../src/render/model_renderers.cpp)、[command_view_menu.cpp](../../../src/window/command_view_menu.cpp)、[ui_init.cpp](../../../src/window/ui_init.cpp) |
| 六档排序上限（早期格式）与单字段 PMM 保存（已合入本地 `main`） | `211d740`、`508aed5`、`57c177c` | [transparent_peeling.cpp](../../../src/render/transparent_peeling.cpp)、[model_renderers.cpp](../../../src/render/model_renderers.cpp)、[enhance_model_io.cpp](../../../src/window/enhance_model_io.cpp)、[pmm_project_settings.cpp](../../../src/io/pmm_project_settings.cpp) |
| 五档排序上限与测试版 PMM 直接读写（已合入本地 `main`） | `1e54b05` | [五档验证记录](../rendering/transparency.md#record-transparent-sort-five-tiers)、[transparent_geometry.cpp](../../../src/render/transparent_geometry.cpp)、[pmm_project_settings.cpp](../../../src/io/pmm_project_settings.cpp) |
| PMM v2 扩展容器 | `58f4ea1` | [pmm_extension.cpp](../../../src/io/pmm_extension.cpp)、[pmm_project_settings.cpp](../../../src/io/pmm_project_settings.cpp) |
| 操作按钮与完整 `.x` 角标 | `b48ade1`、`dbe5c6b` | [sprite_overlay.cpp](../../../src/render/sprite_overlay.cpp)、[HUD 图集](../../../res/assets/hud_sprites.png) |
| 视口光标图标统一 | `e27972b` | [viewport_cursor_style.cpp](../../../src/render/viewport_cursor_style.cpp) |
| 中文名称修正 | `4ecc7ad` | [文案与行为记录](../performance/devices.md#record-skinning-autoselect) |
| 原版隐藏表情崩溃的越界保护 | 建库快照 `ba6b269`，不是新增提交 | [ui_editor_click.cpp](../../../src/window/ui_editor_click.cpp)、[wndproc.cpp](../../../src/window/wndproc.cpp)、[ui_mouse_misc.cpp](../../../src/window/ui_mouse_misc.cpp) |

可在仓库根目录复核本文范围：

```powershell
git log --reverse --oneline ba6b269..80bc305
git diff --stat ba6b269 80bc305 -- src include res docs tests
git rev-list --count ba6b269..80bc305
```

原汇总阶段只增加文档，未重新构建或运行测试。2026-09-27 透明排序集成另完成正式构建、16 项后台回归及两版云图对照，范围见[集成记录](../rendering/transparency.md#record-cloud-sort-main-integration)。隐藏表情案例已于 2026-09-27 补齐真实窗口消息、普通四栏和组合动画的后台回归，验收范围见[时间轴隐藏表情双击崩溃](../animation/editing.md#record-hidden-morph-panel-safety)。各专题文档中的测量只对应其注明的版本及条件，不代表当前全部功能已经重新验收。视口光标图标统一的记录写明只有模块级验证，整包集成与真实场景验收当时未完成，汇总在[原生增强审计](features.md#record-mms-native-features-audit)的未核实项一节。

<a id="record-mms-native-features-audit"></a>
## MMS 原生增强审计（截至 80bc305）

原始记录：`MMS_NATIVE_FEATURES_AUDIT_2026-09-27.md`。其中的版本、日期和验证范围沿用原记录。

本文是 [近期新增特性](features.md#record-additional-features) 的证据底稿。汇总文档面向使用者说明入口、默认值、保存方式和兼容性；本文记录每一条结论的 Git 提交短哈希、源码位置、判定状态和未核实项，供复核使用。

核对日期：2026-09-27。

补查至 `7f28800`：`80bc305..7f28800` 仅有 `375906f`（测试夹具效果范围往返）与 `7f28800`（MMV 分配说明）两个提交，`src`、`include` 没有新增差异。下文原生能力审计范围仍以 `80bc305` 标识；当前未提交的骨骼／表情编辑器和帧率修正不计入已交付清单。

<a id="record-mms-native-features-audit-审计范围与复核命令"></a>
### 审计范围与复核命令

审计的分支为本地 `main`，建库快照为 `ba6b269`，截止提交为 `80bc305`。

```powershell
git show -s --format="%H %ad %s" ba6b269
git show -s --format="%H %ad %s" 80bc305
git rev-list --count ba6b269..80bc305
git log --oneline --reverse ba6b269..80bc305
git diff --stat ba6b269 80bc305 -- src include res docs tests
```

已核对的数字：

| 项目 | 值 |
| --- | --- |
| 建库快照 | `ba6b269548323cf9cd8e837f91b3e420e56140cb`（2026-09-23 02:10:20 +0800） |
| 截止提交 | `80bc305a2f29dcd6dd9fa8c014cc0c0bfe4b4b98`（2026-09-27 12:33:20 +0800） |
| 区间提交数 | 254 |
| `src` 与 `include` 变更文件数 | 214 |
| `src` 与 `include` 净变更 | 50640 行新增、1652 行删除 |

判定正文说法时以当前源码（`80bc305`）为准，不以提交标题为准。凡提交标题所述内容在源码中找不到对应实现，本文在该条目的状态列写明。

<a id="record-mms-native-features-audit-判定口径"></a>
### 判定口径

每条改动归入下面七类之一。同一个提交只按最终行为归类，不按提交前缀归类。

| 类别 | 含义 |
| --- | --- |
| 已交付增强 | 相对原版 MMD 9.32 新增，且已合入 `main` 的宿主可见能力。 |
| 建库快照已有 | 功能存在于 `ba6b269`，无法归属到本区间 254 个提交。原版核验证据不足，标为来源不明。 |
| 纯移植兼容修复 | 把移植实现改回原版 MMD 行为，或修复移植实现自身的缺陷。不计入增强。 |
| 未合入或已停用 | 实验分支、停用路径、未提交规划。不计为已交付。 |
| 插件自有能力 | 属于可选 MMV 插件自身，不计入 MMS 原生；宿主侧的加载与交接单独列出。 |
| 工作树待集成 | 存在于当前未提交工作树，尚未进入 `main`。不计为已交付。 |
| 未核实 | 缺少原版证据或缺少运行验收，无法给出最终判定。 |

<a id="record-mms-native-features-audit-一已交付增强平台语言与界面"></a>
### 一、已交付增强：平台、语言与界面

<a id="record-mms-native-features-audit-原生-win64-构建"></a>
#### 原生 Win64 构建

性质是平台移植属性。构建脚本把 `MikuMikuDanceE_v932x64` 记为 x64 参考程序（`CMakeLists.txt:7`）。寻址布局集中在 `include/mikudancestudio/layout_pins.hpp`。

| 项目 | 内容 |
| --- | --- |
| 入口 | 无用户入口。构建产物为 x64 程序。 |
| 默认行为 | 64 位指针宽度。 |
| 证据 | `CMakeLists.txt:42-44`（`CMAKE_SIZEOF_VOID_P EQUAL 8` 分支）、`include/mikudancestudio/layout_pins.hpp`、`CMakeLists.txt:334-342`（x64 导入库处理）。 |
| 引入提交 | 建库快照 `ba6b269` 已存在。 |
| 状态 | 建库快照已有。 |

<a id="record-mms-native-features-audit-简体中文界面第三种界面语言"></a>
#### 简体中文界面（第三种界面语言）

原版 MMD 9.32 只有日文与英文两种界面语言。本程序在原有 `EnglishUI` 字节上增加第三个取值 `2` 表示简体中文。

| 项目 | 内容 |
| --- | --- |
| 入口 | 语言切换命令 260，循环日文 → 英文 → 中文 → 日文。 |
| 默认行为 | 无存档值时按 Windows 界面语言选择：主语言 `0x04` 为中文，`0x11` 为日文，其余为英文。 |
| 保存方式 | 界面语言字节随项目设置保存。 |
| 数据来源 | 固定词表见 `src/window/ui_translation.cpp` 与 `src/window/localize_ui.cpp`；模型名称按语言取原名或英文名。 |
| 证据 | `include/mikudancestudio/ui_language.hpp`（`kUiChinese = 2`、`ResolveStartupUiLanguage`、`UiUsesEnglishNames`）、`src/window/command_view_menu.cpp:1854-1862`（命令 260 的 `(cur + 1) % 3`）、`src/window/window_init.cpp:229,252`、`src/window/ui_panel_sweep.cpp:78-79`（中文用 Microsoft YaHei UI）。 |
| 引入提交 | 建库快照 `ba6b269` 已包含 `ui_language.hpp`、`localize_ui.cpp` 的中文列与命令 260 的 `% 3` 循环。 |
| 状态 | 建库快照已有。 |

本区间对中文界面的改动只有文案修正，不构成新增语言：`4ecc7ad`（蒙皮与省电菜单中文名）、`ce5dd99`（坐标标识字体与描边）。

<a id="record-mms-native-features-audit-unicode-文件名与路径"></a>
#### Unicode 文件名与路径

原版 MMD 仅支持 ANSI/CP932 路径。本程序使用 Unicode 窗口类和消息处理，PMM 路径字段在保留原布局的前提下按多代码页回退，无法用旧代码页表示时写入带 BOM 的 UTF-8 字段。

| 项目 | 内容 |
| --- | --- |
| 入口 | 无独立入口，作用于文件打开/保存、配件名称与 PMX 名称显示。 |
| 默认行为 | 保留原字段布局；旧工程按 CP932、系统代码页、CP936、CP950、CP949 及 UTF-8 依次尝试。 |
| 兼容性 | 带 BOM 的 UTF-8 路径字段是本程序扩展，旧版 MMD 与第三方插件不保证读取。 |
| 证据 | `src/window/unicode_window.cpp`、`src/main/text_encoding.cpp`、`docs/records/formats/projects.md`。 |
| 引入提交 | 建库快照已有；本区间仅 `342bffe` 触及 `src/main/text_encoding.cpp`。 |
| 状态 | 建库快照已有。原版核验证据未在库内。 |

<a id="record-mms-native-features-audit-视口操作按钮配色与配件-x-角标"></a>
#### 视口操作按钮配色与配件 `.x` 角标

| 项目 | 内容 |
| --- | --- |
| 入口 | 视口旋转、移动按钮。 |
| 默认行为 | 配件坐标状态下六个按钮显示完整 `.x`，周围透明；相机状态仍为相机标识。 |
| 证据 | `src/render/sprite_overlay.cpp`、`res/assets/hud_sprites.png`、[配件角标记录](../interface/controls.md#record-accessory-hud)。 |
| 引入提交 | `b48ade1`、`3da02e4`、`65eaf27`、`33ec81b`、`83d833e`、`dbe5c6b`。 |
| 验证范围 | 六个角标各 37 个笔画像素、缺失 0；渲染目标回读，三种缩放、两种背景。未重做整场景性能验收。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-视口光标图标统一"></a>
#### 视口光标图标统一

| 项目 | 内容 |
| --- | --- |
| 入口 | 视口旋转环与移动箭头。 |
| 默认行为 | 旋转环 70% 不透明度；移动光标原点改为方块；三角箭头内部为单一纯色。 |
| 证据 | `src/render/viewport_cursor_style.cpp`、`src/window/ui_init.cpp`、`src/render/frame_scene.cpp`、[图标记录](../interface/controls.md#record-viewport-cursor-style)。 |
| 引入提交 | `e27972b`。 |
| 验证范围 | 模块级验证：图集左半逐像素差异为 0、圆环 alpha 179、四个移动状态无旧手形残留。 |
| 未核实 | 该功能的记录写明整包集成与真实 PMM/MME 场景验收当时未完成，只有模块级验证。见本文第九节。 |
| 状态 | 已交付增强（验收范围不完整）。 |

<a id="record-mms-native-features-audit-二已交付增强模型格式骨骼缩放与物理"></a>
### 二、已交付增强：模型格式、骨骼缩放与物理

<a id="record-mms-native-features-audit-原生骨骼缩放"></a>
#### 原生骨骼缩放

原版 MMD 9.32 没有骨骼缩放。PMD 与 PMX 的全部骨骼可等比或分轴缩放，包含末端、IK 与物理骨骼。运行时与存储都保留完整 3×3 变换，不压成三个独立数值。

| 项目 | 内容 |
| --- | --- |
| 入口 | 视口移动按钮下方新增的整体、X、Y、Z 四个按钮；局部/全局/视图选择器决定分轴方向。 |
| 默认行为 | 每向上拖动 100 像素约为 `e` 倍；Ctrl 精调，Shift 加速；配件、相机、灯光与无骨骼选择时禁用；播放期间不能编辑。 |
| 保存方式 | 当前姿态与缩放关键帧写入 PMM v2 扩展记录 `org.mikudancestudio.model.<slot>.boneScale.v1`；VMD 标准段不变，缩放写在同文件尾部的 `MMSXMMSX`/`MMSXVMDX` 扩展中（读取兼容旧 `MDSXMDSX`/`MDSXVMDX`）。PMM 全部扩展上限 16 MiB，VMD 扩展上限 8 MiB。 |
| 兼容性 | 原版 MMD 读取标准段并忽略尾部扩展；通过原版重新保存会丢弃扩展。 |
| 证据 | `ad032d9` 为功能提交；`src/model/bone_scale.cpp`（`Enabled` 第 385 行、`Drag` 第 393 行、`Interpolate` 第 146 行）、`include/mikudancestudio/bone_scale.hpp:71-77`、`src/io/bone_scale_io.cpp`、`include/mikudancestudio/bone_scale_io.hpp:40-48`、[骨骼缩放说明](../../guides/bone-scaling.md)。 |
| 验证范围 | 14 组通过：缩放求值、关键帧登记与编辑、撤销重做、真实 VMD 往返、真实 PMM 保存重开、损坏扩展拒绝、HUD 三种 DPI 两组背景共 18 组回读。PMX 最大绝对差 `1.91e-6`，PMD `1.79e-7`。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-刚体与-joint-的缩放继承"></a>
#### 刚体与 joint 的缩放继承

| 项目 | 内容 |
| --- | --- |
| 入口 | 无需额外入口，绑定骨骼的刚体自动继承骨骼缩放。 |
| 默认行为 | 球、盒、胶囊保留完整仿射碰撞边界；joint 两端连接点与平移限位随之更新；角度限位、弹簧刚度、质量、阻尼保持原值。 |
| 证据 | `d214cd4`；`src/physics/physics_scale.cpp`、`src/physics/scene_gizmo_data.inc`。 |
| 验证范围 | 十组后台回归；真实 PMM 为「测试刚体.pmm」，含 140 根骨骼、45 个刚体、27 个关节。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-物理与关键帧烧录"></a>
#### 物理与关键帧烧录

原版 MMD 之外的新增烘焙功能。入口为 **烘焙 → 烧录关键帧…**，在独立 Bullet 世界中从第 0 帧顺序计算。

| 项目 | 内容 |
| --- | --- |
| 入口 | 菜单「烘焙 → 烧录关键帧…」，命令 ID `32800`（`include/mikudancestudio/physics_bake.hpp:72`）；弹出菜单 ID `32802`（同文件第 77 行）。 |
| 默认行为 | ERP `0.2`、CFM `0`、子步数 `1`、内部倍率 `1`、基准频率 `60 Hz`；参数只用于本次烧录。范围含两端，帧号 0 至 100000。 |
| 保存方式 | 结果为原生骨骼关键帧，随 PMM/VMD 常规流程保存；物理键带原生「禁用物理」标记。高级参数可另存为不加密 UTF-8 JSON 预设，预设不写入 PMM 扩展。 |
| 证据 | `51e2f64`；`src/physics/physics_bake.cpp`、`src/physics/physics_bake_settings.cpp`、`src/window/physics_bake_dialog.cpp`、[物理烧录说明](../../guides/physics-baking.md)。 |
| 验证范围 | 三语隐藏对话框、真实 PMM 烧录、事务与撤销、VMD 往返；记录见 [物理烧录验收](../animation/baking.md#record-physics-baking-validation)。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-通道选择稀疏采样与动态骨骼键池"></a>
#### 通道选择、稀疏采样与动态骨骼键池

| 项目 | 内容 |
| --- | --- |
| 入口 | 烧录窗口的「通道…」；相机/灯光/附件主页可选择相机、灯光、自阴影、重力与每个 `.x` 的子参数，模型页可选择骨骼、表情与显示/IK 轨道。 |
| 默认行为 | 起止帧与步长决定采样，总包含结束帧；骨骼稀疏烘焙替换区间内旧键；边界恢复原状态。 |
| 证据 | `80bc305`、`df9f00a`；`src/animation/channel_bake.cpp`、`include/mikudancestudio/channel_bake.hpp`、`src/model/bone_key_pool.cpp`、[通道烘焙验收](../animation/baking.md#record-baking-channels-validation)。 |
| 验证范围 | `channel_bake_tests` 16 阶段通过；真实 PMM 键池增长至 983,040 槽、实际写入槽 600,000。该记录明确这不是生成 983,040 个有效键的压力测试。 |
| 已知限制 | 表情、显示及部分场景轨道仍有既有容量，超限拒绝提交；新通道尚未做成统一长期撤销历史。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-宿主侧可选-bullet3-物理接入"></a>
#### 宿主侧可选 Bullet3 物理接入

计算 DLL 与适配器属于独立仓库 `D:/MMD-Next/MMV-Bullet3`，不在本仓库内。本仓库交付的是宿主侧的可选服务与菜单配合。

| 项目 | 内容 |
| --- | --- |
| 入口 | 可选 MMV 效果以 `MMVPhysics="Bullet3"` 标注；引擎选择作用于整个场景。宿主侧通过 `MmvHostServices` 的可选尾部提供 `physicsInput` 与 `selectBullet3`。 |
| 默认行为 | `selectBullet3` 关闭原生物理并保存此前模式，释放时恢复；原先已关闭则仍恢复为关闭。宿主继续拥有原生物理世界。 |
| 证据 | `8506356`、`b2b5e79`；`include/mmv/mmv.h`（ABI 版本 1）、`src/mmv_host/host.cpp`、[Bullet3 接入](../extensions/physics.md#record-mmv-bullet3)、[集成验收](../extensions/host.md#record-mmv-bullet3-main-integration)。 |
| 验证范围 | Rou 模型 96 个刚体、139 个关节；慢帧播放与固定帧编辑；ABC 与网格缓存集成。 |
| 未核实 | Bullet3 视频导出、原生 VMD 物理烧录、所有第三方 MME 效果组合未验证。 |
| 状态 | 已交付增强（宿主侧交接部分）。 |

<a id="record-mms-native-features-audit-三已交付增强附件"></a>
### 三、已交付增强：附件

<a id="record-mms-native-features-audit-附件曲线编辑入口与附件动作轨道"></a>
#### 附件曲线编辑入口与附件动作轨道

| 项目 | 内容 |
| --- | --- |
| 入口 | 附件操作区的曲线按钮（命令 `32900`）；附件区的「读取」按钮（命令 `32901`）；文件菜单的「读取当前附件动作（VMD）」（命令 `32903`）。 |
| 默认行为 | 附件变换导出为 `Accessory` 骨骼轨道，其他参数写为表情代理；完整名称与曲线保存在 VMD 尾部 `MMVCURV1` 扩展中。 |
| 保存方式 | 宿主主页与附件曲线写入 PMM 扩展；VMD 先写标准骨骼、表情、相机、灯光、自阴影及显示/IK 段，再追加扩展。 |
| 证据 | `a017903`、`f576e96`；`include/mikudancestudio/animation_curve_editor.hpp:52-55`、`src/window/animation_curve_host_ui.cpp:64-65,114-152`、`src/animation/animation_curve_motion.cpp:17-18,477-485`、`src/io/vmd_save.cpp`、[曲线编辑器](../../guides/animation-curves.md)。 |
| 验证范围 | 公共曲线、宿主轨道、曲线窗口、VMD、MMV 参数、运行时、分配窗口共 7/7 CTest 通过；真实 MMS + MMV 窗口 0 失败。 |
| 已知限制 | 没有扩展的多骨骼 VMD 不自动猜测附件目标，会报告无法选择唯一轨道。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-四已交付增强曲线编辑与-vmdpmm-扩展"></a>
### 四、已交付增强：曲线编辑与 VMD/PMM 扩展

<a id="record-mms-native-features-audit-原生浮窗曲线编辑器"></a>
#### 原生浮窗曲线编辑器

宿主原生非模态浮窗，来源包括当前附件、摄像机、灯光、自阴影与重力。

| 项目 | 内容 |
| --- | --- |
| 入口 | 附件操作区曲线按钮（`32900`）；MMV 的材质分配与骨骼分配页在参数区提供同一入口。 |
| 默认行为 | 数值/速度两种视图；支持阶梯、线性、贝塞尔以及独立、对齐、水平、线性、自动切线；布尔与枚举通道保持阶梯。 |
| 保存方式 | 宿主主页与附件曲线写入 PMM 扩展；MMV 参数关键帧与曲线写入 `.vmm`。 |
| 证据 | `f576e96`（公共曲线核心）、`a017903`（宿主窗口集成）、`7d783ca`（与 Bullet3 后端合并）；`src/window/animation_curve_editor.cpp`、`src/animation/animation_curve.cpp`、`src/animation/animation_curve_host.cpp`、[曲线编辑器](../../guides/animation-curves.md)、[曲线与 MMV 窗口验收](../extensions/interface.md#record-mmv-curve-ui-validation)。 |
| 验证范围 | 参数浮窗专项 9,200 项检查 0 失败；真实 PMM 烘焙、菜单、VMD 往返与界面锁 0 失败。 |
| 已知限制 | 当前入口不包含模型表情曲线。`kMorphCurve = 32902` 在 `80bc305` 的 `include/mikudancestudio/animation_curve_editor.hpp:54` 已声明，但在该提交的 `src`、`include`、`tests` 中没有任何使用点。工作树正在接通表情曲线对比显示，尚未合入，见第十一节。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-pmm-v2-扩展容器"></a>
#### PMM v2 扩展容器

| 项目 | 内容 |
| --- | --- |
| 入口 | 无独立入口。正常保存、另存和打开 PMM 时自动读写。 |
| 默认行为 | 文件头仍为 `Polygon Movie maker 0002`；标准数据之后是扩展记录串与 32 字节尾标 `MMSXPMM2`（读取兼容旧 `MDSXPMM2`），含标准长度、扩展长度、CRC32 与保留字段。 |
| 保存方式 | 已定义设置：`org.mikudancestudio.skinning.processor`（运算器类型意图）、`org.mikudancestudio.view.transparent_sort_precision`（透明排序档位）、`org.mikudancestudio.model.<slot>.boneScale.v1`（骨骼缩放）。未知且有效的记录原样写回。 |
| 兼容性 | 原版 MMD 9.32 x64 读完标准数据后关闭文件，忽略附加数据的结论来自库内静态反汇编核对，未启动原版界面验证。原版重新保存会丢失扩展。 |
| 证据 | `58f4ea1`；`src/io/pmm_extension.cpp:1`、`src/io/pmm_project_settings.cpp`、[PMM v2 扩展格式](../../reference/pmm-extensions.md)。 |
| 验证范围 | `pmm_extension_tests` 与 `pmm_project_settings_tests` 覆盖编解码、边界、CRC、未知记录保留与菜单状态。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-vmd-尾部扩展"></a>
#### VMD 尾部扩展

| 项目 | 内容 |
| --- | --- |
| 入口 | 骨骼缩放导出与曲线导出共用同一条尾部扩展通道。 |
| 默认行为 | 标准 VMD 段不变；没有尾部扩展的 VMD 导入时骨骼缩放为单位矩阵，不会报错。 |
| 证据 | `src/io/bone_scale_io.cpp:3`、`include/mikudancestudio/bone_scale_io.hpp:40-48`（`MMSXMMSX`/`MMSXVMDX`，旧 `MDSXMDSX`/`MDSXVMDX` 读取兼容）、`src/animation/animation_curve_motion.cpp:12`（`MMVCURV1` 尾部 40 字节页脚）。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-五已交付增强烘焙网格缓存与输出"></a>
### 五、已交付增强：烘焙、网格缓存与输出

菜单「烘焙」由宿主追加，位置在「帮助」左侧，命令 ID 为 `32800`（烧录关键帧）、`32801`（导出 ABC）、`32803`（清除网格烧录）、`32804`（烘焙网格），弹出菜单 ID 为 `32802`，定义在 `include/mikudancestudio/physics_bake.hpp:68-86`，菜单构建在 `src/window/physics_bake_dialog.cpp:788-838`。

<a id="record-mms-native-features-audit-网格烘焙与-alembic-导出"></a>
#### 网格烘焙与 Alembic 导出

| 项目 | 内容 |
| --- | --- |
| 入口 | 「烘焙 → 烘焙网格…」（默认只写工程缓存）与「烘焙 → 导出 ABC…」（默认只导出文件）；两项可同时勾选。清除用「烘焙 → 清除网格烧录…」。 |
| 默认行为 | 采样 **MMS/MMV 交接处、MME 着色之前**的几何；帧范围 0–100000 按 30 fps 时间轴解释；采样率 1–120，默认 30。 |
| 保存方式 | PMM 记录相对路径映射，几何另存为旁边的 `<工程名>.<内容标识>.mdsmesh.abc`；VMM 保存 FX 开关键帧。 |
| 兼容性 | 原版 MMD 忽略本工程的缓存扩展。旧版没有区间标记的缓存仍按旧规则读取，新规则不追溯改写旧存档。 |
| 证据 | `54bfe14`；`src/io/mesh_bake_cache.cpp`、`src/io/alembic_export.cpp`、`src/window/alembic_export_dialog.cpp`、`src/render/vertex_capture.cpp`、`src/render/deformed_mesh_capture.cpp`、[网格导出与烘焙](../../guides/mesh-export.md)。 |
| 验证范围 | `vertex_capture_tests` 95 项、`mesh_bake_cache_tests` 真实 Ogawa 往返、`mmv_runtime_tests` 208 项；Unicode 文件名、30/60 fps 采样、取消保留旧文件、ABC 原生路径与 MME 顶点变形均通过。 |
| 已知限制 | 屏幕后期、自定义投影、依赖历史渲染目标的效果与非标准绘制有明确限制；不支持局部顶点过滤的纯 ABC 输出会被界面拒绝。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-六已交付增强渲染透明排序与蒙皮"></a>
### 六、已交付增强：渲染、透明排序与蒙皮

<a id="record-mms-native-features-audit-五档透明排序上限"></a>
#### 五档透明排序上限

原版 MMD 没有该选项。菜单为「显示 → 透明排序上限」，命令 ID `304` 至 `308`，定义在 `include/mikudancestudio/view_options.hpp:10-12`。

| 档位 | 命令 ID | 行为 |
| --- | ---: | --- |
| 低 | 304 | 16 桶计数排序，远到近，同桶保持输入顺序；不分层、不分割。 |
| 标准（默认） | 305 | 64 桶计数排序，远到近，同桶保持输入顺序。 |
| 高 | 306 | 按三角形中心深度稳定排序，同深度按原始三角形序号。 |
| 最高 | 307 | 分层上限为裁剪后参与排序的三角形数量加一。 |
| 关闭 | 308 | 不执行自动透明排序。 |

| 项目 | 内容 |
| --- | --- |
| 默认行为 | 新建工程与无字段工程使用标准。关闭只把唯一字节写成 `4`，不记忆先前档位。 |
| 保存方式 | PMM v2 扩展记录 `org.mikudancestudio.view.transparent_sort_precision` 的单字节。 |
| 回退预算 | 仅最高档使用 BSP，额外 4,000,000 个三角形、深度 512、epsilon `2e-13`。三档下调与最新验证见[强度调整记录](../rendering/transparency.md#record-transparent-sort-strengths)。 |
| 证据 | `ab7c2f0`、`211d740`、`904012c`、`c86bf18`、`1e54b05`；`src/render/transparent_geometry.cpp:80-206`、`src/render/model_renderers.cpp`、`src/window/command_view_menu.cpp:2896-2920`、[五档上限](../rendering/transparency.md#record-transparent-sort-five-tiers)。 |
| 历史验证范围（下调前） | 15 次独立运行通过；真实 ToonCloud PMM 保存/重开/另存；低、标准、高、最高各带 4x MSAA 的像素驱动；最高档实际完成 1152 与 4608 层。 |
| 状态 | 已交付增强。 |

旧入口说明：命令 `303` 是历史开关的兼容入口，已没有对应菜单项，只在「关闭」与「标准」之间切换，处理代码见 `src/window/command_view_menu.cpp:2879-2894`。早期六档测试格式不做字节换算，读取规则见 [PMM v2 扩展格式](../../reference/pmm-extensions.md)。

<a id="record-mms-native-features-audit-正式-d3d9mme-蒙皮运算器"></a>
#### 正式 D3D9/MME 蒙皮运算器

| 项目 | 内容 |
| --- | --- |
| 入口 | 「显示 → 蒙皮运算器」，菜单 ID `32000`、自动项 `32001`、CPU 项 `32002`、适配器项从 `32100` 起（`include/mikudancestudio/skinning_menu.hpp:6-9`），只读状态项 `32003`（`src/window/skinning_menu.cpp:85`）。 |
| 默认行为 | 「自动（优先高性能）」，逐模型实测 CPU 与全部可用 GPU 的完整蒙皮耗时，选择结果正确且中位数最低者。CPU 胜出属于正常结果。 |
| 保存方式 | 本机偏好写 EXE 同级的 `data/skinning.ini`；工程另存一份设备类型意图（自动、CPU、集成显卡、独立显卡），不保存品牌、型号或测速结果。 |
| 兼容性 | QDEF、未知权重类型及不满足 GPU 输入条件的模型走 CPU 分支；手动显卡失败后回退 CPU；PMM 指定类型缺失时运行时回到自动，但再次保存仍保留原类型意图。 |
| 证据 | `7523928`、`d3f2e59`、`cf09d31`；`src/model/skinning_backend.cpp`（`AutomaticEntry` 第 44 行、`Median` 第 255 行、`Calibrate` 第 268 行、`ForgetModel` 第 396 行）、`src/window/skinning_menu.cpp`、[GPU 蒙皮](../performance/devices.md#record-gpu-skinning)、[自动选择](../performance/devices.md#record-skinning-autoselect)。 |
| 验证范围 | `skinning_backend_tests` 60 组 CPU/GPU 比较；真实角色 183698 顶点、969 骨骼三组姿态误差 0；MME CPU/GPU 完整渲染通过。 |
| 性能口径 | 该方案包含同步回读与重新上传，在已测样本上慢于 CPU，性能收益未通过验收。交付的是可选择的 GPU 蒙皮能力，不是已完成的蒙皮加速。 |
| 状态 | 已交付增强。 |

<a id="record-mms-native-features-audit-不透明像素预通道与浮点合成"></a>
#### 不透明像素预通道与浮点合成

| 项目 | 内容 |
| --- | --- |
| 入口 | 无独立入口，属于透明渲染路径的一部分。 |
| 证据 | `e73eaaf`（私有浮点目标累积后再提交）、`6f01562`（MSAA 下保留不透明遮挡）、`305f4f9`、`3109a20`。 |
| 状态 | 已交付增强。原版 MMD 没有逐像素透明分层，此路径服务于上面的高档与最高档。 |

<a id="record-mms-native-features-audit-七已交付增强mmv-插件加载与交接宿主侧"></a>
### 七、已交付增强：MMV 插件加载与交接（宿主侧）

MMV 插件自身的界面、参数面板、透视窗口、FX 编译与 `.uifx`/`.vmm`/`.vem` 存档属于插件，不计入 MMS 原生。宿主侧提供下面这些部分，属于宿主扩展。

| 项目 | 内容 |
| --- | --- |
| 插件契约 | `include/mmv/mmv.h` 定义 `MMV_ABI_VERSION 1` 的纯 C ABI，不含 DirectX、Bullet 与 STL 类型。 |
| 宿主交接 | `src/mmv_host/host.cpp:1` 以 `MMV_HOST_MME_BRIDGE` 标识的代码在蒙皮之后、主设备 `BeginScene` 之前获取快照，把插件结果复制到独立渲染记录（同文件第 258、323 行）。 |
| 菜单接入 | `src/exports/effect_api.cpp:452` 与 `src/window/enhance_model_io.cpp:1109` 调用 `mmv_host::RefreshMenu`；命令路由在 `src/window/command_dispatch.cpp:393`。菜单未安装插件时不显示。 |
| 网格烘焙交接 | `src/window/alembic_export_dialog.cpp:170` 调用 `mmv_host::ClearMeshBakeObjects`；烘焙采样 MMS/MMV 交接处几何。 |
| 引入提交 | `b1de95c`、`8506356`。 |
| 宿主导出 | 宿主保持 37 个导出名称与序号不变。 |
| 状态 | 已交付增强（宿主侧）。插件自有能力见 [MMV 实现记录](../extensions/host.md#record-mmv-implementation)。 |

<a id="record-mms-native-features-audit-八建库快照已有的自定义功能来源不明待原版核验"></a>
### 八、建库快照已有的自定义功能（来源不明，待原版核验）

以下能力存在于 `ba6b269`，无法归属到本区间 254 个提交。它们相对原版 MMD 9.32 确实属于扩展，但引入时间与原始出处没有记录，原版核验证据也不在库内。

| 能力 | 位置 | 说明 |
| --- | --- | --- |
| 简体中文界面（第三语言） | `include/mikudancestudio/ui_language.hpp`、`src/window/command_view_menu.cpp:1854-1862`、`src/window/ui_translation.cpp`、`src/window/localize_ui.cpp` | 命令 260 的三态循环与中文词表在快照中已存在。 |
| Unicode 文件名与路径 | `src/window/unicode_window.cpp`、`src/main/text_encoding.cpp`、`docs/records/formats/projects.md` | 快照已存在，本区间只由 `342bffe` 触及 `text_encoding.cpp`。 |
| 实验性 D3D12 预览渲染器 | `src/render/d3d12_native.cpp`、`src/render/d3d12_skinning.cpp`、`include/mikudancestudio/d3d12_native.hpp`、`docs/records/performance/devices.md` | 建库快照已包含全部 D3D12 源文件。本区间只有 `7523928` 把计算结果接进正式 D3D9 蒙皮。D3D12 预览本身仍是实验路径，启动方式与缺失功能见 [迁移记录](../performance/devices.md#record-d3d12-migration)。 |
| FPS 覆盖显示 | `src/app/app_defaults.cpp:57`（`FpsOverlayEnabled() = 0`）、`src/app/frame_driver.cpp:525` | 默认关闭。快照已存在，原版是否有对应显示未核。 |
| 视口操作按钮的既有坐标状态与命中区域 | `src/render/sprite_overlay.cpp`、`src/window/ui_init.cpp` | 本区间只改配色与角标，不增加新的坐标模式。 |
| NVIDIA 3D Vision 立体桥 | `src/render/stereo_nvapi.cpp` | 建库快照已有，本区间无提交触及，未做运行验收。 |
| AVI/DirectShow 录制与 MMdxShow | `src/app/avi_record_start.cpp`、`src/app/dshow_record_graph.cpp`、`src/app/record_readback.cpp`、`src/mmdxshow/` | 建库快照已有；录制与输出在本区间没有新增宿主可见能力。 |
| Kinect 运动捕捉 | `src/app/oni_kinect.cpp`、`src/app/oni_skeleton_pump.cpp` | 建库快照已有。 |

<a id="record-mms-native-features-audit-九纯移植兼容修复不计入增强"></a>
### 九、纯移植兼容修复（不计入增强）

| 改动 | 提交 | 判定 |
| --- | --- | --- |
| 鼠标越过视口边缘时重新居中并继续拖动 | `70a5c15`、`3033b6c` | 恢复原版操作按钮已有行为。前缀为 `feat` 不改变结论。 |
| 保存 PMM 时通知 MME 自动保存同名 EMM | `ad7058e` | 修复原有保存通知链路。见 [EMM 自动保存](../effects/compatibility.md#record-emm-autosave)。 |
| 录制回读的水平采样相位与非法尺寸 | `8b6b0aa`、`6a1843f` | 修复移植实现自身的缺陷：基线第二行即失败，修复后通过。见 [代码审查记录](audits.md#record-rendering-review)。 |
| MME toon 纹理经宿主导入槽加载 | `9facf26` | 让 MME 能注册 toon 纹理，修复移植侧 `LoadLibraryA`/`GetProcAddress` 绕过导入地址表的写法。见 [黑草问题](../rendering/materials.md#record-mme-toon-black)。 |
| 视点保持（跟随开关、相机关键帧、面板切换、启动） | `116e3aa`、`1c142c6`、`bb6a9b3`、`96ce788`、`eb1ae97` | 改回原版行为。 |
| 坐标选择器双次推进 | `c485cfb`、`bede0b8`、`4c11fa5`、`d03c23b` | 修复移植实现的事件处理缺陷。 |
| 配件面板相机参考模式补偿符号与父级相机 | `45ae9b2`、`57f68ff`、`2284dc5` | 改回原版行为。 |
| 场景灯光编辑框提交 | `e3f291e` | 改回原版行为。 |
| PMX 顶点布局与表情帧填入 | `90f6b67` | 改回原版行为。 |
| 文件加载器越界与畸形输入加固 | `b0645b8`、`2986d17`、`69d4301`、`6b1dd24`、`2283f0f`、`0a14f5e`、`f6c16b2` | 安全性修复。按分类规则需逐条确认缺陷来自原版还是移植实现；本区间没有为每条补原版证据，维持原分类并标为待核。 |
| 非法 PMX 关节与非有限骨骼姿态 | `c57af81` | 稳定性修复。原版证据未补，标为待核。见 [物理安全修复](../animation/physics.md#record-physics-joint-pose-safety)。 |
| 视口坐标标识字体与描边 | `ce5dd99`、`8dd274f` | 移植实现的绘制一致性修复。 |

<a id="record-mms-native-features-audit-十未合入或已停用的实验"></a>
### 十、未合入或已停用的实验

| 项目 | 状态 | 依据 |
| --- | --- | --- |
| 全量透明排序（global sort） | 停用 | 保留为实验与性能验收记录。见 [全量实验状态](../rendering/transparency.md#record-transparency-global-sort)。 |
| 六档排序上限与双字段迁移 | 被五档取代，只作为历史记录 | 见 [六档上限记录](../rendering/transparency.md#record-transparent-sort-limit-pmm)。本程序不做字节换算。 |
| BSP 云排序实验 | 失败保留，只作为回退路径预算的来源 | `4173b97`、`dcea555`。 |
| `3026`/实验分支上的其它排序试验 | 分支保留，未合入 `main` | `baseline/*`、`codex/alpha-*` 等分支。 |
| 视口缩放模式与手柄、骨骼三态选择、表情曲线对比 | 未提交 | 见第十一节。 |

<a id="record-mms-native-features-audit-十一工作树待集成未交付"></a>
### 十一、工作树待集成（未交付）

截至 2026-09-27 12:50，`main` 为 `80bc305`。工作树里同时有其他会话正在写入的未提交改动，文件数量在审计期间仍在变化，因此本节只列能力与证据，不按当时的文件计数下结论。下面这些能力只在工作树内，尚未进入 `main`，不计为已交付。

| 待集成能力 | 工作树证据 | 状态 |
| --- | --- | --- |
| 骨骼三态选择按钮 | `include/mikudancestudio/panel_controls.hpp`（`kBoneModeGrid` 新增）、`src/window/localize_ui.cpp`、`src/window/command_control_450.cpp` | 源码与测试已写，未构建、未运行。 |
| 视口缩放手柄（`拡大縮小` 模式） | `include/mikudancestudio/bone_scale_gizmo.hpp`（未跟踪新文件）、`src/model/bone_scale.cpp`（`DragViewPlane`）、`src/render/debug_geometry.cpp` | 同上。 |
| 骨骼缩放插值/视图平面拖动路径 | `include/mikudancestudio/bone_scale.hpp`（新增 `DragViewPlane` 声明）、`src/model/bone_scale.cpp` 第 449、474、490 行为工作树行号 | 同上。 |
| 表情曲线对比显示 | `include/mikudancestudio/animation_curve_comparison.hpp`（未跟踪新文件）、`src/window/animation_curve_editor.cpp`、`include/mikudancestudio/animation_curve_editor.hpp`（新增 `kCompareOff`／`kCompareSameGroup`／`kCompareAllGroups`／`kCompareFree` 与 `Channel::group`） | 源码已写，未构建、未运行。 |

工作树对应的文档记录为 `docs/guides/bone-scaling.md` 中「缩放模式与视口手柄的回归（2026-09-27，已写未构建）」一节与 `tests/bone_scale_gizmo_tests.cpp`。该文件写明本轮只写了源码、测试与文档，没有构建，也没有运行任何测试、渲染回读或 GPU 验收。

<a id="record-mms-native-features-audit-十二未核实项与证据缺口"></a>
### 十二、未核实项与证据缺口

| 未核实项 | 原因 |
| --- | --- |
| 简体中文界面的引入时间与原始出处 | 功能已在建库快照内，本区间无对应提交；缺少原版对照。 |
| Unicode 路径支持的引入时间 | 同上。 |
| D3D12 预览是否应当对外发布 | 源码在快照内，但没有本区间的整体验收记录。 |
| FPS 覆盖显示是否相对原版为新增 | 快照已有默认关闭的实现，未做原版对照。 |
| NVIDIA 3D Vision 立体桥 | 快照已有，本区间无提交，也没有运行验收记录。 |
| Kinect 运动捕捉 | 快照已有，本区间无提交，也没有运行验收记录。 |
| 原版 MMD 隐藏表情双击崩溃的根因 | 用户报告未在原版复现，原版版本号未记录。见 [隐藏表情安全处理](../animation/editing.md#record-hidden-morph-panel-safety)。 |
| 文件加载器加固与非法关节/姿态修复的来源 | 未逐条补原版证据，维持待核。 |
| 视口光标图标统一的整包集成与真实场景验收 | 该功能自己的记录写明模块级验证完成、整包集成未完成。提交 `e27972b` 之后是否有补偿性整包验证，库内没有对应清单。 |
| GPU 蒙皮与 MMV/Bullet3 的第三方效果覆盖 | 各记录均声明未穷举所有第三方 MME 效果与设备故障场景。 |
| ABC 捕获对屏幕后期、自定义投影与依赖历史渲染目标的效果 | 记录声明有明确限制，未逐项验证。 |
| 有编码标记但内容损坏的旧 PMM 路径字段 | 记录声明无法凭内容判断原始代码页，需从未损坏工程重新载入。 |

<a id="record-mms-native-features-audit-十三本次审计发现的既有文档差异"></a>
### 十三、本次审计发现的既有文档差异

| 位置 | 差异 |
| --- | --- |
| 原 `docs/records/development/features.md` 的 Git 范围 | 写为「`ba6b269` 至 `dbe5c6b`」，未覆盖 `dbe5c6b` 之后的提交。本次更新为 `ba6b269` 至 `80bc305`。 |
| 原 `docs/records/development/features.md` 的原生骨骼缩放入口 | 只写视口四个按钮。工作树新增的面板「拡大縮小」模式按钮与视口手柄尚未进入 `main`，本次改为标注待集成。 |
| 原 `docs/records/development/features.md` 的烘焙入口 | 只写「编辑 → 关键帧烧录…」。当前实现是宿主追加的顶层「烘焙」菜单，含四个命令和一个弹出菜单 ID，入口名称为「烘焙 → 烧录关键帧…」。 |
| 原 `docs/records/development/features.md` 未覆盖的能力 | 曲线编辑器、VMD 尾部扩展、PMM v2 容器（只提了项目设置字段）、网格烘焙与 ABC 导出、通道选择、MMV 宿主菜单与插件交接。 |
| `docs/records/rendering/transparency.md` 描述的排序开关 | 描述的是命令 `303` 的开关形态；当前形态是五档上限，命令 `303` 只作兼容入口。该文档保留为历史记录，不作为当前行为。 |
| `tests/README.md` 的核对日期 | 标注为 2026-09-20，未随本区间新增测试更新。 |
