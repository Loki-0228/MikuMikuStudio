# 数据发布自动刷新接口参考

`mms.ui.refresh.subscribe` 1.0 为一个已有 DATA binding 注册自动刷新。来源、绑定和 UI 目标继续使用原句柄；宿主在原刷新队列中创建、执行并释放内部票据。插件无需为每次数据发布手动创建票据。

定义见 [C ABI](include/mms/refresh_subscription.h) 和 [C++17 包装](include/mms/cpp/refresh_subscription.hpp)。来源发布和显示租约沿用 [DATA 服务](data-refresh.md)，执行阶段沿用[刷新服务](refresh.md)。

## 服务和结构

在 UI 线程调用 `host.queryService(context, name, 1, 0, sizeof(MmsUiRefreshSubscriptionApiV1), &api)`，其中 `name` 为 UTF-8 字符串 `mms.ui.refresh.subscribe`。

| 函数 | 线程与作用 |
| --- | --- |
| `getCapabilities(context, out)` | UI 线程。查询订阅数量、阶段及范围。输出先填写结构头。 |
| `subscribe(context, desc, outHandle)` | UI 线程。为本客户端的已有 binding 创建一个订阅。 |
| `query(context, handle, out)` | UI 或工作线程。读取订阅版本、等待状态及最近结果。 |
| `unsubscribe(context, handle)` | UI 线程。删除订阅，取消未完成的内部贡献，释放宿主持有的票据。 |

注册、能力查询和删除在只读回调中返回 `BUSY`。状态查询允许工作线程，但不能与对象销毁并发使用同一个 SDK 包装对象。宿主清理后，旧订阅句柄返回 `STALE`。失败不修改输出参数。

结构按 8 字节对齐，所有输入结构均填写 `h`，保留字段均为 0。

| 结构 | x64 大小 | 字段 |
| --- | --- | --- |
| `MmsUiRefreshSubscriptionCapabilitiesV1` | 32 字节 | `h`、`maxSubscriptionsPerPlugin`、`supportedKinds`、`supportedScopes`、`flags`、`reserved[2]`。 |
| `MmsUiRefreshSubscriptionDescV1` | 32 字节 | `h`、原 `binding`、`kinds`、`flags`、`reserved`。 |
| `MmsUiRefreshSubscriptionStateV1` | 72 字节 | `h`、`subscription`、`binding`、`projectEpoch`、三个来源版本、`lastResult`、`pending`、`reserved`。 |
| `MmsUiRefreshSubscriptionApiV1` | 48 字节 | `h`、`context` 和上述四个函数。 |

每客户端最多 128 个订阅，宿主共 2,048 个。同一 binding 只能有一个订阅，重复注册返回 `CONFLICT`，超额返回 `BUSY`。注册失败不留下半个订阅或唤醒请求。订阅属于精确插件客户端；相同插件 ID 不转移所有权。

`kinds` 必须包含 `MMS_UI_REFRESH_DATA`。可以组合 `STATE`、`LAYOUT` 和 `REPAINT`，每一项都须为该绑定目标当前支持的能力。默认 SDK 组合为 `DATA | REPAINT`。本服务只支持 `SELF`；不会自动展开注册子树或显式依赖图。菜单及没有独立显示区的 GROUP 不接受订阅。

## 发布和执行

注册时来源已有快照，宿主安排该版本；尚未发布时等待首次发布。工作线程仍通过 `DataSource::Publish` 发布完整不可变字节，使用准确的 `expectedRevision`。相同字节不推进来源版本，也不触发新刷新。

每个订阅最多保留一张未结束的内部票据。尚未提交的连续发布合并为最新版本；已经接受的票据保持自己的准确快照。自动订阅不会重写插件已有的显式票据。消费期间再次发布的数据留待后续 UI 安全点处理，不递归进入刷新回调。

宿主按 DATA、STATE、LAYOUT、REPAINT 顺序执行所请求的阶段。DATA 先准备并交换完整显示模型，后续阶段可以读取新显示租约。发布线程不调用 UI 回调，不操作 HWND。

状态字段含义如下。

| 字段 | 含义 |
| --- | --- |
| `latestRevision` | 此订阅已观察到的最新来源版本。 |
| `submittedRevision` | 最近成功交给原队列的来源版本。 |
| `completedRevision` | 最近完整成功执行全部请求阶段的来源版本。 |
| `lastResult` | 最近一次提交或完成结果；成功为 `MMS_OK`。 |
| `pending` | 存在等待提交、停泊或尚未结束的工作。为 1 不等于宿主需要连续重试。 |

阶段失败停止后续执行，不回滚已经完成的显示更新。插件应查询 `lastResult`，不能仅凭 `submittedRevision` 认定显示已完成。

## 等待和清理

队列、来源和显示模型沿用现有配额。宿主在容量不足、目标暂不可用或唤醒失败时保留等待状态，按对应的容量释放、目标重新发布或新数据事件恢复。插件回调返回失败后，不应依靠重复返回同一错误驱动定时刷新；准备新的数据后重新发布。

删除订阅会丢弃未提交版本，并取消内部未完成贡献。已经进入的回调无法追溯停止，宿主在回调后复核身份，阻止失效结果继续发布。解绑、注销来源、目标失效、工程代次改变及插件停止会清除相应订阅。显示租约保持原有寿命规则。

自动刷新不创建工程编辑事务，不移动播放头，不增加撤销记录，也不要求场景重新求值。DATA 回调继续受只读限制。需要编辑数据时，使用另外授权的编辑流程。

## C++17 包装

在 UI 线程协商 `RefreshSubscriptionServices`，对已有 `DataBinding` 调用 `Subscribe`。简单插件和低层 C 插件操作同一订阅句柄。

```cpp
mms::sdk::RefreshSubscriptionServices subscriptions;
mms::sdk::RefreshSubscription subscription;
auto result = mms::sdk::RefreshSubscriptionServices::Query(host, subscriptions);
if (!result) result = subscriptions.Subscribe(binding, subscription);
// 在后续数据发布后，由宿主安排原 DATA 和 REPAINT 阶段。
```

`Subscribe(MmsHandle, ...)` 接受同一 binding 的原 C 句柄；不会创建新绑定。`State` 支持工作线程，失败保持输出不变。`get()` 返回原订阅句柄。

[计数示例](examples/auto_refresh/plugin.cpp)在 `start` 中只注册菜单和按钮。首次命令执行时先确认原按钮已经挂载，再排队到普通 UI 任务中创建来源、绑定和订阅。命令回调本身不能注册这些数据对象。后续 UI 任务仅发布新整数；STATE 读取已消费显示值，以奇偶更新原按钮勾选。示例不请求显式刷新票据。

在 UI 线程显式调用 `Release`。除 `STALE` 表示宿主已清理外，失败时包装对象保留句柄，允许稍后重试。移动构造转移所有权，不改变原 UI 线程；`detach` 将同一句柄及清理责任交给调用者。析构只作一次尽力清理，不能替代插件停止流程。服务 context 必须比全部包装对象活得更久。
