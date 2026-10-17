# 视口与编辑控件记录

本页按功能归并实现、诊断与验收资料。各记录中的「当前」「已完成」只适用于其注明的源码、构建和测试条件；当前使用入口见[文档导航](../../README.md)。

## 记录索引
- [普通窗口缩放与 Alt+Enter 全屏修复](#record-fullscreen-scaling)
- [操作柄四角停靠](#record-viewport-handle-corners)
- [视口光标图标统一](#record-viewport-cursor-style)
- [配件坐标系角标修复](#record-accessory-hud)
- [帮助菜单白块与独立窗口置顶](#record-help-menu-paint)
- [帮助菜单首次绘制与操作柄布局修复](#record-help-popup-layout)
- [模型编辑菜单与描边对话框](#record-model-edit-dialog-fixes)
- [表情下拉列表宽度](#record-morph-dropdown-width)
- [表情曲线按钮滚轮切换验收](#record-morph-launcher-wheel)
- [缩放标签重绘修复](#record-scale-label-repaint)
- [骨骼缩放读数](#record-bone-scale-readout)
- [相机视角与自阴影参数回显](#record-timeline-parameter-readouts)

<a id="record-fullscreen-scaling"></a>
## 普通窗口缩放与 Alt+Enter 全屏修复

原始记录：`FULLSCREEN_SCALING_20261010.md`。其中的版本、日期和验证范围沿用原记录。

日期：2026-10-10。正式入口：`D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe`。

<a id="record-fullscreen-scaling-同日补充普通界面比例仍异常"></a>
### 同日补充：普通界面比例仍异常

用户提供同尺寸窗口的正常/异常截图，确认运行的是上述正式入口。异常图中的按钮、时间轴行高和底部面板约为正常图的 1/1.75；此前“缩放已恢复”的结论不成立。上一轮后台检查只验证全屏往返前后尺寸相同，未验证尺寸是否符合正常界面。

正式 EXE 提取出的清单没有 DPI 声明。现为 x86/x64 清单显式写入 `dpiAware=false` 和 `dpiAwareness=unaware`，由 Windows 放大整套固定 96 DPI 界面。主入口在读取屏幕尺寸、创建窗口之前将界面线程设为 `DPI_AWARENESS_CONTEXT_UNAWARE`，处理启动器或兼容性设置覆盖进程默认值的情况。动态获取接口，保留旧系统通过清单启动的路径。全屏实现仍使用此前的无边框窗口。

这项改动依据 [Microsoft 的进程 DPI 设置说明](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process) 和 [清单 dpiAwareness 定义](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests)。截图表明系统缩放缺失；本次未捕获造成原进程进入高 DPI 模式的具体调用栈。

异常启动后，配置中的侧栏宽度再次保存为 461，而窗口尺寸仍为 1463×880。备份后仅将这项宽度恢复为 263，其他设置保留。原 EXE、配置、提取清单及构建记录保存于 `.local/artifacts/ui-dpi-20261010/`。本轮不新增或运行测试；完成编译后仍需用户确认可见界面与参考图相符。

<a id="record-fullscreen-scaling-原因和修复"></a>
### 原因和修复

- 本机正式 EXE 的 Windows 兼容性设置含 `HIGHDPIAWARE`，使固定尺寸控件失去 Windows 的缩放。系统 DPI 为 168（175%）。已备份并移除该 EXE 的这一设置，将保存的窗口尺寸 2560×1540、侧栏宽度 461 换回逻辑尺寸 1463×880、263。渲染输出分辨率保持 1920×1080。
- 原 Alt+Enter 分支放在第二个消息分发区，但 `WM_SYSKEYDOWN`（0x104）已被第一个分发区处理并返回。现将快捷键放到可到达的位置，同时转发获得焦点的子控件消息，并去除帧轮询中的重复切换。
- 交互全屏使用覆盖当前显示器的无边框窗口，D3D 保持窗口模式，避免把 DPI 逻辑尺寸当作独占显示模式。立体录制仍显式请求原有独占路径。
- 全屏保存窗口样式、位置、菜单、侧栏宽度、呈现参数和可见子控件；退出时恢复。全屏期间跳过普通面板布局，隐藏所有直接子控件，并清黑视口外四边，消除残留按钮。
- 恢复普通窗口原有布局计算，不改变控件的基础尺寸。

本次构建还修正了已有源码中的编译阻塞：模型名称语言菜单分支位置、与透明排序冲突的菜单编号、缺失头文件，以及模型绘制回调的指针类型。名称语言菜单改用 310；绘制顺序优化保留。

<a id="record-fullscreen-scaling-已完成的后台检查"></a>
### 已完成的后台检查

检查程序链接生产核心和资源，窗口保持隐藏，通过 D3D 回读检查黑边，没有桌面截图。

| 情况 | 结果 |
| --- | --- |
| 修复前 MMS 核心，1000×740、96 DPI | 普通窗口初始化成功；Alt+Enter 未进入全屏 |
| 修复后，1000×740、96 DPI | 通过 |
| 修复后，1463×880、96 DPI | 通过 |
| 修复后，1000×740、168 DPI | 通过 |

每个修复后检查包含两轮进入/退出：Alt+Enter 和 Escape 退出、按住快捷键、全屏期间重新布局、完整显示器覆盖、场景比例、黑边像素，以及逐个控件的位置、大小和可见状态恢复。96 DPI 下全屏为 1463×914，168 DPI 下为 2560×1600。

检查范围为当前单显示器和空场景。可见桌面操作、混合 DPI 多显示器、浮动视口、立体录制及性能前后对照尚未验收；本记录不提供 FPS 结论。

<a id="record-fullscreen-scaling-本地产物"></a>
### 本地产物

`.local/artifacts/fullscreen-scale/` 保留修复前后检查程序、原始日志、原正式 EXE、配置和兼容性设置备份。`before-hashes.json` 记录修复前哈希；`final-normal.log`、`final-large.log`、`final-system-dpi.log` 是最终结果。正式产物来源及哈希记录于 `release-result.json`。

修复前检查基于 `87e4fcc` 加必要编译修正和检查入口；修复后在已提交的绘制顺序优化 `5987be8` 上构建。空场景检查不涉及模型绘制性能，两个版本不能作为性能对照。

构建命令：`cmake --build build --config Release --target fullscreen_window_tests MikuDanceStudio --parallel 6 -- /nodeReuse:false`。检查程序在复制的独立运行目录中分别接收 `1000 740`、`1463 880`、`1000 740 system-dpi`。

<a id="record-viewport-handle-corners"></a>
## 操作柄四角停靠

原始记录：`VIEWPORT_HANDLE_CORNERS_20260927.md`。其中的版本、日期和验证范围沿用原记录。

在「帮助」菜单的田字形选择器中点击对应格子，即可将位移、旋转、缩放与坐标系操作柄移到视窗四角。默认右下角。键盘选中该菜单项后按回车，按左上、右上、左下、右下循环切换。

绘制、点击命中与拖动结束后的光标归位共用同一布局。左侧停靠时，等比缩放放在 XYZ 右侧；上方停靠时，坐标系设置放在缩放行下方。左上角同时应用两条规则，XYZ 轴向顺序保持不变。选择立即生效，保存在 `data/viewport.ini`，不写入 PMM、VMM 或 VEM。

<a id="record-viewport-handle-corners-实现与验证"></a>
### 实现与验证

- `viewport_tool_layout.hpp` 定义四角锚点；`frame_modes.cpp`、`sprite_overlay.cpp`、`overlay_producers.cpp` 使用同一映射。
- 选择器为帮助菜单内的直接绘制项，不增加二级菜单。菜单鼠标处理使用当前线程的 `WH_MSGFILTER`，退出菜单后卸载。绘制期间不再子类化系统菜单窗口，避免干扰首次绘制。
- Release 的 `viewport_corner_layout_tests` 通过四角绘制顶点回读、命中操作号、坐标变换和本机设置读写检查；测试不显示窗口，也不改真实用户设置。
- 初版测试因遗漏 `g_Block` 崩溃；独立对照确认补齐测试应用上下文后通过，生产逻辑未因此放宽。证据保留于 `.local/artifacts/viewport-control-corner-20260927/`。
- 本轮未进行前台菜单鼠标操作验收；后台菜单语言与布局检查均通过。

2026-09-28 的首次绘制、布局和默认设置验证见 [帮助菜单与操作柄修复记录](controls.md#record-help-popup-layout)。

<a id="record-viewport-cursor-style"></a>
## 视口光标图标统一

原始记录：`VIEWPORT_CURSOR_STYLE_2026-09-27.md`。其中的版本、日期和验证范围沿用原记录。

旋转环与移动箭头使用与 XYZ 按钮一致的实色、黑色细描边和浅色外缘。移动光标不再绘制白色或红色手形，原点改为方块。三角箭头内部没有高光、阴影或渐变。

旋转环使用 70% 不透明度（179/255），包括绿色普通状态与黄色高亮状态。旋转横轴保持蓝色、竖轴保持红色，使用浓色、不透明填充。移动向上仍为绿色，向右仍为红色；原有黄色与中心红色高亮保留。

<a id="record-viewport-cursor-style-实现"></a>
### 实现

`src/render/viewport_cursor_style.cpp` 直接绘制图集右半的八个光标单元，不从旧图像推断轮廓或继承色块。8×8 超采样按旋转 90×90、移动 75×75 的显示尺寸计算覆盖范围，颜色按 alpha 加权，圆环边界按径向距离计算。单元位置、UV、屏幕原点与输入判断未改。

`src/window/ui_init.cpp` 在图集加载后调用一次重绘。`src/render/frame_scene.cpp` 在精灵绘制期间启用线性缩小、线性放大与无 mip 采样，然后恢复调用方的采样状态。源 PNG、轴网格和图集左半保持原样。纹理仍为 managed，设备 reset 保留内容。

<a id="record-viewport-cursor-style-验证"></a>
### 验证

后台隐藏 D3D9 HAL 设备实际绘制默认与高亮状态，浅、深背景一起回读。最终预览为 `.local/artifacts/viewport-icon-style/after/flat70-final-preview.png`。该文件名用于区分之前包含旧图残留的预览。

已通过的检查：

- 图集左半逐像素差异为 0；行尾 padding 与无效尺寸输入保持原值。
- 圆环内部 alpha 为 179，红色旋转轴内部 alpha 为 255。
- 四个移动状态均无旧手形位置残留；上下、左右三角形内部全部为单一纯色。
- 即使调用方留有点采样，精灵使用线性采样，绘制后恢复原采样状态。
- 原有坐标轴与骨骼覆盖层回归在修改前通过，共 2 项。

正式程序的整包构建仍未通过：第一轮遇到同一工作区其他改动中的 `mdl::BoneKeyCapacity` 接口错误，日志在 `after/build.log`。相关依赖补齐后再次构建，遇到 `src/window/alembic_export_dialog.cpp:148` 附近的字符串语法错误，日志在 `after/build-final.log`。没有改写这些并行任务的代码，也没有把旧 EXE 当作新版交付。因此整包集成与真实 PMM/MME 场景验收标为未完成。

图标模块独立验证使用修改前生产静态库，并在修改后程序中编译本次 `viewport_cursor_style.cpp` 与 `frame_scene.cpp`，通过真实 `DrawFrameOverlayBatches` 绘制。两侧使用同一 `probe.cpp`、相同嵌入图集、MSVC x64 Release `/O2 /Ob2 /DNDEBUG /MD`、560×440 渲染目标、18 个四边形、30 次预热与 600 次采样。没有显示窗口或采集桌面。

| 交替顺序 | CPU 提交毫秒/次 | 等待 GPU 完成的毫秒/次 |
| --- | ---: | ---: |
| 修改前 A1 | 0.000820 | 0.075897 |
| 修改后 B1 | 0.000775 | 0.073988 |
| 修改后 B2 | 0.000770 | 0.061547 |
| 修改前 A2 | 0.000821 | 0.051304 |

数据来自 `before/abba1.log`、`after/abba2.log`、`after/abba3.log`、`before/abba4.log`。GPU 完成时间有明显波动，本次不据此宣称性能提升。这是覆盖层提交与 GPU 排空测量，不是播放 FPS、独立 GPU 时间或整场景性能。初始化日志的 `init_ms` 还包含 CPU 检查，不能当作生产初始化耗时。

<a id="record-viewport-cursor-style-本地产物与版本"></a>
### 本地产物与版本

产物根目录为 `.local/artifacts/viewport-icon-style/`，不入库，保留用于本次复现。

- `before/` 保留修改前生产 EXE、生产静态库、测试 EXE、CMake 缓存、Git 状态和构建日志。初始提交为 `541ab70f8997036a21befd5760d5911a35253577`，当时包含 `src/mmv/editor.cpp` 与 `tests/mmv_editor_tests.cpp` 的已有改动。
- `after/` 保存当前模块独立验证程序、图集二进制回读、PNG、整包失败日志与工作树状态。
- `probe.cpp`、`compile-before.cmd`、`compile-after.cmd` 与两侧 `compile.rsp` 用于复现；`compare.py` 与 `check-arrow-fills.py` 校验图集区域与纯色三角内部。
- `probe-hashes.json`、`before/executable-hashes.json` 与 `after/hashes.json` 记录验证程序、修改前主程序和源码哈希。最终源码与提交号见 `final-manifest.json`。
- DSH 子任务为 `6eece9c2-5356-4360-bfc4-6f070771e8b6`，实际 cwd 为 `D:/MMD-Next/MikuDanceStudio-MME`。正式结果保存在 `dsh-result.json`。主任务复核后将初稿的旧图分类算法替换为直接几何绘制，并完成后续抗锯齿与透明度调整。

<a id="record-accessory-hud"></a>
## 配件坐标系角标修复

原始记录：`ACCESSORY_HUD_2026-09-26.md`。其中的版本、日期和验证范围沿用原记录。

配件状态的六个旋转、移动按钮现在显示完整的 `.x`，周围保留透明背景。图集改动仅限这六个按钮，相机状态、操作命中区域及运行时绘制代码未变。

原生成器从带相机底板的按钮开始擦除，在原底板或透明像素内才绘制角标，导致六个角标分别缺少 14、15、2、9、8、13 个像素。底板掩码也不能还原它遮挡的按钮图案。修复改为复制图集中现有的无角标旋转环、移动箭头，将完整 `.x` 放在左下角；灰色边缘位于笔画内部，不添加背景或投影。预览改用 alpha 合成。

<a id="record-accessory-hud-构建与版本"></a>
### 构建与版本

工作目录为 `D:/MMD-Next/MikuDanceStudio-MME`。修复前提交为 `83d833e0a4cbdde96b8600ba13e3bd2795c75f38`，工作树为空。修复后的构建以 `cf09d3112a7c5054d70f1e3b1753fbfec41f44e1` 加本次四个生成器及资源文件的修改完成；期间另一个任务提交了独立改动，本次没有纳入其未提交文件。

构建命令为 `cmake --build build --config Release --target MikuDanceStudio coordinate_axis_tests --parallel 4`。两侧退出码均为 0，现有 `coordinate_axis_tests.exe` 均通过。正式入口仍为 `build/Release/MikuMikuDance.exe`。

| 文件 | 修复前 SHA-256 | 修复后 SHA-256 |
| --- | --- | --- |
| 应用 EXE | `e8431fcef264b8083a89beb206b96274fc2c5f677da530eb342c32c1ac24119a` | `f93867523ccc996de9619446895679cce8ed6c76609720a8f4f812a59849f53b` |
| HUD 探针 EXE | `c5074b711b30e482d9a83ff753c32358a64c266b61609b4150a678d5d52b3c78` | `dc8c35208471e4fd8beb83006779db8339b40817b190c28cc87339d056049835` |

<a id="record-accessory-hud-验收结果"></a>
### 验收结果

- 六个角标各有 37 个完整笔画像素，缺失数均为 0；点与 x 之间的间隙 alpha 为 0，没有覆盖 X、Y、Z 彩色字母。
- 图集只改变六个配件按钮，共 1899 个像素；重新生成结果相同。两份 `.res` 仅 PNG 102 记录改变，嵌入内容与图集文件逐字节一致。
- 使用隐藏窗口和真实 D3D9 设备，调用生产 `PrepareFrameSpriteOverlay`、`DrawFrameOverlayBatches`，从渲染目标回读。深浅两种背景、1／1.5／2 三档缩放下，角标完整且背景透明；相机状态的六组前后画面逐像素一致。
- 两份独立探针分别链接修复前后资源，链接的生产库相同。修复前快照及最终构建的 `mikudancestudio_core.lib` SHA-256 均为 `ecd27e53aecb47d00e46c0b9078da1ad816b51705f289da400f20124ecee0d9b`。

性能对照交替运行三轮，每轮预热 100 次、采样 2000 次，在相同 512×320 目标、2 倍 HUD、配件状态下绘制两个按钮行，并等待 GPU 完成。

| 等待 GPU 完成的局部 HUD 平均耗时 | 修复前 | 修复后 |
| --- | ---: | ---: |
| 三轮均值，微秒／次 | 348.939 | 346.074 |
| 三轮范围，微秒／次 | 333.557～369.261 | 338.417～360.673 |

均值差异位于采样波动范围内。这是无 Present 的局部绘制吞吐测量，包含 CPU 提交与等待，不是 GPU 独立时间或播放 FPS。本次没有对完整模型场景、物理或第三方 MME 效果重新做性能验收。

<a id="record-accessory-hud-本地证据"></a>
### 本地证据

`.local/artifacts/accessory-mark-20260926/` 保存修复前后应用、资源、生产库快照、探针源码、编译响应文件、构建日志、三轮原始 JSON 和 GPU 回读 PNG。`validation.json` 记录像素检查、资源检查、采样及哈希；这些临时产物不入库，不自动删除。

按项目约定尝试的 DSH 子任务 `a722ed7f-c34e-4e47-a4f1-1bc6993bb829` 的实际 cwd 为上述工作目录，开始执行前因 `QUOTA`／`429 quota exhausted` 结束，正式结果无输出、未修改文件。修复及验收由当前 Codex 会话完成，没有重启 DSH。

<a id="record-help-menu-paint"></a>
## 帮助菜单白块与独立窗口置顶

原始记录：`HELP_MENU_PAINT_20260928.md`。其中的版本、日期和验证范围沿用原记录。

<a id="record-help-menu-paint-白块原因"></a>
### 白块原因

帮助菜单的「操作柄位置」使用 owner-draw 绘制田字格。原实现修改菜单共用 HDC 的文字颜色与背景模式，却没有在返回前恢复。默认选择右下角时，最后绘制的格子使用白色高亮文字，因此后续菜单项可能以白字绘制到白底上。

这与菜单项目未加载不同：项目仍然存在，但绘制状态被前一项影响。修复保留田字格的直接选择方式，通过 `SaveDC` / `RestoreDC` 在该项绘制结束时恢复进入时的 HDC 状态；保存失败时不修改 DC。

<a id="record-help-menu-paint-前后验证方式"></a>
### 前后验证方式

主端探针调用实际 `viewport_corner_menu::HandleDrawItem`，使用内存 DC 和 DIB，不打开可见菜单、不读写用户设置。每轮传入文字颜色 `RGB(17,34,51)`、背景模式 `OPAQUE`，依次检查四种操作柄位置；绘制后再用同一 HDC 绘制相邻菜单文字，并回读其可见像素。

旧实现来自 `3837b7d`。四种位置均未恢复背景模式及文字颜色；默认右下角还使后续文字可见像素数变成 0。修复前测试返回 1，原始记录保留在 `.local/artifacts/help-menu-white-20260928/dc-before.log`。

同一探针在修复后四种位置全部通过，均恢复原文字颜色和 `OPAQUE` 背景模式，后续文字均有 488 个可见像素。前后编译使用同一 MSVC x64、C++17、`/O2 /MD` 条件。源码、原始构建日志、测试日志及可执行文件均保留在本次诊断目录。

正式回归 `menu_draw_state_tests` 进一步覆盖四种位置和四种菜单项状态，并交替使用透明、不透明文字背景。它同时验证字体、文字对齐、背景颜色和裁剪区域没有被污染，再回读后续文字。使用旧生产文件时 16 组全部失败；修复后 16 组全部通过，后续文字均有 264 个可见像素。两种探针选用的字体不同，因此像素数量不同，不作为相互比较的画质指标。

通过 CMake 构建并执行的三个回归为 `menu_draw_state_tests`、`menu_language_regressions`、`viewport_corner_layout_tests`，结果 3/3 通过，见 `ctest.log`。本轮验证使用内存绘制与后台测试，没有前台打开真实下拉菜单做人工验收。

<a id="record-help-menu-paint-独立窗口置顶的范围"></a>
### 独立窗口置顶的范围

命令 281 只对「显示 → 独立窗口」打开的视窗调用 `SetWindowPos(HWND_TOPMOST/HWND_NOTOPMOST)`，不会把主程序窗口置顶。使用嵌入主窗口的视窗时，切换这个选项没有可见变化。

这是原有功能，本轮菜单整理把它移到了帮助菜单。中文名称改为「独立窗口置顶」，英文改为 `keep separate window on top`，使名称与实际作用一致。置顶行为、独立窗口开关及原有用户设置保存方式均未改动。

两套菜单资源的三语名称、动态菜单插入和子菜单句柄稳定性检查通过；资源生成后的 48 个对话框和 2 个菜单验证通过。记录见本次诊断目录的 `label-test.log` 与 `resources-verify.log`。

<a id="record-help-menu-paint-编译版本"></a>
### 编译版本

VS 2022 x64 Release 主程序已编译到 `.local/releases/mms-help-menu-20260928/MikuMikuDance.exe`。原 `build/Release/MikuMikuDance.exe` 正在运行，本轮没有中止或覆盖它。

主程序 SHA-256：`9CBC1DF2A595F4E92F80F5274D1D691CF32E44D1DF3E9F2EDC87C2E6AAC9939F`。构建日志为 `.local/artifacts/help-menu-white-20260928/main-build.log`。新目录的运行 DLL 保留原 `build/Release` 安装组合，不代表重新构建或替换了 MMV 后端；文件哈希与源码提交记录在新目录的 `build-manifest.json` 中。

<a id="record-help-popup-layout"></a>
## 帮助菜单首次绘制与操作柄布局修复

原始记录：`HELP_POPUP_LAYOUT_20260928.md`。其中的版本、日期和验证范围沿用原记录。

2026-09-28，正式 `main` 已完成三项修复，代码提交为 `9d77ed5`、`1c72c1e`、`9a2e28c`。本轮未使用 DSH。

<a id="record-help-popup-layout-行为变化"></a>
### 行为变化

帮助菜单的四角选择器不再在 `WM_DRAWITEM` 中枚举并子类化系统菜单窗口。鼠标点选改用当前线程的 `WH_MSGFILTER`，核对进程、线程及帮助菜单句柄，退出菜单时卸载。绘制回调保留 DC 状态恢复。基线回归确认每次首次绘制都会改写窗口过程，候选不会。

[Windows 菜单消息过滤说明](https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/Controls/cc-faq-iemenubar.yml)说明了菜单循环中的 `WH_MSGFILTER` 与 `MSGF_MENU`。采用该接口使系统继续负责弹出菜单的完整绘制过程。

透明排序默认关闭，三语菜单将“默认”标记移到关闭项。新建工程、没有有效档位字段的 PMM 均使用关闭；已有工程明确保存的档位继续生效。四个启用档位的算法和强度不变。

左侧停靠时等比缩放放在 XYZ 右侧；上方停靠时坐标系设置放在缩放行下方。左上角同时应用两条规则。绘制、灰色禁用状态、悬停、点击与拖动结束后的光标归位使用相同位置，XYZ 轴向含义不变。

<a id="record-help-popup-layout-构建与证据"></a>
### 构建与证据

构建使用 VS 2022 x64 Release。基线提交 `9b52d35ac461580d9782eb6dae9cad1582605a04`，候选代码提交 `9a2e28c263f96b8f09dde31dc6a58bccfe094ca4`。开始时的其他任务改动列于 `before-state.json`，本轮没有提交那些文件。

- 基线 EXE SHA-256：`DF4ED29E067F3C14296B09E0985121D46D3C68C5CE5ABDC93DE0821348FF36D3`。
- 候选 EXE SHA-256：`0DA6E941FDBA1C390139C2F2AC384E3E675E418B8CEE743CB076C6927F1BCA1D`。
- 基线菜单检查：三次首次绘制均改变窗口过程。
- 基线布局检查：新位置预期有 54 项失败。默认关闭的菜单检查有 4 项失败。
- 候选：后台原生菜单连续打开四次，首次绘制无需鼠标移动；两条普通菜单文字每次均有有效像素。四格点击全部通过，绘制期间窗口过程没有变化。共享 DC 的 16 项状态与文字检查全部通过。
- 候选：四角、1/1.5/2 倍 HUD、非零视口原点、缩放纹理列、启用及禁用命中、坐标文字顶点位置全部通过。
- 候选：菜单、操作柄与菜单语言 CTest 三项通过；PMM 默认值、五档保存重开、未知字段、旧工程及损坏扩展回归通过。`frame_benchmark` 已按新默认值更新并编译。

测试没有显示到用户桌面、移动真实鼠标或写入真实用户设置。原生菜单使用独立的非活动桌面，`WM_PRINT` 绘入内存 DIB；四角点选写入测试专用临时目录。`help-initial.png` 是该绘图目标的回读，不是桌面截图。未进行前台交互或桌面合成动画验收，也未测量播放 FPS、CPU/GPU 帧耗时；本轮不作性能数值结论。

新增点选测试最初使用 `TPM_NONOTIFY`，因此屏蔽了菜单初始化通知，过滤器没有启动。去除该测试标志后，真实的初始化与退出通知链、四格点击均通过。失败日志保留，未把它当作有效生产失败样本。

首次候选构建尝试统一覆盖 `OutDir`，但 CMake 生成的链接依赖仍指向常规静态库位置，导致新符号链接失败。最终先更新 `build/lib/Release` 的生产库，再仅为 EXE 指定输出目录并禁用重复项目依赖构建。失败尝试保留于 `after-build.log`，交付以 `final-core-build.log`、`final-main-build.log` 为准。

所有日志、基线程序、SHA-256 清单与绘图回读保存在 `.local/artifacts/help-popup-layout-20260928/`。完整可运行目录为 `.local/releases/mms-help-layout-20260928/`。MMV、MME 和 Bullet3 运行文件从上一个完整构建原样复制，来源及哈希见 `build-manifest.json`。正在运行的旧程序未覆盖或关闭。

<a id="record-help-popup-layout-同步到日常运行目录"></a>
### 同步到日常运行目录

2026-09-28 按用户要求，将上述已验证运行包同步到 `main` 的 `build/Release/`。替换 EXE、MMV.dll、接口头文件和 Bullet3 FX，补齐 FX 同目录的 MMVBullet3.dll，以及 `editions/` 中的可选 MMV 版本。包内 34 个文件与目标相同，无需覆盖；现有 `MMEffect.ini`、`data/` 和工程文件保留。

同步前没有目标程序进程，所有待覆盖文件均通过独占打开检查。旧文件备份到 `.local/archives/help-layout-deploy-20260928-085857/`。覆盖后核对了每个写入文件及全部包内二进制的 SHA-256，均与已验证运行包一致。程序未自动启动；本次只同步相同构建产物，没有重新编译或重复渲染测试。明细见 `.local/artifacts/help-popup-layout-20260928/deployment.json`。

<a id="record-model-edit-dialog-fixes"></a>
## 模型编辑菜单与描边对话框

原始记录：`MODEL_EDIT_DIALOG_FIXES_2026-10-01.md`。其中的版本、日期和验证范围沿用原记录。

“模型扩展编辑”原先只在部分加载与组合框切换路径更新启用状态。程序切换模式或恢复选中模型后，父菜单可能继续保持旧的灰色状态。现在 `WM_INITMENUPOPUP` 按当前模式和所选模型同步该菜单：模型模式且存在所选模型时启用，相机模式或没有所选模型时禁用。

描边粗细窗口的编辑框回调安装使用了 `(LONG)(LONG_PTR)`，将 64 位函数地址截断为 32 位。Windows 在对话框初始化时调用这个截断地址，产生执行访问冲突。现在传入完整的 `LONG_PTR`，保留原输入框与滑块行为。

<a id="record-model-edit-dialog-fixes-隐藏窗口回归"></a>
### 隐藏窗口回归

- `menu_language_tests` 加载正式 `SAMPLE02` 与 `SAMPLE02E` 菜单资源，调用真实 `MainWndProc` 的菜单初始化消息。三种语言下验证过期的灰色状态能够恢复，也验证相机模式和空模型时禁用。修复前共 18 项状态检查失败，修复后全部通过；既有语言、菜单插入和句柄稳定性回归一并通过。
- `ui_message_tests edge` 从真实菜单命令打开描边对话框，通过 CBT 与窗口子类钩子保持隐藏。修复前复现 `0xC0000005` 执行访问冲突；修复后在中英日三种语言下各打开两次，验证初始值、Enter 提交、滑块更新和关闭重开均通过。此模式的异常处理将访问冲突记为失败并退出，不显示系统错误窗口。

前后均使用 `Release`。正式程序为 `build/Release/MikuMikuDance.exe`。测试链接生产源码，未使用桌面截图，未测量 GPU 时间或播放性能。

基线为 `167f503`。前后源码、Git 状态、构建日志、测试 EXE、哈希及原始结果分别保留在 `.local/artifacts/model-extension-menu-20261001/` 和 `.local/artifacts/edge-thickness-crash-20261001/`，不自动清理。其他会话的未提交内容未纳入本次修复。

<a id="record-morph-dropdown-width"></a>
## 表情下拉列表宽度

原始记录：`MORPH_DROPDOWN_WIDTH.md`。其中的版本、日期和验证范围沿用原记录。

眼、眉、嘴和其他四组表情下拉列表默认宽度调整为 240 像素。展开时按当前控件字体测量完整名称，预留边距及滚动条宽度，并限制到当前显示器工作区宽度。

测量在每次展开时执行，因此模型切换、语言切换和字体变化后的名称也会参与计算。

四组表情名称行的前后箭头、曲线入口和下拉框使用相同的上边缘与高度。布局读取原生下拉框的实际收起高度，因此字体或主题改变时，相邻按钮也会同步调整。各列的水平位置保持原有布局。

Release 构建成功。未执行界面运行验证或性能对照。

- 构建日志与版本记录：`.local/artifacts/morph-dropdown-width/20261003/`。
- 独立可运行版本：`.local/releases/morph-dropdown-width/`，包括 EXE、MMV.dll、Data 与 licenses。
- 当前运行程序未替换或重启。

名称行对齐的后续构建记录位于 `.local/artifacts/morph-row-alignment/20261003/`，对应独立可运行版本位于 `.local/releases/morph-row-alignment/`。该版本包含上述列表宽度调整。Release 构建与补丁格式检查通过，尚未运行界面验证。

<a id="record-morph-dropdown-width-首次绘制覆盖"></a>
### 首次绘制覆盖

名称框的实际高度可能超过原布局预留的 20 像素，与下方滑块窗口相交。滑块会覆盖名称框下边缘，鼠标悬停触发名称框重绘后才恢复其外观。

滑块现在从名称框实际下边缘后 2 像素开始，底部限制在该组原有空间内。名称行各控件在布局后失效重绘，避免保留旧边框。列表展开宽度继续自适应。

后续产物直接写入 `build/Release/MikuMikuDance.exe`，不再以 `.local/releases/` 作为交付位置。构建日志位于 `.local/artifacts/morph-row-overlap/20261003/`。Release 构建及补丁格式检查通过，尚未运行界面验证。

<a id="record-morph-launcher-wheel"></a>
## 表情曲线按钮滚轮切换验收

原始记录：`MORPH_LAUNCHER_WHEEL_20261007.md`。其中的版本、日期和验证范围沿用原记录。

鼠标位于表情操作栏的曲线按钮时，向下滚动选择该栏的下一个表情，向上滚动选择上一个表情。四栏均使用原生下拉框处理及 `CBN_SELCHANGE` 通知，列表边界、其他栏的选择和键盘焦点保持原有行为。

父窗口根据滚轮消息的屏幕坐标判断命中的按钮，转发到同栏下拉框并消费消息。禁用按钮、禁用选择器、空列表和零增量不改变选择。Ctrl、Shift 滚轮不转发，因为原生下拉框会把这类消息退回父窗口，直接转发会递归。

<a id="record-morph-launcher-wheel-后台对照"></a>
### 后台对照

测试命令为 `morph_curve_host_ui_tests.exe --morph-wheel`。两侧使用相同测试源码、四个隐藏按钮、每栏三项表情及相同输入序列；没有显示窗口或移动系统鼠标。

| 项目 | 修复前 | 修复后 |
| --- | --- | --- |
| 基础提交 | `1c20f04008cf3ffff8451a5ace19965884dce139` | `1c20f04008cf3ffff8451a5ace19965884dce139` 加本项补丁 |
| 断言数 | 57 | 57 |
| 失败数 | 42 | 0 |
| 测试 EXE SHA256 | `e1f5e99b0eea6a892b0ee89d1066496fcdcdc91f04831b72f1ca45efee1c6058` | `b73889d93ef973f3360b13cc018c6023166c76c372ea063d4db79259611e66bc` |

失败断言包含前序选择失败产生的后续断言，不表示 42 个独立缺陷。验证覆盖按钮收到滚轮和父窗口收到滚轮两条路径、四栏正反方向、首尾边界、禁用控件、零增量、Ctrl/Shift、原生通知次数、其他表情栏、焦点与区域外原有行为。

构建目录为 `D:\MMD-Next\MMS\build`，配置为 Release/x64。`.local/artifacts/morph-launcher-wheel/before-final/` 和 `after-final/` 分别保留构建命令、Git 状态、生产源码与测试文件哈希、EXE 哈希、补丁和原始输出；较早的 `before/`、`after/` 为开发阶段记录。`.local/` 不入库。

<a id="record-morph-launcher-wheel-验收范围"></a>
### 验收范围

本次只验证控件输入，不运行 GPU、播放或用户工程。播放 FPS、CPU 提交时间和 GPU 时间未测，性能对照未验收。

MMS 正式入口为 `D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe`。本项提交后构建宿主，最终来源与哈希写入 `.local/artifacts/morph-launcher-wheel/release.json`。

<a id="record-scale-label-repaint"></a>
## 缩放标签重绘修复

原始记录：`SCALE_LABEL_REPAINT.md`。其中的版本、日期和验证范围沿用原记录。

2026-10-02。

缩放静态标签采用透明背景和空画刷。刷新或切换字体时，旧字形未清除，形成重叠残影。现在每次绘制前恢复对应位置的工具栏灰色渐变，并在布局前设置标签的最终字体。

后台验证输出位于 `.local/artifacts/scale-label/20261002/`，不纳入版本控制。目录保留修复前提交和工作树记录、两侧构建日志、EXE 哈希及测试日志。

同条件用例在内存位图中先绘制英文标签，再绘制中文标签，与单次中文绘制进行逐像素对照。修复前失败，修复后通过。测试窗口保持隐藏。Release 宿主及 `animation_curve_host_tests` 构建完成，已有 MSB8028 中间目录警告仍存在。未进行渲染性能测量，也未操作用户当前进程。

<a id="record-bone-scale-readout"></a>
## 骨骼缩放读数

原始记录：`BONE_SCALE_READOUT_20260927.md`。其中的版本、日期和验证范围沿用原记录。

主菜单下方的「模型 : 骨骼名」状态条新增 `缩放 X 1.00 Y 1.00 Z 1.00`。数值沿用旋转与缩放操作柄的局部、全局、视图轴向定义，包含骨骼固定轴和局部轴；不直接显示缩放矩阵的对角值。

无选中骨骼时显示 `-----`。相机面板、播放中或状态条剩余宽度不足时隐藏。每帧检查格式化后的读数，仅文字变化时请求重画该状态条，不移动原有按钮。

<a id="record-bone-scale-readout-验证与限制"></a>
### 验证与限制

Release `bone_scale_hud_tests` 通过 18 次隐藏 GPU 回读，以及单位缩放、非均匀缩放、旋转骨骼基、局部轴、空选中和隐藏状态检查。内存 DC 像素对照确认数值和坐标模式变化会改变实际绘制文字；测试进程的导入探针确认变化时向正确窗口请求一次状态条重绘、未变化时不重绘。

初版测试用隐藏窗口更新区判断刷新，失败后改用实际 `InvalidateRect` 调用记录。Windows 对从未显示的测试窗口不保留可供该断言读取的更新区；失败日志与独立诊断保留于 `.local/artifacts/bone-scale-info-20260927/`。统一测试日志为 `.local/artifacts/global-processor/scale-hud-fixed.log`。

本轮没有进行真实可见窗口的人工排版验收或视图轴相机场景手动核对。未改 PMM/VMM/VEM，也未改用户配置。

<a id="record-timeline-parameter-readouts"></a>
## 相机视角与自阴影参数回显

原始记录：`TIMELINE_PARAMETER_READOUTS_20261007.md`。其中的版本、日期和验证范围沿用原记录。

换帧后，相机视角和自阴影参数从已经计算的实时值同步到输入框、滑块和阴影模式按钮。相机视角输入框保留过渡帧的小数值，滑块取整不会回写实时参数或关键帧。

`ReloadModels` 在原生相机插值与曲线覆盖之后刷新视角。`RefreshSelfShadowPanel` 在当前帧晚于末关键帧时保持末关键帧，不再退回其前一帧。原生阴影关键帧继续沿用阶梯变化；已编写的连续阴影曲线按实际插值回显。

播放的最终求值步骤直接刷新实时参数，不重新按整数 `CurrentFrame` 求值。几何导出的独立采样入口不新增 UI 操作。只有显示值改变时才写入控件。

<a id="record-timeline-parameter-readouts-后台对照"></a>
### 后台对照

两侧使用 Release/x64、相同隐藏窗口、相同登记数据和同一测试源码。命令为 `runtime_layout_tests.exe --timeline-readouts`，没有显示窗口或执行 GPU 测试。

| 项目 | 修复前 | 修复后 |
| --- | --- | --- |
| 基础提交 | `f693a0f32529c5123abb3fbee86d9a390253ec12` | 同一提交加本项补丁 |
| 定向检查 | 43 项，18 项失败 | 43 项全部通过 |
| 测试 EXE SHA256 | `535f24e63f70a4540cd6a2a27b9a4c7b92a894f16afa859a1ae60c0dd028dc61` | `4671a2473d8b3bbc297422d35ace24964a46a5e732b93fbd98c0c925094b3e4e` |

检查涵盖真实相机和阴影登记、正反向跳转、插值小数视角、末帧以后、连续阴影曲线、播放游标与整数帧不同步时的回显，以及关键帧数据不被改写。修复后同一 EXE 的完整 `runtime_layout_tests` 也通过。

原始构建和测试日志、命令、Git 状态、源码与 EXE 哈希位于 `.local/artifacts/timeline-parameter-readouts/before-final/` 和 `after/`。`before/` 保留测试编译期间遇到 Windows `min` 宏冲突的失败记录，未计入行为对照。构建目录为 `D:\MMD-Next\MMS\build`，这些本地记录不入库。

未在用户工程中手动验收。播放 FPS、CPU 提交时间和 GPU 时间未测，性能对照未验收。

正式入口为 `D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe`。提交后的宿主构建来源和 EXE 哈希写入 `.local/artifacts/timeline-parameter-readouts/release.json`。
