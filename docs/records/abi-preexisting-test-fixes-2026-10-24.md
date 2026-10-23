# 既有测试问题修复（runtime_layout 与 sidebar）

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批修复两个既有问题：原始兼容配置 `runtime_layout_tests` 曲线宿主崩溃、原始兼容配置 `sidebar_resize_tests` 目标不可编译。

## 问题一：原始配置 runtime_layout 崩溃

现象：原始兼容产品（`mmd932` 配置、UI 测试开启）运行 `runtime_layout_tests.exe`，在输出 `FAIL: read shadow curve channels` 后立即以访问冲突退出（0xC0000005）。

原因：`TimelineParameterReadouts` 用例读取并写入阴影曲线通道，随后直接索引 `channels[1]`。曲线宿主是完整配置能力，原始配置下 `curve_host::Read` 返回 false，`channels` 为空，下一行越界导致崩溃；该用例的播放读数期望也依赖曲线写入结果。

修复：[runtime_layout_tests.cpp](../../tests/runtime_layout_tests.cpp) 在原始配置下跳过该曲线读数用例并打印说明，其余布局/面板用例继续运行；完整配置路径不变。

## 问题二：原始配置 sidebar_resize_tests 不可编译

现象：以 `MIKUMIKUSTUDIO_BUILD_UI_TESTS=ON` 配置原始兼容构建并编译 `sidebar_resize_tests`，在 `src/plugins/scene_object_history.hpp` 报 `model_import::Prepared`、`resource_budget::Collector`、`accessory_import::Prepared` 等类型不存在。

原因：该宿主测试直接包含完整配置的插件历史头文件与运行时；原始兼容产品没有这些组件。测试本身是完整配置的 UI 验收用例。

修复：[CMakeLists.txt](../../CMakeLists.txt) 把 `sidebar_resize_tests` 目标限定为完整配置（非 `mmd932` 时创建），不再让原始配置构建尝试编译它；完整配置目标与运行不变。

## 验证

证据目录 `build/verification/abi-preexisting-fixes-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 原始配置 `runtime_layout_tests` | 通过，exit 0（不再崩溃；曲线读数用例打印跳过说明） |
| 完整配置 `runtime_layout_tests` | 通过，exit 0 |
| 完整配置 `sidebar_resize_tests` | 构建通过；隔离副本运行 40.3 秒通过（`Sidebar live resize regressions passed`） |
| 原始配置 CMake 重新生成 | 通过，不再创建 `sidebar_resize_tests` 目标 |

## 未完成

- 原始配置其他 UI 测试未逐一重跑；完整配置 UI 测试仍按原有范围各自覆盖。
- 矩阵中「原始兼容回退路径未同步 `SceneModified()` 与 `EM_SETMODIFY`」仍未处理，不属于本次测试问题修复。
