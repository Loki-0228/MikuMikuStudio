# 插件全局设置：mms.settings 1.0

完整增强配置提供可选的 [C 接口](include/mms/settings.h)。在插件清单的
`capabilities` 数组中添加 `"settings.global"`，通过 `queryService` 请求
`mms.settings` 1.0。建议把历史版本兼容所需的默认值留在插件中；只有必须使用
该服务的插件才应将其加入 `requiresServices`。未声明该能力或宿主未提供该服务时，
查询返回 `MMS_E_NOT_FOUND`。

宿主在 EXE 同目录下的 `data/settings.json` 管理设置，在 `sections.pluginSettings`
下为每个清单 ID 保存一个独立 JSON 对象。ID 按原样作为一个键，点号不是路径分隔符。
接口不接受插件 ID 或路径，也不读写本体其他设置。不能向其他插件传递服务表或 context。
这些限制约束 ABI 调用；原生 DLL 仍在宿主进程中运行，此服务不提供沙箱。
设置用于安装级的普通偏好。MMS 不为文件提供加密，请勿存入密码、访问令牌或其他秘密。
宿主不会把设置内容写入日志。设置不随 PMM 工程打开、关闭或保存，不通过
`mms.storage` 或 `mms.serializer`。

- `read` 在 UI 线程的 `create`、`start` 或本插件普通 `enqueueUi` 执行回调中调用。
  传入空指针和容量 0 可查询当前字节数，再用自己的缓冲区复制 UTF-8 JSON，不含终止符。
  两次调用之间可能有更新。短缓冲区返回 `MMS_E_MEMORY`，只更新所需长度；长度输出不能与
  缓冲区重叠。没有对象返回 `MMS_E_NOT_FOUND`（不同于已存在的空对象）；其他失败不写缓冲区或长度。
- `compareAndMerge` 只能在本插件普通 `enqueueUi` 的 run 回调中调用。不能从
  cancel、停止、事件、渲染、编辑、提供者或序列化回调直接写入。
  把上次读取的完整对象作为 `expected`；若预期还未保存过，传入 `{NULL, 0}`。
  `patch` 是一个只包含待修改字段的 JSON 对象。例如用 `{"preview":{"enabled":true}}`
  更新 `preview.enabled`，会保留同级未知字段。数组整体替换，`null` 是普通值；
  现有对象不能被替换为非对象，接口不删除键。空补丁或无实质变化不会重写文件。
- 基线忽略空白和对象成员次序，保留数字拼写。自身设置变化返回 `MMS_E_CONFLICT`；
  调用方应保留读取快照并在冲突后重新读取、评估修改，其他插件或本体设置变化不会使自身基线失效。
  遇到文件锁或调用阶段不允许时返回 `MMS_E_BUSY`，不要在 UI 线程循环重试。后台线程返回
  `MMS_E_THREAD`，停止中的插件操作返回 `MMS_E_NOT_READY`。
- 补丁、基线和单插件对象最多各 64 KiB；插件区最多 512 KiB，完整设置仍受 1 MiB
  及 JSON 深度、节点数限制。无效参数或 JSON 返回 `MMS_E_ARGUMENT`，合并后超限返回
  `MMS_E_MEMORY`，损坏、不支持的设置文件或 I/O 错误返回 `MMS_E_FAILED`。拒绝的写入不会
  部分发布，也不会用默认值覆盖损坏的设置。现有设置文件具有与默认暂存文件不同的
  所有者、组、DACL 或完整性标签时，拒绝替换，以免放宽这些限制。写入不支持 EFS 文件
  或重解析点，不承诺保留审计 SACL 等额外元数据或命名数据流。
  此并发约定适用于使用宿主文件锁的写入者。不应同时手动编辑该文件。同目录替换不承诺断电恢复。

## C++17 便利封装

包含 [`<mms/cpp/settings.hpp>`](include/mms/cpp/settings.hpp)，在 `create` 或 `start`
通过 `mms::sdk::SettingsServices::Query(host, settings)` 请求同一个服务。查询返回
`MMS_E_NOT_FOUND` 时可以继续使用插件默认值。封装不改变 ABI、权限或调用阶段。

`Read(std::string&)` 在成功时复制完整 JSON，包括已存在的 `"{}"`。未保存的对象仍返回
`MMS_E_NOT_FOUND`。任何失败都保留原输出，不能把原值当成这次读到的基线。
若测量后对象变大，只在 64 KiB 内再复制一次；再次变大时返回 `MMS_E_MEMORY`。
封装不解析 JSON，也不在调用之间缓存设置。

以下示例在本插件普通 `enqueueUi` 的 run 回调中运行。若界面之前已读入设置并以此准备修改，
应直接传入当时保留的基线，而不是写入前重新读取。

```cpp
MmsResult EnablePreview(const mms::sdk::SettingsServices& settings) {
    std::string snapshot;
    std::optional<std::string_view> expected;
    const auto result = settings.Read(snapshot);
    if (result == MMS_OK) expected = std::string_view(snapshot);
    else if (result != MMS_E_NOT_FOUND) return result;
    return settings.CompareAndMerge(expected, R"({"preview":{"enabled":true}})");
}
```

`std::nullopt` 要求原对象不存在，`"{}"` 则要求原对象存在且为空。若返回
`MMS_E_CONFLICT`，先重新读入再决定是否提交，封装不会自动重试写入。
