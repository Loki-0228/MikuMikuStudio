# 布局撤销历史 C++17 封装

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批为 `mms.ui.layout` 1.1 增加 C++17 SDK 封装与契约测试；生产代码不变。

## 实现

- [sdk/mms/include/mms/cpp/layout_history.hpp](../../sdk/mms/include/mms/cpp/layout_history.hpp)：`mms::sdk::LayoutHistoryServices` 以 major=1 minor=1 查询布局历史表，要求 1.0 四个布局函数与 `queryHistory`／`undo`／`redo` 全部存在；`SetDefaultHints` 保留原布局能力，`Info` 读取 canUndo/canRedo/深度，`Undo`／`Redo` 返回原始结果。只持有宿主表，不拥有布局状态。
- [cpp_sdk_checks.inc](../../tests/plugins/cpp_sdk_checks.inc)：假宿主增加 `mms.ui.layout` minor=1 分支与历史计数，验证查询参数、Info 字段、Undo/Redo 转发、缺失服务/缺失函数、旧宿主返回 MMS_E_VERSION；输出 `cpp layout history sdk PASS`。

## 验证

证据目录 `build/verification/abi-layout-history-sdk-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 默认全量契约套件 | 通过，`PASS checks=44781`；输出 `cpp layout history sdk PASS` |
| 完整产品 | 构建通过，SHA-256 `5e78a3138e5d5dee48a3e8dba11bc78e073d66a53c4fa5867ab1f1ed5babcd3c` |
| 原始兼容产品 | 构建通过，SHA-256 `b0780ea816dc34e06f0dae09fe67c15ab9620bfe0ac62188d8ffb0a655ebd263` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 假宿主验证包装转发与错误映射；真实 registry 的撤销/重做语义已由布局批次 C ABI 检查覆盖。
- 封装不检查调用线程；历史只在进程内、全局布局作用域，没有热键或界面入口。

## 透明排序遮挡回归定位

本批另核对 `model_alpha_occlusion_tests`：当前 HEAD 与透明排序批次提交 `3f37a38` 上，standard 与 maximum 模式都出现同样 27 个双层像素失败，说明这些失败不是后续 ABI 变更引入，也不是本批 SDK 改动造成；透明排序 GPU 缓存仍不能据此实现。该问题保持未解决并记录，等待单独定位夹具或渲染状态差异。
