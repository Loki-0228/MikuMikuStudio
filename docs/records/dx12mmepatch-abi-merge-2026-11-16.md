# dx12mmepatch 合入本地 abi（2026-11-16）

按用户确认，仅合入本轮 MMDFacade/dx12mmepatch 相关修改，不提交其他会话的未完成代码，不推送。最新 abi 基线为 `46607409c7e015e30496c8095b94e1685fab324f`；功能分支为 `feature/mmdfacade-original-camera`，复制安装提交为 `3b7af0e8f7a437335882abc1042159631a704837`。

整合在 `build/facade-original-src` 的 `integration/dx12mmepatch-abi-20261116` 分支完成，产物仍在 `build/facade-original-build/Release`，没有覆盖在用安装。后续本地 abi 接收这个整合结果。

## 合并与验证

唯一内容冲突位于 `src/plugins/manager.cpp`：abi 新增 `mms.settings`，功能分支新增 `mms.render.mme.on12`。保留两边的头文件和独立服务查询，未删去设置服务的生命周期检查。

以下证据路径相对 `D:/MMD-Next/MMS-ABI-integration/build`：

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 整合后的 Windows x64 Release 宿主、原版插件驱动、后端协议和设置测试 | 构建通过；29 个资源输入摘要与已验证基线一致 | `validation/facade-copy-install/abi-build.log`、`abi-resources.json` |
| 原版 FirstPersonCamera、Effekseer、MME 的复制安装控制 | 9/9；原版字节与用户配置不变；含黑名单、DX9、中文目录、错误弹框 | `patch-copy-ft1xfbj1/summary.json` |
| 不带参数的正式 EXE，3 种名称 × 3 种组件布局 | 9/9；实际 On12、菜单、停止/销毁和退出 | `mmdfacade-product-o5obwipv/summary.json` |
| MME 启动协议 | 8/8 | `validation/facade-copy-install/abi-services.log` |
| 全局设置管理器测试 | 1 项既有失败，不能记为通过；未合并的 abi 基线同样失败于第 186 行的事件计数断言 | `abi-services.log`、`abi-control-settings.log`（均在上述验证目录） |

首个原版三插件场景的 Effekseer 像素差为 1040，Reset、重启后均为 1040；窗口消息/相机内存同步通过。正式 EXE 的 MMDUtility 是未改上游源码构建的 ABI 3 测试样本，仍不冒充原版发布 DLL。系统级 SendInput、原版 FXAA 和物理 GPU 掉线没有新增验证。

为核实设置失败，单独检出未合并的 `4660740` 到 `build/abi-copy-control-src`，构建并运行同一测试，复现相同断言。主工作区已有另一会话对该测试的修改，未纳入本次提交。

## 安装

使用配套的新宿主与 MMHack，将 `validation/facade-copy-install/dx12mmepatch-win-x64.zip` 中的文件夹复制到 `PLUGINS` 后重启即可。无需 Enable、无需用户编辑 JSON。升级旧包时替换原 `patch4mms` 文件夹，保留已有 config.json；旧黑名单继续有效。

机器可读结果和当前构建摘要见 [JSON 快照](dx12mmepatch-abi-merge-2026-11-16.json)。复制安装的失败用例和修改说明见 [前置记录](dx12mmepatch-copy-install-2026-11-16.md)。
