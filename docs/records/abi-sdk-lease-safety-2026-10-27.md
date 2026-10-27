# C++ SDK 租约安全

审查发现 `RenderLease` 的移动赋值在目标释放失败时保留源句柄，但临时源会在表达式结束时销毁，使句柄不可再访问。`SceneSnapshot` 和 `LayoutSnapshotLease` 也存在无法报告转移失败的移动赋值；重复获取还会静默释放旧快照，使借用的数据失效。

这三个类型删除移动赋值，新增只接受具名源对象并返回结果的 `TransferFrom`。目标释放失败时两个句柄保持不变；已持有句柄的对象再次获取返回 `MMS_E_BUSY`。移动构造和显式释放仍可用。原先使用移动赋值的插件需要修改源码。

完整配置的 `snapshot_cpp_safety_contract`、`render_cpp_extension_contract` 和 `plugin_abi_render_callbacks` 均通过。快照专项覆盖 26 项检查，渲染专项覆盖转移失败与编译期拒绝临时源。完整 `plugin_abi_tests` 已重新编译；当前 USER32 启动失败使该套件尚未运行。真机、设备丢失和插件兼容性验收仍待完成。
