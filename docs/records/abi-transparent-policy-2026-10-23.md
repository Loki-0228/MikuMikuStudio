# 透明排序 CPU 策略

2026-10-23。ABI 基线 `7da5d2e`，开始及最终产物核对时 main 为 `fed9768f9fe726c4b5e068849808803f1713ad42`。行为来源为 main `3782e0c`。本批只完成 CPU 策略和独立验证，生产渲染器仍调用旧三参数 `Order`；菜单、工程字段及真实绘制尚未迁移。

## 新入口与旧入口

内部 C++ 入口 `OrderWithPolicy` 将排序范围、是否切分和是否保留屏幕外面分开表达。它不是插件 C ABI。

- `crossObjects=false` 保留输入的连续物体组顺序，各组单独排序；`true` 排列全部输入。先确定组边界再过滤非法面，避免中间物体被过滤后合并两段原本分开的物体。
- Low、Standard、High 分别使用 16、64、256 个稳定深度桶。桶内保留源顺序。有效 `linearDepth` 表示完整源面的线性深度，未提供时回退到投影 Z 之和。有限深度跨度先求比例，仅跨度溢出时使用半值计算，避免微小跨度倒数溢出。
- 各组全部有效源面参与深度边界计算，包括屏幕外面。可见面重排后填回原可见槽位；根据 `retainOutside` 保留屏幕外槽位或压缩删除，不产生尾部重复面。
- Maximum 不切分时按投影深度精确排序，同深度按原 ordinal 规则处理。它接受有效的未裁剪完整源面，忽略 outside 标记和 `retainOutside`，保留原索引。切分模式要求预裁剪输入，复用原 BSP 核心及属性插值；分片中的索引仅追踪来源，不能当作新顶点编号绘制。
- Off 完整返回输入。未知精度对非空输入保序并标记 limited。新入口在私有结果完成后才发布统计，分配异常保留调用者统计和输入。

`FrustumOutsidePlanes` 返回 D3D 齐次六平面位掩码。调用者须对三点掩码取交集，才能认定整面位于同一平面之外。边界容差及非有限输入保留，不能用单个顶点越界代替整面判定。

旧 `Order` 保留 High 精确全局投影深度、Low 16 桶和 Standard 64 桶。新旧入口共用 `MaximumSplit`，不维护两份 BSP。共享后的三处预算或无进展回退遇非有限深度时改为保序并标记 limited，旧实现会继续调用比较排序；正常预裁剪输入不触发该边界。

## 内存与数值边界

`Triangle` 增加 object、flags、sourceIndices、linearDepth 和显式初始化的 reserved。目标 x64 的实测断言为 Vertex 128 字节、Triangle 424 字节，旧 Triangle 为 392 字节。新增 32 字节填入完整布局；两块预先写入不同字节的存储经默认构造后字节一致，包含固定 NaN 哨兵，避免新增隐式空隙扰动旧渲染器的整块字节缓存比较。

每面增加 32 字节是实际内存成本，多个输入、排序缓存及 BSP 副本会进一步放大。现有四百万额外面数预算不是全场字节预算，后续渲染接入必须重新核对峰值。不能据本批宣称性能或最大场景容量不变。该结构不属于持久化格式或公共插件 ABI。

新增大坐标幅值检查只提供额外整面回退，不保证所有有限输入的中间运算都安全。BSP 仍要求预裁剪坐标；旧插值公式在极端有限 inverseW 比值下的舍入风险尚未独立修复或验证，继续列为待处理数值边界。

## 验证

证据目录为 `build/verification/abi-transparent-policy-20261023`。测试只调用本分支 CPU 实现，不创建窗口、不使用显卡、不加载第三方插件。每次复制到独立目录运行。

| 验证 | 结果与证据 |
| --- | --- |
| 新策略专项 | `policy-full-1`、`policy-full-restored` 各 1,945 项通过。 |
| 原有透明几何全集 | `legacy-full-1`、`legacy-full-restored` 均输出 `Transparent geometry regressions passed.`，退出 0。 |
| 两配置产品 | `build-full-restored.json`、`build-original-restored.json` 均退出 0。原始配置未开启 EFFECT_TESTS，本批 CPU 专项只在完整配置构建运行。 |
| 强制跨物体反例 | `counter-global-scope` 在作者连续组顺序断言失败，退出 1。 |
| High 退回 64 桶反例 | `counter-high-64` 在独立桶数量断言失败，退出 1。 |
| 忽略外侧深度边界反例 | `counter-visible-bounds` 在可见面桶内稳定顺序断言失败，退出 1。 |
| 提前发布统计反例 | `counter-early-stats` 在真实分配失败保持统计的断言失败，退出 1。 |
| 恢复及审查 | `candidate.json` 记录四个候选文件，四个反例均恢复原字节，重建并复验。`review-design.txt`、`review-production.txt`、`review-tests.txt` 保留独立审查，最终整合审查另存 `final-review.txt`。 |
| 产品兼容边界 | `pe-check-final.json`：两产品 x64、子系统 6.1、导入表无增减，效果资源 117 和 118 不变。 |

专项采用独立固定桶、源面字段及重心插值期望，覆盖所有精度的物体范围、同桶稳定性、次正规深度跨度、全源深度边界、外侧槽位、六平面、NaN 和无穷值。两组真实相交平面分别切分一次，检查合计两次、六片、各源面面积及属性；多个范围的外侧计数累加，桶数取最大值。

七组真实 `operator new` 故障扫描分别覆盖桶排序、保留槽位、BSP、整面 Maximum、Off、未知精度及多个物体组。每个预算点都在目标调用外关闭注入，再断言输入及调用者统计不变；后续范围失败不能泄漏前一个范围的统计。

DSH 会话 `0062fda0-6a67-40cc-b5f0-8297427dd009` 通过已安装的 `dsh-delegate` skill，仅修改 `src/render/transparent_geometry.cpp`，返回 cwd 为 `D:\MMD-Next\MMS-abi-dev`。正式结果 completed、sawError=false；输出仍记录了已恢复的命令包装及 skill 调用错误，不能将该标志解释为过程中没有错误。Codex 完成契约、测试、接入构建、审查及文档。

最终增强产品 SHA256 为 `1ea71d5f8f0bb0acc4103ae39be6b15ab62f696789d2a5aef18070414583762b`；原始配置产品为 `8a5e450fe63564a8a6874d2b308416354718e114e27e51032bc990d2f404d660`，与本批开始时相同。稳定版 `build/release/MikuMikuDance.exe` 保持 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。

## 后续接入

下一批须同时完成渲染器源 VB 与私有 IB 路径、范围开关、MMS 工程字段及真实绘制验证，才能认定透明排序行为已同步。还需按范围约束 peeling、材质批次与失败回退，明确外部效果的多段材质提交能力，以及 D3D12 可选旁路的设置支持。缓存生命周期及实际性能单独验收。

本批 CPU 字段检查不证明真实顶点身份、MME、像素结果、多视口、Win7 或 AMD 真机兼容。普通公共 `mms.edit` 仍关闭，其他 ABI 缺项继续保留。
