# main 名称与格式标记同步

日期：2026-10-20。目标分支：`codex/plugin-abi`。

## 同步范围

| main 提交 | ABI 中的处理 |
| --- | --- |
| `2423305` | 接受 PMX 骨骼、表情名称的标准 19 字节 CP932 绑定镜像，不再因此提示不兼容或生成多余副本。仍拒绝任意别名、无法编码的名称及不同完整名称的短名称碰撞。 |
| `edcafb1` | PMM 扩展容器使用 `MMSXPMM2`。其中独立修复工具的改动不再保留，见下文。 |
| `339f2e3` | VMD 名称字典使用 `MMSNAME1`；缩放使用 `MMSXMMSX`、`MMSXVMDX`；VPD 缩放使用 `MMSXVPDS`、`MMSXVPDX`。 |
| `e3fd52f` | 已有标量曲线与缓存记录使用 `MMSC`，骨骼曲线块使用 `MMSBCRV1`，透明排序诊断使用 `MMSBSP01`。 |
| `bcf595f` | 项目、工具、窗口类及显示名称统一为 MikuMikuStudio，补齐有符号比较、句柄转换、NVIDIA 修复 helper 导出和可选 OpenMP 延迟导入的编译警告修复。 |

工作区已有的 `mikudancestudio` → `mikumikustudio` 头文件目录、C++ 命名空间、CMake 目标和宏迁移一并完成。实际程序文件名仍为 `MikuMikuDance.exe`。该迁移也更新 `org.mikumikustudio.*` 私有记录名和内部环境变量，并非仅修改界面文案。SDK 的公开 `mms_*` C ABI 不因此改名。

## 读取规则

按本次要求，ABI 不保留 main 提交中的旧 MDS 标记读取别名。物理预设也仅接受 `MikuMikuStudioPhysicsPreset`，拒绝旧 `MikuDanceStudioPhysicsPreset`。

标准 PMM、VMD、VPD 的字段布局、原生插值字节和读取规则保持原样。未知附加尾段按原有未知数据规则处理，不将其解码为 MMS 专有数据。已知 MMS 容器内出现旧 MDS 负载时，按无效扩展处理；独立曲线或缓存记录不会因只差一个字节而获得兼容解码。

本批未引入 main 的新 PMM `0003` 独立容器、`MMVCURV2`／`MMSN` 原生时间映射，也未同步 H.264、FXAA 或其他渲染功能。ABI 现有 PMM 扩容、共同历史、文本所有权、插件侧车及程序目录 `data` 规则保留。

## 移除独立修复工具

按用户随后明确的要求，从 ABI 分支删除 `plugins/curve_repair`，包括 DLL、独立启动器、修复核心、侧车复制、测试和使用文档源码。本轮生成的独立工具构建目录也已删除。主程序的曲线编辑器、曲线数值修复及公共曲线编解码器保留。

该工具原本独立构建，未接入宿主默认加载或 ABI 发布脚本，因此无需替换宿主加载路径。早期 2026-10-18 记录只说明当时的合并和验证结果，不表示当前 ABI 仍包含该工具。main 工作区未修改。

## 验证

新增回归覆盖 MMS 编码与往返，以及具有有效长度／校验值的旧 MDS 标记拒绝。PMM 名称检查包含实际 PMX 加载、19 字节截断、CP932 半字符边界及碰撞反例。

完整配置和 `mmd932` 配置均完成 x64 Release 构建，保留 SDK 10.0.19041.0、Win7 基线及关闭 DX12 的设置。helper 的三个导出仍存在，检查未调用驱动注册函数。

| 验证 | 结果与证据文件 |
| --- | --- |
| 扩展、曲线、预设、缓存和可选图形导入 | 9 组通过。`test-codecs.log` 中缓存旧边界断言首次失败，按实际帧上限修正后由 `test-integration.log` 复验通过；产品重定时实现未修改。 |
| 标准 VMD 安全读取与兼容 | 通过，见 `test-integration.log` 和 `test-history-markers.log`。 |
| 插件 ABI 契约 | 通过，见 `test-integration.log`。 |
| PMM 元数据、真实 PMX 边界、完整工程保存重载 | 三个入口通过，见 `test-pmm-metadata-desktop.log`、`test-pmx-boundary.log`、`test-pmm-full.log`。 |
| 主 PMM 打开错误及工程身份 | 2 组通过，见 `test-load-errors.log`；最终核心重建后与 ABI、可选图形导入共 4 组再次通过，见 `test-final-full.log`。主文件锁定失败现在使用原有三语言打开错误提示，插件配对恢复和准备顺序保持不变。 |
| 六组格式相关共同历史专项 | 52,676 项通过，见 `history--*.log`。包含模型缩放、骨骼和表情曲线、组合模型曲线、全局 VMD 及 VPD。 |
| 原版兼容读写、状态、插件 ABI、可选图形导入 | 4 组通过，最终重建后复验仍通过，见 `test-original-final.log`。 |
| 正式 EXE 原名与中文改名 | 隔离副本的隐藏启动、菜单、工具栏和正常退出通过，见 `test-host-final-registered.log`。旧启动脚本误点宿主的“更多”按钮，现按命令 ID 范围选择唯一的已注册测试插件按钮；两次调用断言保留。 |

完整共同历史套件未通过本轮整体验收：修正正向夹具残留的 MDS 字节数组后，运行推进到 `NativeCameraHudHistoryChecks`，系统拒绝 `SetCursorPos`。保留原断言与失败日志，不将六组格式专项结果替代整个历史套件结果。

验收还发现原有 `vmd_load_safety_tests` 无条件注册到 `mmd932`，但其用例要求 MMS 专有缩放和曲线扩展，且该配置本就排除了对应实现。测试现在仅注册到完整配置；原版继续由 `original_profile_io`、`original_profile_state` 检查标准行为，不为链接测试而引入扩展实现。

证据目录为 `build/verification/mms-marker-sync-20261020/`。本机结果不代替 Win7 SP1 或 AMD 真机验收。同期完成的[模型顺序修复](abi-model-order-2026-10-20.md)在完整和基础配置各通过 13 个真实对话框场景。

本次正式程序更新至 ABI 工作区的 `build/release/MikuMikuDance.exe`，与 `build/cmake/Release/MikuMikuDance.exe` 核对一致。旧 ABI 程序保存在本轮证据目录，稳定版工作区和产物未覆盖。
