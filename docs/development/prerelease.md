# 整理发布包

使用 PowerShell 7、CMake 和已配置的 Windows x64 Release 构建目录。完整包的构建档为 `full`，最小包为 `mmd932`。两个目录的源码都必须指向当前仓库。

1. 检查工作区改动，运行相关测试。保留测试与基线日志，发布说明只写影响用户的使用限制。
2. 在[发布记录](../records/releases.md)补充三语说明，用 `release-notes:<版本号>:begin` 和 `release-notes:<版本号>:end` HTML 注释标记该版本的范围。
3. 提交源码和说明。打包脚本要求暂存区与工作区干净，本地日志、截图、模型和构建目录应由 `.git/info/exclude` 排除。
4. 运行打包脚本，将参数替换为实际路径。输出目录必须尚不存在。

```powershell
	./scripts/package_release.ps1 -Version v2026.10.15.1 -FullBuild D:/builds/mms-full -MinimalBuild D:/builds/mms-minimal -OutputDirectory D:/releases/v2026.10.15.1
```

正式版使用 `vYYYY.MM.DD.N`，预发布版在末尾加 `-pre.N`。脚本按版本号生成包内说明和 `Prerelease` 字段，也支持旧的 `prerelease-notes` 注释标记。

脚本重新构建两个档位，确认源码未变化，然后只复制程序、许可证和发布说明。许可证保留在 ZIP 内，不单独上传 LICENSE 或 SHA256SUMS。Release 附件只上传完整包、最小包及可选的 `MMSNvidiaAAFix.dll`；简短的三语安装方法写在 Release Notes 中。构建清单保留源码提交、文件哈希及可选组件信息，用于本地核对，不单独上传。不包含模型、设置、MME、MMV、FFmpeg 或调试符号。

5. 检查 ZIP 条目与包内清单，核对源码提交、构建档、程序及许可证的哈希。
6. 为通过检查的提交创建同名标签。推送提交与标签后，上传发布包。GitHub Release 的预发布标记须与版本号一致，正式版可设为最新发布。

打包脚本不推送提交、不创建 GitHub Release，也不修改现有安装。`Uploaded: false` 表示生成时尚未上传。

普通 `cmake --install` 不安装 NVIDIA 辅助 DLL。需要单独安装该 DLL 时，显式指定 `--component NvidiaAAFix`；这只复制文件，不执行驱动设置安装。
