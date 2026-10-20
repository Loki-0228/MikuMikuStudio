# C++17 界面注册 SDK

`mms/cpp/ui.hpp` 是公共 C ABI 的头文件包装。它组合现有 `mms.ui`、`mms.ui.layout` 和 `mms.ui.surface`，不需要链接宿主库。插件内部可以使用 C++ 类型；跨 DLL 的调用仍使用原 C 函数表，不传递 STL 对象或异常。

## 选择封装层

| 类型 | 用途 |
| --- | --- |
| `UiServices` | 查询并验证服务。`mms.ui` 为必需服务，布局和 surface 按需查询。 |
| `RegistrationBundle` | 管理命令、元素、属性及 surface 工厂的注册所有权；返回原始注册句柄。 |
| `CommandUi` | 组合上述注册操作，为一个命令创建可选菜单项、按钮、工具栏分组及默认布局提示。 |

这些类型不创建第二套控件树。便利层的 `command()`、`menu()`、`button()`、`group()` 都借用同一批底层注册引用。复杂插件可以取得原 C 表，再更新这些注册对象；宿主继续检查权限、代次和能力。

[C++17 重绘 SDK](cpp-refresh.md)的 `RefreshServices::Target` 直接接受原 `button().handle`。`RefreshTicket` 管理原服务票据，不重新登记控件。

服务查询只校验包装实际调用的函数。直接使用表内其他函数时，插件仍须检查对应函数指针和所需能力。

## 编译示例

完整入口与生命周期实现见 [hello_menu/plugin.cpp](examples/hello_menu/plugin.cpp)。该示例同时创建菜单和按钮，两者调用同一命令，启用和选中状态也来自同一状态回调。

示例在 `create` 中查询服务并创建 `CommandUi`，在 `start` 中填写以下声明。`p`、`getState` 和 `invoke` 的完整定义见示例源文件。

```cpp
mms::sdk::CommandUiSpec spec;
spec.id = "org.example.hello.say";
spec.owner = "org.example.hello";
spec.label = "示例：输出日志";
spec.context = p;
spec.getState = getState;
spec.invoke = invoke;
spec.menu.id = "org.example.hello.menu";
spec.button.id = "org.example.hello.toolbar";
return p->controls.Register(spec);
```

`menu.parent` 默认是 `mms.menu.plugins`，`button.parent` 默认是 `mms.toolbar.main.extensions`。填写 `group.id` 后，按钮默认放入该工具栏分组。`menu` 或 `button` 的 ID 留空表示不创建该元素。`button.hints`、`group.hints` 指向原 `MmsUiLayoutHintsV1`；描述只需在注册调用期间有效。`order` 为非零时覆盖按钮及分组提示中的顺序值，零时保留提示值。

在 Windows x64 开发机上安装 Visual Studio 2022 的 C++ 桌面工具、Windows SDK 和 CMake 3.21 以上版本。当前兼容工具链固定 MSVC 19.44.35229 和 SDK 10.0.19041.0。在 SDK 目录执行：

```powershell
cmake -S examples/hello_menu -B build/hello -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44.35207,host=x64" "-DCMAKE_SYSTEM_VERSION=10.0.19041.0" "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release"
cmake --build build/hello --config Release
```

示例使用 C++17、独立插件 `/MT`、Win7 宏和子系统 6.01。声明中使用的服务也要写入 `plugin.json`。插件调用布局能力时应查询 `mms.ui.layout`，不能把宿主恰好提供该服务作为清单依赖的替代。

## 句柄与清理

`get` 和便利层的访问器只借用引用，不转移所有权。通过这些引用调用 C API 不创建副本。`detach` 才移交指定注册的所有权；移交后，调用者负责使用对应 C 表注销它，并处理它对命令、父容器或工厂的依赖。

`RegistrationBundle` 和 `CommandUi` 不可复制。移动构造移交所有权，原对象变为空；移动赋值被禁用，避免覆盖目标中尚未注销的注册。

插件应在 `stop` 中显式调用 `Close()`，并返回其错误码。关闭按依赖顺序删除元素，再注销命令、属性和工厂。布局重排后，包内父子关系可能与最初的注册顺序不同；关闭会在有进展时重试受依赖阻挡的元素。宿主返回 `BUSY` 或其他错误时，尚未注销的句柄继续保留，插件实例及回调上下文须继续存活，之后再重试。

析构只尝试清理，无法把错误返回给宿主，不能代替 `stop` 的显式检查。不要在工作线程销毁含注册的对象，也不要在命令状态查询、绘制或宿主停止完成后继续访问函数表。

描述中的字符串和数组只需在调用期间有效，宿主按原 ABI 复制它们。回调及 `context` 不因此取得独立副本。将上下文保存在插件实例中，并在 `destroy` 时释放；尚存的值或快照租约可能使宿主在注销注册后继续保留实例和 DLL。

注册调用前，包装先为自己的记录预留容量。注册成功后记录句柄不再分配内存，避免宿主已创建对象、包装却因分配失败遗失句柄。底层各次注册仍是独立操作，整个组合不保证从未短暂显示部分元素。

便利注册失败后，包装尝试回收本次注册。清理受阻时，它保留剩余句柄；插件仍须调用 `Close()`。注册错误和清理错误分别保留，不能把失败后的对象当成完整界面继续使用。

## 布局和能力边界

工具栏按钮和分组可以提供默认尺寸、伸缩、排列方向、间距及顺序。宿主使用现有布局服务处理这些提示，用户保存的布局覆盖仍优先。菜单项按实际登记顺序排列；本接口未增加菜单专用的排序字段。

位置使用已有挂载点。更多挂载点应先通过 `mms.ui.surface.acquireAnchors` 查询；缺失位置必须返回错误或由插件明确选择备用位置。包装不会把不支持的位置静默替换成菜单。

只读属性和原生面板工厂可以通过 `RegistrationBundle` 注册。快照、值和 anchor 租约仍调用原 C API 获取及释放；本批没有增加租约类。原生窗口及 view context 的创建、重建和销毁规则继续遵循 [SDK 参考](README.md)。

可写属性的[底层暂存接口](property-stage.md)已在受控编辑宿主中接入，专用便利封装及声明式 INPUT 尚未实现。DOCK、其他挂载点、复杂动画和新视口服务仍未开放。便利 SDK 不启用普通宿主的 `mms.edit`。刷新种类按目标的 `supportedKinds` 检查，见[刷新 SDK](cpp-refresh.md)。`updateElement` 保留底层 handle，但可能更新 generation 并重建 view；不能将其当成无副作用的重绘请求。

## 工具栏命令溢出

`MMS_UI_OVERFLOW_MENU` 的值为 `3`，写入已有的 `MmsUiLayoutHintsV1.overflow`。当前支持 `mms.toolbar.main.extensions` 下的 `MMS_UI_BUTTON`、`MMS_UI_GROUP` 和 `MMS_UI_SEPARATOR`。嵌套祖先必须是 GROUP。菜单、工作区面板及其他位置使用此值返回 `MMS_E_UNSUPPORTED`。

在前述 `Register(spec)` 调用前，设置按钮的默认提示：

```cpp
MmsUiLayoutHintsV1 hints{};
hints.h = mms::sdk::Header<MmsUiLayoutHintsV1>();
hints.overflow = MMS_UI_OVERFLOW_MENU;
spec.button.hints = &hints;
```

该声明仍由 `CommandUi` 调用原 `setDefaultHints`，不创建第二个菜单元素或命令。直接使用 C ABI 的插件先为元素声明 `MMS_UI_LAYOUT`，再设置同一字段。`RegistrationBundle::AddElement` 也可注册 SEPARATOR，然后用 `SetDefaultHints` 设置提示；`CommandUiSpec` 没有单独的分隔符声明。

布局按实际生效的 `order` 保留靠前元素，超出可见宽度或高度的后序候选进入「更多」菜单。GROUP 上的溢出提示由其后代继承。GROUP 同时设置 `MMS_HINT_KEEP_TOGETHER` 时，整组作为一个布局单元移动；组内任一可见后代选择菜单溢出，就会连同没有单独选择溢出的可见命令和分隔符一起移动。需要整组折叠时，将提示放在 `spec.group.hints`。用户保存的提示覆盖仍优先。

菜单保留分组层级，清理首尾及连续分隔符。没有命令溢出时不预留「更多」区域。父区域没有可用宽度或高度时，入口随父区域隐藏，不越过父区域显示。未选择菜单溢出的元素继续使用原有换行、裁剪或滚动规则；该选项不会自动改变挂载位置。

溢出只改变呈现。原按钮 HWND、元素 handle、命令和刷新目标实例继续保留；空间恢复后同一按钮回到工具栏。溢出按钮的布局快照为 `visible=0`、`overflowed=1`，`rect` 仍表示原 HWND 的几何，不是弹出菜单中的位置。宿主保留隐藏按钮的客户区尺寸，以便原 STATE 目标继续更新；纯 STATE 不会把溢出按钮显示在工具栏上。

菜单项标签取原按钮的有效标签，启用、选中和显隐使用原命令状态。已打开的菜单可更新现有项的标签、启用及选中状态，并移除已隐藏、注销或失效的项目。调用前重新校验元素 handle、所有者生命周期和命令状态，沿用原调用入口。重新注册同名元素产生的新 handle 不接收旧菜单项的调用。插件停止及卸载遵循原注册清理和租约规则。

此扩展不增加函数表版本，也没有独立的溢出能力查询。取得 `mms.ui.layout` 1.0 不代表旧宿主接受数值 `3`；检查 `Register`、`SetDefaultHints` 或底层 `stageHints` 的结果。返回 `MMS_E_UNSUPPORTED` 时，由插件明确注册受支持的菜单位置，或报告不支持。`CommandUi` 不会静默回退；注册失败后的清理规则不变。

实现状态：**本机已验证**（完整配置 21／21、原始兼容配置 18／18、ABI 契约重复 6／6）。验收证据见 [UI 框架记录](../../docs/records/abi-ui-framework-2026-10-19.md)。工具栏溢出不开放 DOCK、INPUT、新挂载点、通用菜单内容提供者或普通 `mms.edit`。

Win7 SP1 x64、CPU 基础路径和 AMD 继续作为发布约束。当前构建与本机测试不替代 Win7 和 AMD 真机验收。
