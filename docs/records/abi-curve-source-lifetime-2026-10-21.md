# 共享曲线来源回调的会话保护

日期：2026-10-21。分支：`codex/plugin-abi`。

本批统一保护 `Source.read`、`Session.history.query` 和 `Source.localizedLabel`。这些回调在复制、调用或释放闭包时关闭窗口、同窗口替换会话、关闭后重建窗口，旧调用停止处理旧来源。此前的 [motion 文件选择保护](abi-curve-modal-lifetime-2026-10-20.md)继续保留。

## 实现范围

实现位于 [animation_curve_editor.cpp](../../src/window/animation_curve_editor.cpp)。修改不增加公开 C ABI 或新宿主服务。

`Editor.session` 保存 `shared_ptr<Session>` 版本。`EditorLease` 同时保留原会话存储、身份 token 和编辑器分配。即使回调复制构造期间嵌套调用 `Open`，原 `std::function` 所在的会话仍然存活。持有旧存储或旧 token 不延长注册表中的会话有效期。

`InvokeProvider` 在复制闭包前固定原会话版本。复制完成后先核对会话身份和来源索引，失效时不调用旧闭包。回调结束后先释放闭包副本和会话存储引用，再复核身份。复制、调用或释放期间发生异常时，也先复核身份，避免把会话失效当作普通读取失败后继续访问旧状态。

`RetiredContinuation` 使旧调用退出整条内部处理过程。原有读取及刷新中的异常处理不会吞掉该信号。Win32 主窗口、子控件入口及公开 `Open`、`SetLanguage` 在各自边界结束旧调用，失效信号不跨越 Win32 回调。

`WM_NCDESTROY` 撤销窗口注册，但编辑器分配延后到全部调用 lease 退出后释放。lease 先释放会话存储，再减少活动调用计数，因此原会话最后一个闭包析构时仍有编辑器分配保护。`ExternalOperation` 仅在原会话仍有效时恢复保护字段，不恢复新会话的状态。

## 替换与初始化失败

`Open(existing)` 先准备新会话和来源状态，再把旧会话及旧状态移到独立局部对象。新版本安装完成后，释放这些局部对象及替换操作保留的旧存储引用，随后立即复核会话身份。仍有调用 lease 的旧会话继续存活，直到最后一个 lease 释放引用。旧闭包最终析构中的 `CloseOwned` 或嵌套 `Open` 不会重入正在赋值或析构的实时容器。

`OpenAttempt` 只清理本次身份仍有效的初始化失败窗口。旧调用不能关闭嵌套创建的新会话。首次 `WM_CREATE` 同步读取期间若同一个 HWND 已装入新会话，旧创建过程不会通过返回 `-1` 销毁新会话。`CreateWindowExW` 返回后发生普通初始化异常时，本次窗口会关闭，不留下已注册的半初始化窗口。

## 验证

主任务统一构建并运行测试。本记录只核对源码与报告，没有重新构建或运行测试。

完整配置的最终结果见 [curve-final-01/report.json](../../build/verification/abi-panel-background-lifetime-20261021/curve-final-01/report.json)。三项退出码均为 0。

| 范围 | 结果 |
| --- | --- |
| `--source-lifetime` | 2,463 项通过。 |
| `--motion-lifetime` | 278 项通过。 |
| 编辑器全集 | 5,001 项通过。 |

[animation_curve_editor_tests.cpp](../../tests/animation_curve_editor_tests.cpp)包含以下反例：

- 本地来源读取、共同历史来源读取和历史查询在首次创建、通知刷新、父窗口命令、画布快捷键、按钮快捷键及 MMD 数值 EDIT 的 Enter 中关闭或替换窗口。覆盖返回成功、返回失败及抛异常。
- 强持旧 token 后，旧调用仍失效。新会话的状态与通道保持不变，并能继续编辑，旧调用不再触发 `apply`。
- 读取和查询的闭包副本析构时关闭或替换窗口。
- 原会话中读取、历史查询及 `currentFrame` 的最后一个闭包释放时，嵌套关闭或替换窗口。这里验证会话释放过程，不表示 `currentFrame` 调用已统一保护。
- 读取、查询和标签闭包的复制构造中关闭或替换窗口。原函数对象在复制全过程存活，已失效的回调没有执行。覆盖初次创建及复制构造抛异常。
- `SetLanguage` 期间的标签回调失效，以及 `CreateWindowExW` 返回后的普通初始化异常清理。

完整和原始兼容产品构建均通过，退出码为 0。日志分别为 [build-full-02.log](../../build/verification/abi-panel-background-lifetime-20261021/build-full-02.log)和 [build-original-01.log](../../build/verification/abi-panel-background-lifetime-20261021/build-original-01.log)。上述三组曲线运行结果来自完整配置，不能据此增加原始兼容配置的运行通过项。

报告记录继承桌面为 `Default`，`realPrivateDesktop=false`。`SW_HIDE` 只是进程启动请求，窗口显示由夹具控制，不能将整轮称为隐藏窗口或独立桌面隔离验收。桌面环境说明见[本机窗口回归更正](abi-verification-desktops-2026-10-21.md)。本批没有完成真实 IME、多 DPI、硬件或最终产品画面验收。

## 剩余范围

尚未统一到本批复制与会话检查流程的回调包括：

- `Source.apply` 和 `Session.history.traverse`。
- 选择回调 `readKeySelection`、`writeKeySelection`。
- 比较模式回调 `readComparisonMode`、`writeComparisonMode`，以及相邻控制开关 `linkAdjacentControls`。
- 帧与播放控制回调 `Session.currentFrame`、`seek`、`isPlaying`、`setPlaying`。
- `Source.subdivide` 和 `Channel.sample`。

编辑器分配保活不能代替这些回调的参数快照和返回后校验，不能认定全部来源都支持任意重入或窗口自毁。

本批不改变曲线与 VMD 格式兼容规则。普通生产宿主的 `mms.edit` 仍未开放，正式 release 未更新。Win7 SP1 x64、CPU 基础路径、AMD 兼容及程序旁 `data` 的要求保持不变。
