# MMDFacade 与 Patch4MMS 联调（2026-11-16）

后续安装行为已由 [dx12mmepatch 复制安装记录](dx12mmepatch-copy-install-2026-11-16.md) 更新：目录和 DLL 改名，移除 Enable/Disable，复制后默认启用。下文保留本轮联调时的历史行为与证据。

基于独立分支 `feature/mmdfacade-original-camera` 的 `bfd235d`，本地 abi 基线仍为 `19fb0255efdfd4330bf9be8f9fee35f84a8f9585`。源码、构建分别位于 `build/facade-original-src`、`build/facade-original-build`。没有修改另一个会话的 Patch4MMS/Compat 工作区，没有合并 abi、推送或覆盖在用安装。以下证据路径相对 `D:/MMD-Next/MMS-ABI-integration/build`。

## 接入内容

- 复用 Patch4MMS `2a7619c` 的 MMHack D3D9On12 工厂和开关代码；保留本分支现有 MME、D3DX 与旧插件回调。MMDFacade 37 项导出不变；MMHack 1–22 号不变，23 号新增 `MMSSetMmeDx12PatchEnabled`。独立 `MMDPlugin.dll` 名称和接口不变。
- 宿主在设备创建前读取已启用原生插件的 `startup.renderBackend= mme-d3d9on12` 声明。要求 `optIn`、对应只读服务、正常发现/启停/依赖规则、有效原生 PE 和直接工厂导出。只做静态检查，不提前执行普通插件 create/start，也不执行依赖库来猜接口。
- 原版 MMEffect 实际加载成功后才启用桥接。宿主与旧插件共用这一台设备；同时确认 `IDirect3DDevice9On12` 和它返回的真实 `ID3D12Device`，才公布 actual=12。
- 工厂/硬件创建设备/设备确认失败时释放本轮对象并恢复普通 DX9。关闭桥接失败则停止初始化，不能误报回退成功。`--dx9` 优先，正常入口仅 `--dx9`/`--dx12`。
- 提供 `mms.render.mme.on12` 1.0，只读返回请求、实际后端和原因。原生 ABI 主版本不变。
- 配套原生 `Patch4MMS.dll` 在正常生命周期里读取状态。随包提供声明和 Enable.cmd/Disable.cmd，不要求用户手写 JSON。包默认关闭，启用并重启后生效。
- 启用的启动插件损坏/加载失败会汇总弹框，保留原版插件错误处理。识别在启动阶段完成，渲染循环只读取后端状态，没有目录扫描或 PE 解析。

完整安装步骤见 [Patch4MMS 包说明](../../src/patch4mms/README.md)，接口约定见 [启动请求](../../sdk/mms/startup-render.md)。

## 先失败、后验证

| 问题 | 失败证据 | 接入后证据 |
| --- | --- | --- |
| 只有请求插件、宿主未接入 On12 | `mmdfacade-functional-zpdad86q/summary.json`，退出 7，实际设备 E_NOINTERFACE | `mmdfacade-functional-tdfyahrs/summary.json`，9/9 实际 On12 |
| 请求插件 DLL 损坏时只写日志 | `on12-controls-mh83q7g1/summary.json`，未出现要求的错误框 | `on12-controls-tq442sqd/corrupt-plugin/control.log`，确认具体错误框 |

接入期间修正了静态预检仅枚举旧插件导出、没有检查原生工厂的问题。没有通过去掉接口检查来放行 DLL。

## 原版 DLL 验证结果

| 对象与用例 | 实际结果 | 证据 |
| --- | --- | --- |
| 原版 FirstPersonCamera v0.01 + Effekseer v0.25 + MME 0.37，3 种 EXE 名 × 3 种组件布局 | 9/9；actual=12；相机窗口消息与内存同步；粒子绘制、Reset、停用/重启、删除与退出 | `mmdfacade-functional-tdfyahrs/summary.json` |
| 原版 MME 实际材质 FX | 3/3；绿色像素 286524 → 0 → 286524；设置跨缓存代次保留；错误框验证 | 源码工作区 `build/verification/legacy-mme-n_3r2n8b/summary.json` |
| 正式产品 EXE、ABI 3 窗口观察、原版 MME | 9/9；实际 On12、菜单、正常退出；覆盖原名/英文改名/中文名和新旧组件路径 | `mmdfacade-product-d18dl4wd/summary.json` |
| 实际 On12 设备上的丢失/恢复状态注入 | 12/12；渲染、录制读回、完整录制帧；Reset 连续失败后恢复及不可恢复时停止 | `legacy-loss-gqhwv2vp/summary.json` |
| DX9 优先、双参数顺序、禁用/未安装、旧桥接、缺失 MME、中文分组移动、缺失/损坏/不完整请求插件 | 11/11；正确实际后端、错误原因和弹框 | `on12-controls-tq442sqd/summary.json` |
| On12 工厂返回空后实际 DX9 重试 | 通过；actual=9、reason=6；原版三插件仍绘制/重置/重启 | `on12-controls-29_nm0x5/summary.json` |
| On12 五次 HAL 创建设备失败后实际 DX9 重试 | 通过；actual=9、reason=7；原版三插件仍绘制/重置/重启 | `on12-controls-5rrsd44y/summary.json` |
| 只读服务与启动协议 | 8/8；含真实设备确认、线程边界、禁用/缺失/开关失败 | `validation/facade-on12/protocol-final.log` |
| 启用/停用入口 | 5/5；中文带空格路径、幂等、保留其他插件与自定义字段、损坏配置原样保留 | `validation/facade-on12/configuration.json` |

Effekseer 首组粒子像素差为 1036，Reset 后和重启后同样为 1036。MME 的绿色像素来自专用材质 FX 的启停对照，不是从轴线颜色推断。原版 DLL 的输入摘要保持与上一轮一致；仅独立缓存副本进行必要导入适配。

这里的相机是未经修改的原版 DLL，未使用 MMS 相机源码适配版替代。ABI 3 窗口观察样本仍是从未改上游源码构建的测试样本，其结果单列，不能当成全部原版 ABI 3 已验证。

## 同机 DX9/On12 交错测量

相同最终宿主、原版三插件字节、场景、CPU 蒙皮配置与启动请求插件。同配置后续查询确认两边均为 RTX 4070 Laptop GPU（vendor=10de、device=2820、ordinal=0），On12 的真实 D3D12 LUID 为 `00000000:00019eee`；最后交付包分别在两后端再次通过绘制测试（`mmdfacade-functional-293699ci`、`mmdfacade-functional-d2glu0xx`）。两后端各预热一次后 ABBA × 2；每组 4 个有效样本。等待相机和 Effekseer 都 start 作为启动终点；每 20 ms 采样工作集。整个场景包含绘制、Reset、启停，不是纯 GPU 帧率测试。

| 中位数 | DX9 | D3D9On12 |
| --- | ---: | ---: |
| 插件启动完成（秒） | 1.19763 | 1.26907 |
| 完整短场景（秒） | 1.78840 | 1.86051 |
| 进程累计 CPU 时间（秒） | 4.96875 | 1.67969 |
| 采样峰值工作集（MiB） | 564.45117 | 633.87109 |

证据：`on12-abba-zn5si0ww/summary.json` 与 `samples.json`。这组数据中 On12 启动约多 0.071 秒、工作集约多 69.42 MiB，累计 CPU 时间更少；不同驱动/后端线程安排会影响 CPU 指标。样本量小，不能外推为复杂舞台提速或零开销，也不代表纯 GPU/显存测量。

## 构建与边界

Windows x64 Release 已构建，正式宿主构建日志为 `validation/facade-on12/final-build-complete.log`；失败注入测试驱动为 `failure-test-build.log`；插件包为 `package-build.log`。29 个资源输入与基线逐项一致，复用已验证编译资源。导出表对照为 `exports.json`。

- 上一轮“未接入 On12”的失败现在已由上述真实设备和绘制验证闭环，历史失败记录保留。
- 仍未证明所有旧 FX、多 pass 后处理或 DXR 兼容。另一个会话记录的原版 FXAA On12 后处理差异，本轮没有重新验证或修复；MME DX12 Compat 的独立原生 DX12/DXR 路径不在这次联调范围。
- 相机系统级 SendInput 仍受桌面前台条件限制；本轮通过的是实际窗口消息/回调与相机状态同步。没有把它说成真实用户键鼠操作全部验收。
- 设备丢失、工厂/设备失败由测试驱动注入；实际设备和恢复代码是真实的，但没有模拟物理 GPU 拔除或真实驱动重启。测试注入不进入正式 EXE。
- 一个过长的测试目录让原版相机缓存路径超过普通 Win32 DLL 路径限制，返回 206 并正常弹框（`on12-controls-kwdmns2a/summary.json`）。随后使用短编号测试目录完成回退验证。超长安装/缓存路径仍是具体限制，不能用中文路径通过替代它。
- ABI 2/3/4 和 MMDMainData 仍按上一轮已核实的档案/字段支持表限制。没有增加泛化内存假实现。

Release 产物为 `build/facade-original-build/Release`；原生插件 ZIP 为 `build/validation/facade-on12/Patch4MMS-win-x64.zip`，包含 DLL、随包声明、启停入口和说明，不包含原版 MME。与本次配套宿主和 MMHack 一起使用。

机器可读证据快照见 [JSON 记录](mmdfacade-on12-integration-2026-11-16.json)。
