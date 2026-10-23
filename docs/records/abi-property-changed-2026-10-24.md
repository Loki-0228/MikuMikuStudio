# 属性变更事件

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批接入 `PROPERTY_CHANGED`。

## 实现

- [ui_registry.hpp](../../src/plugins/ui_registry.hpp)、[ui_registry.cpp](../../src/plugins/ui_registry.cpp)：`UiRegistry` 新增 `SetPropertyChanged` 回调。宿主原生属性编辑经 `CommitPropertyInput` 成功提交后，回调带属性 id 与当前修订号；回调在 UI 回调守卫释放后执行。
- [manager.cpp](../../src/plugins/manager.cpp)：`Manager` 安装该回调，用 `MmsPropertyEventV1`（版本 1、`propertyId`、零目标、修订号）广播 `MMS_EVENT_PROPERTY_CHANGED`。
- [manager.cpp](../../src/plugins/manager.cpp)：插件声明式属性编辑（`InputEdit`）提交成功后不直接广播（提交发生在插件回调内），而是入队并在 `Drain` 中、回调之外用同一负载广播；队列上限 64，提交后投递唤醒消息。
- [plugin_storage.hpp](../../include/mikumikustudio/plugin_storage.hpp)、[integration.cpp](../../src/plugins/integration.cpp)：新增宿主入口 `PluginNotifyPropertyChanged`，供事件探针确定性地验证入队、投递与负载。
- [lifecycle_plugin.cpp](../../tests/plugins/lifecycle_plugin.cpp)：插件校验事件版本、负载大小、属性 id 非空与修订号非零，并记录 `EVENT_PROPERTY_CHANGED`；失败记录 `PROPERTY_PAYLOAD_FAIL`／`PROPERTY_VALUE_FAIL`。
- [host_event_tests.cpp](../../tests/plugins/host_event_tests.cpp)：探针触发属性变更事件，等待 `EVENT_PROPERTY_CHANGED` 并断言无属性负载失败。

## 验证

证据目录 `build/verification/abi-property-changed-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `plugin_abi_host_events` | 通过 4 项；插件日志含 `EVENT_PROPERTY_CHANGED`，无 `PROPERTY_PAYLOAD_FAIL`／`PROPERTY_VALUE_FAIL` |
| 默认全量契约套件 | 通过，`PASS checks=44764`（新增检查来自属性事件路径） |
| 相关 CTest | 10 个模式全部通过，9.8 秒 |
| 完整产品 | 构建通过，SHA-256 `883b5e51517c6d0fa3baec063dfdab5f31766a883c4ea9b9ebb4b818899a3306` |
| 原始兼容产品 | 构建通过，SHA-256 `4526f6ed2496b32ddf3c43143531f159ee1d87ecfe9ef5548ed11fdedf4b6742` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 事件的目标对象为零：宿主原生属性编辑快照只带元素、属性与场景修订，不带 `MmsObjectRef`；目标身份留待输入绑定路径扩展。
- 插件通过 `InputEdit` 发起的声明式属性编辑现已入队广播，但没有专门的插件级断言；默认套件只验证编辑行为未回归。
- 宿主探针直接调用宿主入口验证事件契约；真实 UI 点击属性输入后再收到事件的端到端链路、多对象属性与真实 MME 工程仍未验证。
- `COMPUTE_POLICY_CHANGED` 仍依赖 P4 计算策略接口。

## 后续

下一批评估插件声明式属性编辑的广播时机、目标身份与计算策略事件；随后继续透明排序性能缓存与 P2–P6 剩余接口。
