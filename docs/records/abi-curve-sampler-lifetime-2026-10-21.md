# 曲线普通采样的会话保护（2026-10-21）

曲线绘制、速度关键点、范围查询、命中及 Bake 的普通采样现共用读取批次。每个参与通道的原生采样闭包在一个批次内只复制一次；复制、执行和最终释放均受原会话保护。没有原生采样器时保留 CPU 曲线计算，原生采样失败时停止该次操作，不改用另一条曲线掩盖失败。

## 实现范围

`SamplingBatch` 按实际 Channel 地址缓存闭包，保活原 Session 和 SourceStates。缓存仅在首次原生采样时构造，纯 CPU 读取不创建空 map。通道集合替换、历史恢复和拖拽恢复推进内部代次，旧读取不能继续使用新通道。`WithSamplingSlice` 在正常及异常路径显式结束批次；工作函数先退出并展开内层读取，再清理最外层缓存。最终释放导致关闭或替换窗口时，退休异常优先于原采样异常。

`PrepareCanvas` 先准备比较曲线、活动曲线、关键点、手柄和范围条的纯数值几何。所有采样闭包及存储保护释放并验代后，才创建绘制 DC、选择字体、分配绘制表面或进入 GDI。输出型查询也先结束读取，再写回调用方参数。采样阶段失败的打印不会把背景、回退曲线或部分几何写入调用方位图。

Handle 拖拽仍在原位预览。回调中取消焦点或捕获时，宿主暂存原始关键帧，保持回调正在借用的预览数组地址。最外层读取结束后，仅对同会话、同来源、同代次的准确通道恢复原值，并废弃旧几何结果。重开会话会丢弃旧恢复记录，不能把旧预览写入新来源；取消不提交编辑或增加历史。

回调中的嵌套 `WM_PAINT` 仅验证当次区域，并保留对应会话和 HWND 的补绘请求。保护退出后合并 `InvalidateRect`，不递归执行 `UpdateWindow`。回调及闭包释放前后比较原生尺寸、绘图区、DPI、模式、范围及拖拽偏移；变化会使旧结果失效，后续读取重新取得当前几何。

Bake 先复制完整私有通道，再通过同一缓存准备输出轨道。缓存释放后，在保护内清除私有原生采样器，重新验代，最后只提交一次。成功提交前还会读取宿主数据核对来源。本次单通道夹具分别核验私有载荷、采样缓存和预检读取各复制一次，这不是任意来源的固定总复制数。拒绝及准备失败保留 redo，接受后的显示恢复失败不会重试 apply。

## 专项证据

证据位于 `build/verification/abi-curve-sampling-20261021`。窗口测试串行继承 Default 桌面，只操作自有窗口；三个补绘用例明确显示自有窗口，其余夹具也可能自行显示窗口。没有读取桌面像素或发送全局输入。

| 检查 | 结果 |
| --- | --- |
| 修复前安全反例 | 原实现绕过闭包复制，预设复制失败时仍执行 live body，轴及打印按采样点重复直接调用。[原报告](../../build/verification/abi-curve-sampling-20261021/copy-red-full-01/report.json)。该阶段不关闭或替换窗口；当时尚未加入整幅打印哨兵检查。 |
| 候选 03 复制边界 | 完整配置独立模式 56 项通过。[报告](../../build/verification/abi-curve-sampling-20261021/initial-full-03/report.json)。失败时调用方输出及整幅 DIB 保持原值，成功轴和两种宽度打印实际覆盖两个通道。 |
| 候选 05 完整配置采样寿命 | 5,517 项，退出 0。[报告](../../build/verification/abi-curve-sampling-20261021/green-full-05/report.json)。 |
| 候选 05 原始兼容配置采样寿命 | 5,517 项，退出 0。[报告](../../build/verification/abi-curve-sampling-20261021/green-original-05/report.json)。 |
| 候选 05 完整配置补绘与几何 | 独立 621 项，退出 0，已包含在寿命专项中。[报告](../../build/verification/abi-curve-sampling-20261021/green-full-05/report.json)。 |
| 候选 09 完整配置曲线全集 | 35,968 项，退出 0，包含采样寿命及全部原有默认回归。运行前后 EXE 哈希均与冻结候选一致。[报告](../../build/verification/abi-curve-sampling-20261021/final-full-09/report.json)。 |
| 候选 09 原始兼容配置 | 独立采样寿命 5,517 项、曲线全集 35,968 项，均退出 0。两次运行前后 EXE 哈希均与冻结候选一致。[报告](../../build/verification/abi-curve-sampling-20261021/final-original-09/report.json)。 |

135 种退休组合覆盖五个入口、复制／执行／最终释放、关闭／同 HWND 替换／重建，以及正常／运行异常／内存不足响应。只读缓存的最后拥有者和 Bake 私有载荷的最后拥有者分别核验，没有把两者混记。另有 24 个真实 Handle 预览取消及替换用例、嵌套操作拒绝、错误不回退、失败保 redo、接受后恢复失败、补绘重新采样和同 HWND 尺寸变化。

最终释放的异常组合通过采样函数抛错后展开实际拥有者触发，析构函数本身不抛异常。

位图像素只读写测试自有 DIB，并在 CPU 访问前调用 `GdiFlush`。补绘用例证明后续 `WM_PAINT` 重新执行采样，独立打印证明自有 DIB 发生绘制；这些断言不能代替最终屏幕像素或真实鼠标验收。

## 保留的失败与修正

首轮完整配置先通过复制边界和 678 项 Handle 检查，但补绘用例在隐藏窗口上要求更新区域存在，整组又误点不存在的 1005 号刷新按钮，均以退出 1 保留于[首轮报告](../../build/verification/abi-curve-sampling-20261021/initial-full-03/report.json)。

保留断言后增加诊断，得到隐藏窗口 `InvalidateRect=1`、`GetUpdateRect=0`、父子均不可见。该条件在武装回调前的普通失效操作中也成立。成功 Bake 的全操作计数还漏算了一次真实提交预检复制，见[诊断报告](../../build/verification/abi-curve-sampling-20261021/diagnostic-full-04/report.json)。随后仅让三个补绘用例显示窗口，保留更新区域及实际再次采样断言；移除不存在的按钮点击，保留来源通知路径；成功 Bake 分别核验两次私有准备和一次预检复制。原 135 种退休矩阵未改。

各阶段二进制、PDB、源码和 CMake 分别冻结于 `candidate-03`、`candidate-04`、[专项候选 candidate-05](../../build/verification/abi-curve-sampling-20261021/candidate-05/manifest.json)和[注册默认回归后的 candidate-06](../../build/verification/abi-curve-sampling-20261021/candidate-06/manifest.json)。早期 `candidate-01` 只有安全复制边界通过，不能冒充后续修正及寿命专项验收。上述候选均不能替代继续修正后的候选 09。

注册默认回归后的完整配置全集在[首次整合报告](../../build/verification/abi-curve-sampling-20261021/final-full-06/report.json)以退出 1 结束。空选区读取后的删除按钮未禁用。检查发现 `PullKeySelection` 的回调保护覆盖到了后续 `UpdateKeyFields`，新采样保护将这段内部刷新误判为递归采样。该结果属于实际生产回归，不能归为夹具或此前堆故障；后续修正需要重新验收选区及完整曲线行为。

随后将选区回调保护收束到读取阶段，退出保护并释放存储、验代后再更新界面。独立选区专项 5,158 项通过，但[第二次整合](../../build/verification/abi-curve-sampling-20261021/final-full-07/report.json)又在既有的无分配 Handle 取消断言退出 1。MSVC 的空 `std::map` 也会分配内部节点，原生采样缓存尚未使用就消耗了注入的分配失败，导致普通取消中途停止。修正需要让缓存延迟至首次原生采样时才构造，保留原无分配取消断言。两项均属于本批引入后由全集发现的实际回归；[候选 candidate-07](../../build/verification/abi-curve-sampling-20261021/candidate-07/manifest.json)保留对应源码及产物。

缓存随后改为延迟构造的 `optional<map>`，纯 CPU 读取不创建它。无分配取消及原有断言均保留。第 08 轮过早冻结快照，当时两个构建句柄尚未确认结束，冻结的四项 EXE／PDB 与结束后的产物哈希不同，见[核验结果](../../build/verification/abi-curve-sampling-20261021/candidate-08-current-check.json)。该快照不作为源码与产物配对证据；[该轮报告](../../build/verification/abi-curve-sampling-20261021/final-full-08/report.json)中的全集 240 秒超时也保留，未认定原因。

构建终态确认后重新冻结[候选 candidate-09](../../build/verification/abi-curve-sampling-20261021/candidate-09/manifest.json)。运行器新增启动前后 EXE 哈希，核对同一候选再复验。两套配置全集各通过 35,968 项，不用它解释或删除第 08 轮超时。两套注册清单均含独立 `animation_curve_sampler_lifetime`，超时 180 秒，同时默认全集调用该组，见[注册核验](../../build/verification/abi-curve-sampling-20261021/registration-07.json)。完整配置构建保留了 MSB8028 中间目录警告，目标构建退出 0；没有把日志写成零警告。

## 边界与后续工作

本批没有修正独立原生插帧 `SampleInsertionChannel` 的同会话尺寸变化问题。该路径已经持有私有通道并保护会话，但还需校验命中准备期间的几何变化，确保变化时不提交插帧。不能将本批普通采样保护扩大为全部插帧几何已完成。

原始兼容全集此前的[堆链表故障](abi-curve-heap-observation-2026-10-21.md)仍未归因，寿命专项通过不关闭该故障。Windows 7 SP1 x64、CPU 基础路径及 AMD 要求保持，本批不改文件格式、VMD 标准字段、公共 C ABI、设置目录或正式 release。

两套产品构建通过。PE 均为 x64、子系统 6.1，无新增普通或延迟导入。完整产品减少 `GdipAlloc`、`GdipCloneBrush`、`GdipFree` 三项 GDI+ 导入，不能表述为导入表完全一致；原始兼容产品和正式 release 哈希保持原值，见[产品清单](../../build/verification/abi-curve-sampling-20261021/pe-products-final-09.json)。严格导入相等检查最初因这三项减少而退出 1，随后单独保存[逐项差异](../../build/verification/abi-curve-sampling-20261021/pe-import-diff-03.json)，按无新增依赖及准确减少项核验。本机未完成 Win7、AMD、IME 或多 DPI 真机验收。
