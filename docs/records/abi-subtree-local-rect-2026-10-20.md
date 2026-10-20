# 子树局部重绘实现记录

日期：2026-10-20。分支：`codex/plugin-abi`。本批实现已通过完整配置的四组刷新回归。原子树规则见[注册子树刷新实现与验证](abi-subtree-refresh-2026-10-20.md)。接口用法见[刷新服务](../../sdk/mms/refresh.md)与 [C++ SDK](../../sdk/mms/cpp-refresh.md)。

## 请求与坐标

`mms.ui.refresh` 1.0 的 SUBTREE 请求接受 `dirtyRects`。沿用原 C 服务表、请求结构、注册句柄、票据和队列。非空矩形必须包含 REPAINT，并提供根的非零 `expectedGeometryRevision`。单次上限仍为 32 个矩形。

矩形以根客户区左上角为原点，单位为 DIP。GROUP 使用保留布局中的逻辑范围。宿主先把矩形裁剪到根范围，再按根 DPI 转到共同坐标空间，平移到每个已注册后代的客户区，最后裁剪到该客户区并向外取整。多个矩形形成区域并集。空矩形或无交集矩形不产生损伤，不转换成整控件重绘。

NativeUi 发布实际 HWND 客户区原点和所属宿主坐标空间。原点保留整数像素，避免在 175% 缩放下经过 float DIP 往返产生边界误差。GROUP 仍不创建 HWND，其原点来自已取整的保留布局。不同目标可以有不同 DPI，但必须属于同一个已发布坐标空间；不一致时整次返回 NOT_READY。

REPAINT 仅使注册树中的实际按钮、分隔线和面板区域失效。GROUP 借用的工具栏窗口不整块失效。插件自行创建的原生子窗口不作为注册后代枚举。

## 接受、执行和失效

后台 request 只复制请求并读取已发布的呈现与拓扑快照，不查询 HWND。入队固定成员、实例、根及各成员几何、状态提供者和 DATA 版本集合。根或成员在执行前移动、缩放、更换 DPI、重新挂载、重建或卸载时，沿用既有 STALE、NOT_READY 或 CANCELLED 规则。旧区域不会自动跟随新位置。

矩形只限制 REPAINT。组合请求仍对整树依次完成 DATA、STATE、一次 LAYOUT 和 REPAINT。STATE 只改变显隐而未改变区域坐标时，可继续使用原区域。成功 LAYOUT 改变根或后代的原点、尺寸、DPI 或坐标空间时，REPAINT 前统一返回 STALE，本次 REPAINT 尚未使任何成员区域失效。票据保留已成功阶段的 `completedKinds`。

显示更新仍遵循原服务的失败规则。单个 DATA 准备失败保留该绑定的完整旧显示，失败后停止后续阶段。宿主不回滚此前已经完成的其他绑定或原生 STATE 操作。

相同合并条件的局部贡献取并集。整目标贡献覆盖同组局部区域。取消尚未执行的票据只撤回该贡献。成员、来源版本集合、几何约束策略和拓扑不同的请求不合并。

## SDK 与验收

C++ SDK 新增 `RefreshSubtreeRects` 和 `RepaintSubtreeRects`。两者构造同一 C 请求；前者允许组合 DATA、STATE 和 LAYOUT。调用者传入 UI 线程取得的完整 `MmsUiRefreshTargetV1` 后，可以在工作线程提交。旧整树便利方法保持原行为。

独立回归文件为 `tests/plugins/ui_refresh_subtree_rect_checks.inc`，入口为 `UiRefreshSubtreeRectChecks()`。该文件依赖原刷新、布局和子树夹具，聚合顺序应位于 `ui_refresh_subtree_checks.inc` 之后。新增断言检查真实 `GetUpdateRgn`，覆盖根裁剪、嵌套偏移、无交集、区域并集、取消、配额、175% 根与不同目标 DPI、后台数组复制、布局和生命周期失效、完整阶段顺序，以及 NativeUi 实际 HWND 与 C++ SDK 接入。

完整配置已构建，`ui_layout_refresh_runtime`、`ui_data_refresh_runtime`、`plugin_abi_contract` 和 `ui_subtree_refresh_runtime` 均通过，共 4 组。日志见 [test-refresh.log](../../build/verification/abi-history-region-lifetime-20261020/test-refresh.log)。上述结果包含新子树区域检查，不把旧批次结果计作本批验收。

`mmd932` 原始兼容配置完成构建，五组刷新、插件合同和可选图形导入全部通过，日志为同目录 `test-original.log`。本批没有 Win7、AMD 或其他目标设备的真机结果。实现继续使用 Win7 已有 GDI 路径，不增加显卡厂商、GPU 或新系统接口依赖。
