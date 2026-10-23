# main 小项同步（alpha 分类、附件菜单、滚动条、栈缓冲）

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `cc22456`。本批同步 main `3782e0c` 中四个独立小项。

## 32 KiB 栈缓冲改为堆缓冲

main 把 `local_data_path.hpp` 的可执行文件路径缓冲与 `effect_paths.cpp` 的同名缓冲从 32 KiB 栈数组改为堆分配，避免在 MME/D3DX 深回调里耗尽保留栈。

ABI 采用：

- [local_data_path.hpp](../../include/mikumikustudio/local_data_path.hpp)：`ExecutableDirectory` 改用 `std::vector<wchar_t>(32768)`，失败仍返回空路径，调用方不退回其他安装的设置。
- [effect_paths.cpp](../../src/exports/effect_paths.cpp) 已与 main 对齐（保留 ABI 头文件要求的 `noexcept` 定义）。

验证：两个产品构建通过；未复现真实 MME 深回调栈耗尽场景，属于代码级同步。

## 不透明纹理 alpha 分类

main 更新 [model_alpha_pass.hpp](../../include/mikumikustudio/model_alpha_pass.hpp)：

- 未知元数据不默认透明，保持作者指定通道。
- `X8R8G8B8`、`X8B8G8R8`、`R8G8B8`、`R5G6B5`、`X1R5G5B5`、`L8`、`L16`、`R16F`、`R32F`、`G16R16`、`G16R16F`、`G32R32F` 直接标记已知不透明。
- `A8R8G8B8`、`A8B8G8R8` 与 DXT1／3／5 按实际像素 alpha 采样（DXT 每 4×4 块检查），A8B8G8R8 通道顺序正确。
- 分类只在加载后执行一次，写入 resourcePool tag 第 4 字节；排序仍由透明策略层决定。

验证：ABI 采用 main 的 [model_alpha_tests.cpp](../../tests/model_alpha_tests.cpp)，CPU 软件光栅回归通过（输出 `Transparent model pixel regressions passed`）。GPU 透明排序回归本轮未跑：main 更新后的 `model_alpha_occlusion_tests.cpp` 依赖尚未同步的 `transparent_sort_cache.hpp`，ABI 保留旧版该测试文件，避免引入未完成依赖；因此新分类在 GPU 排序路径上的行为仍需后续透明排序性能批次一并验证。

## 附件 VMD 菜单相机模式限制

main 仅在相机模式且选中附件时启用「读取当前附件动作（VMD）」，命令入口也拦截。

ABI 修改 [animation_curve_host_ui.cpp](../../src/window/animation_curve_host_ui.cpp)：

- 菜单本地化时按 `app->CameraMode()!=0 && app->AccessorySlot(app->SelectedAccessorySlot())` 设置 `MF_ENABLED/MF_GRAYED`，捕获默认标题保持一致。
- `WM_INITMENUPOPUP` 重新计算同一条件，菜单打开时仍灰显模型模式。
- `WM_COMMAND` 入口在模型模式直接返回，不打开文件选择或提示。

验证：[animation_curve_host_tests.cpp](../../tests/animation_curve_host_tests.cpp) 增加三语循环内相机模式 0/1/0/1 的菜单可用性断言，以及 `WM_INITMENUPOPUP` 后保持可用性的断言；完整曲线宿主回归通过（`all checks passed`）。

## 时间轴滚动条绝对位置

main 把 `SB_THUMBPOSITION` 与 `SB_THUMBTRACK` 统一为读取 `SIF_TRACKPOS` 的绝对位置，读不到时退回带符号的 `HIWORD`，并把下限从 0 改为 `frame_limits::minimum`（支持负帧）。

ABI 修改 [ui_hscroll.cpp](../../src/window/ui_hscroll.cpp)：

- `case 4`（`SB_THUMBPOSITION`）与 `case 5`（`SB_THUMBTRACK`）共用 `GetScrollInfo(ctrl,SB_CTL,SIF_TRACKPOS)`，命中时 `nextFrame=info.nTrackPos`，否则用 `std::int16_t(HIWORD(wParam))`。
- `timelineStartFrame` 夹取范围改为 `[frame_limits::minimum, frame_limits::maximum-page]`。

验证：两个产品构建通过；负帧下限依赖已同步的 `frame_limits`。本轮没有针对拖动消息的专项宿主测试，交互行为未做运行验证。

## 验证汇总

| 检查 | 结果 |
| --- | --- |
| `model_alpha_tests` | 通过（CPU 软件光栅，`Transparent model pixel regressions passed`） |
| `animation_curve_host_tests` | 通过（含新增相机模式菜单断言） |
| 完整产品 | 构建通过，EXE SHA-256 `eaaa20336038178bcb8e7b48613ea54f44f100f27f84542ed6149f4f049f09da` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `797c90f4b7c3930aad6a8866b0d271e6e3e0b22add6e78dfeca104f885753675` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

证据目录：`build/verification/abi-main-small-items-20261024/`。

## 未完成

- GPU 透明排序回归依赖未同步的 `transparent_sort_cache`（按对象/GPU 缓冲缓存），需在透明排序性能批次补齐后重跑。
- 未复现真实 MME 深回调栈耗尽与真实鼠标拖动时间轴；两者只有代码同步与构建证据。
- 原始兼容回退路径的 `SceneModified()`／`EM_SETMODIFY` 脏标记仍未同步。
- 阴影包围盒缓存、DDS 超大纹理与人工视觉、Win7/AMD 验收仍未做。
