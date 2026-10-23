# 渲染服务：设备代次与资源租约

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批实现 P5 的 `mms.render` 1.0 首版：设备代次查询、丢失状态与设备资源租约。

## 接口

- [render.h](../../sdk/mms/include/mms/render.h)：`MmsRenderApiV1` 提供 `query`、`acquireLease`、`releaseLease`。`MmsRenderInfoV1` 带设备代次、丢失标志；`MmsRenderLeaseDescV1` 带名称、释放回调和上下文。
- [render_service.hpp](../../src/plugins/render_service.hpp)、[render_service.cpp](../../src/plugins/render_service.cpp)：宿主服务持有租约表与设备代次。设备丢失时先进入丢失状态并调用所有租约的释放回调，再清空租约；设备恢复时更新代次、清除丢失状态。插件停止时静默丢弃该插件租约，不调用已停止插件的回调。
- [manager.cpp](../../src/plugins/manager.cpp)：每个插件实例带独立客户端身份，查询 `mms.render` 返回同一服务表；停止期返回 NOT_READY。
- [integration.cpp](../../src/plugins/integration.cpp)：设备丢失与恢复通知同时更新渲染服务，保证事件负载代次与查询代次一致。
- lifecycle 探针在启动时查询渲染信息、获取设备租约；设备丢失事件中查询丢失状态，恢复事件中查询新代次并重新获取租约。释放回调记录 `RENDER_LEASE_RELEASED`。
- 宿主事件探针分两段等待丢失与恢复，避免连续触发导致查询看到恢复后的状态；断言 `RENDER_INFO`、`RENDER_LOST_QUERY`、`RENDER_RESTORED_QUERY`、`RENDER_LEASE_ACQUIRED`、`RENDER_LEASE_RELEASED` 均出现且无失败标记。

## 验证

证据目录 `build/verification/abi-render-service-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 宿主事件探针 | 通过 7 项；插件日志顺序为 `RENDER_INFO gen=1 lost=0`、`RENDER_LEASE_ACQUIRED`、`RENDER_LEASE_RELEASED`、`EVENT_DEVICE_LOST`、`RENDER_LOST_QUERY gen=1 lost=1`、`EVENT_DEVICE_RESTORED`、`RENDER_RESTORED_QUERY gen=2 lost=0`、重新获取租约 |
| 默认全量契约套件 | 通过，`PASS checks=44764` |
| 相关 CTest | 10 个模式全部通过，9.3 秒 |
| 完整产品 | 构建通过，SHA-256 `a8fde4b297082fafe77960bd63347485bf55e1d4264b71fbb4f15d1fc0fdc06c` |
| 原始兼容产品 | 构建通过，SHA-256 `4fd72f93a836078324dba4fddb8287368ba386d660594d29841faa40936cb75d` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- `query` 只填充设备代次与丢失状态；帧序号、后缓冲尺寸/格式仍为 0，DX9 帧生命周期与 post 处理服务未接入。
- 租约只有设备类（`MMS_RENDER_LEASE_DEVICE`）：宿主丢失/恢复与停止时释放；显式 `releaseLease` 不调用释放回调。
- 探针通过宿主入口触发丢失/恢复，真实 D3D9 设备丢失、焦点切换、拖动到不同显示器与 Win7/AMD 真机未验证；插件释放回调被要求只释放设备对象，未验证复杂回调。
- 没有 C++17 SDK 包装；多视图、洋葱皮与绘制层仍按视口方案未实现。

## 后续

补充帧信息与 post 服务、租约分类、真实设备丢失验证与 C++ 封装；继续 P2/P3/P4/P6 剩余接口。
