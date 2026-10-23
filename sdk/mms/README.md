# MMS 插件 SDK

本 SDK 为 Windows x64 C ABI 1.0。宿主将 `MmsHostV1` 传给插件，插件只通过函数表访问服务。主 EXE 改名不影响入口解析。插件无需链接宿主、MMSAdapter 或 MMHack 的导入库。

基础 UI 的类型化与声明式封装已提供，见 [C++17 界面注册 SDK](cpp-ui.md)。`UiServices`、`RegistrationBundle` 和 `CommandUi` 包装同一套 C ABI，复杂插件继续操作原始句柄。

[自动刷新订阅](refresh-subscriptions.md)为已有 DATA binding 安排来源发布后的刷新，复用原对象、队列和阶段。C++17 `RefreshSubscriptionServices` 包装同一 C 服务。[原创计数示例](examples/auto_refresh/plugin.cpp)展示延迟绑定、数据发布与按钮状态更新。

[范围自动订阅](scoped-refresh-subscriptions.md)通过服务 1.1 将触发 binding 与原种子分开，驱动注册子树或显式依赖图。C++17 `ScopedRefreshSubscriptionServices` 仍操作同一订阅句柄；1.0 表及 SELF 能力保持。

[原属性自动刷新](property-refresh.md)为原生 INPUT 监听原属性通知，也可按实际能力跟随共同历史。C++17 `PropertyRefreshServices` 组合原目标与属性引用；显式 DATA binding 仍优先。

[原属性范围刷新](scoped-property-refresh.md)通过同一服务 1.1，将原属性通知和明确选择的共同历史发布传播到注册子树或显式依赖图。`ScopedPropertyRefreshServices` 操作同一属性、种子及订阅句柄，成员 INPUT 读取各自当前属性，显式 DATA binding 按成员优先。

[C++17 刷新 SDK](cpp-refresh.md)提供 `RefreshServices` 和可移动的 `RefreshTicket`，封装目标查询、整目标或局部矩形重绘及取消、释放。`RelayoutCurrent` 和 `RefreshStateLayout` 组合原 C 请求，实现保留布局的重排及按钮收缩恢复；`RefreshSubtree` 及便利方法使用同一注册树提交子树请求。票据仍为原 C 句柄，可以移交给低层代码。

[控件状态内容 SDK](state-content.md)在原按钮及 INPUT 的 STATE 阶段包装标签、说明与禁用原因提供者。INPUT 的 `WriteInput` 继续包装同一 C 表、builder 和票据，并可收紧启用、只读和可见状态。简单插件注册提供者后提交 STATE 与 LAYOUT 组合，需要低层控制时继续使用同一对象。

[INPUT 默认 DATA](input.md#布局刷新和卸载)直接重读已注册属性，无需另建来源绑定。活动草稿、最新规范值与草稿起点分别保留，临时失败不丢输入。`RefreshPropertyCurrent` 和 `RefreshPropertyElementCurrent` 仍调用原刷新 C 表。

[属性扩展行](../../docs/records/abi-property-mount-2026-10-21.md) `mms.properties.main.extensions` 已支持 BUTTON、GROUP、SEPARATOR 和 INPUT。它位于主视口下方、本体底部参数区上方，与原工具栏共用对象身份、布局及刷新队列。C 接口填写 `parentId`，C++ 声明填写对应 `parent`；先通过 `acquireAnchors` 查询实际位置。空区域不占高度，命令溢出菜单仍只支持原工具栏。

[左右侧栏](side-mounts.md)提供 `mms.sidebar.left.extensions` 和 `mms.sidebar.right.extensions`，承载同一批原生控件。宿主测量列宽并为中央区域和视口避让；空列不占宽度，普通缩放保留 INPUT 草稿和原实例。

界面异步刷新已提供 [mms.ui.refresh 参考](refresh.md)，支持 SELF 和 SUBTREE 的 REPAINT、按钮及 INPUT STATE、保留布局 LAYOUT，以及[绑定 DATA 和分层 SDK](data-refresh.md)，复用同一刷新票据。GROUP 使用原 handle 开放 SUBTREE。后续设计以[统一插件扩展架构](../../docs/PLUGIN_EXTENSION_ARCHITECTURE.md)为准，另见 [UI 呈现与刷新扩展](../../docs/PLUGIN_UI_PRESENTATION.md)、[多视口与洋葱皮](../../docs/PLUGIN_VIEWPORT_RENDERING.md)及[数据格式与编辑工具](../../docs/PLUGIN_DATA_EXTENSIONS.md)。各页标明已实现范围，其余服务及工具封装仍为提案。

## 开发环境

使用 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK 和 CMake 3.21 以上版本。当前验证工具为 MSVC 19.44.35229、CMake 4.4.3。兼容构建固定 Windows SDK 10.0.19041.0，不使用 AVX2、CUDA 或 NVAPI 作为基础要求。C11 和 C++17 均有结构布局编译检查。

宿主侧静态库跟随 MMS 的运行库配置。示例插件独立使用 `/MT`。分配方释放内存，不能跨 DLL 释放 STL、字符串、异常或分配器对象。

## 编译示例

在仓库根目录执行：

```powershell
cmake -S sdk/mms/examples/hello_menu -B build/hello-menu -A x64 -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
cmake --build build/hello-menu --config Release
```

将生成的 `hello_menu.dll` 和 `plugin.json` 放入 `<MMS目录>/PLUGINS/examples/hello/`。该路径深度为 2，默认可以发现。`PLUGINS/config.json` 中的 `maxDepth` 以根目录为 0，配置 0 仅扫描根目录的 `*.plugin.json`。修改已经加载的 DLL 后重启 MMS。

## 入口和生命周期

导出未修饰名称 `MMSPluginGetApi`，使用示例的 `.def`。入口检查版本与输出容量，失败不修改输出，成功只写完整的 V1.0 前缀。不要在 DllMain 中请求宿主服务。

宿主在 UI 线程调用 `create`、`start`、`onEvent`、`stop`、`destroy`。`create` 的路径和字符串为调用期间借用数据，需要长期保留时自行复制。`start` 失败仍会调用 `stop`，清理已经注册的项目。`create` 失败必须清理未发布的实例，并将输出设为 NULL。

`enqueueUi` 可以从工作线程调用。返回成功后，run 与 cancelled 恰好调用一个，均在 UI 线程执行。取消执行中的任务返回 BUSY，完成后的句柄返回 NOT_FOUND。停止时拒绝新任务和对停止中插件的新命令调用。`stop` 不可阻塞 UI 等待工作线程；尚未排空时返回 BUSY。宿主保留实例、服务上下文和 DLL，不强制卸载。

工程数据准备或只读回调期间，任务留在队列，退出后通过延后调度自动恢复。回调不得等待同一 UI 队列中的任务完成。恢复时仍须检查对象身份及工程修订，入队不锁定场景。每次分发最多处理 64 个任务；取消和停止不会使同一任务重复调用。

## 当前服务

此表描述本分支完整增强配置已经接入的能力。原始 `mmd932` 兼容 profile 继续禁用产品内 ABI 运行时，独立契约测试通过不表示该 profile 加载插件。完整工作包及未验收项见[实现记录](../../docs/PLUGIN_ABI_IMPLEMENTATION.md)。

| 服务 | 当前范围 |
| --- | --- |
| `mms.ui` 1.0 | 命令、菜单项、工具栏及属性扩展行的 BUTTON、GROUP、SEPARATOR、INPUT；元数据更新；只读属性；快照租约；命令参数、范围与选项校验；布局修订检查、指定顺序移动、创建与删除空组、显示、首选尺寸、快捷键及恢复。 |
| `mms.ui.catalog` 1.0 | [UI 只读目录](ui-catalog.md)：原注册树、主窗口已物化菜单及已登记控件；原元素句柄、角色、已知字段、范围、实例和工程代次；不可变快照与单项解析。C++17 包装调用同一底层服务。完整本体目录及原生命令写入仍未开放。 |
| `mms.ui.surface` 1.0 | 菜单、工具栏、工作区页签和属性扩展行的 anchor 查询；各位置通过 allowedKinds 声明范围。原生子窗口工厂、多个视图、尺寸、隐藏、焦点与销毁仍遵守原挂载限制。 |
| `mms.ui.layout` 1.0 | 默认布局提示、事务内用户覆盖、实际几何快照；行、列、换行和网格；伸缩、上下限、隐藏占位；工具栏 BUTTON、GROUP 和 SEPARATOR 可通过 `MMS_UI_OVERFLOW_MENU=3` 选择命令溢出菜单；`mms::sdk::LayoutServices`／`LayoutSnapshotLease` C++17 封装。 |
| `mms.ui.layout` 1.1 | 在 1.0 之上追加[布局撤销历史](layout-history.md)：查询撤销/重做能力与深度，撤销或重做保存的布局偏好快照；仅进程内、上限 32；`mms::sdk::LayoutHistoryServices` C++17 封装。 |
| `mms.ui.refresh` 1.0 | 同插件注册对象的 SELF、SUBTREE、显式 DEPENDENTS 异步刷新。GROUP 无 SELF 绘制区；各成员按 DATA、STATE、一次 LAYOUT、REPAINT 执行，使用原 handle、票据和队列。SUBTREE 支持局部坐标映射；DEPENDENTS 排除种子、按拓扑顺序处理可达目标，使用整目标损伤。两种集合 DATA 原子捕获来源版本，sourceRevision=0 表示集合。 |
| `mms.ui.refresh.dependencies` 1.0 | 同客户端原目标间的显式 DAG。循环检测、配额、图版本及生命周期失效共用原刷新队列；[C ABI 与 C++17 封装](refresh-dependencies.md)使用同一边句柄。显式 DATA 来源可通过订阅服务 1.1 自动传播。 |
| `mms.ui.refresh.subscribe` 1.0、1.1 | 1.0 为原 binding 提供 SELF 自动刷新。1.1 追加范围登记及扩展状态，使用原 GROUP 或其他目标作种子，按 SUBTREE 或 DEPENDENTS 执行单张复合票据；已接受来源集合不受后续发布改写。见[范围参考](scoped-refresh-subscriptions.md)。 |
| `mms.ui.refresh.property` 1.0、1.1 | 1.0 提供原 INPUT 的 SELF 通知和共同历史跟随。1.1 追加独立种子的 SUBTREE、DEPENDENTS 及工作代次查询；成员显式 binding 优先。见[原属性范围刷新](scoped-property-refresh.md)。 |
| `mms.ui.refresh.bind` 1.0 | 本插件不可变来源、来源版本、目标绑定、只读显示准备及显示租约；工作线程发布，原刷新队列消费。 |
| `mms.ui.refresh.state` 1.0 | 本插件已有宿主按钮及 INPUT 的文案、说明及禁用原因；INPUT 可进一步限制启用、只读与可见状态。短命 builder 单次复制，在原 STATE 阶段应用，原 LAYOUT 使用已发布文案量测。附加服务表不改变旧刷新 ABI。 |
| `mms.scene` 1.0 | 阶段 0 的对象、骨骼、网格、绑定、刚体、关节、形状、Morph、材质和纹理路径；独立 ID、运行时代次、不可变快照及 PMM 身份保存。阶段 1–4 返回 UNSUPPORTED。 |
| `mms.render` 1.0 | [设备代次与资源租约](render.md)：设备代次、丢失状态查询；设备租约的获取、显式释放与设备丢失时的回调；`mms::sdk::RenderServices`／`RenderLease` C++17 封装。帧序号、后缓冲尺寸与格式已填充；1.1 提供成功呈现后的每帧回调，1.2 提供帧开始/结束阶段回调；post 处理尚未开放。 |
| `mms.storage` 1.0 | 插件工程数据、两阶段加载、只读保存数据租约；PMM 与扩展文件成对保存和中断恢复。提供者名为 `mms.serializer`。 |
| 其他服务 | 尚未接入，`queryService` 返回 NOT_FOUND。 |

可写属性的[暂存及事务桥](property-stage.md)已在受控编辑宿主中实现，仍使用原 `mms.ui` 表及属性身份。[声明式 INPUT](input.md)及同对象 C++ 包装已接入工具栏、属性扩展行、左右侧栏及各根下的 GROUP，包含草稿、取消、重绑和共同历史。普通宿主查询 `mms.edit` 返回 NOT_FOUND，属性暂存返回 NOT_READY，INPUT 显示为只读。DOCK 返回 UNSUPPORTED。时间线内部和状态栏挂载点尚未开放，全部本体控件及命令也尚未登记。布局撤销历史尚未接入。未支持的操作不会返回成功。

布局事务的用户覆盖由宿主保存，重启或 EXE 改名后按稳定 ID 恢复。缺失插件的配置继续保留。保存位置、默认值合并和失败行为见[界面布局保存](ui-preferences.md)。

需要布局提示时，元素声明 `MMS_UI_LAYOUT`，使用 `layout.setDefaultHints`。用户通过 `stageHints` 设置的覆盖值不会被后续默认值覆盖。布局快照仅枚举有 LAYOUT 能力的元素；新 UI 修订的 HWND 几何尚未完成或布局失败暂停时返回 NOT_READY。旧快照租约不会被后续布局修改，失败不承诺回滚已移动的 HWND。

工具栏溢出使用 `MmsUiLayoutHintsV1.overflow=MMS_UI_OVERFLOW_MENU`，不增加服务表或注册对象。当前只接受 `mms.toolbar.main.extensions` 下的 BUTTON、GROUP、SEPARATOR，祖先只能是该工具栏及 GROUP。空间不足时，后序命令进入「更多」菜单；空间恢复时，原按钮 HWND 和元素 handle 继续使用。菜单项引用原命令，沿用状态、权限和卸载规则。分组、`MMS_HINT_KEEP_TOGETHER`、旧宿主回退及完整边界见 [C++17 布局参考](cpp-ui.md#工具栏命令溢出)。状态：**本机已验证**（完整配置 21／21、原始兼容配置 18／18、ABI 契约重复 6／6）；验收证据见 [UI 框架记录](../../docs/records/abi-ui-framework-2026-10-19.md)。

刷新目标直接使用原元素 handle，按钮不要求 `MMS_UI_LAYOUT` 注册能力；具体刷新种类仍须检查目标的 supportedKinds。纯 LAYOUT 使用缓存状态，不重新查询所有命令。`getCapabilities`、`getTarget` 允许在 UI 线程的命令回调内纯读；只有 `request`、`query`、`cancel`、`release` 允许后台调用。局部矩形使用目标客户区 DIP，并带实际几何修订；同次布局改变几何后，旧矩形返回 STALE，保留已完成阶段。`release` 不取消已接受的请求，未释放票据会阻止插件卸载。完整错误和停止规则见[界面刷新接口参考](refresh.md)。

面板在 `mms.workspace.tabs.extensions` 注册 PANEL，并引用自己注册的 `surfaceId`。尺寸以 DIP 声明；回调使用父窗口坐标空间中的像素和 DPI。当前 MMS 保留 Windows 位图缩放，因此窗口 DPI 为 96；插件不要再乘显示器缩放比例。现代系统动态查询窗口 DPI，Win7 使用窗口 DC。跨原生父窗口移动先销毁后创建，业务数据应保留在插件实例中。

插件 ID 和注册 ID 使用小写 ASCII 字母、数字、点、下划线和连字符，长度为 1–128。注册 ID 必须以 `pluginId + "."` 开头，`ownerPlugin` 必须等于调用者 ID。更新描述不能更换 ID、所有者、原参数类型或回调 context。注销后再注册会获得新句柄。

注册描述中的文本、选项和参数数组会复制。快照只暴露信息记录，不包含回调或 context。读取属性后，宿主复制提供者数据并立即释放提供者租约；消费者负责释放自己的租约。快照和属性租约不得遗留到 `stop` 成功之后。

## 场景读取

`scene.h` 定义通道记录和 `MmsSceneApiV1`。在 UI 线程调用 `acquire`，当前阶段填 `MMS_STAGE_BIND`，`layerId` 填零。`channels` 可为 0，或组合九个 `MMS_CHANNEL_*` 位。`flags=MMS_SCENE_CURRENT_TIME` 表示读取当前已提交数据，此时忽略 time；插件无需预先知道当前帧。flags=0 时 time 必须等于已提交编辑帧数除以 30；请求其他时间返回 NOT_READY，不移动播放头。全对象查询的 objects.count 为 0；筛选数组使用 `MmsObjectRef`、recordVersion=1 和实际 stride。

`objects` 与 `readChannel` 返回快照持有的只读数组，可在工作线程读取。`release` 允许任意线程；调用者必须保证释放某句柄时，所有读取该句柄所得指针的作业都已结束。readChannel 的 channel 是单个通道位，不是位序号。未申请的通道返回 NOT_READY，未实现的通道返回 UNSUPPORTED。

对象 ID 不使用内存地址。保存为包含 MMS 扩展的 PMM 后重开，已发布的对象及场景元素 ID 保留，运行时代次变化。已删除对象和旧代次引用返回 STALE；尚未释放的旧快照及其嵌套数组仍可读取。插件持有快照时，宿主保留其服务上下文和 DLL，直到租约释放。绑定骨骼矩阵来自源骨骼原点，不能作为物理后姿态使用。

附件对象的 `MmsObjectV1.name` 是当前显示名称，`sourcePath` 保留来源路径。改名后的新快照读取新名称，已有租约保持原名称。旧附件名称为空时，以来源文件名作为兼容回退；未终止的名称使快照获取返回 NOT_READY。

网格通道返回一个 `MmsMeshV1`。模型顶点来自 CPU 原始记录，位置、普通 UV、附加 UV 和材质参数在副本中恢复 Morph 基准值；附件读取加载器保留的原始 D3DX 网格，锁定结束前复制数据。快照不暴露 GPU 指针。三角形用稳定材质 ID，权重用同一对象的骨骼 ID。额外 UV 按 float4 连续排列，未声明的 UV、未使用的权重和 SDEF 字段为零。

绑定通道返回一个 `MmsBindingV1`。`vertexBindings` 是 `uint32_t` 数组，逐个显示顶点指向 `bindVertices` 的索引。阶段 0 的 `morphVertices` 与 `bindVertices` 相同。SDEF 的 `sdefR0`、`sdefR1` 是相对于加权中心的偏移；附件的权重类型为 `MMS_WEIGHT_NONE`，骨骼 ID 和权重均为零。ABI 可表达 QDEF，当前 MMS 文件导入器仍只接受 PMX 2.0；运行时 QDEF 记录测试不代表 PMX 2.1 文件导入已经实现。

纹理记录提供 MMS 加载器保留的路径。宽、高、格式和色彩空间为零，表示尚未查询；这不代表资源尺寸为零。材质的纹理 ID 指向同一对象的纹理通道，空 ID 表示没有关联。网格、绑定和材质内容按已观察的通道检测变化，更新场景修订号；仅改变查询通道不产生新修订。工程编辑事务尚未发布，后续写入必须统一提交修订和撤销记录。

物理记录是原始绑定配置。刚体 world 为初始世界矩阵，bodyToBone 将刚体世界矩阵还原为绑定骨骼世界矩阵。速度为零；inertia 是刚体局部坐标的 3×3 惯性矩阵，mode=0 的有效惯性为零，mass 保留源配置。mode 的 0、1、2 分别对应跟随骨骼、动态、动态并保留骨骼位置的原生模式。group 是碰撞组编号，collisionMask 保留宿主的碰撞掩码。

形状 kind 的 0、1、2 对应球、盒、Y 轴胶囊。size 分别为半径、三个半尺寸、半径与圆柱段高度。关节当前为六自由度弹簧，frameA/frameB 位于各自刚体局部坐标。限位、弹簧、连接体碰撞策略和阻尼遵循原生配置；当前原生弹簧阻尼参数为 Bullet 2.75 的 1。缺失连接刚体的关节返回 NOT_READY。复合形状、软体、其他关节及实时物理后状态尚未发布。

Morph 的 value 在绑定阶段为零。kind 决定 elements 的类型，所有目标引用均为稳定 ID；不能把 elements 强制解释成同一种数组。

| kind | 元素记录 | 含义 |
| --- | --- | --- |
| 0 | `MmsMorphGroupElementV1` | 子 Morph ID 和权重。 |
| 1 | `MmsMorphVertexElementV1` | 顶点 ID 和位移。PMD 的第一个 base Morph 带 `MMS_MORPH_BASE`，其 offset 是绝对基准坐标。 |
| 2 | `MmsMorphBoneElementV1` | 骨骼 ID、位移和 xyzw 四元数。 |
| 3–7 | `MmsMorphUvElementV1` | 普通 UV 及四组附加 UV，各含目标顶点 ID 和 float4 偏移。 |
| 8 | `MmsMorphMaterialElementV1` | 材质 ID、operation=0 乘法或 1 加法，以及颜色、边缘和纹理系数。空材质 ID 表示全部材质。 |

flags 中 `MMS_MORPH_PANEL_MASK` 对应原生面板分类，右移 `MMS_MORPH_PANEL_SHIFT` 后得到 0–4。翻转和冲量 Morph 尚未发布，遇到这些种类返回 UNSUPPORTED。附件的骨骼、Morph 和物理数组为空，这是已实现通道没有对应元素。

## 工程数据

编辑事务和插件可选择的单次/多次撤销规则见[编辑事务参考](edit.md)。该服务尚未向宿主插件开放；工程序列化服务已可使用。

完整接口见 [storage.h](include/mms/storage.h)，注册和回调约束见[工程序列化参考](storage.md)。插件通过 `registerProvider("mms.serializer", pluginId, &table, sizeof table, &handle)` 注册自己的 `MmsSerializerV1`，不直接写 PMM 或配套文件。每个插件 ID 只允许一个序列化器。

`prepareLoad` 只准备候选数据，`commitLoad` 才替换当前状态。新建工程及没有该插件记录的工程传入 schema=0 和空数据，插件必须将其解释为默认状态。正常保存返回非零 schema 和原始字节；宿主复制后恰好调用一次 `releaseBytes`。所有回调和 storage 操作均在 UI 线程执行。

`PROJECT_OPENED`、`PROJECT_SAVED` 在原生加载或保存成功后排队发送，payloadVersion=1，payload 指向 `MmsProjectEventV1`。路径为 UTF-8，projectId 为不透明 ID，revision=0 表示本事件尚不提供统一编辑修订号。插件需要保留路径时自行复制。自动备份不改变当前工程路径、保存状态或 dirty 标记，也不发送 `PROJECT_SAVED`。

## 验证

```powershell
cmake --build build --config Release --target plugin_abi_tests
ctest --test-dir build -C Release -R '^plugin_abi_contract$' --output-on-failure
```

契约测试覆盖清单、路径、依赖排序、版本协商、参数拒绝、只读快照、线程边界、UI 队列和停止过程。`tests/plugins/host_smoke.py` 在独立目录隐藏启动实际 MMS，验证原文件名及中文文件名下的菜单和工具栏调用。测试通过不代表完整 P0–P6 或 Win7、AMD 真机验收完成。

刷新专项与普通 CTest 契约分开运行。[run_refresh_checks.py](../../tests/plugins/run_refresh_checks.py) 依赖 Windows 和 pywin32，参数依次为 `plugin_abi_tests.exe` 路径、`lifecycle.dll` 路径与尚不存在的输出目录。运行器在不激活的私有桌面执行 `--refresh-only`，验证队列与真实 Manager，写入 `result.log`、`plugin.log` 和 `report.json`。隐藏目标只验收失效操作完成及没有同步绘制，不要求 Windows 为隐藏窗口保留可查询区域。
