# C++ 自适应 UI 模板

模板提取仓库原创 `third_party/native_ui`，提供不依赖 MMS 的 C++17 / Win32 工程。源码包包含布局与刷新协调库、两个原始示例、自适应工作区示例、中文说明和文件哈希。

## 生成源码包

安装 Python 3.10 或更新版本，在仓库根目录运行：

```powershell
python tools/package-adaptive-ui-template.py
```

输出为 `build/packages/mms-adaptive-ui-template.zip` 和同名源码目录。已有输出不会默认覆盖；改用 `--output build/packages/其他名称`，或确认要更新后使用 `--refresh`。后者只更新文件集合完全匹配的既有源码包，不清理构建目录。

`MANIFEST.json` 记录来源提交、打包脚本及每个文件的 SHA256。打包前要求原始 `native_ui` 和 `LICENSE` 没有未提交修改；源码和示例沿用随包许可。

## 编译及使用

解压后按照包内 `README.md` 安装 VS2022 C++ 桌面工具、MSVC v143、Windows SDK 10.0.19041.0 和 CMake 3.21 以上版本。配置 x64 后编译，运行 `adaptive_workspace.exe`，编辑名称与数值，拖动边缘观察按钮换行、分组重排和预览区域尺寸变化。

在 `examples/workspace.cpp` 中通过 `RegisterCommand` 增加按钮命令，用相同字符串 ID 关联原生控件与布局节点。示例按 DIP 排版，批量应用子窗口几何，在成功后发布预览位置。当前预览为 CPU/GDI 占位；接入渲染器时可在相同位置放置自己的子窗口。

包内注册表用于独立示例。开发 MMS 插件时使用[公共 C ABI 及其 C++17 包装](../../sdk/mms/cpp-ui.md)，继续遵循宿主的能力、线程及生命周期规则。

## 验证范围

本包以 Release x64、SDK 10.0.19041.0 独立构建，4 项自动检查覆盖布局、请求合并、隐藏表单、模拟视口、工作区重排及命令回调。两个原始示例也分别独立构建并完成隐藏自检。

模板使用系统 DPI、Win32 和 GDI，目标为 Win7 SP1 x64，保留 CPU 基础路径及 AMD 兼容要求。混合 DPI 跨屏、连续拖动、输入法及 Win7／AMD 真机验收仍需实际环境验证。
