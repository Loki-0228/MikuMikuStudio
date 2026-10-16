# Win7 x64 基础构建

`win7-x64` preset 固定 x64 MSVC 19.44.35229 和 Windows SDK 10.0.19041.0，使用 Win7 目标宏，并关闭可选 DX12 后端。`profiles/win7-x64.lock` 固定依赖 recipe；依赖的运行库部署和实际系统支持仍须单独验收。

从干净源码配置自己的构建目录：

```powershell
conan install . --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 --lockfile profiles/win7-x64.lock --output-folder build/win7-x64 --build=missing
cmake --preset win7-x64
cmake --build --preset win7-x64-release --target MikuDanceStudio
```

依赖已在本机缓存时可用 `--no-remote` 代替 `--build=missing`。不要复制其他源码工作目录的 CMake 缓存。检查 `CMAKE_HOME_DIRECTORY` 和 `CMAKE_INSTALL_PREFIX`，验证产物放在独立目录，验收前不覆盖 `build/Release`。

默认保留 Release PDB。只需验证且本机符号工具不可用时，可以在配置时使用 `-DMIKUDANCESTUDIO_DEBUG_SYMBOLS=OFF`；发布时仍应保留匹配程序的符号文件，不能把旧 PDB 与新 EXE 配对。

最小档使用另一构建目录，并设置 `MIKUDANCESTUDIO_PROFILE=mmd932`。它和完整档共用的日志、备份及效果接口应分别验证；曲线编辑器和现代媒体不属于最小档范围。

较新系统的媒体处理通过可选 COM 能力查询使用。高级视频处理失败时重新尝试基本 CPU 处理；扩展 Sink Writer 不可用时，通过旧接口查询当前流的编码器。兼容声明不引入新系统静态导入，也不提高 Win7 目标宏。

本机编译、CPU 路径及导入表检查不能代替 Win7 SP1 x64、AMD CPU/GPU 真机验收。运行库部署闭包和旧系统媒体编码器支持仍需实际验证。
