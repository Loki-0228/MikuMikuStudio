# MME 后端启动请求

标准 ABI 宿主在创建设备前读取原生插件声明，插件 DLL 的普通 `create/start` 生命周期仍在设备创建后执行。ABI 主版本保持 1。

原生插件完整清单增加：

```json
{
  "optIn": false,
  "startup": { "renderBackend": "mme-d3d9on12" },
  "requiresServices": [
    { "id": "mms.render.mme.on12", "major": 1, "minMinor": 0 }
  ]
}
```

这是完整原生清单的片段，不是独立清单。请求必须同时声明该只读服务，使旧宿主在加载 DLL 前拒绝插件。未知启动字段、未知后端或缺少服务依赖均拒绝清单。`optIn` 使用普通原生插件规则：缺省/false 表示复制安装后启用；true 表示必须额外启用。黑名单在两种情况下均优先。

宿主使用与普通插件一致的发现、层级、重复 ID、依赖、`enabledPlugins`／`disabledPlugins` 规则。只有启用的原生包可以提出请求；声明本身不会提前执行插件 DLL。`legacy_mme` 不进入原生 `started` 集合，不能用普通 `dependencies` 表示“原版 MME 已加载”。该条件由宿主根据实际模块及安装路径校验。

用户入口只有 `--dx9` 和 `--dx12`。默认优先 DX12，两个参数同时出现时 `--dx9` 优先。MME 实际加载且请求启用时，DX12 选择适用于宿主与 MME 共同使用的设备：旧 D3D9 调用由桥接层转换到 D3D12，不另建一个绕过 MME 的原生主视口。DX9 选择始终禁止启用该桥接。

## MMEPatch 对接边界

本分支从 Patch4MMS 的 `2a7619c` 复用桥接开关与工厂代码，并将启动请求、状态服务接入标准 ABI 宿主；未修改外部 MME-DX12-Patch4MMS 工作区。宿主查询**实际已加载的 MMHack.dll** 上现有接口：

```cpp
extern "C" BOOL WINAPI MMSSetMmeDx12PatchEnabled(BOOL enable);
```

- 原版 MMEffect.dll 加载成功后、首个 `Direct3DCreate9` 之前调用 `TRUE`。返回真表示桥接可尝试 D3D9On12，并不代表设备已经成功创建。
- 桥接层负责把已拦截的 `Direct3DCreate9` 转到 `Direct3DCreate9On12`，并维持原来的 MME 设备／D3DX 回调包装。
- 设备必须透传 `IDirect3DDevice9On12` 查询，且 `GetD3D12Device` 成功，宿主才报告实际 DX12。
- 工厂、硬件设备创建或后端确认失败后，宿主释放本轮设备及工厂，调用 `FALSE`，重新走原来的 DX9 创建路径。返回真必须保证恢复 DX9；返回假时停止初始化，不能声称已回退。
- 导出缺失或 `TRUE` 返回假时，不尝试 On12，使用 DX9 并记录原因。现有 MME 导出签名、序号不变；新增导出不能占用既有序号。

本分支的配套 MMHack 已提供该接口，位于新增的 23 号导出；既有 1–22 号保持原样。旧版 MMHack 不包含这个接口，单独放入普通 Patch 插件 DLL 不会增加桥接能力。宿主不按插件文件名或插件 ID 特判，也不加载原版 EXE 固定地址。

## 只读服务

公共头文件：`sdk/mms/include/mms/mme_on12.h`。通过 `queryService("mms.render.mme.on12", 1, 0, ...)` 获取 `MmsMmeOn12ApiV1`，在 UI 线程调用 `query`；先初始化输出结构的 `MmsHeader`。

状态包含请求后端、MME 实际后端、启动请求是否启用、MME 是否加载、桥接是否支持以及稳定的原因码。实际后端只有确认设备之后才填写：9 表示 DX9，12 表示 D3D9On12，0 表示没有可报告的 MME 设备。MME 未安装、请求禁用或回退时服务仍然存在，插件可以展示原因。此服务不表示无 MME 时的原生 DX12 主视口状态，也不允许普通插件在运行中切换后端。

本分支已联合验证原版 MME 的材质 FX 像素、Effekseer 粒子、相机、设备重置与退出，详见 `docs/records/mmdfacade-on12-integration-2026-11-16.md`。仅独立设备确认不能替代这些功能验证。

随包的 `PLUGINS/dx12mmepatch` 提供实际原生 ABI DLL 和声明，复制安装并重启即可生效，不再分发启用/停用脚本。内部 ID 保持 `patch4mms` 以兼容已有黑名单，宿主不会自动改写用户配置。入口 DLL 的 PE 和直接工厂导出在启动时静态预检；普通插件的 create/start 不提前执行。启用的启动插件损坏或加载失败，宿主在初始化阶段汇总弹框。
