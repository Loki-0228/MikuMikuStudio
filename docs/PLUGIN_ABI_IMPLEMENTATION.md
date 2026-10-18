# 插件 ABI 实现与验收记录

更新：2026-10-19。分支 `codex/plugin-abi` 从 `42d4a71` 建立。依据工作区《MMS 插件实施方案与 ABI 参考》《MMS 插件开发文档》《MMS 界面布局与重绘设计》实施。完整 P0–P6 尚未完成。

状态分为 **未实现**、**已实现未验证**、**本机已验证**、**缺其他系统/硬件环境**。代码、本机结果和平台结果分别列出。

R3 拖动与插件区域问题已由用户确认恢复。按需求归类的当前缺项、部分实现及环境验收见[剩余工作核对](records/abi-remaining-work-2026-10-17.md)。

[工程打开身份](records/abi-project-open-identity-2026-10-19.md)修复审查第 8 项：候选路径不再提前替换保存目标；替换前拒绝保留原工程，已替换后失败清空目标。完整配置 430 项、原始配置 241 项及真实宿主正常 PMM 重开通过。关闭后异常尾链的既有重复关闭问题另行处理。

[相机零帧合并](records/abi-camera-root-payload-2026-10-19.md)补齐父模型、父骨骼及距离／FOV 的控制柄，普通和共同历史入口一致。完整帧线整合 37,820 项、两种配置的普通负载专项及原始配置读写通过；文件格式与原生采样保持。

[参数滑杆重复刷新](records/abi-slider-refresh-2026-10-19.md)已减少灯光、方向、视角和 Morph 的同值位置及文本回写。最终专项 7,318 项、ABI 1,804 项、真实 PMX/PMD 宿主和两种配置构建通过；原生数值与历史语义保持。连续拖动的视觉闪动仍待用户复验，不能以消息计数代替。

[main 全仓审查的 ABI 对照](records/abi-main-review-comparison-2026-10-18.md)已逐项核对 22 项：普通完整配置均有残留，受控路径须分别判断。短路径解析及 PMX toon 索引已实际复现访问冲突；其他条目的本轮证据为源码与覆盖检查。该核对不表示缺陷已修复，正式程序未重建。

随后完成 [13 项审查修复](records/abi-review-fixes-2026-10-19.md)，覆盖普通零帧链、物理编辑、事务 PMD 保存、录制失败与重入、Kinect 数学及两处越界。完整配置和原版构建及定点回归通过；共同历史 263,498 项、ABI 1,804 项、真实物理窗口 1,277 项通过。普通视频重排布局仍有两个像素差，已保存旧副本对照并保留失败断言；不能据此批结果宣称整个 ABI 需求完成。

已同步 MMS 的[图片导出、粘贴及 PMM 保存修复](records/abi-image-paste-save-sync-2026-10-18.md)。PNG、DDS 自动透明；无关重名不再阻止粘贴；无效数据在插件保存事务开始前拒绝。完整共同历史 200,122 项及两条实际导出路径通过，普通宿主编辑服务仍未开放。

随后补齐[曲线修复启动器和正常保存验收](records/abi-main-bugfix-completion-2026-10-18.md)，并适配修复副本的插件侧车哈希及保存身份。五组实际加载与重开、侧车数据保留、三语言保存错误提示及序列化边界通过。

随后同步[配件列表拖拽排序](records/abi-accessory-order-sync-2026-10-18.md)。三语言拖拽、取消、失败回滚及 PMM 往返通过。该批暂时禁用共同历史下的排序；现已按下述配件设置迁移开放。

需求复核修复了[后台 UI 任务丢失调度](records/abi-task-dispatch-2026-10-18.md)：工程准备或只读回调消费唤醒消息后，任务现在延后自动恢复。真实 DLL 的执行、取消、停止和共同历史恢复通过；契约累计 1,775 项，刷新专项 7,761 次断言及两种配置回归通过。独立作业服务仍未实现。

[配件设置](records/abi-accessory-settings-2026-10-18.md)已将顺序、名称和绘制分界合为一次共同历史显式提交。专项 8,819 项及三语言真实宿主通过，补齐时间轴缓存、场景名称快照及元数据不重置物理。共同历史累计 208,941 项；普通公共 `mms.edit` 仍关闭。

[曲线编辑器](records/abi-curve-timeline-2026-10-18.md)补齐顶部刻度同步主时间轴和视图、激活窗口快捷键及长区间速度图。[非骨骼保形插帧](records/abi-camera-subdivision-2026-10-18.md)保留相机原生求值，独立 PMM 片段支持重载和时间域映射。[VMD 兼容检查](records/abi-curve-vmd-compat-2026-10-18.md)补上旧 VMD1 相机记录分支，固定原生六通道读写结果。

随后将[五类全局曲线及窗口撤销](records/abi-curve-external-history-2026-10-18.md)接入共同历史。曲线写入和窗口读取 VMD 都是一次显式提交，窗口撤销直接遍历宿主队列。原始作者数据、当前帧局部求值、后续选区和提交焦点均已验证；十一组整合通过，共同历史 212,918 项，原版兼容四组仍通过。[模型骨骼、表情及显示／IK 窗口](records/abi-model-curve-history-2026-10-18.md)随后完成受控迁移。公共 `mms.edit` 继续关闭。

本轮完成[模型 VMD 自由曲线尾段](records/abi-model-vmd-curves-2026-10-18.md)：标准键、缩放和作者数据在同一私有目标上准备，一次显式提交。保留 VMD 原有格式、匹配、尾部消费和元数据规则，修正合法旧表情大切线被额外跳过的差异。最终 18 组回归、原版兼容四组、真实 PMX/PMD 宿主及产品原名/中文改名通过；原生历史累计 263,498 项。公共 `mms.edit` 继续关闭。

[按钮 STATE 焦点](records/abi-refresh-next-2026-10-18.md)补齐禁用与原生回调的安全转移，独立桌面刷新 7,961 项通过；DATA 与 LAYOUT 仍未实现。

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
| P1/P2 | 同一 C ABI 的类型化与声明式 UI SDK | 本机已验证 | `mms/cpp/ui.hpp` 提供服务查询、原始句柄所有权、显式移交及命令/菜单/按钮/分组组合。默认布局、失败回收重试、同句柄低层更新、重排后的依赖清理通过；独立 C++17 DLL 与真实原名/中文改名宿主均通过。无新增刷新、租约类、溢出或编辑服务，见[验证记录](records/abi-cpp-sdk-2026-10-17.md)。 |
| P1 | 命令、只读属性、更新、快照租约 | 本机已验证 | 所有权、参数范围/选项、上下文不可替换、旧快照及线程边界。 |
| P1 | 全部业务事件通知 | 部分实现 | UI_READY/UI_CLOSING；工程打开、手动保存、重置事件已接入。其他业务来源仍待接入。 |
| P2 | Surface 工厂和生命周期 | 本机已验证 | 单/多视图、失败重试、错误父窗口、隐藏尺寸/DPI、换父重建、focus、停止和一次销毁。 |
| P2 | 工作区双页签实际挂载 | 本机已验证 | 真实 MMS 原名/中文名，切换、隐藏复用、焦点回调、关闭各销毁一次。 |
| P2 | 工具栏自动布局树 | 本机已验证 | 行列、换行、网格、伸缩、隐藏占位、嵌套高度及缩放往返。 |
| P2 | 布局事务和轻量提示 | 本机已验证 | MOVE beforeId、创建/删除空组、显隐、尺寸、快捷键、恢复；默认和用户覆盖分离。 |
| P2 | 几何快照 | 本机已验证 | HWND 应用后发布；UI/几何版本分开；未就绪 NOT_READY，旧租约不变。 |
| P2 | 统一刷新首阶段 REPAINT + SELF | 本机已验证 | `mms.ui.refresh` 支持本插件已有按钮与 surface 面板的后台局部重绘请求、合并、独立取消及票据保活；实例、工程代次与实际几何校验。刷新专项 7,229 次断言及真实 Manager DLL 路径通过，DATA/STATE/LAYOUT、绑定和依赖范围仍未实现，见[验证记录](records/abi-ui-refresh-2026-10-17.md)。 |
| P2 | 同一刷新 C ABI 的 C++17 SDK | 本机已验证 | `RefreshServices` 提供单调用整目标与局部矩形请求；`RefreshTicket` 管理原句柄，支持移动、取消、释放失败重试及显式移交。刷新专项累计 7,349 次断言、既有 ABI 1,694 项与真实改名宿主通过；没有开放新的刷新种类，见[验证记录](records/abi-refresh-sdk-2026-10-17.md)。 |
| P2 | 按钮启用/选中的 STATE 首阶段 | 本机已验证 | 同一票据支持后台 STATE 和 STATE/REPAINT 组合；只读查询目标命令，拒绝显隐变化，抑制 UI 回调中的 STATE 递归，旧刷新及布局延后到回调外。刷新专项累计 7,523 次断言、Enter/Space、真实 DLL 和混合历史回归通过。完整 STATE、DATA、LAYOUT 与依赖范围仍待实现，见[验证记录](records/abi-button-state-2026-10-17.md)。 |
| P2 | 保留槽位按钮显隐与后台 STATE SDK | 本机已验证 | 在有效 PRESERVE_HIDDEN_SLOT 槽位内更新原按钮显隐、焦点及公开几何，不重排或写入布局偏好。RefreshStateCurrent 组合原 C 请求；固定几何与当前几何贡献分组。刷新专项累计 7,764 次断言、真实 DLL、改名宿主、混合历史及调度回归通过；完整 STATE、DATA 和 LAYOUT 仍待实现，见[验证记录](records/abi-slot-visibility-2026-10-17.md)。 |
| P2 | 可写属性、声明式 INPUT | 未实现 | 依赖 P3 工程事务及撤销，当前返回 NOT_READY。 |
| P2 | 左右侧栏、属性区、时间线、状态栏挂载 | 未实现 | 不枚举空壳挂载点。 |
| P2 | DOCK、命令溢出菜单 | 未实现 | 返回 UNSUPPORTED。 |
| P2 | 本体菜单、控件、业务命令全部语义登记 | 未实现 | 新注册表目前覆盖新 ABI 插件。 |
| P2 | 布局持久化、缺失插件保留 | 本机已验证 | 用户覆盖按稳定 ID 保存到程序旁 `data/settings.json` 的 `pluginUi`，旧布局仅缺失时迁移；分区 CAS、其他设置合并、重启、关闭事件及 EXE 改名通过。 |
| P2 | 布局撤销历史 | 未实现 | 当前保存布局，不记录布局 undo/redo。 |
| P2a | 可选布局/绘制面积/帧驱动/场景/物理/设备计数 | 本机已验证 | `MMS_UI_METRICS` 输出 JSONL；受控损伤矩形触发真实 WM_PAINT 和 NM_CUSTOMDRAW，纯 UI 的 scene、effectBegin、present、reset 增量均为 0。effectBegin 是外层代理 BeginScene 入口，不是 MME 内部阶段计数。 |
| P2a | MME 内部阶段计数和 ABBA 性能基线 | 未实现 | 不宣称性能改善。 |
| P2b | 局部 GDI 缓冲、批量 HWND 应用及失败恢复 | 本机已验证 | 多父窗口分批、验证实际坐标、失败回滚、缓冲复用/初始化/尺寸键；接入新工具栏及扩展区。 |
| P2b | 本体既有参数区迁移与全区域缓冲 | 部分实现 | R3 已合并侧栏／底部控件最终几何并缓冲父窗口 GDI 绘制；未完成全部本体控件的语义登记、统一几何和命中迁移，也不保证任意子窗口内部双缓冲。 |
| P2c | 独立更新协调器逻辑 | 本机已验证 | 虚拟时钟合并、重入、代次、Retry、Failed/异常、隐藏恢复；新几何保留未消费工作。 |
| P2c | 统一更新协调器、WM_PAINT 解耦、暴露恢复 | 本机已验证 | 增强构建从主泵及原生尺寸循环的安全点调用帧驱动；WM_PAINT 只请求重绘。QPC 单调时钟、请求合并、失败保留、隐藏与模态退避、回调及帧重入保护。原始兼容构建保留旧路径。 |
| P2c | 停止恢复、播放、媒体、设备及导出 | 本机已验证 | 动态刚体验证停止重绘不推进物理，播放仍推进；12 项设备与录制恢复、停止视频更新、图片序列及时间抗锯齿通过。见下方夹具范围。 |
| P2c | 真实 MME 内部状态、真实桌面遮挡及多 DPI | 未实现 | 当前真实主路径使用内置效果及隐藏窗口；旧 MME 内部状态保持和桌面视觉效果尚未验收。 |
| P2d | 全区域统一几何、绘制和命中迁移 | 未实现 | 新扩展区接入不等于全部本体区域完成。 |
| P2e | 侧栏／底部分隔线实时拖动 | 自动回归及本轮人工复验通过 | R3 统一最终几何批次、宿主 GDI 缓冲及插件边界更新；实时挂载、重复位置、取消、捕获及资源回归通过。用户反馈「均恢复」，见[本轮记录](records/abi-drag-flicker-plugin-layout-2026-10-17.md)。 |
| P2f | 独立表单及模拟视口宿主 | 本机已验证 | 两个独立 CMake 项目构建及隐藏自检 exit 0，无 MMS/图形设备依赖。 |
| P2f | 两个示例的真实输入法、多 DPI、视觉交互 | 已实现未验证 | 自检不替代实际桌面交互验收。 |
| P2 | 输入法组合态、Tab、辅助功能和真实多 DPI | 部分人工验证 | 用户反馈测试副本微软拼音及单屏 100%、175%、200% 正常，R3 拖动与插件区域问题已恢复。快捷键冲突、辅助功能和混合 DPI 跨屏仍未验收，见[手工记录](records/abi-ui-manual-acceptance-2026-10-17.md)。 |
| P3 | scene 对象及绑定骨骼快照、稳定身份 | 本机已验证 | `mms.scene` 已发布：阶段 0 的对象与骨骼。独立 ID、运行时代次、任意线程读取/释放、不可变租约；真实 PMM 保存重开保留对象和骨骼 ID，旧引用返回 STALE。 |
| P3 | 绑定网格、权重、材质、纹理路径 | 本机已验证 | PMX 2.0、PMD、原始 X 附件；稳定元素 ID、嵌套租约、对象筛选、无效拓扑拒绝、内容变化修订及真实 PMM 往返。纹理尺寸/格式未知，运行时 QDEF 记录通过不等于 PMX 2.1 文件导入通过。 |
| P3 | 绑定刚体、形状、关节及 Morph 定义 | 本机已验证 | 球/盒/胶囊、六自由度弹簧、组/顶点/骨骼/UV/材质 Morph；刚体和关节矩阵与原生加载器、Bullet 约束独立比较，身份经真实 PMM 往返保留。 |
| P3 | 当前场景查询 | 本机已验证 | CURRENT_TIME 标志支持非零编辑帧查询，忽略 time，不移动播放头；仍可用 flags=0 检查指定时间。 |
| P3 | 求值阶段及未支持的定义类型 | 未实现 | 阶段 1–4、翻转/冲量 Morph、其他关节、复合形状及软体。绑定配置不能替代物理后状态。 |
| P3 | edit 两阶段事务内核 | 本机已验证 | prepare/join/commit/cancel、修订检查、serializer 代次、撤销字节深复制、失败取消和历史预留。测试使用宿主夹具，见[原生接入边界](PLUGIN_EDIT_INTEGRATION.md)。 |
| P3 | 原生模型历史准备与发布 | 本机已验证 | 类型 1–7、链式操作和回绕，缩放、自由曲线及 Morph 层；准备失败不写状态，发布不分配。按钮、快捷键及直接撤销入口已接入。 |
| P3 | 共同历史内核与插件撤销粒度 | 本机已验证 | `SharedHistory` 实现 `EditHost`；原生和插件同队列、同事务，按提交分步或合并撤销，失败保留游标及重做分支、资源延后回收。 |
| P3 | 对象存在身份与删除绑定恢复 | 本机已验证 | 稳定 ID、恢复后新代次；相机、附件、外部亲骨骼及显示顺序恢复。注册关键帧池正常扩容保留身份，恢复时重新定位字段；池替换、借用数组移动仍拒绝。 |
| P3 | 模型及附件资源保留组件 | 本机已验证 | `RetainedSceneObject` 保留原生对象、关键帧、附件曲线、COM 资源及 Bullet 对象。隐藏宿主探针覆盖源文件离线、DX9 重置及资源往返；删除与私有导入入口均已受控验证。 |
| P3 | 整批对象导入与删除候选 | 本机已验证 | 多对象共用一份物理候选及绑定记录；两模型与两附件作为一条记录往返，恢复原资源及顺序。同种对象槽位冲突拒绝，跨物理 owner 关节仍不支持。 |
| P3 | Bullet 移出和恢复候选 | 本机已验证 | 保留刚体、约束及代理句柄；移出后不参与模拟、碰撞和射线查询。恢复重建静止重叠，保留其他碰撞算法；发布不分配，旧接触在解锁前清理。外部 action、碰撞回调及未保留的关联约束拒绝准备。 |
| P3 | 应用历史寿命与工程切换 | 本机已验证 | 应用持有共同日志；准备工程可取消，PMM 替换前释放旧历史及原生候选；新建和关闭时在世界销毁前回收。混合编辑未取消时拒绝重置。 |
| P3 | 独立 type 1 姿态记录 | 本机已验证 | 100 次覆盖旧环后往返；按平移分量、旋转、dirty 与缩放差异恢复，保留未改字段和播放头。其他类型的独立记录仍待实现。 |
| P3 | 原生保留资源计量与准入 | 部分实现 | 自有文本、旧撤销分配、Bullet 2.75 私有数组、实际池容量、侧表及 DX9 查询均进入实测预算。每次准备重算目标物理快照，发布前拒绝资源增长；未知或外部资源仍标缺项并拒绝准入。全局缓存及驱动内部开销不属对象预算。 |
| P3 | 模型加载后的 UI 延后机制 | 本机已验证 | PMD、PMX 成功尾部支持按窗口嵌套延后；取消或异常不刷新，提交成功后由调用方显式刷新。该组件只延后 UI，不能代替本批私有导入准备。 |
| P3 | 已执行姿态的历史预留与采用 | 本机已验证 | 修改前预留节点和预算，修改后验证反向候选，提交只入历史；失败先回滚再解锁。数字输入、重置、粘贴、镜像粘贴及两窗口轴复位仅在内部绑定下接入，普通宿主保留旧路径。 |
| P3 | 受控共同历史命令 | 本机已验证 | 撤销按钮及 Ctrl+Z、重做按钮及 Ctrl+X 不依赖当前模型或相机模式。空历史、BUSY、准备失败不回退旧环；文本焦点及播放录制检查保持。仅内部绑定启用。 |
| P3 | 历史记录模式、步数与预算 | 受控路径本机已验证 | 帮助系统设置提供两种模式、最大步数、内存预算及本机建议。默认仅显式提交、32 步／64 MiB；先保存再裁剪，预览不受保留预算限制。普通完整历史仍未绑定；原生历史 111,154 项、ABI 1,717 项及真实设置对话框通过，见[本轮记录](records/abi-history-settings-2026-10-17.md)。 |
| P3 | 四类帧线插删 | 受控路径本机已验证 | 骨骼、表情、相机和灯光接入 `255`／`256`／`257`／`258`，两种模式均一次显式提交。骨曲已使用明确帧映射；相机、灯光补入稀疏作者关键点、手柄和相机区间控制的私有重定时，涵盖只有作者层发生变化的情况。旧入口已有作者层时复用同一计划，准备失败不执行原生写入或界面求值尾部。专项 35,106 项、完整历史 187,476 项、15 组相关回归及两种真实宿主通过，见[最新记录](records/abi-global-frame-retime-2026-10-18.md)。 |
| P3 | 骨骼关键帧粘贴 | 受控路径本机已验证 | `421`／`422` 普通及镜像骨骼粘贴持有全部键值和缩放曲线，重做不读剪贴板。无名称匹配不改历史；PMX 固定轴、PMD 尾轴与旧注册器对照通过。 |
| P3 | 表情与显示键单独粘贴 | 受控路径本机已验证 | `421` 支持只含表情或只含显示／IK／外亲的粘贴。多条源记录一次提交，重做不读剪贴板；保留未登记权重及脏姿态，嵌套载荷、外亲身份、预算和失败通过。见[验证记录](records/abi-model-paste-2026-10-17.md)。 |
| P3 | 模型三族混合粘贴 | 受控路径本机已验证 | 骨骼、表情、显示／IK／外亲的全部组合通过 `421` 一次提交，统一共享标记、最大帧及游标，外亲目标统一求值；零骨模型、无操作保留重做、三池增长、失败及源独立重放通过。原生历史累计 125,039 项及 PMX／PMD 实际宿主通过，见[验证记录](records/abi-mixed-model-paste-2026-10-17.md)。 |
| P3 | 五类全局轨道混合粘贴 | 受控路径本机已验证 | 相机、灯光、自阴影、重力及多附件的 `421` 一次显式提交，共享选择及最后登记帧统一发布。零帧覆盖头节点、重力原生求值、附件保留字节、私有增长、失败原子性及剪贴板独立重放通过。原生历史累计 122,936 项及 PMX／PMD 实际命令通过，见[验证记录](records/abi-global-paste-2026-10-17.md)。 |
| P3 | 标准全局 VMD 导入 | 受控路径本机已验证 | 镜头模式支持 VMD 1／2 相机、灯光、自阴影，一次快照解码及私有批量准备，一次显式提交，源删后可重放。专项 7,588 项、原生历史累计 132,575 项及真实 PMX／PMD 宿主通过，见[验证记录](records/abi-vmd-global-2026-10-17.md)。 |
| P3 | 模型标准 VMD 与附件扩展 | 受控路径本机已验证 | 支持 VMD 1／2、Unicode 名称、缩放矩阵及缩放曲线；标准键和作者状态同事务。附件按名称和面板顺序匹配，多对象与标准全局轨道一次提交。模型源 1,455 项、模型 VMD 最新 667 项、附件 508 项；真实 PMX／中文改名 PMD 宿主、删除源文件后重做通过。骨骼与表情自由曲线块仍拒绝，见[最新记录](records/abi-scale-wheel-2026-10-17.md)。 |
| P3 | 相机视口右键、中键与滚轮 | 受控路径本机已验证 | 右键及中键整次手势提交；滚轮每消息一步 Adjustment，保留主／浮动窗口的不同原生数学。取消、捕获、关闭重建、失败原子性、两种记录模式、跨模式／播放头重做已验证。视口专项 2,433 项、完整原生历史 137,897 项及两种真实宿主通过。HUD 与完整跟随开关仍未迁移；模型视图和小键盘另列，见[验证记录](records/abi-scale-wheel-2026-10-17.md)。 |
| P3 | 模型视图右键、中键与滚轮 | 受控路径本机已验证 | 自由视图及父模型不同于当前选择的跟随视图保留原拖拽数学；主窗口和浮动窗口滚轮保留各自精度。一次手势或滚轮作为一次 Adjustment，两种记录模式、变化字段恢复、取消及来源复验已实现。空模型、非法骨索引和待应用缩放读数在写入前拒绝，取消及撤销刷新不隐式应用骨变换。专项 3,182 项、完整历史 199,893 项、五组相关回归及原版档三组通过；PMX 原名及中文改名 PMD 宿主均通过，范围见[本批记录](records/abi-model-viewport-2026-10-18.md)。同模型跟随的拖拽与主窗口滚轮仍待迁移；浮动滚轮支持。 |
| P3 | 完整相机跟随开关 | 方案已细化，未实现 | 启用会覆盖相机、灯光、阴影、重力和全部附件的未登记值，需要复合姿态事务。CPU 私有采样、逐字段发布、别名处理与双入口已列入[落地方案](records/abi-follow-toggle-plan-2026-10-18.md)，静态分析不计为验收。 |
| P3 | 小键盘视图预设 | 本机已验证 | 相机、跟随及模型自由视图的七键共用纯目标计算；模型 `0` 键私有采样相机及作者曲线。一次输入泵按原顺序合为一次 Adjustment，撤销只恢复变化字段，不重新求值。专项 9,235 项、完整历史 196,711 项、15 组相关回归、原版档 2 组及两种真实宿主通过，见[记录](records/abi-numpad-views-2026-10-18.md)。 |
| P3 | MMS 相机与曲线修复同步 | 本机已验证 | 冻结来源 main 的相机补丁，修复第 0 帧链表、整轨误选、快捷键重复处理和过期草稿覆盖，保留 ABI 门禁；通知分配失败不再从析构函数抛出。全局稀疏通道及完整骨曲协调均已适配，现已真实合并来源 `9eec5cf`，见[最新合并记录](records/abi-main-sync-2026-10-18.md)。 |
| P3 | 稀疏通道显式登记 | 本机已验证 | 五类全局作者轨道在原生数值相同时仍补回缺失通道关键点，受控注册、粘贴及对应 VMD 共用一次历史。旧入口批量合并同步，通知不在受控提交后写作者数据。专项 12,686 项、完整原生历史 150,642 项、相关 14 组测试及两种真实宿主通过，见[验证记录](records/abi-sparse-registration-2026-10-18.md)。 |
| P3 | 骨曲与原始轨道协调 | 本机已验证 | 合并原始键改值、插删及重定时，保留未改的精度、欧拉角转数和手柄。受控登记、粘贴、标准模型 VMD、帧线及范围缩放在私有作者目标中协调，并计入基线及待登记集合预算；仅选择标记不同不误记历史。专项 1,230 项、完整原生历史 152,328 项及 15 组相关测试通过，真实宿主范围见[最新记录](records/abi-main-sync-2026-10-18.md)。 |
| P3 | 视图菜单骨骼旋转归零 `302` | 本机已验证 | 显式绑定下将当前骨骼旋转归零作为一条共同历史；保留位置、缩放、选择和播放头。恒等无操作保留 redo，分配失败回滚；专项 138 项、原生历史累计 94,370 项及真实 PMX／PMD 插件交错、保存、工程重开与关闭回归通过。此行为骨骼范围；数值输入与相机迁移另列，见[验证记录](records/abi-bone-rotation-reset-2026-10-17.md)。 |
| P3 | 视图菜单骨骼数值输入 `300` | 本机已验证 | 原模态交互、父骨骼相对坐标和旋转规则接入共同历史。无编辑保留精确原值及 redo；关闭后重新校验绑定、对象、骨骼、帧、姿态和历史，插件待提交事务及递归均拒绝。专项 310 项、原生历史累计 94,680 项及真实 PMX／PMD 对话框、插件交错、保存和生命周期回归通过。此行为骨骼范围；相机数值输入另列，见[验证记录](records/abi-bone-numeric-input-2026-10-17.md)。 |
| P3 | 视图菜单相机旋转归零 `302` | 本机已验证 | 独立旋转分量记录不写关键帧和旧环，位置、距离、绑定及播放头保持；同值保留 redo。取消、源过期、初次与往返准备失败不写状态；专项 73 项、原生历史累计 94,753 项及真实 PMX／PMD 插件交错、保存和生命周期回归通过。普通编辑服务仍关闭，见[验证记录](records/abi-camera-rotation-reset-2026-10-17.md)。 |
| P3 | 视图菜单相机数值输入 `300` | 本机已验证 | 七字段姿态记录与归零共用；原输入交互、符号与未编辑弧度保持。模态结束核对姿态、绑定、父身份及历史，取消／无操作保留 redo。专项 79 项、原生历史累计 94,832 项及真实 PMX／PMD 对话框、插件交错、保存和生命周期回归通过；其他原生写入仍待迁移，普通编辑服务保持关闭，见[验证记录](records/abi-camera-numeric-input-2026-10-17.md)。 |
| P3 | 主／浮动相机字段归零 `537..543` | 本机已验证 | 14 条入口复用七字段记录，位置、旋转与距离各自一条历史。保留成功后的轨道激活差异，无操作／失败保留选择与 redo。专项 538 项、原生历史累计 95,370 项及真实 PMX／PMD 宿主与插件交错、保存和生命周期回归通过；浮动入口为生产函数分派验证，见[验证记录](records/abi-camera-axis-reset-2026-10-17.md)。 |
| P3 | 相机面板位置／旋转／距离文本 `544..550` | 本机已验证 | 修改标志门禁和失焦消费，保留旧 double 角度精度；重复失焦／Enter、同值及非法输入保留历史和 redo。专项 1,036 项、原生历史累计 96,406 项及真实 PMX／PMD 控件、插件交错、保存和生命周期回归通过。浮动侧为隐藏独立控件验证；视角文本及透视切换另列，滑杆仍待迁移，见[验证记录](records/abi-camera-text-input-2026-10-17.md)。 |
| P3 | 灯光颜色／方向文本 `461..466` | 本机已验证 | 六字段记录和 CPU 灯光镜像原子发布；提交后及历史安全刷新时同步滑杆与 GPU。保留旧读取和换算，未修改／同值／失败保留 redo。专项 580 项、原生历史累计 96,986 项及真实 PMX／PMD 插件交错、设备读回、保存和生命周期通过。滑杆已另列迁移项，见[文本验证记录](records/abi-light-text-input-2026-10-17.md)。 |
| P3 | 灯光六滑杆 `455..460` | 本机已验证 | 复用灯光共同历史记录，保留 RGB／256 与方向／100 换算；自身捕获、失败回读、浮点刻度稳定、跨帧和池身份通过。专项 4,535 项；真实 PMX／中文改名 PMD 插件混合历史、GPU 灯光读回与保存通过，见[本轮记录](records/abi-light-morph-sliders-2026-10-17.md)。 |
| P3 | 视角文本 `448` | 本机已验证 | 保留旧整数读取、精确旧值恢复及滑杆限位显示；安全刷新当前透视／正交矩阵，不在历史发布中调用 GPU。专项 73 项、原生历史累计 97,059 项及真实 PMX／PMD 插件交错、设备投影读回、零／极小正交距离、保存及生命周期通过。视角滑杆已另列迁移项，见[文本验证记录](records/abi-fov-text-input-2026-10-17.md)。 |
| P3 | 视角滑杆 `447` | 本机已验证 | 复用投影记录，按有效位置提交，保留原生刻度转换、精确旧 float、模式及播放头；自身捕获、重复通知、失败回读、扩容及替换身份拒绝通过。专项 539 项、原生历史累计 104,594 项、ABI 1,694 项及真实 PMX／中文改名 PMD 插件交错、设备投影与保存通过。普通 `mms.edit` 仍关闭，见[验证记录](records/abi-fov-slider-2026-10-17.md)。 |
| P3 | 相机透视切换 `446` | 本机已验证 | 与视角文本共用投影记录，模式单字段撤销及原模式字节保持；安全刷新用当前视角／距离更新设备。专项 197 项、原生历史累计 98,884 项、ABI 1,694 项及真实 PMX／PMD 插件交错、矩阵读回、正交边界保护、保存及生命周期通过，见[验证记录](records/abi-camera-projection-2026-10-17.md)。 |
| P3 | 四类 Morph 权重文本 `506/511/516/521` | 本机已验证 | 单权重记录保留七字符读取、精确 float、初次滑杆 double 换算和负／超一权重；身份及结构校验，刷新不覆盖未提交文本。专项 753 项、原生历史累计 99,637 项、ABI 1,694 项及真实 PMX／PMD 插件交错、删除恢复、读数、保存和生命周期通过，见[验证记录](records/abi-morph-text-input-2026-10-17.md)。 |
| P3 | 四类 Morph 滑杆 `505/510/515/520` | 本机已验证 | 复用单权重共同历史记录，原模型身份、精确权重和逐位置顺序保留；自身捕获、拒绝回读及 ENDTRACK 刻度稳定通过。专项 1,914 项；完整原生历史 111,043 项、ABI 1,694 项及实际 PMX／PMD 插件交错、保存通过。普通 `mms.edit` 仍关闭，见[本轮记录](records/abi-light-morph-sliders-2026-10-17.md)。 |
| P3 | 附件八字段文本 `478..485` | 本机已验证 | 保留七字符读取、角度换算、缩放及透明度规则；按稳定对象和池身份记录单字段。专项 725 项、原生历史累计 97,784 项、ABI 1,694 项及真实 PMX／PMD 插件交错、删除恢复后撤销、保存和生命周期通过。普通 `mms.edit` 仍关闭，见[验证记录](records/abi-accessory-text-input-2026-10-17.md)。 |
| P3 | 附件显示／加算／投影开关 `476/477/486` | 本机已验证 | 与附件数值共用稳定身份和单字段记录，保留加算不激活轨道的差异；拒绝恢复复选框。同值保留 redo，原字节可精确恢复。专项 602 项、原生历史累计 98,687 项、ABI 1,694 项及真实 PMX／PMD 插件交错、删除恢复、保存及生命周期通过，见[验证记录](records/abi-accessory-flags-2026-10-17.md)。 |
| P3 | 自阴影距离文本及三种模式 `561..564` | 本机已验证 | 保留增强版整数／范围换算、精确旧距离及模式恢复；距离和模式独立撤销。专项 301 项、原生历史累计 98,085 项、ABI 1,694 项及真实 PMX／PMD 插件交错、控件恢复、保存和生命周期通过。直接滑杆 `560` 已另列迁移记录，见[验证记录](records/abi-shadow-parameters-2026-10-17.md)。 |
| P3 | 自阴影距离滑杆 `560` | 本机已验证 | 保留旧整数限位及 double 转 float，按每个有效位置提交共同历史；自身捕获、重复通知、失败回读、轨道选择及跨帧模式撤销通过。专项 1,206 项、原生历史累计 104,055 项、ABI 1,694 项及真实 PMX／中文改名 PMD 插件交错、保存、通用重开与生命周期通过。普通 `mms.edit` 仍关闭，见[验证记录](records/plugin-abi-shadow-slider-2026-10-17.md)。 |
| P3 | 坐标显示菜单／主面板／浮动分派 `215/557` | 本机已验证 | 三入口共用单字节工程记录，恢复菜单和双面板，无操作／失败保留 redo。专项 492 项、原生历史累计 100,129 项、ABI 1,694 项及真实 PMX／PMD 插件交错、保存和生命周期通过；浮动完整鼠标交互仍待验，见[验证记录](records/abi-grid-display-2026-10-17.md)。 |
| P3 | 地面阴影浓度文本及滑杆 `248` | 本机已验证 | 四项 RGBA 镜像统一记录，保留文本与滑杆不同精度规则、实时反馈与关闭保留值的语义。普通刷新保留待提交文本及对应当前作者值的原始滑杆刻度。专项 1,049 项、原生历史累计 102,849 项、ABI 1,694 项及真实 PMX／PMD 插件交错、窗口关闭和 PMM 保存通过；证据见 [验证记录](records/abi-shadow-ambient-2026-10-17.md)。 |
| P3 | VPD 姿势导入、菜单 `202` 及拖放 | 本机已验证 | 位置、四元数和原生缩放整体准备及共同撤销，坏文件和失败不留部分修改。模态输入核对来源、恢复 cwd 并拦截递归。专项 1,131 项、原生历史累计 101,800 项、ABI 1,694 项及真实 PMX／PMD 插件交错、离线重做、菜单与 shell 拖放通过；见 [VPD 验证记录](records/abi-vpd-import-2026-10-17.md)。 |
| P3 | 地面投影显示与透明混合 `221/254` | 本机已验证 | 与坐标显示共用工程状态记录，单字段撤销保持其他显示值，保留原输入消费信号。专项 540 项、原生历史累计 100,669 项、ABI 1,694 项及真实 PMX／PMD 插件交错、菜单读回、保存和生命周期通过；浓度窗口已单独接入，见对应实施项，见[验证记录](records/abi-projected-shadow-flags-2026-10-17.md)。 |
| P3 | 骨骼关键帧池身份及可取消扩容 | 本机已验证 | 扩容保持身份，销毁重建取得新身份；准备不改当前分配，发布保留旧块，回滚恢复指针/容量/访问标记，接受后才释放旧自有块。借用池不释放原数组。 |
| P3 | 独立缩放及自由曲线作者快照 | 本机已验证 | 捕获、克隆、私有修改及无分配交换；保留旧 30 槽环。缩放不覆盖姿态；自由曲线保留 nativeHash。含空存储取消、过期拒绝和同值双交换保护。 |
| P3 | 当前帧选中骨骼登记 | 本机已验证 | 按钮 500 显式绑定下支持插入、覆写、自动插值、缩放和跨模型共同撤销；私有计划与原子发布不写旧环。物理开关变化拒绝；普通宿主仍用旧路径。 |
| P3 | 当前帧表情登记 | 本机已验证 | 四个表情登记按钮接入私有计划，保存键和三族标记；原始曲线保留，首次提交不求值，往返只重采目标表情。私有扩容、选择回放、故障与真实 PMX／PMD 控件通过；普通宿主仍用旧路径。 |
| P3 | 当前帧显示、IK 与外亲登记 | 本机已验证 | 按钮 438 接入私有计划，深层数组与键池扩容在发布前准备；首次提交不求值，往返按当前帧恢复关联状态。原生对照、故障与真实插件交错撤销通过；普通宿主仍用旧路径。 |
| P3 | 相机、灯光、附件与自阴影登记 | 本机已验证 | 按钮 452／468／487／565 接入私有计划，附件七项输入和阴影距离与键共同提交。扩容、完整选择回放、父对象身份、原始曲线、跨帧局部求值及真实插件与导入交错历史通过；普通宿主仍用旧路径。 |
| P3 | 模型三族删键与混合范围缩放 | 本机已验证 | 显式绑定下将骨骼、表情、显示／IK／外亲键合为一条历史；范围对话框支持 7 种勾选组合，保留旧槽序覆盖、名称匹配与插值规则，共享最大帧、游标和标记只发布一次。空操作和整笔失败保留 redo。 |
| P3 | 模型插值粘贴及重置 | 本机已验证 | 显式绑定下接入按钮 431／432，支持 X、Y、Z、旋转、缩放及全部通道。一次操作一条历史，恢复原生及自由曲线作者数据；往返保留当前姿态、播放头和后来调整的选中标记，无变化保留 redo。 |
| P3 | 相机插值粘贴及重置 | 本机已验证 | 显式绑定下接入按钮 431／432，支持位置、旋转、距离、视角及全部通道。只改选中现有键，不跳帧、不覆盖未登记姿态和后来选择；无变化保留 redo。已验证自由曲线在扩容后先读取或保存、与相机登记及插件编辑交错撤销；普通宿主仍用原路径。见[验证记录](records/abi-camera-interpolation-2026-10-17.md)。 |
| P3 | 五类全局曲线及窗口历史 | 受控路径本机已验证 | 相机、灯光、自阴影、重力和附件的写入及窗口 VMD 读取统一显式提交；撤销直接使用共同队列。原始作者状态、精确相机片段、换帧局部求值、后续选区及输入焦点保留。原生专项 3,977 项及真实混合窗口通过，见[记录](records/abi-curve-external-history-2026-10-18.md)。 |
| P3 | 模型骨骼、表情及显示／IK 曲线窗口 | 受控路径本机已验证 | 表情变化轨道、骨骼原生柄与自由九通道、多骨骼及显示／IK 一次私有准备和显式提交。跨模型、跨帧插件交错撤销，原始作者与缩放恢复，选区、dirty 姿态、焦点、预算及逐分配失败通过。骨骼专项 3,730 项、表情专项 3,392 项、外部窗口历史 124 项；真实窗口通过，见[记录](records/abi-model-curve-history-2026-10-18.md)。 |
| P3 | 模型 VMD 骨骼、表情自由曲线尾段 | 受控路径本机已验证 | 标准键与尾段共用私有池，作者数据、缩放和元数据一次显式提交。保留旧 VMD 消费、匹配及跳过规则；专项 43,251 项、完整 18 组及原版四组回归通过，真实 PMX/PMD 两模式、偏移、旧函数缩放精确对照、删源混合历史通过，见[记录](records/abi-model-vmd-curves-2026-10-18.md)。 |
| P3 | 删键及范围操作后的局部姿态求值 | 本机已验证 | 每个方向按当前帧采样受影响的干净骨骼，保留未登记姿态；真实 Seek 位级对照、自由曲线、换帧跨模型撤销和无分配发布通过。物理模式切换和 IK 启动仍拒绝。 |
| P3 | 显示、IK 及外亲键删除与当前帧求值 | 本机已验证 | 深拷贝键数据，保持嵌套缓冲区所有权；每个方向重建可见性、IK 状态和外亲窗口。关联骨骼使用目标窗口重新求值，未登记姿态保留。旧 Seek 对照、实际按钮与插件撤销、扩容及失败检查通过。 |
| P3 | 表情键删除及原始曲线保留 | 本机已验证 | 私有目标键链与作者快照采样，不清除原始曲线及切线；受影响权重按当前帧重算。旧函数位级对照、曲线恢复、跨模型历史、池扩容及分配失败通过。 |
| P3 | 三族删键原子性与依赖校验 | 本机已验证 | 统一准备、统一过期校验、无分配发布；跨池别名、保护页小池替换、准备后新增关联骨骼、后序失败和当前帧变化均有回归。表情曲线在相机模式下及插件重做后同步刷新。 |
| P3 | Manager 显式编辑服务与真实插件混合历史 | 本机已验证 | 受控宿主附加同一 SharedHistory，sceneRevision 与 begin 校验一致；顶层命令及队列编辑、失败取消、发布通知、缓存表分离、模态泵刷新重试、PMM 往返及关闭通过。普通启动不启用。 |
| P3 | 对象删除与历史按钮刷新 | 本机已验证 | 显式绑定下接入 437／473，确认、预算拒绝及忙碌不回退旧释放逻辑；发布先修正内部选择，失败与延迟刷新按稳定 ID 保留注册范围。界面刷新未完成时阻止保存和后续历史操作。 |
| P3 | 实测插入候选 | 本机已验证 | 无固定字节参数的单项及批量插入候选复用目标计量，并计入包装记录；精确预算边界、取消、往返、增长拒绝及失败保留 redo 通过。这些结果针对已加载对象的内部组件，不替代私有加载入口验证。 |
| P3 | 模型及附件私有导入准备 | 本机已验证 | 独占对象及关键帧、局部路径、私有纹理增量和模型物理候选；准备失败或取消回收本次资源，不写活动槽位。VAC 保留源路径并按实际 X 目录解析纹理。已加载旧效果 hook 时保守拒绝；EMM、EMD 仍显式加载。分配失败、物理准备、纹理取消及六类真实 GPU 缓冲写入故障通过。 |
| P3 | 受控导入及文件选择 `435`、`472` | 本机已验证 | 显式绑定后由 `ImportModel`、`ImportAccessory` 形成一个实测历史节点，提交后刷新选择及列表，往返复用保留资源。picker 使用绝对初始目录，返回后恢复 cwd；取消或失败不改记忆目录、路径工作区及当前姿态。PMX 原名、PMD 中文名宿主通过导入及离线撤销、重做。 |
| P3 | 原生统一撤销及可写属性 | 未开放 | 部分姿态命令、骨骼／表情／显示和四类全局登记、删键、范围缩放、插值、对象删除与私有导入已受控验证。其余原生写入、全局编辑和外部资源仍待迁移。普通启动查询 `mms.edit` 仍返回 NOT_FOUND，可写属性仍为 NOT_READY。 |
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

C++17 UI SDK 整合后，ABI 契约累计 **1,694** 项通过，新增 **171** 项。示例通过独立 CMake 构建，并在真实宿主原名及中文改名下验证菜单和按钮共用状态、布局重新执行后的调用与退出。当前实现和限制见 [SDK 手册](../sdk/mms/cpp-ui.md)及[验证记录](records/abi-cpp-sdk-2026-10-17.md)。本批未修改宿主源文件和 C 函数表。

相机、灯光、附件和自阴影登记整合后，原生历史累计 92,245 项检查、ABI 契约 1,382 项和 16 组相关 CTest 全部通过。真实 PMX／PMD 控件覆盖四类全局登记、既有表情与显示登记、跨帧插件交错撤销，以及全局登记与附件私有导入混合往返。故障注入覆盖 C++ `operator new`，不包含系统 `malloc`／`calloc` 失败注入。普通启动仍不提供 `mms.edit`。

统一 C ABI 与分层 SDK 的依赖、对象身份及实施顺序见[扩展架构](PLUGIN_EXTENSION_ARCHITECTURE.md)。复杂美化、事件驱动动画和四类刷新见 [UI 绘制与交互方案](PLUGIN_UI_PRESENTATION.md)；多视口、不同渲染器及洋葱皮见[视口和渲染服务方案](PLUGIN_VIEWPORT_RENDERING.md)；格式、公共与专用数据、编辑器及原子历史见[数据扩展方案](PLUGIN_DATA_EXTENSIONS.md)。这些文档描述后续实现，不代表当前已提供相应服务。

本轮已把 ABI 源码迁移到独立工作目录，保留 `codex/plugin-abi` 分支名；原目录回到 `main`。目录、备份及构建来源见[源码隔离说明](development/abi-source-isolation.md)。审查修复全局登记重复选择清空其他轨道、撤销后参数控件未同步、有限灯光键采样溢出，以及事务取消和析构阶段的历史重入。此次原生历史累计 **92,273** 项、ABI 契约 **1,523** 项通过；动画宿主测试、真实全局控件与插件历史往返及普通宿主启动也通过。此前 16 组 CTest 为上批结果，本轮仅重跑 ABI 契约和动画宿主两组 CTest，另执行下列独立宿主探针。普通宿主仍不提供 `mms.edit`。

| 本轮验证 | 结果 | 独立 ABI 工作目录内日志 |
| --- | --- | --- |
| 原生历史 | 92,273 项；覆盖失败原子性、采样溢出及安全时间点往返 | `build/abi-review-native.log`、`build/abi-review-native/report.json` |
| ABI 契约及动画宿主 | 1,523 项 ABI 检查，两组 CTest 通过；覆盖未准备事务阻止历史、取消和析构回调重入、正常转发 | `build/abi-review-ctest.log` |
| 四类全局控件 | PMX 原名、PMD 中文改名；撤销/重做读数同步，多轨道选择保持和插件交错历史通过 | `build/abi-review-global-host.log` |
| 插件编辑寿命 | 真实插件历史、工程重置、重开及关闭通过 | `build/abi-review-edit-host.log` |
| 普通宿主 | 原名和中文改名；菜单、工具栏、面板与关闭通过 | `build/abi-review-smoke.log` |
| 构建与导入检查 | 完整宿主及四个测试目标编译；五个二进制未直接或延迟导入所检查的 DX12、DXGI、d3dcompiler_47、CUDA、NVAPI 库 | `build/abi-review-build.log`、`build/abi-review-pe.json` |

本轮本机结果不替代 Win7 或 AMD 真机验收。上述检查未发现的故障仍可能存在，不能据此声明所有原生写入或新设计已完成。

私有导入最终记录为 `build/abi-private-import-native-3/report.json` 和 `build/abi-private-import-host-2/report.json`。原生历史累计 36,842 项通过；PMX 原名、PMD 中文名宿主均返回 0，包含主顶点、轮廓顶点及索引缓冲各自 Lock／Unlock 六类故障注入。四处新增解析索引检查经过静态复核，本批未新增其畸形输入运行用例，不能视为解析器的完整安全验证。新增纹理在同一 `Collector` 内去重，不同历史节点仍可保守重复收费；已发布缓存归渲染器管理并跨撤销保留，未实现全日志共享账本。

| 验证 | 结果 | 日志 |
| --- | --- | --- |
| 全局及附件登记原生回归 | 完整历史累计 92,245 项通过；旧注册器和求值对照、扩容及尾部选择、父对象替换、原始曲线、跨附件往返、无分配发布和分配失败 | `plugin-abi-global-registration-native-final.log`、`abi-global-registration-native-final/report.json` |
| 全局登记真实宿主 | PMX 原名、PMD 中文 EXE 均 exit 0；四按钮、真实 DLL、跨帧混合历史、七字符输入、零帧覆写、无变化与拒绝、附件导入后的池身份恢复 | `plugin-abi-global-registration-host-1.log`、`abi-global-registration-host-1/report.json` |
| 全局登记整合回归 | ABI 1,382 项及 16 组 CTest、既有模型登记、私有导入、对象历史、原生／插件历史与普通启动均通过 | `plugin-abi-global-registration-ctest.log`、`plugin-abi-global-registration-model-host.log`、`plugin-abi-global-registration-import.log`、`plugin-abi-global-registration-object.log`、`plugin-abi-global-registration-edit.log`、`plugin-abi-global-registration-smoke.log` |
| 全局登记构建与平台边界 | 18 个目标构建通过；宿主 x64、子系统 6.1，六文件无清单所列可选图形／厂商 DLL 直接或延迟导入。资源缓存复核后恢复 38 个工程的正常资源命令。Win7／AMD 真机未验收 | `plugin-abi-global-registration-build-2.log`、`plugin-abi-global-registration-build-3.log`、`plugin-abi-global-registration-build-checks.log`、`plugin-abi-global-registration-pe.json`、`plugin-abi-global-registration-restore.log` |
| 表情及显示登记原生回归 | 专项 2,118 项、完整历史累计 38,960 项通过；旧函数对照、深层数组、扩容、跨模型与跨帧往返、完整选择回放、原始曲线、无分配发布及 C++ 分配失败 | `plugin-abi-registration-focused-1.log`、`plugin-abi-registration-native-final.log`、`abi-registration-native-final/report.json` |
| 表情及显示登记真实宿主 | PMX 原名与 PMD 中文 EXE 均 exit 0；真实按钮、真实编辑 DLL、单次事务交错、未登记姿态保持、无变化及拒绝不回退旧环 | `plugin-abi-registration-host-probe-2.log`、`abi-registration-host-2/report.json` |
| 插件待提交事务与相关回归 | ABI 1,382 项及 16 组 CTest 通过；打开和已准备插件事务均阻止独立原生 prepare、commit、undo／redo；取消后可重新提交，插件正常提交及混合事务保持可用 | `plugin-abi-registration-regressions-final.log` |
| 登记整合后的导入、对象与启动 | PMX／PMD 私有导入、原生／插件历史、对象恢复、DX9 重置、工程边界及原名／中文改名普通启动、菜单、工具栏和面板通过 | `plugin-abi-registration-import-final.log`、`plugin-abi-registration-edit-final.log`、`plugin-abi-registration-object-final.log`、`plugin-abi-registration-smoke-final.log` |
| 登记整合构建与平台边界 | 18 个目标构建通过；宿主 x64、子系统 6.1，六文件无清单所列可选图形／厂商 DLL 直接或延迟导入。复用已核验资源缓存后恢复 38 个工程的正常资源命令。Win7／AMD 真机未验收 | `plugin-abi-registration-build-final.log`、`plugin-abi-registration-pe.json`、`plugin-abi-registration-resource-restored.log`、`plugin-abi-registration-resource-restored-counts.json` |
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
| 模型插值按钮与真实插件历史 | 原生历史累计 30,321 项通过，包含 24 组旧按钮对照、当前姿态及后来选中标记保留、无变化、过期和分配失败。PMX 原名及 PMD 中文 EXE 通过真实按钮、重复粘贴、插件跨帧撤销、原生重做和工程边界；普通启动及插件 UI 探针也通过 | `plugin-abi-resource-interpolation-native-1.log`、`plugin-abi-resource-interpolation-host-1.log`、`abi-resource-interpolation-host-1/report.json`、`plugin-abi-resource-smoke-1.log` |
| 文本容量与物理目标计量 | 新增检查随原生历史累计 30,321 项通过；实际分配容量、早 NUL、失败清理、名称替换、去重、候选目标快照、世界增长和分配失败均有覆盖。11 组相关 CTest 通过，其中 PMX 文本包含 98 个畸形输入及失败重试 | `plugin-abi-resource-interpolation-native-1.log`、`plugin-abi-resource-interpolation-regressions-1.log` |
| 真实文本寿命与物理编辑窗口 | PMX/PMD 的活动及已删除模型均能测量全部自有文本；对象恢复、批量历史与 DX9 重置通过。独立桌面的物理编辑窗口在实际 ACP 936 下完成 816 项检查、12 次窗口操作，无失败 | `plugin-abi-resource-object-1.log`、`abi-resource-object-1/report.json`、`abi-text-physics-editor-final/report.json`、`abi-text-physics-editor-final/result.log` |
| 工程文本替换与设置回归 | 不带可选模型的完整设置测试通过，覆盖 Unicode 元数据、保存重开、另存、未知及损坏扩展。带模型版本被兼容副本提示中断，未计为通过；PMX/PMD 工程往返另由真实编辑及对象探针覆盖 | `plugin-abi-text-project-empty.log`、`abi-text-project-empty/report.json`、`abi-text-project-final/result.log` |
| 本批构建与平台边界 | 16 个目标已构建；宿主仍为 x64、子系统 6.1，六文件无所列可选后端或厂商 DLL 直接导入。已核验资源缓存并恢复 38 个生成项目的正常资源命令。Win7 与 AMD 真机未验收 | `plugin-abi-resource-interpolation-build-1.log`、`plugin-abi-resource-tests-build-2.log`、`plugin-abi-resource-interpolation-pe.json`、`plugin-abi-resource-interpolation-resource-restored.log` |
| 实测预算、对象删除与插入候选 | 原生历史累计 36,156 项通过；覆盖旧撤销容量与释放、Bullet 私有容量和目标快照、准备后增长、动态费用、三语言真实确认框、重入与分配失败、刷新失败及附件范围保持。实测插入覆盖精确预算、混合批次、取消与所有权、无分配发布 | `plugin-abi-object-command-native-2.log`、`plugin-abi-object-insertion-native-1.log`、`abi-object-insertion-native-1/report.json` |
| 实测预算整合后的相关回归 | 15 组 CTest 全通过，含 ABI、VMD 加载、骨骼登记与缩放、物理姿态与烘焙、稠密播放、对象列表、模型加载 UI、曲线宿主、快捷键、PMX 文本及附件提示 | `plugin-abi-object-regressions-1.log` |
| 对象资源与普通宿主回归 | PMX 原名与 PMD 中文 EXE 两组通过对象保留、源文件离线、DX9 重置及工程边界；普通原名／中文名宿主启动、插件菜单、工具栏、面板和场景通过 | `plugin-abi-object-budget-host-1.log`、`abi-object-budget-host-1/report.json`、`plugin-abi-object-smoke-1.log` |
| 真实删除菜单与插件恢复 | PMX 原名及 PMD 中文 EXE 均通过实际 437／473 删除和真实 DLL 的 undo/redo；源文件离线、原模型 VB／附件网格与键池复用、同 ID 新代次、旧快照寿命及延迟列表刷新通过。既有混合编辑、PMM 保存重开、新建及关闭也通过 | `plugin-abi-object-command-host-2.log`、`abi-object-command-host-2/report.json` |
| 对象删除阶段构建与平台边界 | 17 个目标构建通过；宿主 x64、子系统 6.1，六文件无清单所列可选后端或厂商直接导入。资源缓存经核验，构建完成后恢复正常生成命令。原始兼容 profile 本批未重建，Win7／AMD 真机未验收 | `plugin-abi-object-regressions-build-1.log`、`plugin-abi-object-final-build-1.log`、`plugin-abi-object-final-build-2.log`、`plugin-abi-object-final-pe.json`、`plugin-abi-object-resource-restored.log` |
| 私有导入与取消 | 原生历史累计 36,842 项通过，含私有纹理与物理候选、分配失败、预算拒绝、身份回收、取消和无分配发布；PMX／PMD／VAC 实际导入、六类 GPU 缓冲故障、文件选择取消与失败、源文件离线往返均通过 | `plugin-abi-private-import-native-3.log`、`abi-private-import-native-3/report.json`、`plugin-abi-private-import-host-2.log`、`abi-private-import-host-2/report.json` |
| 私有导入整合回归 | 15 组 CTest 全通过；既有实际删除与混合插件历史、对象保留及 DX9 重置通过；普通原名与中文名宿主的启动、菜单、工具栏和面板通过 | `plugin-abi-private-import-regressions-1.log`、`abi-private-import-edit-1/report.json`、`abi-private-import-objects-1/report.json`、`plugin-abi-private-import-startup-1.log` |
| 私有导入构建与平台边界 | 17 个目标构建通过；宿主 x64、子系统 6.1，六文件无清单所列可选后端或厂商 DLL 导入。资源缓存核验通过，38 个生成项目恢复正常资源命令。原始兼容 profile 本批未重建，Win7／AMD 真机未验收 | `plugin-abi-private-import-regressions-build-2.log`、`plugin-abi-private-import-pe.json`、`plugin-abi-private-import-resource-restored.log` |
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

本批审查发现表情登记只回放稀疏选择标记，撤销后新增的选择可能残留；现按记录的原容量清理三族标记，再恢复目标选择。首次真实登记探针又发现待提交插件事务未阻止独立原生登记；现由共同历史入口拒绝原生准备、提交及历史遍历，保留插件正常提交路径。两项均补回归；首次失败记录 `plugin-abi-registration-host-probe-1.log` 不计为通过，最终结果采用 `host-probe-2`。

对象删除阶段的首次完整原生测试触及旧 60 秒总时限；增加逐段日志并扩展本地运行器时限后，36,016 项通过，加入实测插入测试后累计 36,156 项通过。真实菜单探针首次由测试框架的 MessageBox 拦截器提前退出；现只在测试作用域内精确放行一次预期删除确认，仍创建并验证真实窗口，其他弹窗继续报错。两种 EXE 名称最终通过，以上中断均未计作成功。

对象资源这一轮最初的曲线宿主测试在缩放文字重绘断言失败。夹具将标签放在固定坐标，已不在当前底栏范围内；现按 `FramePanelLayout.bottomBand` 定位。新增嵌套夹具随后暴露测试栈溢出：`MMDAppState{}` 赋值生成约 680 KB 的临时对象。改为原地初始化后，曲线宿主及整组最终回归通过，未修改生产绘制逻辑。池扩容的嵌套测试同样改为原地初始化。

早期多目标构建因本机未配置 `physics_pose_tests.vcxproj` 失败，不能作为整组成功证据。最终构建使用已配置的五个目标，见 `plugin-abi-object-history-batch-final-build.log`。ABI 测试的临时文件曾因沙箱临时目录写入被拒绝而失败；将该进程的 `TEMP` 和 `TMP` 设为仓库内 `build/abi-tmp` 后，700 项通过，最终整组复验也通过。

侧栏测试的附件排序用例缺少初始选中项。关闭新 ABI 初始化时也复现同一失败，见 `plugin-abi-accessory-without-abi.log`。测试已明确初始选择并验证排序后保持选择；附件业务逻辑未修改。

首次双面板测试把 SURFACE_DESTROY 计入 DESTROY 子串数量。已改按完整事件行计数并复跑，保留视图先销毁、插件随后停止和销毁的顺序检查。

几何测试发现 PMX 加载器为附加 UV Morph 保存基准时复制了普通 UV。增强构建现按各附加 UV 通道保存 float4，原始兼容构建保持旧路径。测试从原创 PMX 文件加载后核对原始值，再修改工作数组，验证绑定快照恢复原值且不改写当前 Morph 状态；现有 Morph 并行计算回归也通过。最初 PMX 2.1 夹具被现有导入器拒绝，因此文件测试改用实际支持的 PMX 2.0，QDEF 在运行时记录上单独测试。

统一调度测试启动隔离的隐藏窗口，只替换该测试窗口的可见性查询及绘制损伤矩形。场景、物理、GPU、编码器及输出仍走 MMS 实现。测试覆盖多次 UI 绘制零场景增量、视口暴露合并、绘制失败重试、动态刚体保持、实际窗口尺寸变更、模态与编辑锁、播放推进、设备丢失与取消。该阶段的侧栏和底部仍在松手时提交布局；当前分支已按后续用户反馈改为实时布局，见 P2e 修复记录。

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
