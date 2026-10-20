# main 备份设置与日志修复同步

日期：2026-10-20。目标为 `codex/plugin-abi`，来源功能提交截止 `acbddb4`。核对时 main 最新提交为 `1b9acad`，该提交仅更新正式版发布文档。此前至 `9500a26` 的功能已在[第三批同步](abi-main-versions-sync-2026-10-20.md)完成。

## 同步范围

| main 提交 | ABI 版行为 |
| --- | --- |
| `d6eb91d` 跳过未修改空工程的定时备份 | 新建、无文件名、未修改且无模型或配件的工程不生成定时备份。相机或灯光修改、删除最后一个模型，以及已有文件名的空工程仍可备份。崩溃恢复不受空工程过滤影响。 |
| `acbddb4` 精简自动备份设置并统一日志目录 | 三语言对话框缩短说明，用提示补充 `.pmm1` 和 MME 配套文件的条件。安装目录内的备份路径显示、存储为相对 EXE 路径；运行时仍解析为绝对路径。错误日志写入所选备份目录。 |

设置继续集中写入 `data/settings.json`，不恢复常规 `autosave.ini` 写入。默认目录仍为实际 EXE 旁的 `data/backups`；安装目录内的自定义路径使用 `directoryMode: relative`，复制整个安装目录后随新的 EXE 解析。安装目录外的自定义路径保留绝对路径。旧本地 INI 仅用于一次迁移或测试覆盖。

启动阶段在创建设备和窗口之前读取设置，将已校验目录复制到日志模块的固定缓冲。崩溃处理只尝试获取共享锁；若设置更新正持有锁，则使用预备的默认目录，不等待故障线程。ABI 原有的日志环、线程上下文和内存访问故障诊断均保留。错误日志跟随自定义目录，紧急 PMM 恢复副本仍写入默认 `data/backups`。

仅适配 main 已提交的改动。main 工作区、稳定版程序和发布文档中的产物哈希均未复制或修改。普通 `mms.edit` 保持关闭，MMS-only 扩展标记与独立曲线修复工具的移除状态保持不变。

## 验证结果

证据目录为 `build/verification/abi-physics-audio-history-20261020/`。

| 检查 | 结果与日志 |
| --- | --- |
| 备份及真实设置对话框 | 296 项通过，含三语言控件、提示、取消、备份配对及失败原子性；`test-autosave-main-sync.log`。 |
| 空工程 | 30 项通过，新建工程备份数为 0，包含相机修改、最后模型删除、已命名空工程与紧急备份；`test-autosave-empty.log`。 |
| 设置可移植性 | 12 个真实进程用例通过，含独立安装、EXE 改名、自定义相对路径随目录复制、不同工作目录及一次 INI 迁移；`test-settings-relative.log`。 |
| 错误日志 | 446 项通过，覆盖 ABI 原有故障检查及安装目录内、外的自定义日志目录；`test-runtime-log.log`。 |
| 真实崩溃恢复 | 定时备份关闭时的未处理异常恢复通过；自定义错误日志目录与默认紧急 PMM 目录分别检查；`test-autosave-crash-custom.log`。 |
| 容量提醒 | 首进程 53 项、重启进程 49 项通过；空工程启动也能重新提示已满目录；`test-autosave-full-warning.log`、`test-autosave-full-restart.log`。 |
| 构建 | 完整及原始兼容配置 x64 Release 成功；`build-integrated-final.log`、`build-original-integrated.log`。 |
| 真实产品 | 原名与中文改名的隔离程序均通过隐藏启动、菜单、插件工具栏和侧栏实时布局、关闭；`test-product-final.log`。 |

Codex 查看了 `autosave-main-sync/backup-dialog-{0,1,2}.bmp` 的日文、英文、中文对话框，标签及按钮无裁切。看图和文档未交给 DSH。

本批整合中的物理、音频与属性值模块另有[独立记录](abi-physics-audio-history-2026-10-20.md)。完整原生历史套件停在既有断言 `morph Enter starts with one original ANSI EDIT on the current thread`；当前程序和保存的旧测试程序均复现。未削弱断言，也未登记为全套通过。

Win7 SP1 x64、CPU 基础路径与 AMD 继续作为兼容要求。SDK 10.0.19041 和两种配置构建不代替 Win7 或 AMD 真机验收。

## ABI 程序

ABI 工作区的 `build/release/MikuMikuDance.exe` 更新为本批验证的完整配置程序，大小 8,970,752 字节，SHA-256 为 `577954F6A0EFA9F640B5A290B6AC063040154935A63A719DE5711797594492D4`。此前程序保存在本批证据目录的 `previous-abi-MikuMikuDance.exe`。main 稳定版产物未更新。
