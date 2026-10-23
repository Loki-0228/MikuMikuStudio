# 布局撤销历史

日期：2026-10-24。工作区 `D:/MMD-Next/MMS-abi-dev`，分支 `codex/plugin-abi`。本批实现 P2「布局撤销历史」首版：`mms.ui.layout` 1.1 的撤销/重做。

## 接口

- [layout.h](../../sdk/mms/include/mms/layout.h)：新增 `MmsUiLayoutHistoryInfoV1`（canUndo/canRedo、深度、偏好修订号）与 `MmsUiLayoutApiV1_1`；1.1 前缀保留 1.0 的五个函数，并追加 `queryHistory`、`undo`、`redo`。旧 1.0 布局服务不变。
- [ui_registry.cpp](../../src/plugins/ui_registry.cpp)：成功的布局事务提交若改变保存的布局偏好，记录提交前的偏好 JSON；撤销栈上限 32，新提交清空重做栈。撤销/重做使用 `ImportPreferences` 恢复完整偏好快照，并在回调内或空栈时分别返回 BUSY/NOT_READY。
- [manager.cpp](../../src/plugins/manager.cpp)：`mms.ui.layout` 的 minor=1 查询返回 1.1 表，minor=0 继续返回 1.0 表。
- lifecycle 探针在创建阶段查询 1.1 表并记录 `LAYOUT_HISTORY_PASS`；缺失时记录 `LAYOUT_HISTORY_MISSING`，宿主探针断言该标记不出现。
- [layout_checks.inc](../../tests/plugins/layout_checks.inc)：在真实布局事务后验证 `canUndo`、`canRedo`、深度、撤销后偏好变化、重做后偏好恢复，以及空重做栈返回 NOT_READY。

## 验证

证据目录 `build/verification/abi-layout-history-20261024/`。

| 检查 | 结果 |
| --- | --- |
| 默认全量契约套件 | 通过，`PASS checks=44784`；布局检查包含撤销/重做断言 |
| 宿主事件探针 | 通过 7 项；插件日志无 `LAYOUT_HISTORY_MISSING`，`LAYOUT_HISTORY_PASS` 出现 |
| 相关 CTest | 10 个模式全部通过，9.6 秒 |
| 完整产品 | 构建通过，SHA-256 `903bf97a8c56b380d7f80ca26e1ce94db903e89b779c23f0117e9e0c8e8a7eec` |
| 原始兼容产品 | 构建通过，SHA-256 `b20a88225d20936c3b9e4a73fe3d4fcbe43eaadec380882bffd8bb6198786a5e` |
| 正式 release | 未覆盖，仍为 `180db2580c99a3578f30b2539451ff82605aebdec337daebce8b77edf86cf65a` |

## 边界

- 历史只在进程内、按全局布局偏好记录，不随工程或设置文件持久化；重启后为空。
- 只记录改变保存偏好的成功事务；未改变的提交不进入历史。撤销/重做影响所有插件的共享布局偏好，不区分发起者。
- 没有面向用户的热键或界面入口，也没有 C++17 包装；撤销后几何要等下一次布局刷新，历史服务不立即移动 HWND。
- 布局偏好快照不包含注册表拓扑、刷新状态与面板内容；DOCK、时间线/状态栏挂载仍未实现。
