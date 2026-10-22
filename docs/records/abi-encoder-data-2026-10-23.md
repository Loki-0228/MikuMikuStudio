# 编码器探测文件的安装目录隔离

日期：2026-10-23。基线：`e20df04`。工作区：`D:/MMD-Next/MMS-abi-dev`。本批仅迁移 H.264 设置探测文件，不改变用户指定的 MP4 导出路径。

## 行为与边界

`CreateEncoderProbe` 原先用 `GetTempPathW` 和固定长度缓冲区准备内部 MP4。现通过已有 `local_data::File` 生成实际 EXE 所在目录下的 `data/encoder_probes`，创建目录后将完整宽字符路径传给 Media Foundation。无法定位 EXE 或创建目录时直接失败，不回退 TEMP、cwd 或用户配置目录。

文件名仍为 `MMS-encoder-probe-<PID>-<tick>.mp4`。`CloseEncoderProbe` 仍先释放编码器目标，再释放 Writer，最后删除文件。`CreateEncoderProbeGuarded` 的 SEH 边界和 `AbandonEncoderProbe` 不释放、不删除的保护保持原样，避免故障编码器在释放时再次触发异常。异常遗留文件不能据此自动删除。

本批没有解决原有 PID 加时钟命名的严格并发唯一性，没有新增缓存淘汰，也没有修改探测失败后的硬件降级规则。原始兼容产品仍排除增强编码入口，其独立测试编译该模块用于回归，不表示启用了生产 H.264 功能。

## 实现与验收

DSH 子任务 `1c8a66af-2c13-4fa9-bb9b-fb1315254823` 仅修改 `src/app/mf_h264_recorder.cpp`，实际 cwd 为本 ABI 工作区。正式结果为 `completed`、`sawError=false`；Codex 编写测试、独立审查并运行验收。证据保存在 `build/verification/abi-encoder-data-20261023`，冻结输入为 `candidate-01/manifest.json`。

两套产品和专项测试均构建成功。运行继承 `Default` 桌面，每次复制测试 EXE 至独立标记安装，改名后使用不同的 cwd、TEMP 和 TMP。测试只 Hook 当前进程，不发送全局输入，不启动厂商 MFT。

| 检查 | 实测结果 |
| --- | --- |
| 完整配置路径专项 | 101 项通过，日志 `checks-storage-full-01`。 |
| 原始配置独立模块路径专项 | 101 项通过，日志 `checks-storage-original-01`。 |
| 完整配置原有启动回归 | 126 项通过，日志 `checks-default-full-01`。 |
| 原始配置独立模块启动回归 | 126 项通过，日志 `checks-default-original-01`。 |

路径专项直接调用真实 `CreateEncoderProbe`，仅替换尚未实例化编码器的 Writer 工厂。工厂核对实际 URL 和硬件属性，以 `CREATE_NEW` 创建测试文件后返回失败，验证生产清理确实删除该文件。EXE 查询失败、data 被文件占据、子根被文件占据均在调用工厂前拒绝，外部 cwd 和 TEMP 保持为空。

正常关闭用手工构造的 codec 和 Writer 检查释放顺序、删除时机及重复关闭。异常用例由工厂注入 SEH，经过真实 guard，再调用真实 Abandon，检查指针归零且未释放、未删除；测试随后仅清理自己创建且字节匹配的文件。没有用手工对象证明真实编码器启动成功，transform 对象释放也未覆盖。

原有回归覆盖启动失败后软件重试、输入像素格式及设置分组。此次未重复实际 MP4 编解码方向测试，像素处理代码没有变化。未运行旧生产代码配合新增断言的反例，不将源码差异描述成实测失败。

PE 核对见 `pe-01.json`：两产品均为 x64、子系统 6.1，导入 DLL 和函数没有增减，117／118 资源保持。原始兼容产品 SHA256 仍为 `2036d95309478577fe28ee2a6c5be5f83eedf7e57f70c8a1414069b1772a13c9`。正式 release 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。轮次开始及收尾核对的 main 均为 `fed9768f9fe726c4b5e068849808803f1713ad42`，没有新增提交；main 已知未同步的缓存管理等行为继续保留在清单。

## 尚未覆盖

没有 Win7 或 AMD 真机结果，注入异常不能替代驱动验收。目录失败只覆盖普通文件占位，未覆盖 ACL、磁盘满及底层写入故障。安全放弃路径可能保留文件和资源，未宣称异常路径无残留。

`native_mesh::PreserveEffectAssignments` 的 MME 分配快照仍待迁移。磁盘缓存管理及其他既定 ABI 需求继续推进，普通生产 `mms.edit` 保持关闭。
