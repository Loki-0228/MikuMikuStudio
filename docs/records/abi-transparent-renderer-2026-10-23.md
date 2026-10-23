# 透明排序渲染与工程设置接入

本批完成下述 D3D9 策略和工程设置接入的本机验证，不能计为透明排序全部需求完成。基线为 `00ad606`，本轮开始及最终产物检查读取的已提交 main 均为 `fed9768f9fe726c4b5e068849808803f1713ad42`，没有新增待同步提交。证据目录为 `build/verification/abi-transparent-renderer-20261023`。

## 实现范围

`model_renderers.cpp` 将 CPU 策略接入实际 D3D9 绘制。Low、Standard、High 分别使用 16、64、256 个深度桶，保留模型原 VB、步长和源索引，仅上传排序后的私有索引。跨对象排序是独立选项，关闭时按原模型顺序分别排序。原生 Maximum 保留切分路径，外部效果的源面路径不切分顶点。

菜单命令 309 控制跨对象选项。精度单选与范围复选分离，菜单刷新保留既有弹出菜单身份。PMM 使用 `org.mikumikustudio.view.transparent_sort_cross_objects` 保存范围，默认关闭；未知字段值保持到用户明确修改。异步保存根据场景修订处理保存期间的新编辑。原始兼容配置不开放该增强行为。

准备阶段构造完整候选排序计划，成功后交换缓存。纹理查找预热在主渲染目标提交前执行。提交后的 `bad_alloc` 不再重放整组透明面，但可能留下未完成的当前帧；这不是帧回滚或全部绘制失败的恢复保证。

## 当前证据

| 项目 | 当前结果 | 证据 |
| --- | --- | --- |
| 增强与原始兼容产品构建 | 首轮与反例恢复后均成功 | `build-final-full.json`、`build-final-original.json` |
| 实际菜单 | 增强 425 项、原始兼容 191 项通过 | `menu-full-1/report.json`、`menu-original-1/report.json` |
| 原始兼容工程保存重开 | 1,106 项通过；增强取消用例在该配置不执行 | `project-original-final/report.json` |
| 增强工程保存重开、异步保存及取消 | 4,552 项通过 | `project-full-final/report.json` |
| 实际 D3D9 渲染 | Fixed 9,349 项，Fixed、SM2、SM3 全集 27,654 项通过 | `renderer-fixed-4/report.json`、`renderer-all-1/report.json` |
| 范围转发反例及恢复 | 强制跨对象时第 155 项检出真实顺序错误；恢复后全集 27,654 项通过 | `scope-counterexample.json`、`renderer-restored-scope/report.json` |
| 原生 Maximum BSP | 6,420 项通过，含片段面积、覆盖及红绿像素 | `renderer-maximum-1/report.json` |
| 离屏源面与六平面判定 | 19,791 项通过 | `renderer-visibility-1/report.json` |
| 几何反例及恢复 | 禁止切分与过早剔除离屏面分别在第 144、147 项失败；恢复后扩展全集 53,651 项通过 | `geometry-counterexamples.json`、`renderer-restored-geometry/report.json` |
| PMX 附加 UV | 1 至 4 组附加 UV，13,601 项通过 | `renderer-uv-1/report.json` |
| 实际 FVF 反例及最终全集 | 保持正确步长但绑定错误 FVF，在第 212 项检出；恢复后全部渲染用例 67,145 项通过 | `uv-counterexample.json`、`renderer-final/report.json` |
| 产品兼容性检查 | 两产品 x64、子系统 6.1，导入和 117、118 效果资源相对基线未变 | `pe-final.json` |

工程测试首轮因缺少内嵌 `PNG/102` 失败。宿主通过 PE 资源读取 HUD 图集，没有磁盘 PNG 后备路径。测试目标定义早于 `MIKUMIKUSTUDIO_GEN_RES` 赋值，资源参数为空。CMake 现将两个测试的资源链接移到变量赋值之后；运行器先只读核对内嵌资源及来源，再启动工程测试，不复制用户 data 或插件。

渲染测试首轮没有实际绘制回调。测试给索引缓冲分配的虚表副本位于栈上，生产 `PlausibleD3dObject` 要求虚表属于 `MEM_IMAGE`，因此拒绝该对象。夹具现改为测试 EXE 内的静态表，每个存活缓冲独占一份，并调用原生产检查验证。生产检查没有修改。补齐 `SceneLight.Direction` 并将像素和回调诊断移到断言之前。

`renderer-fixed-3` 在构建尚未结束时启动，复制的仍是旧产物 `b996d262…`，不是静态虚表修复后的验证。保留其失败记录，但不将其用于判断新夹具或新渲染行为。

Maximum 专项从实际私有 VB 写入捕获 96 字节顶点，检查生成索引、对象和材质身份。片段面积、重心坐标和内部覆盖点由源三角形独立计算；左右采样点分别验证红绿两面的遮挡顺序。Effect 场景使用两个模型且关闭跨对象排序，通过生产门禁走 BSP，不将其结果计为 peeling 验收。源 VB、GPU IB、CPU 索引及材质记录保持不变。

离屏专项检查深度锚点从屏幕外移入、再移回时的桶边界，并覆盖六个共同外侧平面及顶点各自屏外但内部覆盖画面的三角形。该专项的范围开关用例只有单模型；跨模型顺序的证据来自基础 `ScopeCases`，不能混称已验证离屏面跨模型桶边界。

附加 UV 专项在真实 PMX 源 VB 中写入独立值，覆盖 48、64、80、96 字节步长及大于 65,535 的源索引。第二观察器读取实际 `GetFVF`，避免仅凭测试自己的格式常量判断成功；完整 VB 和 GPU IB 的前后字节也参与比较。此结果证明布局与数据传递，不证明内置 shader 采样每个 UV 通道的视觉语义，亦不覆盖 Maximum 插值。

取消专项调用真实 `SaveSceneAs`，仅对当前线程、准确 owner 的应用选择器返回取消。它检查两个设置及 edited 标记、dirty、修订、播放头、项目路径、旧 PMM 和插件旁车全部字节、目录及保存状态。没有自动接收错误对话框，也没有用 `beforeWrite=false` 冒充用户取消。原始兼容配置不运行该增强专项。

## 审查与产物

生产接入的独立审查在 `review-renderer.txt`，设置、Maximum、离屏及附加 UV 夹具分别完成独立静态检查。DSH 仅修改原子代码任务，生产渲染会话为 `fbb092e5-ec68-4082-9c8a-e0c2a7ff32aa`，附加 UV 测试会话为 `e32d4230-96f1-4504-b0bc-1fa14d70163a`，实际 cwd 均为本 ABI 工作区。正式结果和原始事件保留在证据目录；子任务结束没有替代 Codex 的审查、构建和运行验证。附加 UV 审查发现的缺少真实 FVF 检查及两处类型拼写均已修正，再完成运行与反例验证。

最终增强产品 SHA256 为 `c142d1b3f79128e13ea5bbe5ea5d848bcd1a48fa7d9bd197b8c6d044dab98e03`，原始兼容产品为 `39196936121deabf4594b6a82e886459ae06b1546b78231fdd6ccfe0c731f407`。正式 `build/release/MikuMikuDance.exe` 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`，没有覆盖。测试仅操作 Default 桌面上的自有隐藏窗口，不使用全局输入或加载第三方插件。独立渲染专项不调用 Present；工程专项运行真实宿主初始化和命令路径，不以其结果证明桌面呈现。

## 尚未取得的证据

- 桶排序基础场景的 SM2、SM3 仍以非背景像素、真实 shader 和索引为证据；Maximum 已有独立彩色合成预期。
- 非空模型工程、真实手动写盘失败时的新增设置行为及正常失败通知。
- Maximum 的额外属性、透视插值及裁剪插值，非零视口与多目标边界。
- 预热分配失败、晚期分配异常不重放，以及外部效果退化面的运行验证。
- 缓存峰值、跨帧预算、多视口和极端 inverseW 插值。
- 真实 MME、Win7、AMD 与完整视觉验收。当前本机适配器是 Intel，不能替代这些平台证据。

可选 D3D12 预览仍有忽略 Off 与范围选项的问题，独立方案在 `d3d12-review-plan.txt`，未由本批 D3D9 接入解决。公共 `mms.edit` 的开放条件和完整 ABI 清单继续保留。
