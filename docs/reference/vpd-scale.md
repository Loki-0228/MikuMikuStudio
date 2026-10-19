# VPD 姿态中的骨骼缩放

保存姿态时，MMS 在原有 VPD 文本结束后追加骨骼缩放。原有位置、旋转文本及其读取边界不变。仅记录本次导出的骨骼，默认缩放无需额外记录。

扩展按骨名和模型内索引标识骨骼，保存九个浮点数组成的缩放矩阵。尾部含版本、长度以及正文和扩展各自的 CRC32。布局定义见 `include/mikumikustudio/vpd_scale_io.hpp`。

读取先完成普通 VPD，再检查扩展。普通 VPD 恢复被载入骨骼的默认缩放，避免残留上一个姿态的值。损坏、截断、未知版本或非法矩阵会拒绝整段缩放扩展，并记录日志；位置和旋转仍按标准段载入。骨名丢失、映射歧义或超出本次姿态范围的条目会跳过，不猜测目标骨骼。

2026-09-27 已完成 VS 2022 x64 Release 编译。独立尾部编解码测试通过 80 项检查；调用真实 `SaveVpdFile`、`LoadVpdFile` 的姿态往返及 PMM/VMD 缩放回归通过 160 项检查。测试包含无尾部旧文件、损坏尾部、骨骼映射和非单位缩放。记录位于 `.local/artifacts/global-processor/vpd-scale-io-integration.log` 和 `bone-scale-io-integration.log`。
