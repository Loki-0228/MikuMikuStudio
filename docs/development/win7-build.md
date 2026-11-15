# Win7 x64 基础构建

`win7-x64` preset 固定 x64 MSVC 19.44.35229 和 Windows SDK 10.0.19041.0，使用 Win7 目标宏，并关闭可选 DX12 后端。`profiles/win7-x64.lock` 固定依赖 recipe；依赖的运行库部署和实际系统支持仍须单独验收。

从干净源码配置自己的构建目录：

```powershell
conan install . --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 --lockfile profiles/win7-x64.lock --output-folder build/conan --build=missing
cmake --preset win7-x64
cmake --build --preset win7-x64-release --target MikuMikuStudio
```

依赖已在本机缓存时可用 `--no-remote` 代替 `--build=missing`。不要复制其他源码工作目录的 CMake 缓存。检查 `CMAKE_HOME_DIRECTORY` 和 `CMAKE_INSTALL_PREFIX`，验证产物放在独立目录，验收前不覆盖 `build/Release`。

构建树位于 `build/cmake`，Conan 依赖和工具链位于 `build/conan/build/generators`；两处路径分别写进 `CMakeCache.txt` 和 preset，可以继续增量构建。搬动或重命名这两个目录后，先删除 `build/cmake/CMakeCache.txt` 和 `CMakeFiles`，再执行 `cmake --preset win7-x64`，让缓存按新路径重新生成。旧路径留下的 `.tlog` 目录仍会在 `*.dir\Release` 里，构建时报 MSB8028；把这些旧 `.tlog` 目录删掉再构建即可。

完整档在构建时会先用 D3DX9 预编译内嵌效果再生成资源（`MIKUMIKUSTUDIO_PRECOMPILE_EFFECTS` 默认开启）。本机实测 117 号效果瞬时完成，118 号约 5 分钟，120 号约 11 分钟，整轮约 16 分钟，期间没有输出，不要当成卡死中断。产物在 `build/cmake/generated/resources`，比资源输入新时该步骤会跳过。

`build/release` 是专用 ABI 发布预备目录。它包含主程序、随当前产品构建的兼容组件、默认扫描配置、许可文件和 GUI 源码模板。用 PowerShell 7 和 PATH 上的 Python 3.10+（`python` 命令）重建：

```powershell
./scripts/package_abi_release.ps1 -Force
```

脚本不编译、不安装到系统，也不发布。默认读取 `build/cmake`，可用 `-BuildDirectory` 指向当前源码的另一份完整档 x64 构建。下列各项都必须存在：

- 同一构建的 `Release/MikuMikuDance.exe`、`Release/MMDFacade.dll`、`Release/MMHack.dll`、`Release/PLUGINS/LEGACY/MMDPlugin.dll`。发布预备目录保留它们相对 `Release/` 的布局。缺文件时重新构建 `MikuMikuStudio` 目标；不要用旧插件 DLL 补齐。
- 源码中的根 `LICENSE`、`PLUGINS/config.json` 和 `licenses/` 中列入清单的许可文件。
- `mms-adaptive-ui-template.zip`。默认路径下缺少时，脚本调用项目的[模板打包器](../guides/cpp-adaptive-ui-template.md)；若 `-UiTemplateArchive` 指向另一个路径，须事先生成该源码 ZIP。

脚本校验四个产品文件的 x64 PE 头并打印全部打包文件的 SHA-256、字节数和相对路径。校验失败不替换既有目录；`-Force` 也不覆盖含工程、日志、插件或其他非打包文件的目录。构建树、PDB、开发插件、测试产物和验证证据不入包。

此脚本不调用 `cmake --install`，不带用户提供的原版 MMEffect、原版 MMHack、D3D9 启动代理、NVIDIA 辅助 DLL，也不从系统目录复制 VC/UCRT/OpenMP 或 DirectX 运行库。旧 MME 的用户补充步骤见[安装说明](../LEGACY_MME_PACKAGES.md#安装)，运行库与实际加载另按[部署审计](../reference/win7-deployment.md)验证。当前源码内置的 MMHack 移植来源和再分发条件仍须在对外发布前确认；本目录清单与 PE 头检查不表示已取得第三方许可或通过真实 Windows、Win7、AMD 或 MME 验收。

可用 `python -B tests/package_abi_release_tests.py -v` 在 Windows 或 Linux 中检验文件清单、校验值与失败保护。测试使用不可运行的 PE 文件头夹具，不生成或上传真正的发布物。

默认保留 Release PDB。只需验证且本机符号工具不可用时，可以在配置时使用 `-DMIKUMIKUSTUDIO_DEBUG_SYMBOLS=OFF`；发布时仍应保留匹配程序的符号文件，不能把旧 PDB 与新 EXE 配对。Release exe 内嵌 PDB 的绝对路径，搬动构建目录或重新链接后，同一份源码生成的 exe 哈希会变化，发布前要按实际文件重新核对。

最小档使用另一构建目录，并设置 `MIKUMIKUSTUDIO_PROFILE=mmd932`。它和完整档共用的日志、备份及效果接口应分别验证；曲线编辑器和现代媒体不属于最小档范围。

较新系统的媒体处理通过可选 COM 能力查询使用。高级视频处理失败时重新尝试基本 CPU 处理；扩展 Sink Writer 不可用时，通过旧接口查询当前流的编码器。兼容声明不引入新系统静态导入，也不提高 Win7 目标宏。

本机编译、CPU 路径及导入表检查不能代替 Win7 SP1 x64、AMD CPU/GPU 真机验收。运行库部署闭包和旧系统媒体编码器支持仍需实际验证。
