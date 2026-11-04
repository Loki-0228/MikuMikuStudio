# 开发入口

| 工作 | 文档 |
| --- | --- |
| 配置依赖并构建程序 | [源码构建](../../README.md#从源码构建)、[最小构建档](../reference/minimal-build.md#构建) |
| 固定 Win7 x64 基础配置 | [Win7 构建与验收边界](win7-build.md) |
| 定位源码、产物及本地数据 | [工作区布局](workspace.md)、[本地数据](../reference/local-data-storage.md) |
| 选择回归与集成检查 | [测试与验收](testing.md)、[运行与恢复机制](runtime-reliability.md) |
| 比较帧耗时与设备表现 | [帧基准](benchmarks.md)、[测量口径](../records/performance/benchmarks.md) |
| 制作完整包和最小包 | [整理发布包](prerelease.md) |
| 注册原生插件菜单 | [菜单接口](plugin-menus.md) |
| 对接可选 MMV 插件 | [宿主接口](../records/extensions/host.md)、[SDK](../../sdk/mmv4mms/README.md) |
| 修改文件格式 | [PMM 扩展](../reference/pmm-extensions.md)、[名称与标识](../records/development/mds-naming-audit.md) |
| 审查边界与跨模块行为 | [代码约束](../records/development/audits.md)、[集成边界](../records/development/integration.md) |
| 核对原版行为 | [二进制核验方法](../records/development/reference-audit.md) |
| 更新文档 | [组织规则](documentation.md) |

实现细节按[功能参考目录](../records/README.md)查阅。未接入的设计不属于 main 可用功能；平台支持范围须由对应系统及设备的实际运行验证。
