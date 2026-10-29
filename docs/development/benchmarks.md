# 多模型帧基准

`frame_benchmark` 是 `EXCLUDE_FROM_ALL` 的本机验证工具，使用实际 `FrameDriver`。默认测强制编辑器帧；`--playback-seconds N --json <文件>` 则执行实际播放、物理和 Present。两者都使用隐藏窗口，不能直接等同于交互窗口的 FPS。

`frame_benchmark_profile` 只给帧驱动及现有计时阶段编译 `MIKUMIKUSTUDIO_PERF`，将对应直接对象链接到测量宿主；普通核心库和正式 EXE 不增加计时。该目标沿用 MME 导出和程序名，输出到 `tests/mme-frame-profile/<配置>/MikuMikuDance.exe`。逐项计时包含嵌套子项，不能相加。正式帧率对照使用无计时的 `frame_benchmark_mme`。

`MMS_SHADOW_BOUNDS_CACHE_DISABLE=1` 可关闭新增的模型阴影边界缓存做对照。播放测量记录每帧绘制量、成功 Present、场景时间推进及实际视口；缓存本身只省去未改变顶点的边界扫描，不省略绘制或效果更新。

2026-09-24 用户更新约定：停止当前测试，后续采用不占用桌面的后台测评，以修复前后两个 build 的同条件结果验收。DirectX/GPU 计时、PresentMon/ETW 等采集方法需先确认在当前路径上有效；后台不能省略正常渲染工作量或把最小化节流算作修复收益。下面的窗口对照流程保留为历史方法，未经用户后续明确要求，不再启动。后台播放 FPS 的采集链路尚未验证完成；已有 GPU 排空吞吐率不改名为播放 FPS。

## 构建与运行

启用 `MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS`，显式构建 `frame_benchmark` 与 `frame_benchmark_mme`。后者输出到 `tests/mme-benchmark/Release/MikuMikuDance.exe`，以保留 MME 需要的程序名和导出。原生测试目录不得含 MME 的 `d3d9.dll`；MME 目录需用户现有的 `d3d9.dll`、`MMEffect.dll`、`MMHack.dll`、配置和 `Data` 资源。

以下命令在有桌面与硬件 D3D9 权限的终端执行，从可执行文件所在目录启动：

```powershell
.\frame_benchmark.exe D:\MMD-Next\tests\Grass\HGrass.pmx `
  --throughput --copies 4 --layout spread --viewport 1280x720 `
  --warmup 60 --samples 300 --json D:\MMD-Next\result.json
```

`--copies` 支持 1 至 8，分别装载独立模型；`--layout spread|overlap` 设置确定的平铺/重叠位置，自动取景。布局通过顶点抽样核验。该定位机制目前用于静态 Grass，不能据此断言所有物理角色的骨骼位移都正确。

正式版本对照脚本 `tools/run_alpha_benchmarks.py` 默认测 1/2/4/8 份、两种布局、原生/MME 默认/MME 素材关联效果三种配置，每个配置两轮 ABBA。`--baseline`、`--candidate` 是各自工作树；`--grass` 是原素材，`--grass-default` 是不含 `.emd/.fx` 自动关联的等内容副本；`--out` 必须使用新目录。正式构建要求 `/O2 /Ob2 /DNDEBUG`、PERF/DIAG 均关闭。

## 指标与失败

JSON `timing.drained_batch_ms_per_frame` 是整批包含最后 GPU 排空的耗时除以采样数，`drained_frames_per_second` 是其倒数。逐帧 `p95_ms` 是 CPU 提交与阻塞时间的 P95，不是 GPU 单帧耗时。`memory` 提供工作集与峰值提交信息。PERF=ON 的阶段数据只供定位瓶颈，不得混入正式对照；父子计时嵌套，不能相加。

JSON 同时记录模型数、顶点数、刚体数、GPU、相机、视口、模型实际取景与位移抽样。脚本核对两侧场景签名，并记录 Git 提交、二进制哈希、命令、环境和每次结果。Grass 没有物理刚体；保持物理调用并不意味着这组数据覆盖了角色物理成本。

manifest 中的 `commit` 是采样启动时的工作树 HEAD，不证明可执行文件由该提交构建。脚本另记录工作树状态、二进制修改时间和 SHA-256；应随结果保留构建日志，以哈希标识实际测量的二进制。

### MMV 阶段与有效性

`--require-mmv` 要求开始采样时已经加载 MMV，`--forbid-mmv` 要求未加载；两者互斥，仅用于完整 D3D9 渲染，不用于 `--load-only`、`--skin-only` 或 `--native`。对应加载条件不满足时分别返回 20、21。

JSON 的 `mmv` 对象记录 `loaded`、`timings_available`、`sample_count`，以及 `snapshot_mean_ms`、`evaluation_mean_ms`、`upload_mean_ms`。三项均为宿主侧 CPU 耗时总和除以采样帧数；对应 `*_samples` 只统计耗时非零的帧，不等于函数调用次数。它们不能解释成播放 FPS 或 GPU 时间，也不能与包含它们的总帧时间相加。本工具不打开 MMV GPU 绘制计时。

加载 MMV 时，每个采样帧必须产生新的、成功接受输出的宿主更新。快照拒绝、求解失败、输出校验或上传失败，以及跳过更新，都会增加 `failed_frames`，使 `updates_valid=false` 并返回 22。原始日志、图像与 JSON 仍保留，但该运行不能进入有效性能汇总。`accepted_frames` 记录通过检查的帧数；未加载 MMV 时两种更新计数均为 0。

汇总同时保存同版本样本的极差占中位数比例，以及每轮 ABBA 的变化比例。极差超过 5% 时标记 `unstable`。未触发此标记也不代表微小差异具有统计显著性。

设备不可用、GPU 查询失败、视口/位置验证失败、模型未装齐、渲染阶段出现错误对话框均使运行失败。退出码 18 表示渲染期弹窗，日志包含窗口文字。消息循环处理隐藏窗口消息，但不额外派发会再次渲染的 `WM_PAINT`。超时保留日志与 manifest，不自动重试，也不把失败样本算作有效测量。

程序名和 MME DLL 加载成功只能证明代理已载入；效果关联还需核对素材 `.emd` 和实际画面。涉及 `SoftParticleModel.fx` 时，Grass 自带的软粒子引擎 accessory 并未随 `--copies` 装载；本场景只测试模型效果文件的对应配置。

## 后台前后 build 对照

`tools/run_alpha_background_compare.py` 是不占用桌面的前后 build 对照脚本。它分别从两个工作树取出独立的 `frame_benchmark.exe`，以隐藏窗口运行原生 D3D9 渲染，并把结果写成可复核的原始记录。脚本不代替用户构建，也不推断某个 EXE 对应哪个提交。

与窗口对照不同，本脚本的指标是整批包含最后 GPU 排空的耗时除以采样数，即 `timing.drained_batch_ms_per_frame`，单位 ms/frame。它不是播放 FPS，也不是 GPU 单帧时间；不要把这个数值换算或改名为 FPS。

必填参数五项：

- `--before-tree` 修复前工作树。
- `--after-tree` 修复后工作树。
- `--output` 输出目录，必须不存在，脚本用 `mkdir(exist_ok=False)` 新建。
- `--grass` 与 `--character` 两份素材路径。

可选参数五项，默认值取自脚本：

- `--warmup` 预热帧数，默认 30。
- `--samples` 采样帧数，默认 120。
- `--cycles` 每组用例的 ABBA 轮数，默认 2。
- `--cases` 逗号分隔的用例名列表，默认跑全部用例。
- `--timeout` 单次子进程超时秒数，默认 180，必须为正整数。

`--cases` 的合法名称与脚本内置定义一致：`grass-1-spread`、`grass-2-spread`、`grass-4-spread`、`grass-8-spread`、`grass-4-overlap`、`face-static`、`face-moving`。名称不存在时报错并列出已知名称；列表为空时只报要求非空列表，不列名称。非法输入不会静默跳过。`--timeout` 只约束单次运行，超时后脚本停止该子进程、把 `timeout; subprocess stopped` 写进 manifest 并抛出异常，不自动重试。

脚本固定视口为 1280×720，并按 ABBA 顺序交替执行：每个用例的每一轮都按 before、after、after、before 运行，因此同轮内两种 build 的先后位置对称。每组结果同时给出 before/after 各自的中位数、最小值、最大值和全部样本值，以及 `change_percent`。

原始记录全部落在输出目录，保留失败样本：

- `manifest.json` 记录创建时间、指标说明、协议、预热与采样参数、超时、用例清单、脚本自身 SHA-256、`OMP_*` 与 `MIKUMIKUSTUDIO_*` 环境变量，以及每个 run 的命令、日志、JSON、退出码和 ms 结果。
- 每个 build 记录工作树路径、HEAD 提交、整树状态、`src`/`include`/`tests`/`CMakeLists.txt` 的状态、`CMakeCache.txt` 的全文与 SHA-256，以及运行时目录下每个文件的 SHA-256。
- 两份素材目录逐文件记录 SHA-256，两侧使用逐字节相同的素材与 `Data` 拷贝。
- 运行日志与逐次 JSON 按 `用例-轮次-序号-标签` 命名保留。
- `summary.json` 保存汇总。

前置检查失败即中止，不做降级：任一工作树在构建输入路径上有未提交改动时抛出 `uncommitted build inputs`；原生基准目录若出现 `d3d9.dll`、`MMEffect.dll`、`MMHack.dll` 中任意一个也中止。子进程退出码非零时抛出错误并保留原始输出，不把失败样本当成有效测量。

用例中有两组可控近景，模型来自 `--character`：

- `face-static` 传入 `--camera 0,18.5,0,-5,0,0,0`，把相机固定在面部近景，yaw 为 0。
- `face-moving` 传入 `--camera 0,18.5,0,-5,-6,0,0` 并追加 `--orbit-degrees 0.08`，初始 yaw 为 -6 度，之后每帧绕轨 0.08 度。

`--camera` 的七个数值依次为位置 x、y、z、距离、yaw、pitch、roll，后三项单位为度；`--orbit-degrees` 是每帧累加到 yaw 的度数。前后两侧必须使用相同取值，否则测的不是同一场景。

原生阴影近景对照还支持以下参数：

- `--self-shadow 0|1|2`：关闭阴影，或选择模式 1、模式 2。
- `--fine-shadow 0|1`：关闭或开启美影。
- `--shadow-range 9204`：使用界面中的阴影范围值，允许 0 至 9999；内部距离为 `(10000 - 值) / 100000`。
- `--light "-.5,-.01,.5"`：设置光照方向 x、y、z；三个值必须有限，向量不能全为零。

JSON 的 `self_shadow` 同时记录实际模式、内部距离、请求范围、光照方向与阴影贴图预览开关。指定 `--viewport` 时，工具同步宿主的视口矩形和 D3D 视口，避免隐藏窗口的侧栏边框涂黑测试画面。模型、描边、物理和透明排序仍经过同一 `FrameDriver`。

```powershell
python tools/run_alpha_background_compare.py `
  --before-tree D:\MMD-Next\before `
  --after-tree D:\MMD-Next\after `
  --output D:\MMD-Next\.alpha-perf-artifacts\background-compare-01 `
  --grass D:\MMD-Next\tests\Grass\HGrass.pmx `
  --character D:\MMD-Next\tests\Character\Character.pmx `
  --cases grass-1-spread,face-static,face-moving `
  --timeout 240
```

上面的路径只是占位示例，实际取值以本机目录和本次对照的两份构建为准。运行前各自构建 Release 并记录生产源码版本、构建日志与 EXE 哈希；脚本自身不构建。

## 历史方法：原版窗口对照（默认不运行）

`--save-scene <new.pmm>` 在采样与布局核验之后导出静态场景。它要求 `--throughput`、明确的 `--copies/--layout`、无刚体模型及未占用的输出路径。根骨骼位移和相机写入第 0 帧关键帧，子骨骼继承根位移。导出失败返回 19。导出器检查文件头；完整可读性由原版实际载入和模型数量、姿态核验确认。

`tools/reference_gui_benchmark.py` 启动一个正常窗口，装载 PMM、校准 1280×720 视口、取消帧率限制，再通过播放按钮开始采样。测试窗口临时置顶但不获取键盘焦点；截图前及 FPS 采样时检查视口中心与四角是否仍属于该窗口，发现遮挡则保留错误记录并退出。每次运行结束或出错时关闭自己启动的程序。它只读取该子进程内存，不写入进程数据。原版布局绑定到已核验的 9.32 EXE 哈希，移植版偏移对应 2026-09-24 的 `MMDAppState`；布局改变后必须重新核验偏移。

`tools/run_reference_gui_benchmarks.py` 按 ABBA 顺序对照两份可执行文件，核对相机、各骨骼矩阵及静态姿态，保存每次运行的 JSON、日志、视口截图和文件哈希。当前配置使用 `--counter-only`，每次读取 12 秒原生 FPS 计数，未取得 ETW 逐帧数据。它与 60 帧预热、300 帧采样的独立基准分开统计。测量结果见 [Grass 帧率对照记录](../records/performance/benchmarks.md#record-grass-performance)。

基线参数二选一：`--original <MMD9.32.exe>` 使用原版内存布局；`--baseline-port <before.exe>` 使用本项目布局，与 `--candidate <after.exe>` 比较修复前后的正常程序。两者不能同时传入。默认覆盖 1/2/4/8 份模型及平铺、重叠布局，每组一次 ABBA，每次采样 12 秒。`--seconds` 可调整采样时长，采样约每 1.05 秒进行一次。

```powershell
python tools/run_reference_gui_benchmarks.py `
  --baseline-port D:\MMD-Next\.alpha-perf-artifacts\build-compare-final\before-native\MikuMikuDance.exe `
  --candidate D:\MMD-Next\.alpha-perf-artifacts\build-compare-final\after-native\MikuMikuDance.exe `
  --scenes D:\MMD-Next\.alpha-perf-artifacts\reference-compare\scenes `
  --out D:\MMD-Next\.alpha-perf-artifacts\build-compare-repeat
```

运行前构建两份 Release，并记录生产源码版本、构建日志、EXE 哈希和依赖文件。该脚本不会替用户构建，也不会推断某个 EXE 对应哪个提交。输出中的 `baseline`、`candidate` 分别记录 A、B 的路径、模式和帧率，不能把移植版基线标为原版 MMD。

## 透明排序开关与美影验证

`--fine-shadow 0|1` 设置美影复选框和实际阴影尺寸（2048 或 4096），不传时保留启动默认值。`--verify-view-toggles` 用于包含透明材质的原生 D3D9 模型场景，检查默认关闭、三语菜单、重复刷新、开关命令以及 GPU 索引开关恢复。该选项在检查启动默认值后显式开启标准档，再验证排序开关，不用于 `--load-only`、`--skin-only` 或 `--native`。

JSON 新增 `view_options`，记录启动美影状态、启动和实际阴影尺寸、排序菜单初始状态以及功能验证是否通过。前后 build 的画质条件和实测记录见 [透明排序开关验收](../records/rendering/transparency.md#record-auto-transparent-sorting)。
