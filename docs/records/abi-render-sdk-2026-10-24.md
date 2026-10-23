# 渲染服务 C++17 封装

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批为 `mms.render` 1.0 增加 C++17 SDK 封装与契约测试；生产代码不变。

## 实现

- [sdk/mms/include/mms/cpp/render.hpp](../../sdk/mms/include/mms/cpp/render.hpp)：`mms::sdk::RenderServices` 查询 `mms.render` 并复制同一 C 表；`Info` 读取设备代次、丢失状态、帧与后缓冲字段；`Acquire` 构造 `MmsRenderLeaseDescV1`、登记设备租约并返回 `RenderLease`；`RenderLease` 移动语义明确，`Release` 幂等且只在显式调用时回调宿主，析构不调用宿主。
- [cpp_sdk_checks.inc](../../tests/plugins/cpp_sdk_checks.inc)：新增假宿主 `mms.render` 服务并验证查询参数、`Info`、缺失服务/缺失函数的错误、`Acquire` 参数、`Release` 计数与幂等、移动后的句柄归属；通过时输出 `cpp render sdk PASS`。
- [sdk/mms/render.md](../render.md) 增补 C++17 用法；[sdk/mms/README.md](../README.md) 标注封装。

## 验证

证据目录 `build/verification/abi-render-sdk-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 默认全量契约套件 | 通过，`PASS checks=44772`；输出含 `cpp render sdk PASS` |
| 生产产品 | 本批只改 SDK 头与测试；完整产品 `903bf97a…`、原始兼容产品 `b20a8822…` 与正式 release `180db258…` 未变 |

## 边界

- 封装不检查调用线程；宿主回调线程规则仍由 C ABI 服务约定。`RenderLease` 不在析构时自动释放，插件须在停止前显式 `Release`。
- 假宿主只验证包装转发与句柄语义；真实服务路径已由 lifecycle 探针和宿主事件探针在渲染服务批次覆盖。帧序号、后缓冲信息、post、租约分类仍未实现。

## 透明排序 GPU 缓存尝试

本批同期尝试给有序透明通道增加默认池缓冲复用（计划缓存命中的帧跳过上传）。实现完成后 `model_alpha_occlusion_tests --precision maximum` 出现 27 个像素样本失败；把渲染改回未修改基线后同一条测试仍失败同样 27 个样本，说明该失败是既有问题，缓存尝试没有可用的回归证据。为避免带入未验证行为，缓存与统计改动已全部回退，未提交；失败证据保留在 `build/verification/abi-transparent-sort-cache-20261024/`。透明排序性能缓存仍列为未完成，需要先定位既有最大精度像素失败。
