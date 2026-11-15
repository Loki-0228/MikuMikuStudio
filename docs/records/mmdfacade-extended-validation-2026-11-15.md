# MMDFacade 扩展验证：启停、MME 像素、设备恢复与后端

延续 `feature/mmdfacade-original-camera` 的 `558c7e5`，仍使用独立源码/构建目录，不修改本地 abi 或在用安装。原版 DLL 摘要与[上一轮记录](mmdfacade-original-camera-2026-11-15.md)一致。下面的路径除特别注明外相对于 `D:/MMD-Next/MMS-ABI-integration/build`。

## 测试发现并修复的缺陷

1. **重复启停泄漏缓存保护句柄。** 原版相机连续启停 20 次，句柄从 233 增到 393。`legacy_runtime.cpp` 每次准备缓存都会重复保留相同目录/文件的保护句柄。现按路径复用已经持有的句柄，复查文件内容前重置读取位置；保留原有只读与禁止替换约束。修复后为 **233→233**，12 组 EXE/组件布局均完成 20 次循环，原版 DLL 每次正常卸载，缓存文件字节不变。
2. **原版 Effekseer 被原生 DX12 视口绕过。** 不安装 MME、请求 DX12 时，原生视口提前返回，Effekseer 的 BeginScene/Draw 回调数均为 0，启用/停用像素差均为 0。现已启动的 Effekseer 渲染档案明确要求 D3D9，宿主使用 D3D9 回退并记录原因。相机和窗口观察档案不触发这一渲染限制。修复后重置前、重置后及重启的粒子像素差均为 1030。
3. **`--dx12` 被当作场景参数。** WinMain 已明确识别该参数；`--dx9` 仍优先，兼容已有 preview 别名。此修复只保证参数解析，不声称原版插件进入了 DX12 后端。

| 失败证据 | 修复证据 |
| --- | --- |
| `original-camera-res_yegi/0/result.log`，句柄 +160 | `original-camera-y1478805/summary.json`，12/12；每组 20 次启停 |
| `mmdfacade-functional-7c6x3kr2/0/result.log`，native active、像素差 0；runtime.log 回调计数 0 | `mmdfacade-functional-fq2pxklt/summary.json`，无 MME、请求 DX12、实际 Effekseer D3D9 回退；像素差 1030 |
| 源码旧解析分支仅识别 dx9/preview | `mmdfacade-product-6hca_32_/summary.json`，带 `--dx12` 的 9 组产品启动/菜单/正常退出 |

## 原版 MME 0.37 的独立像素对照

使用原版 `MMEffect.dll`、内置 MMHack 与 MMDFacade，在 `PLUGINS/LEGACY` 中无清单发现。真实 `.x` 配合绿色 `.fx`，读取实际 D3D9 表面：

| 状态 | 绿色像素 |
| --- | ---: |
| 启用 MME | 286146 |
| 禁用 MME | 0 |
| 重新启用 MME | 286146 |

原名、英文改名、中文 EXE 共 3/3，通过绘制、开关、附件删除和退出；原 DLL 字节不变。还验证缓存版本改变后保留设置，以及设置被独占、来源不明确、硬链接三种情况显示具体错误。负例由测试 CBT 钩子确认实际错误框后以 8 退出，不将这种测试终止写成产品正常退出。

证据：源码工作区内 `build/verification/legacy-mme-m0ejg244/summary.json`；总日志 `validation/facade-extended/mme-final.log`。旧测试只期望无对话框拒绝且读取 stdout，最初未识别 stderr 中的新错误框证据，已修正为检查具体错误文本和实际对话框出口；没有放宽成“任何失败即通过”。

## 丢失状态与恢复

原版 FirstPersonCamera、Effekseer、MME 全部启动，载入真实 `.efk`。围绕真实 D3D9 设备注入 `DEVICELOST`/`DEVICENOTRESET`、Reset 失败及不可恢复状态，调用实际宿主 FrameDriver、读回与录制流程。没有替换实际宿主恢复实现，也没有切换显示模式。

| 入口 | 无 Reset 恢复 | 一次 Reset 恢复 | 两次失败后第三次实际 Reset 成功 | 不可恢复后停止绘制 |
| --- | --- | --- | --- | --- |
| FrameDriver | 通过 | 通过 | 通过 | 通过 |
| 录制读回 | 通过 | 通过 | 通过 | 通过 |
| 完整录制帧 | 通过 | 通过 | 通过 | 通过 |

12/12，均确认原版插件已启动、无失效对象调用、无恢复前继续渲染/读回。录制失败重试保持帧，恢复后只推进一次。证据：`legacy-loss-rtpor_dz/summary.json`。这是**状态故障注入**，不代表已验证物理 GPU 移除、驱动重启或真实切屏丢失。

## DX12 的实际结果

严格要求“原版 MME 设备也必须使用 D3D12”的用例**仍失败**：`IDirect3DDevice9On12` 查询返回 `0x80004002/E_NOINTERFACE`，状态为 `D3D9 fallback: original MME`。见 `mmdfacade-functional-t_maqtvy/0/result.log`。没有将正常加载或 D3D9 回退写成 DX12 兼容通过。

当前验证通过的是 DX12 请求下的正确 D3D9 回退：

- MME 共存：`mmdfacade-functional-j268mhdg/summary.json`，原版相机窗口消息、Effekseer 渲染/Reset/重启均通过。
- 仅旧 MMDPlugin 渲染：`mmdfacade-functional-fq2pxklt/summary.json`，原版 Effekseer 要求 D3D9，保持粒子输出。

负例像素对照固定使用相同 D3D9 表面；不能把停用后原生 DX12 输出与残留 D3D9 表面直接比较。需要 D3D9On12 的设备创建桥、实际后端确认，以及 MME/特效资源和重置回归后，才能声称“全部使用 12”。本轮没有修改用户负责的 MMEPatch。

## 系统输入仍未验证

新增 `--send-input` 用例，通过操作系统队列发送原版相机的 WASD、鼠标、模式与焦点转移。它只在自建窗口确实取得前台后发送输入，结束后恢复焦点与光标。

本环境多次不能把自建窗口置为前台，最终用例返回 **77 / UNVERIFIED**，没有发送任何按键。见 `mmdfacade-functional-0_sa3zbj/summary.json` 及 result.log。此前通过的直接回调/真实窗口消息测试保持成立，但不能替代这个系统输入测试。可在允许前台交互的 Windows 桌面运行已提交的同一用例；不绕过环境限制向其他窗口发按键。

## 构建与未修改宿主性能基线

Release 输出仍为 `build/facade-original-build/Release`。最终构建记录 `validation/facade-extended/fallback-build.log`。

性能对照另建 detached `19fb025` 工作区 `build/facade-baseline-src` 和 `build/facade-baseline-build`。生产源码、头文件、CMake 未改；仅复制同一测试驱动，资源的 29 个输入逐项一致。两边使用相同字节的 MME、Effekseer、MMS 相机适配 DLL 和兼容组件、相同场景、显式 D3D9；基线不能运行原版相机，因此此比较**不计入原版兼容测试**。各预热一次后 ABBA × 2；启动指标等待相机和 Effekseer 均 start，避免工厂/start 排序改变造成指标偏差。

| 中位数（每版 4 次，排除预热） | abi 基线 | 当前版 |
| --- | ---: | ---: |
| 相机与 Effekseer 启动完成（秒） | 1.24920 | 1.23181 |
| 整个验证场景（秒） | 1.89514 | 1.86919 |
| 进程累计 CPU 时间（秒） | 5.56250 | 5.52344 |
| 采样峰值工作集（MiB） | 564.43555 | 564.28711 |

证据：`facade-abba-fgwqaj3l/summary.json`、`samples.json`。每 20 ms 采样一次，峰值工作集可能漏过短峰；累计 CPU 时间是所有线程之和。此场景是短时真实特效与 Reset/启停对照，不是长时复杂舞台压力测试。样本少、差异小，不宣称提速、零开销或统计显著性。两边用相同显式测试清单，因此也没有把这组测量说成自动识别缓存的性能测试。

## 结果分类与剩余缺口

| 对象 | 本轮可证明的范围 | 不能据此证明 |
| --- | --- | --- |
| 原版 FirstPersonCamera v0.01 DLL | 12 组布局下的内存读写、直接输入回调、每组 20 次启停及句柄稳定；三插件共存和恢复 | 系统输入队列/真实鼠标操作仍未验证；其他二进制版本未支持 |
| 原版 EffekseerForMMD v0.25 x64 DLL | 实际粒子像素、Reset、停用/重启；请求 DX12 时使用 D3D9 回退仍绘制 | D3D12/D3D9On12 渲染未实现；没有全面覆盖所有特效类型 |
| 原版 MME 0.37 x64 DLL | 3 种 EXE 名、实际 FX 像素对照、设置保留、加载错误框、三插件共存 | 真正 D3D12 设备明确未通过；没有全面覆盖所有 MME 特效 |
| ABI 3 窗口观察样本 | 9 组真实产品窗口、菜单、退出 | 样本由未改上游源码构建；不等于整个原版 ABI 3 已兼容 |
| MMS 相机源码适配版 | 仅作为两版宿主的相同性能负载；兼容回归见上一轮单独记录 | 不替代原版相机验证 |

未修改 MMDFacade/MMHack 导出声明、签名或序号，独立 MMDPlugin.dll 名称不变。普通依赖不执行识别，原版 DLL 只在 LEGACY 递归发现；现有缺失/歧义失败弹框策略保留。当前原版相机支持的内存字段、同步规则以及 ABI 2/3/4 槽位核对表仍见上一轮记录，不能由本轮通过数推导为“完整兼容”。

系统输入前置条件失败的记录保持为未验证；测试还区分 SendInput 部分成功与完全未发送，部分成功后失败不会标为“没有输入”。最后一次测试驱动构建为 `validation/facade-extended/input-test-build.log`；这一诊断分支不参与已测的 ABBA 场景。

机器可读结果、二进制摘要及所有证据入口见 [mmdfacade-extended-validation-2026-11-15.json](mmdfacade-extended-validation-2026-11-15.json)。本轮仅本地提交，未推送、未合并 abi、未覆盖在用安装。
