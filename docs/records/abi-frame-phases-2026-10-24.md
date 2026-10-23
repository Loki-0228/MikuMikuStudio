# 渲染帧阶段回调

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批为 `mms.render` 增加 1.2 的帧开始/结束阶段回调。

## 实现

- [render.h](../../sdk/mms/include/mms/render.h)：新增 `MMS_RENDER_PHASE_BEGIN`／`MMS_RENDER_PHASE_END`、`MmsRenderPhaseCallbackV1` 与 `MmsRenderApiV1_2`（在 1.1 之后追加 `setPhaseCallback`、`clearPhaseCallback`）；1.0/1.1 表保持不变。
- [render_service.hpp](../../src/plugins/render_service.hpp)、[render_service.cpp](../../src/plugins/render_service.cpp)：每客户端保存帧阶段回调；`FrameBegin`／`FrameEnd` 复制回调列表后以 BEGIN/END 阶段和当前帧信息调用，单回调异常被捕获；插件停止时清除。
- [manager.cpp](../../src/plugins/manager.cpp)：`mms.render` minor=2 返回 1.2 表。
- [integration.cpp](../../src/plugins/integration.cpp)、[plugin_storage.hpp](../../include/mikumikustudio/plugin_storage.hpp)、[frame_driver.cpp](../../src/app/frame_driver.cpp)：新增 `PluginFrameBegin`／`PluginFrameEnd` 宿主入口，`FrameDriver` 在帧工作前与 `PresentScene` 后调用。
- lifecycle 探针查询 1.2 并注册阶段回调，日志记录 `FRAME_PHASE_REGISTERED`、`FRAME_PHASE_BEGIN`、`FRAME_PHASE_END`；宿主探针调用两个入口并断言无 `FRAME_PHASE_FAIL`。

## 验证

证据目录 `build/verification/abi-frame-phases-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 宿主事件探针 | 通过 8 项；插件日志 `FRAME_PHASE_REGISTERED=1`、`FRAME_PHASE_BEGIN=1`、`FRAME_PHASE_END=1`、无失败 |
| 默认全量契约套件 | 通过，`PASS checks=44792` |
| 相关 CTest | 9 个模式全部通过，10.8 秒 |
| 完整产品 | 构建通过，SHA-256 `8a7d3937f0814f2a62a0f55b4da60b1600ae6801125bd2e50d499ab1b54135dc` |
| 原始兼容产品 | 构建通过，SHA-256 `afda7164be1ccab6807ee03cbdaeb5e5a6ad748d95542344eda5b968d355266c` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 探针直接调用宿主入口触发一次阶段回调；`FrameDriver` 的真实播放循环、提前返回路径、设备丢失期间呈现与多线程行为没有端到端验证。
- 阶段回调与 1.1 每帧回调一样要求轻量只读、不得调用宿主服务或阻塞；异常被吞掉。
- post 处理、租约分类与真实设备丢失仍未实现。
