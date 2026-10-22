# main 新增功能的 ABI 迁移核对

日期：2026-10-22，更新于 10 月 23 日。初次核对基线 `3ccb61e`，本批实现基线 `c71b2dc`。本记录核对 main 的 `0fe9fe0`、`3782e0c` 及后续 `fed9768`，只描述差异和迁移顺序，不将 main 的测试结果视为 ABI 验收。`3782e0c` 涉及 141 个文件，不能用整体覆盖替代逐模块迁移。

## 当前状态

| 功能 | ABI 状态 | 下一步 |
| --- | --- | --- |
| 大场景近处自阴影精度，`0fe9fe0` | [已接入及本机验证](abi-preread-shadow-parent-2026-10-22.md)。 | 数值聚焦覆盖及实际产品字节码绘制通过；原始兼容实现保持，本轮未作真机或人工视觉验收。 |
| 近景阴影裁切及 PMM 可逆编码，`fed9768` | [已接入及本机验证](abi-main-fed9768-2026-10-23.md)。 | 处理实际 AA 和参考变换，路径分开检查 MMS 可容纳性与原版副本 CP932 可读性；两配置构建及 14 次运行通过。另已修复并验收[满 20 字节 PMD 身份读写](abi-pmm-identity-2026-10-23.md)。 |
| PMM 标准体预读 | [已接入及本机验证](abi-preread-shadow-parent-2026-10-22.md)。 | 普通文件 64 KiB 窗口、唯一 owner、逻辑位置和真实扩展交接通过；耗时收益尚未测量。 |
| PMM 模型适配名称及 ABI 扩展保护 | [本机验收通过](abi-pmm-model-adaptation-2026-10-22.md)。 | 已处理实际适配槽的名称、身份、父骨曲线及已知网格缓存；同数量几何指纹、skipped 扩展删除及未来格式范围另列。 |
| 文件解析 CPU 几何缓存 | [已接入及本机验证](abi-geometry-history-2026-10-22.md)。 | 独立后端两配置各 10,215 项，真实加载器完整／原始配置各 545／540 项通过；原始配置禁用生产缓存。不据此宣称提速。 |
| PMX 顶点并行准备 | [已接入及本机验证](abi-pmx-parallel-2026-10-22.md)。 | 最多两个 CPU worker，保留整份缓存和原串行回退；两配置独立 CPU 各 3,003,900 项，实际加载器 695／688 项通过。不据此宣称提速。 |
| 缓存管理、效果编译缓存及纹理准备缓存 | 尚未迁移。 | 先统一 data 与设置事务，再接各生产加载入口及失败回退。 |
| 透明排序范围、精度和可见区域 | [CPU 策略已验证](abi-transparent-policy-2026-10-23.md)，生产仍使用旧入口。 | 接入源 VB/私有 IB、范围菜单及 MMS 工程字段，验证实际提交与像素、各路径回退、内存预算及原配置；缓存和可选 D3D12 路径另验。 |
| 手动保存版本 | [已接入及本机验证](abi-numbered-save-2026-10-23.md)。 | 版本链硬中断恢复及新 EMM 同快照完成仍待实施。 |
| 菜单首次绘制 | [已接入及本机验证](abi-menu-initial-paint-2026-10-23.md)。 | 两配置消息、生命周期与生产入口检查通过；真实菜单像素、完整 MME 环境和平台验收仍待完成。 |
| 编辑控件修复 | [长数值前缀](abi-transform-prefix-2026-10-23.md)、[未绑定旋转](abi-unbound-rotation-2026-10-23.md)及[物理接管](abi-physics-takeover-2026-10-23.md)已分别验证。 | 保留 ABI 事务顺序及原配置，其余入口继续逐项核对。 |
| 负数关键帧 | 尚未实现。 | 使用共同历史、稳定对象身份及独立扩展数据，不能引入第二个撤销栈。 |

上一轮 DSH 正式 CLI `status` 返回 `fetch failed`，未进入阴影任务派发，用户随后暂停。用户于本轮恢复 DSH；真实宿主核对工作区后承担独立原生菜单快照及 HUD 预览回滚的原子修改，Codex 负责设计、审查、文档及实际验收。菜单辅助模块两配置各 66 项通过，附件父绑定、删除与相邻历史十组共 95,042 项通过。完整 UI 目录尚未接入，详见[本轮记录](abi-geometry-history-2026-10-22.md)。

## 缓存与读取的分批方案

ABI 已有 `model_vertex_cache`、`import_texture_cache` 及 `effect_file_watch_cache`，分别用于渲染缓冲、事务导入资源及文件监测。它们不等于 main 新增的解析几何缓存、可再生像素缓存和编译字节码缓存。

| 批次 | main 主要源码 | ABI 适配及验收边界 |
| --- | --- | --- |
| PMM 预读 | `pmm_read_buffer.hpp`、`pmm_io_common.hpp`、v1/v2 读取器 | 接入 64 KiB 预读和逻辑位置，保留短读偏移、扩展尾部交接、descriptor 复用及关闭 hook 语义。 |
| 模型适配名称 | `extended_names.cpp`、v2 workspace、`pmm_project_settings` | 把实际适配 slot 传给名称加载器，旧扩展字典不得覆盖替换模型的名称。可与预读分开。 |
| PMX 并行准备 | `pmx_vertex_prepare.hpp`、`pmx_vertex_parallel.hpp`、`pmx_load.cpp` | 大模型最多两个 CPU worker，读取留在调用线程；保留浮点状态、取消 join 与串行回退。不得缩减既有附加 UV 消费能力。 |
| CPU 几何缓存 | `model_geometry_cache.cpp`、PMD/PMX loader、`SequentialFileReader::SeekAbsolute` | main 的默认预算为总量 256 MiB、单项 128 MiB、32 项。命中后复制到新数组，完整加载成功后发布，不共享可变模型数组。此批不依赖并行解析。 |
| 缓存管理 | `cache_manager.cpp`、公共头及存储策略 | 预算、工程关联、固定保留、使用租约、跨进程写入预留及清理先作为独立后端验收。 |
| 效果编译缓存 | `effect_compile_cache.cpp`、`effect_paths` | 依赖管理后端。命中后创建独立 effect 及诊断对象；未知编译器、自定义 include 或缓存失败继续原调用，不缓存 live effect。 |
| 纹理准备缓存 | `texture_prepare_cache.cpp`、`path_resolve.cpp`、ABI 导入准备加载器 | main 当前通过效果缓存的 `ResolveD3DX` 解析 DLL 身份，需要明确依赖。覆盖普通加载和事务捕获路径，D3D 创建及外层 hook 注册留在加载线程。 |
| 解码与管理窗口 | `texture_cpu_decode.cpp`、`texture_prepare_cache::Batch`、`cache_manager_dialog.cpp` | PNG/TGA worker 只处理自有字节；窗口异步扫描及清理，不写工程历史。并行解码和窗口可以继续拆为两个任务。 |

主线迁移必须保留下列已有约定。

- main 默认使用 `EXE/Cache` 和独立 `data/cache.ini`。ABI 的可再生缓存应从 `local_data::File(...)` 派生至 data 内，用户设置进入已有 `data/settings.json`，保留未知字段和跨进程写入语义。索引等可再生数据可以使用独立二进制文件。
- ABI 的 `pmm_io::Input` 唯一拥有输入描述符，`Close()` 先消费 owner 再关闭；不得复制回 main 的旧关闭架构。缓存工程切换也必须晚于 `BeginProjectSceneReplacement()` 成功，拒绝替换时保留旧场景的租约。
- ABI `path_resolve.cpp` 先进入 `TryLoadCaptured`。只包装普通加载不足以覆盖事务导入；还要接入 `import_texture_cache` 的默认准备加载器，保留错误分类、COM 所有权及完整 tag，不能破坏 261、278 和对象删除恢复。
- 可再生磁盘数据与共同历史保活资源分别计预算。磁盘清理不得释放历史持有的模型及 GPU 资源，也不得清理工程自己的几何 sidecar。
- main 的部分工程设置键仍为 `org.mikudancestudio...`。本分支继续使用 MMS 的键和文件标记，不恢复 MDS 读取兼容。

10 月 23 日继续审查 `3782e0c` 的管理后端，确认首批还必须处理以下问题，不能直接复制实现后接入消费者。

- Win32 mutex 的所有权必须由独立 RAII 成员持有。构造途中分配或同步失败时，也必须释放已获得的锁。
- 写入预留不能跨线程搬移仍持有线程锁的对象。线程、进程之间的容量竞争和失败退出需要真实并发测试。
- 清理必须验证并删除同一文件身份。不能先检查路径和文件头、关闭句柄，再按路径删除可能已被替换的文件。
- 未受管理的路径不能默认获得磁盘写入许可。设置损坏、读取繁忙、索引损坏和预算不足必须拒绝相应操作，不能重新采用启用默认值旁路限制。
- 索引刷新需要保留尚未刷盘的访问记录，不能只用修改时间决定覆盖内存状态。显式清空损坏索引也必须保留不属于管理器的文件。

已有 `mesh_bake_cache` 与上述可再生缓存不同：源 ABC 删除后，私有副本可能是烘焙结果的唯一来源，保存快照仍会持有它。其文件不能加入效果和纹理缓存的自动淘汰队列。运行数据路径另按[烘焙目录核对](abi-mesh-data-2026-10-23.md)修正和验收。

后端实施边界已继续对照 main 的实际消费者核对。`fed9768` 的管理器相对 `3782e0c` 没有变化。下一批采用可实例化的 `Store(absoluteCacheRoot, absoluteSettingsFile)`，生产包装固定 `data/cache` 和 `data/settings.json`，测试传入自有路径，不复制 main 的生产环境变量路径覆盖。

设置使用现有 `system_settings::Store::WriteSection` 或 `CompareExchangeSection` 更新 `sections.cacheManager`，保留未知字段。预算以 `limitMiB` 存储，不能用返回 uint32 的 `Json::UInt()` 读取 4 GiB 字节值。缺少 section 可以使用约定默认值；已存在但损坏、忙碌或不可读时应拒绝相应缓存操作。设置保存与随后清理是两个结果，不能声称它们是单个可回滚事务。

写入预留应持有独立文件锁句柄，避免跨线程转移线程所有权锁。读租约持有实际文件句柄并禁止共享删除；清理在同一带 DELETE 权限的句柄上核对文件身份和格式，再调用 Win7 可用的 FileDispositionInfo 删除。预算不足时先确定并持有足量可回收候选，候选不足不得先删除部分文件。实际删除或索引落盘失败应报告部分结果，发布与索引之间还需要可恢复状态，不能使已发布文件漏计后继续接受写入。

实际效果和纹理消费者均依赖 Root、Enabled、Touch、Invalidate、写入预留和 Commit；迁移后还必须在读取前获取租约。租约不能仅绑定一个进程级当前工程，应允许多个场景和视口持有。工程关联切换必须晚于 ABI 场景替换获得许可，加载拒绝时保留旧场景资源。

后端批验收需覆盖设置合并、真实线程和进程争抢预算、进程退出释放、租约阻止清理、路径替换不误删、索引损坏、失败无部分发布及安装隔离。后端测试通过仍不代表用户功能完成。至少接入一个真实消费者和可打开的缓存管理窗口，支持启停、容量、统计、固定及清理，并验证保存重开和失败回退后，才能把首个用户功能标为完成。其他消费者、事务导入和性能验证继续分别保留。

## 负帧的共同历史与格式边界

main `negative_frames.cpp` 的 `State` 自建最多 30 步 undo/redo，按当前帧分流撤销；跨零粘贴还直接修改姿态、调用正帧 registrar。直接把 `negative_frames::Command()` 插到 ABI 命令入口前，会先于共同历史消费撤销、重做和粘贴。它也没有既有历史预算、准备失败回滚及跨模型合并语义。

负帧数据目前保存数值，并采用线性插值或骨骼旋转的归一化插值。没有原生 Bezier 与自由曲线作者信息。ABI 的首键 0 锚点、稀疏轨道及登记计划仍拒绝负帧，不能只放宽校验并宣布支持。模型及附件地址也不能替代稳定对象身份和删除恢复代次。

静态核对还发现 main `WritePmmConfigBlock` 忽略 `ResolveRange` 的返回值。输入播放范围 `-10..-1` 时，该函数先得到 `standardStart=0、standardStop=-1` 再返回 false，而 writer 随后仍写这两个变量。迁移时应拒绝无效播放范围，或使用明确的合法非负标准值并只在扩展中保存负编辑范围。此结论来自源码推导，尚未执行实际失败文件用例。

负帧应按以下依赖顺序实现。

1. 增加签名编辑帧上下界及导航，跨度、滚动与粘贴偏移用 64 位运算。标准文件仍用非负 `Parse/Add/Convert`，不能修改原 VMD 无符号字段含义。
2. 建立稳定对象身份下的负帧数据版本及 `HistoryChange`，先验证登记、删除、选择、取消和预算，再把跨零粘贴组合为一次事务。历史步数与模式继续使用同一系统设置。
3. 接入求值、插帧和曲线编辑，保留原生非负池及 0 帧锚点；明确跨零区间的原生及自由曲线语义，不用线性侧表代替完整曲线能力。
4. 接入 `MMSXPMM2` 扩展、损坏输入处理及工程重开。负帧本身不强制升级 PMM0003；标准 PMM 前缀仍保持非负。
5. 核对登记按钮、菜单、快捷键及播放范围，编辑允许负帧、实际播放保持非负。VMD 对不能表达的负帧应明确报告并提供可选转换流程，不静默遗漏，也不更改原格式指标。

验收至少覆盖 `minimum/-1/0/1/maximum`、跨零混合粘贴、准备与预算失败、两种模式、跨模型删除恢复、全部登记通道、曲线跨零求值、PMM 游标及范围往返、损坏扩展和原有非负文件字节兼容。负帧尚未形成 VMD 导出流程，main 的现有 VMD writer 只遍历非负池，不能把它的通过结果用于本项验收。

## 保存版本、菜单与编辑控件的静态核对

10 月 23 日初次只读比较 main 已提交的 `fed9768` 与当时 ABI 源码，确认下列五组行为尚未同步。main 材料通过 `git show` 和 `git diff` 读取，没有读取 main 工作区未提交的文件。下列相关实现文件在 `3782e0c..fed9768` 之间没有新增变化。本节保留初次核对的问题与验收方案，后续已完成项目在各小节及文末补充，不以初次静态核对代替运行证据。

### 物理模式下的编辑读数

本项后续已按[物理骨骼读数记录](abi-pose-physics-readout-2026-10-23.md)实现，两处旧代码分别触发失败反例，最终轴专项 3,218 项和数值回归 310 项通过。以下保留迁移前问题说明；它不代表后续重置语义和草稿问题已经完成。

当骨骼带刚体、`PlaybackPhysicsMode()==3` 且 `bonePhysicsState[bone]!=0` 时，main 的 `PostViewRefresh` 已改为显示编辑后的 `trans/rotQuat`。迁移前 ABI [ui_view_refresh.cpp](../../src/window/ui_view_refresh.cpp) 只判断物理模式和刚体属性，继续读取 `ikBackup`。ABI 自有的 [native_pose_axis.cpp](../../src/plugins/native_pose_axis.cpp) 中 `ReplacePositionDraft` 也使用同一旧判断。因此，修改或清零骨骼后，面板仍可能显示旧物理姿态。

最小适配是统一这两处读数来源：只有未标记编辑的骨骼才显示物理备份，已编辑骨骼显示当前作者姿态。修改不得新增撤销记录或写关键帧。专项用例可扩展 [native_pose_axis_checks.inc](../../tests/native_pose_axis_checks.inc) 与 [native_pose_numeric_checks.inc](../../tests/native_pose_numeric_checks.inc)，通过现有 `native_edit_history_tests --pose-axis-only` 和 `--pose-numeric-only` 入口检查主窗口、分离窗口、位置、旋转、刚体标记、物理模式及编辑标记的组合。

### 重置后的未提交文本

绑定旋转及 495 整体重置已分别按[旋转草稿记录](abi-rotation-drafts-2026-10-23.md)和[整体重置记录](abi-pose-reset-2026-10-23.md)实现并验证。普通 495 随后按[普通重置记录](abi-legacy-pose-reset-2026-10-23.md)完成，物理接管也已按[接管记录](abi-physics-takeover-2026-10-23.md)完成本机专项。[普通旋转入口](abi-unbound-rotation-2026-10-23.md)随后完成单骨旧环与局部草稿发布，专项 6,597 项及三个旧入口反例通过。以下保留迁移前差异及方案，不再列为该旋转入口的待办；真实平台验收及本体共同历史全面迁移仍有缺口。

ABI 的位置轴重置已经在成功后调用 `ReplacePositionDraft`，但 `TryResetBoundPoseAxis` 的旋转分支没有同等处理。与此同时，[ui_view_refresh.cpp](../../src/window/ui_view_refresh.cpp) 的 `SetReadoutText` 会保护共同历史绑定中的未提交文本，[ui_edit_commit.cpp](../../src/window/ui_edit_commit.cpp) 则会在失焦时提交草稿。输入旋转值后通过转发命令清零，再离开输入框，旧文本仍可能重新覆盖清零结果。

[command_control_450.cpp](../../src/window/command_control_450.cpp) 的 `ApplyBoundPoseCommand` 整体重置也没有 main 新增的六个坐标框草稿清理。未绑定共同历史的 `BoneRotEdit` 和分离窗口旋转路径，分别在 [command_control_500.cpp](../../src/window/command_control_500.cpp) 与 [wndproc_stubs.cpp](../../src/window/wndproc_stubs.cpp)，也缺少 main 对旋转修改标志的清理。

ABI 适配必须在事务成功后取消重置影响的草稿，不能直接复制 main 在操作前清除 `EM_SETMODIFY` 的顺序。忙碌、预算不足和准备失败应保留原草稿。成功的数据 no-op 也应取消目标草稿，同时保留 redo、不新增历史。其他输入框中未提交的内容继续保留。现有位置草稿测试已经覆盖这些边界，可在 `--pose-axis-only` 中补旋转用例；整体重置需要下面的独立命令测试入口。验收包括实际失焦提交，不能只检查清零瞬间的内存值。

后续静态核对确认，`RefreshBoundPoseReadout` 在非单位缩放尚未应用时会跳过 `PostViewRefresh`，连同 `BoneRotationEditDegreesX/Y/Z` 一并不更新。旋转重置收尾需要纯四元数／矩阵读数辅助函数，保持现有主窗口和分离窗口的浮点运算顺序；不能为刷新角度强制求值或安装缩放变换。目标及数学准备在事务前完成，派生缓存与目标草稿只在提交成功并重新确认对象、绑定和窗口身份后更新。其他轴草稿继续保留。

菜单 302 的单位旋转 no-op 和整体 495 的 anyChange=false 路径还须在取消草稿前检查共同历史导航及编辑门禁，防止在另一个已准备事务期间修改输入。整体 495 只取消本次实际影响且当前显示骨骼的位置、旋转和缩放九个框；空选择或显示骨骼不在选中集合时不取消草稿。增强版未绑定共同历史的旧入口也须单独适配和验收。

物理骨骼显式重置接管是迁移前的另一项真实行为差异。main 的合法重置会写 `bonePhysicsState=1`；ABI 原先作者值已零的 no-op 会保留 dirty=0，此时 mode 3 刚体骨骼仍显示物理备份。同步该行为应把正在使用物理备份的骨骼从 dirty=0 改为 1 纳入同一姿态事务，不能在显示刷新或事务之外补写标记，也不能静默丢弃 redo。普通非物理骨骼的重复零位仍按既有 no-op 约定处理。失败保留标记、备份、草稿及重做；撤销恢复物理读数，重做恢复作者读数。接管影响整根骨骼，必须验证其他分量的读数切换。原静态方案保存在本批证据目录 `reset-followup-design.md`；实现与测试现见上述接管记录，真实模拟和硬件仍未验收。

### 整体骨骼重置的目标姿态

绑定入口已按[整体重置记录](abi-pose-reset-2026-10-23.md)完成，新增独立命令测试入口及旧目标反例。普通启动未绑定路径随后按[普通重置记录](abi-legacy-pose-reset-2026-10-23.md)使用原撤销环实现并验收。以下保留原差异；物理 dirty-only 接管已另批实现，见上述接管记录。

main 的命令 495 已改为把所有选中骨骼的位移清零、旋转恢复单位四元数、缩放恢复单位值。ABI [command_control_450.cpp](../../src/window/command_control_450.cpp) 的共同历史路径仍保留根骨骼 X/Z 位移，并对其他无父骨骼执行旧旋转和根偏移计算。这使部分骨骼在整体初始化后仍保留偏移。

[native_pose_command_checks.inc](../../tests/native_pose_command_checks.inc) 目前也明确要求保留根 X/Z，并验证无父骨骼执行旧变换计算。这些期望需要随行为同步更新。最小修复只替换目标姿态构造，继续使用 `NativePoseScope`、一次共同事务及原有失败回滚，不引入 main 的旧 `PushBoneEditUndo`。无父骨骼归零不再需要 D3DX 参与。

`NativePoseCommandChecks` 当前仅在默认历史测试集合中调用，尚无独立参数。实施时应先增加明确的 `--pose-command-only` 入口，不运行默认全套历史测试。专项检查根骨骼、有父骨骼、无父骨骼、混合选择、空选择、重复重置、缩放、跨模型撤销重做、预算和分配失败，以及关键帧和旧撤销环保持不变。原始兼容配置的行为边界需单独核对，不能因完整配置修复而无意改写原始构建。

### 独立的手动编号保存版本

2026-10-23 更新：[编号保存](abi-numbered-save-2026-10-23.md)已完成本节 main 行为同步及 ABI 三成员归档。以下为实施前差异记录；版本链硬中断与新 EMM 同快照确认不属于本批已完成范围。

ABI 仍只有 `keepPrevious`，历史版本沿 `.pmm1/.pmm11/...` 链移动。main 新增独立的 `numberedManualVersions`，让手动保存生成 `.pmm1/.pmm2/.pmm3/...`，支持稀疏编号、孤立 EMM 和十进制进位。周期备份不使用此编号策略。main 备份对话框也增加了开关及当前 EMM 保存状态，ABI [autosave_dialog.cpp](../../src/window/autosave_dialog.cpp) 尚未接入。

ABI 选项需要沿实际保存事务传递：[autosave.hpp](../../include/mikumikustudio/autosave.hpp) 和 [autosave.cpp](../../src/app/autosave.cpp) 的设置及 JSON、[pmm_save.cpp](../../src/io/pmm_save.cpp) 的 `SceneSnapshot`、[integration.cpp](../../src/plugins/integration.cpp) 的 `ProjectSave::PublishDisk`、[project_storage.cpp](../../src/plugins/project_storage.cpp) 的 `PublishProjectPair`，最后进入 [pmm_save_versions.hpp](../../src/io/pmm_save_versions.hpp) 的 `Retention::Prepare`。

现有 ABI `Retention` 同时保护 PMM、EMM、插件 sidecar 和版本保存锁，不能被 main 的两文件发布实现替换。编号扫描还必须将孤立插件 sidecar 算作占用，并保留中断保存记录检查、移动前锁定、失败恢复及原项目配对身份。系统设置继续存入 `data/settings.json`，不能重新建立独立设置文件。

专项可扩展 [pmm_versions_checks.hpp](../../tests/pmm_versions_checks.hpp) 的 `VersionTests`、[autosave_tests.cpp](../../tests/autosave_tests.cpp) 和 [backup_dialog_checks.hpp](../../tests/backup_dialog_checks.hpp)。实际保存入口为 `project_save_export_tests <隔离输出目录>`。新增覆盖应包含 9 到 10 的进位、稀疏版本、孤立 EMM 和插件 sidecar、版本占用、发布失败回滚、编号模式切换，以及周期和崩溃副本不受手动编号开关影响。

### 菜单首次绘制与长输入框的前缀

ABI 尚无 main 的 `menu_initial_paint` 模块。[wndproc.cpp](../../src/window/wndproc.cpp) 没有其消息处理，[effect_display.cpp](../../src/exports/effect_display.cpp) 没有为 MME 编辑器及子控件安装补绘。main 按 owner 和 HMENU 跟踪弹出菜单，在首次可见、首次 idle 及可见约 220 ms 后补绘，并在菜单关闭或 owner 销毁时清理。这针对菜单首次打开时文本尚未正确绘制的情况。

适配可以先移入独立 helper，再接主窗口与 MME 编辑器，保留菜单命令归属和 ABI 菜单目录。验收需覆盖定时器退出、重入、多个 owner、延迟初始化及没有 idle 消息的菜单。main 的 `tests/menu_first_paint_checks.hpp` 提供了相关模式，但其中声明使用 inactive desktop，不能直接运行或引用为本会话 Default desktop 下的证据。ABI 尚无这些测试参数，需先增加受控入口，仅操作测试自己创建的窗口。

2026-10-23：[长数值显示](abi-transform-prefix-2026-10-23.md)已同步。`CreateUiPanelControl` 对 17 个目标取消居中和右对齐，EDIT 子类在失焦或非聚焦文本更新后回到文本开头，避免隐藏负号和整数前缀。ABI 另加入宿主、窗口、线程及焦点复核，防止回调改变上下文后继续滚动。

该显示修复保留 ABI 现有提交和草稿保护顺序，不覆盖透明度 485、表情权重等有界控件，并维持原始配置布局。真实 EDIT 专项在两配置分别通过 12,153、11,843 项，数值提交及两类旋转重置回归通过。三档字体和宽度模拟不代表真实 DPI 验收，菜单补绘仍待实施。

实施可拆为骨骼读数与重置、手动保存编号、菜单与输入显示三个独立批次。编号保存涉及跨文件事务，不能仅凭辅助函数通过或界面出现新开关就标记完成。

## 后续验收约束

2026-10-23 追加：[绑定整体重置 495](abi-pose-reset-2026-10-23.md)已完成局部零位、九框草稿取消和共同历史适配。完整配置命令专项 6,235 项、轴 4,185 项、数值 310 项通过，旧根偏移目标及缺少草稿处理的中间版本分别失败。main 核对仍为 `fed9768`。下一批须覆盖普通启动的未绑定 495，随后处理物理显式接管；不能以绑定路径替代用户产品路径验收。

2026-10-23 追加：[绑定旋转重置草稿](abi-rotation-drafts-2026-10-23.md)已适配共同历史。每次原生消息后检查对象与窗口身份，成功时取消目标草稿并同步派生角；无变化不清 redo，失败保留草稿。完整配置轴、菜单和数值专项分别通过 4,185、826、310 项，旧实现反例及恢复运行已留存。本批开始核对 main 仍为 `fed9768`。普通启动未绑定入口（包括 495）、物理接管、手动编号保存、菜单绘制及长输入显示继续待实现。

2026-10-23 再追加：[普通未绑定整体重置 495](abi-legacy-pose-reset-2026-10-23.md)完成局部零位、九框取消及原撤销环的准备后发布，普通插件初始化不启用共同历史绑定。最终与反例恢复后的专项均为 5,197 项通过，旧重置及旧失焦实现分别触发明确断言失败。首次缩放状态创建覆盖 198 个分配失败点，作者数据、旧环及草稿均保持；两个旧缓冲释放、实际原撤销与重做、混合选择也已验证。相邻命令、轴和数值回归通过，两产品导入表未变，提交前 main 仍为 `fed9768`。前文未绑定 495 待迁移事项在本批完成；未绑定旋转草稿、物理接管及其他待办保持。

先实施不依赖磁盘管理的预读、适配名称和有界几何缓存，再接缓存管理、效果和纹理。透明排序、控件修复及负帧分别审查。每批实际源码、失败反例、构建产物和运行报告均须留存，main 的验证不替代 ABI 的共同历史回归。Win7 SP1 x64、CPU 基础路径、AMD、data 目录及原始兼容配置要求保持；本轮没有新平台或视觉验收结果。
