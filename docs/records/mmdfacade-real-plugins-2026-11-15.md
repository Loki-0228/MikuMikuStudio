# Effekseer / FirstPersonCamera 真实发布 DLL 测试

> 此页记录此前提交的验证；后续 v0.25 与相机适配结果见[接入修复记录](mmdfacade-integration-2026-11-15.md)。

结论：**两个插件在本次 Release 中均不可用**。完成的是实际产品的加载拒绝、禁用、缺失文件与宿主存活检查；没有进入插件工厂、粒子渲染或第一人称相机控制。

## 环境和来源

- 独立工作区 `D:/MMD-Next/MMDFacade`，分支 `feature/mmdfacade-compat`，受测产品对应提交 `93535fd102ecb9fec9b217f0487cb14cbc6660b2`，基线仍为 `a74e6e1267455125fe7f7e8eed7c71ece1c21b9a`。
- Windows 11 x64，版本 `10.0.26200`，使用上一轮已生成的 MSVC x64 Release `build/release`。本轮未改产品代码、重编译或推送。
- EXE SHA-256：`07679ecb878c530b0e31fe40ab22a72c8fc252d6fc6cc6425ea096896e131064`。
- MMDFacade.dll、MMHack.dll、MMDPlugin.dll、MMDUtility 控制插件和 MME 0.37 的来源与摘要见[前次验证记录](mmdfacade-2026-11-15.md)。第三方发布 DLL 保持原字节，未改导出、版本、导入或签名。

| 插件 | 官方来源 | ZIP SHA-256 | 受测 DLL SHA-256 |
| --- | --- | --- | --- |
| EffekseerForMMD v0.21 x64 | [GitHub v0.21 发布包](https://github.com/oigami/EffekseerForMMD/releases/tag/v0.21) | `b2356cfda614fb95b4facdbc039c75a15826798cb7a1e2b03afd6cefc7063117` | `7087ae2cf543e9fbd63b4e91c581ab0811e1a6aa0feb8647600380e9a5f3edd1` |
| FirstPersonCamera v0.01 | [GitHub v0.01 发布包](https://github.com/MikuMikuShaders/FirstPersonCamera/releases/tag/v0.01) | `5cd1db761faa18d99fd0e57219e215731f8b8c1bcc6fe0965f87fa48e0dcaf5c` | `d3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef` |

Effekseer 官方 v0.21 发布说明注明有已知问题，并指向 [BowlRoll v0.25 x64](https://bowlroll.net/file/121167)。本轮按该页面的无密码公开下载流程请求，下载检查成功但文件请求返回 HTTP 403，未取得 v0.25。**下面的 Effekseer 结论仅对应 v0.21，不代表测试过 v0.25。**

ZIP、解包内容和原始 PE 清单保存在忽略目录 `build/candidate-plugins`。Effekseer 包内的原版 `d3d9.dll`、`MMDPlugin.dll` 和 MME 包装器未覆盖 MMS；仅把 `plugin/effekseerForMMD` 子包放入隔离安装，MMDPlugin 使用 MMS 的独立提供者。

## 实测结果

每种状态都覆盖 `MikuMikuDance.exe`、`Renamed Studio.exe`、`中文工作室.exe` 三种名字，以及旧 `legacy-mmdplugin/MMDPlugin.dll`、新 `PLUGINS/legacy-mmdplugin/MMDPlugin.dll` 两处路径。

| 插件与场景 | 组数 | 实际结果 |
| --- | ---: | --- |
| Effekseer，启用 `device_reset_v2` | 6 | `system DLL basename already bound outside System32: gdiplus.dll`；未进入工厂 |
| Effekseer，配置禁用 | 6 | 未进入 legacy 启动流程，模块快照中无候选 DLL |
| Effekseer，候选 DLL 缺失 | 6 | 明确报告 `missing file or reparse point` 及候选 DLL 完整路径 |
| FirstPersonCamera，误配 `window_observer_v3` | 6 | `legacy MMDPlugin missing direct export create3`；真实 DLL 没有被当作 ABI 3 加载 |
| FirstPersonCamera，配置禁用 | 6 | 未进入 legacy 启动流程，模块快照中无候选 DLL |
| FirstPersonCamera，候选 DLL 缺失 | 6 | 明确报告 `missing file or reparse point` 及候选 DLL 完整路径 |
| FirstPersonCamera，填写未实现的 `input_v4` 配置 | 6 | `unsupported legacy MMDPlugin profile`；这是配置层拒绝，不是 ABI 4 回调验证 |

最终 **42/42 组边界与控制检查通过**，不等于插件可用。每组均确认：

- 真实 MMDUtility 核心创建菜单并完成 `start/stop/destroy`，窗口能够响应消息。
- 真实 MME 0.37 成功加载；本轮只检查加载，没有重新做 MME 像素测试。
- 已映射的 MMDPlugin.dll 来自该组指定的新/旧路径；MMDFacade.dll 已映射；候选插件不在模块快照中，且无加载/启动成功日志。
- 正常发送 WM_CLOSE 后退出码为 0，最终矩阵没有强制终止。候选源 DLL 和安装副本摘要未变化。

证据：

- FirstPersonCamera：`build/mmdfacade-candidates-9e7g86k9/summary.json`，24 组。
- Effekseer：`build/mmdfacade-candidates-dmr7gu03/summary.json`，18 组；控制台记录 `build/candidate-plugins/effekseer-product.log`。
- 每组目录保留 `runtime.log`、`modules.json`、`process.log`、配置和独立数据目录。

Effekseer 首次探测预期会先命中 EXE 导入限制，实际更早遇到 GDI+ 路径限制，故测试断言失败，脚本清理时终止了该自建进程。这不是插件崩溃，也未计入最终 42 组；原始失败记录保留于 `build/mmdfacade-candidates-1166t8do/summary.json`。依据观察到的诊断重新完成了整个矩阵，未改动加载器来改变结果。

## 具体阻塞和未测功能

Effekseer 发布 DLL 为 AMD64，直接导出 `create2`（序号 1）、`destroy2`（2）、`version`（3）。这台机器的产品已映射正常的 Windows SxS 组件：

`C:/WINDOWS/WinSxS/amd64_microsoft.windows.gdiplus_6595b64144ccf1df_1.1.26100.9711_none_6ef2ab963dc761f3/gdiplus.dll`

加载器当前仅接受与 System32 目标路径相同的已加载系统 DLL，因而拒绝这一路径。这是宿主对 Windows SxS 的限制，不是证据表明插件包携带了同名 GDI+。

除此之外，PE 清单确认 Effekseer 还导入 `MMDPlugin.dll!getHWND`，以及字面模块名 `MikuMikuDance.exe` 的 14 个 `Exp*`：`ExpGetCurrentTechnic`、`ExpGetPmdFilename`、`ExpGetCurrentObject`、`ExpGetPmdNum`、`ExpGetPmdBoneName`、`ExpGetPmdBoneNum`、`ExpGetFrameTime`、`ExpGetPmdOrder`、`ExpGetPmdID`、`ExpGetCurrentMaterial`、`ExpGetPmdMorphNum`、`ExpGetPmdMorphName`、`ExpGetPmdMorphValue`、`ExpGetPmdBoneWorldMat`。旧 MMDPlugin 加载路径目前没有应用 MME 路径的 MMDFacade 导入改写；根据加载器代码，这仍是后续阻塞，**不是本轮已越过 GDI+ 后观察到的第二条错误**。三种宿主 EXE 名均得到同一首个错误，没有据此把当前 EXE 名要求为 MikuMikuDance.exe。

FirstPersonCamera 发布 DLL 为 AMD64，仅导出 `create4`（序号 1）、`destroy4`（2）、`version`（3）；导入 `d3dx9_43.dll` 的矩阵/向量函数以及 KERNEL32/USER32。宿主还没有 ABI 4 工厂和输入适配配置。结合已审阅的上游源码，工厂还会取得 `MMDMainData` 并直接写入相机私有字段，因此没有跳过预检或调用工厂。需要先以 MMS 公开的相机读写接口替代该依赖，再接入焦点、键盘、鼠标和退出时的光标恢复。

本轮未执行：Effekseer 粒子素材加载、BeginScene/EndScene/DrawIndexedPrimitive、实际粒子像素、原插件 Reset/PostReset；FirstPersonCamera 的 W/A/S/D、鼠标及相机移动；ABI 4 D3DX 效果回调。也没有把原版 EXE 固定地址或 MMDMainData 伪造为已支持。

## 复现

从本工作区运行，所有输入都指定绝对路径或工作区相对路径；脚本总是在 `build` 下创建新的隔离安装，不接管用户已有进程。脚本的 `checksPassed` 表示拒绝和宿主行为符合预期，`pluginUsable` 保持 false。

```powershell
python -B tests/plugins/run_mmdfacade_candidates.py --release build/release --candidate build/candidate-plugins/effekseer-v021/plugin/effekseerForMMD/EffekseerForMMD.dll --utility build/upstream-utility/out/Release/MMDUtility.dll --mme D:/MMD-Next/refs/MMEffect_x64_v037 --label EffekseerForMMD-v0.21 --profile device_reset_v2 --expect 'system DLL basename already bound outside System32: gdiplus.dll'

python -B tests/plugins/run_mmdfacade_candidates.py --release build/release --candidate build/candidate-plugins/firstperson-v001/MMDFirstPersonCamera/MMDFirstPersonCamera.dll --utility build/upstream-utility/out/Release/MMDUtility.dll --mme D:/MMD-Next/refs/MMEffect_x64_v037 --label FirstPersonCamera-v0.01 --profile window_observer_v3 --expect 'legacy MMDPlugin missing direct export create3' --unsupported-profile input_v4
```

不同 Windows 安装的 SxS 绑定状态可能不同，Effekseer 的首个诊断不能据此推广到其他系统；出现不同结果时应保留日志调查，不应只修改预期让测试通过。
