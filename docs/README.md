# MMS 文档

按要完成的工作选择文档。下载地址、完整包与最小包的区别，以及三语项目介绍见[仓库首页](../README.md)。下列使用说明对应当前源码；实际发布包的功能范围以发布说明为准。

## 使用 MMS

| 要做什么 | 从这里开始 |
| --- | --- |
| 编辑骨骼、表情、镜头，调整曲线或烘焙动作 | [动画编辑](guides/animation.md) |
| 复制表情片段并随机安排间隔和速度 | [选中帧序列散布](guides/sequence-scatter.md) |
| 移动或缩放背景图片与视频 | [背景媒体操作](guides/background-media.md) |
| 设置抗锯齿、预览尺寸、透明排序和阴影 | [画面设置](guides/rendering.md) |
| 使用 MME、分配效果和自动重载 FX | [MME 特效](guides/effects.md) |
| 选择运算器，判断缩小预览为何没有提速 | [性能与设备](guides/performance.md) |
| 保存工程、交换动作，处理名称和容量限制 | [文件与兼容性](guides/files.md) |
| 输出网格或烘焙工程缓存 | [网格导出与烘焙](guides/mesh-export.md) |

## 查询格式与构建范围

| 参考 | 内容 |
| --- | --- |
| [PMM 扩展](reference/pmm-extensions.md) | 标准数据、尾部记录、自动扩容、Unicode 名称及版本选择 |
| [新版 PMM 分节设计](reference/pmm-0003-sections.md) | 尚未接入工程读写的设计稿，按类别保存名称、路径与曲线 |
| [旧版 MMD 限制实测](reference/legacy-mmd-limits.md) | MMD 9.26 的名称字节边界、路径损失及保存后重开结果 |
| [VPD 缩放扩展](reference/vpd-scale.md) | 姿态文件的骨骼缩放记录 |
| [最小构建档](reference/minimal-build.md) | 保留功能、保存限制、构建方法和互导验证边界 |

## 开发与验证

[开发入口](development/README.md)汇集构建、测试、性能测量和插件接口资料。[专题记录](records/README.md)保留实现依据、已知问题和测量结果，按功能查阅。

使用指南说明当前操作，参考文档说明数据约定，专题记录说明特定版本做过什么验证。历史记录中的日期用于确定适用范围，不作为主要导航。维护方式见[文档组织规则](development/documentation.md)。
