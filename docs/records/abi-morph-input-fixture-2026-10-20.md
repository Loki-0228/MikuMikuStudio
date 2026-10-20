# 表情输入测试夹具修正

日期：2026-10-20。分支为 `codex/plugin-abi`。本批仅修测试前置条件，没有修改产品的 EDIT 窗口过程。

## 根因与修改

完整原生历史验收停在 `morph Enter starts with one original ANSI EDIT on the current thread`。旧测试程序也能复现。隔离隐藏 Win32 检查确认，`CreateWindowExA` 创建的系统 EDIT 在本机仍可能使用 Unicode 类过程，`GetWindowLongPtrA` 返回 ANSI 转换 cookie；创建函数带 A 后缀不保证当前窗口过程已完成 ANSI 子类化。

产品在创建控件后调用 `InstallControlSubclass`，测试 `MorphSliderFixture` 漏掉了这一步。现对四个表情输入框 506／511／516／521 调用同一个产品函数，并验证编码、实际窗口过程和底层 cookie。焦点观察器保留产品 `EditSubclassProc`，每条消息只转发一次。`OriginalEditProc` 继续指向底层原生过程，不改成产品过程自身，避免递归。

原有 ANSI、真实 Enter、恰好一次 `WM_KILLFOCUS`、主窗口焦点、修改标志、参数值、历史步数和 redo 断言均保留。

该单文件代码任务通过已安装的 `dsh-delegate` 派发至真实 DSH 宿主，session 为 `fb6afdb9-b0da-4d24-adc7-b733c7223a86`，实际 cwd 为 `D:/MMD-Next/MMS-abi-dev`。正式结果为 completed，DSH 明确未构建或运行测试。Codex 独立检查 diff、构建并完成下述验收；看图和文档没有交给 DSH。

## 验证与限制

证据目录为 `build/verification/abi-wave-formats-20261020/`。

- `test-morph-fixture.log`：真实表情滑杆和输入专项通过 14,266 项检查。
- `test-native-all.log`：完整历史套件已通过该旧断言及后续多个测试组，但停在 `NativeCameraHudHistoryChecks` 的真实鼠标坐标断言。日志记录 `SetCursorPos` 的结果 `positioned=0`，实际坐标不等于目标坐标。保留断言，未将本轮登记为全套通过。
- `dsh-morph-fixture-result.json`：正式子任务结果，包含修改范围与未执行测试的说明。

此修正恢复了测试夹具与真实产品的安装顺序，不能据此推定全部输入法、多屏 DPI 或目标平台已验收。
