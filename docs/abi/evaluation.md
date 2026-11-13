# 场景求值、骨骼、网格与材质

[返回功能总览](README.md)

## 当前推进范围

### 独立场景求值剩余能力

SDK1.7已有骨架、网格、材质及引用；旧文档说未蒙皮不再直接适用。QDEF/PMX2.1、flip/impulse、物理及非模型对象仍有边界。

验收要求：按能力版本分拆；冻结输入与独立帧；不改变主播放头/历史；源删除后稳定；未实现明确UNSUPPORTED，不能把枚举当支持。

依赖：ABI-01。任务标识：ABI-06。

## 相关专题

- [SDEF 独立 CPU 数学上下文](../records/abi-sdef-private-context-2026-10-27.md)
- [公开同源材质数值 1.6](../records/abi-public-material-2026-10-27.md)
- [公开父级状态求值](../records/abi-parent-state-2026-10-27.md)
- [公开网格场景 1.5](../records/abi-public-mesh-2026-10-27.md)
- [公开骨架场景帧](../records/abi-public-scene-frame-2026-10-27.md)
- [公开骨架场景帧接入方案](../records/abi-public-scene-frame-plan-2026-10-27.md)
- [冻结显示与外部父级选择轨道](../records/abi-frozen-display-motion-2026-10-27.md)
- [冻结骨骼通道的公开求值服务](../records/abi-evaluation-service-2026-10-27.md)
- [同源拓扑与跨模型网格验证](../records/abi-mesh-scene-topology-2026-10-27.md)
- [场景 C++17 封装](../records/abi-scene-sdk-2026-10-24.md)
- [显示通道的公开求值服务](../records/abi-display-evaluation-service-2026-10-27.md)
- [独立 CPU 网格求值](../records/abi-mesh-evaluation-2026-10-27.md)
- [独立 CPU 表情求值](../records/abi-morph-evaluation-2026-10-27.md)
- [独立持有表情求值载荷](../records/abi-morph-definition-snapshot-2026-10-27.md)
- [独立持有骨架定义](../records/abi-bone-definition-snapshot-2026-10-27.md)
- [独立材质场景结果](../records/abi-material-scene-2026-10-27.md)
- [独立网格场景来源](../records/abi-mesh-scene-2026-10-27.md)
- [独立网格定义快照](../records/abi-mesh-definition-snapshot-2026-10-27.md)
- [独立骨架尺度计算](../records/abi-private-bone-scale-2026-10-27.md)
- [独立骨架工作区装配方案](../records/abi-owned-bone-workspace-plan-2026-10-27.md)
- [独立骨架帧求值](../records/abi-owned-bone-evaluation-2026-10-27.md)
- [独立骨架求解上下文](../records/abi-bone-transform-context-2026-10-27.md)
- [离散采样的精确帧边界与真实 IK 轨道验收](../records/abi-display-rational-boundaries-2026-10-27.md)
- [网格场景时间与真实文件验证](../records/abi-mesh-scene-host-2026-10-27.md)
- [表情通道的公开求值服务](../records/abi-morph-evaluation-service-2026-10-27.md)
- [跨模型骨架场景求值](../records/abi-bone-scene-evaluation-2026-10-27.md)
- [骨架场景 SDK 与能力边界](../records/abi-scene-sdk-2026-10-27.md)
- [骨架场景精确时间](../records/abi-scene-exact-time-2026-10-27.md)

## 实现与验收依据

以下保留原总览中的实现、失败和限制记录；其中日期、版本和“当前”均属于原记录上下文，不能替代上方的当前推进范围。旧执行方式与任务指令不适用于本轮工作。

2026-10-27 更新：`mms.scene.evaluate` 1.6 增加同源材质数值及稳定身份，提供 C 与 C++ 接口；真实 PMX/PMD 插件及双配置验证见[材质记录](../records/abi-public-material-2026-10-27.md)。纹理采样、最终渲染及物理仍未实现。

2026-10-27 更新：`mms.scene.evaluate` 1.5 已公开同源网格和 CPU 变形顶点，并提供 C++ 封装；拓扑需保留来源句柄。真实三模型插件、双配置契约与生命周期验证见[本批记录](../records/abi-public-mesh-2026-10-27.md)。它尚不提供独立物理、最终材质、纹理或多视口渲染。

[2026-10-27 独立网格定义](../records/abi-mesh-definition-snapshot-2026-10-27.md)：内部快照冻结 PMX/PMD 绑定顶点、活动权重、UV 和材质分段，源删除后可并发读取。专项 805 项、骨架和表情回归及真实 PMX/PMD 宿主通过；QDEF/未知权重明确拒绝。独立蒙皮与公开几何帧仍待实现，后续阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 公开骨架场景帧](../records/abi-public-scene-frame-2026-10-27.md)：`mms.scene.evaluate` 1.4 可冻结全部模型及 BIND 身份，并在后台发布独立骨架帧；与旧通道共享句柄、额度和停止保护。双配置场景专项各 617 项、DLL 生命周期各 602 项及 ABI 回归通过，真实三模型身份、目标动作、精确时间、并发和 PMM 生命周期通过。蒙皮、历史物理、独立视口及便利封装仍待实现；正式 release 未覆盖，后续阴影同步暂缓。

[2026-10-27 骨架场景精确时间](../records/abi-scene-exact-time-2026-10-27.md)：独立骨架求值区分连续插值与精确离散键，修复分数时间舍入导致外亲、IK 等状态提前切换。两配置时间专项各 410,948 项通过；骨架场景、运动及真实 PMX/PMD/三模型回归通过。公开场景帧仍待实现，接入和验收要求见[方案](../records/abi-public-scene-frame-plan-2026-10-27.md)。正式 release 未覆盖，后续阴影同步暂缓。

[2026-10-27 公开父级状态](../records/abi-parent-state-2026-10-27.md)：`mms.scene.evaluate` 1.3 追加继承、地面和模型目标三种状态，修复地面及未来地面键捕获失败。新旧接口共用来源、额度和生命周期；旧入口遇地面明确拒绝且保留输出。双配置专项各 300 项、DLL 生命周期各 585 项、旧通道和 ABI 回归通过，真实 PMX/PMM 的状态、身份及跨删除读取通过。公开帧、蒙皮、物理和多视口仍待实现；main 无新提交，后续阴影暂缓，正式 release 未覆盖。

[2026-10-27 跨模型骨架场景](../records/abi-bone-scene-evaluation-2026-10-27.md)：`BoneSceneSource` 捕获完整目标动作与宿主对象身份，按原计算顺序完成全场两轮求值，支持真实工作区之间的传递、循环和自引用。专项 10,377 项、单模型回归 10,043 项、真实三模型及 PMX/PMD 生命周期验证通过，两产品构建通过。公开帧、蒙皮、物理和多视口仍待实现；审查发现公开 1.2 通道的地面父级表达缺口，已单列待修。main 无新提交，后续阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 独立骨架帧](../records/abi-owned-bone-evaluation-2026-10-27.md)：`BoneEvaluationSource` 同次捕获五类数据，在自有工作区执行局部动作、表情、两轮层级与 IK、完整尺度，返回拥有独立寿命的内部帧。专项 10,043 项、采样 10,204 项、表情 873 项和姿态准备 385 项通过，真实 PMX/PMD 的原生对照、PMM 重载、删除及槽位复用通过，两产品构建通过。跨模型场景调度、公开帧、蒙皮、物理和多视口仍待实现。main 阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 独立骨架尺度计算](../records/abi-private-bone-scale-2026-10-27.md)：内部 `BuildTransformsPrivate` 与原入口共用完整矩阵和父链算法，只消费显式输入，不读取宿主注册表或当前矩阵。专项 198 项、原生尺度与 PMM/VMD、物理尺度及 10,204 项采样回归通过，两产品构建通过。补充工作区装配和分阶段外部依赖方案；完整场景帧仍待实现。main 阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 独立骨架求解上下文](../records/abi-bone-transform-context-2026-10-27.md)：内部 `BoneTransformContext` 独立持有 CPU 数学函数，私有工作区复用原五阶段，隔离全局数学表、尺度缓存和诊断。双配置专项及重构前源码对照各 2,777 项通过；寿命、故障、并发、诊断与既有回归通过，两产品构建通过。尚未装配完整场景帧或新增公共 ABI；main 阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 独立 CPU 表情求值](../records/abi-morph-evaluation-2026-10-27.md)：内部 `MorphEvaluationFrame` 将冻结定义与直接权重计算为稀疏位置、五族 UV、骨骼偏移和材质结果，保留原生组贡献、四元数和加乘顺序。专项 873 项含真实原生逐位对照；定义 1,466 项、权重 922 项、原生运行及真实 PMX/PMD 关键帧到结果的后台生命周期验证通过，两产品构建通过。尚未接入公开场景帧，不包含 IK、蒙皮或最终渲染；main 阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 独立表情求值载荷](../records/abi-morph-definition-snapshot-2026-10-27.md)：内部 `MorphDefinitionSnapshot` 持有 PMD 顶点和 PMX 0–8 类表情载荷，以及顶点、UV、材质恢复基准。专项 1,466 项、表情权重 922 项、骨架 372 项及真实 PMX/PMD 删除重载验证通过，两产品构建通过。未生成最终形变或新增公共 ABI；翻转、冲量表情仍缺加载和求值实现，捕获明确拒绝。main 阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 独立骨架定义](../records/abi-bone-definition-snapshot-2026-10-27.md)：内部 `BoneDefinitionSnapshot` 深拷贝骨架、PMX 原链与运行 IK 链、selector 映射，保留顺序及类型语义，源销毁后并发读取。专项 372 项、骨骼动作 973 项、显示轨道 4,426 项、真实 PMX/PMD 及 PMM 生命周期通过，两产品构建通过。审查发现的 PMD 未启用字段过严校验已用失败反例修复。尚未生成或公开完整姿态，main 阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 公开显示通道求值](../records/abi-display-evaluation-service-2026-10-27.md)：`mms.scene.evaluate` 1.2 追加显示状态、IK 开关及外部父级选择采样，保留旧函数前缀，共用原身份、额度和停止保护。父级身份在捕获时固定，目标删除及槽位复用不改变旧源。双配置显示专项各 1,860 项、DLL 生命周期各 496 项及既有回归通过，两产品构建通过。真实 PMX/PMM 宿主验证可见状态及父级寿命；真实含 IK 链模型验收、完整 IK/世界姿态和物理仍待完成。main 无新提交，阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 公开表情通道求值](../records/abi-morph-evaluation-service-2026-10-27.md)：`mms.scene.evaluate` 1.1 支持骨骼、表情或组合捕获，共用原身份、额度及源租约；1.0 布局与骨骼创建语义保持不变。双配置存储各 4,036 项、通道各 1,706 项、真实 DLL 生命周期各 289 项和 ABI 44,806／44,818 项通过，两产品构建通过。真实 PMX/PMM 宿主验证通道隔离、后台权重读取、删除及重载寿命；不改变播放头及编辑状态。表情形变、显示、层级/IK/物理及完整多视口仍待实现。main 无新提交，阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 公开冻结通道求值](../records/abi-evaluation-service-2026-10-27.md)：`mms.scene.evaluate` 1.0 接入生产插件运行时，提供稳定身份、独立源租约、CPU 后台采样及额度和卸载保护。双配置存储各 4,034 项、真实 DLL 生命周期各 160 项，ABI 回归 44,818／44,806 项和两产品构建通过；真实 PMX 宿主验证旧源跨编辑、删除及工程重载仍保持数据。仅局部已注册骨骼通道，完整场景帧、层级/IK/物理和多视口仍待实现；原始兼容产品未开放该服务。main 新阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 骨架场景 SDK](../records/abi-scene-sdk-2026-10-27.md)：新增 C++17 来源与帧封装，沿用原 C ABI、身份、额度与句柄；显式释放失败可重试。未实现的翻转及冲量表情现在明确拒绝。双配置 SDK 各 463 项、场景契约各 617 项、真实 PMX/PMD 拒绝与三模型 SDK 宿主通过。PMX 2.1/QDEF、蒙皮和视口仍待实现；正式 release 未覆盖，后续阴影同步暂缓。

[2026-10-27 离散帧边界及真实 IK 轨道验收](../records/abi-display-rational-boundaries-2026-10-27.md)：修复有理时间浮点舍入导致显示、IK 开关及外亲提前换键的问题，连续骨骼和表情采样不变。先在旧实现复现失败，修复后双配置显示各 2,042 项及存储、通道、DLL 生命周期回归通过。真实含 IK 链 PMX 的身份、字节、局部父级及跨删除读取通过，两产品构建通过。完整 IK/世界姿态仍待实现，main 阴影同步暂缓，正式 release 未覆盖。

[2026-10-27 冻结显示及外部父级选择](../records/abi-frozen-display-motion-2026-10-27.md)：内部 `DisplayMotionSnapshot` 深拷贝显示字节、IK 开关、骨骼映射和外部父级连续窗口，源销毁后可并发无分配采样。专项 4,426 项、显示编辑回归 4,825 项、骨骼 973 项、表情 922 项及真实 PMX/PMM 生命周期验证通过，两产品构建通过。该内部快照批次尚未公开显示 C ABI；后续 1.2 服务见上条。IK/世界姿态仍待实现；main 无新提交，阴影同步暂缓，正式 release 未覆盖。
