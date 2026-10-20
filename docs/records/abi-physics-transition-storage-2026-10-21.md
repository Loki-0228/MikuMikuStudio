# 物理回放准备不再修改当前数组

`Retained::Transition` 原先会在准备恢复成员时扩容实时世界的三个 Bullet 数组。首次私有导入若把世界替换为空存储，随后准备撤销便会改变实时数组地址及容量；取消或历史预算拒绝也无法恢复它们。

现在准备完整的私有 collision object、moving body 和 constraint 数组。发布交换数组的数据、数量、容量及所有权，已消费候选负责释放旧存储。取消和准备失败只释放私有存储。来源仍校验世界步进、成员及顺序、代理、刚体、碰撞缓存、算法和资源所有者代次；接触算法转交规则保持。发布不分配 C++ 或 Bullet 内存。

新增回归接入原 `PhysicsExchangeChecks`，覆盖空世界恢复、保留其他模型的世界、逐项分配失败、取消、两种共同历史模式的预算拒绝，以及 12 轮双向交换。它核对实时数组地址、容量、顺序、接触算法身份及保留账本的计费身份，未以最终成员数量代替准备阶段不变性检查。

完整配置构建通过；最终结果为 `native-hidden-05/report.json`：`--physics-exchange-only` 5,287 项，包含原成员及导入的 `--physics-membership-only` 8,106 项通过。证据根目录为 `build/verification/abi-physics-property-20261021/`，构建日志为 `build-full-core-04.log`。这是同桌面的隐藏窗口及真实 CPU 世界回归，桌面环境见[更正记录](abi-verification-desktops-2026-10-21.md)。

该修复只处理世界成员存储的准备与发布，不单独代表模型物理编辑窗口、完整公共物理编辑 ABI、Bullet 原生分配返回空指针或 Win7／AMD 真机验收完成。
