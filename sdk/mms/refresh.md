# 界面刷新接口参考

`mms.ui.refresh` 1.0 为插件自己的工具栏按钮和原生 surface 面板安排异步刷新。接口定义见 [refresh.h](include/mms/refresh.h)。结构使用 `MmsHeader`、`MMS_CALL` 和 8 字节对齐，调用失败不修改输出参数。

当前支持 `REPAINT + SELF`，以及已创建宿主按钮的 `STATE + SELF`。STATE 更新启用、选中及受支持的显隐；面板不报告 STATE。已绑定目标还支持 `DATA + SELF`，见[数据绑定接口](data-refresh.md)。原生布局后端为已有按钮和已挂载 surface 提供 `LAYOUT + SELF`，按保留的布局树重新测量和排列。SUBTREE、DEPENDENTS、来源订阅、自动依赖、插件量测提供者和动画帧尚未提供。

组合请求在同一队列中按 DATA、STATE、LAYOUT、REPAINT 执行。DATA 准备并交换不可变显示模型；STATE 只读查询目标按钮的 getState；LAYOUT 使用已保留的描述、提示和状态排版；REPAINT 调用 `InvalidateRect` 或 `InvalidateRgn`，不直接执行绘制。刷新不调用 `FrameDriver`，不修改播放头、工程 dirty 或撤销历史，也不保存布局偏好。插件窗口后续的正常 `WM_PAINT` 仍由插件处理。

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
| `supportedKinds`、`supportedScopes` | 宿主按钮报告 REPAINT、STATE；面板报告 REPAINT。原生布局后端支持的已有按钮及已挂载 surface 额外报告 LAYOUT；有效绑定额外报告 DATA。范围均为 SELF。全局能力是并集，调用前还须检查目标能力。 |
| `ready` | 已有可用窗口、布局发布成功且客户区宽高均大于零时为 1。 |
| `visible` | 目标在插件布局中的逻辑可见性。此值不表示窗口无遮挡，也不要求宿主顶层窗口已显示。 |
| `width`、`height` | 目标客户区的 DIP 尺寸。 |

目标引用必须从 `getTarget` 取得。`MmsUiElementV1.generation`、布局快照修订和原生 HWND 都不能代替 `instanceGeneration`。

尚未创建窗口的元素返回 `NOT_READY`，查询不会创建或显示 surface。已创建但隐藏的目标可以有 `ready=1`、`visible=0`，仍可接受重绘请求。请求不强制显示窗口。

布局开始移动窗口前暂停目标和公开几何。暂停期间 `getTarget` 可以成功返回 `ready=0`，新请求返回 `NOT_READY`，新布局快照也返回 `NOT_READY`。只有完整成功的布局才重新发布；失败或分配异常不会继续接受旧坐标，已有不可变快照租约仍可读。暂停本身不增加几何修订，实际几何未变化的再次布局保留原修订和实例代次。

已收缩为零尺寸的目标仍可保留窗口和同一目标引用。它的 `ready=0`，但在未暂停、实例和工程有效且目标支持 LAYOUT 时，可以接受含 LAYOUT 的请求。恢复按钮命令的可见性应使用 STATE 与 LAYOUT 组合；纯 REPAINT 或 STATE 不绕过零尺寸检查。

注销元素、销毁 view 或更新导致目标失效时，旧的待执行票据以 `STALE` 结束。工程上下文切换同样使旧目标引用失效。单纯移动和缩放保持实例代次，但改变几何修订。

## 请求字段

`MmsUiRefreshRequestV1.h` 和 `target.h` 均填写本结构大小、major=1、minor=0。其余字段须明确初始化。

| 字段 | 当前要求 |
| --- | --- |
| `target` | 完整复制 `getTarget` 返回的引用。 |
| `binding`、`sourceRevision` | 含 DATA 时填写有效绑定与当前非零来源版本；不含 DATA 时必须为 0，见[绑定规则](data-refresh.md)。 |
| `kinds` | REPAINT、STATE、DATA、LAYOUT 或其组合；目标须支持所请求的全部种类。 |
| `scope` | 必须为 `MMS_UI_REFRESH_SELF`。 |
| `expectedGeometryRevision` | 局部矩形请求必须填写取得矩形时的非零几何修订。整目标请求可填 0，表示接受执行时的有效几何；填写非零值时仍严格检查。 |
| `dirtyRects` | REPAINT 的局部数组；非空时 stride 至少为结构大小、recordVersion=1。count=0 表示整个目标客户区。不含 REPAINT 时必须 count=0。 |
| `coalesceKey` | 插件自定的合并键。同一插件、目标实例及工程代次内，相同键的待执行贡献可以合并。0 也是有效键。 |
| `flags`、`reserved` | 必须为 0。 |

矩形的 `x`、`y` 是目标客户区左上角起算的局部 DIP，`width`、`height` 是非负 DIP。每个值必须有限且绝对值不超过 1,048,576。负的起点可以表示被客户区裁剪的区域。

宿主把矩形裁剪到目标客户区，再按该窗口坐标空间的 DPI 向外取整为像素区域。多个矩形取并集；零面积或完全位于客户区外的矩形不产生损伤。没有矩形表示整个目标，不表示整个 MMS 窗口或场景。

宿主在接收和执行时都检查实例、工程代次及非零的几何修订。带旧修订的矩形返回 `STALE`，不会自动改成全窗口重绘。零几何修订只免除整目标请求的几何匹配，不能绕过实例、工程或布局暂停检查；零尺寸恢复仅适用上述含 LAYOUT 的请求。

## 票据状态与合并

每次成功的 `request` 拥有一个独立票据。多个票据可共享一次区域失效操作，但每个票据独立取消、查询和释放。

| `status` | 含义 |
| --- | --- |
| `PENDING` | 已接受，等待 UI 安全点处理。 |
| `RUNNING` | 已进入本次处理批次，不能再取消。 |
| `COMPLETED` | 请求的各刷新阶段均成功；`result=MMS_OK`，completedKinds 为请求种类。 |
| `CANCELLED` | 在执行前取消，`result=MMS_E_CANCELLED`。 |
| `FAILED` | 校验或窗口失效操作失败；`result` 保存错误码。 |

完成不表示像素已显示，也不等待 WM_PAINT。隐藏或被遮挡的窗口遵循 Windows 的正常绘制时机，服务不保证隐藏窗口保留可查询的更新区域。sourceRevision 在 DATA 成功时记录实际消费版本，否则为 0；geometryRevision 记录处理时的目标几何，执行前即失败的票据可为 0。

合并只处理本批待执行贡献，要求相同插件、目标、coalesceKey、kinds、binding、sourceRevision 和几何约束策略。固定修订请求与接受当前几何的请求分组处理，避免过期局部请求使合法整目标请求失败。同组 STATE 只计算一次；任一整目标重绘贡献会覆盖同组局部矩形。不同 kinds 独立处理，STATE 失败不会吞掉另一张纯 REPAINT 票据。取消一个 PENDING 票据仅撤回自身贡献。cancel 对终态票据返回成功，不改变既有结果。

组合请求在某一阶段失败后停止后续阶段，FAILED 票据的 completedKinds 保留已成功的阶段位。例如 DATA 和 LAYOUT 已成功、后续局部重绘因几何变化返回 STALE 时，仍保留这些完成位。UI 状态不属于工程事务，不承诺对已发生的原生窗口变化执行事务回滚。

## 按钮状态刷新

STATE 只重新查询目标按钮的命令，更新原 HWND 的 enabled、checked 和受支持的 visible。不调用命令，不刷新共享同一命令的菜单，不读取属性或改变标签。单独 STATE 不改变尺寸和布局；重排需显式组合 LAYOUT。按钮以手动选中、推按钮外观呈现；点击不会自行切换选中来源，命令 getState 仍是唯一来源。Enter 和普通命令激活保留。

单独 STATE 的显隐要求最近成功布局已为按钮保留正尺寸槽位，且有效布局提示包含 `MMS_HINT_PRESERVE_HIDDEN_SLOT`。用户覆盖可能移除此提示，因此目标支持 STATE 不代表单独 STATE 支持任意显隐。缺少提示时，变化以 FAILED/UNSUPPORTED 结束；布局提示修改后尚未重新发布有效布局时，以 FAILED/NOT_READY 结束，均在修改启用和选中前拒绝。此路径隐藏时保留原位置、尺寸及相邻控件位置，不测量或收缩空位。无保留槽位的收缩与恢复使用下述 STATE 与 LAYOUT 组合。

恢复显示遵守最近布局的祖先可见性，不会显示隐藏的父区域。父区域隐藏时，按钮命令状态仍可更新，但目标逻辑 visible 保持 0。隐藏后若焦点仍在该按钮，宿主将焦点移至可用的相邻控件或父窗口；恢复不抢回焦点。

禁用当前聚焦的按钮也遵循相同规则。宿主优先选择同一父区域中可用的下一个 Tab 控件，没有可用控件时选择可见且启用的父窗口；父链均不可用时清除焦点。原生启用或显隐回调已经移动焦点时，宿主保留该选择。重新启用按钮不抢回焦点，同值请求不重复启用窗口或移动焦点。

逻辑可见性变化增加目标 geometryRevision，并更新 `mms.ui.layout` 新快照中的可见性和几何修订；已取得的快照租约保持原值。元素身份、实例代次、UI 修订与偏好修订保持不变。严格匹配旧几何的后续请求会过期；不含坐标的 STATE 可填 expectedGeometryRevision=0，C++ SDK 提供 `RefreshStateCurrent`。实例、工程代次及 ready 仍须有效。

request 返回 OK 只表示成功入队，不在后台调用 getState。面板能力不支持时直接拒绝入队；回调输出、显隐及执行时过期错误在 UI 处理后写入票据，调用者必须用 query 检查结果。

回调运行在 UI 线程，并保持只读回调保护直到状态应用结束。回调内的注册、注销及命令执行返回 BUSY，不继承编辑权限；在同插件的 DATA/STATE/LAYOUT 因果链中再次请求 DATA、STATE 或 LAYOUT 返回 BUSY，限制也传播到该链安排的 UI 任务、取消回调及后代任务，避免递归刷新。回调必须快速返回已准备状态，不能等待需要 UI 的后台作业。

宿主在回调之后、每组实际执行前及原生状态应用后重新检查实例、工程、几何和就绪状态。回调泵消息导致目标过期时，请求失败，不使用旧坐标。原生显隐回调中取得的是上次发布的目标；显隐发布后，基于旧几何的局部请求会过期。显隐过程中工程切换会暂停原目标，等待正常布局重新发布。状态未变化时不重复修改原生状态。未挂载目标仍为 NOT_READY；零尺寸恢复遵守 LAYOUT 规则。

## 保留布局刷新

LAYOUT 使用现有布局树中的元素描述、布局提示、文字量测和已缓存的命令可见性重新排版。纯 LAYOUT 不查询所有命令状态，也不读取属性或准备 DATA。它不会创建尚未挂载的 surface；已有 surface 只收到必要的尺寸及显隐回调。失败面板的错误提示不会因布局请求而被清除。

按钮 STATE 与 LAYOUT 组合可以在没有保留槽位提示时隐藏并收缩空位，也可以用同一目标引用恢复零尺寸按钮。需要重新查询命令可见性时，不能只发 LAYOUT。SELF 指请求的目标范围；保留树的重排可能移动同一布局中的相邻元素，不等于提供 SUBTREE、DEPENDENTS 或自动依赖订阅。

宿主先应用原生布局，更新依赖该区域的视口几何，再发布目标和布局快照。此路径不调用 `HandleWindowSize`、`FrameDriver` 或偏好保存。布局回调处于只读保护，surface 提供者及其因果任务也受保护。它们不能借本次回调执行编辑、注册或递归 DATA/STATE/LAYOUT。

布局失败可能发生在部分 HWND 已移动之后。宿主暂停目标和新几何快照，不把部分结果发布为就绪，也不承诺回滚已移动窗口。后续正常宿主布局成功后恢复发布；旧快照租约保持可读。

局部矩形使用捕获时的目标坐标。若同一请求的 LAYOUT 改变几何，旧矩形不能用于新布局，票据以 FAILED/STALE 结束，保留已成功的 completedKinds，不自动扩大为整目标重绘。需要随布局重绘时应提交无局部矩形、几何修订为 0 的整目标请求；完成后再重新查询几何以计算下一次局部损伤。

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

停止开始后，数据绑定停止接收新发布和显示租约，已有显示租约仍需释放；能力查询、目标查询和新请求返回 `NOT_READY`。宿主取消待执行贡献；已有票据仍可查询、取消和释放。正在调用服务或仍有票据引用时，宿主保留插件实例、服务 context 和 DLL。

插件的 `stop` 必须停止产生新请求、结束自己的工作线程并释放票据。线程尚未退出或清理尚未完成时返回 `BUSY`，不能阻塞 UI 等待它们。刷新票据没有插件完成回调。

## 错误码

| 错误 | 典型原因 |
| --- | --- |
| `THREAD` | 在工作线程调用 `getCapabilities` 或 `getTarget`。 |
| `ARGUMENT` | 非法结构、未知标志、非法矩形、缺少 DATA 绑定及版本、非 DATA 请求的非零 binding 或 sourceRevision、访问其他插件目标。 |
| `UNSUPPORTED` | 请求 SUBTREE、DEPENDENTS；后端或目标不支持 LAYOUT、STATE；单独 STATE 请求没有保留槽位提示的显隐变化；或查询按钮和面板以外的元素。 |
| `NOT_READY` | 停止中、目标未挂载、布局暂停、零尺寸目标请求不含可用的 LAYOUT，或显隐需要的布局尚未重新发布。 |
| `STALE` | 元素已注销、实例重建、工程代次或要求的几何修订不匹配。 |
| `NOT_FOUND` | 票据不存在、已释放或属于其他插件。 |
| `BUSY` | 配额已满、取消执行中的贡献，或在同插件的 DATA/STATE/LAYOUT 只读因果链中再次提交 DATA/STATE/LAYOUT。 |
| `MEMORY`、`FAILED` | 内存、GDI 区域、调度唤醒或窗口失效操作失败。 |

实现仍保留 Win7 的窗口 DC DPI 回退，重绘依赖 GDI，不要求特定显卡。代码接入状态不代替 Win7、AMD、多 DPI 和真实桌面绘制验收。
