# 阴影包围盒复用

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `5bd9265`。本批同步 main `3782e0c` 的阴影包围盒帧内复用（性能项，不改变阴影结果）。

## 来源行为

main 的做法：

- 模型顶点缓存新增帧内接口：`BeginFrame` 清除上一帧的「本帧已验证」标记；`ShadowBounds`/\`RememberShadowBounds` 记录/读取某模型在当前帧已验证的顶点缓冲阴影包围盒；只有 `Reuse`/\`Remember` 在本帧确认过同一位姿的加载器自有几何才可命中，外部模型与原生网格代理不缓存。
- 自适应阴影求场景包围盒时先查缓存；未命中才 `Lock` 完整顶点缓冲求精确包围盒（覆盖全部顶点，不是稀疏采样），并在 `Unlock` 成功后登记。
- 位姿改变、缓存失效、`Invalidate`/设备重置或显式关闭时清除包围盒记录；`MMS_SHADOW_BOUNDS_CACHE_DISABLE=1` 可诊断关闭。
- 统计新增 `shadowBoundsHits/Misses/Stores`。

## ABI 实现

- [model_vertex_cache.hpp](../../include/mikumikustudio/model_vertex_cache.hpp) 采用 main 的新接口与统计字段；[model_vertex_cache.cpp](../../src/model/model_vertex_cache.cpp) 补齐缓存条目字段、`BeginFrame`、命中/登记逻辑与 `Reuse`/`Remember` 的帧内验证标记。ABI 独有的插件资源计量 `MeasureResources` 保留（main 已移除该子系统）。
- [adaptive_shadow.cpp](../../src/render/adaptive_shadow.cpp) 的 `IncludeMeshBounds` 先查缓存，调用方传入模型指针；未命中路径保持原有限制：`GetDesc` 截断到缓冲大小、整段锁定求精确包围盒、`Unlock` 失败不登记。ABI 的中央设置读写（`settings.json` 的 shadow 段）不变。
- [frame_scene.cpp](../../src/render/frame_scene.cpp) 在每帧场景渲染前调用 `model_vertex_cache::BeginFrame()`。
- 保留 main 的测试开关 `SetShadowBoundsEnabledForTests`。

## 验证

证据目录 `build/verification/abi-shadow-bounds-cache-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `model_vertex_cache_tests`（main 扩展版） | 通过，`PASS vertex cache: 611 byte-exact reference cases, PMD and PMX UV0-4`（含新增包围盒复用断言） |
| 生产 SM3 阴影全集（`native_shadow_tests skin_effect_sm3.fx`） | 通过，`native_shadow failures=0`，98 项 native parity 比较、20 个 partial-depth 用例 |
| SM2 覆盖路径（`--coverage-only`） | 通过 |
| 完整产品 | 构建通过，EXE SHA-256 `b8ac2ce0149c950f952d65c0ef72ffc5a542df8b8822ce19aca101fac9ee47f8` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `b72ac636abe63b632cf31a7d97b44d0bfac977d3a7ee4f2868ca9c26069a9127` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 这是纯性能同步：没有帧耗时、锁定次数或缓存命中率的实测对比；只有功能结果与主统计字段。
- 未在真实 MME 场景、超大模型、多模型同屏与设备丢失/重置循环中验证缓存失效路径。
- 人工视觉、Win7/AMD 真机、长时间运行仍未验收。

## 后续

下一批处理原始配置回退路径的脏标记，并重测透明排序 GPU 路径；之后回到 P1–P6 接口。
