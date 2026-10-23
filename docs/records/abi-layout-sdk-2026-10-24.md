# 布局 1.0 C++17 封装

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批为 `mms.ui.layout` 1.0 增加 C++17 封装与契约测试；生产代码不变。

## 实现

- [sdk/mms/include/mms/cpp/layout.hpp](../../sdk/mms/include/mms/cpp/layout.hpp)：`mms::sdk::LayoutServices` 查询 1.0 布局表并校验四个函数，提供 `SetDefaultHints`、`StageHints` 与 `AcquireSnapshot`；`LayoutSnapshotLease` 保存原快照句柄，移动语义明确，`Release` 显式调用宿主释放且析构不调用宿主。
- [cpp_sdk_checks.inc](../../tests/plugins/cpp_sdk_checks.inc)：验证查询、缺失服务/缺失函数、未知元素与事务错误、快照获取/移动/显式释放及幂等；输出 `cpp layout sdk PASS`。

## 验证

证据目录 `build/verification/abi-layout-sdk-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 默认全量契约套件 | 通过，`PASS checks=44792`；输出 `cpp layout sdk PASS` |
| 生产产品 | 本批只改 SDK 头与测试；完整产品 `10c7d991…`、原始兼容产品 `af2302c7…` 与正式 release `180db258…` 未变 |

## 边界

- 封装不检查调用线程；快照租约必须显式释放，析构不回调宿主。
- 假宿主验证包装转发与句柄语义；真实布局事务由既有布局批次覆盖。

## 透明排序 MME 标记回归定位

`model_alpha_sort_off_mme_presence` 在 ABI 分支失败。对照 MMS 工作树源码，main 的固定管线也在排序关闭且存在 `MMEffect.dll` 时使用 `ModelAlphaPass::Original` 单次 authored walk；ABI 与 main 该条件一致。尝试只移除固定管线的 MME 条件后，资源 0 的断言通过但资源 117 的效果路径仍失败，说明该行为不是单处条件偏差。已恢复与 main 完全一致的条件，不凭测试夹具改渲染行为；该 CTest 保持未通过并列为开放问题（固定与效果路径各需 main 行为确认）。
