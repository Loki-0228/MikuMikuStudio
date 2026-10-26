# mms.render 1.0–1.2 参考

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

`lost` 为 1 表示设备处于丢失状态；恢复后设备代次递增。尺寸和格式更新要求宿主成功读取后缓冲信息。

## 设备租约

```c
static void MMS_CALL ReleaseDevice(void* context) {
    // 只释放设备相关对象；回调在 UI 线程的安全边界执行。
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

释放回调开始前，该租约已从登记表移除，因此在回调内取消自身返回 `MMS_E_NOT_FOUND`。回调可以取消同一插件尚未执行的其他租约；成功取消后宿主不会再调用该租约的释放函数。取消不属于当前插件的租约仍被拒绝。回调抛出异常不会阻止其余租约清理。

设备丢失或释放进行中不能登记新租约。释放回调中的重复丢失、恢复通知不会递归派发或提前恢复设备；本轮释放完成后仍需正常的恢复通知。插件应在设备恢复事件后重新申请租约。

普通插件回调内检测到设备丢失时，宿主立即标记设备不可用，但延迟释放到回调返回后的安全边界。事件投递、帧入口及 Reset 准备入口检查待释放状态。设备通知状态与释放义务分别维护。

DX9 Reset 在任何本体资源清理前先完成插件租约释放。回调内请求 Reset 会返回失败并保留待恢复状态，后续帧重试；不会在回调内释放资源或调用设备 Reset。Reset 失败不恢复设备代次；后续成功恢复只递增一次。短暂丢失后，若设备直接恢复为 D3D_OK，也会通知恢复并重新允许申请租约，无需额外 Reset。

## 设备通知

设备 LOST 和 RESTORED 通知保存为最新状态及每插件投递游标，不分配临时事件节点。消息投递失败时，定时器和帧安全边界重试。消息和定时器均失败时，状态保留至后续安全帧入口，不同步进入插件回调。

多次连续丢失和恢复可合并。宿主先向所有有效插件投递最新 LOST，再投递当前 RESTORED。LOST 的代次表示失效的资源代次；投递时设备可能已恢复，不能据此假定查询结果的 `lost` 必为 1。RESTORED 在每次回调开始前必须仍对应当前可用代次。一个插件的回调再次改变设备状态时，宿主中断旧批次，其他插件不会继续收到过期的恢复通知。

同一插件已经尝试处理的通知不会因宿主重试而重复调用。插件回调抛出异常也按已尝试处理，宿主继续通知其他插件；插件负责处理自身资源重建失败，申请租约时仍须检查返回值。停止的插件不再接收通知，新建运行时不继承旧投递状态。

设备恢复通知可能在帧入口补发。如果该回调产生新的待 Reset 请求，宿主在本帧绘制和后续场景处理前退出，等待恢复完成。

## 错误

- 停止期通过 `queryService` 获取服务返回 `MMS_E_NOT_READY`；
- 已退休插件登记回调或申请租约返回 `MMS_E_NOT_READY`；
- 设备丢失或释放进行中申请租约返回 `MMS_E_NOT_READY`，输出句柄不变；
- 空描述、缺少释放回调、未知标志或超长名称返回 `MMS_E_ARGUMENT`；
- 释放不属于当前插件的租约返回 `MMS_E_NOT_FOUND`。

## 每帧回调（1.1）

```c
MmsRenderApiV1_1 render;
host->queryService(host->context, (MmsString){"mms.render", 10}, 1, 1, sizeof(render), &render);
render.setFrameCallback(render.context, OnFrame, user);
// OnFrame(void* user, const MmsRenderInfoV1* info) 在成功 Present 后同步运行。
render.clearFrameCallback(render.context);
```

回调在 UI 线程、插件回调之外运行，必须轻量、只读，不得调用宿主服务或阻塞。1.0 查询仍返回原三函数表。

## 帧阶段回调（1.2）

1.2 在 1.1 上追加 `setPhaseCallback`／`clearPhaseCallback`。DX9 FrameDriver 通过设备恢复与场景修改检查后调用 BEGIN，在成功 Present 后调用 END。局部重绘共用该通知入口。BEGIN 表示一次绘制尝试，不保证有对应 END；没有呈现或呈现失败时不发送 END。回调收到当前帧信息与阶段值，运行在 UI 线程，必须轻量只读。其他插件回调内再次进入帧通知入口会跳过该次通知。1.0/1.1 查询仍返回原表。

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

已提供 DX9 FrameDriver 的帧通知、后缓冲信息、Reset 前安全释放与 C++17 包装。post 处理、租约分类、完整多视图渲染、DX12 原生与 BakePreview 独立呈现路径尚未统一。当前验证使用真实 DX9 设备、实际资源与注入的丢失或失败结果；驱动真实掉线、Win7、AMD 真机及长时间运行仍待验收。
