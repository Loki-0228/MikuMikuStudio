# Windows ABI 候选：锁定依赖与构建前检查

本页从独立源码副本准备 `win7-x64` 完整档 Release。它不需要第三方凭据，不安装软件，不打开产品，不修改主分支或稳定发布目录，也不提交、推送或触发 CI。这里只证明依赖可定位、Bullet 可复原；真实 Windows 构建和测试仍未运行。

## 可核对的依赖

- **Bullet 2.75。** README 里的 GitHub tag ZIP 返回 404。上游[发行公告](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=4067)指向原 Google Code 项目；实际可下载的[官方历史 ZIP](https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/bullet/bullet-2.75.zip) 的 SHA-256 为 `fe2e369593b034c00a1245676edd8417a58c8030a94e061220f0c228ca61fe8f`。保持 ZIP 内 3,154 个文件的原始字节、完整目录和 `conanfile.py` 的 LF；按 [Conan 版本清单算法](https://github.com/conan-io/conan/blob/2.21.0/conan/internal/model/manifest.py) 计算，等于当前锁定的 `bullet275/2.75#b1f525c50ae139d422748c95626b34ef`。只解压 `src/`、重写换行或替换为较新 Bullet 都不会匹配。ZIP 中的旧 demo 二进制只作为锁定清单的一部分，不运行、不链接、不随候选分发。
- **ConanCenter。** 已查到锁定的 `alembic/1.8.8#1eb7f96673ab004a34f1085147fac5f2`、`imath/3.2.2#d5203a84ee631ae6ab2da7c3a6b16787`、`minhook/1.3.4#1ae65f6b5793b097a606940bb1774119` recipe 均在 [官方公共远程](https://docs.conan.io/2/devops/using_conancenter.html)。本次未下载或构建对应 Windows 二进制；锁文件固定的是 recipe，而非每个 `.lib` 字节。若需冻结二进制，保留本次依赖图中实际的 package ID、package revision 和库哈希。
- **编译器与 SDK。** 需要 MSVC `19.44.35229`（工具集目录 `14.44.35207`）、SDK `10.0.19041.0`、CMake/CTest、64 位 Python 3.11+、Conan 2、PowerShell 7.4+ 和 Git。候选保持 Win7 baseline、DX12 关闭和效果预编译开启。环境中有多版 SDK 时，资源生成另须指定 `RC_EXE`，否则它会挑最新 SDK。
- **DirectX。** `scripts/compile_effect.py` 从 64 位 `System32` 绝对路径加载 `d3dx9_43.dll`。仅下载 [Microsoft.DXSDK.D3DX 9.29.952.8](https://www.nuget.org/packages/Microsoft.DXSDK.D3DX/9.29.952.8) 的应用本地 DLL 不能满足此构建步骤。需要已按授权预装 [Microsoft DirectX End-User Runtimes (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=8109) 的受控 Windows x64 环境，并验证 `d3dx9_43.dll` 可加载及 `d3dcompiler_43.dll` 存在。不把 DLL 手动写入系统目录，不使用来源不明的副本，不关掉效果预编译来冒充同一配置。

## 已预装工具的隔离 Windows x64 环境

先在选定 VS 2022 的 x64 开发终端中核对工具版本、SDK、系统 D3DX，并在专用源码副本根目录运行。`C:/mms-qa` 已存在；`win7-lock`、`bullet-recipe` 和 `checks` 都须是新目录。短路径有助于避免 Windows 路径长度限制。Conan 缓存必须独立，避免意外复用以前其他工具链生成的包。

```powershell
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$job = 'C:/mms-qa/win7-lock'
New-Item -ItemType Directory -Path $job | Out-Null
$env:CONAN_HOME = "$job/conan"
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:RC_EXE = Join-Path $env:WindowsSdkDir 'bin/10.0.19041.0/x64/rc.exe'
$cl = Get-Item (Get-Command cl.exe -CommandType Application -ErrorAction Stop).Source
if ($cl.VersionInfo.FileVersionRaw.ToString() -ne '19.44.35229.0') { throw 'Wrong MSVC compiler' }
if (-not (Test-Path -LiteralPath $env:RC_EXE -PathType Leaf)) { throw 'Missing SDK 10.0.19041.0 rc.exe' }
Get-FileHash -Algorithm SHA256 -LiteralPath $cl.FullName, $env:RC_EXE,
    "$env:SystemRoot/System32/d3dx9_43.dll", "$env:SystemRoot/System32/d3dcompiler_43.dll"
python -B -c "import ctypes as c, pathlib as p, sys; assert sys.maxsize > 2**32; k=c.WinDLL('kernel32'); s=c.create_unicode_buffer(32768); assert k.GetSystemDirectoryW(s,len(s)); getattr(c.WinDLL(str(p.Path(s.value) / 'd3dx9_43.dll')), 'D3DXCreateEffectCompiler')"
cmake --version
conan --version
git rev-parse HEAD
```

把官方 ZIP 放在 `$job/bullet-2.75.zip`，离线核验后才复制到外部暂存目录。`conan export` 的输出必须报告上述 Bullet revision。以下使用支持 `tools.microsoft:msvc_update` 的 Conan 2（例如 2.34）；其补丁号写成 `4.35207`，结合 profile 的 `compiler.version=194` 生成 `v143,version=14.44.35207`。运行前确认 `conan remote list` 指向 `https://center2.conan.io`。

```powershell
python -B tools/check_win7_bullet_source.py "$job/bullet-2.75.zip" --stage-recipe "$job/bullet-recipe"
conan export "$job/bullet-recipe"
conan remote list
conan install . --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 `
  --lockfile profiles/win7-x64.lock --output-folder build/conan '--build=*' -r conancenter `
  --conf:all tools.microsoft:msvc_update=4.35207 --conf:all tools.microsoft:winsdk_version=10.0.19041.0 `
  --format=json --out-file "$job/conan-install.json"
cmake --preset win7-x64 -DMIKUMIKUSTUDIO_BUILD_UI_TESTS=ON `
  -DMIKUMIKUSTUDIO_PRECOMPILE_EFFECTS=ON 2>&1 | Tee-Object "$job/configure.log"
cmake --build --preset win7-x64-release --target MikuMikuStudio legacy_mmd_menu_tests `
  legacy_observer_export_tests native_media_tests image_sequence_tests 2>&1 | Tee-Object "$job/build.log"
python -B tools/package-adaptive-ui-template.py --output build/packages/mms-adaptive-ui-template-candidate1
python -B tests/run_windows_abi_candidate.py --build build/cmake `
  --ui-template build/packages/mms-adaptive-ui-template-candidate1.zip --evidence "$job/checks"
```

`--build=*` 强制从锁定 recipe 重建依赖，以免下载的同一 `compiler.version=194` 库实际来自其他 MSVC 补丁版本。检查配置记录中的实际编译器和 SDK，再看 `checks/summary.json`。如果无法从官方远程构建依赖，可由已验证的同一 Conan 版本导出当前锁定的 Windows x64 `/MD` Release 缓存，在独立缓存 `conan cache restore` 后用 `--no-remote`（去掉 `-r conancenter` 和 `--build=*`）运行安装；不要拷贝旧 CMake 缓存。禁止用假 DLL、放宽 lock 或静默切换 MSVC 版本来换取通过。

## GitHub 托管与实际产品验收的边界

[GitHub `windows-2022` 清单](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)列出 VS 2022 和 SDK `10.0.19041.0`，但没有证实本案的 `cl.exe` 补丁版本或 `System32` D3DX 状态；镜像标签也不是版本冻结。Microsoft 的该运行库下载页没有把 Windows Server 2022 列为明确支持的操作系统。尚无授权使用经验证的预装镜像或在该服务安装运行库；本次不新增可能给出假绿灯的 workflow，也没有触发任何 CI。最小交接是提供一个已经具备上述工具和系统运行库的隔离 Windows x64 环境，再从构建前检查开始。若将来允许 CI，先在指定镜像查实这些前置条件，再独立报告包布局、菜单/观察者预检、WIC 发布及私有桌面/D3D 组件的结果；机器缺少某种能力时标记为未验证，不能算通过。

[Windows ABI 候选组件和产品验收](windows-abi-candidate-checks.md) 另列产品路径。托管结果不能替代实际 Shell、真实插件共存、可见界面、Win7 或 AMD 设备验收。可选的界面对照是在同一 Windows 11、3440×1440 的 100% 和 125% 下，使用相同窗口位置、主题、工程和时间轴目标分别测试冻结 main 与 ABI 候选，记录完整截图、目标位置和实际命中结果；它只记录既有行为，不引入新布局或新功能。
