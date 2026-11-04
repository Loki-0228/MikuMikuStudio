# 功能实现参考

这些页面说明 main 的当前功能、实现机制与边界。操作步骤见[使用文档](../README.md)，目录名 `records` 保留以兼容已有引用；正文按功能维护，不累积开发日志。

## 画面与媒体

- [抗锯齿与描边平滑](rendering/anti-aliasing.md)
- [材质、光照与描边](rendering/materials.md)
- [自阴影覆盖、精度与滤波](rendering/shadows.md)
- [透明排序与合成](rendering/transparency.md)
- [背景媒体与音视频](rendering/background-media.md)
- [MME 对象、分配与兼容](effects/compatibility.md)

## 动画与物理

- [动画轨道与编辑机制](animation/editing.md)
- [物理状态、缩放与关节](animation/physics.md)
- [通道烘焙与网格缓存](animation/baking.md)

## 文件与界面

- [模型载入与资源准备](formats/models.md)
- [工程与动作读写](formats/projects.md)
- [界面、输入与编辑操作](interface/controls.md)
- [界面语言、名称与路径](interface/languages.md)
- [本地设置、备份与缓存](../reference/local-data-storage.md)

## 性能与设备

- [运算器选择与回退](performance/devices.md)
- [预览比例与模型缓存](performance/preview.md)
- [启动与资源准备](performance/startup.md)
- [CPU、线程等待与查找](performance/cpu.md)
- [性能测量口径与原始数据](performance/benchmarks.md)

## 可选 MMV4MMS 插件

- [安装、分配与存档](extensions/usage.md)
- [宿主与渲染接口](extensions/host.md)
- [分配与参数界面](extensions/interface.md)
- [FX 作者接口](extensions/effects.md)
- [外置参数描述 UIFX](extensions/parameters.md)
- [原生形变与物理模块](extensions/physics.md)
- [插件性能与有效性](extensions/performance.md)

外部插件能力由实际安装版本决定。主程序包不附带插件；当前 main 不加载旧 `MMV.dll`，MMV4MMS 与 MME 代理不能同时启用，分层烘焙尚未接通。

## 开发与发布

- [功能与构建档范围](development/features.md)
- [代码边界与审查要点](development/audits.md)
- [宿主、插件与模块集成](development/integration.md)
- [输入与编辑问题排查](development/user-reports.md)
- [命名与兼容标识](development/mds-naming-audit.md)
- [原版行为核验方法](development/reference-audit.md)
- [工作区与产物管理](development/workspace.md)
- [运行、备份与故障恢复](../development/runtime-reliability.md)
- [安装与发布包说明](releases.md)

构建、测试、打包及维护方法统一从[开发入口](../development/README.md)查阅。历史版本说明在 [GitHub Releases](https://github.com/Loki-0228/MikuMikuStudio/releases)与 Git 历史中保留。
