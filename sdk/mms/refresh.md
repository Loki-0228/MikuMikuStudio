# 界面重绘接口参考

`mms.ui.refresh` 1.0 为插件自己的工具栏按钮和原生 surface 面板安排异步重绘。接口定义见 [refresh.h](include/mms/refresh.h)。结构使用 `MmsHeader`、`MMS_CALL` 和 8 字节对齐，调用失败不修改输出参数。

当前只支持 `MMS_UI_REFRESH_REPAINT` 与 `MMS_UI_REFRESH_SELF`。`DATA`、`STATE`、`LAYOUT`、`SUBTREE` 和 `DEPENDENTS` 返回 `MMS_E_UNSUPPORTED`。刷新绑定、来源数据订阅和动画帧回调尚未提供。

重绘在 UI 安全点调用 `InvalidateRect` 或 `InvalidateRgn`，不直接执行绘制。此服务不查询命令状态、不重新布局、不调用 `FrameDriver`，也不修改播放头、工程 dirty 或撤销历史。插件窗口随后收到的正常 `WM_PAINT` 仍由插件处理。

## 服务查询与线程

服务通过 `host.queryService(host.context, name, 1, 0, sizeof(MmsUiRefreshApiV1), &api)` 查询，其中 `name` 是 UTF-8 的 `mms.ui.refresh`。依赖该服务的插件在清单 `requiresServices` 中声明相同名称及版本。

`MmsUiRefreshApiV1` 大小为 64 字节。服务查询在 UI 线程执行；取得函数表后，各项调用遵循下表。

`getCapabilities`、`getTarget`、`query` 的输出结构在调用前须填写 `h.size`、major=1、minor=0，其余字段可清零。

| 函数 | 线程 | 行为 |
| --- | --- | --- |
| `getCapabilities(context, outCaps)` | UI 线程 | 返回当前支持的请求位、范围和配额。允许在命令回调内纯读。 |
| `getTarget(context, element, outTarget)` | UI 线程 | 从原元素句柄读取目标身份、实际尺寸和可见状态。允许在命令回调内纯读。 |
| `request(context, request, outTicket)` | UI 线程或工作线程 | 校验并复制请求和矩形，成功后返回独立票据。 |
| `query(context, ticket, outState)` | UI 线程或工作线程 | 读取本插件尚未释放的票据状态。 |
| `cancel(context, ticket)` | UI 线程或工作线程 | 取消尚未开始的贡献；执行中返回 `BUSY`。 |
| `release(context, ticket)` | UI 线程或工作线程 | 放弃状态查询引用，不取消已经接受的请求。 |

后台调用只访问宿主保留的目标快照和队列，不读取 HWND 或注册表。函数表、服务 context 和插件实例必须保持有效，直到宿主完成停止。插件不能在 UI 线程同步等待票据完成。

现有 C++17 SDK 继续返回同一注册句柄。例如，`CommandUi::button().handle` 可以直接传给 `getTarget`。[C++17 重绘封装](cpp-refresh.md)用 `RefreshServices` 和 `RefreshTicket` 包装上述 C 表及票据，不创建第二个按钮或刷新注册。`UiServices` 继续负责界面注册服务。

## 目标身份与几何

`getTarget` 的输入必须是本插件 `MMS_UI_BUTTON` 或 `MMS_UI_PANEL` 的原 `MmsHandle`。命令句柄、surface 工厂句柄和菜单项不是重绘目标。按钮无需声明 `MMS_UI_LAYOUT`。

`MmsUiRefreshTargetV1` 的字段如下。

| 字段 | 含义 |
| --- | --- |
| `target.element` | 原 `mms.ui` 元素注册句柄。 |
| `target.instanceGeneration` | 宿主分配的呈现实例代次。销毁、重建或失效后重新发布会改变该值。 |
| `target.projectEpoch` | 当前工程上下文代次。场景替换或重置后旧值失效。 |
| `geometryRevision` | 当前实际几何修订，包括位置、客户区尺寸、窗口坐标空间 DPI 和逻辑可见性。 |
| `supportedKinds`、`supportedScopes` | 当前目标仅报告 `REPAINT` 和 `SELF`。 |
| `ready` | 已有可用窗口、布局发布成功且客户区宽高均大于零时为 1。 |
| `visible` | 目标在插件布局中的逻辑可见性。此值不表示窗口无遮挡，也不要求宿主顶层窗口已显示。 |
| `width`、`height` | 目标客户区的 DIP 尺寸。 |

目标引用必须从 `getTarget` 取得。`MmsUiElementV1.generation`、布局快照修订和原生 HWND 都不能代替 `instanceGeneration`。

尚未创建窗口的元素返回 `NOT_READY`，查询不会创建或显示 surface。已创建但隐藏的目标可以有 `ready=1`、`visible=0`，仍可接受重绘请求。请求不强制显示窗口。

布局开始移动窗口前暂停目标。暂停期间 `getTarget` 可以成功返回 `ready=0`，新请求返回 `NOT_READY`。只有完整成功的布局才重新发布目标；失败或分配异常不会继续接受旧坐标。暂停本身不增加几何修订，实际几何未变化的再次布局保留原修订和实例代次。

注销元素、销毁 view 或更新导致目标失效时，旧的待执行票据以 `STALE` 结束。工程上下文切换同样使旧目标引用失效。单纯移动和缩放保持实例代次，但改变几何修订。

## 请求字段

`MmsUiRefreshRequestV1.h` 和 `target.h` 均填写本结构大小、major=1、minor=0。其余字段须明确初始化。

| 字段 | 当前要求 |
| --- | --- |
| `target` | 完整复制 `getTarget` 返回的引用。 |
| `binding`、`sourceRevision` | 必须为 0；当前没有刷新绑定和数据来源修订。 |
| `kinds` | 必须为 `MMS_UI_REFRESH_REPAINT`；不能与其他请求位组合。 |
| `scope` | 必须为 `MMS_UI_REFRESH_SELF`。 |
| `expectedGeometryRevision` | 局部矩形请求必须填写取得矩形时的非零几何修订。整目标请求可填 0，表示接受执行时的有效几何；填写非零值时仍严格检查。 |
| `dirtyRects` | `MmsUiDirtyRectV1` 数组；非空时 `stride >= sizeof(MmsUiDirtyRectV1)`、`recordVersion=1`。count=0 表示整个目标客户区。 |
| `coalesceKey` | 插件自定的合并键。同一插件、目标实例及工程代次内，相同键的待执行贡献可以合并。0 也是有效键。 |
| `flags`、`reserved` | 必须为 0。 |

矩形的 `x`、`y` 是目标客户区左上角起算的局部 DIP，`width`、`height` 是非负 DIP。每个值必须有限且绝对值不超过 1,048,576。负的起点可以表示被客户区裁剪的区域。

宿主把矩形裁剪到目标客户区，再按该窗口坐标空间的 DPI 向外取整为像素区域。多个矩形取并集；零面积或完全位于客户区外的矩形不产生损伤。没有矩形表示整个目标，不表示整个 MMS 窗口或场景。

宿主在接收和执行时都检查实例、工程代次及非零的几何修订。带旧修订的矩形返回 `STALE`，不会自动改成全窗口重绘。零几何修订只免除整目标请求的几何匹配，不能绕过 `ready`、实例或工程检查。

## 票据状态与合并

每次成功的 `request` 拥有一个独立票据。多个票据可共享一次区域失效操作，但每个票据独立取消、查询和释放。

| `status` | 含义 |
| --- | --- |
| `PENDING` | 已接受，等待 UI 安全点处理。 |
| `RUNNING` | 已进入本次处理批次，不能再取消。 |
| `COMPLETED` | 损伤区域已提交给窗口系统，或裁剪后没有非空区域。`result=MMS_OK`，`completedKinds=REPAINT`。 |
| `CANCELLED` | 在执行前取消，`result=MMS_E_CANCELLED`。 |
| `FAILED` | 校验或窗口失效操作失败；`result` 保存错误码。 |

完成不表示像素已显示，也不等待 `WM_PAINT`。隐藏或被遮挡的窗口遵循 Windows 的正常绘制时机，服务不保证隐藏窗口保留可查询的更新区域。`sourceRevision` 当前始终为 0；实际失效操作结束后的 `geometryRevision` 记录执行时几何，执行前即失败的票据可为 0。

合并只处理本批待执行贡献。任一整目标贡献会覆盖同组的局部矩形。取消一个 `PENDING` 票据仅撤回该票据，其余贡献仍执行。`cancel` 对终态票据返回成功，不改变既有结果。

`release` 成功后，该句柄再用于 `query`、`cancel` 或 `release` 返回 `NOT_FOUND`。已释放的待执行贡献继续运行并占用配额，终止后才回收。需要撤回贡献时先 `cancel`，再 `release`。

## 配额与停止

运行期上限通过 `getCapabilities` 查询。当前实现返回以下值，插件不应据此假定未来版本仍相同。

| 字段 | 当前值 |
| --- | --- |
| `maxTicketsPerPlugin` | 256。包括尚未释放的终态票据。 |
| `maxRectsPerRequest` | 32。 |
| `maxPendingBytesPerPlugin` | 262,144 字节。计入保留的票据记录及矩形数组容量。 |
| `maxContributionsPerDrain` | 每次安全点最多处理 64 个待执行贡献。 |

进程还有 4,096 个票据、4,194,304 字节的总队列上限。达到票据、矩形或队列字节配额返回 `BUSY`；分配失败返回 `MEMORY`。终态票据会释放矩形数组，但记录本身仍计费，直到 `release`。这些配额约束服务保留的数据，不是对整个 CRT 或插件内存的测量。

停止开始后，能力查询、目标查询和新请求返回 `NOT_READY`。宿主取消待执行贡献；已有票据仍可查询、取消和释放。正在调用服务或仍有票据引用时，宿主保留插件实例、服务 context 和 DLL。

插件的 `stop` 必须停止产生新请求、结束自己的工作线程并释放票据。线程尚未退出或清理尚未完成时返回 `BUSY`，不能阻塞 UI 等待它们。刷新票据没有插件完成回调。

## 错误码

| 错误 | 典型原因 |
| --- | --- |
| `THREAD` | 在工作线程调用 `getCapabilities` 或 `getTarget`。 |
| `ARGUMENT` | 非法结构、未知标志、非法矩形、非零 binding 或 sourceRevision、访问其他插件目标。 |
| `UNSUPPORTED` | 请求 DATA、STATE、LAYOUT、SUBTREE、DEPENDENTS，或查询按钮和面板以外的元素。 |
| `NOT_READY` | 停止中、目标未挂载、布局暂停或客户区尺寸为零。 |
| `STALE` | 元素已注销、实例重建、工程代次或要求的几何修订不匹配。 |
| `NOT_FOUND` | 票据不存在、已释放或属于其他插件。 |
| `BUSY` | 配额已满，或取消执行中的贡献。 |
| `MEMORY`、`FAILED` | 内存、GDI 区域、调度唤醒或窗口失效操作失败。 |

实现仍保留 Win7 的窗口 DC DPI 回退，重绘依赖 GDI，不要求特定显卡。代码接入状态不代替 Win7、AMD、多 DPI 和真实桌面绘制验收。
