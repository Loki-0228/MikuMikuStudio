# MMDFacade（MMD 伪装器）

本轮基线为 `abi-cloud-test` 的 `a74e6e1267455125fe7f7e8eed7c71ece1c21b9a`。
产品名及新版宿主导出桥文件名为 **MMDFacade.dll**。它不宣称完整复刻 MMDPlugin ABI 2、3 或 4。

## 两条独立路径

| 路径 | 组件与职责 | 支持边界 |
| --- | --- | --- |
| 旧 MME | `MMDFacade.dll` 转发宿主 37 个 `Exp*` 导出；`MMHack.dll` 提供 22 个旧状态查询及 MME 接入 | 继续使用本地提供的 MME 0.37 x64。原版 MMHack 和内置 MMHack 分别验证，不能外推其他版本 |
| 旧 MMDPlugin | 独立 `MMDPlugin.dll` 提供 `getHWND`、`createWM_APP_ID`；宿主按清单选择回调配置 | `window_observer_v3`、`device_reset_v2`、`effekseer_v2`、`first_person_camera_v4` 四个有限配置。不提供原版内存或通用 D3DX hook |

主 EXE 文件名任意，包括中文。桥通过 `GetModuleHandleW(nullptr)` 找当前宿主。
`MikuMikuDance.exe` 只用于匹配第三方 DLL 的 PE 导入模块名，不用于确认当前 EXE 身份。
旧 MME 的导入副本改写为 `MMDFacade.dll`，源 DLL 不变；导出名称、序号和调用参数不变。
内置 MMHack 仍名为 `MMHack.dll`，22 个导出及序号不变，独立的 MMDPlugin 也不改名。

MMDPlugin 提供者优先从 `<EXE>/PLUGINS/legacy-mmdplugin/MMDPlugin.dll` 加载；仅该路径不存在时使用
`<EXE>/legacy-mmdplugin/MMDPlugin.dll`。首选位置损坏时明确失败，不静默退回旧位置。

## 上游 ABI 核对

依据 [EffekseerForMMD 固定提交的上游头文件](https://github.com/oigami/EffekseerForMMD/blob/c55b2f4e0e26cee473931624f0180eec84c297e2/EffekseerForMMD/MMDPlugin/mmd_plugin.h)。
`third_party/mmdplugin/callbacks.hpp` 只摘录回调类声明并附 MIT 许可，不带固定地址辅助函数。
用户提供的 `D:/MMD-Next/refs/mmd_plugin.h` SHA-256 为
`775fb19fb67442cff91b31914d440e5901e58cdb6dfa7b7213d22a9ce53b53f0`；
剔除注释和空白后，ABI 1–4 共 250 个虚函数签名与摘录一致。参考文件未修改。

| ABI | 工厂 / 销毁 | 生命周期与回调 | 本轮实现 |
| --- | --- | --- | --- |
| 2 | `int version()` 返回 2；`MMDPluginDLL2* create2(IDirect3DDevice9*)`；`void destroy2(MMDPluginDLL2*)` | 继承 ABI 1 的设备调用前回调，并追加相应的调用后回调。没有 ABI 3 的 `start/stop`、窗口、输入接口。`Reset(D3DPRESENT_PARAMETERS*)` 在实际 Reset 前；`PostReset(D3DPRESENT_PARAMETERS*, HRESULT&)` 在返回后，包括失败 | 重置配置和 Effekseer 配置；构造后持有对象，关闭时调用同 DLL 的 destroy2，不调用不存在的 start/stop |
| 3 | `version()==3`；`create3(device)` / `destroy3(object)` | 在 ABI 2 后追加标题、start/stop、窗口观察、消息过滤、鼠标、取消息、键盘，以及可拦截的 WndProc 重载。头文件规定全部对象创建后 start，释放对象前 stop | 保留基线的逐包 create/start、退出逆序 stop/destroy。没有跨插件对象查询的全局启动屏障，因此不声称完整生命周期兼容 |
| 4 | `version()==4`；`create4(device)` / `destroy4(object)` | 启用 `MMDPLUGIN_D3DX9_HOOK` 后继承 ABI 3，追加 `D3DXCreateEffectFromFileExW` 和 `PostD3DXCreateEffectFromFileExW`。设备、文件名、宏、include、flags、pool、效果及错误输出都是引用参数；post 另带 HRESULT 引用 | 仅 FirstPersonCamera 源码适配的工厂、生命周期、键盘及鼠标。两个 D3DX 特效回调和 EffectHook 自由函数未实现；版本仍返回 4 |

ABI 3 的两个 WndProc 不同：`void WndProc(CWPSTRUCT*)` 是 `WH_CALLWNDPROC` 观察；
`std::pair<bool,LRESULT> WndProc(HWND,UINT,WPARAM,LPARAM)` 可以截断宿主处理。
其余接口对应 `WH_MSGFILTER`、`WH_MOUSE`、`WH_GETMESSAGE`、`WH_KEYBOARD`。
当前观察配置只分发归属菜单命令、显示、位置和控件配色消息，不分发这些输入或拦截接口。
不同上游快照还存在 const 参数差异，FirstPersonCamera 头中 MouseProc 没有 override；需要使用对应头文件核对，不能仅按版本号认定覆盖成功。

ABI 2/3 对象由插件自己的 destroyN 销毁，宿主不跨 CRT delete。工厂设备指针借出，插件若保留独立 COM 引用应自行 AddRef/Release。
MSVC x64 的重置槽为 17/136；ABI 3 start/stop/观察槽保持 240/241/243。
测试用上游类声明编译真正的派生对象，独立于宿主的槽号常量验证调用。

## 各插件实际依赖

| 插件 / 固定源码 | 实际依赖及缺口 | 本轮范围 |
| --- | --- | --- |
| [EffekseerForMMD c55b2f4](https://github.com/oigami/EffekseerForMMD/tree/c55b2f4e0e26cee473931624f0180eec84c297e2) | ABI 2；构造 AddRef、析构 Release 设备；BeginScene、EndScene、DrawIndexedPrimitive、Reset/PostReset；设备矩阵；ExpGetFrameTime、模型/排序/ID/文件名、骨骼与表情、当前对象/材质/technic；getHWND；CRT 文件及拖放 API hook；Effekseer 运行库。其 PostReset 未判断 HRESULT 即重建资源 | 真实 v0.25 x64 已接入并验证基础粒子渲染、启停、重置及删除。接入 MMS 原生材质绘制观察接口和显式 .efk 模型导入；不安装旧 CRT/拖放 hook。旧 v0.21 的拒绝记录保留为历史，未据此宣称其现已可用 |
| [FirstPersonCamera f71c300](https://github.com/MikuMikuShaders/FirstPersonCamera/tree/f71c300f31eaa5c0e1543839854056774035f119) | ABI 4 工厂；键盘、鼠标回调；构造即 getMMDMainData，直接写 xyz/rxyz/length 并读 is_camera_mode；GetKeyState、鼠标位置和可见性；D3DX 数学。未覆盖 D3DX 特效回调 | 新增 MMSFirstPersonCamera.dll 源码适配，通过 MMSAcquireLegacyCameraApi 读取/编辑当前姿态。原版 v0.01 DLL 仍在调用工厂前拒绝，明确报告 MMDMainData 不支持；适配版不注册关键帧、不提供撤销事务 |
| [MMDUtility 核心 0bff277](https://github.com/oigami/MMDUtility/tree/0bff27755a5d86dbb8a5be3f264101ee4bf52f86/MMDUtility) | ABI 3，getHWND/createWM_APP_ID，构造创建菜单。MsgProc 处理菜单选择；getDLL3Object 用于其他模块查询 | 核心菜单加载属于已有观察范围；MsgProc 和子插件互查仍不支持。不能把核心可加载扩大成全部子插件可用 |
| [ExamplePlugin 242115c](https://github.com/PTOM76/mmdplugin-ExamplePlugin/tree/242115c99faabf2bc195e1a24a82bd63f5e4718d) | 源码导出 create3/destroy3，但 version 声明及菜单消息需要适配；getHWND、菜单命令、INI 路径 | 基线记录中的 ABI 3 适配版保持原边界；本轮不将上游原样二进制计为已通过 |
| [DispPlayingFrame 64d7305](https://github.com/Soul-chan/MMDPluginDLL/blob/64d73053739dac0dc126a2c712f5adb72bcd2e78/DispPlayingFrame/DispPlayingFrame.cpp) | ABI 3、窗口/子控件、10ms 定时器，stop 清除定时器；原样检查 MMD 9.31 并读取 MMDMainData 的播放字段 | 已有源码适配使用 MMSAcquireLegacyPlaybackMap 的不可变只读快照，不复刻 MMDMainData；本轮未重跑该适配版的帧号控件 |
| [CameraModeUndo / OutsideParentReaderWriter / SetCameraFollowBone / SwapOutsideParent，64d7305](https://github.com/Soul-chan/MMDPluginDLL/tree/64d73053739dac0dc126a2c712f5adb72bcd2e78) | 都是 ABI 3，窗口/消息/输入接口，原版版本检查和 MMDMainData；分别依赖相机历史、外亲读写、相机跟随骨骼、外亲交换。声明回调不代表每个都有非空逻辑 | 原样未支持。需要分别接 MMS 相机、场景、外亲、选择及历史接口；不暴露假结构指针 |
| [AutoSave，0bff277](https://github.com/oigami/MMDUtility/blob/0bff27755a5d86dbb8a5be3f264101ee4bf52f86/AutoSave/AutoSave.cpp) | ABI 3，getDLL3Object、MouseProc 定时触发、CRT hook；从当前 EXE 基址 + 0x750D0 调保存并传 MMDMainData | 未支持。应走 MMS 保存/备份事务接口，不能调用该偏移；线程及 hook 清理还需单独验证 |

旧发布 DLL 的 42 组拒绝测试是[历史记录](records/mmdfacade-real-plugins-2026-11-15.md)。
本次 v0.25、相机适配及真实像素验证见[接入修复记录](records/mmdfacade-integration-2026-11-15.md)。

## 本轮可独立验收的扩展

`kind=legacy_mmdplugin`、`profile=device_reset_v2` 选择 ABI 2，其他字段与窗口观察清单一致。
PE 预检必须找到直接的 version/create2/destroy2；加载后再核对 version 返回 2。
提供者路径、摘要、依赖检查和 `disabledPlugins` 沿用现有规则，未知配置有诊断。

宿主自身 `PostDeviceReset` 在实际设备 Reset 两侧调用该配置的回调，顺序是包启动顺序的 pre、一次真实 Reset、包启动顺序的 post。
每个 post 都看到真实 HRESULT，即使失败也收到通知。该有限配置忽略 post 对 HRESULT 引用的改写，防止把设备丢失伪装为成功；这与完整可改写 ABI 有意不同。
不改变 ABI 3 观察包的分发范围，不对第三方自行调用设备 Reset 的行为做全局 hook。
回调中停止会延迟到这对回调结束；递归重置返回 D3DERR_INVALIDCALL。
C++ 异常包在异常对象销毁后销毁对象并卸载 DLL；stop/destroy 抛异常时固定代码到进程结束。
访问违规等 SEH 故障不属于 C++ 异常隔离承诺。

## Effekseer 与相机接入边界

`effekseer_v2` 保持真实 ABI 2 的 create2/destroy2。BeginScene/EndScene 在宿主场景边界分发；
DrawIndexedPrimitive 的前回调使用 MMS 已有的原生绘制观察接口，在 MME 选好目标及着色器后执行，
仅覆盖宿主模型主材质绘制。回调中的粒子绘制不会再次进入该观察器；不提供通用 PostDrawIndexedPrimitive。
将 COM QueryInterface 返回的底层设备交给 Effekseer，防止其粒子再次被 MME 当作模型材质处理。
独立运行时，从 MMS 的当前模型读取对象序号；该分支只在 Effekseer 回调中生效，保留 MME 的注册映射。

Reset 前释放插件资源，失败时不调用 Effekseer 不检查 HRESULT 的 PostReset，并暂停绘制回调；成功后才恢复。
MME 自身的旧失败重置行为没有改变：人为传入无效参数会弹出 MikuMikuEffect 重置失败提示。
本次验证了无 MME 的失败重置恢复，以及与 MME 同时启用时的成功重置；不宣称修复 MME 内部的失败恢复。

MMS 显式接受 `.efk`：使用同名 `.pmd` 或包内 `efk.pmd` 作为控制模型，项目保留真实 `.efk` 路径。
仅在 Effekseer 回调里，文件名查询提供上游需要的 `.pmd.efk` 别名，并按 Windows ANSI 编码输出。
模型、骨骼、表情和时间读取来自 MMS 字段，不使用原版 EXE 偏移。
文件 hook 请求返回上游的失败值；MMDPlugin.dll 的 binder 导出只管理寿命，不执行任意导入表 hook。
停止时清除其弱引用后再卸载 DLL。通用 RewriteFunction、原版拖放 hook、任意新格式粒子、复杂折射及所有后期效果组合未验证。

`first_person_camera_v4` 必须具有直接的 version/create4/destroy4 和 MMSLegacyCameraAdapterVersion 导出；
加载前拒绝原版 DLL，加载后核对 API 版本。相机 ABI 用定长 C 结构，校验读写版本、姿态和修订号，
只在主线程的该插件回调期间有效。模型模式、播放、跟随相机、录制、场景事务及模态禁用窗口不能编辑。
文本框和 Ctrl/Alt/Win 快捷键保留给宿主；鼠标只接管视口，失焦、取消和停止恢复捕获与光标。
仅支持当前姿态移动；相机历史、关键帧注册、原版 MMDMainData、全局键鼠 hook 和 ABI 4 D3DX 回调未实现。

### 可选插件安装

正式打包命令生成 `build/release/optional-plugins/first-person-camera/`，含 DLL、许可、说明及正确摘要的清单。
退出 MMS 后将该目录完整复制到 `<EXE>/PLUGINS/first-person-camera/`。安装包不会自动启用它。

Effekseer 使用用户提供的 v0.25 x64 包：将原始 `EffekseerForMMD.dll` 和 `efk.pmd` 复制到
`<EXE>/PLUGINS/effekseer/`，在该目录生成清单（不要覆盖原下载文件）：

```powershell
$dll = Join-Path $PWD 'EffekseerForMMD.dll'
@{ schemaVersion=1; id='mms.legacy.effekseer'; name='EffekseerForMMD'; version='0.25.0';
   kind='legacy_mmdplugin'; profile='effekseer_v2'; entry='EffekseerForMMD.dll';
   sha256=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant();
   architecture='x64'; minWindows='6.1.0'; reloadPolicy='restart' } |
   ConvertTo-Json | Set-Content -LiteralPath plugin.json -Encoding utf8
```

重新启动后显式打开 `.efk`，按照上游用法操作控制模型的自动播放、循环等表情。
依赖的模型、纹理随 `.efk` 保持相对路径。旧插件受 ANSI 路径和长度限制，不能从本机中文测试外推所有字符。
停用时在 `PLUGINS/config.json` 的 `disabledPlugins` 中加入对应 id，重启生效。
包摘要、缺失提供者、未知配置及不支持的原版相机均写入启动日志，不用修改当前 EXE 名称绕过检查。

## DLL 改名与安装迁移

1. **导入改写**：原 MikuMikuDance.exe 导入槽改为 MMDFacade.dll，长度仍能放入原槽。37 个导出从原表沿用，签名和 ordinal 1–37 未变；MMHack 的 ordinal 1–22 未变。
2. **加载器**：加载安装目录中的新名，再发布到内容寻址缓存。新 DLL 字节及导入名称使缓存代次变化；插件身份对应的 INI 路径不变。旧缓存不删除、不执行原地修改。
3. **构建**：目标和 .def 改为 MMDFacade；产物 MMDFacade.dll。不再生成 MMSAdapter.dll；MMHack、MMDPlugin 继续独立构建。`cmake --install` 包含两个 MME 桥和独立提供者。
4. **打包**：两个打包入口均仅带新名。旧名遗留的目录会被保护规则拒绝覆盖，应输出到新的发布目录，确认后再切换安装。
5. **安装**：退出 MMS，以同一构建的 EXE、MMDFacade.dll、MMHack.dll 成套更新。只复制或重命名旧 MMSAdapter 不完成迁移；新宿主缺少 MMDFacade 时明确报错。用户源插件包和 data 下偏好不变。旧安装可保留供回退，新加载器不回退加载旧名；检测到旧桥已在进程中加载时拒绝混用。

`MMDPlugin.dll` 仍支持两处安装位置，移动时保持同一构建；不改名、不将第三方原版提供者混入应用提供者目录。

## 验证入口与证据

在独立 x64 Release 构建中执行，临时目录必须可写。首次配置可按现有 Win7 构建文档准备依赖；本轮 Windows 主机验证不代表 Win7 真机验收。

```powershell
cmake --build build/cmake --config Release --target MikuMikuStudio scene_lifecycle_mme_tests legacy_callback_tests legacy_reset_profile_tests legacy_observer_export_tests legacy_adapter_tests legacy_adapter_missing_tests legacy_image_tests
ctest --test-dir build/cmake -C Release -R "legacy_(reset_profile|observer_export_preflight|adapter_|image_contract|mmhack_)" --output-on-failure
python -B tests/plugins/run_mmdfacade_callbacks.py --build build/cmake
python -B tests/plugins/run_legacy_mme_probe.py --originals <MME037目录> --bridge build/cmake/Release/MMHack.dll
python -B tests/plugins/run_mmdfacade_product.py --build build/cmake --utility <固定源码构建的MMDUtility.dll> --mme <MME037目录>
python -B tests/package_abi_release_tests.py -q
python -B tests/abi_release_package_tests.py pwsh scripts/package_abi_release.ps1
```

具体通过/失败及未跑项见本轮[验证记录](records/mmdfacade-2026-11-15.md)。合成插件的回调、真实设备 Reset、真实 MME 像素、产品窗口分别记录，不能互相替代。
