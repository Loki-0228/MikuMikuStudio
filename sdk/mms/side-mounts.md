# 左右侧栏挂载

`mms.sidebar.left.extensions` 位于本体时间线侧栏右侧、主视口左侧。`mms.sidebar.right.extensions` 位于主视口右侧。两处支持原 BUTTON、GROUP、SEPARATOR 和 INPUT，使用现有 `mms.ui`、布局树、属性、刷新目标和票据。没有新 C 函数表或第二套控件身份。

## 注册位置

先通过 `mms.ui.surface.acquireAnchors` 查询宿主实际开放的位置和 `allowedKinds`。直接注册时，将原 `MmsUiElementV1.parentId` 设为目标根。C++ 包装继续填写 `CommandUiSpec.button.parent`、`CommandUiSpec.group.parent` 或 `InputSpec.parent`。

以下片段指定已有属性的 INPUT 位置。`controls` 是原 `RegistrationBundle`，属性须先注册。

```cpp
mms::sdk::InputSpec field;
field.id = "example.tools.amount.input";
field.owner = "example.tools";
field.label = "数量";
field.propertyId = "example.tools.amount";
field.parent = "mms.sidebar.right.extensions";
const auto result = controls.AddInput(field);
```

组合插件可以将 GROUP 放入侧栏，再将按钮和 INPUT 放入该组。每项仍返回原元素 handle，后续 STATE、DATA、LAYOUT 和 REPAINT 使用同一目标。注册或迁移期间需遵守原 UI 线程及回调门禁；命令回调内的树修改应先通过 `enqueueUi` 排队。

## 布局

宿主先测量两列的内容，再为中央工具栏、工作区、属性扩展行和视口分配剩余宽度。场景保持原渲染比例。本体时间线的 `SidebarWidth` 和 `SidebarRatio` 不因插件列而改变。

侧栏根默认纵向排布，组内继续使用原布局提示。列宽受当前可用宽度约束，空间不足时使用裁剪和滚动。左右列不进入工具栏专属的「更多」命令菜单。只有空组或没有有效控件时，列宽为零。

坐标使用父窗口当前 DPI 坐标系。插件无需再次乘 Windows 显示缩放。浮动视口和全屏模式不保留侧栏宽度，返回普通窗口后恢复布局。

## 身份和清理

普通窗口缩放或分隔线拖动保留原 HWND、元素 handle 和实例代次。INPUT 沿用原草稿、焦点、选区和 IME 规则。

将元素移动到另一挂载根会重建原生视图。元素及属性身份保留，旧运行时目标失效；旧 DATA binding、依赖和订阅须按原失效规则重新取得目标并绑定。不能缓存旧实例引用继续使用。

注销控件或卸载插件后，宿主清理对应视图、焦点和刷新目标，并重新计算列宽。几何应用或发布失败时，目标保持不可用，后续成功布局才恢复 `ready`。

## 范围

侧栏当前承载声明式原生控件，不接收 surface 或 DOCK。时间线内部、状态栏和通用面板折叠仍未开放。普通生产宿主的 INPUT 继续只读，侧栏挂载不开放 `mms.edit`。

接口验证和真机验收边界见[实现记录](../../docs/records/abi-side-mounts-2026-10-22.md)。
