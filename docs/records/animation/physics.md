# 物理状态、缩放与关节

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-physics-joint-pose-safety"></a>
<a id="record-physics-joint-pose-safety-最终行为"></a>
<a id="record-physics-joint-pose-safety-改动清单"></a>
<a id="record-physics-joint-pose-safety-修复前后对照"></a>
<a id="record-physics-joint-pose-safety-性能对照"></a>
<a id="record-physics-joint-pose-safety-回归命令"></a>
<a id="record-physics-joint-pose-safety-整合记录"></a>
<a id="record-repeated-rigid-scale-fix"></a>
<a id="record-repeated-rigid-scale-fix-修复"></a>
<a id="record-repeated-rigid-scale-fix-验证与产物"></a>
<a id="record-pmm-initial-physics"></a>
<a id="record-pmm-initial-physics-原因和修改"></a>
<a id="record-pmm-initial-physics-验证"></a>
<a id="record-pmm-initial-physics-版本与保留记录"></a>

## 原生模拟

原生刚体、约束、碰撞分组和骨骼回写组成物理预览。播放、手动切帧和编辑状态具有不同的推进语义；不能把重复绘制当作新的动画步。模型删除、重载及关闭按所有权顺序释放约束、刚体和世界资源。

物理开关关键帧独立于骨骼选择和未登记编辑标记。选择物理开启帧时遍历具名骨骼、表情和显示记录，不使用另一架构的指针或记录步长。

## 负帧边界

负帧用于编辑和存档，播放从非负范围开始。负帧重新求值和物理状态处理不能套用非负数组索引；尚未支持的操作在[动画指南](../../guides/animation.md)明确说明。

## 骨骼缩放继承

绑定骨骼的刚体随缩放更新碰撞形状、相对偏移和连接框架；joint 两端依据各自刚体变换求值。球、盒、胶囊的尺寸定义及缩放处理见[骨骼指南](../../guides/bone-scaling.md#刚体与-joint)。骨骼位置、缩放与求解器单位保持一致，不能只缩渲染网格。

IK 沿用原有链长，缩放不重新定义 IK 链。原生与外部求解器也不能同时互相覆盖同一骨骼状态。

## 烧录与回放

物理烧录在独立世界从第 0 帧顺序计算，目标模型以外的刚体仍参与碰撞。结果作为带禁用物理标记的关键帧提交；撤销与重做包含关键帧、标记及末帧，取消不提交部分状态。详见[烧录指南](../../guides/physics-baking.md)。

## 可选求解器

Bullet3 等 MMV 算法依赖另行安装的插件和计算模块，不是主程序包的原生物理替换项。作用范围、共享域、跨帧历史和模块错误见[MMV 物理接口](../extensions/physics.md)。

## 检查入口

物理初始化、缩放、joint、状态恢复及烘焙回归的目标见 [tests/README.md](../../../tests/README.md)。合成碰撞场景不能保证所有模型参数稳定；真实素材仍需在对应求解器和设备下核对。
