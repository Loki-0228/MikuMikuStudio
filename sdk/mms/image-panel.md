# C++ 图像面板

[`ImagePanel`](include/mms/cpp/image_panel.hpp) 将现有 `mms.ui.surface` 和 [`mms.render.image`](render-image.md) 组合成可调整尺寸的原生面板。插件提供图像句柄，封装负责读取映射、缩放、CPU 合成和局部绘制。没有新增一套宿主对象、图像存储或底层 C ABI。

完整示例见 [image_panel/plugin.cpp](examples/image_panel/plugin.cpp)。示例可编译为 DLL，通过现有工作区页签扩展位置注册，显示一张带透明度的四色图。此阶段展示插件提供的 CPU 图像；独立场景渲染、GPU 图像和多视口求值仍未开放。

## 环境与注册

使用 Windows x64、C++17 编译器、Windows SDK，链接 `user32.lib` 和 `gdi32.lib`。基线为 Win7 SP1 x64，不依赖 GPU、特定显卡厂商或 DirectComposition。SDK 使用显式宽字符系统函数，无须强制插件定义 `UNICODE`。

先取得 `UiServices`，建立 `RegistrationBundle`，再初始化一个地址稳定的 `ImagePanel`。面板对象不可移动或复制，必须活到 surface 注销之后。

```cpp
// ui 来自 UiServices::Query；host 是 create 收到的原宿主表。
mms::sdk::RegistrationBundle registrations(ui);
mms::sdk::ImagePanel panel;
auto result = panel.Initialize(host);
if (result != MMS_OK) return result;

auto surface = panel.Surface(mms::sdk::Text("your.plugin.image-surface"));
// 可按需求修改 surface 的 minimum/preferred DIP 尺寸。
result = registrations.AddSurface(surface);
if (result != MMS_OK) return result;

MmsUiElementV1 element{};
element.h = mms::sdk::Header<MmsUiElementV1>();
element.id = mms::sdk::Text("your.plugin.image-panel");
element.ownerPlugin = mms::sdk::Text("your.plugin");
element.parentId = mms::sdk::Text("mms.workspace.tabs.extensions");
element.label = mms::sdk::Text("图像预览");
element.surfaceId = surface.id;
element.kind = MMS_UI_PANEL;
result = registrations.AddElement(element);
```

上面的对象须是插件实例的成员，不要在 `start` 的局部栈上创建后返回。所有注册失败都交给同一实例的 `stop` 清理；完整错误处理和对象寿命以可编译示例为准。

每个 `ImagePanel` 只提供一个活动视图，`MMS_SURFACE_MULTIPLE_VIEWS` 未设置。多个面板使用多个对象，仍可读取同一个原始图像句柄。当前直接挂载位置是工作区扩展页签；该封装不新增其他挂载根。

## 换图、缩放与刷新

在初始化所在的 UI 线程调用 `SetImage(image)`。它先获取描述和独立读取映射，完成当前尺寸的候选画布，再替换显示。失败保留原图；成功后调用者可释放原图像句柄。面板保留的映射使像素在视图重建或设备重置后仍然有效。

`SetImage(0)` 清除图像并显示背景。`SetOptions({ImageScale::Contain, 0x202020})` 设置显示方式和 `0x00RRGGBB` 背景色；高八位必须为零。支持：

| 方式 | 行为 |
| --- | --- |
| `Contain`，默认 | 保持宽高比，整数尺寸向下取整并居中；多出的区域显示背景 |
| `Stretch` | 填满面板，允许改变宽高比 |

缩放采用最近邻采样。RGBA/BGRA 的不透明、直通 alpha 和预乘 alpha 按图像描述解释，在 sRGB 字节空间与背景混合，最终画布不透明。不执行线性光校正、色调映射或抗锯齿滤波。透明像素显示面板背景，不会创建透明桌面窗口。

面板收到尺寸变化后重建画布，DPI 变化使用宿主提供的实际客户区像素。普通重绘读取已准备的缓冲，不重新求值场景。换图和选项更新仅使面板窗口失效，不写入工程、不移动播放头、不增加撤销记录。需要宿主统一刷新票据时，仍可对原 PANEL 元素使用 `mms.ui.refresh` 的 REPAINT；没有另一套元素身份。

后台线程可以调用图像 C 服务生成图像，再通过原 `enqueueUi` 将句柄送到 UI 线程调用 `SetImage`。后台不能直接调用面板方法；排队期间必须保留图像句柄，任务成功、取消及停止路径均需释放它。宿主停止后创建新图像会返回 `NOT_READY`。

## 失败与清理

`SetImage`、`SetOptions` 返回即时结果。`LastResult()` 保存窗口处理诊断，成功重新准备画布后清除；普通成功重绘不保证清除先前错误。这些方法及 `Window()` 都限定初始化所在的 UI 线程，不提供跨线程读取保证。窗口缩放时若分配失败，保留旧缓冲并用背景覆盖新尺寸区域，不把不匹配的旧尺寸当作新画面。后续重新设置图像、选项或再次改变尺寸可重试。

每个面板的输出画布上限为 64 MiB、单边最多 16,384 像素；准备候选时旧画布和新画布可能同时存在。此内存与宿主图像配额分别计算，多面板仍需插件控制总量和更新频率。CPU 合成在 UI 线程同步执行，适合预览面板，不能把它当作高分辨率实时 GPU 呈现方案。

在 `stop` 中按顺序执行：

1. 停止并等待工作线程，处理已排队任务及其句柄。
2. `registrations.Close()`，先移除 PANEL 元素和视图，再注销 surface；失败返回宿主并保留实例。
3. `panel.Close()`，释放读取映射并注销窗口类；失败同样返回宿主并保留实例。

当前宿主对 `BUSY` 允许再次尝试停止；其他停止错误会保留 DLL 和上下文，但不保证自动重试。不要在失败后先删除实例，也不要把所有错误都改写成成功。

存在活动视图时 `panel.Close()` 返回 `BUSY`，不会绕过注册表销毁窗口。宿主停止时可能先回收图像句柄，但已有映射仍允许释放。析构函数不替代显式 `Close`；未完成清理时不能删除实例或卸载 DLL。示例已展示这一顺序。

## 单独使用 CPU 合成

[`CpuImageCanvas`](include/mms/cpp/image_canvas.hpp) 不注册窗口，也不调用宿主服务。它借用一份有效图像描述及读取映射，生成紧密 BGRA、不透明的 CPU 画布；映射必须覆盖整个调用期间。

`Compose(info, map, width, height, options)` 成功后通过 `Pixels()`、`Width()`、`Height()` 访问结果。两个输入都为空则生成纯背景；任何一边目标尺寸为零得到空缓冲并保留请求尺寸。失败保留上次完整画布。可将它用于插件已有窗口，不必改用 `ImagePanel`。其本身没有并发同步，一个对象同一时刻只由一个线程操作。

## 构建示例

在已有 ABI 工程配置中运行：

```powershell
cmake --build build/cmake --config Release --target mms_image_panel_example --parallel 3
```

产物目录为 `build/cmake/tests/plugins/image-panel/Release/`。将 `image_panel.dll` 和示例 `plugin.json` 一起放入开发版允许扫描的 `PLUGINS` 子目录即可加载；本工程不会自动部署到正式 release。

也可在 Visual Studio x64 Native Tools 命令提示符中，进入示例目录独立构建：

```text
cl /std:c++17 /EHsc /utf-8 /MT /LD /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /I..\..\include plugin.cpp user32.lib gdi32.lib /link /DEF:plugin.def /SUBSYSTEM:WINDOWS,6.01 /OUT:image_panel.dll
```

通过隐藏窗口和内存位图逐像素验证不等于真实屏幕无闪烁、Win7 真机或多 DPI 视觉验收。具体本机结果见[本批验收记录](../../docs/records/abi-image-panel-2026-10-26.md)。
