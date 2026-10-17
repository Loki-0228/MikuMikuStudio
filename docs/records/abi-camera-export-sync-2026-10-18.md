# 同步模型模式导出的相机构图修复

来源为「排查 MMS 相机崩溃来源」中已提交的 `main` 修复 `ea4b82b4f74acbd8154bcb01b8cbe9d2d427e76d`，ABI 起点为 `cb8ad9b`。源码、构建和测试改动均位于独立的 `D:/MMD-Next/MMS-abi-dev`。

## 问题与修复

模型模式导出 MP4 时，播放已采样相机关键帧，但世界变换仍选择模型编辑参考点。相机目标位置未参与构图，导致画面上移和人物裁切。

`CameraTrackView` 统一判断相机视图。视频及图片序列录制窗口存在，并且正在逐帧输出或等待启动时，`ResolveFrameFocus` 和 `SetupFrameWorldTransform` 均采用相机轨道，包括父骨骼的位置与旋转。相机模式及启用跟随的模型模式沿用相机视图。

普通模型编辑、手动步进及单张截图继续使用当前编辑视角。判定只读取现有状态，不切换界面模式、不写关键帧、不移动播放头、不新增历史记录。透视及正交投影算法保持原实现。ABI 的图片序列调度入口 `coordinator_fixture::Drive` 继续保留。

## 回归范围

新增的 `camera_export_modes` 包含 144 项断言，覆盖透视与正交、有父绑定与无父绑定、三种模型参考、跟随开关，以及录制中和等待启动两种状态。无录制窗口时的步进标志、等待标志及其组合均保留编辑视角；单张截图也核验模型参考。普通相机跟随测试继续验证父骨骼及自由模型视图。

`export_viewport_runner.py --case camera-modes` 走正式 MP4 导出与解码路径，比较相机模式、模型模式和模型跟随模式送入 H.264 编码器的六帧图像。基准必须包含彩色场景内容，并随相机关键帧变化，避免仅凭空白画面相等误判。该用例单独启用，不改变原有默认视窗测试对编码器的要求；`--coordinator` 额外启用 ABI 正式调度器。

真实 MP4 用例验证无父绑定的默认透视视图；父骨骼、模型参考及正交组合由矩阵测试验证。该用例不验证抗锯齿和渲染缩放。图片序列提供模型路径时必须成功加载，避免模型模式被静默跳过。

## 验收

旧实现与新增矩阵测试对照时，12 组模型模式构图检查失败，见 `build/abi-camera-export-baseline-tests-1.log`。恢复修复并补齐启动与无窗口反例后，`camera_export_modes` 和 `camera_follow_regressions` 均通过，见 `build/abi-camera-export-matrix-tests-1.log` 及详细日志 `build/abi-camera-export-matrix-detail-1.log`。

完整版产品、图片序列及共同历史测试构建通过，见 `build/abi-camera-export-final-build-2.log`。正式导出测试构建通过，见 `build/abi-camera-export-final-build-3.log`。首次构建发现旧导出测试直接调用了高版本 DPI API，现已改为通过 `GetProcAddress` 解析并检查可用性；产品的 Win7 编译目标保持不变，测试仍要求支持这些 DPI API 的系统。

MP4 直接驱动和 ABI 调度两条路径均通过，分别见 `build/abi-camera-export-mp4-direct-1.log`、`build/abi-camera-export-mp4-coordinator-1.log` 及各自目录内的 `results.json`、`result.log`。每条路径实际生成三段六帧视频，编码前逐帧图像一致，首尾帧均可解码，编辑模式与跟随选择保持不变。

图片序列的直接驱动通过 914 项，ABI 调度路径通过 878 项，见 `build/abi-camera-export-sequence-1/report.json` 及两份 `result.log`。调度夹具通过正式帧入口检查时间步恢复，未重复执行直接路径的 36 项调用方时间步检查，因此两者计数不同。这些结果包含实际加载模型后的相机、模型模式、不同起点与帧率，以及单图和取消路径；未启用额外的时序抗锯齿模型场景。

ABI 契约、统一设置及可选图形依赖三组通过，见 `build/abi-camera-export-contract-tests-2.log` 与详细日志。首次受限环境运行在 GDI 复制检查处失败，正常桌面权限下重跑通过，无需修改产品。

原版兼容档产品及原有测试目标构建通过，见 `build/abi-camera-export-original-build-2.log`。`original_profile_io`、`original_profile_state` 和可选图形依赖三组通过，见 `build/abi-camera-export-original-tests-1.log`。该配置未启用 UI 测试，构图矩阵回归在完整版配置运行。

完整共同历史最终通过 196,711 项，见 `build/abi-camera-export-history-tests-1.log` 与详细日志 `build/abi-camera-export-history-detail-1.log`。本批不修改公共 ABI、历史准入或事务语义。

本批保留 Win7 SP1 x64、CPU 基础路径及 AMD 兼容要求；本机回归不替代 Win7 和 AMD 真机验收。
