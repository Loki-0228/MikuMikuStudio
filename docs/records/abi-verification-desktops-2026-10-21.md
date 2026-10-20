# 本机窗口回归的桌面环境更正

本轮检查发现，Python 的 `subprocess.STARTUPINFO` 不传递额外赋值的 `lpDesktop`。创建一个桌面，再把该属性交给 `subprocess.run`，不能证明子进程进入了该桌面。

已检查的旧运行器包括 `build/verification/abi-input-state-20261021/run_ui.py`、`run_native.py`，以及 `abi-property-stage-20261020/run_private_desktop.py`。这些运行器中相应调用的结果只证明独立进程内的窗口及功能检查通过；不能继续引用为独立桌面、前台焦点或真实输入验收。不据此否定其他使用真实 Win32 启动接口并核对桌面名称的记录。

本轮改用 `win32process.CreateProcess` 的真实 `STARTUPINFO.lpDesktop` 尝试新建桌面及已有 Codex 隔离桌面。进程在进入测试前发生 `0xC0000142`，受限执行环境的另一轮启动超时；运行器按自身子进程句柄终止并回收，未终止用户程序。原因尚未查明，以上尝试均不计为功能通过。

证据保存在 `build/verification/abi-physics-property-20261021/encoding-original-01` 至 `encoding-original-03` 和 `native-private-02`。第一轮由测试内的桌面名称守卫拒绝；后两轮使用真实 Win32 启动接口。目录内的早期 `realPrivateDesktop` 字段表示启动意图，失败进程没有提供成功的实际桌面证明。

后续运行器报告写入 `realPrivateDesktop=false`，并读取实际继承的桌面名称。`STARTF_USESHOWWINDOW/SW_HIDE` 只是进程启动时的隐藏请求，后续窗口是否显示由各夹具决定。例如刷新夹具会显式调用 `SW_SHOWNOACTIVATE` 显示自己的窗口，不能统称为隐藏窗口测试。

物理窗口专项的 CBT 钩子移除顶层窗口创建时的 `WS_VISIBLE`，将其移至屏幕外并拒绝激活；夹具取得窗口后再隐藏。这不能保证生产入口后续的 `ShowWindow(SW_SHOW)` 从未设置可见样式。编码专项另拦截本进程的 `ShowWindow`、`MessageBoxA/W`，须显式选择 `--hidden-windows-only`，禁止同时用于性能测量或指定独立桌面；默认独立桌面守卫仍保留。上述测试均不发送全局鼠标或键盘输入。

2026-10-21 追加核验中，直接继承已有 `CodexSandboxDesktop` 运行旧属性挂载夹具也在 240 秒后超时，没有测试输出。该结果只说明本轮环境不可用于此夹具，不计为功能通过；运行器只按自身子进程句柄回收。记录位于 `build/verification/abi-panel-background-lifetime-20261021/baseline-ui-sandbox-01/report.json`。

这些结果属于夹具窗口的功能回归，不代替独立桌面隔离、微软拼音、真实前台焦点、多屏 DPI、硬件或最终产品画面验收。新属性区域的生产宿主探针仍强制核对实际隔离桌面，未为绕过环境问题而放宽该要求。
