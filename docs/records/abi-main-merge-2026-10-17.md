# ABI 分支合入 main 功能记录

本次在独立 ABI 工作目录中合入 main 已审查、验证的功能。合并前 ABI 基线为 `8cb6c1d`，合入的 main 提交为 `04d3dec`。本文记录合并范围及本轮验证；main 的既有结果见 [main 源码审查与本地验收记录](main-source-review-2026-10-17.md)，不能代替合并后的验证。

## 源码和输出目录

| 项目 | 本次取值 |
| --- | --- |
| ABI 源码 | `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。 |
| main 源码 | `D:/MMD-Next/MMS`，提交 `04d3dec`。本次合并不修改该目录。 |
| ABI 构建目录 | `D:/MMD-Next/MMS-abi-dev/build/win7-x64/build`。 |
| 安装前缀 | `D:/MMD-Next/MMS-abi-dev/build/win7-x64/build/Release`。 |
| 稳定程序目录 | `D:/MMD-Next/MMS/build/Release`，不作为本次构建或安装目标。 |

`CMakeCache.txt` 的 `CMAKE_HOME_DIRECTORY`、`CMAKE_TOOLCHAIN_FILE` 和 `CMAKE_INSTALL_PREFIX` 均指向 ABI 工作目录。preset 的 `binaryDir` 使用 `${sourceDir}`，各目标输出使用 `${CMAKE_BINARY_DIR}`。本次整合没有发布程序。

## 合并范围与保留行为

合入内容包括定时备份与崩溃备份的目录和容量处理、致命错误日志落盘、日志并发读取保护、曲线编辑和宿主回归、MME 默认设置及对应测试。两处冲突已解决并验证：

- 自动备份继续调用 ABI 的 `DeletePluginBackup` 清理 PMM 及匹配的插件扩展文件，同时用 main 的 `RefreshCapacity` 重新检查容量。
- 曲线宿主测试按实际底栏坐标定位控件，保留 ABI 的附件作者状态保留、内存不足和无分配发布测试，并加入 main 的相机精确键值、相邻单帧保持及 Step 回归。

新增备份测试使用两份未安装插件的真实记录，覆盖含 NUL 和高位字节的二进制数据、空数据、格式版本及工程身份。默认设置、开启和关闭定时备份三种真实异常路径，均重新载入崩溃备份并保存副本，检查插件数据已恢复到当前工程。轮转测试检查旧备份和新备份失败清理、手动文件及其插件扩展文件保护。

普通宿主的 `mms.edit` 门禁未改。`Initialize` 仍调用未启用编辑服务的初始化路径；只有内部隔离测试入口显式附加 `EditHost` 和原生编辑绑定。可写 UI 属性仍未开放。本次合并不表示剩余原生写入已迁入共同历史，范围见 [编辑整合记录](../PLUGIN_EDIT_INTEGRATION.md)。

非曲线、非备份部分的只读审查结果如下。

| 部分 | 合并后的实现与边界 |
| --- | --- |
| CMake | 新增 `runtime_log_tests`，测试目标单独定义 `MMS_RUNTIME_LOG_TESTS=1`。该定义不进入正式程序。ABI 目标和插件加载配置保留。 |
| Win7 构建 | preset、Conan profile 和 lockfile 保留 x64、C++17、锁定工具链及 CPU 基线。当前缓存为 `full`、Win7 基线开启、DX12 关闭。 |
| 故障日志 | 日志环的读写均先尝试占用槽位，竞争时跳过，不等待故障线程。快照在释放槽位后写入文件。致命错误报告仍先落盘，再调用恢复回调并追加恢复结果。 |
| 媒体兼容 | 可选 COM 接口声明、视频基本处理回退和旧 Sink Writer 查询路径与合入的 main 相同。声明本身不增加新版系统静态导入。 |
| MME 设置 | 仅当实际 EXE 旁存在 `MMEffect.dll` 且设置缺失时，补写 `SkipValidation=true` 和 `EMMAutoSave=true`。已有设置保留。PMM 打开通知与保存通知仍保留，不能将其描述为所有效果均须手动载入。 |

上述静态审查未发现新的合并阻塞。运行结果以下表为准。

## 本轮验证

本地证据路径相对于 `D:/MMD-Next/MMS-abi-dev`。本节只记录本次合并后的执行结果，不沿用旧构建的通过计数。

| 验证 | 当前状态 | 证据 |
| --- | --- | --- |
| CMake 配置与生成 | 已完成，生成目录为独立 ABI 构建目录。 | `build/abi-main-merge-configure.log`。 |
| 正式程序及核心构建 | 通过。 | `build/abi-main-merge-build.log`。 |
| 回归测试目标构建 | 通过。 | `build/abi-main-merge-test-build.log`、`build/abi-main-merge-autosave-build.log`、`build/abi-main-merge-runtime-build.log`。 |
| 故障日志 | 115 项检查通过，包括占用日志槽时的故障恢复。 | `build/abi-main-merge-runtime-log/report.json`、同目录 `result.log`。 |
| 备份、真实异常恢复、容量提示和启动视角 | 15 个进程全部通过。备份 189 项，容量提示 510 项，启动视角 3 项；三个异常恢复进程不输出检查总数。 | `build/abi-main-merge-tests-1792191881116051700/report.json`、同目录 `verification.json`。 |
| 曲线编辑器及宿主 | 编辑器 2,000 项通过；宿主全部通过，未输出检查总数。 | `build/abi-main-merge-curve-editor/result.log`、`build/abi-main-merge-curve-host/result.log`。 |
| 原生 CPU 相机保存重开 | VMD 与四次独立 PMM 重开均通过，每次 477 个状态采样；四份 PMM 与 VMD 的 1,908 组采样比较一致。 | `build/abi-main-merge-camera/run-1792191851842839900/verification.json`。 |
| 真实 MME | 英文及中文界面与路径，各含无配置、缺少设置、已保存关闭三种状态，每种运行两次；12 次全部通过。 | `build/abi-main-merge-mme/report.json`。 |
| ABI 契约 | 1,523 项通过。 | `build/abi-main-merge-contract-local-temp.log`、`build/win7-x64/build/Testing/Temporary/LastTest.log`。 |
| 原生共同历史 | 92,273 项通过。 | `build/abi-main-merge-native-history/result.log`、同目录 `report.json`。 |
| 真实插件与原生全局登记、共同历史 | PMX 原名及 PMD 中文改名均通过，包括修订号、取消、工程重开及退出。 | `build/abi-main-merge-global-history-final/report.json`、`build/abi-main-merge-edit-history-final/report.json`。 |
| 真实插件的工程保存重开 | 原名及中文改名均通过；序列化失败不发布部分工程。 | `build/abi-main-merge-storage/report.json`。 |
| 普通宿主启动与界面 | 原名及中文改名均通过菜单、工具栏、面板、关闭检查，测试窗口保持隐藏。 | `build/abi-main-merge-smoke.log`。 |
| 媒体读取与写入 | 本机音频、H.264 视频、定位及 WIC PNG/MOV 回归通过。 | `build/abi-main-merge-media.log`。 |
| 导入表 | 6 个 EXE/DLL 均为 x64，无 DX12、DXGI、d3dcompiler_47、CUDA 或 NVAPI 直接或延迟导入。 | `build/abi-main-merge-pe.json`。 |

正式程序先按正常流程构建。资源输入没有变化，构建前后两份资源的 SHA256 一致，见 `build/abi-main-merge-resource-before.json` 和 `build/abi-main-merge-resource-after.json`。后续测试构建复用这些资源，用本次 MSBuild 参数 `CustomBuildToolBeforeTargets=`、`CustomBuildToolAfterTargets=` 避免重复生成，测试源码仍正常编译和链接。

首次契约测试因沙箱临时目录中的配置保存失败，改用 ABI 工作目录下的 `build/temp` 后完整通过。两次历史探针的首次调用给错了测试 EXE 文件名，未启动程序；修正为 `sidebar_resize_tests.exe` 后的完整结果位于上表 `*-final` 目录。早期失败日志保留，不作为验证结果。

本轮 ABI 程序 SHA256 为 `8C9BB0295E545F90385DCD1CD0503615553B6AC475BDAD45F2EEEFC8CEAB3890`。main 保持 `04d3dec`，工作树无修改；稳定程序 SHA256 仍为 `78FF02CA8F61122C00661E9892E1C7A473B4BFCBCB5CBAF3AFA2EC6F614A79F1`。

## 已有验收入口

下列入口可用于本轮验收，列出入口不表示已执行。

| 入口 | 覆盖范围与调用条件 |
| --- | --- |
| `runtime_log_tests <新输出目录>` | 独立子进程的实际故障、并发日志、占用槽位时崩溃及恢复回调继续写入。该目标未注册到 CTest，须单独运行。 |
| `native_media_tests <媒体夹具目录>` | 原生音频、视频、定位及 WIC MOV 写入。夹具由 `tests/make_native_media_fixtures.ps1` 生成。该目标不包含 `mf_h264_recorder.cpp`，不能证明旧 Sink Writer 回退已通过。 |
| CTest `plugin_abi_contract` | 服务表、插件生命周期、事务互斥、取消及历史故障回归。 |
| CTest `effect_api_regressions` | 效果导出接口和无 MME 时的安全行为。真实 MME 默认设置仍需下项验证。 |
| `scene_lifecycle_mme_tests` | 独立目录中的真实 MME，使用 `--emm-default-on`、`--emm-default-off`、`--emm-save-on`、`--emm-save-off` 核对缺省设置、已有关闭设置、保存及重开。 |
| `build/run_abi_private_desktop.py` | 将指定测试 EXE 复制到新目录，在独立桌面运行；可用于原生共同历史测试。总限时为 180 秒。 |
| `tests/plugins/run_edit_history_probe.py`、`run_global_registration_probe.py`、`run_private_import_probe.py` | 接收 `sidebar_resize_tests`、真实编辑测试 DLL 和新输出目录，验证受控宿主的混合历史。 |
| `tests/plugins/host_smoke.py` | 正式 EXE 加生命周期测试 DLL，在独立目录验证原名和中文改名启动、界面及关闭。 |
| `tests/plugins/pe_audit.py` | 记录 x64 PE、子系统、导入、导出及可选图形后端静态依赖。 |

## 验证限制

当前缓存关闭 Release PDB 生成，仅用于本机验证。该设置不适合作为已备齐发布符号的证据。

ABI 原有的 `DeletePluginBackup` 先删除 PMM；若扩展文件随后被外部程序单独锁住，仍可能留下孤立的扩展文件。本次保留该既有行为，测试覆盖正常成对清理和 PMM 删除被阻止时的回退，不将文件系统的两次删除描述为原子操作。

Win7 SP1 x64、AMD CPU 和 GPU 真机仍未验收。旧系统媒体编码器及运行库部署闭包也未验收。本机编译、CPU 路径或导入表检查不能替代这些结果。
