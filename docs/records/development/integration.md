# 宿主、插件与模块集成

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-review-merged"></a>
<a id="record-repair-batch1"></a>
<a id="record-repair-batch1-改动"></a>
<a id="record-repair-batch1-构建与来源"></a>
<a id="record-repair-batch1-正确性对照"></a>
<a id="record-repair-batch1-性能与内存"></a>
<a id="record-repair-batch1-遗留问题和排除记录"></a>
<a id="record-repair-batch1-保存位置"></a>
<a id="record-repair-batch2"></a>
<a id="record-repair-batch2-修复内容"></a>
<a id="record-repair-batch2-d设备丢失退出与录制"></a>
<a id="record-repair-batch2-e烘焙属性与缓存持久化"></a>
<a id="record-repair-batch2-f仿真快照生命周期"></a>
<a id="record-repair-batch2-正确性结果"></a>
<a id="record-repair-batch2-性能与体积"></a>
<a id="record-repair-batch2-构建及证据"></a>
<a id="record-repair-coordination"></a>
<a id="record-integration-build"></a>
<a id="record-integration-build-运行文件与目录"></a>
<a id="record-integration-build-已整合与验证"></a>
<a id="record-integration-build-保留的限制"></a>
<a id="record-local-display-compatibility"></a>
<a id="record-local-display-compatibility-verification"></a>
<a id="record-local-display-compatibility-local-artifacts"></a>
<a id="record-mmdmodern-new-dll-audit"></a>
<a id="record-mmdmodern-new-dll-audit-结论"></a>
<a id="record-mmdmodern-new-dll-audit-文件身份"></a>
<a id="record-mmdmodern-new-dll-audit-二进制差异"></a>
<a id="record-mmdmodern-new-dll-audit-三项移植机制"></a>
<a id="record-mmdmodern-new-dll-audit-新包的相机撤销缺陷与-pe-头异常"></a>
<a id="record-mmdmodern-new-dll-audit-相机撤销的进一步追踪"></a>
<a id="record-mmdmodern-new-dll-audit-错误写入的两个目标"></a>
<a id="record-mmdmodern-new-dll-audit-为什么清理没有消除错误"></a>
<a id="record-mmdmodern-new-dll-audit-第一次撤销如何到达非法复制"></a>
<a id="record-mmdmodern-new-dll-audit-修正位置与结论边界"></a>
<a id="record-mmdmodern-new-dll-audit-mms-源码核对"></a>
<a id="record-mmdmodern-new-dll-audit-证据与验证范围"></a>

## MMS 与 MMV 的职责

MMS 管理工程、原生动画、物理及最终渲染交接；MMV 是独立安装的可选插件。宿主使用仓库内的 [SDK](../../../sdk/mmv4mms/README.md) 编译，不读取相邻插件源码，不链接或打包外部插件。

跨 DLL 使用版本化的 C 接口、结构大小和明确的生命周期。不能仅因文件名相同而认定 DLL 与宿主兼容。详细交接与失败回退见[MMV 宿主接口](../extensions/host.md)。

## 文件与效果状态

PMM、EMM 和 VMM 各自负责工程、MME 分配和 MMV 状态。保存、另存、打开及结构重新登记时必须保持当前对象身份和效果分配对应；不能把测试临时 EMM 写回用户文件。

MME 材质数、对象顺序和已登记网格发生变化时，需要维护查询表及相关缓存。更新后须同时检查旧材质和新增材质的实际分配，DLL 加载成功不能证明画面正确。

## 编辑与烘焙事务

曲线与原生物理烘焙采用完整提交与回滚；无变化不创建撤销记录。当前 main 的 MMV 分层采样和删除事务不可用，不能从头文件中的接口推断已接通插件历史或可删除任意效果。

## 共享实现与构建档

共同的输入、保存、缓存、备份和 MME 接口分别检查 `full`、`mmd932`。扩展功能不得通过共享数据路径意外进入最小包保存结果；可选 DX12 缺失时仍需正常启动。

主程序文件名 `MikuMikuDance.exe`、原生 MME 导出及公共 SDK 契约属于兼容边界。内部命名整理不能顺带破坏二进制格式或插件调用。

## 外部适配包

MMS 不通过加载 MMDModern 的 `dsound.dll` 获得其功能。相似机制的源码实现不等同于复制该 DLL 的所有行为。对未能复现的外部崩溃，不能仅凭配置变化或相关代码已有保护就宣称修复。第三方 DLL 的静态数据流结论也不能直接推广为 MMS 运行结论。

## 组合验证

无插件、MME、MMV4MMS、冲突插件检测、不同设备及重置恢复分别覆盖。明确记录程序身份、实际加载路径、数据和输出；两个版本的性能比较应使用同条件交替采样。方法见[测试与验收](../../development/testing.md)及[帧基准](../../development/benchmarks.md)。
