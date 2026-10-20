# 控件状态内容与 C++17 SDK

`mms.ui.refresh.state` 1.0 为本插件已有的宿主按钮、声明式 INPUT 及已挂载 PANEL 注册只读状态提供者。它更新原控件的显示文案和提示说明，包括禁用原因。状态内容通过原 `mms.ui.refresh` 的 STATE 阶段执行，不创建控件、第二条刷新队列或独立票据。

C 定义见 [refresh_state.h](include/mms/refresh_state.h)，C++17 封装见 [state_content.hpp](include/mms/cpp/state_content.hpp)。开发环境沿用 [SDK README](README.md#开发环境)。旧刷新表、请求和票据状态结构仍分别为 64、112、40 字节；本服务是独立协商的附加表，不修改旧结构。

当前支持已挂载的宿主 `MMS_UI_BUTTON`、`MMS_UI_INPUT` 和 `MMS_UI_PANEL`。按钮的 enabled、checked、visible 继续由原命令 getState 决定；内容提供者不能改写这些值。INPUT 可额外收紧启用、只读和可见状态，规则见下文。PANEL 更新页签和可用性，不接管插件自绘内容。菜单及其他控件的完整 STATE 尚未开放。

## 服务和原目标

插件清单的 `requiresServices` 声明 `mms.ui.refresh.state` 和 `mms.ui.refresh` 1.0。注册控件仍使用原 `mms.ui`。在宿主 UI 线程查询服务、取得原控件的刷新目标，再注册提供者。

`getCapabilities` 返回当前字段能力和配额。尚无原生内容后端时，supportedFields 为 0。调用前检查服务能力、原目标的 STATE 能力；服务存在不表示窗口已经挂载。

提供者描述包含原 `MmsUiTargetRefV1`、context、prepare 回调和允许覆盖的 fields。每个目标最多一个提供者。只能注册自己的按钮、INPUT 或 PANEL；目标引用须来自 `getTarget`，不能用 HWND、UI generation 或布局修订代替。

| C 函数 | 行为 |
| --- | --- |
| `getCapabilities` | 查询字段及当前配额，失败保留输出。 |
| `registerProvider` | 注册一个提供者并返回句柄；不立即查询或刷新控件。 |
| `updateProvider` | 用同一提供者句柄替换回调、context、字段及该控件的新目标引用；不能迁移到另一个元素。 |
| `unregisterProvider` | 注销提供者；停止期间仍允许清理。 |
| `writePrepared` | 在 prepare 回调中向短期 builder 写入一次完整候选；函数返回前已复制文本。 |

这些函数都限定 UI 线程。工作线程通过原刷新服务提交 STATE 请求，不直接调用本表。后台数据应由插件自行同步，或通过 [DATA 绑定](data-refresh.md)发布不可变显示模型。

## 字段与文本所有权

`MmsUiStateContentV1.fields` 必须是提供者描述允许字段的子集。每次写入都是完整候选，不是对上次提供者结果的增量修改。

| 字段位 | 写入后的含义 | 未设置该位时 |
| --- | --- | --- |
| `MMS_UI_STATE_LABEL` | 原按钮显示文案；后续 LAYOUT 使用已成功显示的文案量测。 | 使用当前注册元素的 label。 |
| `MMS_UI_STATE_DESCRIPTION` | 按钮提示说明。 | 使用当前命令注册信息的 description。 |
| `MMS_UI_STATE_DISABLED_REASON` | 按钮禁用时的原因。 | 使用本次原命令 getState 的 disabledReason。 |

设置字段位并提供空字符串表示清空该字段。未设置的字段须令 bytes 为 0；它回退到注册信息或本次命令状态，不保留上一次提供者的覆盖。fields=0 可以清除全部覆盖。

文本是无内嵌 NUL 的 UTF-8。宿主在 `writePrepared` 内校验、转换并复制文本，插件可以使用栈字符串或随后销毁的临时缓冲。宿主不保留插件字符串指针，不跨 DLL 释放插件分配的内存。

prepare 必须调用一次 `writePrepared` 并快速返回。没有写入、重复写入、非法 UTF-8、越界或回调失败都使该 STATE 失败；忽略写入函数的错误也不会发布候选。builder 在回调结束后失效，后续使用返回 NOT_FOUND，不能传给后台任务。

按钮启用时，原生提示显示 description；禁用时组合 description 和 disabledReason。宿主为禁用按钮保留父区域命中入口。提示使用宿主持有的文本，按钮文案可通过正常 Windows 控件接口读取；本服务没有新增公共显示快照租约。

未注册内容提供者时，原 STATE 也可以显示命令的说明和禁用原因。旧注册信息或旧命令原因超出本服务显示上限时，宿主按有效 UTF-8 边界截取显示文本；INPUT 默认标签也遵守该上限。这不改变原注册信息或命令状态查询接口的结果。提供者显式写入超限文本则返回 ARGUMENT，不截断其候选。

## C++17 用法

`StateContentServices` 包装原服务表，`StateContentProvider` 独占原提供者句柄。二者不持有另一套 UI 注册。以下状态对象应保留到插件停止完成；`button` 是已经注册的原按钮句柄。

```cpp
#include <mms/cpp/state_content.hpp>

struct ButtonPresentation {
	mms::sdk::StateContentServices contents;
	mms::sdk::RefreshServices refresh;
	mms::sdk::StateContentProvider provider;
	std::string label = "处理";
	std::string description = "处理所选对象";
	std::string disabledReason;

	static MmsResult MMS_CALL Prepare(
		void* context, const MmsUiTargetRefV1*, MmsHandle builder) {
		auto& self = *static_cast<ButtonPresentation*>(context);
		return self.contents.Write(builder,
			self.label, self.description, self.disabledReason);
	}

	MmsResult Bind(const MmsHostV1& host, MmsHandle button) {
		if (provider) return MMS_E_BUSY;
		auto result = mms::sdk::StateContentServices::Query(host, contents);
		if (result) return result;
		result = mms::sdk::RefreshServices::Query(host, refresh);
		if (result) return result;
		MmsUiRefreshTargetV1 target{};
		result = refresh.Target(button, target);
		if (result) return result;
		return contents.Register(target.target,
			MMS_UI_STATE_LABEL | MMS_UI_STATE_DESCRIPTION |
			MMS_UI_STATE_DISABLED_REASON,
			Prepare, this, provider);
	}

	MmsResult Request(mms::sdk::RefreshTicket& ticket) const {
		return provider.Refresh(refresh, ticket, true);
	}
};
```

`Bind` 在 UI 线程执行。上例中的三个字符串也由 UI 线程修改；后台工作不能同时改写它们。工作线程可以调用 `Request`，但插件须保证这期间 provider、服务表和 context 不被更新、移动或释放。后台任务完成后可先通过 UI 队列修改显示状态，再提出刷新。

`provider.Refresh(refresh, ticket, true)` 组合原 STATE、LAYOUT、REPAINT，使用原 SELF 目标、几何修订 0。它适合文案长度或可见性变化后重新排版。最后一个 bool 为 false 时，只请求原 STATE。也可以用 `RefreshServices::Request` 组合 DATA；规则与 [C 刷新参考](refresh.md)相同。

`contents.Write(builder, label, description, disabledReason)` 设置全部三个位。只覆盖部分字段时，构造 `MmsUiStateContentV1`，填写结构头及字段位，再调用 `contents.Write(builder, content)`；未设置字段保持上述回退规则。

## PANEL 状态

PANEL 使用原 64 字节内容结构、提供者和 STATE 票据。label 更新原页签文本，description 和 disabledReason 用于页签提示，不改写元素注册的默认标签。未设置 label 时恢复注册标签；没有 commandId 的面板默认启用，按注册可见性显示，说明及禁用原因默认为空。填写 commandId 时，可用性和提示基线来自原命令 getState；注册可见性仍限制显示。

checked 不表示当前页签，也不会向 surface 窗口发送按钮勾选消息。禁用会影响面板内交互并移走其子控件的焦点。可见性变化必须组合 STATE 与 LAYOUT，宿主据此调整页签和工作区；纯 STATE 不重排。切换页签、隐藏恢复和后续布局保留已成功显示的文案及同一 surface view。PANEL 不接受 INPUT 专用标量字段。

只有已创建并发布的 surface view 才有刷新目标。尚未首次打开的页签返回 NOT_READY，getTarget 不创建窗口。已隐藏但保留 view 的面板可继续使用同一有效目标引用；零尺寸时使用含 LAYOUT 的组合请求。重建 view 或切换工程后，重新查询目标并更新提供者。

本服务不扫描或修改插件自行创建的子 HWND。自绘内容和属性值仍通过原 DATA 绑定、插件绘制及控件接口更新。

## INPUT 状态

INPUT 使用同一提供者、builder、目标和 STATE 票据。原 64 字节 `MmsUiStateContentV1` 可继续更新文本；标签回退为注册元素的 label，空标签再回退到属性 label。说明及禁用原因默认为空。设置只读仍可显示禁用原因，宿主保留真实子 EDIT 和禁用区域的提示命中。

`MmsUiInputStateContentV1` 在旧结构后追加四个 u32，共 80 字节。`content.h` 使用整个新结构的大小，major=1、minor=0。服务表仍为 56 字节，服务仍为 1.0。开发者通过 `getCapabilities().supportedFields` 协商新增字段。宿主不读取旧 64 字节结构的尾部；声明完整新结构时会校验全部尾部，包括未使用字段及 reserved。

| 新字段位 | 尾部字段 | 有效值与宿主基线 |
| --- | --- | --- |
| `MMS_UI_STATE_ENABLED` | enabled | 0 或 1；与宿主基线取 AND。 |
| `MMS_UI_STATE_READ_ONLY` | readOnly | 0 或 1；与宿主基线取 OR。 |
| `MMS_UI_STATE_VISIBLE` | visible | 0 或 1；与注册可见性取 AND，继续遵守祖先布局。 |

设置任一新位时，必须提供完整 80 字节结构。未设置的标量及两个 reserved 均为 0。新位只适用于 INPUT，不能覆盖按钮命令状态。未设置新位表示回到本次宿主基线；不会保留上次提供者的限制。

只读属性、缺少 stage 回调或普通宿主未提供编辑能力时，readOnly=false 不能使输入框可写。STATE 只读取属性描述及宿主编辑可用性，不调用属性 read；需要值变化时另行请求 DATA。标签更新保留活动草稿、选区、IME 状态和原始编辑基线。禁用、隐藏或设为只读会先撤销该输入框的待提交意图并取消草稿，再处理焦点或原生可用性，避免失焦提交。

便利 SDK 继续包装原服务：

```cpp
// 在已有 prepare 回调中使用原 builder。
return contents.WriteInput(builder,
	"强度", "调整所选对象的强度", "当前操作暂不允许编辑",
	true, false, true);
```

后三项是 `std::optional<bool>`，省略表示不覆盖。显式 false 也会设置对应字段位。`WriteInput` 每次查询实际能力，再调用原 `writePrepared`；旧宿主缺少所请求字段时返回 UNSUPPORTED。仅改部分字段时，填写新结构并调用 typed `WriteInput(builder, input)`。此 typed 包装要求完整 80 字节结构；旧 64 字节调用继续使用 `Write`。

INPUT 显隐沿用按钮的保留槽位及 STATE 与 LAYOUT 组合规则。仅 STATE 不会自动收缩布局；需要重新量测标签或隐藏后重新排版时请求 STATE、LAYOUT、REPAINT。后台请求通过同一队列合并，消费前后核对准确的属性注册记录；属性或提供者替换使旧贡献失效。注册快照、工程数据、播放头及共同历史不因状态显示变化而修改。

输出票据必须为空。提交成功只代表入队，插件仍须查询 `RefreshTicket` 的 status、result 和 completedKinds。需要撤回时先 Cancel，再 Release；单独 Release 不取消贡献。

## 布局与失败

STATE 先准备内容，再应用控件状态和原生显示。文案成功写入并读回核对后，才进入保留布局的量测数据。纯 LAYOUT 使用已发布的文案与缓存可见性，不回调内容提供者或所有命令。更改插件变量不会自动更新文案，须显式请求 STATE。

注册表快照仍描述注册的默认元数据；内容刷新不修改其修订号，也不把显示文案写回默认标签。当前没有覆盖全部控件的通用状态查询服务。

STATE 与 LAYOUT 组合允许按钮、INPUT 或已挂载 PANEL 隐藏并收缩空位。已收缩的目标可以保持同一引用、尺寸为零及 ready=0；未暂停且支持 LAYOUT 时，组合请求可重新读取状态并恢复。用户布局设置隐藏可能卸载控件，使旧提供者和票据失效；这与 STATE 暂时隐藏不同。未挂载目标、暂停布局或停止中的目标不能通过该例外恢复。

组合仍按 DATA、STATE、LAYOUT、REPAINT 执行。布局改变几何后，旧局部矩形以 STALE 结束，completedKinds 保留已成功阶段；不自动扩大损伤范围。需要随布局重绘时优先使用上例的无坐标整目标组合。

内容准备失败保留上一份有效文案与说明。原生状态应用不是工程事务，不承诺对已发生的全部 HWND 变化执行原子回滚。无法确认原生显示与量测一致时，宿主暂停目标和新布局快照；正常宿主布局成功后恢复，已有不可变布局租约仍可读。

## 提供者替换与停止

注册、更新或注销提供者会改变该目标的内部提供者代次，使已经接受但尚未执行的旧 STATE 贡献以 STALE 结束。含 STATE 的组合也遵守此规则；独立 REPAINT、DATA 票据不受提供者替换影响。更新失败保留旧提供者和原输出，不使旧票据过期。

注销不自动刷新窗口；已有有效显示可保留到下一次 STATE，之后回到字段默认来源。目标重建、工程切换或元素替换仍使用原实例和工程失效规则。重新取得目标后，通过 Update 重新绑定；若原提供者已经被移除，先 Release 清空 SDK 句柄，再注册。

内容回调和状态应用保持只读保护。不能在此注册、注销、执行编辑，或递归请求同插件的 DATA、STATE、LAYOUT。此限制传播到该因果链安排的 UI 任务、取消回调及后代任务；不开放普通 `mms.edit`。不要等待需要 UI 执行的作业。

插件停止生产请求并结束后台调用后，显式释放票据，再在 UI 线程调用 `provider.Release()`。Release 失败时保留对象重试；析构只有尽力清理，不能替代 stop 中的错误处理。停止期间可以注销已有提供者，不能注册、更新或写入新候选。宿主按原票据及任务规则保留 context、实例和 DLL。

## 配额

运行期以 `getCapabilities` 为准。当前每个插件最多 128 个提供者，每个字段最多 4,096 个 UTF-8 字节；宿主保留内容每插件上限 1 MiB，进程上限 8 MiB，进程最多 4,096 个提供者。

保留字节按宿主字符串容量及内容对象计费，包括合并后的禁用提示、仍用于显示的旧快照和准备候选。达到数量或保留配额返回 BUSY；分配失败返回 MEMORY。提供者注销或票据释放不保证立即释放仍用于原生显示的文本。刷新票据继续受原队列配额约束。

本服务沿用 Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求，不新增特定显卡或新系统接口依赖。代码和本机结果不能代替 Win7、AMD 真机验收。
