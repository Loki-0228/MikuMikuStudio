# 缓存目录绑定与快照租约读取

日期：2026-10-23。工作区为 `D:/MMD-Next/MMS-abi-dev`，分支为 `codex/plugin-abi`，起始提交为 `8bab6a2`。本批只完成内部 catalog 的固定目录绑定与带租约读取，不代表业务缓存 Store、管理窗口或实际缓存消费者已完成。main 已提交 HEAD 仍为 `fed9768`，本批没有读取或覆盖 main 的未提交改动。

## 背景

[业务缓存页面读取](abi-cache-business-reader-2026-10-23.md)在“生命周期与后续 Store 的边界”中列出：后续 Store 须补齐固定目录绑定及带 manifest 租约的 catalog 读取，在同一业务锁下检查 pending 操作、加载 manifest 和页面。本批实现该句的前半部分，并把 catalog 的写入来源判定从路径拼写提升到物理目录身份。

## 实现

### 文件层

[cache_files.hpp](../../include/mikumikustudio/cache_files.hpp) 新增 `DirectoryIdentity`（卷序列号与目录文件索引），以及两项目录原语：

- `Directory::Share(Directory&)`：复用当前对象已固定的目录状态与祖先守卫，不再解析 DOS 路径；自身共享有效，失败保留原输出。
- `Directory::GetIdentity(DirectoryIdentity&)`：读取受保护目录句柄的身份，不是 blob 的长度或身份；非目录、重解析点或无效句柄返回 `Invalid`，失败保留输出。

### catalog 层

[cache_catalog.hpp](../../include/mikumikustudio/cache_catalog.hpp)、[cache_catalog.cpp](../../src/io/cache_catalog.cpp) 增加：

- `Store(cache_files::Directory&&)` 固定目录绑定。拷贝共享同一绑定；移走后的 Store 与从未打开成功的绑定都返回 `Invalid`，不再退回路径解析。
- `ReadPinned(Snapshot&, cache_files::ReadLease&)`：在同一 catalog 锁内读取当前快照并交出已验证的文件租约。失败时快照与租约都保持不变；成功后租约替换旧值，并保证该快照在调用方持有期间不会被退休删除。
- `Snapshot` 私有字段保存根目录身份与来源类型。`Read`、`Checkpoint`、`Publish` 在 `ReadImpl`/`OpenRoot`/`AcceptOrigin` 中共用同一判定。

来源判定规则：

| 来源组合 | 判定 |
| --- | --- |
| 两个路径来源 | 与既有行为相同，先要求完全相同路径拼写，再核对物理根身份 |
| 任一侧为目录绑定 | 用物理根身份代替路径拼写核对，其余仍要求 owner、revision、head 一致 |
| 另一物理 catalog | `Conflict`，不读取目标目录中的同名快照 |
| 无效、移走或未打开的绑定 | 一律 `Invalid`，绝不回退到路径 |

失败分类同时收敛：打开元数据根失败不属于“存储记录损坏”，无效路径或绑定统一返回 `Invalid`；读取与初始化保留 `Missing`，带前驱的提交与检查点在根缺失时仍为 `RecoveryRequired`。

`ReadPinned` 不重复读取快照内容。`Load` 已对目录中每个快照执行信封与摘要校验并保留文件租约，读取结果只把当前快照对应的租约移出，因此新增路径除对象移动外不引入新的分配点；[业务缓存读取](abi-cache-business-reader-2026-10-23.md)记录的单点分配扫描覆盖的仍然只是它自己的读取路径。

## 验证

本机 Win10 x64；完整配置与 `mmd932` 原始兼容配置各自构建、运行。每个测试独占新建目录，只操作自身文件与句柄，不启动产品界面，不发送系统键鼠输入。输入冻结于 `build/verification/abi-cache-catalog-pinning-20261023/full/verification-summary.json` 与同目录 `original/verification-summary.json` 的 `sourceSha256`。

| 专项 | 完整配置 | 原始兼容配置 |
| --- | ---: | ---: |
| 目录身份、共享、绑定读取、跨拼写来源、租约保持（新） | 120 | 120 |
| catalog 主体回归 | 1,342 | 1,342 |
| catalog 恢复 | 691 | 691 |
| catalog 分配失败 | 35,580 | 35,923 |
| catalog 预算 | 7,671 | 7,671 |
| catalog 意图 | 931 | 931 |
| owned 恢复 | 2,136 | 2,136 |
| 检查点与退休回收 | 12,722 | 12,722 |
| 维护恢复 | 328 | 328 |
| 维护分配失败 | 54,537 | 55,111 |
| 文件原语 | 864 | 876 |
| 文件 I/O 故障 | 251 | 251 |
| 业务 codec | 3,445 | 3,445 |
| 业务页面读取 | 2,661 | 2,661 |
| 业务分配失败 | 561 | 561 |

计数是断言检查次数，不是独立用例数。分配失败专项的完整与原始配置计数不同，来自既有 `mmd932` 分支差异；两配置的同一专项均零失败。证据目录 `build/verification/abi-cache-catalog-pinning-20261023/` 下的 `full/` 与 `original/` 保存每项的 `report.json`（命令、退出码、耗时、二进制摘要）和 `output.log`/`error.log`。

新增专项覆盖以下行为，均为真实文件与真实句柄：

- 目录身份：重开、共享、自身共享、子目录、大小写不同的同一路径拼写都解析到同一物理目录；无效目录不改变身份输出。
- 绑定读取：初始化、读取、拷贝、移走后的有效与无效绑定；移走或从未打开的绑定在读取、初始化、提交时均返回 `Invalid`，且不创建目录。
- 来源判定：路径拼写不同则冲突；绑定来源允许同一物理根的不同拼写提交与检查点；另一物理 catalog 的合法快照被拒绝；无效快照被拒绝；所有失败保留可见快照。
- 租约：租约阻止删除被固定快照，检查点退休后回收报告为 `busy` 且其他退休元数据仍被删除；释放或替换租约后回收成功；租约活过 Store 与其他结果对象；同一快照可同时持有两个读取租约；读取、忙、缺失、空目录、快照损坏均保留原快照与旧租约。

两份产品构建通过，记录为 `build-product.json`：完整配置 EXE SHA-256 `3737c5b44497f23777142041e1e6a6a4780cb7d5947207392b451876c62b197d`，原始兼容配置 `f382da26862c2d2f6a00c66504225b85db1c654b11dbc31570f352af940596ce`。正式 `build/release/MikuMikuDance.exe` 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。本批没有修改公共插件 ABI、VMD、PMM、运行数据路径或中央设置格式。

未执行：断电或磁盘满、真实多进程新用例、产品 UI、GPU、真实 MME、Win7 真机、AMD 真机、性能基准或人工视觉验收。既有恢复专项只覆盖进程退出与 API 故障，不能当作掉电验收。

## 后续边界

本批之后，catalog 可以与固定目录和租约读取组合，但仍没有业务 Store：业务锁、pending 操作检查、manifest 与页面的同锁加载、页面操作记录、持久前驱与后继校验、catalog 恢复协调、仍可达页面集合、物理额度、工程关联、固定与业务清理、管理窗口及实际消费者都还缺失。效果编译缓存、纹理准备缓存和负数关键帧仍按 [main 行为同步计划](abi-main-sync-plan-2026-10-22.md)保留为待处理。

负载布局的分区限制、业务 Index 的 100,000 条目上限与文件层 65,536 枚举上限之间的差距保持不变，未在 catalog 层解决。
