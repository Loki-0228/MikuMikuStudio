# 缓存目录的指定身份初始化

本批新增内部 `cache_catalog::Store::InitializeOwned(owner, bytes, output)`，允许调用方在首次发布前确定存储身份。这样，空业务 manifest 的 `storeOwner` 与首次目录快照的 `StoreId()` 可以从第一版起保持一致，不必先发布占位内容再修改身份。

原 `Initialize(bytes, output)` 继续生成随机非零身份，行为不变。新增入口复用原发布流程，没有修改快照、提交记录或业务 manifest 的字节格式，也没有新增公开插件 ABI。业务 Store 和缓存消费者尚未接入，不能将这一入口计为完整缓存管理功能。

接口与实现分别位于 [cache_catalog.hpp](../../include/mikumikustudio/cache_catalog.hpp) 和 [cache_catalog.cpp](../../src/io/cache_catalog.cpp)。业务 manifest 的格式见 [缓存业务索引编码与分页](abi-cache-business-codec-2026-10-23.md)。

## 初始化合同

`owner` 是调用方准备的 16 字节非零存储身份，不是工程 ID，也不是文件系统访问凭据。发布仍须经过原目录校验、文件锁、内容校验与容量限制。只有受锁保护的读取确认目录尚无有效库和未知残留时，才能首次初始化。

| 情况 | 返回及效果 |
| --- | --- |
| owner 全零 | `Invalid`，不创建目录，保留 output。 |
| bytes 超过 `MaximumSnapshotBytes` | `Limit`，不创建目录，保留 output。 |
| 已有有效目录 | `Conflict`，即使传入同一 owner 也不覆盖。 |
| 存在未知文件或中断初始化留下的孤立快照 | `RecoveryRequired`，不采用、不清理、不重新初始化残留。 |
| 空目录首次成功发布 | revision 为 1，`StoreId()` 严格等于 owner，负载等于 bytes。 |
| 提交写入已尝试后发生失败 | 沿用 `Indeterminate` 语义。调用方必须重新读取，不能按错误结果盲目重试。 |

所有失败均保留 output，包括其所属目录和 CAS 所需的私有提交身份。输入 owner、bytes 可以引用 output 的成员：实现先复制到局部候选快照，成功后才交换输出。已有 `Commit` 的文件锁和 CAS 规则不变。

`InitializeOwned` 不授予恢复权限。残留文件中的 owner 与请求相同，也不足以证明上一次操作已经完成或可以安全重做。真实业务 Store 仍需定义有来源的操作记录、恢复过程及容量协调。

## 实际验证

本批证据目录为 `build/verification/abi-recording-editor-view-20261023`。以下是实际输出的断言检查计数，不是固定测试用例数。

| 专项 | 增强配置 | 原始配置 | 日志目录 |
| --- | ---: | ---: | --- |
| 目录基础专项 | 1343 | 1342 | `owned-full-final`、`owned-original` |
| 普通 new 分配故障专项 | 22493 | 22503 | `owned-allocation-full`、`owned-allocation-original` |
| 原有进程中断与文件 API 故障专项 | 271 | 271 | `recovery-full`、`recovery-original` |

各目录的 `report.json` 记录退出码，`output.log` 保存检查结果。基础专项包含有界等待，每轮等待也会执行断言，因此实际调度会影响计数。原始配置运行通过只证明这一内部模块及测试可用，不表示原始产品已启用业务缓存功能。

[cache_catalog_tests.cpp](../../tests/cache_catalog_tests.cpp) 先通过真实业务编码器生成空 manifest，再调用 `InitializeOwned` 写入磁盘。重开后，测试验证 manifest owner 与目录身份一致，并实际解码和组装空业务索引。它还检查零身份、超限输入、同身份与不同身份重复初始化、输入输出别名、普通后续 CAS、未知文件及孤立快照拒绝。失败前后的输出和磁盘文件内容保持检查仍然有效。

[cache_catalog_allocation_tests.cpp](../../tests/cache_catalog_allocation_tests.cpp) 保留原 `Read`、`Initialize`、`Commit` 和别名扫描，新增独立 `initialize-owned` 扫描。增强配置实际注入 61 个普通 new 单点，原始配置为 62 个，日志分别记录 `ownedOrdinals=61` 和 `ownedOrdinals=62`。扫描先测当前成功路径的分配次数，再逐点注入；该次数受实际路径字符串、构建和运行环境影响，不是接口保证。本轮未进一步定位两次运行相差一个分配点的具体来源。

Owned 扫描的成功结果必须使用请求的 owner，且真实重读得到相同身份。每次失败都检查输出保留，并在输出原属的健康目录中执行真实 `Commit`，证明私有来源与 CAS 身份仍可使用。磁盘检查区分未发布、需要恢复和提交结果不确定的状态；残留不能因再次调用 `InitializeOwned` 而被采用。分配扫描只覆盖所测成功路径上的普通 new 单点故障，不代表所有内存分配方式或失败分支中的新增分配均已覆盖。

原恢复专项使用受控子进程硬退出和文件 API 故障，重新验证共享发布流程。日志分别包含快照头写入后、快照 flush 后、提交头写入后、提交 flush 后退出，以及提交 flush 报错后的清理结果。这些是进程与 API 故障证据，不是断电证据，也不是 Owned 专属初始化硬退出测试。

## 错误实现反例

`owned-negative-results.json` 记录了将 `InitializeOwned` 临时改为忽略传入 owner、直接调用默认 `Publish` 的反例。实际运行退出码为 1，在第 312 次检查报告：

```text
first durable publication has caller prepared identity
```

这证明新增测试能识别「接口接受 owner，但实际仍生成其他身份」的错误。`negative_owned.py` 在 finally 中恢复保存的源文件，并逐字节确认恢复；`owned-negative-restored.json` 记录恢复后的 `cache_catalog.cpp` SHA256 为 `8529845b425a82a2a0dfbcc43dc68220d923aa3d6ebcb7e079a4af70840f548b`。恢复后重新构建，随后 `owned-full-final` 基础专项通过。

## 产品核对与验收边界

`build-full-identity-fix.json` 和 `build-original-identity-fix.json` 记录本批整合后的两套产品构建成功。`product-pe-final.json` 确认两套产品均为 x64、子系统版本 `6.1`，效果资源与各自基线一致，稳定版程序哈希未变，main 仍为 `fed9768`。

增强配置的系统导入无增减。原始配置因同批录制父对象身份保护新增 `bcrypt.dll!BCryptGenRandom`，没有移除导入。首次导入严格不变检查失败后，核对脚本只允许这一已审查的新增项，最终通过。该变化及 Win7 API 要求见[录制视角批记录](abi-recording-editor-view-2026-10-23.md#产品与平台核对)，不是本缓存接口增加的系统依赖。

本批没有接入真实业务 Store、容量预留、工程关联、访问时间更新、固定和清理策略，或效果及纹理消费者。空业务 manifest 的真实磁盘往返只验证首次身份一致性。

Owned 专属首次初始化硬退出、两个进程同时首次初始化的竞争尚未单独覆盖。原目录专项中的既有竞争与恢复测试不能替代这两项。Win7 真机、AMD 真机和断电恢复也不在本批验证范围内。
