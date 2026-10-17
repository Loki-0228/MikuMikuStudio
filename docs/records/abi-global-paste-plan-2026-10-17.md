# 全局关键帧粘贴的私有计划

本记录核对 `d8a0d17` 的源码，说明命令 421 在镜头模式下的后续迁移。当前受控入口仍拒绝这条路径。本文没有实现或验证全局粘贴。

## 旧路径的具体行为

源码位置以本记录核对时的行为为准，后续编辑会改变行号。

| 位置 | 行为 |
| --- | --- |
| `src/window/command_control_400.cpp:2063`，`PrepareFramePaste` | 校验相对帧，并直接扩展四个全局池和涉及的附件池。共同历史不能调用它做私有准备 |
| `src/window/command_control_400.cpp:2300` | 镜头模式先检查 camera、light、shadow、gravity、accessory 五种数量，全空返回 |
| `src/window/command_control_400.cpp:2316` | 清除四个全局池及全部 255 个附件池的选中标记 |
| `src/window/command_control_400.cpp:2329` | 按 camera、light、shadow、gravity、accessory 顺序，在各族内部按剪贴板顺序注册 |
| `src/window/command_control_400.cpp:374`，`InsertGlobalFrame` | 绝对帧为当前帧加源相对帧。同帧覆盖，其他情况插入空槽。只有新插入提高 `lastRegisteredFrame`，覆盖不提高 |
| `src/window/command_control_400.cpp:433`，`FillCameraFrame` | 复制完整 24 字节插值、相机父模型和父骨骼，并选中新键。不得用当前相机预览值替代源值 |
| `src/window/command_control_400.cpp:461`，`FillGravityFrame` | 复制强度、方向、噪声和噪声开关，并选中新键 |
| `src/window/accessory_paste.cpp:202` | 命令 421 按源 `slot` 定位附件，使用 `frameOffset`。同帧覆盖不提高水位。位置、旋转、显示、阴影、父关系、缩放及透明度都进入键 |
| `src/window/command_control_400.cpp:2389` | 完成后重置物理，并刷新场景及各轨道。共同历史应在发布阶段外完成界面刷新 |

五种剪贴板记录均为值记录，没有 display 剪贴板的嵌套指针。camera 的父模型、父骨骼以及附件源槽仍有对象身份依赖。附件缺失时，单条 `PasteAccessoryKeyRecord` 会跳过，但命令 421 的 `PrepareFramePaste` 会预先拒绝缺失槽。因此新命令计划也应拒绝整次操作，不能静默跳过一部分附件。

## 不能直接聚合现有单键注册记录

`native_global_registration.cpp:232` 的 `Registration<K>` 同时拥有键补丁、`GlobalKeySelection`、`lastRegisteredFrame` 和当前预览姿态。它只适合单族单次注册。

`Candidate::IsCurrent` 在约 257 行校验全部选择与水位，约 268 行的 `Publish` 又会调用这项校验。如果把多个现成注册记录直接组合，第一个记录清除全部选择或提高水位后，后一个记录就会因校验失败终止进程。

附件的 `Registration` 有相同约束，见 `native_accessory_registration.cpp:142`、约 184 行和约 199 行。现有 `PlanAccessoryRegistration` 只接受七个数值控件生成的当前姿态，不接受完整附件剪贴板记录，也不能直接承担批量粘贴。

共享基础可以复用，但需要拆出只拥有本族字段的参与者。

- `native_global_registration.cpp:38` 的 `GlobalKeySelection` 可由顶层记录持有一次。
- 约 135 行的 `Growth<K>` 已能在私有池中准备增长，并保持原池身份。
- 约 220 行的 `Parent` 可保留和校验相机父关系。
- `curve_host::TrackAuthoring` 可针对最终私有键池准备自由曲线衍生结果，不必临时替换当前池。
- 附件约 76 行的 `Parent`、约 97 行的 `Growth`、私有 `Chain` 与 `NativePose` 可以按附件槽复用。

## 内部接口边界

建议新增一个聚合记录，而不是给公开 C ABI 增加专用粘贴接口。

```cpp
struct GlobalPasteSource {
    std::vector<mdl::CameraClipboardRecord> cameras;
    std::vector<mdl::LightClipboardRecord> lights;
    std::vector<mdl::ShadowClipboardRecord> shadows;
    std::vector<mdl::GravityClipboardRecord> gravity;
    std::vector<mdl::AccessoryClipboardKey> accessories;
    std::uint32_t frameOffset;
};
struct GlobalPasteFamilyResult {
    // 本族键、曲线、父关系和求值结果。不得拥有共享选择或水位。
    std::unique_ptr<GlobalPastePart> part;
    std::uint32_t maximumInsertedFrame;
    std::vector<std::uint32_t> pastedSlots;
    bool dataChanged;
};

MmsResult CaptureGlobalPasteSource(MMDApp&, GlobalPasteSource&);
MmsResult PlanCameraKeyPastePart(MMDApp&, const GlobalPasteSource&,
    GlobalPasteFamilyResult&);
MmsResult PlanLightKeyPastePart(MMDApp&, const GlobalPasteSource&,
    GlobalPasteFamilyResult&);
MmsResult PlanShadowKeyPastePart(MMDApp&, const GlobalPasteSource&,
    GlobalPasteFamilyResult&);
MmsResult PlanGravityKeyPastePart(MMDApp&, const GlobalPasteSource&,
    GlobalPasteFamilyResult&);
MmsResult PlanAccessoryKeyPastePart(MMDApp&, unsigned accessorySlot,
    const GlobalPasteSource&, GlobalPasteFamilyResult&);
MmsResult PlanGlobalKeyPaste(MMDApp&, std::shared_ptr<HistoryChange>&);
```

`GlobalPastePart` 需要自己的私有候选接口。候选先校验，再以无分配的 `PublishOwned` 交换本族数据。它不应在其他族发布后重新校验全局共享字段。顶层 `PreparedHistoryChange` 校验一次全部候选，随后提交各族已经准备的目标。

允许将这些函数留在已有注册实现文件内部，由一个对外的 `PlanGlobalKeyPaste` 编排，避免把私有池和曲线细节暴露到更多头文件。

## 私有准备与共享字段

先复制全部剪贴板值并检查数量、指针、帧上限、浮点值及目标身份。每个族在私有稀疏补丁或目标池中模拟源顺序。同族同帧的后条源记录覆盖前条。自由槽从索引 1 扫描，frame 0 始终覆盖头记录。camera 填充全部 24 字节插值，保留目的键中旧函数未覆盖的保留字段。

当前模板的 `Pool<K>`、`TrackKind<K>`、`Pose` 只覆盖 camera、light、shadow。不能直接把 `GravityKey` 代入，其默认分支会误选 shadow。增加 gravity 时必须显式补齐池、曲线种类、姿态结构、原生采样与应用。

聚合器拥有以下共享内容。

| 内容 | 所有权与发布规则 |
| --- | --- |
| 全局与附件选择 | 捕获一次 `GlobalKeySelection`。前向清除原范围，并只选中最终粘贴槽。撤销恢复原标记。新增长尾部槽的标记也由聚合器显式恢复 |
| `lastRegisteredFrame` | 保存前值，后值取前值与各族新插入帧的最大值。覆盖键不提高水位。顶层发布一次 |
| 当前帧 | 捕获准备时的帧，首次提交校验。撤销重做在执行时的当前帧求值，不移动播放头 |
| 资源 | 保留涉及的附件和父模型，记录对象代次、骨骼表及池身份。池增长允许，池替换或对象代次变化拒绝 |
| 修改状态与物理 | 非空提交后标记修改并请求物理重置。界面与 GPU 状态更新放在历史事务结束后 |

建议发布顺序为：统一校验、提交已准备的池增长、写本族键和曲线及求值姿态、统一写选择、统一写水位。选择发布前的键写入需保留原标记，随后由聚合器覆盖最终标记。所有参与者都不能调用会读取半发布共享状态的旧注册函数。

没有实质键值变化时，不清选择、不改变水位、不标脏、不创建节点，也不丢弃重做。比较应排除仅由本次选择形成的差异，但包含插值、父关系、缩放、透明度等实际键内容。两种历史记录模式都以 `ExplicitCommit` 记录一次粘贴。

## 求值不能只在首次提交后调用旧刷新链

原生粘贴会在当前帧重新求值。私有计划应预先求出受影响轨道的前向结果，并在撤销重做时重新准备目标值。不能在发布后才调用可能分配、弹框、改场景或重新求值全部模型的旧函数来补齐结果。

gravity 的求值需专门实现。`src/model/track_apply.cpp:101` 的 `ApplyGravityTrack` 会写当前重力参数并刷新物理对话框，不能作为私有采样。它对噪声做整数截断，强度和方向按原生浮点顺序插值。约 51 行和约 163 行还从 `noiseEnabled` 开始复制四字节到 `GravityNoiseModeWord`，其中包含 `selected` 与保留字节。新路径需明确保留这个兼容行为的范围，并用旧实现对照，不能只按单字节开关猜测状态。

附件原生求值在 `src/window/accessory_paste.cpp:125`。位置、旋转、父关系、显示和阴影按前一键保持，只有缩放和透明度在两键之间线性插值。自由曲线存在时，再由已准备的曲线目标提供对应结果。

## 实施顺序和失败边界

第一批可以完整实现 camera、light、shadow 三族混合粘贴。遇到非空 gravity 或 accessory 剪贴板时，受控入口继续拒绝整次操作。不得先提交已支持三族，再跳过未支持部分。

第二批补齐独立 gravity 参与者。第三批加入按源槽分组的附件计划。最后才允许五族任意组合。每批保持普通未绑定运行不变，生产共同历史绑定仍需等所有其他编辑入口迁移完成。

每个阶段都需要测量保留的键补丁、曲线作者、向量容量、父关系记录和对象所有权。目标候选的临时池分配可能失败，但不能因此改变当前池。撤销重做不得重新读取剪贴板或原文件。

## 必需验收

- 三族或五族混合后只有一条共同历史，前后选择与水位均准确。首族提高水位或清选择不会使后族终止。
- 同帧覆盖、重复源、无变化、frame 0、原生空槽保留字段以及完整池增长。
- 相机父骨骼、附件父骨骼、附件删除后槽重用，以及同身份增长和不同身份替换。
- 原生当前帧求值、后续改变播放头再撤销重做、自由曲线存在及不存在。
- 源剪贴板被清空或替换后，历史仍从自身值重放。
- 每个分配失败点和预算不足均保持池指针、内容、选择、水位、姿态及重做。混合计划最后一个族失败也不能提前发布其他族。
- gravity 的非整数噪声插值、保留字节、极值溢出与非有限结果。附件按前键保持的字段及两项线性插值需要逐值对照。
- 首批拒绝 gravity 或 accessory 混合的测试，以及两种历史模式下的单步记录。

全部数据准备可沿用现有 CPU 路径和 Win7 支持的系统调用，不增加显卡厂商或新系统依赖。本记录没有替代 Win7、AMD 真机验收。
