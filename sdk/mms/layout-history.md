# mms.ui.layout 1.1 布局撤销历史

`mms.ui.layout` 1.1 在 1.0 布局服务之上追加进程内布局撤销/重做。只有成功提交且改变保存布局偏好的事务才进入历史；撤销/重做恢复完整偏好快照，作用域为整个共享布局，不区分插件。

## 查询能力

```c
MmsUiLayoutApiV1_1 layout;
if (host->queryService(host->context, (MmsString){"mms.ui.layout", 14}, 1, 1,
        sizeof(layout), &layout) == MMS_OK) {
    MmsUiLayoutHistoryInfoV1 info = {0};
    info.h = (MmsHeader){sizeof(info), 0, 1};
    layout.queryHistory(layout.context, &info);
    // info.canUndo, info.canRedo, info.undoDepth, info.redoDepth
}
```

使用 major=1、minor=0 查询仍返回 1.0 表，旧代码不受影响。

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
    history.Info(info);
    if (info.canUndo) history.Undo();
}
```

## 当前边界

历史不持久化；没有用户热键或界面入口；没有 C++17 包装；不包含注册表拓扑和刷新状态。DOCK 与时间线/状态栏挂载仍未实现。
