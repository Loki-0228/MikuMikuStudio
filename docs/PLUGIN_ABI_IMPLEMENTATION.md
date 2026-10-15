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
| P2a | 布局/绘制/场景/效果/设备计数和 ABBA | 未实现 | 不宣称性能改善。 |
| P2b | 局部 GDI 缓冲、批量 HWND 应用及失败恢复 | 未实现 | 面板裁剪已接入，其他路径仍待迁移。 |
| P2c | 统一更新协调器、WM_PAINT 解耦、暴露恢复 | 未实现 | 尚未删除现有帧驱动，不以漏刷新减少渲染。 |
| P2d | 全区域统一几何、绘制和命中迁移 | 未实现 | 新扩展区接入不等于全部本体区域完成。 |
| P2e | 可选实时拖动 | 未实现 | 保留原有松手提交默认模式。 |
| P2f | 独立表单及第二宿主运行 | 未实现 | 布局库可独立构建，完整第二宿主未验收。 |
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
| ABI、加载、生命周期、布局及面板契约 | 247 项通过 | `plugin-abi-p2-tests.log` |
| 真实 MMS 原名/中文名菜单、工具栏和关闭 | 两种名称通过；工具栏 32 像素 | `plugin-abi-win7-p2-host-smoke.log` |
| 真实 MMS 双面板页签 | 两种名称通过 | `plugin-abi-panels-host-smoke.log`，夹具 `abi-p2-panels-smoke-2` |
| PE 导入/导出清单 | 四文件通过可选后端直接导入检查 | `plugin-abi-win7-pe-inventory.json` |
| 处理器策略 | 99 项通过，P2 新增前 | `plugin-abi-processor.log` |
| 侧栏/底部松手提交、取消、浮动和媒体 | 通过，P2 新增前 | `plugin-abi-sidebar.log` |
| 视口缩放、菜单语言、可选图形导入、旧插件菜单 | 5 个目标通过，P2 新增前 | `plugin-abi-regressions.log` |

侧栏测试的附件排序用例缺少初始选中项。关闭新 ABI 初始化时也复现同一失败，见 `plugin-abi-accessory-without-abi.log`。测试已明确初始选择并验证排序后保持选择；附件业务逻辑未修改。

首次双面板测试把 SURFACE_DESTROY 计入 DESTROY 子串数量。已改按完整事件行计数并复跑，保留视图先销毁、插件随后停止和销毁的顺序检查。

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
