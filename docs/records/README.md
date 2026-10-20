# 专题记录

这些资料按功能归并，保留原实现、失败、限制及验证条件。原始记录名和日期只用于追溯；使用说明见[文档导航](../README.md)。本地证据路径不保证随仓库提供。

## 画面与渲染

- [抗锯齿实现与验证记录](rendering/anti-aliasing.md)
- [材质、光照与描边记录](rendering/materials.md)
- [阴影实现与验证记录](rendering/shadows.md)
- [透明排序与合成记录](rendering/transparency.md)
- [背景媒体导入与操作记录](rendering/background-media.md)

## 性能与设备

- [场景性能测量记录](performance/benchmarks.md)
- [CPU 等待与查找优化记录](performance/cpu.md)
- [运算器与 GPU 蒙皮记录](performance/devices.md)
- [预览缩放与模型缓存记录](performance/preview.md)
- [启动与资源初始化记录](performance/startup.md)

## 动画与物理

- [相机父骨骼共同历史迁移原计划（2026-10-21）](abi-camera-parent-bone-plan-2026-10-21.md)
- [相机父骨骼共同历史实现与验收（2026-10-21）](abi-camera-parent-bone-history-2026-10-21.md)
- [工程地面开关共同历史（2026-10-21）](abi-floor-history-2026-10-21.md)
- [黑白背景共同历史（2026-10-21）](abi-black-background-history-2026-10-21.md)
- [模型物理资源、私有编辑与共同历史（2026-10-21）](abi-model-physics-history-2026-10-21.md)
- [烘焙与导出验证记录](animation/baking.md)
- [骨骼、曲线与镜头编辑记录](animation/editing.md)
- [物理状态与关节验证记录](animation/physics.md)

## 载入、保存与格式

- [模型载入与格式验证记录](formats/models.md)
- [工程、动作与容量验证记录](formats/projects.md)

## 特效兼容

- [MME 兼容与分配记录](effects/compatibility.md)

## 界面与语言

- [视口与编辑控件记录](interface/controls.md)
- [语言、名称与界面编码记录](interface/languages.md)

## 开发与维护

- [菜单状态刷新、原生身份及跟踪生命周期（2026-10-21）](abi-menu-state-2026-10-21.md)
- [已挂载面板状态刷新及故障恢复（2026-10-21）](abi-panel-state-2026-10-21.md)
- [曲线来源回调会话保护（2026-10-21）](abi-curve-source-lifetime-2026-10-21.md)
- [曲线写入回调与撤销失败恢复（2026-10-21）](abi-curve-write-lifetime-2026-10-21.md)
- [曲线选择回调与稳定时钟（2026-10-21）](abi-curve-selection-lifetime-2026-10-21.md)
- [曲线比较、相邻控制开关及筛选发布（2026-10-21）](abi-curve-comparison-lifetime-2026-10-21.md)
- [属性区插件扩展行与验证边界（2026-10-21）](abi-property-mount-2026-10-21.md)
- [ABI 分支合入 main 功能记录（2026-10-17）](abi-main-merge-2026-10-17.md)
- [main 源码审查与本地验收记录（2026-10-17）](main-source-review-2026-10-17.md)
- [代码审查与缺陷记录](development/audits.md)
- [历史功能范围审计](development/features.md)
- [跨模块集成与兼容记录](development/integration.md)
- [原版二进制核验记录](development/reference-audit.md)
- [用户问题与回归记录](development/user-reports.md)
- [工作区快照与清理记录](development/workspace.md)

## 可选扩展的独立资料

- [可选网格插件 FX 参考](extensions/effects.md)
- [可选网格插件宿主与渲染记录](extensions/host.md)
- [可选网格插件界面与分配记录](extensions/interface.md)
- [可选网格插件参数界面参考](extensions/parameters.md)
- [可选网格插件性能记录](extensions/performance.md)
- [可选网格插件形变与物理参考](extensions/physics.md)
- [可选网格插件安装与存档参考](extensions/usage.md)

## 发布

[发布说明与构建记录](releases.md)保留各版本的原始说明。实际可下载版本见仓库首页的发布链接。

## 原始测量数据

下列数据对应专题记录中的特定样本与版本，不代表当前构建的通用性能。

- [pmx-loading](formats/data/pmx-loading.json)
- [mme-frame-performance](performance/data/mme-frame-performance.json)
- [mme-preview-performance](performance/data/mme-preview-performance.json)
- [model-vertex-cache](performance/data/model-vertex-cache.json)
- [performance-d3dx](performance/data/performance-d3dx.json)
- [performance-phases](performance/data/performance-phases.json)
- [performance-pmd](performance/data/performance-pmd.json)
- [performance-sort](performance/data/performance-sort.json)
- [performance](performance/data/performance.json)
- [pmm-frame-performance](performance/data/pmm-frame-performance.json)
- [reverse-performance-evidence](performance/data/reverse-performance-evidence.txt)
- [stage-frame-performance](performance/data/stage-frame-performance.json)
- [stage-no-ray-gpu-timing](performance/data/stage-no-ray-gpu-timing.json)
- [stage-no-ray-performance](performance/data/stage-no-ray-performance.json)
