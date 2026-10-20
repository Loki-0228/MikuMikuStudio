# 属性暂存接口

`MmsUiApiV1::stageProperty` 在已挂接编辑宿主的受控环境中，将插件自己的可撤销属性加入指定事务。普通 MMS 启动尚未开放 `mms.edit`，暂存返回 `MMS_E_NOT_READY`。本接口不创建输入控件；[声明式 INPUT](input.md)复用该暂存过程。开发环境和 DLL 入口见 [SDK 参考](README.md)，事务流程见[编辑事务](edit.md)。

## 属性描述

可写属性通过原有 `registerProperty` 注册，使用 `MMS_PROPERTY_UNDOABLE`，不设置 `MMS_PROPERTY_READ_ONLY`，并提供 `read`、`releaseValue` 和 `stage`。OBJECT 和 ANIMATABLE 尚未开放。只读属性保持原有注册行为。

标量的 `components` 为 0；VECTOR 为 1 至 4。默认值可以是 EMPTY；非空默认值、选项和边界必须匹配类型及分量数。数值必须有限，文本必须为合法 UTF-8。边界只接受 INT、UINT、NUMBER 和 VECTOR，逐分量满足 minimum 不大于 maximum。默认值和各选项均须位于范围内，非空默认值还须属于 choices。BOOL 的选值限制通过 choices 表达。

宿主复制描述、选项和待暂存值。`stage` 的 value 只在本次回调期间有效；插件需要长期保留时自行复制。读取属性使用单独的消费方租约，不能保留提供者指针。

## 暂存与合并

在本插件的顶层命令或 UI 队列执行回调中，先获取当前工程修订并调用 `edit.begin`，再调用原 UI 表：

```cpp
MmsValue value{};
value.type = MMS_VALUE_INT;
value.value.signedValue = 9;
MmsString propertyId{"org.example.tool.amount", 23};
auto result = ui.stageProperty(ui.context, transaction, propertyId, {}, &value);
```

属性必须属于调用上下文的同一个插件，事务也必须由该插件创建。当前 target 必须为空。后台线程先排队到 UI 线程，不能直接调用此接口。

宿主调用描述中的 `stage(context, transaction, target, value)`。回调在插件私有候选中保存修改，不改变正式数据；首次暂存时，使用同一 `transaction` 调用 `edit.join` 加入本插件的 participant。每个插件在同一事务中最多加入一个 participant。后续暂存更新同一候选的 after，保留第一次修改前的 before，无需再次 join。宿主不自动合并已经提交的事务。

回调正常结束后，由外层命令调用 `edit.prepare` 和 `edit.commit`。连续暂存后提交一次产生一个历史节点；需要逐步撤销时，每步分别创建并提交事务。`cancel` 丢弃候选。撤销和重做读取节点中的完整恢复数据，无需重新打开原始文件。

## 回调边界

`stage` 回调的编辑权限仅限当前插件对准确事务的 `join`。begin、prepare、commit、undo、redo、嵌套 stage、其他事务的 join、UI 注册更新及序列化器替换均被拒绝；cancel 不取消事务。读取属性或查询命令状态的嵌套回调也不能继承 join 权限。

五类 participant 回调同样处于只读 UI 边界。插件不能在其中注册或替换 UI、注册或注销序列化器，或重新发起编辑。commit、cancel 和 release 回调不得抛异常。违反 void 回调约定会冻结共同历史，宿主不会继续发布部分节点。

## 失效与取消

宿主保留准确的属性注册记录，直到事务提交或取消。相同 handle 的 `updateProperty` 仍创建新的注册身份，旧候选因此失效；无关 UI 更新不会使其失效。工程修订、序列化器恢复令牌及全部属性身份在暂存、准备和提交前复核。

已接受的暂存发生错误、异常或失效时，宿主取消整个准确事务，包括此前已加入的其他参与者。同一插件的另一个事务不会仅因此被取消。外层命令最终返回错误时，原有规则仍会取消该插件创建的全部未完成事务；两者是不同的取消边界。

| 情况 | 结果 |
| --- | --- |
| 属性不存在 | `MMS_E_NOT_FOUND`。 |
| 只读属性或没有编辑宿主 | `MMS_E_NOT_READY`。 |
| 属性归属、目标、值或 schema 不合法 | `MMS_E_ARGUMENT`。 |
| 非 UI 线程调用 | `MMS_E_THREAD`。 |
| 不在已授权命令或队列中、嵌套回调 | `MMS_E_BUSY`。 |
| 事务不存在、归属不同或身份失效 | `MMS_E_STALE`。 |
| 事务已准备，再次暂存 | `MMS_E_CONFLICT`，保留原事务。 |

格式校验、归属检查等接受之前的拒绝不取消现有事务。成功返回但未加入本插件 participant 的首次暂存返回 `MMS_E_ARGUMENT` 并消费该事务。单个事务最多接受 4096 次属性暂存；超过限制取消该事务。取消回调抛异常时仍清理其他参与者，并将结果归为 `MMS_E_FAILED`。
