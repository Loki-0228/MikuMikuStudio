# 可选插件 FX 作者接口

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mmv-fx"></a>
<a id="record-mmv-fx-数据与作用范围"></a>
<a id="record-mmv-fx-globals"></a>
<a id="record-mmv-fx-globals-验收范围"></a>
<a id="record-mmv-fx-globals-构建来源与限制"></a>
<a id="record-mmv-fx-globals-同条件计时"></a>
<a id="record-mmv-fx-globals-证据与复跑"></a>
<a id="record-mmv-perspective"></a>
<a id="record-mmv-perspective-适用范围"></a>
<a id="record-mmv-perspective-dx12-计算结果绘制回归"></a>
<a id="record-mmv-perspective-参考资料"></a>

本文保留配套 MMV 效果的声明、数据和求值约定。算法、FX 编译器和示例效果由外部插件提供，主程序包不包含；当前 MMS 仅通过 [MMV4MMS 宿主接口](host.md)接管渲染，不能把作者接口的全部能力视为主程序内置功能。

公共结构以[接口头文件](../../../sdk/mmv4mms/include/mmv/mmv.h)及所安装插件支持的能力为准。MME 语义和 MMV 扩展分别使用，MMV4MMS 不与外部 MME 代理同时启用。

### 数据与作用范围

层按数字从大到小执行，附件层为正数，自带层固定为 0。
非零层的完整输出成为下一层输入。第 0 层也接收完整结构。MMS 的最终图像交接使用独立的 `MmvMmsOutput` 契约，见[宿主接口](host.md)。
层输出不直接改写宿主模型定义、关键帧或原生物理世界。只有明确的烘焙提交才写入宿主数据。

同层所有效果读取该层开始时冻结的输入，彼此不读取输出。后绘制的效果覆盖前者，
运行时从后向前处理，跳过已被覆盖的写入。跨层仍逐层传递。层页签下只显示当前层内容，
自带 0 层只有一个分配页；骨骼与材质是这个页的两种展开方式，不是两个执行阶段。
右键控制是否允许展开及展开方式，默认按骨骼展开。未允许展开时，下拉按钮为灰色且不能点击。

普通网格 FX 的骨骼分配按当前层绑定表中的归一化权重混合效果结果，
不会自动将一个受多骨骼影响的顶点拆成多份。
修改材质组的网格时，运行时隔离该组引用的顶点，避免移动其他材质的共享顶点。

骨骼行的作用范围只包括该骨骼，不自动包含子骨骼；权重为零的顶点逐字段保留。
同拓扑结果对位置、法线、UV、附加 UV 和边缘倍率作混合，法线重新归一化。
拓扑变化只替换三个顶点权重均大于零的三角形，保留边界与范围外的三角形。
生成顶点通过当前层 `vertexBindings` 找到绑定记录；宿主 `sourceVertex` 不能代替该映射。
无法确定层内权重的新三角形不进入该骨骼范围，非法或歧义的引用使当前层对象回退。
骨骼范围保留既有材质参数，
新增材质以稳定标识追加；需要改变既有材质属性时使用材质分配。

刚体和 joint 随模型快照提供，不形成独立分配组。绑定关系是 joint 的 A/B
端刚体索引，再到各刚体的骨骼索引。关系可能是多对多；无骨骼绑定用 `-1`。
`valid=0` 表示当前姿态或连接框架不可用于物理求值，不能把零矩阵当作有效姿态。

| 数据 | 坐标及索引规则 |
|---|---|
| 顶点位置、法线 | 对象网格空间；`MMVWorld` 转到 MMD 世界空间 |
| 宿主原始顶点、权重 | 原始编号固定，数量为 `MMVSourceVertexCount`，不随上层扩容 |
| 当前层绑定顶点、权重 | 独立绑定表；权重访问当前层骨骼，允许上层生成骨骼 |
| 骨骼矩阵、原点 | MMD 世界空间；`parent` 是本对象骨骼索引，`-1` 表示无父级 |
| 刚体矩阵、速度 | 世界空间；尺寸保持刚体形状的局部尺寸定义 |
| joint 的 `anchorA`、`anchorB` | 分别为当前两端的世界坐标连接框架；限位和弹簧按 joint 局部轴 |
| 场景顶点/三角形 | 对象记录给出数组偏移；三角形顶点索引仍相对于该对象 |
| 所有矩阵 | 行主序存储、行向量乘法，位置转换写成 `mul(float4(p,1), matrix)` |

生成顶点若无法对应一个原始顶点，应设 `sourceVertex=0xffffffffu`。
该标记不表示没有层内权重。原生模块可为生成顶点提供有效的绑定记录和四项权重；
不提供映射的普通 HLSL 新顶点不会凭空获得权重。

<a id="record-mmv-fx-原生效果声明"></a>
### 原生效果声明

```hlsl
#include "mmv.fxh"
float MMVConfig < string MMVBackend="DX12"; >;

[numthreads(64,1,1)]
void Expand(uint id:SV_DispatchThreadID) {
    if(id>=MMVVertexCount()) return;
    MMVVertex v=MMVVertices[id];
    v.position+=v.normal*0.1;
    MMVStoreVertex(id,v);
}
technique Main < string MMVType="Mesh"; > {
    pass P < int MMVThreads=64; > {
        ComputeShader=compile cs_5_1 Expand();
    }
}
```

`MMVBackend="DX12"` 选择原生后端；`"DX9"` 或没有 MMV 后端声明的传统 FX
走兼容后端。其他后端值会报错。文件扩展名不决定后端。
编译器处理实际的 include、宏与条件编译，错误定位保留源文件与行号。
UTF-8 为推荐编码，也可读取传统 CP932 FX。

每个 technique 定义一个目标。`MMVTarget` 默认为 `Main`，`MMVType` 为
`Mesh` 或 `Texture`。一个目标可有多个顺序执行的 pass。
mesh pass 使用 `cs_5_1`，原生纹理 pass 使用 `vs_5_1` 和 `ps_5_1`。
`MMVThreads` 必须和 `[numthreads(x,1,1)]` 的 x 一致。
`MMVDispatch` 可为 `Vertices`、`Triangles` 或 `Once`。

输出起始为输入副本，未改写的数据保持输入值。输入 SRV 和输出 UAV 不别名。
`MMVSetCounts` 设置最终顶点/三角形数；`MMVStoreVertex`、`MMVStoreTriangle`
执行边界检查。`MMVAppendVertex`、`MMVAppendTriangle` 使用原子计数追加；
多个线程追加时不保证编号顺序，需要确定性编号时使用显式输出位置。

容量由 `MMVVertexCapacity`、`MMVTriangleCapacity` 明确指定，或由
`MMVVertexMultiplier`、`MMVTriangleMultiplier` 乘输入容量得到。
默认倍率为 1。顶点/三角形数量各上限 16M，材质上限 65536。
容量不足、非法索引、无效材质或非有限数值使当前层整个对象回退到上游网格；不提交部分结果。

`MMV_VERTEX_EDGE_DISABLED` 对应顶点 `flags` 的最低位；设置后采用宿主原有的
边缘收缩处理。`edgeScale=0` 与禁用边缘标记不同，PMX 两者不能混用。

<a id="record-mmv-fx-骨骼及物理查询"></a>
### 骨骼及物理查询

`MMVFindBone`、`MMVFindRigid`、`MMVFindJoint` 接收名称哈希，`MMVFindMaterial` 接收材质稳定键哈希。
使用 UTF-8 FNV-1a 32 位哈希：初值 `2166136261`，逐字节异或后乘
`16777619`，结果保留低 32 位。直接按索引读取也可用。

- `MMVBoneWeight(vertex,bone)` 返回指定骨骼对源顶点的权重。
- `MMVBoneSubtreeWeight(vertex,bone)` 合并该骨骼与子骨骼的权重。
- `MMVLayerBinding(vertexIndex)` 查询当前渲染顶点的层内绑定编号，无关联时返回 `0xffffffffu`。
- `MMVLayerBoneWeight(vertexIndex,bone)` 和 `MMVLayerBoneSubtreeWeight` 读取当前层权重。
- `MMVNextRigidForBone(bone,after)` 枚举绑定刚体，首次传 `after=-1`。
- `MMVNextJointForBone(bone,after)` 枚举任一端关联该骨骼的 joint。
- `MMVJointAnchorA`、`MMVJointAnchorB` 返回两端世界位置；先检查 joint 有效位。

效果内部自行使用骨骼查询时，可以实现自己的阈值与筛选规则。
效果挂在骨骼行时，还会经过上文定义的骨骼范围合并规则。

<a id="record-mmv-fx-目标页签与依赖"></a>
### 目标、页签与依赖

层页签及骨骼／材质展开规则见[分配界面](interface.md)。参数 GUI 的分组不等于这里的目标页签。

HLSL 的 `MMVInput="Source"` 是历史目标名称，现在读取当前层的冻结上游输入。
它与下文原生计算 ABI 中表示宿主原始数据的 `input.source` 不同。
指定本效果内的另一个 mesh 目标名则读取该目标输出。
`MMVDepends="Mask;Other"` 声明附加依赖。纹理依赖依次绑定至
`MMVTextures[0..15]`，通过 `MMVTextureSample(index,uv)` 读取。
纹理目标用 `Width`、`Height` 声明分辨率，默认 512，单边上限 8192。
`MMVGeometryVS` 是可直接编译的通用几何顶点入口。

依赖目标可有多个读者。自引用、间接循环及递归效果加载都被拒绝。
跨对象查询也读取当前层冻结输入。例如指定另一对象的数据：

```hlsl
string MMVDepends="object[1,0,../accessory/cloth.x]#Main";
```

三个字段依次是对象类别（模型 0、附件 1）、同路径实例序号（从 0 开始）和
相对当前 FX 的对象路径。`#Main` 读取该对象的当前层输入，不触发同层对象 FX。
命名目标仅在已发布的上游数据中解析，不能借依赖绕过同层隔离。
本效果内部的 pass 和目标依赖可以顺序计算，但不得递归进入层调度。

命名目标生成分配页签。页签身份由所属对象、效果实例及目标名共同确定。
`Description` 为说明，`DefaultEffect` 延续 MME 的对象匹配用途，例如
`self=local.fx;*.x=accessory.fx;*=none;`。按从左到右首次匹配执行。
`none` 不绘制该对象，`hide` 隐藏该页上的对象，`main_default` 使用目标自身的绘制。
效果路径相对声明它的 FX。界面的显式移除、禁用可覆盖默认值，恢复默认会删除覆盖项。

传统 DX9 FX 在独立离屏设备中生成纹理。纯 DX12 版保留这些分配并报告不支持；
依赖它的当前层网格回退到上游输出。MMV 中的传统 FX 不直接接管主窗口着色。
最终渲染仍可由 MME 按材质分配其他 FX。

<a id="record-mmv-fx-材质声明"></a>
### 材质声明

新增材质在 FX 加载时以 `MMVMATERIAL` 声明：

```hlsl
float NewMaterial : MMVMATERIAL <
    string Id="author.effect.material";
    string Name="生成材质";
    string NameEn="Generated material";
    float4 Diffuse={1,0.5,0.2,1};
    float3 Ambient={0.2,0.1,0.05};
    string Texture="textures/color.png";
>;
```

`Id` 必须在该对象的效果配置中唯一，不得使用宿主保留的 `source:` 前缀。
贴图路径相对实际声明材质的文件，包含在不同目录的同名 include 也分别解析。
原材质编号保持不变；新标识追加到现有编号之后。删除声明保留空闲编号，
后续重新加入相同标识恢复原编号。FX 应按稳定标识查询，不假定新材质一定在最后。
数值材质参数可写到 `MMVOutputMaterials`；材质身份、名称与贴图描述由配置维护。

示例位于 `assets/MMV/examples`，文件名带 `_mmv.fx` 以避开 MME 的同名自动关联。

<a id="record-mmv-fx-原生结构与状态循环"></a>
### 原生结构与状态循环

本节合并 [循环接口契约](physics.md#record-mmv-deformer-interface-contract) 的作者接口。
二进制字段以 [mmv_deformer.h](../../../sdk/mmv4mms/include/mmv/mmv_deformer.h) 为准。
这里的循环包含同帧逐层传递和跨模拟步反馈，不允许循环依赖反复执行效果图。

| 通道 | 内容与生命周期 |
| --- | --- |
| `Source` | 本次宿主原始定义、动画驱动、原始绑定及表情后蒙皮前几何；只读 |
| `Upstream` | 本层开始时冻结的完整场景，含上层新增结构；同层所有实例共享同一版本 |
| `PreviousCommitted` | 本实例最近成功提交的完整输出；首步及重置后无效 |
| `Candidate` | 本次候选结构、网格与下一步私有状态；其他效果不可见 |

每次推进同时使用新的动画输入和本实例历史，不能把最终渲染网格当作原始几何再次蒙皮。
骨骼和材质分配列表也读取当前层 `Upstream`：本层新增骨骼只能从下一层分配。

<a id="record-mmv-fx-模块声明与加载"></a>
#### 模块声明与加载

```hlsl
float MMVConfig <
    string MMVBackend="DX12";
    string MMVSkinning="DDM";
    string MMVDeformerLibrary="modules/MMVDDM.dll";
    string MMVDeformerResetParameters="RestPose;Subdivision";
>;
```

物理模块使用 `MMVPhysics="Bullet3"`、`"AVBD"`、`"SDS"` 或 `"VBD"`；
重蒙皮使用 `MMVSkinning="DDM"`。一个 FX 只能声明一种原生处理类别。
未注册值、缺失 DLL、错误导出或不支持的能力均报告错误，不回退到另一求解器。
声明可用不等于算法 DLL 已安装；算法模块各自提供构建与验收。

`MMVDeformerLibrary` 相对入口 FX 文件所在目录解析。原生计算 DLL 随 FX 打包，放在同目录或子目录；例如同目录写 `"MMVDDM.dll"`，子目录写 `"modules/MMVDDM.dll"`。未声明时，默认读取 FX 同目录的 `MMV<类别>.dll`，不再从宿主 EXE 或 主插件所在目录查找计算 DLL。DLL 缺失或 ABI 不兼容时报告该路径的错误，不回退到别处的同名计算模块。DLL 自身的私有依赖放在该 DLL 同目录。

`MMV4MMS.dll` 是当前 MMS 渲染插件本体，放在宿主运行目录。上述规则适用于 Bullet3、DDM、AVBD、SDS、VBD 等 FX 附属原生模块。
`MMVDeformerDomain` 指定共享求解域。同类物理默认共享一个域，保留组间碰撞；
DDM 默认每个效果实例独立。共享域必须位于同一层，移层时该域成员一起移动。
`selections` 为每项分配保留对象、骨骼或材质键、效果实例键及参数区间；
共享域不能将所有成员错误地套用第一组参数。
`input.gravity` 提供宿主场景世界重力，使用 MMD 单位，尚未乘各组的 `GravityScale`。

宿主不能提供表情后的蒙皮前几何时，`deformedVertices` 为 `null`，HLSL 的
`MMVDeformedVertexCount()` 返回 0。绑定几何和权重仍可读取；需要蒙皮前几何的
原生求解器应报告缺失能力，不能把绑定姿态当成当前表情结果。

`MMVDeformerResetParameters` 是以分号分隔的参数 ID，仅这些参数变化触发参数重置。
普通参数仍在每次求值时采样。`SimulationHz` 决定固定步长；共享域取最高频率。
`SimulateWhileEditing` 控制暂停编辑时是否推进，窗口查询本身不产生编辑时间。

<a id="record-mmv-fx-c-abi-与候选所有权"></a>
#### C ABI 与候选所有权

计算 DLL 导出 `MMVDeformerGetApi`，使用独立版本 `MMV_DEFORMER_ABI_VERSION=1`。
所有结构先清零，再填写 `structureSize`。字符串为 UTF-8，路径字段 `effectPath` 为宽字符。
跨 DLL 不传 STL、DirectX/Bullet 对象、异常或借用指针的所有权。

| 回调 | 要求 |
| --- | --- |
| `getCapabilities` | 声明读写通道、形状掩码、分配范围与状态标志 |
| `create` | 创建实例，不推进时间 |
| `evaluate` | 计算私有候选；输入指针仅本次调用有效；失败保留已提交内部状态 |
| `commit` | 无分配、无失败地切换候选为已提交状态并释放候选 |
| `discard` | 释放候选，不改变已提交状态 |
| `reset` | 按明确原因清除历史，等待下一份有效输入重建 |
| `destroy` | 释放实例及候选资源 |

每个实例串行调用。候选数组、字符串和诊断由模块持有，到 `commit` 或 `discard` 为止。
运行时在结束候选前深拷贝成功输出；输入和输出不得别名。
速度、约束乘子、接触缓存等属于私有候选事务，不能只回退输出位置而保留已推进的隐藏状态。
仅首次、结构变化或明确重置时重建预处理；统计分开报告预处理和推进耗时。

输出的网格、骨骼、绑定、刚体、joint、形状及表情分别使用：

| 模式 | 意义 |
| --- | --- |
| `Preserve=0` | 继承上游，忽略该通道候选指针 |
| `Replace=1` | 替换整个通道，包括对应扩展表 |
| `Clear=2` | 明确清空该通道；仍必须满足剩余引用的有效性 |

`writeChannels` 必须覆盖实际写入集合且不超出能力声明。原生候选的完整写集合不可拆分。
当后绘制效果已占用其中一个通道时，前者整个候选不生效，历史也不推进。
共享求解域只调用一次 `evaluate` 和一次 `commit`。

<a id="record-mmv-fx-完整结构身份与坐标"></a>
#### 完整结构、身份与坐标

`MmvDeformerObject` 内嵌现有 `MmvObject`，另外提供平行扩展表，避免改变既有数组步长。

| 扩展表 | 关键数据 |
| --- | --- |
| `boneInfo` | 稳定键、来源键、新增标志、绑定/逆绑定/蒙皮矩阵 |
| `rigidInfo` | 稳定键、形状索引、质心局部 3×3 惯量、质心到骨骼局部的绑定变换 |
| `jointInfo` | 两端局部框架、阻尼、关联碰撞、显式跨对象端点键 |
| `shapes` | 球、盒、胶囊、凸网格、复合形状；含资源版本、点、索引与子形状局部变换 |
| `bindingVertices` | 当前层绑定几何，可独立扩容 |
| `deformedVertices` | 当前层表情后、蒙皮前的位置和法线 |
| `bindingWeights` | 最多四项非负且归一化的影响，引用当前层骨骼 |
| `vertexBindings` | 每个渲染顶点到当前层绑定表的映射，与宿主来源编号分开 |

宿主 `originalVertices` 与原始权重表保持来源含义。新增顶点即使
`sourceVertex==0xffffffffu`，仍可通过层内绑定表使用新增骨骼。
DDM 矩阵权重和算法缓存属于实例私有资源，不伪装为 PMX 四项权重。

原项稳定键来自对象身份、类型与源项编号；新增项为
`<效果实例键>/<bone|rigid|joint|shape>/<算法局部键>`。名称与数组索引不是身份。
增加、删除、重排或重绑定必须提供完整重映射并更新 `structureVersion`；
仅姿态、速度变化不更新结构版本。网格修改更新 `meshVersion`。
删除仍被父级、权重、刚体或 joint 引用的项会拒绝整项候选，不自动改成第 0 项。

矩阵沿用行主序、行向量约定。网格在对象空间，骨骼和刚体姿态在 MMD 世界空间。
刚体世界变换以质心为原点，形状顶点与子变换在质心局部空间。
`bodyToBone` 将刚体质心局部坐标转为绑定骨骼局部坐标。
动态刚体要求质量为正、惯量对称正定；静态或运动学刚体允许零质量。
只接受主惯量的适配器必须正确转换主轴及关联形状、joint，或明确报告不支持。

joint 局部框架是定义，世界锚点由 `frameA * bodyWorldA` 和 `frameB * bodyWorldB` 派生。
刚体更新后必须同步锚点，不能继续借用宿主旧 joint 数组。

线性和角度限位逐轴沿用 Bullet 定义：下限大于上限为自由轴，相等为锁定，小于上限为区间限位。输入和输出均保留这些值，不自动交换上下限；非有限数值仍是错误。
普通连接使用本对象稠密刚体索引；跨对象连接明确指定 `(objectKey, rigidKey)`。
跨对象连接与凸/复合形状能否求解，由模块能力声明决定，不能静默降为球或盒。

<a id="record-mmv-fx-时间重置与原子发布"></a>
#### 时间、重置与原子发布

`evaluationId` 标识求值请求，`inputRevision` 区分同一时间轴帧的修改，
`stateEpoch` 区分重置后的历史，`simulationTick` 只按成功提交的固定步数增加。
重复渲染请求复用结果；只读查询不会调用 `evaluate`。
暂停且关闭编辑模拟时不推进；编辑模拟使用实际编辑间隔。单次最多 32 个固定子步，
超出时间通过 `unconsumedSeconds` 报告，不做无限补算或负时间推进。

运行时先验证候选并构建完整层快照，再调用不允许失败的 `commit`。
域内任一对象非法时整域 `discard`：显示使用本层上游，tick 和内部历史保留上次成功状态。
独立求解域可以继续提交。同一动态刚体跨层也只能由一个域推进时间，冲突归属明确报错。
后续层可以读取或修正姿态，不能再次积分同一时间间隔。

首次启用、回跳/循环、场景或模型重置、结构变化、重新启用、移层及指定参数变化
分别传入明确的 reset reason。首版从当前动画输入重建，不自动重演历史。
设备丢失只重置声明 GPU 状态的模块，CPU 历史可保留。
需要物理接管的模块必须等待一份确认已关闭宿主原生物理的输入，不能拿切换前旧姿态初始化。
禁用、失败和最后一个接管者退出都必须释放接管，恢复宿主原来的物理模式。

| 错误码 | 含义 |
| --- | --- |
| `UNSUPPORTED_CAPABILITY=1` | 类别、DLL、范围、形状或其他能力缺失 |
| `INVALID_OUTPUT=2` | 非有限值、容量、索引、父环、权重、质量/惯量、引用或稳定键非法 |
| `SOLVER_FAILED=3` | 算法求解失败 |
| `STALE_INPUT=4` | 输入尚未刷新或所请求的历史快照不存在 |
| `INVALID_ARGUMENT=5` | 调用参数或结构大小非法 |

诊断包含层、对象、效果实例、求解域和出错项键。这些返回码独立于宿主 MMV 返回码。

<a id="record-mmv-fx-hlsl-只读结构资源"></a>
#### HLSL 只读结构资源

现有 `t0..t31`、`u0..u3` 与最终网格输出布局保持不变。骨骼、刚体和 joint SRV
上传当前层记录，新增结构只读资源如下；没有任意骨骼/刚体/joint 写入 UAV。

| 寄存器 | 资源 | 元素字节数 |
| --- | --- | --- |
| t32 | `MMVBindingVertices` | 112 |
| t33 | `MMVBindingWeights` | 72 |
| t34 | `MMVVertexBindings`，前置计数头，必须通过查询函数访问映射 | 4 |
| t35 | `MMVShapes` | 48 |
| t36 | `MMVShapeVertices` | 12 |
| t37 | `MMVShapeIndices` | 4 |
| t38 | `MMVShapeChildren` | 80 |
| t39 | `MMVDeformedVertices` | 24 |
| t40 | `MMVBoneInfos` | 208 |

用 `MMVBindingCount`、`MMVVertexBindingCount`、`MMVShapeCount` 等函数查询长度，
不要依赖 root SRV 的 `GetDimensions` 返回元素数。`MMVLayerBinding` 自动跳过 t34 的计数头。
`MMVShapeAt`、`MMVShapeVertex`、`MMVShapeIndex`、`MMVShapeChildAt`、
`MMVBoneInfoAt` 与 `MMVDeformedVertexAt` 提供边界检查。

<a id="record-mmv-fx-只读查看与零层例外"></a>
#### 只读查看与零层例外

插件可选导出 `MMVGetDeformerInspectionApi`。调用 `querySnapshot` 时指定层、阶段，
可再限定效果实例、域及对象。阶段为 `Source`、`Upstream`、`PreviousCommitted` 或 `Output`。
`Output` 只表示最近成功输出；没有阶段数据时返回 `STALE_INPUT`，不自动运行求解器。
成功查询获得独立 lease，直到 `releaseSnapshot` 前保持有效，即使运行时已经发布下一帧。

结构查看器只读显示骨骼、刚体与 joint，包括稳定身份、来源、绑定、局部框架、世界锚点、
速度、形状、质量/惯量、版本、tick、耗时和诊断。选项按稳定键保存，失效时清空并提示。
它不增加刚体或 joint 的 FX 分配范围，也不编辑宿主模型或物理世界。
所有参数草稿仍属于整个编辑窗口的一次事务：全局「应用」建立新基线，关闭回退之后的修改。

只有第 0 层可显式声明 `MMVDeformerMode="MeshOnly"`，且模块必须提供相应能力。
此模式仅改最终网格，保留上游结构供查看，并清除 `MMV_DEFORMER_STRUCTURE_SYNCHRONIZED`。
它不能作为后续重新蒙皮或骨骼关键帧烘焙的输入。非零层禁止使用该例外。

<a id="record-mmv-fx-分层存档与截层烘焙"></a>
### 分层存档与截层烘焙

层号由 VMM 保存，`.x` 的 `MMVAccessory` 声明不规定层号。
VMM 保存层、绘制顺序、展开许可、展开状态和骨骼/材质模式；启用插件自动保存时，在宿主 PMM 保存成功后写同名 VMM。
对象行右键的层号设置旁，以及顶部层页签右键菜单，都提供「层内描绘顺序」。
使用上移／下移调整当前层的效果；列表靠后的效果优先覆盖。0 层也可调整内部顺序。
顺序改动随窗口顶部「应用」保留；关闭分配窗口会回退上次应用之后的改动。
更改顺序不会让同层效果互读，仍从后向前求值、跳过已经被完整覆盖的输出范围。
单物体预设以 `.vem0` 为入口，读取同名 0 到最大层的 `.vemN`；
保存单物体必须选中 0 层，自动按层导出全部预设。缺附件、骨骼或材质时明确报不兼容，
不强行按旧编号套用。格式细节见 [存储格式](usage.md#record-mmv-storage)。

分层烘焙的独立可选导出为 `MMVGetLayerBakeApi`，定义在
[mmv_layer_bake.h](../../../sdk/mmv4mms/include/mmv/mmv_layer_bake.h)。它使用现有插件实例，不另建宿主插件。
加载支持此接口的 DLL 后，可选择最高层到 0 之间的烘焙终点 x；MME -1 不可选。
计算范围是最高层到 x，包含 x，各层内部仍从最后绘制项开始。
当前 main 未接通以下截层采样和删除事务；本节仅说明可选接口的契约，不作为可操作功能。

`beginSampling(x)` 建立独立采样状态；结束时 `endSampling` 恢复交互求解器。
`stageRemoval` 仅在 `MMV_MESH_BAKE_BEGIN` 到 `COMMIT/CANCEL` 事务内有效，
只暂存被烘焙对象的 `[x, 最大层]` 分配移除。宿主先准备可回滚的关键帧或缓存，
成功提交后才使移除永久生效；失败或取消保留原分配与交互状态。
下层 FX 保留，缓存必须在所选层边界进入后续层，不能污染未烘焙对象的上游输入。
宿主通过 `MmvHostServices.bakedMeshes` 可选尾回调提供本帧的 `MmvBakedMeshInput`，
需先确认插件的 `MMV_CAP_LAYER_MESH_CACHE` 能力位。每条记录包含对象 ID、generation、
层号、同步标志和借用网格；全部指针保持到本次 `evaluate` 结束。无缓存时返回成功、数量 0。
运行时复制后将缓存作为该层最低优先级网格输出：同层 FX 仍读取冻结的 Upstream，
新赋予的 FX 可覆盖缓存，下层读取合并结果。重复对象/层、陈旧 generation、非法网格或
正层缺少结构同步标志会明确报错。只有缓存而没有 GPU FX 时，不要求创建 GPU 设备。
首版网格缓存不保存生成骨骼、刚体等完整结构；不能完整表示结果的情况须拒绝烘焙。
生成结构无法映射为宿主骨骼/表情关键帧、结构与网格不同步或只烘焙部分通道会丢失其他
效果输出时，应明确拒绝相应烘焙，不能静默删结构或丢失输出。

<a id="record-mmv-fx-本机-mmv-处理设备"></a>
### 本机 MMV 处理设备

`MmvHostServices.processor` 是 ABI 1 的可选尾回调，能力位为
`MMV_CAP_GLOBAL_PROCESSOR`。它返回本次启动已生效的 `MmvProcessorInfo`，
不返回下次启动的待生效选择。宿主先解析「跟随主渲染」，再提供具体设备。

| `mode` | `adapterId` | 后端要求 |
| --- | --- | --- |
| `MMV_PROCESSOR_AUTOMATIC` | 0 | 由 MMV 选择可用设备，供没有该服务的旧宿主使用 |
| `MMV_PROCESSOR_GPU` | 指定显卡的 DXGI LUID | MMV 计算与中间绘制使用该显卡，不能静默切换到其他显卡 |
| `MMV_PROCESSOR_CPU` | 0 | MMV 图形与计算使用软件设备，不能用 HAL 执行后标为 CPU |

运行时在创建插件实例时查询一次。设备丢失后的重建保留这次选择，
修改待生效设置不影响当前实例。回调失败、结构长度不足、未知模式或
模式与 LUID 不匹配时，创建失败并报告原因。

该接口描述插件可选的设备服务，不写入 PMM、VMM、VEM 或附件。当前 main 的 MMV4MMS 创建路径未提供该回调，不应据此认为帮助菜单已控制插件内部设备。
物理 FX 的求解算法与分组继续由效果配置决定。CPU 求解器不会因为选择显卡
而自动获得 GPU 实现。主渲染、顶点计算和 MMV 处理的分工见
[运算器设置](../performance/devices.md)。

<a id="record-mmv-fx-接口验证"></a>

## 透视修正与夸张透视

以下效果需在配套插件中提供，处理当帧网格，不修改源模型、骨骼、IK 或原生物理。每次从当前快照求值，暂停重绘不累积变形。

### 适用范围

| 场景 | 建议分配 | 预期结果 |
| --- | --- | --- |
| 面部近景、头部侧转 | 头部骨骼范围，按需与脸部材质相交 | 减轻鼻子、脸颊和后脑的近大远小 |
| 身体接近镜头 | 对象或身体骨骼范围 | 保留锚点大小，减轻身体比例失真 |
| 伸手、挥拳、踢腿 | 指定手掌、手臂或脚部骨骼组 | 靠近镜头时放大指定范围 |
| 武器或附件 | 透视修正可按对象/材质分配 | 无骨骼的附件使用局部坐标锚点 |
| 脸部稳定、手掌前伸 | 脸部修正与手部夸张分别分配 | 两个范围使用各自参数 |
| 同一范围同时处理 | `PerspectiveCombined_mmv.fx` | 先修正，再围绕修正后的中心放大 |

材质与骨骼是两种独立的范围条件。材质范围决定哪些面进入效果，骨骼权重决定这些面上的顶点混合程度。宿主运行时先分离材质子网格，避免共享顶点带动未分配材质。骨骼范围可包含子骨骼，同一权重槽只累计一次，不会因为同时选中手掌和手指而重复放大。

刚体和 joint 仍是对象的只读查询数据，不是独立的分配组。其他 MMV FX 可以根据刚体绑定骨骼或 joint 两端连接点计算遮罩、位置或系数。

<a id="record-mmv-perspective-卡通透视修正"></a>
### 卡通透视修正

分配 `PerspectiveCorrection_mmv.fx`。默认正交程度为 `0.5`、总强度为 `1`。选中骨骼范围时，默认锚点为列表中第一个骨骼；没有骨骼范围时，使用 MMS 当帧源网格包围盒中心。也可明确选择锚点骨骼或对象局部坐标。

| 参数 | 含义 |
| --- | --- |
| `OrthographicAmount` | 0 保留透视；1 达到正交式尺寸关系 |
| `VirtualFovDegrees` | 虚拟垂直 FOV，单位为度；有效值不超过当前主相机垂直 FOV |
| `CorrectionStrength` | 总混合强度，范围 0–1 |
| `AnchorMode` | 自动、指定骨骼或对象局部坐标 |
| `AnchorX/Y/Z` | 指定局部坐标模式下的锚点 |
| `NormalMode` | 保留卡通手工法线，或按变形的 Jacobian 变换法线 |

FOV 与正交程度控制同一个透视保留量。最后编辑哪一个，哪一个成为权威输入；另一项根据当前主相机 FOV 换算。总强度与骨骼权重是独立的混合系数。FOV 为零表示正交极限，不生成零度投影矩阵。主相机已经是正交投影时跳过修正。

令主相机空间中的顶点为 `p=(x,y,z)`、锚点深度为 `z₀`、透视保留量为 `k`：

```text
k = 1 − OrthographicAmount
D = z₀ + k(z − z₀)
q = (xz/D, yz/D, z)
a = CorrectionStrength × 范围权重 × smoothstep(near, 2near, z)
output = p + a(q − p)

k = tan(VirtualFov/2) / tan(MainFov/2)
```

以上公式是 MMV 的保留深度重投影推导。它等价于把虚拟相机沿视线后移并缩小 FOV，使锚点平面的画面大小保持不变，再把结果投回原始顶点深度。`k=1` 恒等；`k=0` 时，屏幕横纵坐标与 `x/z₀`、`y/z₀` 成正比。整个锚点深度平面保持原位置和大小，离开画面中心的锚点也不漂移。所有顶点的原始视深 `z` 保留，模型厚度不被直接压扁。

当锚点位于近裁面后方、相机/对象变换无效或不可逆时，保留分配并返回原因。每个顶点在近裁面附近平滑淡出，避免在视深接近零时放大数值误差。

<a id="record-mmv-perspective-骨骼驱动的夸张透视"></a>
### 骨骼驱动的夸张透视

分配 `PerspectiveExaggeration_mmv.fx` 后，明确选择骨骼范围。没有有效骨骼和权重时保留分配并显示原因，不自动应用到全身。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `ReferenceDepth` | 0 | 0 表示从 MMS 源网格中心取得；正值直接指定主相机空间深度 |
| `Exponent` | 1 | 距离变化的夸张程度；0 恒等 |
| `MaxScale` | 2 | 最大倍率 |
| `ExaggerationStrength` | 1 | 总强度，范围 0–1 |
| `IncludeDescendants` | true | 合并选中骨骼及其子骨骼权重 |
| `PivotBone` | 首个范围骨骼 | 缩放中心；组内只围绕一个中心缩放一次 |

```text
s = clamp((ReferenceDepth / SourceBoneDepth)^Exponent, 1, MaxScale)
a = ExaggerationStrength × 合并范围权重
    × smoothstep(near, 2near, SourceBoneDepth)
output = p + a(s − 1)(p − pivot)
```

远近由主相机空间的深度决定，横向移动不会因为欧氏距离增大而意外缩小。参考深度之外保持原尺寸。靠近近裁面的淡出区会主动减弱效果，因此该极近区域不保证倍率单调上升。实现先比较对数上限，避免直接计算极大幂次溢出。

<a id="record-mmv-perspective-组合与法线"></a>
### 组合与法线

`PerspectiveCombined_mmv.fx` 在一次 dispatch 中固定执行顺序。当前顶点按自己的范围权重接受修正；骨骼组缩放中心按组权重 `1` 接受相同修正，然后才做夸张缩放。所有边界顶点因此共用同一个中心。倍率仍使用源骨骼视深。

保留法线模式原样保留手工法线。变形法线模式使用逆转置 Jacobian 并归一化；Jacobian 包含近裁面淡出对深度的导数，也包含对象与相机空间转换。范围权重按单顶点常量处理，不估计相邻顶点之间的权重梯度。奇异 Jacobian 保留原法线。

效果保留输入拓扑、UV、材质身份和源顶点引用。包围盒由输出网格校验阶段重新计算。阴影使用实际变形后的几何，不单独按灯光视角重算效果。

<a id="record-mmv-perspective-参数与存档接口"></a>
### 参数与存档接口

<a id="record-mmv-perspective-原生参数窗口"></a>
#### 原生参数窗口

在 MMV 分配窗口选中使用上述 FX 的对象或材质行，打开参数窗口。窗口随宿主使用中、英、日语言，提供投影修正、夸张系数、锚点、缩放中心、骨骼多选、子骨骼开关、法线模式和控制物绑定。

编辑正交程度时会换算虚拟 FOV；编辑虚拟 FOV 时会换算正交程度，以最后编辑项作为输入。骨骼列表保留存档中的选择顺序，避免仅打开并确认窗口就改变默认锚点。控制物窗口可选择参数、对象、位置/骨骼/表情通道，并设置增益和偏移。未解析骨骼和控制物保留在列表和诊断中。

“应用”通过运行时事务提交，“确定”提交后关闭；“取消”丢弃尚未应用的编辑。高 DPI 或屏幕高度不足时，内容区域滚动，提交按钮固定在底部。参数随所属效果实例写入 `.vmm/.vem`。

<a id="record-mmv-perspective-作者接口"></a>
#### 作者接口

插件的 `perspective.hpp` 定义 CPU 设置、控制物与 `b1` 常量布局。公共 C ABI 不传递这些 C++ 容器。`Prepare` 只读取 `MmvFrame`，输出独立常量与状态；设置验证、反序列化和标量修改失败时保留旧设置。

FX 明确声明参数模式：

```hlsl
float MMVConfig <
    string MMVBackend="DX12";
    string MMVParameterSchema="Perspective1";
    int MMVPerspectiveKind=1; // 1 修正，2 夸张，3 组合
>;
```

参数属于效果实例，不按 FX 文件名全局共享。存档字段包括版本、效果种类、锚点、范围骨骼索引及名称、子骨骼开关、法线模式、标量参数和控制物。控制物身份由类别、路径和实例序号组成，支持对象位置、骨骼世界位置及表情值，经 `value × gain + offset` 驱动标量。相对路径以当前 `.vmm/.vem` 所在目录为基准；未知字段保留。

范围最多声明 64 个根骨骼；控制物最多 128 项。指数范围为 0–64，最大倍率为 1–1024。载入越界参数失败，运行时控制值失效则跳过受影响效果并返回原因。界面必须显示这些原因，而不能把跳过当成执行成功。

<a id="record-mmv-perspective-实现与验收记录"></a>
