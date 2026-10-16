# MMS 插件 SDK

本 SDK 为 Windows x64 C ABI 1.0。宿主将 `MmsHostV1` 传给插件，插件只通过函数表访问服务。主 EXE 改名不影响入口解析。插件无需链接宿主、MMSAdapter 或 MMHack 的导入库。

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

## 当前服务

此表只描述本分支已经接入的能力。完整工作包及未验收项见[实现记录](../../docs/PLUGIN_ABI_IMPLEMENTATION.md)。

| 服务 | 当前范围 |
| --- | --- |
| `mms.ui` 1.0 | 命令、菜单项、工具栏按钮和组；元数据更新；只读属性；快照租约；命令参数、范围与选项校验；布局修订检查、指定顺序移动、创建与删除空组、显示、首选尺寸、快捷键及恢复。 |
| `mms.ui.surface` 1.0 | 菜单、工具栏和工作区页签挂载位置；原生子窗口工厂、多个视图、尺寸、隐藏、焦点与销毁。 |
| `mms.ui.layout` 1.0 | 默认布局提示、事务内用户覆盖、实际几何快照；行、列、换行和网格；伸缩、上下限、隐藏占位。 |
| `mms.scene` 1.0 | 阶段 0 的对象、骨骼、网格、绑定、刚体、关节、形状、Morph、材质和纹理路径；独立 ID、运行时代次、不可变快照及 PMM 身份保存。阶段 1–4 返回 UNSUPPORTED。 |
| `mms.storage` 1.0 | 插件工程数据、两阶段加载、只读保存数据租约；PMM 与扩展文件成对保存和中断恢复。提供者名为 `mms.serializer`。 |
| 其他服务 | 尚未接入，`queryService` 返回 NOT_FOUND。 |

可写属性和工程编辑命令等待编辑事务服务，当前返回 NOT_READY。停靠和命令溢出菜单暂返回 UNSUPPORTED。INPUT、侧栏、属性区、时间线和状态栏挂载点尚未开放。布局文件持久化和布局撤销历史尚未接入。未支持的操作不会返回成功。

需要布局提示时，元素声明 `MMS_UI_LAYOUT`，使用 `layout.setDefaultHints`。用户通过 `stageHints` 设置的覆盖值不会被后续默认值覆盖。布局快照仅枚举有 LAYOUT 能力的元素；新 UI 修订的 HWND 几何尚未完成时返回 NOT_READY。旧快照不会被后续布局修改。

面板在 `mms.workspace.tabs.extensions` 注册 PANEL，并引用自己注册的 `surfaceId`。尺寸以 DIP 声明；回调使用父窗口坐标空间中的像素和 DPI。当前 MMS 保留 Windows 位图缩放，因此窗口 DPI 为 96；插件不要再乘显示器缩放比例。现代系统动态查询窗口 DPI，Win7 使用窗口 DC。跨原生父窗口移动先销毁后创建，业务数据应保留在插件实例中。

插件 ID 和注册 ID 使用小写 ASCII 字母、数字、点、下划线和连字符，长度为 1–128。注册 ID 必须以 `pluginId + "."` 开头，`ownerPlugin` 必须等于调用者 ID。更新描述不能更换 ID、所有者、原参数类型或回调 context。注销后再注册会获得新句柄。

注册描述中的文本、选项和参数数组会复制。快照只暴露信息记录，不包含回调或 context。读取属性后，宿主复制提供者数据并立即释放提供者租约；消费者负责释放自己的租约。快照和属性租约不得遗留到 `stop` 成功之后。

## 场景读取

`scene.h` 定义通道记录和 `MmsSceneApiV1`。在 UI 线程调用 `acquire`，当前阶段填 `MMS_STAGE_BIND`，`layerId` 填零。`channels` 可为 0，或组合九个 `MMS_CHANNEL_*` 位。`flags=MMS_SCENE_CURRENT_TIME` 表示读取当前已提交数据，此时忽略 time；插件无需预先知道当前帧。flags=0 时 time 必须等于已提交编辑帧数除以 30；请求其他时间返回 NOT_READY，不移动播放头。全对象查询的 objects.count 为 0；筛选数组使用 `MmsObjectRef`、recordVersion=1 和实际 stride。

`objects` 与 `readChannel` 返回快照持有的只读数组，可在工作线程读取。`release` 允许任意线程；调用者必须保证释放某句柄时，所有读取该句柄所得指针的作业都已结束。readChannel 的 channel 是单个通道位，不是位序号。未申请的通道返回 NOT_READY，未实现的通道返回 UNSUPPORTED。

对象 ID 不使用内存地址。保存为包含 MMS 扩展的 PMM 后重开，已发布的对象及场景元素 ID 保留，运行时代次变化。已删除对象和旧代次引用返回 STALE；尚未释放的旧快照及其嵌套数组仍可读取。插件持有快照时，宿主保留其服务上下文和 DLL，直到租约释放。绑定骨骼矩阵来自源骨骼原点，不能作为物理后姿态使用。

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

完整接口见 [storage.h](include/mms/storage.h)，注册和回调约束见[工程序列化参考](storage.md)。插件通过 `registerProvider("mms.serializer", pluginId, &table, sizeof table, &handle)` 注册自己的 `MmsSerializerV1`，不直接写 PMM 或配套文件。每个插件 ID 只允许一个序列化器。

`prepareLoad` 只准备候选数据，`commitLoad` 才替换当前状态。新建工程及没有该插件记录的工程传入 schema=0 和空数据，插件必须将其解释为默认状态。正常保存返回非零 schema 和原始字节；宿主复制后恰好调用一次 `releaseBytes`。所有回调和 storage 操作均在 UI 线程执行。

`PROJECT_OPENED`、`PROJECT_SAVED` 在原生加载或保存成功后排队发送，payloadVersion=1，payload 指向 `MmsProjectEventV1`。路径为 UTF-8，projectId 为不透明 ID，revision=0 表示本事件尚不提供统一编辑修订号。插件需要保留路径时自行复制。自动备份不改变当前工程路径、保存状态或 dirty 标记，也不发送 `PROJECT_SAVED`。

## 验证

```powershell
cmake --build build --config Release --target plugin_abi_tests
ctest --test-dir build -C Release -R '^plugin_abi_contract$' --output-on-failure
```

契约测试覆盖清单、路径、依赖排序、版本协商、参数拒绝、只读快照、线程边界、UI 队列和停止过程。`tests/plugins/host_smoke.py` 在独立目录隐藏启动实际 MMS，验证原文件名及中文文件名下的菜单和工具栏调用。测试通过不代表完整 P0–P6 或 Win7、AMD 真机验收完成。
