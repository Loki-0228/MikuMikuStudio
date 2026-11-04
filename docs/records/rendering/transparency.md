# 透明排序与合成

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-sort-scope-motionblur"></a>
<a id="record-object-local-preview-cache"></a>
<a id="record-transparent-sort-strengths"></a>
<a id="record-transparent-sort-strengths-最高档保留优化与精度"></a>
<a id="record-transparent-sort-strengths-验证范围"></a>
<a id="record-transparent-sort-validation"></a>
<a id="record-transparent-sort-validation-逐项隔离"></a>
<a id="record-transparent-sort-validation-阴影测试边界"></a>
<a id="record-transparent-sort-validation-最终验收"></a>
<a id="record-transparent-sort-five-tiers"></a>
<a id="record-transparent-sort-five-tiers-档位与绘制方式"></a>
<a id="record-transparent-sort-five-tiers-pmm-v2-保存"></a>
<a id="record-transparent-sort-five-tiers-构建与后台回归"></a>
<a id="record-transparent-sort-five-tiers-双-build-云场景对照"></a>
<a id="record-transparent-sort-five-tiers-来源与保存位置"></a>
<a id="record-transparent-layer-ordering"></a>
<a id="record-transparent-layer-ordering-实现"></a>
<a id="record-transparent-layer-ordering-透明检测门控"></a>
<a id="record-transparent-layer-ordering-材质依赖排序"></a>
<a id="record-transparent-layer-ordering-三角形排序与安全合批"></a>
<a id="record-transparent-layer-ordering-保守重叠缓存"></a>
<a id="record-transparent-layer-ordering-绘制段数上限"></a>
<a id="record-transparent-layer-ordering-缓存与预算"></a>
<a id="record-transparent-layer-ordering-mme-边界"></a>
<a id="record-transparent-layer-ordering-限制"></a>
<a id="record-transparent-layer-ordering-验收"></a>
<a id="record-transparent-layer-ordering-性能正式对照"></a>
<a id="record-transparent-layer-ordering-优化组合的独立对照"></a>
<a id="record-transparent-layer-ordering-交付版本的面部对照"></a>
<a id="record-transparent-layer-ordering-复现状态"></a>
<a id="record-transparent-layer-ordering-可用构建"></a>
<a id="record-transparent-batching"></a>
<a id="record-transparent-batching-实现与限制"></a>
<a id="record-transparent-batching-同条件性能对照"></a>
<a id="record-transparent-batching-正确性验证"></a>
<a id="record-transparent-batching-fx-最终透明度的后续优化"></a>
<a id="record-transparency-occlusion"></a>
<a id="record-transparency-occlusion-问题与兼容范围"></a>
<a id="record-transparency-occlusion-实现"></a>
<a id="record-transparency-occlusion-面部半透明叠层的补充分析"></a>
<a id="record-transparency-occlusion-验收方法"></a>
<a id="record-transparency-occlusion-git-隔离"></a>
<a id="record-transparency-occlusion-独立基准测量约定"></a>
<a id="record-transparency-occlusion-验证记录"></a>
<a id="record-transparency-occlusion-后续更新透明层排序"></a>
<a id="record-transparent-sort-limit-pmm"></a>
<a id="record-transparent-sort-limit-pmm-用户行为"></a>
<a id="record-transparent-sort-limit-pmm-最高上限的工作量"></a>
<a id="record-transparent-sort-limit-pmm-构建与后台验证"></a>
<a id="record-transparent-sort-limit-pmm-保存重开另存"></a>
<a id="record-transparent-sort-limit-pmm-当前云镜头的图像与耗时"></a>
<a id="record-transparent-sort-limit-pmm-深层透明专项"></a>
<a id="record-transparent-sort-limit-pmm-保存位置"></a>
<a id="record-auto-transparent-sorting"></a>
<a id="record-auto-transparent-sorting-开关行为"></a>
<a id="record-auto-transparent-sorting-后台正确性验收"></a>
<a id="record-auto-transparent-sorting-前后-build-对照"></a>
<a id="record-auto-transparent-sorting-验证工具和产物"></a>
<a id="record-auto-transparent-sorting-2026-09-26-归档核对"></a>
<a id="record-cloud-sort-main-integration"></a>
<a id="record-cloud-sort-main-integration-合并与复核"></a>
<a id="record-cloud-sort-main-integration-构建与后台回归"></a>
<a id="record-cloud-sort-main-integration-候选版与合并版对照"></a>
<a id="record-cloud-sort-main-integration-版本与证据位置"></a>
<a id="record-cloud-float-compositing"></a>
<a id="record-cloud-float-compositing-结论与边界"></a>
<a id="record-cloud-float-compositing-为什么调白-toon-后图片仍会发黑"></a>
<a id="record-cloud-float-compositing-实现"></a>
<a id="record-cloud-float-compositing-两版-build-对照"></a>
<a id="record-cloud-float-compositing-回归结果"></a>
<a id="record-cloud-float-compositing-原始证据与委派"></a>
<a id="record-transparent-sort-precision"></a>
<a id="record-transparent-sort-precision-用户入口和保存"></a>
<a id="record-transparent-sort-precision-场景与问题"></a>
<a id="record-transparent-sort-precision-几何分割路线及失败证据"></a>
<a id="record-transparent-sort-precision-当前逐像素分层原型"></a>
<a id="record-transparent-sort-precision-尚未解决的边界"></a>
<a id="record-transparent-sort-precision-已完成的验证"></a>
<a id="record-transparent-sort-precision-保存位置"></a>
<a id="record-transparent-sort-precision-后续合成精度与目标-alpha-修复"></a>
<a id="record-cloud-transparency-experiment"></a>
<a id="record-cloud-transparency-experiment-验收状态"></a>
<a id="record-cloud-transparency-experiment-已确认原因"></a>
<a id="record-cloud-transparency-experiment-候选行为及限制"></a>
<a id="record-cloud-transparency-experiment-两版本来源"></a>
<a id="record-cloud-transparency-experiment-后台测评"></a>
<a id="record-cloud-transparency-experiment-文件与后续工作"></a>
<a id="record-transparency-global-sort"></a>
<a id="record-transparency-global-sort-两个-pmx-的差异"></a>
<a id="record-transparency-global-sort-排序容量与跨模型"></a>
<a id="record-transparency-global-sort-面部画面与阴影"></a>
<a id="record-transparency-global-sort-用户对照图观察2026-09-25"></a>
<a id="record-transparency-global-sort-验证与版本"></a>
<a id="record-transparency-global-sort-性能对照"></a>
<a id="record-transparency-global-sort-dsh-分工与验收"></a>

完整包通过「显示」菜单设置透明排序。新建工程及没有有效设置的工程默认关闭；最小包使用原生材质提交顺序。操作见[画面设置](../../guides/rendering.md#设置透明排序与阴影)。

<a id="sort-levels"></a>
## 程度与范围

| 程度 | 当前行为 |
| --- | --- |
| 关闭 | 保留原始提交顺序 |
| 低 | 16 个深度桶，桶内保持输入顺序 |
| 标准 | 64 个深度桶，桶内保持输入顺序 |
| 高 | 256 个深度桶，桶内保持输入顺序 |
| 最高 | 原生适用路径使用逐像素深度剥离及几何回退；MME 使用完整三角形精确深度稳定排序 |

「开启跨物体排序」是独立选项，默认关闭。四种程度都可选择物体内部或跨物体排序；关闭排序不会抹掉范围选择。仅物体内时保留不同物体的原有顺序，同一物体的透明材质共同参与。

分桶档位处理全部有效三角形，不按面数截断。降低程度会降低排序精度，不能从桶数推算绘制面数或固定加速比。

<a id="native-sorting"></a>
## 原生最高档

深度剥离以参与排序的裁剪后三角形数量加一作为上限，在透明片段耗尽时提前结束。合成使用适当精度保留低 alpha 的多层累积。仅物体内模式按物体规划，单物体可深度剥离，多物体使用几何回退；跨物体模式允许统一处理。

BSP 回退预算为额外 4,000,000 个三角形、深度 512、epsilon `2e-13`。预算、目标格式或设备能力不足时走相应回退，不能把回退渲染误记为剥离完成。

原生路径可排除最终 alpha 为零的材质，且在几何回退中合并屏幕不重叠的同材质三角形。重叠区域保留必要的先后关系；MME 路径不采用依赖原生 alpha 语义的这些优化。

<a id="mme-sorting"></a>
## MME 顶点身份

MME 可能用 `_INDEX`、`VERTEXINDEXOFFSET` 读取上一帧顶点。排序保留原始模型顶点缓冲，只重排原始三角形索引，GPU 继续执行视锥裁剪。最高档不生成切分顶点，以完整三角形的精确深度稳定排序，因此精度高于高档分桶，但无法逐像素解决所有交叉透明面。

该兼容行为不按效果文件名选择，也不修改外部 FX。最终效果由第三方着色器、深度写入和混合状态共同决定。

<a id="sort-cache"></a>
## 预览内存缓存

缓存保留原始顶点与索引快照、投影结果、排序结果、绘制区间及私有 GPU 资源。每次核对相机、物体、材质和原始数据；MME 可直接改写缓冲，因此不能只看骨骼更新编号。

未改变的物体复用投影与排序结果；全部命中时省去相应分配和上传，仍执行材质绘制、参数绑定、MME 回调与最高档剥离。缓存不保存最终画面。

新增原始快照上限为 128 MiB，新增 GPU 缓冲保留上限为 256 MiB，不包含其他排序工作内存。超限回到逐帧计算，不丢弃几何。关闭排序、物体集合清空、设备重置或退出时释放。`MMS_TRANSPARENT_CACHE_DISABLE=1` 可用于对照。

<a id="sort-storage"></a>
## 保存与验证边界

PMM 独立保存程度和 `org.mikudancestudio.view.transparent_sort_cross_objects`。程度编号为低 0、标准 1、高 2、最高 3、关闭 4。范围切换使排序计划和 GPU 索引缓存失效，不必要的投影仍可复用。详细规则见[PMM 扩展](../../reference/pmm-extensions.md)。

透明回归分别覆盖普通目标、MSAA、低 alpha 累积、相交表面和缓存失效。最高档需要同时检查实际层数、完成与提交状态；最终像素正确不能证明具体路径已执行。测试方法见 [tests/README.md](../../../tests/README.md)。

关闭排序更接近原始提交方式，但不意味着任意原版工程逐像素一致。运动模糊等读取真实时间的 FX 也会影响截图可比性，性能比较须固定有效条件。
