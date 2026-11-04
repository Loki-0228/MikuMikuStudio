# 外置参数界面 UIFX

本页是可选插件的参数描述协议。界面和变量绑定由插件实现，主程序通过[宿主接口](host.md)提供场景。

<a id="record-mmv-ui-guide"></a>
## 为现有 FX 添加外置参数界面


本教程为一个已有 FX 添加中文、英文、日文参数界面和关键帧。原 FX 不需要写入 GUI 注解，也不需要改动算法。示例效果和 `.uifx` 随外部插件提供；语法选项见 [外置 UI 描述参考](parameters.md#record-mmv-ui-reference)。

<a id="record-mmv-ui-guide-在-fx-旁创建描述文件"></a>
### 在 FX 旁创建描述文件

假设原文件 `Face.fx` 已声明 `float Strength=0.5;`。创建 UTF-8 文件 `Face.uifx`，写入：

外置描述文件放在 FX 同目录、用同主文件名，扩展名为 `.uifx`。旧扩展名 `.mmvui` 不再作为默认名匹配，已有文件需要改名成 `.uifx`；如果不想改名，就在 `MMVConfig` 中把原名写进 `string MMVUI="...";`。

```hlsl
float Strength <
    string UIName_zh_CN="强度";
    string UIName_en="Strength";
    string UIName_ja="強さ";
    float UIMin=0;
    float UIMax=1;
    float UIStep=0.01;
    string UIWidget="slider";
>;
```

这里沿用 FX 的变量声明和 `< ... >` 注解语法。`Strength` 必须与原 FX 的变量名、类型一致。只在描述文件中列出的变量会进入这个界面；缺少 `MMVExpose` 时默认暴露。要禁止它记录关键帧，添加 `bool MMVKeyable=false;`。

这条声明没有初始值，因此读取原 FX 的数值初始值。原变量使用复杂初始化表达式时，在描述文件中明确填写界面默认值，例如 `> = 0.5;`。它是运行时输入默认值，不会写回原 FX。

<a id="record-mmv-ui-guide-先使用自动布局"></a>
### 先使用自动布局

只定义变量，不写布局注解，即可自动生成参数面板。最小描述甚至可以只写：

```hlsl
float Strength;
float3 Offset;
bool Enabled;
```

原 FX 必须确实存在这三个同名、同类型变量。面板按变量类型选择控件，并随窗口宽度换行、调整列数。`AutoLayout_mmv.fx/.uifx` 是完整示例。

加载 FX 后，在当前页面选中它的分配行，查看原生参数面板。同一个 FX 的不同分配分别保存参数和关键帧。

<a id="record-mmv-ui-guide-在分配表中赋予效果"></a>
### 在分配表中赋予效果

分配表支持多选。选中行后有两种方式把一个 `.fx` 文件赋予出去：

1. 双击任意一行，打开「设置效果...」文件选择器。这是选 `.fx` 文件的文件对话框，不是打开文件夹。选中文件后，该文件赋予给双击行所属的选择集；选择集有多行时一次全部赋予，取消选择则不做任何修改。
2. 从资源管理器把一个 `.fx` 文件拖到分配表的选中行上松手，该文件同时赋予全部选中行。

拖放按以下规则拒绝，被拒绝的拖放不修改任何一行：一次只接受一个文件；文件必须是 `.fx`；落点必须在选中的行上。拖到表格空白区、或者当前没有选中行时，不猜测赋予目标。

批量赋予是整批生效的：全部行先做校验，任何一行的骨骼或材质范围无法承载赋予（该范围属于其他层、范围键已失效、骨骼范围不兼容），就给出指明该行的错误并整批回滚，不留部分赋予的行，也不把赋予改写到别的范围或别的层上。赋予总是落在分配表当前显示的层上，保存的文件路径就是拖入或选择的那条完整 Unicode 路径。

拖入或双击之后，详情栏按表格顺序逐行列出所有选中行的结果。选中行与焦点行保持不变，参数面板继续跟随焦点行；表格刷新和层切换都按行身份恢复选择，每一层记住自己那一层的选择。

展开规则不变：双击不再是展开手势，展开仍由对象列的箭头单元格、键盘和右键菜单完成；右键菜单里的「允许展开」「展开方式」、灰色箭头和骨骼默认范围规则保持原样。

<a id="record-mmv-ui-guide-添加标题分组与同行排列"></a>
### 添加标题、分组与同行排列

继续编辑外置 `.uifx`，保留原 FX。下面的分组把两个变量优先放在同一行：

```hlsl
float MMVUI <
    int Version=1;
    string UIName_zh_CN="面部调整";
    string UIName_en="Face controls";
    string UIName_ja="顔の調整";
>;
float MMVUIGroup_face <
    string UIName_zh_CN="比例";
    string UIName_en="Proportions";
    string UIName_ja="比率";
    int UIColumns=0; // 自动列数
>;
float Strength <
    string UIGroup="face";
    string UIRow="main";
    int UIOrder=0;
    float UIMin=0; float UIMax=1;
>;
bool Enabled <
    string UIGroup="face";
    string UIRow="main";
    int UIOrder=1;
>;
```

`MMVUI` 和 `MMVUIGroup_face` 是描述文件的界面声明，不要求原 FX 存在这些变量。其他变量仍必须匹配原 FX。窄窗口会把 `main` 行换行；变量不会因固定像素坐标落到窗口外。

分配窗口默认提供「骨骼分配」「材质分配」两个固定页签，分别按对应内容展开；FX 新增页签按其声明的方式展开。作者的 `UIGroup` 只排列选中分配后的参数，不改变页面作用范围。

<a id="record-mmv-ui-guide-给变量记录关键帧"></a>
### 给变量记录关键帧

以位移示例的 `Amount` 为例，在第 0 帧记录值 `0`，在第 30 帧记录值 `1`，选择线性插值。第 15 帧的值为 `0.5`；`Offset=(0,1,0)` 时，网格沿对象局部 Y 方向移动 `0.5`。

操作步骤：

1. 选中效果分配，保持「跟随播放帧」开启，或手动填写记录帧。
2. 修改变量值，按该变量旁的 `K`。同一帧再次按 `K` 会替换原值。
3. 使用变量旁的关键帧列表查看已有记录。选中列表项可设为当前记录帧或删除该键；这只改变面板的记录位置，不移动宿主播放位置。
4. 没有动画的变量可用「应用」保存基础值。已有轨道时，直接应用会提示先按 `K`；开启「自动 K」后，应用将修改记录到当前帧。

编辑中的值不会被播放刷新覆盖。单个变量按 `K` 只提交该变量；其他未应用输入继续保留。运行时拒绝提交时，原设置和待编辑值均保留，面板显示原因。

K 轨道属于效果分配实例，而不是全局 FX 文件。动画帧按每秒 30 帧换算，导出视频的帧率不改变关键帧位置。浮点和向量支持阶跃、线性、平滑；整数、枚举和开关在关键帧处切换。

参数区「曲线」按钮打开原生浮窗，支持增删和移动关键帧、贝塞尔手柄、自定义切线、数值与速度视图，以及选中通道烘焙和 VMD 读写。操作见 [曲线编辑器](../../guides/animation-curves.md)。曲线写入 `.vmm`；手动保存 `.vem` 时只写当前帧静态值，不写动画。

透视修正中的虚拟 FOV 与正交程度是同一逻辑参数的两种表达。内置透视效果以 `OrthographicAmount` 保存 K 轨道，避免两条轨道同时控制同一投影量。

<a id="record-mmv-ui-guide-保持已有工程的参数身份"></a>
### 保持已有工程的参数身份

在描述中添加稳定 ID，可以独立调整名称和语言：

```hlsl
float Strength <
    string MMVId="face_strength";
    string UIName_zh_CN="面部修正强度";
>;
```

翻译不会改变身份。原 FX 升级后若变量重命名，可以把外置声明的变量名改为新名称，并保留 `MMVId`。类型改变、变量缺失或描述语法错误时，加载事务失败并保留上一份成功配置。

外置描述不会把传统 DX9 FX 转换成 DX12。旧 MME FX 的实际执行仍需要兼容版后端；纯 DX12 版保留分配、参数和轨道，但不执行旧 shader。

<a id="record-mmv-ui-reference"></a>
## MMV 外置 UI 描述参考


`.uifx` 使用 FX 变量声明、注解、注释、宏和 `include` 语法。它描述 GUI 与参数绑定，不包含效果算法；加载器只解析元数据，不执行其中的 shader。原 `.fx` 和其 include 文件不被改写。

<a id="record-mmv-ui-reference-文件选择与匹配"></a>
### 文件选择与匹配

默认读取与选中 FX 同目录、同主文件名的 `.uifx`，例如 `Face.fx` 对应 `Face.uifx`。加载器不把旧扩展名 `.mmvui` 当作默认名，也不在 `.uifx` 缺失时回退到它；旧文件需要改成 `.uifx`，或在 `MMVConfig` 中按原名显式指定。这是对已经显式分配的 FX 查找界面，不是为 `.x` 自动搜索或分配同名 FX。

存在外置描述时，以其中的变量列表为界面暴露清单。没有描述文件时，不推测旧 FX 哪些内部变量适合给用户调整；已有 MMV 内联暴露注解仍作为兼容入口。自有 FX 可在 `MMVConfig` 中用 `string MMVUI="ui/face.uifx";` 指定路径；显式路径按写下的后缀读取，加载器不附加也不校验后缀。为禁止二改的 FX 配界面时只需创建默认同名文件。

外置文件为 UTF-8，主文件不超过 1 MiB。`include` 路径按 FX 包含规则相对于所在文件解析。修改现有描述或其 include 会参与热重载；新建首个描述文件后，点击重新加载。

<a id="record-mmv-ui-reference-参数声明"></a>
### 参数声明

除保留的界面声明外，每个声明都必须匹配原 FX 中一个可写全局变量。支持 `float`、`float2/3/4`、`int`、`bool`；不支持数组、矩阵、纹理对象、函数或 `static/const` 变量。类型不匹配、重复声明和未知变量均报错。

没有外置初始值时读取原变量的字面量初始值；支持数字、布尔、向量构造和大括号列表。复杂表达式需要在描述文件提供显式默认值。宏在读取元数据前展开。

| 注解 | 类型 | 默认值或作用 |
| --- | --- | --- |
| `MMVExpose` | bool | 外置声明默认 `true`；`false` 不显示 |
| `MMVId` | string | 默认是原变量名；稳定存档身份 |
| `UIName` | string | 通用显示名称 |
| `UIName_zh_CN`、`_en`、`_ja` | string | 中、英、日名称 |
| `UIHelp` 及语言后缀 | string | 参数帮助文字 |
| `MMVKeyable` | bool | 默认 `true`；允许记录 K |
| `UIMin` / `UIMax` | 数值 | 默认 −1000000 / 1000000；布尔为 0 / 1 |
| `UIStep` | 数值 | 浮点默认 0.01，整数和布尔为 1 |
| `UIWidget` | string | `auto`、`slider`、`number`、`color`、`checkbox`、`enum` |
| `MMVEnum` 及语言后缀 | string | 整数值与标签，例如 `0=关闭;1=开启` |
| `UIGroup` | string | 所属参数分组 ID |
| `UIRow` | string | 同一分组中优先同行的 ID |
| `UIOrder` | int | 非负排序值，相同时保持声明顺序 |

显示文字按当前语言、通用名称、英文、变量 ID 的顺序回退。标签使用 `zh-CN`、`en`、`ja`，注解中的连字符写成下划线。旧 `MMVLabel`/`MMVHelp` 名称仍可读取，外置描述优先使用 `UIName`/`UIHelp`。

一个效果最多暴露 256 个变量。ID 和 HLSL 名称分别唯一；所有数值有限且可由对应类型表示。范围、步长、初始值和枚举候选都必须有效。Color 用于 float3/4，Checkbox 用于 bool，Enum 用于 int，Slider 用于单个 float。

<a id="record-mmv-ui-reference-界面声明"></a>
### 界面声明

文件可包含一个可选标题声明：

```hlsl
float MMVUI <int Version=1;string UIName_zh_CN="参数";>;
```

省略声明时按版本 1 读取。未知版本报错。`MMVUI` 不绑定原 FX 变量。

分组声明名称以 `MMVUIGroup_` 开头，后缀就是分组 ID：

```hlsl
float MMVUIGroup_camera <
    string UIName_zh_CN="相机";
    string UIName_en="Camera";
    int UIColumns=0;
    int UIOrder=0;
>;
```

`UIColumns=0` 为自动；1–4 指定最大列数，窄窗口仍会换行。变量只写 `UIGroup="camera"` 而没有分组声明时，自动创建以 ID 为名称的分组。没有分组或排列注解的变量使用自动布局。作者不填写 Win32 像素坐标。

`UIRow` 只合并同一组的字段。不同组不会因同名行合并。分组与行描述的是参数面板，不是 FX 目标页或场景骨骼层级。描述文件不接受 `technique`、`pass`、函数定义或脚本。

<a id="record-mmv-ui-reference-关键帧"></a>
### 关键帧

关键帧以效果分配实例和稳定变量 ID 定位。整数帧范围为 0–2147483647；同帧重新 K 替换该帧。运行时可以在小数动画帧求值，使用 MMD 每秒 30 帧的时间基准。

| 插值 | 两个关键帧之间的值 |
| --- | --- |
| Step | 保持前一关键帧 |
| Linear | `a + (b-a)t` |
| Smooth | `a + (b-a)t²(3−2t)` |

`t` 是归一化时间，插值模式属于前一关键帧。整数、枚举、布尔使用 Step；向量各分量共享时间和模式。没有关键帧时使用基础值；首键前保持首键，末键后保持末键。`MMVKeyable=false` 的变量不能包含轨道。

采样不修改基础值或关键帧。暂停、跳帧及重复绘制按当前动画时间重新采样。K 轨道保存在 `.vmm` 的 `Parameters.<效果实例ID>` 分段，格式 `Version=1`。未知变量字段保留；已知非法数据导致整个加载事务失败。

<a id="record-mmv-ui-reference-后端与原文件"></a>
### 后端与原文件

旧 DX9/MME FX 保持原源文本交给 D3DX Effect 编译，运行时按原变量名称提交参数。原生 DX12 FX 的内存编译副本自动生成 `MMVUserParameters : register(b1)`，每个参数占 16 字节寄存器；不向磁盘写回转换后的源码。作者不另行声明 b1。内置 `Perspective1` 由参数适配器生成它的既有常量块。

GUI 描述不改变 FX 后端。纯 DX12 版保留旧 DX9 FX 的分配与参数，但不能执行旧 shader。具体后端能力由所安装插件决定，MMS 主程序不解析或执行 UIFX。
