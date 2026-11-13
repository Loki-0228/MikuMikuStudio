# 宿主、SDK、生命周期与验收

[返回功能总览](README.md)

## 当前推进范围

### 已改动代码验证闭环

已确认的局部结果：布局存储回归通过 25,530 项，宿主事件验证覆盖 UTF-8 关闭路径及关闭先于重置，工程打开身份回归在独立临时目录通过 430 项。PMM 失败恢复的最新结果统一见[工程加载与失败恢复](project.md)。

效果路径兼容回归在完整与原始兼容配置分别构建并通过 236 项检查，进程退出码均为 0。完整配置证据为工作区 `本地验收记录（未随本文发布）`、`effect-path-test.log` 和 `effect-path-detail.log`；原始配置证据为 `original-recovery-build.log`、`original-recovery-test.log` 和 `original-recovery-detail.log`。原始配置的工程身份回归另通过 241 项。路径兼容专项不替代真实 MME 消费或完整平台验收。

仍需完成其余已改动代码的验证，以及原始兼容配置的对应覆盖。默认临时目录下曾出现的进程初始化或早期退出问题尚未定位；独立目录下通过不能替代原环境验收。完整配置与原始兼容配置分别记录。

已有证据位于工作区 `本地验收记录（未随本文发布）`、`project-identity-host-events-20261101-1130.log`，以及 `pmm-probe-5d8e41c7689c47ef9694b002e1014aa6/1793504394893089400-26300/report.json`。

验收要求：诊断进程初始化失败并重跑针对性测试，逐项记录真实退出码；原配置/完整配置差异不能混淆。

依赖：ABI-00。任务标识：ABI-01。

### 当前公共服务与契约核对

完整配置 Release 的 `plugin_abi_tests` 重新构建（包含 C11 声明编译和 C++ 布局检查），`plugin_abi_contract` **45,142项通过**，运行35.17秒。报告在工作区 `本地验收记录（未随本文发布）`，详细日志同目录 `detail.log`，构建日志 `abi-contract-revalidation-build.log`。覆盖目录发现、加载失败回收、stop BUSY、UI队列、快照、保存恢复、事务桥及INPUT状态/草稿/取消/生命周期。

该测试的编辑宿主是显式受控绑定；普通生产 `AttachEditHost` 仍受 `migrationTest` 限制，原始兼容配置不启动生产ABI。此次通过不覆盖所有原生命令、未来计算/渲染提供者或目标真机。

当前 `manager.cpp` 的查询分支已记录在工作区 `本地验收记录（未随本文发布）`，附源码摘要。`mms.render.image` 已提供不可变CPU像素与租约，SDK总览已补列；它不提供主视口输出、完整渲染器或GPU互操作。提供者注册仍只接受 `mms.serializer`，其余领域服务不能依据旧提案认为已开放。根草案中2026-10-20“声明式INPUT和其他挂载点未实现”是历史状态，当前INPUT/侧栏/属性扩展行已有，DOCK与时间线等缺口仍保留。

构建保留共享中间目录MSB8028、测试取消NDEBUG及测试析构内CHECK可能抛异常C4297警告；不能将构建成功描述为零警告。生产代码和测试源码本轮未修改。

### 兼容平台、视觉交互与发布验收

Win7/AMD、真实桌面DPI/遮挡/鼠标定位、设备掉线、长时及发布物证据仍单独欠缺；本机编译不替代。

验收要求：工具链/导入/指令集清单；完整与原始配置；真实所需设备和视觉由Codex/用户验收；缺设备标阻塞不伪造通过。

依赖：ABI-03、ABI-04、ABI-05、ABI-08、ABI-10。任务标识：ABI-12。

## 相关专题

- [ABI 分支合入 main 功能记录](../records/abi-main-merge-2026-10-17.md)
- [ABI 剩余工作核对](../records/abi-remaining-work-2026-10-17.md)
- [ABI 对照 MMS main 全仓审查](../records/abi-main-review-comparison-2026-10-18.md)
- [ABI 需求最终审计](../records/abi-final-audit-2026-10-24.md)
- [C++ SDK 租约安全](../records/abi-sdk-lease-safety-2026-10-27.md)
- [main 5af464a 同步与本地提交](../records/abi-main-5af464a-sync-2026-10-30.md)
- [main 9879707 同步状态](../records/abi-main-9879707-sync-2026-10-30.md)
- [main d8af662 行为同步](../records/abi-main-d8af662-sync-2026-10-30.md)
- [main 名称与格式标记同步](../records/abi-main-marker-sync-2026-10-20.md)
- [main 审查问题的 ABI 修复](../records/abi-review-fixes-2026-10-19.md)
- [main 小项同步（alpha 分类、附件菜单、滚动条、栈缓冲）](../records/abi-main-small-items-2026-10-24.md)
- [main 已提交功能同步](../records/abi-main-feature-sync-2026-10-20.md)
- [main 新增功能的 ABI 迁移核对](../records/abi-main-sync-plan-2026-10-22.md)
- [main 新提交核对与插件目录状态](../records/abi-main-af7ab08-sync-2026-10-29.md)
- [main 未提交功能向 ABI 的行为迁移](../records/abi-main-working-sync-2026-10-29.md)
- [main 行为同步矩阵](../records/abi-main-sync-matrix-2026-10-23.md)
- [main 非阴影功能核对与排序补验](../records/abi-main-sync-audit-2026-10-29.md)
- [分支交接状态](../records/abi-handoff-2026-10-24.md)
- [合并 MMS 已提交修复](../records/abi-main-sync-2026-10-18.md)
- [固定 Win7 部署闭包](../records/abi-deployment-closure-2026-10-24.md)
- [宿主事件探针改用生产窗口启动](../records/abi-host-probe-production-bootstrap-2026-10-24.md)
- [宿主侧业务事件：对象删除与 DPI 变化](../records/abi-host-events-object-dpi-2026-10-24.md)
- [宿主侧业务事件：工程关闭、语言切换、设备丢失/恢复](../records/abi-host-events-2026-10-24.md)
- [宿主探针日志隔离与 DPI 消息路径诊断](../records/abi-host-probe-log-isolation-2026-10-24.md)
- [恢复 ABI 契约套件构建](../records/abi-plugin-abi-suite-restore-2026-10-24.md)
- [最终验证矩阵](../records/abi-final-verification-2026-10-24.md)
- [本机窗口回归的桌面环境更正](../records/abi-verification-desktops-2026-10-21.md)
- [补齐 main 的启动器修复与保存验收](../records/abi-main-bugfix-completion-2026-10-18.md)

## 实现与验收依据

以下保留原总览中的实现、失败和限制记录；其中日期、版本和“当前”均属于原记录上下文，不能替代上方的当前推进范围。旧执行方式与任务指令不适用于本轮工作。

更新：2026-10-24。分支从 42d4a71 建立，按插件 ABI 与界面布局设计实施；完整 P0–P6 尚未完成。

当前状态见[需求最终审计](../records/abi-final-audit-2026-10-24.md)：142 行需求中本机已验证类 120 行、已实现未验证 2 行、部分实现 6 行、未实现 11 行、未开放 1 行、缺环境 1 行；已知失败仅 `model_alpha_sort_off_mme_presence`。main 核对至 `d551f1f`。

持续目标同时包含[main 更新的行为级同步](../records/abi-remaining-work-2026-10-17.md#持续目标中的-main-行为同步)。每轮检查已提交更新，按 ABI 架构实现并独立验收对应功能，不用代码合并代替行为核对，也不覆盖用户明确指定的兼容边界。

状态分为 **未实现**、**已实现未验证**、**本机已验证**、**缺其他系统/硬件环境**。代码、本机结果和平台结果分别列出。

main 截至 `fed9768` 的逐提交行为同步核对整理为 [main 行为同步矩阵](../records/abi-main-sync-matrix-2026-10-23.md)：最近 25 个提交中 18 个没有待办行为差异，`3782e0c` 部分同步，`c147454` 按数据集中约定有意偏离。待处理项集中在负数关键帧、效果编译缓存、纹理准备缓存、缓存管理入口与不透明纹理 alpha 分类。

[main 小项同步](../records/abi-main-small-items-2026-10-24.md)补齐四项：`local_data_path.hpp` 的 32 KiB 栈缓冲改为堆分配（`effect_paths.cpp` 已与 main 对齐）；采用 main 的不透明纹理 alpha 分类（未知格式不默认透明，X8R8G8B8／R8G8B8 等直接不透明，A8R8G8B8／A8B8G8R8 与 DXT1／3／5 按像素采样），CPU 回归通过，GPU 排序回归待未同步的 `transparent_sort_cache`；附件 VMD 菜单在本地化、打开与命令入口都要求相机模式，曲线宿主回归通过；时间轴滚动条 case 4/5 改为 `SIF_TRACKPOS` 绝对位置并把下限改为 `frame_limits::minimum`。两产品构建通过，正式 release 未覆盖。GPU 透明排序缓存与夹具已由[透明排序缓存批次](../records/abi-transparent-sort-cache-2026-10-24.md)同步，14/15 个 `model_alpha_*` 模式通过；`model_alpha_sort_off_mme_presence` 仍失败。真实 MME 深回调栈、真实鼠标拖动时间轴、原始回退脏标记、阴影包围盒缓存仍未完成或未验收。

至此 main 已提交更新全部同步。

main 新增 `0fe9fe0`、`3782e0c` 的差异已完成[迁移前核对](../records/abi-main-sync-plan-2026-10-22.md)。随后接入[PMM 预读、聚焦阴影及附件父绑定历史](../records/abi-preread-shadow-parent-2026-10-22.md)：两种配置的真实加载、附件 4,303 项及实际完整产品的 SM3／SM2 绘制通过。旧源码即时编译超时和系统鼠标用例未计入通过；普通 `mms.edit` 继续关闭。[有界 CPU 几何缓存](../records/abi-geometry-history-2026-10-22.md)现已接入实际 PMD／PMX 加载器，该批两配置后端各 10,215 项及实际加载器 545／540 项通过；磁盘缓存管理和负帧仍待迁移。

宿主的私有 `native_menu_snapshot::Capture` 已通过两配置各 66 项真实菜单检查，现已接入目录的宿主采集器。它只观察现有菜单，结果中的 HMENU 数值仍是私有临时来源；公共目录将其映射到原元素句柄或可退役的观察身份。未物化内容和其他未登记窗口不计入已完成范围。

最新编码窗口复核在完整配置通过 1,725 项、42 个对话框、6 条消息，原始兼容配置通过 1,047 项、20 个对话框、25 条消息，两者都是实际 ACP 936、`gpu=0`。两套 reviewed 构建通过，PE 均为 x64、子系统 6.1，无新图形后端或显卡厂商 DLL 直接导入。当前构建原名及中文改名的既有面板启动、侧栏和退出通过，不含新属性行或物理编辑 GPU 呈现。以上是同桌面夹具窗口结果，显示限制见[桌面环境更正](../records/abi-verification-desktops-2026-10-21.md)，真实命名桌面仍未成功。正式 release 仍为 `9d879c8`，公共 `mms.edit` 仍关闭，证据与剩余范围见上述物理记录。

左右侧栏本批两套 NativeUi 专项各通过 3,159 项，刷新恢复专项各 390 项，完整 ABI 契约各 34,062 项；实际 MMS 普通和共同历史宿主通过视口避让、分隔线、模式切换及移除检查。纯视口边界各 1,086,537 项通过。最终 PE 均为 x64、子系统 6.1，导入相对上一批未变，稳定 release 未覆盖。真实 IME、连续桌面无闪烁及 Win7／AMD 真机不在上述通过范围。

2026-10-20 完成 [main 名称与格式标记同步](../records/abi-main-marker-sync-2026-10-20.md)：同步标准 19 字节 PMM 名称判断及所选命名修复；按用户要求仅识别 MMS 扩展标记，不保留旧 MDS 读取别名。独立镜头数值修复工具及启动器已从 ABI 分支移除。完整和基础配置构建、格式专项及六组相关共同历史专项通过；完整历史套件在真实光标定位处受本机环境限制，未声明整套通过。UI 框架已由用户确认修复完成，下列早期闪动描述保留为历史排查记录。

用户最终明确触发条件为 175% 缩放下拖动侧栏或底部分隔线，改变内部布局。此前按直接拖动滑杆排查是 agent 的误解；所有滑条仍会概率出现整条白框或白底，先前相机、阴影人工通过结论撤回。正确触发条件的首次实际诊断有 20,184 条元数据，环缓冲无覆盖；105 个合格布局帧全为 FOV，只有底部拖动标志，无侧栏拖动标志。帧 48、49 有邻区残留，未捕获整条白闪。上述自动回归、最终像素与故障专项仍有效，不能证明连续桌面无闪；早期严格失败和 DPI 不可判定证据继续保留。

R3 拖动与插件区域问题已由用户确认恢复。按需求归类的当前缺项、部分实现及环境验收见[剩余工作核对](../records/abi-remaining-work-2026-10-17.md)。

[main 全仓审查的 ABI 对照](../records/abi-main-review-comparison-2026-10-18.md)已逐项核对 22 项：普通完整配置均有残留，受控路径须分别判断。短路径解析及 PMX toon 索引已实际复现访问冲突；其他条目的本轮证据为源码与覆盖检查。该核对不表示缺陷已修复，正式程序未重建。

随后完成 [13 项审查修复](../records/abi-review-fixes-2026-10-19.md)，覆盖普通零帧链、物理编辑、事务 PMD 保存、录制失败与重入、Kinect 数学及两处越界。完整配置和原版构建及定点回归通过；共同历史 263,498 项、ABI 1,804 项、真实物理窗口 1,277 项通过。普通视频重排布局仍有两个像素差，已保存旧副本对照并保留失败断言；不能据此批结果宣称整个 ABI 需求完成。

随后补齐[曲线修复启动器和正常保存验收](../records/abi-main-bugfix-completion-2026-10-18.md)，并适配修复副本的插件侧车哈希及保存身份。五组实际加载与重开、侧车数据保留、三语言保存错误提示及序列化边界通过。
