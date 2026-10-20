# 声明式 INPUT 实现与验收

`MMS_UI_INPUT` 已连接原 `mms.ui` 属性和元素注册，在工具栏及 GROUP 中创建原生输入框。可写路径复用上一批属性暂存、准确事务和共同历史；普通启动没有编辑宿主，显示为只读。开发接口见 [INPUT 参考](../../sdk/mms/input.md)。

## 实现范围

`UiPropertyDraft` 持有起始规范值、工程修订、原始字段和 IME 组合状态。它接收半成品输入，完整解析成功后才生成候选。活动草稿不被布局和显示刷新覆盖，规范值或修订改变后拒绝旧候选。相同值的不同文本形式不生成历史。

`NativePropertyInput` 管理标签和 1 至 4 个 EDIT。Enter 与离开整组提交一次，VECTOR 内部切焦不提交。Escape、隐藏和禁用取消草稿。重绑改变字段数时重建窗口并恢复本组原字段焦点，外部焦点不受影响。每个默认字段上限 1 MiB UTF-8，超过默认十六进制容量的 BYTES 显示只读摘要，保留原值和底层 codec 容量。

宿主内部的 `CapturePropertyInput` 仅查询修订并复制规范值，不获取编辑权限、不移动播放头。提交通过原 UI 及编辑表创建准确事务，失败仅取消该事务。它没有增加公共 C ABI 或另一套注册树。

提交持有独立快照和失效标记。窗口消息重入导致取消、隐藏、禁用、新输入或新 IME 组合时，旧标记失效；Manager 将其绑定到准确插件和事务的属性 guard，已有 EditStore 检查覆盖准备回调、恢复数据租约、历史预留以及首次正式 commit 之前。旧请求返回后保留新草稿。首次正式 commit 已开始后不承诺撤回。

本体按键轮询、Enter 登记、Delete 删键及全局插件快捷键识别 INPUT 后代焦点，不消费其中的编辑按键。原生回调中的主窗口关闭请求延后处理，清理不调用已停止提供者。

C++ `InputSpec`、`PropertyInputSpec`、`PropertyInput` 和 `RegistrationBundle` 组合原函数表，保留原属性和元素句柄。失败只回收本次新注册，回收失败保留引用供 Close 重试。INPUT 不进入命令溢出菜单，含 INPUT 的 keepTogether 组不拆分；空逻辑 GROUP 不阻止兄弟按钮溢出。

## 验证结果

完整和原始兼容配置均使用 SDK 10.0.19041.0 构建。窗口测试在独立隐藏桌面运行，不切换用户桌面，测试临时目录位于当前 ABI 工作区。

| 验证 | 结果 | 本地证据 |
| --- | --- | --- |
| 完整配置 INPUT、真实 DLL、SDK、原生窗口、取消与历史 | 1,917 项通过 | `build/verification/abi-input-20261020/focused-04/full-input/result.log` |
| 原始兼容配置同组 INPUT | 1,917 项通过 | `build/verification/abi-input-20261020/focused-05/original-input/result.log` |
| 草稿、八类值、容量及分配失败原子性 | 两套各 277 项通过 | `focused-04/full-draft`、`focused-05/original-draft` |
| INPUT 焦点隔离与旧键盘行为 | 两套通过 | `focused-04/full-keyboard`、`focused-05/original-keyboard` |
| 曲线同步与原登记按键回归 | 通过 | `focused-04/curve-sync/result.log` |
| 完整 ABI 契约 | 完整配置 11,335 项、原始兼容配置 11,347 项通过 | `contract-01/report.json` |
| 隐藏产品启动、工具栏、面板、实时侧栏布局及 EXE 改名 | 两种名称通过 | `product-smoke.log` |

后五项相对证据路径均以 `build/verification/abi-input-20261020/` 为根。真实消息测试覆盖 participant 加入后、prepare 后和 Reserve 内触发取消，也覆盖提交期间的新输入与 IME 组合。取消后无正式值变化、无历史节点、无残留候选，新草稿可在随后 Enter 正常提交。

早期失败证据保留。首轮发现测试描述借用注册表字符串后注销，现改为测试自己持有字符串。另修正测试对 IsDialogMessage 返回值的假设，按真实消息泵消费规则验证焦点保持。TEXT 超过 1 MiB 已被 codec 拒绝，超长只读摘要用合法大 BYTES 验证。未放宽产品权限或失败断言来通过测试。

## 剩余范围

普通生产编辑服务仍关闭。INPUT 的自动属性 DATA、STATE、自动来源订阅、其他挂载点和其他类型的专用编辑器尚未完成。现有 REPAINT 与 LAYOUT 不会触发场景求值、工程写入或新增历史。

IME 验证使用合成窗口消息，微软拼音真机、真实多显示器 DPI、Win7 SP1 x64 和 AMD 验收仍待完成。原生窗口实现和两套构建不替代这些验收。
