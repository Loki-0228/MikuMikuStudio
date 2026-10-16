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
| `mms.scene` 1.0 | 阶段 0 的对象基本信息和绑定骨骼；独立 ID、运行时代次、不可变快照及 PMM 身份保存。其他阶段和通道返回 UNSUPPORTED。 |
| 其他服务 | 尚未接入，`queryService` 返回 NOT_FOUND。 |

可写属性和工程编辑命令等待编辑事务服务，当前返回 NOT_READY。停靠和命令溢出菜单暂返回 UNSUPPORTED。INPUT、侧栏、属性区、时间线和状态栏挂载点尚未开放。布局文件持久化和布局撤销历史尚未接入。未支持的操作不会返回成功。

需要布局提示时，元素声明 `MMS_UI_LAYOUT`，使用 `layout.setDefaultHints`。用户通过 `stageHints` 设置的覆盖值不会被后续默认值覆盖。布局快照仅枚举有 LAYOUT 能力的元素；新 UI 修订的 HWND 几何尚未完成时返回 NOT_READY。旧快照不会被后续布局修改。

面板在 `mms.workspace.tabs.extensions` 注册 PANEL，并引用自己注册的 `surfaceId`。尺寸以 DIP 声明；回调使用父窗口坐标空间中的像素和 DPI。当前 MMS 保留 Windows 位图缩放，因此窗口 DPI 为 96；插件不要再乘显示器缩放比例。现代系统动态查询窗口 DPI，Win7 使用窗口 DC。跨原生父窗口移动先销毁后创建，业务数据应保留在插件实例中。

插件 ID 和注册 ID 使用小写 ASCII 字母、数字、点、下划线和连字符，长度为 1–128。注册 ID 必须以 `pluginId + "."` 开头，`ownerPlugin` 必须等于调用者 ID。更新描述不能更换 ID、所有者、原参数类型或回调 context。注销后再注册会获得新句柄。

注册描述中的文本、选项和参数数组会复制。快照只暴露信息记录，不包含回调或 context。读取属性后，宿主复制提供者数据并立即释放提供者租约；消费者负责释放自己的租约。快照和属性租约不得遗留到 `stop` 成功之后。

## 场景读取

`scene.h` 定义通道记录和 `MmsSceneApiV1`。在 UI 线程调用 `acquire`，当前阶段填 `MMS_STAGE_BIND`，`layerId` 与 `flags` 填零，`channels` 可为 0 或 `MMS_CHANNEL_BONES`。当前绑定快照的采样时间为已提交编辑帧数除以 30；请求其他时间返回 NOT_READY，不移动播放头。全对象查询的 objects.count 为 0；筛选数组使用 `MmsObjectRef`、recordVersion=1 和实际 stride。

`objects` 与 `readChannel` 返回快照持有的只读数组，可在工作线程读取。`release` 允许任意线程；调用者必须保证释放某句柄时，所有读取该句柄所得指针的作业都已结束。readChannel 的 channel 是单个通道位，不是位序号。未申请的通道返回 NOT_READY，未实现的通道返回 UNSUPPORTED。

对象 ID 不使用内存地址。保存为包含 MMS 扩展的 PMM 后重开，对象与骨骼 ID 保留，运行时代次变化。已删除对象和旧代次引用返回 STALE；尚未释放的旧快照仍可读取。插件持有快照时，宿主保留其服务上下文和 DLL，直到租约释放。绑定骨骼矩阵来自源骨骼原点，不能作为物理后姿态使用。

## 验证

```powershell
cmake --build build --config Release --target plugin_abi_tests
ctest --test-dir build -C Release -R '^plugin_abi_contract$' --output-on-failure
```

契约测试覆盖清单、路径、依赖排序、版本协商、参数拒绝、只读快照、线程边界、UI 队列和停止过程。`tests/plugins/host_smoke.py` 在独立目录隐藏启动实际 MMS，验证原文件名及中文文件名下的菜单和工具栏调用。测试通过不代表完整 P0–P6 或 Win7、AMD 真机验收完成。
