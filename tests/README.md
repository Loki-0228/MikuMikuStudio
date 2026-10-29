# 测试保留与清理说明

核对日期：2026-09-20。所有测试源文件均在首行用 `MAINTENANCE:` 标记。测试源码不进入发布目录，目标使用 `EXCLUDE_FROM_ALL`；编译后的测试程序位于 `build/tests/`。不要因为测试不随正式程序发布就删除源码。

## 曲线、备份及崩溃日志回归

`animation_curve_editor_tests` 全量测试包含插值和切线按钮立即生效、多选一次撤销、重复操作保留重做、手柄模式和一次性预设。可用 `--curve-modes` 单独运行按钮回归，用 `--curve-ui-compat` 运行手柄、布局、提示及键盘导航。`animation_curve_host_tests` 验证原生相机精确键值、单帧保持和阶跃，以及 PMM 往返。

`autosave_tests <独立输出目录>` 需要位于全新的独立 EXE 目录，避免误用已有 `backups`。它覆盖实际 PMM 备份、轮换、未登记姿态恢复和故障路径。追加 `--full-zh`、`--full-en`、`--full-ja`、`--full-rotate` 或 `--full-crash` 检查容量告警；每种模式在同一测试目录启动两次，核对重启后再次提示、进程内仅一次、两种备份目录、分配失败和弹窗创建失败后重试。测试窗口须隐藏或放在专用桌面。

`runtime_log_tests <尚不存在的输出目录>` 只在独立子进程触发故障，拦截交互弹窗并核对退出码、UTF-8 日志及恢复结果。覆盖正常退出、SEH、Fatal、terminate、abort、关闭期异常、无法创建日志、多线程写入和持有日志槽位时故障。最后两类逐条核对日志内容；单槽竞争只能丢弃记录，不能等待故障线程。`MMS_RUNTIME_LOG_TESTS` 仅为此测试目标定义，不进入正式程序。

共同代码分别在 `full` 和 `mmd932` 构建档验证。静态导入检查和本机运行不能代替 Win7、AMD 真机验收。

## 附件顺序与选中项回归

`scene_lifecycle_tests <输出目录> 1 --accessory-feedback all` 在隐藏的生产窗口生成并导入 X 文件，通过实际「附件编辑」和删除确认对话框检查中英日三种界面。也可将 `all` 换为 `split`、`selection` 或 `delete` 单独运行。

覆盖新增附件时保持绘制分界数值（含 0 和原最大值）、确认与重排后保留当前附件及登记范围、取消编辑与删除、删除后选中显示列表最后一项、复用空槽、删除全部后重新导入，以及 PMM 保存重开。检查下拉框、时间轴高亮和位置输入框指向同一附件。完整包与最小包共用此回归；移除对应附件编辑功能后再评估删除。

## 操作轴定位回归

操作轴定位可单独运行 `scene_lifecycle_tests <模型.pmx、模型.pmd 或工程.pmm> 1 --bone-gizmo`。隐藏窗口先执行正常首帧骨骼求值，再观察生产绘制提交的黄色原点，核对其与选中骨骼标记一致；完整档覆盖三种轴向操作、三坐标系、两个相机角度、偏移视口及 50%/100%/150% 预览比例。`scene_lifecycle_mme_tests` 支持相同参数，并断言实际 MME 模块已加载。该模式不修改输入素材；合成场景通过不能代替报告者工程的复现验证。

## 工程打开时的追踪镜头回归

`scene_lifecycle_tests <工程.pmm> 1 --pmm-camera-load [新建的输出目录]` 在隐藏窗口加载工程，检查首次镜头、当前镜头关键帧重算、关闭后重新开启追踪，以及当前帧的播放初始化是否产生相同的世界、视图和投影矩阵。夹具须开启相机追踪。无父骨骼绑定的镜头还检查三个预览帧后的矩阵；父骨骼的比较在同一姿势下完成，避免把首帧模型或物理更新误判为镜头偏移。不能只比较两次 `ReloadModels`，否则两侧都残留编辑视角偏移时也会通过。

将参数替换为 `--pmm-camera-startup`，可检查初始化窗口时直接打开工程的入口。设置 `MMS_PMM_CAMERA_CAPTURE_DIR` 为已有目录时，测试会保存各阶段的渲染目标 PNG；只用于画面诊断，不作为整套特效的像素一致性断言。

提供输出目录时，还会通过七份独立 PMM 副本检查第 0 帧、帧间插值、有无父骨骼、自由视角，以及相机面板中未登记的编辑。输出文件名必须尚不存在，测试不覆盖输入工程。`scene_lifecycle_mme_tests` 支持同一参数，须在测试程序目录安装本地 MME；该模式验证插件接入后的镜头矩阵，不执行整套特效画质对比。

2026-10-15 的 QSPEED 工程回归记录见 [PMM 启动追踪镜头修复](../docs/records/formats/projects.md#record-pmm-camera-load)。

## 长期回归：`regression-keep`

`curve_sync_tests` 验证密集曲线的双向选区同步、批量表情读取、无通知的原生改值与恢复、乱序及重复帧、循环链表拒绝，以及连续通知的合并。测试只创建隐藏窗口，不创建 D3D 设备。输出的毫秒数用于比较同机同配置的同步开销，不代表工程播放帧率。性能记录见[曲线同步开销](../docs/records/animation/editing.md#record-curve-sync-performance)。

这些文件覆盖真实行为或已发生的缺陷，应保留；仅在对应功能删除，或验证范围完整迁移到其他测试后再移除。

| 文件 | 用途 |
| --- | --- |
| `pmd_selection_render_tests.cpp`、`pmd_selection_fixtures.py` | 真实 PMD 加载与「选择」、模型、相机模式切换的贴图回读。生成器覆盖共享 toon 文件表重排、缺失 toon 回退、自定义与无 toon、背光面，并比较等价文件表的采样颜色。需 D3D9，显式运行；可放入隔离的 MME 或 MMV 插件。命令见下方「PMD 贴图选择模式回归」。 |
| `optional_graphics_imports.py` | 检查正式 EXE 的普通与延迟导入表，防止可选 DX12、DXGI、编译器 DLL 再次成为启动依赖。只用 Python 标准库，注册 CTest。 |
| `graphics_runtime_tests.cpp` | 隐藏 DX9 设备上注入可选 DLL 或入口缺失，验证 CPU 蒙皮回退、描边、Present 和像素回读。标记为 hardware-manual，需 DX9、DX12 硬件，逐场景注册 CTest。 |
| `native_shadow_tests.cpp` | 隐藏 D3D9 设备上的原生 SM3 阴影回读，覆盖两种模式、接收面修正、斜边平滑、贴图边界与缩小采样。 |
| `keyboard_input_tests.cpp` | 快捷键与输入分发。 |
| `mesh_bake_cache_tests.cpp` | Ogawa 采样、区间钳制、替换、清除、PMM 配套文件往返、畸形记录、提交前回调取消、临时文件跨进程归属。独立 CPU/文件测试，注册 CTest。 |
| `vertex_capture_tests.cpp` | 隐藏 D3D9 HAL 设备上重放 VS 1.1/2.0/3.0，检查 GPU 位置回读、分块、状态恢复与不支持路径拒绝。注册 CTest。 |
| `alembic_export_tests.cpp` | 隐藏生产窗口内生成 PMX，核对骨骼动画、MME/MMV 叠加、生成与消失的材质/拓扑、Unicode ABC、取消、三语条件警告、烧录回工程、PMM 保存重开和清除后恢复。需要按模式准备 Data/插件，显式运行。 |
| `sequential_file_reader_tests.cpp` | 顺序文件读取、边界和回退。 |
| `d3dx_import_tests.cpp` | D3DX 导入与 ABI；需要本机 DirectX 运行库。 |
| `effect_api_tests.cpp` | 效果接口、对象导出及材质名称双向回退；无 MME 时安全跳过。 |
| `bone_projection_tests.cpp` | 骨骼投影与命中坐标。 |
| `bone_overlay_tests.cpp` | 骨骼显示/覆盖绘制；issue #37 的操作轴原点回归通过真实 GPU 回读核对黄色原点与骨骼投影重合，覆盖 PMD/PMX、透视/正交、三坐标系及轴向旋转/移动/缩放的悬停与拖动。 |
| `coordinate_axis_tests.cpp` | 视口操作轴与坐标系标识：读取内嵌 `axis.x`，校验 X 轴为红色、Z 轴为蓝色、Y 轴为绿色、原点为黄色；针对三种语言 × 模型/相机两个面板 × 三种坐标系状态，渲染共 18 组标识，并校验标识居中、白字黑描边、措辞首字母大写，以及各面板第三状态的格位；校验中英日字体映射（英文与日文共用 Times New Roman，中文使用 Microsoft YaHei UI）；校验标识描边在 HUD 缩放为 2.0 时仍为 1 设备像素（八个描边四边形加一张白字面，共 9 个）；校验标识按 HUD 缩放取用 30 或 60 像素两档图集副本，且两档绘制的宽度一致；校验在没有配件时选择器上限为 2、标识回落为 Local；并校验视图轴随相机偏航变化。 |
| `background_overlay_tests.cpp` | 实际载入图片及 AVI 后的 GPU 像素回读，覆盖完整背景、纹理坐标、移动缩放及视频换帧；素材临时生成并自动清理。 |
| `media_render_tests.cpp` | H.264 背景、透明导出和播放集成回归。`<输出目录> --video-playback <视频> [<模型> <动作>]` 单独检查空起止帧的按钮/P 键/循环播放、非法帧号拒绝，以及拖入视频前后的动画与物理推进，保存实际后备缓冲区截图。可在隔离目录放入 MME 三个 DLL；须核对日志 `PLAYBACK_HOST mme=1 mmhack=1`。移除对应媒体或播放功能后再评估删除。 |
| `model_alpha_tests.cpp` | 材质透明度与绘制行为。 |
| `model_alpha_occlusion_tests.cpp` | GPU 像素回读的透明叠层回归，覆盖固定管线与内置效果资源两条路径（效果路径含两个着色器变体）。场景含半透明双层叠加、两个模型与单模型内两种材质提交顺序互换、PMD 与 PMX 两种 ABI、PMX 双面材质、材质 Diffuse.a 由 1.0 变 0.5 的对照，以及几何交错模型的相机变换采样；并断言绘制后恢复调用方的 `D3DRS_ALPHAREF`。 |
| `transparent_material_order_tests.cpp` | 纯 CPU 的透明材质依赖排序与三角形分批，不建 D3D 设备、不启窗口。覆盖深度依赖排序与冲突循环的判定、退化与非法输入、三角形预算、以及用独立采样合成器对随机几何逐像素验证分批结果。 |
| `transparent_geometry_tests.cpp` | 纯 CPU 的透明几何排序，不建 D3D 设备、不启窗口。标准档按三角形中心深度稳定排序、以 ordinal 破同深度；低档用 64 个远到近计数桶，同桶保持输入顺序；两档都只重排，每个有效输入面恰好返回一次、顶点分量不变。分割回归在最高档运行，核对分片保留来源 draw/ordinal、分割顶点插值，以及同一屏幕点沿结果的深度不递增。 |
| `capture_downsample_tests.cpp` | 用独立面积重叠算法核验真实 D3D9 缩放像素；检查逐行水平相位、非整数比例及无效目标尺寸。 |
| `pmx_text_tests.cpp` | 缓冲与直接读取两种模式下的 98 个异常 PMX 文本样本；检查截断、奇数字节长度、过大前缀、贴图表清理、单字符及空名称回退。 |
| `model_edge_tests.cpp` | 轮廓宽度与镜头坐标空间、PMD/PMX 膨胀及 GPU 输入。 |
| `device_reset_tests.cpp` | 设备重置和资源恢复。 |
| `filename_encoding_tests.cpp` | 中文/日文路径与编码转换。 |
| `morph_import_tests.cpp` | 表情记录步长、Unicode 控件、VSQ 歌词/时间隔离；可另传 VMD 验证实际素材。 |
| `ui_language_tests.cpp` | 多语言字符串、控件语义、时间轴固定标题翻译、实际 PMX 导入后的 Unicode 绘制与中日文字号。 |
| `ui_message_tests.cpp` | 实际对话框资源和消息。 |
| `video_export_range_tests.cpp` | 由私有桌面 runner 显式运行；实际三语导出对话框的范围默认值、宽高比例初始化、重复打开、取消和确认读回。两档共用，编码器枚举用固定选项隔离；不创建设备、不编码视频、不注册普通 CTest。 |
| `panel_key_tests.cpp` | 时间轴登记、编辑与帧链。 |
| `bone_scale_tests.cpp` | 原生缩放的坐标变换、斜切保留、层级继承、全骨骼类型、关键帧插值、撤销重做与剪贴板。 |
| `bone_rotation_drag_tests.cpp` | CPU 回归，经真实拖拽与骨骼变换入口验证 PMD/PMX、视口与轴按钮、局部/全局/视角坐标各 10,000 次往返操作；检查旋转与矩阵不积累缩放、多选轨道与子骨半径、旧漂移和非法输入，以及完整档合法缩放保留。两档均注册。 |
| `bone_scale_hud_tests.cpp` | 隐藏窗口的真实 GPU 回读：四个缩放按钮、相机和配件禁用蒙版、透明边缘及三档 DPI；不读取桌面。 |
| `bone_scale_io_tests.cpp` | PMM 缩放记录和单文件 VMD 扩展的编解码、校验、未知版本、帧偏移及骨骼匹配。 |
| `affine_skinning_tests.cpp` | 非均匀缩放和斜切的独立数值参考；BDEF、PMD、SDEF 的位置和法线、极分解及单位缩放旧路径一致性。 |
| `physics_bake_tests.cpp` | 烧录事务、撤销重做与三语隐藏对话框。另传 PMM 和输出目录时验证离线模拟、取消隔离、参数生效、范围、选择、回放与 PMM/VMD 往返；不修改输入素材。 |
| `dense_keyframe_checks.hpp` | `physics_bake_tests --dense-playback`，完整档注册为 `dense_keyframe_playback`。验证 0–500 帧密集轨道的半帧姿势、非线性缩放、边界、重播、随机跳帧、末端骨骼及无效游标回退；打印轨道求值耗时，不换算为播放 FPS。 |
| `bake_playback_tests.cpp` | 隐藏生产宿主加载 PMX，实际烘焙 0–500 帧，比较总长 3845 帧工程的区间内外及边界。参数为 `<模型.pmx> <keyframes\|physics> [播放物理模式 0–2] [scale]`；`scale` 添加一条缩放测试轨道。记录 CPU/GPU 等待耗时和逐帧骨骼矩阵摘要；`MMS_BAKE_PROFILE_ONE_MODE=1` 只测模型编辑面板。原模型不改写。 |
| `physics_bake_settings_tests.cpp` | Bullet 默认值、严格 UTF-8 JSON、数值边界、Unicode、格式错误、原子保存及失败保护。 |
| `bone_registration_tests.cpp` | 骨骼选择、重叠骨骼点击轮选、登记、首帧保护、移动、撤销/重做和跳帧。`--pick-cycle` 单独运行 PMD、PMX 在主视口与独立视口中的轮选回归。 |
| `window_interaction_tests.cpp` | 窗口消息、鼠标/选择行为。 |
| `pmm_reload_tests.cpp` | PMM 重载与状态恢复。 |
| `pmm_extension_tests.cpp` | PMM v2 尾部扩展的独立字节布局、CRC32、长度边界、重复键、未知记录与文件位置恢复；新写入 `MMSXPMM2`，读取兼容旧 `MDSXPMM2`；纯文件测试。 |
| `pmm_container_tests.cpp` | PMM 0003 块容器（`PMM3END` footer、`PMM3TOC` 目录）的独立字节布局、CRC32、未知块保留、`MMSXPMM2` 与旧 `MDSXPMM2` 识别和长度/偏移边界；纯文件测试。 |
| `pmm_project_settings_tests.cpp` | 隐藏生产窗口中的实际 PMM 保存、加载、另存和新建，核对运算器类型、缺失类型回退自动、五档排序上限的菜单状态与单一档位字段、未知档位字节原样保留与显式替换、旧开关记录不参与档位判断及未知数据保留。显式传入测试产物目录，可附加 PMX 模型路径；需 D3D9 设备。`--pmx-boundary <输出目录>` 单独运行无顶点测试 PMX 的真实载入器绑定名与 v3 边界检查，不需要 D3D 设备。 |
| `morph_runtime_tests.cpp` | 表情运行时计算和边界。 |
| `runtime_layout_tests.cpp` | 结构体布局相关行为、标准姿态骨骼遍历、稀疏轨道清理、配件、物理及全局帧。 |
| `mmdxshow_interface_tests.cpp` | 加载实际 DLL 验证 COM 子接口、推帧状态与析构。 |
| `physics_pose_tests.cpp` | 骨骼姿态与物理登记中的非有限数值防护：PMM v1/v2 读取与 `RegisterBonePoseAtFrame` 登记时把 NaN/Inf 位置与旋转修复为可用值且保留正常的非单位四元数，物理回写后异常刚体按跟随矩阵复位，超大关节距离用双精度算回并夹到半径界。含样例 PMM 的 23 条无效关键帧回归与修复前后对照入口。 |
| `pmx_joint_tests.cpp` | PMX joint 端点校验：刚体 A 缺失、刚体 B 缺失、双方缺失、索引越界、刚体表为空与刚体未创建时，该 joint 以约束号 `-1` 无效化并保留原始编号与名称；核对中日英三语警告标题与正文，以及关闭模型时只回收自己的约束、不误删其他模型的 0 号约束。 |

部分测试需要 Win32 窗口、D3D 设备或 DLL，不能将沙箱/硬件初始化失败视为功能通过。测试目标统一注册在仓库根目录的 `CMakeLists.txt`（`tests/` 下没有 CMakeLists.txt），部分目标受 `MIKUMIKUSTUDIO_BUILD_*` 选项控制。`ctest --test-dir build -C Release -N` 列出当前构建配置实际注册的项目，README 不代替该清单：注册数量随 CMake 选项与源码变化，以该命令的输出为准。

## 手动验证与性能：按用途保留

| 标记 | 文件 | 依赖与清理条件 |
| --- | --- | --- |
| `integration-manual` | `scene_lifecycle_tests.cpp` | 需要本地 PMM 与素材；普通目标与 MME 目标共用同一份源码，MME 目标还需外部插件。`--startup-view` 在仅执行过 `InitDefaults`、窗口初始化与 D3D 初始化的状态下核对启动视点（相机面板、cameraPos = (0, 10, 0)、距离 -45、两个视图偏移清零、地面原点落在 `atan(10/45)`），并断言一次帧求值（`ResetAppState` + `ReloadModels`）不会改变世界矩阵与视图矩阵。`--playback-view` 验证选择模型后按 P 播放与停止时均保持自由视角，播放过程中的旋转、平移、缩放有效，并隔离残留的镜头骨骼目标与延迟刷新。`--view-tools` 验证以模型首骨骼或选中骨骼为旋转中心、绑定相机的平移与缩放及登记回读、骨骼批量变换与撤销重做。设置环境变量 `MIKUMIKUSTUDIO_HUD_PREVIEW_DIR` 时，该模式另外把相机态与配件态的整帧写进该目录（`hud-camera-rows.png`、`hud-accessory-rows.png`），用于核对两块操作按钮行；不设置时不写文件。MME 目标的 `--material-names` 模式核对由插件实际导入的材质名查询结果。不得以“与单元测试重复”为由删除本测试；只有在替代测试覆盖真实加载、渲染、物理、编辑与重载之后，才可考虑合并。 |
| `benchmark` | `frame_benchmark.cpp` | 普通/MME 性能目标共用源码，结果依赖设备、模型、效果、配置。保留工具，过期结果可按清理清单 C07 归档。 |
| `hardware-manual` | `d3d12_interop_tests.cpp` | D3D9/D3D12 互操作验证，显式构建运行；当前不在 CTest 默认回归集。 |
| `hardware-manual` | `d3d12_skinning_tests.cpp` | D3D12 骨骼计算及正式 D3D9 缓冲桥接验证，需本地 PMD/PMX 素材及硬件设备；当前不在 CTest 默认回归集。 |
| `hardware-manual` | `skinning_backend_tests.cpp` | 隐藏窗口验证各硬件适配器、PMD/PMX 权重与附加 UV、缓冲对象身份、CPU 回退、运算器设置及三语言菜单。另核对自动测速排名、CPU 胜出、无效候选排除、校准复用，以及模型缓冲替换和删除后的失效。需 D3D9/D3D12 设备；显式构建运行，不在 CTest 默认回归集。证据见 [GPU 蒙皮记录](../docs/records/performance/devices.md#record-gpu-skinning) 与 [自动选择记录](../docs/records/performance/devices.md#record-skinning-autoselect)。 |
| `integration-manual` | `hidden_morph_tests.cpp` | 显式 `--model` 指定 Rou PMX 样本；隐藏生产窗口中验证时间轴隐藏表情的单击/双击/Shift、四栏隔离与普通权重/关键帧登记、组合动画的顶点/UV/材质结果。`--mode bounds` 独立验证内存面板值 0/5/255，不保存模型。需本机 D3D9 与 Data，不加入默认 CTest；仅在等效实际交互回归替代后移除。命令与证据见 [隐藏表情验收](../docs/records/animation/editing.md#record-hidden-morph-panel-safety)。 |
| `integration-manual` | `physics_scene_tests.cpp` | 传入现有 PMM 或 PMD 路径推进真实物理场景，逐帧断言全部骨骼姿态与刚体变换为有限值，覆盖跳帧与反复跳转；用于修复前后同条件对照，不注册进 CTest 默认回归集。 |

## 本地临时验证产物

### ABC 与网格烧录回归

构建 `alembic_export_tests`、`vertex_capture_tests`、`mesh_bake_cache_tests`、`mmv_fixture`、`mmv_dx12`、`mmv_compat` 和 `mmv_runtime_tests`。ABC 测试程序名保留为 `MikuMikuDance.exe`，位于 `build/tests/alembic/Release`；从各自独立运行目录执行：

```text
MikuMikuDance.exe <测试输出绝对路径> native
MikuMikuDance.exe <测试输出绝对路径> mme
MikuMikuDance.exe <测试输出绝对路径> combined
MikuMikuDance.exe <测试输出绝对路径> extra
MikuMikuDance.exe <测试输出绝对路径> varying
```

每个运行目录需要 `Data`。`native` 不放插件；`mme` 放 MME 的 `d3d9.dll`、`MMEffect.dll`、`MMHack.dll`；`combined` 再放正式 `MMV.dll` 与 `MMV` 资源目录。`extra`、`varying` 使用 `build/tests/mmv-fixture/Release/MMV.dll`，分别验证追加几何与逐帧拓扑出现/消失。夹具 DLL 只用于测试，不放入正式运行目录。测试自行隐藏窗口，不读取桌面画面。

2026-09-20 已在 Win32/x64 布局编译通过后删除临时 `build/layout-probe/`，同时清理 5 份旧 followup 日志，具体文件见清理清单 C08。正式布局约束保留在头文件断言中，测试源文件及现有测试构建目标全部保留（数量以实际文件与 CTest 清单为准，见上节）。之后产生的本地探针和迭代日志仍不应补加入 Git；清理前确认无需符号、失败日志和探针证据。

2026-09 全量审查复核（命令与结果见 [代码审查报告](../docs/records/development/audits.md#record-full-review)）：`ctest --test-dir build -C Release -N` 报 22 项注册测试，`ctest -C Release` 22/22 通过。`tests/*.cpp` 实际有 26 个文件，其中 22 个对应 CTest 注册目标，另 4 个是 `d3d12_interop_tests`、`d3d12_skinning_tests`、`frame_benchmark`、`scene_lifecycle_tests`（需设备或本地素材，不在默认回归集）。该轮同时发现 CTest 覆盖不到的两条相机/视图缺陷——`--playback-view` 与 `--view-tools` 在真实工程上失败，逐条见 [缺陷清单](../docs/records/development/audits.md#record-code-review-findings)。

全工程候选与条件见 [清理清单](../docs/records/development/workspace.md#record-cleanup-inventory)。新增测试时更新本表；不要把一次性调试输出留在测试断言之外。


## EMM 自动保存回归

`scene_lifecycle_mme_tests` 的输出仍名为 `MikuMikuDance.exe`。在独立运行目录放入同版本 `MMEffect.dll`、`MMHack.dll`、`d3d9.dll` 和 `Data` 后，执行：

```text
MikuMikuDance.exe <尚不存在的输出绝对路径前缀> 1 --emm-save-off [素材PMM绝对路径]
MikuMikuDance.exe <另一个新输出前缀> 1 --emm-save-on [素材PMM绝对路径]
MikuMikuDance.exe <另一个新输出前缀> 1 --emm-default-on [素材PMM绝对路径]
MikuMikuDance.exe <另一个新输出前缀> 1 --emm-default-off [素材PMM绝对路径]
```

前两种模式在隐藏窗口内用真实保存菜单测试初始开关、关闭状态下打开工程后再开启、关闭时保留旧 EMM、再次开启时更新，以及中日文路径另存为。提供素材时须包含模型及可加载的效果；测试核对效果分配内容在重复保存与另存为后保持一致。测试只向新输出前缀写文件，不保存输入素材。每次运行须更换输出前缀。详细证据见 [EMM 自动保存修复记录](../docs/records/effects/compatibility.md#record-emm-autosave)。

`--emm-default-on` 在切换菜单前检查启动时默认勾选，适用于隔离运行目录中没有 `MMEffect.ini` 或缺少 `[System] EMMAutoSave` 的情况。`--emm-default-off` 用于已有 `EMMAutoSave=false` 的运行目录，确认启动时保留用户关闭状态。两种模式随后执行同一套保存、重开、切换和另存为检查。

如果素材 PMM 仅供 MME 读取同名 EMM 映射，设置 `MMS_EMM_FIXTURE_MODEL` 为对应模型的绝对路径。测试先载入模型，再通知 MME 读取映射，不把这种占位文件交给 PMM 加载器。初始关闭自动保存时，首次保存没有 EMM；测试重开工程后显式恢复素材映射，再验证开启后的保存。

## 骨骼缩放物理回归

`affine_collision_shape_tests.cpp` 验证球、盒、胶囊在非均匀缩放和斜切下的解析支撑点、包围盒与源形状隔离。`physics_scale_tests.cpp` 验证三种刚体模式、继承偏移、joint 两端连接点和平移限位、复位、实际 GJK 碰撞及物理回写不重复缩放。`physics_bake_tests.cpp` 包含当前场景已缩放时的离线隔离和 PMM/VMD 往返。测试隐藏运行；仅在相关功能移除或完整迁移覆盖范围后删除。

## 密集透明累积专项

`model_alpha_occlusion_tests --low-alpha` 使用内置 SM3 资源 118，比较 256 层 alpha=1/255 和 512 层 alpha=2/255 的黑色面在白底上的结果与双精度解析式，容差 2，并核对最终 A=255。该模式明确选择最高档；`--peel-budget` 是保留的历史参数名，选择高档直接排序完整三角形，使用 1152 层检查最近的绿色层仍显示，不再表示固定的 1024 层预算。最高档以 `MIKUMIKUSTUDIO_TRACE_FILE` 的实际层数、`complete` 与 `committed` 核对完整剥离，不能仅凭最终像素把回退误报为剥离通过。背景窗口隐藏，像素来自目标读回。历史实现见[浮点合成修复](../docs/records/rendering/transparency.md#record-cloud-float-compositing)，当前夹具与档位复核见[透明排序回归复核](../docs/records/rendering/transparency.md#record-transparent-sort-validation)。

三个专项均支持附加 `--msaa`，使用实际 4x 目标和同尺寸 resolve 后读回；硬件不支持时明确失败。MSAA 三项也注册 CTest。`--intersections` 同时覆盖不透明表面夹在两个透明表面之间，保证后方颜色不会混进前方透明层。

普通遮挡场景默认显式选择标准档，`--precision low|standard|high|maximum` 可分别验证四档；不能与专项模式混用。产品默认仍为关闭，原始提交顺序由独立的 `--sort-off` 及 MME 模块存在用例验证。完整档注册上述开启排序的 GPU 用例；最小档仅注册此测试程序的两项关闭排序用例，并在创建窗口前拒绝开启排序的参数。测试直接发布纹理池条目时必须使查询缓存失效，并检查路径确实解析到预期槽位和纹理；清理时先清空条目、失效缓存，再释放纹理。


## 五档透明排序上限与 PMM v2

`pmm_project_settings_tests <新输出目录> [PMX绝对路径]` 使用隐藏的生产窗口，核对五档（低、标准、高、最高、关闭）、真实保存/重开/另存、单一档位字段、关闭覆盖旧档、菜单三语言、未知档位字节保留与显式替换、旧开关记录原样写回且不参与档位判断，以及 CRC 异常隔离。调用真实菜单、写入器和加载器；夹具直接通过扩展 codec 构造，不经新写入器预先迁移。

`pmm_project_settings_tests --pmx-boundary <输出目录>` 在测试目录生成一个无顶点的 PMX，交给真实 PMX 载入器，核对 19 字节名称、21 字节名称的 19 字节镜像、含半个 CP932 字符的 19 字节镜像、非 CP932 名称和同一镜像的冲突，并逐项检查 `0002` 与 `0003` 的选择边界。该模式不创建顶点缓冲和材质，因此不需要 D3D9 设备。

`model_alpha_occlusion_tests --maximum-limit` 复用黑色低 alpha 面，1152 层和 4608 层均以 alpha=1/255 混合，对照双精度解析式及目标 A=255。支持 `--msaa`，两种模式均注册 CTest。该最高模式使用 16×16 目标和与生产云 build 一致的硬件顶点处理，并断言设备实际标志；层数、几何和九个解析式采样点保留。`--maximum-full-size` 可执行原 96×96 压力用例，不用于默认 CTest。其他旧模式保持原尺寸及软件顶点处理。它检查最终像素；诊断日志另核对是否完成全部层，不能仅把预算数值相等当作正确性证据。

`frame_benchmark_mme` 的 `--sort-precision` 接受存档值 0..4，与档位编号一致：0 低、1 标准、2 高、3 最高、4 关闭。JSON 同时记录请求值与实际生效值，旧 build 对未知命令的忽略不能被写成已选择最高。`--verify-view-toggles` 兼容旧开关和新的单一档位；私有排序几何不要求原始索引缓冲发生改变，排序正确性由像素用例验证。背景采样方法和原始结果见[五档上限与 PMM 保存](../docs/records/rendering/transparency.md#record-transparent-sort-five-tiers)。

## 烘焙通道与动态骨骼池

`bone_key_pool_tests` 和 `bone_registration_tests` 验证超过旧池边界的增长与登记。`physics_bake_tests --capacity` 验证大槽位、撤销和 VMD；`physics_bake_tests --capacity-scene <PMM或PMX> <新输出目录>` 运行真实场景和 PMM/VMD 往返，输出目录须为新的测试位置。

`channel_bake_tests` 不依赖 MMV，覆盖场景/模型通道选择、附件子参数、范围外曲线和中途提交失败恢复。`mmv_binding_animation_tests` 覆盖独立原生效果启停轨道；`mmv_ui_integration <PMX> <FX> <新输出目录>` 通过实际 DLL 和隐藏窗口核对三语、缩放、拖动和连续帧下拉框。`alembic_export_tests <新输出目录> native|mme|combined` 验证对应运行目录下的源网格与渲染缓存。实际执行结果及失败记录见 [验收说明](../docs/records/animation/baking.md#record-baking-channels-validation)。

## MMDModern 查找优化验证

`lookup_cache_tests.cpp` 是保留的回归测试，覆盖纹理重载、资源销毁后地址复用、同名纹理、大小写、满池和稀疏绘制顺序。`effect_api_tests.cpp` 同时检查当前对象缓存的注册、重排和重复渲染失效。

`playback_benchmark.hpp` 由 `frame_benchmark.cpp` 引用，仅用于后台测评。`--playback-seconds 15 --json <路径>` 启动正常播放并记录逐帧 Present、绘制提交和播放游标。此计数方式只在 MME 设备代理上验证通过；系统原生 D3D9 会切换调用表，该路径返回无效结果，不进入 FPS 汇总。删除后台播放模式时一并删除该头文件。

`tools/run_lookup_compare.py` 对封存的前后 EXE 交替采样。所有性能结果保留原始日志、JSON、CSV、资源哈希及失败样本。具体测评范围与结果见 `docs/records/performance/cpu.md`。

低 CPU 预算对照同时传入 `--cpu-percent 2 --logical-cpus 4,6`。百分比按全机 CPU 预算计算；逻辑核心编号须先核对当前机器。`tools/limited_process.py` 挂起创建测试进程，设置并回读 Job Object 硬上限和亲和性后恢复执行。它只管理新建的测试子进程，记录进程及全系统 CPU 时间；显卡共享负载另存 `*-gpu.csv`。限制不预留核心，不限制 GPU，也不改变正式程序的运行设置。该辅助模块随资源限制测试入口保留或一并删除。

本轮后续按用户要求固定 `--cpu-percent 2 --seconds 45 --logical-cpus 4,6`。长采样统一排除前 5 秒，并要求余下首末帧区间达到目标区间的 90%，即至少 36 秒。原始 15 秒短采样保留，不与长采样混算。资源限制工具在退出或超时时清理自身作业中的子进程，不留下后台构建进程。

用户要求同时比较两个 MMS 版本时，使用 `tools/run_lookup_simultaneous.py`。此脚本仅用于该并行测评：前后版本分别限为 1% CPU，合计预算为 2%；分别使用逻辑处理器 4、6，并在四轮中交换核心。两进程都完成限制回读后由同一启动屏障放行。每轮采样 90 秒，排除前 5 秒后逐版本计算帧率；保留各自绘制计数、CPU 记录和全显卡共享负载。采样重叠区间根据结果文件完成时间估算，不能视为逐帧同步。删除并行测评入口时一并删除此脚本。

用户要求直接用正式程序载入工程时，使用 `tools/run_actual_project_compare.py`。传入 `--before`、`--after` 两个已校验的正式 EXE，`--runtime` 正式运行文件目录，`--mme-runtime` 相同 MME 文件目录，及 `--scene`、`--output`。默认两侧各限 1% CPU；用户明确取消上限后传入 `--uncapped`，同时取消固定核心。默认四轮，每轮在两侧开始正常播放并预热 10 秒后，共同采样 90 秒。

该入口限定本报告的两份 EXE 哈希，只读状态偏移也与这两份程序绑定。它通过程序原有的工程命令行与播放处理函数运行隐藏窗口，不修改 EXE、不注入 DLL、不写进程内存。正常帧循环日志提供完成 `FrameDriver` 的次数，约每 100 毫秒读取一次，按首末完整日志批次的观察时间计算频率。该指标不能声称是成功 Present 数、桌面显示 FPS 或逐帧耗时分位数。需要这些指标时应另行验证可用的 Present 采集器。

每轮保留相同运行资源哈希、加载模块、播放游标、视口、场景设置、CPU 记录和 GPU 共享负载。测试结束仅关闭自身作业中的进程，不保存输入 PMM。`limited_process.run` 的可选 `stop_event` 用于这种外部观测结束后的清理；传入 `None, None` 作为 CPU 和核心参数时，回读确认作业没有 CPU 速率控制，并保留继承的处理器亲和性。删除正式程序对照入口时一并删除该脚本。

## 日区名称边界

`ui_language_tests --name-gaps` 验证旧式名称解码失败回退、双字节截断、UTF-8 标记、模型选择框首次显示和真实 PMX 控件显示；`--model-selectors` 单独验证点击前的名称、绑定项和已有选择保留；`--name-lookup-benchmark` 单独测量 40 万次有效名称查询的 CPU 耗时。

在 Windows 11 和已安装 Windows SDK 的环境中，运行 `python tests/windows_locale_runner.py <ui_language_tests.exe> <新输出目录>`，为测试副本设置日区、中文、西文和 UTF-8 进程编码。脚本核对实际代码页并保存哈希与结果；不改变系统设置或正式程序清单。若系统不支持指定代码页，检查失败，不记为通过。保留这组回归，移除旧式名称兼容时再评估删除。记录见[日区名称边界修复](../docs/records/interface/languages.md#record-japanese-name-gaps)。

在命令末尾加 `selectors` 可单独运行模型选择框测试。使用 `python tests/windows_locale_runner.py <menu_language_tests.exe> <新输出目录> menu` 验证真实菜单资源，检查翻译项从灰色恢复可用且不改变勾选值。

`menu_language_tests --first-paint` 在独立非活动桌面检查首次展开的完整菜单。`--first-paint-late` 在首次绘制后注入空白，`--first-paint-no-idle` 屏蔽菜单空闲通知，`--first-paint-slow` 同时模拟较慢的初始化与延后空白。这些用例不移动真实鼠标，不发送 `WM_PRINT`，检查补画结束后不会持续刷新。像素检查关闭菜单动画，非活动桌面的动画回读不计入有效画面。保留这些回归，删除展开补画逻辑时再评估移除。背景见[菜单展开补画记录](../docs/records/interface/controls.md#record-popup-opening-repaint-20261021)。

`effect_compile_cache_tests --small-stack` 在栈预留仅 128 KiB 的线程上执行生产缓存冷、热路径，覆盖深层 MME 回调进入路径解析时的栈占用。CTest 名称为 `effect_compile_cache_small_stack`。该探针不加载 GPU 或真实 D3DX，真实 DLL 验证仍使用 `--integration`。保留此回归，移除编译缓存时再评估删除。背景见[缓存栈溢出修复记录](../docs/records/performance/startup.md#record-mme-cache-stack-fix-20261021)。

## PMD 贴图选择模式回归

构建 `pmd_selection_render_tests` 目标。测试程序位于 `<build>/tests/pmd-selection/Release/MikuMikuDance.exe`，支持完整包与最小包。通过 `python tests/pmd_selection_fixtures.py <样例目录>` 生成原创 PMD。

参数依次为 PMD 的绝对路径、已存在的输出目录，以及可选的 `--hidden-default`、`--fixture`。`--fixture` 对生成器提供的模型启用双色像素断言；测试第三方素材时不传此参数。默认要求 `MMS_REPAIR_*` 私有桌面；`--hidden-default` 显式允许在默认桌面创建隐藏、不激活窗口。每次运行使用独立输出目录，程序保存 8 张 PNG 并输出 `SAMPLES` 行。

将输出保存为日志后，运行 `python tests/pmd_selection_fixtures.py --compare <参考日志> <重映射日志>`，可检查 8 个状态下的采样颜色完全一致。等价组为 `ordered` 与 `remapped`、`fallback-reference` 与 `missing` 或 `last`、`reverse-reference` 与 `reverse`、`shadow-ordered` 与 `shadow-remapped`。

MME 测试需在独立目录放入三个 MME DLL，日志必须显示 `PLUGINS mme=1 mmhack=1`。MMV 测试使用另一独立目录，日志必须显示 `mmv=1`。相关复现、验收及尚未确认的原报告范围见 [PMD toon 修复记录](../docs/records/rendering/materials.md#record-pmd-toon-selection-fix)。

## 阴影、录制预览和 MME 分配显示

`effect_file_watch_tests` 检查真实 Windows 目录通知，覆盖普通保存、原子替换、删除、重建及无关文件修改。它在传入路径后附加进程号生成临时目录，仅清理自己的夹具。CTest 名称为 `effect_file_watch_changes`。MME 固定阴影的专用缩放和 ray 源码改写已移除，相关源码适配测试也已删除。

`python tests/mme_emd_runner.py <mme_display_tests 的 MikuMikuDance.exe> <MME目录> <输出目录> --file-watch` 在四种语言路径下保存测试 FX，验证原生自动重载开关、实际重新编译和屏幕输出颜色。完整包与最小包均运行该模式。`viewport_scale_performance_runner.py --trace-targets --project-settings` 记录实际 StageNoRay、ray 在 100%、25%、25%、100% 预览下的绘制目标，检查 MME 固定阴影保持原尺寸、默认阴影随比例调整。`--no-file-watch` 仅用于关闭通知缓存的对照。实施范围和验证记录见 [MME 预览与自动重载](../docs/records/performance/preview.md#record-mme-preview-optimization)。

`display_compatibility_tests` 与 `display_compatibility_runner.py` 保留为回归测试。它们使用隐藏生产窗口和真实 MOV 录制，验证阴影精度滑条与数值同步、旧动态精度设置不再启用独立开关、菜单开关可双向操作、预览拖动和尺寸切换，以及导出分辨率和 PNG 帧内容。各轮写入新的输出目录。

`mme_display_tests` 是需本地 MME DLL 的隐藏集成测试，输出名为 `MikuMikuDance.exe`。将它和同一版本的 `d3d9.dll`、`MMEffect.dll`、`MMHack.dll` 放入独立测试目录。先运行 `python tests/mme_display_fixtures.py <输出目录>` 生成原创小模型，再分别传入 `bilingual.pmx --bilingual` 和 `alpha-order.pmx --alpha-order` 的绝对路径。前者验证实际列表像素、翻译开关和未改变的绑定文本；后者观测真正的 D3D 材质绘制顺序。测试不会保存输入模型，也不显示或激活窗口。保留这组测试；移除相应兼容行为时再评估删除。

`frame_benchmark` 和 `frame_benchmark_mme` 新增 `--adaptive-shadow 0|1` 与 `--dynamic-precision 0..9999`，JSON 记录实际贴图尺寸、精度值和阴影投影矩阵。后者通过真实滑条消息输入，检查面板刷新不会复位精度，也不会改变光源或阴影范围。验收数据与限制见 [阴影、预览与 MME 修复](../docs/records/rendering/shadows.md#record-shadow-preview-mme)。

阴影覆盖与细节层由模型和配件共享。`native_shadow_tests --bounds-only` 检查最终相机六面裁剪、偏移投影、方形覆盖盒、画外投影物深度、真实近平面覆盖、连续中景层、5 万次密度分配顺序和宽正交视图的预算限制。旧范围盒和迟滞函数的检查只保留为历史参考函数回归，不代表当前生产策略。

`native_shadow_tests <production shader> --detail-only` 检查两种阴影模式的细节采样、近层与中层混合、配件局部变换和斜面假阴影。`--coverage-only` 检查离开覆盖区不改变材质光照。`frame_benchmark` 的 JSON 包含 `coverage_width`、`detail_coverage_width`、`near_coverage_width`、实际分辨率和每单位采样数；`near_density_floor_met` 同时要求近层有效且实际密度达到下限。`--adaptive-shadow 0|1` 仅调整屏幕需求，不切换覆盖盒。实现边界见[可见画面统一覆盖记录](../docs/records/rendering/shadows.md#record-visible-near-density-20261024)。

`frame_benchmark --camera <起点七值> --shadow-camera-sweep <终点七值> --sweep-frames 121` 固定模型与物理状态，逐帧插值相机。七值依次为位置 XYZ、距离、偏航角、俯仰角、滚转角，角度单位为度。需要同时指定 `--throughput`、`--viewport` 和绝对 PNG 输出路径。测试在图片同目录输出 `.sweep-0000.png` 和对应 JSON，记录覆盖层、近层、中层、过渡层的实际尺寸、密度、范围及混合起点。它可对照两种操作得到的等价视图，并检查推进过程是否发生分辨率跳档。`--detail-only` 另包含第三细节层的 72 组 GPU 混合与负控检查。
`frame_benchmark --verify-shadow-cycle` 可用于指定的 PMX 模型，需同时提供 `--throughput`、`--viewport`、`--self-shadow 1|2` 和 PNG 输出路径。它保存固定、禁用投影、动态低精度、动态高精度及恢复后的画面和深度贴图；禁用投影仅临时修改内存中的材质标志，不写入模型。真实模型验证记录见 [自阴影修复](../docs/records/rendering/shadows.md#record-native-shadow-coverage)。

`native_shadow_tests <production shader> --modes-only <输出目录>` 使用真实覆盖矩阵的逆变换和生产模式校准，比较相同物理遮挡距离下的模式 1、模式 2；原版着色器须位于生产着色器同目录的 `original/skin_effect_sm3.fx`。测试覆盖原版中心分段、不同场景大小和覆盖拟合，避免仅给两个模式分别运行同一测试却漏掉差异消失。

`frame_benchmark <工程> --throughput --viewport 1024x768 --verify-shadow-modes <绝对PNG路径>` 在同一加载场景内冻结姿态，保存 `1→1→2→1` 的四张截图与 `.shadow-modes.json`。首张 `.mode-1-control-start.png` 用于估计不切换模式时的重复渲染波动。schema `/3` 允许两模式按原版标尺改变 XY 采样范围，检查共享深度、细节区域、计划密度与实际分配容量，以及切回后的矩阵和交接起点复原。动态开启时允许密度超过原版基准，交接起点可随实际粗层密度变化；动态关闭仍检查固定基准。不自动宣称截图逐像素相同。此项与相机扫掠、其他阴影循环验证互斥。

统一精度检查覆盖原版中心 XY 数值导数、全部滑条数值、动态深度分配、不同物体大小、拆分/合并格网、最低资源尺寸和分配回退。物体不参与目标密度计算，最低贴图尺寸只增加外围覆盖。普通回读、模式回读与扫掠 JSON 提供 `reference_density`、`actual_density` 和 `reference_ratio`；覆盖图按实际采样纹理计算，包含滤波路径。实现与实测见[统一采样精度](../docs/records/rendering/shadows.md#record-uniform-shadow-precision-20261024)。

## 原生自阴影的大场景覆盖回归

动态密度检查覆盖滑条单调、近景额外采样保留 80%、容量受限时的实际削减、远景不超过固定与屏幕两项预算、最终投影等价、正交视图不受深度影响，以及物体包围盒不改变目标密度。交接检查覆盖实际粗层容量、正交缩放连续性和有效混合宽度。模式 JSON 使用 `frame_benchmark/shadow-modes/6`，记录 `close_layer`、`fixed_capacity_density`、`far_density_within_fixed_capacity` 和 `fade_matches_geometry`，并用 `allocation_box_width`、`budget_resolution`、`allocation_within_unchanged_budget` 检查平移覆盖盒没有扩大分配预算。预算从原始名义盒独立重建，不用扩展后的视锥包围球反推。动态桥接层属于中近景，其远端之外只允许覆盖图贡献；覆盖图预算从原始范围独立推导。达到设备上限后，计划密度增加不能视作实际提升。

`CheckExpandedBridgeSampling` 检查两种模式、四种屏幕需求、全部 10,000 个档位和 212 个深度的混合贡献，验证滑条全序与远端贡献归零。集成对照以关闭滤波的真实工程为主，包含四组固定模式像素对照、两模式共 242 帧连续缩放，以及从实际相机和光照矩阵推导的朝向相机平面采样跨度。该平面探针不等同于模型实际三角形。参见[中近景接管记录](../docs/records/rendering/shadows.md#record-shadow-takeover-20261025)。

覆盖盒平移检查包含透视、正交、偏轴投影、多种相机与光照方向及远裁剪。每个角点须落在光空间 XY 的 0.8 留边范围内，矩阵仅允许 XY 平移变化；公共深度、贴图分配和实际密度须保持不变，无效输入须回退。实景边缘另以同镜头无阴影图消除墙纸纹理干扰，测量明暗过渡宽度，不能只凭图层元数据宣称粗细双轮廓消失。参见[覆盖盒利用率记录](../docs/records/rendering/shadows.md#record-shadow-packed-box-20261025)。

加强滤波检查同样位于 `--detail-only`。原生接收端与独立的双精度逐点遮挡比较公式对照，覆盖背景深度不变性、窄阴影、同密度不同贴图尺寸、图层交接、斜平面和亚像素移动。共享 `DepthPrefilter` 的深度卷积另行验证，不能用它代替原生遮挡比较的预期结果。距离渐变检查同时运行滤波开、关两组。真实场景须另留重复渲染对照，避免把少量边缘像素的运行间波动判为滤波变化。参见[远近景采样与遮挡滤波记录](../docs/records/rendering/shadows.md#record-shadow-far-filter-20261025)。

`--direction-only` 单独检查轮廓重建，完整 `--detail-only` 也覆盖它。旧核的严格公式对照显式关闭内部开关；重建检查先验证生产默认值开启，再比较解析直线的 50% 轮廓位置、沿边起伏和 10%–90% 法向宽度。建筑的 16 个关键浅角组逐组要求起伏至少下降 30%，法向宽度增长不超过 25%，且优于将旧结果柔化至同宽的对照。人物部分阴影保持原浓度，法向宽度增长不超过 15%；细条面积至少保留 95%，并限制拐角变化。连续移动以扣除真实位移后的轮廓和细条面积为依据，灰度最大步长仅作诊断，避免把变锐后的正常亮度变化误判为闪烁。缩小回退、斜面、背景、等密度交接及 `s0/s6` 隔离另行检查。不能放宽旧核像素误差门限代替几何验收。见[轮廓重建记录](../docs/records/rendering/shadows.md#record-shadow-contour-filter-20261025)。

低精度动态交接检查位于 `native_shadow_tests <当前正式着色器> --detail-only`。除原有图层排序检查外，它用各层独立分辨率的斜边和窄条深度纹理，连续扫描真实相机深度混合参数，对照旧 35% 区间，并检查两种模式、端点、亮度范围和默认参数复位。屏幕精度规则另生成 2620 帧 GPU 检查，使用新的交接起点，覆盖完整权重区、精确端点和远层回退；日志分别记录公式误差与逐步变化，不能把公式正确等同于视觉上无变化。CPU 检查覆盖完整区间、对数比例及共同设备上限下的实际密度顺序。真实工程验证继续用 `--shadow-camera-sweep`，同时对照位移与距离缩放产生的等价视图；静止截图不能代替连续交接验证。证据见[低精度动态交接](../docs/records/rendering/shadows.md#record-shadow-smooth-20261024)与[屏幕精度交接](../docs/records/rendering/shadows.md#record-shadow-screen-density-20261025)。

`--progressive-only` 单独检查三个相邻图层之间的虚拟网格重建，完整 `--detail-only` 也包含这些检查。测试用独立 CPU 纹理重采样对照 GPU 输出，覆盖非整数密度比、全部级别边界、真实粗图端点、部分阴影、窄条、两种模式、仿射映射和无效矩阵回退。D3D9 线性采样的权重量化与几何映射分开验证，不能通过放宽误差门限掩盖矩阵问题。

回归程序请求高性能显卡，并打印实际适配器名称和像素着色器上限。它对内置技术逐个执行设备验证，然后检查像素着色器是否绑定。驱动仍可能根据系统策略选择其他设备，验收时须核对日志中的设备名称，不能用另一块显卡的结果代替程序实际设备的结果。

真实工程还需关闭「滤波加强」，先扫描整段镜头移动，再细扫实际最差交界。以同一世界表面及同镜头无阴影图排除缩放和材质纹理，检查轮廓位移、宽度、双轮廓和整个窗口的分布。修改后必须重新寻找最差位置，不能仅重复旧截图距离。最近层须调整相机朝向或位置，保证接收面仍在画面内。测试输出保存在正式程序目录下的 `Cache/ShadowTransitionSweep-20261026/`。

`native_shadow_tests` 增加聚焦覆盖检查，验证大型场景不会稀释近景阴影精度、小场景保留原投影、缩放视野会调整覆盖范围，以及沿同一光线的远处投影物仍处于深度范围内。`adaptive_shadow_density_runner.py` 检查近景采样密度和实际远景缩图，不以固定贴图尺寸替代画质判断。

`frame_benchmark --extra-model <PMX/PMD> --ground-shadow 0` 可组合人物与简单场景，关闭平面地面阴影以观察场景表面接收的自阴影。JSON 的 `self_shadow` 包含 `ground_shadow_enabled`、`mme_loaded`，用于核验隔离条件。带 `--extra-model` 且没有布局变换时，`--save-scene` 保存测试副本，保留物理模型与原位置。

修复前后图像和限制见[大场景自阴影修复记录](../docs/records/rendering/shadows.md#record-focused-self-shadow-20261022)。

`negative_frame_tests` 验证带符号帧解析、范围解释、负帧与原生无符号关键帧池隔离、插值、复制粘贴，以及 `MMSXPMM2` 记录往返和损坏数据拒绝。`panel_key_tests --bone-translation-reset` 检查主窗口与分离窗口的 XYZ 重置、物理模式读数及撤销重做。`mme_display_tests <模型绝对路径> --model-edit-controls <输出目录>` 使用真实 MME 与隐藏生产窗口，验证骨骼重置、0 帧左移复位、负帧登记、PMM 保存重载、正负帧双向粘贴及播放下限。集成测试输出写入独立目录，不改写输入模型。

`native_shadow_tests <着色器> --bands-only` 检查放大的低精度斜平面：不启用滤波，不添加投影物，解析预期为全亮，避免硬件插值量化制造假阴影。`--detail-only` 同样包含此检查。`native_shadow_cycle.py` 还检查动态开关关→开→关时固定模式无细节图、动态模式有细节图、关闭后阴影数据恢复一致。

2026-10-29 扩展 `--bands-only`：加强滤波增加两种模式、大覆盖盒、不同深度平移、正负斜率及轮廓开关的 48 组无遮挡斜平面检查，不能用扩大阴影偏置或放宽颜色误差掩盖假阴影。

- `bone_registration_tests --auxiliary-undo`：issue #40 的表情和显示／IK／外亲登记撤销。覆盖四个面板按钮、全部表情单次历史、已有键覆盖、0 帧、负数表情独立历史、混合骨骼历史、重做分支截断，以及表情／显示池扩展后正确恢复链接与负载；默认完整测试也包含这些检查。修改撤销记录或登记入口时保留。

### 大场景中的人物固定阴影

`native_shadow_tests <着色器> --bands-only` 另包含最低精度的曲面保护回归：同一粗网格分别使用平面和弯曲法线场，验证曲面不再跨大范围外推切平面，同时平面仍使用补偿。旧版着色器在曲面用例失败，新版通过；`--detail-only` 自动包含它。实际人物与墙面对照见[曲面块斑修复](../docs/records/rendering/shadows.md#record-curved-shadow-filter-20261029)。

`native_shadow_tests --bounds-only` 包含 `CheckFixedModelProjection`，覆盖两种模式、2048/4096 原生标尺、五个精度位置和三种场景大小，检查人物完整落在固定图的全权重区域、保持参考密度和全场景 Z 覆盖，并检查扩大场景不改变人物 XY 投影。

`frame_benchmark --fixed-model-shadows 0|1` 用于关闭/开启固定人物图的对照，默认开启；这不是用户设置。普通截图和相机扫掠 JSON 的 `self_shadow.fixed_model_shadows` 按模型槽记录有效性、尺寸、密度和矩阵。动态模式仍只使用原有相机分层。

`shadow_filter_tests` 的隐藏宿主支持在原参数后加 `--fixed-model-only`，单独检查固定图的分配复用、当前帧有效性、接收模型隔离、动态开关与阴影关闭释放、卸载清理、设备资源释放、分配失败回退及恢复。输出目录必须全新；示例：`MikuMikuDance.exe <无FX模型.pmx> <新输出目录> --mme 0 --fixed-model-only`。它用进程内失败注入测试，不修改模型文件。实际工程对照保存在 `build/Release/Cache/FixedModelShadow-20261029/`。
