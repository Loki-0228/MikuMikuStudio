# 旧 MME 与特效兼容

[返回功能总览](README.md)

## 当前推进范围

### 旧MME包装剩余验收

适配器/包路径/改名支持已有最新实现记录；旧表里全部未实现已过期。需核实剩余兼容范围而非重建。

验收要求：改名EXE/子目录/移动安装、EMM保存重开、材质/Toon/阴影/离屏/重载；只声明实际已验证版本；未知版本不得冒充支持。

依赖：ABI-01。任务标识：ABI-10。

### 旧插件接口兼容目标

以 `{MMDPLUGIN_REFERENCE_HEADER}` 为只读参考实现 MMS 的 MMDPlugin 兼容层。按插件实际需要核对 `MMDPlugin.dll` 导入、`createN`／`destroyN` 工厂和 D3D9 前后回调的版本、顺序与对象寿命。参考头中依赖原版 EXE 固定偏移的辅助函数单独列为兼容限制。适配工作以公开接口行为为范围，保留插件原有的配套运行时要求，不重制 oigami 的原始发布。对照 [MMDPlugin 0.41 x64 开发者包发布页](https://bowlroll.net/file/135503)。

感谢 oigami（おいがみ）创建 MMDPlugin，为 MMD 插件开发拓展了可能性。其 MMDPlugin 回调接口与舞力介入 P 的 MikuMikuEffect 随包提供的 MMHack 桥接各有独立的加载入口。见 [oigami 的开发自述](https://forum.tcu-ctrl.jp/wp-content/uploads/2017/03/ctrljournal_vol.7_C91.pdf#page=28)、[MMDPlugin 安装器仓库](https://github.com/oigami/MMDPluginInstallManager)及[舞力介入 P 的 MME 安装说明](https://codezine.jp/article/detail/5997?p=2)。

| 插件或接口 | 实际依赖 | MMS 验证状态 |
| --- | --- | --- |
| MikuMikuEffect 0.37 x64 | 原版 `MMEffect.dll` 导入 `MMHack.dll`。可用用户提供的原版 MMHack 或 MMS 内置桥接。 | 已有实际加载、FX 像素变化、对象删除与退出证据，见[桥接验证记录](../records/abi-mmhack-bridge-2026-11-04.md)。 |
| MMDUtility 核心，ABI 3，源码 `0bff277` | `MMDPlugin.dll` 的 `getHWND` 和 `createWM_APP_ID`。 | 原样源码构建在 MMS 产品中创建自己的菜单，与另外两个观察包同时加载并正常退出。此行覆盖核心 DLL，子插件和相互查询对象接口另行验证。见[固定版本的源码](https://github.com/oigami/MMDUtility/tree/0bff27755a5d86dbb8a5be3f264101ee4bf52f86/MMDUtility)。 |
| ExamplePlugin，源码适配 `242115c` | 同一提供者的两个导出，以及归属于该包的 `WM_COMMAND`。 | 适配了 ABI 3 版本声明、菜单消息和 INI 路径的所有权。实测菜单勾选开关、其他包同时运行及退出。保留版本 1 的对照 DLL 被拒绝，有效包继续启动。见[固定版本的源码](https://github.com/PTOM76/mmdplugin-ExamplePlugin/tree/242115c99faabf2bc195e1a24a82bd63f5e4718d)。 |
| DispPlayingFrame，ABI 3，源码适配 `64d7305` | 兼容提供者的 `getHWND` 和 MMS 宿主的 `MMSAcquireLegacyPlaybackMap`。 | 隔离 Release 程序自动发现并启用包。控件随相机距离控件移动，覆盖负帧跳转、播放推进、停播跳帧及退出。和真实 MME 0.37 x64 共存时，Release 离屏回归实测显示 `-18 → 124 → 125 → 42` 并正常退出。见[固定版本的上游源码](https://github.com/Soul-chan/MMDPluginDLL/blob/64d73053739dac0dc126a2c712f5adb72bcd2e78/DispPlayingFrame/DispPlayingFrame.cpp)。 |
| EffekseerForMMD，源码 `c55b2f4` | 使用 ABI 2 工厂及 D3D9 设备重置回调。 | 本版窗口观察模式仅支持 ABI 3。此候选已核对源码，实际绘制和设备重置仍待验证。见[固定版本的项目源码](https://github.com/oigami/EffekseerForMMD/tree/c55b2f4e0e26cee473931624f0180eec84c297e2)。 |
| FirstPersonCamera，源码 `f71c300` | 使用 ABI 4 工厂和鼠标回调。 | 已核对源码。此候选需要单独适配和实测。 |
| MMDDiscordRPC，源码 `61ce9b9` | 使用 ABI 3 工厂，另加载 Discord SDK、安装线程定时器并替换主窗窗口过程。该提交未找到 `destroy3` 实现。 | 已核对源码。此候选需要明确移除定时器和还原窗口过程的生命周期，尚未执行。 |
| CameraModeUndo、OutsideParentReaderWriter、SetCameraFollowBone、SwapOutsideParent，源码 `64d7305` | 原样代码读取原版 `MMDMainData`，并检查 MMD 版本。 | 已核对源码依赖，核心操作需要映射到 MMS 的相机、外亲或历史接口，尚无原样运行通过的证据。见[固定版本的项目源码](https://github.com/Soul-chan/MMDPluginDLL/tree/64d73053739dac0dc126a2c712f5adb72bcd2e78)。 |
| MMS 原生插件 | `MMSPluginGetApi` 和 MMS 宿主函数表。 | 沿用[原生 SDK 的接口与验收](../../sdk/mms/README.md)。 |

三个已验证的窗口观察包与 MME 0.37 x64 同时加载时，菜单操作正常，包按启动的逆序停止和销毁。另用真实 D3D9 探针分别对比内置与原版 MMHack，设备重置前后保留同一设备和视口，FX 像素对照一致。设备重置对照覆盖观察包与 MME 共存，其他旧插件的图形回调需要各自验收。MMS 原生 ABI 和旧插件导出检查均通过独立 CTest。

DispPlayingFrame 的上游版本检查 MMD 9.31 并读取 `MMDMainData`。源码适配版通过映射入口识别宿主能力并读取播放帧。oigami 的 AutoSave 还直接调用原版宿主固定偏移处的保存函数，其保存操作需要另行适配。见 [AutoSave 源码](https://github.com/oigami/MMDUtility/blob/0bff27755a5d86dbb8a5be3f264101ee4bf52f86/AutoSave/AutoSave.cpp)。

本地 MMD 9.32 x64 的 `ExpGetFrameTime` 静态核对显示，主数据指针槽仍位于 EXE 的 RVA `0x1445F8`。它读取的播放标志、停播帧号和播放秒数偏移与样本 9.31 头文件相同，但宿主对播放标志只读取一个字节，样本声明为四字节整数。旧样本读取版本号的 RVA `0x132EA8` 也仍有效，读到的值为 `9.32`，会被上游的 `== 9.31` 检查拒绝。此核对仅覆盖版本读取及播放字段。MMS 适配版继续从宿主自身的状态访问器填充公开映射，MME 的内置桥接通过命名导出取得帧时间。

### MMS 窗口观察插件包

适配插件可将自编译的 x64 DLL 和 `plugin.json` 放在 MMS 安装目录的 `PLUGINS/<包目录>/`。MMS 发行构建的 `legacy-mmdplugin/MMDPlugin.dll` 提供 `getHWND` 和 `createWM_APP_ID`，插件可链接该 DLL 的导入库。安装时保留提供者在原目录。其他发布版插件所需的原版 MMDPlugin 运行时依各自文档配置。MMDPlugin 与 MME 所需的 MMHack 分别配置。

```json
{
  "schemaVersion": 1,
  "id": "example.dispplayingframe",
  "name": "DispPlayingFrame",
  "version": "1.0.0",
  "kind": "legacy_mmdplugin",
  "profile": "window_observer_v3",
  "entry": "DispPlayingFrame.dll",
  "sha256": "<替换为实际 DLL 的 SHA-256，共 64 个小写十六进制字符>",
  "architecture": "x64",
  "minWindows": "6.1.7601",
  "reloadPolicy": "restart"
}
```

使用 `Get-FileHash -Algorithm SHA256` 取得 DLL 摘要并转为小写。宿主按解析后的包顺序逐个启动，每个包独立处理加载错误，同一 DLL 只启用一次。将包 ID 加入 `PLUGINS/config.json` 的 `disabledPlugins` 并重启即可停用。宿主加载前检查 x64、摘要和依赖，允许 MMS 提供者及系统依赖。清单代表使用者选择运行该 DLL，SHA-256 用于确认文件与清单相符。DLL 在 MMS 进程内执行，应采用可信源码及构建产物。

窗口与设备就绪后，宿主依次调用 `version`、`create3`、`start`。传给 `create3` 的 D3D9 设备是宿主借出的指针。随后在编辑器线程分发主窗口、浮动窗口及子控件的显示、位置变化和控件配色观察回调 `WndProc(CWPSTRUCT*)`。主窗口的菜单 `WM_COMMAND` 只分发给分配该 ID 的包。包加载、工厂、启动或观察回调中可调用 `createWM_APP_ID`，该 ID 在本次宿主会话内唯一。退出时先停止回调，再逆序调用 `stop` 和各 DLL 自己的 `destroy3`，清理对应菜单后卸载 DLL。样本在 `stop` 释放定时器、自建控件和字体。输入拦截、D3D 绘制或设备重置回调、`getDLL3Object` 和原版宿主内存布局需要单独适配与验收。

窗口观察包的入口 DLL 必须直接导出 `version`、`create3` 和 `destroy3`。宿主先读取 PE 导出表，再加载 DLL 并调用 `version()` 核对返回值。缺失或转发工厂导出的两个合成包均在 `DllMain` 执行前被拒绝，同目录中的三个有效插件仍正常工作。参见 [Microsoft PE 导出表说明](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#export-address-table)。

仓库中的 `legacy_observer_export_preflight` CTest 用例覆盖直接导出、缺失导出和转发导出。构建目标 `legacy_observer_export_tests` 后即可运行，测试程序和合成 DLL 随目标编译并输出到构建目录的 `tests/` 下，无需第三方插件或 GPU。

复现样本时，使用表中固定提交的 DispPlayingFrame 源码。将原来的 MMD 版本检查替换为映射导出的能力检查，将定时器的数据读取改为下方的只读快照。在 `start` 创建显示控件，保留窗口位置观察回调，并在 `stop` 停止定时器后销毁控件。用 x64 MSVC 构建，包含 MMS 的 `sdk/mms/include`，并使用本构建生成的 `MMDPlugin.lib` 解析 `getHWND`。测试所用的样本 DLL、宿主和提供者的摘要均保存在隔离验收记录中。

样本准备脚本（历史测试脚本，未随当前分支发布）从本地上游仓库读取固定提交，只向新建目录写入适配源码和原 MIT 许可。以下示例从 MMS 根目录运行，使用已经准备好的 x64 MSVC 环境与 Release 构建。替换上游仓库路径及构建目录。

```powershell
$mms = (Get-Location).Path
$sample = Join-Path $mms 'build/legacy-frame-sample'
python tests/plugins/prepare_legacy_dispplayingframe.py '{LEGACY_PLUGIN_PATH}' $sample
Copy-Item 'build/cmake/lib/Release/MMDPlugin.lib' "$sample/MMDPlugin/MMDPlugin.lib"
Push-Location $sample
cl /nologo /EHsc /std:c++17 /LD /MT /O2 /utf-8 /DUNICODE /D_UNICODE /I"$mms/sdk/mms/include" DispPlayingFrame/DispPlayingFrame.cpp /link /out:DispPlayingFrame.dll user32.lib gdi32.lib
Pop-Location
```

按此流程重建的 DLL 已在隐藏 Win32 控件上实测。探针写入停播帧、播放游标及停止后的帧号，控件依次显示 `-18 → 123 → 42`。停用后控件销毁，DLL 卸载。该探针使用真实映射与生产加载代码，未创建 D3D9 设备。

同一重建 DLL 在隔离 Release 正常入口完成离屏播放，实测显示 `-18 → 125 → 126 → 42`。用同一宿主和样本分别停用、填写错误的 SHA-256、移除兼容提供者时，均未加载样本，程序仍正常退出。

用仓库中的 离屏播放回归脚本（历史测试脚本，未随当前分支发布） 复跑正向用例。等其他构建和验证进程结束后，从 MMS 根目录执行。普通 MMS 会话可以留在独立安装目录。脚本将宿主、提供者和样本复制到新的输出目录，每个 Windows 会话同时只运行一个该脚本的用例。输出目录保留帧号与退出结果 `result.json` 和日志 `runtime.log`。

```powershell
python -B tests/plugins/run_legacy_observer_smoke.py `
    --exe 'build/cmake/Release/MikuMikuDance.exe' `
    --provider 'build/cmake/Release/legacy-mmdplugin/MMDPlugin.dll' `
    --sample "$sample/DispPlayingFrame.dll" `
    --output 'build/legacy-observer-smoke'
```

排查异常退出时，请保留这两个文件。`result.json` 记录清理前的退出码、关闭或终止请求、Windows 进程创建和退出时间。时间以 Windows FILETIME 的 100 纳秒计数记录，写为字符串以保留整数精度。可以结合 `runtime.log` 的宿主关闭标记，确认退出发生在测试清理之前还是之后。

需要排查包被拒绝的原因时，可在启动前将 `MIKUMIKUSTUDIO_TRACE_FILE` 设为可写日志文件，查看 `LEGACY_MMD` 记录。未放置有效清单的 DLL 不会作为窗口观察包启动。

同一可选日志还记录 `SHUTDOWN begin`、`WM_DESTROY begin` 和 `NORMAL_EXIT code=...`，可与进程退出码和插件停止记录对照。启用或停用 DispPlayingFrame 的隔离用例都按此顺序记录并以 0 退出。启用用例在窗口销毁开始后记录插件停止。

### 公开播放状态映射

优先面向可从源码适配的插件。通过 `GetProcAddress` 从宿主 EXE 查询 `MMSAcquireLegacyPlaybackMap`，在编辑器线程请求版本 1。返回的是只读 Windows 文件映射句柄。记录含字节长度、版本、播放标志、有符号时间轴帧、快照序号和时间轴秒数，使用固定宽度字段并且不含宿主指针。见 [C 接口头文件](../../sdk/mms/include/mms/legacy_playback_map.h)。

一次请求获得一份不可变快照。消费者用 `FILE_MAP_READ` 映射后验证版本和长度，用完解除映射并关闭句柄。需要新状态时重新请求。停播读取有符号编辑帧，播放读取宿主的实际播放游标，以 30 帧/秒换算为帧。请求失败返回空句柄。启动前、退出后、非编辑器线程和工程恢复隔离期均返回 `ERROR_NOT_READY`，未知版本返回 `ERROR_REVISION_MISMATCH`。

隐藏编辑器验证了输入负帧、开始播放、停止后重新跳帧、拒绝写映射，以及保留快照跨编辑器退出仍可读。早期使用独立测试提供者验证只读映射及控件移除。随后的隔离 Release 验证使用正式清单和提供者，确认自动加载、位置观察回调及退出。停用清单、摘要不匹配、缺少提供者和不支持的导入均实测保留编辑器的正常退出。完全隐藏的窗口会暂停播放更新，连续播放用显示器范围之外且未激活的窗口验证。

关闭效果预编译的隔离构建在默认效果初始化时超时。两组阶段日志均显示 MME 加载和 D3D9 设备创建已返回，停点在 MME 所需的同步效果创建。以既有完整构建的三个预编译效果资源重链同一 Release 核心后，受保护的隐藏场景探针通过仅 MME 和双插件对照。两组真实 MME 0.37 x64 的特效开启、关闭和恢复均读回 `286146 → 0 → 286146` 个绿色像素。双插件用例中，真实播放命令和受控时钟推进使 DispPlayingFrame 从 123 更新到 153，停播跳转后显示 42，关闭时控件、插件和提供者均已释放。另用相同资源重链正常产品入口，双插件用例记录了真实播放循环的 126 到 127 帧和正常退出。绘制像素来自合成场景探针，正常产品测试使用未激活的离屏窗口。

新版 Release 的原生 `plugin_abi_contract` 通过。暂存插件侧车被外部替换的用例已核对回滚、保留异常内容及恢复日志，以及移除异常测试文件后的恢复重试。

当前工作树也在新目录完成了全量 x64 Release 构建，从源码生成预编译效果资源。用该构建的导入库重建 DispPlayingFrame 后，普通离屏播放与真实 MME 0.37 x64 共存的正式入口测试均通过。同一构建中的 `plugin_abi_contract` 和 `legacy_observer_export_preflight` 也通过。

使用全量构建的核心、提供者和 MME 适配器再次运行 D3D9 重置探针。内置桥接与原版 MMHack 各跑仅 MME、加三个窗口观察插件两组，四组在重置前后的效果启停读回均为 `126087 → 169 → 126087` 个绿色像素，均正常退出。含插件的两组在重置后继续读到 `-9` 和 `42`，关闭时释放插件和提供者。

仅启用观察插件的隐藏 Release 基线通过。换用预编译资源的无 MME 对照有两次正常完成离屏播放及退出。另两次在沙箱外等待帧号控件时超时，最终记录到退出码 `0xFFFFFFFF`。两份记录均无脚本强制终止标记，也缺少清理前的进程状态或插件初始化日志，无法区分等待期间退出和收到清理关闭请求后退出。新版产品通过 MMDUtility 单独加载，以及三个窗口观察包同时加载的测试。与真实 MME 0.37 x64 同时加载时，ExamplePlugin 的勾选和 DispPlayingFrame 的负帧读回均正常，程序以退出码 0 结束。

带启动阶段日志的冷启动对照中，空插件与多插件各两次均正常退出。主窗控件就绪后，自动蒙皮校准还用时约 5.1–6.0 秒，随后才开始观察包加载。后续测试会等待插件就绪并记录测试清理前的进程状态。历史的 `0xFFFFFFFF` 退出原因仍未确认。

合成 ABI 3 插件在 `start` 和菜单回调内同步请求关闭时，旧兼容层会在回调返回前销毁窗口。兼容层现沿用 MMS 的关闭延迟机制。隐藏 Win32 探针通过两条路径，均在回调返回后回放一次关闭请求，再停止和卸载插件。同一版加载器也通过 MMDUtility、ExamplePlugin 和 DispPlayingFrame 的隐藏窗口回归，覆盖独立菜单、勾选开关、帧读回和退出清理。新版 Release 的正常产品入口同时加载三个插件和真实 MME 0.37 x64，勾选开关、负帧读回和正常退出均通过。

启用包标识日志后，用正常产品入口实测启动顺序为 ExamplePlugin、DispPlayingFrame、MMDUtility。关闭时按相反顺序调用 `stop` 和 `destroy3`，三个包均完成对象销毁。

使用同一版加载器的 D3D9 探针串行对照仅启用 MME 与 MME 加上述三个插件。相同合成场景在开启、关闭和恢复效果时，两组均读回 `286146 → 0 → 286146` 个绿色像素。两组都完成场景对象删除并以退出码 0 结束，三插件组还验证了帧号随播放推进及窗口关闭时卸载插件和提供者。

设备重置对照先按编辑器布局刷新视口，再调用实际的 `PostDeviceReset`。仅启用 MME 与加装三个观察插件两组均保留原 D3D9 设备对象，重置前后的视口均为 `(313,25)–(1393,633)`。两组在重置前后均读回 `126087 → 169 → 126087` 个绿色像素。三插件组的原有帧号控件在重置后仍能更新至 `-9` 和 `42`，随后正常卸载。这项证据覆盖原设备上的重置及上述插件的窗口观察行为。

上述重置对照已用与最新隔离 Release 相同的旧插件加载和 PE 检查对象文件补跑，结果一致。

MME 清单显式指定随原包提供的 `MMHack.dll` 后，使用同一探针再次测试，仅 MME 与加三个旧插件两组均通过效果启停、设备重置和退出。两组的绿色像素读回与内置桥接用例一致。独立核对加载的缓存 DLL，确认其字节等于原版 DLL 仅改写一处宿主导入名称后的结果，原始文件摘要一致。

使用发生过异常的同一二进制延长就绪等待后，离屏播放实测显示 `-18 → 125 → 126 → 42`，退出码为 0。将观测故意缩短为两秒时，探针在自动蒙皮校准期间超时，进程仍存活并在关闭请求后以 0 退出。短等待能造成就绪误判，历史退出码尚未复现。

另用同一历史二进制和样本复跑八秒就绪对照，帧号控件在主窗就绪后约 4.8 秒出现，离屏播放和退出通过。故意缩短至一秒时，旧脚本只等待四秒就强制终止仍存活的进程，退出码为 1。清理等待改为十五秒后，已排队的关闭请求完成，插件正常停止并卸载，进程以 0 退出。脚本分别记录就绪等待、清理前状态和发送关闭后的退出码。

另一次八秒对照在同一二进制上超时。Windows 进程时间显示，收到清理关闭请求约 4 秒后程序以 0 退出，日志显示插件在此期间启动并正常停止。随后串行对照中，明确选择 CPU 顶点模式时控件在主窗就绪后约 0.37 秒出现，自动模式约 10.73 秒。两次均通过离屏播放与退出。这组证据解释了八秒就绪检查可能超时，历史 `0xFFFFFFFF` 的退出来源仍未确定。

带阶段日志的同一隔离构建完成冷、热启动对照。冷启动的自动蒙皮阶段用时 19.114 秒，观察插件启动阶段用时 0.070 秒。热启动只复用了该冷启动生成的测试缓存，对应阶段用时 0.525 秒和 0.073 秒。两次均完成离屏播放并正常退出。另一次刻意缩短等待的冷启动在蒙皮校准内超时，测试脚本最终结束自有子进程，退出码为 1。

独立隐藏窗口的生命周期探针使用相同生产加载代码，验证同一 DLL 的第二个包被拒绝。关闭插件时窗口仍存活，两个插件菜单已移除，宿主自己的菜单仍在，样本和提供者 DLL 均已卸载。该探针没有创建设备，产品窗口及绘制结果分别以上述对照为准。

## 相关专题

- [旧 MME 插件包与兼容范围](../LEGACY_MME_PACKAGES.md)
- [MME 分配快照的安装目录隔离](../records/abi-mme-data-2026-10-23.md)
- [MME 相对效果与偏好持续性](../records/abi-mme-relative-and-settings-2026-10-30.md)
- [legacy MME 改名与子目录检测](../records/abi-legacy-mme-detection-2026-10-24.md)
- [普通骨骼整体重置与草稿取消](../records/abi-legacy-pose-reset-2026-10-23.md)

## 实现与验收依据

以下保留原总览中的实现、失败和限制记录；其中日期、版本和“当前”均属于原记录上下文，不能替代上方的当前推进范围。旧执行方式与任务指令不适用于本轮工作。

尚无从旧总览归入本页的记录。
