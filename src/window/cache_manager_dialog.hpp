#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
namespace mikumikustudio {
class MMDApp;
namespace cache_manager_ui {
inline constexpr unsigned kCommand=32815;
inline constexpr int kDialog=9200;
enum Control { Enabled=9201,Automatic,Limit,Recent,Summary,Kinds,Projects,Path,
    Pin,ClearProject,ClearReclaimable,ClearAll,Explanation,Status,Apply,LimitLabel,RecentLabel };
void RefreshMenu(MMDApp*);
bool HandleCommand(MMDApp*,unsigned);
INT_PTR CALLBACK DialogProc(HWND,UINT,WPARAM,LPARAM);
}
}
