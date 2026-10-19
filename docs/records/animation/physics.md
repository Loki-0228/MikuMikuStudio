# 物理状态与关节验证记录

本页按功能归并实现、诊断与验收资料。各记录中的「当前」「已完成」只适用于其注明的源码、构建和测试条件；当前使用入口见[文档导航](../../README.md)。

## 记录索引

- [骨骼缩放开发与验收](#骨骼缩放开发与验收)
- [PMX joint 校验与物理姿态安全（2026-09-25）](#record-physics-joint-pose-safety)
- [重复父骨骼缩放造成刚体形变](#record-repeated-rigid-scale-fix)
- [PMM 初始帧关闭物理的加载修复](#record-pmm-initial-physics)

<a id="record-physics-joint-pose-safety"></a>
## PMX joint 校验与物理姿态安全（2026-09-25）

原始记录：`PHYSICS_JOINT_POSE_SAFETY_2026-09-25.md`。其中的版本、日期和验证范围沿用原记录。

修复分支 `fix/physics-load-safety`，基线提交 `7523928`，对照工作树 `.local/worktrees/physics-load-before`（同一基线的未修改检出）。用户提出两项需求：原版 PMX 读取遇到未连接刚体 A 或刚体 B 的 joint 会闪退，改为提示用户后继续加载，并把错误 joint 无效化；手动把刚体注册到科学计数法显示范围的位置后，刚体状态算错，改为保持可计算。

<a id="record-physics-joint-pose-safety-最终行为"></a>
### 最终行为

**joint 端点缺失。** joint 记录里的刚体 A 或刚体 B 不可用（索引越界、刚体表为空、刚体未创建或刚体对象为空）时，模型继续加载，只把该 joint 无效化：`JointRecord::constraint` 置为 `-1`，求解器的关节超伸钳制与物理编辑器都跳过它。无效化前该字段留 `0`，而 0 是合法约束号、可能属于其他已加载模型，关闭模型时会误删别人的首个约束，所以统一改用 `-1` 标记禁用。joint 槽位在文本读取失败前就全部初始化为禁用态，避免半张表留下可被误认的记录。PMX 在刚体表为空时仍会写 joint 计数，因此 joint 表在 `rigidCount == 0` 时也照读。

加载完成后弹一次警告框，按界面语言分中文、日文、英文三种文案，标题分别为「模型物理警告」「モデルの物理警告」「Model physics warning」，正文逐条列出 joint 原始编号、名称与缺失的刚体 A / 刚体 B。编号从 0 开始，取文件里的原始下标，不随重命名或排序变化；英文界面用英文名，其余界面用原名。无属主窗口的导入方不弹框，可调用同一函数取得文本。原始测试文件 `tests/UserFile/测试刚体.pmm` 未改动。

**超大坐标与无效姿态。** 骨骼位置或旋转出现 NaN / Inf 时，后续帧会把污染扩散到整副骨架与刚体。修复分三层：PMM v1 / v2 读取与 `RegisterBonePoseAtFrame` 登记骨骼姿态时，把非有限的位置与旋转修复为可用值（位置取当前骨骼值或零，旋转取当前旋转或单位四元数），正常的非单位四元数原样保留、不自动归一化；物理回写后若刚体的变换或速度出现非有限值，用跟随矩阵把该刚体复位并清零速度，只修受损的刚体，正常模拟的写入集合不变；关节超伸钳制里两刚体间距的平方在单精度下会溢出为无穷，改用双精度算回距离再夹到半径界，半径界本身非法时跳过该 joint。

<a id="record-physics-joint-pose-safety-改动清单"></a>
### 改动清单

| 文件 | 改动 |
| --- | --- |
| `include/mikudancestudio/pmx_joint_validation.hpp` | 新增。端点缺失判定、三语警告文本与标题。 |
| `include/mikudancestudio/bone_pose_safety.hpp` | 新增。姿态修复入口，供 PMM 读取、登记与物理回写共用。 |
| `src/model/pmx_load.cpp` | joint 表预置禁用态；joint 创建条件改为端点完整；`rigidCount == 0` 时仍读 joint 表；加载完成后弹三语警告；刚体变换前检查物理场景与世界对象。 |
| `src/window/physics_model_dialog.cpp` | 物理编辑器跳过无效 joint，列表序号与原始下标分离，首项取第一个有效 joint。 |
| `src/physics/physics_frame.cpp` | 非有限刚体复位；同步与重置前检查跟随矩阵；关节距离溢出时按双精度钳制；回写姿态前修复；无效 joint 不进钳制。 |
| `src/model/model_keyframe_edit.cpp` | 登记骨骼姿态前修复非有限值，记录与骨骼当前姿态取同一份修复结果。 |
| `src/io/pmm_io_common.hpp`、`src/io/pmm_load_v1.cpp`、`src/io/pmm_load_v2.cpp` | 读取骨骼关键帧与模型状态时修复非有限姿态。 |
| `tests/pmx_joint_tests.cpp` | 新增回归：端点缺失、越界、零刚体、无效刚体、三语提示框、关闭模型保留其他模型约束。 |
| `tests/physics_pose_tests.cpp` | 新增回归：样例 PMM 的无效关键帧、非单位四元数保留、登记防护、超大关节距离、刚体复位。 |
| `tests/physics_scene_tests.cpp` | 新增手动对照：传入真实 PMM / PMD 推进物理并逐帧断言姿态有限。 |
| `tests/README.md` | 登记上面三个测试。 |
| `CMakeLists.txt` | 注册 `physics_pose_tests`、`pmx_joint_tests` 到 CTest，`physics_scene_tests` 作为手动目标。 |

<a id="record-physics-joint-pose-safety-修复前后对照"></a>
### 修复前后对照

两侧同条件、同一场景 `tests/UserFile/测试刚体.pmm`（引用 `tests/UserFile/Model/Miku_Hatsune_Ver2.pmd`，140 骨骼、45 刚体、27 joint），原始输出在 `.local/artifacts/physics-load-safety/`。

| 检查 | 修复前（`physics-load-before`） | 修复后 |
| --- | --- | --- |
| 姿态回归（`before-pose.log` / `after-pose.log`） | 20 项检查失败；样例 184 条关键帧中 23 条含非有限数值 | 0 项失败；184 条关键帧全部有限 |
| 完整场景（`before-scene.log` / `after-scene.log`） | 第 10 帧 32 个骨骼、26 个刚体异常 | 第 0、10、14、15、16、24、25、26 帧与反复跳转全部 0 异常 |
| joint 回归（`before-joints.log` / `after-joints.log`） | `pmx_joint_failures=27` | `pmx_joint_failures=0`，`warning_dialogs=3`（中日英各一次） |

joint 回归的对照做法是把同一份测试源码编进修复前的工作树再跑一遍，失败项逐条对应约束号、关闭模型回收与零刚体读取。三语提示框回归核对了中文、日文、英文的实际标题与正文，并在显示前自动关闭。

<a id="record-physics-joint-pose-safety-性能对照"></a>
### 性能对照

同一场景、CPU 蒙皮、1280×720、预热 30 帧、采样 120 帧，两侧交替采样（`perf-0-before.json`、`perf-1-after.json`、`perf-2-after.json`、`perf-3-before.json`，清单 `performance-manifest.json`）。GPU 排空批量耗时修复前 0.560 与 0.597 毫秒/帧，修复后 0.575 与 0.651 毫秒/帧；两侧区间重叠，波动幅度大于前后差，未观察到稳定回归。这些是排空批量吞吐率，不是窗口 FPS。两侧画面校验各 150 帧全部一致（`frames_mismatched=0`）。

<a id="record-physics-joint-pose-safety-回归命令"></a>
### 回归命令

```text
cmake --build build --config Release --target pmx_joint_tests physics_pose_tests pmx_text_tests bone_registration_tests
ctest --test-dir build -C Release -R "physics_pose_tests|pmx_joint_tests|pmx_text_regressions|bone_registration_tests"
```

修复后的结果是 4 项全部通过。`physics_scene_tests` 不注册进 CTest，手动运行时传入 PMM 或 PMD 路径。

<a id="record-physics-joint-pose-safety-整合记录"></a>
### 整合记录

基线提交 `7523928` 的 `CMakeLists.txt` 引用了当时未提交的 `src/io/pmm_extension.cpp` 与 `src/io/pmm_project_settings.cpp`，`src/window/skinning_menu.cpp` 调用了 `pmm_settings::ProcessorChanged`；这三个符号属于 PMM 便携设置功能，到 `58f4ea1` 才正式入库。独立检出的 `physics-load-safety` 工作树没有这些文件，本地用一个 `chore` 提交 `adaa949` 去掉了这两处引用才得以配置构建。

分支已合入 `main`，合并提交 `4409790`。`tests/README.md` 有一处合并冲突，保留 `main` 的 GPU 蒙皮两行并追加 `physics_scene_tests` 行。合并结果相对 `58f4ea1` 只含本次物理修复的 15 个文件（+730/−25），用 `git diff --stat 58f4ea1 main` 可复核：`src/window/skinning_menu.cpp` 回到 `main` 版本，`pmm_settings::ProcessorChanged(*app);` 保留；`mikudancestudio_core` 源清单里的 `src/io/pmm_extension.cpp` 与 `src/io/pmm_project_settings.cpp` 保留。合并时若不手工还原这两处，Git 会按分支侧自动删掉它们，属于必须逐项检查的静默回归。

合并后在 `main` 全量构建并运行 `ctest --test-dir build -C Release`：29 项注册测试通过 28 项，`model_alpha_occlusion_regressions` 失败。该失败是验收基线的既有状态，与本次改动无关：失败的 17 个像素全部来自 `dbf189f` 新增的大材质与阴影 alpha 遮罩夹具，[全局排序实验记录](../rendering/transparency.md#record-transparency-global-sort) 已写明基线提交同样失败 17 个像素，只有因性能被否决的全量排序实验 `7c92ca3` 通过这些样本。本次修复不触及渲染代码，改动集中在 PMX 读取、PMM 姿态读写、骨骼登记与物理回写，提交前的定向回归（`physics_pose_tests`、`pmx_joint_tests`、`pmx_text_regressions`、`bone_registration_tests`）全部通过。

<a id="record-repeated-rigid-scale-fix"></a>
## 重复父骨骼缩放造成刚体形变

原始记录：`REPEATED_RIGID_SCALE_FIX.md`。其中的版本、日期和验证范围沿用原记录。

<a id="record-repeated-rigid-scale-fix-修复"></a>
### 修复

物理读回产生的局部补偿矩阵曾再次参与碰撞形状计算。父骨骼缩放与刚体旋转交替发生时，补偿逐次进入尺寸计算，造成刚体尺寸漂移。

碰撞形状现在使用创建物理状态时记录的骨骼基准，以及用户修改的缩放值。物理读回补偿继续用于姿态显示，但不进入下一次碰撞形状计算。

<a id="record-repeated-rigid-scale-fix-验证与产物"></a>
### 验证与产物

- 基线提交：`87c996c`。
- 同一回归用例交替设置父骨骼 Y 缩放为 1.2 和 1，穿插刚体旋转、物理读回与同步，共 120 次。修复前尺寸检查失败 2 项，修复后失败 0 项。
- 原有刚体缩放、碰撞检查通过；物理烘焙测试 `--scene-physics` 通过。
- 原始日志：`.local/artifacts/repeated-rigid-scale/20261003/`。
- 独立 Release：`.local/releases/rigid-scale-fix/`，包括程序、MMV.dll、Data 和 licenses。
- EXE SHA256：`1701063F6838E3E2AF0AB993500086D2C62BBD14A2C88E67CA73FEAF43A8EABE`。

尚未使用用户截图中的实际模型进行界面验证；完整场景的 CPU/GPU 性能对照未验收。当前运行程序未替换或重启。

<a id="record-pmm-initial-physics"></a>
## PMM 初始帧关闭物理的加载修复

原始记录：`PMM_INITIAL_PHYSICS_20261009.md`。其中的版本、日期和验证范围沿用原记录。

打开 `D:/MMD-Next/tests/ToonGlass_Ray/UserFile/Stage.pmm` 时，MMS 已读到
160 根骨骼的关闭物理标记，但对应的 160 个刚体仍处于动态模式。
MMD 9.26 在同一工程、同一第 0 帧下的动态刚体数为 0。

<a id="record-pmm-initial-physics-原因和修改"></a>
### 原因和修改

PMM v2 加载器和物理菜单的四个入口，把原版 `0x4B2210` 的模式应用调用误接成了
`ModelKinematicSync`（`0x4B22F0`）。后者只同步已有运动学刚体的位置，不会切换
刚体模式或 Bullet 的 `CF_KINEMATIC_OBJECT` 标记。

恢复的 `bone.physicsDisabled` 已经等于初始关键帧值，后续跳帧也不会触发标记变化通知。
因此仅恢复骨骼标记不能弥补首次加载遗漏的刚体更新。

修复复用已有 `ModelBonePhysicsModeApply`，在读完工程全局物理模式后应用骨骼标记，
并接回物理菜单的四个入口。保留原有语义：全局关闭时切到运动学模式；始终计算时
启用动态模式；其余模式遵守骨骼关闭标记。帧循环中的位置同步保持原状。

<a id="record-pmm-initial-physics-验证"></a>
### 验证

使用独立、隐藏的进程副本，原始 PMM 和 EMM 未改动。前后正式程序均为
VS 2022 x64 Release，`MIKUDANCESTUDIO_PERF=OFF`、`MIKUDANCESTUDIO_DIAG=OFF`。
相同 D3D9/MME 运行文件和配置，不加载可选 MMV4MMS；视口均为 800 × 450。

| 情况 | 修复前动态刚体数 | 修复后动态刚体数 |
| --- | ---: | ---: |
| 初次加载，第 0 帧 | 160 | 0 |
| 切到始终计算 | 160 | 160 |
| 切回遵守骨骼开关 | 160 | 0 |
| 切到播放模式 | 160 | 0 |
| 全局关闭 | 160 | 0 |
| 从始终计算状态重新加载 PMM | 160 | 0 |

`physics_scene_tests <Stage.pmm> --physics-modes` 同时检查刚体模式和 Bullet 标记。
修复前上述流程有 6 处失败，修复后为 0。补充的内存关键帧测试在一根骨骼的
第 15 帧开启、第 30 帧关闭物理，跳转 0 → 15 → 30 → 0 时动态刚体数为
0 → 1 → 0 → 0；测试不保存工程。

正式 EXE 的 CPU 和自动蒙皮配置均实测初始动态刚体数为 0，与 9.26 一致。
角色全部骨骼的初始位移、旋转及世界位置与修复前及 9.26 的差值均为 0。
`physics_pose_tests`、`physics_scale_tests` 均通过，`git diff --check` 通过。

另外保留每秒一次、共 6 次的静止预览原生 FPS 读数：修复前 CPU 为
33/33/31/31/32/34，修复后 CPU 为 31/38/33/33/35/39，修复后自动为
33/40/44/45/43/43，原版 9.26 为 39/38/39/37/39/39。
这是后台短时预览检查，不是动作播放、GPU 耗时或长期稳定性能验收，不能据此承诺性能提升。

<a id="record-pmm-initial-physics-版本与保留记录"></a>
### 版本与保留记录

- 修复前源码：`162250d4f23242bdc7a107a5661c67baab1609fa`；生产源码干净，回归测试单独加入。
- 修复后源码：本提交；测试时基于上述提交，修改声明、PMM 加载入口、物理菜单入口和回归测试。
- 修复前 EXE SHA-256：`a8ac6e94aae8731d08e71985ccf3cf2f4af07620e8e83e85a75b35f82abc13eb`。
- 修复后 EXE SHA-256：`79227e91cfabbc3fa6669d576869a0f41ba71822608bc80dbbd9d75ddeeaab1d`。
- 原版 9.26 EXE SHA-256：`72c34b7ab607aef7b18974742ae18ce62f115f6a614bbedc1a4310e4a5406f72`。
- PMM SHA-256：`44041f9a1b88d2d2f10202e089a3b036a83cd0872e0cf1c83221d112de2986ba`。
- EMM SHA-256：`a8231a3651aa6f0ac592ce1ae7a157bc32a120c239a1671ce55fc7d061ee0f67`。
- 发布程序：`D:/MMD-Next/MMS/build/Release/MikuMikuDance.exe`。
- 旧 EXE、MAP、PDB：`.local/archives/pmm-physics-initial/`，保留用于复查。
- 构建日志、原始内存状态、DLL 哈希、私有运行目录、前后测试结果：
  `.local/artifacts/pmm-physics-initial/`。
- 隐藏进程读取脚本：`.local/probe_pmm_physics_initial.py`；集成测试运行脚本：
  `.local/run_pmm_physics_test.py`。这些本地诊断文件不纳入发布包。

构建命令：`cmake --build build --config Release --target MikuDanceStudio physics_scene_tests physics_pose_tests physics_scale_tests --parallel 4 -- /nodeReuse:false`。

## 骨骼缩放开发与验收

VMD 尾部负载依次包含 `MMSXMMSX`（读取兼容旧 `MDSXMDSX`）、版本号（1 或 2）、保留字段、标准前缀长度与 CRC32、记录数量、缩放记录和负载 CRC32。version 1 的每条记录为 15 字节骨骼名、32 位帧号和 9 个 float32 矩阵分量，共 55 字节；version 2 的记录在其后追加 4 字节缩放插值曲线，共 59 字节。最后 16 字节为 64 位负载长度和 `MMSXVMDX`（读取兼容旧 `MDSXVMDX`），整数采用小端序。读取时同时核对标准解析结束位置、长度、版本、校验值和矩阵有效性；扩展异常不阻止标准动作读取。

### 缩放模式与视口手柄的回归（2026-09-27，已写未构建）

`tests/bone_scale_gizmo_tests.cpp` 是纯 CPU 测试，不建窗口、不创建 D3D 设备，覆盖以下断言：

- 手柄命中测试：白框内部与原点角落归视图平面方框，三个轴色方块在中心与边缘命中，杆只在白框对角线之外命中，刚出白框角落为空，空白区域不命中，被标记不可用的轴既无方块也无杆且原点归白框，几何不可用或尺寸为零时不命中任何手柄。
- 禁用门：选择、框选、旋转等编辑模式，相机/配件面板，播放中，无有效骨骼选择，以及本帧未发布几何，都使 `bone_scale::gizmo::Active` 为假且命中测试返回空。
- 拖动路径：单轴手柄按轴端方块的投影方向计算位移，水平拖动 X 杆放大 X，向上拖动 Y 杆放大 Y，光标垂直于手柄移动时不产生改动；相机转向后手柄朝右上时，沿该方向的位移给出对应增量，而垂直方向的位移为零；白色方框的水平与竖直拖动给出同一增量并同时缩放视图平面两轴、不改变深度方向、不产生剪切；被标记不可用的轴与未发布几何的帧不产生改动；视口右下角 HUD 排仍然是纵向拖动，横向移动不产生改动。
- 模式循环：非选择→选择→框选→非选择，其余模式一律进入「选择」；PMM 编辑模式字节 0 到 4 的取值保持不变。
- 面板网格：`kBoneModeGrid` 的 12 个格子互不重叠，列坐标 366/430/494，行距 35 像素；物理演算保留原来的 48×28 单元格。
- 三语文案：合并按钮在日语下是「選　択」与「BOX選択」，英语是 `select` 与 `BOX-sel`，中文是「选择」与「框选」；缩放按钮的三语文案；再用 `DrawControlText` 实际使用的字体测量每个格子的文案宽度，本轮新写或改写的文案不超过同语言中未改动的 60 像素单元格文案的最大宽度，且在单元格两侧各留出至少 2 像素。

本轮只写了源码、测试与文档，没有构建，也没有运行任何测试、渲染回读或 GPU 验收。上面的断言需要主构建方构建后运行 `bone_scale_gizmo_tests` 才成立。手柄的实际外观（轴色、朝向、白框与视图平面的贴合）本轮未做渲染回读验收。

### 缩放功能的历史验收（2026-09-26）

工作区为 `.local/worktrees/bone-scaling`，基线提交为 `dbe5c6b`。构建、原始采样、渲染目标回读和哈希记录保存在 `.local/artifacts/bone-scaling`。已通过缩放求值、关键帧登记和编辑、撤销重做、实际 VMD 往返与普通 VMD 覆盖、实际 PMM 保存重开、损坏扩展拒绝及原有布局回归。HUD 在三种 DPI、两种背景下完成 18 组渲染目标回读检查，覆盖模型、相机和配件模式。

最终结果清单为工作树下的 `.local/artifacts/bone-scaling/validation-final.json`，共 14 组通过。其中蒙皮后端包含 120 组 CPU/GPU 对照；真实 PMX（39,758 顶点、513 骨骼）与合成 PMD（12,000 顶点、2 骨骼）各验证五种姿态，覆盖非均匀缩放、斜切、描边、UV 和原有 D3D9 缓冲身份。PMX 最大绝对差为 `1.91e-6`，PMD 为 `1.79e-7`。缺失骨骼索引也进入回归，防止 HLSL 不短路求值造成越界读取。

测试窗口均隐藏运行，不通过桌面截图验收。早期失败日志保留：VMD 测试的合成骨骼槽索引已修正，PMD 着色器缺失骨骼读取已修正；最终清单指向对应复测日志。

### 同条件性能对照（2026-09-26）

前侧生产源码为 `dbe5c6b`，后侧为 `ad032d9`。两侧使用 MSVC x64 Release、`/O2 /Ob2 /MD`，关闭诊断与阶段计时；基准由各自生产库链接。主目录 `.local/artifacts/bone-scaling/before` 和 `after` 保存 EXE、库、CMake 参数、编译命令、Git 状态及哈希。

生产 EXE SHA-256：

- 前：`F93867523CCC996DE9619446895679CCE8ED6C76609720A8F4F812A59849F53B`
- 后：`46C1D37E75D949A81F15E961BA595119C0EC355A16BA79B6C5A91879BF4212F3`

RTX 4070 Laptop GPU、1280×720、固定素材及相机，各场景和处理器运行两轮 ABBA，共 32 次。每次预热 30 帧、采样 120 帧。模型与贴图哈希、渲染设置、投影和实际模型绘制视口均检查一致，无回退样本。自动模式在这些场景均选择 CPU；GPU 正确性由上面的独立对照覆盖。

下面为**等待 GPU 完成后的批次每帧耗时中位数**，不是播放 FPS 或单独的 GPU 执行时间。波动指同侧四次样本的极差除以中位数。

| 场景 / 处理器 | 前（ms） | 后（ms） | 耗时变化 | 前 / 后波动 |
| --- | ---: | ---: | ---: | ---: |
| Grass / CPU | 12.543 | 12.977 | +3.46% | 23.43% / 47.98% |
| Grass / 自动 | 13.361 | 13.067 | -2.20% | 28.59% / 29.66% |
| 遐蝶 / CPU | 5.980 | 6.417 | +7.30% | 23.45% / 16.18% |
| 遐蝶 / 自动 | 5.703 | 6.135 | +7.58% | 13.73% / 8.00% |

角色场景首轮差异需要进一步核查，因此另作两轮 ABBA，每次预热 60 帧、采样 600 帧，共 16 次。相同条件下：

| 角色场景处理器 | 前（ms） | 后（ms） | 耗时变化 | 前 / 后波动 |
| --- | ---: | ---: | ---: | ---: |
| CPU | 5.720 | 5.950 | +4.03% | 8.45% / 14.43% |
| 自动 | 5.889 | 5.822 | -1.14% | 16.65% / 13.57% |

此次结果存在系统波动，不能据此承诺性能提升或零开销。原始样本全部保留，没有因数值偏高排除样本。测试使用单位缩放的既有场景，衡量常规动作路径的回归开销，不代表大量缩放关键帧或复杂斜切场景的性能。新增 HUD 之外的模型区域像素比较，Grass 最大通道差为 2、角色为 7，仅少量像素变化；基线自身重复采样也有像素差异。

原始清单、JSON、日志和渲染目标图像在主目录 `.local/artifacts/bone-scaling/comparison-01` 与 `comparison-character-02`；最终结果未替换或删除首轮数据。

### 与物理烧录合并后的验证

完成上述独立对照后，主分支新增了 `51e2f64` 的物理烧录实现。集成时保留该功能，并补齐新增关键帧的缩放采样。合并后的生产构建、缩放完整回归、面板关键帧、骨骼登记、烧录事务和三语隐藏对话框、真实 PMM 烧录及项目设置往返均通过。真实烧录场景为「测试刚体.pmm」，包含 140 根骨骼、45 个刚体、27 个关节；新增缩放状态参与取消隔离、提交、撤销和重做检查。

集成日志在工作树 `.local/artifacts/bone-scaling/integration-baking-01`。上方性能表对应缩放功能的独立前后版本，不冒充后续合并版本重新采样的结果。


### 刚体与 joint 缩放验收

本轮在 `3eedd88` 上增加所有绑定骨骼刚体的缩放，源码提交为 `d214cd4`。后台回归共十组通过：仿射碰撞形状、物理缩放、物理姿态修复、PMX joint、骨骼缩放完整流程、缩放文件编解码、骨骼登记、面板关键帧、烧录事务与隐藏对话框、真实 PMM 烧录。joint 缓存调整后，物理缩放与真实 PMM 烧录再次通过。

物理缩放测试覆盖三种模式、父级继承、未绑定刚体不变、相对偏移、两端 joint 连接点、平移限位、角度保留、连续修改和复位、真实 GJK 接触。非均匀缩放与斜切下，模拟结果经 `ModelPhysicsReadback` 和 `SetPhysicsMode` 后的骨骼矩阵与碰撞形状的完整变换一致。模式 2 的骨骼方向保留缩放，继续按原有规则对齐位置。

真实 PMM 为「测试刚体.pmm」，包含 140 根骨骼、45 个刚体和 27 个 joint。验收先让现场模型、刚体和骨骼显示进入已缩放状态，再验证烧录和中途取消。模型记录、骨骼、关键帧保持逐字节一致，现场缩放表、碰撞形状指针和刚体姿态保持不变。重复烧录的关键帧和缩放值相同。提交、撤销、重做和 PMM/VMD 往返通过。

原始记录保存在工作树 `.local/artifacts/bone-scale-physics/validation-final/results.json`。这是相关回归集合，未宣称全仓测试或第三方 MME 特效均已验收。


### 刚体联动的前后性能对照

对照源码为 `3eedd88` 和 `d214cd4`，构建时工作树均无源码修改。两侧分别使用 MSVC x64 Release 生产库构建基准，关闭诊断与阶段计时。完整生产 EXE、库、CMake 缓存、编译项目和哈希保存在正式目录 `.local/artifacts/bone-scale-physics/before` 与 `after`。

生产 EXE SHA-256：

- 前：`6f8b2c55001d44ab1c16a10a22a6acce5fc504f53457f89b76414b7e3aa3f4c5`。
- 后：`9514e8130f800df70b3348b2bbe8909c6a82c81a9404223029310cbba719067d`。

RTX 4070 Laptop GPU、1280×720，每次预热 60 帧、采样 300 帧。两场景各运行 CPU 与自动模式，每组两轮 ABBA，共 32 次有效运行。两侧素材哈希、相机、模型绘制视口、描边、透明排序和自阴影设置一致，实际展开的 Data 资源哈希也一致。自动模式均选择 CPU，无 CPU 回退样本。

以下为等待 GPU 完成后的批次每帧耗时中位数，不是播放 FPS，也不是独立 GPU 耗时。波动是同侧四次样本的极差除以中位数。

| 场景 / 处理器 | 前（ms） | 后（ms） | 耗时变化 | 前 / 后波动 |
| --- | ---: | ---: | ---: | ---: |
| 物理 PMM / CPU | 0.9438 | 0.9447 | +0.10% | 21.59% / 9.04% |
| 物理 PMM / 自动 | 0.8744 | 0.8668 | -0.88% | 10.39% / 23.83% |
| 遐蝶 / CPU | 7.6098 | 7.4038 | -2.71% | 5.77% / 6.92% |
| 遐蝶 / 自动 | 7.8154 | 7.6617 | -1.97% | 33.43% / 3.19% |

这些差异小于本轮样本波动，不能据此承诺提速或零开销。场景使用单位缩放，结果衡量普通动作的回归开销，不代表大量动态缩放和斜切碰撞的成本。物理 PMM 有 12,354 个顶点、45 个刚体、27 个 joint；角色有 39,758 个顶点、376 个刚体、532 个 joint。

CPU 渲染目标回读中，物理 PMM 的前后图像逐像素相同。角色的最大通道差为 31，但各通道平均差均小于 0.00036；基线自身重复图像的最大差同为 31，平均差更大。原始图像和差异数据均保留。

首个样本在含生产 MME 代理 DLL 的目录中初始化崩溃，未进入加载场景，保留于 `comparison-01` 并排除。正式基准按原有非 MME 测试模式，在双方独立的 `benchmark-runtime` 中运行；未以该测试宣称第三方 MME 的性能。32 次有效采样全部保留，没有按耗时排除异常高值。原始清单、JSON、日志与回读图像位于 `comparison-02`，图像差异为 `image-differences.json`。
