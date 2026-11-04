# 动画轨道与编辑机制

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-empty-playback-range"></a>
<a id="record-curve-sync-performance"></a>
<a id="record-mms-native-editors-validation"></a>
<a id="record-mms-native-editors-validation-保留的前版"></a>
<a id="record-mms-native-editors-validation-阶段验收记录"></a>
<a id="record-mms-native-editors-validation-原生功能后续验证与-mmv-交接"></a>
<a id="record-mms-native-editors-validation-最终原生验收"></a>
<a id="record-mms-native-editors-validation-排除的初次结果"></a>
<a id="record-mms-native-editors-validation-当前边界"></a>
<a id="record-bone-click-selection"></a>
<a id="record-bone-pick-cycle-fix"></a>
<a id="record-bone-pick-cycle-fix-cause-and-reference-behavior"></a>
<a id="record-bone-pick-cycle-fix-verification"></a>
<a id="record-bone-pick-cycle-fix-candidate-builds"></a>
<a id="record-rotation-axis-parity"></a>
<a id="record-rotation-axis-parity-原因与修复"></a>
<a id="record-rotation-axis-parity-验证"></a>
<a id="record-rotation-export-fixes"></a>
<a id="record-rotation-export-fixes-骨骼反复旋转后放大"></a>
<a id="record-rotation-export-fixes-视频导出数值多出数字"></a>
<a id="record-rotation-export-fixes-构建与分发"></a>
<a id="record-auto-interpolation-link"></a>
<a id="record-auto-interpolation-link-验收"></a>
<a id="record-curve-save-recovery"></a>
<a id="record-curve-save-recovery-验证记录"></a>
<a id="record-camera-tracking"></a>
<a id="record-camera-tracking-验证"></a>
<a id="record-hidden-morph-panel-safety"></a>
<a id="record-hidden-morph-panel-safety-用户报告"></a>
<a id="record-hidden-morph-panel-safety-样本核对"></a>
<a id="record-hidden-morph-panel-safety-面板分类与表示枠"></a>
<a id="record-hidden-morph-panel-safety-已有保护与证据范围"></a>
<a id="record-hidden-morph-panel-safety-mms-目标行为"></a>
<a id="record-hidden-morph-panel-safety-运行验收2026-09-27"></a>
<a id="record-hidden-morph-panel-safety-复跑专项测试"></a>
<a id="record-hidden-morph-panel-safety-尚未核实的原版证据"></a>
<a id="record-hidden-morph-panel-safety-复核入口"></a>
<a id="record-editor-input-fixes"></a>
<a id="record-editor-input-fixes-行为"></a>
<a id="record-editor-input-fixes-同条件回归"></a>
<a id="record-editor-input-fixes-运行包"></a>
<a id="record-editor-input-fixes-未验收项目"></a>

入口与操作见[动画编辑](../../guides/animation.md)和[曲线编辑器](../../guides/animation-curves.md)。

## 轨道与选择

骨骼、表情、显示/IK、相机、灯光、重力、自阴影及附件按各自轨道登记。列表、3D 视口和时间轴共用有效选择与编辑目标；取消当前骨骼后，从剩余选择确定目标，全部取消才清空。

切帧重新求值并清除未登记编辑标记，保留有效骨骼选择。隐藏表情可在时间轴选择、登记及参与组合求值；面板编号不属于四个操作栏时，不向这些栏写入无效索引。

## 关键帧输入与粘贴

数值在失焦或 Enter 时提交当前输入框，一次有效修改对应一次历史。打开工程、导入动作后的遗留 Enter 不应覆盖载入数据；回到前台时重新同步按键状态，不把已松开的删除键视为新按下。

跨模型粘贴按骨骼、表情名称匹配，同名轨道按模型内的先后次序对应。只复制某个同名项时也保留其相对身份，不总是贴到首项。不存在可匹配目标时不能误分配给其他骨骼。

框选以真实帧列和轨道行为准，处理折叠组、重绘、快速拖动及捕获取消。保留首记录与普通关键帧的移动语义分别维护。

## 自由曲线与原生插值

完整包支持骨骼九通道、表情和场景参数曲线，带对比显示、手柄、视野导航与沿曲线插键。切线模式与一次性预设分开，类型或预设立即应用，没有数据变化不追加历史。

原生相机在精确关键帧返回作者值，保留相邻一帧区间的原生规则。曲线保存、VMD 尾部扩展和烘焙的边界见[曲线指南](../../guides/animation-curves.md)。

## 骨骼缩放

支持等比、XYZ 及局部/全局/视图方向，和骨骼关键帧共同登记、复制、镜像与撤销。刚体形状、偏移与关节框架继承相应缩放。原版 MMD 不读取 MMS 缩放效果；IK 链长不会因缩放自动重建。详见[骨骼缩放](../../guides/bone-scaling.md)。

## 负数帧与范围

完整包支持负帧登记、选择、删除、普通复制粘贴和 PMM 保存；原生曲线编辑与反转粘贴尚未接入负帧。播放排除负帧，标准 VMD 只导出非负键。范围倒数语义和零帧规则见[动画指南](../../guides/animation.md#编辑负数帧与选择范围)。

## 工程时间处理

[帧率修正](../../guides/frame-rate.md)按原、现帧率转换工程时间，报告合并与无法处理的数据。[选中帧序列散布](../../guides/sequence-scatter.md)使用选中表情片段、范围和随机种子安排重复，不扩展成任意轨道散布。

## 保存与回归

原生场景曲线写入 PMM 扩展，MMV 参数动画由插件存档负责。最小包不保存完整包曲线扩展。文件能力见[兼容性](../../guides/files.md)，具体输入、曲线和真实场景回归见 [tests/README.md](../../../tests/README.md)。
