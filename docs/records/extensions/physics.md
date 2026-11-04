# 可选原生形变与物理模块

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mmv-deformer-interface-contract"></a>
<a id="record-mmv-deformer-interface-contract-会话分工与基线"></a>
<a id="record-mmv-deformer-interface-contract-循环与数据可见性"></a>
<a id="record-mmv-deformer-interface-contract-原生计算模块接口"></a>
<a id="record-mmv-deformer-interface-contract-输入与输出字段"></a>
<a id="record-mmv-deformer-interface-contract-稳定身份与结构修改"></a>
<a id="record-mmv-deformer-interface-contract-坐标与-joint-同步"></a>
<a id="record-mmv-deformer-interface-contract-跨帧状态与原子提交"></a>
<a id="record-mmv-deformer-interface-contract-fx-接入和查看窗口"></a>
<a id="record-mmv-deformer-interface-contract-校验降级与零层特例"></a>
<a id="record-mmv-deformer-interface-contract-接口验收与交付"></a>
<a id="record-mmv-bullet3"></a>
<a id="record-mmv-bullet3-按模型或骨骼启用-bullet3"></a>
<a id="record-mmv-bullet3-排查分配位置错误"></a>
<a id="record-mmv-bullet3-参数与状态"></a>
<a id="record-mmv-bullet3-模拟与限制"></a>
<a id="record-mmv-bullet3-构建与验收"></a>
<a id="record-mmv-bullet3-将计算-dll-放在-fx-文件夹"></a>

## 模块与宿主边界

MMV4MMS 可通过插件内部的原生模块处理形变和物理。MMS 提供只读场景及物理输入，继续拥有原生模型、关键帧和世界。主程序包不附带 Bullet3、AVBD、DDM、SDS 或 VBD 算法 DLL，声明名称也不等于已安装对应实现。

C ABI 字段见[mmv_deformer.h](../../../sdk/mmv4mms/include/mmv/mmv_deformer.h)，详细数据约定见[FX 接口](effects.md#record-mmv-fx-原生结构与状态循环)。

## 输入、候选与历史

`Source` 为宿主原始定义及当前动画输入，`Upstream` 为本层冻结结构，`PreviousCommitted` 为最近成功输出，`Candidate` 为本次私有候选。求解失败或校验失败丢弃候选，不能只回退位置而保留已推进的隐藏物理状态。

结构使用稳定键、版本和显式引用，新增骨骼、绑定、刚体及 joint 需要完整重映射。矩阵按行主序、行向量，网格为对象空间，骨骼与刚体为世界空间。缺少所需蒙皮前几何时报告能力不足，不把绑定姿态当当前表情结果。

## 模块加载

计算 DLL 随 FX 放在同目录或子目录，`MMVDeformerLibrary` 相对入口 FX 解析。缺失或 ABI 不符报告实际路径错误，不到宿主其他目录寻找同名模块。当前主插件 `MMV4MMS.dll` 仍位于 EXE 旁。

模块通过 `evaluate` 生成私有候选，由 `commit` 或 `discard` 结束；输入指针只在调用内有效，不能跨 DLL 转移 STL、异常或借用指针的所有权。

## Bullet3 使用约定

配套 Bullet3 FX 可分配给模型整行或关联骨骼，不适用于普通材质子行。模型需有效刚体，骨骼范围只选择实际关联项，缺失数据不自动扩大成全模型范围。

物理启停、重力倍率、迭代数、固定频率、地面碰撞及编辑时模拟属于插件参数。默认值和支持形状以安装的效果描述和计算模块为准，不从旧版截图推断。避免两个求解器同时回写同一状态。

固定步进、向后跳帧重建、共享组碰撞和私有历史由模块实现。QDEF、额外形状、非对角惯量及跨对象连接等能力须显式检查，不能静默用近似数据冒充支持。

## 烘焙与性能

实时输出不自动写入原生关键帧。头文件中的层采样和烘焙接口还需要对应运行时与宿主路径接通，不能仅凭结构定义承诺任意分层结果可无损烘焙。

模块正确性、实际插件组合、连续播放及性能分别验证。CPU 直接输出不等于 GPU Bullet3；没有对应交替基准时不报告固定加速比。验证方法见[插件测量](performance.md)。
