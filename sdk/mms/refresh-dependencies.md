# 显式依赖刷新接口参考

`mms.ui.refresh.dependencies` 1.0 注册同一插件内的有向依赖。边的两个端点都是原 `mms.ui` 元素的目标引用。请求、阶段、取消、状态查询和释放使用原 [刷新服务](refresh.md)，没有第二套控件或队列。

定义见 [C ABI](include/mms/refresh_dependency.h) 和 [C++17 封装](include/mms/cpp/refresh_dependency.hpp)。结构按 8 字节对齐。

## 服务与结构

通过 `host.queryService(context, name, 1, 0, sizeof(MmsUiRefreshDependencyApiV1), &api)` 查询，`name` 为 UTF-8 的 `mms.ui.refresh.dependencies`。服务查询及下表三个函数只能在 UI 线程调用。

| 函数 | 作用 |
| --- | --- |
| `getCapabilities(context, out)` | 返回每插件最多 512 条边、每请求最多 256 个可达目标。输出先填写结构头。 |
| `registerDependency(context, desc, outHandle)` | 在两个已发布目标之间注册边，成功后返回宿主拥有的边句柄。 |
| `unregisterDependency(context, handle)` | 删除本客户端的边；已自动清除的边返回 `NOT_FOUND`，停止期间仍可清理。 |

`MmsUiRefreshDependencyApiV1` 为 40 字节，包含结构头、服务 context 和上述三个函数。`MmsUiRefreshDependencyCapabilitiesV1` 为 24 字节，包含结构头、`maxDependenciesPerPlugin`、`maxDependentsPerRequest`、`flags` 和 `reserved`；后两项为 0。

`MmsUiRefreshDependencyDescV1` 为 80 字节，字段如下。

| 字段 | 要求 |
| --- | --- |
| `h` | 本结构大小，major=1、minor=0。 |
| `upstream` | 通过原 `refresh.getTarget` 取得的完整上游引用。 |
| `dependent` | 同一插件客户端的完整下游引用。可以位于另一个公开 UI 位置。 |
| `flags`、`reserved` | 必须为 0。 |

自环、重复边或造成环的边返回 `CONFLICT`。单插件 512 条边、宿主共 4096 条边；超限返回 `BUSY`。接收先完成验证和分配，失败不发布半条边、不改变图修订，也不修改输出句柄。命令状态、数据准备、布局回调及其因果 UI 任务中增删边返回 `BUSY`。

边属于精确插件客户端。相同文字插件 ID 不能转移边句柄的所有权。不能连接其他插件的目标、任意 HWND 或自造代次。

## DEPENDENTS 请求

`refresh.request` 使用原 `MmsUiRefreshRequestV1`，把 `scope` 设为 `MMS_UI_REFRESH_DEPENDENTS`。宿主从 `target` 展开可达的有向图，排除种子，按拓扑顺序处理。同一个目标经多条路径到达时只处理一次，不自动加入注册子树的其他孩子。

例如 `A→B`、`A→C`、`B→C` 的请求以 A 为种子，只处理 B、C；C 排在 B 后。与 A 同容器的 D 不因此加入。GROUP 可以作为保留身份或中间节点，但没有自身的绘制客户区。

| 请求内容 | 语义 |
| --- | --- |
| `kinds` | 使用各成员实际支持的 DATA、STATE、LAYOUT、REPAINT；每个请求位至少有一个适用成员，否则整次 `UNSUPPORTED`。 |
| DATA | `binding` 和 `sourceRevision` 均为 0。入队时一次捕获所有参与绑定的版本集合，未绑定 INPUT 在消费时读回原属性。 |
| `dirtyRects` | 只能 `count=0`，即各适用目标的整个客户区。不同位置不能共用种子的局部矩形；非空返回 `UNSUPPORTED`。 |
| `expectedGeometryRevision` | 0 允许采用消费时的有效几何。非零固定种子及成员的入队几何，变化则 `STALE`。 |
| `coalesceKey` | 沿用原合并键；还要求相同种子、图修订、成员身份、阶段和来源版本集合。 |

一次请求依次完成全部成员的 DATA、全部 STATE、一次保留布局、全部 REPAINT。阶段内按依赖顺序执行；并非先完成一个控件的全部阶段再处理下一个。LAYOUT 从真正支持该阶段的成员取得宿主后端入口，仍可能重排同一保留树中的相邻控件。

请求成功返回原票据。每个贡献独立取消、查询、释放；合并不会合并票据身份。`completedKinds` 只记完整成功的阶段，`sourceRevision=0` 表示版本集合或消费时读回，不表示 DATA 未执行。阶段失败停止后续工作，不回滚已完成的显示更新。

混合原生菜单和窗口的组在菜单跟踪期间整体等待。其他不涉及菜单的请求可以处理；宿主不会用重试计时器反复消费等待组。退出菜单跟踪后恢复该组。菜单仍只有 STATE，其作为种子不限制下游窗口实际支持的阶段。

## 版本与生命周期

成功增删边推进该客户端的图修订，使该客户端已有的待执行 DEPENDENTS 票据 `STALE`。这是保守规则；即使改变的是另一条边，旧请求也不自动扩大或缩小成员集合。

移动和缩放保留有效边。端点注销、呈现实例重建、工程切换或插件停止会清除相应边；旧句柄不能用于新目标。目标、属性、绑定和状态提供者在入队后变化时，原队列继续检查相应版本，不能把旧请求用于替换对象。

后台线程可以用已捕获的种子引用提交和查询原刷新票据。后台仅读取宿主保留的图与目标快照，不调用注册表、HWND 或提供者。刷新不修改播放头、场景求值、工程数据、撤销历史或布局偏好。

## C++17 包装

`RefreshDependencyServices::Query` 协商上述同一 C 表。`Connect` 接受两个原目标引用；`ConnectElements` 先用 `RefreshServices::Target` 解析原元素句柄，再调用 `Connect`。两者返回不可复制的 `RefreshDependency`，其 `get()` 即原 C 边句柄。

在 UI 线程显式调用 `RefreshDependency::Release`。失败时对象保留句柄，允许稍后重试；`NOT_FOUND` 视为宿主已清理。移动构造转移同一句柄；`detach` 把清理责任交给调用者。析构仅作一次尽力清理，不能替代停止流程。服务 context 必须比持有它的对象活得更久。

```cpp
mms::sdk::RefreshServices refresh;
mms::sdk::RefreshDependencyServices dependencies;
mms::sdk::RefreshDependency edge;
// 在 UI 线程协商并检查每次返回值。
auto result = mms::sdk::RefreshServices::Query(host, refresh);
if (!result) result = mms::sdk::RefreshDependencyServices::Query(host, dependencies);
if (!result) result = dependencies.ConnectElements(refresh, sourceButton, previewPanel, edge);
// 缓存种子引用后，可以在工作线程使用 RefreshDependents 提交原票据。
```

`RefreshServices::RefreshDependents` 接受已捕获的引用，允许后台请求；`RefreshDependentElements` 先解析原元素，因此只能在 UI 线程调用。二者继续返回原 `RefreshTicket`。

此服务只提供显式图。已有显式 DATA binding 可以另用[自动订阅](refresh-subscriptions.md)安排 SELF 刷新；订阅不会自动展开此依赖图。自动依赖传播、插件量测提供者、动画时钟及更多控件能力仍属后续需求。
