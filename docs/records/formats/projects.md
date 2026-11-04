# 工程与动作读写

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-pmm-v2-extensions-validation"></a>
<a id="record-pmm-v2-extensions-validation-结论与范围"></a>
<a id="record-pmm-v2-extensions-validation-构建与产物"></a>
<a id="record-pmm-v2-extensions-validation-正确性"></a>
<a id="record-pmm-v2-extensions-validation-后台性能对照"></a>
<a id="record-pmm-v2-extensions-validation-静态兼容证据与委派"></a>
<a id="record-pmm-camera-load"></a>
<a id="record-pmm-camera-load-原因与修复"></a>
<a id="record-pmm-camera-load-验证"></a>
<a id="record-pmm-camera-load-ui-检查后续修复"></a>
<a id="record-vmd-fixed-axis-import"></a>
<a id="record-vmd-fixed-axis-import-验证"></a>
<a id="record-keyframe-capacity"></a>
<a id="record-keyframe-capacity-行为"></a>
<a id="record-keyframe-capacity-验证结果"></a>
<a id="record-keyframe-capacity-未完成的验收与边界"></a>
<a id="record-keyframe-persistence-audit"></a>
<a id="record-keyframe-persistence-audit-当前支持范围"></a>
<a id="record-keyframe-persistence-audit-修复"></a>
<a id="record-keyframe-persistence-audit-验收记录"></a>
<a id="record-filename-encoding"></a>
<a id="record-filename-encoding-当前处理"></a>
<a id="record-filename-encoding-验证"></a>

## 文件职责

PMM 保存模型、媒体引用、场景和关键帧；VMD 交换动作；VPD 交换姿态；EMM 保存 MME 分配。可选 MMV 状态由其独立存档负责。操作与备份见[文件指南](../../guides/files.md)。

## PMM 版本选择

完整包优先写 `0002`。未使用的预留容量不触发新版格式，高位稀疏槽在文件中压紧。实际非负关键帧数量、显示计数或帧号超出旧能力时才可能选择 `0003`。

完整名称、负数帧和引用路径的编码差异本身不要求 `0003`。固定路径字段仍有大小限制；采用可还原编码不保证原版能定位同一路径。原版和最小包不能读取 `0003`。

当前工程继续采用 `MMSXPMM2` 尾部扩展。新分节布局尚未接入，不能与已支持的 `0003` 容器混为一谈。

## 名称、容量与绑定

完整名称写入扩展，标准字段保留可用于原生绑定的数据。按需扩展关键帧、显示项及范围并检查目标格式，不能通过截断动作、错误分配骨骼或伪造名称来绕过限制。

可安全生成旧名称副本时自动写 `.compat.pmm`、`.compat.vmd`；冲突、容量或路径不兼容时说明原因。同名副本追加序号，不覆盖已有文件。

## 负数帧与扩展

负帧和带符号的当前位置、播放范围写入 PMM 扩展，标准部分保持非负键。含负数数据时不生成会丢掉这些内容的标准部分 PMM 副本。标准 VMD 导出仅含非负关键帧。

缩放、自由曲线和网格缓存由各自扩展保存。原版及最小包再次保存会丢弃 MMS 扩展，应保留独立工程副本。

## 载入与保存事务

载入后不自动登记当前姿态，关闭对话框的遗留 Enter 会被消耗。保存使用构造、写入和替换的完整流程，异常不把部分候选当成功结果。备份目录、增量名称、`.pmm1` 和恢复边界见[本地数据](../../reference/local-data-storage.md)及[运行恢复](../../development/runtime-reliability.md)。

## 格式参考与验证

字节布局、键名和版本以[PMM 扩展](../../reference/pmm-extensions.md)、[VPD 缩放](../../reference/vpd-scale.md)为准。读回成功不等于任意第三方软件无损往返，具体原版版本和场景仍需验证。回归入口见 [tests/README.md](../../../tests/README.md)。
