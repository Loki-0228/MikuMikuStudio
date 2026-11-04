# 插件性能与有效性

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mmv-frame-cost"></a>
<a id="record-mmv-frame-cost-子任务状态"></a>
<a id="record-mmv-frame-cost-修复与实现"></a>
<a id="record-mmv-frame-cost-对照方法"></a>
<a id="record-mmv-frame-cost-结果"></a>
<a id="record-mmv-frame-cost-正确性与构建"></a>
<a id="record-mmv-frame-cost-产物与限制"></a>
<a id="record-mmv-performance-revalidation"></a>
<a id="record-mmv-performance-revalidation-固定版本与条件"></a>
<a id="record-mmv-performance-revalidation-八次交替播放"></a>
<a id="record-mmv-performance-revalidation-排除的历史基线"></a>
<a id="record-mmv-performance-revalidation-证据"></a>
<a id="record-mmv-snapshot-reuse"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-证据与范围"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-问题索引"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-数据准备问题"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f01空-fx-的完整输入准备"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f02重复数组与静态数据构建"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f03完整对象校验"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f04顶点缓冲读取与数组初始化"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f05物理输入与轻量状态耦合"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f06输入需求需要细于-fx-类型"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f07重复发现重绘和缓存失效"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-图像与导出问题"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f08纯图像-fx-的执行时机"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f09图像输出依赖几何代理"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f10图像读回转换与重新上传"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f11线程额外-cpu-消耗"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f12avih264-兼容与导出状态"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-测量正确性与构建问题"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f13阶段计时覆盖不足"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f14gpu-跟踪覆盖与系统符号"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f15基线波动与场景可比性"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f16图像差异与动态正确性缺口"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f17诊断构建的清单错误"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-f18共享工作区与-abi-兼容"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-候选实施顺序"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-2026-09-29-第一批修改与复测"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-2026-09-29-需求查询缓存与按需快照设计"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-当前快照范围"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-缓存查询结果"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-按数据类别提供快照"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-当前实施状态与构建阻塞"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-实施状态修订覆盖本节早期计划描述"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-原始证据索引"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-2026-09-29-未应用-fx-关闭后的需求缓存回退"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-现象与原因"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-修复与验证"></a>
<a id="record-mmv-empty-fx-diagnosis-and-options-后续防护"></a>

## 先确认路径

main 当前使用 `src/mmv4mms/` 的完整场景接管。测量前记录 `MMV4MMS.dll` 的实际路径、版本、哈希和加载状态，确认未同时启用 MME 代理。旧 `MMV.dll` 网格桥接的数字不能作为当前路径成绩。

## 输入准备

宿主可用 `queryInputDemand` 查询所需通道，仍提供原生场景必需的几何、材质、骨骼与表情。无自定义效果不等于可省略场景绘制。快照构造、复制、校验和插件计算分别计时，避免将等待或深拷贝混称 GPU 求解时间。

结构变化、选择范围、参数重置和设备恢复都可能使缓存失效，不能只用静止空 FX 代表实际角色和物理负载。

## 输出与同步

有效输出需要本次成功调用、匹配帧号和尺寸的纹理。失败或沿用旧帧不能计为有效性能样本。输出纹理与宿主 DX9 设备共用，插件内部计算设备仍可能产生上传、回读或同步成本，不能凭后端名称声称零拷贝。

## 测量协议

保持相同场景、物理、效果、相机和输出方式，按 ABBA 交替测量独立构建。记录成功帧、绘制工作、场景推进及输出正确性。CPU/GPU 阶段、GPU 排空吞吐率、播放 FPS 和视频导出速度分别报告。

后台编译、其他渲染任务、错误提示及超时会使样本无效。性能正确性与画面一致性分别判断；逐帧动态结果需要独立验证，静态截图不能代替。

## 覆盖范围

插件求解器、传统 FX 兼容、图像输出、层处理和设备恢复各有独立失败边界。宿主接口检查通过不代表全部效果已经验收。方法见[帧基准](../../development/benchmarks.md)、[测量口径](../performance/benchmarks.md)及 [tests/README.md](../../../tests/README.md)。
