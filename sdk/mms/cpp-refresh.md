# C++17 界面重绘 SDK

`mms/cpp/refresh.hpp` 包装已有 `mms.ui.refresh` C 表，复用原元素、目标和票据，无需链接宿主库。开发环境与[界面注册 SDK](cpp-ui.md)相同。当前支持 REPAINT 及按钮首阶段 STATE，宿主行为见 [C 接口参考](refresh.md)。

## 类型与线程

| 类型 | 行为 |
| --- | --- |
| `RefreshServices` | 保存原服务表，可复制。查询时记录 UI 线程；工作线程使用已发布的副本提交请求。 |
| `RefreshTicket` | 独占一个原 C 票据。禁止复制和移动赋值，支持移动构造；查询、取消和释放允许工作线程执行。 |

同一个 SDK 对象不提供并发修改保护。不能一边调用票据，一边从另一线程移动或析构它。宿主服务 context 必须在调用和票据析构期间有效；SDK 对象不能存活到插件 `destroy` 之后。未清理的原票据继续按宿主规则保留实例和 DLL。

## 查询服务和目标

以下操作限定宿主 UI 线程。

```cpp
#include <mms/cpp/refresh.hpp>

mms::sdk::RefreshServices refresh;
MmsResult result = mms::sdk::RefreshServices::Query(host, refresh);
// 服务查询成功后，用原 CommandUi 按钮句柄取得目标。
MmsUiRefreshTargetV1 target{};
if (result == MMS_OK) result = refresh.Target(controls.button().handle, target);
```

`Query` 校验宿主头、服务表前缀和六个函数指针，接受同 major 的更高 minor 及尾部扩展。失败保留原输出，不回退旧刷新函数。`Capabilities(out)` 返回能力和配额；`Target(element, out)` 返回实际目标，不创建窗口。二者自动填写 C 输出头，失败保留原输出。

`ready()` 仅表示已取得服务表，不代表宿主尚未停止。`CheckThread()` 在未初始化时返回 NOT_READY，在错误线程返回 THREAD。`api()` 借用原 C 表，复杂插件仍可直接调用。

## 提交请求

目标可以复制给工作线程。以下调用无需再次访问 UI 注册表。

```cpp
// ticket 是预先存在的空对象。后续在插件状态中保留它以查询或取消。
mms::sdk::RefreshTicket ticket;
MmsUiDirtyRectV1 dirty{4, 8, 120, 24};
MmsResult result = refresh.RepaintRect(target, dirty, ticket, 17);
```

| 方法 | 请求含义 |
| --- | --- |
| `Repaint(target, ticket, key=0)` | 整个目标客户区，几何修订为 0，允许执行时几何变化；仍检查实例、工程和就绪状态。 |
| `RepaintElement(element, ticket, key=0)` | UI 线程便利入口，用原元素句柄查询目标后提交整目标请求；工作线程仍使用已捕获目标。 |
| `RepaintRect(target, rect, ticket, key=0)` | 一个局部 DIP 矩形，自动填写捕获时的几何修订。 |
| `RefreshState(target, ticket, key=0)` | 请求按钮启用与选中刷新，自动填写目标几何修订；面板及显隐变化不支持。 |
| `RepaintRects(target, rects, count, ticket, key=0)` | 多个局部 DIP 矩形；count=0 等同整目标请求。 |
| `Request(request, ticket)` | 原样提交完整 C 请求，调用者填写结构头和字段。 |

`key` 为原 coalesceKey。SDK 不合并、不重试，不在 STALE 后自动扩大损伤范围。矩形只需在调用期间有效，宿主复制其内容。旧几何被拒绝时，在 UI 线程重新捕获并计算矩形，不能只改代次。

输出票据必须为空。已有票据时返回 BUSY，不向宿主提交或覆盖旧句柄。接受后直接保存原句柄，不再分配内存。未挂载、暂停或停止仍返回 NOT_READY；数据、布局及超出当前按钮范围的状态仍返回 UNSUPPORTED。

提交返回 OK 只代表入队。RefreshState 的显隐不支持、回调失败或执行时过期由 Query 返回的终态和 result 表达，不能只检查提交结果。

## 查询和清理

| 方法 | 行为 |
| --- | --- |
| `get()`、显式 bool 转换 | 借用原句柄，或检查是否持有票据；不表示已完成。 |
| `Query(out)` | 自动初始化 C 状态头，失败保留原输出。 |
| `Cancel()` | 请求取消，不释放；空对象返回 NOT_READY。 |
| `Release()` | 不取消贡献。成功或宿主返回 NOT_FOUND 后清空，其他错误保留句柄供重试；空对象成功。 |
| `CancelAndRelease()` | 先取消再释放。取消返回 BUSY 或其他错误时保留票据；已取消但释放失败也保留。 |
| `detach()` | 返回原 C 句柄并清空 SDK 对象，新所有者负责通过原服务释放。 |

```cpp
MmsUiRefreshStateV1 state{};
MmsResult result = ticket.Query(state);
if (result == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED) {
    result = ticket.Release();
}
```

COMPLETED 表示请求的状态或窗口失效操作已完成，不保证像素已显示。组合请求可用 Request 提交；失败时读取 completedKinds 判断已完成阶段。释放 pending 票据后，贡献仍执行并占额度；撤回使用 `CancelAndRelease()`。

移动构造转移同一个句柄，原对象变空。票据保存服务表副本，原 `RefreshServices` 变量退出作用域不使票据失效，但宿主 context 仍须有效。通过 `get()` 借用原句柄读取状态；转移释放责任时用 `detach()`，避免两个所有者重复清理。

析构仅尝试 `Release()`，不会取消或等待 UI。析构无法报告释放失败，也不能保留已销毁的 SDK 对象。在 `stop` 中先停止生产请求、结束自己的后台任务，再显式清理全部票据。失败时保留对象并返回 BUSY 或错误供宿主重试，不能仅依赖析构完成停止。

本封装不创建后台线程、轮询器、默认订阅或动画时钟，不开放 DATA、LAYOUT 或完整显隐依赖，也不启用 `mms.edit`。包装捕获意外 C++ 异常并返回错误码；插件仍须遵守 C ABI，不让异常跨 DLL。
