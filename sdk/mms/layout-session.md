# 临时布局会话

`mms.ui.layout.session` 在原控件上临时改变显隐、尺寸和顺序。1.0 支持控件挂载区，1.1 增加工作区 PANEL，1.2 增加同挂载区域内临时换组。各版本复用 `mms.ui` 注册的元素 handle、NativeUi 排版和已有布局历史，不创建另一套界面对象。

## 环境和支持范围

插件为 Windows x64 DLL。C 开发者包含 `mms/layout_session.h`；C++17 开发者可包含 `mms/cpp/layout_session.hpp`。服务不要求特定 GPU 或新系统绘图 API，继续支持 Win7 SP1 x64 基础路径。插件必须查询服务和目标能力，不能根据宿主版本推断支持。

1.0 支持本插件在工具栏、属性扩展行、左侧栏和右侧栏下注册的 BUTTON、GROUP、SEPARATOR、INPUT。1.1 另支持本插件直接注册于 `mms.workspace.tabs.extensions` 的 PANEL。显隐要求 `MMS_UI_HIDE`，尺寸要求 `MMS_UI_RESIZE`，顺序要求 `MMS_UI_LAYOUT`。1.2 换组要求 `MMS_UI_MOVE`，只允许上述非 PANEL 控件移入同挂载根的本插件容器 GROUP 或该根。原生本体控件、其他插件目标、DOCK 和跨挂载根移动未开放；它们仍保留在完整扩展需求中。

manifest 的 `requiresServices` 可声明 `{"id":"mms.ui.layout.session","major":1,"minMinor":0}`。只有插件必须依赖该功能才能工作时才列为必需服务；可选功能在启动后查询，失败时禁用对应功能。

依赖工作区会话时将 `minMinor` 设为 1，并显式查询服务 1.1。1.0 的挂点掩码保持 15，拒绝 PANEL 的 `queryTarget` 和 `begin`；1.1 的掩码为 31，新增 `MMS_LAYOUT_SESSION_WORKSPACE=16`。两版表均为 88 字节、9 个函数，记录的版本仍为 1.0。现有会话可通过同插件的任一版本表查询、应用、提交、回退和释放，不复制句柄或会话状态。

## 调用规则

所有调用都在 UI 线程执行。后台任务用宿主队列投递；修改必须在回调之外的安全点执行。服务表的 `context` 属于当前插件实例，不得传给其他插件或跨卸载使用。

旧输出结构先设置 `h={sizeof(结构),1,0}`。失败不修改输出。旧输入数组的 `MmsSpan` 使用精确的结构大小作为 `stride`，`recordVersion=1`；各记录使用精确 `h.size`，保留字段为零。`apply` 中未声明字段的对应值也为零。`visible` 仅接受 0 或 1。1.2 扩展声明／值记录的版本见下节，不能与旧数组混用。

尺寸采用布局系统的 96 DPI 逻辑单位，宽高必须是有限正数、不超过 32768，并满足目标已有的最小／最大约束。它们是排版首选尺寸，实际窗口矩形仍由宿主结合空间与布局提示决定；通过已有布局快照读取实际几何。

| 函数 | 行为 |
| --- | --- |
| `queryCapabilities` | 查询字段、挂载区域和资源上限。当前每会话 64 个目标、每插件 32 个会话、全局 256 个会话；终态未释放句柄仍计入额度。 |
| `queryTarget` | 按原元素 handle 查询本插件目标的静态字段和挂载区域能力。当前拖拽或会话冲突不会改写静态能力；实际开始时仍可能返回 `BUSY` 或 `CONFLICT`。 |
| `begin` | 一次声明不重复的目标及字段，只占有字段，不改变界面。初始修订为 1，输入数据在返回前复制。 |
| `apply` | 携带精确修订，按原目标顺序提交所有仍持有字段的值。已完全被接管的目标不放入数组。先验证整批，再发布；失败不部分应用。 |
| `query` | 查询修订、状态、累计撤回原因、原始目标数及仍有字段的目标数。 |
| `queryClaim` | 按 `begin` 原始索引和精确修订读取剩余字段。部分接管不会重排索引；终态字段全部为零。 |
| `rollback` | 撤回仍属于会话的覆盖，显露当前持久基线。不会写回开始时的旧快照。 |
| `commit` | 复用原布局事务验证与历史，把仍持有的有效修改作为一次正式偏好提交。失败保留会话；无偏好变化不增加历史。 |
| `release` | 释放终态句柄。活动会话返回 `BUSY`，须先回退或提交。 |

临时应用与回退不保存布局偏好、不增加场景或正式布局历史、不改变播放头和工程脏标记。正式提交使用独立的布局偏好历史，不是模型／动作共同历史。正式发布前的准备失败不能撤回其他会话。

## 接管与生命周期

### 1.2 临时换组

查询服务 `major=1, minor=2` 时传入完整的 `MmsUiLayoutSessionApiV1_2`（128 字节）。其前 88 字节保留旧函数顺序，前缀回调继续使用 1.1 的字段掩码 7 和挂载掩码 31。表尾追加以下函数；旧 reserved 字段没有被复用：

| 函数 | 1.2 扩展规则 |
| --- | --- |
| `queryCapabilitiesEx` | 仍输出头为 1.0 的 `MmsUiLayoutSessionCapsV1`，字段掩码增加 `MMS_LAYOUT_SESSION_PARENT=8`。 |
| `queryTargetEx` | 仍输出头为 1.0 的目标能力；逐目标判断 Parent 是否可用，PANEL 不提供 Parent。 |
| `beginEx` | 输入 `MmsUiLayoutSessionTargetV1_2`（32 字节），含 element、fields、reserved、parent。Parent 被选择时 parent 必须为有效目标 handle，否则必须为零。只声明并捕获依赖，不立即换组。 |
| `applyEx` | 输入 `MmsUiLayoutSessionValueV1_2`（48 字节），在旧值字段后追加 parent。仍提交全部剩余字段，允许整批改变父关系；未选择 Parent 时 parent 必须为零。 |
| `queryClaimEx` | 输出 `MmsUiLayoutSessionTargetV1_2`，查询会话当前的声明／应用父节点。Parent 字段已被接管或会话终止时 parent 为零。不是通用的当前布局父节点查询。 |

上述新声明和值记录的 `h={sizeof(记录),1,2}`，数组使用 `recordVersion=2` 和精确 stride；共享能力、目标能力及会话状态记录仍使用头 1.0。表、记录和数组的版本分别核对，不能只把旧表的 minor 改为 2。

`query`、`rollback`、`commit`、`release` 继续使用原函数和同一会话 handle。同插件旧版表也能查询状态、撤回、提交和释放 1.2 会话；旧 `queryClaim` 遇到仍含 Parent 的声明返回 `UNSUPPORTED`，输出保持不变。旧 `apply` 不能丢掉仍被会话持有的 Parent 字段；必须使用 `applyEx`。不要因旧接口无法表达 Parent 而创建第二个会话或重新注册控件。

最终父关系图必须无环，整批验证会考虑其他活动会话。目标注销、同名重建、父链变化或其他会话撤回导致依赖失效时，宿主撤回不再有效的 Parent 字段；已撤回字段不会自行恢复。正式提交只生成一次实际变化的布局偏好历史；可以通过布局历史服务撤销和重做。

同挂载根换组保留原 HWND 和 INPUT 草稿。临时移入隐藏组暂停输入并撤销排队的提交，回退后恢复，不抢焦点；正式提交到隐藏组后采用持久隐藏的销毁／重建规则。子树刷新使用实际有效父关系，旧拓扑的待处理票据失效。此服务是真实排版控制，不能用它代替临时视觉变换。

必需依赖换组的插件声明 `minMinor:2`；可选功能自行协商失败后的降级，不把无换组能力伪装成成功。

临时隐藏 INPUT 或其 GROUP 时，宿主保留已经挂载的输入框及未提交文字。隐藏会撤销排队中的失焦提交，隐藏期间的 Enter 不提交。恢复显示不主动获取焦点。尺寸和顺序改变继续使用原输入框；临时修改本身不形成数据历史。持久隐藏、属性失效、只读状态变化及插件卸载仍按原有规则取消草稿或卸载控件。

持久基线隐藏的 INPUT 可以由会话临时显示。显示期间仍使用原属性编辑入口，显式提交可进入共同历史；撤回最后一个显示覆盖后，宿主按持久隐藏规则卸载输入框。多个覆盖组合时，任一没有临时覆盖的持久隐藏祖先仍使控件不可用。

隐藏正在组合输入的字段时，宿主请求输入法取消组合，不把该过程当作属性提交。同步输入法消息中的刷新延后至输入控件退出消息处理后，避免提前销毁控件。实际候选窗位置、组合文字与选择范围的视觉表现仍需输入法实测。

工具栏溢出菜单打开后，如果布局修订或宿主排版参数改变，宿主丢弃该次旧菜单选择。重新打开菜单使用最新布局及原命令身份。此规则不扩大 INPUT 或其他挂载区的溢出菜单支持范围。

工作区 PANEL 使用原页签和 surface。首次选中时才创建视图；已创建页面在临时隐藏或重排时保留实例及插件内容。临时隐藏选中页时自动选择可见页，撤回后按原元素 handle、视图 HWND 和实例身份恢复原页，不主动获取焦点。永久隐藏、状态隐藏、视图替换、注销或插件停止会使恢复记录失效。

用户主动切页会撤回工作区的会话字段并取消自动恢复。宿主先记录用户点击的原元素 handle，再撤回预览、重建页签并按身份选择；重排前后的索引不同也不会选错页面。如果该页仅由预览显示、撤回后再次隐藏，则保留可见候选页。此接管不撤回同会话在工具栏等其他区域的字段。

成功的持久布局修改会接管相交字段；用户拖拽会接管受影响区域。侧栏拖拽占有五个已支持区域（包括工作区），底部拖拽占有属性扩展行。拖拽起点在撤回预览并恢复真实布局之后记录。拖动期间相交的新预览返回 `BUSY`；取消拖拽也不恢复已经被用户接管的插件预览。

`state` 是 ACTIVE、ROLLED_BACK、COMMITTED 或 REVOKED。`reasons` 是累计位集合，分别表示持久编辑、目标变化、宿主交互、插件停止及修订耗尽。部分接管可以保持 ACTIVE；因此插件应先查询修订，再用 `queryClaim` 获取剩余字段，不能只检查状态。

目标删除或同名重建不会让旧 handle 重新绑定。插件开始停止时就撤回预览，包括其 `stop` 返回 `BUSY` 的情况。停止期间可以查询、回退和释放，不可以开始、应用或正式提交。必须在插件上下文失效前释放句柄；C++ 对象的析构不会自动跨线程调用宿主。

过期修订返回 `STALE`，终态应用返回 `NOT_READY`，其他插件的会话返回 `NOT_FOUND`。不要自动读取新修订后重试旧意图；先处理用户接管或目标变化，再决定下一步。

## 最小 C 流程

下面代码假定处在 UI 安全点，`element` 是本插件已注册、查询能力后确认支持显隐的元素。`retainedSession` 指向插件状态中初始为零的句柄字段；错误返回也保留已取得的句柄，直到后续完成回退和释放。

```c
MmsUiLayoutSessionApiV1 api = {0};
MmsString service = {"mms.ui.layout.session", 21};
MmsResult result = host->queryService(host->context, service, 1, 0,
                                    sizeof(api), &api);
if (result != MMS_OK) return result;

MmsUiLayoutSessionTargetV1 target = {0};
target.h = (MmsHeader){sizeof(target), 1, 0};
target.element = element;
target.fields = MMS_LAYOUT_SESSION_VISIBLE;
MmsSpan targets = {&target, 1, sizeof(target), 1};
if (*retainedSession) return MMS_E_BUSY;
result = api.begin(api.context, targets, retainedSession);
if (result != MMS_OK) return result;
const MmsHandle session = *retainedSession;

MmsUiLayoutSessionInfoV1 info = {0};
info.h = (MmsHeader){sizeof(info), 1, 0};
result = api.query(api.context, session, &info);
if (result != MMS_OK) return result;

MmsUiLayoutSessionValueV1 value = {0};
value.h = (MmsHeader){sizeof(value), 1, 0};
value.element = element;
value.fields = MMS_LAYOUT_SESSION_VISIBLE;
value.visible = 0;
MmsSpan values = {&value, 1, sizeof(value), 1};
result = api.apply(api.context, session, info.revision, values);
/* To end: query current state/revision, rollback if ACTIVE, then release.
   Handle STALE or takeover explicitly; keep the handle if release fails. */
return result;
```

## C++17 包装

`mms::sdk::LayoutSessionServices::Query` 协商同一张 C 表。`Capabilities`、`Target` 和 `Begin` 转发对应调用；`BeginVisibility`、`BeginSize`、`BeginOrder` 只构造单目标声明，再调用同一个 `Begin`。多目标插件继续使用 `LayoutSessionTarget` 构造完整数组，不需要更换对象或服务。

`LayoutSessionServices::Query(host, services)` 保持查询 1.0；工作区使用 `Query(host, services, 1)`。包装校验返回表的版本，不隐式降级；如工作区功能可选，由插件显式选择是否再查询 1.0 并禁用 PANEL 会话。服务 1.1 不改变记录 header 的 minor，仍使用零。

换组使用 `Query(host, services, 2)`，然后通过 `CapabilitiesEx` 和 `TargetEx` 查询扩展能力。`BeginParent(element, parent, session)` 与三参数 `LayoutSessionTarget(element, fields, parent)` 生成 1.2 声明；`LayoutSessionParent(element, parent)` 生成值。新的 typed `Begin`／`Apply`／`QueryClaim` 重载自动选择 Ex 调用，手写 `MmsSpan` 则显式调用 `BeginEx`／`ApplyEx`。旧重载及旧 `Capabilities`／`Target` 继续只表达原字段，低版本服务的 Ex 包装返回 `UNSUPPORTED`，不偷偷降级。

```cpp
// services and retainedSession belong to plugin state, not a temporary local.
// This function runs at a UI safe point after querying TargetEx capabilities.
MmsResult PreviewParent(mms::sdk::LayoutSessionServices& services,
                        mms::sdk::LayoutSession& retainedSession,
                        MmsHandle element, MmsHandle parent) {
    auto result = services.BeginParent(element, parent, retainedSession);
    if (result != MMS_OK) return result;
    MmsUiLayoutSessionInfoV1 info{};
    result = retainedSession.Query(info);
    if (result != MMS_OK) return result;
    const auto value = mms::sdk::LayoutSessionParent(element, parent);
    return retainedSession.Apply(&value, 1);
    // The owner later Query()s the actual revision, then Close()s to roll back,
    // or Commit()s and Release()s. Errors retain the handle for explicit cleanup.
}
```

1.2 下 `api()` 是单独保存、大小为 88 的 1.1 前缀视图，不能强转为扩展表。`extended_api()` 返回完整 1.2 表的借用指针，低版本返回空。需要把会话转回 C 时，必须在 `Detach()` 前**按值复制**完整表；只保存借用指针无效，因为 Detach 会清空包装内的表。转移及关闭仍处理同一个句柄。

`LayoutSession` 显式保存原会话 handle。开始后先调用 `Query(info)`，包装只记录宿主实际返回的修订。用 `LayoutSessionVisibility`、`LayoutSessionSize` 或 `LayoutSessionOrder` 构造完整值数组后调用 `Apply`；未写字段保持零。成功修改后缓存的修订失效，下一次修改前再次显式查询。包装不会猜测修订加一，也不会自动重试 `STALE`。

同一目标同时声明多个字段时，应合并为一条值记录；同一目标不能为显隐和尺寸各放一条。多目标应用须提交所有仍持有字段的目标，按原声明顺序排列，不能用单目标便捷值覆盖整个多目标会话。

`Close()` 对活动会话使用已经查询的修订回退，然后释放；未查询修订时返回 `NOT_READY`。成功回退后若释放失败，会话对象仍保留终态句柄，下次 `Close()` 只重试释放。已被用户撤回的会话先 `Query` 确认终态，再关闭。`Release()` 失败也保留句柄。

复制和移动赋值禁用。向已有对象转移用 `TransferFrom`，目标已占有句柄时返回 `BUSY`。析构不调用宿主；`Detach` 只是把清理责任移回调用方。成功取得服务的 UI 线程执行所有宿主调用；包装的线程检查不代替调用方对对象访问的串行化。

本服务改变真实排版；逐帧视觉变换、绘制层、透明窗口和动画调度不由它提供。需要动画的插件必须按对应服务的实际能力选择实现，不能通过循环改布局冒充完整呈现接口。
