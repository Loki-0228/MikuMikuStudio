# CPU、线程等待与查找

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-cpu-wait-fix"></a>
<a id="record-cpu-wait-fix-修复方式"></a>
<a id="record-cpu-wait-fix-两侧版本和构建"></a>
<a id="record-cpu-wait-fix-采样条件和口径"></a>
<a id="record-cpu-wait-fix-进程-cpu-结果"></a>
<a id="record-cpu-wait-fix-正确性和吞吐检查"></a>
<a id="record-cpu-wait-fix-部署归档和排除记录"></a>
<a id="record-cpu-usage-moning"></a>
<a id="record-cpu-usage-moning-进程-cpu-的测量口径"></a>
<a id="record-cpu-usage-moning-同条件原生对照"></a>
<a id="record-cpu-usage-moning-工作线程为什么持续占用-cpu"></a>
<a id="record-cpu-usage-moning-当前发布包与插件补查"></a>
<a id="record-cpu-usage-moning-版本与原始证据"></a>
<a id="record-mmdmodern-port"></a>
<a id="record-mmdmodern-port-移植范围"></a>
<a id="record-mmdmodern-port-构建来源"></a>
<a id="record-mmdmodern-port-播放测量方法"></a>
<a id="record-mmdmodern-port-正式程序载入工程的同步实测"></a>
<a id="record-mmdmodern-port-实测结果"></a>
<a id="record-mmdmodern-port-指标与证据范围"></a>
<a id="record-mmdmodern-port-先行的-2-正式程序采样"></a>
<a id="record-mmdmodern-port-独立基准的同时运行对照"></a>
<a id="record-mmdmodern-port-串行帧率记录"></a>
<a id="record-mmdmodern-port-全部样本"></a>
<a id="record-mmdmodern-port-结论范围"></a>
<a id="record-mmdmodern-port-资源限制与遥测"></a>
<a id="record-mmdmodern-port-正确性与固定帧对照"></a>
<a id="record-mmdmodern-port-gpu-排空后的批次帧耗时"></a>
<a id="record-mmdmodern-port-无效与保留记录"></a>
<a id="record-mmdmodern-port-正式发布入口"></a>
<a id="record-mmdmodern-port-复现入口"></a>
<a id="record-performance-d3dx"></a>
<a id="record-performance-d3dx-改动与原因"></a>
<a id="record-performance-d3dx-前后对照"></a>
<a id="record-performance-d3dx-正确性验证"></a>
<a id="record-performance-d3dx-复现"></a>
<a id="record-performance-mmd-pmd"></a>
<a id="record-performance-mmd-pmd-参考证据与实现"></a>
<a id="record-performance-mmd-pmd-测试方式"></a>
<a id="record-performance-mmd-pmd-修复测试弹窗"></a>
<a id="record-performance-mmd-pmd-复现"></a>
<a id="record-performance-nex-adaptation"></a>
<a id="record-performance-nex-adaptation-实现"></a>
<a id="record-performance-nex-adaptation-结果"></a>
<a id="record-performance-nex-adaptation-验证与边界"></a>

## 工作线程等待

模型几何工作线程在没有任务时使用受控等待，任务发布、结束和销毁需要配对唤醒。短等待策略与较长空闲等待分开，避免暂停编辑时持续占满逻辑核心，也避免关闭时线程无法退出。

设备丢失、录制确认等同步点有独立等待语义。替换等待方式时保持原有渲染、物理及消息进度，不通过跳帧或少画降低 CPU 占用。

## 查找与缓存

按物体、材质和纹理的查找使用稳定身份及失效规则，减少重复线性查询和模块解析。D3DX 模块函数解析可复用有效结果，释放或变化时不能继续持有无效函数指针。

PMD 并行蒙皮与 PMX 准备遵守任务范围和原数值语义。场景不同、模型大小及物理状态会改变收益，不能从某一素材的查找比例推算通用加速。

## CPU 占用口径

区分单核心百分比与按整机逻辑处理器归一化的进程占用。低 CPU 不自动代表更快，可能只是等待 GPU；高吞吐也不代表后台没有空转。应同时检查耗时、场景推进、成功 Present、绘制量与线程状态。

## 性能对照

使用独立的前后构建、相同素材及设备，按 ABBA 交替采样。限核、CPU 配额、预热、供电状态与采样长度须固定；吞吐率和实际播放 FPS 分别统计。其他编译或渲染任务的干扰应记录并按预设规则排除。

具体工具见[帧基准](../../development/benchmarks.md)及 [tests/README.md](../../../tests/README.md)，不从旧硬件数据推断当前版本必然提速。
