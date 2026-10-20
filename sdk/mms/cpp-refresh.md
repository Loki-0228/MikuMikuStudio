# C++17 界面重绘 SDK

`mms/cpp/refresh.hpp` 包装已有 `mms.ui.refresh` C 表，复用原元素、目标和票据，无需链接宿主库。开发环境与[界面注册 SDK](cpp-ui.md)相同。当前支持 SELF 和 SUBTREE 范围的 REPAINT、按钮 STATE 及保留布局的 LAYOUT；[数据刷新 SDK](data-refresh.md)通过同一 Request 和 RefreshTicket 提交 DATA。实际目标能力和宿主行为见 [C 接口参考](refresh.md)。

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
| `RepaintRect(target, rect, ticket, key=0)` | SELF 范围的一个局部 DIP 矩形，自动填写捕获时的几何修订。 |
| `RefreshState(target, ticket, key=0)` | 请求按钮状态刷新，严格匹配捕获的几何修订。支持启用、选中及保留槽位内的显隐。 |
| `RefreshStateCurrent(ref, ticket, key=0)` | 使用原目标引用提交无坐标 STATE，接受执行时的有效几何。适合工作线程在显隐变化后继续使用同一实例及工程引用。 |
| `RelayoutCurrent(ref, ticket, key=0)` | 使用原目标引用提交 LAYOUT + SELF，几何修订为 0；按保留布局及缓存状态排版，不隐含 STATE 或 REPAINT。 |
| `RefreshStateLayout(ref, ticket, key=0)` | 使用原目标引用提交 STATE、LAYOUT、REPAINT，范围为 SELF、几何修订为 0；支持按钮收缩空位及同一引用恢复零尺寸按钮。 |
| `RepaintRects(target, rects, count, ticket, key=0)` | SELF 范围的多个局部 DIP 矩形；count=0 等同整目标请求。 |
| `RefreshSubtree(ref, kinds, ticket, key=0)` | 注册子树；无局部矩形、几何约束为 0，DATA 原子捕获绑定版本集合。 |
| `RepaintSubtree(ref, ticket, key=0)` | 子树实际呈现的整客户区失效。 |
| `RepaintSubtreeRects(target, rects, count, ticket, key=0)` | 根局部 DIP 区域映射到注册后代，自动填写根几何修订；count=0 等同无几何约束的整子树重绘。 |
| `RefreshSubtreeRects(target, kinds, rects, count, ticket, key=0)` | 同一子树请求中组合各阶段。矩形只限制 REPAINT，非空时 kinds 必须包含 REPAINT；DATA 仍原子捕获整树绑定版本集合。 |
| `RefreshStateSubtree(ref, ticket, key=0)` | 子树宿主按钮的 STATE。 |
| `RelayoutSubtree(ref, ticket, key=0)` | 保留树 LAYOUT，不隐含 STATE。 |
| `RefreshDataSubtree(ref, ticket, key=0)` | 子树 DATA 与 REPAINT。 |
| `RefreshSubtreeElement(element, kinds, ticket, key=0)` | UI 线程先按原 handle 查询目标，再提交同一请求。 |
| `Request(request, ticket)` | 原样提交完整 C 请求，调用者填写结构头和字段。 |

`key` 为原 coalesceKey。SDK 不合并、不重试，不在 STALE 后自动扩大损伤范围。矩形只需在调用期间有效，宿主复制其内容。旧几何被拒绝时，在 UI 线程重新捕获并计算矩形，不能只改代次。

输出票据必须为空。已有票据时返回 BUSY，不向宿主提交或覆盖旧句柄。接受后直接保存原句柄，不再分配内存。未挂载、暂停或停止仍返回 NOT_READY；不支持 LAYOUT 的后端或目标、超出当前按钮范围的 STATE 返回 UNSUPPORTED；SELF + DATA 须使用有效数据绑定及来源版本；SUBTREE + DATA 由宿主捕获版本集合。

提交返回 OK 只代表入队。不支持的显隐、回调失败或执行时过期由 Query 返回的终态和 result 表达，不能只检查提交结果。`RefreshStateCurrent` 不绕过实例、工程、ready 或有效布局提示检查；工程切换或实例重建后仍须重新取得目标。

需要动态显隐且保留空位的声明式按钮，可在注册时指定现有布局提示：

```cpp
MmsUiLayoutHintsV1 hints{};
hints.h = mms::sdk::Header<MmsUiLayoutHintsV1>();
hints.flags = MMS_HINT_PRESERVE_HIDDEN_SLOT;
spec.button.hints = &hints; // spec 是 CommandUiSpec；注册调用复制提示。
```

用户布局覆盖仍可改变有效提示。隐藏保留槽位，不触发重新排版；恢复不显示隐藏的父区域。显隐改变几何修订，使用严格 `RefreshState` 或局部矩形前应重新捕获目标；无坐标后台状态请求可用 `RefreshStateCurrent(target.target, ticket)`。

## 收缩与恢复布局

需要随可见性收缩空位时，不必声明 PRESERVE_HIDDEN_SLOT。向支持 LAYOUT 的按钮提交状态与布局组合：

```cpp
mms::sdk::RefreshTicket ticket;
MmsResult result = refresh.RefreshStateLayout(target.target, ticket);
```

宿主只查询目标按钮状态，再按现有布局树重排并提交重绘。隐藏收缩后，目标可以返回 `ready=0` 和零尺寸，但仍保留同一实例及工程引用；`RefreshStateLayout` 可用该引用恢复。它不绕过未挂载、布局暂停、实例失效或工程变化的检查。面板不支持 STATE，应按实际能力使用 `RelayoutCurrent` 或完整 `Request`。

只需按已保留描述、提示和缓存可见性排版时，使用 `RelayoutCurrent(target.target, ticket)`。纯 LAYOUT 不重新查询命令，不创建 surface，不保存偏好。它和其他方法共用 C 请求、合并键、票据及停止规则，没有独立队列。后台线程使用已捕获的服务与引用提交，不能在 UI 线程等待完成。

组合按 DATA、STATE、LAYOUT、REPAINT 执行。布局改变几何后，基于旧几何的局部矩形返回 STALE；读取票据的 completedKinds 判断已完成阶段。SDK 不自动扩大损伤或重试。上述两个方法使用无坐标请求；复杂插件仍可通过 Request 指定同一套 C 字段。

布局失败会暂停目标和新几何快照，已有快照租约仍可读。宿主不承诺回滚部分已移动的 HWND；后续正常宿主布局成功后恢复发布。

## 同一注册树的子树刷新

简单插件保留组注册得到的原 handle，即可查询 GROUP 目标。复杂插件继续操作同一批对象。

```cpp
MmsUiRefreshTargetV1 target{};
MmsResult result = refresh.Target(groupHandle, target); // UI 线程
mms::sdk::RefreshTicket ticket;
if (result == MMS_OK) {
    result = refresh.RefreshSubtree(target.target,
        MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT |
        MMS_UI_REFRESH_REPAINT, ticket);
}
```

GROUP 只开放 SUBTREE。supportedKinds 是开放范围的能力并集，提交时仍检查每个位是否有适用成员。需要 DATA 时可组合该位，或用 RefreshDataSubtree；没有有效绑定时返回 UNSUPPORTED。PANEL 内部的插件自有 HWND 不是注册后代。

RefreshSubtree 在工作线程使用已捕获引用。SDK 本身不展开树或捕获版本，宿主在同一 C 队列中处理接收、配额、取消和合并。DATA 完成后的 ticket.sourceRevision=0 表示版本集合，读取 completedKinds 确认成功；各绑定消费版本从数据服务读取。

阶段按整树 DATA、整树 STATE、一次布局、各目标 REPAINT 执行。跨所有者整次拒绝，拓扑或绑定集合变化使旧票据 STALE。PENDING 取消撤回全树；失败停止后续阶段，不回滚已经应用的 UI。完整规则见[子树 C 契约](refresh.md#子树范围)。

局部子树请求传入完整 `MmsUiRefreshTargetV1`，由 SDK 填写根的几何修订。矩形以根客户区左上角为原点，GROUP 使用逻辑容器范围：

```cpp
MmsUiDirtyRectV1 dirty{4, 8, 120, 24};
mms::sdk::RefreshTicket ticket;
MmsResult result = refresh.RepaintSubtreeRects(target, &dirty, 1, ticket);
```

`target` 在 UI 线程取得后，服务副本、目标和矩形可交给工作线程提交。每次最多 32 个矩形，宿主在调用期间复制数组。宿主按实际原点和 DPI 把根区域映射并裁剪到各实际成员，多个区域取并集。零面积或无交集区域不产生损伤，不转换成全控件重绘。不同 DPI 的成员必须属于同一已发布宿主坐标空间。

需要组合 DATA 或 STATE 时，使用 `RefreshSubtreeRects(target, kinds, rects, count, ticket)`。非空矩形要求 kinds 包含 REPAINT，其他阶段仍处理整棵注册子树。根或任一成员在排队期间移动、缩放或更换 DPI 时，严格几何检查使旧票据 STALE，即使该成员位于区域之外。

同次 STATE 只改变显隐而未改变空间映射时，可继续使用原区域；LAYOUT 改变原点、尺寸、DPI 或坐标空间时，整树 REPAINT 前返回 STALE，保留已完成的 completedKinds。需要跟随新布局重绘时，使用无坐标的整树方法，再重新取得几何计算下一次局部区域。SDK 不自动重试或扩大损伤。

无矩形但需要严格固定几何时，仍可用 Request 填根的非零 expectedGeometryRevision，宿主同时固定捕获的成员几何。区域请求和整树请求共用同一 C 队列、合并、取消和停止规则。

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

COMPLETED 表示请求的各刷新阶段已完成，不保证像素已显示。组合请求可用 Request 提交；失败时读取 completedKinds 判断已完成阶段。释放 pending 票据后，贡献仍执行并占额度；撤回使用 `CancelAndRelease()`。

移动构造转移同一个句柄，原对象变空。票据保存服务表副本，原 `RefreshServices` 变量退出作用域不使票据失效，但宿主 context 仍须有效。通过 `get()` 借用原句柄读取状态；转移释放责任时用 `detach()`，避免两个所有者重复清理。

析构仅尝试 `Release()`，不会取消或等待 UI。析构无法报告释放失败，也不能保留已销毁的 SDK 对象。在 `stop` 中先停止生产请求、结束自己的后台任务，再显式清理全部票据。失败时保留对象并返回 BUSY 或错误供宿主重试，不能仅依赖析构完成停止。

本封装不创建后台线程、轮询器、默认订阅或动画时钟。DATA 便利入口由同层的 `data_refresh.hpp` 组合本封装实现。属性 STATE、DEPENDENTS、自动依赖和插件量测提供者尚未开放，普通 `mms.edit` 仍关闭。包装捕获意外 C++ 异常并返回错误码；插件仍须遵守 C ABI，不让异常跨 DLL。
