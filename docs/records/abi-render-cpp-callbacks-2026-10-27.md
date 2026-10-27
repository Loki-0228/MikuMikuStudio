# 渲染回调的 C++ 封装

`RenderServices` 可显式查询 `mms.render` 1.1 和 1.2，并注册或清除帧回调及阶段回调。未提供扩展的旧宿主仍可使用 1.0 接口。

`RenderLease` 在释放失败时保留句柄。向已有句柄的对象申请租约返回 `MMS_E_BUSY`；移动赋值遇到释放错误时保留源和目标句柄。设备丢失后，宿主已删除的租约可通过 `Detach()` 清除本地句柄。

`render_cpp_extension_contract` 覆盖扩展查询、回调参数、旧宿主、残缺表及租约错误路径，共 19 项。`plugin_abi_render_callbacks` 验证宿主侧帧回调和租约生命周期。两个 CTest 在完整配置通过；本批未做真实设备丢失、Win7 或 AMD 真机验收。
