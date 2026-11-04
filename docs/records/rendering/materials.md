# 材质、光照与描边

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-pmd-toon-selection-fix"></a>
<a id="record-pmd-toon-selection-fix-验收结果"></a>
<a id="record-pmd-toon-selection-fix-原报告仍待现场确认"></a>
<a id="record-mme-toon-black"></a>
<a id="record-mme-toon-black-结论"></a>
<a id="record-mme-toon-black-复现与判据"></a>
<a id="record-mme-toon-black-消元实验"></a>
<a id="record-mme-toon-black-渲染路径"></a>
<a id="record-mme-toon-black-已排除"></a>
<a id="record-mme-toon-black-插件侧定位2026-09-24-续"></a>
<a id="record-mme-toon-black-与透明材质排序的关系2026-09-24-续二"></a>
<a id="record-mme-toon-black-插件侧定位2026-09-24-续三toon-纹理取自哪张表"></a>
<a id="record-mme-toon-black-gettoontexture-的两层数据源"></a>
<a id="record-mme-toon-black-谁填充它取宿主哪个字段"></a>
<a id="record-mme-toon-black-运行期对照移植侧少了两类键"></a>
<a id="record-mme-toon-black-根因"></a>
<a id="record-mme-toon-black-修正此前对-gate-补丁的读法"></a>
<a id="record-mme-toon-black-附带发现expgetcurrenttechnic-的字段位置两侧不一致"></a>
<a id="record-mme-toon-black-修复与验证2026-09-24-续四"></a>
<a id="record-mme-toon-black-诊断脚本"></a>
<a id="record-cloud-dark-shading"></a>
<a id="record-cloud-dark-shading-素材与-shader-证据"></a>
<a id="record-cloud-dark-shading-同条件后台对照"></a>
<a id="record-cloud-dark-shading-修复方向与边界"></a>
<a id="record-cloud-dark-shading-记录与版本"></a>
<a id="record-cloud-dark-shading-后续覆盖率缺陷"></a>
<a id="record-cloud-toon-adjustment"></a>
<a id="record-cloud-toon-adjustment-修改内容"></a>
<a id="record-cloud-toon-adjustment-文件与复现"></a>
<a id="record-cloud-toon-adjustment-加载问题与无效样本"></a>
<a id="record-cloud-toon-adjustment-委派与验收"></a>
<a id="record-cloud-toon-adjustment-有效对照结果"></a>
<a id="record-cloud-toon-adjustment-后续发黑定位"></a>
<a id="record-model-view-lighting"></a>
<a id="record-outline-width"></a>
<a id="record-outline-width-原因与修复"></a>
<a id="record-outline-width-原版证据"></a>
<a id="record-outline-width-验证"></a>

## Toon 贴图与颜色

PMD 的 toon 文件表不要求按 `toon01.bmp` 至 `toon10.bmp` 排列。共享 toon 根据文件名解析槽位，固定管线、内置效果和宿主交给 MMV 的数据采用一致规则。文件名匹配区分大小写；PMX 自定义 toon、无 toon 及自定义贴图各自按原规则处理。

模型与相机面板使用一致的场景光照来源。切换选择或编辑状态不应使原本有纹理的模型只剩黑色或灰白。第三方效果可能自己采样 toon 或改写灯光，因此需要区分原生与 MME 路径。

## 模型 alpha 与透明材质

材质 alpha、贴图 alpha、模型显示及特效输出共同决定可见性。透明排序改变提交顺序，不修正模型错误法线或特效内部的暗部。对云、草地等大面积半透明材质，先区分光照、toon、混合及深度写入，再判断是否需要排序。

排序档位与 MME 顶点身份约束见[透明排序](transparency.md)。不能把修改用户 toon 素材当作通用渲染器修复。

## 描边与相机距离

描边使用原生投影及镜头距离语义，几何、宽度和抗锯齿是不同步骤。预览缩放不应改变界面工具的尺寸；FXAA 描边平滑也不向外加宽轮廓。详见[抗锯齿](anti-aliasing.md)。

## MME 与材质登记

MME 的模型对象 ID、材质数量、名称和资源登记需对应当前结构。重新分配效果后出现消失或黑色材质时，检查实际效果、全局共享状态及当前对象表，见[MME 兼容](../effects/compatibility.md)。

PMD toon 映射保护不能证明所有 PMD 白模报告都源于同一问题。未知工程仍需原素材与准确操作复现，插件存在也不等于其实际着色已通过验证。

## 实现与回归入口

材质处理主要位于 `src/render/model_renderers.cpp`、`src/render/accessory.cpp` 和对应模型加载代码。PMD 选择渲染、模型 alpha、toon 名称与实际插件组合的测试见 [tests/README.md](../../../tests/README.md)。
