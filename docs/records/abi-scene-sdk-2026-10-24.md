# 场景 C++17 封装

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批为 `mms.scene` 1.0 增加 C++17 封装与契约测试；生产代码不变。

## 实现

- [sdk/mms/include/mms/cpp/scene.hpp](../../sdk/mms/include/mms/cpp/scene.hpp)：`mms::sdk::SceneServices` 查询场景表并校验 acquire/release/objects/readChannel；`Acquire` 构造查询（阶段、CURRENT_TIME、通道、时间、层、对象过滤）并返回 `SceneSnapshot`；`Objects`／`ReadChannel` 返回借用 span；`SceneSnapshot` 可移动、`Release` 显式幂等且析构不回调宿主。
- [cpp_sdk_checks.inc](../../tests/plugins/cpp_sdk_checks.inc)：假场景服务验证查询参数、快照获取与句柄、对象/通道转发、移动与释放计数、无效快照返回 NOT_READY、缺失服务/函数与旧宿主 VERSION；输出 `cpp scene sdk PASS`。

## 验证

证据目录 `build/verification/abi-scene-sdk-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 默认全量契约套件 | 通过，`PASS checks=44806`；输出 `cpp scene sdk PASS` |
| 生产产品 | 本批只改 SDK 头与测试；完整产品 `8a7d3937…`、原始兼容产品 `afda7164…` 与正式 release `180db258…` 未变 |

## 边界

- 封装不检查调用线程；span 只在快照租约有效期内借用，租约必须显式释放。
- 假宿主验证包装转发与生命周期语义；真实场景快照路径已由既有场景批次覆盖。
- 阶段 1–4 仍未实现；包装只转发宿主支持的阶段。
