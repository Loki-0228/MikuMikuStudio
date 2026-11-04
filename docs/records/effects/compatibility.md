# MME 对象、分配与兼容

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-mme-compatibility"></a>
<a id="record-mme-compatibility-材质名双向回退与时间轴编码2026-09-20"></a>
<a id="record-mme-compatibility-mme-主菜单汉化2026-09-20"></a>
<a id="record-mme-compatibility-问题与修复"></a>
<a id="record-mme-compatibility-二进制依据"></a>
<a id="record-mme-compatibility-实测结果"></a>
<a id="record-mme-compatibility-ray-工程重载与语言切换2026-09-17"></a>
<a id="record-mme-compatibility-修复"></a>
<a id="record-mme-compatibility-验证素材与边界"></a>
<a id="record-mme-global-assignment"></a>
<a id="record-mme-global-assignment-运行环境核对"></a>
<a id="record-mme-global-assignment-现场对照"></a>
<a id="record-mme-global-assignment-诊断覆盖"></a>
<a id="record-mme-global-assignment-操作入口审查"></a>
<a id="record-mme-global-assignment-动画纹理生命周期对照"></a>
<a id="record-mme-global-assignment-候选修复验收"></a>
<a id="record-mme-global-assignment-证据与限制"></a>
<a id="record-goldboom-mme-reassign"></a>
<a id="record-goldboom-mme-reassign-覆盖的操作"></a>
<a id="record-goldboom-mme-reassign-当前结果"></a>
<a id="record-goldboom-mme-reassign-后台环境与证据限制"></a>
<a id="record-goldboom-mme-reassign-本地记录"></a>
<a id="record-emm-autosave"></a>
<a id="record-emm-autosave-根因与修改"></a>
<a id="record-emm-autosave-验证结果"></a>
<a id="record-emm-autosave-本地产物"></a>
<a id="record-mme-registration-crash"></a>
<a id="record-mme-registration-crash-验收"></a>
<a id="record-mme-registration-crash-证据与复现"></a>

MME 另行安装，入口见[MME 使用指南](../../guides/effects.md)。宿主保留原生 DX9 接口，不能将菜单出现或 DLL 加载当作任意效果兼容的证明。

## 对象登记

MMHack 通过资源创建时的稳定输出槽建立对象映射。模型索引缓冲与附件网格写入各自记录中的稳定位置；导出对象 ID 按原接口返回地址低 32 位，不能用完整指针或其他资源地址替代。

每帧进入 `BeginScene` 前重置登记状态，只放行有效的当前对象查询。PMM 与模型打开路径通过公开文件接口通知 MME，不加载旧 CRT 或按私有偏移修改 DLL。

原生网格缓存代理与登记资源分开管理，结构变化时同步维护材质数量和已有分配。当前 MMV4MMS 独立接管场景，不与 MME 代理同时启用，见[宿主接口](../extensions/host.md)。

## 材质名与菜单

材质名称按首选语言读取，首选为空时查询另一种已有名称，两者都为空才使用默认标签。兼容层根据已加载模块的具名导入/导出定位查询，条件不满足时跳过，不强制加载插件或改写 DLL 文件。

菜单翻译按命令 ID 更新文字，保留勾选、禁用、快捷键及未知扩展项。语言切换后重新查找有效菜单句柄。

## EMM 保存

PMM 保存工程，EMM 保存效果分配。「随 PMM 保存」控制自动写入；关闭时保留已有 EMM，重新开启后按当前状态保存。另存为和 Unicode 路径也必须保持对象、分配与目标文件对应。保存成功不能仅以文件存在判断，还需检查实际效果分配。

## FX 重新分配与共享状态

同一效果的多次实例化可能共享全局纹理或中间状态。重新分配后模型消失、地面阴影异常或材质变黑时，记录实际 MME 版本、效果、分配顺序及工程，区分对象映射失效和 FX 内部共享行为。

针对某个 toon、GoldBoom 或后处理样本取得的结论不代表所有第三方效果都已修复。未知原报告保留为需复现，不能拿相似样本通过替代。

## 自动重载与缓存

已识别版本使用文件通知减少无变化时的时间戳查询；通知不可用时回到原查询。include 依赖重载范围遵循 MME 原有机制。编译结果复用、纹理准备及磁盘管理见[资源缓存](../performance/startup.md)。

## 验证组合

分别检查无 MME、有 MME、插件冲突检测、重新分配、保存重开和设备重置。确认日志中的实际模块路径和当前着色输出，不使用固定旧哈希声明当前安装已验证。运行器与回归入口见 [tests/README.md](../../../tests/README.md)。
