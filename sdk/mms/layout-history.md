# mms.ui.layout 1.1 布局撤销历史

`mms.ui.layout` 1.1 在 1.0 布局服务之上追加进程内布局撤销/重做。只有成功提交且改变保存布局偏好的事务才进入历史；撤销/重做恢复完整偏好快照，作用域为整个共享布局，不区分插件。

## 查询能力

```c
MmsUiLayoutApiV1_1 layout = {0};
const MmsString id = {"mms.ui.layout", sizeof("mms.ui.layout") - 1};
if (host->queryService(host->context, id, 1, 1, sizeof(layout), &layout) == MMS_OK &&
        layout.h.size >= sizeof(layout) && layout.h.major == 1 && layout.queryHistory) {
    MmsUiLayoutHistoryInfoV1 info = {0};
    info.h = (MmsHeader){sizeof(info), 1, 0};
    if (layout.queryHistory(layout.context, &info) == MMS_OK) {
        // info.canUndo, info.canRedo, info.undoDepth, info.redoDepth
    }
}
```

服务 ID 的字节数为 13，不含结尾 NUL。查询版本为 1.1，`MmsUiLayoutHistoryInfoV1` 结果记录版本为 1.0；两者不同。使用 major=1、minor=0 查询仍返回 1.0 服务表。当前宿主的 1.1 查询分派会返回完整 1.1 表，但其报头 `minor` 仍填 0；这是尚未修正的宿主元数据。当前兼容路径用成功的 1.1 查询、表大小、major 和所用回调判断，不因 `minor=0` 将该结果当作 1.0 表。

## 撤销与重做

```c
layout.undo(layout.context);
layout.redo(layout.context);
```

空栈返回 `MMS_E_NOT_READY`，插件回调内调用返回 `MMS_E_BUSY`。新布局提交清空重做栈；历史上限 32。撤销/重做只恢复保存偏好，实际窗口几何在宿主下一次布局刷新时更新。

## C++17 封装

```cpp
#include <mms/cpp/layout_history.hpp>
mms::sdk::LayoutHistoryServices history;
if (mms::sdk::LayoutHistoryServices::Query(host, history) == MMS_OK) {
    MmsUiLayoutHistoryInfoV1 info{};
    if (history.Info(info) == MMS_OK && info.canUndo) history.Undo();
}
```

## 当前边界

历史不持久化；没有用户热键或界面入口；[C++17 封装](include/mms/cpp/layout_history.hpp)转发同一服务表，不另存历史。历史不包含注册表拓扑和刷新状态。DOCK 与时间线/状态栏挂载仍未开放。
