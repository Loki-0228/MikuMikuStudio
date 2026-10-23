# 透明排序缓存 main 同步

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批同步 main 的透明排序缓存验收夹具，并在有序透明通道加入计划、来源与 GPU 缓冲复用。

## 同步内容

- [tests/model_alpha_occlusion_tests.cpp](../../tests/model_alpha_occlusion_tests.cpp) 同步 main 版本：新增 `CheckTransparentCache`、`CheckObjectCache` 与 Maximum 不透明纹理检查，覆盖冷/热缓存像素等价、来源与计划失配、对象局部复用、释放钩子和 Maximum 不为不透明纹理分配排序缓冲。
- 新增 [include/mikumikustudio/transparent_sort_cache.hpp](../../include/mikumikustudio/transparent_sort_cache.hpp)：`Statistics`、`Release`、`SetEnabledForTests`。
- [model_renderers.cpp](../../src/render/model_renderers.cpp)：有序透明通道在计划命中时复用 GPU 默认池缓冲、跳过重排与上传；计划失配统计来源/计划失配并按模型数报告对象命中与失配；`SetEnabledForTests(false)` 强制逐帧分配上传；`Release` 释放缓冲并清零统计。`device_reset` 与 `ShutdownCleanup` 调用释放钩子。
- 多模型失配的对象计数是**估算**：当前没有像 main 那样按对象保存投影快照，只在计划失配且存在多个模型时各计一次对象命中/失配。真正的按对象快照复用仍未移植。

## 验证

证据目录 `build/verification/abi-transparent-cache-main-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `model_alpha_*` CTest | 15 个模式中 14 个通过：standard、maximum、low、high、intersections、low-alpha、peel-budget、maximum-limit、MSAA 组合与 sort-off 通过；`model_alpha_sort_off_mme_presence` 仍失败 |
| 默认全量契约套件 | 通过，`PASS checks=44805` |
| 缓存契约 | 冷/热像素一致、热帧 gpuHits/sourceHits 增加且 uploads/allocations 不变、变换/几何/材质/范围变化触发失配、Release 后 sourceBytes/gpuBytes 归零、Maximum 不透明纹理不分配排序缓冲，均通过 |
| 完整产品 | 构建通过，SHA-256 `10c7d991036efb156d43751239f1538db35ddf86d96d196b461d158d365837c0` |
| 原始兼容产品 | 构建通过，SHA-256 `af2302c7d1528631ed3e508c41e16608cd2f396935e2ade33c20f2a617cc1272` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成

- `model_alpha_sort_off_mme_presence` 失败：加载 MMEffect 存在标记后，full Off 的固定管线结果没有保持「不随 authored 材质顺序改变的原生 pass 顺序」。该夹具在 ABI 分支首次同步，失败原因未定位；不把 14/15 当作透明排序全部通过。
- 按对象投影快照复用、来源字节统计与 main 的完整缓存预算尚未移植；对象命中/失配只是估算。
- MME 真实插件、外部效果、Win7/AMD 真机与视觉验收仍待外部环境。
