# 2026-11-15：修正 Stage 诊断程序的 MME 启动配置

## 原因与 main 对照

上一份 Stage 骨骼索引诊断程序关闭了 `MIKUMIKUSTUDIO_PRECOMPILE_EFFECTS`。它嵌入的是 HLSL 源码；带原版 MME 启动时，`QueueDefaultEffect` 必须在窗口创建期间通过 `D3DXCreateEffectFromResourceA` 同步建立默认效果，因此复杂效果的编译阻塞了窗口就绪。

`D:/MMD-Next/MMS` 的 `main`（检查时 HEAD 为 `2381bafc40ed864cf15f39278f72bada600d96a7`）已有同样的 MME 同步资源入口，CMake 默认及本地构建缓存均开启预编译。其现有 `build/Release/MikuMikuDance.exe` 的资源 117、118、120 都是预编译字节码；诊断程序的对应资源均是源码。因此本项是诊断交付的配置错误，不能归因于 MME 本身，也不需要删除着色器功能。

旧诊断程序 SHA-256：`745ab88228c18a61a7d68004f8afc0e3b9f7c048490ec8971a0200b48dd4e0bb`。

## 修正

- 将 `build/stage-inherit` 的 `MIKUMIKUSTUDIO_PRECOMPILE_EFFECTS` 恢复为 ON。
- 使用现有 `scripts/gen_resources.py --profile full --compile-effects` 和系统 D3DX9 生成三份效果。保持源码及编译参数不变；本次独立编译耗时分别约 0.13、180.57、294.25 秒。
- 使用现有 Release 对象和库重新链接，仅更换生成的资源与输出路径，保留此前骨骼索引修复。本次没有修改产品 C++、HLSL 或资源编译脚本。
- 三份新效果资源与 main 现有 EXE 对应资源的 SHA-256 全部相同。核对结果：`build/mme-startup-20261115/resource-comparison.json`。
- 修正 EXE 已放回 `build/stage-inherit/Release/MikuMikuDance.exe`；另有带用户原版 MME 三个 DLL 的独立副本 `build/mme-startup-20261115/candidate/`。旧 EXE 保存在同级 `baseline/`，桌面 release 未替换。

修正 EXE SHA-256：`519b10a60f457da07b28445ca3a79345b4097bc78c439ba12c47a39605ea24cd`。

## 验证与范围

使用桌面 release 的原版 `d3d9.dll`、`MMEffect.dll`、`MMHack.dll` 副本，隐藏启动独立安装中的程序；窗口枚举严格限定本次子进程 PID。窗口收到 `WM_NULL` 并返回后记录就绪时间，检查实际加载模块的完整路径，随后发送 `WM_CLOSE` 验证空场景正常退出。

- 修正程序：主窗口约 0.768 秒可响应，三个本地 MME DLL 路径确认，正常退出码 0。结果为 `build/mme-startup-20261115/startup-fixed-4/result.json`。
- main 现有程序：主窗口约 7.040 秒可响应，同组 DLL 路径确认，正常退出码 0。结果为 `build/mme-startup-20261115/startup-main-reference/result.json`。
- 这些单次结果只验证启动完成，不构成分支速度比较。两者的其他代码、配置和缓存状态并不完全相同。
- 初期栈采样显示旧 EXE 在窗口创建的 D3DX 编译链中消耗 CPU；修正 EXE 的采样已进入 `WinMain` 消息循环。采样器有界终止由自身创建的进程，不能把终止码当成程序崩溃。
- 前两次就绪验证误用 Python `subprocess.STARTUPINFO.lpDesktop`，该动态属性未传给 Win32，实际枚举的是空桌面；它们的超时不是产品无响应证据。最终验证已改为隐藏启动并按子进程 PID 枚举窗口。早期采样结果里的 `private_desktop` 字段也不代表已验证的桌面隔离。
- 第三次验证已确认约 0.774 秒响应及正常退出，但模块字典把本地与系统同名 `d3d9.dll` 覆盖，模块断言无效；第四次改为保留同名模块列表，完整验证通过。

本次未重新运行大型 Stage 的带 MME 加载或视觉回归；此前 Stage 骨骼回归结论见 `stage-inheritance-index-2026-11-14.md`。ToonGlass 工程最终可打开但加载慢，仍是独立的加载性能现象。
