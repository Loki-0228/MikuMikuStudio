# 私有物理资源的首次替换

`history_physics::ImportScene::PrepareReplacing` 已实现私有物理世界首次接入与旧 Retained 停放的同一次发布。此接口是本体内部 C++ 实现，不新增公共插件 ABI。它完成[物理编辑迁移方案](abi-physics-editor-plan-2026-10-20.md)的第二步，菜单 262 尚未接入共同历史。

## 候选与所有权

参数中的旧 Retained 必须唯一、非 pending、当前存在且属于目标世界。资源重叠、外部关节、无效成员及关节编号会拒绝整个候选。旧 `Prepare()` 使用空列表调用同一实现。

准备在私有数组中移除旧成员，再按私有世界顺序追加新成员。未参与的实时成员保持原顺序。旧资源的 Saved 共用准备前的顺序快照；新 Retained 在成功发布前保持 pending。两侧世界步进、成员及移动成员数组、关节顺序和编号、代理、掩码、接触缓存及所有者代次均参与来源校验。准备失败或取消不会改变实时及私有世界的数组存储和容量。

发布交换已准备的世界数组和碰撞缓存，停放旧资源并接入新资源，不分配内存、不析构资源、不调用插件。旧代理保留，停放掩码为零且停止模拟。未受影响的接触算法转移至新缓存；旧接触在已发布候选析构时清理。调用方必须在下一次世界步进或销毁前释放该候选。

新旧原生刚体、形状、motion state 和关节仍归调用方所有，世界及资源必须比候选和 Retained 存活更久。后续撤销、重做通过 `Retained::PrepareExchange` 交换保留资源，不再导入文件。模型的表、名称、缩放派生状态和 scenePtr 切换仍须由外层资源版本统一管理。

`MeasureTargetResources` 使用发布后同一批所有者的计费身份，量测新所有者及旧 Saved 和共享顺序账本。它排除临时候选、世界数组及原生 Bullet 资源；外层事务仍须保留并量测完整新旧资源。该数字不能代表完整模型节点成本。

## 本机验证

证据目录为 `build/verification/abi-input-state-20261021/`。完整配置及原始兼容配置的正式程序构建通过；物理专项在完整配置、真实 CPU 世界和独立隐藏桌面执行。

| 入口 | 最终结果 | 证据 |
| --- | --- | --- |
| `native_edit_history_tests --physics-import-replace-only` | 1,850 项通过 | `physics-replace-02/physics-import-replace-only/result.log` |
| `native_edit_history_tests --physics-membership-only` | 6,118 项通过，包含旧成员、导入及首次替换 | `physics-replace-02/physics-membership-only/result.log` |
| 完整配置构建 | 通过 | `build-reviewed-full-04.log` |
| 原始兼容配置构建 | 通过 | `build-reviewed-original-04.log` |

新增用例覆盖多旧资源、反序参数、空新世界、非末项空闲 handle、数组容量、代理及掩码、接触算法、未参与对象状态、来源变化、ABA、外部关节、取消、C++ 分配失败及量测溢出。两种共同历史模式均验证完整资源预算拒绝、一次显式提交、撤销和重做。发布阶段检查 C++ 及 Bullet 分配均为零，顺序检查另执行 12 轮撤销和重做。

首次构建发现测试假设 Bullet 一定提供 `setUid`；夹具改为兼容现有 Bullet 的 `setUserConstraintId`。首轮运行还发现夹具错误地按索引排序还原顺序，未遵守 Bullet 非末项删除时的末项交换；最终夹具固定实际源顺序。两项均保留原校验，未据此放宽生产来源校验。最终报告为 `physics-replace-02/report.json`。

## 未完成范围

本批未执行整个原生历史套件，也未完成真实菜单 262、模型资源版本所有权或私有编辑窗口。深层形状、质量、摩擦和求解器设置的修改仍需外层编辑隔离，不能把成员校验当作任意物理写入的保护。Bullet 原生分配返回空指针的故障扫描、初始 sleeping、static、kinematic 的专门替换组合尚未补齐。

Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求保持；本机结果不能代替 Win7 和 AMD 真机验收。
