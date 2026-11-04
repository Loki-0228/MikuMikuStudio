# MMV4MMS 安装、分配与存档

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mmv4mms"></a>
<a id="record-mmv4mms-菜单与文件格式"></a>
<a id="record-mmv-install"></a>
<a id="record-mmv-install-安装"></a>
<a id="record-mmv-install-分配效果"></a>
<a id="record-mmv-install-存档与作者资料"></a>
<a id="record-mmv-install-2026-09-27-界面与分配更新"></a>
<a id="record-mmv-storage"></a>

## 安装范围

完整包支持可选 `MMV4MMS.dll`，最小包不加载该插件。将插件 DLL 和配套 `MMV/` 资源放在 `MikuMikuDance.exe` 同目录，效果包按插件说明准备。主程序不构建、下载或打包插件。

MMV4MMS 与原生 MME 代理不能同时启用。检测到已加载 `MMEffect.dll` 时不接管场景。未安装或禁用插件时使用 MMS 自身渲染；当前加载器不加载旧名 `MMV.dll`。

## 效果入口

右上角「MMV」菜单由 DLL 注册，包含效果映射、启用、自动重载、自动保存映射、全部重载、日志及加载 EMM。菜单和参数随宿主语言切换，具体可用项取决于安装的插件。

效果分配通过编辑器显式设置。EMM/EMD 导入按插件提供的加载入口执行，不能仅因工程旁有同名 EMM 就假定已经应用。PMM 打开、保存通知传给插件，自动加载与保存 VMM 由插件配置决定。

骨骼、材质范围和参数见[分配界面](interface.md)、[FX 接口](effects.md)及[参数描述](parameters.md)。以下为配套插件存档约定，读取时仍须匹配实际插件支持的版本。

## MMV 工程与对象存档


`.vmm` 保存工程的 MMV 分层、分配、参数和关键帧曲线。`.vem0`、`.vem1` 等文件组成单个对象的静态效果预设，每个文件的数字尾缀表示所属层。
二者是带版本标记的 UTF-8 INI。读取允许 UTF-8 BOM，写入不加 BOM。
MMV4MMS 可通过显式加载导入 `.emm`，不把工程状态写回 EMM；VMM/VEM 不是 EMM 的替代写入格式。效果编辑器也支持显式加载 `.emd`。

`.x` 的声明只指定默认效果及控制附件状态，不规定层号。`.uifx` 描述效果控件，不能替代对象预设。

```ini
[Info]
Type=MMV
Version=3
Kind=Project
AutoLoad=true
AutoSave=true

[Binding.0]
ObjectKind=1
ObjectPath=accessory/cloth.x
Instance=0
Owner=
Target=Main
Material=
Bone=
Effect=effects/cloth_mmv.fx
Enabled=true
Controller=false
Layer=0
```

对象类别 0 为模型，1 为附件。对象由类别、规范路径和同路径实例序号识别；
显示名不参与匹配。`Owner` 为空表示固定分配页；非空值是声明目标的效果实例标识。
内置分配页为 `Target=Main`，可选择骨骼或材质展开。其所在的 MMV 自带层固定为 0。
`Material` 为材质稳定标识；新骨骼分配的 `Bone` 为 `@稳定骨骼键`，在所选层的上游表中解析。
原有 `索引:原始名称` 形式仍可读取，必须同时匹配索引与名称。生成骨骼不得用名称猜测身份。
两字段互斥，均为空时表示整个对象。
源材质标识依次为 `source:0`、`source:1` 等。

格式版本为 3，拒绝版本 1、2 和旧 `.vem` 文件，不进行迁移或猜测兼容。
MMV 层编号为 0～65535，从大到小执行。层号不表示可以与外部 MME 代理同时运行；主程序接管规则见[宿主接口](host.md)。
VMM 的 `Layer` 字段记录分配所在层；单层 `.vemN` 由后缀决定层号，不另存 `Layer` 字段。
层号不属于分配身份，移动层不会改变参数和关键帧所属实例。

空 `Effect` 是显式移除，`Enabled=false` 是显式禁用，都参与覆盖。
`Controller=true` 可将附件用作 MMV 控制对象：保留变换等查询数据并隐藏辅助几何。
同一个 `.x` 可以先由 MMV 修改，再交给 MME 着色，不需要另造附件格式。

对象加载时不读取同名 VEM。手动打开 `名称.vem0` 时，按数字顺序读取相同主体的所有 `.vemN` 文件，作为一个事务应用。
只能从 MMV 的 0 层发起「保存当前物体」，选择 `.vem0` 后自动导出各层文件，包含持久化的空层。
数字尾缀不得包含前导零；`名称.vem01` 不是第 1 层。可编辑层之间允许空号。
保存对象预设时，将当前帧的参数求值结果保存为静态基础值，不保存关键帧和曲线。
含动画字段的 VEM 会被拒绝。缺失对象、所属附件、效果目标、骨骼或材质时，整套加载失败并报告不兼容，不改写其他范围。
工程默认加载同名 `.vmm`，PMM 保存成功后才自动写同名 `.vmm`。
自动加载与自动保存可以分别关闭，也可手动打开或另存。

<a id="record-mmv-storage-持久身份与相对路径"></a>
### 持久身份与相对路径

`ObjectIdentity.<id>` 段记录 `Path`、`Kind`、`Instance`。
`PageInstance.<id>` 段记录 `Object`、`Effect`、`Parent`、`Target`、`Material`、`Bone`，
将页签身份与当前文件的绝对位置分开。`MaterialSlots.<object-id>` 按零填充编号
保存新材质标识和保留编号。效果实例、对象身份段的标识是内部不透明字符串。
手工编写主输出分配不需要生成这些段，运行时会补齐。

路径能相对存档位置表达时优先写相对路径，读取时解析为绝对路径。
移动整个工程目录后，页签分配和材质编号保持原身份。
VEM 的 `[Object] Identity` 记录所属对象。手动打开时按存档的对象身份匹配，
不会自动给每个同名实例复制预设。

<a id="record-mmv-storage-每层的展开设置"></a>
### 每层的展开设置

VMM 与 VEM 都保存以下节。`N` 为层号，身份字符串不依赖界面语言。

| 节 | 字段 |
| --- | --- |
| `Layer.N` | `Present=true`，保留没有分配的空层 |
| `LayerView.N.Page.<hash>` | `Owner`、`Target`、`Expansion=Bone` 或 `Material` |
| `LayerView.N.Object.<hash>` | `Object`、`Owner`、`Target`、`AllowExpand`、`Expanded` |

默认展开方式为骨骼，默认展开许可关闭。`AllowExpand` 控制是否允许点击箭头；`Expanded` 保存实际开合状态。关闭许可时保留开合状态，再次允许后可以恢复。

<a id="record-mmv-storage-参数曲线"></a>
### 参数曲线

`Parameters.<效果实例>` 按变量稳定 ID 保存参数。未编辑曲线的旧关键帧保留原有
Step、Linear、Smooth 语义。曲线编辑后，`Var.<id>.Curve.Version=1` 和
`Var.<id>.Curve` 保存各分量的曲线。曲线数据包含帧、值、两侧斜率、手柄权重、插值及切线类型。
`.uifx` 只描述控件和多语言名称，不保存用户动画。原生宿主的相机、灯光、自阴影、重力和附件曲线
写入 PMM 扩展段，MMV 参数动画写入 `.vmm`。操作见 [曲线编辑器](../../guides/animation-curves.md)。

<a id="record-mmv-storage-编码保留与写盘"></a>
### 编码、保留与写盘

节名与字段名区分大小写。值不自动去除空白，`;` 和 `#` 是普通字符。
换行、回车、反斜杠、等号及方括号分别写作 `\n`、`\r`、`\\`、`\=`、`\[`、`\]`。
未知节和未知字段在往返时保留。缺失对象、缺失效果及纯 DX12 版不支持的传统 FX
仍保留分配，修复路径或换用兼容版后可恢复。

单文件上限 8 MiB，单行上限 64 KiB，分配上限 4096。
不支持的未来版本或语法错误使整个读取失败，当前配置不被部分替换。
写入先创建同目录唯一临时文件并刷盘，再替换目标文件。
替换失败时保留旧存档并报告错误。


<a id="record-mmv-storage-烘焙的原生启停轨道"></a>
### 烘焙的原生启停轨道

每个效果实例的 `Parameters.<实例身份>` 段可以保存：

```ini
NativeEnableVersion=1
NativeEnable.Base=1
NativeEnable.Key.10=Step;0
NativeEnable.Key.21=Step;1
```

示例使 10–20 帧停止运行该实例，21 帧恢复。只能使用 0/1 常值键，不插值。原生字段与作者 `Var.*` 分开保存，不占 HLSL 常量寄存器。VMM 保留轨道；手动 VEM 只保存当前静态值，不含关键帧。没有版本标记时按未创建轨道处理。

参数面板注入三语「启用」变量，作者原有 256 个变量的上限不变。若作者恰好声明了 `MMVEnabled`，该变量仍属于作者，烘焙会明确拒绝为该实例添加隐藏开关，不修改 FX 或 UIFX。
