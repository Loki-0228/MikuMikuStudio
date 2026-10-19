# 共用布局重绘、命令溢出和原生历史迁移

本轮工作目录为 `D:/MMD-Next/MMS-abi-dev`，分支为 `codex/plugin-abi`。稳定版目录未参与修改或部署。

## 当前验收状态

用户提供的 `QQ20261019-182558.mp4` 显示，175% 缩放下拖动内部分隔线时，滑杆、部分图标和曲线入口仍会变白。[此前局部滑杆缓冲记录](abi-light-slider-flicker-2026-10-19.md)中的自动检查不能推翻这次人工失败。

本轮改动覆盖共用布局路径。稳定画面检查通过不代表连续桌面呈现无闪烁，人工验收仍未通过。记录中的合成鼠标消息只调用隔离宿主的真实分隔线处理函数，不生成物理输入。

## 共用布局处理

`ui_control_layout.hpp` 的 `LayoutBatch` 收集作用域内所有布局调用，包括原本要求立即重绘的调用。相同位置、尺寸和字体不重复触发原生更新。字体先用于测量，几何提交后统一绘制；放弃或准备失败时恢复字体，同时保留独立嵌套提交产生的新字体。

原生下拉框会在 `WM_SIZE` 中再次调整自身位置。实测该次调用丢失外层 `SWP_NOREDRAW`，在同批其他控件尚未完成布局时触发擦除和绘制。临时 `PendingPaint` 子类化在提交期间保留 `SWP_NOREDRAW | SWP_NOCOPYBITS`，延后显示 DC 的擦除及普通绘制。带有外来或内存 DC 的绘制保持原处理。该作用域不使用会改变可见性的 `WM_SETREDRAW`，也不改变焦点或鼠标捕获。

最终绘制先登记所有父窗、控件及后代的损伤，再调用 `UpdateWindow`。擦除由原生 `BeginPaint` 在绘制内部消费，避免独立的 `RDW_UPDATENOW` 提前擦除。嵌套提交按提交前的真实位置登记旧区域，跨父窗变化分别刷新。窗口坐标和提交使用所属父窗的 DPI 上下文，退出时恢复调用者上下文。

主窗口调整尺寸、侧栏拖动、底部分隔线拖动、曲线入口布局和视口字体调整使用同一批次规则。嵌套 `WM_SIZE` 保留外层的布局就绪状态，防止提前发布就绪。

插件窗口仍在自己的布局事务内提交几何与快照，但将最终绘制义务交给外层批次。宿主自有控件使用相同的临时绘制保护，插件提供的 surface HWND 不被接管。显式 LAYOUT 刷新仍执行原生 workspace 约束，并发布实际边界；普通拖动继续跳过相同几何。布局回调改变工程代次时，刷新请求仍返回 `MMS_E_STALE`。

如果插件几何已经提交，后续批次准备失败，已发生的外部变化仍保留在最终绘制清单中，字体在失败返回前恢复。失败批次不能再次提交。新增回归包含真实销毁的额外测试窗口，用于确定触发子类化安装失败。

## 命令溢出菜单

工具栏中的 BUTTON、GROUP 和 SEPARATOR 可以显式声明 `MMS_UI_OVERFLOW_MENU`。空间不足的命令进入「更多」菜单，`KEEP_TOGETHER` 分组整体迁移。原控件 HWND、元素 handle、命令和刷新目标保持身份，菜单状态来自同一注册内容。

菜单使用真实命令分派并在调用前重新检查注册、启用状态及所有者。卸载、关闭、注册变化与分配失败均有回归覆盖。菜单关闭前延后宿主销毁，避免弹出菜单仍在调用栈上时释放其所有者。

DOCK、声明式 INPUT 和新增侧栏、属性区、时间线、状态栏挂载点仍未开放。旧宿主不接受该溢出值时，SDK 调用返回不支持，插件可改用已注册的菜单位置。接口用法见 [C++ UI SDK](../../sdk/mms/cpp-ui.md)。

## 原生历史迁移

物理关键帧开关和中心位置偏移加入受控共同历史绑定路径。准备阶段构造独立变化，失败不写入原数据。开关检查关键帧分配及所属骨骼；中心偏移只处理已标记的根骨骼关键帧，并保留原生浮点加法、未修改轴的有符号零和稀疏自由曲线语义。

中心偏移对话框返回后重新检查对象、工程和父骨骼依赖。预算不足、取消、陈旧来源和重入均不能留下部分修改。撤销、重做使用保留的数据，不重新读取文件。

普通宿主的 `mms.edit` 门控未开放。未绑定共同历史的路径及原始兼容配置继续使用原逻辑。

## 验证记录

最终构建和回归结果在本轮完成后补入本节。以下先保留已发生的诊断事实，失败日志不覆盖为通过。

- `build/abi-resume-20261019/framework-contract-5.log` 捕获下拉框在提交期间自行产生的第二次位置变化和提前绘制。
- `build/abi-framework-regressions-20261019/ctest-full-menu-timeout.log` 保存 20/21 组通过及一次真实菜单选择超时。此前同一 ABI 契约单独运行通过，不能据此忽略超时。
- `build/abi-layout-framework-final-ordinary/` 和 `build/abi-layout-framework-final-bound/` 在普通、共同历史绑定模式下各完成 86 次稳定画面对照，无失败或中断。各有 7 次真实 Present、480 个区域样本，覆盖 9 条可见滑杆和 2 个曲线入口。几何改变中的两次中间比较明确跳过，返回原几何后的比较必须完成。该结果早于最后的准备失败收尾修正，不作为最后二进制的验收记录。

## 最终构建与回归（2026-10-20，第一版）

以下结果对应第一版最终源码与二进制；窄侧栏时间线修正后的结果见下一节。本轮在框架记录之后又修改了原始兼容分支的侧栏布局和两个测试。

- 完整配置：21／21 组通过，152.99 秒。日志 `build/verification/logs/ctest-full-final.log`。
- 原始兼容配置：18／18 组通过，16.81 秒。日志 `build/verification/logs/ctest-original-1.log`。
- 布局拖动稳定边界回归：普通与共同历史绑定模式各完成 86 次比较，0 失败、0 不完整。每种模式覆盖 9 条可见滑杆和 2 个曲线入口，另有 2 次预期几何变化比较跳过。第一版证据目录已在 2026-10-20 清理中删除，最终版证据在下一节。
- 契约重复运行：`ui_control_layout_contract` 6／6 通过，`plugin_abi_contract` 6／6 通过。日志 `build/verification/logs/repeat-targets.log`。

最终完整配置产品 `MikuMikuDance.exe` 的 SHA256 为 `D2CAE5EBC669786FBE63EC0939B4E5C1A9E2B0F01839CC5BCFC81EA2EFBD50B8`。用于布局回归的 `small_viewport_host_tests.exe` 的 SHA256 为 `8B7E3BFB78BF81BFF3328A4449EFFBCB8AF800AB153B97D55B30A4A6545B3344`。

回归期间出现过两次环境性中断，均未计为产品失败：

1. 一次绑定模式布局回归在启动阶段因真实按键或鼠标键处于按下状态返回 `INCOMPLETE`。该保护用于排除真实输入干扰；等待后重跑通过。
2. 一次完整回归中 `plugin_abi_contract` 的菜单循环等待 3 秒后超时。该次日志保留在 `build/abi-framework-regressions-20261019/ctest-full-menu-timeout.log`。`tests/plugins/overflow_menu_checks.inc` 现在对已投递的 `WM_CHAR` 做最多两次有界重试，并记录重试次数；契约重复运行 6 次均通过。这不证明所有系统负载下不再超时。

`tests/ui_control_layout_tests.cpp` 原先把“桌面前景窗口必须完全不变”作为断言。共享桌面上的其他程序切换前景会让该断言失败，而夹具本身并未激活。现在断言夹具不得成为前景，并继续要求没有激活、焦点或捕获变化；外部前景变化打印为 `LAYOUT_FOREGROUND` 证据，不再判失败。

原始兼容配置的侧栏拖动在 `ApplySidebarGeometry` 中保留原有的立即移动和 `PanelPaint` 路径，共用布局事务只用于完整配置。该修改改变兼容配置构建形态下的代码路径；上表完整配置和原始兼容配置的结果都在修改后重新构建并运行。

以上自动检查不是连续桌面呈现或 175% 人工拖动验收。侧栏、底部分隔线在 175% 下的用户视觉复验仍待完成；稳定边界比较不能替代该项人工结论。

## 窄侧栏时间线布局修正（2026-10-20）

用户提供的 175% 截图显示，侧栏逻辑宽度小于 328 像素时，换行后的“当前”和横滚动条行被上移 23 像素，压到橙色关键帧区域；纵向滚动条底端停在新位置之上，交界处还留下白条残影。根因是 `LayoutForcePasteRow` 只移动了子控件，面板位图 blit、底部 chrome 填充、损伤区域和命中测试仍使用原来的 `bottom - 248` 边界。

修正内容：

- [panel_controls.hpp](../../include/mikudancestudio/panel_controls.hpp) 新增 `TimelinePanelShift(sidebar)`，统一返回 0 或 23。控件布局、垂直滚动条高度、面板 blit 高度、chrome 填充顶部、损伤区域、选择框上限和点击命中范围使用同一偏移。
- [ui_windowsize.cpp](../../src/window/ui_windowsize.cpp) 在 `HandleWindowSize`、`RelayoutSidebarControls` 和 `LayoutForcePasteRow` 中都使用该偏移，不再先按旧位置移动再改一次。
- [wm_paint.cpp](../../src/window/wm_paint.cpp)、[ui_panel_paint.cpp](../../src/window/ui_panel_paint.cpp)、[ui_palette.cpp](../../src/window/ui_palette.cpp) 和 [ui_editor_click.cpp](../../src/window/ui_editor_click.cpp) 同步缩短或上移可见面板、失效区域、选择框和命中范围。
- [native_ui.cpp](../../src/plugins/native_ui.cpp) 的插件布局在延迟模式下只把损伤交给外层批次，不再自行对父窗和子窗调用 `RedrawWindow`。外层 `RepaintLayout` 因此只做一次父窗失效，也不会在 `WindowLayoutReady=0` 时进入绘制；记录失败时仍直接失效并标记失败。

验证：

- `sidebar_resize_tests` 全部通过，输出 `Sidebar live resize regressions passed`。新增检查覆盖 250、310、328、400、600 五种侧栏宽度下的横、纵滚动条位置、面板底部和二者交接。
- 完整配置 21／21 组通过，282.46 秒。原始兼容配置 18／18 组通过，14.08 秒。日志为 `build/verification/logs/ctest-full-final2.log` 和 `build/verification/logs/ctest-original-final2.log`。
- 布局拖动稳定边界：普通和共同历史绑定模式各 86 次比较，0 失败、0 不完整。普通模式第一次运行因真实按键或鼠标键处于按下状态中止，第二次通过；绑定模式一次通过。证据在 `build/verification/layout/ordinary/` 和 `bound/`。
- 最终完整配置产品 `MikuMikuDance.exe` 的 SHA256 为 `DABFCF5919CAD736B02FCF84A5C7CEB9A2BE67F9A9EF2EFA937BAC944DBDF624`。布局回归宿主 `small_viewport_host_tests.exe` 的 SHA256 为 `F7784142884D5FA09CBF896F440BAB92D56B510CBF2C912FF8CF6229A0C17FB3`。

用户在 2026-10-20 的复验中确认截图红框位置已正常：窄侧栏换行行不再压住橙色关键帧行，横纵滚动条交界没有白条残影。连续拖动时的滑杆白闪仍按首轮记录单独判定，本次确认只覆盖该区域。

以上自动检查仍不替代 175% 下的人工拖动复验。用于该复验的产品包在 `build/release/mms-abi-20261020-slider-check-r2/`，已在发布目录整理中删除。

## 构建目录与发布目录整理（2026-10-20）

发布目录不再包含构建树和验证证据，`build/` 下现在的内容：

- `win7-x64/build/`：完整配置构建树，包含产品、库和测试可执行文件。`CMakeCache.txt` 记录的绝对路径与目录一致，可以继续增量构建。
- `release/`：发布目录，只有 `MikuMikuDance.exe`、`licenses/` 和 `mms-adaptive-ui-template.zip`。
- `verification/logs/`：最终构建和回归日志。
- `verification/layout/`：最终布局拖动证据。
- `packages/mms-adaptive-ui-template.zip`：GUI 模块源码包，发布目录中的压缩包由此复制。

发布目录用 `scripts/package_abi_release.ps1` 重建：

```powershell
./scripts/package_abi_release.ps1 -Force
```

脚本核对构建目录属于当前源码、档位为 `full`、平台为 x64，然后复制程序、`licenses` 和 GUI 模块压缩包；发布目录的条目多于或少于这三项时直接报错。`cmake --install` 还会安装 `PLUGINS/config.json`，所以发布目录不用安装规则生成。

发布目录中的 `MikuMikuDance.exe` SHA256 为 `DABFCF5919CAD736B02FCF84A5C7CEB9A2BE67F9A9EF2EFA937BAC944DBDF624`，与本记录的最终产品一致。此前记录中的 `build/release/build/...` 和 `build/release/verification/...` 分别对应现在的 `build/win7-x64/build/...` 和 `build/verification/...`。

`build/release/mms-abi-20261020-slider-check-r2/` 和上一轮的其他可运行包在这次整理中删除。其余 230 余个测试运行目录、旧构建目录、旧日志和旧补丁在上一轮清理中删除，清理前后 `build/` 占用从约 8.1 GB 降到约 2.4 GB。旧记录引用的 `build/abi-*` 测试证据路径不再存在，历史结论以各记录中的文字为准。

## 兼容边界

本轮不修改 VMD 或 PMM 编解码、插值约定及精度。布局测试同时核对参数、播放头、物理状态、关键帧池和共同历史不被刷新修改。原始兼容配置保留原有立即绘制和字体接线。

基础实现仅使用 Win7 已有的 Win32、GDI 和 Common Controls。新系统 DPI 查询通过动态解析，缺少 API 时测试明确报告不可用。本机结果不等于 Win7 SP1 x64、AMD 真机或完整 CPU 路径验收。
