# 效果编译缓存消费者

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `150514b`。本批接入 main `3782e0c` 的效果编译缓存消费者，并以 ABI 缓存业务 Store 作为容量与回收来源。

## 来源行为

main 的 `effect_compile_cache` 为 MME 外部效果提供编译字节码缓存：

- 键包含缓存格式版本、拥有原入口的 D3DX DLL 文件 SHA-256、源文件绝对路径/调用拼写/工作目录/原始摘要、D3DXSHADER flags、宏名与值、`D3DXPreprocessShaderFromFileW` 的完整预处理文本（因此任何 `#include` 内容变化都会改变键）以及解析出的依赖路径与原始摘要。
- 磁盘格式 `MMEC` v3：magic、版本、key/payload 长度与 SHA-256、存储的键与字节码；读取有界并校验两个摘要和存储键，损坏/截断/异版本按 miss 处理。
- 命中时仍用同一 D3DX DLL、调用方设备/池/flags 新建独立 `ID3DXEffect`；不共享 COM 对象、不跨线程编译。自定义 `ID3DXInclude`、未知 D3DX 版本、过大输入、预处理失败或诊断、编译/创建失败、缓存异常都回退原入口，且不改变 `GetLastError`。
- 默认目录 `<EXE>\Cache\MME\<效果文件名>\<key SHA256>.fxb`（Unicode 名称保留，非法/保留/过长目录名清理）；磁盘预算覆盖所有效果。`MMS_EFFECT_CACHE_DIR` 仅供聚焦测试覆盖，`MMS_EFFECT_CACHE_DISABLE=1` 可诊断关闭。不可写目录先退回内存缓存再退回原入口。
- 诊断：每次结果写一行 `MME_EFFECT_CACHE hit/miss/store/bypass/fallback` 与键摘要前 16 位；缓存问题不向 WINAPI 边界抛异常。

## ABI 实现

- 采用 [effect_compile_cache.hpp](../../src/exports/effect_compile_cache.hpp) 与 [effect_compile_cache.cpp](../../src/exports/effect_compile_cache.cpp)。ABI 适配：
  - 默认缓存目录改为 `cache_manager::Root()/MME`，即程序目录 `data/cache/MME`，与业务 Store 的 `Relative` 布局一致（`MME/<效果名>/<64 hex>.fxb`）。
  - 缓存开关读取 `cache_manager::Enabled()`；登记使用 `cache_manager::WriteReservation/Touch/Invalidate`，受统一配额、最近工程保留、固定与租约保护。管理器禁用时（原始兼容配置）缓存整体旁路。
  - 内存模式、不可写目录、自定义 include、损坏条目与异常路径与 main 相同。
- [effect_paths.cpp](../../src/exports/effect_paths.cpp) 采用 main 版本：在拦截的 `D3DXCreateEffectFromFileW` 槽位先尝试 `TryCreateEffect`，返回 false 时才调用原入口；同时保留 main 的调用计数与设备/池 dispatch 诊断。ABI 的 `effect_api.hpp` 声明带 `noexcept`，定义同步。
- CMake：`src/exports/effect_compile_cache.cpp` 加入核心库；完整配置新增 `effect_compile_cache_tests`（链接核心与 bcrypt）。
- 测试：[effect_compile_cache_tests.cpp](../../tests/effect_compile_cache_tests.cpp) 采用 main 全量套件，并做两处环境适配：
  - 默认缓存路径期望改为 ABI 的 `<EXE>/data/cache/MME`；
  - 系统临时目录与工作目录不同盘时，用例临时目录改到当前工作目录所在卷，保证非 ASCII 相对路径用例有真实相对路径。
- 真实 D3DX 集成模式（`--integration`）在本机可用，作为独立证据运行。

## 验证

本机 Win10 x64，证据目录 `build/verification/abi-effect-compile-cache-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 默认假 D3DX 套件 | 通过，165 项：键与失效、内存命中、磁盘命中、损坏条目回退、限制与不可写目录、磁盘预算、异常安全、内存模式、非 ASCII 路径与 Unicode 效果名 |
| `--small-stack` | 通过，9 项，128 KiB 预留栈 |
| `--integration`（真实 D3DX DLL 与 D3D9 设备） | 通过：缓存处理路径与原入口产生相同 `Tint` 参数，include-resolution 子进程比较通过 |
| 完整产品 | 构建通过，EXE SHA-256 `1c81163233086c26748b42130c6526232cca86c2497534aeab83feaa60015bfe` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `4aebd13298aab7300dc0595038d9b0ead89dd351d1dea43abf6bb3a4c3a81b84` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 没有加载真实 MME 工程或 main 记录中的 45 个效果、约 5.5 秒冷启动场景；因此没有真实启动耗时对比、命中率或缓存目录增长数据。
- 集成模式使用聚焦夹具，不是真实 MME 插件调用；真实 MME 的 include 处理与工厂路径未验收。
- 默认缓存目录位于程序目录 `data/cache`，测试主要使用 `MMS_EFFECT_CACHE_DIR` 覆盖；业务 Store 的容量/回收与该消费者的联动只有代码路径与单元检查，没有真实运行中的配额触发证据。
- 未做人工视觉验收、Win7/AMD 真机、多用户/慢盘/只读目录长时间运行。

## 后续

下一批接入纹理准备缓存消费者（CPU 解码/缩放、`.mtc`、导入路径与命中回收证据），并补一次真实 MME 效果工程的命中与容量观测。
