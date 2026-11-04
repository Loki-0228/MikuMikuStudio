# 自阴影覆盖、精度与滤波

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-shadow-progressive-20261027"></a>
<a id="record-shadow-packed-box-20261025"></a>
<a id="record-shadow-takeover-20261025"></a>
<a id="record-shadow-contour-filter-20261025"></a>
<a id="record-shadow-directional-filter-20261025"></a>
<a id="record-shadow-far-filter-20261025"></a>
<a id="record-shadow-screen-density-20261025"></a>
<a id="record-shadow-far-density-20261025"></a>
<a id="record-shadow-filter-smooth-20261025"></a>
<a id="record-shadow-smooth-20261024"></a>
<a id="record-uniform-shadow-precision-20261024"></a>
<a id="record-shadow-modes-20261024"></a>
<a id="record-shadow-transition-20261024"></a>
<a id="record-visible-near-density-20261024"></a>
<a id="record-shadow-range-box-20261024"></a>
<a id="record-dynamic-precision-20261024"></a>
<a id="record-camera-picture-detail-20261023"></a>
<a id="record-focused-self-shadow-20261022"></a>
<a id="record-adaptive-shadow-range"></a>
<a id="record-adaptive-shadow-range-2026-10-13-实现更新"></a>
<a id="record-adaptive-shadow-range-2026-10-12-历史实现与验收"></a>
<a id="record-adaptive-shadow-range-原因"></a>
<a id="record-adaptive-shadow-range-覆盖保护"></a>
<a id="record-adaptive-shadow-range-范围和像素密度"></a>
<a id="record-adaptive-shadow-range-边界"></a>
<a id="record-adaptive-shadow-range-验证记录"></a>
<a id="record-adaptive-shadow-range-精细度补偿和动态缩放实测"></a>
<a id="record-adaptive-shadow-range-当前构建复测含锁暝模型"></a>
<a id="record-mme-shadow-filter"></a>
<a id="record-mme-shadow-filter-完整包"></a>
<a id="record-mme-shadow-filter-最小包"></a>
<a id="record-mme-shadow-filter-验证记录"></a>
<a id="record-mme-shadow-filter-画质边界"></a>
<a id="record-mme-shadow-filter-发布前复测"></a>
<a id="record-mme-ground-shadow"></a>
<a id="record-mme-ground-shadow-已有验证"></a>
<a id="record-mme-ground-shadow-发布状态"></a>
<a id="record-native-shadow-coverage"></a>
<a id="record-native-shadow-coverage-投影与材质"></a>
<a id="record-native-shadow-coverage-动态精度与主题"></a>
<a id="record-native-shadow-coverage-验证"></a>
<a id="record-native-shadow-update"></a>
<a id="record-native-shadow-update-开关与模式"></a>
<a id="record-native-shadow-update-sdk-移除范围"></a>
<a id="record-native-shadow-update-验证结果"></a>
<a id="record-native-shadow-update-性能样本与限制"></a>
<a id="record-native-shadow-update-本地产物"></a>
<a id="record-shadow-preview-mme"></a>
<a id="record-shadow-preview-mme-行为"></a>
<a id="record-shadow-preview-mme-验证"></a>
<a id="record-plugin-native-rendering"></a>
<a id="record-plugin-native-rendering-修复内容"></a>
<a id="record-plugin-native-rendering-验收"></a>
<a id="record-plugin-native-rendering-已知限制"></a>
<a id="record-shadow-fixed-bands-20261028"></a>
<a id="record-shadow-enhanced-bands-20261029"></a>
<a id="record-fixed-model-shadow-20261029"></a>
<a id="record-curved-shadow-filter-20261029"></a>

本页描述完整包原生 SM3 阴影路径。最小包保留原版阴影；MME 自建阴影依照其 FX，不使用本页全部控制。操作入口见[画面设置](../../guides/rendering.md#设置透明排序与阴影)。

<a id="shadow-precision"></a>
## 精度标尺与覆盖

滑条数值越小，要求的采样密度越高。人物、场景与附件共用原生投影中心的精度标准，不根据物体大小提高密度。全局方形覆盖盒包含当前相机可见物体，深度范围保留画外投影物产生的阴影。

贴图最小尺寸及尺寸取整只增加外围覆盖；设备尺寸上限和显存预算可能使实际密度低于要求。应记录实际贴图、投影矩阵和密度，不能只依据滑条值推断画质。

<a id="fixed-shadows"></a>
## 固定精度

「动态精度」默认关闭。固定模式按滑条申请密度，不生成或混合相机近、中、远动态细节层，关闭动态模式时释放这些资源。

全局贴图因大场景达到尺寸上限时，可为能受益的模型增加固定阴影图。补充图覆盖整个模型、跟随模型移动，精度不随相机距离变化；场景中的其他投影物仍可向它投影。达到目标密度时不额外分配，分配失败时使用全局图。这会增加显存与投影绘制成本。

<a id="dynamic-shadows"></a>
## 动态精度

开启后按最终相机投影与当前渲染视口分配近、中、远细节层，额外增采样量保留对应方案的 80%。各层由全部物体共享，其深度区间不随目标点距离或单个物体大小变化。

中近景扩展使放大的场景更早获得细节。桥接覆盖盒根据视锥平移，在不改变现有图尺寸与密度时提高有效利用率；扩展层最外侧在 2.5 倍深度跨度内淡出。完整覆盖层保持滑条要求的基础密度，超出资源上限时记录实际值。

交接区间按相机与屏幕需求计算，密度相等的图层保持稳定顺序。关闭加强滤波时，可从现有细节图重建中间网格并逐步接到粗层；计算量增加，但不为这些中间网格另分配阴影图或增加投影遍数。低精度锯齿仍可能可见。

<a id="shadow-filtering"></a>
## 采样与加强滤波

全局覆盖图使用浮点双线性插值，保留相邻四点及原有比较规则；它不依赖「滤波加强」。

加强滤波先逐点比较遮挡再混合，避免背景深度侵蚀细小阴影。滤波宽度按实际采样间距计算，同密度扩容不加宽。轮廓重建在可靠的局部边界拟合覆盖率，细条双边、真实拐角及不可靠轮廓保留原结果，人物局部明暗端值保持不变。

表面斜率补偿使用相机相对深度，关闭加强滤波时不用此补偿。曲面保护限制不可靠的切平面外推，避免粗网格在弯曲表面上产生大块假阴影。补偿和轮廓重建增加边缘采样与计算，不提高贴图本身的精度。

<a id="shadow-modes"></a>
## 模式与预览

模式 1、2 保留不同的遮挡深度比较，并以原版默认范围和投影中心校准。完全遮挡处可以相同，差异主要在过渡区域；不承诺逐像素复刻旧版随投影变化的结果。

预览比例不改变导出尺寸。原生最近层保留所需密度，中远层可随屏幕需求调整；MME 的固定目标不被改写。第三方 FX 决定其最终光照和阴影。

<a id="shadow-validation"></a>
## 验证方法与限制

分别检查固定模式、动态模式、两种阴影模式、滤波开关、资源回退及连续镜头移动。数值公式、静帧像素和连续交接是不同层面的检查，不能相互替代。

`native_shadow_tests` 与帧基准的实际用法见 [tests/README.md](../../../tests/README.md)。使用实际贴图和密度判断覆盖，不通过扩大偏置、放宽误差或省略画外投影物掩盖问题。
