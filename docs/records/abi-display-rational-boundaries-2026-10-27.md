# 离散采样的精确帧边界与真实 IK 轨道验收

## 修复内容

显示状态、IK 开关和外部父级选择按整数帧键阶跃变化。旧实现先将有理数秒转换为 `double`，极接近键前的时间可能被舍入到键上，提前切换状态。例如 `{9007199254740992, 270215977642229761}` 秒严格小于第 1 帧，旧结果却为 1。`{INT64_MAX, UINT64_MAX}` 秒对应帧数严格小于 15，旧转换也可能得到 15。

`evaluation_store.cpp::DisplayFrame` 先复用精确有理数上限检查，再用整数商、余数和 30 次安全进位计算 `floor(30*n/d)`。余数累加通过 `denominator - carried` 比较避免溢出；最终整数不超过 2147483646，转为 `double` 时可精确表示。三个离散入口统一使用该函数，父级窗口仍为含起点、不含终点的整数区间。超上限时间在取整前拒绝，失败不调用捕获数据的采样回调或修改输出。

骨骼和表情仍使用原 `FrameTime` 连续路径。公共函数表、宿主插件版本、VMD 文件与插值协议不变。本批不改变原生时间轴及播放求值。完整 IK 求解、层级、世界姿态和物理仍待实现，不能将本批的 IK 开关读取称为 IK 求解。

## 验证

证据目录为 `build/verification/abi-display-rational-boundaries-20261027/`。`build-baseline-01` 在未修改生产代码时编译新增测试，`exact-baseline-01` 真实返回 1，失败断言为首个时间点的显示及 IK 字节；日志保留。`rational-oracle.json` 使用 Python 整数分数独立核对精确值与原浮点值。

| 检查 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| 精确离散时间专项 | 182 项通过，`exact-full-01` | 182 项通过，`exact-original-01` |
| 全部显示求值测试（包含上述专项） | 2,042 项通过，`display-full-01` | 2,042 项通过，`display-original-01` |
| 原存储测试 | 4,036 项通过，`store-full-01` | 4,036 项通过，`store-original-01` |
| 骨骼与表情组合测试 | 1,706 项通过，`channels-full-01` | 1,706 项通过，`channels-original-01` |
| 真实 DLL 生命周期 | 496 项通过，`lifecycle-full-01` | 496 项通过，`lifecycle-original-01` |
| 产品构建 | `build-full-01` 通过 | `build-original-01` 通过 |

25 组合法时间和 7 组非法时间覆盖整数键前后、等价分数、十亿帧、最大帧及 64 位极值。期望值由独立整数恒等式给出，不以浮点转换作为答案。测试还断言骨骼与表情回调继续收到 7.5 帧，而非离散取整后的 7。

生产宿主通过实际 Manager 加载求值 DLL。新增 `--evaluation-display-ik-only` 强制模型包含真实非空 IK 链及局部父级选择器，避免空集合通过。原创 PMX 由现有 `tests/plugins/run_native_registration_probe.py::pmx()` 生成，链来自文件解析而非加载后替换，哈希及生成入口见 `fixture.json`。

`host-ik-full-01` 通过。用例逐链核对公开骨骼身份，采样第 0、4、8 帧及极接近第 4 帧前后的有理时间，检查 197、255 原始字节、全局和局部父级身份与有效窗口。删除源和目标、复用槽位、重载实际 PMM 后，后台仍能读取旧源。采样不改变两份模型、播放头和编辑标志。这里的 PMM 重载验证快照寿命，不代替完整文件往返测试。

`host-bone-full-01`、`host-morph-full-01`、`host-display-full-01` 也通过，覆盖既有三条生产入口。原始兼容产品仍不开放生产求值工厂；其测试使用注入捕获器。本批没有重跑前批全量 `plugin_abi_tests`，未把旧结果作为本批执行记录。Win7 与 AMD 真机验收仍待进行。

## 审查与范围

DSH 会话 `6dde0ab9-2859-4ed2-9528-19566b98c4fa` 的实际 cwd 为 `D:\MMD-Next\MMS-abi-dev`，仅修改 `src/plugins/evaluation_store.cpp`。正式结果已读取，completed，最终游标 30。`sawError=true` 对应技能查询失败 `CodeRunFailedError/CODE_RUN_FAILED`；未导致代码任务中断，未作为成功证据。DSH 未编译、测试、提交或处理文档。

两个 Codex 子任务分别补充精确边界测试与真实 IK 宿主测试，另一子任务独立审查算法及覆盖范围，未发现阻断问题。构建与执行由主任务验收。

main 仍为 `d551f1f3f12759870c5f54d7f8e9cd05f935943d`，没有新增提交；阴影相关同步暂缓，未复制 main 未提交内容。正式 release 未覆盖，SHA256 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，开发产物见 `product-check.json`。

## 后续完整求值

下一步需要冻结骨架定义及嵌套 IK 数据，供独立求值实例持有。当前 `physics_bake::ModelCopy` 仍借用 IK links、动作键和表情载荷，不可直接延长到后台任务。骨架捕获须保留原始骨骼顺序、类型、继承与轴限制、PMX 原链和运行链的各自数据，以及 selector 映射；当前姿态、选择和窗口状态由对应快照承担。

依赖校验须区分语义：`CoRotate::tailIdx` 是百分比，源骨骼为 `tailBone`；继承骨骼才用 `tailIdx` 作索引。`extParent` 是外亲键，不是模型槽号。可以拒绝父子层级环，不能将所有 IK 反馈与继承边合并后统一要求无环。此处记录后续实现约束，不表示骨架冻结或完整姿态已实现。
