# Win7 x64 基础构建

`win7-x64` preset 固定 x64 MSVC 19.44.35229 和 Windows SDK 10.0.19041.0，使用 Win7 目标宏，并关闭可选 DX12 后端。`profiles/win7-x64.lock` 固定依赖 recipe；依赖的运行库部署和实际系统支持仍须单独验收。

从干净源码配置自己的构建目录：

```powershell
conan install . --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 --lockfile profiles/win7-x64.lock --output-folder build/conan --build=missing
cmake --preset win7-x64
cmake --build --preset win7-x64-release --target MikuDanceStudio
```

依赖已在本机缓存时可用 `--no-remote` 代替 `--build=missing`。不要复制其他源码工作目录的 CMake 缓存。检查 `CMAKE_HOME_DIRECTORY` 和 `CMAKE_INSTALL_PREFIX`，验证产物放在独立目录，验收前不覆盖 `build/Release`。

构建树位于 `build/cmake`，Conan 依赖和工具链位于 `build/conan/build/generators`；两处路径分别写进 `CMakeCache.txt` 和 preset，可以继续增量构建。搬动或重命名这两个目录后，先删除 `build/cmake/CMakeCache.txt` 和 `CMakeFiles`，再执行 `cmake --preset win7-x64`，让缓存按新路径重新生成。旧路径留下的 `.tlog` 目录仍会在 `*.dir\Release` 里，构建时报 MSB8028；把这些旧 `.tlog` 目录删掉再构建即可。

完整档在构建时会先用 D3DX9 预编译内嵌效果再生成资源（`MIKUDANCESTUDIO_PRECOMPILE_EFFECTS` 默认开启）。本机实测 117 号效果瞬时完成，118 号约 5 分钟，120 号约 11 分钟，整轮约 16 分钟，期间没有输出，不要当成卡死中断。产物在 `build/cmake/generated/resources`，比资源输入新时该步骤会跳过。

`build/release` 是发布目录，只放 `MikuMikuDance.exe`、`licenses/` 和 `mms-adaptive-ui-template.zip`。用下面的脚本重建：

```powershell
./scripts/package_abi_release.ps1 -Force
```

脚本默认读取 `build/cmake`，可用 `-BuildDirectory` 指向另一份完整档 x64 构建。构建树、测试产物和 `build/verification` 中的日志、证据都不进发布目录。`cmake --install` 还会写入 `PLUGINS/config.json`，因此发布目录由脚本直接复制文件，不使用安装规则。

默认保留 Release PDB。只需验证且本机符号工具不可用时，可以在配置时使用 `-DMIKUDANCESTUDIO_DEBUG_SYMBOLS=OFF`；发布时仍应保留匹配程序的符号文件，不能把旧 PDB 与新 EXE 配对。Release exe 内嵌 PDB 的绝对路径，搬动构建目录或重新链接后，同一份源码生成的 exe 哈希会变化，发布前要按实际文件重新核对。

最小档使用另一构建目录，并设置 `MIKUDANCESTUDIO_PROFILE=mmd932`。它和完整档共用的日志、备份及效果接口应分别验证；曲线编辑器和现代媒体不属于最小档范围。

较新系统的媒体处理通过可选 COM 能力查询使用。高级视频处理失败时重新尝试基本 CPU 处理；扩展 Sink Writer 不可用时，通过旧接口查询当前流的编码器。兼容声明不引入新系统静态导入，也不提高 Win7 目标宏。

本机编译、CPU 路径及导入表检查不能代替 Win7 SP1 x64、AMD CPU/GPU 真机验收。运行库部署闭包和旧系统媒体编码器支持仍需实际验证。
