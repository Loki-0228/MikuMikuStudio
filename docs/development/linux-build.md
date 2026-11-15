# Linux 原生 CPU 构建

在 Linux 上可以原生编译曲线和变速算法的静态库，以及一组独立的 CPU 回归程序。不需要 Wine、Windows SDK、Conan、CMake、显示服务器或显卡。需要 GNU Make、支持 C++17 的编译器（如 GCC 或 Clang）和 `ar`。格式检查工具的测试另需 Python 3。

在源码根目录运行：

```sh
make -C linux -j2
make -C linux check
```

构建产物在 `linux/build/`：`libmms_animation_curve.a` 、`mms_format_inspect` 和 `tests/` 中的原生可执行文件。可用 `BUILD_DIR=/tmp/mms-linux-build` 指定独立目录；构建和测试命令都传入同一个值。切换编译器或编译选项时建议使用新的构建目录。

- `check` 运行冒烟测试：曲线、背景手势、保存完成状态、离屏资源所有权、状态通知和旧插件菜单命令隔离和 PMD 几何缓存摘要的原始字节，以及网格烘焙的骨骼权重及编号范围筛选等。离屏测试使用模拟后端；保存完成状态测试不写真实的 PMM 或 EMM；菜单测试不安装 Win32 消息钩子或加载 DLL。PMD 字节测试不代替 Windows 上的缓存读量或 D3D9 加载测试。
- `make -C linux format-inspect` 单独构建[只读格式检查工具](../PLUGIN_DATA_EXTENSIONS.md#fmt-01-第一阶段只读格式检查工具)。`make -C linux check-format-inspect` 运行头部、可选 PMM3END 目录和载荷 CRC32，以及 PMX 模型文本预览回归，包含真实编解码器生成的夹具、读取故障注入和预算边界。
- `make -C linux check-retime` 单独运行变速完整回归，失败时仍返回非零退出码。此前 GCC 14/Linux 在 30 → 29.97 FPS 时将第 500 帧误合并到第 499 帧。现在先用 `long double` 计算位置并检查范围，再以帧率输入的 `double` 精度四舍五入。回归验证两个键均保留、略低的相邻帧率仍可向下取整、且超过最大帧不会改写输出。整数帧率的合并采样直接按约分后的分数反算位置，测试保持零容差。这里仅验证 CPU 核心，不代替 Windows 原生宿主测试或 [ANI-01](../abi/remaining.md) 的服务验收。

`check` 也用真实临时文件验证保存恢复的清理顺序：删除暂存后终止子进程，随后重试清理。此项只运行与 Win32 恢复共用的清理顺序，不验证 NTFS 共享、恢复日志解码或归档改名。

这里没有构建 MMS 图形程序，也不会运行真实的工程打开、保存、渲染、导出或 MME 流程。现有根目录 CMake 仍用于 Windows，Windows 的构建与打包步骤不变。在 Linux 上用 Wine 启动 Windows 版属于兼容性测试。

下一步可先把 PMM 读写、路径编码及保存事务拆成可在两端运行的逻辑，分别验证 POSIX 和 Windows 的文件锁、持久化及恢复行为。原生 GUI 还需要移植窗口、输入、渲染及媒体接口。现有 MME Windows DLL 的兼容性要单独处理。
