# MME 分配快照的安装目录隔离

日期：2026-10-23。基线：`e5090f9`。工作区：`D:/MMD-Next/MMS-abi-dev`。本批改变宿主内部快照的存储位置，不发布或复制第三方插件。

## 实现

`native_mesh::PreserveEffectAssignments` 原先在系统 TEMP 创建 `MMS-Mesh-<PID>-<tick>-<attempt>`。现从 `local_data::File(L"mme_mesh_snapshots")` 获取实际 EXE 目录下的根路径，创建目录失败或无法定位 EXE 时直接拒绝，不回退其他位置。

原来的独占子目录创建循环保持。每次保存的 `assignments.pmm/.emm` 和后续编辑的 `baked.pmm/.emm` 仍在同一子目录。菜单自动保存状态的恢复、通知顺序、注册绘制流程和异常处理不变。`ClearScene`、`Shutdown` 不新增删除快照的操作；这些文件可能仍被效果运行时引用，不能纳入普通可再生缓存淘汰。

DSH 子任务 `f0b93c67-bdf8-40eb-bd70-c6bb6449e777` 仅修改 `src/render/native_mesh.cpp`，实际 cwd 为本 ABI 工作区，正式结果为 `completed`、`sawError=false`。Codex 编写测试、独立审查、构建、运行和整理证据。

## 本机证据

证据目录为 `build/verification/abi-mme-data-20261023`。`candidate-01/manifest.json` 固定生产及测试输入，两套产品构建成功，完整配置的隐藏宿主测试构建成功。原始兼容产品继续排除增强网格模块，没有启用生产快照功能。

`checks-mme-01` 在独立复制的 EXE 中启动真实隐藏 D3D 宿主并加载三角形模型。cwd 与 TEMP/TMP 使用安装目录外的测试目录，TEMP 和 TMP 共用同一临时目录。测试没有加载 MME、MMHack 或 MMV，没有全局输入或视觉验收。

测试仅在当前线程、指定调用期间伪装最初两次 MMEffect 模块门禁；后续查询全部转发真实函数。保存及打开通知由受控边界接收，真实 `DisableMeshEffects`、快照目录创建、PMM 占位写入、INI 编辑及 `DrainEffectRegistration` 均执行。

已验证以下行为：

- 自动保存原先关闭或启用时，保存及打开各执行一次，结束后恢复原菜单状态。
- 两次成功调用各产生独立目录，原分配不改，选中对象及其材质和离屏分配改为 `none`，其他分配保持。
- 缺少自动保存命令时，在创建目录前拒绝；根目录被自有普通文件占据时，在切换菜单或通知前拒绝，阻挡文件字节保持。
- 保存通知未生成 EMM 时返回实际错误，不打开编辑映射，并恢复自动保存状态。
- 后续操作及 `ClearScene` 后，先前快照路径和字节保持。没有在 cwd 或 TEMP 建立快照目录。

宿主退出后，外层再次检查实际目录。`post-exit-files.json` 记录两个完整快照各四个文件，以及保存失败留下的一个 PMM 占位文件。共九个文件的哈希保留，未递归删除证据。失败遗留占位文件是既有行为，本批没有添加自动回收。

`checks-bake-01` 使用同一测试 EXE 回归上一批真实三帧烘焙，成功、取消、目录阻塞和用户指定 ABC 输出均通过。没有重复未受影响的全部 ABI 契约。

`pe-01.json` 显示两产品为 x64、子系统 6.1，无新增导入；完整产品移除了已无调用的 `GetTempPathW` 导入。117／118 资源保持。原始兼容产品仍为 `2036d95309478577fe28ee2a6c5be5f83eedf7e57f70c8a1414069b1772a13c9`，正式 release 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。轮次开始及收尾核对的 main 仍为 `fed9768f9fe726c4b5e068849808803f1713ad42`，无新增提交。

## 限制与剩余事项

受控通知不证明真实 MMHack 队列、MME 消费、效果恢复或第三方资源寿命。Object 夹具使用模型绝对路径；当前 `DisableMeshEffects` 对裸相对值的 `filesystem::equivalent` 依赖 cwd，未显式以 EMM 父目录解析，仍需单独处理。旧 MME 的编码及路径长度约束未在本批验收。

未覆盖整个 data 根被占据、EXE 定位失败、ACL、磁盘满、打开通知或注册绘制异常后的恢复，也没有旧生产代码配合新增断言的运行反例。Win7 和 AMD 真机验收仍缺。

搜索直接临时目录调用已不再命中这批生产入口，但不据此宣称所有运行数据隔离已完成。`skinning_backend::InitializeAutomatic` 仍会在本地校准缓存未命中时自动读取 `LOCALAPPDATA/MikuMikuStudio` 的 LegacyPath，并在迁移写入失败时继续使用其结果。这是跨安装自动读取的独立缺口，需另行修改和验收。缓存管理及其他 ABI 需求继续保留，普通公共编辑服务没有开放。
