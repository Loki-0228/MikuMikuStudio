# 原生菜单自动选择的间歇超时

此前完整契约在 `overflow_menu_checks.inc:265` 超时：菜单创建和 WM_MENUCHAR 均完成，但菜单未按 MNC_EXECUTE 返回，最终由 3 秒定时器取消。本轮没有修改生产菜单或正式测试，也没有投递额外 Enter 或伪造 WM_COMMAND。

## 受控结果

证据位于 `build/verification/abi-menu-investigation-20261020/`。

| 运行 | 结果 | 证据 |
| --- | --- | --- |
| 独立原生 Win32 菜单，原字符选择路径及输入变体 | 隐藏桌面可正常返回首项 | `probe-01/` |
| 原测试 include，先执行 LayoutService、OverflowValidation 及完整 Overflow | 505 项通过 | `runtime-08/layout-all.log` |
| 加上 TreeLayout 前缀的同组测试 | 517 项通过 | `runtime-08/tree-layout-all.log` |
| 现成完整配置 exe，全套且无输入干预 | 15,399 项通过 | `contract-control-02/` |
| 现成原始兼容配置 exe，全套且无输入干预 | 15,399 项通过 | `contract-original-03/` |

完整配置二进制 SHA-256 为 `84F8006424019C2D2692F162F047A69241B4D686E254064CC80898844CCF609B`，原始兼容配置为 `F4B5366B3BAE28BB9CA157A1A5205664279A6657DF433CB3A19E90E7A024319A`。两次完整运行均记录 `observeOnly=true`、空 observations 和空 interventions；测试进程正常回收，桌面正常关闭，峰值 USER 63、GDI 28。

默认沙箱中有测试进程未进入主函数或返回 `0xC0000142`，这些启动失败不作为菜单回归证据。改用正常隔离桌面测试权限后，原有测试代码可以运行。未切换用户桌面或改变系统设置。

## 尚未解释的结果

`runtime-07/single.log` 仍保留冷单项的同类偶发超时。通过结果证明该二进制和输入路径可以完成，不能证明超时已修复；现有证据也不足以将原因归于隐藏桌面、窗口激活或 DPI。此前失败和本轮通过分别保留，不采用反复重试至通过作为验收办法。

后续需要在确实出现超时的一轮中捕获原生菜单的执行状态，或用同一受控输入、二进制和系统状态建立失败对照。该项继续作为间歇测试缺口跟踪，不扩大为全部插件菜单功能失败。
