# 负数关键帧核心与集成

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `d1fa133`。本批从 main 已提交的 `3782e0c` 中同步负数关键帧部分；该提交同时包含缓存与透明排序等已另行同步的内容。

## 来源行为

main 的负数关键帧实现：

- 0 帧按左方向键进入 -1，当前帧输入框接受负数；切换帧重新求值，未登记的骨骼与表情编辑复位。
- 帧号与范围解析增加带符号路径：`minimum = -maximum`、`ParseSigned`、`ResolveRange(left,right,last,from,to,playback)`。有序端点按字面值处理（含 `-10..-1`）；左端大于右端时右端按倒数第几帧解释（1 为最后登记帧，0 也按最后帧）。播放排除负帧，从负帧开始播放时起点钳到 0；`0..0` 只到 0 帧。
- 负帧关键帧不进入原生无符号关键帧池：骨骼、表情、模型显示、相机、灯光、自阴影、重力与附件各自登记到 `negative_frames` 的独立存储。负帧之间以及最后一个负帧到 0 帧之间，位置与表情线性插值、旋转用最短路径四元数、显示与开关保持前一关键帧的值。
- 负帧支持选择、删除、普通复制粘贴；范围选择与当前帧允许负数。尚未接入原生曲线编辑与反转粘贴；VMD 标准导出只包含原生非负关键帧。
- 负帧及带符号游标、播放范围存入 `MMSXPMM2` 扩展：`org.mikudancestudio.timeline.negative_keys.v1`（版本 1、记录数、每条 164 字节：模型槽、类型、目标索引、带符号帧号位模式、20 字节绑定名、16 个小端 double），`signed_cursor.v1`（当前帧与时间轴左端）与 `signed_play_range.v1`（播放范围输入值）。标准 PMM 部分仍写非负数，负帧本身不触发 `0003`。

## 本批实现

- [frame_limits.hpp](../../include/mikumikustudio/frame_limits.hpp)：采用 main 的 `minimum`、`ParseSigned`、`ResolveRange`，`ParsePlaybackBound` 改走带符号解析；原有的非负 `Parse` 保留给原生关键帧输入。
- [negative_frames.hpp](../../include/mikumikustudio/negative_frames.hpp) 与 [negative_frames.cpp](../../src/animation/negative_frames.cpp)：采用 main 的 670 行独立模块，包含各轨道登记、求值、选择与复制粘贴、扩展记录捕获/载入、时间轴绘制与清空。
- 集成：`key_registrars`、`model_keyframe_edit`、`model_frame_seek`、`track_apply`、`render/accessory`、`command_view_menu`、`accessory_paste`、`bone_scale` 的负帧登记与求值；`pmm_save` 播放范围标准化、`pmm_load_v2` 播放停止帧 0 显示为 -1；`pmm_project_settings` 的 Reset/Load/Capture 接入负帧扩展；`shutdown_cleanup` 清理状态；`command_control_400` 的命令路由、播放范围与帧范围选择；`ui_frame_step`/`ui_frame_refresh`/`ui_hscroll`/`ui_panel_paint`/`ui_editor_click`/`ui_mouse_misc`/`ui_selection_reeval`/`ui_edit_commit` 的负帧游标、滚动、面板绘制与选择；`save_compatibility` 对负帧不生成旧格式副本。
- [tests/negative_frame_tests.cpp](../../tests/negative_frame_tests.cpp) 与 CMake 目标 `negative_frame_tests`（仅完整构建档）：26 项检查，覆盖带符号解析、范围解释、负帧登记与插值、扩展捕获/载入、损坏载荷拒绝与清空。
- 原始兼容配置照常编译：该配置在载入时忽略工程扩展记录，因此负帧关键帧不会从 PMM 恢复；这是该配置既有策略，负帧功能实际只在完整构建档可用。

## 验证

本机 Win10 x64，隐藏窗口；证据目录 `build/verification/abi-negative-frames-20261024/`（每项保存命令、退出码、耗时与二进制摘要）。

| 检查 | 结果 |
| --- | --- |
| `negative_frame_tests`（完整构建） | 通过，`PASS negative frames checks=26` |
| `project_save_export_tests` 完整回归 | 通过，405 项（含真实 PMM 保存/载入路径） |
| `autosave_tests` 完整回归 | 通过，1,143 项 |
| 原始兼容 `--numbered-only` 保存回归 | 通过，39 项 |
| 完整产品 | 构建通过，EXE SHA-256 `8ae7f3c3416232ab55fc86a240b1c3f25d37138ba4e885e6855322fdb31609c3` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `bfc9e253d7525b4366b522c49989f85710a40a34ac63a655894c161024abeaa2` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- main 的宿主级负数检查（`model_edit_controls_checks.hpp`：真实面板登记、负帧 XYZ 重置与撤销、真实 PMM 写出后重载、范围选择/复制/粘贴按钮、从负帧开始播放的边界）尚未移植；本批只验证独立模块与已有保存/备份回归没有退化，**不能据此声称 UI 级负帧操作已验收**。
- 负帧时间轴绘制、选择框和滚动条的像素结果没有截图或人工视觉验收；只有代码路径与模块测试。
- 负帧未接入原生曲线编辑、反转粘贴与 VMD 导出；删除/撤销重做只有普通命令路径，未与共同历史（`plugins::SharedHistory`）合并。
- 原始兼容配置不持久化负帧；真实 MME、多视口、Win7/AMD、多 DPI 未验收。

## 后续

下一轮把 main 的宿主级负帧检查移植为 ABI 可运行的隐藏宿主测试，覆盖真实 PMM 保存/重载、复制粘贴按钮与播放边界，并补齐时间轴绘制核对；之后继续缓存管理、效果/纹理缓存消费者与 P1–P6。
