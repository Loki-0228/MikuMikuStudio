# Linux 原生 CPU 构建

在 Linux 上可以原生编译曲线和变速算法的静态库，以及一组独立的 CPU 回归程序。不需要 Wine、Windows SDK、Conan、CMake、显示服务器或显卡。需要 GNU Make、支持 C++17 的编译器（如 GCC 或 Clang）和 `ar`。

在源码根目录运行：

```sh
make -C linux -j2
make -C linux check
```

构建产物在 `linux/build/`：`libmms_animation_curve.a` 和 `tests/` 中的原生可执行文件。可用 `BUILD_DIR=/tmp/mms-linux-build` 指定独立目录；构建和测试命令都传入同一个值。切换编译器或编译选项时建议使用新的构建目录。

- `check` 运行八组冒烟测试：曲线、背景手势、保存完成状态、离屏资源所有权和状态通知等。离屏测试使用模拟后端；保存完成状态测试不写真实的 PMM 或 EMM。
- `make -C linux check-retime` 单独运行变速完整回归。在 GCC 14/Linux 上，当前 ABI 源码和交接基线都出现两条失败：合并首组时的零容差浮点比较，以及 29.97 FPS 比例。此命令保留真实失败退出码。

这里没有构建 MMS 图形程序，也不会运行真实的工程打开、保存、渲染、导出或 MME 流程。现有根目录 CMake 仍用于 Windows，Windows 的构建与打包步骤不变。在 Linux 上用 Wine 启动 Windows 版属于兼容性测试。

下一步可先把 PMM 读写、路径编码及保存事务拆成可在两端运行的逻辑，分别验证 POSIX 和 Windows 的文件锁、持久化及恢复行为。原生 GUI 还需要移植窗口、输入、渲染及媒体接口。现有 MME Windows DLL 的兼容性要单独处理。
