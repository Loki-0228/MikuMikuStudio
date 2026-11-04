# 模型载入与资源准备

<!-- 保留已有文档链接的定位入口。 -->
<a id="record-allstar-empty-model"></a>
<a id="record-pmx-loading"></a>
<a id="record-pmx-loading-follow-up-skip-hang-fixed"></a>
<a id="record-pmx-loading-change"></a>
<a id="record-pmx-loading-measurements"></a>
<a id="record-pmx-loading-correctness-checks"></a>
<a id="record-pmx-optional-fields-mmv-screenshots"></a>
<a id="record-pmx-optional-fields-mmv-screenshots-实测结果"></a>
<a id="record-pmx-optional-fields-mmv-screenshots-已修复的问题"></a>
<a id="record-pmx-optional-fields-mmv-screenshots-验证与复现"></a>

## PMD 与 PMX

PMD、PMX 可用于动画场景，原生模型编辑页支持 PMD。载入校验文件头、短读、计数、索引、骨骼层级、表情、刚体和 joint 引用；结构可解析不等于能按旧格式无损保存。

PMX Unicode 名称用于界面、绑定及支持的完整名称扩展。最小包也可导入部分超出旧编码或字段限制的模型并提示，但保存时仍按目标格式检查。具体边界见[文件指南](../../guides/files.md)及[旧版格式限制](../../reference/legacy-mmd-limits.md)。

## 绑定与求值

PMX 快照按权重类型读取有效槽，BDEF2 与 SDEF 的第二权重使用 `1-w`，不能把文件未存储的权重当零。骨骼表情和隐藏面板编号分别检查边界，合法组合表情仍参与求值。

模型重载、删除及结构变更更新资源身份和关联缓存。MME 登记资源使用稳定输出槽；MMV 的帧输出不覆盖宿主原始定义。

## 贴图与 Toon

纹理按模型路径解析，共享 toon 根据文件名匹配槽位，不能直接把 PMD 文件表下标当共享 toon 下标。主贴图、sphere、toon 及无贴图材质保持各自语义，见[材质说明](../rendering/materials.md)。

准备缓存只复用有完整身份和失效检查的结果。磁盘纹理缓存与模型内存几何缓存不同，细节见[资源准备](../performance/startup.md)。

## 大模型与失败处理

容量按实际内容及安全上限扩展；失败不接受部分结构。PMX 并行准备仅处理符合条件的临时 CPU 数据，不在工作线程操作 D3D9 或共享模型缓存。文件读取和资源提交保持受控顺序。

## 检查入口

模型导入、名称、toon、隐藏表情、权重及 PMX 文本测试见 [tests/README.md](../../../tests/README.md)。固定样本的[载入数据](data/pmx-loading.json)属于原始测量，不能作为任意当前模型的速度保证。
