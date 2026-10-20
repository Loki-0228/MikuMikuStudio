# 工程地面开关共同历史验证（2026-10-21）

`codex/plugin-abi` 的菜单 `285`「Floor／床」已接入受控共同历史。该入口同时修改工程字段 `MMDAppState::floorVisible` 和当前地面刚体的碰撞 flags。地面不是单纯的界面显示偏好，关闭会禁止其接触响应。本批未开放普通公共 `mms.edit`，正式 release 保持不变。

## 原生入口与内部接口

[command_view_menu.cpp](../../src/window/command_view_menu.cpp) 的 `case 285` 在完整配置中先调用 `TryToggleBoundFloor`。存在内部共同历史绑定时，该函数消费成功、同值及拒绝，不回落旧写入。播放、模态窗口、鼠标捕获、工程保存、历史忙碌和插件事务 pending 均受准备阶段检查。

[native_floor_history.hpp](../../src/plugins/native_floor_history.hpp) 提供以下 C++ 内部接口，没有新增公共 C ABI。

| 接口 | 行为 |
| --- | --- |
| `PlanFloor` | 捕获并检查来源，准备私有 `HistoryChange`，不修改正式字段。成功的同值操作返回空计划。 |
| `ApplyBoundFloor` | 将显式布尔目标提交给共同历史，单独报告是否已经发布变化。 |
| `TryToggleBoundFloor` | 按当前作者字节决定切换方向，并消费绑定入口的拒绝。 |
| `RefreshBoundFloorReadout` | 从作者字节更新菜单 `285`，不重新求值场景。 |

完整配置的未绑定入口和 `MMD_ORIGINAL_PROFILE` 保留原逻辑。原菜单的 `dialogFlags[15]` 仍属于旧路径的临时输入信号，不加入工程历史。受控路径的拒绝从作者值恢复菜单勾选。

## 原始字节、碰撞 flags 与记录模式

`FloorChange` 保存原始 `floorVisible` 字节及地面刚体的完整 `getCollisionFlags()` 值。目标启用写 `CF_STATIC_OBJECT`，目标关闭写 `-1`，与旧菜单一致。撤销恢复完整 flags，不只恢复是否有 `CF_NO_CONTACT_RESPONSE`。

同值判断同时检查字节的布尔含义和真实 flags。两者均匹配时不发布、不标记工程修改、不增加节点，也不清除 redo。原字节为 `7` 或 `255` 时，显式设置启用不归一原字节。若布尔含义相同但真实 flags 不匹配，宿主准备一次实际修复，保留原始字节，并按记录模式处理该变化。撤销可以恢复原先不一致的 flags。

菜单切换和内部显式目标均使用 `NativeIntent::Adjustment`。

| 历史模式 | 实际变化的行为 |
| --- | --- |
| `AllOperations` | 同时发布作者字节与实际 flags，增加一个共同历史节点。 |
| `ExplicitOnly` | 同时发布两个字段并推进工程版本，不增加节点，保留已有 redo。 |

取消尚未发布的候选或 `NativeTransaction` 不修改正式字段。准备失败、预算拒绝和 C++ 分配失败保留原始字节、flags、旧 undo、旧 redo 及保留预算。

## 资源身份与发布边界

[native_floor_history.cpp](../../src/plugins/native_floor_history.cpp) 的来源检查先核对历史绑定、工程 `SceneGeneration()`、相机键池身份和当前 `PhysicsScene`。随后核对 world、groundBody、broadphase、dispatcher，以及地面在当前 collision object 数组中恰好出现一次。只有这些检查通过后，代码才通过当前地面指针读取 proxy、形状、motion state、flags 和质量。移除并释放旧地面后，来源检查不会用旧指针查询资源。

AxisSweep 的 handle 和 UID 可以在移除后复用。记录还保存原生 ray DBVT proxy 的独立 UID，以拒绝刚体在同一内存地址、同一 Sweep 槽重建后误用旧历史的情况。替换工程根、场景、世界、地面、形状或碰撞掩码均使相应记录过期，拒绝时不移动历史游标。

每个短期候选另捕获当前帧、绑定代次、记录代次、世界 `StepSerial()`、当前作者字节、当前 flags，以及 collision object 和 constraint 的完整顺序。准备后发生步进、重排或来源变化时，候选拒绝发布。正常工程键池扩容保留身份，记录仍可使用。

应用程序继续拥有当前世界和地面刚体。记录只保留来源描述及标量值，不接管世界资源，也不复制接触缓存。应用程序和当前场景所有者须覆盖历史的使用期，场景边界通过既有共同历史生命周期清理旧记录。缺少原生 `DynamicsWorld`、带 ray proxy 的 `history_physics::Broadphase`、有效静态地面或完整来源时，准备返回拒绝。

最终发布只调用 Bullet 2.75 的 `setCollisionFlags`、写入作者字节并推进内部记录状态。该 setter 是整数赋值。发布不分配内存，不增删刚体，不替换世界数组，不清理接触算法，不唤醒其他刚体，不求值、不 seek，也不步进或重置物理。真实 CPU 专项同时检查发布前后的世界数组、接触、变换、速度、力、激活状态及时间累积器。

## 绘制通知与共同撤销

首次提交成功后，宿主标记工程修改，更新共同历史按钮和菜单，再调用 `updates::RequestViewport(&app, false)`。该请求只追加 `RenderDirty | PresentDirty`，不追加 `EvaluateDirty`，不推进几何版本。初次提交不调用姿态读回。该行为不排除帧驱动器处理其他已经存在的求值请求。

通知位于数据发布之后，异常边界不会将已经提交的操作报告成失败。专项通过转发真实绘制请求后抛出异常，检查两个目标字段及有效历史节点仍然存在。

共同撤销与重做沿用 `TryBoundHistoryCommand` 和 `RunBoundObjectHistory` 的通用收尾。该收尾允许 `PublishBoundPoseEdit` 调用既有只读 `PostViewRefresh`，并通过 `RefreshBoundGlobalReadout` 同步地面菜单。因此，初次提交的“只请求绘制”不表示共同撤销绕过全部界面读回。专项检查通用路由保留后来未登记的模型姿态、当前模式、播放头和既有 `PhysicsResetPending`。

## 标准 PMM 边界

PMM 格式没有变化。[pmm_save.cpp](../../src/io/pmm_save.cpp) 将 `floorVisible != 0` 写成一个标准 `0` 或 `1` 字节。该字节位于 config2 的相机附件变换抑制字节之后、跳转帧整数之前。[pmm_load_v2.cpp](../../src/io/pmm_load_v2.cpp) 读取该字节，恢复布尔字段及地面碰撞 flags。旧版读取路径 [pmm_load_v1.cpp](../../src/io/pmm_load_v1.cpp) 保留其原有可选扩展门槛。

原始非零字节和完整 flags 的精确保留属于内存中的共同历史。PMM 不新增原始 flags 字段，保存后仍按原规则归一布尔值。本批只核对既有保存与加载源码，没有新增地面开关 PMM 文件往返测试。

## 本机验收证据

证据目录为 `build/verification/abi-panel-background-lifetime-20261021`。[floor-full-01/report.json](../../build/verification/abi-panel-background-lifetime-20261021/floor-full-01/report.json) 保存实际执行命令、专项参数、计数、退出码和桌面信息。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 完整配置产品与原生测试构建 | 完整依赖重建，退出码 `0`。 | [build-floor-full-09.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-full-09.log) |
| 原始配置产品构建 | 退出码 `0`，保留旧入口。 | [build-floor-original-02.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-original-02.log) |
| `--floor-only` | `1370` 项通过，退出码 `0`。 | [地面专项日志](../../build/verification/abi-panel-background-lifetime-20261021/floor-full-01/0-floor-only/result.log) |
| `--physics-mode-only` | `1901` 项通过，退出码 `0`。 | [物理模式日志](../../build/verification/abi-panel-background-lifetime-20261021/floor-full-01/1-physics-mode-only/result.log) |
| `--black-background-only` | `1463` 项通过，退出码 `0`。 | [黑白背景日志](../../build/verification/abi-panel-background-lifetime-20261021/floor-full-01/2-black-background-only/result.log) |

[native_floor_history_checks.inc](../../tests/native_floor_history_checks.inc) 从实际菜单 `285` 进入，覆盖两种记录模式、原始字节 `0/1/7/255`、完整 flags、同值修复、有效准备取消、预算拒绝和逐点 C++ 分配失败。真实 `EditStore` pending 事务阻止菜单写入后仍能提交。模型 A、地面、插件、模型 B、地面的五个节点按实际顺序撤销和重做。

资源专项实际执行旧地面移除并释放、同地址重建、工程根替换、世界替换和短候选失效。发布检查同时禁止 C++ 分配和 Bullet 分配，覆盖两个方向及正式 journal commit。失败和取消不改变世界存储及容量。

`CpuCollisionSteps` 实际执行真实 Bullet CPU 步进。启用与关闭各自对照未绑定的旧菜单行为，逐刚体比较下一步的位置、速度、flags、激活状态及世界时间累积器。启用地面支撑穿入平面的球体，关闭地面允许球体下落。撤销发生在步进之后，先确认撤销保留新的物理状态，再执行下一步并再次对照旧菜单。这些断言没有用假物理实现或跳过碰撞检查。

## 构建失败记录与验收限制

早期 [build-floor-full-05.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-full-05.log) 因另一批曲线编辑器的 `Snapshot` 静态断言失败。[build-floor-core-isolated-06.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-core-isolated-06.log) 仅证明 floor core 配合已有已验证依赖可以构建，未递归重建依赖，不作为完整产品通过的证据。最终完整产品通过依据是 `build-floor-full-09.log`。

[build-floor-native-isolated-07.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-native-isolated-07.log) 暴露测试摘要代码调用 Bullet 2.75 无 const 重载的 `getTotalForce` 和 `getTotalTorque`。夹具改为从真实 `objects[i]` 获取可变刚体指针，再调用这两个只读 getter，未删除或放宽状态断言。随后 [build-floor-native-isolated-08.log](../../build/verification/abi-panel-background-lifetime-20261021/build-floor-native-isolated-08.log) 达到 COFF 节数限制。原生历史测试目标增加 MSVC C++ `/bigobj` 后，完整构建 `09` 通过。

本轮专项继承 `Default` 桌面，`SW_HIDE` 仅为启动请求，窗口显示由夹具管理。测试只向自有窗口发送消息，没有全局输入注入，也没有真实独立桌面隔离。原始配置本轮只有产品构建证据，没有运行地面专项。

Win7 SP1 x64、CPU 基础路径和 AMD 兼容要求保留。本轮证据包括真实 CPU 物理步，但不包括 Win7 或 AMD 真机、IME、多 DPI、GPU 呈现及鼠标全交互验收。正式 release 未覆盖，未部署或推送。
