# MMDFacade 原版相机闭环验证

> 后续启停泄漏修复、MME 独立像素、设备恢复及 DX12 回退结果见[扩展验证记录](mmdfacade-extended-validation-2026-11-15.md)。

基线：本地 `abi` 的 `19fb0255efdfd4330bf9be8f9fee35f84a8f9585`。
独立分支 `feature/mmdfacade-original-camera`，源码在 `build/facade-original-src`，Release 在 `build/facade-original-build/Release`。未推送、未合并 abi、未覆盖使用中的安装目录。

## 本轮结论与边界

| 对象 | 类型 | 本轮结论 |
| --- | --- | --- |
| FirstPersonCamera v0.01 x64 | 未修改的原版发布 DLL | 原版输入与内存测试、产品窗口消息路由、与 MME/Effekseer 共存通过；仅支持下面固定摘要对应的布局 |
| EffekseerForMMD v0.25 x64 | 未重新编译的原版 DLL | `.efk` 导入、真实粒子像素、Reset、停止、重启、模型删除回归通过；导入改写限缓存 |
| MME 0.37 x64 | 未重新编译的原版 DLL | 启动、与上述两插件共存渲染和设备重置通过；本轮未重新执行独立 MME FX 绿色像素对照 |
| MMDUtility ABI 3 窗口观察核心 | 固定上游源码构建的观察样本 | 9 组产品菜单出现、窗口响应、正常退出通过；不称为官方发布 DLL 或完整 MMDUtility 功能通过 |
| MMSFirstPersonCamera | 源码适配版 | 单独的 9 组输入测试通过；不计入原版验证。默认需显式启用，避免与原版争用 |
| 其他 ABI 2/3/4 DLL、其他版本 MMDMainData | 未支持/未验证 | 不根据版本号放行；未知原版 ABI 4 在工厂前拒绝并弹错 |

本轮验证使用 Windows x64 Release、真实 D3D9 设备。窗口测试通过实际窗口过程发送 Win32 键鼠消息，未做人工按键或物理 `SendInput` 验收。DX12/D3D9On12、切屏引发的真实设备丢失、长时间交互以及其他 GPU/系统版本未验证。不存在“完整兼容”结论。

## 可追溯输入

| 输入 | SHA-256 / 固定提交 |
| --- | --- |
| FirstPersonCamera 原版 DLL | `d3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef` |
| FirstPersonCamera 源码 | [f71c300f31eaa5c0e1543839854056774035f119](https://github.com/MikuMikuShaders/FirstPersonCamera/tree/f71c300f31eaa5c0e1543839854056774035f119) |
| Effekseer v0.25 x64 DLL | `2a7cf3d9afce099228168a1529589f48b5a232befa24cbaa4ab363756c11f532` |
| MMEffect 0.37 x64 DLL | `74b3f5882a3bad28131cbc57994e2f960938e6ca850fc7139018ce4822bd87dc` |
| MMDUtility 核心样本 | `c778de083c77a6850b56f98ac8f056d0e8c104c833f24aea420d4715a87612c5`；上游 `0bff27755a5d86dbb8a5be3f264101ee4bf52f86` |
| 用户参考头文件 | `D:/MMD-Next/refs/mmd_plugin.h`，`775fb19fb67442cff91b31914d440e5901e58cdb6dfa7b7213d22a9ce53b53f0` |

原版相机 README 的运行环境是 MMD 9.26 x64 / MMDPlugin 0.41。上游仓库的源码与发布 DLL 均保存在 `build/validation/facade-original/upstream/FirstPersonCamera`。REA 原始导入导出证据在 `firstperson-rea-evidence.json`；其配置的 Ghidra/JDK 路径不存在，反编译不可用，未将失败当作分析成功。实际指令与虚表核对使用本地 PE 文件、pefile 和 Capstone，记录在同目录的 `firstperson-user-code.asm`。

## ABI 与双向转发

完整的 250 项槽位、声明及当前转发状态见 [ABI 槽位表](mmdfacade-abi-slots-2026-11-15.csv)。这是 MSVC AMD64 布局；x86 调用约定、其他 C++ 编译器布局不在支持范围。基础类仅含 8 字节虚表指针，对齐 8。原版相机实际对象分配 0x20 字节：虚表在 +0，锁定标志 +8，持有的数据指针 +0x18。

| 层级 | 工厂/销毁、槽位 | 当前实现 |
| --- | --- | --- |
| ABI 2 | `version`、`create2(IDirect3DDevice9*)`、`destroy2(object*)`；0 析构、1–119 D3D9 前回调、120–238 后回调 | 仅已审核的重置和 Effekseer 路径；无 start/stop |
| ABI 3 | `create3/destroy3`；239 标题、240 start、241 stop、242 拦截式 WndProc、243 观察式 WndProc、244 MsgProc、245 MouseProc、246 GetMsgProc、247 KeyBoardProc | 观察包仅分发 243 的指定窗口消息。244、246、242 未接通；标题不参与加载判定 |
| ABI 4 | `create4/destroy4`；继承 ABI 3；248/249 D3DXCreateEffectFromFileExW 前/后回调 | 相机使用 245/247；248/249 特效编译回调尚未转发，不能因相机成功宣称完整 ABI 4 |

工厂是 `extern "C"` 的 `__cdecl` 声明，在 AMD64 使用统一 Windows x64 调用约定；虚函数另有 this 参数。宿主使用各 DLL 自己的 `destroyN`，不跨模块 delete。MSVC 会把两个 WndProc 重载排在一起，不能把源码声明顺序直接当作槽位顺序。拦截式回调返回 `std::pair<bool,LRESULT>`，未通过猜测返回 ABI 来启用它。D3DX 两回调参数包含引用，后回调另有 `HRESULT&`，未用普通指针签名代替。

生命周期现为先完成所有工厂，再调用 ABI 3/4 start；退出先停止事件分发，再逆序 stop、destroy、释放视图、卸载模块。`MMDPlugin.dll` 提供者向 MMS 的明确宿主导出转发窗口、菜单、模型及相机接口；MMDFacade/MMHack 保持原有导出签名与序号，本轮未修改其导出源。

实际依赖：Effekseer 使用帧/模型导出、hook binder、BeginScene/EndScene、首材质 DrawIndexedPrimitive 观察和 Reset/PostReset；其失败 Reset 后不能直接调用会无条件重建资源的 PostReset。MME 使用 MMHack 设备桥与 MMDFacade 的旧 EXE 导出。MMDUtility 核心使用窗口/菜单导出和 start/stop；其 MsgProc 等额外交互未完整转发。相机原版仅导入 KERNEL32、USER32、d3dx9_43，使用 D3DX 旋转矩阵函数、系统键鼠状态与内联的主数据获取器。

原版 MMDPlugin 的完整宿主 hook 循环实现未找到可核验的对应源码；本轮有参考头文件、消费者源码和实际 DLL 指令证据。对未取得证据的回调顺序/传播语义保留缺口，没有从新头文件推断完整原版宿主布局。

## 原版相机内存视图

原版 DLL 内联 getter 在 RVA `0x1060`，调用 `GetModuleHandleW(NULL)` 后读取模块基址加 `0x1445F8` 的指针槽。它并不导入独立的 `getMMDMainData` 导出，因此单纯转发原版 MMDPlugin.dll 无法满足它。

原文件和缓存文件的字节摘要一致。兼容层仅修改**已加载缓存模块**的 GetModuleHandleW IAT 项（RVA `0xF018`），仅对已核实的返回地址 `module+0x109D`、NULL 参数、创建线程提供专用地址锚点；其他调用原样进入系统函数。绑定前同时检查完整 DLL 摘要和调用点机器码。锚点不是 MMS 的 EXE，也不会按当前 EXE 文件名选取，主 EXE 可任意命名。

| 字段 | 偏移/尺寸 | 支持方式 |
| --- | --- | --- |
| 相机模式 | `0x328`，1 字节 | MMS 实际模式的只读镜像；写入不接受 |
| 旋转 x/y/z | `0x348/0x34C/0x350`，各 float32 | 回调前读取 MMS，回调后通过 CameraEdit 写回 |
| 位置 x/y/z | `0x36C/0x370/0x374`，各 float32 | 同上 |
| 距离 | `0xA18F8`，float32 | 同上；按原版鼠标操作归零并转换位置 |
| 数据指针槽 | 锚点 `+0x1445F8`，8 字节 | 指向独立稳定视图，槽位页只读 |

已支持字段视图尺寸 `0xA18FC`，对齐 4，指针宽度 8；保留整个地址范围，仅提交包含已用字段的两个页。中间未支持页不可访问。视图不是完整 MMDMainData；已提交页内的其他字节也不构成对外支持字段。参考新头文件的距离偏移是 `0xA1900`，不能直接套用；更老 MMDUtility 头的旋转偏移也不同。

同步只发生在该原版对象创建前和 UI 输入事件边界，仅复制 7 个浮点数与模式。插件可以在对象存活期间保留指针；不能跨线程读取、异步写入或在 destroy 返回后使用。回调前记录当前帧、修订号和相机状态，写回时核对宿主状态未改变，拒绝过期、非有限值、超界值及不允许编辑的状态，随后恢复实际宿主状态。若发生冲突，以宿主状态为准，并写入拒绝日志。不存在每帧目录扫描、PE 分析或大型主数据复制。

原版没有失焦取消和析构清理，故在失焦、模式切换、菜单、停止时，先以最后有效模式合成 WM_LBUTTONUP，解除原版自身鼠标锁和隐藏光标，再同步新模式。销毁抛异常时保留模块和视图到进程退出，避免悬挂地址。

原版 v0.01 的代码对 `GetKeyState(VK_SHIFT)>0` 作判断，且不排除 key-up；实测在键盘状态为零时 W 的步长是 4，松开又移动 4。保留该原版行为，未用适配版“松开不移动”的行为替换后宣称一致。

## 安装与失败处理

按用户最后确认：**旧 DLL 自动识别只在 `PLUGINS/LEGACY` 内递归进行**。LEGACY 内可自由分组，深度从 LEGACY 根计数，受 `config.json` 的 maxDepth 控制；其他目录不会自动试探原版 DLL。既有明确清单仍按已有安装约定兼容。普通依赖仅静态检查 PE、缓存阴性结果，不通过加载或调用导出来猜接口。

已核实 DLL 的摘要、接口与兼容能力内置于宿主，用户无需手写 kind/profile/SHA。`data/plugins/legacy-discovery.json` 保存文件标识、大小、修改/变更时间与识别结果；仅变化文件重查。新原版相机自动 ID 为 `org.mikumikustudio.legacy.original-first-person-camera`，可加入 disabledPlugins 禁用并重启。原生插件仍沿用其插件作者提供的原生清单。

组件优先兼容既有固定路径；缺少这些路径时，允许从 PLUGINS 内唯一解析组件。非标准路径出现多份同名组件则拒绝。提供者在加载前必须具有 MMS 的明确兼容导出，不能把原版 MMDPlugin.dll 直接当成提供者。加载器继续检查已经映射的同名 DLL 的路径/内容，禁止悄悄绑定另一份。`MikuMikuDance.exe` 只用于旧导入表匹配，不用于检查当前 EXE。

加载失败会显示 MMDFacade 错误框，包含插件/文件与原因；主动禁用不报错。缺失组件、错误的同名组件、被独占的 DLL、未知原版版本、重复插件、歧义组件均在隔离测试中实际验证。识别错误有独立类别，不靠错误字符串筛选来决定是否弹框。

## 验证记录

所有路径相对于 `D:/MMD-Next/MMS-ABI-integration/build`。

| 检查 | 结果与证据 |
| --- | --- |
| 原版失败用例 | `validation/facade-original/red.log`：基线在工厂前因缺少 MMSLegacyCameraAdapterVersion 拒绝 |
| Release 构建 | `validation/facade-original/final-build.log`；29 个资源输入逐项比对后复用未变化的编译资源，见 resource-reuse.json |
| 原版独立内存和输入 | 12/12，`original-camera-e4uh7_ei/summary.json`；三种 EXE 名称 × 三种旧路径及任意组件路径；原文件和缓存文件摘要不变 |
| 真正错误框及禁用 | 7/7，`original-camera-failures-__jv7jx7/summary.json`；自动检查实际对话框并关闭，正常退出 |
| 识别缓存和 LEGACY 边界 | `validation/facade-original/discovery-test.log`；冷/热缓存、文件变化、损坏缓存、黑名单、深度、改目录、普通依赖均通过 |
| 原版相机 + 原版 Effekseer + MME | 9/9，`mmdfacade-functional-lrzilt23/summary.json`；真实窗口消息、.efk、停止/重启、Reset 后渲染。首组有效/Reset/重启均比停用多 1006 个变化像素 |
| 失败 Reset 后恢复 | `mmdfacade-functional-2yajpk9i/summary.json`；无 MME 对照下使真实 D3D9 Reset 返回失败，再恢复成功 |
| ABI 3 核心观察样本 + MME 产品窗口 | 9/9，`mmdfacade-product-ozuxcmd8/summary.json`，正常退出，无强制结束 |
| 源码适配相机 | 9/9，`first-person-input-k_la0czf/summary.json`，独立计数 |
| 导出预检与重置契约 | `validation/facade-original/preflight-regression.log`，2/2 CTest |

初次共存测试暴露了缓存临时文件路径超长，修复后重新完成矩阵。测试自动关闭错误框最初使用错误的按钮消息导致超时，这两次自建测试进程被终止，原始失败日志保留；最终 7 组改用对话框关闭消息后正常退出。它们不计入通过数。

## 未完成的通用能力

未实现任意 MMDMainData 字段、任意原版版本、后台线程访问、模型/骨骼内存视图、完整 ABI 3 消息过滤、完整 D3D9 前后回调和 D3DX 特效编译回调。下一步应逐个取得目标原版 DLL 与对应源码/布局证据，追加具体失败用例和宿主接口，再扩展表中字段与事件。

本轮不修改 MMEPatch。单机性能测量另存 `validation/facade-original/cost.log`：比较同一新宿主中的原版相机与源码适配相机，两次预热后 ABBA × 2，测到相机 start 的时间、场景总耗时、进程 CPU 与采样峰值工作集。该比较不能替代相对未修改 abi 宿主的性能基线，也不代表零开销；结果以实际样本文件为准。


ABBA 测量结果（各 4 个有效样本的中位数，`facade-cost-xly_wmv4/summary.json`）：

| 指标 | 原版相机 | 源码适配相机 |
| --- | ---: | ---: |
| 到相机 start | 1.2169 s | 1.2212 s |
| 含渲染/Reset/重启的场景总时间 | 1.8338 s | 1.8393 s |
| 进程 CPU 时间 | 5.1484 s | 5.0469 s |
| 采样峰值工作集 | 564.72 MiB | 564.52 MiB |

差值处于这个小样本测试的波动范围，不能据此宣称提速或零损耗。CPU 为所有线程累计时间；内存按 20 ms 采样，可能漏掉瞬时峰值；场景总时间含启动和销毁，不是单帧 GPU 时长。未修改 abi 宿主的交错基线测量尚未完成。

最终提供者静态保护加入后，另跑了原版三插件共存单组与提供者 CTest，见 `functional-provider-final.log`、`provider-final.log`；7 组错误框检查亦在最终二进制上执行。MMDFacade 的 37 个、MMHack 的 22 个导出名称及序号与本地原构建一致，记录在 `export-regression.json`。
