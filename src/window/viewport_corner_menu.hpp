#pragma once
// ===========================================================================
// 帮助菜单里的操作柄停靠位置轮换项。点击后按左上、右上、左下、右下循环，
// 菜单标签显示当前值，交互方式与模型名称语言菜单项一致。
// ===========================================================================
#include <Windows.h>

namespace mikumikustudio {
class MMDApp;

namespace viewport_corner_menu {

// 命令号取全树核对过的空档（占用中：32000..32002 / 32004 / 32100..32227 /
// 32600 / 32800..32804 / 32900..32906 / 33000..33128 / 33400..33529）。
constexpr unsigned kViewportCornerMenuCommand = 32810;

// 幂等补挂并刷新菜单项。语言切换、每次 WM_INITMENUPOPUP 都可以重复调用。
void EnsureItem(MMDApp* app);

bool HandleCommand(MMDApp* app, unsigned command);  // 点击：循环到下一角

}  // namespace viewport_corner_menu
}  // namespace mikumikustudio
