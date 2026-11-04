# 预览比例与模型缓存

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mme-shadow-policy"></a>
<a id="record-mme-preview-optimization"></a>
<a id="record-mme-preview-optimization-自动重载"></a>
<a id="record-mme-preview-optimization-小预览的固定阴影资源"></a>
<a id="record-mme-preview-optimization-已完成验证"></a>
<a id="record-mme-preview-optimization-预览缩放耗时"></a>
<a id="record-mme-preview-optimization-gpu-蒙皮传输"></a>
<a id="record-model-vertex-cache"></a>
<a id="record-model-vertex-cache-缓存范围"></a>
<a id="record-model-vertex-cache-前后对照"></a>
<a id="record-model-vertex-cache-效果核验"></a>
<a id="record-model-vertex-cache-适用边界"></a>
<a id="record-model-vertex-cache-构建与记录"></a>
<a id="record-camera-preview-cache"></a>
<a id="record-camera-preview-cache-正确性验证"></a>
<a id="record-camera-preview-cache-性能记录状态"></a>
<a id="record-stage-no-ray-gpu-timing"></a>
<a id="record-stage-no-ray-gpu-timing-测量范围"></a>
<a id="record-stage-no-ray-gpu-timing-gpu-分段结果"></a>
<a id="record-stage-no-ray-gpu-timing-整帧耗时与重复性"></a>
<a id="record-stage-no-ray-gpu-timing-限制收益的工作"></a>
<a id="record-stage-no-ray-gpu-timing-验证与记录"></a>
<a id="record-stage-no-ray-performance"></a>
<a id="record-stage-no-ray-performance-降低分辨率的补充定位"></a>
<a id="record-stage-no-ray-performance-实现"></a>
<a id="record-stage-no-ray-performance-正式配置对照"></a>
<a id="record-stage-no-ray-performance-验证范围"></a>
<a id="record-mme-preview-performance"></a>
<a id="record-mme-preview-performance-场景与尺寸观察"></a>
<a id="record-mme-preview-performance-修改内容"></a>
<a id="record-mme-preview-performance-abba-对照"></a>
<a id="record-mme-preview-performance-smaa-菜单与验证"></a>

## 预览分辨率

完整包的预览比例调整实际渲染尺寸，不改变导出尺寸。HUD、工具与文字按显示分辨率绘制，低分辨率画面用最近邻放大。最小包没有预览比例菜单。

降低像素数主要减少部分绘制工作，不按比例减少模型求值、物理、CPU 等待或固定大小的中间目标。TAA/T2x 的运动捕获也有独立成本，排查入口见[性能指南](../../guides/performance.md)。

## 原生与 MME 阴影

预览比例只主动调整默认渲染资源；原生最近层保留所需采样密度，动态中远层结合屏幕需求。MME 固定阴影与其他目标依照 FX 声明，不针对 SoftShadow、ray 做专用缩放，也不改写其采样参数。

声明 `ViewportRatio` 的 MME 目标仍遵循 MME 原规则。目标用途无法仅从尺寸可靠判断，强行缩小可能破坏反射、粒子或历史状态。

## 模型结果复用

完整包与最小包都可复用未变化的模型计算结果。CPU 蒙皮下仅相机或描边变化时更新必要描边，不重复完整变形；骨骼、表情、物理和源几何变化仍正常计算。

适用的大模型 GPU 路径减少不必要的读回字节，仍包含 CPU 与 D3D9 上传，不等同于零拷贝或原生 DX12 场景渲染。

## 阴影边界与透明排序缓存

有效的当前模型顶点可复用扫描边界；外部代理、布局变化、设备重置或未验证姿态回到完整扫描。透明排序缓存同时核对相机、原始顶点与索引，命中后仍执行绘制和效果回调。具体预算与失效条件见[排序缓存](../rendering/transparency.md#sort-cache)。

## FX 自动重载通知

已识别 MME 版本通过 Windows 文件通知缓存时间戳，减少无修改时反复打开文件。变更后仍由 MME 重新编译，监听不可用或版本不匹配时回到原查询，不扩大 include 重载范围。

## 比较方法

在同一工程、镜头、动作、物理和特效设置下交替比较 100%、低比例、低比例、100%。固定姿态吞吐率、实际播放 FPS 和分段 GPU 时间分别报告。运动模糊等时间相关 FX 需明确可比条件；被后台编译干扰的样本不能混入有效汇总。

原始数据可在[测量目录](benchmarks.md#原始数据)查阅，不作为当前构建的性能保证。
