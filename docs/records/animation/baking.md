# 通道烘焙、物理烧录与网格缓存

<!-- 保留已有文档链接的定位入口。 -->
<a id="dense-scale-playback"></a>
<a id="record-physics-baking-validation"></a>
<a id="record-physics-baking-validation-版本与构建"></a>
<a id="record-physics-baking-validation-功能验证"></a>
<a id="record-physics-baking-validation-常规帧处理对照"></a>
<a id="record-physics-baking-validation-范围与限制"></a>
<a id="record-physics-baking-validation-dsh-委派验收"></a>
<a id="record-baking-channels-validation"></a>
<a id="record-baking-channels-validation-来源与证据"></a>
<a id="record-baking-channels-validation-已执行验证"></a>
<a id="record-baking-channels-validation-同条件-dx9mme-回读"></a>
<a id="record-baking-channels-validation-失败记录与边界"></a>
<a id="record-alembic-export-validation"></a>
<a id="record-alembic-export-validation-构建与证据"></a>
<a id="record-alembic-export-validation-功能结果"></a>
<a id="record-alembic-export-validation-性能对照"></a>
<a id="record-alembic-export-validation-边界"></a>
<a id="record-physics-bake-undo-state"></a>
<a id="record-physics-bake-undo-state-原因"></a>
<a id="record-physics-bake-undo-state-修改"></a>
<a id="record-physics-bake-undo-state-构建与运行记录"></a>
<a id="record-physics-bake-preview"></a>
<a id="record-physics-bake-preview-操作"></a>
<a id="record-physics-bake-preview-实现"></a>
<a id="record-physics-bake-preview-验证状态"></a>
<a id="record-bake-preview-and-partial-recording"></a>
<a id="record-bake-preview-and-partial-recording-构建与验证"></a>
<a id="record-bake-channel-select-all"></a>
<a id="record-bake-channel-select-all-验证"></a>
<a id="record-bake-warning-logic"></a>
<a id="record-bake-warning-logic-前后对照"></a>
<a id="record-frame-and-bake-fixes"></a>
<a id="record-frame-and-bake-fixes-修复范围"></a>
<a id="record-frame-and-bake-fixes-对照验证"></a>
<a id="record-frame-and-bake-fixes-构建与证据"></a>

## 选择输出类型

| 目的 | 入口与结果 |
| --- | --- |
| 把物理运动保存为骨骼关键帧 | [物理烧录](../../guides/physics-baking.md) |
| 按固定步长重采样动画通道 | 烧录窗口的通道选择与范围设置 |
| 保存最终几何供工程回放 | [网格烘焙](../../guides/mesh-export.md) |
| 导出其他软件可读取的几何动画 | Alembic `.abc` 导出 |

## 原生物理与通道

烘焙方式默认「原生」。选择 MMV 层之前不要求插件截层接口，也不自动触发删除 FX。物理烧录从第 0 帧预计算，全部模型参与碰撞，只向当前模型目标骨骼写结果，支持选中骨骼和局部范围。

通道选择覆盖相机、灯光、自阴影、重力、附件子参数，以及当前模型的骨骼、表情和显示/IK。范围含两端，步长采样始终包含结束帧；替换区间内结果，保留区间外曲线。容量或候选数据无效时拒绝整项提交。

物理参数及预设用于本次计算，不改变编辑中的实时世界。预览和部分录入的实际操作见[烧录指南](../../guides/physics-baking.md)。

## 原子提交与撤销

取消或计算失败保留原关键帧和物理状态。一次烧录对应一次撤销，包含关键帧、物理禁用标记、相关曲线和末帧。边界恢复帧按需写入，不重复覆盖已有范围外姿态。密集结果仍有播放、内存及保存格式成本。

## 网格来源与存档

当前支持原生几何及相应 MME 导出捕获，MMV 层输出尚未接通。工程缓存与 ABC 文件是独立选择，网格菜单默认写工程缓存，ABC 菜单默认输出文件。清除缓存后恢复实时变形；缓存不等于修改原模型文件。

缓存只能表达其格式支持的数据。不能把材质、额外 UV、描边或可见性变化悄悄丢弃后宣称完整保存；输出拓扑与时间样本也需要有效对应。

## MMV 分层限制

当前 `native_mesh::LayerBakeState` 返回不可用，层采样及删除事务会明确拒绝请求。MMV4MMS 通过整帧纹理接管，没有把指定层的几何、结构和历史接到主程序烘焙入口；带分层截点的缓存也不能按原生缓存回放。

SDK 中保留的层烘焙结构和外部插件的启停轨道，不代表 main 已支持自动替换或删除任意效果。

## 验证入口

通道选择、取消、撤销、容量、MMV 接口和 ABC 输出分别由对应回归覆盖，命令见 [tests/README.md](../../../tests/README.md)。无插件夹具不能代替真实效果执行，单帧几何相同也不能证明整段运动缓存正确。
