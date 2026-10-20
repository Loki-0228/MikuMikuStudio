# 声明式 INPUT 的状态刷新

INPUT 已接入原 `mms.ui.refresh.state` 1.0。插件可更新标签、说明和原因，并限制启用、只读与可见状态；底层 C 接口及 C++17 SDK 操作原元素、提供者、builder、队列和票据。普通生产宿主的 `mms.edit` 仍未开放，不能通过状态刷新使只读输入框获得编辑权限。

## 接口与状态规则

`MmsUiStateContentV1` 的 64 字节前缀及 56 字节服务表保持不变。新增 80 字节 `MmsUiInputStateContentV1`，在前缀后追加 enabled、readOnly、visible 和 reserved 四个 u32。新增字段位依次为 8、16、32，只用于 INPUT；服务及结构版本继续为 1.0。新宿主的 `supportedFields` 包含这三个字段，SDK 每次 `WriteInput` 先查询能力，再调用原 `writePrepared`。

旧 64 字节文本请求不会读取尾部。请求任一新字段必须提供完整 80 字节结构；声明完整尾部时，即使仅更新文本，也校验全部标量、未使用字段及两个 reserved。有效布尔值为 0 或 1，未设置的标量为零。旧宿主缺少所需字段时，`WriteInput` 返回 UNSUPPORTED，不修改输出或提供者。

宿主启用状态与 enabled 取 AND，宿主只读状态与 readOnly 取 OR，注册可见性与 visible 取 AND。未设置的字段恢复为本次宿主基线。只读属性、无 stage 回调及无受控编辑宿主均不能被放宽。INPUT 标签默认为元素标签，空标签回退到属性标签；说明和原因默认为空。

INPUT STATE 只检查属性注册描述及宿主编辑可用性，不调用属性 read、获取值快照或读取场景。原 SELF 和注册 SUBTREE 请求均支持；显式 DATA 绑定不改变 STATE 的属性身份检查。来源捕获固定准确的 PropertyRecord，元素、属性或提供者替换使旧贡献失效。后台合并、只读因果保护、消费顺序 DATA、STATE、LAYOUT、REPAINT 继续由原队列执行。

## 原生控件与失败恢复

标签使用真实 STATIC 子窗口，写入后读回核对。仅更新标签保留草稿、选区、IME 状态、编辑基线和修订。禁用、隐藏及设为只读先撤销待提交意图，再取消草稿及转移焦点，防止失焦触发旧提交。后续普通刷新及布局使用已发布状态，不能把插件限制重新覆盖为可编辑。

提示覆盖 wrapper、标签、真实 EDIT 和禁用区域；只读时仍可显示原因。提示文本由宿主保留，不在显示通知中调用插件。界面文案不修改注册元数据、工程数据或共同历史。

审查发现标签应用失败时，候选可见性曾提前改变禁用提示矩形。现已在应用前保存实际旧矩形，失败后恢复并读回；来源已替换时清除仍属于旧控件的候选，避免覆盖替换控件或新内容。无法确认恢复成功时暂停目标和布局快照。新增四组真实 TOOL 矩形反例覆盖显隐双向切换以及拒写、截断标签。

STATE 隐藏后可与 LAYOUT 组合收缩空位，并使用原引用恢复。用户布局隐藏会卸载控件，使目标及旧提供者失效；测试按该生命周期核对，不要求已卸载控件继续响应旧票据。

## 本机验证

证据目录为 `build/verification/abi-input-state-20261021/`。所有 GUI 专项使用独立隐藏桌面，不切换用户桌面。两套配置构建及 C11 结构布局编译通过，最终构建日志为 `build-reviewed-full-04.log`、`build-reviewed-original-04.log`、`build-c-layout-full.log` 和 `build-c-layout-original.log`。

| `plugin_abi_tests` 入口 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| `--input-state-only` | 1,559 项通过 | 1,559 项通过 |
| `--input-only`，附原编辑探针 DLL | 6,022 项通过 | 6,022 项通过 |
| `--state-content-only` | 748 项通过 | 748 项通过 |
| `--subtree-refresh-only` | 1,280 项通过 | 1,304 项通过 |
| 完整 ABI 契约，附 hello、lifecycle、edit DLL | 16,869 项通过 | 16,869 项通过 |

专项报告为 `ui-full-03/report.json`、`ui-original-03/report.json`，完整契约结果为 `contract-full-01/contract/result.log` 和 `contract-original-01/contract/result.log`。子树用例包含实际窗口检查，两套配置的断言数量分别记录。

新增测试使用真实 Registry、NativeUi、C 提供者、原队列、Windows 控件和 SharedHistory。覆盖仅文本、三个状态位、草稿及选区、合成 IME 消息、提示矩形、状态与布局、提供者及同句柄属性替换、后台合并、SUBTREE、无效 UTF-8、配额、只读因果及历史修订不变。另将旧 64 字节结构放在可读页末尾，后一页设为 PAGE_NOACCESS；验证旧文本调用成功，短结构声明新状态位时拒绝且不读取尾部，下一次有效调用仍成功。

首轮测试修正了两项夹具假设：用户布局隐藏会卸载控件；新增注册可能替换元数据存储，使较早借用的描述失效。最终夹具在全部注册完成后捕获描述，保留原生命周期和子树断言。最终报告使用第三轮，未将早期失败计为通过。

`product-smoke.log` 记录完整配置正式构建的原名及中文改名启动、原生工具栏、两个面板、侧栏实时跟随和关闭检查通过。它验证产品集成，不替代新 INPUT 的受控编辑夹具。`pe-inventory.json` 确认两套正式程序为 x64、子系统版本 6.1，无直接可选新渲染后端导入；正式构建仍为 Win7 基线、DX12 关闭。

## 最终 ABI 程序

物理首次替换提交为 `b06bf4d`，INPUT STATE 源码提交为 `9d879c82d7ec84b9a6b3b4258d6dba1cf7b9eabe`。最终完整配置程序已复制到 ABI 工作区的 `build/release/MikuMikuDance.exe`，9,233,408 字节，SHA-256 为 `180DB2580C99A3578F30B2539451FF82605AEBDEC337DAEBCE8B77EDF86CF65A`。

复制后对最终文件再次执行原名及中文改名的产品检查，结果均通过，见 `release-smoke.log`。`release.json` 记录来源提交、路径、长度及散列。上一版 Toon 程序保存在同证据目录的 `previous-abi-MikuMikuDance.exe`，散列为 `D6579C8D790683C002D91F1CCEAE8E606354DE3D7EDCEC830FF695EA78EAC515`。本任务未修改 main 工作区或稳定版程序。

## 验收边界

INPUT 夹具使用最小共同历史 participant，未构造实际 MMS 场景。历史及修订断言证明该夹具的显示刷新不提交编辑；实际工程播放头及场景求值仍需对应集成验收。合成 IME 消息不能代替微软拼音、跨屏多 DPI 或正式鼠标验收。

两套完整 ABI 契约本轮均一次通过，未修改菜单代码或干预测试输入。历史上的菜单自动选择超时仍按[菜单调查记录](abi-menu-selection-investigation-2026-10-21.md)保留，不能声称已修复。

其他挂载点、DOCK、自动依赖、通用菜单及面板 STATE、绘制动画和公共编辑服务继续列为未完成。Win7 SP1 x64 与 AMD 真机验收仍待对应设备。
