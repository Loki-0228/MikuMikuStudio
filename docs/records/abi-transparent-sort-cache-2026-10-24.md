# 透明排序缓存 main 同步

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批同步 main 的透明排序缓存验收夹具，并在有序透明通道加入计划、来源与 GPU 缓冲复用。

## 同步内容

- [tests/model_alpha_occlusion_tests.cpp](../../tests/model_alpha_occlusion_tests.cpp) 同步 main 版本：新增 `CheckTransparentCache`、`CheckObjectCache` 与 Maximum 不透明纹理检查，覆盖冷/热缓存像素等价、来源与计划失配、对象局部复用、释放钩子和 Maximum 不为不透明纹理分配排序缓冲。
- 新增 [include/mikumikustudio/transparent_sort_cache.hpp](../../include/mikumikustudio/transparent_sort_cache.hpp)：`Statistics`、`Release`、`SetEnabledForTests`。
- [model_renderers.cpp](../../src/render/model_renderers.cpp)：有序透明通道在计划命中时复用 GPU 默认池缓冲、跳过重排与上传；计划失配统计来源/计划失配并按模型数报告对象命中与失配；`SetEnabledForTests(false)` 强制逐帧分配上传；`Release` 释放缓冲并清零统计。`device_reset` 与 `ShutdownCleanup` 调用释放钩子。
- 同步 main 的完整缓存实现：`AlphaSource` 按对象保存投影后的三角形与 source key，`CollectAlphaGeometry` 在对象未变化时复用快照、只重投影变化对象；`AlphaPlan` 保存快照、排序与 GPU 缓冲，`SourceHits/SourceMisses/ObjectHits/ObjectMisses` 按真实复用计数。[transparent_geometry.hpp](../../include/mikumikustudio/transparent_geometry.hpp) 与 [transparent_geometry.cpp](../../src/render/transparent_geometry.cpp) 同步 main 的 `Order` 六参数接口、线性深度与相交拆分支持。

## 验证

证据目录 `build/verification/abi-transparent-cache-main-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `model_alpha_*` CTest | 15 个模式中 14 个通过：standard、maximum、low、high、intersections、low-alpha、peel-budget、maximum-limit、MSAA 组合与 sort-off 通过；`model_alpha_sort_off_mme_presence` 仍失败 |
| 默认全量契约套件 | 通过，`PASS checks=44804` |
| 缓存契约 | 冷/热像素一致、热帧 gpuHits/sourceHits 增加且 uploads/allocations 不变、变换/几何/材质/范围变化触发失配、对象缓存局部复用（一个对象移动时另一对象复用投影）、Release 后 sourceBytes/gpuBytes 归零、Maximum 不透明纹理不分配排序缓冲，均通过 |
| 完整产品 | 构建通过，SHA-256 `60269e59023e90fdd9a623872285d5012ede5cefbbadacac0e4dfb6ad89b06e9` |
| 原始兼容产品 | 构建通过，SHA-256 `d5fed0839b7853cba9a56235d6a9863fef81dd5541608155aed337c7bd45577d` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成

- `model_alpha_sort_off_mme_presence` 失败：加载 MMEffect 存在标记后，full Off 的固定管线结果没有保持「不随 authored 材质顺序改变的原生 pass 顺序」。main 的固定管线在同一条件下保留 authored walk，ABI 已与 main 对齐；该模式保持未通过，不把 14/15 当作透明排序全部通过。
- 来源字节与 GPU 字节预算、缓存逐出策略仍按 main 当前实现的范围，未接入真实工程压力与多帧预算验证。
- MME 真实插件、外部效果、Win7/AMD 真机与视觉验收仍待外部环境。
