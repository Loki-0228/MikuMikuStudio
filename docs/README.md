# MMS 功能文档

文档按功能说明 main 分支已实现的行为。下载和完整包、最小包的差异见[仓库首页](../README.md)；发布包以[最新发布页面](https://github.com/Loki-0228/MikuMikuStudio/releases/latest)随附的说明为准。

## 动画与场景

| 功能 | 操作说明 | 实现与限制 |
| --- | --- | --- |
| 骨骼、表情、镜头与关键帧 | [动画编辑](guides/animation.md)、[曲线编辑器](guides/animation-curves.md) | [轨道与求值](records/animation/editing.md) |
| 骨骼缩放与刚体联动 | [骨骼缩放](guides/bone-scaling.md) | [物理状态](records/animation/physics.md) |
| 工程帧率转换 | [根据帧率修正](guides/frame-rate.md) | [动画编辑机制](records/animation/editing.md) |
| 表情片段随机排列 | [选中帧序列散布](guides/sequence-scatter.md) | 范围、随机种子和提交规则见操作页 |
| 物理运动与通道烘焙 | [物理烧录](guides/physics-baking.md) | [烘焙与撤销](records/animation/baking.md) |
| 几何缓存与 ABC 输出 | [网格导出与烘焙](guides/mesh-export.md) | [烘焙来源与边界](records/animation/baking.md) |
| 图片与视频背景 | [背景媒体操作](guides/background-media.md) | [媒体导入与输出](records/rendering/background-media.md) |

## 画面与性能

| 功能 | 操作说明 | 实现与限制 |
| --- | --- | --- |
| 抗锯齿与预览比例 | [画面设置](guides/rendering.md) | [抗锯齿](records/rendering/anti-aliasing.md)、[预览缓存](records/performance/preview.md) |
| 透明排序 | [画面设置](guides/rendering.md#设置透明排序与阴影) | [排序程度与范围](records/rendering/transparency.md) |
| 自阴影精度与滤波 | [画面设置](guides/rendering.md#设置透明排序与阴影) | [阴影覆盖与采样](records/rendering/shadows.md) |
| 材质、光照与描边 | [画面设置](guides/rendering.md) | [材质与渲染](records/rendering/materials.md) |
| MME 安装、分配和重载 | [MME 特效](guides/effects.md) | [MME 兼容](records/effects/compatibility.md) |
| 运算器与性能排查 | [性能与设备](guides/performance.md) | [设备与回退](records/performance/devices.md)、[CPU 与线程](records/performance/cpu.md) |
| 启动、资源缓存与清理 | [本地数据与缓存](reference/local-data-storage.md) | [资源准备与复用](records/performance/startup.md) |

## 文件与扩展

| 功能 | 文档 |
| --- | --- |
| 工程、动作、姿态、备份与兼容副本 | [文件与兼容性](guides/files.md) |
| PMM 数据、名称、容量与负数帧 | [PMM 扩展](reference/pmm-extensions.md)、[工程格式](records/formats/projects.md) |
| PMD、PMX 与名称编码 | [模型载入](records/formats/models.md)、[旧版 MMD 格式边界](reference/legacy-mmd-limits.md) |
| VPD 骨骼缩放 | [VPD 扩展](reference/vpd-scale.md) |
| 最小包保留范围 | [最小构建档](reference/minimal-build.md) |
| 可选 MMV4MMS 插件 | [安装与存档](records/extensions/usage.md)、[宿主接口](records/extensions/host.md)、[FX 与参数](records/extensions/effects.md) |

## 界面与开发

[界面操作](records/interface/controls.md)和[语言与名称](records/interface/languages.md)说明输入、选择、布局与编码规则。[开发入口](development/README.md)提供构建、测试、性能测量和维护方法。[功能参考目录](records/README.md)列出全部专题。

完整包、最小包及外部插件的能力分别说明。当前不支持的新容器标记见 [PMM 格式边界](reference/pmm-0003-sections.md)。文档维护遵循[组织规则](development/documentation.md)。
