# 已保留物理对象的混合成员交换

后续已修正准备阶段对实时数组的扩容，改为私有数组与发布交换；新增取消、预算拒绝和空世界恢复回归，见[存储修复记录](abi-physics-transition-storage-2026-10-21.md)。本记录的早期结果不能代替该补充验证。

`history_physics::Retained::PrepareExchange` 已实现一次候选内的混合加入和移除。它处理同一个世界中已经保留的 present、parked 资源，旧 `PrepareBatch` 通过同一实现组合同方向请求。此接口属于本体内部 C++ 实现，不是新增公共插件 ABI。

## 准备与发布

每项 `PresenceChange` 指定原 Retained 及目标存在状态。对象必须真正改变状态，所有者唯一、资源不重叠且属于同一世界。准备先从私有成员序列移除 outgoing，再将 incoming 的历史顺序投影到目标成员；已移除的历史锚点不会重新进入世界，未参与对象保留相对顺序，冲突返回失败。

候选为移除项捕获新的 Saved，为恢复项使用已有 Saved，分别固定方向和所有者代次。世界步进、全部成员顺序、代理、刚体、接触缓存、算法指针和私有目标数组都参与最终校验。接触缓存只移除 outgoing 的 pair，并按恢复掩码为 incoming 准备 pair；未受影响的接触算法移交至新缓存。

正式发布只更新已准备的掩码、激活状态、数组和缓存，不分配内存。旧接触在候选析构时清理，必须在下一次步进前完成。来源变化使整个候选失效，不串联两个会互相破坏来源的独立发布。

`MeasureTargetResources` 在校验完整目标后量测各项发布后的 Saved 及共同顺序账本。它不包含原生刚体、形状和关节分配；调用方仍须计入这些资源，不能将本测试中的账本费用当作完整模型节点成本。

## 本机验证

完整配置构建通过。`native_edit_history_tests --physics-membership-only` 在独立隐藏桌面通过 4,255 项，证据为 `build/verification/abi-model-names-20261020/focused-02/physics-membership-only/result.log`。该入口同时执行原 Retained、同方向批次、资源账本、ImportScene 回归和新增混合交换。

新增用例使用真实 CPU 世界、旧新两组关节和未参与对象，覆盖双向交换、反序参数、incoming 历史中的 outgoing 锚点、取消、重复所有者、冲突顺序、来源变化、ABA、分配失败、接触算法、射线掩码及资源溢出。两种共同历史模式均验证预算拒绝、不分配的 commit、撤销与重做。

## 尚未接入的范围

`PrepareExchange` 仅交换已保留对象。随后新增的[首次替换候选](abi-physics-first-replacement-2026-10-21.md)支持私有 ImportScene 首次接入与旧资源停放，但模型物理表所有权、缩放派生状态和菜单 262 的私有编辑会话仍按[物理编辑迁移方案](abi-physics-editor-plan-2026-10-20.md)继续实施。不能将这两项世界成员能力标记为物理模型编辑完成。Win7 和 AMD 真机验收仍待对应设备。
