# 代码边界与审查要点

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-code-review-findings"></a>
<a id="record-code-review-findings-落地状态2026-09-23"></a>
<a id="record-code-review-findings-如何读这份清单"></a>
<a id="record-code-review-findings-畸形输入类的处理口径有意加固"></a>
<a id="record-code-review-findings-已核验条目反汇编核对结果"></a>
<a id="record-code-review-findings-核验结果pmm-v2--10-条全部原版无界全部加固"></a>
<a id="record-code-review-findings-核验结果媒体与导出--7-条全部原版无界"></a>
<a id="record-code-review-findings-核验结果pmd-载入器与物理回写--6-条无界1-条误报"></a>
<a id="record-code-review-findings-已核验条目的误报统计"></a>
<a id="record-code-review-findings-核验结果模型运行时--9-条加固1-条按原版改1-条保持"></a>
<a id="record-code-review-findings-核验总表"></a>
<a id="record-code-review-findings-逐条结论"></a>
<a id="record-code-review-findings-误报与偏差统计"></a>
<a id="record-code-review-findings-尚未核验的条目"></a>
<a id="record-code-review-findings-核验结果pmx-载入器--10-条加固2-条按原版改"></a>
<a id="record-code-review-findings-s1--崩溃内存损坏或数据丢失36-条"></a>
<a id="record-code-review-findings-b3-05-模型槽字节--已核验原版无校验--按加固实现"></a>
<a id="record-code-review-findings-b3-01-稀疏关键帧帧号--已核验原版无界加固"></a>
<a id="record-code-review-findings-本条曾两度误判过程记录"></a>
<a id="record-code-review-findings-b2-01-常态可达--置信度高--mem"></a>
<a id="record-code-review-findings-rb-03-常态可达--置信度高--mem"></a>
<a id="record-code-review-findings-rb-02-骨骼标记叠加缓冲--已核验原版同样无顶点上限--边界条件待确认"></a>
<a id="record-code-review-findings-rb-01-边界输入--置信度高--life"></a>
<a id="record-code-review-findings-cb-03-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-cb-04-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-cb-02-常态可达来自-mme-插件-置信度高--mem"></a>
<a id="record-code-review-findings-ab-01-环境相关--置信度高--mem"></a>
<a id="record-code-review-findings-mb-08-pmd-20-字节骨骼名--已核验为误报--撤回"></a>
<a id="record-code-review-findings-mb-01-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-mb-04-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-mb-02-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-mb-03-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-mb-05-每帧路径--需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-mb-06-每帧路径--需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-mb-07-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-ab-02-常态可达--置信度高--port"></a>
<a id="record-code-review-findings-b3-01--b3-21--pmmvpdvsqvmd-读取路径--需畸形输入--置信度高"></a>
<a id="record-code-review-findings-cb-01-需畸形输入--置信度高--mem"></a>
<a id="record-code-review-findings-其余-s1model-加载路径-需畸形输入--置信度高"></a>
<a id="record-code-review-findings-s2--用户可见的功能错误33-条"></a>
<a id="record-code-review-findings-cr-001-已实测复现--v2-工程不恢复相机参照模式"></a>
<a id="record-code-review-findings-cr-002-已修--参照模式补偿读错了旧枢轴"></a>
<a id="record-code-review-findings-相关的两处结构性问题同一区域尚未处理"></a>
<a id="record-code-review-findings-当前测试状态"></a>
<a id="record-code-review-findings-其余-s2"></a>
<a id="record-code-review-findings-cb-05-撤回依据2026-09-23-复核"></a>
<a id="record-code-review-findings-s3--资源泄漏性能退化与维护隐患41-条"></a>
<a id="record-code-review-findings-s4--一致性命名与文档36-条"></a>
<a id="record-code-review-findings-审查覆盖"></a>
<a id="record-full-review"></a>
<a id="record-full-review-一句话结论"></a>
<a id="record-full-review-范围与方法"></a>
<a id="record-full-review-审查口径"></a>
<a id="record-full-review-覆盖"></a>
<a id="record-full-review-实测验证"></a>
<a id="record-full-review-从零重建"></a>
<a id="record-full-review-警告"></a>
<a id="record-full-review-回归"></a>
<a id="record-full-review-发布布局与导出"></a>
<a id="record-full-review-真实素材手动验证"></a>
<a id="record-full-review-缺陷总览"></a>
<a id="record-full-review-根因一文件数据直接充当索引计数与长度s1-主体"></a>
<a id="record-full-review-根因二同类逻辑在多个实现点之间不一致"></a>
<a id="record-full-review-与实测失败对应的两条"></a>
<a id="record-full-review-子系统结论"></a>
<a id="record-full-review-未覆盖与未验证范围"></a>
<a id="record-full-review-畸形输入类缺陷的处理决定"></a>
<a id="record-full-review-建议的后续顺序"></a>
<a id="record-full-review-已解决视点字段的归属cr-002"></a>
<a id="record-full-review-原来的错处"></a>
<a id="record-full-review-原前提一两条测试互斥"></a>
<a id="record-full-review-原前提二这次复制会把补偿抹掉"></a>
<a id="record-full-review-修法与遗留"></a>
<a id="record-full-review-文档审查"></a>
<a id="record-full-review-悬空引用"></a>
<a id="record-full-review-提交哈希"></a>
<a id="record-full-review-测试计数"></a>
<a id="record-full-review-已实测完全一致的文档声明"></a>
<a id="record-full-review-文档引用的构建目录"></a>
<a id="record-full-review-附录本轮验证的原始数据"></a>
<a id="record-full-review-清理与备份"></a>
<a id="record-full-review-构建产物"></a>
<a id="record-rendering-review"></a>
<a id="record-rendering-review-范围与方法"></a>
<a id="record-rendering-review-本次修复"></a>
<a id="record-rendering-review-告警核验与保留问题"></a>
<a id="record-rendering-review-验证状态"></a>
<a id="record-rendering-review-透明层排序复核追加"></a>
<a id="record-runtime-layout-audit"></a>
<a id="record-runtime-layout-audit-已处理的对象与实际错误"></a>
<a id="record-runtime-layout-audit-构建一致性"></a>
<a id="record-runtime-layout-audit-保留的字节格式访问"></a>
<a id="record-runtime-layout-audit-验证与边界"></a>
<a id="record-runtime-layout-audit-第二轮残留步长与运行时临时记录"></a>

本页按模块说明维护时需要保持的约束。具体功能与格式见[文档导航](../../README.md)，测试入口见[测试说明](../../../tests/README.md)。

## 文件输入与容量

PMM、PMD、PMX、VMD、VPD 和媒体载入需要检查短读、计数溢出、分配失败、数组索引及引用关系。循环遍历容量不等于校验某个外部索引。关键帧槽位、帧号和显示项应分别按实际类型验证；输出时也要确认目标格式能表示数据。

标准格式兼容和输入加固分别判断。不能因为原版缺少检查就删除防护，也不能把所有差异归为移植错误。名称、路径及完整包扩展的保存边界见[工程格式](../formats/projects.md)。

## 运行时布局与资源

运行时成员通过具名字段和已核验的数据结构访问。`sizeof`、`offsetof`、ABI 布局断言及文件字段尺寸是有效约束；看到固定数字不能直接判为遗留偏移错误。跨 Win32/x64 迁移时重新检查指针宽度、对齐、数组步长及 COM 调用签名。

D3DX 网格接口由公共声明统一调用，不能按猜测的虚表槽位写入缓冲。网格、材质和纹理有独立生命周期，结构扩容时需要同步登记和失效处理。

## 骨骼、表情与关键帧

骨骼表情索引必须落在所分配数组内；隐藏表情仍可出现在时间轴，但同步四个表情栏时检查面板编号。选择状态、当前编辑骨骼和未登记编辑标记分别维护，切帧不能清掉仍有效的选择。

复制粘贴区分对象身份、轨道名称及同名顺序。载入后的按键抑制、切换焦点时的按键状态和折叠组命中表需要覆盖真实窗口消息，避免测试直接赋值绕过输入路径。

## 绘制、设备与线程

设备丢失、资源重建及关机应唤醒并结束相应等待，不能保留无限空转。异步纹理或模型准备只处理其拥有的 CPU 数据，D3D9 资源交接在正确线程完成。关闭、取消和失败路径也释放句柄和候选资源。

透明排序不能更改依赖原始顶点编号的 MME 语义。阴影覆盖、实际采样密度与滤波宽度分别验证，不通过扩大偏置或降低绘制量掩盖错误。

## 撤销、保存与恢复

一项操作的关键帧、物理禁用标记、末帧及插件状态需要一致提交和回滚。无变化的编辑不制造历史记录。保存先构造完整结果，再替换文件；备份、故障报告失败时不能再次破坏原工程。

日志和崩溃备份以尽力保存为边界，不保证在进程损坏或磁盘不可写时一定成功。机制见[运行与恢复](../../development/runtime-reliability.md)。

## 验证范围

优先用能经过生产入口的回归覆盖问题。合成模型、隐藏窗口、真实素材、实际插件和平台真机各有适用范围；一个类别通过不能替代其他类别。未复现的旧报告保留为未确认，不用已修复问题的测试结果替代。
