# MMDFacade：2026-11-15 Windows x64 Release 验证

基线：`abi-cloud-test` / `a74e6e1267455125fe7f7e8eed7c71ece1c21b9a`。
独立分支：`feature/mmdfacade-compat`；独立工作区：`D:/MMD-Next/MMDFacade`。
通过 `git fetch origin abi-cloud-test` 同步基线，未推送；未修改正式 abi/main 工作区。

## 改动

- 产品及新桥文件名采用 MMDFacade.dll；构建、PE 导入改写、缓存加载、依赖预加载、安装及打包同步。旧 MME 的 37 个宿主导出和 MMHack 的 22 个导出保持签名、名称与序号。MMDPlugin.dll 独立且保留两处路径。
- 加入有限 `device_reset_v2`：按 ABI 2 工厂创建/销毁，仅分发 Reset/PostReset。不增加 ABI 3 输入、绘制或 ABI 4 的兼容声明。
- 修正异常对象尚未销毁就卸载插件 DLL 的问题。测试实际复现了崩溃，修复后 pre/post 异常及回调中 Stop 均正常结束。
- 为 MME 在创建缓存之前的失败增加持久日志，缺失 MMDFacade.dll 时能明确诊断。真实 MME 测试还发现并修复了依赖预加载器的小写旧名遗漏。
- 上游 ABI 及逐插件依赖、迁移步骤见 [MMDFacade 说明](../MMDFACADE.md)。

## 先失败再接入

先加入 `legacy_reset_profile_tests` 并以 Release 构建运行，修改前退出码 1：
`FAIL unsupported legacy MMDPlugin profile`。证据：`build/reset-red-result.log`。
接入后该用例通过；测试 DLL 由独立的上游 ABI 类声明编译，未复用宿主槽号构造对象。
初次异常矩阵留下 `build/mmdfacade-callbacks-4gbroypf/summary.json`，反映 pre 回调抛异常后的卸载崩溃，最终矩阵包含此回归及 post 异常用例。

## 构建与资源

MSVC 19.44.35229，Visual Studio 2022，x64，Release，full 配置，DX12 关闭。
使用本机已缓存的 Conan 依赖；没有复制其他工作区的 CMake 缓存。
完整产品和场景探针的最终构建通过，日志为 `build/final-verified-build.log`。

本工作区从源码完成了一次完整内嵌效果预编译。新增场景测试工程后 MSBuild 再次启动相同资源生成，核对所有资源输入与基线均无改动后，停止本工作区的重复生成进程，在生成的 Release 工程中复用刚生成的同源 `.res`。只修改忽略目录中的生成工程，不修改源码构建规则。后续重新配置可能重新执行资源生成。
资源复用记录：`build/resource-reuse.json`；x64 `.res` SHA-256 为 `e0dd4944810db566552562f799d81e95e58be813a062c9cde42c12efcfe6875f`。

## 已执行并通过

| 验证 | 通过内容 | 本地证据 |
| --- | --- | --- |
| 9 项 CTest | PE 改写、37 个桥导出的名称/序号/参数返回值、缺失宿主导出、MMDPlugin 提供者优先级/损坏路径/直接导出预检、ABI 2 不借用 create3、重置配置、菜单归属、MMHack 场景路由与 COM 寿命，以及下述矩阵 | `build/final-ctest.log` |
| 48 组回调矩阵 | 三种 EXE 名 × 两处 MMDPlugin 路径 × 启用、禁用、缺失提供者、未知配置、错误版本、pre 异常、post 异常、回调中停止。覆盖 ABI 2 工厂/销毁、失败 HRESULT、pre/实际调用/post 顺序、ABI 3 start/窗口观察/stop/destroy、卸载后不再回调。启用组还执行真实 HAL D3D9 成功 Reset | `build/mmdfacade-callbacks-34a23g2_/summary.json` |
| 原版 MMHack + MME 0.37 x64 | 原名、带空格英文名、中文名；真实 FX 像素、效果关/开、对象删除、退出、用户源 DLL 不变、缓存代次更新保持 INI，以及锁/来源不明旧偏好/硬链接拒绝 | `build/verification/legacy-mme-15vr7n8i/summary.json` |
| 内置 MMHack + MME 0.37 x64 | 同上三种 EXE 名和相同操作 | `build/verification/legacy-mme-j86eombu/summary.json` |
| 真实产品正常安装 6 组 | 原名/英文名/中文名 × 新旧提供者路径。实际 MMDUtility 核心源码创建产品菜单，与真实 MME 同时加载；窗口响应，停止/销毁，退出码 0，无强制终止 | `build/mmdfacade-product-xs6jhx_h/summary.json` |
| 真实产品缺新 DLL 6 组 | 同上组合；明确记录缺少 MMDFacade.dll，未加载 MME；独立 MMDUtility 正常创建菜单，编辑器正常退出 | `build/mmdfacade-product-acw33gry/summary.json` |
| 真实产品仅旧名 DLL 6 组 | 桥只以 MMSAdapter.dll 名存在；不隐式回退，明确报告缺少新名，其余程序正常退出 | `build/mmdfacade-product-m52e3xq4/summary.json` |
| 打包 | 13 项 Python 合同测试及 PowerShell 发布布局/缺失输入保护测试；实际生成 `build/release`，四个程序文件与受测构建逐字节相同 | `build/package-tests.log`、`build/package-result.log` |

两套 MMHack 的全部六组 FX 像素读回都是 **启用 286146 → 禁用 0 → 恢复 286146**。这是实际读取渲染目标，不是只检查 DLL 加载或编译结果。
产品入口检查采用隐藏窗口，按子进程 PID 枚举菜单，没有控制用户已打开的编辑器，也没有人工视觉验收。

原版 MMEffect SHA-256：`74b3f5882a3bad28131cbc57994e2f960938e6ca850fc7139018ce4822bd87dc`。
原版 MMHack SHA-256：`e450b25e5cd2bcf125ca4b8eb9e57f858ec3ca379633c7d25f9dd40e864b390d`。
真实 MMDUtility 核心来自 `0bff27755a5d86dbb8a5be3f264101ee4bf52f86` 的源码及对应头文件，以 MSVC x64 Release 构建；DLL SHA-256：`c778de083c77a6850b56f98ac8f056d0e8c104c833f24aea420d4715a87612c5`。没有把其 MsgProc、子插件互查或整个插件集合列为通过。

测试第一次使用本机 Temp 时遇到访问拒绝；最终 CTest 将 TEMP/TMP 指向本工作区 `build/test-temp`。初次沙箱 MSVC 探针有 MSPDB140 链接问题，正常 Windows 构建环境配置成功。首次产品测试误读 MME 日志位置，修正脚本后才定位到实际依赖名称遗漏。上述失败不计入最终通过数，相关日志仍保留在 build 中。

## 最终构建摘要

| 文件 | SHA-256 |
| --- | --- |
| `build/release/MikuMikuDance.exe` | `07679ecb878c530b0e31fe40ab22a72c8fc252d6fc6cc6425ea096896e131064` |
| `build/release/MMDFacade.dll` | `71fa0a15992d2ace42edc3b844de2a1a86377ebbc5f12e69157edc641140f16d` |
| `build/release/MMHack.dll` | `56a80edc5447bf444b75e7d8aaaadf6bf703626a4fad84057496a19048736c84` |
| `build/release/legacy-mmdplugin/MMDPlugin.dll` | `d98716fd3f87de456ac49a62c60f78920140f149a65c59dd1d171a22df61203b` |
| MME 场景探针 EXE | `17b042c5ba6b8db9669cea1de42fd9212f57ac9c05a4806eff0f8aa7bc0b517f` |

发布预备目录不包含第三方 MMEffect、原版 MMHack、MMDUtility 或测试 DLL；没有推送、上传或对外发布。

## 未执行或未支持

- Win7 SP1 真机、其他 Windows/GPU/CPU 组合、其他 MME 版本；本轮结论限这台 Windows x64 主机。
- EffekseerForMMD 完整渲染、素材装载与其原版 PostReset 资源策略；FirstPersonCamera 输入/相机行为；ABI 4 D3DX 特效回调。
- 原版固定地址、MMDMainData、getDLL3Object、通用输入/绘制 hook、原样 AutoSave 及外亲/相机类插件。
- DispPlayingFrame 适配版真实帧号控件、ExamplePlugin 适配版的菜单动作、MMDDiscordRPC；它们的基线历史记录不算本轮验收。
- 大型 PMM 工程、EMM 全量重开、第三方 GPU 资源的重置恢复、长期设备丢失压力和人工画面验收。本轮 ABI 2 真实设备 Reset 与 MME 像素测试是分开的，未验证 Effekseer 与 MME 同场渲染。
