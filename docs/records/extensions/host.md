# MMV4MMS 宿主与渲染接口

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mmv-implementation"></a>
<a id="record-mmv-implementation-2026-09-27-分层循环接口"></a>
<a id="record-mmv-implementation-历史集成记录"></a>
<a id="record-mmv-implementation-宿主交接"></a>
<a id="record-mmv-implementation-已运行的验证"></a>
<a id="record-mmv-implementation-公开接口依据"></a>
<a id="record-mmv-direct-render"></a>
<a id="record-mmv-direct-render-验证记录"></a>
<a id="record-mmv-without-mme"></a>
<a id="record-mmv-without-mme-代码核对与修改"></a>
<a id="record-mmv-without-mme-隐藏窗口验收"></a>
<a id="record-mmv-without-mme-失败记录与验证边界"></a>
<a id="record-mmv-project-split"></a>
<a id="record-mmv4mms-emm-fix"></a>
<a id="record-mmv4mms-emm-fix-验证记录"></a>
<a id="record-mmv-deformation-v3"></a>
<a id="record-mmv-deformation-v3-接口与资源"></a>
<a id="record-mmv-deformation-v3-已验证的行为"></a>
<a id="record-mmv-deformation-v3-未完成的验收与能力"></a>
<a id="record-mmv-layer-validation"></a>
<a id="record-mmv-layer-validation-功能合同"></a>
<a id="record-mmv-layer-validation-已取得的证据"></a>
<a id="record-mmv-layer-validation-前后版本对照"></a>
<a id="record-mmv-layer-validation-待完成"></a>
<a id="record-mmv-bullet3-main-integration"></a>
<a id="record-mmv-bullet3-main-integration-合并内容"></a>
<a id="record-mmv-bullet3-main-integration-构建与正确性"></a>
<a id="record-mmv-bullet3-main-integration-同条件对照"></a>
<a id="record-mmv-bullet3-main-integration-证据与边界"></a>
<a id="record-mmv-bullet3-main-integration-同步最新-main"></a>

本页以 main 的 `src/mmv4mms/` 和 [SDK](../../../sdk/mmv4mms/README.md) 为准。MMV4MMS 是另行安装的完整场景渲染插件，主程序包不附带。最小构建档不加载该插件。

## 加载与启用

完整包从 EXE 同目录加载 `MMV4MMS.dll`，查询 `MMV4MMSGetPluginApi`，校验 MMS ABI、运行时 ABI、结构大小及必需回调。插件缺失或不兼容时不创建占位菜单，保留宿主自身渲染。

已加载 `MMEffect.dll` 时不启用 MMV4MMS，避免同时接管。不能按旧网格桥接方案安装 `MMV.dll` 后期待当前加载器使用它。安装见[插件使用](usage.md)。

## 菜单与生命周期

菜单由插件通过 `MMSGetMenuHostApi` 注册。可选 `MMVGetMenuApi` 版本 1 提供状态和命令；宿主读取 `MMV_MENU_RENDERER_DISABLED` 判断是否使用自身渲染。

项目打开、保存、场景重置、对象移除、语言切换及设备丢失通过通知传递。销毁前释放输出引用和插件实例，设备事件清除失效资源。

## 场景快照

宿主在动画、物理和蒙皮更新后构造只读输入。可选 `queryInputDemand` 先查询需要的通道；最终原生场景绘制仍强制提供顶点、三角形、材质、骨骼和表情，不把没有自定义效果误解成无需几何。

隐藏模型仍可作为效果控制器，保留身份、骨骼与表情，并单独传递可见状态。材质 tint、toon、附件原始面板值和对象透明度在各自字段传递，避免矩阵分解折回整圈旋转。

## 当前帧输出

插件在宿主 DX9 设备上返回借用颜色纹理。输出必须成功、具有有效纹理，并匹配本次帧号和尺寸。失败清空当前输出并记录原因，不提交上一帧纹理。

有效输出替代宿主场景绘制，继续显示、录制及编辑控件合成。调用前后保存和恢复设备状态。实时插件结果不写回模型定义、关键帧或原生物理世界。

## 原生阴影与材质

宿主可提供同设备、同帧的 R32F 原生阴影图、投影矩阵和采样契约，供插件使用；无 CPU 深度回读。采样版本与可选尾字段通过结构大小判断，不能重复过滤已处理的图。

材质回调区分原提交、全不透明、半透明和覆盖预通道。自定义 FX 保留自身 pass 状态，不自动套用原生覆盖预通道。字段定义以 [mms.h](../../../sdk/mmv4mms/include/mmv/mms.h) 为准。

## 能力与检查边界

插件内部 FX、层、参数、求解器和存档由对应插件版本实现；头文件存在不等于所有算法或烘焙流程已经接入主程序。当前接管输出为 DX9，不是原生 DX12 场景输出，也不与 MME 代理同时运行。

菜单、只读物理输入、材质 pass 和原生阴影桥接的测试见 [tests/README.md](../../../tests/README.md)。接口检查不代替所有第三方效果、真实 PMM 和视频输出的完整对照。
