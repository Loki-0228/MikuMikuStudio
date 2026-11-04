# 旧 MME 插件包

ABI 分支通过原创 `MMSAdapter.dll` 转接旧宿主的 37 个 `Exp*` 导出，保留其名称、序号和调用参数。用户本地的 MMHack、MMEffect 由启动加载器适配：只在 `data` 中生成导入重定向副本，原文件保持不变。宿主可以使用任意 EXE 文件名。

宿主同时提供基于 MME-RE/src/mmhack 的独立 `MMHack.dll`，实现旧版 22 个状态查询导出（名称及序号 1–22）。省略清单的 `hack` 字段即可使用随宿主构建的桥接层；显式填写 `hack` 时继续使用用户包内的原版 MMHack。新原生插件使用 `MMSPluginGetApi` 和宿主函数表，不需要链接 MMSAdapter 或 MMHack。

## 安装

MMSAdapter、随宿主构建的 MMHack 与主程序放在同一个目录。MMHack 仅在启用 legacy_mme 包时加载。发布包保留主程序、原创 MMSAdapter、许可文件和 `PLUGINS` 基本结构。

以下是用户本地安装一个旧 MME 包的例子：

```text
MMS.exe
MMSAdapter.dll
PLUGINS/
  config.json
  effects/
    legacy-mme/
      plugin.json
      MMHack.dll
      MMEffect.dll
      MMEffect.ini       # 可选：首次使用时复制初始偏好
data/
```

`PLUGINS/config.json` 与原生插件共用扫描规则：根目录深度为 0，默认最多扫描 2 层，允许配置 0–64。遇到插件包后不再向包内继续发现其他包。

```json
{"schemaVersion":1,"maxDepth":2,"disabledPlugins":[]}
```

`plugin.json`：

```json
{
  "schemaVersion": 1,
  "id": "user.legacy.mme",
  "name": "MME",
  "version": "0.37.0",
  "kind": "legacy_mme",
  "entry": "MMEffect.dll",
  "hack": "MMHack.dll",
  "architecture": "x64",
  "minWindows": "6.1.7601",
  "reloadPolicy": "restart"
}
```

`entry`、`hack` 是包内相对路径，可以位于包的子目录。该类型不使用原生 ABI 的 `abi`、`capabilities`、`requiresServices`、`dependencies` 或 `nativeDependencies` 字段。启停通过 `disabledPlugins` 控制，修改后重启；同一进程只启用一套旧 MME。

这一路径在创建 D3D9 设备前加载 MMHack，不需要把旧 `d3d9.dll` 代理放到 EXE 旁边。已有根目录代理与插件包不得同时接管启动。

## 适配与存储

加载器验证 x64 PE 导入表，把其中的 `MikuMikuDance.exe` 模块名替换为 `MMSAdapter.dll`；函数名、函数序号、资源和其他内容不变。当前处理普通未绑定导入；绑定导入、延迟导入、越界或含糊节映射会明确拒绝。

缓存位置是当前 EXE 所在目录的 `data/plugins/legacy_mme/<内容哈希>/`。哈希包含适配后的两份旧 DLL 与 MMSAdapter。复用缓存前逐字节比较，加载时保持 DLL 只读锁，避免使用被替换的缓存。MMEffect 自身的目录查询仍指向 DLL 缓存，但其目标 `MMEffect.ini` 的 A/W 文件访问单独重定向到 `data/plugins/legacy_mme/settings/<插件身份哈希>/MMEffect.ini`。身份哈希只取稳定插件 ID，与 DLL 内容及缓存格式无关。INI 是旧插件专用数据；MMS 通用设置继续使用 `data/settings.json`。新文件从插件包 INI 初始化，未提供内容时才写默认设置；既有用户偏好不会被宿主覆盖。每个身份使用进程期独占锁，另一进程占用时该旧插件启动会明确失败。

旧版 DLL 缓存中的 INI 没有插件身份标记。稳定偏好缺失但检测到此类文件时，加载器保留它并报告准确目标路径；用户确认要沿用哪一份后，在 MMS 退出时将该 INI 复制到报告的稳定路径，再启动。加载器不按修改时间或 DLL 相同就替用户选择；已有稳定偏好后，升级适配器或旧 DLL 不再改变偏好路径。

加载不改变进程的全局 DLL 搜索目录。系统依赖使用明确的系统路径，保留 Windows 为 GDI+ 等组件选择的 WinSxS 绑定。已经开始安装旧钩子的初始化若失败，停止图形初始化；成功模块保留到进程结束，不提供热卸载。

## 验证边界

验收使用本地原版 MME 0.37 x64，以改名宿主、三级插件目录、实际 D3D9 设备和可切换的测试 FX 检查加载、像素变化、删除与退出。具体运行结果及文件哈希见本轮验证记录。

Win7 SP1 x64、CPU 基础路径和 AMD 仍是兼容要求；本机运行结果不替代 Win7、AMD 真机验收，也不证明所有历史插件和全部 MME 效果均已覆盖。

## 内置 MMHack 桥接

`src/legacy_mmhack` 移植 MME-RE 的状态查询、IAT 接管、D3D9/设备/mesh 包装、场景回调和材质解析实现；构建不依赖相邻 MME-RE 目录。D3DX 头文件及许可保存在 `third_party/dxsdk-d3dx`，链接复用宿主生成的 d3dx9_43 导入库。

使用内置桥接时，删除上述 JSON 示例中的 `hack` 字段，包内只需提供 MMEffect.dll 及可选 INI。加载器把内置 MMHack、MMEffect 和 Adapter 一起复制到内容缓存。原版 MMEffect 仍由用户提供，发布包不携带该文件。

宿主私有的 `MMSLegacyObjectBacking` 和 `MMSLegacyAccessoryAttachment` 提供完整对象地址、宽字符路径、借用资源及经过校验的附件父模型/骨骼。37 个 Exp 导出的原型与序号不变，ID 继续使用无符号低 32 位；桥接不再把该 ID 当作完整地址。标准效果同时跟踪资源创建与预编译二进制创建入口；设备销毁时清空其借用效果、技术与纹理状态。

验证入口：`legacy_mmhack_scene_router`、`legacy_mmhack_com_lifetime`；真实旧 MME 可运行 `tests/plugins/run_legacy_mme_probe.py --originals <本地原版DLL目录> --host <scene_lifecycle_tests.exe> --bridge <构建的MMHack.dll>`，覆盖中英文 EXE 改名、22 个状态导出的名称/序号、实际 FX 像素变化、启停恢复、对象删除、缓存升级及 INI 持久化。此验证不代表全部第三方效果或多设备组合已验收。
