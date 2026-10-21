# 原属性自动刷新接口参考

`mms.ui.refresh.property` 1.0 监听已注册属性的变化，为原生 INPUT 安排 SELF 刷新。定义见 [C 头文件](include/mms/property_refresh.h)和 [C++17 封装](include/mms/cpp/property_refresh.hpp)。它使用原属性、原元素、原目标身份及现有刷新队列，不创建 DATA source、binding 或第二套控件。

需要原属性或共同历史驱动注册子树及显式依赖图时，查询[范围刷新 1.1](scoped-property-refresh.md)。原 1.0 表、SELF 描述与查询结构保持，两个版本共用订阅额度和句柄。

插件改变自身数据后调用 `notify`。宿主在 UI 安全点执行默认属性 DATA，重新调用原属性的 `read`，再按请求执行 STATE、LAYOUT、REPAINT。通知时不读取属性。通知序号表达待处理变化的合并次数，不保存该时刻的属性值；读取结果以实际消费时为准。需要准确版本快照的插件使用[显式 DATA 绑定及订阅](refresh-subscriptions.md)。

## 服务及线程

服务通过 `queryService("mms.ui.refresh.property", 1, 0, ...)` 查询。插件不能据此推断 `mms.edit` 可用；普通生产启动的公共编辑服务仍未开放，原生 INPUT 保持只读。

| 函数 | 线程 | 行为 |
| --- | --- | --- |
| `getCapabilities` | UI | 返回实际阶段、订阅上限及 `FOLLOW_HISTORY` 能力。 |
| `getPropertyRef` | UI | 从本插件原属性句柄取得准确引用。 |
| `subscribe` | UI | 将一个原生 INPUT 与它当前的原属性关联，成功后安排一次当前值读回。 |
| `notify` | UI 或工作线程 | 更新该属性所有有效订阅的待处理序号。 |
| `query` | UI 或工作线程 | 查询原订阅的序号、结果及等待原因。 |
| `unsubscribe` | UI | 删除订阅并取消尚未消费的内部贡献。 |

所有带 `h` 的输入和输出在调用前填写本结构大小、major=1、minor=0。错误不修改调用者输出。服务 context 和插件实例须存活至宿主完成停止；工作线程不能与实例销毁并发。

只读刷新回调不能订阅、注销或通知，包括回调等待的同插件工作线程。普通命令可以通知。通知不会触发递归读取，后台调用不访问注册表或 HWND。

## 引用与订阅

`MmsUiPropertyRefV1` 为 32 字节，包含原属性 `property`、独立的 `registrationGeneration` 和 `projectEpoch`。属性同句柄更新也会更换登记代次，使旧引用和旧订阅失效。元素的 `generation`、场景 revision 和 HWND 均不能代替这些字段。相同插件 ID 的另一个实例不拥有旧实例的属性。

`MmsUiPropertyRefreshSubscriptionDescV1` 为 88 字节。

| 字段 | 要求 |
| --- | --- |
| `target` | 原刷新服务从 INPUT 返回的完整 `MmsUiTargetRefV1`。 |
| `property` | `getPropertyRef` 返回的完整引用，必须与该 INPUT 当前属性相同。 |
| `kinds` | 必须包含 DATA；可组合该目标实际支持的 STATE、LAYOUT、REPAINT。 |
| `flags` | 0，或已报告支持的 `MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY`。 |
| `reserved` | 0。 |

每个 INPUT 最多一个订阅。每个插件最多 128 个，宿主全局最多 2,048 个。超额返回 `BUSY`。尚未挂载的元素没有可订阅的目标；已挂载但暂不可用的目标保留订阅并等待成功的目标发布。

显式 DATA binding 优先。它存在时，原属性订阅停泊为 `WAIT_BINDING`，不会读取属性或争用显示模型。绑定移除后补读当前属性；绑定加入时撤回尚未消费的默认属性贡献。切换来自实际绑定事件，无需插件轮询。

普通缩放保持原实例和订阅。跨挂载根移动、布局隐藏导致视图重建、注销、停止及工程代次切换会使旧目标失效，插件须从新目标引用重新订阅。STATE 隐藏的保留实例遵循原目标生命周期。

## 合并、失败与历史

`MmsUiPropertyRefreshSubscriptionStateV1` 为 120 字节。`latestSequence` 是最新通知序号，`submittedSequence` 是最近提交队列的序号，`completedSequence` 是最近完整成功的序号。`pending` 表示仍有待完成变化；它不保证当前可以运行。每个订阅最多一个内部票据，未提交的多次通知合并为最近序号。

`waitingFor` 是下列位的组合。

| 标志 | 含义及恢复条件 |
| --- | --- |
| `WAIT_BINDING` | 显式绑定接管，等待它移除。 |
| `WAIT_TARGET` | 目标暂停或未就绪，等待实际成功发布。 |
| `WAIT_CAPACITY` | 原队列容量不足，等待容量版本确实变化。 |
| `WAIT_CONSUMER` | 属性读取或后续消费者失败，等待新通知或对应提供者重新发布。 |
| `WAIT_HOST_WAKE` | 宿主投递失败，等待新的通知或成功的宿主唤醒。 |

重复 Drain、无关票据完成或定时器不会重新执行已停泊的消费者失败。属性读回失败保留原显示；后续阶段失败遵循原刷新票据的阶段完成规则，新通知可以重试。输入框的活动草稿、草稿起始规范值、选择、焦点及 IME 组合沿用[原 INPUT 规则](input.md)，自动读回不能覆盖用户正在编辑的文本。

`FOLLOW_HISTORY` 只在宿主安装真实共同历史发布源时报告。成功的原生及插件发布、撤销、重做，以及「仅显式提交」模式下不新增历史步骤的成功 Adjustment，均可安排读回。失败、准备、取消和工程重置不发值变化通知；工程边界另使旧引用失效。该信号只记录 revision 并唤醒队列，属性读取发生在后续 UI 安全点。

自动刷新不改变播放头、工程数据、历史步数或场景求值。这里的共同历史跟随不是工程写入权限，也不推断所有原生编辑入口都已经迁移。

## C++17 包装

`PropertyRefreshServices::Query` 查询原 C 表。`GetPropertyRef` 捕获原引用；`Subscribe` 接收原描述，`SubscribeInput` 和 `WatchInput` 组合原目标查询与属性引用。默认 `kinds=DATA|REPAINT`、`flags=0`，不隐式开启历史跟随。

```cpp
#include <mms/cpp/property_refresh.hpp>

// host、inputHandle、propertyHandle 均来自同一插件实例。
mms::sdk::RefreshServices refresh;
mms::sdk::PropertyRefreshServices properties;
mms::sdk::PropertyRefreshSubscription watch;
auto result = mms::sdk::RefreshServices::Query(host, refresh);
if (!result) result = mms::sdk::PropertyRefreshServices::Query(host, properties);
if (!result) result = properties.WatchInput(refresh, inputHandle, propertyHandle, watch);
```

`Notify(ref)` 和订阅的 `State(out)` 可在工作线程调用。`Release()` 在原 UI 线程注销；失败时保留原服务及句柄，供 `stop` 重试。`STALE` 表示宿主已回收该订阅，包装清空所有权。移动构造仍保留原 UI 线程身份；`detach()` 移交同一 C 句柄，不额外注册对象。析构只作一次尽力清理，插件不能依赖错误线程的析构完成卸载。
