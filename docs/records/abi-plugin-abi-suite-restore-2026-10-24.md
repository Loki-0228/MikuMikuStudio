# 恢复 ABI 契约套件构建

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批修复 `plugin_abi_tests` 目标不可编译的既有问题，并重新跑通默认全量契约套件。

## 问题

- 提交 `084699a`（同步手动编号保存）给 `PublishProjectPair` 增加第 7 个参数 `numberedManualVersions`，并加入 `tests/plugins/storage_checks.inc` 的 `NumberedVersionStorageChecks`，断言编号保存时把旧版本链整体移位。
- 提交 `1cd0242`（同步自定义命名与增量保存）改变手动增量保存语义：每次手动保存写新文件、不移动旧版本，`PublishProjectPair` 恢复为 6 个参数。该提交没有删除仍按旧语义编写的 `NumberedVersionStorageChecks`。
- 结果：`plugin_abi_tests` 在 `storage_checks.inc` 报 `PublishProjectPair` 参数个数不符，套件无法重新构建；已有二进制来自更早提交，不能证明当前源码。

## 修复

删除过时的 `NumberedVersionStorageChecks` 及其调用。它覆盖的「手动编号保存移动旧版本链」行为已不存在：

- 当前手动增量保存由产品保存路径以 create-only 方式发布新工程对，旧版本保持不变；该行为由 `project_save_export_tests --numbered-only`（完整配置 62 项、原始配置 39 项）和 `autosave_tests` 覆盖。
- 插件工程对自身的保留链与恢复仍由同一文件里的 `VersionStorageChecks` 覆盖，使用的就是当前 6 参数接口。

未改动生产代码；`PublishProjectPair` 现有 6 参数行为保持不变。

## 验证

证据目录 `build/verification/abi-plugin-abi-suite-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `plugin_abi_tests` 构建 | 通过（含生命周期插件、编辑探针、hello 探针及全部检查源） |
| 默认全量契约套件 | 通过，`PASS checks=44752`，exit 0，23.2 秒；覆盖发现/加载器、生命周期、任务调度、UI 布局与刷新、存储与工程对、编辑历史、属性输入、DATA/STATE/LAYOUT/REPAINT 阶段、事件与依赖图等 |
| `business_event_tests` 复跑 | 通过，7 项（确认上一批事件通知在套件重建后仍正常） |

测试调用需要传入 hello 探针所在目录（目录内含 `hello_menu.dll` 与 `plugin.json`）、lifecycle DLL 路径与 `manager_edit_probe.dll` 路径。

## 未完成

- 完整套件里仍有若干未开启的独立 CTest 模式（面板/菜单 STATE、refresh/subscription runtime 等）本轮未逐一重跑；它们由默认套件内相应检查覆盖同类路径，但不等于逐模式证据。
- 上一批 P1 事件仍缺宿主侧来源：工程关闭、对象删除、语言切换、属性变更、命令状态、计算策略、设备代次/丢失恢复与 DPI 变化。
- 透明排序性能缓存、真实 MME/MMD 工程观测与外部设备验收仍未完成。

## 后续

下一步接入宿主侧事件来源，并在这些改动后再次运行默认全量契约套件。
