# FirstPersonCamera 的 MMS 适配版

这是基于 FirstPersonCamera f71c300 的独立源码适配，输出 `MMSFirstPersonCamera.dll`。
ABI 4 工厂及输入回调保留；原版的 `MMDMainData` 读写改为 MMS 明确提供的相机 API。
原版 `MMDFirstPersonCamera.dll` 不能直接替换此文件，宿主会在调用其工厂前拒绝。

发布包已将本插件放入 `<EXE>/PLUGINS/first-person-camera/`，启动时会递归发现并加载。
退出 MMS 后可在 `PLUGINS` 内重命名或移动完整插件包，例如 `PLUGINS/相机工具/第一人称/`。
插件发现只以 `PLUGINS` 为根，根目录的 `optional-plugins` 不单独扫描。
默认扫描两层子目录，更深的分组按现有 `PLUGINS/config.json` 的 `maxDepth` 配置。
新版宿主通过 DLL 中的相机适配标记自动识别本插件，不再需要 `plugin.json`，也无需单独启用。
保留 `MMSFirstPersonCamera.dll` 文件名；也可直接放在 `PLUGINS` 根目录。
编译和安装会删除输出目录中本插件的遗留清单。普通原版旧插件仍只在 `PLUGINS/LEGACY` 内自动识别。
停用时在 `PLUGINS/config.json` 的
`disabledPlugins` 中加入 `org.mikumikustudio.legacy.first-person-camera`，重启生效。
独立 `MMDPlugin.dll` 仍由主程序安装包提供，新旧提供者目录均受支持。

在相机模式下，视口取得焦点后按 W/S 前后移动、A/D 横移，Shift 加速。
在视口内按住左键拖动转向；Esc、松键、失焦和停用均释放光标及鼠标捕获。
文本框输入、模型模式、播放、录制和场景事务期间不编辑相机。
编辑的是当前姿态，不自动注册关键帧，不添加撤销记录；按 MMS 原有方式注册相机关键帧。

本配置仅支持经过验证的相机输入回调，不提供通用 ABI 4 / D3DX 特效 hook。
源码许可见 LICENSE。
