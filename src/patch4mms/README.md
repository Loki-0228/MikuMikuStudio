# dx12mmepatch

将整个 `dx12mmepatch` 文件夹复制到配套 MMS 的 `PLUGINS`，重启即可生效。无需运行 Enable、无需修改 JSON；随包声明由开发者提供。目录可自由分组，但须处于宿主配置允许的扫描深度内。

原版 MME 仍由用户提供：将 MME 0.37 x64 的 `MMEffect.dll` 放入 `PLUGINS/LEGACY/mme`。使用配套宿主和 `MMHack.dll`；只复制本插件到旧宿主不会补齐设备创建前的接口。

默认优先 DX12，`--dx12` 可显式选择；`--dx9` 始终优先。设备确认后，宿主与原版 MME 共用 D3D9On12 设备，实际后端为 D3D12；系统/桥接不支持或创建失败时回退普通 DX9。缺失或损坏插件会显示具体错误。

移出本包并重启即可卸载；已有黑名单继续生效。内部 ID 保持 `patch4mms`，因此旧的禁用配置不会因目录或 DLL 改名而失效。升级旧包时用本包替换旧 `PLUGINS/patch4mms`，避免同时保留两个相同 ID 的插件；不要删除 `PLUGINS/config.json` 或其他插件。

已验证的原版 DLL 为 MME 0.37 x64、EffekseerForMMD v0.25 x64、FirstPersonCamera v0.01。原版 FXAA 的 On12 后处理、系统级键鼠输入与物理 GPU/驱动掉线仍未完成验证，超长缓存路径仍有限制。MME DX12 Compat 的原生 FX/DXR 插件另行维护。
