# 运算器选择与回退

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-processor-settings-proposal"></a>
<a id="record-processor-settings-proposal-设置范围"></a>
<a id="record-processor-settings-proposal-物理求解器与设备选择"></a>
<a id="record-processor-settings-proposal-用户配置与工程隔离"></a>
<a id="record-processor-settings-proposal-生效规则"></a>
<a id="record-processor-settings-proposal-验收范围"></a>
<a id="record-processor-settings-proposal-混合显卡的可见设备"></a>
<a id="record-processor-settings-proposal-主渲染设备菜单的完整设备显示"></a>
<a id="record-processor-settings-proposal-性能状态"></a>
<a id="record-processor-settings-proposal-后续验收"></a>
<a id="record-gpu-skinning"></a>
<a id="record-gpu-skinning-设备与回退"></a>
<a id="record-gpu-skinning-正式渲染接入"></a>
<a id="record-gpu-skinning-验证记录"></a>
<a id="record-gpu-skinning-正式目录整合验证"></a>
<a id="record-gpu-skinning-性能限制"></a>
<a id="record-gpu-skinning-图像差异与覆盖边界"></a>
<a id="record-gpu-skinning-本地产物与协作"></a>
<a id="record-skinning-autoselect"></a>
<a id="record-skinning-autoselect-自动项显示当前设备"></a>
<a id="record-skinning-autoselect-测速规则"></a>
<a id="record-skinning-autoselect-缓存与失效"></a>
<a id="record-skinning-autoselect-中文文案"></a>
<a id="record-skinning-autoselect-验证"></a>
<a id="record-skinning-autoselect-性能对照"></a>
<a id="record-skinning-autoselect-本轮复核"></a>
<a id="record-skinning-autoselect-复现产物"></a>
<a id="record-persistent-skinning"></a>
<a id="record-persistent-skinning-验证与位置"></a>
<a id="record-dx12-optional-compat"></a>
<a id="record-dx12-optional-compat-完全关闭-dx12-的构建"></a>
<a id="record-dx12-optional-compat-回归验证"></a>
<a id="record-dx12-startup-validation"></a>
<a id="record-dx12-startup-validation-版本与产物"></a>
<a id="record-dx12-startup-validation-验证结果"></a>
<a id="record-dx12-startup-validation-尚未验收的范围"></a>
<a id="record-d3d12-migration"></a>
<a id="record-d3d12-migration-implemented"></a>
<a id="record-d3d12-migration-why-a-separate-native-viewport"></a>
<a id="record-d3d12-migration-reproduction"></a>
<a id="record-d3d12-migration-verified-on-rtx-4070-laptop-gpu"></a>

## 主渲染与顶点计算

通过「帮助 → 运算器」分别选择主渲染和顶点计算设备。配置保存在程序旁 `data/processor.ini`，跨工程使用，下次启动生效，不写 PMM。主渲染仍受当前 D3D9 能力限制；DXGI 能枚举某张显卡，不表示它能作为 D3D9 主设备。

顶点可使用 CPU 或可用 GPU，输出继续供当前渲染路径使用。DX12 计算与完整场景的绘制设备是不同概念；MMV4MMS 场景接管也使用宿主 DX9 输出。

## 自动选择

自动模式对当前模型比较完整顶点阶段，并检查 GPU 与 CPU 结果。候选预热后交替采样，以中位数排名；输入打包、上传、计算、同步回读及 D3D9 缓冲写入参与计时，初始化和编译不作为持续阶段成绩。

校验失败、不可用或耗时无效的候选不参与排名，CPU 胜出是正常选择。该结果只表示顶点阶段较快，不保证整帧或播放 FPS 最优。

## 缓存与失效

校准结果结合模型结构、设备、源缓冲及有效布局缓存；模型替换、权重布局、设备故障或偏好变化使相关记录失效。跨启动缓存也须核对其有效身份，不把另一台机器或不同负载的成绩当作当前结论。

配置保留自动选择意图，而不是把一次测得的 GPU 写成永久手选。菜单显示实际设备状态；供电、负载变化时应重新评估。

## DX12 与兼容路径

DX12 是可选后端，缺少运行时或设备不满足时保留 DX9/CPU 路径。构建可完全关闭 DX12，Win7 基础 preset 使用这一方式。设备丢失和资源失败需要释放并重建对应状态，不能继续使用无效资源。

## 排查与验证

遇到异常时可手选 CPU，重启后以相同工程对比。记录显卡身份、程序、构建档及实际路径。混合显卡、WARP/REF、独显与集显等分别验证；单台 NVIDIA 设备的结果不能代表 AMD、Intel 或旧系统已通过。

操作见[性能指南](../../guides/performance.md)，硬件回归与测量入口见 [tests/README.md](../../../tests/README.md)。
