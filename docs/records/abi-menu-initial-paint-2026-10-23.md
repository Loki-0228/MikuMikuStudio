# 菜单首次绘制与回调清理

2026-10-23。ABI 基线为 `b4985dd`，开始及构建完成后核对 main 为 `fed9768f9fe726c4b5e068849808803f1713ad42`。行为来源为 main `3782e0c` 的菜单首次绘制，只读取已提交内容。

## 实际行为

增强版主窗口在 `WM_CREATE` 安装一个菜单子类过程。效果编辑器在 `InstallEffectListDisplay` 中为编辑器及当时存在的全部后代安装同一辅助模块，保留原列表绘制子类。重复安装不改变有效附件的代次。后来创建的子窗口须再次调用安装入口才会纳入，不承诺自动观察动态子窗口。原始兼容配置不附加该模块。

每次打开菜单以独立代次记录。原窗口处理完成后才启动 16 ms 计时器，只匹配当前线程、可见、类名为 `#32768` 且 HMENU 相符的窗口。首次发现时重绘，首次菜单 idle 再补绘一次，从首次可见起经过 220 ms 后最终补绘。首次发现与首次 idle 重合时合并为一次。

未完成项通常在打开或首次可见后 600 ms 停止轮询。计时器先枚举再判断期限，所以迟到的计时器仍可能发现刚可见的菜单，并从该次可见起继续补绘。超时不伪造已完成状态，随后真实 idle 仍可观察菜单，但不会重新启动轮询。

`WM_INITMENUPOPUP` 在转发前登记代次，转发后只完成同一条记录。原处理期间关闭、销毁、重开或重新附加，不会使旧回调恢复新菜单的计时器。枚举及 idle 保存开始时的代次上界，防止同步回调重开相同 HMENU 后，把旧观察写入新记录。绘制前先写状态，绘制回调返回后不再访问旧引用。

关闭菜单、退出菜单循环及窗口销毁会清理对应工作。失效子类的再次安装分配新代次；未登记窗口及跨线程 `Detach` 不操作其计时器或子类。新增菜单分配失败保留已有菜单，计时器创建失败清除该窗口的待处理项，原生消息仍继续处理。模块不写工程、播放头或历史。

## 实现与分工

`src/window/menu_initial_paint.cpp` 实现模块，公开的内部 C++ 入口为 `Attach`、`Detach`，本批不增加插件 C ABI。`menu_initial_paint_detail.hpp` 提供私有测试接口。主窗口和效果编辑器只经子类分派，避免同一菜单消息处理两次。

DSH 使用已安装的 `dsh-delegate` skill，仅新增模块源码。真实会话为 `8df4d8bb-7211-47f0-9e63-2d9a90175f14`，返回 cwd 为 `D:\MMD-Next\MMS-abi-dev`。正式结果为 completed，`sawError=true`；过程中的工具调用失败保留在事件记录中，不能将完成状态当作无错误或验收通过。Codex 完成契约、接入、测试、独立审查、构建和文档。

## 验证证据

证据目录为 `build/verification/abi-menu-initial-paint-20261023`。所有运行串行执行，测试程序复制到独立安装目录，继承 Default 桌面，只使用自有隐藏窗口，不使用全局键鼠输入。

| 验证 | 结果与证据 |
| --- | --- |
| 两配置产品及两个专项目标 | `build-full-restored.json`、`build-original-restored.json` 均退出 0。 |
| 增强版核心 | `core-full-1` 与 `core-full-restored` 各 1,861 项通过。 |
| 增强版接入 | `integration-full-1` 与 `integration-full-restored` 各 116 项通过。 |
| 原始兼容核心 | `core-original-1` 与 `core-original-restored` 各 27 项通过。 |
| 原始兼容接入 | `integration-original-1` 与 `integration-original-restored` 各 116 项通过。 |
| 移除观察代次上界 | `counter-cutoff` 在重开菜单继承旧绘制状态断言处失败，退出 1。 |
| 允许过期打开完成 | `counter-stale-complete` 在嵌套 INIT 只能启动一次计时器的断言处失败，退出 1。 |
| 移除主窗口接入 | `counter-main-attach` 在实际 WM_CREATE 安装断言处失败，退出 1。 |
| 源码恢复 | `candidate.json` 记录八个代码及测试文件；三个反例恢复原字节后重建，四组复验通过。 |
| 独立审查 | `review-production.txt`、`review-tests.txt`，最终整合审查另存 `final-review.txt`。 |
| 产品兼容边界 | `pe-check-final.json`：x64、子系统 6.1、效果资源 117 和 118 不变；无新增 DLL 依赖。增强版新增既有 Win32 `lstrcmpW` 导入，原始配置导入表无增减。 |

核心测试覆盖三个补绘时点、零时钟、延迟可见、过滤、多个窗口和菜单、原消息返回值、关闭及重开重入、残留计时器、外部移除子类后的重附加、安装与计时器失败。四组真实 `operator new` 失败扫描覆盖首次线程存储、已有其他窗口、新菜单及同窗口菜单增长；失败后检查已有状态和恢复。另有真实队列计时器测试，首次及最终补绘来自实际 WM_TIMER，不以手工发送代替。

接入测试通过真实 `CreateWindowExW` 进入生产 `MainWndProc` 的 WM_CREATE，只替换重型 `CreateUIControls` 初始化，不在测试里补调用 Attach。效果编辑器调用真实安装函数，检查已有后代、重复安装、晚创建子窗口及子树销毁，并验证列表自定义绘制标志和原返回值。

最终增强产品 SHA256 为 `cde813301177ecdc58261ff54821a761620a0a52d4ad19b209c4b98b9b863e0c`，原始兼容产品为 `8a5e450fe63564a8a6874d2b308416354718e114e27e51032bc990d2f404d660`。稳定版 `build/release/MikuMikuDance.exe` 保持 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。

## 尚未覆盖

弹出窗口的发现元数据及绘制调用由测试接口模拟。本机测试不证明真实菜单像素、主题、多 DPI、完整 MME DLL、Win7 或 AMD 真机表现。任意第三方移除子类后直接销毁窗口且不再调用 Attach 的情形，以及第三方占用同一计时器 ID，不在当前自愈保证内。原生窗口全过程的所有间接焦点影响也未作全面证明。

磁盘缓存管理、负帧、完整 UI 目录、多视口及其他 ABI 缺项继续保留，普通公共 `mms.edit` 仍关闭。本批不修改 VMD、PMM 或渲染路径。
