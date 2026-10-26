# 冻结骨骼通道与表情权重求值

`mms.scene.evaluate` 1.1 提供已注册骨骼动作和表情权重的独立采样。[C 接口表](include/mms/evaluation.h)保留 1.0 的七个函数，并追加四个通道选择与表情入口。插件在 UI 线程通过 `MmsHostV1::queryService` 查询一次，随后可以缓存返回的函数表。

完整配置的生产插件运行时开放此服务。`MMD_ORIGINAL_PROFILE` 配置的 `CreateEvaluationStore` 工厂返回空对象，未开放此服务。插件以服务查询结果判断可用性，不能仅根据 SDK 头文件判断。

## 求值范围

一次 `createSource` 冻结一个模型的已注册骨骼通道，包括原生关键帧及其插值、MMS 自由曲线、尺度关键帧及其插值。未注册的当前编辑姿态不属于采样内容。创建和采样均不移动播放头，不覆盖编辑姿态，不推进物理，也不写入工程或撤销历史。

`sampleBone` 返回层级变换之前的局部通道。结果尚未叠加 IK、表情、动作层或物理。服务不提供完整场景帧，也不声明 `MMS_STAGE_ANIMATED` 支持。附件、相机、灯光和其他非模型对象不在 1.0 或 1.1 的捕获范围内。

1.1 的 `createSourceChannels` 可选择骨骼通道、表情权重，或同时选择两者。表情结果是已注册的直接权重，包括原生关键帧与 MMS 自由曲线。结果保留负数和超过 `1` 的有限权重，不夹取到 `[0, 1]`。服务不展开 group 或 flip，不计算顶点、骨骼、UV、材质或其他表情形变。当前未注册的表情编辑值不参与采样。本次扩展不改变 VMD 格式、插值协议或标记策略。

快照持有采样所需的数据。源模型后续修改、删除或工程切换都不改变既有快照。获取新动作需要再次调用创建入口。

## 查询与创建参数

服务名称为 `mms.scene.evaluate`。Windows x64 下的查询约定如下。

| 查询版本 | 输出表 | 字节数 | 返回头 |
| --- | --- | --- | --- |
| `1, 0` | `MmsEvaluationApiV1` | 72 | `size = 72, major = 1, minor = 0` |
| `1, 1` | `MmsEvaluationApiV1_1` | 104 | `size = 104, major = 1, minor = 1` |

1.1 的前七个函数与 1.0 一致，共用 `context` 和源句柄。返回的 `context` 属于当前插件实例，调用方原样传给表内函数。表版本不等于通道能力，插件仍须调用 `getCapabilities`。1.1 查询缓冲区小于 104 字节时返回 `MMS_E_ARGUMENT`，不会降级写入 1.0 表。主版本不匹配或次版本大于 `1` 时返回 `MMS_E_VERSION`。失败保留输出缓冲区，成功只写所请求版本的表。

在插件清单中声明 `capabilities: ["scene.evaluate"]`。若插件必须依赖此服务，在 `requiresServices` 中增加 `{"id":"mms.scene.evaluate","major":1,"minMinor":0}`。必须调用表情入口的插件将 `minMinor` 设为 `1`。其他清单字段沿用 [SDK 入门](README.md) 的约定。可选功能不应把此服务声明为必需，应在查询失败后关闭对应功能。

`MmsEvaluationSourceQueryV1` 的字段约定如下。

| 字段 | 约定 |
| --- | --- |
| `h` | `size >= sizeof(MmsEvaluationSourceQueryV1)`，`major = 1`，`minor = 0`。宿主不读取多出的尾部字段 |
| `object` | 从 `mms.scene` 获取的当前模型引用。`id` 非空且 `generation` 非零，引用须已在当前场景发布 |
| `maxRetainedBytes` | 此次捕获的保留字节上限。`0` 选择宿主默认值，非零值不得超过 `getCapabilities` 返回的 `maxSourceBytes` |
| `flags`、`reserved` | 均为 `0` |

两个创建入口均使用上述 V1 查询结构，`flags` 和 `reserved` 仍为 `0`，通道掩码通过 `createSourceChannels` 的独立参数传入。创建只能在 UI 线程调用。场景变更、待完成编辑事务或宿主尚未就绪时，创建会被拒绝。已创建的快照可交给后台线程采样。

## 1.0 保留的七个函数

所有输出结构都无需预填 `h`。成功时宿主填写结构头及字段，失败时保留调用方原有输出。

| 函数 | 行为 | 调用线程 |
| --- | --- | --- |
| `getCapabilities(context, out)` | 返回宿主当前支持的通道标志和额度。支持两类通道时为 `3`，仅支持骨骼时为 `1`，与查询表版本无关 | 任意线程 |
| `createSource(context, query, outSource)` | 只捕获骨骼通道，返回新句柄。1.1 表中的此入口也保持该行为 | UI 线程 |
| `describeSource(context, source, out)` | 返回源模型引用、捕获标识、宿主修订信息、保留字节数、骨骼数量和能力标志 | 任意线程 |
| `boneAt(context, source, index, out)` | 按快照内的零起始序号返回骨骼 ID 和支持标志。有效范围为 `[0, boneCount)` | 任意线程 |
| `sampleBone(context, source, bone, time, out)` | 按骨骼 ID 和有理数秒采样，结果复制到调用方缓冲区 | 任意线程 |
| `retain(context, source, outSource)` | 返回另一个独立句柄，与原句柄共享同一份冻结数据 | 任意线程 |
| `release(context, source)` | 释放一个句柄。调用成功后，该句柄不能继续使用 | 任意线程 |

除两个创建入口外，服务入口允许并发调用。调用方须保证不同写操作不共用同一输出缓冲区。创建是同步操作，较大的动作数据会占用 UI 线程时间。

`MmsEvaluationSourceInfoV1` 的 `object` 保留捕获时的引用。源模型删除后，该字段仍可读取，但不再代表可用于新捕获的当前对象。`flags` 表示该源实际冻结的通道，可能为 `1`、`2` 或 `3`。通过旧 `createSource` 创建的源始终为 `1`。旧描述结构保留 64 字节和 `1, 0` 头，不包含表情数量。

`captureId` 在同一个服务实例的每次成功捕获中唯一。`retain` 得到的句柄共享 `captureId`。即使动作未变化，再次捕获也会得到不同的 `captureId`。

`hostRevision` 是捕获时的宿主修订元数据，可能为 `0`。它不是动作内容哈希，不能用于判断两份快照的动作是否相同。`captureId` 和句柄都不用于跨进程或跨运行持久化。

## 1.1 追加的四个函数

| 函数 | 行为 | 调用线程 |
| --- | --- | --- |
| `createSourceChannels(context, query, channels, outSource)` | 原子捕获所选通道，返回一个共享源句柄 | UI 线程 |
| `describeSourceChannels(context, source, out)` | 返回 `MmsEvaluationSourceInfoV1_1`，在旧描述字段后追加 `morphCount` 和零值 `reserved` | 任意线程 |
| `morphAt(context, source, index, out)` | 返回 `MmsEvaluationMorphV1`，有效序号范围为 `[0, morphCount)` | 任意线程 |
| `sampleMorph(context, source, morph, time, out)` | 按表情 ID 和有理数秒返回 `MmsMorphWeightV1` | 任意线程 |

`channels` 可取以下值。

| 值 | 标志 | 捕获内容 |
| --- | --- | --- |
| `1` | `MMS_EVALUATION_CAP_BONE_CHANNELS` | 骨骼通道 |
| `2` | `MMS_EVALUATION_CAP_MORPH_WEIGHTS` | 表情直接权重 |
| `3` | 两个标志按位或 | 同一模型的骨骼通道和表情直接权重 |

`0` 或包含未知位的掩码返回 `MMS_E_ARGUMENT`。宿主没有表情捕获能力时，请求表情返回 `MMS_E_UNSUPPORTED`。只捕获骨骼时不读取表情动作，表情轨道损坏不会导致旧创建入口失败。只捕获表情时不依赖骨骼动作。组合捕获中任一所选通道失败，整个创建失败，不发布部分源。

骨骼与表情共用同一个 `captureId`、计费和租约。未选择通道的数量为零，读取该通道返回 `MMS_E_UNSUPPORTED`。描述入口返回源实际包含的通道，不随宿主能力变化。

新增输出结构的约定如下。

| 结构 | 字节数 | 返回头 | 关键字段 |
| --- | --- | --- | --- |
| `MmsEvaluationSourceInfoV1_1` | 72 | `size = 72, major = 1, minor = 1` | `boneCount`、`morphCount`、实际通道 `flags`，以及共享的 `captureId` 和 `retainedBytes` |
| `MmsEvaluationMorphV1` | 32 | `size = 32, major = 1, minor = 0` | `id`、`kind`、`flags` |
| `MmsMorphWeightV1` | 16 | `size = 16, major = 1, minor = 0` | 有限浮点 `value` 和零值 `reserved` |

表情 `id` 与 `mms.scene` 的 `MMS_CHANNEL_MORPHS` 身份一致。`kind` 沿用 `MMS_MORPH_*` 类型。`flags` 中的 `MMS_MORPH_BASE` 与 `MMS_MORPH_PANEL_MASK` 沿用场景定义，面板值由 `(flags & MMS_MORPH_PANEL_MASK) >> MMS_MORPH_PANEL_SHIFT` 取得。描述中的类型和面板不改变直接权重的采样语义，表情序号也不能代替跨源身份。

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

宿主先用整数分数检查上限，再约分并转换为浮点采样时间。因此 `{9007199254740993, 27021597764222979}` 与 `{1, 3}` 都落在第 `10` 帧。略超上限的有理数仍返回 `MMS_E_ARGUMENT`，不会因浮点舍入而被接受。调用方的 `MmsTime` 不会被改写。

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

`getCapabilities` 返回当前服务的字节额度和单插件句柄额度。1.0 与 1.1 共用以下限制，通道组合不会获得额外额度。

| 范围 | 上限 |
| --- | --- |
| 单份快照的保留字节 | 64 MiB |
| 单插件实例的保留字节 | 128 MiB |
| 同一个服务实例的全部保留字节 | 256 MiB |
| 单插件实例的源句柄，包含 `retain` | 128，对应 `maxSources` |
| 同一个服务实例的源句柄总数 | 1024，1.0 能力结构没有单独的全局句柄字段 |

`retainedBytes` 是宿主计入额度的保留量，包含所选通道的动作快照、骨骼记录、表情记录、宿主数据及服务索引的计费。索引节点和部分所有权开销按保守额度计入。该值不是进程精确堆占用，也不是捕获期间的峰值内存。

捕获期间的临时容器、插件自身缓冲区和 MMS 的其他数据不受上述保留额度约束。`maxRetainedBytes` 只限制本次快照最终保留的计费量，不能作为进程内存上限。

多个 `retain` 句柄共享快照，不重复计入其保留字节。最后一个句柄释放后，已经获准执行的采样仍持有数据。只有这些采样也结束时才归还该份快照的字节额度。

## 停止与句柄寿命

句柄只属于创建它的服务和插件实例。其他插件不能读取、保留或释放该句柄，传给其他服务也没有意义。输出采样数据是调用方持有的副本，不依赖句柄继续存活。

插件进入停止阶段后，服务保留已有句柄并拒绝新的创建、描述、枚举、采样、保留及能力查询。此时只有 `release` 仍可用于清理。宿主不会代替插件自动释放这些源句柄。

此规则也适用于 1.1 的四个追加入口。骨骼和表情的执行中采样共同保留源，释放最后一个句柄不会使已接纳的表情采样失效。

已经获准执行的采样可以完成并写回结果。停止阶段开始前已启动的捕获也可以结束内部工作，但不会再发布新句柄，创建调用返回 `MMS_E_NOT_READY`。已有句柄和执行中的捕获、采样会阻止相关宿主上下文卸载。

插件停止逻辑负责取消后续工作、等待后台线程结束并释放全部句柄。只有所有使用者都结束后，才能丢弃缓存函数表。宿主保活不代替线程退出，也不允许插件最终卸载后继续调用函数表。

## 错误码

服务函数失败时不修改输出，不发布部分快照。多个条件同时无效时，不保证错误码优先级。

| 结果 | 含义 |
| --- | --- |
| `MMS_E_ARGUMENT` | 空输出、无效输入头、非零查询 `flags` 或 `reserved`、空对象 ID、零 `generation`、零或未知位的通道掩码、超过宿主单源上限的预算参数、无效或越界时间、捕获数据无效 |
| `MMS_E_THREAD` | 在非 UI 线程调用 `createSource` 或 `createSourceChannels`。服务查询本身也要求 UI 线程 |
| `MMS_E_NOT_FOUND` | 未知、已释放或属于其他插件的源句柄，骨骼或表情的 ID 或序号不存在，或创建时未找到对象 |
| `MMS_E_STALE` | 创建时模型引用已过期，包括 `generation` 不匹配或对象已删除。不会使既有冻结快照失效 |
| `MMS_E_UNSUPPORTED` | 创建时对象类型或所选通道不支持，读取源未捕获的通道，或采样骨骼缺少 `MMS_EVALUATION_BONE_LOCAL` |
| `MMS_E_NOT_READY` | 插件已进入停止阶段，或宿主尚未就绪、正在变更场景 |
| `MMS_E_BUSY` | 创建重入、编辑导航暂不可用、句柄数量或标识序列耗尽 |
| `MMS_E_MEMORY` | 保留字节额度不足、计费运算溢出或内存分配失败 |
| `MMS_E_FAILED` | 宿主未提供有效采样实现、采样结果出现非有限值或无效标志，或内部异常 |

服务查询还可能因服务未开放或 ABI 版本不匹配而失败。查询的主版本不同或要求不支持的次版本时，返回 `MMS_E_VERSION`。查询失败时，调用方不能使用未取得的函数表。两个创建入口的查询结构均要求 `1, 0` 头，错误版本返回 `MMS_E_ARGUMENT`，不等同于服务查询的版本错误。

本服务只依赖 CPU 路径，不要求特定显卡厂商或新系统图形接口。Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求不变。
