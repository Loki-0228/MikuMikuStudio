# native_ui

MMS 仓库内的原创 C++17 布局模块。`native_ui::layout` 不包含 MMS、Win32 或图形设备类型，可以独立配置和构建。

当前实现 `Wrap` 和 `Arrange` 节点树：行、列、换行、自动列数网格、伸缩权重、最小和最大尺寸、间距、对齐及隐藏占位。输入为 DIP，不修改原始偏好。嵌套换行先测量实际高度再放置后续兄弟，防止重叠。一次排版内缓存自然高度并限制工作量。

MMS 工具栏已接入，工作区面板由原生容器管理。`native_ui::update` 提供请求合并、截止时间、重入保护、代次过滤、失败和隐藏恢复。`native_ui::win32` 提供按父窗口批量应用、实际坐标复核和回滚、可复用 GDI 面板缓冲。MMS 的插件工具栏采用批量移动，新扩展区背景采用局部缓冲。

两个独立示例项目位于 `examples/native_form` 和 `examples/viewport_host`。前者是原生参数表单，后者增加不依赖图形设备的模拟视口。它们在独立构建目录编译和隐藏自检通过；真实输入法、多 DPI 和视觉检查另行进行。

```powershell
cmake -S third_party/native_ui/examples/native_form -B build/native-form -A x64 -DCMAKE_SYSTEM_VERSION=10.0.19041.0 -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
cmake --build build/native-form --config Release
build/native-form/Release/native_form.exe --self-test
```

MMS 原有帧循环尚未接入协调器，本体参数区和其他区域迁移尚未完成。源码为本仓库原创，许可随仓库根目录 LICENSE；未复制 UIFX 源码。
