# 缓存管理窗口

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `f706dba`。本批在缓存管理业务 Store 之上实现应用内管理窗口；消费者（效果编译缓存、纹理准备缓存）仍未接入。

## 来源行为

main `3782e0c` 的缓存管理窗口：

- 「帮助」菜单动态加入「缓存管理…」，三语标题随 UI 语言刷新；打开 9200 原生对话框。
- 对话框显示启用缓存、自动清理、空间上限（GB）、优先保留最近工程数；汇总行显示已用/上限/可回收与 MME 效果、纹理分类占用。
- 工程列表显示名称、上次打开时间、独占/共享/可回收字节与使用状态；同名工程附带父目录区分。
- 「固定保留/取消固定」保存工程偏好；「清理所选工程」只删该工程独占且未使用的缓存；「清理可回收缓存」删除非最近、非固定、未使用和失效的缓存；「清空磁盘缓存…」经确认后清空可删缓存（固定工程也包含），使用中的文件跳过。
- 检查、清理、固定与应用设置都在工作线程执行，对话框用定时器轮询结果；操作期间按钮禁用、状态行显示进度或结果；关闭时若有操作则等完成后关闭。
- 设置上限 0.0625–100 GB、最近工程 1–100；输入非法时给出三语提示。

## ABI 实现

- 采用 [cache_manager_dialog.hpp](../../src/window/cache_manager_dialog.hpp) 与 [cache_manager_dialog.cpp](../../src/window/cache_manager_dialog.cpp)：`kCommand=32815`、`kDialog=9200`、控件 ID 9201–9217。ABI 的管理器 API 已与 main 对齐，因此界面逻辑基本原样采用；设置仍写中央 `data/settings.json` 的 `cacheManager` 段。
- [dialogs.rc](../../res/templates/dialog/dialogs.rc)：加入 main 的 9200 对话框模板（360×206，列表视图、设置输入、固定/清理/应用/关闭按钮、汇总与状态行），资源 ID 与已有的 9120/9162/9140 无冲突。
- 菜单与命令：[wndproc.cpp](../../src/window/wndproc.cpp) 在 `WM_INITMENUPOPUP` 时调用 `cache_manager_ui::RefreshMenu`，[command_dispatch.cpp](../../src/window/command_dispatch.cpp) 把 32815 路由到对话框；两处都在完整配置生效，原始兼容配置不显示入口。
- CMake：`src/window/cache_manager_dialog.cpp` 加入核心库；新增完整配置的 `cache_manager_ui_tests` 目标（链接核心、minhook、comctl32、user32，并嵌入对话框资源）。
- 测试：[cache_manager_ui_tests.cpp](../../tests/cache_manager_ui_tests.cpp) 采用 main 的三语真实对话框回归，并针对本机环境调整：
  - 宿主拒绝私有桌面时退回默认桌面，同时用 MinHook 的 `ShowWindow` 钩子隐藏所有窗口（原测试在私有桌面上运行）。
  - 每个语言用例使用隔离的 `MMS_CACHE_MANAGER_DIR` 与 `MMS_CACHE_MANAGER_SETTINGS`，避免中央设置残留影响默认值。

## 验证

本机 Win10 x64，隐藏窗口，三语各一轮；证据目录 `build/verification/abi-cache-manager-ui-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `cache_manager_ui_tests` | 通过，152 项检查、0 失败：帮助菜单出现入口、真实对话框打开、两种默认勾选、默认 4 GB/10 个工程、列表两个工程、紧凑尺寸与控件不出界、本地化按钮文字宽度、四个悬浮提示、禁用/重新启用缓存、自动清理独立开关、固定工程、清理所选保留共享、最近工程数保存、清理可回收、全部清理确认后清空、三语完成（worker work=0） |
| 对话框位图 | `work/dialog-0/1/2.bmp`（三语各 1,098,254 字节），已抽查英文界面：标题、设置行、汇总行、工程列表（同名工程带父目录区分）、按钮与状态区布局正常 |
| 完整产品 | 构建通过，EXE SHA-256 `69ea6c93360841db3ebbcd9959ce7a618ee50d8c888c4d6acc9d17405e98a389` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `cf6190dc0a8555f26bf1fab79aee711372db47e704a13a8502dada53b41bd854` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 位图只做了开发侧抽查，没有人工视觉验收；没有验证多 DPI 缩放、高对比主题、长路径列表与超长工程名的完整呈现。
- 本机私有桌面被拒绝，测试在默认桌面以隐藏窗口运行；没有验证真实用户桌面上对话框的焦点、模态与键盘导航。
- 管理窗口只管理已登记缓存；效果编译缓存与纹理准备缓存消费者未接入，所以没有真实 `.fxb`/`.mtc` 文件、命中或回收证据。原始兼容配置不显示入口。

## 后续

下一批接入效果编译缓存与纹理准备缓存消费者，用真实效果/纹理工程收集命中、容量与回收证据。
