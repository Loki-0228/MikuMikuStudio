# 插件 ABI 实现与验收记录

更新：2026-10-17。分支 `codex/plugin-abi` 从 `42d4a71` 建立。依据工作区《MMS 插件实施方案与 ABI 参考》《MMS 插件开发文档》《MMS 界面布局与重绘设计》实施。完整 P0–P6 尚未完成。

状态分为 **未实现**、**已实现未验证**、**本机已验证**、**缺其他系统/硬件环境**。代码、本机结果和平台结果分别列出。

## 逐项状态

| 工作包 | 项目 | 状态 | 范围与证据 |
| --- | --- | --- | --- |
| P0 | 固定 MSVC 19.44.35229、SDK 10.0.19041、Win7 宏及子系统 6.1 | 本机已验证 | profile、recipe lock、CMake preset；完整 MMS 和 ABI 测试编译成功。 |
| P0 | C11/C++17 数据布局、独立插件 /MT、宿主库 /MD | 本机已验证 | 编译与真实 DLL 加载通过。Conan 采用兼容的 193 二进制缓存，具体 package ID/revision 在本机 dependencies JSON。 |
| P0 | 普通及延迟 PE 导入清单 | 本机已验证 | 宿主、两个测试插件及测试 EXE；无 DX12、DXGI、d3dcompiler_47、CUDA、NVAPI 直接导入。并非逐 API 的 Win7 兼容证明。 |
| P0 | 固定 CRT/UCRT/VCOMP/DirectX 部署闭包 | 未实现 | 本机新版系统运行库不能代替 Win7 部署组合验收。 |
| P0 | Media Foundation 能力回退 | 已实现未验证 | 可选 COM 查询、旧 Sink Writer 编码器查询；高级 RGB32 协商失败重开基本处理 reader。已编译，尚无 Win7 媒体结果。 |
| P0 | Win7 SP1、Win10、AMD CPU/GPU 真机 | 缺其他系统/硬件环境 | 不以本机结果推定通过。 |
| P1 | 目录深度、清单、稳定 ID、依赖拓扑 | 本机已验证 | 根目录和中文目录；默认深度 2；包边界、重复 ID、环及版本错误。 |
| P1 | 绝对路径加载、原生依赖预检、文件锁及哈希 | 本机已验证 | 安全搜索及回退分支均在本机验证；旧 Win7 加载器另验。 |
| P1 | 生命周期、停止、UI 队列及失败取消 | 本机已验证 | create/start 失败、null 实例任务、BUSY、运行中取消；退休失败时保留实例和 DLL。 |
| P1 | 菜单、工具栏、EXE 改名 | 本机已验证 | 隔离隐藏启动真实 MMS，原名和中文名下调用、关闭通过。 |
| P1 | 命令、只读属性、更新、快照租约 | 本机已验证 | 所有权、参数范围/选项、上下文不可替换、旧快照及线程边界。 |
| P1 | 全部业务事件通知 | 部分实现 | UI_READY/UI_CLOSING；工程打开、手动保存、重置事件已接入。其他业务来源仍待接入。 |
| P2 | Surface 工厂和生命周期 | 本机已验证 | 单/多视图、失败重试、错误父窗口、隐藏尺寸/DPI、换父重建、focus、停止和一次销毁。 |
| P2 | 工作区双页签实际挂载 | 本机已验证 | 真实 MMS 原名/中文名，切换、隐藏复用、焦点回调、关闭各销毁一次。 |
| P2 | 工具栏自动布局树 | 本机已验证 | 行列、换行、网格、伸缩、隐藏占位、嵌套高度及缩放往返。 |
| P2 | 布局事务和轻量提示 | 本机已验证 | MOVE beforeId、创建/删除空组、显隐、尺寸、快捷键、恢复；默认和用户覆盖分离。 |
| P2 | 几何快照 | 本机已验证 | HWND 应用后发布；UI/几何版本分开；未就绪 NOT_READY，旧租约不变。 |
| P2 | 可写属性、声明式 INPUT | 未实现 | 依赖 P3 工程事务及撤销，当前返回 NOT_READY。 |
| P2 | 左右侧栏、属性区、时间线、状态栏挂载 | 未实现 | 不枚举空壳挂载点。 |
| P2 | DOCK、命令溢出菜单 | 未实现 | 返回 UNSUPPORTED。 |
| P2 | 本体菜单、控件、业务命令全部语义登记 | 未实现 | 新注册表目前覆盖新 ABI 插件。 |
| P2 | 布局持久化、缺失插件保留 | 本机已验证 | 用户覆盖按稳定 ID 保存到用户目录，缺失插件和父容器保留配置；重启、关闭事件提交及 EXE 改名恢复通过。 |
| P2 | 布局撤销历史 | 未实现 | 当前保存布局，不记录布局 undo/redo。 |
| P2a | 可选布局/绘制面积/帧驱动/场景/物理/设备计数 | 本机已验证 | `MMS_UI_METRICS` 输出 JSONL；受控损伤矩形触发真实 WM_PAINT 和 NM_CUSTOMDRAW，纯 UI 的 scene、effectBegin、present、reset 增量均为 0。effectBegin 是外层代理 BeginScene 入口，不是 MME 内部阶段计数。 |
| P2a | MME 内部阶段计数和 ABBA 性能基线 | 未实现 | 不宣称性能改善。 |
| P2b | 局部 GDI 缓冲、批量 HWND 应用及失败恢复 | 本机已验证 | 多父窗口分批、验证实际坐标、失败回滚、缓冲复用/初始化/尺寸键；接入新工具栏及扩展区。 |
| P2b | 本体既有参数区迁移与全区域缓冲 | 未实现 | 新扩展区通过不替代旧区域迁移。 |
| P2c | 独立更新协调器逻辑 | 本机已验证 | 虚拟时钟合并、重入、代次、Retry、Failed/异常、隐藏恢复；新几何保留未消费工作。 |
| P2c | 统一更新协调器、WM_PAINT 解耦、暴露恢复 | 本机已验证 | 增强构建从主泵及原生尺寸循环的安全点调用帧驱动；WM_PAINT 只请求重绘。QPC 单调时钟、请求合并、失败保留、隐藏与模态退避、回调及帧重入保护。原始兼容构建保留旧路径。 |
| P2c | 停止恢复、播放、媒体、设备及导出 | 本机已验证 | 动态刚体验证停止重绘不推进物理，播放仍推进；12 项设备与录制恢复、停止视频更新、图片序列及时间抗锯齿通过。见下方夹具范围。 |
| P2c | 真实 MME 内部状态、真实桌面遮挡及多 DPI | 未实现 | 当前真实主路径使用内置效果及隐藏窗口；旧 MME 内部状态保持和桌面视觉效果尚未验收。 |
| P2d | 全区域统一几何、绘制和命中迁移 | 未实现 | 新扩展区接入不等于全部本体区域完成。 |
| P2e | 可选实时拖动 | 未实现 | 保留原有松手提交默认模式。 |
| P2f | 独立表单及模拟视口宿主 | 本机已验证 | 两个独立 CMake 项目构建及隐藏自检 exit 0，无 MMS/图形设备依赖。 |
| P2f | 两个示例的真实输入法、多 DPI、视觉交互 | 已实现未验证 | 自检不替代实际桌面交互验收。 |
| P2 | 输入法组合态、Tab、辅助功能和真实多 DPI | 已实现未验证 | 保留原生控件；隐藏窗口测试不替代完整交互验收。 |
| P3 | scene 对象及绑定骨骼快照、稳定身份 | 本机已验证 | `mms.scene` 已发布：阶段 0 的对象与骨骼。独立 ID、运行时代次、任意线程读取/释放、不可变租约；真实 PMM 保存重开保留对象和骨骼 ID，旧引用返回 STALE。 |
| P3 | 绑定网格、权重、材质、纹理路径 | 本机已验证 | PMX 2.0、PMD、原始 X 附件；稳定元素 ID、嵌套租约、对象筛选、无效拓扑拒绝、内容变化修订及真实 PMM 往返。纹理尺寸/格式未知，运行时 QDEF 记录通过不等于 PMX 2.1 文件导入通过。 |
| P3 | 绑定刚体、形状、关节及 Morph 定义 | 本机已验证 | 球/盒/胶囊、六自由度弹簧、组/顶点/骨骼/UV/材质 Morph；刚体和关节矩阵与原生加载器、Bullet 约束独立比较，身份经真实 PMM 往返保留。 |
| P3 | 当前场景查询 | 本机已验证 | CURRENT_TIME 标志支持非零编辑帧查询，忽略 time，不移动播放头；仍可用 flags=0 检查指定时间。 |
| P3 | 求值阶段及未支持的定义类型 | 未实现 | 阶段 1–4、翻转/冲量 Morph、其他关节、复合形状及软体。绑定配置不能替代物理后状态。 |
| P3 | edit 两阶段事务内核 | 本机已验证 | prepare/join/commit/cancel、修订检查、serializer 代次、撤销字节深复制、失败取消和历史预留。测试使用宿主夹具，见[原生接入边界](PLUGIN_EDIT_INTEGRATION.md)。 |
| P3 | 原生模型历史准备与发布 | 本机已验证 | 类型 1–7、链式操作和回绕，缩放、自由曲线及 Morph 层；准备失败不写状态，发布不分配。按钮、快捷键及直接撤销入口已接入。 |
| P3 | 共同历史内核与插件撤销粒度 | 本机已验证 | `SharedHistory` 实现 `EditHost`；原生和插件同队列、同事务，按提交分步或合并撤销，失败保留游标及重做分支、资源延后回收。 |
| P3 | 对象存在身份与删除绑定恢复 | 本机已验证 | 稳定 ID、恢复后新代次；相机、附件、外部亲骨骼及显示顺序恢复。注册关键帧池正常扩容保留身份，恢复时重新定位字段；池替换、借用数组移动仍拒绝。 |
| P3 | 模型及附件资源保留组件 | 本机已验证 | `RetainedSceneObject` 保留原生对象、关键帧、附件曲线、COM 资源及 Bullet 对象。真实隐藏宿主覆盖源文件离线、DX9 重置、导入与删除往返；调用内部共同历史，菜单入口尚未接入。 |
| P3 | 整批对象导入与删除候选 | 本机已验证 | 多对象共用一份物理候选及绑定记录；两模型与两附件作为一条记录往返，恢复原资源及顺序。同种对象槽位冲突拒绝，跨物理 owner 关节仍不支持。 |
| P3 | Bullet 移出和恢复候选 | 本机已验证 | 保留刚体、约束及代理句柄；移出后不参与模拟、碰撞和射线查询。恢复重建静止重叠，保留其他碰撞算法；发布不分配，旧接触在解锁前清理。外部 action、碰撞回调及未保留的关联约束拒绝准备。 |
| P3 | 应用历史寿命与工程切换 | 本机已验证 | 应用持有共同日志；准备工程可取消，PMM 替换前释放旧历史及原生候选；新建和关闭时在世界销毁前回收。混合编辑未取消时拒绝重置。 |
| P3 | 独立 type 1 姿态记录 | 本机已验证 | 100 次覆盖旧环后往返；按平移分量、旋转、dirty 与缩放差异恢复，保留未改字段和播放头。其他类型的独立记录仍待实现。 |
| P3 | 原生保留资源计量 | 部分实现 | 实际池容量、嵌套数组、侧表与 DX9 查询；批内分配地址和 COM 身份去重。文本、旧撤销、物理及外部后端缺项显式标记，尚未用于自动准入预算。 |
| P3 | 模型加载后的 UI 延后机制 | 本机已验证 | PMD、PMX 成功尾部支持按窗口嵌套延后；取消或异常不刷新，提交成功后由调用方显式刷新。实际导入菜单尚未启用。 |
| P3 | 已执行姿态的历史预留与采用 | 本机已验证 | 修改前预留节点和预算，修改后验证反向候选，提交只入历史；失败先回滚再解锁。数字输入、重置、粘贴、镜像粘贴及两窗口轴复位仅在内部绑定下接入，普通宿主保留旧路径。 |
| P3 | 受控共同历史命令 | 本机已验证 | 撤销按钮及 Ctrl+Z、重做按钮及 Ctrl+X 不依赖当前模型或相机模式。空历史、BUSY、准备失败不回退旧环；文本焦点及播放录制检查保持。仅内部绑定启用。 |
| P3 | 骨骼关键帧池身份及可取消扩容 | 本机已验证 | 扩容保持身份，销毁重建取得新身份；准备不改当前分配，发布保留旧块，回滚恢复指针/容量/访问标记，接受后才释放旧自有块。借用池不释放原数组。 |
| P3 | 独立缩放及自由曲线作者快照 | 本机已验证 | 捕获、克隆、私有修改及无分配交换；保留旧 30 槽环。缩放不覆盖姿态；自由曲线保留 nativeHash。含空存储取消、过期拒绝和同值双交换保护。 |
| P3 | 当前帧选中骨骼登记 | 本机已验证 | 按钮 500 显式绑定下支持插入、覆写、自动插值、缩放和跨模型共同撤销；私有计划与原子发布不写旧环。物理开关变化拒绝；普通宿主仍用旧路径。 |
| P3 | 模型三族删键与混合范围缩放 | 本机已验证 | 显式绑定下将骨骼、表情、显示／IK／外亲键合为一条历史；范围对话框支持 7 种勾选组合，保留旧槽序覆盖、名称匹配与插值规则，共享最大帧、游标和标记只发布一次。空操作和整笔失败保留 redo。 |
| P3 | 删键及范围操作后的局部姿态求值 | 本机已验证 | 每个方向按当前帧采样受影响的干净骨骼，保留未登记姿态；真实 Seek 位级对照、自由曲线、换帧跨模型撤销和无分配发布通过。物理模式切换和 IK 启动仍拒绝。 |
| P3 | 显示、IK 及外亲键删除与当前帧求值 | 本机已验证 | 深拷贝键数据，保持嵌套缓冲区所有权；每个方向重建可见性、IK 状态和外亲窗口。关联骨骼使用目标窗口重新求值，未登记姿态保留。旧 Seek 对照、实际按钮与插件撤销、扩容及失败检查通过。 |
| P3 | 表情键删除及原始曲线保留 | 本机已验证 | 私有目标键链与作者快照采样，不清除原始曲线及切线；受影响权重按当前帧重算。旧函数位级对照、曲线恢复、跨模型历史、池扩容及分配失败通过。 |
| P3 | 三族删键原子性与依赖校验 | 本机已验证 | 统一准备、统一过期校验、无分配发布；跨池别名、保护页小池替换、准备后新增关联骨骼、后序失败和当前帧变化均有回归。表情曲线在相机模式下及插件重做后同步刷新。 |
| P3 | Manager 显式编辑服务与真实插件混合历史 | 本机已验证 | 受控宿主附加同一 SharedHistory，sceneRevision 与 begin 校验一致；顶层命令及队列编辑、失败取消、发布通知、缓存表分离、模态泵刷新重试、PMM 往返及关闭通过。普通启动不启用。 |
| P3 | 对象列表与历史按钮刷新 | 本机已验证 | 967 项隐藏控件检查覆盖原生顺序、父骨骼、选择回退、语言、姿态不变及故障重试。实际导入、删除菜单接入仍待完成。 |
| P3 | 原生统一撤销及可写属性 | 未开放 | 部分姿态命令、骨骼登记、删键、范围缩放及真实插件共同撤销已受控接入；其余原生写入、全局编辑、完整资源计量和对象刷新仍待迁移。普通启动查询 `mms.edit` 仍返回 NOT_FOUND，可写属性仍为 NOT_READY。 |
| P3 | animation 轨道/键、层、片段、变速、烘焙和选择 | 未实现 | 服务返回 NOT_FOUND。 |
| P3 | storage serializer、工程数据和成对保存 | 本机已验证 | C ABI、两阶段加载、保存数据租约、缺失插件保留；PMM 与 JSON 配套哈希、恢复记录、旧对回退、自动备份轮换；真实 DLL 原名/中文 EXE 往返通过。 |
| P3 | 显式导入和扩展文件重新关联 | 未实现 | EMM/EMD 保持显式导入；不匹配的 sidecar 拒绝自动应用，保留原件。 |
| P4 | compute 两阶段求值、状态域及 CPU 协商 | 未实现 | 不以成功空表占位。 |
| P4 | binding、cache、bake、MMV 等价清单 | 未实现 | 依赖 P3；旧 MMV 继续原路径。 |
| P5 | render、post、DX9 帧/设备代次和资源租约 | 未实现 | 现有渲染路径通过不代表新 ABI 通过。 |
| P6 | 原创 MMSAdapter、独立 MMHack 兼容导出 | 未实现 | 先恢复精确原型、调用约定和寿命。 |
| P6 | legacy_mme 改名和子目录兼容 | 未实现 | 尚待适配和实际插件验收；未知旧插件仍属理论范围。 |

## 验证记录

日志位于本地 `build/`，不作为发行内容。探针只启动独立目录的隐藏 MMS，并使用独立 LOCALAPPDATA。

| 验证 | 结果 | 日志 |
| --- | --- | --- |
| Win7 目标完整 MMS 和测试构建 | 通过 | `plugin-abi-p2-host-build.log` |
| ABI、加载、生命周期、布局及面板契约 | 247 项通过（阶段提交 ee42e5b） | `plugin-abi-p2-tests.log` |
| 新增协调器和原生批量/缓冲契约 | 累计 296 项通过 | `plugin-abi-win32-tests.log` |
| 无 MMS 的原生表单和模拟视口 | 两个项目均 exit 0 | `native-form-test.log`、`viewport-host-test.log` |
| 真实 MMS 原名/中文名菜单、工具栏和关闭 | 两种名称通过；工具栏 32 像素 | `plugin-abi-win7-p2-host-smoke.log` |
| 真实 MMS 双面板页签 | 两种名称通过 | `plugin-abi-panels-host-smoke.log`，夹具 `abi-p2-panels-smoke-2` |
| 新缓冲与计数的真实 MMS 探针 | 原名/中文名通过；隐藏启动无 WM_PAINT，不作纯 UI 零增量证据 | `plugin-abi-buffer-host-smoke.log` |
| 统一调度实际帧路径 | 16 组均 exit 0；序列累计 1072 项断言通过 | `plugin-abi-scheduler-full-3.log`、`abi-scheduler-full-3/report.json` |
| 调度接入后的插件、双页签与改名 | 原名/中文名通过 | `plugin-abi-scheduler-host-smoke-2.log` |
| 调度接入后的 ABI 契约 | 通过 | `plugin-abi-scheduler-contract.log` |
| 场景租约、线程、代次、加载器停止契约 | 累计 333 项通过 | `plugin-abi-scene-contract.log` |
| 实际模型与 PMM 保存重开 | 绑定矩阵、ID 保留、代次变化、旧租约及删除后 STALE 通过 | `plugin-abi-scene-host-1.log`、`abi-scene-host-1/report.json` |
| 独立插件查询真实 scene 服务 | 原名/中文改名宿主，菜单和按钮各查询一次通过 | `plugin-abi-scene-plugin-1.log` |
| 绑定几何、材质、身份及坏数据 | PMX/PMD 两组均 exit 0，均包含附件；原始夹具在测试脚本中生成 | `plugin-abi-geometry-host-4.log`、`abi-geometry-host-4/report.json` |
| 几何接入后的插件及 Morph 回归 | 原名/中文名插件通过；ABI 契约和现有 Morph 测试通过 | `plugin-abi-geometry-plugin-1.log`、`plugin-abi-geometry-contract-final.log` |
| 绑定物理和 Morph 定义 | PMX 145 项、PMD 73 项 PASS；包括隐藏宿主初始化、附件、身份往返、无效引用及当前帧查询 | `plugin-abi-definitions-host-2.log`、`abi-definitions-host-2/report.json` |
| 定义接入后的 ABI、Morph、改名及旧骨骼探针 | ABI 累计 335 项通过；现有 Morph、两种 EXE 名称、原骨骼快照探针均通过 | `plugin-abi-definitions-contract.log`、`plugin-abi-definitions-plugin-1.log`、`plugin-abi-definitions-bind-1.log` |
| 当前 SDK 独立示例构建 | 按开发文档固定工具链构建成功，入口版本/长度/空输出检查通过 | `plugin-abi-sdk-configure.log`、`plugin-abi-sdk-build.log`、`plugin-abi-sdk-entry.log` |
| storage 候选、租约、所有权和保存恢复 | ABI 累计 418 项通过；文件替换边界中断、旧对回退、外部哈希冲突及超过 1 MiB 的记录往返 | `plugin-abi-storage-contract-2.log` |
| 真实插件数据和 PMM 往返 | 两种 EXE 名称、缺失插件数据、事件后场景可读、失败不发布、保存数据租约及退出卸载均通过 | `plugin-abi-storage-host-3.log`、`abi-storage-host-3/report.json` |
| 自动备份与既有场景回归 | 自动备份 118 项通过，包括配套扩展轮换；PMX/PMD 几何、定义和身份往返通过 | `plugin-abi-storage-autosave-2.log`、`plugin-abi-storage-geometry-1.log` |
| storage 完整构建与 PE 清单 | Win7 目标构建成功；宿主、新 serializer 测试 DLL 和 ABI 测试无新增可选图形/厂商直接导入 | `plugin-abi-storage-build-5.log`、`plugin-abi-storage-pe-inventory.json` |
| 两阶段编辑事务内核 | ABI 累计 538 项通过；跨插件参与、陈旧修订、代次更换、历史预留失败、撤销 schema 拒绝、停止取消及违规 commit 保护。原生历史仍未接入 | `plugin-abi-edit-contract-2.log` |
| 撤销恢复辅助及重入保护加入后的回归 | 真实插件、PMM 往返及原名/中文 EXE 均通过；自动备份 118 项通过 | `plugin-abi-edit-storage-host-2.log`、`plugin-abi-edit-autosave-1.log` |
| 原生模型历史恢复 | 10,578 项通过；逐分配故障、无分配发布、陈旧拒绝、类型 1–7、链式回绕、真实删除和散布记录、原生键与自由曲线一起恢复、实际撤销命令失败不改 dirty | `plugin-abi-native-history-contract-5.log` |
| 原生恢复接入后的既有回归 | 骨骼曲线、Morph 曲线、帧率转换、缩放及 VMD 往返、ABI 契约通过；物理烘焙、密集键播放和隐藏界面插值联动通过 | `plugin-abi-native-history-regressions-2.log`、`plugin-abi-native-history-physics-1.log`、`plugin-abi-native-history-ui-1.log` |
| 原生恢复接入后的真实工程 | 插件数据、PMM 往返、缺失插件保留、失败不发布、原名/中文 EXE 通过 | `plugin-abi-native-history-storage-1.log`、`abi-native-history-storage-1/report.json` |
| 原生恢复宿主构建及导入 | 宿主和相关测试重建成功，宿主仍为 x64、子系统 6.1，无新增必需的可选图形或厂商 DLL 导入；资源复用范围见下文 | `plugin-abi-native-history-build-10.log`、`plugin-abi-native-history-pe-inventory.json` |
| 共同历史、撤销粒度与对象存在身份 | ABI 累计 700 项通过；原生/插件交错、同事务发布、多次提交、取消保留 redo、缺失 serializer、陈旧及违规 commit、对象保留与代次、预算淘汰和延后释放。对象所有权使用内核夹具，尚非实际导入/删除入口 | `plugin-abi-shared-history-tests-final.log` |
| 原生删除绑定及跨模型恢复 | 累计 10,671 项通过；真实原生姿势跨模型顺序；相机、附件、外部亲骨骼、显示顺序往返；逐分配故障、失效池拒绝、共同历史发布无分配 | `plugin-abi-shared-history-tests-final.log` |
| 共同历史代码加入后的宿主 | Win7 目标完整构建；原名和中文名隐藏宿主的场景查询、面板、菜单、工具栏及正常退出通过；x64 子系统 6.1，无新增必需可选后端导入 | `plugin-abi-shared-history-final-build-2.log`、`plugin-abi-shared-history-smoke-1.log`、`plugin-abi-shared-history-pe.json` |
| 对象资源、Bullet 保留及共同历史回归 | 原生历史 10,938 项、ABI 契约 700 项及曲线宿主测试均通过；包含同槽位先删后导入、候选保活、附件切线、刚体与碰撞恢复、失败及超预算回收、发布不分配 | `plugin-abi-object-history-batch-final-tests.log` |
| 真实对象导入、删除和设备恢复 | PMX 原名 EXE 与 PMD 中文名 EXE 均 exit 0，两组均包含 X 附件；源文件离线、DX9 重置后的撤销及重做通过。探针直接调用内部共同历史，尚非菜单验收 | `plugin-abi-object-history-batch-host-1.log`、`abi-object-history-batch-host-1/report.json` |
| 对象资源加入后的几何及工程回归 | PMX 与 PMD 两组通过，包含附件、稳定身份、PMM 往返和旧快照寿命 | `plugin-abi-object-history-batch-geometry-1.log`、`abi-object-history-batch-geometry-1/report.json` |
| 对象资源整合后的构建与启动 | 宿主、原生历史、ABI、曲线宿主及侧栏探针五个目标构建成功；原名及中文名隐藏宿主的启动、菜单、工具栏、场景、面板和退出通过 | `plugin-abi-object-history-batch-final-build.log`、`plugin-abi-object-history-batch-smoke-1.log` |
| 对象资源代码的 PE 导入清单 | 宿主为 x64、子系统 6.1；宿主及两个测试 EXE 未新增必需的可选图形或厂商后端直接导入 | `plugin-abi-object-history-batch-pe.json` |
| 批量历史、池扩容与布局持久化 | 原生历史 11,821 项、ABI 契约 900 项通过；曲线宿主、原生变速和关键帧池回归通过 | `plugin-abi-batch-layout-final-tests.log` |
| 工程边界、独立姿态与资源计量 | 原生历史 12,724 项、ABI 契约 996 项、模型加载 UI 22 项及曲线宿主回归通过 | `plugin-abi-history-lifecycle-final-tests.log` |
| 应用共同历史的真实场景寿命 | PMX 原名、PMD 中文名均通过；真实资源计量、取消加载、PMM 重开、新建、准备期间关闭及遗留 token 释放。修复空模型显示复选框通知导致的重置崩溃 | `plugin-abi-history-lifecycle-host-3.log`、`abi-history-lifecycle-host-3/report.json` |
| 工程边界后的插件工程回归 | 原名和中文 EXE 下真实 serializer、PMM 保存与恢复、缺失插件数据保留、失败不发布及卸载通过 | `plugin-abi-history-lifecycle-storage-2.log`、`abi-history-lifecycle-storage-2/report.json` |
| 工程边界后的几何与启动回归 | PMX/PMD 几何、绑定、身份和 PMM 往返通过；原名及中文名隐藏宿主启动、菜单、工具栏、场景和面板通过 | `plugin-abi-history-lifecycle-geometry-1.log`、`plugin-abi-history-lifecycle-smoke-1.log` |
| 工程边界代码的 PE 清单 | 宿主为 x64、子系统 6.1；宿主、两项测试和测试插件均无清单列出的可选图形或厂商 DLL 直接导入 | `plugin-abi-history-lifecycle-pe.json` |
| 姿态采用协议与真实数字入口 | 最终原生历史 13,012 项通过；预留和采用失败回滚、无分配提交、同 token 析构重入、跨模型顺序、位置/旋转/缩放高精度输入及重复失焦。原有 D3DX 数学库的旋转子项实际执行 | `plugin-abi-applied-native-tests-final.log` |
| 对象刷新及混合历史回归 | 对象列表 967 项、模型加载 UI 22 项、ABI 1,019 项及曲线宿主通过；ABI 包含已执行原生修改与插件修改交错撤销 | `plugin-abi-applied-final-tests-1.log` |
| 姿态接入后的隐藏宿主 | PMX 原名、PMD 中文名均通过对象恢复、设备重置及应用历史边界；原名/中文名启动、菜单、工具栏、面板和场景查询通过。普通数字入口仍走旧环 | `plugin-abi-applied-host-1.log`、`plugin-abi-applied-smoke-1.log` |
| 姿态接入构建与 PE 清单 | 六项目标构建通过，追加多轴旋转用例后宿主及原生测试再次构建通过；宿主仍为 x64、子系统 6.1，五文件无所列可选后端直接导入 | `plugin-abi-applied-final-build-2.log`、`plugin-abi-applied-final-build-3.log`、`plugin-abi-applied-pe.json` |
| 姿态命令与共同撤销接入 | 原生历史 15,614 项通过；真实重置、粘贴、镜像粘贴、两窗口轴复位、三种缩放坐标、按钮及 Ctrl+Z/Ctrl+X。覆盖跨模型和相机模式、文本焦点、逐分配故障、无变化保留 redo、录制拒绝及无效槽位 | `plugin-abi-pose-commands-tests-final.log` |
| 姿态命令整合后的回归 | 七组 CTest 全部通过；含 ABI 1,019 项、对象列表 967 项、加载 UI 22 项、键池身份、快捷键及曲线宿主。骨骼旋转 D3DX 子项实际执行 | `plugin-abi-pose-commands-tests-final.log` |
| 姿态命令整合后的隐藏宿主 | PMX 原名、PMD 中文改名通过对象恢复、设备重置和工程边界；两种 EXE 名称通过启动、菜单、工具栏、面板和场景查询 | `plugin-abi-pose-commands-object-final.log`、`abi-pose-commands-object-final/report.json`、`plugin-abi-pose-commands-smoke-final.log` |
| 姿态命令构建与兼容性清单 | 九项目标构建通过；宿主仍为 x64、子系统 6.1，五文件无所列可选后端直接导入。资源校验后复用原编译缓存，未重新编译效果着色器 | `plugin-abi-pose-commands-build-final.log`、`plugin-abi-pose-commands-pe.json` |
| 删键、范围缩放、局部求值与显示键计划 | 原生历史 21,536 项通过；旧实现对照、当前帧求值、保留未登记姿态、自由曲线、跨模型、100 次环覆盖、池增长、嵌套缓冲区、过期拒绝和分配失败 | `plugin-abi-history-phase2-native-2.log` |
| 模型三族原子删键 | 原生历史累计 24,147 项通过；三族旧删除对照、当前帧及外亲窗口求值、表情曲线恢复、整笔失败、逐分配故障、保护页池替换、关联集合过期及无分配提交 | `plugin-abi-model-delete-native-1.log` |
| 模型三族混合范围缩放 | 原生历史累计 28,066 项通过；7 种勾选组合、42 组旧注册器对照、同帧覆盖、后序空族、无骨骼模型、目标外亲窗口、逐分配失败、过期拒绝、无分配发布和真实对话框路由 | `plugin-abi-model-range-native-final.log` |
| 混合范围缩放回归与真实 DLL | 十组 CTest 通过；PMX 原名及 PMD 中文 EXE 通过三族缩放调用、插件跨帧撤销和原生重做，工程切换及关闭仍通过 | `plugin-abi-model-range-regressions-1.log`、`plugin-abi-model-range-host-1.log`、`abi-model-range-host-1/report.json` |
| 三族删键后的相关回归 | 另十组 CTest 全部通过，覆盖 ABI、对象列表、模型加载 UI、曲线宿主、旧骨骼登记、缩放、曲线、键池与键盘输入 | `plugin-abi-model-delete-regressions-1.log` |
| 三族实际命令与插件历史 | PMX 原名、PMD 中文 EXE 均通过：按钮一次删除、一次撤销全部恢复、插件跨帧重做、相机模式及插件回调后的表情曲线刷新；工程重开和关闭仍通过 | `plugin-abi-model-delete-host-final.log`、`abi-model-delete-host-final/report.json` |
| 三族接入后的宿主与资源回归 | 普通启动、菜单、工具栏、面板、场景查询通过；对象批量恢复、DX9 重置和工程边界通过 | `plugin-abi-model-delete-smoke-final.log`、`plugin-abi-model-delete-object-final.log` |
| 三族接入构建与平台边界 | 十三个目标构建通过；宿主仍为 x64、子系统 6.1，六文件无所列可选后端或厂商 DLL 直接导入。资源复用已核验缓存，生成目录已恢复正常命令。Win7 与 AMD 真机未验收 | `plugin-abi-model-delete-tests-build-1.log`、`plugin-abi-model-delete-host-build-final.log`、`plugin-abi-model-delete-pe.json`、`plugin-abi-model-delete-resource-restored.log` |
| Manager 编辑接入后的回归 | 另十组 CTest 全通过，含 ABI 1,173 项、对象列表 967 项、模型加载 UI 22 项、曲线宿主、旧登记、缩放、曲线、键池及快捷键 | `plugin-abi-history-phase2-regressions.log` |
| 真实插件与原生命令共同历史 | PMX 原名及 PMD 中文名通过；菜单操作、单次/多次撤销、原生与插件交错、修订号、新建/PMM 重开/关闭、回调泵后的控件刷新、播放与录制期间拒绝 | `plugin-abi-edit-history-host-final.log`、`abi-edit-history-host-final/report.json` |
| 本轮对象寿命及普通启动回归 | 两种 EXE 名称均通过对象批量恢复、DX9 重置、工程边界；普通启动、菜单、工具栏、面板和场景查询通过 | `plugin-abi-history-phase2-object-final.log`、`plugin-abi-history-phase2-smoke-final.log` |
| 本轮构建与兼容性清单 | 十三个目标及新增编辑探针 DLL 构建成功；补充播放限制后宿主和隐藏宿主再次构建。宿主 x64、子系统 6.1；六文件无所列可选后端或厂商 DLL 直接导入。资源复用已校验缓存，生成目录已恢复正常命令 | `plugin-abi-history-phase2-build-final.log`、`plugin-abi-history-host-gate-build.log`、`plugin-abi-history-phase2-pe.json`、`plugin-abi-history-phase2-resource-restored.log` |
| 骨骼登记及独立作者数据 | 原生历史 17,660 项通过；单骨骼和多骨骼登记与旧函数的 16 组插入/覆写/自动插值对照、跨模型姿态混合、100 次旧环覆盖、缩放与自由曲线恢复、无分配发布、扩容与逐分配失败、过期拒绝及 redo 保留 | `plugin-abi-bone-registration-native-final.log` |
| 骨骼登记整合后的回归 | 另十组 CTest 全通过：ABI 1,019 项、对象列表 967 项、加载 UI 22 项，以及原登记、曲线、缩放、键池、快捷键及曲线宿主 | `plugin-abi-bone-registration-regressions-1.log` |
| 骨骼登记整合后的隐藏宿主 | PMX 原名、PMD 中文改名通过对象恢复、设备重置及工程边界；原名和中文 EXE 通过启动、菜单、工具栏、面板和场景查询。测试使用隔离副本 | `plugin-abi-bone-registration-object-final.log`、`abi-bone-registration-object-final/report.json`、`plugin-abi-bone-registration-smoke-final.log` |
| 骨骼登记构建与兼容性清单 | 十三个目标构建成功，最终原生测试再次构建通过。宿主仍为 x64、子系统 6.1；五文件无所列可选后端直接导入。资源经校验后复用既有编译缓存 | `plugin-abi-bone-registration-build-2.log`、`plugin-abi-bone-registration-build-final.log`、`plugin-abi-bone-registration-pe.json` |
| 四对象批量恢复 | PMX 与 PMD 两组均包含两个模型、两个 X 附件；整批导入和删除各占一条记录，源文件离线及 DX9 重置后连续往返通过 | `plugin-abi-multi-object-host-final.log`、`abi-multi-object-host-final/report.json` |
| 布局文件与真实重启 | 正常命令提交及关闭事件提交两组通过；每组覆盖插件暂时缺失、恢复注册和中文 EXE 改名；文件测试覆盖独占锁、外部修改冲突、替换失败清理及坏文件保留 | `plugin-abi-ui-preferences-host-final.log`、`plugin-abi-ui-preferences-close-final.log`、`plugin-abi-batch-layout-final-tests.log` |
| 批量与布局整合后的宿主回归 | 完整构建、PMX/PMD 几何和 PMM 往返、原名及中文名下的菜单、工具栏、双面板与场景查询通过 | `plugin-abi-batch-layout-final-build.log`、`plugin-abi-batch-layout-geometry-final.log`、`plugin-abi-batch-layout-smoke-final.log` |
| 批量与布局代码的 PE 导入清单 | 宿主仍为 x64、子系统 6.1；宿主、两个测试 EXE 和测试插件无新增必需的可选图形或厂商直接导入 | `plugin-abi-batch-layout-pe.json` |
| 定义接入后的 PE 导入清单 | 四文件均无可选图形/厂商后端的直接导入 | `plugin-abi-definitions-pe-inventory.json` |
| PE 导入/导出清单 | 四文件通过可选后端直接导入检查 | `plugin-abi-win7-pe-inventory.json` |
| 处理器策略 | 99 项通过，P2 新增前 | `plugin-abi-processor.log` |
| 侧栏/底部松手提交、取消、浮动和媒体 | 通过，P2 新增前 | `plugin-abi-sidebar.log` |
| 视口缩放、菜单语言、可选图形导入、旧插件菜单 | 5 个目标通过，P2 新增前 | `plugin-abi-regressions.log` |

批量回归发现 Bullet 2.75 新建和复用 sweep 句柄后，包围盒字段要等第一次 `setAabb` 才初始化，导致刚导入对象的历史候选偶发失效。增强 Broadphase 现立即初始化包围盒，状态校验仅比较向量和变换的有效分量。固定回归主动污染回收句柄，并验证未发生物理更新就可撤销和恢复；第四分量为 NaN 不误判，实际坐标变化仍拒绝。布局回归还补齐了关闭事件之后保存，以及尺寸与最小约束冲突时的原子拒绝。

对象资源这一轮最初的曲线宿主测试在缩放文字重绘断言失败。夹具将标签放在固定坐标，已不在当前底栏范围内；现按 `FramePanelLayout.bottomBand` 定位。新增嵌套夹具随后暴露测试栈溢出：`MMDAppState{}` 赋值生成约 680 KB 的临时对象。改为原地初始化后，曲线宿主及整组最终回归通过，未修改生产绘制逻辑。池扩容的嵌套测试同样改为原地初始化。

早期多目标构建因本机未配置 `physics_pose_tests.vcxproj` 失败，不能作为整组成功证据。最终构建使用已配置的五个目标，见 `plugin-abi-object-history-batch-final-build.log`。ABI 测试的临时文件曾因沙箱临时目录写入被拒绝而失败；将该进程的 `TEMP` 和 `TMP` 设为仓库内 `build/abi-tmp` 后，700 项通过，最终整组复验也通过。

侧栏测试的附件排序用例缺少初始选中项。关闭新 ABI 初始化时也复现同一失败，见 `plugin-abi-accessory-without-abi.log`。测试已明确初始选择并验证排序后保持选择；附件业务逻辑未修改。

首次双面板测试把 SURFACE_DESTROY 计入 DESTROY 子串数量。已改按完整事件行计数并复跑，保留视图先销毁、插件随后停止和销毁的顺序检查。

几何测试发现 PMX 加载器为附加 UV Morph 保存基准时复制了普通 UV。增强构建现按各附加 UV 通道保存 float4，原始兼容构建保持旧路径。测试从原创 PMX 文件加载后核对原始值，再修改工作数组，验证绑定快照恢复原值且不改写当前 Morph 状态；现有 Morph 并行计算回归也通过。最初 PMX 2.1 夹具被现有导入器拒绝，因此文件测试改用实际支持的 PMX 2.0，QDEF 在运行时记录上单独测试。

统一调度测试启动隔离的隐藏窗口，只替换该测试窗口的可见性查询及绘制损伤矩形。场景、物理、GPU、编码器及输出仍走 MMS 实现。测试覆盖多次 UI 绘制零场景增量、视口暴露合并、绘制失败重试、动态刚体保持、实际窗口尺寸变更、模态与编辑锁、播放推进、设备丢失与取消。侧栏和底部默认仍在松手时提交布局。

最初序列夹具把每次 SafePoint 调用当成一帧，150 次调用在 60 fps 测试中只生成 48 张图片。夹具现按实际 FrameDriver 进入计数，并为等待设置墙钟上限；输出帧数、连续编号和终结断言保持不变。`full-3` 的 24、30、60、29.97 fps 输出分别为 24、30、60、30 张，包含模型模式、相机模式、七种图片格式、三语言、取消及碰撞检查。时间抗锯齿用本地 `YYB Hatsune Miku_NT_1.0ver.pmx`，路径与 SHA-256 记入报告；它只作为本机夹具，不进入 SDK 或发行包。alpha-triangle 用于设备恢复和菜单初始化，不足以覆盖该抗锯齿用例固定相机下的可见几何要求。

## 重现

先按 `profiles/win7-x64.lock` 安装 Conan 依赖至 `build/win7-x64`，再执行：

```powershell
cmake --preset win7-x64
cmake --build --preset win7-x64-release --target MikuDanceStudio plugin_abi_tests
ctest --preset win7-x64-release -R '^plugin_abi_contract$' --output-on-failure
python tests/plugins/host_smoke.py build/win7-x64/build/Release/MikuMikuDance.exe build/win7-x64/build/tests/plugins/lifecycle/Release/lifecycle.dll build/abi-smoke-new gui-panel
```

本机 PDB 工具版本不匹配，验证构建显式设 `MIKUDANCESTUDIO_DEBUG_SYMBOLS=OFF`，默认配置仍生成符号。资源编译临时目录设为仓库内 `build/abi-tmp`，未修改系统环境。

原生历史阶段的 D3DX 效果预编译停在 `CompileEffect`，原生历史及后续对象资源验证继续使用已校验的资源缓存。资源输入未修改，缓存时间晚于全部输入；另对两份资源各 80 个记录重新核对类型、语言和非着色器内容，并检查缓存哈希及编译后效果标识。记录见 `plugin-abi-native-history-resource-cache.json` 和 `plugin-abi-native-history-resource-verify.log`。C++ 代码及测试重新编译、链接，缓存复用不计为新的着色器编译验证。临时校验命令只用于本机生成目录；每批完成后恢复正常资源构建命令。本次记录见 `plugin-abi-batch-layout-resource-restored.log`。

Win7 回退依据：[GetTransformForStream](https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriterex-gettransformforstream) 要求 Win8；[GetServiceForStream](https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-getserviceforstream) 支持 Win7。代码保持 Win7 目标宏，仅查询可选 COM 能力；编译通过不替代媒体功能验证。
