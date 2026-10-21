# 曲线原生插帧回调寿命（2026-10-21）

原生保形插帧现已私有准备通道集合，再通过唯一 `apply` 入口提交。细分提供者的闭包复制、执行与最终释放均在原会话保护内；采样和闭包析构关闭、替换或重建窗口后，旧调用停止，不能把结果写入新会话。

## 修正内容

EditorLease 同时保活原 Session 和来源状态集合，先释放状态，再释放 Session，最后才允许删除 Editor。复制 live Channel 时，即使 sampler 的复制构造重开同一 HWND，原 Channel 后续字段仍保持存活。预检读取的临时通道、准备失败的私有通道、本地提交参数和 canonical 替换的旧通道都在返回旧调用方前释放并重新验代。

插帧只复制一次通道集合，使用该私有 Channel 完成命中采样。`subdivide` 只能准备候选，不能代替正式提交。返回后检查通道 ID、关键帧索引及帧号。选择节点在提交前准备；canonical 读回后重新按 ID、帧定位，避免通道换序或插入前置关键帧后选错。

接受后的显示恢复失败不会重复 `apply`，拒绝或准备失败保留 redo。该批不改变 VMD 插值规则、文件标记或保存格式。普通绘制、`SampleTrack` 和 `KeyPoint` 的 sampler 调用仍未统一保护，不能据此认定所有采样入口完成。

## 运行证据

证据目录为 `build/verification/abi-curve-subdivision-20261021`。窗口测试串行继承 Default 桌面，只发送自有窗口消息，启动隐藏请求不保证后续夹具窗口不可见。

| 检查 | 结果 |
| --- | --- |
| 修复前复制反例 | 预设复制失败时原代码仍 `copies=0`、`bodies=1`、`live_bodies=1`，执行 apply 并增加历史；汇总断言退出 1。[原报告](../../build/verification/abi-curve-subdivision-20261021/subdivision-copy-red-full-01/report.json)。该阶段不关闭或替换窗口。 |
| 最终插帧寿命专项 | 两套配置各 1,156 项，退出 0。[完整配置](../../build/verification/abi-curve-subdivision-20261021/subdivision-green-full-02/report.json)、[原始兼容配置](../../build/verification/abi-curve-subdivision-20261021/subdivision-green-original-02/report.json)。 |
| 完整配置曲线全集 | 30,451 项，退出 0。[报告](../../build/verification/abi-curve-subdivision-20261021/curve-final-full-02/report.json)。 |
| 原始兼容配置曲线全集 | 再次出现 `0xC0000374`，末条为比较回调 `kind=1 mouse=0 phase=2 action=2 response=1`。未完成全集，不能标记通过。[失败报告](../../build/verification/abi-curve-subdivision-20261021/curve-final-original-02/report.json)。 |

专项覆盖复制、执行、最终释放阶段的关闭、同 HWND 替换及关闭后重建；还覆盖多通道部分复制失败、read 填入输出后抛错、apply 拒绝或抛错时参数尾析构、接受后 canonical 释放、畸形 ID／索引／帧、嵌套编辑拒绝、失败保 redo，以及通道和关键帧重排后的真实选中及下一次删除。

最终拥有者探针仅由实际私有 sampler 强持有，夹具用 weak_ptr 观察，避免局部对象析构提前触发。两套二进制、匹配 PDB 和来源已冻结于[清单](../../build/verification/abi-curve-subdivision-20261021/subdivision-candidate-02/manifest.json)。原始兼容配置的整体堆故障仍按[原生构造记录](abi-curve-native-construction-2026-10-21.md)继续调查，不归为本批已修复。

两套产品均构建成功，PE 为 x64、子系统 6.1，普通与延迟导入表和前批一致，正式 release 哈希不变，见[产品清单](../../build/verification/abi-ui-dependencies-20261021/pe-products-final-04.json)。

追加既有曲线回归时，完整配置七组直接 CTest 中五组通过；动作文件组有 12 项真实文件失败，表情组以 `0xC0000409` 退出，见 `math-full-03.log`。原始兼容三组中动作文件组同样失败，见 `math-original-04.log`。两次均沿用系统 TEMP/TMP。保持原二进制不变、单独改用工作区临时目录后，两套动作文件组及完整配置表情组通过，见[完整动作报告](../../build/verification/abi-curve-subdivision-20261021/math-workspace-full-03/report.json)、[原始兼容动作报告](../../build/verification/abi-curve-subdivision-20261021/math-workspace-original-03/report.json)、[表情报告](../../build/verification/abi-curve-subdivision-20261021/morph-workspace-full-03/report.json)。动作文件组为无窗口 CPU 检查，继承 CodexSandboxDesktop；表情组继承 Default。这证明工作区临时目录下通过，不足以判定原失败的具体系统错误。

原始兼容批量构建还误选了只在完整配置中有实现的 `bone_curve_tests`，出现 18 个未解析符号，日志 `abi-ui-dependencies-20261021/product-original-03.log` 保留。随后只选择该配置实际支持的产品和数学目标构建成功；没有把此链接失败改写为通过。后续已[修正六个测试的配置范围](abi-profile-curve-tests-2026-10-21.md)，保留实际支持的共同数学与编辑器入口。

Win7 SP1 x64、CPU 基础路径及 AMD 要求保持；本机合成窗口不代替真机、IME、多 DPI 或实际鼠标验收。main 和正式 release 未修改。

后续使用同一冻结候选捕获到[堆链表不一致现场](abi-curve-heap-observation-2026-10-21.md)。检测仍发生在下一用例的原生按钮创建阶段，损坏来源未确定，原始兼容配置全集继续保持未通过。
