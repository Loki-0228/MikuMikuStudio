# 编辑事务内核与原生撤销接入

`src/plugins/edit_store.cpp` 已实现 `edit.h` 对应的两阶段事务内核。当前宿主尚未提供原生统一历史实现，因此 `queryService("mms.edit", ...)` 仍返回 NOT_FOUND。内核测试不代表原生统一撤销完成。

## 已实现的事务规则

begin 校验非零场景修订号，并复制标签。事务创建者负责 prepare、commit、cancel；其他插件可通过自己的上下文 join 该事务，但只能交付本插件的 participant。每个插件在同一事务中最多加入一次，并须已注册 serializer。

prepare 在宿主编辑锁内重新校验修订号和 serializer 注册代次。所有 participant.prepare 成功后，宿主才读取 before/after 不透明字节，深复制并释放借用租约，最后为统一历史预留发布资源。任何一步失败，取消全部已经接受的 participant，消费事务，不追加历史。

commit 只接受已准备事务，在同一编辑锁内再次检查场景修订号及 serializer 注册代次。检查通过后消费各 participant 的 context，最后调用 `EditPublication::Publish()`。发布对象须在 prepare 阶段完成内存分配；Publish 只交换状态和已经准备好的历史节点，不运行插件回调或消息循环。

有效的 commit 尝试不论成功还是失败都会消费事务。重复 commit 返回 STALE；重复 cancel 不产生影响。prepare 后不能 join；未 prepare 的 commit 返回 CONFLICT，仍允许修正或取消该事务。插件停止前，`EditStore::Retire` 取消其创建或参与的未完成事务。

违反无异常 commit 契约时，内核停止接受新编辑。不对已消费 context 再调用 cancel，也不将半提交状态标为成功；退休返回失败，宿主接入时须据此保留实例和 DLL。原生插件在进程内执行，这一保护不能消除违规回调自身造成的状态改动。

`StorageStore::PrepareRestore` 只恢复历史记录包含的插件。缺失 provider 时不调用其他候选；任一 prepareLoad 失败时取消已经准备的数据，不改变当前状态或历史游标。恢复不会覆盖 `readSaved` 所指的磁盘保存基准。

## 原生恢复的准备与发布

增强构建已将 `UndoModelEdit` / `RedoModelEdit` 接到 `native_history::ModelEdit`。内部声明见 `include/mikudancestudio/native_edit_history.hpp`，实现见 `src/model/native_edit_history.inc`。原始兼容构建保留旧路径。这一步改造原生恢复过程，尚未把原生和插件记录排入共同历史。

`Prepare(model, frame, redo)` 复制本次操作涉及的姿势、关键帧及辅助状态，分配 redo 缓冲，准备缩放和曲线侧表。关键帧按被修改的槽位保存，不复制整个骨骼关键帧池。类型 4 的连续操作整体准备，最多检查 30 个环槽；损坏循环直接拒绝。覆盖范围如下。

| 原生记录 | 恢复内容 |
| --- | --- |
| 1、3 | 未注册骨骼位置、旋转、编辑标志；类型 3 另恢复当前帧。 |
| 2、4 | 完整骨骼键记录、姿势及链式操作的中间 redo 状态。 |
| 5 | 烘焙键、最大帧、分配游标、骨骼物理开关及缩放键。 |
| 6 | Morph 全池、曲线及切线、最大帧和分配游标。读取真实散布操作产生的 before/after。 |
| 7 | 删除前的骨骼键、Morph 键、显示键、IK 标志和外部亲骨骼选择值；保留活跃数组的所有权。 |

`IsCurrent(frame)` 检查本次读取的数组身份、容量、环槽、快照内容、原生值及侧表。`Publish(frame)` 再次校验，随后只复制准备好的值和交换容器，不分配内存、不发送窗口消息、不求值、不调用插件。候选只能发布一次；放弃候选不改活跃状态。调用者必须保证模型仍存活，并从准备到发布一直串行化编辑。该内部指针接口不能直接暴露给插件，也不能代替宿主的对象代次检查。

`Refresh(physicsMode)` 在发布之后同步物理标志和界面，按原生类型执行必要的重新求值。它不属于无分配发布阶段。按钮和快捷键改用返回成功状态的 `TryUndoModelEdit` / `TryRedoModelEdit`；准备失败时不继续修改工程 dirty 标记或撤销按钮状态。

MSVC 的某些容器移动构造会分配内存，因此包含 `std::map` 的状态对象不能用整体 `std::swap` 实现发布。缩放、骨骼自由曲线及 Morph 层改用各容器自身的 `swap`。回归测试在发布期间禁止 C++ 分配，验证这条约束。

当前恢复候选要求快照容量不超过仍存活的池。池被替换或缩小、模型被删除等情况须由共同历史层使旧引用失效；恢复辅助不会为了应用过期记录而改写新对象。各原生编辑入口创建历史的过程尚未全部改成两阶段提交。

## 宿主接入边界

`EditHost` 是本体内部接口，不进入公共 C ABI。

| 方法 | 本体责任 |
| --- | --- |
| `Revision` | 返回统一场景编辑修订号，覆盖关键帧、未注册姿势、参数、绑定与插件工程状态；不能仅返回绑定几何快照的观察修订号。 |
| `Enter` / `Leave` | 串行化原生和插件编辑；在加载、保存、播放求值、烘焙或其他原生修改未完成时拒绝进入。 |
| `Reserve` | 准备一次操作的原生候选及历史节点，保存插件 before/after，验证所有目标代次。失败不修改活跃场景或历史。 |
| `EditPublication::Publish` | 无分配地交换候选、追加一个共同历史节点、废弃对应 redo，并递增统一修订号。业务事件在完整发布后排队。 |
| `History` | 从共同历史选择 undo/redo，先准备全部原生和插件恢复数据，再一起发布。失败保留历史位置。 |

内核不拥有第二个插件专用撤销栈。测试中的 `EditHostFixture` 只用于验证事务协议，不能作为 MMS 的生产历史实现。

## 当前原生撤销入口

现有历史主要是每模型 30 槽 undo/redo 环，记录布局见 `include/mikudancestudio/undo_layout.hpp`。以下路径须进入共同历史，不能只修改界面按钮。

| 入口 | 当前内容及接入要求 |
| --- | --- |
| `bone_edit_undo.cpp::PushBoneEditUndo` | 未注册骨骼姿势；分配及捕获须先完成，再登记共同历史。 |
| `model_keyframe_edit.cpp::AdvanceUndo`、注册/删除 | 类型 1–4、7 的恢复已整体准备；记录的创建仍须登记共同历史。 |
| `model_keyframe_edit.cpp` 物理烘焙 | 类型 5，同时含关键帧、姿势、容量及搜索游标。须与骨骼缩放和曲线侧表一起恢复。 |
| `sequence_scatter.cpp` | 类型 6，持有完整 Morph 池及曲线状态的 before/after。不得退化为只恢复当前值。 |
| `vmd_load.cpp` | 动作导入；一次导入及其缩放、曲线数据应形成共同历史节点。 |
| `command_control_400.cpp`、`command_control_450.cpp`、`command_file_menu.cpp` | 存在直接写环的注册、粘贴或清理路径；仅接管 `AdvanceUndo` 无法覆盖。 |
| `frame_line_edit.cpp`、`ui_editor_click.cpp`、`misc_dialogs.cpp` | 时间线和对话框直接写环；须先准备快照与内存。 |
| `playback_state.cpp`、`oni_skeleton_pump.cpp` | 播放和捕获相关姿势/键记录；需明确一次操作边界，避免按帧无限追加。 |
| `bone_scale.cpp`、骨骼/Morph/全局曲线侧表 | 模型环关联的缩放、骨骼曲线和 Morph 散布恢复已准备后交换；全局曲线及独立编辑器历史仍待接入。 |
| `command_control_400.cpp`、`key_ladder.cpp`、`UndoModelEdit/RedoModelEdit` | 已统一调用新的模型恢复过程并识别失败；共同历史选择仍待实现。原始兼容构建保留旧路径。 |

删除对象、重开工程和重用环槽时还需使相关原生历史引用失效。当前绑定快照的 ID/代次与工程保存机制可复用，但快照内容本身不能代替完整可撤销编辑状态。

## 开放服务前的验证

需在真实宿主覆盖骨骼姿势、注册键、删除键、VMD 导入、物理烘焙和 Morph 散布，与插件状态改动交错后的 undo/redo；并覆盖跨模型、相机模式、对象删除、环槽回绕、工程重开、缺失插件和 schema 拒绝。prepare 或历史预留失败时，原生状态、插件状态、dirty 标记、修订号和历史游标均须保持不变。

Win7 和 AMD 真机项目暂列待验，不阻塞上述实现与本机验证。接口继续采用 Win7 目标、CPU 基础实现和厂商无关数据，不加入新系统或专有 GPU 的必需依赖。
