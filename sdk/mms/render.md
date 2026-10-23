# mms.render 1.0 参考

`mms.render` 提供渲染设备代次、丢失状态与设备资源租约。宿主在完整配置的生产插件运行时中提供该服务；原始兼容配置不初始化生产插件运行时。

## 查询

成功呈现后 `frameIndex` 递增，`width`／`height` 与 `backBufferFormat` 来自真实 0 号后缓冲。初始查询在这些字段为 0。

```c
MmsRenderApiV1 render;
host->queryService(host->context, (MmsString){"mms.render", 10}, 1, 0, sizeof(render), &render);

MmsRenderInfoV1 info = {0};
info.h = (MmsHeader){sizeof(info), 1, 0};
if (render.query(render.context, &info) == MMS_OK) {
    // info.deviceGeneration, info.lost
}
```

`lost` 为 1 表示设备处于丢失状态；恢复后设备代次递增。帧序号、后缓冲尺寸与格式当前保留为 0。

## 设备租约

```c
static void MMS_CALL ReleaseDevice(void* context) {
    // 只释放设备相关对象；回调在 UI 线程、插件回调之外执行。
}
MmsRenderLeaseDescV1 desc = {0};
desc.h = (MmsHeader){sizeof(desc), 1, 0};
desc.name = (MmsString){"example.device", 14};
desc.release = ReleaseDevice;
desc.flags = MMS_RENDER_LEASE_DEVICE;
MmsHandle lease = 0;
render.acquireLease(render.context, &desc, &lease);
```

设备丢失时宿主调用释放回调并清空租约；设备恢复后插件应重新获取。插件停止时租约被静默丢弃，释放回调不会在停止流程中调用。显式 `releaseLease` 只移除租约，不调用释放回调（插件已经释放了自己的资源）。

## 错误

- 停止期查询返回 `MMS_E_NOT_READY`；
- 空描述、缺少释放回调、未知标志或超长名称返回 `MMS_E_ARGUMENT`；
- 释放不属于当前插件的租约返回 `MMS_E_NOT_FOUND`。

## C++17 封装

```cpp
#include <mms/cpp/render.hpp>
mms::sdk::RenderServices services;
if (mms::sdk::RenderServices::Query(host, services) == MMS_OK) {
    MmsRenderInfoV1 info{};
    services.Info(info);
    mms::sdk::RenderLease lease;
    services.Acquire("example.device", ReleaseDevice, context, lease);
    // 停止前显式释放；析构不回调宿主。
    lease.Release();
}
```

## 当前边界

帧生命周期、DX9 帧钩子、post 处理、后缓冲信息、租约分类与多视图渲染尚未提供；C++17 包装未提供。真实设备丢失与 Win7/AMD 真机验收仍待外部环境。
