# MMS 相机修复同步到 ABI 分支

同步检查时间为 2026-10-17 23:38（UTC+8）。来源是 `D:/MMD-Next/MMS` 的 `main`，目标是 `D:/MMD-Next/MMS-abi-dev` 的 `codex/plugin-abi`，目标基准为 `7b3c990`。本次没有修改来源目录、稳定版程序或发布资产。

## 来源与边界

来源任务名称为「排查 MMS 相机崩溃来源」，任务 ID 为 `01a13efe-8a8b-7ce1-8939-66ef24858d7b`。来源当前提交为 `fae70331f7832067402e59fc349ce6ba0b10c32c`；本批相机补丁仍是该提交之上的未提交修改。先核对文件哈希，再保存差异，再次核对哈希；没有直接合并整个 `main` 或复制覆盖 ABI 宿主实现。

此前的精确键值、原生相机插值、相邻一帧保持和 PMM 往返修复来自 `04d3decc53cf0489249da0c39321ad7b41d6bc97`，已经通过 `6ce1444082d75a58bc21cb98b8e5cbc817de5790` 合并进入 ABI 分支。本批没有重复移入旧历史实现。

## 已迁移内容

| 故障 | 同步后的行为 |
| --- | --- |
| 在已有后续关键帧时重新注册第 0 帧，会产生第二个零帧节点；下次分配复用该节点后丢失中间链表。 | 通用全局轨道注册从永久零帧头结点开始比较，相同帧直接替换原记录。覆盖相机、灯光、自阴影和重力。 |
| 曲线写入把整条全局轨道标为选中。 | 重建记录时按原帧号恢复选区；编辑器完成写入和撤销后重新发布当前选区，覆盖骨骼和表情宿主写入产生的选择标记。 |
| 曲线窗口中的 Enter、Delete、Tab 同时被主窗口轮询处理。 | 编辑器本体及子控件拥有独立键盘输入，主窗口轮询和单独快捷键入口先检查焦点所属窗口。原始界面档提供对应空实现。 |
| 曲线草稿或本地撤销覆盖主界面新注册、移动、删除或修改的关键帧。 | 每次写入、撤销和重做前读取实际源；发现外部修改先刷新并拒绝旧草稿。确认写入后再读回宿主接受的规范值，避免量化误差使自己的下一次撤销被误判为外部修改。 |
| 主界面只改变选区后，立即回曲线窗口删除会沿用旧选区。 | 读取源后和执行删除、插值选择、参数应用前同步原生选区；参数应用发现选择变化时要求重新确认字段，避免把旧字段写进新选区。 |
| MMD 手柄拖动期间异步刷新重建当前编辑区。 | 将源刷新延后到拖动结束，并在提交前再次核对源。 |

保留 ABI 分支已有的受控门禁、场景对象身份、作者状态及共同历史路由。帧线、删键和时间轴鼠标选区补充通知均在原入口完成后触发；受控帧线和删键路径也已有各自通知。

ABI 另补一项异常保护：`NotifyOwnedChanged` 可能从命令分派的析构函数中调用。该函数现在为 `noexcept`，分配失败仅延后界面刷新；写入前仍直接读取源检查，防止内存不足导致已完成命令在栈展开时终止进程。增加实际分配失败回归，不把异常传播到析构函数。

## 冻结快照

本批初始补丁的 SHA-256 为 `AC800800D6F381534482EA75585A1B7394120AFC61E72EA491DA13F142F5B84E`。本地证据保存在 `build/mms-camera-source-snapshot-20261017/`；该目录不作为发布内容。

以下是实际迁移使用的来源文件 SHA-256。编辑器测试随后新增的独立回归在核对两分支基准 blob 相同后单独冻结同步；其余实现保留初始快照。

| 来源文件 | SHA-256 |
| --- | --- |
| `include/mikudancestudio/animation_curve_editor.hpp` | `641D1ADD52E0C379916743768E0D3E5969F39EFBBCB4E8336FA7AE6332C354A9` |
| `src/animation/animation_curve_host.cpp` | `D1786C56B34C51CD803AB8D1D52F927747C7316615CA36B1D4B49C3BF8F93D0B` |
| `src/app/pump_edit_keys.cpp` | `E64185B21C210ABED117A36236152C00A034144A34EC0562640BBF53FFCE9E8B` |
| `src/window/animation_curve_editor.cpp` | `6E9A294FDEC022576472D6F010D0640BA855BA6C939D1C57D1429E82E953A613` |
| `src/window/animation_ui_original.cpp` | `84C2E192BEF70BB2B964A3FFEDEA94E1FF9B7430537A0047A50EF5F8302AE2A3` |
| `src/window/command_control_400.cpp` | `18F538E350FE47A0286E84998A3781F7FF2F1813A196C0A675A15CDD72E61EDC` |
| `src/window/frame_line_edit.cpp` | `B7F33784930A3CBB83B57DE92226B14755DC773F369CEE1B85D234781415FADA` |
| `src/window/ui_mousemove.cpp` | `FE36C6E43757984127E37437F0184830A8A00F2A5F7AC29914037227313AED8C` |
| `tests/animation_curve_editor_tests.cpp` | `1E966DAB3812F4BED75475B3E2BBF792B0148EB86502D6405E9C1A8E3CE737ED` |
| `tests/curve_sync_tests.cpp` | `4DE330FD89FCDE3C049935CEB2CFA7F76F33890D5441F1F985939A8E110B2CE4` |

## 外部相机文件验证入口

原始输入为 `D:/MMD-Next/carp cam 2.vmd`，379 字节，SHA-256 为 `A142339652C42D89A7BF4806F9EC786AEFE6B5AAE7F6C2BF96DC600DA00C880A`。来源分析报告为 `D:/MMD-Next/MMS/.local/artifacts/carp-camera-20261017/vmd-data-analysis.json`。该文件含 0、44、61、92、93 五个相机关键帧；61、92 的时间控制点存在交叉，但数值合法，时间导数为正。名称终止符后有非零填充；相机名比较只取名称及终止符，不能比较全部填充字节。

新增 `tests/plugins/camera_crash_vmd_host_checks.inc`，由 `MMS_CAMERA_CRASH_VMD` 显式提供输入。它只在隔离测试目录创建和删除自己的文件副本，原文件不改动、不纳入仓库。覆盖两种历史模式下的受控导入、44 帧 X 精确为 0、92 至 93 的相邻一帧保持、源副本删除后的跨模式重做及插件历史交错。

求值使用 `ReloadModels` 的相机专用实现（`src/app/timeline_advance.cpp`），该函数只读取相机轨道、应用相机曲线并同步视角与投影，不求值模型或物理。测试恢复相机姿态、FOV、附着对象、游标、模式、播放头、末帧和历史设置；采样不创建历史节点。

## 本批未迁移内容

- `fae7033` 的独立曲线修复插件、启动器、发布记录，以及针对旧 `autosave.ini` 的序列化调整不属于本批相机修复；ABI 已使用统一设置存储，不用整提交覆盖。
- 23:38 再次核对时，来源正在新增稀疏曲线通道注册修复：`ReconcileRegisteredFrame`、带帧号的 `NotifyRegistered` 及相机、附件、重力、自阴影调用点。它会修改曲线作者数据，不能直接替换 ABI 的提交后通知，否则作者修改可能不在共同历史事务内。该追加修改不在上表快照中，须待来源完成后分别迁移旧注册入口及受控计划，并补原子撤销回归。

## 验收状态

ABI 产品与测试目标编译通过，日志为 `build/abi-camera-sync-scale-wheel-build-4.log`。12 组相关 CTest 全部通过，含曲线编辑器、曲线宿主、同步、键盘、面板、缩放编解码和共同历史。原生历史累计 137,897 项、ABI 契约 1,717 项、统一设置 70 项，日志为 `build/abi-camera-sync-scale-wheel-regressions-1.log`。

原名 PMX 与中文改名 PMD 的独立真实宿主均 exit 0。运行时显式设置 `MMS_CAMERA_CRASH_VMD=D:/MMD-Next/carp cam 2.vmd`，两种记录模式下的 44 帧精确值、92/93 相邻帧、源副本删除后重做及插件交错均通过。结果为 `build/abi-camera-sync-scale-wheel-history-host-1/report.json`，逐项输出为 `build/abi-camera-sync-scale-wheel-host-1.log`。验收后原文件 SHA-256 与上表一致。

以上是 ABI 分支的本机结果，不用源任务的构建或测试替代。没有覆盖 Win7、AMD 真机和人工桌面交互验收。
