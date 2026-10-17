# 重绘 SDK 验证（2026-10-17）

本批基于 `codex/plugin-abi` 的 `c762f51`，工作目录为 `D:/MMD-Next/MMS-abi-dev`。新增公共头文件 `mms/cpp/refresh.hpp` 和[开发参考](../../sdk/mms/cpp-refresh.md)，没有修改宿主运行时代码或 C 表。

## 交付与审查

`RefreshServices` 组合原服务查询、目标查询与请求。简单插件可在 UI 命令中用 `RepaintElement` 一次调用提交整目标重绘；后台插件先捕获目标，再用 Repaint、RepaintRect 或 RepaintRects。复杂插件可用 Request 或原 api 表，身份、权限、矩形、代次及队列均为原实现。

`RefreshTicket` 独占原句柄，可移动构造、显式移交给 C 接口，不能复制或移动赋值。接受请求后不再分配内存；已有输出票据时拒绝覆盖。失败的取消和释放保留所有权，NOT_FOUND 表示原票据已不存在，可以清理 SDK 记账。析构只尝试释放，不隐式取消，stop 必须显式检查结果并保留失败对象供重试。

服务表按兼容前缀检查全部六项函数。SDK 输出方法先准备局部 C 输出，成功后再写调用者对象。票据保存表副本，不依赖 RefreshServices 变量的存活地址；宿主 context 必须保持有效。对象本身没有并发修改保护，跨线程转移票据须同步。

STATE 的启用、选中和显隐计算尚未拆离既有布局；本批不通过旧 Refresh 伪装实现 STATE。DATA、STATE、LAYOUT、绑定、默认订阅和帧回调继续等待后续接入。普通 `mms.edit` 仍未开放。

## 验证

| 检查 | 本批结果 | 证据 |
| --- | --- | --- |
| 构建 | 新 SDK、原生契约测试和独立 `/MT` 测试 DLL 按 x64、C++17 构建通过。 | `build/refresh-sdk-build-final-2.log` |
| 新 SDK 与既有刷新专项 | 累计 7,349 次断言通过，含轮询断言。新增检查服务协商、未来兼容前缀、缺失函数、错误线程、输出不变、移动及移交、原句柄互操作、后台操作、作用域释放继续执行、清理失败重试、旧几何和停止门禁。原队列及真实窗口区域检查继续执行。 | `build/refresh-sdk-contract-final-2/result.log`、`report.json` |
| 真实 Manager DLL | 测试 DLL 已改用 SDK 取得目标并提交局部重绘。忙碌和 storage 准备时延后、几何保持、停止取消、终态保留及释放后卸载继续通过。没有用额外 LoadLibrary 引用伪造保活。 | 同上，`plugin.log` |
| 真实完整宿主 | 原名及中文改名各通过菜单和工具栏调用；每次命令内取得原目标，后台提交成功，然后显式释放，宿主正常关闭。 | `build/refresh-sdk-host-final-2.log`、对应 normal/renamed 目录的 `probe.log` |
| 既有 ABI | 1,694 项通过。 | `build/refresh-sdk-regression-final-2.log` |
| PE 清单 | 测试 EXE 与 DLL 均为 x64、子系统 6.1，无所检查的 DX12、DXGI、d3dcompiler_47、CUDA、NVAPI 直接或延迟导入。 | `build/refresh-sdk-pe-final-2.json` |

部分服务失败和清理错误由测试转发表注入，注册表、刷新队列、句柄及损伤区域仍使用生产实现。查询异常检查是 SDK 异常转换的测试，不允许真实插件让异常跨 DLL。未注入全局分配器失败；请求成功后的无分配保存经过源码审查。

真实完整宿主探针覆盖提交、释放和正常关闭；完成状态及真实更新区域由独立桌面的 Manager/窗口测试验证。没有把释放日志当成像素已显示，也没有将本批计为新的场景求值或历史性能测试。宿主源文件未修改，因此本批未重复全套场景、物理、媒体及工程历史回归。

复验命令与上一批相同，增加 SDK 检查后仍使用[私有桌面运行器](../../tests/plugins/run_refresh_checks.py)：

```powershell
python tests/plugins/run_refresh_checks.py build/win7-x64/build/tests/Release/plugin_abi_tests.exe build/win7-x64/build/tests/plugins/lifecycle/Release/lifecycle.dll build/refresh-sdk-new
ctest --test-dir build/win7-x64/build -C Release -R '^plugin_abi_contract$' --output-on-failure
python tests/plugins/host_smoke.py build/win7-x64/build/Release/MikuMikuDance.exe build/win7-x64/build/tests/plugins/lifecycle/Release/lifecycle.dll build/refresh-sdk-host-new gui-refresh-auto-release
```

## 稳定版与平台边界

本批没有重建或覆盖 ABI 宿主 EXE，其 SHA256 仍为 `84A972CDA8A1B26BE1E97B669C95C4054972723332BCC0E85E6CCEBD8364EC1E`。独立测试 DLL 与契约程序已重新编译。

main 仍为 `04d3dec`，工作树无修改。稳定程序 `D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe` SHA256 仍为 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`，没有复制新文件到稳定版目录。

Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求保留。此头文件不引入新的系统或显卡 API；测试插件原已使用工作线程。本机结果与导入检查不能替代 Win7、AMD 真机和运行库部署验收。
