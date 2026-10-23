# 渲染帧回调

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批为 `mms.render` 增加 1.1 每帧回调，补齐 DX9 成功呈现后的帧生命周期观察点。

## 实现

- [render.h](../../sdk/mms/include/mms/render.h)：新增 `MmsRenderFrameCallbackV1` 与 `MmsRenderApiV1_1`，在 1.0 三个函数后追加 `setFrameCallback`、`clearFrameCallback`；旧 1.0 表不变。
- [render_service.hpp](../../src/plugins/render_service.hpp)、[render_service.cpp](../../src/plugins/render_service.cpp)：服务登记客户端及其回调；`FramePresented` 更新帧信息后复制回调列表并逐个调用，单个回调异常被捕获，不影响呈现路径；插件停止时清除回调与客户端登记。
- [manager.cpp](../../src/plugins/manager.cpp)：`mms.render` minor=1 返回 1.1 表，minor=0 仍返回 1.0 表。
- lifecycle 探针查询 1.1 表并注册帧回调，日志记录前三次 `FRAME_CALLBACK frame=N gen=G`；宿主探针断言 `FRAME_CALLBACK_REGISTERED` 与第一条回调日志出现且无失败标记。

## 验证

证据目录 `build/verification/abi-frame-callback-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 宿主事件探针 | 通过 8 项；插件日志含 `FRAME_CALLBACK frame=1 gen=1`，无 `FRAME_CALLBACK_FAIL` |
| 默认全量契约套件 | 通过，`PASS checks=44792` |
| 相关 CTest | 10 个模式全部通过，10.4 秒 |
| 完整产品 | 构建通过，SHA-256 `cd85d4c0401684116d47b2de8cb0c1f749feaa0060591a74b35fb60f2de5b708` |
| 原始兼容产品 | 构建通过，SHA-256 `bed26c6a69bb15cdcfcd056107d8fc813a9c6764083933bbee8aeb6b7f7805ae` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 回调从 `FramePresented` 的呈现路径同步调用，位于插件回调之外；回调被要求轻量、只读且不得调用宿主服务或阻塞，异常会被吞掉。
- 探针直接调用宿主入口触发一次帧回调；`frame_driver` 的真实播放循环、设备丢失期间的呈现与多线程行为没有端到端验证。
- post 处理、begin/end 帧钩子与租约分类仍未实现。
