# 公开同源材质数值 1.6（2026-10-27）

完整配置的 `mms.scene.evaluate` 1.6 追加材质能力查询、显式材质场景来源、来源材质 ID 和帧材质批量读取。材质 ID 与 `mms.scene` BIND 的材质元素身份一致；source 和 frame 共用同一冻结身份图，但各自按完整身份图收费。旧 1.5 来源仍不启用材质输出，1.6 来源继续支持 1.5 网格与 1.4 骨架函数。

结果分别提供原生 diffuse RGB、表情合成的 diffuseMirror RGB 和 alpha、镜面、环境、边缘数值，以及纹理、球面、Toon 的加法和乘法。PMD 为静态数值，纹理混合默认为加零、乘一。帧在 source 和原模型销毁后可独立读取；批量上限为 4096，失败保持整个输出。`EvaluationMaterialServices`、`EvaluationMaterialSource`、`EvaluationMaterialFrame` 复用同一 C 句柄；具体使用见 [SDK 文档](../../sdk/mms/evaluation.md#16-同源材质数值)。

新增内部结构使 1.4/1.5 来源及帧的固定元数据计费略增，严格贴合旧版额度的调用可能返回内存不足；未宣称旧版精确字节数不变。原始兼容配置的生产服务仍不开放，本批双配置 mock 和 DLL 测试不改变该范围。

## 验证

源码在最终构建前冻结并在测试后复核，证据保存在 `build/verification/abi-public-material-20261027/`。完整和原始兼容配置产品构建通过。

- 双配置场景契约各 805 项、C++ SDK 各 550 项、真实 DLL 生命周期各 676 项通过；覆盖版本前缀、身份、空读/越界、输出保持、失败注入、额度、停用及读取在途期间释放。
- 完整 ABI 套件：完整配置 44,818 项，原始兼容配置 44,806 项通过。
- 真实插件在宿主实际加载 PMX 和 PMD：C 接口和 C++ SDK 的材质 ID 与 BIND 一致；PMX 双材质及组、加减乘结果与原生对照，PMD 静态材质与中性 tint 匹配；释放 source 及删除模型后帧仍可读取。QDEF 仅由运行时类型注入验证拒绝，不是 PMX 2.1 文件测试。

结果不包含纹理文件或采样、灯光、解析后的 Toon 色、边缘渲染、独立物理和最终图像；工作区额度不是进程峰值的硬上限。Win7 SP1 x64 和 AMD 真机未验收。

完整配置 EXE SHA256：`64dca8d370ae649fa704cdba506c663123dcc6ec8e2ca21704fdf793bbb357fc`；原始兼容配置：`94153fffbd3d117e2f2bdc749a23db82d535a9784bf5a1c34c625308156ca041`。稳定版 release 未覆盖，SHA256 仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a`。main 本轮为 `d551f1f`，未同步新的阴影修改。
