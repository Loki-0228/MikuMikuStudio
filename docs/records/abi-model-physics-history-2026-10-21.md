# 模型物理资源与菜单 262 共同历史

完整配置的菜单 262 已使用私有物理编辑会话。刚体、关节、名称及几何修改在确认后一次发布，绑定共同历史时形成一次显式提交。资源版本、计划器、实际窗口及真实 PMX、PMD 加载后的编码回归已通过。原始兼容配置保留旧入口，也完成其支持范围内的实际 PMD 编辑和拒绝路径回归；它没有迁移到新编辑路径。

本批验证使用 `WinSta0\Default` 同桌面的隐藏窗口，报告明确标记 `realPrivateDesktop=false`。它不是独立桌面运行，环境依据见[桌面验证更正](abi-verification-desktops-2026-10-21.md)。

## 资源版本与所有权

`src/plugins/model_physics_resources.cpp` 的 `ModelPhysicsResources::Capture` 先借用模型当前资源。打开后取消或准备失败，不会取得模型原分配的释放权。首次成功发布才通过 `AdoptOwnership` 接管旧基线；模型保留当前版本，历史节点保留前后版本，临时计划销毁不会释放模型仍在使用的资源。

每个版本保留刚体表、关节表、完整名称分配容量、Bullet 刚体、基础形状、motion state、关节，以及 `physics_scale::ResourceState`。名称包含首个 NUL 后的原始尾部，不能只按可见字符重建。缩放版本持有派生 affine 形状、骨骼几何基线及关节基线，撤销与重做不重新构造这些资源。

`NativeModelPhysicsPlan::Take` 在私有 `ImportScene` 建立新表和 Bullet 对象，调用 `ResourceState::PreparePrivate` 准备当前缩放。`AdoptPrivate` 的最后一次可能失败的分配先于 scale 节点提取及 scratch 表清空；失败时私有对象继续负责清理。成功后资源版本接管这些私有分配，首次发布用 `PrepareReplacing` 同时停放旧成员并接入新成员，后续方向用 `Retained::PrepareExchange`。

`RetainedSceneObject` 准备删除或恢复时读取当前物理资源版本，不再固定首次捕获的表。模型先编辑物理、再删除恢复、再撤销较早的物理修改，仍使用相应保留资源。`ModelDispose` 的内部释放 hook 先停放 scale 状态并移除世界成员，再清空模型表，由资源版本释放分配；仅借用的旧模型继续走原释放路径。世界与应用须存活到相关版本和候选清理完成。

完整前后资源及发布后的成员账本参与历史预算。未知名称容量、资源来源失效或计量不完整时拒绝准备，不能只按表或标量字段大小计费。准备和取消不改变实时世界数组存储；该边界的修复见[物理回放存储记录](abi-physics-transition-storage-2026-10-21.md)。

## 私有窗口与预览

`command_view_menu.cpp` 的完整配置菜单 262 调用 `TryEditBoundModelPhysics`。`PhysicsDialogDraft` 捕获模型、工程、绑定和来源身份，并持有独立名称、刚体与关节草稿。原生 Unicode 窗口继续使用已有编辑控件，编辑数组中的列表索引、碰撞掩码和关节端点在确认时转换为紧凑模型记录。

窗口打开不清零正式骨骼姿态。`previewBones` 与 `previewModel` 提供只读预览基线，`debug_geometry.cpp` 从草稿读取预览骨骼，`frame_modes.cpp` 和 `frame_modes_bone.cpp` 将物理轴操作写到私有刚体或关节。Enter 更新草稿，切换列表或页面不产生历史；取消、关闭、外部销毁及释放编辑数组也不求值场景、移动播放头或重置其他模型物理。

打开窗口、成功接受 Enter、支持的按钮或选择操作，以及实际改变草稿的轴操作，通过 `updates::RequestViewport(&app, false)` 提交仅绘制请求。空闲轴轮询和零位移不提交请求；普通 `EN_*` 通知、GETTEXT、绘制及 IME 消息也不作为预览触发源。组合期间的 Enter 和 OK 不接受草稿、不排队，组合结束本身也不排队。排队本身不写历史、不移动播放头、不求值正式场景。

关闭使用独立的 `previewRetired` 标志，一次性提交清除预览的绘制请求，不依赖计划器是否仍可编辑。取消、确认后计划已消费、来源过期、非法输入、同步重入销毁和直接 DestroyWindow 均经过该清理，重复的销毁消息不重复提交。

确认先读取当前控件、校验模型与来源、转换记录并准备完整资源。活动 IME 组合阻止确认，非法数值和过期来源拒绝提交。关闭期间的同步窗口重入受草稿保活及延后数组释放保护，不能从失败的受控路径回退到旧版先释放后重建。

## 提交与回放

`CommitBoundModelPhysics` 用共同历史的 `ExplicitCommit` 准备和提交一个节点。两种历史记录模式都将一次确定记为一次操作；相同草稿返回空变更，保留 redo、原浮点值及未修改名称的完整字节。正式发布只交换已准备的世界成员、scale 节点和表，更新骨骼物理关联标志及增强模型保存提醒，不在发布阶段分配 C++ 或 Bullet 存储。旧缓存及私有世界的清理在发布后完成。

撤销和重做恢复保留资源，不重新读取原模型文件，也不覆盖当前未登记骨骼姿态或重置其他模型的物理。非均匀缩放专项核对基础形状与 affine 支持点、关节坐标及完整名称，连续 12 次往返使用原资源身份。

普通、未绑定共同历史的完整配置窗口也使用同一私有资源发布，但不因此创建公共编辑服务。`mms.edit` 的普通宿主开放状态保持关闭；这些内部 C++ 计划器不是可供插件调用的新公共 ABI。

## 本轮缺陷与断言修正

球体界面只显示半径，旧初始化没有填写隐藏的 Y、Z 尺寸文本。确认收集器把空文本误认为用户修改，导致无变化或有效修改在 OK 时失败。现在私有窗口同时初始化三个尺寸字段，并记录宿主最后填入的文本；未修改字段继续使用原数值，避免两位小数回显降低阻尼、尺寸和视口拖动结果的精度。

早期取消及无变化检查只要求窗口关闭，异常关闭也可能满足断言。新增真实确认检查要求质量和 Unicode 名称写入、一次历史节点成立、隐藏尺寸及未编辑浮点精度保持，并核对无变化 OK 保留 redo。该修复后的实际窗口专项通过 4,534 项；不能用早期关闭断言代替该结果。随后补齐预览队列和清除请求，实际窗口专项增至 6,404 项并通过，原确认及精度断言继续保留。

## 构建与专项证据

最新复核构建为完整配置 [build-reviewed-full-09.log](../../build/verification/abi-physics-property-20261021/build-reviewed-full-09.log)和原始兼容配置 [build-reviewed-original-07.log](../../build/verification/abi-physics-property-20261021/build-reviewed-original-07.log)，两者通过。资源基础构建仍保留在 `build-full-core-04.log`、`build-original-core-02.log`，追加历史目标的构建为 `build-full-history-06.log`。

以下是完整配置的真实 CPU 世界和同桌面隐藏窗口专项。资源结果来自 [native-hidden-05/report.json](../../build/verification/abi-physics-property-20261021/native-hidden-05/report.json)，其中旧窗口结果为 4,534 项；表中窗口行采用最新 [native-reviewed-ui-07/report.json](../../build/verification/abi-physics-property-20261021/native-reviewed-ui-07/report.json)：

| 入口 | 通过项数 | 本批覆盖 |
| --- | --- | --- |
| `--model-physics-resources-only` | 276 | 借用取消、版本接管、原始名称容量、当前资源保活、删除恢复及双向回放。 |
| `--model-physics-only` | 1,105 | 完整计划、两种历史模式、预算拒绝、来源过期、未连接关节保留及未绑定发布。 |
| `--model-physics-preparation-only` | 7,481 | Begin、Set、Take 的逐点 C++ 分配失败、同 plan 重试、非均匀缩放、原模型与其他模型不变、12 次离线回放。 |
| `--model-physics-ui-only` | 6,404 | 原 4,534 项窗口断言及新增 PreviewRequests：实际菜单、确认与精度、预览排队、被动消息不排队、一次性清除和状态不变。 |
| `--physics-exchange-only` | 5,287 | 私有世界数组、取消及失败存储不变、空世界恢复、预算、接触算法与双向交换。 |
| `--physics-membership-only` | 8,106 | 原成员、导入、首次替换与成员交换的组合回归。 |

入口有重叠，不将六项相加作为独立检查总数。准备专项日志记录 Begin 的 62 个、Set 的 10 个、Take 的 549 个 C++ 分配失败点。每个失败后检查实时表、刚体与形状、姿态、名称注册、世界数组地址与容量、缓存及其他模型，再用同一个 plan 重试。正式发布的回归同时禁止 C++ 分配并计数 Bullet 分配。

`PreviewRequests` 观察并转发实际 `updates::RequestViewport`，使用真实更新协调器及其消息窗口检查请求、合并和清理。夹具的主窗口隐藏且没有 renderer，测试只证明真实队列接收请求和工程、历史、播放头等状态不变。它不证明 GPU 绘制、SafePoint 最终呈现或用户可见的预览画面已经验收。

关联资源回归来自 [native-related-06/report.json](../../build/verification/abi-physics-property-20261021/native-related-06/report.json)，同样标记为同桌面隐藏窗口：

| 入口 | 通过项数 | 范围 |
| --- | --- | --- |
| `--scene-resource-history-only` | 864 | 原生预算、对象插入、保留账本、私有导入与对象保留组合。 |
| `--model-names-only` | 9,556 | 既有模型名称编辑与资源历史回归。 |
| `--model-toon-only` | 11,104 | 菜单 261 已迁移的 Toon 文件表及纹理资源历史回归。 |

## 真实文件加载与窗口编码

最新真实文件加载及窗口编码复核报告为 [encoding-reviewed-full-07/report.json](../../build/verification/abi-physics-property-20261021/encoding-reviewed-full-07/report.json)和 [encoding-reviewed-original-07/report.json](../../build/verification/abi-physics-property-20261021/encoding-reviewed-original-07/report.json)，均返回退出码 0，标记 `realPrivateDesktop=false`。对应构建为上述 reviewed 两套日志；早期 `encoding-hidden-full-06`、`encoding-hidden-original-06` 保留为前轮证据。

| 配置 | 检查数 | 失败数 | 对话框计数 | 消息计数 | 实际 ACP | GPU |
| --- | --- | --- | --- | --- | --- | --- |
| 完整 | 1,725 | 0 | 42 | 6 | 936 | 0 |
| 原始兼容 | 1,047 | 0 | 20 | 25 | 936 | 0 |

完整配置检查 PMX 与真正 PMD 加载、三语言窗口、Unicode 及完整名称尾部、20 字节 CP932 边界、私有副本、新正式版本、无变化确认保留原分配，以及参数复制、增删、紧凑编号和类型切换。原始配置实际加载 PMD，以其允许的 19 字节可终止字段验证正向编辑；PMX 及合法但无法被其保存门禁接受的完整 20 字节名称，另外验证明确拒绝且表、姿态、名称和 Bullet 成员不变。没有放宽生产格式门禁。

早期完整配置的 1,295 项中有 16 项要求草稿及新正式版本继续使用旧名称指针，夹具已区分三种所有权并保留容量及尾部断言。早期原始夹具由 PMX 加载再改名称，不能替代 PMD。新增真正 PMD 加载时还修正了测试调用契约：传入零初始化的渲染器包装，设备仍为空；原始配置没有名称容量注册表，只验证其真实可用的指针、终止文本及原始字段，不从可见长度推断容量。相关早期失败和访问异常均不计为通过。

本批窗口回归不代替增强模型文件保存、工程保存重开或最终产品画面验收。

最新 [PE 清单](../../build/verification/abi-physics-property-20261021/pe-inventory.json)列出的两套构建均为 x64（`machine=0x8664`），子系统版本为 6.1。直接导入中没有 D3D11、D3D12、DXGI 或显卡厂商 DLL，既有 DX9 依赖保持。此检查不证明 Win7 运行库部署或 AMD 真机运行已通过。

## 当前构建的宿主基础回归

[production-reviewed-panel-07.log](../../build/verification/abi-physics-property-20261021/production-reviewed-panel-07.log)使用当前 `build/cmake/Release/MikuMikuDance.exe` 和 `lifecycle.dll`，在验证目录分别以原名和中文改名隐藏启动。菜单、按钮、工作区页签、侧栏自适应及退出均通过。

这是既有 `gui-panel` 模式的同桌面隐藏窗口回归，不覆盖新属性行的 `gui-properties-panel`、实际模型物理编辑的 GPU 呈现或真机输入，也不表示下述正式 release 已更新。

## 未完成的验收与能力

真实命名桌面运行仍未成功，上述功能结果均来自同桌面隐藏窗口。原生 `malloc`、`calloc` 或 Bullet 分配返回空指针的故障注入、真实微软拼音、前台焦点、多屏 DPI、GPU 与 SafePoint 预览呈现、Win7 SP1 x64 与 AMD 真机验收仍未完成。其余模型增强、全局纹理重载和外部渲染资源写入继续按各自范围迁移；菜单 262 完成不等于全部原生写入或公共物理编辑 ABI 完成。

本批没有更新正式 `build/release/MikuMikuDance.exe`。该文件仍对应 `9d879c8`，本次只读核对的 SHA-256 为 `180DB2580C99A3578F30B2539451FF82605AEBDEC337DAEBCE8B77EDF86CF65A`，原产物记录见 [INPUT STATE 验收](abi-input-state-2026-10-21.md)。源码和构建验证目标的变化不能写成正式产物已包含本批功能。

Win7 SP1 x64、CPU 基础路径、AMD 兼容及程序目录 `data` 的存储规则保持，不增加特定显卡厂商或新系统接口依赖。
