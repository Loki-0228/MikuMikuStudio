# MMDFacade：Effekseer 与 FirstPersonCamera 接入修复

日期：2026-11-15。独立工作区 `D:/MMD-Next/MMDFacade`，分支 `feature/mmdfacade-compat`。
固定基线仍为 `a74e6e1267455125fe7f7e8eed7c71ece1c21b9a`；此前本地提交为 `93535fd`、`a4e6dde`。
本记录与本次修复一起提交，未推送；没有在正式 abi/main 工作区执行修改。

## 实际结果

- 真实 EffekseerForMMD v0.25 x64：显式导入 `.efk`、加载配套模型、真实像素绘制、成功 Reset、停止卸载、重启、删除模型均通过。
- FirstPersonCamera：基于固定上游源码的 `MMSFirstPersonCamera.dll` 适配版，通过真实 ABI 4 工厂及键盘/鼠标测试。原版 v0.01 DLL 仍不支持，已验证在工厂之前明确拒绝。
- 保留 MME 0.37 x64、ABI 3 观察插件、任意 EXE 文件名和两处独立 MMDPlugin 提供者路径。
- 正式包输出到本工作区 `build/release`。相机放在 `optional-plugins/first-person-camera`，含清单和许可，不自动启用。用户下载的 Effekseer 文件未修改，也未放进正式包。

## 先失败，再修复

1. `build/system-import-red-result.log` 记录原加载器把 Windows WinSxS 的 GDI+ 当作不合法同名 DLL 的失败。现在按当前激活上下文核对 Windows 服务目录及清单，并验证已加载模块与 Windows 选择一致；私有同名 DLL 仍拒绝。普通搜索与旧系统回退分支均有测试。
2. `build/mmdfacade-functional-of_4r9sr/0/result.log` 记录资源打开成功、粒子绘制成功，但最终图像差异为 **0**。临时 GPU 查询和阶段截图确认粒子随后被覆盖。改用 MMS 现有原生材质绘制观察接口，在 MME 选好目标之后调用插件，并用 COM 接口取得底层设备。最终测试已移除 IAT/vtable 诊断探针。
3. 无 MME 时，文件查询原先按 CP932 编码中文路径，真实插件报告无法读取 `.efk`：`build/mmdfacade-functional-n08c2bj3/0/result.log`。Effekseer 回调现在使用 Windows ANSI 路径编码；该回调还从 MMS 当前模型读取对象序号，无须依赖 MME 的对象登记。
4. 人为无效 Reset 与 MME 同时运行时，旧 MME 弹出 `Failed to reset MikuMikuEffect`，记录于 `build/mmdfacade-functional-b6himmp_/0/result.log`，没有计为通过。未改变旧 MMHack 的既有 reset-on-failure 约定。失败 Reset 的 Effekseer 恢复验证改在无 MME 的真实原生设备上独立进行。

## 固定输入与 ABI 依据

| 输入 | 身份 |
| --- | --- |
| 用户下载的 Effekseer DLL | `D:/MMD-Next/tests/EffekseerForMMD_v0.25_x64/plugin/EffekseerForMMD/EffekseerForMMD.dll`；SHA-256 `2a7cf3d9afce099228168a1529589f48b5a232befa24cbaa4ab363756c11f532` |
| 原版 FirstPersonCamera v0.01 | SHA-256 `d3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef` |
| 相机适配源码 | [MikuMikuShaders/FirstPersonCamera f71c300](https://github.com/MikuMikuShaders/FirstPersonCamera/tree/f71c300f31eaa5c0e1543839854056774035f119)，MIT；用 MMS 相机 C API 代替 MMDMainData |
| Effekseer 源码对照 | [oigami/EffekseerForMMD c55b2f4](https://github.com/oigami/EffekseerForMMD/tree/c55b2f4e0e26cee473931624f0180eec84c297e2)，只作为回调和资源行为依据，不把源码提交等同于用户 DLL 的构建提交 |
| 用户参考头文件 | `D:/MMD-Next/refs/mmd_plugin.h`；SHA-256 `775fb19fb67442cff91b31914d440e5901e58cdb6dfa7b7213d22a9ce53b53f0`；250 个虚函数签名与摘录一致，证据 `build/header-signatures.json` |
| 基础粒子样例 | [Effekseer 固定提交 b570008](https://github.com/effekseer/Effekseer/tree/b5700082b49bc5f919610880dafddc90211a9a4e) 的 `block_simple.efk` 与 `Model/block.efkmodel`；Git blob 分别为 `6819b866d73a4f06e17015f6058a843c6379a51e`、`93be80369f3d3db7566d0aaacfd892d4acf06cb2` |

ABI 2/3/4 的工厂、寿命、窗口、输入、D3D9 与 D3DX 回调及逐插件依赖见[接口说明](../MMDFACADE.md)。
版本号没有降级伪装。MMDFacade 的旧 MME 37 个导出与 MMHack 的 22 个导出保留原签名和序号；独立 MMDPlugin.dll 未改名。

## Windows x64 Release 验证

MSVC v143 / 19.44，full 配置、DX12 关闭；资源输入没有改动，继续使用[前次记录](mmdfacade-2026-11-15.md)中已核验的同源 `.res`。
下列子进程均使用隔离目录和用户 DLL 副本。产品窗口检查使用隐藏窗口；真实 D3D9 读取像素，不能理解为人工操作了用户当前编辑器。

| 验证 | 结果与证据 |
| --- | --- |
| CTest | 11 项通过；含 SxS、导出及路径预检、ABI 2 配置、MMHack 寿命、菜单、48 组 ABI 2/3 回调矩阵和 6 组相机输入矩阵；`build/facade-final-ctest.log` |
| 相机输入 | WASD、抬键、文本焦点、Ctrl/Alt/Win 快捷键、禁用窗口、播放保护、视口拖动、浮动窗口、失焦、取消、停止卸载及光标恢复；`build/facade-camera-final.log` |
| 真实插件功能：MME 共存 6 组 | 三种 EXE 名 × 两处提供者位置；中文 `.efk` 目录；实际窗口相机按键及鼠标；Effekseer 启用/重置/重启相对停止状态都改变 **998 个像素**；`build/mmdfacade-functional-gmigl_rh/summary.json` |
| 真实插件功能：无 MME 6 组 | 同上组合和像素结果，额外验证真实失败 Reset 后成功恢复；`build/mmdfacade-functional-8zqivtkg/summary.json` |
| Effekseer 产品加载 24 组 | 启用、禁用、缺失 DLL、未知配置；同时保留 MMDUtility 菜单及 MME；工厂/销毁及退出正常，源文件摘要不变；`build/mmdfacade-candidates-vbwsxwlx/summary.json` |
| 相机适配产品加载 18 组 | 启用、禁用、缺失；`build/mmdfacade-candidates-7ktyfclp/summary.json` |
| 原版相机拒绝 18 组 | `first_person_camera_v4` 下报告 `original MMDMainData access is unsupported`，拒绝发生在工厂之前；禁用和缺失也有检查；`build/mmdfacade-candidates-43zvx68l/summary.json` |
| MME 0.37，原版与内置 MMHack | 各三种 EXE 名；实际 FX 开关、删除、退出和缓存迁移回归；绿色像素都是 **286146 → 0 → 286146**；`build/verification/legacy-mme-b8hcv6qo/summary.json`、`build/verification/legacy-mme-bg1drfjs/summary.json` |
| 缺新桥及只留旧名称 | 各 6 组产品窗口测试，明确报告缺少 MMDFacade.dll，ABI 3 观察插件仍可正常退出；`build/mmdfacade-product-mutmbgtr/summary.json`、`build/mmdfacade-product-xg63hkhr/summary.json` |
| 打包 | 13 项 Python 测试、PowerShell 布局与缺输入检查通过；实际包与构建 DLL/EXE 摘要一致；`build/facade-package.log` |

加载矩阵的 `pluginUsable=false` 是该脚本刻意保留的“仅验证加载”标记。粒子与输入可用结论来自独立功能矩阵，汇总索引为 `build/facade-final-evidence.json`。

## 未支持或未执行

- 原版 FirstPersonCamera 的 MMDMainData 访问、通用 ABI 4/D3DX 效果 hook、任意导入改写 hook、其他依赖原版 EXE 固定地址的插件未支持。
- 相机适配不增加撤销记录或自动注册关键帧；不覆盖所有 ABI 3 输入接口。
- 真实粒子只验证上述基础模型样例；复杂粒子、折射、ray-MMD/其他后期组合、视频/透明导出、PMM 保存再打开、DX12/MMV 及 Win7 真机未验证。旧 v0.21 DLL 没有重跑功能验证。
- MME 的失败 Reset 提示仍保留；没有把无 MME 的恢复结果外推到这一组合。
- REA 可读取 PE，但反编译后端配置路径失效，未得到反编译结果；本记录的功能结论来自固定上游源码和实际 Windows 运行测试。

## 复现入口

```powershell
cmake --build build/cmake --config Release --target MikuMikuStudio scene_lifecycle_mme_tests first_person_camera_tests system_import_tests legacy_callback_tests
python -B tests/plugins/run_mmdfacade_functional.py --build build/cmake --effekseer D:/MMD-Next/tests/EffekseerForMMD_v0.25_x64/plugin/EffekseerForMMD/EffekseerForMMD.dll --effect build/candidate-plugins/effekseer-sample/block_simple.efk --mme D:/MMD-Next/refs/MMEffect_x64_v037
# 追加 --no-mme，独立验证失败 Reset 恢复。
python -B tests/plugins/run_first_person_camera.py --build build/cmake
python -B tests/plugins/run_mmdfacade_callbacks.py --build build/cmake
python -B tools/package-abi-release.py --force
```

样例素材需要提前准备，测试不会把缺失文件当作成功，也不会从网络隐式下载第三方 DLL。
插件安装及清单示例见[说明中的可选插件安装](../MMDFACADE.md#可选插件安装)。

## 发布文件摘要

| 文件 | SHA-256 |
| --- | --- |
| `MikuMikuDance.exe` | `b0b65423b118c30ae380d54ecdf70d19c2accdcefcb44dc509fcf3be12a9733a` |
| `MMDFacade.dll` | `db1633b9380c58143869955d3395ee1a2b937dcbe4b771e8a28de7af10d5e2e7` |
| `MMHack.dll` | `56a80edc5447bf444b75e7d8aaaadf6bf703626a4fad84057496a19048736c84` |
| `legacy-mmdplugin/MMDPlugin.dll` | `9fe221f65bee0f0266b93b315c172153b79f19c52b20246f182fac0330ad76fa` |
| `optional-plugins/first-person-camera/MMSFirstPersonCamera.dll` | `8422e0b7e293a9cecdecc7e62e53dacd2a6038e63f09c788ba34efbc0d1f8ec9` |
