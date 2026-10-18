# UI 数据绑定与 C++17 SDK

`mms.ui.refresh.bind` 1.0 将插件准备的数据复制为宿主保留的不可变显示模型。C 定义见 [refresh_binding.h](include/mms/refresh_binding.h)，C++17 包装见 [data_refresh.hpp](include/mms/cpp/data_refresh.hpp)。它复用原 UI 元素、目标引用和 [mms.ui.refresh 票据](refresh.md)，不创建第二个控件或刷新队列。

当前范围为本插件已挂载按钮或 surface 面板的 `DATA + SELF`。发布来源不会自动刷新；插件显式请求 DATA，按需组合 STATE 和 REPAINT。按钮状态回调或面板绘制通过 `acquireDisplay` 读取模型。面板的文字、图形和交互仍由插件实现。依赖订阅、跨插件绑定、LAYOUT、SUBTREE 和 DEPENDENTS 尚未开放。

## 开发和协商

沿用 [SDK 开发环境](README.md#开发环境)：Windows x64、C11 或 C++17、Win7 SP1 最低目标；示例插件使用 `/MT`，不链接宿主库。服务表使用 `MMS_CALL`、`MmsHeader` 和 8 字节对齐，大小为 96 字节。清单同时声明 `mms.ui.refresh.bind` 和 `mms.ui.refresh` 1.0；在 UI 线程通过 `host.queryService` 查询。旧刷新表仍为 64 字节。

所有描述和输出结构先填写 `h.size`、major=1、minor=0；flags 和 reserved 为 0。输入字节在调用期间有效即可，宿主复制内容；失败不修改输出参数。字节格式由插件定义，不能包含需要宿主解引用的插件指针或 STL 对象。

| 函数 | 线程 | 用途 |
| --- | --- | --- |
| `getCapabilities` | UI | 读取模型、句柄和保留字节配额。 |
| `registerSource` | UI | 用本插件唯一的稳定 id 和 `maxBytes` 注册来源；初始 revision=0，尚无数据。 |
| `publishSource` | UI 或工作线程 | 以精确 `expectedRevision` 发布宿主副本。首次填 0；冲突返回 CONFLICT。 |
| `getSource` | UI 或工作线程 | 读取来源句柄、revision、工程代次和当前字节数。 |
| `unregisterSource` | UI | 撤销来源及其绑定；已取得的显示租约仍有效。 |
| `bind` | UI | 将同 owner 的来源绑定到有效原目标引用；每个目标最多一个绑定。 |
| `unbind` | UI | 撤销绑定，旧待执行贡献失效；已取得的租约仍可读。 |
| `writePrepared` | UI 的 prepare 回调 | 用短期 builder 写入一次完整候选显示模型。 |
| `acquireDisplay` | UI 或工作线程 | 返回当前完整模型的不可变字节及独立 lease。尚无模型时返回 NOT_READY。 |
| `releaseDisplay` | UI 或工作线程 | 释放原 lease；停止期间仍可调用。 |

## 来源版本和执行顺序

revision 由宿主生成。成功发布不同字节增加 revision，相同字节保持不变；空字节也是有效模型。插件不能用布局修订或任意计数替代返回的来源版本。

DATA 请求填写 `binding`、非零 `sourceRevision`、原目标引用和 SELF。入队时必须匹配来源的当前版本 N，宿主保留该版本的强引用。其后发布 N+1 不改变已接受的 N；尚未捕获的旧 N 返回 STALE。若 N+1 已先消费，旧 N 不得覆盖新显示。不同绑定或来源版本分组执行，每个票据仍独立取消、查询和释放。

UI 安全点先执行 DATA，再执行组合中的 STATE，最后提交 REPAINT。DATA 单独请求不隐含重绘、状态查询或布局。重复消费同一版本跳过 prepare；新版本得到相同显示字节时只推进来源版本，不增加 displayRevision。显式组合的 STATE 和 REPAINT 仍按请求执行。

prepare 接收借用的 `sourceBytes`、精确 `sourceRevision`、上次消费的 `previousRevision` 和短期 `builder`。回调调用一次 `writePrepared` 后返回；返回错误、没有写入、重复写入、分配失败或目标失效都保留上一份完整显示。builder 在回调结束后失效。prepare 为 NULL 时，宿主直接复制来源作为显示模型。

候选准备、目标复核及最终交换保持只读保护。回调不能注册或注销 UI、执行编辑或递归请求 DATA/STATE。由本插件在该因果链中安排的 UI 任务，包括取消回调及后代任务，继续保留相同限制。无关插件的独立后台任务不会因时间重叠而永久继承该限制。

成功 DATA 的票据 `sourceRevision` 是实际消费版本；`completedKinds` 保留已完成阶段，即使后续 STATE 或 REPAINT 失败。DATA 本身失败时不继续后续阶段。COMPLETED 不表示已显示像素。

刷新不修改工程、播放头、编辑姿态或共同历史，也不触发场景求值。耗时计算应在后台完成后再发布，prepare 只做有界的显示准备，不等待 UI 作业。

## C++17 使用

便利层保存原 C 句柄。复杂插件可以用 `api()` 或 `detach()` 继续操作同一对象；没有独立身份和生命周期。

```cpp
#include <mms/cpp/data_refresh.hpp>

mms::sdk::DataServices data;
mms::sdk::RefreshServices refresh;
mms::sdk::DataSource source;
mms::sdk::DataBinding binding;
MmsUiRefreshTargetV1 target{};

// 在插件状态中保存上述对象，以下初始化在 UI 线程执行。
MmsResult Initialize(const MmsHostV1& host, MmsHandle panel) {
    if (source || binding) return MMS_E_BUSY; // 先完成上次清理，再重试。
    auto result = mms::sdk::DataServices::Query(host, data);
    if (!result) result = mms::sdk::RefreshServices::Query(host, refresh);
    if (!result) result = refresh.Target(panel, target);
    if (!result) result = data.RegisterSource(mms::sdk::Text("example.counter"), 64, source);
    if (!result) result = data.Bind(target.target, source, 64, binding);
    if (result) {
        const auto closeBinding = binding.Release();
        if (closeBinding) return closeBinding;
        const auto closeSource = source.Release();
        if (closeSource) return closeSource;
    }
    return result; // NOT_READY 后等待实际挂载；清理失败由 stop 保留状态重试。
}

// 可从工作线程调用。ticket 必须为空，并由插件保留和查询。
MmsResult Publish(int value, uint64_t expectedRevision, mms::sdk::RefreshTicket& ticket) {
    MmsUiDataSourceStateV1 state{};
    auto result = source.Publish({&value, sizeof(value)}, expectedRevision, state);
    if (!result) result = binding.Refresh(refresh, target.target, state.revision, ticket);
    return result; // 默认显式请求 DATA | REPAINT；不等待 UI。
}
```

绘制时用 `binding.Acquire(display)`，校验 `display.bytes().bytes` 后读取，再释放 `DataDisplay`。租约内字节不随后续发布或显示交换变化。不要把 acquire 失败当成空的新数据；可以保留原绘制或显示未就绪状态。

`DataSource`、`DataBinding`、`DataDisplay` 和 `RefreshTicket` 都是独占、可移动的包装。不能从不同线程同时移动、析构和调用同一个对象。服务 context 必须比它们活得更久；在 stop 中显式清理并检查结果，失败时保留对象重试，不能依赖 destroy 后的析构。

## 失效、配额和停止

目标重建、工程代次变化、来源或绑定注销使旧绑定失效；单纯几何变化保留绑定，但局部矩形仍须严格匹配几何修订。工程切换后重新注册来源、取得目标并绑定。

当前能力上限为每插件 64 个来源、128 个绑定、128 个显示租约、1 个活动 builder、256 个保留 DATA 贡献；单来源和显示各 256 KiB，每插件保留载荷 2 MiB，进程总载荷 32 MiB。旧贡献和租约保留的版本也计费，不只计算最新模型。运行期以能力查询为准，刷新票据另受原队列配额限制。数量或保留总量超额返回 BUSY，描述容量不合法或字节超出描述容量返回 ARGUMENT，分配失败返回 MEMORY。

停止后拒绝新发布、绑定、显示 acquisition 和刷新，取消待执行贡献。已有租约继续可读；宿主在调用、票据或租约未释放时保留 context、实例和 DLL。插件停止生产数据、结束工作线程，释放票据与显示租约，并在 UI 线程清理绑定和来源；清理未完成返回 BUSY，不阻塞 UI。

常见错误：ARGUMENT 为非法结构、跨 owner 或超描述容量；CONFLICT 为版本冲突、重复来源 id、目标已有绑定或重复写 builder；STALE 为失效身份或来源版本；THREAD 为线程错误；BUSY 为只读因果限制或配额；NOT_READY 为未发布、未挂载或停止；FAILED 为 prepare 未提供完整候选或其他准备失败。
