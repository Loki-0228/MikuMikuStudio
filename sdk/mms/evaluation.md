# 冻结动作与骨架场景求值

`mms.scene.evaluate` 1.4 提供已注册通道采样，以及冻结完整模型集合的独立骨架场景帧。[C 接口表](include/mms/evaluation.h)保留 1.0 至 1.3 的函数，追加场景来源和帧入口。插件在 UI 线程通过 `MmsHostV1::queryService` 查询一次，随后可以缓存返回的函数表。

完整配置的生产插件运行时开放此服务，当前通道能力掩码为 `7`。`MMD_ORIGINAL_PROFILE` 配置的 `CreateEvaluationStore` 工厂返回空对象，未开放此服务。插件以服务查询及对应能力函数的结果判断可用性，不能仅根据 SDK 头文件判断。

## 求值范围

一次 `createSource` 冻结一个模型的已注册骨骼通道，包括原生关键帧及其插值、MMS 自由曲线、尺度关键帧及其插值。未注册的当前编辑姿态不属于采样内容。创建和采样均不移动播放头，不覆盖编辑姿态，不推进物理，也不写入工程或撤销历史。

`sampleBone` 返回层级变换之前的局部通道。结果尚未叠加 IK、表情、动作层或物理。服务不提供完整场景帧，也不声明 `MMS_STAGE_ANIMATED` 支持。附件、相机、灯光和其他非模型对象不在当前捕获范围内。

1.1 的 `createSourceChannels` 可选择骨骼通道、表情权重，或同时选择两者。表情结果是已注册的直接权重，包括原生关键帧与 MMS 自由曲线。结果保留负数和超过 `1` 的有限权重，不夹取到 `[0, 1]`。该表情权重采样入口不展开 group 或 flip，也不计算顶点、骨骼、UV、材质或其他表情形变；1.4 骨架场景帧会另行合成骨骼表情。当前未注册的表情编辑值不参与采样。本次扩展不改变 VMD 格式、插值协议或标记策略。

快照持有采样所需的数据。源模型后续修改、删除或工程切换都不改变既有快照。获取新动作需要再次调用创建入口。

1.2 的显示通道捕获模型可见状态、各 IK 链的开关及外部父级选择。采样只返回注册数据，不执行 IK，不应用外部父级变换，也不冻结目标模型的动作。

## 查询与创建参数

服务名称为 `mms.scene.evaluate`。Windows x64 下的查询约定如下。

| 查询版本 | 输出表 | 字节数 | 返回头 |
| --- | --- | --- | --- |
| `1, 0` | `MmsEvaluationApiV1` | 72 | `size = 72, major = 1, minor = 0` |
| `1, 1` | `MmsEvaluationApiV1_1` | 104 | `size = 104, major = 1, minor = 1` |
| `1, 2` | `MmsEvaluationApiV1_2` | 152 | `size = 152, major = 1, minor = 2` |
| `1, 3` | `MmsEvaluationApiV1_3` | 160 | `size = 160, major = 1, minor = 3` |
| `1, 4` | `MmsEvaluationApiV1_4` | 232 | `size = 232, major = 1, minor = 4` |

每个新版本保留前一版本的全部函数，五个版本共用 `context` 和句柄。返回的 `context` 属于当前插件实例，调用方原样传给表内函数。表版本不等于通道能力；插件应分别通过 `getCapabilities` 和 `getSceneCapabilities` 查询旧通道及骨架场景能力。查询缓冲区小于所请求表的大小时返回 `MMS_E_ARGUMENT`，不会降级写入旧表。主版本不匹配或次版本大于 `4` 时返回 `MMS_E_VERSION`。失败保留输出缓冲区，成功只写所请求版本的表。

在插件清单中声明 `capabilities: ["scene.evaluate"]`。若插件必须依赖此服务，在 `requiresServices` 中增加 `{"id":"mms.scene.evaluate","major":1,"minMinor":0}`。必须调用表情入口的插件将 `minMinor` 设为 `1`，必须调用显示或 IK 入口的插件设为 `2`。需要区分地面与继承父级的插件设为 `3`，需要独立骨架场景帧的插件设为 `4`。其他清单字段沿用 [SDK 入门](README.md) 的约定。可选功能不应把此服务声明为必需，应在查询失败后关闭对应功能。

`MmsEvaluationSourceQueryV1` 的字段约定如下。

| 字段 | 约定 |
| --- | --- |
| `h` | `size >= sizeof(MmsEvaluationSourceQueryV1)`，`major = 1`，`minor = 0`。宿主不读取多出的尾部字段 |
| `object` | 从 `mms.scene` 获取的当前模型引用。`id` 非空且 `generation` 非零，引用须已在当前场景发布 |
| `maxRetainedBytes` | 此次捕获的保留字节上限。`0` 选择宿主默认值，非零值不得超过 `getCapabilities` 返回的 `maxSourceBytes` |
| `flags`、`reserved` | 均为 `0` |

旧的两个通道创建入口均使用上述 V1 查询结构，`flags` 和 `reserved` 仍为 `0`，通道掩码通过 `createSourceChannels` 的独立参数传入。创建只能在 UI 线程调用。场景变更、待完成编辑事务或宿主尚未就绪时，创建会被拒绝。已创建的快照可交给后台线程采样。

## 1.0 保留的七个函数

所有输出结构都无需预填 `h`。成功时宿主填写结构头及字段，失败时保留调用方原有输出。

| 函数 | 行为 | 调用线程 |
| --- | --- | --- |
| `getCapabilities(context, out)` | 返回宿主当前支持的通道标志和额度。完整生产工厂为 `7`，骨骼与表情工厂可为 `3`，仅骨骼工厂为 `1`。实际值与查询表版本无关 | 任意线程 |
| `createSource(context, query, outSource)` | 只捕获骨骼通道，返回新句柄。后续版本表中的此入口也保持该行为 | UI 线程 |
| `describeSource(context, source, out)` | 返回源模型引用、捕获标识、宿主修订信息、保留字节数、骨骼数量和能力标志 | 任意线程 |
| `boneAt(context, source, index, out)` | 按快照内的零起始序号返回骨骼 ID 和支持标志。有效范围为 `[0, boneCount)` | 任意线程 |
| `sampleBone(context, source, bone, time, out)` | 按骨骼 ID 和有理数秒采样，结果复制到调用方缓冲区 | 任意线程 |
| `retain(context, source, outSource)` | 返回另一个独立句柄，与原句柄共享同一份冻结数据 | 任意线程 |
| `release(context, source)` | 释放一个句柄。调用成功后，该句柄不能继续使用 | 任意线程 |

`createSource`、`createSourceChannels` 和 `createSceneSource` 必须在 UI 线程调用，其他入口允许并发调用。调用方须保证不同写操作不共用同一输出缓冲区。创建是同步操作，较大的动作数据会占用 UI 线程时间。

`MmsEvaluationSourceInfoV1` 的 `object` 保留捕获时的引用。源模型删除后，该字段仍可读取，但不再代表可用于新捕获的当前对象。`flags` 表示该源实际冻结的通道，是能力掩码的非零子集。通过旧 `createSource` 创建的源始终为 `1`。旧描述结构保留 64 字节和 `1, 0` 头，不包含表情、IK 或选择器数量。

`captureId` 在同一个服务实例的每次成功捕获中唯一。`retain` 得到的句柄共享 `captureId`。即使动作未变化，再次捕获也会得到不同的 `captureId`。

`hostRevision` 是捕获时的宿主修订元数据，可能为 `0`。它不是动作内容哈希，不能用于判断两份快照的动作是否相同。`captureId` 和句柄都不用于跨进程或跨运行持久化。

## 1.1 追加的四个函数

| 函数 | 行为 | 调用线程 |
| --- | --- | --- |
| `createSourceChannels(context, query, channels, outSource)` | 原子捕获所选通道，返回一个共享源句柄 | UI 线程 |
| `describeSourceChannels(context, source, out)` | 返回 `MmsEvaluationSourceInfoV1_1`，在旧描述字段后追加 `morphCount` 和零值 `reserved` | 任意线程 |
| `morphAt(context, source, index, out)` | 返回 `MmsEvaluationMorphV1`，有效序号范围为 `[0, morphCount)` | 任意线程 |
| `sampleMorph(context, source, morph, time, out)` | 按表情 ID 和有理数秒返回 `MmsMorphWeightV1` | 任意线程 |

`channels` 可取以下值。显示位从服务 1.2 起支持，调用前须核对宿主能力。

| 值 | 标志 | 捕获内容 |
| --- | --- | --- |
| `1` | `MMS_EVALUATION_CAP_BONE_CHANNELS` | 骨骼通道 |
| `2` | `MMS_EVALUATION_CAP_MORPH_WEIGHTS` | 表情直接权重 |
| `3` | 两个标志按位或 | 同一模型的骨骼通道和表情直接权重 |
| `4` | `MMS_EVALUATION_CAP_DISPLAY` | 模型可见状态、IK 开关及外部父级选择 |
| `5` | 骨骼与显示标志按位或 | 骨骼通道和显示通道 |
| `6` | 表情与显示标志按位或 | 表情直接权重和显示通道 |
| `7` | 三个标志按位或 | 骨骼、表情和显示通道 |

`0` 或包含未知位的掩码返回 `MMS_E_ARGUMENT`。请求宿主未提供的已知通道返回 `MMS_E_UNSUPPORTED`。宿主内部旧骨骼捕获器只提供 `1`，通道捕获器的默认能力仍为 `3`，显示能力必须显式启用。查询到 1.2 表不代表宿主必然支持显示通道。

只捕获骨骼时不读取表情或显示动作，未选择轨道损坏不会导致旧创建入口失败。只捕获表情时不依赖骨骼或显示动作。组合捕获中任一所选通道失败，整个创建失败，不发布部分源。

所有所选通道共用同一个 `captureId`、计费和租约。未选择通道的数量为零，读取该通道返回 `MMS_E_UNSUPPORTED`。描述入口返回源实际包含的通道，不随宿主能力变化。

新增输出结构的约定如下。

| 结构 | 字节数 | 返回头 | 关键字段 |
| --- | --- | --- | --- |
| `MmsEvaluationSourceInfoV1_1` | 72 | `size = 72, major = 1, minor = 1` | `boneCount`、`morphCount`、实际通道 `flags`，以及共享的 `captureId` 和 `retainedBytes` |
| `MmsEvaluationMorphV1` | 32 | `size = 32, major = 1, minor = 0` | `id`、`kind`、`flags` |
| `MmsMorphWeightV1` | 16 | `size = 16, major = 1, minor = 0` | 有限浮点 `value` 和零值 `reserved` |

表情 `id` 与 `mms.scene` 的 `MMS_CHANNEL_MORPHS` 身份一致。`kind` 沿用 `MMS_MORPH_*` 类型。`flags` 中的 `MMS_MORPH_BASE` 与 `MMS_MORPH_PANEL_MASK` 沿用场景定义，面板值由 `(flags & MMS_MORPH_PANEL_MASK) >> MMS_MORPH_PANEL_SHIFT` 取得。描述中的类型和面板不改变直接权重的采样语义，表情序号也不能代替跨源身份。

## 1.2 追加的六个函数

| 函数 | 行为 | 调用线程 |
| --- | --- | --- |
| `describeSourceDisplay(context, source, out)` | 返回 `MmsEvaluationSourceInfoV1_2`，在 1.1 描述后追加 `ikCount` 和 `selectorCount` | 任意线程 |
| `ikAt(context, source, index, out)` | 按 `[0, ikCount)` 的序号枚举 IK 链对应的场景骨骼 ID | 任意线程 |
| `selectorAt(context, source, index, out)` | 按 `[0, selectorCount)` 的序号枚举外部父级选择器 | 任意线程 |
| `sampleVisibility(context, source, time, out)` | 返回指定时间的注册可见状态 | 任意线程 |
| `sampleIk(context, source, index, time, out)` | 返回指定 IK 链序号的注册开关 | 任意线程 |
| `sampleParent(context, source, index, time, out)` | 返回指定选择器序号的注册目标身份及连续选择窗口 | 任意线程 |

这六个函数允许并发读取。`createSourceChannels` 仍在 UI 线程执行，显示捕获没有新增创建入口。IK 和选择器序号仅在该源内有效。`sampleIk` 和 `sampleParent` 使用枚举序号，不能将骨骼 ID 当作序号传入。

| 结构 | 字节数 | 返回头 | 字段含义 |
| --- | --- | --- | --- |
| `MmsEvaluationSourceInfoV1_2` | 80 | `size = 80, major = 1, minor = 2` | 保留旧源字段，追加 `ikCount`、`selectorCount`。`reserved` 为零 |
| `MmsEvaluationIkV1` | 24 | `size = 24, major = 1, minor = 0` | `bone` 是该 IK 链所属骨骼的稳定 ID |
| `MmsEvaluationSelectorV1` | 32 | `size = 32, major = 1, minor = 0` | `bone` 为源模型内受此选择器影响的骨骼 ID。`MMS_EVALUATION_SELECTOR_GLOBAL` 表示作用于整个源模型，此时 `bone` 为空。`reserved` 为零 |
| `MmsDisplayStateV1` | 16 | `size = 16, major = 1, minor = 0` | `visible` 为原生字节扩展后的 `uint32_t`，`reserved` 为零 |
| `MmsIkStateV1` | 16 | `size = 16, major = 1, minor = 0` | `enabled` 为原生字节扩展后的 `uint32_t`，`reserved` 为零 |
| `MmsParentSelectionV1` | 64 | `size = 64, major = 1, minor = 0` | `target`、`bone`、`windowStart`、`windowEnd`、`flags` 和零值 `reserved` |

`visible` 和 `enabled` 保留 `0` 到 `255` 的原字节，不归一化为布尔值。显示通道按前一个注册关键帧阶跃采样，分数帧不插值。采样不会修改本体当前可见状态或 IK 开关。

### 父级身份与窗口

`MmsParentSelectionV1::flags` 只允许 `MMS_EVALUATION_PARENT_ACTIVE`。启用时，`target.id` 和 `bone` 必须非空，`target.generation` 必须非零。未启用时，`target.id`、`target.generation` 和 `bone` 均为零。

旧 `sampleParent` 无法表达地面。采样地面状态时返回 `MMS_E_UNSUPPORTED`，保留调用方输出；不会将其转换为未启用父级。普通继承与模型目标仍按旧规则返回。

`windowStart` 和 `windowEnd` 是注册帧边界，范围为 `0` 到 `2147483646`。窗口包含起点，不包含非零终点。`windowEnd = 0` 表示没有后续变化。非零终点必须大于起点，采样帧必须位于窗口内。未启用父级时仍保留原连续选择窗口，不将窗口归零或合并。

显示捕获在同一次 UI 安全点解析所有可达显示关键帧引用的目标模型和骨骼，包括未来关键帧。有效模型引用必须对应当前场景已发布的模型身份及有效目标骨骼。继承和地面不需要目标身份，未来关键帧中的地面也可以正常捕获。只要其中一个模型引用失效，整个捕获返回 `MMS_E_STALE`，不发布源。未选择显示通道的捕获不检查这些引用。

旧源中的目标身份是捕获时的不可变元数据。目标模型后续删除、重新加载或槽位复用后，旧源仍返回旧目标 ID、旧 `generation` 和旧骨骼 ID，不改为槽位中的新对象。返回 `ACTIVE` 不代表目标目前仍存活，也不代表目标动作已经冻结。插件若需操作当前目标，应通过当前场景另行解析身份。目标动作与依赖可以通过下述 1.4 骨架场景帧另行冻结和求值；1.2 父级采样入口本身不提供该结果。

## 1.3 父级状态

`sampleParentState(context, source, index, time, out)` 使用已有显示源句柄及选择器序号，允许后台并发调用。无需重新捕获，也没有第二套租约。返回的 `MmsParentStateV1` 为 64 字节，头为 `size = 64, major = 1, minor = 0`。

| `kind` | 含义 | 身份字段 |
| --- | --- | --- |
| `MMS_EVALUATION_PARENT_INHERIT`，值 `0` | 原生 `-1`，保留父骨骼或全局外亲的继承规则 | `target` 和 `bone` 全零 |
| `MMS_EVALUATION_PARENT_MODEL`，值 `1` | 引用捕获时的模型骨骼 | `target.id`、`bone` 非空，`target.generation` 非零 |
| `MMS_EVALUATION_PARENT_GROUND`，值 `2` | 原生 `-2`，局部选择器不再继承父骨骼或全局外亲 | `target` 和 `bone` 全零 |

`kind` 是单个类型值，不是位掩码。`reserved` 必须为零。`windowStart` 和 `windowEnd` 沿用 1.2 的连续窗口规则，状态切换不会丢失原窗口。所有状态均使用同一精确有理数时间边界和冻结数据，失败保留整个输出结构。

全局选择器的继承和地面都没有显式模型目标，但原始状态仍分开返回。局部选择器中两者有实际求值差异，因此插件不得仅依据身份为空就将其合并。该接口返回注册状态，不执行外亲变换。

## 1.4 独立骨架场景帧

1.4 的场景来源捕获当前全部模型及其注册骨骼、表情、显示和外亲依赖。插件不指定单个模型，捕获包括不可见模型及仅由未来外亲键引用的模型。来源只保留冻结数据，不借用主场景的模型指针。创建须在 UI 线程调用，查询、求值、读取与释放可以从后台线程调用。

该阶段提供无物理模拟的骨架求值：局部动作、骨骼表情、层级、IK、外亲和尺度。模型按原生顺序执行全场物理前和物理后两轮，并从确定的加载初值开始。刚体历史、网格蒙皮、附件、相机、灯光、渲染器和多视口不属于此帧。不能把它称为完整场景渲染结果，也不能假定它与连续物理播放的历史状态相同。

首先调用 `getSceneCapabilities`。`flags` 中的 `MMS_EVALUATION_SCENE_SKELETON` 表示宿主提供此功能；查询 1.4 表成功不代表该标志一定存在。原始兼容产品不开放生产求值服务。`reserved` 和所有请求的 `flags`、`reserved` 必须为零。

| 函数 | 行为 |
| --- | --- |
| `createSceneSource(context, query, out)` | UI 安全点捕获完整模型集合；成功返回独立来源句柄 |
| `describeSceneSource(context, source, out)` | 返回捕获 ID、宿主历史元数据、模型数量和计费量 |
| `sceneModelAt(context, source, index, out)` | 按捕获顺序枚举模型身份及骨骼数量 |
| `sceneBoneAt(context, source, modelIndex, boneIndex, out)` | 枚举与 BIND 快照一致的骨骼 ID |
| `evaluateFrame(context, query, out)` | 同步求值并返回独立帧；可由插件后台任务调用 |
| `describeFrame(context, frame, out)` | 返回来源捕获 ID、帧 ID、规范化有理时间及计费量 |
| `frameModelAt(context, frame, index, out)` | 返回冻结模型身份、骨骼数及注册的原始可见字节 |
| `readBonePose(context, frame, modelIndex, boneIndex, out)` | 按帧内序号复制该骨骼的计算结果 |

`MmsEvaluationSceneModelV1::flags` 在来源枚举时为零；帧枚举时包含 `MMS_EVALUATION_SCENE_VISIBILITY_VALID`，`visibility` 是原始注册字节扩展为整数，不是规范化布尔值。标准 PMM 保存会将该字段写成 `0` 或 `1`；重载后的新来源读取标准值，之前的帧仍保留其捕获值。模型和骨骼序号只在对应捕获内有效。若要关联其他服务，使用返回的对象身份与骨骼 ID。

`MmsEvaluationScenePoseV1` 包含刚性变形矩阵 `rigid`、含尺度变形矩阵 `scaled`、物理前和物理后两次扫描的附着矩阵 `attachmentBefore`、`attachmentAfter`，以及 `jointPosition`。矩阵采用原生行向量约定；变形矩阵作用于绑定坐标，不能直接当作骨骼世界矩阵。该结构是调用方持有的副本，不暴露内部指针。

`MmsEvaluationFrameQueryV1::time` 使用非负有理数秒，帧率为 30。上限与前述采样相同。关键帧前的分数即使连续值舍入到整数，也保持前一离散状态；位置、旋转、尺度、表情和自由曲线仍连续计算。`describeFrame` 返回规范化的请求分数。请求与结果均不移动主播放头、不覆盖编辑姿态、不推进主物理世界，也不写工程或共同历史。

来源和帧共用现有 `retain`、`release`。释放来源后，已发布的帧仍可读取；已获准的求值也可在来源最后一个句柄释放后完成。停止开始后，不再发布新帧，并拒绝新的读取和求值；已经接纳的读取可以完成。同步求值当前不支持调用中取消。类型不匹配返回 `MMS_E_UNSUPPORTED`，越界或其他插件的句柄返回 `MMS_E_NOT_FOUND`；所有失败保留输出。

下面的 C 示例读取第一个模型的第一根骨骼。调用此函数前，插件已在 UI 线程查询 1.4 函数表；此示例本身也在 UI 线程调用，因为它创建来源。需要后台求值时，先在 UI 线程创建来源，再将来源句柄传给后台任务。

```c
MmsResult read_first_pose(const MmsEvaluationApiV1_4* api,
                          MmsEvaluationScenePoseV1* out) {
    if (!api || !out) return MMS_E_ARGUMENT;
    MmsEvaluationSceneCapsV1 caps = {0};
    MmsResult result = api->getSceneCapabilities(api->context, &caps);
    if (result != MMS_OK) return result;
    if (!(caps.flags & MMS_EVALUATION_SCENE_SKELETON)) return MMS_E_UNSUPPORTED;

    MmsEvaluationSceneQueryV1 source_query = {{sizeof(source_query), 1, 0}, 0, 0, 0};
    MmsHandle source = 0, frame = 0;
    result = api->createSceneSource(api->context, &source_query, &source);
    if (result != MMS_OK) return result;

    MmsEvaluationFrameQueryV1 frame_query = {0};
    frame_query.h = (MmsHeader){sizeof(frame_query), 1, 0};
    frame_query.source = source;
    frame_query.time = (MmsTime){1, 2}; /* 0.5 s, frame 15. */
    result = api->evaluateFrame(api->context, &frame_query, &frame);
    if (result == MMS_OK) {
        MmsEvaluationScenePoseV1 pose = {0};
        result = api->readBonePose(api->context, frame, 0, 0, &pose);
        if (result == MMS_OK) *out = pose;
        api->release(api->context, frame);
    }
    api->release(api->context, source);
    return result;
}
```

来源和帧与旧通道共享保留额度：每份上限 64 MiB，每个插件实例总计 128 MiB，服务总计 256 MiB。相同内容的独立来源或帧分别计费；`retain` 不重复计费。请求 `maxRetainedBytes = 0` 使用每份上限。场景求值的工作区上限为单次 128 MiB、单插件同时 256 MiB、服务同时 512 MiB；`maxWorkspaceBytes = 0` 使用单次上限。并发预留不足返回 `MMS_E_MEMORY`。这些额度不包括全部嵌套算法的瞬时堆分配，不能作为进程峰值内存的精确上限。

## 骨骼标志与结果

`MmsEvaluationBoneV1::id` 与 `mms.scene` 的骨骼 ID 一致。骨骼序号仅在当前快照内有效，不能作为跨快照身份。

| 骨骼标志 | 1.0 行为 |
| --- | --- |
| `MMS_EVALUATION_BONE_LOCAL` | 可采样已注册局部通道 |
| `MMS_EVALUATION_BONE_EXTERNAL_PARENT` | 骨骼依赖外部父级。保留枚举记录，采样返回 `MMS_E_UNSUPPORTED` |
| `MMS_EVALUATION_BONE_DERIVED` | 骨骼属于当前未支持独立采样的派生类型。保留枚举记录，采样返回 `MMS_E_UNSUPPORTED` |

`LOCAL` 与另两个标志互斥。外部父级标志和派生标志可以同时存在。是否可采样以枚举结果中的 `LOCAL` 为准，不以骨骼名称判断。

`MmsBoneChannelsV1` 返回以下数据。

| 字段 | 内容 |
| --- | --- |
| `position[3]` | 已注册局部位移，顺序为 X、Y、Z。不是骨骼最终世界坐标 |
| `rotation[4]` | 局部四元数，顺序为 X、Y、Z、W |
| `scale[9]` | 完整 3×3 尺度矩阵，按行存放，沿用 MMS 的行向量约定。基准为骨骼未变形时的模型空间基底，不能只取三个对角元素替代整个矩阵 |
| `flags` | `MMS_EVALUATION_PHYSICS_DISABLED` 表示该时刻关键帧通道禁用物理。这是采样数据，不会启动或停止宿主模拟 |
| `reserved` | `0` |

## 有理数秒与帧边界

`MmsTime` 表示 `numerator / denominator` 秒。分母必须非零，分子必须非负。宿主按固定的 30 fps 基准换算为采样帧，不使用视口刷新率或导出帧率。

例如，`{1, 3}` 表示第 `10` 帧，`{1, 4}` 表示第 `7.5` 帧。支持分数帧。合法范围为第 `0` 帧到第 `2147483646` 帧，两个端点均包含在内。

宿主先用整数分数检查上限。骨骼和表情连续通道再约分并转换为浮点采样时间；显示、IK 开关和父级选择则用整数运算精确取得 `floor(30 × numerator / denominator)`。这些离散轨道的键和窗口均位于整数帧，取整不改变前键语义，也不改变调用方的 `MmsTime`。

例如 `{9007199254740993, 27021597764222979}` 与 `{1, 3}` 都落在第 `10` 帧；前者分母增加 `1` 后严格小于第 `10` 帧，离散轨道必须读取第 `9` 帧所在区间，不能因浮点舍入提前切换。略超最大帧的有理数仍返回 `MMS_E_ARGUMENT`，不能先取整再接受。连续通道的分数帧插值不采用上述离散取整。

## C 调用示例

下面的 C11 函数在给定时间采样模型的第一个受支持骨骼。参数 `model` 来自当前 `mms.scene` 模型记录，`host` 来自插件创建回调。整个函数在 UI 线程、编辑事务结束后调用。传入的 `time` 若为 `{1, 3}`，采样点就是第 `10` 帧。

```c
#include <mms/evaluation.h>

MmsResult SampleFirstLocalBone(
	const MmsHostV1* host,
	MmsObjectRef model,
	MmsTime time,
	MmsBoneChannelsV1* out)
{
	static const char serviceName[] = "mms.scene.evaluate";
	MmsString name = {serviceName, sizeof(serviceName) - 1};
	MmsEvaluationApiV1 api = {0};
	MmsEvaluationCapsV1 caps = {0};
	MmsEvaluationSourceQueryV1 query = {0};
	MmsEvaluationSourceInfoV1 info = {0};
	MmsEvaluationBoneV1 bone = {0};
	MmsBoneChannelsV1 sample = {0};
	MmsHandle source = 0;
	MmsResult result, released;
	uint32_t index;

	if (!host || !host->queryService || !out)
		return MMS_E_ARGUMENT;
	result = host->queryService(host->context, name, 1, 0,
		(uint32_t)sizeof(api), &api);
	if (result != MMS_OK)
		return result;
	result = api.getCapabilities(api.context, &caps);
	if (result != MMS_OK)
		return result;
	if (!(caps.flags & MMS_EVALUATION_CAP_BONE_CHANNELS))
		return MMS_E_UNSUPPORTED;

	query.h.size = (uint32_t)sizeof(query);
	query.h.major = 1;
	query.h.minor = 0;
	query.object = model;
	/* maxRetainedBytes = 0 采用宿主默认额度。 */
	result = api.createSource(api.context, &query, &source);
	if (result != MMS_OK)
		return result;

	result = api.describeSource(api.context, source, &info);
	if (result != MMS_OK)
		goto done;
	for (index = 0; index < info.boneCount; ++index) {
		result = api.boneAt(api.context, source, index, &bone);
		if (result != MMS_OK)
			goto done;
		if (bone.flags & MMS_EVALUATION_BONE_LOCAL) {
			result = api.sampleBone(api.context, source, bone.id, time, &sample);
			goto done;
		}
	}
	result = MMS_E_UNSUPPORTED;

done:
	released = api.release(api.context, source);
	if (result != MMS_OK)
		return result;
	if (released != MMS_OK)
		return released;
	*out = sample;
	return MMS_OK;
}
```

持续后台采样可以保存 `source`，或者通过 `retain` 为工作线程取得独立句柄。每个成功创建或保留的句柄都对应一次 `release`。以上示例在返回前释放句柄，不保留后台状态。

### 1.1 表情采样示例

以下 C11 函数在 UI 线程查询 1.1 表、捕获两类通道并读取指定表情。`model` 和 `morph` 分别来自当前 `mms.scene` 的模型引用与该模型的表情 ID。只需表情的插件可将 `channels` 改为 `MMS_EVALUATION_CAP_MORPH_WEIGHTS`，避免捕获骨骼动作。原 1.0 示例无需修改。

```c
MmsResult SampleFrozenMorph(const MmsHostV1* host, MmsObjectRef model,
	MmsId morph, MmsTime time, MmsMorphWeightV1* out)
{
	static const char service[] = "mms.scene.evaluate";
	MmsString name = {service, sizeof(service) - 1};
	MmsEvaluationApiV1_1 api = {0};
	MmsEvaluationCapsV1 caps = {0};
	MmsEvaluationSourceQueryV1 query = {{sizeof(query), 1, 0}, model, 0, 0, 0};
	MmsMorphWeightV1 sample = {0};
	MmsHandle source = 0;
	uint32_t channels = MMS_EVALUATION_CAP_BONE_CHANNELS |
		MMS_EVALUATION_CAP_MORPH_WEIGHTS;
	MmsResult result, released;

	if (!host || !host->queryService || !out)
		return MMS_E_ARGUMENT;
	result = host->queryService(host->context, name, 1, 1,
		(uint32_t)sizeof(api), &api);
	if (result != MMS_OK)
		return result;
	result = api.getCapabilities(api.context, &caps);
	if (result != MMS_OK)
		return result;
	if ((caps.flags & channels) != channels)
		return MMS_E_UNSUPPORTED;
	result = api.createSourceChannels(api.context, &query, channels, &source);
	if (result != MMS_OK)
		return result;
	result = api.sampleMorph(api.context, source, morph, time, &sample);
	released = api.release(api.context, source);
	if (result != MMS_OK)
		return result;
	if (released != MMS_OK)
		return released;
	*out = sample;
	return MMS_OK;
}
```

持续采样时，创建仍在 UI 线程完成。后台线程可调用 `describeSourceChannels`、`morphAt`、`sampleMorph`、`retain` 和 `release`。失败时输出保持原值，调用方不得使用失败调用的缓冲区作为新结果。

## 保留内存与句柄额度

`getCapabilities` 返回当前服务的字节额度和单插件句柄额度。1.0 至 1.4 共用以下限制，通道组合不会获得额外额度。

| 范围 | 上限 |
| --- | --- |
| 单份快照的保留字节 | 64 MiB |
| 单插件实例的保留字节 | 128 MiB |
| 同一个服务实例的全部保留字节 | 256 MiB |
| 单插件实例的通道来源、场景来源及帧句柄合计，包含 `retain` | 128，对应 `maxSources` |
| 同一个服务实例的上述三类句柄总数 | 1024，1.0 能力结构没有单独的全局句柄字段 |

`retainedBytes` 是宿主计入额度的保留量，包含所选通道的动作快照、骨骼与表情记录、IK 与选择器记录、父级身份映射、宿主数据及服务索引的计费。索引节点和部分所有权开销按保守额度计入。该值不是进程精确堆占用，也不是捕获期间的峰值内存。

捕获期间的临时容器、插件自身缓冲区和 MMS 的其他数据不受上述保留额度约束。`maxRetainedBytes` 只限制本次快照最终保留的计费量，不能作为进程内存上限。

多个 `retain` 句柄共享快照，不重复计入其保留字节。最后一个句柄释放后，已经获准执行的采样仍持有数据。只有这些采样也结束时才归还该份快照的字节额度。

## 停止与句柄寿命

句柄只属于创建它的服务和插件实例。其他插件不能读取、保留或释放该句柄，传给其他服务也没有意义。输出采样数据是调用方持有的副本，不依赖句柄继续存活。

插件进入停止阶段后，服务保留已有句柄并拒绝新的创建、描述、枚举、采样、保留及能力查询。此时只有 `release` 仍可用于清理。宿主不会代替插件自动释放这些源句柄。

此规则也适用于 1.1 至 1.4 的追加入口。所有通道的执行中采样共同保留源，释放最后一个句柄不会使已接纳的采样失效。显示或父级采样尚未结束时，宿主仍保留服务上下文和插件 DLL。

已经获准执行的采样可以完成并写回结果。停止阶段开始前已启动的捕获也可以结束内部工作，但不会再发布新句柄，创建调用返回 `MMS_E_NOT_READY`。已有句柄和执行中的捕获、采样会阻止相关宿主上下文卸载。

插件停止逻辑负责取消后续工作、等待后台线程结束并释放全部句柄。只有所有使用者都结束后，才能丢弃缓存函数表。宿主保活不代替线程退出，也不允许插件最终卸载后继续调用函数表。

## 错误码

服务函数失败时不修改输出，不发布部分快照。多个条件同时无效时，不保证错误码优先级。

| 结果 | 含义 |
| --- | --- |
| `MMS_E_ARGUMENT` | 空输出、无效输入头、非零查询 `flags` 或 `reserved`、空对象 ID、零 `generation`、零或未知位的通道掩码、超过单份保留或单次工作区上限的预算参数、无效或越界时间、捕获数据无效 |
| `MMS_E_THREAD` | 在非 UI 线程调用 `createSource`、`createSourceChannels` 或 `createSceneSource`。服务查询本身也要求 UI 线程 |
| `MMS_E_NOT_FOUND` | 未知、已释放或属于其他插件的源句柄，骨骼或表情的 ID 或序号不存在，IK 或选择器序号不存在，或创建时未找到对象 |
| `MMS_E_STALE` | 创建时源模型引用已过期，所选显示通道或场景来源的可达关键帧引用无效目标模型或骨骼，或场景骨架定义无效。包括未来关键帧。不会使既有冻结快照失效 |
| `MMS_E_UNSUPPORTED` | 创建时对象类型或所选通道不支持，读取源未捕获的通道，采样骨骼缺少 `MMS_EVALUATION_BONE_LOCAL`、场景捕获器未配置，或句柄载荷类型与入口不匹配 |
| `MMS_E_NOT_READY` | 插件已进入停止阶段，或宿主尚未就绪、正在变更场景 |
| `MMS_E_BUSY` | 创建重入、编辑导航暂不可用、句柄数量或标识序列耗尽 |
| `MMS_E_MEMORY` | 保留字节或并发工作区预留额度不足、计费运算溢出或内存分配失败 |
| `MMS_E_FAILED` | 宿主未提供有效采样实现，采样结果出现非有限值、超出字节范围的显示或 IK 值、无效父级身份、窗口或标志，或发生内部异常 |

服务查询还可能因服务未开放或 ABI 版本不匹配而失败。查询的主版本不同或要求不支持的次版本时，返回 `MMS_E_VERSION`。查询失败时，调用方不能使用未取得的函数表。三个创建入口的查询结构均要求 `1, 0` 头，错误版本返回 `MMS_E_ARGUMENT`，不等同于服务查询的版本错误。

本服务只依赖 CPU 路径，不要求特定显卡厂商或新系统图形接口。Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求不变。
