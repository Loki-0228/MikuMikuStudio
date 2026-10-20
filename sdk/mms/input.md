# 声明式属性输入

`MMS_UI_INPUT` 将原有属性注册为宿主原生输入框。它复用 `mms.ui` 的元素、属性、布局和生命周期，不增加 C ABI 表或另一套对象身份。开发环境见 [SDK 参考](README.md)，可撤销回调契约见[属性暂存](property-stage.md)。

普通 MMS 启动尚未提供 `mms.edit`，输入框读取属性并以只读方式显示。可写输入目前仅在已挂接编辑宿主的受控路径启用，不能据此假定发布程序已开放全部编辑能力。

## 注册与位置

C 接口先注册属性，再注册 `MmsUiElementV1`。元素的 `kind` 为 `MMS_UI_INPUT`，`propertyId` 指向同插件的原属性。`commandId` 和 `surfaceId` 必须为空。当前支持 `mms.toolbar.main.extensions`、`mms.properties.main.extensions` 及这两个根下的 GROUP。INPUT 不支持快捷键能力、容器能力或菜单挂载。

属性扩展行位于主视口下方、本体底部参数区上方。它按内容换行，受可用高度限制并支持滚动；空行不占高度。先通过 `mms.ui.surface.acquireAnchors` 查询位置和 `allowedKinds`，不能只凭服务版本假定旧宿主已有该位置。

C++17 包装位于 `mms/cpp/ui.hpp`：

| 接口 | 行为 |
| --- | --- |
| `RegistrationBundle::AddInput(InputSpec)` | 将已有属性放入默认工具栏、指定属性扩展行或 GROUP。 |
| `RegistrationBundle::AddPropertyInput(PropertyInputSpec)` | 组合原 C 表注册属性和输入框。 |
| `PropertyInput` | 持有上述注册，提供原 `property()`、`input()` 和 `registrations()`。 |

`InputSpec` 提供 ID、所有者、标签、父组、属性 ID、布局提示和顺序。`PropertyInputSpec` 另提供属性类型、范围、选项及 `read`、`releaseValue`、`stage` 回调。描述由宿主复制，回调上下文仍由插件持有。

已有 `RegistrationBundle controls` 和已注册属性时，输入框声明如下：

```cpp
mms::sdk::InputSpec spec;
spec.id = "org.example.tool.amount-input";
spec.owner = "org.example.tool";
spec.label = "数量";
spec.parent = "mms.properties.main.extensions";
spec.propertyId = "org.example.tool.amount";
return controls.AddInput(spec);
```

组合注册失败只回收本次新建的注册，不删除包内原有项目。`cleanup_result()` 单独报告回收失败，未注销的原句柄保留在包内供 `Close()` 重试。失败的组合不能作为完整界面使用。其余清理规则见 [C++17 界面注册 SDK](cpp-ui.md#句柄与清理)。

## 值与草稿

标量使用一个原生 EDIT，VECTOR 使用 1 至 4 个。BOOL 使用 `0` 或 `1`，INT 和 UINT 使用十进制，NUMBER 和 VECTOR 只接受有限数值。TEXT 使用 UTF-8，ID 使用 32 个十六进制字符，BYTES 使用十六进制字节串。范围及 choices 由原属性描述约束。

每个默认字段最多保存 1 MiB UTF-8 文本。BYTES 的十六进制形式因此最多表示 512 KiB；更大的合法字节值显示只读摘要，需插件提供专用编辑器。底层字节值仍保留 16 MiB 上限，默认输入框不会截断后写回。

获得焦点时读取最新规范值并开始私有草稿。半成品数字和输入法组合文本留在草稿中，不修改正式数据。宿主刷新或重排保留活动草稿。Enter 提交完整字段组，离开字段组也尝试提交；在同一 VECTOR 的字段之间移动焦点不会提交。

Escape、隐藏和禁用取消草稿。输入法组合期间不提交，确认组合使用的 Enter 不再触发额外提交。插件输入框获得焦点时，本体登记、删键和全局插件快捷键不消费它的编辑按键。

## 提交与历史

提交前校验原元素、原属性记录、规范值和工程修订。过期请求丢弃旧候选并重新读取当前显示；无效文本保持为草稿，直到更正或取消。只有数值真正变化时才创建事务，相同值的其他文本形式不增加历史。

宿主为一次字段组提交创建一个事务，再调用原 `stageProperty`、prepare 和 commit。插件仍须提供可撤销 participant 和完整恢复数据。该输入框提交失败只取消自己的准确事务，不取消同插件的其他待处理事务。

回调可能分发窗口消息。取消、隐藏、禁用或提交期间的新输入会使旧请求失效，检查持续到首次正式 commit 回调开始前。新输入的草稿保留，等待下一次明确提交；已开始的正式提交不承诺撤回。

## 布局、刷新和卸载

两个挂载根的 INPUT 使用原布局提示自适应宽度，保留原元素句柄。工具栏中的 INPUT 不进入命令溢出菜单，含 INPUT 的 `keepTogether` 组不会仅折叠其中的按钮。属性扩展行不支持 `MMS_UI_OVERFLOW_MENU`，也不自动改挂到工具栏或菜单。

原刷新服务支持 INPUT 的 REPAINT、默认属性 DATA 和保留布局的 LAYOUT，可指定 SELF 或已注册 SUBTREE。未建立显式 DATA 绑定时，SELF 请求的 `binding` 与 `sourceRevision` 都为 0，宿主在消费时重新读取属性。成功票据的 `sourceRevision` 仍为 0，不表示已消费某个发布快照。C++ 的 `RefreshPropertyCurrent` 和 `RefreshPropertyElementCurrent` 组合原请求与票据，见[刷新 SDK](cpp-refresh.md)。

显式绑定继续使用准确来源版本。SELF 默认属性请求遇到显式绑定返回 CONFLICT；SUBTREE 中显式绑定优先，未绑定 INPUT 才读取属性。显式绑定的字节是插件自己的显示模型，不会自动解释为原生字段内容。INPUT 的 [STATE](state-content.md#input-状态)使用原提供者及队列，支持标签、提示和只能收紧的可用性。两个挂载根共用这些规则，能力以目标查询结果为准；自动属性订阅、侧栏、时间线和状态栏挂载尚未实现。

DATA 成功更新最新规范值缓存，活动草稿仍保留起始值和原始文本。过期草稿不能提交，Escape 恢复最新缓存。首次读取暂时失败时，宿主保留按类型创建的只读空字段；后续 DATA 成功后才建立真实值和编辑基线。准备或原生写入失败保留旧显示，回滚也失败时暂停该呈现并返回失败。

移除或重绑属性使旧草稿失效。停止、销毁和隐藏后的清理只读取宿主持有的规范值，不再调用已停止提供者。输入框内产生的窗口关闭请求延后到原生回调退出后处理。

通过原布局事务在两个挂载根之间移动元素时，元素 handle 和属性记录保留，原生视图会重建。移动先取消活动草稿，旧呈现实例和待处理票据失效；插件须重新查询刷新目标。实现及自动验收范围见[属性扩展行记录](../../docs/records/abi-property-mount-2026-10-21.md)。

实现使用 Win7 可用的原生窗口及公共控件，没有新增显卡厂商依赖。合成 IME 消息测试不替代微软拼音真机验收，Win7 和 AMD 真机验收仍待完成。
