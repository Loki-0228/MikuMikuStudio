# UI 局部重绘验证（2026-10-17）

本批基于 `codex/plugin-abi` 的 `d5e8e7a`，工作目录为 `D:/MMD-Next/MMS-abi-dev`。交付 `mms.ui.refresh` 1.0 的 `REPAINT + SELF` 阶段，接口参考见 [SDK 文档](../../sdk/mms/refresh.md)。

## 实现范围

公共 C 表提供能力查询、目标查询及 request/query/cancel/release。目标使用原 `mms.ui` 注册句柄，额外携带呈现实例和工程代次；局部 DIP 矩形还检查实际几何修订。按钮无需声明 LAYOUT，面板查询只读取现存 view，不创建或显示窗口。

后台只访问锁保护的目标镜像和自有票据。宿主使用现有消息窗口的独立刷新消息，在 UI 安全点合并区域并调用异步窗口失效 API，不执行普通插件任务或旧 `Refresh`。忙碌时用单一定时器重试，准备失败结束待处理票据。它不调用命令状态回调、布局、场景求值、物理更新或工程历史。

布局开始移动窗口前暂停目标；只有完整成功的布局重新发布目标。实际几何不变时保留修订，避免命令返回后的常规布局误作废局部请求。元素更新、窗口销毁及工程替换分别使对应旧引用失效。

每插件 256 票据、32 矩形/请求、256 KiB 保留数据；全局 4,096 票据、4 MiB，每次最多处理 64 项贡献。取消只撤回自己的贡献，释放不等于取消。未释放终态和已释放但仍待执行的票据均占额度。停止取消 pending；未释放票据和在途调用继续保留插件实例与 DLL。

DATA、STATE、LAYOUT、绑定、依赖刷新、任意宿主区域、绘制层和动画仍待实现。现有 C 表布局未变；普通宿主的 `mms.edit` 仍未开放，可写属性仍返回 NOT_READY。C++ 便利 SDK 尚未包装本服务，直接查询同一套 C 表即可。

## 验证结果

证据路径相对于 ABI 工作目录。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 完整宿主及测试构建 | Win7 配置的完整宿主、C11/C++17 布局检查、插件契约、侧栏与历史测试目标构建通过。 | `build/refresh-host-build.log`、`build/refresh-tests-build.log`、`build/refresh-contract-build-4.log` |
| 新刷新契约与真实 Manager | 最终 7,229 次断言通过，包含状态轮询断言。覆盖后台四种票据操作、所有权、有限参数、局部裁剪、144 DPI 外扩、区域并集、取消、释放、配额、64 项批次、唤醒失败、实例及工程失效。 | `build/refresh-complete-final/result.log`、`report.json`、`plugin.log` |
| 实际窗口绘制 | 测试专用窗口用真实 GetUpdateRgn 和 BeginPaint；未替换绘制损伤或伪造 rcPaint。局部区域、复杂并集及面板异步绘制通过。隐藏目标只验证完成、不显示及不同步绘制。 | 同上，`tests/plugins/refresh_checks.inc` |
| 真实服务与卸载 | 独立 DLL 经 Manager 查询服务，命令内取得目标，工作线程提交。普通回调后的同几何布局、宿主忙碌和 storage 准备期间延后、定时器恢复、pending 取消、终态保留及释放后卸载通过。 | 同上，`tests/plugins/manager_refresh_checks.inc` |
| 既有 ABI 与场景历史 | 既有 ABI 1,694 项及场景历史 UI 回归通过。 | `build/refresh-regressions-final.log` |
| 既有帧调度 | 隐藏生产宿主的纯 UI 绘制、视口暴露恢复、动态刚体保持、忙碌及播放调度通过。本项是既有调度回归，不单独计为新服务的场景计数测试。 | `build/refresh-scheduler-1.log` |
| 普通宿主与 SDK | 原名和中文改名下的启动、菜单、工具栏、双面板与关闭通过；SDK 示例的共享状态和重新布局后调用通过。 | `build/refresh-smoke-final.log`、`build/refresh-sdk-1.log` 及对应目录报告 |
| PE 导入检查 | 宿主、契约测试、侧栏测试与测试 DLL 均为 x64，无清单所列 DX12、DXGI、d3dcompiler_47、CUDA、NVAPI 直接或延迟导入。宿主子系统为 6.1。 | `build/refresh-pe-final.json` |

分组分配异常终结、布局失败暂停和纯刷新不进入编辑来源的行为经过源码审查；本轮未注入全局分配器或 SetTimer 失败。最大字节配额包含票据记录及矩形数组容量，不是对整个分配器的测量；合法请求通常先达到票据数上限，不将字节上限单独触发计为测试成功。

首次将完整契约放到私有桌面时，既有 GDI 缓冲复制测试失败，因此完整契约仍在通常桌面的隐藏窗口执行。刷新测试独立放到不激活的私有桌面。新增测试首次要求隐藏按钮保留可查询更新区域而失败；Windows 可以丢弃该区域，现验证失效调用完成而不声称像素显示。两次失败均未计入通过。

## 重现

先构建 `MikuDanceStudio`、`plugin_abi_tests`、`sidebar_resize_tests` 和 `scene_history_ui_tests`。刷新专项运行器要求 Windows Python 和 pywin32；这是测试依赖，不是 MMS 运行依赖。

```powershell
python tests/plugins/run_refresh_checks.py build/win7-x64/build/tests/Release/plugin_abi_tests.exe build/win7-x64/build/tests/plugins/lifecycle/Release/lifecycle.dll build/refresh-new
ctest --test-dir build/win7-x64/build -C Release -R '^(plugin_abi_contract|scene_history_ui_regressions)$' --output-on-failure
build/win7-x64/build/tests/sidebar/Release/sidebar_resize_tests.exe --scheduler-only
```

输出目录须不存在。运行器只操作自己创建的进程、文件与桌面，不切换用户桌面。刷新专项使用 `--refresh-only`，完整契约不包含这些需要显示测试窗口的断言。

## 稳定版与平台边界

ABI 完整宿主 SHA256 为 `84A972CDA8A1B26BE1E97B669C95C4054972723332BCC0E85E6CCEBD8364EC1E`。本批修改宿主源文件，已重新编译、链接，不沿用旧 EXE。

main 仍为 `04d3dec`，工作树无修改。`D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe` SHA256 仍为 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`。未向稳定版目录复制新文件。

Win7 SP1 x64、CPU 基础路径及 AMD 要求保留。DPI 新系统接口继续动态查询，Win7 使用窗口 DC 回退；未引入特定厂商基础依赖。本机编译、静态导入与运行结果不替代 Win7、AMD 真机及部署闭包验收。
