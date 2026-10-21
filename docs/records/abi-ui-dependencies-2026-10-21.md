# 显式 UI 依赖刷新（2026-10-21）

已实现同插件显式依赖图。`mms.ui.refresh.dependencies` 注册原 UI 目标之间的边；`mms.ui.refresh` 的 DEPENDENTS 范围使用原对象、队列、票据、来源版本集合和四个阶段。C++17 SDK 包装同一 C 表，不维护另一套 UI。

接口和开发规则见[依赖刷新参考](../../sdk/mms/refresh-dependencies.md)。该批只实现显式图，来源发布的自动订阅、插件量测提供者和动画时钟仍未实现。

## 行为与边界

图只连接精确插件客户端拥有的已发布目标。自环、重复边和循环被拒绝；单插件最多 512 条边，全宿主 4096 条，每请求最多 256 个可达目标。先完成分配再发布，失败保留输出、旧图修订和待执行票据。

DEPENDENTS 排除种子及不可达目标，菱形路径只处理共同下游一次。整组依次执行 DATA、STATE、一次保留布局、REPAINT，各阶段内按拓扑顺序处理。DATA 原子捕获绑定版本集合，并继续支持未绑定 INPUT 的消费时属性读回。图、绑定、属性或状态提供者替换会使对应旧请求失效。

各目标可处在不同公开 UI 位置，因此只接受整目标损伤，不把种子坐标中的局部矩形用于其他窗口。布局从实际支持 LAYOUT 的成员选择入口，仍使用既有保留树后端。混合菜单和窗口的组在菜单跟踪期间整体等待；退出后恢复，不反复唤醒重试。其他窗口请求可继续执行。

修改依赖图在 UI 线程进行。后台请求只读宿主保存的图和目标快照。状态、数据、布局回调及其因果任务中不能改图或递归提交求值型刷新。端点重建、注销、工程切换和停止清除相应边；移动、缩放不清边。刷新不新增工程写入、场景求值、播放头改变、历史记录或布局偏好保存。

## 运行证据

证据目录为 `build/verification/abi-ui-dependencies-20261021`。窗口与菜单测试串行继承 Default 桌面；启动时请求隐藏，后续显示由夹具控制。只使用自有窗口和局部消息，不发送全局输入。

| 检查 | 结果 |
| --- | --- |
| 真实队列、SDK 和插件 DLL | 完整配置 5,602 项、原始兼容配置 5,601 项，退出 0。检查数包含等待过程的查询次数。[完整报告](../../build/verification/abi-ui-dependencies-20261021/dependencies-full-03/report.json)、[原始兼容报告](../../build/verification/abi-ui-dependencies-20261021/dependencies-original-03/report.json)。 |
| 实际分配失败 | 命中 56 字节 owner 节点、112 字节 edge 节点及首个循环检查缓冲分配。验证 edge 插入失败不会留下空 owner、发布半条边或使旧票据过期。节点大小仅用于当前 MSVC 测试的故障注入，不是公共 ABI。 |
| 实际菜单跟踪 | 使用现有 NativeUi 和 `TrackPopupMenu` 夹具，检查混合组整组延后、Pending 不反复重试、退出时唤醒，以及菜单种子到窗口成员的布局入口。 |
| 真实 DLL 停止 | 插件通过公开协商建立 A→B、A→C、B→C，D 不相连。两个后台票据合并后只查询 B、C 一次；停止清图，持有票据时不卸载，工作线程逐个释放后才 Destroy 和卸载。 |
| ABI 与相邻回归 | 两套 ABI 契约各 24,546 项通过；刷新、真实 DATA DLL 和真实 LAYOUT DLL 专项也全部退出 0。[完整报告](../../build/verification/abi-ui-dependencies-20261021/neighbors-full-03/report.json)、[原始兼容报告](../../build/verification/abi-ui-dependencies-20261021/neighbors-original-03/report.json)。 |
| 最终源码构建 | 两套产品和 ABI 测试构建成功，最终 DLL 专项分别 5,601、5,603 项，退出 0。[完整配置](../../build/verification/abi-ui-dependencies-20261021/final-full-04/report.json)、[原始兼容配置](../../build/verification/abi-ui-dependencies-20261021/final-original-04/report.json)。 |
| 产品兼容边界 | 两套产品为 x64、子系统 6.1，直接及延迟导入表与前批一致，正式 release 哈希保持不变。[PE 清单](../../build/verification/abi-ui-dependencies-20261021/pe-products-final-04.json)。 |

首轮运行在跨位置区域断言失败，见 [dependencies-full-02](../../build/verification/abi-ui-dependencies-20261021/dependencies-full-02/report.json)。测试原先错误要求从 A 出发也刷新只共享上游的 B；按准确可达集合修正为 A、B 均无损伤，分隔线和跨位置面板有损伤，未修改生产算法。原始程序与源码保留于 `candidate-02`。

首次两套测试编译均遇到 MSVC COFF 节数上限。仅为测试目标启用 `/bigobj` 后构建成功；产品编译选项和运行时系统要求不变。

本机结果不能替代 Win7 SP1 x64、CPU 基础路径、AMD、真实 IME 或多 DPI 验收。main 和正式 release 未修改。
