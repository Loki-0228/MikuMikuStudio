# 可选插件分配与参数界面

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mmv-ui-implementation"></a>
<a id="record-mmv-ui-implementation-已完成并复核的模块"></a>
<a id="record-mmv-ui-implementation-外置描述与算法源码"></a>
<a id="record-mmv-ui-implementation-验证产物"></a>
<a id="record-mmv-ui-scope-acceptance"></a>
<a id="record-mmv-ui-scope-acceptance-本轮行为"></a>
<a id="record-mmv-ui-scope-acceptance-验证证据"></a>
<a id="record-mmv-ui-scope-acceptance-已知边界"></a>
<a id="record-mmv-curve-ui-validation"></a>
<a id="record-mmv-curve-ui-validation-修复内容"></a>
<a id="record-mmv-curve-ui-validation-实际程序验证"></a>
<a id="record-mmv-curve-ui-validation-构建与回归"></a>
<a id="record-mmv-curve-ui-validation-前后耗时对照"></a>
<a id="record-mmv-curve-ui-validation-产物与来源"></a>
<a id="record-mmv-assignment-scope-design"></a>
<a id="record-mmv-assignment-scope-design-作用范围与输出"></a>
<a id="record-mmv-assignment-scope-design-层间数据"></a>
<a id="record-mmv-assignment-scope-design-展开操作"></a>
<a id="record-mmv-assignment-scope-design-初始错误的来源"></a>
<a id="record-mmv-assignment-scope-design-源码依据"></a>
<a id="record-mmv-editor-undo-plan"></a>
<a id="record-mmv-curve-motion-plan"></a>
<a id="record-mmv-curve-motion-plan-已确认的范围"></a>
<a id="record-mmv-curve-motion-plan-表情与附件的入口图标"></a>
<a id="record-mmv-curve-motion-plan-附件操作区布局用户截图已确认"></a>
<a id="record-mmv-curve-motion-plan-界面布局用户已选定"></a>
<a id="record-mmv-curve-motion-plan-曲线编辑器交互规则"></a>
<a id="record-mmv-curve-motion-plan-浮窗视野导航"></a>
<a id="record-mmv-curve-motion-plan-视野导航与当前帧滑块的区别"></a>
<a id="record-mmv-curve-motion-plan-时间范围条与数值范围条"></a>
<a id="record-mmv-curve-motion-plan-鼠标与键盘快捷键"></a>
<a id="record-mmv-curve-motion-plan-视图之间的范围共享"></a>
<a id="record-mmv-curve-motion-plan-本节状态"></a>
<a id="record-mmv-curve-motion-plan-通道树"></a>
<a id="record-mmv-curve-motion-plan-速度视图"></a>
<a id="record-mmv-curve-motion-plan-mmd-局部手柄与完整曲线"></a>
<a id="record-mmv-curve-motion-plan-切线模式与显示"></a>
<a id="record-mmv-curve-motion-plan-求值与边界情况"></a>
<a id="record-mmv-curve-motion-plan-曲线与关键帧语义"></a>
<a id="record-mmv-curve-motion-plan-vmd-必须保留原版可读取的标准数据"></a>
<a id="record-mmv-curve-motion-plan-工程与预设的存储边界"></a>
<a id="record-mmv-curve-motion-plan-烘焙与动作范围选择"></a>
<a id="record-mmv-curve-motion-plan-实现与验收边界"></a>
<a id="record-mmv-curve-motion-plan-核对依据"></a>

## 菜单与编辑器

当前 main 加载 [MMV4MMS](usage.md)，由插件注册右上角 MMV 菜单。效果映射、启停、自动重载、保存、日志和 EMM 加载不是宿主硬编码菜单。没有兼容插件时不显示占位入口。

## 层与分配范围

配套效果编辑器按层展示分配，自带层为 0，正层从大到小执行。同层各效果读取该层冻结的输入；重叠写入按既定顺序覆盖，跨层逐层传递，不反复执行循环依赖图。

自带分配页的骨骼与材质是两种展开方式，不是两个执行阶段。右键可选择展开许可与方式；未允许时箭头禁用。骨骼行按当前层的绑定及权重限定范围，材质行限定对应材质，不从显示名称猜测稳定身份。

刚体和 joint 是结构查询及求解数据，不作为普通独立分配组。作用规则见[FX 数据与范围](effects.md#record-mmv-fx-数据与作用范围)。

## 参数与曲线

FX 通过同名 `.uifx` 或显式描述声明可编辑参数、多语言名称、范围、分组与 K 轨道，见[UIFX 参考](parameters.md)。描述文件不改写原 FX，不从无声明的传统 FX 自动猜测可编辑变量。

参数身份绑定效果实例和稳定 ID。源文件换名、移动层或改变界面标签不能随意更换动画归属。曲线及工程状态保存在 VMM；VEM 对象预设保存当前静态值。原生相机、灯光等场景曲线由 MMS 自己保存。

## 窗口与操作

参数面板、曲线、分配表和宿主时间轴的交互由各自接口负责。编辑器状态显示就绪不等于求解一定成功，应同时查看诊断日志。应用、取消、窗口关闭及撤销不能留下部分配置。

## 能力边界

页面描述的是配套插件交互约定，不代表所有插件版本都有相同布局、求解器或历史集成。当前宿主输出是完整场景纹理，不使用旧版 `MMV.dll` 网格桥接方式，也不能同时启用 MME 代理。

曲线启动按钮的独立[设计资产](../../ui/mmv-curve-launcher/README.md)未接入生产界面，不能按预览图认定已有该按钮。
