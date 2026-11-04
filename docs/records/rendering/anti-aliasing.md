# 抗锯齿与描边平滑

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-temporal-aa-gpu"></a>
<a id="record-temporal-aa-gpu-实现范围"></a>
<a id="record-temporal-aa-gpu-正确性检查"></a>
<a id="record-temporal-aa-gpu-本机性能对比"></a>
<a id="record-temporal-aa-gpu-审核证据"></a>
<a id="record-native-shadow-aa"></a>
<a id="record-native-shadow-aa-行为与范围"></a>
<a id="record-native-shadow-aa-正确性"></a>
<a id="record-native-shadow-aa-遐蝶模型对照"></a>
<a id="record-native-shadow-aa-耗时与限制"></a>
<a id="record-native-shadow-aa-本地产物"></a>
<a id="record-nvidia-fix-binding"></a>
<a id="record-nvidia-fix-binding-绑定对象"></a>
<a id="record-nvidia-fix-binding-只读核对"></a>
<a id="record-nvidia-fix-binding-看起来像电脑绑定"></a>
<a id="record-nvidia-fix-binding-改动与验证"></a>

操作入口见[画面设置](../../guides/rendering.md#选择抗锯齿)。本页说明完整包实现；最小包仅保留原生抗锯齿开关。

## 算法与采样

| 算法 | 采样和主要开销 |
| --- | --- |
| 原生 MSAA | D3D9 多重采样路径，受设备、目标格式与 MME 影响 |
| FXAA | 处理当前帧边缘，不使用时间历史 |
| TAA | 使用运动矢量对齐历史采样，运动捕获增加额外工作 |
| SMAA 1x | 当前帧边缘检测与混合，可选择斜线和拐角增强 |
| SMAA T2x | 结合两帧采样与运动矢量，也提供增强档 |

选择的是具体档位，SMAA 父菜单只负责展开。算法、开关和 SMAA 档位随 PMM 扩展保存。

## 原生描边

FXAA 的原生描边平滑只在描边覆盖的像素间处理，不向外加宽轮廓；已经有渐变覆盖的描边保留原位。细线可能仍因覆盖不足而断续，可调整描边粗细。其他特效绘制的描边由其自身实现决定。

## 静帧与导出

静止画面及只有第 0 帧的工程补充所需采样。图片序列以输出帧组织历史，避免重复推进整套渲染。图片、序列和视频的透明背景、尺寸与采样路径分别检查，不能用交互预览结果代替导出结果。

TAA、T2x 的时间采样开销不随预览像素数等比例下降，复杂 MME 场景可能仍受运动捕获影响。预览比例和输出尺寸的关系见[预览机制](../performance/preview.md)。

## NVIDIA 辅助设置

`MMSNvidiaAAFix.dll` 是独立可选附件，通过帮助菜单安装或卸载 NVIDIA 驱动设置，主要针对原生 MSAA 下的双面绘制白边。它不改变 FXAA、TAA、SMAA 的算法。安装和路径范围见[说明](../../guides/rendering.md#处理-nvidia-双面绘制白线)。

## 维护入口

算法及轮廓回归见 [tests/README.md](../../../tests/README.md)。验证需固定算法、采样档、视口、动作、MME 配置及导出方式；不将单张静帧等同于时间采样无闪烁的证明。
