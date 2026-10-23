# legacy MME 改名与子目录检测

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批实现 P6「legacy_mme 改名和子目录兼容」的模块识别部分；真实旧插件验收仍属外部条件。

## 实现

- 新增 [legacy_mme.hpp](../../include/mikumikustudio/legacy_mme.hpp)：`LegacyMmeModule/LegacyMmeLoaded/LegacyMmeHackModule/LegacyMmeHackLoaded`。先走 `GetModuleHandleW("MMEffect.dll")` 快速路径，未命中时用 `K32EnumProcessModules` 做节流（500 ms）的基名前缀扫描，匹配 `MMEffect`／`MMHack` 前缀并校验模块仍已加载。子目录中的模块按基名识别，不要求位于 EXE 目录。
- 14 个源文件中的 30 处 `GetModuleHandleW(L"MMEffect.dll")`／`MMHack.dll` 判断改用该检测：渲染、效果路径/名称/菜单/APNG/文件监视、模型导入、MMV 菜单等；快速路径保持原名行为不变。
- 新增 [legacy_mme_tests.cpp](../../tests/legacy_mme_tests.cpp) 与 CTest `legacy_mme_renamed_detection`：复制标记 DLL 为 `MMEffectJP.dll`（改名）、子目录 `plugin/mme/MMEffectSub.dll`、无关名 `NotMMEffect.dll`，验证加载后识别、句柄一致、卸载后不再识别、无关名忽略（20 项）。

## 验证

证据目录 `build/verification/abi-legacy-mme-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `legacy_mme_renamed_detection` | 通过，20 项；MMEffect 与 MMHack 的改名均识别，卸载后恢复未加载，无关前缀忽略，MMHack 不误报为 MMEffect |
| `model_alpha_*` CTest | 14/15 通过；仅既有的 `model_alpha_sort_off_mme_presence` 失败 |
| 宿主事件探针 | 通过 8 项 |
| 默认全量契约套件 | 通过，`PASS checks=44792` |
| 完整产品 | 构建通过，SHA-256 `6eeda4c51ab4ce6179a704a179a73ab542827bd6363a5d22d84b05598cf2e729` |
| 原始兼容产品 | 构建通过，SHA-256 `4296ec5aba98c0725f3d7ff630695f118a73912ab81cd9e404d063a121dee926` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 只识别基名以 `MMEffect`／`MMHack` 开头的改名；完全去掉前缀的任意改名仍不识别。
- 扫描有 500 ms 节流，插件运行中极晚加载的模块最多延迟一个节拍被识别；真实旧 MME 插件、未知导出与调用约定、寿命和实际渲染结果仍未验收。
- 未加载任何真实 MMEffect.dll；标记 DLL 只验证存在性检测，不覆盖插件内部行为。
