# 业务缓存页面的真实读取与租约

本批新增内部 [cache_business_reader.hpp](../../src/io/cache_business_reader.hpp) 和 [cache_business_reader.cpp](../../src/io/cache_business_reader.cpp)，将已验证的 catalog 快照加载为只读业务索引。读取会打开 manifest 引用的实际页面、校验文件身份与内容，再组装工程、条目和当前映射。成功结果持有全部页面租约，直到结果销毁或被替换。

这是业务 Store 的读取组件，尚未接入产品缓存命中、管理窗口或页面发布。main 已提交 HEAD 仍为 `fed9768`；`3782e0c` 的完整缓存管理及效果、纹理缓存行为仍未同步完成。

## 读取合同

`LoadPublishedIndex(source, pages, output)` 接收有效的 `cache_catalog::Snapshot` 和已打开的页面 `Directory`。空 manifest 也要求目录有效。函数按以下顺序准备候选结果：

1. 解码 source 中的 manifest，并核对 `storeOwner` 与 catalog 的存储身份。
2. 保存完整 source 副本，包括原 catalog 的私有 CAS 身份。
3. 按 PageRef 顺序取得实际文件租约。`Acquire` 校验名字、卷及文件身份、种类、owner、key、长度和摘要；`ReadAll` 读取实际负载。
4. 解码所有页面，再通过 `Assemble` 校验页面种类、序号、数量、长度及跨页关系。
5. 全部成功后交换 output。失败释放候选租约，保留原 output 和它持有的页面租约。

`PublishedIndex` 只能移动，不能复制。`Source()`、`Value()`、`Description()` 返回只读引用，分别暴露原 catalog 快照、已组装 Index 和 Manifest。无效或移走后的对象返回静态空值。有效对象的引用只在该对象尚未销毁或替换时有效。source 可以引用 `output.Source()`；成功和分配失败的别名调用均有实际测试。

| 结果 | 含义 |
| --- | --- |
| `Invalid` | source 或页面目录无效。 |
| `RecoveryRequired` | 已发布数据缺失、损坏、身份不符或关系非法。不退回旧 manifest，不采用同名替代文件。 |
| `Busy` | 实际页面存在共享访问冲突。 |
| `Unsupported` | 页面或 manifest 使用不支持的版本。 |
| `Limit` | 编码数据超过现有 codec 或文件层的限制。 |
| `AllocationFailure` | reader 自身分配失败，或 codec 明确报告分配失败。 |
| `Io` | 文件层或其他读取失败。文件层内部已经归类为 Io 的分配失败不再猜测分类。 |

本组件不检查 Entry 引用的效果或纹理负载。Index 中的 `Valid` 是索引字段，不代表本次读取已经打开该负载。消费者仍需取得对应负载租约并校验身份与消费者格式，才能将其视为缓存命中。

## 生命周期与后续 Store 的边界

返回结果是传入版本的读取结果，不保证它是最新 head。测试在保留旧结果时实际提交新 catalog 版本，旧结果保持不变；显式读取新版本才替换结果并释放旧页面租约。

`Source()` 保留内存中的 manifest 字节和完整 CAS token，但不持有 manifest 文件租约。本组件也不绑定业务根、不取得业务根锁，不保证跨目录原子读取，不授予写入额度或删除权限。因此不能将它直接包装成完整业务 Store，也不能以一个逻辑 owner 代替物理根身份。

后续 Store 仍须补齐固定目录绑定及带 manifest 租约的 catalog 读取，在同一业务锁下检查 pending 操作、加载 manifest 和页面。非空发布需要页面操作记录、持久前驱与后继校验、catalog 恢复协调，以及全部仍可达页面的集合。物理额度必须包含旧页、未发布文件、catalog 元数据和预留量，不能只按 `liveEncodedBytes` 计算。

负载布局也尚未冻结。业务 Index 允许 100,000 个条目，而当前文件层一次目录枚举最多 65,536 项；后续设计须采用有界分区或明确的容量限制，不能把全部合法条目无条件平铺到一个目录。本批没有降低原业务 schema 的数量上限。

## 实际验证

证据目录为 `build/verification/abi-cache-business-store-20261023`。`verification-summary.json` 汇总下列运行记录及源码摘要。每个目录的 `report.json` 保存实际命令、二进制摘要和退出码，`output.log` 保存检查结果。计数是断言检查次数，不是独立用例数。

| 专项 | 增强配置 | 原始配置 | 记录目录 |
| --- | ---: | ---: | --- |
| 真实页面读取、租约、拒绝及普通分配失败 | 2661 | 2661 | `reader-02`、`reader-original` |
| 既有业务 codec 回归 | 3445 | 3445 | `codec-full-final`、`codec-original` |
| 既有业务 codec 分配失败回归 | 561 | 561 | `allocation-full-final`、`allocation-original` |

[cache_business_reader_tests.cpp](../../tests/cache_business_reader_tests.cpp) 先创建真实负载与三个种类的页面，再通过真实 `InitializeOwned` 发布 manifest 并重新读取。成功场景检查具体字段、合法同类跨页、空 manifest、移动、保留旧版本、通过 `Source()` 进行真实后续 CAS，以及结果替换后旧页面可以删除。读取失败场景包含错误 owner、错误标记、未来版本、数量超限、页面序号、尾数据、跨页重复路径、工程外键、当前 generation、缺失页面、实际摘要损坏、共享冲突及同名字节的身份替换。

普通读取和 source/output 别名路径分别测量成功路径的分配次数，再逐点注入。两配置本次分别测得各 80 个普通 new 单点。每次失败检查完整输出与旧页面租约保留，普通路径还检查候选页面无句柄泄漏。别名扫描完成后释放输出，确认所有页面均可删除。该扫描不覆盖 aligned、CRT、Win32 或 CNG 内部分配，也不是任意故障组合测试。

首次 `reader-01` 在身份替换夹具的重命名准备阶段失败，因为夹具的 payload 目录仍持有共同父目录保护。修正后在换名之前释放夹具自己的两组目录 guards，并加入两个同时存在文件的 fileIndex 独立校准和换名后的身份核对。没有放宽生产目录保护或身份拒绝规则。最终 `reader-02` 和原始配置通过，审查记录为 `reader-review.md`。

两产品构建通过，记录为 `build-product-full-01.json` 和 `build-original-reader-01.json`。`product-check.json` 确认 x64、子系统 6.1、与前批最终产品一致的导入和效果资源，稳定版 EXE 哈希未变。本批没有修改公共插件 ABI、VMD、PMM、运行数据路径或中央设置格式。

本批没有执行产品 UI、GPU、第三方 MME、性能基准、Win7 真机、AMD 真机或断电验收。原始配置的内部模块专项通过不表示该产品配置已启用业务缓存。业务初始化、页面发布与恢复、物理额度、工程事件、固定及清理、消费者和管理窗口仍列为未完成。
