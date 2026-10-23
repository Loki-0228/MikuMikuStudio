# 渲染服务帧信息

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批补齐 P5 的帧信息：成功呈现的帧序号、后缓冲尺寸与格式进入 `mms.render` 查询。

## 实现

- [render_service.hpp](../../src/plugins/render_service.hpp)、[render_service.cpp](../../src/plugins/render_service.cpp)：新增 `FramePresented`，成功呈现时递增帧序号并记录后缓冲宽高与 D3DFORMAT；`query` 现在填充 `frameIndex`、`width`、`height`、`backBufferFormat`，不再固定为 0。
- [plugin_storage.hpp](../../include/mikumikustudio/plugin_storage.hpp)、[integration.cpp](../../src/plugins/integration.cpp)：新增宿主入口 `PluginFramePresented`。它读取真实设备的 0 号后缓冲描述后写入渲染服务；原始兼容配置或未初始化插件运行时时为空操作。
- [frame_driver.cpp](../../src/app/frame_driver.cpp)：`Present` 成功后调用该入口，使帧序号跟随真实呈现而不是 UI 刷新。
- lifecycle 探针的渲染查询日志带 `frame`、`size` 与 `fmt`，并在有帧时记录 `RENDER_FRAME_SEEN`；宿主探针接收 `RENDER_FRAME_SEEN` 并额外断言日志含 `frame=1`。

## 验证

证据目录 `build/verification/abi-render-frame-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 宿主事件探针 | 通过 8 项；插件日志 `RENDER_LOST_QUERY gen=1 lost=1 frame=1 size=1463x914 fmt=21`、`RENDER_RESTORED_QUERY gen=2 lost=0 frame=1 …`、`RENDER_FRAME_SEEN` |
| 默认全量契约套件 | 通过，`PASS checks=44772` |
| 相关 CTest | 10 个模式全部通过，9.4 秒 |
| 完整产品 | 构建通过，SHA-256 `b6edb0562bc3b313ef0f0aa47acf52ea8ab744e60d081ae052953d3e25d00437` |
| 原始兼容产品 | 构建通过，SHA-256 `122bdd8f63a0655ddea8c2bba804428e4a7f9e9f4850a67f8c81232757afbfb3` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 探针通过宿主入口触发一次帧信息更新；`frame_driver` 的成功 Present 调用点没有在真实播放循环里端到端验证。
- 帧序号只在进程内计数，不持久化；设备丢失期间的呈现不计入。后缓冲格式是原始 `D3DFORMAT`，不做语义映射。
- post 处理、DX9 帧生命周期钩子、租约分类与真实设备丢失仍未实现；透明排序最大精度既有像素失败也未解决。
