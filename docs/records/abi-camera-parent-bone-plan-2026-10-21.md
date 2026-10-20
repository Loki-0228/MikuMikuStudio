# 相机父骨骼下拉框的共同历史迁移计划

本记录仅为 2026-10-21 的源码审计和待实施方案。控件 450 的共同历史迁移尚未实现，本方案未进行构建或运行测试；下列验收项均为计划，不能据此登记为已完成。

## 现有入口与写入边界

实际入口为 `command_dispatch.cpp::DefaultSelChangeChain` 的控件 450 `CBN_SELCHANGE` 分支。它通过 `SelectedComboBone` 读取下拉框的 item data，写入 `CameraParentBone()`，再调用 `RefreshRequest(-1)` 并将焦点交给主窗口。当前没有受控历史路由。

该操作只改变父骨骼索引，不保证世界画面保持不动。`debug_geometry.cpp::ResolveFrameFocus/SetupFrameWorldTransform` 会从新骨骼当前的 `matInit` 和 `position` 取得焦点及绑定旋转。不得增加 `CarryEyeForFocusChange` 补偿，也不得调用 `PlanCameraFollow(true)`；后者还会采样相机、灯光、阴影、重力和附件。

历史记录只拥有一个 `int32_t` 字段：`CameraParentBone()` 的 before/after。相机位置、角度、offset、distance、FOV、perspective、attachment basis、suppressed、follow，以及模型未登记姿态均保持当前值。由于索引发布不依赖骨骼姿态计算，首次提交和短候选都不冻结骨骼 pose，不保存旧矩阵供以后覆盖。

PMM 当前父骨骼由 `pmm_save.cpp` 的 config2 写入四字节，由 `pmm_load_v2.cpp` 对应读取；相机注册后，`CameraKey` 同样保存父模型和父骨骼。迁移不自动注册关键帧，不修改 PMM、VMD 或其他标准格式布局。

## 记录身份与候选有效期

建议独立增加 `native_camera_parent_history.hpp/.cpp`，提供内部 `PlanCameraParentBone`、`TrySelectBoundCameraParentBone` 和 `RefreshBoundCameraParentBoneReadout`。这些名称为待实施接口，不表示已有实现；普通 `mms.edit` 的开放边界保持不变。

长期记录保留以下来源：

- 工程 `SceneGeneration`、相机池逻辑 identity、绑定的 `SharedHistory*`。
- 父模型 slot、原始指针、稳定 owner ID 和 `RetainedSceneObject`。
- `boneTable`、`boneCount` 和 before/after 骨骼索引。

检查顺序必须先确认绑定、工程、模型在 slot 中的真实成员关系和稳定 owner，再解引用表。当前 `CameraParentModel()` 必须仍指向该父模型；不能把旧模型的骨骼索引写入新父模型。表替换、数量改变、不同 owner 或工程替换均拒绝。相机池容量增长可接受，逻辑 identity 替换必须拒绝。

首次提交额外检查捕获时的 history revision、binding generation、对象 generation 和原始父骨骼值。每次 `Prepare` 的短候选重新捕获当前对象 generation、binding、播放头和父骨骼值，发布前再次核对，并检查候选 serial、消费状态和资源账本。

不能将首次对象 generation 锁定为长期重放条件：删除后恢复同一模型保留稳定 ID，但会改变 runtime generation。恢复后可以重新准备重放；删除或恢复期间已经准备好的旧候选必须失效。重放不锁定最初播放头，也不恢复旧播放头。

## 事务、预算与刷新

使用 `NativeIntent::Adjustment`。全部操作模式形成一个节点；显式提交模式只改变当前索引，不增加节点。同值不提交并保留 redo。绑定入口的 pending、忙、过期、预算和分配失败均须消费通知，恢复实际控件选择，不回落旧写入；未绑定及 original 路径保留原有行为。

准备阶段完成候选和预算所需分配，复用 `RetainedSceneObject::MeasureBookkeeping/CoversBookkeeping`，通过动态 `TargetRetainedBytes` 计入真实保活账本，不能仅按索引四字节计费。最终发布无分配，只写父骨骼索引和记录自身状态；`RequiresPhysicsReset()` 返回 false。通知失败不推翻已成功提交的数据。

初次提交后使用 `updates::RequestViewport(app, false)`，排队 `RenderDirty | PresentDirty`。渲染从当前骨骼和相机参数更新显示、重建投影。索引变化本身不设置 projection pending，不 seek、不求值轨道、不重置物理。不得使用 `ApplyCurrentCameraView/PostModelReload` 作刷新：它们会改 basis、offset、distance 或 suppressed。

`RefreshRequest(-1)` 的真实作用是选择相机全局轨，并清除附件行选择；`PostLanguageSweep` 在模型模式还可能调整 `selectedBone`。这一选择语义只能用于已接受的初始用户动作，包括已确认的同值动作，不能提前在准备或拒绝路径执行，也不能用作 undo/redo 的通用刷新。实施时应以传入 app 和实际面板上下文处理选择，避免依赖全局 `g_Block` 进行泛用刷新。历史不恢复旧 selection；共同撤销保留现有只读 UI 收尾。

## 下拉框同步

当前 `RefreshBoundCameraFollowReadout` 不同步 450，纯父骨骼变化也不会触发 `RunBoundObjectHistory` 的对象列表变化分支。新读回必须挂入共同 readout，覆盖直接撤销、重做及插件延后 UI 刷新。

初始通知核对控件 HWND、449 的父模型映射、450 的选中 item data、索引范围和可选骨骼类型。恢复选择时按 item data 查找，不能将 bone index 直接传给 `CB_SETCURSEL`：`FillBoneCombo` 会过滤部分骨骼，行号不等于索引。

若列表与当前父模型不匹配，准备当前模型的项目数据后重建该列表，再按数据恢复选择；不沿用旧模型行号或重复 item data。Win32 调用后重验 HWND、owner、表及数量。读回须阻止通知重入并保留未提交文本；UI 写入失败安排修复，不回滚已提交的索引。拒绝路径应尽量不分配地恢复旧选择，不额外转移焦点。

## 必要验收

- 从真实 `CommandDispatch` 的 450 `CBN_SELCHANGE` 进入；覆盖过滤骨骼、非连续索引、无效或重复 item data、父模型映射过期及实际选择恢复。
- 两种记录模式、同值保留 redo、pending 排他、预算拒绝、完整分配失败 sweep、取消和无分配发布；拒绝只恢复控件选择，不改变工程索引、轨道或附件选择，也不产生历史节点。
- 真实共同历史 UI 路由，在相机及模型模式下撤销重做；直接和延后刷新均同步 450，保留未提交文本、其他 selection 和未登记 pose。
- 同一父模型删除、恢复后仍可重放；替换 owner、表、数量、工程或相机池后拒绝。准备期间删除再恢复，旧短候选失效。
- 用不同骨骼的实时矩阵验证焦点随绑定切换；相机位置、角度、offset、distance、FOV、perspective、basis、suppressed、播放头及物理状态保持。该验收不要求世界画面固定。
- 与跨模型、插件、follow、相机注册历史交错；撤销重做不读原模型文件，不隐式注册关键帧。源码核对和实际标准 PMM 往返应分别记录，不能相互替代。

可复用 `native_camera_follow_checks.inc` 的真实入口、草稿保护、owner 预算和共同历史夹具，以及现有投影与视口请求检查。CPU 源码或隐藏窗口测试不能替代实际 GPU 绘制、Win7 SP1 x64 或 AMD 真机验收；实现继续保留这些兼容要求，不增加特定厂商或新系统接口的基础依赖。

源码定位：`src/window/command_dispatch.cpp`、`include/mikumikustudio/bone_combo.hpp`、`src/render/debug_geometry.cpp`、`src/window/ui_refresh.cpp`、`src/window/ui_model_reload.cpp`、`src/plugins/native_camera_follow.cpp`、`src/plugins/native_camera_follow_ui.cpp`、`src/plugins/native_object_commands.cpp`、`src/plugins/scene_identity.cpp`、`src/plugins/scene_object_history.cpp`、`src/app/update_coordinator.cpp`、`src/app/frame_driver.cpp`。
