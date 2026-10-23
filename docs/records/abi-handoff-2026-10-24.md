# 分支交接状态

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，HEAD `b82f7aa`（其后仅文档提交则按实际 HEAD 为准）。本页给下一轮或下一位维护者一份可复跑的入口，不替代各批次记录。

## 当前结论

完整 P0–P6 尚未完成，持续目标保持 active。最近一次[需求最终审计](abi-final-audit-2026-10-24.md)按 142 行需求统计：本机已验证类 120、已实现未验证 2、部分实现 6、部分人工验证 1、未实现 11、未开放 1、缺环境 1。[最终验证矩阵](abi-final-verification-2026-10-24.md)记录最近一次集中复跑。

## 分支与产物

| 项目 | 值 |
| --- | --- |
| main 工作区 HEAD | `d551f1f`（行为同步已完成） |
| ABI 工作树 | 干净，无未提交改动 |
| 完整产品 | `8a7d3937f0814f2a62a0f55b4da60b1600ae6801125bd2e50d499ab1b54135dc` |
| 原始兼容产品 | `afda7164be1ccab6807ee03cbdaeb5e5a6ad748d95542344eda5b968d355266c` |
| 正式 release | `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，未覆盖 |
| 部署闭包 | 两配置各 55 PE、51 运行时可再分发、2 DirectX；`DEPLOYMENT CLOSURE OK` |

## 复跑命令

```powershell
# 完整档构建与契约套件
cmake --build D:/MMD-Next/MMS-abi-dev/build/cmake --config Release --target MikuMikuStudio plugin_abi_tests
D:/MMD-Next/MMS-abi-dev/build/cmake/tests/Release/plugin_abi_tests.exe ...

# 目标 CTest（26 项；仅 model_alpha_sort_off_mme_presence 预期失败）
ctest -C Release -R 'model_alpha_|plugin_abi_host_events|plugin_abi_business_events|plugin_abi_panel_state|plugin_abi_menu_state|legacy_mme_renamed_detection|ui_.*_runtime' --output-on-failure

# 原始兼容档构建
cmake --build D:/MMD-Next/MMS-abi-dev/build/verification/mms-marker-sync-20261020/original-sdk19041 --config Release --target MikuMikuStudio

# 部署闭包
cmake --install D:/MMD-Next/MMS-abi-dev/build/cmake --config Release --prefix D:/MMD-Next/MMS-abi-dev/build/deployment-full-20261024
pwsh -File D:/MMD-Next/MMS-abi-dev/tools/verify-deployment-closure.ps1 -DeploymentDir D:/MMD-Next/MMS-abi-dev/build/deployment-full-20261024 -ReportPath ...json
```

上一次结果：默认套件 `PASS checks=44806`；目标 CTest 26 项中 25 项通过，仅 `model_alpha_sort_off_mme_presence` 失败；`legacy_mme_renamed_detection` 20 项通过。

## 最近批次

- `5e46307`／`0684375` 透明排序缓存 main 同步。
- `1cb6c6f`／`b07a7c1` 布局 1.0 C++17 封装。
- `1abd224`／`8ea8779` 渲染帧回调 1.1。
- `9129f04`／`61b35c7` 透明排序缓存完整 main 移植（按对象投影快照）。
- `0f391c3`／`adf34ce` legacy MME 改名/子目录检测。
- `0ea588a`／`f80dc6d` 帧开始/结束阶段回调 1.2。
- `f6b14c0`／`85bfef2` 场景 C++17 封装。
- `3050edd` 需求最终审计；`4d9e9fb` 最终验证矩阵；`b82f7aa` MMHack 检测覆盖。

## 已知失败

`model_alpha_sort_off_mme_presence`：加载 MMEffect 存在标记后，full Off 固定管线没有保持原生 pass 顺序。main 源码在同一条件下也保留 authored walk；ABI 与 main 对齐，未为夹具偏离。需要上游确认该测试的预期，再决定是否改渲染行为。

## 剩余需求

- P2：DOCK；本体菜单/控件/业务命令全部语义登记；侧栏、时间线、状态栏挂载；全区域统一几何、绘制和命中迁移。
- P3：求值阶段 1–4 及未支持定义类型；animation 轨道/层/片段/变速/烘焙；显式导入和扩展文件重新关联；原生统一撤销及可写属性（未开放）。
- P4：compute 两阶段求值、状态域、CPU 协商；binding/cache/bake/MMV 等价清单。
- P5：post 处理、租约分类、真实播放循环与设备丢失帧验证。
- P6：原创 MMSAdapter、独立 MMHack 兼容导出；真实旧 MME 插件导出/调用约定/寿命/实际渲染验收。
- 其他：P1 `COMPUTE_POLICY_CHANGED` 与属性事件目标身份/声明式路径插件级断言；P2a MME 阶段计数与 ABBA 基线；P2c 真实 MME 状态、桌面遮挡与多 DPI。

## 外部待验收

Win7 SP1 x64、Windows 10、AMD CPU/GPU、真实 MME 旧插件、多 DPI 与输入法、人工视觉、性能基线、真实播放循环与设备丢失场景。

## 下一步建议

1. 先由上游确认 `model_alpha_sort_off_mme_presence` 的预期，再决定渲染修正或夹具调整。
2. 按需求表优先补 P2 语义登记与 P3 求值阶段，它们解锁 P4 与部分 P6。
3. 每批保持：源码、契约测试、产品哈希、部署闭包、记录与提交成对更新，不沿用旧数字。
