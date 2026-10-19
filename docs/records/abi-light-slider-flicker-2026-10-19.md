# 照明参数滑杆闪动诊断

日期：2026-10-19。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本记录补充[上一批焦点提示与绘制诊断](abi-small-viewport-morph-reset-2026-10-19.md)，保留本轮失败和无法判定的结果。

实现与诊断提交：`9e25758`。

## 用户确认的范围

用户确认相机、阴影滑杆的闪动消失，照明滑杆仍闪动，并补充六条照明滑杆似乎都会出现。相机和阴影记为本次人工复验通过；照明仍未通过。不能将此前整圈焦点框修复扩大为所有参数滑杆消闪完成。

本轮检查实际照明控件 `455..460`，分别对应 RGB 和方向 XYZ。原生鼠标消息、原生绘制输出、桌面呈现及业务数据分别记录。

## 原生输入与数据观察

| 运行目录 | 输入观察结果 | 限制 |
| --- | --- | --- |
| `ordinary-first/` | 114 个阶段、401 条固定消息，8.234 秒；原生输入、参数及 VMD 观察通过。 | 后续原生绘制对照因真实按键处于按下状态而中止，不能将整个运行记为通过。 |
| `ordinary-visible/` | 114 个阶段、400 条固定消息，8.203 秒；原生输入、参数及 VMD 观察通过。 | 后续完整绘制对照保留六项失败，进程退出码为 1。 |

上述时间是诊断运行时间，不是性能比较。证据为各目录中的 `light-paint/light-report.txt`、`light-messages.csv`、`light-samples.csv` 和 `report.json`。

观察分别保存窗口 `GetDC` 和桌面同位置的 `GetDC` 图像。两次运行的五点窗口归属检查均为 0，未取得合格的桌面可见性证据。即使成功复制桌面图像，也不能据此声明窗口无遮挡、DWM 已呈现该帧或闪动消失。

证据根目录为 [`build/abi-light-slider-flicker/`](../../build/abi-light-slider-flicker/)。上述输入与表面对照宿主的 SHA256 为 `6bf093fdeb827ccce938b4008ed99f36ebef41b7a815e65efb25e1e7375a7838`；完整参数见 `ordinary-visible/report.json`。后续缓冲专项另列最终宿主哈希。

## 指定 HDC 的原生绘制仍有严格失败

绘制对照使用六个实际 MMS HWND、未改动的样式和几何，以及实际 ANSI 原生窗口过程。参考图来自整块客户区原生重绘后的窗口像素；对照图来自一次 `WM_PAINT(HDC)`。两种不同标记底色用来检查原生绘制是否完整初始化输出，不屏蔽焦点、刻度或其他像素。

| 状态 | 结果 |
| --- | --- |
| 空闲且隐藏焦点提示 | 六个控件全部逐像素一致。 |
| 已聚焦但隐藏焦点提示 | 六个控件全部逐像素一致。 |
| 原生捕获并移动、隐藏焦点提示 | 六个控件全部逐像素一致，实际刻度和原有参数运算同时核对。 |
| 显示焦点提示 | RGB 三个控件一致；XYZ 三个控件分别在两种标记图中各有两个像素不同，共六项比较失败。 |

隐藏焦点提示的 18 组输出为零像素差，但完整结果仍是 `FAILED groups=24 comparisons=120 failed_comparisons=6`。实际 COMCTL32 版本报告为 `6.16.10586`，控件为 ANSI `msctls_trackbar32`。这些结果不能推广为全部主题、尺寸和系统版本通过。

`ordinary-first/light-paint/native-surface/native-surface-report.txt` 保留 `INCOMPLETE groups=0 comparisons=0`，原因为真实按键或鼠标键处于按下状态。`ordinary-visible/light-paint/native-surface/native-surface-report.txt` 保留完整的 24 组和六项失败。

输入均为明确标注的合成按下、移动、松开和导航消息。本项不移动真实光标，不验证硬件输入、完整消息泵或连续桌面呈现。

## 物理矩形交集不足以证明交叉绘制

调用线程实际为 DPI unaware，报告 96 DPI。诊断通过动态 API，在短作用域内切换到经核对的 per-monitor v2 上下文，读取 15 个实际 HWND 的矩形后恢复原上下文；恢复核对通过。

逻辑矩形中，G `456` 和 B `457` 首尾相接。临时 per-monitor 上下文读到的物理 `WindowRect` 有 `238×1` 像素交集，范围为 `[945,1342,1183,1343]`。RGB 逻辑客户区为 `136×18`，方向 XYZ 为 `132×20`。物理窗口外框与客户区高度不同，外框交集本身不能证明两个客户区 DC 都能写入该行。

后续只读 DC 诊断未能校准坐标：G 的实际 `GetDCOrgEx` 为 `(540,749)`，per-monitor `ClientToScreen` 为 `(945,1311)`。两者不一致，物理共享行到原生 DC 的映射未通过资格检查。因此 `light-slider-overlap-owners.csv` 明确记录 `UNAVAILABLE`，不能把其中为零的 `PtVisible` 计数解释为已经正确裁剪，也不能判定共享行的可绘制归属。

证据分别位于 `ordinary-first/light-paint/light-slider-geometry.txt`、`light-slider-intersections.csv`，以及 `clip-geometry/light-paint/light-slider-native-dc.csv`、`light-slider-overlap-owners.csv`、`light-slider-clip-points.csv`。几何采集的 `CAPTURED` 与共享行归属的 `UNAVAILABLE` 是不同结果。

本轮曾提出只给六条照明滑杆增加 `WS_CLIPSIBLINGS`，未获得实际客户区交叉绘制的因果证据后已撤回。`ui_init.cpp` 不保留这项候选；该样式不计为修复。

## 捕获期间的照明局部缓冲

完整配置已接入私有 `src/window/light_slider_paint_buffer.hpp`，接线位于 `TrackbarSubclassProc` 的最终转发。缓冲只处理照明 `455..460`、捕获属于当前 HWND、隐藏焦点提示且客户区与已对照尺寸一致的普通 `WM_PAINT`。

缓冲调用原生 `WM_PAINT(HDC)` 绘制完整客户区，再做一次最终复制。显示键盘焦点提示、外来 HDC、非照明控件、尺寸不符及原始兼容配置保留原生直接绘制。实现没有修改滑杆布局、刻度、鼠标运算或参数作者值，也没有增加插件 ABI。

实现包含资源准备失败后的原生一次回退、复制失败后的后续直接重绘，以及准备和清理期间的重入保护。原生绘制返回后还检查 HWND 生命周期、窗口过程、客户区、捕获和提示状态；销毁标记阻止对旧 HWND 执行原有 `EndPaint` 或重试。

源码一致重编后的普通宿主 `buffer-source-aligned-ordinary/` 先通过，耗时 78 毫秒。最终 `buffer-ordinary-final/` 普通模式耗时 94 毫秒，`buffer-bound-input-diagnostic/` 共同历史绑定模式耗时 78 毫秒，两者退出码均为 0。每种模式的六个真实照明 HWND 均检查完整原生内存图、最终窗口图及复制前窗口未变。

八种 `BeginPaint` 前资源失败、`BeginPaint` 空结果、复制失败、`EndPaint` 失败、重试属性失败，以及原生返回前的捕获丢失和尺寸变化均经过故障注入；拥有的 GDI 资源释放数量逐项匹配。两模式均完整完成这些分支。

显式请求擦除时，`BeginPaint` 触发的实际 `WM_ERASEBKGND` 转到内存 DC，窗口在最终复制前保持不变。每条绘制消息分别核对一次原生调用及返回值；同步回调产生的独立嵌套消息单独计数，不要求整个外层消息期间只能出现一次原生调用。绘制专用范围内的参数、VMD 和历史保持检查同时通过。

早期 `buffer-first/` 的原生调用次数断言失败和 `buffer-dispatch-ordinary/` 的直接回退断言失败仍保留。最终结果以 `buffer-ordinary-final/` 和 `buffer-bound-input-diagnostic/` 为准，没有将早期失败日志改为通过。

共同历史绑定宿主的 `buffer-source-aligned-bound/` 和 `buffer-bound-final/` 分别在 16 毫秒和 63 毫秒检测到真实按键或鼠标键按下后中止，保留 `INCOMPLETE`。这两次没有完成全部分支，不作为生产失败或绑定模式通过证据。后续仅为输入保护诊断文本补上 VK 编号，未修改生产绘制或放宽保护条件；最终绑定模式重新完成全部分支。

最终普通及绑定模式的缓冲、焦点四次专项使用同一宿主，SHA256 为 `9b112da75698eb0894261b99393b7569776735b901fc0b721814773fa9ceaaa5`。各目录的 `report.json`、`result.log` 和 `light-paint/buffer/light-buffer-report.txt` 分别记录二进制、退出码、逐消息调用、像素和资源结果。较早源码一致重编宿主的 SHA256 为 `cbd3f1ed5e562c7af71c552a80ffab124b7823e3b887598385635b9160eef468`，不混作最终四次专项的二进制。真实销毁及 HWND 值复用未在本专项运行。

此前显示焦点提示的严格像素差异完整保留，新缓冲不接管该状态。缓冲专项通过不替代用户视觉复验或连续 DWM 呈现验收。

## 鼠标与键盘焦点提示回归

最终 `focus-ordinary-final/` 和 `focus-bound-final/` 均通过。每种模式检查七个真实控件并保存 21 张 BMP：鼠标入口只改变外缘焦点提示，内部零像素差；三次捕获期间同点移动不使提示回跳，外部焦点转入恢复键盘提示。九个导航键与相同初始状态的原生 UI 树逐位对照，每条消息的原生调用次数、只改变提示时的参数和历史、VMD 字节均保持。

上一批首次键盘检查要求父窗口和兄弟控件的提示位全部不变，因原生键盘处理本身会传播这些提示而失败。最终沿用真实原生对照，不放宽原生一致性。该次失败及早期缓冲原型的严格失败仍在[上一批记录](abi-small-viewport-morph-reset-2026-10-19.md)中保留。该焦点专项只覆盖同点鼠标移动；本轮合成鼠标消息引起的刻度变化另见照明专项。硬件连续拖动和真实 Tab 导航器仍未验收。

## 本轮构建与回归

| 检查 | 本轮结果 | 证据 |
| --- | --- | --- |
| 完整配置 | 同轮 21／21 组通过，95.31 秒。 | `ctest-full-final.log`。 |
| 原始兼容配置 | 同轮 18／18 组通过，15.00 秒。 | `ctest-original-final.log`。 |
| 累计专项 | 共同历史 345,223 项、ABI 2,552 项、DATA 897 项、LAYOUT 647 项通过。 | 本轮实际回归计数，不沿用上一批 LAYOUT 数。 |
| 完整配置产品启动 | 原名及中文改名的隐藏启动、菜单、工具栏和关闭通过。 | `product-smoke-final.log`。 |
| 实际照明缓冲 | 普通与共同历史绑定模式全部分支通过。两次绑定模式真实输入中断仍保留。 | `buffer-ordinary-final/`、`buffer-bound-input-diagnostic/`。 |
| 焦点提示 | 普通与共同历史绑定模式均通过七控件、21 张 BMP、九导航键及外部焦点核对。 | `focus-ordinary-final/`、`focus-bound-final/`。 |

产品 SHA256 分别为：

- 完整配置：`2A87B40BE8DD90D971B0E67F7A4FFF41E3843D70F286DF3ABB1298EDB0CF8254`。
- 原始兼容配置：`025143F312AFD81106783626A5A0479AF9C3D1B3B725A18496FD0164959F827C`。

启动检查使用完整配置产品的原名与中文改名隔离副本。原始兼容配置结果来自该配置的 18 组回归，未将完整配置启动结果算作其启动验收。

## 兼容与未完成验收

本轮未修改 main 或稳定版，也未同步 main 中其他会话尚未提交的内容。原始兼容配置不启用照明缓冲。VMD、PMM、共同历史、原有参数精度及程序根目录 `data` 约定不变。

Win7 SP1 x64、CPU 基础路径和 AMD 兼容要求继续适用。动态 DPI 查询在旧系统缺少 API 时明确记为不可用，不增加新系统 API 的静态依赖。缓冲只使用 Win7 已有的 Win32 和 GDI 调用；这不替代 Win7、AMD 真机验收。

照明六条的用户视觉复验、实际硬件拖动、连续 DWM 呈现、真实销毁及 HWND 值复用仍未验收。不能将合成消息和窗口像素专项扩展为这些项目通过。
