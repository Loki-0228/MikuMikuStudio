# 编辑事务内核与原生撤销接入

`src/plugins/edit_store.cpp` 已实现 `edit.h` 对应的两阶段事务内核。当前宿主尚未提供原生统一历史实现，因此 `queryService("mms.edit", ...)` 仍返回 NOT_FOUND。内核测试不代表原生统一撤销完成。

## 已实现的事务规则

begin 校验非零场景修订号，并复制标签。事务创建者负责 prepare、commit、cancel；其他插件可通过自己的上下文 join 该事务，但只能交付本插件的 participant。每个插件在同一事务中最多加入一次，并须已注册 serializer。

prepare 在宿主编辑锁内重新校验修订号和 serializer 注册代次。所有 participant.prepare 成功后，宿主才读取 before/after 不透明字节，深复制并释放借用租约，最后为统一历史预留发布资源。任何一步失败，取消全部已经接受的 participant，消费事务，不追加历史。

commit 只接受已准备事务，在同一编辑锁内再次检查场景修订号及 serializer 注册代次。检查通过后消费各 participant 的 context，最后调用 `EditPublication::Publish()`。发布对象须在 prepare 阶段完成内存分配；Publish 只交换状态和已经准备好的历史节点，不运行插件回调或消息循环。

有效的 commit 尝试不论成功还是失败都会消费事务。重复 commit 返回 STALE；重复 cancel 不产生影响。prepare 后不能 join；未 prepare 的 commit 返回 CONFLICT，仍允许修正或取消该事务。插件停止前，`EditStore::Retire` 取消其创建或参与的未完成事务。

违反无异常 commit 契约时，内核停止接受新编辑。不对已消费 context 再调用 cancel，也不将半提交状态标为成功；退休返回失败，宿主接入时须据此保留实例和 DLL。原生插件在进程内执行，这一保护不能消除违规回调自身造成的状态改动。

`StorageStore::PrepareRestore` 只恢复历史记录包含的插件。缺失 provider 时不调用其他候选；任一 prepareLoad 失败时取消已经准备的数据，不改变当前状态或历史游标。恢复不会覆盖 `readSaved` 所指的磁盘保存基准。

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
| `model_keyframe_edit.cpp::AdvanceUndo`、注册/删除 | 类型 1–4，以及带 Morph、IK/显示辅助状态的类型 7；保留链式操作的一次撤销语义。 |
| `model_keyframe_edit.cpp` 物理烘焙 | 类型 5，同时含关键帧、姿势、容量及搜索游标。须与骨骼缩放和曲线侧表一起恢复。 |
| `sequence_scatter.cpp` | 类型 6，持有完整 Morph 池及曲线状态的 before/after。不得退化为只恢复当前值。 |
| `vmd_load.cpp` | 动作导入；一次导入及其缩放、曲线数据应形成共同历史节点。 |
| `command_control_400.cpp`、`command_control_450.cpp`、`command_file_menu.cpp` | 存在直接写环的注册、粘贴或清理路径；仅接管 `AdvanceUndo` 无法覆盖。 |
| `frame_line_edit.cpp`、`ui_editor_click.cpp`、`misc_dialogs.cpp` | 时间线和对话框直接写环；须先准备快照与内存。 |
| `playback_state.cpp`、`oni_skeleton_pump.cpp` | 播放和捕获相关姿势/键记录；需明确一次操作边界，避免按帧无限追加。 |
| `bone_scale.cpp`、骨骼/Morph/全局曲线侧表 | 与原生关键帧同时提交及恢复，不能只改 30 槽环而遗漏侧表。 |
| `command_control_400.cpp`、`key_ladder.cpp`、`UndoModelEdit/RedoModelEdit` | 界面按钮、快捷键及直接调用须使用同一历史选择；须保留原始兼容构建的旧路径。 |

删除对象、重开工程和重用环槽时还需使相关原生历史引用失效。当前绑定快照的 ID/代次与工程保存机制可复用，但快照内容本身不能代替完整可撤销编辑状态。

## 开放服务前的验证

需在真实宿主覆盖骨骼姿势、注册键、删除键、VMD 导入、物理烘焙和 Morph 散布，与插件状态改动交错后的 undo/redo；并覆盖跨模型、相机模式、对象删除、环槽回绕、工程重开、缺失插件和 schema 拒绝。prepare 或历史预留失败时，原生状态、插件状态、dirty 标记、修订号和历史游标均须保持不变。

Win7 和 AMD 真机项目暂列待验，不阻塞上述实现与本机验证。接口继续采用 Win7 目标、CPU 基础实现和厂商无关数据，不加入新系统或专有 GPU 的必需依赖。
