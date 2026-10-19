# MDS 命名一致性审计

日期：2026-10-20。范围：`D:\MMD-Next` 工作区中的当前 MMS 仓库，以及 `MMN`、`MMV4MMS`、`MMS-abi-dev`、`MMV-FX`、`MME-RE`、`PmxEditor-RE` 等 git 仓库。目标是找出仍以旧名 `MikuDanceStudio` / `MDS` 出现、与当前产品名 `MikuMikuStudio`（MMS）不一致的内容，并区分必须保留的旧文件读取兼容。

## 结论

当前不一致分成四类，处理成本从低到高：

1. **仍在写入的 MDS 线格式标记**：`MDSNAME1`、`MDSXMDSX`、`MDSXVMDX`。这类最值得先改：新写入改成 `MMS*`，读取同时接受旧标记，和刚完成的 `MDSXPMM2` → `MMSXPMM2` 同一模式。
2. **用户可见名称**：消息框标题、关于文本、主窗口标题、崩溃日志、窗口类名、缓存目录、JSON 格式名、Alembic 元数据。改这些不影响旧文件读取，但缓存/JSON 需要迁移或兼容读取。
3. **命名空间和构建标识**：`namespace mikudancestudio`、`include/mikudancestudio`、`mikudancestudio_*` 目标、`MIKUDANCESTUDIO_*` 选项、`project(MikuDanceStudio)`、`add_executable(MikuDanceStudio ...)`、`org.mikudancestudio.*` 扩展键。这一层是系统性改名，跨所有子仓库和插件 SDK，不能简单全局替换。
4. **文档与历史记录**：当前指南里混用旧名，历史记录和工作区路径大量出现 `MikuDanceStudio-MME`。当前文档应统一到 MMS；历史记录不建议重写，可加一行说明。

## 1. 仍在写入的 MDS 线格式标记

| 标记 | 位置 | 当前写入 | 当前读取 | 建议 |
| --- | --- | --- | --- | --- |
| `MDSXPMM2` | PMM v2 尾部扩展尾标 | 已改为 `MMSXPMM2` | 新旧都读 | 已完成，旧标记只读 |
| `MDSNAME1` | VMD 完整名称扩展 | `MDSNAME1` | `MDSNAME1` | 新写 `MMSNAME1`，读取兼容旧标记 |
| `MDSXMDSX` | VMD 骨骼缩放载荷头 | `MDSXMDSX` | `MDSXMDSX` | 新写 `MMSXMMSX`，读取兼容旧标记 |
| `MDSXVMDX` | VMD 尾部页脚 | `MDSXVMDX` | `MDSXVMDX` | 新写 `MMSXVMDX`，读取兼容旧标记 |
| `MDSMCRV1` | VMD 曲线扩展 | 未在 MMS 源码中发现 | — | 当前使用 `MMSMCRV1`，无需改 |

依据：

- `MDSNAME1`：`src/io/extended_names.cpp:127` 写入，`:132` 读取；`docs/reference/pmm-extensions.md:60` 仍把它写成完整包的 VMD 名称扩展。
- `MDSXMDSX` / `MDSXVMDX`：`include/mikudancestudio/bone_scale_io.hpp:76,84` 定义布局；`src/io/bone_scale_io.cpp` 写入；`docs/records/animation/physics.md:181` 和 `docs/records/development/features.md:431` 记录格式。
- 测试直接构造这些标记：`tests/bone_scale_io_tests.cpp:1004`、`tests/vmd_load_safety_tests.cpp:257,469,649`、`tests/original_profile_io_tests.cpp:183`。

这类标记和 `MDSXPMM2` 一样，都是“写旧名、读旧名”。新软件继续写 `MDS*` 会让名称问题永久留在文件里；应改成 `MMS*`，读取两套。

## 2. 用户可见名称

当前源码里仍有大量 `MikuDanceStudio` 字符串：

- 消息框标题：`src/window/accessory_paste.cpp:65`、`command_view_menu.cpp:844`、`command_control_400.cpp:397`、`physics_model_dialog.cpp:284`、`render/accessory.cpp:1017`、`ui_frame_refresh.cpp:333`。
- 关于文本：`src/window/command_dispatch.cpp:441,443` 仍输出 `MikuDanceStudio Ver...`。
- 主窗口标题：`src/window/window_init.cpp:393` 创建窗口时用 `L"MikuDanceStudio"`。
- 崩溃对话框和日志：`src/main/runtime_log.cpp:435,436,597,604`。
- 窗口类名：`MikuDanceStudioCurveEditorWindow`、`MikuDanceStudioCurveCanvasWindow`、`MikuDanceStudioCurveMmdWindow`（`src/window/animation_curve_editor.cpp:281-283`）、`MikuDanceStudioFrameRateCorrection`（`frame_rate_dialog.cpp:221`）、`MikuDanceStudio.UnicodeTitle.PreviousProc`（`unicode_window.cpp:7`）。
- 本机缓存目录：`%LOCALAPPDATA%/MikuDanceStudio/`，见 `src/model/skinning_calibration_cache.hpp:27`、`src/io/mesh_bake_cache.cpp:442`、`src/render/processor_settings.cpp:223`。
- JSON 格式名：`MikuDanceStudioPhysicsPreset`，见 `src/physics/physics_bake_settings.cpp:6,83,688`。
- Alembic：`alembic_export.cpp:131,138` 使用 `MikuDanceStudio-mesh-bake.abc` 和 `application=MikuDanceStudio`。
- NVIDIA 抗锯齿配置：`src/render/nvidia_antialias_profile.hpp:73,145` 的 `MikuDanceStudio AA fix` 和 friendly name。
- PMM 内部注释仍提到旧端口名：`src/io/pmm_io_common.hpp:375`、`src/io/pmm_load_v1.cpp:2002`。

这些名称应统一为 `MikuMikuStudio` 或 `MMS`。缓存目录和 JSON 格式名不能直接替换：旧目录/旧 JSON 仍要能读，建议“新名写入、旧名回退读取、首次保存时迁移”。

## 3. 命名空间与构建标识

当前仓库的主体仍是旧名：

- CMake `project(MikuDanceStudio LANGUAGES CXX)`、`add_executable(MikuDanceStudio WIN32 ...)`（`CMakeLists.txt:3,1064`）。
- 选项和编译定义 `MIKUDANCESTUDIO_PROFILE`、`MIKUDANCESTUDIO_ENABLE_DX12`、`MIKUDANCESTUDIO_BUILD_*`、`MIKUDANCESTUDIO_*` 环境探针。
- 静态库目标 `mikudancestudio_core`、`mikudancestudio_animation_curve`、`mikudancestudio_curve_editor`、`mikudancestudio_d3d12` 等。
- 头文件目录 `include/mikudancestudio/` 和 C++ 命名空间 `mikudancestudio`。
- PMM/VMD 扩展键 `org.mikudancestudio.*`；新格式 `pmm-0003-sections.md` 仍建议 `org.mikudancestudio.path.v1` 等。

CMake 文本匹配约 310 行；`src/` 中 `MikuDanceStudio` 字符串约 39 处；`include/mikudancestudio` 和 `namespace mikudancestudio` 遍布源码。其他仓库同样依赖这套名字：

- `MMV4MMS/include/mikudancestudio/*.hpp`、`MMV-FX/plugins/.../include "mikudancestudio/..."`、`MMS-abi-dev/include/mikudancestudio/*`。
- 插件 ABI 的公共头文件路径、C++ 命名空间和 CMake 目标名都属于对外契约；直接改名会破坏现有插件源码和二进制构建。

建议把这层单独作为“命名空间迁移”项目，不要和线格式/用户文案一起做。可行方式是新增 `mikumikustudio` 命名空间和 `include/mikumikustudio`，保留旧头文件转发和 `namespace mikudancestudio = mikumikustudio;` 别名，先迁移内部实现，再逐步迁移插件 SDK；CMake 目标可以新增别名目标，旧名保留一个发布周期。

## 4. 其他仓库

| 仓库 | 主要 MDS 内容 | 匹配行数（文本搜索） |
| --- | --- | --- |
| `MMS` | 命名空间、CMake、用户文案、线格式标记、文档 | 文档 136；CMake 310；`src` 用户文案 39；标记 18 |
| `MMN` | `MikuDanceStudio` 构建目标、`MikuDanceStudio-MME` 工作区路径、`mikudancestudio` 头和命名空间、文档 | 4896 |
| `MMS-abi-dev` | 完整源码/SDK 副本，`project(MikuDanceStudio)`、`mikudancestudio_*`、`include/mikudancestudio`、文档 | 5932 |
| `MMV4MMS` | `mikudancestudio/...` 头、`MikuDanceStudio` 构建目标、`MikuDanceStudio-MME` 路径 | 208 |
| `MMV-FX` | 指向 `MikuDanceStudio-MME` 的验证/构建路径、`mikudancestudio` 头 | 147 |
| `PmxEditor-RE` | 第三方源码清单引用 `refs/MikuDanceStudio-main` | 300 |
| `MME-RE` | 来源说明、许可证文本引用 `refs/MikuDanceStudio-main` / “MikuDanceStudio License” | 14 |

说明：

- 匹配行数是递归文本搜索的命中数，包含文档、生成目录或清单里的路径；不是去重后的文件数。MMN/MMS-abi-dev 的数字主要是 `mikudancestudio` 头路径和命名空间。
- `PmxEditor-RE` 和 `MME-RE` 的旧名大多是**来源/许可证/参考项目路径**，属于历史出处，不建议改；如果要改，只需要在来源说明里加“旧参考项目名”注释。
- `MMN`、`MMV4MMS`、`MMV-FX` 是 MMS 的配套/下游仓库，它们的 `MikuDanceStudio` 目标名和 `mikudancestudio` 头依赖会跟随 MMS 的命名空间迁移一起改；单独改一个仓库会破坏构建。

## 5. 有意保留的兼容读取

以下旧名不应删除：

- `MDSXPMM2`：旧 PMM 工程读取；新写入已经是 `MMSXPMM2`。
- `MDSNAME1`、`MDSXMDSX`、`MDSXVMDX`：如果按建议改成 `MMS*`，旧标记仍需读取。
- 旧缓存目录 `%LOCALAPPDATA%/MikuDanceStudio/`：迁移后至少保留一次读取回退。
- 旧 JSON `MikuDanceStudioPhysicsPreset`：读取兼容，写入新名后再决定是否迁移旧文件。
- 历史文档/记录里的 `MikuDanceStudio`、`MikuDanceStudio-MME`：保留为历史事实，不重写；在当前指南和 README 里使用 MMS。

## 6. 建议优先级

1. **P0 线格式标记**：`MDSNAME1` → `MMSNAME1`，`MDSXMDSX` → `MMSXMMSX`，`MDSXVMDX` → `MMSXVMDX`；新写新名，旧名只读；补读取旧标记的测试。低风险，和 `MMSXPMM2` 一致。
2. **P1 用户可见名称**：消息框、关于、主窗口、崩溃日志、窗口类名统一到 `MikuMikuStudio`/`MMS`；缓存目录和 JSON 用“新写旧读 + 迁移”；Alembic 元数据直接改。
3. **P2 命名空间/构建标识**：`mikudancestudio` → `mikumikustudio` 的完整迁移，跨 MMS/MMN/MMV4MMS/MMS-abi-dev/MMV-FX 和插件 SDK；先做别名和转发头，再迁移内部代码。不要全局替换。
4. **P3 文档**：当前 README/指南统一到 MMS；历史记录保留旧名并加说明；发布说明和 GitHub 链接检查一次。

## 7. 复现方法

在对应仓库根目录用文本搜索：

```text
MDSXPMM2|MDSNAME1|MDSXMDSX|MDSXVMDX|MDSMCRV1
MikuDanceStudio|mikudancestudio
MIKUDANCESTUDIO_
org\.mikudancestudio\.
```

`MMS` 仓库的线格式命中集中在 `src/io/extended_names.cpp`、`src/io/bone_scale_io.cpp`、`include/mikudancestudio/bone_scale_io.hpp` 和相应测试；用户文案集中在 `src/window/`、`src/render/`、`src/main/runtime_log.cpp`、`src/physics/physics_bake_settings.cpp`。其他仓库的命中以 `mikudancestudio` 头路径和 `MikuDanceStudio` 构建目标为主。
