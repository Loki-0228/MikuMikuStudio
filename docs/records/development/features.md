# 功能与构建档范围

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-additional-features"></a>
<a id="record-additional-features-分类规则"></a>
<a id="record-additional-features-原版缺陷修复案例"></a>
<a id="record-additional-features-功能一览"></a>
<a id="record-additional-features-原生骨骼缩放"></a>
<a id="record-additional-features-物理与关键帧烧录"></a>
<a id="record-additional-features-运算器"></a>
<a id="record-additional-features-透明排序上限"></a>
<a id="record-additional-features-档位与分层上限"></a>
<a id="record-additional-features-分层不可用时的回退"></a>
<a id="record-additional-features-pmm-v2-项目设置扩展"></a>
<a id="record-additional-features-视口操作按钮与配件状态标识"></a>
<a id="record-additional-features-曲线编辑器"></a>
<a id="record-additional-features-相机照明-vmd-中的-x-附件动作"></a>
<a id="record-additional-features-网格烘焙与-abc-导出"></a>
<a id="record-additional-features-宿主对可选-mmv-插件的加载与交接"></a>
<a id="record-additional-features-建库快照已有的自定义功能来源不明待原版核验"></a>
<a id="record-additional-features-2026-09-27-原生编辑器补充"></a>
<a id="record-additional-features-同期默认值与文案调整"></a>
<a id="record-additional-features-不计入已交付能力的项目"></a>
<a id="record-additional-features-git-与源码索引"></a>
<a id="record-mms-native-features-audit"></a>
<a id="record-mms-native-features-audit-审计范围与复核命令"></a>
<a id="record-mms-native-features-audit-判定口径"></a>
<a id="record-mms-native-features-audit-一已交付增强平台语言与界面"></a>
<a id="record-mms-native-features-audit-原生-win64-构建"></a>
<a id="record-mms-native-features-audit-简体中文界面第三种界面语言"></a>
<a id="record-mms-native-features-audit-unicode-文件名与路径"></a>
<a id="record-mms-native-features-audit-视口操作按钮配色与配件-x-角标"></a>
<a id="record-mms-native-features-audit-视口光标图标统一"></a>
<a id="record-mms-native-features-audit-二已交付增强模型格式骨骼缩放与物理"></a>
<a id="record-mms-native-features-audit-原生骨骼缩放"></a>
<a id="record-mms-native-features-audit-刚体与-joint-的缩放继承"></a>
<a id="record-mms-native-features-audit-物理与关键帧烧录"></a>
<a id="record-mms-native-features-audit-通道选择稀疏采样与动态骨骼键池"></a>
<a id="record-mms-native-features-audit-宿主侧可选-bullet3-物理接入"></a>
<a id="record-mms-native-features-audit-三已交付增强附件"></a>
<a id="record-mms-native-features-audit-附件曲线编辑入口与附件动作轨道"></a>
<a id="record-mms-native-features-audit-四已交付增强曲线编辑与-vmdpmm-扩展"></a>
<a id="record-mms-native-features-audit-原生浮窗曲线编辑器"></a>
<a id="record-mms-native-features-audit-pmm-v2-扩展容器"></a>
<a id="record-mms-native-features-audit-vmd-尾部扩展"></a>
<a id="record-mms-native-features-audit-五已交付增强烘焙网格缓存与输出"></a>
<a id="record-mms-native-features-audit-网格烘焙与-alembic-导出"></a>
<a id="record-mms-native-features-audit-六已交付增强渲染透明排序与蒙皮"></a>
<a id="record-mms-native-features-audit-五档透明排序上限"></a>
<a id="record-mms-native-features-audit-正式-d3d9mme-蒙皮运算器"></a>
<a id="record-mms-native-features-audit-不透明像素预通道与浮点合成"></a>
<a id="record-mms-native-features-audit-七已交付增强mmv-插件加载与交接宿主侧"></a>
<a id="record-mms-native-features-audit-八建库快照已有的自定义功能来源不明待原版核验"></a>
<a id="record-mms-native-features-audit-九纯移植兼容修复不计入增强"></a>
<a id="record-mms-native-features-audit-十未合入或已停用的实验"></a>
<a id="record-mms-native-features-audit-十一工作树待集成未交付"></a>
<a id="record-mms-native-features-audit-十二未核实项与证据缺口"></a>
<a id="record-mms-native-features-audit-十三本次审计发现的既有文档差异"></a>

本页用于区分原生功能、完整包扩展和可选插件。操作说明以[功能导航](../../README.md)和相应指南为准。

## 原生动画与场景

PMD、PMX 模型用于骨骼、IK、表情动画；VMD 交换动作，VPD 交换姿态，PMM 保存工程。模型编辑页支持 PMD，不能据此认为可编辑任意 PMX 模型结构。相机、灯光、重力、自阴影、附件和物理沿用原生轨道。

名称栏、3D 点选、关键帧选择、复制粘贴、登记和撤销通过对应的编辑状态衔接。跨模型粘贴按名称和同名项顺序对应轨道，切换前台或载入文件不应产生额外登记或删除操作。详见[编辑机制](../animation/editing.md)和[界面操作](../interface/controls.md)。

## 完整包扩展

| 功能 | 说明 |
| --- | --- |
| 曲线编辑器 | 骨骼九通道、表情、附件和场景曲线；支持编辑手柄、比较、插键与通道烘焙 |
| 骨骼缩放 | 整体与分轴缩放，关联刚体与关节按对应变换求值 |
| 帧率修正与序列散布 | 变换工程时间或按规则排列选中表情片段 |
| 负数帧 | PMM 扩展保存负帧编辑；播放、原生曲线及 VMD 的边界见[动画指南](../../guides/animation.md) |
| 物理与网格烘焙 | 将运动或几何写为可保存的结果，也可导出 Alembic |
| 画面控制 | 预览比例、FXAA/TAA/SMAA、透明排序与扩展自阴影 |
| 运算器 | 可选 GPU 顶点计算、自动选择和 DX12 预览；不支持时保留 DX9/CPU 路径 |
| 媒体 | 现代音视频导入、MP4/H.264 和透明 MOV 输出，依赖系统组件 |
| 工程扩展 | 按需扩容、完整名称与附加曲线数据；旧程序仅识别其标准部分 |

功能能力不代表旧格式能保存全部内容，见[文件兼容性](../../guides/files.md)。

## 最小包

`mmd932` 构建档保留原生功能及共享的输入、编码、载入、备份、MME 和部分预览计算修复。扩展曲线、骨骼缩放、新抗锯齿、透明排序、现代媒体及额外烘焙不编入。保存只写标准数据；具体例外和限制以[最小构建档](../../reference/minimal-build.md)为准。

## 外部插件与设计

MME 和 MMV4MMS 均需另行安装，不能把宿主接口等同于插件或算法已随包提供。MMV4MMS 使用独立场景接管，不能与 MME 代理同时启用。分层烘焙尚未接通。main 的菜单接口与 MMV 接口各自维护；通用 ABI 插件宿主属于独立开发范围。PMM 新分节布局尚未接入实际读写。

性能、画质和原版行为的结论须绑定具体素材及实现路径，不能从功能存在推断固定加速比或任意工程无损兼容。
