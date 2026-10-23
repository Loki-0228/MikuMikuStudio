# Windows 7 部署闭包

本页说明 MikuMikuStudio 固定运行时（P0）的部署集合、安装命令与审计方法。闭包只覆盖本机可验证的文件清单；**Windows 7 SP1 真机运行验收仍待外部环境**。

## 闭包组成

| 类别 | 文件 | 来源 |
| --- | --- | --- |
| Windows 7 SP1 系统组件 | `kernel32`、`user32`、`gdi32`、`advapi32`、`shell32`、`ole32`、`oleaut32`、`comdlg32`、`comctl32`、`gdiplus`、`mfplat`、`mfreadwrite`、`d3d9`、`bcrypt`、`msvcrt` 等 | 操作系统自带，不随包分发 |
| VC++ 2015–2022 x64 运行时 | `msvcp140*.dll`、`vcruntime140*.dll`、`concrt140.dll`、`vcomp140.dll` | `InstallRequiredSystemLibraries` 从本机 VC Redist 选取；`vcomp140` 单独从 `Microsoft.VC143.OpenMP` 复制 |
| UCRT | `ucrtbase.dll`、`api-ms-win-crt-*.dll`、`api-ms-win-core-*.dll` | Windows SDK 10.0.19041 Redist；随包分发，Windows 7 无需另装 KB2999226 |
| DirectX 9 | `d3dx9_43.dll`、`d3dcompiler_43.dll` | DirectX End-User Runtime（June 2010）；安装时若能找到系统文件则复制进闭包，否则告警 |
| NVIDIA 辅助 DLL | `MMSNvidiaAAFix.dll` | 本项目构建；单独组件，可选 |
| 禁止项 | `d3d12.dll`、`dxgi.dll`、`d3d11.dll`、`d3dcompiler_47.dll`、CUDA/NVAPI DLL | 审计失败；DX12 预览通过运行期动态加载，不在导入表 |

## 安装

```
cmake --install build/cmake --config Release --prefix build/deployment-full
```

原始兼容配置同理，前缀换成原始构建目录的 install 目标。安装布局：

- 根目录：`MikuMikuDance.exe`、`MMSNvidiaAAFix.dll`、上述运行时 DLL；
- `PLUGINS/config.json`、`licenses/`。

安装步骤不重新编译产品；部署目录中的 EXE 与构建产物一致。

## 审计

```
pwsh -File tools/verify-deployment-closure.ps1 -DeploymentDir build/deployment-full
```

脚本用 `dumpbin /dependents` 读取部署目录中每个 EXE/DLL 的导入与延迟导入表，把依赖分类为系统组件、VC/UCRT 运行时、DirectX 9 或禁止项。未识别的 DLL、缺失的运行时文件与禁止项都会使审计失败，并写出 JSON 报告（`deployment-closure.json`）。当前结果：

| 部署目录 | 结果 | 说明 |
| --- | --- | --- |
| 完整配置 `deployment-full-20261024` | 通过 | 55 个 PE、51 个运行时文件、2 个 DirectX 文件；产品导入包括 `d3dx9_43`、VC/UCRT 与延迟加载的 `VCOMP140` |
| 原始兼容配置 `deployment-original-20261024` | 通过 | 同一闭包；产品少一组运行时导入 |

报告见 `build/verification/abi-deployment-closure-20261024/full.json` 与 `original.json`。

## 许可与分发

- VC++ 运行时与 UCRT 文件按 Microsoft 可再分发条款随包分发；
- DirectX 9 文件按 DirectX End-User Runtime 条款分发，或要求目标机安装该运行时；
- 运行时 DLL 不应提交进源码仓库，只存在于构建/安装目录。

## 未完成

- 未在 Windows 7 SP1 x64 真机安装并运行本闭包；本机为 Windows 10 x64，缺少 Win7 加载器、UCRT/VCOMP 行为与 DirectX 组件的真实环境证据。
- 未验证 32 位、WOW64、无管理员权限安装、代码签名与自解压安装器。
- 审计是静态导入表检查，不覆盖运行期动态加载（例如可选 DX12 路径通过 `GetProcAddress` 探测）。
