# Windows ABI 候选：打包和媒体验收

此流程收集真实 Windows 构建的打包与组件证据。它不会启动发布版主程序、安装系统依赖或自动操作可见桌面。原始工程、稳定版和已有发布目录不参与写入。组件通过也不能替代下面的产品操作，更不等于 ABI 可以替代 main。

## 先准备独立构建与证据目录

使用已获授权的 Windows x64 测试环境、单独源码副本、预先配置的本地依赖和 DirectX 9 运行环境。参考 [Win7 x64 基础构建](win7-build.md)，在这个专用工作区启用 UI 测试并构建以下目标。不要向稳定版工作区套用这些命令。以下示例从源码根运行，需要 Python 3.11+、Git、CMake/CTest 和 PowerShell 7；输入路径请替换成实际位置。首次编译资源可能长时间不输出，应按基础构建说明等待。

```powershell
cmake --preset win7-x64 -DMIKUMIKUSTUDIO_BUILD_UI_TESTS=ON
cmake --build --preset win7-x64-release --target MikuMikuStudio legacy_mmd_menu_tests legacy_observer_export_tests native_media_tests image_sequence_tests
python -B tools/package-adaptive-ui-template.py --output build/packages/mms-adaptive-ui-template-candidate1
python -B tests/run_windows_abi_candidate.py --build build/cmake --ui-template build/packages/mms-adaptive-ui-template-candidate1.zip --evidence C:/mms-qa/abi-candidate1
```

`C:/mms-qa` 须已存在，`abi-candidate1` 必须是新目录，且在源码和构建目录之外。证据目录路径宜短，以免触碰媒体组件的 Windows 路径限制。构建树须允许 `ctest` 写其 `Testing` 日志和 `cmake --install` 写 `install_manifest.txt`。这里的 `cmake --install` 只把产物复制到新证据目录下的 `cmake-install`，不做系统安装；另将脚本发行布局复制到 `release`，不使用 `-Force`。

| 自动检查 | 证据与边界 |
| --- | --- |
| 构建与模板身份 | `build.json` 记录配置、实际候选 EXE 和私有兼容 DLL、组件 EXE、模板等哈希。模板清单必须与当前 HEAD、脚本和源码字节对应。前后源码清单记录工作树实际内容，不证明二进制一定由这些字节构建；应另存本次完整构建日志并避免中途切换源码。 |
| 发行与安装 | `package-hashes.json` 检查预期发布布局、私有 `PLUGINS/LEGACY/MMDPlugin.dll`、无第二份同名 DLL，及已复制文件的 SHA-256。安装树可另外有运行库。运行库闭包及 Win7/AMD 真机仍是单独的门槛。 |
| 菜单与观察者预检 | 运行三个精确匹配的 CTest 名称。未注册的项记 `BLOCKED`，不得把空 CTest 算通过。已注册的预检使用测试观察者，不代替真实审阅 DLL、MME 0.37 或 AA 共存。 |
| 单张 PNG，原生 Shell 组件 | 在组件自建的非输入桌面运行 `image_sequence_tests --still-shell-only`。真实 Shell 对话框用隔离控件脚本测试。 |
| 单张 PNG，渲染组件 | 在新目录运行 `image_sequence_tests --still-only`。渲染使用隐藏的真实 D3D 路径，选择输入由测试替代。与上一项互补，不能算可见产品一条路径通过。 |
| 透明 MOV，发布组件 | `native_media_tests --mov-publication` 使用 WIC 和真实 Windows 文件，检查目标变更、锁定、取消和重试。它不证明产品菜单或最终 MOV 的场景透明度。 |

查 `summary.json` 及各项日志。`PASS` 只适用于对应的自动步骤，`FAIL` 和 `BLOCKED` 均返回非零。可见产品项恒标 `NOT RUN`，须另取证。

## 可见候选：分开执行三条媒体路径

使用发布包副本。将已获授权的 Review 工程和素材复制到专用工作目录，确认项目加载的是这些副本，记录原件与副本的哈希、候选 EXE 哈希及场景设置。保留对话框截图、结果、实际设置和候选版本；截图、日志放输出目录外。每个动作使用不同的新空目录。

1. **单张 PNG。** 通过实际「渲染到图片文件」菜单，核对现代保存窗口与默认 PNG。先取消一次，确认目录仍空。重新打开后用无扩展名的 `still` 保存；确认得到 `still.png`、应用仍打开、活动工程未变。用包含透明背景、实心前景和可预期半透明边缘的场景，事先确定样点和可接受的 alpha 区间，再独立解码核对，不能从输出反推样点来宣称通过。
2. **29–31 图片序列。** 走实际「导出图片序列」菜单的文件夹选择器，点「选择文件夹」，确认选定路径进入导出设置。30 FPS 工程以 30 FPS 导出 29–31，PNG、640×480、`frame_`，关闭「导出后关闭」。应用保持打开、活动路径不变、源文件哈希不变。复用 ABI 验证包的 `fixtures/png/verify_png.py`（需 Pillow）；必须另外有三个独立可区分的参考帧，或可对应到输出的实际采样证据，才能判定时间顺序。验证包中的 `--candidate main/abi` 只对应旧冻结 EXE；新构建使用本次的 `build.json` 及产品证据。若原生选择器仍卡住，记准确标题、路径、操作和截图，不能用组件模式或绕过选择器判通过。登记、撤销、重做、另存和重开是另一个验收步骤，仍需按 ABI 验证矩阵留证。
3. **透明 MOV。** 使用实际产品的「PNG transparent MOV」入口，用已知透明背景和实心前景的静态短场景，关闭音频并记录输出尺寸、帧数、帧率。确认选定文件、输出完成和应用状态。独立解码每一帧检查已知样点。若是运动场景，选整个片段中含义不变的样点，不能把这项固定样点检查当成时间顺序证明。WIC 发布组件不能代替这条产品路径。

下列样点仅为参数示例，正式证据须替换为实际场景预先记录的坐标与区间。`$candidate` 为正在验收的 EXE，`$expectedHash` 从验收前的可信哈希记录读取。PNG 用 Pillow；MOV 用已存在的 `ffprobe` 和 `ffmpeg`，必须只有 PNG 视频流，按预期帧率完整解码指定短片。脚本拒绝包含残留文件的目录、尺寸/帧数不符及错误 alpha，且不写输出文件。将 JSON 结果保存到独立证据目录。

```powershell
python -B tests/verify_candidate_export_alpha.py C:/mms-qa/still-out/still.png --size 640 480 --sample 20 20 0 0 --sample 320 240 255 255 --sample 340 220 120 136 --candidate-exe $candidate --candidate-sha256 $expectedHash
python -B tests/verify_candidate_export_alpha.py C:/mms-qa/mov-out/alpha.mov --size 640 480 --frames 3 --fps 30 --sample 20 20 0 0 --sample 320 240 255 255 --candidate-exe $candidate --candidate-sha256 $expectedHash
```

打包之后，仍须在隔离运行副本用真实审阅过的观察者 DLL 检查加载路径、菜单路由和原生 AA（含关闭、设备支持的模式、插件禁用、重启及 MME 共存）。这些媒体检查不测试插件运行。

## 便携自测

`python -B -W error tests/windows_abi_candidate_checks_test.py` 在已有 Pillow 和 FFmpeg 的机器上检查工具自身。合成图像、合成 MOV、PE 头样本和临时目录不属于候选；Linux 运行不会调用 Windows 打包脚本或 Windows 组件。
