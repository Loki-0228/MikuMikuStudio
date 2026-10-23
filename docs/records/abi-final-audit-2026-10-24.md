# ABI 需求最终审计

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本页按 [PLUGIN_ABI_IMPLEMENTATION](../PLUGIN_ABI_IMPLEMENTATION.md) 的 P0–P6 需求表逐行统计当前状态，汇总已完成证据与未完成范围。本轮不新增验收结论，只核对已有代码、测试与记录。

## 结论

完整 P0–P6 尚未完成，持续目标不能标记完成。需求表共 **142 行**：

| 状态 | 行数 |
| --- | --- |
| 本机已验证（含受控路径、自动回归与安装静态审计） | 120 |
| 已实现未验证 | 2 |
| 部分实现（含 P6 检测部分实现） | 6 |
| 部分人工验证 | 1 |
| 未实现 | 11 |
| 未开放 | 1 |
| 缺其他系统/硬件环境 | 1 |

本机已验证类覆盖事件、布局、刷新、场景与编辑、导入、部署闭包、渲染设备/帧服务、透明排序缓存和 legacy MME 检测。剩余主体集中在 DOCK、本体语义登记、全区域迁移、求值阶段、compute/bake、MMSAdapter/MMHack 与若干外部环境项。

## 最近完成（按批次）

| 批次 | 提交 | 结果 |
| --- | --- | --- |
| 透明排序缓存 main 同步 | `5e46307`／`0684375` | 同步缓存夹具与统计接口，14/15 个 `model_alpha_*` 模式通过 |
| 布局 1.0 C++17 封装 | `1cb6c6f`／`b07a7c1` | `LayoutServices`／`LayoutSnapshotLease`，假宿主契约通过 |
| 帧回调 | `1abd224`／`8ea8779` | `mms.render` 1.1 每帧回调，宿主探针通过 |
| 透明排序 main 完整移植 | `9129f04`／`61b35c7` | 按对象投影快照、六参数几何接口，14/15 模式通过 |
| legacy MME 改名/子目录检测 | `0f391c3`／`adf34ce` | 20 项检测 CTest 通过（含 MMHack），30 处硬编码判断统一入口 |
| 帧阶段回调 | `0ea588a`／`f80dc6d` | `mms.render` 1.2 BEGIN/END，宿主探针通过 |
| 场景 C++17 封装 | `f6b14c0`／`85bfef2` | `SceneServices`／`SceneSnapshot`，套件 44,806 项 |

## 已实现未验证

- P0 — Media Foundation 能力回退。
- P2f — 两个示例的真实输入法、多 DPI、视觉交互。

## 部分实现

- P1 — 全部业务事件通知：已交付 10 个业务事件与帧/布局/DPI 等事件；`COMPUTE_POLICY_CHANGED`、属性事件目标身份和声明式路径的插件级断言仍缺。
- P2 — 左右侧栏、时间线、状态栏挂载：工具栏、工作区双页签、属性行已挂载；侧栏、时间线、状态栏未接。
- P2b — 本体既有参数区迁移与全区域缓冲：扩展区已接入，本体全部参数区和全区域缓冲未迁移。
- P3 — 原生保留资源计量与准入：保留组件已有，实际额度与消费者未接。
- P5 — render、post、DX9 帧/设备代次和资源租约：1.0 设备/租约、1.1 帧回调、1.2 阶段回调与帧信息已完成；post、租约分类、真实设备丢失与真实播放循环未验收。
- P6 — legacy_mme 改名和子目录检测：检测已实现并测试；真实旧插件导出/调用约定/寿命/渲染未验收，MMSAdapter/MMHack 兼容导出未实现。

## 未实现

- P2 — DOCK。
- P2 — 本体菜单、控件、业务命令全部语义登记。
- P2a — MME 内部阶段计数和 ABBA 性能基线。
- P2c — 真实 MME 内部状态、真实桌面遮挡及多 DPI。
- P2d — 全区域统一几何、绘制和命中迁移。
- P3 — 求值阶段及未支持的定义类型（阶段 1–4、翻转/冲量 Morph、其他关节、复合形状、软体）。
- P3 — animation 轨道/键、层、片段、变速、烘焙和选择。
- P3 — 显式导入和扩展文件重新关联。
- P4 — compute 两阶段求值、状态域及 CPU 协商。
- P4 — binding、cache、bake、MMV 等价清单。
- P6 — 原创 MMSAdapter、独立 MMHack 兼容导出。

另有 P2 输入法组合态、Tab、辅助功能和真实多 DPI 为部分人工验证；P3 原生统一撤销及可写属性未开放；P0 Win7 SP1、Win10、AMD 真机缺环境。

## 本机证据

| 项目 | 当前结果 |
| --- | --- |
| 默认全量契约套件 | `PASS checks=44806`（第 37 轮） |
| 宿主事件探针 | 8 项通过，含 `FRAME_CALLBACK`、`FRAME_PHASE_*` |
| 透明排序 `model_alpha_*` | 14/15 通过；`model_alpha_sort_off_mme_presence` 失败 |
| legacy MME 检测 | 20 项通过（MMEffect/MMHack 改名、子目录、卸载、无关名） |
| 相关 CTest | 帧阶段批 9/9、帧回调批 10/10、宿主事件通过 |
| 部署闭包 | 55 个 PE、51 个运行时可再分发、2 个 DirectX；`DEPLOYMENT CLOSURE OK` |
| 完整产品 | `8a7d3937f0814f2a62a0f55b4da60b1600ae6801125bd2e50d499ab1b54135dc` |
| 原始兼容产品 | `afda7164be1ccab6807ee03cbdaeb5e5a6ad748d95542344eda5b968d355266c` |
| 正式 release | 未覆盖，`180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

证据目录按批次保存在 `build/verification/abi-*`，记录在 [docs/records](.)。

## 已知失败

`model_alpha_sort_off_mme_presence`：加载 MMEffect 存在标记后，full Off 的固定管线结果没有保持「不随 authored 材质顺序改变的原生 pass 顺序」。main 的固定管线在同一条件下也保留 authored walk，ABI 已与 main 对齐；不去为了单个夹具偏离 main，该模式保持未通过。

## 同步与外部条件

- main 工作区 HEAD 仍为 `d551f1f`；其行为同步已完成，记录见[范围盒同步](abi-main-d551f1f-2026-10-24.md)与[main 行为同步矩阵](abi-main-sync-matrix-2026-10-23.md)。
- 外部待验收：Win7 SP1 x64、Windows 10、AMD CPU/GPU、真实 MME 旧插件、多 DPI/输入法、人工视觉与性能基线、真实播放循环与设备丢失场景。
- [最终验证矩阵](abi-final-verification-2026-10-24.md)集中复跑：默认套件 44,806 项、目标 CTest 26 项中 25 项通过、两产品构建与部署闭包通过、产品与 release 哈希未变。
- 本页只代表 2026-10-24 当前工作树状态；后续批次需更新本页与需求表，不能沿用旧数字。
