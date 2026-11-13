# 编辑事务、共同历史与负帧

[返回功能总览](README.md)

本功能按可验收阶段推进，阶段结束回到[日常替代main里程碑](main-parity.md#近期里程碑日常可替代-main)重新排序。新增登记、保存、跨零粘贴及其依赖全部保留，只降低后续扩展优先级；已开放负帧功能的错误仍优先修复。

## 当前推进范围

### 负帧与正负跨零共同历史

纯负绑定删除、粘贴与姿态恢复已实现。最新负帧共同历史专项 3,258 项、基础负帧 407 项通过，包含本页保活对象修复；当前验收（本地证据，未随本文发布）。早期历史模式独立 128 项及原基线仍保留于 `本地验收记录（未随本文发布）`。通过纯负回归不代表跨零完成。

HIS-01 绑定删除、生产420作者复制及负作者结构证据保留。普通负帧跳帧与共同历史现在共用采样器，保留旧v1舍入与插值；相机复用正帧宿主算法。实际回放/跳帧逐字节一致及纯采样专项2785项、相邻198247项通过（本地证据，未随本文发布），full/original构建通过。新作者尚未生产发布；零帧作者/物理视图、保存读取、真实编辑及421/422（本地证据，未随本文发布）继续，HIS03预算、可见/设备验收与整项目标保持开放。

HIS-02 明确未实现：负帧拖动仍通过私有 `History(state)` 保存，目标上界限制为 -1。基础套件通过说明现有私有拖动边界和中断保护有效，不代表共同历史手势、跨零或真实鼠标验收。HIS-03 的生命周期与全局预算仍待进一步核对。

验收要求：有符号播放头；正负候选同笔准备/发布/撤销；从目标正帧候选取零帧基准；保留作者及选区；OOM/陈旧身份零部分写入；手势取消和释放无悬空。

依赖：ABI-00、ABI-01。任务标识：ABI-02。


### 负键生命周期与普通求值核对（HIS-03）

附件的两项风险已实际复现并修复。共同历史回归 3,258 项、基础负帧回归 407 项通过，full/original 核心库均构建通过；原始配置不生成该专项测试目标，未宣称其运行通过。证据见负键生命周期验收（本地证据，未随本文发布）。下表区分已验证范围与剩余工作：

| 范围 | 源码证据 | 必须补齐的验收 |
| --- | --- | --- |
| 删除对象后继续编辑 | 真实共同历史删除附件 A 后，登记相机/B负键的反例已由失败转通过；新增模型 A 删除后编辑相机/B模型的回归也已通过。候选允许未变缺席键保留，禁止改写缺席键；准备后成员状态改变仍拒绝。两种历史模式下撤销、重做、分支及旧负键恢复均通过。 | 模型原分配、平移/旋转/缩放与负键恢复已验证；稳定身份和全局预算属于下列独立未完成项。 |
| 附件槽位复用 | `EvaluateGlobalsImpl` 收集同kind关键帧时补查Current。不同地址A/B同槽、同帧及异帧反例转通过；旧键保留，切回A恢复其专属数据，相机和离槽对象不受影响。 | 同地址分配复用的稳定ID/代次，以及共同历史下同槽新导入仍待验证；当前证据为未绑定求值及另一组真实共同历史缺席对象回归。 |
| 预算和身份 | 单条共同历史候选已有保留/临时预算；私有历史仍最多30份整向量副本，对象资源计数未发现全局负键state计费接点。负键目前按对象/表地址识别。 | 活体、剪贴板、历史和候选全生命周期计费，删除回收与稳定ID/代次闭合；大工程及预算拒绝无半笔结果。 |
| 普通seek | 模型先求零帧再应用负帧；全局ReloadModels暂改播放头到0递归后恢复，再应用负帧。 | 普通seek与独立求值、跨零往返及异常中断后的状态一致。现有纯负局部套件不能替代全部对象覆盖。 |

定位：[共同删除协调器](../../src/plugins/native_signed_key_delete.inc)、[负键存储与求值](../../src/animation/negative_frames.cpp)、[对象历史保活](../../src/plugins/scene_object_history.cpp)、[资源计数](../../src/plugins/scene_resource_budget.cpp)、[模型seek](../../src/model/model_frame_seek.cpp)、[全局seek](../../src/app/timeline_advance.cpp)。方向目标准备方案（本地证据，未随本文发布）为历史设计；当前绑定删除已整合共享额度、保活记账去重、联合源守卫及固定发布顺序。该计数不等于HIS-03活体/剪贴板/历史全生命周期总预算，跨零paste与额外命令/视觉验收继续开放。

### 其余原生命令与生产mms.edit迁移

AttachEditHost仍在migrationTest门内。散布、全部表情、ResetModelSelection等独立记录不等于完整共同历史生产接线。普通默认路径已有同名骨骼/表情跨模型粘贴、更换剪贴板后的撤销重做及两次真实PMM保存重开证据，完整与原始兼容配置通过。同名粘贴与保存重开证据（本地证据，未随本文发布） 本次未启用NativePoseTestBinding，仅补普通路径基线；不代表全部mms.edit生产接线或真实桌面验收。

普通时间轴复制左臂第 10 帧和右臂第 14 帧，在第 40 帧反转粘贴。右臂已有第 40 帧被镜像覆盖，左臂插入第 44 帧，中心轨道和源帧保持。位置 X、四元数 Y/Z 按预期翻转，原生插值和物理标志保留。换掉剪贴板后，一次撤销恢复覆盖前的内容并移除插入帧，一次重做恢复整次操作。真实 PMM 保存重开后，两个镜像关键帧仍正确。 完整和原始兼容 x64 Release 的隐藏 Win32/D3D9 宿主，非活动私有桌面、合成 PMX、受控选区和真实时间轴命令。main 的普通左右匹配、镜像和登记源码相同，ABI 命令另有共同历史入口和分配保护。只对照 main 源码，指定 main 程序未运行。本轮仅覆盖标准左右名称、非负帧和默认编辑路径，前台鼠标操作、重复或复杂左右名称、共同历史及负帧另有范围。 反转粘贴与重开验收（本地证据，未随本文发布）

验收要求：清点生产入口并逐个绑定共同事务、对象保活、预算、保存/重开；全部依赖验证后才评估开放服务。

依赖：ABI-02。任务标识：ABI-03。

骨骼数值框持有原输入对应的工程、帧、模型和骨骼。输入后切换目标、切到相机、删除模型或替换同地址模型身份时，失焦或回车会丢弃旧值，新的骨骼、相机和撤销记录保持。重新输入后，传统撤销和共同历史均能提交新值。 骨骼数值输入目标切换验收（本地证据，未随本文发布）

## 相关专题

### 原生动作插件的读取与写回

以 [MMD-AINakawari v0.2.3](https://github.com/BeamManP/MMD-AINakawari/tree/v0.2.3) 为首个验收场景，分三步推进。此项属于 EDIT-02 的新增能力，不阻塞日常 main 等价里程碑。

1. 提供可版本化的只读动作快照。冻结当前编辑模型身份、播放帧和动作修订，以及实际登记的骨骼键、用户选区、原始插值和生成所需的 IK 状态。求值采样可以补充姿态，不能替代登记键与选区。
2. 提供可撤销的区间写回。按模型身份、动作修订、目标骨骼和帧区间验证后原子提交。区间外的键与曲线、无关骨骼、表情、IK 和外亲都保留；模型被替换、动作已修改、取消或提交失败时不改工程。整批一次撤销或重做。生产开放沿用 EDIT-02 的门禁。
3. 通过 MMS 原生菜单打开独立窗口，复用上游 Python 生成逻辑。验收选区读取、生成、范围写回、撤销重做、保存重开和过期拒绝。确定性夹具与真实推理分别留证。

目前只有源码核查。旧版 DLL 的窗口观察能力保留；原版固定地址、日文窗口自动化和内嵌面板不属于这条原生路径的验收条件。

- [CPU 几何缓存与附件历史协调](../records/abi-geometry-history-2026-10-22.md)
- [MMS 相机修复同步到 ABI 分支](../records/abi-mms-camera-sync-2026-10-17.md)
- [Morph 文本共同历史验证](../records/abi-morph-text-input-2026-10-17.md)
- [五类全局轨道粘贴](../records/abi-global-paste-2026-10-17.md)
- [保留槽位按钮显隐验证](../records/abi-slot-visibility-2026-10-17.md)
- [全局关键帧粘贴的私有计划](../records/abi-global-paste-plan-2026-10-17.md)
- [全局删键与相机 HUD 共同历史](../records/abi-global-delete-hud-2026-10-19.md)
- [共享曲线写入与历史回调的会话保护](../records/abi-curve-write-lifetime-2026-10-21.md)
- [共享曲线来源回调的会话保护](../records/abi-curve-source-lifetime-2026-10-21.md)
- [共享曲线选择回调的会话保护](../records/abi-curve-selection-lifetime-2026-10-21.md)
- [共同历史的影响分类与延迟刷新](../records/abi-history-effects-2026-10-22.md)
- [冻结表情动作权重](../records/abi-frozen-morph-motion-2026-10-27.md)
- [冻结骨骼动作采样](../records/abi-frozen-bone-motion-2026-10-27.md)
- [历史记录模式与统一系统设置](../records/abi-history-settings-2026-10-17.md)
- [原生插帧的输入几何与写入边界](../records/abi-curve-insertion-geometry-2026-10-22.md)
- [原生相机曲线导入、导出与烘焙](../records/abi-native-camera-motion-2026-10-19.md)
- [原生菜单共同历史迁移](../records/abi-native-menu-history-2026-10-20.md)
- [口型、眨眼与模型关键帧乘加共同历史](../records/abi-morph-model-menu-history-2026-10-20.md)
- [同模型重名轨道粘贴同步](../records/abi-main-6facf91-2026-10-28.md)
- [同步模型模式导出的相机构图修复](../records/abi-camera-export-sync-2026-10-18.md)
- [同步配件列表拖拽排序](../records/abi-accessory-order-sync-2026-10-18.md)
- [坐标显示共同历史验证](../records/abi-grid-display-2026-10-17.md)
- [小窗口渲染边界与全部表情重置](../records/abi-small-viewport-morph-reset-2026-10-19.md)
- [小键盘视图预设与共同历史](../records/abi-numpad-views-2026-10-18.md)
- [工程地面开关共同历史验证](../records/abi-floor-history-2026-10-21.md)
- [布局撤销历史 C++17 封装](../records/abi-layout-history-sdk-2026-10-24.md)
- [布局撤销历史](../records/abi-layout-history-2026-10-24.md)
- [帧线插删与骨骼关键帧粘贴](../records/abi-frame-lines-paste-2026-10-17.md)
- [普通插件编辑服务开放前的原生写入核对](../records/abi-native-writer-audit-2026-10-20.md)
- [普通旋转重置与原撤销环](../records/abi-unbound-rotation-2026-10-23.md)
- [普通编辑历史与原生 MME 备份通知](../records/abi-ordinary-edit-history-2026-10-19.md)
- [曲线原始兼容配置的堆故障现场](../records/abi-curve-heap-observation-2026-10-21.md)
- [曲线原生控件回调寿命与捕获验收](../records/abi-curve-native-control-lifetime-2026-10-21.md)
- [曲线原生插帧回调寿命](../records/abi-curve-subdivision-lifetime-2026-10-21.md)
- [曲线对比与相邻控制柄回调的会话保护](../records/abi-curve-comparison-lifetime-2026-10-21.md)
- [曲线帧号与播放状态查询的会话保护](../records/abi-curve-transport-query-lifetime-2026-10-21.md)
- [曲线文件选择期间的会话保护](../records/abi-curve-modal-lifetime-2026-10-20.md)
- [曲线普通采样的会话保护](../records/abi-curve-sampler-lifetime-2026-10-21.md)
- [曲线测试按构建配置注册](../records/abi-profile-curve-tests-2026-10-21.md)
- [曲线编辑器导航、快捷键与速度图修复](../records/abi-curve-timeline-2026-10-18.md)
- [曲线编辑器接入宿主共同历史](../records/abi-curve-external-history-2026-10-18.md)
- [曲线编辑器的原生构造与插件回调边界](../records/abi-curve-native-construction-2026-10-21.md)
- [曲线跳帧与播放写回调的会话保护](../records/abi-curve-transport-write-lifetime-2026-10-21.md)
- [根骨骼在 IK 后继承阶段的越界修复](../records/abi-bone-root-inheritance-2026-10-27.md)
- [模型 Toon 文件表共同历史](../records/abi-model-toon-history-2026-10-21.md)
- [模型 VMD 缩放与滚轮共同历史迁移](../records/abi-scale-wheel-2026-10-17.md)
- [模型关键帧混合粘贴](../records/abi-mixed-model-paste-2026-10-17.md)
- [模型排序共同历史与容量边界](../records/abi-model-order-history-2026-10-20.md)
- [模型曲线共同历史独立审查](../records/abi-model-curve-history-review-2026-10-18.md)
- [模型英文名称与备注共同历史](../records/abi-model-names-history-2026-10-21.md)
- [模型视图拖拽与滚轮的共同历史](../records/abi-model-viewport-2026-10-18.md)
- [模型轮廓粗细共同历史](../records/abi-model-outline-history-2026-10-20.md)
- [灯光与 Morph 滑杆共同历史迁移](../records/abi-light-morph-sliders-2026-10-17.md)
- [独立骨架姿态准备](../records/abi-bone-pose-preparation-2026-10-27.md)
- [相机、灯光帧线与自由曲线](../records/abi-global-frame-retime-2026-10-18.md)
- [相机单字段归零共同历史验证](../records/abi-camera-axis-reset-2026-10-17.md)
- [相机插值共同历史验证](../records/abi-camera-interpolation-2026-10-17.md)
- [相机数值输入共同历史验证](../records/abi-camera-numeric-input-2026-10-17.md)
- [相机旋转归零共同历史验证](../records/abi-camera-rotation-reset-2026-10-17.md)
- [相机父模型共同历史实现与验收记录](../records/abi-camera-parent-model-history-2026-10-21.md)
- [相机父骨骼下拉框的共同历史迁移计划](../records/abi-camera-parent-bone-plan-2026-10-21.md)
- [相机父骨骼共同历史实现与验收记录](../records/abi-camera-parent-bone-history-2026-10-21.md)
- [相机跟随共同历史](../records/abi-camera-follow-2026-10-19.md)
- [相机跟随开关共同历史方案](../records/abi-follow-toggle-plan-2026-10-18.md)
- [相机透视切换共同历史验证](../records/abi-camera-projection-2026-10-17.md)
- [相机零帧合并负载](../records/abi-camera-root-payload-2026-10-19.md)
- [相机面板文本共同历史验证](../records/abi-camera-text-input-2026-10-17.md)
- [离线骨骼曲线复制修复](../records/abi-detached-curve-copy-2026-10-26.md)
- [稀疏通道显式登记与共同历史](../records/abi-sparse-registration-2026-10-18.md)
- [纹理重载的资源版本与共同历史](../records/abi-texture-reload-history-2026-10-22.md)
- [绑定旋转重置的草稿与派生角同步](../records/abi-rotation-drafts-2026-10-23.md)
- [绑定骨骼整体重置的局部零位与草稿](../records/abi-pose-reset-2026-10-23.md)
- [编辑回调内的序列化器保护](../records/abi-edit-provider-guard-2026-10-20.md)
- [表情与显示关键帧粘贴](../records/abi-model-paste-2026-10-17.md)
- [表情曲线测试的模型槽初始化](../records/abi-morph-fixture-slots-2026-10-21.md)
- [表情输入测试夹具修正](../records/abi-morph-input-fixture-2026-10-20.md)
- [负帧共同历史注册阶段](../records/abi-negative-registration-history-2026-10-30.md)
- [负帧共同历史：模型注册通道恢复](../records/abi-negative-model-pose-2026-10-30.md)
- [负帧删除、粘贴与选区共同历史](../records/abi-negative-bound-commands-2026-10-30.md)
- [负数关键帧核心与集成](../records/abi-negative-frames-2026-10-24.md)
- [轮廓颜色共同历史](../records/abi-outline-color-history-2026-10-20.md)
- [通道滚动条的取消与计时器身份](../records/abi-curve-repeat-timer-2026-10-21.md)
- [配件设置接入共同历史](../records/abi-accessory-settings-2026-10-18.md)
- [长变换数值的前缀显示](../records/abi-transform-prefix-2026-10-23.md)
- [附件开关共同历史验证](../records/abi-accessory-flags-2026-10-17.md)
- [附件文本共同历史验证](../records/abi-accessory-text-input-2026-10-17.md)
- [非骨骼保形插帧](../records/abi-camera-subdivision-2026-10-18.md)
- [骨骼与表情曲线共同历史](../records/abi-model-curve-history-2026-10-18.md)
- [骨骼数值输入共同历史验证](../records/abi-bone-numeric-input-2026-10-17.md)
- [骨骼旋转归零共同历史验证](../records/abi-bone-rotation-reset-2026-10-17.md)

## 实现与验收依据

以下保留原总览中的实现、失败和限制记录；其中日期、版本和“当前”均属于原记录上下文，不能替代上方的当前推进范围。旧执行方式与任务指令不适用于本轮工作。

[2026-10-27 独立骨架姿态准备](../records/abi-bone-pose-preparation-2026-10-27.md)：内部 `BonePreparePosePrivate` 与原 `SetPhysicsMode` 共用来源选择和骨骼表情合成循环，私有入口隔离全局数学表，保留原入口的 fallback 与层级调度。双配置各 385 项准备检查、2,777 项上下文回归通过；原生表情、场景烘焙及两产品构建通过。完整工作区和公开帧仍待装配；main 阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 根骨骼 IK 后继承修复](../records/abi-bone-root-inheritance-2026-10-27.md)：修复合法根骨骼在继承阶段读取 `boneTable[-1]` 的问题，复用原世界变换规则，保留非根和物理复制语义。旧实现专项 123 项失败，修复后双配置各 374 项通过；拖拽回归、完整配置场景烘焙及两产品构建通过。原始配置烘焙测试目标的既有链接问题已单独记录，未计通过。尚未开放完整独立姿态，main 阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 冻结表情权重](../records/abi-frozen-morph-motion-2026-10-27.md)：内部 `morph_curve::SamplingState` 持有原生键和私有协调的自由曲线，源删除后可并发、无分配采样。专项 922 项、现有曲线 211 项、历史写入 3,572 项、注册 2,178 项、VMD 曲线 13,970 项及真实 PMX/PMM 生命周期验证通过，两产品构建通过。尚未开放表情 C ABI，也不含组/翻转展开、显示、层级或最终形变；main 无新提交，阴影继续暂缓，正式 release 未覆盖。

[2026-10-27 冻结骨骼动作](../records/abi-frozen-bone-motion-2026-10-27.md)：内部快照拥有原生关键帧、尺度和自由曲线，源删除后仍可并发采样模拟前通道。动作专项 973 项、自由曲线 632 项、原历史求值 194 项、复制 5,941 项及现有曲线回归通过；两产品构建通过。场景租约、完整求值和公开 C ABI 尚待接入，main 新阴影继续暂缓。

[2026-10-26 离线曲线复制修复](../records/abi-detached-curve-copy-2026-10-26.md)：为独立求值修复源作者被同步和分配失败丢失目标的问题，补两侧范围编辑拒绝及私有副本构造失败清理。复制专项 5,941 项、现有曲线与实际烘焙回归通过，两产品配置构建通过。未新增公开求值接口；main 新阴影继续暂缓，正式 release 未覆盖。

[长变换数值前缀](../records/abi-transform-prefix-2026-10-23.md)已同步 main 的目标框左对齐与非聚焦回到开头行为，保留输入提交及原始兼容配置。两套专项各 12,153／11,843 项、相邻数值与旋转回归及两个旧行为反例通过。三档字体和宽度只模拟布局，真实多屏 DPI 未验收；其余 ABI 缺项继续待实现。

[普通未绑定旋转重置](../records/abi-unbound-rotation-2026-10-23.md)已接入单骨原撤销环的准备后发布，修复未选显示骨漏快照与草稿重新提交；成功后局部更新派生角和目标文本。新专项及恢复运行各 6,597 项通过，三个旧入口反例成立，两产品和相邻回归通过。该批不代表全部本体共同历史迁移完成，公共 `mms.edit` 仍关闭。

同批附件协调使用私有 `HistoryChange::PrepareInitial(SharedHistory&, bool record, ...)` 传递真实记录意图；默认委托原 `Prepare`，组合及 VMD 包装继续转发。它不改变公共 C ABI。父绑定、HUD、登记和粘贴区分逻辑历史值与可信求值后的实际值。模型及附件删除的方向快照与原保活、身份及资源证明共同准备，最终发布期间不分配。父绑定及删除 6,266 项、HUD 核心 5,711 项、相邻历史十组共 95,042 项通过；精确范围及保留的失败证据见[本轮记录](../records/abi-geometry-history-2026-10-22.md)。

[共同历史影响分类](../records/abi-history-effects-2026-10-22.md)已接入原历史及延迟 UI 队列，纯视口和 None 的本地、原 DLL 撤销重做不再建立场景 mutation、启停顶层窗或无条件通知曲线变化。随后[补齐排序夹具的有效父骨骼](../records/abi-model-order-fixture-2026-10-22.md)，原排序断言及 5,573 项通过，生产代码不变。[实际纹理重载 278](../records/abi-texture-reload-history-2026-10-22.md)现已接入前后资源保活、一次发布及同一轻量刷新；专项 16,276 项、相邻资源与历史检查及既有 21 组 ABI 回归通过。第三方 hook 私有登记、其他原生写入和完整 ABI 仍未完成，普通 `mms.edit` 保持关闭。

[模型轮廓粗细](../records/abi-model-outline-history-2026-10-20.md)已接入共同历史绑定宿主，覆盖实际 646 文本、647 滑杆、精确原位还原、窗口关闭、删除恢复及混合插件事务。最终底层和真实控件专项共 6,493 项通过；随后接入[轮廓颜色](../records/abi-outline-color-history-2026-10-20.md)的 PMD 资源恢复及实际菜单 286。普通公共 `mms.edit` 仍关闭，其他模型资源编辑继续列为未完成。

[模型英文名称及备注](../records/abi-model-names-history-2026-10-21.md)随后接入菜单 259 的私有草稿和一次显式事务，名称专项 9,514 项、保存专项 11,634 项及真实 PMX／PMD 工程保存重开通过。标准短字段保持，完整备注使用独立 MMS 记录；损坏原始备注和非零 Unicode 尾数据按语言保留源模型内容。已保留物理对象的[混合成员交换](../records/abi-physics-exchange-2026-10-21.md)及私有 ImportScene 的[首次替换候选](../records/abi-physics-first-replacement-2026-10-21.md)均已实现，随后整合到下述模型物理资源版本。准备阶段的实时数组扩容已按[存储修复记录](../records/abi-physics-transition-storage-2026-10-21.md)改为私有存储。

[模型 Toon 文件表](../records/abi-model-toon-history-2026-10-21.md)已接入菜单 261 的 Unicode 私有草稿、真实纹理保活与缓存原子发布。11,062 项、真实 D3DX 两种模式重放和满 100 字节 CP932 导入通过。DX9 纹理与效果颜色使用同一解析规则；可选 DX12 源文件启用编译通过，正式产物仍为 Win7 CPU 基线。全局 278 的宿主资源版本已按上述记录实现；261 缺失旧缓存及外部渲染器的资源版本仍有限制，普通 `mms.edit` 继续关闭。

[口型、眨眼和模型乘加](../records/abi-morph-model-menu-history-2026-10-20.md)补齐 225、227、251、252 的受控共同历史。新增两组专项共 3,567 项、相关既有历史六组 18,457 项通过。随机结果只生成一次，键、作者层、缩放和未登记姿态在同一事务中处理。普通编辑服务继续关闭。

[曲线模态生命周期](../records/abi-curve-modal-lifetime-2026-10-20.md)修复文件选择期间销毁或替换编辑器后的悬空访问。原批次完整编辑器 2,538 项、真实 PMX／PMD 原名／改名宿主及最终保存 419 项通过。随后[统一来源读取、历史查询及标签回调的会话保护](../records/abi-curve-source-lifetime-2026-10-21.md)：共享会话版本覆盖闭包复制、调用和释放中的重入，旧调用不写入新会话。完整配置来源专项 2,463 项、motion 专项 278 项和编辑器全集 5,001 项通过，两套产品构建通过。运行继承 `Default` 桌面，显示由夹具控制，不是隔离、IME 或多 DPI 验收。比较模式及相邻控制开关由[后续批次](../records/abi-curve-comparison-lifetime-2026-10-21.md)接入；帧与播放控制、`subdivide`、`Channel.sample` 仍待统一，正式 release 未更新。

普通 `mms.edit` 仍未开放。[原生写入审计](../records/abi-native-writer-audit-2026-10-20.md)确认部分旧命令既未迁移也未被统一拦截，早期“所有其余入口关闭”的描述不能作为保护保证。下一批继续迁移这些实际写入入口。

[小窗口边界与全部表情重置](../records/abi-small-viewport-morph-reset-2026-10-19.md)补齐一次表情重置调整、采样退出的真实布局恢复、设备恢复尾部再次失效及浮动窗口烘焙输入保护，视口缩放／捕获 411 项通过。该页和下列旧批摘要保留当时结果；滑杆的最新实现、人工复验范围及早期严格失败以本轮记录为准。

[普通编辑历史与 MME 备份通知](../records/abi-ordinary-edit-history-2026-10-19.md)补齐审查第 11、13、14、15 项；完整配置 15 组不同回归、原版兼容 11 组通过。MME 的通知与已有 EMM 清理已实现，真实保存消费和立即退出仍未完成。未扩大普通公共编辑 ABI。

[完整跟随开关](../records/abi-camera-follow-2026-10-19.md)现已接通主窗口、浮动窗口及菜单，保留复合采样、两种历史模式和实际对象保活计费。专项 10,918 项、完整原生历史 274,832 项、真实 PMX/PMD 宿主及两种配置最终整合通过；普通 `mms.edit` 继续关闭。

[原生相机导入、导出和烘焙](../records/abi-native-camera-motion-2026-10-19.md)修复审查第 9、10 项，保留合法交叉／端点 X、稀疏标准记录及原始控制字节，局部烘焙保持范围外和未选通道。专项 63,112 项、原生历史 274,832 项、完整配置 11 组及原始配置 7 组整合通过；真实 PMX／PMD 窗口入口和正式程序改名检查通过。公共动画 ABI 尚未开放。

[相机零帧合并](../records/abi-camera-root-payload-2026-10-19.md)补齐父模型、父骨骼及距离／FOV 的控制柄，普通和共同历史入口一致。完整帧线整合 37,820 项、两种配置的普通负载专项及原始配置读写通过；文件格式与原生采样保持。

随后同步[配件列表拖拽排序](../records/abi-accessory-order-sync-2026-10-18.md)。三语言拖拽、取消、失败回滚及 PMM 往返通过。该批暂时禁用共同历史下的排序；现已按下述配件设置迁移开放。

[配件设置](../records/abi-accessory-settings-2026-10-18.md)已将顺序、名称和绘制分界合为一次共同历史显式提交。专项 8,819 项及三语言真实宿主通过，补齐时间轴缓存、场景名称快照及元数据不重置物理。共同历史累计 208,941 项；普通公共 `mms.edit` 仍关闭。

[曲线编辑器](../records/abi-curve-timeline-2026-10-18.md)补齐顶部刻度同步主时间轴和视图、激活窗口快捷键及长区间速度图。[非骨骼保形插帧](../records/abi-camera-subdivision-2026-10-18.md)保留相机原生求值，独立 PMM 片段支持重载和时间域映射。[VMD 兼容检查](../records/abi-curve-vmd-compat-2026-10-18.md)补上旧 VMD1 相机记录分支，固定原生六通道读写结果。

随后将[五类全局曲线及窗口撤销](../records/abi-curve-external-history-2026-10-18.md)接入共同历史。曲线写入和窗口读取 VMD 都是一次显式提交，窗口撤销直接遍历宿主队列。原始作者数据、当前帧局部求值、后续选区和提交焦点均已验证；十一组整合通过，共同历史 212,918 项，原版兼容四组仍通过。[模型骨骼、表情及显示／IK 窗口](../records/abi-model-curve-history-2026-10-18.md)随后完成受控迁移。公共 `mms.edit` 继续关闭。

[工程地面开关 285](../records/abi-floor-history-2026-10-21.md)已把原始工程字节与实际 Bullet 碰撞标志作为同一 Adjustment 发布和恢复。来源先验证当前世界成员及独立 ray UID，短候选另冻结步进和成员顺序；发布不步进或重置物理。地面专项 1,370 项、物理模式 1,901 项和背景 1,463 项通过，含下一次实际 CPU 步与原生路径对照。原始兼容仍保留旧入口，公共编辑服务和正式 release 未更新。

[曲线写入及历史遍历](../records/abi-curve-write-lifetime-2026-10-21.md)随后统一到同一会话保护。稳定指针节点使本地撤销在回调拒绝、异常及分配失败时保留原栈；成功后先转交已准备节点，再读取规范数据，读取失败不重复提交。两套配置写入专项各 3,259 项、来源专项各 2,463 项、motion 各 278 项和编辑器全集各 8,222 项通过。其余帧与播放控制等回调仍需迁移，未开放普通公共编辑服务。

[选择读写回调](../records/abi-curve-selection-lifetime-2026-10-21.md)也已统一保护复制、调用和释放中的会话失效，参数由调用私有保留，返回后重新取得当前通道。local Undo/Redo 各发布一次选择。两套配置选择专项各 5,158 项、编辑器全集各 13,380 项通过；2 与 100,000 键各 8 次稳定空选择时钟的夹具分配均为 0，不能扩展为全部回调无分配或速度提升。帧与播放控制、subdivide 和采样仍待统一。

[比较模式与相邻控制开关](../records/abi-curve-comparison-lifetime-2026-10-21.md)已接入同一会话保护。区间筛选先准备私有显示集合，宿主接受后才一起发布模式与显示标记；拒绝或异常保持原状态及历史，成功后的界面故障不重复提交。相邻控制开关正常返回 false 仍提交一次不联动的编辑，异常则拒绝编辑。两套配置专项各 2,674 项、编辑器全集各 16,054 项通过。本批不改变 VMD 格式、原生控制点规则或普通编辑服务的开放范围。

[相机父骨骼下拉框 450](../records/abi-camera-parent-bone-history-2026-10-21.md)已按 Adjustment 接入共同历史，保留独立相机参数、模型姿态和原始字段。来源在草稿读取前固定，捕获只授权实际控件；列表按 item data 同步，稳定 4000 骨读回不分配、不写列表。父骨骼专项 2,367 项及相邻八组共 90,849 项通过，两套产品构建通过；标准 PMM prefix 只改变既有四字节字段。未新增完整工程重开或 GPU 验收，普通编辑服务及正式 release 未更新。

[相机父模型下拉框 449](../records/abi-camera-parent-model-history-2026-10-21.md)随后接入 Adjustment。父模型改变时，骨索引作为同一绑定对重放；解除附着结果只采样一次，其他未变姿态保留。最终专项 6,332 项、450 及相邻八组共 97,214 项通过，包含真实删除、登记交错及复审发现的两种列表恢复。普通读回与严格编辑授权分开，末次读取后重排也会重验实际选中身份。两套产品构建通过，普通编辑服务及正式 release 未更新。

[帧号和播放状态查询](../records/abi-curve-transport-query-lifetime-2026-10-21.md)已接入同一会话保护，缓存仅用于显示，失败查询不授权导航；先查询再借用轨道或绘制资源，非法帧号不进入整数转换。两套配置查询专项各 8,566 项、完整配置全集 24,620 项通过。原始兼容全集出现 `0xC0000374` 堆损坏，虽比较专项单独通过，根因及全集验收仍未完成。`seek`、`setPlaying` 写回调、`subdivide`、一般 `Channel.sample` 及真实播放停止的重复求值仍需处理，普通公共编辑服务与正式 release 未更新。

[原生构造边界](../records/abi-curve-native-construction-2026-10-21.md)现将全部初始 provider 推迟到外层 `CreateWindowExW` 返回后，构造期消息不再进入 provider，原生控件维护仍进入原窗口过程，来源通知保留并合并唤醒。真实创建返回钩子、126 组初始退役及必要子控件创建失败均通过；两套配置最终构造专项各 1,627 项、编辑器全集各 26,667 项通过。冻结产物的独立调试仍重现堆损坏；外部扩展限制的反向对照也失败，故障尚未归因，不能关闭。

[分段控件和通道滚动条](../records/abi-curve-native-control-lifetime-2026-10-21.md)进一步保护原生消息中同步销毁及同 HWND 更换 State，捕获转移后不再启动旧拖动。两套配置专项各 756 项、完整配置曲线全集 26,667 项通过；原始兼容全集仍退出 `0xC0000374`，整体堆故障继续诊断。数字拖动、主题辅助和同 State 跨 Session 的输入连续性不属于本批完成范围。

随后[跳帧与播放写回调](../records/abi-curve-transport-write-lifetime-2026-10-21.md)接入闭包复制、执行及最终释放的会话保护。关键帧点击重新取得轨道，同窗口重开先取消旧手势；异常及成功后显示失败均不重试可能已生效的写回调。两套配置专项各 2,628 项、最终曲线全集各 29,295 项通过，两套产品构建及 PE 基线核对通过。相邻查询曾有一次替换会话编辑断言失败，追加诊断后未重现，仍未归因；不能用后续通过关闭它或既有堆损坏。

[滚动条计时器身份](../records/abi-curve-repeat-timer-2026-10-21.md)再修正取消后重启计时、同 State 新手势被旧回调覆盖及旧消息跨手势执行。每手势独立 ID、精确取消和重复捕获保护保持 400/60 ms，最终两套配置专项各 1,705 项、控件全集各 2,459 项通过。`subdivide`、一般 `Channel.sample`、数字拖动、主题辅助及真实停止求值仍待迁移，普通公共编辑服务和正式 release 未更新。

[全局删键与相机 HUD](../records/abi-global-delete-hud-2026-10-19.md)补齐五族及多附件原子删除、六轴完整手势和取消所有权。完整配置 18 组、原始兼容配置 16 组通过，共同历史累计 283,180 项。原有 VMD/PMM 格式与普通重定时保持，公共编辑服务仍关闭。
