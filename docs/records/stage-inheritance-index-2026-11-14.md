# QSPEED Stage.pmm 继承骨骼索引越界

## 输入与定位

先读取 `D:/MMD-Next/patches/MMS-ABI-LOVE-source-migration.zip` 内的 `README.zh-CN.md`，在恢复目录中继续处理，未将迁移补丁再次应用到其他工作树。

本次输入由用户确认为 `D:/MMD-Next/tests/QSPEED菠萝能量仓库 Ray用一键渲染/UserFile/Stage.pmm`，SHA-256 为 `159ad3df96759adc64af5667146124d081258b7df16ab9866782c35aa38c4649`。PMM 内引用 `Model/RENO Warehouse 2L/RENO Warehouse 2L Lite.pmx`。

只读解析工程目录内 23 个 PMX，发现 Lite 和 FULL 两个仓库模型均有以下记录：

- 骨骼数量 48；索引 2，名称 `Y回転`。
- 标志 `0x011e`，其中 `0x0100` 启用旋转继承。
- 继承源 `-1`，倍率 `1.0`，变换层 0。

`pmx_load.cpp` 按有符号索引读入并保留 `tailIdx=-1`。旧 `InheritRotQuat` 未检查源索引，直接读取 `bones[srcIdx].matLocal`；A/B/E 三个调用方直接传入 `tailIdx`。Windows x64 的骨骼记录步幅为 624 字节，所以这会访问骨骼数组之前的内存。平移继承及 PMD 的部分继承路径也有同类访问。

迁移说明所述 02:44:38 EXE/MAP 和旧异常现场未在本次定位中取得；未将其他时间构建的 MAP 当作该次构建。因此上述结论来自实际模型数据、当前源码及本次回归，不能声称已重新还原历史 `EXE+0x266630` 的寄存器现场。

## 修复范围

在 `bone_transform.cpp` 共用有符号上下界检查，保护 `InheritRotQuat`、A/B/E 的平移继承，以及 B/E 的 PMD `RotateGrant` 和 `CoRotate` 源索引。非法源只跳过对应继承，继续计算本骨骼姿态；不改写输入骨骼数据，也不钳制到骨骼 0。

`CoRotate` 的 `tailIdx` 是百分比倍率，检查的是 `tailBone`，因此负百分比继续有效。合法首尾索引及 PMX 的零、正、负倍率保持原公式。

## 验证记录

独立目录：`build/stage-inherit/`。输入 PMM/EMM 已备份到其中的 `evidence/input-backup/`，原模型和工程未修改。

新增 `bone_inheritance_index_tests`，覆盖 A/B/E、物理前后通道、原生及私有数学上下文，使用真实数组中的前后哨兵使非法继承得到确定的错误姿态；期望矩阵由独立的旋转与静止位置补偿公式构造。

- 修复前：有界哨兵用例 4944 项，592 项失败。
- 修复前既有回归：根继承 374 项、独立数学上下文 2777 项均通过。
- 更极端的 `INT_MIN` / `INT_MAX` 作为修复后额外的越界用例。
- 修复后：索引专项 5736 项、根继承 374 项、独立数学上下文 2777 项全部通过；三个 CTest 目标均通过。

`stage_inheritance_load_tests` 使用真实 Windows 隐藏主窗口和 D3D 设备，连续载入指定工程三轮，每轮运行 12 帧并检查进程堆，随后重置和正常退出。结果为 exit 0；每轮均为 61 个模型、10 个附件。运行时明确记录 `model=0 bone=2 count=48 flags=0x11e source=-1 hasFlag=0 layer=0`，对应 phase B。该非法继承项仍保留在输入记录中，局部、世界及发布矩阵均为有限值。

这三轮的加载段分别为 2457.090、2162.823、2218.089 ms，仅用于描述本次诊断宿主运行，不是用户完整 MME 场景的启动性能或优化对比。

Windows x64 诊断构建使用 MSVC 19.44.35229、SDK 10.0.19041.0、`full`、DX12 关闭。内置效果预编译持续较久后，仅停止了本次生成器进程，在此构建目录设置 `MIKUMIKUSTUDIO_PRECOMPILE_EFFECTS=OFF` 完成验证；产品源码的默认选项仍为 ON。`MikuMikuStudio` 产品目标链接成功，诊断程序为 `build/stage-inherit/Release/MikuMikuDance.exe`。该程序未替换桌面稳定版。

没有安装真实 MME 到测试宿主，没有进行完整效果或视觉验收，也未完成默认预编译配置的正式发布包验证。原有 `scene_lifecycle_tests` 的迁移遗留编译错误已通过显式包含 `negative_frames.hpp` 和 `native_edit_history.hpp` 修复。随后完整配置的通用宿主通过三轮载入、日/英/中文切换、每轮 12 帧物理求值和堆校验，exit 0；每轮均为 61 个模型、10 个附件，`skipped=0`。输入为字节不变的 Stage 副本，结果见 `build/stage-inherit/stage-full-cycles.stdout.log` 与对应 `.exit`。

原版兼容配置在独立的 `build/stage-inherit-mmd932/` 中构建成功，索引专项 5736 项、根继承 374 项、独立数学上下文 2777 项均通过。真实工程载入仍未完成，不能计为该配置的 Stage 回归通过。另建的 `Stage-local-paths.pmm` 仅将 71 个固定宽度素材路径字段改为已存在的隔离素材路径；模型、骨骼和关键帧不变，替换清单见 `minimal-path-fixture.json`。使用原始路径或此副本的通用宿主都停留在载入步骤，已结束这些测试进程；专用宿主在 60 秒诊断上限内仍未完成载入，已自动终止并记录 `timeout=true` 到 `stage-minimal-host.result.json`。此现象的具体阻塞点尚未定位，不等同于已证实再次发生继承越界崩溃。

复核修复前保留的可执行文件，`--bounded` 仍为 4944 项中 592 项失败、exit 1，证明哨兵回归能捕获原缺陷。原工程 `Stage.pmm`、`Stage.emm`、`MME.emm` 均与输入备份哈希一致。

## 云端与本地分工

LOVE 在 `D:/MMD-Next/MMS-ABI-migrated` 处理 Stage 载入、骨骼继承及对应测试。云端只处理 PNG 验收工具的 `verify_png.py`、`selftest_verifier.py` 和 `README.md`；其独立安装目录是 `D:/MMD-Next/patches/png-acceptance`。本轮没有合并或修改这三个工具文件，未发现两项任务的路径冲突。

## 原版兼容配置的定向等待诊断（东八区 2026-11-15）

复测范围严格限制为现有 `stage_inheritance_load_tests.exe`、匹配的本地 PDB 和既有 `Stage-local-paths.pmm` 副本，未重编或重跑完整配置。只运行一次、请求一轮求值，没有切换或操作用户桌面。测试在 00:00:09.052 启动，00:00:34.377 到达诊断上限后自动结束；记录中的 exit 1 来自诊断脚本终止进程，不是应用自然退出或异常崩溃码。后续复查发现此启动器虽然创建了 `MMSStageWait_15900` 桌面对象，但给 Python `subprocess.STARTUPINFO` 增设的 `lpDesktop` 属性被启动器忽略；因此撤回该次子进程已在私有桌面运行的说法。线程栈定位仍是该次实际进程的证据。

在 00:00:14.072 和 00:00:19.198 采集到相同的主线程调用栈：

```text
NtUserWaitMessage
MessageBoxW
original_import::WarnModelImport  original_import.hpp:107
LoadPMX                         pmx_load.cpp:2092
LoadModelFile
project_model::LoadExact
LoadSceneV2_ModelBlock           pmm_load_v2.cpp:1280
TryLoadSceneV2
LoadSceneFile
wmain                           stage_inheritance_load_tests.cpp:42
```

两次主线程累计 CPU 时间均为 9,218,750 个 100ns 单位（0.921875 秒）。此处明确等待模态提示框消息；本次采样不支持将等待归因于资源 I/O、无限帧率或骨骼继承崩溃。

对应源码在模型数据准备完成后调用 `WarnModelImport`。它只在 `mmd932` 中显示“模型已导入，部分内容无法完整保存到原版 MMD 格式”的兼容提示，使用 `MB_OK | MB_ICONWARNING`，并保留已读取的数据。此次未读取提示正文，不能进一步断言触发的是名称、路径还是结构限制。专用 Stage 宿主只隐藏主窗口，没有处理该兼容提示，因而尚未从 `LoadSceneFile()` 返回，也未进入 12 帧求值。过去通用宿主超时没有抓栈，不据此把所有历史超时认定为同因。

具体缺口与下一步：给专用测试驱动添加仅作用于自身进程/线程的兼容提示识别与正文记录，只有精确匹配这类已导入提示时才确认；未知警告、导入拒绝和文件选择应失败并保留证据。随后只重建受影响的原版兼容测试宿主，在隔离环境限时验证 61 个模型、10 个附件、非法继承项及物理求值。不要为通过测试而屏蔽产品提示或改原工程。当前仍不声称原版兼容的完整 Stage 回归、真实 MME 或发布包验收通过；完整配置已有通过结论保留。

证据：`build/stage-inherit/minimal-wait-diagnostic/result.json`（两次线程栈、东八区时间、PID、EXE/输入哈希与超时结果）、同目录 `stdout.log`、`stderr.log`，以及 `build/stage-inherit/diagnose_minimal_wait.py`。EXE SHA-256 为 `ac9b8b639363ba3e854ce558bf9fdc00150e141691d9ad70491c9cdb9210d280`，复测前后保持一致；工程副本 SHA-256 为 `6dcca62accd31e6f619029b6ecca30c56a671c0818db313640340f26cf78bd26`，同样未变。

## 提示语义与外部测试驱动（东八区 2026-11-15）

`WarnModelImport` 是已读取模型的格式兼容信息通知。标题为 `MMD`，按钮由 `MB_OK` 指定为单个 IDOK；图标虽为 `MB_ICONWARNING`，点击确认的返回值未被使用，也没有格式转换、保存、权限或设置分支。后续继续原有载入流程。它和 `Refuse` 的导入拒绝提示、关节告警、文件选择器不同，不可泛化确认。

源码中的中文正文为以下固定首尾，按标志插入适用的限制项；本轮未成功采集实际提示正文，不能声称已知道该模型触发了哪组限制：

```text
模型已导入。以下内容无法完整保存到部分原版 MMD 格式：
－名称的字符或长度（VMD 的骨骼／表情名最多为 15 个 CP932 字节）
－模型或贴图引用路径
－材质、物理或显示项目

已保留读取到的数据。保存时会按所选格式检查，无法完整保存时会提示。
```

上面列出全部可能项，不是一条实际显示的完整消息；`namesFit=false` 时 `dataFit=true`，因此名称与结构项不会同时出现在该函数生成的提示中。

新增测试侧 `tests/stage_inheritance_runner.py`，固定 EXE/工程 SHA-256、复制独立安装和现有中文配置，禁用测试自动保存；仅枚举私有桌面中本次子进程的顶层对话框，核对标题、完整正文、同进程所有者和线程、单个启用 IDOK 按钮（文本为“确定”或“OK”）后才允许向该对话框发送 IDOK。未知正文、导入拒绝、多按钮、外部 PID 或状态变化均停止。纯匹配自测通过，包括这些拒绝分支。未修改产品源码或 CMake，也没有重新编译。

两次尝试必须区分：

- 00:07:30.992–00:09:01.389：初版外部驱动仍使用被忽略的 Python `lpDesktop`，枚举到了空的目标桌面，最终超时。确认提示数 0，日志只有 `cycle=0 load PMM`，实际物理推进 0；EXE 和全部复制素材的哈希无变化。不能计为有效的私有桌面回归。
- 随后修正为原生 `CreateProcessW` 的 `STARTUPINFOW.lpDesktop`。合成探针在 `MMSNativeWindowProbe_38688` 内实际枚举到自身 PID 34988 的窗口，证明桌面归属验证工作；不再依赖被 Windows 拒绝的跨进程 `GetThreadDesktop` 查询。
- 00:18:23：现有 Stage 测试 EXE 在这套真实私有桌面启动条件下返回 `0xC0000142`（`STATUS_DLL_INIT_FAILED`），没有进入 `wmain` 的载入日志。没有确认提示，也没有推进物理。具体失败 DLL 或启动条件尚未定位，不能把该错误归因于 Stage 内容或骨骼修复。此次应用进程已经退出，未替换或修改原 EXE。

结果与完整提示匹配规则分别保存在 `build/stage-inherit/minimal-notice-regression/`、`minimal-notice-regression-isolated/` 和测试驱动源码中。私有桌面合成证据见 `notice-driver-native-window.json`。最新原版兼容配置结论仍是 CPU 骨骼回归通过，真实 Stage 物理回归未完成。

下一步应优先使用进程内的测试专用对话框记录与精确确认，并在创建自身窗口时阻止显示和激活；这需要对测试宿主做小范围修改及只重建该目标，而不是改产品提示或重新构建全项目。当前遵守复用现有 EXE、避免额外编译的范围，未执行此路线。保留完整配置通过结论，真实 MME、日用程序和发布包均未参与。

原始日志、模型检查 JSON、前后源码和测试程序保存在独立构建目录。

验证后再次核对原始 `Stage.pmm` 与备份 SHA-256 一致。

另一个 `ToonGlass_Ray/UserFile/Stage.pmm` 曾被用户观察到未响应，随后用户确认最终打开且显示正常，只是载入慢。本项记录为独立的加载性能现象，不计作本次崩溃修复或性能优化结论。
