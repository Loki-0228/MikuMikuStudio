# 校准缓存的安装目录隔离

日期：2026-10-23。基线：`6bf065b`。工作区：`D:/MMD-Next/MMS-abi-dev`。

## 实现

`skinning::InitializeAutomatic` 原先在本地缓存未命中时读取固定 `LOCALAPPDATA/MikuMikuStudio/skinning-MMS-v1.bin`，即使迁移写盘失败也继续使用该缓存。现仅读取实际 EXE 所在目录的 `data/processor-config-MMS-v1.bin`。路径共用 `local_data::File`，不依赖 cwd、TEMP 或用户配置目录，也不提供旧目录自动回退。

有效本地缓存仍直接加载。缺失或损坏时继续原校准流程；初始化门禁、CPU 回退、校准算法、缓存版本及硬件身份校验不变。原来只有旧目录缓存的安装需要重新校准，首次启动可能因此耗时增加。旧文件不删除。该文件存储生成的测量结果，不是用户设置；用户设置仍统一于 `data/settings.json`。

DSH 子任务 `d43a870d-1843-453e-bc7f-13c53e288851` 仅修改两个生产文件，实际 cwd 为本 ABI 工作区。正式结果为 `completed`、`sawError=false`；Codex 完成测试、独立审查、构建及验证。结果和差异保存在 `build/verification/abi-calibration-data-20261023`。

## 本机验证

最终输入由 `candidate-03/manifest.json` 固定，产品及专项测试副本保存在其 `products` 目录。完整配置和原始兼容配置均构建成功。既有硬件测试只在完整配置构建，没有运行。

`skinning_calibration_storage_tests --storage <新建绝对输出目录>` 仅在带严格安装标记的复制安装中运行。实际调用生产 `InitializeAutomatic`，GPU 库存由测试设为空，完整 typed COM 设备拒绝首次真实顶点缓冲分配。测试不创建 D3D 设备、不枚举 GPU、不执行硬件测速。旧缓存见证文件位于测试自建的假 LOCALAPPDATA，未访问真实用户配置。

两配置 `checks-full-01` 和 `checks-original-01` 各通过 154 项检查：

- 本地缺失、损坏或 data 被文件占用时，不采用有效旧缓存；真实校准路径到达首个缓冲申请并处理失败。
- 本地有效缓存含六组不同计时，逐项核对实际加载内容，并确认没有启动缓冲分配。
- 空参数、显式 CPU 模式及重复初始化保持原门禁。旧缓存、损坏缓存及阻挡文件字节保持，cwd 与 TEMP 无回退文件。
- 调用实际 `UpdateModelVertexBuffers`，由自有内存缓冲执行 CPU 蒙皮。验证完整 Lock/Unlock 和已知位移结果，区分未命中后的 CPU 回退与命中 CPU 配置后的直接选择。

两配置 CTest 独立安装入口 `skinning_calibration_storage` 也通过，见 `ctest-*-02`。所有运行串行继承 Default 桌面，无全局键鼠输入，不操作用户窗口。

保留了两个集成失败：首次原始配置构建找不到新测试目标，因为 EFFECT_TESTS 关闭；现将目标移入两配置均启用的 UI_TESTS，不开启产品功能。首次 CTest 的 `file(WRITE)` 产生 CRLF，严格安装标记拒绝；现用 `file(CONFIGURE ... NEWLINE_STYLE UNIX)` 固定 LF。失败日志未覆盖。

`pe-01.json` 核对两产品为 x64、子系统 6.1，导入表没有增减，117／118 资源保持。完整产品 SHA256 为 `d2c5283615fc7dcde3d1a3825cfa8033ad4f42e796f7829fa80705adb0c84453`；原始兼容产品为 `c50b66e4546a0ed7f4678eb30357e25dded3b26a7df874433338c5cf2ef23517`。共享校准代码在两配置中均修正，原始产品其他功能开关不变。

正式 release 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。轮次开始及验收时 main 为 `fed9768f9fe726c4b5e068849808803f1713ad42`，本批没有修改 main 或覆盖正式产物。

## 证据边界与后续

失败夹具没有生成六组完整测量，Save 在参数门禁返回，因此不能据此声称完整校准结果写盘失败后仍可用的行为已验证。EXE 定位失败、ACL、磁盘满、GPU 身份重映射、真实硬件测速、Win7 和 AMD 真机仍未在本批验收。CPU 路径依赖现有 D3DX 数学运行库。没有运行旧生产代码配合新增断言的反例。

本批证明该入口停止自动借用固定用户目录缓存，不代表所有运行数据路径已经完成审计。磁盘缓存管理和其他 ABI 功能继续保留。main 骨骼读数、重置草稿、整体重置语义、手动编号保存、菜单及长输入显示的未同步行为已补充至[迁移计划](abi-main-sync-plan-2026-10-22.md)，当前只有静态核对证据，尚未实现。普通公共编辑服务继续关闭。
