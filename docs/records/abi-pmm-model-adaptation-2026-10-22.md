# PMM 模型适配与扩展数据保护

日期：2026-10-22。实现基线 `eeca7eb`，参考 main `3782e0c` 的模型适配名称修复，并补充 ABI 扩展数据的适配边界。本机验收通过，平台验收范围见末段。

## 适配行为

PMM 保存后，用户可能在外部修改源 PMX，再用旧 PMM 恢复动作。标准加载器按名称匹配新骨骼和表情；扩展字典若继续按旧索引覆盖名称，就会让新模型显示旧名称。本批从最终加载 workspace 取得实际适配槽，覆盖同数量换序、增删、改名和显示组变化。只复制槽号和骨骼映射数值，不保留 workspace 指针，不增加输入描述符。

| 扩展数据 | 本批处理 |
| --- | --- |
| 模型、骨骼、表情、显示组的 Unicode 名称及显示标志 | 已适配模型保留源模型内容。独立的记录计数不匹配也跳过整个名称字典；其他模型照常恢复。 |
| 双语备注 | 独立读取，缺少某语言时保留对应源备注。名称、备注和显示组仍在全部准备及校验完成后统一发布。 |
| 对象及元素身份 | 保留逻辑对象 ID，创建新运行代次；退役已适配模型的旧位置元素 ID。未适配模型及附件保留已有恢复行为。格式校验仍覆盖被退役元素。 |
| 相机、附件父骨曲线 | 使用与标准成功尾一致的骨骼名称映射。按原父模型轨与父骨轨的时间并集转换，必要时补 step 键；同时转换 native baseline，避免首次读取重复协调。其他通道与精确相机区间保留。 |
| 已知网格缓存映射 | 完整校验记录后，在打开 sidecar 前过滤适配模型；共享档案中的其他模型和附件继续绑定。旧绑定移除和新绑定安装使用同一候选交换。 |

骨骼映射缺失、旧索引越界或目标超出新模型范围时，沿用标准加载器的骨骼 `0` 回落。只改表情或显示组时不转换父骨。已有骨骼缩放、骨骼曲线和表情曲线的名称身份检查继续生效，不统一清空这些数据。

身份恢复若在适配期间遇到内存不足，交给现有 PMM 加载失败、关闭和重置路径。否则有效的旧身份记录可能被当作 opaque 数据保存，并在下一次结构已经匹配时重新套到新元素。普通无适配情况下的旧错误处理及未来版本保留规则维持。

## 保存与兼容范围

加载不改原 PMM 和持久 sidecar。适配后的缓存只保留实际有效绑定；下一次成功保存移除已失效的已知映射，避免重开后复活。共享档案可继续包含未引用的 mesh，加载器只按 PMM 中实际记录绑定，不删除持久文件。原文档在保存前仍保留，未知的无关记录继续透传。

本批不声称缓存加载失败、缺失档案或未知未来网格格式已获得完整的无损写回机制；现有 `CaptureSave` 仍基于实际 live 绑定生成已知版本。也不把“未触发 workspace 差异”视为同数量几何、材质或 Unicode 别名修改的内容兼容证明。skipped 模型的原生删除规则维持，扩展父通道的独立删除模式尚未加入。

原始兼容配置继续只读取标准体，不解析 MMS 扩展。新写入和本批正例使用 MMS 标记；旧 MDS trailer 只作为不应用扩展的反例。VMD、VPD 和标准 PMM 的字段编码未改；本批不扩展模型总数或改变标准文件指标。

## 验收

最终源码见 [candidate-04](../../build/verification/abi-model-adaptation-sync-20261022/candidate-04/manifest.json)，共 22 项构建输入。完整配置 [build-full-04](../../build/verification/abi-model-adaptation-sync-20261022/build-full-04.json) 及原始兼容配置 [build-original-01](../../build/verification/abi-model-adaptation-sync-20261022/build-original-01.json) 通过。原始配置构建后只调整了完整配置测试代码，生产输入未变。

| 验收 | 实际结果 |
| --- | --- |
| 名称适配 CPU 专项 | 32,910 项通过，包含两语言、换序、四类计数差异、备注、损坏载荷及分配失败。 |
| 身份适配 CPU 专项 | 9,082 项通过，逐点覆盖 84 次分配失败和最终成功，检查旧元素不复活及普通错误行为。 |
| 既有名称持久化 | 11,634 项通过。 |
| 父骨曲线及既有 host 回归 | 全部通过，覆盖扩展独有事件、父模型切换、原生不可解码、首次协调、重开、codec 和分配失败。 |
| 网格缓存 | 两套配置的真实 Ogawa 全集通过，覆盖共享 sidecar、全部过滤、失败原子、冻结快照及保存重开。 |
| 完整插件契约 | 完整配置 44,752 项，原始兼容配置 44,776 项通过。 |
| 相邻名称、来源路径、排序、历史通知及格式元数据 | 五组完整配置和一组原始配置通过；标准绑定名称及格式自动选择保持原行为。 |
| 真实 PMM 工作区 | 0002、0003 各带 MMS／仅标准体的 11 类适配，共 44 组保存重开通过；旧 MDS trailer 一组反例通过，共 11,792 项检查。 |

原始日志见 [完整专项](../../build/verification/abi-model-adaptation-sync-20261022/cpu-full-02/report.json)、[原始兼容专项](../../build/verification/abi-model-adaptation-sync-20261022/cpu-original-01/report.json)、[相邻完整回归](../../build/verification/abi-model-adaptation-sync-20261022/adjacent-full-01/report.json)、[相邻原始回归](../../build/verification/abi-model-adaptation-sync-20261022/adjacent-original-01/report.json) 及 [真实宿主](../../build/verification/abi-model-adaptation-sync-20261022/real-host-02/report.json)。真实宿主使用复制 EXE 的实际目录，所有运行文件位于其 `data`。运行继承 `Default` 桌面，只操作夹具自身的窗口和消息；兼容窗口由真实按钮 687 处理选择，不直接关闭窗口冒充适配。未执行已知有捕获问题的整套 `native_edit_history_tests` 默认入口。

首次构建缺少 `model.hpp` 显式包含，补齐后通过。首次身份对照在 `Find` 的边界外键分配处失败，未到旧 opaque 路径；测试已改为逐点区分边界前拒绝与边界内保留，生产普通行为未改。失败日志保留为 [build-full-01](../../build/verification/abi-model-adaptation-sync-20261022/build-full-01.log) 和 [cpu-full-01](../../build/verification/abi-model-adaptation-sync-20261022/cpu-full-01/report.json)。构建中已有中间目录及测试警告未作为本批已修复项。

[PE 核对](../../build/verification/abi-model-adaptation-sync-20261022/pe-products-final-01.json)显示两套产物均为 x64、子系统 6.1，与前批相比没有增删系统导入。正式 release 的 SHA256 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。

Win7 SP1 x64、CPU 基础路径及 AMD 要求保持。PE 导入及目标子系统检查属于静态验证，不能替代 Win7 或 AMD 真机验收。普通生产 `mms.edit` 继续关闭，main 和正式 release 不在本批写入范围。
