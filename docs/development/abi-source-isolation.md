# 隔离 ABI 开发与稳定版构建

ABI 开发使用独立 Git 工作目录。构建目录和源码目录都必须独立；仅更换 EXE 输出位置不能隔离公共源文件的修改。

## 本机目录

| 用途 | 路径及状态 |
| --- | --- |
| 后续 ABI 开发 | `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，从 `eb7f711` 完整迁移。现从 ABI 基线 `8cb6c1d` 合入 main 提交 `04d3dec` 的已验证功能，合并后验证单独记录。临时分支名 `codex/plugin-abi-isolated` 已删除。 |
| ABI 构建输出 | `D:/MMD-Next/MMS-abi-dev/build/cmake`；Conan 依赖和工具链在 `D:/MMD-Next/MMS-abi-dev/build/conan`。 |
| ABI 发布目录 | `D:/MMD-Next/MMS-abi-dev/build/release`，只放程序、`licenses` 和 GUI 模块压缩包，由 `scripts/package_abi_release.ps1` 重建。 |
| 稳定版工作目录 | `D:/MMD-Next/MMS`，分支 `main`，当前提交 `04d3dec`。分离时基线为 `42d4a71`（`v2026.10.15.2`）；恢复的稳定版修改已完成审查、本机验证和提交。 |
| 稳定版验证输出 | `D:/MMD-Next/MMS/build/main-review/build`，独立于现有稳定版产物。 |
| 现有稳定版产物 | `D:/MMD-Next/MMS/build/Release`。本次开发不向该目录安装或复制程序。 |

分离前，原目录的两套构建都读取同一份 ABI 开发源码。分离后，ABI 构建只读取独立目录；原目录恢复为 `main` 源码。旧构建缓存没有作为新验证构建的起点。

main 的已验证功能通过 Git 合并进入 ABI 分支，两份源码和构建目录继续分离。合并基线、冲突处理及本轮结果见 [ABI 分支合入 main 功能记录](../records/abi-main-merge-2026-10-17.md)。普通宿主的 `mms.edit` 门禁保持关闭，内部受控验证不等于正式开放。

## 构建开发版本

在独立 ABI 工作目录中，按 `profiles/win7-x64` 和 `profiles/win7-x64.lock` 生成该目录自己的 Conan 依赖配置，然后使用 `win7-x64` CMake preset。

```powershell
	Set-Location D:/MMD-Next/MMS-abi-dev
	conan install . --profile:host profiles/win7-x64 --profile:build profiles/win7-x64 --lockfile profiles/win7-x64.lock --output-folder build/conan --no-remote
	cmake --preset win7-x64
	cmake --build --preset win7-x64-release --target MikuDanceStudio plugin_abi_tests
```

`--no-remote` 适用于依赖已在本机缓存的环境；缺少依赖时先按项目构建指南准备锁定版本。测试产生的工程、配置、插件和日志放在独立目录的 `build/` 内。

检查生成的 `CMakeCache.txt`：`CMAKE_HOME_DIRECTORY` 必须等于独立源码目录，安装前缀及测试路径不得指向稳定版产物目录。不要复制旧 `CMakeCache.txt` 或旧生成工程来创建新构建。

## 构建稳定版本

从确认的发布提交建立另一份独立工作目录，并在其中重新配置构建。不要把当前 ABI 分支或带有其他未提交修改的原工作目录用作稳定版发布源。

ABI 开发需要修改公共文件时，只修改 ABI 工作目录中的文件。稳定版需要同一项修复时，将对应的独立提交移入稳定分支，在稳定版工作目录重新审查和验证；不要批量复制整个公共文件。

分离前的 24 个已修改或未跟踪文件已逐个复制并核对 SHA-256，同时保留 Git stash。备份位于 ABI 工作目录的 `build/source-migration-before/`，其中记录原提交、stash ID 和文件清单。恢复到 `main` 的修改排除 ABI 文档；自动保存的分支差异按 `main` 现有接口解决，不能把 ABI 的插件备份函数带入稳定版。

旧发布元数据与现有 EXE 不一致，不能据此声称现有 EXE 精确对应 `42d4a71`。`main` 的源码基线和本次验证记录与现有程序文件分开记录。

`04d3dec` 的本机验收记录见 [main 源码审查与本地验收记录](../records/main-source-review-2026-10-17.md)。该提交未替换现有稳定版 EXE，也未发布新程序；ABI 合并后的结果另行验收。

## 每批审查与验证

- 检查普通宿主的原有入口、受控新入口和失败路径，防止失败后再次执行旧命令。
- 检查准备失败、取消、重入、资源释放和陈旧引用；失败不得留下部分作者数据或移动历史游标。
- 检查撤销与重做的数据、当前界面读数、播放头、未涉及对象及用户当前选择。
- 对确认的问题添加能触发故障的回归用例，再运行相关契约测试及隔离宿主测试。
- 提交前复核差异及构建来源，记录实际执行的验证。静态兼容检查不代替 Win7 或 AMD 真机验收。

统一 UI、视口及编辑接口的实现范围见[ABI 实现记录](../PLUGIN_ABI_IMPLEMENTATION.md)。
