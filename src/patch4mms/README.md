# Patch4MMS 接入包

这个包让受支持的原版 MME 与宿主共用 D3D9On12 设备，实际 GPU 后端为 D3D12。它复用 Patch4MMS 的桥接实现，不分发原版 MMEffect.dll，也不是 MME DX12 Compat 的 FX/DXR 插件。

1. 使用本次 MMDFacade 配套宿主和 MMHack.dll；仅复制 Patch4MMS.dll 到旧宿主不会增加桥接能力。
2. 将整个 patch4mms 文件夹放到宿主 PLUGINS 内，处于配置允许的扫描深度以内。可以自行分组，保留包内文件。
3. 将自备原版 MME 0.37 x64 的 MMEffect.dll 放在 PLUGINS/LEGACY/mme。旧 DLL 无需手写清单。
4. 双击本包 Enable.cmd，然后重启 MMS。它保留原有配置、扫描深度及其他插件的启停项；无需编辑 JSON。停用时运行 Disable.cmd 并重启。
5. 默认优先 DX12，也可显式使用 --dx12。--dx9 强制普通 D3D9；两个参数同时出现时 --dx9 优先。

随包 plugin.json 是原生插件声明，由开发者提供，用户不需要填写。启用前的普通 DX9 路径保持可用；桥接缺失、系统不支持或设备创建失败时回退 DX9。插件损坏或声明不完整会弹出错误。

已在原版 MME 0.37、EffekseerForMMD v0.25 和 FirstPersonCamera v0.01 共存时验证实际 D3D12 设备、选定特效像素、重置、启停与退出。不是对所有旧 FX、ABI 2/3/4 或所有显卡的完整兼容承诺。系统级键鼠输入、物理驱动掉线，以及原版 FXAA 的 On12 后处理仍未完成本轮验证。
