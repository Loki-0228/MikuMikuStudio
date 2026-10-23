# 缓存管理业务 Store

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `7831bbc`。本批实现 main `3782e0c` 缓存管理部分中的业务 Store；ABI 已有的策略、文件、索引、业务页面各层保持不变。

## 来源行为

main 的缓存管理模块提供：

- 安装级配额与自动清理设置（启用、自动清理、上限、最近工程数），设置只保存在本机。
- 统一缓存根下只管理注册过的 `.fxb` 与 `.mtc` 文件；管理索引记录文件、大小、最后使用、失效标记和工程归属。
- `Inspect` 汇总总量、效果/纹理分类、可回收量和逐工程的总量/共享/可回收字节与固定状态；`Clean` 支持可回收、指定工程、全部三种模式；`Pin` 固定工程；`Touch`/`Invalidate` 记录使用与失效；`WriteReservation` 在原子发布前检查配额并预留写入，`Commit` 后登记。
- 工程打开/关闭会建立或释放租约：当前工程与跨进程正在使用的缓存不会被普通清理删除；最近工程优先保留，固定工程不参与自动回收。
- 索引损坏时报告错误并停止自动/普通清理，显式「全部清理」可安全恢复；符号链接/重解析点路径拒绝管理；受限内存模式不创建磁盘缓存或索引。

## ABI 实现

- 原来的 ABI 策略模块拆为 [cache_policy.hpp](../../include/mikumikustudio/cache_policy.hpp) 与 [cache_policy.cpp](../../src/io/cache_policy.cpp)：保留 `Store/Snapshot/Configuration/Result` 的读写与比较交换、未知字段保留、非法值和并发拒绝，生产入口继续固定在程序目录 `data/cache` 与 `data/settings.json`，并忽略测试环境变量。
- 新增完整 [cache_manager.hpp](../../include/mikumikustudio/cache_manager.hpp) 与 [cache_manager.cpp](../../src/io/cache_manager.cpp)：采用 main 的 `Settings/Project/Snapshot/CleanupResult/Cleanup` 与 `Root/Managed/Current/Enabled/Configure/Inspect/Clean/Pin/BeginProject/ProjectOpened/EndProjectLoad/ResetScene/Shutdown/Touch/Invalidate/WriteReservation`。ABI 差异：
  - 设置存中央 `data/settings.json` 的 `cacheManager` 段（经策略模块比较交换），不是 main 的 `data/cache.ini`；未知字段继续保留。
  - 缓存根为程序目录 `data/cache`，索引在 `data/cache/Management/index.bin`。
  - 原始兼容配置完全禁用管理器（`Root` 为空、写预留与清理拒绝），与既有策略一致。
  - 索引新鲜度同时比较修改时间与文件长度，避免同一时钟刻度内的外部改写被误判为未变化。
  - 测试环境变量 `MMS_CACHE_MANAGER_DIR`、`MMS_CACHE_MANAGER_SETTINGS` 只影响管理器测试入口，不改变策略模块的生产路径。
- CMake：[plugin_abi.cmake](../../cmake/plugin_abi.cmake) 新增 `mms_cache_manager` 静态库（链接 `mms_cache_policy`），核心库改为链接它；新增完整配置下的 `cache_manager_tests` 目标。策略测试改用 `cache_policy` 命名空间。
- 测试：[cache_manager_tests.cpp](../../tests/cache_manager_tests.cpp) 采用 main 的管理器回归，并把每个用例的根目录同时指向隔离的中央设置文件；[cache_manager_policy_tests.cpp](../../tests/cache_manager_policy_tests.cpp) 保持原策略覆盖并改用新命名空间。

## 验证

本机 Win10 x64，隔离目录与自有子进程；证据目录 `build/verification/abi-cache-manager-20261024/`（每项保存命令、退出码、耗时与二进制摘要）。

| 检查 | 结果 |
| --- | --- |
| 策略 `--store` | 通过，5,701 项（默认值、边界、非法输入、快照比较交换、未知字段、失败保持、并发、分配失败） |
| 策略 `--installation` | 通过，145 项（生产入口固定目录、忽略环境与工作目录、只写安装内设置、不创建缓存根） |
| `cache_manager_tests` | 通过，145 项：设置持久化、内存模式、共享/独占记账、工程清理、当前租约保护、LRU 自动回收、固定工程、手动清理、外来文件与非缓存目录保护、重解析点拒绝、跨进程租约、并发发布配额、失败加载不发布工程关联、失效优先、损坏索引报告与全部清理恢复 |
| 完整产品 | 构建通过，EXE SHA-256 `d04c6a7578fc924779f5ddaecde63230481840112195d834280649eace546339` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `96ab80a99203ba21b9f72eddddf77cac6cc813acf027b7d5537b5c920182eeeb` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 管理窗口（应用内列表、清理/固定按钮、设置页）尚未实现，用户目前无法从界面操作管理器。
- 效果编译缓存与纹理准备缓存消费者尚未接入：没有真实 `.fxb`/`.mtc` 生产者使用 `WriteReservation/Touch/Invalidate`，因此没有真实缓存命中、容量占用或回收证据。
- 没有加载真实 MME 效果或大纹理工程，配额与 LRU 行为只在测试夹具上验证；跨机器/多用户、慢盘、只读目录与长时间运行未验收。
- 原始兼容配置按既有策略禁用缓存管理。

## 后续

下一批实现缓存管理窗口与效果/纹理缓存消费者，并用真实工程收集命中与回收证据。
