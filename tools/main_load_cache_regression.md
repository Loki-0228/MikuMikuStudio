# main / ABI：普通工程载入与缓存回归

这套工具只编排输入、记录缓存文件系统证据和比对结果。它不会启动、关闭或注入 MMS，不会清理任何目录，也不会保存工程。Linux 上的合成测试可直接执行。真实 Windows 结果需要一份两边共用的、已验证的采集适配器和两套精确构建；目前没有随工具交付这样的适配器或真实应用成绩。

## 对照点和范围

- main：`2381bafc40ed864cf15f39278f72bada600d96a7`。这是交接包中的基准；没有自动读取远端最新 main。
- ABI：`bc2b7a795d17a942e5c1ed95c3878f8ba0e5c28f` 加云端增量。指定构建输入清单、可重放补丁包或其他已验证源快照的 SHA-256，并保留哈希的出处。
- 同为 Windows x64 完整档。EXE SHA-256 必须来自独立的构建记录或发布清单。工具会重新读文件、检查 PE32+ 头和哈希，但不会仅凭 EXE 声称源码提交来源已被证明。运行适配器应在启动前核验实际副本和同分发包依赖。
- 一次运行只测一个安全的小型 PMM 夹具、普通工程打开、PMD/PMX 输出、纹理准备和测试效果实际接受或拒绝。普通导入使用的 `.mtc` 与 ABI 事务导入的共享 `.mtc` 不是同一验收场景。这里不测事务发布、缓存管理界面、冻结保存、Stage、真实 MME 工程、播放／超帧率、刚体和渲染吞吐。打开工程时产品内部必然完成的初始化不另计为上述专项通过。
- ABI 已有 `frame_benchmark --cache-reopen`，会单独检查其进程内重开及纹理缓存统计，范围不含 MME/MMV。这里不拿它与没有等价入口的 main 做对比。若无法在两个版本以同一边界观测，记录缺口。

## 本地验证

在仓库根目录，用 Python 3.10 或更高版本：

```sh
python -B tools/test_main_load_cache_regression.py
```

测试用临时空间，生成假 EXE、夹具、符合文件头形状的假缓存和假计时。它们仅验报告、配对、隔离防护和差异检测，不含真实 PMM 解析、纹理命中、效果编译或产品性能。

## 准备真实采集

使用隔离的 Windows 账户或云虚拟机，并让两套应用各自从专用的运行目录启动，不使用稳定版安装或任何真实工程。夹具只放自建或获准的文件。将 PMM 内的模型／附件路径、效果 include 和纹理引用核实为隔离目录内的拷贝；单凭文件清单无法判断文件是否内含外部引用。文件名用通用 ASCII，例如 `scene.pmm`、`models/grid.pmx`，不要带机器名或用户名。

```text
python tools/main_load_cache_regression.py prepare
  --output-parent EXISTING_DIRECTORY_OUTSIDE_THE_FIXTURE
  --fixture-dir INPUT_FIXTURE
  --expectations EXPECTED_JSON
  --main-exe MAIN_EXE --main-sha256 INDEPENDENT_MAIN_EXE_SHA256
  --abi-exe ABI_EXE --abi-sha256 INDEPENDENT_ABI_EXE_SHA256
  --abi-source-sha256 ABI_BUILD_INPUT_SHA256 --pairs 5
```

命令参数接在一行，或使用当前 shell 的续行符。命令打印唯一运行目录。两边共用这个目录的 `fixture`，工具设定普通文件只读并在每个节点复核内容和相对路径（不改原夹具）。Windows 目录 ACL 或虚拟机挂载仍需禁写，防止程序重命名／删除夹具，也不要让其他进程写实验缓存。工具拒绝符号链接、重解析点、夹具／缓存硬链接、Windows 保留名和大小写冲突。

`expectations.json` 是独立核实过的验收结果，下面是结构示例，哈希不能直接照抄：

```json
{
  "pmm": {
    "fixture": "scene.pmm",
    "version": "0002",
    "result": "opened",
    "active_model_count": 1
  },
  "models": [
    {
      "fixture": "models/grid.pmx",
      "vertices": 6,
      "indices": 6,
      "materials": 2,
      "semantic_sha256": "<64 lowercase hex digits>"
    }
  ],
  "effects": {
    "fx/valid.fx": "accepted",
    "fx/invalid.fx": "rejected"
  }
}
```

PMM 的 `result` 是调用实际结果；`active_model_count` 和有序 `models` 是调用结束后的活动工程状态。真实模式仅验合法形状的 PMM 文件头与声明版本对应，可包括明确预期拒绝的版本；短头或坏 magic 是其他测试。模型摘要必须来自两边相同且有版本的实际输出序列化，例如稳定顺序的顶点位置、法线、UV、索引和材质面分组，不包含绝对路径、内存地址或时钟。不要以输入文件哈希充当这一摘要。效果结果是效果实际被应用接受或拒绝；「找到文件」「编译调用完成」或写出了 `.fxb` 都不能替代它。拿不到实际语义观测时，这一端到端结果仍为待验；不要构造占位通过。

## 三段重复和适配器约定

用 `python tools/main_load_cache_regression.py paths RUN_DIR main 1`（或 `abi` / 其他 pair）获得固定的独立运行目录和环境变量。将各应用分发包拷贝到该 pair 的 `runtime_dir`，将映射显式传给该应用进程，去掉任何继承的禁用／内存专用缓存设置，并核对两版的磁盘策略和实际写入位置。已核对两边都有 `MMS_CACHE_MANAGER_DIR`、`MMS_EFFECT_CACHE_DIR`、`MMS_TEXTURE_CACHE_DIR`、`MMS_CACHE_MANAGER_SETTINGS` 和 `MMS_RECENT_PROJECTS_FILE`。这不承诺这些变量能隔离所有应用或插件写入；不要跳过独立账户／虚拟机。原始日志可能包含本机路径，只留作私下核证；对比报告只包含摘要和通用相对路径。

每个版本的每个 pair 按以下顺序采集。每个阶段都要在隔离观察点执行 `checkpoint RUN_DIR VARIANT PAIR PHASE before` 和 `checkpoint RUN_DIR VARIANT PAIR PHASE after`，参数前接脚本命令：

1. **`app-cache-cold`**：磁盘应用缓存目录中没有文件。记 `before` 后启动新进程，完成加载、退出并等后台缓存写入停稳，才记 `after`。
2. **`disk-repeat-new-process`**：应用未运行时记 `before`，核实与上次 `after` 的磁盘快照一致。用不同进程加载同一夹具，等写入停稳，记 `after`，保持这个进程存活。若两边并没有建立相应的缓存候选文件，则这一阶段的相应计时不纳入磁盘条件比较。
3. **`repeat-same-process`**：保持第二段的进程，只恢复到相同的打开前工程条件，不显式清缓存；记 `before`、重复加载、待写停稳记 `after`。记录末次快照后，不再运行会改写这些缓存的动作，直接出报告；可在最后一次快照之后保留进程到报告完成。

冷只指 MMS 此次实验没有已有应用缓存。工具会读文件计算哈希，既不清操作系统页缓存，也不提供物理磁盘冷启动证明。同进程不等于观察到了内存命中。有效文件名、格式头、长度、键摘要字段与文件名一致，仅用于识别持久化候选；不是验证缓存内容合法或被成功使用。若要判断缓存命中，需单独取得可复验的来源统计和应用结果。

使用同一 Windows 主机、驱动、配置和适配器，串行而不是并发跑同一对。交错两侧先后次序，避免作业负载变化给一边造成偏差。阶段计时应由等价的进程内单调时钟观测；`total_ready` 从实际开始打开到应用完成同一种就绪条件，不包括进程启动、注入、等日志、文件哈希、缓存停稳或关进程。不把重叠的阶段相加。若应用/工具版本不提供一个共同的阶段计时边界，留 `null`。现有 PMM 诊断是选编开关，日志不提供完整阶段计时。效果缓存诊断也不能代替效果接受信号。

## 采集文件和报告

适配器输出单一 JSON，顶层 `schema: 1`、`run_id`、`mode` 和 `fixture_sha256` 从 `plan.json` 复制。另有：

- `capture`：`host_label` 为通用环境标签（例如 `win11-26100-d3d9`，不要用设备名）；`adapter_sha256` 对应两边共用的采集适配器产物；`timing_clock` 为 `monotonic_in_process`，没有等价时钟时为 `unavailable`。
- `records` 每条严格含 `variant`（`main` / `abi`）、`pair`（从 1 开始）、`phase`（上述三项）、`exe_sha256`、`process_session`、`elapsed_ns`、`outcome`。每次实际进程启动分配唯一 32 位小写十六进制标识，绑定本地进程号与启动时间；第二和第三段标识相同，跨 pair 或版本不能复用。产品进程身份由采集端观察；离线工具只检查标识关系。
- `elapsed_ns` 的字段是 `pmm_open`、`model_prepare`、`texture_prepare`、`effect_accept` 和 `total_ready`，值是正整数纳秒或 `null`。时钟不可用则全部 `null`。`outcome` 同验收结果结构，填实测值。

```text
python tools/main_load_cache_regression.py compare RUN_DIR
  --main-exe MAIN_EXE --abi-exe ABI_EXE --observations OBSERVATIONS_JSON
```

每次生成新的 `report-*` 文件夹，含 `report.json`（输入哈希、缓存快照、语义字段差异、每阶段可配对的样本数和中位数）及 `observations.csv`（逐次记录）。阶段数据缺失为 JSON `null`／CSV 空格位。磁盘段对纹理和效果分别要求两边存在对应格式的持久化候选；命中不由文件存在推断。每阶段报告 ABI 相对 main 的成对差值，中位数小于零仅代表这些样本观测到 ABI 耗时更少；没有自动性能通过判定。

退出码：`0` 观察到的语义符合验收输入；`1` 有可以定位的语义差异（已出报告）；`2` 证据无效或不全。缺计时不使语义结果失败。缓存不干净、快照变化或进程关系不符时保留现场，新建一组实验目录；脚本不会删改已有数据，也不会替代缺失的 Windows 验收。
