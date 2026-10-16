# 工程序列化接口参考

本页对应 `storage.h` 的 ABI 1.0。所有结构按 8 字节对齐，调用约定为 `MMS_CALL`。插件只链接自身依赖，不链接 MMS 的导入库。

## 注册

`host.registerProvider` 的 service 为 `mms.serializer`，providerId 必须等于 `create` 收到的 pluginId。传入 `MmsSerializerV1` 和完整字节数。宿主复制函数表，context 与回调代码由插件保持有效，直到成功注销或宿主完成退休。

一个 pluginId 重复注册返回 CONFLICT。错误所有者、缺失回调、错误表尺寸返回 ARGUMENT；非 UI 线程返回 THREAD；加载候选已准备或正在执行序列化回调时返回 BUSY。未知提供者类别返回 UNSUPPORTED。

注册会立即调用 `prepareLoad` 和 `commitLoad`：有保留记录时恢复该记录，否则加载默认状态。注册成功后才返回提供者句柄。初始化不能依赖该句柄已经写入输出变量。失败的 `prepareLoad` 由插件清理自己的未发布候选，宿主不调用其 commit 或 cancel。

## `MmsSerializerV1`

结构大小 56 字节，h 位于偏移 0，context 位于偏移 8。

| 偏移 | 回调 | 契约 |
| --- | --- | --- |
| 16 | `prepareLoad(context, bytes, schema, outCandidate)` | 校验 schema 和数据，分配全部资源，准备候选；成功返回 OK。不得修改当前工程状态。bytes 仅在本次调用期间有效。 |
| 24 | `commitLoad(context, candidate)` | 无分配、无失败地发布候选；消费 candidate。禁止抛异常。 |
| 32 | `cancelLoad(context, candidate)` | 释放候选；消费 candidate；不修改当前状态。禁止抛异常。 |
| 40 | `save(context, outBytes, outSchema, outLease)` | 返回当前工程数据。schema 必须非零；成功后 bytes 保持有效，直到 releaseBytes。失败不发布输出，插件自行清理。 |
| 48 | `releaseBytes(context, lease)` | 释放 save 返回的借用数据。宿主在复制后调用；即使复制或分配失败，成功 save 的租约也释放一次。 |

schema=0 且 bytes.bytes=0 表示新工程或该插件没有保存数据。默认状态不需要文件格式版本。空的有效记录仍使用非零 schema，可以返回零长度数据。

全部提供者 prepare 成功后才开始 commit；其中一个失败，宿主按相反顺序 cancel 已成功的候选。commit/cancel 的上下文只属于这一次加载，不可长期保留候选句柄。项目解析失败或用户取消加载也会取消候选。原生 PMM 加载器自身的失败恢复仍由本体处理。

callback 内不可注册、注销、保存、打开或新建工程，不可运行消息循环。所有函数均在 UI 线程执行。违反无异常 commit/cancel/release 契约时，宿主停止继续使用该存储实例并保留 DLL，防止已经发布的回调指向卸载代码。

单插件原始数据上限 64 MiB；整个 JSON 扩展文件上限 96 MiB；每个工程最多 4096 条插件记录。超限拒绝保存，保留原工程。插件应将可重建的大型缓存放入缓存目录，不放入序列化数据。

## `MmsStorageApiV1`

`queryService("mms.storage", 1, 0, sizeof(MmsStorageApiV1), &api)` 返回 32 字节函数表。

| 偏移 | 函数 | 契约 |
| --- | --- | --- |
| 16 | `readSaved(context, outBytes, outSchema, outLease)` | 读取本插件最近一次成功加载或手动保存的数据。不是当前实时参数。无记录返回 NOT_FOUND，失败不改输出。 |
| 24 | `releaseSaved(context, lease)` | 释放只读副本。重复释放或其他插件的句柄不产生影响。 |

两项操作仅允许 UI 线程。宿主复制数据并持有租约，工程替换不使既有租约失效。每个 storage 实例最多 4096 个活动租约；停止中的插件不能获取新租约，但仍可释放。未排空租约时不卸载其 DLL。

## 文件与恢复

主文件继续为 PMM，插件文件为 `<完整PMM文件名>.mms-plugins.json`，例如 `dance.pmm.mms-plugins.json`。顶层字段为 schemaVersion=1、projectId、saveId、mainSha256 和 plugins；plugins 以 pluginId 为键，记录含 schemaVersion 与 Base64 编码的 data。Base64 不加密数据。未知插件的 schema 和原始字节经过读取、再次保存后不变。

projectId 标识工程，另存为和自动备份保持该 ID；每次保存生成新的 saveId。两个 ID 在 JSON 中为 32 个小写十六进制字符。主文件哈希为 64 字符 SHA-256。复制工程时须同时复制 PMM 和扩展文件；不配套的文件会在原生工程被替换前拒绝加载，扩展原件保留。显式重新关联界面尚未实现，不能自动套用不匹配的数据。

保存器先收集全部序列化数据，再在目标目录写完整暂存 PMM 和 JSON，刷新到磁盘并保留旧文件对。随后写入 `<PMM>.mms-save.json` 恢复记录，逐个替换并核对新文件哈希。只有文件对完整时才移除恢复记录。新数据不完整时恢复旧文件对；发现外部改写或无法校验的文件时保留恢复材料并拒绝覆盖。

保存失败会立即尝试恢复。再次打开或保存该项目前也检查恢复记录，包括启动时打开的项目。没有恢复记录的工程可以从只读目录打开。自动备份沿用同一保存协议；轮换时只删除已确认归属该 PMM 的扩展文件。

恢复测试覆盖各文件替换边界、损坏的新扩展文件和不匹配的既有文件。存储介质掉电后的真实刷新保障仍取决于系统和设备，不由进程内故障注入测试证明。

## 示例和验证入口

[生命周期测试插件](../../../tests/plugins/lifecycle_plugin.cpp) 的 `RegisterSerializer` 是可编译的最小整数状态示例；它经过独立 DLL 加载、真实 PMM 保存重开和中文 EXE 改名测试。生产插件应将该整数编码替换为自己的明确字节格式，不直接保存含指针的结构。

`tests/plugins/run_storage_probe.py` 驱动隐藏宿主验证菜单修改、save/release 配对、prepare/commit、缺失插件保留、保存失败、保存事件和事件内场景查询。`tests/plugins/storage_checks.inc` 覆盖候选取消、所有权、线程、租约、Base64 和成对文件恢复。
