# CPU 图像租约

通过 `queryService("mms.render.image", 1, 0, sizeof(MmsRenderImageApiV1), &api)` 获取 [C 表](include/mms/render_image.h)。在 UI 线程查询一次；表内七个入口均可在后台线程调用。

完整配置的生产插件运行时提供此服务；原始兼容配置不启动生产插件运行时，其专项测试用于核对构建和接口行为。清单需要非空 `capabilities`，图像插件可声明 `render.image`，并将本服务列入 `requiresServices`。

此服务保存插件提供的不可变 CPU 像素，不执行场景求值、渲染、窗口呈现或工程编辑。它是后续视口输出与合成的基础，目前不能直接把图像挂到 MMS 视口。现有 `mms.render` 的设备资源登记和回调语义保持不变。

## 创建与读取

`createCpu` 同步复制有效像素，返回本插件的图像句柄。输入缓冲区在调用返回前必须可读且不可被其他线程修改，返回后可以立即释放或改写。

| 字段 | 1.0 支持范围 |
| --- | --- |
| `format` | `MMS_IMAGE_RGBA8` 或 `MMS_IMAGE_BGRA8`，每像素四个字节 |
| `colorSpace` | `MMS_IMAGE_COLOR_SRGB` |
| `alphaMode` | OPAQUE、STRAIGHT、PREMULTIPLIED；只是描述，宿主不转换像素 |
| 行方向 | 自上而下；`rowStride` 为非负字节跨度，至少 `width * 4` |
| `flags` | 必须为零 |
| 输入字节范围 | 至少 `(height - 1) * rowStride + width * 4`；不读取行尾填充 |

输出采用紧密行，`rowStride = width * 4`、`byteCount = rowStride * height`。`deviceGeneration` 为零，设备丢失或恢复不使 CPU 图像失效。输出颜色、通道和 alpha 字节保持原样；OPAQUE 不会自动将 alpha 改为 255，PREMULTIPLIED 也不会自动预乘。

`describe` 读取描述。`retain` 返回新的独立图像句柄，与原句柄共享像素；对新旧句柄分别调用 `release`。`mapRead` 返回只读指针和独立 `mapping` 句柄，即使全部图像句柄已经释放，映射仍然有效。最后一个使用者结束读取后调用 `unmap`，此后不得再访问指针。每次 `mapRead` 都须对应一次 `unmap`。

```c
MmsRenderImageDescV1 desc = {0};
desc.h = (MmsHeader){sizeof(desc), 1, 0};
desc.width = 1; desc.height = 1;
desc.format = MMS_IMAGE_RGBA8;
desc.colorSpace = MMS_IMAGE_COLOR_SRGB;
desc.alphaMode = MMS_IMAGE_ALPHA_STRAIGHT;
desc.rowStride = 4;
const unsigned char pixel[4] = {255, 80, 20, 128};
MmsHandle image = 0;
if (api.createCpu(api.context, &desc, (MmsBytes){pixel, sizeof(pixel)}, &image) == MMS_OK) {
    MmsRenderImageMapV1 mapped = {0};
    if (api.mapRead(api.context, image, &mapped) == MMS_OK) {
        /* 在这里读取 mapped.pixels；不允许写入。 */
        api.unmap(api.context, mapped.mapping);
    }
    api.release(api.context, image);
}
```

这是 C11 示例；C++ 使用相同函数表和句柄即可，不存在第二套图像存储。

## 内存与句柄上限

先调用 `getCapabilities` 取得实际限制，插件应保留更小的自身预算。1.0 默认限制：

| 范围 | 上限 |
| --- | --- |
| 单边尺寸 | 16,384 像素；同时满足字节限制 |
| 单张紧密像素数据 | 64 MiB |
| 单插件存活像素数据 | 128 MiB |
| 一个宿主图像服务内全部像素数据 | 256 MiB |
| 单插件图像句柄，包括 retain | 4,096 |
| 单插件读取映射 | 1,024 |

相同图像的 retain 和 mapping 不重复计算像素字节。只有最后一个图像句柄或映射释放后才归还字节额度，不能先释放图像、继续保留映射来绕过限制。这些额度不包含插件输入缓冲区、容器元数据或进程其他内存，因此不是整个 MMS 的内存上限。

复制与分配在调用期间完成，并由同一锁保护。服务不提供无锁或异步上传承诺；较大的图像宜在插件工作线程准备。调用返回前输入缓冲区的所有权不转交宿主。

## 错误与停止

失败不修改输出参数，也不发布部分图像。输出结构不要求预填 `h`，成功时由宿主填写；输入描述必须具有有效版本头。超过版本大小的输入尾部不读取。

| 结果 | 含义 |
| --- | --- |
| `ARGUMENT` | 空输出、无效头或 flags、零尺寸、行跨度或输入长度不足、地址及跨度运算溢出 |
| `UNSUPPORTED` | 不支持的像素格式、颜色空间或 alpha 模式 |
| `MEMORY` | 尺寸、字节或句柄额度不足，或者分配失败 |
| `NOT_FOUND` | 已释放、未知、其他插件的句柄，或图像和映射类型混用 |
| `NOT_READY` | 插件已进入停止阶段，不能再查询能力、创建、描述、retain 或 map |

多个条件同时无效时只保证返回失败，不保证错误码优先级。服务检查长度和地址运算，不替调用方验证任意内存地址是否可读。同进程插件仍需遵循指针契约。

停止时宿主回收该插件所有图像句柄，保留尚未释放的映射。缓存表的 `unmap` 仍可调用；已回收的图像再 `release` 返回 `NOT_FOUND`。未完成映射会阻止 DLL 和宿主上下文卸载，包含创建失败且未返回实例的情况。因依赖关系暂缓调用 `stop` 的插件也立即进入此状态。

插件的 `stop` 仍必须停止并等待全部后台工作，释放映射后才返回成功；宿主保活不能代替线程退出。不得在另一个线程尚读取时 `unmap`，也不得在插件最终卸载后调用缓存表。

句柄只在当前服务、当前插件实例内使用，不持久化、不跨插件传递。独立求值、多视口挂载、渲染器提供者、GPU 互操作和跨插件图像授权仍未开放。设计见[视口方案](../../docs/PLUGIN_VIEWPORT_RENDERING.md)。本实现不要求新系统图形接口、特定显卡或 GPU。
