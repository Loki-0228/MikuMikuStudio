# 范围自动订阅接口参考

`mms.ui.refresh.subscribe` 1.1 允许一次显式 DATA 来源发布驱动注册子树或依赖图中的目标。它把真实触发 binding 与展开目标 `seed` 分开：后台发布可以驱动其他位置的控件，GROUP 可以直接作为种子，无需给 GROUP 创建 binding。

本服务继续使用原来源、绑定、UI 目标、依赖图及刷新队列。1.0 的 SELF 表仍为 48 字节；1.1 追加两个函数，表大小为 64 字节。定义见 [C ABI](include/mms/refresh_subscription.h)及 [C++17 SDK](include/mms/cpp/scoped_refresh_subscription.hpp)。独立服务版本不改变插件入口的全局 ABI 1.0。

## 服务协商

需要范围订阅的插件在 UI 线程查询 `queryService("mms.ui.refresh.subscribe", 1, 1, sizeof(MmsUiRefreshSubscriptionApiV1_1), ...)`。返回表的 `h.minor` 至少为 1，前四个回调与原表同序；新 `subscribeScoped` 位于偏移 48，`queryScoped` 位于偏移 56。

1.0 查询仍返回原 48 字节表及 SELF 能力。1.1 查询的能力结构仍使用 1.0 结构头，按实际支持报告 SELF、SUBTREE、DEPENDENTS。旧宿主不能通过新查询时，插件须禁用范围订阅或明确安排自己支持的行为；SDK 不把多个 SELF 请求伪装成复合传播。

| 函数 | 线程 | 行为 |
| --- | --- | --- |
| 原 `getCapabilities` | UI | 返回当前表开放的范围、阶段及额度。 |
| 原 `subscribe` | UI | 保留原绑定目标的 SELF 行为。 |
| 原 `query` | UI 或工作线程 | 读取同一订阅的 72 字节基础状态。 |
| 原 `unsubscribe` | UI | 清理同一池中的原订阅或范围订阅。 |
| `subscribeScoped` | UI | 以触发 binding、原种子引用和范围登记订阅。 |
| `queryScoped` | UI 或工作线程 | 读取范围、工作代次、图版本和等待原因；也可查询原 SELF 句柄。 |

注册、能力查询及注销拒绝只读回调。后台查询只读宿主保留的状态，不遍历图、不访问 HWND。所有回调输入及输出结构填本结构大小、major=1、minor=0；只有 1.1 服务表本身使用 minor=1。失败保持调用者输出不变。

DATA 发布继续保持 1.0 行为：UI 只读回调直接发布返回 `BUSY`，工作线程可以发布不可变来源，即使 UI 正在消费同一客户端的旧版本。后台发布不调用 UI 提供者，也不递归刷新；新版本留待后续安全点。宿主不会仅凭客户端相同就把独立后台发布认定为回调写入。

## 触发与范围

`MmsUiRefreshScopedSubscriptionDescV1` 为 80 字节、8 字节对齐。

| 字段 | 含义 |
| --- | --- |
| `binding` | 本客户端已有的真实 DATA binding，监听其来源版本。 |
| `seed` | 原刷新目标引用；可以与触发 binding 的目标不同。 |
| `scope` | SELF、SUBTREE 或 DEPENDENTS。 |
| `kinds` | 必须包含 DATA，可组合 STATE、LAYOUT、REPAINT；各成员仍按实际能力执行。 |
| `flags`、`reserved`、`reservedTail` | 全为 0。 |

SELF 要求种子是触发 binding 自身目标，沿用原订阅行为。SUBTREE 包含原注册树中的种子及适用后代。DEPENDENTS 按显式有向图展开，不包含种子，也不会自动加入注册后代。触发控件只有在所选范围中实际可达时才参与消费，不因它负责发布而额外刷新。

GROUP 使用原注册句柄及呈现代次。空依赖图可以先登记订阅，等待实际边加入。种子尚未挂载、所属客户端不符或引用过期时拒绝登记。目标引用须从原刷新服务取得，不能使用元素登记代次、HWND 或另造的种子。

已有[依赖图规则](refresh-dependencies.md)继续适用：同客户端、循环拒绝、最多 256 个展开成员，并按拓扑顺序处理同一阶段。订阅沿用每客户端 128 个、全局 2,048 个额度。

范围订阅按 `(binding, seed, scope)` 去重，重复登记返回 `CONFLICT`。1.0 的登记仍保持同一 binding 只能有一个订阅的规则；范围 SELF 委托该原路径。所有范围共用上述额度。

## 接受与消费

来源发布只更新有界通知状态并唤醒宿主。图展开和回调在 UI 安全点执行。尚未提交的发布可以合并；接受票据时，在同一个 binding 锁内捕获准确的触发版本及成员的显式来源集合。与触发器同源的成员使用同一版本，其他来源取该次捕获的版本。

一张原复合票据执行整个集合：先完成全部适用 DATA，再进行 STATE、一次保留布局 LAYOUT，最后 REPAINT。它不拆成独立 SELF 票据。后续发布不能改写已接受集合，触发快照保持到该票据结束；新版本在后续安全点消费。

显式 DATA 仍可复用同一版本的已完成显示模型，不重复调用 `prepare`。未绑定原生 INPUT 则按[当前属性读回](input.md)规则在消费时读取，不把它宣称为通知时刻的不可变属性快照。阶段失败停止后续阶段，不回滚此前已完成的显示更新。

图或绑定改变会使受影响的旧集合失效，并按新结构重新安排当前触发版本。来源发布产生下一轮工作，但不使已接受快照失效。触发器以外的成员源正常更新不是独立触发器；缺失来源首次发布，或相关来源从停泊失败恢复时，可以补做同一个触发版本。

## 状态与恢复

`MmsUiRefreshScopedSubscriptionStateV1` 为 160 字节。前 72 字节与原状态字段同序；`query` 与 `queryScoped` 查询同一个句柄，不创建另一套订阅。

| 字段 | 含义 |
| --- | --- |
| 原三个 `*Revision` | 最新观察、最近提交及最近完整成功的触发来源版本。 |
| `seed`、`scope`、`kinds` | 登记时的原种子、范围及阶段。 |
| `workGeneration` | 最近需要处理的工作代次，包括同一触发版本下的拓扑或恢复变化。 |
| `submittedWorkGeneration` | 最近接受到原队列的工作代次。 |
| `completedWorkGeneration` | 最近完整成功的工作代次。 |
| `submittedDependencyRevision` | 最近接受集合使用的依赖图版本。 |
| `pending` | 有未完成或停泊的工作，不代表宿主应持续重试。 |
| `waitingFor` | 当前实际等待事件。 |

完成条件需同时查看 `pending`、`lastResult` 和工作代次。仅比较 `completedRevision == latestRevision` 无法识别同一来源版本下新增目标还未刷新的情况。

等待位为 `WAIT_GRAPH`、`WAIT_TARGET`、`WAIT_QUEUE_CAPACITY`、`WAIT_SOURCE`、`WAIT_CONSUMER`、`WAIT_BINDING`、`WAIT_HOST_WAKE`、`WAIT_DISPLAY_CAPACITY`。宿主按相关图、绑定、来源、提供者、容量或成功投递事件恢复，不靠定时器反复执行相同失败。无关票据完成不能重新运行失败消费者。

注销种子、种子实例重建、触发绑定解除、来源注销、工程代次改变或停止均使原订阅失效。种子仍是同一实例而暂不可用时等待目标恢复。已进入的回调结束后仍复核原身份，不能向失效对象继续发布。显示租约及 DLL 保活沿用原服务规则。

自动传播不移动播放头、不写工程、不增加撤销记录，也不要求整个场景重新求值。原属性通知及共同历史目前使用[原属性刷新服务](property-refresh.md)；通用事件、动画时钟及这些来源的自动图传播仍需另行协商实际能力。

## C++17 包装

`ScopedRefreshSubscriptionServices::Query` 精确协商 1.1。`SubscribeSubtree`、`SubscribeDependents` 接受原 binding 和目标引用，也可组合原 `RefreshServices` 查询已有种子 handle。`Subscribe` 接收原描述；便利层不注册来源、binding、元素或图边。

```cpp
#include <mms/cpp/scoped_refresh_subscription.hpp>

mms::sdk::ScopedRefreshSubscriptionServices subscriptions;
mms::sdk::ScopedRefreshSubscription watch;
auto result = mms::sdk::ScopedRefreshSubscriptionServices::Query(host, subscriptions);
if (!result) result = subscriptions.SubscribeDependents(triggerBinding, seed, watch);
```

`State` 读取扩展状态，`BaseState` 读取原基础状态，均允许工作线程。`Release` 必须在原 UI 线程执行；失败保留表和句柄供停止时重试，`STALE` 表示宿主已清理。移动构造保留原线程身份；`get` 和 `detach` 交付同一个 C 句柄，低层代码可继续通过原查询及注销函数操作它。
