# 性能测量口径

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mme-926-performance"></a>
<a id="record-mme-926-performance-修改"></a>
<a id="record-mme-926-performance-定位证据"></a>
<a id="record-mme-926-performance-本体帧率对照"></a>
<a id="record-mme-926-performance-正确性与限制"></a>
<a id="record-mme-926-performance-构建与本地产物"></a>
<a id="record-mme-frame-performance"></a>
<a id="record-mme-frame-performance-controlled-comparison"></a>
<a id="record-mme-frame-performance-what-the-original-mmd-path-shows"></a>
<a id="record-mme-frame-performance-changes-and-why-they-help"></a>
<a id="record-mme-frame-performance-validation-and-limits"></a>
<a id="record-stage-frame-performance"></a>
<a id="record-stage-frame-performance-workload-and-method"></a>
<a id="record-stage-frame-performance-change"></a>
<a id="record-stage-frame-performance-validation"></a>
<a id="record-stage-frame-performance-remaining-measured-work"></a>
<a id="record-stage-frame-performance-reproduction"></a>
<a id="record-pmm-frame-performance"></a>
<a id="record-pmm-frame-performance-controlled-comparison"></a>
<a id="record-pmm-frame-performance-bottleneck-and-changes"></a>
<a id="record-pmm-frame-performance-validation-and-reproduction"></a>
<a id="record-grass-performance"></a>
<a id="record-grass-performance-修复前后-release-build-对照"></a>
<a id="record-grass-performance-原版-mmd-932-对照"></a>
<a id="record-grass-performance-修复前后与优化实验"></a>
<a id="record-grass-performance-mme-兼容性对照"></a>
<a id="record-performance-phases"></a>
<a id="record-performance-phases-方法与构建核查"></a>
<a id="record-performance-phases-结果"></a>
<a id="record-performance-phases-后续修复顺序"></a>
<a id="record-performance-phases-复现与验证"></a>
<a id="record-performance"></a>
<a id="record-performance-method"></a>
<a id="record-performance-results"></a>
<a id="record-performance-limits-and-next-investigation"></a>
<a id="record-performance-reproduction"></a>
<a id="record-reverse-performance"></a>
<a id="record-reverse-performance-样本与方法"></a>
<a id="record-reverse-performance-1-mmdpmd-也经过并行蒙皮而当前实现遗漏了这一点"></a>
<a id="record-reverse-performance-2-mmd观察到的材质绘制路径不含当前新增的三角形重排"></a>
<a id="record-reverse-performance-3-nexgima可核实的-gpu-蒙皮数据流"></a>
<a id="record-reverse-performance-4-nexgima描边在顶点着色器扩张"></a>
<a id="record-reverse-performance-5-nexgima提交与等待分开并支持批量-command-list"></a>
<a id="record-reverse-performance-应用顺序与-mme-边界"></a>
<a id="record-reverse-performance-api-核对与复现"></a>

## 指标

| 指标 | 含义与边界 |
| --- | --- |
| 实际播放 FPS | 正常推进动画并成功显示的帧率；须同时记录限帧与 Present |
| GPU 排空吞吐率 | 整批提交并最终排空后的平均吞吐，不能改称播放 FPS |
| CPU 提交耗时 | 包含提交与部分阻塞，不是 GPU 单帧耗时 |
| 顶点阶段 | 打包、上传、计算、回读与缓冲写入的范围，以实际计时点为准 |
| GPU 分段 | 当前路径上有效的 GPU 时间戳范围，不和嵌套父项相加 |
| 载入耗时 | 区分窗口出现、资源准备、首次可用帧、冷缓存与重开 |

## 对照条件

保存源码身份、构建日志、EXE 与插件哈希、设备、驱动、素材、相机、物理、特效、视口、预热和采样范围。工作树 HEAD 不能单独证明 EXE 由该提交构建。

前后版本串行交替采样，常用 ABBA；不同时运行两个渲染器争抢资源。后台可隐藏窗口，但保持正常绘制和场景推进，不以最小化节流充当收益。诊断阶段计时与关闭诊断的正式对照分别保存。

## 有效样本

检查模型数、姿态、相机、绘制量、成功 Present、动画时间及输出。加载错误、设备丢失、效果更新失败、超时、弹窗或干扰样本保留原始记录并明确排除，不把失败换算成零耗时。

MMV 每采样帧需要新的有效输出，不能反复展示上一帧来计算速度。像素对照需控制真实时间相关效果，正确性与性能是独立结论。

## 工具

[帧基准说明](../../development/benchmarks.md)提供构建、参数与后台前后对照用法。[测试说明](../../../tests/README.md)按用途列出运行器。原版窗口工具绑定特定二进制与布局，不能直接复用于任意版本。

## 原始数据

下面是固定样本的原始结果，保留其程序和测量条件，不代表当前 main 或任意设备的性能：

- [通用场景](data/performance.json)、[阶段计时](data/performance-phases.json)
- [D3DX 查询](data/performance-d3dx.json)、[PMD](data/performance-pmd.json)、[排序](data/performance-sort.json)
- [MME 帧](data/mme-frame-performance.json)、[MME 预览](data/mme-preview-performance.json)
- [模型顶点缓存](data/model-vertex-cache.json)、[PMM 帧](data/pmm-frame-performance.json)
- [舞台](data/stage-frame-performance.json)、[无 ray 舞台](data/stage-no-ray-performance.json)、[GPU 分段](data/stage-no-ray-gpu-timing.json)
- [原版性能证据](data/reverse-performance-evidence.txt)

新结论应来自对应构建的重新测量，不能只刷新文档日期或沿用旧百分比。
