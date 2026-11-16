# dx12mmepatch 复制安装（2026-11-16）

在 `feature/mmdfacade-original-camera` 的 `9992dc9563089c6b927983ce65ed0055ce10c192` 上修改。测试和打包均在隔离构建目录；没有覆盖在用安装，也没有修改另一个会话的 Patch4MMS/Compat 工程。以下证据路径相对 `D:/MMD-Next/MMS-ABI-integration/build`。

## 安装行为

- 包目录、显示名为 `dx12mmepatch`，入口为 `dx12mmepatch.dll`，版本为 0.2.0。复制完整文件夹到配套宿主的 `PLUGINS` 并重启即可，无需 Enable、无需用户手写 JSON。ZIP 内只有 DLL、开发者提供的 plugin.json 和 README.md。
- 宿主启动请求不再强制 `optIn=true`，使用原生插件的统一启用规则。显式 `optIn=true` 的其他插件仍需启用；黑名单仍优先。
- 内部 ID 保持 `patch4mms`，兼容旧的启用和禁用记录，宿主不会改写用户配置。旧包升级应替换 `PLUGINS/patch4mms`，避免相同 ID 重复安装。
- 服务声明、PE/直接工厂导出、实际 MME 加载和设备确认检查均保留。缺失服务等错误仍弹框。默认/`--dx12` 优先 D3D12，`--dx9` 和原有 DX9 回退规则不变。
- 必须使用配套的新宿主及 MMHack，单独复制此包不能给旧宿主补齐接口。原版 MME 仍由用户安装。

## 先失败、再验证

原有 `9992dc9` 包仅复制、不执行 Enable 时，实际仍为 DX9，严格 On12 用例退出 7。证据为 `patch-copy-bpvdxmho/summary.json` 和 `0/copy.log`；日志记录没有启用的启动请求。原始 DLL 和用户配置均未被改写。

修改后的 Windows x64 Release 构建通过：`validation/facade-copy-install/configure.log`、`build.log`。29 个资源输入与已验证基线一致，复用其编译资源。

| 验证组 | 结果 | 证据 |
| --- | --- | --- |
| 无 config.json，直接复制包 | 原版三插件实际 D3D12，退出 0；运行后仍无配置文件 | `patch-copy-u30toh_e/0/copy.log` |
| 已有配置、旧启用记录、旧黑名单、显式 optIn 的启停、`--dx9`、中文分组、缺失服务 | 与上项合计 9/9；所有配置逐字节不变、原版 DLL 摘要不变；缺失服务实际弹框并退出 8 | `patch-copy-u30toh_e/summary.json` |
| 正式 EXE 不带任何后端参数；3 种 EXE 名 × 3 种新旧组件路径 | 9/9；实际 On12、MME 加载、观察插件菜单与正常销毁 | `mmdfacade-product-jxtdpz7w/summary.json` |

首项为未经修改的原版 FirstPersonCamera v0.01、EffekseerForMMD v0.25 x64 和 MME 0.37 x64。实际窗口消息路由与相机内存同步通过；Effekseer 绘制、Reset、停止/重启、删除模型通过，像素差为 1035，Reset 和重启后同为 1035。实际设备确认为 RTX 4070 Laptop GPU 的 D3D9On12/D3D12。

正式 EXE 组的 ABI 3 观察插件仍是从未修改上游源码编译的 MMDUtility 样本，不能称为发布版原始 DLL。所有适配版与原版的结果继续分开记录。

本轮没有重测性能、原版 FXAA、系统级 SendInput 或物理 GPU/驱动掉线；此前 [联调记录](mmdfacade-on12-integration-2026-11-16.md) 的限制仍适用。没有因改名或编译成功扩大兼容性声明。

## 产物

- 配套宿主：`facade-original-build/Release`。
- 插件包：`validation/facade-copy-install/dx12mmepatch-win-x64.zip`，结构为 `PLUGINS/dx12mmepatch`。
- 本隔离构建的旧包移至 `validation/facade-copy-install/previous-package`，避免两包同时被发现；未改用户安装目录。
- 原始结果和产物摘要快照：[JSON 记录](dx12mmepatch-copy-install-2026-11-16.json)。
