# 纹理准备缓存消费者

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`，起始提交 `9125c06`。本批接入 main `3782e0c` 的纹理准备缓存与 CPU 解码 worker。

## 来源行为

main 的 `texture_prepare_cache`：

- 只包裹宿主现有的 `D3DXCreateTextureFromFileExW`（先调用宿主导入，MMHack 仍能看到原文件名并登记返回纹理）。仅缓存宿主标准请求：A8R8G8B8、MANAGED、默认尺寸、单层或自动生成 mipmap；手写源 mip 链与其他请求走原生。
- 键包含源内容、D3DX DLL 身份、适配器/驱动/能力、创建 flags 与 usage；磁盘存原生缩放后的像素与源元数据，不保留任何存活 GPU 对象。
- 磁盘：`<EXE>/Cache/Textures/<源文件名>/<key>.mtc`，全局上限 128 个文件／512 MiB，单图最多 128 MiB；损坏或任何缓存失败回退原加载。`MMS_TEXTURE_CACHE_DISABLE=1` 旁路，`MMS_TEXTURE_CACHE_DIR` 供隔离测试覆盖根目录。
- `Batch` 是模型局部准备范围：文件与磁盘缓存只由构造线程读取，最多两个 CPU worker 解码自有字节；创建纹理与 MMHack 登记仍在原加载线程的 `Create` 中完成；已有 GPU 池条目由调用方跳过，重复路径合并；不支持格式、失败或预算溢出保持原生。所有退出路径 join worker，像素不越出范围。
- 统计：命中／原生调用／存储计数与并行暂存／解码／使用／磁盘就绪／失败计数。

## ABI 实现

- 采用 [texture_prepare_cache.hpp](../../include/mikumikustudio/texture_prepare_cache.hpp)、[texture_prepare_cache.cpp](../../src/render/texture_prepare_cache.cpp)、[texture_cpu_decode.cpp](../../src/render/texture_cpu_decode.cpp)（WIC 解码）与 [parallel_prepare_policy.hpp](../../include/mikumikustudio/parallel_prepare_policy.hpp)。
- ABI 适配：缓存根改为 `cache_manager::Root()/Textures`，即程序目录 `data/cache/Textures`，与业务 Store 的 `Textures/<名称>/<64 hex>.mtc` 布局一致；开关与容量/回收走缓存管理器（`Enabled`、`WriteReservation/Touch/Invalidate` 由消费者使用，管理器禁用时原始兼容配置整体旁路）。
- 普通模型纹理加载：[path_resolve.cpp](../../src/model/path_resolve.cpp) 的 `LoadTextureShared` 把两次 `fromFileExW` 调用改为 `load(usage)` lambda，完整配置走 `texture_prepare_cache::Create`，原始兼容配置仍调用原生入口。
- PMX 批量准备：[pmx_load.cpp](../../src/model/pmx_load.cpp) 在材质加载前收集最多 64 个尚无 GPU 池条目的纹理路径（使用与材质加载完全相同的拼接路径），构造 `texture_prepare_cache::Batch`；只在并行准备策略未禁用且非原始兼容配置时启用。ABI 已有的顶点并行/几何缓存路径不变。
- CMake：纹理准备与 CPU 解码源加入核心库；完整配置新增 `texture_prepare_cache_tests`（直接编译三个消费者源，链接缓存管理器、runtime log、d3d9、bcrypt、ole32、windowscodecs）。

## 验证

本机 Win10 x64，真实 `d3dx9_43.dll` 与 HAL 设备；证据目录 `build/verification/abi-texture-prepare-cache-20261024/`。

| 检查 | 结果 |
| --- | --- |
| `texture_prepare_cache_tests`（合成 TGA/DDS） | 通过：usage 0 与 AUTOGENMIPMAP 的像素、元数据、独立所有权与 mip 渲染完全一致；手写 DDS mip 链保持原生行为；并行 worker 输出与原入口一致、源稳定、去重、热复用与仅内存模式；损坏/mtime 保持的编辑/不可写目录/禁用/缺失源均回退；`hits=25 native=17 stores=42` |
| 同上 + 真实 BMP 纹理 | 通过：额外解码 `color.bmp`、`shade.bmp`，`hits=37 native=39 stores=48` |
| 模型加载回归（`autosave_tests` 完整配置） | 通过，1,143 项：真实隐藏设备加载模型与纹理，未因缓存路径退化 |
| 原始兼容保存回归 `--numbered-only` | 通过，39 项（该配置原生加载、不使用缓存） |
| 完整产品 | 构建通过，EXE SHA-256 `fd6cec650e81c5e1933bd8afb0d5badf35328f2338bd0392fcbb3f2cb9d8b610` |
| 原始兼容产品 | 构建通过，EXE SHA-256 `25893208686b92a0f8436d3b380457535a4cf279645fd7a4bc0abf4a26d6c094` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 未完成与边界

- 没有加载真实 MMD 模型（多材质、非 ASCII 路径、成人/复杂纹理集）或 MME 场景；没有真实工程下的命中率、内存/显存与加载耗时对比。
- 事务导入路径仍使用已有的 `import_texture_cache` 去重，没有与本缓存共用；需求若要求两条路径共用，仍需后续接入。
- 纹理种类只覆盖测试夹具（TGA、DDS、BMP）与 CPU 解码支持范围；未验证 DDS 压缩格式、超大纹理与 128 MiB 边界在真实工程中的表现。
- 原始兼容配置不使用缓存；未做人工视觉、Win7/AMD 真机、多用户、慢盘、只读目录与长时间运行验收。

## 后续

下一批补真实 MMD 模型加载与事务导入共用的观测，然后回到 P1–P6 剩余接口与外部验收。
