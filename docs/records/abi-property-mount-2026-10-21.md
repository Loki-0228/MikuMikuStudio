# 属性区插件扩展行

`mms.properties.main.extensions` 已提供真实的主窗口扩展行，位于停靠主视口下方、本体底部参数区上方。它支持已有 BUTTON、GROUP、SEPARATOR 和 INPUT。状态为本机自动验证通过，尚未完成本批产品集成和真机输入验收。

## 注册与布局

插件通过原 `mms.ui.surface.acquireAnchors` 查询挂载点和 `allowedKinds`，再用原 `mms.ui.registerElement` 注册元素。C 接口填写 `parentId`；C++ 包装填写 `InputSpec.parent`、`button.parent` 或 `group.parent`。属性、命令、元素 handle、所有者和生命周期沿用原实现，不增加 C 表或 ABI 版本。

`mms.toolbar.main.extensions` 和属性扩展行可以同时存在。一个属性可以绑定到两个不同的 INPUT 元素，各元素仍有自己的原注册 handle 和呈现实例；不会复制属性注册或建立另一套控件树。用法见 [INPUT 参考](../../sdk/mms/input.md)和 [C++17 界面注册](../../sdk/mms/cpp-ui.md#布局和能力边界)。

属性行按原布局提示量测、伸缩、分组及换行，用户保存的覆盖仍优先。内容超过可用高度时使用区域内滚动。没有有效控件或保留槽位时，高度为 0，不改变本体原有布局。工具栏的 `MMS_UI_OVERFLOW_MENU` 不适用于属性行，设置该策略返回 `UNSUPPORTED`；INPUT 也不会进入命令溢出菜单。

停靠视口的可用矩形、图像适配及覆盖区同时扣除属性行高度，既有 HUD 和交互继续使用这些实际视口几何。全屏及浮动模式不预留属性行。相关接线位于 `integration.cpp`、`viewport_bounds.hpp`、`ui_viewport_refresh.cpp` 和 `frame_presentation.cpp`；本批自动测试检查几何计算与覆盖区边界，没有新增 GPU 或最终产品画面验收。

## 刷新、草稿及清理

两个挂载根使用同一注册表、布局实现和 `mms.ui.refresh` 队列。后台仍通过原 request 入队，DATA、STATE、LAYOUT、REPAINT 按原顺序在 UI 安全点消费，结果写入原票据。能力按 `getTarget` 查询，不因挂载在属性行就扩大：

| 对象 | 已实现范围 |
| --- | --- |
| INPUT | REPAINT、默认属性 DATA、STATE、LAYOUT。显式 DATA 绑定仍优先，活动草稿保留起始值及文本，规范值变化后拒绝旧提交。 |
| BUTTON | 原命令状态和内容提供者、重绘与保留布局。DATA 继续遵守原显式绑定规则。 |
| SEPARATOR | 重绘与保留布局，不声明独立属性读回或命令状态。 |
| 注册 GROUP | 使用原 handle 提交同插件 SUBTREE，固定原成员、来源集合与阶段顺序。 |

INPUT 标签、启用、只读和可见状态沿用原 STATE 限制。纯 LAYOUT 使用缓存状态和成功发布的文案，不调用属性读取，也不求值场景、移动播放头、修改工程或增加撤销记录。

STATE 隐藏后可用 STATE 与 LAYOUT 组合收缩到零高度，并用同一呈现实例恢复。用户布局隐藏可能卸载控件，旧目标随之失效，不能用状态刷新绕过用户布局。禁用、隐藏、卸载和跨根移动先取消草稿及提交意图，再处理焦点和窗口清理。

通过原布局事务在两个根之间移动元素，元素 handle 和属性记录不变，原生视图重建。旧实例及其排队请求失效，插件重新查询目标后再提交刷新。插件停止和注销使用原清理流程，不保留旧 HWND 的回调或可提交草稿。

普通生产启动仍未提供 `mms.edit`：服务查询返回 `NOT_FOUND`，属性暂存返回 `NOT_READY`，INPUT 以只读方式显示。受控编辑宿主中的提交测试不代表普通生产编辑服务已经开放。

## 验证证据

本批使用已保存的以下报告，完整配置与原始兼容配置均通过：

| 检查 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| `--property-mount-only` | 681 项 | 681 项 |
| `--input-state-only` | 1,559 项 | 1,559 项 |
| `--subtree-refresh-only` | 1,280 项 | 1,268 项 |
| 完整 ABI 契约 | 17,565 项 | 17,565 项 |

最终报告为 [ui-hidden-full-04](../../build/verification/abi-physics-property-20261021/ui-hidden-full-04/report.json)和 [ui-hidden-original-04](../../build/verification/abi-physics-property-20261021/ui-hidden-original-04/report.json)，各自包含上述四个入口。报告保存运行命令、退出码和原日志路径；这些本地证据不保证随仓库分发。SUBTREE 的实际窗口分支数量按各次报告记录。

本轮是独立进程、同桌面隐藏窗口的功能回归，报告明确标注 `realPrivateDesktop=false`。早期运行器的 `lpDesktop` 未实际传入子进程，不能引用为独立桌面证据，详见[桌面环境更正](abi-verification-desktops-2026-10-21.md)。

属性挂载测试使用真实 HWND、原 C 表及同对象 C++ SDK，覆盖两个根同时挂载、祖先身份、窄宽换行、实际窗口与 DIP 几何、四类刷新及后台入队、草稿保留、过期提交、零高恢复、只读、跨根移动和卸载。视口检查覆盖属性行高度为 0 的原矩形相等、停靠视口避让、覆盖区不越界，以及全屏、浮动和过小客户区。

本批没有执行最终产品启动、原名或改名产物检查，也没有新增微软拼音真机、175% 持续拖动或跨屏不同 DPI 验收。合成 IME 消息与实际窗口几何断言不能替代这些人工项目。历史菜单自动选择的间歇超时仍按[调查记录](abi-menu-selection-investigation-2026-10-21.md)保留，本轮通过不表示其根因已修复。

## 保留范围

DOCK、左右侧栏、时间线及状态栏挂载、属性行命令溢出、通用面板折叠、全部本体控件与业务命令查询、布局撤销会话、多视口、独立求值、绘制层及复杂动画接口仍未实现。本批扩展行不扩大这些服务的状态。

Win7 SP1 x64、CPU 基础路径、AMD 兼容和程序目录 `data` 的存储规则保持。本批没有增加新系统接口或特定显卡厂商依赖；Win7、AMD 真机及运行库部署仍需验收。
