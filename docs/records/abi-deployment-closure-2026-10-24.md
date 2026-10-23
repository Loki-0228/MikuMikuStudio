# 固定 Win7 部署闭包

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批实现 P0「固定 CRT/UCRT/VCOMP/DirectX 部署闭包」的安装规则与导入审计；Windows 7 SP1 真机运行仍待外部环境。

## 实现

- [CMakeLists.txt](../../CMakeLists.txt)：新增部署闭包装配。`InstallRequiredSystemLibraries` 选择当前工具集的 VC/UCRT 文件，由 `CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION` 安装到包根目录；`vcomp140.dll` 从 VC Redist 的 `Microsoft.VC143.OpenMP` 复制；`d3dx9_43.dll`、`d3dcompiler_43.dll` 在构建机存在时复制进闭包，否则安装期告警。原有 EXE、NVIDIA 辅助 DLL、licenses 与 PLUGINS 安装规则保留。
- [tools/verify-deployment-closure.ps1](../../tools/verify-deployment-closure.ps1)：用 `dumpbin /dependents` 读取每个 EXE/DLL 的导入与延迟导入表，分类为 Windows 7 系统组件、VC/UCRT 运行时、DirectX 9 或禁止项（DX12/DXGI/D3D11/d3dcompiler_47/CUDA/NVAPI）。未知 DLL、缺失的运行时文件与禁止项都会失败并写出 JSON 报告。
- [docs/reference/win7-deployment.md](../reference/win7-deployment.md)：部署集合、安装/审计命令、许可说明与未完成边界。

## 验证

证据目录 `build/verification/abi-deployment-closure-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 完整配置安装 `cmake --install build/cmake --prefix build/deployment-full-20261024` | 通过；根目录含 EXE、NVIDIA DLL、VC/UCRT、vcomp140、d3dx9_43、d3dcompiler_43 |
| 完整配置审计 | `DEPLOYMENT CLOSURE OK: 55 binaries, 51 redistributable, 2 DirectX files`；JSON 报告 `full.json` |
| 原始兼容配置安装与审计 | 通过；`original.json` |
| 导入清单 | 产品导入 `d3dx9_43`、`MSVCP140`、`VCRUNTIME140(_1)`、UCRT `api-ms-win-crt-*`，延迟导入 `VCOMP140.DLL`；无 DX12/DXGI/CUDA/NVAPI 静态导入 |
| 产品二进制 | 本批只加安装与审计，完整产品 `bb121beb…`、原始兼容产品 `5eaf8da4…` 不变；正式 release 仍为 `180db258…` |

## 未完成

- 未在 Windows 7 SP1 x64 真机安装运行；本机 Windows 10 结果不能代替 Win7 加载器、UCRT/VCOMP 行为与 DirectX 环境验收。
- 未验证 32 位/WOW64、无管理员安装、代码签名、自解压安装器。
- 静态导入审计不覆盖运行期动态加载路径；DirectX 文件从构建机系统目录复制，分发须遵守对应再分发条款。

## 后续

下一批继续 P0 的其余外部项记录，并推进插件事件/渲染等剩余接口；Win7 与 AMD 真机验收保留为外部条件。
