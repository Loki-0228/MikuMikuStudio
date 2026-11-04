# 界面、输入与编辑操作

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-fullscreen-scaling"></a>
<a id="record-fullscreen-scaling-同日补充普通界面比例仍异常"></a>
<a id="record-fullscreen-scaling-原因和修复"></a>
<a id="record-fullscreen-scaling-已完成的后台检查"></a>
<a id="record-fullscreen-scaling-本地产物"></a>
<a id="record-viewport-handle-corners"></a>
<a id="record-viewport-handle-corners-实现与验证"></a>
<a id="record-viewport-cursor-style"></a>
<a id="record-viewport-cursor-style-实现"></a>
<a id="record-viewport-cursor-style-验证"></a>
<a id="record-viewport-cursor-style-本地产物与版本"></a>
<a id="record-accessory-hud"></a>
<a id="record-accessory-hud-构建与版本"></a>
<a id="record-accessory-hud-验收结果"></a>
<a id="record-accessory-hud-本地证据"></a>
<a id="record-help-menu-paint"></a>
<a id="record-help-menu-paint-白块原因"></a>
<a id="record-help-menu-paint-前后验证方式"></a>
<a id="record-help-menu-paint-独立窗口置顶的范围"></a>
<a id="record-help-menu-paint-编译版本"></a>
<a id="record-help-popup-layout"></a>
<a id="record-help-popup-layout-行为变化"></a>
<a id="record-help-popup-layout-构建与证据"></a>
<a id="record-help-popup-layout-同步到日常运行目录"></a>
<a id="record-model-edit-dialog-fixes"></a>
<a id="record-model-edit-dialog-fixes-隐藏窗口回归"></a>
<a id="record-morph-dropdown-width"></a>
<a id="record-morph-dropdown-width-首次绘制覆盖"></a>
<a id="record-morph-launcher-wheel"></a>
<a id="record-morph-launcher-wheel-后台对照"></a>
<a id="record-morph-launcher-wheel-验收范围"></a>
<a id="record-scale-label-repaint"></a>
<a id="record-bone-scale-readout"></a>
<a id="record-bone-scale-readout-验证与限制"></a>
<a id="record-timeline-parameter-readouts"></a>
<a id="record-timeline-parameter-readouts-后台对照"></a>
<a id="record-popup-opening-repaint-20261021"></a>
<a id="record-issue40-registration-undo"></a>

## 选择与视口

名称栏、时间轴和 3D 点选维护一致的有效目标。骨骼选择与未登记编辑标记分开，切帧不清掉选择；Shift 切换只执行一次。折叠组和框选按实际布局命中，鼠标松开、取消或丢失捕获时结束拖动。

视口移动、旋转、缩放控件按当前模型、附件或相机状态工作。附件控件显示 `.x` 标识。没有有效骨骼或处于禁止编辑状态时，骨骼缩放入口不修改数据。手柄与坐标规则见[骨骼缩放](../../guides/bone-scaling.md)。

Ctrl 配合右上角移动、缩放按钮操作背景；普通拖动保持相机操作，见[背景指南](../../guides/background-media.md)。

## 键盘与数值输入

快捷键分发区分文字输入焦点与场景命令。数值编辑在失焦或 Enter 时提交当前控件，避免一次输入创建重复历史。载入对话框和重新获得前台的按键状态独立同步，不把遗留按键当新命令。

当前帧支持负数输入，其范围选择、播放和编辑边界见[动画指南](../../guides/animation.md)。

## 布局、缩放与重绘

时间轴文字限制在所属行和名称栏内，长名称裁剪，不覆盖帧列。工具、底栏、分割线和独立窗口按实际可用尺寸布局；显示缩放、窄窗口和拖动边界使用真实控件坐标。

菜单按当前语言和命令状态绘制，首次展开的补画在完成后停止，不持续刷新。不能用调试截图坐标替代实际布局。

## 曲线与窗口

通道树标签点击同步活动通道和键盘定位。对比显示、主曲线、勾选与显隐各自维护状态。独立曲线窗口、MMV 参数窗和主时间轴的关系见[曲线指南](../../guides/animation-curves.md)及[MMV 参数界面](../extensions/interface.md)。

## 检查入口

窗口、控件、骨骼登记、菜单首次展开、捕获、快捷键及语言测试见 [tests/README.md](../../../tests/README.md)。隐藏窗口与消息检查能验证状态和绘制，但不自动等于所有显示缩放及人工交互都已验收。
