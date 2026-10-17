# 程序本地数据目录

程序生成的设置、状态与缓存统一保存在程序旁的 `data` 文件夹。`data` 与程序文件同级，不创建名为 `EXE` 的目录，也不随当前工作目录变化。

| 内容 | 相对程序目录的路径 |
| --- | --- |
| 自动备份设置 | `data/autosave.ini` |
| 默认自动备份、崩溃恢复与错误日志 | `data/backups/` |
| 抗锯齿设置 | `data/antialias.ini` |
| 运算器设置 | `data/processor.ini` |
| 操作柄位置 | `data/viewport.ini` |
| 最近工程记录 | `data/recent-pmm.txt` |
| ABI 插件布局 | `data/plugins/ui-layout.json` |
| ABI 插件布局锁 | `data/plugins/ui-layout.json.lock` |
| ABI 插件专用持久数据 | `data/plugins/<插件 ID>/` |

插件安装包仍放在 `PLUGINS`；该目录存放代码及随包资源。main 暂无 ABI 插件宿主，插件持久数据规则适用于 ABI 分支。

默认备份位置按当前程序所在目录解析，配置不保存上一次安装目录的绝对地址。整体搬迁程序与 `data` 后，默认位置跟随新目录；程序改名不影响设置。用户明确选择的自定义定时备份目录继续保留。崩溃恢复和错误日志固定在 `data/backups`。

不自动继承旧 AppData 设置，避免把另一份程序的绝对备份地址带入当前安装；不删除旧文件。设置无法写入时保留旧值或报告失败，不回退到 AppData。测试专用的显式路径覆盖接口保留。

Windows 上已有 `Data` 与 `data` 通常指向同一个目录，无需为了大小写搬迁或删除原有文件。代码中新建的本地设置目录使用小写 `data`。

## 验证

两个分支均通过 9 个独立进程目录用例，覆盖共享系统配置目录、安装隔离、中文程序改名、默认目录搬迁、自定义目录、写入失败及系统配置目录缺失。抗锯齿、运算器、操作柄与最近工程的实际读写也在这些用例中检查。

两个分支均通过完整自动备份测试：main 为 132 项，ABI 为 190 项；各通过 115 项运行时错误日志检查，覆盖崩溃、终止、并发写入及路径被文件占用的情况。ABI 另通过真实隐藏宿主的布局保存、插件缺失、中文改名恢复测试，确认布局和锁位于 `data/plugins`，且不生成 AppData 插件状态目录。

日志位于各工作区 `build/local-data-final-*.log`，目录隔离报告位于 `build/local-data-final-path-probe*/report.json`。这些是本机验证，不代表完成 Win7 或 AMD 真机验收。此次修改未覆盖 main 的 `build/Release/MikuMikuDance.exe`；已运行的旧进程须在之后启动新构建时才采用新路径。
