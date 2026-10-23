# 缓存业务索引编码与分页

本批实现内部 `cache_business` 编码器，将工程关系、缓存文件记录和当前版本映射编码为有界页面及 manifest。它提供校验、分页、编码、解码和跨页组装，不读写缓存文件，也不授予容量、发布或删除许可。业务 Store、工程加载与保存接入、效果及纹理消费者、管理窗口和有来源的自动恢复仍未实现，不能将本批计为完整缓存管理功能。

main 行为核对仍为 `fed9768`。本批基线与验证材料位于 `build/verification/abi-cache-business-20261023`；接口以 [cache_business_codec.hpp](../../src/io/cache_business_codec.hpp) 为准。

## 数据对象与关系

| 对象 | 保存的内容 | 校验规则 |
| --- | --- | --- |
| `Index` | `storeOwner`、`maintenanceEpoch`、`nextProjectId`，以及工程、物理条目、当前映射三张表 | 存储身份不能全零，epoch 和 nextProjectId 非零；nextProjectId 大于已有工程 ID。 |
| `Project` | 非零工程 ID、规范路径、显示路径、lastOpened、pinned | 按 ID 严格递增；规范路径按 Windows ordinal 忽略大小写比较不得重复。 |
| `Entry` | 完整 `BlobInfo`、消费者格式、lastUse、`Valid/Invalid/Retired` 状态及 owners | 按 `(kind,name)` 严格递增；owners 是非零、严格递增的工程 ID，完整 Index 中必须引用已有工程。 |
| `Current` | `(kind,consumerFormat,key)` 与 16 字节 generation | 按逻辑键严格递增；必须找到种类、名字、key、format 均对应的 `Valid` 条目。 |

条目的种类仅允许 `Effect` 或 `Texture`。不同种类可以有相同 generation 文件名；同一种类不能重复。一个物理 generation 最多被一个当前映射引用。`Valid` 条目可以暂时不在当前映射中，`Invalid` 和 `Retired` 条目不能被当前映射引用。codec 不自行删除旧代、修复外键、排序或丢弃记录。

generation 由文件名前 32 位小写十六进制字符从左到右、每两位解码为一个字节。它不是 Windows GUID 的内存字节序，也不能独立证明磁盘文件身份。完整 `BlobInfo` 保留文件名、种类、存储身份、逻辑键、负载摘要、负载长度、卷序列号、文件索引和实际文件长度；文件长度必须等于 128 字节 envelope 加负载长度。

`storeOwner` 是缓存存储的身份，不是工程 ID。工程 owners 与存储身份分别表达，允许多个工程关联同一物理条目。本批只保存 lastUse、lastOpened 和 pinned 等值，没有实现访问时间更新、固定策略、清理选择或工程关联事件。

## 路径与 UTF-16

规范路径和显示路径都必须是词法绝对 DOS 路径或 UNC 路径，使用反斜杠。校验拒绝 NUL、控制字符、设备前缀、点路径段、内部空段、ADS、正斜杠、非法路径字符、段尾点或空格，以及保留 DOS 名称。保留名称带扩展名也被拒绝，例如 `COM1.txt`；检查还覆盖 Windows 识别的上标数字设备名。

DOS 根 `X:\` 合法。UNC 至少包含 server 和 share，share 根允许带或不带末尾反斜杠；普通非根路径不允许末尾反斜杠。这两种根路径拼写不会在 codec 中自动归一，调用方负责提供一致形式。

两份路径必须通过 Win7 可用的 `CompareStringOrdinal(..., TRUE)` 判定为同一路径，因此可以保留大小写拼写差异。工程间身份去重也使用这一比较，不依赖当前 locale。codec 不解析 junction、短路径或文件系统别名，也不检查工程文件是否存在。

字符串按 UTF-16 代码单元保存，未配对 surrogate 原样保留。编码器逐单元写入小端值，不直接复制平台 `wchar_t` 或 C++ 字符串布局；字符串没有终止 NUL。

## 页面与 manifest 格式

所有整数采用显式小端格式，保留区域必须全部为零。页面标记 `MMSCachePage0001` 和 manifest 标记 `MMSCacheIndex001` 各为恰好 16 个 ASCII 字节，线格式不含 NUL。初稿将其误写为含终止符的 `char[16]`，已在静态审查中纠正；当前实现使用明确的 16 字节初始化。

| 结构 | 固定长度 | 内容 |
| --- | --- | --- |
| Page 头 | 64 字节 | 标记、版本 1、页面种类、该类页序号、记录数、正文长度、存储身份、8 字节保留区。 |
| Project 记录 | 40 字节加两个 UTF-16 数组 | 记录总长、pinned 标志、ID、lastOpened、两份路径的单元数、8 字节保留区，再接路径内容。 |
| Entry 记录 | 192 字节加每个 owner 的 8 字节 ID | 记录总长、状态、consumerFormat、owner 数、lastUse、8 字节保留区、160 字节完整文件引用，再接 owners。 |
| Current 记录 | 64 字节 | 总长、种类、consumerFormat、4 字节保留区、32 字节逻辑键、16 字节 generation。 |
| Manifest 头 | 96 字节 | 标记、版本 1、头长、存储身份、epoch、nextProjectId、三类记录总数、页数、liveEncodedBytes、16 字节保留区。 |
| PageRef | 176 字节 | 页面种类、序号、记录数、4 字节保留区、160 字节完整文件引用。 |

160 字节文件引用沿用明确字段布局：37 字节 ASCII 文件名、3 字节零填充、种类、4 字节零填充、16 字节 owner、32 字节 key、32 字节 payloadDigest、负载长度、卷序列号、4 字节零填充、文件索引和文件长度。名字必须为 32 位小写十六进制加 `.blob`。

页面引用要求文件种类为 `IndexPage=6`，key 为 `PageKey{4}`，其余 key 字节为零，owner 等于 manifest 的存储身份。引用负载在 64 字节至 1 MiB 之间。这个引用格式不代表页面已经写入文件；业务目录发布和真实文件验证尚未接入。

一个 Page 只能包含其 kind 对应的记录集合，且不能为空。三类页面的 ordinal 各自从 0 开始；manifest 按页面种类及 ordinal 排序，不允许缺号或重复文件引用。

## 分页与容量边界

| 限制 | 当前值 |
| --- | --- |
| 工程数量 | 10,000 |
| 物理条目数量 | 100,000 |
| 当前映射数量 | 100,000 |
| 每个条目的 owner 数 | 10,000 |
| 每份工程路径长度 | 32,768 个 UTF-16 单元 |
| 单页编码长度，含页头 | 1 MiB |
| 总页面数 | 128 |
| live 编码总量 | 64 MiB |
| Entry 引用的单个负载长度 | 文件层上限 1 GiB |

这些限制同时生效。100,000 个 Entry 即使没有 owners，仅固定记录就占 19,200,000 字节；再加同数量 Current 占 6,400,000 字节，不能装入 catalog 的单个 16 MiB 快照。分页后 manifest 最多为 `96 + 128 × 176 = 22,624` 字节，页面数据与 manifest 分开表达。

`Partition` 按原有记录顺序贪心装页：下一条记录将超过 1 MiB 时开启同类下一页，记录不跨页。不输出空页，不改变记录顺序。完整 `Index` 的 `Validate` 也会扫描分页尺寸，因此不能先通过 Index 校验，再在分页时才发现总编码量超限。

live 总量精确计算为：

```text
96 + 176 × 页数 + 所有页面的编码长度
```

每页编码长度包含 64 字节页头。总量恰为 64 MiB 合法，超过则返回 `Limit`。字段分别允许的最大值不能任意组合；例如每条 10,000 个 owners 会使总量远超 64 MiB，codec 不会截断记录来满足容量。

`liveEncodedBytes` 只描述当前编码图，不是物理磁盘预算。它不包含文件 envelope、旧页、未发布文件、catalog 历史、操作意图或尚未确认删除的文件，也不等于用户设置的缓存容量。后续业务 Store 必须分别统计真实占用与预留，不能凭本字段发放写入额度。

## 校验、组装与失败保证

`ValidatePage` 检查单页字段、对应集合、局部排序、路径及尺寸，不要求该页包含 owner 引用的工程或 Current 对应的 Entry。`ValidateManifest` 检查页引用、序号、计数、总字节和文件引用结构。它不打开文件。

`Assemble` 要求 Page 按 manifest 顺序提供。它先检查每页的 kind、ordinal、owner、记录数和编码尺寸与引用一致，再组装候选 Index，最后校验跨页排序、重复路径、owner 外键、nextProjectId 以及 Current 的完整关系。局部有效的页面不能绕过完整索引的关系校验。

调用者在组装前必须对每个 PageRef 执行真实 `Acquire/ReadAll`。仅有解码值不能证明文件摘要、卷身份或 generation 正确，本批纯 codec 测试中的合成引用也不能提供这项证明。

空 Index 合法，但存储身份、epoch 和 nextProjectId 仍须有效。它分为零页，manifest 的三类计数和页数均为零，liveEncodedBytes 恰为 96。空页本身不合法。

所有带输出参数的操作先准备局部候选，成功后才交换输出；失败保留调用方原值。解析拒绝错误记录长度、未知字段标志、非零保留区和多余尾字节。结果按以下含义区分：

- `Invalid` 表示结构、字段或关系非法。
- `Unsupported` 表示不支持的线格式版本。
- `Limit` 表示超过公开计数、路径或字节上限。
- `AllocationFailure` 表示内存准备失败。

同一输入同时存在多个错误时，不承诺错误检查的优先级。consumerFormat 的全部 `uint32` 值均被保留，codec 不替消费者判断哪些私有格式可用。

## 验证状态

增强配置与原始兼容配置均已完成最终运行。下表以证据目录中各 `*-full-final`、`*-original-final` 的 `report.json` 和 `output.log` 为准；较早 `final-review.txt` 中的 3,442 项属于审查时已读取的初次结果，不替代最终 3,445 项。

| 专项 | 增强配置 | 原始兼容配置 | 范围 |
| --- | --- | --- | --- |
| 业务 codec | 3,445 项通过 | 3,445 项通过 | 独立字节期望、三类记录及 manifest 往返、路径与关系拒绝、100,000 条目及当前映射、10,000 工程、跨页组装及容量边界。 |
| 普通 C++ 分配失败 | 561 项通过；15 个扫描场景，83 次注入 | 561 项通过；15 个扫描场景，83 次注入 | 页面和 manifest 编解码、校验、Partition、Assemble；失败时输入和原输出保持。 |
| 文件层回归 | 864 项通过，跳过 0 项 | 864 项通过，跳过 0 项 | 原有文件身份、租约及目录保护，并验证文件层接受新增 `IndexPage` 种类。 |
| 文件层 I/O 故障 | 251 项通过 | 251 项通过 | 原有真实文件写入、刷新及删除故障回归。 |
| catalog 基础回归 | 1,303 次检查通过 | 1,304 次检查通过 | 原有索引发布回归；计数包含 `WaitFile` 有界等待的轮次，受实际调度影响，不是固定用例数。 |

前两项只操作 CPU 内存，没有缓存磁盘 I/O。文件层新增的 `IndexPage` 用例确实创建、读取和校验了文件，但负载只有 5 字节，不是有效业务 Page。它证明文件层能够保留新增种类及其字节，不能证明业务页面发布、manifest 与磁盘页对应、预算或业务 Store 已完成。

容量专项实际构造了编码总长 67,108,864 字节、66 页、822 个 Entry 的边界数据并通过。这是内存编码尺寸的证据，不是 64 MiB 磁盘预留、磁盘满或文件删除测试。

分配注入范围是当前线程普通 `new/new[]` 的单点故障，不覆盖对齐分配、CRT `malloc`、Win32/CNG 分配或多点同时失败。两个无普通分配的局部校验场景明确记录为零分配，没有声称发生了故障注入。

三个受控错误实现均触发预期断言，结果保存在 `negative-results.json`：允许已退休条目被 Current 引用、漏计 manifest 编码预算、忽略工程总数。反例检查同时涉及返回类别和原输出保持，不能据此声称每个字段都有独立反例。脚本在 `finally` 中按原字节恢复修改的生产文件并核对相等；恢复构建和随后两配置最终运行均通过。

`build-full-final.json` 与 `build-original-final.json` 记录两套 `MikuMikuStudio` 产品及本批专项构建退出码均为 0。`product-pe-final.json` 中两产物均为 x64、子系统 6.1；直接与延迟导入符号、效果资源 117/118 与基线一致。检查脚本同时断言正式 `build/release/MikuMikuDance.exe` 的 SHA-256 保持不变，正式稳定产物未被替换。

DSH 只承担 `src/io/cache_business_page.cpp`，会话为 `df3c430d-9773-4374-ab82-78e015211224`。派发及正式结果中的工作区和 cwd 均为 `D:/MMD-Next/MMS-abi-dev`，正式结果已读取。主任务完成整合、错误结果分类修订、构建和验收，DSH 的正常结束不替代这些证据。独立生产审查 `final-review.txt` 未发现阻断，其较早计数已由本节最终运行结果补齐。

Win7 SP1 x64、AMD 真机、断电持久性以及真实业务页面存储和消费者验收未由本批覆盖。原始兼容配置通过内部 codec 专项也不表示已启用产品缓存功能。

实现分别位于 [validation](../../src/io/cache_business_validation.cpp)、[page](../../src/io/cache_business_page.cpp)、[manifest](../../src/io/cache_business_manifest.cpp) 和 [wire](../../src/io/cache_business_wire.hpp)。后续仍须实现有来源的操作恢复、物理页面发布、业务预算与租约、工程关联、固定及清理，再接入效果、普通纹理、ABI 事务纹理和管理窗口；本批没有增加第二套历史或改动标准 VMD 指标。
