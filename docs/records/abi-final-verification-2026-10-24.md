# 最终验证矩阵

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本页是一次集中复跑，不新增功能结论；源码状态为 `f6b14c0`（场景 SDK）之后的文档提交，二进制与需求最终审计时的产品一致。

## 复跑范围

| 项目 | 命令/对象 | 结果 |
| --- | --- | --- |
| 默认全量契约套件 | `plugin_abi_tests.exe` | `PASS checks=44806` |
| 目标 CTest | 15 个 `model_alpha_*`、宿主/业务事件、面板/菜单状态、legacy MME 检测、6 个 UI 刷新运行时 | 26 项中 25 项通过；仅 `model_alpha_sort_off_mme_presence` 失败（41.24 秒） |
| 完整产品构建 | `cmake --build build/cmake --config Release --target MikuMikuStudio` | 通过 |
| 原始兼容产品构建 | `build/verification/mms-marker-sync-20261020/original-sdk19041` 同目标 | 通过 |
| 完整部署闭包 | 安装到 `build/deployment-full-20261024` 并审计 | `DEPLOYMENT CLOSURE OK: 55 binaries, 51 redistributable, 2 DirectX files` |
| 原始部署闭包 | 安装到 `build/deployment-original-20261024` 并审计 | 同上 |
| 完整产品 SHA-256 | `build/cmake/Release/MikuMikuDance.exe` | `8a7d3937f0814f2a62a0f55b4da60b1600ae6801125bd2e50d499ab1b54135dc` |
| 原始兼容产品 SHA-256 | `build/verification/.../original-sdk19041/Release/MikuMikuDance.exe` | `afda7164be1ccab6807ee03cbdaeb5e5a6ad748d95542344eda5b968d355266c` |
| 正式 release SHA-256 | `build/release/MikuMikuDance.exe` | `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，未覆盖 |
| main 工作区 | `D:/MMD-Next/MMS` | HEAD `d551f1f`，与同步记录一致 |

证据文件在 `build/verification/abi-final-verification-20261024/`：`suite.log`、`suite.err`、`ctest.log`、`deployment-full.json`、`deployment-original.json`。

## 已知失败

`model_alpha_sort_off_mme_presence` 仍在加载 MMEffect 存在标记后检查 full Off 的固定管线顺序时失败。main 源码在同一条件下也保留 authored walk，ABI 保持与 main 对齐；本页不把 25/26 写成全部通过。

## 结论

本机可复跑的功能、构建、部署闭包与兼容性检查保持上一轮结果；外部条件（Win7 SP1 x64、Windows 10、AMD、真实 MME 旧插件、多 DPI/输入法、人工视觉与性能基线、真实播放循环与设备丢失）仍未验收。完整 P0–P6 未完成，持续目标保持 active。
