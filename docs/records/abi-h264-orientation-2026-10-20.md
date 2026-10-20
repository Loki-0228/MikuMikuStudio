# main H.264 导出方向修复同步

已将 main 的 `ff7616e` 功能同步到 ABI 分支。共享录制缓冲按底行在前保存 BGRA，NV12 编码输入需要顶行在前。原转换没有翻转源行，因此系统 H.264 的 NV12 路径导出上下颠倒。

`ConvertBgraToNv12` 现在同时翻转亮度和色度的源行。软件 RGB32 路径继续传原始字节及负步长，不做第二次翻转。ABI 已有直接写入 Media Foundation 缓冲的异常保护保留，未用 main 版本覆盖整个文件。

## 验证

完整配置和原始兼容配置均构建通过，证据目录为 `build/verification/abi-h264-orientation-20261020/`。

| 验证 | 完整配置 | 原始兼容配置 |
| --- | --- | --- |
| 原编码器启动故障、软件回退、写帧及四象限输入契约 | 126 项通过 | 126 项通过 |
| 实际 RGB32 与 NV12 短片编码、解码、尺寸及四象限方向 | 62 项通过 | 62 项通过 |

日志为 `full-contract.log`、`original-contract.log`、`full-orientation.log` 和 `original-orientation.log`。实际 MP4 分别保存在 `full-mp4/` 和 `original-mp4/`，均由软件编码器生成。NV12 使用与硬件相同的生产转换和写帧路径，验证不依赖指定显卡。

实际 AMD 编码器及 Win7 真机未验收。此记录没有将软件编码、解码结果记为硬件验收。main 的未提交修改未纳入同步。
