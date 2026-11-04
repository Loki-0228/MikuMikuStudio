# 工作区与产物管理

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-local-workspace-layout"></a>
<a id="record-cleanup-inventory"></a>
<a id="record-cleanup-inventory-清理状态"></a>
<a id="record-cleanup-inventory-本次清理验证"></a>
<a id="record-cleanup-inventory-应保留的测试工具与布局信息"></a>
<a id="record-cleanup-inventory-仍需后续审计的代码"></a>
<a id="record-cleanup-inventory-2026-09-全量审查的交接"></a>

## 目录职责

源码、构建、运行与证据目录的约定见[工作区布局](../../development/workspace.md)。正式本地产物位于 `build/Release/`；独立实验使用 `.local/builds/` 与 `.local/worktrees/`，不据目录名推断它已合入或发布。

## 程序身份

复现前确认运行程序的绝对路径、文件哈希、源码提交和构建选项。插件还要确认实际加载的 DLL。安装目录中的旧二进制可能与当前源码不一致，不能引用某篇文档中的旧哈希当作当前程序身份。

## 保留与清理

[测试用途表](../../../tests/README.md)记录测试与工具的目的。构建目录、PDB、基线、失败日志、截图和原始测量按其用途决定保留；被 Git 忽略不意味着可删除。用户模型、舞台和参考二进制不属于自动清理对象。

生产实现、资源生成器、布局断言和文件格式尺寸不能仅因名称含 `stub`、`unported` 或固定数字而删除。移动文件前核对 CMake、声明和所有调用。

```powershell
rg -n 'CLEANUP\(|MAINTENANCE:' src include tests scripts tools
```

标记仅供定位，清理前重新检查调用、构建输入、文档引用及活跃进程。移除工具后应确保替代入口可用，不能依赖本仓库中不存在的提交恢复文件。

## 原始数据

专题 `data/` 文件保留固定样本的测量事实。它们不代表当前构建的普遍性能；分析时核对源码、设备、素材和口径。新测量单独保存原始输入与日志，不覆盖旧样本以冒充复测。
