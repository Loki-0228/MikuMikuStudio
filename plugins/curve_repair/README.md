# MMS 镜头数值修复插件

本插件在现有 MMS 内添加「数值修复」菜单，无需替换 `MikuMikuDance.exe`。它修复旧版曲线编辑器把关键帧端点近似插值结果写回 PMM 的问题，例如将原始 `0` 写成约 `-5.6×10⁻⁸`。

## 安装与使用

适用于 Windows x64 MMS。宿主须提供 `MMSGetMenuHostApi` 菜单接口；原版 MikuMikuDance 不在支持范围内。

本插件使用唯一文件名 `MMSCurveRepair.dll`，不会占用系统或其他程序的 DLL 名称。启动器会在开始 MMS 前加载该 DLL。它使用当前选定的 MMS EXE，不会改写或替换 EXE。

1. 关闭所有 MMS 进程。
2. 将本包解压到 `MikuMikuDance.exe` 所在目录。
3. 启动 `MMSCurveRepairLauncher.exe`。确认路径指向当前 MMS 的 `MikuMikuDance.exe`，然后点击「启动 MMS」。
4. 在已启动的 MMS 中点击「数值修复」→「修复已保存的镜头数值…」。
5. 选择已经保存的 PMM。如果工程没有保留原始曲线，可以另选与该镜头序列对应的原始 VMD。
6. 点击「扫描」，核对帧号、通道、修改前后数值和恢复依据。
7. 点击「保存修复副本」。插件在原目录生成带 `_repaired_` 时间戳的 PMM 和 `.repair.txt` 报告。
8. 用 MMS 打开修复副本，查看镜头效果。

扫描读取磁盘文件，不读取当前场景的未保存修改。保存副本不会自动替换或重新载入当前场景。

同名 `.emm` 和已有的 `.pmm.mms-plugins.json` 会按副本名称复制。没有这些文件时不会创建。EMM 不会在扫描或保存过程中自动加载。副本留在原目录，保留相对路径的基准位置。

卸载时关闭 MMS，移除 `MMSCurveRepairLauncher.exe` 和 `MMSCurveRepair.dll`。

## 恢复范围

- 支持标准 PMM `0002`。PMM `0001`、扩展格式 `0003` 暂不处理。单个输入文件上限为 512 MiB。
- 检查镜头的位置、旋转、距离和视角。旋转在窗口中以角度显示。此版本不修复骨骼、表情、切线类型或一般工程损坏。
- 优先读取 PMM 尾部保存的原始曲线；没有这些曲线时才采用所选 VMD。VMD 必须有相同的镜头关键帧序列、控制点和投影设置，且 PMM 镜头没有绑定父模型。
- 只有保存值与旧版 12 次二分计算的结果逐位一致，且该通道没有其他无法解释的差异时，才列为可恢复项。原始的小数值保持原值，不做统一清零、取整或平滑。
- 原始曲线和 VMD 都不存在，或原值已经一起被覆盖时，插件无法推断原来的值。此类数据会跳过。未检出可恢复项也不能证明工程没有其他问题。
- 只有出错后导出的 VMD 时，本版无法可靠修复。窗口中的 VMD 是原始参照，不是待修复文件。将错误 VMD 当作原始参照不会恢复已经丢失的原值。
- 修复后的工程仍受所用 MMS 版本影响。旧版编辑器再次提交曲线时，仍可能重新产生同类误差。本插件修复已保存的数据，不修改宿主的运行时插值算法。

插件生成新文件，原 PMM 和配套文件保持不变。保存前会再次核对原 PMM 与扫描时是否一致；不一致时要求重新扫描。新文件使用禁止覆盖的创建方式，写入失败时清理由本次操作创建的副本。

## 构建

在 MMS 源码目录，使用 Visual Studio 2022 的 x64 工具链运行：

```powershell
cmake -S plugins/curve_repair -B .local/builds/curve-repair -A x64 -T v143
cmake --build .local/builds/curve-repair --config Release
```

DLL 和启动器分别位于 `.local/builds/curve-repair/Release/MMSCurveRepair.dll` 与 `.local/builds/curve-repair/Release/MMSCurveRepairLauncher.exe`。独立工程不会重新编译 MMS 主程序。

`verify_runtime.py` 在隔离目录复制指定的 MMS EXE，验证启动器加载、菜单、中文文件路径和保存副本。需要正常 Windows 桌面图形环境、Python 和 Pillow。运行该脚本会启动并关闭隔离的 MMS 副本。

## 加载方式

启动器先创建一个挂起的 MMS 进程，将唯一命名的插件 DLL 加载到该进程后再恢复 MMS。插件通过已有的菜单 ABI 注册功能表，未使用私有内存偏移。MMS 运行时只会打开启动器所选的原 EXE。

本目录代码和引用的曲线编解码器遵循仓库 `LICENSE`。
