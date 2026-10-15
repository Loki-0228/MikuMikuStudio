# 插件 ABI 实现与验收记录

更新：2026-10-16。分支 `codex/plugin-abi` 从 `42d4a71` 建立。依据工作区《MMS 插件实施方案与 ABI 参考》《MMS 插件开发文档》《MMS 界面布局与重绘设计》实施。完整 P0–P6 尚未完成。

状态分为 **未实现**、**已实现未验证**、**本机已验证**、**缺其他系统/硬件环境**。代码、本机结果和平台结果分别列出。

## 逐项状态

| 工作包 | 项目 | 状态 | 范围与证据 |
| --- | --- | --- | --- |
| P0 | 固定 MSVC 19.44.35229、SDK 10.0.19041、Win7 宏及子系统 6.1 | 本机已验证 | profile、recipe lock、CMake preset；完整 MMS 和 ABI 测试编译成功。 |
| P0 | C11/C++17 数据布局、独立插件 /MT、宿主库 /MD | 本机已验证 | 编译与真实 DLL 加载通过。Conan 采用兼容的 193 二进制缓存，具体 package ID/revision 在本机 dependencies JSON。 |
| P0 | 普通及延迟 PE 导入清单 | 本机已验证 | 宿主、两个测试插件及测试 EXE；无 DX12、DXGI、d3dcompiler_47、CUDA、NVAPI 直接导入。并非逐 API 的 Win7 兼容证明。 |
| P0 | 固定 CRT/UCRT/VCOMP/DirectX 部署闭包 | 未实现 | 本机新版系统运行库不能代替 Win7 部署组合验收。 |
| P0 | Media Foundation 能力回退 | 已实现未验证 | 可选 COM 查询、旧 Sink Writer 编码器查询；高级 RGB32 协商失败重开基本处理 reader。已编译，尚无 Win7 媒体结果。 |
| P0 | Win7 SP1、Win10、AMD CPU/GPU 真机 | 缺其他系统/硬件环境 | 不以本机结果推定通过。 |
| P1 | 目录深度、清单、稳定 ID、依赖拓扑 | 本机已验证 | 根目录和中文目录；默认深度 2；包边界、重复 ID、环及版本错误。 |
| P1 | 绝对路径加载、原生依赖预检、文件锁及哈希 | 本机已验证 | 安全搜索及回退分支均在本机验证；旧 Win7 加载器另验。 |
| P1 | 生命周期、停止、UI 队列及失败取消 | 本机已验证 | create/start 失败、null 实例任务、BUSY、运行中取消；退休失败时保留实例和 DLL。 |
| P1 | 菜单、工具栏、EXE 改名 | 本机已验证 | 隔离隐藏启动真实 MMS，原名和中文名下调用、关闭通过。 |
| P1 | 命令、只读属性、更新、快照租约 | 本机已验证 | 所有权、参数范围/选项、上下文不可替换、旧快照及线程边界。 |
| P1 | 全部业务事件通知 | 未实现 | 当前提供 UI_READY/UI_CLOSING；其他来源仍待接入。 |
| P2 | Surface 工厂和生命周期 | 本机已验证 | 单/多视图、失败重试、错误父窗口、隐藏尺寸/DPI、换父重建、focus、停止和一次销毁。 |
| P2 | 工作区双页签实际挂载 | 本机已验证 | 真实 MMS 原名/中文名，切换、隐藏复用、焦点回调、关闭各销毁一次。 |
| P2 | 工具栏自动布局树 | 本机已验证 | 行列、换行、网格、伸缩、隐藏占位、嵌套高度及缩放往返。 |
| P2 | 布局事务和轻量提示 | 本机已验证 | MOVE beforeId、创建/删除空组、显隐、尺寸、快捷键、恢复；默认和用户覆盖分离。 |
| P2 | 几何快照 | 本机已验证 | HWND 应用后发布；UI/几何版本分开；未就绪 NOT_READY，旧租约不变。 |
| P2 | 可写属性、声明式 INPUT | 未实现 | 依赖 P3 工程事务及撤销，当前返回 NOT_READY。 |
| P2 | 左右侧栏、属性区、时间线、状态栏挂载 | 未实现 | 不枚举空壳挂载点。 |
| P2 | DOCK、命令溢出菜单 | 未实现 | 返回 UNSUPPORTED。 |
| P2 | 本体菜单、控件、业务命令全部语义登记 | 未实现 | 新注册表目前覆盖新 ABI 插件。 |
| P2 | 布局持久化、缺失插件保留、布局历史 | 未实现 | 当前偏好仅存本次进程。 |
| P2a | 可选布局/绘制面积/帧驱动/场景/物理/设备计数 | 本机已验证 | `MMS_UI_METRICS` 输出 JSONL；受控损伤矩形触发真实 WM_PAINT 和 NM_CUSTOMDRAW，纯 UI 的 scene、effectBegin、present、reset 增量均为 0。effectBegin 是外层代理 BeginScene 入口，不是 MME 内部阶段计数。 |
| P2a | MME 内部阶段计数和 ABBA 性能基线 | 未实现 | 不宣称性能改善。 |
| P2b | 局部 GDI 缓冲、批量 HWND 应用及失败恢复 | 本机已验证 | 多父窗口分批、验证实际坐标、失败回滚、缓冲复用/初始化/尺寸键；接入新工具栏及扩展区。 |
| P2b | 本体既有参数区迁移与全区域缓冲 | 未实现 | 新扩展区通过不替代旧区域迁移。 |
| P2c | 独立更新协调器逻辑 | 本机已验证 | 虚拟时钟合并、重入、代次、Retry、Failed/异常、隐藏恢复；新几何保留未消费工作。 |
| P2c | 统一更新协调器、WM_PAINT 解耦、暴露恢复 | 本机已验证 | 增强构建从主泵及原生尺寸循环的安全点调用帧驱动；WM_PAINT 只请求重绘。QPC 单调时钟、请求合并、失败保留、隐藏与模态退避、回调及帧重入保护。原始兼容构建保留旧路径。 |
| P2c | 停止恢复、播放、媒体、设备及导出 | 本机已验证 | 动态刚体验证停止重绘不推进物理，播放仍推进；12 项设备与录制恢复、停止视频更新、图片序列及时间抗锯齿通过。见下方夹具范围。 |
| P2c | 真实 MME 内部状态、真实桌面遮挡及多 DPI | 未实现 | 当前真实主路径使用内置效果及隐藏窗口；旧 MME 内部状态保持和桌面视觉效果尚未验收。 |
| P2d | 全区域统一几何、绘制和命中迁移 | 未实现 | 新扩展区接入不等于全部本体区域完成。 |
| P2e | 可选实时拖动 | 未实现 | 保留原有松手提交默认模式。 |
| P2f | 独立表单及模拟视口宿主 | 本机已验证 | 两个独立 CMake 项目构建及隐藏自检 exit 0，无 MMS/图形设备依赖。 |
| P2f | 两个示例的真实输入法、多 DPI、视觉交互 | 已实现未验证 | 自检不替代实际桌面交互验收。 |
| P2 | 输入法组合态、Tab、辅助功能和真实多 DPI | 已实现未验证 | 保留原生控件；隐藏窗口测试不替代完整交互验收。 |
| P3 | scene 全通道快照和稳定对象身份 | 未实现 | 服务返回 NOT_FOUND。 |
| P3 | edit 两阶段事务、可写属性和统一撤销 | 未实现 | edit.h 布局已固定，服务尚未发布。 |
| P3 | animation 轨道/键、层、片段、变速、烘焙和选择 | 未实现 | 服务返回 NOT_FOUND。 |
| P3 | storage serializer、工程数据和显式导入 | 未实现 | 保持 EMM/EMD 显式导入要求。 |
| P4 | compute 两阶段求值、状态域及 CPU 协商 | 未实现 | 不以成功空表占位。 |
| P4 | binding、cache、bake、MMV 等价清单 | 未实现 | 依赖 P3；旧 MMV 继续原路径。 |
| P5 | render、post、DX9 帧/设备代次和资源租约 | 未实现 | 现有渲染路径通过不代表新 ABI 通过。 |
| P6 | 原创 MMSAdapter、独立 MMHack 兼容导出 | 未实现 | 先恢复精确原型、调用约定和寿命。 |
| P6 | legacy_mme 改名和子目录兼容 | 未实现 | 未发布第三方文件；未知旧插件仍属理论范围。 |

## 验证记录

日志位于本地 `build/`，不作为发行内容。探针只启动独立目录的隐藏 MMS，并使用独立 LOCALAPPDATA。

| 验证 | 结果 | 日志 |
| --- | --- | --- |
| Win7 目标完整 MMS 和测试构建 | 通过 | `plugin-abi-p2-host-build.log` |
| ABI、加载、生命周期、布局及面板契约 | 247 项通过（阶段提交 ee42e5b） | `plugin-abi-p2-tests.log` |
| 新增协调器和原生批量/缓冲契约 | 累计 296 项通过 | `plugin-abi-win32-tests.log` |
| 无 MMS 的原生表单和模拟视口 | 两个项目均 exit 0 | `native-form-test.log`、`viewport-host-test.log` |
| 真实 MMS 原名/中文名菜单、工具栏和关闭 | 两种名称通过；工具栏 32 像素 | `plugin-abi-win7-p2-host-smoke.log` |
| 真实 MMS 双面板页签 | 两种名称通过 | `plugin-abi-panels-host-smoke.log`，夹具 `abi-p2-panels-smoke-2` |
| 新缓冲与计数的真实 MMS 探针 | 原名/中文名通过；隐藏启动无 WM_PAINT，不作纯 UI 零增量证据 | `plugin-abi-buffer-host-smoke.log` |
| 统一调度实际帧路径 | 16 组均 exit 0；序列累计 1072 项断言通过 | `plugin-abi-scheduler-full-3.log`、`abi-scheduler-full-3/report.json` |
| 调度接入后的插件、双页签与改名 | 原名/中文名通过 | `plugin-abi-scheduler-host-smoke-2.log` |
| 调度接入后的 ABI 契约 | 通过 | `plugin-abi-scheduler-contract.log` |
| PE 导入/导出清单 | 四文件通过可选后端直接导入检查 | `plugin-abi-win7-pe-inventory.json` |
| 处理器策略 | 99 项通过，P2 新增前 | `plugin-abi-processor.log` |
| 侧栏/底部松手提交、取消、浮动和媒体 | 通过，P2 新增前 | `plugin-abi-sidebar.log` |
| 视口缩放、菜单语言、可选图形导入、旧插件菜单 | 5 个目标通过，P2 新增前 | `plugin-abi-regressions.log` |

侧栏测试的附件排序用例缺少初始选中项。关闭新 ABI 初始化时也复现同一失败，见 `plugin-abi-accessory-without-abi.log`。测试已明确初始选择并验证排序后保持选择；附件业务逻辑未修改。

首次双面板测试把 SURFACE_DESTROY 计入 DESTROY 子串数量。已改按完整事件行计数并复跑，保留视图先销毁、插件随后停止和销毁的顺序检查。

统一调度测试启动隔离的隐藏窗口，只替换该测试窗口的可见性查询及绘制损伤矩形。场景、物理、GPU、编码器及输出仍走 MMS 实现。测试覆盖多次 UI 绘制零场景增量、视口暴露合并、绘制失败重试、动态刚体保持、实际窗口尺寸变更、模态与编辑锁、播放推进、设备丢失与取消。侧栏和底部默认仍在松手时提交布局。

最初序列夹具把每次 SafePoint 调用当成一帧，150 次调用在 60 fps 测试中只生成 48 张图片。夹具现按实际 FrameDriver 进入计数，并为等待设置墙钟上限；输出帧数、连续编号和终结断言保持不变。`full-3` 的 24、30、60、29.97 fps 输出分别为 24、30、60、30 张，包含模型模式、相机模式、七种图片格式、三语言、取消及碰撞检查。时间抗锯齿用本地 `YYB Hatsune Miku_NT_1.0ver.pmx`，路径与 SHA-256 记入报告；它只作为本机夹具，不进入 SDK 或发行包。alpha-triangle 用于设备恢复和菜单初始化，不足以覆盖该抗锯齿用例固定相机下的可见几何要求。

## 重现

先按 `profiles/win7-x64.lock` 安装 Conan 依赖至 `build/win7-x64`，再执行：

```powershell
cmake --preset win7-x64
cmake --build --preset win7-x64-release --target MikuDanceStudio plugin_abi_tests
ctest --preset win7-x64-release -R '^plugin_abi_contract$' --output-on-failure
python tests/plugins/host_smoke.py build/win7-x64/build/Release/MikuMikuDance.exe build/win7-x64/build/tests/plugins/lifecycle/Release/lifecycle.dll build/abi-smoke-new gui-panel
```

本机 PDB 工具版本不匹配，验证构建显式设 `MIKUDANCESTUDIO_DEBUG_SYMBOLS=OFF`，默认配置仍生成符号。资源编译临时目录设为仓库内 `build/abi-tmp`，未修改系统环境。

Win7 回退依据：[GetTransformForStream](https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriterex-gettransformforstream) 要求 Win8；[GetServiceForStream](https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-getserviceforstream) 支持 Win7。代码保持 Win7 目标宏，仅查询可选 COM 能力；编译通过不替代媒体功能验证。
