# 缓存来源登记与现有预留修复

本批为视频来源及统一缓存预算补齐业务登记格式，并修复现有效果／纹理缓存入口的预算错误。**统一来源 Store 的生产写入、unknown 扫描、跨进程来源预留和视频菜单接线仍未完成。** main 已提交 HEAD 仍为 `6facf91`，目标范围未修改，阴影同步暂缓。

## 同一个 catalog 保存来源责任

新增内部 `cache_source_registry.hpp/.cpp`，沿用 `cache_business`、既有目录及 catalog 服务。`BusinessRoot` 在一个 catalog payload 内组合原 v1 `Manifest` 与 `SourceRegistry`，后续可通过一次 CAS 同时发布业务页引用和来源责任；没有另建一份视频预算或独立 catalog。

| 记录 | 内容与约束 |
| --- | --- |
| `DirectoryBinding` | 宿主定义的区域、逻辑 ID 和实际目录身份。当前只开放 Video 区域，不接受任意目标路径。使用文件前仍须与实际打开目录核对。 |
| `SourceRecord` | 来源 ID、创建操作 ID、目录引用、完整 `RawSourceInfo`、Published／Retired 状态、工程归属及最后使用时间。退休仍保留文件责任。 |
| `SourceOperation` | 操作 ID、来源 ID、目录引用、完整预留额，以及 Reserved／Prepared／Captured／AbortPrepared／AbortCaptured 阶段。未使用的计划／摘要字段必须全空。 |
| `BusinessRoot` | 旧 Manifest 和来源登记的 owner、maintenanceEpoch 必须一致；原始来源不能与旧缓存条目或元数据页承担同一文件身份。 |

Prepared 阶段保存创建时空文件身份及最大来源长度；Captured 阶段要求最终名称、身份、长度与计划完全一致。来源、操作、创建操作、文件名称和物理身份的重复均拒绝；工程归属必须在实际组装后的业务页中存在。已知来源及完整预留的累加检查溢出，但这只是未来总额度核算的输入，不包含 unknown 或元数据峰值。

编码采用有界小端格式和明确长度，拒绝未知版本／枚举、非零保留字段、截断、尾随字节及越界数量。登记表上限 8 MiB，组合 payload 不超过 catalog 的 16 MiB。超限先于集合分配判断；跨旧索引的文件身份验证通过集合查找，避免对大量条目做逐项嵌套扫描。所有失败保持输出。

旧 v1 Manifest 仍可读取，转换成 owner／epoch 匹配的空来源登记；新编码统一写组合根。旧页／Manifest 格式本身未改。`LoadPublishedIndex` 已接入组合根，并在真实页面读取／组装后验证来源的工程引用；`PublishedIndex::Sources()` 返回同一 CAS 快照中的元数据。它仍只持业务页租约，**不会自动取得 raw 文件租约或赋予删除权限**。

## 生产效果／纹理缓存预留修复

修改 `cache_manager`，现有消费者继续使用 `WriteReservation`：

- 原子覆盖时，旧目标与新临时文件同时计入峰值；不能先扣除仍存在的旧文件，也不能通过自动清理删除本次写入的旧目标来凑额度。
- 同线程嵌套写入纳入未完成预留，同名活动预留拒绝。移动转移同一责任，提交或取消精确释放；此令牌持有线程所属的互斥锁，使用和移动必须在同一线程。
- `Commit()` 现在返回登记是否持久成功。实际文件超过批准预留时拒绝，登记失败的已发布文件保留责任并阻止新写入；销毁不冒充退款，之后按真实文件重新登记成功才解除保守预留。原消费者忽略返回值仍可编译，不能因此把该文件描述成登记成功。
- 设置读取出现 Invalid／Busy／Io 时拒绝新增写入，`Current`／`Inspect` 明确报告不可用或禁用，不退回默认 4 GiB；设置缺失但由策略服务成功返回默认值的行为保持。
- named mutex 的所有权改为独立 RAII，获取锁后的构造异常也会释放。活跃 State 用共享引用保持，按安装根的弱映射复用，防止 A→B→A 重入切换丢失 A 的预留；仅未登记发布的责任需要额外自保活。
- 删除请求成功后，必须明确观察到文件命名空间不存在才扣账；存在、AccessDenied 等不确定状态继续计费。账目不等于卷物理空间已经回收。

独立审查补修了实际尺寸大于预留仍返回成功、设置错误时检查页显示旧启用值，以及删除结果不确定时提前扣账的路径。新增测试对这些情况使用真实稀疏文件和局部故障注入；不是只验证计算函数。

## 验证

证据位于 ABI 工作区 `build/verification`。源码冻结后构建，使用全新测试目录；二进制摘要保存在各 `summary.json`。

| 检查 | 完整配置 | 原始兼容配置 |
| --- | ---: | ---: |
| 来源登记与真实 catalog／reader | 4,244 通过 | 4,244 通过 |
| 原业务 codec | 3,445 通过 | 3,445 通过 |
| 原业务分配故障 | 561 通过 | 561 通过 |
| 原业务真实页面 reader | 3,236 通过 | 3,236 通过 |
| 新预留专项 | 136 通过 | 生产管理器禁用，不运行此专项 |
| 原管理器回归 | 144 通过，1 项跳过 | 同上 |

- 前 8 组：`cache-registration-nmn_d7ms/summary.json`。
- 管理器两组：`cache-registration-edf0gzir/summary.json`。跳过项为目录符号链接创建，当前权限不足，未标为通过。
- 来源专项包括独立完整 wire 对照、五个操作阶段、两种来源状态、逐字节截断、分配失败、真实页／raw／catalog CAS、删源后按读回元数据重新取得租约，以及从可信 Prepared／AbortPrepared 记录重新获取同句柄清理责任。旧页面租约和失败输出保持有回归。
- 预留专项包括真实 sparse 文件的新旧峰值、嵌套、移动、跨根保活、少报额度提交拒绝、写登记失败后恢复、策略读取失败、真实 share-delete 读者和删除不确定结果，以及获得 named mutex 后注入分配失败、由独立隐藏子进程确认锁已释放。
- 构建日志：`logs/cache-source-registry-{full,original}-build.log`、`logs/cache-reservation-{full,original}-build.log`。两产品构建成功；禁止的可选图形启动导入为空，差异检查通过。
- 完整产品 SHA-256：`12a85ecdd24e43a33a21f776417be4c2784c2a44f3561829b12c7e98302ee545`。
- 原始兼容产品 SHA-256：`4fbc884aa1cbb3b6b0b176c096eed6f1647dd940b8bdb2051d5105395300de44`。
- 重跑入口：`python tests/run_cache_registration_checks.py`；可用 `--business-only` 或 `--manager-only` 单独运行对应组。

没有改写 main、覆盖正式稳定版或新增 Win7／AMD／人工视觉／真实鼠标验收。

## 尚未完成的生产迁移

本记录完成时新组合根只有 codec、reader 和真实 catalog 组合验证；后续[来源持久事务](abi-cache-source-journal-2026-10-29.md)已实现阶段转换和明确恢复。效果／纹理生产管理器仍使用 `Management/index.bin`，尚未由统一业务 Store 取代，其磁盘总额度及 unknown 核算也未完成。

下一步在共用协调器下实现“批准完整预留 → 可信计划登记 → 捕获 → 最终发布 → 失败／退休清理”，并把现有生产管理器也迁入同一个持久账目。必须核算 unknown、元数据峰值及待删除文件，验证双进程和效果／纹理／视频共同超限，再接视频菜单、PMM 与共同历史。现有内存预留修复不替代这些要求。
