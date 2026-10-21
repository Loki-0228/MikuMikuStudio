# 原属性与历史的范围自动刷新

`mms.ui.refresh.property` 1.1 允许原属性通知及真实共同历史发布驱动注册子树或显式依赖图。触发属性与范围种子分别声明，属性无需关联 INPUT，GROUP 无需创建额外 DATA source 或 binding。服务继续使用原属性、元素、目标、依赖图、订阅池及刷新队列。

定义见 [C ABI](include/mms/property_refresh.h)及 [C++17 SDK](include/mms/cpp/scoped_property_refresh.hpp)。原 1.0 SELF 接口见[原属性刷新](property-refresh.md)。本服务不开放普通生产 `mms.edit`，不增加工程写入权限。

## 协商与线程

`queryService("mms.ui.refresh.property", 1, 1, sizeof(MmsUiPropertyRefreshApiV1_1), ...)` 查询 88 字节表。原 64 字节前缀及六个回调保持。1.0 查询仍返回原表；全局插件 ABI 保持 1.0。

| 追加函数 | 偏移 | 行为 |
| --- | --- | --- |
| `getScopedCapabilities` | 64 | UI 线程查询 40 字节能力结构。 |
| `subscribeScoped` | 72 | UI 线程登记原属性、种子及范围。 |
| `queryScoped` | 80 | UI 或工作线程读取 176 字节扩展状态。 |

新旧 `query`、`unsubscribe` 操作同一订阅句柄；新查询也能读取原 SELF 订阅。原 `getPropertyRef` 和 `notify` 继续使用原属性引用，不创建另一套通知源。所有值结构使用本结构大小、major=1、minor=0；仅服务表的 minor=1。

能力、引用查询、登记和注销必须在 UI 线程，且拒绝只读刷新回调。`notify` 和状态查询允许工作线程。属性通知保留 1.0 的只读因果限制，包括刷新回调等待的同客户端工作线程；普通命令可以通知。显式 DATA 的后台不可变来源发布仍按其原接口规则运行，不能因为 UI 正在消费旧版本而一律拒绝。

失败保持输出不变。SDK 不将多个 SELF 订阅伪装成范围能力。

## 属性、种子与范围

`MmsUiPropertyRefreshScopedSubscriptionDescV1` 为 96 字节，按 8 字节对齐。

| 字段 | 含义 |
| --- | --- |
| `property` | 同客户端 `getPropertyRef` 返回的原属性引用，包含登记代次和工程代次。 |
| `seed` | 原刷新服务返回的完整目标引用，可以与触发属性无关。 |
| `scope` | SELF、SUBTREE 或 DEPENDENTS。 |
| `kinds` | 必须包含 DATA，可组合 STATE、LAYOUT 和 REPAINT。 |
| `flags` | 0，或明确请求并已报告支持的 `FOLLOW_HISTORY`。 |
| `reserved`、`reservedTail` | 0。 |

SELF 沿用原路径，种子必须是关联该属性的原 INPUT。SUBTREE 使用原注册树中的种子及适用后代。DEPENDENTS 使用[原有向图](refresh-dependencies.md)，排除种子，不自动加入注册后代。不同位置的目标可以通过图相连，触发属性不必出现在集合内。

空依赖图可以先登记并等待图边。范围没有默认 INPUT DATA 或显式 DATA binding 时等待 binding 或相应提供者恢复。范围登记按 `(property, seed, scope)` 去重；与原 SELF 共用每客户端 128 个、全局 2,048 个订阅额度。

尚未挂载、外来或陈旧种子不接受登记。同名插件的其他实例不继承原属性或订阅。触发属性更换登记代次、种子失效、工程切换及停止会使原订阅失效。

## 读取与阶段

属性通知序号只表示待处理变化，不是通知时刻的属性值。宿主在 UI 安全点展开范围。每个原生 INPUT 在消费时读取自身当前属性，保留其草稿、选区、焦点及 IME 规则；不能把触发属性的值写入全部成员。

每个成员的显式 DATA binding 优先于默认属性读回。一个成员受绑定接管时，其他原 INPUT 仍可读取；绑定移除后相应成员恢复默认读回。显式来源在接受整张复合票据时一次捕获完整版本集合，后续发布不能替换该票据的旧快照。

单张原复合票据先完成全部适用 DATA，再进行 STATE、一次保留布局 LAYOUT，最后 REPAINT。同版本显式显示模型可使用已完成缓存。默认原属性按消费时读取，不能使用通知序号冒充不可变来源版本。

阶段失败停止后续阶段，已经完成的显示更新不作工程事务回滚。通知和历史发布安排后续工作；相关图、绑定或身份变化可以使旧集合失效，并按同一通知序号重新安排。刷新不移动播放头、不写工程、不增加历史步骤，也不隐式请求完整场景求值。

## 状态与恢复

40 字节能力结构保留原 32 字节字段前缀，追加 `supportedScopes`。`supportedFlags` 仅在真实共同历史发布源可用时报告 `FOLLOW_HISTORY`。

176 字节状态保留原 120 字节字段前缀，原 `target` 字段表示范围种子。追加字段包括 `scope`、`kinds`、`flags`、`workGeneration`、`submittedWorkGeneration`、`completedWorkGeneration` 及 `submittedDependencyRevision`。

通知序号区分原属性变化，工作代次区分同一序号下的图、绑定和恢复变化。完整成功需要同时检查 `pending`、`lastResult`、序号和工作代次；仅比较 `completedSequence == latestSequence` 不能识别同序号新增目标尚未完成的情况。

等待位保留原 BINDING、TARGET、CAPACITY、CONSUMER、HOST_WAKE，追加 GRAPH、SOURCE 和 DISPLAY_CAPACITY。宿主按实际图、目标、绑定、来源、提供者或资源释放事件恢复；重复 Drain、定时器和无关票据完成不能反复执行同一消费者失败。

临时观察分配失败保留上一份完整成员、来源观察及等待原因，不冒充图变化。来源捕获的容量不足等待已接受贡献的真实释放；显示准备失败不会因为释放自身贡献而自动重试，仍等待相关来源或显示容量事件。

相关真实图变化或成功发布的子树变化保留一次原队列请求机会，即使本次观察暂时失败。请求已尝试并失败后，成功补观察不再重试同一次变化，也不清除失败的宿主唤醒停泊。后续图事件按当前图的可达性判断相关范围，避免遗漏新成员或误认已经移出的成员。

真实绑定身份变化也只记录一次工作。若来源观察与原请求连续发生内存失败，后续成功观察只补齐监听，不清除原失败或启动重复消费。新的属性通知可以安排新工作；真实容量释放可以恢复原工作，且不额外增加通知序号或工作代次。没有容量变化的普通来源发布仍遵循 SOURCE、CONSUMER 的恢复条件。

## 共同历史

`FOLLOW_HISTORY` 必须显式选择。真实成功提交、撤销、重做及「仅显式提交」模式下不新增步骤的成功 Adjustment 可以驱动整个范围。准备、失败和取消不产生成功发布；工程边界通过身份失效处理。

历史发布阶段只记录原可信通知并安排后续安全点，不在发布阶段遍历图、调用属性读取或分配 UI 工作。未开启历史跟随的订阅继续只响应显式通知，即使其他订阅监听同一属性的历史发布。

## C++17 包装

`ScopedPropertyRefreshServices::Query` 精确协商 1.1。`Capabilities` 查询扩展能力，`BaseCapabilities` 查询原能力；`GetPropertyRef`、`Notify` 使用同一表的原函数。`SubscribeSubtree` 和 `SubscribeDependents` 接受已有引用，也可组合原 `RefreshServices` 查询原 property 与 seed 句柄。

句柄便利重载按属性服务的 `supportedScopes` 判断范围能力，目标查询仅解析原种子身份。空依赖图可以先登记等待图边，不要求目标已经报告非空 DEPENDENTS，也不要求种子自身支持 DATA。

```cpp
#include <mms/cpp/scoped_property_refresh.hpp>

mms::sdk::ScopedPropertyRefreshServices properties;
mms::sdk::ScopedPropertyRefreshSubscription watch;
mms::sdk::RefreshServices refresh;
auto result = mms::sdk::RefreshServices::Query(host, refresh);
if (!result) result = mms::sdk::ScopedPropertyRefreshServices::Query(host, properties);
if (!result) result = properties.SubscribeDependents(refresh, propertyHandle, groupHandle, watch);
```

默认不跟随历史。需要时在尾部明确传入阶段组合及 `MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY`；SDK 先检查实际能力。`State` 和 `BaseState` 分别读取扩展及原状态，均可用于工作线程。

订阅可移动并保留原 UI 线程身份。`Release` 必须在该线程，失败保留所有权，`STALE` 表示宿主已清理。`get` 借用原句柄，`detach` 移交相同句柄。服务 context 与插件实例必须存活至宿主完成停止，不能与工作线程访问并发销毁包装对象。
