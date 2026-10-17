# VMD 导入的共同历史边界与实现方案

本记录保留初始模型与扩展迁移方案。标准全局 VMD 现已完成受控迁移，范围和测试见 [实现记录](abi-vmd-global-2026-10-17.md)。模型 VMD 及有效非空附件扩展仍拒绝，未绑定普通运行保留原有导入功能。以下初始门禁描述不作为标准全局导入的当前状态。

`LoadVmdMotionPath` 在打开文件之前调用 `RefuseBoundVmdImport`。受控绑定同时拒绝 ANSI 路径和 Unicode 路径，说明共同历史测试模式暂不支持 VMD 导入。两种历史记录模式采用相同规则。拒绝不增加历史，也不清空撤销和重做。

## 现有代码的限制

| 位置 | 已有能力 | 迁移需要补齐的能力 |
| --- | --- | --- |
| `src/model/vmd_read_guard.hpp` | 从一次打开的文件捕获不可变字节，校验标准记录边界和帧号 | 生成带类型的导入计划，记录各族偏移、名称映射及扩展状态 |
| `src/model/vmd_load.cpp` | 标准骨骼、Morph、display、camera、light、shadow，及 MMS 扩展 | 当前 `KeyPoolEnsure`、旧撤销环、`Register*` 和扩展应用直接写入当前场景，不能当成私有准备 |
| `native_bone_registration.cpp` | 私有关键帧补丁、池增长、缩放和自由曲线恢复、独立求值 | 暴露接受多条导入记录的构造函数，保留源曲线和帧偏移，统一处理覆盖与插入 |
| `native_morph_keys.cpp` | 注册、删除、范围缩放及目标池采样 | 接受批量权重与扩展曲线的私有计划 |
| `native_display_keys.cpp` | display 关键帧历史与父窗口查询 | 私有 IK 名称映射、可见性与 display 批量插入 |
| `native_global_registration.cpp` | camera、light、shadow 单帧注册的私有计划 | 接受完整导入键值，构造多帧目标池，不从当前预览姿态生成源数据 |
| `bone_curve.hpp` | 捕获、克隆、删除和恢复私有自由曲线 | 在私有 `AuthoringState` 中解码并合并 VMD 自由曲线 |
| `morph_curve.hpp` | 捕获图层及针对目标池采样 | 针对私有完整目标键池解码、重建 VMD 图层，无需临时修改当前模型 |
| `bone_scale_io.hpp` | 解码扩展并直接应用到模型 | 将扩展映射到私有 `bone_scale::AuthoringState`，包括覆盖键的默认线性曲线 |

单帧注册计划只能读取当前姿态，不能通过反复改姿态、注册、恢复来模拟批量导入。导入后再捕获并回滚，也不能保证分配失败时不留下部分修改。

## 拟新增的内部接口

这些接口服务宿主迁移，不属于公开 C ABI。源记录先解码成值，不保留文件句柄。

```cpp
struct VmdImportOptions {
    unsigned modelSlot;
    std::uint32_t frameOffset;
};
struct VmdImportData; // 标准键、Unicode 名称表、扩展数据及解码状态
struct VmdFamilyImportResult; // HistoryChange、最终最大帧、游标及标记

MmsResult DecodeVmdImport(const wchar_t* path, VmdImportData&);
MmsResult PlanBoneVmdImport(MMDApp&, const VmdImportData&,
    const VmdImportOptions&, VmdFamilyImportResult&);
MmsResult PlanMorphVmdImport(MMDApp&, const VmdImportData&,
    const VmdImportOptions&, VmdFamilyImportResult&);
MmsResult PlanDisplayVmdImport(MMDApp&, const VmdImportData&,
    const VmdImportOptions&, VmdFamilyImportResult&);
MmsResult PlanGlobalVmdImport(MMDApp&, const VmdImportData&,
    std::uint32_t frameOffset, std::shared_ptr<HistoryChange>&);
MmsResult PlanAccessoryVmdImport(MMDApp&, const VmdImportData&,
    std::uint32_t frameOffset, std::shared_ptr<HistoryChange>&);
```

每个族在自己的实现文件中复用既有私有补丁与池增长机制。聚合器只保留一个历史节点，由它发布共享最大帧、游标与最终选择标记。不得由多个族分别覆盖共享元数据。

## 准备、发布与重放

1. 捕获模型或全局目标的身份、池身份、当前帧和共同历史 revision。
2. 完整读取一次源文件，解码标准键与受支持扩展。在修改场景前完成模型名确认。
3. 每个族从原键池建立私有目标。重复帧按旧导入的源记录顺序覆盖，绝对帧使用检查过的加法。
4. 在目标池上解析名称与曲线，准备必要的模型和附件资源引用。display 的目标父窗口参与骨骼求值。
5. 为前后两个方向保留全部键值、扩展、姿态及元数据。记录的内存预算包括容器容量、补丁、曲线和资源所有权。任何计划失败，直接丢弃私有对象。
6. 调用 `PrepareNative`，意图为 `ExplicitCommit`。准备所有族的池增长、图层恢复和目标求值，再校验全部 `IsCurrent`。
7. 在不分配内存、不派发窗口消息的发布阶段提交全部参与者，形成一条共同历史。之后刷新界面和依赖数据。

撤销与重做从记录中的值准备目标，不再打开 VMD 文件。播放头保持当前位置，姿态根据目标轨道在该位置重新求值。切换模型不影响记录所属对象。对象身份失效时拒绝重放并保持历史游标。

两种历史记录模式都记录导入，因为导入是显式提交。无匹配键、或结果完全相同的导入不生成节点，也不丢弃已有重做。

## 分批实施范围

先实现标准 camera、light、shadow 的完整批量计划。遇到有效附件扩展时，这个范围必须明确拒绝，不能只导入标准部分而静默丢弃附件。

随后实现模型骨骼、Morph 与 display 的聚合计划。骨骼缩放、自由曲线和 Morph 曲线各自补齐私有解码后，再接受这些扩展。尚不支持的有效扩展应在准备阶段拒绝。旧普通路径对未知或损坏扩展的处理保持原有规则。

## 验收范围

本批新增 `NativeVmdImportChecks`，由总测试入口接入。它检查未绑定允许继续、两种模式与两种编辑目标均拒绝，并通过自动关闭的实际提示框验证 ANSI 与 Unicode 入口在不存在的源文件上先显示迁移原因。拒绝前后比较当前应用数据、模型摘要、选择、旧撤销环和共同历史计数。专项 105 项通过，见 `build/abi-frame-paste-vmd-3.log`；完整整合回归见 [本批记录](abi-frame-lines-paste-2026-10-17.md)。这些结果只证明入口保护，不表示导入已迁移。

真正迁移时还需要验证以下场景。

- VMD 1、VMD 2、完整与截断可选节、Unicode 名称、重复帧和帧上限。
- PMX 与 PMD，IK、可见性、曲线、缩放、附件，以及覆盖既有关键帧。
- 分配失败逐点注入，包含扩展解码、私有池增长、目标求值和历史预算不足。每次失败都保持场景、选择、旧环、历史游标和重做。
- 准备后切换模型、改变帧、修改原数据或替换对象身份，提交应拒绝。
- 导入后删除源文件，仍可跨模型撤销与重做，并保持当前播放头。
- 保存再加载后的键值及扩展与预期一致。两种模式均只有一次导入提交。

本方案只用现有 CPU 数据结构与 Win7 已支持的系统调用，不增加特定显卡或新版系统依赖。Win7 和 AMD 真机验收仍需单独记录。
