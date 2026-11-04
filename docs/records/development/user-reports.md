# 输入与编辑问题排查

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-user-test-fixes"></a>
<a id="record-user-test-fixes-九项问题"></a>
<a id="record-user-test-fixes-2026-09-20时间轴框选与骨骼登记点击"></a>
<a id="record-user-test-fixes-2026-09-20首帧固定与中文时间轴文字"></a>
<a id="record-user-test-fixes-2026-09-20选中骨骼后无法移动"></a>
<a id="record-user-test-fixes-2026-09-20切到非零帧后不能移动"></a>
<a id="record-user-test-fixes-2026-09-20帧范围物理菜单与遗漏汉化"></a>
<a id="record-user-test-fixes-崩溃证据与-d3dx-abi"></a>
<a id="record-user-test-fixes-运行方式"></a>
<a id="record-user-test-fixes-验证结果"></a>
<a id="record-user-test-fixes-2026-09-22相机物体视图跳变视图轴颜色与坐标系标识"></a>
<a id="record-user-test-fixes-2026-09-22续hud-文字的语言切换与坐标系标识位置"></a>
<a id="record-user-test-fixes-2026-09-22续视图坐标系旋转相机视角锁定条件pmm-文件对话框取消"></a>
<a id="record-user-test-fixes-2026-09-23坐标系标识的措辞字体与相机第三态"></a>
<a id="record-user-test-fixes-2026-09-23续坐标系标识的中日英字体一致性与描边清晰度"></a>
<a id="record-user-test-fixes-2026-09-23续二标识的放大模糊与无配件时的选择器范围"></a>
<a id="record-user-test-fixes-2026-09-23续三一次点击把坐标系推进两步"></a>
<a id="record-user-test-fixes-2026-09-23续四连点第二下被吞掉"></a>
<a id="record-user-test-fixes-2026-09-23续五配件坐标系是否被完全屏蔽"></a>
<a id="record-user-test-fixes-2026-09-23续六点多少次都不切换同一次按下被推进两次"></a>
<a id="record-user-test-fixes-2026-09-23续七初启动视角偏高切一帧才回到-mmd-的起始位置"></a>
<a id="record-user-test-fixes-2026-09-23续八相机与物体面板互切跳变自由视角枢轴"></a>
<a id="record-user-test-fixes-2026-09-23续九追踪模式下切到物体面板跳变停止播放后视角回退"></a>
<a id="record-user-test-fixes-2026-09-23续十关闭追踪时视图跳变"></a>
<a id="record-user-test-fixes-2026-09-24续十一用移动与旋转工具时视图漂移"></a>
<a id="record-user-test-fixes-2026-09-26续十二视口操作按钮改浅色配件态改用-x-标记"></a>
<a id="record-user-test-fixes-2026-09-26续十三快速双击操作按钮导致视角跳变"></a>
<a id="record-user-test-fixes-2026-09-26续十四拖拽操作按钮时行程不再受窗口边界限制"></a>
<a id="record-user-test-fixes-2026-09-26续十五配件态的-x-标记去掉黑色描边与残留阴影"></a>
<a id="record-user-test-fixes-2026-09-30续十六表示半透明显示的模型出现穿透边缘"></a>

按操作选择对应说明与回归入口，不以开发批次分类。

## 选择、登记与切换焦点

骨骼列表、3D 点选和时间轴选择应同步当前编辑目标。Shift 切换选择只处理一次；取消当前骨骼后仍有其他选中骨骼时，继续使用有效目标。切换帧清除未登记编辑状态，保留仍有效的骨骼选择。

从其他程序返回 MMS 不应将已松开的按键视为新按下。关闭文件对话框后仍按住的 Enter 不应登记姿态覆盖刚载入的动作。相关行为见[编辑机制](../animation/editing.md)；输入回归需经过实际消息与按键入口。

## 时间轴与复制粘贴

框选按帧列、轨道行处理，折叠组与普通轨道共用有效的命中范围。快速拖动、窗口外坐标、重绘和丢失鼠标捕获都需要保持选区一致。跨模型粘贴按骨骼、表情名称及同名顺序匹配，不用单纯数组位置推测目标。

负数帧、反转粘贴、首记录和区间选择的边界见[动画指南](../../guides/animation.md)。不能把正帧操作的结果直接当作负帧支持。

## 数值、语言与名称

数值输入在失焦或 Enter 时提交当前输入框，一次编辑对应一次历史。焦点接收方不是提交来源。模型名称和固定界面文案使用各自的编码及语言回退规则，见[语言与名称](../interface/languages.md)。

## 物理、材质与特效

物理骨骼绑定、显示帧及模型结构要按真实记录大小访问。材质不显示时分别检查模型 alpha、toon、MME 分配及 MMV 输出，不把所有异常归因于驱动。MME、MMV 的加载组合和实际 DLL 路径均需记录。

## 提交可复现的问题

附上构建档、程序版本或哈希、界面语言、素材及准确步骤。涉及画面时提供固定镜头和效果配置；涉及性能时说明动作、物理、预览尺寸及采样方法。不要把合成夹具通过、隐藏窗口检查或静态反汇编解释成已人工复现任意工程。

功能测试与工具用途见 [tests/README.md](../../../tests/README.md)。无法重复的问题保留为未确认，直到取得对应执行证据。
